//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3.cpp
//  VST3 effect plugins. See ../README.vst3 for how it fits together.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2
//  of the License, or (at your option) any later version.
//=========================================================

#include "vst3.h"

#ifdef VST3_SUPPORT

#include <QByteArray>
#include <QGuiApplication>
#include <QMessageBox>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "vst3/vst3editor.h"

#include "audio.h"
#include "globals.h"
#include "midi_consts.h"
#include "minstrument.h"
#include "popupmenu.h"
#include "pos.h"
#include "song.h"
#include "gconfig.h"
#include "plugin_list.h"
#include "pluglist.h"
#include "sig.h"
#include "sync.h"
#include "tempo.h"
#include "track.h"

// Do not print anything from the audio thread unless debugging.
//#define VST3_DEBUG
#ifdef VST3_DEBUG
#define DEBUG_VST3(dev, format, args...) std::fprintf(dev, format, ##args)
#else
#define DEBUG_VST3(dev, format, args...)
#endif

namespace MusECore {

namespace {

// Steinberg::Vst::RestartFlags that we act upon. See ivstcomponent.h.
const int kRestartParamValuesChanged = 1 << 2;
const int kRestartLatencyChanged = 1 << 3;

const char* const STATE_PREFIX = "vst3-state:";

} // anonymous namespace

//---------------------------------------------------------
//   Vst3PluginState
//---------------------------------------------------------

Vst3PluginState::Vst3PluginState(Vst3PluginWrapper* w, PluginI* p)
 : wrapper(w), pluginI(p), maxBlock(0), blocksProcessed(0), peakOut(0.0f),
   reportedFirstBlock(false), reportedSound(false), testTone(false), testOffline(false), offlineBeats(0), tonePos(0), editor(nullptr), guiVisible(false)
{
}

Vst3PluginState::~Vst3PluginState()
{
  // The editor must go before the plugin does.
  if(editor)
  {
    delete editor;
    editor = nullptr;
  }
  if(inst)
    inst->setEditCallback(nullptr);
}

void Vst3PluginState::pluginEdit(const MusEVst3::EditEvent& ev)
{
  if(MusEGlobal::debugMsg)
    std::fprintf(stderr, "Vst3PluginState::pluginEdit: %s: param %u value %f\n",
                 ev.type == MusEVst3::EditEvent::Begin ? "begin" : ev.type == MusEVst3::EditEvent::End ? "end" : "edit",
                 ev.id, ev.value);
  const auto it = wrapper->_inIdToIdx.find(ev.id);
  if(it == wrapper->_inIdToIdx.end() || !pluginI)
    return;
  const unsigned long idx = it->second;
  const float value = float(ev.value);
  AudioTrack* t = pluginI->track();
  const int plugId = pluginI->id();
  const bool haveTrack = t && plugId != -1;

  switch(ev.type)
  {
    case MusEVst3::EditEvent::Begin:
      if(haveTrack)
        t->startAutoRecord(genACnum(plugId, idx), value);
      pluginI->addIpcCallbackEvent(PluginCallbackEventStruct(PluginCallbackEventStruct::AutomationBeginType, idx, value));
    break;

    case MusEVst3::EditEvent::Perform:
      if(haveTrack)
        t->recordAutomation(genACnum(plugId, idx), value);
      pluginI->addIpcCallbackEvent(PluginCallbackEventStruct(PluginCallbackEventStruct::AutomationEditType, idx, value));
    break;

    case MusEVst3::EditEvent::End:
    {
      AutomationType at = AUTO_OFF;
      if(t)
        at = t->automationType();
      if(haveTrack)
        t->stopAutoRecord(genACnum(plugId, idx), value);
      if(at == AUTO_OFF || (at == AUTO_READ && MusEGlobal::audio->isPlaying()) || at == AUTO_TOUCH)
        pluginI->addIpcCallbackEvent(PluginCallbackEventStruct(PluginCallbackEventStruct::AutomationEndType, idx, value));
    }
    break;
  }
}

void Vst3PluginState::syncControlsFromPlugin()
{
  if(!inst)
    return;
  for(size_t c = 0; c < wrapper->_inCtl.size() && c < inControls.size(); ++c)
  {
    const float v = float(inst->getParam(wrapper->_inCtl[c].id));
    if(v == lastIn[c])
      continue;
    // The audio thread might be about to send the old value. Changing the cache first makes it
    //  skip the value that we are about to put in the control.
    lastIn[c] = v;
    if(inControls[c])
      *inControls[c] = v;
    if(pluginI)
      pluginI->addIpcCallbackEvent(PluginCallbackEventStruct(PluginCallbackEventStruct::AutomationEditType, c, v));
  }
}

void Vst3PluginState::updateWindowTitle()
{
  if(editor && pluginI)
    editor->setWindowTitle(pluginI->titlePrefix() + pluginI->pluginName());
}

void Vst3PluginState::heartBeat()
{
  if(!inst)
    return;
  if(MusEGlobal::debugMsg)
  {
    if(!reportedFirstBlock && blocksProcessed.load() > 0)
    {
      reportedFirstBlock = true;
      std::fprintf(stderr, "Vst3PluginState: %s: processing audio\n", wrapper->name().toLocal8Bit().constData());
    }
    if(!reportedSound && peakOut.load() > 0.0f)
    {
      reportedSound = true;
      std::fprintf(stderr, "Vst3PluginState: %s: output peak %f\n",
                   wrapper->name().toLocal8Bit().constData(), double(peakOut.load()));
    }
  }
  // For trying the offline mode without exporting (MUSE_VST3_TEST_FREEWHEEL=1 with -D): offline for a while.
  if(MusEGlobal::debugMsg && qEnvironmentVariableIsSet("MUSE_VST3_TEST_FREEWHEEL"))
  {
    ++offlineBeats;
    if(offlineBeats == 60)  testOffline = true;
    if(offlineBeats == 140) testOffline = false;
  }

  // For testing without being able to click in the editor (MUSE_VST3_TEST_EDIT=1 together with -D):
  //  after a while, behave as if the plugin reported an edit of its last control.
  if(MusEGlobal::debugMsg && !wrapper->_inCtl.empty() && qEnvironmentVariableIsSet("MUSE_VST3_TEST_EDIT"))
  {
    static int beats = 0;
    ++beats;
    if(beats == 50 || beats == 51 || beats == 52)
    {
      const MusEVst3::EditEvent ev { beats == 50 ? MusEVst3::EditEvent::Begin :
                                     beats == 51 ? MusEVst3::EditEvent::Perform : MusEVst3::EditEvent::End,
                                     wrapper->_inCtl.back().id, 0.625 };
      pluginEdit(ev);
    }
  }

  inst->applyControllerUpdates();
  const int flags = inst->takeRestartFlags();
  if(flags & kRestartParamValuesChanged)
    syncControlsFromPlugin();
  // A changed latency is picked up by getPluginLatency() on its own.
  (void)kRestartLatencyChanged;
}

//---------------------------------------------------------
//   Vst3PluginWrapper
//---------------------------------------------------------

Vst3PluginWrapper::Vst3PluginWrapper(const MusEPlugin::PluginScanInfoStruct& info)
 : Plugin(info)
{
  _hasGui = info._pluginFlags & MusEPlugin::PluginHasGui;
  _infoLoaded = false;
}

Vst3PluginWrapper::~Vst3PluginWrapper() {}

bool Vst3PluginWrapper::reference()
{
  ++_references;
  return true;
}

int Vst3PluginWrapper::release()
{
  if(_references > 0)
    --_references;
  return _references;
}

LADSPA_Handle Vst3PluginWrapper::instantiate(PluginI* pluginI)
{
  MusEVst3::PluginInfo pi;
  pi.path = filePath().toStdString();
  pi.uid = uri().toStdString();
  pi.name = name().toStdString();

  std::string err;
  std::unique_ptr<MusEVst3::Instance> inst = MusEVst3::Instance::create(pi, err);
  if(!inst)
  {
    std::fprintf(stderr, "Vst3PluginWrapper::instantiate: %s: %s\n", pi.name.c_str(), err.c_str());
    return nullptr;
  }

  if(!_infoLoaded)
  {
    _inCtl = inst->inputControls();
    _outCtl = inst->outputControls();
    _inIdToIdx.clear();
    _outIdToIdx.clear();
    for(size_t i = 0; i < _inCtl.size(); ++i)
      _inIdToIdx[_inCtl[i].id] = int(i);
    for(size_t i = 0; i < _outCtl.size(); ++i)
      _outIdToIdx[_outCtl[i].id] = int(i);

    _portNames.clear();
    for(unsigned long i = 0; i < _inports; ++i)
      _portNames.push_back("input" + std::to_string(i));
    for(unsigned long i = 0; i < _outports; ++i)
      _portNames.push_back("output" + std::to_string(i));
    for(size_t i = 0; i < _inCtl.size(); ++i)
      _portNames.push_back(_inCtl[i].title.empty() ? "control" + std::to_string(i) : _inCtl[i].title);
    for(size_t i = 0; i < _outCtl.size(); ++i)
      _portNames.push_back(_outCtl[i].title.empty() ? "output control" + std::to_string(i) : _outCtl[i].title);
    _infoLoaded = true;
  }

  if(_inCtl.size() != _controlInPorts || _outCtl.size() != _controlOutPorts)
  {
    std::fprintf(stderr, "Vst3PluginWrapper::instantiate: %s: the plugin has %zu/%zu control ports, the plugin cache says %lu/%lu."
                 " Please rescan the plugins.\n",
                 pi.name.c_str(), _inCtl.size(), _outCtl.size(), _controlInPorts, _controlOutPorts);
    return nullptr;
  }

  Vst3PluginState* state = new Vst3PluginState(this, pluginI);
  state->inst = std::move(inst);
  state->inPorts.assign(_inports, nullptr);
  state->outPorts.assign(_outports, nullptr);
  state->inControls.assign(_controlInPorts, nullptr);
  state->outControls.assign(_controlOutPorts, nullptr);
  state->lastIn.resize(_controlInPorts);
  for(size_t c = 0; c < _inCtl.size(); ++c)
    state->lastIn[c] = float(_inCtl[c].defaultNormalized);
  state->chunkIn.assign(_inports, nullptr);
  state->chunkOut.assign(_outports, nullptr);

  // We handle what the plugin does by itself ourselves, through the control values of MusE.
  state->inst->setForwardEditsToProcessor(false);
  state->inst->setEditCallback([state](const MusEVst3::EditEvent& ev) { state->pluginEdit(ev); });

  QObject::connect(MusEGlobal::heartBeatTimer, SIGNAL(timeout()), state, SLOT(heartBeat()));
  state->testTone = MusEGlobal::debugMsg && qEnvironmentVariableIsSet("MUSE_VST3_TEST_TONE");
  if(MusEGlobal::debugMsg)
    std::fprintf(stderr, "Vst3PluginWrapper::instantiate: %s: audio in:%lu out:%lu, controls in:%lu out:%lu\n",
                 pi.name.c_str(), _inports, _outports, _controlInPorts, _controlOutPorts);
  return LADSPA_Handle(state);
}

void Vst3PluginWrapper::activate(LADSPA_Handle handle)
{
  Vst3PluginState* state = static_cast<Vst3PluginState*>(handle);
  if(!state || state->inst->isActive())
    return;
  std::string err;
  state->maxBlock = MusEGlobal::segmentSize;
  if(!state->inst->setup(MusEGlobal::sampleRate, state->maxBlock, err))
    std::fprintf(stderr, "Vst3PluginWrapper::activate: %s: %s\n", name().toLocal8Bit().constData(), err.c_str());
  else if(MusEGlobal::debugMsg)
    std::fprintf(stderr, "Vst3PluginWrapper::activate: %s: set up for %d samples at %d Hz, latency:%u\n",
                 name().toLocal8Bit().constData(), state->maxBlock, MusEGlobal::sampleRate, state->inst->latencySamples());
}

void Vst3PluginWrapper::deactivate(LADSPA_Handle handle)
{
  Vst3PluginState* state = static_cast<Vst3PluginState*>(handle);
  if(state && state->inst->isActive())
    state->inst->teardown();
}

void Vst3PluginWrapper::cleanup(LADSPA_Handle handle)
{
  Vst3PluginState* state = static_cast<Vst3PluginState*>(handle);
  if(!state)
    return;
  // Stops the heart beat, closes the editor and then the plugin, in that order.
  delete state;
}

void Vst3PluginWrapper::connectPort(LADSPA_Handle handle, unsigned long port, float* value)
{
  Vst3PluginState* state = static_cast<Vst3PluginState*>(handle);
  if(!state)
    return;
  if(port < _inports)
    state->inPorts[port] = value;
  else if(port < _inports + _outports)
    state->outPorts[port - _inports] = value;
  else if(port < _inports + _outports + _controlInPorts)
    state->inControls[port - _inports - _outports] = value;
  else if(port < _inports + _outports + _controlInPorts + _controlOutPorts)
    state->outControls[port - _inports - _outports - _controlInPorts] = value;
}

void Vst3PluginWrapper::apply(LADSPA_Handle handle, unsigned long n, float latency_corr)
{
  Vst3PluginState* state = static_cast<Vst3PluginState*>(handle);
  MusEVst3::Instance* inst = state->inst.get();

  // Not set up, or the plugin does not agree with what the plugin cache says about its channels.
  // Pass the audio through if that is possible, else silence.
  const bool ok = inst->isActive() &&
                  inst->mainInputChannels() == int(_inports) && inst->mainOutputChannels() == int(_outports);
  if(!ok)
  {
    for(unsigned long o = 0; o < _outports; ++o)
    {
      float* out = state->outPorts[o];
      if(!out)
        continue;
      const float* in = (_inports == _outports) ? state->inPorts[o] : nullptr;
      if(in)
      {
        if(in != out)
          std::memmove(out, in, n * sizeof(float));
      }
      else
        std::memset(out, 0, n * sizeof(float));
    }
    return;
  }

  // Offline (freewheel, for example when exporting) or real time. The plugin is set up again if that
  //  changes, which is fine at such a transition.
  {
    const bool wantOffline = MusEGlobal::audio->freewheel() || state->testOffline;
    if(wantOffline != inst->offline())
    {
      inst->setOffline(wantOffline);
      if(MusEGlobal::debugMsg)
        std::fprintf(stderr, "Vst3PluginWrapper::apply: %s: %s processing\n", name().toLocal8Bit().constData(),
                     wantOffline ? "offline" : "real time");
      if(!inst->isActive())
        return;
    }
  }

  // Control values that changed since the last time, for the processor and for the plugin's editor.
  for(size_t c = 0; c < _inCtl.size(); ++c)
  {
    const float* p = state->inControls[c];
    if(!p)
      continue;
    const float v = *p;
    if(v != state->lastIn[c])
    {
      state->lastIn[c] = v;
      inst->queueParamRT(_inCtl[c].id, v);
      inst->controllerUpdateRT(_inCtl[c].id, v);
      // Printing is not for the audio thread, but this is only with the debug option.
      if(MusEGlobal::debugMsg)
        std::fprintf(stderr, "Vst3PluginWrapper::apply: %s: control %zu (%s) = %f\n",
                     name().toLocal8Bit().constData(), c, _inCtl[c].title.c_str(), double(v));
    }
  }

  // For testing without an audio source (MUSE_VST3_TEST_TONE=1 together with the debug option -D):
  //  replace the input with a sine wave.
  if(state->testTone)
  {
    for(unsigned long i = 0; i < _inports; ++i)
      if(float* in = state->inPorts[i])
        for(unsigned long s = 0; s < n; ++s)
          in[s] = 0.25f * float(std::sin(2.0 * M_PI * 220.0 * double(state->tonePos + s) / double(MusEGlobal::sampleRate)));
    state->tonePos += n;
  }

  // Where the transport is.
  {
    unsigned int cur_frame = MusEGlobal::audio->pos().frame();
    unsigned int cur_tick = MusEGlobal::audio->tickPos();
    // Account for the latency correction, as the other plugin types do. The value is negative for correction.
    if(MusEGlobal::config.enableLatencyCorrection && !MusEGlobal::extSyncFlag && int(latency_corr) < 0)
    {
      cur_frame += (unsigned int)(-latency_corr);
      Pos ppp(cur_frame, false);
      cur_tick = ppp.tick();
    }
    MusEVst3::Instance::Transport t;
    t.playing = MusEGlobal::audio->isPlaying();
    t.sample = cur_frame;
    t.quarters = double(cur_tick) / double(MusEGlobal::config.division);
    const double tempo = MusEGlobal::tempomap.tempo(cur_tick);
    t.tempo = (double(MusEGlobal::tempomap.globalTempo()) * 600000.0) / tempo;
    int z, nn;
    MusEGlobal::sigmap.timesig(cur_tick, z, nn);
    t.timeSigNumerator = z;
    t.timeSigDenominator = nn;
    inst->setTransport(t);
  }

  // The plugin was set up for blocks of at most the audio buffer size. Do it in pieces if we get more.
  const unsigned long maxBlock = state->maxBlock > 0 ? (unsigned long)state->maxBlock : n;
  unsigned long done = 0;
  while(done < n)
  {
    const unsigned long len = std::min(n - done, maxBlock);
    bool connected = true;
    for(unsigned long i = 0; i < _inports; ++i)
    {
      connected = connected && state->inPorts[i];
      state->chunkIn[i] = state->inPorts[i] ? state->inPorts[i] + done : nullptr;
    }
    for(unsigned long o = 0; o < _outports; ++o)
    {
      connected = connected && state->outPorts[o];
      state->chunkOut[o] = state->outPorts[o] ? state->outPorts[o] + done : nullptr;
    }
    if(!connected)
      return;
    inst->process(_inports ? state->chunkIn.data() : nullptr, state->chunkOut.data(), int(len));
    done += len;
  }

  if(MusEGlobal::debugMsg)
  {
    float peak = state->peakOut.load(std::memory_order_relaxed);
    for(unsigned long o = 0; o < _outports; ++o)
      for(unsigned long s = 0; s < n; ++s)
        peak = std::max(peak, std::fabs(state->outPorts[o][s]));
    state->peakOut.store(peak, std::memory_order_relaxed);
    state->blocksProcessed.fetch_add(1, std::memory_order_relaxed);
  }

  // What the plugin did to its output parameters.
  if(!state->outControls.empty())
  {
    const uint32_t* ids = nullptr;
    const double* vals = nullptr;
    const int cnt = inst->outputParamChanges(&ids, &vals);
    for(int i = 0; i < cnt; ++i)
    {
      const auto it = _outIdToIdx.find(ids[i]);
      if(it != _outIdToIdx.end() && state->outControls[it->second])
        *state->outControls[it->second] = float(vals[i]);
    }
  }
}

float Vst3PluginWrapper::getPluginLatency(void* handle)
{
  Vst3PluginState* state = static_cast<Vst3PluginState*>(handle);
  return (state && state->inst) ? float(state->inst->latencySamples()) : 0.0f;
}

//---------------------------------------------------------
//   Ports. Audio inputs, audio outputs, control inputs, control outputs, in that order,
//    the same as the plugin scan.
//---------------------------------------------------------

int Vst3PluginWrapper::inControlIndex(unsigned long port) const
{
  const unsigned long first = _inports + _outports;
  if(port >= first && port < first + _controlInPorts)
    return int(port - first);
  return -1;
}

int Vst3PluginWrapper::outControlIndex(unsigned long port) const
{
  const unsigned long first = _inports + _outports + _controlInPorts;
  if(port >= first && port < first + _controlOutPorts)
    return int(port - first);
  return -1;
}

LADSPA_PortDescriptor Vst3PluginWrapper::portd(unsigned long k) const
{
  if(k < _inports)
    return LADSPA_PORT_INPUT | LADSPA_PORT_AUDIO;
  if(k < _inports + _outports)
    return LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO;
  if(k < _inports + _outports + _controlInPorts)
    return LADSPA_PORT_INPUT | LADSPA_PORT_CONTROL;
  return LADSPA_PORT_OUTPUT | LADSPA_PORT_CONTROL;
}

LADSPA_PortRangeHint Vst3PluginWrapper::range(unsigned long i) const
{
  LADSPA_PortRangeHint hint;
  hint.HintDescriptor = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
  hint.LowerBound = 0.0f;
  hint.UpperBound = 1.0f;
  const int c = inControlIndex(i);
  if(c >= 0 && size_t(c) < _inCtl.size() && _inCtl[c].stepCount == 1)
    hint.HintDescriptor |= LADSPA_HINT_TOGGLED;
  return hint;
}

void Vst3PluginWrapper::range(unsigned long, float* min, float* max) const
{
  // Parameter values are normalized in VST3. The plugin turns them into real values by itself.
  *min = 0.0f;
  *max = 1.0f;
}

double Vst3PluginWrapper::defaultValue(unsigned long port) const
{
  const int c = inControlIndex(port);
  if(c >= 0 && size_t(c) < _inCtl.size())
    return _inCtl[c].defaultNormalized;
  return 0.0;
}

const char* Vst3PluginWrapper::portName(unsigned long i) const
{
  return i < _portNames.size() ? _portNames[i].c_str() : "";
}

CtrlValueType Vst3PluginWrapper::ctrlValueType(unsigned long i) const
{
  const int c = inControlIndex(i);
  if(c >= 0 && size_t(c) < _inCtl.size() && _inCtl[c].stepCount == 1)
    return VAL_BOOL;
  return VAL_LINEAR;
}

CtrlList::Mode Vst3PluginWrapper::ctrlMode(unsigned long i) const
{
  const int c = inControlIndex(i);
  if(c >= 0 && size_t(c) < _inCtl.size() && _inCtl[c].stepCount == 1)
    return CtrlList::DISCRETE;
  return CtrlList::INTERPOLATE;
}

//---------------------------------------------------------
//   Native gui: the plugin's own editor
//---------------------------------------------------------

bool Vst3PluginWrapper::hasNativeGui() const
{
  // Whether the plugin has an editor. Whether we can show it is checked when it is asked for.
  return _hasGui;
}

void Vst3PluginWrapper::showNativeGui(PluginI* p, bool show)
{
  if(p->instances <= 0 || !hasNativeGui())
    return;
  Vst3PluginState* state = static_cast<Vst3PluginState*>(p->handle[0]);
  if(!state)
    return;

  if(show)
  {
    // The editors are X11 windows. They cannot be embedded in a Qt that uses another platform,
    //  for example native Wayland. Tell the user how to get them.
    if(QGuiApplication::platformName() != QLatin1String("xcb"))
    {
      QMessageBox::information(nullptr, QObject::tr("VST3 plugin editor"),
        QObject::tr("The editor of the VST3 plugin \"%1\" is an X11 window and can only be shown "
                    "when MusE runs on the X11 (xcb) Qt platform, which also works under Wayland through XWayland.\n\n"
                    "Start MusE with the environment variable QT_QPA_PLATFORM=xcb set to use it.\n\n"
                    "The current Qt platform is \"%2\".").arg(name(), QGuiApplication::platformName()));
      state->guiVisible = false;
      return;
    }
    if(!state->editor)
    {
      state->editor = new MusEVst3::Editor(*state->inst);
      state->editor->setCloseCallback([state]() { state->guiVisible = false; });
    }
    if(!state->editor->isOpen())
    {
      std::string err;
      if(!state->editor->open(err))
      {
        std::fprintf(stderr, "Vst3PluginWrapper::showNativeGui: %s: %s\n", name().toLocal8Bit().constData(), err.c_str());
        return;
      }
    }
    state->updateWindowTitle();
    state->editor->show();
    state->editor->raise();
    state->editor->activateWindow();
    state->guiVisible = true;
  }
  else if(state->editor)
  {
    state->editor->close();
    state->guiVisible = false;
  }
}

bool Vst3PluginWrapper::nativeGuiVisible(const PluginI* p) const
{
  if(p->instances <= 0)
    return false;
  const Vst3PluginState* state = static_cast<const Vst3PluginState*>(p->handle[0]);
  return state && state->guiVisible && state->editor && state->editor->isVisible();
}

void Vst3PluginWrapper::updateNativeGuiWindowTitle(const PluginI* p) const
{
  if(p->instances <= 0)
    return;
  Vst3PluginState* state = static_cast<Vst3PluginState*>(p->handle[0]);
  if(state)
    state->updateWindowTitle();
}

//---------------------------------------------------------
//   State
//---------------------------------------------------------

QString Vst3PluginWrapper::getCustomConfiguration(LADSPA_Handle handle)
{
  Vst3PluginState* state = static_cast<Vst3PluginState*>(handle);
  std::vector<uint8_t> blob;
  if(!state || !state->inst || !state->inst->saveState(blob))
    return QString();
  return QString(STATE_PREFIX) +
    QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(blob.data()), int(blob.size())).toBase64());
}

