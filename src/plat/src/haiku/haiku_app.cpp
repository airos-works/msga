// The Haiku backend's App: the PosixLoop on the App thread, the
// BApplication on a thread of its own, and the desktop services the Be API
// has (clipboard, notifications, file panels, the roster, MIME types, the
// network roster, the locale roster).
#include "haiku/haiku_internal.h"
#include "haiku/deskbar_protocol.h"

#include "core/pacing.h"
#include "core/strings.h"
#include "core/transfer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <csignal>
#include <cstdio>
#include <fnmatch.h>
#include <unistd.h>

#include <Bitmap.h>
#include <BitmapStream.h>
#include <Clipboard.h>
#include <DataIO.h>
#include <Deskbar.h>
#include <Directory.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <FindDirectory.h>
#include <Looper.h>
#include <LocaleRoster.h>
#include <MimeType.h>
#include <Notification.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <TranslationUtils.h>
#include <TranslatorFormats.h>
#include <TranslatorRoster.h>
#include <net/if.h>
#include <NetworkInterface.h>
#include <NetworkNotifications.h>
#include <NetworkRoster.h>

namespace plat::haiku {

namespace {

constexpr const char *kNotificationArg = "--plat-notification=";

// The BApplication's looper: it was made (and is locked) on the App thread;
// this thread takes the lock over and runs it.
int32 applicationThread(void *data) {
    auto *app = static_cast<PlatBApplication *>(data);
    // Run() returns with the looper still locked by this thread; the
    // application goes on this same thread, as an application's main()
    // deletes it after Run(). Deleted from another thread once this one has
    // gone, ~BApplication waits on its port forever and the team never
    // exits: a quit MSGA lingered, and single launch handed every later
    // launch to it.
    if (app->Lock())
        app->Run();
    delete app;
    return 0;
}

uint32_t argb(rgb_color c) {
    return 0xff000000u | uint32_t(c.red) << 16 | uint32_t(c.green) << 8 | c.blue;
}

// The MIME names air/OS's clipboard and drags use for plat's.
std::string nativeMime(std::string_view mime) {
    return core::isTextMime(mime) ? std::string("text/plain") : std::string(mime);
}

// A picture on air/OS's clipboard is an archived BBitmap, the message
// "image/bitmap" (Screenshot, ShowImage, airShot, the browsers; a few say
// "image/x-be-bitmap"): plat's image/png both ways.
constexpr const char *kPngMime       = "image/png";
constexpr const char *kBitmapNames[] = {"image/bitmap", "image/x-be-bitmap"};

bool findBitmap(const BMessage &clip, BMessage *archive) {
    for (const char *name : kBitmapNames)
        if (clip.FindMessage(name, archive) == B_OK)
            return true;
    return false;
}

bool hasBitmap(const BMessage &clip) {
    for (const char *name : kBitmapNames)
        if (clip.HasMessage(name))
            return true;
    return false;
}

std::optional<std::string> pngFromBitmap(BMessage &archive) {
    // Another application's data: only a frame a bitmap can have.
    BRect frame;
    if (archive.FindRect("_frame", &frame) != B_OK || !frame.IsValid() || frame.Width() >= 32768 ||
        frame.Height() >= 32768)
        return std::nullopt;
    auto *bitmap = new BBitmap(&archive);
    if (bitmap->InitCheck() != B_OK) {
        delete bitmap;
        return std::nullopt;
    }
    BBitmapStream in(bitmap); // owns the bitmap
    BMallocIO     png;
    if (BTranslatorRoster::Default()->Translate(&in, nullptr, nullptr, &png, B_PNG_FORMAT) != B_OK)
        return std::nullopt;
    return std::string(static_cast<const char *>(png.Buffer()), png.BufferLength());
}

bool bitmapFromPicture(const std::string &encoded, BMessage *archive) {
    BMemoryIO in(encoded.data(), encoded.size());
    BBitmap  *bitmap = BTranslationUtils::GetBitmap(&in);
    if (!bitmap)
        return false;
    const bool ok = bitmap->InitCheck() == B_OK && bitmap->Archive(archive) == B_OK;
    delete bitmap;
    return ok;
}

} // namespace

// ── PlatBApplication ────────────────────────────────────────────────────────

PlatBApplication::PlatBApplication(HaikuApp *owner, status_t *error)
    : BApplication(kSignature, error), _owner(owner) {}

bool PlatBApplication::QuitRequested() {
    if (leaving)
        return true;
    // The system shutting down or restarting: the registrar takes the reply
    // BLooper sends for our false as "no" and calls the shutdown off, so it
    // gets its yes here (BLooper's own is then a duplicate, which a message
    // refuses), and the quit below has the team gone well within the time
    // the registrar waits for it.
    bool shutdown = false;
    if (BMessage *msg = CurrentMessage();
        msg && msg->FindBool("_shutdown_", &shutdown) == B_OK && shutdown) {
        BMessage reply(B_REPLY);
        reply.AddBool("result", true);
        reply.AddInt32("thread", Thread());
        msg->SendReply(&reply);
    }
    // Command+Q, Deskbar's Quit, the system shutting down: the app decides.
    _owner->post([o = _owner] { o->fromApplication({.type = EventType::QuitRequested}); });
    return false;
}

void PlatBApplication::ReadyToRun() {
    // Network changes from now on (B_NETWORK_MONITOR, below).
    start_watching_network(
        B_WATCH_NETWORK_INTERFACE_CHANGES | B_WATCH_NETWORK_LINK_CHANGES, BMessenger(this)
    );
}

void PlatBApplication::ArgvReceived(int32 argc, char **argv) {
    // This launch's own arguments come here too, before ReadyToRun: main()
    // has them already.
    if (!IsLaunching()) {
        std::vector<std::string> args;
        for (int32 i = 1; i < argc; ++i)
            args.emplace_back(argv[i]);
        const char *cwd = nullptr;
        if (BMessage *m = CurrentMessage())
            m->FindString("cwd", &cwd);
        // A click on one of our notifications: notification_server launches
        // the app with the argument notify() gave it.
        for (const std::string &a : args)
            if (a.rfind(kNotificationArg, 0) == 0) {
                const uint64_t id = std::strtoull(a.c_str() + std::strlen(kNotificationArg), nullptr, 10);
                _owner->post([o = _owner, id] { o->notificationClicked(id); });
                return;
            }
        Event e{.type = EventType::InstanceActivated, .text = cwd ? cwd : ""};
        e.strings = std::move(args);
        _owner->post([o = _owner, e = std::move(e)] { o->fromApplication(e); });
    }
}

void PlatBApplication::RefsReceived(BMessage *msg) {
    // Files opened with the app (Tracker's "Open with"): as a later launch
    // with their paths.
    std::vector<std::string> paths;
    entry_ref                ref;
    for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; ++i) {
        BPath path;
        if (BEntry(&ref, true).GetPath(&path) == B_OK)
            paths.emplace_back(path.Path());
    }
    if (paths.empty())
        return;
    Event e{.type = EventType::InstanceActivated};
    e.strings = std::move(paths);
    _owner->post([o = _owner, e = std::move(e)] { o->fromApplication(e); });
}

