//=========================================================
//  MusE
//  Linux Music Editor
//
//  audioparallel.cpp
//  See audioparallel.h.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "audioparallel.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>

#include <pthread.h>
#include <sched.h>
#include <semaphore.h>

#include "globals.h"
#include "gconfig.h"
#include "song.h"
#include "track.h"
#include "audiodev.h"
#include "audio.h"
#include "synth.h"
#include "mpevent.h"
#include "mididev.h"
#include "midi_consts.h"

namespace MusECore {

std::atomic<bool> audioParallelRunning { false };

namespace {

// Fixed sizes: the graph is built in the audio thread, where nothing may be allocated.
// A project that needs more is processed serially.
const int MAX_NODES = 1024;
const int MAX_EDGES = 16384;
const int MAX_THREADS = 16;
// Less than this is not worth the cost of waking the threads.
const int MIN_NODES = 3;

struct Node
{
  AudioTrack* track;
  int depBegin;        // Into Graph::deps.
  int depCount;
  bool audioOnly;      // Only the audio thread may process it (AudioTrack::processSerially).
  int level;           // Longest chain of tracks from this one to the end (in tracks).
  std::atomic<int> state;   // 0 idle, 1 running, 2 done
};

struct Graph
{
  uint64_t signature = 0;
  bool built = false;
  bool valid = false;       // Can be used (not too big, no cycle).
  int nodeCount = 0;
  Node nodes[MAX_NODES];
  int deps[MAX_EDGES];
  int order[MAX_NODES];     // Node numbers, the ones with the longest way left first.
};

Graph graph;

// The numbers of the run, set by the audio thread before the workers are woken.
unsigned runPos = 0;
unsigned runFrames = 0;
std::atomic<int> doneCount { 0 };
std::atomic<bool> running { false };
std::atomic<int> inside { 0 };
std::atomic<bool> stopping { false };

// Threads to use, including the audio thread. 0 = off. Set in the main thread, read in the audio thread.
std::atomic<int> activeThreads { 0 };
std::atomic<bool> autoMode { false };
int workerCount = 0;        // Started worker threads. Main thread only.
bool semReady = false;
bool workersRealTime = true;
bool rtWarned = false;
sem_t wakeSem;
pthread_t workers[MAX_THREADS];

// Chooses between parallel and serial processing by measuring both (audio thread only, except the
//  status numbers which the main thread reads). A cycle is timed from audioParallelProcess to
//  audioParallelEnd. The mode that is not in use is tried now and then, so it adapts when the project changes.
const int ADAPT_SAMPLES = 8;     // Cycles of each mode before the first decision.
const int ADAPT_PROBE = 32;      // Then every this many cycles the other mode is tried.
struct Adapt
{
  uint32_t cycle = 0;
  int nPar = 0, nSer = 0;
  double emaPar = 0.0, emaSer = 0.0;     // ns
  bool useParallel = true;
  bool thisParallel = false;             // The mode of the cycle in progress.
  bool started = false;
  uint64_t start = 0;
  int lastThreads = 0;
  void reset() { nPar = nSer = 0; emaPar = emaSer = 0.0; useParallel = true; cycle = 0; }
} adapt;

// For the settings dialog.
std::atomic<int> stState { 0 };          // See AudioParallelStatus::State.
std::atomic<uint64_t> stParNs { 0 };
std::atomic<uint64_t> stSerNs { 0 };
std::atomic<int> stTracks { 0 };
std::atomic<int> stChain { 0 };

// Tracks that take part. The outputs are left to the audio thread, they write to the driver.
inline bool participates(Track* t)
{
  return t && t->isAudioTrack() && t->type() != Track::AUDIO_OUTPUT;
}

//---------------------------------------------------------
//   signature
//    Changes when the tracks or what they depend on change. Computed in the audio thread, which is
//    the only one that changes the routes, so it is always consistent.
//---------------------------------------------------------

uint64_t signature()
{
  uint64_t h = 1469598103934665603ULL;
  auto mix = [&h](uint64_t v) { h ^= v; h *= 1099511628211ULL; };
  const TrackList* tl = MusEGlobal::song->tracks();
  mix(tl->size());
  for(ciTrack it = tl->begin(); it != tl->end(); ++it)
  {
    Track* t = *it;
    if(!participates(t))
      continue;
    AudioTrack* at = static_cast<AudioTrack*>(t);
    mix((uint64_t)(uintptr_t)at);
    mix(at->hasAuxSend() ? (at->auxRefCount() ? 2 : 1) : 0);
    mix(at->processSerially() ? 7 : 3);
    const RouteList* rl = at->inRoutes();
    for(ciRoute ir = rl->begin(); ir != rl->end(); ++ir)
      if(ir->type == Route::TRACK_ROUTE)
        mix((uint64_t)(uintptr_t)ir->track);
    mix(0xffffffffULL);
  }
  return h;
}

//---------------------------------------------------------
//   build
//---------------------------------------------------------

void build(Graph& g)
{
  g.built = true;
  g.valid = false;
  g.nodeCount = 0;
  g.signature = signature();

  const TrackList* tl = MusEGlobal::song->tracks();
  int n = 0;
  for(ciTrack it = tl->begin(); it != tl->end(); ++it)
  {
    if(!participates(*it))
      continue;
    if(n >= MAX_NODES)
      return;
    g.nodes[n].track = static_cast<AudioTrack*>(*it);
    g.nodes[n].audioOnly = g.nodes[n].track->processSerially();
    ++n;
  }
  g.nodeCount = n;

  // Track -> node number.
  static std::pair<const Track*, int> index[MAX_NODES];
  for(int i = 0; i < n; ++i)
    index[i] = std::make_pair(static_cast<const Track*>(g.nodes[i].track), i);
  std::sort(index, index + n);
  auto find = [&](const Track* t) -> int {
    const auto* p = std::lower_bound(index, index + n, std::make_pair(t, -1));
    return (p != index + n && p->first == t) ? p->second : -1;
  };

  int edges = 0;
  for(int i = 0; i < n; ++i)
  {
    Node& nd = g.nodes[i];
    nd.depBegin = edges;
    nd.depCount = 0;
    auto add = [&](int d) -> bool {
      if(d < 0 || d == i)
        return true;
      for(int k = nd.depBegin; k < nd.depBegin + nd.depCount; ++k)
        if(g.deps[k] == d)
          return true;
      if(edges >= MAX_EDGES)
        return false;
      g.deps[edges++] = d;
      ++nd.depCount;
      return true;
    };

    const RouteList* rl = nd.track->inRoutes();
    for(ciRoute ir = rl->begin(); ir != rl->end(); ++ir)
      if(ir->type == Route::TRACK_ROUTE && !add(find(ir->track)))
        return;

    // An aux track gets the sends of all the tracks that have them, see AudioAux::getData.
    if(nd.track->type() == Track::AUDIO_AUX)
      for(int j = 0; j < n; ++j)
        if(g.nodes[j].track->hasAuxSend() && !g.nodes[j].track->auxRefCount() && !add(j))
          return;
  }

  // Order the nodes so that every node comes after the nodes it depends on (Kahn). If some are left,
  //  there is a cycle and the graph is not used.
  static int pending[MAX_NODES];
  static int topo[MAX_NODES];
  static int succBegin[MAX_NODES + 1];
  static int succ[MAX_EDGES];
  int succFill[MAX_NODES];
  memset(succBegin, 0, sizeof(succBegin));
  for(int i = 0; i < n; ++i)
  {
    pending[i] = g.nodes[i].depCount;
    for(int k = 0; k < g.nodes[i].depCount; ++k)
      ++succBegin[g.deps[g.nodes[i].depBegin + k] + 1];
  }
  for(int i = 0; i < n; ++i)
    succBegin[i + 1] += succBegin[i];
  for(int i = 0; i < n; ++i)
    succFill[i] = succBegin[i];
  for(int i = 0; i < n; ++i)
    for(int k = 0; k < g.nodes[i].depCount; ++k)
      succ[succFill[g.deps[g.nodes[i].depBegin + k]]++] = i;

  int head = 0, tail = 0;
  for(int i = 0; i < n; ++i)
    if(pending[i] == 0)
      topo[tail++] = i;
  while(head < tail)
  {
    const int v = topo[head++];
    for(int k = succBegin[v]; k < succBegin[v + 1]; ++k)
      if(--pending[succ[k]] == 0)
        topo[tail++] = succ[k];
  }
  if(tail != n)
  {
    fprintf(stderr, "MusE: parallel audio: the tracks depend on each other in a loop, processing serially\n");
    return;
  }

  for(int i = 0; i < n; ++i)
    g.nodes[i].level = 0;
  for(int h = n - 1; h >= 0; --h)
  {
    const int v = topo[h];
    int lv = 0;
    for(int k = succBegin[v]; k < succBegin[v + 1]; ++k)
      lv = std::max(lv, g.nodes[succ[k]].level + 1);
    g.nodes[v].level = lv;
  }
  for(int i = 0; i < n; ++i)
    g.order[i] = i;
  std::stable_sort(g.order, g.order + n, [&g](int a, int b) { return g.nodes[a].level > g.nodes[b].level; });

  g.valid = true;
  int chain = 0;
  for(int i = 0; i < n; ++i)
    chain = std::max(chain, g.nodes[i].level + 1);
  stTracks.store(n);
  stChain.store(chain);
}

//---------------------------------------------------------
//   work
//    Run by the audio thread and the workers: take the tracks whose dependencies are done, until all are.
//---------------------------------------------------------

void runNode(Node& nd)
{
  AudioTrack* t = nd.track;
  const int chans = t->channels();
  // Just a dummy buffer, the consumers copy the track's output buffers when they need it.
  float* buff[chans];
  float data[runFrames * chans];
  for(int i = 0; i < chans; ++i)
    buff[i] = data + i * runFrames;
  t->copyData(runPos, -1, chans, chans, -1, -1, runFrames, buff);
}

void work(bool audioThread)
{
  Graph& g = graph;
  const int n = g.nodeCount;
  while(doneCount.load() < n && running.load())
  {
    bool progress = false;
    for(int oi = 0; oi < n; ++oi)
    {
      Node& nd = g.nodes[g.order[oi]];
      if(nd.state.load() != 0 || (nd.audioOnly && !audioThread))
        continue;
      bool ready = true;
      for(int k = 0; k < nd.depCount; ++k)
        if(g.nodes[g.deps[nd.depBegin + k]].state.load() != 2)
        {
          ready = false;
          break;
        }
      if(!ready)
        continue;
      int expected = 0;
      if(!nd.state.compare_exchange_strong(expected, 1))
        continue;
      runNode(nd);
      nd.state.store(2);
      doneCount.fetch_add(1);
      progress = true;
    }
    if(!progress)
      sched_yield();
  }
}

void* workerMain(void*)
{
  while(!stopping.load())
  {
    if(sem_wait(&wakeSem) != 0)
      continue;
    // Count ourselves in before looking at the flag, so the audio thread cannot finish
    //  and change the graph while we are still looking at it.
    inside.fetch_add(1);
    if(running.load())
      work(false);
    inside.fetch_sub(1);
  }
  return nullptr;
}

// Starts one more worker. Main thread.
bool startWorker()
{
  if(!semReady)
  {
    if(sem_init(&wakeSem, 0, 0) != 0)
    {
      fprintf(stderr, "MusE: parallel audio: sem_init failed\n");
      return false;
    }
    semReady = true;
  }

  int rv = EPERM;
  // The same real time priority as the audio thread, if there is one.
  if(MusEGlobal::realTimeScheduling && MusEGlobal::realTimePriority > 0)
  {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = MusEGlobal::realTimePriority;
    if(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) == 0 &&
       pthread_attr_setschedpolicy(&attr, SCHED_FIFO) == 0 &&
       pthread_attr_setschedparam(&attr, &param) == 0)
      rv = pthread_create(&workers[workerCount], &attr, workerMain, nullptr);
    pthread_attr_destroy(&attr);
  }
  if(rv != 0)
  {
    if(!rtWarned)
    {
      rtWarned = true;
      fprintf(stderr, "MusE: parallel audio: no real time priority for the worker threads (%s), "
                      "using normal ones. Expect dropouts.\n", strerror(rv));
    }
    workersRealTime = false;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 4 * 1024 * 1024);
    rv = pthread_create(&workers[workerCount], &attr, workerMain, nullptr);
    pthread_attr_destroy(&attr);
  }
  if(rv != 0)
  {
    fprintf(stderr, "MusE: parallel audio: could not start a worker thread: %s\n", strerror(rv));
    return false;
  }
  pthread_detach(workers[workerCount]);
  ++workerCount;
  return true;
}

} // anonymous namespace

