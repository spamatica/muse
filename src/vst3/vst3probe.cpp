//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3probe.cpp
//  Command line tool for trying out the VST3 hosting code without MusE.
//
//    vst3probe list   [dir...]
//    vst3probe params <plugin> [dir...]
//    vst3probe render <plugin> [--note N] [--seconds S] [--rate R] [--block B] [--wav file] [dir...]
//    vst3probe midi   <plugin>
//      Shows which MIDI controllers the plugin maps to parameters, and its programs.
//    vst3probe state  <plugin> --out file [--set id=value ...] [dir...]
//      Saves the state of the plugin, after setting some normalized parameter values, as the text MusE keeps in
//      a song file ('vst3-state:' and base64). For making test songs.
//
//  <plugin> is a case insensitive part of the plugin name or the exact uid.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "vst3host.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace MusEVst3;

static std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

static bool findPlugin(const std::vector<PluginInfo>& all, const std::string& key, PluginInfo& out)
{
  for(const auto& p : all)
    if(p.uid == key || lower(p.name) == lower(key)) { out = p; return true; }
  for(const auto& p : all)
    if(lower(p.name).find(lower(key)) != std::string::npos) { out = p; return true; }
  return false;
}

static void writeWav(const char* file, const std::vector<std::vector<float>>& ch, int rate)
{
  FILE* f = fopen(file, "wb");
  if(!f) { perror(file); return; }
  uint32_t n = ch.empty() ? 0 : uint32_t(ch[0].size());
  uint16_t nc = uint16_t(ch.size());
  uint32_t dataBytes = n * nc * 4, v32;
  uint16_t v16;
  fwrite("RIFF", 1, 4, f); v32 = 36 + dataBytes; fwrite(&v32, 4, 1, f);
  fwrite("WAVEfmt ", 1, 8, f); v32 = 16; fwrite(&v32, 4, 1, f);
  v16 = 3; fwrite(&v16, 2, 1, f);              // IEEE float
  fwrite(&nc, 2, 1, f);
  v32 = rate; fwrite(&v32, 4, 1, f);
  v32 = rate * nc * 4; fwrite(&v32, 4, 1, f);
  v16 = nc * 4; fwrite(&v16, 2, 1, f);
  v16 = 32; fwrite(&v16, 2, 1, f);
  fwrite("data", 1, 4, f); fwrite(&dataBytes, 4, 1, f);
  for(uint32_t i = 0; i < n; ++i)
    for(uint16_t c = 0; c < nc; ++c)
      fwrite(&ch[c][i], 4, 1, f);
  fclose(f);
}

