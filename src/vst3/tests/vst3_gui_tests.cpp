//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3_gui_tests.cpp
//  Tests for hosting VST3 plugin editors in Qt5 on X11. They need a display
//  (XWayland is fine) and real plugins. Windows open briefly while they run.
//  Skipped (exit code 77) without a display or without plugins.
//
//  Environment: VST3_TEST_DIRS, VST3_TEST_INSTRUMENT, VST3_TEST_EFFECT as for
//  vst3_tests. VST3_TEST_NO_CLICK=1 skips the synthetic mouse click test, VST3_TEST_REQUIRE_CLICK=1 makes a miss an error.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "../vst3editor.h"
#include "../vst3host.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QThread>

#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
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
static PluginInfo g_plugins[2];       // Instrument, effect.
static bool g_have[2] = { false, false };
static const char* g_names[2] = { "instrument", "effect" };

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

// Runs the Qt event loop and keeps the plugin processing, in step with the wall clock.
static void pump(Instance& inst, int ms)
{
  const int block = 256, rate = 48000;
  int inCh = inst.mainInputChannels(), outCh = inst.mainOutputChannels();
  static std::vector<std::vector<float>> in, out;
  in.assign(inCh, std::vector<float>(block, 0.0f));
  out.assign(outCh, std::vector<float>(block));
  std::vector<const float*> inp(inCh);
  std::vector<float*> outp(outCh);
  for(int c = 0; c < inCh; ++c) inp[c] = in[c].data();
  for(int c = 0; c < outCh; ++c) outp[c] = out[c].data();

  QElapsedTimer t;
  t.start();
  long done = 0;
  while(t.elapsed() < ms)
  {
    QApplication::processEvents(QEventLoop::AllEvents, 5);
    long due = long(t.elapsed() * rate / 1000) - done;
    while(due >= block)
    {
      inst.process(inCh ? inp.data() : nullptr, outp.data(), block);
      done += block;
      due -= block;
    }
    QThread::msleep(2);
  }
}

static std::unique_ptr<Instance> load(const PluginInfo& pi)
{
  std::string err;
  auto inst = Instance::create(pi, err);
  CHECK_MSG(inst != nullptr, "create: " + err);
  if(inst && !inst->setup(48000.0, 256, err))
  {
    CHECK_MSG(false, "setup: " + err);
    inst.reset();
  }
  return inst;
}

// X window geometry of the first child of the editor's native window (the plugin's window).
struct ChildInfo { int count = 0; unsigned w = 0, h = 0; bool mapped = false; };

static ChildInfo childOf(Display* dpy, Window parent)
{
  ChildInfo ci;
  Window root, par, *kids = nullptr;
  unsigned n = 0;
  if(XQueryTree(dpy, parent, &root, &par, &kids, &n) && n > 0)
  {
    ci.count = int(n);
    XWindowAttributes a;
    if(XGetWindowAttributes(dpy, kids[0], &a))
    {
      ci.w = a.width;
      ci.h = a.height;
      ci.mapped = a.map_state == IsViewable;
    }
  }
  if(kids) XFree(kids);
  return ci;
}

static Display* g_dpy = nullptr;

//---------------------------------------------------------

static void test_open_close_cycles(int which)
{
  auto inst = load(g_plugins[which]);
  if(!inst) return;
  for(int cycle = 0; cycle < 3; ++cycle)
  {
    Editor ed(*inst);
    std::string err;
    bool ok = ed.open(err);
    CHECK_MSG(ok, "open: " + err);
    if(!ok) return;
    CHECK(ed.isOpen());
    CHECK(ed.width() > 50 && ed.height() > 50);
    ed.show();
    pump(*inst, 400);
    CHECK(ed.isVisible());

    XSync(g_dpy, False);
    ChildInfo ci = childOf(g_dpy, ed.winId());
    CHECK_MSG(ci.count >= 1, "plugin created no X window inside the editor");
    CHECK_MSG(ci.mapped, "plugin window is not mapped");
    CHECK_MSG(int(ci.w) == ed.width() && int(ci.h) == ed.height(),
              "plugin window " + std::to_string(ci.w) + "x" + std::to_string(ci.h) + " vs editor " +
              std::to_string(ed.width()) + "x" + std::to_string(ed.height()));

    ed.close();               // Also detaches the view.
    CHECK(!ed.isOpen());
    pump(*inst, 100);         // Nothing may fire into the released view.
  }
}

