# AGENTS.md: Throw

The why. `CLAUDE.md` has the commands; `README.md` the claims and the numbers;
`~/Projects/resolume/specs/SPEC-throw.md` the spec this was built from.

## The idea

Allan asked (2026-11-02) for an effect that "puts the video onto a textured background as
if it were projected onto a real surface" (crumpled paper, a brick wall, corrugated
sheet, hatched metal sheet, pleated drapes, as a minimum) and said: make it a projector,
not a texture map.

**A projector is a lamp with a picture in it, and you are not standing at the lens.**
Every pixel of the clip is a ray from a pinhole at C. Where it meets the relief it
delivers light by the clip's value, the cosine between the ray and the surface, and the
inverse square of the distance, relative to the flat screen the projector was built to
light evenly. The wall gives that back times its albedo, plus a GGX highlight on metal and
glaze. The viewer is a second pinhole at V with a shift lens: the frame is the wall's
front plane wherever they stand, which keeps the wall still and lets the picture slide
over the relief.

Three passes (`source/Shaders.cpp`, assembled per pass by `shaders::Assemble`):

1. **Surface** (cached): albedo in rgb, height in metres in a (RGBA32F), from the
   procedural surface or the Surface File. Each texel averages 2×2 samples inside it.
2. **Occlusion** (cached): how much of the room's light reaches each point. Horizon-based,
   8 directions, measured against the local tangent plane so any plane is 1, out to 4R
   in metres. A sample off the frame contributes no horizon (the wall beyond the frame is
   unknown, and inventing one darkened the edges).
3. **Project** (every frame): for each pixel, march the viewer's ray from z = 0 to −R
   (two steps per texel of its horizontal travel, at most 256), bisect, then secant;
   take the normal there; find which projector ray lands on it and whether it is inside
   the picture; march from the point to the lens for the shadow; then

       g   = d²·max(n·ω, 0) / (r²·cos³α)
       out = ρ·(g·vis·(b + (1−b)·L) + A·ao) + spec·g·vis·(b + (1−b)·L) + F0·A·ao (metal)

   with A = lux·W·H/lumens, then Exposure, sRGB, Mix.

Both cached passes rebuild only when `SurfaceKey` changes (every surface control, the
raster, the file's generation). A still surface costs the projection alone.

## Decisions taken without asking (the brief says decide and write it down)

- **Name and id.** `SW Throw`, `TW01`, bundle `com.stoatworks.ffgl.throw`, harness
  `twtest`. A projector's throw is how far, and at what, it sends its picture. `throw` is
  a C++ keyword, so the namespace is `throwfx`. Built in `~/Projects/resolume/throw` as
  its own repo and pushed, public, to github.com/stoatworks-labs/throw on 2026-11-02, as
  the fleet's plugins are. Released as v0.1.0 the same day, on Allan's go-ahead.
- **More surfaces than the minimum.** The list's five, plus Weatherboard (feather-edge
  boards: a lip that casts a shadow down every board) and Tiles (glazed metro tile: a
  dielectric highlight on a diffuse body, the grout without it), plus Flat (the reference
  screen, which the checks lean on) and File (the user's own height map or photo).
  "Hatched metal sheet" became aluminium tread plate with three patterns: Diamond,
  Five-Bar and Crosshatch.
- **Everything at its real size, and Depth multiplies.** UK metric brick (215 × 65 mm,
  10 mm joints raked 8 mm), 3-inch corrugated (76 mm, 18 mm deep, depth scaling with
  pitch), 30 mm tread-plate cells with 1.5 mm lugs, 150 mm weatherboard with a 15 mm lip,
  200 × 100 mm tiles with 3 mm grout, drape folds half as deep as they are apart, paper
  facets 120 mm and ±15 mm. On a 2.5 m wall at 1080p a pixel is 2.3 mm, so a brick
  joint's slide at Depth 1, from the default viewpoint, is about two pixels, and Depth 4
  is the knob for drama. The README says
  so rather than exaggerating the default.
- **The defaults**: Brick, stretcher bond, a 2.5 m wall; a 10 000 lm projector 1 m above
  the frame's centre at 6 m with lens shift and a 2000:1 black (0.05%); a 20 lux room;
  the viewer 2 m right of and 0.8 m below the frame's centre, 4 m back. Chosen so every
  effect the brief lists shows at once: shading, shadow, slide, the room's grey.
- **The metals' F0 is computed, not remembered.** `tools/f0.py` (repousse's, cut down to
  two metals) integrates published n,k against CIE 1931 under D65: zinc from **Werner,
  Glantschnig and Ambrosch-Draxl (2009)**, aluminium from **Rakić (1995)**. The spec said
  Querry (1985) for zinc; refractiveindex.info's zinc in the visible is Werner's, so that
  is what is used. `f0.py --check` holds `source/Room.h` to it at 3 dp. Clean bulk metal;
  real galvanising goes grey, which Roughness covers, not F0.
