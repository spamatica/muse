//=========================================================
//  MusE
//  Linux Music Editor
//  $Id: mstrip.cpp,v 1.70 2006/01/12 14:49:13 wschweer Exp $
//
//  (C) Copyright 2000-2005 Werner Schweer (ws@seh.de)
//=========================================================

#include "midictrl.h"
#include "mstrip.h"
#include "audio.h"
#include "song.h"
#include "mixer.h"
#include "widgets/simplebutton.h"
#include "widgets/utils.h"
#include "driver/alsamidi.h"
#include "synth.h"
#include "midirack.h"
#include "midiplugin.h"

#include "awl/midimslider.h"
#include "awl/midimeter.h"
#include "awl/midivolentry.h"
#include "awl/midipanentry.h"
#include "awl/midipanknob.h"
#include "awl/knob.h"

enum { KNOB_PAN, KNOB_CHOR_SEND, KNOB_VAR_SEND, KNOB_REV_SEND };

//---------------------------------------------------------
//   addKnob
//---------------------------------------------------------

void MidiChannelStrip::addKnob(int ctrl, int idx, const QString& tt, const QString& label,
   const char* slot, bool enabled)
      {
      Awl::FloatEntry* dl;
      Awl::Knob* knob;

      if (idx == KNOB_PAN) {
            dl = new Awl::MidiPanEntry(this);
            knob = new Awl::MidiPanKnob(this);
            }
      else {
            dl = new Awl::MidiVolEntry(this);
            knob = new Awl::Knob(this);
            knob->setRange(0.0, 127.0);
            }
      knob->setId(ctrl);
      dl->setId(ctrl);

      controller[idx].knob = knob;
      knob->setFixedSize(buttonSize.width(), entrySize.height() * 2);
      knob->setToolTip(tt);
      knob->setEnabled(enabled);

      controller[idx].dl = dl;
      dl->setFont(*config.fonts[1]);
      dl->setFixedSize(entrySize);
      dl->setEnabled(enabled);

      QLabel* lb = new QLabel(label, this);
      controller[idx].lb = lb;
      lb->setFont(*config.fonts[1]);
      lb->setFixedSize(entrySize);
      lb->setAlignment(Qt::AlignCenter);
      lb->setEnabled(enabled);

      QGridLayout* grid = new QGridLayout;
      grid->setMargin(0);
      grid->setSpacing(0);
      grid->addWidget(lb, 0, 0);
      grid->addWidget(dl, 1, 0);
      grid->addWidget(knob, 0, 1, 2, 1);
      layout->addLayout(grid);

      connect(knob, SIGNAL(valueChanged(float,int)), slot);
      connect(dl, SIGNAL(valueChanged(float,int)), slot);
      connect(knob, SIGNAL(sliderPressed(int)), SLOT(sliderPressed(int)));
      connect(knob, SIGNAL(sliderReleased(int)), SLOT(sliderReleased(int)));
      }

//---------------------------------------------------------
//   MidiChannelStrip
//---------------------------------------------------------

MidiChannelStrip::MidiChannelStrip(Mixer* m, MidiChannel* t, bool align)
   : Strip(m, t, align)
      {
      volumeTouched     = false;
      panTouched        = false;
      reverbSendTouched = false;
      variSendTouched   = false;
      chorusSendTouched = false;

      addKnob(CTRL_VARIATION_SEND, KNOB_VAR_SEND, tr("VariationSend"), tr("Var"), SLOT(ctrlChanged(float,int)), true);
      addKnob(CTRL_REVERB_SEND, KNOB_REV_SEND, tr("ReverbSend"), tr("Rev"), SLOT(ctrlChanged(float,int)), true);
      addKnob(CTRL_CHORUS_SEND, KNOB_CHOR_SEND, tr("ChorusSend"), tr("Cho"), SLOT(ctrlChanged(float,int)), true);

      int auxsSize = song->auxs()->size();
      if (auxsSize)
            layout->addSpacing((STRIP_WIDTH/2 + 1) * auxsSize);

      //---------------------------------------------------
      //    slider, label, meter
      //---------------------------------------------------

      slider = new Awl::MidiMeterSlider(this);
      slider->setId(CTRL_VOLUME);
      slider->setFixedWidth(40);
      layout->addWidget(slider, 100, Qt::AlignRight);

      sl = new Awl::MidiVolEntry(this);
      sl->setId(CTRL_VOLUME);
      sl->setFont(*config.fonts[1]);

      connect(slider, SIGNAL(valueChanged(float,int)), SLOT(ctrlChanged(float, int)));
      connect(slider, SIGNAL(sliderPressed(int)), SLOT(sliderPressed(int)));
      connect(slider, SIGNAL(sliderReleased(int)), SLOT(sliderReleased(int)));
      connect(sl,     SIGNAL(valueChanged(float,int)), SLOT(ctrlChanged(float, int)));
      layout->addWidget(sl);

      //---------------------------------------------------
      //    pan, balance
      //---------------------------------------------------

      addKnob(CTRL_PANPOT, KNOB_PAN, tr("Pan/Balance"), tr("Pan"), SLOT(ctrlChanged(float,int)), true);

      //---------------------------------------------------
      //    ---   record
      //    mute, solo
      //---------------------------------------------------

      if (_align)
            layout->addSpacing(STRIP_WIDTH/3);

      mute  = newMuteButton(this);
      mute->setChecked(track->isMute());
      mute->setFixedSize(buttonSize);
      connect(mute, SIGNAL(clicked(bool)), SLOT(muteToggled(bool)));

      solo  = newSoloButton(this);
      solo->setFixedSize(buttonSize);
      solo->setChecked(track->solo());
      connect(solo, SIGNAL(clicked(bool)), SLOT(soloToggled(bool)));

      QHBoxLayout* smBox2 = new QHBoxLayout(0);

      smBox2->addWidget(mute);
      smBox2->addWidget(solo);

      layout->addLayout(smBox2);

      //---------------------------------------------------
      //    automation mode
      //---------------------------------------------------

      addAutomationButtons();

      //---------------------------------------------------
      //    routing
      //---------------------------------------------------

      QHBoxLayout* rBox = new QHBoxLayout(0);
      iR = new QToolButton(this);
      iR->setCheckable(false);
      iR->setFont(*config.fonts[1]);
      iR->setFixedWidth((STRIP_WIDTH-4)/2);
      iR->setText(tr("iR"));
      iR->setToolTip(tr("input routing"));
      rBox->addWidget(iR);
      connect(iR, SIGNAL(pressed()), SLOT(iRoutePressed()));
      rBox->addStretch(100);
      layout->addLayout(rBox);

      connect(heartBeatTimer, SIGNAL(timeout()), SLOT(heartBeat()));
      connect(song,  SIGNAL(songChanged(int)), SLOT(songChanged(int)));
      connect(track, SIGNAL(muteChanged(bool)), mute, SLOT(setChecked(bool)));
      connect(track, SIGNAL(soloChanged(bool)), solo, SLOT(setChecked(bool)));
      connect(track, SIGNAL(autoReadChanged(bool)),  SLOT(autoChanged()));
      connect(track, SIGNAL(autoWriteChanged(bool)), SLOT(autoChanged()));
      connect(track, SIGNAL(controllerChanged(int)), SLOT(controllerChanged(int)));
      autoChanged();
      }

