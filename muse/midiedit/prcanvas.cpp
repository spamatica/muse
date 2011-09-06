//=========================================================
//  MusE
//  Linux Music Editor
//    $Id: prcanvas.cpp,v 1.20.2.19 2009/11/16 11:29:33 lunar_shuttle Exp $
//  (C) Copyright 1999-2004 Werner Schweer (ws@seh.de)
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

#include <QApplication>
#include <QClipboard>
#include <QPainter>
#include <QDrag>
#include <QDragLeaveEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QMouseEvent>

#include <set>

#include <values.h>
#include <stdio.h>
#include <math.h>
#include <errno.h>
//#include <sys/stat.h>
//#include <sys/types.h>
//#include <sys/mman.h>
//#include <fcntl.h>
//#include <dirent.h>

#include "xml.h"
#include "prcanvas.h"
#include "midiport.h"
#include "event.h"
#include "mpevent.h"
#include "globals.h"
#include "cmd.h"
#include "song.h"
#include "audio.h"
#include "functions.h"
#include "gconfig.h"

#define CHORD_TIMEOUT 75

//---------------------------------------------------------
//   NEvent
//---------------------------------------------------------

NEvent::NEvent(Event& e, Part* p, int y) : CItem(e, p)
      {
      y = y - KH/4;
      unsigned tick = e.tick() + p->tick();
      setPos(QPoint(tick, y));
      setBBox(QRect(tick, y, e.lenTick(), KH/2));
      }

//---------------------------------------------------------
//   addItem
//---------------------------------------------------------

void PianoCanvas::addItem(Part* part, Event& event)
      {
      if (signed(event.tick())<0) {
            printf("ERROR: trying to add event before current part!\n");
            return;
      }

      NEvent* ev = new NEvent(event, part, pitch2y(event.pitch()));
      items.add(ev);

      int diff = event.tick()-part->lenTick();
      if (diff > 0)  {// too short part? extend it
            //printf("addItem - this code should not be run!\n");
            //Part* newPart = part->clone();
            //newPart->setLenTick(newPart->lenTick()+diff);
            //audio->msgChangePart(part, newPart,false);
            //part = newPart;
            part->setLenTick(part->lenTick()+diff);
            }
      }

//---------------------------------------------------------
//   PianoCanvas
//---------------------------------------------------------

PianoCanvas::PianoCanvas(MidiEditor* pr, QWidget* parent, int sx, int sy)
   : EventCanvas(pr, parent, sx, sy)
      {
      colorMode = 0;
      playedPitch = -1;
      for (int i=0;i<128;i++) noteHeldDown[i]=false;
      
      steprec=new StepRec(noteHeldDown);
      
      songChanged(SC_TRACK_INSERTED);
      connect(song, SIGNAL(midiNote(int, int)), SLOT(midiNote(int,int)));
      }

//---------------------------------------------------------
//   pitch2y
//---------------------------------------------------------

int PianoCanvas::pitch2y(int pitch) const
      {
      int tt[] = {
            5, 13, 19, 26, 34, 44, 52, 58, 65, 71, 78, 85
            };
      int y = (75 * KH) - (tt[pitch%12] + (7 * KH) * (pitch/12));
      if (y < 0)
            y = 0;
      return y;
      }

//---------------------------------------------------------
//   y2pitch
//---------------------------------------------------------

int PianoCanvas::y2pitch(int y) const
      {
      const int total = (10 * 7 + 5) * KH;       // 75 Ganztonschritte
      y = total - y;
      int oct = (y / (7 * KH)) * 12;
      char kt[] = {
            0, 0, 0, 0, 0,  0,   0, 0, 0, 0,          // 5
            1, 1, 1,      1,   1, 1, 1,               // 13
            2, 2,         2,   2, 2, 2,               // 19
            3, 3, 3,      3,   3, 3, 3,               // 26
            4, 4, 4, 4,   4,   4, 4, 4, 4,            // 34
            5, 5, 5, 5,   5,   5, 5, 5, 5, 5,         // 43
            6, 6, 6,      6,   6, 6, 6,               // 52
            7, 7,         7,   7, 7, 7,               // 58
            8, 8, 8,      8,   8, 8, 8,               // 65
            9, 9,         9,   9, 9, 9,               // 71
            10, 10, 10,  10,   10, 10, 10,            // 78
            11, 11, 11, 11, 11,   11, 11, 11, 11, 11  // 87
            };
      return kt[y % 91] + oct;
      }

//---------------------------------------------------------
//   drawEvent
//    draws a note
//---------------------------------------------------------

