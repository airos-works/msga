// The air/OS (Haiku) backend: the Be API under plat.
//
// Threads. plat wants one thread for everything (the one that called
// App::create()); the Be API gives every BWindow a thread of its own and the
// BApplication another. So the App thread runs a posix::PosixLoop (fd
// watches, timers, posted closures: the net layer's sockets plug straight
// in), the BApplication runs on a thread of its own, and every window thread
// turns what it receives into plat Events and posts them to the App thread.
// The other way, the App thread locks a window to change it; no window
// thread ever waits for the App thread, so that cannot deadlock.
//
// Pixels. Each window paints into a B_RGB32 BBitmap (0xAARRGGBB in memory on
// a little-endian CPU: plat's Canvas format as it is) that its view draws
// from: no copy between the canvas and the screen. On air/OS the desktop is
// drawn at its monitors' density ("render scale"): view coordinates stay
// logical and the bitmap is made in device pixels, which app_server puts on
// the screen one to one.
#pragma once

#include "core/backends.h"
#include "core/loop_core.h"
#include "posix/posix_loop.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <Application.h>
#include <Bitmap.h>
#include <Message.h>
#include <Messenger.h>
#include <Rect.h>
#include <View.h>
#include <Window.h>

namespace plat::haiku {

class HaikuApp;
class HaikuWindow;

// The application signature the package declares (msga.rdef).
inline constexpr const char *kSignature = "application/x-vnd.airos-msga";

// Messages between the threads.
enum : uint32 {
    kMsgPlatSetCursor = 'plCu',
    kMsgPlatAppQuit   = 'plQt',
};

// Device pixels per logical pixel: air/OS's display layout "render scale"
// (the largest of the enabled displays), 1 on a Haiku without it.
double deviceScale();
// The frame of the monitor most of `frame` is on (air/OS spans one desktop
// over every monitor); the main screen's elsewhere.
BRect  monitorFrame(BRect frame);

// Haiku modifier bits as plat's.
uint32_t modsFrom(uint32 haikuModifiers);
// A Haiku key code (the "key" of B_KEY_DOWN) as plat's Key, by position on
// a US keyboard; `rawChar` (the unmodified character the keymap gives) wins
// for letters, digits and punctuation, so shortcuts follow the layout.
Key      keyFrom(int32 code, int32 rawChar);

// A plat Image as a B_RGBA32 bitmap (premultiplied in, straight out).
BBitmap *bitmapFrom(const Image &image);

// ── The window ──────────────────────────────────────────────────────────────

class PlatView;

class PlatBWindow final : public BWindow {
public:
    PlatBWindow(HaikuWindow *owner, BRect frame, const WindowDesc &d);

    bool QuitRequested() override;
    void MessageReceived(BMessage *msg) override;
    void DispatchMessage(BMessage *msg, BHandler *target) override;
    void WindowActivated(bool active) override;
    void FrameMoved(BPoint where) override;
    void FrameResized(float w, float h) override;
    void Minimize(bool minimize) override;
    void Zoom(BPoint origin, float w, float h) override;
    void ScreenChanged(BRect frame, color_space mode) override;
    void WorkspaceActivated(int32 workspace, bool active) override;

    PlatView *view = nullptr;
    // Set on the App thread under the window's lock.
    bool      maximized = false;
    BRect     restoreFrame;

private:
    void post(Event e);

    HaikuApp *_app;
    uint64_t  _id;
};

class PlatView final : public BView {
public:
    PlatView(HaikuApp *app, uint64_t id, BRect frame);
    ~PlatView() override;

    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage *drag) override;
    void KeyDown(const char *bytes, int32 numBytes) override;
    void KeyUp(const char *bytes, int32 numBytes) override;
    void MessageReceived(BMessage *msg) override;
    void MakeFocus(bool focus) override;

    void keyEvent(BMessage *msg, bool down);
    void modifiersChanged(BMessage *msg);
    void wheel(BMessage *msg);
    void inputMethod(BMessage *msg);
    void dropped(BMessage *msg, BPoint where);
    void post(Event e);

