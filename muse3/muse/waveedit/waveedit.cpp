//=========================================================
//  MusE
//  Linux Music Editor
//    $Id: waveedit.cpp,v 1.5.2.12 2009/04/06 01:24:54 terminator356 Exp $
//  (C) Copyright 2000 Werner Schweer (ws@seh.de)
//  (C) Copyright 2012 Tim E. Real (terminator356 on users dot sourceforge dot net)
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

#include <limits.h>

#include <QMenu>
#include <QToolBar>
#include <QToolButton>
#include <QLayout>
#include <QSizeGrip>
#include <QScrollBar>
#include <QLabel>
#include <QSlider>
#include <QMenuBar>
#include <QAction>
#include <QCloseEvent>
#include <QResizeEvent>
#include <QKeyEvent>
#include <QSettings>
#include <QCursor>
#include <QPoint>
#include <QRect>

#include "app.h"
#include "xml.h"
#include "waveedit.h"
#include "mtscale.h"
#include "scrollscale.h"
#include "wavecanvas.h"
#include "ttoolbar.h"
#include "globals.h"
#include "audio.h"
#include "utils.h"
#include "song.h"
#include "poslabel.h"
#include "gconfig.h"
#include "icons.h"
#include "shortcuts.h"
#include "cmd.h"
#include "operations.h"
#include "trackinfo_layout.h"
#include "splitter.h"

// For debugging output: Uncomment the fprintf section.
#define ERROR_WAVEEDIT(dev, format, args...)  fprintf(dev, format, ##args)
#define DEBUG_WAVEEDIT(dev, format, args...) // fprintf(dev, format, ##args)

namespace MusECore {
extern QColor readColor(MusECore::Xml& xml);
}

namespace MusEGui {

static int waveEditTools = MusEGui::PointerTool | MusEGui::PencilTool | MusEGui::RubberTool | 
                           MusEGui::CutTool | MusEGui::RangeTool | PanTool | ZoomTool |
                           StretchTool | SamplerateTool;

int WaveEdit::_rasterInit = 96;
int WaveEdit::_trackInfoWidthInit = 50;
int WaveEdit::_canvasWidthInit = 300;
int WaveEdit::colorModeInit = 0;
  
//---------------------------------------------------------
//   closeEvent
//---------------------------------------------------------

void WaveEdit::closeEvent(QCloseEvent* e)
      {
      _isDeleting = true;  // Set flag so certain signals like songChanged, which may cause crash during delete, can be ignored.

      QSettings settings;
      //settings.setValue("Waveedit/geometry", saveGeometry());
      settings.setValue("Waveedit/windowState", saveState());
      
      //Store values of the horizontal splitter
      QList<int> sizes = hsplitter->sizes();
      QList<int>::iterator it = sizes.begin();
      _trackInfoWidthInit = *it; //There are only 2 values stored in the sizelist, size of trackinfo widget and canvas widget
      it++;
      _canvasWidthInit = *it;
    
      emit isDeleting(static_cast<TopWin*>(this));
      e->accept();
      }

//---------------------------------------------------------
//   WaveEdit
//---------------------------------------------------------

WaveEdit::WaveEdit(MusECore::PartList* pl, QWidget* parent, const char* name)
   : MidiEditor(TopWin::WAVE, 1, pl, parent, name)
      {
      setFocusPolicy(Qt::NoFocus);
      colorMode      = colorModeInit;

      QAction* act;
      
      //---------Pulldown Menu----------------------------
      // We probably don't need an empty menu - Orcan
      //QMenu* menuFile = menuBar()->addMenu(tr("&File"));
      QMenu* menuEdit = menuBar()->addMenu(tr("&Edit"));
      
      menuFunctions = menuBar()->addMenu(tr("Func&tions"));

      menuGain = menuFunctions->addMenu(tr("&Gain"));
      
      act = menuGain->addAction("200%");
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_GAIN_200); } );
      
      act = menuGain->addAction("150%");
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_GAIN_150); } );
      
      act = menuGain->addAction("75%");
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_GAIN_75); } );
      
      act = menuGain->addAction("50%");
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_GAIN_50); } );
      
      act = menuGain->addAction("25%");
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_GAIN_25); } );
      
      act = menuGain->addAction(tr("Other"));
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_GAIN_FREE); } );
      
      menuFunctions->addSeparator();

      copyAction = menuEdit->addAction(tr("&Copy"));
      connect(copyAction, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_EDIT_COPY); } );

      copyPartRegionAction = menuEdit->addAction(tr("&Create Part from Region"));
      connect(copyPartRegionAction, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_CREATE_PART_REGION); } );

      cutAction = menuEdit->addAction(tr("C&ut"));
      connect(cutAction, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_EDIT_CUT); } );

      pasteAction = menuEdit->addAction(tr("&Paste"));
      connect(pasteAction, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_EDIT_PASTE); } );


      act = menuEdit->addAction(tr("Edit in E&xternal Editor..."));
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_EDIT_EXTERNAL); } );
      
      menuEdit->addSeparator();

