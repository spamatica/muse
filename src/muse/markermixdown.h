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

//---------------------------------------------------------
//   MarkerMixdown
//    Renders one downmix file for each named marker: from the marker to the next
//    marker (of any kind), the last one until the end of the song. The mixdowns run
//    one after the other without interaction. Deletes itself when done.
//---------------------------------------------------------

class MarkerMixdown : public QObject {
      Q_OBJECT

      struct Job {
            QString name;
            unsigned startFrame;
            unsigned endFrame;
            };

      AudioOutput* _ao;
      MixdownSettings _settings;
      QString _projectName;
      std::vector<Job> _jobs;
      size_t _index;          // Next job to start, or the running one + 1.
      bool _running;
      bool _cancelled;
      QStringList _created;
      QProgressDialog* _pd;
      QTimer* _timer;

      MarkerMixdown(AudioOutput* ao, const MixdownSettings& s, const QString& projectName);
      ~MarkerMixdown();

      void buildJobs();
      QString uniquePath(const QString& markerName) const;
      void startJob();
      void finish();

   private slots:
      void poll();

   public:
      // GUI thread. Returns true if nothing was started (a message has been shown).
      static bool start(AudioOutput* ao, const MixdownSettings& s, const QString& projectName);
      };

} // namespace MusECore

#endif
