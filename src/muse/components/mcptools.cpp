//=========================================================
//  MusE
//  Linux Music Editor
//
//  mcptools.cpp
//  The tools of the MCP server. See mcpserver.h.
//  The Read tier tools look at the song and change nothing. The Control tier tools make reversible changes
//  (transport, mute, solo, volume, pan, plugin parameters). Record arms tracks and records, Edit adds and
//  changes tracks, parts, notes, plugins and routes (undoable except routes), Dangerous deletes and saves.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "mcpserver.h"

#include <QAction>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QJsonArray>
#include <QTimer>

#include <cmath>
#include <memory>
#include <vector>

#include "audio.h"
#include "app.h"
#include "audiodev.h"
#include "event.h"
#include "gconfig.h"
#include "globals.h"
#include "marker/marker.h"
#include "markermixdown.h"
#include "stemexport.h"
#include "midi_consts.h"
#include "midictrl.h"
#include "midievent.h"
#include "midiport.h"
#include "popupmenu.h"
#include "midiedit/drummap.h"
#include "synth.h"
#include "instruments/minstrument.h"
#include "part.h"
#include "plugin.h"
#include "route.h"
#include "sig.h"
#include "song.h"
#include "tempo.h"
#include "undo.h"
#include "xml.h"
#include "track.h"

