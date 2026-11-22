#!/usr/bin/env python3
# Test server: tests/pages plus a few dynamic answers.
#
#   server.py [PORT] [ADDRESS]   (default 8383, 127.0.0.1; another
#                                 loopback address is another site)
#
#   /attachment/NAME     Content-Disposition: attachment (a download)
#   /blob?size=N         N bytes of application/octet-stream, slowly
#   /post                POST: the form data back as an attachment
#   /postraw             POST: the form data back, application/octet-stream
#   /cookie              sets a persistent and a session cookie, shows the cookies sent
#   /auth                HTTP Basic authentication (user / secret)
#   /ads/*, /track/*, /res/*, /regex-ad*, /sep-ad*, /ads-redirected.js
#                        ad-like resources for pages/adblock.html (an image,
#                        a script or a frame, by extension)
#   /frames/*            a small third-party frame
#   /typed?type=T&name=N a small body of content type T (application/pdf: a
#                        real one-page PDF), Content-Disposition inline; N
#   /youtubei/v1/player  JSON shaped like YouTube's player answer, with ads
#                        (adPlacements, playerAds, adSlots), GET or POST
#   /replace.json        JSON with "adSlots" (text replacement test)
#   /ads-api/*           must never be requested (pages/scriptlets.html)
#   /slowscript          an inline script that arrives in two pieces
#   /report?host=&name=&expected=&result=
#                        pages/adblock.html reports a result: printed on stdout

import base64
import os
import sys
import time
import urllib.parse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

PAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages')


# A minimal one-page PDF.
PDF = (b'%PDF-1.4\n1 0 obj<</Type/Catalog/Pages 2 0 R>>endobj\n'
       b'2 0 obj<</Type/Pages/Kids[3 0 R]/Count 1>>endobj\n'
       b'3 0 obj<</Type/Page/Parent 2 0 R/MediaBox[0 0 300 200]/Contents 4 0 R'
       b'/Resources<</Font<</F1 5 0 R>>>>>>endobj\n'
       b'4 0 obj<</Length 44>>stream\nBT /F1 24 Tf 40 100 Td (kwpepart PDF) Tj ET\nendstream endobj\n'
       b'5 0 obj<</Type/Font/Subtype/Type1/BaseFont/Helvetica>>endobj\n'
       b'trailer<</Root 1 0 R>>\n%%EOF\n')


# YouTube's pattern: the ads come in the same answer as the video.
PLAYER = (b'{"responseContext":{},"playabilityStatus":{"status":"OK"},'
          b'"videoDetails":{"videoId":"test","title":"kwpepart"},'
          b'"adPlacements":[{"adPlacementRenderer":{"kind":"preroll"}}],'
          b'"playerAds":[{"playerLegacyDesktopWatchAdsRenderer":{}}],'
          b'"adSlots":[{"adSlotRenderer":{}}]}')


