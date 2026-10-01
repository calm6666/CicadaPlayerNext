#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Generate the CicadaPlayerNext application icons for every platform demo.

Run it with Python 3.10+ and Pillow -- nothing else:

    python tools/make_icons/make_icons.py

There is no SVG renderer anywhere in this toolchain (no cairosvg, no librsvg /
rsvg-convert, no ImageMagick, no Inkscape, and Pillow cannot read SVG), so the
generator rasterises the source artwork itself:

    doc/Cicada.svg
      -> svgpath.py flattens every path into straight segments
      -> the nested group transforms are applied to get final user units
      -> each subpath is filled with PIL.ImageDraw.polygon at scale = 8
      -> the 8x coverage masks are combined (union across paths, even-odd
         inside one path, intersection for the SVG mask)
      -> Image.LANCZOS downscale to the target size, then a flat colour is
         applied to the antialiased coverage

Everything is deterministic: no timestamps, no randomness, no adaptive
tolerances, and only integer/fixed-point geometry.  Running the generator twice
produces byte-identical files (PNG and ICO writers in Pillow add no metadata).

See README.md in this directory for the design rules and the platform wiring
that still has to happen outside these image files.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import xml.etree.ElementTree as ET
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

# Running as a script puts this directory on sys.path already; the explicit
# insert also covers `python -m tools.make_icons.make_icons` and frozen runs.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from PIL import Image, ImageChops, ImageDraw  # noqa: E402  (after sys.path fix)

import svgpath  # noqa: E402  (sibling module, see path insert above)


# ===========================================================================
# Locations
# ===========================================================================

HERE = os.path.dirname(os.path.abspath(__file__))
#: Absolute path of this file, so the manifest is stable no matter which
#: directory the generator was invoked from.
THIS_FILE = os.path.abspath(__file__)
#: Repository checkout of CicadaPlayerNext (tools/make_icons -> ../..).
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SOURCE_SVG = os.path.join(REPO, "doc", "Cicada.svg")
DEFAULT_OUT = os.path.join(HERE, "out")

# Existing platform source trees, relative to REPO and always written with
# forward slashes in the report (they are read by humans and by CMake).
ANDROID_RES = "platform/Android/source/paasApp/src/main/res"
HARMONY_ENTRY_MEDIA = "platform/HarmonyOS/entry/src/main/resources/base/media"
HARMONY_APP_MEDIA = "platform/HarmonyOS/AppScope/resources/base/media"
IOS_APPICONSET = (
    "platform/Apple/demo/iOS/CicadaDemo/CicadaDemo/"
    "CicadaResource/Assets.xcassets/AppIcon.appiconset"
)
QT_APPICON_DIR = "platform/QtPlayer/assets/appicon"


# ===========================================================================
# Design rules (see README.md -- these are the numbers the user specified)
# ===========================================================================

#: Glyph colour, straight from doc/Cicada.svg (every rendering path uses it).
GLYPH_COLOR_HEX = "#00C1DE"
GLYPH_COLOR = (0x00, 0xC1, 0xDE)

#: Supersampling factor: draw at N*8, then LANCZOS down to N.  Fixed, so the
#: output does not depend on Pillow's (or anyone's) antialiasing behaviour.
SUPERSAMPLE = 8

#: Rounded-square variants (Android / HarmonyOS / macOS / iOS): the white
#: rounded square fills the whole canvas and the glyph takes this share of the
#: canvas side (longest glyph edge), centred.
CORNER_RADIUS_FRACTION = 0.2237          # 22.37 % of the canvas side
GLYPH_FRACTION_ROUNDED = 0.62            # about 62 %

#: macOS only: the rounded square must NOT fill the canvas.
#:
#: Apple's macOS icon grid puts the icon's body on a smaller square centred in
#: the image, leaving a transparent margin around it.  A macOS icon whose body
#: fills the full canvas therefore looks a notch bigger than every system and
#: third-party icon next to it in the Dock -- which is exactly the bug this
#: constant fixes ("macOS 上 Qt 程序图标比别的图标大了一圈").
#:
#: The value is Apple's published macOS icon geometry: the body occupies
#: 824 x 824 of the 1024 x 1024 canvas, i.e. 0.8046875 of the side, centred
#: (100 px transparent margin on each side).  The corner radius stays 22.37 %
#: *of the body*, so it stays proportional as the body shrinks.
MACOS_CONTENT_FRACTION = 824.0 / 1024.0

#: The glyph is scaled with the body so its relative size inside the white
#: square is unchanged on macOS (0.62 * 0.8046875).
MACOS_GLYPH_FRACTION = GLYPH_FRACTION_ROUNDED * MACOS_CONTENT_FRACTION

#: Flat variants (Windows / Linux): no background, glyph only.
GLYPH_FRACTION_FLAT = 0.92               # about 92 %

#: Resolution used by the one-off "measure the ink" pass, in pixels per SVG
#: user unit.  Only the glyph's tight bounding box comes out of it (a few
#: thousand pixels per side), so this is cheap.
MEASURE_PX_PER_UNIT = 16

#: `Image.LANCZOS` moved under `Image.Resampling` in Pillow 9.1.  Both spellings
#: are still accepted today, but prefer the modern one.
_RESAMPLING = getattr(Image, "Resampling", None)
LANCZOS = getattr(_RESAMPLING, "LANCZOS", None) or Image.LANCZOS


# ===========================================================================
# Artifact tables
# ===========================================================================

#: Android launcher icon densities.  These are the five `mipmap-*` folders that
#: actually exist under ANDROID_RES in this repository.
ANDROID_DENSITIES: Tuple[Tuple[str, int], ...] = (
    ("mdpi", 48),
    ("hdpi", 72),
    ("xhdpi", 96),
    ("xxhdpi", 144),
    ("xxxhdpi", 192),
)
ANDROID_ICON_NAMES = ("ic_launcher.png", "ic_launcher_round.png")

#: Android adaptive icon (API 26+) foreground bitmap.
#:
#: Why this exists: on API 26 and later the `mipmap-anydpi-v26/ic_launcher.xml`
#: adaptive-icon resource WINS over the density PNGs above, so regenerating only
#: `ic_launcher.png` leaves modern devices showing whatever the adaptive icon
#: points at -- in this repository the stock Android Studio template (a teal
#: #26A69A square with a white grid).  The generator therefore also emits a
#: white background and a transparent Cicada glyph for the foreground.
#:
#: Sizing: an adaptive icon is a 108 x 108 dp canvas of which only the central
#: 72 x 72 dp is guaranteed visible (the outer ring is masked and parallaxed).
#: So the 62 % glyph fills 0.62 * 108 = 67 dp, comfortably inside the 72 dp safe
#: zone, and we render the bitmap at 432 px = 108 dp * 4 (xxxhdpi).
ANDROID_ADAPTIVE_FOREGROUND_PX = 432
ANDROID_ADAPTIVE_BACKGROUND = "drawable/ic_launcher_background"
ANDROID_ADAPTIVE_FOREGROUND = "drawable/ic_launcher_foreground"

