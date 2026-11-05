# AGENTS.md

Guidance for AI coding agents working in this repository.

## Overview

MusE is a MIDI/audio sequencer for Linux (C++17, Qt5, CMake). The git root is `muse_spamatica/`; all source lives in `src/` (this directory). It is a fork of https://github.com/muse-sequencer/muse (`master` tracks upstream). Besides JACK/RtAudio/dummy it has direct ALSA and PipeWire audio backends.

## Build

```
./compile_muse.sh          # creates build/, runs cmake (release, -DMODULES_BUILD_STATIC=ON), then `make clean all`
```

Note that the script runs `make clean`, so it always does a full rebuild. For incremental work use the existing `build/` directory:

```
cd build && cmake -DCMAKE_BUILD_TYPE=release -DMODULES_BUILD_STATIC=ON .. && make -j$(nproc)
make install               # prefix is set by -DCMAKE_INSTALL_PREFIX (the script uses $HOME/muse_spamatica)
```

- Feature toggles are CMake options in [CMakeLists.txt](CMakeLists.txt) (`ENABLE_ALSA`, `ENABLE_PIPEWIRE`, `ENABLE_RTAUDIO`, `ENABLE_LV2`, `ENABLE_VST_NATIVE`, `ENABLE_DSSI`, `ENABLE_OSC`, `ENABLE_FLUID`, `ENABLE_RUBBERBAND`, `ENABLE_PYTHON`, etc.). JACK is a core dependency. Each option wraps code in `#ifdef` guarded by a matching define in `config.h.in`.
- `MODULES_BUILD_STATIC=ON` links the internal modules (muse/ subdirectories, `libs/`, `al`, `awl`, ...) statically; the default is OFF (shared).
- Qt5 only (Widgets, Core, UiTools, LinguistTools, Xml, Svg). Dialogs/widgets are largely `.ui` files compiled via uic.
- VST3 (work in progress, see [README.vst3](README.vst3)): `-DENABLE_VST3=ON -DVST3_SDK_PATH=<vst3sdk checkout>` (default OFF). Plugin scanning and VST3 effects work (`muse/vst3.cpp`; bundles are scanned in the `muse_plugin_scan` sandbox; both use the host code in `vst3/`); VST3 instruments (synth tracks) work too. `vst3/` also builds stand-alone with its own tests (`ctest` there), and `ctest -R vst3_cache` runs the cache test from the MusE build dir.
- There is **no automated test suite for the application and no linter configuration**. Verify changes by building and running the `muse` binary. (`awl/ltest.cpp` is an unrelated widget demo.)
- Release steps (version bump in CMakeLists.txt, ChangeLog, `make package_source`, tagging) are in [README.developer](README.developer). Style guidance there is just "keep it consistent".

## Layout

- `muse/` – the application (main.cpp, app.cpp = main window, song, tracks, parts, events, plugins, the audio engine). Subdirectories: `driver/` (audio/MIDI/timer backends), `arranger/`, `midiedit/` (pianoroll, drum edit), `waveedit/`, `mixer/`, `master/` (tempo/sig/key editors), `liste/` (list editor), `marker/`, `cliplist/`, `components/` and `widgets/` (dialogs and custom widgets), `instruments/` (MIDI instrument definitions, MidNam), `ctrl/`, `lv2Support/`, `mplugins/`, `remote/` (Python/OSC control), `function_dialogs/`.
- `libs/` – shared helper libraries: `evdata`, `file`, `memory`, `midi_controller`, `midnam`, `mpevent`, `plugin` (scan/cache), `string`, `sysex_helper`, `time_stretch`, `wave` (`SndFile`), `xml`.
- `synti/` – bundled MESS soft synths (`deicsonze`, `fluid`/`fluidsynth`, `organ`, `s1`, `simpledrums2`, `vam`) built on `libsynti`/`libsimpleplugin`; `plugins/` – bundled LADSPA effects (doublechorus, freeverb, pandelay); `utils/` – `muse-find-unused-wavs`, `muse-song-convert.py`.
- `al/`, `awl/` – low-level time/position helpers and Qt widget library; `audio_convert/` – sample-rate/stretch converters.
- `share/`, `xpm/`, `svg/`, `qss/`, `resources.qrc` – runtime data, icons, themes. Translations come from `muse/` sources via Qt linguist.
- Docs worth reading: `README.developer`, `README.developer.undo_system`, `README.softsynth`, `README.ladspaguis`.

## Architecture: threads and the Operations system

Understand this before touching song/track/part/event data. (Full explanation: [README.developer.undo_system](README.developer.undo_system).)