bool Vst3PluginWrapper::setCustomData(LADSPA_Handle handle, const std::vector<QString>& customParams)
{
  Vst3PluginState* state = static_cast<Vst3PluginState*>(handle);
  if(!state || !state->inst)
    return false;

  for(const QString& s : customParams)
  {
    if(!s.startsWith(STATE_PREFIX))
      continue;
    const QByteArray raw = QByteArray::fromBase64(s.mid(int(std::strlen(STATE_PREFIX))).toLatin1());
    const std::vector<uint8_t> blob(raw.begin(), raw.end());
    if(!state->inst->loadState(blob))
    {
      std::fprintf(stderr, "Vst3PluginWrapper::setCustomData: %s: the plugin did not accept its state\n",
                   name().toLocal8Bit().constData());
      return false;
    }
    // The state holds the values of the parameters. Let the control values of MusE follow them.
    int changed = 0;
    for(size_t c = 0; c < _inCtl.size() && c < state->inControls.size(); ++c)
    {
      const float v = float(state->inst->getParam(_inCtl[c].id));
      if(MusEGlobal::debugMsg && v != state->lastIn[c])
      {
        ++changed;
        std::fprintf(stderr, "Vst3PluginWrapper::setCustomData: %s: control %zu (%s) = %f\n",
                     name().toLocal8Bit().constData(), c, _inCtl[c].title.c_str(), double(v));
      }
      state->lastIn[c] = v;
      if(state->inControls[c])
        *state->inControls[c] = v;
    }
    if(MusEGlobal::debugMsg)
      std::fprintf(stderr, "Vst3PluginWrapper::setCustomData: %s: state of %d bytes restored, %d control(s) differ from the default\n",
                   name().toLocal8Bit().constData(), int(blob.size()), changed);
    return true;
  }
  return false;
}

