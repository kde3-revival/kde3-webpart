# kde3-wpepart

A browser part for Konqueror (KDE 3.5, kde3-revival) that shows web
pages with [WPE WebKit](https://wpewebkit.org/): `KWPEPart`, a
`KParts::ReadOnlyPart` with a `BrowserExtension`, as KHTML's part.

Status: **stage 2 in progress.**  Pages load and render, links, forms,
scripts, back/forward through Konqueror's history, title, progress,
padlock, status bar text and wheel scrolling work, on
the GPU (DMA-BUF) or through shared memory.  Stage 2 so far: downloads
through KDE's save dialog and KIO's progress window, Konqueror's context
menus, find in page, zoom, persistent cookies, HTTP authentication,
recovery from web process crashes.  See "Next stages" for what is
missing.

Why WPE WebKit: it is GLib based and embeddable without a toolkit (the
embedder implements a small "platform": display, view, buffers,
events), and it gets its security updates from the distribution.

## Architecture

```
 Konqueror (Qt 3, Qt's GLib main loop)                     WebKit's processes
 ┌───────────────────────────────────────────────┐
 │ KWPEPart (kwpepart.cpp)                       │
 │   KParts::ReadOnlyPart + BrowserExtension     │
 │   history tokens, openURLNotify, caption ...  │
 │ KWPEWidget (kwpewidget.cpp), a QWidget        │
 │   WebKitWebView, Qt events -> WPEEvents       │      WPEWebProcess (per site,
 │ X11 WPE platform (wpex11platform.cpp)         │◄────► sandboxed) renders into
 │   WPEDisplay/WPEView/WPEToplevel/WPEScreen    │ IPC   GBM (DMA-BUF) or SHM
 │   render_buffer -> DRI3 / MIT-SHM + Present   │       buffers
 └────────────────────────┬──────────────────────┘      WPENetworkProcess
                          │ Xlib/xcb (Qt's connection)
                          ▼
                       X server (DRI3, Present, MIT-SHM)
```

* **No threads.**  Qt 3 here runs on the GLib main loop
  (`QT_GLIB_EVENTLOOP`), so WebKit's GLib sources (IPC with its
  processes) are dispatched by Qt's event loop.
* **Header hygiene.**  Qt 3 defines `signals`, `slots` and `emit` as
  macros, GLib uses those words; Xlib defines `None`, `Bool`, `Status`.
  WebKit/GLib headers come first and only in `.cpp` files; the headers
  between the layers (`kwpewidget.h`, `wpex11platform.h`) contain
  neither GLib, WPE, Xlib nor (for the platform) Qt.
* **The module is never unloaded** (`__kde_do_not_unload`): GObject
  types cannot be unregistered and the part installs a Qt X11 event
  filter.

### The X11 platform (`wpex11platform.cpp`)

WebKit 2.54's WPEPlatform API lets the embedder subclass `WPEDisplay`,
`WPEView`, `WPEToplevel` and `WPEScreen` (GObject).  Our own platform
was chosen over the headless one: the headless platform only throws the
buffers away on a timer, we need them in an X window, with the X
server's GPU, its formats and its frame timing.

* **Buffer path, decided once per process:**
  * **DMA-BUF** if the X server has DRI3 and Present (and depth 24): the
    display answers `get_drm_device` with the device from `DRI3Open`
    (primary and render node via libdrm) and
    `get_preferred_buffer_formats` with XRGB8888/ARGB8888 and the
    modifiers of DRI3 `GetSupportedModifiers` (window and screen
    lists; implicit modifier before DRI3 1.2).  WebKit's web process
    then allocates GBM buffers on that device with one of those
    modifiers and renders into them on the GPU.  Each buffer becomes an
    X pixmap once (`PixmapFromBuffers`, all planes, e.g. AMD DCC has
    two), each frame is a `PresentPixmap` of the damaged region: the
    copy into the window is done by the GPU at the next vblank, no pixel
    crosses the CPU.
  * **SHM** otherwise (Xephyr, Xvfb, remote displays, `KWPE_FORCE_SHM=1`):
    no DRM device is advertised, so WebKit renders (GPU with read-back,
    or CPU) into shared memory buffers.  The damaged rectangles are
    copied into a MIT-SHM `XImage` (plain `XPutImage` when MIT-SHM is
    missing or the display is remote), put into a backing pixmap and
    presented from there.
