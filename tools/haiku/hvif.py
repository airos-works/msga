#!/usr/bin/env python3
# Copied from TasAmp (tools/hvif.py, MIT, Copyright (c) 2026 Amp contributors).
"""Reader, writer and host preview for Haiku Vector Icon Format (HVIF) files.

The format matches Haiku's src/libs/icon/flat_icon. Icons are described as a
small dictionary: styles (solid colours or gradients), paths (points with
optional cubic control handles) and shapes (a style plus paths, optional
transform and transformers such as stroke/contour). The canvas is 64 x 64.

    python3 tools/hvif.py dump  icon.hvif
    python3 tools/hvif.py roundtrip icon.hvif      # decode + encode, compare
    python3 tools/hvif.py preview icon.hvif out.png [size]
"""
import math
import struct
import sys

MAGIC = b'ncif'
STYLE_SOLID = 1
STYLE_GRADIENT = 2
STYLE_SOLID_NO_ALPHA = 3
STYLE_GRAY = 4
STYLE_GRAY_NO_ALPHA = 5
GRADIENT_TRANSFORM = 1 << 1
GRADIENT_NO_ALPHA = 1 << 2
GRADIENT_16BIT = 1 << 3
GRADIENT_GRAYS = 1 << 4
GRADIENT_LINEAR, GRADIENT_CIRCULAR, GRADIENT_DIAMOND, GRADIENT_CONIC, GRADIENT_XY, GRADIENT_SQRT_XY = range(6)
SHAPE_PATH_SOURCE = 10
SHAPE_TRANSFORM = 1 << 1
SHAPE_HINTING = 1 << 2
SHAPE_LOD_SCALE = 1 << 3
SHAPE_HAS_TRANSFORMERS = 1 << 4
SHAPE_TRANSLATION = 1 << 5
PATH_CLOSED = 1 << 1
PATH_USES_COMMANDS = 1 << 2
PATH_NO_CURVES = 1 << 3
CMD_H_LINE, CMD_V_LINE, CMD_LINE, CMD_CURVE = range(4)
TRANSFORMER_AFFINE = 20
TRANSFORMER_CONTOUR = 21
TRANSFORMER_PERSPECTIVE = 22
TRANSFORMER_STROKE = 23


class Reader:
    def __init__(self, data):
        self.data = data
        self.at = 0

    def u8(self):
        value = self.data[self.at]
        self.at += 1
        return value

    def coord(self):
        value = self.u8()
        if value & 0x80:
            value = ((value & 0x7f) << 8) | self.u8()
            return value / 102.0 - 128.0
        return float(value - 32)

    def float24(self):
        b1, b2, b3 = self.u8(), self.u8(), self.u8()
        value = (b1 << 16) | (b2 << 8) | b3
        sign = (value & 0x800000) >> 23
        exponent = ((value & 0x7e0000) >> 17) - 32
        mantissa = (value & 0x01ffff) << 6
        bits = (sign << 31) | ((exponent + 127) << 23) | mantissa
        return struct.unpack('<f', struct.pack('<I', bits))[0]

    def matrix(self):
        return [self.float24() for _ in range(6)]

    def float32(self):
        value = struct.unpack('<f', bytes(self.data[self.at:self.at + 4]))[0]
        self.at += 4
        return value


class Writer:
    def __init__(self):
        self.out = bytearray()

    def u8(self, value):
        if not 0 <= int(value) <= 255:
            raise ValueError('byte out of range: %r' % value)
        self.out.append(int(value))

    def coord(self, value):
        if -32 <= value <= 95 and float(value).is_integer():
            self.u8(int(value) + 32)
            return
        if not -128 <= value < 193:
            raise ValueError('coordinate out of range: %r' % value)
        packed = int(round((value + 128.0) * 102.0))
        packed = max(0, min(32767, packed))
        self.u8(0x80 | (packed >> 8))
        self.u8(packed & 0xff)

    def float24(self, value):
        bits = struct.unpack('<I', struct.pack('<f', float(value)))[0]
        sign = (bits & 0x80000000) >> 31
        exponent = ((bits & 0x7f800000) >> 23) - 127
        mantissa = bits & 0x007fffff
        if value == 0.0:
            exponent, mantissa = -32, 0
        if exponent >= 32 or exponent < -32:
            raise ValueError('float24 exponent out of range: %r' % value)
        packed = (sign << 23) | ((exponent + 32) << 17) | (mantissa >> 6)
        self.u8((packed >> 16) & 0xff)
        self.u8((packed >> 8) & 0xff)
        self.u8(packed & 0xff)

    def matrix(self, m):
        for value in m:
            self.float24(value)


