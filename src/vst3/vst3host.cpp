//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3host.cpp
//  Step 1 proof of concept: headless VST3 hosting. See vst3host.h.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "vst3host.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <unordered_map>

#include "base/source/fobject.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstnoteexpression.h"
#include "pluginterfaces/vst/ivstunits.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/utility/stringconvert.h"

using namespace Steinberg;

namespace MusEVst3 {

namespace {

constexpr uint32_t STATE_MAGIC = 0x3353564d; // "MVS3" little endian.
constexpr int MAX_EVENTS_PER_BLOCK = 512;

std::string toUtf8(const Vst::TChar* s) { return Steinberg::Vst::StringConvert::convert(s); }

// One host application object shared by all plugins, as the SDK examples do.
Vst::HostApplication& hostApplication()
{
  static Vst::HostApplication app;
  return app;
}

// Receives edits the plugin makes by itself (for example from its own GUI).
// Per the VST3 spec the host must forward these to the processor.
class ComponentHandler : public FObject, public Vst::IComponentHandler
{
  public:
    std::mutex mutex;                                    // For 'edits' and 'toProcessor'.
    std::vector<std::pair<uint32_t, double>> edits;      // For the host to observe.
    std::map<uint32_t, double> toProcessor;              // Latest value per parameter, for the next process().
    std::atomic<int32> restartFlags { 0 };
    std::atomic<bool> forward { true };                  // Whether edits go to the processor from here.
    Vst::IEditController* controller = nullptr;          // To read current values for begin and end.

    std::mutex cbMutex;
    std::function<void(const EditEvent&)> callback;

    void setCallback(std::function<void(const EditEvent&)> cb)
    {
      std::lock_guard<std::mutex> lock(cbMutex);
      callback = std::move(cb);
    }

    void notify(EditEvent::Type type, uint32_t id, double value)
    {
      std::function<void(const EditEvent&)> cb;
      {
        std::lock_guard<std::mutex> lock(cbMutex);
        cb = callback;
      }
      if(cb)
        cb(EditEvent { type, id, value });
    }

    tresult PLUGIN_API beginEdit(Vst::ParamID id) override
    {
      notify(EditEvent::Begin, id, controller ? controller->getParamNormalized(id) : 0.0);
      return kResultOk;
    }
    tresult PLUGIN_API performEdit(Vst::ParamID id, Vst::ParamValue v) override
    {
      {
        std::lock_guard<std::mutex> lock(mutex);
        if(edits.size() >= 4096)
          edits.erase(edits.begin(), edits.begin() + 2048);
        edits.emplace_back(id, v);
        if(forward)
          toProcessor[id] = v;
      }
      notify(EditEvent::Perform, id, v);
      return kResultOk;
    }
    tresult PLUGIN_API endEdit(Vst::ParamID id) override
    {
      notify(EditEvent::End, id, controller ? controller->getParamNormalized(id) : 0.0);
      return kResultOk;
    }
    tresult PLUGIN_API restartComponent(int32 flags) override { restartFlags |= flags; return kResultOk; }

    OBJ_METHODS(ComponentHandler, FObject)
    DEFINE_INTERFACES
      DEF_INTERFACE(Vst::IComponentHandler)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)
};

void appendU32(std::vector<uint8_t>& v, uint32_t x)
{
  for(int i = 0; i < 4; ++i) v.push_back((x >> (8 * i)) & 0xff);
}

bool readU32(const std::vector<uint8_t>& v, size_t& pos, uint32_t& x)
{
  if(pos + 4 > v.size()) return false;
  x = 0;
  for(int i = 0; i < 4; ++i) x |= uint32_t(v[pos + i]) << (8 * i);
  pos += 4;
  return true;
}

} // anonymous namespace

//---------------------------------------------------------
//   scan
//---------------------------------------------------------

std::vector<PluginInfo> scan(const std::vector<std::string>& dirs, std::vector<std::string>* errors)
{
  namespace fs = std::filesystem;

  std::vector<std::string> paths;
  if(dirs.empty())
    paths = VST3::Hosting::Module::getModulePaths();
  else
  {
    for(const auto& d : dirs)
    {
      std::error_code ec;
      if((fs::is_directory(d, ec) || fs::is_regular_file(d, ec)) && fs::path(d).extension() == ".vst3")
      {
        paths.push_back(d);
        continue;
      }
      fs::recursive_directory_iterator it(d, fs::directory_options::skip_permission_denied, ec), end;
      for(; !ec && it != end; it.increment(ec))
      {
        if(it->path().extension() == ".vst3")
        {
          paths.push_back(it->path().string());
          it.disable_recursion_pending();   // Do not look inside bundles.
        }
      }
    }
  }
  std::sort(paths.begin(), paths.end());

  std::vector<PluginInfo> result;
  for(const auto& path : paths)
  {
    std::string err;
    auto module = VST3::Hosting::Module::create(path, err);
    if(!module)
    {
      if(errors) errors->push_back(path + ": " + err);
      continue;
    }
    for(const auto& ci : module->getFactory().classInfos())
    {
      if(ci.category() != kVstAudioEffectClass)
        continue;
      PluginInfo pi;
      pi.path = path;
      pi.name = ci.name();
      pi.vendor = ci.vendor();
      pi.version = ci.version();
      pi.subCategories = ci.subCategoriesString();
      pi.uid = ci.ID().toString();
      pi.isInstrument = pi.subCategories.find("Instrument") != std::string::npos;
      result.push_back(std::move(pi));
    }
  }
  return result;
}

