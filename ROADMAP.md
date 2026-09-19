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
    The original goal.  Started: glare-core can now draw into a sub-rectangle of the render target, which is what
    lets two eyes share one framebuffer - setViewportRect(), and scissored clears so the second eye does not wipe
    the first.  ?stereo=1 in the web client draws the scene twice side by side and is what proved it: tracing the
    GL context shows two viewport rectangles a frame, each taking the same 66 draw calls.

    Then the cost of a second view was measured, by splitting the canvas rather than enlarging it so the pixel
    count stays the same.  At gfx=min, scale 2, 4.31 Mpix either way:

        one view    54.0 fps   18.5 ms
        two views   42.4 fps   23.6 ms

    A second view costs 5.1 ms with no extra pixels.  That splits the 7.4 ms fixed term into 5.1 ms per view and
    2.3 ms per frame, and it changes the outlook badly: two views cost 12.5 ms of the 13.9 ms available at 72 Hz
    before a single pixel is shaded, leaving room for 6% of a Quest 3's per-eye resolution.  At 90 Hz the
    overhead alone exceeds the budget.  The earlier 26% figure assumed that fixed cost was per frame.  It is not.

    That suspicion fell to a trace.  Tracing the GL context at gfx=min shows no framebuffer objects bound at all
    and only the two eye viewports: every draw goes straight to the default framebuffer.  drawCloudEnvMap() never
    runs, because volumetric_clouds_support defaults off and the client never sets it, and drawAuroraTex() never
    runs either, because the engine's own draw_aurora defaults off.  There was no redundant pass to hoist.

    What did differ between the views was the clear.  The second eye's clear was confined to its rectangle, which
    is the expensive case on a tile-based GPU: a whole-framebuffer clear lets the driver treat the old contents
    as dead, while a partial one forces them to be loaded and written back.  Clearing the whole target once for
    the first view and not at all for later ones - setViewIndexInFrame() - recovered 3.2 ms of the 5.1:

        one view            54.0 fps   18.5 ms
        two views, before   42.4 fps   23.6 ms
        two views, after    49.0 fps   20.4 ms

    So the fixed cost now splits as 1.9 ms per view and 5.5 ms per frame, and at 72 Hz that leaves 18% of a
    Quest 3's per-eye resolution, a framebuffer scale of 0.43.  90 Hz remains out of reach.

    The per-frame 5.5 ms is now the largest term and is mostly not the client: cpu is 0.2 ms and draw submission
    0.5 ms.  Fitting gfx=high from the scales where the canvas overflows and extrapolating to the one where it
    does not put about 3.8 ms on the browser compositing an oversized canvas - which an immersive WebXR session
    does not do at all, since the app renders into the headset's framebuffer rather than a 2D page.  If that
    holds, the figure at 72 Hz is nearer 33%, a scale of 0.58, which is a usable headset image.

    That is an inference the 2D panel cannot test, because the thing in question is the panel.  Measuring further
    through it has stopped paying.  The next step that reduces uncertainty is a minimal WebXR session.

    Scope of the WebXR session work
    -------------------------------
    Two things that looked like engine work turn out not to be.

    FrameBuffer already has an explicit FrameBuffer(GLuint) constructor that does not take ownership, and
    setTargetFrameBuffer() already redirects the main passes at it.  Rendering into the framebuffer WebXR hands
    over needs no new engine code, only a way to get its handle across from JS.

    And the camera model is a sensor-and-lens one that already carries lens_shift_up_distance and
    lens_shift_right_distance, which is exactly the asymmetry a per-eye projection needs.  calcCamFrustumVerts()
    already accounts for the shift, so culling follows.  A per-eye projection can be expressed by converting
    WebXR's numbers into that model rather than by adding a frustum entry point to the engine.

    With the viewport offset and the per-frame clear already done, glare-core may need no further changes at all.
    What is left is the client and a JS shim.

    Phase 2 renders correctly and does not display.  This is the open problem.

    What is established, by driving the headset's browser directly over adb rather than by inference:

      * The engine's output is correct.  Substituting a readable framebuffer for the session's, behind the same
        GL name, and reading it back gives proper stereo - sky above, ground below, the two eyes differing,
        alpha 255 throughout.
      * The client binds the session's own live framebuffer, with a correct camera, no GL errors, and a single
        render pass at 79-86 fps at full native 3360x1760.
      * The session and its setup are fine: painting a solid colour from a callback of our own, on the page's own
        session, displays correctly.
      * Cancelling Emscripten's main loop is harmless - a session keeps displaying after it.
      * Nothing illegal appears in a device trace of a frame: no drawBuffers, no reads, no blits, no invalidates.

    And what actually happens: a session displays a plain clear, and stops displaying anything from the first
    frame opengl_engine->draw() runs - permanently, for the rest of that session.  Alternating between a clear
    and the world three seconds apart shows blue once and then black for good.  Drawing into a framebuffer of our
    own and copying the result across does not help either, so it is not about what the session's framebuffer
    receives; the act of the engine drawing at all disables the session's display.

    Neither binding the target as FRAMEBUFFER rather than DRAW_FRAMEBUFFER, nor forcing alpha to one, nor
    turning off the layer's antialiasing, nor bypassing the offscreen path made any difference.

    Next, without needing anyone in a headset: cut down draw() until the session survives it - the simplest
    possible draw against the same context and session, then add passes back.  The context is created by SDL with
    antialias on and a thirty-thread pool, which is the remaining thing that differs from the JavaScript that
    does display correctly.

    Phase 1 is done: an Enter VR button, a session, its frame loop driving the client, and a clean exit.  The
    framebuffer handover - the risk everything else rested on - works: a JavaScript WebGLFramebuffer registered
    in Emscripten's table through Module.GL can be bound and cleared from C++.  Proved without a headset by
    registering an ordinary framebuffer, calling the three entry points by hand and reading the pixel back: it
    came out 128,64,255,255, exactly what the C++ clear computes at t=0.

    Two numbers came back from the headset that had been assumptions until then.  The session runs at 90 Hz, not
    the 72 every earlier calculation used, so the budget is 11.1 ms rather than 13.9.  And the runtime asks for
    3360x1760, which is 1680x1760 an eye - 2.96 Mpix, about 65% of the 4.56 taken from a spec sheet.  Fewer
    pixels, less time.

    Against the measured cost model - 5.5 ms a frame, 1.9 ms a view, 2.78 ms a Mpix - native resolution comes to
    25.7 ms a frame, or 39 fps against the 90 needed.  What fits:

                                          90 Hz               72 Hz
        as measured                    11% of native       28% of native
        if page compositing is absent  34% of native       51% of native

    So two levers matter more than anything in the renderer.  Asking for 72 Hz instead of 90 buys 2.8 ms, worth
    roughly twice the pixels, and smoothness is a cheaper thing to give up in a classroom than legibility.  And
    framebufferScaleFactor is the direct control, with cost going as its square.  Both are now URL parameters -
    ?xrrate= and ?xrscale= - so phase 2 can find the usable combination on hardware instead of by arithmetic.

    Phase 2 is written: both eyes, head tracked, drawn through the XR framebuffer.  The projection conversion is
    the only subtle part.  A projection matrix in the usual form gives the half extents at unit distance as 1/m0
    and 1/m5, and the asymmetry that makes it a per-eye projection as m8/m0 and m9/m5 - which map exactly onto
    the lens shift the engine already has and already culls against, so no new camera entry point was needed.
    WebXR's camera space is x right, y up, z back and the engine's is x right, y forwards, z up; one basis change
    serves both to convert the view transform and to place the reference space in the z-up world.

    Verified without a headset by driving a session by hand against a 3360x1760 framebuffer with two asymmetric
    projections: both halves came back with sky above and ground below, and the two eyes differ - sky 76,110,150
    against 88,116,152 - which is the asymmetry taking effect rather than the same image drawn twice.

    Phase 1, session lifecycle.  An Enter VR button, since a session can only start from a user gesture;
    navigator.xr.isSessionSupported and requestSession('immersive-vr'); a reference space; and swapping the main
    loop over - emscripten_cancel_main_loop() and then let XRSession.requestAnimationFrame call an exported
    function, rather than emscripten_set_main_loop.  Also stop calling SDL_GL_SwapWindow while in session, since
    the headset compositor presents instead.  Deliverable: a session that starts, runs frames and exits cleanly,
    rendering nothing.  Do this first because the hardest risks are here, not in the rendering.

    Phase 2, render both eyes.  Per XR frame: get the viewer pose, loop over its views, and for each one set the
    viewport rect from XRWebGLLayer.getViewport(), the view index, and the camera transform converted from the
    view's projection and transform.  The pieces this needs already exist.  Deliverable: correct stereo with head
    tracking, which is also the point at which the per-frame cost can finally be measured for real rather than
    inferred from a 2D panel.

    Phase 3, pose and locomotion.  WebXR is y-up and metres; the world is z-up.  One fixed rotation between them,
    applied to the viewer pose, and the camera controller's position becomes the origin offset of the reference
    space.  Then thumbstick movement from XRInputSource gamepads, because a headset has no keyboard.

    Phase 4, the interface.  gl_ui draws in screen space, which in a headset is wrong rather than merely ugly.
    Disable it in session for phases 1 to 3 and give it a world-space quad afterwards.  The tutor is text, so
    this is what decides whether the thing is usable by a student, and it is the phase most likely to be
    underestimated.

    Risks, worst first:

    * Emscripten addresses GL objects by integer name through its own table, and XRWebGLLayer hands out a
        JavaScript WebGLFramebuffer.  Registering that object in Emscripten's table so C++ can bind it by name is
        the one piece with no precedent in this codebase.  If it does not work nothing else in phase 2 matters,
        which is the argument for proving it early.
    * The WebGL context is created by SDL, and a session needs an XR-compatible one.  makeXRCompatible() can
        migrate the context to a different adapter and lose it, so context loss has to be survivable.
    * The XR framebuffer is opaque: it cannot be read back or have attachments changed.  The engine only binds it
        and draws, so this should hold, but the offscreen render path must stay off - which it already is,
        because stereo forces the cheap profile.
    * None of this can be tested on the development machine.  Every iteration needs the headset, which makes the
        loop slow and argues for small, verifiable steps rather than a large change tested at the end.

    Still to do for a real XR path: per-eye projection matrices from the headset rather than one shared camera
    (setFrustumCameraTransform), rendering into the framebuffer WebXR hands over, driving the loop from
    XRSession.requestAnimationFrame instead of emscripten_set_main_loop, the y-up to z-up conversion, and a
    world-space UI.  Note also that two eyes currently means twice the draw calls, so multiview is worth having.

    There is no other VR or XR code anywhere in Substrata; see the notes from the initial assessment.  The web client is the right target, and the engine work - per-eye projection, rendering into the
    XR framebuffer, a world-space UI - lands mostly in glare-core.

    First measurements on a Quest, web client as a 2D browser panel, empty root world, ?fps=1&scale=N:

        scale 1   1253x859   1.08 Mpix    ~60 fps (refresh cap)    worst  27.8 ms
        scale 2   2506x1718  4.31 Mpix    ~24 fps                  worst  41.7 ms
        scale 3   3759x2577  9.69 Mpix    ~13 fps                  worst   266 ms

    Pixel throughput therefore tops out around 125 Mpix/s.  Stereo WebXR on a Quest 3 wants roughly 9.1 Mpix at
    72 Hz, about 656 Mpix/s, so on these numbers the client is around five times too slow.  scale 3 is close to
    the stereo pixel count and ran at 13 fps against the 72 needed, which agrees.

    Those were measured at maximum quality: the client infers "mobile" from a device pixel ratio above 1 and a
    Quest browser reports exactly 1, so the headset was handed 4x MSAA, bloom, full shadow detail and offscreen
    render targets on a mobile GPU.  ?gfx=low selects the cheap profile explicitly.  Rerun with it:

        scale 2   4.31 Mpix    ~38 fps     was 24
        scale 3   9.69 Mpix    19-24 fps   was 13

    So the cheap profile is worth about 1.6x, and the ceiling moves to roughly 208 Mpix/s against the 656 a
    Quest 3 wants.  Around three times short rather than five.

    Fitting frame time against pixel count separates the two costs, and this is the part that decides the
    project.  At ?gfx=low the fit is about 3.75 ms per Mpix plus a fixed 10 ms per frame that does not depend on
    resolution at all.  At 72 Hz the entire frame budget is 13.9 ms.  If that 10 ms is real then no amount of
    resolution reduction, foveation or eye-buffer scaling reaches 72 Hz, because those only attack the per-pixel
    term - the work would have to go into draw calls, culling and scene traversal instead, which is a much larger
    project than an XR render profile.

    Two more scale points confirm it.  Four measurements at ?gfx=low, frame time against pixel count:

        scale 1.5   2.42 Mpix   ~57 fps      17.5 ms
        scale 2     4.31 Mpix   ~38 fps      26.3 ms
        scale 2.5   6.73 Mpix   26-33 fps    33.9 ms
        scale 3     9.69 Mpix   19-24 fps    46.5 ms

        fit:  frame_ms = 8.5 + 3.9 * Mpix      R^2 = 0.996, residuals under 1 ms

    So there really is a fixed 8.5 ms per frame that does not depend on resolution, against a 13.9 ms budget at
    72 Hz.  That leaves 5.4 ms for pixels, or about 0.7 Mpix an eye - roughly 15% of what a Quest 3 asks for, a
    framebuffer scale of 0.39.  Fill rate is therefore not the binding constraint any more and the per-pixel
    levers - foveation, eye buffer scaling - cannot reach 72 Hz on their own.

    Three quality profiles, each fitted the same way:

        gfx=high   frame_ms = 10.2 + 6.95 * Mpix     ->  0.27 Mpix/eye at 72 Hz, 6% of a Quest 3 eye
        gfx=low    frame_ms =  8.5 + 3.90 * Mpix     ->  0.69 Mpix/eye,          15%
        gfx=min    frame_ms =  7.4 + 2.78 * Mpix     ->  1.17 Mpix/eye,          26%, a 0.51 framebuffer scale

    Two corrections to earlier readings.  The Quest browser's refresh cap is about 72 Hz, not 60: gfx=min reached
    68 fps.  Every number recorded above is therefore a real measurement and not a capped one, including the
    first 60 fps reading, which was treated as a cap at the time and was not.

    And the client's own work is negligible - cpu about 0.5 ms, draw submission about 1.1 ms, inside frames of 17
    to 46 ms, flat across a 2.7x change in pixel count.  Whatever the fixed cost is, it is not main loop work,
    draw call submission or scene traversal.  That rules out the expensive scenario, which would have meant
    rewriting batching and culling before any XR code could be written.

    Shadow mapping turned out to be worth 1.1 ms of the fixed term and 1.1 ms per Mpix - real, but only about an
    eighth of the fixed cost, so not the explanation.  Fitting gfx=high from the two scales where the canvas
    overflows the window and extrapolating down to scale 1, where it does not, overshoots by 3.8 ms, which
    suggests a good part of the remaining fixed cost is the browser compositing an oversized canvas - an artifact
    of how the measurement works that real WebXR would never pay.

    That is where this method stops being useful: the thing we now most want to measure is the thing the method
    itself contaminates.  Going further means either fence-based GPU timing under WebGL2, or implementing a
    minimal XR path and measuring it directly.  The second is probably the better spend, because the question the
    spike existed to answer has an answer: at gfx=min a Quest 3 should hold 72 Hz in stereo at roughly half to
    two thirds linear resolution on a simple scene, so WebXR is worth attempting.  What it is not worth is
    attempting at full quality - the render profile is part of the feature, not a tuning pass afterwards.


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
