/*
 * Hydrogen
 * Copyright(c) 2002-2008 by Alex >Comix< Cominu [comix@users.sourceforge.net]
 * Copyright(c) 2008-2021 The hydrogen development team [hydrogen-devel@lists.sourceforge.net]
 *
 * http://www.hydrogen-music.org
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY, without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see https://www.gnu.org/licenses
 *
 */

#include <core/AudioEngine/AudioEngine.h>
#include <core/AudioEngine/TransportPosition.h>

#ifdef WIN32
#    include "core/Timehelper.h"
#else
#    include <unistd.h>
#    include <sys/time.h>
#endif

#include <core/EventQueue.h>
#include <core/FX/Effects.h>
#include <core/Basics/Song.h>
#include <core/Basics/Pattern.h>
#include <core/Basics/PatternList.h>
#include <core/Basics/Note.h>
#include <core/Basics/DrumkitComponent.h>
#include <core/Basics/AutomationPath.h>
#include <core/Basics/InstrumentList.h>
#include <core/Basics/InstrumentLayer.h>
#include <core/Basics/InstrumentComponent.h>
#include <core/Sampler/Sampler.h>
#include <core/Helpers/Filesystem.h>

#include <core/IO/AudioOutput.h>
#include <core/IO/JackAudioDriver.h>
#include <core/IO/NullDriver.h>
#include <core/IO/MidiInput.h>
#include <core/IO/MidiOutput.h>
#include <core/IO/CoreMidiDriver.h>
#include <core/IO/OssDriver.h>
#include <core/IO/FakeDriver.h>
#include <core/IO/AlsaAudioDriver.h>
#include <core/IO/PortAudioDriver.h>
#include <core/IO/DiskWriterDriver.h>
#include <core/IO/AlsaMidiDriver.h>
#include <core/IO/JackMidiDriver.h>
#include <core/IO/PortMidiDriver.h>
#include <core/IO/CoreAudioDriver.h>
#include <core/IO/PulseAudioDriver.h>

#include <core/Hydrogen.h>
#include <core/Preferences/Preferences.h>
#include <cassert>
#include <limits>
#include <random>

namespace H2Core
{

const int AudioEngine::nMaxTimeHumanize = 2000;

inline int randomValue( int max )
{
	return rand() % max;
}

inline float getGaussian( float z )
{
	// gaussian distribution -- dimss
	float x1, x2, w;
	do {
		x1 = 2.0 * ( ( ( float ) rand() ) / static_cast<float>(RAND_MAX) ) - 1.0;
		x2 = 2.0 * ( ( ( float ) rand() ) / static_cast<float>(RAND_MAX) ) - 1.0;
		w = x1 * x1 + x2 * x2;
	} while ( w >= 1.0 );

	w = sqrtf( ( -2.0 * logf( w ) ) / w );
	return x1 * w * z + 0.0; // tunable
}


/** Gets the current time.
 * \return Current time obtained by gettimeofday()*/
inline timeval currentTime2()
{
	struct timeval now;
	gettimeofday( &now, nullptr );
	return now;
}

AudioEngine::AudioEngine()
		: m_pSampler( nullptr )
		, m_pSynth( nullptr )
		, m_pAudioDriver( nullptr )
		, m_pMidiDriver( nullptr )
		, m_pMidiDriverOut( nullptr )
		, m_state( State::Initialized )
		, m_pMetronomeInstrument( nullptr )
		, m_fSongSizeInTicks( 0 )
		, m_nRealtimeFrame( 0 )
		, m_fMasterPeak_L( 0.0f )
		, m_fMasterPeak_R( 0.0f )
		, m_nextState( State::Ready )
		, m_fProcessTime( 0.0f )
		, m_fLadspaTime( 0.0f )
		, m_fMaxProcessTime( 0.0f )
		, m_fNextBpm( 120 )
		, m_pLocker({nullptr, 0, nullptr})
		, m_currentTickTime( {0,0})
		, m_fLastTickEnd( 0 )
		, m_nLastLeadLagFactor( 0 )
		, m_bLookaheadApplied( false )
{
	m_pTransportPosition = std::make_shared<TransportPosition>( "Transport" );
	m_pQueuingPosition = std::make_shared<TransportPosition>( "Queuing" );
	
	m_pSampler = new Sampler;
	m_pSynth = new Synth;
	
	gettimeofday( &m_currentTickTime, nullptr );
	
	m_pEventQueue = EventQueue::get_instance();
	
	srand( time( nullptr ) );

	// Create metronome instrument
	// Get the path to the file of the metronome sound.
	QString sMetronomeFilename = Filesystem::click_file_path();
	m_pMetronomeInstrument = std::make_shared<Instrument>( METRONOME_INSTR_ID, "metronome" );
	
	auto pLayer = std::make_shared<InstrumentLayer>( Sample::load( sMetronomeFilename ) );
	auto pCompo = std::make_shared<InstrumentComponent>( 0 );
	pCompo->set_layer(pLayer, 0);
	m_pMetronomeInstrument->get_components()->push_back( pCompo );
	m_pMetronomeInstrument->set_is_metronome_instrument(true);
	
	m_AudioProcessCallback = &audioEngine_process;

#ifdef H2CORE_HAVE_LADSPA
	Effects::create_instance();
#endif
}

AudioEngine::~AudioEngine()
{
	stopAudioDrivers();
	if ( getState() != State::Initialized ) {
		ERRORLOG( "Error the audio engine is not in State::Initialized" );
		return;
	}
	m_pSampler->stopPlayingNotes();

	this->lock( RIGHT_HERE );
	INFOLOG( "*** Hydrogen audio engine shutdown ***" );

	clearNoteQueue();

	// change the current audio engine state
	setState( State::Uninitialized );

	m_pMetronomeInstrument = nullptr;

	this->unlock();
	
#ifdef H2CORE_HAVE_LADSPA
	delete Effects::get_instance();
#endif

//	delete Sequencer::get_instance();
	delete m_pSampler;
	delete m_pSynth;
}

Sampler* AudioEngine::getSampler() const
{
	assert(m_pSampler);
	return m_pSampler;
}

Synth* AudioEngine::getSynth() const
{
	assert(m_pSynth);
	return m_pSynth;
}

void AudioEngine::lock( const char* file, unsigned int line, const char* function )
{
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__,
					   QString( "by %1 : %2 : %3" ).arg( function ).arg( line ).arg( file ) );
	}
	#endif

	m_EngineMutex.lock();
	m_pLocker.file = file;
	m_pLocker.line = line;
	m_pLocker.function = function;
	m_LockingThread = std::this_thread::get_id();
}

bool AudioEngine::tryLock( const char* file, unsigned int line, const char* function )
{
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__,
					   QString( "by %1 : %2 : %3" ).arg( function ).arg( line ).arg( file ) );
	}
	#endif
	bool res = m_EngineMutex.try_lock();
	if ( !res ) {
		// Lock not obtained
		return false;
	}
	m_pLocker.file = file;
	m_pLocker.line = line;
	m_pLocker.function = function;
	m_LockingThread = std::this_thread::get_id();
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__, QString( "locked" ) );
	}
	#endif
	return true;
}

bool AudioEngine::tryLockFor( std::chrono::microseconds duration, const char* file, unsigned int line, const char* function )
{
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__,
					   QString( "by %1 : %2 : %3" ).arg( function ).arg( line ).arg( file ) );
	}
	#endif
	bool res = m_EngineMutex.try_lock_for( duration );
	if ( !res ) {
		// Lock not obtained
		WARNINGLOG( QString( "Lock timeout: lock timeout %1:%2:%3, lock held by %4:%5:%6" )
					.arg( file ).arg( function ).arg( line )
					.arg( m_pLocker.file ).arg( m_pLocker.function ).arg( m_pLocker.line ));
		return false;
	}
	m_pLocker.file = file;
	m_pLocker.line = line;
	m_pLocker.function = function;
	m_LockingThread = std::this_thread::get_id();
	
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__, QString( "locked" ) );
	}
	#endif
	return true;
}

void AudioEngine::unlock()
{
	// Leave "__locker" dirty.
	m_LockingThread = std::thread::id();
	m_EngineMutex.unlock();
	#ifdef H2CORE_HAVE_DEBUG
	if ( __logger->should_log( Logger::Locks ) ) {
		__logger->log( Logger::Locks, _class_name(), __FUNCTION__, QString( "" ) );
	}
	#endif
}

void AudioEngine::startPlayback()
{
	INFOLOG( "" );

	// check current state
	if ( getState() != State::Ready ) {
	   ERRORLOG( "Error the audio engine is not in State::Ready" );
		return;
	}

	// change the current audio engine state
	setState( State::Playing );

	// The locking of the pattern editor only takes effect if the
	// transport is rolling.
	handleSelectedPattern();
}

void AudioEngine::stopPlayback()
{
	INFOLOG( "" );

	// check current state
	if ( getState() != State::Playing ) {
		ERRORLOG( QString( "Error the audio engine is not in State::Playing but [%1]" )
				  .arg( static_cast<int>( getState() ) ) );
		return;
	}

	setState( State::Ready );
}

void AudioEngine::reset( bool bWithJackBroadcast ) {
	const auto pHydrogen = Hydrogen::get_instance();
	
	clearNoteQueue();
	
	m_fMasterPeak_L = 0.0f;
	m_fMasterPeak_R = 0.0f;

	m_fLastTickEnd = 0;
	m_nLastLeadLagFactor = 0;
	m_bLookaheadApplied = false;

	m_pTransportPosition->reset();
	m_pQueuingPosition->reset();

	updateBpmAndTickSize( m_pTransportPosition );
	updateBpmAndTickSize( m_pQueuingPosition );

	updatePlayingPatterns();
	
#ifdef H2CORE_HAVE_JACK
	if ( pHydrogen->hasJackTransport() && bWithJackBroadcast ) {
		// Tell all other JACK clients to relocate as well. This has
		// to be called after updateBpmAndTickSize().
		static_cast<JackAudioDriver*>( m_pAudioDriver )->locateTransport( 0 );
	}
#endif
}

float AudioEngine::computeTickSize( const int nSampleRate, const float fBpm, const int nResolution)
{
	float fTickSize = nSampleRate * 60.0 / fBpm / nResolution;
	
	return fTickSize;
}

double AudioEngine::computeDoubleTickSize( const int nSampleRate, const float fBpm, const int nResolution)
{
	double fTickSize = static_cast<double>(nSampleRate) * 60.0 /
		static_cast<double>(fBpm) /
		static_cast<double>(nResolution);
	
	return fTickSize;
}

float AudioEngine::getElapsedTime() const {
	
	const auto pHydrogen = Hydrogen::get_instance();
	const auto pDriver = pHydrogen->getAudioOutput();
	
	if ( pDriver == nullptr || pDriver->getSampleRate() == 0 ) {
		return 0;
	}

	return ( m_pTransportPosition->getFrame() -
			 m_pTransportPosition->getFrameOffsetTempo() )/
		static_cast<float>(pDriver->getSampleRate());
}

void AudioEngine::locate( const double fTick, bool bWithJackBroadcast ) {
	const auto pHydrogen = Hydrogen::get_instance();

	// DEBUGLOG( QString( "fTick: %1" ).arg( fTick ) );

#ifdef H2CORE_HAVE_JACK
	// In case Hydrogen is using the JACK server to sync transport, it
	// has to be up to the server to relocate to a different
	// position.
	if ( pHydrogen->hasJackTransport() && bWithJackBroadcast ) {
		double fTickMismatch;
		const long long nNewFrame =	TransportPosition::computeFrameFromTick(
			fTick, &fTickMismatch );
		static_cast<JackAudioDriver*>( m_pAudioDriver )->locateTransport( nNewFrame );
		return;
	}
#endif

	resetOffsets();
	m_fLastTickEnd = fTick;
	const long long nNewFrame = TransportPosition::computeFrameFromTick(
		fTick, &m_pTransportPosition->m_fTickMismatch );

	updateTransportPosition( fTick, nNewFrame, m_pTransportPosition );
	m_pQueuingPosition->set( m_pTransportPosition );
	
	handleTempoChange();
}

