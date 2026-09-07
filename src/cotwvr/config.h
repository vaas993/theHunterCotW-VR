#pragma once
#include <string>

namespace cotwvr {

constexpr int kProfileSlots = 5;

// Settings persist beside the DLL (cotwvr.ini).  Unknown keys are ignored and
// preserved-by-default semantics are deliberate: an old ini must never brick a
// new build, and a new build must never silently drop a key it does not know.
class Config {
public:
    void Load();
    void Save() const;

    // --- values -----------------------------------------------------------
    int   version          = 1;
    bool  enabled          = true;   // master switch; false = vanilla game
    bool  submit_to_headset= true;   // false = hooks only, no OpenXR (debugging)
    bool  blank_layer_test = false;  // submit a solid colour instead of the game
    int   log_frame_every  = 600;    // frames between heartbeat log lines (0=off)

    // Reverse-engineering aid: hook the engine's transform builders and report
    // which call site behaves like the player camera. Costs framerate; off in
    // normal use.
    bool  camera_probe       = false;
    int   camera_probe_every = 600;  // frames between probe reports

    // Axis test / input-vs-derived test. 0 = off, 1/2/3 = shift the camera
    // position along basis row 0/1/2 by camera_test_amount metres.
    int   camera_test_axis   = 0;
    float camera_test_amount = 3.0f;

    // Submit both eyes with one shared SYMMETRIC frustum instead of the
    // runtime's mirrored asymmetric per-eye ones. Required whenever both eyes
    // are shown the same rendered image, or it reads as double vision.
    // Render resolution written into the game's own settings at startup.
    // 0 = leave the game alone. Square-ish shapes waste far fewer pixels: the
    // headset's eye frustum is 0.916:1, so a 2.667:1 render loses 66% of what
    // the GPU drew. Takes effect on the NEXT launch.
    int   render_preset = 32;
    // A resolution of the player's own choosing. Non-zero overrides the preset
    // entirely - the presets are a menu of sensible shapes, not a limit, and a
    // headset this table has never seen wants a shape none of them carry.
    //
    // Written to the game exactly as given; the mod's own crop and FOV work off
    // whatever the game then renders, so any size is legal.
    int   render_custom_w = 0;
    int   render_custom_h = 0;
    // Written alongside it. 0 = leave the game's own FOV setting,
    // -1 = calculate the value that exactly fills the headset for that shape.
    int   force_game_fov = 0;
    // The headset's vertical field of view, used for that calculation.
    // VDXR on a Quest 3 reports 44 up + 55 down = 99.
    float headset_vfov_deg = 99.0f;

    // WHETHER game_fov_deg IS ALLOWED TO FOLLOW THE FOV GIVEN TO THE GAME.
    //
    // It always has, on every launch, so that the number the mod tells the
    // headset and the number the game renders with cannot drift apart - a
    // mismatch there is what made the picture look stretched once already.
    //
    // But game_fov_deg is also the most useful live control in the panel: it
    // decides how wide an angle the rendered image is claimed to cover, so
    // raising it brings the whole world nearer and lowering it pushes it away.
    // Hand-tuning it was pointless while every launch reset it. Set this to 0
    // and the mod leaves game_fov_deg exactly as the player left it.
    bool  game_fov_follows_auto = true;

    // LOOK UP AND DOWN WITH YOUR HEAD ONLY.
    //
    // In VR the head already owns pitch, so a stick or mouse that ALSO pitches
    // the view fights it: the horizon tilts away from where your neck says it
    // is, which is the classic way to feel ill. Yaw is different - you cannot
    // turn 180 degrees in a chair - so it stays on the stick.
    //
    // Suppressed where the game collects the input, not where it acts on it: the
    // pad through COTWVR_PadPostProcess (which the proxy already routes here) and
    // the mouse through the raw-input hooks the panel already owns. Both are
    // paths the mod holds anyway, so this adds no new hook.
    // HOLD THE VIEW STEADIER WHILE AIMING.
    //
    // A rifle magnifies whatever your neck does. At 8x a millimetre of sway is
    // several metres downrange, and a head that feels perfectly still is not -
    // heartbeat, breathing and the weight of the headset are all in there. So
    // the aim wanders in a way that reads as the mod being imprecise.
    //
    // Exponential smoothing of the head pose, applied ONLY while scoped, at the
    // source - where the pose is stored, not at each of the places that read it.
    // Every consumer then sees one steadied pose and they cannot disagree.
    //
    // How many frames D3D may queue ahead. 1 is what every VR mod in the
    // prior-art notes uses; the D3D default of 3 makes the pacing uneven, which
    // is what alternate-eye rendering punishes hardest. 0 = leave the game's own
    // setting alone, which is the way back if this costs more than it buys.
    int   frame_latency = 1;

    // MEASURED IN MILLISECONDS, NOT IN A BLEND FACTOR.
    //
    // A per-frame blend is silently a different setting at a different frame
    // rate, and this mod ships a switch that HALVES the frame rate (AER against
    // full-rate stereo). Simulated: the same blend factor removed 67% of the
    // tremor at 90 fps and 61% at 45. A time constant - how long the view takes
    // to catch up - behaves the same at both, so tuning it once is enough.
    //
    // Sized against simulation rather than guessed, at 90 fps, against a 0.25
    // degree hand tremor plus a deliberate 5 degree traverse:
    //
    //     20 ms   50% of the tremor gone,  5 deg move takes  56 ms
    //     60 ms   74%                                       144 ms   <- default
    //    120 ms   84%                                       289 ms
    //    250 ms   92%                                       578 ms - sluggish
    //
    // Prior art warns this is a seasoning, not a fix (MCC VR caps its smoothing
    // low; MELE1 smooths the reticle only and leaves the bullets raw), so the
    // ceiling is 250 ms. Past that the view stops belonging to your head: it
    // lags, then catches up, which is worse than the shake and makes people ill.
    bool  aim_steady    = true;   // master switch, off = old behaviour exactly
    float aim_steady_ms = 120.0f;   // 0 = off, 250 = the most it will take
    bool  aim_steady_scoped_only = true;   // off = steady all the time

    // *** THE HEARTBEAT. ***
    //
    // A pulse tremors the head by a fraction of a degree several times a
    // second and the whole view beats with it - the owner feels their own
    // heartbeat in the game (2026-08-14). A fixed smoother would remove it and
    // lag every real head turn with it, which trades one problem for a worse
    // one. So the time constant is picked from the ROTATION RATE: full
    // smoothing while the head is essentially still (tremor is all there is at
    // that speed), none once you are genuinely turning, ramped between. That
    // is the 1-euro filter's idea, built into the smoother that already
    // exists. Prior art warns pose smoothing is a seasoning, not a fix
    // (VR_PRIOR_ART_LESSONS #10) - which is exactly why it is rate-gated.
    // *** PREDICTION DAMPENING (see vr.cpp for the mechanism). ***
    //
    // Percentage of one display period to pull the pose request back toward
    // now. The runtime extrapolates the head pose to display time, and
    // extrapolation amplifies velocity - which is why a heartbeat tremor
    // arrives bigger than it really is. Shortening the prediction attacks
    // that at the source and, unlike smoothing, costs no responsiveness on a
    // real head turn (it trades a little latency for the overshoot).
    //
    // This is OpenXR Toolkit's "Over-prediction reduction" (ex "Prediction
    // dampening", ex "Shaking reduction") reimplemented in our own locate
    // call. Its published range could not be verified - the project's docs
    // site is dead - so this is a plain 0-100 dial to be found by feel.
    // 0 = stock.
    int   prediction_damp_pct = 0;

    // *** A LOW-PASS CANNOT DO THIS, AND THE FIRST VERSION WAS WRONG. ***
    //
    // Cardiac head motion is 0.75-5 Hz - THE SAME BAND as voluntary head
    // motion (0.5-2 Hz for gross looking, energy to 8 Hz). No frequency
    // filter can separate them. The first attempt smoothed at tau = 70 ms and
    // removes about 10% of a 1.2 Hz pulse; 90% would need tau = 1.3 s, which
    // is unusable. Its comment claimed "70 ms is enough for a pulse at
    // 1-2 Hz" - that arithmetic was borrowed from a ~10 Hz HAND tremor, where
    // the same tau does remove 74%. For scale: Halo MCC VR caps head
    // smoothing at 4.8 ms, HaloCEVR applies NONE to the view, UEVR's camera
    // lerp ships disabled. 70 ms was ~15x the strictest shipped ceiling,
    // which is exactly why the owner reported "floaty".
    //
    // Pulse and looking separate by AMPLITUDE, not frequency: a heartbeat
    // moves the head a few hundredths of a degree, a real look moves it
    // degrees. So the tool is a DEAD-BAND - opentrack's Accela ships one at
    // 0.03 deg (max 0.2), subtractive so there is no step as you leave it -
    // followed by a smoothstep release (the Oculus prediction patent's own
    // recommendation), a SHORT ema, and a hard leash so lag can never exceed
    // a known angle. Dead-band costs zero latency, which a filter cannot.
    //
    // 0 = off (behaviour identical to not having this at all), 1 = light,
    // 2 = medium, 3 = strong. One dial, because tuning a three-parameter
    // control loop is not a thing to ask of the person wearing the headset.
    int   head_tremor_level = 0;
    // The fine shape, ini-only. Defaults from the prior-art sweep: opentrack's
    // dead-band scale, Casiez's 10-20 ms filter budget, and a leash matching
    // the ~0.2 deg lag the owner already tolerated.
    float head_tremor_release_deg = 0.25f;  // fully transparent by D + this
    float head_tremor_tau_ms = 15.0f;       // the ema once released
    float head_tremor_leash_deg = 0.30f;    // lag can never exceed this

    // *** ONE HEAD POSE PER FRAME. ***
    // The head offset is written at several sites in a frame (early write,
    // transform builder, reseed cave) and each read the pose fresh; the pose
    // is republished from the OpenXR thread, so two reads in one frame can
    // disagree and the residual is injected into the engine's camera. That is
    // a per-frame apparent camera movement - several pixels on close geometry
    // and nothing at distance, which is the measured signature of the object
    // shake (2026-08-13: shake vanishes with head tracking off, is unaffected
    // by orientation smoothing, unaffected by IPD, and reads as a ~5 mm
    // position error at 1.8 m). Prior art #15: the pose travels with the
    // frame. Off = the old behaviour exactly.
    bool  head_latch_per_frame = false;

    // *** THE EDGE BUZZ, AND WHY IT IS NOT AN INPUT BUG. ***
    //
    // Measured 2026-08-13 with a static camera: the reprojection is accurate
    // to ~0.3 px most frames and then spikes to 4, 10, 25 px. The engine
    // rasterises with a 16-phase jitter, so a pixel on ANY silhouette edge
    // catches the near surface on some frames and the far one on others; the
    // depth flips, the world position reconstructed from it flips, and the
    // history is fetched from somewhere else. Every object has edges - hence
    // "rocks, trees, house logs, foliage, most objects shake", and hence no
    // amount of jitter/camera/pose correctness ever touched it.
    //
    // Closest-depth dilation is what every shipped TAA does about this: take
    // the nearest depth in the 3x3 neighbourhood, so the near surface wins
    // consistently and the pixel stops flipping.
    // *** BUILD THE REPROJECTION IN DOUBLE PRECISION. ***
    //
    // The captured matrices carry world-scale translations (m[15] measures
    // ~8500) next to a 0.0094 near plane. Inverting that in 32-bit float and
    // multiplying by another one is catastrophic cancellation, and the
    // residue is a few TENTHS OF A PIXEL that changes every frame - measured
    // with a completely static camera, and the exact signature of the fault
    // the owner reports: silhouettes hold (the clamp pins high contrast)
    // while the low-contrast TEXTURE inside every object swims. Vanilla is
    // clean because the engine reprojects from velocities and never inverts.
    // The result is near-identity, so only the intermediate needs double.
    bool  taa_double_reproj = true;

    // Closest-depth dilation. Default OFF: it did not touch the buzz, and it
    // makes DLSS worse - the programming guide is explicit that render-res
    // vectors must NOT be pre-dilated because DLSS dilates them itself.
    bool  taa_dilate_depth = false;
    // And a still scene should be fetched from the EXACT texel: any fraction
    // of a pixel means the history is resampled, and a different fraction
    // each frame means a different blur each frame. In pixels; 0 = off.
    float taa_mv_deadzone_px = 0.0f;

    bool  lock_pitch_input      = false;   // master switch, off = old behaviour
    bool  lock_pitch_pad        = true;    // the right stick's up/down
    bool  lock_pitch_mouse      = true;    // the mouse's up/down

    // THE GAME'S OWN GRAPHICS OPTIONS, APPLIED FROM HERE RATHER THAN FROM THE
    // LAUNCHER.
    //
    // The launcher used to write these straight into settings.json while the
    // game was closed, and they kept coming back. The reason is that the game
    // REWRITES THE WHOLE FILE FROM MEMORY WHEN IT EXITS - and regenerates it
    // outright on the first run after a reinstall, which is exactly when the
    // owner found the recommended set had not taken. Anything written to that
    // file while the game is not running survives only until the next quit.
    //
    // The resolution never had this problem because the mod writes it from
    // DllMain, on every launch, after the engine has done its own regeneration.
    // These now go the same way. The launcher edits the ini - a file the game
    // has never heard of - and the mod applies it, every time.
    //
    // -1 on any of these means LEAVE THE GAME'S OWN VALUE ALONE, so a player who
    // prefers the in-game panel is not fought by the mod.
    bool  apply_game_graphics = true;
    int   graphics_motion_blur     = 0;
    int   graphics_depth_of_field  = 0;
    int   graphics_vignette        = 0;
    int   graphics_ssao            = 0;
    int   graphics_ssr             = 0;
    int   graphics_contact_shadows = 0;
    // *** 3 = FXAA + TAA, AND IT IS NOT A TASTE. ***
    //
    // The mod's resolve pass works by TAKING OVER the engine's temporal
    // anti-aliasing, and that pass is where DLSS gets its motion vectors. Set
    // the game to anything else and there is no pass to take over, so the
    // resolve does nothing and DLSS never runs at all.
    //
    // This shipped as 1 (FXAA alone) for months, left over from before the
    // resolve existed, when TAA ghosted under full-rate stereo. The mod then
    // WROTE that 1 into the game at every launch - so a clean install of the
    // package actively switched off the thing DLSS depends on, and the log said
    // "NOTHING was replaced ... it wants a TAA mode" while the player wondered
    // which file they were missing. -1 would leave it alone; that is not enough,
    // because the value the game happens to hold is what decides whether half
    // the mod functions.
    int   graphics_aa              = 3;

    // Weapon / viewmodel field of view. The game renders the held weapon
    // through its own projection, so the world FOV does not affect it.
    // 0 = leave the game's own value alone. Applied live, no relaunch.

    bool  symmetric_fov = true;

