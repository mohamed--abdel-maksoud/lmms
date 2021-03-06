/*
 * song.cpp - root of the model tree
 *
 * Copyright (c) 2004-2014 Tobias Doerffel <tobydox/at/users.sourceforge.net>
 *
 * This file is part of LMMS - http://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "Song.h"
#include <QTextStream>
#include <QCoreApplication>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QApplication>

#include <math.h>

#include "AutomationTrack.h"
#include "AutomationEditor.h"
#include "BBEditor.h"
#include "BBTrack.h"
#include "BBTrackContainer.h"
#include "ConfigManager.h"
#include "ControllerRackView.h"
#include "ControllerConnection.h"
#include "embed.h"
#include "EnvelopeAndLfoParameters.h"
#include "ExportProjectDialog.h"
#include "FxMixer.h"
#include "FxMixerView.h"
#include "GuiApplication.h"
#include "ImportFilter.h"
#include "ExportFilter.h"
#include "InstrumentTrack.h"
#include "MainWindow.h"
#include "FileDialog.h"
#include "MidiClient.h"
#include "DataFile.h"
#include "NotePlayHandle.h"
#include "Pattern.h"
#include "PianoRoll.h"
#include "ProjectJournal.h"
#include "ProjectNotes.h"
#include "ProjectRenderer.h"
#include "RenameDialog.h"
#include "SongEditor.h"
#include "templates.h"
#include "TextFloat.h"
#include "TimeLineWidget.h"
#include "PeakController.h"


tick_t MidiTime::s_ticksPerTact = DefaultTicksPerTact;



Song::Song() :
	TrackContainer(),
	m_globalAutomationTrack( dynamic_cast<AutomationTrack *>(
				Track::create( Track::HiddenAutomationTrack,
								this ) ) ),
	m_tempoModel( DefaultTempo, MinTempo, MaxTempo, this, tr( "Tempo" ) ),
	m_timeSigModel( this ),
	m_oldTicksPerTact( DefaultTicksPerTact ),
	m_masterVolumeModel( 100, 0, 200, this, tr( "Master volume" ) ),
	m_masterPitchModel( 0, -12, 12, this, tr( "Master pitch" ) ),
	m_fileName(),
	m_oldFileName(),
	m_modified( false ),
	m_recording( false ),
	m_exporting( false ),
	m_exportLoop( false ),
	m_renderBetweenMarkers( false ),
	m_playing( false ),
	m_paused( false ),
	m_loadingProject( false ),
	m_errors( new QList<QString>() ),
	m_playMode( Mode_None ),
	m_length( 0 ),
	m_trackToPlay( NULL ),
	m_patternToPlay( NULL ),
	m_loopPattern( false ),
	m_elapsedMilliSeconds( 0 ),
	m_elapsedTicks( 0 ),
	m_elapsedTacts( 0 )
{
	connect( &m_tempoModel, SIGNAL( dataChanged() ),
						this, SLOT( setTempo() ) );
	connect( &m_tempoModel, SIGNAL( dataUnchanged() ),
						this, SLOT( setTempo() ) );
	connect( &m_timeSigModel, SIGNAL( dataChanged() ),
					this, SLOT( setTimeSignature() ) );


	connect( Engine::mixer(), SIGNAL( sampleRateChanged() ), this,
						SLOT( updateFramesPerTick() ) );

	connect( &m_masterVolumeModel, SIGNAL( dataChanged() ),
			this, SLOT( masterVolumeChanged() ) );
/*	connect( &m_masterPitchModel, SIGNAL( dataChanged() ),
			this, SLOT( masterPitchChanged() ) );*/

	qRegisterMetaType<Note>( "Note" );
	setType( SongContainer );
}




Song::~Song()
{
	m_playing = false;
	delete m_globalAutomationTrack;
}




void Song::masterVolumeChanged()
{
	Engine::mixer()->setMasterGain( m_masterVolumeModel.value() /
								100.0f );
}




void Song::setTempo()
{
	Engine::mixer()->lockPlayHandleRemoval();
	const bpm_t tempo = (bpm_t) m_tempoModel.value();
	PlayHandleList & playHandles = Engine::mixer()->playHandles();
	for( PlayHandleList::Iterator it = playHandles.begin();
						it != playHandles.end(); ++it )
	{
		NotePlayHandle * nph = dynamic_cast<NotePlayHandle *>( *it );
		if( nph && !nph->isReleased() )
		{
			nph->lock();
			nph->resize( tempo );
			nph->unlock();
		}
	}
	Engine::mixer()->unlockPlayHandleRemoval();

	Engine::updateFramesPerTick();

	m_vstSyncController.setTempo( tempo );

	emit tempoChanged( tempo );
}




