//=========================================================
//  MusE
//  Linux Music Editor
//  $Id: midisyncimpl.cpp,v 1.9 2005/12/07 19:23:29 wschweer Exp $
//
//  (C) Copyright 1999/2000 Werner Schweer (ws@seh.de)
//=========================================================

#include "al/al.h"
#include "midisync.h"
#include "sync.h"

//---------------------------------------------------------
//   MidiSyncConfig
//    Midi Sync Config
//---------------------------------------------------------

MidiSyncConfig::MidiSyncConfig(QWidget* parent)
  : QDialog(parent)
      {
      setupUi(this);
      connect(okButton, SIGNAL(clicked()), SLOT(ok()));
      connect(applyButton, SIGNAL(clicked()), SLOT(apply()));
      connect(cancelButton, SIGNAL(clicked()), SLOT(cancel()));

      connect(syncMaster, SIGNAL(toggled(bool)), SLOT(syncMasterChanged(bool)));
      connect(syncSlave, SIGNAL(toggled(bool)), SLOT(syncSlaveChanged(bool)));

      dstDevId->setValue(txDeviceId);
      srcDevId->setValue(rxDeviceId);

      mtcSync->setChecked(genMTCSync);
      mcSync->setChecked(genMCSync);
      midiMachineControl->setChecked(genMMC);

      acceptMTCCheckbox->setChecked(acceptMTC);
      acceptMCCheckbox->setChecked(acceptMC);
      acceptMMCCheckbox->setChecked(acceptMMC);

      mtcSyncType->setCurrentIndex(AL::mtcType);

      mtcOffH->setValue(mtcOffset.h());
      mtcOffM->setValue(mtcOffset.m());
      mtcOffS->setValue(mtcOffset.s());
      mtcOffF->setValue(mtcOffset.f());
      mtcOffSf->setValue(mtcOffset.sf());

      syncSlaveChanged(extSyncFlag);
      }

//---------------------------------------------------------
//   ok Pressed
//---------------------------------------------------------

void MidiSyncConfig::ok()
      {
      apply();
      cancel();
      }

//---------------------------------------------------------
//   cancel Pressed
//---------------------------------------------------------

void MidiSyncConfig::cancel()
      {
      close();
      }

//---------------------------------------------------------
//   apply Pressed
//---------------------------------------------------------

void MidiSyncConfig::apply()
      {
      txDeviceId  = dstDevId->value();
      rxDeviceId  = srcDevId->value();

      genMTCSync  = mtcSync->isChecked();
      genMCSync   = mcSync->isChecked();
      genMMC      = midiMachineControl->isChecked();

      AL::mtcType = mtcSyncType->currentIndex();
      extSyncFlag = syncSlave->isChecked();

      mtcOffset.setH(mtcOffH->value());
      mtcOffset.setM(mtcOffM->value());
      mtcOffset.setS(mtcOffS->value());
      mtcOffset.setF(mtcOffF->value());
      mtcOffset.setSf(mtcOffSf->value());

      acceptMC  = acceptMCCheckbox->isChecked();
      acceptMMC = acceptMMCCheckbox->isChecked();
      acceptMTC = acceptMTCCheckbox->isChecked();
      emit syncChanged();
      }

//---------------------------------------------------------
//   syncMasterChanged
//---------------------------------------------------------

void MidiSyncConfig::syncMasterChanged(bool val)
      {
      syncSlave->setChecked(!val);
      }

//---------------------------------------------------------
//   syncSlaveChanged
//---------------------------------------------------------

void MidiSyncConfig::syncSlaveChanged(bool val)
      {
      syncMaster->setChecked(!val);
      }