    // The VERTICAL field of view, in degrees, that the GAME renders with.
    //
    // This must match reality or the geometry is wrong: the image is shown
    // through this cone, and if the cone is wider than the one the game drew
    // with, every angle is magnified. Distant objects survive that (their
    // stereo disparity is ~0) but near objects do not - their disparity is
    // magnified into a convergence demand the eyes cannot meet, which reads as
    // "depth is there but close things are uncomfortable".
    //
    // 0 = fill the headset's own FOV instead (wrong geometry, but no black
    // borders) - what the first build did.
    float game_fov_deg = 100.0f;

    // Is the number above the game's HORIZONTAL fov rather than its vertical
    // one? Most shooters quote horizontal. Getting this backwards inflates the
    // frustum by ~24% at 4:3, which magnifies stereo disparity by the same
    // factor - harmless at distance, uncomfortable close up.
    bool  fov_is_horizontal = false;

    // --- stereo -----------------------------------------------------------
    bool  stereo        = true;
    // *** 6DoF - THE HEAD'S POSITION, NOT JUST ITS DIRECTION. ***
    //
    // Lean around a trunk, duck under a branch, move your head to see past the
    // scope. The game has no concept of any of it: there is no field to write,
    // the way there is for yaw and pitch, so the camera's world position row is
    // offset directly at the site the stereo eye separation already uses.
    //
    // OFF is the master switch and the default - with it off not one instruction
    // of this exists in the frame.
    //
    // Nothing here collides with the world. That is not a bug that can be fixed
    // from outside the engine: the mod moves a camera, and the game does not
    // know the camera moved, so a big enough lean puts the view inside a rock.
    // six_dof_limit_m is the guard, and it is why the default is a lean rather
    // than a room.
    bool  six_dof = false;
    float six_dof_scale = 1.00f;      // 1 = your real movement, 1:1
    float six_dof_limit_m = 0.60f;    // how far from centre the camera may go
    bool  six_dof_invert_x = false;   // signs unsettled by measurement, as the
    bool  six_dof_invert_y = false;   // eye separation's own axis was until one
    bool  six_dof_invert_z = false;   // headset session pinned it

    // *** WHICH FRAME THE LEAN IS MEASURED IN. ***
    //
    // On: the head's own yaw and pitch are taken back out of the offset before
    // it is applied, leaving the BODY's facing - so turning your head turns your
    // view without moving you, and turning with the stick carries your lean
    // round with you. Off is the first build's behaviour, where a room-space
    // offset was projected onto a head-rotated basis and the player orbited a
    // point off to one side instead of turning on the spot.
    //
    // Kept switchable because it is the difference the owner will feel
    // immediately, and A/B is the fastest way to confirm a rotation fix.
    // *** WHICH WAY THE ROOM IS POINTING. ***
    //
    // OpenXR's local space is pinned to the ROOM, not to the chair, and
    // recentring turns the VIEW without turning the frame the position is
    // measured in. If the guardian was drawn facing a different way from the
    // way the player sits, every lean lands a fixed number of degrees round from
    // where it should - and a QUARTER turn is the common case, which no invert
    // switch can express because inverting is a half turn.
    //
    // Quarter turns only: it is a room misalignment, not a fine adjustment.
    int   six_dof_yaw_offset_deg = 0;

    // Print the room offset and the applied movement once a second. Three
    // sessions were spent describing directions in words; the numbers settle it.
    bool  six_dof_log = false;

    // *** WHICH FACING THE LEAN IS MEASURED AGAINST. ***
    //
    // 0 the facing at the last recentre   (right if the basis turns with the STICK)
    // 1 the head's current facing         (right if the basis turns with the HEAD)
    // 2 the head's turn SINCE the recentre
    // 3 none - the room's own axes
    //
    // These four are the entire space, and which one is correct is a property of
    // engine code this mod does not own. Two builds read it two different ways
    // and each fixed one case while breaking another, so it is a setting now and
    // one session in the headset decides it.
    int   six_dof_frame_mode = 1;

    // Take the standing position when the WORLD first appears, rather than
    // whenever 6DoF first happens to ask - which could be a menu, a loading
    // screen, or the headset on the desk. Any difference between that moment
    // and where the player actually settles is applied to the camera forever
    // after, and reads as "the view sits high until I press recentre".
    // Position only; the direction they are facing is theirs to choose.
    bool  six_dof_anchor_on_enter = true;

    bool  six_dof_body_frame = true;
    // If turning your head makes the swing WORSE with the above on, the head
    // yaw the camera receives runs the other way and this flips it back.
    bool  six_dof_body_frame_invert = false;

    float ipd_mm        = 64.0f;   // interpupillary distance

    // ONE OPTICAL AXIS WHILE SCOPED - see stereo.cpp for the full reasoning.
    // A scope has an exit pupil and only one eye can use it; in VR both eyes
    // see the picture from their own position and disagree about where the
    // reticle points. 0 mm while scoped puts them on one axis.
    bool  scope_mono    = true;   // master switch, off = today's behaviour
    float scope_ipd_mm  = 0.0f;    // separation to use while scoped
    // Also collapse it for IRON SIGHTS, which have no optic and so never set
    // PlayerIsScoped(). Detected from the projection narrowing when aiming -
    // measured at about 2.75x - so it needs no per-weapon knowledge.
    bool  scope_mono_ads = false;
    // Which row of the view basis is "right". The axis test settled row1 = up,
    // which leaves 0 and 2; this is measured in the headset, not guessed.
    // Take the eye-separation basis from the site that actually TURNS with the
    // player, rather than the one whose basis was measured to be world-fixed.
    bool  stereo_basis_from_lever = true;
    int   eye_axis_row  = 0;
    bool  eye_swap      = true;   // flip if the depth is inverted
    int   stereo_log_every = 600;

    // Alternate-eye fast path. Only the eye this frame was rendered for gets
    // new pixels; the other eye's swapchain still holds the image it was last
    // given, and OpenXR composites the most recently released image - so
    // re-uploading it is a whole crop-sized blit of pure waste. Off = the older
    // route through two hold textures (3 blits/frame instead of 1). Kept as a
    // fallback in case a runtime dislikes an untouched swapchain.
    // *** THE DESKTOP CLOCK MUST NOT PACE THE HEADSET. *** (prior art lesson 7)
    //
    // The VR frame is submitted inside the game's own Present, so a V-Synced
    // desktop present makes the monitor's refresh the clock for the whole VR
    // loop. Alternate-eye rendering suffers most: each eye is redrawn every
    // other frame, so uneven pacing leaves one eye stale longer on some frames
    // than others, and irregular staleness reads as flicker.
    //
    // Halo MCC VR ships the same switch, with the same trade: the desktop
    // window may tear, the headset never does.
    bool  desktop_present_unlocked = true;

    // *** THE LOG IS THE SUPPORT CHANNEL, SO IT IS ON. ***
    //
    // %LOCALAPPDATA%\theHunterCotWVR\cotwvr.log, with the previous run kept
    // beside it. Measured cost in normal play: about 2.4 lines a second, which
    // is the once-per-five-seconds counters and nothing else - not a frame's
    // worth in an hour. It is a switch rather than a fact of life because the
    // diagnostic options multiply that rate, and because somebody will want it
    // gone on principle.
    //
    // OFF does not silence the opening: what the game's build is, whether the
    // hooks took and what was applied are written before cotwvr.ini has even
    // been read. A bug report needs those, and a log that can be turned off
    // before it says anything is a support channel that fails when it is
    // needed. Off stops the commentary from there on.
    bool  logging = true;

    // *** RUN ON A BUILD THE ADDRESSES WERE NOT TAKEN FROM. ***
    //
    // Off, and it should stay off. The mod finds the engine by fingerprint
    // (apex.h); on a mismatch every hardcoded address is a guess, so all of
    // them are refused and the mod does nothing rather than write to whatever
    // happens to live there now.
    //
    // ON says "I know, try anyway". Worth having for a game PATCH, where the
    // code may be nearly the same and the addresses may survive.
    //
    // *** DO NOT USE IT FOR ANOTHER STORE'S BUILD. *** The Epic executable was
    // compared byte for byte against the Steam one (tools\compare_builds.py):
    // identical PE timestamp, and a completely different link. Functions have
    // moved by varying amounts - +0x37B0 for many, +0x3490 and +0x34E0 for
    // others - and eleven of the recorded addresses do not exist in it at all.
    // The identical timestamp is not evidence of identical code, which is
    // exactly what it looked like before anyone measured. Turning this on there
    // does not risk a crash so much as guarantee writing to the wrong code.
    bool  apex_ignore_fingerprint = false;

    bool  aer_reuse_swapchain = true;
    // Submit each eye with the pose ITS pixels were drawn from, rather than
    // one pose for both. Only alternate-eye rendering needs it, and it is
    // the reason AER never felt smooth: the eye that was not redrawn this
    // frame was being presented as though it had been, so the runtime
    // reprojected it from a viewpoint a frame in its future - and since the
    // eyes alternate, that error changed sides every frame.
    bool  aer_per_eye_pose = true;

    // Report where the per-frame time actually goes (wait / our GPU work /
    // submit), so performance work is measured rather than guessed.
    int   perf_log_every = 0;      // frames between reports, 0 = off

    // Render the whole frame twice per eye instead of alternating eyes across
    // frames. Both eyes then come from the same instant, which is the only way
    // to stop motion corrupting the disparity. Costs roughly half the framerate.
    bool  full_rate_stereo = true;

    // *** WHETHER THE SCENE-DRAW HOOK IS INSTALLED EVEN WHILE FULL-RATE IS OFF.
    //
    // On (the default) the hook is installed whenever stereo is, and left idle
    // by its own per-frame gate - which is what lets the panel switch INTO
    // full-rate without a relaunch. It used to be installed only when full-rate
    // was already on, back when the hook was experimental, and the result was a
    // control that worked in one direction only: launch in full-rate and both
    // ways worked, launch in alternate-eye and half the row did nothing,
    // silently.
    //
    // Off restores that older, more cautious behaviour: no detour at all in the
    // alternate-eye path, at the price of needing a relaunch to reach full-rate.
    bool  full_rate_hook_always = true;

    // Scene draws to wait through before full-rate engages. The first crash
    // engaged it 0.7 s into startup, while Steam and the engine were still
    // coming up; ~20 s of warm-up avoids testing re-entrancy against a
    // half-initialised game.
    int   full_rate_warmup_frames = 1800;
    // Restore the engine's frame-clock counters around the second-eye replay,
    // so the world advances once per eye PAIR. Without it the second present
    // measures ~0 elapsed time and the whole game runs in slow motion.
    bool  full_rate_freeze_clock = false;

    // Tell the engine that ZERO time passed while the second eye was rendered -
    // which is the truth, and the proper fix for the world advancing twice per
    // eye pair. The delta reaches the engine by two routes (the present path's
    // float argument and field 0 of the scene description) and both are zeroed.
    bool  full_rate_zero_dt = true;

    // The second eye is a duplicate of what the player already saw, 64 mm to the
    // side, so it does not need to reach the monitor. Skipping its flip saves a
    // full present per frame.
    bool  full_rate_skip_second_present = true;

    // Put the engine's global time object back to how it was before the replay.
    // Whatever computes the frame delta ticks inside the replayed region, so
    // without this one real frame's elapsed time is split across two ticks and
    // everything time-based - walking, animations, reloads - runs at half speed
    // while the game's own fps counter reads double.
    bool  full_rate_freeze_time = false;

    // Write the REAL frame interval into the engine's delta once per frame.
    // Restoring the time object is not enough on its own - the tick's baseline
    // timestamp lives somewhere else, so the two ticks per frame each measure
    // their own half and the engine ends up believing frames are twice as fast
    // as they are. Measuring the frame ourselves and stating it sidesteps the
    // engine's bookkeeping entirely.
    bool  full_rate_correct_delta = false;

    // The engine keeps a raw delta and a SCALED one. Restoring the time object
    // fixes the raw (the world runs right) but leaves the scaled at half - and
    // the player and the UI are what read the scaled one. Rebuild it from the
    // raw delta and the dilation the game itself was using.
    // WRONG AND OFF: +0x2C is a fixed 1/30 timestep, not a scaled delta, so
    // this was writing computed garbage over an engine constant.
    bool  full_rate_fix_scaled_delta = false;

    // TRIED AND IT CRASHED THE GAME - off, and not to be retried casually.
    // Freezing the performance counter for the replay also froze it for the
    // OpenXR runtime and DXGI, which run INSIDE the replay via our own Present
    // hook. Stopping time under a compositor that is computing predicted
    // display times is not survivable.
    bool  full_rate_freeze_qpc = false;

    // Instead: substitute the value at the point it is READ.
    //
    // The engine's delta getter is a pure function of a stored field, so the
    // whole phase problem - +0x14 reading 10.16 ms during the frame and 5.05 ms
    // between frames - disappears if the getter simply returns the true frame
    // interval whenever full-rate is driving. Every consumer is then correct at
    // every point in the frame, whatever the stored field happens to hold, and
    // no engine state is written at all.
    bool  full_rate_patch_delta_getter = true;

    // Hooking the SECOND delta getter (0x0EB020) crashed the game on startup -
    // it is 0x18 bytes long and 32 bytes from the tick, which is also hooked.
    // Its field is kept in step from the first getter instead.
    bool  full_rate_hook_second_getter = false;

    // Superseded by correcting the value at the getter. Off, because every hook
    // installed is a risk surface and this one no longer earns its place.
    bool  full_rate_hook_timer_tick = false;

    // A delta below this fraction of the real frame interval is treated as a
    // half-frame FRAGMENT and rounded up to the full interval; anything at or
    // above it is already correct and passes through untouched. 0.7 sits
    // between the two clusters that were measured (5.05 ms and 10.16 ms against
    // an 11.1 ms frame). Raise it toward 1.0 if the player or UI still run
    // slow; lower it if the world runs fast.
    float full_rate_fragment_threshold = 1.0f;

    // Which callers of the delta getter get corrected, by the flag they pass:
    //   0 = only the pause-respecting reads
    //   1 = only the pause-ignoring reads
    //   2 = both
    // The world sums two reads per frame and is already right; the player reads
    // once and is short. They cannot be told apart by VALUE - measured - so this
    // is the remaining discriminator.
    int   full_rate_patch_flag = 2;

    // Correct only the delta read made from THIS call site (an RVA). 0 = all of
    // them. Value and flag both failed to separate the world from the player;
    // the address the read comes from cannot fail to, because they are
    // different code.
    // 0 = both, 1 = only kDeltaReadA, 2 = only kDeltaReadB, 3 = neither.
    // Measured: only the first read matters at all - the second changes nothing.
    int   full_rate_patch_callsite = 1;

    // The replay re-runs BUILD with the engine's own description buffer, which
    // carries this frame's delta - so the world gets advanced a second time.
    // Scan the buffer for fields holding that delta, zero them for the replay,
    // and restore them after.
    bool  full_rate_zero_desc_dt = true;
    // Log the call sites and how often each asks per frame. A site asking TWICE
    // is summing halves and is already correct; one asking ONCE is short.
    bool  full_rate_log_callsites = false;