//---------------------------------------------------------
//   Vst3Synth
//---------------------------------------------------------

Vst3Synth::Vst3Synth(const MusEPlugin::PluginScanInfoStruct& info)
 : Synth(info)
{
  _hasGui = info._pluginFlags & MusEPlugin::PluginHasGui;
  _scanInports = info._inports;
  _scanOutports = info._outports;
  _inports = info._inports;
  _outports = info._outports;
  _mainOutports = info._outports;
  _controlInPorts = info._controlInPorts;
  _infoLoaded = false;
}

bool Vst3Synth::reference()
{
  ++_references;
  return true;
}

int Vst3Synth::release()
{
  if(_references > 0)
    --_references;
  return _references;
}

SynthIF* Vst3Synth::createSIF(SynthI* s)
{
  Vst3SynthIF* sif = new Vst3SynthIF(s);
  if(!sif->init(this))
  {
    delete sif;
    sif = nullptr;
  }
  return sif;
}

//---------------------------------------------------------
//   Vst3SynthIF
//---------------------------------------------------------

Vst3SynthIF::Vst3SynthIF(SynthI* s) : SynthIF(s)
{
  _synth = nullptr;
  _editor = nullptr;
  _guiVisible = false;
  _controls = nullptr;
  _last = nullptr;
  _audioOutBuffers = nullptr;
  _audioInBuffers = nullptr;
  _audioInSilenceBuf = nullptr;
  _maxBlock = 0;
  _transportLatencyCorr = 0.0f;
  _testOffline = false;
  _dbgPeak = 0.0f;
  _dbgPorts = 0;
  _dbgCrossings = 0;
  _dbgSamples = 0;
  _dbgLastSample = 0.0f;
  _testBeats = 0;
  _ipcCallbackEvents = new LockFreeMPSCRingBuffer<PluginCallbackEventStruct>(256);
}