void AudioEngine::locateToFrame( const long long nFrame ) {

	// DEBUGLOG( QString( "nFrame: %1" ).arg( nFrame ) );
	
	resetOffsets();

	double fNewTick = TransportPosition::computeTickFromFrame( nFrame );

	// As the tick mismatch is lost when converting a sought location
	// from ticks into frames, sending it to the JACK server,
	// receiving it in a callback, and providing it here, we will use
	// a heuristic in order to not experience any glitches upon
	// relocation.
	if ( std::fmod( fNewTick, std::floor( fNewTick ) ) >= 0.97 ) {
		INFOLOG( QString( "Computed tick [%1] will be rounded to [%2] in order to avoid glitches" )
				 .arg( fNewTick, 0, 'E', -1 ).arg( std::round( fNewTick ) ) );
		fNewTick = std::round( fNewTick );
	}
	m_fLastTickEnd = fNewTick;

	// Assure tick<->frame can be converted properly using mismatch.
	const long long nNewFrame = TransportPosition::computeFrameFromTick(
		fNewTick, &m_pTransportPosition->m_fTickMismatch );
	if ( nNewFrame != nFrame ) {
		ERRORLOG( QString( "Something went wrong: nFrame: %1, nNewFrame: %2, fNewTick: %3, m_fTickMismatch: %4" )
				  .arg( nFrame ).arg( nNewFrame ).arg( fNewTick )
				  .arg( m_pQueuingPosition->m_fTickMismatch ) );
	}

	updateTransportPosition( fNewTick, nNewFrame, m_pTransportPosition );
	m_pQueuingPosition->set( m_pTransportPosition );

	handleTempoChange();

	// While the locate function is wrapped by a caller in the
	// CoreActionController - which takes care of queuing the
	// relocation event - this function is only meant to be used in
	// very specific circumstances and is not that nicely wrapped.
	EventQueue::get_instance()->push_event( EVENT_RELOCATION, 0 );
}

void AudioEngine::resetOffsets() {
	clearNoteQueue();

	// m_fLastTickEnd = 0;
	m_nLastLeadLagFactor = 0;
	m_bLookaheadApplied = false;

	m_pTransportPosition->setFrameOffsetTempo( 0 );
	m_pTransportPosition->setTickOffsetQueuing( 0 );
	m_pTransportPosition->setTickOffsetSongSize( 0 );
	m_pQueuingPosition->setFrameOffsetTempo( 0 );
	m_pQueuingPosition->setTickOffsetQueuing( 0 );
	m_pQueuingPosition->setTickOffsetSongSize( 0 );
}

void AudioEngine::incrementTransportPosition( uint32_t nFrames ) {
	auto pSong = Hydrogen::get_instance()->getSong();

	if ( pSong == nullptr ) {
		return;
	}	

	const long long nNewFrame = m_pTransportPosition->getFrame() + nFrames;
	const double fNewTick = TransportPosition::computeTickFromFrame( nNewFrame );
	m_pTransportPosition->m_fTickMismatch = 0;
		
	// DEBUGLOG( QString( "nFrames: %1, old frame: %2, new frame: %3, old tick: %4, new tick: %5, ticksize: %6" )
	// 		  .arg( nFrames )
	// 		  .arg( m_pTransportPosition->getFrame() )
	// 		  .arg( nNewFrame )
	// 		  .arg( m_pTransportPosition->getDoubleTick(), 0, 'f' )
	// 		  .arg( fNewTick, 0, 'f' )
	// 		  .arg( m_pTransportPosition->getTickSize(), 0, 'f' ) );
	updateTransportPosition( fNewTick, nNewFrame, m_pTransportPosition );

	// We are not updating the queuing position in here. This will be
	// done in updateNoteQueue.
}

void AudioEngine::updateTransportPosition( double fTick, long long nFrame, std::shared_ptr<TransportPosition> pPos ) {

	const auto pHydrogen = Hydrogen::get_instance();
	const auto pSong = pHydrogen->getSong();

	assert( pSong );
	
	// WARNINGLOG( QString( "[Before] fTick: %1, nFrame: %2, pos: %3" )
	// 			.arg( fTick, 0, 'f' )
	// 			.arg( nFrame )
	// 			.arg( pPos->toQString( "", true ) ) );

	// Update pPos->m_nPatternStartTick, pPos->m_nPatternTickPosition,
	// and pPos->m_nPatternSize.
	if ( pHydrogen->getMode() == Song::Mode::Song ) {
		updateSongTransportPosition( fTick, nFrame, pPos );
	}
	else {  // Song::Mode::Pattern
		updatePatternTransportPosition( fTick, nFrame, pPos );
	}

	updateBpmAndTickSize( pPos );
	
	// WARNINGLOG( QString( "[After] fTick: %1, nFrame: %2, pos: %3, frame: %4" )
	// 			.arg( fTick, 0, 'f' )
	// 			.arg( nFrame )
	// 			.arg( pPos->toQString( "", true ) )
	// 			.arg( pPos->getFrame() ) );
	
}

void AudioEngine::updatePatternTransportPosition( double fTick, long long nFrame, std::shared_ptr<TransportPosition> pPos ) {

	auto pHydrogen = Hydrogen::get_instance();

	pPos->setTick( fTick );
	pPos->setFrame( nFrame );

	// In selected pattern mode we update the pattern size _before_
	// checking the whether transport reached the end of a
	// pattern. This way when switching from a shorter to one double
	// its size transport will just continue till the end of the new,
	// longer one. If we would not update pattern size, transport
	// would be looped at half the length of the newly selected
	// pattern, which looks like a glitch.
	//
	// The update of the playing pattern is done asynchronous in
	// Hydrogen::setSelectedPatternNumber() and does not have to
	// queried in here in each run.

	// Transport went past the end of the pattern or Pattern mode
	// was just activated.
	const double fPatternStartTick =
		static_cast<double>(pPos->getPatternStartTick());
	const int nPatternSize = pPos->getPatternSize();
	
	if ( fTick >= fPatternStartTick + static_cast<double>(nPatternSize) ||
		 fTick < fPatternStartTick ) {
		pPos->setPatternStartTick( pPos->getPatternStartTick() +
								   static_cast<long>(std::floor( ( fTick - fPatternStartTick ) /
																 static_cast<double>(nPatternSize) )) *
								   nPatternSize );

		// In stacked pattern mode we will only update the playing
		// patterns if the transport of the original pattern is looped
		// back to the beginning. This way all patterns start fresh.
		//
		// In selected pattern mode pattern change does occur
		// asynchonically by user interaction.
		if ( pHydrogen->getPatternMode() == Song::PatternMode::Stacked ) {
			updatePlayingPatternsPos( pPos );
		}
	}

	long nPatternTickPosition = static_cast<long>(std::floor( fTick )) -
		pPos->getPatternStartTick();
	if ( nPatternTickPosition > nPatternSize ) {
		nPatternTickPosition = ( static_cast<long>(std::floor( fTick ))
								 - pPos->getPatternStartTick() ) %
			nPatternSize;
	}
	pPos->setPatternTickPosition( nPatternTickPosition );
}

void AudioEngine::updateSongTransportPosition( double fTick, long long nFrame, std::shared_ptr<TransportPosition> pPos ) {
	
	// WARNINGLOG( QString( "[Before] fTick: %1, nFrame: %2, pos: %3" )
	// 			.arg( fTick, 0, 'f' )
	// 			.arg( nFrame )
	// 			.arg( pPos->toQString( "", true ) ) );

	const auto pHydrogen = Hydrogen::get_instance();
	const auto pSong = pHydrogen->getSong();

	pPos->setTick( fTick );
	pPos->setFrame( nFrame );

	if ( fTick < 0 ) {
		ERRORLOG( QString( "[%1] Provided tick [%2] is negative!" )
				  .arg( pPos->getLabel() )
				  .arg( fTick, 0, 'f' ) );
		return;
	}

	long nPatternStartTick;
	const int nNewColumn = pHydrogen->getColumnForTick(
		std::floor( fTick ), pSong->isLoopEnabled(), &nPatternStartTick );
	pPos->setPatternStartTick( nPatternStartTick );

	// While the current tick position is constantly increasing,
	// m_nPatternStartTick is only defined between 0 and
	// m_fSongSizeInTicks. We will take care of the looping next.
	if ( fTick >= m_fSongSizeInTicks && m_fSongSizeInTicks != 0 ) {
		
		pPos->setPatternTickPosition(
			std::fmod( std::floor( fTick ) - nPatternStartTick,
					   m_fSongSizeInTicks ) );
	}
	else {
		pPos->setPatternTickPosition( std::floor( fTick ) - nPatternStartTick );
	}
	
	if ( pPos->getColumn() != nNewColumn ) {
		pPos->setColumn( nNewColumn );

		updatePlayingPatternsPos( pPos );
		handleSelectedPattern();
	}
	
	// WARNINGLOG( QString( "[After] fTick: %1, nFrame: %2, pos: %3, frame: %4" )
	// 			.arg( fTick, 0, 'f' )
	// 			.arg( nFrame )
	// 			.arg( pPos->toQString( "", true ) )
	// 			.arg( pPos->getFrame() ) );
	
}

void AudioEngine::updateBpmAndTickSize( std::shared_ptr<TransportPosition> pPos ) {
	if ( ! ( m_state == State::Playing ||
			 m_state == State::Ready ||
			 m_state == State::Testing ) ) {
		return;
	}
	
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	
	const float fOldBpm = pPos->getBpm();
	
	const float fNewBpm = getBpmAtColumn( pPos->getColumn() );
	if ( fNewBpm != fOldBpm ) {
		pPos->setBpm( fNewBpm );
		EventQueue::get_instance()->push_event( EVENT_TEMPO_CHANGED, 0 );
	}

	const float fOldTickSize = pPos->getTickSize();
	const float fNewTickSize =
		AudioEngine::computeTickSize( static_cast<float>(m_pAudioDriver->getSampleRate()),
									  fNewBpm, pSong->getResolution() );
	// Nothing changed - avoid recomputing
	if ( fNewTickSize == fOldTickSize ) {
		return;
	}

	if ( fNewTickSize == 0 ) {
		ERRORLOG( QString( "[%1] Something went wrong while calculating the tick size. [oldTS: %2, newTS: %3]" )
				  .arg( pPos->getLabel() )
				  .arg( fOldTickSize, 0, 'f' ).arg( fNewTickSize, 0, 'f' ) );
		return;
	}

	m_nLastLeadLagFactor = 0;

	// DEBUGLOG(QString( "[%1] [%7,%8] sample rate: %2, tick size: %3 -> %4, bpm: %5 -> %6" )
	// 		 .arg( pPos->getLabel() )
	// 		 .arg( static_cast<float>(m_pAudioDriver->getSampleRate()))
	// 		 .arg( fOldTickSize, 0, 'f' )
	// 		 .arg( fNewTickSize, 0, 'f' )
	// 		 .arg( fOldBpm, 0, 'f' )
	// 		 .arg( pPos->getBpm(), 0, 'f' )
	// 		 .arg( pPos->getFrame() )
	// 		 .arg( pPos->getDoubleTick(), 0, 'f' ) );
	
	pPos->setTickSize( fNewTickSize );
	
	calculateTransportOffsetOnBpmChange( pPos );
}

void AudioEngine::calculateTransportOffsetOnBpmChange( std::shared_ptr<TransportPosition> pPos ) {

	// If we deal with a single speed for the whole song, the frames
	// since the beginning of the song are tempo-dependent and have to
	// be recalculated.
	const long long nNewFrame =
		TransportPosition::computeFrameFromTick( pPos->getDoubleTick(),
												 &pPos->m_fTickMismatch );
	pPos->setFrameOffsetTempo( nNewFrame - pPos->getFrame() +
							   pPos->getFrameOffsetTempo() );

	if ( m_fLastTickEnd != 0 ) {
		const long long nNewLookahead =
			getLeadLagInFrames( pPos->getDoubleTick() ) +
			AudioEngine::nMaxTimeHumanize + 1;
		const double fNewTickEnd = TransportPosition::computeTickFromFrame(
			nNewFrame + nNewLookahead );
		pPos->setTickOffsetQueuing( fNewTickEnd - m_fLastTickEnd );

		// DEBUGLOG( QString( "[%1 : [%2] timeline] old frame: %3, new frame: %4, tick: %5, nNewLookahead: %6, pPos->getFrameOffsetTempo(): %7, pPos->getTickOffsetQueuing(): %8, fNewTickEnd: %9, m_fLastTickEnd: %10" )
		// 		  .arg( pPos->getLabel() )
		// 		  .arg( Hydrogen::get_instance()->isTimelineEnabled() )
		// 		  .arg( pPos->getFrame() )
		// 		  .arg( nNewFrame )
		// 		  .arg( pPos->getDoubleTick(), 0, 'f' )
		// 		  .arg( nNewLookahead )
		// 		  .arg( pPos->getFrameOffsetTempo() )
		// 		  .arg( pPos->getTickOffsetQueuing(), 0, 'f' )
		// 		  .arg( fNewTickEnd, 0, 'f' )
		// 		  .arg( m_fLastTickEnd, 0, 'f' )
		// 		  );
	}

	// Happens when the Timeline was either toggled or tempo
	// changed while the former was deactivated.
	if ( pPos->getFrame() != nNewFrame ) {
		pPos->setFrame( nNewFrame );
	}

	handleTempoChange();
}

