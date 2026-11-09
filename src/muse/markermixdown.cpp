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

// Makes a string usable as part of a file name.
static QString sanitizeFileName(const QString& s)
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
//   MarkerMixdown
//---------------------------------------------------------

MarkerMixdown::MarkerMixdown(AudioOutput* ao, const MixdownSettings& s, const QString& projectName)
  : QObject(nullptr), _ao(ao), _settings(s), _projectName(projectName),
    _index(0), _running(false), _cancelled(false), _pd(nullptr), _timer(nullptr)
{
}

MarkerMixdown::~MarkerMixdown()
{
}

//---------------------------------------------------------
//   buildJobs
//---------------------------------------------------------

void MarkerMixdown::buildJobs()
{
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
    _jobs.push_back({markers[i].name, start, end});
  }
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

  // The mixdowns run in the audio thread and are finished from the normal GUI message handling,
  //  so poll instead of using a nested event loop.
  mm->_timer = new QTimer(mm);
  connect(mm->_timer, SIGNAL(timeout()), mm, SLOT(poll()));
  mm->startJob();
  mm->_timer->start(100);
  return false;
}

//---------------------------------------------------------
//   startJob
//---------------------------------------------------------

void MarkerMixdown::startJob()
{
  const Job& job = _jobs[_index++];
  const QString path = uniquePath(job.name);
  SndFile* sf = _settings.createSndFile(path);
  _created.append(path);
  _pd->setLabelText(tr("Mixdown %1 of %2: %3").arg(_index).arg(_jobs.size()).arg(job.name));
  _pd->setValue(0);

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
    // Done when the transport has stopped and the recording has been wrapped up by the GUI thread.
    if(!MusEGlobal::audio->bounce() && !MusEGlobal::audio->isPlaying() && MusEGlobal::song->bounceOutput == nullptr)
    {
      _running = false;
      MusEGlobal::audio->clearBounceEndFrame();
    }
    else
    {
      if(!_cancelled)
      {
        const Job& job = _jobs[_index - 1];
        const unsigned pos = MusEGlobal::audio->pos().frame();
        const int64_t len = int64_t(job.endFrame) - job.startFrame;
        int v = pos > job.startFrame && len > 0 ? int((int64_t(pos) - job.startFrame) * 100 / len) : 0;
        _pd->setValue(std::min(99, std::max(0, v)));
      }
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
  _pd->close();
  const QString title = tr("Downmix");
  if(_cancelled)
    QMessageBox::information(MusEGlobal::muse, title,
      tr("The downmix was cancelled. The last file may be incomplete."));
  else
    QMessageBox::information(MusEGlobal::muse, title,
      tr("%n file(s) written to %1", nullptr, int(_created.size())).arg(QDir::toNativeSeparators(_settings.path)));
  deleteLater();
}

} // namespace MusECore
