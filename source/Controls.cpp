#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace throwfx
{
namespace
{
double clamp01( float v )
{
	return std::clamp( static_cast< double >( v ), 0.0, 1.0 );
}
double geometric( float v, double low, double high )
{
	return low * std::pow( high / low, clamp01( v ) );
}
float inverseGeometric( double value, double low, double high )
{
	const double lo = std::min( low, high ), hi = std::max( low, high );
	return static_cast< float >( std::log( std::clamp( value, lo, hi ) / low ) / std::log( high / low ) );
}
/// most * v^2: exact 0 at the bottom, fine control near it.
double quadratic( float v, double most )
{
	const double x = clamp01( v );
	return most * x * x;
}
float inverseQuadratic( double value, double most )
{
	return static_cast< float >( std::sqrt( std::clamp( value / most, 0.0, 1.0 ) ) );
}
/// -most..+most, quadratically about the middle: exact 0 at 0.5.
double signedQuadratic( float v, double most )
{
	const double s = 2.0 * clamp01( v ) - 1.0;
	return most * s * std::fabs( s );
}
float inverseSignedQuadratic( double value, double most )
{
	const double s = std::clamp( value / most, -1.0, 1.0 );
	const double r = std::sqrt( std::fabs( s ) );
	return static_cast< float >( 0.5 + 0.5 * ( s < 0.0 ? -r : r ) );
}

constexpr double kWallLow = 0.25, kWallHigh = 20.0;
constexpr double kScaleLow = 0.25, kScaleHigh = 4.0;
constexpr double kDepthMost = 4.0;
constexpr double kMortarLow = 0.003, kMortarHigh = 0.025;
constexpr double kPitchLow = 0.025, kPitchHigh = 0.300;
constexpr double kRoughLow = 0.05, kRoughHigh = 0.8;
constexpr double kOffsetMost = 10.0;
constexpr double kDistanceLow = 1.0, kDistanceHigh = 50.0;
constexpr double kLumensLow = 500.0, kLumensHigh = 50000.0;
constexpr double kBlackMost = 0.02;
constexpr double kAmbientMost = 1000.0;
constexpr double kStopsMost = 4.0;

const char* const kSurfaceNames[] = { "Flat", "Paper", "Brick", "Corrugated", "Plate", "Drapes", "Weatherboard", "Tiles", "File" };
const char* const kBondNames[]    = { "Stretcher", "English", "Flemish", "Stack" };
const char* const kProfileNames[] = { "Sine", "Box" };
const char* const kPatternNames[] = { "Diamond", "Five-Bar", "Crosshatch" };
const char* const kHoldsNames[]   = { "Height", "Photo" };
const char* const kAimNames[]     = { "Lens Shift", "Pointed" };
const char* const kViewNames[]    = { "Picture", "Height", "Normals", "Light", "Shadow", "Albedo" };

ControlInfo standard( unsigned int id, const char* name, const char* group, float value )
{
	return { id, name, group, Kind::Standard, value, nullptr, 0, 0.0f, 1.0f };
}
ControlInfo option( unsigned int id, const char* name, const char* group, float value, const char* const* names, int count )
{
	return { id, name, group, Kind::Option, value, names, count, 0.0f, 1.0f };
}
ControlInfo integer( unsigned int id, const char* name, const char* group, float value, float low, float high )
{
	return { id, name, group, Kind::Integer, value, nullptr, 0, low, high };
}
ControlInfo file( unsigned int id, const char* name, const char* group )
{
	return { id, name, group, Kind::File, 0.0f, nullptr, 0, 0.0f, 1.0f };
}

//---------------------------------------------------------------------------
// The defaults: a brick wall 2.5 m high in the frame, a 10,000 lumen
// projector a metre above its centre at 6 m, a dim room (20 lux), and
// somebody standing 2 m to the right of it and a little below, 4 m back.
// Everything at its real size and its real depth.
//---------------------------------------------------------------------------
const ControlInfo kTable[] = {
	option( PT_SURFACE, "Surface", "Surface", static_cast< float >( Surface::Brick ), kSurfaceNames, static_cast< int >( Surface::Count ) ),
	standard( PT_WALL_HEIGHT, "Wall Height", "Surface", ParamFromWallHeight( 2.5 ) ),
	standard( PT_SCALE, "Scale", "Surface", ParamFromScale( 1.0 ) ),
	standard( PT_DEPTH, "Depth", "Surface", ParamFromDepth( 1.0 ) ),
	integer( PT_SEED, "Seed", "Surface", 1.0f, 0.0f, 99.0f ),

	standard( PT_CRUMPLE, "Crumple", "Paper", 0.7f ),
	option( PT_BOND, "Bond", "Brick", 0.0f, kBondNames, static_cast< int >( Bond::Count ) ),
	standard( PT_MORTAR, "Mortar", "Brick", ParamFromMortar( 0.010 ) ),
	option( PT_PROFILE, "Profile", "Corrugated", 0.0f, kProfileNames, static_cast< int >( Profile::Count ) ),
	standard( PT_PITCH, "Pitch", "Corrugated", ParamFromPitch( 0.076 ) ),
	option( PT_PATTERN, "Pattern", "Plate", 0.0f, kPatternNames, static_cast< int >( Pattern::Count ) ),
	integer( PT_PLEATS, "Pleats", "Drapes", 16.0f, 2.0f, 60.0f ),
	standard( PT_SAG, "Sag", "Drapes", 0.3f ),
	standard( PT_ROUGHNESS, "Roughness", "Finish", ParamFromRoughness( 0.35 ) ),

	file( PT_FILE, "Surface File", "File" ),
	option( PT_FILE_HOLDS, "File Holds", "File", 0.0f, kHoldsNames, static_cast< int >( FileHolds::Count ) ),

	standard( PT_PROJECTOR_X, "Projector X", "Projector", ParamFromOffset( 0.0 ) ),
	standard( PT_PROJECTOR_Y, "Projector Y", "Projector", ParamFromOffset( 1.0 ) ),
	standard( PT_THROW, "Throw", "Projector", ParamFromDistance( 6.0 ) ),
	option( PT_AIM, "Aim", "Projector", 0.0f, kAimNames, static_cast< int >( Aim::Count ) ),
	standard( PT_LUMENS, "Lumens", "Projector", ParamFromLumens( 10000.0 ) ),
	standard( PT_BLACK_LEVEL, "Black Level", "Projector", ParamFromBlackLevel( 0.0005 ) ),

	standard( PT_AMBIENT, "Ambient", "Room", ParamFromAmbient( 20.0 ) ),
	standard( PT_EXPOSURE, "Exposure", "Room", ParamFromExposure( 1.0 ) ),

	standard( PT_VIEWER_X, "Viewer X", "Viewer", ParamFromOffset( 2.0 ) ),
	standard( PT_VIEWER_Y, "Viewer Y", "Viewer", ParamFromOffset( -0.8 ) ),
	standard( PT_VIEWER_DISTANCE, "Viewer Distance", "Viewer", ParamFromDistance( 4.0 ) ),

	option( PT_VIEW, "View", "Output", 0.0f, kViewNames, static_cast< int >( View::Count ) ),
	standard( PT_MIX, "Mix", "Output", 1.0f ),
};

static_assert( sizeof( kTable ) / sizeof( kTable[ 0 ] ) == PT_ABOUT_FIRST, "every control needs a row in kTable" );
} // namespace

const ControlInfo& InfoOf( unsigned int id )
{
	return kTable[ std::min< unsigned int >( id, PT_ABOUT_FIRST - 1 ) ];
}

int OptionIndex( float value, int count )
{
	const int index = static_cast< int >( std::lround( value ) );
	return std::clamp( index, 0, count - 1 );
}

double WallHeightFromParam( float value )
{
	return geometric( value, kWallLow, kWallHigh );
}
float ParamFromWallHeight( double metres )
{
	return inverseGeometric( metres, kWallLow, kWallHigh );
}

double ScaleFromParam( float value )
{
	return geometric( value, kScaleLow, kScaleHigh );
}
float ParamFromScale( double scale )
{
	return inverseGeometric( scale, kScaleLow, kScaleHigh );
}

double DepthFromParam( float value )
{
	return quadratic( value, kDepthMost );
}
float ParamFromDepth( double depth )
{
	return inverseQuadratic( depth, kDepthMost );
}

double MortarFromParam( float value )
{
	return kMortarLow + ( kMortarHigh - kMortarLow ) * clamp01( value );
}
float ParamFromMortar( double metres )
{
	return static_cast< float >( std::clamp( ( metres - kMortarLow ) / ( kMortarHigh - kMortarLow ), 0.0, 1.0 ) );
}

double PitchFromParam( float value )
{
	return geometric( value, kPitchLow, kPitchHigh );
}
float ParamFromPitch( double metres )
{
	return inverseGeometric( metres, kPitchLow, kPitchHigh );
}

double RoughnessFromParam( float value )
{
	return kRoughLow + ( kRoughHigh - kRoughLow ) * clamp01( value );
}
float ParamFromRoughness( double roughness )
{
	return static_cast< float >( std::clamp( ( roughness - kRoughLow ) / ( kRoughHigh - kRoughLow ), 0.0, 1.0 ) );
}

double OffsetFromParam( float value )
{
	return signedQuadratic( value, kOffsetMost );
}
float ParamFromOffset( double metres )
{
	return inverseSignedQuadratic( metres, kOffsetMost );
}

double DistanceFromParam( float value )
{
	return geometric( value, kDistanceLow, kDistanceHigh );
}
float ParamFromDistance( double metres )
{
	return inverseGeometric( metres, kDistanceLow, kDistanceHigh );
}

double LumensFromParam( float value )
{
	return geometric( value, kLumensLow, kLumensHigh );
}
float ParamFromLumens( double lumens )
{
	return inverseGeometric( lumens, kLumensLow, kLumensHigh );
}

double BlackLevelFromParam( float value )
{
	return quadratic( value, kBlackMost );
}
float ParamFromBlackLevel( double fraction )
{
	return inverseQuadratic( fraction, kBlackMost );
}

double AmbientFromParam( float value )
{
	return quadratic( value, kAmbientMost );
}
float ParamFromAmbient( double lux )
{
	return inverseQuadratic( lux, kAmbientMost );
}

double ExposureFromParam( float value )
{
	return std::pow( 2.0, kStopsMost * ( 2.0 * clamp01( value ) - 1.0 ) );
}
float ParamFromExposure( double gain )
{
	return static_cast< float >( std::clamp( 0.5 + std::log2( std::max( gain, 1e-9 ) ) / ( 2.0 * kStopsMost ), 0.0, 1.0 ) );
}

} // namespace throwfx
