/**
 * Throw — browser demo.
 *
 * A projector throwing the clip onto a real surface. The one idea, from
 * `AGENTS.md`: **a projector is a lamp with a picture in it, and you are not
 * standing at the lens.** Every pixel of the clip is a ray from a pinhole; where
 * it meets the relief it brings light by the clip's value, the cosine and the
 * inverse square; the wall gives that back in its own colour; and the viewer,
 * somewhere else, sees each point of the relief from their own angle.
 *
 * Two halves, and they are not equally faithful here:
 *
 *   **The GPU half is the plugin's own GLSL.** `shaders.js` is the five
 *   `R"( ... )"` pieces of `source/Shaders.cpp` plus kVersion, spliced by
 *   `demo/tools/check_shaders.py --write` and never typed; the same script
 *   compares them character for character, checks they are joined as
 *   `Assemble()` and InitGL join them and that every uniform they declare is
 *   named here, and `tools/verify.sh` runs it. They run as the plugin's three
 *   passes -- the surface (albedo and height in metres) and its occlusion,
 *   rebuilt only when the surface key changes, and the projection every frame
 *   -- on the plugin's formats: RGBA32F for the surface, R16F for the
 *   occlusion, which need EXT_color_buffer_float.
 *
 *   **The CPU half is a port.** Between the PORT markers below: Controls.cpp
 *   (every mapping, the names, groups, order and defaults) and Room.h /
 *   Room.cpp (the facts, the room, the relief, the paper's generations, the
 *   metals), in double, crossing to the GPU as floats as the plugin's do.
 *   Between the FRAME markers: HeightFile.cpp's reading of a Surface File and
 *   the CPU half of Throw.cpp -- ProcessOpenGL (the file, the surface key) and
 *   buildSurface -- which turn the controls into each pass's uniforms.
 *   `demo/tools/crosscheck/` renders the plugin's own harness (`twtest
 *   --pipe`) and this page over the same card and compares the pictures; it
 *   needs Chrome and a GPU, so it is run by hand (AGENTS.md).
 *
 * What the page cannot do as the plugin does, all said on the page: read a
 * Surface File at 16 bits a channel (the browser decodes 8), and filter a
 * 32-bit float texture on a browser without OES_texture_float_linear (the
 * surface drops to RGBA16F there, and the page says so).
 */

import { Program, PassBuffer, bindTexture } from './vendor/gl.js';
import { mountDemo } from './vendor/demo.js';
import * as S from './shaders.js';

const f32 = Math.fround;
const clampTo = (v, lo, hi) => (v < lo ? lo : v > hi ? hi : v);

// ==== BEGIN PORT ===========================================================
//
// Controls.h / Controls.cpp. Every ranged host parameter is 0..1, because
// SetParamInfo clamps an FF_TYPE_STANDARD default into 0..1 before a range
// could be attached; the conversions are here, one per control. Option
// parameters hold the element index. Pleats and Seed are real integers
// (FF_TYPE_INTEGER is exempt from the clamp). Surface File is a path.

const PT = {
  SURFACE: 0, WALL_HEIGHT: 1, SCALE: 2, DEPTH: 3, SEED: 4,
  CRUMPLE: 5, BOND: 6, MORTAR: 7, PROFILE: 8, PITCH: 9, PATTERN: 10, PLEATS: 11, SAG: 12, ROUGHNESS: 13,
  FILE: 14, FILE_HOLDS: 15,
  PROJECTOR_X: 16, PROJECTOR_Y: 17, THROW: 18, AIM: 19, LUMENS: 20, BLACK_LEVEL: 21,
  AMBIENT: 22, EXPOSURE: 23,
  VIEWER_X: 24, VIEWER_Y: 25, VIEWER_DISTANCE: 26,
  VIEW: 27, MIX: 28,
  ABOUT_FIRST: 29,
};

const Kind = { Standard: 0, Option: 1, Integer: 2, File: 3 };

const Surface = { Flat: 0, Paper: 1, Brick: 2, Corrugated: 3, Plate: 4, Drapes: 5, Weatherboard: 6, Tiles: 7, File: 8, Count: 9 };
const Bond = { Stretcher: 0, English: 1, Flemish: 2, Stack: 3, Count: 4 };
const Profile = { Sine: 0, Box: 1, Count: 2 };
const Pattern = { Diamond: 0, FiveBar: 1, Crosshatch: 2, Count: 3 };
const FileHolds = { Height: 0, Photo: 1, Count: 2 };
const Aim = { LensShift: 0, Pointed: 1, Count: 2 };
const View = { Picture: 0, Height: 1, Normals: 2, Light: 3, Shadow: 4, Albedo: 5, Count: 6 };

const clamp01 = (v) => clampTo(v, 0.0, 1.0);
const geometric = (v, low, high) => low * Math.pow(high / low, clamp01(v));
function inverseGeometric(value, low, high) {
  const lo = Math.min(low, high);
  const hi = Math.max(low, high);
  return f32(Math.log(clampTo(value, lo, hi) / low) / Math.log(high / low));
}
/** most * v^2: exact 0 at the bottom, fine control near it. */
function quadratic(v, most) {
  const x = clamp01(v);
  return most * x * x;
}
const inverseQuadratic = (value, most) => f32(Math.sqrt(clampTo(value / most, 0.0, 1.0)));
/** -most..+most, quadratically about the middle: exact 0 at 0.5. */
function signedQuadratic(v, most) {
  const s = 2.0 * clamp01(v) - 1.0;
  return most * s * Math.abs(s);
}
function inverseSignedQuadratic(value, most) {
  const s = clampTo(value / most, -1.0, 1.0);
  const r = Math.sqrt(Math.abs(s));
  return f32(0.5 + 0.5 * (s < 0.0 ? -r : r));
}

const kWallLow = 0.25, kWallHigh = 20.0;
const kScaleLow = 0.25, kScaleHigh = 4.0;
const kDepthMost = 4.0;
const kMortarLow = 0.003, kMortarHigh = 0.025;
const kPitchLow = 0.025, kPitchHigh = 0.300;
const kRoughLow = 0.05, kRoughHigh = 0.8;
const kOffsetMost = 10.0;
const kDistanceLow = 1.0, kDistanceHigh = 50.0;
const kLumensLow = 500.0, kLumensHigh = 50000.0;
const kBlackMost = 0.02;
const kAmbientMost = 1000.0;
const kStopsMost = 4.0;

const kSurfaceNames = ['Flat', 'Paper', 'Brick', 'Corrugated', 'Plate', 'Drapes', 'Weatherboard', 'Tiles', 'File'];
const kBondNames = ['Stretcher', 'English', 'Flemish', 'Stack'];
const kProfileNames = ['Sine', 'Box'];
const kPatternNames = ['Diamond', 'Five-Bar', 'Crosshatch'];
const kHoldsNames = ['Height', 'Photo'];
const kAimNames = ['Lens Shift', 'Pointed'];
const kViewNames = ['Picture', 'Height', 'Normals', 'Light', 'Shadow', 'Albedo'];

/** std::lround on the values a host sends: half away from zero. */
const lround = (v) => (v < 0 ? -Math.round(-v) : Math.round(v));
const OptionIndex = (value, count) => clampTo(lround(value), 0, count - 1);

