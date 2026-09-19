#!/usr/bin/env python3
"""
Talk to the browser on a Quest over adb, so the web client can be debugged where it actually runs.

Everything about the headset that matters - whether a draw landed, what the GL state is, what the console said -
is invisible from the machine serving the page.  This connects to the Chrome DevTools Protocol endpoint the
Quest browser exposes, which gives all of it directly rather than through a person reading numbers aloud.

    python3 quest_devtools.py targets                 # list the pages the headset has open
    python3 quest_devtools.py eval "<expression>"     # evaluate JavaScript in the first substrata page
    python3 quest_devtools.py console                 # stream console messages until interrupted

Requires the headset in developer mode, connected over USB, and authorised.  No third-party packages: the
WebSocket framing is done here so this works with whatever Python is to hand.
"""
import base64, json, os, re, socket, struct, subprocess, sys, urllib.request

ADB = os.environ.get("ADB", "/opt/homebrew/bin/adb")
PORT = 9222


def sh(args):
    return subprocess.run(args, capture_output=True, text=True).stdout.strip()


def ensure_forwarded():
    devices = [l for l in sh([ADB, "devices"]).splitlines()[1:] if l.strip() and "device" in l]
    if not devices:
        sys.exit("No device. Check: developer mode on, USB connected, and the headset's "
                 "'Allow USB debugging' prompt accepted (put it on to see the prompt).")
    # The Quest browser exposes DevTools on this abstract socket, same as Chrome on Android.
    for name in ("chrome_devtools_remote", "com.oculus.browser_devtools_remote"):
        out = subprocess.run([ADB, "forward", "tcp:%d" % PORT, "localabstract:" + name],
                             capture_output=True, text=True)
        if out.returncode == 0:
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/json/version" % PORT, timeout=3).read()
                return name
            except Exception:
                continue
    sys.exit("adb sees the headset but its browser is not exposing DevTools. Open a tab in the Quest browser.")


def targets():
    raw = urllib.request.urlopen("http://127.0.0.1:%d/json" % PORT, timeout=5).read()
    return json.loads(raw)


def pick_page(match="webclient"):
    for t in targets():
        if t.get("type") == "page" and match in (t.get("url") or ""):
            return t
    for t in targets():
        if t.get("type") == "page":
            return t
    sys.exit("No page open in the headset's browser.")


class WS:
    """Enough of RFC 6455 to drive DevTools: a client handshake, masked text frames, no extensions."""

    def __init__(self, url):
        m = re.match(r"ws://([^:/]+):(\d+)(/.*)", url)
        host, port, path = m.group(1), int(m.group(2)), m.group(3)
        self.sock = socket.create_connection((host, port), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(("GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                           "Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n"
                           % (path, host, port, key)).encode())
        buf = b""
        while b"\r\n\r\n" not in buf:
            buf += self.sock.recv(4096)
        if b"101" not in buf.split(b"\r\n")[0]:
            raise RuntimeError("DevTools refused the WebSocket upgrade: " + buf.split(b"\r\n")[0].decode())
        self.rest = buf.split(b"\r\n\r\n", 1)[1]

    def send(self, obj):
        payload = json.dumps(obj).encode()
        header = bytearray([0x81])
        n = len(payload)
        if n < 126:
            header.append(0x80 | n)
        elif n < 65536:
            header.append(0x80 | 126); header += struct.pack(">H", n)
        else:
            header.append(0x80 | 127); header += struct.pack(">Q", n)
        mask = os.urandom(4)
        header += mask
        self.sock.sendall(bytes(header) + bytes(b ^ mask[i % 4] for i, b in enumerate(payload)))

    def _read(self, n):
        while len(self.rest) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise RuntimeError("DevTools closed the connection")
            self.rest += chunk
        out, self.rest = self.rest[:n], self.rest[n:]
        return out

    def recv(self):
        while True:
            b0, b1 = self._read(2)
            opcode, length = b0 & 0x0F, b1 & 0x7F
            if length == 126:
                length = struct.unpack(">H", self._read(2))[0]
            elif length == 127:
                length = struct.unpack(">Q", self._read(8))[0]
            payload = self._read(length)
            if opcode == 0x8:
                raise RuntimeError("DevTools closed the connection")
            if opcode == 0x9:  # ping: answering keeps the session alive during a long console watch
                continue
            if opcode in (0x1, 0x2):
                return json.loads(payload)


def connect():
    ensure_forwarded()
    page = pick_page()
    ws = WS(page["webSocketDebuggerUrl"])
    return ws, page


def evaluate(expr):
    ws, page = connect()
    ws.send({"id": 1, "method": "Runtime.enable"})
    # userGesture grants transient activation, so things that require a user action - starting an XR session,
    # above all - can be driven from here instead of asking someone to look at a button and press it.
    ws.send({"id": 2, "method": "Runtime.evaluate",
             "params": {"expression": expr, "returnByValue": True, "awaitPromise": True,
                        "userGesture": True}})
    while True:
        msg = ws.recv()
        if msg.get("id") == 2:
            res = msg.get("result", {})
            if "exceptionDetails" in res:
                print("EXCEPTION:", json.dumps(res["exceptionDetails"], indent=2))
            else:
                print(json.dumps(res.get("result", {}).get("value"), indent=2))
            return


def console():
    ws, page = connect()
    print("Watching %s - interrupt to stop.\n" % page.get("url"))
    ws.send({"id": 1, "method": "Runtime.enable"})
    ws.send({"id": 2, "method": "Log.enable"})
    while True:
        msg = ws.recv()
        if msg.get("method") == "Runtime.consoleAPICalled":
            p = msg["params"]
            vals = " ".join(str(a.get("value", a.get("description", ""))) for a in p.get("args", []))
            print("[%s] %s" % (p.get("type"), vals), flush=True)
        elif msg.get("method") == "Log.entryAdded":
            e = msg["params"]["entry"]
            print("[%s/%s] %s" % (e.get("source"), e.get("level"), e.get("text")), flush=True)
        elif msg.get("method") == "Runtime.exceptionThrown":
            d = msg["params"]["exceptionDetails"]
            print("[exception] %s" % d.get("text"), flush=True)


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "targets"
    if cmd == "targets":
        ensure_forwarded()
        for t in targets():
            print("%-8s %s\n         %s" % (t.get("type"), t.get("title"), t.get("url")))
    elif cmd == "eval":
        evaluate(sys.argv[2])
    elif cmd == "console":
        console()
    else:
        sys.exit(__doc__)