void PlatBApplication::MessageReceived(BMessage *msg) {
    switch (msg->what) {
    case B_NETWORK_MONITOR:
        _owner->post([o = _owner] { o->networkChanged(); });
        break;
    case deskbar::kHello:
    case deskbar::kActivated:
    case deskbar::kMenuItem:
    case deskbar::kDetached:
        _owner->post([o = _owner, m = BMessage(*msg)] { o->trayMessage(m); });
        break;
    default:
        BApplication::MessageReceived(msg);
    }
}

// ── HaikuApp ────────────────────────────────────────────────────────────────

HaikuApp::HaikuApp() {
    _loop.beforeWait = [this] { return beforeWait(); };
}

HaikuApp::~HaikuApp() {
    _loop.core.shutdown();
    if (_bapp && !_secondary && _appThread >= 0) {
        // The application thread deletes it once Run() returns.
        _bapp->leaving = true;
        BMessenger(_bapp).SendMessage(B_QUIT_REQUESTED);
        status_t ignored;
        wait_for_thread(_appThread, &ignored);
    } else {
        delete _bapp; // never run: a secondary launch, or no thread
    }
    _bapp = nullptr;
}

bool HaikuApp::start(std::string *error) {
    // A peer that closes a socket we write to must not kill the process
    // (OpenSSL's socket BIO writes with plain write()); left alone when the
    // embedding program has its own handler.
    struct sigaction sa {};
    if (sigaction(SIGPIPE, nullptr, &sa) == 0 && sa.sa_handler == SIG_DFL)
        signal(SIGPIPE, SIG_IGN);
    // Made here, on the App thread, like any application's: the registrar
    // registers the team's main thread, and every Be API call the App
    // thread makes has be_app in place.
    status_t err = B_OK;
    _bapp        = new PlatBApplication(this, &err);
    if (err == B_ALREADY_RUNNING) {
        // BApplication sent our arguments to the running instance;
        // claimSingleInstance() says so and the app exits.
        _secondary = true;
        return true;
    }
    if (err != B_OK) {
        if (error)
            *error = std::string("BApplication: ") + strerror(err);
        delete _bapp;
        _bapp = nullptr;
        return false;
    }
    _bapp->Unlock();
    _appThread = spawn_thread(applicationThread, "msga application", B_DISPLAY_PRIORITY, _bapp);
    if (_appThread < 0 || resume_thread(_appThread) != B_OK) {
        if (error)
            *error = "cannot start the application thread";
        return false;
    }
    _online = networkOnline();
    return true;
}

