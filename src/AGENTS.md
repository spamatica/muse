# AGENTS.md

Guidance for working in this repository. This is a fork of MusE (https://github.com/muse-sequencer/muse),
the Qt5 MIDI/audio sequencer for Linux (v4.3.0). Current branch `ogg_vorbis_recording` adds Ogg Vorbis
recording; `master` tracks upstream. The git root is one level above this directory (`../`), which only
holds `README.md`, `LICENSE`, `CONTRIBUTING`; all code lives in `src/` (this directory).

## Build

- C++17, CMake >= 3.10.2, Qt >= 5.10, gcc >= 7.3 / clang >= 6. Required libs: libsndfile, libsamplerate, JACK, LADSPA, extra-cmake-modules. Full list in [README](README).
- Quick build: `./compile_muse.sh` — creates `build/`, runs cmake (`-DCMAKE_BUILD_TYPE=release -DMODULES_BUILD_STATIC=ON`, install prefix `$HOME/muse_spamatica`), then `make clean all`. Note it does `make clean`, so it always rebuilds everything. For incremental builds run `make -j$(nproc)` inside `build/`.
- Optional features are CMake options in [CMakeLists.txt](CMakeLists.txt) (`ENABLE_LV2`, `ENABLE_DSSI`, `ENABLE_VST_NATIVE`, `ENABLE_RTAUDIO`, `ENABLE_ALSA`, `ENABLE_FLUID`, `ENABLE_RUBBERBAND`, `ENABLE_MIDNAM`, `ENABLE_INSTPATCH`, `ENABLE_PYTHON`, `ENABLE_OSC`, `ENABLE_LASH`, `ENABLE_LRDF`, `UPDATE_TRANSLATIONS`...). `MODULES_BUILD_STATIC` selects static vs shared internal modules.
- `build/` is in-tree and contains `compile_commands.json`. `compile_muse_mingw.sh` is the Windows cross build.
- There is no automated test suite (only `awl/ltest.cpp`, a widget test). Verify by building and running the `muse` binary.
- Version bumps: `MusE_VERSION*` in CMakeLists.txt; release steps in [README.developer](README.developer).
- `UPDATE_TRANSLATIONS=ON` rewrites `share/locale/*.ts` in the source tree — leave OFF unless intended.

## Layout

- `muse/` — the application (~112k lines in top-level .cpp alone). Each subdirectory has its own CMakeLists.txt and builds into a module/library:
  - Core model: `song`, `track`/`audiotrack`/`wavetrack`, `part`, `event`/`eventbase`/`midievent`/`waveevent`, `pos`, `tempo`, `sig`, `key`, `ctrl`, `midictrl`, `route`, `marker`.
  - Engine/threads: `audio.cpp` (realtime audio cycle), `midi.cpp`, `midiseq`, `thread`, `seqmsg.cpp`, `sync`, `mtc`, `transport`, `latency_compensator`, `audioprefetch` (disk read/write thread), `audio_fifo`, `lock_free_buffer.h`.
  - State change machinery: `operations.cpp/h` and `undo.cpp/h` (see below).
  - Devices/ports: `mididev`, `midiport`, `device.h`, `driver/` (JACK, ALSA, RtAudio, dummy audio; timers: ALSA/RTC/posix/qt/simple).
  - Plugins/synths: `plugin`, `pluglist`, `synth`, `dssihost`, `lv2host` + `lv2Support/` + `lv2Gtk2Support/`, `vst`, `vst_native`, `osc`, `ticksynth`. Plugin scanning/cache is in `libs/plugin/`.
  - Song I/O: `songfile.cpp`, `songfile_discovery`, `conf.cpp` (config), `importmidi`/`exportmidi`/`midifile`. Song files are XML (`libs/xml`, `al/xml`).
  - GUI: `app.cpp` (main window), `main.cpp` (entry), `arranger/`, `midiedit/` (piano roll, drum edit, score), `waveedit/`, `master/` (tempo/sig), `ctrl/` (controller lanes), `mixer/`, `liste/` (list editor), `cliplist/`, `marker/`, `components/` (~200 dialogs, mostly Qt Designer `.ui` + impl), `function_dialogs/`, `mplugins/` (MIDI filter/transform/remote), `instruments/` (instrument/controller editor), `widgets/` (custom Qt widgets: knob, meter, sliders, spinboxes).
  - `remote/` — Python control API (`pyapi`), gated by `ENABLE_PYTHON`.
- `libs/` — small shared libraries: `evdata`, `file`, `memory`, `midi_controller`, `midnam`, `mpevent`, `plugin`, `string`, `sysex_helper`, `time_stretch`, `wave`, `xml`.
- `al/` — low-level helpers (DSP incl. SSE variants, signature, xml). `awl/` — "Audio Widget Library": knobs, sliders, pos/tempo/pitch editors; loaded as a Qt Designer plugin.
- `audio_convert/` — pluggable audio converters/stretchers: `rubberband/`, `src_resampler/`, `zita_resampler/`.
- `synti/` — MESS softsynths (`deicsonze`, `fluid`/`fluidsynth`, `organ`, `s1`, `simpledrums2`, `vam`) built on `libsynti`/`libsimpleplugin`; `plugins/` — bundled LADSPA effects (doublechorus, freeverb, pandelay).
- `utils/` — `muse-find-unused-wavs`, `muse-song-convert.py`. Also: `grepmidi*/`, `sandbox/`, `vestige/` (VST header), `windows/`, `packaging/`, `share/` (icons, scores, locale `.ts`, templates, instruments), `qss/`, `svg/`, `xpm/`, `resources.qrc`, `doc/`, `man/`, `demos/`.

## Key architecture rules

- **Realtime safety**: the audio thread must never take mutexes, allocate memory, or print. GUI-thread changes to song data go through the **Operations** system (`operations.h`) — a request is composed in the GUI thread, then executed in a REALTIME stage inside the audio callback followed by a NON-REALTIME stage (cleanup, freeing). Pre-find iterators and pre-allocate in the GUI thread so the realtime part is minimal.
- **Undo** (`undo.h`) wraps Operations: "Undo operations" (e.g. `AddTempo`, `ModifyTempo`) are distinct from Operation requests (e.g. `ModifyTempoList`). The Undo system has three stages (non-RT prep, RT, non-RT cleanup), can merge/reduce repeated commands (e.g. a mouse drag stores only the final value), and can pass non-undoable operations through. Prefer using the Undo system for user-visible edits. Read [README.developer.undo_system](README.developer.undo_system) before touching either.
- Other docs: [README.developer](README.developer) (style is relaxed; prefer Qt facilities over extra libraries), `README.softsynth`, `README.ladspaguis`, `README.vstsdk`, `README.win32`, `ChangeLog`.

## Conventions

- Style is loose; match the surrounding file (mixed indentation and brace styles exist, much code is from 2000s-era Werner Schweer). Files carry the GPL header.
- Qt Designer `.ui` files are compiled via CMake; `compile_components_ui.pl` is a helper script.
- User-facing strings use `tr()`; `find_translation_concatenation.sh` and `find_manual_plural.sh` check for i18n problems.
- GPL v2+.
