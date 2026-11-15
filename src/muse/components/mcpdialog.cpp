//=========================================================
//  MusE
//  Linux Music Editor
//
//  mcpdialog.cpp
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "mcpdialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

namespace MusEGui {

static const struct { unsigned bit; const char* name; const char* tip; bool available; } tierInfo[] = {
  { McpTierRead,      QT_TRANSLATE_NOOP("McpDialog", "Read"),
    QT_TRANSLATE_NOOP("McpDialog", "Look at the song, the mixer and the levels. Always on."), true },
  { McpTierControl,   QT_TRANSLATE_NOOP("McpDialog", "Control"),
    QT_TRANSLATE_NOOP("McpDialog", "Mute, solo, volume, pan, plugin parameters and transport."), true },
  { McpTierRecord,    QT_TRANSLATE_NOOP("McpDialog", "Record"),
    QT_TRANSLATE_NOOP("McpDialog", "Arm tracks and record. Creates files."), true },
  { McpTierEdit,      QT_TRANSLATE_NOOP("McpDialog", "Edit"),
    QT_TRANSLATE_NOOP("McpDialog", "Add and move tracks and parts, change routing, undo and redo."), true },
  { McpTierDangerous, QT_TRANSLATE_NOOP("McpDialog", "Dangerous"),
    QT_TRANSLATE_NOOP("McpDialog", "Delete things, save the song."), true }
};

McpDialog::McpDialog(McpServer* server, QWidget* parent) : QDialog(parent), _server(server)
{
  setWindowTitle(tr("MCP Server"));
  setAttribute(Qt::WA_DeleteOnClose);
  resize(620, 640);
  _settings = server->isRunning() ? server->settings() : McpSettings::load();

  QVBoxLayout* lay = new QVBoxLayout(this);
  QLabel* intro = new QLabel(
    tr("The MCP server lets an AI assistant look at your song and mixer. It only accepts connections from this computer, "
       "and only with the token below. Changes it makes can be undone in MusE."), this);
  intro->setWordWrap(true);
  lay->addWidget(intro);

  QGroupBox* box = new QGroupBox(tr("Server"), this);
  QGridLayout* g = new QGridLayout(box);
  _enabled = new QCheckBox(tr("Enable the MCP server"), box);
  _enabled->setChecked(_settings.enabled);
  g->addWidget(_enabled, 0, 0, 1, 3);

  g->addWidget(new QLabel(tr("Port:"), box), 1, 0);
  _port = new QSpinBox(box);
  _port->setRange(1024, 65535);
  _port->setValue(_settings.port);
  g->addWidget(_port, 1, 1);
  _status = new QLabel(box);
  g->addWidget(_status, 1, 2);

  g->addWidget(new QLabel(tr("Address:"), box), 2, 0);
  _url = new QLineEdit(box);
  _url->setReadOnly(true);
  g->addWidget(_url, 2, 1, 1, 2);

  g->addWidget(new QLabel(tr("Token:"), box), 3, 0);
  _token = new QLineEdit(_settings.token, box);
  _token->setReadOnly(true);
  g->addWidget(_token, 3, 1, 1, 2);

  QHBoxLayout* buttons = new QHBoxLayout();
  QPushButton* copyToken = new QPushButton(tr("Copy token"), box);
  QPushButton* copyCmd = new QPushButton(tr("Copy Claude Code command"), box);
  QPushButton* regen = new QPushButton(tr("New token..."), box);
  buttons->addWidget(copyToken);
  buttons->addWidget(copyCmd);
  buttons->addWidget(regen);
  buttons->addStretch();
  g->addLayout(buttons, 4, 0, 1, 3);
  lay->addWidget(box);

  QGroupBox* tiers = new QGroupBox(tr("What the assistant may do"), this);
  QVBoxLayout* tl = new QVBoxLayout(tiers);
  for(int i = 0; i < 5; ++i)
  {
    _tierBoxes[i] = new QCheckBox(QCoreApplication::translate("McpDialog", tierInfo[i].name), tiers);
    _tierBoxes[i]->setToolTip(QCoreApplication::translate("McpDialog", tierInfo[i].tip));
    _tierBoxes[i]->setChecked(_settings.tiers & tierInfo[i].bit);
    // Only the tiers that have tools can be chosen.
    _tierBoxes[i]->setEnabled(tierInfo[i].available && tierInfo[i].bit != McpTierRead);
    if(!tierInfo[i].available)
      _tierBoxes[i]->setText(_tierBoxes[i]->text() + "  -  " + tr("no tools yet"));
    tl->addWidget(_tierBoxes[i]);
    connect(_tierBoxes[i], &QCheckBox::toggled, this, [this]() { apply(); });
  }
  lay->addWidget(tiers);

  lay->addWidget(new QLabel(tr("Activity:"), this));
  _log = new QPlainTextEdit(this);
  _log->setReadOnly(true);
  _log->setMaximumBlockCount(500);
  _log->setPlainText(server->activityLog().join("\n"));
  lay->addWidget(_log, 1);

  QDialogButtonBox* bb = new QDialogButtonBox(this);
  QPushButton* clear = bb->addButton(tr("Clear log"), QDialogButtonBox::ActionRole);
  bb->addButton(QDialogButtonBox::Close);
  lay->addWidget(bb);
  connect(clear, &QPushButton::clicked, _log, &QPlainTextEdit::clear);
  connect(bb, &QDialogButtonBox::rejected, this, &QDialog::close);

  connect(_enabled, &QCheckBox::toggled, this, [this]() { apply(); });
  connect(_port, &QSpinBox::editingFinished, this, [this]() { apply(); });
  connect(copyToken, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(_settings.token); });
  connect(copyCmd, &QPushButton::clicked, this, [this]() { QApplication::clipboard()->setText(command()); });
  connect(regen, &QPushButton::clicked, this, [this]() {
    if(QMessageBox::question(this, tr("New token"),
         tr("Make a new token? Assistants that use the old one will be refused until they are given the new one."),
         QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) != QMessageBox::Yes)
      return;
    _settings.token = McpSettings::generateToken();
    _token->setText(_settings.token);
    apply();
  });
  connect(server, &McpServer::activity, _log, &QPlainTextEdit::appendPlainText);
  connect(server, &McpServer::statusChanged, this, [this]() { updateStatus(); });

  updateStatus();
}

QString McpDialog::command() const
{
  return QString("claude mcp add --transport http muse http://127.0.0.1:%1/mcp --header \"Authorization: Bearer %2\"")
         .arg(_settings.port).arg(_settings.token);
}

void McpDialog::updateStatus()
{
  _url->setText(QString("http://127.0.0.1:%1/mcp").arg(_port->value()));
  if(_server->isRunning())
    _status->setText(tr("Running"));
  else if(!_server->errorString().isEmpty())
    _status->setText(tr("Could not start: %1").arg(_server->errorString()));
  else
    _status->setText(tr("Not running"));
}

void McpDialog::apply()
{
  _settings.enabled = _enabled->isChecked();
  _settings.port = _port->value();
  unsigned tiers = McpTierRead;
  for(int i = 0; i < 5; ++i)
    if(_tierBoxes[i]->isChecked())
      tiers |= tierInfo[i].bit;
  _settings.tiers = tiers;
  _settings.save();

  // Restarting is what makes a new port, token or tiers apply.
  if(_settings.enabled)
    _server->start(_settings);
  else
    _server->stop();
  updateStatus();
}

} // namespace MusEGui
