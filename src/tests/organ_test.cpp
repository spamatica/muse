//=========================================================
//  MusE
//  Linux Music Editor
//
//  organ_test.cpp
//  Test program for the Organ soft synth (synti/organ): loads organ.so,
//  renders notes and checks the defaults, the envelopes and that odd
//  controller values neither hang nor break the output.
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//
//=========================================================
//
//  Build, from the build directory (B = build dir, S = source dir):
//    S=.. ; g++ -std=c++17 -fPIC -o organ_test $S/tests/organ_test.cpp \
//      -I$S/synti -I$S/libs/mpevent -I$S/libs/evdata -I$S/libs/memory -I$S/libs/midi_controller \
//      -I$S/muse -I. $(pkg-config --cflags --libs Qt5Widgets) -ldl
//  Run:
//    QT_QPA_PLATFORM=offscreen ./organ_test synti/organ/organ.so
//
//  Only virtual functions of Mess are called, so nothing of MusE needs to be linked.
//  Exit code 0 when all checks pass.
//=========================================================

#include <QApplication>
#include <dlfcn.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "libsynti/mess.h"
#include "midictrl_consts.h"

// Copied from synti/organ/organ.h (the controller numbers are part of the song format).
static const int HARM0      =  0 + MusECore::CTRL_RPN14_OFFSET;
static const int ATTACK_LO  =  6 + MusECore::CTRL_RPN14_OFFSET;
static const int SUSTAIN_LO =  8 + MusECore::CTRL_RPN14_OFFSET;
static const int RELEASE_LO =  9 + MusECore::CTRL_RPN14_OFFSET;
static const int ATTACK_HI  = 10 + MusECore::CTRL_RPN14_OFFSET;
static const int SUSTAIN_HI = 12 + MusECore::CTRL_RPN14_OFFSET;
static const int RELEASE_HI = 13 + MusECore::CTRL_RPN14_OFFSET;
static const int NUM_HARM = 6;

static const int SR = 48000;
static const int BLOCK = 256;

static int failures = 0;
static int checks = 0;