Vst3SynthIF::~Vst3SynthIF()
{
  if(_inst)
    std::fprintf(stderr, "ERROR: ~Vst3SynthIF: the plugin instance still exists!\n");

  if(_audioOutBuffers)
  {
    for(unsigned long i = 0; i < _synth->_outports; ++i)
      std::free(_audioOutBuffers[i]);
    delete[] _audioOutBuffers;
  }
  if(_audioInBuffers)
  {
    for(unsigned long i = 0; i < _synth->_inports; ++i)
      std::free(_audioInBuffers[i]);
    delete[] _audioInBuffers;
  }
  std::free(_audioInSilenceBuf);
  delete[] _controls;
  delete[] _last;
  delete _ipcCallbackEvents;
}

namespace {

// An aligned buffer of the audio buffer size, with silence (or the denormal bias).
float* newAudioBuffer()
{
  void* p = nullptr;
  if(posix_memalign(&p, 16, sizeof(float) * MusEGlobal::segmentSize) != 0)
  {
    std::fprintf(stderr, "ERROR: Vst3SynthIF: posix_memalign failed. Aborting!\n");
    std::abort();
  }
  float* f = static_cast<float*>(p);
  for(unsigned q = 0; q < MusEGlobal::segmentSize; ++q)
    f[q] = MusEGlobal::config.useDenormalBias ? MusEGlobal::denormalBias : 0.0f;
  return f;
}

} // anonymous namespace

bool Vst3SynthIF::init(Vst3Synth* s)
{
  _synth = s;

  MusEVst3::PluginInfo pi;
  pi.path = s->filePath().toStdString();
  pi.uid = s->uri().toStdString();
  pi.name = s->name().toStdString();
  std::string err;
  _inst = MusEVst3::Instance::create(pi, err);
  if(!_inst)
  {
    std::fprintf(stderr, "Vst3SynthIF::init: %s: %s\n", pi.name.c_str(), err.c_str());
    return false;
  }

  if(!s->_infoLoaded)
  {
    s->_inCtl = _inst->inputControls();
    s->_inIdToIdx.clear();
    for(size_t i = 0; i < s->_inCtl.size(); ++i)
      s->_inIdToIdx[s->_inCtl[i].id] = int(i);
    s->_infoLoaded = true;
  }
  if(s->_inCtl.size() != s->_controlInPorts)
  {
    std::fprintf(stderr, "Vst3SynthIF::init: %s: the plugin has %zu control ports, the plugin cache says %lu."
                 " Please rescan the plugins.\n", pi.name.c_str(), s->_inCtl.size(), s->_controlInPorts);
    _inst.reset();
    return false;
  }

  // Set up now, to learn the real channel counts. An instrument gets all of its outputs, and the sidechain
  //  inputs, which can be routed to the track like any other input.
  _setupOptions.allOutputBuses = true;
  _setupOptions.sidechainInputs = true;
  _maxBlock = MusEGlobal::segmentSize;
  if(!_inst->setup(MusEGlobal::sampleRate, _maxBlock, err, _setupOptions) ||
     _inst->mainOutputChannels() != int(s->_scanOutports) || _inst->mainInputChannels() != int(s->_scanInports))
  {
    std::fprintf(stderr, "Vst3SynthIF::init: %s: cannot set up: %s (main channels %d/%d, the plugin cache says %lu/%lu)\n",
                 pi.name.c_str(), err.c_str(), _inst->mainInputChannels(), _inst->mainOutputChannels(),
                 s->_scanInports, s->_scanOutports);
    _inst.reset();
    return false;
  }
  s->_inports = _inst->inputChannels();
  s->_outports = _inst->outputChannels();
  s->_mainOutports = _inst->mainOutputChannels();
  if(MusEGlobal::debugMsg)
  {
    std::fprintf(stderr, "Vst3SynthIF::init: %s: audio in:%lu out:%lu (main out:%lu), note expression types:%zu\n",
                 pi.name.c_str(), s->_inports, s->_outports, s->_mainOutports, _inst->noteExpressionTypes().size());
    for(const auto& b : _inst->audioBuses(false))
      std::fprintf(stderr, "  output bus %s: %d channel(s)%s\n", b.name.c_str(), b.channels, b.aux ? ", aux" : "");
    for(const auto& b : _inst->audioBuses(true))
      std::fprintf(stderr, "  input bus %s: %d channel(s)%s%s\n", b.name.c_str(), b.channels, b.aux ? ", aux" : "",
                   b.active ? "" : ", not used");
  }
  _inst->teardown();   // activate() sets up again, when the track is made active.

  _inst->setForwardEditsToProcessor(false);
  _inst->setEditCallback([this](const MusEVst3::EditEvent& ev) { pluginEdit(ev); });

  if(s->_outports != 0)
  {
    _audioOutBuffers = new float*[s->_outports];
    for(unsigned long k = 0; k < s->_outports; ++k)
      _audioOutBuffers[k] = newAudioBuffer();
  }
  if(s->_inports != 0)
  {
    _audioInBuffers = new float*[s->_inports];
    for(unsigned long k = 0; k < s->_inports; ++k)
      _audioInBuffers[k] = newAudioBuffer();
    _audioInSilenceBuf = newAudioBuffer();
  }

  const unsigned long controlPorts = s->_controlInPorts;
  if(controlPorts != 0)
  {
    _controls = new Port[controlPorts];
    _last = new float[controlPorts];
  }
  for(unsigned long i = 0; i < controlPorts; ++i)
  {
    const float plugin_val = float(_inst->getParam(s->_inCtl[i].id));
    _last[i] = plugin_val;
    _controls[i].idx = i;
    _controls[i].val = plugin_val;
    _controls[i].enCtrl = true;

    const int id = genACnum(MusECore::MAX_PLUGINS, i);
    CtrlList* cl;
    CtrlListList* cll = track()->controller();
    iCtrlList icl = cll->find(id);
    if(icl == cll->end())
    {
      cl = new CtrlList(id);
      cll->add(cl);
      cl->setCurVal(plugin_val);
    }
    else
    {
      cl = icl->second;
      // A value from the song. The audio thread sends it to the plugin, it differs from _last.
      _controls[i].val = float(cl->curVal());
    }
    setupController(cl);
  }

  activate();
  return true;
}

void Vst3SynthIF::activate()
{
  if(_curActiveState)
    return;
  std::string err;
  _maxBlock = MusEGlobal::segmentSize;
  if(!_inst->setup(MusEGlobal::sampleRate, _maxBlock, err, _setupOptions))
    std::fprintf(stderr, "Vst3SynthIF::activate: %s: %s\n", _synth->name().toLocal8Bit().constData(), err.c_str());
  else if(MusEGlobal::debugMsg)
    std::fprintf(stderr, "Vst3SynthIF::activate: %s: set up for %d samples at %d Hz, latency:%u\n",
                 _synth->name().toLocal8Bit().constData(), _maxBlock, MusEGlobal::sampleRate, _inst->latencySamples());
  SynthIF::activate();
}

void Vst3SynthIF::deactivate()
{
  if(!_curActiveState)
    return;
  SynthIF::deactivate();
  _inst->teardown();
}

void Vst3SynthIF::deactivate3()
{
  closeEditor();
  deactivate();
  if(_inst)
  {
    _inst->setEditCallback(nullptr);
    _inst.reset();
  }
}

void Vst3SynthIF::closeEditor()
{
  if(_editor)
  {
    delete _editor;   // The editor goes before the plugin does.
    _editor = nullptr;
  }
  _guiVisible = false;
}

//---------------------------------------------------------
//   Gui
//---------------------------------------------------------

bool Vst3SynthIF::hasNativeGui() const { return _synth && _synth->_hasGui; }