- **No depth of field, no lens aperture.** At real distances there is nothing to model: a
  40 mm aperture focused at 6 m blurs a 150 mm drape fold by about 1 mm, under half a
  pixel of a 2.5 m wall at 1080p. The spec's "optional focus" control was not built.
- **The viewer has a shift lens.** The frame is the front plane's patch whatever the
  viewer's position, as a camera on a shift lens or a photo rectified to the wall would
  show it. The alternative, a camera aimed at the wall, would keystone the whole wall
  every time the viewer moved, which is not what anybody asked for.
- **Black Level** is the projector's own black, as a fraction of its white: a projector
  cannot project black, and this plus the room is why.
- **The Surface File** is an FFGL file parameter. Height reads grey as height (white at
  the front, 100 mm deep at Depth 1); Photo reads colour as albedo and luminance as
  height, stretched to the picture's own range. Neither is a measurement: a photo's
  darks are not its recesses, and the README says so. Decoded with stb_image at 16 bits a
  channel on the GL thread, once per file; a missing or broken file is Flat, logged.
- **The About block** has the fleet's four buttons (User guide, Project page, Source on
  GitHub, Support the work). `source/StoatworksAbout.h` and `ATTRIBUTIONS.md` are
  generated: the plugin is registered in stoatworks-backend (`sync-about.py` TARGETS,
  `attributions/` names, visibility and derived) and the website's projects.json, and
  `sync-about.py --only throw --apply` and `sync-attributions.py --only throw` write them.
  Edit the masters, not the files. 34 parameters in all; `tools/wincheck.ps1 -Params 34`
  in release.yml pins it.
- **The workflows are slitscan's.** `.github/workflows/ci.yml` and `release.yml` with
  the names changed. The repo is public, so they run on GitHub's free minutes; ci.yml's
  first run, on the first push, is also the first Windows build. If the repo ever goes
  private, delete them first: private repos share one small Actions allowance (the house
  rule is no workflows on private repos). release.yml runs only on a `v*` tag or by hand.
- **Seed** reaches every procedural surface: paper's facets, each brick's colour, tilt
  and set, the zinc's spangle, the plate's wear, the drapes' wander and weave, each
  board's and tile's tone. Changing it rebuilds the surface (a 4–35 ms frame; see the
  bench).

## The traps actually hit

Ordered by how much they would have cost.

- **A GPU's bilinear filter has 8-bit weights.** The normal from filtered heights was off
  by 1.8e-3 of the light on a plain tilted plane, ten times the light law's own bound,
  and the shadow march saw false steps near steep walls. Everything that decides reads
  `heightExact` (four `texelFetch`es, float weights): the normal, the hit's bisection and
  secant, and the first two texels of the shadow march. The rest of the march, where only
  a clear miss or hit matters, keeps the filtered read. The Surface File gets the same
  (`fileAt`), because Apple's software renderer's filter is not exact even at a texel's
  centre and a map at the frame's raster must come back as itself.
- **A smoothed normal can face away from the eye.** At the foot of a steep wall the
  filtered slope belongs half to the wall, and the viewer saw "turned-away" pixels that
  were lit only by the room. If the normal faces away from the eye the plugin takes the
  one-sided exact slope (`slopeExact`) instead. One pixel of the Plate at Depth 4 still
  turns away; it is counted in `--atlens`'s residual.