void Song::setTimeSignature()
{
	MidiTime::setTicksPerTact( ticksPerTact() );
	emit timeSignatureChanged( m_oldTicksPerTact, ticksPerTact() );
	emit dataChanged();
	m_oldTicksPerTact = ticksPerTact();

	m_vstSyncController.setTimeSignature( getTimeSigModel().getNumerator(), getTimeSigModel().getDenominator() );
}




void Song::savePos()
{
	TimeLineWidget * tl = m_playPos[m_playMode].m_timeLine;

	if( tl != NULL )
	{
		tl->savePos( m_playPos[m_playMode] );
	}
}




void Song::processNextBuffer()
{
	if( m_playing == false )
	{
		return;
	}

	TrackList track_list;
	int tco_num = -1;

	switch( m_playMode )
	{
		case Mode_PlaySong:
			track_list = tracks();
			// at song-start we have to reset the LFOs
			if( m_playPos[Mode_PlaySong] == 0 )
			{
				EnvelopeAndLfoParameters::instances()->reset();
			}
			break;

		case Mode_PlayTrack:
			track_list.push_back( m_trackToPlay );
			break;

		case Mode_PlayBB:
			if( Engine::getBBTrackContainer()->numOfBBs() > 0 )
			{
				tco_num = Engine::getBBTrackContainer()->
								currentBB();
				track_list.push_back( BBTrack::findBBTrack(
								tco_num ) );
			}
			break;

		case Mode_PlayPattern:
			if( m_patternToPlay != NULL )
			{
				tco_num = m_patternToPlay->getTrack()->
						getTCONum( m_patternToPlay );
				track_list.push_back(
						m_patternToPlay->getTrack() );
			}
			break;

		default:
			return;

	}

	if( track_list.empty() == true )
	{
		return;
	}

	// check for looping-mode and act if necessary
	TimeLineWidget * tl = m_playPos[m_playMode].m_timeLine;
	bool check_loop = tl != NULL && m_exporting == false &&
				tl->loopPointsEnabled();
	if( check_loop )
	{
		if( m_playPos[m_playMode] < tl->loopBegin() ||
					m_playPos[m_playMode] >= tl->loopEnd() )
		{
			m_elapsedMilliSeconds = (tl->loopBegin().getTicks()*60*1000/48)/getTempo();
			m_playPos[m_playMode].setTicks(
						tl->loopBegin().getTicks() );
		}
	}

	f_cnt_t total_frames_played = 0;
	const float frames_per_tick = Engine::framesPerTick();

	while( total_frames_played
				< Engine::mixer()->framesPerPeriod() )
	{
		m_vstSyncController.update();

		f_cnt_t played_frames = Engine::mixer()->framesPerPeriod() - total_frames_played;

		float current_frame = m_playPos[m_playMode].currentFrame();
		// did we play a tick?
		if( current_frame >= frames_per_tick )
		{
			int ticks = m_playPos[m_playMode].getTicks() + (int)( current_frame / frames_per_tick );

			m_vstSyncController.setAbsolutePosition( ticks );

			// did we play a whole tact?
			if( ticks >= MidiTime::ticksPerTact() )
			{
				// per default we just continue playing even if
				// there's no more stuff to play
				// (song-play-mode)
				int max_tact = m_playPos[m_playMode].getTact()
									+ 2;

				// then decide whether to go over to next tact
				// or to loop back to first tact
				if( m_playMode == Mode_PlayBB )
				{
					max_tact = Engine::getBBTrackContainer()
							->lengthOfCurrentBB();
				}
				else if( m_playMode == Mode_PlayPattern &&
					m_loopPattern == true &&
					tl != NULL &&
					tl->loopPointsEnabled() == false )
				{
					max_tact = m_patternToPlay->length()
								.getTact();
				}

				// end of played object reached?
				if( m_playPos[m_playMode].getTact() + 1
								>= max_tact )
				{
					// then start from beginning and keep
					// offset
					ticks = ticks % ( max_tact * MidiTime::ticksPerTact() );

					// wrap milli second counter
					m_elapsedMilliSeconds = ( ticks * 60 * 1000 / 48 ) / getTempo();

					m_vstSyncController.setAbsolutePosition( ticks );
				}
			}
			m_playPos[m_playMode].setTicks( ticks );

			if( check_loop )
			{
				m_vstSyncController.startCycle( tl->loopBegin().getTicks(), tl->loopEnd().getTicks() );

				if( m_playPos[m_playMode] >= tl->loopEnd() )
				{
					m_playPos[m_playMode].setTicks( tl->loopBegin().getTicks() );
					m_elapsedMilliSeconds = ((tl->loopBegin().getTicks())*60*1000/48)/getTempo();
				}
			}
			else
			{
				m_vstSyncController.stopCycle();
			}

			current_frame = fmodf( current_frame, frames_per_tick );
			m_playPos[m_playMode].setCurrentFrame( current_frame );
		}

		f_cnt_t last_frames = (f_cnt_t)frames_per_tick -
						(f_cnt_t) current_frame;
		// skip last frame fraction
		if( last_frames == 0 )
		{
			++total_frames_played;
			m_playPos[m_playMode].setCurrentFrame( current_frame
								+ 1.0f );
			continue;
		}
		// do we have some samples left in this tick but these are
		// less then samples we have to play?
		if( last_frames < played_frames )
		{
			// then set played_samples to remaining samples, the
			// rest will be played in next loop
			played_frames = last_frames;
		}

		if( (f_cnt_t) current_frame == 0 )
		{
			if( m_playMode == Mode_PlaySong )
			{
				m_globalAutomationTrack->play(
						m_playPos[m_playMode],
						played_frames,
						total_frames_played, tco_num );
			}

			// loop through all tracks and play them
			for( int i = 0; i < track_list.size(); ++i )
			{
				track_list[i]->play( m_playPos[m_playMode],
						played_frames,
						total_frames_played, tco_num );
			}
		}

		// update frame-counters
		total_frames_played += played_frames;
		m_playPos[m_playMode].setCurrentFrame( played_frames +
								current_frame );
		m_elapsedMilliSeconds += (((played_frames/frames_per_tick)*60*1000/48)/getTempo());
		m_elapsedTacts = m_playPos[Mode_PlaySong].getTact();
		m_elapsedTicks = (m_playPos[Mode_PlaySong].getTicks()%ticksPerTact())/48;
	}
}

