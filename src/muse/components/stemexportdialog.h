//=========================================================
//  MusE
//  Linux Music Editor
//
//  stemexportdialog.h
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

#ifndef __STEMEXPORTDIALOG_H__
#define __STEMEXPORTDIALOG_H__

#include <vector>

#include <QString>
#include <QList>

#include "ui_stemexportdialogbase.h"
#include "mixdowndialog.h"

class QWidget;
class QTreeWidgetItem;
class QEvent;
class QShowEvent;

namespace MusECore {
class AudioOutput;
class Track;

//---------------------------------------------------------
//   StemExportSettings
//    Result of the stem export dialog.
//---------------------------------------------------------

struct StemExportSettings {
      MixdownSettings output;       // 'path' is the folder.
      std::vector<Track*> tracks;   // The tracks to export, in song order.
      // Take the stems from this output track (after its effects), nullptr: from the tracks themselves.
      AudioOutput* throughOutput = nullptr;
      };

// Shows the stem export dialog. Returns false if cancelled.
extern bool getStemExportSettings(QWidget* parent, const QString& suggestedDir, StemExportSettings& settings);
}

namespace MusEGui {

//---------------------------------------------------------
//   StemExportDialog
//---------------------------------------------------------

class StemExportDialog : public QDialog, public Ui::StemExportDialogBase {
    Q_OBJECT
      MusECore::StemExportSettings _settings;
      QList<int> _formats; // libsndfile format (without channel info) of each comboFormat row.
      // Width of the track name column set by the user dragging its handle, otherwise -1.
      int _userTrackColumnWidth;
      bool _fittingColumns;

      void fillTracks();
      // Gives the track name column the room it needs, but keeps the handle between the columns in view.
      void fitTrackColumns();
      int typeColumnContentWidth() const;
      // Checks the exportable tracks for which 'pred' is true, unchecks the others.
      template <class Pred> void checkTracks(Pred pred);
      static MusECore::Track* itemTrack(const QTreeWidgetItem* item);

   private slots:
      void fdialog();
      virtual void accept();
      void formatChanged(int);
      void modeChanged(int);
      void checkAll();
      void checkNone();
      void checkSelected();
      void trackColumnResized(int logicalIndex, int oldSize, int newSize);
      void outputToggled(bool);

   protected:
      void showEvent(QShowEvent* e) override;
      bool eventFilter(QObject* o, QEvent* e) override;

   public:
      StemExportDialog(QWidget* parent, const QString& suggestedDir);
      const MusECore::StemExportSettings& settings() const { return _settings; }
      };

}

#endif