- **One march step per texel skips corners.** The viewer's march walked past a brick's
  arris and hit the joint behind it. Two steps per texel of horizontal travel, with the
  exact heights above, brought `--atlens` down to the residual described below.
- **The occlusion's reach was in texels.** A 24-texel horizon on a tilted plane reached
  the held edge of the buffer and darkened it by 5%; the reach is now 4R in metres and a
  sample off the frame gives no horizon. `--ambient` checks a tilted plane to 1.6e-7.
- **Offset mapping passes a plateau.** The first "flat viewer" negative shifted the
  picture by the height under the pixel, which is exact on a level recess, so
  `--parallax` passed it. The negative is now "no march, no shift", and offset mapping is
  its own negative (`kHookOffsetMap`), which `--lambert`'s slopes catch.
- **A backface lit is also a backface shadowed.** "Turned-away faces lit as if facing
  the lens" alone did nothing: the shadow march already darkens a face that looks away
  from the lens. The negative is that and no shadow march, which is the wrong model a
  plain texture map actually is.
- **Apple's software renderer does not repeat itself.** The same instance, the same
  input and the next frame changed 43 080 of 230 400 values (Drapes, 320×180); a
  running instance's frames differed from a fresh one's by up to 6.9e-5 (Plate). Most
  likely a variant of the program compiled behind the first frames and swapped in. On the GPU, frames repeat
  bit for bit and the checks say so; frame-against-frame comparisons (`--dirty`,
  `--resize`, `--away`, `--shadow`'s band) allow 1e-4 of a value (`kRepeat`, with one value
  in a thousand past it in `--dirty` and `--resize`), and report the worst.
- **No file is not texture 0.** With no file loaded the surface pass bound texture 0
  to FileTexture and Apple's driver logged it as unloadable. A one-texel black
  placeholder is bound instead.
- **ffglex's Scoped* bindings clear to 0 on exit** rather than restoring, and
  `FFGLFBO::Initialise` binds under one, so the cached buffers are allocated before
  anything is bound for the frame (as in slitscan).

## Verified, and assumed

Verified on this Mac (numbers in README Status): everything `tools/verify.sh` runs. In
words: a flat wall gives the clip back exactly, times its albedo, from any projector and
viewer position; the light on tilted planes is the inverse-square-and-cosine law relative
to the design screen at every pixel; faces turned away get the room only; a step's
shadow has the closed-form length; from the lens nothing is in shadow; the lens's
highlight on zinc sits at the mirror point and is GGX; the room's floor is A + b; a
pointed projector's dots land where the pinhole puts them; recesses show the picture
where the closed-form parallax says; every surface has its stated size; a height map
comes back exact; the surface rebuilds when, and only when, it must; the host's GL state
comes back.

Assumed, not measured:

- **A pinhole projector and a pinhole viewer.** No lens blur, no depth of field, no
  aperture penumbrae, no vignetting beyond the 1/cos³ a projector is designed for.
- **No interreflection.** Light that bounces off one facet onto another is not there;
  inside a deep drape fold or a corrugation's trough it would add a little.
- **Lambertian bodies.** Paper, brick, mortar, cloth and timber are perfectly matte.
  Their albedos (linear sRGB) are chosen to look right, not measured.
- **The room's light is uniform and white**, occluded by the relief, not shadowed by
  the projector or anything else in the room.
- **The projector's white is the clip's white.** No gamut, no white point, no lamp
  colour.
- **A Photo's luminance is not its relief.** File Holds Photo is the user's say-so.
- **Resolume's input** arrives sRGB-encoded; the plugin linearises with IEC 61966-2-1 and
  encodes again.

## Would this hold on another rasteriser, at another raster?

- `--units`, `--cues`, `--names`: no GL, double precision.
- `--flat`: the sRGB round trip's bound, 1e-5 (GLSL's pow is exp2·log2, a few ulp).
  Measured 3.0e-7 on the GPU and 7.6e-6 on the software renderer: the margin there is
  only 1.3×, which is the check most likely to need a look on a new rasteriser.
- `--lambert`: per pixel, kLightRel·g (1e-4) plus the spread a last-bit error in the
  height makes in the slope at that raster (4·2^(ilogb R − 23)/(2·texel)). Reported as a
  fraction of each pixel's own bound.
