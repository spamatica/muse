//=========================================================
//  MusE
//  Linux Music Editor
//    $Id: pianoroll.cpp,v 1.81 2006/02/10 16:40:59 wschweer Exp $
//  (C) Copyright 1999-2006 Werner Schweer (ws@seh.de)
//=========================================================

#include "pianoroll.h"
#include "song.h"
#include "icons.h"
#include "cmd.h"
#include "muse.h"
#include "widgets/tools.h"
#include "quantconfig.h"
#include "shortcuts.h"
#include "audio.h"
#include "part.h"

static int pianorollTools        = PointerTool | PencilTool | RubberTool | DrawTool;
int PianoRoll::initWidth         = PianoRoll::INIT_WIDTH;
int PianoRoll::initHeight        = PianoRoll::INIT_HEIGHT;
int PianoRoll::initRaster        = PianoRoll::INIT_RASTER;
int PianoRoll::initQuant         = PianoRoll::INIT_QUANT;
int PianoRoll::initColorMode     = PianoRoll::INIT_COLOR_MODE;
bool PianoRoll::initFollow       = PianoRoll::INIT_FOLLOW;
bool PianoRoll::initSpeaker      = PianoRoll::INIT_SPEAKER;
bool PianoRoll::initMidiin       = PianoRoll::INIT_MIDIIN;
double PianoRoll::initXmag       = 0.08;	// PianoRoll::INIT_XMAG; (compiler problem?)
int PianoRoll::initApplyTo       = PianoRoll::INIT_APPLY_TO;
int PianoRoll::initQuantStrength = PianoRoll::INIT_QUANT_STRENGTH;
int PianoRoll::initQuantLimit    = PianoRoll::INIT_QUANT_LIMIT;
bool PianoRoll::initQuantLen     = PianoRoll::INIT_QUANT_LEN;

//---------------------------------------------------------
//   PianoRoll
//---------------------------------------------------------

