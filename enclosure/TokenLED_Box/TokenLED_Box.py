"""
Token LED enclosure for Fusion 360 (script), concept "A": a wedge with a trapezoid footprint.
Wide and tall at the front (7-LED light window), narrower and lower at the back (controller + USB-C).

  front: light window for the 7-LED strip (50 x 10 mm) behind a thin skin = diffuser
  back : the ESP32-C3 SuperMini Plus with its USB-C opening
  lid  : sloped plate of constant thickness, 4 x M2 screws into brass heat-set inserts (soldering iron), heads sunk
  feet : 4 round recesses in the bottom for adhesive rubber feet

Run: Fusion 360 > Utilities > Scripts and Add-Ins > Scripts > "+" (add existing script) > pick the TokenLED_Box folder > Run.
All sizes are in the PARAMETERS block (millimetres); change them and run again (the previous model is removed first).
Model axes: X = width (centred), Y = depth (front face at Y = 0, back at Y = DEPTH), Z = height (bottom at Z = 0).
Fusion's default "Y up" shows it lying on its back; switch to Z up in Preferences > General > Default modeling orientation.
"""
import math
import os
import traceback

import adsk.core
import adsk.fusion

# ----------------------------------------------------------------------------------------------------------------------
# PARAMETERS (mm)
# ----------------------------------------------------------------------------------------------------------------------
# Controller: ESP32-C3 SuperMini Plus at the back, its USB-C end against the rear wall. Measure your board and adjust.
BOARD_L = 23.0          # length (along the box depth)
BOARD_W = 18.0          # width
PCB_T = 1.2             # PCB thickness
STANDOFF = 1.5          # board height above the floor (rails under its long edges)
USB_ABOVE_PCB = 1.7     # USB-C centre above the PCB top
USB_SLOT_W = 11.0       # opening for the plug (with its overmould)
USB_SLOT_H = 5.2

# LED strip behind the front wall (LEDs facing forward, strip standing on its edge)
STRIP_LEN = 50.0
STRIP_W = 10.0
WIN_H = 8.0             # light window height (the strip is centred on it)
WIN_Z0 = 10.0           # window bottom above the box bottom
SKIN = 0.8              # diffuser skin left in the window (print in white / natural PLA)

# Box
W_FRONT = 70.0          # width at the front
W_REAR = 36.0           # width at the back
DEPTH = 52.0
H_FRONT = 22.0          # height at the front (top surface)
H_REAR = 13.0           # height at the back
WALL = 2.4
FLOOR = 2.0
LID_T = 2.4
R_FRONT = 6.0           # vertical corner radii
R_REAR = 4.0

# M2 heat-set insert (brass, soldering iron) and screw
INS_D = 3.2             # hole for the insert (check the datasheet of yours: 3.0 - 3.6)
INS_DEPTH = 4.0         # insert length
BOSS_D = 6.4
SCREW_D = 2.4           # clearance in the lid
HEAD_D = 4.4            # counterbore for the screw head
HEAD_DEPTH = 1.4
BOSS_INSET = 4.3        # boss centre from the (slanted) side's outside
BOSS_Y_FRONT = 5.5      # boss centre from the front face
BOSS_Y_REAR = 6.0       # boss centre from the rear face

# Rubber feet (adhesive, round)
FOOT_D = 10.0
FOOT_RECESS = 1.0
FOOT_INSET = 8.0        # from the side and from the front / back

EXPORT_STL = True       # also save base.stl and lid.stl next to this script


# ----------------------------------------------------------------------------------------------------------------------
def half_w(y):
    """Outer half width of the trapezoid footprint at depth y."""
    return W_FRONT / 2.0 + (W_REAR - W_FRONT) / 2.0 * y / DEPTH


def half_w_in(y):
    """Inner half width (the slanted side walls are WALL thick, measured square to the wall)."""
    k = (W_FRONT - W_REAR) / 2.0 / DEPTH
    return half_w(y) - WALL * math.sqrt(1 + k * k)


def top_z(y):
    """Height of the sloped top surface at depth y."""
    return H_FRONT + (H_REAR - H_FRONT) * y / DEPTH


def under_z(y):
    """Underside of the lid = top of the base walls."""
    return top_z(y) - LID_T


def usb_zc():
    return FLOOR + STANDOFF + PCB_T + USB_ABOVE_PCB


def board_y():
    """Board front and back edge (its USB-C end is at the back)."""
    y1 = DEPTH - WALL - 0.3
    return y1 - BOARD_L, y1


def screw_points():
    out = []
    for y in (BOSS_Y_FRONT, DEPTH - BOSS_Y_REAR):
        x = half_w(y) - BOSS_INSET
        out += [(-x, y), (x, y)]
    return out