void AudioEngine::clearAudioBuffers( uint32_t nFrames )
{
	QMutexLocker mx( &m_MutexOutputPointer );
	float *pBuffer_L, *pBuffer_R;

	// clear main out Left and Right
	if ( m_pAudioDriver != nullptr ) {
		pBuffer_L = m_pAudioDriver->getOut_L();
		pBuffer_R = m_pAudioDriver->getOut_R();
		assert( pBuffer_L != nullptr && pBuffer_R != nullptr );
		memset( pBuffer_L, 0, nFrames * sizeof( float ) );
		memset( pBuffer_R, 0, nFrames * sizeof( float ) );
	}
	
#ifdef H2CORE_HAVE_JACK
	if ( Hydrogen::get_instance()->hasJackAudioDriver() ) {
		JackAudioDriver* pJackAudioDriver = static_cast<JackAudioDriver*>(m_pAudioDriver);
	
		if ( pJackAudioDriver != nullptr ) {
			pJackAudioDriver->clearPerTrackAudioBuffers( nFrames );
		}
	}
#endif

	mx.unlock();

#ifdef H2CORE_HAVE_LADSPA
	if ( getState() == State::Ready ||
		 getState() == State::Playing ||
		 getState() == State::Testing ) {
		Effects* pEffects = Effects::get_instance();
		for ( unsigned i = 0; i < MAX_FX; ++i ) {	// clear FX buffers
			LadspaFX* pFX = pEffects->getLadspaFX( i );
			if ( pFX != nullptr ) {
				assert( pFX->m_pBuffer_L );
				assert( pFX->m_pBuffer_R );
				memset( pFX->m_pBuffer_L, 0, nFrames * sizeof( float ) );
				memset( pFX->m_pBuffer_R, 0, nFrames * sizeof( float ) );
			}
		}
	}
#endif
}

AudioOutput* AudioEngine::createAudioDriver( const QString& sDriver )
{
	INFOLOG( QString( "Creating driver [%1]" ).arg( sDriver ) );

	auto pPref = Preferences::get_instance();
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	AudioOutput *pAudioDriver = nullptr;

	if ( sDriver == "OSS" ) {
		pAudioDriver = new OssDriver( m_AudioProcessCallback );
	} else if ( sDriver == "JACK" ) {
		pAudioDriver = new JackAudioDriver( m_AudioProcessCallback );
#ifdef H2CORE_HAVE_JACK
		if ( auto pJackDriver = dynamic_cast<JackAudioDriver*>( pAudioDriver ) ) {
			pJackDriver->setConnectDefaults(
				Preferences::get_instance()->m_bJackConnectDefaults
			);
		}
#endif
	}
	else if ( sDriver == "ALSA" ) {
		pAudioDriver = new AlsaAudioDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "PortAudio" ) {
		pAudioDriver = new PortAudioDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "CoreAudio" ) {
		pAudioDriver = new CoreAudioDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "PulseAudio" ) {
		pAudioDriver = new PulseAudioDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "Fake" ) {
		WARNINGLOG( "*** Using FAKE audio driver ***" );
		pAudioDriver = new FakeDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "DiskWriterDriver" ) {
		pAudioDriver = new DiskWriterDriver( m_AudioProcessCallback );
	}
	else if ( sDriver == "NullDriver" ) {
		pAudioDriver = new NullDriver( m_AudioProcessCallback );
	}
	else {
		ERRORLOG( QString( "Unknown driver [%1]" ).arg( sDriver ) );
		raiseError( Hydrogen::UNKNOWN_DRIVER );
		return nullptr;
	}

	if ( pAudioDriver == nullptr ) {
		INFOLOG( QString( "Unable to create driver [%1]" ).arg( sDriver ) );
		return nullptr;
	}

	// Initialize the audio driver
	int nRes = pAudioDriver->init( pPref->m_nBufferSize );
	if ( nRes != 0 ) {
		ERRORLOG( QString( "Error code [%2] while initializing audio driver [%1]." )
				  .arg( sDriver ).arg( nRes ) );
		delete pAudioDriver;
		return nullptr;
	}

	this->lock( RIGHT_HERE );
	QMutexLocker mx(&m_MutexOutputPointer);

	// Some audio drivers require to be already registered in the
	// AudioEngine while being connected.
	m_pAudioDriver = pAudioDriver;

	// change the current audio engine state
	if ( pSong != nullptr ) {
		setState( State::Ready );
	} else {
		setState( State::Prepared );
	}

	// Unlocking earlier might execute the jack process() callback before we
	// are fully initialized.
	mx.unlock();
	this->unlock();
	
	nRes = m_pAudioDriver->connect();
	if ( nRes != 0 ) {
		raiseError( Hydrogen::ERROR_STARTING_DRIVER );
		ERRORLOG( QString( "Error code [%2] while connecting audio driver [%1]." )
				  .arg( sDriver ).arg( nRes ) );

		this->lock( RIGHT_HERE );
		mx.relock();
		
		delete m_pAudioDriver;
		m_pAudioDriver = nullptr;
		
		mx.unlock();
		this->unlock();

		return nullptr;
	}

	if ( pSong != nullptr && pHydrogen->hasJackAudioDriver() ) {
		pHydrogen->renameJackPorts( pSong );
	}
		
	setupLadspaFX();

	if ( pSong != nullptr ) {
		handleDriverChange();
	}

	EventQueue::get_instance()->push_event( EVENT_DRIVER_CHANGED, 0 );

	return pAudioDriver;
}

void AudioEngine::startAudioDrivers()
{
	INFOLOG("");
	Preferences *pPref = Preferences::get_instance();
	
	// check current state
	if ( getState() != State::Initialized ) {
		ERRORLOG( QString( "Audio engine is not in State::Initialized but [%1]" )
				  .arg( static_cast<int>( getState() ) ) );
		return;
	}

	if ( m_pAudioDriver ) {	// check if the audio m_pAudioDriver is still alive
		ERRORLOG( "The audio driver is still alive" );
	}
	if ( m_pMidiDriver ) {	// check if midi driver is still alive
		ERRORLOG( "The MIDI driver is still active" );
	}

	QString sAudioDriver = pPref->m_sAudioDriver;
#if defined(WIN32)
	QStringList drivers = { "PortAudio", "JACK" };
#elif defined(__APPLE__)
    QStringList drivers = { "CoreAudio", "JACK", "PulseAudio", "PortAudio" };
#else /* Linux */
    QStringList drivers = { "JACK", "ALSA", "OSS", "PulseAudio", "PortAudio" };
#endif

	if ( sAudioDriver != "Auto" ) {
		drivers.clear();
		drivers << sAudioDriver;
	}
	AudioOutput* pAudioDriver;
	for ( QString sDriver : drivers ) {
		if ( ( pAudioDriver = createAudioDriver( sDriver ) ) != nullptr ) {
			break;
		}
	}

	// If the audio driver could not be created, we resort to the
	// NullDriver.
	if ( m_pAudioDriver == nullptr ) {
		ERRORLOG( QString( "Couldn't start audio driver [%1], falling back to NullDriver" )
				  .arg( sAudioDriver ) );
		createAudioDriver( "NullDriver" );
	}

	// Lock both the AudioEngine and the audio output buffers.
	this->lock( RIGHT_HERE );
	QMutexLocker mx(&m_MutexOutputPointer);
	
	if ( pPref->m_sMidiDriver == "ALSA" ) {
#ifdef H2CORE_HAVE_ALSA
		// Create MIDI driver
		AlsaMidiDriver *alsaMidiDriver = new AlsaMidiDriver();
		m_pMidiDriverOut = alsaMidiDriver;
		m_pMidiDriver = alsaMidiDriver;
		m_pMidiDriver->open();
		m_pMidiDriver->setActive( true );
#endif
	} else if ( pPref->m_sMidiDriver == "PortMidi" ) {
#ifdef H2CORE_HAVE_PORTMIDI
		PortMidiDriver* pPortMidiDriver = new PortMidiDriver();
		m_pMidiDriver = pPortMidiDriver;
		m_pMidiDriverOut = pPortMidiDriver;
		m_pMidiDriver->open();
		m_pMidiDriver->setActive( true );
#endif
	} else if ( pPref->m_sMidiDriver == "CoreMIDI" ) {
#ifdef H2CORE_HAVE_COREMIDI
		CoreMidiDriver *coreMidiDriver = new CoreMidiDriver();
		m_pMidiDriver = coreMidiDriver;
		m_pMidiDriverOut = coreMidiDriver;
		m_pMidiDriver->open();
		m_pMidiDriver->setActive( true );
#endif
	} else if ( pPref->m_sMidiDriver == "JACK-MIDI" ) {
#ifdef H2CORE_HAVE_JACK
		JackMidiDriver *jackMidiDriver = new JackMidiDriver();
		m_pMidiDriverOut = jackMidiDriver;
		m_pMidiDriver = jackMidiDriver;
		m_pMidiDriver->open();
		m_pMidiDriver->setActive( true );
#endif
	}
	
	mx.unlock();
	this->unlock();
}

void AudioEngine::stopAudioDrivers()
{
	INFOLOG( "" );

	// check current state
	if ( m_state == State::Playing ) {
		this->stopPlayback(); 
	}

	if ( ( m_state != State::Prepared )
		 && ( m_state != State::Ready ) ) {
		ERRORLOG( QString( "Audio engine is not in State::Prepared or State::Ready but [%1]" )
				  .arg( static_cast<int>(m_state) ) );
		return;
	}

	this->lock( RIGHT_HERE );

	// change the current audio engine state
	setState( State::Initialized );

	// delete MIDI driver
	if ( m_pMidiDriver != nullptr ) {
		m_pMidiDriver->close();
		delete m_pMidiDriver;
		m_pMidiDriver = nullptr;
		m_pMidiDriverOut = nullptr;
	}

	// delete audio driver
	if ( m_pAudioDriver != nullptr ) {
		m_pAudioDriver->disconnect();
		QMutexLocker mx( &m_MutexOutputPointer );
		delete m_pAudioDriver;
		m_pAudioDriver = nullptr;
		mx.unlock();
	}

	this->unlock();
}

/** 
 * Restart all audio and midi drivers.
 */
void AudioEngine::restartAudioDrivers()
{
	if ( m_pAudioDriver != nullptr ) {
		stopAudioDrivers();
	}
	startAudioDrivers();
}

void AudioEngine::handleDriverChange() {

	if ( Hydrogen::get_instance()->getSong() == nullptr ) {
		WARNINGLOG( "no song set yet" );
		return;
	}
	
	handleTimelineChange();
}

float AudioEngine::getBpmAtColumn( int nColumn ) {

	auto pHydrogen = Hydrogen::get_instance();
	auto pAudioEngine = pHydrogen->getAudioEngine();
	auto pSong = pHydrogen->getSong();

	if ( pSong == nullptr ) {
		WARNINGLOG( "no song set yet" );
		return MIN_BPM;
	}

	float fBpm = pAudioEngine->getTransportPosition()->getBpm();

	// Check for a change in the current BPM.
	if ( pHydrogen->getJackTimebaseState() == JackAudioDriver::Timebase::Slave &&
		 pHydrogen->getMode() == Song::Mode::Song ) {
		// Hydrogen is using the BPM broadcast by the JACK
		// server. This one does solely depend on external
		// applications and will NOT be stored in the Song.
		const float fJackMasterBpm = pHydrogen->getMasterBpm();
		if ( ! std::isnan( fJackMasterBpm ) && fBpm != fJackMasterBpm ) {
			fBpm = fJackMasterBpm;
			// DEBUGLOG( QString( "Tempo update by the JACK server [%1]").arg( fJackMasterBpm ) );
		}
	}
	else if ( pSong->getIsTimelineActivated() &&
				pHydrogen->getMode() == Song::Mode::Song ) {

		const float fTimelineBpm = pHydrogen->getTimeline()->getTempoAtColumn( nColumn );
		if ( fTimelineBpm != fBpm ) {
			// DEBUGLOG( QString( "Set tempo to timeline value [%1]").arg( fTimelineBpm ) );
			fBpm = fTimelineBpm;
		}
	}
	else {
		// Change in speed due to user interaction with the BPM widget
		// or corresponding MIDI or OSC events.
		if ( pAudioEngine->getNextBpm() != fBpm ) {
			// DEBUGLOG( QString( "BPM changed via Widget, OSC, or MIDI from [%1] to [%2]." )
			// 		  .arg( fBpm ).arg( pAudioEngine->getNextBpm() ) );
			fBpm = pAudioEngine->getNextBpm();
		}
	}
	return fBpm;
}

void AudioEngine::setupLadspaFX()
{
	Hydrogen* pHydrogen = Hydrogen::get_instance();
	std::shared_ptr<Song> pSong = pHydrogen->getSong();
	if ( ! pSong ) {
		return;
	}

#ifdef H2CORE_HAVE_LADSPA
	for ( unsigned nFX = 0; nFX < MAX_FX; ++nFX ) {
		LadspaFX *pFX = Effects::get_instance()->getLadspaFX( nFX );
		if ( pFX == nullptr ) {
			return;
		}

		pFX->deactivate();

		Effects::get_instance()->getLadspaFX( nFX )->connectAudioPorts(
					pFX->m_pBuffer_L,
					pFX->m_pBuffer_R,
					pFX->m_pBuffer_L,
					pFX->m_pBuffer_R
					);
		pFX->activate();
	}
#endif
}

