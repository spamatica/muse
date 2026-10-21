//=========================================================
//  MusE
//  Linux Music Editor
//
//  Direct PipeWire audio backend.
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

#ifndef __PIPEWIREAUDIO_H__
#define __PIPEWIREAUDIO_H__

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>

#include <atomic>
#include <list>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include <QString>

#include "audiodev.h"
#include "globals.h"
#include "large_int.h"
#include "pipewiregraph.h"

namespace MusECore {

//---------------------------------------------------------
//   PwPortHandle
//   What the void* port pointers of the AudioDevice interface point to.
//   Both for our own ports and for the ports of other nodes.
//---------------------------------------------------------

struct PwPortHandle {
  bool own;
  // The direction as seen from the port's own node. For one of our ports, an input port receives audio from the graph.
  bool isInput;
  bool isAudio;
  // The "node:port" name.
  std::string fullName;
  // The global object id. For our own ports it is not known until PipeWire has told us about the port.
  uint32_t id;

  // The following are only for our own ports.
  std::string name;
  void* pwPort;                // The port of the pw_filter.
  float* buffer;               // The MusE side buffer, one segment long.
  // For when the graph runs with another cycle size than the one MusE uses. A buffer of the audio in
  //  (inputs) or out (outputs) that is waiting to be used.
  std::vector<float> ring;
  size_t ringRead;
  size_t ringFill;
  bool ringPrimed;
};

//---------------------------------------------------------
//   PipeWireAudioDevice
//   Registers one node with PipeWire, with one mono float port for each port that MusE registers.
//   The node's process callback runs the MusE process cycle. The other nodes' ports can be linked
//    to ours, in the MusE routing menus or in a patchbay program.
//---------------------------------------------------------

class PipeWireAudioDevice : public AudioDevice {
  private:
      pw_thread_loop* _loop;
      pw_context* _context;
      pw_core* _core;
      pw_filter* _filter;
      spa_hook _filterListener;
      // Mutable so that the const port queries can wait for the graph to catch up.
      mutable PipeWireGraph _graph;
      bool _pwInitialized;

      // Set by the process callback the first time it runs.
      std::atomic<bool> _gotFirstCycle;
      unsigned int _probeRate;
      unsigned int _probeQuantum;
      int _rtPriority;
      // The node id is only known after the node has been created.
      uint32_t _nodeId;

      // Whether the process callback should run the MusE cycle.
      std::atomic<bool> _active;
      std::atomic<bool> _rateMismatchWarned;

      // Protects the lists of our own ports. The process callback holds it while it runs.
      std::recursive_mutex _portMutex;
      std::list<PwPortHandle*> _outPorts;
      std::list<PwPortHandle*> _inPorts;

      // Handles for the ports of other nodes. They are kept until we shut down.
      std::mutex _extMutex;
      std::map<uint32_t, PwPortHandle*> _extHandles;

      // Adapter state for when the graph cycle size is not the MusE segment size.
      unsigned int _adapterQuantum;
      size_t _adapterPendingIn;

      // Critical variables that need to all update at once.
      // We employ a 'flipping' technique.
      unsigned _framesAtCycleStart[2];
      uint64_t _timeUSAtCycleStart[2];
      unsigned _frameCounter[2];
      unsigned _criticalVariablesIdx;
      bool _firstCycle;

      static void onProcess(void* data, struct spa_io_position* position);
      void process(struct spa_io_position* position);
      void runCycle();
      void processDirect(uint32_t n);
      void processAdapted(uint32_t n);
      void silenceOutputs(uint32_t n);
      void setCriticalVariables(unsigned segmentSize);

      // Makes a handle for an external port. Returns nullptr if it is not a port we can use.
      PwPortHandle* externalHandle(const PwGraphPort& p);
      // Fills in the id of one of our own ports. Waits for PipeWire to tell us if necessary.
      bool resolveHandleId(const PwPortHandle* h, uint32_t& id, bool wait) const;
      bool resolveName(const char* name, PwGraphPort& p, bool wait) const;
      PwPortHandle* addPort(const char* name, bool input);
      void autoConnect(PwPortHandle* h, int index);
      void cleanup();

  public:
      // Time in microseconds at which the driver was created.
      uint64_t _start_timeUS;

      PipeWireAudioDevice();
      virtual ~PipeWireAudioDevice();

      // Connects to PipeWire, and sets MusEGlobal::sampleRate and MusEGlobal::segmentSize.
      // Must be called before any ports are registered. Returns true on success.
      bool open();

      virtual const char* driverName() const { return "PipeWireAudioDevice"; }
      virtual inline int deviceType() const { return PIPEWIRE_AUDIO; }

      virtual bool start(int priority);
      virtual void stop();

      virtual unsigned framePos() const { return framesAtCycleStart() + framesSinceCycleStart(); }
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

      virtual float* getBuffer(void* port, unsigned long nframes);

      virtual std::list<QString> outputPorts(bool midi = false, int aliases = -1);
      virtual std::list<QString> inputPorts(bool midi = false, int aliases = -1);

      virtual void registerClient() {}
      virtual const char* clientName() { return "MusE"; }

      virtual void* registerOutPort(const char* name, bool midi);
      virtual void* registerInPort(const char* name, bool midi);
      virtual void unregisterPort(void* port);

      virtual PortType portType(void* port) const;
      virtual PortDirection portDirection(void* port) const;

      virtual bool connect(void* src, void* dst);
      virtual bool connect(const char* src, const char* dst);
      virtual bool disconnect(void* src, void* dst);
      virtual bool disconnect(const char* src, const char* dst);
      virtual int connections(void* port);
      virtual bool portConnectedTo(void* ourPort, const char* port);
      virtual bool portsCanDisconnect(void* src, void* dst) const;
      virtual bool portsCanDisconnect(const char* src, const char* dst) const;
      virtual bool portsCanConnect(void* src, void* dst) const;
      virtual bool portsCanConnect(const char* src, const char* dst) const;
      virtual bool portsCompatible(void* src, void* dst) const;
      virtual bool portsCompatible(const char* src, const char* dst) const;
      virtual void setPortName(void* port, const char* name);
      virtual void* findPort(const char* name);
      virtual char* portName(void* port, char* str, int str_size, int preferred_name_or_alias = -1);
      virtual const char* canonicalPortName(void* port);
      virtual unsigned int portLatency(void*, bool /*capture*/) const { return 0; }

      virtual bool isRealtime() { return _rtPriority > 0; }
      virtual int realtimePriority() const { return _rtPriority; }

      virtual void setFreewheel(bool) {}
      virtual int setMaster(bool, bool /*unconditional*/ = false) { return 1; }
};

} // namespace MusECore

#endif