- The realtime audio thread (driven by the audio driver callback; `Audio` in [muse/audio.h](muse/audio.h)) must never take mutexes, allocate, or print. The GUI thread therefore **must not mutate song containers (tracks, parts, events, routes, ...) directly**.
- Instead, build a `PendingOperationList` ([muse/operations.h](muse/operations.h)) and execute it via `MusEGlobal::audio->msgExecutePendingOperations(...)`. The GUI thread blocks while the audio thread applies the operations at the start of the next cycle. Undoable changes go through `Undo` / `UndoOp` in [muse/undo.cpp](muse/undo.cpp), which wraps operations and drives undo/redo.
- Operations have two stages: a REALTIME stage run in the audio callback, then a NON-REALTIME stage for cleanup/freeing. Do allocation and iterator lookup in the GUI thread beforehand so the realtime part is minimal.
- The Undo system has three stages (non-RT prep, RT, non-RT cleanup), merges repeated commands (a mouse drag stores only the final value) and can pass non-undoable operations through. Undo requests are not the same as Operation requests (e.g. Undo has `AddTempo`/`ModifyTempo`, Operations has `ModifyTempoList`).
- Other threads: `MidiSeq` (MIDI sequencer, [muse/midiseq.h](muse/midiseq.h)), `AudioPrefetch` (disk read/write for wave tracks), all derived from `Thread` ([muse/thread.h](muse/thread.h)). Cross-thread messages use the `seqmsg.cpp` / `msg*` mechanisms. Lock-free FIFOs (`lock_free_buffer.h`, `controlfifo`) pass data to the RT thread.
- Globals live in the `MusEGlobal` namespace (`globals.h`, `gconfig.h`); notably `MusEGlobal::song`, `audio`, `audioDevice`, `midiSeq`.

## Architecture: audio/MIDI drivers

- `AudioDevice` ([muse/driver/audiodev.h](muse/driver/audiodev.h)) is the abstract backend interface; `deviceType()` returns one of `DUMMY_AUDIO, JACK_AUDIO, RTAUDIO_AUDIO, ALSA_AUDIO, PIPEWIRE_AUDIO`. Implementations: `jack.cpp`, `rtaudio.cpp`, `alsaaudio.cpp`, `pipewireaudio.cpp` (+ `pipewiregraph.*`), `dummyaudio.cpp`. Each init function assigns `MusEGlobal::audioDevice`, and clears it on exit.
- Freewheel (process as fast as possible, used by bounce/export, `AudioDevice::setFreewheel`) is implemented by every backend: JACK natively; ALSA and dummy in their own loop thread; PipeWire and RtAudio with a thread of their own while the sound card/graph callback gives silence (moving the PipeWire node to the graph's freewheel driver was 5x slower). The driver must call `Audio::setFreewheel()` from the thread that processes.
- Backends differ in capabilities (e.g. xrun counting, DSP load reporting; recent commits make the GUI gray out or measure these when a driver lacks them). When adding a backend feature, check how the other drivers and the GUI handle absence.
- MIDI backends (`alsamidi.*`, `jackmidi.*`) and timer sources (`alsatimer`, `rtctimer`, `posixtimer`, `qttimer`, `simpletimer`, all behind `timerdev.h`) are independent of the audio backend.

## Architecture: data model

- `Song` ([muse/song.h](muse/song.h)) owns track lists (`Track` → `MidiTrack`, `WaveTrack`, `AudioTrack` → `AudioInput`/`AudioOutput`/`AudioGroup`/`AudioAux`, `SynthI`), each holding `Part`s of `Event`s. Tempo/sig/key maps are `tempo.*`, `sig.*`, `key.*`; positions are in `pos.*` with ticks/frames.
- Routing between tracks, audio ports and MIDI ports is in `route.*`; plugin hosting is split across `plugin.*` (LADSPA), `dssihost`, `lv2host`, `vst_native`, `synth.*`.
- Song files are XML (`songfile.cpp`, `libs/xml`); per-class `read()`/`write()` methods serialize each object.

## Conventions

- Style is loose; match the surrounding file (mixed indentation and brace styles exist, much code is from 2000s-era Werner Schweer). Files carry the GPL header (GPL v2+).
- Prefer Qt facilities over adding support libraries (see README.developer).
- Test cases: when you write any test case (unit test, test program, test data), put it in the `tests/` directory (`src/tests/`), not next to the code it tests. Existing tests elsewhere (`vst3/`) stay where they are.
- User-facing strings use `tr()`; `find_translation_concatenation.sh` and `find_manual_plural.sh` check for i18n problems. `UPDATE_TRANSLATIONS=ON` rewrites `share/locale/*.ts` in the source tree, so leave it OFF unless intended.
