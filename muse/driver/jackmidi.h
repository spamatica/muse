//=========================================================
//  MusE
//  Linux Music Editor
//  $Id: jackmidi.h,v 1.1.1.1 2010/01/27 09:06:43 terminator356 Exp $
//  (C) Copyright 1999-2010 Werner Schweer (ws@seh.de)
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

#ifndef __JACKMIDI_H__
#define __JACKMIDI_H__

//#include <config.h>

#include <map>

#include <jack/jack.h>
#include <jack/midiport.h>

#include "mididev.h"
#include "route.h"

class QString;
//class MidiFifo;
class MidiRecordEvent;
class MidiPlayEvent;
//class RouteList;
class Xml;

// Turn on to show multiple devices, work in progress, 
//  not working fully yet, can't seem to connect...
//#define JACK_MIDI_SHOW_MULTIPLE_DEVICES

// It appears one client port per remote port will be necessary.
// Jack doesn't seem to like manipulation of non-local ports buffers.
//#define JACK_MIDI_USE_MULTIPLE_CLIENT_PORTS

/* jack-midi channels */
//#define JACK_MIDI_CHANNELS 32

/* jack-midi buffer size */
//#define JACK_MIDI_BUFFER_SIZE 32

/*
typedef struct {
  int  give;
  int  take;
  // 32 parallel midi events, where each event contains three
  //  midi-bytes and one busy-byte 
  char buffer[4 * JACK_MIDI_BUFFER_SIZE];
} muse_jack_midi_buffer;
*/

/*
struct JackMidiPort 
{
  jack_port_t* _jackPort;
  QString _name;
  int _flags; // 1 = writable, 2 = readable - do not mix
  JackMidiPort(jack_port_t* jp, const QString& s, int f) 
  {
    _jackPort = jp;
    _name = QString(s);
    _flags = f;
  }
};

typedef std::map<jack_port_t*, JackMidiPort, std::less<jack_port_t*> >::iterator iJackMidiPort;
typedef std::map<jack_port_t*, JackMidiPort, std::less<jack_port_t*> >::const_iterator ciJackMidiPort;

class JackMidiPortList : public std::map<jack_port_t*, JackMidiPort, std::less<jack_port_t*> > 
{
   private:   
      static int _nextOutIdNum;
      static int _nextInIdNum;
      
   public:
      JackMidiPortList();
      ~JackMidiPortList();
      iJackMidiPort createClientPort(int flags);
      bool removeClientPort(jack_port_t* port);
};

extern JackMidiPortList jackMidiClientPorts;
*/

//---------------------------------------------------------
//   MidiJackDevice
//---------------------------------------------------------

class MidiJackDevice : public MidiDevice {
   public:
      //int adr;

   private:
      // fifo for midi events sent from gui
      // direct to midi port:
      //MidiFifo eventFifo;  // Moved into MidiDevice p4.0.15

      //static int _nextOutIdNum;
      //static int _nextInIdNum;
      
      //jack_port_t* _client_jackport;
      // p3.3.55
      jack_port_t* _in_client_jackport;
      jack_port_t* _out_client_jackport;
      
      //RouteList _routes;
      
      virtual QString open();
      virtual void close();
      //bool putEvent(int*);
      
      bool processEvent(const MidiPlayEvent&);
      // Port is not midi port, it is the port(s) created for MusE.
      bool queueEvent(const MidiPlayEvent&);
      
      virtual bool putMidiEvent(const MidiPlayEvent&);
      //bool sendEvent(const MidiPlayEvent&);
      
      void eventReceived(jack_midi_event_t*);

   public:
      //MidiJackDevice() {}  // p3.3.55  Removed.
      //MidiJackDevice(const int&, const QString& name);
      
      //MidiJackDevice(jack_port_t* jack_port, const QString& name);
      //MidiJackDevice(jack_port_t* in_jack_port, jack_port_t* out_jack_port, const QString& name); // p3.3.55 In or out port can be null.
      MidiJackDevice(const QString& name); 
      
      //static MidiDevice* createJackMidiDevice(QString /*name*/, int /*rwflags*/); // 1:Writable 2: Readable. Do not mix.
      static MidiDevice* createJackMidiDevice(QString name = "", int rwflags = 3); // p3.3.55 1:Writable 2: Readable 3: Writable + Readable
      
      virtual inline int deviceType() const { return JACK_MIDI; } 
      
      virtual void setName(const QString&);
      
      virtual void processMidi();
      virtual ~MidiJackDevice(); 
      //virtual int selectRfd();
      //virtual int selectWfd();
      //virtual void processInput();
      
      virtual void recordEvent(MidiRecordEvent&);
      
      virtual bool putEvent(const MidiPlayEvent&);
      virtual void collectMidiEvents();
      
      //virtual jack_port_t* jackPort() { return _jackport; }
      //virtual jack_port_t* clientJackPort() { return _client_jackport; }
      
      //virtual void* clientPort() { return (void*)_client_jackport; }
      // p3.3.55
      virtual void* inClientPort()  { return (void*)  _in_client_jackport; }
      virtual void* outClientPort() { return (void*) _out_client_jackport; }
      
      //RouteList* routes()   { return &_routes; }
      //bool noRoute() const   { return _routes.empty();  }
      virtual void writeRouting(int, Xml&) const;
      };

extern bool initMidiJack();
//extern int jackSelectRfd();
//extern int jackSelectWfd();
//extern void jackProcessMidiInput();
//extern void jackScanMidiPorts();

#endif

