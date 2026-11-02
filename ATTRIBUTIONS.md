# Attributions

Throw is built on other people's work. This file lists what that work is, who did it,
and what it is doing here.

In the released plugins this file is generated from the master lists in the
`stoatworks-backend` repo by `scripts/sync-attributions.py`. Throw has not been
registered there yet (that waits for a release), so for now it is written by hand in the
same shape. Registering it replaces this file; carry anything new across first.

## Code we derived from other people's work

Someone else solved this first, and this project would not exist in its current form without their work.

### The effect's shape, the GL state guard, the pass buffers, the log and the tooling — Stoatworks slitscan

<https://github.com/stoatworks-labs/slitscan>  
Licence: MIT  
Copyright: Stoatworks Labs

The plugin class's shape and its table of controls, source/GLState.h, source/PassBuffer.{h,cpp} and source/Diag.{h,cpp} (by way of radar, vectrix, millpond and schlieren); the harness's rig, PNG writer, cue sheets, --pipe/--film, --negative with its Perturb, --state, --names, --cues and --offline; and tools/verify.sh, tools/mutate.sh, tools/glslc.sh, tools/sweep.py, tools/wincheck.ps1 and the two workflows. All adapted.

### The highlight and the metal table — Stoatworks repousse

<https://github.com/stoatworks-labs/repousse>  
Licence: MIT  
Copyright: Stoatworks Labs

GGX with height-correlated Smith visibility and Schlick's Fresnel, as repousse lights its metal; and tools/f0.py, which computes a metal's F0 from published n,k, here run for zinc and aluminium.

### The file parameter and 16-bit height maps — Stoatworks knockout and isohypse

<https://github.com/stoatworks-labs/knockout>  
<https://github.com/stoatworks-labs/isohypse>  
Licence: MIT  
Copyright: Stoatworks Labs

How a file path arrives as an FFGL file parameter and is held under a lock until the GL thread reads it (knockout), and reading PNGs through stb_image at 16 bits a channel so a 16-bit height map keeps its precision (isohypse). source/HeightFile.{h,cpp}.

## Third-party code this project uses

Libraries, SDKs and frameworks the project is built on or bundles.

### Resolume FFGL SDK

<https://github.com/resolume/ffgl>  
Licence: BSD-3-Clause  
Copyright: FreeFrame

Vendored as a git submodule at external/ffgl.

The plugin ABI itself. An FFGL effect or source is defined by this SDK's headers — there is no other way to be loadable by Resolume Arena and Avenue.

### GLEW — the OpenGL Extension Wrangler Library

<https://github.com/nigels-com/glew>  
Licence: BSD-3-Clause (with Mesa 3-D and Khronos components)  
Copyright: Milan Ikits, Marcelo E. Magallon and Lev Povalahev

Arrives inside the FFGL submodule at external/ffgl/deps/glew-2.1.0. Not fetched separately.

Resolves OpenGL entry points on Windows, where the system headers stop at OpenGL 1.1.

### stb_image — Sean Barrett

<https://github.com/nothings/stb>  
Licence: public domain (Unlicense) or MIT, at your choice  
Copyright: Sean Barrett and contributors

v2.30, vendored at external/stb/stb_image.h. Reads the Surface File: PNG at 8 or 16 bits, JPEG, and the other formats it knows.

### libpng

<http://www.libpng.org/pub/png/libpng.html>  
Licence: PNG Reference Library License (libpng)  
Copyright: the PNG Reference Library authors

Arrives inside the FFGL submodule, under the SDK's CustomThumbnail sample.

Part of the upstream SDK tree rather than something these plugins call directly — listed because it is present in the checkout.

## Data

### Optical constants of zinc and aluminium — refractiveindex.info

<https://refractiveindex.info>  
Licence: CC0 1.0  
Copyright: Mikhail N. Polyanskiy (database); the measurements are the authors' below

tools/f0-data/Zn-Werner.yml: W. S. M. Werner, K. Glantschnig and C. Ambrosch-Draxl, "Optical constants and inelastic electron-scattering data for 17 elemental metals", J. Phys. Chem. Ref. Data 38, 1013 (2009). tools/f0-data/Al-Rakic.yml: A. D. Rakić, "Algorithm for the determination of intrinsic optical constants of metal films: application to aluminum", Appl. Opt. 34, 4755 (1995). tools/f0.py turns them into the galvanised sheet's and the tread plate's F0 in source/Room.h.

### CIE 1931 colour-matching functions and illuminant D65 — CVRL

<http://cvrl.org>  
Licence: the CIE's standard tables, freely published by CVRL  
Copyright: CIE; tabulated by the Colour & Vision Research Laboratory, UCL

tools/f0-data/ciexyz31.csv and d65.csv: what tools/f0.py integrates a metal's reflectance spectrum against.

## Work we checked ourselves against

No code was taken from these — but they were how we knew we had it right, and that is worth saying out loud.

### Microfacet models for refraction through rough surfaces — B. Walter, S. R. Marschner, H. Li and K. E. Torrance (EGSR 2007)

The GGX distribution in source/Shaders.cpp. twtest --hotspot checks the highlight's centroid against the mirror point and its values against GGX in double.

### Understanding the masking-shadowing function in microfacet-based BRDFs — E. Heitz (JCGT 2014)

The height-correlated Smith visibility term used with GGX.

### An inexpensive BRDF model for physically-based rendering — C. Schlick (Computer Graphics Forum 1994)

The Fresnel approximation from F0.

## Standards and published specifications

What the implementation is measured against.

- **The inverse-square and cosine laws of illumination** — irradiance from a point source is I cos θ / r². A projector built to light its design screen evenly has intensity ∝ 1/cos³α off its axis, so the light at a surface point relative to that screen is d²·cos θ/(r²·cos³α), exactly 1 on the flat screen. twtest --lambert checks every pixel of tilted planes against it in double.
- **BS EN 771-1 (and BS 3921 before it)** — the UK metric brick's work size, 215 × 102.5 × 65 mm, laid with 10 mm joints. The bonds (stretcher, English, Flemish, stack) are the traditional layouts built from it.
- **The 3-inch corrugated profile** — 76 mm pitch and 18 mm deep, as galvanised corrugated sheet is commonly sold. A trade profile, not checked against a standard.
- **IEC 61966-2-1 (sRGB)** — the clip is linearised so the light adds as light does, then encoded again; the albedos are linear sRGB.

## Getting this wrong

If your work is here and the description is inaccurate, the licence is wrong, or you would rather not be listed — open an issue and it will be fixed.
