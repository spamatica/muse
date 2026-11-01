//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3host.h
//  Step 1 proof of concept: headless VST3 hosting built on
//  the Steinberg VST3 SDK (MIT licensed) hosting classes.
//  Nothing here depends on MusE or Qt so it can be tested
//  stand-alone. See ../README.vst3 for the plan.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __MUSE_VST3HOST_H__
#define __MUSE_VST3HOST_H__

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace Steinberg { class IPlugView; }

namespace MusEVst3 {

struct PluginInfo
{
  std::string path;           // Path of the .vst3 bundle.
  std::string name;
  std::string vendor;
  std::string version;
  std::string subCategories;  // For example "Instrument|Synth".
  std::string uid;            // 32 hex characters.
  bool isInstrument = false;
};

struct ParamInfo
{
  uint32_t id = 0;
  std::string title;
  std::string units;
  double defaultNormalized = 0.0;
  int stepCount = 0;          // 0 = continuous, 1 = toggle, n = n+1 discrete values.
  bool readOnly = false;
  bool hidden = false;
  bool isProgramChange = false;
  bool isBypass = false;
};

// Scans the given directories (recursively) for .vst3 bundles.
// If 'dirs' is empty the standard locations are used (~/.vst3, /usr/lib/vst3, /usr/local/lib/vst3).
// Only audio effect classes (instruments included) are reported.
std::vector<PluginInfo> scan(const std::vector<std::string>& dirs, std::vector<std::string>* errors = nullptr);

// Which audio buses are used. The main buses (index 0) are always active, and all event buses.
struct SetupOptions
{
  // Also activate the auxiliary output buses, for instruments with more than one output.
  bool allOutputBuses = false;
  // Also activate the auxiliary input buses (sidechain inputs).
  bool sidechainInputs = false;
  // Tell the plugin that it is processing offline (freewheeling, exporting) instead of in real time.
  bool offline = false;
};

// An edit the plugin reports through IComponentHandler, usually from its editor.
struct EditEvent
{
  enum Type { Begin, Perform, End };
  Type type;
  uint32_t id;
  double value;     // Normalized. For Begin and End the current value.
};

// One loaded plugin: module, component, controller and processor.
//
// Threading, following the VST3 model:
//  * process(), queueParamRT() and controllerUpdateRT() belong to the realtime (audio) thread.
//    They do not lock, allocate or call into the edit controller. Parameter and bus lists are
//    prepared by setup(), so queueParamRT() is only valid after a successful setup().
//  * Everything else, including setup(), teardown(), state, the editor and applyControllerUpdates(),
//    belongs to the main (GUI) thread. A processor must not be set up or torn down while process()
//    runs on another thread.
class Instance
{
  public:
    struct Impl;

    static std::unique_ptr<Instance> create(const PluginInfo& info, std::string& error);
    ~Instance();

    using SetupOptions = ::MusEVst3::SetupOptions;

    // Prepares for processing.
    // With only the main buses, the channel arrays of process() have mainInputChannels() and
    //  mainOutputChannels() entries. With more buses they have inputChannels() and outputChannels(),
    //  the channels of the active buses one after the other in the order of the buses.
    bool setup(double sampleRate, int maxBlockSize, std::string& error, const SetupOptions& options = SetupOptions());
    // Stops processing and deactivates. Called by the destructor.
    void teardown();
    bool isActive() const;

    int mainInputChannels() const;
    int mainOutputChannels() const;
    // Channels of all the active audio buses together, valid after setup().
    int inputChannels() const;
    int outputChannels() const;

    struct BusDetail
    {
      std::string name;
      int channels = 0;
      bool aux = false;            // Not the main bus (a sidechain input, an extra output).
      bool defaultActive = false;  // What the plugin suggests.
      bool active = false;         // Whether setup() activated it.
    };
    std::vector<BusDetail> audioBuses(bool input) const;

    // Tells the plugin that the processing is offline (as in freewheel mode), or real time again.
    // If the plugin is set up this sets it up again, because VST3 only allows changing the process mode
    //  that way. That is not free, so it is for transitions and not for every block. It may be called
    //  from the audio thread, as setup() and teardown() are by MusE. Returns false if setting up failed.
    bool setOffline(bool offline);
    bool offline() const;
    int audioInputBusCount() const;
    int audioOutputBusCount() const;
    bool acceptsNoteEvents() const;
    // Processing latency in samples, as reported by the plugin.
    uint32_t latencySamples() const;
    // Number of samples the plugin keeps producing after its input stopped (reverb tails).
    uint32_t tailSamples() const;

    // Parameters.
    int paramCount() const;
    ParamInfo paramInfo(int index) const;
    // The parameters a host shows as control ports: visible ones that are not program changes.
    // Inputs are the writable ones, outputs the read-only ones. The order is the order of the plugin.
    // The MusE plugin scan and the MusE plugin wrapper both use these, so that port numbers agree.
    std::vector<ParamInfo> inputControls() const;
    std::vector<ParamInfo> outputControls() const;
    double getParam(uint32_t id) const;            // Normalized 0..1.
    // Main thread, not while another thread processes: tells the edit controller and queues the
    //  value for the next process() call.
    void setParam(uint32_t id, double normalized);
    // Audio thread: queues the value for the next process() call. Does not touch the edit controller.
    // Returns false if the id is unknown.
    bool queueParamRT(uint32_t id, double normalized);
    // Audio thread: asks the main thread to tell the edit controller about a new value, for example
    //  after automation moved a parameter, so that the plugin's editor follows. Returns false if full.
    bool controllerUpdateRT(uint32_t id, double normalized);
    // Main thread: gives the edit controller the values sent by controllerUpdateRT().
    // Returns the number of values applied.
    int applyControllerUpdates();
    // Main thread: the IComponentHandler::restartComponent flags the plugin has asked for since the
    //  last call (Steinberg::Vst::RestartFlags). Clears them.
    int takeRestartFlags();
    // After process(): changes the plugin made to its output parameters (read-only ones, like meters).
    // The arrays belong to the instance and are valid until the next process() call.
    int outputParamChanges(const uint32_t** ids, const double** values) const;
    std::string paramToString(uint32_t id, double normalized) const;