const WallHeightFromParam = (value) => geometric(value, kWallLow, kWallHigh);
const ParamFromWallHeight = (metres) => inverseGeometric(metres, kWallLow, kWallHigh);
const ScaleFromParam = (value) => geometric(value, kScaleLow, kScaleHigh);
const ParamFromScale = (scale) => inverseGeometric(scale, kScaleLow, kScaleHigh);
const DepthFromParam = (value) => quadratic(value, kDepthMost);
const ParamFromDepth = (depth) => inverseQuadratic(depth, kDepthMost);
const MortarFromParam = (value) => kMortarLow + (kMortarHigh - kMortarLow) * clamp01(value);
const ParamFromMortar = (metres) => f32(clampTo((metres - kMortarLow) / (kMortarHigh - kMortarLow), 0.0, 1.0));
const PitchFromParam = (value) => geometric(value, kPitchLow, kPitchHigh);
const ParamFromPitch = (metres) => inverseGeometric(metres, kPitchLow, kPitchHigh);
const RoughnessFromParam = (value) => kRoughLow + (kRoughHigh - kRoughLow) * clamp01(value);
const ParamFromRoughness = (roughness) => f32(clampTo((roughness - kRoughLow) / (kRoughHigh - kRoughLow), 0.0, 1.0));
const OffsetFromParam = (value) => signedQuadratic(value, kOffsetMost);
const ParamFromOffset = (metres) => inverseSignedQuadratic(metres, kOffsetMost);
const DistanceFromParam = (value) => geometric(value, kDistanceLow, kDistanceHigh);
const ParamFromDistance = (metres) => inverseGeometric(metres, kDistanceLow, kDistanceHigh);
const LumensFromParam = (value) => geometric(value, kLumensLow, kLumensHigh);
const ParamFromLumens = (lumens) => inverseGeometric(lumens, kLumensLow, kLumensHigh);
const BlackLevelFromParam = (value) => quadratic(value, kBlackMost);
const ParamFromBlackLevel = (fraction) => inverseQuadratic(fraction, kBlackMost);
const AmbientFromParam = (value) => quadratic(value, kAmbientMost);
const ParamFromAmbient = (lux) => inverseQuadratic(lux, kAmbientMost);
const ExposureFromParam = (value) => Math.pow(2.0, kStopsMost * (2.0 * clamp01(value) - 1.0));
const ParamFromExposure = (gain) =>
  f32(clampTo(0.5 + Math.log2(Math.max(gain, 1e-9)) / (2.0 * kStopsMost), 0.0, 1.0));

const standard = (id, name, group, value) =>
  ({ id, name, group, kind: Kind.Standard, defaultValue: f32(value), options: null, low: 0, high: 1 });
const option = (id, name, group, value, names) =>
  ({ id, name, group, kind: Kind.Option, defaultValue: f32(value), options: names, low: 0, high: 1 });
const integer = (id, name, group, value, low, high) =>
  ({ id, name, group, kind: Kind.Integer, defaultValue: f32(value), options: null, low, high });
const file = (id, name, group) =>
  ({ id, name, group, kind: Kind.File, defaultValue: 0.0, options: null, low: 0, high: 1 });

// The defaults: a brick wall 2.5 m high in the frame, a 10,000 lumen
// projector a metre above its centre at 6 m, a dim room (20 lux), and
// somebody standing 2 m to the right of it and a little below, 4 m back.
// Everything at its real size and its real depth.
const kTable = [
  option(PT.SURFACE, 'Surface', 'Surface', Surface.Brick, kSurfaceNames),
  standard(PT.WALL_HEIGHT, 'Wall Height', 'Surface', ParamFromWallHeight(2.5)),
  standard(PT.SCALE, 'Scale', 'Surface', ParamFromScale(1.0)),
  standard(PT.DEPTH, 'Depth', 'Surface', ParamFromDepth(1.0)),
  integer(PT.SEED, 'Seed', 'Surface', 1.0, 0.0, 99.0),

  standard(PT.CRUMPLE, 'Crumple', 'Paper', 0.7),
  option(PT.BOND, 'Bond', 'Brick', 0.0, kBondNames),
  standard(PT.MORTAR, 'Mortar', 'Brick', ParamFromMortar(0.010)),
  option(PT.PROFILE, 'Profile', 'Corrugated', 0.0, kProfileNames),
  standard(PT.PITCH, 'Pitch', 'Corrugated', ParamFromPitch(0.076)),
  option(PT.PATTERN, 'Pattern', 'Plate', 0.0, kPatternNames),
  integer(PT.PLEATS, 'Pleats', 'Drapes', 16.0, 2.0, 60.0),
  standard(PT.SAG, 'Sag', 'Drapes', 0.3),
  standard(PT.ROUGHNESS, 'Roughness', 'Finish', ParamFromRoughness(0.35)),

  file(PT.FILE, 'Surface File', 'File'),
  option(PT.FILE_HOLDS, 'File Holds', 'File', 0.0, kHoldsNames),

  standard(PT.PROJECTOR_X, 'Projector X', 'Projector', ParamFromOffset(0.0)),
  standard(PT.PROJECTOR_Y, 'Projector Y', 'Projector', ParamFromOffset(1.0)),
  standard(PT.THROW, 'Throw', 'Projector', ParamFromDistance(6.0)),
  option(PT.AIM, 'Aim', 'Projector', 0.0, kAimNames),
  standard(PT.LUMENS, 'Lumens', 'Projector', ParamFromLumens(10000.0)),
  standard(PT.BLACK_LEVEL, 'Black Level', 'Projector', ParamFromBlackLevel(0.0005)),

  standard(PT.AMBIENT, 'Ambient', 'Room', ParamFromAmbient(20.0)),
  standard(PT.EXPOSURE, 'Exposure', 'Room', ParamFromExposure(1.0)),

  standard(PT.VIEWER_X, 'Viewer X', 'Viewer', ParamFromOffset(2.0)),
  standard(PT.VIEWER_Y, 'Viewer Y', 'Viewer', ParamFromOffset(-0.8)),
  standard(PT.VIEWER_DISTANCE, 'Viewer Distance', 'Viewer', ParamFromDistance(4.0)),

  option(PT.VIEW, 'View', 'Output', 0.0, kViewNames),
  standard(PT.MIX, 'Mix', 'Output', 1.0),
];
if (kTable.length !== PT.ABOUT_FIRST || kTable.some((row, i) => row.id !== i)) {
  throw new Error('every control needs a row in kTable, in ParamId order');
}

const InfoOf = (id) => kTable[Math.min(id, PT.ABOUT_FIRST - 1)];

// Room.h / Room.cpp. Metres throughout: the wall's front plane is z = 0, +z
// towards the room, +y up; the frame is x in [-W/2, W/2], y in [-H/2, H/2];
// every surface is a height field in [-relief, 0].

/** What each surface is made of, and how big it is at Scale 1, Depth 1. */
const facts = {
  kPaperFacet: 0.12, kPaperRelief: 0.015, kPaperShrink: 0.45, kPaperLayers: 4,
  kBrickLength: 0.215, kBrickHeight: 0.065, kHeaderLength: 0.1025, kJointRecess: 0.008,
  kSineDepthPerPitch: 18.0 / 76.0, kBoxDepthPerPitch: 35.0 / 200.0,
  kPlateCell: 0.030, kLugHeight: 0.0015,
  kFoldDepthPerSpacing: 0.5,
  kBoard: 0.150, kBoardLip: 0.015,
  kTileLength: 0.200, kTileHeight: 0.100, kGrout: 0.003, kBevel: 0.006, kBevelDrop: 0.003, kTileRecess: 0.0045,
  kFileRelief: 0.10,
  kZincF0: [0.875, 0.869, 0.855],
  kAluminiumF0: [0.907, 0.916, 0.922],
  kGlazeF0: 0.04,
};

