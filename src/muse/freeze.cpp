//=========================================================
//  MusE
//  Linux Music Editor
//
//  freeze.cpp
//   Freeze track: The post-effects (pre-fader) signal of a wave or synth track is rendered to a
//   wave file. While frozen the track plays that file instead of processing its parts, synth and
//   effects rack, which saves CPU.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2
//  of the License, or (at your option) any later version.
//=========================================================

#include <unistd.h>
#include <string.h>
#include <mutex>

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>

#include "track.h"
#include "audio.h"
#include "driver/audiodev.h"
#include "globals.h"
#include "song.h"
#include "wave.h"
#include "al/dsp.h"
#include "app.h"
#include "pos.h"
#include "type_defs.h"

namespace MusECore {

// Guards the frozen/freeze files against the prefetch thread while the GUI thread opens and closes them.
static std::mutex freezeMutex;

//---------------------------------------------------------
//   canFreeze
//---------------------------------------------------------

bool AudioTrack::canFreeze() const
{
  if(type() != Track::WAVE && type() != Track::AUDIO_SOFTSYNTH)
    return false;
  // Only the main (first two) channels go through the effects rack and are captured.
  return totalProcessBuffers() == channels();
}

//---------------------------------------------------------
//   freezeBegin
//---------------------------------------------------------

bool AudioTrack::freezeBegin(QString* errorMsg)
{
  if(!canFreeze() || frozen() || freezeCapturing())
  {
    if(errorMsg)
      *errorMsg = QObject::tr("This track can not be frozen.");
    return true;
  }

  const QString fbase = MusEGlobal::museProject + QString("/") +
                        QString("FREEZE_%1_").arg(name().simplified().replace(" ", "_"));
  int n = 1;
  QString fname;
  for(;; ++n)
  {
    fname = fbase + QString("%1.wav").arg(n);
    if(!QFile::exists(fname))
      break;
  }

  SndFile* sf = new SndFile(fname, false);
  sf->setFormat(SF_FORMAT_WAV | SF_FORMAT_FLOAT, channels(), MusEGlobal::sampleRate);
  if(sf->openWrite())
  {
    if(errorMsg)
      *errorMsg = QObject::tr("Could not create %1").arg(fname);
    delete sf;
    return true;
  }

  std::lock_guard<std::mutex> lock(freezeMutex);
  _freezeSf = sf;
  _freezeFifo.clear();
  _freezeFilePos = -1; // Forces a seek before the first write.
  _freezeCapturing.store(true, std::memory_order_release);
  return false;
}

//---------------------------------------------------------
//   freezeWriteAt
//    Write nframes at the given frame position of the file. filePos is the current end of the
//    written data (-1 if nothing has been written). libsndfile can not seek past the end of a file
//    opened for writing, so a gap (ie. the first cycles were not captured) is filled with silence.
//    Otherwise everything after the gap would end up too early in the file.
//---------------------------------------------------------

static void freezeWriteAt(SndFileR& sf, int64_t& filePos, int chans, float** bp, int64_t pos, unsigned nframes)
{
  if(filePos < 0)
    filePos = 0;
  if(pos > filePos)
  {
    static const float zeros[1024] = { 0.0f };
    const float* zp[chans];
    for(int c = 0; c < chans; ++c)
      zp[c] = zeros;
    sf->seek(filePos, 0);
    while(filePos < pos)
    {
      const unsigned n = (pos - filePos) < 1024 ? (unsigned)(pos - filePos) : 1024;
      sf->write(chans, const_cast<float**>(zp), n, false);
      filePos += n;
    }
  }
  else if(pos < filePos)
    sf->seek(pos, 0);
  sf->write(chans, bp, nframes, false);
  filePos = pos + nframes;
}

//---------------------------------------------------------
//   freezeCapture
//    Audio thread.
//---------------------------------------------------------

void AudioTrack::freezeCapture(unsigned pos, int chans, unsigned nframes, float** bp)
{
  if(!MusEGlobal::audio->isPlaying() || _freezeSf.isNull())
    return;

  if(MusEGlobal::audio->freewheel())
  {
    // Freewheel (offline): Write directly.
    freezeWriteAt(_freezeSf, _freezeFilePos, chans, bp, pos, nframes);
  }
  else if(_freezeFifo.put(chans, nframes, bp, pos, 0.0f))
    fprintf(stderr, "AudioTrack::freezeCapture(%s): fifo overrun at frame %u\n", name().toLocal8Bit().constData(), pos);
}

//---------------------------------------------------------
//   freezeWriteTick
//    Prefetch thread.
//---------------------------------------------------------

void AudioTrack::freezeWriteTick()
{
  std::lock_guard<std::mutex> lock(freezeMutex);
  if(_freezeSf.isNull())
    return;
  const int chans = channels();
  float* buf[chans];
  while(_freezeFifo.getCount())
  {
    MuseCount_t p;
    if(_freezeFifo.peek(chans, MusEGlobal::segmentSize, buf, &p))
      break;
    freezeWriteAt(_freezeSf, _freezeFilePos, chans, buf, p, MusEGlobal::segmentSize);
    // Remove only after writing, so the count reaching zero means everything has been written.
    _freezeFifo.remove();
  }
}

//---------------------------------------------------------
//   freezeEnd
//    GUI thread. The transport must have stopped.
//    Returns true on error, ie. the render was incomplete and has been discarded.
//---------------------------------------------------------

bool AudioTrack::freezeEnd(int64_t requiredFrames)
{
  if(!freezeCapturing())
    return true;

  // Wait for the render fifo to be written to the file by the prefetch thread.
  // Slow disks may take a while, so restart the timeout as long as there is progress.
  int tout = 100;
  int last = _freezeFifo.getCount();
  while(_freezeFifo.getCount() != 0)
  {
    usleep(100000);
    const int cnt = _freezeFifo.getCount();
    if(cnt < last)
      tout = 100;
    last = cnt;
    if(--tout == 0)
    {
      fprintf(stderr, "AudioTrack::freezeEnd(%s): Timeout waiting for the render fifo to empty\n",
              name().toLocal8Bit().constData());
      break;
    }
  }

  QString path;
  bool complete;
  {
    std::lock_guard<std::mutex> lock(freezeMutex);
    _freezeCapturing.store(false, std::memory_order_release);
    complete = _freezeFilePos >= requiredFrames && _freezeFifo.getCount() == 0;
    path = _freezeSf->path();
    _freezeSf->close();
    _freezeSf = nullptr;
  }

  QFileInfo fi(path);
  const QString wca = fi.absolutePath() + QString("/") + fi.completeBaseName() + QString(".wca");
  QFile::remove(wca);
  if(!complete || freezeSetFile(fi.fileName()))
  {
    QFile::remove(path);
    return true;
  }
  return false;
}

//---------------------------------------------------------
//   freezeSetFile
//    GUI thread. Also used when loading a song.
//---------------------------------------------------------

bool AudioTrack::freezeSetFile(const QString& fileName)
{
  QString path = fileName;
  if(QFileInfo(path).isRelative())
    path = MusEGlobal::museProject + QString("/") + fileName;
  if(!QFile::exists(path))
  {
    fprintf(stderr, "AudioTrack::freezeSetFile(%s): file <%s> not found\n",
            name().toLocal8Bit().constData(), path.toLocal8Bit().constData());
    return true;
  }

  SndFile* sf = new SndFile(path, false);
  if(sf->openRead(false))
  {
    fprintf(stderr, "AudioTrack::freezeSetFile(%s): could not open <%s>\n",
            name().toLocal8Bit().constData(), path.toLocal8Bit().constData());
    delete sf;
    return true;
  }
  // The frozen signal is played back as is, without any conversion. So it must match the engine.
  if(sf->sampleRateDiffers() || sf->channels() != channels())
  {
    fprintf(stderr, "AudioTrack::freezeSetFile(%s): <%s> sample rate or channels do not match\n",
            name().toLocal8Bit().constData(), path.toLocal8Bit().constData());
    sf->close();
    delete sf;
    return true;
  }

  {
    std::lock_guard<std::mutex> lock(freezeMutex);
    _frozenSf = sf;
    _frozenFileName = fileName;
    _frozenFifo.clear();
    _frozenWritePos = ~0U;
    _frozenDirectPos = -1;
    _frozen.store(true, std::memory_order_release);
  }
  // Make the prefetch thread position the file and fill the fifo.
  if(MusEGlobal::audio && MusEGlobal::audio->isRunning() && MusEGlobal::song)
    MusEGlobal::audio->msgSeek(MusEGlobal::song->cPos());
  return false;
}

//---------------------------------------------------------
//   unfreeze
//    GUI thread. The frozen file is kept on disk.
//---------------------------------------------------------

void AudioTrack::unfreeze()
{
  if(!frozen())
    return;
  _frozen.store(false, std::memory_order_release);
  // Let the audio thread complete a cycle, so it is no longer using the file.
  if(MusEGlobal::audio && MusEGlobal::audio->isRunning())
  {
    MusEGlobal::audio->msgAudioWait();
    MusEGlobal::audio->msgAudioWait();
  }
  {
    std::lock_guard<std::mutex> lock(freezeMutex);
    if(!_frozenSf.isNull())
    {
      _frozenSf->close();
      _frozenSf = nullptr;
    }
  }
  // Make the prefetch thread position the normal wave data again.
  if(MusEGlobal::audio && MusEGlobal::audio->isRunning() && MusEGlobal::song)
    MusEGlobal::audio->msgSeek(MusEGlobal::song->cPos());
}

//---------------------------------------------------------
//   frozenSeek
//    Prefetch thread.
//---------------------------------------------------------

void AudioTrack::frozenSeek(unsigned pos)
{
  std::lock_guard<std::mutex> lock(freezeMutex);
  if(!frozen() || _frozenSf.isNull())
    return;
  _frozenFifo.clear();
  _frozenWritePos = pos;
  _frozenSf->seek(pos, 0);
}

//---------------------------------------------------------
//   frozenPrefetch
//    Prefetch thread. Fills the playback fifo from the frozen file.
//---------------------------------------------------------

void AudioTrack::frozenPrefetch(bool doLoops, unsigned lposFrame, unsigned rposFrame)
{
  std::lock_guard<std::mutex> lock(freezeMutex);
  if(!frozen() || _frozenSf.isNull() || _frozenWritePos == ~0U)
    return;

  const int empty = _frozenFifo.getEmptyCount();
  if(empty <= 0)
    return;

  const int ch = channels();
  float* bp[ch];
  unsigned wp = _frozenWritePos;
  const unsigned seg = MusEGlobal::segmentSize;

  for(int i = 0; i < empty; ++i)
  {
    if(doLoops)
    {
      // Same loop handling as the wave track prefetch.
      unsigned n = rposFrame - wp;
      if(n < seg)
      {
        if(n > lposFrame)
          n = 0;
        wp = lposFrame - n;
        _frozenSf->seek(wp, 0);
      }
    }

    if(_frozenFifo.getWriteBuffer(ch, seg, bp, wp))
      break;

    const size_t rn = _frozenSf->read(ch, bp, seg);
    if(rn < seg)
    {
      for(int c = 0; c < ch; ++c)
        memset(bp[c] + rn, 0, sizeof(float) * (seg - rn));
    }
    _frozenFifo.add();
    wp += seg;
  }
  _frozenWritePos = wp;
}

//---------------------------------------------------------
//   frozenFetch
//    Audio thread. Returns whether data was available.
//---------------------------------------------------------

bool AudioTrack::frozenFetch(unsigned pos, int chans, unsigned nframes, float** bp)
{
  if(_frozenSf.isNull())
    return false;

  if(MusEGlobal::audio->freewheel())
  {
    // Freewheel (offline): The prefetch thread is idle, so read directly from the file.
    if(_frozenDirectPos != (int64_t)pos)
      _frozenSf->seek(pos, 0);
    const size_t rn = _frozenSf->read(chans, bp, nframes);
    if(rn < nframes)
    {
      for(int c = 0; c < chans; ++c)
        memset(bp[c] + rn, 0, sizeof(float) * (nframes - rn));
    }
    _frozenDirectPos = (int64_t)pos + nframes;
    return true;
  }

  float* pf[chans];
  MuseCount_t fpos;
  for(;;)
  {
    if(_frozenFifo.peek(chans, nframes, pf, &fpos))
      return false; // Underrun.
    // A buffer entirely behind us is stale. Drop it.
    if(fpos + nframes <= pos)
    {
      _frozenFifo.remove();
      continue;
    }
    break;
  }

  if(fpos > (MuseCount_t)pos)
    return false; // Not there yet.

  const unsigned off = pos - fpos;
  const unsigned cnt = nframes - off;
  for(int c = 0; c < chans; ++c)
  {
    memcpy(bp[c], pf[c] + off, sizeof(float) * cnt);
    if(off)
      memset(bp[c] + cnt, 0, sizeof(float) * off);
  }
  if(off == 0)
    _frozenFifo.remove();
  return true;
}


//---------------------------------------------------------
//   freezeTrackStart
//    GUI thread. Renders the track in an offline (bounce) pass from the start of the song.
//    Returns true on error, a message has been shown.
//---------------------------------------------------------

bool Song::freezeTrackStart(AudioTrack* t)
{
  const QString title = tr("Freeze track");

  if(!t || !t->canFreeze())
  {
    QMessageBox::warning(MusEGlobal::muse, title, tr("Only wave and synth tracks can be frozen."));
    return true;
  }
  if(t->frozen())
    return true;
  if(MusEGlobal::audio->isPlaying() || MusEGlobal::audio->bounce() || freezeTrack)
  {
    QMessageBox::warning(MusEGlobal::muse, title, tr("Stop the transport before freezing a track."));
    return true;
  }
  if(t->off() || t->isMute() || t->recordFlag())
  {
    QMessageBox::warning(MusEGlobal::muse, title,
      tr("The track must be on and audible (not muted, and no other track soloed), and not armed for recording."));
    return true;
  }

  // Render to the end of the last part on a wave track, or to the end of the song for a synth
  //  (it is fed by midi tracks), plus a tail for reverbs, synth releases etc.
  const unsigned tail_frames = 5 * MusEGlobal::sampleRate;
  int64_t end_frame = 0;
  if(t->type() == Track::WAVE)
  {
    for(ciPart ip = t->cparts()->begin(); ip != t->cparts()->end(); ++ip)
    {
      const int64_t e = ip->second->endFrame();
      if(e > end_frame)
        end_frame = e;
    }
  }
  else
    end_frame = Pos(len(), true).frame();
  const int64_t frames = end_frame + tail_frames;

  QString err;
  if(t->freezeBegin(&err))
  {
    QMessageBox::warning(MusEGlobal::muse, title, err);
    return true;
  }

  freezeFrames = frames;
  freezeCancelled = false;
  freezeTrack = t;
  // Switch all wave converters to offline settings mode.
  setAudioConvertersOfflineOperation(true);
  MusEGlobal::audio->setFreezeEndFrame((unsigned)frames);
  // This will wait a few cycles until freewheel is set and a seek is done.
  const Pos start(0, false);
  MusEGlobal::audio->msgBounce(&start);
  setPlay(true);
  return false;
}

//---------------------------------------------------------
//   freezeTrackFinish
//    GUI thread. Called when the transport has stopped. Does nothing if no freeze is in progress.
//---------------------------------------------------------

void Song::freezeTrackFinish()
{
  AudioTrack* t = freezeTrack;
  if(!t)
    return;
  freezeTrack = nullptr;

  // Allow a little slack at the end, the last cycle may not have been written.
  const int64_t required = freezeFrames - 2 * (int64_t)MusEGlobal::segmentSize;
  const bool err = t->freezeEnd(required);
  if(err && !freezeCancelled)
    QMessageBox::warning(MusEGlobal::muse, tr("Freeze track"),
      tr("The freeze was interrupted. The track was not frozen."));
  setDirty();
  update(SC_TRACK_MODIFIED);
}

//---------------------------------------------------------
//   unfreezeTrack
//---------------------------------------------------------

void Song::unfreezeTrack(AudioTrack* t)
{
  if(!t || !t->frozen())
    return;
  if(MusEGlobal::audio->isPlaying())
  {
    QMessageBox::warning(MusEGlobal::muse, tr("Unfreeze track"), tr("Stop the transport before unfreezing a track."));
    return;
  }
  t->unfreeze();
  setDirty();
  update(SC_TRACK_MODIFIED);
}

//---------------------------------------------------------
//   freezeTrackCancel
//---------------------------------------------------------

void Song::freezeTrackCancel()
{
  if(!freezeTrack)
    return;
  freezeCancelled = true;
  // Same as when the end is reached. The resulting stopRolling finishes the freeze, which will
  //  see that there is not enough data and remove the file.
  MusEGlobal::audioDevice->stopTransport();
}

//---------------------------------------------------------
//   freezeTrackInteractive
//---------------------------------------------------------

void Song::freezeTrackInteractive(AudioTrack* t)
{
  if(!t || t->frozen() || t->freezeCapturing() || freezeTrack)
    return;

  QMessageBox mb(QMessageBox::Question, tr("Freeze track"),
    tr("Freeze track \"%1\"?\n\nThe track will be rendered to a wave file that is played instead of the "
       "track's processing. Parts on the track can not be edited until it is unfrozen.").arg(t->name()),
    QMessageBox::NoButton, MusEGlobal::muse);
  // Custom buttons, the standard ones trigger a KDE localization warning.
  QPushButton* ok_btn = mb.addButton(tr("Freeze"), QMessageBox::AcceptRole);
  mb.addButton(tr("Cancel"), QMessageBox::RejectRole);
  mb.setDefaultButton(ok_btn);
  mb.exec();
  if(mb.clickedButton() != ok_btn)
    return;

  if(freezeTrackStart(t))
    return;

  QProgressDialog* pd = new QProgressDialog(tr("Freezing track \"%1\"...").arg(t->name()),
                                            tr("Cancel"), 0, 100, MusEGlobal::muse);
  pd->setWindowTitle(tr("Freeze track"));
  pd->setAttribute(Qt::WA_DeleteOnClose);
  pd->setWindowModality(Qt::ApplicationModal);
  pd->setMinimumDuration(0);
  pd->setAutoClose(false);
  pd->setAutoReset(false);
  pd->setValue(0);
  pd->show();

  // Poll the progress. The freeze itself runs in the audio thread and is finished
  //  from the normal GUI message handling, so no nested event loop is needed.
  QTimer* timer = new QTimer(pd);
  QObject::connect(timer, &QTimer::timeout, [this, pd, timer]() {
    if(!freezeTrack)
    {
      timer->stop();
      pd->close();
      return;
    }
    if(pd->wasCanceled())
    {
      timer->stop();
      freezeTrackCancel();
      // Keep the dialog up until the freeze has really finished.
      pd->setCancelButton(nullptr);
      pd->setLabelText(tr("Cancelling..."));
      timer->start(50);
      return;
    }
    const int64_t total = freezeFrames > 0 ? freezeFrames : 1;
    int v = (int)((int64_t)MusEGlobal::audio->pos().frame() * 100 / total);
    if(v < 0) v = 0;
    if(v > 99) v = 99;
    pd->setValue(v);
  });
  timer->start(100);
}

//---------------------------------------------------------
//   unfreezeTrackInteractive
//---------------------------------------------------------

void Song::unfreezeTrackInteractive(AudioTrack* t)
{
  if(!t || !t->frozen())
    return;
  QMessageBox mb(QMessageBox::Question, tr("Unfreeze track"),
    tr("Unfreeze track \"%1\"?\n\nThe track will be processed normally again. "
       "The frozen wave file is kept.").arg(t->name()),
    QMessageBox::NoButton, MusEGlobal::muse);
  QPushButton* ok_btn = mb.addButton(tr("Unfreeze"), QMessageBox::AcceptRole);
  mb.addButton(tr("Cancel"), QMessageBox::RejectRole);
  mb.setDefaultButton(ok_btn);
  mb.exec();
  if(mb.clickedButton() != ok_btn)
    return;
  unfreezeTrack(t);
}

} // namespace MusECore