void PianoCanvas::drawItem(QPainter& p, const CItem* item,
   const QRect& rect)
      {
      QRect r = item->bbox();
      if(!virt())
        r.moveCenter(map(item->pos()));
      
      //QRect rr = p.transform().mapRect(rect);  // Gives inconsistent positions. Source shows wrong operation for our needs.
      QRect rr = map(rect);                      // Use our own map instead.        
      QRect mer = map(r);                              
      
      ///r = r.intersected(rect);
      //QRect rr = r & rect;
      ///if(!r.isValid())
      //if(!rr.isValid())
      //  return;
      QRect mr = rr & mer;
      //if(!mr.isValid())
      if(mr.isNull())
        return;
      
      //p.save();
      
      p.setPen(Qt::black);
      struct Triple {
            int r, g, b;
            };

      static Triple myColors /*Qt::color1*/[12] = {  // ddskrjp
            { 0xff, 0x3d, 0x39 },
            { 0x39, 0xff, 0x39 },
            { 0x39, 0x3d, 0xff },
            { 0xff, 0xff, 0x39 },
            { 0xff, 0x3d, 0xff },
            { 0x39, 0xff, 0xff },
            { 0xff, 0x7e, 0x7a },
            { 0x7a, 0x7e, 0xff },
            { 0x7a, 0xff, 0x7a },
            { 0xff, 0x7e, 0xbf },
            { 0x7a, 0xbf, 0xff },
            { 0xff, 0xbf, 0x7a }
            };

      QColor color;
      NEvent* nevent   = (NEvent*) item;
      Event event = nevent->event();
      if (nevent->part() != curPart){
            if(item->isMoving()) 
              color = Qt::gray;
            else if(item->isSelected()) 
              color = Qt::black;
            else  
              color = Qt::lightGray;
      }      
      else {
            if (item->isMoving()) {
                    color = Qt::gray;
                }
            else if (item->isSelected()) {
                  color = Qt::black;
                  }
            else {
                  color.setRgb(0, 0, 255);
                  switch(colorMode) {
                        case 0:
                              break;
                        case 1:     // pitch
                              {
                              Triple* c = &myColors/*Qt::color1*/[event.pitch() % 12];
                              color.setRgb(c->r, c->g, c->b);
                              }
                              break;
                        case 2:     // velocity
                              {
                              int velo = event.velo();
                              if (velo < 64)
                                    color.setRgb(velo*4, 0, 0xff);
                              else
                                    color.setRgb(0xff, 0, (127-velo) * 4);
                              }
                              break;
                        }
                  }
            }
      
      bool wmtxen = p.worldMatrixEnabled();
      p.setWorldMatrixEnabled(false);
      int mx = mr.x();
      int my = mr.y();
      int mw = mr.width();
      int mh = mr.height();
      int mex = mer.x();
      int mey = mer.y();
      int mew = mer.width();
      int meh = mer.height();
      //int mfx = mx;
      //if(mfx == mex) mfx += 1;
      //int mfy = my;
      //if(mfy == mey) mfy += 1;
      //int mfw = mw;
      //if(mfw == mew) mfw -= 1;
      //if(mfx == mex) mfw -= 1;
      //int mfh = mh;
      //if(mfh == meh) mfh -= 1;
      //if(mfy == mey) mfh -= 1;
      color.setAlpha(config.globalAlphaBlend);
      //QLinearGradient gradient(mex + 1, mey + 1, mex + 1, mey + meh - 2);    // Inside the border
      //gradient.setColorAt(0, color);
      //gradient.setColorAt(1, color.darker());
      //QBrush brush(gradient);
      QBrush brush(color);
      p.fillRect(mr, brush);       
      //p.fillRect(mfx, mfy, mfw, mfh, brush);       
      
      if(mex >= mx && mex <= mx + mw)
        p.drawLine(mex, my, mex, my + mh - 1);                       // The left edge
      if(mex + mew >= mx && mex + mew <= mx + mw)
        p.drawLine(mex + mew, my, mex + mew, my + mh - 1);           // The right edge
      if(mey >= my && mey <= my + mh)
        p.drawLine(mx, mey, mx + mw - 1, mey);                       // The top edge
      if(mey + meh >= my && mey + meh <= my + mh)
        p.drawLine(mx, mey + meh - 1, mx + mw - 1, mey + meh - 1);   // The bottom edge
      
      //p.setWorldMatrixEnabled(true);
      p.setWorldMatrixEnabled(wmtxen);
      //p.restore();
      }

//---------------------------------------------------------
//   drawTopItem
//---------------------------------------------------------
void PianoCanvas::drawTopItem(QPainter& , const QRect&)
{}

//---------------------------------------------------------
//   drawMoving
//    draws moving items
//---------------------------------------------------------

