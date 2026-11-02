#pragma once

#include "StoatworksAboutLinks.h"

/**
    The host's parameters, and what they mean.

    Every ranged parameter the host sees is 0..1, because `SetParamInfo`
    clamps an `FF_TYPE_STANDARD` default into 0..1 before `SetParamRange`
    could widen it. The conversions live in Controls.cpp, one function per
    control, each with its inverse where a default or the harness needs it.
    Option parameters hold the element index. Pleats and Seed are real
    integers (FF_TYPE_INTEGER is exempt from the clamp). Surface File is a
    path.

    Units: metres for every length in the room (the wall, the projector, the
    viewer), millimetres where a control sizes a feature of the surface
    (Mortar, Pitch), lumens and lux for light.
*/
namespace throwfx
{
/// Every control, in the order Resolume shows them. The About block is
/// LAST, so the day a user guide adds a button no control moves.
enum ParamId : unsigned int
{
	// -- Surface ---------------------------------------------------------------
	PT_SURFACE = 0,
	PT_WALL_HEIGHT,
	PT_SCALE,
	PT_DEPTH,
	PT_SEED,

	// -- Per surface -----------------------------------------------------------
	PT_CRUMPLE,  ///< Paper
	PT_BOND,     ///< Brick
	PT_MORTAR,   ///< Brick
	PT_PROFILE,  ///< Corrugated
	PT_PITCH,    ///< Corrugated
	PT_PATTERN,  ///< Plate
	PT_PLEATS,   ///< Drapes
	PT_SAG,      ///< Drapes
	PT_ROUGHNESS,///< metal and glaze

	// -- File ------------------------------------------------------------------
	PT_FILE,
	PT_FILE_HOLDS,

	// -- Projector -------------------------------------------------------------
	PT_PROJECTOR_X,
	PT_PROJECTOR_Y,
	PT_THROW,
	PT_AIM,
	PT_LUMENS,
	PT_BLACK_LEVEL,

	// -- Room ------------------------------------------------------------------
	PT_AMBIENT,
	PT_EXPOSURE,

	// -- Viewer ----------------------------------------------------------------
	PT_VIEWER_X,
	PT_VIEWER_Y,
	PT_VIEWER_DISTANCE,

	// -- Output ----------------------------------------------------------------
	PT_VIEW,
	PT_MIX,