int main(int argc, char** argv)
{
  if(argc < 2)
  {
    fprintf(stderr, "usage: %s list|params|render ...\n", argv[0]);
    return 2;
  }
  std::string cmd = argv[1];
  std::string key;
  std::vector<std::string> dirs;
  int note = 60, rate = 48000, block = 256;
  double seconds = 2.0;
  const char* wav = nullptr;
  const char* stateOut = nullptr;
  std::vector<std::pair<unsigned, double>> sets;

  int i = 2;
  if(cmd != "list" && i < argc) key = argv[i++];
  for(; i < argc; ++i)
  {
    std::string a = argv[i];
    if(a == "--note" && i + 1 < argc)         note = atoi(argv[++i]);
    else if(a == "--seconds" && i + 1 < argc) seconds = atof(argv[++i]);
    else if(a == "--rate" && i + 1 < argc)    rate = atoi(argv[++i]);
    else if(a == "--block" && i + 1 < argc)   block = atoi(argv[++i]);
    else if(a == "--wav" && i + 1 < argc)     wav = argv[++i];
    else if(a == "--out" && i + 1 < argc)     stateOut = argv[++i];
    else if(a == "--set" && i + 1 < argc)
    {
      unsigned id = 0;
      double v = 0;
      if(sscanf(argv[++i], "%u=%lf", &id, &v) == 2)
        sets.emplace_back(id, v);
    }
    else dirs.push_back(a);
  }

  std::vector<std::string> errors;
  std::vector<PluginInfo> all = scan(dirs, &errors);
  for(const auto& e : errors)
    fprintf(stderr, "warning: %s\n", e.c_str());

  if(cmd == "list")
  {
    for(const auto& p : all)
      printf("%-32s %-18s %-24s %s\n  %s\n", p.name.c_str(), p.vendor.c_str(), p.subCategories.c_str(),
             p.uid.c_str(), p.path.c_str());
    printf("%zu plugin(s)\n", all.size());
    return 0;
  }

  PluginInfo info;
  if(!findPlugin(all, key, info))
  {
    fprintf(stderr, "no plugin matching '%s'\n", key.c_str());
    return 1;
  }
  std::string err;
  auto inst = Instance::create(info, err);
  if(!inst)
  {
    fprintf(stderr, "cannot load %s: %s\n", info.name.c_str(), err.c_str());
    return 1;
  }
  if(!inst->setup(rate, block, err))
  {
    fprintf(stderr, "cannot set up %s: %s\n", info.name.c_str(), err.c_str());
    return 1;
  }

  if(cmd == "params")
  {
    printf("%s: %d parameter(s)\n", info.name.c_str(), inst->paramCount());
    for(int p = 0; p < inst->paramCount(); ++p)
    {
      ParamInfo pi = inst->paramInfo(p);
      double v = inst->getParam(pi.id);
      printf("%10u  %-32s %-6s steps=%d default=%.3f value=%.3f (%s)%s%s\n", pi.id, pi.title.c_str(),
             pi.units.c_str(), pi.stepCount, pi.defaultNormalized, v, inst->paramToString(pi.id, v).c_str(),
             pi.readOnly ? " [ro]" : "", pi.hidden ? " [hidden]" : "");
    }
    return 0;
  }

  if(cmd == "midi")
  {
    printf("%s: notes=%s, programs=%d\n", info.name.c_str(), inst->acceptsNoteEvents() ? "yes" : "no", inst->programCount());
    printf("mapped controllers on channel 1:");
    for(int cc = 0; cc < 130; ++cc)
      if(inst->hasMidiMapping(0, cc))
        printf(" %d", cc);
    printf("\n");
    for(bool input : { true, false })
      for(const auto& b : inst->audioBuses(input))
        printf("%s bus %-24s %d channel(s)%s%s\n", input ? "input " : "output", b.name.c_str(), b.channels,
               b.aux ? ", aux" : "", b.defaultActive ? ", on by default" : "");
    for(const auto& t : inst->noteExpressionTypes())
      printf("note expression %u: %s %s\n", t.typeId, t.title.c_str(), t.units.c_str());
    for(int i = 0; i < inst->programCount() && i < 10; ++i)
      printf("  program %d: %s\n", i, inst->programName(i).c_str());
    return 0;
  }

  if(cmd == "state")
  {
    if(!stateOut) { fprintf(stderr, "state needs --out file\n"); return 2; }
    for(const auto& s : sets)
      inst->setParam(s.first, s.second);
    // Let the processor see the values.
    {
      int outCh = inst->mainOutputChannels(), inCh = inst->mainInputChannels();
      std::vector<std::vector<float>> in(inCh, std::vector<float>(block, 0.0f)), out(outCh, std::vector<float>(block));
      std::vector<const float*> inp(inCh);
      std::vector<float*> outp(outCh);
      for(int c = 0; c < inCh; ++c) inp[c] = in[c].data();
      for(int c = 0; c < outCh; ++c) outp[c] = out[c].data();
      for(int b = 0; b < 4; ++b)
        inst->process(inCh ? inp.data() : nullptr, outp.data(), block);
    }
    std::vector<uint8_t> blob;
    if(!inst->saveState(blob)) { fprintf(stderr, "saveState failed\n"); return 1; }
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string b64 = "vst3-state:";
    for(size_t i = 0; i < blob.size(); i += 3)
    {
      uint32_t v = blob[i] << 16;
      if(i + 1 < blob.size()) v |= blob[i + 1] << 8;
      if(i + 2 < blob.size()) v |= blob[i + 2];
      b64 += tbl[(v >> 18) & 63];
      b64 += tbl[(v >> 12) & 63];
      b64 += i + 1 < blob.size() ? tbl[(v >> 6) & 63] : '=';
      b64 += i + 2 < blob.size() ? tbl[v & 63] : '=';
    }
    FILE* f = fopen(stateOut, "w");
    if(!f) { perror(stateOut); return 1; }
    fputs(b64.c_str(), f);
    fclose(f);
    printf("wrote %zu bytes of state (%zu characters) to %s\n", blob.size(), b64.size(), stateOut);
    return 0;
  }

  if(cmd == "render")
  {
    int inCh = inst->mainInputChannels(), outCh = inst->mainOutputChannels();
    printf("%s: %d in / %d out channel(s), %d/%d audio bus(es), notes=%s, latency=%u, tail=%u\n",
           info.name.c_str(), inCh, outCh, inst->audioInputBusCount(), inst->audioOutputBusCount(),
           inst->acceptsNoteEvents() ? "yes" : "no", inst->latencySamples(), inst->tailSamples());
    if(outCh <= 0) { fprintf(stderr, "no audio output\n"); return 1; }

    // A 220 Hz sine as input for effects, nothing for instruments.
    std::vector<std::vector<float>> inBuf(inCh, std::vector<float>(block));
    std::vector<const float*> inPtr(inCh);
    std::vector<std::vector<float>> outBuf(outCh, std::vector<float>(block));
    std::vector<float*> outPtr(outCh);
    std::vector<std::vector<float>> rendered(outCh);

    long total = long(seconds * rate), done = 0, noteOffAt = total * 3 / 4;
    bool on = false, off = false;
    double phase = 0.0, peak = 0.0, sumSq = 0.0;
    long count = 0;
    bool bad = false;
    while(done < total)
    {
      int n = int(std::min<long>(block, total - done));
      if(inst->acceptsNoteEvents())
      {
        if(!on) { inst->noteOn(0, note, 0.8f); on = true; }
        if(!off && done + n > noteOffAt) { inst->noteOff(0, note); off = true; }
      }
      for(int c = 0; c < inCh; ++c)
      {
        for(int s = 0; s < n; ++s)
        {
          inBuf[c][s] = info.isInstrument ? 0.0f : 0.25f * float(std::sin(phase + 2 * M_PI * 220.0 * s / rate));
        }
        inPtr[c] = inBuf[c].data();
      }
      phase += 2 * M_PI * 220.0 * n / rate;
      for(int c = 0; c < outCh; ++c) outPtr[c] = outBuf[c].data();
      if(!inst->process(inCh ? inPtr.data() : nullptr, outPtr.data(), n))
        fprintf(stderr, "process() reported an error at sample %ld\n", done);
      for(int c = 0; c < outCh; ++c)
        for(int s = 0; s < n; ++s)
        {
          float v = outBuf[c][s];
          if(!std::isfinite(v)) bad = true;
          peak = std::max(peak, double(std::fabs(v)));
          sumSq += double(v) * v;
          ++count;
          rendered[c].push_back(v);
        }
      done += n;
    }
    printf("rendered %.2f s: peak=%.4f rms=%.4f%s\n", seconds, peak, count ? std::sqrt(sumSq / count) : 0.0,
           bad ? "  NON-FINITE SAMPLES!" : "");
    if(wav)
    {
      writeWav(wav, rendered, rate);
      printf("wrote %s\n", wav);
    }
    return bad ? 1 : 0;
  }

  fprintf(stderr, "unknown command '%s'\n", cmd.c_str());
  return 2;
}
