#!/usr/bin/env python3
# Test server: tests/pages plus a few dynamic answers.
#
#   server.py [PORT]   (default 8383, 127.0.0.1 only)
#
#   /attachment/NAME     Content-Disposition: attachment (a download)
#   /blob?size=N         N bytes of application/octet-stream, slowly
#   /post                POST: the form data back as an attachment
#   /postraw             POST: the form data back, application/octet-stream
#   /cookie              sets a persistent and a session cookie, shows the cookies sent
#   /auth                HTTP Basic authentication (user / secret)

import base64
import os
import sys
import time
import urllib.parse
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer

PAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages')


class Handler(SimpleHTTPRequestHandler):
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
        if self.path == '/post':
            return self.send_body(b'posted: ' + data + b'\n', 'application/octet-stream',
                                  [('Content-Disposition', 'attachment; filename="posted.bin"')])
        if self.path == '/postraw':
            return self.send_body(b'posted: ' + data + b'\n', 'application/octet-stream')
        self.send_error(404)


if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8383
    ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
