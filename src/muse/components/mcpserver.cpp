//=========================================================
//  MusE
//  Linux Music Editor
//
//  mcpserver.cpp
//  See mcpserver.h.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; either version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "mcpserver.h"

#include <QElapsedTimer>
#include <QFile>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QSettings>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTime>
#include <QTimer>
#include <QUrl>

#include <memory>

#include <sys/stat.h>

#include "config.h"
#include "globals.h"

namespace MusEGui {

static const int mcpMaxHeaderBytes = 16 * 1024;
static const int mcpMaxBodyBytes = 1024 * 1024;
// A connection is never kept longer than this. (The longest tool takes ten seconds.)
static const int mcpConnectionLifetimeMs = 30000;
static const int mcpMaxLogLines = 500;

//---------------------------------------------------------
//   McpSettings
//---------------------------------------------------------

static QString mcpSettingsPath()
{
  return MusEGlobal::configPath + "/mcp.conf";
}

QString McpSettings::generateToken()
{
  // From the random generator of the system.
  QByteArray b(32, 0);
  QRandomGenerator* rg = QRandomGenerator::system();
  for(int i = 0; i < b.size(); ++i)
    b[i] = char(rg->bounded(256));
  return QString::fromLatin1(b.toHex());
}

McpSettings McpSettings::load()
{
  McpSettings s;
  QSettings qs(mcpSettingsPath(), QSettings::IniFormat);
  s.enabled = qs.value("enabled", false).toBool();
  s.port = qs.value("port", 8765).toInt();
  if(s.port < 1024 || s.port > 65535)
    s.port = 8765;
  s.tiers = qs.value("tiers", int(McpTierRead)).toUInt() | McpTierRead;
  s.token = qs.value("token").toString();
  if(s.token.size() < 32)
  {
    s.token = generateToken();
    s.save();
  }
  return s;
}

void McpSettings::save() const
{
  const QString path = mcpSettingsPath();
  // The token is in the file, so only the user may read it. Set that before anything is written to it.
  if(!QFile::exists(path))
  {
    QFile f(path);
    if(f.open(QIODevice::WriteOnly))
      f.close();
  }
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
  // Whatever file is made when saving, nobody else may read it.
  const mode_t old_umask = umask(0077);
  {
    QSettings qs(path, QSettings::IniFormat);
    qs.setValue("enabled", enabled);
    qs.setValue("port", port);
    qs.setValue("tiers", int(tiers));
    qs.setValue("token", token);
    qs.sync();
  }
  umask(old_umask);
  QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
}

//---------------------------------------------------------
//   Helpers
//---------------------------------------------------------

// Compares in the same time whatever the strings are.
static bool constantTimeEquals(const QByteArray& a, const QByteArray& b)
{
  unsigned char diff = a.size() == b.size() ? 0 : 1;
  const int n = qMin(a.size(), b.size());
  for(int i = 0; i < n; ++i)
    diff |= (unsigned char)(a[i] ^ b[i]);
  return diff == 0;
}

static QByteArray compactJson(const QJsonObject& o)
{
  return QJsonDocument(o).toJson(QJsonDocument::Compact);
}

static const char* statusText(int status)
{
  switch(status)
  {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 431: return "Request Header Fields Too Large";
    default:  return "Error";
  }
}

//---------------------------------------------------------
//   McpServer
//---------------------------------------------------------

McpServer::McpServer(QObject* parent) : QObject(parent), _server(nullptr)
{
}

McpServer::~McpServer()
{
  // The window that listens may already be gone, so do not tell anyone.
  blockSignals(true);
  stop();
}

bool McpServer::isRunning() const
{
  return _server && _server->isListening();
}

int McpServer::port() const
{
  return isRunning() ? _server->serverPort() : _settings.port;
}

void McpServer::stop()
{
  if(_server)
  {
    _server->close();
    const QList<QTcpSocket*> socks = _conns.keys();
    for(QTcpSocket* s : socks)
      s->abort();
    _server->deleteLater();
    _server = nullptr;
    addLog(tr("Server stopped"));
    emit statusChanged();
  }
}

bool McpServer::start(const McpSettings& settings)
{
  stop();
  _settings = settings;
  _settings.tiers |= McpTierRead;
  _error.clear();

  _server = new QTcpServer(this);
  // Only this computer: The loopback address, never any other.
  if(!_server->listen(QHostAddress::LocalHost, quint16(_settings.port)))
  {
    _error = _server->errorString();
    delete _server;
    _server = nullptr;
    addLog(tr("Could not start: %1").arg(_error));
    emit statusChanged();
    return false;
  }
  connect(_server, &QTcpServer::newConnection, this, &McpServer::onNewConnection);
  addLog(tr("Server started on 127.0.0.1:%1").arg(_server->serverPort()));
  emit statusChanged();
  return true;
}

void McpServer::addLog(const QString& line)
{
  const QString l = QTime::currentTime().toString("HH:mm:ss") + "  " + line;
  _log.push_back(l);
  while(_log.size() > mcpMaxLogLines)
    _log.pop_front();
  emit activity(l);
}

void McpServer::onNewConnection()
{
  while(_server && _server->hasPendingConnections())
  {
    QTcpSocket* sock = _server->nextPendingConnection();
    _conns.insert(sock, Conn());
    connect(sock, &QTcpSocket::readyRead, this, [this, sock]() { onReadyRead(sock); });
    connect(sock, &QTcpSocket::disconnected, this, [this, sock]() {
      _conns.remove(sock);
      sock->deleteLater();
    });
    // Never keep a connection for long. The timer goes away with the socket.
    QTimer::singleShot(mcpConnectionLifetimeMs, sock, [sock]() { sock->abort(); });
  }
}

void McpServer::onReadyRead(QTcpSocket* sock)
{
  const auto it = _conns.find(sock);
  if(it == _conns.end())
    return;
  Conn& c = it.value();
  const QByteArray data = sock->readAll();
  if(c.handled)
    return;
  c.buf += data;

  const int hdrEnd = c.buf.indexOf("\r\n\r\n");
  if(hdrEnd < 0)
  {
    if(c.buf.size() > mcpMaxHeaderBytes)
    {
      c.handled = true;
      sendHttp(sock, 431);
    }
    return;
  }
  if(hdrEnd > mcpMaxHeaderBytes)
  {
    c.handled = true;
    sendHttp(sock, 431);
    return;
  }

  const QList<QByteArray> lines = c.buf.left(hdrEnd).split('\n');
  const QList<QByteArray> reqLine = lines.isEmpty() ? QList<QByteArray>() : lines[0].trimmed().split(' ');
  if(reqLine.size() != 3)
  {
    c.handled = true;
    sendHttp(sock, 400);
    return;
  }
  const QString method = QString::fromLatin1(reqLine[0]);
  const QString path = QString::fromLatin1(reqLine[1]);
  QHash<QString, QString> headers;
  for(int i = 1; i < lines.size(); ++i)
  {
    const QByteArray line = lines[i].trimmed();
    const int colon = line.indexOf(':');
    if(colon <= 0)
      continue;
    headers.insert(QString::fromLatin1(line.left(colon)).trimmed().toLower(),
                   QString::fromLatin1(line.mid(colon + 1)).trimmed());
  }

  if(headers.contains("transfer-encoding"))
  {
    // Chunked bodies are not supported.
    c.handled = true;
    sendHttp(sock, 411);
    return;
  }
  bool ok = true;
  const int contentLength = headers.contains("content-length") ? headers.value("content-length").toInt(&ok) : 0;
  if(!ok || contentLength < 0)
  {
    c.handled = true;
    sendHttp(sock, 400);
    return;
  }
  if(contentLength > mcpMaxBodyBytes)
  {
    c.handled = true;
    sendHttp(sock, 413);
    return;
  }

  const int bodyStart = hdrEnd + 4;
  if(c.buf.size() < bodyStart + contentLength)
  {
    // Some clients wait for this before they send the body.
    if(!c.continueSent && headers.value("expect").compare("100-continue", Qt::CaseInsensitive) == 0)
    {
      c.continueSent = true;
      sock->write("HTTP/1.1 100 Continue\r\n\r\n");
    }
    return;
  }

  c.handled = true;
  const QByteArray body = c.buf.mid(bodyStart, contentLength);
  c.buf.clear();
  handleRequest(sock, method, path, headers, body);
}

bool McpServer::hostAllowed(const QString& host) const
{
  // Only the names of this computer, with our port or no port. Others could be a web page that has been
  //  made to point to us (DNS rebinding).
  QString h = host.toLower();
  QString portPart;
  if(h.startsWith('['))
  {
    const int e = h.indexOf(']');
    if(e < 0)
      return false;
    portPart = h.mid(e + 1);
    h = h.left(e + 1);
  }
  else
  {
    const int colon = h.lastIndexOf(':');
    if(colon >= 0)
    {
      portPart = h.mid(colon);
      h = h.left(colon);
    }
  }
  if(h != "127.0.0.1" && h != "localhost" && h != "[::1]")
    return false;
  if(portPart.isEmpty())
    return true;
  return portPart == QString(":%1").arg(port());
}

bool McpServer::originAllowed(const QString& origin)
{
  // Clients that are not web pages do not send an Origin. A web page does, and it must be local.
  const QString h = QUrl(origin).host().toLower();
  return h == "127.0.0.1" || h == "localhost" || h == "::1";
}

void McpServer::sendHttp(QTcpSocket* sock, int status, const QByteArray& body, const QString& extraHeaders)
{
  if(!sock || sock->state() != QAbstractSocket::ConnectedState)
    return;
  QByteArray out = QString("HTTP/1.1 %1 %2\r\n").arg(status).arg(statusText(status)).toLatin1();
  if(!body.isEmpty())
    out += "Content-Type: application/json\r\n";
  out += QString("Content-Length: %1\r\n").arg(body.size()).toLatin1();
  out += "Cache-Control: no-store\r\nConnection: close\r\n";
  if(!extraHeaders.isEmpty())
    out += extraHeaders.toLatin1();
  out += "\r\n";
  out += body;
  sock->write(out);
  // Closes when everything has been written.
  sock->disconnectFromHost();
}

void McpServer::sendJson(QTcpSocket* sock, const QJsonObject& obj, int status)
{
  sendHttp(sock, status, compactJson(obj));
}

void McpServer::sendRpcError(QTcpSocket* sock, const QJsonValue& id, int code, const QString& message, int status)
{
  QJsonObject err;
  err["code"] = code;
  err["message"] = message;
  QJsonObject o;
  o["jsonrpc"] = "2.0";
  o["id"] = id;
  o["error"] = err;
  sendJson(sock, o, status);
}

void McpServer::handleRequest(QTcpSocket* sock, const QString& method, const QString& path,
                              const QHash<QString, QString>& headers, const QByteArray& body)
{
  if(!hostAllowed(headers.value("host")))
  {
    addLog(tr("Refused a request: bad Host header"));
    sendHttp(sock, 403);
    return;
  }
  if(headers.contains("origin") && !originAllowed(headers.value("origin")))
  {
    addLog(tr("Refused a request from the web page %1").arg(headers.value("origin")));
    sendHttp(sock, 403);
    return;
  }
  const QString p = path.section('?', 0, 0);
  if(p != "/mcp" && p != "/mcp/")
  {
    sendHttp(sock, 404);
    return;
  }

  // The token.
  const QString auth = headers.value("authorization");
  const QByteArray token = auth.startsWith("Bearer ", Qt::CaseInsensitive) ? auth.mid(7).trimmed().toUtf8() : QByteArray();
  if(token.isEmpty() || !constantTimeEquals(token, _settings.token.toUtf8()))
  {
    addLog(tr("Refused a request: missing or wrong token"));
    sendHttp(sock, 401, QByteArray(), "WWW-Authenticate: Bearer\r\n");
    return;
  }

  if(method != "POST")
  {
    // There is no stream of server messages, and no sessions to end.
    sendHttp(sock, 405, QByteArray(), "Allow: POST\r\n");
    return;
  }

  QJsonParseError pe;
  const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
  if(pe.error != QJsonParseError::NoError)
  {
    sendRpcError(sock, QJsonValue(), -32700, tr("Parse error"), 400);
    return;
  }
  if(!doc.isObject())
  {
    sendRpcError(sock, QJsonValue(), -32600, tr("Only single messages are supported"), 400);
    return;
  }
  handleJsonRpc(sock, doc.object());
}

void McpServer::handleJsonRpc(QTcpSocket* sock, const QJsonObject& msg)
{
  // A response from the client (we never ask), or a notification: Nothing to answer.
  if(!msg.contains("method") || !msg.contains("id"))
  {
    sendHttp(sock, 202);
    return;
  }

  const QJsonValue id = msg.value("id");
  const QString method = msg.value("method").toString();
  const QJsonObject params = msg.value("params").toObject();

  QJsonObject result;
  if(method == "initialize")
  {
    static const QStringList supported = { "2025-06-18", "2025-03-26", "2024-11-05" };
    const QString wanted = params.value("protocolVersion").toString();
    result["protocolVersion"] = supported.contains(wanted) ? wanted : supported.first();
    QJsonObject tools;
    tools["listChanged"] = false;
    QJsonObject caps;
    caps["tools"] = tools;
    result["capabilities"] = caps;
    QJsonObject info;
    info["name"] = "MusE";
    info["title"] = "MusE Sequencer";
    info["version"] = QString(VERSION);
    result["serverInfo"] = info;
    result["instructions"] = tr(
      "MusE is a MIDI and audio sequencer. Tracks are identified by their unique names. "
      "Positions are in ticks unless a name says otherwise, and get_transport also gives frames and seconds. "
      "Levels are in dBFS (-120 means silence) and gains are given both linear and in dB. "
      "Only the tools of the tiers that the user has allowed are available.");
    addLog(tr("Client connected: %1").arg(params.value("clientInfo").toObject().value("name").toString()));
  }
  else if(method == "ping")
  {
    // An empty result.
  }
  else if(method == "tools/list")
  {
    QJsonArray tools;
    for(const McpTool& t : _tools)
    {
      if(!(t.tier & _settings.tiers))
        continue;
      QJsonObject o;
      o["name"] = t.name;
      o["description"] = t.description;
      o["inputSchema"] = t.inputSchema;
      QJsonObject ann;
      ann["readOnlyHint"] = t.readOnly;
      o["annotations"] = ann;
      tools.push_back(o);
    }
    result["tools"] = tools;
  }
  else if(method == "tools/call")
  {
    callTool(sock, id, params);
    return;
  }
  else
  {
    sendRpcError(sock, id, -32601, tr("Method not found: %1").arg(method));
    return;
  }

  QJsonObject o;
  o["jsonrpc"] = "2.0";
  o["id"] = id;
  o["result"] = result;
  sendJson(sock, o);
}

void McpServer::callTool(QTcpSocket* sock, const QJsonValue& id, const QJsonObject& params)
{
  const QString name = params.value("name").toString();
  const McpTool* tool = nullptr;
  for(const McpTool& t : _tools)
    if(t.name == name && (t.tier & _settings.tiers))
      tool = &t;
  if(!tool)
  {
    sendRpcError(sock, id, -32602, tr("Unknown tool: %1").arg(name));
    return;
  }

  const QJsonObject args = params.value("arguments").toObject();
  QPointer<QTcpSocket> psock(sock);
  QPointer<McpServer> self(this);
  const auto called = std::make_shared<bool>(false);
  const McpTool toolCopy = *tool;

  McpDone done = [self, psock, id, called, name, args](const QJsonObject& res, const QString& err) {
    if(*called || !self)
      return;
    *called = true;

    QString shown = QString::fromUtf8(compactJson(args));
    if(shown.size() > 160)
      shown = shown.left(160) + "...";
    self->addLog(QString("%1 %2 -> %3").arg(name, shown, err.isEmpty() ? QString("ok") : QString("error: ") + err));

    if(!psock)
      return;
    QJsonObject r;
    QJsonObject text;
    text["type"] = "text";
    if(err.isEmpty())
    {
      text["text"] = QString::fromUtf8(compactJson(res));
      r["structuredContent"] = res;
      r["isError"] = false;
    }
    else
    {
      text["text"] = err;
      r["isError"] = true;
    }
    QJsonArray content;
    content.push_back(text);
    r["content"] = content;
    QJsonObject o;
    o["jsonrpc"] = "2.0";
    o["id"] = id;
    o["result"] = r;
    self->sendJson(psock, o);
  };

  toolCopy.handler(args, done);
}

} // namespace MusEGui
