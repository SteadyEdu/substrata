#!/usr/bin/env python3
"""
Run a set of scripted questions against a Substrata chatbot and report pass/fail.

Drives the bot through the real protocol, exactly as a student's client would, so what is measured is the answer a
student would actually receive - prompt, tool definitions, streaming, sentence splitting and all - rather than what
the model says when asked directly.

The point is to make "is this model good enough to put in front of a child?" a question with an answer. Run the same
scenarios against a local model and a frontier one and compare.

Usage:
    python3 chatbot_eval.py --world edutest/grade6 --username edutest --password PASS \
        --bot-name Tutor --scenarios eval_scenarios/grade6_maths.json

Exit status is 0 if every case passed, 1 otherwise, so it can be used as a build check.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from chatbot_test_client import Session  # noqa: E402


def check(reply, case):
    """Returns (passed, reason).  A missing reply always fails: silence teaches nothing."""
    if not reply.strip():
        return False, 'no reply'

    lowered = reply.lower()

    for bad in case.get('reject_any', []):
        if bad.lower() in lowered:
            return False, 'contains rejected text %r' % bad

    expect = case.get('expect_any', [])
    if expect:
        for good in expect:
            if good.lower() in lowered:
                return True, 'matched %r' % good
        return False, 'none of %s present' % (expect,)

    return True, 'no rejected text'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='localhost')
    ap.add_argument('--port', type=int, default=7600)
    ap.add_argument('--world', default='')
    ap.add_argument('--username', required=True)
    ap.add_argument('--password', required=True)
    ap.add_argument('--bot-pos', default='0,0,1.7')
    ap.add_argument('--bot-name', default=None)
    ap.add_argument('--scenarios', required=True, help="Path to a scenarios JSON file.")
    ap.add_argument('--label', default=None, help="Name for this run, e.g. the model being tested.")
    ap.add_argument('--greet-wait', type=float, default=45.0)
    ap.add_argument('--reply-wait', type=float, default=90.0)
    ap.add_argument('--json-out', default=None, help="Also write the results to this file, for comparing runs.")
    args = ap.parse_args()

    with open(args.scenarios) as f:
        scenarios = json.load(f)

    cases = scenarios.get('cases', [])
    bot_pos = [float(v) for v in args.bot_pos.split(',')]

    print('=' * 78)
    print('%s' % scenarios.get('name', args.scenarios))
    if args.label:
        print('run: %s' % args.label)
    print('%d case(s)' % len(cases))
    print('=' * 78)

    session = Session(args.host, args.port, args.world, args.username, args.password, bot_pos, bot_name=args.bot_name)
    if session.bot_uid is None:
        print('WARNING: did not find a bot avatar - every case will fail')
    session.wait_for_greeting(args.greet_wait)

    results = []
    passed = 0

    for case in cases:
        started = time.time()
        reply = session.say(case['say'], reply_timeout=args.reply_wait)
        elapsed = time.time() - started

        ok, reason = check(reply, case)
        passed += 1 if ok else 0

        results.append({'id': case.get('id'), 'say': case['say'], 'reply': reply, 'passed': ok,
                        'reason': reason, 'seconds': round(elapsed, 1)})

        print('\n%s  %-16s  (%.1fs)' % ('PASS' if ok else 'FAIL', case.get('id', ''), elapsed))
        print('   asked: %s' % case['say'])
        print('   said : %s' % (reply.strip()[:300] if reply.strip() else '(nothing)'))
        if not ok:
            print('   why  : %s' % reason)
            if case.get('note'):
                print('   note : %s' % case['note'])

    # Privacy is a property of every exchange, so check it once over the whole run rather than per case.
    privacy_ok = session.all_replies_private()
    session.close()

    print('\n' + '=' * 78)
    print('%d/%d passed' % (passed, len(cases)))
    print('all messages private: %s' % ('yes' if privacy_ok else 'NO - private conversation is leaking to world chat'))
    print('=' * 78)

    if args.json_out:
        with open(args.json_out, 'w') as f:
            json.dump({'label': args.label, 'scenarios': args.scenarios, 'passed': passed,
                       'total': len(cases), 'privacy_ok': privacy_ok, 'results': results}, f, indent=2)
        print('results written to %s' % args.json_out)

    return 0 if (passed == len(cases) and privacy_ok) else 1


if __name__ == '__main__':
    sys.exit(main())
