// The Haiku backend's windows: a BWindow with one BView that draws the
// canvas bitmap and turns input into plat Events (see haiku_internal.h for
// the threading).
#include "haiku/haiku_internal.h"

#include "core/drop.h"
#include "core/input.h"
#include "core/pacing.h"
#include "core/transfer.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <dlfcn.h>

#include <Cursor.h>
#include <Entry.h>
#include <Input.h>
#include <InterfaceDefs.h>
#include <Path.h>
#include <Region.h>
#include <Screen.h>

namespace plat::haiku {

namespace {

// Events the window threads post carry what plat's Event has no field for
// in fields the type leaves unused: a size in dx/dy (Resized), a minimised
// flag in `repeat` (StateChanged).
constexpr uint32 kWindowFlags = B_ASYNCHRONOUS_CONTROLS | B_NOT_ANCHORED_ON_ACTIVATE;

int32 utf8Count(const char *s) {
    int32 n = 0;
    for (; *s; ++s)
        n += (uint8(*s) & 0xc0) != 0x80;
    return n;
}

} // namespace

// ── Helpers ─────────────────────────────────────────────────────────────────

double deviceScale() {
    // BPrivate::get_display_layout (air/OS's app_server): every display with
    // its "render scale" in percent. Looked up at run time so the same
    // binary runs on a Haiku without it.
    using GetLayout             = status_t (*)(BMessage &);
    static GetLayout getLayout = reinterpret_cast<GetLayout>(
        dlsym(RTLD_DEFAULT, "_ZN8BPrivate18get_display_layoutER8BMessage")
    );
    if (!getLayout)
        return 1.0;
    BMessage layout;
    if (getLayout(layout) != B_OK)
        return 1.0;
    int32    best = 0;
    BMessage display;
    for (int32 i = 0; layout.FindMessage("display", i, &display) == B_OK; ++i) {
        int32 scale = 0;
        if (display.GetBool("enabled", true) && display.FindInt32("render scale", &scale) == B_OK &&
            scale > best)
            best = scale;
    }
    return best >= 100 && best <= 400 ? best / 100.0 : 1.0;
}

BRect monitorFrame(BRect frame) {
    using GetDisplayFrame          = status_t (*)(BRect, bool, BRect &);
    static GetDisplayFrame getFrame = reinterpret_cast<GetDisplayFrame>(
        dlsym(RTLD_DEFAULT, "_ZN8BPrivate17get_display_frameE5BRectbRS0_")
    );
    BRect display;
    if (getFrame && getFrame(frame, false, display) == B_OK && display.IsValid())
        return display;
    return BScreen(B_MAIN_SCREEN_ID).Frame();
}

uint32_t modsFrom(uint32 m) {
    uint32_t out = 0;
    if (m & B_SHIFT_KEY)
        out |= ModShift;
    if (m & B_CONTROL_KEY)
        out |= ModCtrl;
    // Command (the Alt key by default) is the shortcut key, as on macOS;
    // Option (the Windows key) is plat's Alt.
    if (m & B_COMMAND_KEY)
        out |= ModSuper;
    if (m & B_OPTION_KEY)
        out |= ModAlt;
    if (m & B_CAPS_LOCK)
        out |= ModCaps;
    if (m & B_NUM_LOCK)
        out |= ModNum;
    return out;
}

Key keyFrom(int32 code, int32 rawChar) {
    using K = Key;
    // Haiku's key codes (Keyboard preferences, the "key" field), by position.
    static const K kTable[0x6b] = {
        K::Unknown,      K::Escape,       K::F1,           K::F2,          K::F3,
        K::F4,           K::F5,           K::F6,           K::F7,          K::F8,
        K::F9,           K::F10,          K::F11,          K::F12,         K::PrintScreen,
        K::ScrollLock,   K::Pause,        K::Grave,        K::Num1,        K::Num2,
        K::Num3,         K::Num4,         K::Num5,         K::Num6,        K::Num7,
        K::Num8,         K::Num9,         K::Num0,         K::Minus,       K::Equal,
        K::Backspace,    K::Insert,       K::Home,         K::PageUp,      K::NumLock,
        K::KpDivide,     K::KpMultiply,   K::KpSubtract,   K::Tab,         K::Q,
        K::W,            K::E,            K::R,            K::T,           K::Y,
        K::U,            K::I,            K::O,            K::P,           K::BracketLeft,
        K::BracketRight, K::Backslash,    K::Delete,       K::End,         K::PageDown,
        K::Kp7,          K::Kp8,          K::Kp9,          K::KpAdd,       K::CapsLock,
        K::A,            K::S,            K::D,            K::F,           K::G,
        K::H,            K::J,            K::K,            K::L,           K::Semicolon,
        K::Apostrophe,   K::Enter,        K::Kp4,          K::Kp5,         K::Kp6,
        K::ShiftLeft,    K::Z,            K::X,            K::C,           K::V,
        K::B,            K::N,            K::M,            K::Comma,       K::Period,
        K::Slash,        K::ShiftRight,   K::Up,           K::Kp1,         K::Kp2,
        K::Kp3,          K::KpEnter,      K::ControlLeft,  K::AltLeft,     K::Space,
        K::AltRight,     K::ControlRight, K::Left,         K::Down,        K::Right,
        K::Kp0,          K::KpDecimal,    K::SuperLeft,    K::SuperRight,  K::Menu,
        K::Backslash,    K::KpEqual,
    };
    const K pos = code > 0 && code < int32(std::size(kTable)) ? kTable[code] : K::Unknown;
    // Letters, digits and punctuation follow the layout (what the key types
    // unmodified), so Ctrl+Z is the key with z on it; a non-Latin character
    // keeps the US position, as GTK does.
    const bool typing = (pos >= K::A && pos <= K::Num9) ||
                        (pos >= K::Minus && pos <= K::Slash);
    if (typing && rawChar > 0x20 && rawChar < 0x7f) {
        const K k = core::keyFromAscii(uint32_t(rawChar));
        if (k != K::Unknown)
            return k;
    }
    return pos;
}

BBitmap *bitmapFrom(const Image &image) {
    if (image.empty() || image.pixels.size() < size_t(image.width) * image.height)
        return nullptr;
    auto *b = new BBitmap(BRect(0, 0, image.width - 1, image.height - 1), B_RGBA32);
    if (b->InitCheck() != B_OK) {
        delete b;
        return nullptr;
    }
    auto     *bits = static_cast<uint8 *>(b->Bits());
    const int bpr  = b->BytesPerRow();
    for (int y = 0; y < image.height; ++y) {
        auto *row = reinterpret_cast<uint32_t *>(bits + size_t(y) * bpr);
        for (int x = 0; x < image.width; ++x) {
            // B_RGBA32 is straight alpha.
            const uint32_t p = image.pixels[size_t(y) * image.width + x];
            const uint32_t a = p >> 24;
            if (a == 0 || a == 255) {
                row[x] = a ? p : 0;
                continue;
            }
            auto ch = [&](int s) { return std::min<uint32_t>(255, (((p >> s) & 255) * 255 + a / 2) / a); };
            row[x] = (a << 24) | (ch(16) << 16) | (ch(8) << 8) | ch(0);
        }
    }
    return b;
}

// ── PlatBWindow ─────────────────────────────────────────────────────────────

PlatBWindow::PlatBWindow(HaikuWindow *owner, BRect frame, const WindowDesc &d)
    : BWindow(
          frame,
          d.title.c_str(),
          B_TITLED_WINDOW_LOOK,
          B_NORMAL_WINDOW_FEEL,
          kWindowFlags | (d.resizable ? 0 : B_NOT_RESIZABLE | B_NOT_ZOOMABLE)
      ),
      _app(nullptr), _id(0) {
    (void)owner;
}

void PlatBWindow::post(Event e) {
    if (view)
        view->post(std::move(e));
}

bool PlatBWindow::QuitRequested() {
    // The close button: the app decides (it may hide to the tray instead).
    post({.type = EventType::CloseRequested});
    return false;
}

void PlatBWindow::DispatchMessage(BMessage *msg, BHandler *target) {
    if (view) {
        if (msg->what == B_KEY_DOWN || msg->what == B_UNMAPPED_KEY_DOWN) {
            // BWindow eats every Command+key it has no shortcut for (and
            // turns Command+X/C/V/A into editing messages for a BTextView);
            // the app's own shortcuts need them all. Command+Q asks the app
            // to quit, as it does everywhere on air/OS.
            int32 mods = 0, raw = 0;
            msg->FindInt32("modifiers", &mods);
            msg->FindInt32("raw_char", &raw);
            if (mods & B_COMMAND_KEY) {
                if ((raw == 'q' || raw == 'Q') && !(mods & (B_SHIFT_KEY | B_CONTROL_KEY))) {
                    if (be_app)
                        be_app->PostMessage(B_QUIT_REQUESTED);
                    return;
                }
                view->keyEvent(msg, true);
                return;
            }
        } else if (msg->what == B_MODIFIERS_CHANGED) {
            view->modifiersChanged(msg);
        } else if (msg->what == B_MOUSE_WHEEL_CHANGED) {
            view->wheel(msg);
            return;
        }
    }
    BWindow::DispatchMessage(msg, target);
}

void PlatBWindow::MessageReceived(BMessage *msg) {
    switch (msg->what) {
    case B_COLORS_UPDATED:
        post({.type = EventType::ThemeChanged});
        break;
    case kMsgPlatSetCursor:
        break;
    default:
        BWindow::MessageReceived(msg);
    }
}

void PlatBWindow::WindowActivated(bool active) {
    BWindow::WindowActivated(active);
    post({.type = active ? EventType::FocusIn : EventType::FocusOut});
}

void PlatBWindow::FrameMoved(BPoint where) {
    post({.type = EventType::Moved, .pos = {where.x, where.y}});
}

void PlatBWindow::FrameResized(float w, float h) {
    post({.type = EventType::Resized, .dx = std::floor(w) + 1, .dy = std::floor(h) + 1});
}

void PlatBWindow::Minimize(bool minimize) {
    BWindow::Minimize(minimize);
    post({.type = EventType::StateChanged, .repeat = minimize});
}

void PlatBWindow::Zoom(BPoint origin, float w, float h) {
    // The tab's zoom button and setMaximized(): toggle between the frame the
    // window had and the screen (BWindow's own Zoom() works out that size).
    if (maximized) {
        maximized = false;
        MoveTo(restoreFrame.LeftTop());
        ResizeTo(restoreFrame.Width(), restoreFrame.Height());
    } else {
        restoreFrame = Frame();
        maximized    = true;
        BWindow::Zoom(origin, w, h);
    }
    post({.type = EventType::StateChanged, .cancelled = true});
}

void PlatBWindow::ScreenChanged(BRect frame, color_space mode) {
    BWindow::ScreenChanged(frame, mode);
    // The render scale may have changed with the mode: Resized re-reads it.
    const BRect b = Bounds();
    post({.type = EventType::Resized, .dx = std::floor(b.Width()) + 1, .dy = std::floor(b.Height()) + 1});
    post({.type = EventType::MonitorsChanged});
}

void PlatBWindow::WorkspaceActivated(int32 workspace, bool active) {
    BWindow::WorkspaceActivated(workspace, active);
}

// ── PlatView ────────────────────────────────────────────────────────────────

PlatView::PlatView(HaikuApp *app, uint64_t id, BRect frame)
    : BView(frame, "canvas", B_FOLLOW_ALL_SIDES, B_WILL_DRAW | B_INPUT_METHOD_AWARE),
      _app(app), _id(id) {
    // No erase before Draw: the canvas covers the view.
    SetViewColor(B_TRANSPARENT_COLOR);
    SetDrawingMode(B_OP_COPY);
}

PlatView::~PlatView() {
    delete bitmap;
}

void PlatView::post(Event e) {
    _app->post([app = _app, id = _id, e = std::move(e)]() mutable { app->fromWindow(id, std::move(e)); });
}

void PlatView::Draw(BRect update) {
    // The App thread may be painting the next frame: wait a little for it,
    // never long (it may be waiting for this window's lock), and leave what
    // could not be drawn to the invalidation after its endPaint.
    std::unique_lock<std::mutex> lock(paintLock, std::try_to_lock);
    for (int i = 0; !lock.owns_lock() && i < 20; ++i) {
        snooze(1000);
        (void)lock.try_lock();
    }
    if (!lock.owns_lock()) {
        std::lock_guard<std::mutex> m(missedLock);
        missed = missed.IsValid() ? (missed | update) : update;
        return;
    }
    BRect painted;
    if (bitmap) {
        const BRect b = bitmap->Bounds();
        BRect logical(0, 0, (b.right + 1) / scale - 1, (b.bottom + 1) / scale - 1);
        painted = logical & update;
        if (painted.IsValid()) {
            BRect source(
                painted.left * scale,
                painted.top * scale,
                (painted.right + 1) * scale - 1,
                (painted.bottom + 1) * scale - 1
            );
            DrawBitmapAsync(bitmap, source & b, painted);
        }
    }
    // The window grew and the next frame is not there yet: the panel colour,
    // not garbage.
    BRegion rest(update);
    if (painted.IsValid())
        rest.Exclude(painted);
    if (rest.CountRects() > 0) {
        SetHighColor(ui_color(B_PANEL_BACKGROUND_COLOR));
        FillRegion(&rest);
    }
}

namespace {

Button buttonOf(uint32 bit) {
    switch (bit) {
    case B_SECONDARY_MOUSE_BUTTON:
        return Button::Right;
    case B_TERTIARY_MOUSE_BUTTON:
        return Button::Middle;
    case 8:
        return Button::Back;
    case 16:
        return Button::Forward;
    default:
        return Button::Left;
    }
}

} // namespace

void PlatView::MouseDown(BPoint where) {
    BMessage *msg     = Window()->CurrentMessage();
    int32     buttons = 0, clicks = 1, mods = 0;
    if (msg) {
        msg->FindInt32("buttons", &buttons);
        msg->FindInt32("clicks", &clicks);
        msg->FindInt32("modifiers", &mods);
    }
    if (!IsFocus())
        MakeFocus(true);
    // Keep the pointer while a button is down, like every other backend.
    SetMouseEventMask(B_POINTER_EVENTS, B_NO_POINTER_HISTORY);
    const uint32 pressed = uint32(buttons) & ~_buttons;
    _buttons             = uint32(buttons);
    for (uint32 bit = 1; bit <= 16; bit <<= 1)
        if (pressed & bit)
            post(
                {.type   = EventType::PointerDown,
                 .pos    = {where.x, where.y},
                 .button = buttonOf(bit),
                 .clicks = std::max<int32>(1, clicks),
                 .mods   = modsFrom(uint32(mods))}
            );
}

void PlatView::MouseUp(BPoint where) {
    BMessage *msg     = Window()->CurrentMessage();
    int32     buttons = 0, mods = int32(modifiers());
    if (msg) {
        msg->FindInt32("buttons", &buttons);
        msg->FindInt32("modifiers", &mods);
    }
    const uint32 released = _buttons & ~uint32(buttons);
    _buttons              = uint32(buttons);
    for (uint32 bit = 1; bit <= 16; bit <<= 1)
        if (released & bit)
            post(
                {.type   = EventType::PointerUp,
                 .pos    = {where.x, where.y},
                 .button = buttonOf(bit),
                 .mods   = modsFrom(uint32(mods))}
            );
}

void PlatView::MouseMoved(BPoint where, uint32 transit, const BMessage *drag) {
    const uint32_t mods = modsFrom(modifiers());
    if (drag && !_buttons) {
        // Another app's (or our own) drag passing over: the drop target side.
        if (transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW) {
            if (_dragOver) {
                _dragOver = false;
                post({.type = EventType::DropLeave});
            }
            return;
        }
        Event e{.type = _dragOver ? EventType::DropMove : EventType::DropEnter, .pos = {where.x, where.y}};
        e.mods           = mods;
        e.allowedActions = ActCopy;
        e.dropAction     = DropAction::Copy;
        if (!_dragOver) {
            if (drag->HasRef("refs"))
                e.items.push_back({"text/uri-list", {}});
            char      *name = nullptr;
            type_code  type = 0;
            for (int32 i = 0; drag->GetInfo(B_MIME_TYPE, i, &name, &type) == B_OK; ++i)
                e.items.push_back({std::strcmp(name, "text/plain") == 0 ? core::kTextMime : name, {}});
        }
        _dragOver = true;
        post(std::move(e));
        return;
    }
    if (_dragOver) {
        _dragOver = false;
        post({.type = EventType::DropLeave});
    }
    if (transit == B_ENTERED_VIEW) {
        _inside = true;
        post({.type = EventType::PointerEnter, .pos = {where.x, where.y}, .mods = mods});
    } else if (transit == B_EXITED_VIEW && !_buttons) {
        _inside = false;
        post({.type = EventType::PointerMove, .pos = {where.x, where.y}, .mods = mods});
        post({.type = EventType::PointerLeave, .pos = {where.x, where.y}, .mods = mods});
        return;
    }
    post({.type = EventType::PointerMove, .pos = {where.x, where.y}, .mods = mods});
}

void PlatView::wheel(BMessage *msg) {
    float dx = 0, dy = 0;
    msg->FindFloat("be:wheel_delta_x", &dx);
    msg->FindFloat("be:wheel_delta_y", &dy);
    BPoint   where;
    uint32   buttons = 0;
    GetMouse(&where, &buttons, false);
    uint32_t mods = modsFrom(modifiers());
    // Shift turns a vertical wheel sideways, as elsewhere.
    if ((mods & ModShift) && dx == 0) {
        dx = dy;
        dy = 0;
    }
    post({.type = EventType::Scroll, .pos = {where.x, where.y}, .dx = dx, .dy = dy, .mods = mods});
}

void PlatView::KeyDown(const char *, int32) {
    if (BMessage *msg = Window()->CurrentMessage())
        keyEvent(msg, true);
}

void PlatView::KeyUp(const char *, int32) {
    if (BMessage *msg = Window()->CurrentMessage())
        keyEvent(msg, false);
}

void PlatView::keyEvent(BMessage *msg, bool down) {
    int32       code = 0, mods = 0, raw = 0, repeat = 0;
    const char *bytes = nullptr;
    msg->FindInt32("key", &code);
    msg->FindInt32("modifiers", &mods);
    msg->FindInt32("raw_char", &raw);
    msg->FindInt32("be:key_repeat", &repeat);
    msg->FindString("bytes", &bytes);
    const Key key = keyFrom(code, raw);
    post(
        {.type     = down ? EventType::KeyDown : EventType::KeyUp,
         .key      = key,
         .scancode = uint32_t(code),
         .mods     = modsFrom(uint32(mods)),
         .repeat   = repeat > 0}
    );
    // Typed characters: printable ones (the keymap's dead keys and Option
    // combinations included), not the control codes Haiku gives Enter, Tab,
    // the arrows and Ctrl+letters, nor shortcuts.
    if (down && textInput && bytes && uint8(bytes[0]) >= 0x20 && bytes[0] != 0x7f &&
        !(mods & (B_COMMAND_KEY | B_CONTROL_KEY)))
        post({.type = EventType::TextInput, .text = bytes});
}

void PlatView::modifiersChanged(BMessage *msg) {
    int32 now = 0, old = 0;
    msg->FindInt32("modifiers", &now);
    msg->FindInt32("be:old_modifiers", &old);
    struct Bit {
        uint32 mask;
        Key    key;
        int32  code;
    };
    static const Bit kBits[] = {
        {B_LEFT_SHIFT_KEY, Key::ShiftLeft, 0x4b},     {B_RIGHT_SHIFT_KEY, Key::ShiftRight, 0x56},
        {B_LEFT_CONTROL_KEY, Key::ControlLeft, 0x5c}, {B_RIGHT_CONTROL_KEY, Key::ControlRight, 0x60},
        {B_LEFT_COMMAND_KEY, Key::AltLeft, 0x5d},     {B_RIGHT_COMMAND_KEY, Key::AltRight, 0x5f},
        {B_LEFT_OPTION_KEY, Key::SuperLeft, 0x66},    {B_RIGHT_OPTION_KEY, Key::SuperRight, 0x67},
        {B_CAPS_LOCK, Key::CapsLock, 0x3b},
    };
    const uint32 changed = uint32(now ^ old);
    for (const Bit &b : kBits)
        if (changed & b.mask)
            post(
                {.type     = (now & b.mask) ? EventType::KeyDown : EventType::KeyUp,
                 .key      = b.key,
                 .scancode = uint32_t(b.code),
                 .mods     = modsFrom(uint32(now))}
            );
    _modifiers = uint32(now);
}

void PlatView::inputMethod(BMessage *msg) {
    int32 opcode = 0;
    msg->FindInt32("be:opcode", &opcode);
    switch (opcode) {
    case B_INPUT_METHOD_STARTED:
        imeActive = true;
        msg->FindMessenger("be:reply_to", &imeReply);
        break;
    case B_INPUT_METHOD_STOPPED:
        imeActive = false;
        imeReply  = BMessenger();
        post({.type = EventType::TextPreedit});
        break;
    case B_INPUT_METHOD_CHANGED: {
        const char *s         = nullptr;
        bool        confirmed = false;
        msg->FindString("be:string", &s);
        msg->FindBool("be:confirmed", &confirmed);
        const std::string text = s ? s : "";
        if (confirmed) {
            post({.type = EventType::TextPreedit});
            if (!text.empty())
                post({.type = EventType::TextInput, .text = text});
        } else {
            int32 b = -1, e = -1;
            msg->FindInt32("be:selection", 0, &b);
            msg->FindInt32("be:selection", 1, &e);
            Event ev{.type = EventType::TextPreedit, .text = text};
            ev.preeditCursorBegin = b;
            ev.preeditCursorEnd   = e;
            post(std::move(ev));
        }
        break;
    }
    case B_INPUT_METHOD_LOCATION_REQUEST: {
        if (!imeReply.IsValid())
            break;
        // Where the candidate window goes: one place per character of the
        // composition, all at the caret (plat knows no more).
        BMessage reply(B_INPUT_METHOD_EVENT);
        reply.AddInt32("be:opcode", B_INPUT_METHOD_LOCATION_REQUEST);
        const BPoint at = ConvertToScreen(caret.LeftTop());
        const char  *s  = nullptr;
        msg->FindString("be:string", &s);
        const int32 n = std::max<int32>(1, s ? utf8Count(s) : 1);
        for (int32 i = 0; i < n; ++i)
            reply.AddPoint("be:location_reply", at);
        reply.AddFloat("be:height_reply", std::max(12.f, caret.Height() + 1));
        imeReply.SendMessage(&reply);
        break;
    }
    }
}

void PlatView::MakeFocus(bool focus) {
    BView::MakeFocus(focus);
}

void PlatView::MessageReceived(BMessage *msg) {
    if (msg->WasDropped()) {
        BPoint where = msg->DropPoint();
        ConvertFromScreen(&where);
        dropped(msg, where);
        return;
    }
    switch (msg->what) {
    case B_INPUT_METHOD_EVENT:
        inputMethod(msg);
        break;
    case B_MOUSE_WHEEL_CHANGED:
        wheel(msg);
        break;
    default:
        BView::MessageReceived(msg);
    }
}

void PlatView::dropped(BMessage *msg, BPoint where) {
    _dragOver = false;
    Event e{.type = EventType::Drop, .pos = {where.x, where.y}};
    e.mods           = modsFrom(modifiers());
    e.allowedActions = ActCopy;
    e.dropAction     = DropAction::Copy;
    // Files (Tracker): refs, as a text/uri-list of file:// URIs.
    entry_ref   ref;
    std::string list;
    for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; ++i) {
        BPath path;
        if (BEntry(&ref, true).GetPath(&path) == B_OK) {
            list += core::fileUri(path.Path());
            list += "\r\n";
        }
    }
    if (!list.empty())
        e.items.push_back({"text/uri-list", std::move(list)});
    // Data in the message itself (text from another app, a picture).
    char     *name = nullptr;
    type_code type = 0;
    for (int32 i = 0; msg->GetInfo(B_MIME_TYPE, i, &name, &type) == B_OK; ++i) {
        const void *data = nullptr;
        ssize_t     size = 0;
        if (msg->FindData(name, B_MIME_TYPE, &data, &size) != B_OK)
            continue;
        const std::string mime = std::strcmp(name, "text/plain") == 0 ? core::kTextMime : name;
        if (core::readOnDrop(mime))
            e.items.push_back({mime, std::string(static_cast<const char *>(data), size_t(size))});
    }
    core::fillDropText(e);
    post(std::move(e));
}

