# `tools/make_icons` — application icons for every CicadaPlayerNext demo

Deterministic, re-runnable generator for the app icons of all six platform demos.
Python 3.10+ and **Pillow only** — there is no SVG renderer in this toolchain
(no `cairosvg`, no `librsvg` / `rsvg-convert`, no ImageMagick, no Inkscape, and
Pillow cannot read SVG), so the generator rasterises `doc/Cicada.svg` itself.

```
tools/make_icons/
├── svgpath.py        # tiny SVG path parser + flattener (stdlib only)
├── make_icons.py     # the generator
├── README.md         # this file
└── out/              # every generated file, mirroring the repo layout
```

## How to run it

```sh
python tools/make_icons/make_icons.py
```

That is the whole thing. It reads `CicadaPlayerNext/doc/Cicada.svg`, writes all
49 icons under `tools/make_icons/out/`, and additionally writes the QtPlayer
asset directory `platform/QtPlayer/assets/appicon/` (which had no icon at all).
It does **not** touch Android / HarmonyOS / iOS unless the target file already
exists — see *Install modes* below.

Useful switches:

| switch | effect |
| --- | --- |
| `--out DIR` | write the mirror tree somewhere else (default `tools/make_icons/out`) |
| `--install {none,existing,all}` | `none` = `out/` only; `existing` (default) = also overwrite platform files that already exist; `all` = also create ones that do not |
| `--no-qt-appicon` | skip `platform/QtPlayer/assets/appicon/**` |
| `--only PLATFORM` | restrict to one of `android harmonyos ios macos windows linux` (repeatable) |

Sanity check for the path parser on its own (no Pillow needed):

```sh
python tools/make_icons/svgpath.py     # runs a small self-test, prints OK
```

Every run also writes `out/manifest.json`: the settings, the measured glyph
bounding box, and one record per artifact (`out` path, pixel size, variant,
destination, whether the destination was written). It contains no timestamps.

## Determinism

Running the generator twice produces byte-identical files:

* Bezier flattening uses a fixed subdivision count (`svgpath.CURVE_SEGMENTS`,
  16 segments per cubic/quadratic) — no adaptive tolerance, no randomness.
* Rasterisation is `PIL.ImageDraw.polygon` at `scale = 8` followed by
  `Image.LANCZOS` downscale. Both are integer/fixed-point operations.
* Nothing time dependent is embedded. Pillow writes no timestamp into PNG or
  ICO, and the generator passes no metadata.
* `out/manifest.json` is serialised with sorted keys and no clock.

Caveat: byte-identity is guaranteed for a fixed Pillow + zlib build. A different
Pillow version may compress slightly differently (and `Image.LANCZOS` is used
through `Image.Resampling.LANCZOS` when available, falling back to the old
`Image.LANCZOS` alias on Pillow < 9.1).

Peak memory: the largest icon (1024 px, iOS marketing) is rasterised at
8192 × 8192 in 8-bit mode, so a run peaks at roughly 300 MB.

## Design rules

| rule | value |
| --- | --- |
| glyph colour | `#00C1DE`, taken from the source SVG (`GLYPH_COLOR_HEX`) |
| rounded-square variants | Android, HarmonyOS, macOS, iOS |
| rounded-square background | `#FFFFFF` |
| corner radius | **22.37 %** of the *body* side (`CORNER_RADIUS_FRACTION = 0.2237`) |
| glyph size, rounded variant | **62 %** of the *body* side |
| flat variants | Windows, Linux |
| flat background | transparent |
| glyph size, flat variant | **92 %** of the canvas side |
| body size | fills the canvas, **except macOS** (see below) |
| supersampling | `scale = 8`, downscaled with `Image.LANCZOS` |
| curve flattening | 16 straight segments per cubic/quadratic |

"The glyph occupies N % of the canvas side" is implemented as: the glyph's
*tight ink bounding box* is uniformly scaled so that its **longest edge** is
`N %` of the canvas side, the aspect ratio is preserved, and the box is centred.
The tight box is measured by rendering the glyph once and asking for the
non-zero bounding box, so the mask clip and the curves' real extent are both
accounted for.

### macOS gets Apple's icon grid — the body does NOT fill the canvas

Every other platform masks the icon itself, so the rounded square is meant to
fill the canvas. **macOS does not:** the Finder/Dock draw the image as-is, and
Apple's macOS icon grid places the icon's body on a smaller centred square with
a transparent margin around it. A macOS icon whose body fills the whole canvas
therefore renders **visibly bigger than every neighbouring icon in the Dock**.