namespace MusEGui {

namespace {

using namespace MusECore;

//---------------------------------------------------------
//   Helpers
//---------------------------------------------------------

// Linear gain to dB. JSON can not hold minus infinity, so silence is -120.
double toDb(double lin)
{
  if(!(lin > 1e-6))
    return -120.0;
  return qMax(-120.0, 20.0 * std::log10(lin));
}

// Rounds, to keep the answers short.
double round2(double v) { return std::round(v * 100.0) / 100.0; }
double round4(double v) { return std::round(v * 10000.0) / 10000.0; }

QJsonObject prop(const char* type, const QString& description)
{
  QJsonObject o;
  o["type"] = type;
  o["description"] = description;
  return o;
}

QJsonObject trackListProp(const QString& description)
{
  QJsonObject items;
  items["type"] = "string";
  QJsonObject o;
  o["type"] = "array";
  o["items"] = items;
  o["description"] = description;
  return o;
}

QJsonObject schema(const QJsonObject& props = QJsonObject(), const QStringList& required = QStringList())
{
  QJsonObject o;
  o["type"] = "object";
  o["properties"] = props;
  if(!required.isEmpty())
  {
    QJsonArray r;
    for(const QString& s : required)
      r.push_back(s);
    o["required"] = r;
  }
  return o;
}

const char* trackTypeName(Track::TrackType t)
{
  switch(t)
  {
    case Track::MIDI:            return "midi";
    case Track::DRUM:            return "drum";
    case Track::WAVE:            return "wave";
    case Track::AUDIO_OUTPUT:    return "audio_output";
    case Track::AUDIO_INPUT:     return "audio_input";
    case Track::AUDIO_GROUP:     return "audio_group";
    case Track::AUDIO_AUX:       return "audio_aux";
    case Track::AUDIO_SOFTSYNTH: return "synth";
    case Track::FOLDER:          return "folder";
    default:                     return "unknown";
  }
}

double sampleRate() { return MusEGlobal::sampleRate > 0 ? double(MusEGlobal::sampleRate) : 48000.0; }

double tickToSeconds(unsigned tick)
{
  return double(MusEGlobal::tempomap.tick2frame(tick)) / sampleRate();
}

QJsonObject timeSigJson(unsigned tick)
{
  int z = 4, n = 4;
  MusEGlobal::sigmap.timesig(tick, z, n);
  QJsonObject o;
  o["numerator"] = z;
  o["denominator"] = n;
  return o;
}

double tempoBpm(unsigned tick)
{
  const int t = MusEGlobal::tempomap.tempo(tick);
  return t > 0 ? 60000000.0 / double(t) : 0.0;
}

// A position in ticks, and the same in other units.
QJsonObject positionJson(unsigned tick)
{
  QJsonObject o;
  o["tick"] = double(tick);
  const unsigned frame = MusEGlobal::tempomap.tick2frame(tick);
  o["frame"] = double(frame);
  o["seconds"] = round4(double(frame) / sampleRate());
  int bar = 0, beat = 0;
  unsigned t = 0;
  MusEGlobal::sigmap.tickValues(tick, &bar, &beat, &t);
  o["bar"] = bar + 1;
  o["beat"] = beat + 1;
  o["tickInBeat"] = double(t);
  return o;
}

QJsonObject routeJson(const Route& r)
{
  QJsonObject o;
  switch(r.type)
  {
    case Route::TRACK_ROUTE:       o["type"] = "track"; break;
    case Route::JACK_ROUTE:        o["type"] = "audio_port"; break;
    case Route::MIDI_DEVICE_ROUTE: o["type"] = "midi_device"; break;
    case Route::MIDI_PORT_ROUTE:   o["type"] = "midi_port"; break;
    default:                       o["type"] = "unknown"; break;
  }
  o["name"] = r.type == Route::TRACK_ROUTE && r.track ? r.track->name() : r.name();
  o["channel"] = r.channel;
  o["channels"] = r.channels;
  o["remoteChannel"] = r.remoteChannel;
  return o;
}

QJsonObject pluginJson(const PluginI* p, int slot)
{
  QJsonObject o;
  o["slot"] = slot + 1;
  o["name"] = p->name();
  o["label"] = p->plugin() ? p->plugin()->label() : QString();
  o["on"] = p->on();
  QJsonArray params;
  for(unsigned long i = 0; i < p->parameters(); ++i)
  {
    QJsonObject po;
    po["index"] = double(i);
    po["name"] = QString::fromLocal8Bit(p->paramName(i));
    po["value"] = round4(p->param(i));
    float mn = 0.0f, mx = 0.0f;
    p->range(i, &mn, &mx);
    po["min"] = round4(mn);
    po["max"] = round4(mx);
    po["default"] = round4(p->defaultValue(i));
    params.push_back(po);
  }
  o["parameters"] = params;
  return o;
}

QJsonObject partJson(const Part* part)
{
  QJsonObject o;
  o["name"] = part->name();
  o["track"] = part->track() ? part->track()->name() : QString();
  o["startTick"] = double(part->tick());
  o["lengthTicks"] = double(part->lenTick());
  o["startSeconds"] = round4(tickToSeconds(part->tick()));
  o["lengthSeconds"] = round4(tickToSeconds(part->tick() + part->lenTick()) - tickToSeconds(part->tick()));
  o["mute"] = part->mute();
  o["selected"] = part->selected();
  o["isClone"] = part->hasClones();
  if(part->track() && part->track()->type() == Track::WAVE)
  {
    o["type"] = "wave";
    o["volume"] = round4(part->volume());
    o["volumeDb"] = round2(toDb(part->volume()));
    o["fadeInFrames"] = double(part->fadeInFrames());
    o["fadeOutFrames"] = double(part->fadeOutFrames());
  }
  else
  {
    o["type"] = "midi";
    o["eventCount"] = double(part->events().size());
  }
  return o;
}

QJsonObject trackJson(const Track* t, bool detail)
{
  QJsonObject o;
  o["name"] = t->name();
  o["type"] = trackTypeName(t->type());
  o["selected"] = t->selected();
  o["mute"] = t->mute();
  o["solo"] = t->solo();
  o["off"] = t->off();
  o["channels"] = t->channels();
  o["folder"] = t->folder() ? QJsonValue(t->folder()->name()) : QJsonValue();
  o["recordArmed"] = t->recordFlag();
  if(!t->comment().isEmpty())
    o["comment"] = t->comment();
  o["partCount"] = t->isFolderTrack() ? 0 : int(t->cparts()->size());

  if(t->isMidiTrack())
  {
    const MidiTrack* mt = static_cast<const MidiTrack*>(t);
    o["midiPort"] = mt->outPort() + 1;
    o["midiChannel"] = mt->outChannel() + 1;
    if(detail && mt->outPort() >= 0 && mt->outPort() < MusECore::MIDI_PORTS)
    {
      const MidiInstrument* mi = MusEGlobal::midiPorts[mt->outPort()].instrument();
      if(mi)
        o["instrument"] = mi->iname();
    }
  }
  else if(t->isAudioTrack())
  {
    const AudioTrack* at = static_cast<const AudioTrack*>(t);
    o["volume"] = round4(at->volume());
    o["volumeDb"] = round2(toDb(at->volume()));
    o["pan"] = round4(at->pan());
    if(t->type() == Track::AUDIO_SOFTSYNTH)
    {
      // The midi port that sends to the synth. Midi tracks that play it have this port as their output.
      for(int i = 0; i < MIDI_PORTS; ++i)
        if(MusEGlobal::midiPorts[i].device() == static_cast<const MidiDevice*>(static_cast<const SynthI*>(t)))
          o["midiPort"] = i + 1;
    }
    int plugins = 0;
    for(int i = 0; i < PipelineDepth; ++i)
      if((*at->efxPipe())[i])
        ++plugins;
    o["pluginCount"] = plugins;

    if(detail)
    {
      o["gain"] = round4(at->gain());
      o["gainDb"] = round2(toDb(at->gain()));
      o["prefader"] = at->prefader();
      o["frozen"] = at->frozen();
      o["monitorInput"] = at->recMonitor();

      if(at->hasAuxSend())
      {
        QJsonArray sends;
        const AuxList* auxs = MusEGlobal::song->auxs();
        for(unsigned i = 0; i < auxs->size(); ++i)
        {
          QJsonObject s;
          s["aux"] = (*auxs)[i]->name();
          s["value"] = round4(at->auxSend(i));
          s["db"] = round2(toDb(at->auxSend(i)));
          sends.push_back(s);
        }
        o["auxSends"] = sends;
      }

      QJsonArray fx;
      for(int i = 0; i < PipelineDepth; ++i)
      {
        const PluginI* p = (*at->efxPipe())[i];
        if(p)
          fx.push_back(pluginJson(p, i));
      }
      o["effects"] = fx;

      QJsonArray in, out;
      for(ciRoute r = at->inRoutes()->begin(); r != at->inRoutes()->end(); ++r)
        in.push_back(routeJson(*r));
      for(ciRoute r = at->outRoutes()->begin(); r != at->outRoutes()->end(); ++r)
        out.push_back(routeJson(*r));
      o["inRoutes"] = in;
      o["outRoutes"] = out;
    }
  }
  return o;
}


// The track named in args["track"], or null with error set. If audioOnly it must be an audio track.
Track* trackFromArgs(const QJsonObject& args, bool audioOnly, QString& error)
{
  const QString name = args.value("track").toString();
  Track* t = name.isEmpty() ? nullptr : MusEGlobal::song->findTrack(name);
  if(!t)
  {
    error = QObject::tr("There is no track named \"%1\".").arg(name);
    return nullptr;
  }
  if(t->isFolderTrack())
  {
    error = QObject::tr("\"%1\" is a folder.").arg(name);
    return nullptr;
  }
  if(audioOnly && !t->isAudioTrack())
  {
    error = QObject::tr("\"%1\" is not an audio track.").arg(name);
    return nullptr;
  }
  return t;
}

// Adds a tool of the Control tier: It changes things, but reversibly.
McpTool controlTool(const char* name, const QString& description, const QJsonObject& inputSchema,
                    unsigned tier = McpTierControl)
{
  McpTool t;
  t.name = name;
  t.description = description;
  t.inputSchema = inputSchema;
  t.tier = tier;
  t.readOnly = false;
  return t;
}

// The properties of a position: One of tick, seconds, or bar with an optional beat.
QJsonObject positionProps(const QString& what)
{
  QJsonObject p;
  p["tick"] = prop("integer", QObject::tr("%1 in ticks.").arg(what));
  p["seconds"] = prop("number", QObject::tr("Or %1 in seconds.").arg(what));
  p["bar"] = prop("integer", QObject::tr("Or %1 as a bar number, the first bar is 1.").arg(what));
  p["beat"] = prop("integer", QObject::tr("Beat in the bar, the first beat is 1. 1 if left out."));
  return p;
}

bool hasPosition(const QJsonObject& args)
{
  return args.contains("tick") || args.contains("seconds") || args.contains("bar");
}

// The position given by tick, seconds, or bar (and beat) in args. False with error set if there is none.
bool tickFromPosition(const QJsonObject& args, unsigned& tick, QString& error)
{
  if(args.contains("tick"))
    tick = unsigned(qMax(0, args.value("tick").toInt()));
  else if(args.contains("seconds"))
  {
    const double sec = qMax(0.0, args.value("seconds").toDouble());
    tick = MusEGlobal::tempomap.frame2tick(unsigned(sec * sampleRate()));
  }
  else if(args.contains("bar"))
    tick = MusEGlobal::sigmap.bar2tick(qMax(1, args.value("bar").toInt()) - 1,
                                       qMax(1, args.value("beat").toInt(1)) - 1, 0);
  else
  {
    error = QObject::tr("Give tick, seconds or bar.");
    return false;
  }
  return true;
}

QJsonObject markerJson(const Marker& m)
{
  QJsonObject o = positionJson(m.tick());
  o["id"] = double(m.id());
  o["name"] = m.name();
  o["locked"] = m.type() == Pos::FRAMES;
  return o;
}

// The marker given by args["id"], or by args["name"] if that is unique. Null with error set if there is none.
const Marker* markerFromArgs(const QJsonObject& args, QString& error)
{
  const MarkerList* ml = MusEGlobal::song->marker();
  if(args.contains("id"))
  {
    const EventID_t id = EventID_t(args.value("id").toDouble(-1));
    for(ciMarker i = ml->cbegin(); i != ml->cend(); ++i)
      if(i->second.id() == id)
        return &i->second;
    error = QObject::tr("There is no marker with the id %1 (list_markers gives the ids).").arg(double(id));
    return nullptr;
  }
  if(!args.contains("name"))
  {
    error = QObject::tr("Give the id or the name of the marker.");
    return nullptr;
  }
  const QString name = args.value("name").toString();
  const Marker* found = nullptr;
  int count = 0;
  for(ciMarker i = ml->cbegin(); i != ml->cend(); ++i)
    if(i->second.name() == name)
    {
      found = &i->second;
      ++count;
    }
  if(count == 0)
    error = QObject::tr("There is no marker named \"%1\".").arg(name);
  else if(count > 1)
    error = QObject::tr("There are %1 markers named \"%2\": Give the id instead (list_markers gives the ids).").arg(count).arg(name);
  return count == 1 ? found : nullptr;
}



//---------------------------------------------------------
//   Exports (downmix and stems)
//---------------------------------------------------------

// The export started through MCP. Only one runs at a time, like in the GUI.
struct McpExport {
  QPointer<QObject> job;                  // The MarkerMixdown or StemExport while it runs.
  std::function<ExportReport()> report;   // Reads the running job.
  std::function<void()> cancel;
  QString kind;                           // "mixdown" or "stems".
  QString path;                           // The file or folder.
  int id = 0;
  bool hasLast = false;
  ExportReport last;                      // Of the last finished one.
};
McpExport mcpExport;

QJsonObject exportReportJson(const ExportReport& r, bool running)
{
  QJsonObject o;
  o["id"] = mcpExport.id;
  o["kind"] = mcpExport.kind;
  o["path"] = mcpExport.path;
  o["running"] = running;
  o["finished"] = r.finished;
  o["cancelled"] = r.cancelled;
  o["job"] = r.job;
  o["jobCount"] = r.jobCount;
  if(running)
  {
    o["percent"] = r.percent;
    o["current"] = r.current;
  }
  QJsonArray created, failed;
  for(const QString& f : r.created)
    created.push_back(f);
  for(const QString& f : r.failed)
    failed.push_back(f);
  o["created"] = created;
  o["failed"] = failed;
  return o;
}

// Remembers a started export: the result stays readable after the job has deleted itself.
template <class Job> void trackExport(Job* job, const QString& kind, const QString& path)
{
  mcpExport.job = job;
  mcpExport.report = [job]() { return job->report(); };
  mcpExport.cancel = [job]() { job->cancel(); };
  mcpExport.kind = kind;
  mcpExport.path = path;
  ++mcpExport.id;
  mcpExport.hasLast = false;
  QObject::connect(job, &Job::finished, [](const ExportReport& r) {
    mcpExport.last = r;
    mcpExport.hasLast = true;
  });
}

// The project folder, canonical. Empty with error set if the song has not been saved (it has no folder).
QString projectFolder(QString& error)
{
  if(MusEGlobal::museProject == MusEGlobal::museProjectInitPath)
  {
    error = QObject::tr("The song has no project folder yet. Save it once in MusE: exports are written inside its folder.");
    return QString();
  }
  const QString c = QFileInfo(MusEGlobal::museProject).canonicalFilePath();
  if(c.isEmpty())
    error = QObject::tr("The project folder %1 does not exist.").arg(MusEGlobal::museProject);
  return c;
}

// The absolute path of 'path' (relative to the project folder, or absolute), if it is inside the project folder.
//  Empty with error set otherwise. Links are followed as far as the path exists, so a link can not lead out.
QString pathInProject(const QString& path, QString& error)
{
  const QString project = projectFolder(error);
  if(project.isEmpty())
    return QString();
  const QString abs = QDir::cleanPath(QDir(project).absoluteFilePath(path));
  // The deepest part of the path that exists, resolved.
  QString existing = abs, rest;
  while(!QFileInfo::exists(existing))
  {
    const int slash = existing.lastIndexOf('/');
    if(slash <= 0)
      break;
    rest = existing.mid(slash) + rest;
    existing = existing.left(slash);
  }
  const QString resolved = QFileInfo(existing).canonicalFilePath() + rest;
  if(resolved != project && !resolved.startsWith(project + "/"))
  {
    error = QObject::tr("%1 is not inside the project folder %2. Exports can only be written there.").arg(path, project);
    return QString();
  }
  return resolved;
}

// The properties for the file format, shared by the export tools.
void addFormatProps(QJsonObject& props)
{
  QJsonArray names;
  for(const MixdownFormat& f : availableMixdownFormats())
    names.push_back(QString(f.name));
  QJsonObject format = prop("string", QObject::tr("File format, wav16 if left out. ogg and mp3 are only listed if "
                                                  "MusE's libsndfile supports them."));
  format["enum"] = names;
  props["format"] = format;
  props["channels"] = prop("integer", QObject::tr("1 (mono) or 2 (stereo, the default)."));
  props["quality"] = prop("number", QObject::tr("For ogg and mp3: 0.0 (smallest) to 1.0 (best). 0.4 if left out."));
  props["mode"] = prop("string", QObject::tr("\"range\" (the default): between the left and right locators (the range "
                                             "markers). \"markers\": one for each named marker, from it to the next "
                                             "marker (the last one to the end of the song)."));
  props["overwrite"] = prop("boolean", QObject::tr("true to replace files that exist. Otherwise the export is refused "
                                                   "if one would be replaced."));
}

// The settings from format, channels, quality and mode. False with error set if they are not valid.
bool mixdownSettingsFromArgs(const QJsonObject& args, MixdownSettings& s, QString& error)
{
  const QString name = args.value("format").toString("wav16");
  bool found = false;
  for(const MixdownFormat& f : availableMixdownFormats())
    if(name == f.name)
    {
      s.format = f.format;
      found = true;
    }
  if(!found)
  {
    error = QObject::tr("The format %1 is not available.").arg(name);
    return false;
  }
  s.channels = args.value("channels").toInt(2);
  if(s.channels != 1 && s.channels != 2)
  {
    error = QObject::tr("channels must be 1 or 2.");
    return false;
  }
  if(s.extension() == "wav")
    s.quality = -1.0;
  else
  {
    s.quality = args.value("quality").toDouble(0.4);
    if(!(s.quality >= 0.0 && s.quality <= 1.0))
    {
      error = QObject::tr("quality must be 0.0 to 1.0.");
      return false;
    }
  }
  const QString mode = args.value("mode").toString("range");
  if(mode != "range" && mode != "markers")
  {
    error = QObject::tr("mode must be \"range\" or \"markers\".");
    return false;
  }
  s.markerMode = mode == "markers";
  return true;
}

// The name used in the file names, as in the GUI.
QString exportProjectName()
{
  const QString n = MusEGlobal::muse->projectFileInfo().completeBaseName();
  return n.isEmpty() ? QString("untitled") : n;
}

// Why an export can not start, also if one started through MCP is still running.
QString exportBusy()
{
  if(mcpExport.job)
    return QObject::tr("An export is running (see get_export_status).");
  return exportBlockedReason();
}

// The output track named in args[key], or the only one if not given. Null with error set.
AudioOutput* outputFromArgs(const QJsonObject& args, const char* key, QString& error)
{
  const OutputList* ol = MusEGlobal::song->outputs();
  if(!args.contains(key))
  {
    if(ol->size() == 1)
      return ol->front();
    error = ol->empty() ? QObject::tr("There is no audio output track.")
                        : QObject::tr("There are %1 audio output tracks: Give %2.").arg(ol->size()).arg(key);
    return nullptr;
  }
  const QString name = args.value(key).toString();
  for(AudioOutput* ao : *ol)
    if(ao->name() == name)
      return ao;
  error = QObject::tr("There is no audio output track named \"%1\".").arg(name);
  return nullptr;
}


const char* pluginTypeName(MusEPlugin::PluginType t)
{
  switch(t)
  {
    case MusEPlugin::PluginTypeLADSPA:   return "ladspa";
    case MusEPlugin::PluginTypeDSSI:     return "dssi";
    case MusEPlugin::PluginTypeVST:      return "vst";
    case MusEPlugin::PluginTypeDSSIVST:  return "dssi_vst";
    case MusEPlugin::PluginTypeLinuxVST: return "linux_vst";
    case MusEPlugin::PluginTypeLV2:      return "lv2";
    case MusEPlugin::PluginTypeMESS:     return "mess";
    case MusEPlugin::PluginTypeVST3:     return "vst3";
    default:                             return "other";
  }
}


//---------------------------------------------------------
//   Effect rack presets
//   The files are the same as those of the effect rack menu and the plugin windows (.pre), in
//   <config>/presets/plugins/<plugin label>/ for the user, and in the global share directory.
//---------------------------------------------------------

// The label without the web address that some ladspa plugins have in front, as a directory name.
QString presetDirName(const QString& label)
{
  QString l = label;
  while(l.endsWith('/'))
    l.chop(1);
  l = l.mid(l.lastIndexOf('/') + 1).trimmed();
  return l;
}

QString presetRoot(bool user)
{
  return (user ? MusEGlobal::configPath : MusEGlobal::museGlobalShare) + QString("/presets/plugins");
}

// A name of a preset or directory that is safe as a file name.
bool presetNameOk(const QString& n)
{
  if(n.isEmpty() || n.size() > 100 || n.startsWith('.'))
    return false;
  for(const QChar c : n)
    if(!(c.isLetterOrNumber() || c == ' ' || c == '_' || c == '-' || c == '.' || c == '(' || c == ')' || c == '+'))
      return false;
  return true;
}

// The file of a preset id "user:Dir/Name" or "global:Dir/Name". Empty with error set if it is not valid.
QString presetFileFromId(const QString& id, QString& error)
{
  const int c = id.indexOf(':');
  const QString src = c > 0 ? id.left(c) : QString();
  const QStringList parts = id.mid(c + 1).split('/');
  if((src != "user" && src != "global") || parts.size() != 2 || !presetNameOk(parts[0]) || !presetNameOk(parts[1]))
  {
    error = QObject::tr("Not a preset id. Use the ids from list_plugin_presets.");
    return QString();
  }
  const QString file = presetRoot(src == "user") + "/" + parts[0] + "/" + parts[1] + ".pre";
  if(!QFileInfo::exists(file))
  {
    error = QObject::tr("There is no such preset.");
    return QString();
  }
  return file;
}

// The part of a track that starts at the given tick, or null with error set.
Part* partFromArgs(const QJsonObject& args, const char* trackKey, const char* tickKey, QString& error, Track** trackOut = nullptr)
{
  const QString name = args.value(trackKey).toString();
  Track* t = name.isEmpty() ? nullptr : MusEGlobal::song->findTrack(name);
  if(!t || t->isFolderTrack())
  {
    error = QObject::tr("There is no track named \"%1\".").arg(name);
    return nullptr;
  }
  if(!args.value(tickKey).isDouble())
  {
    error = QObject::tr("%1 is needed: The start of the part in ticks, see list_parts.").arg(tickKey);
    return nullptr;
  }
  const unsigned tick = unsigned(qMax(0.0, args.value(tickKey).toDouble()));
  if(trackOut)
    *trackOut = t;
  const PartList* pl = t->cparts();
  for(ciPart i = pl->begin(); i != pl->end(); ++i)
    if(i->second->tick() == tick)
      return i->second;
  error = QObject::tr("Track \"%1\" has no part starting at tick %2.").arg(name).arg(tick);
  return nullptr;
}


QJsonObject noteJson(const Event& e)
{
  QJsonObject o;
  o["tick"] = double(e.tick());
  o["pitch"] = e.pitch();
  o["length_ticks"] = double(e.lenTick());
  o["velocity"] = e.velo();
  return o;
}

// The midi part of the request (track and part_start_tick), or null with error set.
Part* midiPartFromArgs(const QJsonObject& args, QString& error)
{
  Part* part = partFromArgs(args, "track", "part_start_tick", error);
  if(part && (!part->track() || !part->track()->isMidiTrack()))
  {
    error = QObject::tr("The part is not a midi part.");
    return nullptr;
  }
  return part;
}

// The first note of the part at this tick (from the start of the part) with this pitch, not in used.
bool findNote(const Part* part, int tick, int pitch, const std::vector<Event>& used, Event& found)
{
  if(tick < 0)
    return false;
  cEventRange range = part->events().equal_range(unsigned(tick));
  for(ciEvent i = range.first; i != range.second; ++i)
  {
    const Event& e = i->second;
    if(!e.isNote() || e.pitch() != pitch)
      continue;
    bool taken = false;
    for(const Event& u : used)
      if(u == e)
        taken = true;
    if(taken)
      continue;
    found = e;
    return true;
  }
  return false;
}


// Collects the patches of a popup menu, with the names of the sub menus as the group.
void collectPatches(const QMenu* menu, const QString& group, QJsonArray& out, int limit, const QString& filter)
{
  for(const QAction* a : menu->actions())
  {
    if(a->isSeparator())
      continue;
    if(a->menu())
    {
      collectPatches(a->menu(), group.isEmpty() ? a->text() : group + " / " + a->text(), out, limit, filter);
      continue;
    }
    if(out.size() >= limit || !(a->data().type() == QVariant::Int || a->data().type() == QVariant::UInt))
      continue;
    const int id = a->data().toInt();
    if(id == -1)
      continue;
    if(!filter.isEmpty() && !a->text().contains(filter, Qt::CaseInsensitive) && !group.contains(filter, Qt::CaseInsensitive))
      continue;
    QJsonObject o;
    o["name"] = a->text();
    o["group"] = group;
    o["patch"] = id;
    const int hb = (id >> 16) & 0xff, lb = (id >> 8) & 0xff, pr = id & 0xff;
    o["bankHigh"] = hb == 0xff ? QJsonValue() : QJsonValue(hb);
    o["bankLow"] = lb == 0xff ? QJsonValue() : QJsonValue(lb);
    o["program"] = pr == 0xff ? QJsonValue() : QJsonValue(pr);
    out.push_back(o);
  }
}

// The midi port and channel (0 based) that a midi track or a synth track sends to. False with error set.
bool trackMidiOutput(Track* t, int channelArg, int& port, int& channel, QString& error)
{
  port = -1;
  channel = qBound(1, channelArg, 16) - 1;
  if(t->isMidiTrack())
  {
    port = static_cast<MidiTrack*>(t)->outPort();
    if(channelArg <= 0)
      channel = static_cast<MidiTrack*>(t)->outChannel();
  }
  else if(t->type() == Track::AUDIO_SOFTSYNTH)
  {
    for(int i = 0; i < MIDI_PORTS; ++i)
      if(MusEGlobal::midiPorts[i].device() == static_cast<const MidiDevice*>(static_cast<SynthI*>(t)))
        port = i;
  }
  else
  {
    error = QObject::tr("\"%1\" is not a midi track or a synth track.").arg(t->name());
    return false;
  }
  if(port < 0 || port >= MIDI_PORTS)
  {
    error = QObject::tr("\"%1\" has no midi port.").arg(t->name());
    return false;
  }
  return true;
}

// The audio tracks asked for by name, or all of them if there is no list. Sets error if a name is wrong.
std::vector<AudioTrack*> audioTracksFromArgs(const QJsonObject& args, QString& error)
{
  std::vector<AudioTrack*> v;
  if(args.contains("tracks"))
  {
    const QJsonArray names = args.value("tracks").toArray();
    for(const QJsonValue& n : names)
    {
      Track* t = MusEGlobal::song->findTrack(n.toString());
      if(!t)
      {
        error = QObject::tr("There is no track named \"%1\".").arg(n.toString());
        return v;
      }
      if(!t->isAudioTrack())
      {
        error = QObject::tr("\"%1\" is not an audio track.").arg(n.toString());
        return v;
      }
      v.push_back(static_cast<AudioTrack*>(t));
    }
  }
  else
  {
    const TrackList* tl = MusEGlobal::song->tracks();
    for(ciTrack it = tl->begin(); it != tl->end(); ++it)
      if((*it)->isAudioTrack())
        v.push_back(static_cast<AudioTrack*>(*it));
  }
  return v;
}

bool g_measuring = false;

} // anonymous namespace

//---------------------------------------------------------
//   registerMcpTools
//---------------------------------------------------------

void registerMcpTools(McpServer& server)
{
  //---------------------------------------------------------
  //   get_song_info
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "get_song_info";
    t.description = QObject::tr("Overview of the song: audio setup, length, tempo and time signature at the start, "
                                "and how many tracks of each type there are.");
    t.inputSchema = schema();
    t.handler = [](const QJsonObject&, McpDone done) {
      QJsonObject o;
      o["projectDirectory"] = MusEGlobal::museProject;
      o["modified"] = MusEGlobal::song->dirty;
      o["audioDriver"] = MusEGlobal::audioDevice ? QString(MusEGlobal::audioDevice->driverName()) : QString();
      o["sampleRate"] = int(MusEGlobal::sampleRate);
      o["segmentSize"] = int(MusEGlobal::segmentSize);
      o["ticksPerQuarterNote"] = MusEGlobal::config.division;
      o["lengthTicks"] = double(MusEGlobal::song->len());
      o["lengthSeconds"] = round2(tickToSeconds(MusEGlobal::song->len()));
      o["tempoBpmAtStart"] = round2(tempoBpm(0));
      o["timeSignatureAtStart"] = timeSigJson(0);

      QJsonObject counts;
      int total = 0;
      const TrackList* tl = MusEGlobal::song->tracks();
      for(ciTrack it = tl->begin(); it != tl->end(); ++it)
      {
        const QString k = trackTypeName((*it)->type());
        counts[k] = counts.value(k).toInt() + 1;
        ++total;
      }
      o["trackCount"] = total;
      o["tracksByType"] = counts;
      o["markerCount"] = int(MusEGlobal::song->marker()->size());
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   get_transport
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "get_transport";
    t.description = QObject::tr("The transport: whether it is playing or recording, the position of the cursor and the "
                                "left and right locators (as ticks, frames, seconds and bar/beat), loop and punch, and the "
                                "tempo and time signature at the cursor.");
    t.inputSchema = schema();
    t.handler = [](const QJsonObject&, McpDone done) {
      Song* s = MusEGlobal::song;
      QJsonObject o;
      o["playing"] = MusEGlobal::audio->isPlaying();
      o["recording"] = MusEGlobal::audio->isRecording();
      o["recordEnabled"] = s->record();
      o["loop"] = s->loop();
      o["punchIn"] = s->punchin();
      o["punchOut"] = s->punchout();
      o["cursor"] = positionJson(s->cpos());
      o["leftLocator"] = positionJson(s->lpos());
      o["rightLocator"] = positionJson(s->rpos());
      o["tempoBpmAtCursor"] = round2(tempoBpm(s->cpos()));
      o["timeSignatureAtCursor"] = timeSigJson(s->cpos());
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   list_tracks
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_tracks";
    t.description = QObject::tr("All tracks in the order of the arranger, with type, mute, solo, off, record arm, folder, "
                                "volume and pan (audio tracks) or port and channel (midi tracks). "
                                "Use get_track for the plugins, aux sends and routing of one track.");
    t.inputSchema = schema();
    t.handler = [](const QJsonObject&, McpDone done) {
      QJsonArray arr;
      const TrackList* tl = MusEGlobal::song->tracks();
      for(ciTrack it = tl->begin(); it != tl->end(); ++it)
        arr.push_back(trackJson(*it, false));
      QJsonObject o;
      o["tracks"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   get_track
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "get_track";
    t.description = QObject::tr("Everything about one track: its settings, and for audio tracks the gain, prefader, aux "
                                "sends, effect plugins with the values of all their parameters, and the routes in and out.");
    QJsonObject props;
    props["name"] = prop("string", QObject::tr("The name of the track."));
    t.inputSchema = schema(props, QStringList() << "name");
    t.handler = [](const QJsonObject& args, McpDone done) {
      const Track* tr = MusEGlobal::song->findTrack(args.value("name").toString());
      if(!tr)
      {
        done(QJsonObject(), QObject::tr("There is no track named \"%1\".").arg(args.value("name").toString()));
        return;
      }
      done(trackJson(tr, true), QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   list_parts
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_parts";
    t.description = QObject::tr("The parts (regions) of one track, or of all tracks, with start and length in ticks and "
                                "seconds. Wave parts also have their volume and fades.");
    QJsonObject props;
    props["track"] = prop("string", QObject::tr("The name of a track. All tracks if left out."));
    t.inputSchema = schema(props);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QJsonArray arr;
      const auto add = [&arr](const Track* tr) {
        if(tr->isFolderTrack())
          return;
        for(ciPart ip = tr->cparts()->cbegin(); ip != tr->cparts()->cend(); ++ip)
          arr.push_back(partJson(ip->second));
      };
      if(args.contains("track"))
      {
        const Track* tr = MusEGlobal::song->findTrack(args.value("track").toString());
        if(!tr)
        {
          done(QJsonObject(), QObject::tr("There is no track named \"%1\".").arg(args.value("track").toString()));
          return;
        }
        add(tr);
      }
      else
      {
        const TrackList* tl = MusEGlobal::song->tracks();
        for(ciTrack it = tl->begin(); it != tl->end(); ++it)
          add(*it);
      }
      QJsonObject o;
      o["parts"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   get_levels
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "get_levels";
    t.description = QObject::tr("The level of audio tracks right now, per channel, in dBFS: The peak of the last audio block, "
                                "the highest peak since the peaks were reset in the mixer, and whether the track has clipped. "
                                "This is a single instant. To see how loud a track is over time, use measure_levels.");
    QJsonObject props;
    props["tracks"] = trackListProp(QObject::tr("Names of audio tracks. All audio tracks if left out."));
    t.inputSchema = schema(props);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      const std::vector<AudioTrack*> tracks = audioTracksFromArgs(args, error);
      if(!error.isEmpty())
      {
        done(QJsonObject(), error);
        return;
      }
      QJsonArray arr;
      for(const AudioTrack* at : tracks)
      {
        QJsonArray chans;
        for(int c = 0; c < at->channels(); ++c)
        {
          QJsonObject co;
          co["peakDb"] = round2(toDb(at->meter(c)));
          co["peakHoldDb"] = round2(toDb(at->peak(c)));
          co["clipped"] = at->isClipped(c);
          chans.push_back(co);
        }
        QJsonObject to;
        to["name"] = at->name();
        to["type"] = trackTypeName(at->type());
        to["channels"] = chans;
        arr.push_back(to);
      }
      QJsonObject o;
      o["tracks"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   measure_levels
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "measure_levels";
    t.description = QObject::tr("Measures the level of audio tracks over a time window while the audio runs, and answers when "
                                "the time is up: Per channel the highest peak and the RMS level in dBFS, and whether the signal "
                                "reached full scale. Start the transport or let the inputs play before measuring, "
                                "the audio is measured as it is.");
    QJsonObject props;
    props["duration_ms"] = prop("integer", QObject::tr("The length of the measurement in milliseconds, 100 to 10000. 500 if left out. 500 is enough to tell whether there is signal and how loud it is; use longer only to cover a whole musical phrase or section."));
    props["tracks"] = trackListProp(QObject::tr("Names of audio tracks. All audio tracks if left out."));
    t.inputSchema = schema(props);
    t.handler = [](const QJsonObject& args, McpDone done) {
      if(g_measuring)
      {
        done(QJsonObject(), QObject::tr("A measurement is already running."));
        return;
      }
      int ms = args.contains("duration_ms") ? args.value("duration_ms").toInt(500) : 500;
      ms = qBound(100, ms, 10000);
      QString error;
      const std::vector<AudioTrack*> tracks = audioTracksFromArgs(args, error);
      if(!error.isEmpty())
      {
        done(QJsonObject(), error);
        return;
      }
      // By name: A track may be gone when the time is up.
      const auto names = std::make_shared<QStringList>();
      for(AudioTrack* at : tracks)
      {
        names->push_back(at->name());
        at->startLevelMeasure();
      }
      g_measuring = true;

      QTimer::singleShot(ms, QCoreApplication::instance(), [names, ms, done]() {
        g_measuring = false;
        QJsonArray arr;
        for(const QString& n : *names)
        {
          Track* tr = MusEGlobal::song->findTrack(n);
          if(!tr || !tr->isAudioTrack())
            continue;
          AudioTrack* at = static_cast<AudioTrack*>(tr);
          AudioTrack::LevelMeasure m;
          at->stopLevelMeasure(m);
          QJsonArray chans;
          for(int c = 0; c < at->channels(); ++c)
          {
            QJsonObject co;
            co["peakDb"] = round2(toDb(m.peak[c]));
            co["rmsDb"] = round2(toDb(m.rms[c]));
            co["reachedFullScale"] = m.peak[c] >= 1.0;
            chans.push_back(co);
          }
          QJsonObject to;
          to["name"] = at->name();
          to["type"] = trackTypeName(at->type());
          to["framesMeasured"] = double(m.frames);
          to["channels"] = chans;
          arr.push_back(to);
        }
        QJsonObject o;
        o["durationMs"] = ms;
        o["transportPlaying"] = MusEGlobal::audio->isPlaying();
        o["note"] = QObject::tr("framesMeasured 0 means the track processed no audio during the measurement.");
        o["tracks"] = arr;
        done(o, QString());
      });
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   list_audio_ports
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_audio_ports";
    t.description = QObject::tr("The audio ports of the sound card or audio system that tracks can be routed from and to. "
                                "The routes of a track show which of these it uses (see get_track).");
    t.inputSchema = schema();
    t.handler = [](const QJsonObject&, McpDone done) {
      QJsonObject o;
      QJsonArray in, out;
      if(MusEGlobal::audioDevice)
      {
        // Ports that the audio system gives us as inputs are where audio comes from, so the sources of the audio inputs.
        const std::list<QString> ins = MusEGlobal::audioDevice->outputPorts(false, -1);
        const std::list<QString> outs = MusEGlobal::audioDevice->inputPorts(false, -1);
        for(const QString& s : ins)
          in.push_back(s);
        for(const QString& s : outs)
          out.push_back(s);
      }
      o["sources"] = in;
      o["destinations"] = out;
      o["note"] = QObject::tr("sources are what audio input tracks can record from, destinations are what audio output tracks can play to.");
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   list_markers
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_markers";
    t.description = QObject::tr("The markers of the song (for example the sections) with their positions, ids (to "
                                "name a marker in modify_marker and delete_marker, valid until MusE is closed) and "
                                "whether they are locked to time (they stay at their time when the tempo changes). "
                                "The range markers are the left and right locators, see get_transport.");
    t.inputSchema = schema();
    t.handler = [](const QJsonObject&, McpDone done) {
      QJsonArray arr;
      const MarkerList* ml = MusEGlobal::song->marker();
      for(ciMarker i = ml->cbegin(); i != ml->cend(); ++i)
        arr.push_back(markerJson(i->second));
      QJsonObject o;
      o["markers"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   list_plugins
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_plugins";
    t.description = QObject::tr("The effect plugins that are installed and can be added to audio tracks with add_plugin. "
                                "Give a filter text to search the name, label and maker, there can be hundreds.");
    QJsonObject props;
    props["filter"] = prop("string", QObject::tr("Case insensitive text that must be in the name, label or maker."));
    props["type"] = prop("string", QObject::tr("Only this type: ladspa, dssi, lv2, vst, vst3, linux_vst."));
    props["limit"] = prop("integer", QObject::tr("The most plugins to return, 1 to 500. 100 if left out."));
    t.inputSchema = schema(props);
    t.handler = [](const QJsonObject& args, McpDone done) {
      const QString filter = args.value("filter").toString().trimmed();
      const QString type = args.value("type").toString().trimmed().toLower();
      const int limit = qBound(1, args.value("limit").toInt(100), 500);
      QJsonArray arr;
      int total = 0;
      for(ciPlugin i = MusEGlobal::plugins.cbegin(); i != MusEGlobal::plugins.cend(); ++i)
      {
        const Plugin* p = *i;
        if(!(p->pluginClass() & MusEPlugin::PluginClassEffect))
          continue;
        if(!type.isEmpty() && type != pluginTypeName(p->pluginType()))
          continue;
        if(!filter.isEmpty() && !p->name().contains(filter, Qt::CaseInsensitive) &&
           !p->label().contains(filter, Qt::CaseInsensitive) && !p->maker().contains(filter, Qt::CaseInsensitive))
          continue;
        ++total;
        if(arr.size() >= limit)
          continue;
        QJsonObject o;
        o["name"] = p->name();
        o["label"] = p->label();
        o["maker"] = p->maker();
        o["type"] = pluginTypeName(p->pluginType());
        o["uri"] = p->uri();
        o["file"] = p->lib();
        o["audioInputs"] = int(p->inports());
        o["audioOutputs"] = int(p->outports());
        o["controls"] = int(p->controlInPorts());
        arr.push_back(o);
      }
      QJsonObject o;
      o["total"] = total;
      o["truncated"] = total > arr.size();
      o["plugins"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   list_synths
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_synths";
    t.description = QObject::tr("The software synthesizers that are installed and can be added as tracks with add_synth_track. "
                                "Give a filter text to search the name, label and maker.");
    QJsonObject props;
    props["filter"] = prop("string", QObject::tr("Case insensitive text that must be in the name, label or maker."));
    props["limit"] = prop("integer", QObject::tr("The most synths to return, 1 to 500. 100 if left out."));
    t.inputSchema = schema(props);
    t.handler = [](const QJsonObject& args, McpDone done) {
      const QString filter = args.value("filter").toString().trimmed();
      const int limit = qBound(1, args.value("limit").toInt(100), 500);
      QJsonArray arr;
      int total = 0;
      for(const Synth* sy : MusEGlobal::synthis)
      {
        if(!filter.isEmpty() && !sy->name().contains(filter, Qt::CaseInsensitive) &&
           !sy->label().contains(filter, Qt::CaseInsensitive) && !sy->maker().contains(filter, Qt::CaseInsensitive))
          continue;
        ++total;
        if(arr.size() >= limit)
          continue;
        QJsonObject o;
        o["name"] = sy->name();
        o["label"] = sy->label();
        o["maker"] = sy->maker();
        o["type"] = pluginTypeName(sy->pluginType());
        o["uri"] = sy->uri();
        o["file"] = sy->completeBaseName();
        arr.push_back(o);
      }
      QJsonObject o;
      o["total"] = total;
      o["truncated"] = total > arr.size();
      o["synths"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   list_plugin_presets
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_plugin_presets";
    t.description = QObject::tr("The saved effect plugin presets (the files of the effect rack's save and load preset). "
                                "With track and slot only the presets of that plugin are listed. "
                                "Load one with load_plugin_preset, using its id.");
    QJsonObject props;
    props["track"] = prop("string", QObject::tr("Audio track name, to list the presets of one of its plugins."));
    props["slot"] = prop("integer", QObject::tr("Effect slot, with track."));
    props["filter"] = prop("string", QObject::tr("Case insensitive text that must be in the preset id."));
    t.inputSchema = schema(props);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString onlyDir;
      if(args.contains("track"))
      {
        QString error;
        Track* tr = trackFromArgs(args, true, error);
        if(!tr)
        {
          done(QJsonObject(), error);
          return;
        }
        const int slot = args.value("slot").toInt(0) - 1;
        const PluginI* p = (slot >= 0 && slot < PipelineDepth) ? (*static_cast<AudioTrack*>(tr)->efxPipe())[slot] : nullptr;
        if(!p)
        {
          done(QJsonObject(), QObject::tr("There is no plugin in that slot."));
          return;
        }
        onlyDir = presetDirName(p->pluginLabel());
      }
      const QString filter = args.value("filter").toString().trimmed();
      QJsonArray arr;
      for(int u = 1; u >= 0; --u)
      {
        const QString root = presetRoot(u == 1);
        const QStringList dirs = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for(const QString& d : dirs)
        {
          if((!onlyDir.isEmpty() && d != onlyDir) || !presetNameOk(d))
            continue;
          const QStringList files = QDir(root + "/" + d).entryList(QStringList() << "*.pre", QDir::Files, QDir::Name);
          for(const QString& f : files)
          {
            const QString base = f.left(f.size() - 4);
            if(!presetNameOk(base))
              continue;
            const QString id = QString(u ? "user:" : "global:") + d + "/" + base;
            if(!filter.isEmpty() && !id.contains(filter, Qt::CaseInsensitive))
              continue;
            QJsonObject o;
            o["id"] = id;
            o["plugin"] = d;
            o["name"] = base;
            o["source"] = u ? "user" : "global";
            arr.push_back(o);
          }
        }
      }
      QJsonObject o;
      o["presets"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   get_midi_notes
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "get_midi_notes";
    t.description = QObject::tr("The notes of a midi part, in time order. Ticks are counted from the start of the part. "
                                "Use the tick and pitch of a note to modify or erase it.");
    QJsonObject props;
    props["track"] = prop("string", QObject::tr("Track name."));
    props["part_start_tick"] = prop("integer", QObject::tr("Start tick of the part, from list_parts."));
    props["from_tick"] = prop("integer", QObject::tr("Only notes starting at or after this tick of the part."));
    props["to_tick"] = prop("integer", QObject::tr("Only notes starting before this tick of the part."));
    props["pitch_min"] = prop("integer", QObject::tr("Lowest pitch."));
    props["pitch_max"] = prop("integer", QObject::tr("Highest pitch."));
    props["limit"] = prop("integer", QObject::tr("The most notes to return, 1 to 5000. 500 if left out."));
    t.inputSchema = schema(props, {"track", "part_start_tick"});
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Part* part = midiPartFromArgs(args, error);
      if(!part)
      {
        done(QJsonObject(), error);
        return;
      }
      const double from = args.value("from_tick").toDouble(0.0);
      const double to = args.contains("to_tick") ? args.value("to_tick").toDouble() : 1e18;
      const int pmin = args.value("pitch_min").toInt(0);
      const int pmax = args.value("pitch_max").toInt(127);
      const int limit = qBound(1, args.value("limit").toInt(500), 5000);
      QJsonArray arr;
      int total = 0;
      for(ciEvent i = part->events().begin(); i != part->events().end(); ++i)
      {
        const Event& e = i->second;
        if(!e.isNote() || double(e.tick()) < from || double(e.tick()) >= to || e.pitch() < pmin || e.pitch() > pmax)
          continue;
        ++total;
        if(arr.size() < limit)
          arr.push_back(noteJson(e));
      }
      QJsonObject o;
      o["total"] = total;
      o["truncated"] = total > arr.size();
      o["notes"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   get_audio_part
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "get_audio_part";
    t.description = QObject::tr("The audio of a wave part: For each audio event the sound file (full path, so that it can be "
                                "analysed with other tools), its sample rate, channels and length, and where in the file and in "
                                "the song it plays. Part volume, fades and effects are not applied to the file.");
    QJsonObject props;
    props["track"] = prop("string", QObject::tr("Wave track name."));
    props["part_start_tick"] = prop("integer", QObject::tr("Start tick of the part, from list_parts."));
    t.inputSchema = schema(props, {"track", "part_start_tick"});
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Part* part = partFromArgs(args, "track", "part_start_tick", error);
      if(!part)
      {
        done(QJsonObject(), error);
        return;
      }
      if(!part->track() || part->track()->type() != Track::WAVE)
      {
        done(QJsonObject(), QObject::tr("The part is not a wave part."));
        return;
      }
      const double songRate = sampleRate();
      const double partStartSec = tickToSeconds(part->tick());
      QJsonArray evs;
      for(ciEvent i = part->events().begin(); i != part->events().end(); ++i)
      {
        const Event& e = i->second;
        if(e.type() != Wave)
          continue;
        QJsonObject o;
        const SndFileR sf = e.sndFile();
        if(sf.isNull())
          o["file"] = QJsonValue();
        else
        {
          o["file"] = sf.path();
          o["fileName"] = sf.name();
          o["fileSampleRate"] = sf.samplerate();
          o["channels"] = sf.channels();
          o["fileFrames"] = double(sf.samples());
          o["fileSeconds"] = round4(sf.samplerate() > 0 ? double(sf.samples()) / sf.samplerate() : 0.0);
          o["startOffsetInFileSeconds"] = round4(sf.samplerate() > 0 ? double(e.spos()) / sf.samplerate() : 0.0);
        }
        o["startOffsetInFileFrames"] = e.spos();
        o["lengthFrames"] = double(e.lenFrame());
        o["lengthSeconds"] = round4(double(e.lenFrame()) / songRate);
        o["positionInPartFrames"] = double(e.frame());
        o["positionInPartSeconds"] = round4(double(e.frame()) / songRate);
        o["positionInSongSeconds"] = round4(partStartSec + double(e.frame()) / songRate);
        evs.push_back(o);
      }
      QJsonObject o;
      o["part"] = partJson(part);
      o["songSampleRate"] = songRate;
      o["events"] = evs;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   get_tempo_map
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "get_tempo_map";
    t.description = QObject::tr("The tempo and time signature changes of the song. If the tempo map is off, the song has one "
                                "static tempo (see set_tempo_map_enabled).");
    t.inputSchema = schema();
    t.handler = [](const QJsonObject&, McpDone done) {
      QJsonObject o;
      o["tempoMapEnabled"] = MusEGlobal::tempomap.masterFlag();
      const int st = MusEGlobal::tempomap.staticTempo();
      o["staticTempoBpm"] = round2(st > 0 ? 60000000.0 / st : 0.0);
      // In these lists the key is the END of an item, and the item has its own start tick
      //  (the last item is stored under a position past the end of the song).
      QJsonArray tempos;
      if(MusEGlobal::tempomap.masterFlag())
      {
        for(ciTEvent i = MusEGlobal::tempomap.begin(); i != MusEGlobal::tempomap.end(); ++i)
        {
          QJsonObject e = positionJson(i->second->tick);
          e["bpm"] = round2(i->second->tempo > 0 ? 60000000.0 / i->second->tempo : 0.0);
          tempos.push_back(e);
        }
      }
      else
      {
        QJsonObject e = positionJson(0);
        e["bpm"] = round2(st > 0 ? 60000000.0 / st : 0.0);
        tempos.push_back(e);
      }
      o["tempos"] = tempos;
      QJsonArray sigs;
      for(ciSigEvent i = MusEGlobal::sigmap.begin(); i != MusEGlobal::sigmap.end(); ++i)
      {
        QJsonObject e = positionJson(i->second->tick);
        e["numerator"] = i->second->sig.z;
        e["denominator"] = i->second->sig.n;
        sigs.push_back(e);
      }
      o["timeSignatures"] = sigs;
      o["ticksPerQuarterNote"] = MusEGlobal::config.division;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   get_drum_map
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "get_drum_map";
    t.description = QObject::tr("The drum map of a drum track: Which note (pitch) plays which drum. Use these pitches for "
                                "the notes of the drum track. Hidden drums are left out unless include_hidden is set.");
    QJsonObject props;
    props["track"] = prop("string", QObject::tr("Drum track name."));
    props["include_hidden"] = prop("boolean", QObject::tr("Also list the drums that are hidden in the editor."));
    t.inputSchema = schema(props, {"track"});
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      if(tr->type() != Track::DRUM)
      {
        done(QJsonObject(), QObject::tr("\"%1\" is not a drum track.").arg(tr->name()));
        return;
      }
      const MidiTrack* mt = static_cast<const MidiTrack*>(tr);
      const DrumMap* dm = mt->drummap();
      const bool hidden = args.value("include_hidden").toBool();
      QJsonArray arr;
      for(int i = 0; i < 128; ++i)
      {
        if((dm[i].hide && !hidden) || dm[i].name.isEmpty())
          continue;
        QJsonObject o;
        o["pitch"] = i;
        o["name"] = dm[i].name;
        o["mute"] = dm[i].mute;
        o["hidden"] = dm[i].hide;
        o["defaultLengthTicks"] = dm[i].len;
        arr.push_back(o);
      }
      QJsonObject o;
      o["track"] = tr->name();
      o["drums"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   list_midi_ports
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_midi_ports";
    t.description = QObject::tr("The midi ports that have a device (a synth, or a hardware or software midi port), with the "
                                "device and the instrument. A midi track plays on its output port; the port of a synth is also "
                                "in get_track of the synth track.");
    t.inputSchema = schema();
    t.handler = [](const QJsonObject&, McpDone done) {
      QJsonArray arr;
      for(int i = 0; i < MIDI_PORTS; ++i)
      {
        const MidiPort* mp = &MusEGlobal::midiPorts[i];
        if(!mp->device())
          continue;
        QJsonObject o;
        o["port"] = i + 1;
        o["device"] = mp->device()->name();
        o["isSynth"] = mp->device()->isSynti();
        o["instrument"] = mp->instrument() ? mp->instrument()->iname() : QString();
        QJsonArray users;
        for(const auto& mt : *MusEGlobal::song->midis())
          if(mt->outPort() == i)
            users.push_back(mt->name());
        o["midiTracks"] = users;
        arr.push_back(o);
      }
      QJsonObject o;
      o["ports"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   list_patches
  //---------------------------------------------------------
  {
    McpTool t;
    t.name = "list_patches";
    t.description = QObject::tr("The patch (sound) names of the instrument that a midi track or a synth track plays on. "
                                "Use the patch number with set_patch. For some synths the list is long: Use the filter.");
    QJsonObject props;
    props["track"] = prop("string", QObject::tr("Midi track or synth track name."));
    props["channel"] = prop("integer", QObject::tr("Midi channel 1-16. The track's channel if left out (1 for a synth track)."));
    props["filter"] = prop("string", QObject::tr("Case insensitive text that must be in the patch name or its group."));
    props["limit"] = prop("integer", QObject::tr("The most patches to return, 1 to 2000. 300 if left out."));
    t.inputSchema = schema(props, {"track"});
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      int port, chan;
      if(!tr || !trackMidiOutput(tr, args.value("channel").toInt(0), port, chan, error))
      {
        done(QJsonObject(), error);
        return;
      }
      MidiInstrument* instr = MusEGlobal::midiPorts[port].instrument();
      if(!instr)
      {
        done(QJsonObject(), QObject::tr("The port has no instrument."));
        return;
      }
      const int limit = qBound(1, args.value("limit").toInt(300), 2000);
      MusEGui::PopupMenu menu(false);
      instr->populatePatchPopup(&menu, chan, tr->type() == Track::DRUM);
      QJsonArray arr;
      collectPatches(&menu, QString(), arr, limit, args.value("filter").toString().trimmed());
      QJsonObject o;
      o["track"] = tr->name();
      o["port"] = port + 1;
      o["channel"] = chan + 1;
      o["instrument"] = instr->iname();
      o["limited"] = arr.size() >= limit;
      o["patches"] = arr;
      done(o, QString());
    };
    server.addTool(t);
  }

  //=========================================================
  //   Control tier
  //=========================================================

  //---------------------------------------------------------
  //   transport
  //---------------------------------------------------------
  {
    McpTool t = controlTool("transport",
      QObject::tr("Starts or stops the transport. Does nothing while MusE follows external sync. "
                  "The state changes in the next audio cycle, check get_transport to see it."),
      schema({{"action", prop("string", QObject::tr("\"play\" or \"stop\"."))}}, {"action"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      const QString a = args.value("action").toString();
      if(a != "play" && a != "stop")
      {
        done(QJsonObject(), QObject::tr("action must be \"play\" or \"stop\"."));
        return;
      }
      if(MusEGlobal::extSyncFlag)
      {
        done(QJsonObject(), QObject::tr("The transport follows external sync and can not be controlled."));
        return;
      }
      if(a == "play")
        MusEGlobal::song->setPlay(true);
      else
        MusEGlobal::song->setStop(true);
      QJsonObject o;
      o["requested"] = a;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   set_position
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_position",
      QObject::tr("Moves the cursor (also while playing). Give one of: tick, seconds, or bar (with optional beat, "
                  "both counted from 1)."),
      schema({{"tick", prop("integer", QObject::tr("Position in ticks."))},
              {"seconds", prop("number", QObject::tr("Position in seconds."))},
              {"bar", prop("integer", QObject::tr("Bar number, the first bar is 1."))},
              {"beat", prop("integer", QObject::tr("Beat in the bar, the first beat is 1. 1 if left out."))}}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      if(MusEGlobal::extSyncFlag)
      {
        done(QJsonObject(), QObject::tr("The transport follows external sync and can not be controlled."));
        return;
      }
      unsigned tick;
      QString error;
      if(!tickFromPosition(args, tick, error))
      {
        done(QJsonObject(), error);
        return;
      }
      MusEGlobal::song->setPos(Song::CPOS, Pos(tick, true), true, true, true);
      QJsonObject o;
      o["cursor"] = positionJson(tick);
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   set_loop / set_click
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_loop", QObject::tr("Turns the loop on or off."),
      schema({{"on", prop("boolean", QObject::tr("true to loop between the locators."))}}, {"on"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      MusEGlobal::song->setLoop(args.value("on").toBool());
      QJsonObject o;
      o["loop"] = MusEGlobal::song->loop();
      done(o, QString());
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("set_click", QObject::tr("Turns the metronome on or off."),
      schema({{"on", prop("boolean", QObject::tr("true for the metronome on."))}}, {"on"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      MusEGlobal::song->setClick(args.value("on").toBool());
      QJsonObject o;
      o["click"] = MusEGlobal::song->click();
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   set_locators
  //---------------------------------------------------------
  {
    QJsonObject left, right;
    left["type"] = "object";
    left["description"] = QObject::tr("The left locator (start of the range): tick, seconds, or bar and beat.");
    left["properties"] = positionProps(QObject::tr("The position"));
    right = left;
    right["description"] = QObject::tr("The right locator (end of the range): tick, seconds, or bar and beat.");
    McpTool t = controlTool("set_locators",
      QObject::tr("Sets the left and/or right locator, the range markers that the loop, recording with punch in/out "
                  "and the mixdown use. Give left, right or both. If the left one would come after the right one, "
                  "MusE swaps them. Not undoable, like in the GUI."),
      schema({{"left", left}, {"right", right}}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      const bool hasLeft = args.value("left").isObject(), hasRight = args.value("right").isObject();
      if(!hasLeft && !hasRight)
      {
        done(QJsonObject(), QObject::tr("Give left, right or both."));
        return;
      }
      unsigned l = 0, r = 0;
      QString error;
      if((hasLeft && !tickFromPosition(args.value("left").toObject(), l, error)) ||
         (hasRight && !tickFromPosition(args.value("right").toObject(), r, error)))
      {
        done(QJsonObject(), error);
        return;
      }
      Song* s = MusEGlobal::song;
      if(hasLeft && hasRight)
      {
        if(l > r)
          std::swap(l, r);
        // Song::setPos swaps the locators when the left one passes the right one. Set them in an order
        //  where that can not happen with the old position of the other one.
        if(l > s->rpos())
        {
          s->setPos(Song::RPOS, Pos(r, true));
          s->setPos(Song::LPOS, Pos(l, true));
        }
        else
        {
          s->setPos(Song::LPOS, Pos(l, true));
          s->setPos(Song::RPOS, Pos(r, true));
        }
      }
      else if(hasLeft)
        s->setPos(Song::LPOS, Pos(l, true));
      else
        s->setPos(Song::RPOS, Pos(r, true));
      QJsonObject o;
      o["leftLocator"] = positionJson(s->lpos());
      o["rightLocator"] = positionJson(s->rpos());
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   set_track_mute / set_track_solo
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_track_mute", QObject::tr("Mutes or unmutes a track."),
      schema({{"track", prop("string", QObject::tr("Track name."))},
              {"mute", prop("boolean", QObject::tr("true to mute."))}}, {"track", "mute"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      const bool val = args.value("mute").toBool();
      PendingOperationList operations;
      operations.add(PendingOperationItem(tr, val, PendingOperationItem::SetTrackMute));
      MusEGlobal::audio->msgExecutePendingOperations(operations, true);
      QJsonObject o;
      o["track"] = tr->name();
      o["mute"] = tr->mute();
      done(o, QString());
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("set_track_solo", QObject::tr("Solos or unsolos a track."),
      schema({{"track", prop("string", QObject::tr("Track name."))},
              {"solo", prop("boolean", QObject::tr("true to solo."))}}, {"track", "solo"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      const bool val = args.value("solo").toBool();
      PendingOperationList operations;
      operations.add(PendingOperationItem(tr, val, PendingOperationItem::SetTrackSolo));
      MusEGlobal::audio->msgExecutePendingOperations(operations, true);
      QJsonObject o;
      o["track"] = tr->name();
      o["solo"] = tr->solo();
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   set_track_volume / set_track_pan
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_track_volume",
      QObject::tr("Sets the fader of an audio track, in dB (about +10 at most, -120 or less is silence). "
                  "With automation in read mode while playing, the automation takes over again."),
      schema({{"track", prop("string", QObject::tr("Audio track name."))},
              {"volume_db", prop("number", QObject::tr("Fader value in dB, 0 is unity."))}}, {"track", "volume_db"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, true, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      if(!args.value("volume_db").isDouble())
      {
        done(QJsonObject(), QObject::tr("volume_db must be a number."));
        return;
      }
      const double db = args.value("volume_db").toDouble();
      const double lin = db <= -120.0 ? 0.0 : qBound(0.0, std::pow(10.0, db / 20.0), 3.16227766017);
      AudioTrack* at = static_cast<AudioTrack*>(tr);
      at->setParam(AC_VOLUME, lin);
      at->enableController(AC_VOLUME, false);
      const AutomationType atype = at->automationType();
      if(atype == AUTO_OFF || (atype == AUTO_READ && MusEGlobal::audio->isPlaying()) || atype == AUTO_TOUCH)
        at->enableController(AC_VOLUME, true);
      QJsonObject o;
      o["track"] = at->name();
      o["volume"] = round4(lin);
      o["volumeDb"] = round2(toDb(lin));
      done(o, QString());
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("set_track_pan", QObject::tr("Sets the pan of an audio track."),
      schema({{"track", prop("string", QObject::tr("Audio track name."))},
              {"pan", prop("number", QObject::tr("-1 is full left, 0 is center, 1 is full right."))}}, {"track", "pan"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, true, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      if(!args.value("pan").isDouble())
      {
        done(QJsonObject(), QObject::tr("pan must be a number."));
        return;
      }
      const double pan = qBound(-1.0, args.value("pan").toDouble(), 1.0);
      AudioTrack* at = static_cast<AudioTrack*>(tr);
      at->setParam(AC_PAN, pan);
      at->enableController(AC_PAN, false);
      const AutomationType atype = at->automationType();
      if(atype == AUTO_OFF || (atype == AUTO_READ && MusEGlobal::audio->isPlaying()) || atype == AUTO_TOUCH)
        at->enableController(AC_PAN, true);
      QJsonObject o;
      o["track"] = at->name();
      o["pan"] = round4(pan);
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   set_plugin_param / set_plugin_on
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_plugin_param",
      QObject::tr("Sets a parameter of an effect plugin on an audio track. The slot and parameter index are those "
                  "from get_track. The value is clamped to the range of the parameter."),
      schema({{"track", prop("string", QObject::tr("Audio track name."))},
              {"slot", prop("integer", QObject::tr("Effect slot, as in get_track."))},
              {"parameter", prop("integer", QObject::tr("Parameter index, as in get_track."))},
              {"value", prop("number", QObject::tr("The new value."))}}, {"track", "slot", "parameter", "value"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, true, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      AudioTrack* at = static_cast<AudioTrack*>(tr);
      const int slot = args.value("slot").toInt(0) - 1;
      PluginI* p = (slot >= 0 && slot < PipelineDepth) ? (*at->efxPipe())[slot] : nullptr;
      if(!p)
      {
        done(QJsonObject(), QObject::tr("There is no plugin in that slot."));
        return;
      }
      const int pi = args.value("parameter").toInt(-1);
      if(pi < 0 || (unsigned long)pi >= p->parameters() || !args.value("value").isDouble())
      {
        done(QJsonObject(), QObject::tr("Wrong parameter index or value."));
        return;
      }
      float mn = 0.0f, mx = 0.0f;
      p->range(pi, &mn, &mx);
      double val = args.value("value").toDouble();
      if(mx > mn)
        val = qBound(double(mn), val, double(mx));
      p->setParam(pi, val);
      p->enableController(pi, false);
      const AutomationType atype = at->automationType();
      if(atype == AUTO_OFF || (atype == AUTO_READ && MusEGlobal::audio->isPlaying()) || atype == AUTO_TOUCH)
        p->enableController(pi, true);
      QJsonObject o;
      o["track"] = at->name();
      o["slot"] = slot + 1;
      o["parameter"] = pi;
      o["value"] = round4(val);
      done(o, QString());
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("set_plugin_on",
      QObject::tr("Turns an effect plugin on an audio track on, or off (bypassed)."),
      schema({{"track", prop("string", QObject::tr("Audio track name."))},
              {"slot", prop("integer", QObject::tr("Effect slot, as in get_track."))},
              {"on", prop("boolean", QObject::tr("true for on, false for bypassed."))}}, {"track", "slot", "on"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, true, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      AudioTrack* at = static_cast<AudioTrack*>(tr);
      const int slot = args.value("slot").toInt(0) - 1;
      if(slot < 0 || slot >= PipelineDepth || !(*at->efxPipe())[slot])
      {
        done(QJsonObject(), QObject::tr("There is no plugin in that slot."));
        return;
      }
      at->efxPipe()->setOn(slot, args.value("on").toBool());
      MusEGlobal::song->update(SC_RACK);
      QJsonObject o;
      o["track"] = at->name();
      o["slot"] = slot + 1;
      o["on"] = at->efxPipe()->isOn(slot);
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   set_patch
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_patch",
      QObject::tr("Selects a patch (sound) on the instrument that a midi track or synth track plays on, by sending a "
                  "program change. Give the patch number from list_patches, or program with optional bank_high and bank_low."),
      schema({{"track", prop("string", QObject::tr("Midi track or synth track name."))},
              {"patch", prop("integer", QObject::tr("Patch number from list_patches."))},
              {"program", prop("integer", QObject::tr("Program 0-127 (as program in list_patches)."))},
              {"bank_high", prop("integer", QObject::tr("Bank 0-127, left out if the instrument has no banks."))},
              {"bank_low", prop("integer", QObject::tr("Bank 0-127, left out if the instrument has no banks."))},
              {"channel", prop("integer", QObject::tr("Midi channel 1-16. The track's channel if left out."))}}, {"track"}));
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      int port, chan;
      if(!tr || !trackMidiOutput(tr, args.value("channel").toInt(0), port, chan, error))
      {
        done(QJsonObject(), error);
        return;
      }
      int id;
      if(args.contains("patch"))
        id = args.value("patch").toInt(-1);
      else if(args.contains("program"))
      {
        const int pr = args.value("program").toInt(-1);
        const int hb = args.contains("bank_high") ? args.value("bank_high").toInt(-1) : 0xff;
        const int lb = args.contains("bank_low") ? args.value("bank_low").toInt(-1) : 0xff;
        if(pr < 0 || pr > 127 || hb < 0 || (hb > 127 && hb != 0xff) || lb < 0 || (lb > 127 && lb != 0xff))
        {
          done(QJsonObject(), QObject::tr("program and banks must be 0-127."));
          return;
        }
        id = (hb << 16) | (lb << 8) | pr;
      }
      else
      {
        done(QJsonObject(), QObject::tr("Give patch, or program."));
        return;
      }
      if(id < 0 || id > 0xffffff)
      {
        done(QJsonObject(), QObject::tr("Not a valid patch number."));
        return;
      }
      if(id == CTRL_PROGRAM_VAL_DONT_CARE)
        id = 0xffff00;
      MidiPort* mp = &MusEGlobal::midiPorts[port];
      MidiPlayEvent ev(MusEGlobal::audio->curFrame(), port, chan, ME_CONTROLLER, CTRL_PROGRAM, id);
      mp->putEvent(ev);
      QJsonObject o;
      o["track"] = tr->name();
      o["port"] = port + 1;
      o["channel"] = chan + 1;
      o["patch"] = id;
      if(mp->instrument())
        o["name"] = mp->instrument()->getPatchName(chan, id, tr->type() == Track::DRUM, false);
      done(o, QString());
    };
    server.addTool(t);
  }

  //=========================================================
  //   Record tier
  //=========================================================

  //---------------------------------------------------------
  //   set_track_record_arm
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_track_record_arm",
      QObject::tr("Arms or disarms a midi or wave track for recording. Nothing is recorded until the transport records."),
      schema({{"track", prop("string", QObject::tr("Track name."))},
              {"arm", prop("boolean", QObject::tr("true to arm."))}}, {"track", "arm"}),
      McpTierRecord);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      if(!(tr->isMidiTrack() || tr->type() == Track::WAVE))
      {
        done(QJsonObject(), QObject::tr("Only midi and wave tracks can be armed."));
        return;
      }
      MusEGlobal::song->setRecordFlag(tr, args.value("arm").toBool());
      QJsonObject o;
      o["track"] = tr->name();
      o["recordArmed"] = tr->recordFlag();
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   record
  //---------------------------------------------------------
  {
    McpTool t = controlTool("record",
      QObject::tr("Starts recording on the armed tracks (the transport starts from the cursor), or stops: "
                  "\"stop\" stops the transport and turns record off. Recording on wave tracks creates audio files "
                  "in the project directory, so the song must have been saved once."),
      schema({{"action", prop("string", QObject::tr("\"start\" or \"stop\"."))}}, {"action"}),
      McpTierRecord);
    t.handler = [](const QJsonObject& args, McpDone done) {
      const QString a = args.value("action").toString();
      if(a != "start" && a != "stop")
      {
        done(QJsonObject(), QObject::tr("action must be \"start\" or \"stop\"."));
        return;
      }
      if(MusEGlobal::extSyncFlag)
      {
        done(QJsonObject(), QObject::tr("The transport follows external sync and can not be controlled."));
        return;
      }
      Song* s = MusEGlobal::song;
      if(a == "stop")
      {
        s->setStop(true);
        s->setRecord(false);
      }
      else
      {
        if(MusEGlobal::audio->isRecording() || MusEGlobal::audio->isPlaying())
        {
          done(QJsonObject(), QObject::tr("The transport is already running."));
          return;
        }
        bool armed = false;
        for(ciTrack it = s->tracks()->begin(); it != s->tracks()->end(); ++it)
          if((*it)->recordFlag())
            armed = true;
        if(!armed)
        {
          done(QJsonObject(), QObject::tr("No track is armed. Use set_track_record_arm first."));
          return;
        }
        // Song::setRecord would open a dialog in these cases, which would freeze the request.
        if(MusEGlobal::config.useProjectSaveDialog && MusEGlobal::museProject == MusEGlobal::museProjectInitPath)
        {
          done(QJsonObject(), QObject::tr("The song has not been saved yet. Save it in MusE first, recording needs a project directory."));
          return;
        }
        s->setRecord(true, false);
        s->setPlay(true);
      }
      QJsonObject o;
      o["requested"] = a;
      done(o, QString());
    };
    server.addTool(t);
  }

  //=========================================================
  //   Edit tier
  //=========================================================

  //---------------------------------------------------------
  //   add_track
  //---------------------------------------------------------
  {
    McpTool t = controlTool("add_track",
      QObject::tr("Adds a track at the end of the track list. Undoable. Synth tracks are added with add_synth_track."),
      schema({{"type", prop("string", QObject::tr("midi, drum, wave, audio_group, audio_aux, audio_input, audio_output or folder."))},
              {"name", prop("string", QObject::tr("The name of the track. A default name is used if left out."))}}, {"type"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      static const struct { const char* name; Track::TrackType type; } types[] = {
        {"midi", Track::MIDI}, {"drum", Track::DRUM}, {"wave", Track::WAVE}, {"audio_group", Track::AUDIO_GROUP},
        {"audio_aux", Track::AUDIO_AUX}, {"audio_input", Track::AUDIO_INPUT}, {"audio_output", Track::AUDIO_OUTPUT},
        {"folder", Track::FOLDER} };
      const QString ty = args.value("type").toString();
      int found = -1;
      for(unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i)
        if(ty == types[i].name)
          found = int(i);
      if(found < 0)
      {
        done(QJsonObject(), QObject::tr("Unknown track type \"%1\".").arg(ty));
        return;
      }
      const QString name = args.value("name").toString().trimmed();
      if(!name.isEmpty() && MusEGlobal::song->findTrack(name))
      {
        done(QJsonObject(), QObject::tr("There is already a track named \"%1\".").arg(name));
        return;
      }
      Track* tr = MusEGlobal::song->addTrack(types[found].type);
      if(!tr)
      {
        done(QJsonObject(), QObject::tr("The track could not be added."));
        return;
      }
      if(!name.isEmpty())
        MusEGlobal::song->applyOperation(UndoOp(UndoOp::ModifyTrackName, tr, tr->name(), name));
      done(trackJson(tr, false), QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   rename_track
  //---------------------------------------------------------
  {
    McpTool t = controlTool("rename_track", QObject::tr("Renames a track. Undoable."),
      schema({{"track", prop("string", QObject::tr("Current track name."))},
              {"new_name", prop("string", QObject::tr("The new, unique name."))}}, {"track", "new_name"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      const QString nn = args.value("new_name").toString().trimmed();
      if(nn.isEmpty() || (nn != tr->name() && MusEGlobal::song->findTrack(nn)))
      {
        done(QJsonObject(), QObject::tr("The new name is empty or already used."));
        return;
      }
      if(nn != tr->name())
        MusEGlobal::song->applyOperation(UndoOp(UndoOp::ModifyTrackName, tr, tr->name(), nn));
      QJsonObject o;
      o["name"] = tr->name();
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   add_midi_part
  //---------------------------------------------------------
  {
    McpTool t = controlTool("add_midi_part",
      QObject::tr("Adds an empty part to a midi or drum track. Undoable. Give the start as start_tick, or as start_bar "
                  "(counted from 1). The length defaults to one bar."),
      schema({{"track", prop("string", QObject::tr("Midi or drum track name."))},
              {"start_tick", prop("integer", QObject::tr("Start in ticks."))},
              {"start_bar", prop("integer", QObject::tr("Start bar, the first bar is 1."))},
              {"length_ticks", prop("integer", QObject::tr("Length in ticks."))},
              {"name", prop("string", QObject::tr("Name of the part. The track name if left out."))}}, {"track"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      if(!tr->isMidiTrack())
      {
        done(QJsonObject(), QObject::tr("\"%1\" is not a midi or drum track.").arg(tr->name()));
        return;
      }
      unsigned start;
      if(args.contains("start_tick"))
        start = unsigned(qMax(0, args.value("start_tick").toInt()));
      else if(args.contains("start_bar"))
        start = MusEGlobal::sigmap.bar2tick(qMax(1, args.value("start_bar").toInt()) - 1, 0, 0);
      else
      {
        done(QJsonObject(), QObject::tr("Give start_tick or start_bar."));
        return;
      }
      int len = args.value("length_ticks").toInt(0);
      if(len <= 0)
      {
        int bar = 0, beat = 0;
        unsigned tk = 0;
        MusEGlobal::sigmap.tickValues(start, &bar, &beat, &tk);
        len = int(MusEGlobal::sigmap.bar2tick(bar + 1, 0, 0)) - int(start);
        if(len <= 0)
          len = MusEGlobal::config.division * 4;
      }
      MidiPart* p = new MidiPart(static_cast<MidiTrack*>(tr));
      p->setTick(start);
      p->setLenTick(unsigned(len));
      const QString nm = args.value("name").toString().trimmed();
      p->setName(nm.isEmpty() ? tr->name() : nm);
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::AddPart, p));
      const unsigned end = start + unsigned(len);
      if(MusEGlobal::song->len() < end)
        MusEGlobal::song->setLen(end);
      done(partJson(p), QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   add_midi_notes
  //---------------------------------------------------------
  {
    QJsonObject noteProps;
    noteProps["tick"] = prop("integer", QObject::tr("Start of the note in ticks, from the start of the part."));
    noteProps["pitch"] = prop("integer", QObject::tr("Midi note number 0-127, 60 is middle C."));
    noteProps["length_ticks"] = prop("integer", QObject::tr("Length of the note in ticks."));
    noteProps["velocity"] = prop("integer", QObject::tr("1-127, 100 if left out."));
    QJsonObject noteSchema = schema(noteProps, {"tick", "pitch", "length_ticks"});
    QJsonObject notes;
    notes["type"] = "array";
    notes["items"] = noteSchema;
    notes["description"] = QObject::tr("The notes to add, at most 2000.");
    McpTool t = controlTool("add_midi_notes",
      QObject::tr("Adds notes to an existing midi part (see list_parts, add_midi_part and ticksPerQuarterNote in "
                  "get_song_info). All notes are one undo step."),
      schema({{"track", prop("string", QObject::tr("Track name."))},
              {"part_start_tick", prop("integer", QObject::tr("Start tick of the part, from list_parts."))},
              {"notes", notes}}, {"track", "part_start_tick", "notes"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Part* part = partFromArgs(args, "track", "part_start_tick", error);
      if(!part)
      {
        done(QJsonObject(), error);
        return;
      }
      if(!part->track() || !part->track()->isMidiTrack())
      {
        done(QJsonObject(), QObject::tr("The part is not a midi part."));
        return;
      }
      const QJsonArray arr = args.value("notes").toArray();
      if(arr.isEmpty() || arr.size() > 2000)
      {
        done(QJsonObject(), QObject::tr("Give between 1 and 2000 notes."));
        return;
      }
      Undo group;
      for(const QJsonValue& v : arr)
      {
        const QJsonObject n = v.toObject();
        const int tick = n.value("tick").toInt(-1);
        const int pitch = n.value("pitch").toInt(-1);
        const int len = n.value("length_ticks").toInt(0);
        const int vel = n.value("velocity").toInt(100);
        if(tick < 0 || pitch < 0 || pitch > 127 || len < 1 || vel < 1 || vel > 127)
        {
          done(QJsonObject(), QObject::tr("A note is wrong (tick >= 0, pitch 0-127, length >= 1, velocity 1-127)."));
          return;
        }
        Event e(Note);
        e.setTick(unsigned(tick));
        e.setPitch(pitch);
        e.setLenTick(unsigned(len));
        e.setVelo(vel);
        group.push_back(UndoOp(UndoOp::AddEvent, e, part, true, true));
      }
      MusEGlobal::song->applyOperationGroup(group);
      QJsonObject o;
      o["added"] = arr.size();
      o["part"] = partJson(part);
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   move_part
  //---------------------------------------------------------
  {
    McpTool t = controlTool("move_part",
      QObject::tr("Moves a part in time, and optionally to another track of the same type. Undoable."),
      schema({{"track", prop("string", QObject::tr("Track of the part."))},
              {"part_start_tick", prop("integer", QObject::tr("Start tick of the part, from list_parts."))},
              {"new_start_tick", prop("integer", QObject::tr("New start in ticks."))},
              {"new_track", prop("string", QObject::tr("Track to move it to. The same track if left out."))}},
             {"track", "part_start_tick", "new_start_tick"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* src = nullptr;
      Part* part = partFromArgs(args, "track", "part_start_tick", error, &src);
      if(!part)
      {
        done(QJsonObject(), error);
        return;
      }
      if(!args.value("new_start_tick").isDouble())
      {
        done(QJsonObject(), QObject::tr("new_start_tick is needed."));
        return;
      }
      Track* dst = src;
      if(args.contains("new_track"))
      {
        dst = MusEGlobal::song->findTrack(args.value("new_track").toString());
        if(!dst || dst->isFolderTrack() || dst->type() != src->type())
        {
          done(QJsonObject(), QObject::tr("new_track must be an existing track of the same type."));
          return;
        }
      }
      const unsigned nt = unsigned(qMax(0.0, args.value("new_start_tick").toDouble()));
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::MovePart, part, part->posValue(), nt, Pos::TICKS, src, dst));
      const unsigned end = nt + part->lenTick();
      if(MusEGlobal::song->len() < end)
        MusEGlobal::song->setLen(end);
      QJsonObject o;
      o["track"] = dst->name();
      o["startTick"] = double(nt);
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   add_route / remove_route
  //---------------------------------------------------------
  for(int k = 0; k < 2; ++k)
  {
    const bool add = k == 0;
    McpTool t = controlTool(add ? "add_route" : "remove_route",
      add ? QObject::tr("Connects the output of an audio track to another audio track (a group, aux or output track), "
                        "channel by channel for as many channels as both have. Not undoable, like in the GUI. Connections to audio ports are not handled here.")
          : QObject::tr("Removes the connection from the output of an audio track to another audio track. Not undoable, like in the GUI."),
      schema({{"from_track", prop("string", QObject::tr("Audio track whose output is routed."))},
              {"to_track", prop("string", QObject::tr("Audio track that receives it."))}}, {"from_track", "to_track"}),
      McpTierEdit);
    t.handler = [add](const QJsonObject& args, McpDone done) {
      Track* a = MusEGlobal::song->findTrack(args.value("from_track").toString());
      Track* b = MusEGlobal::song->findTrack(args.value("to_track").toString());
      if(!a || !b || !a->isAudioTrack() || !b->isAudioTrack() || a == b)
      {
        done(QJsonObject(), QObject::tr("Both must be different, existing audio tracks."));
        return;
      }
      // Like the routing popup of the GUI: One single channel route per channel, as pending operations.
      // They are not undoable, like the same changes in the GUI.
      PendingOperationList operations;
      const int n = qMin(a->channels(), b->channels());
      for(int c = 0; c < n; ++c)
      {
        const Route s(a, c, 1);
        const Route d(b, c, 1);
        if(add && routeCanConnect(s, d))
          operations.add(PendingOperationItem(s, d, PendingOperationItem::AddRoute));
        else if(!add && routeCanDisconnect(s, d))
          operations.add(PendingOperationItem(s, d, PendingOperationItem::DeleteRoute));
      }
      if(operations.empty())
      {
        done(QJsonObject(), add ? QObject::tr("They are already connected, or the route is not possible (a loop).")
                                : QObject::tr("They are not connected."));
        return;
      }
      MusEGlobal::audio->msgExecutePendingOperations(operations, true);
      QJsonObject o;
      o["from"] = a->name();
      o["to"] = b->name();
      o["connected"] = add;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   undo / redo
  //---------------------------------------------------------
  for(int k = 0; k < 2; ++k)
  {
    const bool isUndo = k == 0;
    McpTool t = controlTool(isUndo ? "undo" : "redo",
      isUndo ? QObject::tr("Undoes the last undoable change, from the assistant or from the user.")
             : QObject::tr("Redoes the last undone change."),
      schema(), McpTierEdit);
    t.handler = [isUndo](const QJsonObject&, McpDone done) {
      Song* s = MusEGlobal::song;
      if(MusEGlobal::audio->isRecording())
      {
        done(QJsonObject(), QObject::tr("Not while recording."));
        return;
      }
      QAction* act = isUndo ? MusEGlobal::undoAction : MusEGlobal::redoAction;
      if(!act || !act->isEnabled())
      {
        done(QJsonObject(), isUndo ? QObject::tr("Nothing to undo.") : QObject::tr("Nothing to redo."));
        return;
      }
      if(isUndo)
        s->undo();
      else
        s->redo();
      QJsonObject o;
      o["done"] = isUndo ? "undo" : "redo";
      o["canUndo"] = MusEGlobal::undoAction && MusEGlobal::undoAction->isEnabled();
      o["canRedo"] = MusEGlobal::redoAction && MusEGlobal::redoAction->isEnabled();
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   add_plugin
  //---------------------------------------------------------
  {
    McpTool t = controlTool("add_plugin",
      QObject::tr("Adds an effect plugin to the effect rack of an audio track. Undoable. Name the plugin with its uri, "
                  "or with file and label, as list_plugins shows them. The slot is the first free one if left out. "
                  "A slot that is in use is only replaced with replace set."),
      schema({{"track", prop("string", QObject::tr("Audio track name."))},
              {"uri", prop("string", QObject::tr("Plugin uri (LV2 and others), from list_plugins."))},
              {"file", prop("string", QObject::tr("Plugin file, from list_plugins. Used with label if there is no uri."))},
              {"label", prop("string", QObject::tr("Plugin label, from list_plugins."))},
              {"type", prop("string", QObject::tr("The plugin type, to tell apart plugins that share a label."))},
              {"slot", prop("integer", QObject::tr("Effect slot, the first is 1."))},
              {"replace", prop("boolean", QObject::tr("true to replace the plugin in the slot."))}}, {"track"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, true, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      AudioTrack* at = static_cast<AudioTrack*>(tr);
      const QString uri = args.value("uri").toString();
      const QString file = args.value("file").toString();
      const QString label = args.value("label").toString();
      if(uri.isEmpty() && label.isEmpty())
      {
        done(QJsonObject(), QObject::tr("Give uri, or label (and file). See list_plugins."));
        return;
      }
      MusEPlugin::PluginTypes_t types = MusEPlugin::PluginTypesAll;
      const QString type = args.value("type").toString().toLower();
      if(!type.isEmpty())
      {
        types = MusEPlugin::PluginTypeNone;
        for(int b = 0; b < 16; ++b)
          if(type == pluginTypeName(MusEPlugin::PluginType(1 << b)))
            types = 1 << b;
        if(types == MusEPlugin::PluginTypeNone)
        {
          done(QJsonObject(), QObject::tr("Unknown plugin type \"%1\".").arg(type));
          return;
        }
      }
      Plugin* plugin = MusEGlobal::plugins.find(types, file, uri, label);
      if(!plugin || !(plugin->pluginClass() & MusEPlugin::PluginClassEffect))
      {
        done(QJsonObject(), QObject::tr("There is no such effect plugin. See list_plugins."));
        return;
      }
      int slot = -1;
      if(args.contains("slot"))
      {
        slot = args.value("slot").toInt(0) - 1;
        if(slot < 0 || slot >= PipelineDepth)
        {
          done(QJsonObject(), QObject::tr("The slot must be 1 to %1.").arg(int(PipelineDepth)));
          return;
        }
        if((*at->efxPipe())[slot] && !args.value("replace").toBool())
        {
          done(QJsonObject(), QObject::tr("The slot is in use. Set replace to replace it."));
          return;
        }
      }
      else
      {
        for(int i = 0; i < PipelineDepth; ++i)
          if(!(*at->efxPipe())[i])
          {
            slot = i;
            break;
          }
        if(slot < 0)
        {
          done(QJsonObject(), QObject::tr("All effect slots are in use."));
          return;
        }
      }
      PluginI* plugi = new PluginI();
      if(plugi->initPluginInstance(plugin, at->channels()))
      {
        delete plugi;
        done(QJsonObject(), QObject::tr("The plugin could not be created."));
        return;
      }
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::ChangeRackEffectPlugin, at, plugi, slot));
      QJsonObject o;
      o["track"] = at->name();
      const PluginI* now = (*at->efxPipe())[slot];
      if(now)
        o["plugin"] = pluginJson(now, slot);
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   add_synth_track
  //---------------------------------------------------------
  {
    McpTool t = controlTool("add_synth_track",
      QObject::tr("Adds a synth track with a software synthesizer, at the end of the track list. Undoable. Name the synth "
                  "with its uri, or with file and label, as list_synths shows them. The synth is also given a free "
                  "midi port, as when it is added in the GUI."),
      schema({{"uri", prop("string", QObject::tr("Synth uri, from list_synths."))},
              {"file", prop("string", QObject::tr("Synth file, from list_synths. Used with label if there is no uri."))},
              {"label", prop("string", QObject::tr("Synth label, from list_synths."))},
              {"type", prop("string", QObject::tr("The synth type, to tell apart synths that share a label."))},
              {"name", prop("string", QObject::tr("The name of the track. A default name if left out."))}}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      const QString uri = args.value("uri").toString();
      const QString file = args.value("file").toString();
      const QString label = args.value("label").toString();
      if(uri.isEmpty() && label.isEmpty())
      {
        done(QJsonObject(), QObject::tr("Give uri, or label (and file). See list_synths."));
        return;
      }
      MusEPlugin::PluginTypes_t types = MusEPlugin::PluginTypesAll;
      const QString type = args.value("type").toString().toLower();
      if(!type.isEmpty())
      {
        types = MusEPlugin::PluginTypeNone;
        for(int b = 0; b < 16; ++b)
          if(type == pluginTypeName(MusEPlugin::PluginType(1 << b)))
            types = 1 << b;
        if(types == MusEPlugin::PluginTypeNone)
        {
          done(QJsonObject(), QObject::tr("Unknown synth type \"%1\".").arg(type));
          return;
        }
      }
      Synth* synth = MusEGlobal::synthis.find(types, file, uri, label);
      if(!synth)
      {
        done(QJsonObject(), QObject::tr("There is no such synth. See list_synths."));
        return;
      }
      const QString name = args.value("name").toString().trimmed();
      if(!name.isEmpty() && MusEGlobal::song->findTrack(name))
      {
        done(QJsonObject(), QObject::tr("There is already a track named \"%1\".").arg(name));
        return;
      }
      SynthI* si = MusEGlobal::song->createSynthI(synth->pluginType(), synth->completeBaseName(), synth->uri(), synth->label());
      if(!si)
      {
        done(QJsonObject(), QObject::tr("The synth could not be created."));
        return;
      }
      if(MusEGlobal::config.unhideTracks)
        SynthI::setVisible(true);
      // Give the new instance the first free midi port, like the GUI does.
      for(int i = 0; i < MIDI_PORTS; ++i)
      {
        MidiPort* port = &MusEGlobal::midiPorts[i];
        if(!port->device())
        {
          MusEGlobal::audio->msgSetMidiDevice(port, si, si);
          MusEGlobal::muse->changeConfig(true);
          break;
        }
      }
      if(!name.isEmpty())
        MusEGlobal::song->applyOperation(UndoOp(UndoOp::ModifyTrackName, si, si->name(), name));
      done(trackJson(si, false), QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   move_tracks_to_folder
  //---------------------------------------------------------
  {
    McpTool t = controlTool("move_tracks_to_folder",
      QObject::tr("Puts tracks in a folder track, as its last tracks, or takes them out of their folders. Undoable. "
                  "Folders themselves can not be moved this way."),
      schema({{"tracks", trackListProp(QObject::tr("Names of the tracks to move."))},
              {"folder", prop("string", QObject::tr("Name of the folder track. If left out the tracks leave their folders."))}},
             {"tracks"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      FolderTrack* folder = nullptr;
      if(args.contains("folder") && !args.value("folder").toString().isEmpty())
      {
        Track* f = MusEGlobal::song->findTrack(args.value("folder").toString());
        if(!f || !f->isFolderTrack())
        {
          done(QJsonObject(), QObject::tr("\"%1\" is not a folder track.").arg(args.value("folder").toString()));
          return;
        }
        folder = static_cast<FolderTrack*>(f);
      }
      std::vector<Track*> v;
      for(const QJsonValue& n : args.value("tracks").toArray())
      {
        Track* tr = MusEGlobal::song->findTrack(n.toString());
        if(!tr || tr->isFolderTrack())
        {
          done(QJsonObject(), QObject::tr("\"%1\" is not an existing track that is not a folder.").arg(n.toString()));
          return;
        }
        v.push_back(tr);
      }
      if(v.empty())
      {
        done(QJsonObject(), QObject::tr("Give at least one track."));
        return;
      }
      MusEGlobal::song->moveTracksToFolder(v, folder);
      QJsonObject o;
      o["moved"] = int(v.size());
      o["folder"] = folder ? QJsonValue(folder->name()) : QJsonValue();
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   save_plugin_preset / load_plugin_preset
  //---------------------------------------------------------
  {
    McpTool t = controlTool("save_plugin_preset",
      QObject::tr("Saves the settings of an effect plugin as a preset file, in the user's preset directory. "
                  "An existing preset of the same name is only replaced with overwrite set."),
      schema({{"track", prop("string", QObject::tr("Audio track name."))},
              {"slot", prop("integer", QObject::tr("Effect slot, as in get_track."))},
              {"name", prop("string", QObject::tr("Name of the preset: letters, digits, space and _ - . ( ) +."))},
              {"overwrite", prop("boolean", QObject::tr("true to replace a preset of the same name."))}},
             {"track", "slot", "name"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, true, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      const int slot = args.value("slot").toInt(0) - 1;
      const PluginI* p = (slot >= 0 && slot < PipelineDepth) ? (*static_cast<AudioTrack*>(tr)->efxPipe())[slot] : nullptr;
      if(!p)
      {
        done(QJsonObject(), QObject::tr("There is no plugin in that slot."));
        return;
      }
      const QString name = args.value("name").toString().trimmed();
      const QString dirName = presetDirName(p->pluginLabel());
      if(!presetNameOk(name) || !presetNameOk(dirName))
      {
        done(QJsonObject(), QObject::tr("The preset name is not valid (letters, digits, space and _ - . ( ) +)."));
        return;
      }
      const QString dir = presetRoot(true) + "/" + dirName;
      const QString path = dir + "/" + name + ".pre";
      if(QFileInfo::exists(path) && !args.value("overwrite").toBool())
      {
        done(QJsonObject(), QObject::tr("A preset with that name exists. Set overwrite to replace it."));
        return;
      }
      if(!QDir().mkpath(dir))
      {
        done(QJsonObject(), QObject::tr("The preset directory could not be created."));
        return;
      }
      QFile f(path);
      if(!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
      {
        done(QJsonObject(), QObject::tr("The file could not be written."));
        return;
      }
      {
        Xml xml(&f);
        xml.header();
        xml.tag(0, "muse version=\"1.0\"");
        const_cast<PluginI*>(p)->writeConfiguration(1, xml);
        xml.tag(1, "/muse");
      }
      f.close();
      QJsonObject o;
      o["id"] = QString("user:") + dirName + "/" + name;
      o["file"] = path;
      done(o, QString());
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("load_plugin_preset",
      QObject::tr("Puts the plugin of a preset (with its settings) in an effect slot of an audio track, like the effect rack "
                  "does with a preset file. Undoable. A plugin in the slot is replaced, so the preset normally "
                  "belongs to the same plugin. Use an id from list_plugin_presets."),
      schema({{"track", prop("string", QObject::tr("Audio track name."))},
              {"slot", prop("integer", QObject::tr("Effect slot, the first is 1."))},
              {"preset", prop("string", QObject::tr("Preset id, like user:Reverb/Big hall."))}},
             {"track", "slot", "preset"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, true, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      AudioTrack* at = static_cast<AudioTrack*>(tr);
      const int slot = args.value("slot").toInt(0) - 1;
      if(slot < 0 || slot >= PipelineDepth)
      {
        done(QJsonObject(), QObject::tr("The slot must be 1 to %1.").arg(int(PipelineDepth)));
        return;
      }
      const QString file = presetFileFromId(args.value("preset").toString(), error);
      if(file.isEmpty())
      {
        done(QJsonObject(), error);
        return;
      }
      QFile f(file);
      if(!f.open(QIODevice::ReadOnly))
      {
        done(QJsonObject(), QObject::tr("The preset could not be read."));
        return;
      }
      const QByteArray ba = f.readAll();
      f.close();
      PluginI* newplug = initPluginFromXml(at, Xml(ba.constData()), slot);
      if(!newplug || !newplug->plugin())
      {
        if(newplug)
        {
          newplug->initialConfiguration()._ctrlListList.clearDelete();
          delete newplug;
        }
        done(QJsonObject(), QObject::tr("The plugin of the preset could not be created (is it installed?)."));
        return;
      }
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::ChangeRackEffectPlugin, at, newplug, slot));
      QJsonObject o;
      o["track"] = at->name();
      const PluginI* now = (*at->efxPipe())[slot];
      if(now)
        o["plugin"] = pluginJson(now, slot);
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   modify_midi_notes
  //---------------------------------------------------------
  {
    QJsonObject cp;
    cp["tick"] = prop("integer", QObject::tr("Start of the note now, in ticks from the start of the part."));
    cp["pitch"] = prop("integer", QObject::tr("Pitch of the note now."));
    cp["new_tick"] = prop("integer", QObject::tr("New start in ticks."));
    cp["new_pitch"] = prop("integer", QObject::tr("New pitch, 0-127."));
    cp["new_length_ticks"] = prop("integer", QObject::tr("New length in ticks."));
    cp["new_velocity"] = prop("integer", QObject::tr("New velocity, 1-127."));
    QJsonObject changes;
    changes["type"] = "array";
    changes["items"] = schema(cp, {"tick", "pitch"});
    changes["description"] = QObject::tr("The notes to change, at most 2000. Leave out what stays as it is.");
    McpTool t = controlTool("modify_midi_notes",
      QObject::tr("Changes notes of a midi part: their start, pitch, length or velocity. A note is found by its tick "
                  "and pitch (see get_midi_notes). Nothing is changed if any note is not found. One undo step."),
      schema({{"track", prop("string", QObject::tr("Track name."))},
              {"part_start_tick", prop("integer", QObject::tr("Start tick of the part, from list_parts."))},
              {"changes", changes}}, {"track", "part_start_tick", "changes"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Part* part = midiPartFromArgs(args, error);
      if(!part)
      {
        done(QJsonObject(), error);
        return;
      }
      const QJsonArray arr = args.value("changes").toArray();
      if(arr.isEmpty() || arr.size() > 2000)
      {
        done(QJsonObject(), QObject::tr("Give between 1 and 2000 changes."));
        return;
      }
      Undo group;
      std::vector<Event> used;
      for(const QJsonValue& v : arr)
      {
        const QJsonObject c = v.toObject();
        Event old;
        if(!findNote(part, c.value("tick").toInt(-1), c.value("pitch").toInt(-1), used, old))
        {
          done(QJsonObject(), QObject::tr("No note at tick %1 with pitch %2 (or it is used twice).")
                                .arg(c.value("tick").toInt(-1)).arg(c.value("pitch").toInt(-1)));
          return;
        }
        used.push_back(old);
        Event ne = old.clone();
        if(c.contains("new_tick"))
        {
          const int nt = c.value("new_tick").toInt(-1);
          if(nt < 0)
          {
            done(QJsonObject(), QObject::tr("new_tick must be 0 or more."));
            return;
          }
          ne.setTick(unsigned(nt));
        }
        if(c.contains("new_pitch"))
        {
          const int np = c.value("new_pitch").toInt(-1);
          if(np < 0 || np > 127)
          {
            done(QJsonObject(), QObject::tr("new_pitch must be 0-127."));
            return;
          }
          ne.setPitch(np);
        }
        if(c.contains("new_length_ticks"))
        {
          const int nl = c.value("new_length_ticks").toInt(0);
          if(nl < 1)
          {
            done(QJsonObject(), QObject::tr("new_length_ticks must be 1 or more."));
            return;
          }
          ne.setLenTick(unsigned(nl));
        }
        if(c.contains("new_velocity"))
        {
          const int nv = c.value("new_velocity").toInt(0);
          if(nv < 1 || nv > 127)
          {
            done(QJsonObject(), QObject::tr("new_velocity must be 1-127."));
            return;
          }
          ne.setVelo(nv);
        }
        group.push_back(UndoOp(UndoOp::ModifyEvent, ne, old, part, true, true));
      }
      MusEGlobal::song->applyOperationGroup(group);
      QJsonObject o;
      o["modified"] = arr.size();
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   delete_midi_notes
  //---------------------------------------------------------
  {
    QJsonObject np;
    np["tick"] = prop("integer", QObject::tr("Start of the note, in ticks from the start of the part."));
    np["pitch"] = prop("integer", QObject::tr("Pitch of the note."));
    QJsonObject notes;
    notes["type"] = "array";
    notes["items"] = schema(np, {"tick", "pitch"});
    notes["description"] = QObject::tr("Notes to erase by tick and pitch, at most 2000.");
    McpTool t = controlTool("delete_midi_notes",
      QObject::tr("Erases notes of a midi part. Either list notes (by tick and pitch, see get_midi_notes), or give a "
                  "range with from_tick and to_tick and optionally pitch_min and pitch_max to erase every note starting in it. "
                  "One undo step."),
      schema({{"track", prop("string", QObject::tr("Track name."))},
              {"part_start_tick", prop("integer", QObject::tr("Start tick of the part, from list_parts."))},
              {"notes", notes},
              {"from_tick", prop("integer", QObject::tr("Range: erase notes starting at or after this tick of the part."))},
              {"to_tick", prop("integer", QObject::tr("Range: and before this tick."))},
              {"pitch_min", prop("integer", QObject::tr("Range: lowest pitch, 0 if left out."))},
              {"pitch_max", prop("integer", QObject::tr("Range: highest pitch, 127 if left out."))}},
             {"track", "part_start_tick"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Part* part = midiPartFromArgs(args, error);
      if(!part)
      {
        done(QJsonObject(), error);
        return;
      }
      const bool byList = args.contains("notes");
      const bool byRange = args.contains("from_tick") && args.contains("to_tick");
      if(byList == byRange)
      {
        done(QJsonObject(), QObject::tr("Give either notes, or from_tick and to_tick."));
        return;
      }
      Undo group;
      std::vector<Event> used;
      if(byList)
      {
        const QJsonArray arr = args.value("notes").toArray();
        if(arr.isEmpty() || arr.size() > 2000)
        {
          done(QJsonObject(), QObject::tr("Give between 1 and 2000 notes."));
          return;
        }
        for(const QJsonValue& v : arr)
        {
          const QJsonObject n = v.toObject();
          Event old;
          if(!findNote(part, n.value("tick").toInt(-1), n.value("pitch").toInt(-1), used, old))
          {
            done(QJsonObject(), QObject::tr("No note at tick %1 with pitch %2 (or it is listed twice).")
                                  .arg(n.value("tick").toInt(-1)).arg(n.value("pitch").toInt(-1)));
            return;
          }
          used.push_back(old);
          group.push_back(UndoOp(UndoOp::DeleteEvent, old, part, true, true));
        }
      }
      else
      {
        const double from = args.value("from_tick").toDouble();
        const double to = args.value("to_tick").toDouble();
        const int pmin = args.value("pitch_min").toInt(0);
        const int pmax = args.value("pitch_max").toInt(127);
        for(ciEvent i = part->events().begin(); i != part->events().end(); ++i)
        {
          const Event& e = i->second;
          if(e.isNote() && double(e.tick()) >= from && double(e.tick()) < to && e.pitch() >= pmin && e.pitch() <= pmax)
            group.push_back(UndoOp(UndoOp::DeleteEvent, e, part, true, true));
        }
        if(group.empty())
        {
          done(QJsonObject(), QObject::tr("There are no notes in that range."));
          return;
        }
      }
      const int count = int(group.size());
      MusEGlobal::song->applyOperationGroup(group);
      QJsonObject o;
      o["erased"] = count;
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   set_tempo / set_time_signature / set_tempo_map_enabled
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_tempo",
      QObject::tr("Sets the tempo from a position on (default the start of the song). Undoable. If the tempo map is off "
                  "the song has one static tempo and only a tempo at the start can be set: Turn the tempo map on with "
                  "set_tempo_map_enabled for tempo changes later in the song."),
      schema({{"bpm", prop("number", QObject::tr("Beats (quarter notes) per minute, 20 to 400."))},
              {"tick", prop("integer", QObject::tr("Position in ticks. 0 if left out."))},
              {"bar", prop("integer", QObject::tr("Or the position as a bar number, the first bar is 1."))}}, {"bpm"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      const double bpm = args.value("bpm").toDouble(0.0);
      if(!(bpm >= 20.0 && bpm <= 400.0))
      {
        done(QJsonObject(), QObject::tr("bpm must be 20 to 400."));
        return;
      }
      unsigned tick = 0;
      if(args.contains("tick"))
        tick = unsigned(qMax(0, args.value("tick").toInt()));
      else if(args.contains("bar"))
        tick = MusEGlobal::sigmap.bar2tick(qMax(1, args.value("bar").toInt()) - 1, 0, 0);
      if(tick > 0 && !MusEGlobal::tempomap.masterFlag())
      {
        done(QJsonObject(), QObject::tr("The tempo map is off, so there is one static tempo. Use set_tempo_map_enabled first."));
        return;
      }
      const int tempo = int(std::lround(60000000.0 / bpm));
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::SetTempo, int(tick), tempo));
      QJsonObject o;
      o["bpm"] = round2(60000000.0 / tempo);
      o["tick"] = double(tick);
      done(o, QString());
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("set_time_signature",
      QObject::tr("Sets the time signature from a bar on. Undoable. Give the bar (the first is 1, the default)."),
      schema({{"numerator", prop("integer", QObject::tr("Beats per bar, 1 to 63."))},
              {"denominator", prop("integer", QObject::tr("Note value of a beat: 1, 2, 4, 8, 16, 32 or 64."))},
              {"bar", prop("integer", QObject::tr("Bar number where it starts. 1 if left out."))}}, {"numerator", "denominator"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      const int z = args.value("numerator").toInt(0);
      const int n = args.value("denominator").toInt(0);
      if(z < 1 || z > 63 || !(n == 1 || n == 2 || n == 4 || n == 8 || n == 16 || n == 32 || n == 64))
      {
        done(QJsonObject(), QObject::tr("The time signature is not valid."));
        return;
      }
      const int bar = qMax(1, args.value("bar").toInt(1));
      const unsigned tick = MusEGlobal::sigmap.bar2tick(bar - 1, 0, 0);
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::AddSig, int(tick), z, n));
      QJsonObject o;
      o["bar"] = bar;
      o["numerator"] = z;
      o["denominator"] = n;
      done(o, QString());
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("set_tempo_map_enabled",
      QObject::tr("Turns the tempo map on (the tempo can change during the song) or off (one static tempo for the whole "
                  "song). Not undoable, like in the GUI."),
      schema({{"enabled", prop("boolean", QObject::tr("true for the tempo map on."))}}, {"enabled"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      MusEGlobal::song->setMasterFlag(args.value("enabled").toBool());
      QJsonObject o;
      o["tempoMapEnabled"] = MusEGlobal::tempomap.masterFlag();
      done(o, QString());
    };
    server.addTool(t);
  }


  //---------------------------------------------------------
  //   add_marker / modify_marker / delete_marker
  //---------------------------------------------------------
  {
    QJsonObject props = positionProps(QObject::tr("The position"));
    props["name"] = prop("string", QObject::tr("Name of the marker, for example the section (Verse, Chorus)."));
    props["locked"] = prop("boolean", QObject::tr("true to lock the marker to its time, so it stays there when the "
                                                  "tempo changes. false (the default) keeps it at its bar and beat."));
    McpTool t = controlTool("add_marker",
      QObject::tr("Adds a marker at a position (tick, seconds, or bar and beat). Undoable. Several markers can have "
                  "the same name and position. For the range markers (left and right locators) see set_locators."),
      schema(props, {"name"}), McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      unsigned tick;
      QString error;
      if(!tickFromPosition(args, tick, error))
      {
        done(QJsonObject(), error);
        return;
      }
      Marker m(args.value("name").toString());
      m.setType(args.value("locked").toBool() ? Pos::FRAMES : Pos::TICKS);
      m.setTick(tick);
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::AddMarker, m));
      const MarkerList* ml = MusEGlobal::song->marker();
      for(ciMarker i = ml->cbegin(); i != ml->cend(); ++i)
        if(i->second.id() == m.id())
        {
          done(markerJson(i->second), QString());
          return;
        }
      done(QJsonObject(), QObject::tr("The marker was not added."));
    };
    server.addTool(t);
  }
  {
    QJsonObject props = positionProps(QObject::tr("The new position"));
    props["id"] = prop("integer", QObject::tr("Id of the marker, from list_markers."));
    props["name"] = prop("string", QObject::tr("Or the name of the marker, if no other marker has it."));
    props["newName"] = prop("string", QObject::tr("The new name."));
    props["locked"] = prop("boolean", QObject::tr("true to lock the marker to its time, false to keep it at its bar and beat."));
    McpTool t = controlTool("modify_marker",
      QObject::tr("Renames and/or moves a marker, or locks it to its time. Give the marker by id or name, and any of "
                  "newName, a new position (tick, seconds, or bar and beat) and locked. Undoable."),
      schema(props), McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      const Marker* old = markerFromArgs(args, error);
      if(!old)
      {
        done(QJsonObject(), error);
        return;
      }
      const bool move = hasPosition(args);
      if(!move && !args.contains("newName") && !args.contains("locked"))
      {
        done(QJsonObject(), QObject::tr("Give newName, a new position or locked."));
        return;
      }
      unsigned tick = old->tick();
      if(move && !tickFromPosition(args, tick, error))
      {
        done(QJsonObject(), error);
        return;
      }
      // A copy keeps the id.
      Marker m(*old);
      if(args.contains("newName"))
        m.setName(args.value("newName").toString());
      if(move)
        m.setTick(tick);
      if(args.contains("locked"))
        m.setType(args.value("locked").toBool() ? Pos::FRAMES : Pos::TICKS);
      const EventID_t id = m.id();
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::ModifyMarker, *old, m));
      const MarkerList* ml = MusEGlobal::song->marker();
      for(ciMarker i = ml->cbegin(); i != ml->cend(); ++i)
        if(i->second.id() == id)
        {
          done(markerJson(i->second), QString());
          return;
        }
      done(QJsonObject(), QObject::tr("The marker was not modified."));
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("delete_marker",
      QObject::tr("Deletes a marker, given by id or name. Undoable."),
      schema({{"id", prop("integer", QObject::tr("Id of the marker, from list_markers."))},
              {"name", prop("string", QObject::tr("Or the name of the marker, if no other marker has it."))}}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      const Marker* m = markerFromArgs(args, error);
      if(!m)
      {
        done(QJsonObject(), error);
        return;
      }
      QJsonObject o = markerJson(*m);
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::DeleteMarker, *m));
      o["deleted"] = true;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   set_midi_track_output
  //---------------------------------------------------------
  {
    McpTool t = controlTool("set_midi_track_output",
      QObject::tr("Sets the midi port and channel that a midi or drum track plays on, for example the port of a synth "
                  "(see list_midi_ports and get_track of the synth track). Not undoable, like in the GUI."),
      schema({{"track", prop("string", QObject::tr("Midi or drum track name."))},
              {"port", prop("integer", QObject::tr("Midi port number, from list_midi_ports."))},
              {"channel", prop("integer", QObject::tr("Midi channel 1-16. Left as it is if left out."))}}, {"track", "port"}),
      McpTierEdit);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, false, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      if(!tr->isMidiTrack())
      {
        done(QJsonObject(), QObject::tr("\"%1\" is not a midi or drum track.").arg(tr->name()));
        return;
      }
      MidiTrack* mt = static_cast<MidiTrack*>(tr);
      const int port = args.value("port").toInt(0) - 1;
      const int chan = args.contains("channel") ? args.value("channel").toInt(0) - 1 : mt->outChannel();
      if(port < 0 || port >= MIDI_PORTS || chan < 0 || chan >= MUSE_MIDI_CHANNELS)
      {
        done(QJsonObject(), QObject::tr("The port must be 1 to %1 and the channel 1 to 16.").arg(int(MIDI_PORTS)));
        return;
      }
      // Like the GUI does it.
      MusEGlobal::audio->msgIdle(true);
      const MidiTrack::ChangedType_t changed = mt->setOutPortAndChannelAndUpdate(port, chan, false);
      MusEGlobal::audio->msgIdle(false);
      MusEGlobal::audio->msgUpdateSoloStates();
      MusEGlobal::song->update(SC_ROUTE | ((changed & MidiTrack::DrumMapChanged) ? SC_DRUMMAP : 0));
      MusEGlobal::audio->msgInitMidiDevices(false);
      QJsonObject o;
      o["track"] = mt->name();
      o["port"] = mt->outPort() + 1;
      o["channel"] = mt->outChannel() + 1;
      done(o, QString());
    };
    server.addTool(t);
  }

  //=========================================================
  //   Dangerous tier
  //=========================================================

  //---------------------------------------------------------
  //   delete_track
  //---------------------------------------------------------
  {
    McpTool t = controlTool("delete_track",
      QObject::tr("Deletes a track with all its parts. Undoable, but a deleted track is only restored as it was in memory. "
                  "A folder can only be deleted when it is empty (move its tracks out first)."),
      schema({{"track", prop("string", QObject::tr("Track name."))}}, {"track"}),
      McpTierDangerous);
    t.handler = [](const QJsonObject& args, McpDone done) {
      const QString tname = args.value("track").toString();
      Track* tr = tname.isEmpty() ? nullptr : MusEGlobal::song->findTrack(tname);
      if(!tr)
      {
        done(QJsonObject(), QObject::tr("There is no track named \"%1\".").arg(tname));
        return;
      }
      // Song::applyOperationGroup would ask the user with a dialog, which would freeze the request.
      if(tr->isFolderTrack() && MusEGlobal::song->folderHasTracks(static_cast<const FolderTrack*>(tr)))
      {
        done(QJsonObject(), QObject::tr("The folder is not empty. Move its tracks out with move_tracks_to_folder first."));
        return;
      }
      const QString name = tr->name();
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::DeleteTrack, MusEGlobal::song->tracks()->index(tr), tr));
      QJsonObject o;
      o["deleted"] = name;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   delete_part
  //---------------------------------------------------------
  {
    McpTool t = controlTool("delete_part", QObject::tr("Deletes a part. Undoable."),
      schema({{"track", prop("string", QObject::tr("Track of the part."))},
              {"part_start_tick", prop("integer", QObject::tr("Start tick of the part, from list_parts."))}},
             {"track", "part_start_tick"}),
      McpTierDangerous);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Part* part = partFromArgs(args, "track", "part_start_tick", error);
      if(!part)
      {
        done(QJsonObject(), error);
        return;
      }
      const QString name = part->name();
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::DeletePart, part));
      QJsonObject o;
      o["deleted"] = name;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   remove_plugin
  //---------------------------------------------------------
  {
    McpTool t = controlTool("remove_plugin",
      QObject::tr("Removes an effect plugin from an audio track, with its settings. Undoable."),
      schema({{"track", prop("string", QObject::tr("Audio track name."))},
              {"slot", prop("integer", QObject::tr("Effect slot, as in get_track."))}}, {"track", "slot"}),
      McpTierDangerous);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      Track* tr = trackFromArgs(args, true, error);
      if(!tr)
      {
        done(QJsonObject(), error);
        return;
      }
      AudioTrack* at = static_cast<AudioTrack*>(tr);
      const int slot = args.value("slot").toInt(0) - 1;
      if(slot < 0 || slot >= PipelineDepth || !(*at->efxPipe())[slot])
      {
        done(QJsonObject(), QObject::tr("There is no plugin in that slot."));
        return;
      }
      const QString name = (*at->efxPipe())[slot]->name();
      MusEGlobal::song->applyOperation(UndoOp(UndoOp::ChangeRackEffectPlugin, at, (PluginI*)nullptr, slot));
      QJsonObject o;
      o["track"] = at->name();
      o["removed"] = name;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   save_song
  //---------------------------------------------------------
  {
    McpTool t = controlTool("save_song",
      QObject::tr("Saves the song to its current file, replacing it (the previous file is kept as .backup). "
                  "A song that has never been saved has to be saved in MusE first."),
      schema(), McpTierDangerous);
    t.handler = [](const QJsonObject&, McpDone done) {
      if(MusEGlobal::museProject == MusEGlobal::museProjectInitPath)
      {
        done(QJsonObject(), QObject::tr("The song has no file yet. Save it once in MusE."));
        return;
      }
      // save() is a private slot of the main window.
      bool ok = false;
      QMetaObject::invokeMethod(MusEGlobal::muse, "save", Qt::DirectConnection, Q_RETURN_ARG(bool, ok));
      if(!ok)
      {
        done(QJsonObject(), QObject::tr("Saving failed."));
        return;
      }
      QJsonObject o;
      o["saved"] = true;
      o["project"] = MusEGlobal::museProject;
      done(o, QString());
    };
    server.addTool(t);
  }

  //---------------------------------------------------------
  //   export_mixdown / export_stems / get_export_status / cancel_export
  //---------------------------------------------------------
  {
    QJsonObject props;
    addFormatProps(props);
    props["output"] = prop("string", QObject::tr("The audio output track to mix down. Needed if there is more than one."));
    props["path"] = prop("string", QObject::tr("In the project folder (relative to it, or absolute inside it). Range "
                                               "mode: the file, default downmix/<project>_downmix.<ext> (the extension "
                                               "of the format is added if missing). Markers mode: the folder, default "
                                               "downmix; the files are <project>_<marker>.<ext>, numbered instead of "
                                               "replacing a file."));
    McpTool t = controlTool("export_mixdown",
      QObject::tr("Renders the downmix of an output track to a file in the project folder, like Render Downmix to a "
                  "File, offline (faster than real time) using the transport. Starts it and answers at once: follow it "
                  "with get_export_status. The transport must be stopped. The song must have been saved once."),
      schema(props), McpTierDangerous);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      MixdownSettings s;
      AudioOutput* ao = nullptr;
      if(!(error = exportBusy()).isEmpty() || !mixdownSettingsFromArgs(args, s, error) ||
         !(ao = outputFromArgs(args, "output", error)))
      {
        done(QJsonObject(), error);
        return;
      }
      const QString project = exportProjectName();
      QString path = args.value("path").toString(s.markerMode ? QString("downmix")
                                                              : QString("downmix/%1_downmix").arg(project));
      if(!s.markerMode && QFileInfo(path).suffix().toLower() != s.extension())
        path += "." + s.extension();
      s.path = pathInProject(path, error);
      if(s.path.isEmpty())
      {
        done(QJsonObject(), error);
        return;
      }
      MarkerMixdown* mm = MarkerMixdown::begin(ao, s, project, args.value("overwrite").toBool(), &error);
      if(!mm)
      {
        done(QJsonObject(), error);
        return;
      }
      trackExport(mm, "mixdown", s.path);
      QJsonObject o = exportReportJson(mm->report(), true);
      o["output"] = ao->name();
      done(o, QString());
    };
    server.addTool(t);
  }
  {
    QJsonObject props;
    addFormatProps(props);
    props["tracks"] = trackListProp(QObject::tr("The tracks to export, one file each: wave, midi (the synth it plays), "
                                                "synth and group tracks."));
    props["throughOutput"] = prop("string", QObject::tr("Take each stem from this output track, after its effects and "
                                                        "fader, instead of from the track itself (post fader, with the "
                                                        "aux returns)."));
    props["path"] = prop("string", QObject::tr("The folder, in the project folder (relative to it, or absolute inside "
                                               "it). Default downmix/stems. Range mode: <project>_<track>.<ext>; markers "
                                               "mode: <marker>/<project>_<marker>_<track>.<ext>."));
    McpTool t = controlTool("export_stems",
      QObject::tr("Exports stems like Export Stems: one file per track (and per named marker in markers mode), each "
                  "rendered offline with the track and the aux tracks soloed; the solo states are put back afterwards. "
                  "Starts it and answers at once: follow it with get_export_status. The transport must be stopped. "
                  "The song must have been saved once."),
      schema(props, {"tracks"}), McpTierDangerous);
    t.handler = [](const QJsonObject& args, McpDone done) {
      QString error;
      MixdownSettings s;
      if(!(error = exportBusy()).isEmpty() || !mixdownSettingsFromArgs(args, s, error))
      {
        done(QJsonObject(), error);
        return;
      }
      AudioOutput* through = nullptr;
      if(args.contains("throughOutput") && !(through = outputFromArgs(args, "throughOutput", error)))
      {
        done(QJsonObject(), error);
        return;
      }
      std::vector<Track*> tracks;
      const QJsonArray names = args.value("tracks").toArray();
      for(const QJsonValue& v : names)
      {
        Track* tr = MusEGlobal::song->findTrack(v.toString());
        QString why;
        if(!tr)
          error = QObject::tr("There is no track named \"%1\".").arg(v.toString());
        else if(!StemExport::listed(tr) || !StemExport::source(tr, &why))
          error = QObject::tr("\"%1\" can not be exported: %2").arg(tr->name(),
                    why.isEmpty() ? QObject::tr("Only wave, midi, synth and group tracks can be exported as stems.") : why);
        else if(std::find(tracks.begin(), tracks.end(), tr) == tracks.end())
          tracks.push_back(tr);
        if(!error.isEmpty())
        {
          done(QJsonObject(), error);
          return;
        }
      }
      if(tracks.empty())
      {
        done(QJsonObject(), QObject::tr("Give the tracks to export."));
        return;
      }
      // In song order, like the dialog.
      std::vector<Track*> ordered;
      for(Track* tr : *MusEGlobal::song->tracks())
        if(std::find(tracks.begin(), tracks.end(), tr) != tracks.end())
          ordered.push_back(tr);
      s.path = pathInProject(args.value("path").toString("downmix/stems"), error);
      if(s.path.isEmpty())
      {
        done(QJsonObject(), error);
        return;
      }
      StemExport* se = StemExport::begin(s, ordered, through, exportProjectName(),
                                         args.value("overwrite").toBool(), &error);
      if(!se)
      {
        done(QJsonObject(), error);
        return;
      }
      trackExport(se, "stems", s.path);
      done(exportReportJson(se->report(), true), QString());
    };
    server.addTool(t);
  }
  {
    McpTool t;
    t.name = "get_export_status";
    t.description = QObject::tr("The state of the export started with export_mixdown or export_stems: running, the "
                                "job (file) it is at and its percentage, and the files written so far. When it is "
                                "finished: the files written, those that failed, and whether it was cancelled.");
    t.inputSchema = schema();
    t.handler = [](const QJsonObject&, McpDone done) {
      if(mcpExport.job)
        done(exportReportJson(mcpExport.report(), true), QString());
      else if(mcpExport.hasLast)
        done(exportReportJson(mcpExport.last, false), QString());
      else
        done(QJsonObject(), QObject::tr("No export has been started through MCP since MusE was started."));
    };
    server.addTool(t);
  }
  {
    McpTool t = controlTool("cancel_export",
      QObject::tr("Cancels the running export (started with export_mixdown or export_stems). The file being "
                  "rendered is incomplete (stems: removed); the others are not started."),
      schema());
    t.handler = [](const QJsonObject&, McpDone done) {
      if(!mcpExport.job)
      {
        done(QJsonObject(), QObject::tr("No export is running."));
        return;
      }
      mcpExport.cancel();
      QJsonObject o;
      o["cancelling"] = true;
      o["id"] = mcpExport.id;
      done(o, QString());
    };
    server.addTool(t);
  }
}

} // namespace MusEGui
