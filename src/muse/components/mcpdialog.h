//=========================================================
//  MusE
//  Linux Music Editor
//
//  mcpdialog.h
//  The settings and the activity log of the MCP server.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __MCPDIALOG_H__
#define __MCPDIALOG_H__

#include <QDialog>

#include "mcpserver.h"

class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QSpinBox;

namespace MusEGui {

class McpDialog : public QDialog
{
      Q_OBJECT

      McpServer* _server;
      McpSettings _settings;
      QCheckBox* _enabled;
      QSpinBox* _port;
      QLabel* _status;
      QLineEdit* _url;
      QLineEdit* _token;
      QCheckBox* _tierBoxes[5];
      QPlainTextEdit* _log;

      // Saves the settings and starts or stops the server to match them.
      void apply();
      void updateStatus();
      QString command() const;

   public:
      McpDialog(McpServer* server, QWidget* parent = nullptr);
};

} // namespace MusEGui

#endif