    // Skip the engine's clock update during the replay. TRIED AND REJECTED:
    // 0x00143330 does render work as well as timing, and skipping it made the
    // world flicker black. Left switchable, but it is not the answer.
    bool  full_rate_skip_clock_update = false;

    // --- head tracking -----------------------------------------------------
    bool  head_tracking = true;
    // Which camera site the head rotation is written to. The position lever
    // (site 1) is what steers the picture; the basis source (site 0) tracks the
    // view but was only a weak lever. Both are tried because whether ORIENTATION
    // is writable is a different question from whether POSITION is.
    bool  head_invert   = false;
    // Rotate the direction vector going INTO the camera builder (the input),
    // rather than rewriting the basis rows it produces (derived output).
    // Read-only hunt for the field inside the camera object that holds the view
    // orientation. Costs nothing when off.
    bool  camera_object_probe = false;
    // Axis test: 0 = use the real head pose, 1/2/3 = force a fixed yaw/pitch/roll
    // so the sign and axis can be seen rather than guessed.
    int   head_test_axis    = 0;
    float head_test_degrees = 30.0f;
    // Sweep the test angle back and forth instead of holding it fixed. A fixed
    // offset is very hard to judge by eye; a world that rocks on its own while
    // the player stands still is unmistakable.
    bool  head_test_sweep   = true;

    // Write the head yaw into the engine's OWN yaw field rather than rewriting
    // the matrix the engine derives from it. The field was found by correlating
    // every word of the camera object against the view yaw: +0x4C fits "yaw in
    // radians" at r = -1.000. Rewriting derived output is what made every
    // earlier attempt appear to do nothing.
    bool  head_write_yaw    = true;
    int   head_yaw_field    = 0x4C;
    // Pitch: a candidate, not yet confirmed. +0x50 is the float immediately
    // after the yaw and spans -1.099..+1.118 rad (~ +/-63 deg), which is what a
    // first-person pitch clamp looks like.
    bool  head_write_pitch  = true;
    int   head_pitch_field  = 0x50;
    // Pitch has its own sign flag: the two axes are separate fields with
    // independently unknown conventions, so one shared flag would mean fixing
    // one by breaking the other.
    bool  head_pitch_invert = true;

    // *** HEAD ROLL - tipping your head sideways so the horizon tips too. ***
    //
    // Roll has been computed from the headset all along and simply never sent to
    // the game, which is why tilting your head leaves the world stubbornly level.
    //
    // THE FIELD IS A GUESS AND THAT IS WHY IT IS A SETTING. Yaw sits at +0x4C and
    // pitch at +0x50, so the roll of an ordinary euler triple should be +0x54 -
    // but that has never been confirmed on this engine. If +0x54 turns out to be
    // something else, move it from the overlay instead of waiting for a rebuild.
    bool  head_write_roll = true;

    // *** THE VIEW FROM A DIFFERENT HOOK - the structural answer to the ratchet.
    //
    // Every other approach wrote an ANGLE the engine stores and later reads back.
    // That is the bug: a camera transition samples the aim at its start and again
    // at its end, which is why one stance change gives TWO rotations, and why
    // aiming - a camera transition like any other - does it too.
    //
    // This turns the view by rotating the matrix kViewCommit builds (the
    // EulerToMatrix call returning to 0x0049E8F1) and writes nothing at all. With
    // no stored angle carrying our offset there is nothing for a transition to
    // sample, so the ratchet is not patched, it becomes impossible.
    //
    // The cost is honest: this moves the VIEW only. The weapon follows through
    // head_matrix_rotate, which drives the other two call sites and was already
    // measured drift-free. Gameplay aim stays where the ENGINE thinks it points.
    bool  head_view_basis = false;
    // *** THE RATCHET FIX, and the first one aimed at an instruction the game
    // itself pointed at rather than one that was reasoned about. ***
    //
    // 0x00638730 re-seeds the aim from the current view. Cheat Engine caught its
    // write to the yaw accumulator firing exactly three times for three crouches.
    // It is correct code: in a game where only the player can move the camera,
    // "where the camera looks" IS the aim. Head tracking breaks that assumption,
    // so the head offset gets adopted as permanent aim - once entering the
    // transition and once leaving, which is the two rotations per stance change,
    // and why aiming does it too.
    //
    // This lets the routine run and puts the aim back afterwards.
    bool  head_keep_aim_on_reseed = false;
    // *** THE CODE CAVE - correct the write instead of blocking it. ***
    //
    // Blocking the accumulator write stops the ratchet but causes the transient
    // swing, because the routine writes a consistent PAIR and removing one
    // without a replacement breaks the invariant between them. This lets the
    // engine's own store run, with our head offset taken out of the value first.
    //
    // Takes precedence over head_keep_aim_on_reseed: both target the same six
    // bytes, and installing one over the other would leave half an instruction.
    // ON by default: this is the fix for the crouch/aim rotation, confirmed
    // in the headset, and with head tracking off it corrects by zero - which
    // makes the cave behaviourally identical to the original instruction.
    bool  head_reseed_detour = true;
    // *** THE SAME BUG, BY A SECOND ROUTE: HOLD BREATH. ***
    //
    // Crouch, stand, prone, jump and aim all re-seed through kAimReseed, which
    // head_reseed_detour above corrects. Hold breath does not go anywhere near
    // that function - it arms a deferred latch inside the aim update and
    // re-seeds from the camera on the next pass, at kHoldBreathReseedYaw/Pitch.
    // No placement of the other cave could ever have caught it.
    //
    // Two more 5-byte detours, taking the same head angles out of the same
    // accumulators. Refuses to patch unless the bytes are exactly the expected
    // store, and with head tracking off it corrects by zero.
    bool  head_reseed_holdbreath = true;
    // Which way, and how much, to take out. The sign convention of that field
    // was never measured directly - -1 is as likely as +1 - so it is a setting
    // rather than a constant, and 0 makes the cave a no-op without removing it.
    // MEASURED, not guessed: sweeping this, the rotation stopped dead at
    // -1.0 and came back past it. A coefficient of exactly one means the
    // accumulator was gaining PRECISELY the head angle.
    float head_reseed_correct_scale = -1.0f;
    // Also block the re-seed's write to the LIVE aim yaw (0x0063888E).
    // Blocking the accumulators alone stopped the drift ACCUMULATING but
    // left a swing into each crouch that undid itself coming out. This is
    // that swing. Riskier - it is the field the renderer reads - so it is a
    // separate switch and the first thing to turn off if a camera move that
    // should happen stops happening.
    bool  head_block_reseed_yaw = false;
    // *** DISPROVEN AND DEFAULTED OFF - kept as a warning. ***
    //
    // The theory: 0x004B720B decomposes our rotated view matrix into the stored
    // aim, so removing our part there would stop the ratchet. Measured with a
    // real offset applied, on every single sample:
    //
    //   [bake] aim euler yaw +2.1592 -> +2.1592 (moved +0.00 deg);
    //          head offset yaw -33.28 deg
    //
    // The engine writes it back BIT-IDENTICAL. That matrix carries nothing of
    // ours, exactly as it carried nothing before the view-basis hook existed, so
    // the call is innocent twice over.
    //
    // And the correction was worse than useless. `out[0] = got - headYaw` with a
    // NEGATIVE offset ADDS 33.28 degrees every call - which the log shows in the
    // successive `before` values: 2.1592, 2.7399, 3.3208, stepping by exactly our
    // own offset. Switching this on manufactured a second ratchet on top of the
    // one being hunted. That is twice now that a "fix" for this bug has made it
    // worse, and both were shipped on reasoning instead of on a measurement.
    bool  head_bake_undo = false;
    int   head_roll_field = 0x54;
    bool  head_roll_invert = true;
    // Less than 1 tilts the world less than your head, which some people find
    // steadier; 1.0 is one-for-one with your real head.
    float head_roll_scale = 1.0f;

    // *** SMOOTHNESS: hand the compositor the pose the frame was really drawn
    // with, instead of one located a frame later. ***
    //
    // Inside the Present hook, SubmitFrame runs before HeadTrackTick. So the
    // angles that rotated the game camera for the frame being submitted came
    // from the pose located at the PREVIOUS present. Submitting the fresh pose
    // tells the runtime the image was rendered from somewhere it never was, and
    // its reprojection then starts one frame of head motion out of place. That
    // error is zero while the head is still and grows with how fast it turns -
    // which is judder you only notice while moving.
    //
    // Off by default so it can be felt against the old behaviour in one session.
    bool  submit_rendered_pose = true;

    // *** DO NOT PROMISE THE COMPOSITOR A POSITION THE GAME NEVER RENDERED. ***
    //
    // The mod injects head ROTATION into the engine's camera; it does not move
    // the camera with head POSITION - the game has no 6DoF. But the pose we
    // submit carries the runtime's full tracked position, so the compositor
    // dutifully reprojects for translation that is not in the pixels. Every
    // millimetre of tracker noise becomes a shift of the whole image: a small
    // vibration, in the headset only, in BOTH eyes, with a perfectly clean
    // desktop mirror - and it survives with head tracking off, because the
    // pose is submitted either way. Measured 2026-08-13, after the rendered
    // image itself was proven stable.
    //
    // Freezing the declared position (rotation stays live, so rotational
    // timewarp still does its job) makes the promise match the pixels. The
    // eye separation is unaffected: it is baked into the images themselves.
    bool  submit_frozen_position = false;
    // *** AND THE ORIENTATION - a TEST, not a shipping setting. ***
    //
    // Two things rotate this view: the engine (from the angles we inject) and
    // the compositor (which warps the submitted image to the live pose). If
    // the two do not agree exactly - a clamp, a quantisation, a fraction of a
    // frame - the difference wobbles per frame, in the headset only, and the
    // desktop mirror stays clean because it is never warped. Measured
    // 2026-08-13: head tracking OFF stops the headset shake, which is what
    // that disagreement would predict.
    //
    // Freezing the declared orientation removes the compositor's half. The
    // picture then follows the head ONLY through the engine, so expect more
    // latency and judder when turning fast - that is the cost, and it is why
    // this is a diagnostic. If the shimmer dies here, the fix is to make the
    // submitted pose describe exactly what the engine rendered.
    bool  submit_frozen_orientation = false;

    // *** LATCH THE PER-EYE CROP RECTANGLE. ***
    //
    // The crop is recomputed every frame from the fov the runtime reports for
    // that frame. That number wobbles in its last digits, and every wobble
    // moves the rectangle copied into the headset swapchain by a pixel or
    // two - a vibration the headset shows and the uncropped desktop mirror
    // does not. Measured 2026-08-13 with every temporal feature off and the
    // submitted pose frozen, and it tracks head tracking on/off exactly.
    // TIER1_FOV_PLAN.md gives the rule for the runtime fov: latch once.
    bool  tier1_crop_latch = true;

    // The yaw probe: at the view site, log the angles the engine is about to
    // build the frame's view from (cam+0x4C) against the head angle we handed
    // it. Smooth head + stepping engine = the injection path quantises, which
    // is the last standing explanation for the headset-only vibration (clean
    // on the desktop, stops with head tracking off, predates the TAA/DLSS
    // work, survives with compositor warping disabled). Read-only, two lines
    // a second.
    bool  head_yaw_probe = false;

    // *** ROLL IS THE HEADSET VIBRATION. LEAVE IT TO THE COMPOSITOR. ***
    //
    // Measured 2026-08-13, with the TAA fix, DLSS and weapon_3d all off so
    // nothing else could contribute:
    //   yaw only          - calm when still
    //   yaw + pitch       - calm when still, a brief shake WHILE pitching
    //   yaw + pitch + roll- vibrating constantly (the long-standing fault)
    // The engine applies our three EULER angles while we tell the runtime the
    // frame was rendered from its QUATERNION; where the two rotations
    // disagree, the compositor's warp over- or under-corrects by an amount
    // that changes as the head moves. Roll is the worst offender - its field
    // was identified by inference (+0x54) and an error there rotates the whole
    // image about the view axis.
    //
    // Not injecting roll costs NOTHING: head roll has no gameplay meaning (aim
    // does not depend on tilting your head), and the compositor still rolls
    // the picture correctly from the pose we submit. head_write_roll is
    // therefore DEFAULT OFF; the residual pitch transient is smaller and is
    // the next thing to chase (the engine hard-clamps pitch at +/-90, which is
    // a candidate).

    // *** THE FLAT SCREEN - what makes the game's own menus readable. ***
    //
    // The menus are drawn flat at screen scale, then stretched over the whole
    // field of view, so their edges - and most of the options - land outside
    // where the eye can comfortably look. This puts the game's image on a
    // floating panel instead: copied verbatim, with the panel's size in METRES
    // deciding how big it looks. No scaling and no shader.
    bool  menu_screen = false;          // the hotkey and the overlay toggle
    // Hotkey, a Windows virtual key code, rebindable from the overlay. 0x71 = F2.
    int   menu_screen_key = 0x2E;
    // Find the menus without being asked. Two signals, because one is not
    // enough: no player camera catches the MAIN menu, and a visible mouse
    // cursor catches the IN-GAME one - which leaves the world running, so the
    // camera test cannot see it. A menu you click through needs a cursor; a
    // first-person game in play does not.
    bool  menu_screen_auto = true;
    // Width and distance in metres. 2.4 m at 2.2 m away is a very large monitor
    // at desk distance.
    float menu_screen_width_m = 2.4f;
    float menu_screen_distance_m = 1.80f;
    // Blank the world behind the panel - applied to the MAIN menu only, where
    // the world IS the stretched menu the panel replaces. In an in-game menu you
    // are stood in the world, and blanking it there is exactly the mess the
    // first attempt made.
    bool  menu_screen_hide_world = true;

    // How long the mouse cursor must stay visible before it counts as a menu.
    // A single sample fired 26 times in one session on ordinary mouse-look, and
    // each one dropped the flat cinema screen in front of the player. A real
    // menu holds the cursor up; a mouse-look flicker does not. 0 = believe it
    // instantly, which is the behaviour that produced those 26.
    int   menu_cursor_hold_ms = 0;
    // The cursor test did not fire on this game's in-game menu. Rather than
    // guess a third time, log every candidate signal the moment it changes -
    // open the menu once and the trace says which one moved.
    bool  menu_screen_log_signals = false;
    // Treat "the game let go of the mouse cursor" as a menu, as well as "a
    // cursor is visible". ESC opens the menu WITH a cursor; START on the pad
    // opens the same menu WITHOUT one, so cursor visibility alone only catches
    // half the ways in. Letting go of the clip is about the game's input mode,
    // not about the mouse, so it should hold either way.
    bool  menu_screen_use_clip = true;