#define CHECK(cond, ...) do { ++checks; if (!(cond)) { ++failures; \
      std::printf("  FAILED %s:%d: %s: ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static const MESS* descr = nullptr;

// MessConfig's constructor is in libsynti, which is linked into organ.so with hidden
//  symbols. Set the fields directly instead.
static MessConfig* makeConfig()
      {
      static char storage[sizeof(MessConfig)];
      std::memset(storage, 0, sizeof(storage));
      MessConfig* c = reinterpret_cast<MessConfig*>(storage);
      c->_segmentSize = BLOCK;
      c->_sampleRate = SR;
      c->_minMeterVal = -60;
      c->_configPath = c->_cachePath = c->_globalLibPath = c->_globalSharePath = "";
      c->_userPath = c->_projectPath = "";
      return c;
      }

static Mess* newOrgan()
      {
      return descr->instantiate(0, "organ test", makeConfig());
      }

// Render the given number of samples.
static std::vector<float> render(Mess* m, int samples)
      {
      std::vector<float> out(samples, 0.0f);
      for (int pos = 0; pos < samples; pos += BLOCK) {
            const int n = std::min(BLOCK, samples - pos);
            float* ports[1] = { out.data() };
            m->processMessages();
            m->process(0, ports, 1, pos, n);
            }
      return out;
      }

static float peak(const std::vector<float>& v, int from = 0, int to = -1)
      {
      if (to < 0) to = v.size();
      float p = 0.0f;
      for (int i = from; i < to; ++i)
            p = std::max(p, std::fabs(v[i]));
      return p;
      }

static bool finite(const std::vector<float>& v)
      {
      for (float f : v)
            if (!std::isfinite(f))
                  return false;
      return true;
      }

static int ms(int m) { return SR * m / 1000; }

// Level controllers are sent with the bias of 8192, as MusE and the gui do.
static int level(int cb) { return 8192 - cb; }

int main(int argc, char** argv)
      {
      if (argc < 2) {
            std::fprintf(stderr, "usage: %s <organ.so>\n", argv[0]);
            return 2;
            }
      QApplication app(argc, argv);
      void* h = dlopen(argv[1], RTLD_NOW);
      if (!h) {
            std::fprintf(stderr, "dlopen: %s\n", dlerror());
            return 2;
            }
      MESS_Descriptor_Function f = (MESS_Descriptor_Function)dlsym(h, "mess_descriptor");
      if (!f) {
            std::fprintf(stderr, "no mess_descriptor\n");
            return 2;
            }
      descr = f();
      // A hang in the audio code shows up as a timeout.
      alarm(60);

      std::printf("[ RUN  ] defaults make sound\n");
      {
            Mess* m = newOrgan();
            m->playNote(0, 60, 100);
            auto out = render(m, ms(200));
            CHECK(finite(out), "non-finite output");
            CHECK(peak(out, ms(100)) > 0.05f, "peak %f", peak(out, ms(100)));
            CHECK(peak(out) <= 1.0f, "peak %f", peak(out));
            m->playNote(0, 60, 0);
            out = render(m, ms(300));
            CHECK(peak(out, ms(100)) == 0.0f, "still sounding after release: %f", peak(out, ms(100)));
            delete m;
      }

      std::printf("[ RUN  ] controller info reports the shown defaults\n");
      {
            Mess* m = newOrgan();
            const char* name; int ctrl, mn, mx, init;
            int id = 0, n = 0;
            while ((id = m->getControllerInfo(id, &name, &ctrl, &mn, &mx, &init)) != 0) {
                  ++n;
                  CHECK(init >= mn && init <= mx, "%s: init %d not in %d..%d", name, init, mn, mx);
                  }
            CHECK(n == 19, "%d controllers", n);
            delete m;
      }

      std::printf("[ RUN  ] settings of one instance do not change the defaults of another\n");
      {
            Mess* a = newOrgan();
            for (int i = 0; i < NUM_HARM; ++i)
                  a->setController(0, HARM0 + i, level(960));
            a->playNote(0, 60, 100);
            CHECK(peak(render(a, ms(100))) == 0.0f, "all harmonics off but sounding");
            Mess* b = newOrgan();
            b->playNote(0, 60, 100);
            CHECK(peak(render(b, ms(100))) > 0.05f, "new instance is silent");
            delete b;

            std::printf("[ RUN  ] reset all controllers restores the defaults\n");
            a->setController(0, MusECore::CTRL_RESET_ALL_CTRL, 0);
            a->playNote(0, 62, 100);
            CHECK(peak(render(a, ms(100)), ms(50)) > 0.05f, "silent after reset");
            delete a;
      }

      std::printf("[ RUN  ] old songs: unbiased level values\n");
      {
            Mess* m = newOrgan();
            for (int i = 0; i < NUM_HARM; ++i)
                  m->setController(0, HARM0 + i, 0);      // 0 dB, as the organ stored its defaults
            m->setController(0, SUSTAIN_LO, 0);
            m->setController(0, SUSTAIN_HI, 0);
            m->playNote(0, 60, 100);
            CHECK(peak(render(m, ms(200)), ms(100)) > 0.05f, "silent");
            delete m;
      }

      std::printf("[ RUN  ] note off during the attack releases from the reached level\n");
      {
            Mess* m = newOrgan();
            m->setController(0, ATTACK_LO, 100);
            m->setController(0, ATTACK_HI, 100);
            m->setController(0, RELEASE_LO, 100);
            m->setController(0, RELEASE_HI, 100);
            m->playNote(0, 60, 100);
            auto before = render(m, ms(50));
            m->playNote(0, 60, 0);
            auto after = render(m, ms(200));
            // Before the fix the release started at the sustain level (0 dB). Halfway the
            //  attack the level is -48 dB, the release must start there.
            Mess* ref = newOrgan();
            ref->playNote(0, 60, 100);
            const float full = peak(render(ref, ms(100)), ms(50));
            delete ref;
            const float pb = peak(before, ms(30)), pa = peak(after);
            CHECK(pa < full * 0.02f, "jumps up at the release: before %f, after %f, full level %f", pb, pa, full);
            CHECK(peak(after, ms(150)) == 0.0f, "still sounding");
            delete m;
      }

      std::printf("[ RUN  ] decay goes from the full level down to the sustain level\n");
      {
            Mess* m = newOrgan();
            m->setController(0, ATTACK_LO, 0);
            m->setController(0, ATTACK_HI, 0);
            m->setController(0, SUSTAIN_LO, level(400));
            m->setController(0, SUSTAIN_HI, level(400));
            m->playNote(0, 60, 100);
            auto out = render(m, ms(300));
            const float start = peak(out, 0, ms(5));
            const float sustain = peak(out, ms(200), ms(300));
            CHECK(start > sustain * 3, "start %f, sustain %f", start, sustain);
            CHECK(sustain > 0.0f, "no sustain");
            delete m;
      }

      for (int lowFirst = 0; lowFirst < 2; ++lowFirst)
      {
            std::printf("[ RUN  ] the envelope that ends first does not come back (%s release shorter)\n",
                        lowFirst ? "low" : "high");
            Mess* m = newOrgan();
            m->setController(0, RELEASE_LO, lowFirst ? 10 : 400);
            m->setController(0, RELEASE_HI, lowFirst ? 400 : 10);
            m->playNote(0, 48, 100);
            auto held = render(m, ms(100));
            m->playNote(0, 48, 0);
            auto out = render(m, ms(500));
            // After the low release ended, only the decaying high part remains.
            float prev = peak(out, ms(20), ms(60));
            CHECK(prev <= peak(held, ms(50)), "louder than while held: %f", prev);
            for (int t = 60; t < 400; t += 40) {
                  const float p = peak(out, ms(t), ms(t + 40));
                  CHECK(p <= prev * 1.05f + 1e-6f, "rises at %d ms: %f after %f", t, p, prev);
                  prev = p;
                  }
            delete m;
      }

      std::printf("[ RUN  ] out of range controller values\n");
      {
            const int odd[] = { -100000, -8192, -1, 0, 1, 7000, 16383, 100000, 0x7fffffff };
            for (int v : odd) {
                  Mess* m = newOrgan();
                  for (int c = 0; c < 18; ++c)
                        m->setController(0, HARM0 + c, v);
                  m->playNote(0, 60, 100);
                  m->playNote(0, 127, 127);
                  auto out = render(m, ms(50));
                  m->playNote(0, 60, 0);
                  m->playNote(0, 127, 0);
                  auto rel = render(m, ms(50));
                  CHECK(finite(out) && finite(rel), "value %d: non-finite output", v);
                  CHECK(peak(out) <= 1.0f && peak(rel) <= 1.0f, "value %d: peak %f", v, std::max(peak(out), peak(rel)));
                  delete m;
                  }
      }

      std::printf("\n%d check(s), %d failed\n", checks, failures);
      return failures ? 1 : 0;
      }
