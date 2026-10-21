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

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/utils/dict.h>

#include <algorithm>
#include <string.h>
#include <stdlib.h>

#include "pipewiregraph.h"

namespace MusECore {

static const char* lookup(const struct spa_dict* props, const char* key)
{
  return props ? spa_dict_lookup(props, key) : nullptr;
}

static bool isTrue(const char* v)
{
  return v && (strcmp(v, "true") == 0 || strcmp(v, "1") == 0);
}

// Gets the value of the "name" key from a small json object like {"name":"alsa_output.foo"}.
static std::string jsonName(const char* json)
{
  if(!json)
    return std::string();
  const char* k = strstr(json, "\"name\"");
  if(!k)
    return std::string();
  const char* c = strchr(k + 6, ':');
  if(!c)
    return std::string();
  const char* q1 = strchr(c, '"');
  if(!q1)
    return std::string();
  const char* q2 = strchr(q1 + 1, '"');
  if(!q2)
    return std::string();
  return std::string(q1 + 1, q2 - q1 - 1);
}

PipeWireGraph::PipeWireGraph()
{
  _loop = nullptr;
  _core = nullptr;
  _registry = nullptr;
  _initialized = false;
  _metadata = nullptr;
  _metadataId = 0;
  _syncSeq = 0;
  _syncDone = false;
  memset(&_registryListener, 0, sizeof(_registryListener));
  memset(&_coreListener, 0, sizeof(_coreListener));
  memset(&_metadataListener, 0, sizeof(_metadataListener));
}

PipeWireGraph::~PipeWireGraph()
{
}

//---------------------------------------------------------
//   Callbacks. These run in the thread loop thread.
//---------------------------------------------------------

void PipeWireGraph::registryGlobal(void* data, uint32_t id, uint32_t /*permissions*/, const char* type,
                                   uint32_t /*version*/, const struct spa_dict* props)
{
  PipeWireGraph* g = static_cast<PipeWireGraph*>(data);
  if(!type)
    return;

  if(strcmp(type, PW_TYPE_INTERFACE_Node) == 0)
  {
    PwGraphNode n;
    n.id = id;
    const char* name = lookup(props, PW_KEY_NODE_NAME);
    n.name = name ? name : "";
    const char* mc = lookup(props, PW_KEY_MEDIA_CLASS);
    n.mediaClass = mc ? mc : "";
    std::lock_guard<std::mutex> lock(g->_mutex);
    g->_nodes[id] = n;
  }
  else if(strcmp(type, PW_TYPE_INTERFACE_Port) == 0)
  {
    PwGraphPort p;
    p.id = id;
    const char* node_id = lookup(props, PW_KEY_NODE_ID);
    p.nodeId = node_id ? (uint32_t)strtoul(node_id, nullptr, 10) : SPA_ID_INVALID;
    const char* name = lookup(props, PW_KEY_PORT_NAME);
    p.name = name ? name : "";
    const char* dir = lookup(props, PW_KEY_PORT_DIRECTION);
    p.isInput = dir && strcmp(dir, "in") == 0;
    const char* fmt = lookup(props, PW_KEY_FORMAT_DSP);
    p.isAudio = fmt && strstr(fmt, "audio") != nullptr;
    p.isMidi = fmt && strstr(fmt, "midi") != nullptr;
    p.physical = isTrue(lookup(props, PW_KEY_PORT_PHYSICAL));
    p.terminal = isTrue(lookup(props, PW_KEY_PORT_TERMINAL));
    const char* ch = lookup(props, PW_KEY_AUDIO_CHANNEL);
    p.channel = ch ? ch : "";
    std::lock_guard<std::mutex> lock(g->_mutex);
    g->_ports[id] = p;
  }
  else if(strcmp(type, PW_TYPE_INTERFACE_Link) == 0)
  {
    PwGraphLink l;
    l.id = id;
    const char* o = lookup(props, PW_KEY_LINK_OUTPUT_PORT);
    const char* i = lookup(props, PW_KEY_LINK_INPUT_PORT);
    l.outPort = o ? (uint32_t)strtoul(o, nullptr, 10) : SPA_ID_INVALID;
    l.inPort = i ? (uint32_t)strtoul(i, nullptr, 10) : SPA_ID_INVALID;
    std::lock_guard<std::mutex> lock(g->_mutex);
    g->_links[id] = l;
  }
  else if(strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0)
  {
    // The metadata called "default" holds the default sink and source.
    const char* name = lookup(props, "metadata.name");
    if(name && strcmp(name, "default") == 0 && !g->_metadata)
    {
      g->_metadata = static_cast<pw_metadata*>(pw_registry_bind(g->_registry, id, type, PW_VERSION_METADATA, 0));
      if(g->_metadata)
      {
        g->_metadataId = id;
        static pw_metadata_events events;
        events.version = PW_VERSION_METADATA_EVENTS;
        events.property = &PipeWireGraph::metadataProperty;
        pw_metadata_add_listener(g->_metadata, &g->_metadataListener, &events, g);
      }
    }
  }
}

void PipeWireGraph::registryGlobalRemove(void* data, uint32_t id)
{
  PipeWireGraph* g = static_cast<PipeWireGraph*>(data);
  std::lock_guard<std::mutex> lock(g->_mutex);
  g->_nodes.erase(id);
  g->_ports.erase(id);
  g->_links.erase(id);
}

void PipeWireGraph::coreDone(void* data, uint32_t id, int seq)
{
  PipeWireGraph* g = static_cast<PipeWireGraph*>(data);
  if(id == PW_ID_CORE && seq == g->_syncSeq)
  {
    g->_syncDone = true;
    pw_thread_loop_signal(g->_loop, false);
  }
}

int PipeWireGraph::metadataProperty(void* data, uint32_t /*subject*/, const char* key, const char* /*type*/, const char* value)
{
  PipeWireGraph* g = static_cast<PipeWireGraph*>(data);
  if(!key)
    return 0;
  std::lock_guard<std::mutex> lock(g->_mutex);
  if(strcmp(key, "default.audio.sink") == 0)
    g->_defaultSink = jsonName(value);
  else if(strcmp(key, "default.audio.source") == 0)
    g->_defaultSource = jsonName(value);
  return 0;
}

//---------------------------------------------------------
//   init
//---------------------------------------------------------

bool PipeWireGraph::init(pw_thread_loop* loop, pw_core* core)
{
  if(_initialized)
    return true;
  _loop = loop;
  _core = core;

  static pw_core_events core_events;
  core_events.version = PW_VERSION_CORE_EVENTS;
  core_events.done = &PipeWireGraph::coreDone;
  pw_core_add_listener(_core, &_coreListener, &core_events, this);

  _registry = pw_core_get_registry(_core, PW_VERSION_REGISTRY, 0);
  if(!_registry)
  {
    spa_hook_remove(&_coreListener);
    return false;
  }

  static pw_registry_events reg_events;
  reg_events.version = PW_VERSION_REGISTRY_EVENTS;
  reg_events.global = &PipeWireGraph::registryGlobal;
  reg_events.global_remove = &PipeWireGraph::registryGlobalRemove;
  pw_registry_add_listener(_registry, &_registryListener, &reg_events, this);

  _initialized = true;
  return true;
}

void PipeWireGraph::shutdown()
{
  if(!_initialized)
    return;
  if(_metadata)
  {
    spa_hook_remove(&_metadataListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_metadata));
    _metadata = nullptr;
  }
  spa_hook_remove(&_registryListener);
  pw_proxy_destroy(reinterpret_cast<pw_proxy*>(_registry));
  _registry = nullptr;
  spa_hook_remove(&_coreListener);
  _initialized = false;
}