//---------------------------------------------------------
//   songChanged
//---------------------------------------------------------

void MidiChannelStrip::songChanged(int val)
      {
      if (val & SC_TRACK_MODIFIED)
            updateLabel();
      }

//---------------------------------------------------------
//   heartBeat
//---------------------------------------------------------

void MidiChannelStrip::heartBeat()
      {
      float a = track->meter(0); // fast_log10(track->meter(0)) * .2f;
      slider->setMeterVal(a * 0.008);
      track->setMeter(0, a * 0.8);  // hack
      }

//---------------------------------------------------------
//   controllerChanged
//---------------------------------------------------------

void MidiChannelStrip::controllerChanged(int id)
      {
      float val = float(track->ctrlVal(id).i);

      switch (id) {
            case CTRL_VOLUME:
                  if (!volumeTouched)
                        slider->setValue(val);
             	sl->setValue(val);
                  break;
            case CTRL_PANPOT:
                  if (!panTouched)
                        controller[KNOB_PAN].knob->setValue(val);
              	controller[KNOB_PAN].dl->setValue(val);
                  break;
            case CTRL_VARIATION_SEND:
                  if (!variSendTouched)
                        controller[KNOB_VAR_SEND].knob->setValue(val);
             	controller[KNOB_VAR_SEND].dl->setValue(val);
                  break;
            case CTRL_REVERB_SEND:
                  if (!reverbSendTouched)
                        controller[KNOB_REV_SEND].knob->setValue(val);
             	controller[KNOB_REV_SEND].dl->setValue(val);
                  break;
            case CTRL_CHORUS_SEND:
                  if (!chorusSendTouched)
                        controller[KNOB_CHOR_SEND].knob->setValue(val);
                  controller[KNOB_CHOR_SEND].dl->setValue(val);
                  break;
            }
      }

//---------------------------------------------------------
//   ctrlChanged
//	called when user changes controller
//---------------------------------------------------------

void MidiChannelStrip::ctrlChanged(float val, int num)
      {
      int ival = int(val);
      CVal cval;
      cval.i = ival;
      song->setControllerVal(track, num, cval);
      }

//---------------------------------------------------------
//   sliderPressed
//---------------------------------------------------------

void MidiChannelStrip::sliderPressed(int id)
      {
      switch (id) {
            case CTRL_VOLUME:         volumeTouched = true;     break;
            case CTRL_PANPOT:         panTouched = true;        break;
            case CTRL_VARIATION_SEND: variSendTouched = true;   break;
            case CTRL_REVERB_SEND:    reverbSendTouched = true; break;
            case CTRL_CHORUS_SEND:    chorusSendTouched = true; break;
            }
      track->startAutoRecord(id);
      }

//---------------------------------------------------------
//   sliderReleased
//---------------------------------------------------------

void MidiChannelStrip::sliderReleased(int id)
      {
      switch (id) {
            case CTRL_VOLUME:         volumeTouched = false;     break;
            case CTRL_PANPOT:         panTouched = false;        break;
            case CTRL_VARIATION_SEND: variSendTouched = false;   break;
            case CTRL_REVERB_SEND:    reverbSendTouched = false; break;
            case CTRL_CHORUS_SEND:    chorusSendTouched = false; break;
            }
      track->stopAutoRecord(id);
      }

//---------------------------------------------------------
//   muteToggled
//---------------------------------------------------------

void MidiChannelStrip::muteToggled(bool val)
      {
      song->setMute(track, val);
      }

//---------------------------------------------------------
//   soloToggled
//---------------------------------------------------------

void MidiChannelStrip::soloToggled(bool val)
      {
      song->setSolo(track, val);
      }

//---------------------------------------------------------
//   autoChanged
//---------------------------------------------------------

void MidiChannelStrip::autoChanged()
      {
      bool ar = track->autoRead();
      bool aw = track->autoWrite();

      //  controller are enabled if
      //    autoRead is off
      //    autoRead and autoWrite are on (touch mode)

      bool ec = !ar || (ar && aw);
      for (unsigned i = 0; i < sizeof(controller)/sizeof(*controller); ++i) {
            controller[i].knob->setEnabled(ec);
            controller[i].dl->setEnabled(ec);
            }
      slider->setEnabled(ec);
      sl->setEnabled(ec);
      }

//---------------------------------------------------------
//   autoReadToggled
//---------------------------------------------------------

void MidiChannelStrip::autoReadToggled(bool val)
      {
      song->setAutoRead(track, val);
      }

//---------------------------------------------------------
//   autoWriteToggled
//---------------------------------------------------------

void MidiChannelStrip::autoWriteToggled(bool val)
      {
      song->setAutoWrite(track, val);
      }

//---------------------------------------------------------
//   iRoutePressed
//---------------------------------------------------------

void MidiChannelStrip::iRoutePressed()
      {
      QMenu* pup = new QMenu(iR);
      pup->addSeparator()->setText(tr("Tracks"));

      MidiChannel* t = (MidiChannel*)track;
      RouteList* irl = t->inRoutes();

      MidiTrackList* tl = song->midis();
      int tn = 0;
      for (iMidiTrack i = tl->begin();i != tl->end(); ++i, ++tn) {
            QAction* id = pup->addAction((*i)->name());
            id->setData(QVariant(0));
            Route dst(*i, -1, Route::TRACK);
            for (iRoute ir = irl->begin(); ir != irl->end(); ++ir) {
                  if (*ir == dst) {
                              id->setData(QVariant(1));
                              id->setCheckable(true);
                              id->setChecked(true);
                              break;
                        }
                  }
            }

      QAction* n = pup->exec(QCursor::pos());
      if (n) {//if (n != -1) {
            int was_checked = n->data().toInt(); // isChecked appears to always give false, storing in QVariant instead
            QString s(n->text());
            Route dstRoute(t, -1, Route::TRACK);
            Route srcRoute(s, -1, Route::TRACK);


            if (was_checked == 1) {
                  audio->msgRemoveRoute(srcRoute, dstRoute);
                  }
            else {
                  audio->msgAddRoute(srcRoute, dstRoute);
                  }
            song->update(SC_ROUTE);
            }
      delete pup;
      iR->setDown(false);     // pup->exec() catches mouse release event
      }

//---------------------------------------------------------
//   MidiStrip
//---------------------------------------------------------