#: iOS AppIcon.appiconset.  The classic Xcode default file set; the fifteen
#: names below cover all eighteen slots of the Contents.json that is already in
#: the repository (several slots reuse the same file, exactly like Xcode does).
IOS_APPICON_FILES: Tuple[Tuple[str, int], ...] = (
    ("icon-20.png", 20),
    ("icon-20@2x.png", 40),
    ("icon-20@3x.png", 60),
    ("icon-29.png", 29),
    ("icon-29@2x.png", 58),
    ("icon-29@3x.png", 87),
    ("icon-40.png", 40),
    ("icon-40@2x.png", 80),
    ("icon-40@3x.png", 120),
    ("icon-60@2x.png", 120),
    ("icon-60@3x.png", 180),
    ("icon-76.png", 76),
    ("icon-76@2x.png", 152),
    ("icon-83.5@2x.png", 167),
    ("icon-1024.png", 1024),
)

#: iOS Contents.json, in the order the file already uses in this repository.
#: (idiom, logical size, scale, filename)
IOS_CONTENTS_ENTRIES: Tuple[Tuple[str, str, str, str], ...] = (
    ("iphone", "20x20", "2x", "icon-20@2x.png"),
    ("iphone", "20x20", "3x", "icon-20@3x.png"),
    ("iphone", "29x29", "2x", "icon-29@2x.png"),
    ("iphone", "29x29", "3x", "icon-29@3x.png"),
    ("iphone", "40x40", "2x", "icon-40@2x.png"),
    ("iphone", "40x40", "3x", "icon-40@3x.png"),
    ("iphone", "60x60", "2x", "icon-60@2x.png"),
    ("iphone", "60x60", "3x", "icon-60@3x.png"),
    ("ipad", "20x20", "1x", "icon-20.png"),
    ("ipad", "20x20", "2x", "icon-20@2x.png"),
    ("ipad", "29x29", "1x", "icon-29.png"),
    ("ipad", "29x29", "2x", "icon-29@2x.png"),
    ("ipad", "40x40", "1x", "icon-40.png"),
    ("ipad", "40x40", "2x", "icon-40@2x.png"),
    ("ipad", "76x76", "1x", "icon-76.png"),
    ("ipad", "76x76", "2x", "icon-76@2x.png"),
    ("ipad", "83.5x83.5", "2x", "icon-83.5@2x.png"),
    ("ios-marketing", "1024x1024", "1x", "icon-1024.png"),
)

#: macOS `.iconset`, exactly the ten file names `iconutil` insists on.
MACOS_ICONSET_FILES: Tuple[Tuple[str, int], ...] = (
    ("icon_16x16.png", 16),
    ("icon_16x16@2x.png", 32),
    ("icon_32x32.png", 32),
    ("icon_32x32@2x.png", 64),
    ("icon_128x128.png", 128),
    ("icon_128x128@2x.png", 256),
    ("icon_256x256.png", 256),
    ("icon_256x256@2x.png", 512),
    ("icon_512x512.png", 512),
    ("icon_512x512@2x.png", 1024),
)

WINDOWS_ICO_SIZES: Tuple[int, ...] = (16, 24, 32, 48, 64, 128, 256)
LINUX_PNG_SIZES: Tuple[int, ...] = (16, 24, 32, 48, 64, 128, 256, 512)

#: Nothing in the repository fixes a Linux icon name yet (there is no .desktop
#: file and no `Icon=` anywhere), so this is a *proposal* documented in the
#: report -- it matches the .desktop file the generator also emits.
LINUX_ICON_NAME = "CicadaPlayer"

PLATFORMS = ("android", "harmonyos", "ios", "macos", "windows", "linux")

#: The small shell wrapper for Apple's own .icns packer.  Written verbatim to
#: both the out/ tree and (unless disabled) the QtPlayer asset directory.
MAKE_ICNS_SH = """#!/bin/sh
#
# Cicada.icns is the *rounded-white* macOS icon.  Pillow cannot write a real
# macOS icon family, so the .iconset directory next to this script is the
# source of truth and this wrapper feeds it to Apple's own tool.
#
# MUST BE RUN ON macOS (iconutil ships with Xcode / the command line tools):
#
#     cd platform/QtPlayer/assets/appicon
#     ./make_icns.sh
#
# It reads  ./Cicada.iconset   and writes  ./Cicada.icns
# (both paths are resolved relative to this script, so any cwd works)
set -eu

here=$(cd "$(dirname "$0")" && pwd)
iconset="$here/Cicada.iconset"
target="$here/Cicada.icns"

if [ ! -d "$iconset" ]; then
    echo "make_icns.sh: $iconset is missing -- run" >&2
    echo "    python tools/make_icons/make_icons.py" >&2
    echo "first (it generates the .iconset PNGs)." >&2
    exit 1
fi

if ! command -v iconutil >/dev/null 2>&1; then
    echo "make_icns.sh: iconutil not found -- this script only runs on macOS" >&2
    exit 1
fi

# -c icns asks for the modern (macOS 10.7+) icon family; -o is the output file.
iconutil -c icns "$iconset" -o "$target"
echo "wrote $target"
"""


# ===========================================================================
# Small geometry helpers (SVG transform lists)
#
# A matrix is the usual 2x3 affine (a, b, c, d, e, f):
#     x' = a*x + c*y + e
#     y' = b*x + d*y + f
# ===========================================================================

IDENTITY: Tuple[float, float, float, float, float, float] = (1.0, 0.0, 0.0, 1.0, 0.0, 0.0)

_TRANSFORM_RE = re.compile(r"([A-Za-z]+)\s*\(([^)]*)\)")
_NUMBER_RE = re.compile(r"[-+]?(?:\d+\.?\d*|\.\d+)(?:[eE][-+]?\d+)?")
_URL_REF_RE = re.compile(r"url\(\s*['\"]?#([^)'\"]+)['\"]?\s*\)")


class IconError(RuntimeError):
    """Raised when the source art is not what this generator expects."""


def mat_mul(outer, inner):
    """Matrix that applies `inner` first and `outer` second.

    With this convention an SVG transform *list* is folded left to right
    (`translate(...) scale(...)` puts the translate on the outside, matching
    the spec) and nested groups fold the same way.
    """
    a1, b1, c1, d1, e1, f1 = outer
    a2, b2, c2, d2, e2, f2 = inner
    return (
        a2 * a1 + b2 * c1,
        a2 * b1 + b2 * d1,
        c2 * a1 + d2 * c1,
        c2 * b1 + d2 * d1,
        e2 * a1 + f2 * c1 + e1,
        e2 * b1 + f2 * d1 + f1,
    )


def mat_apply(matrix, point):
    a, b, c, d, e, f = matrix
    x, y = point
    return (a * x + c * y + e, b * x + d * y + f)


def transform_points(points, matrix):
    a, b, c, d, e, f = matrix
    return [(x * a + y * c + e, x * b + y * d + f) for (x, y) in points]


