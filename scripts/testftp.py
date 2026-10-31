#!/usr/bin/env python3
"""Host FTP server for the Fetch and NCSA Telnet tests (docs/dev/APPS.md), unprivileged port.
    scripts/testftp.py [port]          (default 17021; 0.0.0.0; Ctrl-C stops it)
Root: $MT_HOME/ftp (created) with big10m.bin (10 MiB of seeded pseudo-random bytes, made once;
SHA-256 printed) and a writable incoming/ for uploads. Users: anonymous (read, upload) and
moof/moof (everything). Passive ports 17100-17199. Active mode (PORT) connects back to the guest,
which only works on a TAP link (scripts/tap-up.sh), not through Snow's NAT. On TAP the guest reaches
it as 10.0.0.1:21 through tap-up.sh's redirect (Fetch 2.1.2 cannot change the port).
Needs pyftpdlib: python3 -m pip install --target "$MT_HOME/pylib" pyftpdlib"""
import hashlib
import logging
import os
import random
import sys

MT_HOME = os.environ.get("MT_HOME", os.path.expanduser("~/.local/share/mooftcp"))
sys.path.insert(0, os.path.join(MT_HOME, "pylib"))
try:
    from pyftpdlib.authorizers import DummyAuthorizer
    from pyftpdlib.handlers import FTPHandler
    from pyftpdlib.servers import FTPServer
except ImportError:
    sys.exit('testftp.py: pyftpdlib missing: python3 -m pip install --target "%s/pylib" pyftpdlib'
             % MT_HOME)

root = os.path.join(MT_HOME, "ftp")
os.makedirs(os.path.join(root, "incoming"), exist_ok=True)
big = os.path.join(root, "big10m.bin")
if not os.path.isfile(big) or os.path.getsize(big) != 10 * 1024 * 1024:
    rnd = random.Random(12)
    with open(big, "wb") as f:
        for _ in range(10 * 16):
            f.write(rnd.randbytes(65536))
print("big10m.bin sha256", hashlib.sha256(open(big, "rb").read()).hexdigest(), flush=True)

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s")
auth = DummyAuthorizer()
auth.add_user("moof", "moof", root, perm="elradfmwMT")
auth.add_anonymous(root, perm="elrw")
FTPHandler.authorizer = auth
FTPHandler.passive_ports = range(17100, 17200)
port = int(sys.argv[1]) if len(sys.argv) > 1 else 17021
FTPServer(("0.0.0.0", port), FTPHandler).serve_forever()