bool PipeWireGraph::roundtrip(int timeoutSec)
{
  if(!_initialized)
    return false;
  pw_thread_loop_lock(_loop);
  _syncDone = false;
  _syncSeq = pw_core_sync(_core, PW_ID_CORE, _syncSeq);
  bool ok = true;
  while(!_syncDone)
  {
    if(pw_thread_loop_timed_wait(_loop, timeoutSec) != 0)
    {
      ok = false;
      break;
    }
  }
  pw_thread_loop_unlock(_loop);
  return ok;
}

//---------------------------------------------------------
//   Queries
//---------------------------------------------------------

std::string PipeWireGraph::portFullName(const PwGraphPort& p) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  std::map<uint32_t, PwGraphNode>::const_iterator in = _nodes.find(p.nodeId);
  return (in != _nodes.end() ? in->second.name : std::string()) + ":" + p.name;
}

bool PipeWireGraph::findPort(const std::string& fullName, PwGraphPort& port) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  for(std::map<uint32_t, PwGraphPort>::const_iterator ip = _ports.begin(); ip != _ports.end(); ++ip)
  {
    std::map<uint32_t, PwGraphNode>::const_iterator in = _nodes.find(ip->second.nodeId);
    if(in == _nodes.end())
      continue;
    if(in->second.name + ":" + ip->second.name == fullName)
    {
      port = ip->second;
      return true;
    }
  }
  return false;
}

bool PipeWireGraph::getPort(uint32_t id, PwGraphPort& port) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  std::map<uint32_t, PwGraphPort>::const_iterator ip = _ports.find(id);
  if(ip == _ports.end())
    return false;
  port = ip->second;
  return true;
}

bool PipeWireGraph::getNode(uint32_t id, PwGraphNode& node) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  std::map<uint32_t, PwGraphNode>::const_iterator in = _nodes.find(id);
  if(in == _nodes.end())
    return false;
  node = in->second;
  return true;
}

std::vector<std::string> PipeWireGraph::portNames(bool midi, bool input, uint32_t excludeNodeId) const
{
  std::vector<std::string> physical;
  std::vector<std::string> others;
  std::lock_guard<std::mutex> lock(_mutex);
  for(std::map<uint32_t, PwGraphPort>::const_iterator ip = _ports.begin(); ip != _ports.end(); ++ip)
  {
    const PwGraphPort& p = ip->second;
    if(p.isInput != input || p.nodeId == excludeNodeId)
      continue;
    if(midi ? !p.isMidi : !p.isAudio)
      continue;
    std::map<uint32_t, PwGraphNode>::const_iterator in = _nodes.find(p.nodeId);
    if(in == _nodes.end())
      continue;
    const std::string name = in->second.name + ":" + p.name;
    // Put the ports of hardware devices first.
    if(p.physical)
      physical.push_back(name);
    else
      others.push_back(name);
  }
  physical.insert(physical.end(), others.begin(), others.end());
  return physical;
}

