//=========================================================
//  MusE
//  Linux Music Editor
//
//  audioprofile.cpp
//  See audioprofile.h.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "audioprofile.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "audio.h"
#include "globals.h"
#include "route.h"
#include "song.h"
#include "track.h"

namespace MusECore {

bool audioProfileEnabled = false;

namespace {

int reportBeats = 100;        // The heart beat is about 20 times a second.
int beats = 0;

// The audio thread side.
std::atomic<uint64_t> cycleNs { 0 }, cycleMaxNs { 0 };
std::atomic<uint32_t> cycles { 0 };
std::atomic<uint64_t> cycleFrames { 0 };

// Tracks being processed, innermost last. The time of the outer one stops while an inner one runs.
struct Frame { AudioTrack* track; uint64_t start; };
const int MAX_DEPTH = 256;
thread_local Frame stack[MAX_DEPTH];
thread_local int depth = 0;
thread_local int overflow = 0;     // Scopes that did not fit on the stack.

void addTo(AudioTrack* t, uint64_t ns)
{
  TrackProfile& p = t->profile();
  p.ns.fetch_add(ns, std::memory_order_relaxed);
  if(ns > p.maxNs.load(std::memory_order_relaxed))
    p.maxNs.store(ns, std::memory_order_relaxed);
}

} // anonymous namespace

ProfileScope::ProfileScope(AudioTrack* track) : _active(audioProfileEnabled), _track(track)
{
  if(!_active)
    return;
  if(depth >= MAX_DEPTH)
  {
    ++overflow;
    _active = false;
    return;
  }
  const uint64_t now = audioProfileNow();
  if(depth > 0)
  {
    // The outer track waits for this one.
    Frame& outer = stack[depth - 1];
    addTo(outer.track, now - outer.start);
  }
  stack[depth++] = { track, now };
}

ProfileScope::~ProfileScope()
{
  if(!_active)
    return;
  const uint64_t now = audioProfileNow();
  Frame& f = stack[--depth];
  addTo(f.track, now - f.start);
  _track->profile().runs.fetch_add(1, std::memory_order_relaxed);
  if(depth > 0)
    stack[depth - 1].start = now;
}

void audioProfileCycle(uint64_t ns, unsigned frames)
{
  cycleNs.fetch_add(ns, std::memory_order_relaxed);
  if(ns > cycleMaxNs.load(std::memory_order_relaxed))
    cycleMaxNs.store(ns, std::memory_order_relaxed);
  cycleFrames.fetch_add(frames, std::memory_order_relaxed);
  cycles.fetch_add(1, std::memory_order_relaxed);
}

void audioProfileInit()
{
  const char* env = std::getenv("MUSE_AUDIO_PROFILE");
  if(!env || !*env)
    return;
  const double secs = std::atof(env);
  reportBeats = int((secs > 0.0 ? secs : 5.0) * 20.0);
  audioProfileEnabled = true;
  std::fprintf(stderr, "Audio profile: on, a report every %.1f s\n", reportBeats / 20.0);
}

//---------------------------------------------------------
//   The report
//---------------------------------------------------------

namespace {

struct Node
{
  AudioTrack* track = nullptr;
  std::string name;
  const char* type = "";
  double avgUs = 0.0;
  double maxUs = 0.0;
  unsigned runs = 0;
  std::vector<int> deps;       // Indexes of the nodes that must be done first.
  std::vector<int> users;
  std::string depNames;
  // Filled in by the analysis.
  double pathUs = 0.0;         // The longest time from here to the end, this one included.
  double finishUs = 0.0;       // For the longest chain ending here.
  int prev = -1;
};

const char* typeName(const Track* t)
{
  switch(t->type())
  {
    case Track::WAVE:           return "wave";
    case Track::AUDIO_OUTPUT:   return "output";
    case Track::AUDIO_INPUT:    return "input";
    case Track::AUDIO_GROUP:    return "group";
    case Track::AUDIO_AUX:      return "aux";
    case Track::AUDIO_SOFTSYNTH: return "synth";
    default:                    return "other";
  }
}

// Time to finish all the work with P workers, if the work is handed out the way a simple scheduler would:
//  whenever a worker is free it takes the ready task with the longest way left to the end.
double schedule(const std::vector<Node>& nodes, int workers)
{
  const int n = int(nodes.size());
  std::vector<int> left(n);
  for(int i = 0; i < n; ++i)
    left[i] = int(nodes[i].deps.size());
  std::vector<double> readyAt(n, 0.0);
  std::vector<bool> done(n, false), started(n, false);
  std::vector<double> workerFree(workers, 0.0);
  std::vector<double> finish(n, 0.0);
  int remaining = n;
  double now = 0.0;
  double makespan = 0.0;
  // Running tasks: finish time and node.
  std::multimap<double, int> running;
  std::set<std::pair<double, int>> readyList;   // (-path, node)
  for(int i = 0; i < n; ++i)
    if(left[i] == 0)
      readyList.insert({ -nodes[i].pathUs, i });
  int idle = workers;
  while(remaining > 0)
  {
    while(idle > 0 && !readyList.empty())
    {
      const int i = readyList.begin()->second;
      readyList.erase(readyList.begin());
      started[i] = true;
      running.insert({ now + nodes[i].avgUs, i });
      --idle;
    }
    if(running.empty())
      break;     // Should not happen in a graph without cycles.
    const auto first = running.begin();
    now = first->first;
    const int i = first->second;
    running.erase(first);
    ++idle;
    done[i] = true;
    --remaining;
    makespan = std::max(makespan, now);
    for(int u : nodes[i].users)
      if(--left[u] == 0)
        readyList.insert({ -nodes[u].pathUs, u });
  }
  return makespan;
}

void report()
{
  // What the audio thread counted.
  const uint32_t nCycles = cycles.exchange(0);
  const uint64_t cNs = cycleNs.exchange(0);
  const uint64_t cMax = cycleMaxNs.exchange(0);
  const uint64_t cFrames = cycleFrames.exchange(0);

  std::vector<Node> nodes;
  std::map<const AudioTrack*, int> index;

  auto add = [&](AudioTrack* t) -> int
  {
    auto it = index.find(t);
    if(it != index.end())
      return it->second;
    Node n;
    n.track = t;
    n.name = t->name().toStdString();
    n.type = typeName(t);
    TrackProfile& p = t->profile();
    n.runs = p.runs.exchange(0);
    const uint64_t ns = p.ns.exchange(0);
    n.maxUs = double(p.maxNs.exchange(0)) / 1000.0;
    n.avgUs = nCycles ? double(ns) / 1000.0 / double(nCycles) : 0.0;
    nodes.push_back(n);
    index[t] = int(nodes.size()) - 1;
    return int(nodes.size()) - 1;
  };

  const TrackList* tl = MusEGlobal::song->tracks();
  for(ciTrack it = tl->begin(); it != tl->end(); ++it)
    if((*it)->isAudioTrack())
      add(static_cast<AudioTrack*>(*it));

  // The edges, from the routes into a track and, for an aux track, from the tracks that send to it.
  // A track found only through a route (the metronome) is added as well.
  for(size_t i = 0; i < nodes.size(); ++i)
  {
    AudioTrack* t = nodes[i].track;
    std::set<int> deps;
    const RouteList* rl = t->inRoutes();
    for(ciRoute ir = rl->begin(); ir != rl->end(); ++ir)
    {
      if(ir->type != Route::TRACK_ROUTE || !ir->track || !ir->track->isAudioTrack())
        continue;
      deps.insert(add(static_cast<AudioTrack*>(ir->track)));
    }
    if(t->type() == Track::AUDIO_AUX)
      for(ciTrack it = tl->begin(); it != tl->end(); ++it)
      {
        if(!(*it)->isAudioTrack())
          continue;
        AudioTrack* s = static_cast<AudioTrack*>(*it);
        if(s->hasAuxSend() && !s->auxRefCount())
          deps.insert(add(s));
      }
    for(int d : deps)
    {
      nodes[i].deps.push_back(d);
      if(!nodes[i].depNames.empty())
        nodes[i].depNames += ", ";
      nodes[i].depNames += nodes[d].name;
    }
  }
  for(size_t i = 0; i < nodes.size(); ++i)
    for(int d : nodes[i].deps)
      nodes[d].users.push_back(int(i));

  // The longest way to the end from each node, and the longest chain from the start. Done by
  //  repeating, since the graph has no cycles but its order is not known.
  const int n = int(nodes.size());
  std::vector<int> order;
  {
    std::vector<int> left(n);
    std::vector<int> queue;
    for(int i = 0; i < n; ++i)
    {
      left[i] = int(nodes[i].deps.size());
      if(left[i] == 0) queue.push_back(i);
    }
    for(size_t q = 0; q < queue.size(); ++q)
    {
      order.push_back(queue[q]);
      for(int u : nodes[queue[q]].users)
        if(--left[u] == 0) queue.push_back(u);
    }
  }
  const bool cyclic = int(order.size()) != n;
  double totalUs = 0.0, criticalUs = 0.0;
  int criticalEnd = -1;
  for(const Node& nd : nodes)
    totalUs += nd.avgUs;
  if(!cyclic)
  {
    for(int i : order)
    {
      Node& nd = nodes[i];
      double best = 0.0;
      nd.prev = -1;
      for(int d : nd.deps)
        if(nodes[d].finishUs > best || nd.prev < 0)
        {
          best = nodes[d].finishUs;
          nd.prev = d;
        }
      nd.finishUs = best + nd.avgUs;
      if(nd.finishUs > criticalUs)
      {
        criticalUs = nd.finishUs;
        criticalEnd = i;
      }
    }
    for(int k = n - 1; k >= 0; --k)
    {
      Node& nd = nodes[order[k]];
      double best = 0.0;
      for(int u : nd.users)
        best = std::max(best, nodes[u].pathUs);
      nd.pathUs = best + nd.avgUs;
    }
  }

  // Print.
  const double budgetUs = nCycles && cFrames ? 1.0e6 * double(cFrames) / double(nCycles) / double(MusEGlobal::sampleRate) : 0.0;
  const double cycleAvgUs = nCycles ? double(cNs) / 1000.0 / double(nCycles) : 0.0;
  std::fprintf(stderr,
    "\n== Audio profile: %u cycles, %.0f frames each at %d Hz, a cycle may take %.0f us ==\n"
    "   a whole pass over the tracks: %.0f us on average (%.1f %% of a cycle), the longest %.0f us\n",
    nCycles, nCycles ? double(cFrames) / nCycles : 0.0, MusEGlobal::sampleRate, budgetUs,
    cycleAvgUs, budgetUs > 0 ? 100.0 * cycleAvgUs / budgetUs : 0.0, double(cMax) / 1000.0);
  if(nCycles == 0)
  {
    std::fprintf(stderr, "   (no audio was processed)\n");
    return;
  }

  std::vector<int> byTime(n);
  for(int i = 0; i < n; ++i) byTime[i] = i;
  std::sort(byTime.begin(), byTime.end(), [&](int a, int b) { return nodes[a].avgUs > nodes[b].avgUs; });
  std::fprintf(stderr, "   %-28s %-7s %9s %9s  %s\n", "track", "type", "avg us", "max us", "needs");
  for(int i : byTime)
  {
    const Node& nd = nodes[i];
    std::fprintf(stderr, "   %-28.28s %-7s %9.1f %9.1f  %s\n", nd.name.c_str(), nd.type, nd.avgUs, nd.maxUs,
                 nd.depNames.empty() ? "-" : nd.depNames.c_str());
  }

  std::fprintf(stderr, "   all tracks together: %.0f us\n", totalUs);
  if(cyclic)
  {
    std::fprintf(stderr, "   the routes have a cycle, no estimate\n");
    return;
  }
  std::string chain;
  for(int i = criticalEnd; i >= 0; i = nodes[i].prev)
    chain = nodes[i].name + (chain.empty() ? "" : " > " + chain);
  std::fprintf(stderr, "   the longest chain: %.0f us (%s)\n", criticalUs, chain.c_str());
  if(criticalUs > 0.0)
    std::fprintf(stderr, "   most that parallel processing could gain: %.2fx\n", totalUs / criticalUs);
  std::fprintf(stderr, "   estimated time of a pass, by number of threads:");
  for(int p : { 1, 2, 3, 4, 6, 8, 12 })
  {
    const double t = schedule(nodes, p);
    std::fprintf(stderr, "  %d: %.0f us (%.2fx)", p, t, t > 0 ? totalUs / t : 1.0);
  }
  std::fprintf(stderr, "\n   (the estimate leaves out the cost of handing work to other threads)\n");
  std::fflush(stderr);
}

} // anonymous namespace

void audioProfileTick()
{
  if(!audioProfileEnabled)
    return;
  if(++beats < reportBeats)
    return;
  beats = 0;
  if(MusEGlobal::song)
    report();
}

} // namespace MusECore