//---------------------------------------------------------
//   Instance::Impl
//---------------------------------------------------------

struct Instance::Impl
{
  // Declared first so that it is destroyed last: the plugin code lives in the module.
  VST3::Hosting::Module::Ptr module;
  IPtr<Vst::PlugProvider> provider;
  IPtr<Vst::IComponent> component;
  IPtr<Vst::IAudioProcessor> processor;
  IPtr<Vst::IEditController> controller;
  IPtr<ComponentHandler> handler;

  bool active = false;
  double sampleRate = 0.0;
  int maxBlock = 0;
  int64 sampleCounter = 0;

  int audioIns = 0, audioOuts = 0;
  int mainInCh = 0, mainOutCh = 0;
  std::vector<bool> inBusActive, outBusActive;
  // Channels of every audio bus, where the channels of a bus start in the arrays of process(), and the totals.
  std::vector<int> inBusCh, outBusCh, inBusOff, outBusOff;
  int inTotal = 0, outTotal = 0;
  Instance::SetupOptions options;
  std::vector<Instance::BusDetail> inBusDetails, outBusDetails;

  // Note ids. The id of the note that is on, by channel and pitch, or -1.
  std::vector<int32> noteIds;      // 16 * 128
  int32 nextNoteId = 0;
  std::vector<Instance::NoteExpressionType> expressionTypes;

  Vst::EventList inEvents { MAX_EVENTS_PER_BLOCK };
  Vst::EventList outEvents { MAX_EVENTS_PER_BLOCK };
  Vst::ParameterChanges inParams, outParams;

  // Parameter values waiting for the next process(), by parameter index. Sized in create(),
  //  so that queueParamRT() needs no allocation.
  std::unordered_map<uint32_t, int> idToIdx;
  std::vector<uint32_t> paramIds;
  std::vector<double> staged;
  std::vector<uint8_t> dirty;
  std::vector<int> dirtyList;
  int dirtyCount = 0;

  // Audio thread to main thread: parameter values for the edit controller. Single producer, single consumer.
  struct Update { uint32_t id; double value; };
  std::vector<Update> updRing;
  std::atomic<uint32_t> updWrite { 0 }, updRead { 0 };

  // Output parameter changes of the last process() call.
  std::vector<uint32_t> outIds;
  std::vector<double> outVals;
  int outCount = 0;

  Instance::Transport transport;
  bool haveTransport = false;

  // Sample accurate parameter points waiting for the next process(). Fixed size, set in create().
  struct Point { int idx; int offset; double value; };
  std::vector<Point> points;
  int pointCount = 0;

  // MIDI controller to parameter, from IMidiMapping: [channel * 130 + controller], valid flags beside.
  static const int MIDI_CONTROLLERS = 130;
  std::vector<uint32_t> midiMap;
  std::vector<uint8_t> midiMapValid;

  // Programs.
  std::vector<std::string> programNames;
  uint32_t programParamId = 0;
  int programSteps = 0;
  bool hasProgramParam = false;

  std::vector<float> silence;                 // Stand-in input when the caller gives none.
  std::vector<float*> silencePtrs;
  std::vector<Vst::AudioBusBuffers> inBuses, outBuses;
};

Instance::Instance() : _impl(new Impl) {}

Instance::~Instance()
{
  teardown();
  if(_impl->handler)
  {
    _impl->handler->setCallback(nullptr);
    _impl->handler->controller = nullptr;
  }
  if(_impl->controller)
    _impl->controller->setComponentHandler(nullptr);
  _impl->controller = nullptr;
  _impl->processor = nullptr;
  _impl->component = nullptr;
  _impl->provider = nullptr;   // Terminates and disconnects the plugin.
}

