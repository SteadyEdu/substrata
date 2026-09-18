#!/usr/bin/env python3
"""
Minimal Substrata protocol client, for exercising chatbots without a GUI client.

Connects to a Substrata server, logs in, creates an avatar standing in front of a
chatbot and looking at it, says something, and prints whatever comes back - noting
which replies arrived on a private channel.

This exists so chatbot behaviour can be tested, and regression-tested, without
building and driving the full 3D client.

Usage:
    python3 chatbot_test_client.py --world edutest/grade6 --username edutest \
        --password TestPassword123 --bot-pos 0,0,1.7 --say "what is 7 times 8?"
"""

import argparse
import socket
import ssl
import struct
import sys
import threading
import time

# See shared/Protocol.h.  Both ends call setUseNetworkByteOrder(false), so everything is little-endian.
CYBERSPACE_HELLO = 1357924680
PROTOCOL_VERSION = 54
CLIENT_PROTOCOL_OK = 10000
CLIENT_PROTOCOL_TOO_OLD = 10001
CLIENT_PROTOCOL_TOO_NEW = 10002
CONNECTION_TYPE_UPDATES = 500

CREATE_AVATAR = 1004
AVATAR_TRANSFORM_UPDATE = 1002
AVATAR_CREATED = 1000
AVATAR_IS_HERE = 1005
AVATAR_FULL_UPDATE = 1003
CHAT_MESSAGE_ID = 2000
USER_MOVED_NEAR_TO_AVATAR = 1200
LOG_IN_MESSAGE = 8000
LOGGED_IN_MESSAGE_ID = 8003

# Client capabilities.
STREAMING_COMPRESSED_OBJECT_SUPPORT = 0x1
SENDS_USER_MOVED_CHATBOT_MSGS = 0x2
PRIVATE_CHAT_SUPPORT = 0x4

CHAT_MESSAGE_FLAG_PRIVATE = 0x1