// REMOVE Tim. Also remove CMD_ADJUST_WAVE_OFFSET and so on...      
//       adjustWaveOffsetAction = menuEdit->addAction(tr("Adjust wave offset..."));
//       mapper->setMapping(adjustWaveOffsetAction, WaveCanvas::CMD_ADJUST_WAVE_OFFSET);
//       connect(adjustWaveOffsetAction, SIGNAL(triggered()), mapper, SLOT(map()));
      
      act = menuFunctions->addAction(tr("Mute Selection"));
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_MUTE); } );
      
      act = menuFunctions->addAction(tr("Normalize Selection"));
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_NORMALIZE); } );
      
      act = menuFunctions->addAction(tr("Fade In Selection"));
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_FADE_IN); } );
      
      act = menuFunctions->addAction(tr("Fade Out Selection"));
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_FADE_OUT); } );
      
      act = menuFunctions->addAction(tr("Reverse Selection"));
      connect(act, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_REVERSE); } );
      
      select = menuEdit->addMenu(QIcon(*selectIcon), tr("Select"));
      
      selectAllAction = select->addAction(QIcon(*select_allIcon), tr("Select &All"));
      connect(selectAllAction, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_SELECT_ALL); } );
      
      selectNoneAction = select->addAction(QIcon(*select_allIcon), tr("&Deselect All"));
      connect(selectNoneAction, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_SELECT_NONE); } );
      
      select->addSeparator();
      
      selectPrevPartAction = select->addAction(QIcon(*select_all_parts_on_trackIcon), tr("&Previous Part"));
      connect(selectPrevPartAction, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_SELECT_PREV_PART); } );
      
      selectNextPartAction = select->addAction(QIcon(*select_all_parts_on_trackIcon), tr("&Next Part"));
      connect(selectNextPartAction, &QAction::triggered, [this]() { cmd(WaveCanvas::CMD_SELECT_NEXT_PART); } );

      
      QMenu* settingsMenu = menuBar()->addMenu(tr("&Display"));

      settingsMenu->addAction(subwinAction);
//      settingsMenu->addAction(shareAction);
      settingsMenu->addAction(fullscreenAction);

      settingsMenu->addSeparator();

      eventColor = settingsMenu->addMenu(tr("&Event Color"));      
      
      QActionGroup* actgrp = new QActionGroup(this);
      actgrp->setExclusive(true);
      
      evColorNormalAction = actgrp->addAction(tr("&Part colors"));
      evColorNormalAction->setCheckable(true);
      connect(evColorNormalAction, &QAction::triggered, [this]() { eventColorModeChanged(0); } );
      
      evColorPartsAction = actgrp->addAction(tr("&Gray"));
      evColorPartsAction->setCheckable(true);
      connect(evColorPartsAction, &QAction::triggered, [this]() { eventColorModeChanged(1); } );
      
      eventColor->addActions(actgrp->actions());
      
      connect(MusEGlobal::muse, SIGNAL(configChanged()), SLOT(configChanged()));


      //--------------------------------------------------
      //    ToolBar:   Solo  Cursor1 Cursor2

      // NOTICE: Please ensure that any tool bar object names here match the names assigned 
      //          to identical or similar toolbars in class MusE or other TopWin classes. 
      //         This allows MusE::setCurrentMenuSharingTopwin() to do some magic
      //          to retain the original toolbar layout. If it finds an existing
      //          toolbar with the same object name, it /replaces/ it using insertToolBar(),
      //          instead of /appending/ with addToolBar().

      addToolBarBreak();
      
      // Already has an object name.
      tools2 = new MusEGui::EditToolBar(this, waveEditTools);
      addToolBar(tools2);
      
      tb1 = addToolBar(tr("WaveEdit tools"));
      tb1->setObjectName("Wave Pos/Snap/Solo-tools");

      solo = new QToolButton();
      solo->setText(tr("Solo"));
      solo->setCheckable(true);
      solo->setFocusPolicy(Qt::NoFocus);
      tb1->addWidget(solo);
      connect(solo,  SIGNAL(toggled(bool)), SLOT(soloChanged(bool)));
      
      QLabel* label = new QLabel(tr("Cursor"));
      tb1->addWidget(label);
      label->setAlignment(Qt::AlignRight|Qt::AlignVCenter);
      label->setIndent(3);
      pos1 = new PosLabel(nullptr);
      pos1->setObjectName("PosLabel");
      pos1->setFixedHeight(22);
      tb1->addWidget(pos1);
      pos2 = new PosLabel(nullptr);
      pos2->setObjectName("PosLabel");
      pos2->setFixedHeight(22);
      pos2->setSmpte(true);
      tb1->addWidget(pos2);

      //---------------------------------------------------
      //    Rest
      //---------------------------------------------------

      int yscale = 256;
      int xscale;

      if (!parts()->empty()) { // Roughly match total size of part
            MusECore::Part* firstPart = parts()->begin()->second;
            xscale = 0 - firstPart->lenFrame()/_widthInit[_type];
            }
      else {
            xscale = -8000;
            }

      hscroll = new ScrollScale(-32768, 1, xscale, 10000, Qt::Horizontal, mainw, 0, false, 10000.0);
      //view    = new WaveView(this, mainw, xscale, yscale);
      canvas    = new WaveCanvas(this, mainw, xscale, yscale);
      //wview   = canvas;   // HACK!

      QSizeGrip* corner    = new QSizeGrip(mainw);
      ymag                 = new QSlider(Qt::Vertical, mainw);
      ymag->setMinimum(1);
      ymag->setMaximum(256);
      ymag->setPageStep(256);
      ymag->setValue(yscale);
      ymag->setFocusPolicy(Qt::NoFocus);

      //---------------------------------------------------
      //    split
      //---------------------------------------------------

      hsplitter = new MusEGui::Splitter(Qt::Horizontal, mainw, "hsplitter");
      hsplitter->setChildrenCollapsible(true);
      //hsplitter->setHandleWidth(4);
      
      trackInfoWidget = new TrackInfoWidget(hsplitter);
      genTrackInfo(trackInfoWidget);

      time                 = new MTScale(&_raster, mainw, xscale, true);
      
      //QWidget* split1     = new QWidget(splitter);
      QWidget* split1     = new QWidget();
      split1->setObjectName("split1");
      QGridLayout* gridS1 = new QGridLayout(split1);
      gridS1->setContentsMargins(0, 0, 0, 0);
      gridS1->setSpacing(0);  

      gridS1->setRowStretch(2, 100);
      gridS1->setColumnStretch(1, 100);     

