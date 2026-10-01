# -*- coding: utf-8 -*-
"""Tiny deterministic SVG path parser and flattener.  Standard library only.

Why this file exists
--------------------
``make_icons.py`` has to rasterise ``doc/Cicada.svg``, and the machine it runs
on has **no SVG renderer at all**: no cairosvg, no librsvg / rsvg-convert, no
ImageMagick, no Inkscape, and Pillow itself cannot read SVG.  The only way to
get that art onto pixels is to turn the path data into polygons and fill them
with ``PIL.ImageDraw``.  This module does the first half: SVG ``d`` attribute
in, straight line segments out.

Determinism (this is the whole point)
-------------------------------------
Every cubic and quadratic Bezier is flattened with a FIXED subdivision count --
``CURVE_SEGMENTS`` straight segments, sampled at ``t = k / CURVE_SEGMENTS``.
There is no adaptive tolerance, no error-driven recursion and no randomness, so
the same ``d`` string always yields exactly the same points.  That is what
makes the generated icons byte-identical between runs.

Supported commands
------------------
``M m   L l   H h   V v   C c   S s   Q q   T t   Z z``

Deliberately NOT supported: elliptical arcs (``A`` / ``a``).  The source art
uses none, and silently guessing at the arc flag conventions would produce a
wrong glyph rather than an obvious failure.  Unsupported input raises
``SvgPathError``.
"""

from __future__ import annotations

import re
from typing import Iterable, List, Sequence, Tuple

Point = Tuple[float, float]

#: Straight segments used to approximate one cubic or quadratic Bezier.
#: Fixed on purpose (see the module docstring).  16 is far more than enough for
#: this art: even on the tightest curve of ``Cicada.svg`` the chord error is
#: well under a tenth of a pixel at the largest icon we emit (1024 px).
CURVE_SEGMENTS = 16

#: Commands this parser understands.  ``A``/``a`` are listed so that an arc is
#: reported as "unsupported command" instead of being mistaken for garbage.
_COMMANDS = "MmLlHhVvCcSsQqTtZzAa"

_NUMBER_BODY = r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?"
_TOKEN_RE = re.compile(r"[%s]|%s" % (_COMMANDS, _NUMBER_BODY))
_CMD_RE = re.compile(r"[%s]" % _COMMANDS)
_NUMBER_RE = re.compile(_NUMBER_BODY)


class SvgPathError(ValueError):
    """Raised for path data this parser refuses to guess about."""


class Subpath:
    """One ``moveto`` run, already flattened into straight segments.

    ``points`` is a list of ``(x, y)`` pairs in SVG user units; ``closed`` says
    whether the run ended with ``Z``.  Filling does not care about the flag
    (a fill always closes implicitly), but reporting it keeps this honest.
    """

    __slots__ = ("points", "closed")

    def __init__(self, points: Sequence[Point] = (), closed: bool = False):
        self.points: List[Point] = [(float(x), float(y)) for (x, y) in points]
        self.closed: bool = bool(closed)

    def __repr__(self) -> str:  # pragma: no cover - debugging aid only
        return "Subpath(%d points, closed=%s)" % (len(self.points), self.closed)


def _tokenize(d: str) -> List[str]:
    """Split path data into command letters and numbers.

    Anything that is neither (stray characters, a broken exponent, ...) makes
    the function fail loudly instead of silently shifting every later number by
    one -- a class of bug that is very hard to spot in a rendered icon.
    """
    tokens = _TOKEN_RE.findall(d)
    residue = _TOKEN_RE.sub("", d)
    for junk in (",", " ", "\t", "\n", "\r"):
        residue = residue.replace(junk, "")
    if residue:
        raise SvgPathError(
            "unsupported or malformed path data near %r" % (residue[:32],)
        )
    return tokens


def _flatten_cubic(sub: Subpath, p0: Point, p1: Point, p2: Point, p3: Point) -> None:
    """Append a cubic Bezier as ``CURVE_SEGMENTS`` straight segments."""
    for k in range(1, CURVE_SEGMENTS):
        t = k / CURVE_SEGMENTS
        mt = 1.0 - t
        a = mt * mt * mt
        b = 3.0 * mt * mt * t
        c = 3.0 * mt * t * t
        e = t * t * t
        sub.points.append(
            (
                a * p0[0] + b * p1[0] + c * p2[0] + e * p3[0],
                a * p0[1] + b * p1[1] + c * p2[1] + e * p3[1],
            )
        )
    # Land exactly on the endpoint rather than on a floating point near-miss:
    # the source art closes several loops by ending on the start point, and the
    # mask we build later is sensitive to that tangency.
    sub.points.append((p3[0], p3[1]))


