#!/usr/bin/env python3
# Smoke test of the MCP server in a running MusE.
#
# Usage: mcp_smoke_test.py PORT TOKEN
#
# Needs MusE running with the MCP server enabled (Settings > MCP Server), and a song with some tracks.
# Only uses the Python standard library. Exits with 1 if anything fails.
#
# The tests of a tier only run if that tier is switched on in the MCP dialog; for a tier that is off the test
# checks that its tools are not available. The Edit tier tests make scratch tracks (named mcptest<pid>-...),
# and the other tiers' tests use them, so enable Edit as well for the most complete (and least intrusive)
# run. The scratch tracks are deleted at the end if the Dangerous tier is on, otherwise the end says which
# are left. The test never records, never saves the song, and puts back what it changes on existing tracks.
# It briefly starts and stops the transport if the Control tier is on.

import http.client
import json
import os
import sys
import time

if len(sys.argv) != 3:
    print(__doc__)
    sys.exit(2)

PORT = int(sys.argv[1])
TOKEN = sys.argv[2]
failures = 0


def check(cond, what):
    global failures
    print(("ok    " if cond else "FAIL  ") + what)
    if not cond:
        failures += 1


def request(method, path="/mcp", body=None, headers=None, token=TOKEN, host=None):
    conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=30)
    h = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    if token is not None:
        h["Authorization"] = "Bearer " + token
    if headers:
        h.update(headers)
    data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
    conn.putrequest(method, path, skip_host=host is not None)
    if host is not None:
        conn.putheader("Host", host)
    for k, v in h.items():
        conn.putheader(k, v)
    conn.putheader("Content-Length", str(len(data or b"")))
    conn.endheaders(data)
    r = conn.getresponse()
    raw = r.read()
    conn.close()
    try:
        parsed = json.loads(raw) if raw else None
    except ValueError:
        parsed = None
    return r.status, parsed


_id = 0


def rpc(method, params=None):
    global _id
    _id += 1
    msg = {"jsonrpc": "2.0", "id": _id, "method": method}
    if params is not None:
        msg["params"] = params
    return request("POST", body=msg)


def call(name, args=None):
    status, body = rpc("tools/call", {"name": name, "arguments": args or {}})
    return status, body


# --- Security -------------------------------------------------------------------------------------------
status, _ = request("POST", body={"jsonrpc": "2.0", "id": 1, "method": "ping"}, token=None)
check(status == 401, "no token is refused (401)")
status, _ = request("POST", body={"jsonrpc": "2.0", "id": 1, "method": "ping"}, token="wrong" * 10)
check(status == 401, "wrong token is refused (401)")
status, _ = request("POST", body={"jsonrpc": "2.0", "id": 1, "method": "ping"}, host="evil.example.com")
check(status == 403, "foreign Host header is refused (403)")
status, _ = request("POST", body={"jsonrpc": "2.0", "id": 1, "method": "ping"}, headers={"Origin": "https://evil.example.com"})
check(status == 403, "foreign Origin is refused (403)")
status, _ = request("POST", body={"jsonrpc": "2.0", "id": 1, "method": "ping"}, headers={"Origin": "http://localhost:3000"})
check(status == 200, "local Origin is accepted")
status, _ = request("GET")
check(status == 405, "GET is not allowed (405)")
status, _ = request("POST", path="/other", body={})
check(status == 404, "other paths are 404")
status, body = request("POST", body=b"{not json")
check(status == 400 and body and body["error"]["code"] == -32700, "bad JSON gives a parse error (400)")
status, body = request("POST", body=b"[]")
check(status == 400, "batches are refused (400)")

# --- Protocol ------------------------------------------------------------------------------------------
status, body = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {},
                                  "clientInfo": {"name": "smoke-test", "version": "1"}})
check(status == 200 and body["result"]["protocolVersion"] == "2025-06-18", "initialize echoes a supported version")
check(body["result"]["serverInfo"]["name"] == "MusE", "serverInfo is MusE")
status, body = rpc("initialize", {"protocolVersion": "1999-01-01"})
check(body["result"]["protocolVersion"] == "2025-06-18", "initialize answers with the latest for an unknown version")
status, body = request("POST", body={"jsonrpc": "2.0", "method": "notifications/initialized"})
check(status == 202, "a notification is accepted (202)")
status, body = rpc("ping")
check(status == 200 and body["result"] == {}, "ping")
status, body = rpc("nothing/here")
check(body["error"]["code"] == -32601, "unknown method is -32601")

status, body = rpc("tools/list")
tools = {t["name"]: t for t in body["result"]["tools"]}
READ_TOOLS = {"get_song_info", "get_transport", "list_tracks", "get_track", "list_parts", "get_levels", "measure_levels",
              "list_audio_ports", "list_markers", "list_plugins", "list_synths", "list_plugin_presets", "get_midi_notes",
              "get_audio_part", "get_tempo_map", "get_drum_map", "list_midi_ports", "list_patches"}
CONTROL_TOOLS = {"set_patch", "transport", "set_position", "set_loop", "set_click", "set_locators", "set_track_mute", "set_track_solo",
                 "set_track_volume", "set_track_pan", "set_plugin_param", "set_plugin_on"}
RECORD_TOOLS = {"set_track_record_arm", "record"}
EDIT_TOOLS = {"add_track", "rename_track", "add_midi_part", "add_midi_notes", "move_part", "add_route", "remove_route",
              "undo", "redo", "add_plugin", "add_synth_track", "move_tracks_to_folder", "save_plugin_preset",
              "load_plugin_preset", "modify_midi_notes", "delete_midi_notes", "set_tempo", "set_time_signature",
              "set_tempo_map_enabled", "set_midi_track_output", "add_marker", "modify_marker", "delete_marker"}
