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

#ifndef __MUSE_DEBUG_H__
#define __MUSE_DEBUG_H__

#include "globals.h"

#ifdef DEBUG_1
#define M_REPORT(string) printf("%s:%d:%s: " string "\n", __FILE__, __LINE__, __PRETTY_FUNCTION__);
#define M_REPORTARG(format, args...) printf("%s:%d:%s: " format "\n", __FILE__, __LINE__, __PRETTY_FUNCTION__, ##args);
#define M_DEBUG(string) if (debugMsg) fprintf(stderr, "%s:%d:%s: " string "\n", __FILE__, __LINE__, __PRETTY_FUNCTION__);
#define M_DEBUGARG(format, args...) if (debugMsg) fprintf(stderr, "%s:%d:%s: " format "\n", __FILE__, __LINE__, __PRETTY_FUNCTION__, ##args);
#define M_ERROR(string) fprintf(stderr, "%s:%d:%s: " string "\n", __FILE__, __LINE__, __PRETTY_FUNCTION__);
#define M_ERRORARG(format, args...) fprintf(stderr, "%s:%d:%s: " format "\n", __FILE__, __LINE__, __PRETTY_FUNCTION__, ##args);
#else
#define M_REPORT(string)
#define M_REPORTARG(format, args...)
#define M_DEBUG(string)
#define M_DEBUGARG(format, args...)
#define M_ERROR(string)
#define M_ERRORARG(format, args...)
#endif

#define DEBUG_ARGS __FILE__ << ":" << __LINE__ << ":" << __PRETTY_FUNCTION__ << ": "

#endif /* __MUSE_DEBUG_H__ */