bool Song::isExportDone() const
{
	if ( m_renderBetweenMarkers )
	{
		return m_exporting == true &&
			m_playPos[Mode_PlaySong].getTicks() >= m_playPos[Mode_PlaySong].m_timeLine->loopEnd().getTicks();
	}
	if( m_exportLoop)
	{
		return m_exporting == true &&
				m_playPos[Mode_PlaySong].getTicks() >= length() * ticksPerTact();
	}
	else
	{
		return m_exporting == true &&
			m_playPos[Mode_PlaySong].getTicks() >= ( length() + 1 ) * ticksPerTact();
	}
}




void Song::playSong()
{
	m_recording = false;

	if( isStopped() == false )
	{
		stop();
	}

	m_playMode = Mode_PlaySong;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePos();

	emit playbackStateChanged();
}




void Song::record()
{
	m_recording = true;
	// TODO: Implement
}




void Song::playAndRecord()
{
	playSong();
	m_recording = true;
}




void Song::playTrack( Track * _trackToPlay )
{
	if( isStopped() == false )
	{
		stop();
	}
	m_trackToPlay = _trackToPlay;

	m_playMode = Mode_PlayTrack;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePos();

	emit playbackStateChanged();
}




void Song::playBB()
{
	if( isStopped() == false )
	{
		stop();
	}

	m_playMode = Mode_PlayBB;
	m_playing = true;
	m_paused = false;

	m_vstSyncController.setPlaybackState( true );

	savePos();

	emit playbackStateChanged();
}




void Song::playPattern( const Pattern* patternToPlay, bool _loop )
{
	if( isStopped() == false )
	{
		stop();
	}

	m_patternToPlay = patternToPlay;
	m_loopPattern = _loop;

	if( m_patternToPlay != NULL )
	{
		m_playMode = Mode_PlayPattern;
		m_playing = true;
		m_paused = false;
	}

	savePos();

	emit playbackStateChanged();
}




void Song::updateLength()
{
	m_length = 0;
	m_tracksMutex.lockForRead();
	for( TrackList::const_iterator it = tracks().begin();
						it != tracks().end(); ++it )
	{
		const tact_t cur = ( *it )->length();
		if( cur > m_length )
		{
			m_length = cur;
		}
	}
	m_tracksMutex.unlock();

	emit lengthChanged( m_length );
}




