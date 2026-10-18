#!/usr/bin/env python3
"""serve.py — local static server that sends the cross-origin-isolation headers.

The threaded module needs SharedArrayBuffer, which browsers only enable on a
page served with
    Cross-Origin-Opener-Policy: same-origin
    Cross-Origin-Embedder-Policy: require-corp
(the same headers dist/vercel.json sets in production). A plain
`python3 -m http.server` does not send them, so the page falls back to the
single-threaded module there.

usage:  python3 wasm/serve.py [ROOT] [PORT]
        ROOT defaults to the repo root (open /web/), PORT to 8123.
        Serves dist/ the same way:  python3 wasm/serve.py dist 8124

Binds 127.0.0.1 only. From Windows with the repo in WSL, http://localhost:PORT/
reaches it through WSL's localhost forwarding.
"""
import functools
import http.server
import sys


class IsolatedHandler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {
        **http.server.SimpleHTTPRequestHandler.extensions_map,
        ".wasm": "application/wasm",
        ".js": "text/javascript",
    }

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cache-Control", "no-store")
        super().end_headers()


def main():
    root = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8123
    handler = functools.partial(IsolatedHandler, directory=root)
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), handler)
    print(f"serving {root} with COOP/COEP on http://localhost:{port}/")
    server.serve_forever()


if __name__ == "__main__":
    main()