// ── HaikuWindow ─────────────────────────────────────────────────────────────

HaikuWindow::HaikuWindow(HaikuApp *app, uint64_t id, const WindowDesc &d)
    : _app(app), _id(id), _size(d.size) {
    _scale = deviceScale();
    const float w = float(std::max(1, d.size.w)), h = float(std::max(1, d.size.h));
    BRect       frame(0, 0, w - 1, h - 1);
    if (d.position)
        frame.OffsetTo(float(d.position->x), float(d.position->y));
    _win = new PlatBWindow(this, frame, d);
    _view = new PlatView(app, id, _win->Bounds());
    _view->scale = _scale;
    if (_win->Lock()) {
        _win->view = _view;
        _win->AddChild(_view);
        _view->MakeFocus(true);
        if (d.minSize.w > 0 || d.minSize.h > 0)
            _win->SetSizeLimits(
                float(std::max(1, d.minSize.w)) - 1, 32768, float(std::max(1, d.minSize.h)) - 1, 32768
            );
        if (!d.position) {
            // Centred on the main monitor, the tab and borders on screen.
            BRect m = monitorFrame(_win->Frame());
            m.InsetBy(5, 0);
            m.top += 27;
            m.bottom -= 5;
            _win->MoveTo(
                m.left + std::max(0.f, (m.Width() - w) / 2), m.top + std::max(0.f, (m.Height() - h) / 2)
            );
        }
        _pos = Point{_win->Frame().left, _win->Frame().top};
        _win->Unlock();
    }
    if (d.visible)
        show();
}

