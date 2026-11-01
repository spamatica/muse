//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3gui.cpp
//  Opens a VST3 plugin's own editor in a Qt5 window and keeps the plugin
//  processing, to try out the editor hosting without MusE.
//
//    vst3gui <plugin> [--seconds S] [--screenshot file.png] [--wav file] [--rate R] [dir...]
//
//  There is no audio output. Audio is processed in step with the wall clock
//  and can be saved with --wav. Hold the button of the small control window
//  to play a note on an instrument. Edits made in the plugin's GUI are printed.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "vst3editor.h"
#include "vst3host.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QTimer>
#include <QVBoxLayout>
#include <QWindow>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace MusEVst3;

static std::string lower(std::string s)
{
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
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
  v16 = 3; fwrite(&v16, 2, 1, f);
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
  // VST3 on Linux embeds X11 windows, under Wayland that means XWayland.
  if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "xcb");

  if(argc < 2)
  {
    fprintf(stderr, "usage: %s <plugin> [--seconds S] [--screenshot file.png] [--wav file] [--rate R] [dir...]\n", argv[0]);
    return 2;
  }
  std::string key = argv[1];
  std::vector<std::string> dirs;
  double seconds = 0.0;
  int rate = 48000;
  const char* shot = nullptr;
  const char* wav = nullptr;
  for(int i = 2; i < argc; ++i)
  {
    std::string a = argv[i];
    if(a == "--seconds" && i + 1 < argc)         seconds = atof(argv[++i]);
    else if(a == "--screenshot" && i + 1 < argc) shot = argv[++i];
    else if(a == "--wav" && i + 1 < argc)        wav = argv[++i];
    else if(a == "--rate" && i + 1 < argc)       rate = atoi(argv[++i]);
    else dirs.push_back(a);
  }

  QApplication app(argc, argv);

  std::vector<PluginInfo> all = scan(dirs);
  const PluginInfo* found = nullptr;
  for(const auto& p : all)
    if(p.uid == key || lower(p.name) == lower(key)) { found = &p; break; }
  if(!found)
    for(const auto& p : all)
      if(lower(p.name).find(lower(key)) != std::string::npos) { found = &p; break; }
  if(!found)
  {
    fprintf(stderr, "no plugin matching '%s'\n", key.c_str());
    return 1;
  }
  PluginInfo info = *found;

  const int block = 256;
  std::string err;
  auto inst = Instance::create(info, err);
  if(!inst || !inst->setup(rate, block, err))
  {
    fprintf(stderr, "cannot load %s: %s\n", info.name.c_str(), err.c_str());
    return 1;
  }

  Editor editor(*inst);
  if(!editor.open(err))
  {
    fprintf(stderr, "cannot open the editor of %s: %s\n", info.name.c_str(), err.c_str());
    return 1;
  }
  editor.setWindowTitle(QString::fromStdString(info.name + " (VST3)"));
  editor.show();
  printf("%s: editor %d x %d, %s\n", info.name.c_str(), editor.width(), editor.height(),
         editor.view()->canResize() == Steinberg::kResultTrue ? "resizable" : "fixed size");

  // A small window with a note button, and the last edit made in the plugin GUI.
  QWidget controls;
  controls.setWindowTitle("vst3gui");
  QVBoxLayout lay(&controls);
  QPushButton noteBtn("Hold to play C4");
  QLabel editLabel("no edits yet");
  noteBtn.setEnabled(inst->acceptsNoteEvents());
  lay.addWidget(&noteBtn);
  lay.addWidget(&editLabel);
  bool noteHeld = false, wantOn = false, wantOff = false;
  QObject::connect(&noteBtn, &QPushButton::pressed, [&]() { wantOn = true; });
  QObject::connect(&noteBtn, &QPushButton::released, [&]() { wantOff = true; });
  controls.move(editor.x() + editor.width() + 20, editor.y());
  controls.show();

  // Process in step with the wall clock.
  const int outCh = inst->mainOutputChannels(), inCh = inst->mainInputChannels();
  std::vector<std::vector<float>> in(inCh, std::vector<float>(block, 0.0f)), out(outCh, std::vector<float>(block));
  std::vector<const float*> inp(inCh);
  std::vector<float*> outp(outCh);
  for(int c = 0; c < inCh; ++c) inp[c] = in[c].data();
  for(int c = 0; c < outCh; ++c) outp[c] = out[c].data();
  std::vector<std::vector<float>> rendered(outCh);

  QElapsedTimer clock;
  clock.start();
  long done = 0;
  QTimer pump;
  QObject::connect(&pump, &QTimer::timeout, [&]() {
    long due = long(clock.elapsed() * rate / 1000) - done;
    while(due >= block)
    {
      if(wantOn && !noteHeld)  { inst->noteOn(0, 60, 0.8f); noteHeld = true; }
      if(wantOff && noteHeld)  { inst->noteOff(0, 60); noteHeld = false; }
      wantOn = wantOff = false;
      inst->process(inCh ? inp.data() : nullptr, outp.data(), block);
      if(wav)
        for(int c = 0; c < outCh; ++c)
          rendered[c].insert(rendered[c].end(), out[c].begin(), out[c].end());
      done += block;
      due -= block;
    }
  });
  pump.start(4);

  QTimer edits;
  QObject::connect(&edits, &QTimer::timeout, [&]() {
    for(const auto& e : inst->pluginEdits(true))
    {
      std::string s = inst->paramToString(e.first, e.second);
      printf("edit: param %u = %.4f (%s)\n", e.first, e.second, s.c_str());
      editLabel.setText(QString("param %1 = %2").arg(e.first).arg(QString::fromStdString(s)));
    }
    fflush(stdout);
  });
  edits.start(50);

  if(shot)
    QTimer::singleShot(int(std::max(1.0, seconds / 2) * 1000), [&]() {
      QPixmap pm = editor.screen()->grabWindow(editor.winId());
      printf("screenshot %s: %s (%d x %d)\n", shot, pm.save(shot) ? "saved" : "FAILED", pm.width(), pm.height());
    });
  if(seconds > 0)
    QTimer::singleShot(int(seconds * 1000), &app, &QApplication::quit);
  QObject::connect(&app, &QApplication::lastWindowClosed, &app, &QApplication::quit);

  app.exec();

  pump.stop();
  edits.stop();
  editor.closeView();
  if(wav)
    writeWav(wav, rendered, rate);
  return 0;
}