The generator therefore shrinks the body for the macOS `.iconset`/`.icns` to
Apple's published geometry (`MACOS_CONTENT_FRACTION`):

| quantity | value |
| --- | --- |
| canvas | 1024 × 1024 |
| body (rounded square) | **824 × 824**, centred |
| transparent margin | **100 px** on each side |
| corner radius | 22.37 % **of the body** (scales with it) |
| glyph | scaled with the body (`MACOS_GLYPH_FRACTION`), so its size *relative to the white square* is unchanged |

Measured on the generated files (non-zero alpha span across the mid row):

```
icon_16x16.png        canvas=  16  body=  13  (target 13)
icon_256x256.png      canvas= 256  body= 206  (target 206)
icon_512x512@2x.png   canvas=1024  body= 824  (target 824)
```

The body is drawn at **target size** rather than supersampled: LANCZOS
downscaling spreads a straight edge over ~4 px of ringing (a row of near-zero
alphas before the solid edge), which made the body measure ~3 px too wide.
`ImageDraw.rounded_rectangle` antialiases by itself, so the body is exact; the
**glyph** is still supersampled because its curves genuinely need the coverage
mask.

Qt is used on **both** macOS and Windows, and the two get deliberately different
icons: macOS gets the rounded-white-background `.icns` on Apple's grid, Windows
gets the flat transparent-background `.ico`.

### How the source art is read

`doc/Cicada.svg` is a 48 × 48 Sketch export. The generator parses it with
`xml.etree.ElementTree`, resolves `id` references, folds
`translate/scale/matrix/rotate` down the group tree, and collects three filled
paths (all `fill="#00C1DE"`).

Two details of the file are load bearing, and getting either wrong still
produces a *plausible* icon rather than an obvious failure:

1. **`fill-rule="evenodd"`.** The outer `<g id="画板">` sets
   `fill-rule="evenodd"`, and `fill-rule` is an inherited property, so it
   applies to every descendant. `<path id="Fill-5">` contains **two** subpaths:
   the cicada body outline and the round-cornered **play triangle**, and the
   triangle lies strictly inside the body loop. With `evenodd` the triangle is
   knocked out of the body; with the default `nonzero` it would be swallowed and
   the play glyph would vanish. The generator therefore implements the even-odd
   rule (rasterise each subpath, combine with XOR — for non-self-intersecting
   subpaths that is exactly the even-odd rule). The run log prints
   `knocked out by even-odd=NNN px` for the shape, which is non-zero here.
2. **`mask="url(#mask-2)"`.** Only `<path id="Fill-1">` is masked, by
   `<polygon id="path-1">` — the Sketch artboard rectangle
   `(0, 0.428215856)`–`(35.7569, 19.7253)` in that group's local coordinates.
   It is applied by rasterising the path and the rectangle at the same
   supersampled size and multiplying them. The run log prints
   `NNN px clipped away by the mask rectangle`.

   Measured result on the current art: the artboard rectangle is *tangent* to
   `Fill-1`'s silhouette on all four sides (the closest approach is about
   `6e-5` user units at the left edge and about `6e-4` at the top), so the clip
   is a no-op for practical purposes — it removes a sliver thinner than
   0.0002 px even at 1024 px. It is still applied, because the generator should
   not depend on that coincidence holding.

### Verification the generator performs

The generator refuses to write a blank icon: after rasterising it checks
`getbbox()` is non-`None` on the supersampled coverage and raises
`IconError` otherwise. It also prints, for the record:

* the three shapes with their subpath count, fill, fill rule and mask;
* the measured glyph ink box in user units — roughly `42.5 x 41.1` units on the
  current art (about `x 3.0 … 45.5`, `y 4.0 … 45.1` inside the 48 x 48 viewBox);
* per shape, the even-odd versus union coverage and the mask's effect;
* the total non-empty glyph coverage at 512 × 512.

## What is generated

All paths below are relative to `CicadaPlayerNext/`.

### Under `tools/make_icons/out/` (always)

