//=========================================================
//  MusE
//  Linux Music Editor
//
//  Direct ALSA audio backend, an alternative to Jack and Pulse.
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

#ifndef __ALSAAUDIO_H__
#define __ALSAAUDIO_H__

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <string.h>

#include <QList>
#include <QString>

#include <vector>

#include "audiodev.h"
#include "globals.h"
#include "large_int.h"

// For debugging output: Uncomment the fprintf section.
#define DEBUG_ALSAAUDIO(dev, format, args...) // fprintf(dev, format, ##args);

namespace MusECore {

struct MuseAlsaAudioPort {
  QString name;
  float* buffer;
};

//---------------------------------------------------------
//   AlsaAudioDevice
//   Talks directly to an ALSA pcm device, using one thread
//    that reads a period of capture data, runs the MusE
//    process cycle (using the built-in transport), and writes
//    a period of playback data.
//---------------------------------------------------------

class AlsaAudioDevice : public AudioDevice {
  private:
      snd_pcm_t* _playback;
      snd_pcm_t* _capture;
      snd_pcm_format_t _playFormat;
      snd_pcm_format_t _capFormat;
      unsigned int _playChannels;
      unsigned int _capChannels;
      snd_pcm_uframes_t _periodSize;
      unsigned int _periodCount;

      // The interleaved device data of one period.
      std::vector<char> _playBuf;
      std::vector<char> _capBuf;

      pthread_t _thread;
      bool _threadCreated;
      volatile bool _quit;
      int _realTimePriority;

      // Critical variables that need to all update at once.
      // We employ a 'flipping' technique.
      unsigned _framesAtCycleStart[2];
      uint64_t _timeUSAtCycleStart[2];
      unsigned _frameCounter[2];
      unsigned _criticalVariablesIdx;
      bool _firstCycle;
      // Whether an xrun was already counted in the current cycle. The capture and playback
      //  streams are linked so one glitch usually shows up in both.
      bool _xrunThisCycle;
      // How long to wait for the device in each cycle before giving up on that cycle, in milliseconds.
      int _waitMs;
      uint64_t _lastStallWarnUS;

      // Opens and sets up one stream. Returns true on success.
      bool setupStream(snd_pcm_t* pcm, bool playback, unsigned int& rate, snd_pcm_uframes_t& period,
                       unsigned int& channels, snd_pcm_format_t& format);
      bool readPeriod();
      void writePeriod();
      void runLoop();
      // Called with the result of a failed read or write. Counts it if it was an xrun.
      void checkXrun(snd_pcm_sframes_t err);
      // Prints a warning, at most once every few seconds, that the device did not deliver or accept data in time.
      void warnStall(const char* what);
      // Stops, prepares, fills with silence and starts the streams. Used at startup and to restart after an error.
      // It never blocks. Returns false if the playback device cannot be prepared.
      bool startStreams();
      // For use only by the audio thread.
      void setCriticalVariables(unsigned segmentSize);
      static void* audioLoop(void* arg);

  public:
      // Time in microseconds at which the driver was created.
      uint64_t _start_timeUS;

      QList<MuseAlsaAudioPort*> outputPortsList;
      QList<MuseAlsaAudioPort*> inputPortsList;

      AlsaAudioDevice();
      virtual ~AlsaAudioDevice();

      // Opens the pcm device(s) set in the configuration, and sets MusEGlobal::sampleRate and
      //  MusEGlobal::segmentSize to what the device actually accepted. Must be called before any ports are registered.
      // Returns true on success.
      bool open();
      void close();

      virtual const char* driverName() const { return "AlsaAudioDevice"; }
      virtual inline int deviceType() const { return ALSA_AUDIO; }

      virtual bool start(int priority);
      virtual void stop();

      virtual unsigned framePos() const {
            // Not much choice but to do this:
            return framesAtCycleStart() + framesSinceCycleStart();
            }

      // These are meant to be called from inside process thread only.
      virtual unsigned framesAtCycleStart() const { return _framesAtCycleStart[_criticalVariablesIdx]; }
      virtual unsigned framesSinceCycleStart() const
      {
        const uint64_t ct = systemTimeUS();
        // Do not round up here since time resolution is higher than (audio) frame resolution.
        unsigned f = muse_multiply_64_div_64_to_64(ct - _timeUSAtCycleStart[_criticalVariablesIdx], MusEGlobal::sampleRate, 1000000UL);

        // Safety due to inaccuracies. It cannot be after the segment, right?
        if(f >= MusEGlobal::segmentSize)
          f = MusEGlobal::segmentSize - 1;
        return f;
      }

      virtual unsigned frameTime() const { return _frameCounter[_criticalVariablesIdx]; }

      virtual float* getBuffer(void* port, unsigned long nframes)
      {
        if (nframes > MusEGlobal::segmentSize) {
          fprintf(stderr, "AlsaAudioDevice::getBuffer nframes > segment size\n");
          exit(-1);
        }
        return ((MuseAlsaAudioPort*)port)->buffer;
      }

      virtual std::list<QString> outputPorts(bool midi = false, int aliases = -1);
      virtual std::list<QString> inputPorts(bool midi = false, int aliases = -1);

      virtual void registerClient() {}
      virtual const char* clientName() { return "Alsa"; }

      virtual void* registerOutPort(const char* name, bool midi);
      virtual void* registerInPort(const char* name, bool midi);

      virtual AudioDevice::PortType portType(void*) const { return AudioPort; }
      virtual AudioDevice::PortDirection portDirection(void*) const { return OutputPort; }
      virtual void unregisterPort(void*) {}

      // There is no port routing with this driver. The first two output ports go to the first two
      //  playback channels, and the first two input ports come from the first two capture channels.
      virtual bool connect(void*, void*) { return false; }
      virtual bool connect(const char*, const char*) { return false; }
      virtual bool disconnect(void*, void*) { return false; }
      virtual bool disconnect(const char*, const char*) { return false; }
      // Always return nonzero, like the other drivers without routing.
      virtual int connections(void*) { return 1; }
      virtual bool portConnectedTo(void*, const char*) { return false; }
      virtual bool portsCanDisconnect(void*, void*) const { return false; }
      virtual bool portsCanDisconnect(const char*, const char*) const { return false; }
      virtual bool portsCanConnect(void*, void*) const { return false; }
      virtual bool portsCanConnect(const char*, const char*) const { return false; }
      virtual bool portsCompatible(void*, void*) const { return false; }
      virtual bool portsCompatible(const char*, const char*) const { return false; }
      virtual void setPortName(void*, const char*) {}
      virtual void* findPort(const char*) { return nullptr; }
      // preferred_name_or_alias: -1: No preference 0: Prefer canonical name 1: Prefer 1st alias 2: Prefer 2nd alias.
      virtual char* portName(void*, char* str, int str_size, int /*preferred_name_or_alias*/ = -1)
      {
        if(str_size == 0)
          return nullptr;
        str[0] = '\0';
        return str;
      }
      virtual const char* canonicalPortName(void*) { return nullptr; }
      virtual unsigned int portLatency(void*, bool /*capture*/) const { return 0; }

      virtual bool countsXruns() const { return true; }
      virtual bool isRealtime() { return MusEGlobal::realTimeScheduling; }
      virtual int realtimePriority() const { return 40; }

      virtual void setFreewheel(bool) {}
      virtual int setMaster(bool, bool /*unconditional*/ = false) { return 1; }
};

} // namespace MusECore

#endif