class Conn:
    def __init__(self, sock):
        self.sock = sock
        self.lock = threading.Lock()

    # ---- raw reads, used during the handshake ----
    def read_exactly(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise EOFError('server closed the connection')
            buf += chunk
        return buf

    def read_u32(self):
        return struct.unpack('<I', self.read_exactly(4))[0]

    def read_i32(self):
        return struct.unpack('<i', self.read_exactly(4))[0]

    def read_u64(self):
        return struct.unpack('<Q', self.read_exactly(8))[0]

    def read_string(self):
        n = self.read_u32()
        if n > 10_000_000:
            raise ValueError('implausible string length %d' % n)
        return self.read_exactly(n).decode('utf-8', 'replace')

    def write(self, data):
        with self.lock:
            self.sock.sendall(data)


def s_u32(x):
    return struct.pack('<I', x)


def s_u64(x):
    return struct.pack('<Q', x)


def s_str(s):
    b = s.encode('utf-8')
    return s_u32(len(b)) + b


def s_vec3d(v):
    return struct.pack('<ddd', *v)


def s_vec3f(v):
    return struct.pack('<fff', *v)


def message(msg_type, payload=b''):
    """Frame a message: uint32 type, uint32 total length (including this header), payload."""
    total_len = 8 + len(payload)
    return s_u32(msg_type) + s_u32(total_len) + payload


def avatar_settings_bytes(model_url=''):
    # model_url, material count, then the 4x4 pre_ob_to_world matrix.
    return s_str(model_url) + s_u32(0) + struct.pack('<16f', *([0.0] * 16))


def parse_string(buf, off):
    (n,) = struct.unpack_from('<I', buf, off)
    off += 4
    s = buf[off:off + n].decode('utf-8', 'replace')
    return s, off + n


class Reader(threading.Thread):
    """Frames and decodes incoming messages.  Everything after the handshake is (type, len) framed."""

    daemon = True

    def __init__(self, conn, on_chat, on_logged_in):
        super().__init__()
        self.conn = conn
        self.on_chat = on_chat
        self.on_logged_in = on_logged_in
        self.running = True
        self.error = None
        self.avatars = {}  # avatar UID -> name, learned from the avatar messages the server sends.

    def run(self):
        try:
            while self.running:
                header = self.conn.read_exactly(8)
                msg_type, msg_len = struct.unpack('<II', header)
                if msg_len < 8 or msg_len > 100_000_000:
                    raise ValueError('bad message length %d for type %d' % (msg_len, msg_type))
                body = self.conn.read_exactly(msg_len - 8)

                if msg_type == CHAT_MESSAGE_ID:
                    name, off = parse_string(body, 0)
                    text, off = parse_string(body, off)
                    flags = 0
                    if off + 8 <= len(body):
                        off += 8  # sender avatar UID
                        if off + 4 <= len(body):
                            (flags,) = struct.unpack_from('<I', body, off)
                    self.on_chat(name, text, bool(flags & CHAT_MESSAGE_FLAG_PRIVATE))
                elif msg_type in (AVATAR_IS_HERE, AVATAR_CREATED, AVATAR_FULL_UPDATE):
                    # Avatar records start with the UID and the name; the rest we do not need.
                    if len(body) >= 12:
                        (uid,) = struct.unpack_from('<Q', body, 0)
                        try:
                            name, _ = parse_string(body, 8)
                            self.avatars[uid] = name
                        except (struct.error, UnicodeDecodeError):
                            pass
                elif msg_type == LOGGED_IN_MESSAGE_ID:
                    self.on_logged_in()
        except Exception as e:  # noqa: BLE001 - a test tool; report and stop.
            if self.running:
                self.error = e


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='localhost')
    ap.add_argument('--port', type=int, default=7600)
    ap.add_argument('--world', default='', help="World to connect to.  Empty string is the root world.")
    ap.add_argument('--username', required=True)
    ap.add_argument('--password', required=True)
    ap.add_argument('--bot-pos', default='0,0,1.7', help="Where the chatbot is, as x,y,z.")
    ap.add_argument('--bot-name', default=None, help="Name of the bot's avatar.  Defaults to any other avatar present.")
    ap.add_argument('--stand-back', type=float, default=2.0, help="How far in front of the bot to stand, in metres.")
    ap.add_argument('--say', action='append', default=[], help="A message to send.  Repeatable.")
    ap.add_argument('--greet-wait', type=float, default=25.0,
                    help="Seconds to wait for the bot to notice us and greet before saying anything.")
    ap.add_argument('--reply-wait', type=float, default=90.0, help="Seconds to wait for a reply after each message.")
    args = ap.parse_args()

    bot_pos = [float(v) for v in args.bot_pos.split(',')]

    # Stand --stand-back metres away along -x and look towards +x, straight at the bot: the bot only starts a
    # conversation with someone who has been looking at it (see isOtherAvatarAttendingToOurAvatar in ChatBot.cpp).
    my_pos = [bot_pos[0] - args.stand_back, bot_pos[1], bot_pos[2]]
    my_rotation = [0.0, 0.0, 0.0]  # rotation.z is the heading; 0 = looking along +x.

    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE  # Dev servers use a self-signed certificate.

    raw = socket.create_connection((args.host, args.port), timeout=30)
    sock = ctx.wrap_socket(raw, server_hostname=args.host)
    conn = Conn(sock)

    # ---- Handshake.  Mirrors ClientThread::doRun(). ----
    conn.write(s_u32(CYBERSPACE_HELLO) + s_u32(PROTOCOL_VERSION) + s_u32(CONNECTION_TYPE_UPDATES) + s_str(args.world))

    if conn.read_u32() != CYBERSPACE_HELLO:
        sys.exit('server did not return the expected hello')

    response = conn.read_u32()
    if response in (CLIENT_PROTOCOL_TOO_OLD, CLIENT_PROTOCOL_TOO_NEW):
        sys.exit('server rejected our protocol version: ' + conn.read_string())
    if response != CLIENT_PROTOCOL_OK:
        sys.exit('unexpected protocol response %d' % response)

    peer_version = conn.read_u32()
    if peer_version >= 41:
        conn.read_u32()  # server capabilities
    if peer_version >= 43:
        conn.read_i32()  # optimised mesh version

    my_avatar_uid = conn.read_u64()
    print('connected: server protocol %d, our avatar UID %d' % (peer_version, my_avatar_uid))

    if peer_version >= 42:
        conn.write(s_u32(STREAMING_COMPRESSED_OBJECT_SUPPORT | SENDS_USER_MOVED_CHATBOT_MSGS | PRIVATE_CHAT_SUPPORT))

    replies = []   # Messages from the bot.
    echoes = []    # Our own messages, echoed back by the server.
    logged_in = threading.Event()

    def on_chat(name, text, private):
        if name == args.username:  # The server echoes our own messages back to us.
            print('  (echo of our message%s)' % (', private' if private else ', PUBLIC'), flush=True)
            echoes.append((text, private))
            return
        replies.append((name, text, private))
        print('\n  [%s]%s %s' % (name, ' (private)' if private else '', text), flush=True)

    reader = Reader(conn, on_chat, logged_in.set)
    reader.start()

    # ---- Log in.  Chat is refused unless the connection is logged in. ----
    conn.write(message(LOG_IN_MESSAGE, s_str(args.username) + s_str(args.password)))
    if not logged_in.wait(timeout=15):
        sys.exit('did not receive a LoggedIn message - check the username and password')
    print('logged in as %s' % args.username)

    # ---- Create our avatar, standing in front of the bot and facing it. ----
    avatar = s_u64(my_avatar_uid) + s_str(args.username) + s_vec3d(my_pos) + s_vec3f(my_rotation) + avatar_settings_bytes()
    conn.write(message(CREATE_AVATAR, avatar))
    print('avatar created at %s, looking at the bot at %s' % (my_pos, bot_pos))

    # Keep the transform fresh so the bot's attention timer keeps running.
    def keep_alive():
        while reader.running:
            try:
                conn.write(message(AVATAR_TRANSFORM_UPDATE,
                                   s_u64(my_avatar_uid) + s_vec3d(my_pos) + s_vec3f(my_rotation) + s_u32(0)))
            except OSError:
                return
            time.sleep(1.0)

    threading.Thread(target=keep_alive, daemon=True).start()

    # Find the bot's avatar and tell the server we have moved next to it, which is what makes the bot start
    # paying attention to us (ChatBot::userMovedNearToBotAvatar).
    bot_uid = None
    deadline = time.time() + 10
    while time.time() < deadline and bot_uid is None:
        for uid, name in list(reader.avatars.items()):
            if uid != my_avatar_uid and (args.bot_name is None or name == args.bot_name):
                bot_uid = uid
                print('found avatar %d named %r - treating it as the bot' % (uid, name))
                break
        time.sleep(0.25)

    if bot_uid is not None:
        conn.write(message(USER_MOVED_NEAR_TO_AVATAR, s_u64(bot_uid)))
    else:
        print('WARNING: did not find the bot avatar; it may not respond')

    # The bot needs to notice us before it will converse: it wants ~1.5s of being looked at, and the greeting it
    # then sends goes to the LLM, which takes a while on a local model.
    print('waiting up to %.0fs for the bot to notice us...' % args.greet_wait, end='', flush=True)
    deadline = time.time() + args.greet_wait
    while time.time() < deadline and not replies:
        time.sleep(0.5)
    print()

    for text in args.say:
        print('\n> %s' % text, flush=True)
        before = len(replies)
        conn.write(message(CHAT_MESSAGE_ID, s_str(text)))

        deadline = time.time() + args.reply_wait
        while time.time() < deadline:
            if len(replies) > before:
                # Give the bot a moment to finish streaming further sentences.
                quiet_until = time.time() + 6.0
                last = len(replies)
                while time.time() < quiet_until:
                    time.sleep(0.5)
                    if len(replies) != last:
                        last = len(replies)
                        quiet_until = time.time() + 6.0
                break
            time.sleep(0.5)
        else:
            print('  (no reply within %.0fs)' % args.reply_wait)

    reader.running = False
    try:
        sock.close()
    except OSError:
        pass

    print('\n--- %d bot message(s), %d of them private ---'
          % (len(replies), sum(1 for r in replies if r[2])))
    print('--- %d echo(es) of our own messages, %d of them private ---'
          % (len(echoes), sum(1 for e in echoes if e[1])))
    if reader.error:
        print('reader stopped with: %r' % reader.error)
    return 0 if replies else 1


if __name__ == '__main__':
    sys.exit(main())
