"""Local dev server for gpu_video_demo, with the COOP/COEP headers -pthread
needs for SharedArrayBuffer. See README.md section 3.8. Usage:

    python3 serve_coop_coep.py [directory] [port]
"""

import functools
import http.server
import sys


class Handler(http.server.SimpleHTTPRequestHandler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


if __name__ == "__main__":
    directory = sys.argv[1] if len(sys.argv) > 1 else "."
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 8080
    handler = functools.partial(Handler, directory=directory)
    http.server.test(HandlerClass=handler, port=port)