HaikuWindow::~HaikuWindow() {
    _app->forget(_id);
    if (_win && _win->Lock())
        _win->Quit(); // deletes the window and its view
}

void HaikuWindow::emit(const Event &e) {
    Event out  = e;
    out.window = this;
    _app->emit(out);
}

void HaikuWindow::handle(const Event &e) {
    switch (e.type) {
    case EventType::Resized: {
        const Size   s{int(e.dx), int(e.dy)};
        const double sc = deviceScale();
        if (s.w == _size.w && s.h == _size.h && sc == _scale)
            return;
        _size  = s;
        _scale = sc;
        emit({.type = EventType::Resized});
        _framePending = true;
        return;
    }
    case EventType::Moved:
        _pos = e.pos;
        emit({.type = EventType::Moved});
        return;
    case EventType::FocusIn:
    case EventType::FocusOut:
        _active = e.type == EventType::FocusIn;
        emit({.type = e.type});
        emit({.type = EventType::StateChanged});
        return;
    case EventType::StateChanged:
        if (e.cancelled) { // the zoom button
            if (_win->Lock()) {
                _maximized = _win->maximized;
                _win->Unlock();
            }
        } else {
            _minimized = e.repeat;
            if (!_minimized)
                _framePending = true;
        }
        emit({.type = EventType::StateChanged});
        return;
    default: {
        Event out = e;
        emit(out);
    }
    }
}