//---------------------------------------------------------
//   audioParallelTick
//---------------------------------------------------------

void audioParallelTick()
{
  // The environment variable MUSE_AUDIO_THREADS=<n> overrides the settings, and turns the measuring off.
  static const char* env = getenv("MUSE_AUDIO_THREADS");

  // For trying the synths of all kinds without a keyboard: MUSE_AUDIO_TEST_NOTES=1 plays a note on every
  //  synth track now and then.
  static const bool testNotes = getenv("MUSE_AUDIO_TEST_NOTES") != nullptr;
  if(testNotes && MusEGlobal::audioDevice && MusEGlobal::audio && MusEGlobal::song)
  {
    static int beat = 0;
    const int b = beat++ % 100;
    if(b == 50 || b == 90)
    {
      const TrackList* tl = MusEGlobal::song->tracks();
      int k = 0;
      for(ciTrack it = tl->begin(); it != tl->end(); ++it, ++k)
      {
        if(!(*it)->isSynthTrack())
          continue;
        SynthI* si = static_cast<SynthI*>(*it);
        const int note = 48 + (k * 5) % 24;
        si->putEvent(MidiPlayEvent(0, si->midiPort(), 0, b == 50 ? ME_NOTEON : ME_NOTEOFF, note, b == 50 ? 100 : 0),
                     MidiDevice::NotLate);
      }
    }
  }

  if(!MusEGlobal::audioDevice || !MusEGlobal::audio)
    return;

  int wanted = env ? atoi(env) : MusEGlobal::config.audioThreads;
  wanted = std::min(std::max(wanted, 0), MAX_THREADS);
  if(wanted < 2)
    wanted = 0;
  // MUSE_AUDIO_THREADS_AUTO=0 or 1 overrides the setting for measuring, for trying.
  static const char* envAuto = getenv("MUSE_AUDIO_THREADS_AUTO");
  autoMode.store(envAuto ? atoi(envAuto) != 0 : MusEGlobal::config.audioThreadsAuto);

  // With the audio profile on (audioprofile.h), also say what the choice is now and then.
  static int statusTicks = 0;
  if(audioProfileEnabled && ++statusTicks % 100 == 0 && activeThreads.load() >= 2)
  {
    const AudioParallelStatus st = audioParallelStatus();
    static const char* names[] = { "off", "measuring", "parallel", "serial (not faster)", "not used (too small)" };
    fprintf(stderr, "parallel audio: %s, %d threads, a pass %.0f us in parallel, %.0f us serial\n",
            names[st.state], st.threads, st.parallelUs, st.serialUs);
  }

  // Start the threads here, in the main thread. Then tell the audio thread how many to use.
  while(wanted >= 2 && workerCount < wanted - 1 && workerCount < MAX_THREADS - 1 && startWorker())
    ;
  const int active = wanted >= 2 ? std::min(wanted, workerCount + 1) : 0;
  if(active != activeThreads.load())
  {
    activeThreads.store(active);
    if(active)
      fprintf(stderr, "MusE: parallel audio processing on %d threads%s\n", active, workersRealTime ? "" : " (not real time)");
    else
      fprintf(stderr, "MusE: parallel audio processing is off\n");
  }
}