MidiStrip::MidiStrip(Mixer* m, MidiTrack* t, bool align)
   : Strip(m, t, align)
      {
      int auxsSize = song->auxs()->size();
      if (_align)
            layout->addSpacing((STRIP_WIDTH/2 + 1) * auxsSize + STRIP_WIDTH/2 * 3);

      //---------------------------------------------------
      //    slider, label, meter
      //---------------------------------------------------

      meter = new Awl::MidiMeter(this);
      meter->setId(CTRL_VOLUME);
      meter->setFixedWidth(40);
      layout->addWidget(meter, 100, Qt::AlignRight);

      if (_align)
            layout->addSpacing(LABEL_HEIGHT);

      //---------------------------------------------------
      //    pan, balance
      //---------------------------------------------------

      if (_align)
            layout->addSpacing(STRIP_WIDTH/2 + 1);

      //---------------------------------------------------
      //    mute, solo
      //    or
      //    record, mixdownfile
      //---------------------------------------------------

      SimpleButton* record = newRecordButton(this);
      record->setFixedSize(buttonSize);
      record->setChecked(track->recordFlag());
      connect(record, SIGNAL(clicked(bool)), SLOT(recordToggled(bool)));
      connect(t, SIGNAL(recordChanged(bool)), record, SLOT(setChecked(bool)));

      mute  = newMuteButton(this);
      mute->setChecked(track->isMute());
      mute->setFixedSize(buttonSize);
      connect(mute, SIGNAL(clicked(bool)), SLOT(muteToggled(bool)));

      solo  = newSoloButton(this);
      solo->setFixedSize(buttonSize);
      solo->setChecked(track->solo());
      connect(solo, SIGNAL(clicked(bool)), SLOT(soloToggled(bool)));

      QHBoxLayout* smBox1 = new QHBoxLayout(0);
      QHBoxLayout* smBox2 = new QHBoxLayout(0);

      smBox2->addWidget(mute);
      smBox2->addWidget(solo);

      smBox1->addStretch(100);
      smBox1->addWidget(record);
      layout->addLayout(smBox1);
      layout->addLayout(smBox2);

      //---------------------------------------------------
      //    automation mode
      //---------------------------------------------------

      if (_align)
            layout->addSpacing(STRIP_WIDTH/3);

      //---------------------------------------------------
      //    routing
      //---------------------------------------------------

      QHBoxLayout* rBox = new QHBoxLayout(0);
      iR = new QToolButton(this);
      iR->setFont(*config.fonts[1]);
      iR->setFixedWidth((STRIP_WIDTH-4)/2);
      iR->setText(tr("iR"));
      iR->setCheckable(false);
      iR->setToolTip(tr("input routing"));
      rBox->addWidget(iR);
      connect(iR, SIGNAL(pressed()), SLOT(iRoutePressed()));

      oR = new QToolButton(this);
      oR->setFont(*config.fonts[1]);
      oR->setFixedWidth((STRIP_WIDTH-4)/2);
      oR->setText(tr("oR"));
      oR->setCheckable(false);
      oR->setToolTip(tr("output routing"));
      rBox->addWidget(oR);
      connect(oR, SIGNAL(pressed()), SLOT(oRoutePressed()));

      layout->addLayout(rBox);

      connect(heartBeatTimer, SIGNAL(timeout()), SLOT(heartBeat()));
      connect(song,  SIGNAL(songChanged(int)), SLOT(songChanged(int)));
      connect(track, SIGNAL(muteChanged(bool)), mute, SLOT(setChecked(bool)));
      connect(track, SIGNAL(soloChanged(bool)), solo, SLOT(setChecked(bool)));
      }

//---------------------------------------------------------
//   songChanged
//---------------------------------------------------------

void MidiStrip::songChanged(int val)
      {
      if (val & SC_TRACK_MODIFIED)
            updateLabel();
      }

//---------------------------------------------------------
//   heartBeat
//---------------------------------------------------------

void MidiStrip::heartBeat()
      {
      float a = track->meter(0); // fast_log10(track->meter(0)) * .2f;
      meter->setMeterVal(a * 0.008);
      track->setMeter(0, a * 0.8);  // hack
      }

//---------------------------------------------------------
//   muteToggled
//---------------------------------------------------------

void MidiStrip::muteToggled(bool val)
      {
      song->setMute(track, val);
      }

//---------------------------------------------------------
//   soloToggled
//---------------------------------------------------------

void MidiStrip::soloToggled(bool val)
      {
      song->setSolo(track, val);
      }

//---------------------------------------------------------
//   recordToggled
//---------------------------------------------------------

void MidiStrip::recordToggled(bool val)
      {
      song->setRecordFlag(track, !val);
      }

//---------------------------------------------------------
//   iRoutePressed
//---------------------------------------------------------

void MidiStrip::iRoutePressed()
      {
      QMenu* pup = new QMenu(oR);
      pup->addSeparator()->setText(tr("Input Ports"));
      MidiOutPort* t = (MidiOutPort*)track;
      RouteList* irl = t->inRoutes();

      MidiInPortList* ipl = song->midiInPorts();
      int tn = 0;
      for (iMidiInPort i = ipl->begin(); i != ipl->end(); ++i, ++tn) {
            QMenu* m = pup->addMenu((*i)->name());
            m->addSeparator()->setText(tr("Channel"));
            QAction* all_action = m->addAction(tr("All"));
            QMap<QString, QVariant> map;
            map["was_checked"] = false;
            map["id"] = tn * 32 + MIDI_CHANNELS;
            all_action->setData(map);

            for (int channel = 0; channel < MIDI_CHANNELS; ++channel) {
                  QString s;
                  s.setNum(channel+1);
                  QAction* channel_action = m->addAction(s);
                  QMap<QString, QVariant> cmap;
                  cmap["was_checked"] = false;
                  cmap["id"] = tn * 32 + channel; // trackno + channel as id
                  channel_action->setData(cmap);
                  Route src(*i, channel, Route::TRACK);
                  for (iRoute ir = irl->begin(); ir != irl->end(); ++ir) {
                        if (*ir == src) {
                              cmap["was_checked"] = true;
                              channel_action->setData(cmap);
                              channel_action->setCheckable(true);
                              channel_action->setChecked(true);
                              break;
                              }
                        }
                  }
            }

      QAction* n = pup->exec(QCursor::pos());
      if (n) {
            QMap<QString, QVariant> data = n->data().toMap();
            int trackid = data["id"].toInt();
            int was_checked = data["was_checked"].toInt();
            int port = trackid >> 5;
            int channel = trackid & 0x1f;

            MidiInPort* mip = ipl->index(port);
            if (channel == MIDI_CHANNELS) {
                  for (channel = 0; channel < MIDI_CHANNELS; ++channel) {
                        Route srcRoute(mip, channel, Route::TRACK);
                        Route dstRoute(track, channel, Route::TRACK);
                        audio->msgAddRoute(srcRoute, dstRoute);
                        }
                  }
            else {
                  Route srcRoute(mip, channel, Route::TRACK);
                  Route dstRoute(track, channel, Route::TRACK);

                  if (was_checked == 1)
                        audio->msgRemoveRoute(srcRoute, dstRoute);
                  else
                        audio->msgAddRoute(srcRoute, dstRoute);
                  }
            song->update(SC_ROUTE);
            }
      delete pup;
      iR->setDown(false);     // pup->exec() catches mouse release event
      }

//---------------------------------------------------------
//   oRoutePressed
//---------------------------------------------------------

