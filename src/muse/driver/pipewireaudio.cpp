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

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdint.h>

#include <spa/utils/result.h>

#include "audio.h"
#include "audiodev.h"
#include "globals.h"
#include "gconfig.h"
#include "al/al.h"
#include "strntcpy.h"
#include "pipewireaudio.h"

namespace MusECore {

PipeWireAudioDevice* pipeWireAudioDevice = nullptr;

// The name of our node. The names of our ports in the graph are this followed by ':' and the port name.
static const char* const pwNodeName = "MusE";

// The room, in samples, for the audio that waits when the graph runs with another cycle size than MusE does.
static size_t ringCapacity()
{
  return 2 * (size_t)MusEGlobal::segmentSize + 32768;
}

static void ringPush(PwPortHandle* h, const float* src, size_t count)
{
  const size_t cap = h->ring.size();
  if(count > cap)
    count = cap;
  if(h->ringFill + count > cap)
  {
    // Overflow. Something is wrong, start over.
    h->ringRead = 0;
    h->ringFill = 0;
  }
  size_t w = (h->ringRead + h->ringFill) % cap;
  for(size_t i = 0; i < count; ++i)
  {
    h->ring[w] = src ? src[i] : 0.0f;
    if(++w == cap)
      w = 0;
  }
  h->ringFill += count;
}

// Pops count samples. If there are not enough the rest is silence.
static void ringPop(PwPortHandle* h, float* dst, size_t count)
{
  const size_t cap = h->ring.size();
  const size_t avail = count < h->ringFill ? count : h->ringFill;
  size_t r = h->ringRead;
  for(size_t i = 0; i < avail; ++i)
  {
    if(dst)
      dst[i] = h->ring[r];
    if(++r == cap)
      r = 0;
  }
  if(dst)
    for(size_t i = avail; i < count; ++i)
      dst[i] = 0.0f;
  h->ringRead = r;
  h->ringFill -= avail;
}

//---------------------------------------------------------
//   PipeWireAudioDevice
//---------------------------------------------------------

PipeWireAudioDevice::PipeWireAudioDevice() : AudioDevice()
{
  _loop = nullptr;
  _context = nullptr;
  _core = nullptr;
  _filter = nullptr;
  memset(&_filterListener, 0, sizeof(_filterListener));
  _pwInitialized = false;
  _gotFirstCycle = false;
  _probeRate = 0;
  _probeQuantum = 0;
  _rtPriority = 0;
  _nodeId = SPA_ID_INVALID;
  _active = false;
  _rateMismatchWarned = false;
  _adapterQuantum = 0;
  _adapterPendingIn = 0;
  _start_timeUS = systemTimeUS();
  _criticalVariablesIdx = 0;
  _firstCycle = true;
  for(unsigned x = 0; x < 2; ++x)
  {
    _timeUSAtCycleStart[x] = 0;
    _framesAtCycleStart[x] = 0;
    _frameCounter[x] = 0;
  }
}

PipeWireAudioDevice::~PipeWireAudioDevice()
{
  cleanup();
}

void PipeWireAudioDevice::setCriticalVariables(unsigned segmentSize)
{
  const unsigned idx = (_criticalVariablesIdx + 1) % 2;
  _timeUSAtCycleStart[idx] = systemTimeUS();
  // Let these start at zero and only increment on subsequent cycles.
  if(!_firstCycle)
  {
    _framesAtCycleStart[idx] = _framesAtCycleStart[_criticalVariablesIdx] + segmentSize;
    _frameCounter[idx] = _frameCounter[_criticalVariablesIdx] + segmentSize;
  }
  _firstCycle = false;
  // Now 'flip' the variables all at once.
  _criticalVariablesIdx = idx;
}

//---------------------------------------------------------
//   Processing. This runs in the PipeWire data thread.
//---------------------------------------------------------

void PipeWireAudioDevice::onProcess(void* data, struct spa_io_position* position)
{
  static_cast<PipeWireAudioDevice*>(data)->process(position);
}

void PipeWireAudioDevice::silenceOutputs(uint32_t n)
{
  // Take the buffers of the input ports too, even if we do not use them. PipeWire expects each cycle's buffers to be taken.
  for(std::list<PwPortHandle*>::iterator i = _inPorts.begin(); i != _inPorts.end(); ++i)
    pw_filter_get_dsp_buffer((*i)->pwPort, n);
  for(std::list<PwPortHandle*>::iterator i = _outPorts.begin(); i != _outPorts.end(); ++i)
  {
    float* out = static_cast<float*>(pw_filter_get_dsp_buffer((*i)->pwPort, n));
    if(out)
      memset(out, 0, n * sizeof(float));
  }
}

void PipeWireAudioDevice::runCycle()
{
  setCriticalVariables(MusEGlobal::segmentSize);

  if(MusEGlobal::audio->isRunning())
  {
    // Use our built-in transport, which INCLUDES the necessary
    //  calls to Audio::sync() and ultimately Audio::process(),
    //  and increments the built-in play position.
    processTransport(MusEGlobal::segmentSize);
  }
  else
  {
    // Nothing is processing. Do not send stale data.
    for(std::list<PwPortHandle*>::iterator i = _outPorts.begin(); i != _outPorts.end(); ++i)
      memset((*i)->buffer, 0, MusEGlobal::segmentSize * sizeof(float));
  }
}

// The graph cycle is the same size as ours.
void PipeWireAudioDevice::processDirect(uint32_t n)
{
  for(std::list<PwPortHandle*>::iterator i = _inPorts.begin(); i != _inPorts.end(); ++i)
  {
    const float* in = static_cast<const float*>(pw_filter_get_dsp_buffer((*i)->pwPort, n));
    if(in)
      memcpy((*i)->buffer, in, n * sizeof(float));
    else
      memset((*i)->buffer, 0, n * sizeof(float));
  }

  runCycle();

  for(std::list<PwPortHandle*>::iterator i = _outPorts.begin(); i != _outPorts.end(); ++i)
  {
    float* out = static_cast<float*>(pw_filter_get_dsp_buffer((*i)->pwPort, n));
    if(out)
      memcpy(out, (*i)->buffer, n * sizeof(float));
  }
}

// The graph cycle is not the same size as ours. Collect the input until there is a whole MusE cycle of it,
//  and keep the output of the MusE cycles until the graph asks for it. This costs some extra latency.
void PipeWireAudioDevice::processAdapted(uint32_t n)
{
  const size_t seg = MusEGlobal::segmentSize;

  if(_adapterQuantum != n)
  {
    // The cycle size changed, start over.
    _adapterQuantum = n;
    _adapterPendingIn = 0;
    for(std::list<PwPortHandle*>::iterator i = _inPorts.begin(); i != _inPorts.end(); ++i)
    {
      (*i)->ringRead = 0;
      (*i)->ringFill = 0;
    }
    for(std::list<PwPortHandle*>::iterator i = _outPorts.begin(); i != _outPorts.end(); ++i)
    {
      (*i)->ringRead = 0;
      (*i)->ringFill = 0;
      (*i)->ringPrimed = false;
    }
  }

  for(std::list<PwPortHandle*>::iterator i = _inPorts.begin(); i != _inPorts.end(); ++i)
    ringPush(*i, static_cast<const float*>(pw_filter_get_dsp_buffer((*i)->pwPort, n)), n);
  _adapterPendingIn += n;

  while(_adapterPendingIn >= seg)
  {
    _adapterPendingIn -= seg;

    for(std::list<PwPortHandle*>::iterator i = _inPorts.begin(); i != _inPorts.end(); ++i)
      ringPop(*i, (*i)->buffer, seg);

    runCycle();

    for(std::list<PwPortHandle*>::iterator i = _outPorts.begin(); i != _outPorts.end(); ++i)
    {
      // The first time, start with some silence so that there is something to give while we wait for a whole cycle.
      if(!(*i)->ringPrimed)
      {
        ringPush(*i, nullptr, seg);
        (*i)->ringPrimed = true;
      }
      ringPush(*i, (*i)->buffer, seg);
    }
  }

  for(std::list<PwPortHandle*>::iterator i = _outPorts.begin(); i != _outPorts.end(); ++i)
  {
    if(!(*i)->ringPrimed)
    {
      ringPush(*i, nullptr, seg);
      (*i)->ringPrimed = true;
    }
    float* out = static_cast<float*>(pw_filter_get_dsp_buffer((*i)->pwPort, n));
    ringPop(*i, out, n);
  }
}

void PipeWireAudioDevice::process(struct spa_io_position* position)
{
  const uint32_t n = position->clock.duration;
  const unsigned int rate = position->clock.rate.denom;

  if(!_gotFirstCycle.load())
  {
    _probeRate = rate;
    _probeQuantum = n;
    struct sched_param sp;
    int policy;
    if(pthread_getschedparam(pthread_self(), &policy, &sp) == 0 && (policy == SCHED_FIFO || policy == SCHED_RR))
      _rtPriority = sp.sched_priority;
    _gotFirstCycle = true;
    pw_thread_loop_signal(_loop, false);
  }

  // If someone else is changing the ports, skip this cycle.
  std::unique_lock<std::recursive_mutex> lock(_portMutex, std::try_to_lock);
  if(!lock.owns_lock())
    return;

  if(!_active.load())
  {
    silenceOutputs(n);
    return;
  }

  if((int)rate != MusEGlobal::sampleRate)
  {
    // We cannot change MusE's samplerate while it is running.
    if(!_rateMismatchWarned.exchange(true))
      fprintf(stderr, "Warning: PipeWireAudioDevice: the PipeWire samplerate changed to %u while MusE uses %u. "
                      "There is no audio until MusE is restarted.\n", rate, MusEGlobal::sampleRate);
    silenceOutputs(n);
    return;
  }

  if(n == MusEGlobal::segmentSize)
  {
    _adapterQuantum = 0;
    processDirect(n);
  }
  else
    processAdapted(n);
}

//---------------------------------------------------------
//   open
//   Returns true on success.
//---------------------------------------------------------

bool PipeWireAudioDevice::open()
{
  pw_init(nullptr, nullptr);
  _pwInitialized = true;

  _loop = pw_thread_loop_new("MusE-pipewire", nullptr);
  if(!_loop)
  {
    fprintf(stderr, "Error: PipeWireAudioDevice: cannot create the PipeWire thread loop\n");
    cleanup();
    return false;
  }

  pw_thread_loop_lock(_loop);
  if(pw_thread_loop_start(_loop) < 0)
  {
    pw_thread_loop_unlock(_loop);
    fprintf(stderr, "Error: PipeWireAudioDevice: cannot start the PipeWire thread loop\n");
    cleanup();
    return false;
  }

  _context = pw_context_new(pw_thread_loop_get_loop(_loop), nullptr, 0);
  if(_context)
    _core = pw_context_connect(_context, nullptr, 0);
  if(!_core)
  {
    pw_thread_loop_unlock(_loop);
    fprintf(stderr, "Error: PipeWireAudioDevice: cannot connect to the PipeWire server: %s\n", strerror(errno));
    cleanup();
    return false;
  }

  _graph.init(_loop, _core);

  // What we ask the graph for. It is only a request, other nodes can make the graph run differently.
  unsigned int wanted_rate = MusEGlobal::config.deviceAudioSampleRate > 0 ? MusEGlobal::config.deviceAudioSampleRate : 48000;
  unsigned int seg = MusEGlobal::config.deviceAudioBufSize > 0 ? MusEGlobal::config.deviceAudioBufSize : 256;
  char latency[64];
  snprintf(latency, sizeof(latency), "%u/%u", seg, wanted_rate);

  pw_properties* props = pw_properties_new(
    PW_KEY_NODE_NAME, pwNodeName,
    PW_KEY_NODE_DESCRIPTION, "MusE",
    PW_KEY_APP_NAME, "MusE",
    PW_KEY_MEDIA_TYPE, "Audio",
    PW_KEY_MEDIA_CATEGORY, "Filter",
    PW_KEY_MEDIA_ROLE, "DSP",
    // Run even when nothing is linked to us, so that MusE always has a steady cycle.
    PW_KEY_NODE_ALWAYS_PROCESS, "true",
    PW_KEY_NODE_LATENCY, latency,
    nullptr);

  _filter = pw_filter_new(_core, "MusE", props);
  if(!_filter)
  {
    pw_thread_loop_unlock(_loop);
    fprintf(stderr, "Error: PipeWireAudioDevice: cannot create the PipeWire filter\n");
    cleanup();
    return false;
  }

  static pw_filter_events events;
  memset(&events, 0, sizeof(events));
  events.version = PW_VERSION_FILTER_EVENTS;
  events.process = &PipeWireAudioDevice::onProcess;
  pw_filter_add_listener(_filter, &_filterListener, &events, this);

  const int res = pw_filter_connect(_filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0);
  if(res < 0)
  {
    pw_thread_loop_unlock(_loop);
    fprintf(stderr, "Error: PipeWireAudioDevice: cannot connect the PipeWire filter: %s\n", spa_strerror(res));
    cleanup();
    return false;
  }

  // Wait for the first cycle to find out what the graph runs at.
  bool timed_out = false;
  while(!_gotFirstCycle.load())
  {
    if(pw_thread_loop_timed_wait(_loop, 4) != 0)
    {
      timed_out = true;
      break;
    }
  }
  pw_thread_loop_unlock(_loop);

  if(timed_out)
  {
    fprintf(stderr, "Error: PipeWireAudioDevice: PipeWire did not start processing\n");
    cleanup();
    return false;
  }

  // Let the graph catch up so that we know our node id and all the other nodes.
  _graph.roundtrip();
  pw_thread_loop_lock(_loop);
  _nodeId = pw_filter_get_node_id(_filter);
  pw_thread_loop_unlock(_loop);

  if(_probeRate == 0)
  {
    fprintf(stderr, "Error: PipeWireAudioDevice: cannot find out the PipeWire samplerate\n");
    cleanup();
    return false;
  }

  if(_probeRate != wanted_rate)
    fprintf(stderr, "Warning: PipeWireAudioDevice: PipeWire runs at %u Hz, not the wanted %u Hz. Using %u Hz.\n",
            _probeRate, wanted_rate, _probeRate);

  MusEGlobal::sampleRate = _probeRate;
  // Make sure the AL namespace variables mirror our variables.
  AL::sampleRate = MusEGlobal::sampleRate;
  MusEGlobal::segmentSize = seg;

  fprintf(stderr, "PipeWireAudioDevice: samplerate:%u period size:%u PipeWire cycle size:%u node id:%u realtime priority:%d\n",
          MusEGlobal::sampleRate, MusEGlobal::segmentSize, _probeQuantum, _nodeId, _rtPriority);
  if(_probeQuantum != MusEGlobal::segmentSize)
    fprintf(stderr, "PipeWireAudioDevice: the PipeWire cycle size is not the period size. MusE will run with an extra period of latency "
                    "while they differ. For the lowest latency make PipeWire use the same size, for example with: "
                    "pw-metadata -n settings 0 clock.force-quantum %u\n", MusEGlobal::segmentSize);
  return true;
}

void PipeWireAudioDevice::cleanup()
{
  _active = false;

  if(_loop)
  {
    pw_thread_loop_lock(_loop);
    if(_filter)
    {
      pw_filter_destroy(_filter);
      _filter = nullptr;
    }
    _graph.shutdown();
    if(_core)
    {
      pw_core_disconnect(_core);
      _core = nullptr;
    }
    if(_context)
    {
      pw_context_destroy(_context);
      _context = nullptr;
    }
    pw_thread_loop_unlock(_loop);
    pw_thread_loop_stop(_loop);
    pw_thread_loop_destroy(_loop);
    _loop = nullptr;
  }

  {
    std::lock_guard<std::recursive_mutex> lock(_portMutex);
    for(std::list<PwPortHandle*>::iterator i = _outPorts.begin(); i != _outPorts.end(); ++i)
    {
      delete[] (*i)->buffer;
      delete *i;
    }
    _outPorts.clear();
    for(std::list<PwPortHandle*>::iterator i = _inPorts.begin(); i != _inPorts.end(); ++i)
    {
      delete[] (*i)->buffer;
      delete *i;
    }
    _inPorts.clear();
  }
  {
    std::lock_guard<std::mutex> lock(_extMutex);
    for(std::map<uint32_t, PwPortHandle*>::iterator i = _extHandles.begin(); i != _extHandles.end(); ++i)
      delete i->second;
    _extHandles.clear();
  }

  if(_pwInitialized)
  {
    pw_deinit();
    _pwInitialized = false;
  }
}

bool PipeWireAudioDevice::start(int /*priority*/)
{
  if(!_filter)
    return false;
  // The thread that runs the cycles is PipeWire's, so the priority is not up to us.
  _firstCycle = true;
  _active = true;
  return true;
}

void PipeWireAudioDevice::stop()
{
  // Taking the lock waits for any cycle that is running.
  std::lock_guard<std::recursive_mutex> lock(_portMutex);
  _active = false;
}

//---------------------------------------------------------
//   Ports
//---------------------------------------------------------

float* PipeWireAudioDevice::getBuffer(void* port, unsigned long nframes)
{
  if(nframes > MusEGlobal::segmentSize)
  {
    fprintf(stderr, "PipeWireAudioDevice::getBuffer nframes > segment size\n");
    exit(-1);
  }
  PwPortHandle* h = static_cast<PwPortHandle*>(port);
  return (h && h->own) ? h->buffer : nullptr;
}

PwPortHandle* PipeWireAudioDevice::addPort(const char* name, bool input)
{
  if(!_filter || !name || name[0] == '\0')
    return nullptr;

  {
    // Like the other drivers without routing, give back the existing port if the name is already used.
    std::lock_guard<std::recursive_mutex> lock(_portMutex);
    const std::list<PwPortHandle*>& l = input ? _inPorts : _outPorts;
    for(std::list<PwPortHandle*>::const_iterator i = l.begin(); i != l.end(); ++i)
      if((*i)->name == name)
        return *i;
  }

  PwPortHandle* h = new PwPortHandle();
  h->own = true;
  h->isInput = input;
  h->isAudio = true;
  h->name = name;
  h->fullName = std::string(pwNodeName) + ":" + name;
  h->id = SPA_ID_INVALID;
  h->buffer = new float[MusEGlobal::segmentSize];
  memset(h->buffer, 0, MusEGlobal::segmentSize * sizeof(float));
  h->ring.assign(ringCapacity(), 0.0f);
  h->ringRead = 0;
  h->ringFill = 0;
  h->ringPrimed = false;

  pw_thread_loop_lock(_loop);
  h->pwPort = pw_filter_add_port(_filter, input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                                 PW_FILTER_PORT_FLAG_MAP_BUFFERS, sizeof(void*),
                                 pw_properties_new(PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                                   PW_KEY_PORT_NAME, name, nullptr),
                                 nullptr, 0);
  pw_thread_loop_unlock(_loop);

  if(!h->pwPort)
  {
    fprintf(stderr, "PipeWireAudioDevice: port <%s> registration FAILED\n", name);
    delete[] h->buffer;
    delete h;
    return nullptr;
  }

  std::lock_guard<std::recursive_mutex> lock(_portMutex);
  (input ? _inPorts : _outPorts).push_back(h);
  return h;
}

void PipeWireAudioDevice::autoConnect(PwPortHandle* h, int index)
{
  // Only the first stereo pair, connected to the first two ports of the default sink.
  if(!h || index < 0 || index > 1 || _nodeId == SPA_ID_INVALID)
    return;

  uint32_t id;
  if(!resolveHandleId(h, id, true))
    return;

  const std::vector<PwGraphPort> sink = _graph.defaultDevicePorts(true);
  if((size_t)index >= sink.size())
    return;
  if(!_graph.linkExists(id, sink[index].id))
    _graph.createLink(id, sink[index].id);
}

void* PipeWireAudioDevice::registerOutPort(const char* name, bool midi)
{
  if(midi)
    return nullptr;
  PwPortHandle* h = addPort(name, false);
  if(h)
  {
    int index;
    {
      std::lock_guard<std::recursive_mutex> lock(_portMutex);
      index = (int)_outPorts.size() - 1;
    }
    autoConnect(h, index);
  }
  return h;
}

void* PipeWireAudioDevice::registerInPort(const char* name, bool midi)
{
  if(midi)
    return nullptr;
  return addPort(name, true);
}

void PipeWireAudioDevice::unregisterPort(void* port)
{
  PwPortHandle* h = static_cast<PwPortHandle*>(port);
  if(!h || !h->own)
    return;

  {
    // Taking the lock waits for any cycle that is running.
    std::lock_guard<std::recursive_mutex> lock(_portMutex);
    _outPorts.remove(h);
    _inPorts.remove(h);
  }

  if(_loop)
  {
    pw_thread_loop_lock(_loop);
    pw_filter_remove_port(h->pwPort);
    pw_thread_loop_unlock(_loop);
  }
  delete[] h->buffer;
  delete h;
}

PwPortHandle* PipeWireAudioDevice::externalHandle(const PwGraphPort& p)
{
  if(!p.isAudio && !p.isMidi)
    return nullptr;
  std::lock_guard<std::mutex> lock(_extMutex);
  std::map<uint32_t, PwPortHandle*>::iterator i = _extHandles.find(p.id);
  if(i != _extHandles.end())
    return i->second;

  PwPortHandle* h = new PwPortHandle();
  h->own = false;
  h->isInput = p.isInput;
  h->isAudio = p.isAudio;
  h->id = p.id;
  h->fullName = _graph.portFullName(p);
  h->name = p.name;
  h->pwPort = nullptr;
  h->buffer = nullptr;
  h->ringRead = 0;
  h->ringFill = 0;
  h->ringPrimed = false;
  _extHandles[p.id] = h;
  return h;
}

void* PipeWireAudioDevice::findPort(const char* name)
{
  if(!name || name[0] == '\0')
    return nullptr;

  {
    std::lock_guard<std::recursive_mutex> lock(_portMutex);
    for(std::list<PwPortHandle*>::iterator i = _outPorts.begin(); i != _outPorts.end(); ++i)
      if((*i)->fullName == name)
        return *i;
    for(std::list<PwPortHandle*>::iterator i = _inPorts.begin(); i != _inPorts.end(); ++i)
      if((*i)->fullName == name)
        return *i;
  }

  PwGraphPort p;
  if(!resolveName(name, p, true))
    return nullptr;
  return externalHandle(p);
}

//---------------------------------------------------------
//   Finding ports in the graph
//---------------------------------------------------------

// Finds the graph id of the port. If wait is set it may wait for PipeWire to tell us about a port that was just made.
bool PipeWireAudioDevice::resolveHandleId(const PwPortHandle* h, uint32_t& id, bool wait) const
{
  if(!h)
    return false;
  if(h->id != SPA_ID_INVALID)
  {
    id = h->id;
    return true;
  }
  if(!h->own || _nodeId == SPA_ID_INVALID)
    return false;

  PwGraphPort p;
  for(int tries = 0; tries < 2; ++tries)
  {
    if(_graph.findNodePort(_nodeId, h->name, p))
    {
      // Our own ports are made by us, so the cached id is ours to set.
      const_cast<PwPortHandle*>(h)->id = p.id;
      id = p.id;
      return true;
    }
    if(!wait)
      break;
    _graph.roundtrip(1);
  }
  return false;
}

bool PipeWireAudioDevice::resolveName(const char* name, PwGraphPort& p, bool wait) const
{
  if(!name || name[0] == '\0')
    return false;
  if(_graph.findPort(name, p))
    return true;
  if(!wait)
    return false;
  _graph.roundtrip(1);
  return _graph.findPort(name, p);
}

std::list<QString> PipeWireAudioDevice::outputPorts(bool midi, int /*aliases*/)
{
  std::list<QString> list;
  // We do not make any midi ports.
  if(midi)
    return list;
  const std::vector<std::string> names = _graph.portNames(false, false, _nodeId);
  for(size_t i = 0; i < names.size(); ++i)
    list.push_back(QString::fromUtf8(names[i].c_str()));
  return list;
}

std::list<QString> PipeWireAudioDevice::inputPorts(bool midi, int /*aliases*/)
{
  std::list<QString> list;
  if(midi)
    return list;
  const std::vector<std::string> names = _graph.portNames(false, true, _nodeId);
  for(size_t i = 0; i < names.size(); ++i)
    list.push_back(QString::fromUtf8(names[i].c_str()));
  return list;
}

AudioDevice::PortType PipeWireAudioDevice::portType(void* port) const
{
  const PwPortHandle* h = static_cast<const PwPortHandle*>(port);
  if(!h)
    return UnknownType;
  return h->isAudio ? AudioPort : MidiPort;
}

AudioDevice::PortDirection PipeWireAudioDevice::portDirection(void* port) const
{
  const PwPortHandle* h = static_cast<const PwPortHandle*>(port);
  if(!h)
    return UnknownDirection;
  return h->isInput ? InputPort : OutputPort;
}

char* PipeWireAudioDevice::portName(void* port, char* str, int str_size, int /*preferred_name_or_alias*/)
{
  const PwPortHandle* h = static_cast<const PwPortHandle*>(port);
  return MusELib::strntcpy(str, h ? h->fullName.c_str() : nullptr, str_size);
}

const char* PipeWireAudioDevice::canonicalPortName(void* port)
{
  const PwPortHandle* h = static_cast<const PwPortHandle*>(port);
  return h ? h->fullName.c_str() : nullptr;
}

void PipeWireAudioDevice::setPortName(void* port, const char* name)
{
  PwPortHandle* h = static_cast<PwPortHandle*>(port);
  if(!h || !h->own || !name || name[0] == '\0' || !_loop)
    return;

  struct spa_dict_item item = SPA_DICT_ITEM_INIT(PW_KEY_PORT_NAME, name);
  struct spa_dict dict = SPA_DICT_INIT(&item, 1);
  pw_thread_loop_lock(_loop);
  pw_filter_update_properties(_filter, h->pwPort, &dict);
  pw_thread_loop_unlock(_loop);

  std::lock_guard<std::recursive_mutex> lock(_portMutex);
  h->name = name;
  h->fullName = std::string(pwNodeName) + ":" + name;
}

//---------------------------------------------------------
//   Connections
//---------------------------------------------------------

bool PipeWireAudioDevice::connect(void* src, void* dst)
{
  const PwPortHandle* s = static_cast<const PwPortHandle*>(src);
  const PwPortHandle* d = static_cast<const PwPortHandle*>(dst);
  uint32_t so, di;
  if(!s || !d || s->isInput || !d->isInput || s->isAudio != d->isAudio ||
     !resolveHandleId(s, so, true) || !resolveHandleId(d, di, true))
  {
    fprintf(stderr, "PipeWireAudioDevice::connect: cannot connect <%s> to <%s>\n",
            s ? s->fullName.c_str() : "?", d ? d->fullName.c_str() : "?");
    return false;
  }
  if(_graph.linkExists(so, di))
    return true;
  return _graph.createLink(so, di);
}

bool PipeWireAudioDevice::connect(const char* src, const char* dst)
{
  PwGraphPort s, d;
  if(!resolveName(src, s, true) || !resolveName(dst, d, true) || s.isInput || !d.isInput || s.isAudio != d.isAudio)
  {
    fprintf(stderr, "PipeWireAudioDevice::connect: cannot connect <%s> to <%s>\n", src ? src : "?", dst ? dst : "?");
    return false;
  }
  if(_graph.linkExists(s.id, d.id))
    return true;
  return _graph.createLink(s.id, d.id);
}

bool PipeWireAudioDevice::disconnect(void* src, void* dst)
{
  uint32_t so, di;
  if(!resolveHandleId(static_cast<const PwPortHandle*>(src), so, false) ||
     !resolveHandleId(static_cast<const PwPortHandle*>(dst), di, false))
    return false;
  return _graph.destroyLink(so, di);
}

bool PipeWireAudioDevice::disconnect(const char* src, const char* dst)
{
  PwGraphPort s, d;
  if(!resolveName(src, s, false) || !resolveName(dst, d, false))
    return false;
  return _graph.destroyLink(s.id, d.id);
}

int PipeWireAudioDevice::connections(void* port)
{
  uint32_t id;
  // A port that PipeWire has not told us about yet. It is silent anyway, so it does no harm to say it is in use.
  if(!resolveHandleId(static_cast<const PwPortHandle*>(port), id, false))
    return 1;
  return (int)_graph.linkedPorts(id).size();
}

bool PipeWireAudioDevice::portConnectedTo(void* ourPort, const char* port)
{
  uint32_t id;
  PwGraphPort other;
  if(!resolveHandleId(static_cast<const PwPortHandle*>(ourPort), id, false) || !resolveName(port, other, false))
    return false;
  return _graph.linkExists(id, other.id) || _graph.linkExists(other.id, id);
}

bool PipeWireAudioDevice::portsCompatible(void* src, void* dst) const
{
  const PwPortHandle* s = static_cast<const PwPortHandle*>(src);
  const PwPortHandle* d = static_cast<const PwPortHandle*>(dst);
  return s && d && !s->isInput && d->isInput && s->isAudio == d->isAudio;
}

bool PipeWireAudioDevice::portsCompatible(const char* src, const char* dst) const
{
  PwGraphPort s, d;
  return resolveName(src, s, false) && resolveName(dst, d, false) && !s.isInput && d.isInput && s.isAudio == d.isAudio;
}

bool PipeWireAudioDevice::portsCanConnect(void* src, void* dst) const
{
  uint32_t so, di;
  if(!portsCompatible(src, dst) ||
     !resolveHandleId(static_cast<const PwPortHandle*>(src), so, false) ||
     !resolveHandleId(static_cast<const PwPortHandle*>(dst), di, false))
    return false;
  return !_graph.linkExists(so, di);
}

bool PipeWireAudioDevice::portsCanConnect(const char* src, const char* dst) const
{
  PwGraphPort s, d;
  if(!resolveName(src, s, false) || !resolveName(dst, d, false) || s.isInput || !d.isInput || s.isAudio != d.isAudio)
    return false;
  return !_graph.linkExists(s.id, d.id);
}

bool PipeWireAudioDevice::portsCanDisconnect(void* src, void* dst) const
{
  uint32_t so, di;
  if(!resolveHandleId(static_cast<const PwPortHandle*>(src), so, false) ||
     !resolveHandleId(static_cast<const PwPortHandle*>(dst), di, false))
    return false;
  return _graph.linkExists(so, di);
}

bool PipeWireAudioDevice::portsCanDisconnect(const char* src, const char* dst) const
{
  PwGraphPort s, d;
  if(!resolveName(src, s, false) || !resolveName(dst, d, false))
    return false;
  return _graph.linkExists(s.id, d.id);
}

//---------------------------------------------------------
//   exitPipeWireAudio
//---------------------------------------------------------

void exitPipeWireAudio()
{
  if(pipeWireAudioDevice)
  {
    // Don't leave a dangling pointer around.
    if(MusEGlobal::audioDevice == pipeWireAudioDevice)
      MusEGlobal::audioDevice = nullptr;
    delete pipeWireAudioDevice;
  }
  pipeWireAudioDevice = nullptr;
}

//---------------------------------------------------------
//   initPipeWireAudio
//   Returns true on FAILURE, like initRtAudio is used.
//---------------------------------------------------------

bool initPipeWireAudio()
{
  pipeWireAudioDevice = new PipeWireAudioDevice();
  if(!pipeWireAudioDevice->open())
  {
    delete pipeWireAudioDevice;
    pipeWireAudioDevice = nullptr;
    return true;
  }
  MusEGlobal::audioDevice = pipeWireAudioDevice;
  return false;
}

} // namespace MusECore