void HaikuApp::run() {
    _loop.run();
}

std::unique_ptr<Window> HaikuApp::createWindow(const WindowDesc &desc) {
    if (_secondary)
        return nullptr;
    const uint64_t id = _nextWindow++;
    auto           w  = std::make_unique<HaikuWindow>(this, id, desc);
    _windows[id]      = w.get();
    return w;
}

void HaikuApp::fromWindow(uint64_t id, Event e) {
    auto it = _windows.find(id);
    if (it != _windows.end())
        it->second->handle(e);
}

void HaikuApp::fromApplication(Event e) {
    if (e.type == EventType::InstanceActivated) {
        std::vector<std::string> urls = core::schemeUrls(e.strings, _schemes);
        emit(e);
        if (!urls.empty()) {
            Event o{.type = EventType::OpenUrls};
            o.strings = std::move(urls);
            emit(o);
        }
        return;
    }
    emit(e);
}

void HaikuApp::notificationClicked(uint64_t id) {
    emit({.type = EventType::NotificationActivated, .id = id});
}

void HaikuApp::networkChanged() {
    const std::optional<bool> now = networkOnline();
    if (now == _online)
        return;
    _online = now;
    emit({.type = EventType::NetworkChanged, .online = now.value_or(true)});
}

int HaikuApp::frameIntervalMs() const {
    static int cached = 0;
    if (!cached) {
        int milliHz = 0;
        for (const Monitor &m : monitors())
            milliHz = std::max(milliHz, m.refreshMilliHz);
        cached = core::frameIntervalMs(milliHz);
    }
    return cached;
}

bool HaikuApp::beforeWait() {
    emitFrames();
    const auto now = core::Clock::now();
    for (auto &[id, w] : _windows)
        if (w->frameReady(now, nullptr))
            return false;
    return true;
}

void HaikuApp::emitFrames() {
    const auto              now     = core::Clock::now();
    core::Clock::time_point soonest = core::Clock::time_point::max();
    // Snapshot: a Frame handler may create or destroy windows.
    std::vector<uint64_t> ids = std::move(_frameIds);
    ids.clear();
    for (auto &[id, w] : _windows)
        ids.push_back(id);
    for (uint64_t id : ids) {
        auto it = _windows.find(id);
        if (it == _windows.end())
            continue;
        core::Clock::time_point notBefore{};
        if (it->second->frameReady(now, &notBefore))
            it->second->sendFrame(now);
        else if (notBefore != core::Clock::time_point{})
            soonest = std::min(soonest, notBefore);
    }
    _frameIds = std::move(ids);
    if (soonest != core::Clock::time_point::max() && !_frameTimer) {
        const int ms = int(std::max<long long>(
            1, std::chrono::ceil<std::chrono::milliseconds>(soonest - now).count()
        ));
        // Only wakes the loop; beforeWait sends the Frame.
        _frameTimer = addTimer(ms, false, [this] { _frameTimer = 0; });
    }
}

// ── Clipboard ───────────────────────────────────────────────────────────────