void Song::setPlayPos( tick_t _ticks, PlayModes _play_mode )
{
	m_elapsedTicks += m_playPos[_play_mode].getTicks() - _ticks;
	m_elapsedMilliSeconds += (((( _ticks - m_playPos[_play_mode].getTicks()))*60*1000/48)/getTempo());
	m_playPos[_play_mode].setTicks( _ticks );
	m_playPos[_play_mode].setCurrentFrame( 0.0f );

// send a signal if playposition changes during playback
	if( isPlaying() ) 
	{
		emit playbackPositionChanged();
	}
}




void Song::togglePause()
{
	if( m_paused == true )
	{
		m_playing = true;
		m_paused = false;
	}
	else
	{
		m_playing = false;
		m_paused = true;
	}

	m_vstSyncController.setPlaybackState( m_playing );

	emit playbackStateChanged();
}




void Song::stop()
{
	// do not stop/reset things again if we're stopped already
	if( m_playMode == Mode_None )
	{
		return;
	}

	TimeLineWidget * tl = m_playPos[m_playMode].m_timeLine;
	m_playing = false;
	m_paused = false;
	m_recording = true;

	if( tl != NULL )
	{

		switch( tl->behaviourAtStop() )
		{
			case TimeLineWidget::BackToZero:
				m_playPos[m_playMode].setTicks( 0 );
				m_elapsedMilliSeconds = 0;
				break;

			case TimeLineWidget::BackToStart:
				if( tl->savedPos() >= 0 )
				{
					m_playPos[m_playMode].setTicks( tl->savedPos().getTicks() );
					m_elapsedMilliSeconds = (((tl->savedPos().getTicks())*60*1000/48)/getTempo());
					tl->savePos( -1 );
				}
				break;

			case TimeLineWidget::KeepStopPosition:
			default:
				break;
		}
	}
	else
	{
		m_playPos[m_playMode].setTicks( 0 );
		m_elapsedMilliSeconds = 0;
	}

	m_playPos[m_playMode].setCurrentFrame( 0 );

	m_vstSyncController.setPlaybackState( m_exporting );
	m_vstSyncController.setAbsolutePosition( m_playPos[m_playMode].getTicks() );

	// remove all note-play-handles that are active
	Engine::mixer()->clear();

	m_playMode = Mode_None;

	emit playbackStateChanged();
}




void Song::startExport()
{
	stop();
	if(m_renderBetweenMarkers)
	{
		m_playPos[Mode_PlaySong].setTicks( m_playPos[Mode_PlaySong].m_timeLine->loopBegin().getTicks() );
	}
	else
	{
		m_playPos[Mode_PlaySong].setTicks( 0 );
	}

	playSong();

	m_exporting = true;

	m_vstSyncController.setPlaybackState( true );
}




void Song::stopExport()
{
	stop();
	m_exporting = false;
	m_exportLoop = false;

	m_vstSyncController.setPlaybackState( m_playing );
}




void Song::insertBar()
{
	m_tracksMutex.lockForRead();
	for( TrackList::const_iterator it = tracks().begin();
					it != tracks().end(); ++it )
	{
		( *it )->insertTact( m_playPos[Mode_PlaySong] );
	}
	m_tracksMutex.unlock();
}




void Song::removeBar()
{
	m_tracksMutex.lockForRead();
	for( TrackList::const_iterator it = tracks().begin();
					it != tracks().end(); ++it )
	{
		( *it )->removeTact( m_playPos[Mode_PlaySong] );
	}
	m_tracksMutex.unlock();
}




void Song::addBBTrack()
{
	Track * t = Track::create( Track::BBTrack, this );
	Engine::getBBTrackContainer()->setCurrentBB( dynamic_cast<BBTrack *>( t )->index() );
}




void Song::addSampleTrack()
{
	(void) Track::create( Track::SampleTrack, this );
}




void Song::addAutomationTrack()
{
	(void) Track::create( Track::AutomationTrack, this );
}




bpm_t Song::getTempo()
{
	return (bpm_t) m_tempoModel.value();
}




AutomationPattern * Song::tempoAutomationPattern()
{
	return AutomationPattern::globalAutomationPattern( &m_tempoModel );
}