void HaikuWindow::setTitle(std::string_view utf8) {
    if (_win->Lock()) {
        _win->SetTitle(std::string(utf8).c_str());
        _win->Unlock();
    }
}

void HaikuWindow::setSize(Size s) {
    if (_win->Lock()) {
        _win->ResizeTo(float(std::max(1, s.w)) - 1, float(std::max(1, s.h)) - 1);
        _win->Unlock();
    }
}

void HaikuWindow::setMinSize(Size s) {
    if (_win->Lock()) {
        _win->SetSizeLimits(
            float(std::max(1, s.w)) - 1, 32768, float(std::max(1, s.h)) - 1, 32768
        );
        _win->Unlock();
    }
}

void HaikuWindow::show() {
    if (_visible)
        return;
    _visible = true;
    // Show/Hide nest: one Show for each Hide, and the first one starts the
    // window's thread.
    _win->Show();
    _shown        = true;
    _framePending = true;
}

void HaikuWindow::hide() {
    if (!_visible)
        return;
    _visible = false;
    if (_win->Lock()) {
        _win->Hide();
        _win->Unlock();
    }
}

void HaikuWindow::minimize() {
    if (_win->Lock()) {
        _win->Minimize(true);
        _win->Unlock();
    }
}

void HaikuWindow::setMaximized(bool on) {
    if (_win->Lock()) {
        if (_win->maximized != on)
            _win->BWindow::Zoom();
        _maximized = _win->maximized;
        _win->Unlock();
    }
}

