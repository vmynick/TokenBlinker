"""Token LED box: print orientation (base on its bottom, lid on its top face) + a P2S plate 3MF."""
import math, os, struct, zipfile
import numpy as np

SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'TokenLED_Box') + '/'   # base.stl / lid.stl from Fusion
PLATE = 256.0
H_FRONT, H_REAR, DEPTH = 22.0, 13.0, 52.0   # same as in TokenLED_Box.py


def read_stl(path):
    data = open(path, 'rb').read()
    n = struct.unpack('<I', data[80:84])[0]
    assert 84 + 50 * n == len(data), 'not a binary STL: ' + path
    rec = np.frombuffer(data[84:], dtype=np.dtype([('n', '<3f4'), ('v', '<9f4'), ('a', '<u2')]), count=n)
    return rec['v'].reshape(-1, 3, 3).astype(np.float64)


def write_stl(path, tris, title):
    with open(path, 'wb') as f:
        f.write(title.encode().ljust(80, b' '))
        f.write(struct.pack('<I', len(tris)))
        for t in tris:
            nrm = np.cross(t[1] - t[0], t[2] - t[0])
            l = np.linalg.norm(nrm)
            f.write(struct.pack('<12fH', *(nrm / l if l > 0 else nrm), *t.reshape(-1), 0))


def rot_x(t, phi):
    c, s = math.cos(phi), math.sin(phi)
    y, z = t[..., 1], t[..., 2]
    return np.stack([t[..., 0], y * c - z * s, y * s + z * c], axis=-1)


# lid: turn the sloped top face (normal (0, a, b)) to face straight down
k = (H_FRONT - H_REAR) / DEPTH
a, b = k / math.hypot(k, 1), 1 / math.hypot(k, 1)
phi = math.radians(-90) - math.atan2(b, a)
parts = [('base', 'Base', lambda t: t, (20.0, 128.0)),
         ('lid', 'Lid', lambda t: rot_x(t, phi), (110.0, 128.0))]
placed = []
for fn, nm, rot, (x0, yc) in parts:
    t = rot(read_stl(SRC + fn + '.stl'))
    t[..., 2] -= t[..., 2].min()
    t[..., 0] += x0 - t[..., 0].min()
    t[..., 1] += yc - (t[..., 1].min() + t[..., 1].max()) / 2
    placed.append((fn, nm, t))
    write_stl(SRC + fn + '_P2S.stl', t, 'Token LED box %s - print orientation' % nm)
    v = t.reshape(-1, 3)
    mn, mx = v.min(0), v.max(0)
    bed = np.isclose(v[:, 2], 0, atol=0.02).sum()
    print('%s: %d triangles, x %.1f..%.1f  y %.1f..%.1f  z %.1f..%.1f, vertices on the bed: %d'
          % (nm, len(t), mn[0], mx[0], mn[1], mx[1], mn[2], mx[2], bed))
    assert mn[0] >= 0 and mn[1] >= 0 and mx[0] <= PLATE and mx[1] <= PLATE, 'does not fit on the plate'

objs, items = [], []
for i, (fn, nm, t) in enumerate(placed, start=1):
    v = t.reshape(-1, 3)
    uniq, inv = np.unique(np.round(v, 4), axis=0, return_inverse=True)
    tri = inv.reshape(-1, 3)
    tri = tri[(tri[:, 0] != tri[:, 1]) & (tri[:, 1] != tri[:, 2]) & (tri[:, 0] != tri[:, 2])]
    vs = ''.join('<vertex x="%.4f" y="%.4f" z="%.4f"/>' % tuple(p) for p in uniq)
    ts = ''.join('<triangle v1="%d" v2="%d" v3="%d"/>' % tuple(r) for r in tri)
    objs.append('<object id="%d" name="%s" type="model"><mesh><vertices>%s</vertices><triangles>%s</triangles></mesh></object>'
                % (i, nm, vs, ts))
    items.append('<item objectid="%d"/>' % i)
model = ('<?xml version="1.0" encoding="UTF-8"?>\n'
         '<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02">'
         '<metadata name="Title">Token LED box (BBL P2S, PLA)</metadata>'
         '<resources>%s</resources><build>%s</build></model>') % (''.join(objs), ''.join(items))
ct = ('<?xml version="1.0" encoding="UTF-8"?>\n<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
      '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
      '<Default Extension="model" ContentType="application/vnd.ms-package.3dmanufacturing-3dmodel+xml"/></Types>')
rels = ('<?xml version="1.0" encoding="UTF-8"?>\n<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Target="/3D/3dmodel.model" Id="rel0" '
        'Type="http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel"/></Relationships>')
out = SRC + 'TokenLED_Box_P2S.3mf'
with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
    z.writestr('[Content_Types].xml', ct)
    z.writestr('_rels/.rels', rels)
    z.writestr('3D/3dmodel.model', model)
print('3MF ->', out)
