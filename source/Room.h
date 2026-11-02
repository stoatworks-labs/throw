#pragma once

#include "Controls.h"

/**
    The room, in metres, worked out in double on the CPU and handed to the GPU
    as floats.

    The wall's front plane is z = 0, +z towards the room, +y up. The frame is
    the patch of wall x in [-W/2, W/2], y in [-H/2, H/2]. Every surface is a
    height field in [-relief, 0]: its front faces on the plane, its recesses
    behind it.
*/
namespace throwfx
{
struct Vec3
{
	double x = 0.0, y = 0.0, z = 0.0;
};

/// What each surface is made of, and how big it is at Scale 1, Depth 1.
/// Real sizes, chosen and sourced in AGENTS.md.
namespace facts
{
// Paper: four generations of facets, each this much finer than the last.
constexpr double kPaperFacet  = 0.12; ///< the first generation's junction spacing
constexpr double kPaperRelief = 0.015;///< the first generation's corner heights, +-
constexpr double kPaperShrink = 0.45;
constexpr int kPaperLayers    = 4;

// Brick: UK metric (BS EN 771-1 work size), raked joints.
constexpr double kBrickLength  = 0.215;
constexpr double kBrickHeight  = 0.065;
constexpr double kHeaderLength = 0.1025;
constexpr double kJointRecess  = 0.008;

// Corrugated: depth in proportion to pitch. 3-inch corrugated is 76 mm by
// 18 mm; box profile cladding about 200 mm by 35 mm.
constexpr double kSineDepthPerPitch = 18.0 / 76.0;
constexpr double kBoxDepthPerPitch  = 35.0 / 200.0;

// Tread plate.
constexpr double kPlateCell = 0.030;
constexpr double kLugHeight = 0.0015;

// Drapes: folds half as deep as they are apart.
constexpr double kFoldDepthPerSpacing = 0.5;

// Weatherboard: feather-edge boards.
constexpr double kBoard    = 0.150;
constexpr double kBoardLip = 0.015;

// Tiles: metro tile, bevelled, grout recessed.
constexpr double kTileLength = 0.200;
constexpr double kTileHeight = 0.100;
constexpr double kGrout      = 0.003;
constexpr double kBevel      = 0.006;///< the bevel's width
constexpr double kBevelDrop  = 0.003;
constexpr double kTileRecess = 0.0045;

// File: white at the front, black this far behind it.
constexpr double kFileRelief = 0.10;

/// Normal-incidence reflectance, linear sRGB, from tools/f0.py: zinc
/// (Werner, Glantschnig and Ambrosch-Draxl 2009) for galvanised sheet,
/// aluminium (Rakic 1995) for tread plate. Glaze is a dielectric of index
/// about 1.5.
constexpr double kZincF0[ 3 ]      = { 0.875, 0.869, 0.855 };
constexpr double kAluminiumF0[ 3 ] = { 0.907, 0.916, 0.922 };
constexpr double kGlazeF0          = 0.04;
} // namespace facts

/// Which parts of a surface take a specular highlight.
enum class Glaze
{
	None = 0,///< diffuse only
	All,     ///< metal: every point
	Tiles    ///< the tile faces and bevels, not the grout
};

struct Room
{
	double W = 0.0, H = 0.0;///< the frame on the wall
	Vec3 lens;              ///< the projector's lens
	Vec3 eye;               ///< the viewer
	Aim aim = Aim::LensShift;
	Vec3 axis, right, up;   ///< the projector's basis (axis forward)
	double design      = 0.0;///< distance to the screen it lights evenly
	double relief      = 0.0;///< the surface's deepest recess, R
	double roomRatio   = 0.0;///< the room's illuminance over the projector's, A
	double blackLevel  = 0.0;
	double gain        = 1.0;
	double alpha       = 0.0;///< GGX alpha, Roughness squared
	double f0[ 3 ]     = { 0.0, 0.0, 0.0 };
	Glaze glaze        = Glaze::None;
	double glazeFloor  = 0.0;///< Tiles: deeper than this is grout
	double pleatSpacing = 0.0;
	double paperWeights[ facts::kPaperLayers ] = {};
};

/// The room for these host parameters at this raster.
Room MakeRoom( const float* params, int width, int height );

/// The paper's generations: each arrives as Crumple rises past l / 4, over a
/// quarter of its travel.
double PaperWeight( double crumple, int layer );

/// R, the deepest a surface goes behind its front plane, for these parameters.
double ReliefOf( Surface surface, const float* params, double frameWidth );

} // namespace throwfx