/** Which parts of a surface take a specular highlight. */
const Glaze = { None: 0, All: 1, Tiles: 2 };

const vec3 = (x = 0.0, y = 0.0, z = 0.0) => ({ x, y, z });
const sub = (a, b) => vec3(a.x - b.x, a.y - b.y, a.z - b.z);
const cross = (a, b) => vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
const length = (a) => Math.sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
function unit(a) {
  const l = length(a);
  return l > 0.0 ? vec3(a.x / l, a.y / l, a.z / l) : vec3(0.0, 0.0, -1.0);
}

/** The paper's generations: each arrives as Crumple rises past l / 4, over a quarter of its travel. */
const PaperWeight = (crumple, layer) => clampTo(4.0 * crumple - layer, 0.0, 1.0);

/** std::round: half away from zero, as lround. */
const round = lround;

/** R, the deepest a surface goes behind its front plane, for these parameters. */
function ReliefOf(surface, p, frameWidth) {
  const scale = ScaleFromParam(p[PT.SCALE]);
  const depth = DepthFromParam(p[PT.DEPTH]);
  switch (surface) {
    case Surface.Flat: return 0.0;
    case Surface.Paper: {
      // Every corner of every generation is within +-amp of its plane, so the
      // sum spans twice the weighted amplitudes.
      const crumple = clampTo(p[PT.CRUMPLE], 0.0, 1.0);
      let sum = 0.0;
      let amp = facts.kPaperRelief;
      for (let l = 0; l < facts.kPaperLayers; l += 1, amp *= facts.kPaperShrink) sum += PaperWeight(crumple, l) * amp;
      return 2.0 * sum * scale * depth;
    }
    case Surface.Brick: return facts.kJointRecess * scale * depth;
    case Surface.Corrugated: {
      const profile = OptionIndex(p[PT.PROFILE], Profile.Count);
      const ratio = profile === Profile.Sine ? facts.kSineDepthPerPitch : facts.kBoxDepthPerPitch;
      return ratio * PitchFromParam(p[PT.PITCH]) * scale * depth;
    }
    case Surface.Plate: return facts.kLugHeight * scale * depth;
    case Surface.Drapes: {
      const pleats = clampTo(round(p[PT.PLEATS]), 2.0, 60.0);
      return facts.kFoldDepthPerSpacing * scale * frameWidth / pleats * depth;
    }
    case Surface.Weatherboard: return facts.kBoardLip * scale * depth;
    case Surface.Tiles: return facts.kTileRecess * scale * depth;
    case Surface.File: return facts.kFileRelief * scale * depth;
    default: return 0.0;
  }
}

/** The room for these host parameters at this raster. */
function MakeRoom(p, width, height) {
  const room = {
    W: 0.0, H: 0.0, lens: vec3(), eye: vec3(), aim: Aim.LensShift, axis: vec3(), right: vec3(), up: vec3(),
    design: 0.0, relief: 0.0, roomRatio: 0.0, blackLevel: 0.0, gain: 1.0, alpha: 0.0, f0: [0.0, 0.0, 0.0],
    glaze: Glaze.None, glazeFloor: 0.0, pleatSpacing: 0.0, paperWeights: [0.0, 0.0, 0.0, 0.0],
  };
  room.H = WallHeightFromParam(p[PT.WALL_HEIGHT]);
  room.W = room.H * Math.max(width, 1) / Math.max(height, 1);

  room.lens = vec3(OffsetFromParam(p[PT.PROJECTOR_X]), OffsetFromParam(p[PT.PROJECTOR_Y]), DistanceFromParam(p[PT.THROW]));
  room.eye = vec3(OffsetFromParam(p[PT.VIEWER_X]), OffsetFromParam(p[PT.VIEWER_Y]), DistanceFromParam(p[PT.VIEWER_DISTANCE]));
  room.aim = OptionIndex(p[PT.AIM], Aim.Count);

  if (room.aim === Aim.LensShift) {
    // Square to the wall; it lights the wall itself evenly.
    room.axis = vec3(0.0, 0.0, -1.0);
    room.right = vec3(1.0, 0.0, 0.0);
    room.up = vec3(0.0, 1.0, 0.0);
    room.design = room.lens.z;
  } else {
    // At the frame's centre, horizon level: right is square to the axis and
    // to the world's up, and up completes a right-handed view.
    const toCentre = sub(vec3(), room.lens);
    room.axis = unit(toCentre);
    room.right = unit(cross(room.axis, vec3(0.0, 1.0, 0.0)));
    room.up = cross(room.right, room.axis);
    room.design = length(toCentre);
  }

  const surface = OptionIndex(p[PT.SURFACE], Surface.Count);
  room.relief = ReliefOf(surface, p, room.W);

  // The projector's lumens over its picture, W x H on its design screen; the
  // room's lux on the wall. Their ratio is all the picture needs.
  room.roomRatio = AmbientFromParam(p[PT.AMBIENT]) * room.W * room.H / LumensFromParam(p[PT.LUMENS]);
  room.blackLevel = BlackLevelFromParam(p[PT.BLACK_LEVEL]);
  room.gain = ExposureFromParam(p[PT.EXPOSURE]);

  const rough = RoughnessFromParam(p[PT.ROUGHNESS]);
  room.alpha = rough * rough;
  switch (surface) {
    case Surface.Corrugated:
      room.glaze = Glaze.All;
      room.f0 = facts.kZincF0.slice();
      break;
    case Surface.Plate:
      room.glaze = Glaze.All;
      room.f0 = facts.kAluminiumF0.slice();
      break;
    case Surface.Tiles:
      room.glaze = Glaze.Tiles;
      room.f0 = [facts.kGlazeF0, facts.kGlazeF0, facts.kGlazeF0];
      // Halfway between the bevel's foot and the grout.
      room.glazeFloor = 0.5 * (facts.kBevelDrop + facts.kTileRecess) * ScaleFromParam(p[PT.SCALE]) * DepthFromParam(p[PT.DEPTH]);
      break;
    default: room.glaze = Glaze.None; break;
  }

  const pleats = clampTo(round(p[PT.PLEATS]), 2.0, 60.0);
  room.pleatSpacing = ScaleFromParam(p[PT.SCALE]) * room.W / pleats;
  const crumple = clampTo(p[PT.CRUMPLE], 0.0, 1.0);
  for (let l = 0; l < facts.kPaperLayers; l += 1) room.paperWeights[l] = PaperWeight(crumple, l);
  return room;
}

// ==== END PORT =============================================================

// ==== BEGIN FRAME ==========================================================
//
// HeightFile.cpp and the CPU half of Throw.cpp: what ProcessOpenGL and
// buildSurface do with the controls each frame.

/** Shaders.h's negative-control hooks. The page's are always 0, as a shipped instance's are. */
const kHookStaleSurface = 1 << 10;

function setInt2(program, name, x, y) {
  const location = program.location(name);
  if (location === null) program.missing.add(name);
  else program.gl.uniform2i(location, x | 0, y | 0);
}

/** sRGB code value to linear light, IEC 61966-2-1, in double. */
const decode = (code) => (code <= 0.04045 ? code / 12.92 : Math.pow((code + 0.055) / 1.055, 2.4));

/**
 * HeightFile: the Surface File, decoded once and held as a texture the surface
 * pass samples. The plugin decodes with stb_image at 16 bits a channel; here
 * the browser has decoded it at 8 (decodeImage below), which stb_image's
 * widening of an 8-bit file (v * 257) turns into the same 16-bit values.
 */