void HaikuWindow::setFullscreen(bool on) {
    if (on == _fullscreen || !_win->Lock())
        return;
    if (on) {
        _beforeFullscreen = _win->Frame();
        const BRect m     = monitorFrame(_beforeFullscreen);
        _win->SetLook(B_NO_BORDER_WINDOW_LOOK);
        _win->MoveTo(m.LeftTop());
        _win->ResizeTo(m.Width(), m.Height());
    } else {
        _win->SetLook(B_TITLED_WINDOW_LOOK);
        _win->MoveTo(_beforeFullscreen.LeftTop());
        _win->ResizeTo(_beforeFullscreen.Width(), _beforeFullscreen.Height());
    }
    _fullscreen = on;
    _win->Unlock();
    emit({.type = EventType::StateChanged});
}

void HaikuWindow::activate() {
    if (_win->Lock()) {
        if (_minimized)
            _win->Minimize(false);
        _win->Activate(true);
        _win->Unlock();
    }
}

void HaikuWindow::setAlwaysOnTop(bool on) {
    if (on == _above || !_win->Lock())
        return;
    _win->SetFeel(on ? B_FLOATING_ALL_WINDOW_FEEL : B_NORMAL_WINDOW_FEEL);
    _above = on;
    _win->Unlock();
    emit({.type = EventType::StateChanged});
}

