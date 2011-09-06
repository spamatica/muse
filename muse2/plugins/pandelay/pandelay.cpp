//=========================================================
//  PanDelay for MusE
//  
//  (C) Copyright 2006 Nil Geisweiller
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

#include "ladspapandelay.h"
#include <stdio.h>

//---------------------------------------------------------
//   instantiate pandelay
//    Construct a new plugin instance.
//---------------------------------------------------------

LADSPA_Handle instantiate(const LADSPA_Descriptor* /*Descriptor*/,
			  unsigned long samplerate)
{
  return new LADSPAPanDelay(samplerate);
}

//---------------------------------------------------------
//   connect PortTo pandelay
//    Connect a port to a data location.
//---------------------------------------------------------

void connect(LADSPA_Handle Instance, unsigned long port,
	     LADSPA_Data* data)
{
  ((LADSPAPanDelay*)Instance)->port[port] = data;
}

//---------------------------------------------------------
//   activate
//---------------------------------------------------------

void activate(LADSPA_Handle instance)
{
  ((LADSPAPanDelay*)instance)->activate();
}

//---------------------------------------------------------
//   deactivate
//---------------------------------------------------------

void deactivate(LADSPA_Handle /*Instance*/)
{
}

//---------------------------------------------------------
//   run pandelay
//---------------------------------------------------------

void run(LADSPA_Handle Instance, unsigned long n)
{
  ((LADSPAPanDelay*)Instance)->processReplace(n);
}

//---------------------------------------------------------
//   runAdding pandelay
//    *ADD* the output to the output buffer.
//---------------------------------------------------------

void runAdding(LADSPA_Handle Instance, unsigned long n)
{
  ((LADSPAPanDelay*)Instance)->processMix(n);
}

//---------------------------------------------------------
//   set pandelay RunAddingGain
//---------------------------------------------------------

void setGain(LADSPA_Handle /*Instance*/, LADSPA_Data /*Gain*/)
{
  printf("TEST setGain\n");
  //      ((LADSPAPanDelay*)Instance)->m_fRunAddingGain = Gain;
}

//---------------------------------------------------------
//   cleanup pandelay
//---------------------------------------------------------

void cleanup(LADSPA_Handle Instance)
{
  delete (LADSPAPanDelay*)Instance;
}

static const char* portNames[] = {
  "Input (Left)",
  "Input (Right)",
  "Output (Left)",
  "Output (Right)",
  "BMP",
  "Beat Ratio",
  "Feedback",
  "Pan LFO Freq",
  "Pan LFO Depth",
  "Dry/Wet"
};

LADSPA_PortDescriptor portDescriptors[] = {
  LADSPA_PORT_INPUT  | LADSPA_PORT_AUDIO,
  LADSPA_PORT_INPUT  | LADSPA_PORT_AUDIO,
  LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
  LADSPA_PORT_OUTPUT | LADSPA_PORT_AUDIO,
  LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
  LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
  LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
  LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
  LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL,
  LADSPA_PORT_INPUT  | LADSPA_PORT_CONTROL
};

LADSPA_PortRangeHint portRangeHints[] = {
  { 0, 0.0, 0.0 },
  { 0, 0.0, 0.0 },
  { 0, 0.0, 0.0 },
  { 0, 0.0, 0.0 },
  { LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_DEFAULT_100, MINBPM, MAXBPM },
  { LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_DEFAULT_1, MINBEATRATIO, MAXBEATRATIO },
  { LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_DEFAULT_HIGH, -1.0, 1.0 },
  { LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_LOGARITHMIC | LADSPA_HINT_DEFAULT_HIGH, MINFREQ, MAXFREQ },
  { LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_DEFAULT_HIGH,  0.0, 1.0 },
  { LADSPA_HINT_BOUNDED_ABOVE | LADSPA_HINT_BOUNDED_BELOW | LADSPA_HINT_DEFAULT_MIDDLE, 0.0, 1.0 }
};

LADSPA_Descriptor descriptor = {
  1052,
  "pandelay",
  LADSPA_PROPERTY_HARD_RT_CAPABLE,
  "PanDelay",
  "Nil Geisweiller",
  "GPL",
  NBRPARAM + 4,
  portDescriptors,
  portNames,
  portRangeHints,
  0,                // impl. data
  instantiate,
  connect,
  activate,
  run,
  runAdding,
  setGain,
  deactivate,
  cleanup
};

//---------------------------------------------------------
//   _init
//    called automatically when the plugin library is first
//    loaded.
//---------------------------------------------------------
void _init() {
}

//---------------------------------------------------------
//   _fini
//    called automatically when the library is unloaded.
//---------------------------------------------------------
void _fini() {
}

//---------------------------------------------------------
//   ladspa_descriptor
//    Return a descriptor of the requested plugin type.
//---------------------------------------------------------
const LADSPA_Descriptor* ladspa_descriptor(unsigned long i) {
  return (i == 0) ? &descriptor : 0;
}

