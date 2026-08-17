# The optics flicker — what it actually was, and why it took two days

Owner-verified fixed 2026-08-08. Scope, binoculars, weapon and hands all steady.

---

## The bug, in one sentence

**A scope's glass and the invisible stencil circle it is clipped against are the
same disc, issued twice — and the mod sent the two copies down two different
transforms, one of which could silently fail.**

Measured: the two draws share index and vertex buffers, and their post-VS clip
positions are bit-identical to six decimals (`15960 ≡ 16691` at z/w
0.036561–0.036745; `15963 ≡ 16700` at 0.219812–0.223026). One object. Two draws.

The mod routed them apart:

| copy | route | can it fail? |
|---|---|---|
| glass (`h.glass`) | constant buffer — the weapon's own matrix | **yes** |
| mask (`h.mask`) | rasterizer — a viewport shift | **no** |

That asymmetry is the whole bug. On a frame where the constants could not be
resolved, **the glass did not move and the mask still did**. Same disc, two
positions, alternating with the failure rate — irregular, a few seconds apart,
appearing on whichever piece happened to lose out.

**Binoculars stamp no mask at all.** They cannot split this way, which is exactly
why they came out clean the moment the glass was identified properly, while the
scope kept flickering. That asymmetry was the strongest clue in the whole hunt
and it sat unexplained for hours.

The fix is one condition (`weapon_3d_mask_follows_glass`): the mask takes the
same route as the glass.

---

## The pattern underneath — this bug was here four times

Every visual artefact in this project has had the same shape:

> **One object reached by two mechanisms, chosen by something unrelated to the
> object.**

1. **The crescent** — the shroud shifted through its matrix, the mask disc not
   moved at all.
2. **The detached lenses** — body applied the eye shift *then* the FOV scale;
   the viewport applied the scale *then* the shift. Off by `(s−1)·k`, which is
   zero at scale 1.00 and grows from there.
3. **The lens thrown across the sky** — the glass took the body's shift from a
   global "last shift written", which crossed deferred contexts and sometimes
   held the *other eye's* value.
4. **This one** — glass on constants, mask on the rasterizer, one able to fail.

**The rule that would have caught all four:** *if two draws are the same object,
they must take the same code path — not equivalent paths, the same one.* Any
place where a fallback exists for one and not the other is a latent artefact.

---

## Why it took two days

Roughly twenty test cycles. Almost all were spent on **wrong theories that I
believed**, not on the fix. In order:

### 1. A measurement that was silently false

The overlay printed 3D strength as `%.2f` on a slider that steps in `0.005`. Ten
distinct values — including two non-zero ones — all displayed `0.00`.

I used "set the strength to 0, is it still flickering?" as the load-bearing test
that ruled out the **entire weapon-3D subsystem**, and then ruled out AER, TAA,
and two more mechanisms on top of that conclusion. The slider was never at zero.
**Four eliminations, all void.**

The owner found it: *"at 0.00 there is flickering, but at −0.00 there is none."*
Signed zero on a rounded display.

> **Lesson: before a measurement decides anything, verify the instrument can
> represent the value it is reporting.** A control that cannot express "off"
> cannot be used to test "off".

### 2. Counters that measured one exit of seven

The log reported `~20 skipped (our snapshot is of a different buffer)`. I built
three consecutive theories on that number being *the* failure rate, including
raising a table from 64 → 256 → 512 → 2048 slots (three times, no change).

When I finally routed *every* constants failure to the rasterizer, the whole
weapon flickered — far more than 20 draws could explain. `BindShiftedWeaponCB`
has **seven** failure exits; the log counted one.

> **Lesson: a counter on one branch is not a failure rate.** Count every exit, or
> quote the one you counted and nothing more.

### 3. Instrumentation that changed behaviour

Two separate diagnostic builds were themselves broken:

- A counter spliced *into* an `if` condition rather than its body — it compiled,
  and reported `9720 Map failures` that exactly equalled the `9720 successes`.
- An "expect exactly 2 optic draws per frame" check that tripped on every scoped
  frame, because **one Present spans both eye passes** and every counter doubles.
  It reported a rule that was working perfectly as broken.

> **Lesson: read the instrument's own output for impossible values before
> believing it.** A failure count equal to the success count is not a finding.