DANGEROUS_TOOLS = {"delete_track", "delete_part", "remove_plugin", "save_song"}

check(READ_TOOLS <= set(tools), "tools/list has all the read tools (%d tools)" % len(tools))
check(all(t["inputSchema"]["type"] == "object" for t in tools.values()), "every tool has an object schema")
check(all(t["annotations"]["readOnlyHint"] == (n in READ_TOOLS) for n, t in tools.items()),
      "only the read tools are marked read only")
has_control = CONTROL_TOOLS <= set(tools)
has_record = RECORD_TOOLS <= set(tools)
has_edit = EDIT_TOOLS <= set(tools)
has_danger = DANGEROUS_TOOLS <= set(tools)
for name, tier_tools, on in (("Control", CONTROL_TOOLS, has_control), ("Record", RECORD_TOOLS, has_record),
                             ("Edit", EDIT_TOOLS, has_edit), ("Dangerous", DANGEROUS_TOOLS, has_danger)):
    listed = tier_tools & set(tools)
    check(on or not listed, "%s tier is all on or all off (%d of %d tools listed)" % (name, len(listed), len(tier_tools)))
    print("      %s tier: %s" % (name, "on" if on else "off"))

status, body = call("no_such_tool")
check(body["error"]["code"] == -32602, "unknown tool is an error")

# --- Tools ---------------------------------------------------------------------------------------------
def result(name, args=None):
    status, body = call(name, args)
    if status != 200 or "result" not in body:
        return None, body
    r = body["result"]
    return (None if r["isError"] else r["structuredContent"]), r


r, _ = result("get_song_info")
check(r is not None and r["sampleRate"] > 0 and r["trackCount"] >= 1, "get_song_info")
if r:
    print("      %s tracks, %d Hz, %s" % (r["trackCount"], r["sampleRate"], r["tracksByType"]))

r, _ = result("get_transport")
check(r is not None and "cursor" in r and r["cursor"]["bar"] >= 1, "get_transport")

r, _ = result("list_tracks")
check(r is not None and len(r["tracks"]) >= 1, "list_tracks")
tracks = r["tracks"] if r else []
audio = [t for t in tracks if t["type"].startswith("audio_") or t["type"] in ("wave", "synth")]
wave = [t for t in tracks if t["type"] == "wave"]

if tracks:
    r, _ = result("get_track", {"name": tracks[0]["name"]})
    check(r is not None and r["name"] == tracks[0]["name"], "get_track")
r, err = result("get_track", {"name": "no such track"})
check(r is None and err["isError"], "get_track with a wrong name is a tool error")

if audio:
    r, _ = result("get_track", {"name": audio[0]["name"]})
    check(r is not None and "effects" in r and "outRoutes" in r and "gainDb" in r, "get_track of an audio track has effects and routes")

r, _ = result("list_parts")
check(r is not None and isinstance(r["parts"], list), "list_parts (all)")
if tracks:
    r, _ = result("list_parts", {"track": tracks[0]["name"]})
    check(r is not None, "list_parts (one track)")

r, _ = result("get_levels")
check(r is not None and len(r["tracks"]) == len(audio), "get_levels covers the audio tracks")
if audio:
    r, _ = result("get_levels", {"tracks": [audio[0]["name"]]})
    check(r is not None and len(r["tracks"]) == 1 and "peakDb" in r["tracks"][0]["channels"][0], "get_levels of one track")
r, err = result("get_levels", {"tracks": ["no such track"]})
check(r is None and err["isError"], "get_levels with a wrong name is a tool error")

t0 = time.time()
r, _ = result("measure_levels", {"duration_ms": 500})
dt = time.time() - t0
check(r is not None and 0.4 < dt < 3.0 and len(r["tracks"]) == len(audio), "measure_levels takes the time asked (%.2fs)" % dt)
if r and r["tracks"]:
    c = r["tracks"][0]["channels"][0]
    check("peakDb" in c and "rmsDb" in c, "measure_levels has peak and rms")
    print("      %s: peak %.1f dB, rms %.1f dB, %d frames" % (r["tracks"][0]["name"], c["peakDb"], c["rmsDb"], r["tracks"][0]["framesMeasured"]))

r, _ = result("list_audio_ports")
check(r is not None and "sources" in r and "destinations" in r, "list_audio_ports")
r, _ = result("list_markers")
check(r is not None and isinstance(r["markers"], list), "list_markers")


# --- Read tools added later ------------------------------------------------------------------------------
r, _ = result("list_plugins", {"limit": 5})
check(r is not None and r["total"] >= len(r["plugins"]) and len(r["plugins"]) <= 5, "list_plugins with a limit")
plugins = r["plugins"] if r else []
if plugins:
    check(all(k in plugins[0] for k in ("name", "label", "type", "uri", "file", "audioInputs")), "list_plugins has the fields")
    r2, _ = result("list_plugins", {"filter": plugins[0]["label"]})
    check(r2 is not None and r2["total"] >= 1, "list_plugins filter finds a plugin by its label")
r, _ = result("list_plugins", {"filter": "zzzz-no-such-plugin-zzzz"})
check(r is not None and r["total"] == 0, "list_plugins filter can find nothing")
r, _ = result("list_synths", {"limit": 5})
check(r is not None and "synths" in r and len(r["synths"]) <= 5, "list_synths")
synths = r["synths"] if r else []
r, _ = result("list_plugin_presets")
check(r is not None and isinstance(r["presets"], list), "list_plugin_presets")
r, err = result("list_plugin_presets", {"track": "no such track", "slot": 1})
check(r is None and err["isError"], "list_plugin_presets with a wrong track is a tool error")
r, err = result("get_midi_notes", {"track": "no such track", "part_start_tick": 0})
check(r is None and err["isError"], "get_midi_notes with a wrong track is a tool error")