class HeightFile {
  constructor(gl, floatLinear) {
    this.gl = gl;
    this.floatLinear = floatLinear;
    this.pixels = null; // RGBA, bottom row first, 16 bits a channel
    this.width = 0;
    this.height = 0;
    this.texture = null;
  }

  /** Read `picked` (decodeImage's answer). False, with the reason, if it will not. */
  Load(picked, out) {
    this.pixels = null;
    this.width = 0;
    this.height = 0;
    if (picked.rgba === null) {
      out.reason = picked.reason ?? 'it could not be decoded';
      return false;
    }
    const w = picked.width;
    const h = picked.height;
    const most = this.gl.getParameter(this.gl.MAX_TEXTURE_SIZE);
    if (most > 0 && (w > most || h > most)) {
      out.reason = `${w}x${h} is larger than this GPU's ${most} texels`;
      return false;
    }
    this.width = w;
    this.height = h;
    const pixels = new Uint16Array(w * h * 4);
    const row = w * 4;
    for (let y = 0; y < h; y += 1) {
      const from = (h - 1 - y) * row;
      const to = y * row;
      for (let i = 0; i < row; i += 1) pixels[to + i] = picked.rgba[from + i] * 257;
    }
    this.pixels = pixels;
    return true;
  }

  /** Build the texture for this reading of the file. */
  Upload(photo) {
    if (this.pixels === null) return false;
    const gl = this.gl;
    const count = this.width * this.height;
    const texels = new Float32Array(count * 4);
    const p = this.pixels;

    // Luminance from the code values, Rec. 709's weights: a photograph's
    // darks as its recesses, measured the way the eye ranks them.
    let low = 1.0;
    let high = 0.0;
    if (photo) {
      for (let i = 0; i < count; i += 1) {
        const y = (0.2126 * p[i * 4] + 0.7152 * p[i * 4 + 1] + 0.0722 * p[i * 4 + 2]) / 65535.0;
        low = Math.min(low, y);
        high = Math.max(high, y);
      }
    }
    const span = high - low;

    for (let i = 0; i < count; i += 1) {
      const t = i * 4;
      if (photo) {
        const y = (0.2126 * p[t] + 0.7152 * p[t + 1] + 0.0722 * p[t + 2]) / 65535.0;
        for (let c = 0; c < 3; c += 1) texels[t + c] = decode(p[t + c] / 65535.0);
        texels[t + 3] = span > 0.0 ? (y - low) / span : 1.0;
      } else {
        // A grey height map: the red channel is the grey.
        texels[t] = 1.0;
        texels[t + 1] = 1.0;
        texels[t + 2] = 1.0;
        texels[t + 3] = p[t] / 65535.0;
      }
    }

    // Somebody else's error is not ours to report.
    for (let i = 0; i < 16 && gl.getError() !== gl.NO_ERROR; i += 1) { /* drain */ }
    if (this.texture === null) this.texture = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.texture);
    gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, this.width, this.height, 0, gl.RGBA, gl.FLOAT, texels);
    // The plugin's LINEAR, where this browser can filter a float texture. The
    // shader reads the file only through texelFetch (fileAt), so NEAREST
    // gives the same values; it only keeps the texture complete without
    // OES_texture_float_linear.
    const filter = this.floatLinear ? gl.LINEAR : gl.NEAREST;
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, filter);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, filter);
    // Below Scale 1 the file tiles; mirrored, so it tiles without a seam.
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.MIRRORED_REPEAT);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.MIRRORED_REPEAT);
    gl.bindTexture(gl.TEXTURE_2D, null);
    return gl.getError() === gl.NO_ERROR;
  }

  Release() {
    if (this.texture !== null) this.gl.deleteTexture(this.texture);
    this.texture = null;
  }

  Loaded() {
    return this.texture !== null;
  }
}