bool HaikuWindow::setPosition(Point p) {
    if (_maximized || _fullscreen || _minimized || !_win->Lock())
        return false;
    _win->MoveTo(float(p.x), float(p.y));
    _win->Unlock();
    return true;
}

void HaikuWindow::requestAttention() {
    // air/OS has no taskbar flashing; a window that wants attention while
    // inactive is the app's to show (the unread badge, a notification).
}

void HaikuWindow::setCursor(Cursor c) {
    static BCursor *cache[size_t(Cursor::Hidden) + 1] = {};
    const size_t    i                                  = size_t(c);
    if (i >= std::size(cache))
        return;
    if (!cache[i]) {
        BCursorID id = B_CURSOR_ID_SYSTEM_DEFAULT;
        switch (c) {
        case Cursor::Arrow: id = B_CURSOR_ID_SYSTEM_DEFAULT; break;
        case Cursor::IBeam: id = B_CURSOR_ID_I_BEAM; break;
        case Cursor::Hand: id = B_CURSOR_ID_FOLLOW_LINK; break;
        case Cursor::Wait: id = B_CURSOR_ID_PROGRESS; break;
        case Cursor::Progress: id = B_CURSOR_ID_PROGRESS; break;
        case Cursor::Crosshair: id = B_CURSOR_ID_CROSS_HAIR; break;
        case Cursor::NotAllowed: id = B_CURSOR_ID_NOT_ALLOWED; break;
        case Cursor::Move: id = B_CURSOR_ID_MOVE; break;
        case Cursor::Grab: id = B_CURSOR_ID_GRAB; break;
        case Cursor::Grabbing: id = B_CURSOR_ID_GRABBING; break;
        case Cursor::ResizeH: id = B_CURSOR_ID_RESIZE_EAST_WEST; break;
        case Cursor::ResizeV: id = B_CURSOR_ID_RESIZE_NORTH_SOUTH; break;
        case Cursor::ResizeNWSE: id = B_CURSOR_ID_RESIZE_NORTH_WEST_SOUTH_EAST; break;
        case Cursor::ResizeNESW: id = B_CURSOR_ID_RESIZE_NORTH_EAST_SOUTH_WEST; break;
        case Cursor::Hidden: id = B_CURSOR_ID_NO_CURSOR; break;
        }
        cache[i] = new BCursor(id);
    }
    if (_win->Lock()) {
        _view->SetViewCursor(cache[i], true);
        _win->Unlock();
    }
}