    // The canvas, guarded by paintLock: the App thread paints into it while
    // this view may want to draw it.
    std::mutex paintLock;
    BBitmap   *bitmap = nullptr;
    double     scale  = 1.0; // of the bitmap
    // What Draw() could not draw because the App thread was painting; the
    // App thread invalidates it after endPaint. Guarded by missedLock.
    std::mutex missedLock;
    BRect      missed;

    // Text input state (App thread writes, window thread reads; under the
    // window lock).
    bool   textInput = false;
    BRect  caret;
    bool   imeActive = false;
    BMessenger imeReply;

private:
    HaikuApp *_app;
    uint64_t  _id;
    uint32    _buttons   = 0;
    uint32    _modifiers = 0;
    bool      _inside    = false;
    bool      _dragOver  = false;
};

class HaikuWindow final : public Window {
public:
    HaikuWindow(HaikuApp *app, uint64_t id, const WindowDesc &d);
    ~HaikuWindow() override;

    void   setTitle(std::string_view utf8) override;
    Size   size() const override { return _size; }
    double scale() const override { return _scale; }
    void   setSize(Size logical) override;
    void   setMinSize(Size logical) override;
    void   show() override;
    void   hide() override;
    void   minimize() override;
    void   setMaximized(bool on) override;
    void   setFullscreen(bool on) override;
    bool   isMaximized() const override { return _maximized; }
    bool   isFullscreen() const override { return _fullscreen; }
    bool   isMinimized() const override { return _minimized; }
    bool   isActive() const override { return _active; }
    void   activate() override;
    bool   supportsAlwaysOnTop() const override { return true; }
    void   setAlwaysOnTop(bool on) override;
    bool   isAlwaysOnTop() const override { return _above; }
    std::optional<Point> position() const override { return _pos; }
    bool                 setPosition(Point logical) override;
    uint64_t             monitor() const override { return 1; }
    void                 setCursor(Cursor c) override;
    void setHitTest(std::function<HitArea(Point)> fn) override { _hitTest = std::move(fn); }
    // Always the native window tab: air/OS draws the title bar.
    bool hasSystemDecorations() const override { return true; }
    void setTextInput(const TextInputState &s) override;
    void requestFrame() override { _framePending = true; }
    Canvas beginPaint() override;
    void   endPaint(const std::vector<Rect> &damage) override;
    void   setDropAction(DropAction a) override { dropAction = a; }
    void   requestAttention() override;
    void  *nativeHandle() const override { return _win; }

    // From the window thread, on the App thread.
    void handle(const Event &e);
    bool frameReady(core::Clock::time_point now, core::Clock::time_point *notBefore) const;
    void sendFrame(core::Clock::time_point now);
    void emit(const Event &e);

    DropAction dropAction = DropAction::Copy;

private:
    HaikuApp                      *_app;
    uint64_t                       _id;
    PlatBWindow                   *_win  = nullptr;
    PlatView                      *_view = nullptr;
    Size                           _size;
    double                         _scale = 1.0;
    std::optional<Point>           _pos;
    bool                           _visible = false, _shown = false;
    bool                           _maximized = false, _fullscreen = false, _minimized = false;
    bool                           _active = false, _above = false;
    bool                           _framePending = false, _painting = false;
    core::Clock::time_point        _lastFrame{};
    BRect                          _beforeFullscreen;
    std::function<HitArea(Point)>  _hitTest;
};

// ── The tray: MSGA's Deskbar item (deskbar_tray.cpp) ───────────────────────

class HaikuTray final : public Tray {
public:
    explicit HaikuTray(HaikuApp *app);
    ~HaikuTray() override;

    void setIcon(const std::vector<Image> &sizes) override;
    void setTooltip(std::string_view utf8) override;
    void setMenu(std::vector<MenuItem> items) override;
    // While the Deskbar item is there and has said hello.
    bool isVisible() const override { return _item.IsValid(); }