/** One running instance of the plugin: ThrowPlugin's state, and its ProcessOpenGL. */
class ThrowInstance {
  constructor(gl, quad, programs, formats) {
    this.gl = gl;
    this.quad = quad;
    this.programs = programs;
    this.formats = formats;

    // Declaration, from kTable: every control at its default.
    this.params = new Float32Array(PT.ABOUT_FIRST);
    for (let id = 0; id < PT.ABOUT_FIRST; id += 1) this.params[id] = InfoOf(id).defaultValue;

    this.surface = new PassBuffer(gl, { filter: 'linear' }); // albedo (linear) and height (metres)
    this.occlusion = new PassBuffer(gl, { filter: 'linear' }); // the room's light reaching each point

    // One black texel for FileTexture when no file is loaded.
    this.blank = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.blank);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA32F, 1, 1, 0, gl.RGBA, gl.FLOAT, new Float32Array([0, 0, 0, 1]));
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.bindTexture(gl.TEXTURE_2D, null);

    this.builtKey = null;
    this.built = false;
    this.surfaceBuilds = 0;

    this.file = new HeightFile(gl, formats.floatLinear);
    this.filePath = null; // as the host set it: here, the visitor's decoded pick
    this.fileLoadedPath = null; // what `file` holds
    this.fileFailed = false;
    this.fileHolds = -1; // the reading `file`'s texture was built for
    this.fileGeneration = 0;
    this.fileReason = '';

    this.room = null;
    this.hooks = 0;
  }

  SetFloatParameter(index, value) {
    if (index >= PT.ABOUT_FIRST) return false; // the About block is not on the page
    if (index === PT.FILE) return true; // a path, not a number
    this.params[index] = value;
    return true;
  }

  /** SetTextParameter( PT_FILE ): the path, which here is a decoded file or null. */
  SetFile(picked) {
    this.filePath = picked;
  }

  buildSurface(width, height, kind) {
    const gl = this.gl;
    const p = this.params;
    const room = this.room;
    this.surface.ensure(width, height, this.formats.surface);
    this.occlusion.ensure(width, height, gl.R16F);

    const scale = ScaleFromParam(p[PT.SCALE]);
    {
      this.surface.bind();
      const shader = this.programs.surface.use();
      bindTexture(gl, 0, this.file.Loaded() ? this.file.texture : this.blank);
      shader.setSampler('FileTexture', 0);
      shader.setInt('FileLoaded', this.file.Loaded() ? 1 : 0);
      setInt2(shader, 'Size', width, height);
      shader.set('Frame', room.W, room.H);
      shader.setInt('SurfaceIndex', kind);
      shader.set('Scale', scale);
      shader.set('DepthScale', DepthFromParam(p[PT.DEPTH]));
      shader.set('Relief', room.relief);
      shader.setInt('Seed', clampTo(lround(p[PT.SEED]), 0, 99));
      shader.set('PaperWeights', room.paperWeights[0], room.paperWeights[1], room.paperWeights[2], room.paperWeights[3]);
      shader.setInt('BondIndex', OptionIndex(p[PT.BOND], Bond.Count));
      shader.set('Mortar', MortarFromParam(p[PT.MORTAR]));
      shader.setInt('ProfileIndex', OptionIndex(p[PT.PROFILE], Profile.Count));
      shader.set('Pitch', PitchFromParam(p[PT.PITCH]));
      shader.setInt('PatternIndex', OptionIndex(p[PT.PATTERN], Pattern.Count));
      shader.set('PleatSpacing', room.pleatSpacing);
      shader.set('Sag', clampTo(p[PT.SAG], 0.0, 1.0));
      this.quad.draw();
    }
    {
      this.occlusion.bind();
      const shader = this.programs.occlusion.use();
      bindTexture(gl, 0, this.surface.texture);
      shader.setSampler('Surface', 0);
      setInt2(shader, 'Size', width, height);
      shader.set('Frame', room.W, room.H);
      shader.set('Relief', room.relief);
      this.quad.draw();
    }
    bindTexture(gl, 0, null);
    this.surfaceBuilds += 1;
  }

  /**
   * One frame. `picture` is the input ({texture, width, height}); the host's
   * viewport is (0, 0, viewWidth, viewHeight) of the bound default framebuffer.
   */
  ProcessOpenGL(picture, viewWidth, viewHeight) {
    if (!picture || !picture.texture || picture.width === 0 || picture.height === 0) return false;
    const gl = this.gl;
    gl.disable(gl.BLEND);

    const width = picture.width;
    const height = picture.height;
    const kind = OptionIndex(this.params[PT.SURFACE], Surface.Count);

    // The file. Read when it is chosen, not when its path arrives.
    if (kind === Surface.File) {
      const path = this.filePath;
      const failed = this.fileFailed && path === this.fileLoadedPath;
      if (!failed && path !== this.fileLoadedPath) {
        const out = { reason: '' };
        let ok = false;
        if (path !== null) {
          ok = this.file.Load(path, out);
          // eslint-disable-next-line no-console
          if (!ok) console.warn(`Surface File will not load: ${path.name} (${out.reason}); showing Flat`);
        }
        if (!ok) this.file.Release();
        this.fileHolds = -1;
        this.fileGeneration += 1;
        this.fileLoadedPath = path;
        this.fileFailed = !ok && path !== null;
        this.fileReason = out.reason;
      }
      const holds = OptionIndex(this.params[PT.FILE_HOLDS], FileHolds.Count);
      if (!this.fileFailed && this.fileLoadedPath !== null && holds !== this.fileHolds) {
        // eslint-disable-next-line no-console
        if (!this.file.Upload(holds === FileHolds.Photo)) console.warn('the Surface File would not upload');
        this.fileHolds = holds;
        this.fileGeneration += 1;
      }
    }

    this.room = MakeRoom(this.params, width, height);
    const room = this.room;
    // No file, nothing behind the front plane.
    if (kind === Surface.File && !this.file.Loaded()) room.relief = 0.0;

    const key = {
      params: this.params.slice(0, PT.FILE_HOLDS + 1),
      width,
      height,
      fileGeneration: this.fileGeneration,
    };
    const same = (a, b) => a !== null && a.width === b.width && a.height === b.height
      && a.fileGeneration === b.fileGeneration && a.params.every((v, i) => v === b.params[i]);
    const stale = (this.hooks & kHookStaleSurface) !== 0 && this.built && this.surface.texture !== null;
    if (!stale && (!this.built || !same(this.builtKey, key) || this.surface.texture === null)) {
      this.buildSurface(width, height, kind);
      this.builtKey = key;
      this.built = true;
    }

    // The projection, onto the host's buffer.
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, viewWidth, viewHeight);
    const shader = this.programs.project.use();
    bindTexture(gl, 0, picture.texture);
    bindTexture(gl, 1, this.surface.texture);
    bindTexture(gl, 2, this.occlusion.texture);
    shader.setSampler('InputTexture', 0);
    shader.setSampler('Surface', 1);
    shader.setSampler('Occlusion', 2);
    // A browser texture is never padded: GetMaxGLTexCoords is 1 by 1.
    shader.set('MaxUV', 1.0, 1.0);
    setInt2(shader, 'Size', width, height);
    setInt2(shader, 'ViewOrigin', 0, 0);
    setInt2(shader, 'ViewSize', Math.max(viewWidth, 1), Math.max(viewHeight, 1));
    shader.set('Frame', room.W, room.H);
    shader.set('Relief', room.relief);
    shader.set('Lens', room.lens.x, room.lens.y, room.lens.z);
    shader.set('Eye', room.eye.x, room.eye.y, room.eye.z);
    shader.setInt('AimIndex', room.aim);
    shader.set('Axis', room.axis.x, room.axis.y, room.axis.z);
    shader.set('Right', room.right.x, room.right.y, room.right.z);
    shader.set('Up', room.up.x, room.up.y, room.up.z);
    shader.set('Design', room.design);
    shader.set('BlackLevel', room.blackLevel);
    shader.set('RoomRatio', room.roomRatio);
    shader.set('Gain', room.gain);
    shader.setInt('GlazeIndex', room.glaze);
    shader.set('GlazeFloor', room.glazeFloor);
    shader.set('F0', room.f0[0], room.f0[1], room.f0[2]);
    shader.set('Alpha', room.alpha);
    shader.setInt('ViewIndex', OptionIndex(this.params[PT.VIEW], View.Count));
    shader.set('MixAmount', clampTo(this.params[PT.MIX], 0.0, 1.0));
    shader.setInt('Hooks', this.hooks);
    this.quad.draw();
    for (let unit = 2; unit >= 0; unit -= 1) bindTexture(gl, unit, null);
    return true;
  }

  dispose() {
    this.surface.dispose();
    this.occlusion.dispose();
    this.file.Release();
    this.gl.deleteTexture(this.blank);
    this.built = false;
  }
}

// ==== END FRAME ============================================================

/**
 * A picked image file, decoded in the page: {width, height, rgba} with rgba
 * the file's own 8-bit RGBA, top row first and not premultiplied (as
 * stb_image hands it over), or rgba null and a reason. The colour-managed,
 * premultiplied canvas path would change the values, so the bitmap goes
 * through a texture and back.
 */
async function decodeImage(gl, blob) {
  let bitmap;
  try {
    bitmap = await createImageBitmap(blob, { premultiplyAlpha: 'none', colorSpaceConversion: 'none' });
  } catch (error) {
    return { width: 0, height: 0, rgba: null, reason: `this browser could not decode it (${error.message})` };
  }
  const width = bitmap.width;
  const height = bitmap.height;
  const most = gl.getParameter(gl.MAX_TEXTURE_SIZE);
  if (width > most || height > most) {
    bitmap.close();
    return { width, height, rgba: null, reason: `${width}x${height} is larger than this GPU's ${most} texels` };
  }
  const texture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, texture);
  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.NONE);
  gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, width, height, 0, gl.RGBA, gl.UNSIGNED_BYTE, bitmap);
  gl.pixelStorei(gl.UNPACK_COLORSPACE_CONVERSION_WEBGL, gl.BROWSER_DEFAULT_WEBGL);
  bitmap.close();
  const fbo = gl.createFramebuffer();
  gl.bindFramebuffer(gl.FRAMEBUFFER, fbo);
  gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, texture, 0);
  const rgba = new Uint8Array(width * height * 4);
  gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
  // Row 0 of the texture is the image's top row (no flip on upload).
  gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, rgba);
  gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  gl.deleteFramebuffer(fbo);
  gl.bindTexture(gl.TEXTURE_2D, null);
  gl.deleteTexture(texture);
  return { width, height, rgba };
}

const page = { gl: null, instance: null, formats: null, programs: null, quad: null };