class Handler(SimpleHTTPRequestHandler):
    def log_message(self, fmt, *args):
        # Who asked: KIO (Konqueror's mime type probe) or WebKit.
        agent = self.headers.get('User-Agent', '') if hasattr(self, 'headers') and self.headers else ''
        sys.stderr.write('%s [%s]\n' % (fmt % args, 'KIO' if 'Konqueror' in agent else 'WebKit' if 'WebKit' in agent else agent))

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=PAGES, **kwargs)

    def send_body(self, body, ctype, extra=()):
        self.send_response(200)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        url = urllib.parse.urlparse(self.path)
        query = urllib.parse.parse_qs(url.query)
        if url.path.startswith('/attachment/'):
            name = url.path.split('/')[-1]
            body = ('attachment %s\n' % name).encode() * 100
            return self.send_body(body, 'text/plain',
                                  [('Content-Disposition', 'attachment; filename="%s"' % name)])
        if url.path == '/blob':
            size = int(query.get('size', ['1000000'])[0])
            self.send_response(200)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(size))
            self.end_headers()
            chunk = b'x' * 65536
            sent = 0
            while sent < size:
                n = min(len(chunk), size - sent)
                try:
                    self.wfile.write(chunk[:n])
                except (BrokenPipeError, ConnectionResetError):
                    return
                sent += n
                time.sleep(0.05)
            return
        if url.path == '/cookie':
            sent = self.headers.get('Cookie', '')
            body = ('<html><head><title>cookies</title></head><body><p id="c">Cookies sent: [%s]</p>'
                    '<script>localStorage.visits = (+(localStorage.visits || 0)) + 1;'
                    'document.write("<p id=ls>localStorage visits: " + localStorage.visits + "</p>")</script>'
                    '</body></html>' % sent).encode()
            return self.send_body(body, 'text/html; charset=utf-8',
                                  [('Set-Cookie', 'persistent=yes; Max-Age=86400; Path=/'),
                                   ('Set-Cookie', 'session=yes; Path=/')])
        if url.path == '/typed':
            ctype = query.get('type', ['text/plain'])[0]
            name = query.get('name', [''])[0]
            body = PDF if ctype == 'application/pdf' else ('typed %s\n' % ctype).encode() * 20
            extra = [('Content-Disposition', 'inline; filename="%s"' % name)] if name else []
            return self.send_body(body, ctype, extra)
        if url.path in ('/youtubei/v1/player', '/replace.json'):
            return self.send_body(PLAYER, 'application/json')
        if url.path == '/slowscript':
            self.send_response(200)
            self.send_header('Content-Type', 'text/html')
            self.end_headers()
            self.wfile.write(b'<!DOCTYPE html><html><body><script>var first = 1;\n' + b'/* padding */\n' * 2000)
            self.wfile.flush()
            time.sleep(0.5)
            self.wfile.write(b'/* AD_SCRIPT_MARK */ window.slowRan = true;</script>'
                             b'<script>parent.postMessage({ slow: String(window.slowRan) }, "*");</script></body></html>')
            self.close_connection = True
            return
        if url.path.startswith('/ads-api/'):
            print('SERVED %s' % url.path, flush=True)
            return self.send_body(b'ad answer', 'text/plain')
        if url.path == '/report':
            print('REPORT %s: %s | expected %s | got %s' % (query.get('host', [''])[0], query.get('name', [''])[0],
                                                           query.get('expected', [''])[0], query.get('result', [''])[0]),
                  flush=True)
            return self.send_body(b'', 'text/plain')
        if url.path.startswith(('/ads/', '/track/', '/res/', '/regex-ad', '/sep-ad', '/ads-redirected')):
            print('SERVED %s' % url.path, flush=True)
            if url.path.endswith('.js'):
                return self.send_body(b'window.adLoaded = true;\n', 'application/javascript')
            with open(os.path.join(PAGES, 'img.png'), 'rb') as f:
                return self.send_body(f.read(), 'image/png')
        if url.path.startswith('/frames/'):
            print('SERVED %s' % url.path, flush=True)
            return self.send_body(b'<html><body style="background:#fdd">third-party frame (blocked: empty)</body></html>',
                                  'text/html')
        if url.path == '/auth':
            expected = 'Basic ' + base64.b64encode(b'user:secret').decode()
            if self.headers.get('Authorization') != expected:
                self.send_response(401)
                self.send_header('WWW-Authenticate', 'Basic realm="kwpepart test"')
                self.send_header('Content-Length', '0')
                self.end_headers()
                return
            return self.send_body(b'<html><body><h1>Authenticated</h1></body></html>', 'text/html')
        return super().do_GET()

    def do_POST(self):
        length = int(self.headers.get('Content-Length', '0'))
        data = self.rfile.read(length)
        if self.path.startswith('/youtubei/v1/player'):
            return self.send_body(PLAYER, 'application/json')
        if self.path == '/post':
            return self.send_body(b'posted: ' + data + b'\n', 'application/octet-stream',
                                  [('Content-Disposition', 'attachment; filename="posted.bin"')])
        if self.path == '/postraw':
            return self.send_body(b'posted: ' + data + b'\n', 'application/octet-stream')
        self.send_error(404)


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8383
    address = sys.argv[2] if len(sys.argv) > 2 else '127.0.0.1'
    if not address.startswith('127.'):
        sys.exit('server.py: loopback addresses only')
    ThreadingHTTPServer((address, port), Handler).serve_forever()