void PianoCanvas::drawMoving(QPainter& p, const CItem* item, const QRect& rect)
    {
      //if(((NEvent*)item)->part() != curPart)
      //  return;
      //if(!item->isMoving()) 
      //  return;
      QRect mr = QRect(item->mp().x(), item->mp().y() - item->height()/2, item->width(), item->height());
      mr = mr.intersected(rect);
      if(!mr.isValid())
        return;
      p.setPen(Qt::black);
      p.setBrush(Qt::NoBrush);
      p.drawRect(mr);
    }

//---------------------------------------------------------
//   viewMouseDoubleClickEvent
//---------------------------------------------------------

void PianoCanvas::viewMouseDoubleClickEvent(QMouseEvent* event)
      {
      if ((_tool != PointerTool) && (event->button() != Qt::LeftButton)) {
            mousePress(event);
            return;
            }
      }

//---------------------------------------------------------
//   moveCanvasItems
//---------------------------------------------------------

Undo PianoCanvas::moveCanvasItems(CItemList& items, int dp, int dx, DragType dtype)
{      
  if(editor->parts()->empty())
    return Undo(); //return empty list
  
  PartsToChangeMap parts2change;
  Undo operations;  
  
  for(iPart ip = editor->parts()->begin(); ip != editor->parts()->end(); ++ip)
  {
    Part* part = ip->second;
    if(!part)
      continue;
    
    int npartoffset = 0;
    for(iCItem ici = items.begin(); ici != items.end(); ++ici) 
    {
      CItem* ci = ici->second;
      if(ci->part() != part)
        continue;
      
      int x = ci->pos().x() + dx;
      int y = pitch2y(y2pitch(ci->pos().y()) + dp);
      QPoint newpos = raster(QPoint(x, y));
      
      // Test moving the item...
      NEvent* nevent = (NEvent*) ci;
      Event event    = nevent->event();
      x              = newpos.x();
      if(x < 0)
        x = 0;
      int ntick = editor->rasterVal(x) - part->tick();
      if(ntick < 0)
        ntick = 0;
      int diff = ntick + event.lenTick() - part->lenTick();
      
      // If moving the item would require a new part size...
      if(diff > npartoffset)
        npartoffset = diff;
    }
        
    if(npartoffset > 0)
    {    
      iPartToChange ip2c = parts2change.find(part);
      if(ip2c == parts2change.end())
      {
        PartToChange p2c = {0, npartoffset};
        parts2change.insert(std::pair<Part*, PartToChange> (part, p2c));
      }
      else
        ip2c->second.xdiff = npartoffset;
    }
  }
  
  bool forbidden=false;
  for(iPartToChange ip2c = parts2change.begin(); ip2c != parts2change.end(); ++ip2c)
  {
    Part* opart = ip2c->first;
    //int diff = ip2c->second.xdiff;
    
    if (opart->hasHiddenEvents())
    {
			forbidden=true;
			break;
		}
  }    

	
	if (!forbidden)
	{
		std::vector< CItem* > doneList;
		typedef std::vector< CItem* >::iterator iDoneList;
		
		for(iCItem ici = items.begin(); ici != items.end(); ++ici) 
		{
			CItem* ci = ici->second;
			
			int x = ci->pos().x();
			int y = ci->pos().y();
			int nx = x + dx;
			int ny = pitch2y(y2pitch(y) + dp);
			QPoint newpos = raster(QPoint(nx, ny));
			selectItem(ci, true);
			
			iDoneList idl;
			for(idl = doneList.begin(); idl != doneList.end(); ++idl)
				// This compares EventBase pointers to see if they're the same...
				if((*idl)->event() == ci->event())
					break;
				
			// Do not process if the event has already been processed (meaning it's an event in a clone part)...
			if (idl == doneList.end())
			{
				operations.push_back(moveItem(ci, newpos, dtype));
				doneList.push_back(ci);
			}
			ci->move(newpos);
						
			if(moving.size() == 1) 
						itemReleased(curItem, newpos);

			if(dtype == MOVE_COPY || dtype == MOVE_CLONE)
						selectItem(ci, false);
		}  

    for(iPartToChange ip2c = parts2change.begin(); ip2c != parts2change.end(); ++ip2c)
    {
      Part* opart = ip2c->first;
      int diff = ip2c->second.xdiff;
      
      schedule_resize_all_same_len_clone_parts(opart, opart->lenTick() + diff, operations);
    }    
					
  	return operations;
  }
  else
  {
		return Undo(); //return empty list
	}
}
      
//---------------------------------------------------------
//   moveItem
//    called after moving an object
//---------------------------------------------------------