### 4. Widening coverage, four times, when the answer was to narrow

Every attempt to move *more* draws made things worse:

- stencil-tag fallback → whole weapon flickered
- all constants failures → rasterizer → same
- eliminating snapshot misses entirely (they reached **zero**) → weapon and hands
  began flickering
- dropping the blend test from the mesh rules → no improvement

Four results pointing the same way before I tried the opposite. Narrowing to the
engine's own stencil tag fixed the weapon and hands immediately.

> **Lesson: when a class of fix makes things worse twice, the sign is wrong.
> Stop adding and start removing.**

### 5. Reading a mistake as a discovery

The glass-resource table held 16 entries and the log read `16 distinct block
site(s)`. That is a table at capacity — the code returned early when full and
could never learn another buffer. I read the number as an observation, not a
limit. (Real bug, fixed; not the flicker.)

> **Lesson: when a reported count equals a capacity, it is a limit until proven
> otherwise.**

### 6. Theorising about a resource instead of describing it

Three theories about why the lens's constant buffer was unreachable: table
capacity, `UpdateSubresource`, an unhooked context. All wrong.

One line printing the buffer's own description settled it in a single run:

```
THE LENS BUFFER: 128 bytes, usage DYNAMIC, cpuAccess WRITE, bind CONSTANT_BUFFER
```

It was **DYNAMIC** — written through `Map`/`Unmap`, seen every frame — and
dropped because both snapshot paths required `>= 256` bytes, a threshold written
for a different block.

> **Lesson: when a resource "cannot be found", describe the resource before
> theorising about the lookup.**

---

## The tools that actually worked, in order of value

1. **RenderDoc captures analysed by multi-agent workflows.** Every real
   breakthrough came from a frame capture, never from reasoning:
   - the mask disc (`eid 17030`, stencil-write role) — from a capture
   - the lens buffer being 128 bytes — from one descriptor line
   - the optic's exact fingerprint (stencil ref `0x12`, two RTVs, motion vectors
     second) across six captures at four resolutions
   - **the same-disc-two-routes cause** — from post-VS positions being
     bit-identical

   The workflows also **killed my hypotheses before I coded them**: the last one
   was launched to confirm "the mask matches the optic fingerprint" and proved it
   false on eight fields. That saved a build and a wrong report.

2. **The owner's own experiments.** Repeatedly sharper than my instrumentation:
   - *"0.00 flickers, −0.00 doesn't"* → the fake-zero slider
   - *"strength 0 and FOV 1.00 is clean; move the FOV and it flickers"* → proved
     it was **any** transform, not the eye shift, which killed the entire
     stereo/AER line of investigation
   - *free-look revealing the blue square* → localised the flicker to one quad
   - *"binoculars fixed, scope not"* → the single most valuable clue, because it
     isolated the mask as the only difference

3. **ReShade Shader Toggler** (owner) — identified the lens material by hash when
   the mod's own fingerprinting could not.

4. **Diffing against backups.** The rotation regression took one `diff` to find
   (`head_reseed_correct_scale` had flipped `−1 → +1`). I should have diffed the
   moment "it returned" was reported instead of theorising.

### What I should have reached for sooner

- **A capture, immediately**, whenever a visual artefact resisted two attempts.
  The rule for this project: *two failed hypotheses ⇒ capture, don't theorise.*
- **A baseline check.** My own standing note says run vanilla first. Never did.
- **Diff-against-last-good** on any "it came back" report.
- **Verify the instrument before the experiment** — the slider and the counters
  cost more time than every real bug combined.

---

## What is still open

- **The blue square's identity.** Three candidates were tested and killed (the
  head-locked magnified window; the reticle box — `UsePostProcessReticle` is
  `0.000000` in all twelve draws, dead code; lens element A — its footprint
  tracks the weapon, not the head). No fourth was invented.
- **Whether `BindShiftedWeaponCB` always succeeds for the mask.** The new counter
  `g_maskNoConstants` watches it. **If it is ever non-zero, restore the fallback
  for glass and mask together — never one alone**, or the split reopens by
  another door.
- The scope's magnified picture is drawn from the camera, not the weapon, so it
  slides under free-look. Cosmetic, unfixed, and not the flicker.