void HaikuApp::setClipboard(std::vector<DataItem> items, Selection sel) {
    if (sel != Selection::Clipboard)
        return;
    // A copied picture also as the bitmap the system's applications paste
    // (decoded before the clipboard is locked).
    BMessage bitmap;
    bool     picture = false;
    for (const DataItem &i : items)
        if (i.mime.rfind("image/", 0) == 0 && bitmapFromPicture(i.data, &bitmap)) {
            picture = true;
            break;
        }
    if (!be_clipboard->Lock())
        return;
    be_clipboard->Clear();
    if (BMessage *clip = be_clipboard->Data()) {
        for (const DataItem &i : items)
            clip->AddData(nativeMime(i.mime).c_str(), B_MIME_TYPE, i.data.data(), ssize_t(i.data.size()));
        if (picture)
            clip->AddMessage(kBitmapNames[0], &bitmap);
    }
    be_clipboard->Commit();
    be_clipboard->Unlock();
}

void HaikuApp::requestClipboard(
    std::string_view mime, std::function<void(std::optional<std::string>)> cb, Selection sel
) {
    std::optional<std::string> out;
    BMessage                   bitmap;
    bool                       picture = false;
    if (sel == Selection::Clipboard && be_clipboard->Lock()) {
        if (BMessage *clip = be_clipboard->Data()) {
            const void *data = nullptr;
            ssize_t     size = 0;
            if (clip->FindData(nativeMime(mime).c_str(), B_MIME_TYPE, &data, &size) == B_OK)
                out = std::string(static_cast<const char *>(data), size_t(size));
            else if (mime == kPngMime)
                picture = findBitmap(*clip, &bitmap);
        }
        be_clipboard->Unlock();
    }
    // Encoded with the clipboard unlocked: other applications go on using it.
    if (picture)
        out = pngFromBitmap(bitmap);
    // Never re-entrant: the answer comes from the loop.
    post([cb = std::move(cb), out = std::move(out)] { cb(out); });
}

void HaikuApp::requestClipboardMimes(std::function<void(std::vector<std::string>)> cb, Selection sel) {
    std::vector<std::string> out;
    if (sel == Selection::Clipboard && be_clipboard->Lock()) {
        if (BMessage *clip = be_clipboard->Data()) {
            char     *name = nullptr;
            type_code type = 0;
            for (int32 i = 0; clip->GetInfo(B_MIME_TYPE, i, &name, &type) == B_OK; ++i)
                out.emplace_back(std::strcmp(name, "text/plain") == 0 ? core::kTextMime : name);
            if (hasBitmap(*clip) && std::find(out.begin(), out.end(), kPngMime) == out.end())
                out.emplace_back(kPngMime);
        }
        be_clipboard->Unlock();
    }
    post([cb = std::move(cb), out = std::move(out)] { cb(out); });
}

// ── Notifications ───────────────────────────────────────────────────────────

uint64_t HaikuApp::notify(const Notification &n) {
    const uint64_t id = _nextNotification++;
    BNotification  note(B_INFORMATION_NOTIFICATION);
    note.SetGroup(appInfo().name.c_str());
    note.SetTitle(n.title.c_str());
    note.SetContent(n.body.c_str());
    note.SetMessageID(std::to_string(id).c_str());
    if (BBitmap *icon = bitmapFrom(n.image)) {
        note.SetIcon(icon);
        delete icon;
    }
    // A click launches us with this argument; the roster hands it to the
    // running instance (single launch), see ArgvReceived().
    note.SetOnClickApp(kSignature);
    note.AddOnClickArg((std::string(kNotificationArg) + std::to_string(id)).c_str());
    const bigtime_t timeout = n.timeoutMs > 0 ? bigtime_t(n.timeoutMs) * 1000 : -1;
    if (note.Send(timeout) != B_OK) {
        return 0;
    }
    return id;
}

// ── The tray ────────────────────────────────────────────────────────────────

namespace {

// The Deskbar add-on beside the application: the package's
// add-ons/MSGA/MSGA Deskbar item, or msga_deskbar.so next to a build's binary.
bool trayAddOn(entry_ref *ref) {
    image_info info;
    int32      cookie = 0;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
        if (info.type != B_APP_IMAGE)
            continue;
        BPath app(info.name), dir;
        if (app.InitCheck() != B_OK || app.GetParent(&dir) != B_OK)
            return false;
        BPath prefix, packaged, built(dir.Path(), "msga_deskbar.so");
        if (dir.GetParent(&prefix) == B_OK)
            packaged.SetTo(prefix.Path(), "add-ons/MSGA/MSGA Deskbar item");
        for (const BPath *p : {&packaged, &built})
            if (p->InitCheck() == B_OK && BEntry(p->Path()).Exists())
                return get_ref_for_path(p->Path(), ref) == B_OK;
        return false;
    }
    return false;
}