    // --- the held weapon / viewmodel --------------------------------------
    // The weapon rides the camera, so it inherits the stereo eye shift and ends
    // up on identical pixels in both eyes - zero disparity, which reads as flat
    // and infinitely far away. Cancelling that shift at the viewmodel's own
    // transform site gives it the depth of a real object at arm's length.
    //
    // Which call site that is has to be IDENTIFIED before it is written to:
    // weapon_test_sweep swings the site sideways so it can be seen.
    int   weapon_site       = 0x004B6F11;   // suspect: same position as the camera
    bool  weapon_test_sweep = false;        // identify: does the WEAPON swing?
    float weapon_test_amount = 0.25f;       // metres
    // Sweep EVERY call site in turn, a few seconds each, so one run tests all
    // of them instead of one launch per candidate. CTRL+ALT+W marks the one on
    // screen when the weapon moves.
    bool  weapon_scan       = false;
    int   weapon_scan_seconds = 5;
    bool  weapon_stereo     = false;        // fix: cancel the inherited shift
    // 1.0 fully cancels it (weapon sits at its true distance). Lower reduces the
    // effect; above 1 exaggerates it, which some people prefer on a close object.
    float weapon_depth      = 1.0f;

    // Search the game's D3D11 constant buffers for the camera position and
    // basis. The weapon is not built by any transform-builder call site, so the
    // render view is the only place left to give it depth - and this finds where
    // that view actually lives by looking for numbers we already know.
    bool  weapon_cb_scan    = false;

    // *** THE WEAPON DEPTH FIX ***
    // CONFIRMED by holstering: the 192-byte constant buffer at +0x0000 holds the
    // viewmodel's transform - 0.27-0.52 m from the camera, twice per frame, and
    // written not at all while the weapon is holstered. It is camera-relative,
    // so it inherits the stereo eye shift and ends up with zero disparity.
    // Cancelling that shift gives the weapon real depth.
    bool  weapon_cb_stereo  = false;
    int   weapon_cb_size    = 192;
    int   weapon_cb_offset  = 0;
    // Which axis of the weapon's own space is sideways. Measured, not guessed -
    // the same question the eye-separation axis needed, and the same answer
    // method: try each and look.
    int   weapon_cb_axis    = 0;
    // 1.0 puts the weapon at its true distance. Higher exaggerates its depth,
    // which some people prefer on something this close.
    float weapon_cb_depth   = 1.75f;

    // Manual placement of the held weapon, in metres, in its own space. Useful
    // in its own right - a gun framed for a monitor often sits awkwardly in a
    // headset - and it doubles as the decisive test that this matrix really is
    // what positions the weapon: if it MOVES, the target is confirmed.
    // Destructive tests, because a nudge that does nothing cannot distinguish
    // "wrong buffer" from "right buffer, wrong convention".
    //   0 = off   1 = collapse the matrix (the gun MUST vanish or distort)
    //   2 = shove it 5 m (unmissable)   3 = shift the last COLUMN instead
    // FINDING THE WEAPON BY HIDING IT - the Shader Toggler approach, and the
    // one that runs backwards instead of forwards. Skip one shader's draws until
    // the weapon disappears; then its draws, and therefore its constant buffers,
    // are known exactly rather than guessed at.
    int   shader_hide_index = -1;   // first shader to hide, -1 = hide nothing
    int   shader_hide_count = 1;    // how many in a row - bisect with this
    bool  shader_list       = true; // keep the shader table populated
    // List every shader with the SAME crc32 Shader Toggler displays, so a
    // hash noted in its UI can be looked up here directly.
    bool  shader_dump       = false;
    // Print the contents of the weapon's own constant buffers. With the
    // shaders identified there are only four, so they can simply be read.
    bool  weapon_cb_dump    = false;

    // *** THE WEAPON DEPTH FIX THAT NEEDS NO MATRIX. ***
    // For an object at a fixed distance, stereo disparity IS a horizontal shift
    // on screen. The weapon's draws are identified exactly by their pixel
    // shaders, so they can be drawn through a shifted viewport - nothing else in
    // the frame is touched, and no transform has to be found at all.
    bool  weapon_screen_stereo = false;
    // Off by default: the weapon's vertex shader turned out to be shared with
    // other geometry, so matching on it shifted objects out in the world too.
    // Only worth turning on if the weapon's depth pass proves to need it.
    bool  weapon_shift_use_vs  = false;
    float weapon_distance_m    = 0.400f;   // how far the gun sits from the eye
    float weapon_shift_scale   = 1.00f;   // taste: >1 exaggerates the depth
    bool  weapon_shift_invert  = false;   // if the depth comes out inside-out
    int   weapon_shift_width   = 2574;    // render width, for the pixel maths

    // The three PIXEL shaders that draw the held weapon, found with ReShade's
    // Shader Toggler and identified by the same CRC-32 it uses. A draw using one
    // of these IS the weapon - no searching, no heuristics.
    // *** THE VIEWMODEL PASS, FROM THE RENDERDOC CAPTURE. ***
    // The visible weapon is a forward pass after post-processing: ten meshes,
    // each drawn three times through plain DrawIndexed. Ask the DRIVER what is
    // bound rather than our own shadow table, which a game that cycles deferred
    // contexts can exhaust - after which every draw looks like "nothing bound".
    bool  weapon_pass_live_ps = true;
    // Shift the two depth-only rounds as well, matched by an index count the
    // colour round taught us. Without this the colour moves and the depth does
    // not, and the gun fails its own depth test - which is the flicker.
    bool  weapon_pass_depth_rounds = true;
    // The accounting that says whether the hooks see the pass at all.
    bool  weapon_pass_diag  = true;
    // Hook the DEFERRED contexts as well. The game records its frame on them
    // and replays command lists on the immediate one, so without this we see
    // 0.9 draws a frame out of 718 and the weapon can never be reached. It
    // puts our code on the renderer's worker threads, so it is a switch: if a
    // build ever becomes unstable, turn this off to get back to a working game.
    bool  hook_deferred_context = true;
    // Remove the viewmodel entirely. A flat gun welded to the face reads worse
    // than no gun, which is why several shipped VR mods do exactly this.
    bool  weapon_hide       = false;
    // *** MATCH THE VIEWMODEL BY INDEX COUNT, WITHOUT ANY SHADER IDENTITY. ***
    //
    // Everything else here hangs off the colour round, which is matched by
    // pixel-shader CRC - and that match has NEVER fired, in any session. Not
    // because the CRCs are wrong (a capture of this exact build finds all three,
    // current) but because a shader has to be fingerprinted AT CREATION to be
    // recognisable later, and the mod only ever sees ~101 of the 2,039 shaders a
    // frame uses. The game builds its library before our hook lands.
    //
    // The index counts do not need any of that. Measured over all 2,214 draws of
    // a frame, seven of the viewmodel's counts appear NOWHERE else - so matching
    // on the count alone is exact, needs no shader, and survives patches that
    // recompile shaders. Seeded below rather than learned, because learning ran
    // through the colour round that never fires.
    // *** THE ENGINE'S OWN FIRST-PERSON TAG - USE THIS, NOT THE INDEX COUNTS. ***
    //
    // The renderer marks first-person geometry with stencil bit 6. Measured over
    // two captures: every viewmodel draw carries it, not one of the ~2,180 world
    // draws per frame does. Zero false positives, zero false negatives.
    //
    // It replaces the whole index-count scheme and its failures: no per-weapon
    // seeding, no confusion with a fence that draws the same triangle count at
    // close LOD, and every round of the pass is tagged so nothing half-vanishes.
    // Reading it is one virtual call with no AddRef/Release.
    // MOVE ONLY WHAT THE EXACT RULES CLAIM.
    //
    // The engine's first-person stencil tag matched 57/57 viewmodel draws and
    // 0/4319 world draws across two captures. The older rules - a shader
    // fingerprint that misses most of the frame, an index-count table (a fence
    // shared 3252 with a gun mesh), a neighbour rule - are right most of the time
    // and wrong sometimes, and a wrong claim moves something that should be still.
    //
    // Every attempt to move MORE draws today made the flicker worse. This tests
    // the opposite. If pieces of the gun go flat with this on, those pieces are
    // exactly the ones that need identifying properly.
    // NEVER MOVE THE SCOPE'S MAGNIFIED PICTURE.
    //
    // It is a screen-space quad that samples a COPY of the scene, not real
    // geometry, and it is positioned from the camera rather than the weapon -
    // hold free-look while aimed and it slides away from the scope with your head.
    // Moving it slides the picture against the world it is a picture of, so the
    // scope parts inside it sit at their unshifted positions while the same parts
    // outside are shifted. That boundary alternating IS the flicker.
    // *** TRIED, DEFAULT OFF: the glass and the picture are the SAME draw. ***
    //
    // Excluding draws that sample a full-size copy of the scene stopped the
    // magnified picture from being moved - and stopped the GLASS moving with it,
    // because they are one draw, not two. The lens does not show geometry; the
    // lens IS the picture.
    //
    // And the flicker survived. That is the useful half: with the glass not being
    // moved AT ALL, the scope and binoculars still flickered - so moving the glass
    // was never what caused it.
    bool  weapon_3d_skip_scene_picture = true;
    // MATCH THE OPTIC GLASS BY ITS EXACT PIPELINE STATE.
    //
    // Measured across six captures at four resolutions: depth GREATER_EQUAL with
    // writes off, stencil ALWAYS with ref 0x12 and writeMask 0x12 REPLACE, two
    // render targets with motion vectors second, SrcAlpha blending. No other draw
    // in those frames shares that conjunction.
    //
    // It replaces guessing by index count and by which buffer a draw happened to
    // read, and it treats the glass as what the footprints prove it to be -
    // weapon geometry - so it takes the body's own matrix rather than a
    // rasterizer approximation.
    // THE LENS MASK TAKES THE SAME ROUTE AS THE LENS.
    //
    // Measured: they are the SAME DISC issued twice - shared index and vertex
    // buffers, post-VS clip positions bit-identical to six decimals. The mod was
    // sending one copy through the constants and the other through the
    // rasterizer, and the two are asymmetric: the rasterizer never declines,
    // the constants sometimes do. On a frame where the constants fail the glass
    // does not move and the mask still does - two positions for one object.
    //
    // Binoculars stamp no mask, so they cannot split this way. That is why they
    // came out fixed and scopes did not.
    //
    // 0 restores the previous routing exactly.
    // TIER 1 FOV, STAGE 0: measure the camera's own projection and log it.
    //
    // Read-only instrumentation. This renderer has no separable projection
    // matrix - all 26,397 constant buffers were tested - so it is recovered from
    // the structure of the shared world->clip matrix instead. It must report
    // 90.0 vertical / 85.6 horizontal / aspect 0.9254 / near 0.0094 on the
    // FULLVIEW preset with the game's slider at 90. If it does not, the reader is
    // wrong and every later stage would be built on a bad instrument.
    // TAA ghosting under full-rate stereo.
    //
    // Both eyes render every frame, so the image in TAA's history is the OTHER
    // EYE, not the previous frame - but the engine hands TAA the previous
    // FRAME's view-projection to reproject against. This replaces it with the
    // matrix of the render that actually produced the history.
    //
    // Off by default: it rewrites what every temporal and velocity pass in the
    // frame reprojects against.
    bool  taa_prev_from_last_render = false;

    // THE TAA RESOLVE PROBE. Read-only, and the gate on step 2 of
    // docs\TAA_PER_EYE_PLAN.md.
    //
    // Finds the engine's temporal-resolve draw by resource topology - one draw
    // in 785 in both captures - and reports, per REAL FRAME: how many resolves
    // ran, what eye tag each carried, and whether both read the SAME history
    // texture. Those three numbers decide whether per-eye history is built at
    // all, and the last attempt at this failed precisely because its counter was
    // never printed by any of 113 logs.
    //
    // Costs one bool read per Draw() call while off. Arm it WHILE PLAYING - it
    // starts the moment it is switched on. See taa.cpp for the rule itself.
    bool  taa_probe = false;
    // A diagnostic left on overnight fills the log and is indistinguishable from
    // a broken one, so this stops by itself after N seconds. 0 = never stop.
    // Re-arm by turning taa_probe off and on again.
    int   taa_probe_seconds = 15;

    // GIVE EACH EYE ITS OWN TEMPORAL HISTORY.
    //
    // MEASURED, 2026-08-10, ~1350 frames in the headset: the resolve runs twice
    // per frame, tagged eye 0 and eye 1, and the two CROSS-FEED - eye 0 blends
    // against what eye 1 wrote last frame, eye 1 against what eye 0 wrote this
    // frame. Neither eye ever sees itself, which is the smearing around trees
    // and grass. Two private textures, one per eye, substituted into the
    // history slot for the length of that one draw.
    //
    // THE MASTER OFF SWITCH. 0 = today's behaviour, exactly - nothing is bound,
    // nothing is copied, and the engine's own ping-pong is untouched.
    bool  per_eye_temporal_history = false;
    // The two controls that make a null result mean something. A switch that can
    // only express "better" cannot test anything: if turning the feature on
    // changes nothing BUT one of these visibly changes the image, the plumbing
    // is proven live and the idea is genuinely refuted. If none of the three
    // changes anything, the substitution is not reaching the shader.
    bool  per_eye_temporal_eye_swap = false;   // feed each eye the WRONG history
    bool  per_eye_temporal_starve   = false;   // feed this frame's own colour back
    // THE SECOND HALF. Substituting the pixels alone leaves TAA reprojecting
    // them with the matrix of a DIFFERENT render - eye 1's was off by a whole
    // frame of motion - and a history sample fetched from the wrong place is
    // blur when still and smear when moving. Owner's verdict on the pixels-only
    // build was exactly that. This hands each eye the matrix of its own previous
    // frame, so the pixels and the reprojection describe the same render.
    // Only acts when per_eye_temporal_history is on; supersedes
    // taa_prev_from_last_render, which is ignored while both are set.
    // DISPROVEN 2026-08-11 and defaulted OFF. It rewrites +0x210 / +0x260 of the
    // shared camera block, which were measured - bitwise, 0 of 89 frames - NOT to
    // be the previous frame's view-projection. Luma's Just Cause 3 mod says why:
    // on this engine the projection matrix is not in any constant buffer at all,
    // it lives on the CPU. Kept only so the disproof is reproducible.
    bool  per_eye_temporal_matrix   = false;

    // *** STAGE 1 OF THE REPLACEMENT ROUTE. ***
    //
    // Intercept the engine's temporal resolve and satisfy it ourselves instead of
    // trying to correct it from outside - the architecture Luma uses to put DLSS
    // into Just Cause 3 on this same engine. Whoever owns the pass owns the
    // history, and a history we own is per-eye by construction.
    //
    // At this stage it only passes the frame through: sharp, aliased, ghost-free.
    // Its value is proving the interception before any shader exists.
    bool  taa_replace_pass = true;