//---------------------------------------------------------
//   audioParallelProcess
//---------------------------------------------------------

void audioParallelProcess(unsigned pos, unsigned frames)
{
  adapt.started = false;
  const int threads = activeThreads.load();
  if(threads < 2)
  {
    stState.store(AudioParallelStatus::Off);
    return;
  }

  Graph& g = graph;
  if(!g.built || g.signature != signature())
  {
    build(g);
    adapt.reset();
  }
  if(!g.valid || g.nodeCount < MIN_NODES)
  {
    stState.store(AudioParallelStatus::TooSmall);
    return;
  }
  if(threads != adapt.lastThreads)
  {
    adapt.lastThreads = threads;
    adapt.reset();
  }

  bool parallel = true;
  if(autoMode.load(std::memory_order_relaxed))
  {
    ++adapt.cycle;
    if(adapt.nPar < ADAPT_SAMPLES)
      parallel = true;
    else if(adapt.nSer < ADAPT_SAMPLES)
      parallel = false;
    else if(adapt.cycle % ADAPT_PROBE == 0)
      parallel = !adapt.useParallel;
    else
      parallel = adapt.useParallel;
    stState.store(adapt.nPar < ADAPT_SAMPLES || adapt.nSer < ADAPT_SAMPLES ? AudioParallelStatus::Measuring :
                  adapt.useParallel ? AudioParallelStatus::Parallel : AudioParallelStatus::SerialAuto);
  }
  else
    stState.store(AudioParallelStatus::Parallel);

  adapt.thisParallel = parallel;
  adapt.start = audioProfileNow();
  adapt.started = true;
  if(!parallel)
    return;

  for(int i = 0; i < g.nodeCount; ++i)
    g.nodes[i].state.store(0);
  doneCount.store(0);
  runPos = pos;
  runFrames = frames;
  audioParallelRunning.store(true);
  running.store(true);

  for(int i = 0; i < threads - 1; ++i)
    sem_post(&wakeSem);

  work(true);

  // Everything is done. Make sure the workers have left before anything changes.
  running.store(false);
  while(inside.load() != 0)
    sched_yield();
  audioParallelRunning.store(false);
}

