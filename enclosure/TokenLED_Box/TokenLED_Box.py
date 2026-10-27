"""
Token LED enclosure for Fusion 360 (script): a flat wedge-shaped box, taller at the front, thin at the back.

  front: USB-C opening for the ESP32-C3 SuperMini Plus + a light window (thin skin = diffuser) for the 7-LED strip
  back : thin; the lid is a sloped plate of constant thickness
  screws: 4 x M2 into brass heat-set inserts (pressed in with a soldering iron), screw heads sunk in the lid
  feet : 4 round recesses in the bottom for adhesive rubber feet

Run: Fusion 360 > Utilities > Scripts and Add-Ins > Scripts > "+" (add existing script) > pick the TokenLED_Box folder > Run.
All sizes are in the PARAMETERS block (millimetres); change them and run again (delete the previous bodies first).
Model axes: X = width (centred), Y = depth (front face at Y = 0), Z = height (bottom at Z = 0).
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
# Controller: ESP32-C3 SuperMini Plus lies with its USB-C end at the front. Measure your board and adjust.
BOARD_L = 23.0          # length (along the box depth)
BOARD_W = 18.0          # width
PCB_T = 1.2             # PCB thickness
STANDOFF = 1.5          # board height above the floor (rails under its long edges)
USB_ABOVE_PCB = 1.7     # USB-C centre above the PCB top
USB_SLOT_W = 11.0       # opening for the plug (with its overmould)
USB_SLOT_H = 5.2

# LED strip behind the front wall (LEDs facing forward, strip standing on its edge)
LED_COUNT = 7
LED_PITCH = 6.94        # 144 LED/m = 6.94 mm, 60 LED/m = 16.7 mm (then the box gets wide)
WIN_MARGIN = 4.5        # window extends this far beyond the outer LEDs (each side, total 9)
WIN_H = 8.0             # light window height
WIN_Z0 = 10.0           # window bottom above the box bottom
SKIN = 0.8              # diffuser skin left in the window (print in white / natural PLA)

# Box
DEPTH = 42.0
H_FRONT = 21.0          # height at the front (top surface)
H_REAR = 9.0            # height at the back
WIDTH_MIN = 70.0
WALL = 2.4
FLOOR = 2.0
LID_T = 2.4
CORNER_R = 3.0

# M2 heat-set insert (brass, soldering iron) and screw
INS_D = 3.2             # hole for the insert (check the datasheet of yours: 3.0 - 3.6)
INS_DEPTH = 4.0         # insert length
BOSS_D = 6.4
SCREW_D = 2.4           # clearance in the lid
HEAD_D = 4.4            # counterbore for the screw head
HEAD_DEPTH = 1.4
BOSS_INSET = 5.0        # boss centre from the side wall's outside
BOSS_Y_FRONT = 6.0      # boss centre from the front face
BOSS_Y_REAR = 6.0       # boss centre from the rear face

# Rubber feet (adhesive, round)
FOOT_D = 10.0
FOOT_RECESS = 1.0
FOOT_INSET = 9.0

EXPORT_STL = True       # also save base.stl and lid.stl next to this script

# ----------------------------------------------------------------------------------------------------------------------
LED_LEN = (LED_COUNT - 1) * LED_PITCH + 2 * WIN_MARGIN
WIDTH = max(WIDTH_MIN, math.ceil(LED_LEN + 17.0))


def top_z(y):
    """Height of the sloped top surface at depth y."""
    return H_FRONT + (H_REAR - H_FRONT) * y / DEPTH


def under_z(y):
    """Underside of the lid = top of the base walls."""
    return top_z(y) - LID_T


def screw_points():
    bx = WIDTH / 2.0 - BOSS_INSET
    return [(sx * bx, y) for sx in (-1, 1) for y in (BOSS_Y_FRONT, DEPTH - BOSS_Y_REAR)]


def check_layout():
    """Returns a list of problems (empty = fine)."""
    bad = []
    if WIN_Z0 + WIN_H > under_z(0) - 0.5:
        bad.append("Light window reaches the lid: raise H_FRONT or lower WIN_Z0 / WIN_H.")
    usb_zc = FLOOR + STANDOFF + PCB_T + USB_ABOVE_PCB
    if usb_zc + USB_SLOT_H / 2.0 > WIN_Z0 - 0.4:
        bad.append("USB opening and light window are too close: raise WIN_Z0 or lower the board.")
    if usb_zc - USB_SLOT_H / 2.0 < FLOOR + 0.4:
        bad.append("USB opening is too low (floor).")
    rear_y = DEPTH - BOSS_Y_REAR
    if under_z(rear_y) - FLOOR < INS_DEPTH + 0.6:
        bad.append("Rear boss is too short for the insert: raise H_REAR or lower INS_DEPTH.")
    if WIDTH / 2.0 - BOSS_INSET - BOSS_D / 2.0 < LED_LEN / 2.0 + 0.5:
        bad.append("Corner boss collides with the LED strip: widen the box (WIDTH_MIN).")
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
        planes = self.root.constructionPlanes
        inp = planes.createInput()
        inp.setByOffset(self.root.xYConstructionPlane, V(z))
        return planes.add(inp)

    # --- sketch + extrude helpers ---
    def polygon_extrude(self, plane, pts, op, full_len, bodies=None):
        """Closed polygon of world points (x, y, z in mm) on `plane`, extruded symmetrically (total length full_len)."""
        sk = self.root.sketches.add(plane)
        sp = [sk.modelToSketchSpace(P(*p)) for p in pts]
        lines = sk.sketchCurves.sketchLines
        first = prev = lines.addByTwoPoints(sp[0], sp[1])
        for i in range(2, len(sp)):
            prev = lines.addByTwoPoints(prev.endSketchPoint, sp[i])
        lines.addByTwoPoints(prev.endSketchPoint, first.startSketchPoint)
        return self._extrude(sk.profiles.item(0), op, full_len, bodies)

    def rects_extrude(self, plane, rects, op, full_len=None, height=None, bodies=None):
        """Rectangles [(x1, y1, z1, x2, y2, z2), ...] (world mm; two opposite corners, axis aligned in the plane)."""
        sk = self.root.sketches.add(plane)
        for r in rects:
            a = sk.modelToSketchSpace(P(r[0], r[1], r[2]))
            b = sk.modelToSketchSpace(P(r[3], r[4], r[5]))
            sk.sketchCurves.sketchLines.addTwoPointRectangle(a, b)
        return self._extrude(self._all_profiles(sk), op, full_len, bodies, height)

    def circles_extrude(self, plane, centers, diameter, op, full_len=None, height=None, bodies=None):
        sk = self.root.sketches.add(plane)
        for c in centers:
            sk.sketchCurves.sketchCircles.addByCenterRadius(sk.modelToSketchSpace(P(*c)), mm(diameter / 2.0))
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

    def fillet_vertical(self, body, radius):
        try:
            edges = adsk.core.ObjectCollection.create()
            for e in body.edges:
                a, b = e.startVertex.geometry, e.endVertex.geometry
                if abs(a.x - b.x) < 1e-6 and abs(a.y - b.y) < 1e-6 and abs(a.z - b.z) > 1e-6:
                    edges.add(e)
            if edges.count:
                fil = self.root.features.filletFeatures
                inp = fil.createInput()
                inp.addConstantRadiusEdgeSet(edges, V(radius), True)
                fil.add(inp)
        except Exception:
            pass    # rounded corners are cosmetic


def build(design):
    b = Builder(design)
    yz = b.root.yZConstructionPlane
    xz = b.root.xZConstructionPlane
    xy0 = b.root.xYConstructionPlane
    Join = adsk.fusion.FeatureOperations.JoinFeatureOperation
    Cut = adsk.fusion.FeatureOperations.CutFeatureOperation
    New = adsk.fusion.FeatureOperations.NewBodyFeatureOperation

    # ---------- base (tray) ----------
    # outer wedge, a little taller than the final rim; the rim is trimmed to the lid's underside below
    f = b.polygon_extrude(yz, [(0, 0, 0), (0, DEPTH, 0), (0, DEPTH, under_z(DEPTH) + 3), (0, 0, under_z(0) + 3)], New, WIDTH)
    base = f.bodies.item(0)
    base.name = "Base"
    b.fillet_vertical(base, CORNER_R)

    # cavity (open at the top)
    b.polygon_extrude(yz, [(0, WALL, FLOOR), (0, DEPTH - WALL, FLOOR), (0, DEPTH - WALL, 80), (0, WALL, 80)],
                      Cut, WIDTH - 2 * WALL)

    # board seat: two rails under the long edges, two side fences and a rear stop
    y0 = WALL + 0.3
    y1 = y0 + BOARD_L
    hx = BOARD_W / 2.0
    zf = FLOOR - 0.1
    rails = [(-hx + 0.5, y0, zf, -hx + 2.5, y1, zf), (hx - 2.5, y0, zf, hx - 0.5, y1, zf)]
    b.rects_extrude(b.xy_plane(zf), rails, Join, height=STANDOFF + 0.1)
    fence_h = STANDOFF + PCB_T + 1.5
    fences = [(-hx - 1.45, y0, zf, -hx - 0.25, y1 + 1.45, zf), (hx + 0.25, y0, zf, hx + 1.45, y1 + 1.45, zf),
              (-hx - 0.25, y1 + 0.25, zf, hx + 0.25, y1 + 1.45, zf)]
    b.rects_extrude(b.xy_plane(zf), fences, Join, height=fence_h + 0.1)

    # screw bosses (tall, trimmed with the rim below)
    b.circles_extrude(b.xy_plane(zf), [(x, y, zf) for x, y in screw_points()], BOSS_D, Join, height=60)

    # trim everything above the lid's underside plane
    b.polygon_extrude(yz, [(0, -5, under_z(-5)), (0, DEPTH + 5, under_z(DEPTH + 5)), (0, DEPTH + 5, 90), (0, -5, 90)],
                      Cut, WIDTH + 10)

    # USB-C opening in the front wall (centred on the board)
    usb_zc = FLOOR + STANDOFF + PCB_T + USB_ABOVE_PCB
    b.rects_extrude(xz, [(-USB_SLOT_W / 2.0, 0, usb_zc - USB_SLOT_H / 2.0, USB_SLOT_W / 2.0, 0, usb_zc + USB_SLOT_H / 2.0)],
                    Cut, full_len=2 * (WALL + 0.2))

    # light window: a pocket from the outside, SKIN mm of wall stays as the diffuser
    b.rects_extrude(xz, [(-LED_LEN / 2.0, 0, WIN_Z0, LED_LEN / 2.0, 0, WIN_Z0 + WIN_H)], Cut, full_len=2 * (WALL - SKIN))

    # rubber feet recesses in the bottom
    fx = WIDTH / 2.0 - FOOT_INSET
    feet = [(sx * fx, y, 0) for sx in (-1, 1) for y in (FOOT_INSET, DEPTH - FOOT_INSET)]
    b.circles_extrude(xy0, feet, FOOT_D + 0.2, Cut, full_len=2 * FOOT_RECESS)

    # insert holes in the boss tops (hole spans [top - INS_DEPTH - 0.4, top + 0.5], the top is sloped)
    for x, y in screw_points():
        top = under_z(y)
        zc = top - (INS_DEPTH + 0.4) / 2.0 + 0.25
        b.circles_extrude(b.xy_plane(zc), [(x, y, zc)], INS_D, Cut, full_len=INS_DEPTH + 0.4 + 0.5, bodies=[base])

    # ---------- lid (sloped plate of constant thickness) ----------
    f = b.polygon_extrude(yz, [(0, 0, under_z(0)), (0, DEPTH, under_z(DEPTH)), (0, DEPTH, top_z(DEPTH)), (0, 0, top_z(0))],
                          New, WIDTH)
    lid = f.bodies.item(0)
    lid.name = "Lid"
    b.fillet_vertical(lid, CORNER_R)
    for x, y in screw_points():
        t = top_z(y)
        zc = t - HEAD_DEPTH + (HEAD_DEPTH + 1.0) / 2.0          # counterbore: [t - HEAD_DEPTH, t + 1]
        b.circles_extrude(b.xy_plane(zc), [(x, y, zc)], HEAD_D, Cut, full_len=HEAD_DEPTH + 1.0, bodies=[lid])
        u = under_z(y)
        zc = (u - 0.5 + t + 1.0) / 2.0                           # clearance hole through the lid
        b.circles_extrude(b.xy_plane(zc), [(x, y, zc)], SCREW_D, Cut, full_len=(t + 1.0) - (u - 0.5), bodies=[lid])

    # hide the helper planes / sketches
    for i in range(b.root.constructionPlanes.count):
        b.root.constructionPlanes.item(i).isLightBulbOn = False
    for i in range(b.root.sketches.count):
        b.root.sketches.item(i).isLightBulbOn = False
    return base, lid


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
        base, lid = build(design)

        if EXPORT_STL:
            try:
                out = os.path.dirname(os.path.abspath(__file__))
                em = design.exportManager
                for body, name in ((base, "base"), (lid, "lid")):
                    opt = em.createSTLExportOptions(body, os.path.join(out, name + ".stl"))
                    opt.meshRefinement = adsk.fusion.MeshRefinementSettings.MeshRefinementMedium
                    em.execute(opt)
            except Exception:
                pass
        app.activeViewport.fit()
        ui.messageBox("Done: bodies 'Base' and 'Lid'.\n\nBox %.0f x %.0f mm, height %.0f mm at the front, %.0f mm at the back.\n"
                      "Screws: 4 x M2 x 5 into M2 heat-set inserts (hole %.1f mm, %.1f mm long)."
                      % (WIDTH, DEPTH, H_FRONT, H_REAR, INS_D, INS_DEPTH))
    except Exception:
        if ui:
            ui.messageBox("Failed:\n%s" % traceback.format_exc())


def stop(context):
    pass