void HaikuWindow::setTextInput(const TextInputState &s) {
    if (_win->Lock()) {
        _view->textInput = s.enabled;
        _view->caret     = BRect(
            float(s.caret.x),
            float(s.caret.y),
            float(s.caret.x + std::max(1, s.caret.w) - 1),
            float(s.caret.y + std::max(1, s.caret.h) - 1)
        );
        _win->Unlock();
    }
}

// ── Frames and painting ─────────────────────────────────────────────────────

bool HaikuWindow::frameReady(core::Clock::time_point now, core::Clock::time_point *notBefore) const {
    if (!_framePending || !_visible || _minimized || _painting)
        return false;
    const auto due = _lastFrame + std::chrono::milliseconds(_app->frameIntervalMs());
    if (now < due) {
        if (notBefore)
            *notBefore = due;
        return false;
    }
    return true;
}

void HaikuWindow::sendFrame(core::Clock::time_point now) {
    _framePending = false;
    _lastFrame    = now;
    emit({.type = EventType::Frame});
}

Canvas HaikuWindow::beginPaint() {
    const int pw = std::max(1, int(std::ceil(_size.w * _scale)));
    const int ph = std::max(1, int(std::ceil(_size.h * _scale)));
    if (!_painting) {
        _view->paintLock.lock();
        _painting = true;
    }
    BBitmap *b = _view->bitmap;
    if (!b || b->Bounds().IntegerWidth() + 1 != pw || b->Bounds().IntegerHeight() + 1 != ph) {
        auto *nb = new BBitmap(BRect(0, 0, pw - 1, ph - 1), B_RGB32);
        if (nb->InitCheck() != B_OK) {
            delete nb;
            return {};
        }
        // What overlaps keeps its pixels: the canvas holds the last frame.
        if (b) {
            const int ow = std::min(pw, b->Bounds().IntegerWidth() + 1);
            const int oh = std::min(ph, b->Bounds().IntegerHeight() + 1);
            for (int y = 0; y < oh; ++y)
                std::memcpy(
                    static_cast<uint8 *>(nb->Bits()) + size_t(y) * nb->BytesPerRow(),
                    static_cast<uint8 *>(b->Bits()) + size_t(y) * b->BytesPerRow(),
                    size_t(ow) * 4
                );
        }
        delete b;
        _view->bitmap = b = nb;
    }
    _view->scale = _scale;
    return {static_cast<uint32_t *>(b->Bits()), pw, ph, int(b->BytesPerRow() / 4), _scale};
}

