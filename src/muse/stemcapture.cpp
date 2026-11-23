//=========================================================
//  MusE
//  Linux Music Editor
//
//  stemcapture.cpp
//   Renders the summed output of some tracks to a file, sample exact.
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
#include <climits>
#include <cmath>
#include <cstring>

#include "stemcapture.h"
#include "globals.h"
#include "track.h"
#include "latency_info.h"

namespace MusECore {

//---------------------------------------------------------
//   StemCapture
//---------------------------------------------------------

StemCapture::StemCapture(const std::vector<AudioTrack*>& sources, const SndFileR& sf, int channels,
                         unsigned startFrame, unsigned frames)
  : _sources(sources), _sf(sf), _channels(channels), _startFrame(startFrame), _frames(frames),
    _complete(0), _written(0), _overrun(false), _minLatency(0), _maxLatency(0), _latencyKnown(false)
{
  // Room for a few seconds: the latency differences between the sources, plus the
  //  frames the writer may be behind when not freewheeling.
  int64_t sz = 1 << 17;
  while(sz < 8 * (int64_t)MusEGlobal::segmentSize)
    sz <<= 1;
  _ringSize = sz;
  _ringMask = sz - 1;
  _ring.assign(_channels * _ringSize, 0.0f);
  _scratch.assign(_channels * MusEGlobal::segmentSize, 0.0f);
}

//---------------------------------------------------------
//   process
//    Audio thread.
//---------------------------------------------------------

void StemCapture::process(unsigned pos, unsigned nframes, bool playing)
{
  if(nframes > MusEGlobal::segmentSize)
    nframes = MusEGlobal::segmentSize;

  // The signal leaving a track at transport frame 'pos' belongs to timeline frame
  //  pos - outputLatency (the latency may be negative if the track corrects for it by playing early).
  // Without latency correction the engine does not compensate either (as the bounce to file).
  const size_t nsrc = _sources.size();
  int lat[nsrc > 0 ? nsrc : 1];
  int lmin = INT_MAX, lmax = INT_MIN;
  for(size_t i = 0; i < nsrc; ++i)
  {
    AudioTrack* src = _sources[i];
    if(!src->useLatencyCorrection())
      lat[i] = 0;
    else
    {
      const TrackLatencyInfo& li = src->getLatencyInfo(false);
      // An output track's own latency includes its port's, which comes after the signal taken here
      //  (as in AudioOutput::processWrite() for the bounce to file).
      lat[i] = (int)lrintf(src->type() == Track::AUDIO_OUTPUT ?
                           li._inputLatency + li._worstPluginLatency : li._outputLatency);
    }
    lmin = std::min(lmin, lat[i]);
    lmax = std::max(lmax, lat[i]);
  }
  if(nsrc == 0)
    lmin = lmax = 0;
  _minLatency.store(lmin, std::memory_order_release);
  _maxLatency.store(lmax, std::memory_order_release);
  _latencyKnown.store(true, std::memory_order_release);

  if(!playing)
    return;

  const int64_t written = _written.load(std::memory_order_acquire);
  const int64_t complete = _complete.load(std::memory_order_relaxed);
  // Ring slots of the frames from 'written' on are free (zeroed) up to here.
  const int64_t limit = std::min(_frames, written + _ringSize);

  float* buf[_channels];
  for(int ch = 0; ch < _channels; ++ch)
    buf[ch] = _scratch.data() + ch * MusEGlobal::segmentSize;

  for(size_t i = 0; i < nsrc; ++i)
  {
    // File frames of this cycle for this source.
    const int64_t fo = (int64_t)pos - _startFrame - lat[i];
    // Frames before 'complete' are final (only if the latency grew during the pass), frames before
    //  zero and from _frames on are not part of the file.
    const int64_t lo = std::max(fo, std::max(complete, (int64_t)0));
    const int64_t hi = std::min(fo + (int64_t)nframes, limit);
    if(std::min(fo + (int64_t)nframes, _frames) > limit)
      _overrun.store(true, std::memory_order_release);
    if(lo >= hi)
      continue;

    for(int ch = 0; ch < _channels; ++ch)
      memset(buf[ch], 0, sizeof(float) * nframes);
    // The track has been processed in this cycle, so this only takes its output buffers, converted to
    //  the file's channels (with the pan applied to a mono track). A muted track leaves them silent.
    AudioTrack* src = _sources[i];
    src->copyData(pos, 0, _channels, _channels, 0, src->channels(), nframes, buf, true);

    for(int ch = 0; ch < _channels; ++ch)
    {
      float* ring = _ring.data() + ch * _ringSize;
      const float* sp = buf[ch] + (lo - fo);
      for(int64_t f = lo; f < hi; ++f)
        ring[f & _ringMask] += *sp++;
    }
  }

  // Every source has delivered the frames up to here.
  int64_t c = (int64_t)pos + nframes - _startFrame - lmax;
  if(c > limit)
    c = limit;
  if(c > complete)
    _complete.store(c, std::memory_order_release);
}

//---------------------------------------------------------
//   writeChunk
//---------------------------------------------------------

void StemCapture::writeChunk(int64_t from, int64_t n)
{
  const int64_t idx = from & _ringMask;
  float* bp[_channels];
  for(int ch = 0; ch < _channels; ++ch)
    bp[ch] = _ring.data() + ch * _ringSize + idx;
  if(!_sf.isNull())
    _sf->write(_channels, bp, n, false);
  // Zero the slots, they are added to when they come round again.
  for(int ch = 0; ch < _channels; ++ch)
    memset(bp[ch], 0, sizeof(float) * n);
}

//---------------------------------------------------------
//   write
//---------------------------------------------------------

void StemCapture::write()
{
  int64_t w = _written.load(std::memory_order_relaxed);
  const int64_t c = _complete.load(std::memory_order_acquire);
  while(w < c)
  {
    const int64_t idx = w & _ringMask;
    const int64_t n = std::min(c - w, _ringSize - idx);
    writeChunk(w, n);
    w += n;
  }
  _written.store(w, std::memory_order_release);
}

//---------------------------------------------------------
//   finish
//---------------------------------------------------------

bool StemCapture::finish()
{
  write();
  const bool ok = _written.load(std::memory_order_acquire) >= _frames && !overrun();
  if(!_sf.isNull())
  {
    _sf->close();
    _sf = nullptr;
  }
  return ok;
}

} // namespace MusECore