def parse_transform(text):
    """Parse an SVG `transform` attribute (translate/scale/matrix/rotate)."""
    if not text:
        return IDENTITY
    result = IDENTITY
    found = False
    for name, arguments in _TRANSFORM_RE.findall(text):
        found = True
        values = [float(t) for t in _NUMBER_RE.findall(arguments)]
        key = name.lower()
        if key == "translate":
            if not values:
                raise IconError("translate() without arguments")
            tx = values[0]
            ty = values[1] if len(values) > 1 else 0.0
            local = (1.0, 0.0, 0.0, 1.0, tx, ty)
        elif key == "scale":
            if not values:
                raise IconError("scale() without arguments")
            sx = values[0]
            sy = values[1] if len(values) > 1 else sx
            local = (sx, 0.0, 0.0, sy, 0.0, 0.0)
        elif key == "matrix":
            if len(values) != 6:
                raise IconError("matrix() needs exactly six numbers")
            local = tuple(values)  # type: ignore[assignment]
        elif key == "rotate":
            if not values:
                raise IconError("rotate() without arguments")
            import math

            angle = math.radians(values[0])
            cos_a = math.cos(angle)
            sin_a = math.sin(angle)
            spin = (cos_a, sin_a, -sin_a, cos_a, 0.0, 0.0)
            if len(values) >= 3:
                cx, cy = values[1], values[2]
                local = mat_mul(
                    mat_mul((1.0, 0.0, 0.0, 1.0, cx, cy), spin),
                    (1.0, 0.0, 0.0, 1.0, -cx, -cy),
                )
            else:
                local = spin
        else:
            raise IconError(
                "unsupported SVG transform %r (only translate/scale/matrix/"
                "rotate are implemented; the source art uses translate)" % (name,)
            )
        result = mat_mul(result, local)
    if not found and text.strip():
        raise IconError("could not parse transform %r" % (text,))
    return result


# ===========================================================================
# Reading doc/Cicada.svg
# ===========================================================================

#: Elements that are never painted directly.  `<mask>` is skipped because its
#: content is consumed through `mask="url(#...)"` on the element it applies to.
_SKIP_TAGS = {
    "defs",
    "mask",
    "clippath",
    "symbol",
    "title",
    "desc",
    "metadata",
    "style",
    "lineargradient",
    "radialgradient",
    "pattern",
    "filter",
    "marker",
}


class Shape:
    """One painted shape, already in final SVG user units."""

    __slots__ = ("polylines", "clip", "fill", "even_odd")

    def __init__(self, polylines, clip, fill, even_odd):
        self.polylines: List[svgpath.Subpath] = polylines
        self.clip: Optional[List[svgpath.Subpath]] = clip
        self.fill: str = fill
        #: True when the inherited `fill-rule` is `evenodd`.  This matters a
        #: lot for this artwork -- see `extract_geometry`.
        self.even_odd: bool = even_odd


def _local_name(tag: str) -> str:
    return tag.rsplit("}", 1)[-1]


def _attr(elem, name, default=None):
    """Read an attribute by local name (ElementTree keeps the xlink prefix)."""
    for key, value in elem.attrib.items():
        if key == name or key.rsplit("}", 1)[-1] == name:
            return value
    return default


def _num(value, default=0.0):
    if value is None:
        return default
    try:
        return float(value)
    except (TypeError, ValueError):
        return default


def _is_none_fill(value) -> bool:
    return (value or "").strip().lower() == "none"


def _rect_points(elem):
    x = _num(elem.get("x"), 0.0)
    y = _num(elem.get("y"), 0.0)
    w = _num(elem.get("width"), 0.0)
    h = _num(elem.get("height"), 0.0)
    if w <= 0.0 or h <= 0.0:
        return []
    if elem.get("rx") or elem.get("ry"):
        raise IconError(
            "rounded <rect> is not supported; the source art uses a <polygon> "
            "for its clip rectangle"
        )
    return [(x, y), (x + w, y), (x + w, y + h), (x, y + h)]


def _mask_polylines(elem, ctm, ids, depth=0):
    """Collect the *keep* region of an SVG mask, in final user units.

    SVG renders mask content in the user space of the element that references
    the mask, so `ctm` here is the referencing element's matrix (not the mask
    element's ancestors).  Every painted shape counts as "opaque"; the source
    mask is a plain white polygon, which is the only case this generator needs.
    """
    tag = _local_name(elem.tag)
    key = tag.lower()
    if key in ("title", "desc", "metadata"):
        return []

    ctm = mat_mul(ctm, parse_transform(elem.get("transform")))

    if key in ("mask", "clippath", "g", "svg", "a"):
        collected: List[svgpath.Subpath] = []
        for child in elem:
            collected.extend(_mask_polylines(child, ctm, ids, depth))
        return collected

    if key == "path":
        data = elem.get("d")
        if not data:
            return []
        return [
            svgpath.Subpath(transform_points(sub.points, ctm), sub.closed)
            for sub in svgpath.parse_path(data)
        ]

    if key == "polygon":
        points = svgpath.parse_points(elem.get("points") or "")
        if len(points) < 3:
            return []
        return [svgpath.Subpath(transform_points(points, ctm), True)]

    if key == "polyline":
        points = svgpath.parse_points(elem.get("points") or "")
        if len(points) < 3:
            return []
        return [svgpath.Subpath(transform_points(points, ctm), False)]

    if key == "rect":
        points = _rect_points(elem)
        if len(points) < 3:
            return []
        return [svgpath.Subpath(transform_points(points, ctm), True)]

    if key == "use":
        if depth > 8:
            raise IconError("<use> nesting is too deep (cycle?)")
        target = ids.get(_href_id(_attr(elem, "href")))
        if target is None:
            return []
        moved = mat_mul(
            ctm,
            (
                1.0,
                0.0,
                0.0,
                1.0,
                _num(elem.get("x"), 0.0),
                _num(elem.get("y"), 0.0),
            ),
        )
        return _mask_polylines(target, moved, ids, depth + 1)

    return []


def _href_id(href):
    if not href:
        return None
    return href.strip().lstrip("#")


def _resolve_mask(elem, ctm, ids):
    """Return the clip polylines for `mask="url(#id)"`, or None."""
    value = _attr(elem, "mask")
    if not value:
        return None
    match = _URL_REF_RE.match(value.strip())
    if not match:
        raise IconError("unsupported mask reference %r" % (value,))
    mask_id = match.group(1)
    mask_elem = ids.get(mask_id)
    if mask_elem is None:
        raise IconError("mask %r is referenced but not defined" % (mask_id,))
    polylines = _mask_polylines(mask_elem, ctm, ids)
    if not polylines:
        raise IconError("mask %r resolves to no geometry" % (mask_id,))
    return polylines