r, err = result("get_audio_part", {"track": "no such track", "part_start_tick": 0})
check(r is None and err["isError"], "get_audio_part with a wrong track is a tool error")
r, err = result("get_drum_map", {"track": "no such track"})
check(r is None and err["isError"], "get_drum_map with a wrong track is a tool error")
tm, _ = result("get_tempo_map")
check(tm is not None and len(tm["tempos"]) >= 1 and len(tm["timeSignatures"]) >= 1 and tm["tempos"][0]["bpm"] > 0
      and tm["timeSignatures"][0]["numerator"] >= 1, "get_tempo_map has a tempo and a time signature")
wave_parts = [p for p in (result("list_parts")[0] or {"parts": []})["parts"] if p.get("type") == "wave"]
if wave_parts:
    ap, _ = result("get_audio_part", {"track": wave_parts[0]["track"], "part_start_tick": wave_parts[0]["startTick"]})
    check(ap is not None and len(ap["events"]) >= 1 and "lengthSeconds" in ap["events"][0], "get_audio_part of a wave part")
    if ap and ap["events"] and ap["events"][0].get("file"):
        e0 = ap["events"][0]
        check(os.path.exists(e0["file"]) and e0["fileSampleRate"] > 0 and e0["channels"] >= 1, "the sound file exists (%s)" % e0["fileName"])
else:
    print("      no wave part in the song, get_audio_part is only tested for errors")

r, _ = result("list_midi_ports")
check(r is not None and isinstance(r["ports"], list) and all("device" in p and "instrument" in p for p in r["ports"]), "list_midi_ports")
r, err = result("list_patches", {"track": "no such track"})
check(r is None and err["isError"], "list_patches with a wrong track is a tool error")

# --- Helpers for the tiers that change things -------------------------------------------------------------
PREFIX = "mcptest%d-" % os.getpid()
scratch = []          # Names of tracks made by this test.
preset_files = []     # Preset files made by this test.


def ok(name, args=None):
    r, err = result(name, args)
    if r is None:
        print("      %s failed: %s" % (name, json.dumps(err)[:200]))
    return r


def fails(name, args=None):
    r, err = result(name, args)
    return r is None and bool(err) and err.get("isError")


def wait_for(fn, timeout=3.0):
    # Control changes are scheduled for the next audio cycle.
    end = time.time() + timeout
    while time.time() < end:
        try:
            if fn():
                return True
        except (TypeError, KeyError):
            pass
        time.sleep(0.05)
    return False


def track_info(name):
    r, _ = result("get_track", {"name": name})
    return r


def new_track(kind, suffix):
    r = ok("add_track", {"type": kind, "name": PREFIX + suffix})
    if r:
        scratch.append(r["name"])
    return r


# Tools of a tier that is off must not exist.
if not has_control:
    check(fails("set_track_mute", {"track": "x", "mute": True}), "Control tool is not available when the tier is off")
if not has_record:
    check(fails("record", {"action": "stop"}), "Record tool is not available when the tier is off")
if not has_edit:
    check(fails("add_track", {"type": "midi"}), "Edit tool is not available when the tier is off")
if not has_danger:
    check(fails("delete_track", {"track": "x"}), "Dangerous tool is not available when the tier is off")

