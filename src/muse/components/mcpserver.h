//=========================================================
//  MusE
//  Linux Music Editor
//
//  mcpserver.h
//  A Model Context Protocol (MCP) server in MusE, so that an AI assistant can look at the song and the mixer.
//
//  It speaks MCP over HTTP (the "streamable HTTP" transport, without server sent events) on the loopback
//  interface only. It is off by default. Every request needs the bearer token, and the Host and Origin
//  headers must be local (against DNS rebinding from web pages). The tools are in tiers that the user can
//  switch on and off, and a tool in a tier that is off is not even listed. All tools run in the GUI thread,
//  so changes go through the Operations and Undo system like any change from the GUI.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __MCPSERVER_H__
#define __MCPSERVER_H__

#include <QHash>
#include <QJsonObject>
#include <QJsonValue>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QByteArray>

#include <functional>
#include <vector>

class QTcpServer;
class QTcpSocket;

namespace MusEGui {

// The tiers of tools. Combined as bits.
enum McpTier
{
  McpTierRead      = 0x01,   // Looking at the song, the mixer and the levels. Always on.
  McpTierControl   = 0x02,   // Reversible controls: Mute, solo, volume, pan, plugin parameters, transport.
  McpTierRecord    = 0x04,   // Arming tracks and recording. Creates files.
  McpTierEdit      = 0x08,   // Adding and moving tracks and parts, routing.
  McpTierDangerous = 0x10    // Deleting, file import and export.
};

struct McpSettings
{
  bool enabled = false;
  int port = 8765;
  unsigned tiers = McpTierRead;
  QString token;

  // Loads the settings, and makes a token if there is none. The file is only readable by the user,
  //  since it holds the token.
  static McpSettings load();
  void save() const;
  static QString generateToken();
};

// Called by a tool when it is done, with the result (an object), or an error message.
typedef std::function<void(const QJsonObject& result, const QString& error)> McpDone;

struct McpTool
{
  QString name;
  QString description;
  QJsonObject inputSchema;         // JSON Schema of the arguments, an object.
  unsigned tier = McpTierRead;     // One McpTier.
  bool readOnly = true;            // For the annotations that clients show to their users.
  // Runs in the GUI thread. Must call done exactly once, now or later (a measurement over time).
  std::function<void(const QJsonObject& args, McpDone done)> handler;
};

class McpServer : public QObject
{
      Q_OBJECT

      struct Conn
      {
        QByteArray buf;
        bool continueSent = false;
        bool handled = false;       // A request was read. Anything more is ignored, the connection is closed after the answer.
      };

      QTcpServer* _server;
      QHash<QTcpSocket*, Conn> _conns;
      McpSettings _settings;
      QString _error;
      std::vector<McpTool> _tools;
      QStringList _log;

      void onNewConnection();
      void onReadyRead(QTcpSocket* sock);
      // A whole request has been read.
      void handleRequest(QTcpSocket* sock, const QString& method, const QString& path,
                         const QHash<QString, QString>& headers, const QByteArray& body);
      void handleJsonRpc(QTcpSocket* sock, const QJsonObject& msg);
      void callTool(QTcpSocket* sock, const QJsonValue& id, const QJsonObject& params);
      void sendHttp(QTcpSocket* sock, int status, const QByteArray& body = QByteArray(),
                    const QString& extraHeaders = QString());
      void sendJson(QTcpSocket* sock, const QJsonObject& obj, int status = 200);
      void sendRpcError(QTcpSocket* sock, const QJsonValue& id, int code, const QString& message, int status = 200);
      bool hostAllowed(const QString& host) const;
      static bool originAllowed(const QString& origin);
      void addLog(const QString& line);

   signals:
      // The server was started or stopped.
      void statusChanged();
      // A line was added to the activity log.
      void activity(const QString& line);

   public:
      explicit McpServer(QObject* parent = nullptr);
      ~McpServer();

      // Starts listening with the settings (and stops first if it is running). Returns false on failure,
      //  then errorString() tells why.
      bool start(const McpSettings& settings);
      void stop();
      bool isRunning() const;
      int port() const;
      QString errorString() const { return _error; }
      const McpSettings& settings() const { return _settings; }
      // Changes which tiers are available, at once. The settings are not saved.
      void setTiers(unsigned tiers) { _settings.tiers = tiers | McpTierRead; }

      void addTool(const McpTool& tool) { _tools.push_back(tool); }
      const QStringList& activityLog() const { return _log; }
};

// Adds the tools to the server. In mcptools.cpp.
void registerMcpTools(McpServer& server);

} // namespace MusEGui

#endif