//       gridS1->addWidget(time,                   0, 1, 1, 2);
//       gridS1->addWidget(MusECore::hLine(split1),          1, 0, 1, 3);
//       //gridS1->addWidget(piano,                  2,    0);
//       gridS1->addWidget(canvas,                 2,    1);
//       gridS1->addWidget(vscroll,                2,    2);

      gridS1->addWidget(time,   0, 0, 1, 2);
      gridS1->addWidget(MusECore::hLine(mainw),    1, 0, 1, 2);
      gridS1->addWidget(canvas,    2, 0);
      gridS1->addWidget(ymag,    2, 1);

      
      QWidget* gridS2_w = new QWidget();
      gridS2_w->setObjectName("gridS2_w");
      gridS2_w->setContentsMargins(0, 0, 0, 0);
      QGridLayout* gridS2 = new QGridLayout(gridS2_w);
      gridS2->setContentsMargins(0, 0, 0, 0);
      gridS2->setSpacing(0);  
      gridS2->setRowStretch(0, 100);
      //gridS2->setColumnStretch(1, 100);
      //gridS2->addWidget(ctrl,    0, 0);
      gridS2->addWidget(hscroll, 0, 0);
      gridS2->addWidget(corner,  0, 1, Qt::AlignBottom|Qt::AlignRight);
      gridS2_w->setMaximumHeight(hscroll->sizeHint().height());
      gridS2_w->setMinimumHeight(hscroll->sizeHint().height());
      
      QWidget* splitter_w = new QWidget();
      splitter_w->setObjectName("splitter_w");
      splitter_w->setContentsMargins(0, 0, 0, 0);
      QVBoxLayout* splitter_vbox = new QVBoxLayout(splitter_w);
      splitter_vbox->setContentsMargins(0, 0, 0, 0);
      splitter_vbox->setSpacing(0);  
      //splitter_vbox->addWidget(splitter);
      splitter_vbox->addWidget(split1);
      splitter_vbox->addWidget(gridS2_w);
      
      hsplitter->addWidget(splitter_w);
          
      hsplitter->setStretchFactor(hsplitter->indexOf(trackInfoWidget), 0);
      QSizePolicy tipolicy = QSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);
      tipolicy.setHorizontalStretch(0);
      tipolicy.setVerticalStretch(100);
      trackInfoWidget->setSizePolicy(tipolicy);

      hsplitter->setStretchFactor(hsplitter->indexOf(splitter_w), 1);
      QSizePolicy epolicy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
      epolicy.setHorizontalStretch(255);
      epolicy.setVerticalStretch(100);
      //splitter->setSizePolicy(epolicy);
      


      
      ymag->setFixedWidth(16);
      connect(canvas, SIGNAL(mouseWheelMoved(int)), this, SLOT(moveVerticalSlider(int)));
      connect(ymag, SIGNAL(valueChanged(int)), canvas, SLOT(setYScale(int)));
      connect(canvas, SIGNAL(horizontalZoom(bool, const QPoint&)), SLOT(horizontalZoom(bool, const QPoint&)));
      connect(canvas, SIGNAL(horizontalZoom(int, const QPoint&)), SLOT(horizontalZoom(int, const QPoint&)));

      time->setOrigin(0, 0);

      
      
