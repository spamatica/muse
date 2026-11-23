//=========================================================
//  MusE
//  Linux Music Editor
//
//  stemexport.h
//   Exports the output of tracks to files, one file per track ("stems").
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

#ifndef __STEMEXPORT_H__
#define __STEMEXPORT_H__

#include <vector>
#include <utility>

#include <QObject>
#include <QString>
#include <QStringList>

#include "markermixdown.h"

class QProgressDialog;
class QTimer;

namespace MusECore {

class AudioOutput;
class AudioTrack;
class StemCapture;
class Track;

//---------------------------------------------------------
//   StemExport
//    Renders one file per track and section (the range markers, or each named marker).
//    A stem is the output of the track (after fader and pan) plus the returns of the aux
//    tracks, not passing groups or the master output. Or, if asked for, what an output track
//    gets of it (after the output's effects and fader). Each stem is an offline pass with
//    the track and the aux tracks soloed, so the aux tracks only get that track's sends,
//    and a midi track is heard alone on its synth even if other midi tracks play it too.
//    The solo states are restored afterwards. Deletes itself when done.
//---------------------------------------------------------

class StemExport : public QObject {
      Q_OBJECT

      struct Job {
            QString section;     // Marker name, empty for the range markers.
            unsigned startFrame;
            unsigned endFrame;
            Track* track;
            QString path;
            };

      MixdownSettings _settings;
      AudioOutput* _output;   // Take the stems from this output track, nullptr: from the tracks.
      QString _projectName;
      std::vector<Job> _jobs;
      std::vector<std::pair<Track*, bool> > _soloStates; // To restore.
      size_t _index;          // Next job to start, or the running one + 1.
      bool _running;          // A job's pass has been started and is not finished.
      bool _stopSeen;         // The transport stopped (and the GUI handled it) since the pass was started.
      bool _playSeen;
      int _waitPolls;
      bool _cancelled;
      bool _cancelRequested;
      bool _finished;
      QStringList _created;
      QStringList _failed;
      StemCapture* _capture;
      QProgressDialog* _pd;
      QTimer* _timer;

      StemExport(const MixdownSettings& s, AudioOutput* output, const QString& projectName);
      ~StemExport();

      void buildJobs(const std::vector<Track*>& tracks);
      // Checks and makes the jobs. nullptr with 'error' (and 'warning' for a warning) set if it can not start.
      //  'existing' is the number of files that would be overwritten.
      static StemExport* create(const MixdownSettings& settings, const std::vector<Track*>& tracks,
                                AudioOutput* output, const QString& projectName,
                                QString* error, bool* warning, int* existing);
      void run();
      void applySolo(const Track* soloTrack);
      void restoreSolo();
      bool startJob();
      void finishJob();
      void finish();

   private slots:
      void poll();
      void playChanged(bool);

   signals:
      // Just before it deletes itself.
      void finished(const MusECore::ExportReport& report);

   public:
      // GUI thread. 'settings.path' is the folder. With 'output', each stem is taken from that output track
      //  (through the groups, and the effects and fader of the output) instead of from the track itself.
      // Returns true if nothing was started (a message has been shown).
      static bool start(const MixdownSettings& settings, const std::vector<Track*>& tracks,
                        AudioOutput* output, const QString& projectName);
      // The same without dialogs or messages. Existing stem files are only replaced if 'overwrite'.
      // Returns nullptr with 'error' set if nothing was started.
      static StemExport* begin(const MixdownSettings& settings, const std::vector<Track*>& tracks,
                               AudioOutput* output, const QString& projectName, bool overwrite, QString* error);
      // Stops the running stem (its file is removed) and does not start the others.
      void cancel() { _cancelRequested = true; }
      ExportReport report() const;

      // Whether the track is offered for stem export (folders are only for grouping the list).
      static bool listed(const Track* t);
      // The track whose output is the stem of 't': the track itself, or the synth a midi track plays.
      // nullptr if 't' can not be exported, then 'why' (if given) says why.
      static AudioTrack* source(const Track* t, QString* why = nullptr);
      };

} // namespace MusECore

#endif