//---------------------------------------------------------
//   audioParallelEnd
//---------------------------------------------------------

void audioParallelEnd()
{
  if(!adapt.started)
    return;
  adapt.started = false;
  const double dt = double(audioProfileNow() - adapt.start);
  const double alpha = 0.125;
  if(adapt.thisParallel)
  {
    adapt.emaPar = adapt.nPar == 0 ? dt : adapt.emaPar + alpha * (dt - adapt.emaPar);
    if(adapt.nPar < (1 << 20)) ++adapt.nPar;
    stParNs.store(uint64_t(adapt.emaPar));
  }
  else
  {
    adapt.emaSer = adapt.nSer == 0 ? dt : adapt.emaSer + alpha * (dt - adapt.emaSer);
    if(adapt.nSer < (1 << 20)) ++adapt.nSer;
    stSerNs.store(uint64_t(adapt.emaSer));
  }
  if(adapt.nPar >= ADAPT_SAMPLES && adapt.nSer >= ADAPT_SAMPLES)
  {
    // Serial is the safe one, so parallel must win by 10 % to be chosen, and loses as soon as it is slower.
    if(adapt.useParallel)
      adapt.useParallel = adapt.emaPar <= adapt.emaSer;
    else
      adapt.useParallel = adapt.emaPar < adapt.emaSer * 0.9;
  }
}

//---------------------------------------------------------
//   audioParallelStatus
//---------------------------------------------------------

AudioParallelStatus audioParallelStatus()
{
  AudioParallelStatus st;
  st.state = AudioParallelStatus::State(stState.load());
  st.threads = activeThreads.load();
  st.realTime = workersRealTime;
  st.tracks = stTracks.load();
  st.longestChain = stChain.load();
  st.parallelUs = double(stParNs.load()) / 1000.0;
  st.serialUs = double(stSerNs.load()) / 1000.0;
  return st;
}

} // namespace MusECore