def _walk(elem, ctm, fill, fill_rule, ids, out):
    tag = _local_name(elem.tag)
    key = tag.lower()
    if key in _SKIP_TAGS:
        return

    ctm = mat_mul(ctm, parse_transform(elem.get("transform")))
    fill = elem.get("fill", fill)
    fill_rule = elem.get("fill-rule", fill_rule)
    even_odd = (fill_rule or "").strip().lower() == "evenodd"

    if key in ("g", "svg", "a", "switch"):
        for child in elem:
            _walk(child, ctm, fill, fill_rule, ids, out)
        return

    if key == "path":
        data = elem.get("d")
        if not data or _is_none_fill(fill):
            return
        polylines = [
            svgpath.Subpath(transform_points(sub.points, ctm), sub.closed)
            for sub in svgpath.parse_path(data)
        ]
        out.append(Shape(polylines, _resolve_mask(elem, ctm, ids), fill, even_odd))
        return

    if key == "polygon":
        points = svgpath.parse_points(elem.get("points") or "")
        if len(points) < 3 or _is_none_fill(fill):
            return
        out.append(
            Shape(
                [svgpath.Subpath(transform_points(points, ctm), True)],
                _resolve_mask(elem, ctm, ids),
                fill,
                even_odd,
            )
        )
        return

    if key == "polyline":
        points = svgpath.parse_points(elem.get("points") or "")
        if len(points) < 3 or _is_none_fill(fill):
            return
        out.append(
            Shape(
                [svgpath.Subpath(transform_points(points, ctm), False)],
                _resolve_mask(elem, ctm, ids),
                fill,
                even_odd,
            )
        )
        return

    if key == "rect":
        points = _rect_points(elem)
        if len(points) < 3 or _is_none_fill(fill):
            return
        out.append(
            Shape(
                [svgpath.Subpath(transform_points(points, ctm), True)],
                _resolve_mask(elem, ctm, ids),
                fill,
                even_odd,
            )
        )
        return

    if key == "use":
        target = ids.get(_href_id(_attr(elem, "href")))
        if target is None:
            raise IconError("<use> points at an unknown id")
        moved = mat_mul(
            ctm,
            (
                1.0,
                0.0,
                0.0,
                1.0,
                _num(elem.get("x"), 0.0),
                _num(elem.get("y"), 0.0),
            ),
        )
        _walk(target, moved, fill, fill_rule, ids, out)
        return


def extract_geometry(svg_path=SOURCE_SVG):
    """Parse the source SVG into a list of :class:`Shape` in final user units.

    Two details of this particular file are load bearing and worth spelling
    out, because getting either wrong still produces a *plausible* icon:

    * ``<g id="画板" ... fill-rule="evenodd">`` -- `fill-rule` is an inherited
      property, so it is `evenodd` for every descendant.  The path that draws
      the cicada body also contains the round-cornered play triangle as a
      second subpath, and the triangle sits *strictly inside* the body loop.
      With `evenodd` the triangle is knocked out of the body; with the default
      `nonzero` it would be swallowed and the play glyph would disappear.
      This generator therefore implements the even-odd rule.
    * ``<path id="Fill-1" mask="url(#mask-2)">`` -- the only masked path.  The
      mask is `<polygon id="path-1">`, i.e. the Sketch artboard rectangle, and
      it is applied by intersecting coverage masks.
    """
    root = ET.parse(svg_path).getroot()

    ids: Dict[str, object] = {}
    for elem in root.iter():
        elem_id = elem.get("id")
        if elem_id and elem_id not in ids:
            ids[elem_id] = elem

    shapes: List[Shape] = []
    # `fill` starts as black and `fill-rule` as nonzero: the SVG initial
    # values.  The outer group of this file immediately overrides both.
    _walk(root, IDENTITY, "black", "nonzero", ids, shapes)

    if not shapes:
        raise IconError("no filled shapes found in %s" % (svg_path,))

    expected = GLYPH_COLOR_HEX.upper()
    for index, shape in enumerate(shapes):
        if shape.fill.strip().upper() != expected:
            raise IconError(
                "shape %d uses fill %r, but the generator assumes the glyph "
                "colour %s (update GLYPH_COLOR_HEX if the art changed)"
                % (index, shape.fill, GLYPH_COLOR_HEX)
            )
    return shapes


# ===========================================================================
# Rasterisation
# ===========================================================================

def fill_polylines(polylines, size, matrix):
    """Fill `polylines` into a new `L` coverage image of `size` x `size`.

    Coverage is binary (0 or 255) on purpose: ImageDraw does no antialiasing,
    and the antialiasing comes from rasterising at `size = target * 8` and
    downscaling with LANCZOS.  Keeping the masks binary also makes the combine
    steps below exact (a masked paste really is an OR, a difference really is
    an XOR).
    """
    image = Image.new("L", (size, size), 0)
    draw = ImageDraw.Draw(image)
    for poly in polylines:
        points = transform_points(poly.points, matrix)
        if len(points) >= 3:
            draw.polygon(points, fill=255)
    del draw
    return image


def rasterise_shape(shape, size, matrix, force_union=False, skip_clip=False):
    """Rasterise one :class:`Shape`, honouring its fill rule and mask clip."""
    if shape.even_odd and not force_union and len(shape.polylines) > 1:
        # Even-odd over several non-self-intersecting subpaths is exactly the
        # XOR of their individual even-odd fills (parity of the crossing count
        # adds up mod 2), and ImageChops.difference on binary masks is XOR.
        layer = fill_polylines(shape.polylines[:1], size, matrix)
        for poly in shape.polylines[1:]:
            piece = fill_polylines([poly], size, matrix)
            layer = ImageChops.difference(layer, piece)
            del piece
    else:
        # Nonzero (and the single-subpath even-odd case): the union.  Paint
        # order does not matter because every shape is the same flat colour.
        layer = fill_polylines(shape.polylines, size, matrix)

    if shape.clip and not skip_clip:
        clip = fill_polylines(shape.clip, size, matrix)
        layer = ImageChops.multiply(layer, clip)
        del clip
    return layer


def render_glyph(shapes, size, matrix, force_union=False):
    """Union of every shape's coverage -- the glyph silhouette."""
    canvas = Image.new("L", (size, size), 0)
    for shape in shapes:
        layer = rasterise_shape(shape, size, matrix, force_union=force_union)
        # Layers are binary, so pasting through the layer as a mask is an
        # in-place OR -- and it avoids a second full size allocation.
        canvas.paste(255, (0, 0), layer)
        del layer
    return canvas


def union_bbox(shapes):
    """Bounding box of every flattened point, in final user units."""
    xs: List[float] = []
    ys: List[float] = []
    for shape in shapes:
        for poly in shape.polylines:
            for x, y in poly.points:
                xs.append(x)
                ys.append(y)
    if not xs:
        raise IconError("the source SVG produced no geometry")
    return (min(xs), min(ys), max(xs), max(ys))


def measure_ink_box(shapes):
    """Tight bounding box of the *visible* ink, in final user units.

    Rendering once at MEASURE_PX_PER_UNIT and asking for the non-zero bounding
    box is both simpler and more faithful than unioning the path boxes: it
    automatically accounts for the mask clip and for curves that never reach
    their control points.
    """
    x0, y0, x1, y1 = union_bbox(shapes)
    # One unit of slack so ink can never be cropped by the measuring canvas.
    x0 -= 1.0
    y0 -= 1.0
    x1 += 1.0
    y1 += 1.0

    scale = float(MEASURE_PX_PER_UNIT)
    width = max(1, int(round((x1 - x0) * scale)))
    height = max(1, int(round((y1 - y0) * scale)))
    matrix = (scale, 0.0, 0.0, scale, -x0 * scale, -y0 * scale)

    # render_glyph only produces square canvases; the extra margin is harmless
    # because the ink box is read back out of the mask.
    probe = render_glyph(shapes, max(width, height), matrix)

    box = probe.getbbox()
    del probe
    if box is None:
        raise IconError(
            "the glyph rendered to nothing -- the mask/clip or the fill rule "
            "is probably wrong"
        )
    # getbbox() returns an exclusive lower-right corner, so the outer edge of
    # the last inked pixel is what we want (an over-estimate of well under one
    # measured pixel, i.e. < 1/16 user unit).
    return (
        x0 + box[0] / scale,
        y0 + box[1] / scale,
        x0 + box[2] / scale,
        y0 + box[3] / scale,
    )