def feet_points():
    out = []
    for y in (FOOT_INSET + 1.0, DEPTH - FOOT_INSET):
        x = half_w(y) - FOOT_INSET
        out += [(-x, y), (x, y)]
    return out


def check_layout():
    """Returns a list of problems (empty = fine)."""
    bad = []
    if WIN_Z0 + WIN_H / 2.0 + STRIP_W / 2.0 > under_z(0):
        bad.append("The strip does not fit under the lid at the front: raise H_FRONT or lower WIN_Z0.")
    if WIN_Z0 + WIN_H > under_z(0) - 0.5:
        bad.append("Light window reaches the lid: raise H_FRONT or lower WIN_Z0 / WIN_H.")
    if STRIP_LEN / 2.0 + 0.5 > half_w(0) - R_FRONT:
        bad.append("The strip is wider than the front: raise W_FRONT.")
    fx = half_w(BOSS_Y_FRONT) - BOSS_INSET
    if fx - BOSS_D / 2.0 < STRIP_LEN / 2.0 + 0.5:
        bad.append("Front boss collides with the LED strip: raise W_FRONT or BOSS_Y_FRONT.")
    if usb_zc() + USB_SLOT_H / 2.0 > under_z(DEPTH) - 0.8:
        bad.append("USB opening reaches the lid at the back: raise H_REAR or lower the board.")
    if usb_zc() - USB_SLOT_H / 2.0 < FLOOR + 0.4:
        bad.append("USB opening is too low (floor).")
    if usb_zc() + 2.0 > under_z(DEPTH) - 0.3:
        bad.append("Board + USB-C is taller than the box at the back: raise H_REAR.")
    rx = half_w(DEPTH - BOSS_Y_REAR) - BOSS_INSET
    if rx - BOSS_D / 2.0 < BOARD_W / 2.0 + 1.45 + 0.5:
        bad.append("Rear boss collides with the board seat: raise W_REAR.")
    if half_w_in(DEPTH - WALL) < USB_SLOT_W / 2.0 + 1.0:
        bad.append("The back is too narrow for the USB opening.")
    if under_z(DEPTH - BOSS_Y_REAR) - FLOOR < INS_DEPTH + 0.6:
        bad.append("Rear boss is too short for the insert: raise H_REAR or lower INS_DEPTH.")
    if LID_T - HEAD_DEPTH < 0.8:
        bad.append("Lid is too thin under the screw head.")
    if WALL - SKIN < 0.8:
        bad.append("Front wall too thin for the diffuser window.")
    return bad


# ----------------------------------------------------------------------------------------------------------------------
def mm(v):
    return v / 10.0     # Fusion's internal unit is cm


def P(x, y, z):
    return adsk.core.Point3D.create(mm(x), mm(y), mm(z))


def V(v_mm):
    return adsk.core.ValueInput.createByReal(mm(v_mm))