IDENTITY = [1.0, 0.0, 0.0, 1.0, 0.0, 0.0]


def is_identity(m, eps=1e-6):
    return all(abs(a - b) < eps for a, b in zip(m, IDENTITY))


def decode(data):
    r = Reader(data)
    if data[:4] != MAGIC:
        raise ValueError('not an HVIF file')
    r.at = 4
    icon = {'styles': [], 'paths': [], 'shapes': []}
    for _ in range(r.u8()):
        kind = r.u8()
        if kind == STYLE_SOLID:
            icon['styles'].append({'color': (r.u8(), r.u8(), r.u8(), r.u8())})
        elif kind == STYLE_SOLID_NO_ALPHA:
            icon['styles'].append({'color': (r.u8(), r.u8(), r.u8(), 255)})
        elif kind == STYLE_GRAY:
            g = r.u8()
            icon['styles'].append({'color': (g, g, g, r.u8())})
        elif kind == STYLE_GRAY_NO_ALPHA:
            g = r.u8()
            icon['styles'].append({'color': (g, g, g, 255)})
        elif kind == STYLE_GRADIENT:
            gtype, flags, stops = r.u8(), r.u8(), r.u8()
            style = {'gradient': gtype, 'matrix': list(IDENTITY), 'stops': []}
            if flags & GRADIENT_TRANSFORM:
                style['matrix'] = r.matrix()
            for _ in range(stops):
                offset = r.u8() / 255.0
                if flags & GRADIENT_GRAYS:
                    g = r.u8()
                    a = 255 if flags & GRADIENT_NO_ALPHA else r.u8()
                    color = (g, g, g, a)
                else:
                    rr, gg, bb = r.u8(), r.u8(), r.u8()
                    a = 255 if flags & GRADIENT_NO_ALPHA else r.u8()
                    color = (rr, gg, bb, a)
                style['stops'].append((offset, color))
            icon['styles'].append(style)
        else:
            raise ValueError('unknown style type %d' % kind)
    for _ in range(r.u8()):
        flags, count = r.u8(), r.u8()
        path = {'closed': bool(flags & PATH_CLOSED), 'points': []}
        if flags & PATH_NO_CURVES:
            for _ in range(count):
                x, y = r.coord(), r.coord()
                path['points'].append(((x, y), (x, y), (x, y)))
        elif flags & PATH_USES_COMMANDS:
            commands = [r.u8() for _ in range((count + 3) // 4)]
            lx = ly = 0.0
            for i in range(count):
                cmd = (commands[i // 4] >> (2 * (i % 4))) & 3
                if cmd == CMD_H_LINE:
                    x, y = r.coord(), ly
                    path['points'].append(((x, y), (x, y), (x, y)))
                elif cmd == CMD_V_LINE:
                    x, y = lx, r.coord()
                    path['points'].append(((x, y), (x, y), (x, y)))
                elif cmd == CMD_LINE:
                    x, y = r.coord(), r.coord()
                    path['points'].append(((x, y), (x, y), (x, y)))
                else:
                    x, y = r.coord(), r.coord()
                    xi, yi = r.coord(), r.coord()
                    xo, yo = r.coord(), r.coord()
                    path['points'].append(((x, y), (xi, yi), (xo, yo)))
                lx, ly = x, y
        else:
            for _ in range(count):
                x, y = r.coord(), r.coord()
                xi, yi = r.coord(), r.coord()
                xo, yo = r.coord(), r.coord()
                path['points'].append(((x, y), (xi, yi), (xo, yo)))
        icon['paths'].append(path)
    for _ in range(r.u8()):
        kind = r.u8()
        if kind != SHAPE_PATH_SOURCE:
            raise ValueError('unknown shape type %d' % kind)
        shape = {'style': r.u8(), 'paths': [], 'matrix': list(IDENTITY), 'hinting': False,
                 'lod': (0.0, 4.0), 'transformers': []}
        shape['paths'] = [r.u8() for _ in range(r.u8())]
        flags = r.u8()
        if flags & SHAPE_TRANSFORM:
            shape['matrix'] = r.matrix()
        if flags & SHAPE_TRANSLATION:
            shape['matrix'] = [1.0, 0.0, 0.0, 1.0, r.coord(), r.coord()]
        if flags & SHAPE_HINTING:
            shape['hinting'] = True
        if flags & SHAPE_LOD_SCALE:
            shape['lod'] = (r.u8() / 63.75, r.u8() / 63.75)
        if flags & SHAPE_HAS_TRANSFORMERS:
            for _ in range(r.u8()):
                ttype = r.u8()
                if ttype == TRANSFORMER_AFFINE:
                    shape['transformers'].append({'type': 'affine', 'matrix': [r.float32() for _ in range(6)]})
                elif ttype == TRANSFORMER_CONTOUR:
                    shape['transformers'].append({'type': 'contour', 'width': r.u8() - 128.0,
                                                  'join': r.u8(), 'miter': r.u8()})
                elif ttype == TRANSFORMER_PERSPECTIVE:
                    shape['transformers'].append({'type': 'perspective',
                                                  'matrix': [r.float24() for _ in range(9)]})
                elif ttype == TRANSFORMER_STROKE:
                    width = r.u8() - 128.0
                    options = r.u8()
                    shape['transformers'].append({'type': 'stroke', 'width': width,
                                                  'join': options & 15, 'cap': options >> 4,
                                                  'miter': r.u8()})
                else:
                    raise ValueError('unknown transformer %d' % ttype)
        icon['shapes'].append(shape)
    if r.at != len(data):
        raise ValueError('trailing bytes: %d' % (len(data) - r.at))
    return icon


def encode(icon):
    w = Writer()
    w.out += MAGIC
    styles, paths, shapes = icon['styles'], icon['paths'], icon['shapes']
    w.u8(len(styles))
    for style in styles:
        if 'gradient' in style:
            stops = style['stops']
            flags = 0
            matrix = style.get('matrix', IDENTITY)
            if not is_identity(matrix):
                flags |= GRADIENT_TRANSFORM
            if all(c[3] == 255 for _, c in stops):
                flags |= GRADIENT_NO_ALPHA
            if all(c[0] == c[1] == c[2] for _, c in stops):
                flags |= GRADIENT_GRAYS
            w.u8(STYLE_GRADIENT)
            w.u8(style['gradient'])
            w.u8(flags)
            w.u8(len(stops))
            if flags & GRADIENT_TRANSFORM:
                w.matrix(matrix)
            for offset, color in stops:
                w.u8(int(round(offset * 255)))
                if flags & GRADIENT_GRAYS:
                    w.u8(color[0])
                else:
                    w.u8(color[0]); w.u8(color[1]); w.u8(color[2])
                if not flags & GRADIENT_NO_ALPHA:
                    w.u8(color[3])
        else:
            r_, g_, b_, a_ = style['color']
            if r_ == g_ == b_:
                if a_ == 255:
                    w.u8(STYLE_GRAY_NO_ALPHA); w.u8(r_)
                else:
                    w.u8(STYLE_GRAY); w.u8(r_); w.u8(a_)
            elif a_ == 255:
                w.u8(STYLE_SOLID_NO_ALPHA); w.u8(r_); w.u8(g_); w.u8(b_)
            else:
                w.u8(STYLE_SOLID); w.u8(r_); w.u8(g_); w.u8(b_); w.u8(a_)
    w.u8(len(paths))
    for path in paths:
        points = [normalize_point(p) for p in path['points']]
        flags = PATH_CLOSED if path.get('closed', True) else 0
        straight = all(p[0] == p[1] == p[2] for p in points)
        if straight:
            flags |= PATH_NO_CURVES
        else:
            flags |= PATH_USES_COMMANDS
        w.u8(flags)
        w.u8(len(points))
        if straight:
            for (x, y), _, _ in points:
                w.coord(x); w.coord(y)
        else:
            commands = []
            lx = ly = 0.0
            for (x, y), pin, pout in points:
                if (x, y) == pin == pout:
                    if y == ly:
                        commands.append(CMD_H_LINE)
                    elif x == lx:
                        commands.append(CMD_V_LINE)
                    else:
                        commands.append(CMD_LINE)
                else:
                    commands.append(CMD_CURVE)
                lx, ly = x, y
            packed = [0] * ((len(points) + 3) // 4)
            for i, cmd in enumerate(commands):
                packed[i // 4] |= cmd << (2 * (i % 4))
            for byte in packed:
                w.u8(byte)
            for cmd, ((x, y), (xi, yi), (xo, yo)) in zip(commands, points):
                if cmd == CMD_H_LINE:
                    w.coord(x)
                elif cmd == CMD_V_LINE:
                    w.coord(y)
                elif cmd == CMD_LINE:
                    w.coord(x); w.coord(y)
                else:
                    w.coord(x); w.coord(y); w.coord(xi); w.coord(yi); w.coord(xo); w.coord(yo)
    w.u8(len(shapes))
    for shape in shapes:
        w.u8(SHAPE_PATH_SOURCE)
        w.u8(shape['style'])
        w.u8(len(shape['paths']))
        for index in shape['paths']:
            w.u8(index)
        matrix = shape.get('matrix', IDENTITY)
        flags = 0
        translation = None
        if not is_identity(matrix):
            if is_identity(matrix[:4] + [0.0, 0.0]):
                flags |= SHAPE_TRANSLATION
                translation = matrix[4:]
            else:
                flags |= SHAPE_TRANSFORM
        if shape.get('hinting'):
            flags |= SHAPE_HINTING
        lod = shape.get('lod', (0.0, 4.0))
        if lod != (0.0, 4.0):
            flags |= SHAPE_LOD_SCALE
        transformers = shape.get('transformers', [])
        if transformers:
            flags |= SHAPE_HAS_TRANSFORMERS
        w.u8(flags)
        if flags & SHAPE_TRANSFORM:
            w.matrix(matrix)
        if translation is not None:
            w.coord(translation[0]); w.coord(translation[1])
        if flags & SHAPE_LOD_SCALE:
            w.u8(int(round(lod[0] * 63.75))); w.u8(int(round(lod[1] * 63.75)))
        if transformers:
            w.u8(len(transformers))
            for t in transformers:
                if t['type'] == 'affine':
                    w.u8(TRANSFORMER_AFFINE)
                    for value in t['matrix']:
                        w.out += struct.pack('<f', float(value))
                elif t['type'] == 'contour':
                    w.u8(TRANSFORMER_CONTOUR); w.u8(int(round(t['width'])) + 128)
                    w.u8(t.get('join', 0)); w.u8(t.get('miter', 4))
                elif t['type'] == 'perspective':
                    w.u8(TRANSFORMER_PERSPECTIVE)
                    for value in t['matrix']:
                        w.float24(value)
                elif t['type'] == 'stroke':
                    w.u8(TRANSFORMER_STROKE); w.u8(int(round(t['width'])) + 128)
                    w.u8((t.get('join', 0) & 15) | ((t.get('cap', 0) & 15) << 4))
                    w.u8(t.get('miter', 4))
                else:
                    raise ValueError('unknown transformer %r' % t['type'])
    return bytes(w.out)


def normalize_point(point):
    """Accept (x, y) for corners or ((x, y), (xin, yin), (xout, yout))."""
    if len(point) == 2 and not isinstance(point[0], (tuple, list)):
        p = (float(point[0]), float(point[1]))
        return (p, p, p)
    (x, y), (xi, yi), (xo, yo) = point
    return ((float(x), float(y)), (float(xi), float(yi)), (float(xo), float(yo)))


# ---------------------------------------------------------------- helpers

def linear_gradient(start, end, stops):
    """A linear gradient from canvas point start to end. Haiku's gradient space
    runs from x = -64 to x = 64; the matrix maps that span onto the canvas."""
    (ax, ay), (bx, by) = start, end
    cx, cy = (ax + bx) / 2.0, (ay + by) / 2.0
    sx, shy = (bx - cx) / 64.0, (by - cy) / 64.0
    return {'gradient': GRADIENT_LINEAR, 'matrix': [sx, shy, -shy, sx, cx, cy],
            'stops': [(o, tuple(c)) for o, c in stops]}


def radial_gradient(center, radius, stops, ratio=1.0):
    """A circular gradient of the given canvas radius (gradient radius is 64)."""
    s = radius / 64.0
    return {'gradient': GRADIENT_CIRCULAR, 'matrix': [s, 0.0, 0.0, s * ratio, center[0], center[1]],
            'stops': [(o, tuple(c)) for o, c in stops]}


def hex_color(text, alpha=255):
    text = text.lstrip('#')
    return (int(text[0:2], 16), int(text[2:4], 16), int(text[4:6], 16), alpha)


def transform_point(m, x, y):
    return (m[0] * x + m[2] * y + m[4], m[1] * x + m[3] * y + m[5])


# ---------------------------------------------------------------- preview

def flatten(path, matrix=IDENTITY, steps=12):
    points = [normalize_point(p) for p in path['points']]
    result = []
    n = len(points)
    if n == 0:
        return result
    count = n if path.get('closed', True) else n - 1
    for i in range(count):
        p0, _, out0 = points[i]
        p1, in1, _ = points[(i + 1) % n]
        if out0 == p0 and in1 == p1:
            result.append(transform_point(matrix, *p0))
            continue
        for s in range(steps):
            t = s / float(steps)
            mt = 1.0 - t
            x = mt ** 3 * p0[0] + 3 * mt ** 2 * t * out0[0] + 3 * mt * t ** 2 * in1[0] + t ** 3 * p1[0]
            y = mt ** 3 * p0[1] + 3 * mt ** 2 * t * out0[1] + 3 * mt * t ** 2 * in1[1] + t ** 3 * p1[1]
            result.append(transform_point(matrix, x, y))
    if not path.get('closed', True):
        result.append(transform_point(matrix, *points[-1][0]))
    return result


def preview(icon, size=256, background=None):
    from PIL import Image, ImageDraw
    scale = 8
    big = size * scale
    image = Image.new('RGBA', (big, big), background or (0, 0, 0, 0))
    unit = big / 64.0
    for shape in icon['shapes']:
        # Haiku leaves out shapes whose level-of-detail range does not hold the scale (the
        # largest maximum, 4, stands for no limit)
        low, high = shape.get('lod', (0.0, 4.0))
        if size / 64.0 < low or (high < 4.0 and size / 64.0 > high):
            continue
        style = icon['styles'][shape['style']]
        polys = [flatten(icon['paths'][i], shape.get('matrix', IDENTITY)) for i in shape['paths']]
        layer = Image.new('RGBA', (big, big), (0, 0, 0, 0))
        draw = ImageDraw.Draw(layer)
        stroke = next((t for t in shape.get('transformers', []) if t['type'] == 'stroke'), None)
        contour = next((t for t in shape.get('transformers', []) if t['type'] == 'contour'), None)
        if 'gradient' in style:
            fill = _gradient_layer(style, big, unit)
        else:
            fill = style['color']
        mask = Image.new('L', (big, big), 0)
        mdraw = ImageDraw.Draw(mask)
        for poly in polys:
            pts = [(x * unit, y * unit) for x, y in poly]
            if len(pts) < 2:
                continue
            if stroke:
                width = max(1, int(round(stroke['width'] * unit)))
                mdraw.line(pts + ([pts[0]] if icon['paths'][shape['paths'][0]].get('closed', True) else []),
                           fill=255, width=width, joint='curve')
                for x, y in pts:
                    mdraw.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=255)
            else:
                mdraw.polygon(pts, fill=255)
                if contour:
                    width = max(1, int(round(contour['width'] * unit)))
                    mdraw.line(pts + [pts[0]], fill=255, width=width, joint='curve')
                    for x, y in pts:
                        mdraw.ellipse([x - width / 2, y - width / 2, x + width / 2, y + width / 2], fill=255)
        if isinstance(fill, tuple):
            solid = Image.new('RGBA', (big, big), fill)
            layer.paste(solid, (0, 0), mask)
        else:
            layer.paste(fill, (0, 0), mask)
        image = Image.alpha_composite(image, layer)
    return image.resize((size, size), Image.LANCZOS)


def _gradient_layer(style, big, unit):
    from PIL import Image
    m = style['matrix']
    stops = sorted(style['stops'])
    det = m[0] * m[3] - m[1] * m[2]
    if abs(det) < 1e-9:
        return Image.new('RGBA', (big, big), stops[0][1])
    inv = [m[3] / det, -m[1] / det, -m[2] / det, m[0] / det, 0.0, 0.0]
    inv[4] = -(inv[0] * m[4] + inv[2] * m[5])
    inv[5] = -(inv[1] * m[4] + inv[3] * m[5])
    small = 128
    img = Image.new('RGBA', (small, small))
    px = img.load()
    for j in range(small):
        for i in range(small):
            cx, cy = (i + 0.5) * 64.0 / small, (j + 0.5) * 64.0 / small
            gx, gy = transform_point(inv, cx, cy)
            if style['gradient'] == GRADIENT_LINEAR:
                t = (gx + 64.0) / 128.0
            elif style['gradient'] == GRADIENT_CIRCULAR:
                t = math.hypot(gx, gy) / 64.0
            elif style['gradient'] == GRADIENT_DIAMOND:
                t = max(abs(gx), abs(gy)) / 64.0
            elif style['gradient'] == GRADIENT_CONIC:
                t = abs(math.atan2(gy, gx)) / math.pi
            else:
                t = (gx + 64.0) / 128.0
            t = max(0.0, min(1.0, t))
            px[i, j] = _interpolate(stops, t)
    return img.resize((big, big), Image.BILINEAR)


def _interpolate(stops, t):
    if t <= stops[0][0]:
        return stops[0][1]
    for (o0, c0), (o1, c1) in zip(stops, stops[1:]):
        if t <= o1:
            f = 0.0 if o1 == o0 else (t - o0) / (o1 - o0)
            return tuple(int(round(a + (b - a) * f)) for a, b in zip(c0, c1))
    return stops[-1][1]


def dump(icon):
    for i, s in enumerate(icon['styles']):
        print('style %d: %s' % (i, s))
    for i, p in enumerate(icon['paths']):
        print('path %d: closed=%s points=%d' % (i, p['closed'], len(p['points'])))
        for pt in p['points']:
            print('   ', pt)
    for i, s in enumerate(icon['shapes']):
        print('shape %d: %s' % (i, s))


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    action, source = sys.argv[1], sys.argv[2]
    data = open(source, 'rb').read()
    if action == 'dump':
        dump(decode(data))
    elif action == 'roundtrip':
        icon = decode(data)
        again = encode(icon)
        if again == data:
            print('identical (%d bytes)' % len(data))
        else:
            print('differs: %d vs %d bytes' % (len(data), len(again)))
            decode(again)
            print('re-encoded output decodes; compare dumps for detail')
            sys.exit(2)
    elif action == 'preview':
        out = sys.argv[3]
        size = int(sys.argv[4]) if len(sys.argv) > 4 else 256
        preview(decode(data), size).save(out)
        print(out)


# ---------------------------------------------------------------- SVG export

def to_svg(icon, size=64):
    """An SVG rendering of the same shapes (gradients, strokes and contours
    approximated with SVG equivalents) for editing outside Haiku."""
    out = ['<?xml version="1.0" encoding="UTF-8"?>',
           '<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 64 64">' % (size, size),
           '  <defs>']
    for i, style in enumerate(icon['styles']):
        if 'gradient' not in style:
            continue
        m = style['matrix']
        if style['gradient'] == GRADIENT_CIRCULAR:
            out.append('    <radialGradient id="s%d" gradientUnits="userSpaceOnUse" cx="0" cy="0" r="64" gradientTransform="matrix(%s)">'
                       % (i, ' '.join('%.4f' % v for v in m)))
        else:
            out.append('    <linearGradient id="s%d" gradientUnits="userSpaceOnUse" x1="-64" y1="0" x2="64" y2="0" gradientTransform="matrix(%s)">'
                       % (i, ' '.join('%.4f' % v for v in m)))
        for offset, (r, g, b, a) in sorted(style['stops']):
            out.append('      <stop offset="%.3f" stop-color="#%02x%02x%02x" stop-opacity="%.3f"/>' % (offset, r, g, b, a / 255.0))
        out.append('    </radialGradient>' if style['gradient'] == GRADIENT_CIRCULAR else '    </linearGradient>')
    out.append('  </defs>')
    for shape in icon['shapes']:
        style = icon['styles'][shape['style']]
        if 'gradient' in style:
            paint = 'url(#s%d)' % shape['style']
            opacity = ''
        else:
            r, g, b, a = style['color']
            paint = '#%02x%02x%02x' % (r, g, b)
            opacity = ' opacity="%.3f"' % (a / 255.0) if a < 255 else ''
        stroke = next((t for t in shape.get('transformers', []) if t['type'] == 'stroke'), None)
        contour = next((t for t in shape.get('transformers', []) if t['type'] == 'contour'), None)
        d = []
        for index in shape['paths']:
            path = icon['paths'][index]
            points = [normalize_point(p) for p in path['points']]
            if not points:
                continue
            d.append('M %.3f %.3f' % points[0][0])
            n = len(points)
            count = n if path.get('closed', True) else n - 1
            for k in range(count):
                p0, _, o0 = points[k]
                p1, i1, _ = points[(k + 1) % n]
                if o0 == p0 and i1 == p1:
                    d.append('L %.3f %.3f' % p1)
                else:
                    d.append('C %.3f %.3f %.3f %.3f %.3f %.3f' % (o0 + i1 + p1))
            if path.get('closed', True):
                d.append('Z')
        transform = ''
        if not is_identity(shape.get('matrix', IDENTITY)):
            transform = ' transform="matrix(%s)"' % ' '.join('%.4f' % v for v in shape['matrix'])
        if stroke:
            joins = {0: 'miter', 1: 'miter', 2: 'round', 3: 'bevel'}
            caps = {0: 'butt', 1: 'square', 2: 'round'}
            out.append('  <path d="%s" fill="none" stroke="%s" stroke-width="%.2f" stroke-linejoin="%s" stroke-linecap="%s"%s%s/>'
                       % (' '.join(d), paint, stroke['width'], joins.get(stroke.get('join', 0), 'miter'), caps.get(stroke.get('cap', 0), 'butt'), opacity, transform))
        elif contour:
            out.append('  <path d="%s" fill="%s" stroke="%s" stroke-width="%.2f" stroke-linejoin="round"%s%s/>'
                       % (' '.join(d), paint, paint, contour['width'], opacity, transform))
        else:
            out.append('  <path d="%s" fill="%s"%s%s/>' % (' '.join(d), paint, opacity, transform))
    out.append('</svg>')
    return '\n'.join(out) + '\n'
