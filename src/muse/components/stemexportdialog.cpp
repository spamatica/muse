//=========================================================
//  MusE
//  Linux Music Editor
//
//  stemexportdialog.cpp
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

#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QHash>
#include <QHeaderView>
#include <QMessageBox>
#include <QSet>
#include <QShowEvent>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <sndfile.h>

#include "globals.h"
#include "gconfig.h"
#include "song.h"
#include "track.h"
#include "synth.h"
#include "stemexport.h"
#include "stemexportdialog.h"

namespace MusECore {

bool getStemExportSettings(QWidget* parent, const QString& suggestedDir, StemExportSettings& settings)
      {
      MusEGui::StemExportDialog dialog(parent, suggestedDir);
      if (dialog.exec() != QDialog::Accepted)
            return false;
      settings = dialog.settings();
      return true;
      }

} // namespace MusECore

namespace MusEGui {

// The names of the tracks checked the last time, so the next export in this session starts from there.
static QSet<QString> lastChecked;
static bool haveLastChecked = false;

// The track of a list row, nullptr for folders and rows that can not be exported.
static const int TrackRole = Qt::UserRole;

//---------------------------------------------------------
//   StemExportDialog
//---------------------------------------------------------

StemExportDialog::StemExportDialog(QWidget* parent, const QString& suggestedDir)
   : QDialog(parent), _userTrackColumnWidth(-1), _fittingColumns(false)
      {
      setupUi(this);

      // Both columns can be adjusted.
      QHeaderView* h = treeTracks->header();
      h->setSectionsMovable(false);
      h->setSectionResizeMode(QHeaderView::Interactive);
      // The type column is fitted to what is left by fitTrackColumns().
      h->setStretchLastSection(false);
      connect(h, &QHeaderView::sectionResized, this, &StemExportDialog::trackColumnResized);
      // The width of the columns follows the width of the list (dialog resized, scroll bar shown).
      treeTracks->viewport()->installEventFilter(this);
      // Room for five lines of the description, whatever the font size is.
      labelModeInfo->setMinimumHeight(labelModeInfo->fontMetrics().lineSpacing() * 5);

      MixdownFileDialog::fillFormats(comboFormat, _formats);
      spinQuality->setValue(MusEGlobal::config.stemQuality);
      const int fi = _formats.indexOf(MusEGlobal::config.stemFormat);
      if (fi >= 0)
            comboFormat->setCurrentIndex(fi);
      const int ci = MusEGlobal::config.stemChannels;
      if (ci >= 0 && ci < comboChannel->count())
            comboChannel->setCurrentIndex(ci);
      comboMode->setCurrentIndex(MusEGlobal::config.stemMode != 0 ? 1 : 0);

      editPath->setText(QDir(suggestedDir).filePath("downmix/stems"));

      // Off by default, it is not what a stem normally is.
      // The choice of output tracks is only shown if there is one to make.
      // The first selected output track, as the downmix does.
      const MusECore::OutputList* ol = MusEGlobal::song->outputs();
      int selectedOutput = -1;
      for (MusECore::AudioOutput* o : *ol) {
            comboOutput->addItem(o->name(), QVariant::fromValue<void*>(o));
            if (selectedOutput < 0 && o->selected())
                  selectedOutput = comboOutput->count() - 1;
            }
      if (selectedOutput >= 0)
            comboOutput->setCurrentIndex(selectedOutput);
      comboOutput->setVisible(ol->size() > 1);
      checkOutput->setEnabled(!ol->empty());
      checkOutput->setChecked(false);
      connect(checkOutput, SIGNAL(toggled(bool)), SLOT(outputToggled(bool)));
      outputToggled(false);

      fillTracks();

      connect(buttonPath, SIGNAL(clicked()), SLOT(fdialog()));
      connect(comboFormat, SIGNAL(currentIndexChanged(int)), SLOT(formatChanged(int)));
      connect(comboMode, SIGNAL(currentIndexChanged(int)), SLOT(modeChanged(int)));
      connect(buttonAll, SIGNAL(clicked()), SLOT(checkAll()));
      connect(buttonNone, SIGNAL(clicked()), SLOT(checkNone()));
      connect(buttonSelected, SIGNAL(clicked()), SLOT(checkSelected()));
      modeChanged(comboMode->currentIndex());
      formatChanged(comboFormat->currentIndex());
      }

//---------------------------------------------------------
//   itemTrack
//---------------------------------------------------------

MusECore::Track* StemExportDialog::itemTrack(const QTreeWidgetItem* item)
      {
      return static_cast<MusECore::Track*>(item->data(0, TrackRole).value<void*>());
      }

//---------------------------------------------------------
//   fillTracks
//    The tracks in song order, inside their folders.
//---------------------------------------------------------

void StemExportDialog::fillTracks()
      {
      treeTracks->clear();
      QHash<const MusECore::Track*, QTreeWidgetItem*> folderItems;
      QList<QTreeWidgetItem*> folderOrder;
      bool anyRemembered = false;

      for (MusECore::Track* t : *MusEGlobal::song->tracks()) {
            if (!t->isFolderTrack() && !MusECore::StemExport::listed(t))
                  continue;
            // The nearest folder above it that is in the list.
            QTreeWidgetItem* parent = nullptr;
            for (const MusECore::Track* f = t->folder(); f && !parent; f = f->folder())
                  parent = folderItems.value(f, nullptr);
            QTreeWidgetItem* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(treeTracks);
            item->setText(0, t->name());
            item->setIcon(0, *t->icon());

            if (t->isFolderTrack()) {
                  item->setText(1, tr("Folder"));
                  // Checking a folder checks the tracks in it.
                  item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
                  folderItems.insert(t, item);
                  folderOrder.append(item);
                  continue;
                  }

            QString why;
            MusECore::AudioTrack* src = MusECore::StemExport::source(t, &why);
            QString type;
            switch (t->type()) {
                  case MusECore::Track::WAVE:            type = tr("Wave"); break;
                  case MusECore::Track::AUDIO_SOFTSYNTH: type = tr("Synth"); break;
                  case MusECore::Track::AUDIO_GROUP:     type = tr("Group"); break;
                  case MusECore::Track::DRUM:            type = tr("Drum"); break;
                  default:                               type = tr("Midi"); break;
                  }
            if (t->isMidiTrack()) {
                  const MusECore::SynthI* si = static_cast<const MusECore::MidiTrack*>(t)->drivenSynth();
                  if (si)
                        type = tr("%1, plays %2").arg(type).arg(si->name());
                  }
            item->setText(1, type);
            if (!src) {
                  // No check box, so checking a folder leaves it alone.
                  item->setFlags(Qt::NoItemFlags);
                  item->setToolTip(0, why);
                  item->setToolTip(1, why);
                  continue;
                  }
            item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            // The column may be too narrow for it.
            item->setToolTip(1, type);
            item->setData(0, TrackRole, QVariant::fromValue<void*>(t));
            item->setCheckState(0, Qt::Unchecked);
            if (haveLastChecked && lastChecked.contains(t->name()))
                  anyRemembered = true;
            }

      // Folders with nothing to export in them are of no use. Innermost first.
      for (int i = folderOrder.size() - 1; i >= 0; --i)
            if (folderOrder.at(i)->childCount() == 0)
                  delete folderOrder.at(i);

      treeTracks->expandAll();

      if (anyRemembered)
            checkTracks([](const MusECore::Track* t) { return lastChecked.contains(t->name()); });
      else
            // Synth and group tracks would repeat what their midi or member tracks already have,
            //  so by default only the wave and midi tracks that are heard.
            checkTracks([](const MusECore::Track* t) {
                  return (t->type() == MusECore::Track::WAVE || t->isMidiTrack()) && !t->mute() && !t->off(); });
      }

//---------------------------------------------------------
//   fitTrackColumns
//    The track name column gets the width of its contents, more if the type column does not need
//    the room, or the width the user gave it. But never so wide that the handle between the
//    columns can not be seen. The type column fills the rest, or extends to the right if its
//    contents need more (the list scrolls then).
//    (Not with QHeaderView's stretchLastSection, which keeps the last column at some 100 pixels
//    and so shows a scroll bar even when everything fits.)
//---------------------------------------------------------

int StemExportDialog::typeColumnContentWidth() const
      {
      // (Public in the base class, QTreeView made it protected.)
      const QAbstractItemView* view = treeTracks;
      return qMax(view->sizeHintForColumn(1), treeTracks->header()->sectionSizeHint(1));
      }

void StemExportDialog::fitTrackColumns()
      {
      QHeaderView* h = treeTracks->header();
      const int avail = treeTracks->viewport()->width();
      if (avail <= 0)
            return;
      const int content1 = typeColumnContentWidth();
      int w;
      if (_userTrackColumnWidth >= 0)
            w = _userTrackColumnWidth;
      else {
            const QAbstractItemView* view = treeTracks;
            const int content0 = qMax(view->sizeHintForColumn(0), h->sectionSizeHint(0));
            w = qMax(content0, avail - content1);
            }
      // Leave a bit of the type column, so the handle stays in view.
      const int minType = qMax(h->minimumSectionSize(), fontMetrics().averageCharWidth() * 6);
      w = qMax(h->minimumSectionSize(), qMin(w, avail - minType));
      _fittingColumns = true;
      h->resizeSection(0, w);
      h->resizeSection(1, qMax(avail - w, content1));
      _fittingColumns = false;
      }

void StemExportDialog::trackColumnResized(int logicalIndex, int, int newSize)
      {
      if (_fittingColumns || logicalIndex != 0)
            return;
      // The user dragged the handle. Keep that width, and let the type column fill the rest.
      _userTrackColumnWidth = newSize;
      _fittingColumns = true;
      treeTracks->header()->resizeSection(1, qMax(treeTracks->viewport()->width() - newSize, typeColumnContentWidth()));
      _fittingColumns = false;
      }

void StemExportDialog::showEvent(QShowEvent* e)
      {
      QDialog::showEvent(e);
      fitTrackColumns();
      // Once more when the layout has settled.
      QTimer::singleShot(0, this, [this]() { fitTrackColumns(); });
      }

bool StemExportDialog::eventFilter(QObject* o, QEvent* e)
      {
      if (o == treeTracks->viewport() && e->type() == QEvent::Resize)
            fitTrackColumns();
      return QDialog::eventFilter(o, e);
      }

//---------------------------------------------------------
//   outputToggled
//---------------------------------------------------------

void StemExportDialog::outputToggled(bool on)
      {
      comboOutput->setEnabled(on);
      labelTrackInfo->setText(on ?
            tr("Each stem is the track alone as the output track gets it: through the group tracks on the way, "
               "plus what its aux sends return, with the effects and the fader of the output track. "
               "A midi track is rendered through the synth it plays.") :
            tr("Each stem is the track's output after its fader and pan, plus what its aux sends return "
               "(reverb, delay ...). Group tracks and the master output are not included. "
               "A midi track is rendered through the synth it plays."));
      }

//---------------------------------------------------------
//   checkTracks
//---------------------------------------------------------

template <class Pred> void StemExportDialog::checkTracks(Pred pred)
      {
      for (QTreeWidgetItemIterator it(treeTracks); *it; ++it) {
            const MusECore::Track* t = itemTrack(*it);
            if (t)
                  (*it)->setCheckState(0, pred(t) ? Qt::Checked : Qt::Unchecked);
            }
      }

void StemExportDialog::checkAll()
      {
      checkTracks([](const MusECore::Track*) { return true; });
      }

void StemExportDialog::checkNone()
      {
      checkTracks([](const MusECore::Track*) { return false; });
      }

void StemExportDialog::checkSelected()
      {
      checkTracks([](const MusECore::Track* t) { return t->selected(); });
      }

//---------------------------------------------------------
//   formatChanged
//---------------------------------------------------------

void StemExportDialog::formatChanged(int idx)
      {
      const int fmt = _formats.value(idx);
      const bool compressed = (fmt & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV;
      labelQuality->setEnabled(compressed);
      spinQuality->setEnabled(compressed);
      }

//---------------------------------------------------------
//   modeChanged
//---------------------------------------------------------

void StemExportDialog::modeChanged(int idx)
      {
      labelModeInfo->setText(idx != 0 ?
            tr("For each named marker, one file for each checked track, from the marker to the next marker "
               "(the last one to the end of the song). Each marker gets a subfolder. The file name is the "
               "project name plus the marker and track names.") :
            tr("One file for each checked track, between the left and right range markers. "
               "The file name is the project name plus the track name."));
      }

//---------------------------------------------------------
//   accept
//---------------------------------------------------------

void StemExportDialog::accept()
      {
      std::vector<MusECore::Track*> tracks;
      QSet<QString> names;
      for (QTreeWidgetItemIterator it(treeTracks); *it; ++it) {
            MusECore::Track* t = itemTrack(*it);
            if (t && (*it)->checkState(0) == Qt::Checked) {
                  tracks.push_back(t);
                  names.insert(t->name());
                  }
            }
      if (tracks.empty()) {
            QMessageBox::information(this, windowTitle(), tr("Check the tracks to export."));
            return;
            }
      const QString path = editPath->text().trimmed();
      if (path.isEmpty()) {
            QMessageBox::information(this, windowTitle(), tr("Choose the folder to put the stems in."));
            return;
            }

      const int format = _formats.value(comboFormat->currentIndex());
      _settings.tracks = tracks;
      _settings.throughOutput = checkOutput->isChecked() ?
            static_cast<MusECore::AudioOutput*>(comboOutput->currentData().value<void*>()) : nullptr;
      _settings.output.markerMode = comboMode->currentIndex() != 0;
      _settings.output.path       = path;
      _settings.output.format     = format;
      _settings.output.channels   = comboChannel->currentIndex() == 1 ? 1 : 2;
      _settings.output.quality    = (format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV ? spinQuality->value() / 10.0 : -1.0;

      lastChecked = names;
      haveLastChecked = true;
      MusEGlobal::config.stemMode     = _settings.output.markerMode ? 1 : 0;
      MusEGlobal::config.stemFormat   = format;
      MusEGlobal::config.stemChannels = comboChannel->currentIndex();
      MusEGlobal::config.stemQuality  = spinQuality->value();
      done(1);
      }

//---------------------------------------------------------
//   fdialog
//---------------------------------------------------------

void StemExportDialog::fdialog()
      {
      const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder for the stems"), editPath->text());
      if (!dir.isEmpty())
            editPath->setText(dir);
      }

} // namespace MusEGui
