#!/usr/bin/env python3
"""Converts gfx/icon.svg (flat filled paths) into the HVIF icon of the air/OS
package, so the app keeps the very logo of the other platforms.

    python3 tools/haiku/svg2hvif.py gfx/icon.svg resources/haiku/msga.hvif [preview.png]

Handles what the logo uses: <path fill="#rrggbb" d="..."> with the M/L/H/V/
C/S/Q/T/A/Z commands (absolute and relative). Arcs and quadratics become
cubics; the viewBox maps onto HVIF's 64 x 64 canvas with a small margin.
"""
import math
import re
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
import hvif  # noqa: E402

TOKEN = re.compile(r'[MmLlHhVvCcSsQqTtAaZz]|[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?')


def arc_to_cubics(x1, y1, rx, ry, phi, large, sweep, x2, y2):
    """SVG elliptical arc (endpoint form) as cubic segments (F.6 of SVG 1.1)."""
    if rx == 0 or ry == 0 or (x1, y1) == (x2, y2):
        return [((x1, y1), (x2, y2), (x2, y2))]
    rx, ry = abs(rx), abs(ry)
    cp, sp = math.cos(math.radians(phi)), math.sin(math.radians(phi))
    dx, dy = (x1 - x2) / 2, (y1 - y2) / 2
    x1p, y1p = cp * dx + sp * dy, -sp * dx + cp * dy
    lam = (x1p / rx) ** 2 + (y1p / ry) ** 2
    if lam > 1:
        rx, ry = rx * math.sqrt(lam), ry * math.sqrt(lam)
    num = rx * rx * ry * ry - rx * rx * y1p * y1p - ry * ry * x1p * x1p
    den = rx * rx * y1p * y1p + ry * ry * x1p * x1p
    co = math.sqrt(max(0.0, num / den)) * (-1 if large == sweep else 1)
    cxp, cyp = co * rx * y1p / ry, -co * ry * x1p / rx
    cx = cp * cxp - sp * cyp + (x1 + x2) / 2
    cy = sp * cxp + cp * cyp + (y1 + y2) / 2

    def angle(ux, uy, vx, vy):
        a = math.atan2(ux * vy - uy * vx, ux * vx + uy * vy)
        return a

    t1 = angle(1, 0, (x1p - cxp) / rx, (y1p - cyp) / ry)
    dt = angle((x1p - cxp) / rx, (y1p - cyp) / ry, (-x1p - cxp) / rx, (-y1p - cyp) / ry)
    if not sweep and dt > 0:
        dt -= 2 * math.pi
    elif sweep and dt < 0:
        dt += 2 * math.pi
    n = max(1, int(math.ceil(abs(dt) / (math.pi / 2))))
    step = dt / n
    k = 4 / 3 * math.tan(step / 4)
    out = []

    def point(t):
        x, y = rx * math.cos(t), ry * math.sin(t)
        return cp * x - sp * y + cx, sp * x + cp * y + cy

    def deriv(t):
        x, y = -rx * math.sin(t), ry * math.cos(t)
        return cp * x - sp * y, sp * x + cp * y

    t = t1
    for _ in range(n):
        p0, p3 = point(t), point(t + step)
        d0, d3 = deriv(t), deriv(t + step)
        out.append(((p0[0] + k * d0[0], p0[1] + k * d0[1]),
                    (p3[0] - k * d3[0], p3[1] - k * d3[1]), p3))
        t += step
    out[-1] = (out[-1][0], out[-1][1], (x2, y2))
    return out


