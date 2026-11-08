//=========================================================
//  MusE
//  Linux Music Editor
//
//  mixerconfigdialog.cpp
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "mixerconfigdialog.h"

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QFileDialog>
#include <QHeaderView>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include "audio.h"
#include "filedialog.h"
#include "globals.h"
#include "song.h"

namespace MusEGui {

static const char* mixerConfigSuffix = "med_mixer_config";

static const struct { unsigned bit; const char* name; } catInfo[] = {
  { MusECore::MixerFader,            QT_TRANSLATE_NOOP("MixerImportDialog", "Fader, pan and gain") },
  { MusECore::MixerFlags,            QT_TRANSLATE_NOOP("MixerImportDialog", "Mute, solo and off") },
  { MusECore::MixerPrefader,         QT_TRANSLATE_NOOP("MixerImportDialog", "Prefader") },
  { MusECore::MixerAuxSends,         QT_TRANSLATE_NOOP("MixerImportDialog", "Aux sends") },
  { MusECore::MixerPlugins,          QT_TRANSLATE_NOOP("MixerImportDialog", "Effect plugins") },
  { MusECore::MixerPluginAutomation, QT_TRANSLATE_NOOP("MixerImportDialog", "Plugin automation") },
  { MusECore::MixerRouting,          QT_TRANSLATE_NOOP("MixerImportDialog", "Routing to other tracks") },
  { MusECore::MixerPortInputs,       QT_TRANSLATE_NOOP("MixerImportDialog", "Input routing from the soundcard") },
  { MusECore::MixerPortOutputs,      QT_TRANSLATE_NOOP("MixerImportDialog", "Output routing to the soundcard") }
};
static const int numCats = sizeof(catInfo) / sizeof(catInfo[0]);

static QString trackTypeName(MusECore::Track::TrackType t)
{
  switch(t)
  {
    case MusECore::Track::WAVE:            return QObject::tr("Wave");
    case MusECore::Track::AUDIO_INPUT:     return QObject::tr("Audio input");
    case MusECore::Track::AUDIO_OUTPUT:    return QObject::tr("Audio output");
    case MusECore::Track::AUDIO_GROUP:     return QObject::tr("Group");
    case MusECore::Track::AUDIO_AUX:       return QObject::tr("Aux");
    case MusECore::Track::AUDIO_SOFTSYNTH: return QObject::tr("Synth");
    default:                               return QString();
  }
}

//---------------------------------------------------------
//   saveMixerConfiguration
//---------------------------------------------------------

void saveMixerConfiguration(QWidget* parent)
{
  QString name = QFileDialog::getSaveFileName(parent, QObject::tr("MusE: Save Mixer Configuration"), QString(),
    QObject::tr("Mixer configuration (*.%1)").arg(mixerConfigSuffix));
  if(name.isEmpty())
    return;
  if(!name.endsWith(QString(".") + mixerConfigSuffix, Qt::CaseInsensitive))
    name += QString(".") + mixerConfigSuffix;

  MusEFile::File f(name, QString(".") + mixerConfigSuffix, parent);
  if(MusEGui::fileOpen(f, QIODevice::WriteOnly, parent, false, true) != MusEFile::File::NoError)
    return;
  QString error;
  const bool ok = MusECore::MixerSnapshot::saveSong(f.iodevice(), &error);
  f.close();
  if(!ok)
    QMessageBox::critical(parent, QObject::tr("MusE: Save Mixer Configuration"), error);
}

//---------------------------------------------------------
//   loadMixerConfiguration
//---------------------------------------------------------

void loadMixerConfiguration(QWidget* parent)
{
  const QString title = QObject::tr("MusE: Load Mixer Configuration");
  const QString name = QFileDialog::getOpenFileName(parent, title, QString(),
    QObject::tr("Mixer configuration and song files (*.%1 *.med *.med.gz *.med.bz2);;All files (*)").arg(mixerConfigSuffix));
  if(name.isEmpty())
    return;

  MusECore::MixerSnapshot snap;
  {
    MusEFile::File f(name, QString(".med"), parent);
    if(MusEGui::fileOpen(f, QIODevice::ReadOnly, parent, false, false) != MusEFile::File::NoError)
      return;
    QString error;
    const bool ok = snap.load(f.iodevice(), &error);
    f.close();
    if(!ok)
    {
      QMessageBox::critical(parent, title, error);
      return;
    }
  }

  // How well does the file fit this song? Find out before bothering the user with the choices.
  {
    std::vector<MusECore::MixerImportChoice> all;
    int withContent = 0;
    for(size_t i = 0; i < snap.tracks.size(); ++i)
    {
      if(snap.tracks[i].categories() == 0)
        continue;
      ++withContent;
      MusECore::MixerImportChoice c;
      c.source = int(i);
      c.target = MusECore::MixerSnapshot::findTarget(snap.tracks[i]);
      c.categories = MusECore::MixerAllCategories;
      all.push_back(c);
    }
    MusECore::MixerImportReport probe;
    snap.apply(all, true, probe);
    QString problem;
    if(withContent == 0)
      problem = QObject::tr("The file has no mixer settings.");
    else if(probe.tracksMatched == 0)
      problem = QObject::tr("None of the %1 tracks in the file has a track here with the same name and type.").arg(withContent);
    else if(probe.tracksMatched * 2 < withContent)
      problem = QObject::tr("Only %1 of the %2 tracks in the file have a track here with the same name and type.")
                  .arg(probe.tracksMatched).arg(withContent);
    if(!problem.isEmpty())
    {
      if(withContent == 0 || probe.tracksMatched == 0)
      {
        QMessageBox::warning(parent, title, problem + "\n" + QObject::tr("There is nothing that can be imported."));
        return;
      }
      if(QMessageBox::question(parent, title, problem + "\n" + QObject::tr("Continue anyway?"),
                               QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
        return;
    }
  }

  MixerImportDialog dlg(snap, parent);
  if(dlg.exec() != QDialog::Accepted)
    return;

  MusECore::MixerImportReport report;
  snap.apply(dlg.choices(), false, report);
  MixerImportReportDialog rdlg(report, parent);
  rdlg.exec();
}

//---------------------------------------------------------
//   MixerImportDialog
//---------------------------------------------------------

// The check state of an item that has children: Checked or unchecked if all of them are, otherwise partially checked.
static void updateAncestors(QTreeWidgetItem* item)
{
  for(QTreeWidgetItem* p = item ? item->parent() : nullptr; p; p = p->parent())
  {
    int checked = 0, unchecked = 0;
    for(int i = 0; i < p->childCount(); ++i)
    {
      const Qt::CheckState s = p->child(i)->checkState(0);
      if(s == Qt::Checked)
        ++checked;
      else if(s == Qt::Unchecked)
        ++unchecked;
      else
      {
        ++checked;
        ++unchecked;
      }
    }
    p->setCheckState(0, unchecked == 0 ? Qt::Checked : (checked == 0 ? Qt::Unchecked : Qt::PartiallyChecked));
  }
}

static void setDescendants(QTreeWidgetItem* item, Qt::CheckState state)
{
  for(int i = 0; i < item->childCount(); ++i)
  {
    item->child(i)->setCheckState(0, state);
    setDescendants(item->child(i), state);
  }
}

MixerImportDialog::MixerImportDialog(const MusECore::MixerSnapshot& snapshot, QWidget* parent)
  : QDialog(parent), _snapshot(snapshot)
{
  setWindowTitle(tr("Load Mixer Configuration"));
  resize(520, 560);
  QVBoxLayout* lay = new QVBoxLayout(this);
  lay->addWidget(new QLabel(tr("Choose what to import. Tracks are matched by name and type, no tracks are created."), this));

  _tree = new QTreeWidget(this);
  _tree->setHeaderHidden(true);
  lay->addWidget(_tree, 1);

  for(size_t i = 0; i < snapshot.tracks.size(); ++i)
  {
    const MusECore::MixerTrackConfig& t = snapshot.tracks[i];
    const unsigned cats = t.categories();
    if(cats == 0)
      continue;
    Row row;
    row.target = MusECore::MixerSnapshot::findTarget(t);
    QTreeWidgetItem* top = new QTreeWidgetItem(_tree);
    row.item = top;
    top->setData(0, Qt::UserRole, int(i));
    if(!row.target)
    {
      top->setText(0, tr("%1 (%2) - no matching track here").arg(t.name, trackTypeName(t.type)));
      top->setFlags(Qt::ItemIsEnabled);
      top->setForeground(0, palette().brush(QPalette::Disabled, QPalette::Text));
      _rows.push_back(row);
      continue;
    }
    top->setText(0, QString("%1 (%2)").arg(t.name, trackTypeName(t.type)));
    top->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
    top->setCheckState(0, Qt::Checked);
    for(int k = 0; k < numCats; ++k)
    {
      QTreeWidgetItem* ci = nullptr;
      if(cats & catInfo[k].bit)
      {
        ci = new QTreeWidgetItem(top);
        ci->setText(0, QCoreApplication::translate("MixerImportDialog", catInfo[k].name));
        ci->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        // The routes to the soundcard depend on the audio device, so they are off by default.
        ci->setCheckState(0, (catInfo[k].bit & (MusECore::MixerPortInputs | MusECore::MixerPortOutputs)) ? Qt::Unchecked : Qt::Checked);
        if(catInfo[k].bit == MusECore::MixerPlugins)
          for(const MusECore::MixerPluginConfig& p : t.plugins)
          {
            QTreeWidgetItem* pi = new QTreeWidgetItem(ci);
            pi->setText(0, tr("%1 (slot %2)").arg(p.name).arg(p.slot + 1));
            pi->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
            pi->setCheckState(0, Qt::Checked);
            row.plugins.push_back(pi);
          }
      }
      row.categories.push_back(ci);
    }
    top->setExpanded(false);
    _rows.push_back(row);
  }

  // The parents show partially checked when only some of what is below them is checked,
  //  also when they are collapsed. Set the initial states now that all items exist.
  for(const Row& r : _rows)
  {
    if(!r.target)
      continue;
    for(QTreeWidgetItem* c : r.categories)
      if(c && c->childCount() == 0)
        updateAncestors(c);
    for(QTreeWidgetItem* p : r.plugins)
      updateAncestors(p);
  }
  connect(_tree, &QTreeWidget::itemChanged, [this](QTreeWidgetItem* item, int column) {
    if(column != 0 || _updatingChecks || !(item->flags() & Qt::ItemIsUserCheckable))
      return;
    _updatingChecks = true;
    if(item->checkState(0) != Qt::PartiallyChecked)
      setDescendants(item, item->checkState(0));
    updateAncestors(item);
    _updatingChecks = false;
  });

  // Whole kinds of tracks, and the soundcard routing, at once.
  QGroupBox* typeBox = new QGroupBox(tr("Track types to import"), this);
  QHBoxLayout* typeLay = new QHBoxLayout(typeBox);
  static const MusECore::Track::TrackType types[] = { MusECore::Track::AUDIO_INPUT, MusECore::Track::WAVE,
    MusECore::Track::AUDIO_GROUP, MusECore::Track::AUDIO_AUX, MusECore::Track::AUDIO_SOFTSYNTH, MusECore::Track::AUDIO_OUTPUT };
  for(MusECore::Track::TrackType tt : types)
  {
    QCheckBox* cb = new QCheckBox(trackTypeName(tt), typeBox);
    cb->setChecked(true);
    typeLay->addWidget(cb);
    connect(cb, &QCheckBox::toggled, [this, tt](bool on) {
      // A kind of tracks that is not to be imported is removed from the tree.
      for(const Row& r : _rows)
        if(_snapshot.tracks[r.item->data(0, Qt::UserRole).toInt()].type == tt)
        {
          r.item->setHidden(!on);
          if(r.target)
            r.item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
        }
      applyPortStates();
    });
  }
  lay->addWidget(typeBox);

  QGroupBox* portBox = new QGroupBox(tr("Routing to the soundcard (depends on the audio device)"), this);
  QHBoxLayout* portLay = new QHBoxLayout(portBox);
  const struct { unsigned bit; const char* text; } ports[] = {
    { MusECore::MixerPortInputs,  QT_TR_NOOP("Input routing") },
    { MusECore::MixerPortOutputs, QT_TR_NOOP("Output routing") } };
  int portIdx = 0;
  for(const auto& p : ports)
  {
    const int pi = portIdx++;
    QCheckBox* cb = new QCheckBox(tr(p.text), portBox);
    cb->setChecked(false);
    portLay->addWidget(cb);
    const unsigned bit = p.bit;
    (void)bit;
    connect(cb, &QCheckBox::toggled, [this, pi](bool on) {
      _portOn[pi] = on;
      applyPortStates();
    });
  }
  lay->addWidget(portBox);

  QDialogButtonBox* box = new QDialogButtonBox(this);
  QPushButton* all = box->addButton(tr("Select all"), QDialogButtonBox::ActionRole);
  QPushButton* none = box->addButton(tr("Select none"), QDialogButtonBox::ActionRole);
  box->addButton(tr("Load"), QDialogButtonBox::AcceptRole);
  box->addButton(QDialogButtonBox::Cancel);
  connect(all, &QPushButton::clicked, [this]() { setAll(true); });
  connect(none, &QPushButton::clicked, [this]() { setAll(false); });
  connect(box, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
  lay->addWidget(box);
}

void MixerImportDialog::setAll(bool on)
{
  for(const Row& r : _rows)
    if(r.target && !r.item->isHidden())
      r.item->setCheckState(0, on ? Qt::Checked : Qt::Unchecked);
  applyPortStates();
}

void MixerImportDialog::applyPortStates()
{
  for(const Row& r : _rows)
    for(int k = 0; k < numCats && k < int(r.categories.size()); ++k)
    {
      if(!r.categories[k])
        continue;
      if(catInfo[k].bit == MusECore::MixerPortInputs)
        r.categories[k]->setCheckState(0, _portOn[0] ? Qt::Checked : Qt::Unchecked);
      else if(catInfo[k].bit == MusECore::MixerPortOutputs)
        r.categories[k]->setCheckState(0, _portOn[1] ? Qt::Checked : Qt::Unchecked);
    }
}

std::vector<MusECore::MixerImportChoice> MixerImportDialog::choices() const
{
  std::vector<MusECore::MixerImportChoice> out;
  for(const Row& r : _rows)
  {
    if(!r.target || r.item->isHidden())
      continue;
    MusECore::MixerImportChoice c;
    c.source = r.item->data(0, Qt::UserRole).toInt();
    c.target = r.target;
    for(int k = 0; k < numCats && k < int(r.categories.size()); ++k)
      if(r.categories[k] && r.categories[k]->checkState(0) != Qt::Unchecked)
        c.categories |= catInfo[k].bit;
    if(c.categories == 0)
      continue;
    for(QTreeWidgetItem* pi : r.plugins)
      c.plugins.push_back(pi->checkState(0) == Qt::Checked);
    if(c.categories & MusECore::MixerPlugins)
    {
      // An empty list means all plugins, so a list with nothing in it must not be given: Drop the category.
      bool any = false;
      for(bool b : c.plugins)
        any = any || b;
      if(!any)
        c.categories &= ~(MusECore::MixerPlugins | MusECore::MixerPluginAutomation);
    }
    if(c.categories)
      out.push_back(c);
  }
  return out;
}

//---------------------------------------------------------
//   MixerImportReportDialog
//---------------------------------------------------------

MixerImportReportDialog::MixerImportReportDialog(const MusECore::MixerImportReport& report, QWidget* parent)
  : QDialog(parent)
{
  setWindowTitle(tr("Mixer Load Report"));
  resize(680, 460);
  QVBoxLayout* lay = new QVBoxLayout(this);

  const int dev = report.count(MusECore::MixerImportReportItem::Deviation);
  const int done = report.count(MusECore::MixerImportReportItem::Done);
  QLabel* summary = new QLabel(this);
  summary->setWordWrap(true);
  if(dev == 0)
    summary->setText(tr("The import was done as planned: %1 item(s) in %2 track(s).").arg(done).arg(report.tracksMatched));
  else
    summary->setText(tr("<b>The import did not go entirely as planned.</b> %1 deviation(s), %2 item(s) done as planned, %3 track(s).")
                       .arg(dev).arg(done).arg(report.tracksMatched));
  lay->addWidget(summary);

  QTreeWidget* tree = new QTreeWidget(this);
  tree->setRootIsDecorated(false);
  tree->setAlternatingRowColors(true);
  tree->setHeaderLabels(QStringList() << tr("Result") << tr("Track") << tr("What") << tr("Note"));
  // The deviations first.
  for(int pass = 0; pass < 2; ++pass)
    for(const MusECore::MixerImportReportItem& it : report.items)
    {
      const bool isDev = it.result == MusECore::MixerImportReportItem::Deviation;
      if(isDev != (pass == 0))
        continue;
      QTreeWidgetItem* ti = new QTreeWidgetItem(tree);
      ti->setText(0, isDev ? tr("Deviation") : tr("Done"));
      ti->setText(1, it.track);
      ti->setText(2, it.what);
      ti->setText(3, it.note);
      if(isDev)
        for(int c = 0; c < 4; ++c)
        {
          QFont f = ti->font(c);
          f.setBold(true);
          ti->setFont(c, f);
          ti->setForeground(c, QColor(190, 40, 40));
        }
    }
  for(int c = 0; c < 3; ++c)
    tree->resizeColumnToContents(c);
  lay->addWidget(tree, 1);

  QDialogButtonBox* box = new QDialogButtonBox(QDialogButtonBox::Close, this);
  connect(box, &QDialogButtonBox::rejected, this, &QDialog::accept);
  lay->addWidget(box);
}

} // namespace MusEGui