std::unique_ptr<Instance> Instance::create(const PluginInfo& info, std::string& error)
{
  std::unique_ptr<Instance> inst(new Instance);
  Impl& d = *inst->_impl;

  d.module = VST3::Hosting::Module::create(info.path, error);
  if(!d.module)
    return nullptr;

  auto& factory = d.module->getFactory();
  const auto classes = factory.classInfos();
  const VST3::Hosting::ClassInfo* found = nullptr;
  for(const auto& ci : classes)
    if(ci.category() == kVstAudioEffectClass && ci.ID().toString() == info.uid)
      found = &ci;
  if(!found)
  {
    error = "class " + info.uid + " not found in " + info.path;
    return nullptr;
  }

  Vst::PluginContextFactory::instance().setPluginContext(&hostApplication());
  d.provider = owned(new Vst::PlugProvider(factory, *found, true));
  if(!d.provider->initialize())
  {
    error = "plugin initialization failed";
    return nullptr;
  }

  d.component = d.provider->getComponentPtr();
  d.controller = d.provider->getControllerPtr();
  if(!d.component)
  {
    error = "plugin has no component";
    return nullptr;
  }
  d.processor = FUnknownPtr<Vst::IAudioProcessor>(d.component);
  if(!d.processor)
  {
    error = "plugin has no IAudioProcessor";
    return nullptr;
  }

  if(d.controller)
  {
    d.handler = owned(new ComponentHandler);
    d.handler->controller = d.controller;
    d.controller->setComponentHandler(d.handler);
  }

  // Tables for queueParamRT(), so that nothing is allocated while processing.
  const int np = d.controller ? d.controller->getParameterCount() : 0;
  d.paramIds.resize(np);
  d.staged.assign(np, 0.0);
  d.dirty.assign(np, 0);
  d.dirtyList.assign(np, 0);
  d.idToIdx.reserve(size_t(np) * 2);
  for(int i = 0; i < np; ++i)
  {
    Vst::ParameterInfo pi {};
    if(d.controller->getParameterInfo(i, pi) == kResultOk)
    {
      d.paramIds[i] = pi.id;
      d.idToIdx[pi.id] = i;
    }
  }
  d.points.resize(8192);
  d.noteIds.assign(16 * 128, -1);

  // Note expression types the plugin understands.
  if(d.controller)
  {
    FUnknownPtr<Vst::INoteExpressionController> expr(d.controller);
    if(expr)
      for(int32 i = 0; i < expr->getNoteExpressionCount(0, 0); ++i)
      {
        Vst::NoteExpressionTypeInfo ti {};
        if(expr->getNoteExpressionInfo(0, 0, i, ti) != kResultOk)
          continue;
        Instance::NoteExpressionType t;
        t.typeId = ti.typeId;
        t.title = toUtf8(ti.title);
        t.units = toUtf8(ti.units);
        t.defaultValue = ti.valueDesc.defaultValue;
        t.minPlain = ti.valueDesc.minimum;
        t.maxPlain = ti.valueDesc.maximum;
        t.bipolar = ti.flags & Vst::NoteExpressionTypeInfo::kIsBipolar;
        d.expressionTypes.push_back(t);
      }
  }

  // Which parameters MIDI controllers go to.
  d.midiMap.assign(16 * Impl::MIDI_CONTROLLERS, 0);
  d.midiMapValid.assign(16 * Impl::MIDI_CONTROLLERS, 0);
  if(d.controller && d.component->getBusCount(Vst::kEvent, Vst::kInput) > 0)
  {
    FUnknownPtr<Vst::IMidiMapping> mapping(d.controller);
    if(mapping)
      for(int ch = 0; ch < 16; ++ch)
        for(int cc = 0; cc < Impl::MIDI_CONTROLLERS; ++cc)
        {
          Vst::ParamID id = 0;
          if(mapping->getMidiControllerAssignment(0, ch, cc, id) == kResultTrue && d.idToIdx.count(id))
          {
            d.midiMap[ch * Impl::MIDI_CONTROLLERS + cc] = id;
            d.midiMapValid[ch * Impl::MIDI_CONTROLLERS + cc] = 1;
          }
        }
  }

  // The program list that goes with the program change parameter.
  for(int i = 0; i < np && !d.hasProgramParam; ++i)
  {
    Vst::ParameterInfo pi {};
    if(d.controller->getParameterInfo(i, pi) != kResultOk || !(pi.flags & Vst::ParameterInfo::kIsProgramChange))
      continue;
    d.hasProgramParam = true;
    d.programParamId = pi.id;
    d.programSteps = pi.stepCount;
    FUnknownPtr<Vst::IUnitInfo> units(d.controller);
    if(!units)
      continue;
    Vst::ProgramListID listId = Vst::kNoProgramListId;
    for(int32 u = 0; u < units->getUnitCount(); ++u)
    {
      Vst::UnitInfo ui {};
      if(units->getUnitInfo(u, ui) == kResultOk && ui.id == pi.unitId)
        listId = ui.programListId;
    }
    if(listId == Vst::kNoProgramListId)
      continue;
    for(int32 l = 0; l < units->getProgramListCount(); ++l)
    {
      Vst::ProgramListInfo li {};
      if(units->getProgramListInfo(l, li) != kResultOk || li.id != listId)
        continue;
      for(int32 k = 0; k < li.programCount; ++k)
      {
        Vst::String128 name {};
        d.programNames.push_back(units->getProgramName(listId, k, name) == kResultOk ? toUtf8(name) : std::string());
      }
    }
  }

  d.updRing.resize(1024);
  d.outIds.resize(np + 8);
  d.outVals.resize(np + 8);

  d.audioIns  = d.component->getBusCount(Vst::kAudio, Vst::kInput);
  d.audioOuts = d.component->getBusCount(Vst::kAudio, Vst::kOutput);
  return inst;
}

