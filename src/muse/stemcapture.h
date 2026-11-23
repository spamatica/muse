//=========================================================
//  MusE
//  Linux Music Editor
//
//  stemcapture.h
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

#ifndef __STEMCAPTURE_H__
#define __STEMCAPTURE_H__

#include <atomic>
#include <cstdint>
#include <vector>

#include "wave.h"

namespace MusECore {

class AudioTrack;

//---------------------------------------------------------
//   StemCapture
//    Sums the output (after fader and pan) of the source tracks into a file of
//    exactly 'frames' frames, which starts at the timeline frame 'startFrame'.
//    A source can also be an output track (its signal before it goes to the port).
//
//    Each source is placed by its own output latency, so the file is aligned to the
//    timeline whatever plugin latencies there are. The audio thread mixes into a ring
//    buffer (process()), the finished part is written to the file by write(): from
//    the prefetch thread, or from the audio thread while freewheeling.
//    See Audio::setStemCapture().
//---------------------------------------------------------

class StemCapture {
      std::vector<AudioTrack*> _sources;
      SndFileR _sf;
      const int _channels;
      const int64_t _startFrame;
      const int64_t _frames;

      // The mix. Ring buffer of _ringSize frames per channel, _ringSize is a power of two.
      // File frame f is at index (f & _ringMask).
      std::vector<float> _ring;
      int64_t _ringSize;
      int64_t _ringMask;
      // Audio thread: scratch buffers for one cycle of one source.
      std::vector<float> _scratch;

      // Frames [0, _complete) are mixed and final. Set by the audio thread.
      std::atomic<int64_t> _complete;
      // Frames [0, _written) are in the file, and their ring slots are zeroed for reuse. Set by the writer.
      std::atomic<int64_t> _written;
      // The ring buffer was full (the writer could not keep up), or data came after its frames were final.
      std::atomic<bool> _overrun;
      // Output latency range of the sources, in frames. Measured in the audio thread.
      std::atomic<int> _minLatency;
      std::atomic<int> _maxLatency;
      std::atomic<bool> _latencyKnown;

      void writeChunk(int64_t from, int64_t n);

   public:
      // GUI thread. 'sf' must be open for writing, with 'channels' channels.
      StemCapture(const std::vector<AudioTrack*>& sources, const SndFileR& sf, int channels,
                  unsigned startFrame, unsigned frames);

      // Audio thread. Once in every cycle, after all tracks have been processed.
      // Mixes the cycle into the ring buffer if 'playing', otherwise only measures the latencies.
      void process(unsigned pos, unsigned nframes, bool playing);
      // Writes the finished part of the mix to the file. Prefetch thread, or the audio thread
      //  while freewheeling. Never at the same time from two threads (see Audio::_stemMutex).
      void write();
      // GUI thread, after the capture has been removed from the audio engine.
      // Writes the rest and closes the file. Returns true if the file is complete.
      bool finish();

      // Whether all frames have been mixed. The transport can then stop.
      bool complete() const { return _complete.load(std::memory_order_acquire) >= _frames; }
      int64_t mixedFrames() const { return _complete.load(std::memory_order_acquire); }
      int64_t frames() const { return _frames; }
      bool overrun() const { return _overrun.load(std::memory_order_acquire); }
      bool latencyKnown() const { return _latencyKnown.load(std::memory_order_acquire); }
      int minLatency() const { return _minLatency.load(std::memory_order_acquire); }
      int maxLatency() const { return _maxLatency.load(std::memory_order_acquire); }
      };

} // namespace MusECore

#endif
