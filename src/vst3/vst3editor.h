//=========================================================
//  MusE
//  Linux Music Editor
//
//  vst3editor.h
//  Step 2 proof of concept: a Qt5 widget that hosts a VST3 plugin's editor
//  (IPlugView) on X11, including the IRunLoop and IPlugFrame the plugin needs.
//
//  The widget is a top level window whose native X window is the parent
//  handed to the plugin ("X11EmbedWindowID"). Under Wayland this runs through
//  XWayland, so Qt must use the xcb platform plugin (QT_QPA_PLATFORM=xcb).
//
//  This program is free software; you can redistribute it and/or modify
//  it under the terms of the GNU General Public License
//  as published by the Free Software Foundation; version 2 of
//  the License, or (at your option) any later version.
//=========================================================

#ifndef __MUSE_VST3EDITOR_H__
#define __MUSE_VST3EDITOR_H__

#include <QWidget>
#include <functional>
#include <string>

#include "pluginterfaces/gui/iplugview.h"
#include "base/source/fobject.h"

namespace MusEVst3 {

class Instance;
class QtFrame;

class Editor : public QWidget
{
  public:
    explicit Editor(Instance& inst, QWidget* parent = nullptr);
    ~Editor() override;

    // Creates the plugin view and attaches it to this widget. Call before show().
    bool open(std::string& error);
    // Detaches and releases the plugin view. Also done by the destructor and on close.
    void closeView();
    bool isOpen() const { return _view != nullptr; }
    Steinberg::IPlugView* view() const { return _view; }

    // Called after the user closed the window (and the view was detached). Not called by closeView().
    void setCloseCallback(std::function<void()> cb) { _onClosed = std::move(cb); }

    // Number of timers and file descriptors the plugin has registered with the run loop.
    size_t runLoopTimers() const;
    size_t runLoopFds() const;

    // Called by the plugin (through IPlugFrame) when it wants another size.
    Steinberg::tresult requestSize(Steinberg::ViewRect* r);

  protected:
    void resizeEvent(QResizeEvent*) override;
    void closeEvent(QCloseEvent*) override;

  private:
    Instance& _inst;
    Steinberg::IPtr<Steinberg::IPlugView> _view;
    Steinberg::IPtr<QtFrame> _frame;
    bool _inResize = false;
    std::function<void()> _onClosed;
};

} // namespace MusEVst3

#endif