UndoOp PianoCanvas::moveItem(CItem* item, const QPoint& pos, DragType dtype)
      {
      NEvent* nevent = (NEvent*) item;
      Event event    = nevent->event();
      int npitch     = y2pitch(pos.y());
      Event newEvent = event.clone();
      int x          = pos.x();
      if (x < 0)
            x = 0;
      if (event.pitch() != npitch && _playEvents) {
            int port    = track()->outPort();
            int channel = track()->outChannel();
            // release note:
            MidiPlayEvent ev1(0, port, channel, 0x90, event.pitch() + track()->transposition, 0);
            audio->msgPlayMidiEvent(&ev1);
            MidiPlayEvent ev2(0, port, channel, 0x90, npitch + track()->transposition, event.velo());
            audio->msgPlayMidiEvent(&ev2);
            }
      
      // Changed by T356. 
      Part* part = nevent->part(); //
      //Part* part = Canvas::part();  // part can be dynamically recreated, ask the authority
      
      newEvent.setPitch(npitch);
      int ntick = editor->rasterVal(x) - part->tick();
      if (ntick < 0)
            ntick = 0;
      newEvent.setTick(ntick);
      newEvent.setLenTick(event.lenTick());

      // Added by T356, removed by flo93: with operation groups, it happens that the
      // part is too short right now, even if it's queued for being extended
      //if(((int)newEvent.endTick() - (int)part->lenTick()) > 0)  
      //  printf("PianoCanvas::moveItem Error! New event end:%d exceeds length:%d of part:%s\n", newEvent.endTick(), part->lenTick(), part->name().toLatin1().constData());
      
      if (dtype == MOVE_COPY || dtype == MOVE_CLONE)
            return UndoOp(UndoOp::AddEvent, newEvent, part, false, false);
      else
            return UndoOp(UndoOp::ModifyEvent, newEvent, event, part, false, false);
      }

//---------------------------------------------------------
//   newItem(p, state)
//---------------------------------------------------------

CItem* PianoCanvas::newItem(const QPoint& p, int)
      {
      //printf("newItem point\n");
      int pitch = y2pitch(p.y());
      int tick  = editor->rasterVal1(p.x());
      int len   = p.x() - tick;
      tick     -= curPart->tick();
      if (tick < 0)
            tick=0;
      Event e =  Event(Note);
      e.setTick(tick);
      e.setPitch(pitch);
      e.setVelo(curVelo);
      e.setLenTick(len);
      return new NEvent(e, curPart, pitch2y(pitch));
      }

void PianoCanvas::newItem(CItem* item, bool noSnap)
      {
      //printf("newItem citem\n");
      NEvent* nevent = (NEvent*) item;
      Event event    = nevent->event();
      int x = item->x();
      if (x<0)
            x=0;
      int w = item->width();

      if (!noSnap) {
            x = editor->rasterVal1(x); //round down
            w = editor->rasterVal(x + w) - x;
            if (w == 0)
                  w = editor->raster();
            }
      Part* part = nevent->part();
      event.setTick(x - part->tick());
      event.setLenTick(w);
      event.setPitch(y2pitch(item->y()));

      Undo operations;
      int diff = event.endTick()-part->lenTick();
      
      if (! ((diff > 0) && part->hasHiddenEvents()) ) //operation is allowed
      {
        operations.push_back(UndoOp(UndoOp::AddEvent,event, part, false, false));
        
        if (diff > 0)// part must be extended?
        {
              schedule_resize_all_same_len_clone_parts(part, event.endTick(), operations);
              printf("newItem: extending\n");
        }
        
        song->applyOperationGroup(operations);
      }
      //else forbid action by not applying it
      else      
          songChanged(SC_EVENT_INSERTED); //this forces an update of the itemlist, which is neccessary
                                          //to remove "forbidden" events from the list again
      }

//---------------------------------------------------------
//   resizeItem
//---------------------------------------------------------

void PianoCanvas::resizeItem(CItem* item, bool noSnap, bool)         // experimental changes to try dynamically extending parts
      {
      //printf("resizeItem!\n");
      NEvent* nevent = (NEvent*) item;
      Event event    = nevent->event();
      Event newEvent = event.clone();
      int len;

      Part* part = nevent->part();

      if (noSnap)
            len = nevent->width();
      else {
            //Part* part = nevent->part();
            unsigned tick = event.tick() + part->tick();
            len = editor->rasterVal(tick + nevent->width()) - tick;
            if (len <= 0)
                  len = editor->raster();
      }

      Undo operations;
      int diff = event.tick()+len-part->lenTick();
      
      if (! ((diff > 0) && part->hasHiddenEvents()) ) //operation is allowed
      {
        newEvent.setLenTick(len);
        operations.push_back(UndoOp(UndoOp::ModifyEvent,newEvent, event, nevent->part(), false, false));
        
        if (diff > 0)// part must be extended?
        {
              schedule_resize_all_same_len_clone_parts(part, event.tick()+len, operations);
              printf("resizeItem: extending\n");
        }
      }
      //else forbid action by not performing it
      song->applyOperationGroup(operations);
      songChanged(SC_EVENT_MODIFIED); //this forces an update of the itemlist, which is neccessary
                                      //to remove "forbidden" events from the list again
      }