void Vst3SynthIF::showNativeGui(bool v)
{
  PluginIBase::showNativeGui(v);
  if(!hasNativeGui() || !_inst)
    return;

  if(v)
  {
    if(QGuiApplication::platformName() != QLatin1String("xcb"))
    {
      QMessageBox::information(nullptr, QObject::tr("VST3 plugin editor"),
        QObject::tr("The editor of the VST3 plugin \"%1\" is an X11 window and can only be shown "
                    "when MusE runs on the X11 (xcb) Qt platform, which also works under Wayland through XWayland.\n\n"
                    "Start MusE with the environment variable QT_QPA_PLATFORM=xcb set to use it.\n\n"
                    "The current Qt platform is \"%2\".").arg(_synth->name(), QGuiApplication::platformName()));
      _guiVisible = false;
      return;
    }
    if(!_editor)
    {
      _editor = new MusEVst3::Editor(*_inst);
      _editor->setCloseCallback([this]() { _guiVisible = false; });
    }
    if(!_editor->isOpen())
    {
      std::string err;
      if(!_editor->open(err))
      {
        std::fprintf(stderr, "Vst3SynthIF::showNativeGui: %s: %s\n", _synth->name().toLocal8Bit().constData(), err.c_str());
        return;
      }
    }
    updateNativeGuiWindowTitle();
    _editor->show();
    _editor->raise();
    _editor->activateWindow();
    _guiVisible = true;
  }
  else if(_editor)
  {
    _editor->close();
    _guiVisible = false;
  }
}

void Vst3SynthIF::getNativeGeometry(int* x, int* y, int* w, int* h) const
{
  if(!_editor)
  {
    *x = *y = *w = *h = 0;
    return;
  }
  *x = _editor->x();
  *y = _editor->y();
  *w = _editor->width();
  *h = _editor->height();
}

void Vst3SynthIF::setNativeGeometry(int x, int y, int, int)
{
  // The size is up to the plugin. Only the place is ours.
  if(_editor)
    _editor->move(x, y);
}

void Vst3SynthIF::updateNativeGuiWindowTitle()
{
  if(_editor)
    _editor->setWindowTitle(displayName());
}

void Vst3SynthIF::guiHeartBeat()
{
  SynthIF::guiHeartBeat();
  if(!_inst)
    return;

  if(MusEGlobal::debugMsg)
  {
    ++_testBeats;
    // Report what is produced about every two seconds, as a rough pitch.
    if(_testBeats % 40 == 0)
    {
      const unsigned samples = _dbgSamples.exchange(0);
      const unsigned crossings = _dbgCrossings.exchange(0);
      const float peak = _dbgPeak.exchange(0.0f);
      if(samples > 0)
        std::fprintf(stderr, "Vst3SynthIF: %s: output peak %f, about %.1f Hz (%d output buffers asked for)\n",
                     _synth->name().toLocal8Bit().constData(), double(peak),
                     double(crossings) * double(MusEGlobal::sampleRate) / double(samples), _dbgPorts.load());
    }
    // For trying the offline mode without exporting: MUSE_VST3_TEST_FREEWHEEL=1 is offline for a while.
    if(qEnvironmentVariableIsSet("MUSE_VST3_TEST_FREEWHEEL"))
    {
      if(_testBeats == 100) _testOffline = true;
      if(_testBeats == 200) _testOffline = false;
    }
    // For trying without a keyboard: MUSE_VST3_TEST_NOTE=1 plays a note, bends it up and stops it.
    if(qEnvironmentVariableIsSet("MUSE_VST3_TEST_NOTE"))
    {
      const int port = synti->midiPort();
      if(_testBeats == 50)
        synti->putEvent(MidiPlayEvent(0, port, 0, ME_NOTEON, 60, 100), MidiDevice::NotLate);
      else if(_testBeats == 130)
        synti->putEvent(MidiPlayEvent(0, port, 0, ME_CONTROLLER, CTRL_PITCH, 8191), MidiDevice::NotLate);
      else if(_testBeats == 90)
        synti->putEvent(MidiPlayEvent(0, port, 0, ME_PROGRAM, 3, 0), MidiDevice::NotLate);
      else if(_testBeats == 250)
        synti->putEvent(MidiPlayEvent(0, port, 0, ME_NOTEOFF, 60, 0), MidiDevice::NotLate);
    }
  }
  _inst->applyControllerUpdates();
  const int flags = _inst->takeRestartFlags();
  if(flags & kRestartParamValuesChanged)
  {
    // The plugin changed its parameters by itself, for example by a program change. Follow it.
    for(unsigned long c = 0; c < _synth->_controlInPorts; ++c)
    {
      const float v = float(_inst->getParam(_synth->_inCtl[c].id));
      if(v != _controls[c].val)
        addIpcCallbackEvent(PluginCallbackEventStruct(PluginCallbackEventStruct::AutomationEditType, c, v));
    }
  }
}

void Vst3SynthIF::pluginEdit(const MusEVst3::EditEvent& ev)
{
  const auto it = _synth->_inIdToIdx.find(ev.id);
  if(it == _synth->_inIdToIdx.end())
    return;
  const unsigned long idx = it->second;
  const float value = float(ev.value);
  const int plugId = id();

  switch(ev.type)
  {
    case MusEVst3::EditEvent::Begin:
      if(plugId != -1)
        synti->startAutoRecord(genACnum(plugId, idx), value);
      addIpcCallbackEvent(PluginCallbackEventStruct(PluginCallbackEventStruct::AutomationBeginType, idx, value));
    break;

    case MusEVst3::EditEvent::Perform:
      if(plugId != -1)
        synti->recordAutomation(genACnum(plugId, idx), value);
      addIpcCallbackEvent(PluginCallbackEventStruct(PluginCallbackEventStruct::AutomationEditType, idx, value));
    break;

    case MusEVst3::EditEvent::End:
    {
      const AutomationType at = synti->automationType();
      if(plugId != -1)
        synti->stopAutoRecord(genACnum(plugId, idx), value);
      if(at == AUTO_OFF || (at == AUTO_READ && MusEGlobal::audio->isPlaying()) || at == AUTO_TOUCH)
        addIpcCallbackEvent(PluginCallbackEventStruct(PluginCallbackEventStruct::AutomationEndType, idx, value));
    }
    break;
  }
}

//---------------------------------------------------------
//   Channels, programs
//---------------------------------------------------------

MidiPlayEvent Vst3SynthIF::receiveEvent() { return MidiPlayEvent(); }

int Vst3SynthIF::channels() const
{
  // The track gets the main bus. The channels of the other buses can be routed from the synth's outputs.
  return _synth->_mainOutports > (unsigned long)MusECore::MAX_CHANNELS ? MusECore::MAX_CHANNELS : int(_synth->_mainOutports);
}
int Vst3SynthIF::totalOutChannels() const { return int(_synth->_outports); }
int Vst3SynthIF::totalInChannels() const  { return int(_synth->_inports); }

// MusE numbers programs as (high bank << 16) | (low bank << 8) | program, with 7 bits each.
// The programs of the plugin are numbered in a row, so they use all of that.
static int vst3ProgramIndex(int bankH, int bankL, int prog)
{
  if(bankH > 127) bankH = 0;
  if(bankL > 127) bankL = 0;
  if(prog > 127) prog = 0;
  return (bankH << 14) | (bankL << 7) | prog;
}

void Vst3SynthIF::doSelectProgram(int bankH, int bankL, int prog, unsigned ft)
{
  if(!_inst)
    return;
  const int p = vst3ProgramIndex(bankH, bankL, prog);
  const bool ok = _inst->selectProgramRT(p, int(ft));
  // Printing is not for the audio thread, but this is only with the debug option.
  if(MusEGlobal::debugMsg)
    std::fprintf(stderr, ok ? "Vst3SynthIF::doSelectProgram: program %d: %s\n" : "Vst3SynthIF::doSelectProgram: program %d is not there\n",
                 p, _inst->programName(p).c_str());
}

QString Vst3SynthIF::getPatchName(int /*chan*/, int prog, bool /*drum*/) const
{
  if(!_inst)
    return "?";
  const int p = vst3ProgramIndex((prog >> 16) & 0xff, (prog >> 8) & 0xff, prog & 0xff);
  if(p < _inst->programCount())
    return QString::fromStdString(_inst->programName(p));
  return "?";
}

void Vst3SynthIF::populatePatchPopup(MusEGui::PopupMenu* menu, int /*chan*/, bool /*drum*/)
{
  menu->clear();
  if(!_inst)
    return;
  const int n = _inst->programCount();
  for(int i = 0; i < n; ++i)
  {
    const int prog = (((i >> 14) & 0x7f) << 16) | (((i >> 7) & 0x7f) << 8) | (i & 0x7f);
    QAction* act = menu->addAction(QString::number(i + 1) + QString(" ") + QString::fromStdString(_inst->programName(i)));
    act->setData(prog);
  }
}

//---------------------------------------------------------
//   Parameters
//---------------------------------------------------------

double Vst3SynthIF::getParameter(unsigned long idx) const
{
  return (_controls && idx < _synth->_controlInPorts) ? _controls[idx].val : 0.0;
}

void Vst3SynthIF::setParameter(unsigned long idx, double value)
{
  // The audio thread sees that it differs from what the plugin was told, and tells the plugin.
  if(_controls && idx < _synth->_controlInPorts)
    _controls[idx].val = float(value);
}

unsigned long Vst3SynthIF::pluginID() const { return _synth ? _synth->id() : 0; }
void Vst3SynthIF::enableController(unsigned long i, bool v) { if(_controls) _controls[i].enCtrl = v; }
bool Vst3SynthIF::controllerEnabled(unsigned long i) const { return _controls ? _controls[i].enCtrl : true; }

