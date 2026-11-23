//=========================================================
//  MusE
//  Linux Music Editor
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
#include <QFileInfo>
#include <QMessageBox>
#include <QProgressDialog>
#include <QTimer>

#include "markermixdown.h"
#include "app.h"
#include "audio.h"
#include "driver/audiodev.h"
#include "globals.h"
#include "song.h"
#include "track.h"
#include "marker/marker.h"
#include "wave.h"

namespace MusECore {

//---------------------------------------------------------
//   sanitizeFileName
//---------------------------------------------------------

QString sanitizeFileName(const QString& s)
{
  QString r;
  for(const QChar& c : s.simplified())
  {
    if(c.unicode() < 32 || QString("/\\:*?\"<>|").contains(c))
      r += QLatin1Char('_');
    else
      r += c;
  }
  while(r.startsWith('.'))
    r.remove(0, 1);
  return r;
}

//---------------------------------------------------------
//   exportBlockedReason
//---------------------------------------------------------

QString exportBlockedReason()
{
  if(!MusEGlobal::audioDevice)
    return QObject::tr("There is no audio device running.");
  if(MusEGlobal::audio->isPlaying() || MusEGlobal::audio->bounce() || MusEGlobal::song->freezeTrack ||
     MusEGlobal::song->bounceOutput || MusEGlobal::song->bounceTrack)
    return QObject::tr("Stop the transport (and wait for a running export or freeze to finish) before exporting.");
  if(MusEGlobal::song->record())
    return QObject::tr("Switch off recording before exporting.");
  if(MusEGlobal::extSyncFlag)
    return QObject::tr("The transport follows external sync, exports are not possible.");
  return QString();
}

//---------------------------------------------------------
//   MarkerMixdown
//---------------------------------------------------------

MarkerMixdown::MarkerMixdown(AudioOutput* ao, const MixdownSettings& s, const QString& projectName)
  : QObject(nullptr), _ao(ao), _settings(s), _projectName(projectName),
    _index(0), _running(false), _cancelled(false), _cancelRequested(false), _finished(false),
    _pd(nullptr), _timer(nullptr)
{
}

MarkerMixdown::~MarkerMixdown()
{
}

//---------------------------------------------------------
//   markerMixdownSections
//   buildJobs
//---------------------------------------------------------

std::vector<MixdownSection> markerMixdownSections()
{
  std::vector<MixdownSection> sections;
  struct M { QString name; unsigned frame; };
  std::vector<M> markers;
  for(ciMarker i = MusEGlobal::song->marker()->cbegin(); i != MusEGlobal::song->marker()->cend(); ++i)
    markers.push_back({i->second.name().trimmed(), i->second.frame()});
  std::stable_sort(markers.begin(), markers.end(), [](const M& a, const M& b) { return a.frame < b.frame; });

  const unsigned song_end = Pos(MusEGlobal::song->len(), true).frame();
  for(size_t i = 0; i < markers.size(); ++i)
  {
    if(markers[i].name.isEmpty())
      continue;
    const unsigned start = markers[i].frame;
    const unsigned end = (i + 1 < markers.size()) ? markers[i + 1].frame : song_end;
    if(end <= start)
      continue; // Nothing to render.
    sections.push_back({markers[i].name, start, end});
  }
  return sections;
}

void MarkerMixdown::buildJobs()
{
  if(_settings.markerMode)
  {
    for(const MixdownSection& s : markerMixdownSections())
      _jobs.push_back({s.name, s.startFrame, s.endFrame, QString()});
  }
  else
    _jobs.push_back({tr("Range"), MusEGlobal::song->lPos().frame(), MusEGlobal::song->rPos().frame(), _settings.path});
}

//---------------------------------------------------------
//   uniquePath
//    <folder>/<project>_<marker>.<ext>, with _2, _3... added if that exists.
//---------------------------------------------------------

QString MarkerMixdown::uniquePath(const QString& markerName) const
{
  QString stem = sanitizeFileName(markerName);
  if(stem.isEmpty())
    stem = QString("marker");
  if(!_projectName.isEmpty())
    stem = sanitizeFileName(_projectName) + "_" + stem;
  const QString base = QDir(_settings.path).filePath(stem);
  const QString ext = _settings.extension();
  for(int n = 1; ; ++n)
  {
    const QString p = (n == 1 ? base : base + QString("_%1").arg(n)) + "." + ext;
    if(!QFileInfo::exists(p))
      return p;
  }
}

//---------------------------------------------------------
//   start
//---------------------------------------------------------

bool MarkerMixdown::start(AudioOutput* ao, const MixdownSettings& s, const QString& projectName)
{
  const QString title = tr("MusE: Record Downmix to File");
  MarkerMixdown* mm = new MarkerMixdown(ao, s, projectName);
  mm->buildJobs();
  if(mm->_jobs.empty())
  {
    QMessageBox::critical(MusEGlobal::muse, title,
      tr("No named markers found to mix down.\nGive the markers where each mixdown should start a name, "
         "and make sure there is a marker after it, or that it is before the end of the song."));
    delete mm;
    return true;
  }

  mm->_pd = new QProgressDialog(QString(), tr("Cancel"), 0, 100, MusEGlobal::muse);
  mm->_pd->setWindowTitle(tr("Downmix"));
  mm->_pd->setAttribute(Qt::WA_DeleteOnClose);
  mm->_pd->setWindowModality(Qt::ApplicationModal);
  mm->_pd->setMinimumDuration(0);
  mm->_pd->setAutoClose(false);
  mm->_pd->setAutoReset(false);
  mm->_pd->setValue(0);
  mm->_pd->show();

  mm->run();
  return false;
}

//---------------------------------------------------------
//   begin
//---------------------------------------------------------

MarkerMixdown* MarkerMixdown::begin(AudioOutput* ao, const MixdownSettings& s, const QString& projectName,
                                    bool overwrite, QString* error)
{
  QString e = exportBlockedReason();
  if(e.isEmpty() && !s.markerMode && MusEGlobal::song->rPos().frame() <= MusEGlobal::song->lPos().frame())
    e = tr("Set the left and right range markers around the part of the song to mix down.");
  if(e.isEmpty() && !s.markerMode && !overwrite && QFileInfo::exists(s.path))
    e = tr("The file %1 exists.").arg(QDir::toNativeSeparators(s.path));
  if(e.isEmpty() && (ao->off() || ao->mute()))
    e = tr("The output track \"%1\" is muted or off, the downmix would be silent.").arg(ao->name());
  MarkerMixdown* mm = nullptr;
  if(e.isEmpty())
  {
    mm = new MarkerMixdown(ao, s, projectName);
    mm->buildJobs();
    if(mm->_jobs.empty())
    {
      e = tr("No named markers found to mix down. Give the markers where each mixdown should start a name, "
             "and make sure there is a marker after it, or that it is before the end of the song.");
      delete mm;
      mm = nullptr;
    }
  }
  if(!mm)
  {
    if(error)
      *error = e;
    return nullptr;
  }
  mm->run();
  return mm;
}

//---------------------------------------------------------
//   run
//---------------------------------------------------------

void MarkerMixdown::run()
{
  // The mixdowns run in the audio thread and are finished from the normal GUI message handling,
  //  so poll instead of using a nested event loop.
  _timer = new QTimer(this);
  connect(_timer, SIGNAL(timeout()), this, SLOT(poll()));
  startJob();
  _timer->start(100);
}

//---------------------------------------------------------
//   report
//---------------------------------------------------------

ExportReport MarkerMixdown::report() const
{
  ExportReport r;
  r.finished = _finished;
  r.cancelled = _cancelled;
  r.jobCount = int(_jobs.size());
  r.job = int(_index);
  if(_running && _index > 0 && _index <= _jobs.size())
  {
    const Job& job = _jobs[_index - 1];
    r.current = job.name;
    const unsigned pos = MusEGlobal::audio->pos().frame();
    const int64_t len = int64_t(job.endFrame) - job.startFrame;
    const int v = pos > job.startFrame && len > 0 ? int((int64_t(pos) - job.startFrame) * 100 / len) : 0;
    r.percent = std::min(99, std::max(0, v));
  }
  r.created = _created;
  return r;
}

//---------------------------------------------------------
//   startJob
//---------------------------------------------------------

void MarkerMixdown::startJob()
{
  const Job& job = _jobs[_index++];
  const QString path = job.path.isEmpty() ? uniquePath(job.name) : job.path;
  SndFile* sf = _settings.createSndFile(path);
  _created.append(path);
  if(_pd)
  {
    _pd->setLabelText(tr("Mixdown %1 of %2: %3").arg(_index).arg(_jobs.size()).arg(job.name));
    _pd->setValue(0);
  }

  // Switch all wave converters to offline settings mode.
  MusEGlobal::song->setAudioConvertersOfflineOperation(true);
  MusEGlobal::audio->setBounceEndFrame(job.endFrame);

  // This will wait a few cycles until freewheel is set and a seek is done.
  const Pos start(job.startFrame, false);
  MusEGlobal::audio->msgBounce(&start);
  MusEGlobal::song->bounceOutput = _ao;
  _ao->setRecFile(sf);
  MusEGlobal::song->setRecord(true, false);
  MusEGlobal::song->setRecordFlag(_ao, true);
  _ao->prepareRecording();
  _running = true;
  MusEGlobal::song->setPlay(true);
}

//---------------------------------------------------------
//   poll
//---------------------------------------------------------

void MarkerMixdown::poll()
{
  if(!_cancelled && (_cancelRequested || (_pd && _pd->wasCanceled())))
  {
    _cancelled = true;
    if(_running && MusEGlobal::audioDevice)
      MusEGlobal::audioDevice->stopTransport();
    if(_pd)
    {
      _pd->setCancelButton(nullptr);
      _pd->setLabelText(tr("Cancelling..."));
    }
  }

  if(_running)
  {
    // Done when the transport has stopped and the recording has been wrapped up by the GUI thread.
    // Also wait for freewheel mode to be off again, so that the next mixdown starts from a settled transport.
    if(!MusEGlobal::audio->bounce() && !MusEGlobal::audio->isPlaying() && MusEGlobal::song->bounceOutput == nullptr &&
       !MusEGlobal::audio->freewheel())
    {
      _running = false;
      MusEGlobal::audio->clearBounceEndFrame();
    }
    else
    {
      if(!_cancelled && _pd)
        _pd->setValue(report().percent);
      return;
    }
  }

  if(_cancelled || _index >= _jobs.size())
  {
    finish();
    return;
  }
  startJob();
}

//---------------------------------------------------------
//   finish
//---------------------------------------------------------

void MarkerMixdown::finish()
{
  _timer->stop();
  _finished = true;
  if(_pd)
  {
    _pd->close();
    const QString title = tr("Downmix");
    if(_cancelled)
      QMessageBox::information(MusEGlobal::muse, title,
        tr("The downmix was cancelled. The last file may be incomplete."));
    else
      QMessageBox::information(MusEGlobal::muse, title,
        tr("%n file(s) written to %1", nullptr, int(_created.size())).arg(QDir::toNativeSeparators(_settings.path)));
  }
  emit finished(report());
  deleteLater();
}

} // namespace MusECore
