/*
 * Hydrogen
 * Copyright(c) 2002-2008 by Alex >Comix< Cominu [comix@users.sourceforge.net]
 * Copyright(c) 2008-2024 The hydrogen development team [hydrogen-devel@lists.sourceforge.net]
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

#include <core/IO/MidiInput.h>
#include <core/EventQueue.h>
#include <core/Preferences/Preferences.h>
#include <core/CoreActionController.h>
#include <core/Hydrogen.h>
#include <core/Basics/Drumkit.h>
#include <core/Basics/Instrument.h>
#include <core/Basics/InstrumentList.h>
#include <core/Basics/Note.h>
#include <core/MidiAction.h>
#include <core/AudioEngine/AudioEngine.h>
#include <core/MidiMap.h>

namespace H2Core
{

MidiInput::MidiInput()
		: m_bActive( false )
		, m_nHihatOpenness ( 127 )
{
	//

}


MidiInput::~MidiInput()
{
	//INFOLOG( "DESTROY" );
}

void MidiInput::handleMidiMessage( const MidiMessage& msg )
{
		EventQueue::get_instance()->push_event( EVENT_MIDI_ACTIVITY, -1 );

		INFOLOG( QString( "Incoming message:  [%1]" ).arg( msg.toQString() ) );

		// midi channel filter for all messages
		bool bIsChannelValid = true;
		Preferences* pPref = Preferences::get_instance();
		if ( pPref->m_nMidiChannelFilter != -1
		  && pPref->m_nMidiChannelFilter != msg.m_nChannel
		) {
			bIsChannelValid = false;
		}

		// exclude all midi channel filter independent messages
		int type = msg.m_type;
		if (  MidiMessage::SYSEX == type
		   || MidiMessage::START == type
		   || MidiMessage::CONTINUE == type
		   || MidiMessage::STOP == type
		   || MidiMessage::SONG_POS == type
		   || MidiMessage::QUARTER_FRAME == type
		) {
			bIsChannelValid = true;
		}

		if ( !bIsChannelValid) {
			return;
		}

		Hydrogen* pHydrogen = Hydrogen::get_instance();
		auto pAudioEngine = pHydrogen->getAudioEngine();
		if ( ! pHydrogen->getSong() ) {
			ERRORLOG( "No song loaded, skipping note" );
			return;
		}

		switch ( type ) {
		case MidiMessage::SYSEX:
				handleSysexMessage( msg );
				break;

		case MidiMessage::NOTE_ON:
				handleNoteOnMessage( msg );
				break;

		case MidiMessage::NOTE_OFF:
				handleNoteOffMessage( msg, false );
				break;

		case MidiMessage::POLYPHONIC_KEY_PRESSURE:
				handlePolyphonicKeyPressureMessage( msg );
				break;

		case MidiMessage::CONTROL_CHANGE:
				handleControlChangeMessage( msg );
				break;

		case MidiMessage::PROGRAM_CHANGE:
				handleProgramChangeMessage( msg );
				break;

		case MidiMessage::START: /* Start from position 0 */
			if ( pAudioEngine->getState() != AudioEngine::State::Playing ) {
				CoreActionController::locateToColumn( 0 );
				auto pAction = std::make_shared<Action>("PLAY");
				MidiActionManager::get_instance()->handleAction( pAction );
			}
			break;

		case MidiMessage::CONTINUE: /* Just start */ {
			auto pAction = std::make_shared<Action>("PLAY");
			MidiActionManager::get_instance()->handleAction( pAction );
			break;
		}

		case MidiMessage::STOP: /* Stop in current position i.e. Pause */ {
			auto pAction = std::make_shared<Action>("PAUSE");
			MidiActionManager::get_instance()->handleAction( pAction );
			break;
		}

		case MidiMessage::CHANNEL_PRESSURE:
		case MidiMessage::PITCH_WHEEL:
		case MidiMessage::SONG_POS:
		case MidiMessage::QUARTER_FRAME:
		case MidiMessage::SONG_SELECT:
		case MidiMessage::TUNE_REQUEST:
		case MidiMessage::TIMING_CLOCK:
		case MidiMessage::ACTIVE_SENSING:
		case MidiMessage::RESET:
			ERRORLOG( QString( "MIDI message of type [%1] is not supported by Hydrogen" )
					  .arg( MidiMessage::TypeToQString( msg.m_type ) ) );
			break;

		case MidiMessage::UNKNOWN:
			ERRORLOG( "Unknown midi message" );
			break;

		default:
			ERRORLOG( QString( "unhandled midi message type: %1 (%2)" )
					  .arg( static_cast<int>( msg.m_type ) )
					  .arg( MidiMessage::TypeToQString( msg.m_type ) ) );
		}

		// Two spaces after "msg." in a row to align message parameters
		INFOLOG( QString( "DONE handling msg: [%1]" ).arg( msg.toQString() ) );
}

