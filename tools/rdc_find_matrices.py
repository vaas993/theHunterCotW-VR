"""Find the view and projection matrices in a RenderDoc capture of theHunter CotW.

Run headlessly:

    qrenderdoc.exe --python tools/rdc_find_matrices.py -- <capture.rdc>

Why this exists: the camera hunt needs GROUND TRUTH numbers.  With the real
view-matrix floats in hand, finding their storage in the running process is an
exact 16-float search instead of a blind scan for angles - which is the method
that repeatedly failed on Far Cry 2 ("473 candidates collapsing to 2" means the
method is fooling you).

Approach: rather than fight the pipeline-state API (whose shape changes between
RenderDoc versions), enumerate every small buffer in the capture and scan its
bytes for float 4x4s that have the STRUCTURE of a view or projection matrix:

  view       upper-left 3x3 is orthonormal (rows unit length, mutually
             perpendicular) and it is not the identity
  projection D3D perspective shape - [2][3] == +/-1, [3][3] == 0, [0][0] and
             [1][1] positive; then fov_y = 2*atan(1/[1][1])

Both row-major and column-major are tested, because "which one is it" is a
question you cannot answer from code alone.
"""
import math
import struct
import sys

import renderdoc as rd


def floats(data, off, n):
    return struct.unpack_from("<%df" % n, data, off)


def is_finite(vals):
    return all(math.isfinite(v) for v in vals)


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def norm(a):
    return math.sqrt(dot(a, a))


def orthonormal_3x3(rows, tol=0.02):
    for r in rows:
        if abs(norm(r) - 1.0) > tol:
            return False
    if abs(dot(rows[0], rows[1])) > tol:
        return False
    if abs(dot(rows[0], rows[2])) > tol:
        return False
    if abs(dot(rows[1], rows[2])) > tol:
        return False
    # reject identity - every engine has hundreds of those
    ident = abs(rows[0][0] - 1) < 1e-4 and abs(rows[1][1] - 1) < 1e-4 and \
            abs(rows[2][2] - 1) < 1e-4 and abs(rows[0][1]) < 1e-4
    return not ident


def classify(m):
    """m is 16 floats. Return a list of (kind, detail) findings."""
    out = []
    if not is_finite(m):
        return out

    # --- row-major view: rows 0..2 hold the basis, row 3 holds translation
    rows = [m[0:3], m[4:7], m[8:11]]
    if orthonormal_3x3(rows):
        out.append(("view(row-major)", "pos=(%.2f, %.2f, %.2f)" % (m[12], m[13], m[14])))

    # --- column-major view: columns 0..2 hold the basis
    cols = [(m[0], m[4], m[8]), (m[1], m[5], m[9]), (m[2], m[6], m[10])]
    if orthonormal_3x3(cols):
        out.append(("view(col-major)", "pos=(%.2f, %.2f, %.2f)" % (m[3], m[7], m[11])))

    # --- D3D perspective projection
    for name, w, h, z1, z2 in (
        ("proj(row-major)", m[0], m[5], m[11], m[15]),
        ("proj(col-major)", m[0], m[5], m[14], m[15]),
    ):
        if abs(abs(z1) - 1.0) < 1e-3 and abs(z2) < 1e-6 and w > 0.05 and h > 0.05:
            fovy = 2.0 * math.atan(1.0 / h) * 180.0 / math.pi
            fovx = 2.0 * math.atan(1.0 / w) * 180.0 / math.pi
            out.append((name, "fovY=%.2f deg  fovX=%.2f deg  aspect=%.3f"
                        % (fovy, fovx, h / w if w else 0)))
    return out


def main():
    args = sys.argv[1:]
    if "--" in args:
        args = args[args.index("--") + 1:]
    if not args:
        print("usage: qrenderdoc --python this.py -- <capture.rdc>")
        return
    path = args[0]

    print("opening %s" % path)
    cap = rd.OpenCaptureFile()
    res = cap.OpenFile(path, "", None)
    # RenderDoc changed this return type across versions; accept either.
    ok = (res == rd.ResultCode.Succeeded) if hasattr(rd, "ResultCode") else bool(res)
    if not ok and str(res).lower().find("succe") < 0:
        print("could not open capture: %s" % res)
        return

    res, controller = cap.OpenCapture(rd.ReplayOptions(), None)
    if controller is None:
        print("could not start replay: %s" % res)
        return

    # Move to a draw well into the frame so constant buffers hold the values the
    # scene was actually drawn with, not their initial contents.
    try:
        actions = controller.GetRootActions()
    except AttributeError:
        actions = controller.GetDrawcalls()

    def leaves(lst):
        for a in lst:
            kids = getattr(a, "children", [])
            if kids:
                for k in leaves(kids):
                    yield k
            else:
                yield a

    all_leaves = list(leaves(actions))
    print("capture has %d leaf actions" % len(all_leaves))
    if not all_leaves:
        print("no actions - nothing to do")
        controller.Shutdown()
        return

    target = all_leaves[len(all_leaves) // 3]     # a third of the way in
    eid = getattr(target, "eventId", None)
    print("setting frame event to eid %s (%s)" % (eid, target.GetName(controller.GetStructuredFile())
          if hasattr(target, "GetName") else "?"))
    controller.SetFrameEvent(eid, True)

    bufs = controller.GetBuffers()
    print("capture has %d buffers; scanning the small ones (constant buffers)" % len(bufs))

    findings = {}
    scanned = 0
    for b in bufs:
        if b.length < 64 or b.length > 65536:
            continue
        try:
            data = controller.GetBufferData(b.resourceId, 0, 0)
        except Exception:
            continue
        if not data or len(data) < 64:
            continue
        scanned += 1
        # matrices are 16-byte aligned in every cbuffer layout worth the name
        for off in range(0, len(data) - 63, 16):
            m = floats(data, off, 16)
            for kind, detail in classify(m):
                key = (kind, detail)
                if key in findings:
                    continue
                findings[key] = (int(b.resourceId), off, m)

    print("scanned %d constant-sized buffers" % scanned)
    print()
    print("=" * 78)
    for (kind, detail), (rid, off, m) in sorted(findings.items()):
        print("%-18s %s" % (kind, detail))
        print("   buffer %d  offset 0x%X" % (rid, off))
        for r in range(4):
            print("     [% 12.5f % 12.5f % 12.5f % 12.5f]" % m[r * 4:r * 4 + 4])
        print()

    if not findings:
        print("nothing matched - widen the tolerances or pick a different action")

    controller.Shutdown()
    cap.Shutdown()


main()