def glyph_matrix(ink_box, canvas, fraction):
    """Uniform scale + centring that puts the glyph box at `fraction` of `canvas`.

    "The glyph occupies about N % of the canvas side" is read as: the glyph's
    *longest* edge spans `fraction * canvas`, the aspect ratio is preserved, and
    the resulting box is centred.
    """
    x0, y0, x1, y1 = ink_box
    width = x1 - x0
    height = y1 - y0
    extent = max(width, height)
    if extent <= 0.0:
        raise IconError("the glyph box is empty")
    scale = (fraction * canvas) / extent
    offset_x = (canvas - width * scale) / 2.0 - x0 * scale
    offset_y = (canvas - height * scale) / 2.0 - y0 * scale
    return (scale, 0.0, 0.0, scale, offset_x, offset_y)


def draw_rounded_square(image, radius, inset=0):
    """Fill `image` (mode L) with a rounded square.

    `inset` (in pixels) shrinks the square symmetrically towards the centre,
    leaving a transparent margin.  macOS needs this: its icon grid places the
    body on a smaller centred square (see MACOS_CONTENT_FRACTION), and an icon
    whose body fills the whole canvas renders visibly larger than every other
    icon in the Dock.
    """
    size = image.size[0]
    left = int(round(inset))
    right = size - 1 - left

    if right <= left:
        return

    draw = ImageDraw.Draw(image)
    if hasattr(draw, "rounded_rectangle"):
        draw.rounded_rectangle((left, left, right, right), radius=radius, fill=255)
        del draw
        return
    # Fallback for very old Pillow: two rectangles plus four quarter discs.
    r = int(round(radius))
    draw.rectangle((left + r, left, right - r, right), fill=255)
    draw.rectangle((left, left + r, right, right - r), fill=255)
    d = 2 * r
    draw.pieslice((left, left, left + d, left + d), 180, 270, fill=255)
    draw.pieslice((right - d, left, right, left + d), 270, 360, fill=255)
    draw.pieslice((left, right - d, left + d, right), 90, 180, fill=255)
    draw.pieslice((right - d, right - d, right, right), 0, 90, fill=255)
    del draw


def render_icon(shapes, ink_box, size, variant, fraction=None, content_fraction=1.0):
    """Render one icon at `size` x `size`.

    `variant` is "rounded" (white rounded square, glyph at 62 %) or "flat"
    (transparent background, glyph at 92 %).  `fraction`, when given, overrides
    the glyph size fraction the variant would otherwise pick -- the Android
    adaptive foreground needs a transparent background at the 62 % size.

    `content_fraction` shrinks the whole rounded-square body towards the centre,
    leaving a transparent margin.  macOS needs it (see MACOS_CONTENT_FRACTION):
    Apple's icon grid places the body on a smaller centred square, so a body that
    fills the canvas looks bigger than every neighbouring icon in the Dock.

    Memory note: the supersampled coverage images are `size * 8` squared, so
    the 1024 px iOS marketing icon works on 8192 x 8192 8-bit masks (about
    67 MB each, ~270 MB peak).  Nothing bigger is generated.
    """
    if variant not in ("rounded", "flat"):
        raise IconError("unknown icon variant %r" % (variant,))
    rounded = variant == "rounded"
    if content_fraction <= 0.0 or content_fraction > 1.0:
        raise IconError("content_fraction must be in (0, 1], got %r" % (content_fraction,))
    if fraction is None:
        fraction = GLYPH_FRACTION_ROUNDED if rounded else GLYPH_FRACTION_FLAT

    # Shrinking the body must shrink the glyph with it, otherwise the glyph
    # would grow *relative* to the white square it sits on.
    fraction = fraction * content_fraction

    canvas = size * SUPERSAMPLE
    matrix = glyph_matrix(ink_box, canvas, fraction)

    coverage = render_glyph(shapes, canvas, matrix)
    if coverage.getbbox() is None:
        raise IconError(
            "glyph is empty at %dx%d (%s) -- refusing to write a blank icon"
            % (size, size, variant)
        )
    coverage = coverage.resize((size, size), LANCZOS)

    if not rounded:
        # Transparent background: the glyph colour straight over the coverage.
        flat = Image.new("RGBA", (size, size), GLYPH_COLOR + (0,))
        flat.putalpha(coverage)
        del coverage
        return flat

    # The white rounded-square body is drawn at TARGET size rather than
    # supersampled: `rounded_rectangle` already antialiases, whereas rendering at
    # 8x and downscaling spreads each edge over ~4 px as LANCZOS ringing (a row of
    # near-zero alphas like 0,0,1,0,17,238 before the solid edge).  That ringing
    # makes the body measure ~3 px wider than it is, which is exactly the kind of
    # slop the macOS grid is meant to avoid.  The GLYPH is still supersampled,
    # because its curves genuinely need the coverage mask.
    square = Image.new("L", (size, size), 0)
    # The corner radius stays proportional to the *body*, so shrinking the body
    # for macOS keeps the same visual roundness.
    body_side = size * content_fraction
    draw_rounded_square(square, CORNER_RADIUS_FRACTION * body_side,
                        inset=(size - body_side) / 2.0)

    # Compositing is linear in colour, so "blend white towards the glyph by the
    # glyph coverage, then set alpha from the rounded square" is identical to
    # compositing at 8x and downscaling -- and it uses target-size images only.
    white = Image.new("L", (size, size), 255)
    channels = []
    for value in GLYPH_COLOR:
        solid = Image.new("L", (size, size), value)
        channels.append(Image.composite(solid, white, coverage))
    channels.append(square)
    result = Image.merge("RGBA", channels)
    del coverage, square, white, channels
    return result


# ===========================================================================
# Artifact plan
# ===========================================================================

class Artifact:
    """One PNG the generator knows how to produce."""

    __slots__ = ("platform", "out_rel", "size", "variant", "dest_rel", "note", "fraction",
                 "content_fraction")

    def __init__(self, platform, out_rel, size, variant, dest_rel=None, note="", fraction=None,
                 content_fraction=1.0):
        self.platform = platform
        self.out_rel = out_rel.replace("\\", "/")
        self.size = size
        self.variant = variant
        self.dest_rel = dest_rel.replace("\\", "/") if dest_rel else None
        self.note = note
        # Overrides the variant's default glyph fraction.  Used by the Android
        # adaptive foreground, which needs the plain 62 % glyph rather than the
        # 92 % the "flat" variant would give it.
        self.fraction = fraction
        # Shrinks the rounded-square body towards the centre, leaving a
        # transparent margin.  macOS needs it (Apple's icon grid); everything
        # else fills the canvas.
        self.content_fraction = content_fraction