void AudioEngine::raiseError( unsigned nErrorCode )
{
	m_pEventQueue->push_event( EVENT_ERROR, nErrorCode );
}

void AudioEngine::handleSelectedPattern() {
	// This function keeps the selected pattern in line with the one
	// the transport position resides in
	
	const auto pHydrogen = Hydrogen::get_instance();
	const auto pSong = pHydrogen->getSong();
	
	if ( pHydrogen->isPatternEditorLocked() &&
		 ( m_state == State::Playing ||
		   m_state == State::Testing ) ) {
		
		int nPatternNumber = -1;

		const int nColumn = std::max( m_pTransportPosition->getColumn(), 0 );
		if ( nColumn >= (*pSong->getPatternGroupVector()).size() ) {

			const auto pPatternList = pSong->getPatternList();
			if ( pPatternList == nullptr ) {

				const auto pColumn = ( *pSong->getPatternGroupVector() )[ nColumn ];

				int nIndex;
				for ( const auto& pattern : *pColumn ) {
					nIndex = pPatternList->index( pattern );

					if ( nIndex > nPatternNumber ) {
						nPatternNumber = nIndex;
					}
				}
			}
		}

		pHydrogen->setSelectedPatternNumber( nPatternNumber, true );
	}
}

void AudioEngine::processPlayNotes( unsigned long nframes )
{
	Hydrogen* pHydrogen = Hydrogen::get_instance();
	std::shared_ptr<Song> pSong = pHydrogen->getSong();

	long long nFrame;
	if ( getState() == State::Playing || getState() == State::Testing ) {
		// Current transport position.
		nFrame = m_pTransportPosition->getFrame();
		
	} else {
		// In case the playback is stopped and all realtime events,
		// by e.g. MIDI or Hydrogen's virtual keyboard, we disregard
		// tempo changes in the Timeline and pretend the current tick
		// size is valid for all future notes.
		nFrame = getRealtimeFrame();
	}

	// reading from m_songNoteQueue
	while ( !m_songNoteQueue.empty() ) {
		Note *pNote = m_songNoteQueue.top();
		const long long nNoteStartInFrames = pNote->getNoteStart();

		// DEBUGLOG( QString( "m_pTransportPosition->getDoubleTick(): %1, m_pTransportPosition->getFrame(): %2, nframes: %3, " )
		// 		  .arg( m_pTransportPosition->getDoubleTick() )
		// 		  .arg( m_pTransportPosition->getFrame() )
		// 		  .arg( nframes )
		// 		  .append( pNote->toQString( "", true ) ) );

		if ( nNoteStartInFrames <
			 nFrame + static_cast<long long>(nframes) ) {
			/* Check if the current note has probability != 1
			 * If yes remove call random function to dequeue or not the note
			 */
			float fNoteProbability = pNote->get_probability();
			if ( fNoteProbability != 1. ) {
				if ( fNoteProbability < (float) rand() / (float) RAND_MAX ) {
					m_songNoteQueue.pop();
					pNote->get_instrument()->dequeue();
					continue;
				}
			}

			if ( pSong->getHumanizeVelocityValue() != 0 ) {
				const float fRandom = pSong->getHumanizeVelocityValue() * getGaussian( 0.2 );
				pNote->set_velocity(
							pNote->get_velocity()
							+ ( fRandom
								- ( pSong->getHumanizeVelocityValue() / 2.0 ) )
							);
				if ( pNote->get_velocity() > 1.0 ) {
					pNote->set_velocity( 1.0 );
				} else if ( pNote->get_velocity() < 0.0 ) {
					pNote->set_velocity( 0.0 );
				}
			}

			// Offset + Random Pitch ;)
			float fPitch = pNote->get_pitch() + pNote->get_instrument()->get_pitch_offset();
			/* Check if the current instrument has random pitch factor != 0.
			 * If yes add a gaussian perturbation to the pitch
			 */
			const float fRandomPitchFactor = pNote->get_instrument()->get_random_pitch_factor();
			if ( fRandomPitchFactor != 0. ) {
				fPitch += getGaussian( 0.4 ) * fRandomPitchFactor;
			}
			pNote->set_pitch( fPitch );

			/*
			 * Check if the current instrument has the property "Stop-Note" set.
			 * If yes, a NoteOff note is generated automatically after each note.
			 */
			auto pNoteInstrument = pNote->get_instrument();
			if ( pNoteInstrument->is_stop_notes() ){
				Note *pOffNote = new Note( pNoteInstrument,
										   0.0,
										   0.0,
										   0.0,
										   -1,
										   0 );
				pOffNote->set_note_off( true );
				m_pSampler->noteOn( pOffNote );
				delete pOffNote;
			}

			m_pSampler->noteOn( pNote );
			m_songNoteQueue.pop(); // rimuovo la nota dalla lista di note
			pNote->get_instrument()->dequeue();
			// raise noteOn event
			const int nInstrument = pSong->getInstrumentList()->index( pNote->get_instrument() );
			if( pNote->get_note_off() ){
				delete pNote;
			}

			// Check whether the instrument could be found.
			if ( nInstrument != -1 ) {
				m_pEventQueue->push_event( EVENT_NOTEON, nInstrument );
			}
			
			continue;
		} else {
			// this note will not be played
			break;
		}
	}
}

void AudioEngine::clearNoteQueue()
{
	// delete all copied notes in the song notes queue
	while ( !m_songNoteQueue.empty() ) {
		m_songNoteQueue.top()->get_instrument()->dequeue();
		delete m_songNoteQueue.top();
		m_songNoteQueue.pop();
	}

	// delete all copied notes in the midi notes queue
	for ( unsigned i = 0; i < m_midiNoteQueue.size(); ++i ) {
		delete m_midiNoteQueue[i];
	}
	m_midiNoteQueue.clear();
}

int AudioEngine::audioEngine_process( uint32_t nframes, void* /*arg*/ )
{
	AudioEngine* pAudioEngine = Hydrogen::get_instance()->getAudioEngine();
	timeval startTimeval = currentTime2();

	// Resetting all audio output buffers with zeros.
	pAudioEngine->clearAudioBuffers( nframes );

	// Calculate maximum time to wait for audio engine lock. Using the
	// last calculated processing time as an estimate of the expected
	// processing time for this frame, the amount of slack time that
	// we can afford to wait is: m_fMaxProcessTime - m_fProcessTime.

	float sampleRate = static_cast<float>(pAudioEngine->m_pAudioDriver->getSampleRate());
	pAudioEngine->m_fMaxProcessTime = 1000.0 / ( sampleRate / nframes );
	float fSlackTime = pAudioEngine->m_fMaxProcessTime - pAudioEngine->m_fProcessTime;

	// If we expect to take longer than the available time to process,
	// require immediate locking or not at all: we're bound to drop a
	// buffer anyway.
	if ( fSlackTime < 0.0 ) {
		fSlackTime = 0.0;
	}

	/*
	 * The "try_lock" was introduced for Bug #164 (Deadlock after during
	 * alsa driver shutdown). The try_lock *should* only fail in rare circumstances
	 * (like shutting down drivers). In such cases, it seems to be ok to interrupt
	 * audio processing. Returning the special return value "2" enables the disk 
	 * writer driver to repeat the processing of the current data.
	 */
	if ( !pAudioEngine->tryLockFor( std::chrono::microseconds( (int)(1000.0*fSlackTime) ),
							  RIGHT_HERE ) ) {
		___ERRORLOG( QString( "Failed to lock audioEngine in allowed %1 ms, missed buffer" ).arg( fSlackTime ) );

		if ( dynamic_cast<DiskWriterDriver*>(pAudioEngine->m_pAudioDriver) != nullptr ) {
			return 2;	// inform the caller that we could not acquire the lock
		}

		return 0;
	}

	if ( ! ( pAudioEngine->getState() == AudioEngine::State::Ready ||
			 pAudioEngine->getState() == AudioEngine::State::Playing ) ) {
		pAudioEngine->unlock();
		return 0;
	}

	Hydrogen* pHydrogen = Hydrogen::get_instance();
	std::shared_ptr<Song> pSong = pHydrogen->getSong();
	assert( pSong );

	// Sync transport with server (in case the current audio driver is
	// designed that way)
#ifdef H2CORE_HAVE_JACK
	if ( Hydrogen::get_instance()->hasJackTransport() ) {
		// Compares the current transport state, speed in bpm, and
		// transport position with a query request to the JACK
		// server. It will only overwrite the transport state, if
		// the transport position was changed by the user by
		// e.g. clicking on the timeline.
		static_cast<JackAudioDriver*>( pHydrogen->getAudioOutput() )->updateTransportPosition();
	}
#endif

	// Check whether the tempo was changed.
	pAudioEngine->updateBpmAndTickSize( pAudioEngine->m_pTransportPosition );
	pAudioEngine->updateBpmAndTickSize( pAudioEngine->m_pQueuingPosition );

	// Update the state of the audio engine depending on whether it
	// was started or stopped by the user.
	if ( pAudioEngine->getNextState() == State::Playing ) {
		if ( pAudioEngine->getState() == State::Ready ) {
			pAudioEngine->startPlayback();
		}
		
		pAudioEngine->setRealtimeFrame( pAudioEngine->m_pTransportPosition->getFrame() );
	} else {
		if ( pAudioEngine->getState() == State::Playing ) {
			pAudioEngine->stopPlayback();
		}
		
		// go ahead and increment the realtimeframes by nFrames
		// to support our realtime keyboard and midi event timing
		pAudioEngine->setRealtimeFrame( pAudioEngine->getRealtimeFrame() +
										 static_cast<long long>(nframes) );
	}

	// always update note queue.. could come from pattern or realtime input
	// (midi, keyboard)
	int nResNoteQueue = pAudioEngine->updateNoteQueue( nframes );
	if ( nResNoteQueue == -1 ) {	// end of song
		___INFOLOG( "End of song received" );
		pAudioEngine->stop();
		pAudioEngine->stopPlayback();
		pAudioEngine->locate( 0 );

		if ( dynamic_cast<FakeDriver*>(pAudioEngine->m_pAudioDriver) != nullptr ) {
			___INFOLOG( "End of song." );

			// TODO: This part of the code might not be reached
			// anymore.
			pAudioEngine->unlock();
			return 1;	// kill the audio AudioDriver thread
		}
	}

	pAudioEngine->processAudio( nframes );

	// increment the transport position
	if ( pAudioEngine->getState() == AudioEngine::State::Playing ) {
		pAudioEngine->incrementTransportPosition( nframes );
	}

	timeval finishTimeval = currentTime2();
	pAudioEngine->m_fProcessTime =
			( finishTimeval.tv_sec - startTimeval.tv_sec ) * 1000.0
			+ ( finishTimeval.tv_usec - startTimeval.tv_usec ) / 1000.0;
	
#ifdef CONFIG_DEBUG
	if ( pAudioEngine->m_fProcessTime > pAudioEngine->m_fMaxProcessTime ) {
		___WARNINGLOG( "" );
		___WARNINGLOG( "----XRUN----" );
		___WARNINGLOG( QString( "XRUN of %1 msec (%2 > %3)" )
					   .arg( ( pAudioEngine->m_fProcessTime - pAudioEngine->m_fMaxProcessTime ) )
					   .arg( pAudioEngine->m_fProcessTime ).arg( pAudioEngine->m_fMaxProcessTime ) );
		___WARNINGLOG( QString( "Ladspa process time = %1" ).arg( fLadspaTime ) );
		___WARNINGLOG( "------------" );
		___WARNINGLOG( "" );
		// raise xRun event
		EventQueue::get_instance()->push_event( EVENT_XRUN, -1 );
	}
#endif

	pAudioEngine->unlock();

	return 0;
}