void MidiStrip::oRoutePressed()
      {
      QMenu* pup = new QMenu(oR);
      pup->addSeparator()->setText(tr("OutputPorts"));
      RouteList* orl = track->outRoutes();

      MidiOutPortList* mpl = song->midiOutPorts();
      int pn = 0;
      for (iMidiOutPort i = mpl->begin(); i != mpl->end(); ++i, ++pn) {
            MidiOutPort* op = *i;
            QMenu* m = pup->addMenu(op->name());
            m->addSeparator()->setText(tr("Channel"));
            for (int channel = 0; channel < MIDI_CHANNELS; ++channel) {
                  QString s;
                  s.setNum(channel+1);
                  QMap<QString, QVariant> data;
                  QAction* action = m->addAction(s); //int id = m->insertItem(s, pn * 16 + channel);
                  data["was_checked"] = false;
                  data["trackid"] = pn * 16 + channel;
                  action->setData(data);

                  MidiChannel* mc = op->channel(channel);
                  Route r(mc, -1, Route::TRACK);
                  for (iRoute ir = orl->begin(); ir != orl->end(); ++ir) {
                        if (r == *ir) {
                              action->setCheckable(true);
                              action->setChecked(true);
                              data["was_checked"] = true;
                              action->setData(data);
                              break;
                              }
                        }
                  }
            }
      QAction* n = pup->exec(QCursor::pos());
      if (n) {
            QMap<QString, QVariant> data = n->data().toMap();
            int trackid = data["trackid"].toInt();
            int was_checked = data["was_checked"].toInt();
            int portno  = trackid >> 4;
            int channel = trackid & 0xf;
            MidiOutPort* mp = mpl->index(portno);
            MidiChannel* mc = mp->channel(channel);
            Route srcRoute(track, -1, Route::TRACK);
            Route dstRoute(mc, -1, Route::TRACK);

            if (was_checked == 1)
                  audio->msgRemoveRoute(srcRoute, dstRoute);
            else
                  audio->msgAddRoute(srcRoute, dstRoute);

            song->update(SC_ROUTE);
            }
      delete pup;
      oR->setDown(false);     // pup->exec() catches mouse release event
      }

//---------------------------------------------------------
//   MidiOutPortStrip
//---------------------------------------------------------

MidiOutPortStrip::MidiOutPortStrip(Mixer* m, MidiOutPort* t, bool align)
   : Strip(m, t, align)
      {
      int auxsSize = song->auxs()->size();
      if (_align)
            layout->addSpacing((STRIP_WIDTH/2 + 1) * auxsSize + STRIP_WIDTH/2 * 3);

      volumeTouched = false;

      //---------------------------------------------------
      //    slider, label, meter
      //---------------------------------------------------

      slider = new Awl::MidiMeterSlider(this);
      slider->setId(CTRL_MASTER_VOLUME);
      slider->setRange(0.0, 1024*16.0);
      slider->setFixedWidth(40);
      layout->addWidget(slider, 100, Qt::AlignRight);

      sl = new Awl::MidiVolEntry(this);
      sl->setId(CTRL_MASTER_VOLUME);
      sl->setMax(128 * 128 - 1);
      sl->setFont(*config.fonts[1]);

      controllerChanged(CTRL_MASTER_VOLUME);

      connect(slider, SIGNAL(valueChanged(float,int)), SLOT(ctrlChanged(float, int)));
      connect(slider, SIGNAL(sliderPressed(int)), SLOT(sliderPressed(int)));
      connect(slider, SIGNAL(sliderReleased(int)), SLOT(sliderReleased(int)));
      connect(sl,     SIGNAL(valueChanged(float,int)), SLOT(ctrlChanged(float, int)));
      layout->addWidget(sl);

      //---------------------------------------------------
      //    pan, balance
      //---------------------------------------------------

      if (_align)
            layout->addSpacing(entrySize.height() * 2);

      //---------------------------------------------------
      //    sync
      //    mute, solo
      //---------------------------------------------------

      sync = newSyncButton(this);
      sync->setFixedHeight(buttonSize.height());
      sync->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
      sync->setChecked(((MidiOutPort*)track)->sendSync());
      layout->addWidget(sync);
      connect(sync, SIGNAL(clicked(bool)), SLOT(syncToggled(bool)));
      connect(track, SIGNAL(sendSyncChanged(bool)), sync, SLOT(setChecked(bool)));

      mute  = newMuteButton(this);
      mute->setChecked(track->isMute());
      mute->setFixedSize(buttonSize);
      connect(mute, SIGNAL(clicked(bool)), SLOT(muteToggled(bool)));

      solo  = newSoloButton(this);
      solo->setFixedSize(buttonSize);
      solo->setChecked(track->solo());
      connect(solo, SIGNAL(clicked(bool)), SLOT(soloToggled(bool)));

      QHBoxLayout* smBox2 = new QHBoxLayout(0);

      smBox2->addWidget(mute);
      smBox2->addWidget(solo);

      layout->addLayout(smBox2);

      //---------------------------------------------------
      //    automation mode
      //---------------------------------------------------

      addAutomationButtons();

      //---------------------------------------------------
      //    output routing
      //---------------------------------------------------

      QHBoxLayout* rBox = new QHBoxLayout(0);
      rBox->addStretch(100);

      oR = new QToolButton(this);
      oR->setFont(*config.fonts[1]);
      oR->setFixedWidth((STRIP_WIDTH-4)/2);
      oR->setText(tr("oR"));
      oR->setCheckable(false);
      oR->setToolTip(tr("output routing"));
      rBox->addWidget(oR);
      connect(oR, SIGNAL(pressed()), SLOT(oRoutePressed()));

      layout->addLayout(rBox);

      connect(heartBeatTimer, SIGNAL(timeout()), SLOT(heartBeat()));
      connect(song,  SIGNAL(songChanged(int)), SLOT(songChanged(int)));
      connect(track, SIGNAL(muteChanged(bool)), mute, SLOT(setChecked(bool)));
      connect(track, SIGNAL(soloChanged(bool)), solo, SLOT(setChecked(bool)));
      connect(track, SIGNAL(autoReadChanged(bool)), SLOT(autoChanged()));
      connect(track, SIGNAL(autoWriteChanged(bool)), SLOT(autoChanged()));
      autoChanged();
      }

//---------------------------------------------------------
//   songChanged
//---------------------------------------------------------

void MidiOutPortStrip::songChanged(int val)
      {
      if (val & SC_TRACK_MODIFIED)
            updateLabel();
      }

//---------------------------------------------------------
//   heartBeat
//---------------------------------------------------------

void MidiOutPortStrip::heartBeat()
      {
      float a = track->meter(0); // fast_log10(track->meter(0)) * .2f;
      slider->setMeterVal(a * 0.008);
      track->setMeter(0, a * 0.8);  // hack
      }

//---------------------------------------------------------
//   ctrlChanged
//---------------------------------------------------------

void MidiOutPortStrip::ctrlChanged(float val, int num)
      {
      int ival = int(val);
      CVal cval;
      cval.i = ival;
      song->setControllerVal(track, num, cval);
      }