//---------------------------------------------------------
//   deleteItem
//---------------------------------------------------------

bool PianoCanvas::deleteItem(CItem* item)
      {
      NEvent* nevent = (NEvent*) item;
      if (nevent->part() == curPart) {
            Event ev = nevent->event();
            // Indicate do undo, and do not do port controller values and clone parts. 
            audio->msgDeleteEvent(ev, curPart, true, false, false);
            return true;
            }
      return false;
      }

//---------------------------------------------------------
//   pianoCmd
//---------------------------------------------------------

void PianoCanvas::pianoCmd(int cmd)
      {
      switch(cmd) {
            case CMD_LEFT:
                  {
                  int spos = pos[0];
                  if(spos > 0) 
                  {
                    spos -= 1;     // Nudge by -1, then snap down with raster1.
                    spos = AL::sigmap.raster1(spos, editor->rasterStep(pos[0]));
                  }  
                  if(spos < 0)
                    spos = 0;
                  Pos p(spos,true);
                  song->setPos(0, p, true, true, true);
                  }
                  break;
            case CMD_RIGHT:
                  {
                  int spos = AL::sigmap.raster2(pos[0] + 1, editor->rasterStep(pos[0]));    // Nudge by +1, then snap up with raster2.
                  Pos p(spos,true);
                  song->setPos(0, p, true, true, true); 
                  }
                  break;
            case CMD_LEFT_NOSNAP:
                  {
                  int spos = pos[0] - editor->rasterStep(pos[0]);
                  if (spos < 0)
                        spos = 0;
                  Pos p(spos,true);
                  song->setPos(0, p, true, true, true); //CDW
                  }
                  break;
            case CMD_RIGHT_NOSNAP:
                  {
                  Pos p(pos[0] + editor->rasterStep(pos[0]), true);
                  song->setPos(0, p, true, true, true); //CDW
                  }
                  break;
            case CMD_INSERT:
                  {
                  if (pos[0] < start() || pos[0] >= end())
                        break;
                  MidiPart* part = (MidiPart*)curPart;

                  if (part == 0)
                        break;

                  EventList* el = part->events();
                  Undo operations;

                  std::list <Event> elist;
                  for (iEvent e = el->lower_bound(pos[0] - part->tick()); e != el->end(); ++e)
                        elist.push_back((Event)e->second);
                  for (std::list<Event>::iterator i = elist.begin(); i != elist.end(); ++i) {
                        Event event = *i;
                        Event newEvent = event.clone();
                        newEvent.setTick(event.tick() + editor->raster());// - part->tick());
                        // Do not do port controller values and clone parts. 
                        operations.push_back(UndoOp(UndoOp::ModifyEvent, newEvent, event, part, false, false));
                        }
                  song->applyOperationGroup(operations);
                  
                  Pos p(editor->rasterVal(pos[0] + editor->rasterStep(pos[0])), true);
                  song->setPos(0, p, true, false, true);
                  }
                  return;
            case CMD_BACKSPACE:
                  if (pos[0] < start() || pos[0] >= end())
                        break;
                  {
                  MidiPart* part = (MidiPart*)curPart;
                  if (part == 0)
                        break;
                  
                  Undo operations;
                  EventList* el = part->events();

                  std::list<Event> elist;
                  for (iEvent e = el->lower_bound(pos[0]); e != el->end(); ++e)
                        elist.push_back((Event)e->second);
                  for (std::list<Event>::iterator i = elist.begin(); i != elist.end(); ++i) {
                        Event event = *i;
                        Event newEvent = event.clone();
                        newEvent.setTick(event.tick() - editor->raster() - part->tick());
                        // Do not do port controller values and clone parts. 
                        operations.push_back(UndoOp(UndoOp::ModifyEvent, newEvent, event, part, false, false));
                        }
                  song->applyOperationGroup(operations);
                  Pos p(editor->rasterVal(pos[0] - editor->rasterStep(pos[0])), true);
                  song->setPos(0, p, true, false, true);
                  }
                  break;
            }
      }

//---------------------------------------------------------
//   pianoPressed
//---------------------------------------------------------