| path | pixels | variant |
| --- | --- | --- |
| `android/mdpi/ic_launcher.png`, `ic_launcher_round.png` | 48 | rounded |
| `android/hdpi/…` | 72 | rounded |
| `android/xhdpi/…` | 96 | rounded |
| `android/xxhdpi/…` | 144 | rounded |
| `android/xxxhdpi/…` | 192 | rounded |
| `harmonyos/icon.png`, `app_icon.png` | 216 | rounded |
| `harmonyos/icon-114.png`, `startIcon.png` | 114 | rounded |
| `ios/AppIcon.appiconset/icon-20.png` | 20 | rounded |
| `ios/AppIcon.appiconset/icon-20@2x.png` | 40 | rounded |
| `ios/AppIcon.appiconset/icon-20@3x.png` | 60 | rounded |
| `ios/AppIcon.appiconset/icon-29.png` | 29 | rounded |
| `ios/AppIcon.appiconset/icon-29@2x.png` | 58 | rounded |
| `ios/AppIcon.appiconset/icon-29@3x.png` | 87 | rounded |
| `ios/AppIcon.appiconset/icon-40.png` | 40 | rounded |
| `ios/AppIcon.appiconset/icon-40@2x.png` | 80 | rounded |
| `ios/AppIcon.appiconset/icon-40@3x.png` | 120 | rounded |
| `ios/AppIcon.appiconset/icon-60@2x.png` | 120 | rounded |
| `ios/AppIcon.appiconset/icon-60@3x.png` | 180 | rounded |
| `ios/AppIcon.appiconset/icon-76.png` | 76 | rounded |
| `ios/AppIcon.appiconset/icon-76@2x.png` | 152 | rounded |
| `ios/AppIcon.appiconset/icon-83.5@2x.png` | 167 | rounded |
| `ios/AppIcon.appiconset/icon-1024.png` | 1024 | rounded |
| `ios/AppIcon.appiconset/Contents.json` | — | Xcode manifest for the 15 PNGs above |
| `macos/Cicada.iconset/icon_16x16.png` | 16 | rounded |
| `macos/Cicada.iconset/icon_16x16@2x.png` | 32 | rounded |
| `macos/Cicada.iconset/icon_32x32.png` | 32 | rounded |
| `macos/Cicada.iconset/icon_32x32@2x.png` | 64 | rounded |
| `macos/Cicada.iconset/icon_128x128.png` | 128 | rounded |
| `macos/Cicada.iconset/icon_128x128@2x.png` | 256 | rounded |
| `macos/Cicada.iconset/icon_256x256.png` | 256 | rounded |
| `macos/Cicada.iconset/icon_256x256@2x.png` | 512 | rounded |
| `macos/Cicada.iconset/icon_512x512.png` | 512 | rounded |
| `macos/Cicada.iconset/icon_512x512@2x.png` | 1024 | rounded |
| `macos/make_icns.sh` | — | `iconutil` wrapper (macOS only) |
| `macos/Cicada.icns` | — | **only if** Pillow reports an ICNS writer |
| `windows/Cicada.ico` | 16/24/32/48/64/128/256 | flat |
| `linux/hicolor/<N>x<N>/apps/CicadaPlayer.png` | 16, 24, 32, 48, 64, 128, 256, 512 | flat |
| `linux/Cicada-256.png`, `linux/Cicada-512.png` | 256, 512 | flat |
| `linux/CicadaPlayer.desktop` | — | template, for the `Icon=` key |
| `manifest.json` | — | run manifest |

### Additionally written into the repository

`platform/QtPlayer/assets/appicon/` (created by this generator because nothing
there was wired at all):

| file | pixels | variant |
| --- | --- | --- |
| `Cicada.ico` | 16/24/32/48/64/128/256 | flat |
| `Cicada.iconset/` (10 PNGs, same names as above) | 16 … 1024 | rounded |
| `make_icns.sh` | — | `iconutil` wrapper |
| `Cicada.icns` | — | only if Pillow can write ICNS |
| `Cicada-256.png`, `Cicada-512.png` | 256, 512 | flat |

`Pillow` usually has no ICNS writer (and even when it does it produces a plain
icon family without the macOS "squircle" treatment). The supported path is:

```sh
cd platform/QtPlayer/assets/appicon
./make_icns.sh          # macOS only; needs Apple's iconutil
```

That reads `Cicada.iconset/` and writes `Cicada.icns`.

With the default `--install existing`, the generator overwrites these files
**because they already exist**:

* `platform/Android/source/paasApp/src/main/res/mipmap-{mdpi,hdpi,xhdpi,xxhdpi,xxxhdpi}/ic_launcher.png`
* `…/mipmap-{…}/ic_launcher_round.png`
* `platform/HarmonyOS/entry/src/main/resources/base/media/icon.png`
* `platform/HarmonyOS/AppScope/resources/base/media/app_icon.png`
* `platform/Apple/demo/iOS/CicadaDemo/CicadaDemo/CicadaResource/Assets.xcassets/AppIcon.appiconset/{icon-60@2x,icon-60@3x,icon-76,icon-76@2x,icon-83.5@2x}.png`

It deliberately does **not** overwrite the iOS `AppIcon.appiconset/Contents.json`.
The repository copy describes eighteen slots but names only five files; the
generator's `out/ios/AppIcon.appiconset/Contents.json` names all fifteen files
it emits. Copy the two together (all fifteen PNGs plus that JSON) or not at all
— dropping the new JSON in alone would leave the asset catalogue referencing
files that do not exist.

Files with **no matching file in the repository** are written to `out/` only
under the default mode. Re-run with `--install all` to create them in place:

* `platform/HarmonyOS/entry/src/main/resources/base/media/startIcon.png` (114 px)
* the ten extra iOS `AppIcon.appiconset` PNGs
* `platform/Android/ComposePlayer/app/src/main/res/mipmap-<density>/…` — **not
  generated at all**, see "Not covered" below

### Not covered, on purpose

* `platform/Android/ComposePlayer/app/src/main/res/` is a *different* Android
  demo and uses `ic_launcher.webp` / `ic_launcher_round.webp` plus adaptive-icon
  XML (`mipmap-anydpi-v26/ic_launcher.xml`, `drawable/ic_launcher_foreground.xml`).
  WebP and vector adaptive icons are out of scope for this PNG generator.
* `platform/Flutter/` launcher icons.
* `platform/Android/source/<module>/build/intermediates/<variant>/<task>/` — build
  outputs. The generator never writes there; a Gradle build regenerates them
  from `src/`.

## Platform wiring that still has to happen

These are **configuration** changes, not image files, so the generator does not
make them. Current values are quoted as found in the repository.

### Root cause on modern Android

`platform/Android/source/paasApp/src/main/res/mipmap-anydpi-v26/ic_launcher.xml`
(and `ic_launcher_round.xml`) win over the density PNGs on API 26+, and they
currently point at the stock Android Studio template:

```xml
<adaptive-icon xmlns:android="http://schemas.android.com/apk/res/android">
    <background android:drawable="@drawable/ic_launcher_background" />
    <foreground android:drawable="@drawable/ic_launcher_foreground" />
</adaptive-icon>
```

with `res/drawable/ic_launcher_background.xml` painting a teal `#26A69A` square
plus a white grid. Replacing the mipmap PNGs alone therefore changes nothing on
API 26+ devices. To actually show the cicada you must also replace
`ic_launcher_background.xml` (e.g. `#FFFFFF`) and `ic_launcher_foreground.xml`
(the cicada, drawn into the 108 × 108 viewport with the 72 × 72 safe zone), or
delete both `mipmap-anydpi-v26/*.xml` files so the density PNGs are used.

### Android

`platform/Android/source/paasApp/src/main/AndroidManifest.xml` — already correct,
no change needed:

```xml
android:icon="@mipmap/ic_launcher"        <!-- line 14 -->
android:roundIcon="@mipmap/ic_launcher_round"  <!-- line 16 -->
```

`platform/Android/ComposePlayer/app/src/main/AndroidManifest.xml` declares the
same two attributes (lines 40 and 42) but resolves them to its own WebP /
adaptive-icon resources.

### HarmonyOS

`platform/HarmonyOS/entry/src/main/module.json5`:

```json5
"icon": "$media:icon",            // line 40
"startWindowIcon": "$media:icon", // line 42
```

Both resolve to `entry/src/main/resources/base/media/icon.png` (216 × 216,
regenerated). If you want the 114 px start-window icon instead, copy
`out/harmonyos/startIcon.png` to
`entry/src/main/resources/base/media/startIcon.png` and change line 42 to
`"startWindowIcon": "$media:startIcon"`.

`platform/HarmonyOS/AppScope/app.json5`:

```json5
"icon": "$media:app_icon",        // line 7
```

resolves to `AppScope/resources/base/media/app_icon.png` (regenerated). There is
no `startIcon.png` anywhere in the tree today, and no other `media/` directory
(`resources/base/media` is the only one).