void MidiInput::handleControlChangeMessage( const MidiMessage& msg )
{
	//INFOLOG( QString( "[handleMidiMessage] CONTROL_CHANGE Parameter: %1, Value: %2" ).arg( msg.m_nData1 ).arg( msg.m_nData2 ) );
	Hydrogen *pHydrogen = Hydrogen::get_instance();
	MidiActionManager *pMidiActionManager = MidiActionManager::get_instance();
	MidiMap *pMidiMap = MidiMap::get_instance();

	for ( const auto& ppAction : pMidiMap->getCCActions( msg.m_nData1 ) ) {
		if ( ppAction != nullptr && ! ppAction->isNull() ) {
			auto pNewAction = std::make_shared<Action>( ppAction );
			pNewAction->setValue( QString::number( msg.m_nData2 ) );
			pMidiActionManager->handleAction( pNewAction );
		}
	}

	if ( msg.m_nData1 == 04 ) {
		m_nHihatOpenness = msg.m_nData2;
	}

	pHydrogen->setLastMidiEvent( MidiMessage::Event::CC );
	pHydrogen->setLastMidiEventParameter( msg.m_nData1 );
}

void MidiInput::handleProgramChangeMessage( const MidiMessage& msg )
{
	Hydrogen *pHydrogen = Hydrogen::get_instance();
	MidiActionManager *pMidiActionManager = MidiActionManager::get_instance();
	MidiMap *pMidiMap = MidiMap::get_instance();

	for ( const auto& ppAction : pMidiMap->getPCActions() ) {
		if ( ppAction != nullptr && ! ppAction->isNull() ) {
			auto pNewAction = std::make_shared<Action>( ppAction );
			pNewAction->setValue( QString::number( msg.m_nData1 ) );
			pMidiActionManager->handleAction( pNewAction );
		}
	}

	pHydrogen->setLastMidiEvent( MidiMessage::Event::PC );
	pHydrogen->setLastMidiEventParameter( 0 );
}

void MidiInput::handleNoteOnMessage( const MidiMessage& msg )
{
//	INFOLOG( "handleNoteOnMessage" );

	const int nNote = msg.m_nData1;
	float fVelocity = msg.m_nData2 / 127.0;

	if ( fVelocity == 0 ) {
		handleNoteOffMessage( msg, false );
		return;
	}

	MidiActionManager * pMidiActionManager = MidiActionManager::get_instance();
	MidiMap * pMidiMap = MidiMap::get_instance();
	Hydrogen *pHydrogen = Hydrogen::get_instance();
	auto pPref = Preferences::get_instance();

	pHydrogen->setLastMidiEvent( MidiMessage::Event::Note );
	pHydrogen->setLastMidiEventParameter( msg.m_nData1 );

	bool bActionSuccess = false;
	for ( const auto& ppAction : pMidiMap->getNoteActions( msg.m_nData1 ) ) {
		if ( ppAction != nullptr && ! ppAction->isNull() ) {
			auto pNewAction = std::make_shared<Action>( ppAction );
			pNewAction->setValue( QString::number( msg.m_nData2 ) );
			if ( pMidiActionManager->handleAction( pNewAction ) ) {
				bActionSuccess = true;
			}
		}
	}

	if ( bActionSuccess && pPref->m_bMidiDiscardNoteAfterAction ) {
		return;
	}


	CoreActionController::handleNote( nNote, fVelocity );
}

/*
	EDrums (at least Roland TD-6V) uses PolyphonicKeyPressure
	for cymbal choke.
	If the message is 127 (choked) we send a NoteOff
*/
void MidiInput::handlePolyphonicKeyPressureMessage( const MidiMessage& msg )
{
	if( msg.m_nData2 == 127 ) {
		handleNoteOffMessage( msg, true );
	}
}