	// -- The Stoatworks About block: a text line, then one button per link.
	PT_ABOUT_FIRST,
	PT_COUNT = PT_ABOUT_FIRST + 1 + stoatworks::about::kButtonCount
};

enum class Kind
{
	Standard,///< 0..1, converted in Controls.cpp
	Option,  ///< an element index
	Integer, ///< a real integer with a real range
	File     ///< a path
};

/// One control as the host is told about it.
struct ControlInfo
{
	unsigned int id;
	const char* name; ///< at most 16 characters: FFGL's name field is not terminated
	const char* group;
	Kind kind;
	float defaultValue;///< in the host's units: 0..1, the index, or the integer
	const char* const* options;
	int optionCount;
	float low, high;///< an Integer's range
};

/// The table, indexed by ParamId, for every id below PT_ABOUT_FIRST.
const ControlInfo& InfoOf( unsigned int id );

//---------------------------------------------------------------------------
// Options. The shaders switch on these indices, so their order is fixed.
//---------------------------------------------------------------------------
enum class Surface
{
	Flat = 0,  ///< a gain-1.0 matte screen: the reference
	Paper,     ///< crumpled paper: flat facets, straight creases
	Brick,     ///< UK metric brick, raked joints
	Corrugated,///< galvanised sheet
	Plate,     ///< aluminium tread plate
	Drapes,    ///< pleated cloth
	Weatherboard,///< feather-edge boards
	Tiles,     ///< glazed metro tile
	File,      ///< the user's own height map or photo
	Count
};

enum class Bond
{
	Stretcher = 0,///< half a brick out, course to course
	English,      ///< a course of stretchers, a course of headers
	Flemish,      ///< header, stretcher, header in every course
	Stack,        ///< every joint in line
	Count
};

enum class Profile
{
	Sine = 0,///< corrugated iron
	Box,     ///< trapezoidal cladding
	Count
};

enum class Pattern
{
	Diamond = 0,///< lozenge lugs, alternating ±45 degrees
	FiveBar,    ///< five bars a cell, alternating across and along
	Crosshatch, ///< ridges on both diagonals
	Count
};

enum class FileHolds
{
	Height = 0,///< grey is height, white at the front; the surface is white
	Photo,     ///< colour is the albedo, luminance the height
	Count
};

enum class Aim
{
	LensShift = 0,///< axis square to the wall, the picture fills the frame
	Pointed,      ///< axis at the frame's centre: keystone
	Count
};

enum class View
{
	Picture = 0,
	Height, ///< the surface's height map: 1 at the front, 0 at -Relief
	Normals,///< the surface's normals, 0.5 + 0.5 n
	Light,  ///< the projector's irradiance where the viewer looks, relative to its design screen
	Shadow, ///< where the projector's light reaches, from where the viewer stands
	Albedo, ///< the surface's diffuse albedo
	Count
};

/// An option's index from the value the host stored (it reads back 0..1
/// whatever the count -- the fleet's trap -- so the table's own count is
/// what bounds it).
int OptionIndex( float value, int count );

//---------------------------------------------------------------------------
// The mappings. Each says its range and its shape.
//---------------------------------------------------------------------------

/// Wall Height: the height of the patch of wall in the frame, 0.25 to 20 m,
/// geometrically.
double WallHeightFromParam( float value );
float ParamFromWallHeight( double metres );

/// Scale: a multiplier on the size of the surface's pattern in the plane,
/// 0.25 to 4, geometrically.
double ScaleFromParam( float value );
float ParamFromScale( double scale );

/// Depth: a multiplier on the surface's relief, 0 to 4, quadratically, so
/// 0.5 is exactly 1 (the real thing) and 0 is exactly flat.
double DepthFromParam( float value );
float ParamFromDepth( double depth );

/// Mortar: the brick joint's width, 3 to 25 mm, linearly. Returned in metres.
double MortarFromParam( float value );
float ParamFromMortar( double metres );

/// Pitch: the corrugation's period, 25 to 300 mm, geometrically. Returned in
/// metres.
double PitchFromParam( float value );
float ParamFromPitch( double metres );

/// Roughness: GGX's perceptual roughness, 0.05 to 0.8, linearly. alpha is its
/// square.
double RoughnessFromParam( float value );
float ParamFromRoughness( double roughness );

/// Projector X / Y, Viewer X / Y: a position across the wall from the frame's
/// centre, -10 to +10 m, quadratically about the middle (fine near the axis).
double OffsetFromParam( float value );
float ParamFromOffset( double metres );

/// Throw, Viewer Distance: a distance from the wall's front plane, 1 to 50 m,
/// geometrically.
double DistanceFromParam( float value );
float ParamFromDistance( double metres );

/// Lumens: the projector's light output, 500 to 50,000 lm, geometrically.
double LumensFromParam( float value );
float ParamFromLumens( double lumens );

/// Black Level: what the projector emits for black, as a fraction of its
/// white, 0 to 2%, quadratically. 0 is an ideal projector; 0.0005 is 2000:1.
double BlackLevelFromParam( float value );
float ParamFromBlackLevel( double fraction );

/// Ambient: the room's light on the wall, 0 to 1000 lux, quadratically.
double AmbientFromParam( float value );
float ParamFromAmbient( double lux );

/// Exposure: a gain on the picture, -4 to +4 stops, linearly in stops.
double ExposureFromParam( float value );
float ParamFromExposure( double gain );

constexpr double kPi = 3.14159265358979323846;

} // namespace throwfx
