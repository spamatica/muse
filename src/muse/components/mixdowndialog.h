//=========================================================
//  MusE
//  Linux Music Editor
//    $Id: mixdowndialog.h,v 1.1.1.1 2003/10/27 18:54:28 wschweer Exp $
//  (C) Copyright 2001 Werner Schweer (ws@seh.de)
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

#ifndef __MIXDOWNDIALOG_H__
#define __MIXDOWNDIALOG_H__

#include <QString>
#include <QList>

#include "ui_mixdowndialogbase.h"

class QWidget;

namespace MusECore {
class SndFile;

//---------------------------------------------------------
//   MixdownSettings
//    Result of the downmix dialog.
//---------------------------------------------------------

struct MixdownSettings {
      bool markerMode = false;   // false: between the range markers, true: one file per named marker.
      QString path;              // Range mode: the file. Marker mode: the folder to put the files in.
      int format = 0;            // libsndfile format (type | subtype).
      int channels = 2;
      double quality = -1.0;     // 0.0 - 1.0 for compressed formats, < 0 for none.

      // File extension (without dot) matching the format.
      QString extension() const;
      // Creates a SndFile for the given path (creating folders, removing an existing file).
      // The caller owns it. Not opened.
      SndFile* createSndFile(const QString& filePath) const;
      };

// Shows the downmix dialog. Returns false if cancelled.
extern bool getMixdownSettings(QWidget* parent, const QString& suggestedDir,
                               const QString& suggestedName, MixdownSettings& settings);
}

namespace MusEGui {

//---------------------------------------------------------
//   MixdownFileDialog
//---------------------------------------------------------

class MixdownFileDialog : public QDialog, public Ui::MixdownFileDialogBase {
    Q_OBJECT
      MusECore::MixdownSettings _settings;
      QString _suggestedDir;
      QString _suggestedName;

      QList<int> _formats; // libsndfile format (without channel info) of each comboFormat row.

      QString suggestPath(const QString& dir, const QString& name) const;

   private slots:
      void fdialog();
      virtual void accept();
      void formatChanged(int);
      void modeChanged(int);

   public:
      MixdownFileDialog(QWidget* parent, const QString& suggestedDir, const QString& suggestedName);
      const MusECore::MixdownSettings& settings() const { return _settings; }
      };

}

#endif