### iOS

Wiring is already correct — the asset catalogue is named in the Xcode project:

```
platform/Apple/demo/iOS/CicadaDemo/CicadaDemo.xcodeproj/project.pbxproj
  line 930:  ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;
  line 962:  ASSETCATALOG_COMPILER_APPICON_NAME = AppIcon;
  line 931:  ASSETCATALOG_COMPILER_LAUNCHIMAGE_NAME = LaunchImage;
```

`platform/Apple/demo/iOS/.../AppSupportFiles/Info.plist` has **no**
`CFBundleIconName` / `CFBundleIcons` key, which is normal for a modern asset
catalogue (Xcode injects `CFBundleIconName` at build time). Nothing to change
there. The only missing piece was the PNG files themselves.

If you copy the generator's full PNG set into the appiconset, also copy
`out/ios/AppIcon.appiconset/Contents.json` over the repository copy so every
slot has a `filename`.

### macOS — QtPlayer bundle

`platform/QtPlayer/CMakeLists.txt` lines 1043-1047:

```cmake
set_target_properties(appQtPlayer PROPERTIES
        MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
        MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}
        MACOSX_BUNDLE TRUE
        )
```

There is **no** `MACOSX_BUNDLE_ICON_FILE` and no `Cicada.icns` anywhere, so the
bundle currently gets Qt's default icon. Add:

```cmake
        MACOSX_BUNDLE_ICON_FILE Cicada.icns
```

and make sure `Cicada.icns` ends up as `appQtPlayer.app/Contents/Resources/Cicada.icns`
(`set_source_files_properties(… MACOSX_PACKAGE_LOCATION Resources)` plus adding
it to the target, or an `install(FILES …)` rule).

The repository's Xcode-based macOS demo has an empty
`platform/Apple/demo/macOS/CicadaDemo/CicadaDemo/Assets.xcassets/AppIcon.appiconset/Contents.json`
(ten slots, no files). The ten `.iconset` PNG names the generator emits are
exactly the names that appiconset expects (`icon_16x16.png`, `icon_32x32.png`,
… `icon_512x512@2x.png`), so they can be reused there verbatim.

### Windows — QtPlayer

There is no `.rc` file and no icon resource anywhere in `platform/QtPlayer`.
`platform/QtPlayer/CMakeLists.txt` only adds `appQtPlayer.manifest` to the
target (under `if (WIN32 AND MSVC)`, around line 502). To give the executable an
icon you need a new `.rc` such as:

```rc
IDI_ICON1 ICON DISCARDABLE "assets/appicon/Cicada.ico"
```

added to the target sources on Windows, **or** switch to Qt's resource route: an
`.qrc` with `QT_RESOURCE_ALIAS` plus
`set_target_properties(appQtPlayer PROPERTIES WIN32_EXECUTABLE TRUE)` — note the
project deliberately avoids `WIN32_EXECUTABLE` today and drives the subsystem
per configuration through `/SUBSYSTEM:` link options instead (around line 1069).
`assets/appicon/Cicada.ico` is the file both routes want.

### Linux — QtPlayer

Nothing in the repository references an icon: `platform/QtPlayer/build_linux.sh`
only runs `cmake --install` into `platform/QtPlayer/deploy`, there is no
`.desktop` file anywhere, and no `Icon=` key. The convention used here is:

* install the PNGs as
  `share/icons/hicolor/<N>x<N>/apps/CicadaPlayer.png`
  (`out/linux/hicolor/` already has that layout), and
* add `share/applications/CicadaPlayer.desktop` containing
  `Icon=CicadaPlayer` (the template is `out/linux/CicadaPlayer.desktop`).

`CicadaPlayer` is a *proposal*: no existing file or resource fixes the Linux
icon name, so pick whatever the packaging script ends up using and keep the two
in sync. The executable is `appQtPlayer` (see `project(QtPlayer …)` and
`qt_add_executable(appQtPlayer …)` in `CMakeLists.txt`), which is what the
template's `Exec=` and `StartupWMClass=` assume.

## Overwriting rules

* Files this generator writes only ever land in `tools/make_icons/out/`,
  `platform/QtPlayer/assets/appicon/`, or on top of the exact platform icon
  files listed above.
* It never deletes anything, and it never writes into a build directory.
* If you change the art or the design rules, delete `out/` first: stale files
  from an earlier rule set are not cleaned up for you.
