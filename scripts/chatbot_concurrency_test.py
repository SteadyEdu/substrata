#!/usr/bin/env python3
"""
Put several students in front of a chatbot at once, and see what happens.

PINNED: intended for the staging environment, not a laptop.  A single student already waits about 12 seconds for a
reply from a 9b model on an M-series laptop; running a classroom's worth of students against that will measure the
hardware rather than the software.  Run it on the staging box, against the model you actually intend to deploy.

It answers three questions, in order of how much they matter:

1. Does a private conversation stay private when several are happening at once?  This is the one that must never
   fail.  Each student sends a unique marker, and the test asserts no student ever receives another's marker or
   another's reply.  A leak here is a child reading another child's tutoring session.

2. How many students does one chatbot actually serve?  A bot with private conversations turned on takes one student
   at a time and tells the rest it is busy - correct for the design, and a queue of 24 in a class of 25.  This
   reports how many got a real answer, how many got the busy notice, and how many got nothing at all.

3. What happens to reply latency as students are added?

Usage:
    python3 chatbot_concurrency_test.py --world myworld --username-prefix student --password PASS \
        --students 10 --bot-name Tutor

Each student needs an account: student1, student2, ... created beforehand with the same password.  Use
--single-account to run them all on one login instead, which exercises the server but not per-user behaviour.
"""

import argparse
import os
import statistics
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from chatbot_test_client import Session  # noqa: E402


class Student(threading.Thread):
    daemon = True

    def __init__(self, index, args, bot_pos):
        super().__init__()
        self.index = index
        self.args = args
        self.bot_pos = bot_pos
        self.marker = 'MARKER%03d' % index
        self.username = args.username_prefix if args.single_account else ('%s%d' % (args.username_prefix, index))

        self.session = None
        self.reply = ''
        self.latency = None
        self.error = None
        self.received = []  # Everything this student was sent, for the leak check.

    def run(self):
        try:
            # Spread the students out slightly so they do not all collide on the same instant, which is not how a
            # class arrives either.
            time.sleep(self.index * self.args.stagger)

            self.session = Session(self.args.host, self.args.port, self.args.world, self.username,
                                   self.args.password, self.bot_pos,
                                   stand_back=2.0 + (self.index % 4) * 0.4, bot_name=self.args.bot_name)
            self.session.wait_for_greeting(self.args.greet_wait)
            self.session.settle()

            started = time.time()
            self.reply = self.session.say('%s here. What is 7 times 8?' % self.marker,
                                          reply_timeout=self.args.reply_wait)
            self.latency = time.time() - started
            self.received = [text for _, text, _ in self.session.replies]
        except Exception as e:  # noqa: BLE001 - one student failing should not stop the run.
            self.error = e
        finally:
            if self.session:
                try:
                    self.session.close()
                except Exception:  # noqa: BLE001
                    pass


BUSY_HINTS = ("someone else", "busy", "with another", "be free", "moment")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='localhost')
    ap.add_argument('--port', type=int, default=7600)
    ap.add_argument('--world', default='')
    ap.add_argument('--username-prefix', default='student')
    ap.add_argument('--password', required=True)
    ap.add_argument('--single-account', action='store_true',
                    help="Log every student in with the same account, rather than student1, student2, ...")
    ap.add_argument('--students', type=int, default=5)
    ap.add_argument('--bot-pos', default='0,0,1.7')
    ap.add_argument('--bot-name', default=None)
    ap.add_argument('--stagger', type=float, default=0.5, help="Seconds between students arriving.")
    ap.add_argument('--greet-wait', type=float, default=45.0)
    ap.add_argument('--reply-wait', type=float, default=120.0)
    args = ap.parse_args()

    bot_pos = [float(v) for v in args.bot_pos.split(',')]

    print('Putting %d students in front of %s at once.' % (args.students, args.bot_name or 'the bot'))
    print('')

    students = [Student(i + 1, args, bot_pos) for i in range(args.students)]
    started = time.time()
    for s in students:
        s.start()
    for s in students:
        s.join(timeout=args.greet_wait + args.reply_wait + args.stagger * args.students + 60)
    wall = time.time() - started

    # ---- 1. Privacy.  Checked first because nothing else matters if this fails. ----
    leaks = []
    for s in students:
        for other in students:
            if other is s:
                continue
            for text in s.received:
                if other.marker in text:
                    leaks.append((s.index, other.index, text[:80]))

    # ---- 2. Who actually got taught ----
    answered, busy, silent, failed = [], [], [], []
    for s in students:
        if s.error is not None:
            failed.append(s)
        elif not s.reply.strip():
            silent.append(s)
        elif any(h in s.reply.lower() for h in BUSY_HINTS) and '56' not in s.reply:
            busy.append(s)
        else:
            answered.append(s)

    print('=' * 78)
    print('PRIVACY')
    if leaks:
        print('  *** LEAK: a student received another student\'s message ***')
        for a, b, text in leaks[:10]:
            print('      student %d saw student %d\'s marker: %s' % (a, b, text))
    else:
        print('  ok - no student received another student\'s marker')

    print('')
    print('WHO GOT TAUGHT')
    print('  answered by the tutor : %d' % len(answered))
    print('  told it was busy      : %d' % len(busy))
    print('  got nothing           : %d' % len(silent))
    print('  connection failed     : %d' % len(failed))
    for s in failed[:5]:
        print('      student %d: %r' % (s.index, s.error))

    latencies = [s.latency for s in students if s.latency is not None and s.reply.strip()]
    print('')
    print('LATENCY (students who got a reply)')
    if latencies:
        print('  n=%d  min %.1fs  median %.1fs  max %.1fs' %
              (len(latencies), min(latencies), statistics.median(latencies), max(latencies)))
    else:
        print('  no replies to measure')
    print('  wall clock for the whole run: %.1fs' % wall)
    print('=' * 78)

    # A leak, or a student the system simply ignored, is a failure.  Being told the bot is busy is not - that is the
    # current design, and the point of the test is to show how much of the class it leaves unserved.
    return 0 if (not leaks and not silent and not failed) else 1


if __name__ == '__main__':
    sys.exit(main())