PianoRoll::PianoRoll(PartList* pl, bool init)
   : MidiEditor(pl)
      {
      _applyTo       = initApplyTo;
      _colorMode     = initColorMode;
	_quantStrength = initQuantStrength;
	_quantLimit    = initQuantLimit;
	_quantLen      = initQuantLen;

      deltaMode   = false;
      selPart     = 0;
      quantConfig = 0;

      tcanvas = new PianoCanvas(this);
      QMenuBar* mb = menuBar();

      //---------Men�----------------------------------
      menuEdit->addSeparator();

      QAction* a;
      a = menuEdit->addAction(tr("Delete Events"));
      a->setIcon(*deleteIcon);
      a->setData(PianoCanvas::CMD_DEL);
      a->setShortcut(Qt::Key_Delete);

      menuEdit->addSeparator();

      menuSelect = menuEdit->addMenu(QIcon(*selectIcon),tr("&Select"));

      cmdActions[PianoCanvas::CMD_SELECT_ALL] = menuSelect->addAction(QIcon(*select_allIcon), tr("Select &All"));
      cmdActions[PianoCanvas::CMD_SELECT_ALL]->setData(PianoCanvas::CMD_SELECT_ALL);

      cmdActions[PianoCanvas::CMD_SELECT_NONE] = menuSelect->addAction(QIcon(*select_deselect_allIcon), tr("&Deselect All"));
      cmdActions[PianoCanvas::CMD_SELECT_NONE]->setData(PianoCanvas::CMD_SELECT_NONE);

      cmdActions[PianoCanvas::CMD_SELECT_INVERT] = menuSelect->addAction(QIcon(*select_invert_selectionIcon), tr("Invert &Selection"));
      cmdActions[PianoCanvas::CMD_SELECT_INVERT]->setData(PianoCanvas::CMD_SELECT_INVERT);

      cmdActions[PianoCanvas::CMD_SELECT_ILOOP] = menuSelect->addAction(QIcon(*select_inside_loopIcon), tr("&Inside Loop"));
      cmdActions[PianoCanvas::CMD_SELECT_ILOOP]->setData(PianoCanvas::CMD_SELECT_ILOOP);

      cmdActions[PianoCanvas::CMD_SELECT_OLOOP] = menuSelect->addAction(QIcon(*select_outside_loopIcon), tr("&Outside Loop"));
      cmdActions[PianoCanvas::CMD_SELECT_OLOOP]->setData(PianoCanvas::CMD_SELECT_OLOOP);

      menuConfig = mb->addMenu(tr("&Config"));
      eventColor = menuConfig->addMenu(tr("event color"));
      menu_ids[CMD_EVENT_COLOR] = eventColor->menuAction();

      colorModeAction[0] = eventColor->addAction(tr("blue"));
      colorModeAction[0]->setData(0);
      colorModeAction[0]->setCheckable(true);
      colorModeAction[1] = eventColor->addAction(tr("pitch colors"));
      colorModeAction[1]->setData(1);
      colorModeAction[1]->setCheckable(true);
      colorModeAction[2] = eventColor->addAction(tr("velocity colors"));
      colorModeAction[2]->setData(2);
      colorModeAction[2]->setCheckable(true);
      connect(eventColor, SIGNAL(triggered(QAction*)), SLOT(setEventColorMode(QAction*)));

      menuFunctions = mb->addMenu(tr("&Functions"));
      cmdActions[PianoCanvas::CMD_OVER_QUANTIZE] = menuFunctions->addAction(tr("Over Quantize"));
      cmdActions[PianoCanvas::CMD_OVER_QUANTIZE]->setData(PianoCanvas::CMD_OVER_QUANTIZE);
      cmdActions[PianoCanvas::CMD_ON_QUANTIZE] = menuFunctions->addAction(tr("Note On Quantize"));
      cmdActions[PianoCanvas::CMD_ON_QUANTIZE]->setData(PianoCanvas::CMD_ON_QUANTIZE);
      cmdActions[PianoCanvas::CMD_ONOFF_QUANTIZE] = menuFunctions->addAction(tr("Note On/Off Quantize"));
      cmdActions[PianoCanvas::CMD_ONOFF_QUANTIZE]->setData(PianoCanvas::CMD_ONOFF_QUANTIZE);
      cmdActions[PianoCanvas::CMD_ITERATIVE_QUANTIZE] = menuFunctions->addAction(tr("Iterative Quantize"));
      cmdActions[PianoCanvas::CMD_ITERATIVE_QUANTIZE]->setData(PianoCanvas::CMD_ITERATIVE_QUANTIZE);

      menuFunctions->addSeparator();

      menu_ids[CMD_CONFIG_QUANT] = menuFunctions->addAction(tr("Config Quant..."));
      connect(menu_ids[CMD_CONFIG_QUANT], SIGNAL(triggered()), this, SLOT(configQuant()));

      menuFunctions->addSeparator();

      cmdActions[PianoCanvas::CMD_MODIFY_GATE_TIME] = menuFunctions->addAction(tr("Modify Gate Time"));
      cmdActions[PianoCanvas::CMD_MODIFY_GATE_TIME]->setData(PianoCanvas::CMD_MODIFY_GATE_TIME);

      cmdActions[PianoCanvas::CMD_MODIFY_VELOCITY] = menuFunctions->addAction(tr("Modify Velocity"));
      cmdActions[PianoCanvas::CMD_MODIFY_VELOCITY]->setData(PianoCanvas::CMD_MODIFY_VELOCITY);

      cmdActions[PianoCanvas::CMD_CRESCENDO] = menuFunctions->addAction(tr("Crescendo"));
      cmdActions[PianoCanvas::CMD_CRESCENDO]->setData(PianoCanvas::CMD_CRESCENDO);
      cmdActions[PianoCanvas::CMD_CRESCENDO]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_TRANSPOSE] = menuFunctions->addAction(tr("Transpose"));
      cmdActions[PianoCanvas::CMD_TRANSPOSE]->setData(PianoCanvas::CMD_TRANSPOSE);
      cmdActions[PianoCanvas::CMD_TRANSPOSE]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_THIN_OUT] = menuFunctions->addAction(tr("Thin Out"));
      cmdActions[PianoCanvas::CMD_THIN_OUT]->setData(PianoCanvas::CMD_THIN_OUT);
      cmdActions[PianoCanvas::CMD_THIN_OUT]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_ERASE_EVENT] = menuFunctions->addAction(tr("Erase Event"));
      cmdActions[PianoCanvas::CMD_ERASE_EVENT]->setData(PianoCanvas::CMD_ERASE_EVENT);
      cmdActions[PianoCanvas::CMD_ERASE_EVENT]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_NOTE_SHIFT] = menuFunctions->addAction(tr("Note Shift"));
      cmdActions[PianoCanvas::CMD_NOTE_SHIFT]->setData(PianoCanvas::CMD_NOTE_SHIFT);
      cmdActions[PianoCanvas::CMD_NOTE_SHIFT]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_MOVE_CLOCK] = menuFunctions->addAction(tr("Move Clock"));
      cmdActions[PianoCanvas::CMD_MOVE_CLOCK]->setData(PianoCanvas::CMD_MOVE_CLOCK);
      cmdActions[PianoCanvas::CMD_MOVE_CLOCK]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_COPY_MEASURE] = menuFunctions->addAction(tr("Copy Measure"));
      cmdActions[PianoCanvas::CMD_COPY_MEASURE]->setData(PianoCanvas::CMD_COPY_MEASURE);
      cmdActions[PianoCanvas::CMD_COPY_MEASURE]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_ERASE_MEASURE] = menuFunctions->addAction(tr("Erase Measure"));
      cmdActions[PianoCanvas::CMD_ERASE_MEASURE]->setData(PianoCanvas::CMD_ERASE_MEASURE);
      cmdActions[PianoCanvas::CMD_ERASE_MEASURE]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_DELETE_MEASURE] = menuFunctions->addAction(tr("Delete Measure"));
      cmdActions[PianoCanvas::CMD_DELETE_MEASURE]->setData(PianoCanvas::CMD_DELETE_MEASURE);
      cmdActions[PianoCanvas::CMD_DELETE_MEASURE]->setEnabled(false);

      cmdActions[PianoCanvas::CMD_CREATE_MEASURE] = menuFunctions->addAction(tr("Create Measure"));
      cmdActions[PianoCanvas::CMD_CREATE_MEASURE]->setData(PianoCanvas::CMD_CREATE_MEASURE);
      cmdActions[PianoCanvas::CMD_CREATE_MEASURE]->setEnabled(false);

      connect(menuSelect,    SIGNAL(triggered(QAction*)), SLOT(cmd(QAction*)));
      connect(menuFunctions, SIGNAL(triggered(QAction*)), SLOT(cmd(QAction*)));

      //---------ToolBar----------------------------------
      tools = addToolBar(tr("Pianoroll Tools"));
      tools->addAction(undoAction);
      tools->addAction(redoAction);
      tools->addSeparator();

      tools->addAction(stepRecAction);
      stepRecAction->setChecked(INIT_SREC);

      tools->addAction(midiInAction);
      midiInAction->setChecked(INIT_MIDIIN);

      tools->addAction(speaker);
      speaker->setChecked(INIT_SPEAKER);

      tools->addAction(followSongAction);
      followSongAction->setChecked(INIT_FOLLOW);
      tcanvas->setFollow(INIT_FOLLOW);

      tools2 = new EditToolBar(this, pianorollTools);
      addToolBar(tools2);

      QToolBar* panicToolbar = addToolBar(tr("Panic"));
      panicToolbar->addAction(panicAction);

      //-------------------------------------------------------------
      //    Transport Bar

      QToolBar* transport = addToolBar(tr("Transport"));
      muse->setupTransportToolbar(transport);

      addToolBarBreak();
      toolbar = new Toolbar1(initRaster, initQuant);
      addToolBar(toolbar);

      addToolBarBreak();
      info = new NoteInfo(this);
      addToolBar(info);

      setCentralWidget(tcanvas);
      tcanvas->setCornerWidget(new QSizeGrip(tcanvas));

      connect(song,   SIGNAL(posChanged(int,const AL::Pos&,bool)), canvas(), SLOT(setLocatorPos(int,const AL::Pos&,bool)));
      connect(canvas(), SIGNAL(posChanged(int,const AL::Pos&)), SLOT(setPos(int,const AL::Pos&)));

      connect(canvas(), SIGNAL(toolChanged(int)), tools2, SLOT(set(int)));
      connect(tools2, SIGNAL(toolChanged(int)), canvas(), SLOT(setTool(int)));

      connect(info, SIGNAL(valueChanged(NoteInfo::ValType, int)), SLOT(noteinfoChanged(NoteInfo::ValType, int)));

      connect(canvas(), SIGNAL(selectionChanged(int, Event&, Part*)), this,
         SLOT(setSelection(int, Event&, Part*)));

      info->setEnabled(false);

      setWindowTitle(canvas()->getCaption());
      int s1, e;
      canvas()->range(&s1, &e);
      e += AL::sigmap.ticksMeasure(e);  // show one more measure
      canvas()->setTimeRange(s1, e);

      // connect to toolbar
      connect(canvas(),   SIGNAL(pitchChanged(int)), toolbar, SLOT(setPitch(int)));
      connect(canvas(),   SIGNAL(yChanged(int)), toolbar, SLOT(setInt(int)));
      connect(canvas(),   SIGNAL(cursorPos(const AL::Pos&,bool)),  toolbar, SLOT(setTime(const AL::Pos&,bool)));
      connect(toolbar,  SIGNAL(quantChanged(int)), SLOT(setQuant(int)));
      connect(toolbar,  SIGNAL(rasterChanged(int)),SLOT(setRaster(int)));
      connect(toolbar,  SIGNAL(toChanged(int)),    SLOT(setApplyTo(int)));
      connect(toolbar,  SIGNAL(soloChanged(bool)), SLOT(soloChanged(bool)));

      setEventColorMode(_colorMode);

      clipboardChanged(); 	// enable/disable "Paste"
      selectionChanged(); 	// enable/disable "Copy" & "Paste"
      initShortcuts(); 		// initialize shortcuts

      //
      // install misc shortcuts
      //
      QShortcut* sc;
      sc = new QShortcut(shortcuts[SHRT_POS_INC].key, this);
      sc->setContext(Qt::WindowShortcut);
      connect(sc, SIGNAL(activated()), SLOT(cmdRight()));

      sc = new QShortcut(shortcuts[SHRT_POS_DEC].key, this);
      sc->setContext(Qt::WindowShortcut);
      connect(sc, SIGNAL(activated()), SLOT(cmdLeft()));

      sc = new QShortcut(Qt::Key_Escape, this);
      sc->setContext(Qt::WindowShortcut);

      connect(sc, SIGNAL(activated()), SLOT(close()));
      connect(song, SIGNAL(songChanged(int)), canvas(), SLOT(songChanged(int)));
      connect(followSongAction, SIGNAL(toggled(bool)), canvas(), SLOT(setFollow(bool)));
      canvas()->selectFirst();

      Part* part = canvas()->part();
	setRaster(part->raster() != -1   ? part->raster() : initRaster);
      setQuant(part->quant()   != -1   ? part->quant()  : initQuant);
      setXmag(part->xmag()     != -1.0 ? part->xmag()   : initXmag);

      if (init)
            initFromPart();
      else {
	      resize(initWidth, initHeight);
printf("resize %d %d\n", initWidth, initHeight);
            }
      }