void AudioEngine::processAudio( uint32_t nFrames ) {

	auto pSong = Hydrogen::get_instance()->getSong();

	// play all notes
	processPlayNotes( nFrames );

	float *pBuffer_L = m_pAudioDriver->getOut_L(),
		*pBuffer_R = m_pAudioDriver->getOut_R();
	assert( pBuffer_L != nullptr && pBuffer_R != nullptr );

	// SAMPLER
	getSampler()->process( nFrames, pSong );
	float* out_L = getSampler()->m_pMainOut_L;
	float* out_R = getSampler()->m_pMainOut_R;
	for ( unsigned i = 0; i < nFrames; ++i ) {
		pBuffer_L[ i ] += out_L[ i ];
		pBuffer_R[ i ] += out_R[ i ];
	}

	// SYNTH
	getSynth()->process( nFrames );
	out_L = getSynth()->m_pOut_L;
	out_R = getSynth()->m_pOut_R;
	for ( unsigned i = 0; i < nFrames; ++i ) {
		pBuffer_L[ i ] += out_L[ i ];
		pBuffer_R[ i ] += out_R[ i ];
	}

	timeval ladspaTime_start = currentTime2();

#ifdef H2CORE_HAVE_LADSPA
	// Process LADSPA FX
	for ( unsigned nFX = 0; nFX < MAX_FX; ++nFX ) {
		LadspaFX *pFX = Effects::get_instance()->getLadspaFX( nFX );
		if ( ( pFX ) && ( pFX->isEnabled() ) ) {
			pFX->processFX( nFrames );

			float *buf_L, *buf_R;
			if ( pFX->getPluginType() == LadspaFX::STEREO_FX ) {
				buf_L = pFX->m_pBuffer_L;
				buf_R = pFX->m_pBuffer_R;
			} else { // MONO FX
				buf_L = pFX->m_pBuffer_L;
				buf_R = buf_L;
			}

			for ( unsigned i = 0; i < nFrames; ++i ) {
				pBuffer_L[ i ] += buf_L[ i ];
				pBuffer_R[ i ] += buf_R[ i ];
				if ( buf_L[ i ] > m_fFXPeak_L[nFX] ) {
					m_fFXPeak_L[nFX] = buf_L[ i ];
				}

				if ( buf_R[ i ] > m_fFXPeak_R[nFX] ) {
					m_fFXPeak_R[nFX] = buf_R[ i ];
				}
			}
		}
	}
#endif
	timeval ladspaTime_end = currentTime2();
	m_fLadspaTime =
			( ladspaTime_end.tv_sec - ladspaTime_start.tv_sec ) * 1000.0
			+ ( ladspaTime_end.tv_usec - ladspaTime_start.tv_usec ) / 1000.0;

	// update master peaks
	float val_L, val_R;
	for ( unsigned i = 0; i < nFrames; ++i ) {
		val_L = pBuffer_L[i];
		val_R = pBuffer_R[i];

		if ( val_L > m_fMasterPeak_L ) {
			m_fMasterPeak_L = val_L;
		}

		if ( val_R > m_fMasterPeak_R ) {
			m_fMasterPeak_R = val_R;
		}
	}

	for ( auto component : *pSong->getComponents() ) {
		DrumkitComponent *pComponent = component.get();
		for ( unsigned i = 0; i < nFrames; ++i ) {
			float compo_val_L = pComponent->get_out_L(i);
			float compo_val_R = pComponent->get_out_R(i);

			if( compo_val_L > pComponent->get_peak_l() ) {
				pComponent->set_peak_l( compo_val_L );
			}
			if( compo_val_R > pComponent->get_peak_r() ) {
				pComponent->set_peak_r( compo_val_R );
			}
		}
	}

}

void AudioEngine::setState( AudioEngine::State state ) {
	m_state = state;
	EventQueue::get_instance()->push_event( EVENT_STATE, static_cast<int>(state) );
}

void AudioEngine::setNextBpm( float fNextBpm ) {
	if ( fNextBpm > MAX_BPM ) {
		m_fNextBpm = MAX_BPM;
		WARNINGLOG( QString( "Provided bpm %1 is too high. Assigning upper bound %2 instead" )
					.arg( fNextBpm ).arg( MAX_BPM ) );
	}
	else if ( fNextBpm < MIN_BPM ) {
		m_fNextBpm = MIN_BPM;
		WARNINGLOG( QString( "Provided bpm %1 is too low. Assigning lower bound %2 instead" )
					.arg( fNextBpm ).arg( MIN_BPM ) );
	}
	
	m_fNextBpm = fNextBpm;
}

void AudioEngine::setSong( std::shared_ptr<Song> pNewSong )
{
	auto pHydrogen = Hydrogen::get_instance();
	
	INFOLOG( QString( "Set song: %1" ).arg( pNewSong->getName() ) );
	
	this->lock( RIGHT_HERE );

	// check current state
	// should be set by removeSong called earlier
	if ( getState() != State::Prepared ) {
		ERRORLOG( QString( "Error the audio engine is not in State::Prepared but [%1]" )
				  .arg( static_cast<int>( getState() ) ) );
	}

	// setup LADSPA FX
	if ( m_pAudioDriver != nullptr ) {
		setupLadspaFX();
	}

	// Reset (among other things) the transport position. This causes
	// the locate() call below to update the playing patterns.
	reset( false );

	pHydrogen->renameJackPorts( pNewSong );
	m_fSongSizeInTicks = static_cast<double>( pNewSong->lengthInTicks() );

	// change the current audio engine state
	setState( State::Ready );

	setNextBpm( pNewSong->getBpm() );
	// Will adapt the audio engine to the song's BPM.
	locate( 0 );

	pHydrogen->setTimeline( pNewSong->getTimeline() );
	pHydrogen->getTimeline()->activate();

	this->unlock();
}

void AudioEngine::removeSong()
{
	this->lock( RIGHT_HERE );

	if ( getState() == State::Playing ) {
		stop();
		this->stopPlayback();
	}

	// check current state
	if ( getState() != State::Ready ) {
		ERRORLOG( QString( "Error the audio engine is not in State::Ready but [%1]" )
				  .arg( static_cast<int>( getState() ) ) );
		this->unlock();
		return;
	}

	m_pSampler->stopPlayingNotes();
	reset();

	// change the current audio engine state
	setState( State::Prepared );
	this->unlock();
}

void AudioEngine::updateSongSize() {
	
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();

	if ( pSong == nullptr ) {
		ERRORLOG( "No song set yet" );
		return;
	}

	auto updatePatternSize = []( std::shared_ptr<TransportPosition> pPos ) {
		if ( pPos->getPlayingPatterns()->size() > 0 ) {
			pPos->setPatternSize( pPos->getPlayingPatterns()->longest_pattern_length() );
		} else {
			pPos->setPatternSize( MAX_NOTES );
		}
	};
	updatePatternSize( m_pTransportPosition );
	updatePatternSize( m_pQueuingPosition );

	if ( pHydrogen->getMode() == Song::Mode::Pattern ) {
		EventQueue::get_instance()->push_event( EVENT_SONG_SIZE_CHANGED, 0 );
		return;
	}

	// Expected behavior:
	// - changing any part of the song except of the pattern currently
	//   play shouldn't affect transport position
	// - the current transport position is defined as current column &
	//   current pattern tick position
	// - there shouldn't be a difference in behavior whether the song
	//   was already looped or not
	// - the offsets used for compensation of the transport position
	//   will only be used internally and not propagated to external
	//   audio servers, like JACK.
	const double fNewSongSizeInTicks = static_cast<double>( pSong->lengthInTicks() );

	// Strip away all repetitions when in loop mode but keep their
	// number. nPatternStartTick and nColumn are only defined
	// between 0 and fSongSizeInTicks.
	double fNewStrippedTick = std::fmod( m_pTransportPosition->getDoubleTick(),
										 m_fSongSizeInTicks );
	const double fRepetitions =
		std::floor( m_pTransportPosition->getDoubleTick() / m_fSongSizeInTicks );
	const int nOldColumn = m_pTransportPosition->getColumn();

	// WARNINGLOG( QString( "[Before] fNewStrippedTick: %1, fRepetitions: %2, m_fSongSizeInTicks: %3, fNewSongSizeInTicks: %4, transport: %5, queuing: %6" )
	// 			.arg( fNewStrippedTick, 0, 'f' )
	// 			.arg( fRepetitions )
	// 			.arg( m_fSongSizeInTicks )
	// 			.arg( fNewSongSizeInTicks )
	// 			.arg( m_pTransportPosition->toQString( "", true ) )
	// 			.arg( m_pQueuingPosition->toQString( "", true ) )
	// 			);

	m_fSongSizeInTicks = fNewSongSizeInTicks;

	auto endOfSongReached = [&](){
		if ( pSong->getLoopMode() != Song::LoopMode::Enabled ) {
			stop();
			stopPlayback();
		}
		locate( 0 );
		
		// WARNINGLOG( QString( "[End of song reached] fNewStrippedTick: %1, fRepetitions: %2, m_fSongSizeInTicks: %3, fNewSongSizeInTicks: %4, transport: %5, queuing: %6" )
		// 			.arg( fNewStrippedTick, 0, 'f' )
		// 			.arg( fRepetitions )
		// 			.arg( m_fSongSizeInTicks )
		// 			.arg( fNewSongSizeInTicks )
		// 			.arg( m_pTransportPosition->toQString( "", true ) )
		// 			.arg( m_pQueuingPosition->toQString( "", true ) )
		// 			);
		
		EventQueue::get_instance()->push_event( EVENT_SONG_SIZE_CHANGED, 0 );
	};

	if ( nOldColumn >= pSong->getPatternGroupVector()->size() ) {
		// Old column exceeds the new song size.
		endOfSongReached();
		return;
	}
		

	const long nNewPatternStartTick = pHydrogen->getTickForColumn( nOldColumn );

	if ( nNewPatternStartTick == -1 ) {
		// Failsave in case old column exceeds the new song size.
		endOfSongReached();
		return;
	}
	
	if ( nNewPatternStartTick != m_pTransportPosition->getPatternStartTick() ) {
		// A pattern prior to the current position was toggled,
		// enlarged, or shrunk. We need to compensate this in order to
		// keep the current pattern tick position constant.

		// DEBUGLOG( QString( "[nPatternStartTick mismatch] old: %1, new: %2" )
		// 		  .arg( m_pTransportPosition->getPatternStartTick() )
		// 		  .arg( nNewPatternStartTick ) );
		
		fNewStrippedTick +=
			static_cast<double>(nNewPatternStartTick -
								m_pTransportPosition->getPatternStartTick());
	}
	
#ifdef H2CORE_HAVE_DEBUG
	const long nNewPatternTickPosition =
		static_cast<long>(std::floor( fNewStrippedTick )) - nNewPatternStartTick;
	if ( nNewPatternTickPosition !=
		 m_pTransportPosition->getPatternTickPosition() ) {
		ERRORLOG( QString( "[nPatternTickPosition mismatch] old: %1, new: %2" )
				  .arg( m_pTransportPosition->getPatternTickPosition() )
				  .arg( nNewPatternTickPosition ) );
	}
#endif

	// Incorporate the looped transport again
	const double fNewTick = fNewStrippedTick + fRepetitions * fNewSongSizeInTicks;
	const long long nNewFrame = TransportPosition::computeFrameFromTick(
		fNewTick, &m_pTransportPosition->m_fTickMismatch );

	double fTickOffset = fNewTick - m_pTransportPosition->getDoubleTick();

	// The tick interval end covered in updateNoteQueue is stored as
	// double and needs to be more precise.
	m_fLastTickEnd += fTickOffset;

	// Small rounding noise introduced in the calculation does spoil
	// things as we floor the resulting tick offset later on. Hence,
	// we round it to a specific precision.
	fTickOffset *= 1e8;
	fTickOffset = std::round( fTickOffset );
	fTickOffset *= 1e-8;
	m_pTransportPosition->setTickOffsetSongSize( fTickOffset );

	// Moves all notes currently processed by Hydrogen with respect to
	// the offsets calculated above.
	handleSongSizeChange();

	m_pTransportPosition->setFrameOffsetTempo(
		nNewFrame - m_pTransportPosition->getFrame() +
		m_pTransportPosition->getFrameOffsetTempo() );
		
	// INFOLOG(QString( "[update] nNewFrame: %1, m_pTransportPosition->getFrame() (old): %2, m_pTransportPosition->getFrameOffsetTempo(): %3, fNewTick: %4, m_pTransportPosition->getDoubleTick() (old): %5, m_pTransportPosition->getTickOffsetSongSize() : %6, tick offset (without rounding): %7, fNewSongSizeInTicks: %8, fRepetitions: %9")
	// 		.arg( nNewFrame )
	// 		.arg( m_pTransportPosition->getFrame() )
	// 		.arg( m_pTransportPosition->getFrameOffsetTempo() )
	// 		.arg( fNewTick, 0, 'g', 30 )
	// 		.arg( m_pTransportPosition->getDoubleTick(), 0, 'g', 30 )
	// 		.arg( m_pTransportPosition->getTickOffsetSongSize(), 0, 'g', 30 )
	// 		.arg( fNewTick - m_pTransportPosition->getDoubleTick(), 0, 'g', 30 )
	// 		.arg( fNewSongSizeInTicks, 0, 'g', 30 )
	// 		.arg( fRepetitions )
	// 		);

	const auto fOldTickSize = m_pTransportPosition->getTickSize();
	updateTransportPosition( fNewTick, nNewFrame, m_pTransportPosition );

	// Ensure the tick offset is calculated as well (we do not expect
	// the tempo to change).
	if ( fOldTickSize == m_pTransportPosition->getTickSize() ) {
		calculateTransportOffsetOnBpmChange( m_pTransportPosition );
	}
	
	// Updating the transport position by the same offset to keep them
	// approximately in sync.
	const double fNewTickQueuing = m_pQueuingPosition->getDoubleTick() +
		fTickOffset;
	const long long nNewFrameQueuing = TransportPosition::computeFrameFromTick(
		fNewTickQueuing, &m_pQueuingPosition->m_fTickMismatch );
	// Use offsets calculated above for the transport position.
	m_pQueuingPosition->set( m_pTransportPosition );
	updateTransportPosition( fNewTickQueuing, nNewFrameQueuing,
							 m_pQueuingPosition );

	updatePlayingPatterns();
	
#ifdef H2CORE_HAVE_DEBUG
	if ( nOldColumn != m_pTransportPosition->getColumn() ) {
		ERRORLOG( QString( "[nColumn mismatch] old: %1, new: %2" )
				  .arg( nOldColumn )
				  .arg( m_pTransportPosition->getColumn() ) );
	}
#endif

	if ( m_pQueuingPosition->getColumn() == -1 ) {
		endOfSongReached();
		return;
	}

	// WARNINGLOG( QString( "[After] fNewTick: %1, fRepetitions: %2, m_fSongSizeInTicks: %3, fNewSongSizeInTicks: %4, transport: %5, queuing: %6" )
	// 			.arg( fNewTick, 0, 'g', 30 )
	// 			.arg( fRepetitions, 0, 'f' )
	// 			.arg( m_fSongSizeInTicks )
	// 			.arg( fNewSongSizeInTicks )
	// 			.arg( m_pTransportPosition->toQString( "", true ) )
	// 			.arg( m_pQueuingPosition->toQString( "", true ) )
	// 			);
	
	EventQueue::get_instance()->push_event( EVENT_SONG_SIZE_CHANGED, 0 );
}