def build_plan():
    """Every PNG artifact, in a deterministic order (small sizes first)."""
    plan: List[Artifact] = []

    # --- Android: rounded-white launcher icons, five densities ------------
    for density, px in ANDROID_DENSITIES:
        for name in ANDROID_ICON_NAMES:
            plan.append(
                Artifact(
                    "android",
                    "android/%s/%s" % (density, name),
                    px,
                    "rounded",
                    "%s/mipmap-%s/%s" % (ANDROID_RES, density, name),
                )
            )

    # --- Android: adaptive-icon foreground (API 26+) ----------------------
    # Written to drawable-nodpi: that density-less folder is exactly the right
    # home for an adaptive foreground, because the adaptive icon's own 108 dp
    # canvas already defines the rendered size and Android must NOT rescale the
    # bitmap by density on top of that.
    plan.append(
        Artifact(
            "android",
            "android/ic_launcher_foreground.png",
            ANDROID_ADAPTIVE_FOREGROUND_PX,
            "flat",
            "%s/drawable-nodpi/ic_launcher_foreground.png" % ANDROID_RES,
            "adaptive-icon foreground (transparent, centred 62 %% glyph)",
            fraction=GLYPH_FRACTION_ROUNDED,
        )
    )

    # --- HarmonyOS: entry ability icon + AppScope app icon ----------------
    # Real names found by walking platform/HarmonyOS and its media/ resource
    # directories:
    #   entry/src/main/resources/base/media/icon.png      ($media:icon)
    #   AppScope/resources/base/media/app_icon.png        ($media:app_icon)
    # There is no startIcon.png / startWindowIcon.png in the tree, so the
    # 114 px variants are emitted to out/ only and reported.
    plan.append(
        Artifact("harmonyos", "harmonyos/icon.png", 216, "rounded",
                 "%s/icon.png" % HARMONY_ENTRY_MEDIA)
    )
    plan.append(
        Artifact("harmonyos", "harmonyos/icon-114.png", 114, "rounded", None,
                 "114 px variant of the ability icon; no matching file exists")
    )
    plan.append(
        Artifact("harmonyos", "harmonyos/startIcon.png", 114, "rounded", None,
                 "candidate name for a distinct start-window icon; not in the tree")
    )
    plan.append(
        Artifact("harmonyos", "harmonyos/app_icon.png", 216, "rounded",
                 "%s/app_icon.png" % HARMONY_APP_MEDIA)
    )

    # --- iOS: full AppIcon.appiconset -------------------------------------
    for name, px in IOS_APPICON_FILES:
        plan.append(
            Artifact("ios", "ios/AppIcon.appiconset/%s" % name, px, "rounded",
                     "%s/%s" % (IOS_APPICONSET, name))
        )

    # --- macOS: .iconset (the .icns comes from iconutil / Pillow) ----------
    # NOTE the content_fraction: a macOS icon's body must sit on Apple's smaller
    # centred grid, NOT fill the canvas.  Without it the icon renders visibly
    # larger than every neighbouring icon in the Dock.
    for name, px in MACOS_ICONSET_FILES:
        plan.append(
            Artifact("macos", "macos/Cicada.iconset/%s" % name, px, "rounded",
                     "%s/Cicada.iconset/%s" % (QT_APPICON_DIR, name),
                     content_fraction=MACOS_CONTENT_FRACTION)
        )

    # --- Linux: flat PNGs, hicolor layout ---------------------------------
    for px in LINUX_PNG_SIZES:
        plan.append(
            Artifact("linux", "linux/hicolor/%dx%d/apps/%s.png" % (px, px, LINUX_ICON_NAME),
                     px, "flat", None,
                     "hicolor tree for the .desktop Icon= key")
        )
    for px in (256, 512):
        plan.append(
            Artifact("linux", "linux/%s-%d.png" % (LINUX_ICON_NAME, px), px, "flat",
                     "%s/%s-%d.png" % (QT_APPICON_DIR, LINUX_ICON_NAME, px))
        )

    # Small sizes first: the big supersampled buffers are then allocated last
    # and never sit around while smaller icons are rendered.
    plan.sort(key=lambda a: (a.size, a.platform, a.out_rel))
    return plan


def ios_contents_json():
    """The Contents.json matching the file names this generator emits."""
    images = []
    for idiom, logical, scale, filename in IOS_CONTENTS_ENTRIES:
        images.append(
            {
                "filename": filename,
                "idiom": idiom,
                "scale": scale,
                "size": logical,
            }
        )
    return {"images": images, "info": {"author": "xcode", "version": 1}}


# ===========================================================================
# Writing
# ===========================================================================

def ensure_dir(path):
    if path:
        os.makedirs(path, exist_ok=True)


def write_png(image, path):
    ensure_dir(os.path.dirname(path))
    # Pillow writes no timestamp into PNG, and no metadata is passed here, so
    # the bytes depend only on the pixels and the Pillow/zlib version.
    image.save(path, format="PNG")


