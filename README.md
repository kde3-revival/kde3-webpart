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
recovery from web process crashes, an ad blocker (EasyList and friends
as WebKit content filters).  See "Next stages" for what is missing.

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

### Clipboard, page dialogs, favicons, source (`kwpeclipboard.cpp`, `kwpedialogs.cpp`)

* **Paste** (`KWPEClipboard`, the X11 platform's `WPEClipboard`): Ctrl+V,
  Ctrl+Shift+V and Shift+Insert paste the X CLIPBOARD, a middle click
  into a text field pastes PRIMARY (WebKit does that for GTK only):
  text, HTML (sanitized by WebKit), URI lists, images.  What WebKit
  copies itself (a page's `execCommand("copy")` on a click) becomes the
  X CLIPBOARD.  WPE has a single clipboard that WebKit reads whenever
  the web process asks, so other programs' data is served only during a
  paste the user started (the key or the click opens a one second
  window); scripts cannot paste (`javascript-can-access-clipboard` off,
  DOM paste access denied by WPE).  WebKit waits for every read (and so
  does the window), so another program's targets and data are read by
  `kwpexselection.cpp`, not by Qt 3: INCR for large data (a screenshot
  PNG of several MB arrives in pieces of about 256 KB; Qt 3 slept 50 ms
  per piece), waits on the X connection, gives up after 2 s for the
  targets and after 3 s without progress / 10 s in all for the data
  (Qt 3 waited 5 s per step), only targets the owner offers are asked for,
  and the last image read is reused within the same paste (WebKit reads
  it two or three times per paste).
* **Page dialogs** are modal to the page only: non-modal windows, the
  page takes no input while one is open, they hide with their tab and
  come back with it, closing the tab answers them (cancel) before the
  view goes; no nested event loop runs.  `<input type=file>`: KDE's
  file dialog (multiple files, accept='s mime types as a filter, local
  regular readable files only).  `alert`, `confirm`, `prompt`,
  `onbeforeunload`: the page's origin as the caption, the message as
  plain text, "Do not show more dialogs from this page" until the next
  page.
* **Favicons**: after a load the best `<link rel=icon>` for 16–32
  pixels (http/https) goes to Konqueror (`setIconURL()`), which has
  kded's favicons module download it into KDE's favicon cache (location
  bar, tabs, bookmarks); without one Konqueror asks for `/favicon.ico`.
* **View Document Source** (View menu, Ctrl+U): the main resource as
  WebKit loaded it (no new request) in the text editor, from a temporary
  file.
* **Other schemes**: links to `mailto:`, `ftp:`, `smb:`, ... (a click or
  another user gesture) go to Konqueror (the mail program, KIO).
* **PDF** goes to Konqueror (KDE's viewer, an application or saving)
  unless `[Settings] PDFInPage=true` (WebKit's PDF.js, a check box in
  the settings dialog).

### Ad blocking (`kwpeadblock.cpp`, `abpconverter.cpp`, `kwpe-filterconv`)

Adblock Plus filter lists become WebKit **content filters**
(`WebKitUserContentFilterStore`, Safari's content blocker format): WebKit
compiles them once into byte code (a DFA) and matches every load itself,
in the web process; the part is not called per request.

* **Lists**: EasyList, EasyPrivacy, uBlock filters and uBlock filters –
  Quick fixes by default (the uBlock lists carry the scriptlets, e.g.
  YouTube's; a choice saved before them gets them added once); uBlock
  filters – Annoyances and ROList (Romanian) can be switched on; more by
  URL.  Downloaded with KIO over **HTTPS only** (or read from a
  local file): no cookies, no language header, no authentication, a
  certificate that does not validate fails the download (no dialog), a
  redirect must stay on HTTPS, at most 16 MiB, HTML (a captive portal)
  is refused.  uBlock's `!#include file` is fetched from the directory of
  its list on the same host.  Nothing else is contacted: no telemetry.
  A list that cannot be fetched keeps its last copy.
* **Converting and compiling** happen in `kwpe-filterconv`, started in
  the background (KProcess, nice 10): ABP syntax → WebKit JSON
  (`abpconverter.cpp`, plain C++) → `webkit_user_content_filter_store_save()`.
  What WebKit cannot express is **dropped and counted, never widened**
  (a rule that blocks more than its list says breaks sites):
  `$redirect`, `$csp`, `$removeparam`, `$denyallow`, `$to`, scriptlets
  the part does not implement, procedural cosmetics (`:has-text`, `:upward`, `:style`, ...),
  HTML filters, `$method` (WebKit compares it with the *page's* method),
  regular expressions with `\b`, look-around or long `{n,m}`, patterns
  that could match an empty URL, `$elemhide` & co. on blocking filters,
  unknown options.  Exceptions that cannot be read completely apply more
  widely (blocking less): sites they cannot name are left out, `\b` is
  left out and a long `{n,m}` becomes `+`/`*`, `$to`/`$denyallow`/
  `$method` are ignored; other unknown options (`$genericblock`, ...)
  drop the exception.  uBlock's `example.*` entities become a site
  pattern with any suffix; in `!#if` blocks `env_safari` is true (those
  variants are written for WebKit's content blockers), every other
  token false; `$badfilter` matches whatever the order of options.
  Network patterns become WebKit's regex subset (`||` host anchor, `^`
  separator also at the end of the address, `|` anchors, escaping);
  `/regex/` filters are rewritten (`\d`, `\w`, small `{n,m}`, alternatives
  expanded into several rules) or dropped.  Sites become anchored
  `if-top-url`/`unless-top-url` patterns, not `if-domain`: WebKit finds an
  `if-domain` site anywhere in the page's URL (a `?next=http://site/`
  parameter counts).
* **Rule lists**: WebKit applies `ignore-previous-rules` only inside one
  rule list, and a list holds at most 150 000 rules, so the converter
  writes several lists of at most 50 000 rules, **each with every
  exception** that may cancel its rules: network blocks (slices) +
  exceptions + `$important` blocks + `$document` exceptions; generic
  element hiding (WebKit makes one style sheet of it) + `$document`,
  `$elemhide`, `$generichide` exceptions; site-specific element hiding +
  `$document`, `$elemhide`, `$specifichide` exceptions.  `#@#`
  exceptions are folded into the hiding rules (`unless-top-url`).
* **Updates**: daily by default (uBlock's lists follow YouTube several
  times a week; every 3 days or weekly in the dialog, kwpepartrc
  `[AdBlock] UpdateIntervalHours`; checked hourly), on "Update Now", when the choice
  of lists changes; at most once an hour after a failure.  One process
  at a time (`KLockFile`, a dead holder's lock is taken over).  A new
  set gets new identifiers; kwpepartrc names the set in use only after
  all of it compiled and loaded, then the old set leaves the store: a
  crash or a failed download, conversion or compilation leaves the old
  set in use and nothing half-written that is used.  Unchanged lists
  (MD5 of the files) are not compiled again.  Other Konqueror processes
  load the new set when told (DCOP `KWPEPartSettings changed()`).
* **First run**: nothing to block with until the lists are compiled
  (about 6 s); then every view gets the filters.  At the start of a
  process the compiled set is read from the store (20 ms); the first
  page waits for it (at most a second).
* **Off for a site**: Settings → "Disable Ad Blocking on This Site"
  (also from the status bar label), a list of sites in kwpepartrc
  (`[AdBlock] DisabledSites`, a site covers its subdomains).  Every view
  has a content manager of its own; the filters are taken off it while
  it shows such a site.  WebKit decides which elements to hide when it
  *requests* a page, so the part sets the filters before the loads it
  starts (typed URLs, history); a link or redirect that crosses between
  a site with and one without blocking switches them when the response
  arrives (before the page commits) and loads the page once more —
  not an answer to POST and not the end of a redirect (a login or
  payment callback must not be requested twice), those keep the element
  hiding of the previous site.  The site entry is the host as shown
  (`www.google.com`, not all of `google.com`).
* **Downloads** must be complete: a body shorter than announced fails,
  an unterminated last line is dropped (half a filter blocks more), a
  list must start with its header (`[Adblock ...]` or `! ...`).  A
  failed download is tried again in an hour even if the last copy was
  used.  A failed load of the stored set (another WebKit version)
  compiles anew at once.
* **UI**: Settings → "Configure WPE WebKit..." (smooth scrolling; ad
  blocking on/off, scriptlets on/off, update interval, lists with checkboxes, another list by URL, Update
  Now, last update and rule count, the sites where it is off) and a
  status bar label ("Ads: blocked / allowed / off"; a click shows the
  menu).  WebKit does not report what its content filters blocked (no
  signal, no failed resource), so there is no count.  No Konqueror
  control center page yet.

### Scriptlets (`scriptlets.js`, `abpconverter.cpp`, `kwpeadblock.cpp`)

Some ads cannot be blocked as requests: YouTube sends them in the same
answers as the video (`ytInitialPlayerResponse` in an inline script, the
`/youtubei/v1/player` answers of `fetch()`/XHR: `adPlacements`,
`playerAds`, `adSlots`).  uBlock's lists remove them with **scriptlets**,
`site##+js(name, args...)`: small patches of the page's JavaScript run
before the page's own scripts.

* **Converter**: `##+js(...)` and `#@#+js(...)` filters are parsed with
  uBlock's argument rules (`\,` is a comma; an argument in `'`, `"` or
  `` ` `` quotes needs no escapes), names and aliases resolved (`set`,
  `set-constant.js` → `set-constant`, `rmnt`, `nostif`, `trusted-rpfr`,
  ...).  Exceptions become excluded sites (`#@#+js()`: all scriptlets of
  the site); generic scriptlets (no site) are dropped.  `kwpe-filterconv
  --scriptlets FILE` writes them next to the compiled filters, with the
  set's identifier (`<set>.scriptlets`, written to a temporary name and
  renamed), one line per scriptlet: sites, excluded sites (domains or
  `example.*` entities) and the call as a JSON array.  The part reads it
  with the filters of the set (same atomic switch, old files removed with
  the old set).
* **Trust**: scriptlets that can put arbitrary content or code into a
  page (`trusted-*`, and `replace-node-text`, which rewrites inline
  scripts) run **only from uBlock's own lists** ("uBlock filters" with
  its includes and "Quick fixes", fetched from their fixed HTTPS URLs,
  `--trusted`), never from a list the user added; they are dropped as
  `scriptlet-untrusted`.  An exception of any list may switch any
  scriptlet off.
* **Library** (`src/scriptlets.js`, built into the part): written for
  kde3-wpepart from the documented behaviour (uBlock's own resources are
  GPLv3 and were not used).  `set-constant`, `trusted-set-constant`,
  `abort-on-property-read`/`-write`, `abort-current-script` (inline
  scripts), `abort-on-stack-trace`, `no-setTimeout-if`/`-setInterval-if`,
  `adjust-setTimeout`/`-setInterval`, `noeval-if`, `prevent-window-open`,
  `prevent-addEventListener`, `remove-attr`, `remove-class`,
  `json-prune` (`JSON.parse` and `Response.json()`; paths with `[]`,
  `*`, `[-]`), `json-prune-fetch-response`/`-xhr-response`,
  `trusted-replace-fetch-response`/`-xhr-response`, `prevent-fetch`,
  `prevent-xhr`, `remove-node-text`, `replace-node-text`,
  `trusted-prevent-dom-bypass`.  Patched functions are `Proxy`s of the
  originals (still `[native code]`); everything else stays in a closure.
  Not implemented (dropped as `scriptlet-unsupported`, ~830 filters of
  uBlock's lists): `trusted-edit-inbound-object`, `trusted-replace-argument`,
  `nowebrtc`, cookies and storage items, `json-edit`, `xml-prune`, ...
* **Injection**: a `WebKitUserScript` per host (document start, all
  frames, the page's own world), made from the library and the host's
  calls (its domain, parent domains and entities via libsoup's public
  suffix list, minus excluded sites).  WebKit matches a user script's
  allow list against the **main frame's** URL, not the frame's, so the
  scripts carry no allow list and each one first checks
  `location.hostname`; every view's content manager gets the scripts of
  the hosts it shows: the main site (set before the request) and each
  frame's site, added in the navigation (and response) policy decision,
  which WebKit sees before the frame's document.  Off with the filters
  ("Disable Ad Blocking on This Site" covers the frames of that page) or
  with the scriptlets switch.  User scripts are not subject to the page's
  CSP; the library itself adds no `<script>` and uses no `eval`.
* **Cost**: about 3 ms per frame of a host with scriptlets (the library
  is ~37 KB of source); every frame of the view parses the scripts of
  the other hosts too (at most 32 hosts per view; one library shared by
  small per-host scripts would be cheaper).  Response edits read only
  text/JSON bodies up to 16 MiB, and the page gets the response once all
  of it arrived.
* **Limits**: frames without a host (`about:blank`, `srcdoc`, `blob:`)
  get no scriptlets (`trusted-prevent-dom-bypass` covers some of that);
  `noeval-if` makes a direct `eval()` indirect on its sites (as uBlock);
  a rebuilt `Response` keeps `url`/`type`/`redirected` but its `clone()`
  does not.

### Which part Konqueror uses

KDE 3 chooses parts by mime type: `kwpepart.desktop` registers for
`text/html` and `application/xhtml+xml` with `InitialPreference=12`,
above KHTML's 10, so Konqueror uses this part for web pages once it is
installed (also for local HTML files).  Users can reorder the parts in
Control Center → File Associations → text/html → Embedding, or switch a
view with View → View Mode.

Konqueror normally asks KIO for the mime type of a typed (or otherwise
opened) http URL first (KonqRun): a request of its own, and KDE's cookie
or certificate dialogs, before the part loads the page again.
`kwpepart.desktop` says `X-KDE-BrowserView-FetchesProtocols=http,https`:
the Konqueror of kde3-kdebase's `konq-wpe` branch then opens such URLs as
`text/html` directly when this part would show them (the view's part,
or the preferred text/html part), except POST requests; KHTML views
keep the probe.  Whatever WebKit cannot show still reaches Konqueror
(`openURLRequest()` with the mime type: other parts, applications, the
save dialog; KIO fetches it once more), attachments are downloaded by
WebKit, and the location bar returns to the page on screen.

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

It installs `lib/kde3/libkwpepart.so`, `share/services/kwpepart.desktop`,
`bin/kwpeview` and `bin/kwpe-filterconv` (the ad blocker's converter;
`kwpe-filterconv --store DIR --id NAME [--json DIR] [--verbose] LIST...`
converts and compiles lists by hand, `--verbose` names every dropped
filter).

## Debugging and testing

`kwpeview URL` loads the part like Konqueror does (KLibLoader, KParts
factory) in a window of its own.

Environment variables:

| variable | effect |
| --- | --- |
| `KWPE_DEBUG=1` | the platform's decisions: extensions, DRM device, formats, imports; every paste and clipboard read |
| `KWPE_DEBUG=2` / `3` | also every Present completion / every damage rectangle |
| `KWPE_STATS=1` | per second: frames shown (and how many DMA-BUF), Present latency, longest gap, SHM copy time, damaged pixels; wheel event → first frame on screen, last wheel event of a burst → page at rest; time to commit, first paint and load end |
| `KWPE_FORCE_SHM=1` | shared memory even when DRI3 is there (to compare) |
| `KWPE_EPHEMERAL=1` | no data or cache directories |
| `KWPE_CANVAS_ACCELERATION=1` | GPU 2D canvas (off by default, see below) |
| `KWPE_DEVELOPER_EXTRAS=1` | WebKit's developer extras |

`tests/` has test pages (`pages/`, serve them with
`python3 -m http.server`), `xinject.py` (XTest input for a nested X
server: clicks, wheel, keys, UTF-8 text through the server's keymap; it
refuses `:0`/`:1`), `xselection.py` (owns an X selection with text,
HTML or a PNG like another program, for paste tests), `wpemem.sh`
(RSS/PSS and CPU of a UI process and its WebKit processes) and
`bench.sh` (kwpeview idle and wheel scrolling with frame statistics).
`server.py` serves the pages plus downloads, POST, cookies, Basic
authentication and `/typed?type=T&name=N` answers of any type (a real
PDF for `application/pdf`); its log says whether KIO or WebKit asked.
`tests/adblock/scriptlets.sh DISPLAY` checks the scriptlets with
`pages/scriptlets.html` (YouTube's pattern and every scriptlet, in a frame
of the same and of another site, with blocking off for the site, and on a
site without scriptlets).
Pages for stage 3: `probe.html` (PDF, archive, attachment, mailto:),
`paste.html`, `upload.html`, `dialogs.html`; `pasteimage.html` (a pasted
screenshot: the file, shown and scaled, while a counter keeps running).
`tests/clipboard/run.sh DISPLAY BUILD_DIR` runs `xseltest`: the X
selection reader against owners that behave and that do not (silent,
stalled INCR, too large).

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

### Ad blocking (2026-11-21)

`kwpe-filterconv`, EasyList + EasyPrivacy (135 486 filters): 108 995
network blocks, 1 606 exceptions, 13 638 generic and 10 268 site
element hiding rules, 444 dropped (0.3 %: extended CSS 289, procedural
50, redirect 30, ...); 5 rule lists, 137 401 rules with the
repeated exceptions.  Converting 0.18 s, compiling 4.4–5.2 s, peak RSS
of the converter 510 MiB (it exits), store 74 MB on disk (WebKit keeps
the source too).  All five lists (163 002 filters): 15 751 dropped
(11 593 of them uBlock scriptlets), compiling 6.1 s, 535 MiB.  The
first update (downloads included) takes about 6 s.

In use: the set is read from the store in 20 ms; kwpeview with a still
page, filters off/on: RSS 347 / 352 MiB, PSS 183 / 188 MiB (+5 MiB, in
the web process).  dailymail.co.uk: 1586 → 1060 MiB RSS, no ad slots
and no consent wall (screenshots in `build/wpepart-tests/adblock/`).

Scriptlets (2026-11-22): EasyList, EasyPrivacy, uBlock filters and Quick
fixes give 9 092 scriptlet injections (828 unsupported, 76 exceptions
applied), 22 of them for www.youtube.com.  Three videos without
scriptlets: pre-rolls on two (a skippable "Sponsored" ad, then a second
one); with them: no ad, the video plays (autoplay needs a click in a new
profile either way) — screenshots in `/var/tmp/kwpe-scriptlets/`.
Google, Wikipedia, BBC, CNN (5 scriptlets), Daily Mail (frames with
their own), Twitch, digi24 render as before.

## Known problems

* **Images scaled down by a page look jagged** (screenshots lose their
  text, photos alias; e.g. the screenshot on icesoft.ro).  This is WebKit
  itself, not the part: the Skia port draws `InterpolationQuality::Default`
  with bilinear filtering and no mipmaps, which point samples anything
  shown at less than half its size (the Cairo port box-filtered).  The
  part's output is bit-identical to Skia's `drawImageRect` with
  `SkSamplingOptions(kLinear, kNone)`; buffers are presented 1:1.  No
  setting, CSS (`image-rendering`) or environment variable changes it, and
  Skia is linked into libWPEWebKit with hidden symbols.  The fix is
  `webkit/0001-Skia-Sample-scaled-down-images-from-mipmaps.patch` (applies
  to wpewebkit 2.54.3; trilinear filtering when a decoded image, CSS
  background (`background-size`, repeated tiles), `border-image` or sprite
  is scaled down; a sprite or border-image slice is drawn from a mipmapped
  copy of its part of the image, so it takes no colour from its neighbours;
  unchanged at 1:1, scaled up, while resizing, for `image-rendering:
  pixelated` and for texture-backed images (canvas, video)), which needs a
  patched WPE WebKit build.
  Measured with the Skia of 2.54.3 (`tests/imagescale`), PSNR / SSIM against
  Lanczos-3: zone plate 2048 -> 280: 12.7 dB / 0.05 -> 30.7 dB / 0.77;
  1920x1152 screenshot -> 400x240: 22.4 dB / 0.83 -> 34.4 dB / 0.98,
  -> 640x384: 20.5 dB / 0.80 -> 32.6 dB / 0.97 (Firefox: Lanczos-3 itself).
  In kwpeview (`tests/pages/imagescale.html`, `tests/imagescale/run.sh`):
  `<img>` and `background-size` 400x240 / cover / contain 22.4 -> 32.8 dB,
  repeated 2048 -> 200 tiles 12.8 -> 35.7 dB, border-image 13.1 -> 29.7 dB,
  sprites pixel exact at their edges.
  Images in composited layers (`will-change`, 3D transforms) are drawn by
  the compositor, still without mipmaps.
* WebKit 2.54 drops the contents of GPU-accelerated 2D canvases of pages
  in the back/forward cache (going back shows them empty), so 2D canvas
  acceleration is off by default.
* Without the `konq-wpe` Konqueror, typed http/https URLs are still
  fetched by kio_http first (the mime type probe, KDE's cookie dialog).
  With it, a file WebKit cannot show is fetched twice (WebKit, then
  KIO) and KDE's save dialog does not know the server's file name
  (`Content-Disposition` on a non-attachment).
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
* `window.open()` returns null (popups using `opener` break).
* Page dialogs: the caption shows the top page's origin, also for a
  dialog of a frame from another site (WPE does not tell the frame).
  While an `alert()` is open the page's web process waits, and so does
  a URL Konqueror opens in that view until the dialog is answered;
  `onbeforeunload` is asked for links and scripts, not for URLs
  Konqueror opens.  "No more dialogs" lasts until the next page (a
  page can reload itself).
  The file chooser's filter knows accept='s mime types only (WPE does
  not expose its file extensions); remote files are refused, not
  downloaded.
* Clipboard: selecting text in a page does not set PRIMARY; a middle
  click outside text fields does not open the selection as a URL (as
  KHTML does).  Konqueror's Edit/Copy and Ctrl+C copy plain text only.
  The one second paste window is for the whole process (a compromised
  web process of another tab could read during it); a page can lay a
  text field over a link so that a middle click also pastes PRIMARY
  (as in GTK browsers and Firefox).
* Links to other schemes cannot be told apart from those of frames of
  other sites (WPE's navigation action does not say); they need a user
  gesture and KDE's URL policy.
* Favicons go through KDE's cache at 16 pixels (kded's favicons module
  downloads them once more); SVG icons need kdelibs' SVG kimgio plugin;
  the module and the ICO reader needed fixes (kde3-kdebase and
  kde3-kdelibs, branches `favicons`: a kded crash with icons over 64 KiB,
  PNG entries in ICO files, error pages taken for icons).  The icon
  arrives after the page and is taken for the page shown then.
* No password manager yet (KWallet for forms; the `submit-form` hook is
  a stub).
* Ad blocking: no count of blocked requests (WebKit does not report
  them); some uBlock scriptlets (see above), procedural cosmetics, `$redirect` and
  `$removeparam` are not supported; a link between a site where ad
  blocking is off and one where it is on loads the page twice (after a
  redirect it keeps the previous site's element hiding instead);
  `$document`/`$elemhide` exceptions and site element hiding follow the
  top page, also inside frames of other sites.
* The DMA-BUF path was tested on Xwayland (AMD), not yet on a real Xorg
  (Fedora 44 does not package WPE WebKit; the Arch build needs glibc
  2.44).

## Next stages

1. **Konqueror integration**: a Konqueror setting for the web engine
   (per protocol); PRIMARY from page selections, middle click to open a
   URL; printing (no WPE API: maybe a snapshot or a PDF via a headless
   GTK WebKit).  Done: context menus, find, zoom, copy, paste (X
   CLIPBOARD and PRIMARY), no KIO probe (Konqueror `konq-wpe`), file
   chooser, script dialogs, favicons, view source, other schemes.
2. **Downloads**: `URLArgs::postData`; unsupported GET content
   downloaded by WebKit when no part embeds it.  Done: downloads via
   KDE's dialog and KIO's progress window, POST downloads.
3. **Cookies and passwords**: KDE's cookie policy (kcookiejar), KWallet
   for HTTP authentication and forms (hook: `submit-form`; https top
   frames only, never into cross-origin frames, filled on a user
   gesture; WPE's form submission request does not tell the frame, so
   the origin check needs a script world of our own).  Done:
   persistent cookies, HTTP authentication dialog.
4. **Input**: XIM preedit through a `WPEInputMethodContext`; smooth
   XI2.1 scrolling as precise deltas with phases (with the Qt 3 XI2
   work); touchpad gestures.
5. **Frame timing**: a `WPEScreenSyncObserver` driven by Present MSC
   notifications (on its own X connection: WebKit calls it from another
   thread), explicit sync (syncobj) where the X server has it, KDE's
   font, colour and double-click settings into `WPESettings`.
6. **Settings module** (kcm): engine choice, JavaScript, plugins, user
   agent, fonts, cache; the ad blocker's page (now a dialog of the
   part).

## WebKit deadlock under memory pressure

`webkit/0002-Skia-Don-t-deadlock-when-releasing-GPU-resources-und.patch`
(applies to 2.54.3 after 0001): stock WebKit hangs the web process when it
frees GPU resources under critical memory pressure while display lists with
promise images are pending (seen while playing YouTube, above 1.5 GB).
`tests/deadlock` reproduces it.