void AudioEngine::removePlayingPattern( Pattern* pPattern ) {
	auto removePattern = [&]( std::shared_ptr<TransportPosition> pPos ) {
		auto pPlayingPatterns = pPos->getPlayingPatterns();
		
		for ( int ii = 0; ii < pPlayingPatterns->size(); ++ii ) {
			if ( pPlayingPatterns->get( ii ) == pPattern ) {
				pPlayingPatterns->del( ii );
				break;
			}
		}
	};

	removePattern( m_pTransportPosition );
	removePattern( m_pQueuingPosition );
}

void AudioEngine::updatePlayingPatterns() {
	updatePlayingPatternsPos( m_pTransportPosition );
	updatePlayingPatternsPos( m_pQueuingPosition );
}
	
void AudioEngine::updatePlayingPatternsPos( std::shared_ptr<TransportPosition> pPos ) {
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	auto pPlayingPatterns = pPos->getPlayingPatterns();

	if ( pHydrogen->getMode() == Song::Mode::Song ) {

		pPlayingPatterns->clear();

		auto nColumn = std::max( pPos->getColumn(), 0 );
		if ( nColumn >= pSong->getPatternGroupVector()->size() ) {
			ERRORLOG( QString( "Provided column [%1] exceeds allowed range [0,%2]. Using 0 as fallback." )
					  .arg( nColumn ).arg( pSong->getPatternGroupVector()->size() - 1 ) );
			nColumn = 0;
		}

		for ( const auto& ppattern : *( *( pSong->getPatternGroupVector() ) )[ nColumn ] ) {
			if ( ppattern != nullptr ) {
				pPlayingPatterns->add( ppattern );
				ppattern->addFlattenedVirtualPatterns( pPlayingPatterns );
			}
		}

		if ( pPos == m_pTransportPosition ) {
			// GUI does not care about the internals of the audio
			// engine and just moves along the transport position.
			EventQueue::get_instance()->push_event( EVENT_PLAYING_PATTERNS_CHANGED, 0 );
		}
	}
	else if ( pHydrogen->getPatternMode() == Song::PatternMode::Selected ) {
		
		auto pSelectedPattern =
			pSong->getPatternList()->get( pHydrogen->getSelectedPatternNumber() );

		if ( pSelectedPattern != nullptr &&
			 ! ( pPlayingPatterns->size() == 1 &&
				 pPlayingPatterns->get( 0 ) == pSelectedPattern ) ) {
			pPlayingPatterns->clear();
			pPlayingPatterns->add( pSelectedPattern );
			pSelectedPattern->addFlattenedVirtualPatterns( pPlayingPatterns );

			if ( pPos == m_pTransportPosition ) {
				// GUI does not care about the internals of the audio
				// engine and just moves along the transport position.
				EventQueue::get_instance()->push_event( EVENT_PLAYING_PATTERNS_CHANGED, 0 );
			}
		}
	}
	else if ( pHydrogen->getPatternMode() == Song::PatternMode::Stacked ) {

		auto pNextPatterns = pPos->getNextPatterns();
		
		if ( pNextPatterns->size() > 0 ) {
			for ( const auto& ppattern : *pNextPatterns ) {
				if ( ppattern == nullptr ) {
					continue;
				}

				// If provided pattern is not part of the list, a
				// nullptr will be returned. Else, a pointer to the
				// deleted pattern will be returned.
				if ( ( pPlayingPatterns->del( ppattern ) ) == nullptr ) {
					// pPattern was not present yet. It will
					// be added.
					pPlayingPatterns->add( ppattern );
					ppattern->addFlattenedVirtualPatterns( pPlayingPatterns );
				} else {
					// pPattern was already present. It will
					// be deleted.
					ppattern->removeFlattenedVirtualPatterns( pPlayingPatterns );
				}

				if ( pPos == m_pTransportPosition ) {
					// GUI does not care about the internals of the audio
					// engine and just moves along the transport position.
					EventQueue::get_instance()->push_event( EVENT_PLAYING_PATTERNS_CHANGED, 0 );
				}
			}
			pNextPatterns->clear();
		}
	}

	if ( pPlayingPatterns->size() > 0 ) {
		pPos->setPatternSize( pPlayingPatterns->longest_pattern_length() );
	} else {
		pPos->setPatternSize( MAX_NOTES );
	}
}

void AudioEngine::toggleNextPattern( int nPatternNumber ) {
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	auto pPatternList = pSong->getPatternList();
	auto pPattern = pPatternList->get( nPatternNumber );
	if ( pPattern == nullptr ) {
		return;
	}
	
	if ( m_pTransportPosition->getNextPatterns()->del( pPattern ) == nullptr ) {
		m_pTransportPosition->getNextPatterns()->add( pPattern );
	}
	if ( m_pQueuingPosition->getNextPatterns()->del( pPattern ) == nullptr ) {
		m_pQueuingPosition->getNextPatterns()->add( pPattern );
	}
}

void AudioEngine::clearNextPatterns() {
	m_pTransportPosition->getNextPatterns()->clear();
	m_pQueuingPosition->getNextPatterns()->clear();
}

void AudioEngine::flushAndAddNextPattern( int nPatternNumber ) {
	auto pHydrogen = Hydrogen::get_instance();
	auto pSong = pHydrogen->getSong();
	auto pPatternList = pSong->getPatternList();

	bool bAlreadyPlaying = false;
	
	// Note: we will not perform a bound check on the provided pattern
	// number. This way the user can use the SELECT_ONLY_NEXT_PATTERN
	// MIDI or OSC command to flush all playing patterns.
	auto pRequestedPattern = pPatternList->get( nPatternNumber );

	auto flushAndAddNext = [&]( std::shared_ptr<TransportPosition> pPos ) {

		auto pNextPatterns = pPos->getNextPatterns();
		auto pPlayingPatterns = pPos->getPlayingPatterns();
		
		pNextPatterns->clear();
		for ( int ii = 0; ii < pPlayingPatterns->size(); ++ii ) {

			auto pPlayingPattern = pPlayingPatterns->get( ii );
			if ( pPlayingPattern != pRequestedPattern ) {
				pNextPatterns->add( pPlayingPattern );
			}
			else if ( pRequestedPattern != nullptr ) {
				bAlreadyPlaying = true;
			}
		}
	
		// Appending the requested pattern.
		if ( ! bAlreadyPlaying && pRequestedPattern != nullptr ) {
			pNextPatterns->add( pRequestedPattern );
		}
	};

	flushAndAddNext( m_pTransportPosition );
	flushAndAddNext( m_pQueuingPosition );
}

void AudioEngine::handleTimelineChange() {

	// INFOLOG( QString( "before:\n%1\n%2" )
	// 		 .arg( m_pTransportPosition->toQString() )
	// 		 .arg( m_pQueuingPosition->toQString() ) );

	const auto fOldTickSize = m_pTransportPosition->getTickSize();
	updateBpmAndTickSize( m_pTransportPosition );
	updateBpmAndTickSize( m_pQueuingPosition );
	
	if ( fOldTickSize == m_pTransportPosition->getTickSize() ) {
		// As tempo did not change during the Timeline activation, no
		// update of the offsets took place. This, however, is not
		// good, as it makes a significant difference to be located at
		// tick X with e.g. 120 bpm tempo and at X with a 120 bpm
		// tempo marker active but several others located prior to X. 
		calculateTransportOffsetOnBpmChange( m_pTransportPosition );
	}
	
	// INFOLOG( QString( "after:\n%1\n%2" )
	// 		 .arg( m_pTransportPosition->toQString() )
	// 		 .arg( m_pQueuingPosition->toQString() ) );
}

void AudioEngine::handleTempoChange() {
	if ( m_songNoteQueue.size() != 0 ) {

		std::vector<Note*> notes;
		for ( ; ! m_songNoteQueue.empty(); m_songNoteQueue.pop() ) {
			notes.push_back( m_songNoteQueue.top() );
		}

		// All notes share the same ticksize state (or things have gone
		// wrong at some point).
		for ( auto nnote : notes ) {
			nnote->computeNoteStart();
			m_songNoteQueue.push( nnote );
		}
	}
	
	getSampler()->handleTimelineOrTempoChange();
}

void AudioEngine::handleSongSizeChange() {
	if ( m_songNoteQueue.size() != 0 ) {

		std::vector<Note*> notes;
		for ( ; ! m_songNoteQueue.empty(); m_songNoteQueue.pop() ) {
			notes.push_back( m_songNoteQueue.top() );
		}

		const long nTickOffset =
			static_cast<long>(std::floor(m_pTransportPosition->getTickOffsetSongSize()));

		for ( auto nnote : notes ) {

			// DEBUGLOG( QString( "name: %1, pos: %2 -> %3, tick offset: %4, tick offset floored: %5" )
			// 		  .arg( nnote->get_instrument()->get_name() )
			// 		  .arg( nnote->get_position() )
			// 		  .arg( std::max( nnote->get_position() + nTickOffset,
			// 						  static_cast<long>(0) ) )
			// 		  .arg( m_pTransportPosition->getTickOffsetSongSize(), 0, 'f' )
			// 		  .arg( nTickOffset ) );
		
			nnote->set_position( std::max( nnote->get_position() + nTickOffset,
										   static_cast<long>(0) ) );
			nnote->computeNoteStart();
			m_songNoteQueue.push( nnote );
		}
	}
	
	getSampler()->handleSongSizeChange();
}

