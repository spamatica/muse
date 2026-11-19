#!/usr/bin/env python3
#
# Creates the test project for the stem export test (see README):
#   <dir>/a.wav, <dir>/b.wav   10 s of stereo 16 bit noise each, different seeds
#   <dir>/stemtest.med         wave tracks "A" and "B" playing them from frame 0,
#                              track "A" sends to the aux track "Aux" at 0.5.
#                              All three go to the output "Out 1", its fader is at 0.5.
#   <dir>/miditest.med         midi tracks "M1" and "M2" playing the same synth "Synth" (vam)
#                              at different times (needs the bundled MESS synths).
#
# Tempo 120 bpm, division 384: 384 ticks = 0.5 s = 22050 frames at 44100 Hz.
# Range markers: tick 384 - 3456   = frames 22050 - 198450
# Markers:       "Intro" at 384, "Verse" at 1920, unnamed at 3456
#                = sections 22050 - 110250 and 110250 - 198450

import struct
import sys
import wave
from pathlib import Path

RATE = 44100
FRAMES = 10 * RATE


def noise(path, seed):
    x = seed
    data = bytearray()
    for _ in range(FRAMES * 2):
        x = (x * 1103515245 + 12345) & 0x7fffffff
        # Keep well below full scale, the aux send adds to track A.
        data += struct.pack("<h", ((x >> 8) % 16001) - 8000)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(bytes(data))


def route(src):
    return f"""    <Route>
      <source name="{src}"/>
      <dest name="Out 1"/>
    </Route>
"""


def track(name, wav, aux_send):
    send = f'<auxSend idx="0">{aux_send}</auxSend>' if aux_send is not None else ""
    return f"""    <wavetrack>
      <AudioTrack>
        <Track>
          <name>{name}</name>
          <record>0</record>
          <mute>0</mute>
          <solo>0</solo>
          <off>0</off>
          <channels>2</channels>
          <locked>0</locked>
        </Track>
        <prefader>0</prefader>
        <sendMetronome>0</sendMetronome>
        <automation>0</automation>
        {send}
      </AudioTrack>
      <part>
        <name>{name}</name>
        <poslen sample="0" len="{FRAMES}" />
        <selected>0</selected>
        <color>0</color>
        <event>
          <poslen sample="0" len="{FRAMES}" />
          <frame>0</frame>
          <file>{wav}</file>
        </event>
      </part>
    </wavetrack>
"""


def main():
    d = Path(sys.argv[1])
    d.mkdir(parents=True, exist_ok=True)
    noise(d / "a.wav", 1)
    noise(d / "b.wav", 2)
    song = f"""<?xml version="1.0"?>
<muse version="4.0">
  <song>
    <info></info>
    <showinfo>0</showinfo>
    <cpos>0</cpos>
    <rpos>3456</rpos>
    <lpos>384</lpos>
    <master>1</master>
    <loop>0</loop>
    <punchin>0</punchin>
    <punchout>0</punchout>
    <record>0</record>
    <solo>0</solo>
    <recmode>0</recmode>
    <cycle>0</cycle>
    <click>0</click>
    <quantize>0</quantize>
    <len>7680</len>
    <follow>0</follow>
    <midiDivision>384</midiDivision>
    <sampleRate>{RATE}</sampleRate>
    <AudioOutput>
      <AudioTrack>
        <Track>
          <name>Out 1</name>
          <record>0</record>
          <mute>0</mute>
          <solo>0</solo>
          <off>0</off>
          <channels>2</channels>
          <locked>1</locked>
        </Track>
        <prefader>0</prefader>
        <sendMetronome>0</sendMetronome>
        <automation>0</automation>
        <controller id="0" cur="0.5" color="#ff0000" visible="0">
        </controller>
      </AudioTrack>
    </AudioOutput>
    <AudioAux>
      <AudioTrack>
        <Track>
          <name>Aux</name>
          <record>0</record>
          <mute>0</mute>
          <solo>0</solo>
          <off>0</off>
          <channels>2</channels>
          <locked>0</locked>
        </Track>
        <prefader>0</prefader>
        <sendMetronome>0</sendMetronome>
        <automation>0</automation>
      </AudioTrack>
    </AudioAux>
{track("A", "a.wav", 0.5)}{track("B", "b.wav", None)}{route("A")}{route("B")}{route("Aux")}    <marker tick="384" name="Intro" />
    <marker tick="1920" name="Verse" />
    <marker tick="3456" name="" />
  </song>
</muse>
"""
    (d / "stemtest.med").write_text(song)
    (d / "miditest.med").write_text(midi_song())


def midi_track(name, notes):
    events = "".join(f'          <event tick="{t}" len="{n}" a="{p}" b="100" />\n' for t, n, p in notes)
    return f"""    <miditrack>
      <Track>
        <name>{name}</name>
        <record>0</record>
        <mute>0</mute>
        <solo>0</solo>
        <off>0</off>
        <channels>0</channels>
        <locked>0</locked>
      </Track>
      <device>198</device>
      <channel>0</channel>
      <echo>0</echo>
      <part>
        <name>{name}</name>
        <poslen tick="0" len="7680" />
        <selected>0</selected>
        <color>0</color>
{events}      </part>
    </miditrack>
"""


# Two midi tracks play the same synth: M1 from tick 384 to 1084, M2 from 2304 to 3004.
# In the range markers (384 - 3456 = frames 22050 - 198450) that is
# M1 in file frames 0 - 40204, M2 in 110250 - 150454.
def midi_song():
    return f"""<?xml version="1.0"?>
<muse version="4.0">
  <song>
    <info></info>
    <showinfo>0</showinfo>
    <cpos>0</cpos>
    <rpos>3456</rpos>
    <lpos>384</lpos>
    <master>1</master>
    <loop>0</loop>
    <record>0</record>
    <solo>0</solo>
    <len>7680</len>
    <follow>0</follow>
    <midiDivision>384</midiDivision>
    <sampleRate>{RATE}</sampleRate>
    <AudioOutput>
      <AudioTrack>
        <Track>
          <name>Out 1</name>
          <channels>2</channels>
        </Track>
      </AudioTrack>
    </AudioOutput>
    <SynthI>
      <AudioTrack>
        <Track>
          <name>Synth</name>
          <record>0</record>
          <mute>0</mute>
          <solo>0</solo>
          <off>0</off>
          <channels>1</channels>
          <locked>0</locked>
        </Track>
        <prefader>0</prefader>
        <sendMetronome>0</sendMetronome>
        <automation>0</automation>
      </AudioTrack>
      <type>MESS</type>
      <file>vam</file>
      <label>vam</label>
      <name>Synth</name>
      <port>198</port>
      <midistate version="2">
        <event type="2" datalen="131">
          7c 02 01 00 20 00 00 00 20 00 00 01 00 00 00 01 
          00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
          00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
          00 00 00 fe 3e 00 00 fe 3e 00 00 00 00 00 00 00 
          00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 
          00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ff 
          3f 00 00 00 00 00 00 00 00 00 00 00 00 00 00 7a 
          3a 00 00 00 20 00 00 00 20 00 00 00 00 00 00 00 
          00 00 00 
        </event>
      </midistate>
    </SynthI>
{midi_track("M1", [(384, 700, 60), (384, 700, 64)])}{midi_track("M2", [(2304, 700, 67)])}  </song>
</muse>
"""


if __name__ == "__main__":
    main()
