//=========================================================
//  MusE
//  Linux Music Editor
//
//  stemexport.cpp
//   Exports the output of tracks to files, one file per track ("stems").
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
//=========================================================

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSet>
#include <QTimer>

#include "stemexport.h"
#include "stemcapture.h"
#include "markermixdown.h"
#include "app.h"
#include "audio.h"
#include "driver/audiodev.h"
#include "gconfig.h"
#include "globals.h"
#include "operations.h"
#include "song.h"
#include "synth.h"
#include "track.h"
#include "wave.h"

namespace MusECore {

//---------------------------------------------------------
//   listed
//---------------------------------------------------------

bool StemExport::listed(const Track* t)
{
  switch(t->type())
  {
    case Track::WAVE:
    case Track::MIDI:
    case Track::DRUM:
    case Track::AUDIO_SOFTSYNTH:
    case Track::AUDIO_GROUP:
      return true;
    // Inputs carry no recorded material, outputs are the downmix, and the aux tracks
    //  are part of every stem.
    default:
      return false;
  }
}

//---------------------------------------------------------
//   source
//---------------------------------------------------------

AudioTrack* StemExport::source(const Track* t, QString* why)
{
  switch(t->type())
  {
    case Track::WAVE:
    case Track::AUDIO_SOFTSYNTH:
    case Track::AUDIO_GROUP:
      return const_cast<AudioTrack*>(static_cast<const AudioTrack*>(t));

    case Track::MIDI:
    case Track::DRUM:
    {
      SynthI* si = static_cast<const MidiTrack*>(t)->drivenSynth();
      if(!si)
      {
        if(why)
          *why = tr("The track does not play a synth, there is no audio to export.");
        return nullptr;
      }
      if(si->frozen())
      {
        // The synth plays its frozen file, whichever midi tracks are soloed.
        if(why)
          *why = tr("The synth \"%1\" that the track plays is frozen. Export the synth track instead, "
                    "or unfreeze it.").arg(si->name());
        return nullptr;
      }
      return si;
    }

    default:
      if(why)
        *why = tr("Only wave, midi, synth and group tracks can be exported as stems.");
      return nullptr;
  }
}

//---------------------------------------------------------
//   StemExport
//---------------------------------------------------------

StemExport::StemExport(const MixdownSettings& s, AudioOutput* output, const QString& projectName)
  : QObject(nullptr), _settings(s), _output(output), _projectName(projectName),
    _index(0), _running(false), _stopSeen(false), _playSeen(false), _waitPolls(0), _cancelled(false),
    _capture(nullptr), _pd(nullptr), _timer(nullptr)
{
}

StemExport::~StemExport()
{
  delete _capture;
}

//---------------------------------------------------------
//   buildJobs
//    Range markers: <folder>/<project>_<track>.<ext>
//    Markers:       <folder>/<marker>/<project>_<marker>_<track>.<ext>
//---------------------------------------------------------

void StemExport::buildJobs(const std::vector<Track*>& tracks)
{
  std::vector<MixdownSection> sections;
  if(_settings.markerMode)
    sections = markerMixdownSections();
  else
    sections.push_back({QString(), MusEGlobal::song->lPos().frame(), MusEGlobal::song->rPos().frame()});

  const QString project = sanitizeFileName(_projectName);
  const QString ext = _settings.extension();
  QSet<QString> used;
  for(const MixdownSection& s : sections)
  {
    QDir dir(_settings.path);
    QString prefix = project;
    if(!s.name.isEmpty())
    {
      QString marker = sanitizeFileName(s.name);
      if(marker.isEmpty())
        marker = QString("marker");
      dir.setPath(dir.filePath(marker));
      prefix = prefix.isEmpty() ? marker : prefix + "_" + marker;
    }
    for(Track* t : tracks)
    {
      QString name = sanitizeFileName(t->name());
      if(name.isEmpty())
        name = QString("track");
      const QString base = dir.filePath(prefix.isEmpty() ? name : prefix + "_" + name);
      // Tracks (or markers) with the same name must not write to the same file.
      QString path;
      for(int n = 1; ; ++n)
      {
        path = (n == 1 ? base : base + QString("_%1").arg(n)) + "." + ext;
        if(!used.contains(QFileInfo(path).absoluteFilePath()))
          break;
      }
      used.insert(QFileInfo(path).absoluteFilePath());
      _jobs.push_back({s.name, s.startFrame, s.endFrame, t, path});
    }
  }
}

//---------------------------------------------------------
//   start
//---------------------------------------------------------

bool StemExport::start(const MixdownSettings& settings, const std::vector<Track*>& tracks,
                       AudioOutput* output, const QString& projectName)
{
  const QString title = tr("Export Stems");
  if(!MusEGlobal::audioDevice)
  {
    QMessageBox::critical(MusEGlobal::muse, title, tr("There is no audio device running."));
    return true;
  }
  if(MusEGlobal::audio->isPlaying() || MusEGlobal::audio->bounce() || MusEGlobal::song->freezeTrack)
  {
    QMessageBox::warning(MusEGlobal::muse, title, tr("Stop the transport before exporting stems."));
    return true;
  }
  if(MusEGlobal::song->record())
  {
    QMessageBox::warning(MusEGlobal::muse, title, tr("Switch off recording before exporting stems."));
    return true;
  }
  if(!settings.markerMode && MusEGlobal::song->rPos().frame() <= MusEGlobal::song->lPos().frame())
  {
    QMessageBox::critical(MusEGlobal::muse, title,
      tr("Set the left and right range markers around the part of the song to export."));
    return true;
  }

  if(output && (output->off() || output->mute()))
  {
    QMessageBox::warning(MusEGlobal::muse, title,
      tr("The output track \"%1\" is muted or off, the stems would be silent.").arg(output->name()));
    return true;
  }

  StemExport* se = new StemExport(settings, output, projectName);
  se->buildJobs(tracks);
  if(se->_jobs.empty())
  {
    QMessageBox::critical(MusEGlobal::muse, title,
      settings.markerMode ?
        tr("No named markers found to export.\nGive the markers where each part should start a name, "
           "and make sure there is a marker after it, or that it is before the end of the song.") :
        tr("No tracks to export."));
    delete se;
    return true;
  }

  // A set of stems belongs together, so rather overwrite than number single files.
  int existing = 0;
  for(const Job& j : se->_jobs)
    if(QFileInfo::exists(j.path))
      ++existing;
  if(existing > 0)
  {
    QMessageBox mb(QMessageBox::Question, title,
      tr("%n stem file(s) in %1 will be overwritten.", nullptr, existing)
        .arg(QDir::toNativeSeparators(settings.path)),
      QMessageBox::NoButton, MusEGlobal::muse);
    // Custom buttons, the standard ones trigger a KDE localization warning.
    QPushButton* ok_btn = mb.addButton(tr("Overwrite"), QMessageBox::AcceptRole);
    mb.addButton(tr("Cancel"), QMessageBox::RejectRole);
    mb.setDefaultButton(ok_btn);
    mb.exec();
    if(mb.clickedButton() != ok_btn)
    {
      delete se;
      return true;
    }
  }

  for(Track* t : *MusEGlobal::song->tracks())
    if(!t->isFolderTrack())
      se->_soloStates.push_back(std::make_pair(t, t->solo()));

  se->_pd = new QProgressDialog(QString(), tr("Cancel"), 0, 100, MusEGlobal::muse);
  se->_pd->setWindowTitle(title);
  se->_pd->setAttribute(Qt::WA_DeleteOnClose);
  se->_pd->setWindowModality(Qt::ApplicationModal);
  se->_pd->setMinimumDuration(0);
  se->_pd->setAutoClose(false);
  se->_pd->setAutoReset(false);
  se->_pd->setValue(0);
  se->_pd->show();

  connect(MusEGlobal::song, SIGNAL(playChanged(bool)), se, SLOT(playChanged(bool)));

  // The passes run in the audio thread and are finished from the normal GUI message handling,
  //  so poll instead of using a nested event loop.
  se->_timer = new QTimer(se);
  connect(se->_timer, SIGNAL(timeout()), se, SLOT(poll()));
  se->_timer->start(100);
  return false;
}

//---------------------------------------------------------
//   applySolo
//    Solo the track and the aux tracks that are heard. Everything else is muted by that.
//---------------------------------------------------------

void StemExport::applySolo(const Track* soloTrack)
{
  PendingOperationList ops;
  for(Track* t : *MusEGlobal::song->tracks())
  {
    if(t->isFolderTrack())
      continue;
    const bool want = t == soloTrack || (t->type() == Track::AUDIO_AUX && !t->mute() && !t->off());
    if(t->solo() != want)
      ops.add(PendingOperationItem(t, want, PendingOperationItem::SetTrackSolo));
  }
  if(!ops.empty())
    MusEGlobal::audio->msgExecutePendingOperations(ops, true);
}

//---------------------------------------------------------
//   restoreSolo
//---------------------------------------------------------

void StemExport::restoreSolo()
{
  PendingOperationList ops;
  for(const std::pair<Track*, bool>& s : _soloStates)
    if(s.first->solo() != s.second)
      ops.add(PendingOperationItem(s.first, s.second, PendingOperationItem::SetTrackSolo));
  if(!ops.empty())
    MusEGlobal::audio->msgExecutePendingOperations(ops, true);
}

//---------------------------------------------------------
//   startJob
//    Returns false if the job could not be started.
//---------------------------------------------------------

bool StemExport::startJob()
{
  const Job& job = _jobs[_index++];
  const QString name = job.section.isEmpty() ? job.track->name() : job.section + " - " + job.track->name();
  _pd->setLabelText(tr("Stem %1 of %2: %3").arg(_index).arg(_jobs.size()).arg(name));
  _pd->setValue(0);

  AudioTrack* src = source(job.track);
  if(!src)
  {
    _failed.append(name);
    return false;
  }

  applySolo(job.track);

  std::vector<AudioTrack*> sources;
  if(_output)
    // The aux tracks, groups etc. on the way are in there. Everything else is muted by the solo.
    sources.push_back(_output);
  else
  {
    sources.push_back(src);
    for(AudioAux* a : *MusEGlobal::song->auxs())
      if(a->solo() && a != src)
        sources.push_back(a);
  }

  SndFile* sf = _settings.createSndFile(job.path);
  SndFileR sfr(sf);
  if(sf->openWrite())
  {
    _failed.append(QDir::toNativeSeparators(job.path));
    return false;
  }
  if(sf->isWriteOnly() && _settings.quality >= 0.0 && sf->setEncodingQuality(_settings.quality))
    fprintf(stderr, "StemExport: could not set encoding quality for %s\n", job.path.toLocal8Bit().constData());

  _capture = new StemCapture(sources, sfr, _settings.channels, job.startFrame, job.endFrame - job.startFrame);
  MusEGlobal::audio->setStemCapture(_capture);
  // The capture measures the latencies of its tracks in the audio thread.
  for(int i = 0; i < 20 && !_capture->latencyKnown(); ++i)
    MusEGlobal::audio->msgAudioWait();
  const int lmin = _capture->minLatency();
  const int lmax = _capture->maxLatency();
  // A track that plays early to correct for latency (negative latency) must start before the section.
  const unsigned preroll = lmin < 0 ? std::min((unsigned)-lmin, job.startFrame) : 0;

  // Switch all wave converters to offline settings mode.
  MusEGlobal::song->setAudioConvertersOfflineOperation(true);
  // The capture stops the transport when it has all frames, this end is just a safety net.
  MusEGlobal::audio->setBounceEndFrame(job.endFrame + (unsigned)std::max(lmax, 0) + MusEGlobal::sampleRate);

  _stopSeen = false;
  _playSeen = false;
  _waitPolls = 0;
  _running = true;
  // This will wait a few cycles until freewheel is set and a seek is done.
  const Pos start(job.startFrame - preroll, false);
  MusEGlobal::audio->msgBounce(&start);
  MusEGlobal::song->setPlay(true);
  return true;
}

//---------------------------------------------------------
//   finishJob
//    The transport has stopped.
//---------------------------------------------------------

void StemExport::finishJob()
{
  _running = false;
  MusEGlobal::audio->clearBounceEndFrame();
  MusEGlobal::audio->setStemCapture(nullptr);
  const bool ok = _capture->finish();
  delete _capture;
  _capture = nullptr;

  const Job& job = _jobs[_index - 1];
  if(ok)
    _created.append(job.path);
  else
  {
    // A stem that is not complete is of no use.
    QFile::remove(job.path);
    if(!_cancelled)
      _failed.append(QDir::toNativeSeparators(job.path));
  }
}

//---------------------------------------------------------
//   playChanged
//---------------------------------------------------------

void StemExport::playChanged(bool playing)
{
  if(playing)
    _playSeen = true;
  else if(_running)
    // Song::stopRolling() is handled completely before the next poll.
    _stopSeen = true;
}

//---------------------------------------------------------
//   poll
//---------------------------------------------------------

void StemExport::poll()
{
  if(_pd->wasCanceled() && !_cancelled)
  {
    _cancelled = true;
    if(_running && MusEGlobal::audioDevice)
      MusEGlobal::audioDevice->stopTransport();
    _pd->setCancelButton(nullptr);
    _pd->setLabelText(tr("Cancelling..."));
  }

  if(_running)
  {
    if(MusEGlobal::audio->isPlaying())
      _playSeen = true;
    const bool stopped = !MusEGlobal::audio->isPlaying() && !MusEGlobal::audio->bounce();
    // Did the pass not start at all (some 10 seconds)?
    if(!_playSeen && stopped && ++_waitPolls > 100)
      _stopSeen = true;
    if(!(_stopSeen && stopped))
    {
      if(!_cancelled)
      {
        const int64_t total = _capture->frames();
        const int v = total > 0 ? int(_capture->mixedFrames() * 100 / total) : 0;
        _pd->setValue(std::min(99, std::max(0, v)));
      }
      return;
    }
    finishJob();
  }

  if(_cancelled || _index >= _jobs.size())
  {
    finish();
    return;
  }
  // Should it fail, the next poll goes on with the next one.
  startJob();
}

//---------------------------------------------------------
//   finish
//---------------------------------------------------------

void StemExport::finish()
{
  _timer->stop();
  disconnect(MusEGlobal::song, SIGNAL(playChanged(bool)), this, SLOT(playChanged(bool)));
  restoreSolo();
  _pd->close();

  const QString title = tr("Export Stems");
  const QString folder = QDir::toNativeSeparators(_settings.path);
  if(_cancelled)
    QMessageBox::information(MusEGlobal::muse, title,
      tr("The stem export was cancelled. %n file(s) written to %1", nullptr, int(_created.size())).arg(folder));
  else if(!_failed.isEmpty())
    QMessageBox::warning(MusEGlobal::muse, title,
      tr("%n file(s) written to %1", nullptr, int(_created.size())).arg(folder) + "\n\n" +
      tr("These stems could not be exported:") + "\n" + _failed.join("\n"));
  else
    QMessageBox::information(MusEGlobal::muse, title,
      tr("%n file(s) written to %1", nullptr, int(_created.size())).arg(folder));
  deleteLater();
}

} // namespace MusECore