void Song::clearProject()
{
	Engine::projectJournal()->setJournalling( false );

	if( m_playing )
	{
		stop();
	}

	for( int i = 0; i < Mode_Count; i++ )
	{
		setPlayPos( 0, ( PlayModes )i );
	}


	Engine::mixer()->lock();

	if( gui && gui->getBBEditor() )
	{
		gui->getBBEditor()->trackContainerView()->clearAllTracks();
	}
	if( gui && gui->songEditor() )
	{
		gui->songEditor()->m_editor->clearAllTracks();
	}
	if( gui && gui->fxMixerView() )
	{
		gui->fxMixerView()->clear();
	}
	QCoreApplication::sendPostedEvents();
	Engine::getBBTrackContainer()->clearAllTracks();
	clearAllTracks();

	Engine::fxMixer()->clear();

	if( gui && gui->automationEditor() )
	{
		gui->automationEditor()->setCurrentPattern( NULL );
	}

	if( gui && gui->pianoRoll() )
	{
		gui->pianoRoll()->reset();
	}

	m_tempoModel.reset();
	m_masterVolumeModel.reset();
	m_masterPitchModel.reset();
	m_timeSigModel.reset();

	AutomationPattern::globalAutomationPattern( &m_tempoModel )->clear();
	AutomationPattern::globalAutomationPattern( &m_masterVolumeModel )->
									clear();
	AutomationPattern::globalAutomationPattern( &m_masterPitchModel )->
									clear();

	Engine::mixer()->unlock();

	if( gui && gui->getProjectNotes() )
	{
		gui->getProjectNotes()->clear();
	}

	// Move to function
	while( !m_controllers.empty() )
	{
		delete m_controllers.last();
	}

	emit dataChanged();

	Engine::projectJournal()->clearJournal();

	Engine::projectJournal()->setJournalling( true );

	InstrumentTrackView::cleanupWindowCache();
}





// create new file
void Song::createNewProject()
{
	QString default_template = ConfigManager::inst()->userProjectsDir()
						+ "templates/default.mpt";

	if( QFile::exists( default_template ) )
	{
		createNewProjectFromTemplate( default_template );
		return;
	}

	default_template = ConfigManager::inst()->factoryProjectsDir()
						+ "templates/default.mpt";
	if( QFile::exists( default_template ) )
	{
		createNewProjectFromTemplate( default_template );
		return;
	}

	m_loadingProject = true;

	clearProject();

	Engine::projectJournal()->setJournalling( false );

	m_fileName = m_oldFileName = "";

	Track * t;
	t = Track::create( Track::InstrumentTrack, this );
	dynamic_cast<InstrumentTrack * >( t )->loadInstrument(
					"tripleoscillator" );
	t = Track::create( Track::InstrumentTrack,
						Engine::getBBTrackContainer() );
	dynamic_cast<InstrumentTrack * >( t )->loadInstrument(
						"kicker" );
	Track::create( Track::SampleTrack, this );
	Track::create( Track::BBTrack, this );
	Track::create( Track::AutomationTrack, this );

	m_tempoModel.setInitValue( DefaultTempo );
	m_timeSigModel.reset();
	m_masterVolumeModel.setInitValue( 100 );
	m_masterPitchModel.setInitValue( 0 );

	QCoreApplication::instance()->processEvents();

	m_loadingProject = false;

	Engine::getBBTrackContainer()->updateAfterTrackAdd();

	Engine::projectJournal()->setJournalling( true );

	QCoreApplication::sendPostedEvents();

	m_modified = false;

	if( gui->mainWindow() )
	{
		gui->mainWindow()->resetWindowTitle();
	}
}




void Song::createNewProjectFromTemplate( const QString & _template )
{
	loadProject( _template );
	// clear file-name so that user doesn't overwrite template when
	// saving...
	m_fileName = m_oldFileName = "";
	// update window title
	if( gui->mainWindow() )
	{
		gui->mainWindow()->resetWindowTitle();
	}

}