//---------------------------------------------------------
//   cmdLeft
//---------------------------------------------------------

void PianoRoll::cmdLeft()
      {
      canvas()->pianoCmd(MCMD_LEFT);
      }

//---------------------------------------------------------
//   cmdRight
//---------------------------------------------------------

void PianoRoll::cmdRight()
      {
      canvas()->pianoCmd(MCMD_RIGHT);
      }

//---------------------------------------------------------
//   configChanged
//---------------------------------------------------------

void PianoRoll::configChanged()
      {
      initShortcuts();
      }

//---------------------------------------------------------
//   cmd
//    pulldown menu commands
//---------------------------------------------------------

void PianoRoll::cmd(QAction* a)
      {
      int cmd = a->data().toInt();
      canvas()->cmd(cmd, _quantStrength, _quantLimit, _quantLen);
      }

//---------------------------------------------------------
//   setSelection
//    update Info Line
//---------------------------------------------------------

void PianoRoll::setSelection(int tick, Event& e, Part* p)
      {
      int selections = canvas()->selectionSize();

      selEvent = e;
      selPart  = p;

      if (selections > 1) {
            info->setEnabled(true);
            info->setDeltaMode(true);
            if (!deltaMode) {
                  deltaMode = true;
                  info->setValues(0, 0, 0, 0, 0);
                  tickOffset    = 0;
                  lenOffset     = 0;
                  pitchOffset   = 0;
                  veloOnOffset  = 0;
                  veloOffOffset = 0;
                  }
            }
      else if (selections == 1) {
            deltaMode = false;
            info->setEnabled(true);
            info->setDeltaMode(false);
            info->setValues(tick,
               selEvent.lenTick(),
               selEvent.pitch(),
               selEvent.velo(),
               selEvent.veloOff());
            }
      else {
            deltaMode = false;
            info->setEnabled(false);
            }
      selectionChanged();
      }

