#!/usr/bin/env python3
# httplog.py ADDRESS LOG: an HTTP server (port 80) that logs every request
# ("<dest> <host> <method> <path>" per line) and answers with a small image,
# style sheet or font.  dest: Sec-Fetch-Dest if sent (WebKit 2.54 GLib
# does not), else guessed (marked "?"): Accept image/* -> image, text/css
# -> style, */* for a font file name -> font; anything else is "unknown"
# (fails the external check).
import base64, http.server, sys, threading

ADDR, LOG = sys.argv[1], sys.argv[2]
lock = threading.Lock()
PNG = base64.b64decode('iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg==')

class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def do_any(self):
        dest = self.headers.get('Sec-Fetch-Dest')
        accept = self.headers.get('Accept', '')
        if not dest:
            if accept.startswith('image/'):
                dest = 'image?'
            elif 'text/css' in accept:
                dest = 'style?'
            elif accept in ('', '*/*') and self.path.split('?')[0].endswith(('.woff', '.woff2', '.ttf', '.otf')):
                dest = 'font?'
            else:
                dest = 'unknown'
        with lock, open(LOG, 'a') as f:
            f.write('%s %s %s %s\n' % (dest, self.headers.get('Host', '-'), self.command, self.path))
        path = self.path.split('?')[0]
        if path.endswith('.css'):
            body, ctype = b'body { color: #000 }', 'text/css'
        elif path.endswith('.woff') or path.endswith('.woff2'):
            body, ctype = b'wOFF', 'font/woff'
        else:
            body, ctype = PNG, 'image/png'
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    do_GET = do_POST = do_HEAD = do_OPTIONS = do_any

http.server.ThreadingHTTPServer((ADDR, 80), Handler).serve_forever()