* **Frame pacing like a Wayland compositor.**  `render_buffer` presents;
  `PresentCompleteNotify` (the frame is on screen) emits
  `wpe_view_buffer_rendered`, which lets WebKit draw the next frame.
  The DMA-BUF buffer on screen is held (Expose repaints from it) until
  the next one is shown; it is given back with `wpe_view_buffer_released`
  on `PresentIdleNotify`.  SHM buffers are given back as soon as they
  are copied.  A watchdog (250 ms) ends frames whose notification never
  comes (window not viewable), without Present a 16 ms timer paces.
  WebKit itself schedules rendering with a timer at the screen's
  refresh rate (from RandR).
* **Explicit sync** is not used (implicit sync of the dma-bufs); WebKit
  waits for its rendering fence before handing a buffer over.
* **Present events** arrive as XGE events in Qt's X queue; the widget
  code installs a Qt X11 event filter (`qt_set_x11_event_filter`,
  chained) that passes every event to `KWPE::handleXEvent()`.
* **Keys**: the filter also remembers the X key event Qt is delivering;
  the platform sends WebKit its X keycode (= evdev + 8, what WPE wants),
  the keysym of the event's XKB group and level (`XkbLookupKeySym`: a
  Romanian standard layout gives `scommabelow`, `abreve`, ...) and the
  modifiers.  Qt-synthesized key events (input-method text) are
  translated from the Qt event instead.

### The widget (`kwpewidget.cpp`)