# --- Edit tier ------------------------------------------------------------------------------------------
midi = group_a = group_b = folder = None
part = None
if has_edit:
    n_before = len(result("list_tracks")[0]["tracks"])

    # Undo and redo of a track.
    r = ok("add_track", {"type": "midi", "name": PREFIX + "undo"})
    check(r is not None and r["name"] == PREFIX + "undo" and r["type"] == "midi", "add_track (midi, named)")
    check(fails("add_track", {"type": "midi", "name": PREFIX + "undo"}), "add_track with a used name is refused")
    check(fails("add_track", {"type": "nonsense"}), "add_track with a wrong type is refused")
    def exists(name):
        return any(t["name"] == name for t in result("list_tracks")[0]["tracks"])

    # The track and its rename can be separate undo steps: Undo only as far as needed.
    for _ in range(2):
        if exists(PREFIX + "undo"):
            ok("undo")
    check(not exists(PREFIX + "undo"), "undo removes the added track")
    for _ in range(2):
        if not exists(PREFIX + "undo"):
            ok("redo")
    back = exists(PREFIX + "undo")
    check(back, "redo brings it back")
    if back:
        scratch.append(PREFIX + "undo")

    midi = new_track("midi", "midi")
    group_a = new_track("audio_group", "groupA")
    group_b = new_track("audio_group", "groupB")
    folder = new_track("folder", "folder")
    check(all(x is not None for x in (midi, group_a, group_b, folder)), "add_track makes midi, group and folder tracks")
    if folder:
        check(folder["type"] == "folder", "the folder is a folder track")

    r = ok("rename_track", {"track": PREFIX + "midi", "new_name": PREFIX + "midi2"})
    check(r is not None and r["name"] == PREFIX + "midi2", "rename_track")
    if r:
        scratch[scratch.index(PREFIX + "midi")] = PREFIX + "midi2"
        midi["name"] = PREFIX + "midi2"
    check(fails("rename_track", {"track": PREFIX + "midi2", "new_name": PREFIX + "groupA"}), "rename_track to a used name is refused")

    if midi and folder:
        r = ok("move_tracks_to_folder", {"tracks": [midi["name"]], "folder": folder["name"]})
        check(r is not None and r["moved"] == 1, "move_tracks_to_folder")
        t = track_info(midi["name"])
        check(t is not None and t["folder"] == folder["name"], "the track is in the folder")
        ok("move_tracks_to_folder", {"tracks": [midi["name"]]})
        t = track_info(midi["name"])
        check(t is not None and t["folder"] is None, "the track leaves the folder")
        check(fails("move_tracks_to_folder", {"tracks": [midi["name"]], "folder": midi["name"]}), "a track is not a folder")

    # Parts and notes.
    if midi:
        part = ok("add_midi_part", {"track": midi["name"], "start_bar": 2, "length_ticks": 1920, "name": "mcp"})
        check(part is not None and part["lengthTicks"] == 1920 and part["name"] == "mcp", "add_midi_part")
        check(fails("add_midi_part", {"track": midi["name"]}), "add_midi_part needs a start")
        if part:
            args = {"track": midi["name"], "part_start_tick": part["startTick"]}
            notes = [{"tick": 0, "pitch": 60, "length_ticks": 240, "velocity": 90},
                     {"tick": 240, "pitch": 64, "length_ticks": 240},
                     {"tick": 480, "pitch": 67, "length_ticks": 240, "velocity": 70},
                     {"tick": 480, "pitch": 72, "length_ticks": 480}]
            r = ok("add_midi_notes", dict(args, notes=notes))
            check(r is not None and r["added"] == 4, "add_midi_notes")
            check(fails("add_midi_notes", dict(args, notes=[{"tick": 0, "pitch": 200, "length_ticks": 10}])), "add_midi_notes refuses a wrong pitch")
            r = ok("get_midi_notes", args)
            check(r is not None and r["total"] == 4 and [n["pitch"] for n in r["notes"]].count(64) == 1, "get_midi_notes has the notes")
            if r:
                n64 = [n for n in r["notes"] if n["pitch"] == 64][0]
                check(n64["velocity"] == 100 and n64["length_ticks"] == 240, "default velocity and the length are kept")
            r = ok("get_midi_notes", dict(args, pitch_min=65))
            check(r is not None and r["total"] == 2, "get_midi_notes pitch filter")
            r = ok("get_midi_notes", dict(args, from_tick=240, to_tick=480))
            check(r is not None and r["total"] == 1, "get_midi_notes tick range")

            r = ok("modify_midi_notes", dict(args, changes=[
                {"tick": 240, "pitch": 64, "new_pitch": 65, "new_velocity": 55, "new_tick": 300, "new_length_ticks": 100}]))
            check(r is not None and r["modified"] == 1, "modify_midi_notes")
            r = ok("get_midi_notes", dict(args, from_tick=300, to_tick=301))
            check(r is not None and r["total"] == 1 and r["notes"][0]["pitch"] == 65 and r["notes"][0]["velocity"] == 55
                  and r["notes"][0]["length_ticks"] == 100, "the note was changed")
            check(fails("modify_midi_notes", dict(args, changes=[{"tick": 240, "pitch": 64, "new_pitch": 1}])),
                  "modify_midi_notes of a note that is not there is refused")
            check(fails("modify_midi_notes", dict(args, changes=[{"tick": 0, "pitch": 60, "new_velocity": 0},
                                                                 {"tick": 300, "pitch": 65, "new_pitch": 3}])),
                  "modify_midi_notes with a wrong value is refused")
            r = ok("get_midi_notes", args)
            check(r is not None and r["total"] == 4 and sorted(n["pitch"] for n in r["notes"]) == [60, 65, 67, 72],
                  "a refused modify changes nothing")

            r = ok("delete_midi_notes", dict(args, notes=[{"tick": 0, "pitch": 60}]))
            check(r is not None and r["erased"] == 1, "delete_midi_notes by tick and pitch")
            check(fails("delete_midi_notes", dict(args, notes=[{"tick": 0, "pitch": 60}])), "erasing it again is refused")
            check(fails("delete_midi_notes", args), "delete_midi_notes needs notes or a range")
            check(fails("delete_midi_notes", dict(args, from_tick=1000, to_tick=1100)), "an empty range is refused")
            r = ok("delete_midi_notes", dict(args, from_tick=480, to_tick=481, pitch_min=70))
            check(r is not None and r["erased"] == 1, "delete_midi_notes by range and pitch")
            r = ok("get_midi_notes", args)
            check(r is not None and r["total"] == 2, "two notes are left")
            ok("undo")
            r = ok("get_midi_notes", args)
            check(r is not None and r["total"] == 3, "undo brings back the erased notes")

            r = ok("move_part", dict(args, new_start_tick=part["startTick"] + 3840))
            check(r is not None and r["startTick"] == part["startTick"] + 3840, "move_part")
            r, _ = result("list_parts", {"track": midi["name"]})
            check(r is not None and any(p["startTick"] == part["startTick"] + 3840 for p in r["parts"]), "the part has moved")
            part["startTick"] += 3840
            check(fails("move_part", {"track": midi["name"], "part_start_tick": part["startTick"], "new_start_tick": 0,
                                      "new_track": group_a["name"] if group_a else "x"}), "move_part to another type of track is refused")

    # Routes.
    if group_a and group_b:
        r = ok("add_route", {"from_track": group_a["name"], "to_track": group_b["name"]})
        check(r is not None and r["connected"], "add_route")
        t = track_info(group_a["name"])
        check(t is not None and any(x["name"] == group_b["name"] for x in t["outRoutes"]), "the route is in outRoutes")
        check(fails("add_route", {"from_track": group_a["name"], "to_track": group_b["name"]}), "a second add_route is refused")
        check(fails("add_route", {"from_track": group_b["name"], "to_track": group_a["name"]}), "a route loop is refused")
        r = ok("remove_route", {"from_track": group_a["name"], "to_track": group_b["name"]})
        check(r is not None and not r["connected"], "remove_route")
        t = track_info(group_a["name"])
        check(t is not None and not any(x["name"] == group_b["name"] for x in t["outRoutes"]), "the route is gone")
        check(fails("remove_route", {"from_track": group_a["name"], "to_track": group_b["name"]}), "remove_route of nothing is refused")
        check(fails("add_route", {"from_track": midi["name"] if midi else "x", "to_track": group_b["name"]}), "add_route needs audio tracks")

    # Drum map, tempo and time signature.
    drum = new_track("drum", "drum")
    if drum:
        r = ok("get_drum_map", {"track": drum["name"]})
        check(r is not None and len(r["drums"]) >= 10 and all("pitch" in d and d["name"] for d in r["drums"]),
              "get_drum_map lists the drums (%d)" % (len(r["drums"]) if r else 0))
        if r:
            print("      e.g. %s" % ", ".join("%d %s" % (d["pitch"], d["name"]) for d in r["drums"][:4]))
    if midi:
        check(fails("get_drum_map", {"track": midi["name"]}), "get_drum_map needs a drum track")

    tm0 = result("get_tempo_map")[0]
    r = ok("set_tempo", {"bpm": 97.5})
    check(r is not None and abs(r["bpm"] - 97.5) < 0.05, "set_tempo")
    tm1 = result("get_tempo_map")[0]
    check(abs(tm1["tempos"][0]["bpm"] - 97.5) < 0.1 or abs(tm1["staticTempoBpm"] - 97.5) < 0.1, "the tempo is set")
    check(fails("set_tempo", {"bpm": 5}), "set_tempo refuses a wrong value")
    if not tm0["tempoMapEnabled"]:
        check(fails("set_tempo", {"bpm": 90, "bar": 3}), "set_tempo later in the song needs the tempo map")
    ok("undo")
    tm2 = result("get_tempo_map")[0]
    check(abs(tm2["tempos"][0]["bpm"] - tm0["tempos"][0]["bpm"]) < 0.05 and
          abs(tm2["staticTempoBpm"] - tm0["staticTempoBpm"]) < 0.05, "undo puts the tempo back")

    # Turning the tempo map on or off is not undoable: Put it back explicitly.
    r = ok("set_tempo_map_enabled", {"enabled": not tm0["tempoMapEnabled"]})
    check(r is not None and r["tempoMapEnabled"] == (not tm0["tempoMapEnabled"]), "set_tempo_map_enabled")
    if r and r["tempoMapEnabled"]:
        r = ok("set_tempo", {"bpm": 140, "bar": 5})
        check(r is not None and r["tick"] > 0, "set_tempo at a bar")
        tm3 = result("get_tempo_map")[0]
        check(len(tm3["tempos"]) >= 2, "the tempo map has the tempo change")
        ok("undo")
    ok("set_tempo_map_enabled", {"enabled": tm0["tempoMapEnabled"]})
    tm4 = result("get_tempo_map")[0]
    check(tm4["tempoMapEnabled"] == tm0["tempoMapEnabled"] and len(tm4["tempos"]) == len(tm0["tempos"]),
          "the tempo map is back as it was")

    # A signature that differs from the one in effect: The same one would not be a change.
    cur = tm0["timeSignatures"][-1]
    num, den = (5, 8) if (cur["numerator"], cur["denominator"]) != (5, 8) else (7, 8)
    r = ok("set_time_signature", {"numerator": num, "denominator": den, "bar": 5})
    check(r is not None and r["bar"] == 5, "set_time_signature")
    tm5 = result("get_tempo_map")[0]
    check(any(x["bar"] == 5 and x["numerator"] == num and x["denominator"] == den for x in tm5["timeSignatures"]),
          "the time signature is in the tempo map")
    check(fails("set_time_signature", {"numerator": 4, "denominator": 5}), "set_time_signature refuses a wrong denominator")
    ok("undo")
    tm6 = result("get_tempo_map")[0]
    check(len(tm6["timeSignatures"]) == len(tm0["timeSignatures"]), "undo removes the time signature change")

    # Markers. Every change is undone again, so the song's markers are as they were.
    def markers():
        return result("list_markers")[0]["markers"]

    def marker(mid):
        return next((x for x in markers() if x["id"] == mid), None)

    mk0 = markers()
    mname = PREFIX + "marker"
    r = ok("add_marker", {"name": mname, "bar": 3, "beat": 2})
    check(r is not None and r["name"] == mname and r["bar"] == 3 and r["beat"] == 2 and r["locked"] is False,
          "add_marker by bar and beat")
    mid = r["id"] if r else None
    check(mid is not None and marker(mid) is not None, "list_markers has the marker with its id")
    check(fails("add_marker", {"name": "x"}), "add_marker needs a position")
    r = ok("modify_marker", {"id": mid, "newName": mname + "b"})
    check(r is not None and r["id"] == mid and r["name"] == mname + "b" and r["bar"] == 3, "modify_marker renames")
    r = ok("modify_marker", {"name": mname + "b", "tick": 7680})
    check(r is not None and r["id"] == mid and r["tick"] == 7680, "modify_marker by name moves it")
    r = ok("modify_marker", {"id": mid, "locked": True})
    check(r is not None and r["locked"] is True and r["tick"] == 7680, "modify_marker locks it to its time")
    check(fails("modify_marker", {"id": mid}), "modify_marker needs a change")
    check(fails("modify_marker", {"id": -5, "newName": "x"}), "modify_marker with a wrong id is refused")
    # Two markers with the same name: by name is refused, by id works.
    r2 = ok("add_marker", {"name": mname + "b", "seconds": 1.0, "locked": True})
    check(r2 is not None and r2["locked"] is True and abs(r2["seconds"] - 1.0) < 0.05, "add_marker by seconds, locked")
    check(fails("delete_marker", {"name": mname + "b"}), "delete_marker of an ambiguous name is refused")
    r = ok("delete_marker", {"id": mid})
    check(r is not None and r["deleted"] is True and marker(mid) is None, "delete_marker by id")
    ok("undo")
    check(marker(mid) is not None and marker(mid)["tick"] == 7680, "undo brings the marker back")
    # Undo the rest: the second marker, the lock, the move, the rename and the add (some can be one
    #  undo step), as far as needed.
    for _ in range(6):
        if len(markers()) != len(mk0):
            ok("undo")
    check(len(markers()) == len(mk0), "undo removes the added markers")

    # Midi ports, patches and track output.
    if midi:
        before = track_info(midi["name"])
        r = ok("list_patches", {"track": midi["name"], "limit": 20})
        check(r is not None and r["instrument"] and isinstance(r["patches"], list), "list_patches of a midi track")
        if r and r["patches"]:
            p0 = r["patches"][0]
            check(all(k in p0 for k in ("name", "group", "patch", "program")), "a patch has name, group and number")
            r2 = ok("list_patches", {"track": midi["name"], "filter": p0["name"].split()[-1][:4], "limit": 5})
            check(r2 is not None and len(r2["patches"]) >= 1, "list_patches filter")
            r3 = ok("set_patch", {"track": midi["name"], "patch": p0["patch"]})
            check(r3 is not None and r3["patch"] == p0["patch"], "set_patch")
        check(fails("set_patch", {"track": midi["name"]}), "set_patch needs a patch or a program")
        check(fails("set_patch", {"track": midi["name"], "program": 200}), "set_patch refuses a wrong program")
        check(fails("list_patches", {"track": group_a["name"] if group_a else "x"}), "list_patches needs a midi or synth track")
        r = ok("set_midi_track_output", {"track": midi["name"], "port": before["midiPort"], "channel": 3})
        check(r is not None and r["channel"] == 3, "set_midi_track_output")
        t = track_info(midi["name"])
        check(t is not None and t["midiChannel"] == 3, "the track plays on channel 3")
        ok("set_midi_track_output", {"track": midi["name"], "port": before["midiPort"], "channel": before["midiChannel"]})
        check(fails("set_midi_track_output", {"track": midi["name"], "port": 9999}), "set_midi_track_output refuses a wrong port")
        check(fails("set_midi_track_output", {"track": group_a["name"] if group_a else "x", "port": 1}), "set_midi_track_output needs a midi track")

