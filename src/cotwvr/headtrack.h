#pragma once

namespace cotwvr {

// Head tracking: turn your head, the game world turns.
//
// THE OPEN QUESTION this answers (playbook section 2, the decision everything
// depends on): is the engine's camera ORIENTATION a writable input, or derived
// output?
//   * writable  -> Model A. The head IS the game camera. Weapon, body, aim and
//                  audio all follow because the character genuinely looks there,
//                  and roll comes free. By far the better result.
//   * derived   -> Model B. The head moves the render view only; aiming stays on
//                  the mouse/stick and a floating reticle shows where shots
//                  actually go.
// Position was already proven writable (that is what gives stereo), but
// orientation is a separate question and is tested, not assumed.
//
// NOTE: the two mechanisms this header used to declare - ApplyHeadRotation
// (rewrite the builder's basis rows) and RotateForwardVector (rotate its
// direction argument) - have been REMOVED. The question above is answered:
// orientation IS writable, but through the engine's own yaw/pitch fields, not
// through the matrix it derives from them.

// The head orientation relative to the recentre reference, as Euler angles in
// DEGREES (pitch, yaw, roll) - the units Tobii reports and the game's Extended
// View consumes. Exported from the DLL for the stream_engine proxy.
bool HeadEulerDegrees(float* pitchYawRoll);

// Is the free-look key held right now? While it is, the head offset is applied
// AFTER the weapon is placed instead of before, so the view moves and the gun
// stays where it was aimed. Polled once a frame and cached, so this is safe to
// call from a hot hook deep in the engine.
bool FreeLookHeld();

// *** ONE POSE PER FRAME (head_latch_per_frame). ***
// The offset is written at several points in a frame and each one used to read
// the pose fresh; the pose is republished from another thread, so two reads in
// one frame can differ and the difference lands in the engine's camera as a
// tiny per-frame movement - several pixels on close geometry, nothing at
// distance. Latching stamps the frame with one pose and serves it to every
// consumer. Prior art: VR_PRIOR_ART_LESSONS #15.
void HeadLatchFrame();
long HeadLatchServedLastSecond();

// The yaw, in radians, the view should be turned by: the axis test's swept angle
// while that is on, otherwise the real head yaw.
//
// This drives the engine's OWN yaw field (found by correlation at +0x4C of the
// lever's object, r = -1.000 against yaw in radians) rather than rewriting the
// matrix the engine derives from it. Rewriting derived output was the mistake
// that made every earlier head-tracking attempt look like it did nothing.
bool HeadYawRadians(float* yaw);

// The pitch, in radians, on the same terms. The candidate field is +0x50 - the
// float immediately after the yaw at +0x4C, ranging -1.099..+1.118 rad (about
// +/-63 deg, a first-person pitch clamp). Adjacency to a CONFIRMED field plus a
// plausible clamp is the reason it is worth testing first; it is still a guess
// until the swept write moves the view.
bool HeadPitchRadians(float* pitch);

// Head roll - how far you have tipped your head sideways, ear towards a
// shoulder, relative to where you last recentred. Already computed for every
// pose; this simply hands it out, the same way pitch is handed out.
bool HeadRollRadians(float* roll);

// Head position relative to the recentre reference, in millimetres, in Tobii's
// convention. Extended View is largely position-driven.
bool HeadPositionMM(float* xyz);

// The same movement WITHOUT the face-tracker treatment: real metres, real
// distance, OpenXR local axes (+x right, +y up, -z forward), relative to the
// last recentre. This is what 6DoF moves the camera by; HeadPositionMM's
// quarter-scale desk-sized box would turn leaning into a twitch.
bool HeadPositionMetres(float* xyz);

// The head's ABSOLUTE yaw in the room - NOT measured from the recentre, unlike
// every other angle here, and no invert flag or axis test either.
//
// The head's POSITION is reported in the room's axes and those do not move when
// the player recentres, so mapping a room offset into the game world needs the
// whole angle between the room's zero and the world's. Using the
// recentre-relative yaw instead leaves the mapping wrong by whatever direction
// the player was facing when they last recentred - which is invisible if that
// was square to the room and different every time otherwise.
bool HeadPhysicalYaw(float* yaw);

// Rotate a ROOM-space vector into the head's own horizontal frame: +x to the
// player's right, +y up, +z behind them.
//
// Done with the head matrix's own basis vectors rather than with an angle,
// because the angle above is not a right-handed rotation about +Y - undoing it
// with the textbook matrix doubles it instead of cancelling it, and a doubled
// rotation is right in exactly two places 180 degrees apart. Anything that has
// to express a tracked POSITION in the player's frame should call this rather
// than roll its own trigonometry.
bool HeadRoomToBody(const float* room, float* out);

// The same projection onto the facing the player had AT THE LAST RECENTRE,
// rather than the one they have now. Which one a caller wants depends on the
// frame it will apply the result in: a frame that turns with the HEAD wants
// HeadRoomToBody, a frame that turns with the BODY - the character, the stick -
// wants this. Getting it the wrong way round is invisible until the player
// turns their head without touching the stick.
bool HeadRoomToRecentre(const float* room, float* out);

// The same projection by an EXPLICIT angle, so the caller can choose which
// facing to cancel rather than the choice being wired in. All four candidates
// go through this one expression, so switching between them cannot introduce a
// sign error - which is the other half of what six headset sessions were spent
// on.
bool HeadRoomByYaw(const float* room, float* out, float psi);

// The head's absolute room yaw and the recentre reference, for choosing between
// those candidates and for the log that will settle which is right.
void HeadFrameAngles(float* absYaw, float* refYaw);

// Take the current head orientation as "straight ahead".
void RecentreHead();

// *** THE RECENTRE HANDSHAKE, split across the sites that own the picture. ***
//
// The request is raised in HeadTrackTick and PERFORMED wherever the head offset
// last entered the picture this frame - the aim-consumer hook when the weapon is
// coupled, WriteYawField when it is not (decoupled play, free-look held, undo
// mode 1). Only those sites can reach the engine's own look accumulator with the
// offset still standing, and crediting -O to it is the only way the press can
// leave the picture where it is (docs\RECENTRE_REQUIREMENT.md).
//
// An earlier build put the credit behind CoupledNow(), which made recentring do
// nothing AT ALL - forever, silently - for anyone who turned "Weapon follows
// your head" off. Both sites, or the feature has a hole the size of a panel row.

// True once, on the tick the request is taken. Interlocked: there are TWO camera
// objects alternating, one per eye pass, and a credit applied on both would move
// the view by one whole offset in the wrong direction.
bool TakeRecentreRequest();

// How much of what is still outstanding to hand the engine THIS frame.
// 1/N on the first slice, 1/(N-1) next, finally 1/1 so it lands exactly.
// Re-arms the request itself while slices remain. Call once per credit,
// immediately after TakeRecentreRequest() has returned true.
float RecentreRampFraction();
int RecentreRampRemaining();
// Hand it back: this tick could not credit the offset (gated, or free-look).
void PutBackRecentreRequest();
// The raw head offset the readers are using RIGHT NOW, before the invert flags -
// taken from the smoothed pose, i.e. the same number that reached the camera.
bool CurrentRawOffset(float* yaw, float* pitch);
// Advance the reference by exactly what was credited to the engine, and
// re-anchor position. Never re-latches from a fresh pose: the credit and the
// reference must come from the SAME sample or the difference is a jump.
//
// SCHEDULED, not applied: the advance lands at the top of the next
// HeadTrackTick. Doing it inside the credit splits the eyes - the aim consumer
// is entered twice per frame with two alternating camera objects, so an offset
// that drops to zero inside pass 1 leaves pass 2 nothing to apply, and the two
// eyes render one whole offset apart for that frame.
void CommitRecentre(float rawYaw, float rawPitch);
// Is a scheduled advance still waiting? A second credit before the first has
// settled would double the engine's side of the sum and move the picture.
bool RecentreCommitPending();
// The apply sites stamp this whenever an offset actually reaches the picture, so
// "nothing is being applied" is an OBSERVATION and not an inference from the
// config flags. The gates are dynamic; getting this backwards is the one way
// this design produces the exact failure it exists to prevent.
//
// NOT every path stamps it. head_matrix_rotate and head_view_basis put the
// offset into the view matrix downstream of every field this mod can credit, so
// they are handled by name in HeadTrackTick and told the truth in the log,
// rather than being inferred from a timestamp they never write.
void NoteHeadOffsetApplied();
// Has an offset reached the picture within the last ~250 ms?
bool HeadOffsetIsLive();
// Has one EVER? Distinguishes "the startup auto-recentre, nothing applied yet"
// from "applies have stalled while a full offset is still sitting in the field".
// A plain latch is continuous in the first case and a jump in the second.
bool HeadOffsetEverApplied();
// True for a few seconds around a press. The accumulator watch logs on this as
// well as on head_reset_watch, so the one measurement the design rests on does
// not depend on the owner hand-editing an ini key nobody told him about.
bool RecentreWatchActive();

// Called once per frame from Present: reads the head pose out of OpenXR and
// caches it for the game thread, and handles the recentre hotkey.
void HeadTrackTick();

}  // namespace cotwvr