    // *** ONE DIAL, BECAUSE THE TWO ALWAYS TRADE AGAINST EACH OTHER. ***
    //
    // Blend weight and clamp strength cannot be set independently in any useful
    // way: loosen one and you have to tighten the other or the picture either
    // smears or crawls. So the panel shows a single strength, calibrated so that
    // 1.00 is exactly the pair the owner tuned in the headset (0.46 / 1.00).
    // Below the panel, taa_blend and taa_clamp_strength remain the base values
    // this scales, so the calibration can be retuned without a rebuild.
    float taa_ghosting_fix = 1.00f;

    // Bring NGX up once and report whether DLSS is available on this machine,
    // and if not, why. Nothing is rendered. It is the first step of the DLSS
    // work and the one most likely to fail outright, so it is asked before
    // anything is built on the answer.
    // Bring NGX up and report what this machine offers, even with DLSS off.
    // A DIAGNOSTIC, which is what the name says - and now what it does.
    // Initialisation for normal use follows dlss_enable (render_hook.cpp).
    bool  dlss_probe = false;

    // Write the motion vectors DLSS needs into a texture, as a second render
    // target on the resolve we already own. The shader has already worked out
    // where each pixel came from, so this costs one target rather than a whole
    // extra pass. Off restores the single-target draw exactly.
    bool  taa_write_motion_vectors = true;

    // Run DLSS (DLAA, native resolution) instead of the mod's own resolve. Off
    // by default: the resolve that is there works, and this is new.
    // Put back the head rotation the camera matrices never carried. MEASURED:
    // none of the four cameras in the shared block turns when the head does -
    // 0.0000 deg over 60 samples, against 0.47..1.52 for the gamepad stick. The
    // reprojection therefore lands short by however much the head turned between
    // the two frames, and content appears to drag with your head.
    // 2026-08-12, REOPENED: the "+1.0, -1.0, no difference" verdict was reached
    // while every DLSS frame was ALSO misregistered by the unreported jitter
    // and the missing exposure - the very bugs fixed today. A quarter-pixel
    // alternating error every frame is easily large enough to mask a head-delta
    // shift. AND the old test could never have passed anyway: both axes shared
    // ONE scale, while yaw and pitch reach the screen through independently
    // unknown sign conventions (the head-invert flags are tuned to the ENGINE's
    // fields, not to UV) - camera_probe.cpp says in as many words that sharing
    // one sign flag "would mean fixing pitch by breaking yaw". Separate scales
    // now; settle yaw first with the head level, then pitch by nodding.
    bool  taa_head_rotation_fix = false;
    // 1.0 is the derived amount for its axis; negative flips that axis alone.
    // If content still drags WITH your head, too small; swims AGAINST you, too
    // large.
    float taa_head_rotation_scale = 1.0f;      // yaw (left-right)
    float taa_head_rotation_scale_y = 1.0f;    // pitch (up-down), OWN sign
    // Use the PREVIOUS frame's head delta instead of this frame's. The mod
    // writes head angles into engine fields; if the engine renders them one
    // frame late, this is the phase that actually matches the picture.
    bool  taa_head_rotation_delay = false;
    // Read the centre texel of the motion-vector texture back every frame and
    // log it against the head shift that was uploaded - the numeric version of
    // the dev overlay's dot grid. Diagnostic, costs a 1x1 copy per frame.
    // 1 = read the motion-vector texture's centre texel back and log it.
    // 2 = the same, but the shader writes DEPTH there instead of the vector
    //     (D3D11 will not copy a texel out of a depth-stencil resource, and
    //     depth is the one input the CPU cross-check cannot otherwise see).
    int   taa_mv_probe = 0;

    // *** OWN THE JITTER: 8 sample positions instead of the engine's 2. ***
    //
    // Adds a small 8-entry Halton offset to the main-view camera's projection
    // at every upload (shadow/reflection cameras excluded by the tangent test),
    // on top of the engine's own two-phase quarter-pixel alternation. DLAA
    // requires at least 8 jitter phases; the engine's 2 are the measured cause
    // of the residual edge-shake, and the estimator's guesswork about which of
    // the 2 phases is live is the measured cause of the texture-shake. With
    // the overlay, the added part is EXACT (we generated it) and the combined
    // sequence covers 8+ positions inside the spec's half-pixel budget.
    bool  taa_jitter_inject = false;
    // Amplitude of the overlay, 1.0 = up to ~0.22 px. The owner's first test:
    // full amplitude kills the rock shake on every preset but makes DISTANT
    // TREES flicker - alpha-tested billboards whose sub-pixel detail pops in
    // and out of coverage as the sample point sweeps a wider area than the
    // art was tuned for. Smaller keeps the 8-position diversity with a
    // tighter sweep; the sweet spot is found by eye.
    float taa_jitter_inject_scale = 0.5f;
    // The jitter hunt (jitterhunt.cpp). 0 = off. 1 = scan memory for the CPU
    // homes of the view-projection, then hardware-watch who writes them
    // (needs the player perfectly still during the scan). 2 = skip the scan
    // and watch the TRUE home found by round 1: the object at
    // [[0x02718A78]+0x5C8], matrix +0x184, jitter row +0x1C4 - the address
    // the staging writer at +0x1308E5 provably copies from.
    int   jitter_hunt = 0;
    // Mode 2's watch offset inside the camera-data object. The ladder so far:
    // 0x1C4 = current VP's jitter row (written by the promote at +0xCF946,
    // which copies it from...) 0x344 = the PENDING VP's jitter row. Accepts
    // hex in the ini ("0x344").
    int   jitter_hunt_offset = 0x344;

    // *** THE JITTER TAKEOVER - the hunt's payoff. *** Detours the engine's
    // own jitter generator (apex::kJitterGenerator) so the WHOLE renderer -
    // per-object matrices, shared block, every compensating pass - samples an
    // 8-phase Halton instead of the stock 2-phase, all self-consistently, and
    // DLSS is told the exact value each eye rendered with (no estimator).
    // This is the fix the shared-block overlay could never be (7e). Off =
    // stock behaviour exactly, hook passes through.
    bool  jitter_take = false;
    // Multiplies the sequence (full = up to ~0.44 px, inside the half-pixel
    // budget). Negative flips both axes if the headset ever says so.
    float jitter_take_scale = 1.0f;
    // The takeover that actually reaches the hot path: the per-frame jitter
    // code is INLINED in the renderer (the detour above only catches a cold
    // caller, measured at ~33 calls/s vs 90+ fps), but it branches on the AA
    // mode at [obj+0x3D8] - and mode 3 is a 16-entry jitter table the game's
    // settings never enable. This forces mode 3 every frame: the ENGINE
    // renders and compensates 16 phases natively. Reporting reads the true
    // per-frame value straight from the composed projection, so it is exact
    // in every mode.
    bool  jitter_mode3 = false;

    // *** THE RESOLVE'S MISSING TEXTBOOK STEP - de-jitter. *** The engine
    // renders with a SIXTEEN-phase sub-pixel jitter (established 2026-08-12;
    // the 2-phase model was fiction), and our resolve reprojected through the
    // raw jittered matrices - so every frame fetched history from a slightly
    // different sub-pixel spot. Under 2 phases that error pair-cancels, which
    // is why the resolve looked fine when built; under 16 it is a universal
    // small shake on every object - the owner's rock/house/table report, and
    // it survives with DLSS off, which is what pinned it here. ON: both
    // matrices are stripped of their exact jitter (extracted per matrix, the
    // col0.col3 identity), the sample position is un-jittered, and DLSS gets
    // CLEAN motion vectors (MVJittered no longer declared). Off = the old
    // behaviour exactly.
    // DEFAULT OFF 2026-08-12 late: in the same evening's builds DLSS lost its
    // clean look ("world looks shimmery with it, before it was clean") and the
    // shake was NOT cured - this change is one of the unvalidated suspects and
    // every one of them goes back to opt-in until re-introduced ONE at a time
    // from the BEST baseline.
    bool  taa_dejitter = false;
    // The camera consistency gate (hold off capture candidates far from last
    // frame's camera). The IDEA is measured-correct - the accepted matrix
    // jumped by meters on 87 of 90 frames at rest, the impostor-camera race -
    // but THIS implementation (reject the only candidate, re-seed after 15)
    // produced 15-frame stale cycles that flickered trees on look movement
    // and did not cure the shake.
    // v2 IS SELECTION, and it is what this switch now means: every main-view
    // camera written during an eye's pass is collected at Hook_Unmap, and the
    // resolve picks the one continuous with last frame's choice (cbscan.cpp
    // NoteCameraCandidate / PickCameraCandidate). Selection cannot starve -
    // there is always a nearest - and it ignores whatever happens to be bound
    // at the resolve draw, which is the thing that could never be trusted.
    bool  taa_camera_lock = false;
    // Report the EXACT jitter to DLSS, extracted from the captured matrix,
    // instead of the (two-phase, now-known-fictional) estimator. Off by
    // default: it entered the tree in the same evening DLSS lost its clean
    // look, so it gets its own A/B from a good baseline before it is trusted.
    bool  dlss_jitter_exact = false;

    // Let DLSS derive exposure from the image. ON: the first build supplied no
    // exposure at all while declaring the input HDR, so DLSS assumed 1.0 against
    // values that go far above it - and its history decisions are made on
    // luminance. Off only to prove what it is worth.
    bool  dlss_auto_exposure = true;

    // *** OFF BY DEFAULT, AND THE PASS BELOW STAYS ON. ***
    //
    // DLSS needs an NVIDIA RTX card. Shipping it on means everyone else starts
    // with a switch that cannot do anything, and a first impression built on a
    // feature their hardware will never run.
    //
    // taa_replace_pass stays ON, so the default is "Per-eye smoothing (mod)":
    // the mod's own resolve, which works on any card of any brand and is what
    // stops the game's own temporal pass blending each eye with the other eye's
    // picture. RTX owners turn DLSS on in one row and get the better picture.
    bool  dlss_enable = false;
    // *** THE JITTER IS WIRED NOW - AND REPORTING ZERO WAS A SPEC VIOLATION. ***
    //
    // The programming guide (3.7.3 rule 3) requires the per-frame sub-pixel
    // offset to be reported whether or not the motion vectors also carry it;
    // zero may only be passed when NO jitter was applied. This engine jitters
    // every frame (two-phase, ~1.478 in m[12] of world->clip), so telling DLSS
    // "no jitter" misregistered every other frame by ~0.5 px - which punishes
    // exactly high-frequency moving content, i.e. the foliage and snow smear.
    //
    // The conversion that stalled the first attempt is now understood: the
    // engine adds an NDC-space offset j into the projection, and in the combined
    // world->clip matrix that lands as m[12] += j * m[15] (m[15] is the view
    // translation's w coefficient, ~-8487 here). So j_ndc = delta_m12 / m[15],
    // depth-independent after the perspective divide - the "divides by w"
    // confusion was about the matrix element, not the screen effect. In pixels:
    // 1.478 / 8487 * (3072/2) = 0.267 px, the measured quarter pixel.
    //
    // 0 = report zero (the old, wrong behaviour, kept as the control).
    // 1 = auto: per-frame signed value read from the captured matrices via the
    //     third difference (annihilates motion up to acceleration), magnitude
    //     locked to a running mean so one bad sample cannot spike it.
    // 2 = manual: dlss_jitter_x/y below, verbatim.
    int   dlss_jitter_mode = 1;
    // Multiplies the auto estimate. -1 flips the sign - the one convention that
    // can only be settled in the headset (or with the dev DLL's CTRL+ALT+F9
    // jitter-transform cycle, which exists for exactly this).
    float dlss_jitter_scale = 1.0f;
    float dlss_jitter_x = 0.0f;
    float dlss_jitter_y = 0.0f;

    // Our motion vectors are built from the engine's JITTERED matrices, so they
    // carry the frame-to-frame jitter difference. This flag tells DLSS that
    // (MVJittered), and it subtracts the reported offsets itself. The clean
    // alternative - dejittering the matrices before building the reprojection -
    // is a later experiment; declaring the truth is the correct first step.
    bool  dlss_mv_jittered = true;

    // Apex is a reversed-Z engine (near=1, far=0) - Luma sets DepthInverted for
    // Just Cause 3, same family. DLSS assumes near=0 unless told; wrong depth
    // direction corrupts its closest-depth dilation, which is moving-edge
    // quality. Off only to prove what it is worth.
    bool  dlss_depth_inverted = true;

    // Which DLSS model to run. 0 = the DLL's default (transformer preset K on
    // this 310.7 runtime). The rest are the documented transformer presets:
    // 1 = J (slightly less ghosting than K, a little more flicker), 2 = K,
    // 3 = L (documented "less ghosting than J, K", always auto-exposes, most
    // expensive), 4 = M (MSFS's official "ghost killer", L's quality nearer
    // J/K's speed). Changing it live rebuilds both eyes' features.
    int   dlss_preset = 2;

    // *** NVIDIA'S OWN QUALITY LADDER. ***
    //
    // The same five names every DLSS game offers, plus a custom slot so the
    // percentage below can still be dialled by hand:
    //   0 DLAA (100%)  1 Quality (67%)  2 Balanced (58%)
    //   3 Performance (50%)  4 Ultra Performance (33%)  5 Custom
    // These are NVIDIA's published per-axis ratios, not invented ones, so the
    // preset tables NGX keys on the quality value line up with what we ask for.
    int   dlss_quality = 2;

    // *** UPSCALING: what percentage of the OUTPUT each axis is rendered at. ***
    //
    // 100 (or anything outside 40..99) = off, i.e. DLAA - the game renders the
    // size the headset is handed and DLSS only anti-aliases. Below that it does
    // BOTH halves of the deal, in two different files:
    //   - gamesettings.cpp writes render_preset x pct into the game's
    //     settings.json, so the game genuinely draws less. This is where the
    //     frame rate comes from.
    //   - vr.cpp sizes the swapchains and hold textures at render / pct, so the
    //     headset is handed the preset's size again, reconstructed by DLSS.
    // 67 = Quality, 58 = Balanced, 50 = Performance, 33 would be Ultra
    // Performance (below the 40 floor here on purpose - it is not a sensible VR
    // setting). The percentage is per AXIS, so 50 means a quarter of the pixels.
    //
    // Applied at swapchain creation and at settings.json write time, so it takes
    // a game restart. Live editing would mean tearing down the OpenXR swapchains
    // and every hold texture mid-frame, which is a far bigger risk than the
    // convenience is worth.
    int   dlss_upscale_pct = 100;

    // With upscaling on there are two places DLSS could run - the DLAA evaluate
    // inside the resolve, and the reconstruction at the capture - and running
    // both means two temporal accumulations per eye per frame: twice the cost,
    // and a softer picture than either alone. On (the default) the in-resolve
    // DLAA steps aside and our own cheap shader resolve runs instead, which is
    // what writes the motion vectors the upscale needs. Off restores the stacked
    // behaviour for comparison; it is not a setting anyone should want.
    bool  dlss_upscale_solo = true;