def _flatten_quadratic(sub: Subpath, p0: Point, p1: Point, p2: Point) -> None:
    """Append a quadratic Bezier as ``CURVE_SEGMENTS`` straight segments.

    Evaluated directly (no conversion to a cubic), so the point list depends on
    nothing but the numbers in the file.
    """
    for k in range(1, CURVE_SEGMENTS):
        t = k / CURVE_SEGMENTS
        mt = 1.0 - t
        a = mt * mt
        b = 2.0 * mt * t
        c = t * t
        sub.points.append(
            (
                a * p0[0] + b * p1[0] + c * p2[0],
                a * p0[1] + b * p1[1] + c * p2[1],
            )
        )
    sub.points.append((p2[0], p2[1]))


def parse_path(d: str) -> List[Subpath]:
    """Parse an SVG ``d`` attribute into a list of flattened :class:`Subpath`.

    Implicit command repetition is implemented (``M 0 0 1 1`` is a moveto
    followed by a lineto, and ``C`` repeated without the letter keeps drawing
    cubics), as is the reflection rule for ``S`` / ``T``.
    """
    tokens = _tokenize(d)
    total = len(tokens)

    out: List[Subpath] = []
    cur: Point = (0.0, 0.0)
    start: Point = (0.0, 0.0)
    sub: Subpath = None          # type: ignore[assignment]
    after_close = False
    cubic_ctrl: Point = None     # type: ignore[assignment]
    quad_ctrl: Point = None      # type: ignore[assignment]
    prev = None                  # last command actually executed
    i = 0

    def take(count: int) -> List[float]:
        nonlocal i
        if i + count > total:
            raise SvgPathError(
                "path data ends in the middle of a %s command" % (prev or "?",)
            )
        values = [float(t) for t in tokens[i:i + count]]
        i += count
        return values

    while i < total:
        token = tokens[i]
        if _CMD_RE.fullmatch(token):
            cmd = token
            i += 1
            implied = False
        else:
            if prev is None:
                raise SvgPathError("path data must start with a moveto command")
            cmd = prev
            implied = True

        # A repeated moveto becomes a lineto (SVG 1.1, 8.3.2).
        if implied and cmd in ("M", "m"):
            cmd = "L" if cmd == "M" else "l"

        relative = cmd.islower()
        upper = cmd.upper()

        if upper == "Z":
            if sub is None:
                raise SvgPathError("closepath before any moveto")
            sub.closed = True
            cur = start
            after_close = True
            cubic_ctrl = None      # type: ignore[assignment]
            quad_ctrl = None       # type: ignore[assignment]
            prev = cmd
            continue

        if upper == "M":
            x, y = take(2)
            if relative:
                x += cur[0]
                y += cur[1]
            cur = (x, y)
            start = cur
            sub = Subpath([cur])
            out.append(sub)
            after_close = False
            cubic_ctrl = None      # type: ignore[assignment]
            quad_ctrl = None       # type: ignore[assignment]
            prev = cmd
            continue

        # Every remaining command draws, so it needs a current subpath.
        if sub is None:
            raise SvgPathError("drawing command before any moveto")
        if after_close:
            # After Z the current point is the closed subpath's start point and
            # a following drawing command opens a *new* subpath there.
            sub = Subpath([cur])
            out.append(sub)
            after_close = False

        if upper == "L":
            x, y = take(2)
            if relative:
                x += cur[0]
                y += cur[1]
            cur = (x, y)
            sub.points.append(cur)
            cubic_ctrl = None      # type: ignore[assignment]
            quad_ctrl = None       # type: ignore[assignment]
        elif upper == "H":
            (x,) = take(1)
            if relative:
                x += cur[0]
            cur = (x, cur[1])
            sub.points.append(cur)
            cubic_ctrl = None      # type: ignore[assignment]
            quad_ctrl = None       # type: ignore[assignment]
        elif upper == "V":
            (y,) = take(1)
            if relative:
                y += cur[1]
            cur = (cur[0], y)
            sub.points.append(cur)
            cubic_ctrl = None      # type: ignore[assignment]
            quad_ctrl = None       # type: ignore[assignment]
        elif upper == "C":
            x1, y1, x2, y2, x, y = take(6)
            if relative:
                x1 += cur[0]
                y1 += cur[1]
                x2 += cur[0]
                y2 += cur[1]
                x += cur[0]
                y += cur[1]
            _flatten_cubic(sub, cur, (x1, y1), (x2, y2), (x, y))
            cur = (x, y)
            cubic_ctrl = (x2, y2)
            quad_ctrl = None       # type: ignore[assignment]
        elif upper == "S":
            x2, y2, x, y = take(4)
            if relative:
                x2 += cur[0]
                y2 += cur[1]
                x += cur[0]
                y += cur[1]
            if prev is not None and prev.upper() in ("C", "S") and cubic_ctrl:
                x1 = 2.0 * cur[0] - cubic_ctrl[0]
                y1 = 2.0 * cur[1] - cubic_ctrl[1]
            else:
                x1, y1 = cur
            _flatten_cubic(sub, cur, (x1, y1), (x2, y2), (x, y))
            cur = (x, y)
            cubic_ctrl = (x2, y2)
            quad_ctrl = None       # type: ignore[assignment]
        elif upper == "Q":
            x1, y1, x, y = take(4)
            if relative:
                x1 += cur[0]
                y1 += cur[1]
                x += cur[0]
                y += cur[1]
            _flatten_quadratic(sub, cur, (x1, y1), (x, y))
            cur = (x, y)
            quad_ctrl = (x1, y1)
            cubic_ctrl = None      # type: ignore[assignment]
        elif upper == "T":
            x, y = take(2)
            if relative:
                x += cur[0]
                y += cur[1]
            if prev is not None and prev.upper() in ("Q", "T") and quad_ctrl:
                x1 = 2.0 * cur[0] - quad_ctrl[0]
                y1 = 2.0 * cur[1] - quad_ctrl[1]
            else:
                x1, y1 = cur
            _flatten_quadratic(sub, cur, (x1, y1), (x, y))
            cur = (x, y)
            quad_ctrl = (x1, y1)
            cubic_ctrl = None      # type: ignore[assignment]
        else:
            raise SvgPathError(
                "unsupported SVG path command %r (elliptical arcs are not "
                "implemented; the source art uses none)" % (cmd,)
            )

        prev = cmd

    return out


