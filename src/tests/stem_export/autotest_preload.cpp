//=========================================================
//  MusE
//  Linux Music Editor
//
//  autotest_preload.cpp
//   Test helper for the stem export (see README). Loaded into MusE with LD_PRELOAD,
//   it drives the GUI: Audio > Export Stems..., fills in the dialog from environment
//   variables, presses Export, prints the final message and exits.
//
//   STEMTEST_DIR      folder for the stems
//   STEMTEST_MODE     0 = range markers, 1 = markers
//   STEMTEST_TRACKS   comma separated track names to check (all others unchecked)
//   STEMTEST_OUTPUT   1 = through the output track's effects
//
//  This program is free software; you can redistribute it and/or
//  modify it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include <cstdio>
#include <cstdlib>

#include <QAbstractButton>
#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QTimer>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>

static int state = 0;
static int ticks = 0;

static void log(const QString& s)
{
  fprintf(stdout, "AUTOTEST: %s\n", s.toLocal8Bit().constData());
  fflush(stdout);
}

static QMainWindow* mainWindow()
{
  for(QWidget* w : QApplication::topLevelWidgets())
    if(QMainWindow* mw = qobject_cast<QMainWindow*>(w))
      if(mw->isVisible() && mw->inherits("MusEGui::MusE"))
        return mw;
  return nullptr;
}

static void step()
{
  ++ticks;
  if(ticks > 1200) // 2 minutes
  {
    log("TIMEOUT in state " + QString::number(state));
    std::_Exit(2);
  }

  // Message boxes: The final report ends the test, others (startup warnings) are logged and closed.
  for(QWidget* w : QApplication::topLevelWidgets())
  {
    QMessageBox* mb = qobject_cast<QMessageBox*>(w);
    if(!mb || !mb->isVisible())
      continue;
    log("MESSAGE [" + mb->windowTitle() + "] " + mb->text());
    if(state == 3 && mb->windowTitle() == "Export Stems")
    {
      log("DONE");
      std::_Exit(0);
    }
    mb->done(QMessageBox::Ok);
    return;
  }

  switch(state)
  {
    case 0:
    {
      // Startup dialogs are in the way.
      for(QWidget* w : QApplication::topLevelWidgets())
      {
        QDialog* d = qobject_cast<QDialog*>(w);
        // Progress dialogs (loading the song) go away by themselves.
        if(d && d->isVisible() && !d->inherits("QProgressDialog"))
        {
          log("close dialog [" + d->windowTitle() + "] " + d->metaObject()->className());
          d->reject();
          return;
        }
      }
      // Let the song load and the audio run for a moment.
      QMainWindow* mw = mainWindow();
      if(!mw || ticks < 30)
        return;
      for(QAction* a : mw->findChildren<QAction*>())
      {
        if(a->text() == "Export Stems...")
        {
          state = 1;
          log("trigger Export Stems");
          // The dialog runs a nested event loop, this timer goes on in it.
          QTimer::singleShot(0, a, &QAction::trigger);
          return;
        }
      }
      log("no Export Stems action");
      std::_Exit(2);
    }

    case 1:
    {
      for(QWidget* w : QApplication::topLevelWidgets())
      {
        QDialog* d = qobject_cast<QDialog*>(w);
        if(!d || !d->isVisible() || d->objectName() != "StemExportDialogBase")
          continue;
        d->findChild<QComboBox*>("comboMode")->setCurrentIndex(qgetenv("STEMTEST_MODE").toInt());
        d->findChild<QComboBox*>("comboFormat")->setCurrentIndex(0); // wav 16 bit
        d->findChild<QComboBox*>("comboChannel")->setCurrentIndex(0); // stereo
        d->findChild<QLineEdit*>("editPath")->setText(QString::fromLocal8Bit(qgetenv("STEMTEST_DIR")));
        d->findChild<QCheckBox*>("checkOutput")->setChecked(qgetenv("STEMTEST_OUTPUT") == "1");
        const QStringList want = QString::fromLocal8Bit(qgetenv("STEMTEST_TRACKS")).split(',');
        QTreeWidget* tree = d->findChild<QTreeWidget*>("treeTracks");
        for(QTreeWidgetItemIterator it(tree); *it; ++it)
        {
          QTreeWidgetItem* item = *it;
          log("list: " + item->text(0) + " | " + item->text(1) +
              ((item->flags() & Qt::ItemIsEnabled) ? "" : " (disabled: " + item->toolTip(0) + ")"));
          if(item->flags() & Qt::ItemIsUserCheckable && !item->text(1).startsWith("Folder"))
            item->setCheckState(0, want.contains(item->text(0)) ? Qt::Checked : Qt::Unchecked);
        }
        state = 2;
        log("press Export");
        QTimer::singleShot(0, d->findChild<QPushButton*>("buttonOk"), &QAbstractButton::click);
        return;
      }
      return;
    }

    case 2:
      state = 3;
      return;

    default:
      return;
  }
}

static void startup()
{
  QTimer* t = new QTimer(qApp);
  QObject::connect(t, &QTimer::timeout, step);
  t->start(100);
}

Q_COREAPP_STARTUP_FUNCTION(startup)