//---------------------------------------------------------
//   setup / teardown
//---------------------------------------------------------

bool Instance::setup(double sampleRate, int maxBlockSize, std::string& error, const SetupOptions& options)
{
  Impl& d = *_impl;
  if(d.active)
    teardown();

  if(d.processor->canProcessSampleSize(Vst::kSample32) != kResultTrue)
  {
    error = "plugin does not support 32 bit float processing";
    return false;
  }

  // Tell the plugin the speaker arrangements it already reports, as hosts are expected to.
  std::vector<Vst::SpeakerArrangement> ins(d.audioIns), outs(d.audioOuts);
  for(int i = 0; i < d.audioIns; ++i)  d.processor->getBusArrangement(Vst::kInput, i, ins[i]);
  for(int i = 0; i < d.audioOuts; ++i) d.processor->getBusArrangement(Vst::kOutput, i, outs[i]);
  d.processor->setBusArrangements(ins.data(), d.audioIns, outs.data(), d.audioOuts);

  // The main bus (index 0) is active. The others only if asked for. All event buses are active.
  d.options = options;
  d.inBusActive.assign(d.audioIns, false);
  d.outBusActive.assign(d.audioOuts, false);
  d.inBusCh.assign(d.audioIns, 0);
  d.outBusCh.assign(d.audioOuts, 0);
  d.inBusOff.assign(d.audioIns, 0);
  d.outBusOff.assign(d.audioOuts, 0);
  d.inBusDetails.assign(d.audioIns, BusDetail());
  d.outBusDetails.assign(d.audioOuts, BusDetail());
  d.inTotal = d.outTotal = 0;
  d.mainInCh = d.mainOutCh = 0;

  for(int dir = 0; dir < 2; ++dir)
  {
    const Vst::BusDirection busDir = dir == 0 ? Vst::kInput : Vst::kOutput;
    const int count = dir == 0 ? d.audioIns : d.audioOuts;
    std::vector<bool>& active = dir == 0 ? d.inBusActive : d.outBusActive;
    std::vector<int>& channels = dir == 0 ? d.inBusCh : d.outBusCh;
    std::vector<int>& offsets = dir == 0 ? d.inBusOff : d.outBusOff;
    std::vector<BusDetail>& details = dir == 0 ? d.inBusDetails : d.outBusDetails;
    int& total = dir == 0 ? d.inTotal : d.outTotal;
    for(int i = 0; i < count; ++i)
    {
      Vst::BusInfo bi {};
      const bool have = d.component->getBusInfo(Vst::kAudio, busDir, i, bi) == kResultTrue;
      BusDetail& det = details[i];
      det.name = have ? toUtf8(bi.name) : std::string();
      det.channels = have ? bi.channelCount : 0;
      det.aux = have ? bi.busType == Vst::kAux : i != 0;
      det.defaultActive = have && (bi.flags & Vst::BusInfo::kDefaultActive);
      const bool want = have && (i == 0 || (dir == 0 ? (options.sidechainInputs && det.aux) : options.allOutputBuses));
      d.component->activateBus(Vst::kAudio, busDir, i, want);
      active[i] = want;
      det.active = want;
      if(want)
      {
        channels[i] = bi.channelCount;
        offsets[i] = total;
        total += bi.channelCount;
      }
    }
    if(count > 0)
      (dir == 0 ? d.mainInCh : d.mainOutCh) = channels[0];
  }
  for(int i = 0; i < d.component->getBusCount(Vst::kEvent, Vst::kInput); ++i)
    d.component->activateBus(Vst::kEvent, Vst::kInput, i, true);
  for(int i = 0; i < d.component->getBusCount(Vst::kEvent, Vst::kOutput); ++i)
    d.component->activateBus(Vst::kEvent, Vst::kOutput, i, true);

  Vst::ProcessSetup ps {};
  ps.processMode = options.offline ? Vst::kOffline : Vst::kRealtime;
  ps.symbolicSampleSize = Vst::kSample32;
  ps.maxSamplesPerBlock = maxBlockSize;
  ps.sampleRate = sampleRate;
  if(d.processor->setupProcessing(ps) != kResultOk)
  {
    error = "setupProcessing failed";
    return false;
  }
  if(d.component->setActive(true) != kResultOk)
  {
    error = "setActive(true) failed";
    return false;
  }
  d.processor->setProcessing(true);   // Some plugins return kNotImplemented, which is fine.

  d.sampleRate = sampleRate;
  d.maxBlock = maxBlockSize;
  d.sampleCounter = 0;
  d.active = true;

  d.silence.assign(size_t(std::max(d.inTotal, 1)) * maxBlockSize, 0.0f);
  d.silencePtrs.clear();
  for(int c = 0; c < d.inTotal; ++c)
    d.silencePtrs.push_back(d.silence.data() + size_t(c) * maxBlockSize);

  d.inBuses.assign(d.audioIns, Vst::AudioBusBuffers());
  d.outBuses.assign(d.audioOuts, Vst::AudioBusBuffers());

  int32 np = d.controller ? d.controller->getParameterCount() : 0;
  d.inParams.setMaxParameters(np + 8);
  d.outParams.setMaxParameters(np + 8);

  // No note is on in a plugin that was just set up.
  std::fill(d.noteIds.begin(), d.noteIds.end(), -1);
  return true;
}