//---------------------------------------------------------
//    edit currently selected Event
//---------------------------------------------------------

void PianoRoll::noteinfoChanged(NoteInfo::ValType type, int val)
      {
      int selections = canvas()->selectionSize();

      if (selections == 0) {
            printf("noteinfoChanged while nothing selected\n");
            }
      else if (selections == 1) {
            Event event = selEvent.clone();
            switch(type) {
                  case NoteInfo::VAL_TIME:
                        event.setTick(val - selPart->tick());
                        break;
                  case NoteInfo::VAL_LEN:
                        event.setLenTick(val);
                        break;
                  case NoteInfo::VAL_VELON:
                        event.setVelo(val);
                        break;
                  case NoteInfo::VAL_VELOFF:
                        event.setVeloOff(val);
                        break;
                  case NoteInfo::VAL_PITCH:
                        event.setPitch(val);
                        break;
                  }
            audio->msgChangeEvent(selEvent, event, selPart);
            }
      else {
            // multiple events are selected; treat noteinfo values
            // as offsets to event values

            int delta = 0;
            switch (type) {
                  case NoteInfo::VAL_TIME:
                        delta = val - tickOffset;
                        tickOffset = val;
                        break;
                  case NoteInfo::VAL_LEN:
                        delta = val - lenOffset;
                        lenOffset = val;
                        break;
                  case NoteInfo::VAL_VELON:
                        delta = val - veloOnOffset;
                        veloOnOffset = val;
                        break;
                  case NoteInfo::VAL_VELOFF:
                        delta = val - veloOffOffset;
                        veloOffOffset = val;
                        break;
                  case NoteInfo::VAL_PITCH:
                        delta = val - pitchOffset;
                        pitchOffset = val;
                        break;
                  }
            if (delta)
                  canvas()->modifySelected(type, delta);
            }
      }

