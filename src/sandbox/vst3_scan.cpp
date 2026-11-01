//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3_scan.cpp
//  Scanning of VST3 bundles, for the plugin scan sandbox program.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "vst3_scan.h"

#include <cstdio>
#include <memory>
#include <string>

#include "plugin_cache_reader.h"
#include "plugin_cache_writer.h"
#include "xml.h"

// The VST3 host code, shared with the stand-alone tests in vst3/.
#include "vst3/vst3host.h"

#include "pluginterfaces/gui/iplugview.h"

namespace MusEPlugin {

namespace {

const double SCAN_SAMPLE_RATE = 48000.0;
const int SCAN_BLOCK_SIZE = 512;

// Parses "1.2.3" or "1.2" into major and minor. Anything else leaves them alone.
void parseVersion(const std::string& v, int* major, int* minor)
{
  int a = 0, b = 0;
  const int n = std::sscanf(v.c_str(), "%d.%d", &a, &b);
  if(n >= 1)
  {
    *major = a;
    if(n >= 2)
      *minor = b;
  }
}

bool scanOne(const MusEVst3::PluginInfo& pi, bool doPorts, PluginScanInfoStruct* info)
{
  std::string err;
  std::unique_ptr<MusEVst3::Instance> inst = MusEVst3::Instance::create(pi, err);
  if(!inst)
  {
    std::fprintf(stderr, "muse_plugin_scan: VST3 %s: cannot create: %s\n", pi.name.c_str(), err.c_str());
    return false;
  }
  // Setting up proves that the plugin can really process. It also gives the real channel counts.
  if(!inst->setup(SCAN_SAMPLE_RATE, SCAN_BLOCK_SIZE, err))
  {
    std::fprintf(stderr, "muse_plugin_scan: VST3 %s: cannot set up: %s\n", pi.name.c_str(), err.c_str());
    return false;
  }

  setPluginScanFileInfo(QString::fromStdString(pi.path), info);

  info->_type = PluginTypeVST3;
  info->_uri = PLUGIN_SET_STDSTRING(pi.uid);
  info->_name = PLUGIN_SET_STDSTRING(pi.name);
  info->_label = info->_name;
  info->_maker = PLUGIN_SET_STDSTRING(pi.vendor);
  info->_version = PLUGIN_SET_STDSTRING(pi.version);
  info->_description = PLUGIN_SET_STDSTRING(pi.subCategories);
  info->_apiVersionMajor = 3;
  info->_apiVersionMinor = 0;
  parseVersion(pi.version, &info->_pluginVersionMajor, &info->_pluginVersionMinor);

  const int ins = inst->mainInputChannels();
  const int outs = inst->mainOutputChannels();

  info->_class = pi.isInstrument ? PluginClassInstrument : PluginClassEffect;
  // An instrument with audio inputs can also be used as an effect.
  if(pi.isInstrument && ins > 0)
    info->_class |= PluginClassEffect;

  // The state of a VST3 plugin is always a binary chunk, and it always gets the transport position.
  info->_pluginFlags |= PluginHasChunks | PluginSupportsTimePosition;

  // Does it have an editor? Creating the view without attaching it is cheap.
  {
    Steinberg::IPlugView* view = inst->createEditorView();
    if(view)
    {
      info->_pluginFlags |= PluginHasGui;
      view->release();
    }
  }

  // Control ports: the visible parameters that are not program changes. Read-only ones are outputs.
  // The plugin wrapper in MusE uses the same lists, which keeps the port numbers in step.
  const std::vector<MusEVst3::ParamInfo> ctlIn = inst->inputControls();
  const std::vector<MusEVst3::ParamInfo> ctlOut = inst->outputControls();
  const unsigned long cip = ctlIn.size(), cop = ctlOut.size();
  PluginPortList controlPorts;
  unsigned long portIdx = unsigned(ins) + unsigned(outs);
  for(int pass = 0; pass < 2; ++pass)
  {
    const std::vector<MusEVst3::ParamInfo>& list = pass == 0 ? ctlIn : ctlOut;
    for(size_t n = 0; n < list.size(); ++n)
    {
      const MusEVst3::ParamInfo& p = list[n];
      PluginPortInfo port;
      port._index = portIdx++;
      port._name = PLUGIN_SET_STDSTRING(p.title);
      port._type = PluginPortInfo::ControlPort | (pass == 0 ? PluginPortInfo::InputPort : PluginPortInfo::OutputPort);
      // The host side sees normalized values.
      port._min = 0.0f;
      port._max = 1.0f;
      port._defaultVal = float(p.defaultNormalized);
      port._valueFlags |= PluginPortInfo::HasMin | PluginPortInfo::HasMax | PluginPortInfo::HasDefault;
      if(p.stepCount == 1)
        port._valueFlags |= PluginPortInfo::ToggledVal;
      else if(p.stepCount > 1)
      {
        port._valueFlags |= PluginPortInfo::HasStep;
        port._step = 1.0f / float(p.stepCount);
      }
      if(pass == 0 && p.isBypass)
      {
        info->_pluginBypassType = PluginBypassTypeBypassPort;
        info->_enableOrBypassPortIdx = n;
        port._flags |= PluginPortInfo::IsBypass;
      }
      controlPorts.push_back(port);
    }
  }

  info->_inports = ins;
  info->_outports = outs;
  info->_controlInPorts = cip;
  info->_controlOutPorts = cop;
  info->_eventInPorts = inst->acceptsNoteEvents() ? 1 : 0;
  info->_eventOutPorts = 0;
  info->_portCount = unsigned(ins) + unsigned(outs) + cip + cop;
  info->_pluginLatencyReportingType = PluginLatencyTypeFunction;
  // The processing mode (real time or offline) is told to the plugin when it is set up.
  info->_pluginFreewheelType = PluginFreewheelTypeFunction;
  if(info->_pluginBypassType != PluginBypassTypeBypassPort)
    info->_pluginBypassType = PluginBypassTypeEmulatedEnableFunction;

  // Processing in place is not something we rely on with VST3 buffers.
  info->_requiredFeatures |= PluginNoInPlaceProcessing;

  if(doPorts)
  {
    unsigned long k = 0;
    for(int i = 0; i < ins; ++i)
    {
      PluginPortInfo port;
      port._index = k++;
      port._type = PluginPortInfo::AudioPort | PluginPortInfo::InputPort;
      info->_portList.push_back(port);
    }
    for(int i = 0; i < outs; ++i)
    {
      PluginPortInfo port;
      port._index = k++;
      port._type = PluginPortInfo::AudioPort | PluginPortInfo::OutputPort;
      info->_portList.push_back(port);
    }
    for(const PluginPortInfo& port : controlPorts)
      info->_portList.push_back(port);
  }

  // The instance, and with it the plugin's editor controller, closes here.
  return true;
}

} // anonymous namespace

bool writeVst3Info(const char* filename, bool doPorts, int level, MusECore::Xml& xml)
{
  std::vector<std::string> errors;
  const std::vector<MusEVst3::PluginInfo> classes = MusEVst3::scan({std::string(filename)}, &errors);
  for(const std::string& e : errors)
    std::fprintf(stderr, "muse_plugin_scan: VST3: %s\n", e.c_str());

  bool any = false;
  for(const MusEVst3::PluginInfo& pi : classes)
  {
    PluginScanInfoStruct info;
    if(scanOne(pi, doPorts, &info))
    {
      writePluginScanInfo(level, xml, info, doPorts);
      any = true;
    }
  }
  return any;
}

} // namespace MusEPlugin
