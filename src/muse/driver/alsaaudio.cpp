//=========================================================
//  MusE
//  Linux Music Editor
//
//  Direct ALSA audio backend, an alternative to Jack and Pulse.
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

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

#include "audio.h"
#include "audiodev.h"
#include "globals.h"
#include "gconfig.h"
#include "al/al.h"
#include "alsaaudio.h"

namespace MusECore {

AlsaAudioDevice* alsaAudioDevice = nullptr;

//---------------------------------------------------------
//   Sample format conversion between the interleaved device
//    data and our float port buffers. The formats are the
//    native endian ones.
//---------------------------------------------------------

static void deviceToFloat(const char* src, snd_pcm_format_t fmt, unsigned int channels,
                          unsigned int frames, unsigned int ch, float* dst)
{
  switch(fmt)
  {
    case SND_PCM_FORMAT_FLOAT:
    {
      const float* s = reinterpret_cast<const float*>(src) + ch;
      for(unsigned int i = 0; i < frames; ++i, s += channels)
        dst[i] = *s;
    }
    break;
    case SND_PCM_FORMAT_S32:
    {
      const int32_t* s = reinterpret_cast<const int32_t*>(src) + ch;
      for(unsigned int i = 0; i < frames; ++i, s += channels)
        dst[i] = (float)*s * (1.0f / 2147483648.0f);
    }
    break;
    case SND_PCM_FORMAT_S24:
    {
      // 24 bits in the lower part of a 32 bit word. Shift up and down again to sign extend.
      const int32_t* s = reinterpret_cast<const int32_t*>(src) + ch;
      for(unsigned int i = 0; i < frames; ++i, s += channels)
        dst[i] = (float)((int32_t)((uint32_t)*s << 8) >> 8) * (1.0f / 8388608.0f);
    }
    break;
    case SND_PCM_FORMAT_S16:
    {
      const int16_t* s = reinterpret_cast<const int16_t*>(src) + ch;
      for(unsigned int i = 0; i < frames; ++i, s += channels)
        dst[i] = (float)*s * (1.0f / 32768.0f);
    }
    break;
    default:
      memset(dst, 0, frames * sizeof(float));
    break;
  }
}

static inline float clampSample(float v)
{
  return v > 1.0f ? 1.0f : (v < -1.0f ? -1.0f : v);
}

static void floatToDevice(char* dst, snd_pcm_format_t fmt, unsigned int channels,
                          unsigned int frames, unsigned int ch, const float* src)
{
  switch(fmt)
  {
    case SND_PCM_FORMAT_FLOAT:
    {
      float* d = reinterpret_cast<float*>(dst) + ch;
      for(unsigned int i = 0; i < frames; ++i, d += channels)
        *d = clampSample(src[i]);
    }
    break;
    case SND_PCM_FORMAT_S32:
    {
      int32_t* d = reinterpret_cast<int32_t*>(dst) + ch;
      for(unsigned int i = 0; i < frames; ++i, d += channels)
        *d = (int32_t)(clampSample(src[i]) * 2147483647.0f);
    }
    break;
    case SND_PCM_FORMAT_S24:
    {
      int32_t* d = reinterpret_cast<int32_t*>(dst) + ch;
      for(unsigned int i = 0; i < frames; ++i, d += channels)
        *d = (int32_t)(clampSample(src[i]) * 8388607.0f);
    }
    break;
    case SND_PCM_FORMAT_S16:
    {
      int16_t* d = reinterpret_cast<int16_t*>(dst) + ch;
      for(unsigned int i = 0; i < frames; ++i, d += channels)
        *d = (int16_t)(clampSample(src[i]) * 32767.0f);
    }
    break;
    default:
    break;
  }
}

//---------------------------------------------------------
//   AlsaAudioDevice
//---------------------------------------------------------

AlsaAudioDevice::AlsaAudioDevice() : AudioDevice()
{
  _playback = nullptr;
  _capture = nullptr;
  _playFormat = SND_PCM_FORMAT_UNKNOWN;
  _capFormat = SND_PCM_FORMAT_UNKNOWN;
  _playChannels = 0;
  _capChannels = 0;
  _periodSize = 0;
  _periodCount = 3;
  _threadCreated = false;
  _quit = false;
  _realTimePriority = 0;
  _start_timeUS = systemTimeUS();
  _criticalVariablesIdx = 0;
  _firstCycle = true;
  _xrunThisCycle = false;
  _waitMs = 1000;
  _lastStallWarnUS = 0;
  for(unsigned x = 0; x < 2; ++x)
  {
    _timeUSAtCycleStart[x] = 0;
    _framesAtCycleStart[x] = 0;
    _frameCounter[x] = 0;
  }
}

AlsaAudioDevice::~AlsaAudioDevice()
{
  stop();
  close();

  while(outputPortsList.size() > 0)
  {
    MuseAlsaAudioPort* port = outputPortsList.takeFirst();
    delete[] port->buffer;
    delete port;
  }
  while(inputPortsList.size() > 0)
  {
    MuseAlsaAudioPort* port = inputPortsList.takeFirst();
    delete[] port->buffer;
    delete port;
  }
}

void AlsaAudioDevice::checkXrun(snd_pcm_sframes_t err)
{
  // ALSA reports both a capture overrun and a playback underrun as EPIPE.
  if(err == -EPIPE && !_xrunThisCycle)
  {
    _xrunThisCycle = true;
    MusEGlobal::audio->incXruns();
  }
}

void AlsaAudioDevice::warnStall(const char* what)
{
  const uint64_t now = systemTimeUS();
  if(now - _lastStallWarnUS > 3000000UL)
  {
    _lastStallWarnUS = now;
    fprintf(stderr, "Warning: AlsaAudioDevice: the %s device did not respond within %d ms\n", what, _waitMs);
  }
}

void AlsaAudioDevice::setCriticalVariables(unsigned segmentSize)
{
  const unsigned idx = (_criticalVariablesIdx + 1) % 2;
  _timeUSAtCycleStart[idx] = systemTimeUS();
  // Let these start at zero and only increment on subsequent cycles.
  if(!_firstCycle)
  {
    _framesAtCycleStart[idx] = _framesAtCycleStart[_criticalVariablesIdx] + segmentSize;
    _frameCounter[idx] = _frameCounter[_criticalVariablesIdx] + segmentSize;
  }
  _firstCycle = false;
  // Now 'flip' the variables all at once.
  _criticalVariablesIdx = idx;
}

//---------------------------------------------------------
//   setupStream
//   Sets the hardware and software parameters of one stream. The rate, period and channels arguments are the
//    wanted values and are replaced by what the device accepted. Returns true on success.
//---------------------------------------------------------

bool AlsaAudioDevice::setupStream(snd_pcm_t* pcm, bool playback, unsigned int& rate, snd_pcm_uframes_t& period,
                                  unsigned int& channels, snd_pcm_format_t& format)
{
  const char* what = playback ? "playback" : "capture";
  snd_pcm_hw_params_t* hw;
  snd_pcm_hw_params_alloca(&hw);

  int err = snd_pcm_hw_params_any(pcm, hw);
  if(err < 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: cannot get hardware parameters: %s\n", what, snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
  if(err < 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: interleaved access is not supported: %s\n", what, snd_strerror(err));
    return false;
  }

  // Prefer the best resolution that the device supports.
  static const snd_pcm_format_t formats[] =
    { SND_PCM_FORMAT_FLOAT, SND_PCM_FORMAT_S32, SND_PCM_FORMAT_S24, SND_PCM_FORMAT_S16 };
  bool fmt_ok = false;
  for(unsigned int i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
  {
    if(snd_pcm_hw_params_set_format(pcm, hw, formats[i]) == 0)
    {
      format = formats[i];
      fmt_ok = true;
      break;
    }
  }
  if(!fmt_ok)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: none of the supported sample formats can be used by the device\n", what);
    return false;
  }

  unsigned int ch = 2;
  err = snd_pcm_hw_params_set_channels_near(pcm, hw, &ch);
  if(err < 0 || ch == 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: cannot set the number of channels: %s\n", what, snd_strerror(err));
    return false;
  }
  channels = ch;

  unsigned int r = rate;
  err = snd_pcm_hw_params_set_rate_near(pcm, hw, &r, nullptr);
  if(err < 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: cannot set the sample rate %u: %s\n", what, rate, snd_strerror(err));
    return false;
  }

  snd_pcm_uframes_t p = period;
  err = snd_pcm_hw_params_set_period_size_near(pcm, hw, &p, nullptr);
  if(err < 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: cannot set the period size %lu: %s\n", what, (unsigned long)period, snd_strerror(err));
    return false;
  }

  unsigned int np = _periodCount;
  err = snd_pcm_hw_params_set_periods_near(pcm, hw, &np, nullptr);
  if(err < 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: cannot set the number of periods: %s\n", what, snd_strerror(err));
    return false;
  }

  err = snd_pcm_hw_params(pcm, hw);
  if(err < 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: cannot set hardware parameters: %s\n", what, snd_strerror(err));
    return false;
  }

  // Read back what we actually got.
  snd_pcm_hw_params_get_rate(hw, &r, nullptr);
  snd_pcm_hw_params_get_period_size(hw, &p, nullptr);
  snd_pcm_hw_params_get_periods(hw, &np, nullptr);
  rate = r;
  period = p;
  if(playback)
    _periodCount = np;

  snd_pcm_sw_params_t* sw;
  snd_pcm_sw_params_alloca(&sw);
  err = snd_pcm_sw_params_current(pcm, sw);
  if(err < 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: cannot get software parameters: %s\n", what, snd_strerror(err));
    return false;
  }
  snd_pcm_sw_params_set_avail_min(pcm, sw, period);
  if(playback)
  {
    // Start when all but one period has been filled with the initial silence, see start().
    snd_pcm_uframes_t thr = period * (np > 1 ? np - 1 : 1);
    snd_pcm_sw_params_set_start_threshold(pcm, sw, thr);
  }
  err = snd_pcm_sw_params(pcm, sw);
  if(err < 0)
  {
    fprintf(stderr, "AlsaAudioDevice: %s: cannot set software parameters: %s\n", what, snd_strerror(err));
    return false;
  }

  return true;
}

//---------------------------------------------------------
//   open
//   Returns true on success.
//---------------------------------------------------------

bool AlsaAudioDevice::open()
{
  const QString dev_name = MusEGlobal::config.deviceAudioAlsaDevice.isEmpty() ?
                             QString("default") : MusEGlobal::config.deviceAudioAlsaDevice;
  const QByteArray name = dev_name.toLocal8Bit();

  int err = snd_pcm_open(&_playback, name.constData(), SND_PCM_STREAM_PLAYBACK, 0);
  if(err < 0)
  {
    fprintf(stderr, "Error: AlsaAudioDevice: cannot open playback device \"%s\": %s\n", name.constData(), snd_strerror(err));
    _playback = nullptr;
    return false;
  }

  // Capture is optional. The device might not have any inputs.
  err = snd_pcm_open(&_capture, name.constData(), SND_PCM_STREAM_CAPTURE, 0);
  if(err < 0)
  {
    fprintf(stderr, "Warning: AlsaAudioDevice: cannot open capture device \"%s\", running without inputs: %s\n",
            name.constData(), snd_strerror(err));
    _capture = nullptr;
  }

  unsigned int rate = MusEGlobal::config.deviceAudioSampleRate > 0 ? MusEGlobal::config.deviceAudioSampleRate : 48000;
  snd_pcm_uframes_t period = MusEGlobal::config.deviceAudioBufSize > 0 ? MusEGlobal::config.deviceAudioBufSize : 256;
  const unsigned int wanted_rate = rate;
  const snd_pcm_uframes_t wanted_period = period;

  if(!setupStream(_playback, true, rate, period, _playChannels, _playFormat))
  {
    close();
    return false;
  }

  if(_capture)
  {
    // The capture stream has to match the playback stream.
    unsigned int c_rate = rate;
    snd_pcm_uframes_t c_period = period;
    if(!setupStream(_capture, false, c_rate, c_period, _capChannels, _capFormat) ||
       c_rate != rate || c_period != period)
    {
      fprintf(stderr, "Warning: AlsaAudioDevice: the capture device does not match the playback device, running without inputs\n");
      snd_pcm_close(_capture);
      _capture = nullptr;
      _capChannels = 0;
    }
    else
    {
      // Make the two streams start and stop together. Not fatal if it does not work.
      snd_pcm_link(_capture, _playback);
    }
  }

  if(rate != wanted_rate)
    fprintf(stderr, "Warning: AlsaAudioDevice: samplerate %u is not supported, using %u\n", wanted_rate, rate);
  if(period != wanted_period)
    fprintf(stderr, "Warning: AlsaAudioDevice: period size %lu is not supported, using %lu\n",
            (unsigned long)wanted_period, (unsigned long)period);

  _periodSize = period;
  // Several periods, but not less than a tenth of a second.
  _waitMs = (int)(4000UL * period / rate);
  if(_waitMs < 100)
    _waitMs = 100;
  MusEGlobal::sampleRate = rate;
  // Make sure the AL namespace variables mirror our variables.
  AL::sampleRate = MusEGlobal::sampleRate;
  MusEGlobal::segmentSize = period;

  _playBuf.assign(_periodSize * _playChannels * (snd_pcm_format_physical_width(_playFormat) / 8), 0);
  if(_capture)
    _capBuf.assign(_periodSize * _capChannels * (snd_pcm_format_physical_width(_capFormat) / 8), 0);

  fprintf(stderr, "AlsaAudioDevice: device \"%s\": samplerate:%u period size:%lu periods:%u playback channels:%u capture channels:%u\n",
          name.constData(), rate, (unsigned long)period, _periodCount, _playChannels, _capChannels);
  return true;
}

void AlsaAudioDevice::close()
{
  if(_capture)
  {
    snd_pcm_close(_capture);
    _capture = nullptr;
  }
  if(_playback)
  {
    snd_pcm_close(_playback);
    _playback = nullptr;
  }
}

//---------------------------------------------------------
//   readPeriod
//   Reads one period of capture data into the input ports.
//   Returns false if there is no data. The input ports are cleared then.
//---------------------------------------------------------

bool AlsaAudioDevice::readPeriod()
{
  const unsigned int frame_bytes = _capChannels * (snd_pcm_format_physical_width(_capFormat) / 8);
  snd_pcm_uframes_t done = 0;
  bool ok = true;

  while(done < _periodSize)
  {
    // Never wait forever, a stream that did not start would otherwise block the whole program.
    const int w = snd_pcm_wait(_capture, _waitMs);
    if(w == 0)
    {
      if(snd_pcm_state(_capture) == SND_PCM_STATE_PREPARED)
        snd_pcm_start(_capture);
      warnStall("capture");
      ok = false;
      break;
    }
    const snd_pcm_sframes_t n = (w < 0) ? w : snd_pcm_readi(_capture, _capBuf.data() + done * frame_bytes, _periodSize - done);
    if(n >= 0)
    {
      done += n;
      continue;
    }

    checkXrun(n);
    // Try to recover from for example an xrun.
    const int err = snd_pcm_recover(_capture, n, 1);
    if(err < 0)
    {
      fprintf(stderr, "AlsaAudioDevice: cannot read from the capture device: %s\n", snd_strerror(err));
      // Avoid spinning if the device is gone.
      usleep(MusEGlobal::segmentSize * 1000000UL / MusEGlobal::sampleRate);
    }
    else
    {
      // The recovery leaves the streams stopped. Start them again.
      // Do not retry the read in this cycle, or a stream that keeps failing would never let the cycle finish.
      startStreams();
    }
    ok = false;
    break;
  }

  const unsigned int nports = inputPortsList.size();
  for(unsigned int c = 0; c < nports; ++c)
  {
    float* buf = inputPortsList.at(c)->buffer;
    if(!ok)
      memset(buf, 0, _periodSize * sizeof(float));
    else if(c < _capChannels)
      deviceToFloat(_capBuf.data(), _capFormat, _capChannels, _periodSize, c, buf);
    else if(_capChannels == 1)
      // A mono input feeds all the input ports.
      deviceToFloat(_capBuf.data(), _capFormat, _capChannels, _periodSize, 0, buf);
    else
      memset(buf, 0, _periodSize * sizeof(float));
  }
  return ok;
}

//---------------------------------------------------------
//   writePeriod
//   Writes one period of the output ports to the playback device.
//---------------------------------------------------------

void AlsaAudioDevice::writePeriod()
{
  const unsigned int frame_bytes = _playChannels * (snd_pcm_format_physical_width(_playFormat) / 8);

  // Silence for any channel that has no port. Zero is zero in all our formats.
  memset(_playBuf.data(), 0, _playBuf.size());
  const unsigned int nports = outputPortsList.size();
  for(unsigned int c = 0; c < _playChannels && c < nports; ++c)
    floatToDevice(_playBuf.data(), _playFormat, _playChannels, _periodSize, c, outputPortsList.at(c)->buffer);

  snd_pcm_uframes_t done = 0;
  while(done < _periodSize)
  {
    // Never wait forever, a stream that did not start would otherwise block the whole program.
    const int w = snd_pcm_wait(_playback, _waitMs);
    if(w == 0)
    {
      if(snd_pcm_state(_playback) == SND_PCM_STATE_PREPARED)
        snd_pcm_start(_playback);
      warnStall("playback");
      // Drop this period.
      return;
    }
    const snd_pcm_sframes_t n = (w < 0) ? w : snd_pcm_writei(_playback, _playBuf.data() + done * frame_bytes, _periodSize - done);
    if(n >= 0)
    {
      done += n;
      continue;
    }

    checkXrun(n);
    // Try to recover from for example an underrun. The data of this period is dropped then.
    const int err = snd_pcm_recover(_playback, n, 1);
    if(err < 0)
    {
      fprintf(stderr, "AlsaAudioDevice: cannot write to the playback device: %s\n", snd_strerror(err));
      // Avoid spinning if the device is gone.
      usleep(MusEGlobal::segmentSize * 1000000UL / MusEGlobal::sampleRate);
    }
    else
      // The recovery leaves the streams stopped. Start them again.
      startStreams();
    break;
  }
}

//---------------------------------------------------------
//   runLoop
//---------------------------------------------------------

void AlsaAudioDevice::runLoop()
{
  while(!_quit)
  {
    _xrunThisCycle = false;

    // Blocks until a period of capture data is available, which paces the cycle.
    // Without capture, writing blocks until there is room in the playback buffer.
    if(_capture)
      readPeriod();

    setCriticalVariables(_periodSize);

    if(MusEGlobal::audio->isRunning())
    {
      // Use our built-in transport, which INCLUDES the necessary
      //  calls to Audio::sync() and ultimately Audio::process(),
      //  and increments the built-in play position.
      processTransport(_periodSize);
    }
    else
    {
      // Nothing is processing. Do not play stale data.
      for(int i = 0; i < outputPortsList.size(); ++i)
        memset(outputPortsList.at(i)->buffer, 0, _periodSize * sizeof(float));
    }

    writePeriod();
  }
}

void* AlsaAudioDevice::audioLoop(void* arg)
{
  static_cast<AlsaAudioDevice*>(arg)->runLoop();
  return nullptr;
}

//---------------------------------------------------------
//   startStreams
//---------------------------------------------------------

bool AlsaAudioDevice::startStreams()
{
  // Start from a known state. If the streams are linked this affects both.
  snd_pcm_drop(_playback);
  if(_capture)
    snd_pcm_drop(_capture);

  int err = snd_pcm_prepare(_playback);
  if(err < 0)
  {
    fprintf(stderr, "Error: AlsaAudioDevice: cannot prepare the playback device: %s\n", snd_strerror(err));
    return false;
  }
  if(_capture)
  {
    err = snd_pcm_prepare(_capture);
    if(err < 0)
      fprintf(stderr, "Warning: AlsaAudioDevice: cannot prepare the capture device: %s\n", snd_strerror(err));
  }

  // Fill all but one period of the playback buffer with silence, so that the process loop has
  //  one period of time to deliver the first real data. Only write what fits so that this never blocks.
  memset(_playBuf.data(), 0, _playBuf.size());
  snd_pcm_sframes_t room = snd_pcm_avail(_playback);
  snd_pcm_uframes_t want = _periodSize * (_periodCount > 1 ? _periodCount - 1 : 1);
  if(room < 0)
    want = 0;
  else if(want > (snd_pcm_uframes_t)room)
    want = room;
  while(want > 0)
  {
    const snd_pcm_uframes_t chunk = want < _periodSize ? want : _periodSize;
    const snd_pcm_sframes_t n = snd_pcm_writei(_playback, _playBuf.data(), chunk);
    if(n <= 0)
    {
      fprintf(stderr, "Warning: AlsaAudioDevice: cannot fill the playback buffer: %s\n",
              n < 0 ? snd_strerror(n) : "no room");
      break;
    }
    want -= n;
  }

  // Do not depend on the automatic start of the streams. Some devices, such as the ones provided by sound servers,
  //  do not start the capture stream by themselves. If the streams are linked, starting one starts both.
  if(_capture && snd_pcm_state(_capture) == SND_PCM_STATE_PREPARED)
  {
    err = snd_pcm_start(_capture);
    if(err < 0)
      fprintf(stderr, "Warning: AlsaAudioDevice: cannot start the capture device: %s\n", snd_strerror(err));
  }
  if(snd_pcm_state(_playback) == SND_PCM_STATE_PREPARED)
  {
    err = snd_pcm_start(_playback);
    if(err < 0)
      fprintf(stderr, "Warning: AlsaAudioDevice: cannot start the playback device: %s\n", snd_strerror(err));
  }
  return true;
}

//---------------------------------------------------------
//   start
//   Returns true on success.
//---------------------------------------------------------

bool AlsaAudioDevice::start(int priority)
{
  if(!_playback)
    return false;
  if(_threadCreated)
    stop();

  _realTimePriority = priority;
  _quit = false;
  _firstCycle = true;

  if(!startStreams())
    return false;

  pthread_attr_t* attributes = nullptr;
  if(MusEGlobal::realTimeScheduling && _realTimePriority > 0)
  {
    attributes = (pthread_attr_t*) malloc(sizeof(pthread_attr_t));
    pthread_attr_init(attributes);

    if(pthread_attr_setschedpolicy(attributes, SCHED_FIFO))
      fprintf(stderr, "AlsaAudioDevice: cannot set FIFO scheduling class for RT thread\n");
    if(pthread_attr_setscope(attributes, PTHREAD_SCOPE_SYSTEM))
      fprintf(stderr, "AlsaAudioDevice: cannot set scheduling scope for RT thread\n");
    if(pthread_attr_setinheritsched(attributes, PTHREAD_EXPLICIT_SCHED))
      fprintf(stderr, "AlsaAudioDevice: cannot set setinheritsched for RT thread\n");

    struct sched_param rt_param;
    memset(&rt_param, 0, sizeof(rt_param));
    rt_param.sched_priority = priority;
    if(pthread_attr_setschedparam(attributes, &rt_param))
      fprintf(stderr, "AlsaAudioDevice: cannot set scheduling priority %d for RT thread (%s)\n",
              priority, strerror(errno));
  }

  int rv = pthread_create(&_thread, attributes, audioLoop, this);
  if(rv && attributes)
  {
    // Not allowed to run realtime? Try again without.
    fprintf(stderr, "AlsaAudioDevice: cannot create a realtime audio thread (%s), trying a normal thread\n", strerror(rv));
    rv = pthread_create(&_thread, nullptr, audioLoop, this);
  }

  if(attributes)
  {
    pthread_attr_destroy(attributes);
    free(attributes);
  }

  if(rv)
  {
    fprintf(stderr, "Error: AlsaAudioDevice: creating the audio thread failed: %s\n", strerror(rv));
    return false;
  }
  _threadCreated = true;
  return true;
}

void AlsaAudioDevice::stop()
{
  if(_threadCreated)
  {
    // The thread notices this after at most one period, since it blocks in the reads and writes.
    _quit = true;
    pthread_join(_thread, nullptr);
    _threadCreated = false;
  }
  if(_playback)
    snd_pcm_drop(_playback);
  if(_capture)
    snd_pcm_drop(_capture);
}

//---------------------------------------------------------
//   Ports
//---------------------------------------------------------

std::list<QString> AlsaAudioDevice::outputPorts(bool midi, int /*aliases*/)
{
  std::list<QString> list;
  if(!midi)
  {
    for(int i = 0; i < outputPortsList.size(); ++i)
      list.push_back(outputPortsList.at(i)->name);
  }
  return list;
}

std::list<QString> AlsaAudioDevice::inputPorts(bool midi, int /*aliases*/)
{
  std::list<QString> list;
  if(!midi)
  {
    for(int i = 0; i < inputPortsList.size(); ++i)
      list.push_back(inputPortsList.at(i)->name);
  }
  return list;
}

void* AlsaAudioDevice::registerOutPort(const char* name, bool midi)
{
  if(midi)
    return nullptr;

  for(int i = 0; i < outputPortsList.size(); ++i)
  {
    if(outputPortsList.at(i)->name == name)
      return outputPortsList.at(i);
  }

  MuseAlsaAudioPort* port = new MuseAlsaAudioPort();
  port->name = name;
  port->buffer = new float[MusEGlobal::segmentSize];
  memset(port->buffer, 0, MusEGlobal::segmentSize * sizeof(float));
  outputPortsList.push_back(port);
  return port;
}

void* AlsaAudioDevice::registerInPort(const char* name, bool midi)
{
  if(midi)
    return nullptr;

  for(int i = 0; i < inputPortsList.size(); ++i)
  {
    if(inputPortsList.at(i)->name == name)
      return inputPortsList.at(i);
  }

  MuseAlsaAudioPort* port = new MuseAlsaAudioPort();
  port->name = name;
  port->buffer = new float[MusEGlobal::segmentSize];
  memset(port->buffer, 0, MusEGlobal::segmentSize * sizeof(float));
  inputPortsList.push_back(port);
  return port;
}

//---------------------------------------------------------
//   exitAlsaAudio
//---------------------------------------------------------

void exitAlsaAudio()
{
  if(alsaAudioDevice)
  {
    // Don't leave a dangling pointer around.
    if(MusEGlobal::audioDevice == alsaAudioDevice)
      MusEGlobal::audioDevice = nullptr;
    delete alsaAudioDevice;
  }
  alsaAudioDevice = nullptr;
}

//---------------------------------------------------------
//   initAlsaAudio
//   Returns true on FAILURE, like initRtAudio is used.
//---------------------------------------------------------

bool initAlsaAudio()
{
  alsaAudioDevice = new AlsaAudioDevice();
  if(!alsaAudioDevice->open())
  {
    delete alsaAudioDevice;
    alsaAudioDevice = nullptr;
    return true;
  }
  MusEGlobal::audioDevice = alsaAudioDevice;
  return false;
}

} // namespace MusECore