//---------------------------------------------------------
//   soloChanged
//    signal from solo button
//---------------------------------------------------------

void PianoRoll::soloChanged(bool flag)
      {
      song->setSolo(canvas()->track(), flag);
      }

static int rasterTable[] = {
      //-9----8-  7    6     5     4    3(1/4)     2   1
      4,  8, 16, 32,  64, 128, 256,  512, 1024,  // triple
      6, 12, 24, 48,  96, 192, 384,  768, 1536,
      9, 18, 36, 72, 144, 288, 576, 1152, 2304   // dot
      };

//---------------------------------------------------------
//   viewKeyPressEvent
//---------------------------------------------------------

void PianoRoll::keyPressEvent(QKeyEvent* event)
      {
      if (info->hasFocus()) {
            event->ignore();
            return;
            }

      int index;
      int n = sizeof(rasterTable)/sizeof(*rasterTable);
      for (index = 0; index < n; ++index)
            if (rasterTable[index] == raster())
                  break;
      if (index == n) {
            index = 0;
            // raster 1 is not in table
            }
      int off = (index / 9) * 9;
      index   = index % 9;

      int val = 0;

      PianoCanvas* pc = canvas();
      int key = event->key();

      if (event->modifiers() & Qt::ShiftModifier)
            key += Qt::SHIFT;
      if (event->modifiers() & Qt::AltModifier)
            key += Qt::ALT;
      if (event->modifiers() & Qt::ControlModifier)
            key+= Qt::CTRL;

      if (key == shortcuts[SHRT_TOOL_POINTER].key) {
            tools2->set(PointerTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_PENCIL].key) {
            tools2->set(PencilTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_RUBBER].key) {
            tools2->set(RubberTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_LINEDRAW].key) {
            tools2->set(DrawTool);
            return;
            }
      else if (key == shortcuts[SHRT_INSERT_AT_LOCATION].key) {
            pc->pianoCmd(MCMD_INSERT);
            return;
            }
      else if (key == Qt::Key_Delete) {
            pc->pianoCmd(MCMD_DELETE);
            return;
            }
      else if (key == shortcuts[SHRT_SET_QUANT_1].key)
            val = rasterTable[8 + off];
      else if (key == shortcuts[SHRT_SET_QUANT_2].key)
            val = rasterTable[7 + off];
      else if (key == shortcuts[SHRT_SET_QUANT_3].key)
            val = rasterTable[6 + off];
      else if (key == shortcuts[SHRT_SET_QUANT_4].key)
            val = rasterTable[5 + off];
      else if (key == shortcuts[SHRT_SET_QUANT_5].key)
            val = rasterTable[4 + off];
      else if (key == shortcuts[SHRT_SET_QUANT_6].key)
            val = rasterTable[3 + off];
      else if (key == shortcuts[SHRT_SET_QUANT_7].key)
            val = rasterTable[2 + off];
      else if (key == shortcuts[SHRT_TOGGLE_TRIOL].key)
            val = rasterTable[index + ((off == 0) ? 9 : 0)];
      else if (key == shortcuts[SHRT_EVENT_COLOR].key) {
            _colorMode = (_colorMode + 1) % 3;
            setEventColorMode(_colorMode);
            return;
            }
      else if (key == shortcuts[SHRT_TOGGLE_PUNCT].key)
            val = rasterTable[index + ((off == 18) ? 9 : 18)];

      else if (key == shortcuts[SHRT_TOGGLE_PUNCT2].key) {//CDW
            if ((off == 18) && (index > 2)) {
                  val = rasterTable[index + 9 - 1];
                  }
            else if ((off == 9) && (index < 8)) {
                  val = rasterTable[index + 18 + 1];
                  }
            else
                  return;
            }
      else { //Default:
            event->ignore();
            return;
            }

      setQuant(val);
      setRaster(val);
      toolbar->setQuant(quant());
      toolbar->setRaster(raster());
      }