# --- Control tier ---------------------------------------------------------------------------------------
if has_control:
    # On a scratch track if there is one, otherwise on a real track whose values are put back.
    target = group_a["name"] if group_a else (audio[0]["name"] if audio else None)
    if target:
        orig = track_info(target)
        r = ok("set_track_mute", {"track": target, "mute": True})
        check(r is not None and r["mute"] is True, "set_track_mute")
        check(wait_for(lambda: track_info(target)["mute"] is True), "the track is muted")
        ok("set_track_mute", {"track": target, "mute": orig["mute"]})
        r = ok("set_track_solo", {"track": target, "solo": True})
        check(r is not None and r["solo"] is True, "set_track_solo")
        ok("set_track_solo", {"track": target, "solo": orig["solo"]})
        check(wait_for(lambda: track_info(target)["solo"] == orig["solo"]), "solo is put back")

        r = ok("set_track_volume", {"track": target, "volume_db": -6.0})
        check(r is not None and abs(r["volumeDb"] + 6.0) < 0.05, "set_track_volume answers with the value")
        check(wait_for(lambda: abs(track_info(target)["volumeDb"] + 6.0) < 0.1), "the volume is set")
        r = ok("set_track_volume", {"track": target, "volume_db": 50.0})
        check(r is not None and r["volumeDb"] < 10.5, "set_track_volume is limited to about +10 dB")
        ok("set_track_volume", {"track": target, "volume_db": orig["volumeDb"]})
        check(fails("set_track_volume", {"track": target}), "set_track_volume needs a value")

        r = ok("set_track_pan", {"track": target, "pan": -0.5})
        check(r is not None and r["pan"] == -0.5, "set_track_pan")
        check(wait_for(lambda: abs(track_info(target)["pan"] + 0.5) < 0.01), "the pan is set")
        r = ok("set_track_pan", {"track": target, "pan": 7})
        check(r is not None and r["pan"] == 1.0, "set_track_pan is limited to 1")
        ok("set_track_pan", {"track": target, "pan": orig["pan"]})
        check(wait_for(lambda: abs(track_info(target)["pan"] - orig["pan"]) < 0.01), "the pan is put back")
    check(fails("set_track_mute", {"track": "no such track", "mute": True}), "set_track_mute with a wrong track is refused")
    if midi:
        check(fails("set_track_volume", {"track": midi["name"], "volume_db": 0}), "set_track_volume needs an audio track")
        r = ok("set_track_mute", {"track": midi["name"], "mute": True})
        check(r is not None and r["mute"] is True, "a midi track can be muted")
        ok("set_track_mute", {"track": midi["name"], "mute": False})

    # Transport and position. Not while something is playing: Do not disturb.
    tp = result("get_transport")[0]
    if tp and not tp["playing"]:
        cursor = tp["cursor"]["tick"]
        loop = tp["loop"]
        r = ok("set_position", {"tick": 1920})
        check(r is not None and r["cursor"]["tick"] == 1920, "set_position by tick")
        check(wait_for(lambda: result("get_transport")[0]["cursor"]["tick"] == 1920), "the cursor is at the tick")
        r = ok("set_position", {"bar": 3, "beat": 2})
        check(r is not None and r["cursor"]["bar"] == 3 and r["cursor"]["beat"] == 2, "set_position by bar and beat")
        r = ok("set_position", {"seconds": 1.0})
        check(r is not None and abs(r["cursor"]["seconds"] - 1.0) < 0.05, "set_position by seconds")
        check(fails("set_position", {}), "set_position needs a position")
        ok("set_loop", {"on": not loop})
        check(wait_for(lambda: result("get_transport")[0]["loop"] == (not loop)), "set_loop")
        ok("set_loop", {"on": loop})

        # Locators (range markers), put back afterwards.
        lt0, rt0 = tp["leftLocator"]["tick"], tp["rightLocator"]["tick"]
        r = ok("set_locators", {"left": {"bar": 2}, "right": {"bar": 4}})
        check(r is not None and r["leftLocator"]["bar"] == 2 and r["rightLocator"]["bar"] == 4, "set_locators by bar")
        tp2 = result("get_transport")[0]
        check(tp2["leftLocator"]["bar"] == 2 and tp2["rightLocator"]["bar"] == 4, "get_transport has the locators")
        # Both moved past the old right locator: Must not be swapped on the way.
        r = ok("set_locators", {"left": {"bar": 9}, "right": {"bar": 12}})
        check(r is not None and r["leftLocator"]["bar"] == 9 and r["rightLocator"]["bar"] == 12,
              "set_locators past the old right locator")
        r = ok("set_locators", {"left": {"bar": 6}, "right": {"bar": 5}})
        check(r is not None and r["leftLocator"]["bar"] == 5 and r["rightLocator"]["bar"] == 6,
              "set_locators puts left and right in order")
        r = ok("set_locators", {"right": {"tick": r["rightLocator"]["tick"] + 960}})
        check(r is not None and r["leftLocator"]["bar"] == 5 and r["rightLocator"]["tick"] > r["leftLocator"]["tick"],
              "set_locators with only the right one")
        check(fails("set_locators", {}), "set_locators needs a locator")
        check(fails("set_locators", {"left": {}}), "set_locators needs a position")
        ok("set_locators", {"left": {"tick": lt0}, "right": {"tick": rt0}})
        tp3 = result("get_transport")[0]
        check(tp3["leftLocator"]["tick"] == lt0 and tp3["rightLocator"]["tick"] == rt0, "the locators are put back")

        r = ok("set_click", {"on": True})
        check(r is not None and r["click"] is True, "set_click")
        ok("set_click", {"on": False})
        check(fails("transport", {"action": "rewind"}), "transport with a wrong action is refused")
        ok("transport", {"action": "play"})
        check(wait_for(lambda: result("get_transport")[0]["playing"] is True), "transport play")
        ok("transport", {"action": "stop"})
        check(wait_for(lambda: result("get_transport")[0]["playing"] is False), "transport stop")
        ok("set_position", {"tick": cursor})
    else:
        print("      transport is playing, the transport tests are skipped")