//       QWidget* split1     = new QWidget(splitter_w);
//       split1->setObjectName("split1");
//       QGridLayout* gridS1 = new QGridLayout(split1);
//       gridS1->setContentsMargins(0, 0, 0, 0);
//       gridS1->setSpacing(0);  
// 
//       gridS1->setRowStretch(2, 100);
//       gridS1->setColumnStretch(1, 100);     
// 
// //       gridS1->addWidget(time,                   0, 1, 1, 2);
// //       gridS1->addWidget(MusECore::hLine(split1),          1, 0, 1, 3);
// //       //gridS1->addWidget(piano,                  2,    0);
// //       gridS1->addWidget(canvas,                 2,    1);
// //       gridS1->addWidget(vscroll,                2,    2);
// 
//       gridS1->addWidget(time,   0, 0, 1, 2);
//       gridS1->addWidget(MusECore::hLine(mainw),    1, 0, 1, 2);
//       gridS1->addWidget(canvas,    2, 0);
//       gridS1->addWidget(ymag,    2, 1);

      
//       mainGrid->setRowStretch(0, 100);
//       mainGrid->setColumnStretch(0, 100);

//       mainGrid->addWidget(time,   0, 0, 1, 2);
//       mainGrid->addWidget(MusECore::hLine(mainw),    1, 0, 1, 2);
//       mainGrid->addWidget(canvas,    2, 0);
//       mainGrid->addWidget(ymag,    2, 1);
//       mainGrid->addWidget(hscroll, 3, 0);
//       mainGrid->addWidget(corner,  3, 1, Qt::AlignBottom | Qt::AlignRight);

      mainGrid->addWidget(hsplitter, 0, 0, 1, 1);

      canvas->setFocus();  
      
      connect(canvas, SIGNAL(toolChanged(int)), tools2, SLOT(set(int)));
      connect(tools2, SIGNAL(toolChanged(int)), canvas,   SLOT(setTool(int)));
      
      connect(hscroll, SIGNAL(scrollChanged(int)), canvas, SLOT(setXPos(int)));
      connect(hscroll, SIGNAL(scaleChanged(int)),  canvas, SLOT(setXMag(int)));
      setWindowTitle(canvas->getCaption());
      connect(canvas, SIGNAL(followEvent(int)), hscroll, SLOT(setOffset(int)));

      connect(hscroll, SIGNAL(scrollChanged(int)), time,  SLOT(setXPos(int)));
      connect(hscroll, SIGNAL(scaleChanged(int)),  time,  SLOT(setXMag(int)));
      connect(time,    SIGNAL(timeChanged(unsigned)),  SLOT(timeChanged(unsigned)));
      connect(canvas,    SIGNAL(timeChanged(unsigned)),  SLOT(setTime(unsigned)));

      connect(canvas,  SIGNAL(horizontalScroll(unsigned)),hscroll, SLOT(setPos(unsigned)));
      connect(canvas,  SIGNAL(horizontalScrollNoLimit(unsigned)),hscroll, SLOT(setPosNoLimit(unsigned))); 
      connect(canvas, SIGNAL(curPartHasChanged(MusECore::Part*)), SLOT(updateTrackInfo()));

      connect(hscroll, SIGNAL(scaleChanged(int)),  SLOT(updateHScrollRange()));
      connect(MusEGlobal::song, SIGNAL(songChanged(MusECore::SongChangedStruct_t)), SLOT(songChanged1(MusECore::SongChangedStruct_t)));

      // For the wave editor, let's start with the operation range selection tool.
      canvas->setTool(MusEGui::RangeTool);
      tools2->set(MusEGui::RangeTool);
      
      setEventColorMode(colorMode);
      
      initShortcuts();
      
      updateHScrollRange();
      configChanged();
      
      if(!parts()->empty())
      {
        MusECore::WavePart* part = (MusECore::WavePart*)(parts()->begin()->second);
        solo->setChecked(part->track()->solo());
      }

      QList<int> mops;
      mops.append(_trackInfoWidthInit);
      mops.append(_canvasWidthInit);
      hsplitter->setSizes(mops);
    
      if(canvas->track())
      {
        updateTrackInfo();
        //toolbar->setSolo(canvas->track()->solo());
      }
         
      initTopwinState();
      finalizeInit();
      }