//---------------------------------------------------------
//   configQuant
//---------------------------------------------------------

void PianoRoll::configQuant()
      {
	QuantConfig quantConfig(_quantStrength, _quantLimit, _quantLen, this);
      if (!quantConfig.exec())
            return;
      _quantStrength = quantConfig.quantStrength();
      _quantLimit    = quantConfig.quantLimit();
      _quantLen      = quantConfig.doQuantLen();
      }

//---------------------------------------------------------
//   setEventColorMode
//---------------------------------------------------------

void PianoRoll::setEventColorMode(QAction* a)
      {
      setEventColorMode(a->data().toInt());
      }

void PianoRoll::setEventColorMode(int mode)
      {
      _colorMode = mode;
      for (int i = 0; i < 3; ++i)
            colorModeAction[i]->setChecked(mode == i);
      canvas()->setColorMode(mode);
      }

//---------------------------------------------------------
//   initShortcuts
//---------------------------------------------------------

void PianoRoll::initShortcuts()
      {
      cmdActions[PianoCanvas::CMD_SELECT_NONE]->setShortcut(shortcuts[SHRT_SELECT_ALL].key);
      cmdActions[PianoCanvas::CMD_SELECT_NONE]->setShortcut(shortcuts[SHRT_SELECT_NONE].key);
      cmdActions[PianoCanvas::CMD_SELECT_INVERT]->setShortcut(shortcuts[SHRT_SELECT_INVERT].key);
      cmdActions[PianoCanvas::CMD_SELECT_ILOOP]->setShortcut(shortcuts[SHRT_SELECT_ILOOP].key);
      cmdActions[PianoCanvas::CMD_SELECT_OLOOP]->setShortcut(shortcuts[SHRT_SELECT_OLOOP].key);

      menu_ids[CMD_EVENT_COLOR]->setShortcut(shortcuts[SHRT_EVENT_COLOR].key);

      cmdActions[PianoCanvas::CMD_OVER_QUANTIZE]->setShortcut(shortcuts[SHRT_OVER_QUANTIZE].key);
      cmdActions[PianoCanvas::CMD_ON_QUANTIZE]->setShortcut(shortcuts[SHRT_ON_QUANTIZE].key);
      cmdActions[PianoCanvas::CMD_ONOFF_QUANTIZE]->setShortcut(shortcuts[SHRT_ONOFF_QUANTIZE].key);
      cmdActions[PianoCanvas::CMD_ITERATIVE_QUANTIZE]->setShortcut(shortcuts[SHRT_ITERATIVE_QUANTIZE].key);
      cmdActions[PianoCanvas::CMD_MODIFY_GATE_TIME]->setShortcut(shortcuts[SHRT_MODIFY_GATE_TIME].key);
      cmdActions[PianoCanvas::CMD_MODIFY_VELOCITY]->setShortcut(shortcuts[SHRT_MODIFY_VELOCITY].key);
      cmdActions[PianoCanvas::CMD_CRESCENDO]->setShortcut(shortcuts[SHRT_CRESCENDO].key);
      cmdActions[PianoCanvas::CMD_TRANSPOSE]->setShortcut(shortcuts[SHRT_TRANSPOSE].key);
      cmdActions[PianoCanvas::CMD_THIN_OUT]->setShortcut(shortcuts[SHRT_THIN_OUT].key);
      cmdActions[PianoCanvas::CMD_ERASE_EVENT]->setShortcut(shortcuts[SHRT_ERASE_EVENT].key);
      cmdActions[PianoCanvas::CMD_NOTE_SHIFT]->setShortcut(shortcuts[SHRT_NOTE_SHIFT].key);
      cmdActions[PianoCanvas::CMD_MOVE_CLOCK]->setShortcut(shortcuts[SHRT_MOVE_CLOCK].key);
      cmdActions[PianoCanvas::CMD_COPY_MEASURE]->setShortcut(shortcuts[SHRT_COPY_MEASURE].key);
      cmdActions[PianoCanvas::CMD_ERASE_MEASURE]->setShortcut(shortcuts[SHRT_ERASE_MEASURE].key);
      cmdActions[PianoCanvas::CMD_DELETE_MEASURE]->setShortcut(shortcuts[SHRT_DELETE_MEASURE].key);
      cmdActions[PianoCanvas::CMD_CREATE_MEASURE]->setShortcut(shortcuts[SHRT_CREATE_MEASURE].key);

      menu_ids[CMD_CONFIG_QUANT]->setShortcut(shortcuts[SHRT_CONFIG_QUANT].key);
      }