long long AudioEngine::computeTickInterval( double* fTickStart, double* fTickEnd, unsigned nIntervalLengthInFrames ) {

	const auto pHydrogen = Hydrogen::get_instance();
	const auto pTimeline = pHydrogen->getTimeline();

	long long nFrameStart, nFrameEnd;

	if ( getState() == State::Ready ) {
		// This case handles all realtime events, like MIDI input or
		// Hydrogen's virtual keyboard strokes, while playback is
		// stopped. We disregard tempo changes in the Timeline and
		// pretend the current tick size is valid for all future
		// notes.
		nFrameStart = getRealtimeFrame();
	} else {
		// Enters here both when transport is rolling or the unit
		// tests are run.
		nFrameStart = m_pTransportPosition->getFrame();
	}
	
	// We don't use the getLookaheadInFrames() function directly
	// because the lookahead contains both a frame-based and a
	// tick-based component and would be twice as expensive to
	// calculate using the mentioned call.
	long long nLeadLagFactor =
		getLeadLagInFrames( m_pTransportPosition->getDoubleTick() );

	// Due to rounding errors in tick<->frame conversions the leadlag
	// factor in frames can differ by +/-1 even if the corresponding
	// lead lag in ticks is exactly the same. This, however, would
	// result in small holes and overlaps in tick coverage for the
	// queuing position and note enqueuing in updateNoteQueue. That's
	// why we stick to a single lead lag factor invalidated each time
	// the tempo of the song does change.
    if ( m_nLastLeadLagFactor != 0 ) {
		if ( m_nLastLeadLagFactor != nLeadLagFactor ) {
			nLeadLagFactor = m_nLastLeadLagFactor;
			
			if ( std::abs( m_nLastLeadLagFactor - nLeadLagFactor ) > 1 ) {
				ERRORLOG( QString( "Difference between calculated lead lag factors is too large! m_nLastLeadLagFactor: %1, nLeadLagFactor: %2" )
						  .arg( m_nLastLeadLagFactor )
						  .arg( nLeadLagFactor ) );
			}
		}
	} else {
		m_nLastLeadLagFactor = nLeadLagFactor;
	}
	
	const long long nLookahead = nLeadLagFactor +
		AudioEngine::nMaxTimeHumanize + 1;

	nFrameEnd = nFrameStart + nLookahead +
		static_cast<long long>(nIntervalLengthInFrames);

	// Checking whether transport and queuing position are identical
	// is not enough in here. For specific audio driver parameters and
	// very tiny buffersizes used by drivers with dynamic buffer sizes
	// they both can be identical.
	if ( m_bLookaheadApplied ) {
		nFrameStart += nLookahead;
	}

	*fTickStart = TransportPosition::computeTickFromFrame( nFrameStart ) -
		m_pTransportPosition->getTickOffsetQueuing();
	*fTickEnd = TransportPosition::computeTickFromFrame( nFrameEnd ) -
		m_pTransportPosition->getTickOffsetQueuing();

	// INFOLOG( QString( "nFrameStart: %1, nFrameEnd: %2, fTickStart: %3, fTickEnd: %4, fTickStart (without offset): %5, fTickEnd (without offset): %6, m_pTransportPosition->getTickOffsetQueuing(): %7, nLookahead: %8, nIntervalLengthInFrames: %9, m_pTransportPosition->getFrame(): %10, m_pTransportPosition->getTickSize(): %11, m_pQueuingPosition->getFrame(): %12, m_bLookaheadApplied: %13" )
	// 		 .arg( nFrameStart )
	// 		 .arg( nFrameEnd )
	// 		 .arg( *fTickStart, 0, 'f' )
	// 		 .arg( *fTickEnd, 0, 'f' )
	// 		 .arg( TransportPosition::computeTickFromFrame( nFrameStart ), 0, 'f' )
	// 		 .arg( TransportPosition::computeTickFromFrame( nFrameEnd ), 0, 'f' )
	// 		 .arg( m_pTransportPosition->getTickOffsetQueuing(), 0, 'f' )
	// 		 .arg( nLookahead )
	// 		 .arg( nIntervalLengthInFrames )
	// 		 .arg( m_pTransportPosition->getFrame() )
	// 		 .arg( m_pTransportPosition->getTickSize(), 0, 'f' )
	// 		 .arg( m_pQueuingPosition->getFrame())
	// 		 .arg( m_bLookaheadApplied )
	// 		 );

	return nLeadLagFactor;
}

int AudioEngine::updateNoteQueue( unsigned nIntervalLengthInFrames )
{
	Hydrogen* pHydrogen = Hydrogen::get_instance();
	std::shared_ptr<Song> pSong = pHydrogen->getSong();

	double fTickStart, fTickEnd;

	long long nLeadLagFactor =
		computeTickInterval( &fTickStart, &fTickEnd, nIntervalLengthInFrames );

	// Get initial timestamp for first tick
	gettimeofday( &m_currentTickTime, nullptr );
		
	// MIDI events now get put into the `m_songNoteQueue` as well,
	// based on their timestamp (which is given in terms of its
	// transport position and not in terms of the date-time as above).
	while ( m_midiNoteQueue.size() > 0 ) {
		Note *pNote = m_midiNoteQueue[0];
		if ( pNote->get_position() >
			 static_cast<long long>(std::floor( fTickEnd )) ) {
			break;
		}

		m_midiNoteQueue.pop_front();
		pNote->get_instrument()->enqueue();
		pNote->computeNoteStart();
		m_songNoteQueue.push( pNote );
	}

	if ( getState() != State::Playing && getState() != State::Testing ) {
		// only keep going if we're playing
		return 0;
	}
	double fTickMismatch;

	AutomationPath* pAutomationPath = pSong->getVelocityAutomationPath();

	// computeTickInterval() is always called regardless whether
	// transport is rolling or not. But we mark the lookahead consumed
	// if the notes of the associated tick interval were actually
	// queued.
	if ( ! m_bLookaheadApplied ) {
		m_bLookaheadApplied = true;
	}

	// Only store the last tick interval end if transport is
	// rolling. Else the realtime frame processing will mess things
	// up.
	m_fLastTickEnd = fTickEnd;

	// WARNINGLOG( QString( "tick interval: [%1 : %2], m_pTransportPosition->getDoubleTick(): %3, m_pTransportPosition->getFrame(): %4, m_pQueuingPosition->getDoubleTick(): %5, m_pQueuingPosition->getFrame(): %6, nLeadLagFactor: %7")
	// 		  .arg( fTickStart, 0, 'f' ).arg( fTickEnd, 0, 'f' )
	// 		  .arg( m_pTransportPosition->getDoubleTick(), 0, 'f' )
	// 		  .arg( m_pTransportPosition->getFrame() )
	// 		  .arg( m_pQueuingPosition->getDoubleTick(), 0, 'f' )
	// 		  .arg( m_pQueuingPosition->getFrame() )
	// 		  .arg( nLeadLagFactor ) );

	// We loop over integer ticks to ensure that all notes encountered
	// between two iterations belong to the same pattern.
	for ( long nnTick = static_cast<long>(std::floor(fTickStart));
		  nnTick < static_cast<long>(std::floor(fTickEnd)); nnTick++ ) {

		//////////////////////////////////////////////////////////////
		// SONG MODE
		if ( pHydrogen->getMode() == Song::Mode::Song ) {
			if ( pSong->getPatternGroupVector()->size() == 0 ) {
				// there's no song!!
				ERRORLOG( "no patterns in song." );
				stop();
				return -1;
			}

			const long long nNewFrame = TransportPosition::computeFrameFromTick(
				static_cast<double>(nnTick),
				&m_pQueuingPosition->m_fTickMismatch );
			updateSongTransportPosition( static_cast<double>(nnTick),
										 nNewFrame, m_pQueuingPosition );

			// If no pattern list could not be found, either choose
			// the first one if loop mode is activate or the
			// function returns indicating that the end of the song is
			// reached.
			if ( m_pQueuingPosition->getColumn() == -1 ||
				 ( ( pSong->getLoopMode() == Song::LoopMode::Finishing ) &&
				   ( m_pTransportPosition->getColumn() > 0 ) &&
				   ( m_pQueuingPosition->getColumn() <
					 m_pTransportPosition->getColumn() ) ) ) {
				INFOLOG( QString( "End of Song\n%1\n%2" )
						 .arg( m_pQueuingPosition->toQString() )
						 .arg( m_pTransportPosition->toQString() ) );

				if( pHydrogen->getMidiOutput() != nullptr ){
					pHydrogen->getMidiOutput()->handleQueueAllNoteOff();
				}

				return -1;
			}
		}
		
		//////////////////////////////////////////////////////////////
		// PATTERN MODE
		else if ( pHydrogen->getMode() == Song::Mode::Pattern )	{

			const long long nNewFrame = TransportPosition::computeFrameFromTick(
				static_cast<double>(nnTick),
				&m_pQueuingPosition->m_fTickMismatch );
			updatePatternTransportPosition( static_cast<double>(nnTick),
											nNewFrame, m_pQueuingPosition );
		}
		
		//////////////////////////////////////////////////////////////
		// Metronome
		// Only trigger the metronome at a predefined rate.
		if ( m_pQueuingPosition->getPatternTickPosition() % 48 == 0 ) {
			float fPitch;
			float fVelocity;
			
			// Depending on whether the metronome beat will be issued
			// at the beginning or in the remainder of the pattern,
			// two different sounds and events will be used.
			if ( m_pQueuingPosition->getPatternTickPosition() == 0 ) {
				fPitch = 3;
				fVelocity = 1.0;
				EventQueue::get_instance()->push_event( EVENT_METRONOME, 1 );
			} else {
				fPitch = 0;
				fVelocity = 0.8;
				EventQueue::get_instance()->push_event( EVENT_METRONOME, 0 );
			}
			
			// Only trigger the sounds if the user enabled the
			// metronome. 
			if ( Preferences::get_instance()->m_bUseMetronome ) {
				m_pMetronomeInstrument->set_volume(
							Preferences::get_instance()->m_fMetronomeVolume
							);
				Note *pMetronomeNote = new Note( m_pMetronomeInstrument,
												 nnTick,
												 fVelocity,
												 0.f, // pan
												 -1,
												 fPitch
												 );
				m_pMetronomeInstrument->enqueue();
				pMetronomeNote->computeNoteStart();
				m_songNoteQueue.push( pMetronomeNote );
			}
		}

		//////////////////////////////////////////////////////////////
		// Update the notes queue.
		//
		// Supporting ticks with float precision:
		// - make FOREACH_NOTE_CST_IT_BOUND loop over all notes
		// `(_it)->first >= (_bound) && (_it)->first < (_bound + 1)`
		// - add remainder of pNote->get_position() % 1 when setting
		// nnTick as new position.
		//
		const auto pPlayingPatterns = m_pQueuingPosition->getPlayingPatterns();
		if ( pPlayingPatterns->size() != 0 ) {
			for ( auto nPat = 0; nPat < pPlayingPatterns->size(); ++nPat ) {
				Pattern *pPattern = pPlayingPatterns->get( nPat );
				assert( pPattern != nullptr );
				Pattern::notes_t* notes = (Pattern::notes_t*)pPattern->get_notes();

				// Loop over all notes at tick nPatternTickPosition
				// (associated tick is determined by Note::__position
				// at the time of insertion into the Pattern).
				FOREACH_NOTE_CST_IT_BOUND(notes, it,
										  m_pQueuingPosition->getPatternTickPosition()) {
					Note *pNote = it->second;
					if ( pNote != nullptr ) {
						pNote->set_just_recorded( false );
						
						/** Time Offset in frames (relative to sample rate)
						*	Sum of 3 components: swing, humanized timing, lead_lag
						*/
						int nOffset = 0;

					   /** Swing 16ths //
						* delay the upbeat 16th-notes by a constant (manual) offset
						*/
						if ( ( ( m_pQueuingPosition->getPatternTickPosition() %
								 ( MAX_NOTES / 16 ) ) == 0 ) &&
							 ( ( m_pQueuingPosition->getPatternTickPosition() %
								 ( MAX_NOTES / 8 ) ) != 0 ) &&
							 pSong->getSwingFactor() > 0 ) {
							/* TODO: incorporate the factor MAX_NOTES / 32. either in Song::m_fSwingFactor
							* or make it a member variable.
							* comment by oddtime:
							* 32 depends on the fact that the swing is applied to the upbeat 16th-notes.
							* (not to upbeat 8th-notes as in jazz swing!).
							* however 32 could be changed but must be >16, otherwise the max delay is too long and
							* the swing note could be played after the next downbeat!
							*/
							// If the Timeline is activated, the tick
							// size may change at any
							// point. Therefore, the length in frames
							// of a 16-th note offset has to be
							// calculated for a particular transport
							// position and is not generally applicable.
							nOffset +=
								TransportPosition::computeFrameFromTick( nnTick + MAX_NOTES / 32.,
																		 &fTickMismatch ) *
								pSong->getSwingFactor() -
								TransportPosition::computeFrameFromTick( nnTick, &fTickMismatch );
						}

						/* Humanize - Time parameter //
						* Add a random offset to each note. Due to
						* the nature of the Gaussian distribution,
						* the factor Song::__humanize_time_value will
						* also scale the variance of the generated
						* random variable.
						*/
						if ( pSong->getHumanizeTimeValue() != 0 ) {
							nOffset += ( int )(
										getGaussian( 0.3 )
										* pSong->getHumanizeTimeValue()
										* AudioEngine::nMaxTimeHumanize
										);
						}

						// Lead or Lag - timing parameter //
						// Add a constant offset to all notes.
						nOffset += (int) ( pNote->get_lead_lag() * nLeadLagFactor );

						// Lower bound of the offset. No note is
						// allowed to start prior to the beginning of
						// the song.
						if( m_pQueuingPosition->getFrame() + nOffset < 0 ){
							nOffset = -1 * m_pQueuingPosition->getFrame();
						}

						if ( nOffset > AudioEngine::nMaxTimeHumanize ) {
							nOffset = AudioEngine::nMaxTimeHumanize;
						} else if ( nOffset < -1 * AudioEngine::nMaxTimeHumanize ) {
							nOffset = -AudioEngine::nMaxTimeHumanize;
						}
						
						// Generate a copy of the current note, assign
						// it the new offset, and push it to the list
						// of all notes, which are about to be played
						// back.
						//
						// Why a copy? because it has the new offset
						// (including swing and random timing) in its
						// humanized delay, and tick position is
						// expressed referring to start time (and not
						// pattern).
						Note *pCopiedNote = new Note( pNote );
						pCopiedNote->set_humanize_delay( nOffset );
						
						pCopiedNote->set_position( nnTick );
						// Important: this call has to be done _after_
						// setting the position and the humanize_delay.
						pCopiedNote->computeNoteStart();

						// DEBUGLOG( QString( "m_pQueuingPosition->getDoubleTick(): %1, m_pQueuingPosition->getFrame(): %2, m_pQueuingPosition->getColumn(): %3, original note position: %4, nOffset: %5" )
						// 		  .arg( m_pQueuingPosition->getDoubleTick() )
						// 		  .arg( m_pQueuingPosition->getFrame() )
						// 		  .arg( m_pQueuingPosition->getColumn() )
						// 		  .arg( pNote->get_position() )
						// 		  .arg( nOffset )
						// 		  .append( pCopiedNote->toQString("", true ) ) );
						
						if ( pHydrogen->getMode() == Song::Mode::Song ) {
							const float fPos = static_cast<float>( m_pQueuingPosition->getColumn() ) +
								pCopiedNote->get_position() % 192 / 192.f;
							pCopiedNote->set_velocity( pNote->get_velocity() *
													   pAutomationPath->get_value( fPos ) );
						}
						pNote->get_instrument()->enqueue();
						m_songNoteQueue.push( pCopiedNote );
					}
				}
			}
		}
	}

	return 0;
}

