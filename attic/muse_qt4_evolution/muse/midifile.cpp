//=============================================================================
//  MusE
//  Linux Music Editor
//  $Id:$
//
//  Copyright (C) 2002-2006 by Werner Schweer and others
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License version 2.
//
//  This program is distributed in the hope that it will be useful,
//  but WITHOUT ANY WARRANTY; without even the implied warranty of
//  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//  GNU General Public License for more details.
//
//  You should have received a copy of the GNU General Public License
//  along with this program; if not, write to the Free Software
//  Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
//=============================================================================

#include "song.h"
#include "midi.h"
#include "midifile.h"
#include "midiedit/drummap.h"
#include "event.h"
#include "globals.h"
#include "midictrl.h"
#include "midictrl.h"
#include "midievent.h"
#include "gconfig.h"

const char* errString[] = {
      "no Error",
      "unexpected EOF",
      "read Error",
      "write Error",
      "bad midifile: 'MTrk' expected",
      "bad midifile: 'MThd' expected",
      "bad midi fileformat",
      };

enum ERROR {
      MF_NO_ERROR,
      MF_EOF,
      MF_READ,
      MF_WRITE,
      MF_MTRK,
      MF_MTHD,
      MF_FORMAT,
      };

//---------------------------------------------------------
//   error
//---------------------------------------------------------

QString MidiFile::error()
      {
      return QString(errString[_error]);
      }

//---------------------------------------------------------
//   MidiFile
//---------------------------------------------------------

MidiFile::MidiFile()
      {
      fp        = 0;
      curPos    = 0;
      _error    = MF_NO_ERROR;
      _tracks   = new MidiFileTrackList;
      _midiType = MT_GENERIC;
      }

MidiFile::~MidiFile()
      {
      delete _tracks;
      }

//---------------------------------------------------------
//   read
//    return true on error
//---------------------------------------------------------

bool MidiFile::read(void* p, size_t len)
      {
      for (;;) {
            curPos += len;
            qint64 rv = fp->read((char*)p, len);
            if (rv == len)
                  return false;
            if (fp->atEnd()) {
                  _error = MF_EOF;
                  return true;
                  }
            _error = MF_READ;
            return true;
            }
      return false;
      }

//---------------------------------------------------------
//   write
//    return true on error
//---------------------------------------------------------

bool MidiFile::write(const void* p, size_t len)
      {
      qint64 rv = fp->write((char*)p, len);
      if (rv == len)
            return false;
      _error = MF_WRITE;
      return true;
      }

//---------------------------------------------------------
//   writeShort
//    return true on error
//---------------------------------------------------------

bool MidiFile::writeShort(int i)
      {
      short format = BE_SHORT(i);
      return write(&format, 2);
      }

//---------------------------------------------------------
//   writeLong
//    return true on error
//---------------------------------------------------------

bool MidiFile::writeLong(int i)
      {
      int format = BE_LONG(i);
      return write(&format, 4);
      }

//---------------------------------------------------------
//   readShort
//---------------------------------------------------------

int MidiFile::readShort()
      {
      short format;
      read(&format, 2);
      return BE_SHORT(format);
      }

//---------------------------------------------------------
//   readLong
//   writeLong
//---------------------------------------------------------

int MidiFile::readLong()
      {
      int format;
      read(&format, 4);
      return BE_LONG(format);
      }

/*---------------------------------------------------------
 *    skip
 *    This is meant for skipping a few bytes in a
 *    file or fifo.
 *---------------------------------------------------------*/

bool MidiFile::skip(size_t len)
      {
      char tmp[len];
      return read(tmp, len);
      }

/*---------------------------------------------------------
 *    getvl
 *    Read variable-length number (7 bits per byte, MSB first)
 *---------------------------------------------------------*/

int MidiFile::getvl()
      {
      int l = 0;
      for (int i = 0; i < 16; i++) {
            uchar c;
            if (read(&c, 1))
                  return -1;
            l += (c & 0x7f);
            if (!(c & 0x80))
                  return l;
            l <<= 7;
            }
      return -1;
      }

/*---------------------------------------------------------
 *    putvl
 *    Write variable-length number (7 bits per byte, MSB first)
 *---------------------------------------------------------*/

void MidiFile::putvl(unsigned val)
      {
      unsigned long buf = val & 0x7f;
      while ((val >>= 7) > 0) {
            buf <<= 8;
            buf |= 0x80;
            buf += (val & 0x7f);
            }
      for (;;) {
            put(buf);
            if (buf & 0x80)
                  buf >>= 8;
            else
                  break;
            }
      }

