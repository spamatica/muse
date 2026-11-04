//=========================================================
//  MusE
//  Linux Music Editor
//
//  audioprofile.h
//  Measures how much time every track needs in the audio thread and which tracks
//  depend on which, to find out how much of the audio processing could run in parallel.
//  Off by default. Turn it on with the environment variable MUSE_AUDIO_PROFILE=<seconds>
//  (the report interval, 5 if the value is not a number). The report goes to stderr.
//  This only measures, it does not change how anything is processed.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __AUDIOPROFILE_H__
#define __AUDIOPROFILE_H__

#include <atomic>
#include <chrono>
#include <cstdint>

namespace MusECore {

class AudioTrack;

// The time one track needs, without the time of the tracks it pulls from.
// Counted in the audio thread, read by the report in the main thread.
// Copying one gives an empty one, so that copying a track does not copy its numbers.
struct TrackProfile
{
  std::atomic<uint64_t> ns { 0 };        // Since the last report.
  std::atomic<uint64_t> maxNs { 0 };     // The longest single run since the last report.
  std::atomic<uint32_t> runs { 0 };      // Number of runs since the last report.

  TrackProfile() {}
  TrackProfile(const TrackProfile&) {}
  TrackProfile& operator=(const TrackProfile&) { return *this; }
};

extern bool audioProfileEnabled;

// Reads MUSE_AUDIO_PROFILE. Call once at start.
void audioProfileInit();
// Main thread, about every 50 ms (the heart beat). Prints the report when it is time.
void audioProfileTick();
// Audio thread: the time of one pass over all the tracks (Audio::process1).
void audioProfileCycle(uint64_t ns, unsigned frames);

// Audio thread. Put one on the stack where a track starts to process, and it adds the time until
//  it goes out of scope to that track, minus what was used by tracks processed in the meantime.
class ProfileScope
{
  public:
    ProfileScope(AudioTrack* track);
    ~ProfileScope();
  private:
    bool _active;
    AudioTrack* _track;
};

// For timing a stretch of the audio thread.
inline uint64_t audioProfileNow()
{
  return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
    std::chrono::steady_clock::now().time_since_epoch()).count());
}

} // namespace MusECore

#endif