void AudioEngine::noteOn( Note *note )
{
	// check current state
	if ( ! ( getState() == State::Playing ||
			 getState() == State::Ready ||
			 getState() == State::Testing ) ) {
		ERRORLOG( QString( "Error the audio engine is not in State::Ready, State::Playing, or State::Testing but [%1]" )
					 .arg( static_cast<int>( getState() ) ) );
		delete note;
		return;
	}

	m_midiNoteQueue.push_back( note );
}

bool AudioEngine::compare_pNotes::operator()(Note* pNote1, Note* pNote2)
{
	float fTickSize = Hydrogen::get_instance()->getAudioEngine()->
		getTransportPosition()->getTickSize();
	return (pNote1->get_humanize_delay() +
			TransportPosition::computeFrame( pNote1->get_position(), fTickSize ) ) >
		(pNote2->get_humanize_delay() +
		 TransportPosition::computeFrame( pNote2->get_position(), fTickSize ) );
}

void AudioEngine::play() {
	
	assert( m_pAudioDriver );

#ifdef H2CORE_HAVE_JACK
	if ( Hydrogen::get_instance()->hasJackTransport() ) {
		// Tell all other JACK clients to start as well and wait for
		// the JACK server to give the signal.
		static_cast<JackAudioDriver*>( m_pAudioDriver )->startTransport();
		return;
	}
#endif

	setNextState( State::Playing );

	if ( dynamic_cast<FakeDriver*>(m_pAudioDriver) != nullptr ) {
		static_cast<FakeDriver*>( m_pAudioDriver )->processCallback();
	}
}

void AudioEngine::stop() {
	assert( m_pAudioDriver );
	
#ifdef H2CORE_HAVE_JACK
	if ( Hydrogen::get_instance()->hasJackTransport() ) {
		// Tell all other JACK clients to stop as well and wait for
		// the JACK server to give the signal.
		static_cast<JackAudioDriver*>( m_pAudioDriver )->stopTransport();
		return;
	}
#endif
	
	setNextState( State::Ready );
}

double AudioEngine::getLeadLagInTicks() {
	return 5;
}

long long AudioEngine::getLeadLagInFrames( double fTick ) {
	double fTmp;
	const long long nFrameStart =
		TransportPosition::computeFrameFromTick( fTick, &fTmp );
	const long long nFrameEnd =
		TransportPosition::computeFrameFromTick( fTick +
												 AudioEngine::getLeadLagInTicks(),
												 &fTmp );

	// WARNINGLOG( QString( "nFrameStart: %1, nFrameEnd: %2, diff: %3" )
	// 			.arg( nFrameStart )
	// 			.arg( nFrameEnd )
	// 			.arg( nFrameEnd - nFrameStart )
	// 			);

	return nFrameEnd - nFrameStart;
}

long long AudioEngine::getLookaheadInFrames( double fTick ) {
	return getLeadLagInFrames( fTick ) +
		AudioEngine::nMaxTimeHumanize + 1;
}

const PatternList* AudioEngine::getPlayingPatterns() const {
	if ( m_pTransportPosition != nullptr ) {
		return m_pTransportPosition->getPlayingPatterns();
	}
	return nullptr;
}

const PatternList* AudioEngine::getNextPatterns() const {
	if ( m_pTransportPosition != nullptr ) {
		return m_pTransportPosition->getNextPatterns();
	}
	return nullptr;
}

QString AudioEngine::toQString( const QString& sPrefix, bool bShort ) const {
	QString s = Base::sPrintIndention;
	QString sOutput;
	if ( ! bShort ) {
		sOutput = QString( "%1[AudioEngine]\n" ).arg( sPrefix )
			.append( "%1%2m_pTransportPosition:\n").arg( sPrefix ).arg( s );
		if ( m_pTransportPosition != nullptr ) {
			sOutput.append( QString( "%1" )
							.arg( m_pTransportPosition->toQString( sPrefix + s, bShort ) ) );
		} else {
			sOutput.append( QString( "nullptr\n" ) );
		}
		sOutput.append( QString( "%1%2m_pQueuingPosition:\n").arg( sPrefix ).arg( s ) );
		if ( m_pQueuingPosition != nullptr ) {
			sOutput.append( QString( "%1" )
							.arg( m_pQueuingPosition->toQString( sPrefix + s, bShort ) ) );
		} else {
			sOutput.append( QString( "nullptr\n" ) );
		}
		sOutput.append( QString( "%1%2m_fNextBpm: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fNextBpm, 0, 'f' ) )
			.append( QString( "%1%2m_state: %3\n" ).arg( sPrefix ).arg( s ).arg( static_cast<int>(m_state) ) )
			.append( QString( "%1%2m_nextState: %3\n" ).arg( sPrefix ).arg( s ).arg( static_cast<int>(m_nextState) ) )
			.append( QString( "%1%2m_currentTickTime: %3 ms\n" ).arg( sPrefix ).arg( s ).arg( m_currentTickTime.tv_sec * 1000 + m_currentTickTime.tv_usec / 1000) )
			.append( QString( "%1%2m_fSongSizeInTicks: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fSongSizeInTicks, 0, 'f' ) )
			.append( QString( "%1%2m_fLastTickEnd: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fLastTickEnd, 0, 'f' ) )
			.append( QString( "%1%2m_nLastLeadLagFactor: %3\n" ).arg( sPrefix ).arg( s ).arg( m_nLastLeadLagFactor ) )
			.append( QString( "%1%2m_pSampler: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pSynth: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pAudioDriver: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pMidiDriver: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pMidiDriverOut: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_pEventQueue: \n" ).arg( sPrefix ).arg( s ) );
#ifdef H2CORE_HAVE_LADSPA
		sOutput.append( QString( "%1%2m_fFXPeak_L: [" ).arg( sPrefix ).arg( s ) );
		for ( auto ii : m_fFXPeak_L ) {
			sOutput.append( QString( " %1" ).arg( ii ) );
		}
		sOutput.append( QString( "]\n%1%2m_fFXPeak_R: [" ).arg( sPrefix ).arg( s ) );
		for ( auto ii : m_fFXPeak_R ) {
			sOutput.append( QString( " %1" ).arg( ii ) );
		}
		sOutput.append( QString( " ]\n" ) );
#endif
		sOutput.append( QString( "%1%2m_fMasterPeak_L: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fMasterPeak_L ) )
			.append( QString( "%1%2m_fMasterPeak_R: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fMasterPeak_R ) )
			.append( QString( "%1%2m_fProcessTime: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fProcessTime ) )
			.append( QString( "%1%2m_fMaxProcessTime: %3\n" ).arg( sPrefix ).arg( s ).arg( m_fMaxProcessTime ) )
			.append( QString( "%1%2m_nRealtimeFrame: %3\n" ).arg( sPrefix ).arg( s ).arg( m_nRealtimeFrame ) )
			.append( QString( "%1%2m_AudioProcessCallback: \n" ).arg( sPrefix ).arg( s ) )
			.append( QString( "%1%2m_songNoteQueue: length = %3\n" ).arg( sPrefix ).arg( s ).arg( m_songNoteQueue.size() ) );
		sOutput.append( QString( "%1%2m_midiNoteQueue: [\n" ).arg( sPrefix ).arg( s ) );
		for ( const auto& nn : m_midiNoteQueue ) {
			sOutput.append( nn->toQString( sPrefix + s, bShort ) );
		}
		sOutput.append( QString( "]\n%1%2m_pMetronomeInstrument: %3\n" ).arg( sPrefix ).arg( s ).arg( m_pMetronomeInstrument->toQString( sPrefix + s, bShort ) ) )
			.append( QString( "%1%2nMaxTimeHumanize: %3\n" ).arg( sPrefix ).arg( s ).arg( AudioEngine::nMaxTimeHumanize ) );
		
	}
	else {
		sOutput = QString( "%1[AudioEngine]" ).arg( sPrefix )
			.append( ", m_pTransportPosition:\n");
		if ( m_pTransportPosition != nullptr ) {
			sOutput.append( QString( "%1" )
							.arg( m_pTransportPosition->toQString( sPrefix, bShort ) ) );
		} else {
			sOutput.append( QString( "nullptr\n" ) );
		}
		sOutput.append( ", m_pQueuingPosition:\n");
		if ( m_pQueuingPosition != nullptr ) {
			sOutput.append( QString( "%1" )
							.arg( m_pQueuingPosition->toQString( sPrefix, bShort ) ) );
		} else {
			sOutput.append( QString( "nullptr\n" ) );
		}
		sOutput.append( QString( ", m_fNextBpm: %1" ).arg( m_fNextBpm, 0, 'f' ) )
			.append( QString( ", m_state: %1" ).arg( static_cast<int>(m_state) ) )
			.append( QString( ", m_nextState: %1" ).arg( static_cast<int>(m_nextState) ) )
			.append( QString( ", m_currentTickTime: %1 ms" ).arg( m_currentTickTime.tv_sec * 1000 + m_currentTickTime.tv_usec / 1000) )
			.append( QString( ", m_fSongSizeInTicks: %1" ).arg( m_fSongSizeInTicks, 0, 'f' ) )
			.append( QString( ", m_fLastTickEnd: %1" ).arg( m_fLastTickEnd, 0, 'f' ) )
			.append( QString( ", m_nLastLeadLagFactor: %1" ).arg( m_nLastLeadLagFactor ) )
			.append( QString( ", m_pSampler:" ) )
			.append( QString( ", m_pSynth:" ) )
			.append( QString( ", m_pAudioDriver:" ) )
			.append( QString( ", m_pMidiDriver:" ) )
			.append( QString( ", m_pMidiDriverOut:" ) )
			.append( QString( ", m_pEventQueue:" ) );
#ifdef H2CORE_HAVE_LADSPA
		sOutput.append( QString( ", m_fFXPeak_L: [" ) );
		for ( auto ii : m_fFXPeak_L ) {
			sOutput.append( QString( " %1" ).arg( ii ) );
		}
		sOutput.append( QString( "], m_fFXPeak_R: [" ) );
		for ( auto ii : m_fFXPeak_R ) {
			sOutput.append( QString( " %1" ).arg( ii ) );
		}
		sOutput.append( QString( " ]" ) );
#endif
		sOutput.append( QString( ", m_fMasterPeak_L: %1" ).arg( m_fMasterPeak_L ) )
			.append( QString( ", m_fMasterPeak_R: %1" ).arg( m_fMasterPeak_R ) )
			.append( QString( ", m_fProcessTime: %1" ).arg( m_fProcessTime ) )
			.append( QString( ", m_fMaxProcessTime: %1" ).arg( m_fMaxProcessTime ) )
			.append( QString( ", m_nRealtimeFrame: %1" ).arg( m_nRealtimeFrame ) )
			.append( QString( ", m_AudioProcessCallback:" ) )
			.append( QString( ", m_songNoteQueue: length = %1" ).arg( m_songNoteQueue.size() ) );
		sOutput.append( QString( ", m_midiNoteQueue: [" ) );
		for ( const auto& nn : m_midiNoteQueue ) {
			sOutput.append( nn->toQString( sPrefix + s, bShort ) );
		}
		sOutput.append( QString( "], m_pMetronomeInstrument: id = %1" ).arg( m_pMetronomeInstrument->get_id() ) )
			.append( QString( ", nMaxTimeHumanize: id %1" ).arg( AudioEngine::nMaxTimeHumanize ) );
	}
	
	return sOutput;
}

void AudioEngineLocking::assertAudioEngineLocked() const 
{
#ifndef NDEBUG
		if ( m_bNeedsLock ) {
			H2Core::Hydrogen::get_instance()->getAudioEngine()->assertLocked();
		}
#endif
}

}; // namespace H2Core
