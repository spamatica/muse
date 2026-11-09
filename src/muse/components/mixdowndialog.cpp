//=========================================================
//  MusE
//  Linux Music Editor
//    $Id: mixdowndialog.cpp,v 1.1.1.1 2003/10/27 18:55:02 wschweer Exp $
//  (C) Copyright 2001 Werner Schweer (ws@seh.de)
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

#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <sndfile.h>
#include <cstring>

#include "globals.h"
#include "gconfig.h"
#include "mixdowndialog.h"
#include "wave.h"

namespace MusECore {

QString MixdownSettings::extension() const
      {
      switch (format & SF_FORMAT_TYPEMASK) {
            case SF_FORMAT_OGG: return QString("ogg");
            case 0x230000:      return QString("mp3");
            default:            return QString("wav");
            }
      }

SndFile* MixdownSettings::createSndFile(const QString& filePath) const
      {
      QDir().mkpath(QFileInfo(filePath).absolutePath());
      // An existing file would be appended to (or refused for compressed formats); start afresh.
      if (QFileInfo::exists(filePath))
            QFile::remove(filePath);
      SndFile* sf = new SndFile(filePath);
      sf->setFormat(format, channels, MusEGlobal::sampleRate);
      if (quality >= 0.0)
            sf->setWantedEncodingQuality(quality);
      return sf;
      }

bool getMixdownSettings(QWidget* parent, const QString& suggestedDir,
                        const QString& suggestedName, MixdownSettings& settings)
      {
      MusEGui::MixdownFileDialog dialog(parent, suggestedDir, suggestedName);
      if (dialog.exec() != QDialog::Accepted)
            return false;
      settings = dialog.settings();
      return true;
      }

} // namespace MusECore