    // *** AND THE OTHER HALF OF THAT SAME IDEA. ***
    //
    // dlss_upscale_solo stops the in-resolve DLAA evaluate from running under
    // the upscale, but our OWN shader resolve still averages over time - so
    // there are still two accumulators chained, ours then DLSS's. This makes
    // ours pass the current frame straight through while still reprojecting and
    // still writing the motion vectors the upscale needs, leaving DLSS as the
    // only thing with a history.
    //
    // That is what Luma does on this engine family: replace the temporal pass
    // outright rather than feed one into another. Off by default because the
    // current arrangement is what the owner's good configurations were tuned
    // on, and this is the A/B rather than an assumption.
    bool  taa_single_accumulation = false;

    // Keep the rectangle DLSS reconstructs from fixed for the session, even if
    // the per-eye crop that is SUBMITTED evolves. DLSS's history is keyed to
    // that rectangle; moving it misaligns the whole image against everything
    // accumulated so far, which is invisible standing still and a shimmer the
    // moment anything moves. On by default - a moving reconstruction frame is
    // never what anyone wants - and switchable because it is the fix for a
    // fault the owner isolated by hand ("cut each eye its own view" was the
    // culprit) and it deserves to be A/B'd against that finding.
    bool  dlss_upscale_rect_latch = true;

    // *** SHARPNESS AND SATURATION, ON THE FINISHED EYE. ***
    //
    // NVIDIA removed DLSS's own sharpener - "Sharpening is deprecated" in the
    // Programming Guide - and expect the application to sharpen the OUTPUT
    // instead. taa_sharpen cannot do that job when upscaling is on: it runs
    // inside the resolve, at render resolution, on pixels the reconstruction is
    // about to replace. This one runs last, on the image the headset receives.
    //
    // RCAS rather than a plain unsharp mask, so it cannot ring: the lobe is
    // limited by each pixel's own neighbourhood, which is what stops bright
    // halos forming on trunks and antlers against the sky. 0 = off.
    float post_sharpen = 0.35f;
    // 1.00 = the game's own colour. Below drains toward grey, above deepens.
    // A headset's optics and the compositor's own colour handling can leave the
    // picture flatter than the monitor shows, and this is the one stage that
    // sees the finished image.
    float post_saturation = 1.10f;

    // How much of the ENGINE's per-object motion to mix into the vectors DLSS
    // is given. Our own are camera-only - correct for rocks and houses, silent
    // about wind-blown foliage, which is why leaves smeared under DLSS and the
    // bed did not. The engine's carry object motion but reference the previous
    // RENDER (the other eye under full-rate), so they cannot simply replace
    // ours. 0 = ours alone, 1 = the engine's alone. A blend, because the two
    // disagree for two different reasons at once and this does not pretend to
    // separate them.
    // MEASURED IRRELEVANT, 2026-08-11: 0.00, 0.50 and 1.00 are indistinguishable
    // in the headset, because there is no object motion in the engine's vectors
    // to blend in. Just Cause 3's decompiled resolve says it plainly for this
    // engine - "the game has no motion vectors (not for moving/animated
    // meshes)". Kept at 0 so the engine's wrong temporal reference contributes
    // nothing.
    float dlss_mv_object_blend = 0.0f;

    // *** STAGE 2. Once the pass is ours, actually resolve it. ***
    //
    // A full-screen shader of our own, sampling THIS EYE's history instead of
    // the shared pair. 0 leaves stage 1's straight pass-through, which is the
    // control: sharp, aliased, no smoothing at all.
    bool  taa_resolve = true;
    // How much of the CURRENT frame survives each frame.
    //
    // 0.5, not the 0.1 a long-history TAA would use - because this engine's own
    // resolve is a TWO-FRAME blend. Its decompiled shader (Luma's Just Cause 3
    // dump, same engine) ends in `lerp(cur, hist, w)` where w is clamped at 0.5.
    // The first build used 0.1, i.e. 90% history, which is nine times more
    // accumulation than the game ever had - so every reprojection error piled up
    // into the long streaks the owner photographed instead of decaying away.
    // The base the strength dial scales. 0.46 is the value the owner settled on
    // in the headset, not a figure from a paper.
    float taa_blend = 0.46f;
    // The engine's motion vectors are in UV space, but under full-rate stereo
    // they describe the OTHER eye, not this eye's previous frame. Off is the
    // honest control: no reprojection, so the neighbourhood clamp rejects
    // history wherever the picture moved.
    bool  taa_use_motion_vectors = true;
    // ON, and no longer a guess. The engine's own resolve reads its history at
    // `uv - mv`: `r0.yz = v1.xy - r0.yz;` in SMAA_Temporal_0xF7078237. The first
    // build added it instead of subtracting, which is why the streaks ran the
    // wrong way and compounded.
    bool  taa_mv_invert = true;

    // *** OUR OWN MOTION VECTORS, FROM DEPTH AND THIS EYE'S TWO MATRICES. ***
    //
    // The engine's velocities describe the previous RENDER, which under
    // full-rate stereo is the other eye - so they are wrong for a per-eye
    // history no matter which sign they are given, which is what the headset
    // showed. This reprojects from the depth buffer instead, using the
    // world-to-clip matrix captured at this pass for THIS eye, now and one frame
    // ago. Static geometry only; animals carry no per-object history.
    bool  taa_own_motion_vectors = true;

    // Leave FIRST-PERSON geometry where it is. The weapon and the phone do not
    // move when the camera turns, so reprojecting them as world geometry at
    // their depth drags them around - the shake the owner reported. The engine
    // tags them with stencil bit 6, the same tag the weapon 3D feature uses.
    // OFF, and the reason is measured. The stencil view now builds correctly,
    // and turning it on introduced smearing on ORDINARY OBJECTS while walking -
    // so bit 6 in the depth BUFFER marks more than the viewmodel does. The
    // 57/57-viewmodel result this was based on was about the stencil STATE at
    // draw time, which is not the same thing as what ends up in the buffer.
    bool  taa_exclude_viewmodel = false;
    // Resample the history with Catmull-Rom rather than bilinear. A history
    // filtered bilinearly every frame loses a little sharpness each time and it
    // compounds; nine taps get it back. Off is the way to see how much it is
    // doing.
    bool  taa_sharp_history = true;
    // Correct for the renderer being CAMERA-RELATIVE: its matrices take
    // positions relative to the camera, so the origin itself moves between
    // frames and a reprojection has to add the distance walked. Off is the
    // control - with it off, turning your head stays clean and WALKING trails on
    // everything solid, which is exactly what the owner saw.
    bool  taa_camera_relative = true;
    // Clamp the history into the colours around the pixel. With it, a history
    // fetched from the wrong place is rejected and the pixel aliases; without
    // it, it is accepted whole and smears. Off is the way to SEE the
    // reprojection error rather than have it hidden.
    bool  taa_clamp = true;
    // How far the history may stray from the neighbourhood, in standard
    // deviations. LOWER is tighter: sharper when moving, more shimmer. Higher
    // lets more history through: smoother, and it starts to blur again. 1.0 is
    // the usual figure. This is the knob for "medium blurness when moving".
    float taa_clamp_strength = 1.0f;
    // Sharpen the finished image. Any temporal resolve is an average and
    // averages are soft; this gives the acutance back for four taps, clamped to
    // the local neighbourhood so it cannot ring into halos. Prior-art lesson 12
    // - sharpen the OUTPUT, not the render. 0 = off.
    // 0 by default: the owner's tuning came out here, and the game has its own
    // TAA sharpness slider. Two sharpeners stacked look overdone.
    float taa_sharpen = 0.0f;

    bool  tier1_measure_fov = false;
    // TIER 1 FOV, STAGE 1: widen the engine's own world field of view.
    //
    // OFF by default - this is the first stage that changes what you see. It
    // multiplies the TANGENT of the engine's FOV, so the scope's magnification
    // (which narrows the whole scene, 1.08 -> 2.98) survives scaled rather than
    // being stomped.
    //
    // Judge it against tier1_measure_fov's readout: vfov must become
    // 2*atan(k*tan(45)) within 0.5%. If the log moves and the picture does not,
    // the lever writes something the renderer ignores; if the picture moves and
    // the log does not, the Stage 0 reader is wrong.
    // TIER 1 FOV, STAGE 2: give each eye its OWN rectangle of the rendered
    // frame, and submit the fov that rectangle actually spans.
    //
    // A sub-rectangle of a symmetric perspective image is an off-centre
    // sub-frustum exactly, so this is not an approximation of Tier 1 - it is
    // Tier 1, obtained without a projection matrix to patch.
    //
    // Safe to enable before the FOV lever is trusted: the submitted fov is
    // derived from the CLAMPED bounds, so a render too narrow costs field at
    // that edge but still fuses. Needs tier1_fov to be worth anything, though -
    // without a wider render there is nothing extra to crop from.
    bool  tier1_per_eye_crop = false;
    bool  tier1_fov = true;
    // Stage 1 uses a raw multiplier so the lever can be proved flat, with no
    // headset. Stage 2 replaces it with the tangent that covers both eyes.
    float tier1_fov_k_override = 1.200f;
    // The viewmodel has its OWN field of view (lensOut[1]). Left alone by
    // default: the weapon already has its own FOV control on the WEAPON 3D tab,
    // and moving both at once would make a bad result unreadable.
    bool  tier1_scale_viewmodel_fov = false;
    bool  weapon_3d_mask_follows_glass = true;
    bool  weapon_3d_optic_signature = true;
    bool  weapon_stencil_only = true;
    bool  weapon_match_stencil = true;
    // The old scheme. Left in as a fallback in case a future game build stops
    // tagging, but it should stay off: index counts are not unique across a map
    // and no amount of extra conditions made them safe.
    bool  weapon_match_index = false;
    // *** THE FIVE DEVICES, AND WHY THIS IS OFF. ***
    //
    // Hooking D3D11CreateDevice proved the thing every other theory had missed:
    // the game creates FIVE D3D11 devices, and the mod only ever knew the one
    // that presents. That is the whole of the 0.4%-of-the-frame problem - not
    // shader CRCs, not index counts, not deferred contexts.
    //
    // Finding them is safe; the crash comes AFTER, when the newly reachable
    // devices lead HookAllContextsOf to a SECOND context implementation with
    // genuinely different function addresses (DrawIndexed at ...FC56FB40 against
    // ...FC574B70 for the present-path one) and hooking it takes the game down.
    //
    // So this is off until that second implementation is handled safely. On, the
    // mod sees the whole frame and crashes; off, it behaves exactly as the build
    // before this existed. There is no middle setting yet, and pretending
    // otherwise would just cost another test cycle.
    bool  hook_device_creation = true;
    // *** HOW CONTEXT METHODS ARE HOOKED, AND THE SWITCH BACK. ***
    //
    // On (default): write the VTABLE ENTRY. One aligned pointer store - atomic,
    // no thread suspension, no prologue relocation - so it cannot catch the
    // renderer mid-draw.
    //
    // Off: the old MinHook path, which patches the function body. That is what
    // crashed the game at startup, twice, on the same line - it rewrites
    // DrawIndexed's prologue while the renderer is already calling it, and a
    // thread inside the relocated bytes corrupts. Identical builds died at
    // 2.586 s and survived 142 s, which is what a race looks like.
    //
    // This is the one switch back to exactly the old behaviour.
    bool  hook_context_vtable = false;
    // *** THE WINDOW NUDGE. ***
    //
    // Our context hooks land at first Present, after the engine has already
    // built the contexts it records the frame on - so we see 0.4% of the frame
    // and the weapon flickers instead of responding. Resizing the game window
    // makes the engine rebuild those contexts, and the ones it builds now are
    // ones we catch: coverage goes to 194% (the whole frame, twice, for stereo)
    // and the weapon responds on every frame.
    //
    // Found by the owner resizing the window by hand. This just does it for you,
    // once, a couple of seconds after the hooks are in.
    //
    // Off = no nudge, exactly as before: stable, and 0.4%.
    bool  rebuild_contexts_on_start = false;
    // *** THE WORKER THREAD, AND WHY IT DEFAULTS OFF. ***
    //
    // Drains the pending-device list every few ms from process start, so
    // contexts are hooked at ~1.3 s instead of at first Present (~2.6 s). It
    // demonstrably fires - the crash line changed from "14 newly hooked" to
    // "13 newly hooked, 1 ALREADY KNOWN" - but it also means MinHook is patching
    // code on a worker thread all through startup, and the build that produced a
    // clean vanish did not have it.
    //
    // Shipped without a switch first time round, which broke the rule this
    // project has for exactly this reason: a build that changes behaviour must
    // have one switch restoring the old behaviour exactly. Off = that build.
    bool  hook_contexts_early = false;