void Vst3SynthIF::enableAllControllers(bool v)
{
  if(!_synth || !_controls)
    return;
  for(unsigned long i = 0; i < _synth->_controlInPorts; ++i)
    _controls[i].enCtrl = v;
}

void Vst3SynthIF::updateController(unsigned long i)
{
  if(!_synth || !_controls || !synti || id() < 0)
    return;
  const float v = param(i);
  _controls[i].val = v;
  synti->setPluginCtrlVal(genACnum(id(), i), v);
}

void Vst3SynthIF::updateControllers()
{
  if(!_synth)
    return;
  for(unsigned long i = 0; i < _synth->_controlInPorts; ++i)
    updateController(i);
}

unsigned long Vst3SynthIF::parameters() const    { return _synth ? _synth->_controlInPorts : 0; }
unsigned long Vst3SynthIF::parametersOut() const { return 0; }
void Vst3SynthIF::setParam(unsigned long i, double val) { setParameter(i, val); }
double Vst3SynthIF::param(unsigned long i) const { return getParameter(i); }
double Vst3SynthIF::paramOut(unsigned long) const { return 0.0; }

const char* Vst3SynthIF::paramName(unsigned long i) const
{
  return (_synth && i < _synth->_inCtl.size()) ? _synth->_inCtl[i].title.c_str() : "";
}

const char* Vst3SynthIF::paramOutName(unsigned long) const { return nullptr; }

LADSPA_PortRangeHint Vst3SynthIF::range(unsigned long i) const
{
  LADSPA_PortRangeHint h;
  h.HintDescriptor = LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_BOUNDED_ABOVE;
  h.LowerBound = 0.0f;
  h.UpperBound = 1.0f;
  if(_synth && i < _synth->_inCtl.size() && _synth->_inCtl[i].stepCount == 1)
    h.HintDescriptor |= LADSPA_HINT_TOGGLED;
  return h;
}

LADSPA_PortRangeHint Vst3SynthIF::rangeOut(unsigned long) const
{
  LADSPA_PortRangeHint h;
  h.HintDescriptor = 0;
  h.LowerBound = 0.0f;
  h.UpperBound = 1.0f;
  return h;
}

void Vst3SynthIF::range(unsigned long, float* min, float* max) const { *min = 0.0f; *max = 1.0f; }
void Vst3SynthIF::rangeOut(unsigned long, float* min, float* max) const { *min = 0.0f; *max = 1.0f; }

CtrlValueType Vst3SynthIF::ctrlValueType(unsigned long i) const
{
  return (_synth && i < _synth->_inCtl.size() && _synth->_inCtl[i].stepCount == 1) ? VAL_BOOL : VAL_LINEAR;
}

CtrlList::Mode Vst3SynthIF::ctrlMode(unsigned long i) const
{
  return (_synth && i < _synth->_inCtl.size() && _synth->_inCtl[i].stepCount == 1) ? CtrlList::DISCRETE : CtrlList::INTERPOLATE;
}

CtrlValueType Vst3SynthIF::ctrlOutValueType(unsigned long) const { return VAL_LINEAR; }
CtrlList::Mode Vst3SynthIF::ctrlOutMode(unsigned long) const { return CtrlList::INTERPOLATE; }

bool Vst3SynthIF::usesTransportSource() const { return true; }

//---------------------------------------------------------
//   State
//---------------------------------------------------------

std::vector<QString> Vst3SynthIF::getCustomData() const
{
  std::vector<QString> v;
  std::vector<uint8_t> blob;
  if(_inst && _inst->saveState(blob))
    v.push_back(QString(STATE_PREFIX) +
      QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(blob.data()), int(blob.size())).toBase64()));
  return v;
}

bool Vst3SynthIF::setCustomData(const std::vector<QString>& customParams)
{
  if(!_inst)
    return false;
  for(const QString& s : customParams)
  {
    if(!s.startsWith(STATE_PREFIX))
      continue;
    const QByteArray raw = QByteArray::fromBase64(s.mid(int(std::strlen(STATE_PREFIX))).toLatin1());
    const std::vector<uint8_t> blob(raw.begin(), raw.end());
    if(!_inst->loadState(blob))
    {
      std::fprintf(stderr, "Vst3SynthIF::setCustomData: %s: the plugin did not accept its state\n",
                   _synth->name().toLocal8Bit().constData());
      return false;
    }
    // The state holds the values of the parameters. Let the controls of MusE follow them. The plugin
    //  already has them, so the audio thread must not send them again.
    for(unsigned long c = 0; c < _synth->_controlInPorts; ++c)
    {
      const float v = float(_inst->getParam(_synth->_inCtl[c].id));
      if(MusEGlobal::debugMsg && v != _last[c])
        std::fprintf(stderr, "Vst3SynthIF::setCustomData: %s: control %lu (%s) = %f\n",
                     _synth->name().toLocal8Bit().constData(), c, _synth->_inCtl[c].title.c_str(), double(v));
      _last[c] = v;
      _controls[c].val = v;
      if(id() >= 0)
        synti->setPluginCtrlVal(genACnum(id(), c), v);
    }
    if(MusEGlobal::debugMsg)
      std::fprintf(stderr, "Vst3SynthIF::setCustomData: %s: state of %d bytes restored\n",
                   _synth->name().toLocal8Bit().constData(), int(blob.size()));
    return true;
  }
  return false;
}

//---------------------------------------------------------
//   Midi
//---------------------------------------------------------

void Vst3SynthIF::processEvent(const MidiPlayEvent& e, unsigned ft)
{
  const int type = e.type();
  const int chn = e.channel() & 0x0f;
  const int a = e.dataA();
  const int b = e.dataB();
  const MidiInstrument::NoteOffMode nom = synti->noteOffMode();

  auto controller = [&](int cc, double value)
  {
    _inst->midiController(chn, cc, std::min(1.0, std::max(0.0, value)), int(ft));
  };

  switch(type)
  {
    case ME_NOTEON:
      if(b == 0)
      {
        switch(nom)
        {
          case MidiInstrument::NoteOffAll:
            _inst->noteOff(chn, a & 0x7f, 0.0f, int(ft));
          break;
          case MidiInstrument::NoteOffNone:
          case MidiInstrument::NoteOffConvertToZVNoteOn:
            // The plugin has no 'note on with zero velocity', it is a note off here as well.
            _inst->noteOff(chn, a & 0x7f, 0.0f, int(ft));
          break;
        }
      }
      else
        _inst->noteOn(chn, a & 0x7f, float(b & 0x7f) / 127.0f, int(ft));
    break;

    case ME_NOTEOFF:
      if(nom != MidiInstrument::NoteOffNone)
        _inst->noteOff(chn, a & 0x7f, float(b & 0x7f) / 127.0f, int(ft));
    break;

    case ME_PROGRAM:
    {
      int hb, lb;
      synti->currentProg(chn, nullptr, &lb, &hb);
      synti->setCurrentProg(chn, a & 0xff, lb, hb);
      doSelectProgram(hb, lb, a, ft);
    }
    break;

    case ME_CONTROLLER:
    {
      if(b == CTRL_VAL_UNKNOWN)
        return;
      if(a == CTRL_PROGRAM)
      {
        const int bankH = (b >> 16) & 0xff;
        const int bankL = (b >> 8) & 0xff;
        const int prog = b & 0xff;
        synti->setCurrentProg(chn, prog, bankL, bankH);
        doSelectProgram(bankH, bankL, prog, ft);
        return;
      }
      if(a == CTRL_HBANK)
      {
        int lb, pr;
        synti->currentProg(chn, &pr, &lb, nullptr);
        synti->setCurrentProg(chn, pr, lb, b & 0xff);
        doSelectProgram(b, lb, pr, ft);
        return;
      }
      if(a == CTRL_LBANK)
      {
        int hb, pr;
        synti->currentProg(chn, &pr, nullptr, &hb);
        synti->setCurrentProg(chn, pr, b & 0xff, hb);
        doSelectProgram(hb, b, pr, ft);
        return;
      }
      if(a == CTRL_PITCH)
      {
        controller(129, double(b + 8192) / 16383.0);
        return;
      }
      if(a == CTRL_AFTERTOUCH)
      {
        controller(128, double(b & 0x7f) / 127.0);
        return;
      }
      if((a | 0xff) == CTRL_POLYAFTER)
      {
        _inst->polyPressure(chn, a & 0x7f, float(b & 0x7f) / 127.0f, int(ft));
        return;
      }
      // Any other controller, if the plugin wants it.
      if(a >= 0 && a < 128)
        controller(a, double(b & 0x7f) / 127.0);
    }
    break;

    case ME_PITCHBEND:
      controller(129, double(a + 8192) / 16383.0);
    break;

    case ME_AFTERTOUCH:
      controller(128, double(a & 0x7f) / 127.0);
    break;

    case ME_POLYAFTER:
      _inst->polyPressure(chn, a & 0x7f, float(b & 0x7f) / 127.0f, int(ft));
    break;

    case ME_SYSEX:
      // Not for VST3 plugins for now.
    break;

    default:
      if(MusEGlobal::debugMsg)
        std::fprintf(stderr, "Vst3SynthIF::processEvent: unknown midi event type:%d\n", type);
    break;
  }
}

//---------------------------------------------------------
//   getData
//    Based on the one of the native VST synths. Splits the cycle into slices where controller
//     values change, and gives the plugin the midi events of each slice.
//---------------------------------------------------------

