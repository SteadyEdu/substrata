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
            try:
                chunk = self.sock.recv(n - len(buf))
            except (BlockingIOError, ssl.SSLWantReadError):
                # "No data yet" is not an error.  A socket created with a timeout is non-blocking underneath, and a
                # TLS record can arrive in pieces, so this fires in normal operation.  Treating it as fatal killed the
                # reader mid-run and made a perfectly healthy server look like a model that had stopped answering.
                time.sleep(0.01)
                continue
            except ssl.SSLError as e:
                if 'WANT_READ' in str(e) or 'WANT_WRITE' in str(e):
                    time.sleep(0.01)
                    continue
                raise
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
        self.decode_errors = 0
        self.first_decode_error = None
        self.avatars = {}  # avatar UID -> name, learned from the avatar messages the server sends.

    def run(self):
        # A decoding problem in one message must not stop us reading the rest.  An earlier version let any exception
        # kill this thread silently: the socket stayed open, questions kept being sent and answered, and every reply
        # from that point was simply never seen.  The run then looked like a model that degraded halfway through,
        # which is a far more plausible and far more misleading story than "the test harness broke".
        try:
            while self.running:
                header = self.conn.read_exactly(8)
                msg_type, msg_len = struct.unpack('<II', header)
                if msg_len < 8 or msg_len > 100_000_000:
                    raise ValueError('bad message length %d for type %d' % (msg_len, msg_type))
                body = self.conn.read_exactly(msg_len - 8)

                try:
                    self._handle(msg_type, body)
                except Exception as e:  # noqa: BLE001 - one bad message, keep reading.
                    self.decode_errors += 1
                    if self.first_decode_error is None:
                        self.first_decode_error = 'msg type %d, %d bytes: %r' % (msg_type, len(body), e)
        except Exception as e:  # noqa: BLE001 - connection level: nothing more will arrive.
            if self.running:
                self.error = e

    def _handle(self, msg_type, body):
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