//---------------------------------------------------------
//   sliderPressed
//---------------------------------------------------------

void MidiOutPortStrip::sliderPressed(int id)
      {
      switch (id) {
            case CTRL_MASTER_VOLUME:  volumeTouched = true;     break;
            }
      track->startAutoRecord(id);
      }

//---------------------------------------------------------
//   sliderReleased
//---------------------------------------------------------

void MidiOutPortStrip::sliderReleased(int id)
      {
      switch (id) {
            case CTRL_MASTER_VOLUME: volumeTouched = false;     break;
            }
      track->stopAutoRecord(id);
      }

//---------------------------------------------------------
//   muteToggled
//---------------------------------------------------------

void MidiOutPortStrip::muteToggled(bool val)
      {
      song->setMute(track, val);
      }

//---------------------------------------------------------
//   soloToggled
//---------------------------------------------------------

void MidiOutPortStrip::soloToggled(bool val)
      {
      song->setSolo(track, val);
      }

//---------------------------------------------------------
//   autoChanged
//---------------------------------------------------------

void MidiOutPortStrip::autoChanged()
      {
      bool ar = track->autoRead();
      bool aw = track->autoWrite();

      //  controller are enabled if
      //    autoRead is off
      //    autoRead and autoWrite are on (touch mode)

      bool ec = !ar || (ar && aw);
      slider->setEnabled(ec);
      sl->setEnabled(ec);
      }

//---------------------------------------------------------
//   autoReadToggled
//---------------------------------------------------------

void MidiOutPortStrip::autoReadToggled(bool val)
      {
      song->setAutoRead(track, val);
      }

//---------------------------------------------------------
//   autoWriteToggled
//---------------------------------------------------------

void MidiOutPortStrip::autoWriteToggled(bool val)
      {
      song->setAutoWrite(track, val);
      }

//---------------------------------------------------------
//   controllerChanged
//---------------------------------------------------------

void MidiOutPortStrip::controllerChanged(int id)
      {
      if (id == CTRL_MASTER_VOLUME) {
            float val = float(track->ctrlVal(id).i);
      	if (!volumeTouched)
            	slider->setValue(val);
            sl->setValue(val);
            }
      }

//---------------------------------------------------------
//   oRoutePressed
//---------------------------------------------------------

void MidiOutPortStrip::oRoutePressed()
      {
      QMenu* pup = new QMenu(oR);
      pup->addSeparator()->setText(tr("MidiDevices"));
      RouteList* orl = track->outRoutes();

      std::list<PortName>* ol = midiDriver->outputPorts();
      int idx = 0;
      for (std::list<PortName>::iterator ip = ol->begin(); ip != ol->end(); ++ip, ++idx) {
            QAction* oa = pup->addAction(ip->name);

            QMap<QString, QVariant> data;
            data["was_checked"] = false;
            data["id"] = idx;
            oa->setData(data);

            Port port = ip->port;
            Route dst(port, Route::MIDIPORT);
            for (iRoute ir = orl->begin(); ir != orl->end(); ++ir) {
                  if (*ir == dst) {
                        oa->setCheckable(true);
                        oa->setChecked(true);
                        data["was_checked"] = true;
                        oa->setData(data);
                        break;
                        }
                  }
            }

      //
      // add software synthesizer to list
      //
      idx = 1000;
      SynthIList* sl = song->syntis();
      for (iSynthI i = sl->begin(); i != sl->end(); ++i, ++idx) {
            //int id = pup->insertItem((*i)->name(), idx);
            QAction* oa = pup->addAction((*i)->name());

            QMap<QString, QVariant> data;
            data["was_checked"] = false;
            data["id"] = idx;
            oa->setData(data);

            Route dst(*i, Route::SYNTIPORT);
            for (iRoute ir = orl->begin(); ir != orl->end(); ++ir) {
                  if (*ir == dst) {
                        oa->setCheckable(true);
                        oa->setChecked(true);
                        data["was_checked"] = true;
                        oa->setData(data);
                        break;
                        }
                  }
            }

      QAction* action = pup->exec(QCursor::pos());
      if (action) {
            QString s(action->text()); //(pup->text(n));
            int n = action->data().toMap()["id"].toInt();
            int was_checked = action->data().toMap()["was_checked"].toInt();

            Route srcRoute(track, -1, Route::TRACK);
            if (n < 1000) {
                  int idx = 0;
                  std::list<PortName>::iterator ip;
                  for (ip = ol->begin(); ip != ol->end(); ++ip, ++idx) {
                        if (idx == n)
                              break;
                        }
                  Port port = ip->port;
                  Route dstRoute(port, Route::MIDIPORT);
                  if (was_checked)
                        audio->msgRemoveRoute(srcRoute, dstRoute);
                  else
                        audio->msgAddRoute(srcRoute, dstRoute);
                  }
            else {
                  for (iSynthI i = sl->begin(); i != sl->end(); ++i) {
                        if ((*i)->name() == s) {
                              Route dstRoute(*i, Route::SYNTIPORT);
                              if (was_checked)
                                    audio->msgRemoveRoute(srcRoute, dstRoute);
                              else
                                    audio->msgAddRoute(srcRoute, dstRoute);
                              break;
                              }
                        }
                  }
            song->update(SC_ROUTE);
            }
      delete pup;
      delete ol;
      oR->setDown(false);     // pup->exec() catches mouse release event
      }

//---------------------------------------------------------
//   syncToggled
//---------------------------------------------------------

void MidiOutPortStrip::syncToggled(bool val) const
      {
      ((MidiOutPort*)track)->setSendSync(val);
      }

//---------------------------------------------------------
//   MidiInPortStrip
//---------------------------------------------------------