    // *** THE GUN IN 3D. ***
    //
    // It is flat because it rides the camera: the engine re-places it relative
    // to the view every frame, so moving the eye moves the gun with it and both
    // eyes land on identical pixels. Zero disparity reads as infinitely far
    // away - hence "welded to your face".
    //
    // This puts the disparity back by shifting clip-space X on the viewmodel's
    // WorldViewProjection, opposite per eye. Needs no camera basis, and it is
    // four floats.
    bool  weapon_3d = true;
    // How much parallax, in clip units. Start small: 0.02 is already visible,
    // 0.1 is a lot. Negative swaps which eye leads, i.e. pushes the gun behind
    // the world instead of in front of it.
    float weapon_3d_amount = 0.1300f;
    // Which VS constant-buffer slot InstanceConsts is bound to. The shader
    // declares it at register b1, so 1 - but this is a slider rather than a
    // constant because RenderDoc reports the DESCRIPTOR INDEX (0) next to the
    // bind number (1), and reading one for the other has already cost a session.
    int   weapon_3d_slot = 1;
    // 0 = column0 += k*column3 (row-vector matrix as reflection displays it)
    // 1 = row0    += k*row3    (the transpose, i.e. HLSL column_major storage)
    // If one shears the gun instead of sliding it sideways, use the other.
    int   weapon_3d_layout = 0;
    // Where WorldViewProjection sits inside the block. -1 = work it out per draw,
    // which is what you want: two block layouts are in use at the same slot and
    // they disagree - InstanceConsts has it at +0x00, LocalConstants (the hands)
    // at +0x40. Set 0 or 0x40 to force one, for testing.
    int   weapon_3d_wvp_offset = -1;
    // *** GLASS: SCOPE AND BINOCULAR LENSES. ***
    //
    // The engine does NOT put its first-person stencil bit on the blended pass
    // that draws glass, so it stays flat while the rest of the gun gains depth.
    //
    // 0 = off, nonzero = on. (The value used to be a draw-count window measured
    // from the last tagged draw; that never worked - the transparency pass runs
    // on its own deferred context, so on the lens's thread no tagged draw ever
    // happens. Now every blended draw is checked against the matrices actually
    // shifted on tagged draws this frame, and only a bit-identical match is
    // shifted - the same object at the same instant. Scenery cannot fake 64
    // bytes of camera-dependent floats.)
    // How many draws AFTER the last first-person (stencil-tagged) draw a blended
    // draw may still be treated as viewmodel glass. 0 = glass off.
    //
    // The viewmodel is drawn at the very end of the frame, so the lens sits a
    // short way behind the tagged draws while house windows and ground decals
    // are hundreds of draws away in the world pass. Too large and those come
    // back; too small and the lens is missed.
    int   weapon_3d_glass = 24;
    // Fine trim on how far the glass moves, relative to the body.
    //
    // The body is moved by editing its matrix; the glass by nudging the
    // viewport. Converting one to the other needs the viewport WIDTH of that
    // draw, and if the glass renders into its own target that width is not the
    // main one - so the computed amount can be right in principle and wrong on
    // screen. 1.0 = the computed amount; adjust until the lens sits centred in
    // its shroud.
    float weapon_3d_glass_scale = 1.00f;
    // Require glass draws to be BLENDED (see-through), or move every viewmodel
    // draw reading the weapon's buffer whatever its blend state.
    //
    // A scope's lens region is typically carved out by a MASK disc drawn with
    // blending OFF. With this on, that disc is the one thing that does not move
    // - and a stationary mask over a moving shroud reads as a crescent being
    // eaten out of the metal ring. Off is wider and risks catching scenery, so
    // it is a switch rather than a change.
    bool  weapon_3d_glass_blended_only = true;
    // Move EVERY draw within weapon_3d_glass draws of a tagged first-person
    // draw, without asking anything about it.
    //
    // A scope is not one object: a lens picture, a metal shroud, and at least
    // one invisible disc that carves out where the picture goes. Every attempt
    // to identify the pieces individually has missed one of them - by blend
    // state, by which buffer it reads, by which address in that buffer - and
    // whichever piece is missed stands still and cuts into the ones that moved.
    //
    // This stops identifying and moves the whole neighbourhood. The window is
    // weapon_3d_glass; keep it SMALL, because everything inside it moves.
    bool  weapon_3d_glass_whole_pass = false;
    // THE OTHER HALF OF THE FIRST-PERSON TAG.
    //
    // Stencil bit 6 is used two ways. The gun READS it (ReadMask 0x40,
    // NOT_EQUAL) - that half has been matched since the tag was found. The
    // scope's lens mask WRITES it (WriteMask 0x40, ALWAYS, REPLACE): one
    // draw, no pixel shader, no render target, depth off, that stamps a
    // circle into the stencil buffer for the gun draws to be clipped
    // against. RenderDoc found it at eid 17030 of the sniper capture - after
    // it, bit 0x40 is set from screen centre to NDC 0.6167 and clear beyond.
    //
    // Leaving it unshifted is the crescent: the shroud moves, the circle it
    // is cut against does not. 0 restores the previous behaviour exactly.
    // A SEPARATE 3D STRENGTH WHILE LOOKING THROUGH AN OPTIC.
    //
    // Aiming down a scope is a different viewing problem from carrying a gun
    // at the hip. The optic sits against the eye, its picture fills most of
    // the view, and a separation tuned for a weapon held at arm's length is
    // usually far too strong there - the two eyes are being asked to fuse an
    // object a few centimetres away.
    //
    // 'Scoped' costs nothing to detect: the draw that stamps the lens circle
    // into stencil bit 6 exists ONLY while an optic is raised. No address to
    // scan for, no game-state pointer to chase, nothing to re-find when the
    // game patches. See g_scopedNow in cbscan.cpp.
    //
    // 0 = one strength everywhere, exactly as before.
    // WHEN THE CONSTANT-BUFFER SHIFT CANNOT REACH A PIECE, MOVE IT AT THE
    // RASTERIZER INSTEAD.
    //
    // The constants path fails sometimes - the snapshot we hold is of a
    // different buffer than the draw actually reads. The log counts it: 11
    // skips per 180 frames, across two gun meshes. Each one is a piece of the
    // weapon standing still for a single frame while the rest moves, which is
    // seen as a flicker rather than as a missed shift.
    //
    // A viewport shift is the same displacement by another route, so a piece
    // we cannot reach through its constants still moves correctly. 0 = leave
    // those draws unshifted, as before.
    // *** DEFAULT OFF, AND THAT IS THE MEASUREMENT, NOT A PREFERENCE. ***
    //
    // Turned on, it flickered MORE things, not fewer. The reason is the
    // company it keeps: a draw reaches this code if ANY rule claimed it -
    // stencil tag, index count, neighbour, shader fingerprint - and the
    // looser of those are wrong sometimes. Until now a wrong claim was
    // harmless whenever the constants path could not reach the draw: it
    // silently did nothing. This fallback removed that accident, and every
    // mistaken claim started moving world geometry.
    //
    // So the skips are NOT all failures. Some are the safety net that keeps
    // loose rules from doing damage. The mask keeps its own fallback because
    // it is identified by pipeline state that is never wrong.
    // THE GLASS COPIES THE BODY'S EYE INSTEAD OF DECIDING FOR ITSELF.
    //
    // The gun body is shifted when the game WRITES its constants; the lens is
    // shifted later, when the game DRAWS. Both used to ask which eye they were
    // rendering, separately. When the answer changed between those two
    // moments, the lens shifted by -k against a body shifted by +k and landed
    // 2k from its shroud - which is why the lenses appear outside the
    // binoculars, why the direction follows the sign of the strength, and why
    // it is intermittent. The owner confirmed the two symptoms are one: the
    // lenses are displaced only on the frames they flicker.
    //
    // Copying the body's answer removes the second question, so there is no
    // window in which the two can disagree. 0 = ask twice, as before.
    // MATCH THE SECOND LENS BY THE MATERIAL OF THE FIRST.
    //
    // Binoculars have two lenses and only one was ever caught: the buffer rule
    // finds it every single frame, while its twin reads a constant buffer that
    // was never recorded, so it is never matched and never moves. The log
    // measured it precisely - 'glass PER FRAME: min 1, max 2' with the caught
    // lens at exactly 1 per frame, and 450 blended draws a window near the gun
    // reading an unrecorded buffer against 4 buffers ever learned.
    //
    // Two lenses of one pair of binoculars are made of the same thing, so the
    // confirmed one identifies its twin. Both the learning and the matching
    // stay inside weapon_3d_glass, and only blended draws qualify: an earlier
    // attempt learned a shader this way and moved 10,527 draws a frame, but it
    // applied the match 512 draws deep. The leash is the difference.
    //
    // 0 = previous behaviour: only the lens whose buffer we recorded moves.
    // MOVE LENS GLASS THE SAME WAY THE GUN BODY IS MOVED.
    //
    // Everything that flickers is a lens - both binocular lenses and a part of
    // the scope - and lens parts were the only things moved at the RASTERIZER
    // while the body moves through its CONSTANTS. Setting the 3D strength to a
    // true zero stops the flicker entirely, so the shift is what causes it, and
    // only one of the two routes is implicated.
    //
    // Why the routes differ for a lens: its pixel shader samples a full-size
    // copy of the scene and magnifies it in SCREEN SPACE. Moving the viewport
    // moves the geometry and its sample position together, so the picture
    // inside the circle comes from somewhere other than where the circle now
    // is. Moving the matrix moves only the geometry - what the body gets, and
    // what a lens needs.
    //
    // The lens's clip matrix is bit-identical to its body's, so the constants
    // path can find it. The rasterizer remains the fallback when it cannot.
    // 0 = rasterizer only, as before.
    // CATCH THE LENS BY THE MESH THE MASK NAMES.
    //
    // The flicker was measured down to one draw: 1584 indices, blended, WITHOUT
    // the first-person tag, present on some frames and missing on others. That
    // is the scope's lens geometry, which shares its mesh with the mask disc
    // (byte-identical post-VS bounds in the capture).
    //
    // It was being recognised only when its constant buffer happened to be one
    // of the handful recorded, and the engine rotates those buffers - so it
    // moved on some frames and stood still on the rest. Widening that table to
    // 128 entries did not help, because the dependence was the problem, not the
    // capacity.
    //
    // The mask is identified from pipeline state and is never in doubt, so it
    // names the mesh. A blended draw of that mesh on the viewmodel is the lens.
    // Nothing in that rule rotates or has to be learned from a lucky upload.
    // 0 = previous behaviour, lens caught only via the buffer table.
    bool  weapon_3d_glass_mask_mesh = true;
    // MOVE LENS AND MASK AT THE RASTERIZER, NOT THROUGH THEIR CONSTANTS.
    //
    // The lens's pixel shader samples a full-size copy of the scene in SCREEN
    // SPACE. Moving it through its constants moves the geometry and leaves the
    // sampling where it was, so the picture inside the circle slides by the
    // shift - and the shift flips sign per eye, so under alternating eyes it
    // alternates every frame. That is exactly the flicker, and it is why a true
    // 0.000 strength is clean while any other value is not.
    //
    // The viewport moves geometry AND sampling together, because both come from
    // the same SV_Position. Its old shortcoming - knowing only the eye shift,
    // so a repositioned scope left its lens behind - is fixed: it now carries
    // the position offsets and the weapon field of view too.
    //
    // 0 = back through the constants.
    bool  weapon_3d_glass_via_rasterizer = true;
    bool  weapon_3d_glass_via_constants = true;
    bool  weapon_3d_glass_follow_body_eye = false;
    // BINOCULARS COUNT AS AN OPTIC, NOT JUST SCOPES.
    //
    // 'Scoped' is detected from the lens-mask stamp, and only scopes stamp one,
    // so binoculars never got the scoped strength. A lens draw is the general
    // signal - glass appears on the viewmodel only while an optic is raised.
    //
    // CAVEAT worth knowing: if a rifle's mounted scope has its glass drawn while
    // the gun is merely held, this reads as 'optic up' during normal play and
    // the whole weapon takes the scoped strength. If the gun loses its depth at
    // the hip, that is this, and turning it off restores it.
    // WHERE THE WEAPON SITS, AND HOW WIDE ITS OWN LENS IS.
    //
    // The engine draws the viewmodel with a projection SEPARATE from the
    // world's, which is why a gun can look right on a monitor and far too big
    // in a headset - the two fields of view were never the same number.
    //
    // weapon_view_scale is that field of view. Below 1 the gun shrinks, as
    // through a wider lens; above 1 it fills more of the view. The offsets move
    // it, in units where 1.0 is half the screen.
    //
    // All four ride the same per-eye matrix rewrite the 3D shift uses, so they
    // cost nothing extra and cannot disagree with it.
    float weapon_view_scale = 0.500f;
    float weapon_view_offset_x = 0.00f;
    float weapon_view_offset_y = 0.00f;
    bool  weapon_3d_scoped_separate = true;
    float weapon_3d_amount_scoped = 0.0000f;
    bool  weapon_match_stencil_write = true;
    bool  weapon_all_draw_types = true;
    // How many draws after the hands (77484) the viewmodel pass may still be
    // running. One round of the pass is nine draws:
    //     77484  5007  162  6087  5958  10272  3252  2484  9594
    // so a dozen covers a round with room to spare, and each round re-anchors on
    // the hands.
    //
    // MEASURED FROM THE ANCHOR, not from the last match. An earlier version
    // reset it on every match, which let a row of fences sharing an index count
    // keep the run alive indefinitely - the whole row picked up the weapon's
    // shift and drew as displaced black copies. Larger values here re-open that
    // door.
    int   weapon_pass_gap = 12;
    // *** HOW FAR PAST A DEPTH CLEAR THE VIEWMODEL IS DRAWN. MEASURED. ***
    //
    // Index counts are not unique across a map - a fence shares 3252 with a real
    // weapon mesh, and 2484 too - so no table of counts can separate them. Where
    // in the frame they are drawn does:
    //
    //     real weapon meshes   clear = 1003 .. 1033
    //     2484 impostor        clear =  710
    //     3252 impostor        clear =  228
    //
    // A MINIMUM is the test, with ~300 draws of margin on either side. The first
    // attempt at this used a maximum of 64, on the assumption that the weapon is
    // drawn right after a clear - the opposite of the truth, and it excluded the
    // entire pass.
    //
    // Lower this if parts of the gun go flat in a busier scene; raise it if
    // scenery starts picking up the shift.
    // How late in a context's recording a draw must be, as a PERCENTAGE of the
    // furthest that recording has ever got from a depth clear. The viewmodel is
    // drawn last, so it sits at ~96% of that mark; the fence that shares its
    // index count sits at 22%. 60 is the middle of a very wide gap.
    //
    // A percentage rather than a fixed draw count on purpose: the band moves
    // with scene density, and a fixed threshold that fits a forest excludes the
    // whole weapon in an open field.
    int   weapon_pass_tail_pct = 0;
    // Absolute floor, applied as well. 0 = rely on the percentage alone.
    int   weapon_min_since_clear = 0;
    int   weapon_max_since_clear = 1000000;

    // *** WHY THE WEAPON DOES NOT FOLLOW THE HEAD. ***
    // Head tracking offsets the VIEW at head_yaw_field. The weapon is oriented
    // from the player's aim, which never sees that offset - so the view turns
    // and the gun stays pointing where the body is aimed. This probe finds the
    // field the aim chain uses, by correlating every field against the yaw WE
    // INJECT: view-chain fields carry it, aim-chain fields do not.
    // The other half of head tracking: +0x4C turns the VIEW, this matrix turns
    // the WEAPON (and the body). Writing only the first is exactly the symptom
    // "the view turns and the gun stays where the body is aimed".
    // Apply the head offset at kAimConsumer (0x0063C0A0) instead of at the
    // camera transform builder. The builder runs AFTER the weapon has been
    // placed, which is why the view followed the head and the gun did not;
    // kAimConsumer runs before both.
    bool  head_early_write  = true;
    // Hold to break the coupling for a moment: the view still follows your head
    // but the weapon stays where it was aimed. A Windows virtual-key code;
    // 0xA4 is Left Alt. 0 disables it.
    int   freelook_key      = 0x12;

    // *** RECENTRING. THE ONE RULE: PRESSING IT MUST NOT MOVE THE PICTURE. ***
    //
    // The mod writes an ABSOLUTE angle (V = B - O), so latching ref = head drops
    // O to zero and throws V to B - the horizon. The fix credits -O to the
    // engine's own look accumulator instead, so B' = B - O and V is unchanged.
    // See docs\RECENTRE_REQUIREMENT.md, which is the authority.
    // The first-run page has been read and dismissed. Until it has, opening the
    // panel lands on it - the three things a new player has to be told (the
    // game's own anti-aliasing, single player only, and that this is unfinished)
    // are not discoverable by exploring, and the two keys worth binding are the
    // two they will reach for in the first minute.
    bool  welcome_seen = false;