class Session:
    """A logged-in connection with an avatar standing in front of a chatbot.

    Separated from the command line so that scripted evaluation (chatbot_eval.py) can drive a bot the same way a
    student would, rather than testing the model through a different path than the one students use.
    """

    def __init__(self, host, port, world, username, password, bot_pos, stand_back=2.0, bot_name=None):
        self.username = username
        self.bot_name = bot_name
        self.replies = []   # (name, text, private) from anyone but us.
        self.echoes = []    # (text, private) - our own messages, echoed back.
        self._logged_in = threading.Event()
        self._consumed = 0  # Replies already attributed to a question.

        bot_pos = list(bot_pos)
        # Stand in front of the bot and look straight at it: a bot only converses with someone who has been looking
        # at it (isOtherAvatarAttendingToOurAvatar in ChatBot.cpp).
        self.my_pos = [bot_pos[0] - stand_back, bot_pos[1], bot_pos[2]]
        self.my_rotation = [0.0, 0.0, 0.0]  # rotation.z is the heading; 0 = looking along +x.

        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # Dev servers use a self-signed certificate.

        raw = socket.create_connection((host, port), timeout=30)
        self.sock = ctx.wrap_socket(raw, server_hostname=host)
        # Back to blocking once the handshake is done: the reader is a dedicated thread whose whole job is to wait for
        # the next message, and timeouts are handled per question in say() instead.
        self.sock.settimeout(None)
        self.conn = Conn(self.sock)

        self._handshake(world)
        self._start_reader()
        self._log_in(username, password)
        self._create_avatar(username)
        self._greet_bot()

    # ---- setup ----
    def _handshake(self, world):
        conn = self.conn
        conn.write(s_u32(CYBERSPACE_HELLO) + s_u32(PROTOCOL_VERSION) + s_u32(CONNECTION_TYPE_UPDATES) + s_str(world))

        if conn.read_u32() != CYBERSPACE_HELLO:
            raise RuntimeError('server did not return the expected hello')

        response = conn.read_u32()
        if response in (CLIENT_PROTOCOL_TOO_OLD, CLIENT_PROTOCOL_TOO_NEW):
            raise RuntimeError('server rejected our protocol version: ' + conn.read_string())
        if response != CLIENT_PROTOCOL_OK:
            raise RuntimeError('unexpected protocol response %d' % response)

        peer_version = conn.read_u32()
        if peer_version >= 41:
            conn.read_u32()  # server capabilities
        if peer_version >= 43:
            conn.read_i32()  # optimised mesh version

        self.my_avatar_uid = conn.read_u64()
        self.peer_version = peer_version

        if peer_version >= 42:
            conn.write(s_u32(STREAMING_COMPRESSED_OBJECT_SUPPORT | SENDS_USER_MOVED_CHATBOT_MSGS | PRIVATE_CHAT_SUPPORT))

    def _on_chat(self, name, text, private):
        if name == self.username:  # The server echoes our own messages back to us.
            self.echoes.append((text, private))
            return
        self.replies.append((name, text, private))

    def _start_reader(self):
        self.reader = Reader(self.conn, self._on_chat, self._logged_in.set)
        self.reader.start()

    def _log_in(self, username, password):
        self.conn.write(message(LOG_IN_MESSAGE, s_str(username) + s_str(password)))
        if not self._logged_in.wait(timeout=15):
            raise RuntimeError('did not receive a LoggedIn message - check the username and password')

    def _create_avatar(self, username):
        avatar = (s_u64(self.my_avatar_uid) + s_str(username) + s_vec3d(self.my_pos) + s_vec3f(self.my_rotation) +
                  avatar_settings_bytes())
        self.conn.write(message(CREATE_AVATAR, avatar))

        def keep_alive():
            while self.reader.running:
                try:
                    self.conn.write(message(AVATAR_TRANSFORM_UPDATE,
                                            s_u64(self.my_avatar_uid) + s_vec3d(self.my_pos) +
                                            s_vec3f(self.my_rotation) + s_u32(0)))
                except OSError:
                    return
                time.sleep(1.0)

        threading.Thread(target=keep_alive, daemon=True).start()

    def _greet_bot(self):
        """Find the bot's avatar and tell the server we have walked up to it."""
        self.bot_uid = None
        deadline = time.time() + 10
        while time.time() < deadline and self.bot_uid is None:
            for uid, name in list(self.reader.avatars.items()):
                if uid != self.my_avatar_uid and (self.bot_name is None or name == self.bot_name):
                    self.bot_uid = uid
                    self.bot_avatar_name = name
                    break
            time.sleep(0.25)

        if self.bot_uid is not None:
            self.conn.write(message(USER_MOVED_NEAR_TO_AVATAR, s_u64(self.bot_uid)))

    # ---- use ----
    def wait_for_greeting(self, timeout):
        """Wait for the bot to notice us.  Returns True if it said anything."""
        deadline = time.time() + timeout
        while time.time() < deadline and not self.replies:
            time.sleep(0.25)
        return bool(self.replies)

    def settle(self, quiet_period=5.0, timeout=30.0):
        """Wait until no reply has arrived for quiet_period, and report anything that turned up.

        Without this, a slow model silently corrupts a scripted run: the reply to question N arrives after we have
        already sent question N+1, and every answer from then on is attributed to the wrong question.  That happened
        with a 2b model answering in ~25s, and the results looked like nonsense from the model rather than a fault in
        the harness.  Settling before each question keeps replies and questions lined up, and returns any late ones so
        the caller can say so rather than quietly mis-scoring them.
        """
        deadline = time.time() + timeout
        seen = len(self.replies)
        quiet_until = time.time() + quiet_period
        while time.time() < quiet_until and time.time() < deadline:
            time.sleep(0.25)
            if len(self.replies) != seen:
                seen = len(self.replies)
                quiet_until = time.time() + quiet_period

        late = self.replies[self._consumed:]
        self._consumed = len(self.replies)
        return late

    def say(self, text, reply_timeout=90.0, quiet_period=5.0):
        """Send a message and return the bot's reply as a single string.

        The bot streams its answer a sentence at a time, so wait for a quiet period after the first sentence rather
        than taking only the first one.  Returns '' if nothing came back in time.
        """
        self.settle(quiet_period=quiet_period)  # Flush any reply still in flight from the previous question.

        if self.reader.error is not None:
            raise RuntimeError('connection to the server is dead: %r' % (self.reader.error,))

        before = len(self.replies)
        self.conn.write(message(CHAT_MESSAGE_ID, s_str(text)))

        deadline = time.time() + reply_timeout
        while time.time() < deadline and len(self.replies) == before:
            time.sleep(0.25)

        if len(self.replies) == before:
            return ''

        quiet_until = time.time() + quiet_period
        last = len(self.replies)
        while time.time() < quiet_until:
            time.sleep(0.25)
            if len(self.replies) != last:
                last = len(self.replies)
                quiet_until = time.time() + quiet_period

        self._consumed = len(self.replies)
        return ' '.join(r[1].strip() for r in self.replies[before:])

    def all_replies_private(self):
        return all(r[2] for r in self.replies) and all(e[1] for e in self.echoes)

    def close(self):
        self.reader.running = False
        try:
            self.sock.close()
        except OSError:
            pass


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
    ap.add_argument('--greet-wait', type=float, default=45.0,
                    help="Seconds to wait for the bot to notice us and greet before saying anything.")
    ap.add_argument('--reply-wait', type=float, default=90.0, help="Seconds to wait for a reply after each message.")
    args = ap.parse_args()

    bot_pos = [float(v) for v in args.bot_pos.split(',')]

    session = Session(args.host, args.port, args.world, args.username, args.password, bot_pos,
                      stand_back=args.stand_back, bot_name=args.bot_name)
    print('connected: our avatar UID %d' % session.my_avatar_uid)
    print('logged in as %s' % args.username)
    print('avatar created at %s, looking at the bot at %s' % (session.my_pos, bot_pos))
    if session.bot_uid is None:
        print('WARNING: did not find the bot avatar; it may not respond')
    else:
        print('found avatar %d named %r - treating it as the bot' % (session.bot_uid, session.bot_avatar_name))

    print('waiting up to %.0fs for the bot to notice us...' % args.greet_wait)
    if session.wait_for_greeting(args.greet_wait):
        for name, text, private in session.replies:
            print('\n  [%s]%s %s' % (name, ' (private)' if private else '', text))

    for text in args.say:
        print('\n> %s' % text, flush=True)
        reply = session.say(text, reply_timeout=args.reply_wait)
        if reply:
            print('  %s' % reply)
        else:
            print('  (no reply within %.0fs)' % args.reply_wait)

    session.close()

    print('\n--- %d bot message(s), %d of them private ---'
          % (len(session.replies), sum(1 for r in session.replies if r[2])))
    print('--- %d echo(es) of our own messages, %d of them private ---'
          % (len(session.echoes), sum(1 for e in session.echoes if e[1])))
    return 0 if session.replies else 1


if __name__ == '__main__':
    sys.exit(main())
