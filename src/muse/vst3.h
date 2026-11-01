//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3.h
//  VST3 effect plugins. See ../README.vst3 for how it fits together.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __VST3_H__
#define __VST3_H__

#include "config.h"

#ifdef VST3_SUPPORT

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <unordered_map>
#include <vector>

#include "plugin.h"
#include "plugin_scan.h"
#include "synth.h"

// The VST3 host code. Only forward declares the Steinberg types, and the editor is only a pointer here.
#include "vst3/vst3host.h"

namespace MusEVst3 {
class Editor;
}

namespace MusECore {

class Vst3PluginWrapper;

//---------------------------------------------------------
//   Vst3PluginState
//    One instance of a plugin, the 'handle' of a PluginI.
//    Lives in the main thread, apart from apply() which runs
//    in the audio thread and only uses the members marked RT.
//---------------------------------------------------------

class Vst3PluginState : public QObject
{
  Q_OBJECT

  public:
    Vst3PluginState(Vst3PluginWrapper* wrapper, PluginI* pluginI);
    ~Vst3PluginState() override;

    std::unique_ptr<MusEVst3::Instance> inst;
    Vst3PluginWrapper* wrapper;
    PluginI* pluginI;

    // Port pointers set by connectPort(), RT.
    std::vector<float*> inPorts, outPorts, inControls, outControls;
    // The value last sent to the plugin for each control input, RT.
    std::vector<float> lastIn;
    // Scratch for processing in pieces if a call is longer than the set up block size, RT.
    std::vector<const float*> chunkIn;
    std::vector<float*> chunkOut;

    // The block size the plugin was set up for.
    int maxBlock;

    // Only for diagnostics, counted in the audio thread and looked at in the main thread (-D).
    std::atomic<unsigned> blocksProcessed;
    std::atomic<float> peakOut;
    bool reportedFirstBlock;
    bool reportedSound;
    // Diagnostics: feed a test tone instead of the input, set by MUSE_VST3_TEST_TONE together with -D.
    bool testTone;
    // Diagnostics: pretend that MusE is in freewheel mode (MUSE_VST3_TEST_FREEWHEEL together with -D).
    bool testOffline;
    int offlineBeats;
    unsigned long tonePos;

    MusEVst3::Editor* editor;
    bool guiVisible;

    // Called when the plugin reports an edit, usually from its editor, in the thread of the plugin.
    void pluginEdit(const MusEVst3::EditEvent& ev);
    // Makes the MusE control values follow the plugin's current parameter values.
    void syncControlsFromPlugin();
    // Called with the state of the song after its restore, and after changes the plugin asks for.
    void updateWindowTitle();

  protected slots:
    void heartBeat();
};

//---------------------------------------------------------
//   Vst3PluginWrapper
//    The plugin as MusE sees it, in the style of the other
//    plugin types: a fake set of ladspa style ports.
//---------------------------------------------------------

class Vst3PluginWrapper : public Plugin
{
  friend class Vst3PluginState;

  private:
    bool _hasGui;
    // Read in when the first instance is made. Port numbers of controls are positions in these lists.
    bool _infoLoaded;
    std::vector<MusEVst3::ParamInfo> _inCtl;
    std::vector<MusEVst3::ParamInfo> _outCtl;
    std::unordered_map<unsigned, int> _inIdToIdx;
    std::unordered_map<unsigned, int> _outIdToIdx;
    std::vector<std::string> _portNames;

  public:
    Vst3PluginWrapper(const MusEPlugin::PluginScanInfoStruct& info);
    ~Vst3PluginWrapper() override;

    bool reference() override;
    int release() override;

    LADSPA_Handle instantiate(PluginI*) override;
    void activate(LADSPA_Handle handle) override;
    void deactivate(LADSPA_Handle handle) override;
    void cleanup(LADSPA_Handle handle) override;
    void connectPort(LADSPA_Handle handle, unsigned long port, float* value) override;
    void apply(LADSPA_Handle handle, unsigned long n, float latency_corr = 0.0f) override;
    float getPluginLatency(void* handle) override;

    LADSPA_PortDescriptor portd(unsigned long k) const override;
    LADSPA_PortRangeHint range(unsigned long i) const override;
    void range(unsigned long i, float* min, float* max) const override;
    double defaultValue(unsigned long port) const override;
    const char* portName(unsigned long i) const override;
    CtrlValueType ctrlValueType(unsigned long i) const override;
    CtrlList::Mode ctrlMode(unsigned long i) const override;

    bool hasNativeGui() const;
    void showNativeGui(PluginI* p, bool show);
    bool nativeGuiVisible(const PluginI* p) const;
    void updateNativeGuiWindowTitle(const PluginI* p) const;

    // The state of the plugin as one string, and back. Returns true if the state was set.
    QString getCustomConfiguration(LADSPA_Handle handle);
    bool setCustomData(LADSPA_Handle handle, const std::vector<QString>& customParams);

  private:
    // Index into the control lists for a port number, or -1.
    int inControlIndex(unsigned long port) const;
    int outControlIndex(unsigned long port) const;
};


class Vst3SynthIF;

//---------------------------------------------------------
//   Vst3Synth
//    A VST3 instrument as MusE sees it.
//---------------------------------------------------------

class Vst3Synth : public Synth
{
  friend class Vst3SynthIF;