void WaveEdit::initShortcuts()
      {
      cutAction->setShortcut(shortcuts[SHRT_CUT].key);
      copyAction->setShortcut(shortcuts[SHRT_COPY].key);
      pasteAction->setShortcut(shortcuts[SHRT_PASTE].key);
      selectAllAction->setShortcut(shortcuts[SHRT_SELECT_ALL].key);
      selectNoneAction->setShortcut(shortcuts[SHRT_SELECT_NONE].key);
      
      //selectInvertAction->setShortcut(shortcuts[SHRT_SELECT_INVERT].key);
      //selectInsideLoopAction->setShortcut(shortcuts[SHRT_SELECT_ILOOP].key);
      //selectOutsideLoopAction->setShortcut(shortcuts[SHRT_SELECT_OLOOP].key);
      selectPrevPartAction->setShortcut(shortcuts[SHRT_SELECT_PREV_PART].key);
      selectNextPartAction->setShortcut(shortcuts[SHRT_SELECT_NEXT_PART].key);
      
//      eventColor->menuAction()->setShortcut(shortcuts[SHRT_EVENT_COLOR].key);
      //evColorNormalAction->setShortcut(shortcuts[  ].key);
      //evColorPartsAction->setShortcut(shortcuts[  ].key);
      
      }

//---------------------------------------------------------
//   configChanged
//---------------------------------------------------------

void WaveEdit::configChanged()
      {
      if (MusEGlobal::config.canvasBgPixmap.isEmpty()) {
            canvas->setBg(MusEGlobal::config.waveEditBackgroundColor);
            canvas->setBg(QPixmap());
      }
      else {
            canvas->setBg(QPixmap(MusEGlobal::config.canvasBgPixmap));
      }
      
      initShortcuts();
      }

//---------------------------------------------------------
//   updateHScrollRange
//---------------------------------------------------------
void WaveEdit::updateHScrollRange()
{
      int s, e;
      canvas->range(&s, &e);   // Range in frames
      unsigned tm = MusEGlobal::sigmap.ticksMeasure(MusEGlobal::tempomap.frame2tick(e));
      
      // Show one more measure, and show another quarter measure due to imprecise drawing at canvas end point.
      //e += MusEGlobal::tempomap.tick2frame(tm + tm / 4);  // TODO: Try changing scrollbar to use units of frames?
      e += (tm + tm / 4);
      
      // Compensate for the vscroll width. 
      //e += wview->rmapxDev(-vscroll->width()); 
      int s1, e1;
      hscroll->range(&s1, &e1);                             //   ...
      if(s != s1 || e != e1) 
        hscroll->setRange(s, e);                            //   ...
}

//---------------------------------------------------------
//   timeChanged
//---------------------------------------------------------

void WaveEdit::timeChanged(unsigned t)
{
      if(t == INT_MAX)
      {
        // Let the PosLabels disable themselves with INT_MAX.
        pos1->setValue(t);
        pos2->setValue(t);
        return;
      }
     
      unsigned frame = MusEGlobal::tempomap.tick2frame(t);
      pos1->setValue(t);
      pos2->setValue(frame);
      time->setPos(3, t, false);
}

//---------------------------------------------------------
//   setTime
//---------------------------------------------------------

void WaveEdit::setTime(unsigned samplepos)
      {
      if(samplepos == INT_MAX)
      {
        // Let the PosLabels disable themselves with INT_MAX.
        pos1->setValue(samplepos);
        pos2->setValue(samplepos);
        return;
      }
     
      // Normally frame to tick methods round down. But here we need it to 'snap'
      //  the frame from either side of a tick to the tick. So round to nearest.
      unsigned tick = MusEGlobal::tempomap.frame2tick(samplepos, 0, MusECore::LargeIntRoundNearest);
      pos1->setValue(tick);
      pos2->setValue(samplepos);
      time->setPos(3, tick, false);
      }

//---------------------------------------------------------
//   ~WaveEdit
//---------------------------------------------------------

WaveEdit::~WaveEdit()
      {
      DEBUG_WAVEEDIT(stderr, "WaveEdit dtor\n");
      }

//---------------------------------------------------------
//   cmd
//---------------------------------------------------------

void WaveEdit::cmd(int n)
      {
      // Don't process if user is dragging or has clicked on the events. 
      // Causes crashes later in Canvas::viewMouseMoveEvent and viewMouseReleaseEvent.
      if(canvas->getCurrentDrag())
        return;
      
      ((WaveCanvas*)canvas)->cmd(n);
      }