bool Vst3SynthIF::getData(MidiPort* /*mp*/, unsigned pos, int ports, unsigned nframes, float** buffer)
{
  const unsigned int syncFrame = MusEGlobal::audio->curSyncFrame();
  if(MusEGlobal::debugMsg)
    _dbgPorts.store(ports, std::memory_order_relaxed);

  const unsigned long in_ports = _synth->_inports;
  const unsigned long out_ports = _synth->_outports;
  const unsigned long nop = ((unsigned long)ports) > out_ports ? out_ports : ((unsigned long)ports);
  unsigned long sample = 0;

  const bool isOn = on();
  const MusEPlugin::PluginBypassType bypassType = pluginBypassType();

  // Offline (freewheel, for example when exporting) or real time. The plugin is set up again if that
  //  changes, which is fine at such a transition.
  if(_curActiveState)
  {
    const bool wantOffline = MusEGlobal::audio->freewheel() || _testOffline;
    if(wantOffline != _inst->offline())
    {
      _inst->setOffline(wantOffline);
      if(MusEGlobal::debugMsg)
        std::fprintf(stderr, "Vst3SynthIF::getData: %s: %s processing\n", _synth->name().toLocal8Bit().constData(),
                     wantOffline ? "offline" : "real time");
    }
  }

  // Normally if the plugin is inactive or off we connect it to dummy audio ports and use a fixed
  //  controller period. This can change depending on the bypass type.
  bool connectToDummyAudioPorts = !_curActiveState || !isOn;
  bool usefixedrate = !_curActiveState || !isOn;
  const unsigned int fin_nsamp = nframes;

  if(_curActiveState)
  {
    switch(bypassType)
    {
      case MusEPlugin::PluginBypassTypeEmulatedEnableController:
      break;
      case MusEPlugin::PluginBypassTypeEnablePort:
      case MusEPlugin::PluginBypassTypeBypassPort:
        connectToDummyAudioPorts = false;
        usefixedrate = false;
      break;
      case MusEPlugin::PluginBypassTypeEmulatedEnableFunction:
      break;
      case MusEPlugin::PluginBypassTypeEnableFunction:
      case MusEPlugin::PluginBypassTypeBypassFunction:
        connectToDummyAudioPorts = false;
      break;
    }
  }

  const unsigned long min_per =
    (usefixedrate || MusEGlobal::config.minControlProcessPeriod > nframes) ? nframes : MusEGlobal::config.minControlProcessPeriod;
  const unsigned long min_per_mask = min_per - 1;   // min_per must be power of 2

  AudioTrack* atrack = track();

  _transportLatencyCorr = 0.0f;
  if(atrack)
  {
    TransportSource& ts = atrack->transportSource();
    const TrackLatencyInfo& li = ts.getLatencyInfo(false);
    if(li._canCorrectOutputLatency)
      _transportLatencyCorr = li._sourceCorrectionValue;
  }

  const AutomationType at = atrack ? atrack->automationType() : AUTO_OFF;
  const bool no_auto = !MusEGlobal::automation || at == AUTO_OFF;
  const unsigned long in_ctrls = _synth->_controlInPorts;
  CtrlListList* cll = atrack->controller();
  ciCtrlList icl_first;
  const int plug_id = id();
  if(plug_id != -1)
    icl_first = cll->lower_bound(genACnum(plug_id, 0));

  // Audio inputs from the routes, if the plugin has any.
  bool used_in_chan_array[in_ports ? in_ports : 1];
  if(_curActiveState)
  {
    for(unsigned long i = 0; i < in_ports; ++i)
      used_in_chan_array[i] = false;
    if(!atrack->noInRoute())
    {
      RouteList* irl = atrack->inRoutes();
      for(ciRoute i = irl->begin(); i != irl->end(); ++i)
      {
        if(i->track->isMidiTrack())
          continue;
        const int dst_ch = i->channel <= -1 ? 0 : i->channel;
        if((unsigned long)dst_ch >= in_ports)
          continue;
        const int dst_chs = i->channels <= -1 ? in_ports : i->channels;
        const int src_ch = i->remoteChannel <= -1 ? 0 : i->remoteChannel;
        const int src_chs = i->channels;
        int fin_dst_chs = dst_chs;
        if((unsigned long)(dst_ch + fin_dst_chs) > in_ports)
          fin_dst_chs = in_ports - dst_ch;
        static_cast<AudioTrack*>(i->track)->copyData(pos, dst_ch, dst_chs, fin_dst_chs, src_ch, src_chs,
                                                     nframes, &_audioInBuffers[0], false, used_in_chan_array);
        const int nxt_ch = dst_ch + fin_dst_chs;
        for(int ch = dst_ch; ch < nxt_ch; ++ch)
          used_in_chan_array[ch] = true;
      }
    }
  }

  // Events sent to us by the plugin's callbacks, before the other controller handling.
  if(_ipcCallbackEvents)
  {
    const unsigned sz = _ipcCallbackEvents->getSize();
    for(unsigned i = 0; i < sz; ++i)
    {
      PluginCallbackEventStruct ev;
      if(!_ipcCallbackEvents->get(ev))
        continue;
      switch(ev._type)
      {
        case PluginCallbackEventStruct::AutomationBeginType:
          if(plug_id != -1)
            synti->setPluginCtrlVal(genACnum(plug_id, ev._id), ev._value);
          // Disable the automation controller stream while a control is being manipulated.
          enableController(ev._id, false);
        break;

        case PluginCallbackEventStruct::AutomationEditType:
          if(plug_id != -1)
            synti->setPluginCtrlVal(genACnum(plug_id, ev._id), ev._value);
        break;

        case PluginCallbackEventStruct::AutomationEndType:
          if(plug_id != -1)
            synti->setPluginCtrlVal(genACnum(plug_id, ev._id), ev._value);
          enableController(ev._id, true);
        break;

        case PluginCallbackEventStruct::UpdateDisplayType:
        case PluginCallbackEventStruct::NoType:
        break;
      }
    }
  }

  int cur_slice = 0;
  while(sample < fin_nsamp)
  {
    unsigned long slice_samps = fin_nsamp - sample;
    const unsigned long slice_frame = pos + sample;

    //-------------------------------------------------------------
    // Process automation control values, while also determining the
    //  maximum acceptable size of this run.
    //-------------------------------------------------------------
    {
      ciCtrlList icl = icl_first;
      for(unsigned long k = 0; k < in_ctrls; ++k)
      {
        CtrlList* cl = (cll && plug_id != -1 && icl != cll->end()) ? icl->second : nullptr;
        CtrlInterpolate& ci = _controls[k].interp;
        // Always refresh the interpolation member at first slice, and after the user changes a control.
        if(cur_slice == 0 || (!ci.eStop && MusEGlobal::audio->isPlaying() &&
            (slice_frame < (unsigned long)ci.sFrame || (ci.eFrameValid && slice_frame >= (unsigned long)ci.eFrame))))
        {
          if(cl && plug_id != -1 && (unsigned long)cl->id() == genACnum(plug_id, k))
          {
            cl->getInterpolation(slice_frame, no_auto || !_controls[k].enCtrl, &ci);
            if(icl != cll->end())
              ++icl;
          }
          else
          {
            // No controller for this control. Use the current value.
            ci.sFrame = 0;
            ci.eFrame = 0;
            ci.eFrameValid = false;
            ci.sVal = _controls[k].val;
            ci.eVal = ci.sVal;
            ci.doInterp = false;
            ci.eStop = false;
          }
        }
        else
        {
          if(ci.eStop && ci.eFrameValid && slice_frame >= (unsigned long)ci.eFrame)
          {
            ci.sFrame = 0;
            ci.eFrame = 0;
            ci.eFrameValid = false;
            ci.sVal = ci.eVal;
            ci.doInterp = false;
            ci.eStop = false;
          }
          if(cl && cll && icl != cll->end())
            ++icl;
        }

        if(!usefixedrate && MusEGlobal::audio->isPlaying())
        {
          unsigned long samps = slice_samps;
          if(ci.eFrameValid)
            samps = (unsigned long)ci.eFrame - slice_frame;

          if(!ci.doInterp && samps > min_per)
          {
            samps &= ~min_per_mask;
            if((samps & min_per_mask) != 0)
              samps += min_per;
          }
          else
            samps = min_per;

          if(samps < slice_samps)
            slice_samps = samps;
        }

        float new_val;
        if(ci.doInterp && cl)
          new_val = cl->interpolate(MusEGlobal::audio->isPlaying() ? slice_frame : pos, ci);
        else
          new_val = ci.sVal;

        _controls[k].val = new_val;
      }
    }

    //------------------------------------------------------------
    // Control FIFO items (from the GUI, midi controller mappings).
    //------------------------------------------------------------
    bool found = false;
    unsigned long frame = 0;
    unsigned long index = 0;
    unsigned long evframe;
    while(!_controlFifo.isEmpty())
    {
      const ControlEvent& v = _controlFifo.peek();
      evframe = (syncFrame > v.frame + nframes) ? 0 : v.frame - syncFrame + nframes;

      if(found && evframe < frame)
      {
        _controlFifo.remove();
        continue;
      }

      if(evframe >= nframes
         || (!usefixedrate && !found && !v.unique && (evframe - sample >= slice_samps))
         || (found && !v.unique && (evframe - sample >= min_per))
         || (usefixedrate && found && v.unique && v.idx == index))
        break;

      if(v.idx >= in_ctrls)
      {
        _controlFifo.remove();
        break;
      }

      found = true;
      frame = evframe;
      index = v.idx;

      {
        CtrlInterpolate* ci = &_controls[v.idx].interp;
        ci->eFrame = frame;
        ci->eFrameValid = true;
        ci->eVal = v.value;
        ci->eStop = true;
      }

      if(plug_id != -1)
        synti->setPluginCtrlVal(genACnum(plug_id, v.idx), v.value);

      _controlFifo.remove();
    }

    if(found && !usefixedrate)
      slice_samps = frame - sample;

    if(sample + slice_samps > nframes)
      slice_samps = nframes - sample;

    if(slice_samps != 0)
    {
      // Control values that changed go to the plugin, for the start of this slice.
      if(_curActiveState)
        for(unsigned long k = 0; k < in_ctrls; ++k)
          if(_controls[k].val != _last[k])
          {
            _last[k] = _controls[k].val;
            _inst->queueParamRT(_synth->_inCtl[k].id, _controls[k].val);
            _inst->controllerUpdateRT(_synth->_inCtl[k].id, _controls[k].val);
          }

      //----------------------------------
      // Midi events for this slice.
      //----------------------------------
      {
        const bool do_stop = synti->stopFlag();
        const bool we = synti->writeEnable();
        MidiPlayEvent buf_ev;

        if(do_stop || !_curActiveState || !we)
        {
          // Transfer the user event buffer to the user event list so it is not lost, and drop the playback events.
          const unsigned int usr_buf_sz = synti->eventBuffers(MidiDevice::UserBuffer)->getSize();
          for(unsigned int i = 0; i < usr_buf_sz; ++i)
            if(synti->eventBuffers(MidiDevice::UserBuffer)->get(buf_ev))
              synti->_outUserEvents.addExclusive(buf_ev);
          synti->eventBuffers(MidiDevice::PlaybackBuffer)->clearRead();
          synti->_outPlaybackEvents.clear();
          synti->setStopFlag(false);
        }
        else
        {
          const unsigned int usr_buf_sz = synti->eventBuffers(MidiDevice::UserBuffer)->getSize();
          for(unsigned int i = 0; i < usr_buf_sz; ++i)
            if(synti->eventBuffers(MidiDevice::UserBuffer)->get(buf_ev))
              synti->_outUserEvents.insert(buf_ev);
          const unsigned int pb_buf_sz = synti->eventBuffers(MidiDevice::PlaybackBuffer)->getSize();
          for(unsigned int i = 0; i < pb_buf_sz; ++i)
            if(synti->eventBuffers(MidiDevice::PlaybackBuffer)->get(buf_ev))
              synti->_outPlaybackEvents.insert(buf_ev);
        }

        if(_curActiveState && we)
        {
          iMPEvent impe_pb = synti->_outPlaybackEvents.begin();
          iMPEvent impe_us = synti->_outUserEvents.begin();
          bool using_pb;

          while(1)
          {
            if(impe_pb != synti->_outPlaybackEvents.end() && impe_us != synti->_outUserEvents.end())
              using_pb = *impe_pb < *impe_us;
            else if(impe_pb != synti->_outPlaybackEvents.end())
              using_pb = true;
            else if(impe_us != synti->_outUserEvents.end())
              using_pb = false;
            else
              break;

            const MidiPlayEvent& e = using_pb ? *impe_pb : *impe_us;

            // Event is for a later slice or period.
            if(e.time() >= (sample + slice_samps + syncFrame))
              break;

            unsigned int ft = (e.time() < syncFrame) ? 0 : e.time() - syncFrame;
            ft = (ft < sample) ? 0 : ft - sample;
            if(ft >= slice_samps)
              ft = slice_samps - 1;

            processEvent(e, ft);

            if(using_pb)
              impe_pb = synti->_outPlaybackEvents.erase(impe_pb);
            else
              impe_us = synti->_outUserEvents.erase(impe_us);
          }
        }
      }

      //----------------------------------
      // Process the audio of this slice.
      //----------------------------------
      if(_curActiveState && _inst->isActive())
      {
        const float* in_bufs[in_ports ? in_ports : 1];
        float* out_bufs[out_ports ? out_ports : 1];

        for(unsigned long k = 0; k < out_ports; ++k)
          out_bufs[k] = (!connectToDummyAudioPorts && k < nop) ? buffer[k] + sample : _audioOutBuffers[k] + sample;
        for(unsigned long k = 0; k < in_ports; ++k)
          in_bufs[k] = (!connectToDummyAudioPorts && used_in_chan_array[k]) ? _audioInBuffers[k] + sample
                                                                            : _audioInSilenceBuf + sample;

        // Where the transport is.
        {
          unsigned int cur_frame = MusEGlobal::audio->pos().frame();
          unsigned int cur_tick = MusEGlobal::audio->tickPos();
          if(MusEGlobal::config.enableLatencyCorrection && !MusEGlobal::extSyncFlag && int(_transportLatencyCorr) < 0)
          {
            cur_frame += (unsigned int)(-_transportLatencyCorr);
            Pos ppp(cur_frame, false);
            cur_tick = ppp.tick();
          }
          MusEVst3::Instance::Transport t;
          t.playing = MusEGlobal::audio->isPlaying();
          t.sample = cur_frame;
          t.quarters = double(cur_tick) / double(MusEGlobal::config.division);
          const double tempo = MusEGlobal::tempomap.tempo(cur_tick);
          t.tempo = (double(MusEGlobal::tempomap.globalTempo()) * 600000.0) / tempo;
          int z, nn;
          MusEGlobal::sigmap.timesig(cur_tick, z, nn);
          t.timeSigNumerator = z;
          t.timeSigDenominator = nn;
          _inst->setTransport(t);
        }

        _inst->process(in_ports ? in_bufs : nullptr, out_bufs, int(slice_samps));

        if(MusEGlobal::debugMsg && out_ports > 0)
        {
          float peak = _dbgPeak.load(std::memory_order_relaxed);
          unsigned zc = 0;
          float last = _dbgLastSample;
          for(unsigned long s = 0; s < slice_samps; ++s)
          {
            const float v = out_bufs[0][s];
            peak = std::max(peak, std::fabs(v));
            if(last < 0.0f && v >= 0.0f)
              ++zc;
            last = v;
          }
          _dbgLastSample = last;
          _dbgPeak.store(peak, std::memory_order_relaxed);
          _dbgCrossings.fetch_add(zc, std::memory_order_relaxed);
          _dbgSamples.fetch_add(unsigned(slice_samps), std::memory_order_relaxed);
        }
      }

      sample += slice_samps;
    }

    ++cur_slice;
  }

  return true;
}