void PianoCanvas::pianoPressed(int pitch, int velocity, bool shift)
      {
      int port      = track()->outPort();
      int channel   = track()->outChannel();
      pitch        += track()->transposition;

      // play note:
      MidiPlayEvent e(0, port, channel, 0x90, pitch, velocity);
      audio->msgPlayMidiEvent(&e);
      
      if (_steprec && pos[0] >= start_tick /* && pos[0] < end_tick [removed by flo93: this is handled in steprec->record] */ && curPart)
				 steprec->record(curPart,pitch,editor->raster(),editor->raster(),velocity,globalKeyState&Qt::ControlModifier,shift);
      }

//---------------------------------------------------------
//   pianoReleased
//---------------------------------------------------------

void PianoCanvas::pianoReleased(int pitch, bool)
      {
      int port    = track()->outPort();
      int channel = track()->outChannel();
      pitch      += track()->transposition;

      // release key:
      MidiPlayEvent e(0, port, channel, 0x90, pitch, 0);
      audio->msgPlayMidiEvent(&e);
      }

//---------------------------------------------------------
//   draw
//---------------------------------------------------------

void PianoCanvas::drawCanvas(QPainter& p, const QRect& rect)
      {
      int x = rect.x();
      int y = rect.y();
      int w = rect.width();
      int h = rect.height();

      // Changed to draw in device coordinate space instead of virtual, transformed space.     Tim. p4.0.30  
      
      //int mx = mapx(x);
      //int my = mapy(y);
      //int mw = mapx(x + w) - mx;
      //int mw = mapx(x + w) - mx - 1;
      //int mh = mapy(y + h) - my;
      //int mh = mapy(y + h) - my - 1;
      
      // p.save();
      // FIXME Can't get horizontal lines quite right yet. Draw in virtual space for now...
      ///p.setWorldMatrixEnabled(false);
      
      //---------------------------------------------------
      //  horizontal lines
      //---------------------------------------------------

      int yy  = ((y-1) / KH) * KH + KH;
      //int yy  = my + KH;
      //int yy  = ((my-1) / KH) * KH + KH;
      //int yoff = -rmapy(yorg) - ypos;
      int key = 75 - (yy / KH);
      
      //printf("PianoCanvas::drawCanvas x:%d y:%d w:%d h:%d mx:%d my:%d mw:%d mh:%d yy:%d key:%d\n", x, y, w, h, mx, my, mw, mh, yy, key);  
      
      for (; yy < y + h; yy += KH) {
      //for (; yy + yoff < my + mh; yy += KH) {
      //for (; yy < my + mh; yy += KH) {
            switch (key % 7) {
                  case 0:
                  case 3:
                        p.setPen(Qt::black);
                        p.drawLine(x, yy, x + w, yy);
                        //p.drawLine(mx, yy + yoff, mx + mw, yy + yoff);
                        //p.drawLine(mx, yy, mx + mw, yy);
                        break;
                  default:
                        p.fillRect(x, yy-3, w, 6, QBrush(QColor(230,230,230)));
                        //p.fillRect(mx, yy-3 + yoff, mw, 6, QBrush(QColor(230,230,230)));
                        //p.fillRect(mx, yy-3, mw, 6, QBrush(QColor(230,230,230)));
                        break;
                  }
            --key;
            }
      //p.restore();
      ///p.setWorldMatrixEnabled(true);
      
      //p.setWorldMatrixEnabled(false);
      
      //---------------------------------------------------
      // vertical lines
      //---------------------------------------------------

      drawTickRaster(p, x, y, w, h, editor->raster());
      
      //p.restore();
      //p.setWorldMatrixEnabled(true);
      }

//---------------------------------------------------------
//   cmd
//    pulldown menu commands
//---------------------------------------------------------

