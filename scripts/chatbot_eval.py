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
import re
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from chatbot_test_client import Session  # noqa: E402


# Spelled-out numbers, because a tutor writes "fifty-six" as readily as "56".
_UNITS = {
    'zero': 0, 'one': 1, 'two': 2, 'three': 3, 'four': 4, 'five': 5, 'six': 6, 'seven': 7, 'eight': 8,
    'nine': 9, 'ten': 10, 'eleven': 11, 'twelve': 12, 'thirteen': 13, 'fourteen': 14, 'fifteen': 15,
    'sixteen': 16, 'seventeen': 17, 'eighteen': 18, 'nineteen': 19,
}
_TENS = {'twenty': 20, 'thirty': 30, 'forty': 40, 'fifty': 50, 'sixty': 60, 'seventy': 70, 'eighty': 80, 'ninety': 90}
_DENOMS = {'half': 2, 'halves': 2, 'third': 3, 'thirds': 3, 'quarter': 4, 'quarters': 4, 'fourth': 4,
           'fourths': 4, 'fifth': 5, 'fifths': 5, 'sixth': 6, 'sixths': 6, 'eighth': 8, 'eighths': 8,
           'tenth': 10, 'tenths': 10}
_NEGATIVE_WORDS = ('negative', 'minus')


def extract_numbers(text):
    """Every number the reply states, as floats.

    Substring matching on numbers is what made the first version of these scenarios unreliable: "4 " matched
    "negative 4 because", and "25" matched a model restating "25% of 80". Pulling the numbers out and comparing them
    as numbers is far harder to fool, and it accepts digits, words, and fractions equally, so a case does not have to
    anticipate how the model chose to write the answer.
    """
    values = set()
    lowered = text.lower().replace('−', '-')  # U+2212 MINUS SIGN

    # a/b fractions, before bare digits so the parts are not mistaken for separate answers.
    for m in re.finditer(r'(-?\d+)\s*/\s*(\d+)', lowered):
        denom = float(m.group(2))
        if denom != 0:
            values.add(float(m.group(1)) / denom)

    # Digits, including decimals and a leading sign.  A digit followed by '/' is part of a fraction already handled.
    for m in re.finditer(r'(-|negative\s+|minus\s+)?(\d+(?:\.\d+)?)(?!\s*/)', lowered):
        value = float(m.group(2))
        if m.group(1):
            value = -value
        values.add(value)

    # Number words, e.g. "fifty-six", "three quarters", "negative four".
    words = re.split(r'[^a-z]+', lowered)
    i = 0
    while i < len(words):
        negative = False
        if words[i] in _NEGATIVE_WORDS and (i + 1) < len(words):
            negative = True
            i += 1

        value = None
        if words[i] in _TENS:
            value = _TENS[words[i]]
            if (i + 1) < len(words) and words[i + 1] in _UNITS and _UNITS[words[i + 1]] < 10:
                i += 1
                value += _UNITS[words[i]]
        elif words[i] in _UNITS:
            value = _UNITS[words[i]]

        if value is not None:
            # "one hundred", "three hundred twenty"
            if (i + 1) < len(words) and words[i + 1] == 'hundred':
                i += 1
                value *= 100
                if (i + 1) < len(words) and (words[i + 1] in _UNITS or words[i + 1] in _TENS):
                    i += 1
                    value += _TENS.get(words[i], _UNITS.get(words[i], 0))

            # "three quarters" -> 0.75
            elif (i + 1) < len(words) and words[i + 1] in _DENOMS:
                i += 1
                value = value / float(_DENOMS[words[i]])

            # "three point seven five" -> 3.75.  A tutor reading a decimal aloud writes it this way.
            elif (i + 1) < len(words) and words[i + 1] == 'point':
                digits = ''
                j = i + 2
                while j < len(words) and words[j] in _UNITS and _UNITS[words[j]] < 10:
                    digits += str(_UNITS[words[j]])
                    j += 1
                if digits:
                    value = float('%d.%s' % (value, digits))
                    i = j - 1

            values.add(-float(value) if negative else float(value))

        i += 1

    return values