//---------------------------------------------------------
//   loadConfiguration
//---------------------------------------------------------

void WaveEdit::readConfiguration(MusECore::Xml& xml)
      {
      for (;;) {
            MusECore::Xml::Token token = xml.parse();
            const QString& tag = xml.s1();
            switch (token) {
                  case MusECore::Xml::TagStart:
                        if (tag == "bgcolor")
                              MusEGlobal::config.waveEditBackgroundColor = readColor(xml);
                        else if (tag == "raster")
                              _rasterInit = xml.parseInt();
                        else if (tag == "trackinfowidth")
                              _trackInfoWidthInit = xml.parseInt();
                        else if (tag == "canvaswidth")
                              _canvasWidthInit = xml.parseInt();
                        else if (tag == "colormode")
                              colorModeInit = xml.parseInt();
                        else if (tag == "topwin")
                              TopWin::readConfiguration(WAVE, xml);
                        else
                              xml.unknown("WaveEdit");
                        break;
                  case MusECore::Xml::TagEnd:
                        if (tag == "waveedit")
                              return;
                  default:
                        break;
                  case MusECore::Xml::Error:
                  case MusECore::Xml::End:
                        return;
                  }
            }
      }

//---------------------------------------------------------
//   saveConfiguration
//---------------------------------------------------------

void WaveEdit::writeConfiguration(int level, MusECore::Xml& xml)
      {
      xml.tag(level++, "waveedit");
      xml.colorTag(level, "bgcolor", MusEGlobal::config.waveEditBackgroundColor);
      xml.intTag(level, "raster", _rasterInit);
      xml.intTag(level, "trackinfowidth", _trackInfoWidthInit);
      xml.intTag(level, "canvaswidth", _canvasWidthInit);
      xml.intTag(level, "colormode", colorModeInit);
      TopWin::writeConfiguration(WAVE, level,xml);
      xml.tag(level, "/waveedit");
      }

//---------------------------------------------------------
//   writeStatus
//---------------------------------------------------------

void WaveEdit::writeStatus(int level, MusECore::Xml& xml) const
      {
      writePartList(level, xml);
      xml.tag(level++, "waveedit");
      MidiEditor::writeStatus(level, xml);
      xml.intTag(level, "tool", int(canvas->tool()));
      xml.intTag(level, "xmag", hscroll->mag());
      xml.intTag(level, "xpos", hscroll->pos());
      xml.intTag(level, "ymag", ymag->value());
      xml.tag(level, "/waveedit");
      }

//---------------------------------------------------------
//   readStatus
//---------------------------------------------------------

void WaveEdit::readStatus(MusECore::Xml& xml)
      {
      for (;;) {
            MusECore::Xml::Token token = xml.parse();
            if (token == MusECore::Xml::Error || token == MusECore::Xml::End)
                  break;
            QString tag = xml.s1();
            switch (token) {
                  case MusECore::Xml::TagStart:
                        if (tag == "midieditor")
                              MidiEditor::readStatus(xml);
                        else if (tag == "tool") {
                              int tool = xml.parseInt();
                              canvas->setTool(tool);
                              tools2->set(tool);
                              }
                        else if (tag == "xmag")
                              hscroll->setMag(xml.parseInt());
                        else if (tag == "ymag")
                              ymag->setValue(xml.parseInt());
                        else if (tag == "xpos")
                              hscroll->setPos(xml.parseInt());
                        else
                              xml.unknown("WaveEdit");
                        break;
                  case MusECore::Xml::TagEnd:
                        if (tag == "waveedit")
                              return;
                  default:
                        break;
                  }
            }
      }


//---------------------------------------------------------
//   songChanged1
//    signal from "song"
//---------------------------------------------------------

void WaveEdit::songChanged1(MusECore::SongChangedStruct_t bits)
      {
        if(_isDeleting)  // Ignore while while deleting to prevent crash.
          return;

        // We must catch this first and be sure to update the strips.
        if(bits & SC_TRACK_REMOVED)
          checkTrackInfoTrack();
        
        if (bits & SC_SOLO)
        {
          MusECore::WavePart* part = (MusECore::WavePart*)(parts()->begin()->second);
          solo->blockSignals(true);
          solo->setChecked(part->track()->solo());
          solo->blockSignals(false);
        }  
        
        songChanged(bits);

        // We'll receive SC_SELECTION if a different part is selected.
        // Addition - also need to respond here to moving part to another track. (Tim)
        if (bits & (SC_PART_INSERTED | SC_PART_REMOVED))
          updateTrackInfo();

        // We must marshall song changed instead of connecting to the strip's song changed
        //  otherwise it crashes when loading another song because track is no longer valid
        //  and the strip's songChanged() seems to be called before Pianoroll songChanged()
        //  gets called and has a chance to stop the crash.
        // Also, calling updateTrackInfo() from here is too heavy, it destroys and recreates
        //  the strips each time no matter what the flags are !
        else  
          trackInfoSongChange(bits);
      }


