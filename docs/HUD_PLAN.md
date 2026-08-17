# Moving the game's HUD — what is known, and the goal

Started 2026-08-09. Nothing implemented yet; this is the handover.

## The goal

The game's HUD sits at the edges of the screen, which in a headset is the worst
place for it — the outer edge of the view is exactly where small text is hardest
to read.

**The requirement is PER-COMPONENT, not one global scale.** The owner wants each
HUD element found and controlled separately if that is possible: compass, ammo,
mission text, prompts, and so on. A single "HUD scale" slider is the fallback,
not the target. This matters for how the work is structured — see below.

## The mechanism already exists

`cbscan.cpp:3005` already saves the viewports, scales them about their centre,
draws, and restores, in order to move the scope lens. Shrinking a viewport about
its centre pulls everything drawn through it inward, which IS "move the HUD in
from the edges". No new rendering technique is needed; it has to be pointed at
different draws.

## The unknown is which draws are the HUD

Nothing in the mod identifies them today. `vr.cpp:932` only notes in passing that
the game's HUD lives at the edges.

Do not guess a state fingerprint. Guessing state produced the crescent bug and
cost two days on the flicker; the postmortem's rule is to describe the resource
before theorising about it.

### Route A — the ShaderToggler hashes (preferred)

`C:\Program Files (x86)\Steam\steamapps\common\theHunterCotW\ShaderToggler.ini` holds one
group, `Default`, toggle key 335544320, `IsActiveAtStartup=False`:

    Vertex (4): 1787392644  461146230  1145290386  2910689842
    Pixel  (6): 422461458   2727569164 2611165027  893034202  304038693  198363578

**CONFIRMED 2026-08-09: this is a fresh HUD hunt by the owner, not the old scope
glass hunt.** The same tool and file were used earlier in this project for the
glass, and the group is only called "Default", so the file alone is ambiguous -
the owner settled it. These ten shaders draw the HUD.

To use the hashes the mod must hook `CreateVertexShader` / `CreatePixelShader`,
hash each blob the way ReShade does, and tag the matches.

**The shader CANNOT be the per-component handle.** Owner, 2026-08-09: more than
one HUD component shares the same shader, and the list is partial — not every
HUD shader has been hunted. So bisecting the list into per-element groups, which
was the first plan, cannot work: the finest separation a shader gives is
"several elements at once", and a partial list misses elements entirely.

This is worth knowing BEFORE building anything on shader identity. Treat the ten
hashes as what they are — a confirmed sample of HUD shaders, useful for
VALIDATING a fingerprint, not as the fingerprint itself.

### Identify by WHERE IT IS, not by what drew it

A HUD is laid out by position: compass top-centre, ammo bottom-right, prompts
bottom-centre. Position is what distinguishes the components, and position is
also the thing to be changed. So the discriminator and the lever are the same
quantity, which is a much better place to stand than shader identity.

Two candidate handles, in order of preference:

1. **The constant buffer holding the element's screen rectangle.** The mod
   already owns this machinery — `Hook_Map`/`Hook_Unmap` snapshot and substitute
   constant buffers, and `kMinSnapshotBytes` is 64 (it was 256 and dropped the
   scope's 128-byte lens buffer, which is exactly the bug that taught us to look
   at small buffers). If each HUD quad's rectangle lives in a CB, reading it
   identifies the element by where it sits and rewriting it moves that element
   alone. No viewport work, no shader matching.
2. **The bound shader-resource view.** UI elements usually sample their own
   atlas or texture, so the SRV often separates elements that share a shader.

The coarse "is this the HUD at all" filter should be STATE, not the shader list:
depth off, blend on, drawn late. Then use the ten known hashes to check that
filter catches them — the sample validates the net rather than being the net.

### First diagnostic to run

For every draw passing the coarse filter, log: shader hashes, vertex/index
count, bound SRV, viewport, and any small constant buffer's contents. Play for a
minute with the HUD up. The elements should fall out as clusters at distinct
screen positions, and that listing is the component map.

Risk to check early: a shader may draw a HUD element AND something in the world.
Scaling its viewport would move both. Same "one object, two paths" shape as the
scope glass.

### Route B — the conditional jump (from the cheat tables)

`cheat engine/theHunterCotW_v.1.22 (free cam, graphic settings).CT` has a
`noHUD` script: a one-byte patch, `75` -> `74` (JNE -> JE), on the AOB

    75 0E 49 83 C0 04 4D 3B C1

So the engine gates HUD visibility on a single conditional jump inside a loop
that walks a list of elements. That list is interesting — it is probably the
per-component handle we want.

**The pattern does NOT exist in the current build** (0 matches; the table is for
v1.22). The `add r8,4 / cmp r8,r9` idiom survives at six places in `.text`:

    0x68BB31  0x88F0F9  0x8AFD09  0x10C2CF2  0x10C3872  0x1807C52

None has a conditional jump immediately before it, so none is a structural match.
Finding the equivalent site in this build is real work, not a rescan.

(RVAs above are from a corrected PE section parse. A first attempt read
`PointerToRelocations` as the section's file base and put these in `.rdata`, one
of them past the end of the image.)

### Route C — hide it entirely, no work at all

The game's own `settings.json` has `GameDisplayHUD`, currently `1`. If hiding
would do, that is a launcher row and nothing more. It gives no per-component
control, so it does not meet the goal — but it costs nothing.

## Order of work

1. Resolve the ShaderToggler ambiguity (HUD or glass?).
2. Bisect the shader list into per-element groups. This is the step that decides
   whether per-component control is possible.
3. Hook shader creation, hash, tag.
4. Point the existing viewport helper at each group, with its own scale/offset.
   1.00 and 0 offset = untouched, per the master-off-switch rule.
5. Check no tagged shader also draws world geometry.