//---------------------------------------------------------
//   readTrack
//    return true on error
//---------------------------------------------------------

bool MidiFile::readTrack(MidiFileTrack* t)
      {
      MidiEventList* el = &(t->events);
      char tmp[4];
      if (read(tmp, 4))
            return true;
      if (memcmp(tmp, "MTrk", 4)) {
            _error = MF_MTRK;
            return true;
            }
      int len    = readLong();       // len
      int endPos = curPos + len;
      status     = -1;
      sstatus    = -1;     // running status, not reset scanning meta or sysex
      click      = 0;

      int port    = 0;
      int channel = 0;

      for (;;) {
            MidiEvent event;
            lastport    = -1;
            lastchannel = -1;

            int rv = readEvent(&event, t);
            if (lastport != -1)
                  port = lastport;
            if (lastchannel != -1) {
                  channel = lastchannel;
                  if (channel >= MIDI_CHANNELS) {
                        printf("channel %d >= %d, reset to 0\n", port, MIDI_CHANNELS);
                        channel = 0;
                        }
                  }
            if (rv == 0)
                  break;
            else if (rv == -1)
                  continue;
            else if (rv == -2)          // error
                  return true;

//TODO3            event.setPort(port);
            if (event.type() == ME_SYSEX || event.type() == ME_META)
                  event.setChannel(channel);
            else
                  channel = event.channel();
            el->insert(event);
            }
      int end = curPos;
      if (end != endPos) {
            printf("MidiFile::readTrack(): TRACKLEN does not fit %d+%d != %d, %d too much\n",
               endPos-len, len, end, endPos-end);
            if (end < endPos)
                  skip(endPos - end);
            }
      return false;
      }

//---------------------------------------------------------
//   readEvent
//    returns:
//          0     End of track
//          -1    Event filtered
//          -2    Error
//---------------------------------------------------------