class Builder:
    def __init__(self, design):
        self.root = design.rootComponent
        self.design = design

    # --- planes ---
    def xy_plane(self, z):
        if abs(z) < 1e-9:
            return self.root.xYConstructionPlane
        planes = self.root.constructionPlanes
        inp = planes.createInput()
        inp.setByOffset(self.root.xYConstructionPlane, V(z))
        return planes.add(inp)

    # --- sketch + extrude helpers ---
    def polygon_extrude(self, plane, pts, op, full_len=None, bodies=None, height=None):
        """Closed polygon of world points (x, y, z in mm) on `plane`; extruded symmetrically (total length full_len)
        or one way along the plane normal (height)."""
        sk = self.root.sketches.add(plane)
        sp = [sk.modelToSketchSpace(P(*p)) for p in pts]
        lines = sk.sketchCurves.sketchLines
        first = prev = lines.addByTwoPoints(sp[0], sp[1])
        for i in range(2, len(sp)):
            prev = lines.addByTwoPoints(prev.endSketchPoint, sp[i])
        lines.addByTwoPoints(prev.endSketchPoint, first.startSketchPoint)
        return self._extrude(sk.profiles.item(0), op, full_len, bodies, height)

    def rects_extrude(self, plane, rects, op, full_len=None, height=None, bodies=None):
        """Rectangles [(x1, y1, z1, x2, y2, z2), ...] (world mm; two opposite corners, axis aligned in the plane)."""
        sk = self.root.sketches.add(plane)
        for r in rects:
            a = sk.modelToSketchSpace(P(r[0], r[1], r[2]))
            b = sk.modelToSketchSpace(P(r[3], r[4], r[5]))
            sk.sketchCurves.sketchLines.addTwoPointRectangle(a, b)
        return self._extrude(self._all_profiles(sk), op, full_len, bodies, height)

    def circles_extrude(self, plane, centers, diameter, op, full_len=None, height=None, bodies=None, polygons=()):
        """Circles (and optional closed polygons of world points) in one sketch; every region is extruded."""
        sk = self.root.sketches.add(plane)
        for c in centers:
            sk.sketchCurves.sketchCircles.addByCenterRadius(sk.modelToSketchSpace(P(*c)), mm(diameter / 2.0))
        lines = sk.sketchCurves.sketchLines
        for pts in polygons:
            sp = [sk.modelToSketchSpace(P(*p)) for p in pts]
            first = prev = lines.addByTwoPoints(sp[0], sp[1])
            for i in range(2, len(sp)):
                prev = lines.addByTwoPoints(prev.endSketchPoint, sp[i])
            lines.addByTwoPoints(prev.endSketchPoint, first.startSketchPoint)
        return self._extrude(self._all_profiles(sk), op, full_len, bodies, height)

    @staticmethod
    def _all_profiles(sk):
        col = adsk.core.ObjectCollection.create()
        for i in range(sk.profiles.count):
            col.add(sk.profiles.item(i))
        return col

    def _extrude(self, profile, op, full_len=None, bodies=None, height=None):
        ext = self.root.features.extrudeFeatures
        inp = ext.createInput(profile, op)
        if full_len is not None:
            inp.setSymmetricExtent(V(full_len), True)
        else:
            inp.setDistanceExtent(False, V(height))
        if bodies:
            inp.participantBodies = bodies
        return ext.add(inp)

    def fillet_vertical(self, body, radius, y_min=-1e9, y_max=1e9):
        """Rounds the vertical edges of `body` whose y (mm) is within [y_min, y_max]."""
        try:
            edges = adsk.core.ObjectCollection.create()
            for e in body.edges:
                a, b = e.startVertex.geometry, e.endVertex.geometry
                if abs(a.x - b.x) < 1e-6 and abs(a.y - b.y) < 1e-6 and abs(a.z - b.z) > 1e-6 and y_min <= a.y * 10 <= y_max:
                    edges.add(e)
            if edges.count:
                fil = self.root.features.filletFeatures
                inp = fil.createInput()
                inp.addConstantRadiusEdgeSet(edges, V(radius), True)
                fil.add(inp)
        except Exception as e:
            print("fillet skipped:", e)    # rounded corners are cosmetic


# ---------- build steps (each one can also be run on its own, in this order) ----------
Join = None
Cut = None
New = None


def _ops():
    global Join, Cut, New
    Join = adsk.fusion.FeatureOperations.JoinFeatureOperation
    Cut = adsk.fusion.FeatureOperations.CutFeatureOperation
    New = adsk.fusion.FeatureOperations.NewBodyFeatureOperation


def body(b, name):
    for i in range(b.root.bRepBodies.count):
        if b.root.bRepBodies.item(i).name == name:
            return b.root.bRepBodies.item(i)
    raise RuntimeError("body not found: " + name)


def plan(z=0.0, grow=0.0):
    """Trapezoid footprint at height z (grow: extra margin on every side)."""
    return [(-half_w(-grow) - grow, -grow, z), (half_w(-grow) + grow, -grow, z),
            (half_w(DEPTH + grow) + grow, DEPTH + grow, z), (-half_w(DEPTH + grow) - grow, DEPTH + grow, z)]


def s_base_prism(b):
    # trapezoid prism, a little taller than the final rim; the rim is trimmed to the lid's underside later
    f = b.polygon_extrude(b.root.xYConstructionPlane, plan(), New, height=under_z(0) + 3)
    f.bodies.item(0).name = "Base"


def s_base_fillet(b):
    b.fillet_vertical(body(b, "Base"), R_FRONT, y_max=1.0)
    b.fillet_vertical(body(b, "Base"), R_REAR, y_min=DEPTH - 1.0)


def s_cavity(b):
    y0, y1 = WALL, DEPTH - WALL
    pts = [(-half_w_in(y0), y0, FLOOR), (half_w_in(y0), y0, FLOOR), (half_w_in(y1), y1, FLOOR), (-half_w_in(y1), y1, FLOOR)]
    b.polygon_extrude(b.xy_plane(FLOOR), pts, Cut, height=80, bodies=[body(b, "Base")])


