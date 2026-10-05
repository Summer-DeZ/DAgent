"""DAgent's private SearXNG entrypoint; one ephemeral loopback listener per backend."""

import os
import signal
import sys
import threading

# Child owns the pipe's only writer. EOF also covers abrupt backend exit.
# PR_SET_PDEATHSIG would bind to the temporary tool worker thread on Linux.
def watch_backend():
    sys.stdin.buffer.read()
    os.killpg(os.getpgrp(), signal.SIGTERM)


threading.Thread(target=watch_backend, daemon=True).start()

from searx.webapp import app
from werkzeug.serving import make_server

server = make_server("127.0.0.1", 0, app, threaded=True)
print(f"DAGENT_SEARXNG_PORT={server.server_port}", flush=True)
server.serve_forever()