//---------------------------------------------------------
//   soloChanged
//    signal from solo button
//---------------------------------------------------------

void WaveEdit::soloChanged(bool flag)
      {
      MusECore::WavePart* part = (MusECore::WavePart*)(parts()->begin()->second);
      // This is a minor operation easily manually undoable. Let's not clog the undo list with it.
      MusECore::PendingOperationList operations;
      operations.add(MusECore::PendingOperationItem(part->track(), flag, MusECore::PendingOperationItem::SetTrackSolo));
      MusEGlobal::audio->msgExecutePendingOperations(operations, true);
      }

//---------------------------------------------------------
//   viewKeyPressEvent
//---------------------------------------------------------

void WaveEdit::keyPressEvent(QKeyEvent* event)
      {
// TODO: Raster:
//       int index;
//       int n = sizeof(rasterTable)/sizeof(*rasterTable);
//       for (index = 0; index < n; ++index)
//             if (rasterTable[index] == raster())
//                   break;
//       if (index == n) {
//             index = 0;
//             // raster 1 is not in table
//             }
//       int off = (index / 9) * 9;
//       index   = index % 9;

//       int val = 0;

      WaveCanvas* wc = (WaveCanvas*)canvas;
      int key = event->key();

      if (((QInputEvent*)event)->modifiers() & Qt::ShiftModifier)
            key += Qt::SHIFT;
      if (((QInputEvent*)event)->modifiers() & Qt::AltModifier)
            key += Qt::ALT;
      if (((QInputEvent*)event)->modifiers() & Qt::ControlModifier)
            key+= Qt::CTRL;

      if (key == Qt::Key_Escape) {
            close();
            return;
            }
            
      else if (key == shortcuts[SHRT_POS_INC].key) {
            wc->waveCmd(CMD_RIGHT);
            return;
            }
      else if (key == shortcuts[SHRT_POS_DEC].key) {
            wc->waveCmd(CMD_LEFT);
            return;
            }
      else if (key == shortcuts[SHRT_POS_INC_NOSNAP].key) {
            wc->waveCmd(CMD_RIGHT_NOSNAP);
            return;
            }
      else if (key == shortcuts[SHRT_POS_DEC_NOSNAP].key) {
            wc->waveCmd(CMD_LEFT_NOSNAP);
            return;
            }
      else if (key == shortcuts[SHRT_INSERT_AT_LOCATION].key) {
            wc->waveCmd(CMD_INSERT);
            return;
            }
      else if (key == Qt::Key_Backspace) {
            wc->waveCmd(CMD_BACKSPACE);
            return;
            }
            
      else if (key == shortcuts[SHRT_TOOL_POINTER].key) {
            tools2->set(MusEGui::PointerTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_PENCIL].key) {
            tools2->set(MusEGui::PencilTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_RUBBER].key) {
            tools2->set(MusEGui::RubberTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_SCISSORS].key) {
            tools2->set(MusEGui::CutTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_PAN].key) {
            tools2->set(MusEGui::PanTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_ZOOM].key) {
            tools2->set(MusEGui::ZoomTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_RANGE].key) {
            tools2->set(MusEGui::RangeTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_STRETCH].key) {
            tools2->set(MusEGui::StretchTool);
            return;
            }
      else if (key == shortcuts[SHRT_TOOL_SAMPLERATE].key) {
            tools2->set(MusEGui::SamplerateTool);
            return;
            }
            
      else if (key == shortcuts[SHRT_EVENT_COLOR].key) {
            if (colorMode == 0)
                  colorMode = 1;
            else if (colorMode == 1)
                  colorMode = 0;
            setEventColorMode(colorMode);
            return;
            }
            
      // TODO: New WaveCanvas: Convert some of these to use frames.
      else if (key == shortcuts[SHRT_ZOOM_IN].key) {
            horizontalZoom(true, QCursor::pos());
            return;
            }
      else if (key == shortcuts[SHRT_ZOOM_OUT].key) {
            horizontalZoom(false, QCursor::pos());
            return;
            }
      else if (key == shortcuts[SHRT_GOTO_CPOS].key) {
            MusECore::PartList* p = this->parts();
            MusECore::Part* first = p->begin()->second;
            hscroll->setPos(MusEGlobal::song->cpos() - first->tick() );
            return;
            }
      else if (key == shortcuts[SHRT_SCROLL_LEFT].key) {
            int pos = hscroll->pos() - MusEGlobal::config.division;
            if (pos < 0)
                  pos = 0;
            hscroll->setPos(pos);
            return;
            }
      else if (key == shortcuts[SHRT_SCROLL_RIGHT].key) {
            int pos = hscroll->pos() + MusEGlobal::config.division;
            hscroll->setPos(pos);
            return;
            }
            
// TODO: Raster:            
//       else if (key == shortcuts[SHRT_SET_QUANT_1].key)
//             val = rasterTable[8 + off];
//       else if (key == shortcuts[SHRT_SET_QUANT_2].key)
//             val = rasterTable[7 + off];
//       else if (key == shortcuts[SHRT_SET_QUANT_3].key)
//             val = rasterTable[6 + off];
//       else if (key == shortcuts[SHRT_SET_QUANT_4].key)
//             val = rasterTable[5 + off];
//       else if (key == shortcuts[SHRT_SET_QUANT_5].key)
//             val = rasterTable[4 + off];
//       else if (key == shortcuts[SHRT_SET_QUANT_6].key)
//             val = rasterTable[3 + off];
//       else if (key == shortcuts[SHRT_SET_QUANT_7].key)
//             val = rasterTable[2 + off];
//       else if (key == shortcuts[SHRT_TOGGLE_TRIOL].key)
//             val = rasterTable[index + ((off == 0) ? 9 : 0)];
//       else if (key == shortcuts[SHRT_TOGGLE_PUNCT].key)
//             val = rasterTable[index + ((off == 18) ? 9 : 18)];
//       else if (key == shortcuts[SHRT_TOGGLE_PUNCT2].key) {//CDW
//             if ((off == 18) && (index > 2)) {
//                   val = rasterTable[index + 9 - 1];
//                   }
//             else if ((off == 9) && (index < 8)) {
//                   val = rasterTable[index + 18 + 1];
//                   }
//             else
//                   return;
//             }
            
      else { //Default:
            event->ignore();
            return;
            }
        
      // TODO: Raster:  
      //setRaster(val);
      //toolbar->setRaster(_raster);
      }

