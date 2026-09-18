SubstrataEdu roadmap
====================

SubstrataEdu is a fork of Substrata (https://github.com/glaretechnologies/substrata) being built by Steady Studios,
a nonprofit, into a virtual campus where K-12 students work with AI tutors.

This file tracks what is done, what is pinned waiting on something, and what is next.  See docs/running server.txt
for how to configure and run what exists, and docs/building_macos.txt for how to build it.


Branches
--------
master    A pure mirror of glaretechnologies/substrata.  Nothing of ours goes here, so upstream can always be merged.
staging   Everything we have built.  The integration branch; it is not deployed to students.
feature/* Individual pieces of work, merged into staging when they build and are tested.

The same layout is used in the glare-core checkout, which carries our engine-level changes.

Some of what is on staging is generic and belongs upstream rather than in a fork - see "To offer upstream" below.


Done
----
Private tutor channels
    A chatbot can be set to converse privately: it takes one user at a time, its replies go only to that user, and
    the user's own messages are kept out of world chat.  Fails closed - if the partner disconnects mid-turn the reply
    is dropped rather than broadcast.

Transcript logging
    Every chatbot utterance, and the user's side of every private conversation, written as JSON Lines to one file per
    UTC day.  Retention defaults to keeping everything.

Safety checking and alerting
    Each student message is checked for possible self-harm, disclosure of abuse, threats, or bullying.  A flagged
    message goes to the server console, to a separate alerts file, to the /safety_alerts page, and by email to every
    user with the safeguarding lead flag.  On an urgent flag the student is told, privately and immediately, that a
    person will see what they said - sent independently of the model, so it does not depend on the model working.

Per-bot model selection
    Models are declared in the server config, including ones served from the local network, and each chatbot picks
    one.  A change takes effect on the bot's next turn.

Evaluation harness
    scripts/chatbot_eval.py runs a scenario file against a bot through the real protocol and reports pass/fail per
    topic.  scripts/eval_scenarios/grade6_maths.json has 48 grade 6 cases.  A run that is not sound - broken
    connection, undecodable message, fewer cases than the file - refuses to report a score.

Server builds and runs on macOS with only the Command Line Tools installed.


Pinned: waiting on the staging environment
------------------------------------------
Concurrent student test
    scripts/chatbot_concurrency_test.py is written and ready.  It is deliberately not run on a development laptop: a
    single student already waits about 12 seconds for a reply from a 9b model there, so a classroom's worth of
    students would measure the hardware and nothing else.  Run it on the staging box, on the model intended for
    deployment, and record the numbers here.

    It answers three questions:

      1. Does a private conversation stay private when several run at once?  Each student sends a unique marker and
         the test asserts nobody ever sees another's.  This must never fail.
      2. How many students does one chatbot actually serve?  A private bot takes one at a time and tells the rest it
         is busy.  In a class of 25 that is a queue of 24, and this quantifies it.
      3. What happens to reply latency as students are added?

    Expect question 2 to fail in the sense that matters: the current design is one conversation per bot.  The likely
    answer is a tutor session per student against a shared bot definition, rather than one bot avatar per
    conversation.  That is a significant change and is much cheaper to make before content is built on top of it.

Frontier model comparison
    Run the same 48 cases against a frontier cloud model for a like-for-like comparison with local models, to see
    what staying local costs in answer quality.


Next
----
Roles beyond the safeguarding lead
    There is still only one god user (UserID 0), per-parcel permissions, and the safeguarding lead flag.  A school
    needs district, school, teacher and student roles.  Chatbot ownership is currently standing in for "the teacher
    responsible for this tutor"; the permission check for reviewing transcripts is isolated in one function so it can
    move to a real role model cleanly.

A second safety layer
    The current check is a deterministic phrase match: reliable and fast, but it misses anything phrased unexpectedly.
    It needs a model-based classifier behind it.  The hook point is the same place the phrase check runs.

Wider evaluation
    Maths is the proving ground.  Once the tutor is solid, replicate the pattern for other subjects - the harness and
    scenario format are subject-agnostic; only the scenario file changes.

Compliance
    SSO (Google Classroom, Clever), age gating, data retention policy, and an LLM provider with contractual
    zero-retention terms before any real student data exists.

Text to speech, and speech to text
    Spatial voice chat already exists; the tutors are text-only.  The youngest grades cannot read well enough for a
    text tutor to work at all.

WebXR
    The original goal.  There is no VR or XR code anywhere in Substrata today; see the notes from the initial
    assessment.  The web client is the right target, and the engine work - per-eye projection, rendering into the
    XR framebuffer, a world-space UI - lands mostly in glare-core.

    First measurements on a Quest, web client as a 2D browser panel, empty root world, ?fps=1&scale=N:

        scale 1   1253x859   1.08 Mpix    ~60 fps (refresh cap)    worst  27.8 ms
        scale 2   2506x1718  4.31 Mpix    ~24 fps                  worst  41.7 ms
        scale 3   3759x2577  9.69 Mpix    ~13 fps                  worst   266 ms

    Pixel throughput therefore tops out around 125 Mpix/s.  Stereo WebXR on a Quest 3 wants roughly 9.1 Mpix at
    72 Hz, about 656 Mpix/s, so on these numbers the client is around five times too slow.  scale 3 is close to
    the stereo pixel count and ran at 13 fps against the 72 needed, which agrees.

    Do not treat that as the answer yet.  Every one of those numbers was measured at maximum quality, because the
    client infers "mobile" from a device pixel ratio above 1 and a Quest browser reports exactly 1 - so the
    headset was handed 4x MSAA, bloom, full shadow detail and offscreen render targets on a mobile GPU.  ?gfx=low
    now selects the cheap profile explicitly.  Rerun scale 2 and 3 with it before deciding anything: that
    measurement is what separates "needs an XR render profile" from "needs a different engine".


To offer upstream
-----------------
These are generic fixes with no education flavour, and are worth contributing back regardless of what happens to the
fork:

    glare-core  LLMThread ignored the reasoning effort it was given, so every request used the default.
    glare-core  No way to ask a model not to reason, which makes small local reasoning models unusable.
    glare-core  AIModel could not express a scheme or port, so a local model server could not be reached.
    glare-core  The built-in model list was unreachable by an application wanting to show it to a user.
    substrata   A chatbot response was discarded unless "[SPEAK]" was the very first character, so a model that began
                with a newline was silently ignored.
    substrata   A chatbot settings change did not take effect until its LLM thread died of inactivity.
    substrata   CMAKE_OSX_SYSROOT was hardcoded to the Xcode SDK, so the build failed with only the Command Line
                Tools installed.
    substrata   Vendored zlib and libpng take Classic Mac OS code paths on a modern macOS SDK.

Known upstream problem we have worked around rather than fixed: ServerTestSuite aborts two tests in, because
BasisDecoder::test() reads a hardcoded "d:/files/..." path and runTest does not catch.  Our tests are registered
before it so that they run at all.