MidiInPortStrip::MidiInPortStrip(Mixer* m, MidiInPort* t, bool align)
   : Strip(m, t, align)
      {
      //---------------------------------------------------
      //    plugin rack
      //---------------------------------------------------

      MidiRack* rack = new MidiRack(this, t);
      rack->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
      rack->setFixedSize(STRIP_WIDTH, rack->sizeHint().height()+2);
      layout->addWidget(rack);

      if (_align)
      //      layout->addSpacing(STRIP_WIDTH/2);
            layout->addSpacing(LABEL_HEIGHT);

      int auxsSize = song->auxs()->size();
      if (_align && auxsSize)
            layout->addSpacing((STRIP_WIDTH/2 + 1) * auxsSize);

      //---------------------------------------------------
      //    slider, label, meter
      //---------------------------------------------------

      meter = new Awl::MidiMeter(this);
      meter->setFixedWidth(40);
      layout->addWidget(meter, 100, Qt::AlignRight);

      //---------------------------------------------------
      //    pan, balance
      //---------------------------------------------------

      if (_align) {
            layout->addSpacing(STRIP_WIDTH/2 + STRIP_WIDTH/3);
            layout->addSpacing(LABEL_HEIGHT);
            }

      //---------------------------------------------------
      //    mute, solo
      //    or
      //    record, mixdownfile
      //---------------------------------------------------

      mute  = newMuteButton(this);
      mute->setChecked(track->isMute());
      mute->setFixedSize(buttonSize);
      connect(mute, SIGNAL(clicked(bool)), SLOT(muteToggled(bool)));

      solo  = newSoloButton(this);
      solo->setFixedSize(buttonSize);
      solo->setChecked(track->solo());
      connect(solo, SIGNAL(clicked(bool)), SLOT(soloToggled(bool)));

      QHBoxLayout* smBox1 = new QHBoxLayout(0);
      QHBoxLayout* smBox2 = new QHBoxLayout(0);

      smBox2->addWidget(mute);
      smBox2->addWidget(solo);

      layout->addLayout(smBox1);
      layout->addLayout(smBox2);

      //---------------------------------------------------
      //    output routing
      //---------------------------------------------------

      if (_align)
            layout->addSpacing(STRIP_WIDTH/3);        // automation line

      QHBoxLayout* rBox = new QHBoxLayout(0);
      iR = new QToolButton(this);
      iR->setFont(*config.fonts[1]);
      iR->setFixedWidth((STRIP_WIDTH-4)/2);
      iR->setText(tr("iR"));
      iR->setCheckable(false);
      iR->setToolTip(tr("input routing"));
      rBox->addWidget(iR);
      connect(iR, SIGNAL(pressed()), SLOT(iRoutePressed()));

      oR = new QToolButton(this);
      oR->setFont(*config.fonts[1]);
      oR->setFixedWidth((STRIP_WIDTH-4)/2);
      oR->setText(tr("oR"));
      oR->setCheckable(false);
      oR->setToolTip(tr("output routing"));
      rBox->addWidget(oR);
      connect(oR, SIGNAL(pressed()), SLOT(oRoutePressed()));

      layout->addLayout(rBox);

      connect(heartBeatTimer, SIGNAL(timeout()), SLOT(heartBeat()));
      connect(song,  SIGNAL(songChanged(int)), SLOT(songChanged(int)));
      connect(track, SIGNAL(muteChanged(bool)), mute, SLOT(setChecked(bool)));
      connect(track, SIGNAL(soloChanged(bool)), solo, SLOT(setChecked(bool)));
      }

//---------------------------------------------------------
//   songChanged
//---------------------------------------------------------

void MidiInPortStrip::songChanged(int val)
      {
      if (val & SC_TRACK_MODIFIED)
            updateLabel();
      }

//---------------------------------------------------------
//   heartBeat
//---------------------------------------------------------

void MidiInPortStrip::heartBeat()
      {
      float a = track->meter(0); // fast_log10(track->meter(0)) * .2f;
      meter->setMeterVal(a * 0.008);
      track->setMeter(0, a * 0.8);  // hack
      }

//---------------------------------------------------------
//   muteToggled
//---------------------------------------------------------

void MidiInPortStrip::muteToggled(bool val)
      {
      song->setMute(track, val);
      }

//---------------------------------------------------------
//   soloToggled
//---------------------------------------------------------

void MidiInPortStrip::soloToggled(bool val)
      {
      song->setSolo(track, val);
      }

//---------------------------------------------------------
//   oRoutePressed
//---------------------------------------------------------

void MidiInPortStrip::oRoutePressed()
      {
      QMenu* pup = new QMenu(tr("Channel"), oR);
      pup->addSeparator()->setText(tr("Channel"));

      MidiTrackList* tl = song->midis();
      MidiSyntiList* sl = song->midiSyntis();
      RouteList* orl    = track->outRoutes();
      SynthIList* asl   = song->syntis();

      int sn = 0;
      for (iSynthI i = asl->begin(); i != asl->end(); ++i) {
            QAction* action = pup->addAction((*i)->name());

            QMap<QString, QVariant> data;
            data["id"] = QVariant(0x20000 + sn);
            data["was_checked"] = false;
            action->setData(data);

            Route dst(*i, -1, Route::SYNTIPORT);
            for (iRoute ir = orl->begin(); ir != orl->end(); ++ir) {
                  if (*ir == dst) {
                        action->setCheckable(true);
                        action->setChecked(true);
                        data["was_checked"] = true;
                        action->setData(data);
                        break;
                        }
                  }
            }
      for (int channel = 0; channel < MIDI_CHANNELS; ++channel) {
            QMenu* m = pup->addMenu(QString(tr("Channel %1")).arg(channel + 1));
            m->addSeparator()->setText(tr("Tracks"));

            //
            //  route to midi track for recording
            //  and monitoring
            //
            int tn = 0;
            for (iMidiTrack i = tl->begin(); i != tl->end(); ++i, ++tn) {
                  QAction* trackno_action = m->addAction((*i)->name());

                  QMap<QString, QVariant> data;
                  data["id"] = tn * 16 + channel;
                  data["was_checked"] = false;
                  trackno_action->setData(data);

                  Route dst(*i, channel, Route::TRACK);
                  for (iRoute ir = orl->begin(); ir != orl->end(); ++ir) {
                        if (*ir == dst) {
                              trackno_action->setCheckable(true);
                              trackno_action->setChecked(true);
                              data["was_checked"] = true;
                              trackno_action->setData(data);
                              break;
                              }
                        }
                  }
            //
            // route to midi synthesizer (arpeggiator etc.)
            //
            tn = 0;
            for (iMidiSynti i = sl->begin(); i != sl->end(); ++i, ++tn) {
                  QAction* trackno_action = m->addAction((*i)->name());

                  QMap<QString, QVariant> data;
                  data["id"] = 0x10000 + tn * 16 + channel;
                  data["was_checked"] = false;
                  trackno_action->setData(data);

                  Route dst(*i, channel, Route::TRACK);
                  for (iRoute ir = orl->begin(); ir != orl->end(); ++ir) {
                        if (*ir == dst) {
                              trackno_action->setCheckable(true);
                              trackno_action->setChecked(true);
                              data["was_checked"] = true;
                              trackno_action->setData(data);
                              break;
                              }
                        }
                  }
            }

      QAction* action = pup->exec(QCursor::pos());
      if (action) {
            int n = action->data().toMap()["id"].toInt();
            int was_checked = action->data().toMap()["was_checked"].toInt();
            Route srcRoute(track, -1, Route::TRACK);
            Route dstRoute;
            if (n >= 0x20000) {
                  dstRoute.track   = asl->index(n - 0x20000);
                  dstRoute.channel = -1;
                  dstRoute.type    = Route::SYNTIPORT;
                  }
            else if (n >= 0x10000) {
                  int channel      = (n-0x10000) & 0xf;
                  srcRoute.channel = channel;
                  dstRoute.track   = sl->index((n - 0x10000) >> 4);
                  dstRoute.channel = channel;
                  dstRoute.type    = Route::TRACK;
                  }
            else {
                  int channel      = n & 0xf;
                  srcRoute.channel = channel;
                  dstRoute.track   = tl->index(n >> 4);
                  dstRoute.channel = channel;
                  dstRoute.type    = Route::TRACK;
                  }
            if (was_checked)
                  audio->msgRemoveRoute(srcRoute, dstRoute);
            else
                  audio->msgAddRoute(srcRoute, dstRoute);
            song->update(SC_ROUTE);
            }
      delete pup;
      oR->setDown(false);     // pup->exec() catches mouse release event
      }