//---------------------------------------------------------
//   moveVerticalSlider
//---------------------------------------------------------

void WaveEdit::moveVerticalSlider(int val)
      {
      ymag->setValue(ymag->value() + val);
      }

void WaveEdit::horizontalZoom(bool zoom_in, const QPoint& glob_pos)
{
  int mag = hscroll->mag();
  int zoomlvl = ScrollScale::getQuickZoomLevel(mag);
  if(zoom_in)
  {
    if (zoomlvl < MusEGui::ScrollScale::zoomLevels-1)
        zoomlvl++;
  }
  else
  {
    if (zoomlvl > 1)
        zoomlvl--;
  }
  int newmag = ScrollScale::convertQuickZoomLevelToMag(zoomlvl);

  QPoint cp = canvas->mapFromGlobal(glob_pos);
  QPoint sp = mainw->mapFromGlobal(glob_pos);
  if(cp.x() >= 0 && cp.x() < canvas->width() && sp.y() >= 0 && sp.y() < mainw->height())
    hscroll->setMag(newmag, cp.x());
}

void WaveEdit::horizontalZoom(int mag, const QPoint& glob_pos)
{
  QPoint cp = canvas->mapFromGlobal(glob_pos);
  QPoint sp = mainw->mapFromGlobal(glob_pos);
  if(cp.x() >= 0 && cp.x() < canvas->width() && sp.y() >= 0 && sp.y() < mainw->height())
    hscroll->setMag(hscroll->mag() + mag, cp.x());
}

//---------------------------------------------------------
//   focusCanvas
//---------------------------------------------------------

void WaveEdit::focusCanvas()
{
  if(MusEGlobal::config.smartFocus)
  {
    canvas->setFocus();
    canvas->activateWindow();
  }
}

//---------------------------------------------------------
//   eventColorModeChanged
//---------------------------------------------------------

void WaveEdit::eventColorModeChanged(int mode)
      {
      colorMode = mode;
      colorModeInit = colorMode;
      
      ((WaveCanvas*)(canvas))->setColorMode(colorMode);
      }

//---------------------------------------------------------
//   setEventColorMode
//---------------------------------------------------------

void WaveEdit::setEventColorMode(int mode)
      {
      colorMode = mode;
      colorModeInit = colorMode;
      
      evColorNormalAction->setChecked(mode == 0);
      evColorPartsAction->setChecked(mode == 1);
      
      ((WaveCanvas*)(canvas))->setColorMode(colorMode);
      }



} // namespace MusEGui