- `--parallax`: Gaussian dots' centroids against a closed-form render of the same dots,
  0.02 px (8-bit filter weights move a dot's samples, not its centre; measured 0.0007),
  and the floor's slide per metre of viewer against R/(V_z + R) (24.39 mm/m both).
- `--keystone`: the same for a pointed projector's dots, 0.1 px (measured 0.015); white
  against g, kLightRel (1e-4; measured 9.9e-7).
- `--shadow`: binary visibility at a pixel's centre, 1 px; the band measured is the floor
  a texel clear of the step's ramp.
- `--atlens`: Depth 1, at most 1e-5 of the frame's pixels lit by the room only; Depth 4,
  1e-3. The residual is real: at four times a surface's depth the viewer's march still
  catches an arris a texel late now and then (at 1280×720: brick 435 pixels of 921 600,
  plate 60 and one turned away, tiles 16, paper 8; at Depth 1, brick 4). Stated, not
  hidden.
- `--hotspot`: the centroid by symmetry (0.05 px; measured 0.0000), each pixel against
  GGX in double (measured 2.8e-5 relative), and flat paper bit for bit from three
  viewpoints.
- `--ambient`, `--file`: closed forms in double (measured 8.6e-7 relative; heights to
  9e-9 m).
- `--surfaces`: measured off the surface buffer in metres, to within a texel. Brick
  joints are read per run at −0.75R and the median taken; an English bond is told from a
  stretcher bond by its spacing, not its phase, which depends on where the frame's centre
  falls.
- `--dirty`, `--resize`, `--away`, `--shadow`'s band: bit for bit on the GPU; within
  `kRepeat` on the software renderer, for the reason in the traps.
- `--state`: state queries. Raster-independent.

All of it runs at 1280×720 and 320×180 on this GPU, and again on Apple's software
renderer at 320×180, in `verify.sh`.

## The negative controls and mutants

Each check fails against a wrong model (`--negative`, 18): the cues ramping options; the
projector as a point lamp (no 1/cos³α: caught by `--flat`, `--lambert` and `--keystone`);
every surface white; the viewer's ray marched from the lens; the relief seen straight
on, not marched (caught by `--parallax` and `--atlens`); light by the flat wall's angle;
the inverse square to the design screen, not the relief; offset mapping; turned-away
faces lit and unshadowed; no shadow march; the highlight where the normal points at the
lens; no room light; and the surface built once and never again (caught by `--dirty` and
`--resize`).

`tools/mutate.sh` changes one character of the shipped GLSL or C++ 23 times (the beam's
spread, the inverse square, both rays' similar triangles, the shadow's direction and
bias, the normal, sRGB, GGX's denominator, Smith, Schlick's power, the pointed
projector's centre, the occlusion, the brick's course, the corrugation's depth and
cosTurn, the picture's area, the pointed projector's up, the paper's generations,
Exposure, and the viewer's and zinc's values crossing to the GPU) and requires the named
check to fail. All 23 are caught. The recorded GLSL one, for the brief: GGX's denominator
`nh * nh * ( a2 - 1.0 ) + 1.0` → `( a2 + 1.0 )` survives a build and every check but
`--hotspot`, which fails on both the centroid's values and GGX's.

## The sweep's honest list

`tools/sweep.py` sweeps all 29 controls (About excluded), each in a context where it
should act: Paper for Crumple and Seed, Corrugated for Profile, Pitch and Roughness,
Plate for Pattern, Drapes for Pleats and Sag, File (with a written height map or photo)
for Surface File and File Holds. All are live. `--bare` (no context) lists Crumple,
Profile, Pitch, Pattern, Pleats, Sag, Roughness, Surface File and File Holds as dead:
they belong to surfaces the default (Brick) is not, and Roughness only acts on metal and
glaze. That run exits 1 on purpose.

## The browser demo (2026-11-02)

`demo/` is <https://throw-demo.stoatworks-labs.com>, built to the fleet's `resolume-demo` kit
rules (`~/Projects/resolume/specs/DEMO-BRIEF.md`), from slitscan's. What a reader of it must
know:

- **The shaders are the plugin's**: the five pieces plus kVersion, spliced by
  `demo/tools/check_shaders.py --write` into `demo/shaders.js` and compared character for
  character, with the JOIN on both sides (`Assemble( Pass )` is kVersion + kCommon + the
  fragment, InitGL's three stages, plugin.js's copy), and every uniform the three fragments
  declare named in plugin.js (a missed one would sit at 0). All three programs compile and
  link in WebGL2 as spliced. `tools/verify.sh` runs the check.
- **The CPU half is a port**: Controls.cpp, Room.h and Room.cpp in full, in double (between
  the PORT markers in `demo/plugin.js`), and HeightFile.cpp plus the CPU half of Throw.cpp --
  SetFloatParameter, the file's load and generation, ProcessOpenGL with the surface key,
  buildSurface (between the FRAME markers). Change those and you change the port too. Not
  ported: the negative-control hooks (Hooks stays 0), the log, the About block.
- **Same formats**: the surface RGBA32F with linear filtering and the occlusion R16F
  (EXT_color_buffer_float, or the page stops and says so). Without OES_texture_float_linear
  a 32-bit float texture cannot be filtered, so the surface drops to RGBA16F and a line
  under the picture says so.
- **The Surface File is an image the visitor picks**, decoded by the browser at 8 bits and
  widened ×257 as stb_image widens an 8-bit file, so an 8-bit PNG is exact; a 16-bit map
  loses its low bits. Picking one also sets Surface to File. Nothing is uploaded.
- **Seed and Pleats** (FF_TYPE_INTEGER) are dropdowns of every value; the kit has no
  integer control. The Presets menu is the page's.
- **The page's own `differences` list says all of this on the page.** Keep it true.

How it was verified, and how to again (it needs this Mac's GPU and Chrome, so verify.sh does
not run it):

- `demo/tools/crosscheck/crosscheck.py [--size 1280x720]` (after verify.sh, or
  `cmake --build build-verify --target twtest`): the harness's look card, taken from
  `twtest --film 1` at Mix 0, fed to `twtest --pipe` and to the page through
  `window.throwDemo.renderCase` (a fresh instance) in headless Chrome, one frame compared;
  first the page's table of controls against `twtest --list`. 22 cases: the defaults, every
  surface, all four bonds, both corrugation profiles, two plate patterns, the drapes from
  the left with Sag, a dim room and a bright one, the pointed projector on brick and on a
  flat wall, Mix 0.6, all five diagnostic views, File with no file, and an 8-bit height map
  and an 8-bit photo (tiled) the script writes. A case passes when no pixel is more than one
  level of 255 apart and 99% are identical. 2 November 2026, M4 Max, Chrome through ANGLE on
  Metal, surface in RGBA32F: all 29 controls agree; every case within one level everywhere
  at 640 × 360 and 1280 × 720, 99.17% to 100% identical. Negative controls, each caught:
  Throw one slider step (0.001) off is 98.8% identical; Viewer X, Depth and Sag one step
  off are up to 140 levels out at an edge; Lumens one step off is 93.0% identical; the
  paper's Seed one off, 5%. The RGBA16F fallback and every other GPU and browser are not
  compared.

## Not done

- **Never loaded into Resolume on macOS.** On Windows the release workflow's own build
  passed plugin-bench's Arena gate twice in Arena 7.27.1 on software rendering (10 passed:
  35 controls as declared, 28 of 29 live against a still, Surface File inconclusive as a
  file control always is there, log clean). Nothing in Resolume on a GPU.
- No OpenFX port or presets (the browser demo's menu is the page's own). Registered in
  stoatworks-backend and the website's projects.json on 2026-11-02; the user guide is
  `docs/USER-GUIDE.md`.
- Not built: depth of field and a focus control (sub-pixel at real distances, above);
  interreflection; a projector's gamut and white point; normal maps (no height, so no
  parallax and no shadow); more than one projector; a moving viewer driven by the beat.
- Open questions for Allan: whether the default should be a deeper surface (Drapes, or
  Brick at Depth 2) so the effect reads at a glance, at the cost of real scale; whether
  Photo mode should take its height from something better than luminance; whether a
  second projector (two overlapping throws, as a real show would) is worth a 0.2.