void addMenuItems(BMessage *into, const std::vector<MenuItem> &items) {
    for (const MenuItem &m : items) {
        BMessage item;
        item.AddInt32(
            "kind",
            m.kind == MenuItem::Kind::Separator  ? deskbar::kKindSeparator
            : m.kind == MenuItem::Kind::Submenu  ? deskbar::kKindSubmenu
            : m.kind == MenuItem::Kind::Checkbox ? deskbar::kKindCheckbox
                                                 : deskbar::kKindAction
        );
        item.AddUInt32("id", m.id);
        item.AddString("label", m.label.c_str());
        item.AddBool("enabled", m.enabled);
        item.AddBool("checked", m.checked);
        addMenuItems(&item, m.children);
        into->AddMessage("item", &item);
    }
}

} // namespace

HaikuTray::HaikuTray(HaikuApp *app)
    : _app(app), _icon(deskbar::kIcon), _tooltip(deskbar::kTooltip), _menu(deskbar::kMenu) {
    _app->tray = this;
    BDeskbar  deskbar;
    entry_ref ref;
    if (!deskbar.IsRunning() || !trayAddOn(&ref))
        return;
    // One left behind (MSGA did not get to remove it) is for a gone MSGA.
    if (deskbar.HasItem(deskbar::kItemName))
        deskbar.RemoveItem(deskbar::kItemName);
    if (const status_t st = deskbar.AddItem(&ref); st != B_OK)
        std::fprintf(stderr, "plat/haiku: the Deskbar item failed: %s\n", strerror(st));
}

void HaikuTray::detached() {
    _item = BMessenger();
    if (_readd)
        return;
    // Deskbar quit (a new one loads the item from its settings) or the
    // add-on's file went, as when pkgman updates MSGA while it runs: once
    // there is a Deskbar and the (new) file, add the item again unless it
    // is back. A minute at most.
    _readdTries = 0;
    _readd      = _app->addTimer(1000, true, [this] {
        BDeskbar  deskbar;
        entry_ref ref;
        bool      done = ++_readdTries >= 60;
        if (!done && deskbar.IsRunning()) {
            if (deskbar.HasItem(deskbar::kItemName))
                done = true;
            else if (trayAddOn(&ref))
                done = deskbar.AddItem(&ref) == B_OK;
        }
        if (done) {
            _app->cancelTimer(_readd);
            _readd = 0;
        }
    });
}

HaikuTray::~HaikuTray() {
    if (_readd)
        _app->cancelTimer(_readd);
    BMessage bye(deskbar::kGoodbye);
    send(&bye);
    BDeskbar().RemoveItem(deskbar::kItemName);
    if (_app->tray == this)
        _app->tray = nullptr;
}

void HaikuTray::send(BMessage *msg) {
    if (_item.IsValid() && _item.SendMessage(msg, (BHandler *)nullptr, 500000) != B_OK)
        _item = BMessenger(); // Deskbar quit, or the item was taken out
}

void HaikuTray::attach(BMessenger item) {
    _item = item;
    BMessage hello(deskbar::kHello);
    hello.AddMessenger("msga", BMessenger(be_app));
    send(&hello);
    send(&_icon);
    send(&_tooltip);
    send(&_menu);
}

void HaikuTray::setIcon(const std::vector<Image> &sizes) {
    _icon.MakeEmpty();
    for (const Image &img : sizes) {
        BBitmap *b = bitmapFrom(img); // straight alpha, as B_RGBA32 wants
        if (!b)
            continue;
        std::string rows;
        rows.reserve(size_t(img.width) * img.height * 4);
        for (int y = 0; y < img.height; ++y)
            rows.append(static_cast<const char *>(b->Bits()) + size_t(y) * b->BytesPerRow(),
                        size_t(img.width) * 4);
        delete b;
        _icon.AddInt32("width", img.width);
        _icon.AddInt32("height", img.height);
        _icon.AddData("pixels", B_RAW_TYPE, rows.data(), ssize_t(rows.size()), false);
    }
    send(&_icon);
}

void HaikuTray::setTooltip(std::string_view utf8) {
    _tooltip.MakeEmpty();
    _tooltip.AddString("text", std::string(utf8).c_str());
    send(&_tooltip);
}

void HaikuTray::setMenu(std::vector<MenuItem> items) {
    _menu.MakeEmpty();
    addMenuItems(&_menu, items);
    send(&_menu);
}