static void test_runloop_registrations(int which)
{
  auto inst = load(g_plugins[which]);
  if(!inst) return;
  Editor ed(*inst);
  std::string err;
  if(!ed.open(err)) { CHECK_MSG(false, err); return; }
  ed.show();
  pump(*inst, 300);
  // Not every plugin uses the host run loop, but VSTGUI based ones register a timer or fds.
  printf("    run loop: %zu timer(s), %zu fd(s)\n", ed.runLoopTimers(), ed.runLoopFds());
  ed.closeView();
  CHECK(ed.runLoopTimers() == 0);
  CHECK(ed.runLoopFds() == 0);
}

static void test_resize(int which)
{
  auto inst = load(g_plugins[which]);
  if(!inst) return;
  Editor ed(*inst);
  std::string err;
  if(!ed.open(err)) { CHECK_MSG(false, err); return; }
  ed.show();
  pump(*inst, 300);
  if(ed.view()->canResize() != Steinberg::kResultTrue)
  {
    printf("    (editor is fixed size, skipped)\n");
    return;
  }
  int w = ed.width(), h = ed.height();
  ed.resize(w + 120, h + 80);
  pump(*inst, 400);
  XSync(g_dpy, False);
  ChildInfo ci = childOf(g_dpy, ed.winId());
  CHECK_MSG(int(ci.w) == ed.width() && int(ci.h) == ed.height(),
            "after resize plugin window " + std::to_string(ci.w) + "x" + std::to_string(ci.h) + " vs editor " +
            std::to_string(ed.width()) + "x" + std::to_string(ed.height()));
  CHECK(ed.width() > w || ed.height() > h);   // The constraint check may adjust, but not shrink it.
}

// Clicks and drags with synthetic X events across the editor, looking for a control that makes
// the plugin report an edit through IComponentHandler. This tests the GUI -> host parameter path.
static void test_gui_edit_reaches_host(int which)
{
  if(getenv("VST3_TEST_NO_CLICK")) { printf("    (skipped by VST3_TEST_NO_CLICK)\n"); return; }
  int ev, er, major, minor;
  if(!XTestQueryExtension(g_dpy, &ev, &er, &major, &minor)) { printf("    (no XTest, skipped)\n"); return; }

  auto inst = load(g_plugins[which]);
  if(!inst) return;
  Editor ed(*inst);
  std::string err;
  if(!ed.open(err)) { CHECK_MSG(false, err); return; }
  ed.move(100, 100);
  ed.show();
  ed.raise();
  ed.activateWindow();
  pump(*inst, 500);
  inst->pluginEdits(true);
  int callbackPerform = 0, callbackBegin = 0, callbackEnd = 0;
  inst->setEditCallback([&](const EditEvent& e) {
    if(e.type == EditEvent::Perform) ++callbackPerform;
    else if(e.type == EditEvent::Begin) ++callbackBegin;
    else ++callbackEnd;
  });

  Window root = DefaultRootWindow(g_dpy);
  Window child = 0;
  int rx = 0, ry = 0;
  XTranslateCoordinates(g_dpy, ed.winId(), root, 0, 0, &rx, &ry, &child);

  bool got = false;
  std::string where;
  // First a coarse grid with small drags, then a finer grid with long drags.
  const struct { int grid, dx, dy; } passes[] = { { 12, 6, -4 }, { 24, 25, -15 } };
  for(const auto& pass : passes)
    for(int gy = 1; gy < pass.grid && !got; ++gy)
      for(int gx = 1; gx < pass.grid && !got; ++gx)
      {
        int x = rx + ed.width() * gx / pass.grid, y = ry + ed.height() * gy / pass.grid;
        XTestFakeMotionEvent(g_dpy, -1, x, y, 0);
        XTestFakeButtonEvent(g_dpy, 1, True, 0);
        XFlush(g_dpy);
        pump(*inst, 15);
        XTestFakeMotionEvent(g_dpy, -1, x + pass.dx, y + pass.dy, 0);
        XFlush(g_dpy);
        pump(*inst, 15);
        XTestFakeButtonEvent(g_dpy, 1, False, 0);
        XFlush(g_dpy);
        pump(*inst, 30);
        auto edits = inst->pluginEdits(false);
        if(!edits.empty())
        {
          got = true;
          where = "at " + std::to_string(gx) + "/" + std::to_string(pass.grid) + "," + std::to_string(gy) +
                  ": param " + std::to_string(edits[0].first) + " = " + std::to_string(edits[0].second);
        }
      }
  if(got) printf("    plugin reported an edit %s\n", where.c_str());
  // Synthetic input does not always reach X windows on a Wayland compositor, it depends on the
  //  compositor and on window focus. So a miss is only a warning unless VST3_TEST_REQUIRE_CLICK is set.
  if(!got)
  {
    if(getenv("VST3_TEST_REQUIRE_CLICK"))
      CHECK_MSG(false, "no click or drag in the editor produced a parameter edit");
    else
      printf("    WARNING: no synthetic click produced a parameter edit (input injection not delivered?)\n");
  }
  if(got)
  {
    // The edit must also reach the processor without breaking it.
    pump(*inst, 100);
    // And the host callback saw it, with begin and end around the changes.
    CHECK_MSG(callbackPerform > 0, "edit callback saw no performEdit");
    CHECK_MSG(callbackBegin > 0 && callbackEnd > 0, "edit callback saw no beginEdit or endEdit");
  }
  inst->setEditCallback(nullptr);
}