def s_board_seat(b):
    # two rails under the board's long edges, two side fences and a front stop (the USB-C end is at the back)
    y0, y1 = board_y()
    hx = BOARD_W / 2.0
    zf = FLOOR - 0.1
    rails = [(-hx + 0.5, y0, zf, -hx + 2.5, y1, zf), (hx - 2.5, y0, zf, hx - 0.5, y1, zf)]
    b.rects_extrude(b.xy_plane(zf), rails, Join, height=STANDOFF + 0.1, bodies=[body(b, "Base")])
    fence_h = STANDOFF + PCB_T + 1.5
    fences = [(-hx - 1.45, y0 - 1.45, zf, -hx - 0.25, y1, zf), (hx + 0.25, y0 - 1.45, zf, hx + 1.45, y1, zf),
              (-hx - 0.25, y0 - 1.45, zf, hx + 0.25, y0 - 0.25, zf)]
    b.rects_extrude(b.xy_plane(zf), fences, Join, height=fence_h + 0.1, bodies=[body(b, "Base")])


def _foot(c, p1, p2):
    """Foot of the perpendicular from point c onto the line p1-p2 (2D)."""
    dx, dy = p2[0] - p1[0], p2[1] - p1[1]
    t = ((c[0] - p1[0]) * dx + (c[1] - p1[1]) * dy) / (dx * dx + dy * dy)
    return p1[0] + t * dx, p1[1] + t * dy


def corner_fills(z):
    """For every boss: a quadrilateral boss centre - wall - inner corner - wall, so no thin gap is left in the corner."""
    yf, yr = WALL, DEPTH - WALL
    out = []
    for x, y in screw_points():
        s = 1 if x > 0 else -1
        front = y < DEPTH / 2
        corner = (s * half_w_in(yf if front else yr), yf if front else yr)
        across = (-corner[0], corner[1])                               # the same wall's other end
        along = (s * half_w_in(yr if front else yf), yr if front else yf)  # the side wall's other end
        pts = [(x, y, z)]
        for far in (across, along):
            f = _foot((x, y), corner, far)
            # a boss radius further from the corner, so the boss meets the wall without a step
            dx, dy = far[0] - corner[0], far[1] - corner[1]
            n = math.hypot(dx, dy)
            pts.append((f[0] + dx / n * BOSS_D / 2.0, f[1] + dy / n * BOSS_D / 2.0, z))
        out.append([pts[0], pts[1], (corner[0], corner[1], z), pts[2]])
    return out


def s_bosses(b):
    # tall, trimmed with the rim in the next step; each boss is joined to its corner
    zf = FLOOR - 0.1
    b.circles_extrude(b.xy_plane(zf), [(x, y, zf) for x, y in screw_points()], BOSS_D, Join, height=60,
                      bodies=[body(b, "Base")], polygons=corner_fills(zf))


def s_trim(b):
    # everything above the lid's underside plane goes
    b.polygon_extrude(b.root.yZConstructionPlane,
                      [(0, -5, under_z(-5)), (0, DEPTH + 5, under_z(DEPTH + 5)), (0, DEPTH + 5, 90), (0, -5, 90)],
                      Cut, W_FRONT + 10, bodies=[body(b, "Base")])


def s_usb(b):
    # USB-C opening in the rear wall: a rectangle in a horizontal plane, extruded up and down to the slot height
    zc = usb_zc()
    rect = [(-USB_SLOT_W / 2.0, DEPTH - WALL - 0.2, zc, USB_SLOT_W / 2.0, DEPTH + 1.0, zc)]
    b.rects_extrude(b.xy_plane(zc), rect, Cut, full_len=USB_SLOT_H, bodies=[body(b, "Base")])


def s_window(b):
    # a pocket from the outside of the front wall; SKIN mm of the wall stays as the diffuser
    b.rects_extrude(b.root.xZConstructionPlane, [(-STRIP_LEN / 2.0, 0, WIN_Z0, STRIP_LEN / 2.0, 0, WIN_Z0 + WIN_H)],
                    Cut, full_len=2 * (WALL - SKIN), bodies=[body(b, "Base")])


def s_feet(b):
    b.circles_extrude(b.root.xYConstructionPlane, [(x, y, 0) for x, y in feet_points()], FOOT_D + 0.2, Cut,
                      full_len=2 * FOOT_RECESS, bodies=[body(b, "Base")])


def s_inserts(b):
    # hole spans [top - INS_DEPTH - 0.4, top + 0.5] (the boss top is sloped)
    for x, y in screw_points():
        zc = under_z(y) - (INS_DEPTH + 0.4) / 2.0 + 0.25
        b.circles_extrude(b.xy_plane(zc), [(x, y, zc)], INS_D, Cut, full_len=INS_DEPTH + 0.4 + 0.5, bodies=[body(b, "Base")])