bool Instance::setOffline(bool offline)
{
  Impl& d = *_impl;
  if(d.options.offline == offline)
    return true;
  SetupOptions o = d.options;
  o.offline = offline;
  if(!d.active)
  {
    d.options = o;
    return true;
  }
  std::string err;
  return setup(d.sampleRate, d.maxBlock, err, o);
}

bool Instance::offline() const { return _impl->options.offline; }

int Instance::inputChannels() const  { return _impl->inTotal; }
int Instance::outputChannels() const { return _impl->outTotal; }

std::vector<Instance::BusDetail> Instance::audioBuses(bool input) const
{
  return input ? _impl->inBusDetails : _impl->outBusDetails;
}

void Instance::teardown()
{
  Impl& d = *_impl;
  if(!d.active)
    return;
  d.processor->setProcessing(false);
  d.component->setActive(false);
  d.active = false;
}

int Instance::mainInputChannels() const    { return _impl->mainInCh; }
int Instance::mainOutputChannels() const   { return _impl->mainOutCh; }
int Instance::audioInputBusCount() const   { return _impl->audioIns; }
int Instance::audioOutputBusCount() const  { return _impl->audioOuts; }
bool Instance::acceptsNoteEvents() const   { return _impl->component->getBusCount(Vst::kEvent, Vst::kInput) > 0; }
uint32_t Instance::latencySamples() const  { return _impl->processor->getLatencySamples(); }
uint32_t Instance::tailSamples() const     { return _impl->processor->getTailSamples(); }

//---------------------------------------------------------
//   parameters
//---------------------------------------------------------

int Instance::paramCount() const
{
  return _impl->controller ? _impl->controller->getParameterCount() : 0;
}

ParamInfo Instance::paramInfo(int index) const
{
  ParamInfo r;
  Vst::ParameterInfo pi {};
  if(_impl->controller && _impl->controller->getParameterInfo(index, pi) == kResultOk)
  {
    r.id = pi.id;
    r.title = toUtf8(pi.title);
    r.units = toUtf8(pi.units);
    r.defaultNormalized = pi.defaultNormalizedValue;
    r.stepCount = pi.stepCount;
    r.readOnly = pi.flags & Vst::ParameterInfo::kIsReadOnly;
    r.hidden = pi.flags & Vst::ParameterInfo::kIsHidden;
    r.isProgramChange = pi.flags & Vst::ParameterInfo::kIsProgramChange;
    r.isBypass = pi.flags & Vst::ParameterInfo::kIsBypass;
  }
  return r;
}

double Instance::getParam(uint32_t id) const
{
  return _impl->controller ? _impl->controller->getParamNormalized(id) : 0.0;
}

void Instance::setParam(uint32_t id, double normalized)
{
  if(_impl->controller)
    _impl->controller->setParamNormalized(id, normalized);
  queueParamRT(id, normalized);
}

bool Instance::queueParamRT(uint32_t id, double normalized)
{
  Impl& d = *_impl;
  const auto it = d.idToIdx.find(id);
  if(it == d.idToIdx.end())
    return false;
  const int idx = it->second;
  d.staged[idx] = normalized;
  if(!d.dirty[idx])
  {
    d.dirty[idx] = 1;
    d.dirtyList[d.dirtyCount++] = idx;
  }
  return true;
}

bool Instance::controllerUpdateRT(uint32_t id, double normalized)
{
  Impl& d = *_impl;
  const uint32_t w = d.updWrite.load(std::memory_order_relaxed);
  const uint32_t r = d.updRead.load(std::memory_order_acquire);
  if(w - r >= d.updRing.size())
    return false;
  d.updRing[w % d.updRing.size()] = { id, normalized };
  d.updWrite.store(w + 1, std::memory_order_release);
  return true;
}

int Instance::applyControllerUpdates()
{
  Impl& d = *_impl;
  int n = 0;
  uint32_t r = d.updRead.load(std::memory_order_relaxed);
  const uint32_t w = d.updWrite.load(std::memory_order_acquire);
  while(r != w)
  {
    const Impl::Update u = d.updRing[r % d.updRing.size()];
    if(d.controller)
      d.controller->setParamNormalized(u.id, u.value);
    ++r;
    ++n;
  }
  d.updRead.store(r, std::memory_order_release);
  return n;
}