static void test_close_via_instance_destruction_order(int which)
{
  // Editor first, instance after: the order MusE must follow. Repeated creation catches use after free.
  for(int i = 0; i < 3; ++i)
  {
    auto inst = load(g_plugins[which]);
    if(!inst) return;
    {
      Editor ed(*inst);
      std::string err;
      if(!ed.open(err)) { CHECK_MSG(false, err); return; }
      ed.show();
      pump(*inst, 150);
    }
    inst.reset();
  }
}

//---------------------------------------------------------

typedef void (*TestFn)(int);

static void run(const char* name, TestFn fn)
{
  for(int w = 0; w < 2; ++w)
  {
    if(!g_have[w]) continue;
    int before = g_failed;
    std::string full = std::string(name) + " [" + g_names[w] + ": " + g_plugins[w].name + "]";
    printf("[ RUN  ] %s\n", full.c_str());
    fflush(stdout);
    fn(w);
    ++g_run;
    printf("[ %s ] %s\n", g_failed == before ? " OK " : "FAIL", full.c_str());
    fflush(stdout);
  }
}

#define RUN(t) run(#t, t)

int main(int argc, char** argv)
{
  if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
    qputenv("QT_QPA_PLATFORM", "xcb");
  if(qEnvironmentVariableIsEmpty("DISPLAY"))
  {
    printf("no X display, skipping\n");
    return 77;
  }
  g_dpy = XOpenDisplay(nullptr);
  if(!g_dpy)
  {
    printf("cannot open the X display, skipping\n");
    return 77;
  }

  std::vector<std::string> dirs;
  if(const char* d = getenv("VST3_TEST_DIRS"))
  {
    std::stringstream ss(d);
    std::string item;
    while(std::getline(ss, item, ':'))
      if(!item.empty()) dirs.push_back(item);
  }
  g_all = scan(dirs);
  g_have[0] = pick(true, "VST3_TEST_INSTRUMENT", g_plugins[0]);
  g_have[1] = pick(false, "VST3_TEST_EFFECT", g_plugins[1]);
  if(!g_have[0] && !g_have[1])
  {
    printf("no VST3 plugins found, skipping\n");
    return 77;
  }
  for(int w = 0; w < 2; ++w)
    if(g_have[w]) printf("%s under test: %s\n", g_names[w], g_plugins[w].name.c_str());

  QApplication app(argc, argv);

  RUN(test_open_close_cycles);
  RUN(test_runloop_registrations);
  RUN(test_resize);
  RUN(test_close_via_instance_destruction_order);
  RUN(test_gui_edit_reaches_host);

  printf("\n%d test(s), %d check(s), %d failed\n", g_run, g_checks, g_failed);
  XCloseDisplay(g_dpy);
  return g_failed ? 1 : 0;
}