std::unique_ptr<Tray> HaikuApp::createTray() {
    if (_secondary || tray)
        return nullptr;
    return std::make_unique<HaikuTray>(this);
}

void HaikuApp::trayMessage(const BMessage &msg) {
    if (!tray)
        return;
    switch (msg.what) {
    case deskbar::kHello: {
        BMessenger item;
        if (msg.FindMessenger("item", &item) == B_OK)
            tray->attach(item);
        break;
    }
    case deskbar::kActivated:
        emit({.type = EventType::TrayActivated, .tray = tray});
        break;
    case deskbar::kMenuItem:
        emit({.type = EventType::TrayMenuItem, .tray = tray, .id = msg.GetUInt32("id", 0)});
        break;
    case deskbar::kDetached:
        tray->detached();
        break;
    }
}

// ── Monitors ────────────────────────────────────────────────────────────────

std::vector<Monitor> HaikuApp::monitors() const {
    BScreen screen(B_MAIN_SCREEN_ID);
    if (!screen.IsValid())
        return {};
    const BRect f = screen.Frame();
    Monitor     m;
    m.id      = 1;
    m.name    = "screen";
    m.bounds  = {int(f.left), int(f.top), int(f.Width()) + 1, int(f.Height()) + 1};
    // Where a window's content fits with its tab and borders on screen (the
    // work area is for content: plat's window positions and sizes are), and
    // clear of a Deskbar stretched along the top or bottom.
    BRect area   = f;
    BRect insets(5, 27, 5, 5); // the default decorator's, until a window tells
    for (auto &[id, w] : _windows) {
        auto *bw = static_cast<BWindow *>(w->nativeHandle());
        if (bw && bw->Lock()) {
            const BRect d = bw->DecoratorFrame(), c = bw->Frame();
            bw->Unlock();
            if (d.IsValid() && c.IsValid())
                insets.Set(c.left - d.left, c.top - d.top, d.right - c.right, d.bottom - c.bottom);
            break;
        }
    }
    BDeskbar     deskbar;
    bool         expanded = false;
    const BRect  bar      = deskbar.Frame();
    switch (deskbar.Location(&expanded)) {
    case B_DESKBAR_TOP:
        area.top = std::max(area.top, bar.bottom + 1);
        break;
    case B_DESKBAR_BOTTOM:
        area.bottom = std::min(area.bottom, bar.top - 1);
        break;
    default:
        break;
    }
    area.left += insets.left;
    area.top += insets.top;
    area.right -= insets.right;
    area.bottom -= insets.bottom;
    m.workArea = {int(std::ceil(area.left)), int(std::ceil(area.top)), int(area.Width()) + 1,
                  int(area.Height()) + 1};
    m.scale   = deviceScale();
    m.primary = true;
    display_mode mode;
    if (screen.GetMode(&mode) == B_OK && mode.timing.h_total && mode.timing.v_total)
        m.refreshMilliHz = int(
            double(mode.timing.pixel_clock) * 1000.0 * 1000.0 /
            (double(mode.timing.h_total) * mode.timing.v_total)
        );
    return {m};
}

// ── Single instance, URLs ───────────────────────────────────────────────────

bool HaikuApp::claimSingleInstance(std::string_view, const std::vector<std::string> &) {
    // The package declares the app single launch: when another instance
    // runs, the roster has already handed it our arguments.
    return !_secondary;
}

bool HaikuApp::registerUrlScheme(std::string_view scheme) {
    if (!core::validScheme(scheme))
        return false;
    core::rememberScheme(_schemes, scheme);
    const std::string type = "application/x-vnd.Be.URL." + core::asciiLower(std::string(scheme));
    BMimeType         mime(type.c_str());
    if (mime.InitCheck() != B_OK)
        return false;
    if (!mime.IsInstalled() && mime.Install() != B_OK)
        return false;
    char preferred[B_MIME_TYPE_LENGTH] = "";
    if (mime.GetPreferredApp(preferred) != B_OK || std::strcmp(preferred, kSignature) != 0)
        mime.SetPreferredApp(kSignature);
    return true;
}