def parse(d):
    """Subpaths as (start, [(c1, c2, end), ...], closed)."""
    tokens = TOKEN.findall(d)
    i, cmd = 0, None
    x = y = sx = sy = 0.0
    last_c2 = last_q = None
    subpaths, segs, start = [], None, None

    def num():
        nonlocal i
        v = float(tokens[i])
        i += 1
        return v

    def flag():
        # Arc flags may be packed ("0 01-20"): one digit each.
        nonlocal i
        t = tokens[i]
        if len(t) > 1 and t[0] in '01':
            tokens[i] = t[1:]
            return int(t[0])
        i += 1
        return int(float(t))

    while i < len(tokens):
        if re.match(r'[A-Za-z]', tokens[i]):
            cmd = tokens[i]
            i += 1
            if cmd in 'Zz':
                if segs is not None:
                    if (x, y) != start:
                        segs.append(((x, y), start, start))
                    subpaths.append((start, segs, True))
                    segs = None
                x, y = start if start else (x, y)
                last_c2 = last_q = None
                continue
        rel = cmd.islower()
        ox, oy = (x, y) if rel else (0.0, 0.0)
        c = cmd.upper()
        if c == 'M':
            if segs:
                subpaths.append((start, segs, False))
            x, y = ox + num(), oy + num()
            start, segs = (x, y), []
            cmd = 'l' if rel else 'L'
            last_c2 = last_q = None
            continue
        if segs is None:
            start, segs = (x, y), []
        if c == 'L':
            nx, ny = ox + num(), oy + num()
            segs.append(((x, y), (nx, ny), (nx, ny)))
            last_c2 = last_q = None
        elif c == 'H':
            nx, ny = ox + num() if rel else num(), y
            segs.append(((x, y), (nx, ny), (nx, ny)))
            last_c2 = last_q = None
        elif c == 'V':
            nx, ny = x, (oy + num()) if rel else num()
            segs.append(((x, y), (nx, ny), (nx, ny)))
            last_c2 = last_q = None
        elif c == 'C':
            c1 = (ox + num(), oy + num())
            c2 = (ox + num(), oy + num())
            nx, ny = ox + num(), oy + num()
            segs.append((c1, c2, (nx, ny)))
            last_c2, last_q = c2, None
        elif c == 'S':
            c1 = (2 * x - last_c2[0], 2 * y - last_c2[1]) if last_c2 else (x, y)
            c2 = (ox + num(), oy + num())
            nx, ny = ox + num(), oy + num()
            segs.append((c1, c2, (nx, ny)))
            last_c2, last_q = c2, None
        elif c in 'QT':
            if c == 'Q':
                q = (ox + num(), oy + num())
            else:
                q = (2 * x - last_q[0], 2 * y - last_q[1]) if last_q else (x, y)
            nx, ny = ox + num(), oy + num()
            c1 = (x + 2 / 3 * (q[0] - x), y + 2 / 3 * (q[1] - y))
            c2 = (nx + 2 / 3 * (q[0] - nx), ny + 2 / 3 * (q[1] - ny))
            segs.append((c1, c2, (nx, ny)))
            last_q, last_c2 = q, None
        elif c == 'A':
            rx, ry, phi = num(), num(), num()
            large, sweep = flag(), flag()
            nx, ny = ox + num(), oy + num()
            segs.extend(arc_to_cubics(x, y, rx, ry, phi, large, sweep, nx, ny))
            last_c2 = last_q = None
        else:
            raise ValueError('unsupported path command ' + cmd)
        x, y = nx, ny
    if segs:
        subpaths.append((start, segs, False))
    return subpaths


def to_points(start, segs, transform):
    """HVIF points: (position, in-control, out-control), closed path."""
    t = transform
    pts = [[t(start), t(start), t(start)]]
    for c1, c2, end in segs:
        pts[-1][2] = t(c1)
        pts.append([t(end), t(c2), t(end)])
    # The closing segment ends on the start point: merge them.
    if len(pts) > 1 and math.dist(pts[-1][0], pts[0][0]) < 1e-3:
        pts[0][1] = pts[-1][1]
        pts.pop()
    return [tuple(map(tuple, p)) for p in pts]


def main():
    src, dst = sys.argv[1], sys.argv[2]
    svg = open(src).read()
    vb = [float(v) for v in re.search(r'viewBox="([^"]+)"', svg).group(1).split()]
    margin = 2.0
    scale = (64 - 2 * margin) / max(vb[2], vb[3])
    ox = margin + ((64 - 2 * margin) - vb[2] * scale) / 2
    oy = margin + ((64 - 2 * margin) - vb[3] * scale) / 2

    def transform(p):
        return (round(ox + (p[0] - vb[0]) * scale, 3), round(oy + (p[1] - vb[1]) * scale, 3))

    icon = {'styles': [], 'paths': [], 'shapes': []}
    for fill, d in re.findall(r'<path[^>]*fill="#([0-9a-fA-F]{6})"[^>]*d="([^"]+)"', svg):
        icon['styles'].append({'color': hvif.hex_color(fill)})
        indices = []
        for start, segs, _closed in parse(d):
            points = to_points(start, segs, transform)
            if len(points) > 255:
                raise ValueError('a subpath has more than 255 points')
            indices.append(len(icon['paths']))
            icon['paths'].append({'points': points, 'closed': True})
        icon['shapes'].append({'style': len(icon['styles']) - 1, 'paths': indices})
    data = hvif.encode(icon)
    open(dst, 'wb').write(data)
    print(dst, len(data), 'bytes,', len(icon['paths']), 'paths')
    if len(sys.argv) > 3:
        image = hvif.preview(hvif.decode(data), 256)
        image.save(sys.argv[3])


if __name__ == '__main__':
    main()
