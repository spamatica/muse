//=========================================================
//  MusE
//  Linux Music Editor
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

#ifndef __MARKERMIXDOWN_H__
#define __MARKERMIXDOWN_H__

#include <vector>

#include <QObject>
#include <QString>
#include <QStringList>

#include "components/mixdowndialog.h"

class QProgressDialog;
class QTimer;

namespace MusECore {

class AudioOutput;

// A part of the song to mix down.
struct MixdownSection {
      QString name;
      unsigned startFrame;
      unsigned endFrame;
      };

// One section for each named marker: from the marker to the next marker (of any kind),
//  the last one until the end of the song. Sorted by time.
std::vector<MixdownSection> markerMixdownSections();
// Makes a string usable as part of a file name.
QString sanitizeFileName(const QString& s);
// Why an export (downmix or stems) can not start now, or an empty string. GUI thread.
QString exportBlockedReason();

// The state of a running or finished export (downmix or stems), for those that started it
//  without the GUI (the MCP server).
struct ExportReport {
      bool finished = false;
      bool cancelled = false;
      int job = 0;            // The running job, counted from 1. When finished: the jobs done.
      int jobCount = 0;
      int percent = 0;        // Of the running job.
      QString current;        // What the running job renders.
      QStringList created;    // The files written.
      QStringList failed;     // What could not be exported.
      };

//---------------------------------------------------------
//   MarkerMixdown
//    Renders one downmix file for each named marker: from the marker to the next
//    marker (of any kind), the last one until the end of the song. The mixdowns run
//    one after the other without interaction. Deletes itself when done.
//    Started with begin() (no GUI), it can also render the range between the range
//    markers to one file.
//---------------------------------------------------------

class MarkerMixdown : public QObject {
      Q_OBJECT

      struct Job {
            QString name;
            unsigned startFrame;
            unsigned endFrame;
            QString path;     // Empty: made from the marker name when the job starts.
            };

      AudioOutput* _ao;
      MixdownSettings _settings;
      QString _projectName;
      std::vector<Job> _jobs;
      size_t _index;          // Next job to start, or the running one + 1.
      bool _running;
      bool _cancelled;
      bool _cancelRequested;
      bool _finished;
      QStringList _created;
      QProgressDialog* _pd;   // Only when started from the GUI.
      QTimer* _timer;

      MarkerMixdown(AudioOutput* ao, const MixdownSettings& s, const QString& projectName);
      ~MarkerMixdown();

      void buildJobs();
      QString uniquePath(const QString& markerName) const;
      void run();
      void startJob();
      void finish();

   private slots:
      void poll();

   signals:
      // Just before it deletes itself.
      void finished(const MusECore::ExportReport& report);

   public:
      // GUI thread. Returns true if nothing was started (a message has been shown).
      static bool start(AudioOutput* ao, const MixdownSettings& s, const QString& projectName);
      // GUI thread, no dialogs or messages. With s.markerMode one file per named marker in the folder s.path
      //  (numbered if a file exists), otherwise the range markers to the file s.path, which is only replaced
      //  if 'overwrite'. Returns nullptr with 'error' set if nothing was started.
      static MarkerMixdown* begin(AudioOutput* ao, const MixdownSettings& s, const QString& projectName,
                                  bool overwrite, QString* error);
      // Stops the running job (its file may be incomplete) and does not start the others.
      void cancel() { _cancelRequested = true; }
      ExportReport report() const;
      };

} // namespace MusECore

#endif