def parse_points(text: str) -> List[Point]:
    """Parse a ``polygon`` / ``polyline`` ``points`` attribute."""
    values = [float(t) for t in _NUMBER_RE.findall(text or "")]
    if len(values) % 2:
        raise SvgPathError(
            "polygon/polyline points need an even number of coordinates"
        )
    return [(values[i], values[i + 1]) for i in range(0, len(values), 2)]


def bbox(subpaths: Iterable[Subpath]) -> Tuple[float, float, float, float]:
    """Axis aligned bounding box of the flattened points, as ``(x0, y0, x1, y1)``."""
    xs: List[float] = []
    ys: List[float] = []
    for sub in subpaths:
        for x, y in sub.points:
            xs.append(x)
            ys.append(y)
    if not xs:
        raise SvgPathError("no points to measure")
    return (min(xs), min(ys), max(xs), max(ys))


def _selftest() -> None:
    """``python svgpath.py`` -- a handful of checks that need no test runner."""
    poly = parse_path("M0,0 L10,0 L10,10 Z")
    assert len(poly) == 1, poly
    assert poly[0].closed is True
    assert poly[0].points == [(0.0, 0.0), (10.0, 0.0), (10.0, 10.0)], poly[0].points

    # "M 0 0 1 1" is a moveto followed by an implicit lineto.
    implied = parse_path("M0,0 10,0 10,10")
    assert len(implied) == 1
    assert len(implied[0].points) == 3, implied[0].points

    # A cubic adds CURVE_SEGMENTS points on top of the moveto point it starts
    # from (15 interior samples plus the exact endpoint).
    cubic = parse_path("M0,0 C0,0 10,0 10,10")
    assert len(cubic[0].points) == 1 + CURVE_SEGMENTS, len(cubic[0].points)
    assert cubic[0].points[0] == (0.0, 0.0)
    assert cubic[0].points[-1] == (10.0, 10.0)

    quadratic = parse_path("M0,0 Q5,10 10,0")
    assert len(quadratic[0].points) == 1 + CURVE_SEGMENTS

    # Relative commands and H/V.
    rel = parse_path("m1,2 h3 v4 z")
    assert rel[0].points == [(1.0, 2.0), (4.0, 2.0), (4.0, 6.0)], rel[0].points
    assert rel[0].closed is True

    # A drawing command after Z starts a new subpath at the closed start point.
    two = parse_path("M0,0 L10,0 Z L5,5")
    assert len(two) == 2, two
    assert two[0].closed is True
    assert two[1].points[0] == (0.0, 0.0), two[1].points

    # Reflection for S: the first control point mirrors the previous one.
    smooth = parse_path("M0,0 C0,10 10,10 10,0 S20,10 20,0")
    assert smooth[0].points[-1] == (20.0, 0.0)

    assert parse_points("0 0.5 35.7569 19.7253") == [
        (0.0, 0.5),
        (35.7569, 19.7253),
    ]

    try:
        parse_path("M0,0 A5,5 0 0 1 10,10")
    except SvgPathError:
        pass
    else:  # pragma: no cover
        raise AssertionError("arcs must be rejected, not guessed at")

    print("svgpath.py self-test passed (CURVE_SEGMENTS=%d)" % CURVE_SEGMENTS)


if __name__ == "__main__":
    _selftest()
