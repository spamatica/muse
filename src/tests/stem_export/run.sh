#!/bin/sh
#
# Stem export test, see README.
#   run.sh [muse binary]
# Set STEMTEST_WORK to keep the files in a given folder (default: a new temporary folder).
# Set STEMTEST_FREEWHEEL=0 to render in real time instead of freewheeling.

set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MUSE=${1:-$HERE/../../build/muse/muse4}
WORK=${STEMTEST_WORK:-$(mktemp -d)}
# A binary in the build folder (not installed) needs the module libraries next to it.
BUILD=$(cd "$(dirname "$MUSE")/.." && pwd)
LIBS=$(find "$BUILD" -name '*.so' -printf '%h\n' | sort -u | tr '\n' ':')
mkdir -p "$WORK"
echo "Work folder: $WORK"

g++ -std=c++17 -shared -fPIC -o "$WORK/autotest_preload.so" "$HERE/autotest_preload.cpp" \
    $(pkg-config --cflags --libs Qt5Widgets)
python3 "$HERE/make_song.py" "$WORK/project"

# A home of its own, so the user's MusE configuration is not touched.
CFG="$WORK/home/.config/MusE/MusE"
mkdir -p "$CFG"
if [ "${STEMTEST_FREEWHEEL:-1}" = "0" ]; then
  # Read on top of the defaults.
  printf '<?xml version="1.0"?>\n<muse version="4.0">\n<configuration>\n<freewheelMode>0</freewheelMode>\n</configuration>\n</muse>\n' > "$CFG/MusE-seq.cfg"
else
  rm -f "$CFG/MusE-seq.cfg"
fi

# run_muse <song> <mode> <tracks> <stem folder> <log> [through output 1]
run_muse() {
  rm -rf "$4"
  HOME="$WORK/home" XDG_CONFIG_HOME="$WORK/home/.config" XDG_CACHE_HOME="$WORK/home/.cache" \
  XDG_DATA_HOME="$WORK/home/.local/share" QT_QPA_PLATFORM=${STEMTEST_QPA:-minimal} \
  STEMTEST_DIR="$4" STEMTEST_MODE=$2 STEMTEST_TRACKS=$3 STEMTEST_OUTPUT=${6:-0} \
  LD_LIBRARY_PATH="$LIBS$LD_LIBRARY_PATH" LD_PRELOAD="$WORK/autotest_preload.so" \
    "$MUSE" -b dummy -p -N -I -2 "$WORK/project/$1" > "$5" 2>&1 || true
  grep AUTOTEST "$5" || true
}

status=0
for mode in 0 1; do
  echo "=== wave tracks, mode $mode (0 = range markers, 1 = markers)"
  run_muse stemtest.med $mode A,B "$WORK/stems$mode" "$WORK/muse$mode.log"
  python3 "$HERE/check_stems.py" "$WORK/project" "$WORK/stems$mode" $mode || status=1
done

echo "=== wave tracks through the output track's effects (fader at 0.5)"
run_muse stemtest.med 0 A,B "$WORK/stemsout" "$WORK/museout.log" 1
python3 "$HERE/check_stems.py" "$WORK/project" "$WORK/stemsout" out || status=1

# Midi tracks on one synth need the bundled synths: set STEMTEST_APPDIR to a
#  'make install DESTDIR=<folder>' of this build.
if [ -n "$STEMTEST_APPDIR" ]; then
  echo "=== midi tracks on one synth"
  export APPDIR="$STEMTEST_APPDIR"
  run_muse miditest.med 0 M1,M2,Synth "$WORK/stemsmidi" "$WORK/musemidi.log"
  python3 "$HERE/check_stems.py" "$WORK/project" "$WORK/stemsmidi" midi || status=1
else
  echo "=== midi tracks: skipped (STEMTEST_APPDIR not set)"
fi
exit $status