bool HaikuApp::openUrl(std::string_view url) {
    const std::string u(url);
    const size_t      colon = u.find(':');
    if (colon == std::string::npos || !core::validScheme(std::string_view(u).substr(0, colon)))
        return false;
    const std::string scheme = core::asciiLower(u.substr(0, colon));
    if (scheme == "file") {
        const std::string path = core::pathFromFileUri(u);
        entry_ref         ref;
        if (path.empty() || get_ref_for_path(path.c_str(), &ref) != B_OK)
            return false;
        const status_t st = be_roster->Launch(&ref);
        return st == B_OK || st == B_ALREADY_RUNNING;
    }
    // What BUrl::OpenWithPreferredApplication does: the scheme's MIME type
    // names the handler (Summit or WebPositive for https).
    const std::string type   = "application/x-vnd.Be.URL." + scheme;
    char             *argv[] = {const_cast<char *>(u.c_str()), nullptr};
    const status_t    st     = be_roster->Launch(type.c_str(), 1, argv);
    return st == B_OK || st == B_ALREADY_RUNNING;
}

// ── Network ─────────────────────────────────────────────────────────────────

std::optional<bool> HaikuApp::networkOnline() const {
    BNetworkRoster   &roster = BNetworkRoster::Default();
    BNetworkInterface iface;
    uint32            cookie = 0;
    bool              any    = false;
    while (roster.GetNextInterface(&cookie, iface) == B_OK) {
        any = true;
        if (iface.Flags() & IFF_LOOPBACK)
            continue;
        if ((iface.Flags() & IFF_UP) && iface.HasLink() && iface.CountAddresses() > 0)
            return true;
    }
    if (!any)
        return std::nullopt;
    return false;
}

// ── File panels ─────────────────────────────────────────────────────────────

namespace {

class PatternFilter final : public BRefFilter {
public:
    explicit PatternFilter(std::vector<std::string> patterns) : _patterns(std::move(patterns)) {}
    bool Filter(const entry_ref *ref, BNode *node, struct stat_beos *st, const char *) override {
        if (node && node->IsDirectory())
            return true;
        BEntry entry(ref, true);
        if (entry.IsDirectory())
            return true;
        for (const std::string &p : _patterns)
            if (p == "*" || p == "*.*" || fnmatch(p.c_str(), ref->name, FNM_CASEFOLD) == 0)
                return true;
        return false;
    }

private:
    std::vector<std::string> _patterns;
};

// The file panel's target: one per dialog, on a thread of its own, gone
// with the dialog.
class PanelLooper final : public BLooper {
public:
    PanelLooper(HaikuApp *app, std::function<void(std::vector<std::string>)> cb)
        : BLooper("file panel"), _app(app), _cb(std::move(cb)) {}
    ~PanelLooper() override {
        delete panel;
        delete filter;
    }

    void MessageReceived(BMessage *msg) override {
        switch (msg->what) {
        case B_REFS_RECEIVED: {
            std::vector<std::string> paths;
            entry_ref                ref;
            for (int32 i = 0; msg->FindRef("refs", i, &ref) == B_OK; ++i) {
                BPath path;
                if (BEntry(&ref, false).GetPath(&path) == B_OK)
                    paths.emplace_back(path.Path());
            }
            answer(std::move(paths));
            break;
        }
        case B_SAVE_REQUESTED: {
            entry_ref   dir;
            const char *name = nullptr;
            BPath       path;
            if (msg->FindRef("directory", &dir) == B_OK && msg->FindString("name", &name) == B_OK &&
                BEntry(&dir, true).GetPath(&path) == B_OK && path.Append(name) == B_OK)
                answer({path.Path()});
            else
                answer({});
            break;
        }
        case B_CANCEL:
            // Also sent when the panel hides after a choice: the first
            // answer counts.
            answer({});
            PostMessage(B_QUIT_REQUESTED);
            break;
        default:
            BLooper::MessageReceived(msg);
        }
    }

    BFilePanel *panel  = nullptr;
    BRefFilter *filter = nullptr;

private:
    void answer(std::vector<std::string> paths) {
        if (!_cb)
            return;
        _app->post([cb = std::move(_cb), paths = std::move(paths)] { cb(paths); });
        _cb = nullptr;
    }

    HaikuApp                                      *_app;
    std::function<void(std::vector<std::string>)> _cb;
};

} // namespace

