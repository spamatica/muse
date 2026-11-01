//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3editor.cpp
//  Step 2 proof of concept: VST3 plugin editor in a Qt5 widget on X11.
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#include "vst3editor.h"
#include "vst3host.h"

#include <QCloseEvent>
#include <QResizeEvent>
#include <QSocketNotifier>
#include <QTimer>
#include <list>
#include <memory>

using namespace Steinberg;

namespace MusEVst3 {

//---------------------------------------------------------
//   QtFrame
//   Implements IPlugFrame, and the Linux run loop that X11
//   plugin views use to get file descriptor and timer events,
//   on top of the Qt event loop.
//---------------------------------------------------------

class QtFrame : public FObject, public IPlugFrame, public Linux::IRunLoop
{
  public:
    explicit QtFrame(Editor* editor) : _editor(editor) {}
    ~QtFrame() override { clear(); }

    // Drops all registrations. Called before the view goes away.
    void clear()
    {
      _fds.clear();
      _timers.clear();
      _editor = nullptr;
    }

    // IPlugFrame
    tresult PLUGIN_API resizeView(IPlugView*, ViewRect* newSize) override
    {
      return _editor ? _editor->requestSize(newSize) : kResultFalse;
    }

    // Linux::IRunLoop
    tresult PLUGIN_API registerEventHandler(Linux::IEventHandler* handler, Linux::FileDescriptor fd) override
    {
      if(!handler)
        return kInvalidArgument;
      Fd e;
      e.handler = handler;
      e.fd = fd;
      e.notifier.reset(new QSocketNotifier(fd, QSocketNotifier::Read));
      Linux::IEventHandler* h = handler;
      QObject::connect(e.notifier.get(), &QSocketNotifier::activated, [h](int f) { h->onFDIsSet(f); });
      _fds.push_back(std::move(e));
      return kResultTrue;
    }

    tresult PLUGIN_API unregisterEventHandler(Linux::IEventHandler* handler) override
    {
      bool found = false;
      for(auto it = _fds.begin(); it != _fds.end();)
      {
        if(it->handler == handler)
        {
          it->notifier->setEnabled(false);
          it = _fds.erase(it);
          found = true;
        }
        else
          ++it;
      }
      return found ? kResultTrue : kResultFalse;
    }

    tresult PLUGIN_API registerTimer(Linux::ITimerHandler* handler, Linux::TimerInterval ms) override
    {
      if(!handler || ms == 0)
        return kInvalidArgument;
      Tm t;
      t.handler = handler;
      t.timer.reset(new QTimer);
      t.timer->setTimerType(Qt::CoarseTimer);
      Linux::ITimerHandler* h = handler;
      QObject::connect(t.timer.get(), &QTimer::timeout, [h]() { h->onTimer(); });
      t.timer->start(int(ms));
      _timers.push_back(std::move(t));
      return kResultTrue;
    }

    tresult PLUGIN_API unregisterTimer(Linux::ITimerHandler* handler) override
    {
      bool found = false;
      for(auto it = _timers.begin(); it != _timers.end();)
      {
        if(it->handler == handler)
        {
          it->timer->stop();
          it = _timers.erase(it);
          found = true;
        }
        else
          ++it;
      }
      return found ? kResultTrue : kResultFalse;
    }

    size_t fdCount() const { return _fds.size(); }
    size_t timerCount() const { return _timers.size(); }

    OBJ_METHODS(QtFrame, FObject)
    DEFINE_INTERFACES
      DEF_INTERFACE(IPlugFrame)
      DEF_INTERFACE(Linux::IRunLoop)
    END_DEFINE_INTERFACES(FObject)
    REFCOUNT_METHODS(FObject)

  private:
    struct Fd
    {
      IPtr<Linux::IEventHandler> handler;
      int fd = -1;
      std::unique_ptr<QSocketNotifier> notifier;
    };
    struct Tm
    {
      IPtr<Linux::ITimerHandler> handler;
      std::unique_ptr<QTimer> timer;
    };
    Editor* _editor;
    std::list<Fd> _fds;
    std::list<Tm> _timers;
};

//---------------------------------------------------------
//   Editor
//---------------------------------------------------------

Editor::Editor(Instance& inst, QWidget* parent) : QWidget(parent), _inst(inst)
{
  // The plugin needs a real X window to put its own window into.
  setAttribute(Qt::WA_NativeWindow);
  setAttribute(Qt::WA_DontCreateNativeAncestors);
  setAttribute(Qt::WA_DeleteOnClose, false);
}

Editor::~Editor()
{
  closeView();
}

bool Editor::open(std::string& error)
{
  if(_view)
    return true;

  IPtr<IPlugView> view = owned(_inst.createEditorView());
  if(!view)
  {
    error = "plugin has no editor";
    return false;
  }
  if(view->isPlatformTypeSupported(kPlatformTypeX11EmbedWindowID) != kResultTrue)
  {
    error = "editor does not support X11 embedding";
    return false;
  }

  ViewRect r {};
  if(view->getSize(&r) != kResultOk || r.getWidth() <= 0 || r.getHeight() <= 0)
  {
    r.left = r.top = 0;
    r.right = 640;
    r.bottom = 480;
  }
  if(view->canResize() == kResultTrue)
    setMinimumSize(32, 32);
  else
    setFixedSize(r.getWidth(), r.getHeight());
  _inResize = true;
  resize(r.getWidth(), r.getHeight());
  _inResize = false;

  _frame = owned(new QtFrame(this));
  view->setFrame(_frame);

  WId wid = winId();   // Creates the native window.
  if(view->attached(reinterpret_cast<void*>(wid), kPlatformTypeX11EmbedWindowID) != kResultTrue)
  {
    view->setFrame(nullptr);
    _frame->clear();
    _frame = nullptr;
    error = "IPlugView::attached failed";
    return false;
  }
  _view = view;
  return true;
}

void Editor::closeView()
{
  if(!_view)
    return;
  _view->removed();
  _view->setFrame(nullptr);
  _view = nullptr;
  if(_frame)
  {
    _frame->clear();
    _frame = nullptr;
  }
}

size_t Editor::runLoopTimers() const { return _frame ? _frame->timerCount() : 0; }
size_t Editor::runLoopFds() const    { return _frame ? _frame->fdCount() : 0; }

tresult Editor::requestSize(ViewRect* r)
{
  if(!r || !_view || _inResize)
    return kResultFalse;
  _inResize = true;
  const int w = r->getWidth(), h = r->getHeight();
  if(_view->canResize() == kResultTrue)
    resize(w, h);
  else
    setFixedSize(w, h);
  _view->onSize(r);
  _inResize = false;
  return kResultTrue;
}

void Editor::resizeEvent(QResizeEvent* e)
{
  QWidget::resizeEvent(e);
  if(!_view || _inResize || _view->canResize() != kResultTrue)
    return;
  ViewRect r(0, 0, width(), height());
  _inResize = true;
  _view->checkSizeConstraint(&r);
  _view->onSize(&r);
  _inResize = false;
  // The plugin may keep an aspect ratio or a step size. Make the window match what it accepted.
  if(r.getWidth() != width() || r.getHeight() != height())
  {
    const int w = r.getWidth(), h = r.getHeight();
    QTimer::singleShot(0, this, [this, w, h]() {
      if(!_view) return;
      _inResize = true;
      resize(w, h);
      _inResize = false;
    });
  }
}

void Editor::closeEvent(QCloseEvent* e)
{
  closeView();
  QWidget::closeEvent(e);
  if(_onClosed)
    _onClosed();
}

} // namespace MusEVst3