int MidiFile::readEvent(MidiEvent* event, MidiFileTrack* t)
      {
      uchar me, type, a, b;

      int nclick = getvl();
      if (nclick == -1) {
            printf("readEvent: error 1\n");
            return 0;
            }
      click += nclick;
      for (;;) {
            if (read(&me, 1)) {
                  printf("readEvent: error 2\n");
                  return 0;
                  }
            if (me >= 0xf8 && me <= 0xfe)
                  printf("Midi: Real Time Message 0x%02x??\n", me & 0xff);
            else
                  break;
            }

      event->setTime(click);
      int len;
      unsigned char* buffer;

      if ((me & 0xf0) == 0xf0) {
            if (me == 0xf0 || me == 0xf7) {
                  //
                  //    SYSEX
                  //
                  status = -1;                  // no running status
                  len = getvl();
                  if (len == -1) {
                        printf("readEvent: error 3\n");
                        return -2;
                        }
                  buffer = new unsigned char[len];
                  if (read(buffer, len)) {
                        printf("readEvent: error 4\n");
                        delete[] buffer;
                        return -2;
                        }
                  if (buffer[len-1] != 0xf7) {
                        printf("SYSEX endet nicht mit 0xf7!\n");
                        // Forstsetzung folgt?
                        }
                  else
                        --len;      // don't count 0xf7
                  event->setType(ME_SYSEX);
                  event->setData(buffer, len);

                  if ((len == (signed)gmOnMsgLen) && memcmp(buffer, gmOnMsg, gmOnMsgLen) == 0)
                        _midiType = MT_GM;
                  else if ((len == (signed)gsOnMsgLen) && memcmp(buffer, gsOnMsg, gsOnMsgLen) == 0)
                        _midiType = MT_GS;

                  if (buffer[0] == 0x43) {    // Yamaha
                        _midiType = MT_XG;
                        int type   = buffer[1] & 0xf0;
                        switch (type) {
                              case 0x00:  // bulk dump
                                    buffer[1] = 0;
                                    break;
                              case 0x10:
                                    if (buffer[1] != 0x10) {
                                          buffer[1] = 0x10;    // fix to Device 1
                                          }
                                    if (len == 7 && buffer[2] == 0x4c && buffer[3] == 0x08 && buffer[5] == 7) {
                                          // part mode
                                          // 0 - normal
                                          // 1 - DRUM
                                          // 2 - DRUM 1
                                          // 3 - DRUM 2
                                          // 4 - DRUM 3
                                          // 5 - DRUM 4
                                          printf("xg set part mode channel %d to %d\n", buffer[4]+1, buffer[6]);
                                          if (buffer[6] != 0)
                                                t->isDrumTrack = true;
                                          }
                                    break;
                              case 0x20:
                                    printf("YAMAHA DUMP REQUEST\n");
                                    return -1;
                              case 0x30:
                                    printf("YAMAHA PARAMETER REQUEST\n");
                                    return -1;
                              default:
                                    printf("YAMAHA unknown SYSEX: data[2]=%02x\n", buffer[1]);
                                    return -1;
                              }
                        }
                  else if (buffer[0] == 0x43) {
                        if (_midiType != MT_XG)
                              _midiType = MT_GS;
                        }

                  return 3;
                  }
            if (me == 0xff) {
                  //
                  //    META
                  //
                  status = -1;                  // no running status
                  if (read(&type, 1)) {         // read type
                        printf("readEvent: error 5\n");
                        return -2;
                        }
                  len = getvl();                // read len
                  if (len == -1) {
                        printf("readEvent: error 6\n");
                        return -2;
                        }
                  buffer = new unsigned char[len+1];
                  *buffer = 0;
                  if (len) {
                        if (read(buffer, len)) {
                              printf("readEvent: error 7\n");
                              delete[] buffer;
                              return -2;
                              }
                        buffer[len] = 0;
                        }
                  event->setType(ME_META);
                  event->setData(buffer, len+1);
                  event->setA(type);
                  buffer[len] = 0;
                  switch(type) {
                        case 0x21:        // switch port
                              lastport = buffer[0];
                              return 3;
                        case 0x20:        // switch channel
                              lastchannel = buffer[0];
                              return 3;
                        case 0x2f:        // End of Track
                              delete[] buffer;
                              return 0;
                        default:
                              return 3;
                        }
                  }
            else {
                  printf("Midi: unknown Message 0x%02x\n", me & 0xff);
                  return -1;
                  }
            }

      if (me & 0x80) {                     // status byte
            status   = me;
            sstatus  = status;
            if (read(&a, 1)) {
                  printf("readEvent: error 9\n");
                  return -2;
                  }
            a &= 0x7F;
            }
      else {
            if (status == -1) {
                  // Meta events and sysex events cancel running status.
                  // There are some midi files which do not send
                  // status again after this events. Silently assume
                  // old running status.

                  if (debugMsg || sstatus == -1)
                        printf("readEvent: no running status, read 0x%02x, old status 0x%02x\n", me, sstatus);
                  if (sstatus == -1)
                        return -1;
                  status = sstatus;
                  }
            a = me;
            }
      b = 0;
      switch (status & 0xf0) {
            case ME_NOTEOFF:
            case ME_NOTEON:
            case ME_POLYAFTER:
            case ME_CONTROLLER:
            case ME_PITCHBEND:
                  if (read(&b, 1)) {
                        printf("readEvent: error 15\n");
                        return -2;
                        }
                  event->setB(b & 0x80 ? 0 : b);
                  break;
            case ME_PROGRAM:
            case ME_AFTERTOUCH:
                  break;
            default:          // f1 f2 f3 f4 f5 f6 f7 f8 f9
                  printf("BAD STATUS 0x%02x, me 0x%02x\n", status, me);
                  return -2;
            }
      event->setA(a & 0x7f);
      event->setType(status & 0xf0);
      event->setChannel(status & 0xf);
      if ((a & 0x80) || (b & 0x80)) {
            printf("8'tes Bit in Daten(%02x %02x): tick %d read 0x%02x  status:0x%02x\n",
               a & 0xff, b & 0xff, click, me, status);
            printf("readEvent: error 16\n");
            if (b & 0x80) {
                  // Try to fix: interpret as channel byte
                  status   = b & 0xf0;
                  sstatus  = status;
                  return 3;
                  }
            return -1;
            }
      if (event->type() == ME_PITCHBEND) {
            int val = (event->dataB() << 7) + event->dataA();
            val -= 8192;
            event->setA(val);
            }
      return 3;
      }

//---------------------------------------------------------
//   writeTrack
//---------------------------------------------------------