//---------------------------------------------------------
//   initVST3
//---------------------------------------------------------

void initVST3()
{
  const MusEPlugin::PluginScanList& scan_list = MusEPlugin::pluginList;
  for(MusEPlugin::ciPluginScanList isl = scan_list.begin(); isl != scan_list.end(); ++isl)
  {
    const MusEPlugin::PluginScanInfoStruct& info = (*isl)->info();
    if(info._type != MusEPlugin::PluginTypeVST3 || !MusEGlobal::loadVST3)
      continue;

    const QString inf_cbname = PLUGIN_GET_QSTRING(info._completeBaseName);
    const QString inf_name   = PLUGIN_GET_QSTRING(info._name);
    const QString inf_label  = PLUGIN_GET_QSTRING(info._label);
    const QString inf_uri    = PLUGIN_GET_QSTRING(info._uri);
    const Plugin* plug_found = MusEGlobal::plugins.find(info._type, inf_cbname, inf_uri, inf_label);
    const Synth* synth_found = MusEGlobal::synthis.find(info._type, inf_cbname, inf_uri, inf_label);
    if(plug_found || synth_found)
      std::fprintf(stderr, "Ignoring VST3 %s name:%s uri:%s path:%s duplicate of path:%s\n",
                   plug_found ? "effect" : "synth",
                   inf_name.toLocal8Bit().constData(), inf_uri.toLocal8Bit().constData(),
                   PLUGIN_GET_QSTRING(info.filePath()).toLocal8Bit().constData(),
                   (plug_found ? plug_found->filePath() : synth_found->filePath()).toLocal8Bit().constData());

    const bool is_effect = info._class & MusEPlugin::PluginClassEffect;
    const bool is_synth  = info._class & MusEPlugin::PluginClassInstrument;
    const bool add_plug  = is_effect && info._inports > 0 && info._outports > 0 && !plug_found;
    // Like the other plugin types, an effect can also be used on a synth track.
    const bool add_synth = (is_synth || is_effect) && info._outports > 0 && !synth_found;

    if(MusEGlobal::debugMsg)
      PluginBase::dump(info, "initVST3: ");
    if(add_synth)
      MusEGlobal::synthis.push_back(new Vst3Synth(info));
    if(add_plug)
      MusEGlobal::plugins.push_back(new Vst3PluginWrapper(info));
  }
}

} // namespace MusECore

#endif // VST3_SUPPORT