function createRenderer(gl, quad) {
  // The kit has already insisted on EXT_color_buffer_float (needFloat). The
  // surface is RGBA32F and sampled LINEAR, which in WebGL2 also needs
  // OES_texture_float_linear: without it the texture is incomplete and reads
  // 0. A browser that lacks it gets RGBA16F, which is filterable in core
  // WebGL2, and the page says so.
  const floatLinear = gl.getExtension('OES_texture_float_linear') !== null;
  const formats = { surface: floatLinear ? gl.RGBA32F : gl.RGBA16F, floatLinear };

  // InitGL: the vertex stage is kVersion + kQuadVertex; each fragment stage is
  // Assemble( pass ) = kVersion + kCommon + the pass's fragment.
  const vertex = S.VERSION + S.QUAD_VERTEX;
  const assemble = (fragment) => S.VERSION + S.COMMON + fragment;
  const programs = {
    surface: new Program(gl, vertex, assemble(S.SURFACE_FRAGMENT), 'surface'),
    occlusion: new Program(gl, vertex, assemble(S.OCCLUSION_FRAGMENT), 'occlusion'),
    project: new Program(gl, vertex, assemble(S.PROJECT_FRAGMENT), 'project'),
  };

  const instance = new ThrowInstance(gl, quad, programs, formats);
  Object.assign(page, { gl, instance, formats, programs, quad });

  // For demo/tools/crosscheck/: a FRESH instance (twtest's is one too), the
  // settings by the plugin's display name (an option by its element's name,
  // Surface File by a URL here where twtest takes a path), one frame of the
  // given RGBA8 picture (top row first, as `twtest --pipe` reads it), read
  // back the same way. The draw and the read are one task, so the page's own
  // frames cannot interleave; the next one puts the canvas back.
  window.throwDemo = {
    formats: { surface: floatLinear ? 'RGBA32F' : 'RGBA16F' },
    table: kTable.map((row) => ({
      name: row.name,
      kind: ['standard', 'option', 'integer', 'file'][row.kind],
      value: row.defaultValue,
      options: row.options,
    })),
    async renderCase({ settings = {}, frame, width, height }) {
      const run = new ThrowInstance(gl, quad, programs, formats);
      try {
        for (const [name, value] of Object.entries(settings)) {
          const info = kTable.find((row) => row.name === name);
          if (!info) throw new Error(`no parameter called '${name}'`);
          if (info.kind === Kind.File) {
            const blob = await (await fetch(value)).blob();
            run.SetFile({ name: value, ...(await decodeImage(gl, blob)) });
            continue;
          }
          const index = info.options ? info.options.indexOf(value) : -1;
          const number = index >= 0 ? index : Number(value);
          if (!Number.isFinite(number)) throw new Error(`'${value}' is not a number or an option of ${name}`);
          run.SetFloatParameter(info.id, number);
        }
        const texture = gl.createTexture();
        gl.bindTexture(gl.TEXTURE_2D, texture);
        gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA8, width, height);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
        gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
        const row = width * 4;
        const flipped = new Uint8Array(row * height);
        for (let y = 0; y < height; y += 1) flipped.set(frame.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
        gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
        gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
        gl.bindTexture(gl.TEXTURE_2D, null);

        const canvas = gl.canvas;
        canvas.width = width;
        canvas.height = height;
        gl.bindFramebuffer(gl.FRAMEBUFFER, null);
        gl.viewport(0, 0, width, height);
        gl.clearColor(0, 0, 0, 0);
        gl.clear(gl.COLOR_BUFFER_BIT);
        run.ProcessOpenGL({ texture, width, height }, width, height);
        gl.pixelStorei(gl.PACK_ALIGNMENT, 1);
        gl.readPixels(0, 0, width, height, gl.RGBA, gl.UNSIGNED_BYTE, flipped);
        const out = new Uint8Array(row * height);
        for (let y = 0; y < height; y += 1) out.set(flipped.subarray(y * row, (y + 1) * row), (height - 1 - y) * row);
        gl.deleteTexture(texture);
        return { pixels: out, missing: [...Object.values(programs)].flatMap((p) => [...p.missing]) };
      } finally {
        run.dispose();
      }
    },
  };

  return {
    gl,
    instance,
    programs,

    render({ input, params, width, height }) {
      // The host restates every parameter each frame; the Surface File is a
      // path, set when the visitor picks one (see the File row below).
      for (const p of PARAMS) {
        if (p.type === 'text') continue;
        instance.SetFloatParameter(p.slot, hostValue(p, params.get(p.id)));
      }
      instance.ProcessOpenGL(input, width, height);
      notes.update();
    },
  };
}

//===========================================================================
// The parameters: the constructor's, in kTable's order and groups, with the
// plugin's names, element lists and defaults. Hints from the README and
// Controls.h.
//===========================================================================
const fix = (n, d = 2) => n.toFixed(d);
const signed = (n, d = 1) => `${n >= 0 ? '+' : '−'}${Math.abs(n).toFixed(d)}`;
const sig = (n) => (n >= 100 ? n.toFixed(0) : n >= 10 ? n.toFixed(1) : n >= 1 ? n.toFixed(2) : n.toFixed(3));
const V = (v) => f32(v);
const metres = (n) => `${signed(n, 2)} m`;