//---------------------------------------------------------
//   iRoutePressed
//---------------------------------------------------------

void MidiInPortStrip::iRoutePressed()
      {
      QMenu* pup = new QMenu(oR);
      pup->addSeparator()->setText(tr("AlsaDevices"));

      RouteList* irl = track->inRoutes();

      std::list<PortName>* ol = midiDriver->inputPorts();
      for (std::list<PortName>::iterator ip = ol->begin(); ip != ol->end(); ++ip) {
            QAction* action = pup->addAction(ip->name);
            Port port = ip->port;
            Route dst(port, Route::MIDIPORT);
            for (iRoute ir = irl->begin(); ir != irl->end(); ++ir) {
                  if (*ir == dst) {
                        action->setCheckable(true);
                        action->setChecked(true);
                        break;
                        }
                  }
            }
      delete ol;
      QAction* action = pup->exec(QCursor::pos());
      if (action) {
            QString s(action->text());
            Port port = midiDriver->findPort(s.toAscii().data());
            Route srcRoute(port, Route::MIDIPORT);
            Route dstRoute(track, -1, Route::TRACK);

            // check if route src->dst exists:
            iRoute ir = irl->begin();
            for (; ir != irl->end(); ++ir) {
                  if (*ir == srcRoute)
                        break;
                  }
            if (ir != irl->end()) {
                  // disconnect if route exists
                  audio->msgRemoveRoute(srcRoute, dstRoute);
                  }
            else {
                  // connect if route does not exist
                  audio->msgAddRoute(srcRoute, dstRoute);
                  }
            song->update(SC_ROUTE);
            }
      delete pup;
      iR->setDown(false);     // pup->exec() catches mouse release event
      }

//---------------------------------------------------------
//   MidiSyntiStrip
//---------------------------------------------------------

MidiSyntiStrip::MidiSyntiStrip(Mixer* m, MidiSynti* t, bool align)
   : Strip(m, t, align)
      {
      int auxsSize = song->auxs()->size();
      if (_align)
            layout->addSpacing((STRIP_WIDTH/2 + 1) * auxsSize + STRIP_WIDTH/2 * 3);

      volumeTouched = false;

      //---------------------------------------------------
      //    slider, label, meter
      //---------------------------------------------------

      slider = new Awl::MidiMeterSlider(this);
      slider->setId(CTRL_MASTER_VOLUME);
      slider->setRange(0.0, 1024*16.0);
      slider->setFixedWidth(40);
      layout->addWidget(slider, 100, Qt::AlignRight);

      sl = new Awl::MidiVolEntry(this);
      sl->setId(CTRL_MASTER_VOLUME);
      sl->setFont(*config.fonts[1]);
      sl->setFixedWidth(STRIP_WIDTH-2);

      connect(slider, SIGNAL(valueChanged(float,int)), SLOT(ctrlChanged(float, int)));
      connect(slider, SIGNAL(sliderPressed(int)), SLOT(sliderPressed(int)));
      connect(slider, SIGNAL(sliderReleased(int)), SLOT(sliderReleased(int)));
      connect(sl,     SIGNAL(valueChanged(float,int)), SLOT(ctrlChanged(float, int)));
      layout->addWidget(sl);

      //---------------------------------------------------
      //    pan, balance
      //---------------------------------------------------

      if (_align)
            layout->addSpacing(STRIP_WIDTH);

      //---------------------------------------------------
      //    sync
      //    mute, solo
      //---------------------------------------------------

      mute  = newMuteButton(this);
      mute->setChecked(track->isMute());
      mute->setFixedSize(buttonSize);
      connect(mute, SIGNAL(clicked(bool)), SLOT(muteToggled(bool)));

      solo  = newSoloButton(this);
      solo->setFixedSize(buttonSize);
      solo->setChecked(track->solo());
      connect(solo, SIGNAL(clicked(bool)), SLOT(soloToggled(bool)));

      QHBoxLayout* smBox2 = new QHBoxLayout(0);

      smBox2->addWidget(mute);
      smBox2->addWidget(solo);

      layout->addLayout(smBox2);

      //---------------------------------------------------
      //    automation mode
      //---------------------------------------------------

      addAutomationButtons();

      //---------------------------------------------------
      //    output routing
      //---------------------------------------------------

      QHBoxLayout* rBox = new QHBoxLayout(0);

      iR = new QToolButton(this);
      iR->setFont(*config.fonts[1]);
      iR->setFixedWidth((STRIP_WIDTH-4)/2);
      iR->setText(tr("iR"));
      iR->setCheckable(false);
      iR->setToolTip(tr("input routing"));
      rBox->addWidget(iR);
      connect(iR, SIGNAL(pressed()), SLOT(iRoutePressed()));

      oR = new QToolButton(this);
      oR->setFont(*config.fonts[1]);
      oR->setFixedWidth((STRIP_WIDTH-4)/2);
      oR->setText(tr("oR"));
      oR->setCheckable(false);
      oR->setToolTip(tr("output routing"));
      rBox->addWidget(oR);
      connect(oR, SIGNAL(pressed()), SLOT(oRoutePressed()));

      layout->addLayout(rBox);

      connect(heartBeatTimer, SIGNAL(timeout()), SLOT(heartBeat()));
      connect(song,  SIGNAL(songChanged(int)), SLOT(songChanged(int)));
      connect(track, SIGNAL(muteChanged(bool)), mute, SLOT(setChecked(bool)));
      connect(track, SIGNAL(soloChanged(bool)), solo, SLOT(setChecked(bool)));
      connect(track, SIGNAL(autoReadChanged(bool)), SLOT(autoChanged()));
      connect(track, SIGNAL(autoWriteChanged(bool)), SLOT(autoChanged()));
      autoChanged();
      }

//---------------------------------------------------------
//   songChanged
//---------------------------------------------------------

void MidiSyntiStrip::songChanged(int val)
      {
      if (val & SC_TRACK_MODIFIED)
            updateLabel();
      }

//---------------------------------------------------------
//   heartBeat
//---------------------------------------------------------

void MidiSyntiStrip::heartBeat()
      {
      float a = track->meter(0); // fast_log10(track->meter(0)) * .2f;
      slider->setMeterVal(a * 0.008);
      track->setMeter(0, a * 0.8);  // hack
      }

//---------------------------------------------------------
//   ctrlChanged
//---------------------------------------------------------

void MidiSyntiStrip::ctrlChanged(float val, int num)
      {
      int ival = int(val);
      CVal cval;
      cval.i = ival;
      song->setControllerVal(track, num, cval);
      }

//---------------------------------------------------------
//   sliderPressed
//---------------------------------------------------------

void MidiSyntiStrip::sliderPressed(int id)
      {
      switch (id) {
            case CTRL_MASTER_VOLUME:  volumeTouched = true;     break;
            }
      track->startAutoRecord(id);
      }

