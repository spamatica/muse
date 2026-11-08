//=========================================================
//  MusE
//  Linux Music Editor
//
//  mixerconfig.h
//  Saving the mixer settings of the audio tracks to a file, and importing them into another song.
//  The settings are: faders and pans, mute/solo/off, prefader, aux sends, the effect plugins with their
//  settings, and the routing. Tracks are matched by name and type. No tracks are created.
//  The file can be a mixer configuration file (*.med_mixer_config), or a song file, which has the same
//  information.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __MIXERCONFIG_H__
#define __MIXERCONFIG_H__

#include <QString>
#include <QIODevice>

#include <utility>
#include <vector>

#include "track.h"

namespace MusECore {

class Xml;

// What can be imported. Combined as bits.
enum MixerCategory
{
  MixerFader            = 0x01,   // Volume, pan and gain.
  MixerFlags            = 0x02,   // Mute, solo and off.
  MixerPrefader         = 0x04,
  MixerAuxSends         = 0x08,
  MixerPlugins          = 0x10,   // The effect rack: Plugins, their settings and midi assignments.
  MixerRouting          = 0x20,   // The routes from the track to other audio tracks.
  MixerPluginAutomation = 0x40,   // The automation of the plugin parameters.
  MixerPortInputs       = 0x80,   // The routes from audio ports (the soundcard) to audio input tracks.
  MixerPortOutputs      = 0x100,  // The routes from audio output tracks to audio ports.
  MixerAllCategories    = 0x1ff
};

// An effect plugin of a track.
struct MixerPluginConfig
{
  int slot = -1;              // Position in the effect rack.
  QString xml;                // The plugin element, with its automation and midi assignments. For PluginI.
  QString name;               // For display.
  int type = 0;               // MusEPlugin::PluginType
  QString file, uri, label;   // To find the plugin.
  bool hasAutomation = false;
};

// A route from a track to another track.
struct MixerTrackRoute
{
  QString dstTrack;
  int srcChannel = -1;
  int dstChannel = -1;
  int channels = -1;
};

// A route between an audio input or output track and an audio port.
struct MixerDeviceRoute
{
  QString port;
  int channel = -1;
  bool input = false;         // From the port to the track (audio input), or from the track to the port.
};

// The settings of one audio track.
struct MixerTrackConfig
{
  QString name;
  Track::TrackType type = Track::AUDIO_GROUP;
  int channels = -1;
  bool hasFader = false;
  double volume = 1.0, pan = 0.0, gain = 1.0;
  bool hasFlags = false;
  bool mute = false, solo = false, off = false;
  bool hasPrefader = false;
  bool prefader = false;
  // By the name of the aux track.
  std::vector<std::pair<QString, double> > auxSends;
  std::vector<MixerPluginConfig> plugins;
  std::vector<MixerTrackRoute> routes;
  std::vector<MixerDeviceRoute> deviceRoutes;

  // The MixerCategory bits that there is something to import for.
  unsigned categories() const;
};

// What to import for a track of the file.
struct MixerImportChoice
{
  int source = -1;                      // Index into MixerSnapshot::tracks.
  AudioTrack* target = nullptr;         // Null: Do not import.
  unsigned categories = 0;              // MixerCategory bits.
  std::vector<bool> plugins;            // Which of the plugins of the source, if MixerPlugins. Empty means all.
};

// One line of the report.
struct MixerImportReportItem
{
  enum Result
  {
    Done,        // As planned.
    Deviation    // Not (fully) as planned.
  };
  Result result = Done;
  QString track;       // Name of the track (of the file, and here if different).
  QString what;        // What was to be done.
  QString note;        // Why it was not done, or a remark.
};

struct MixerImportReport
{
  std::vector<MixerImportReportItem> items;
  int tracksRequested = 0;     // Tracks that something was to be imported for.
  int tracksMatched = 0;       // Of those, the ones that have a track to import to.

  int count(MixerImportReportItem::Result r) const;
};

class MixerSnapshot
{
  public:
    std::vector<MixerTrackConfig> tracks;

    // Writes the mixer settings of the audio tracks of the current song. Returns false and sets the error on failure.
    static bool saveSong(QIODevice* dev, QString* error = nullptr);

    // Reads the settings from a mixer configuration file, or a song file. Returns false and sets the error on failure.
    bool load(QIODevice* dev, QString* error = nullptr);

    // The track of the current song with the name and type of the track in the file, or null.
    static AudioTrack* findTarget(const MixerTrackConfig& config);

    // Imports into the current song, or only finds out what could be imported, if dryRun.
    // Everything that can be undone is one undo step. Setting faders, prefader and aux sends can not be undone.
    void apply(const std::vector<MixerImportChoice>& choices, bool dryRun, MixerImportReport& report) const;
};

} // namespace MusECore

#endif
