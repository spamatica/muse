//=========================================================
//  MusE
//  Linux Music Editor
//
//  mixerconfig.cpp
//  See mixerconfig.h.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "mixerconfig.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QMap>

#include <map>

#include "audio.h"
#include "audiodev.h"
#include "globals.h"
#include "hex_float.h"
#include "plugin.h"
#include "route.h"
#include "song.h"
#include "undo.h"
#include "xml.h"

namespace MusECore {

static QString tr(const char* s)
{
  return QCoreApplication::translate("MixerConfig", s);
}

//---------------------------------------------------------
//   MixerTrackConfig::categories
//---------------------------------------------------------

unsigned MixerTrackConfig::categories() const
{
  unsigned c = 0;
  if(hasFader)
    c |= MixerFader;
  if(hasFlags)
    c |= MixerFlags;
  if(hasPrefader)
    c |= MixerPrefader;
  if(!auxSends.empty())
    c |= MixerAuxSends;
  if(!plugins.empty())
  {
    c |= MixerPlugins;
    for(const MixerPluginConfig& p : plugins)
      if(p.hasAutomation)
        c |= MixerPluginAutomation;
  }
  if(!routes.empty())
    c |= MixerRouting;
  for(const MixerDeviceRoute& r : deviceRoutes)
    c |= r.input ? MixerPortInputs : MixerPortOutputs;
  return c;
}

int MixerImportReport::count(MixerImportReportItem::Result r) const
{
  int n = 0;
  for(const MixerImportReportItem& i : items)
    if(i.result == r)
      ++n;
  return n;
}

//---------------------------------------------------------
//   The types of tracks and their elements in the files
//---------------------------------------------------------

static const char* wrapperTag(Track::TrackType type)
{
  switch(type)
  {
    case Track::WAVE:            return "wavetrack";
    case Track::AUDIO_INPUT:     return "AudioInput";
    case Track::AUDIO_OUTPUT:    return "AudioOutput";
    case Track::AUDIO_GROUP:     return "AudioGroup";
    case Track::AUDIO_AUX:       return "AudioAux";
    case Track::AUDIO_SOFTSYNTH: return "SynthI";
    default:                     return nullptr;
  }
}

// The audio track type of an element in a song file, or false if it is not one.
static bool audioTypeOfTag(const QString& tag, Track::TrackType& type)
{
  static const Track::TrackType types[] =
    { Track::WAVE, Track::AUDIO_INPUT, Track::AUDIO_OUTPUT, Track::AUDIO_GROUP, Track::AUDIO_AUX, Track::AUDIO_SOFTSYNTH };
  for(Track::TrackType t : types)
    if(tag == wrapperTag(t))
    {
      type = t;
      return true;
    }
  return false;
}

static bool isOtherTrackTag(const QString& tag)
{
  return tag == "miditrack" || tag == "drumtrack" || tag == "newdrumtrack" || tag == "FolderTrack";
}

//---------------------------------------------------------
//   Writing
//---------------------------------------------------------

bool MixerSnapshot::saveSong(QIODevice* dev, QString* error)
{
  if(!dev || !MusEGlobal::song)
  {
    if(error)
      *error = tr("No file to write to.");
    return false;
  }

  Xml xml(dev);
  xml.header();
  int level = xml.putFileVersion(0);
  xml.tag(level++, "mixerconfig");

  const TrackList* tl = MusEGlobal::song->tracks();
  const AuxList* auxs = MusEGlobal::song->auxs();
  for(ciTrack it = tl->begin(); it != tl->end(); ++it)
  {
    const Track* t = *it;
    const char* tag = wrapperTag(t->type());
    if(!t->isAudioTrack() || !tag)
      continue;
    const AudioTrack* at = static_cast<const AudioTrack*>(t);

    xml.tag(level++, tag);

    // The settings, the effect plugins and the automation. The same as in the song file.
    at->writeProperties(level, xml);

    // The aux sends in the song file are by the number of the aux track, which depends on the order of the
    //  aux tracks. Write them with the names of the aux tracks too.
    if(at->hasAuxSend())
    {
      xml.tag(level++, "auxSends");
      for(unsigned i = 0; i < auxs->size() && i < at->getAuxSendValueList()->size(); ++i)
        xml.emptyTag(level, QString("send aux=\"%1\" value=\"%2\"")
                              .arg(Xml::xmlString((*auxs)[i]->name())).arg((*at->getAuxSendValueList())[i], 0, 'g', 12));
      xml.etag(--level, "auxSends");
    }

    // Routes to other audio tracks, and to and from audio ports.
    const RouteList* orl = at->outRoutes();
    for(ciRoute r = orl->begin(); r != orl->end(); ++r)
    {
      if(r->type == Route::TRACK_ROUTE && r->track && r->track->isAudioTrack())
        xml.emptyTag(level, QString("mixerRoute dst=\"%1\" sch=\"%2\" dch=\"%3\" chs=\"%4\"")
                              .arg(Xml::xmlString(r->track->name())).arg(r->channel).arg(r->remoteChannel).arg(r->channels));
      else if(r->type == Route::JACK_ROUTE && t->type() == Track::AUDIO_OUTPUT && !r->name().isEmpty())
        xml.emptyTag(level, QString("deviceRoute port=\"%1\" ch=\"%2\" in=\"0\"")
                              .arg(Xml::xmlString(r->name())).arg(r->channel));
    }
    if(t->type() == Track::AUDIO_INPUT)
    {
      const RouteList* irl = at->inRoutes();
      for(ciRoute r = irl->begin(); r != irl->end(); ++r)
        if(r->type == Route::JACK_ROUTE && !r->name().isEmpty())
          xml.emptyTag(level, QString("deviceRoute port=\"%1\" ch=\"%2\" in=\"1\"")
                                .arg(Xml::xmlString(r->name())).arg(r->channel));
    }

    xml.etag(--level, tag);
  }

  xml.etag(--level, "mixerconfig");
  xml.etag(--level, "muse");
  return true;
}

//---------------------------------------------------------
//   Reading: Helpers
//---------------------------------------------------------

// Skips the rest of an element whose start tag has been read.
static void skipElement(Xml& xml)
{
  int depth = 1;
  while(depth > 0)
  {
    const Xml::Token token = xml.parse();
    if(token == Xml::Error || token == Xml::End)
      return;
    if(token == Xml::TagStart)
      ++depth;
    else if(token == Xml::TagEnd)
      --depth;
  }
}

// Reads the attributes and the text of an element whose start tag has been read, and skips any elements in it.
static void readElement(Xml& xml, QMap<QString, QString>& attrs, QString* text = nullptr)
{
  int depth = 1;
  while(depth > 0)
  {
    const Xml::Token token = xml.parse();
    if(token == Xml::Error || token == Xml::End)
      return;
    switch(token)
    {
      case Xml::Attribut:
        if(depth == 1)
          attrs[xml.s1()] = xml.s2();
      break;
      case Xml::Text:
        if(text && depth == 1)
          *text += xml.s1();
      break;
      case Xml::TagStart:
        ++depth;
      break;
      case Xml::TagEnd:
        --depth;
      break;
      default:
      break;
    }
  }
}

// Reads an element whose start tag has been read, and returns it as XML.
static QString captureElement(Xml& xml, const QString& startTag)
{
  QString out = QString("<%1").arg(startTag);
  bool open = true;
  int depth = 1;
  const auto closeOpen = [&]() {
    if(open)
    {
      out += ">";
      open = false;
    }
  };
  while(depth > 0)
  {
    const Xml::Token token = xml.parse();
    switch(token)
    {
      case Xml::Error:
      case Xml::End:
        return QString();
      case Xml::Attribut:
        if(open)
          out += QString(" %1=\"%2\"").arg(xml.s1(), Xml::xmlString(xml.s2()));
      break;
      case Xml::TagStart:
        closeOpen();
        out += QString("<%1").arg(xml.s1());
        open = true;
        ++depth;
      break;
      case Xml::Text:
        closeOpen();
        out += Xml::xmlString(xml.s1());
      break;
      case Xml::TagEnd:
        closeOpen();
        out += QString("</%1>").arg(xml.s1());
        --depth;
      break;
      default:
      break;
    }
  }
  return out;
}

// Gets what the plugin is from its XML. Returns false if it is not a plugin.
static bool readPluginInfo(const QString& pluginXml, MixerPluginConfig& pc)
{
  const QByteArray ba = pluginXml.toUtf8();
  Xml xml(ba.constData());
  Xml::Token t = xml.parse();
  if(t != Xml::TagStart || xml.s1() != "plugin")
    return false;
  PluginConfiguration conf;
  bool done = false;
  while(!done)
  {
    const Xml::Token token = xml.parse();
    const QString tag = xml.s1();
    switch(token)
    {
      case Xml::Error:
      case Xml::End:
        done = true;
      break;
      case Xml::TagStart:
        // Does the configuration recognize the element?
        if(!conf.readProperties(xml, token))
          xml.unknown("MixerConfig");
      break;
      case Xml::Attribut:
        conf.readProperties(xml, token);
      break;
      case Xml::TagEnd:
        if(tag == "plugin")
          done = true;
      break;
      default:
      break;
    }
  }
  pc.slot = conf._id;
  pc.type = int(conf._pluginType);
  pc.file = conf._file;
  pc.uri = conf._uri;
  pc.label = conf._pluginLabel;
  pc.name = !conf._name.isEmpty() ? conf._name : conf._pluginLabel;
  pc.hasAutomation = !conf._ctrlListList.empty();
  // The automation was only read to know about it.
  conf._ctrlListList.clearDelete();
  return true;
}

//---------------------------------------------------------
//   Reading: The tracks
//---------------------------------------------------------

namespace {

struct RawRoute
{
  QString srcName, dstName;
  int srcIdx = -1, dstIdx = -1;
  QString srcPort, dstPort;     // Audio ports.
  int ch = -1, chs = -1, remch = -1;
};

struct ParseState
{
  std::vector<MixerTrackConfig> configs;
  // The names of all tracks in the order of the file, so that song file routes can be found by track number.
  std::vector<QString> nameOrder;
  std::vector<QString> auxNames;
  // The aux sends of the song file: By the number of the aux track.
  std::map<QString, std::map<int, double> > auxByIndex;
  // The aux sends with the names of the aux tracks (mixer configuration file).
  std::map<QString, bool> auxNamed;
  std::vector<RawRoute> rawRoutes;
};

} // anonymous namespace

static void readTrackProps(Xml& xml, MixerTrackConfig& cfg, ParseState& st)
{
  for(;;)
  {
    const Xml::Token token = xml.parse();
    const QString tag = xml.s1();
    switch(token)
    {
      case Xml::Error:
      case Xml::End:
        return;
      case Xml::TagStart:
        if(tag == "Track")
        {
          // The name, the channels, mute, solo and off. The rest is skipped.
          for(;;)
          {
            const Xml::Token t2 = xml.parse();
            const QString tag2 = xml.s1();
            if(t2 == Xml::Error || t2 == Xml::End)
              return;
            if(t2 == Xml::TagEnd && tag2 == "Track")
              break;
            if(t2 != Xml::TagStart)
              continue;
            if(tag2 == "name")
              cfg.name = xml.parse1();
            else if(tag2 == "channels")
              cfg.channels = xml.parseInt();
            else if(tag2 == "mute")
            {
              cfg.mute = xml.parseInt();
              cfg.hasFlags = true;
            }
            else if(tag2 == "solo")
            {
              cfg.solo = xml.parseInt();
              cfg.hasFlags = true;
            }
            else if(tag2 == "off")
            {
              cfg.off = xml.parseInt();
              cfg.hasFlags = true;
            }
            else
              skipElement(xml);
          }
        }
        else if(tag == "prefader")
        {
          cfg.prefader = xml.parseInt();
          cfg.hasPrefader = true;
        }
        else if(tag == "gain")
          cfg.gain = MusELib::museStringToDouble(xml.parse1().simplified());
        else if(tag == "auxSend")
        {
          QMap<QString, QString> attrs;
          QString text;
          readElement(xml, attrs, &text);
          st.auxByIndex[cfg.name][attrs.value("idx", "0").toInt()] = text.toDouble();
        }
        else if(tag == "plugin")
        {
          const QString px = captureElement(xml, tag);
          MixerPluginConfig pc;
          if(!px.isEmpty() && readPluginInfo(px, pc))
          {
            pc.xml = px;
            // The plugin windows are not to open.
            pc.xml.replace("<guiVisible>1</guiVisible>", "<guiVisible>0</guiVisible>");
            pc.xml.replace("<nativeGuiVisible>1</nativeGuiVisible>", "<nativeGuiVisible>0</nativeGuiVisible>");
            cfg.plugins.push_back(pc);
          }
        }
        else if(tag == "controller")
        {
          // The volume and the pan, the rest is automation.
          QMap<QString, QString> attrs;
          readElement(xml, attrs);
          const int id = attrs.value("id", "-1").toInt();
          if(attrs.contains("cur"))
          {
            if(id == AC_VOLUME)
            {
              cfg.volume = MusELib::museStringToDouble(attrs.value("cur"));
              cfg.hasFader = true;
            }
            else if(id == AC_PAN)
            {
              cfg.pan = MusELib::museStringToDouble(attrs.value("cur"));
              cfg.hasFader = true;
            }
          }
        }
        else
          skipElement(xml);
      break;
      case Xml::TagEnd:
        if(tag == "AudioTrack")
          return;
      break;
      default:
      break;
    }
  }
}

static void readTrackBlock(Xml& xml, const QString& wrapper, MixerTrackConfig& cfg, ParseState& st)
{
  std::vector<std::pair<QString, double> > named;
  for(;;)
  {
    const Xml::Token token = xml.parse();
    const QString tag = xml.s1();
    switch(token)
    {
      case Xml::Error:
      case Xml::End:
        return;
      case Xml::TagStart:
        if(tag == "AudioTrack")
          readTrackProps(xml, cfg, st);
        else if(tag == "auxSends")
        {
          // Mixer configuration file: The aux sends by name.
          for(;;)
          {
            const Xml::Token t2 = xml.parse();
            const QString tag2 = xml.s1();
            if(t2 == Xml::Error || t2 == Xml::End || (t2 == Xml::TagEnd && tag2 == "auxSends"))
              break;
            if(t2 == Xml::TagStart && tag2 == "send")
            {
              QMap<QString, QString> attrs;
              readElement(xml, attrs);
              named.push_back(std::make_pair(attrs.value("aux"), attrs.value("value").toDouble()));
            }
            else if(t2 == Xml::TagStart)
              skipElement(xml);
          }
        }
        else if(tag == "mixerRoute")
        {
          QMap<QString, QString> attrs;
          readElement(xml, attrs);
          MixerTrackRoute r;
          r.dstTrack = attrs.value("dst");
          r.srcChannel = attrs.value("sch", "-1").toInt();
          r.dstChannel = attrs.value("dch", "-1").toInt();
          r.channels = attrs.value("chs", "-1").toInt();
          cfg.routes.push_back(r);
        }
        else if(tag == "deviceRoute")
        {
          QMap<QString, QString> attrs;
          readElement(xml, attrs);
          MixerDeviceRoute r;
          r.port = attrs.value("port");
          r.channel = attrs.value("ch", "-1").toInt();
          r.input = attrs.value("in", "0").toInt() != 0;
          cfg.deviceRoutes.push_back(r);
        }
        else
          // The parts, the synth, the folder... Not for the mixer.
          skipElement(xml);
      break;
      case Xml::TagEnd:
        if(tag == wrapper)
        {
          if(!named.empty())
          {
            cfg.auxSends = named;
            st.auxNamed[cfg.name] = true;
          }
          return;
        }
      break;
      default:
      break;
    }
  }
}

// A track that is not an audio track. Only the name is needed, for the track numbers in the routes.
static QString readOtherTrackName(Xml& xml, const QString& wrapper)
{
  QString name;
  for(;;)
  {
    const Xml::Token token = xml.parse();
    const QString tag = xml.s1();
    if(token == Xml::Error || token == Xml::End || (token == Xml::TagEnd && tag == wrapper))
      return name;
    if(token != Xml::TagStart)
      continue;
    if(tag != "Track")
    {
      skipElement(xml);
      continue;
    }
    for(;;)
    {
      const Xml::Token t2 = xml.parse();
      const QString tag2 = xml.s1();
      if(t2 == Xml::Error || t2 == Xml::End || (t2 == Xml::TagEnd && tag2 == "Track"))
        break;
      if(t2 != Xml::TagStart)
        continue;
      if(tag2 == "name")
        name = xml.parse1();
      else
        skipElement(xml);
    }
  }
}

// A route of a song file. Only what is of use is kept.
static void readSongRoute(Xml& xml, ParseState& st)
{
  RawRoute r;
  for(;;)
  {
    const Xml::Token token = xml.parse();
    const QString tag = xml.s1();
    switch(token)
    {
      case Xml::Error:
      case Xml::End:
        return;
      case Xml::Attribut:
        if(tag == "channel")
          r.ch = xml.s2().toInt();
        else if(tag == "channels")
          r.chs = xml.s2().toInt();
        else if(tag == "remch")
          r.remch = xml.s2().toInt();
      break;
      case Xml::TagStart:
        if(tag == "source" || tag == "dest")
        {
          const bool src = tag == "source";
          QMap<QString, QString> attrs;
          readElement(xml, attrs);
          const int type = attrs.value("type", "0").toInt();
          const bool isPort = type == int(Route::JACK_ROUTE);
          if(isPort)
            (src ? r.srcPort : r.dstPort) = attrs.value("name");
          else if(type == int(Route::TRACK_ROUTE))
          {
            if(attrs.contains("track"))
              (src ? r.srcIdx : r.dstIdx) = attrs.value("track").toInt();
            else
              (src ? r.srcName : r.dstName) = attrs.value("name");
          }
          else
            // Midi: Not for the mixer. Mark the route so that it is not used.
            r.ch = -2;
        }
        else
          skipElement(xml);
      break;
      case Xml::TagEnd:
        if(tag == "Route")
        {
          if(r.ch != -2)
            st.rawRoutes.push_back(r);
          return;
        }
      break;
      default:
      break;
    }
  }
}

static void readContainer(Xml& xml, const QString& container, ParseState& st, bool isSong)
{
  for(;;)
  {
    const Xml::Token token = xml.parse();
    const QString tag = xml.s1();
    switch(token)
    {
      case Xml::Error:
      case Xml::End:
        return;
      case Xml::TagStart:
      {
        Track::TrackType type;
        if(audioTypeOfTag(tag, type))
        {
          MixerTrackConfig cfg;
          cfg.type = type;
          readTrackBlock(xml, tag, cfg, st);
          st.nameOrder.push_back(cfg.name);
          if(type == Track::AUDIO_AUX)
            st.auxNames.push_back(cfg.name);
          st.configs.push_back(cfg);
        }
        else if(isOtherTrackTag(tag))
          st.nameOrder.push_back(readOtherTrackName(xml, tag));
        else if(isSong && tag == "Route")
          readSongRoute(xml, st);
        else
          skipElement(xml);
      }
      break;
      case Xml::TagEnd:
        if(tag == container)
          return;
      break;
      default:
      break;
    }
  }
}

bool MixerSnapshot::load(QIODevice* dev, QString* error)
{
  tracks.clear();
  if(!dev)
  {
    if(error)
      *error = tr("No file to read from.");
    return false;
  }

  Xml xml(dev);
  ParseState st;
  bool found = false;
  bool done = false;
  while(!done)
  {
    const Xml::Token token = xml.parse();
    const QString tag = xml.s1();
    switch(token)
    {
      case Xml::Error:
      case Xml::End:
        done = true;
      break;
      case Xml::TagStart:
        if(tag == "muse")
          break;
        if(tag == "song" || tag == "mixerconfig")
        {
          readContainer(xml, tag, st, tag == "song");
          found = true;
        }
        else
          skipElement(xml);
      break;
      case Xml::TagEnd:
        if(tag == "muse")
          done = true;
      break;
      default:
      break;
    }
  }

  if(!found)
  {
    if(error)
      *error = tr("The file is not a mixer configuration file or a song file.");
    return false;
  }

  tracks = st.configs;
  const auto indexOf = [this](const QString& name) -> int {
    for(size_t i = 0; i < tracks.size(); ++i)
      if(tracks[i].name == name)
        return int(i);
    return -1;
  };
  const auto nameOfIndex = [&st](int idx) -> QString {
    return (idx >= 0 && idx < int(st.nameOrder.size())) ? st.nameOrder[idx] : QString();
  };

  // The aux sends of song files: The number of the aux track is its place among the aux tracks.
  for(MixerTrackConfig& t : tracks)
  {
    const auto it = st.auxByIndex.find(t.name);
    if(it == st.auxByIndex.end() || st.auxNamed[t.name])
      continue;
    for(const auto& sv : it->second)
      if(sv.first >= 0 && sv.first < int(st.auxNames.size()))
        t.auxSends.push_back(std::make_pair(st.auxNames[sv.first], sv.second));
  }

  // The routes of song files. (The routes of mixer configuration files are in the tracks.)
  for(const RawRoute& r : st.rawRoutes)
  {
    const QString src = !r.srcName.isEmpty() ? r.srcName : nameOfIndex(r.srcIdx);
    const QString dst = !r.dstName.isEmpty() ? r.dstName : nameOfIndex(r.dstIdx);
    if(!r.srcPort.isEmpty() && !dst.isEmpty())
    {
      // From an audio port to a track: Audio input.
      const int ti = indexOf(dst);
      if(ti >= 0 && tracks[ti].type == Track::AUDIO_INPUT)
      {
        MixerDeviceRoute d;
        d.port = r.srcPort;
        d.channel = r.ch;
        d.input = true;
        tracks[ti].deviceRoutes.push_back(d);
      }
    }
    else if(!r.dstPort.isEmpty() && !src.isEmpty())
    {
      const int ti = indexOf(src);
      if(ti >= 0 && tracks[ti].type == Track::AUDIO_OUTPUT)
      {
        MixerDeviceRoute d;
        d.port = r.dstPort;
        d.channel = r.ch;
        d.input = false;
        tracks[ti].deviceRoutes.push_back(d);
      }
    }
    else if(!src.isEmpty() && !dst.isEmpty())
    {
      const int si = indexOf(src);
      const int di = indexOf(dst);
      // Only between audio tracks.
      if(si >= 0 && di >= 0)
      {
        MixerTrackRoute tr;
        tr.dstTrack = dst;
        // As Song::readRoute() understands them.
        tr.srcChannel = r.ch;
        tr.dstChannel = r.chs > 0 ? r.remch : r.ch;
        tr.channels = r.chs;
        tracks[si].routes.push_back(tr);
      }
    }
  }
  return true;
}

//---------------------------------------------------------
//   Importing
//---------------------------------------------------------

AudioTrack* MixerSnapshot::findTarget(const MixerTrackConfig& config)
{
  if(!MusEGlobal::song)
    return nullptr;
  const TrackList* tl = MusEGlobal::song->tracks();
  for(ciTrack it = tl->begin(); it != tl->end(); ++it)
    if((*it)->isAudioTrack() && (*it)->type() == config.type && (*it)->name() == config.name)
      return static_cast<AudioTrack*>(*it);
  return nullptr;
}

void MixerSnapshot::apply(const std::vector<MixerImportChoice>& choices, bool dryRun, MixerImportReport& report) const
{
  report = MixerImportReport();
  Song* song = MusEGlobal::song;
  if(!song)
    return;

  // The track of this song that each track of the file goes to, for the routes.
  std::map<QString, AudioTrack*> targetOf;
  for(const MixerImportChoice& c : choices)
    if(c.source >= 0 && c.source < int(tracks.size()) && c.target)
      targetOf[tracks[c.source].name] = c.target;

  Undo ops;

  const auto add = [&report](MixerImportReportItem::Result result, const QString& track, const QString& what,
                             const QString& note = QString()) {
    MixerImportReportItem item;
    item.result = result;
    item.track = track;
    item.what = what;
    item.note = note;
    report.items.push_back(item);
  };

  for(const MixerImportChoice& c : choices)
  {
    if(c.source < 0 || c.source >= int(tracks.size()) || c.categories == 0)
      continue;
    const MixerTrackConfig& src = tracks[c.source];
    ++report.tracksRequested;

    if(!c.target)
    {
      add(MixerImportReportItem::Deviation, src.name, tr("Everything"), tr("There is no track here to import to."));
      continue;
    }
    if(!song->trackExists(c.target))
    {
      add(MixerImportReportItem::Deviation, src.name, tr("Everything"), tr("The track is gone."));
      continue;
    }
    ++report.tracksMatched;
    AudioTrack* t = c.target;
    const QString label = t->name() == src.name ? src.name : QString("%1 -> %2").arg(src.name, t->name());

    if(src.channels > 0 && src.channels != t->channels())
      add(MixerImportReportItem::Deviation, label, tr("Channels"),
          tr("The track has %1 channel(s) in the file and %2 here. Settings and plugins were still imported.")
            .arg(src.channels).arg(t->channels()));

    //----------------------------------
    // Fader, pan and gain
    //----------------------------------
    if((c.categories & MixerFader) && src.hasFader)
    {
      if(!dryRun)
      {
        t->setVolume(src.volume);
        t->setPan(src.pan);
        t->setGain(src.gain);
      }
      add(MixerImportReportItem::Done, label, tr("Fader, pan and gain"));
    }

    //----------------------------------
    // Mute, solo and off
    //----------------------------------
    if((c.categories & MixerFlags) && src.hasFlags)
    {
      if(!dryRun)
      {
        if(t->mute() != src.mute)
          ops.push_back(UndoOp(UndoOp::SetTrackMute, t, double(src.mute), double(0), double(0), double(0), double(0)));
        if(t->solo() != src.solo)
          ops.push_back(UndoOp(UndoOp::SetTrackSolo, t, double(src.solo), double(0), double(0), double(0), double(0)));
        if(t->off() != src.off)
          ops.push_back(UndoOp(UndoOp::SetTrackOff, t, double(src.off), double(0), double(0), double(0), double(0)));
      }
      add(MixerImportReportItem::Done, label, tr("Mute, solo and off"));
    }

    //----------------------------------
    // Prefader
    //----------------------------------
    if((c.categories & MixerPrefader) && src.hasPrefader)
    {
      if(!dryRun && t->prefader() != src.prefader)
        MusEGlobal::audio->msgSetPrefader(t, src.prefader);
      add(MixerImportReportItem::Done, label, tr("Prefader"));
    }

    //----------------------------------
    // Aux sends
    //----------------------------------
    if((c.categories & MixerAuxSends) && !src.auxSends.empty())
    {
      int done = 0;
      for(const std::pair<QString, double>& send : src.auxSends)
      {
        int idx = -1;
        const AuxList* auxs = song->auxs();
        for(unsigned i = 0; i < auxs->size(); ++i)
          if((*auxs)[i]->name() == send.first)
          {
            idx = int(i);
            break;
          }
        if(idx < 0)
          add(MixerImportReportItem::Deviation, label, tr("Aux send to %1").arg(send.first),
              tr("There is no aux track with that name here."));
        else if(!t->hasAuxSend())
          add(MixerImportReportItem::Deviation, label, tr("Aux send to %1").arg(send.first),
              tr("The track has no aux sends."));
        else
        {
          if(!dryRun)
            MusEGlobal::audio->msgSetAux(t, idx, send.second);
          ++done;
        }
      }
      if(done > 0)
        add(MixerImportReportItem::Done, label, tr("Aux sends (%1)").arg(done));
    }

    //----------------------------------
    // Effect plugins
    //----------------------------------
    if((c.categories & MixerPlugins) && !src.plugins.empty())
    {
      for(size_t k = 0; k < src.plugins.size(); ++k)
      {
        if(!c.plugins.empty() && (k >= c.plugins.size() || !c.plugins[k]))
          continue;
        const MixerPluginConfig& p = src.plugins[k];
        const QString what = tr("Plugin %1 in slot %2").arg(p.name).arg(p.slot + 1);
        if(p.slot < 0 || p.slot >= PipelineDepth)
        {
          add(MixerImportReportItem::Deviation, label, what, tr("The rack position is not valid."));
          continue;
        }
        // Old song files have no plugin type.
        const MusEPlugin::PluginTypes_t ptypes = p.type == MusEPlugin::PluginTypeNone ? int(MusEPlugin::PluginTypesAll) : p.type;
        if(!MusEGlobal::plugins.find(ptypes, p.file, p.uri, p.label))
        {
          add(MixerImportReportItem::Deviation, label, what, tr("The plugin is not available here."));
          continue;
        }
        const PluginI* existing = (*t->efxPipe())[p.slot];
        const QString replaced = existing ? tr("It replaced the plugin %1.").arg(existing->name()) : QString();
        if(dryRun)
        {
          add(MixerImportReportItem::Done, label, what, replaced);
          continue;
        }

        const bool withAutomation = (c.categories & MixerPluginAutomation) && p.hasAutomation;
        CtrlListList* cll = withAutomation ? new CtrlListList() : nullptr;
        MidiAudioCtrlMap* macm = new MidiAudioCtrlMap();
        const QByteArray ba = QString("<muse version=\"1.0\">\n%1\n</muse>\n").arg(p.xml).toUtf8();
        PluginI* newplug = initPluginFromXml(t, Xml(ba.constData()), p.slot, cll, macm);
        if(!newplug || !newplug->plugin())
        {
          if(newplug)
          {
            newplug->initialConfiguration()._ctrlListList.clearDelete();
            delete newplug;
          }
          if(cll)
          {
            cll->clearDelete();
            delete cll;
          }
          delete macm;
          add(MixerImportReportItem::Deviation, label, what, tr("The plugin could not be created."));
          continue;
        }
        if(cll && cll->empty())
        {
          delete cll;
          cll = nullptr;
        }
        if(macm->empty())
        {
          delete macm;
          macm = nullptr;
        }
        if(cll)
          newplug->setupControllers(cll);
        ops.push_back(UndoOp(UndoOp::ChangeRackEffectPlugin, t, newplug, p.slot, cll, macm));
        add(MixerImportReportItem::Done, label, what, replaced);
      }
    }

    //----------------------------------
    // Routing
    //----------------------------------
    if(c.categories & MixerRouting)
    {
      for(const MixerTrackRoute& r : src.routes)
      {
        const QString what = tr("Route to %1").arg(r.dstTrack);
        AudioTrack* dst = nullptr;
        const auto tit = targetOf.find(r.dstTrack);
        if(tit != targetOf.end())
          dst = tit->second;
        else
        {
          // Not one of the tracks that are imported: The track of the same name here, if any.
          MixerTrackConfig probe;
          for(const MixerTrackConfig& other : tracks)
            if(other.name == r.dstTrack)
              probe = other;
          probe.name = r.dstTrack;
          dst = findTarget(probe);
        }
        if(!dst)
        {
          add(MixerImportReportItem::Deviation, label, what, tr("There is no such track here."));
          continue;
        }
        const Route s(t, r.srcChannel, r.channels);
        const Route d(dst, r.dstChannel, r.channels);
        bool exists = false;
        const RouteList* orl = t->outRoutes();
        for(ciRoute ro = orl->begin(); ro != orl->end(); ++ro)
          if(ro->type == Route::TRACK_ROUTE && ro->track == dst && ro->channel == r.srcChannel &&
             ro->remoteChannel == r.dstChannel && ro->channels == r.channels)
            exists = true;
        if(exists)
          add(MixerImportReportItem::Done, label, what, tr("It was already there."));
        else if(!routeCanConnect(s, d))
          add(MixerImportReportItem::Deviation, label, what, tr("The route is not possible (loop or channels that do not fit)."));
        else
        {
          if(!dryRun)
            ops.push_back(UndoOp(UndoOp::AddRoute, s, d));
          add(MixerImportReportItem::Done, label, what);
        }
      }

    }
    {
      for(const MixerDeviceRoute& r : src.deviceRoutes)
      {
        if(!(c.categories & (r.input ? MixerPortInputs : MixerPortOutputs)))
          continue;
        const QString what = r.input ? tr("Route from the audio port %1").arg(r.port) : tr("Route to the audio port %1").arg(r.port);
        if(!MusEGlobal::audioDevice || !MusEGlobal::audioDevice->findPort(r.port.toUtf8().constData()))
        {
          add(MixerImportReportItem::Deviation, label, what, tr("The audio port is not available here."));
          continue;
        }
        const Route portRoute(r.port, !r.input, r.channel, Route::JACK_ROUTE);
        const Route trackRoute(t, r.channel);
        const Route& s = r.input ? portRoute : trackRoute;
        const Route& d = r.input ? trackRoute : portRoute;
        bool exists = false;
        const RouteList* rl = r.input ? t->inRoutes() : t->outRoutes();
        for(ciRoute ro = rl->begin(); ro != rl->end(); ++ro)
          if(ro->type == Route::JACK_ROUTE && ro->channel == r.channel && ro->name() == r.port)
            exists = true;
        if(exists)
          add(MixerImportReportItem::Done, label, what, tr("It was already there."));
        else if(!routeCanConnect(s, d))
          add(MixerImportReportItem::Deviation, label, what, tr("The route is not possible."));
        else
        {
          if(!dryRun)
            ops.push_back(UndoOp(UndoOp::AddRoute, s, d));
          add(MixerImportReportItem::Done, label, what);
        }
      }
    }
  }

  if(!dryRun && !ops.empty())
    song->applyOperationGroup(ops);
}

} // namespace MusECore