def check(reply, case):
    """Returns (passed, reason).  A missing reply always fails: silence teaches nothing."""
    if not reply.strip():
        return False, 'no reply'

    lowered = reply.lower()

    for bad in case.get('reject_any', []):
        if bad.lower() in lowered:
            return False, 'contains rejected text %r' % bad

    # expect_number is the right check for an arithmetic answer: it compares numbers as numbers, so it does not care
    # whether the tutor wrote 56, "fifty-six" or 3/4 versus 0.75.
    if 'expect_number' in case:
        wanted = float(case['expect_number'])
        found = extract_numbers(reply)
        if any(abs(v - wanted) < 1e-6 for v in found):
            return True, 'stated %g' % wanted
        shown = sorted(found)[:8]
        return False, 'expected %g, reply states %s' % (wanted, shown if shown else 'no numbers')

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
    session.settle()  # Consume the greeting so it is not mistaken for an answer to the first question.

    results = []
    passed = 0

    aborted = None
    for case in cases:
        started = time.time()
        try:
            reply = session.say(case['say'], reply_timeout=args.reply_wait)
        except RuntimeError as e:
            aborted = str(e)
            print('\nABORTED at %s: %s' % (case.get('id'), e))
            break
        elapsed = time.time() - started

        ok, reason = check(reply, case)
        passed += 1 if ok else 0

        results.append({'id': case.get('id'), 'topic': case.get('topic'), 'say': case['say'], 'reply': reply,
                        'passed': ok, 'reason': reason, 'seconds': round(elapsed, 1)})

        # With a large scenario file, printing every passing reply buries the failures.  Passes get one line; only
        # failures get the reply, which is what someone will actually need to read.
        if ok:
            print('PASS  %-24s (%.1fs)' % (case.get('id', ''), elapsed), flush=True)
        else:
            print('FAIL  %-24s (%.1fs)' % (case.get('id', ''), elapsed), flush=True)
            print('   asked: %s' % case['say'])
            print('   said : %s' % (reply.strip()[:300] if reply.strip() else '(nothing)'))
            print('   why  : %s' % reason)
            if case.get('note'):
                print('   note : %s' % case['note'])

    # Privacy is a property of every exchange, so check it once over the whole run rather than per case.
    privacy_ok = session.all_replies_private()
    reader_error = session.reader.error
    decode_errors = session.reader.decode_errors
    first_decode_error = session.reader.first_decode_error
    session.close()

    print('\n' + '=' * 78)

    # Per topic, so a weakness in one strand is visible rather than averaged away.
    topics = {}
    for r in results:
        topic = r.get('topic') or '(none)'
        got, total = topics.get(topic, (0, 0))
        topics[topic] = (got + (1 if r['passed'] else 0), total + 1)

    for topic in sorted(topics, key=lambda k: (topics[k][0] / float(topics[k][1]), k)):
        got, total = topics[topic]
        bar = '#' * got + '.' * (total - got)
        print('  %-14s %2d/%-2d  %s' % (topic, got, total, bar))

    print('')
    print('%d/%d passed' % (passed, len(cases)))
    print('all messages private: %s' % ('yes' if privacy_ok else 'NO - private conversation is leaking to world chat'))

    # A run whose connection broke is not a measurement of the model, and must not be read as one.  Say so loudly and
    # exit non-zero even if the cases that did run all passed.
    run_valid = (aborted is None) and (reader_error is None) and (decode_errors == 0) and (len(results) == len(cases))
    if not run_valid:
        print('')
        print('*** THESE RESULTS ARE NOT VALID ***')
        if aborted:
            print('    run aborted: %s' % aborted)
        if reader_error is not None:
            print('    connection failed: %r' % (reader_error,))
        if decode_errors:
            print('    %d message(s) could not be decoded; first: %s' % (decode_errors, first_decode_error))
        if len(results) != len(cases):
            print('    only %d of %d cases ran' % (len(results), len(cases)))
        print('    Fix the harness and run again before drawing any conclusion about the model.')

    print('=' * 78)

    if args.json_out:
        with open(args.json_out, 'w') as f:
            json.dump({'label': args.label, 'scenarios': args.scenarios, 'passed': passed,
                       'total': len(cases), 'privacy_ok': privacy_ok, 'run_valid': run_valid,
                       'aborted': aborted, 'decode_errors': decode_errors, 'results': results}, f, indent=2)
        print('results written to %s' % args.json_out)

    return 0 if (run_valid and passed == len(cases) and privacy_ok) else 1


if __name__ == '__main__':
    sys.exit(main())
