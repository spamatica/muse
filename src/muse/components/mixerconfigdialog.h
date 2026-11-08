//=========================================================
//  MusE
//  Linux Music Editor
//
//  mixerconfigdialog.h
//  Saving and importing the mixer configuration: The dialog for choosing what to import, and the report.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __MIXERCONFIGDIALOG_H__
#define __MIXERCONFIGDIALOG_H__

#include <QDialog>

#include <vector>

#include "mixerconfig.h"

class QTreeWidget;
class QTreeWidgetItem;

namespace MusEGui {

// Asks for a file and saves the mixer configuration of the current song.
void saveMixerConfiguration(QWidget* parent);
// Asks for a mixer configuration file or a song file, lets the user choose what to import, imports and reports.
void loadMixerConfiguration(QWidget* parent);

//---------------------------------------------------------
//   MixerImportDialog
//---------------------------------------------------------

class MixerImportDialog : public QDialog
{
      Q_OBJECT

      const MusECore::MixerSnapshot& _snapshot;
      QTreeWidget* _tree;
      struct Row
      {
        QTreeWidgetItem* item = nullptr;
        MusECore::AudioTrack* target = nullptr;
        std::vector<QTreeWidgetItem*> categories;   // In the order of _catBits.
        std::vector<QTreeWidgetItem*> plugins;
      };
      std::vector<Row> _rows;

      bool _portOn[2] = { false, false };   // Input routing, output routing from the soundcard checkboxes.
      bool _updatingChecks = false;
      void setAll(bool on);
      // Sets the soundcard routing items of all tracks as the soundcard checkboxes say.
      void applyPortStates();

   public:
      MixerImportDialog(const MusECore::MixerSnapshot& snapshot, QWidget* parent = nullptr);
      std::vector<MusECore::MixerImportChoice> choices() const;
};

//---------------------------------------------------------
//   MixerImportReportDialog
//---------------------------------------------------------

class MixerImportReportDialog : public QDialog
{
      Q_OBJECT
   public:
      MixerImportReportDialog(const MusECore::MixerImportReport& report, QWidget* parent = nullptr);
};

} // namespace MusEGui

#endif
