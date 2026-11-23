# MusE (spamatica fork): summary and handover

Status as of 2026-11-23. This file describes what this fork adds to upstream MusE, which branches
are in flight and what is unfinished. How the code works is in [src/AGENTS.md](src/AGENTS.md) (written
for coding agents, but the best architecture overview there is); building and releasing are in
[src/README](src/README) and [src/README.developer](src/README.developer).

## The fork and upstream

- `origin` = https://github.com/spamatica/muse, `upstream` = https://github.com/muse-sequencer/muse.
- `master` is upstream's master plus the work below. Upstream is merged into `master` from time to
  time (last: Wayland fixes, mid November 2026); `master` currently contains all of `upstream/master`.
- Work is done on a feature branch, which is merged into `master` when it is done (some went through a
  `bugfixes` collection branch first). Smaller things meant for upstream are offered as pull requests
  there (e.g. the Shift+Alt part drag with automation, which went in upstream and came back by merge).
- `src/ChangeLog` is upstream's log; the fork's changes are mostly not in it, only in the git history.
- Commits are only made when the developer asks for them (also stated in AGENTS.md for coding agents).

## What the fork adds (merged into master)

| Feature | Where | Notes |
|---|---|---|
| Direct ALSA audio backend | `muse/driver/alsaaudio.cpp` | CMake `ENABLE_ALSA` |
| Direct PipeWire audio backend | `muse/driver/pipewireaudio.cpp`, `pipewiregraph.*` | CMake `ENABLE_PIPEWIRE` |
| Freewheel (bounce/export as fast as possible) for ALSA, PipeWire, RtAudio, dummy | the drivers | PipeWire and RtAudio use a thread of their own while the card gets silence |
| DSP load measured / xrun display grayed out when a driver does not report them | GUI + drivers | |
| Parallel audio processing of the tracks | `muse/audioparallel.*`, `audioprofile.*` | Off by default; Global Settings > Audio, `MUSE_AUDIO_THREADS`, `MUSE_AUDIO_PROFILE=<s>`. Only used while measured to be faster |
| Recording directly to Ogg Vorbis, faster peak files | wave/recording code | Option in Global Settings > Audio |
| MP3 import | `libs/wave` | |
| Setting to turn off sample rate conversion of wave files | Global Settings | |
| Freeze Track (wave and synth tracks) | | Frozen tracks are read-only, automation edits refused |
| Folder tracks in the arranger | `track.*`, `song.cpp`, arranger | Shift+F adds one; see AGENTS.md for the data model |
| Per-part volume and fade in/out for wave parts | arranger, wave part code | |
| Drag all selected tracks in the track list; extended arranger zoom; automation column polish | arranger | |
| Save/load mixer configuration | `muse/components/mixerconfig*` | `*.med_mixer_config`, can also read from a song |
| Downmix dialog: suggested path, Ogg/MP3, one mixdown per marker | `markermixdown.*`, downmix dialog | |
| Marker list: in-line editing of the description | `muse/marker/` | |
| VST native effects get sample rate and block size on instantiate | `vst_native.cpp` | |
| Organ synth: sound with default settings, fixed envelopes | `synti/organ` | |
| Plugin scan: no rescan on every start, progress for big bundles | `libs/plugin`, scanner | |
| VST3 support (**unfinished**, see below) | `muse/vst3.cpp`, `vst3/` | CMake `ENABLE_VST3`, default OFF |
| Assorted crash fixes | | Quit with drum editor/piano roll open, folder tracks treated as audio tracks, clipboard, etc. |

## Unfinished work

### VST3 (on master, not finished)

Build with `-DENABLE_VST3=ON -DVST3_SDK_PATH=<vst3sdk checkout>` (SDK source checkout with submodules,
not the distro package). [src/README.vst3](src/README.vst3) is the detailed log, step by step, with
how each step was tried. Working: scanning (in the `muse_plugin_scan` sandbox), effects, instruments
as synth tracks, editors (X11/XWayland only), programs, state, automation, offline mode, auxiliary
output buses. Known gaps (from README.vst3):

- No sidechain buses, no MIDI input to effects, no MIDI output from instruments (arpeggiators), no
  sysex, note expression or note ids.
- The editor needs Qt's xcb platform; on native Wayland only an information dialog is shown.
- Step 7 "polish" is only partly done; see the list there.
- Tested mainly with Surge XT, Diva and Six Sines. Real mouse interaction in an embedded editor
  could not be tested automatically.

### MCP server (branch `mcp-server`, not merged, not finished)

A Model Context Protocol server inside MusE, so an AI client can read and edit the song
(Settings > MCP Server). HTTP on 127.0.0.1 only, bearer token, off by default, tools grouped in tiers
the user switches on: read, control, record, edit, dangerous. Code: `muse/components/mcpserver.*`,
`mcptools.cpp` (~3000 lines), `mcpdialog.*`; test: `tests/mcp_smoke_test.py PORT TOKEN` against a
running MusE. Adds a Qt Network dependency. The AGENTS.md on that branch describes it in detail.

Open points:
- Audio file import/export tools are not implemented.
- The branch is based on an older master and carries copies of three commits that are also on master
  (bounce seek, VST native sample rate, sample rate conversion setting); expect to merge master into
  it (or rebase) before it can go in.
- Last worked on 2026-11-17.

## Branches in flight

| Branch | State |
|---|---|
| `stems` | Export Stems (one file per track and range/marker, `stemexport.*`, `stemcapture.*`), plus two small fixes (built-in transport losing commands, arranger crash without clipboard data). Has a headless test: `src/tests/stem_export/run.sh`. Master merged in; not yet merged into master. |
| `mcp-server` | See above. |

Already merged (can be deleted when no longer wanted): `ogg_vorbis_recording`, `parallel-audio`,
`track_freeze`, `downmix`, `backport_to_muse`, `automation_follow_parts` (went upstream), and the
remote-only `bugfixes`, `folder-tracks`, `mixer-state`, `new-backends`, `part-volume-fixes`,
`ui-polish`, `vst3-support`. Older remote branches (`attic`, `experimental`, `infrastruct_*`, ...)
are upstream's.

## Building and testing

- `src/compile_muse.sh` does a full release build and installs to `$HOME/muse_spamatica`; for
  incremental builds use `build/` (see AGENTS.md).
- There is no test suite for the application. Tests that exist: `src/tests/stem_export/run.sh`
  (on `stems`; headless MusE with the dummy driver, checks exported stems sample by sample),
  `src/tests/mcp_smoke_test.py` (on `mcp-server`), and the VST3 host tests (`ctest` in `vst3/`,
  `ctest -R vst3_cache` from the build dir). Everything else is verified by running MusE.
- Test cases go in `src/tests/`.

## Things to know

- Qt5 only. Wayland: MusE forces `QT_QPA_PLATFORM=xcb` by default (upstream change); native Wayland
  is behind `ENABLE_WAYLAND` and not recommended.
- The realtime rules (no locks, allocation or printing in the audio thread; song changes only through
  the Operations/Undo system) apply to everything; see AGENTS.md and
  `src/README.developer.undo_system`. Parallel audio makes several threads run track processing, so
  code in `AudioTrack::copyData` and below must be safe for that.
- Loops over all tracks must check `Track::isAudioTrack()` before casting, because of folder tracks.