void HaikuWindow::endPaint(const std::vector<Rect> &damage) {
    if (!_painting)
        return;
    BBitmap *b = _view->bitmap;
    const int pw = b ? b->Bounds().IntegerWidth() + 1 : 0, ph = b ? b->Bounds().IntegerHeight() + 1 : 0;
    _painting = false;
    _view->paintLock.unlock();
    std::vector<BRect> rects;
    auto logical = [&](int x0, int y0, int x1, int y1) {
        rects.push_back(BRect(
            std::floor(x0 / _scale), std::floor(y0 / _scale), std::ceil(x1 / _scale) - 1,
            std::ceil(y1 / _scale) - 1
        ));
    };
    if (damage.empty()) {
        logical(0, 0, pw, ph);
    } else {
        for (const Rect &d : damage) {
            const int x0 = std::max(0, d.x), y0 = std::max(0, d.y);
            const int x1 = std::min(pw, d.x + d.w), y1 = std::min(ph, d.y + d.h);
            if (x1 > x0 && y1 > y0)
                logical(x0, y0, x1, y1);
        }
    }
    BRect missed;
    {
        std::lock_guard<std::mutex> m(_view->missedLock);
        missed         = _view->missed;
        _view->missed = BRect();
    }
    if (missed.IsValid())
        rects.push_back(missed);
    if (rects.empty() || !_win->Lock())
        return;
    for (const BRect &r : rects)
        _view->Invalidate(r);
    _win->Unlock();
}

} // namespace plat::haiku