//---------------------------------------------------------
//   readConfiguration
//	read static init values
//---------------------------------------------------------

void PianoRoll::readConfiguration(QDomNode node)
      {
      for (node = node.firstChild(); !node.isNull(); node = node.nextSibling()) {
            QDomElement e = node.toElement();
            QString tag(e.tagName());
            int i = e.text().toInt();
            if (tag == "width")
                  PianoRoll::initWidth = i;
            else if (tag == "height")
                  PianoRoll::initHeight = i;
            else if (tag == "raster")
                  PianoRoll::initRaster = i;
            else if (tag == "quant")
                  PianoRoll::initQuant = i;
            else
                  printf("MusE:PianoRoll: unknown tag %s\n", tag.toLatin1().data());
            }
      }

//---------------------------------------------------------
//   writeConfiguration
//	write static init values
//---------------------------------------------------------

void PianoRoll::writeConfiguration(Xml& xml)
      {
      xml.tag("PianoRoll");
      if (PianoRoll::initWidth != PianoRoll::INIT_WIDTH)
            xml.intTag("width", PianoRoll::initWidth);
      if (PianoRoll::initHeight != PianoRoll::INIT_HEIGHT)
            xml.intTag("height", PianoRoll::initHeight);
      if (PianoRoll::initRaster != PianoRoll::INIT_RASTER)
            xml.intTag("raster", PianoRoll::initRaster);
      if (PianoRoll::initQuant != PianoRoll::INIT_QUANT)
            xml.intTag("quant", PianoRoll::initQuant);
      xml.etag("PianoRoll");
      }

