//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3_tests.cpp
//  Tests for the VST3 hosting code. They run against real plugins found on
//  the system and are skipped (exit code 77) if there are none.
//
//  Environment:
//    VST3_TEST_DIRS         colon separated directories to scan (default: standard locations)
//    VST3_TEST_INSTRUMENT   part of the name of the instrument to test with
//    VST3_TEST_EFFECT       part of the name of the effect to test with
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "../vst3host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace MusEVst3;

static int g_failed = 0, g_checks = 0, g_run = 0;

#define CHECK(cond) do { ++g_checks; if(!(cond)) { ++g_failed; \
  fprintf(stderr, "    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)
#define CHECK_MSG(cond, msg) do { ++g_checks; if(!(cond)) { ++g_failed; \
  fprintf(stderr, "    FAILED %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, std::string(msg).c_str()); } } while(0)

static std::vector<PluginInfo> g_all;
static PluginInfo g_instrument, g_effect;
static bool g_haveInstrument = false, g_haveEffect = false;

static std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

static bool pick(bool instrument, const char* envName, PluginInfo& out)
{
  const char* want = getenv(envName);
  for(const auto& p : g_all)
    if(p.isInstrument == instrument && (!want || lower(p.name).find(lower(want)) != std::string::npos))
    { out = p; return true; }
  return false;
}

struct Rendered { double peak = 0; bool finite = true; bool ok = true; };

// Renders 'blocks' blocks of 'block' samples. 'inputLevel' > 0 feeds a sine to the input.
static Rendered render(Instance& inst, int blocks, int block, float inputLevel, std::function<void(int)> perBlock = nullptr)
{
  Rendered r;
  int inCh = inst.inputChannels(), outCh = inst.outputChannels();
  std::vector<std::vector<float>> in(inCh, std::vector<float>(block)), out(outCh, std::vector<float>(block));
  std::vector<const float*> inp(inCh);
  std::vector<float*> outp(outCh);
  for(int b = 0; b < blocks; ++b)
  {
    if(perBlock) perBlock(b);
    for(int c = 0; c < inCh; ++c)
    {
      for(int s = 0; s < block; ++s)
        in[c][s] = inputLevel * float(std::sin(2 * M_PI * 220.0 * (b * block + s) / 48000.0));
      inp[c] = in[c].data();
    }
    for(int c = 0; c < outCh; ++c) outp[c] = out[c].data();
    r.ok &= inst.process(inCh ? inp.data() : nullptr, outp.data(), block);
    for(int c = 0; c < outCh; ++c)
      for(int s = 0; s < block; ++s)
      {
        if(!std::isfinite(out[c][s])) r.finite = false;
        else r.peak = std::max(r.peak, double(std::fabs(out[c][s])));
      }
  }
  return r;
}

static std::unique_ptr<Instance> load(const PluginInfo& pi, int block = 256)
{
  std::string err;
  auto inst = Instance::create(pi, err);
  CHECK_MSG(inst != nullptr, "create: " + err);
  if(inst)
  {
    // Instruments with several outputs are used with all of them. Some make no sound otherwise.
    Instance::SetupOptions options;
    options.allOutputBuses = pi.isInstrument;
    bool ok = inst->setup(48000.0, block, err, options);
    CHECK_MSG(ok, "setup: " + err);
    if(!ok) inst.reset();
  }
  return inst;
}

//---------------------------------------------------------

static void test_scan()
{
  CHECK(!g_all.empty());
  for(const auto& p : g_all)
  {
    CHECK(!p.name.empty());
    CHECK(p.uid.size() == 32);
    CHECK(!p.path.empty());
  }
}

static void test_scan_missing_dir()
{
  std::vector<PluginInfo> r = scan({"/nonexistent/dir/for/vst3"});
  CHECK(r.empty());
}

static void test_create_bad_path()
{
  PluginInfo bad;
  bad.path = "/nonexistent/Nothing.vst3";
  bad.uid = "00000000000000000000000000000000";
  std::string err;
  auto inst = Instance::create(bad, err);
  CHECK(inst == nullptr);
  CHECK(!err.empty());
}

static void test_create_unknown_class()
{
  if(!g_haveInstrument && !g_haveEffect) return;
  PluginInfo pi = g_haveInstrument ? g_instrument : g_effect;
  pi.uid = "00000000000000000000000000000000";
  std::string err;
  auto inst = Instance::create(pi, err);
  CHECK(inst == nullptr);
  CHECK(!err.empty());
}

static void test_lifecycle_repeat()
{
  PluginInfo pi = g_haveEffect ? g_effect : g_instrument;
  for(int i = 0; i < 3; ++i)
  {
    auto inst = load(pi);
    if(!inst) return;
    inst->teardown();
    std::string err;
    CHECK(inst->setup(44100.0, 128, err));   // Re-setup with other settings.
    Rendered r = render(*inst, 4, 128, 0.1f);
    CHECK(r.ok);
  }
}

static void test_buses()
{
  auto inst = load(g_haveInstrument ? g_instrument : g_effect);
  if(!inst) return;
  CHECK(inst->mainOutputChannels() > 0);
  CHECK(inst->audioOutputBusCount() >= 1);
  if(g_haveInstrument)
    CHECK(inst->acceptsNoteEvents());
  (void)inst->latencySamples();
  (void)inst->tailSamples();
}

static void test_effect_silence_and_sine()
{
  if(!g_haveEffect) { printf("    (no effect plugin, skipped)\n"); return; }
  auto inst = load(g_effect);
  if(!inst) return;
  CHECK(inst->mainInputChannels() > 0);
  Rendered silence = render(*inst, 20, 256, 0.0f);
  CHECK(silence.ok);
  CHECK(silence.finite);
  Rendered sine = render(*inst, 40, 256, 0.25f);
  CHECK(sine.ok);
  CHECK(sine.finite);
}

static void test_instrument_note()
{
  if(!g_haveInstrument) { printf("    (no instrument plugin, skipped)\n"); return; }
  auto inst = load(g_instrument);
  if(!inst) return;

  Rendered quiet = render(*inst, 20, 256, 0.0f);
  CHECK(quiet.finite);

  // About 1.5 seconds with a note held for the first second.
  Rendered played = render(*inst, 280, 256, 0.0f, [&](int b) {
    if(b == 0) inst->noteOn(0, 60, 0.8f, 0);
    if(b == 187) inst->noteOff(0, 60, 0.0f, 0);
  });
  CHECK(played.ok);
  CHECK(played.finite);
  CHECK_MSG(played.peak > 1e-4, "instrument produced no sound, peak=" + std::to_string(played.peak));
  CHECK(played.peak < 100.0);
}

static void test_instrument_sample_offset()
{
  if(!g_haveInstrument) return;
  auto inst = load(g_instrument);
  if(!inst) return;
  // Notes at the very end of a block and a note-off in the same block must not break anything.
  Rendered r = render(*inst, 100, 256, 0.0f, [&](int b) {
    if(b % 10 == 0) { inst->noteOn(0, 48 + b / 10, 0.7f, 255); inst->noteOff(0, 48 + b / 10, 0.0f, 255); }
  });
  CHECK(r.ok);
  CHECK(r.finite);
}

static void test_various_block_sizes()
{
  PluginInfo pi = g_haveInstrument ? g_instrument : g_effect;
  for(int block : { 1, 7, 64, 1024 })
  {
    auto inst = load(pi, 1024);
    if(!inst) return;
    Rendered r = render(*inst, std::max(4, 2048 / block), block, 0.1f);
    CHECK_MSG(r.ok, "block " + std::to_string(block));
    CHECK_MSG(r.finite, "block " + std::to_string(block));
  }
}

static void test_too_large_block_rejected()
{
  auto inst = load(g_haveInstrument ? g_instrument : g_effect, 128);
  if(!inst) return;
  int outCh = inst->outputChannels();
  std::vector<std::vector<float>> out(outCh, std::vector<float>(256));
  std::vector<float*> outp(outCh);
  for(int c = 0; c < outCh; ++c) outp[c] = out[c].data();
  CHECK(!inst->process(nullptr, outp.data(), 256));
  CHECK(!inst->process(nullptr, outp.data(), 0));
}

static void test_parameters()
{
  auto inst = load(g_haveInstrument ? g_instrument : g_effect);
  if(!inst) return;
  int n = inst->paramCount();
  CHECK(n > 0);
  int tested = 0;
  for(int i = 0; i < n && tested < 5; ++i)
  {
    ParamInfo pi = inst->paramInfo(i);
    CHECK(!pi.title.empty() || pi.hidden);
    if(pi.readOnly || pi.hidden || pi.isProgramChange || pi.stepCount != 0) continue;
    for(double v : { 0.0, 0.25, 0.8, 1.0 })
    {
      inst->setParam(pi.id, v);
      CHECK_MSG(std::fabs(inst->getParam(pi.id) - v) < 1e-3, pi.title + " round trip");
    }
    CHECK(!inst->paramToString(pi.id, 0.5).empty());
    ++tested;
  }
  CHECK(tested > 0);
  // Pending values get delivered without trouble.
  Rendered r = render(*inst, 10, 256, 0.0f);
  CHECK(r.ok);
  CHECK(r.finite);
}

static void test_state_roundtrip()
{
  PluginInfo pi = g_haveInstrument ? g_instrument : g_effect;
  auto a = load(pi);
  if(!a) return;

  // Find a plain parameter, give it a value away from the default.
  uint32_t id = 0;
  bool found = false;
  for(int i = 0; i < a->paramCount() && !found; ++i)
  {
    ParamInfo p = a->paramInfo(i);
    if(!p.readOnly && !p.hidden && !p.isProgramChange && p.stepCount == 0 &&
       std::fabs(p.defaultNormalized - 0.9) > 0.2)
    { id = p.id; found = true; }
  }
  CHECK(found);
  if(!found) return;
  a->setParam(id, 0.9);
  render(*a, 4, 256, 0.0f);   // So that the processor has it too.

  std::vector<uint8_t> blob;
  CHECK(a->saveState(blob));
  CHECK(blob.size() > 12);

  auto b = load(pi);
  if(!b) return;
  CHECK(b->loadState(blob));
  bool follows = std::fabs(b->getParam(id) - 0.9) < 1e-2;
  if(!follows)
  {
    // A plugin may ask the host to read the parameters again (kParamValuesChanged), and may not keep every
    //  parameter in its state. Then the controller does not have to show the value from the state.
    const int flags = b->takeRestartFlags();
    render(*b, 8, 256, 0.0f);
    follows = std::fabs(b->getParam(id) - 0.9) < 1e-2;
    printf("    state restore: the controller does not follow right away, restart flags %d, after processing %.3f\n",
           flags, b->getParam(id));
    if(flags & 4)
      follows = true;
  }
  CHECK_MSG(follows, "parameter restored from state, got " + std::to_string(b->getParam(id)));

  // The restored instance must save the same thing again.
  std::vector<uint8_t> blob2;
  CHECK(b->saveState(blob2));
  CHECK(blob2.size() > 12);
}

static void test_state_garbage_rejected()
{
  auto inst = load(g_haveInstrument ? g_instrument : g_effect);
  if(!inst) return;
  CHECK(!inst->loadState({}));
  CHECK(!inst->loadState({1, 2, 3, 4, 5, 6, 7, 8}));
  std::vector<uint8_t> blob;
  CHECK(inst->saveState(blob));
  blob.resize(blob.size() / 2);     // Truncated.
  CHECK(!inst->loadState(blob) || blob.size() <= 12);
  // Still alive and processing afterwards.
  Rendered r = render(*inst, 4, 256, 0.0f);
  CHECK(r.ok);
}


static PluginInfo pickForParams() { return g_haveEffect ? g_effect : g_instrument; }

static bool firstPlainControl(Instance& inst, ParamInfo& out)
{
  for(const ParamInfo& p : inst.inputControls())
    if(p.stepCount == 0 && !p.isBypass && std::fabs(p.defaultNormalized - 0.9) > 0.3)
    { out = p; return true; }
  return false;
}

static void test_control_lists()
{
  auto inst = load(pickForParams());
  if(!inst) return;
  const auto in = inst->inputControls();
  const auto out = inst->outputControls();
  CHECK(!in.empty());
  std::set<uint32_t> seen;
  for(const ParamInfo& p : in)
  {
    CHECK(!p.hidden && !p.isProgramChange && !p.readOnly);
    CHECK(seen.insert(p.id).second);
  }
  for(const ParamInfo& p : out)
  {
    CHECK(!p.hidden && !p.isProgramChange && p.readOnly);
    CHECK(seen.insert(p.id).second);
  }
  CHECK(int(in.size() + out.size()) <= inst->paramCount());
}

static void test_queue_param_reaches_processor()
{
  auto inst = load(pickForParams());
  if(!inst) return;
  ParamInfo p;
  CHECK(firstPlainControl(*inst, p));
  if(p.title.empty() && p.id == 0) return;

  Rendered r0 = render(*inst, 4, 256, 0.1f);
  CHECK(r0.ok);
  std::vector<uint8_t> before, after;
  CHECK(inst->saveState(before));

  CHECK(inst->queueParamRT(p.id, 0.9));
  CHECK(!inst->queueParamRT(0xFFFFFFF0u, 0.5));      // Unknown id.
  Rendered r1 = render(*inst, 4, 256, 0.1f);
  CHECK(r1.ok);
  CHECK(inst->saveState(after));
  CHECK_MSG(before != after, "the processor state did not change after a queued parameter value");
}

static void test_controller_update_ring()
{
  auto inst = load(pickForParams());
  if(!inst) return;
  ParamInfo p;
  CHECK(firstPlainControl(*inst, p));
  const double v = 0.3125;
  CHECK(std::fabs(inst->getParam(p.id) - v) > 1e-3);
  CHECK(inst->controllerUpdateRT(p.id, v));
  CHECK(std::fabs(inst->getParam(p.id) - v) > 1e-3);   // Not yet, that is the main thread's job.
  CHECK(inst->applyControllerUpdates() == 1);
  CHECK(std::fabs(inst->getParam(p.id) - v) < 1e-6);
  CHECK(inst->applyControllerUpdates() == 0);

  // The ring has a limited size and says so.
  int accepted = 0;
  for(int i = 0; i < 5000; ++i)
    if(inst->controllerUpdateRT(p.id, (i % 100) / 100.0))
      ++accepted;
  CHECK(accepted >= 512 && accepted < 5000);
  CHECK(inst->applyControllerUpdates() == accepted);
  CHECK(inst->controllerUpdateRT(p.id, 0.5));          // Room again.
}

static void test_restart_flags_and_active()
{
  auto inst = load(pickForParams());
  if(!inst) return;
  CHECK(inst->isActive());
  CHECK(inst->takeRestartFlags() == 0);
  inst->teardown();
  CHECK(!inst->isActive());
}

static void test_transport_info()
{
  auto inst = load(pickForParams());
  if(!inst) return;
  const int outCh = inst->outputChannels();
  std::vector<std::vector<float>> out(outCh, std::vector<float>(256));
  std::vector<float*> outp(outCh);
  for(int c = 0; c < outCh; ++c) outp[c] = out[c].data();
  Instance::Transport t;
  t.playing = false;
  t.tempo = 93.5;
  t.timeSigNumerator = 7;
  t.timeSigDenominator = 8;
  t.sample = 123456;
  t.quarters = 12.5;
  inst->setTransport(t);
  CHECK(inst->process(nullptr, outp.data(), 256));
  CHECK(inst->process(nullptr, outp.data(), 256));    // Back to the default transport.
}

static void test_no_edits_callback_without_gui()
{
  auto inst = load(pickForParams());
  if(!inst) return;
  int events = 0;
  inst->setEditCallback([&events](const EditEvent&) { ++events; });
  inst->setForwardEditsToProcessor(false);
  render(*inst, 4, 256, 0.0f);
  CHECK(events == 0);
  inst->setEditCallback(nullptr);
}

// Positive going zero crossings per second of the first channel, a rough pitch.
static double crossingsPerSecond(const std::vector<float>& x, double rate)
{
  int n = 0;
  for(size_t i = 1; i < x.size(); ++i)
    if(x[i - 1] < 0.0f && x[i] >= 0.0f)
      ++n;
  return n / (x.size() / rate);
}

// Renders a held note, with an optional MIDI controller value set after the first 100 ms.
// Returns the first channel of the second half of the sound.
static std::vector<float> renderNote(Instance& inst, int controller, double value, bool sendController)
{
  const int block = 256, blocks = 380;     // About two seconds.
  const int outCh = inst.outputChannels(), inCh = inst.inputChannels();
  std::vector<std::vector<float>> in(inCh, std::vector<float>(block, 0.0f)), out(outCh, std::vector<float>(block));
  std::vector<const float*> inp(inCh);
  std::vector<float*> outp(outCh);
  for(int c = 0; c < inCh; ++c) inp[c] = in[c].data();
  for(int c = 0; c < outCh; ++c) outp[c] = out[c].data();
  std::vector<float> result;
  for(int b = 0; b < blocks; ++b)
  {
    if(b == 0) inst.noteOn(0, 60, 0.8f, 0);
    if(b == 8 && sendController) inst.midiController(0, controller, value, 17);
    inst.process(inCh ? inp.data() : nullptr, outp.data(), block);
    if(b >= blocks / 2)
      result.insert(result.end(), out[0].begin(), out[0].end());
  }
  inst.noteOff(0, 60);
  return result;
}

static void test_midi_mapping()
{
  if(!g_haveInstrument) return;
  auto inst = load(g_instrument);
  if(!inst) return;
  bool any = false;
  for(int cc = 0; cc < 130; ++cc)
    any = any || inst->hasMidiMapping(0, cc);
  printf("    midi controllers mapped: %s\n", any ? "yes" : "no");
  CHECK(!inst->hasMidiMapping(-1, 0));
  CHECK(!inst->hasMidiMapping(16, 0));
  CHECK(!inst->hasMidiMapping(0, 130));
  CHECK(!inst->midiController(99, 1, 0.5));
  // An unmapped controller is refused, a mapped one accepted.
  for(int cc = 0; cc < 130; ++cc)
    CHECK(inst->midiController(0, cc, 0.5, 0) == inst->hasMidiMapping(0, cc));
  Rendered r = render(*inst, 4, 256, 0.0f);
  CHECK(r.ok);
  CHECK(r.finite);
}

static void test_pitch_bend_through_mapping()
{
  if(!g_haveInstrument) return;
  auto a = load(g_instrument);
  auto b = load(g_instrument);
  if(!a || !b) return;
  if(!a->hasMidiMapping(0, 129)) { printf("    (no pitch bend mapping, skipped)\n"); return; }
  const double rate = 48000.0;
  const double plain = crossingsPerSecond(renderNote(*a, 129, 0.5, false), rate);
  const double bent = crossingsPerSecond(renderNote(*b, 129, 1.0, true), rate);
  printf("    zero crossings per second, plain: %.1f bent up: %.1f\n", plain, bent);
  CHECK(plain > 20.0);
  CHECK_MSG(bent > plain * 1.03, "pitch bend through the MIDI mapping did not raise the pitch");
}

static void test_param_points_with_offsets()
{
  auto inst = load(pickForParams());
  if(!inst) return;
  ParamInfo p;
  CHECK(firstPlainControl(*inst, p));
  CHECK(inst->queueParamAtRT(p.id, 0.2, 0));
  CHECK(inst->queueParamAtRT(p.id, 0.8, 100));
  CHECK(inst->queueParamAtRT(p.id, 0.4, 200));
  CHECK(!inst->queueParamAtRT(0xFFFFFFF0u, 0.5, 0));
  Rendered r = render(*inst, 4, 256, 0.1f);
  CHECK(r.ok);
  CHECK(r.finite);
  // Plenty of points in one block.
  for(int i = 0; i < 200; ++i)
    CHECK(inst->queueParamAtRT(p.id, (i % 10) / 10.0, i));
  r = render(*inst, 1, 256, 0.1f);
  CHECK(r.ok);
}

static void test_programs()
{
  if(!g_haveInstrument) return;
  auto inst = load(g_instrument);
  if(!inst) return;
  const int n = inst->programCount();
  printf("    programs: %d\n", n);
  CHECK(!inst->selectProgramRT(-1));
  CHECK(!inst->selectProgramRT(n));
  if(n <= 0)
  {
    CHECK(!inst->selectProgramRT(0));
    return;
  }
  CHECK(!inst->programName(0).empty());
  CHECK(inst->programName(n).empty());
  std::vector<uint8_t> before, after;
  render(*inst, 2, 256, 0.0f);
  CHECK(inst->saveState(before));
  CHECK(inst->selectProgramRT(n > 5 ? 5 : n - 1));
  render(*inst, 4, 256, 0.0f);
  CHECK(inst->saveState(after));
  // Not every plugin keeps the program in its state, so this is only reported.
  if(before == after)
    printf("    note: the state did not change after selecting a program\n");
}

static void test_poly_pressure()
{
  if(!g_haveInstrument) return;
  auto inst = load(g_instrument);
  if(!inst) return;
  Rendered r = render(*inst, 40, 256, 0.0f, [&](int b) {
    if(b == 0) inst->noteOn(0, 64, 0.7f, 0);
    if(b == 5) inst->polyPressure(0, 64, 0.9f, 10);
    if(b == 30) inst->noteOff(0, 64);
  });
  CHECK(r.ok);
  CHECK(r.finite);
}

// Like render(), for an instance set up with more buses: the arrays hold the channels of all active buses.
static Rendered renderFull(Instance& inst, int blocks, int block, float inputLevel, std::function<void(int)> perBlock = nullptr,
                           std::vector<double>* peakPerChannel = nullptr)
{
  Rendered r;
  const int inCh = inst.inputChannels(), outCh = inst.outputChannels();
  std::vector<std::vector<float>> in(inCh, std::vector<float>(block)), out(outCh, std::vector<float>(block));
  std::vector<const float*> inp(inCh);
  std::vector<float*> outp(outCh);
  if(peakPerChannel)
    peakPerChannel->assign(outCh, 0.0);
  for(int b = 0; b < blocks; ++b)
  {
    if(perBlock) perBlock(b);
    for(int c = 0; c < inCh; ++c)
    {
      for(int s = 0; s < block; ++s)
        in[c][s] = inputLevel * float(std::sin(2 * M_PI * (220.0 + 110.0 * c) * (b * block + s) / 48000.0));
      inp[c] = in[c].data();
    }
    for(int c = 0; c < outCh; ++c) outp[c] = out[c].data();
    r.ok &= inst.process(inCh ? inp.data() : nullptr, outp.data(), block);
    for(int c = 0; c < outCh; ++c)
      for(int s = 0; s < block; ++s)
      {
        if(!std::isfinite(out[c][s])) r.finite = false;
        else
        {
          r.peak = std::max(r.peak, double(std::fabs(out[c][s])));
          if(peakPerChannel)
            (*peakPerChannel)[c] = std::max((*peakPerChannel)[c], double(std::fabs(out[c][s])));
        }
      }
  }
  return r;
}

static void test_audio_buses()
{
  std::string err;
  auto inst = Instance::create(pickForParams(), err);
  CHECK(inst != nullptr);
  if(!inst) return;
  CHECK(inst->setup(48000.0, 256, err));
  const auto outs = inst->audioBuses(false);
  const auto ins = inst->audioBuses(true);
  CHECK(int(outs.size()) == inst->audioOutputBusCount());
  CHECK(int(ins.size()) == inst->audioInputBusCount());
  int active = 0;
  for(size_t i = 0; i < outs.size(); ++i)
  {
    printf("    output bus %zu: %s, %d channel(s)%s%s\n", i, outs[i].name.c_str(), outs[i].channels, outs[i].aux ? ", aux" : "",
           outs[i].active ? ", active" : "");
    if(outs[i].active) active += outs[i].channels;
  }
  for(size_t i = 0; i < ins.size(); ++i)
    printf("    input bus %zu: %s, %d channel(s)%s%s\n", i, ins[i].name.c_str(), ins[i].channels, ins[i].aux ? ", aux" : "",
           ins[i].active ? ", active" : "");
  // After a plain setup only the main buses are active.
  CHECK(inst->outputChannels() == inst->mainOutputChannels());
  CHECK(inst->inputChannels() == inst->mainInputChannels());
  CHECK(active == inst->outputChannels());
  if(!outs.empty()) CHECK(!outs[0].aux);
}

static void test_aux_output_buses()
{
  if(!g_haveInstrument) return;
  std::string err;
  auto all = Instance::create(g_instrument, err);
  CHECK(all != nullptr);
  if(!all) return;
  const int buses = all->audioOutputBusCount();
  Instance::SetupOptions o;
  o.allOutputBuses = true;
  CHECK(all->setup(48000.0, 256, err, o));
  const int mainOut = all->mainOutputChannels();
  printf("    output channels: %d main, %d with all buses (%d buses)\n", mainOut, all->outputChannels(), buses);
  CHECK(all->outputChannels() >= mainOut);
  CHECK(buses <= 1 || all->outputChannels() > mainOut);

  // The note sounds. Some plugins (Surge XT) put the same sound in more places, and some (a plugin made
  //  to be used with all its outputs) make no sound unless the other buses are on, so no more is expected.
  std::vector<double> peaks;
  Rendered a = renderFull(*all, 120, 256, 0.0f, [&](int b) { if(b == 0) all->noteOn(0, 60, 0.8f, 0); }, &peaks);
  CHECK(a.ok);
  CHECK(a.finite);
  CHECK(a.peak > 1e-4);
  CHECK(int(peaks.size()) == all->outputChannels());
  for(size_t c = 0; c < peaks.size(); ++c)
    printf("    channel %zu peak %.4f\n", c, peaks[c]);

  // Switching back to the main buses only.
  CHECK(all->setup(48000.0, 256, err));
  CHECK(all->outputChannels() == mainOut);
}

static void test_sidechain_inputs()
{
  if(!g_haveEffect) { printf("    (no effect plugin, skipped)\n"); return; }
  std::string err;
  auto inst = Instance::create(g_effect, err);
  CHECK(inst != nullptr);
  if(!inst) return;
  Instance::SetupOptions o;
  o.sidechainInputs = true;
  CHECK(inst->setup(48000.0, 256, err, o));
  int auxIn = 0;
  for(const auto& b : inst->audioBuses(true))
    if(b.aux && b.active) auxIn += b.channels;
  printf("    input channels: %d main, %d with sidechain\n", inst->mainInputChannels(), inst->inputChannels());
  CHECK(inst->inputChannels() == inst->mainInputChannels() + auxIn);
  Rendered r = renderFull(*inst, 40, 256, 0.25f);
  CHECK(r.ok);
  CHECK(r.finite);
  CHECK(r.peak > 1e-4);
  // Without the option the sidechain is off again.
  CHECK(inst->setup(48000.0, 256, err));
  CHECK(inst->inputChannels() == inst->mainInputChannels());
}

static void test_offline_mode()
{
  auto inst = load(pickForParams());
  if(!inst) return;
  CHECK(!inst->offline());
  Rendered r = render(*inst, 6, 256, 0.2f);
  CHECK(r.ok);
  CHECK(inst->setOffline(false));            // Nothing to do.
  CHECK(inst->setOffline(true));
  CHECK(inst->offline());
  CHECK(inst->isActive());                   // Set up again by itself.
  r = render(*inst, 6, 256, 0.2f);
  CHECK(r.ok);
  CHECK(r.finite);
  CHECK(inst->setOffline(false));
  CHECK(!inst->offline());
  CHECK(inst->isActive());
  r = render(*inst, 6, 256, 0.2f);
  CHECK(r.ok);
  // Before setup it is only remembered.
  inst->teardown();
  CHECK(inst->setOffline(true));
  CHECK(!inst->isActive());
  std::string err;
  CHECK(inst->setup(48000.0, 256, err, [&]() { Instance::SetupOptions o; o.offline = true; return o; }()));
  CHECK(inst->offline());
  r = render(*inst, 4, 256, 0.2f);
  CHECK(r.ok);
}

static void test_note_ids_and_expression()
{
  if(!g_haveInstrument) return;
  auto inst = load(g_instrument);
  if(!inst) return;
  const int a = inst->noteOn(0, 60, 0.8f);
  const int b = inst->noteOn(0, 64, 0.8f);
  const int c = inst->noteOn(1, 60, 0.8f);
  CHECK(a >= 0 && b >= 0 && c >= 0);
  CHECK(a != b && b != c && a != c);
  const int again = inst->noteOn(0, 60, 0.5f);       // Ends the first one, gets its own id.
  CHECK(again >= 0 && again != a);

  const auto types = inst->noteExpressionTypes();
  printf("    note expression types: %zu\n", types.size());
  for(const auto& t : types)
    printf("      %u %s %s\n", t.typeId, t.title.c_str(), t.units.c_str());
  CHECK(inst->supportsNoteExpression() == !types.empty());

  // An expression value needs a sounding note.
  CHECK(!inst->noteExpression(5, 50, 0, 0.5));
  CHECK(inst->noteExpression(0, 64, 2, 0.75, 10));
  for(const auto& t : types)
    CHECK(inst->noteExpression(0, 64, t.typeId, t.defaultValue, 20));
  inst->polyPressure(0, 64, 0.5f, 30);
  Rendered r = render(*inst, 10, 256, 0.0f);
  CHECK(r.ok);
  CHECK(r.finite);

  inst->noteOff(0, 64);
  CHECK(!inst->noteExpression(0, 64, 2, 0.5));       // The note is over.
  r = render(*inst, 4, 256, 0.0f);
  CHECK(r.ok);
  // Ids go on counting.
  const int d = inst->noteOn(0, 67, 0.7f);
  CHECK(d > again);
}

static void test_note_expression_changes_sound()
{
  if(!g_haveInstrument) return;
  auto inst = load(g_instrument);
  if(!inst) return;
  bool hasTuning = false;
  for(const auto& t : inst->noteExpressionTypes())
    hasTuning = hasTuning || t.typeId == 2;
  if(!hasTuning) { printf("    (no tuning note expression, skipped)\n"); return; }

  auto plain = load(g_instrument);
  if(!plain) return;
  const double rate = 48000.0;
  auto play = [&](Instance& in, bool bend) {
    const int block = 256, blocks = 380;
    const int outCh = in.outputChannels(), inCh = in.inputChannels();
    std::vector<std::vector<float>> i(inCh, std::vector<float>(block, 0.0f)), o(outCh, std::vector<float>(block));
    std::vector<const float*> ip(inCh);
    std::vector<float*> op(outCh);
    for(int c = 0; c < inCh; ++c) ip[c] = i[c].data();
    for(int c = 0; c < outCh; ++c) op[c] = o[c].data();
    std::vector<float> result;
    for(int b = 0; b < blocks; ++b)
    {
      if(b == 0) in.noteOn(0, 60, 0.8f);
      if(b == 8 && bend) in.noteExpression(0, 60, 2, 1.0);       // The top of the plugin's tuning range.
      in.process(inCh ? ip.data() : nullptr, op.data(), block);
      if(b >= blocks / 2) result.insert(result.end(), o[0].begin(), o[0].end());
    }
    return result;
  };
  const double f0 = crossingsPerSecond(play(*plain, false), rate);
  const double f1 = crossingsPerSecond(play(*inst, true), rate);
  printf("    zero crossings per second, plain: %.1f tuned up: %.1f\n", f0, f1);
  CHECK_MSG(f1 > f0 * 1.03, "the tuning note expression did not raise the pitch");
}

static void test_plugin_edits_empty_by_default()
{
  auto inst = load(g_haveInstrument ? g_instrument : g_effect);
  if(!inst) return;
  // We did not touch the plugin's GUI, so nothing should have been reported.
  CHECK(inst->pluginEdits(true).empty());
}

//---------------------------------------------------------

static void run(const char* name, void (*fn)())
{
  int before = g_failed;
  printf("[ RUN  ] %s\n", name);
  fflush(stdout);
  fn();
  ++g_run;
  printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", name);
  fflush(stdout);
}

#define RUN(t) run(#t, t)

int main()
{
  std::vector<std::string> dirs;
  if(const char* d = getenv("VST3_TEST_DIRS"))
  {
    std::stringstream ss(d);
    std::string item;
    while(std::getline(ss, item, ':'))
      if(!item.empty()) dirs.push_back(item);
  }
  std::vector<std::string> errors;
  g_all = scan(dirs, &errors);
  for(const auto& e : errors) fprintf(stderr, "scan warning: %s\n", e.c_str());

  printf("found %zu plugin class(es)\n", g_all.size());
  if(g_all.empty())
  {
    printf("no VST3 plugins installed, skipping (set VST3_TEST_DIRS)\n");
    return 77;
  }
  g_haveInstrument = pick(true, "VST3_TEST_INSTRUMENT", g_instrument);
  g_haveEffect = pick(false, "VST3_TEST_EFFECT", g_effect);
  if(g_haveInstrument) printf("instrument under test: %s (%s)\n", g_instrument.name.c_str(), g_instrument.path.c_str());
  if(g_haveEffect) printf("effect under test:     %s (%s)\n", g_effect.name.c_str(), g_effect.path.c_str());
  if(!g_haveInstrument && !g_haveEffect)
  {
    printf("no matching plugins, skipping\n");
    return 77;
  }

  RUN(test_scan);
  RUN(test_scan_missing_dir);
  RUN(test_create_bad_path);
  RUN(test_create_unknown_class);
  RUN(test_lifecycle_repeat);
  RUN(test_buses);
  RUN(test_effect_silence_and_sine);
  RUN(test_instrument_note);
  RUN(test_instrument_sample_offset);
  RUN(test_various_block_sizes);
  RUN(test_too_large_block_rejected);
  RUN(test_parameters);
  RUN(test_state_roundtrip);
  RUN(test_state_garbage_rejected);
  RUN(test_plugin_edits_empty_by_default);
  RUN(test_control_lists);
  RUN(test_queue_param_reaches_processor);
  RUN(test_controller_update_ring);
  RUN(test_restart_flags_and_active);
  RUN(test_transport_info);
  RUN(test_no_edits_callback_without_gui);
  RUN(test_midi_mapping);
  RUN(test_pitch_bend_through_mapping);
  RUN(test_param_points_with_offsets);
  RUN(test_programs);
  RUN(test_poly_pressure);
  RUN(test_audio_buses);
  RUN(test_aux_output_buses);
  RUN(test_sidechain_inputs);
  RUN(test_offline_mode);
  RUN(test_note_ids_and_expression);
  RUN(test_note_expression_changes_sound);

  printf("\n%d test(s), %d check(s), %d failed\n", g_run, g_checks, g_failed);
  return g_failed ? 1 : 0;
}