void PianoCanvas::cmd(int cmd)
      {
      switch (cmd) {
            case CMD_SELECT_ALL:     // select all
                  for (iCItem k = items.begin(); k != items.end(); ++k) {
                        if (!k->second->isSelected())
                              selectItem(k->second, true);
                        }
                  break;
            case CMD_SELECT_NONE:     // select none
                  deselectAll();
                  break;
            case CMD_SELECT_INVERT:     // invert selection
                  for (iCItem k = items.begin(); k != items.end(); ++k) {
                        selectItem(k->second, !k->second->isSelected());
                        }
                  break;
            case CMD_SELECT_ILOOP:     // select inside loop
                  for (iCItem k = items.begin(); k != items.end(); ++k) {
                        NEvent* nevent = (NEvent*)(k->second);
                        Part* part     = nevent->part();
                        Event event    = nevent->event();
                        unsigned tick  = event.tick() + part->tick();
                        if (tick < song->lpos() || tick >= song->rpos())
                              selectItem(k->second, false);
                        else
                              selectItem(k->second, true);
                        }
                  break;
            case CMD_SELECT_OLOOP:     // select outside loop
                  for (iCItem k = items.begin(); k != items.end(); ++k) {
                        NEvent* nevent = (NEvent*)(k->second);
                        Part* part     = nevent->part();
                        Event event    = nevent->event();
                        unsigned tick  = event.tick() + part->tick();
                        if (tick < song->lpos() || tick >= song->rpos())
                              selectItem(k->second, true);
                        else
                              selectItem(k->second, false);
                        }
                  break;
            case CMD_SELECT_PREV_PART:     // select previous part
                  {
                    Part* pt = editor->curCanvasPart();
                    Part* newpt = pt;
                    PartList* pl = editor->parts();
                    for(iPart ip = pl->begin(); ip != pl->end(); ++ip)
                      if(ip->second == pt) 
                      {
                        if(ip == pl->begin())
                          ip = pl->end();
                        --ip;
                        newpt = ip->second;
                        break;    
                      }
                    if(newpt != pt)
                      editor->setCurCanvasPart(newpt);
                  }
                  break;
            case CMD_SELECT_NEXT_PART:     // select next part
                  {
                    Part* pt = editor->curCanvasPart();
                    Part* newpt = pt;
                    PartList* pl = editor->parts();
                    for(iPart ip = pl->begin(); ip != pl->end(); ++ip)
                      if(ip->second == pt) 
                      {
                        ++ip;
                        if(ip == pl->end())
                          ip = pl->begin();
                        newpt = ip->second;
                        break;    
                      }
                    if(newpt != pt)
                      editor->setCurCanvasPart(newpt);
                  }
                  break;

            case CMD_FIXED_LEN:
            case CMD_CRESCENDO:
            case CMD_TRANSPOSE:
            case CMD_THIN_OUT:
            case CMD_ERASE_EVENT:
            case CMD_NOTE_SHIFT:
            case CMD_MOVE_CLOCK:
            case CMD_COPY_MEASURE:
            case CMD_ERASE_MEASURE:
            case CMD_DELETE_MEASURE:
            case CMD_CREATE_MEASURE:
                  break;
            default:
//                  printf("unknown ecanvas cmd %d\n", cmd);
                  break;
            }
      updateSelection();
      redraw();
      }

//---------------------------------------------------------
//   midiNote
//---------------------------------------------------------
void PianoCanvas::midiNote(int pitch, int velo)
      {
      if (debugMsg) printf("PianoCanvas::midiNote: pitch=%i, velo=%i\n", pitch, velo);

      if (velo)
        noteHeldDown[pitch]=true;
      else
        noteHeldDown[pitch]=false;

      if (heavyDebugMsg)
      {
        printf("  held down notes are: ");
        for (int i=0;i<128;i++)
          if (noteHeldDown[i])
            printf("%i ",i);
        printf("\n");
      }
      
      if (_midiin && _steprec && curPart
         && !audio->isPlaying() && velo && pos[0] >= start_tick
         /* && pos[0] < end_tick [removed by flo93: this is handled in steprec->record] */
         && !(globalKeyState & Qt::AltModifier)) {
					 steprec->record(curPart,pitch,editor->raster(),editor->raster(),velo,globalKeyState&Qt::ControlModifier,globalKeyState&Qt::ShiftModifier);
         }
      }


//---------------------------------------------------------
//   startDrag
//---------------------------------------------------------

void PianoCanvas::startDrag(CItem* /* item*/, bool copymode)
      {
      QMimeData* md = selected_events_to_mime(partlist_to_set(editor->parts()), 1);
      
      if (md) {
            // "Note that setMimeData() assigns ownership of the QMimeData object to the QDrag object. 
            //  The QDrag must be constructed on the heap with a parent QWidget to ensure that Qt can 
            //  clean up after the drag and drop operation has been completed. "
            QDrag* drag = new QDrag(this);
            drag->setMimeData(md);
            
            if (copymode)
                  drag->exec(Qt::CopyAction);
            else
                  drag->exec(Qt::MoveAction);
            }
      }

//---------------------------------------------------------
//   dragEnterEvent
//---------------------------------------------------------

void PianoCanvas::dragEnterEvent(QDragEnterEvent* event)
      {
      ///event->accept(Q3TextDrag::canDecode(event));
      event->acceptProposedAction();  // TODO CHECK Tim.
      }

//---------------------------------------------------------
//   dragMoveEvent
//---------------------------------------------------------

void PianoCanvas::dragMoveEvent(QDragMoveEvent*)
      {
      //printf("drag move %x\n", this);
      //event->acceptProposedAction();  
      }

//---------------------------------------------------------
//   dragLeaveEvent
//---------------------------------------------------------

void PianoCanvas::dragLeaveEvent(QDragLeaveEvent*)
      {
      //printf("drag leave\n");
      //event->acceptProposedAction();  
      }

