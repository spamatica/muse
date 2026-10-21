//=========================================================
//  MusE
//  Linux Music Editor
//
//  Keeps track of the objects (nodes, ports, links) of a PipeWire graph
//   and makes and breaks links. Used by the PipeWire audio backend.
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

#ifndef __PIPEWIREGRAPH_H__
#define __PIPEWIREGRAPH_H__

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>

#include <stdint.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace MusECore {

struct PwGraphNode {
  uint32_t id;
  std::string name;
  std::string mediaClass;
};

struct PwGraphPort {
  uint32_t id;
  uint32_t nodeId;
  std::string name;
  bool isInput;      // Direction as seen from the port's own node.
  bool isAudio;
  bool isMidi;
  bool physical;
  bool terminal;
  std::string channel; // The audio channel, for example FL.
};

struct PwGraphLink {
  uint32_t id;
  uint32_t outPort;
  uint32_t inPort;
};

//---------------------------------------------------------
//   PipeWireGraph
//   The query functions are thread safe. The functions that
//    talk to PipeWire lock the thread loop themselves, so
//    they must not be called from the thread loop thread.
//---------------------------------------------------------

class PipeWireGraph {
  private:
      pw_thread_loop* _loop;
      pw_core* _core;
      pw_registry* _registry;
      spa_hook _registryListener;
      spa_hook _coreListener;
      bool _initialized;

      pw_metadata* _metadata;
      spa_hook _metadataListener;
      uint32_t _metadataId;

      // For waiting on a round trip to the server.
      int _syncSeq;
      bool _syncDone;

      mutable std::mutex _mutex;
      std::map<uint32_t, PwGraphNode> _nodes;
      std::map<uint32_t, PwGraphPort> _ports;
      std::map<uint32_t, PwGraphLink> _links;
      std::string _defaultSink;
      std::string _defaultSource;

      static void registryGlobal(void* data, uint32_t id, uint32_t permissions, const char* type,
                                 uint32_t version, const struct spa_dict* props);
      static void registryGlobalRemove(void* data, uint32_t id);
      static void coreDone(void* data, uint32_t id, int seq);
      static int metadataProperty(void* data, uint32_t subject, const char* key, const char* type, const char* value);

  public:
      PipeWireGraph();
      ~PipeWireGraph();

      // Starts listening. The thread loop must be running and the caller must hold its lock.
      // Does not wait for the initial objects, call roundtrip() for that.
      bool init(pw_thread_loop* loop, pw_core* core);
      // Stops listening. The caller must hold the thread loop lock.
      void shutdown();

      // Waits until the server has processed everything sent so far, so that the graph is up to date.
      // The caller must NOT hold the thread loop lock. Returns false on a timeout.
      bool roundtrip(int timeoutSec = 3);

      // The "node:port" name of a port.
      std::string portFullName(const PwGraphPort& p) const;
      // Finds a port by its "node:port" name.
      bool findPort(const std::string& fullName, PwGraphPort& port) const;
      bool getPort(uint32_t id, PwGraphPort& port) const;
      bool getNode(uint32_t id, PwGraphNode& node) const;
      // Full names of the audio or midi ports with the given direction as seen from their own nodes.
      // Physical (hardware) ports come first. Ports of the node excludeNodeId are not included.
      std::vector<std::string> portNames(bool midi, bool input, uint32_t excludeNodeId) const;
      // Whether the two ports are linked, and the id of the link.
      bool linkExists(uint32_t outPort, uint32_t inPort, uint32_t* linkId = nullptr) const;
      // The ids of the ports that the port is linked to.
      std::vector<uint32_t> linkedPorts(uint32_t port) const;
      // The input (sink) or output (source) audio ports of the default sink or source, in channel order.
      std::vector<PwGraphPort> defaultDevicePorts(bool sink) const;
      // Finds the port with the given name on the given node.
      bool findNodePort(uint32_t nodeId, const std::string& portName, PwGraphPort& port) const;

      // Make or break a link. Return true if the request was sent.
      bool createLink(uint32_t outPort, uint32_t inPort);
      bool destroyLink(uint32_t outPort, uint32_t inPort);
};

} // namespace MusECore

#endif