// load given song
void Song::loadProject( const QString & _file_name )
{
	QDomNode node;

	m_loadingProject = true;

	Engine::projectJournal()->setJournalling( false );

	m_fileName = _file_name;
	m_oldFileName = _file_name;

	DataFile dataFile( m_fileName );
	// if file could not be opened, head-node is null and we create
	// new project
	if( dataFile.head().isNull() )
	{
		return;
	}

	clearProject();

	clearErrors();

	DataFile::LocaleHelper localeHelper( DataFile::LocaleHelper::ModeLoad );

	Engine::mixer()->lock();

	// get the header information from the DOM
	m_tempoModel.loadSettings( dataFile.head(), "bpm" );
	m_timeSigModel.loadSettings( dataFile.head(), "timesig" );
	m_masterVolumeModel.loadSettings( dataFile.head(), "mastervol" );
	m_masterPitchModel.loadSettings( dataFile.head(), "masterpitch" );

	if( m_playPos[Mode_PlaySong].m_timeLine )
	{
		// reset loop-point-state
		m_playPos[Mode_PlaySong].m_timeLine->toggleLoopPoints( 0 );
	}

	if( !dataFile.content().firstChildElement( "track" ).isNull() )
	{
		m_globalAutomationTrack->restoreState( dataFile.content().
						firstChildElement( "track" ) );
	}

	//Backward compatibility for LMMS <= 0.4.15
	PeakController::initGetControllerBySetting();

	// Load mixer first to be able to set the correct range for FX channels
	node = dataFile.content().firstChildElement( Engine::fxMixer()->nodeName() );
	if( !node.isNull() )
	{
		Engine::fxMixer()->restoreState( node.toElement() );
		if( Engine::hasGUI() )
		{
			// refresh FxMixerView
			gui->fxMixerView()->refreshDisplay();
		}
	}

	node = dataFile.content().firstChild();
	while( !node.isNull() )
	{
		if( node.isElement() )
		{
			if( node.nodeName() == "trackcontainer" )
			{
				( (JournallingObject *)( this ) )->restoreState( node.toElement() );
			}
			else if( node.nodeName() == "controllers" )
			{
				restoreControllerStates( node.toElement() );
			}
			else if( Engine::hasGUI() )
			{
				if( node.nodeName() == gui->getControllerRackView()->nodeName() )
				{
					gui->getControllerRackView()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == gui->pianoRoll()->nodeName() )
				{
					gui->pianoRoll()->restoreState( node.toElement() );
				}
				else if( node.nodeName() == gui->automationEditor()->m_editor->nodeName() )
				{
					gui->automationEditor()->m_editor->restoreState( node.toElement() );
				}
				else if( node.nodeName() == gui->getProjectNotes()->nodeName() )
				{
					 gui->getProjectNotes()->SerializingObject::restoreState( node.toElement() );
				}
				else if( node.nodeName() == m_playPos[Mode_PlaySong].m_timeLine->nodeName() )
				{
					m_playPos[Mode_PlaySong].m_timeLine->restoreState( node.toElement() );
				}
			}
		}
		node = node.nextSibling();
	}

	// quirk for fixing projects with broken positions of TCOs inside
	// BB-tracks
	Engine::getBBTrackContainer()->fixIncorrectPositions();

	// Connect controller links to their controllers 
	// now that everything is loaded
	ControllerConnection::finalizeConnections();

	// resolve all IDs so that autoModels are automated
	AutomationPattern::resolveAllIDs();


	Engine::mixer()->unlock();

	ConfigManager::inst()->addRecentlyOpenedProject( _file_name );

	Engine::projectJournal()->setJournalling( true );

	emit projectLoaded();

	if ( hasErrors())
	{
		if ( Engine::hasGUI() )
		{
			QMessageBox::warning( NULL, "LMMS Error report", *errorSummary(),
							QMessageBox::Ok );
		}
		else
		{
			QTextStream(stderr) << *Engine::getSong()->errorSummary() << endl;
		}
	}

	m_loadingProject = false;
	m_modified = false;

	if( gui && gui->mainWindow() )
	{
		gui->mainWindow()->resetWindowTitle();
	}
}


// only save current song as _filename and do nothing else
bool Song::saveProjectFile( const QString & _filename )
{
	DataFile::LocaleHelper localeHelper( DataFile::LocaleHelper::ModeSave );

	DataFile dataFile( DataFile::SongProject );

	m_tempoModel.saveSettings( dataFile, dataFile.head(), "bpm" );
	m_timeSigModel.saveSettings( dataFile, dataFile.head(), "timesig" );
	m_masterVolumeModel.saveSettings( dataFile, dataFile.head(), "mastervol" );
	m_masterPitchModel.saveSettings( dataFile, dataFile.head(), "masterpitch" );

	saveState( dataFile, dataFile.content() );

	m_globalAutomationTrack->saveState( dataFile, dataFile.content() );
	Engine::fxMixer()->saveState( dataFile, dataFile.content() );
	if( Engine::hasGUI() )
	{
		gui->getControllerRackView()->saveState( dataFile, dataFile.content() );
		gui->pianoRoll()->saveState( dataFile, dataFile.content() );
		gui->automationEditor()->m_editor->saveState( dataFile, dataFile.content() );
		gui->getProjectNotes()->SerializingObject::saveState( dataFile, dataFile.content() );
		m_playPos[Mode_PlaySong].m_timeLine->saveState( dataFile, dataFile.content() );
	}

	saveControllerStates( dataFile, dataFile.content() );

	return dataFile.writeFile( _filename );
}