namespace MusEGui {

//---------------------------------------------------------
//   MixdownFileDialog
//---------------------------------------------------------

static const int FMT_MPEG_MP3 = 0x230000 | 0x0082; // SF_FORMAT_MPEG | SF_FORMAT_MPEG_LAYER_III, libsndfile >= 1.1.0

static const QStringList& audioExtensions()
      {
      static const QStringList l = { "wav", "ogg", "mp3" };
      return l;
      }

static bool formatAvailable(int format)
      {
      SF_INFO info;
      memset(&info, 0, sizeof(info));
      info.samplerate = 44100;
      info.channels   = 2;
      info.format     = format;
      return sf_format_check(&info) != 0;
      }

static QString formatExtension(int format)
      {
      switch (format & SF_FORMAT_TYPEMASK) {
            case SF_FORMAT_OGG: return QString("ogg");
            case 0x230000:      return QString("mp3");
            default:            return QString("wav");
            }
      }

MixdownFileDialog::MixdownFileDialog(QWidget* parent, const QString& suggestedDir, const QString& suggestedName)
   : QDialog(parent), _suggestedDir(suggestedDir), _suggestedName(suggestedName)
      {
      setupUi(this);
      // Room for four lines of the description, whatever the font size is.
      labelModeInfo->setMinimumHeight(labelModeInfo->fontMetrics().lineSpacing() * 4);

      // Formats, only those libsndfile supports are offered.
      comboFormat->clear();
      _formats.clear();
      comboFormat->addItem(tr("wav, 16 Bit"));
      _formats.append(SF_FORMAT_WAV | SF_FORMAT_PCM_16);
      comboFormat->addItem(tr("wav, 24 Bit"));
      _formats.append(SF_FORMAT_WAV | SF_FORMAT_PCM_24);
      comboFormat->addItem(tr("wav, 32 Bit (float)"));
      _formats.append(SF_FORMAT_WAV | SF_FORMAT_FLOAT);
      if (formatAvailable(SF_FORMAT_OGG | SF_FORMAT_VORBIS)) {
            comboFormat->addItem(tr("ogg, Vorbis"));
            _formats.append(SF_FORMAT_OGG | SF_FORMAT_VORBIS);
            }
      if (formatAvailable(FMT_MPEG_MP3)) {
            comboFormat->addItem(tr("mp3"));
            _formats.append(FMT_MPEG_MP3);
            }
      // Restore the last used settings.
      spinQuality->setValue(MusEGlobal::config.mixdownQuality);
      {
      const int fi = _formats.indexOf(MusEGlobal::config.mixdownFormat);
      if (fi >= 0)
            comboFormat->setCurrentIndex(fi);
      const int ci = MusEGlobal::config.mixdownChannels;
      if (ci >= 0 && ci < comboChannel->count())
            comboChannel->setCurrentIndex(ci);
      comboMode->setCurrentIndex(MusEGlobal::config.mixdownMode != 0 ? 1 : 0);
      }

      connect(buttonPath, SIGNAL(clicked()), SLOT(fdialog()));
      connect(comboFormat, SIGNAL(currentIndexChanged(int)), SLOT(formatChanged(int)));
      connect(comboMode, SIGNAL(currentIndexChanged(int)), SLOT(modeChanged(int)));
      if (!suggestedName.isEmpty())
            editPath->setText(suggestPath(suggestedDir, suggestedName));
      modeChanged(comboMode->currentIndex());
      formatChanged(comboFormat->currentIndex());
      }

//---------------------------------------------------------
//   suggestPath
//    <dir>/downmix/<name>_downmix.<ext>, with _2, _3 ... appended
//    if a downmix with that name already exists (or was the last one made).
//---------------------------------------------------------

QString MixdownFileDialog::suggestPath(const QString& dir, const QString& name) const
      {
      const QString base = QDir(dir).filePath("downmix/" + name + "_downmix");
      const QString ext  = formatExtension(_formats.value(comboFormat->currentIndex()));
      const QString last = QFileInfo(MusEGlobal::config.mixdownPath).absoluteFilePath();
      for (int n = 1; ; ++n) {
            const QString stem = n == 1 ? base : base + QString("_%1").arg(n);
            bool taken = false;
            for (const QString& e : audioExtensions()) {
                  const QString p = stem + "." + e;
                  if (QFileInfo::exists(p) || QFileInfo(p).absoluteFilePath() == last)
                        taken = true;
                  }
            if (!taken)
                  return stem + "." + ext;
            }
      }

//---------------------------------------------------------
//   formatChanged
//---------------------------------------------------------

void MixdownFileDialog::formatChanged(int idx)
      {
      const int fmt = _formats.value(idx);
      const bool compressed = (fmt & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV;
      labelQuality->setEnabled(compressed);
      spinQuality->setEnabled(compressed);
      if (comboMode->currentIndex() != 0)
            return; // The path is a folder.
      // Keep the file extension in step with the format.
      QString path = editPath->text();
      const QString ext = formatExtension(fmt);
      for (const QString& e : audioExtensions()) {
            if (path.endsWith("." + e, Qt::CaseInsensitive)) {
                  path.chop(e.size());
                  editPath->setText(path + ext);
                  break;
                  }
            }
      }

//---------------------------------------------------------
//   accept
//---------------------------------------------------------

void MixdownFileDialog::modeChanged(int idx)
      {
      const bool markers = idx != 0;
      TextLabel1->setText(markers ? tr("Folder") : tr("File Path"));
      labelModeInfo->setText(markers ?
            tr("One file for each named marker, from the marker to the next marker "
               "(the last one to the end of the song). The file name is the project name plus the marker name.") :
            tr("One file between the left and right range markers."));
      if (markers) {
            // Suggest the folder instead of a file.
            const QString p = editPath->text();
            bool isFile = false;
            for (const QString& e : audioExtensions())
                  if (p.endsWith("." + e, Qt::CaseInsensitive))
                        isFile = true;
            if (p.isEmpty() || isFile)
                  editPath->setText(QDir(_suggestedDir).filePath("downmix"));
            }
      else {
            const QString p = editPath->text();
            bool isFile = false;
            for (const QString& e : audioExtensions())
                  if (p.endsWith("." + e, Qt::CaseInsensitive))
                        isFile = true;
            if (!isFile && !_suggestedName.isEmpty())
                  editPath->setText(suggestPath(_suggestedDir, _suggestedName));
            }
      }

//---------------------------------------------------------
//   accept
//---------------------------------------------------------

void MixdownFileDialog::accept()
      {
      int channel = comboChannel->currentIndex();
      switch (channel) {
            case 0: channel = 2; break;
            case 1: channel = 1; break;
            case 2: channel = 6; break;     // not implemented!
            }
      const int format = _formats.value(comboFormat->currentIndex());
      QString path = editPath->text();
      if (path.isEmpty()) {
            reject();
            return;
            }
      _settings.markerMode = comboMode->currentIndex() != 0;
      _settings.format     = format;
      _settings.channels   = channel;
      _settings.quality    = (format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV ? spinQuality->value() / 10.0 : -1.0;
      if (!_settings.markerMode) {
            const QString ext = _settings.extension();
            if (!path.endsWith("." + ext, Qt::CaseInsensitive))
                  path += "." + ext;
            }
      _settings.path = path;
      MusEGlobal::config.mixdownMode     = _settings.markerMode ? 1 : 0;
      MusEGlobal::config.mixdownFormat   = format;
      MusEGlobal::config.mixdownChannels = comboChannel->currentIndex();
      MusEGlobal::config.mixdownQuality  = spinQuality->value();
      done(1);
      }

//---------------------------------------------------------
//   fdialog
//---------------------------------------------------------

void MixdownFileDialog::fdialog()
{
  QString oldpath = editPath->text();
  if (oldpath.isEmpty())
        oldpath = MusEGlobal::config.mixdownPath;
  if (comboMode->currentIndex() != 0) {
        const QString dir = QFileDialog::getExistingDirectory(this, QString(), oldpath);
        if (!dir.isEmpty())
              editPath->setText(dir);
        return;
        }
  QString path = QFileDialog::getSaveFileName(this, QString(), oldpath,
        tr("Audio Files (*.wav *.ogg *.mp3);;All Files (*)"));
  if (!path.isEmpty()) {
        editPath->setText(path);
        // Pick the matching format if the extension says so.
        for (int i = 0; i < _formats.size(); ++i)
              if (path.endsWith("." + formatExtension(_formats[i]), Qt::CaseInsensitive)) {
                    if (formatExtension(_formats[comboFormat->currentIndex()]) !=
                        formatExtension(_formats[i]))
                          comboFormat->setCurrentIndex(i);
                    break;
                    }
  }
}

} // namespace MusEGui