    // Edits made by the plugin itself (for example from its editor), as the plugin reports them
    //  through IComponentHandler. The callback runs on the thread of the plugin, usually the main
    //  thread, and must not block. It stays valid until it is replaced or the instance dies.
    void setEditCallback(std::function<void(const EditEvent&)> callback);
    // By default the edits are also sent to the processor on the next process() call, as the VST3
    //  specification wants from a host. A host that handles them itself, for example by way of its own
    //  controller values, turns that off. Then process() never takes the lock for the edits.
    void setForwardEditsToProcessor(bool forward);
    // The edits seen so far, newest last (at most 4096 are kept). Drains the list when 'drain' is true.
    std::vector<std::pair<uint32_t, double>> pluginEdits(bool drain);

    // Sample accurate parameter change for the next process() call, audio thread. There can be
    //  several for one parameter, as long as their sample offsets differ. Returns false if the id
    //  is unknown or too many values are waiting.
    bool queueParamAtRT(uint32_t id, double normalized, int sampleOffset);

    // MIDI controllers are parameters in VST3. The plugin says which ones it wants through IMidiMapping.
    // Controller numbers are the MIDI ones, 0..127, plus 128 for channel pressure and 129 for pitch bend.
    bool hasMidiMapping(int channel, int controller) const;
    // Audio thread: sets the mapped parameter for the next process() call. 'normalized' is 0..1 (for
    //  pitch bend 0.5 is the middle). Returns false if the plugin does not map the controller.
    bool midiController(int channel, int controller, double normalized, int sampleOffset = 0);

    // Programs, from the plugin's program list. The program change parameter is how one is selected.
    int programCount() const;
    std::string programName(int index) const;
    // Audio thread: selects a program for the next process() call. False if the plugin has no programs.
    bool selectProgramRT(int index, int sampleOffset = 0);

    // Note input for the next process() call. sampleOffset is relative to the start of that block.
    // Every note gets a note id, which later events of the note (note off, pressure, note expression) use.
    // A note that is already on, on the same channel and pitch, is ended first. Returns the note id.
    int noteOn(int channel, int pitch, float velocity, int sampleOffset = 0);
    void noteOff(int channel, int pitch, float velocity = 0.0f, int sampleOffset = 0);
    void polyPressure(int channel, int pitch, float pressure, int sampleOffset = 0);

    // Note expression: values that belong to one sounding note, like a different tuning, volume or
    //  brightness for every note. The plugin says what it understands. The type ids are the ones
    //  of the VST3 specification (Steinberg::Vst::NoteExpressionTypeIDs): 0 volume, 1 pan, 2 tuning,
    //  3 vibrato, 4 expression, 5 brightness, and plugin specific ones from 100000 on.
    // Values are normalized 0..1. The tuning type is in semitones, with the plugin's own range
    //  around 0.5 for 'no change'.
    struct NoteExpressionType
    {
      uint32_t typeId = 0;
      std::string title;
      std::string units;
      double defaultValue = 0.0;   // Normalized.
      double minPlain = 0.0, maxPlain = 1.0;
      bool bipolar = false;
    };
    std::vector<NoteExpressionType> noteExpressionTypes() const;
    bool supportsNoteExpression() const;
    // Audio thread. Applies to the note that is on for the channel and pitch. Returns false if there is none.
    bool noteExpression(int channel, int pitch, uint32_t typeId, double normalized, int sampleOffset = 0);

    // Processes one block. 'in' and 'out' are the main audio buses, with the plugin's
    //  mainInputChannels() / mainOutputChannels() arrays of nframes samples. 'in' may be null.
    // Auxiliary buses are inactive. nframes must not exceed the maxBlockSize given to setup().
    // Optional transport information for the plugin, valid for this block only. Without it the
    //  plugin is told that the transport plays at 120 bpm from sample 0.
    struct Transport
    {
      bool playing = true;
      double tempo = 120.0;
      int timeSigNumerator = 4;
      int timeSigDenominator = 4;
      int64_t sample = 0;       // Project time in samples.
      double quarters = 0.0;    // Project time in quarter notes.
    };
    void setTransport(const Transport& t);
    bool process(const float* const* in, float* const* out, int nframes);

    // Creates the plugin's editor view, or returns null if it has none. The caller owns the
    //  returned reference (release() it or wrap it in Steinberg::owned()). The view must be
    //  removed and released before this Instance is destroyed. Main thread only.
    Steinberg::IPlugView* createEditorView();

    // Complete state (component + controller) in an opaque blob.
    bool saveState(std::vector<uint8_t>& blob) const;
    bool loadState(const std::vector<uint8_t>& blob);

  private:
    Instance();
    std::unique_ptr<Impl> _impl;
};

} // namespace MusEVst3

#endif