// save current song and update the gui
bool Song::guiSaveProject()
{
	DataFile dataFile( DataFile::SongProject );
	m_fileName = dataFile.nameWithExtension( m_fileName );
	if( saveProjectFile( m_fileName ) && Engine::hasGUI() )
	{
		TextFloat::displayMessage( tr( "Project saved" ),
					tr( "The project %1 is now saved."
							).arg( m_fileName ),
				embed::getIconPixmap( "project_save", 24, 24 ),
									2000 );
		ConfigManager::inst()->addRecentlyOpenedProject( m_fileName );
		m_modified = false;
		gui->mainWindow()->resetWindowTitle();
	}
	else if( Engine::hasGUI() )
	{
		TextFloat::displayMessage( tr( "Project NOT saved." ),
				tr( "The project %1 was not saved!" ).arg(
							m_fileName ),
				embed::getIconPixmap( "error" ), 4000 );
		return false;
	}

	return true;
}




// save current song in given filename
bool Song::guiSaveProjectAs( const QString & _file_name )
{
	QString o = m_oldFileName;
	m_oldFileName = m_fileName;
	m_fileName = _file_name;
	if( guiSaveProject() == false )
	{
		m_fileName = m_oldFileName;
		m_oldFileName = o;
		return false;
	}
	m_oldFileName = m_fileName;
	return true;
}




void Song::importProject()
{
	FileDialog ofd( NULL, tr( "Import file" ),
			ConfigManager::inst()->userProjectsDir(),
			tr("MIDI sequences") +
			" (*.mid *.midi *.rmi);;" +
			tr("FL Studio projects") +
			" (*.flp);;" +
			tr("Hydrogen projects") +
			" (*.h2song);;" +
			tr("All file types") +
			" (*.*)");

	ofd.setFileMode( FileDialog::ExistingFiles );
	if( ofd.exec () == QDialog::Accepted && !ofd.selectedFiles().isEmpty() )
	{
		ImportFilter::import( ofd.selectedFiles()[0], this );
	}
}




void Song::saveControllerStates( QDomDocument & _doc, QDomElement & _this )
{
	// save settings of controllers
	QDomElement controllersNode =_doc.createElement( "controllers" );
	_this.appendChild( controllersNode );
	for( int i = 0; i < m_controllers.size(); ++i )
	{
		m_controllers[i]->saveState( _doc, controllersNode );
	}
}




void Song::restoreControllerStates( const QDomElement & _this )
{
	QDomNode node = _this.firstChild();
	while( !node.isNull() )
	{
		Controller * c = Controller::create( node.toElement(), this );
		Q_ASSERT( c != NULL );

		/* For PeakController, addController() was called in
		 * PeakControllerEffect::PeakControllerEffect().
		 * This line removes the previously added controller for PeakController
		 * without affecting the order of controllers in Controller Rack
		 */
		Engine::getSong()->removeController( c );
		addController( c );

		node = node.nextSibling();
	}
}


void Song::exportProjectTracks()
{
	exportProject(true);
}

void Song::exportProject(bool multiExport)
{
	if( isEmpty() )
	{
		QMessageBox::information( gui->mainWindow(),
				tr( "Empty project" ),
				tr( "This project is empty so exporting makes "
					"no sense. Please put some items into "
					"Song Editor first!" ) );
		return;
	}

	FileDialog efd( gui->mainWindow() );
	if (multiExport)
	{
		efd.setFileMode( FileDialog::Directory);
		efd.setWindowTitle( tr( "Select directory for writing exported tracks..." ) );
		if( !m_fileName.isEmpty() )
		{
			efd.setDirectory( QFileInfo( m_fileName ).absolutePath() );
		}
	}
	else
	{
		efd.setFileMode( FileDialog::AnyFile );
		int idx = 0;
		QStringList types;
		while( __fileEncodeDevices[idx].m_fileFormat != ProjectRenderer::NumFileFormats )
		{
			types << tr( __fileEncodeDevices[idx].m_description );
			++idx;
		}
		efd.setNameFilters( types );
		QString base_filename;
		if( !m_fileName.isEmpty() )
		{
			efd.setDirectory( QFileInfo( m_fileName ).absolutePath() );
			base_filename = QFileInfo( m_fileName ).completeBaseName();
		}
		else
		{
			efd.setDirectory( ConfigManager::inst()->userProjectsDir() );
			base_filename = tr( "untitled" );
		}
		efd.selectFile( base_filename + __fileEncodeDevices[0].m_extension );
		efd.setWindowTitle( tr( "Select file for project-export..." ) );
	}

	efd.setAcceptMode( FileDialog::AcceptSave );


	if( efd.exec() == QDialog::Accepted && !efd.selectedFiles().isEmpty() && !efd.selectedFiles()[0].isEmpty() )
	{
		QString suffix = "";
		if ( !multiExport )
		{
			int stx = efd.selectedNameFilter().indexOf( "(*." );
			int etx = efd.selectedNameFilter().indexOf( ")" );
	
			if ( stx > 0 && etx > stx ) 
			{
				// Get first extension from selected dropdown.
				// i.e. ".wav" from "WAV-File (*.wav), Dummy-File (*.dum)"
				suffix = efd.selectedNameFilter().mid( stx + 2, etx - stx - 2 ).split( " " )[0].trimmed();
				if ( efd.selectedFiles()[0].endsWith( suffix ) )
				{
					suffix = "";
				}
			}
		}

		const QString export_file_name = efd.selectedFiles()[0] + suffix;
		ExportProjectDialog epd( export_file_name, gui->mainWindow(), multiExport );
		epd.exec();
	}
}