int Instance::takeRestartFlags()
{
  return _impl->handler ? _impl->handler->restartFlags.exchange(0) : 0;
}

int Instance::outputParamChanges(const uint32_t** ids, const double** values) const
{
  *ids = _impl->outIds.data();
  *values = _impl->outVals.data();
  return _impl->outCount;
}

std::vector<ParamInfo> Instance::inputControls() const
{
  std::vector<ParamInfo> r;
  for(int i = 0; i < paramCount(); ++i)
  {
    ParamInfo p = paramInfo(i);
    if(!p.hidden && !p.isProgramChange && !p.readOnly)
      r.push_back(p);
  }
  return r;
}

std::vector<ParamInfo> Instance::outputControls() const
{
  std::vector<ParamInfo> r;
  for(int i = 0; i < paramCount(); ++i)
  {
    ParamInfo p = paramInfo(i);
    if(!p.hidden && !p.isProgramChange && p.readOnly)
      r.push_back(p);
  }
  return r;
}

bool Instance::queueParamAtRT(uint32_t id, double normalized, int sampleOffset)
{
  Impl& d = *_impl;
  const auto it = d.idToIdx.find(id);
  if(it == d.idToIdx.end() || d.pointCount >= int(d.points.size()))
    return false;
  d.points[d.pointCount++] = { it->second, sampleOffset, normalized };
  return true;
}

bool Instance::hasMidiMapping(int channel, int controller) const
{
  if(channel < 0 || channel > 15 || controller < 0 || controller >= Impl::MIDI_CONTROLLERS)
    return false;
  return _impl->midiMapValid[channel * Impl::MIDI_CONTROLLERS + controller];
}

bool Instance::midiController(int channel, int controller, double normalized, int sampleOffset)
{
  if(!hasMidiMapping(channel, controller))
    return false;
  const uint32_t id = _impl->midiMap[channel * Impl::MIDI_CONTROLLERS + controller];
  if(!queueParamAtRT(id, normalized, sampleOffset))
    return false;
  controllerUpdateRT(id, normalized);
  return true;
}

int Instance::programCount() const { return int(_impl->programNames.size()); }

std::string Instance::programName(int index) const
{
  return (index >= 0 && index < int(_impl->programNames.size())) ? _impl->programNames[index] : std::string();
}

bool Instance::selectProgramRT(int index, int sampleOffset)
{
  Impl& d = *_impl;
  if(!d.hasProgramParam || index < 0 || index >= int(d.programNames.size()))
    return false;
  const double normalized = d.programSteps > 0 ? double(index) / double(d.programSteps) : 0.0;
  if(!queueParamAtRT(d.programParamId, normalized, sampleOffset))
    return false;
  controllerUpdateRT(d.programParamId, normalized);
  return true;
}

void Instance::setEditCallback(std::function<void(const EditEvent&)> callback)
{
  if(_impl->handler)
    _impl->handler->setCallback(std::move(callback));
}

void Instance::setForwardEditsToProcessor(bool forward)
{
  if(_impl->handler)
    _impl->handler->forward = forward;
}

bool Instance::isActive() const { return _impl->active; }

void Instance::setTransport(const Transport& t)
{
  _impl->transport = t;
  _impl->haveTransport = true;
}

std::string Instance::paramToString(uint32_t id, double normalized) const
{
  Vst::String128 s {};
  if(_impl->controller && _impl->controller->getParamStringByValue(id, normalized, s) == kResultOk)
    return toUtf8(s);
  return std::string();
}

std::vector<std::pair<uint32_t, double>> Instance::pluginEdits(bool drain)
{
  std::vector<std::pair<uint32_t, double>> r;
  if(_impl->handler)
  {
    std::lock_guard<std::mutex> lock(_impl->handler->mutex);
    r = _impl->handler->edits;
    if(drain)
      _impl->handler->edits.clear();
  }
  return r;
}

//---------------------------------------------------------
//   notes
//---------------------------------------------------------

int Instance::noteOn(int channel, int pitch, float velocity, int sampleOffset)
{
  Impl& d = *_impl;
  const int ch = channel & 15;
  const int slot = ch * 128 + (pitch & 127);

  // A note that is already on is ended first, so that every note has its own id.
  if(d.noteIds[slot] >= 0)
    noteOff(channel, pitch, 0.0f, sampleOffset);

  const int32 id = d.nextNoteId;
  d.nextNoteId = d.nextNoteId == 0x7fffffff ? 0 : d.nextNoteId + 1;
  d.noteIds[slot] = id;

  Vst::Event e {};
  e.busIndex = 0;
  e.sampleOffset = sampleOffset;
  e.type = Vst::Event::kNoteOnEvent;
  e.noteOn.channel = channel;
  e.noteOn.pitch = pitch;
  e.noteOn.velocity = velocity;
  e.noteOn.length = 0;
  e.noteOn.tuning = 0.0f;
  e.noteOn.noteId = id;
  d.inEvents.addEvent(e);
  return id;
}