  private:
    bool _hasGui;
    // What the plugin scan says, for the main buses.
    unsigned long _scanInports, _scanOutports;
    // The channels of all the buses that are used. The same as the scan if the plugin has only the main
    //  buses. Set when the first instance is made.
    unsigned long _inports, _outports, _mainOutports, _controlInPorts;
    // Read in when the first instance is made. Parameter numbers are positions in the list.
    bool _infoLoaded;
    std::vector<MusEVst3::ParamInfo> _inCtl;
    std::unordered_map<unsigned, int> _inIdToIdx;

  public:
    Vst3Synth(const MusEPlugin::PluginScanInfoStruct& info);
    ~Vst3Synth() override {}

    bool reference() override;
    int release() override;
    SynthIF* createSIF(SynthI*) override;

    unsigned long inPorts() const { return _inports; }
    unsigned long outPorts() const { return _outports; }
    unsigned long inControls() const { return _controlInPorts; }
    bool hasGui() const { return _hasGui; }
};

//---------------------------------------------------------
//   Vst3SynthIF
//    One instance of a VST3 instrument, on a synth track.
//---------------------------------------------------------

class Vst3SynthIF : public SynthIF
{
  friend class Vst3Synth;

  private:
    Vst3Synth* _synth;
    std::unique_ptr<MusEVst3::Instance> _inst;
    MusEVst3::Editor* _editor;
    bool _guiVisible;

    Port* _controls;                 // The values of the control inputs, and the automation state.
    float* _last;                    // What the plugin was last told for each control, audio thread.
    float** _audioOutBuffers;
    float** _audioInBuffers;
    float* _audioInSilenceBuf;       // All zeros all the time.
    int _maxBlock;                   // What the plugin was set up for.

    float _transportLatencyCorr;
    // Which buses the plugin is set up with. All the outputs and the sidechain inputs.
    MusEVst3::SetupOptions _setupOptions;
    // Diagnostics: pretend freewheel mode (MUSE_VST3_TEST_FREEWHEEL with -D).
    bool _testOffline;

    // Diagnostics (-D): what the audio thread produced, looked at in the main thread. And the test
    //  hooks MUSE_VST3_TEST_NOTE (a note, then a pitch bend, through MusE's own midi event path).
    std::atomic<int> _dbgPorts;      // The number of output buffers MusE asked for.
    std::atomic<float> _dbgPeak;
    std::atomic<unsigned> _dbgCrossings, _dbgSamples;
    float _dbgLastSample;            // Audio thread.
    int _testBeats;

    bool init(Vst3Synth* s);
    // Turns a MusE midi event into calls to the plugin. 'ft' is the frame inside the block.
    void processEvent(const MidiPlayEvent& e, unsigned ft);
    void doSelectProgram(int bankH, int bankL, int prog, unsigned ft);
    // The plugin reports an edit, usually from its editor.
    void pluginEdit(const MusEVst3::EditEvent& ev);
    void closeEditor();

  protected:
    void activate() override;
    void deactivate() override;

  public:
    Vst3SynthIF(SynthI* s);
    ~Vst3SynthIF() override;

    void guiHeartBeat() override;
    bool hasGui() const override { return true; }
    bool nativeGuiVisible() const override { return _guiVisible; }
    void showNativeGui(bool v) override;
    bool hasNativeGui() const override;
    void getNativeGeometry(int* x, int* y, int* w, int* h) const override;
    void setNativeGeometry(int x, int y, int w, int h) override;
    void updateNativeGuiWindowTitle() override;
    bool getData(MidiPort*, unsigned pos, int ports, unsigned nframes, float** buffer) override;
    MidiPlayEvent receiveEvent() override;
    int eventsPending() const override { return 0; }
    int channels() const override;
    int totalOutChannels() const override;
    int totalInChannels() const override;
    void deactivate3() override;
    QString getPatchName(int chan, int prog, bool drum) const override;
    void populatePatchPopup(MusEGui::PopupMenu* menu, int chan, bool drum) override;
    double getParameter(unsigned long idx) const override;
    void setParameter(unsigned long idx, double value) override;
    int getControllerInfo(int, QString*, int*, int*, int*, int*) override { return 0; }

    // Methods for PluginIBase:
    unsigned long pluginID() const override;
    void enableController(unsigned long i, bool v = true) override;
    bool controllerEnabled(unsigned long i) const override;
    void enableAllControllers(bool v = true) override;
    void updateController(unsigned long i) override;
    void updateControllers() override;
    unsigned long parameters() const override;
    unsigned long parametersOut() const override;
    void setParam(unsigned long i, double val) override;
    double param(unsigned long i) const override;
    double paramOut(unsigned long) const override;
    const char* paramName(unsigned long i) const override;
    const char* paramOutName(unsigned long) const override;
    LADSPA_PortRangeHint range(unsigned long i) const override;
    LADSPA_PortRangeHint rangeOut(unsigned long) const override;
    void range(unsigned long i, float*, float*) const override;
    void rangeOut(unsigned long, float*, float*) const override;
    CtrlValueType ctrlValueType(unsigned long i) const override;
    CtrlList::Mode ctrlMode(unsigned long i) const override;
    CtrlValueType ctrlOutValueType(unsigned long) const override;
    CtrlList::Mode ctrlOutMode(unsigned long) const override;
    std::vector<QString> getCustomData() const override;
    bool setCustomData(const std::vector<QString>&) override;
    bool usesTransportSource() const override;
};

// Makes plugin objects of the VST3 plugins in the plugin cache.
void initVST3();

} // namespace MusECore

#else

namespace MusECore {
inline void initVST3() {}
}

#endif // VST3_SUPPORT

#endif