void HaikuApp::showFileDialog(
    const FileDialogDesc &d, std::function<void(std::vector<std::string>)> cb
) {
    using M          = FileDialogDesc::Mode;
    auto *looper     = new PanelLooper(this, std::move(cb));
    const bool save  = d.mode == M::Save;
    uint32     nodes = d.mode == M::PickFolder ? B_DIRECTORY_NODE : B_FILE_NODE;
    std::vector<std::string> patterns;
    for (const FileFilter &f : d.filters)
        patterns.insert(patterns.end(), f.patterns.begin(), f.patterns.end());
    if (!patterns.empty() && d.mode != M::PickFolder)
        looper->filter = new PatternFilter(std::move(patterns));
    looper->Run();
    BMessenger target(looper);
    entry_ref  dir, *start = nullptr;
    if (!d.initialDir.empty() && get_ref_for_path(d.initialDir.c_str(), &dir) == B_OK)
        start = &dir;
    looper->panel = new BFilePanel(
        save ? B_SAVE_PANEL : B_OPEN_PANEL,
        &target,
        start,
        nodes,
        d.mode == M::OpenMultiple,
        nullptr,
        looper->filter,
        false,
        true
    );
    if (save && !d.suggestedName.empty())
        looper->panel->SetSaveText(d.suggestedName.c_str());
    if (BWindow *w = looper->panel->Window(); w && w->Lock()) {
        if (!d.title.empty())
            w->SetTitle(d.title.c_str());
        w->Unlock();
    }
    looper->panel->Show();
}

// ── Settings and places ─────────────────────────────────────────────────────

std::string HaikuApp::standardDir(StandardDir d) const {
    auto find = [](directory_which which, const char *sub = nullptr) -> std::string {
        BPath path;
        if (find_directory(which, &path, true) != B_OK)
            return {};
        if (sub) {
            BPath full(path.Path(), sub);
            if (BEntry(full.Path()).Exists())
                return full.Path();
        }
        return path.Path();
    };
    switch (d) {
    case StandardDir::Config:
    case StandardDir::State:
    // Not B_USER_DATA_DIRECTORY: ~/config is packagefs, and ~/config/data is
    // what packages put there, read-only (and absent when none do). An app's
    // own files go in ~/config/settings, which is writable.
    case StandardDir::Data:
        return find(B_USER_SETTINGS_DIRECTORY);
    case StandardDir::Cache:
        return find(B_USER_CACHE_DIRECTORY);
    case StandardDir::Temp:
        return find(B_SYSTEM_TEMP_DIRECTORY);
    case StandardDir::Home:
        return find(B_USER_DIRECTORY);
    case StandardDir::Desktop:
        return find(B_DESKTOP_DIRECTORY);
    case StandardDir::Documents:
        return find(B_USER_DIRECTORY, "Documents");
    case StandardDir::Downloads: {
        // Where Summit and WebPositive save: ~/Downloads when there is one.
        std::string dl = find(B_USER_DIRECTORY, "Downloads");
        return dl == find(B_USER_DIRECTORY) ? find(B_DESKTOP_DIRECTORY) : dl;
    }
    case StandardDir::Pictures:
        return find(B_USER_DIRECTORY, "Pictures");
    }
    return {};
}

SystemSettings HaikuApp::systemSettings() const {
    SystemSettings s;
    s.accentColor  = argb(ui_color(B_CONTROL_HIGHLIGHT_COLOR));
    s.caretBlinkMs = 500;
    // The Appearance preferences' font size, against the 12 pt default.
    if (be_plain_font)
        s.textScale = std::clamp(double(be_plain_font->Size()) / 12.0, 0.75, 3.0);
    return s;
}

std::vector<std::string> HaikuApp::preferredLanguages() const {
    std::vector<std::string> out;
    BMessage                 langs;
    if (BLocaleRoster::Default()->GetPreferredLanguages(&langs) == B_OK) {
        const char *l = nullptr;
        for (int32 i = 0; langs.FindString("language", i, &l) == B_OK; ++i)
            out.emplace_back(l);
    }
    if (out.empty())
        return App::preferredLanguages();
    return out;
}

bool HaikuApp::darkMode() const {
    const rgb_color c = ui_color(B_PANEL_BACKGROUND_COLOR);
    return 0.299 * c.red + 0.587 * c.green + 0.114 * c.blue < 128;
}

int HaikuApp::doubleClickMs() const {
    bigtime_t speed = 500000;
    get_click_speed(&speed);
    return int(std::clamp<bigtime_t>(speed / 1000, 100, 2000));
}

} // namespace plat::haiku

namespace plat {

std::unique_ptr<App> createHaikuApp(std::string *error) {
    auto app = std::make_unique<haiku::HaikuApp>();
    if (!app->start(error))
        return nullptr;
    return app;
}

} // namespace plat