def write_text(path, text, executable=False):
    ensure_dir(os.path.dirname(path))
    with open(path, "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    if executable and os.name != "nt":
        # Shell wrappers must be runnable straight after generation; Windows has
        # no execute bit, so this is skipped there.
        os.chmod(path, 0o755)


def rel(path):
    return os.path.relpath(path, REPO).replace("\\", "/")


def qt_path(*parts):
    return os.path.join(REPO, QT_APPICON_DIR.replace("/", os.sep), *parts)


# ===========================================================================
# Verification helpers (they print numbers; they do not change any output)
# ===========================================================================

def _count(img):
    return img.histogram()[255]


def describe_source(shapes, ink_box, out_dir):
    """Human readable proof that the two subtle steps really happened."""
    lines: List[str] = []
    lines.append("source SVG        : %s" % rel(SOURCE_SVG))
    lines.append("filled shapes     : %d" % len(shapes))
    for index, shape in enumerate(shapes):
        lines.append(
            "  shape %d         : %d subpath(s), fill=%s, fill-rule=%s, mask=%s"
            % (
                index,
                len(shape.polylines),
                shape.fill,
                "evenodd" if shape.even_odd else "nonzero",
                ("%d polygon(s)" % len(shape.clip)) if shape.clip else "none",
            )
        )
    lines.append(
        "glyph ink box     : x %.4f..%.4f  y %.4f..%.4f  (%.4f x %.4f user units)"
        % (
            ink_box[0], ink_box[2], ink_box[1], ink_box[3],
            ink_box[2] - ink_box[0], ink_box[3] - ink_box[1],
        )
    )

    probe = 512
    matrix = glyph_matrix(ink_box, probe, 1.0)
    for index, shape in enumerate(shapes):
        even_odd = rasterise_shape(shape, probe, matrix, force_union=False)
        union = rasterise_shape(shape, probe, matrix, force_union=True)
        knocked_out = _count(ImageChops.difference(union, even_odd))
        lines.append(
            "  shape %d coverage: evenodd=%d px, nonzero/union=%d px, "
            "knocked out by even-odd=%d px" % (index, _count(even_odd), _count(union), knocked_out)
        )
        if shape.clip:
            unclipped = rasterise_shape(shape, probe, matrix, skip_clip=True)
            clipped_away = _count(ImageChops.difference(unclipped, even_odd))
            lines.append(
                "  shape %d mask    : %d px clipped away by the mask rectangle"
                % (index, clipped_away)
            )
        del even_odd, union

    glyph = render_glyph(shapes, probe, matrix)
    lines.append("glyph coverage    : %d px non-empty at %dx%d" % (_count(glyph), probe, probe))
    lines.append("output root       : %s" % rel(out_dir))
    return lines


# ===========================================================================
# Main
# ===========================================================================

def generate(out_dir, install_mode="existing", qt_appicon=True, only=None):
    Image.init()  # populate Image.SAVE so the ICNS capability check is honest

    # "--install none" means "touch nothing outside out/", so it also switches
    # the QtPlayer asset directory off.
    if install_mode == "none":
        qt_appicon = False

    shapes = extract_geometry(SOURCE_SVG)
    ink_box = measure_ink_box(shapes)

    notes = describe_source(shapes, ink_box, out_dir)
    for line in notes:
        print(line)
    print("")

    plan = build_plan()
    if only:
        wanted = set(only)
        plan = [a for a in plan if a.platform in wanted]

    cache: Dict[Tuple[int, str, float, float], Image.Image] = {}

    def get(size, variant, fraction=None, content_fraction=1.0):
        key = (size, variant,
               fraction if fraction is not None else -1.0,
               content_fraction)
        image = cache.get(key)
        if image is None:
            image = render_icon(shapes, ink_box, size, variant, fraction, content_fraction)
            cache[key] = image
        return image

    written: List[Dict[str, object]] = []
    skipped: List[Dict[str, str]] = []

    for artifact in plan:
        image = get(artifact.size, artifact.variant, artifact.fraction, artifact.content_fraction)
        out_path = os.path.join(out_dir, artifact.out_rel.replace("/", os.sep))
        write_png(image, out_path)
        record = {
            "platform": artifact.platform,
            "out": rel(out_path),
            "size": artifact.size,
            "variant": artifact.variant,
            "dest": artifact.dest_rel,
            "dest_written": False,
            "note": artifact.note,
        }
        print("[out]   %-58s %4dx%-4d %s" % (artifact.out_rel, artifact.size, artifact.size, artifact.variant))

        if artifact.dest_rel:
            dest_path = os.path.join(REPO, artifact.dest_rel.replace("/", os.sep))
            dest_exists = os.path.exists(dest_path)
            # The QtPlayer asset directory is requested explicitly (there is no
            # icon wired there yet), so it is written even under the default
            # "existing" mode; everything else follows install_mode strictly.
            #
            # The Android adaptive foreground is the same kind of case: it is a
            # NEW file this generator owns (drawable-nodpi/ did not exist), and
            # the adaptive-icon XML written below references it by name.  If it
            # were skipped, `@drawable/ic_launcher_foreground` would silently
            # keep resolving to the stock Android Studio vector and the icon
            # would still not change -- exactly the bug this is fixing.
            is_qt_appicon = artifact.dest_rel.startswith(QT_APPICON_DIR + "/")
            is_android_adaptive = artifact.dest_rel.startswith(ANDROID_RES + "/drawable-nodpi/")
            if (
                install_mode == "all"
                or (install_mode == "existing" and dest_exists)
                or (is_qt_appicon and qt_appicon)
                or is_android_adaptive
            ):
                write_png(image, dest_path)
                record["dest_written"] = True
                print("        -> wrote %s%s" % (rel(dest_path), "" if dest_exists else "  (new)"))
            else:
                if install_mode == "none":
                    reason = "install mode is 'none'"
                elif is_qt_appicon:
                    reason = "--no-qt-appicon"
                else:
                    reason = "does not exist yet (use --install all)"
                skipped.append({"dest": artifact.dest_rel, "reason": reason})
                print("        -> skipped %s (%s)" % (artifact.dest_rel, reason))
        written.append(record)

    # ------------------------------------------------------------------
    # Android adaptive-icon XML (API 26+).
    #
    # The density PNGs above are what pre-26 devices use.  From API 26 the
    # `mipmap-anydpi-v26/ic_launcher{,_round}.xml` resources take precedence,
    # so those two files decide the icon on every modern device.  They are
    # rewritten here to point at the white background and the regenerated
    # foreground; without this the app keeps showing the stock template.
    #
    # `anydpi-v26` and `drawable-nodpi` are version/density qualified and never
    # coexist with the density folders, so there is no duplicate-resource
    # conflict with mipmap-hdpi/... above.
    # ------------------------------------------------------------------
    if not only or "android" in set(only):
        adaptive_xml = (
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">\n'
            '    <background android:drawable="@%s" />\n'
            '    <foreground android:drawable="@%s" />\n'
            '</adaptive-icon>\n'
        ) % (ANDROID_ADAPTIVE_BACKGROUND, ANDROID_ADAPTIVE_FOREGROUND)
        background_xml = (
            '<?xml version="1.0" encoding="utf-8"?>\n'
            '<!--\n'
            '    Adaptive-icon background: the flat white the Cicada glyph sits on.\n'
            '    Generated by tools/make_icons/make_icons.py; the previous contents were\n'
            '    the Android Studio template (a teal #26A69A square plus a white grid),\n'
            '    which is why the launcher icon did not change on API 26+ devices.\n'
            '    A <shape> is used rather than a colour resource so that the launcher has\n'
            '    no resizable bitmap to scale.\n'
            '-->\n'
            '<shape xmlns:android="http://schemas.android.com/apk/res/android"\n'
            '    android:shape="rectangle">\n'
            '    <solid android:color="#FFFFFFFF" />\n'
            '</shape>\n'
        )

        for rel_dest, text, printable in (
            ("%s/mipmap-anydpi-v26/ic_launcher.xml" % ANDROID_RES, adaptive_xml, "ic_launcher.xml"),
            ("%s/mipmap-anydpi-v26/ic_launcher_round.xml" % ANDROID_RES, adaptive_xml, "ic_launcher_round.xml"),
            ("%s/drawable/ic_launcher_background.xml" % ANDROID_RES, background_xml, "ic_launcher_background.xml"),
        ):
            dest_path = os.path.join(REPO, rel_dest.replace("/", os.sep))
            if install_mode == "none":
                skipped.append({"dest": rel_dest, "reason": "install mode is 'none'"})
                continue
            existed = os.path.exists(dest_path)
            if install_mode == "existing" and not existed:
                skipped.append({"dest": rel_dest, "reason": "does not exist yet (use --install all)"})
                continue
            ensure_dir(os.path.dirname(dest_path))
            with open(dest_path, "w", encoding="utf-8", newline="\n") as fp:
                fp.write(text)
            print("        -> wrote %s%s" % (rel(dest_path), "" if existed else "  (new)"))

    # ------------------------------------------------------------------
    # iOS Contents.json (emitted to out/ only -- the repository copy is left
    # untouched on purpose, see README: rewriting it without also dropping in
    # all fifteen PNGs would break the asset catalogue).
    # ------------------------------------------------------------------
    ios_dir = os.path.join(out_dir, "ios", "AppIcon.appiconset")
    ios_contents = os.path.join(ios_dir, "Contents.json")
    write_text(ios_contents, json.dumps(ios_contents_json(), indent=2) + "\n")
    print("[out]   %-58s %s" % ("ios/AppIcon.appiconset/Contents.json", "json"))

    # ------------------------------------------------------------------
    # Windows .ico
    # ------------------------------------------------------------------
    ico_out = os.path.join(out_dir, "windows", "Cicada.ico")
    ensure_dir(os.path.dirname(ico_out))
    ico_base = get(256, "flat")
    # Pillow resamples the base image down to every requested size, and writes
    # no metadata, so this file is reproducible.
    ico_base.save(ico_out, format="ICO", sizes=[(s, s) for s in WINDOWS_ICO_SIZES])
    print("[out]   %-58s %s" % ("windows/Cicada.ico", "/".join(str(s) for s in WINDOWS_ICO_SIZES)))
    ico_dest = qt_path("Cicada.ico")
    ico_dest_written = False
    if qt_appicon:
        ensure_dir(os.path.dirname(ico_dest))
        ico_base.save(ico_dest, format="ICO", sizes=[(s, s) for s in WINDOWS_ICO_SIZES])
        ico_dest_written = True
        print("        -> wrote %s" % rel(ico_dest))
    else:
        skipped.append({"dest": rel(ico_dest), "reason": "--no-qt-appicon"})

    # ------------------------------------------------------------------
    # macOS .icns -- best effort only.  Pillow usually has no ICNS writer;
    # make_icns.sh (Apple's iconutil) is the supported path.
    # ------------------------------------------------------------------
    icns_out = os.path.join(out_dir, "macos", "Cicada.icns")
    icns_note = ""
    icns_written = False
    if "ICNS" in Image.SAVE:
        try:
            # The .icns MUST use the macOS grid too, otherwise the Dock icon is
            # the oversized one again on the one platform this matters for.
            get(1024, "rounded", MACOS_GLYPH_FRACTION, MACOS_CONTENT_FRACTION).save(
                icns_out, format="ICNS")
            icns_written = True
            icns_note = "written by Pillow (ICNS writer present)"
        except Exception as exc:  # pragma: no cover - depends on Pillow build
            icns_note = "Pillow has an ICNS writer but it failed: %s" % (exc,)
            # Do not leave a half written file behind that later looks valid.
            if os.path.exists(icns_out):
                os.remove(icns_out)
    else:
        icns_note = ("Pillow on this machine has no ICNS writer -- run "
                     "make_icns.sh on macOS to build Cicada.icns")
    print("[icns]  %s" % icns_note)
    if icns_written:
        icns_dest = qt_path("Cicada.icns")
        if qt_appicon:
            ensure_dir(os.path.dirname(icns_dest))
            with open(icns_out, "rb") as src, open(icns_dest, "wb") as dst:
                dst.write(src.read())
            print("        -> wrote %s" % rel(icns_dest))
    else:
        skipped.append({
            "dest": rel(qt_path("Cicada.icns")),
            "reason": icns_note,
        })

    # ------------------------------------------------------------------
    # make_icns.sh (and the Linux .desktop template)
    # ------------------------------------------------------------------
    write_text(os.path.join(out_dir, "macos", "make_icns.sh"), MAKE_ICNS_SH, executable=True)
    print("[out]   %-58s %s" % ("macos/make_icns.sh", "shell wrapper for iconutil"))
    if qt_appicon:
        write_text(qt_path("make_icns.sh"), MAKE_ICNS_SH, executable=True)
        print("        -> wrote %s" % rel(qt_path("make_icns.sh")))

    desktop = (
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=CicadaPlayer\n"
        "Comment=Qt6 QML player component demo\n"
        # "%%U" is a literal "%U" after the % formatting below: the desktop
        # entry spec's field code for "a list of URLs", not a format specifier.
        "Exec=appQtPlayer %%U\n"
        "Icon=%s\n"
        "Terminal=false\n"
        "Categories=AudioVideo;Player;\n"
        "StartupWMClass=appQtPlayer\n"
    ) % LINUX_ICON_NAME
    write_text(os.path.join(out_dir, "linux", "%s.desktop" % LINUX_ICON_NAME), desktop)
    print("[out]   %-58s %s" % ("linux/%s.desktop" % LINUX_ICON_NAME,
                               "template, Icon= reference only"))

    # ------------------------------------------------------------------
    # Manifest (deterministic: no timestamps, sorted keys)
    # ------------------------------------------------------------------
    manifest = {
        "generator": rel(THIS_FILE),
        "source_svg": rel(SOURCE_SVG),
        "settings": {
            "corner_radius_fraction": CORNER_RADIUS_FRACTION,
            "glyph_color": GLYPH_COLOR_HEX,
            "glyph_fraction_flat": GLYPH_FRACTION_FLAT,
            "glyph_fraction_rounded": GLYPH_FRACTION_ROUNDED,
            "curve_segments": svgpath.CURVE_SEGMENTS,
            "supersample": SUPERSAMPLE,
        },
        "glyph_ink_box": [round(v, 6) for v in ink_box],
        "artifacts": written,
        "not_written": skipped,
        "special": {
            "ios_contents_json": rel(ios_contents),
            "windows_ico": rel(ico_out),
            "windows_ico_sizes": list(WINDOWS_ICO_SIZES),
            "windows_ico_dest_written": ico_dest_written,
            "macos_icns": rel(icns_out),
            "macos_icns_written": icns_written,
            "macos_icns_note": icns_note,
            "linux_icon_name": LINUX_ICON_NAME,
        },
    }
    manifest_path = os.path.join(out_dir, "manifest.json")
    write_text(manifest_path, json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    print("")
    print("manifest        : %s" % rel(manifest_path))
    print("artifacts       : %d PNG written under out/" % len(written))

    if skipped:
        print("")
        print("NOT written into the repository (create them explicitly if wanted):")
        seen = set()
        for entry in skipped:
            key = (entry["dest"], entry["reason"])
            if key in seen:
                continue
            seen.add(key)
            print("  %-70s %s" % (entry["dest"], entry["reason"]))
    return manifest


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description="Generate every CicadaPlayerNext platform icon (deterministic).",
    )
    parser.add_argument(
        "--out",
        default=DEFAULT_OUT,
        help="output root (default: tools/make_icons/out)",
    )
    parser.add_argument(
        "--install",
        choices=("none", "existing", "all"),
        default="existing",
        help=(
            "write into the platform source trees: 'none' = out/ only, "
            "'existing' = overwrite only files that already exist (default), "
            "'all' = also create the ones that do not exist yet"
        ),
    )
    parser.add_argument(
        "--qt-appicon",
        dest="qt_appicon",
        action="store_true",
        default=True,
        help="also write platform/QtPlayer/assets/appicon/** (default: on)",
    )
    parser.add_argument(
        "--no-qt-appicon",
        dest="qt_appicon",
        action="store_false",
        help="skip platform/QtPlayer/assets/appicon/**",
    )
    parser.add_argument(
        "--only",
        action="append",
        choices=PLATFORMS,
        help="restrict to one platform (repeatable)",
    )
    args = parser.parse_args(argv)

    try:
        generate(
            os.path.abspath(args.out),
            install_mode=args.install,
            qt_appicon=args.qt_appicon,
            only=args.only,
        )
    except (IconError, svgpath.SvgPathError) as exc:
        print("make_icons: %s" % (exc,), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