void Instance::noteOff(int channel, int pitch, float velocity, int sampleOffset)
{
  Impl& d = *_impl;
  const int slot = (channel & 15) * 128 + (pitch & 127);
  Vst::Event e {};
  e.busIndex = 0;
  e.sampleOffset = sampleOffset;
  e.type = Vst::Event::kNoteOffEvent;
  e.noteOff.channel = channel;
  e.noteOff.pitch = pitch;
  e.noteOff.velocity = velocity;
  e.noteOff.tuning = 0.0f;
  e.noteOff.noteId = d.noteIds[slot];
  d.noteIds[slot] = -1;
  d.inEvents.addEvent(e);
}

std::vector<Instance::NoteExpressionType> Instance::noteExpressionTypes() const { return _impl->expressionTypes; }
bool Instance::supportsNoteExpression() const { return !_impl->expressionTypes.empty(); }

bool Instance::noteExpression(int channel, int pitch, uint32_t typeId, double normalized, int sampleOffset)
{
  Impl& d = *_impl;
  const int32 id = d.noteIds[(channel & 15) * 128 + (pitch & 127)];
  if(id < 0)
    return false;
  Vst::Event e {};
  e.busIndex = 0;
  e.sampleOffset = sampleOffset;
  e.type = Vst::Event::kNoteExpressionValueEvent;
  e.noteExpressionValue.noteId = id;
  e.noteExpressionValue.typeId = typeId;
  e.noteExpressionValue.value = normalized;
  d.inEvents.addEvent(e);
  return true;
}

void Instance::polyPressure(int channel, int pitch, float pressure, int sampleOffset)
{
  Vst::Event e {};
  e.busIndex = 0;
  e.sampleOffset = sampleOffset;
  e.type = Vst::Event::kPolyPressureEvent;
  e.polyPressure.channel = channel;
  e.polyPressure.pitch = pitch;
  e.polyPressure.pressure = pressure;
  e.polyPressure.noteId = _impl->noteIds[(channel & 15) * 128 + (pitch & 127)];
  _impl->inEvents.addEvent(e);
}

//---------------------------------------------------------
//   process
//---------------------------------------------------------

bool Instance::process(const float* const* in, float* const* out, int nframes)
{
  Impl& d = *_impl;
  if(!d.active || nframes <= 0 || nframes > d.maxBlock)
    return false;

  // Parameter changes: from queueParamRT() and, if asked for, from the plugin's own edits.
  d.inParams.clearQueue();
  if(d.handler && d.handler->forward)
  {
    // Not realtime safe, but a host that cares turns forwarding off.
    std::lock_guard<std::mutex> lock(d.handler->mutex);
    for(const auto& kv : d.handler->toProcessor)
      queueParamRT(kv.first, kv.second);
    d.handler->toProcessor.clear();
  }
  for(int i = 0; i < d.dirtyCount; ++i)
  {
    const int idx = d.dirtyList[i];
    int32 index = 0;
    if(Vst::IParamValueQueue* q = d.inParams.addParameterData(d.paramIds[idx], index))
      q->addPoint(0, d.staged[idx], index);
    d.dirty[idx] = 0;
  }
  d.dirtyCount = 0;
  for(int i = 0; i < d.pointCount; ++i)
  {
    const Impl::Point& pt = d.points[i];
    int32 index = 0;
    if(Vst::IParamValueQueue* q = d.inParams.addParameterData(d.paramIds[pt.idx], index))
      q->addPoint(std::min(std::max(pt.offset, 0), nframes - 1), pt.value, index);
  }
  d.pointCount = 0;
  d.outParams.clearQueue();
  d.outEvents.clear();

  // Buffers. Inactive buses get zero channels. The channel arrays hold the channels of the active
  //  buses one after the other.
  for(int b = 0; b < d.audioIns; ++b)
  {
    Vst::AudioBusBuffers& bb = d.inBuses[b];
    bb = Vst::AudioBusBuffers();
    if(d.inBusActive[b])
    {
      bb.numChannels = d.inBusCh[b];
      bb.channelBuffers32 = const_cast<float**>((in ? in : d.silencePtrs.data()) + d.inBusOff[b]);
    }
  }
  for(int b = 0; b < d.audioOuts; ++b)
  {
    Vst::AudioBusBuffers& bb = d.outBuses[b];
    bb = Vst::AudioBusBuffers();
    if(d.outBusActive[b])
    {
      bb.numChannels = d.outBusCh[b];
      bb.channelBuffers32 = const_cast<float**>(out + d.outBusOff[b]);
    }
  }

  Vst::ProcessContext ctx {};
  {
    const Instance::Transport& t = d.transport;
    ctx.state = Vst::ProcessContext::kTempoValid | Vst::ProcessContext::kTimeSigValid |
                Vst::ProcessContext::kProjectTimeMusicValid;
    if(!d.haveTransport || t.playing)
      ctx.state |= Vst::ProcessContext::kPlaying;
    ctx.sampleRate = d.sampleRate;
    if(d.haveTransport)
    {
      ctx.projectTimeSamples = t.sample;
      ctx.tempo = t.tempo;
      ctx.timeSigNumerator = t.timeSigNumerator;
      ctx.timeSigDenominator = t.timeSigDenominator;
      ctx.projectTimeMusic = t.quarters;
    }
    else
    {
      ctx.projectTimeSamples = d.sampleCounter;
      ctx.tempo = 120.0;
      ctx.timeSigNumerator = 4;
      ctx.timeSigDenominator = 4;
      ctx.projectTimeMusic = double(d.sampleCounter) / d.sampleRate * ctx.tempo / 60.0;
    }
  }

  Vst::ProcessData data {};
  data.processMode = d.options.offline ? Vst::kOffline : Vst::kRealtime;
  data.symbolicSampleSize = Vst::kSample32;
  data.numSamples = nframes;
  data.numInputs = d.audioIns;
  data.numOutputs = d.audioOuts;
  data.inputs = d.audioIns ? d.inBuses.data() : nullptr;
  data.outputs = d.audioOuts ? d.outBuses.data() : nullptr;
  data.inputParameterChanges = &d.inParams;
  data.outputParameterChanges = &d.outParams;
  data.inputEvents = &d.inEvents;
  data.outputEvents = &d.outEvents;
  data.processContext = &ctx;

  tresult res = d.processor->process(data);

  // Remember what the plugin did to its output parameters.
  d.outCount = 0;
  for(int32 i = 0; i < d.outParams.getParameterCount() && d.outCount < int(d.outIds.size()); ++i)
  {
    Vst::IParamValueQueue* q = d.outParams.getParameterData(i);
    if(!q || q->getPointCount() <= 0)
      continue;
    int32 offset = 0;
    Vst::ParamValue v = 0.0;
    if(q->getPoint(q->getPointCount() - 1, offset, v) == kResultTrue)
    {
      d.outIds[d.outCount] = q->getParameterId();
      d.outVals[d.outCount] = v;
      ++d.outCount;
    }
  }

  d.haveTransport = false;
  d.inEvents.clear();
  d.sampleCounter += nframes;
  return res == kResultOk;
}

