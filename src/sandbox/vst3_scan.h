//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3_scan.h
//  Scanning of VST3 bundles, for the plugin scan sandbox program.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __MUSE_VST3_SCAN_H__
#define __MUSE_VST3_SCAN_H__

#include "plugin_scan.h"

namespace MusECore {
class Xml;
}

namespace MusEPlugin {

// Loads the VST3 bundle, creates and sets up every audio effect class in it, and writes one
//  plugin scan entry per class. Returns false if no class could be scanned.
// Plugin code runs in here, so this must only be called from the sandbox program.
bool writeVst3Info(const char* filename, bool doPorts, int level, MusECore::Xml& xml);

} // namespace MusEPlugin

#endif