/** id, ParamId, display, hint. */
const SPEC = [
  ['surface', PT.SURFACE, null, 'What the wall is made of. Flat is the reference screen, where the clip comes back as it went in; File is a height map or photo of your own (Surface File, below).'],
  ['wallHeight', PT.WALL_HEIGHT, (v) => `${sig(WallHeightFromParam(V(v)))} m`, 'How much wall the frame shows, 0.25 to 20 m high, geometrically. Every surface is at its real size, so a taller wall shows smaller bricks.'],
  ['scale', PT.SCALE, (v) => `×${fix(ScaleFromParam(V(v)))}`, 'A multiplier on every surface’s size in the plane, 0.25 to 4, geometrically.'],
  ['depth', PT.DEPTH, (v) => `×${fix(DepthFromParam(V(v)))}`, 'A multiplier on the relief, 0 to 4, quadratically: the middle is the real thing, 0 is flat. The knob for drama: at its real depth a brick joint slides about two pixels at 1080p.'],
  ['seed', PT.SEED, null, 'Which paper, which bricks, which drapes: every procedural surface’s random part. FF_TYPE_INTEGER 0–99 in the plugin; a dropdown here. Changing it rebuilds the surface.'],

  ['crumple', PT.CRUMPLE, (v) => fix(V(v)), 'Paper: flat facets in up to four generations, each finer than the last, arriving as this rises. 0 is a flat sheet.'],
  ['bond', PT.BOND, null, 'Brick: how the bricks are laid. Stretcher (half a brick out), English (a course of headers between), Flemish (header and stretcher in every course), Stack.'],
  ['mortar', PT.MORTAR, (v) => `${(MortarFromParam(V(v)) * 1000).toFixed(1)} mm`, 'Brick: the joint’s width, 3 to 25 mm. The joints are raked 8 mm deep.'],
  ['profile', PT.PROFILE, null, 'Corrugated: Sine (3-inch corrugated iron) or Box (trapezoidal cladding).'],
  ['pitch', PT.PITCH, (v) => `${(PitchFromParam(V(v)) * 1000).toFixed(0)} mm`, 'Corrugated: the corrugation’s period, 25 to 300 mm, geometrically; its depth in proportion.'],
  ['pattern', PT.PATTERN, null, 'Plate: aluminium tread plate’s lugs. Diamond, Five-Bar or Crosshatch, 30 mm cells, 1.5 mm proud.'],
  ['pleats', PT.PLEATS, null, 'Drapes: folds across the frame, each half as deep as it is wide. FF_TYPE_INTEGER 2–60 in the plugin; a dropdown here.'],
  ['sag', PT.SAG, (v) => fix(V(v)), 'Drapes: how slack the cloth hangs, so the folds wander and deepen towards the floor.'],
  ['roughness', PT.ROUGHNESS, (v) => fix(RoughnessFromParam(V(v))), 'How sharp the lens’s highlight is on metal and glaze (Corrugated, Plate, Tiles): GGX’s perceptual roughness, 0.05 to 0.8.'],

  ['surfaceFile', PT.FILE, null, 'File: your own height map or photo. An FFGL file parameter in the plugin; here an image you pick, decoded in this page and never uploaded.'],
  ['fileHolds', PT.FILE_HOLDS, null, 'File: Height reads grey as height (white at the front, black 100 mm back at Depth 1, the wall white); Photo takes the picture’s colour as the wall’s and its brightness, stretched to its own range, as the height. A photo’s darks are not really its recesses: a look, not a measurement.'],

  ['projectorX', PT.PROJECTOR_X, (v) => metres(OffsetFromParam(V(v))), 'Where the lens is across the wall from the frame’s centre, −10 to +10 m, quadratically about the middle.'],
  ['projectorY', PT.PROJECTOR_Y, (v) => metres(OffsetFromParam(V(v))), 'Where the lens is up the wall from the frame’s centre, −10 to +10 m.'],
  ['throw', PT.THROW, (v) => `${sig(DistanceFromParam(V(v)))} m`, 'How far the lens is from the wall, 1 to 50 m, geometrically.'],
  ['aim', PT.AIM, null, 'Lens Shift: square to the wall, the picture filling the frame, as a shift lens keeps it. Pointed: aimed at the frame’s centre, so the picture is a keystone, bright on the near side and dim on the far.'],
  ['lumens', PT.LUMENS, (v) => `${Math.round(LumensFromParam(V(v)))} lm`, 'The projector’s light, 500 to 50,000 lumens, geometrically. Against the room’s light it sets how grey the blacks are.'],
  ['blackLevel', PT.BLACK_LEVEL, (v) => `${(BlackLevelFromParam(V(v)) * 100).toFixed(3)}%`, 'What the projector sends for black, as a share of its white, 0 to 2%, quadratically. 0.05% is 2000:1. A projector cannot project black.'],

  ['ambient', PT.AMBIENT, (v) => `${sig(AmbientFromParam(V(v)))} lx`, 'The room’s own light on the wall, 0 to 1000 lux, quadratically. It is the floor under every dark part of the clip.'],
  ['exposure', PT.EXPOSURE, (v) => `${signed(Math.log2(ExposureFromParam(V(v))), 1)} st`, 'A gain on the picture, −4 to +4 stops.'],

  ['viewerX', PT.VIEWER_X, (v) => metres(OffsetFromParam(V(v))), 'Where you stand across the wall from the frame’s centre, −10 to +10 m. Off to one side, the picture slides over the relief. Stand at the lens and it all goes straight again, and the shadows vanish.'],
  ['viewerY', PT.VIEWER_Y, (v) => metres(OffsetFromParam(V(v))), 'Your eye’s height from the frame’s centre, −10 to +10 m.'],
  ['viewerDistance', PT.VIEWER_DISTANCE, (v) => `${sig(DistanceFromParam(V(v)))} m`, 'How far you stand from the wall, 1 to 50 m, geometrically.'],

  ['view', PT.VIEW, null, 'What the plugin is working with: the Picture; the relief’s Height; its Normals; the projector’s Light alone; its Shadow (white lit, black shadowed, grey turned away); the wall’s Albedo.'],
  ['mix', PT.MIX, (v) => fix(V(v)), 'The whole effect against the clip. At 0 the clip comes back as it went in.'],
];

const PARAMS = SPEC.map(([id, slot, display, hint]) => {
  const info = InfoOf(slot);
  if (info.id !== slot) throw new Error(`${id}: not a declared parameter`);
  const p = { id, slot, name: info.name, group: info.group, hint };
  if (info.kind === Kind.Option) {
    Object.assign(p, { type: 'option', elements: info.options.slice(), default: info.defaultValue, offset: 0 });
  } else if (info.kind === Kind.Integer) {
    // Every value in the plugin's range; the slot is the value less the low end.
    const elements = [];
    for (let n = info.low; n <= info.high; n += 1) elements.push(String(n));
    Object.assign(p, { type: 'option', elements, default: info.defaultValue - info.low, offset: info.low });
  } else if (info.kind === Kind.File) {
    Object.assign(p, { type: 'text', default: '' });
  } else {
    Object.assign(p, { type: 'standard', display, default: info.defaultValue, offset: 0 });
  }
  return p;
});
if (PARAMS.length !== PT.ABOUT_FIRST || PARAMS.some((p, i) => p.slot !== i)) {
  throw new Error('the panel must carry every control, in the plugin’s order');
}

/** The value the host would send for what the panel holds. */
const hostValue = (p, value) => value + (p.offset ?? 0);

// Presets: whole states, on top of the constructor's defaults. Each is a set of
// the plugin's own controls; the README's figures where there is one.
const PRESETS = {
  'Brick (the defaults)': {},
  'Brick at Depth 4, from 4 m right': { depth: ParamFromDepth(4.0), viewerX: ParamFromOffset(4.0) },
  'Drapes': { surface: Surface.Drapes },
  'Drapes, from 4 m left (README)': {
    surface: Surface.Drapes, viewerX: ParamFromOffset(-4.0), viewerY: ParamFromOffset(1.0), viewerDistance: ParamFromDistance(6.0),
  },
  'Drapes, from the lens (README)': {
    surface: Surface.Drapes, viewerX: ParamFromOffset(0.0), viewerY: ParamFromOffset(1.0), viewerDistance: ParamFromDistance(6.0),
  },
  'Corrugated: the lens in the zinc': { surface: Surface.Corrugated },
  'Tread plate, Five-Bar': { surface: Surface.Plate, pattern: Pattern.FiveBar },
  'Crumpled paper, fully': { surface: Surface.Paper, crumple: 1.0, depth: ParamFromDepth(2.0) },
  'Weatherboard': { surface: Surface.Weatherboard },
  'Glazed tiles': { surface: Surface.Tiles },
  'Pointed projector: keystone': { surface: Surface.Flat, aim: Aim.Pointed, projectorX: ParamFromOffset(2.5) },
  'A bright room: no black': { ambient: ParamFromAmbient(300.0) },
  'The projector’s light alone': { view: View.Light, depth: ParamFromDepth(2.0) },
};

// ---- page furniture: the Surface File row, and what the page has to say ----

const notes = {
  node: null,
  last: '',
  extra: '',
  update() {
    if (!this.node || !page.instance) return;
    const lines = [];
    if (!page.formats.floatLinear) {
      lines.push('This browser cannot filter 32-bit float textures (no OES_texture_float_linear), so the surface is held in RGBA16F here, not the plugin’s RGBA32F: its heights are rounded to half float.');
    }
    const instance = page.instance;
    if (OptionIndex(instance.params[PT.SURFACE], Surface.Count) === Surface.File) {
      if (instance.filePath === null) {
        lines.push('Surface is File and no Surface File is chosen, so this is Flat, as it is in the plugin. Choose an image in the File group.');
      } else if (instance.fileFailed) {
        lines.push(`The Surface File will not load (${instance.fileReason}), so this is Flat, as it is in the plugin.`);
      }
    }
    if (this.extra) lines.push(this.extra);
    const text = lines.join(' ');
    if (text === this.last) return;
    this.last = text;
    this.node.hidden = text === '';
    this.node.textContent = text;
  },
};