bool PipeWireGraph::linkExists(uint32_t outPort, uint32_t inPort, uint32_t* linkId) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  for(std::map<uint32_t, PwGraphLink>::const_iterator il = _links.begin(); il != _links.end(); ++il)
  {
    if(il->second.outPort == outPort && il->second.inPort == inPort)
    {
      if(linkId)
        *linkId = il->first;
      return true;
    }
  }
  return false;
}

std::vector<uint32_t> PipeWireGraph::linkedPorts(uint32_t port) const
{
  std::vector<uint32_t> ret;
  std::lock_guard<std::mutex> lock(_mutex);
  for(std::map<uint32_t, PwGraphLink>::const_iterator il = _links.begin(); il != _links.end(); ++il)
  {
    if(il->second.outPort == port)
      ret.push_back(il->second.inPort);
    else if(il->second.inPort == port)
      ret.push_back(il->second.outPort);
  }
  return ret;
}

static bool channelLess(const PwGraphPort& a, const PwGraphPort& b)
{
  // Left before right before the rest, then by name.
  const int ra = a.channel == "FL" ? 0 : (a.channel == "FR" ? 1 : 2);
  const int rb = b.channel == "FL" ? 0 : (b.channel == "FR" ? 1 : 2);
  if(ra != rb)
    return ra < rb;
  return a.name < b.name;
}

std::vector<PwGraphPort> PipeWireGraph::defaultDevicePorts(bool sink) const
{
  std::vector<PwGraphPort> ret;
  std::lock_guard<std::mutex> lock(_mutex);

  const std::string& wanted_name = sink ? _defaultSink : _defaultSource;
  const char* media_class = sink ? "Audio/Sink" : "Audio/Source";

  uint32_t node_id = SPA_ID_INVALID;
  for(std::map<uint32_t, PwGraphNode>::const_iterator in = _nodes.begin(); in != _nodes.end(); ++in)
  {
    if(!wanted_name.empty() && in->second.name == wanted_name)
    {
      node_id = in->first;
      break;
    }
  }
  // No default known? Use the first one of the right kind.
  if(node_id == SPA_ID_INVALID)
  {
    for(std::map<uint32_t, PwGraphNode>::const_iterator in = _nodes.begin(); in != _nodes.end(); ++in)
    {
      if(in->second.mediaClass == media_class)
      {
        node_id = in->first;
        break;
      }
    }
  }
  if(node_id == SPA_ID_INVALID)
    return ret;

  for(std::map<uint32_t, PwGraphPort>::const_iterator ip = _ports.begin(); ip != _ports.end(); ++ip)
  {
    const PwGraphPort& p = ip->second;
    // A sink is played to, so we want its input ports. A source is recorded from, so we want its output ports.
    if(p.nodeId == node_id && p.isAudio && p.isInput == sink)
    {
      // The monitor ports of a sink are outputs, so they are not included here.
      ret.push_back(p);
    }
  }
  std::sort(ret.begin(), ret.end(), channelLess);
  return ret;
}

bool PipeWireGraph::findNodePort(uint32_t nodeId, const std::string& portName, PwGraphPort& port) const
{
  std::lock_guard<std::mutex> lock(_mutex);
  for(std::map<uint32_t, PwGraphPort>::const_iterator ip = _ports.begin(); ip != _ports.end(); ++ip)
  {
    if(ip->second.nodeId == nodeId && ip->second.name == portName)
    {
      port = ip->second;
      return true;
    }
  }
  return false;
}

//---------------------------------------------------------
//   Links
//---------------------------------------------------------

bool PipeWireGraph::createLink(uint32_t outPort, uint32_t inPort)
{
  if(!_initialized)
    return false;
  char o[32], i[32];
  snprintf(o, sizeof(o), "%u", outPort);
  snprintf(i, sizeof(i), "%u", inPort);
  pw_properties* props = pw_properties_new(PW_KEY_LINK_OUTPUT_PORT, o, PW_KEY_LINK_INPUT_PORT, i, nullptr);

  pw_thread_loop_lock(_loop);
  void* proxy = pw_core_create_object(_core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &props->dict, 0);
  pw_thread_loop_unlock(_loop);
  pw_properties_free(props);
  return proxy != nullptr;
}

bool PipeWireGraph::destroyLink(uint32_t outPort, uint32_t inPort)
{
  if(!_initialized)
    return false;
  uint32_t link_id;
  if(!linkExists(outPort, inPort, &link_id))
    return false;
  pw_thread_loop_lock(_loop);
  const int res = pw_registry_destroy(_registry, link_id);
  pw_thread_loop_unlock(_loop);
  return res >= 0;
}

} // namespace MusECore
