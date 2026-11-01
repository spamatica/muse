//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3_cache_test.cpp
//  Tests the VST3 part of the plugin cache: scanning bundles in the sandbox
//  program, writing and reading the cache file, and detecting changes.
//
//  usage: vst3_cache_test <path of muse_plugin_scan>
//
//  Runs against the VST3 plugins installed in the standard locations, which
//  are linked into a temporary directory. Exit code 77 means skipped.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <set>

#include "config.h"
#include "plugin_cache_reader.h"
#include "plugin_cache_writer.h"
#include "plugin_list.h"

using namespace MusEPlugin;

static int g_failed = 0, g_checks = 0;

#define CHECK(cond) do { ++g_checks; if(!(cond)) { ++g_failed; \
  std::fprintf(stderr, "    FAILED %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while(0)
#define CHECK_MSG(cond, msg) do { ++g_checks; if(!(cond)) { ++g_failed; \
  std::fprintf(stderr, "    FAILED %s:%d: %s (%s)\n", __FILE__, __LINE__, #cond, qPrintable(msg)); } } while(0)

static const PluginTypes_t TYPES = PluginTypeVST3 | PluginTypeUnknown;

static int countType(const PluginScanList& list, PluginType type)
{
  int n = 0;
  for(ciPluginScanList i = list.begin(); i != list.end(); ++i)
    if((*i)->info()._type == type)
      ++n;
  return n;
}

static bool hasFile(const PluginScanList& list, const QString& filePath, PluginType type)
{
  for(ciPluginScanList i = list.begin(); i != list.end(); ++i)
    if((*i)->info()._type == type && (*i)->info().filePath() == filePath)
      return true;
  return false;
}

// A directory that looks like a VST3 bundle but holds garbage instead of a plugin.
static void makeBrokenBundle(const QString& path)
{
  QDir().mkpath(path + "/Contents/x86_64-linux");
  QFile f(path + "/Contents/x86_64-linux/" + QFileInfo(path).completeBaseName() + ".so");
  f.open(QIODevice::WriteOnly);
  f.write("this is not an ELF file");
  f.close();
}

static void touchBundleLibrary(const QString& path)
{
  const QFileInfo so(path + "/Contents/x86_64-linux/" + QFileInfo(path).completeBaseName() + ".so");
  QFile f(so.filePath());
  f.open(QIODevice::Append);
  f.write("x");
  f.close();
}

static bool check(const QString& cacheDir, PluginScanList* list, bool alwaysRecreate = false, bool dontRecreate = false,
                  PluginTypes_t types = TYPES)
{
  list->clear();
  return checkPluginCacheFiles(cacheDir, list, false, alwaysRecreate, dontRecreate, QString(), types, false);
}

static QDateTime cacheTime(const QString& cacheDir)
{
  return QFileInfo(cacheDir + "/" + pluginCacheFilename(PluginTypeVST3)).lastModified();
}

int main(int argc, char** argv)
{
  QCoreApplication app(argc, argv);

  if(argc < 2)
  {
    std::fprintf(stderr, "usage: %s <muse_plugin_scan>\n", argv[0]);
    return 2;
  }
  const QString scanner = QFileInfo(argv[1]).absoluteFilePath();

  // Real plugins to scan, from the standard locations.
  QStringList real;
  for(const QString& d : pluginGetVst3Directories())
  {
    const QFileInfoList l = QDir(d).entryInfoList(QStringList() << "*.vst3", QDir::AllEntries | QDir::NoDotAndDotDot);
    for(const QFileInfo& fi : l)
      real << fi.absoluteFilePath();
  }
  real.removeDuplicates();
  if(real.isEmpty())
  {
    std::printf("no VST3 plugins installed, skipping\n");
    return 77;
  }
  if(real.size() > 3)
    real = real.mid(0, 3);

  QTemporaryDir tmp;
  if(!tmp.isValid())
    return 2;
  const QString root = tmp.path();

  // The scanner is found through APPDIR + BINDIR.
  const QString appdir = root + "/appdir";
  QDir().mkpath(appdir + BINDIR);
  CHECK(QFile::link(scanner, appdir + BINDIR + "/muse_plugin_scan"));
  qputenv("APPDIR", appdir.toLocal8Bit());

  // Plugin directory: the real plugins, one of them in a sub directory, plus a broken bundle.
  const QString vst3 = root + "/vst3";
  QDir().mkpath(vst3 + "/sub/deeper");
  QStringList linked;
  for(int i = 0; i < real.size(); ++i)
  {
    const QString dest = (i == 0 ? vst3 + "/sub/deeper/" : vst3 + "/") + QFileInfo(real[i]).fileName();
    CHECK(QFile::link(real[i], dest));
    linked << dest;
  }
  const QString broken = vst3 + "/Broken.vst3";
  makeBrokenBundle(broken);
  qputenv("VST3_PATH", vst3.toLocal8Bit());

  const QString cache = root + "/cache";

  std::printf("[ RUN  ] directories\n");
  CHECK(pluginGetVst3Directories() == QStringList(vst3));

  std::printf("[ RUN  ] bundle time\n");
  {
    const std::int64_t t = pluginVst3BundleTime(broken);
    CHECK(t > 0);
    touchBundleLibrary(broken);
    QThread::msleep(20);
    CHECK(pluginVst3BundleTime(broken) >= t);
    CHECK(pluginVst3BundleTime(root + "/does/not/exist.vst3") == 0);
  }

  PluginScanList list;

  std::printf("[ RUN  ] first scan creates the cache\n");
  CHECK(check(cache, &list));
  CHECK(QFileInfo(cache + "/" + pluginCacheFilename(PluginTypeVST3)).exists());
  CHECK(QFileInfo(cache + "/" + pluginCacheFilename(PluginTypeUnknown)).exists());
  const int vst3Count = countType(list, PluginTypeVST3);
  CHECK_MSG(vst3Count >= real.size(), QString("found %1 VST3 plugin(s) in %2 bundle(s)").arg(vst3Count).arg(real.size()));
  for(ciPluginScanList i = list.begin(); i != list.end(); ++i)
  {
    const PluginScanInfoStruct& info = (*i)->info();
    if(info._type != PluginTypeVST3)
      continue;
    CHECK(info._uri.size() == 32);
    CHECK(!info._name.isEmpty());
    CHECK(!info._label.isEmpty());
    CHECK(info._class != PluginClassNone);
    CHECK(info._outports > 0);
    CHECK(info._fileTime == pluginVst3BundleTime(info.filePath()));
    CHECK(!info._fileIsBad);
    CHECK(info.filePath().endsWith(".vst3"));
  }
  for(const QString& l : linked)
  {
    int n = 0;
    for(ciPluginScanList i = list.begin(); i != list.end(); ++i)
      if((*i)->info()._type == PluginTypeVST3 && (*i)->info().filePath() == l)
        ++n;
    CHECK_MSG(n >= 1, "no plugin found in " + l);
  }
  // The broken bundle is kept as an unknown file, so that it is not scanned again and again.
  CHECK(hasFile(list, broken, PluginTypeUnknown));
  CHECK(!hasFile(list, broken, PluginTypeVST3));

  std::printf("[ RUN  ] unchanged plugins are not scanned again\n");
  const QDateTime t1 = cacheTime(cache);
  QThread::msleep(1200);   // File time stamps may be coarse.
  PluginScanList list2;
  CHECK(check(cache, &list2));
  CHECK(cacheTime(cache) == t1);
  CHECK(countType(list2, PluginTypeVST3) == vst3Count);
  CHECK(hasFile(list2, broken, PluginTypeUnknown));
  // What came back from the cache file is the same as what the scan produced.
  for(ciPluginScanList i = list.begin(); i != list.end(); ++i)
  {
    const PluginScanInfoStruct& a = (*i)->info();
    if(a._type != PluginTypeVST3)
      continue;
    const PluginScanInfoRef r = list2.find(a);
    CHECK(r);
    if(r)
    {
      CHECK(r->info()._name == a._name);
      CHECK(r->info()._maker == a._maker);
      CHECK(r->info()._class == a._class);
      CHECK(r->info()._inports == a._inports);
      CHECK(r->info()._outports == a._outports);
      CHECK(r->info()._controlInPorts == a._controlInPorts);
      CHECK(r->info()._fileTime == a._fileTime);
    }
  }

  std::printf("[ RUN  ] a modified bundle triggers a rescan\n");
  touchBundleLibrary(broken);
  QThread::msleep(1200);
  CHECK(check(cache, &list2));
  const QDateTime t2 = cacheTime(cache);
  CHECK(t2 > t1);
  CHECK(countType(list2, PluginTypeVST3) == vst3Count);

  std::printf("[ RUN  ] a new bundle triggers a rescan\n");
  QThread::msleep(1200);
  const QString another = vst3 + "/sub/Another.vst3";
  makeBrokenBundle(another);
  CHECK(check(cache, &list2));
  CHECK(cacheTime(cache) > t2);
  CHECK(hasFile(list2, another, PluginTypeUnknown));
  const QDateTime t3 = cacheTime(cache);

  std::printf("[ RUN  ] a removed bundle triggers a rescan\n");
  QThread::msleep(1200);
  QDir(broken).removeRecursively();
  CHECK(check(cache, &list2));
  CHECK(cacheTime(cache) > t3);
  CHECK(!hasFile(list2, broken, PluginTypeUnknown));
  CHECK(hasFile(list2, another, PluginTypeUnknown));
  CHECK(countType(list2, PluginTypeVST3) == vst3Count);

  std::printf("[ RUN  ] forced rescan\n");
  const QDateTime t4 = cacheTime(cache);
  QThread::msleep(1200);
  CHECK(check(cache, &list2, true));
  CHECK(cacheTime(cache) > t4);

  std::printf("[ RUN  ] other types do not see VST3\n");
  PluginScanList other;
  CHECK(readPluginCacheFile(cache, &other, false, false, PluginTypeVST3));
  CHECK(countType(other, PluginTypeVST3) == vst3Count);
  PluginScanList none;
  QDir(cache).removeRecursively();
  CHECK(check(cache, &none, false, false, PluginTypeUnknown));
  CHECK(countType(none, PluginTypeVST3) == 0);
  CHECK(!QFileInfo(cache + "/" + pluginCacheFilename(PluginTypeVST3)).exists());

  std::printf("[ RUN  ] type strings\n");
  CHECK(pluginStringToType(pluginTypeToString(PluginTypeVST3)) == PluginTypeVST3);
  CHECK(QString(pluginTypeToString(PluginTypeVST3)) == "VST3");
  CHECK((PluginTypesAll & PluginTypeVST3) != 0);

  std::printf("\n%d check(s), %d failed\n", g_checks, g_failed);
  return g_failed ? 1 : 0;
}