    int   recentre_key = 0x13;                  // VK_HOME - today's key, unchanged
    bool  recentre_pad_enable = true;
    // XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB. A CONFIG VALUE and
    // not a constant, for the same reason head_roll_field is: which pad buttons
    // CotW leaves spare has not been measured. 0 switches the chord off.
    int   recentre_pad_chord = 0x00C0;
    int   recentre_pad_hold_ms = 600;
    bool  recentre_pad_swallow = true;
    // THE MASTER SWITCH. 0 restores today's behaviour exactly - the old consume
    // block is kept verbatim as the else branch rather than reconstructed.
    //
    // Turning it off also turns the stick chord off, deliberately: with
    // compensation off a press SNAPS the view to the horizon, and a gesture the
    // player has not learned about yet firing that snap mid-hunt is exactly what
    // "one switch restores the old behaviour" exists to prevent. The old build
    // had no chord at all, so one key really does put all of it back.
    bool  recentre_compensate = true;
    bool  recentre_compensate_invert = false;   // if the press doubles the jump
    // *** THE PITCH CREDIT IS CLAMPED, AND THE REFERENCE ADVANCES ONLY BY WHAT
    // FITTED. *** The engine hard-clamps its own pitch accumulator (apex.h:
    // +/-PI/2 at 0x004A03F9, and a tighter data-driven clamp at 0x00644200), and
    // a credit past that limit is a credit the engine never holds: B would
    // absorb part of the offset while the reference advanced by all of it, and
    // the view would drop by the difference - "it still brings me down, just
    // less". Clamping here leaves the shortfall in the mod's own offset, where
    // it is invisible, and the press stays partial instead of wrong.
    // THE LIMIT IS A GUESS AND THAT IS WHY IT IS A SETTING: the measured camera
    // pitch range is only -63..+64 deg, so if a press still tips the view when
    // looking far up or down, lower this to 63.
    int   recentre_pitch_limit_deg = 90;
    // Re-anchor 6DOF position on a press, so the body ends up back under the
    // head - what the mod did before, and what every prior implementation does.
    // It is also the one part of a press that can still move the picture: this
    // feed drives the game's own Extended View through stream_engine.dll, so
    // recentring while leaning slides the view back by the lean. 0 leaves
    // position alone, at the cost of the body staying where it was.
    bool  recentre_position = true;
    // RECENTRE_REQUIREMENT item 3. Default 0 - see RECENTRE_DESIGN section 4:
    // dropping the smoother publishes a RAW pose for one frame, which is itself
    // a jump of up to ~6 deg mid-turn.
    bool  recentre_reset_smoother = false;

    // HAND THE OFFSET OVER IN SLICES, NOT IN ONE STEP.
    //
    // The credit itself works - measured in the headset: asked 15.99 deg, landed
    // 15.99, with the camera accumulator following to the same value. The
    // picture does not move, because the mod's side changes by the same amount
    // in the same instant.
    //
    // But the GAME does not know that. Its own camera really did move 16 degrees
    // in one frame - the log says so twice per press, once per camera object -
    // and everything the engine builds out of frame-to-frame motion chokes on a
    // discontinuity that size. The owner's report was a brief artefact on the
    // press whose appearance depended on what he was looking at: HUD, house,
    // gun, sky, trees. Content-dependent is the signature of a screen-space
    // effect reacting to the jump, not of anything actually moving.
    //
    // So spread it. Each frame hand over 1/N of what is still outstanding and
    // advance the reference by that same slice. The picture still never moves -
    // both sides step together on every one of those frames - but no single
    // frame contains a jump big enough to break anything downstream.
    //
    // Self-correcting: the offset is re-read live each frame, so moving your
    // head during the ramp is absorbed rather than fought. The last slice is
    // 1/1, so it lands exactly rather than approaching forever.
    //
    // 1 = the whole thing in one frame, which is the behaviour measured above.
    int   recentre_ramp_frames = 31;

    // WAIT UNTIL THE HEAD IS STILL BEFORE HANDING ANYTHING OVER.
    //
    // Luke Ross's R.E.A.L. mod does this, and the reason is worth writing down:
    // it "will wait for a fraction of a second to allow your head position to
    // stabilize, and then it will instruct the VR runtime to recenter". Firing
    // the instant the key goes down means firing at the one moment the offset is
    // largest and least meaningful - mid-movement, with the head still swinging
    // toward wherever it was going. The hand-over is then bigger than it needed
    // to be, and the artefact scales with exactly that.
    //
    // Measured stillness, not a fixed delay: hold under the speed below for the
    // time below and it goes. That way a player who is already still waits the
    // minimum, and one still turning waits until they have arrived.
    //
    // The timeout matters as much as the threshold. Without it, a player on a
    // moving vehicle - or with a twitchy tracker - would press recentre and get
    // nothing at all, which reads as a broken key rather than a patient one.
    //
    // 0 = fire immediately, which is the behaviour before this existed.
    int   recentre_settle_ms      = 150;    // how long it must stay still
    float recentre_settle_deg_s   = 20.0f;  // what counts as still
    int   recentre_settle_max_ms  = 1200;   // give up waiting and fire anyway

    // Watch for the game resetting the view itself (crouching was reported to).
    bool  head_reset_watch  = true;
    // WHEN the early offset is taken back out of the engine's field.
    //   0 = never          weapon follows; crouch drifts (the first working build)
    //   1 = after the aim consumer   crouch clean, but the weapon stops following
    //   2 = at the camera transform builder, after the weapon and view read it
    // Default 0 because it is the only one PROVEN to give a following weapon;
    // the crouch drift only shows up when crouching.
    int   head_undo_mode    = 0;
    // Detect the engine ADOPTING our offset (crouching does this) and add
    // it straight back, so the view cannot ratchet. The signature is exact:
    // the value falls by our own offset between two passes of the same eye,
    // which no human turn can imitate.
    // Read the engine's own announcement that it is about to copy the aim
    // into persistent state ([rsi+0xBC]==0) and apply no offset that tick.
    // This is a stated fact rather than a guess, unlike every detector.
    // Suspend the head offset while the stance is changing. Crouch, stand,
    // prone and jump all move the camera's height fast; walking a slope is an
    // order of magnitude slower, which is what separates them.
    // ROTATE THE VIEW MATRIX instead of writing the engine's aim angles.
    // Every angle-write variant was measured to drift on crouch, jump, prone
    // and even slopes; a value we never write cannot be read back. When this
    // is on, no angle is written anywhere.
    // THE FIX. When the engine saves the aim for a scripted camera episode
    // (crouch/stand/prone/jump/slope), hand it its OWN value instead of ours,
    // so nothing of ours can be restored afterwards as the player's aim.
    bool  head_clean_saved_aim = true;
    bool  head_matrix_rotate = false;
    bool  head_matrix_yaw_invert = true;
    bool  head_matrix_pitch_invert = false;
    int   head_matrix_pitch_axis_row = 2;   // 2 or 0 - which row is 'right'
    bool  head_suspend_on_stance = false;
    float stance_rate_mps  = 0.60f;   // vertical speed that counts as a stance change
    int   stance_hold_ms   = 700;     // suspend this long after the last fast frame
    bool  head_gate_on_latch = false;
    bool  head_leak_correct = false;
    // Fallback only: correct AFTER the engine has taken the offset,
    // rather than never leaving one to take. Every frame fights the
    // player's turning; once per event lets the drift back. Off.
    bool  head_leak_after_the_fact = false;

    bool  head_rotate_lever = false;
    // One axis at a time. Testing both together produced a report that mixed
    // yaw and pitch effects and could not be read cleanly.
    bool  head_rotate_lever_yaw   = true;
    bool  head_rotate_lever_pitch = true;
    // A sign per axis. The angles come from the ANGLE-FIELD conventions
    // (+0x4C stores a negated heading), which say nothing about the handedness
    // a geometric rotation needs, so the two cannot share one flag.
    bool  head_rotate_lever_invert = false;         // yaw
    bool  head_rotate_lever_pitch_invert = false;   // pitch

    bool  weapon_rot_probe  = false;
    // A candidate to test: give it the same offset the view gets and watch
    // whether the weapon comes with you. -1 = write nothing.
    int   weapon_rot_field  = -1;
    bool  weapon_rot_invert = false;   // the sign convention may not match

    // The three PIXEL shaders that draw the held weapon, found with ReShade's
    // Shader Toggler and identified by the same CRC-32 it uses. The capture
    // confirmed them: they draw the hands, the rifle and the scope in the
    // forward pass, and appear nowhere else in the frame.
    int   weapon_ps_crc0    = (int)0xFBECF3C0;
    int   weapon_ps_crc1    = (int)0xF616CE07;
    int   weapon_ps_crc2    = (int)0x7A86206F;
    int   weapon_cb_test    = 0;
    float weapon_pos_x      = 0.500f;
    float weapon_pos_y      = 0.040f;
    float weapon_pos_z      = 0.500f;

    // --- cheats, from the community tables ---------------------------------
    // Set the hour and it is applied once, then this returns to -1 so time can
    // carry on. A permanent write would be a freeze under another name.
    float cheat_set_hour       = -1.0f;   // 0..24, -1 = do nothing
    bool  cheat_freeze_time    = false;   // holds whatever hour it is switched on at
    // How fast the day runs. 1 = the game's own speed, 0.25 = a quarter speed
    // so dawn lasts, 5 = watch the sun cross the sky.
    //
    // Done by MEASURING the engine's own hourly step each frame and re-applying
    // it scaled, not by writing the multiplier field a cheat table claims lives
    // at +0xF4 - that offset comes from a different build and has never been
    // checked against ours.
    float cheat_time_scale = 1.0f;
    float cheat_weather_speed  = -1.0f;   // -1 = leave the game's own value alone

    // --- the HUD component probe (read-only, ini only, no panel row) --------
    //
    // The HUD sits at the EDGES of the screen, which is the worst place for it
    // in a headset, and moving each component inward needs each component
    // identified first. Shader identity cannot be the handle: several
    // components share one shader, and the hunted list of HUD shaders is
    // partial. So the handle has to be WHERE THE QUAD LANDS ON SCREEN, and this
    // prints exactly that for every draw that passes a coarse "could be HUD"
    // filter - depth test off, blending on, and not already claimed by the
    // viewmodel or optic-glass rules.
    //
    // Per accepted draw: frame, draw ordinal, eye, context, vertex/index/
    // instance counts, topology, the VIEWPORT, the SCISSOR rect, the render
    // target's size and format, the first two pixel-shader textures DESCRIBED
    // (not just their pointers), the VS and PS crc32 - the same number Shader
    // Toggler shows - and the first sixteen floats of the small constant buffer
    // the draw actually reads, taken through the range-aware bind so the block
    // is the one this draw uses rather than the head of the pool.
    //
    // READ-ONLY BY CONSTRUCTION: it only ever calls Get*, releases every object
    // it queries, and never touches the draw. At 0 it costs one global read per
    // draw and nothing else.
    bool  hud_probe = false;
    // A probe that logs forever fills the disk and tanks the framerate, so this
    // one stops by itself after N frames and has to be re-armed by turning
    // hud_probe off and on again.
    //
    // Counted in PRESENTS. One Present spans BOTH eye passes under full-rate
    // stereo, so three frames is up to six eye passes - which is what the eye
    // column in each line is for.
    int   hud_probe_frames = 3;

    // Hide ONE group of HUD draws so the owner can say what disappeared.
    // Grouped by BOUND TEXTURE, which the probe measured to be the handle that
    // separates components - the shader does not: one hunted pixel shader
    // carries 216 of the 408 on-screen draws by itself.
    //   0  = off
    //   1..N = hide the Nth texture, in the order they first appear in a frame
    //   -1 = hide every accepted draw. The sanity check: if this does not blank
    //        the HUD, the filter is wrong and every index below it is noise.
    int   hud_isolate = 0;
    // Sweep the POST-PROCESS family instead of the HUD one: same depth-less
    // state, but writing rather than blending. The lens-dirt / flare layer is
    // there - hud_isolate = -1 hid every blended overlay and the dirt survived.
    bool  hud_isolate_post = false;

    // MOVE THE HUD IN FROM THE EDGES.
    //
    // The outer edge of a headset's view is the worst place for small text, and
    // this game puts the compass, health, heartbeat and phone there.
    //
    // Shift, in pixels of the render. Moves without resizing - the right control
    // when the whole HUD sits in one corner, which it does here.
    int   hud_move_x = 0;    // negative = left
    int   hud_move_y = 0;    // negative = up
    // Shrink about the centre. Pulls everything inward from wherever it is, and
    // makes it smaller by the same factor; the two cannot be separated, which is
    // why the shift above exists as well. 1.0 = untouched.
    float hud_scale  = 1.0f;

    // Remove the bloom / glare overlay the game composites over the scene - the
    // haze that reads as a dirty lens or sun scatter. Matched by its HDR float
    // format rather than by size, so it survives a resolution change.
    bool  hud_hide_glare = false;

    // Remove only the COLOURED FRINGING around the sun, leaving bloom
    // everywhere else. It is the first upsample rung of the bloom pyramid;
    // matched by direction and scale rather than by size, because the
    // downsample rung at the same size is byte-identical to it.
    bool  hud_hide_sunflare = false;

    // --- in-headset panel --------------------------------------------------
    bool  sound_enabled   = true;
    float sound_volume    = 0.5f;   // 0..1
    float panel_distance  = 1.30f;  // metres in front of the player
    float panel_size      = 1.00f;  // width in metres
    // Show the settings panel on the MONITOR as well as in the headset, so a
    // setting can be tried without putting anything on. One blit, taken after
    // the headset images are already finished with the frame.
    bool  panel_on_monitor = true;

private:
    std::wstring Path() const;

public:
    // --- profiles and reset -----------------------------------------------
    //
    // A profile is just this same ini under another name, so there is one file
    // format and one parser rather than two that can disagree. Saving means
    // "write the settings, then copy the file"; loading is the reverse. Nothing
    // here can invent a key that Load() does not already understand.
    bool SaveProfile(int slot) const;   // 1..kProfileSlots
    bool LoadProfile(int slot);
    bool ProfileExists(int slot) const;
    // Every value back to the number it is declared with in this header. Done by
    // assigning a freshly constructed Config, so it can never drift out of step
    // with the defaults - there is no second list to maintain.
    void ResetToDefaults();

    // Which slot the panel's save/load act on.
    int   profile_slot = 1;
    // Reads the file back after writing it and warns if fewer keys came out than
    // Load() accepts - the "setting that does nothing" bug, made loud.
    void VerifyRoundTrip() const;
};

Config& Cfg();

// *** ONE ANSWER TO "WHAT PERCENTAGE ARE WE RENDERING AT". ***
//
// Two settings can say it - the quality ladder and the custom percentage - and
// three files need the answer (the game's resolution writer, the swapchain
// sizer, the panel's live readout). Anywhere that reads dlss_upscale_pct
// directly is a place the ladder would be silently ignored, so nowhere does.
// Returns 100 when upscaling is off or DLSS is not enabled at all.
int DlssRenderPct();

}  // namespace cotwvr