One `WebKitWebContext` and `WebKitNetworkSession` per process (data in
`$KDEHOME/share/apps/kwpepart`, cache in KDE's cache directory), one
`WebKitWebView` and one WPE toplevel per widget.  Qt events become WPE
events: resize, show/hide (map/unmap), focus, window activation, mouse
buttons (WPE counts double clicks), motion, enter/leave, wheel notches
as discrete scroll steps, keys.

**Wheel scrolling**: a notch is one discrete WebKit step (WebKit
scrolls by a "line step" of view height^(2/3) pixels: 100 for a view
1000 pixels high), scaled by KDE's "scroll by N lines" (3: one step).
Not as precise (pixel) deltas: WebKit would then report 120 × delta /
2.5 as the DOM's `wheelDelta`.  WebKit's smooth scrolling (an animation
of every step) is **off** by default: with a fast wheel every notch
extends the animation and the page lags behind the wheel (see the
measurements).  `[Settings] SmoothScrolling` in `kwpepartrc` = `Never`
(default), `WhenEfficient` (on the GPU/DMA-BUF path only) or `Always`;
without it the same key of `khtmlrc` (`[KHTML Settings]`, KDE 4's name)
is read.  The settings are read again in every Konqueror process when
one changes them (DCOP signal `KWPEPartSettings changed()`).
While the page has the focus, keys without Ctrl/Alt/Meta go to the page
rather than to Konqueror's single-key shortcuts (`AccelOverride`), F-keys
and Escape stay Konqueror's; Tab moves inside the page.

### History: who navigates

WebKit navigates by itself (links, forms, scripts, redirects) and keeps
its own back/forward list; Konqueror keeps the history the user sees.
Routing every navigation through Konqueror (KonqRun, a KIO request for
the mime type of every link) would break POST forms, scripts and
same-document navigation and double every request, so:

* every **new** WebKit back/forward item (a link, a form, an anchor,
  `pushState`) becomes `openURLNotify()`: Konqueror saves the previous
  page and appends an entry, as with KHTML; a redirect or
  `location.replace()` only updates the URL;
* `saveState()` stores a token of WebKit's item in Konqueror's history
  entry; on Back/Forward Konqueror calls `restoreState()` and WebKit goes
  to that very item (page cache, scroll position, form contents, no
  POST resubmission).  Unknown tokens (another part instance showed the
  page) load the URL;
* `history.back()` and friends from scripts are turned into Konqueror's
  `goHistory(n)`, so both histories stay in step;
* `target=_blank`, `window.open()` and middle clicks go to
  `createNewWindow()` (window or tab, Konqueror's settings);
* main-frame responses WebKit cannot show (archives, PDF, ...) go back to
  Konqueror with `openURLRequest()` and their mime type: it embeds
  another part, starts an application or offers to save (KIO fetches
  them again); answers to POST requests cannot be fetched again and are
  downloaded by WebKit;
* tokens are unique in the process (Konqueror copies history entries
  between views) and are checked against the item's URL.

### Downloads, menus, find, zoom (`kwpedownload.cpp`, `kwpepart.cpp`)

* **Downloads** WebKit makes (attachments, `<a download>`, POST
  answers, "Save Link/Image As..."), with WebKit's cookies: KDE's save
  dialog (any KIO URL, starts in the last download directory), a
  `KIO::Job` that shows the transfer in KIO's progress window (Cancel
  cancels it), WebKit writes local files itself (`<file>.wkdownload`
  until complete), remote destinations get a temporary file that KIO
  uploads.
* **Context menus**: `BrowserExtension::popupMenu()` with a GUI client
  like KHTML's.  Konqueror adds Open in New Window/Tab, bookmarks, Open
  With/Preview In and Back/Forward/Reload; the part adds Save Link As,
  Copy Link Address, Save Image As, Copy Image Location, View Image,
  Copy Text.  Copy (also Konqueror's Edit/Copy) reads the selection in a
  script world of its own and puts it into the X clipboard.
* **Find** (`kwpepart.rc`): Edit/Find (Ctrl+F, KDE's find dialog), Find
  Next/Previous (F3/Shift+F3) with WebKit's find controller (all matches
  highlighted, wrap around, match count in the status bar).
* **Zoom**: View/Zoom In (Ctrl++, Ctrl+=), Zoom Out (Ctrl+-), Reset
  Zoom (Ctrl+0), Ctrl+wheel; 30–300 % page zoom.
* **Data**: cookies in `$KDEHOME/share/apps/kwpepart/cookies.sqlite`,
  local storage, IndexedDB etc. next to them (`KWPE_EPHEMERAL=1`: in
  memory only); WebKit's cookie policy (no third-party cookies), not
  kcookiejar's.
* **HTTP authentication**: KIO's password dialog, kept for the session.
  `submit-form` is connected as the hook for a password manager (stub).
* **Security**: WebKit's bubblewrap sandbox for the web processes
  (always on), TLS errors fail the load, file: pages get no access to
  other files, permission requests (location, camera, ...) are denied,
  mixed content shows Konqueror's "partially encrypted" padlock.
* **Web process crash**: the page is reloaded once; a second crash
  within 30 s or an out-of-memory kill leaves the last frame and a
  status bar message.

### Which part Konqueror uses

KDE 3 chooses parts by mime type: `kwpepart.desktop` registers for
`text/html` and `application/xhtml+xml` with `InitialPreference=12`,
above KHTML's 10, so Konqueror uses this part for web pages once it is
installed (also for local HTML files).  Users can reorder the parts in
Control Center → File Associations → text/html → Embedding, or switch a
view with View → View Mode.  A per-protocol choice (only http/https)
would need a Konqueror setting; see "Next stages".

## Building

Needs kde3-qt and kdelibs (CMake port), WPE WebKit ≥ 2.50 with the
WPEPlatform API (`wpe-webkit-2.0`, `wpe-platform-2.0`; developed with
2.54.3) and libX11, libX11-xcb, libXext, libXfixes, libXpresent,
libXrandr, libxcb-dri3, libdrm.

```sh
cmake -G Ninja -S . -B build \
      -DCMAKE_PREFIX_PATH=/opt/kde3 -DCMAKE_INSTALL_PREFIX=/opt/kde3
ninja -C build && ninja -C build install
kbuildsycoca
```

It installs `lib/kde3/libkwpepart.so`, `share/services/kwpepart.desktop`
and `bin/kwpeview`.

## Debugging and testing

`kwpeview URL` loads the part like Konqueror does (KLibLoader, KParts
factory) in a window of its own.

Environment variables:

| variable | effect |
| --- | --- |
| `KWPE_DEBUG=1` | the platform's decisions: extensions, DRM device, formats, imports |
| `KWPE_DEBUG=2` / `3` | also every Present completion / every damage rectangle |
| `KWPE_STATS=1` | per second: frames shown (and how many DMA-BUF), Present latency, longest gap, SHM copy time, damaged pixels; wheel event → first frame on screen, last wheel event of a burst → page at rest; time to commit, first paint and load end |
| `KWPE_FORCE_SHM=1` | shared memory even when DRI3 is there (to compare) |
| `KWPE_EPHEMERAL=1` | no data or cache directories |
| `KWPE_CANVAS_ACCELERATION=1` | GPU 2D canvas (off by default, see below) |
| `KWPE_DEVELOPER_EXTRAS=1` | WebKit's developer extras |

`tests/` has test pages (`pages/`, serve them with
`python3 -m http.server`), `xinject.py` (XTest input for a nested X
server: clicks, wheel, keys, UTF-8 text through the server's keymap; it
refuses `:0`/`:1`), `wpemem.sh` (RSS/PSS and CPU of a UI process and its
WebKit processes) and `bench.sh` (kwpeview idle and wheel scrolling with
frame statistics).

Xephyr has no DRI3, so it exercises the SHM path.  A rootful Xwayland
in a headless compositor has DRI3 and Present (glamor) and exercises the
DMA-BUF path without a second machine:

```sh
export XDG_RUNTIME_DIR=$(mktemp -d)
dbus-run-session -- sh -c 'mutter --headless --no-x11 --wayland-display=wl-test \
    --virtual-monitor 1920x1080 & sleep 2; WAYLAND_DISPLAY=wl-test Xwayland :83 \
    -geometry 1920x1080 -fullscreen -noreset'
```

## Measurements (stage 1, 2026-11-21)

Arch Linux, AMD GPU (radeonsi), WPE WebKit 2.54.3, kwpeview 1600×1000,
X server: rootful Xwayland with glamor on a headless mutter (DRI3 1.4,
Present 1.4, 59.96 Hz), same server for both paths.  Scrolling: 60 wheel
notches down and 60 up, 30 ms apart (WebKit's animated scrolling,
full-window damage every frame).

| | DMA-BUF | SHM (`KWPE_FORCE_SHM=1`) |
| --- | --- | --- |
| Wikipedia "KDE": first paint / load finished | 330 / 430 ms | 250 / 400 ms |
| Wikipedia, idle (a 60 fps CSS pulse animation on the page) | 12 % CPU | 51 % CPU |
| Wikipedia, scrolling | 60 fps, max gap 18 ms, 39 % CPU | 60 fps, max gap 17 ms, 76 % CPU, 1 ms copy/frame |
| long local page, scrolling | 60 fps, 11.5 % CPU | 60 fps, 28 % CPU |
| still page, idle | 0.2 % CPU | 0.0 % CPU |
| memory, Wikipedia (kwpeview + web + network process) | RSS 559 MiB, PSS 313 MiB | RSS 593 MiB, PSS 337 MiB |
| memory, still page | RSS 449 MiB, PSS 219 MiB | RSS 467 MiB, PSS 227 MiB |

CPU is the sum over kwpeview and WebKit's processes, in % of one core.
The SHM path costs a full-frame GPU read-back in the web process and a
copy of the damage in the part; the DMA-BUF path costs neither.  In
Xephyr (SHM, software X server) wheel scrolling of Wikipedia also runs
at 55–60 fps.

### Wheel latency (2026-11-21)

Xephyr (SHM path, software X server), kwpeview 1600×1000, `long.html`,
`tests/scrollbench.sh`: the time from Qt's wheel event to the first
frame on screen that changed a quarter of the view or more, and from the
last wheel event of a burst to the last such frame (the page at rest).

| | smooth scrolling on (before) | off (now the default) |
| --- | --- | --- |
| single notch: first frame | 39 ms | 40 ms |
| single notch: page at rest | 141 ms | 40 ms |
| 25 notches 12 ms apart: page at rest after the last one | 261 ms | 56 ms |

## Known problems

* WebKit 2.54 drops the contents of GPU-accelerated 2D canvases of pages
  in the back/forward cache (going back shows them empty), so 2D canvas
  acceleration is off by default.
* For http/https URLs typed or opened from outside, Konqueror first asks
  KIO for the mime type (KonqRun), so the first request of a page goes
  through kio_http too (KDE's cookie dialog may appear) before WebKit
  loads it itself.
* WebKit's frames are paced by its own refresh-rate timer, not by the
  X server's vblank: frames are occasionally one vblank late.
* POST data handed over by Konqueror (`URLArgs::postData`, e.g. a form
  submitted in KHTML and reloaded here) is not passed to WebKit.
* Printing: WPE WebKit has no print API (`WebKitPrintOperation` is
  GTK-only); Konqueror's Print stays disabled.
* `WebKitEditorState` never changes with the WPE platform API, so
  Konqueror's Copy is always enabled.
* Unsupported GET responses are fetched again by KIO for Konqueror
  (without WebKit's cookies): downloads behind a login may get the login
  page; "Save Link As" uses WebKit and works.
* `history.back()` steps are counted in WebKit's back/forward list, which
  also has items of subframe navigations Konqueror never saw.
* No `get_keymap` in the platform (WPE assumes a US keymap for its own
  keymap queries); autorepeat is not filtered; Escape never reaches the
  page.
* Script dialogs (`alert`, `confirm`, `beforeunload`) and the file
  chooser have no UI yet; `window.open()` returns null (popups using
  `opener` break).
* The DMA-BUF path was tested on Xwayland (AMD), not yet on a real Xorg
  (Fedora 44 does not package WPE WebKit; the Arch build needs glibc
  2.44).

## Next stages

1. **Konqueror integration**: a Konqueror setting for the web engine
   (per protocol, skipping the KIO mime type probe for http/https);
   view source, favicons, script dialogs, file chooser, the X clipboard
   and primary selection (`WPEClipboard`: paste into pages), printing
   (no WPE API: maybe a snapshot or a PDF via a headless GTK WebKit).
   Done: context menus, find, zoom, copy.
2. **Downloads**: `URLArgs::postData`; unsupported GET content
   downloaded by WebKit when no part embeds it.  Done: downloads via
   KDE's dialog and KIO's progress window, POST downloads.
3. **Cookies and passwords**: KDE's cookie policy (kcookiejar), KWallet
   for HTTP authentication and forms (hook: `submit-form`).  Done:
   persistent cookies, HTTP authentication dialog.
4. **Input**: XIM preedit through a `WPEInputMethodContext`; smooth
   XI2.1 scrolling as precise deltas with phases (with the Qt 3 XI2
   work); touchpad gestures.
5. **Frame timing**: a `WPEScreenSyncObserver` driven by Present MSC
   notifications (on its own X connection: WebKit calls it from another
   thread), explicit sync (syncobj) where the X server has it, KDE's
   font, colour and double-click settings into `WPESettings`.
6. **Settings module** (kcm): engine choice, JavaScript, plugins, user
   agent, fonts, cache.