//---------------------------------------------------------
//   sliderReleased
//---------------------------------------------------------

void MidiSyntiStrip::sliderReleased(int id)
      {
      switch (id) {
            case CTRL_MASTER_VOLUME: volumeTouched = false;     break;
            }
      track->stopAutoRecord(id);
      }

//---------------------------------------------------------
//   muteToggled
//---------------------------------------------------------

void MidiSyntiStrip::muteToggled(bool val)
      {
      song->setMute(track, val);
      }

//---------------------------------------------------------
//   soloToggled
//---------------------------------------------------------

void MidiSyntiStrip::soloToggled(bool val)
      {
      song->setSolo(track, val);
      }

//---------------------------------------------------------
//   autoChanged
//---------------------------------------------------------

void MidiSyntiStrip::autoChanged()
      {
      bool ar = track->autoRead();
      bool aw = track->autoWrite();

      //  controller are enabled if
      //    autoRead is off
      //    autoRead and autoWrite are on (touch mode)

      bool ec = !ar || (ar && aw);
      slider->setEnabled(ec);
      sl->setEnabled(ec);
      }

//---------------------------------------------------------
//   autoReadToggled
//---------------------------------------------------------

void MidiSyntiStrip::autoReadToggled(bool val)
      {
      song->setAutoRead(track, val);
      }

//---------------------------------------------------------
//   autoWriteToggled
//---------------------------------------------------------

void MidiSyntiStrip::autoWriteToggled(bool val)
      {
      song->setAutoWrite(track, val);
      }

//---------------------------------------------------------
//   controllerChanged
//---------------------------------------------------------

void MidiSyntiStrip::controllerChanged(int id)
      {
      if (id == CTRL_MASTER_VOLUME) {
            float val = float(track->ctrlVal(id).i);
      	if (!volumeTouched)
            	slider->setValue(val);
            sl->setValue(val);
            }
      }

//---------------------------------------------------------
//   oRoutePressed
//---------------------------------------------------------

void MidiSyntiStrip::oRoutePressed()
      {
      QMenu* pup = new QMenu(oR);
      pup->addSeparator()->setText(tr("OutputPorts"));
      RouteList* orl = track->outRoutes();

      MidiOutPortList* mpl = song->midiOutPorts();
      int pn = 0;
      for (iMidiOutPort i = mpl->begin(); i != mpl->end(); ++i, ++pn) {
            MidiOutPort* op = *i;
            QMenu* m = pup->addMenu(op->name());
            m->addSeparator()->setText(tr("Channel"));
            for (int channel = 0; channel < MIDI_CHANNELS; ++channel) {
                  QString s;
                  s.setNum(channel+1);
                  QAction* action = m->addAction(s);//int id = m->insertItem(s, pn * 16 + channel);
                  action->setData(pn * 16 + channel);
                  MidiChannel* mc = op->channel(channel);
                  Route r(mc, -1, Route::TRACK);
                  for (iRoute ir = orl->begin(); ir != orl->end(); ++ir) {
                        if (r == *ir) {
                              action->setCheckable(true);//m->setItemChecked(id, true);
                              action->setChecked(true);
                              break;
                              }
                        }
                  }
            }
      QAction* action = pup->exec(QCursor::pos()); //int n = pup->exec(QCursor::pos());
      if (action) {
            int n = action->data().toInt();
            int portno  = n >> 4;
            int channel = n & 0xf;
            MidiOutPort* mp = mpl->index(portno);
            MidiChannel* mc = mp->channel(channel);
            Route srcRoute(track, -1, Route::TRACK);
            Route dstRoute(mc, -1, Route::TRACK);

            // remove old route
            // note: audio->msgRemoveRoute() changes orl list
            //
            bool removed;
            do {
                  removed = false;
                  for (iRoute ir = orl->begin(); ir != orl->end(); ++ir) {
                        Route s(track, ir->channel, Route::TRACK);
                        s.dump();
                        ir->dump();
                        audio->msgRemoveRoute(s, *ir);
                        removed = true;
                        break;
                        }
                  } while (removed);
            audio->msgAddRoute(srcRoute, dstRoute);
            song->update(SC_ROUTE);
            if (mixer)
                  mixer->setUpdateMixer();
            }
      delete pup;
      oR->setDown(false);     // pup->exec() catches mouse release event
      }

//---------------------------------------------------------
//   iRoutePressed
//---------------------------------------------------------

void MidiSyntiStrip::iRoutePressed()
      {
      QMenu* pup = new QMenu(oR);
      pup->addSeparator()->setText(tr("Input Ports"));
      MidiOutPort* t = (MidiOutPort*)track;
      RouteList* irl = t->inRoutes();

      MidiInPortList* ipl = song->midiInPorts();
      int tn = 0;
      for (iMidiInPort i = ipl->begin(); i != ipl->end(); ++i, ++tn) {
            QMenu* m = pup->addMenu((*i)->name());
            m->addSeparator()->setText(tr("Channel"));
            QAction* action = m->addAction(tr("All"));
            QMap<QString, QVariant> data;
            data["id"] = tn * 32 + MIDI_CHANNELS;
            data["was_checked"] = false;
            action->setData(data);

            for (int channel = 0; channel < MIDI_CHANNELS; ++channel) {
                  QString s;
                  s.setNum(channel+1);

                  QMap<QString, QVariant> data;
                  action = m->addAction(s); //int id = m->insertItem(s, tn * 32 + channel);
                  data["id"] = tn * 32 + channel;
                  data["was_checked"] = false;
                  action->setData(data);

                  Route src(*i, channel, Route::TRACK);
                  for (iRoute ir = irl->begin(); ir != irl->end(); ++ir) {
                        if (*ir == src) {
                              //m->setItemChecked(id, true);
                              action->setCheckable(true);
                              action->setChecked(true);
                              data["was_checked"] = true;
                              action->setData(data);
                              break;
                              }
                        }
                  }
            }

      QAction* action = pup->exec(QCursor::pos());//int n = pup->exec(QCursor::pos());
      if (action) {
            int n = action->data().toMap()["id"].toInt();
            int was_checked = action->data().toMap()["was_checked"].toInt();
            int port    = n >> 5;
            int channel = n & 0x1f;
            MidiInPort* mip = ipl->index(port);
            if (channel == MIDI_CHANNELS) {
                  for (channel = 0; channel < MIDI_CHANNELS; ++channel) {
                        Route srcRoute(mip, channel, Route::TRACK);
                        Route dstRoute(track, channel, Route::TRACK);
                        audio->msgAddRoute(srcRoute, dstRoute);
                        }
                  }
            else {
                  Route srcRoute(mip, channel, Route::TRACK);
                  Route dstRoute(track, channel, Route::TRACK);

                  if (was_checked)
                        audio->msgRemoveRoute(srcRoute, dstRoute);
                  else
                        audio->msgAddRoute(srcRoute, dstRoute);
                  }
            song->update(SC_ROUTE);
            }
      delete pup;
      iR->setDown(false);     // pup->exec() catches mouse release event
      }

