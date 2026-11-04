//=========================================================
//  MusE
//  Linux Music Editor
//
//  audioparallel.h
//  Processes the audio tracks of a cycle on several threads. Off by default. Turn it on with the
//  the number of threads (including the audio thread, 2 to 16) in the general settings, or with the
//  environment variable MUSE_AUDIO_THREADS=<n>, which overrides the settings. Unless that is used, the
//  processing measures both ways now and then and uses the parallel one only while it is faster.
//  See README.parallel.
//
//  The dependencies between the tracks are the in-routes and, for an aux track, the tracks that send to
//  it. Every track is processed once per cycle (see AudioTrack::copyData, which is claimed by the first
//  thread to get there), so a dependency that is missing from the graph costs time, not correctness.
//  The tracks are processed ahead of whoever pulls from them. Then the usual serial passes of
//  Audio::process1 find them done, and only the audio outputs are left for them.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __AUDIOPARALLEL_H__
#define __AUDIOPARALLEL_H__

#include <atomic>

namespace MusECore {

// True while the tracks are being processed in parallel (tells AudioTrack::waitDone to wait).
extern std::atomic<bool> audioParallelRunning;

// Main thread, about every 50 ms (the heart beat). Follows the settings: starts worker threads when needed.
void audioParallelTick();

// Audio thread, in Audio::process1 before the serial passes. Does nothing if it is off, if there is
//  nothing to gain, or if the graph cannot be used (then the serial passes do all the work).
void audioParallelProcess(unsigned pos, unsigned frames);
// Audio thread, at the end of Audio::process1. Times the cycle, to choose between parallel and serial.
void audioParallelEnd();

// What the settings dialog shows. Numbers are averages of whole passes over the tracks (without the
//  audio outputs), measured by the audio thread.
struct AudioParallelStatus
{
  enum State { Off, Measuring, Parallel, SerialAuto, TooSmall };
  State state = Off;
  int threads = 0;             // Including the audio thread, 0 if off.
  bool realTime = true;        // The worker threads have real time priority.
  int tracks = 0;              // Tracks in the graph.
  int longestChain = 0;        // Tracks in the longest chain of tracks that need each other.
  double parallelUs = 0.0;     // Average pass in parallel mode, 0 if not measured.
  double serialUs = 0.0;       // Average pass in serial mode, 0 if not measured.
};
AudioParallelStatus audioParallelStatus();

} // namespace MusECore

#endif