    // The item said hello (App thread): it gets everything set so far.
    void attach(BMessenger item);
    // Deskbar took the item out (App thread): add it again when it can.
    void detached();

private:
    void send(BMessage *msg);

    HaikuApp  *_app;
    BMessenger _item;
    BMessage   _icon, _tooltip, _menu;
    TimerId    _readd      = 0;
    int        _readdTries = 0;
};

// ── The app ─────────────────────────────────────────────────────────────────

class PlatBApplication final : public BApplication {
public:
    PlatBApplication(HaikuApp *owner, status_t *error);

    bool QuitRequested() override;
    void ArgvReceived(int32 argc, char **argv) override;
    void RefsReceived(BMessage *msg) override;
    void MessageReceived(BMessage *msg) override;
    void ReadyToRun() override;

    // Set by ~HaikuApp: QuitRequested() lets the application go.
    std::atomic<bool> leaving{false};

private:
    HaikuApp *_owner;
};

class HaikuApp final : public BackendApp {
public:
    HaikuApp();
    ~HaikuApp() override;
    bool start(std::string *error);

    const char *backendName() const override { return "haiku"; }

    std::unique_ptr<Window> createWindow(const WindowDesc &desc) override;
    void                    run() override;
    void                    quit() override { _loop.quit(); }
    void                    pump(int timeoutMs) override { _loop.iterate(timeoutMs); }
    void                    post(std::function<void()> fn) override {
        _loop.core.post(std::move(fn));
    }
    TimerId addTimer(int ms, bool repeat, std::function<void()> fn) override {
        return _loop.core.addTimer(ms, repeat, std::move(fn));
    }
    void     cancelTimer(TimerId id) override { _loop.core.cancelTimer(id); }
    uint64_t watchFd(int fd, uint32_t ev, std::function<void(uint32_t)> fn) override {
        return _loop.watch(fd, ev, std::move(fn));
    }
    void unwatchFd(uint64_t id) override { _loop.unwatch(id); }

    void setClipboard(std::vector<DataItem> items, Selection sel) override;
    void requestClipboard(
        std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
    ) override;
    void requestClipboardMimes(std::function<void(std::vector<std::string>)> cb, Selection sel)
        override;

    std::unique_ptr<Tray> createTray() override;
    bool                  notificationsAvailable() const override { return true; }
    uint64_t              notify(const Notification &n) override;

    std::vector<Monitor> monitors() const override;
    bool claimSingleInstance(std::string_view key, const std::vector<std::string> &args) override;
    bool registerUrlScheme(std::string_view scheme) override;
    std::optional<bool> networkOnline() const override;

    void showFileDialog(const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb)
        override;
    std::string              standardDir(StandardDir d) const override;
    SystemSettings           systemSettings() const override;
    std::vector<std::string> preferredLanguages() const override;
    bool                     darkMode() const override;
    int                      doubleClickMs() const override;
    bool                     openUrl(std::string_view url) override;

    // Posted from the window and application threads.
    void fromWindow(uint64_t id, Event e);
    void fromApplication(Event e);
    void forget(uint64_t id) { _windows.erase(id); }
    int  frameIntervalMs() const;
    void networkChanged();
    void notificationClicked(uint64_t id);
    // From the Deskbar item, on the App thread.
    void trayMessage(const BMessage &msg);
    HaikuTray *tray = nullptr; // the one tray, while it exists

    using App::emit;

private:
    bool beforeWait();
    void emitFrames();

    posix::PosixLoop                    _loop;
    PlatBApplication                   *_bapp      = nullptr;
    thread_id                           _appThread = -1;
    bool                                _secondary = false;
    std::map<uint64_t, HaikuWindow *>   _windows;
    uint64_t                            _nextWindow = 1;
    std::vector<uint64_t>               _frameIds;
    TimerId                             _frameTimer = 0;
    std::vector<std::string>            _schemes;
    std::optional<bool>                 _online;
    uint64_t                            _nextNotification = 1;

    friend class PlatBApplication;
};

} // namespace plat::haiku
