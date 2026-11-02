# throw

A projector throwing the clip onto a real surface, as an FFGL effect for Resolume
Arena/Avenue: crumpled paper, brick, corrugated sheet, tread plate, drapes, weatherboard,
tiles, or a height map of your own. The light leaves a lens at a real place in the room
and the viewer stands somewhere else, so the picture is shaded by angle and distance,
coloured by the wall, shadowed by the relief, slides over it with the viewer, flares off
metal and never reaches black. C++/GLSL, CMake MODULE → universal `.bundle` (macOS). MIT.
ID `TW01`, display name `SW Throw`. The C++ namespace is `throwfx` (`throw` is a keyword).

Read `AGENTS.md` before touching the light law, the viewer's march, the shadow march, the
surface's exact heights or the harness's tolerances.

## Commands (CMake)
- Configure: `cmake -B build -DCMAKE_BUILD_TYPE=Release`
- Fast dev build: add `-DCMAKE_OSX_ARCHITECTURES=arm64`
- Build: `cmake --build build`
- Install to Resolume: `cmake --install build` (writes into Arena's Extra Effects)
- Render offline: `./build/twtest --out /tmp/frame.png` (the look card on brick, at the defaults)
- A picture of your own: `./build/twtest --out /tmp/frame.png --clip still.png --set "Surface=Drapes"`
- List parameters: `./build/twtest --list`
- Set anything by name: `--set "Surface=Corrugated" --set "Depth=0.8"` (options by name,
  Surface File by path, everything else the host's 0..1)
- Frames before the one written: `--frames N` (default 2; nothing moves with time)
- Film: `./build/twtest --film 300 --size 1280x720 --script cues | ffmpeg -f rawvideo -pix_fmt rgba -s 1280x720 -r 60 -i - out.mp4`
- A clip through it: `ffmpeg -i in.mov -f rawvideo -pix_fmt rgba - | ./build/twtest --pipe --size WxH | ffmpeg …`
- The README's figures: `tools/figures.py` (needs ffmpeg, Pillow and Resolume's bundled
  clips); `--film walk.mp4` adds the viewer walking past the drapes.
- The metal table: `python3 tools/f0.py --check` (zinc and aluminium F0 from published n,k
  against `source/Room.h`).

## Verify
- Everything: `tools/verify.sh` (greps for GLSL reserved words, trig and WebGL2 gaps,
  no shader string literal over 15 KB for MSVC, glslc, the metal table, a fresh universal
  build, every check at 1280x720 and 320x180, again on Apple's software renderer, the
  pipe, the negative controls, the mutants, the sweep, lipo, plist, ad-hoc signature,
  `oxbow probe` and `selftest`, the bench; about a quarter of an hour, most of it the
  software renderer and the mutants). `OXBOW=` points at an oxbow binary if `~/Projects/resolume/oxbow/build/oxbow`
  isn't built.
- **The light**: `--flat` (a flat wall gives the clip back, times its albedo, from any
  projector and viewer), `--lambert` (every pixel of tilted planes is d²cosθ/(r²cos³α)),
  `--away` (faces turned from the lens get the room only), `--ambient` (black clip →
  A + b in closed form), `--keystone` (Pointed: dots where the pinhole puts them, light
  per pixel).
- **The relief**: `--parallax` (a recess's lines where the closed form puts them),
  `--shadow` (a step's shadow R(x_e − C_x)/T long), `--atlens` (from the lens, no shadow
  on any surface), `--hotspot` (the lens's highlight at the mirror point, GGX in double).
- **The surfaces**: `--surfaces` (brick, bonds, joints, corrugation, plate, pleats, paper,
  measured off the Height view), `--file` (16- and 8-bit maps come back exact, Photo's
  albedo, missing files are Flat, Scale tiles).
- **The plumbing**: `--dirty`, `--resize`, `--state`; no GL: `--units`, `--cues`,
  `--names`; `--offline` is those plus their negatives (what CI runs).
- The checks can fail: `--negative` (18 wrong models), `tools/mutate.sh` (23 mutants).
- No dead controls: `python3 tools/sweep.py` (29 swept, all live; `--bare` lists the
  surface-specific ones as dead, on purpose).
- Render cost: `./build/twtest --bench`.
- On the software renderer: `TWTEST_RENDERER=software ./build/twtest --flat --size 320x180`.
- CI (`.github/workflows/ci.yml`, public repo): macOS arm64 build, `twtest --list` and
  `--offline`, glslc; a Windows x64 build through vcpkg's GLEW. No pixel checks: the
  runner has no GL. If the repo is ever made private, delete the workflows first
  (private repos share one small Actions allowance).
- Release (`.github/workflows/release.yml`, slitscan's): a `v*` tag builds the universal
  bundle and the Windows DLL, packages and publishes; a manual dispatch builds and
  publishes nothing. `tools/wincheck.ps1 -Params 34` is the 29 controls plus the About
  block's text and four buttons.

## Notes
- **Metres throughout.** The wall's front plane is z = 0, the relief behind it down to
  −R; the frame is W × H on the wall, H = Wall Height. `source/Room.{h,cpp}` turns host
  parameters into the room once a frame, in double.
- **Two cached passes and one per frame.** The surface (albedo + height, RGBA32F) and its
  occlusion are rebuilt only when `SurfaceKey` changes; the projection runs every frame.
- **Exact heights near anything that decides.** A GPU's bilinear weights are 8-bit, so
  the normal, the hit refinement and the first two texels of the shadow march read the
  height with `heightExact` (four `texelFetch`es, float weights). `fileAt` does the same
  for the Surface File, so a map at the frame's raster is a copy.
- **No trig in the GLSL.** The corrugation and the folds use `cosTurn` (Taylor to 1e-10);
  the projector's basis comes from the CPU. `verify.sh` greps.
- All host parameters are 0..1 and mapped in `Controls.cpp`; options hold the element
  index. `Surface File` is an FFGL file parameter.
- GLSL reserved words (`patch sample input output filter common active half layout
  flat packed`, `noise1`–`noise4`) must not be identifiers; `verify.sh` greps for them.
- Override `SetTextParameter` to return `FF_SUCCESS` for the About block, or no host can
  instantiate the plugin.
- `throw_core` is an OBJECT library: the plugin registers itself from a file-scope
  constructor nothing references.
- The macOS build must be universal. Check with `lipo`, never the build log.
- `source/StoatworksAbout.h` and `ATTRIBUTIONS.md` are generated by stoatworks-backend's
  `sync-about.py` and `sync-attributions.py` from the website's projects.json and the
  backend's attribution masters. Edit those, never the files here.

## Not done yet
- Never loaded into Resolume on macOS. On Windows the v0.1.0 build passed the fleet's Arena
  gate in Arena 7.27.1 on software rendering (plugin-bench `arena/expect/throw.json`).
- No presets, no OpenFX port.

## Browser demo
- `demo/` is <https://throw-demo.stoatworks-labs.com>; `.github/workflows/deploy.yml`
  deploys it on a push to main (a route, not a custom domain: `wrangler.toml`).
- After a shader edit: `python3 demo/tools/check_shaders.py --write` (verify.sh fails on drift).
- Locally: `python3 -m http.server -d demo`, or `npx wrangler dev` for the real `_headers`.
- Against the plugin: `python3 demo/tools/crosscheck/crosscheck.py` (needs `build-verify/twtest`,
  Chrome and a GPU). AGENTS.md has the gaps and the last figures.

## Diagnostics

`source/Diag.{h,cpp}`: log file only, no crash handler (this runs inside Resolume).

    ~/Library/Logs/throw/throw.YYYY-MM-DD.log
    %LOCALAPPDATA%\throw\logs\throw.YYYY-MM-DD.log

`THROW_LOG_DIR` overrides the folder.
