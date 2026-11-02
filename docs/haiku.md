# MSGA on air/OS

This fork (`airos-works/msga`, branch `airos`) runs msga natively on
[air/OS](https://github.com/airos-works), the Haiku-based operating system,
on x86_64 and ARM64. air/OS images carry it in Deskbar's **Office** folder;
it is also a package (`msga`) in the air/OS package pool.

Everything above the platform layer is upstream's code, unchanged: the same
Slack (and Claude Code) client, toolkit and renderer. What is air/OS's own is
below it, and wherever the system has a service, msga uses it.

## What uses the Be API

| Area | air/OS | Code |
|---|---|---|
| Windows and drawing | `BWindow` with one `BView`; the toolkit paints straight into a `B_RGB32` `BBitmap` (no copy), which the view draws. On air/OS's scaled desktops (the display layout's render scale) the bitmap is in device pixels, so text stays sharp. | `src/plat/src/haiku/haiku_window.cpp` |
| Input | Key codes by position with the keymap's characters for letters and punctuation, input methods (`B_INPUT_METHOD_EVENT`), wheel, buttons 1–5, modifier keys. Command (the Alt key by default) is the shortcut key, as on macOS; the shortcut labels name the key the Keymap preferences assign to it. | same |
| Event loop | The App thread runs msga's POSIX loop (the network layer's sockets plug in); the `BApplication` loops on a thread of its own and every window thread posts its events over. No thread ever waits for the App thread. | `src/plat/src/haiku/haiku_app.cpp` |
| Window | The native tab (title, zoom, close; minimise from Deskbar), so msga's own title strip is off. Zoom, full screen, "on top" (floating feel), the work area left by the tab, borders and a Deskbar along the top or bottom. | same |
| Clipboard | `BClipboard` (`text/plain`, and any other MIME type as is) | same |
| Tray icon | A Deskbar item: a small add-on of its own (`add-ons/MSGA/MSGA Deskbar item`, libbe only, never MSGA itself in Deskbar's team) with the logo and the unread dot; a click brings MSGA back, the right button opens its menu (workspaces, Settings, Quit). With it the window's close hides MSGA, as on the other platforms (the "Use tray" setting turns it off). The item says hello to MSGA, which sends it the icon, tooltip and menu; one left behind by a crash takes itself out | `src/plat/src/haiku/deskbar_tray.cpp`, `haiku_app.cpp` |
| Notifications | `BNotification`; a click relaunches MSGA with the notification's id, which the roster hands to the running copy (single launch) | same |
| Files | `BFilePanel` for open, save and folder choices; drops of Tracker files and of data from other apps | same |
| URLs | `BRoster` launches the scheme's preferred application (Summit or WebPositive for https); `msga://` is registered as a MIME type with MSGA as its handler | same |
| Single instance | The application flag `B_SINGLE_LAUNCH`: a second launch hands its arguments to the running copy | `tools/haiku/make-rdef.py` |
| Network state | `BNetworkRoster` and the net_server's notifications | `haiku_app.cpp` |
| Languages | `BLocaleRoster` (Locale preferences) | same |
| Sign-in tokens | The user's keyring, `BKeyStore` (air/OS asks once whether MSGA may use it). The settings file only marks which keys are in the keyring, so nothing asks before a first sign-in. | `src/base/secret_haiku.cpp` |
| Audio | The Media Kit: `BMediaFile`/`BMediaTrack` decode (every codec of the media add-ons, Slack's AAC clips included), `BSoundPlayer` plays, `BMediaRecorder` records voice input; system sounds from the Sounds preferences | `src/plat/src/audio/audio_haiku.cpp` |
| TLS | The system's OpenSSL 3 and its CA store (`/boot/system/data/ssl/CARootCertificates.pem`), not a TLS library of msga's own | `src/net/posix/tls_openssl.cpp` |
| Browser sign-in | Your own Summit (its profile, extensions and password manager) through its WebDriver service: msga asks Summit to open its automation socket at 127.0.0.1:47089 (starting Summit when it isn't running) and drives it with `summitdriver --target`. Slack's sign-in page opens as an "Automated" window; msga checks the session with Slack, reads the `d` cookie and the web client's workspaces, then closes that window | `src/app/slack/browser_login_haiku.cpp` |
| Fonts | The system's (Noto Sans, Noto Color Emoji and the Noto scripts), the user's in `~/config/data/fonts` | `src/text/font_index.cpp` |
| Spelling | Hunspell with HaikuPorts' dictionaries (`pkgman install myspell_en`, …) in `/boot/system/data/hunspell` | `src/app/spell/backend_linux.cpp` |
| Settings, caches | `~/config/settings/msga` (settings, workspaces, the app's data), `~/config/cache/msga` | `plat` `standardDir()` |
| Crashes | debug_server's reports (msga's own handler is off) | `src/app/crash/crash_handler.cpp` |

Not on air/OS: the self-updater (pkgman updates MSGA), the Linux launcher
entry, importing the Slack desktop app's session (there is no Slack desktop
app for air/OS).

FreeType and HarfBuzz are built in (as on macOS and Windows): the ARM64 SDK
has no HarfBuzz.

## Building

Cross-compiled on Linux with the airos-ci SDKs (`/data1/airos/sdk/ARCH`):

```sh
tools/haiku/build.sh x86_64            # build-haiku-x86_64/MSGA
tools/haiku/build.sh arm64 --package   # and a quick .hpkg
HAIKU_SDK=/data1/airos/sdk/x86_64-r1beta6 BUILD=build-haiku-beta6 tools/haiku/build.sh
```

`tools/haiku/toolchain.cmake` is the CMake toolchain file.
`tools/haiku/make-rdef.py` writes the resources (signature, single launch, the
version from `version.cmake`, the icon); `resources/haiku/msga.hvif` is
`gfx/icon.svg` converted by `tools/haiku/svg2hvif.py`, so the icon is the
logo of every other platform.

airos-ci builds the packages (`apps/build-app-packages.sh`, `build_msga`):
the workflow in `.github/workflows/airos.yml` on every push to `airos`, and
every image at the commit pinned in airos-ci's `apps/office.lock.json`.

## Testing

`tools/haiku/vm/run-vm.sh` boots a throwaway Haiku R1/beta6 VM (VNC
127.0.0.1:5913, SSH 2234 through `tools/haiku/vm/haiku.sh`, input and
screenshots with `tools/haiku/vm/vm.py`); build for it with the r1beta6 SDK
as above.

## Gotchas

- The Be API `dynamic_cast`s handlers, views and windows: the Haiku backend
  is compiled with `-frtti` although the rest of plat is not.
- Summit opens one automation socket for the life of the process (a second
  `--automation` is refused as busy) and hands a start only to the copy that
  runs with the same profile: hence the fixed address, no `--profile`, and
  the driver connecting with `--target` instead of starting a browser.
- `~/config` is packagefs: only `settings`, `cache`, `var`, `packages` and
  `non-packaged` under it are writable, and `~/config/data` (what
  `B_USER_DATA_DIRECTORY` names) is what packages put there. msga's data
  directory is therefore under `~/config/settings` too.
- Deskbar never unloads an add-on image and makes an item from its
  archive, by class name, out of the first image it loaded with that class;
  so the tray view's class is named after a hash of the add-on's sources.
  Deskbar also drops an item when its file goes (pkgman updating MSGA while
  it runs): the item tells MSGA, which adds it again from the new package.
- Haiku's runtime_loader has no `DT_RELR`: packed relocations are off there.
- No `eventfd` and no `getrandom` on Haiku: the transport's wake fds are
  pipes, random bytes come from `getentropy`.
- packagefs keeps a directory's mtime when a package adds files to it: the
  font index also watches the package folders, or a newly installed font
  package (noto_emoji_color) would go unseen until the cache is deleted.
- BWindow eats every Command+key it has no shortcut for: the window passes
  them all to the view (Command+Q still asks MSGA to quit).
