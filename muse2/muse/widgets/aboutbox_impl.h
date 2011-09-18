//=========================================================
//  MusE
//  Linux Music Editor
//  $Id: ./muse/widgets/aboutbox_impl.h $
//
//  Copyright (C) 1999-2011 by Werner Schweer and others
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
#ifndef ABOUTBOXIMPL_H
#define ABOUTBOXIMPL_H

#include "ui_aboutbox.h"

namespace MusEWidget {

class AboutBoxImpl : public QDialog, public Ui::AboutBox
{
    Q_OBJECT

public:
    AboutBoxImpl();


};

}

#endif // ABOUTBOXIMPL_H