# --- Plugins (Edit, Control and Dangerous) ------------------------------------------------------------------
if has_edit and group_a:
    effects = [p for p in plugins if p["audioInputs"] >= 1 and p["audioOutputs"] >= 1 and p["controls"] >= 1]
    if not effects:
        r, _ = result("list_plugins", {"type": "ladspa", "limit": 50})
        effects = [p for p in (r["plugins"] if r else []) if p["audioInputs"] >= 1 and p["audioOutputs"] >= 1 and p["controls"] >= 1]
    if effects:
        p = effects[0]
        spec = {"track": group_a["name"]}
        spec.update({"uri": p["uri"]} if p["uri"] else {"file": p["file"], "label": p["label"], "type": p["type"]})
        r = ok("add_plugin", spec)
        check(r is not None and r["plugin"]["slot"] == 1 and r["plugin"]["parameters"], "add_plugin (%s)" % p["name"])
        if r:
            par = r["plugin"]["parameters"][0]
            check(all(k in par for k in ("min", "max", "default")), "plugin parameters have min, max and default")
            check(fails("add_plugin", spec | {"slot": 1}), "add_plugin to a used slot is refused")
            r2 = ok("add_plugin", spec | {"slot": 1, "replace": True})
            check(r2 is not None and r2["plugin"]["slot"] == 1, "add_plugin with replace")
            check(fails("add_plugin", {"track": group_a["name"], "label": "no-such-plugin-label"}), "add_plugin with an unknown plugin is refused")
            check(fails("add_plugin", {"track": midi["name"] if midi else "x", "label": p["label"]}), "add_plugin needs an audio track")

            if has_control:
                mid = (par["min"] + par["max"]) / 2.0
                r3 = ok("set_plugin_param", {"track": group_a["name"], "slot": 1, "parameter": par["index"], "value": mid})
                check(r3 is not None, "set_plugin_param")
                r3 = ok("set_plugin_param", {"track": group_a["name"], "slot": 1, "parameter": par["index"], "value": par["max"] + 1000})
                check(r3 is not None and r3["value"] <= par["max"] + 0.001, "set_plugin_param clamps to the range")
                check(fails("set_plugin_param", {"track": group_a["name"], "slot": 1, "parameter": 9999, "value": 0}),
                      "set_plugin_param with a wrong parameter is refused")
                check(fails("set_plugin_param", {"track": group_a["name"], "slot": 9, "parameter": 0, "value": 0}),
                      "set_plugin_param with an empty slot is refused")
                r3 = ok("set_plugin_on", {"track": group_a["name"], "slot": 1, "on": False})
                check(r3 is not None and r3["on"] is False, "set_plugin_on (bypass)")
                ok("set_plugin_on", {"track": group_a["name"], "slot": 1, "on": True})

            # Presets.
            name = "mcptest%d" % os.getpid()
            r3 = ok("save_plugin_preset", {"track": group_a["name"], "slot": 1, "name": name})
            check(r3 is not None and r3["id"].endswith("/" + name), "save_plugin_preset")
            if r3:
                preset_files.append(r3["file"])
                check(os.path.exists(r3["file"]), "the preset file exists")
                check(fails("save_plugin_preset", {"track": group_a["name"], "slot": 1, "name": name}), "saving over a preset needs overwrite")
                check(ok("save_plugin_preset", {"track": group_a["name"], "slot": 1, "name": name, "overwrite": True}) is not None,
                      "save_plugin_preset with overwrite")
                check(fails("save_plugin_preset", {"track": group_a["name"], "slot": 1, "name": "../evil"}), "a preset name with a path is refused")
                r4 = ok("list_plugin_presets", {"track": group_a["name"], "slot": 1})
                check(r4 is not None and any(x["id"] == r3["id"] for x in r4["presets"]), "list_plugin_presets finds it for the plugin")
                r4 = ok("list_plugin_presets", {"filter": name})
                check(r4 is not None and any(x["id"] == r3["id"] for x in r4["presets"]), "list_plugin_presets filter")
                if has_danger:
                    ok("remove_plugin", {"track": group_a["name"], "slot": 1})
                r4 = ok("load_plugin_preset", {"track": group_a["name"], "slot": 2, "preset": r3["id"]})
                check(r4 is not None and r4["plugin"]["slot"] == 2, "load_plugin_preset puts the plugin in a slot")
                check(fails("load_plugin_preset", {"track": group_a["name"], "slot": 2, "preset": "user:../../etc/passwd"}),
                      "load_plugin_preset refuses a path")
                check(fails("load_plugin_preset", {"track": group_a["name"], "slot": 2, "preset": "user:Nothing/nothing"}),
                      "load_plugin_preset of a missing preset is refused")
            if has_danger:
                r3 = ok("remove_plugin", {"track": group_a["name"], "slot": 2})
                check(r3 is not None, "remove_plugin")
                t = track_info(group_a["name"])
                check(t is not None and not any(e["slot"] == 2 for e in t["effects"]), "the rack has no plugin in slot 2 now")
                check(fails("remove_plugin", {"track": group_a["name"], "slot": 2}), "remove_plugin of an empty slot is refused")
                ok("undo")
                t = track_info(group_a["name"])
                check(t is not None and any(e["slot"] == 2 for e in t["effects"]), "undo brings the plugin back")
    else:
        print("      no suitable effect plugin found, the plugin tests are skipped")

    if synths:
        mess = [x for x in synths if x["type"] == "mess"]
        sy = (mess or synths)[0]
        spec = {"name": PREFIX + "synth"}
        spec.update({"uri": sy["uri"]} if sy["uri"] else {"file": sy["file"], "label": sy["label"], "type": sy["type"]})
        r = ok("add_synth_track", spec)
        check(r is not None and r["type"] == "synth" and r["name"] == PREFIX + "synth", "add_synth_track (%s)" % sy["name"])
        if r:
            scratch.append(r["name"])
            t = track_info(r["name"])
            check(t is not None and t.get("midiPort", 0) >= 1, "a synth track shows its midi port")
            ports = ok("list_midi_ports")
            check(ports is not None and any(p["device"] == r["name"] and p["isSynth"] for p in ports["ports"]), "list_midi_ports lists the synth")
            pr = ok("list_patches", {"track": r["name"], "limit": 5})
            check(pr is not None and isinstance(pr["patches"], list), "list_patches of a synth track (%d)" % (len(pr["patches"]) if pr else 0))
    check(fails("add_synth_track", {"label": "no-such-synth-label"}), "add_synth_track with an unknown synth is refused")