void Song::exportProjectMidi()
{
	if( isEmpty() )
	{
		QMessageBox::information( gui->mainWindow(),
				tr( "Empty project" ),
				tr( "This project is empty so exporting makes "
					"no sense. Please put some items into "
					"Song Editor first!" ) );
		return;
	}

	FileDialog efd( gui->mainWindow() );
	
	efd.setFileMode( FileDialog::AnyFile );
	
	QStringList types;
	types << tr("MIDI File (*.mid)");
	efd.setNameFilters( types );
	QString base_filename;
	if( !m_fileName.isEmpty() )
	{
		efd.setDirectory( QFileInfo( m_fileName ).absolutePath() );
		base_filename = QFileInfo( m_fileName ).completeBaseName();
	}
	else
	{
		efd.setDirectory( ConfigManager::inst()->userProjectsDir() );
		base_filename = tr( "untitled" );
	}
	efd.selectFile( base_filename + ".mid" );
	efd.setWindowTitle( tr( "Select file for project-export..." ) );

	efd.setAcceptMode( FileDialog::AcceptSave );


	if( efd.exec() == QDialog::Accepted && !efd.selectedFiles().isEmpty() && !efd.selectedFiles()[0].isEmpty() )
	{
		const QString suffix = ".mid";

		QString export_filename = efd.selectedFiles()[0];
		if (!export_filename.endsWith(suffix)) export_filename += suffix;
		
		// NOTE start midi export
		
		// instantiate midi export plugin
		TrackContainer::TrackList tracks;
		tracks += Engine::getSong()->tracks();
		tracks += Engine::getBBTrackContainer()->tracks();
		ExportFilter *exf = dynamic_cast<ExportFilter *> (Plugin::instantiate("midiexport", NULL, NULL));
		if (exf==NULL) {
			qDebug() << "failed to load midi export filter!";
			return;
		}
		exf->tryExport(tracks, Engine::getSong()->getTempo(), export_filename);
	}
}



void Song::updateFramesPerTick()
{
	Engine::updateFramesPerTick();
}




void Song::setModified()
{
	if( !m_loadingProject )
	{
		m_modified = true;
		if( Engine::hasGUI() && gui->mainWindow() &&
			QThread::currentThread() == gui->mainWindow()->thread() )
		{
			gui->mainWindow()->resetWindowTitle();
		}
	}
}




void Song::addController( Controller * _c )
{
	if( _c != NULL && !m_controllers.contains( _c ) ) 
	{
		m_controllers.append( _c );
		emit dataChanged();
	}
}




void Song::removeController( Controller * _controller )
{
	int index = m_controllers.indexOf( _controller );
	if( index != -1 )
	{
		m_controllers.remove( index );

		if( Engine::getSong() )
		{
			Engine::getSong()->setModified();
		}
		emit dataChanged();
	}
}



void Song::clearErrors()
{
	m_errors->clear();
}



void Song::collectError( const QString error )
{
	m_errors->append( error );
}



bool Song::hasErrors()
{
	return ( m_errors->length() > 0 );
}



QString* Song::errorSummary()
{
	QString* errors = new QString();

	for ( int i = 0 ; i < m_errors->length() ; i++ )
	{
		errors->append( m_errors->value( i ) + "\n" );
	}

	errors->prepend( "\n\n" );
	errors->prepend( tr( "The following errors occured while loading: " ) );

	return errors;
}