def s_lid(b):
    # sloped plate of constant thickness, cut to the trapezoid
    f = b.polygon_extrude(b.root.yZConstructionPlane,
                          [(0, 0, under_z(0)), (0, DEPTH, under_z(DEPTH)), (0, DEPTH, top_z(DEPTH)), (0, 0, top_z(0))],
                          New, W_FRONT + 4)
    f.bodies.item(0).name = "Lid"
    for s in (-1, 1):
        outside = [(s * half_w(-5), -5, 0), (s * (W_FRONT / 2 + 20), -5, 0), (s * (W_FRONT / 2 + 20), DEPTH + 5, 0),
                   (s * half_w(DEPTH + 5), DEPTH + 5, 0)]
        b.polygon_extrude(b.root.xYConstructionPlane, outside, Cut, full_len=100, bodies=[body(b, "Lid")])


def s_lid_fillet(b):
    b.fillet_vertical(body(b, "Lid"), R_FRONT, y_max=1.0)
    b.fillet_vertical(body(b, "Lid"), R_REAR, y_min=DEPTH - 1.0)


def s_lid_holes(b):
    for x, y in screw_points():
        t, u = top_z(y), under_z(y)
        zc = t - HEAD_DEPTH + (HEAD_DEPTH + 1.0) / 2.0          # counterbore: [t - HEAD_DEPTH, t + 1]
        b.circles_extrude(b.xy_plane(zc), [(x, y, zc)], HEAD_D, Cut, full_len=HEAD_DEPTH + 1.0, bodies=[body(b, "Lid")])
        zc = (u - 0.5 + t + 1.0) / 2.0                           # clearance hole through the lid
        b.circles_extrude(b.xy_plane(zc), [(x, y, zc)], SCREW_D, Cut, full_len=(t + 1.0) - (u - 0.5),
                          bodies=[body(b, "Lid")])


def s_tidy(b):
    for i in range(b.root.constructionPlanes.count):
        b.root.constructionPlanes.item(i).isLightBulbOn = False
    for i in range(b.root.sketches.count):
        b.root.sketches.item(i).isLightBulbOn = False


STEPS = [s_base_prism, s_base_fillet, s_cavity, s_board_seat, s_bosses, s_trim, s_usb, s_window, s_feet, s_inserts,
         s_lid, s_lid_fillet, s_lid_holes, s_tidy]


def clear(design):
    """Removes everything this script (or anything else) built in the design's timeline."""
    tl = design.timeline
    if tl.count:
        tl.moveToBeginning()
        tl.deleteAllAfterMarker()


def build(design, first=0, last=None):
    """Runs STEPS[first..last] (all by default); returns the Base and Lid bodies (None if not built yet)."""
    _ops()
    b = Builder(design)
    for step in STEPS[first:(len(STEPS) if last is None else last + 1)]:
        step(b)
    names = [b.root.bRepBodies.item(i).name for i in range(b.root.bRepBodies.count)]
    return (body(b, "Base") if "Base" in names else None), (body(b, "Lid") if "Lid" in names else None)


def run(context):
    app = adsk.core.Application.get()
    ui = app.userInterface
    try:
        design = adsk.fusion.Design.cast(app.activeProduct)
        if not design:
            ui.messageBox("Open (or create) a Fusion design first.")
            return
        problems = check_layout()
        if problems:
            ui.messageBox("Check the parameters:\n\n- " + "\n- ".join(problems))
            return
        if design.timeline.count and ui.messageBox("Remove the current model and build the box?", "Token LED box",
                                                   adsk.core.MessageBoxButtonTypes.OKCancelButtonType) != 0:
            return
        clear(design)
        base, lid = build(design)

        if EXPORT_STL:
            try:
                out = os.path.dirname(os.path.abspath(__file__))
                em = design.exportManager
                for bd, name in ((base, "base"), (lid, "lid")):
                    opt = em.createSTLExportOptions(bd, os.path.join(out, name + ".stl"))
                    opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementMedium
                    em.execute(opt)
            except Exception:
                pass
        app.activeViewport.fit()
        ui.messageBox("Done: bodies 'Base' and 'Lid'.\n\nBox %.0f mm wide at the front, %.0f at the back, %.0f deep; "
                      "%.0f mm high at the front, %.0f at the back.\n"
                      "Screws: 4 x M2 x 5 into M2 heat-set inserts (hole %.1f mm, %.1f mm long)."
                      % (W_FRONT, W_REAR, DEPTH, H_FRONT, H_REAR, INS_D, INS_DEPTH))
    except Exception:
        if ui:
            ui.messageBox("Failed:\n%s" % traceback.format_exc())


def stop(context):
    pass