//---------------------------------------------------------
//   itemPressed
//---------------------------------------------------------

void PianoCanvas::itemPressed(const CItem* item)
      {
      if (!_playEvents)
            return;

      int port         = track()->outPort();
      int channel      = track()->outChannel();
      NEvent* nevent   = (NEvent*) item;
      Event event      = nevent->event();
      playedPitch      = event.pitch() + track()->transposition;
      int velo         = event.velo();

      // play note:
      MidiPlayEvent e(0, port, channel, 0x90, playedPitch, velo);
      audio->msgPlayMidiEvent(&e);
      }

//---------------------------------------------------------
//   itemReleased
//---------------------------------------------------------

void PianoCanvas::itemReleased(const CItem*, const QPoint&)
      {
      if (!_playEvents)
            return;
      int port    = track()->outPort();
      int channel = track()->outChannel();

      // release note:
      MidiPlayEvent ev(0, port, channel, 0x90, playedPitch, 0);
      audio->msgPlayMidiEvent(&ev);
      playedPitch = -1;
      }

//---------------------------------------------------------
//   itemMoved
//---------------------------------------------------------

void PianoCanvas::itemMoved(const CItem* item, const QPoint& pos)
      {
      int npitch = y2pitch(pos.y());
      if ((playedPitch != -1) && (playedPitch != npitch)) {
            int port         = track()->outPort();
            int channel      = track()->outChannel();
            NEvent* nevent   = (NEvent*) item;
            Event event      = nevent->event();

            // release note:
            MidiPlayEvent ev1(0, port, channel, 0x90, playedPitch, 0);
            audio->msgPlayMidiEvent(&ev1);
            // play note:
            MidiPlayEvent e2(0, port, channel, 0x90, npitch + track()->transposition, event.velo());
            audio->msgPlayMidiEvent(&e2);
            playedPitch = npitch + track()->transposition;
            }
      }

//---------------------------------------------------------
//   curPartChanged
//---------------------------------------------------------

void PianoCanvas::curPartChanged()
      {
      editor->setWindowTitle(getCaption());
      }

//---------------------------------------------------------
//   modifySelected
//---------------------------------------------------------

void PianoCanvas::modifySelected(NoteInfo::ValType type, int delta)
      {
      audio->msgIdle(true);
      song->startUndo();
      for (iCItem i = items.begin(); i != items.end(); ++i) {
            if (!(i->second->isSelected()))
                  continue;
            NEvent* e   = (NEvent*)(i->second);
            Event event = e->event();
            if (event.type() != Note)
                  continue;

            MidiPart* part = (MidiPart*)(e->part());
            Event newEvent = event.clone();

            switch (type) {
                  case NoteInfo::VAL_TIME:
                        {
                        int newTime = event.tick() + delta;
                        if (newTime < 0)
                           newTime = 0;
                        newEvent.setTick(newTime);
                        }
                        break;
                  case NoteInfo::VAL_LEN:
                        {
                        int len = event.lenTick() + delta;
                        if (len < 1)
                              len = 1;
                        newEvent.setLenTick(len);
                        }
                        break;
                  case NoteInfo::VAL_VELON:
                        {
                        int velo = event.velo() + delta;
                        if (velo > 127)
                              velo = 127;
                        else if (velo < 0)
                              velo = 0;
                        newEvent.setVelo(velo);
                        }
                        break;
                  case NoteInfo::VAL_VELOFF:
                        {
                        int velo = event.veloOff() + delta;
                        if (velo > 127)
                              velo = 127;
                        else if (velo < 0)
                              velo = 0;
                        newEvent.setVeloOff(velo);
                        }
                        break;
                  case NoteInfo::VAL_PITCH:
                        {
                        int pitch = event.pitch() + delta;
                        if (pitch > 127)
                              pitch = 127;
                        else if (pitch < 0)
                              pitch = 0;
                        newEvent.setPitch(pitch);
                        }
                        break;
                  }
            song->changeEvent(event, newEvent, part);
            // Indicate do not do port controller values and clone parts. 
            //song->addUndo(UndoOp(UndoOp::ModifyEvent, newEvent, event, part));
            song->addUndo(UndoOp(UndoOp::ModifyEvent, newEvent, event, part, false, false));
            }
      song->endUndo(SC_EVENT_MODIFIED);
      audio->msgIdle(false);
      }

//---------------------------------------------------------
//   resizeEvent
//---------------------------------------------------------

void PianoCanvas::resizeEvent(QResizeEvent* ev)
      {
      if (ev->size().width() != ev->oldSize().width())
            emit newWidth(ev->size().width());
      EventCanvas::resizeEvent(ev);
      }