bool MidiFile::writeTrack(const MidiFileTrack* t)
      {
      const MidiEventList* events = &(t->events);
      write("MTrk", 4);
      int lenpos = fp->pos();
      writeLong(0);                 // dummy len

      status = -1;
      int tick = 0;
      for (iMidiEvent i = events->begin(); i != events->end(); ++i) {
            int ntick = i->time();
            if (ntick < tick) {
                  printf("MidiFile::writeTrack: ntick %d < tick %d\n", ntick, tick);
                  ntick = tick;
                  }
            putvl(((ntick - tick) * config.midiDivision + config.division/2)/config.division);
            tick = ntick;
            writeEvent(&(*i));
            }

      //---------------------------------------------------
      //    write "End Of Track" Meta
      //    write Track Len
      //

      putvl(0);
      put(0xff);        // Meta
      put(0x2f);        // EOT
      putvl(0);         // len 0
      int endpos = fp->pos();
      fp->seek(lenpos);
      writeLong(endpos-lenpos-4);   // tracklen
      fp->seek(endpos);
      return false;
      }

//---------------------------------------------------------
//   writeEvent
//---------------------------------------------------------

void MidiFile::writeEvent(const MidiEvent* event)
      {
      int c     = event->channel();
      int nstat = event->type();

      // we dont save meta data into smf type 0 files:

      if (config.smfFormat == 0 && nstat == ME_META)
            return;

      nstat |= c;
      //
      //  running status; except for Sysex- and Meta Events
      //
      if (((nstat & 0xf0) != 0xf0) && (nstat != status)) {
            status = nstat;
            put(nstat);
            }
      switch (event->type()) {
            case ME_NOTEOFF:
            case ME_NOTEON:
            case ME_POLYAFTER:
            case ME_CONTROLLER:
            case ME_PITCHBEND:
                  put(event->dataA());
                  put(event->dataB());
                  break;
            case ME_PROGRAM:        // Program Change
            case ME_AFTERTOUCH:     // Channel Aftertouch
                  put(event->dataA());
                  break;
            case ME_SYSEX:
                  put(0xf0);
                  putvl(event->len() + 1);  // including 0xf7
                  write(event->data(), event->len());
                  put(0xf7);
                  status = -1;      // invalidate running status
                  break;
            case ME_META:
                  put(0xff);
                  put(event->dataA());
                  putvl(event->len());
                  write(event->data(), event->len());
                  status = -1;
                  break;
            }
      }

//---------------------------------------------------------
//   write
//    returns true on error
//---------------------------------------------------------

bool MidiFile::write(QFile* _fp)
      {
      fp = _fp;
      write("MThd", 4);
      writeLong(6);                 // header len
      writeShort(format);
      if (format == 0) {
            // ?? writeShort(1);
            MidiFileTrack dst;
            for (iMidiFileTrack i = _tracks->begin(); i != _tracks->end(); ++i) {
                  MidiEventList* sl = &((*i)->events);
                  for (iMidiEvent ie = sl->begin(); ie != sl->end(); ++ie)
                        dst.events.insert(*ie);
                  }
            writeShort(1);
            writeShort(_division);
            writeTrack(&dst);
            }
      else {
            writeShort(_tracks->size());
            writeShort(_division);
            for (ciMidiFileTrack i = _tracks->begin(); i != _tracks->end(); ++i)
                  writeTrack(*i);
            }
      return false; // (ferror(fp) != 0);
      }

//---------------------------------------------------------
//   readMidi
//    returns true on error
//---------------------------------------------------------

bool MidiFile::read(QFile* _fp)
      {
      fp = _fp;
      _error = MF_NO_ERROR;
      int i;
      char tmp[4];

      if (read(tmp, 4))
            return true;
      int len = readLong();
      if (memcmp(tmp, "MThd", 4) || len < 6) {
            _error = MF_MTHD;
            return true;
            }
      format   = readShort();
      ntracks  = readShort();
      _division = readShort();

      if (_division < 0)
            _division = (-(_division/256)) * (_division & 0xff);
      if (len > 6)
            skip(len-6); // skip excess bytes

      switch (format) {
            case 0:
                  {
                  MidiFileTrack* t = new MidiFileTrack;
                  _tracks->push_back(t);
                  if (readTrack(t))
                        return true;
                  }
                  break;
            case 1:
                  for (i = 0; i < ntracks; i++) {
                        MidiFileTrack* t = new MidiFileTrack;
                        _tracks->push_back(t);
                        if (readTrack(t))
                              return true;
                        }
                  break;
            default:
                  _error = MF_FORMAT;
                  return true;
            }
      return false;
      }