function fileRow(mounted) {
  const input = document.getElementById('p-surfaceFile');
  if (!input) return; // embed mode has no panel
  input.readOnly = true;
  input.placeholder = 'None (Flat)';
  input.title = 'The image you picked. It is decoded in this page and never leaves it.';

  const cell = document.createElement('div');
  cell.style.display = 'flex';
  cell.style.gap = '6px';
  cell.style.minWidth = '0';
  input.replaceWith(cell);
  input.style.minWidth = '0';
  cell.append(input);

  const choose = document.createElement('label');
  choose.className = 'btn';
  choose.textContent = 'Choose…';
  choose.style.padding = '4px 8px';
  choose.style.fontSize = '12px';
  choose.style.whiteSpace = 'nowrap';
  choose.title = 'A height map (grey, white at the front) or a photo. Decoded in this page; it never leaves your machine.';
  const picker = document.createElement('input');
  picker.type = 'file';
  picker.accept = 'image/*';
  picker.hidden = true;
  choose.append(picker);

  const clear = document.createElement('button');
  clear.type = 'button';
  clear.className = 'btn';
  clear.textContent = 'Clear';
  clear.style.padding = '4px 8px';
  clear.style.fontSize = '12px';
  clear.title = 'No Surface File: the plugin’s default, which shows Flat';
  cell.append(choose, clear);

  picker.addEventListener('change', async () => {
    const chosen = picker.files?.[0];
    picker.value = '';
    if (!chosen) return;
    const decoded = await decodeImage(page.gl, chosen);
    page.instance.SetFile({ name: chosen.name, ...decoded });
    input.placeholder = decoded.rgba ? `${chosen.name} (${decoded.width} × ${decoded.height})` : `${chosen.name}: will not load`;
    // A convenience the plugin does not have: in Resolume, Surface File and
    // Surface are separate controls.
    mounted.params.set('surface', Surface.File);
    mounted.redraw();
  });
  clear.addEventListener('click', () => {
    page.instance.SetFile(null);
    input.placeholder = 'None (Flat)';
    mounted.redraw();
  });
}

const mounted = mountDemo({
  name: 'Throw',
  pluginId: 'TW01',
  tagline:
    'A projector throwing the clip onto a real surface: crumpled paper, a brick wall, corrugated sheet, tread plate, pleated drapes, weatherboard, glazed tiles, or a height map of your own. The light leaves a lens at a real place in the room and you stand somewhere else, so the picture is shaded by angle and distance, coloured by the wall, shadowed by the relief, slides over it as you move, flares off metal and never reaches black.',
  repo: 'https://github.com/stoatworks-labs/throw',
  page: 'https://stoatworks-labs.com/software/throw/',

  blurb:
    'It is Throw’s own GLSL, ported from the repository to WebGL2 unedited, fed by a JavaScript port of the plugin’s CPU half (every control’s mapping and the room worked out in metres, in double), running on generated clips or your own in this page — no install.',

  needFloat: true,

  // The geometry card first: its straight lines are what kink across mortar
  // joints and ripple over folds when you stand off to one side. Then the
  // general case, the bars (a projector adds light, so red brick eats the
  // blue), lights on black (the room's grey where black should be), the ramps
  // and fine detail.
  sources: ['scene', 'grid', 'bars', 'spot', 'ramp', 'detail'],

  params: PARAMS,
  presets: PRESETS,

  differences: [
    'The CPU half is a PORT, not the plugin’s code: Controls.cpp (every mapping, name, group and default), Room.cpp (the room in metres, worked out in double: the lens, the eye, the pointed projector’s basis, each surface’s relief, the room’s light against the projector’s, the metals’ F0 and the paper’s generations), HeightFile.cpp’s reading of a Surface File and the CPU half of Throw.cpp that turns them into each pass’s uniforms and rebuilds the surface only when its key changes. Only a reader checks it line by line; what is measured is the picture. demo/tools/crosscheck/ renders 22 cases from the plugin’s harness card — every surface, bond, profile and pattern, every view, the pointed projector, Mix, and an 8-bit height map and photo as the Surface File — through the plugin itself (twtest) and through this page. On an M4 Max (OpenGL 4.1 against Chrome’s ANGLE on Metal), at 640×360 and 1280×720, no pixel is more than one level of 255 apart and 99.2% or more are identical; the page’s 29 controls are twtest’s, name, kind and default. One slider step off in Throw, Viewer X, Depth, Sag or Lumens, or the paper’s Seed one off, fails that comparison. Other browsers and GPUs are not measured.',
    'The GPU half is not a port: the surface, occlusion and project passes are the plugin’s own GLSL, joined as the plugin joins them, on its formats — the surface in RGBA32F, sampled with linear filtering, and its occlusion in R16F. They need EXT_color_buffer_float, without which the page stops and says so. A browser without OES_texture_float_linear cannot filter a 32-bit float texture; there the surface is held in RGBA16F instead and the page says so under the picture. demo/tools/check_shaders.py fails the repository’s verify script if a character of the shaders, the way they are joined, or a uniform the page does not set drifts.',
    'Surface File is an FFGL file parameter, a path, in the plugin; here it is an image you choose, decoded in this page and never uploaded. The plugin reads PNG, JPEG, TGA and BMP with stb_image at 16 bits a channel; the page takes whatever this browser decodes, at 8 bits a channel, with the browser’s own JPEG decoder and a photo’s orientation tag honoured. An 8-bit PNG gives the plugin’s values exactly; a 16-bit height map loses its low bits here, and its slopes can show the 8-bit steps. Choosing a file also sets Surface to File, as a convenience; in Resolume they are two controls. Defaults and the presets leave the chosen file in place, where the plugin’s default is no file.',
    'Seed and Pleats are FF_TYPE_INTEGER, 0–99 and 2–60, in the plugin. The kit has no integer control, so they are dropdowns of every value in the plugin’s range. The About block is not on the panel.',
    'Nothing in Throw moves with time: the clock and the transport only move the generated clip. The clip is the kit’s or your own file, 8-bit as Resolume hands an effect its input, never padded (MaxUV is 1), and read as sRGB as the plugin reads it.',
    'The Presets menu is the page’s: the plugin has none, since Resolume saves parameter presets itself. Each is a set of the plugin’s own controls on top of its defaults; the drapes from the left and from the lens are the README’s parallax figure.',
    'The plugin’s negative-control hooks (the Hooks uniform its harness flips to prove its checks can fail) are 0 here, as in a shipped instance.',
    'The crosscheck says the page draws what the plugin draws on this machine; it says nothing about whether either is right. The plugin’s harness, twtest, shows a flat wall giving the clip back to 3e-7 from any projector and viewer, every pixel of a tilted plane carrying the inverse-square-and-cosine light its closed form says, a recess showing the picture where the parallax formula puts it to 0.0007 px, a step’s shadow the closed-form length to within a pixel, and the lens’s highlight on zinc at the mirror point; 18 wrong models and 23 one-character mutants are caught. The plugin has never been loaded into Resolume. That harness, not this page, is the reason to believe the picture.',
  ],

  createRenderer,
});

if (mounted) {
  const stage = document.querySelector('.stage');
  if (stage) {
    notes.node = document.createElement('p');
    notes.node.className = 'stage__status';
    notes.node.hidden = true;
    stage.append(notes.node);
  }
  fileRow(mounted);
  notes.update();
}