# --- Record tier ----------------------------------------------------------------------------------------
if has_record:
    armed_before = [t["name"] for t in result("list_tracks")[0]["tracks"] if t.get("recordArmed")]
    if midi:
        r = ok("set_track_record_arm", {"track": midi["name"], "arm": True})
        check(r is not None and r["recordArmed"] is True, "set_track_record_arm")
        check(wait_for(lambda: track_info(midi["name"])["recordArmed"] is True), "the track is armed")
        ok("set_track_record_arm", {"track": midi["name"], "arm": False})
        check(wait_for(lambda: track_info(midi["name"])["recordArmed"] is False), "the track is disarmed")
    if group_a:
        check(fails("set_track_record_arm", {"track": group_a["name"], "arm": True}), "an audio group can not be armed")
    check(fails("record", {"action": "pause"}), "record with a wrong action is refused")
    # Starting is only tried when nothing is armed, so that it can not record: It must be refused.
    if not armed_before:
        check(fails("record", {"action": "start"}), "record start with no armed track is refused")
    else:
        print("      tracks are armed (%s), record start is not tried" % ", ".join(armed_before))

# --- Dangerous tier and clean up ----------------------------------------------------------------------------
if has_danger:
    if midi and part:
        args = {"track": midi["name"], "part_start_tick": part["startTick"]}
        r = ok("delete_part", args)
        check(r is not None and r["deleted"] == "mcp", "delete_part")
        check(fails("delete_part", args), "delete_part of a part that is gone is refused")
    check(fails("delete_track", {"track": "no such track"}), "delete_track with a wrong track is refused")
    if midi and folder:
        ok("move_tracks_to_folder", {"tracks": [midi["name"]], "folder": folder["name"]})
        check(fails("delete_track", {"track": folder["name"]}), "a folder with tracks is not deleted")
        ok("move_tracks_to_folder", {"tracks": [midi["name"]]})
    for name in list(scratch):
        r = ok("delete_track", {"track": name})
        if r is not None:
            scratch.remove(name)
    check(not scratch, "the scratch tracks are deleted")
    tracks_now = [t["name"] for t in result("list_tracks")[0]["tracks"]]
    check(not any(n.startswith(PREFIX) for n in tracks_now), "no scratch track is left")
elif scratch:
    print("      The Dangerous tier is off, so these scratch tracks are left: %s" % ", ".join(scratch))

for f in preset_files:
    try:
        os.remove(f)
    except OSError:
        pass

print()
print("%d failure(s)" % failures)
sys.exit(1 if failures else 0)