//---------------------------------------------------------
//   editor
//---------------------------------------------------------

IPlugView* Instance::createEditorView()
{
  return _impl->controller ? _impl->controller->createView(Vst::ViewType::kEditor) : nullptr;
}

//---------------------------------------------------------
//   state
//---------------------------------------------------------

bool Instance::saveState(std::vector<uint8_t>& blob) const
{
  Impl& d = *_impl;
  MemoryStream comp, ctrl;
  if(d.component->getState(&comp) != kResultOk)
    return false;
  bool haveCtrl = d.controller && d.controller->getState(&ctrl) == kResultOk;

  blob.clear();
  appendU32(blob, STATE_MAGIC);
  appendU32(blob, uint32_t(comp.getSize()));
  blob.insert(blob.end(), (const uint8_t*)comp.getData(), (const uint8_t*)comp.getData() + comp.getSize());
  uint32_t cs = haveCtrl ? uint32_t(ctrl.getSize()) : 0;
  appendU32(blob, cs);
  if(cs)
    blob.insert(blob.end(), (const uint8_t*)ctrl.getData(), (const uint8_t*)ctrl.getData() + cs);
  return true;
}

bool Instance::loadState(const std::vector<uint8_t>& blob)
{
  Impl& d = *_impl;
  size_t pos = 0;
  uint32_t magic = 0, compLen = 0, ctrlLen = 0;
  if(!readU32(blob, pos, magic) || magic != STATE_MAGIC || !readU32(blob, pos, compLen) ||
     pos + compLen > blob.size())
    return false;
  std::vector<uint8_t> comp(blob.begin() + pos, blob.begin() + pos + compLen);
  pos += compLen;
  if(!readU32(blob, pos, ctrlLen) || pos + ctrlLen > blob.size())
    return false;
  std::vector<uint8_t> ctrl(blob.begin() + pos, blob.begin() + pos + ctrlLen);

  MemoryStream compStream(comp.data(), comp.size());
  if(d.component->setState(&compStream) != kResultOk)
    return false;
  if(d.controller)
  {
    // Lets the edit controller follow the processor's state.
    MemoryStream again(comp.data(), comp.size());
    d.controller->setComponentState(&again);
    if(ctrlLen)
    {
      MemoryStream ctrlStream(ctrl.data(), ctrl.size());
      d.controller->setState(&ctrlStream);
    }
  }
  return true;
}

} // namespace MusEVst3