void MidiInput::handleNoteOffMessage( const MidiMessage& msg, bool CymbalChoke )
{
//	INFOLOG( "handleNoteOffMessage" );
	if ( !CymbalChoke && Preferences::get_instance()->m_bMidiNoteOffIgnore ) {
		return;
	}

	Hydrogen *pHydrogen = Hydrogen::get_instance();
	auto pInstrList = pHydrogen->getSong()->getDrumkit()->getInstruments();

	int nNote = msg.m_nData1;
	int nInstrument = nNote - MidiMessage::instrumentOffset;
	std::shared_ptr<Instrument> pInstr = nullptr;

	if ( Preferences::get_instance()->__playselectedinstrument ){
		nInstrument = pHydrogen->getSelectedInstrumentNumber();
		pInstr = pInstrList->get( pHydrogen->getSelectedInstrumentNumber());
	}
	else if( Preferences::get_instance()->m_bMidiFixedMapping ) {
		pInstr = pInstrList->findMidiNote( nNote );
		nInstrument = pInstrList->index( pInstr );
	}
	else {
		if( nInstrument < 0 || nInstrument >= pInstrList->size()) {
			WARNINGLOG( QString( "Instrument number [%1] - derived from note [%2] - out of bound note [%3,%4]" )
						.arg( nInstrument ).arg( nNote )
						.arg( 0 ).arg( pInstrList->size() ) );
			return;
		}
		pInstr = pInstrList->get( nInstrument );
	}

	if( pInstr == nullptr ) {
		WARNINGLOG( QString( "Can't find corresponding Instrument for note %1" ).arg( nNote ));
		return;
	}

	Hydrogen::get_instance()->addRealtimeNote( nInstrument, 0.0, true, nNote );
}

void MidiInput::handleSysexMessage( const MidiMessage& msg )
{

	/*
		General MMC message
		0	1	2	3	4	5
		F0	7F	id	6	cmd	247

		cmd:
		1	stop
		2	play
		3	Deferred play
		4	Fast Forward
		5	Rewind
		6	Record strobe (punch in)
		7	Record exit (punch out)
		8	Record ready
		9	Pause


		Goto MMC message
		0	1	2	3	4	5	6	7	8	9	10	11	12
		240	127	id	6	68	6	1	hr	mn	sc	fr	ff	247
	*/


	MidiActionManager * pMidiActionManager = MidiActionManager::get_instance();
	MidiMap * pMidiMap = MidiMap::get_instance();
	Hydrogen *pHydrogen = Hydrogen::get_instance();


	if ( msg.m_sysexData.size() == 6 && 
		 msg.m_sysexData[ 1 ] == 127 && msg.m_sysexData[ 3 ] == 6 ) {
		// MIDI Machine Control (MMC) message

		MidiMessage::Event event = MidiMessage::Event::Null;
		QString sMMCtype;
		switch ( msg.m_sysexData[4] ) {
		case 1:	// STOP
			event = MidiMessage::Event::MmcStop;
			break;

		case 2:	// PLAY
			event = MidiMessage::Event::MmcPlay;
			break;

		case 3:	//DEFERRED PLAY
			event = MidiMessage::Event::MmcDeferredPlay;
			break;

		case 4:	// FAST FWD
			event = MidiMessage::Event::MmcFastForward;
			break;

		case 5:	// REWIND
			event = MidiMessage::Event::MmcRewind;
			break;

		case 6:	// RECORD STROBE (PUNCH IN)
			event = MidiMessage::Event::MmcRecordStrobe;
			break;

		case 7:	// RECORD EXIT (PUNCH OUT)
			event = MidiMessage::Event::MmcRecordExit;
			break;

		case 8:	// RECORD READY
			event = MidiMessage::Event::MmcRecordReady;
			break;

		case 9:	//PAUSE
			event = MidiMessage::Event::MmcPause;
			break;
		}

		if ( event != MidiMessage::Event::Null ) {
			const QString sMMCtype = MidiMessage::EventToQString( event );
			INFOLOG( QString( "MIDI Machine Control command: [%1]" )
					 .arg( sMMCtype ) );
			
			pHydrogen->setLastMidiEvent( event );
			pHydrogen->setLastMidiEventParameter( msg.m_nData1 );
			
			pMidiActionManager->handleActions( pMidiMap->getMMCActions( sMMCtype ) );
		}
		else {
			WARNINGLOG( "Unknown MIDI Machine Control (MMC) Command" );
		}
	}
	else if ( msg.m_sysexData.size() == 13 && 
			  msg.m_sysexData[ 1 ] == 127 && msg.m_sysexData[ 3 ] == 68 ) {
		WARNINGLOG( "MMC GOTO Message not implemented yet" );
		// int hr = msg.m_sysexData[7];
		// int mn = msg.m_sysexData[8];
		// int sc = msg.m_sysexData[9];
		// int fr = msg.m_sysexData[10];
		// int ff = msg.m_sysexData[11];
		// char tmp[200];
		// sprintf( tmp, "[handleSysexMessage] GOTO %d:%d:%d:%d:%d", hr, mn, sc, fr, ff );
		// INFOLOG( tmp );
	}
	else {
		WARNINGLOG( QString( "Unsupported SysEx message: [%1]" )
					.arg( msg.toQString() ) );
	}
}

};
