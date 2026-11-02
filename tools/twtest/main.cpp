/**
    twtest -- render Throw offline, and measure the light that lands.

    It drives the REAL plugin class, through the same ProcessOpenGL a host
    calls, in a headless CGL context, and reads the host's output back as
    floats. What the picture is measured against is the room, not the code:
    every reference here is closed-form geometry and the light law, worked in
    double from the positions the controls stand for -- the hit of a viewer's
    ray on a plane, a projector ray's landing place, the shadow an edge casts,
    a GGX lobe -- never the plugin's own march.

    So that each geometry HAS a closed form, the surfaces under test are height
    maps the harness writes as 16-bit PNGs at the test raster and loads
    through Surface File, the path a user's file takes: a recessed plateau, a
    tilted plane, a sawtooth, a step. The procedural surfaces are measured off
    the surface buffer itself (--surfaces).

        twtest --out /tmp/throw.png       the defaults on the harness's card
        twtest --list                     every parameter and its default
        twtest --pipe                     raw RGBA frames in on stdin, out on stdout
        twtest --film N                   N frames of the card, raw RGBA on stdout
        twtest --offline                  the checks that need no GL context (CI)

    `--script` is the fleet's cue format: `frame  Parameter Name  value`
    lines, held before the first key and after the last. A STANDARD (0..1)
    control is linear between its keys; anything else STEPS.

    TWTEST_RENDERER=software asks for Apple's software renderer by id, on a
    Mac with a GPU: what a GPU-less CI runner falls back to.
*/

#include "Controls.h"
#include "Room.h"
#include "Shaders.h"
#include "Throw.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <stb_image.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <unistd.h>
#include <vector>

using namespace throwfx;

namespace
{
using Floats = std::vector< float >;
using Bytes  = std::vector< unsigned char >;
using Words  = std::vector< uint16_t >;

//---------------------------------------------------------------------------
// Reporting.
//---------------------------------------------------------------------------
int g_failures = 0;

std::string fmt( const char* format, ... )
{
	char buffer[ 4096 ];
	va_list args;
	va_start( args, format );
	std::vsnprintf( buffer, sizeof( buffer ), format, args );
	va_end( args );
	return buffer;
}

void Check( bool condition, const std::string& message )
{
	std::printf( "  %s  %s\n", condition ? "ok  " : "FAIL", message.c_str() );
	if( !condition )
		++g_failures;
}

int Verdict()
{
	std::printf( "\n  %s\n", g_failures == 0 ? "PASS" : "FAIL" );
	return g_failures == 0 ? 0 : 1;
}

//---------------------------------------------------------------------------
// Tolerances, each with where it comes from.
//---------------------------------------------------------------------------
/// sRGB there and back in GLSL: pow is exp2( y log2 x ), each promised to
/// about 3 ulp, so a code value comes back within a few 1e-7. 1e-5 is that
/// with a margin, and a hundred times under an 8-bit step.
constexpr double kCodeRound = 1e-5;
/// The light law in float, relative: a normal, a distance, a cube, a few
/// roundings each near 1e-7, and the hit point the march's secant puts on a
/// plane to float precision. 1e-4 is that with room, and still 40 times under
/// what the lightest wrong model (the inverse square to the front plane, not
/// the relief, on a ramp) changes.
constexpr double kLightRel = 1e-4;
/// A Gaussian dot's centroid, in pixels. GPUs filter with 8-bit weights (a
/// 256th of a texel), which moves a dot's samples, not its centre, by up to a
/// 512th of a sample's difference; over a dot two pixels wide that sums to
/// under 0.01 px. 0.02 px is that with room.
constexpr double kCentroidPx = 0.02;
/// Two frames of what must be the same picture. A GPU repeats itself bit for
/// bit and is held to that. Apple's software renderer does not: the same
/// instance, the same input, and the next frame changed 43080 values (Drapes,
/// 320x180); a running instance's frames differed from a fresh one's by up to
/// 6.9e-5 (Plate). Most likely a variant of the program compiled behind the
/// first frames and swapped in, rounding differently, with a march carrying a
/// last-bit difference a long way. So it is held to 1e-4 of a
/// value, with one value in a thousand allowed past that for a march step
/// that went the other way. A surface a step stale misses by tenths, on most
/// of the frame.
constexpr double kRepeat         = 1e-4;
constexpr double kRepeatOutliers = 1e-3;

struct Gap
{
	size_t values = 0, differ = 0, past = 0;
	double worst = 0.0;
	bool Exact() const { return differ == 0; }
	bool Same() const { return values > 0 && past <= static_cast< size_t >( kRepeatOutliers * values ); }
	std::string Say() const
	{
		if( differ == 0 )
			return "bit for bit";
		char text[ 160 ];
		std::snprintf( text, sizeof text, "%zu of %zu values differ, by up to %.1e (bound %.0e), %zu past it", differ, values, worst,
		               kRepeat, past );
		return text;
	}
};
/// One value in two frames that must agree: kRepeat's bound, and how far off.
bool sameValue( float want, float got, double& worst )
{
	const double d = std::fabs( double( want ) - double( got ) ) / std::max( 1.0, std::fabs( double( want ) ) );
	worst          = std::max( worst, d );
	return d <= kRepeat;
}
Gap gapBetween( const Floats& want, const Floats& got )
{
	Gap gap;
	if( want.size() != got.size() || want.empty() )
		return gap;
	gap.values = want.size();
	for( size_t k = 0; k < want.size(); ++k )
		if( want[ k ] != got[ k ] )
		{
			++gap.differ;
			const double d = std::fabs( double( want[ k ] ) - double( got[ k ] ) ) / std::max( 1.0, std::fabs( double( want[ k ] ) ) );
			gap.worst      = std::max( gap.worst, d );
			if( d > kRepeat )
				++gap.past;
		}
	return gap;
}

//---------------------------------------------------------------------------
// PNG. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( Bytes& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( Bytes& out, const char* type, const Bytes& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

/// `raw` is the filtered scanlines, top row first.
bool writePngRaw( const std::string& path, int width, int height, int depth, int colourType, const Bytes& raw )
{
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	Bytes compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	Bytes png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	Bytes ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.insert( ihdr.end(), { static_cast< unsigned char >( depth ), static_cast< unsigned char >( colourType ), 0, 0, 0 } );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );
	FILE* file = std::fopen( path.c_str(), "wb" );
	if( !file )
		return false;
	const size_t written = std::fwrite( png.data(), 1, png.size(), file );
	std::fclose( file );
	return written == png.size();
}

/// `rgba` is floats, row 0 at the BOTTOM (GL's order); the file is written
/// top row first, which is the only place anything here flips.
bool writePng( const std::string& path, int width, int height, const Floats& rgba )
{
	Bytes raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
			for( int c = 0; c < 4; ++c )
			{
				const float v = c == 3 ? 1.0f : rgba[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ];
				raw.push_back( static_cast< unsigned char >( std::lround( std::clamp( v, 0.0f, 1.0f ) * 255.0f ) ) );
			}
	}
	return writePngRaw( path, width, height, 8, 6, raw );
}

/// A 16-bit grey height map, bottom row first.
bool writeGrey16( const std::string& path, int width, int height, const Words& grey )
{
	Bytes raw;
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		for( int x = 0; x < width; ++x )
		{
			const uint16_t v = grey[ static_cast< size_t >( y ) * width + x ];
			raw.push_back( static_cast< unsigned char >( v >> 8 ) );
			raw.push_back( static_cast< unsigned char >( v & 0xFF ) );
		}
	}
	return writePngRaw( path, width, height, 16, 0, raw );
}

/// An 8-bit file, grey (channels 1) or RGB (3), bottom row first.
bool write8( const std::string& path, int width, int height, int channels, const Bytes& pixels )
{
	Bytes raw;
	for( int y = height - 1; y >= 0; --y )
	{
		raw.push_back( 0 );
		const size_t row = static_cast< size_t >( y ) * width * channels;
		raw.insert( raw.end(), pixels.begin() + static_cast< std::ptrdiff_t >( row ),
		            pixels.begin() + static_cast< std::ptrdiff_t >( row + static_cast< size_t >( width ) * channels ) );
	}
	return writePngRaw( path, width, height, 8, channels == 1 ? 0 : 2, raw );
}

/// Somewhere to put the height maps.
std::string scratchPath( const std::string& name )
{
	const char* dir = std::getenv( "TMPDIR" );
	std::string base = dir != nullptr ? dir : "/tmp/";
	if( base.back() != '/' )
		base += '/';
	return base + "twtest-" + std::to_string( getpid() ) + "-" + name;
}

//---------------------------------------------------------------------------
// The context.
//---------------------------------------------------------------------------
bool g_software = false;

CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute fallback[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "TWTEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		g_software = true;
		std::fprintf( stderr, "twtest: TWTEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( fallback, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}
	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;
	CGLSetCurrentContext( context );
	return context;
}

const char* kindName( unsigned int type )
{
	switch( type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_STANDARD: return "standard";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_FILE: return "file";
	default: return "other";
	}
}

/// Only a STANDARD control ramps between cues; a choice, an integer, a switch
/// or a press steps.
bool stepsBetweenCues( unsigned int type )
{
	return type != FF_TYPE_STANDARD;
}

//---------------------------------------------------------------------------
// The cue sheet.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( std::istream& in, const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::string line;
	int lineNumber = 0;
	while( std::getline( in, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream words( line );
		int frame = 0;
		if( !( words >> frame ) )
			continue;
		std::vector< std::string > parts;
		std::string word;
		while( words >> word )
			parts.push_back( word );
		if( parts.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		char* end         = nullptr;
		const float value = std::strtof( parts.back().c_str(), &end );
		if( end == parts.back().c_str() || *end != '\0' )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": '" + parts.back() + "' is not a number";
			return {};
		}
		parts.pop_back();
		std::string name = parts.front();
		for( size_t i = 1; i < parts.size(); ++i )
			name += " " + parts[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::stable_sort( entry.second.begin(), entry.second.end(),
		                  []( const std::pair< int, float >& a, const std::pair< int, float >& b ) { return a.first < b.first; } );
	return tracks;
}

float valueAt( const Track& track, int frame, bool ramp )
{
	if( track.empty() )
		return 0.0f;
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 0; i + 1 < track.size(); ++i )
	{
		const auto& a = track[ i ];
		const auto& b = track[ i + 1 ];
		if( frame >= a.first && frame < b.first )
		{
			if( !ramp )
				return a.second;
			const float t = static_cast< float >( frame - a.first ) / static_cast< float >( b.first - a.first );
			return a.second + ( b.second - a.second ) * t;
		}
	}
	return track.back().second;
}

//---------------------------------------------------------------------------
// sRGB in double, for the references.
//---------------------------------------------------------------------------
double linearOf( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}
double codeOf( double light )
{
	return light <= 0.0031308 ? light * 12.92 : 1.055 * std::pow( light, 1.0 / 2.4 ) - 0.055;
}

//---------------------------------------------------------------------------
// Pictures. Every one is rows BOTTOM first (GL's order), as uploaded.
//---------------------------------------------------------------------------
uint32_t pcg( uint32_t v )
{
	const uint32_t state = v * 747796405u + 2891336453u;
	const uint32_t word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
double hash01( uint32_t a, uint32_t b = 0 )
{
	return pcg( a * 2654435761u ^ pcg( b + 0x9e3779b9u ) ) * ( 1.0 / 4294967296.0 );
}

float eightBit( double v )
{
	return static_cast< float >( std::lround( std::clamp( v, 0.0, 1.0 ) * 255.0 ) / 255.0 );
}

/// The look card: a sunset sky, a sun, a skyline, a stripe of saturated bars
/// and a white title block -- edges, flats and colour.
Floats buildCard( int width, int height )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	const double s = std::min( width, height );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double u = ( x + 0.5 - 0.5 * width ) / s, v = ( y + 0.5 - 0.5 * height ) / s;
			const double k = std::clamp( 0.5 + v, 0.0, 1.0 );
			double r = 0.95 - 0.8 * k, g = 0.45 - 0.3 * k, b = 0.25 + 0.5 * k;
			const double d = std::hypot( u + 0.25, v + 0.02 );
			if( d < 0.2 )
			{
				r = 1.0;
				g = 0.85 - 0.5 * d;
				b = 0.35;
			}
			const int block  = static_cast< int >( std::floor( ( u + 20.0 ) * 11.0 ) );
			const double top = -0.12 + 0.16 * hash01( static_cast< uint32_t >( block ), 7 );
			if( v < top )
			{
				r = g = b = 0.05;
				const int wx = static_cast< int >( std::floor( ( u + 20.0 ) * 70.0 ) ), wy = static_cast< int >( std::floor( v * 60.0 + 100.0 ) );
				if( ( wx % 3 ) != 0 && ( wy % 2 ) == 0 && hash01( static_cast< uint32_t >( wx * 131 + wy ), 9 ) > 0.55 )
				{
					r = 1.0;
					g = 0.9;
					b = 0.55;
				}
			}
			if( v < -0.36 )
			{
				const int bar               = std::min( 6, static_cast< int >( std::floor( ( x + 0.5 ) / width * 7.0 ) ) );
				const double bars[ 7 ][ 3 ] = { { 1, 1, 1 }, { 1, 1, 0 }, { 0, 1, 1 }, { 0, 1, 0 }, { 1, 0, 1 }, { 1, 0, 0 }, { 0, 0, 1 } };
				r = 0.75 * bars[ bar ][ 0 ];
				g = 0.75 * bars[ bar ][ 1 ];
				b = 0.75 * bars[ bar ][ 2 ];
			}
			if( std::fabs( u - 0.42 ) < 0.28 && std::fabs( v - 0.3 ) < 0.06 )
				r = g = b = 0.95;
			float* o = &card[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
			o[ 0 ]   = eightBit( r );
			o[ 1 ]   = eightBit( g );
			o[ 2 ]   = eightBit( b );
			o[ 3 ]   = 1.0f;
		}
	return card;
}

/// A picture from a file, in place of the card: any format stb_image reads,
/// at 16 bits a channel, scaled bilinearly to the raster and turned to GL's
/// rows, bottom first. Empty if it will not read.
Floats clipCard( const std::string& path, int width, int height )
{
	int w = 0, h = 0, channels = 0;
	stbi_us* data = stbi_load_16( path.c_str(), &w, &h, &channels, 4 );
	if( data == nullptr )
		return {};
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = std::clamp( ( x + 0.5 ) * w / width - 0.5, 0.0, w - 1.0 );
			const double fy = std::clamp( ( height - 0.5 - y ) * h / height - 0.5, 0.0, h - 1.0 );
			const int x0 = static_cast< int >( fx ), y0 = static_cast< int >( fy );
			const int x1 = std::min( x0 + 1, w - 1 ), y1 = std::min( y0 + 1, h - 1 );
			const double tx = fx - x0, ty = fy - y0;
			for( int c = 0; c < 4; ++c )
			{
				auto at = [&]( int i, int j ) { return data[ ( static_cast< size_t >( j ) * w + i ) * 4 + c ] / 65535.0; };
				const double top = at( x0, y0 ) * ( 1.0 - tx ) + at( x1, y0 ) * tx;
				const double bot = at( x0, y1 ) * ( 1.0 - tx ) + at( x1, y1 ) * tx;
				card[ ( static_cast< size_t >( y ) * width + x ) * 4 + c ] = static_cast< float >( top * ( 1.0 - ty ) + bot * ty );
			}
		}
	stbi_image_free( data );
	return card;
}

Floats flatCard( int width, int height, double r, double g, double b )
{
	Floats card( static_cast< size_t >( width ) * height * 4 );
	for( size_t i = 0; i < card.size(); i += 4 )
	{
		card[ i ]     = static_cast< float >( r );
		card[ i + 1 ] = static_cast< float >( g );
		card[ i + 2 ] = static_cast< float >( b );
		card[ i + 3 ] = 1.0f;
	}
	return card;
}

GLuint makeTexture( int width, int height, const float* pixels, GLint format = GL_RGBA32F )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, format, width, height, 0, GL_RGBA, GL_FLOAT, pixels );
	//Linear, as a host's clip texture is.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

//---------------------------------------------------------------------------
// A rig: the real plugin and a float output framebuffer.
//---------------------------------------------------------------------------
struct Rig
{
	ThrowPlugin plugin;
	int width = 0, height = 0;
	GLuint sourceTexture = 0, outputTexture = 0, outputFBO = 0;
	Floats card;

	ProcessOpenGLStruct process    = {};
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };

	~Rig()
	{
		plugin.DeInitGL();
		release();
	}

	void release()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool attach( int w, int h, const Floats* picture )
	{
		width         = w;
		height        = h;
		outputTexture = makeTexture( width, height, nullptr );
		glGenFramebuffers( 1, &outputFBO );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, outputTexture, 0 );
		if( glCheckFramebufferStatus( GL_FRAMEBUFFER ) != GL_FRAMEBUFFER_COMPLETE )
			return false;
		process.HostFBO   = outputFBO;
		card              = picture ? *picture : buildCard( width, height );
		sourceTexture     = makeTexture( width, height, card.data() );
		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;
		process.numInputTextures                        = 1;
		process.inputTextures                           = inputs;
		return true;
	}

	bool Init( int w, int h, const Floats* picture = nullptr )
	{
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( w );
		viewport.height             = static_cast< FFUInt32 >( h );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see ~/Library/Logs/throw for which shader\n" );
			return false;
		}
		return attach( w, h, picture );
	}

	/// The host's raster changes under a running instance: no InitGL.
	bool Resize( int w, int h, const Floats* picture = nullptr )
	{
		release();
		return attach( w, h, picture );
	}

	void Upload( const Floats& picture )
	{
		card = picture;
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, picture.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void Set( unsigned int id, double value )
	{
		plugin.SetFloatParameter( id, static_cast< float >( value ) );
	}
	template< typename E >
	void Option( unsigned int id, E value )
	{
		plugin.SetFloatParameter( id, static_cast< float >( static_cast< int >( value ) ) );
	}
	float Get( unsigned int id )
	{
		return plugin.GetFloatParameter( id );
	}
	void File( const std::string& path )
	{
		plugin.SetTextParameter( PT_FILE, path.c_str() );
	}

	bool Render( int frames = 1 )
	{
		for( int i = 0; i < frames; ++i )
		{
			glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
			glViewport( 0, 0, width, height );
			glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
			glClear( GL_COLOR_BUFFER_BIT );
			if( plugin.ProcessOpenGL( &process ) != FF_SUCCESS )
			{
				std::fprintf( stderr, "ProcessOpenGL failed\n" );
				return false;
			}
		}
		return true;
	}

	Floats Output() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_FLOAT, pixels.data() );
		return pixels;
	}

	/// The surface buffer: linear albedo in rgb, height in metres in a.
	Floats Surface() const
	{
		Floats pixels( static_cast< size_t >( width ) * height * 4 );
		glBindTexture( GL_TEXTURE_2D, plugin.SurfaceTextureForTest() );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glGetTexImage( GL_TEXTURE_2D, 0, GL_RGBA, GL_FLOAT, pixels.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		return pixels;
	}

	/// Render and read back, in one.
	Floats Frame()
	{
		return Render( 1 ) ? Output() : Floats();
	}
};

const float* pixelAt( const Floats& image, int width, int x, int y )
{
	return &image[ ( static_cast< size_t >( y ) * width + x ) * 4 ];
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	float value;
	unsigned int type;
};

std::vector< NamedParameter > listParameters( ThrowPlugin& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const char* const name = plugin.GetParamName( i );
		list.push_back( NamedParameter { name ? name : "?", i, plugin.GetFloatParameter( i ), plugin.GetParamType( i ) } );
	}
	return list;
}

/// `Name=Value`. The value is a number, an option's element by name, or for
/// Surface File a path; anything else is refused, never read as 0
/// (polyhedral's trap: strtof( "Brick" ) is 0, silently).
bool applySetting( ThrowPlugin& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.find( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name  = assignment.substr( 0, equals );
	const std::string value = assignment.substr( equals + 1 );
	for( const NamedParameter& parameter : listParameters( plugin ) )
		if( parameter.name == name )
		{
			if( parameter.type == FF_TYPE_FILE )
				return plugin.SetTextParameter( parameter.index, value.c_str() ) == FF_SUCCESS;
			if( parameter.type == FF_TYPE_TEXT || parameter.type == FF_TYPE_EVENT )
			{
				error = name + " cannot be set from here";
				return false;
			}
			if( parameter.type == FF_TYPE_OPTION )
				for( unsigned int e = 0; e < plugin.GetNumParamElements( parameter.index ); ++e )
					if( value == plugin.GetParamElementName( parameter.index, e ) )
					{
						plugin.SetFloatParameter( parameter.index, static_cast< float >( e ) );
						return true;
					}
			char* end       = nullptr;
			const float got = std::strtof( value.c_str(), &end );
			if( value.empty() || end == value.c_str() || *end != '\0' )
			{
				error = "'" + value + "' is not a number" + ( parameter.type == FF_TYPE_OPTION ? " or an option of " + name : "" );
				return false;
			}
			plugin.SetFloatParameter( parameter.index, got );
			return true;
		}
	error = "no parameter called '" + name + "'";
	return false;
}

struct Cue
{
	Track track;
	bool ramp;
};

bool bindScript( ThrowPlugin& plugin, const std::string& path, std::map< unsigned int, Cue >& out, std::string& error )
{
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	const std::map< std::string, Track > tracks = loadScript( file, path, error );
	if( !error.empty() )
		return false;
	const std::vector< NamedParameter > known = listParameters( plugin );
	for( const auto& entry : tracks )
	{
		bool found = false;
		for( const NamedParameter& parameter : known )
			if( parameter.name == entry.first )
			{
				out[ parameter.index ] = Cue { entry.second, !stepsBetweenCues( parameter.type ) };
				found                  = true;
			}
		if( !found )
		{
			error = "script names '" + entry.first + "', which is not a parameter (try --list)";
			return false;
		}
	}
	return true;
}

//===========================================================================
// --pipe and --film. Raw RGBA, top row first. --film renders the card.
//===========================================================================
int runPipe( int width, int height, const std::string& scriptPath, int count, bool readStdin, const std::vector< std::string >& settings )
{
	Rig rig;
	if( !rig.Init( width, height ) )
		return 1;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( !applySetting( rig.plugin, setting, error ) )
		{
			std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
			return 2;
		}
	}
	std::map< unsigned int, Cue > automation;
	if( !scriptPath.empty() )
	{
		std::string error;
		if( !bindScript( rig.plugin, scriptPath, automation, error ) )
		{
			std::fprintf( stderr, "%s\n", error.c_str() );
			return 2;
		}
	}

	Bytes in( static_cast< size_t >( width ) * height * 4 );
	Floats picture( in.size() );
	for( int index = 0; readStdin || index < count; ++index )
	{
		if( readStdin )
		{
			size_t filled = 0;
			while( filled < in.size() )
			{
				const ssize_t got = read( STDIN_FILENO, in.data() + filled, in.size() - filled );
				if( got <= 0 )
					break;
				filled += static_cast< size_t >( got );
			}
			//A partial frame is the end of the stream, never a frame.
			if( filled < in.size() )
			{
				if( filled > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes): dropped\n", filled, in.size() );
				break;
			}
			for( int y = 0; y < height; ++y )
				for( int x = 0; x < width * 4; ++x )
					picture[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ] = in[ static_cast< size_t >( y ) * width * 4 + x ] / 255.0f;
			rig.Upload( picture );
		}

		for( const auto& cue : automation )
			rig.plugin.SetFloatParameter( cue.first, valueAt( cue.second.track, index, cue.second.ramp ) );
		if( !rig.Render( 1 ) )
			return 1;

		const Floats out = rig.Output();
		Bytes bytes( in.size() );
		for( int y = 0; y < height; ++y )
			for( int x = 0; x < width * 4; ++x )
				bytes[ static_cast< size_t >( y ) * width * 4 + x ] = static_cast< unsigned char >(
					std::lround( std::clamp( out[ static_cast< size_t >( height - 1 - y ) * width * 4 + x ], 0.0f, 1.0f ) * 255.0f ) );
		size_t written = 0;
		while( written < bytes.size() )
		{
			const ssize_t put = write( STDOUT_FILENO, bytes.data() + written, bytes.size() - written );
			//The reader has gone. SIGPIPE is ignored in main(), so this is
			//EPIPE and not a silent 141: say so and stop.
			if( put <= 0 )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				return 1;
			}
			written += static_cast< size_t >( put );
		}
	}
	return 0;
}

//===========================================================================
// The real sizes the README states, written here and not read from Room.h:
// a size wrong in the plugin's facts AND its shader is then still caught.
//===========================================================================
namespace stated
{
constexpr double kBrickLength       = 0.215; ///< BS EN 771-1 work size
constexpr double kBrickHeight       = 0.065;
constexpr double kHeaderLength      = 0.1025;///< half a brick less half a joint
constexpr double kSineDepthPerPitch = 18.0 / 76.0;///< 3-inch corrugated: 76 mm pitch, 18 mm deep
constexpr double kPlateCell         = 0.030;
constexpr double kFileRelief        = 0.10;  ///< white at the front, black 100 mm behind
} // namespace stated

//===========================================================================
// The room, in double, as the harness reads the controls.
//===========================================================================
struct V3
{
	double x = 0.0, y = 0.0, z = 0.0;
};
V3 operator+( V3 a, V3 b )
{
	return { a.x + b.x, a.y + b.y, a.z + b.z };
}
V3 operator-( V3 a, V3 b )
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}
V3 operator*( V3 a, double s )
{
	return { a.x * s, a.y * s, a.z * s };
}
double dot( V3 a, V3 b )
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}
double norm( V3 a )
{
	return std::sqrt( dot( a, a ) );
}
V3 unit( V3 a )
{
	return a * ( 1.0 / norm( a ) );
}

/// The geometry the controls stand for, worked out here and not taken from
/// the plugin: W from the raster's aspect, the Pointed basis from the axis.
struct Scene
{
	int w = 0, h = 0;
	double W = 0.0, H = 0.0;
	V3 C, V;
	bool pointed = false;
	V3 axis, right, up;
	double d = 0.0;

	double texel() const
	{
		return H / h;
	}
	/// A pixel centre's place on the front plane.
	double xOf( double i ) const
	{
		return ( ( i + 0.5 ) / w - 0.5 ) * W;
	}
	double yOf( double j ) const
	{
		return ( ( j + 0.5 ) / h - 0.5 ) * H;
	}
	/// ...and back, in pixel-index coordinates.
	double iOf( double x ) const
	{
		return ( x / W + 0.5 ) * w - 0.5;
	}
	double jOf( double y ) const
	{
		return ( y / H + 0.5 ) * h - 0.5;
	}
};

Scene sceneOf( Rig& rig )
{
	Scene s;
	s.w       = rig.width;
	s.h       = rig.height;
	s.H       = WallHeightFromParam( rig.Get( PT_WALL_HEIGHT ) );
	s.W       = s.H * rig.width / rig.height;
	s.C       = { OffsetFromParam( rig.Get( PT_PROJECTOR_X ) ), OffsetFromParam( rig.Get( PT_PROJECTOR_Y ) ), DistanceFromParam( rig.Get( PT_THROW ) ) };
	s.V       = { OffsetFromParam( rig.Get( PT_VIEWER_X ) ), OffsetFromParam( rig.Get( PT_VIEWER_Y ) ), DistanceFromParam( rig.Get( PT_VIEWER_DISTANCE ) ) };
	s.pointed = std::lround( rig.Get( PT_AIM ) ) == static_cast< int >( Aim::Pointed );
	if( s.pointed )
	{
		s.axis  = unit( V3 {} - s.C );
		s.right = unit( V3 { -s.axis.z, 0.0, s.axis.x } );//axis x world-up, by hand
		s.up    = { s.right.y * s.axis.z - s.right.z * s.axis.y, s.right.z * s.axis.x - s.right.x * s.axis.z,
		            s.right.x * s.axis.y - s.right.y * s.axis.x };
		s.d     = norm( s.C );
	}
	else
	{
		s.axis = { 0.0, 0.0, -1.0 };
		s.d    = s.C.z;
	}
	return s;
}

/// What the projector sends to point S of a face with normal n.
struct Beam
{
	bool inside = false;
	double g    = 0.0;///< irradiance relative to the design screen's
	double qu = 0.0, qv = 0.0;
	V3 w;///< towards the lens
	double facing = 0.0;
};

Beam beamAt( const Scene& s, V3 S, V3 n )
{
	Beam b;
	const V3 toLens = s.C - S;
	const double r  = norm( toLens );
	b.w             = toLens * ( 1.0 / r );
	double ahead = 0.0, cosA = 0.0;
	if( !s.pointed )
	{
		ahead = s.C.z - S.z;
		cosA  = b.w.z;
		//The ray from the lens through S meets the front plane at Q.
		const double k = S.z / ( s.C.z - S.z );
		b.qu           = ( S.x + ( S.x - s.C.x ) * k ) / s.W + 0.5;
		b.qv           = ( S.y + ( S.y - s.C.y ) * k ) / s.H + 0.5;
	}
	else
	{
		const V3 a = S - s.C;
		ahead      = dot( a, s.axis );
		cosA       = ahead / r;
		b.qu       = 0.5 + s.d * dot( a, s.right ) / ( ahead * s.W );
		b.qv       = 0.5 + s.d * dot( a, s.up ) / ( ahead * s.H );
	}
	b.inside = ahead > 0.0 && b.qu >= 0.0 && b.qu <= 1.0 && b.qv >= 0.0 && b.qv <= 1.0;
	b.facing = dot( n, b.w );
	b.g      = b.inside ? s.d * s.d * std::max( b.facing, 0.0 ) / ( r * r * cosA * cosA * cosA ) : 0.0;
	return b;
}

/// The clip's code value at (u, v), bilinear and clamped, as a GPU samples it.
double clipCode( const Floats& card, int w, int h, double u, double v, int c )
{
	const double x = std::clamp( u * w - 0.5, 0.0, w - 1.0 ), y = std::clamp( v * h - 0.5, 0.0, h - 1.0 );
	const int x0 = std::min( static_cast< int >( x ), w - 2 ), y0 = std::min( static_cast< int >( y ), h - 2 );
	const double fx = x - x0, fy = y - y0;
	auto at = [ & ]( int i, int j ) { return static_cast< double >( card[ ( static_cast< size_t >( j ) * w + i ) * 4 + c ] ); };
	return ( 1 - fy ) * ( ( 1 - fx ) * at( x0, y0 ) + fx * at( x0 + 1, y0 ) ) + fy * ( ( 1 - fx ) * at( x0, y0 + 1 ) + fx * at( x0 + 1, y0 + 1 ) );
}

//===========================================================================
// The checks.
//
// Each takes a Perturb. With every field at its default the check scores the
// plugin against the stated law; `--negative` sets one field at a time to a
// deliberately wrong MODEL -- a hook in the plugin, so the shipped code
// computes the wrong thing -- and requires the check to FAIL.
//===========================================================================
struct Perturb
{
	int hooks          = 0;
	bool unitsAspect   = false;///< --units: the harness takes W as H x height / width
	bool cuesRamp      = false;///< --cues: ramp every control
};
using CheckFn = int ( * )( const Perturb& );

struct Raster
{
	int w, h;
};
std::vector< Raster > kRasters = { { 1280, 720 }, { 320, 180 } };

/// A rig on the File surface with no room light and an ideal projector, so
/// that what comes back is the projector's light alone. Depth 1, Scale 1:
/// R = 0.1 m exactly.
void quietRoom( Rig& rig )
{
	rig.Set( PT_AMBIENT, 0.0 );
	rig.Set( PT_BLACK_LEVEL, 0.0 );
	rig.Set( PT_EXPOSURE, ParamFromExposure( 1.0 ) );
}

/// Load a height map through Surface File.
bool useMap( Rig& rig, const std::string& name, const Words& grey )
{
	const std::string path = scratchPath( name + ".png" );
	if( !writeGrey16( path, rig.width, rig.height, grey ) )
		return false;
	rig.Option( PT_SURFACE, Surface::File );
	rig.Option( PT_FILE_HOLDS, FileHolds::Height );
	rig.Set( PT_SCALE, ParamFromScale( 1.0 ) );
	rig.File( path );
	return true;
}

double reliefOf( Rig& rig )
{
	return stated::kFileRelief * ScaleFromParam( rig.Get( PT_SCALE ) ) * DepthFromParam( rig.Get( PT_DEPTH ) );
}

//===========================================================================
// --flat: a flat wall gives the clip back, times the surface's colour.
//===========================================================================
int runFlat( const Perturb& perturb )
{
	std::printf( "\n=== flat: a flat wall under Lens Shift gives the clip back exactly, wherever the projector and the viewer are,"
	             " times the surface's albedo\n" );
	struct Place
	{
		V3 C, V;
	};
	const Place places[] = {
		{ { 0.0, 1.0, 6.0 }, { 2.0, -0.8, 4.0 } },
		{ { -4.0, 2.5, 2.0 }, { 3.0, 0.0, 1.5 } },
		{ { 6.0, -3.0, 10.0 }, { -7.0, 4.0, 20.0 } },
	};
	for( const Raster& raster : kRasters )
		for( const Place& place : places )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			rig.plugin.SetHooksForTest( perturb.hooks );
			quietRoom( rig );
			rig.Set( PT_PROJECTOR_X, ParamFromOffset( place.C.x ) );
			rig.Set( PT_PROJECTOR_Y, ParamFromOffset( place.C.y ) );
			rig.Set( PT_THROW, ParamFromDistance( place.C.z ) );
			rig.Set( PT_VIEWER_X, ParamFromOffset( place.V.x ) );
			rig.Set( PT_VIEWER_Y, ParamFromOffset( place.V.y ) );
			rig.Set( PT_VIEWER_DISTANCE, ParamFromDistance( place.V.z ) );

			//Flat: the clip itself.
			rig.Option( PT_SURFACE, Surface::Flat );
			const Floats flat = rig.Frame();
			double worst      = 0.0;
			for( size_t i = 0; i < flat.size(); ++i )
				worst = std::max( worst, std::fabs( static_cast< double >( flat[ i ] ) - rig.card[ i ] ) );

			//Brick at Depth 0: flat, but brick-coloured. out( clip ) is
			//out( white ) times the clip, per pixel and channel, in light.
			rig.Option( PT_SURFACE, Surface::Brick );
			rig.Set( PT_DEPTH, 0.0 );
			const Floats card  = rig.card;
			const Floats onCard = rig.Frame();
			rig.Upload( flatCard( raster.w, raster.h, 1.0, 1.0, 1.0 ) );
			const Floats onWhite = rig.Frame();
			double worstBrick    = 0.0;
			for( size_t i = 0; i < onCard.size(); ++i )
			{
				if( i % 4 == 3 )
					continue;
				const double want = codeOf( linearOf( onWhite[ i ] ) * linearOf( card[ i ] ) );
				worstBrick        = std::max( worstBrick, std::fabs( onCard[ i ] - want ) );
			}
			//Pure blue: the brick's own blue, and no red or green at all.
			rig.Upload( flatCard( raster.w, raster.h, 0.0, 0.0, 1.0 ) );
			const Floats onBlue = rig.Frame();
			rig.Set( PT_VIEW, static_cast< float >( View::Albedo ) );
			const Floats albedo = rig.Frame();
			double worstBlue    = 0.0, leak = 0.0;
			for( size_t i = 0; i < onBlue.size(); i += 4 )
			{
				leak      = std::max( { leak, static_cast< double >( onBlue[ i ] ), static_cast< double >( onBlue[ i + 1 ] ) } );
				worstBlue = std::max( worstBlue, std::fabs( static_cast< double >( onBlue[ i + 2 ] ) - albedo[ i + 2 ] ) );
			}
			Check( worst <= kCodeRound && worstBrick <= 2.0 * kCodeRound && worstBlue <= kCodeRound && leak == 0.0,
			       fmt( "%4dx%-4d lens (%+.0f, %+.1f, %.0f) m, viewer (%+.0f, %+.1f, %.1f) m: Flat returns the clip within %.1e (bound %.0e); "
			            "brick at Depth 0 is the clip times out( white ) within %.1e; pure blue gives the brick's blue within %.1e, red and green %g",
			            raster.w, raster.h, place.C.x, place.C.y, place.C.z, place.V.x, place.V.y, place.V.z, worst, kCodeRound, worstBrick,
			            worstBlue, leak ) );
		}
	return Verdict();
}

//===========================================================================
// --parallax: a recessed plateau, dots in the clip, the viewer moved.
//===========================================================================
/// A dot in the clip, in frame metres.
struct Dot
{
	double x, y;
};

/// Gaussian dots, sigma in pixels, written as code values of linear light.
Floats dotCard( const Scene& s, const std::vector< Dot >& dots, double sigmaPx )
{
	Floats card( static_cast< size_t >( s.w ) * s.h * 4 );
	const double sigma = sigmaPx * s.texel();
	for( int j = 0; j < s.h; ++j )
		for( int i = 0; i < s.w; ++i )
		{
			double L = 0.0;
			for( const Dot& d : dots )
			{
				const double dx = s.xOf( i ) - d.x, dy = s.yOf( j ) - d.y;
				L += std::exp( -( dx * dx + dy * dy ) / ( 2.0 * sigma * sigma ) );
			}
			float* o = &card[ ( static_cast< size_t >( j ) * s.w + i ) * 4 ];
			o[ 0 ] = o[ 1 ] = o[ 2 ] = static_cast< float >( codeOf( std::min( L, 1.0 ) ) );
			o[ 3 ]                   = 1.0f;
		}
	return card;
}

/// Light-weighted centroid of a window, in pixel-index coordinates.
bool centroid( const Floats& image, const Scene& s, double ci, double cj, int half, std::function< double( int, int ) > weight, double& x,
               double& y )
{
	const int i0 = static_cast< int >( std::lround( ci ) ) - half, j0 = static_cast< int >( std::lround( cj ) ) - half;
	if( i0 < 0 || j0 < 0 || i0 + 2 * half >= s.w || j0 + 2 * half >= s.h )
		return false;
	double sw = 0.0, sx = 0.0, sy = 0.0;
	for( int j = j0; j <= j0 + 2 * half; ++j )
		for( int i = i0; i <= i0 + 2 * half; ++i )
		{
			const double wgt = weight( i, j );
			sw += wgt;
			sx += wgt * i;
			sy += wgt * j;
		}
	(void)image;
	if( !( sw > 0.0 ) )
		return false;
	x = sx / sw;
	y = sy / sw;
	return true;
}

int runParallax( const Perturb& perturb )
{
	std::printf( "\n=== parallax: a plateau recessed 100 mm; each dot lands where the projector's ray meets the floor, and is seen"
	             " where the viewer's ray from it crosses the front plane\n" );
	struct Eye
	{
		V3 V;
		const char* what;
	};
	const Eye eyes[] = {
		{ { 2.0, -0.8, 4.0 }, "right and below" },
		{ { -3.0, 1.5, 3.0 }, "left and above" },
		{ { 1.0, -0.8, 4.0 }, "half as far right" },
		{ { 0.0, 1.0, 6.0 }, "at the lens" },
	};
	for( const Raster& raster : kRasters )
	{
		std::vector< double > shiftAt;//one dot's displacement, per eye
		for( const Eye& eye : eyes )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			rig.plugin.SetHooksForTest( perturb.hooks );
			quietRoom( rig );
			rig.Set( PT_DEPTH, ParamFromDepth( 1.0 ) );
			rig.Set( PT_VIEWER_X, ParamFromOffset( eye.V.x ) );
			rig.Set( PT_VIEWER_Y, ParamFromOffset( eye.V.y ) );
			rig.Set( PT_VIEWER_DISTANCE, ParamFromDistance( eye.V.z ) );
			Scene s        = sceneOf( rig );
			const double R = reliefOf( rig );

			//The recess: the middle half of the frame each way, whole texels.
			const int i0 = s.w / 4, i1 = 3 * s.w / 4, j0 = s.h / 4, j1 = 3 * s.h / 4;
			Words grey( static_cast< size_t >( s.w ) * s.h, 65535 );
			for( int j = j0; j < j1; ++j )
				for( int i = i0; i < i1; ++i )
					grey[ static_cast< size_t >( j ) * s.w + i ] = 0;
			if( !useMap( rig, "plateau", grey ) )
				return 1;
			//Its edges, halfway between the last texel of one level and the
			//first of the other.
			const double ex0 = s.xOf( i0 - 0.5 ), ex1 = s.xOf( i1 - 0.5 ), ey0 = s.yOf( j0 - 0.5 ), ey1 = s.yOf( j1 - 0.5 );

			//Dots at awkward fractions, inside and out. Each lands on one level.
			std::vector< Dot > dots;
			const double fx[] = { -0.41, -0.33, -0.137, -0.04, 0.0213, 0.093, 0.171, 0.37 };
			const double fy[] = { -0.39, -0.14, 0.017, 0.12, 0.36 };
			for( double ax : fx )
				for( double ay : fy )
					dots.push_back( { ax * s.W, ay * s.H } );
			const double sigmaPx = 2.0;
			const Floats card    = dotCard( s, dots, sigmaPx );
			rig.Upload( card );
			const Floats out = rig.Frame();

			int measured = 0, inRecess = 0;
			double worstRef = 0.0, worstForward = 0.0;
			double recordShift = 0.0;
			for( const Dot& d : dots )
			{
				//Where the dot's centre lands: on the front plane at q, or on the
				//floor at S = C + ( q - C )( T + R ) / T.
				const bool floor = d.x > ex0 && d.x < ex1 && d.y > ey0 && d.y < ey1;
				const double zr  = floor ? -R : 0.0;
				V3 S             = { d.x, d.y, 0.0 };
				if( floor && !s.pointed )
					S = s.C + ( V3 { d.x, d.y, 0.0 } - s.C ) * ( ( s.C.z - zr ) / s.C.z );
				//Seen where the ray from the eye through S crosses z = 0.
				const double k = s.V.z / ( s.V.z - zr );
				const double px = s.V.x + ( S.x - s.V.x ) * k, py = s.V.y + ( S.y - s.V.y ) * k;

				//Every pixel of the window must see the same level, lit, with
				//room to spare: the floor's lit and visible part keeps clear of
				//its edges by the runs to the lens and to the eye.
				const int half    = 6;
				const double ci   = s.iOf( px ), cj = s.jOf( py );
				bool safe         = true;
				auto levelOf = [ & ]( double x, double y ) { return x > ex0 && x < ex1 && y > ey0 && y < ey1; };
				std::vector< double > reference;
				for( int j = static_cast< int >( std::lround( cj ) ) - half; j <= std::lround( cj ) + half && safe; ++j )
					for( int i = static_cast< int >( std::lround( ci ) ) - half; i <= std::lround( ci ) + half && safe; ++i )
					{
						if( i < 2 || j < 2 || i >= s.w - 2 || j >= s.h - 2 )
						{
							safe = false;
							break;
						}
						const V3 p    = { s.xOf( i ), s.yOf( j ), 0.0 };
						const V3 hit  = { p.x + ( p.x - s.V.x ) * ( -zr / s.V.z ), p.y + ( p.y - s.V.y ) * ( -zr / s.V.z ), zr };
						const double margin = 3.0 * s.texel() + ( floor ? R * std::max( std::hypot( s.C.x - hit.x, s.C.y - hit.y ) / ( s.C.z + R ),
						                                                                   std::hypot( s.V.x - hit.x, s.V.y - hit.y ) / ( s.V.z + R ) )
						                                                : 0.0 );
						const bool here = levelOf( hit.x, hit.y );
						const bool clear = levelOf( hit.x - margin, hit.y ) == here && levelOf( hit.x + margin, hit.y ) == here
						                   && levelOf( hit.x, hit.y - margin ) == here && levelOf( hit.x, hit.y + margin ) == here;
						if( here != floor || !clear )
						{
							safe = false;
							break;
						}
						const Beam b = beamAt( s, hit, { 0.0, 0.0, 1.0 } );
						reference.push_back( b.g * linearOf( clipCode( card, s.w, s.h, b.qu, b.qv, 1 ) ) );
					}
				if( !safe )
					continue;
				size_t k2 = 0;
				const int i0w = static_cast< int >( std::lround( ci ) ) - half, j0w = static_cast< int >( std::lround( cj ) ) - half;
				double gx = 0.0, gy = 0.0, rx = 0.0, ry = 0.0;
				const bool gotMeasured = centroid( out, s, ci, cj, half, [ & ]( int i, int j ) { return linearOf( pixelAt( out, s.w, i, j )[ 1 ] ); }, gx, gy );
				const bool gotReference = centroid( out, s, ci, cj, half, [ & ]( int i, int j ) {
					return reference[ static_cast< size_t >( ( j - j0w ) * ( 2 * half + 1 ) + ( i - i0w ) ) ];
				}, rx, ry );
				(void)k2;
				if( !gotMeasured || !gotReference )
					continue;
				++measured;
				inRecess += floor;
				worstRef     = std::max( worstRef, std::hypot( gx - rx, gy - ry ) );
				worstForward = std::max( worstForward, std::hypot( gx - ci, gy - cj ) );
				if( floor && std::fabs( d.x - 0.0213 * s.W ) < 1e-9 && std::fabs( d.y - 0.017 * s.H ) < 1e-9 )
					recordShift = ( gx - s.iOf( d.x ) ) * s.texel();
			}
			shiftAt.push_back( recordShift );
			Check( measured >= 12 && inRecess >= 4 && worstRef <= kCentroidPx && worstForward <= 0.05,
			       fmt( "%4dx%-4d viewer %-17s (%+.1f, %+.1f, %.1f) m: %d dots (%d on the floor), each within %.4f px of the closed-form render "
			            "(bound %.2f) and %.4f px of its centre's closed-form landing place",
			            raster.w, raster.h, eye.what, s.V.x, s.V.y, s.V.z, measured, inRecess, worstRef, kCentroidPx, worstForward ) );
		}
		//Displacement on the floor, for one dot: proportional to the viewer's
		//offset (eyes 0 and 2 differ by 1 m in x at the same distance), with
		//slope R / ( Vz + R ), and nothing at the lens.
		const double R = 0.1, want = R / ( 4.0 + R );
		const double slope = shiftAt.size() >= 4 ? ( shiftAt[ 0 ] - shiftAt[ 2 ] ) / 1.0 : 0.0;
		Check( shiftAt.size() >= 4 && std::fabs( slope - want ) <= 0.05 * std::fabs( want ) && std::fabs( shiftAt[ 3 ] ) <= 0.05 * 3.47e-3,
		       fmt( "%4dx%-4d the floor's image moves %.2f mm per metre the viewer moves across (closed form %.2f), and %.3f mm with the viewer at the lens",
		            raster.w, raster.h, 1000.0 * slope, 1000.0 * want, shiftAt.size() >= 4 ? 1000.0 * shiftAt[ 3 ] : 0.0 ) );
	}
	return Verdict();
}

//===========================================================================
// --lambert: tilted planes, every pixel's light in closed form.
//===========================================================================
/// A plane through the texel centres of a 16-bit map, a = a0 + ai i + aj j.
struct PlaneMap
{
	long a0, ai, aj;
};

Words planeGrey( const Scene& s, const PlaneMap& m )
{
	Words grey( static_cast< size_t >( s.w ) * s.h );
	for( int j = 0; j < s.h; ++j )
		for( int i = 0; i < s.w; ++i )
			grey[ static_cast< size_t >( j ) * s.w + i ] = static_cast< uint16_t >( std::clamp( m.a0 + m.ai * i + m.aj * j, 0L, 65535L ) );
	return grey;
}

/// The same plane in metres: h = c0 + sx x + sy y.
struct Plane
{
	double c0, sx, sy;
};
Plane planeOf( const Scene& s, const PlaneMap& m, double R )
{
	const double k = R / 65535.0, t = s.texel();
	Plane p;
	p.sx = k * m.ai / t;
	p.sy = k * m.aj / t;
	p.c0 = -R + k * ( m.a0 + m.ai * ( s.w / 2.0 - 0.5 ) + m.aj * ( s.h / 2.0 - 0.5 ) );
	return p;
}

/// Where the viewer's ray through pixel ( i, j ) meets the plane.
V3 hitPlane( const Scene& s, const Plane& pl, int i, int j )
{
	const double px = s.xOf( i ), py = s.yOf( j );
	const double tx = ( px - s.V.x ) / s.V.z, ty = ( py - s.V.y ) / s.V.z;
	const double z = ( pl.c0 + pl.sx * px + pl.sy * py ) / ( 1.0 + pl.sx * tx + pl.sy * ty );
	return { px - z * tx, py - z * ty, z };
}

int runLambert( const Perturb& perturb )
{
	std::printf( "\n=== lambert: a white clip on a tilted plane is g = d^2 max( n.w, 0 ) / ( r^2 cos^3 a ) at every pixel, from the plane's"
	             " closed-form intersection\n" );
	struct Lamp
	{
		V3 C;
		bool pointed;
		const char* what;
	};
	const Lamp lamps[] = {
		{ { -1.5, 0.3, 2.0 }, false, "left" },
		{ { 2.0, -0.6, 1.5 }, false, "right" },
		{ { 0.1, 1.2, 2.5 }, false, "above" },
		{ { 0.15, 0.05, 1.0 }, false, "close" },
		{ { 0.8, 0.5, 1.6 }, true, "pointed" },
	};
	for( const Raster& raster : kRasters )
	{
		const int w = raster.w, h = raster.h;
		const long kx = 65535 / w, ky = 65535 / h;
		const PlaneMap maps[] = {
			{ 65535, -kx, 0 },
			{ 0, 0, ky / 2 },
			{ 65535 - ( kx / 2 ) * ( w - 1 ), kx / 2, -( ky / 2 ) },
		};
		const char* mapNames[] = { "falling right", "rising up", "diagonal" };
		for( int m = 0; m < 3; ++m )
			for( const Lamp& lamp : lamps )
			{
				Rig rig;
				if( !rig.Init( w, h, nullptr ) )
					return 1;
				rig.plugin.SetHooksForTest( perturb.hooks );
				quietRoom( rig );
				rig.Upload( flatCard( w, h, 1.0, 1.0, 1.0 ) );
				//A small wall and a deep relief, so the plane leans 24 degrees.
				rig.Set( PT_WALL_HEIGHT, ParamFromWallHeight( 0.5 ) );
				rig.Set( PT_DEPTH, ParamFromDepth( 4.0 ) );
				rig.Set( PT_PROJECTOR_X, ParamFromOffset( lamp.C.x ) );
				rig.Set( PT_PROJECTOR_Y, ParamFromOffset( lamp.C.y ) );
				rig.Set( PT_THROW, ParamFromDistance( lamp.C.z ) );
				rig.Option( PT_AIM, lamp.pointed ? Aim::Pointed : Aim::LensShift );
				rig.Set( PT_VIEWER_X, ParamFromOffset( 0.4 ) );
				rig.Set( PT_VIEWER_Y, ParamFromOffset( -0.3 ) );
				rig.Set( PT_VIEWER_DISTANCE, ParamFromDistance( 2.5 ) );
				Scene s = sceneOf( rig );
				const double R = reliefOf( rig );
				const PlaneMap pm = maps[ m ];
				if( pm.a0 + pm.ai * ( w - 1 ) < 0 || pm.a0 + pm.aj * ( h - 1 ) < 0 || pm.a0 + pm.ai * ( w - 1 ) + pm.aj * ( h - 1 ) > 65535 )
				{
					Check( false, "the plane map leaves 0..65535" );
					continue;
				}
				if( !useMap( rig, "plane", planeGrey( s, pm ) ) )
					return 1;
				const Plane pl = planeOf( s, pm, R );
				const V3 n     = unit( { -pl.sx, -pl.sy, 1.0 } );
				const Floats out = rig.Frame();

				int counted = 0;
				double worst = 0.0, used = 0.0, lo = 1e9, hi = 0.0;
				const double slopeUlp = 4.0 * std::ldexp( 1.0, std::ilogb( R ) - 23 ) / ( 2.0 * s.texel() );
				for( int j = 0; j < h; ++j )
					for( int i = 0; i < w; ++i )
					{
						const V3 S = hitPlane( s, pl, i, j );
						//Clear of the frame's edges (where the map clamps) and of
						//the picture's.
						const double edge = 3.0 * s.texel();
						if( std::fabs( S.x ) > 0.5 * s.W - edge || std::fabs( S.y ) > 0.5 * s.H - edge )
							continue;
						const Beam b = beamAt( s, S, n );
						const double mu = 3.0 / w, mv = 3.0 / h;
						if( !b.inside || b.qu < mu || b.qu > 1.0 - mu || b.qv < mv || b.qv > 1.0 - mv || b.facing <= 0.0 )
							continue;
						//What the float heights allow: the slope from two texels'
						//difference, each stored to half an ulp and interpolated to
						//a few more, is good to 4 ulp of R over two texels. At
						//grazing light that moves g by more than kLightRel.
						double spread = 0.0;
						for( double dx : { -slopeUlp, slopeUlp } )
							for( double dy : { -slopeUlp, slopeUlp } )
								spread = std::max( spread, std::fabs( beamAt( s, S, unit( { -pl.sx - dx, -pl.sy - dy, 1.0 } ) ).g - b.g ) );
						const double got = linearOf( pixelAt( out, w, i, j )[ 1 ] );
						worst            = std::max( worst, std::fabs( got - b.g ) / b.g );
						used             = std::max( used, std::fabs( got - b.g ) / ( kLightRel * b.g + spread ) );
						lo               = std::min( lo, b.g );
						hi               = std::max( hi, b.g );
						++counted;
					}
				Check( counted > w * h / 8 && used <= 1.0,
				       fmt( "%4dx%-4d plane %-13s lens %-7s (%+.2f, %+.2f, %.2f) m: %6d pixels, g %.3f..%.3f, worst %.1e relative, %.2f of the"
				            " bound",
				            w, h, mapNames[ m ], lamp.what, s.C.x, s.C.y, s.C.z, counted, lo, hi, worst, used ) );
			}
	}
	return Verdict();
}

//===========================================================================
// Reference march in double over a 16-bit map's bilinear surface: fine steps,
// then bisection. Independent of the plugin's: a hundred steps a texel.
//===========================================================================
struct MapField
{
	const Scene* s;
	const Words* grey;
	double R;
	double at( double x, double y ) const
	{
		const double fi = std::clamp( s->iOf( x ), 0.0, s->w - 1.0 ), fj = std::clamp( s->jOf( y ), 0.0, s->h - 1.0 );
		const int i0 = std::min( static_cast< int >( fi ), s->w - 2 ), j0 = std::min( static_cast< int >( fj ), s->h - 2 );
		const double a = fi - i0, b = fj - j0;
		auto g = [ & ]( int i, int j ) { return -R * ( 1.0 - ( *grey )[ static_cast< size_t >( j ) * s->w + i ] / 65535.0 ); };
		return ( 1 - b ) * ( ( 1 - a ) * g( i0, j0 ) + a * g( i0 + 1, j0 ) ) + b * ( ( 1 - a ) * g( i0, j0 + 1 ) + a * g( i0 + 1, j0 + 1 ) );
	}
};

V3 marchView( const MapField& f, double px, double py )
{
	const Scene& s  = *f.s;
	const double tx = ( px - s.V.x ) / s.V.z, ty = ( py - s.V.y ) / s.V.z;
	auto F          = [ & ]( double z ) { return z - f.at( px - z * tx, py - z * ty ); };
	if( F( 0.0 ) <= 0.0 )
		return { px, py, 0.0 };
	const int steps = std::max( 64, static_cast< int >( 100.0 * f.R * std::hypot( tx, ty ) / s.texel() ) );
	double za = 0.0, zb = -f.R;
	for( int k = 1; k <= steps; ++k )
	{
		const double z = -f.R * k / steps;
		if( F( z ) <= 0.0 )
		{
			zb = z;
			break;
		}
		za = z;
	}
	for( int k = 0; k < 60; ++k )
	{
		const double zm = 0.5 * ( za + zb );
		( F( zm ) <= 0.0 ? zb : za ) = zm;
	}
	const double z = 0.5 * ( za + zb );
	return { px - z * tx, py - z * ty, z };
}

//===========================================================================
// --away: faces turned from the lens get the room's light and nothing else.
//===========================================================================
int runAway( const Perturb& perturb )
{
	std::printf( "\n=== away: a sawtooth whose ramps face away from the lens; on them the clip changes nothing, and the picture is the"
	             " room's light alone, which the cliffs partly hide\n" );
	for( const Raster& raster : kRasters )
	{
		const int w = raster.w, h = raster.h;
		Rig rig;
		if( !rig.Init( w, h ) )
			return 1;
		rig.plugin.SetHooksForTest( perturb.hooks );
		quietRoom( rig );
		rig.Set( PT_AMBIENT, ParamFromAmbient( 50.0 ) );
		rig.Set( PT_WALL_HEIGHT, ParamFromWallHeight( 0.25 ) );
		rig.Set( PT_DEPTH, ParamFromDepth( 1.0 ) );
		rig.Set( PT_PROJECTOR_X, ParamFromOffset( -1.0 ) );
		rig.Set( PT_PROJECTOR_Y, ParamFromOffset( 0.0 ) );
		rig.Set( PT_THROW, ParamFromDistance( 2.0 ) );
		rig.Set( PT_VIEWER_X, ParamFromOffset( 0.3 ) );
		rig.Set( PT_VIEWER_Y, ParamFromOffset( 0.0 ) );
		rig.Set( PT_VIEWER_DISTANCE, ParamFromDistance( 3.0 ) );
		Scene s        = sceneOf( rig );
		const double R = reliefOf( rig );
		//Teeth 25 mm wide (whole texels), each falling to the right, so each
		//ramp faces +x and the lens is off to -x.
		const int P = std::max( 8, static_cast< int >( std::lround( 0.025 / s.texel() ) ) );
		const long k = 65535 / ( P - 1 );
		Words grey( static_cast< size_t >( w ) * h );
		for( int j = 0; j < h; ++j )
			for( int i = 0; i < w; ++i )
				grey[ static_cast< size_t >( j ) * w + i ] = static_cast< uint16_t >( 65535 - k * ( i % P ) );
		if( !useMap( rig, "sawtooth", grey ) )
			return 1;
		const MapField field { &s, &grey, R };
		const double A = AmbientFromParam( rig.Get( PT_AMBIENT ) ) * s.W * s.H / LumensFromParam( rig.Get( PT_LUMENS ) );

		rig.Upload( flatCard( w, h, 1.0, 1.0, 1.0 ) );
		const Floats white = rig.Frame();
		rig.Upload( buildCard( w, h ) );
		const Floats card = rig.Frame();
		rig.Upload( flatCard( w, h, 0.0, 0.0, 0.0 ) );
		const Floats dark = rig.Frame();

		const double slope = R * k / 65535.0 / s.texel();
		const V3 n         = unit( { slope, 0.0, 1.0 } );
		int away = 0, changed = 0, picked = 0;
		double drift = 0.0;
		double lo = 1e9, hi = 0.0;
		for( int j = 4; j < h - 4; j += 2 )
			for( int i = 4; i < w - 4; ++i )
			{
				//A pixel centred on a crest is a knife edge: the plugin's ray
				//starts on the crest (h = 0) and takes its normal across the
				//cliff, the reference's sees h = -1e-17 and falls past it. Either
				//is the edge; neither is a ramp.
				if( i % P == 0 )
					continue;
				const V3 S = marchView( field, s.xOf( i ), s.yOf( j ) );
				//On a ramp, a texel and a half clear of every cliff.
				const double along = std::fmod( s.iOf( S.x ) + 0.5, static_cast< double >( P ) );
				if( along < 1.5 || along > P - 2.5 )
					continue;
				++picked;
				const Beam b = beamAt( s, S, n );
				if( !b.inside || b.facing >= 0.0 )
					continue;
				++away;
				const float* a = pixelAt( white, w, i, j );
				const float* c = pixelAt( card, w, i, j );
				const float* d = pixelAt( dark, w, i, j );
				if( !sameValue( a[ 0 ], c[ 0 ], drift ) || !sameValue( a[ 1 ], c[ 1 ], drift ) || !sameValue( a[ 2 ], c[ 2 ], drift )
				    || !sameValue( a[ 1 ], d[ 1 ], drift ) )
					++changed;
				//The room's light, as much as the cliffs leave: above nothing,
				//and no more than all of it.
				lo = std::min( lo, linearOf( a[ 1 ] ) );
				hi = std::max( hi, linearOf( a[ 1 ] ) );
			}
		Check( away > picked / 2 && changed == 0 && lo > 0.0 && hi <= A * ( 1.0 + kLightRel ),
		       fmt( "%4dx%-4d ramps %.0f degrees from the wall, the lens %.0f degrees off it: %d pixels face away, %d of them change "
		            "with the clip (white, the card, black: largest change %.1e, bound %.0e); they hold %.2f to %.2f of the room's %.5f",
		            w, h, std::atan( slope ) * 180.0 / kPi, std::atan( s.C.z / ( 0.0 - s.C.x ) ) * 180.0 / kPi, away, changed, drift, kRepeat,
		            lo / A, hi / A, A ) );
	}
	return Verdict();
}

//===========================================================================
// --shadow: a step, its edge's shadow on the floor.
//===========================================================================
int runShadow( const Perturb& perturb )
{
	std::printf( "\n=== shadow: a step down of R, the lens off to the left; the edge's shadow on the floor is R ( x_e - C_x ) / T long\n" );
	for( const Raster& raster : kRasters )
		for( double depth : { 1.0, 2.0 } )
		{
			const int w = raster.w, h = raster.h;
			Rig rig;
			if( !rig.Init( w, h ) )
				return 1;
			rig.plugin.SetHooksForTest( perturb.hooks );
			quietRoom( rig );
			rig.Set( PT_AMBIENT, ParamFromAmbient( 20.0 ) );
			rig.Set( PT_DEPTH, ParamFromDepth( depth ) );
			rig.Set( PT_PROJECTOR_X, ParamFromOffset( -3.0 ) );
			rig.Set( PT_PROJECTOR_Y, ParamFromOffset( 0.0 ) );
			rig.Set( PT_THROW, ParamFromDistance( 3.0 ) );
			rig.Set( PT_VIEWER_X, ParamFromOffset( 4.0 ) );
			rig.Set( PT_VIEWER_Y, ParamFromOffset( 0.0 ) );
			rig.Set( PT_VIEWER_DISTANCE, ParamFromDistance( 5.0 ) );
			Scene s        = sceneOf( rig );
			const double R = reliefOf( rig );
			//Front plane left of x_e, the floor right of it.
			const int ie = w / 3;
			Words grey( static_cast< size_t >( w ) * h, 65535 );
			for( int j = 0; j < h; ++j )
				for( int i = ie; i < w; ++i )
					grey[ static_cast< size_t >( j ) * w + i ] = 0;
			if( !useMap( rig, "step", grey ) )
				return 1;
			//The bilinear surface falls from the last front texel's centre to
			//the first floor texel's: the edge that casts is the top of that
			//ramp, and the floor begins at its foot.
			const double xe = s.xOf( ie - 1 ), foot = s.xOf( ie );
			rig.Upload( flatCard( w, h, 1.0, 1.0, 1.0 ) );
			const Floats lit = rig.Frame();
			rig.Upload( buildCard( w, h ) );
			const Floats card = rig.Frame();

			//The shadow's far end on the floor, and where the viewer sees it.
			const double L   = R * ( xe - s.C.x ) / s.C.z;
			const double end = s.V.x + ( xe + L - s.V.x ) * s.V.z / ( s.V.z + R );
			const double want = s.iOf( end );
			//The first pixel whose ray reaches the floor a texel past the foot,
			//clear of the ramp the normal's differences straddle.
			const int floorStart = static_cast< int >( std::ceil( s.iOf( s.V.x + ( foot + s.texel() - s.V.x ) * s.V.z / ( s.V.z + R ) ) ) );
			//Along each row, the first pixel right of the edge that is lit:
			//brighter than half of the floor's light.
			const double floorLight = s.C.z * s.C.z / ( ( s.C.z + R ) * ( s.C.z + R ) );
			double worst = 0.0;
			int rows = 0, moved = 0, band = 0;
			double drift = 0.0;
			for( int j = h / 4; j < 3 * h / 4; ++j )
			{
				int first = -1;
				for( int i = ie; i < w; ++i )
					if( linearOf( pixelAt( lit, w, i, j )[ 1 ] ) > 0.5 * floorLight )
					{
						first = i;
						break;
					}
				if( first < 0 )
					continue;
				++rows;
				//The edge is between first - 1 and first.
				worst = std::max( worst, std::fabs( ( first - 0.5 ) - want ) );
				for( int i = floorStart; i < first - 2; ++i )
				{
					++band;
					if( !sameValue( pixelAt( lit, w, i, j )[ 1 ], pixelAt( card, w, i, j )[ 1 ], drift ) )
						++moved;
				}
			}
			Check( rows == h / 2 && worst <= 1.0 && band > 0 && moved == 0,
			       fmt( "%4dx%-4d R %.0f mm: the shadow is %.1f mm long on the floor, its end seen at pixel %.2f; every row's within %.2f px "
			            "(bound 1: the shadow is all or nothing at a pixel's centre); %d of %d shadowed floor pixels change with the clip"
			            " (largest change %.1e, bound %.0e)",
			            w, h, 1000.0 * R, 1000.0 * L, want, worst, moved, band, drift, kRepeat ) );
		}
	return Verdict();
}

//===========================================================================
// --atlens: from the lens, nothing is in shadow.
//===========================================================================
int runAtLens( const Perturb& perturb )
{
	std::printf( "\n=== atlens: with the viewer at the lens, every point it sees is lit -- on every surface, at its real depth and at four"
	             " times it\n" );
	const Surface kinds[] = { Surface::Paper, Surface::Brick, Surface::Corrugated, Surface::Plate, Surface::Drapes, Surface::Weatherboard, Surface::Tiles };
	const char* names[]   = { "Paper", "Brick", "Corrugated", "Plate", "Drapes", "Weatherboard", "Tiles" };
	for( const Raster& raster : kRasters )
		for( double depth : { 1.0, 4.0 } )
		for( int k = 0; k < 7; ++k )
		{
			Rig rig;
			if( !rig.Init( raster.w, raster.h ) )
				return 1;
			rig.plugin.SetHooksForTest( perturb.hooks );
			rig.Option( PT_SURFACE, kinds[ k ] );
			rig.Set( PT_DEPTH, ParamFromDepth( depth ) );
			rig.Set( PT_CRUMPLE, 1.0 );
			rig.Set( PT_PROJECTOR_X, ParamFromOffset( -2.5 ) );
			rig.Set( PT_PROJECTOR_Y, ParamFromOffset( 1.5 ) );
			rig.Set( PT_THROW, ParamFromDistance( 3.0 ) );
			rig.Set( PT_VIEWER_X, rig.Get( PT_PROJECTOR_X ) );
			rig.Set( PT_VIEWER_Y, rig.Get( PT_PROJECTOR_Y ) );
			rig.Set( PT_VIEWER_DISTANCE, rig.Get( PT_THROW ) );
			rig.Option( PT_VIEW, View::Shadow );
			const Floats out = rig.Frame();
			int dark = 0, away = 0;
			for( size_t i = 0; i < out.size(); i += 4 )
			{
				dark += out[ i + 1 ] == 0.0f;
				away += out[ i + 1 ] == 0.5f;
			}
			const double total = static_cast< double >( out.size() / 4 );
			//The viewer's march steps half a texel; a convex corner its ray
			//clips by less than a step is stepped over, and the shadow march,
			//stepping from the other end, can find it: a dark pixel. Measured
			//on an M4 Max: 4 in 921,600 at the real depth (brick's arrises),
			//435 at four times it. The bounds are those rates rounded up to a
			//power of ten. A face turned from the eye it is seen by is a
			//rounding at an edge seen exactly edge-on.
			const double rate = depth > 1.0 ? 1e-3 : 1e-5;
			Check( dark <= total * rate && away <= total * 1e-5,
			       fmt( "%4dx%-4d %-12s at Depth %.0f: %d pixels in shadow, %d turned away, of %.0f", raster.w, raster.h, names[ k ], depth, dark,
			            away, total ) );
		}
	return Verdict();
}

//===========================================================================
// --hotspot: the lens's highlight on flat metal.
//===========================================================================
/// GGX, height-correlated Smith, Schlick: the specular term out( white ) -
/// albedo, at a point of the flat front plane, relative to a white screen.
double specularAt( const Scene& s, V3 S, double alpha, double f0 )
{
	const V3 n = { 0.0, 0.0, 1.0 };
	const Beam b = beamAt( s, S, n );
	if( !b.inside || b.facing <= 0.0 )
		return 0.0;
	const V3 v      = unit( s.V - S );
	const V3 hv     = unit( b.w + v );
	const double nl = b.facing, nv = std::max( v.z, 1e-4 ), nh = hv.z, vh = std::clamp( dot( v, hv ), 0.0, 1.0 );
	const double a2 = alpha * alpha;
	const double dd = nh * nh * ( a2 - 1.0 ) + 1.0;
	const double D  = a2 / ( kPi * dd * dd );
	const double Vis = 0.5 / ( nl * std::sqrt( nv * nv * ( 1.0 - a2 ) + a2 ) + nv * std::sqrt( nl * nl * ( 1.0 - a2 ) + a2 ) );
	const double F   = f0 + ( 1.0 - f0 ) * std::pow( 1.0 - vh, 5.0 );
	return kPi * D * Vis * F * b.g;
}

int runHotspot( const Perturb& perturb )
{
	std::printf( "\n=== hotspot: flat zinc throws back a highlight of the lens; with lens and viewer mirror-symmetric about M its centroid is M,"
	             " and every pixel of it is GGX with zinc's F0\n" );
	for( const Raster& raster : kRasters )
	{
		const int w = raster.w, h = raster.h;
		//M on a pixel corner, a whole number of pixels from the centre at both
		//rasters, so a window symmetric about it is a whole number of pixels.
		const double fracs[][ 2 ] = { { 0.0, 0.0 }, { 3.0 / 32.0, -2.0 / 18.0 } };
		for( const auto& frac : fracs )
		{
			Rig rig;
			if( !rig.Init( w, h ) )
				return 1;
			rig.plugin.SetHooksForTest( perturb.hooks );
			quietRoom( rig );
			rig.Upload( flatCard( w, h, 1.0, 1.0, 1.0 ) );
			rig.Option( PT_SURFACE, Surface::Corrugated );
			rig.Set( PT_DEPTH, 0.0 );
			Scene s0       = sceneOf( rig );
			const double Mx = frac[ 0 ] * s0.W, My = frac[ 1 ] * s0.H;
			const double ax = 0.9, ay = 0.5, T = 5.0;
			rig.Set( PT_PROJECTOR_X, ParamFromOffset( Mx + ax ) );
			rig.Set( PT_PROJECTOR_Y, ParamFromOffset( My + ay ) );
			rig.Set( PT_THROW, ParamFromDistance( T ) );
			rig.Set( PT_VIEWER_X, ParamFromOffset( Mx - ax ) );
			rig.Set( PT_VIEWER_Y, ParamFromOffset( My - ay ) );
			rig.Set( PT_VIEWER_DISTANCE, ParamFromDistance( T ) );
			Scene s = sceneOf( rig );
			//The mirror image of the controls' float rounding: where the
			//symmetry actually is.
			const double mx = 0.5 * ( s.C.x + s.V.x ), my = 0.5 * ( s.C.y + s.V.y );
			const Floats out = rig.Frame();
			rig.Option( PT_VIEW, View::Albedo );
			const Floats albedo = rig.Frame();

			const double alpha = std::pow( RoughnessFromParam( rig.Get( PT_ROUGHNESS ) ), 2.0 );
			const double f0g   = facts::kZincF0[ 1 ];
			const int ci = static_cast< int >( std::lround( s.iOf( Mx ) + 0.5 ) ), cj = static_cast< int >( std::lround( s.jOf( My ) + 0.5 ) );
			const int hx = std::min( ci, w - ci ) - 2, hy = std::min( cj, h - cj ) - 2;
			double sw = 0.0, sx = 0.0, sy = 0.0, worst = 0.0, peak = 0.0;
			for( int j = cj - hy; j < cj + hy; ++j )
				for( int i = ci - hx; i < ci + hx; ++i )
				{
					const double spec = linearOf( pixelAt( out, w, i, j )[ 1 ] ) - linearOf( pixelAt( albedo, w, i, j )[ 1 ] );
					sw += spec;
					sx += spec * i;
					sy += spec * j;
					const double want = specularAt( s, { s.xOf( i ), s.yOf( j ), 0.0 }, alpha, f0g );
					worst             = std::max( worst, std::fabs( spec - want ) / std::max( want, 1e-3 ) );
					peak              = std::max( peak, want );
				}
			const double gx = sx / sw - s.iOf( mx ), gy = sy / sw - s.jOf( my );
			Check( std::hypot( gx, gy ) <= 0.05 && worst <= 3.0 * kLightRel,
			       fmt( "%4dx%-4d M (%+.3f, %+.3f) m: the highlight's centroid is %.4f px from M (bound 0.05) over a %dx%d window; peak %.2f x white,"
			            " every pixel GGX within %.1e relative",
			            w, h, mx, my, std::hypot( gx, gy ), 2 * hx, 2 * hy, peak, worst ) );
		}
		//Matte paper has no highlight: flat paper is the same from anywhere.
		Rig rig;
		if( !rig.Init( w, h ) )
			return 1;
		rig.plugin.SetHooksForTest( perturb.hooks );
		rig.Option( PT_SURFACE, Surface::Paper );
		rig.Set( PT_CRUMPLE, 0.0 );
		std::vector< Floats > views;
		for( const V3& V : { V3 { 2.0, -0.8, 4.0 }, V3 { -6.0, 3.0, 1.5 }, V3 { 0.0, 1.0, 6.0 } } )
		{
			rig.Set( PT_VIEWER_X, ParamFromOffset( V.x ) );
			rig.Set( PT_VIEWER_Y, ParamFromOffset( V.y ) );
			rig.Set( PT_VIEWER_DISTANCE, ParamFromDistance( V.z ) );
			views.push_back( rig.Frame() );
		}
		Check( views[ 0 ] == views[ 1 ] && views[ 1 ] == views[ 2 ],
		       fmt( "%4dx%-4d flat paper from three places: %s", w, h, views[ 0 ] == views[ 1 ] && views[ 1 ] == views[ 2 ] ? "bit for bit the same" : "DIFFERENT" ) );
	}
	return Verdict();
}

//===========================================================================
// --ambient: the room's light, in closed form.
//===========================================================================
int runAmbient( const Perturb& perturb )
{
	std::printf( "\n=== ambient: black comes back as b + A and white as 1 + A, A = lux x W x H / lumens; Exposure is x 2^stops; any plane"
	             " gets all of the room's light\n" );
	struct Case
	{
		int w, h;
		double lux, lumens, wall, black, stops;
	};
	const Case cases[] = {
		{ 1280, 720, 20.0, 10000.0, 2.5, 0.0005, 0.0 }, { 640, 480, 300.0, 2000.0, 4.0, 0.0, 0.0 }, { 256, 256, 5.0, 40000.0, 12.0, 0.01, 0.0 },
		{ 320, 180, 150.0, 5000.0, 1.0, 0.002, 1.5 },   { 640, 480, 800.0, 600.0, 0.4, 0.0, -2.0 },
	};
	for( const Case& c : cases )
	{
		Rig rig;
		if( !rig.Init( c.w, c.h ) )
			return 1;
		rig.plugin.SetHooksForTest( perturb.hooks );
		rig.Option( PT_SURFACE, Surface::Flat );
		rig.Set( PT_AMBIENT, ParamFromAmbient( c.lux ) );
		rig.Set( PT_LUMENS, ParamFromLumens( c.lumens ) );
		rig.Set( PT_WALL_HEIGHT, ParamFromWallHeight( c.wall ) );
		rig.Set( PT_BLACK_LEVEL, ParamFromBlackLevel( c.black ) );
		rig.Set( PT_EXPOSURE, ParamFromExposure( std::pow( 2.0, c.stops ) ) );
		const double H    = WallHeightFromParam( rig.Get( PT_WALL_HEIGHT ) );
		const double W    = H * c.w / c.h;
		const double A    = AmbientFromParam( rig.Get( PT_AMBIENT ) ) * W * H / LumensFromParam( rig.Get( PT_LUMENS ) );
		const double b    = BlackLevelFromParam( rig.Get( PT_BLACK_LEVEL ) );
		const double gain = ExposureFromParam( rig.Get( PT_EXPOSURE ) );
		rig.Upload( flatCard( c.w, c.h, 0.0, 0.0, 0.0 ) );
		const Floats dark = rig.Frame();
		rig.Upload( flatCard( c.w, c.h, 1.0, 1.0, 1.0 ) );
		const Floats white = rig.Frame();
		double worst = 0.0;
		for( size_t i = 0; i < dark.size(); i += 4 )
		{
			worst = std::max( worst, std::fabs( linearOf( dark[ i + 1 ] ) - gain * ( b + A ) ) / ( gain * ( b + A ) ) );
			worst = std::max( worst, std::fabs( linearOf( white[ i + 1 ] ) - gain * ( 1.0 + A ) ) / ( gain * ( 1.0 + A ) ) );
		}
		Check( worst <= kLightRel, fmt( "%4dx%-4d %4.0f lux, %5.0f lm, a %4.1f m wall, black %.4f, %+.1f stops: A = %.5f, black %.5f and white %.4f"
		                                " within %.1e relative",
		                                c.w, c.h, c.lux, c.lumens, c.wall, c.black, c.stops, A, gain * ( b + A ), gain * ( 1.0 + A ), worst ) );
	}
	//A tilted plane under a black clip and an ideal projector: the room alone,
	//unoccluded. Occlusion measured against the tangent plane sees no horizon.
	for( const Raster& raster : kRasters )
	{
		Rig rig;
		if( !rig.Init( raster.w, raster.h ) )
			return 1;
		rig.plugin.SetHooksForTest( perturb.hooks );
		rig.Set( PT_BLACK_LEVEL, 0.0 );
		rig.Set( PT_AMBIENT, ParamFromAmbient( 100.0 ) );
		rig.Set( PT_WALL_HEIGHT, ParamFromWallHeight( 0.5 ) );
		rig.Set( PT_DEPTH, ParamFromDepth( 4.0 ) );
		Scene s = sceneOf( rig );
		const long kx = 65535 / raster.w, ky = 65535 / raster.h;
		const PlaneMap pm = { 65535 - ( kx / 2 ) * ( raster.w - 1 ), kx / 2, -( ky / 2 ) };
		if( !useMap( rig, "tilt", planeGrey( s, pm ) ) )
			return 1;
		const Plane pl = planeOf( s, pm, reliefOf( rig ) );
		const double A = AmbientFromParam( rig.Get( PT_AMBIENT ) ) * s.W * s.H / LumensFromParam( rig.Get( PT_LUMENS ) );
		rig.Upload( flatCard( raster.w, raster.h, 0.0, 0.0, 0.0 ) );
		const Floats out = rig.Frame();
		//Clear of the frame's edges by the central difference and the hit's
		//bilinear: the buffer holds its edge, and a slope taken across it
		//is not the plane's.
		const double clear = 3.0 * s.texel();
		double worst       = 0.0;
		int counted        = 0;
		for( int j = 0; j < raster.h; ++j )
			for( int i = 0; i < raster.w; ++i )
			{
				const V3 S = hitPlane( s, pl, i, j );
				if( std::fabs( S.x ) > 0.5 * s.W - clear || std::fabs( S.y ) > 0.5 * s.H - clear )
					continue;
				++counted;
				worst = std::max( worst, std::fabs( linearOf( pixelAt( out, raster.w, i, j )[ 1 ] ) - A ) / A );
			}
		Check( counted > raster.w * raster.h / 4 && worst <= kLightRel,
		       fmt( "%4dx%-4d a plane leaning %.0f degrees: the room's %.5f at %d pixels within %.1e relative", raster.w, raster.h,
		            std::atan( std::hypot( pl.sx, pl.sy ) ) * 180.0 / kPi, A, counted, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --keystone: Pointed from off-centre.
//===========================================================================
int runKeystone( const Perturb& perturb )
{
	std::printf( "\n=== keystone: Pointed from off-centre; dots land where the pinhole puts them, and white is g, near side and far\n" );
	for( const Raster& raster : kRasters )
	{
		const int w = raster.w, h = raster.h;
		Rig rig;
		if( !rig.Init( w, h ) )
			return 1;
		rig.plugin.SetHooksForTest( perturb.hooks );
		quietRoom( rig );
		rig.Option( PT_SURFACE, Surface::Flat );
		rig.Option( PT_AIM, Aim::Pointed );
		rig.Set( PT_PROJECTOR_X, ParamFromOffset( 2.5 ) );
		rig.Set( PT_PROJECTOR_Y, ParamFromOffset( 1.2 ) );
		rig.Set( PT_THROW, ParamFromDistance( 5.0 ) );
		Scene s = sceneOf( rig );

		//Dots in the clip, in its own frame metres.
		std::vector< Dot > dots;
		for( double fx : { -0.3, -0.1, 0.12, 0.31 } )
			for( double fy : { -0.28, 0.04, 0.27 } )
				dots.push_back( { fx * s.W, fy * s.H } );
		rig.Upload( dotCard( s, dots, 2.0 ) );
		const Floats out = rig.Frame();
		double worstDot  = 0.0;
		int found        = 0;
		for( const Dot& d : dots )
		{
			//The clip point on the design screen, and its ray to the wall.
			const V3 P = s.C + s.axis * s.d + s.right * d.x + s.up * d.y;
			const double t = s.C.z / ( s.C.z - P.z );
			const V3 Q = s.C + ( P - s.C ) * t;
			double gx = 0.0, gy = 0.0;
			if( !centroid( out, s, s.iOf( Q.x ), s.jOf( Q.y ), 8, [ & ]( int i, int j ) { return linearOf( pixelAt( out, w, i, j )[ 1 ] ); }, gx, gy ) )
				continue;
			++found;
			worstDot = std::max( worstDot, std::hypot( gx - s.iOf( Q.x ), gy - s.jOf( Q.y ) ) );
		}
		rig.Upload( flatCard( w, h, 1.0, 1.0, 1.0 ) );
		const Floats white = rig.Frame();
		double worst = 0.0, lo = 1e9, hi = 0.0;
		int counted = 0;
		for( int j = 0; j < h; ++j )
			for( int i = 0; i < w; ++i )
			{
				const Beam b = beamAt( s, { s.xOf( i ), s.yOf( j ), 0.0 }, { 0.0, 0.0, 1.0 } );
				if( !b.inside || b.qu < 2.0 / w || b.qu > 1.0 - 2.0 / w || b.qv < 2.0 / h || b.qv > 1.0 - 2.0 / h )
					continue;
				++counted;
				worst = std::max( worst, std::fabs( linearOf( pixelAt( white, w, i, j )[ 1 ] ) - b.g ) / b.g );
				lo    = std::min( lo, b.g );
				hi    = std::max( hi, b.g );
			}
		Check( found == static_cast< int >( dots.size() ) && worstDot <= 0.1,
		       fmt( "%4dx%-4d lens (%+.1f, %+.1f, %.1f) m pointed at the centre: %d dots, each within %.3f px of the pinhole's place (bound 0.1)", w, h,
		            s.C.x, s.C.y, s.C.z, found, worstDot ) );
		Check( counted > w * h / 4 && worst <= kLightRel,
		       fmt( "%4dx%-4d white on the keystoned picture: g from %.3f (far side) to %.3f (near), %d pixels within %.1e relative", w, h, lo, hi,
		            counted, worst ) );
	}
	return Verdict();
}

//===========================================================================
// --surfaces: the procedural surfaces' sizes, off the surface buffer.
//===========================================================================
/// The centres of the runs where `inside` holds along a line of samples.
std::vector< double > runCentres( const std::vector< bool >& inside )
{
	std::vector< double > centres;
	for( size_t i = 0; i < inside.size(); )
	{
		if( !inside[ i ] )
		{
			++i;
			continue;
		}
		size_t e = i;
		while( e < inside.size() && inside[ e ] )
			++e;
		if( i > 0 && e < inside.size() )//whole runs only
			centres.push_back( 0.5 * ( i + e - 1 ) );
		i = e;
	}
	return centres;
}

/// Least-squares spacing of evenly spaced positions.
double spacingOf( const std::vector< double >& at )
{
	const size_t n = at.size();
	if( n < 2 )
		return 0.0;
	double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
	for( size_t k = 0; k < n; ++k )
	{
		sx += k;
		sy += at[ k ];
		sxx += static_cast< double >( k ) * k;
		sxy += k * at[ k ];
	}
	return ( n * sxy - sx * sy ) / ( n * sxx - sx * sx );
}

int runSurfaces( const Perturb& perturb )
{
	std::printf( "\n=== surfaces: every procedural surface at its stated size, read off the surface buffer in metres\n" );
	for( const Raster& raster : kRasters )
	{
		const int w = raster.w, h = raster.h;
		auto fresh = [ & ]( Rig& rig, Surface kind ) {
			if( !rig.Init( w, h ) )
				return false;
			rig.plugin.SetHooksForTest( perturb.hooks );
			rig.Option( PT_SURFACE, kind );
			//A texel of 2.5 mm at either raster: the joints resolve.
			rig.Set( PT_WALL_HEIGHT, ParamFromWallHeight( 0.0025 * h ) );
			return rig.Render( 1 );
		};
		const double texel = 0.0025;

		//Brick: course and head-joint spacing, half-bond offset, joint width.
		for( double mortar : { 0.010, 0.020 } )
		{
			Rig rig;
			if( !fresh( rig, Surface::Brick ) )
				return 1;
			rig.Set( PT_MORTAR, ParamFromMortar( mortar ) );
			rig.Render( 1 );
			const Floats sf    = rig.Surface();
			const double R     = rig.plugin.RoomForTest().relief;
			const double mt    = MortarFromParam( rig.Get( PT_MORTAR ) );
			auto joint         = [ & ]( int i, int j ) { return sf[ ( static_cast< size_t >( j ) * w + i ) * 4 + 3 ] < -0.5 * R; };
			std::vector< bool > column( static_cast< size_t >( h ) );
			const int ci = w / 2 + 7;
			for( int j = 0; j < h; ++j )
				column[ static_cast< size_t >( j ) ] = joint( ci, j );
			std::vector< double > beds = runCentres( column );
			const double course        = spacingOf( beds ) * texel;
			//Each whole bed joint's width where the height crosses three
			//quarters of the way down: below the deepest arris (the brick's
			//set-back and a 2 mm arris reach 4.3 mm of the 8), above the
			//mortar. The crossing interpolated between texel centres; the
			//median, so a run that meets a head joint is not the answer.
			std::vector< double > widths;
			{
				auto at       = [ & ]( int j ) { return static_cast< double >( sf[ ( static_cast< size_t >( j ) * w + ci ) * 4 + 3 ] ); };
				const double level = -0.75 * R;
				double start = -1.0;
				for( int j = 1; j < h; ++j )
				{
					const double a = at( j - 1 ), b = at( j );
					if( a >= level && b < level )
						start = j - 1 + ( a - level ) / ( a - b );
					else if( a < level && b >= level && start >= 0.0 )
						widths.push_back( ( j - 1 + ( level - a ) / ( b - a ) - start ) * texel );
				}
				std::sort( widths.begin(), widths.end() );
			}
			const double width = widths.empty() ? 0.0 : widths[ widths.size() / 2 ];
			//Head joints along the middle of two consecutive courses.
			const double bed0 = beds.size() > 3 ? beds[ beds.size() / 2 ] : 0.0;
			auto heads = [ & ]( double row ) {
				std::vector< bool > line( static_cast< size_t >( w ) );
				for( int i = 0; i < w; ++i )
					line[ static_cast< size_t >( i ) ] = joint( i, static_cast< int >( std::lround( row ) ) );
				return runCentres( line );
			};
			const std::vector< double > a = heads( bed0 + 0.5 * course / texel ), b = heads( bed0 + 1.5 * course / texel );
			const double unit  = spacingOf( a ) * texel;
			double offset      = 0.0;
			if( !a.empty() && !b.empty() )
			{
				//b's nearest head joint to a's first, folded into one unit.
				double best = 1e9;
				for( double x : b )
					best = std::min( best, std::fabs( x - a.front() ) );
				offset = best * texel;
			}
			const double wantCourse = stated::kBrickHeight + mt, wantUnit = stated::kBrickLength + mt;
			Check( std::fabs( course - wantCourse ) <= 0.25 * texel && std::fabs( unit - wantUnit ) <= 0.25 * texel
			           && std::fabs( offset - 0.5 * wantUnit ) <= 1.0 * texel && std::fabs( width - mt ) <= 1.0 * texel,
			       fmt( "%4dx%-4d Brick, Stretcher, Mortar %.0f mm: courses %.2f mm apart (%.2f), bricks %.2f mm (%.2f), the next course %.2f mm "
			            "along (%.2f), joints %.1f mm wide",
			            w, h, 1000.0 * mt, 1000.0 * course, 1000.0 * wantCourse, 1000.0 * unit, 1000.0 * wantUnit, 1000.0 * offset,
			            500.0 * wantUnit, 1000.0 * width ) );
		}
		//English: a course of headers, 112.5 mm apart. Flemish: alternate
		//stretcher and header in one course, so the joints' gaps alternate.
		{
			for( Bond bond : { Bond::English, Bond::Flemish } )
			{
				Rig rig;
				if( !fresh( rig, Surface::Brick ) )
					return 1;
				rig.Option( PT_BOND, bond );
				rig.Render( 1 );
				const Floats sf = rig.Surface();
				const double R  = rig.plugin.RoomForTest().relief;
				const double mt = MortarFromParam( rig.Get( PT_MORTAR ) );
				auto joint      = [ & ]( int i, int j ) { return sf[ ( static_cast< size_t >( j ) * w + i ) * 4 + 3 ] < -0.5 * R; };
				const double course = stated::kBrickHeight + mt;
				std::vector< std::vector< double > > rows;
				for( int c = 2; c < 6; ++c )
				{
					std::vector< bool > line( static_cast< size_t >( w ) );
					const int j = static_cast< int >( std::lround( ( c + 0.5 ) * course / texel - 0.5 ) );
					for( int i = 0; i < w; ++i )
						line[ static_cast< size_t >( i ) ] = joint( i, j );
					rows.push_back( runCentres( line ) );
				}
				bool ok = true;
				std::string what;
				if( bond == Bond::English )
				{
					//Stretchers and headers in alternate courses; which comes
					//first depends on where the frame's centre falls.
					const bool flip = spacingOf( rows[ 0 ] ) < spacingOf( rows[ 1 ] );
					const double st = spacingOf( rows[ flip ? 1 : 0 ] ) * texel, hd = spacingOf( rows[ flip ? 0 : 1 ] ) * texel;
					const double st2 = spacingOf( rows[ flip ? 3 : 2 ] ) * texel;
					ok   = std::fabs( st - ( stated::kBrickLength + mt ) ) <= 0.25 * texel && std::fabs( hd - ( stated::kHeaderLength + mt ) ) <= 0.25 * texel
					     && std::fabs( st2 - st ) <= 0.25 * texel;
					what = fmt( "stretchers %.2f mm, headers %.2f mm apart (%.2f, %.2f)", 1000.0 * st, 1000.0 * hd,
					            1000.0 * ( stated::kBrickLength + mt ), 1000.0 * ( stated::kHeaderLength + mt ) );
				}
				else
				{
					const std::vector< double >& r = rows[ 0 ];
					double longGap = 0.0, shortGap = 0.0;
					int nl = 0, ns = 0;
					for( size_t k = 1; k < r.size(); ++k )
					{
						const double gap = ( r[ k ] - r[ k - 1 ] ) * texel;
						if( gap > 0.16 )
						{
							longGap += gap;
							++nl;
						}
						else
						{
							shortGap += gap;
							++ns;
						}
					}
					longGap /= std::max( nl, 1 );
					shortGap /= std::max( ns, 1 );
					ok   = nl >= 2 && ns >= 2 && std::abs( nl - ns ) <= 1 && std::fabs( longGap - ( stated::kBrickLength + mt ) ) <= 1.0 * texel
					     && std::fabs( shortGap - ( stated::kHeaderLength + mt ) ) <= 1.0 * texel;
					what = fmt( "%d stretchers and %d headers alternating in one course, %.1f and %.1f mm", nl, ns, 1000.0 * longGap, 1000.0 * shortGap );
				}
				Check( ok, fmt( "%4dx%-4d Brick, %-8s %s", w, h, bond == Bond::English ? "English:" : "Flemish:", what.c_str() ) );
			}
		}
		//Corrugated: period Pitch, depth 18/76 of it, and the sine itself.
		for( double pitch : { 0.076, 0.15 } )
		{
			Rig rig;
			if( !fresh( rig, Surface::Corrugated ) )
				return 1;
			rig.Set( PT_PITCH, ParamFromPitch( pitch ) );
			rig.Render( 1 );
			const Floats sf  = rig.Surface();
			const double P   = PitchFromParam( rig.Get( PT_PITCH ) );
			const double R   = rig.plugin.RoomForTest().relief;
			const int j      = h / 2;
			std::vector< double > rises;
			double lo = 0.0, hi = -1.0, worstSine = 0.0;
			for( int i = 0; i < w; ++i )
			{
				const double z = sf[ ( static_cast< size_t >( j ) * w + i ) * 4 + 3 ];
				lo             = std::min( lo, z );
				hi             = std::max( hi, z );
				//The sine, at the texel centre, against the 2x2 average of it.
				const double x = ( ( i + 0.5 ) / w - 0.5 ) * texel * w;
				double want    = 0.0;
				for( double dx : { -0.25, 0.25 } )
					want += -R * ( 0.5 - 0.5 * std::cos( 2.0 * kPi * ( x + dx * texel ) / P ) ) * 0.5;
				worstSine = std::max( worstSine, std::fabs( z - want ) );
				if( i > 0 )
				{
					const double z0 = sf[ ( static_cast< size_t >( j ) * w + i - 1 ) * 4 + 3 ];
					if( z0 < -0.5 * R && z >= -0.5 * R )
						rises.push_back( i - 1 + ( -0.5 * R - z0 ) / ( z - z0 ) );
				}
			}
			const double period = spacingOf( rises ) * texel;
			Check( std::fabs( period - P ) <= 0.002 * P && std::fabs( ( hi - lo ) - stated::kSineDepthPerPitch * P ) <= 0.01 * R && worstSine <= 1e-6 + 1e-5 * R,
			       fmt( "%4dx%-4d Corrugated, Pitch %.0f mm: period %.3f mm, %.2f mm deep (%.2f), the sine's profile within %.1e m", w, h, 1000.0 * P,
			            1000.0 * period, 1000.0 * ( hi - lo ), 1000.0 * stated::kSineDepthPerPitch * P, worstSine ) );
		}
		//Plate: 30 mm cells.
		{
			Rig rig;
			if( !fresh( rig, Surface::Plate ) )
				return 1;
			rig.Option( PT_PATTERN, Pattern::FiveBar );
			rig.Render( 1 );
			const Floats sf = rig.Surface();
			//Five-Bar's cells alternate, so the pattern repeats every two cells:
			//the height's autocorrelation along a row peaks at 60 mm.
			const int j = h / 2 + 3;
			auto at     = [ & ]( int i ) { return sf[ ( static_cast< size_t >( j ) * w + i ) * 4 + 3 ]; };
			double best = -1e9;
			int bestLag = 0;
			for( int lag = 4; lag < static_cast< int >( 0.09 / texel ); ++lag )
			{
				double sum = 0.0;
				for( int i = 0; i + lag < w; ++i )
					sum += at( i ) * at( i + lag );
				sum /= ( w - lag );
				if( sum > best )
				{
					best    = sum;
					bestLag = lag;
				}
			}
			Check( std::abs( bestLag * texel - 2.0 * stated::kPlateCell ) <= 1.0 * texel,
			       fmt( "%4dx%-4d Plate, Five-Bar: the pattern repeats every %.1f mm (two 30 mm cells)", w, h, 1000.0 * bestLag * texel ) );
		}
		//Drapes: Pleats folds across the top.
		for( int pleats : { 8, 21 } )
		{
			Rig rig;
			if( !fresh( rig, Surface::Drapes ) )
				return 1;
			rig.Set( PT_PLEATS, pleats );
			rig.Render( 1 );
			const Floats sf = rig.Surface();
			const double R  = rig.plugin.RoomForTest().relief;
			int crests      = 0;
			const int j     = h - 1;
			for( int i = 1; i < w; ++i )
			{
				const double a = sf[ ( static_cast< size_t >( j ) * w + i - 1 ) * 4 + 3 ], b = sf[ ( static_cast< size_t >( j ) * w + i ) * 4 + 3 ];
				crests += a < -0.5 * R && b >= -0.5 * R;
			}
			Check( crests == pleats, fmt( "%4dx%-4d Drapes, %2d pleats: %d folds across the top", w, h, pleats, crests ) );
		}
		//Paper, crumpled not at all, is flat.
		{
			Rig rig;
			if( !fresh( rig, Surface::Paper ) )
				return 1;
			rig.Set( PT_CRUMPLE, 0.0 );
			rig.Render( 1 );
			const Floats sf = rig.Surface();
			double most     = 0.0;
			for( size_t i = 3; i < sf.size(); i += 4 )
				most = std::max( most, std::fabs( static_cast< double >( sf[ i ] ) ) );
			Check( most == 0.0, fmt( "%4dx%-4d Paper, Crumple 0: flat (deepest %.1e m)", w, h, most ) );
		}
	}
	return Verdict();
}

//===========================================================================
// --file: the user's own map.
//===========================================================================
int runFile( const Perturb& perturb )
{
	std::printf( "\n=== file: a height map or a photo, read exactly; a missing or broken file is Flat; Scale tiles it, mirrored\n" );
	for( const Raster& raster : kRasters )
	{
		const int w = raster.w, h = raster.h;
		Rig rig;
		if( !rig.Init( w, h ) )
			return 1;
		rig.plugin.SetHooksForTest( perturb.hooks );
		rig.Set( PT_DEPTH, ParamFromDepth( 1.0 ) );
		const double R = 0.1;

		//16 bits.
		Words grey( static_cast< size_t >( w ) * h );
		for( int j = 0; j < h; ++j )
			for( int i = 0; i < w; ++i )
				grey[ static_cast< size_t >( j ) * w + i ] = static_cast< uint16_t >( ( i * 7919u + j * 104729u + i * j * 31u ) % 65536u );
		if( !useMap( rig, "grey16", grey ) || !rig.Render( 1 ) )
			return 1;
		Floats sf     = rig.Surface();
		double worst16 = 0.0, white = 0.0;
		for( size_t k = 0; k < grey.size(); ++k )
		{
			worst16 = std::max( worst16, std::fabs( sf[ k * 4 + 3 ] - ( -R * ( 1.0 - grey[ k ] / 65535.0 ) ) ) );
			white   = std::max( white, std::fabs( sf[ k * 4 + 1 ] - 1.0 ) );
		}

		//8 bits.
		Bytes grey8( static_cast< size_t >( w ) * h );
		for( int j = 0; j < h; ++j )
			for( int i = 0; i < w; ++i )
				grey8[ static_cast< size_t >( j ) * w + i ] = static_cast< unsigned char >( ( i * 31 + j * 17 ) % 256 );
		const std::string path8 = scratchPath( "grey8.png" );
		write8( path8, w, h, 1, grey8 );
		rig.File( path8 );
		rig.Render( 1 );
		sf             = rig.Surface();
		double worst8  = 0.0;
		for( size_t k = 0; k < grey8.size(); ++k )
			worst8 = std::max( worst8, std::fabs( sf[ k * 4 + 3 ] - ( -R * ( 1.0 - grey8[ k ] / 255.0 ) ) ) );

		//A photo: its colour the albedo, its luminance (stretched) the height.
		Bytes rgb( static_cast< size_t >( w ) * h * 3 );
		for( size_t k = 0; k < rgb.size(); ++k )
			rgb[ k ] = static_cast< unsigned char >( 40 + ( k * 2654435761u >> 7 ) % 180 );
		const std::string pathRgb = scratchPath( "photo.png" );
		write8( pathRgb, w, h, 3, rgb );
		rig.File( pathRgb );
		rig.Option( PT_FILE_HOLDS, FileHolds::Photo );
		rig.Render( 1 );
		sf = rig.Surface();
		double ylo = 1e9, yhi = -1e9;
		for( size_t k = 0; k < rgb.size() / 3; ++k )
		{
			const double y = ( 0.2126 * rgb[ k * 3 ] * 257 + 0.7152 * rgb[ k * 3 + 1 ] * 257 + 0.0722 * rgb[ k * 3 + 2 ] * 257 ) / 65535.0;
			ylo            = std::min( ylo, y );
			yhi            = std::max( yhi, y );
		}
		double worstPhoto = 0.0, worstHeight = 0.0;
		for( size_t k = 0; k < rgb.size() / 3; ++k )
		{
			for( int c = 0; c < 3; ++c )
				worstPhoto = std::max( worstPhoto, std::fabs( sf[ k * 4 + c ] - linearOf( rgb[ k * 3 + c ] / 255.0 ) ) );
			const double y = ( 0.2126 * rgb[ k * 3 ] * 257 + 0.7152 * rgb[ k * 3 + 1 ] * 257 + 0.0722 * rgb[ k * 3 + 2 ] * 257 ) / 65535.0;
			worstHeight    = std::max( worstHeight, std::fabs( sf[ k * 4 + 3 ] - ( -R * ( 1.0 - ( y - ylo ) / ( yhi - ylo ) ) ) ) );
		}
		Check( worst16 <= 1e-7 && white == 0.0 && worst8 <= 1e-7 && worstPhoto <= 1e-6 && worstHeight <= 1e-7,
		       fmt( "%4dx%-4d a 16-bit map's heights within %.1e m (R %.1f m), white; an 8-bit map's within %.1e m; a photo's albedo within %.1e,"
		            " its luminance's height within %.1e m",
		            w, h, worst16, R, worst8, worstPhoto, worstHeight ) );

		//Missing, then broken: Flat.
		bool flat = true;
		for( const std::string& bad : { scratchPath( "no-such-file.png" ), scratchPath( "broken.png" ) } )
		{
			if( bad.find( "broken" ) != std::string::npos )
			{
				std::ofstream junk( bad, std::ios::binary );
				junk << "\x89PNG\r\n\x1a\nthis is not a picture";
			}
			rig.File( bad );
			rig.Render( 1 );
			sf = rig.Surface();
			for( size_t k = 0; k < sf.size(); k += 4 )
				flat = flat && sf[ k ] == 1.0f && sf[ k + 3 ] == 0.0f;
			flat = flat && !rig.plugin.FileLoadedForTest() && rig.plugin.RoomForTest().relief == 0.0;
		}

		//Scale 0.5: the map at half size, mirrored around it. Every texel is
		//then halfway between four of the file's, which a GPU's 8-bit weights
		//hold exactly.
		rig.Option( PT_FILE_HOLDS, FileHolds::Height );
		if( !useMap( rig, "grey16b", grey ) )
			return 1;
		rig.Set( PT_SCALE, ParamFromScale( 0.5 ) );
		rig.Render( 1 );
		sf              = rig.Surface();
		const double Rh = stated::kFileRelief * 0.5;
		auto mirror     = [ ]( int k, int n ) {
			const int period = 2 * n;
			k                = ( ( k % period ) + period ) % period;
			return k < n ? k : period - 1 - k;
		};
		double worstTile = 0.0;
		for( int j = 0; j < h; ++j )
			for( int i = 0; i < w; ++i )
			{
				//File texel coordinate 2 i + 0.5 - w / 2: between 2 i - w/2 and the next.
				const int fi = 2 * i - w / 2, fj = 2 * j - h / 2;
				double a     = 0.0;
				for( int dj = 0; dj < 2; ++dj )
					for( int di = 0; di < 2; ++di )
						a += 0.25 * grey[ static_cast< size_t >( mirror( fj + dj, h ) ) * w + mirror( fi + di, w ) ] / 65535.0;
				worstTile = std::max( worstTile, std::fabs( sf[ ( static_cast< size_t >( j ) * w + i ) * 4 + 3 ] - ( -Rh * ( 1.0 - a ) ) ) );
			}
		Check( flat && worstTile <= 2e-7,
		       fmt( "%4dx%-4d a missing file and a broken one are both Flat (%s); at Scale 0.5 the map tiles mirrored, within %.1e m", w, h,
		            flat ? "yes" : "NO", worstTile ) );
	}
	return Verdict();
}

//===========================================================================
// --dirty: the surface follows its controls, and only them.
//===========================================================================
int runDirty( const Perturb& perturb )
{
	std::printf( "\n=== dirty: every surface control changed on a running instance gives the frame a fresh instance gives; nothing else"
	             " rebuilds the surface\n" );
	struct Step
	{
		const char* what;
		std::vector< std::pair< unsigned int, float > > set;
	};
	const Step steps[] = {
		{ "Surface Paper", { { PT_SURFACE, 1.0f } } },
		{ "Crumple", { { PT_CRUMPLE, 0.35f } } },
		{ "Seed", { { PT_SEED, 17.0f } } },
		{ "Surface Brick", { { PT_SURFACE, 2.0f } } },
		{ "Bond", { { PT_BOND, 2.0f } } },
		{ "Mortar", { { PT_MORTAR, 0.8f } } },
		{ "Scale", { { PT_SCALE, 0.7f } } },
		{ "Depth", { { PT_DEPTH, 0.8f } } },
		{ "Wall Height", { { PT_WALL_HEIGHT, 0.3f } } },
		{ "Surface Corrugated", { { PT_SURFACE, 3.0f } } },
		{ "Profile", { { PT_PROFILE, 1.0f } } },
		{ "Pitch", { { PT_PITCH, 0.8f } } },
		{ "Surface Plate", { { PT_SURFACE, 4.0f } } },
		{ "Pattern", { { PT_PATTERN, 2.0f } } },
		{ "Surface Drapes", { { PT_SURFACE, 5.0f } } },
		{ "Pleats", { { PT_PLEATS, 9.0f } } },
		{ "Sag", { { PT_SAG, 0.9f } } },
	};
	for( const Raster& raster : kRasters )
	{
		Rig running;
		if( !running.Init( raster.w, raster.h ) )
			return 1;
		running.plugin.SetHooksForTest( perturb.hooks );
		running.Render( 2 );
		std::vector< std::pair< unsigned int, float > > all;
		int wrong = 0, missed = 0, exact = 0;
		Gap largest;
		std::string what;
		for( const Step& step : steps )
		{
			const int before = running.plugin.SurfaceBuildsForTest();
			for( const auto& s : step.set )
			{
				running.Set( s.first, s.second );
				all.push_back( s );
			}
			const Floats got = running.Frame();
			if( running.plugin.SurfaceBuildsForTest() != before + 1 )
				++missed;
			Rig fresh;
			if( !fresh.Init( raster.w, raster.h ) )
				return 1;
			for( const auto& s : all )
				fresh.Set( s.first, s.second );
			const Gap gap = gapBetween( fresh.Frame(), got );
			if( gap.Exact() )
				++exact;
			else if( gap.worst >= largest.worst )
				largest = gap;
			if( !gap.Same() )
			{
				++wrong;
				what += fmt( " %s (%s)", step.what, gap.Say().c_str() );
			}
		}
		//The room's controls move the picture, never the surface.
		const int before = running.plugin.SurfaceBuildsForTest();
		for( unsigned int id : { PT_PROJECTOR_X, PT_THROW, PT_VIEWER_X, PT_AMBIENT, PT_EXPOSURE, PT_MIX, PT_VIEW } )
		{
			running.Set( id, 0.3 );
			running.Render( 1 );
		}
		running.Render( 3 );
		const int rebuilt = running.plugin.SurfaceBuildsForTest() - before;
		const int count = static_cast< int >( sizeof( steps ) / sizeof( steps[ 0 ] ) );
		Check( wrong == 0 && missed == 0 && rebuilt == 0,
		       fmt( "%4dx%-4d %d surface changes: %d frames are a fresh instance's bit for bit%s, %d are not%s, %d changes not rebuilt;"
		            " projector, viewer, room and output changes rebuilt it %d times",
		            raster.w, raster.h, count, exact, exact == count ? "" : fmt( " (the rest within the renderer's own repeatability: worst %s)", largest.Say().c_str() ).c_str(),
		            wrong, what.c_str(), missed, rebuilt ) );
	}
	return Verdict();
}

//===========================================================================
// --resize: a new raster.
//===========================================================================
int runResize( const Perturb& perturb )
{
	std::printf( "\n=== resize: the host's raster changes under a running instance; the frame is a fresh instance's at the new size\n" );
	const Raster sizes[] = { { 1280, 720 }, { 640, 480 }, { 320, 180 } };
	Rig running;
	if( !running.Init( sizes[ 0 ].w, sizes[ 0 ].h ) )
		return 1;
	running.plugin.SetHooksForTest( perturb.hooks );
	running.Option( PT_SURFACE, Surface::Drapes );
	running.Render( 2 );
	for( int k = 1; k < 3; ++k )
	{
		if( !running.Resize( sizes[ k ].w, sizes[ k ].h ) )
			return 1;
		const Floats got = running.Frame();
		Rig fresh;
		if( !fresh.Init( sizes[ k ].w, sizes[ k ].h ) )
			return 1;
		fresh.Option( PT_SURFACE, Surface::Drapes );
		const Gap gap = gapBetween( fresh.Frame(), got );
		Check( gap.Same(), fmt( "%dx%d to %dx%d: the frame is a fresh instance's: %s", sizes[ k - 1 ].w, sizes[ k - 1 ].h, sizes[ k ].w,
		                        sizes[ k ].h, gap.Say().c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --state: the GL state the host hands over is the state it gets back.
//===========================================================================
int runState( const Perturb& )
{
	std::printf( "\n=== state: the GL state the host hands over is the state it gets back, on every surface\n" );
	for( int kind = 0; kind < static_cast< int >( Surface::Count ); ++kind )
	{
		Rig rig;
		if( !rig.Init( 320, 180 ) )
			return 1;
		rig.Set( PT_SURFACE, kind );
		if( kind == static_cast< int >( Surface::File ) )
		{
			Words grey( 320 * 180 );
			for( size_t k = 0; k < grey.size(); ++k )
				grey[ k ] = static_cast< uint16_t >( k * 977 );
			if( !useMap( rig, "state", grey ) )
				return 1;
		}
		GLuint hostArray = 0, hostBuffer = 0;
		glGenVertexArrays( 1, &hostArray );
		glGenBuffers( 1, &hostBuffer );
		int problems = 0;
		std::string what;
		for( int frame = 0; frame < 6; ++frame )
		{
			if( frame == 3 && !rig.Resize( 256, 144 ) )
				return 1;
			if( frame == 4 )
				rig.Set( PT_SEED, 5.0 );
			glBindFramebuffer( GL_FRAMEBUFFER, rig.outputFBO );
			glViewport( 0, 0, rig.width, rig.height );
			glBindVertexArray( hostArray );
			glBindBuffer( GL_ARRAY_BUFFER, hostBuffer );
			glEnable( GL_BLEND );
			glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ZERO );
			glClearColor( 0.2f, 0.3f, 0.4f, 0.5f );
			glEnable( GL_SCISSOR_TEST );
			glScissor( 0, 0, rig.width, rig.height );
			glActiveTexture( GL_TEXTURE0 );
			glUseProgram( 0 );
			if( rig.plugin.ProcessOpenGL( &rig.process ) != FF_SUCCESS )
				return 1;
			GLint viewport[ 4 ] = {}, array = 0, buffer = 0, program = 0, unit = 0, fbo = 0, src = 0, dst = 0;
			GLfloat clear[ 4 ]   = {};
			GLboolean mask[ 4 ]  = {};
			glGetIntegerv( GL_VIEWPORT, viewport );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &array );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &buffer );
			glGetIntegerv( GL_CURRENT_PROGRAM, &program );
			glGetIntegerv( GL_ACTIVE_TEXTURE, &unit );
			glGetIntegerv( GL_FRAMEBUFFER_BINDING, &fbo );
			glGetIntegerv( GL_BLEND_SRC_RGB, &src );
			glGetIntegerv( GL_BLEND_DST_RGB, &dst );
			glGetFloatv( GL_COLOR_CLEAR_VALUE, clear );
			glGetBooleanv( GL_COLOR_WRITEMASK, mask );
			auto expect = [ & ]( bool ok, const char* name ) {
				if( !ok )
				{
					++problems;
					what += std::string( " " ) + name;
				}
			};
			expect( viewport[ 0 ] == 0 && viewport[ 1 ] == 0 && viewport[ 2 ] == rig.width && viewport[ 3 ] == rig.height, "viewport" );
			expect( array == static_cast< GLint >( hostArray ), "vertex-array" );
			expect( buffer == static_cast< GLint >( hostBuffer ), "array-buffer" );
			expect( program == 0, "program" );
			expect( unit == GL_TEXTURE0, "active-unit" );
			expect( fbo == static_cast< GLint >( rig.outputFBO ), "framebuffer" );
			expect( glIsEnabled( GL_BLEND ) && src == GL_SRC_ALPHA && dst == GL_ONE_MINUS_SRC_ALPHA, "blend" );
			expect( glIsEnabled( GL_SCISSOR_TEST ), "scissor" );
			expect( clear[ 0 ] == 0.2f && clear[ 1 ] == 0.3f && clear[ 2 ] == 0.4f && clear[ 3 ] == 0.5f, "clear-colour" );
			expect( mask[ 0 ] && mask[ 1 ] && mask[ 2 ] && mask[ 3 ], "colour-mask" );
			for( int u = 0; u < 8; ++u )
			{
				GLint bound = 0;
				glActiveTexture( static_cast< GLenum >( GL_TEXTURE0 + u ) );
				glGetIntegerv( GL_TEXTURE_BINDING_2D, &bound );
				expect( bound == 0, "texture-unit" );
			}
			glActiveTexture( GL_TEXTURE0 );
		}
		glDisable( GL_SCISSOR_TEST );
		glDisable( GL_BLEND );
		glBindVertexArray( 0 );
		glBindBuffer( GL_ARRAY_BUFFER, 0 );
		glDeleteVertexArrays( 1, &hostArray );
		glDeleteBuffers( 1, &hostBuffer );
		const char* names[] = { "Flat", "Paper", "Brick", "Corrugated", "Plate", "Drapes", "Weatherboard", "Tiles", "File" };
		Check( problems == 0, fmt( "%-12s six frames, a resize and a rebuild: viewport, vertex array, array buffer, program, active unit, framebuffer,"
		                           " blend, scissor, clear colour, colour mask, eight texture units (%d wrong:%s)",
		                           names[ kind ], problems, what.empty() ? " none" : what.c_str() ) );
	}
	return Verdict();
}

//===========================================================================
// --units (no GL): the controls mean what the README says.
//===========================================================================
int runUnits( const Perturb& perturb )
{
	std::printf( "\n=== units: every control's mapping hits its stated values; the defaults are the spec's; the room is worked out right\n" );
	auto near = []( double a, double b, double rel = 1e-6 ) { return std::fabs( a - b ) <= rel * std::max( 1.0, std::fabs( b ) ); };
	struct Stated
	{
		const char* what;
		double got, want;
	};
	const Stated ends[] = {
		{ "Depth at 0.5 is exactly 1", DepthFromParam( 0.5f ), 1.0 },
		{ "Depth at 0 is exactly 0", DepthFromParam( 0.0f ), 0.0 },
		{ "Depth at 1 is 4", DepthFromParam( 1.0f ), 4.0 },
		{ "Scale at 0.5 is exactly 1", ScaleFromParam( 0.5f ), 1.0 },
		{ "Scale's ends 0.25", ScaleFromParam( 0.0f ), 0.25 },
		{ "Scale's ends 4", ScaleFromParam( 1.0f ), 4.0 },
		{ "Wall Height 0.25 m", WallHeightFromParam( 0.0f ), 0.25 },
		{ "Wall Height 20 m", WallHeightFromParam( 1.0f ), 20.0 },
		{ "an offset at 0.5 is 0", OffsetFromParam( 0.5f ), 0.0 },
		{ "an offset at 0 is -10 m", OffsetFromParam( 0.0f ), -10.0 },
		{ "an offset at 1 is +10 m", OffsetFromParam( 1.0f ), 10.0 },
		{ "a distance at 0 is 1 m", DistanceFromParam( 0.0f ), 1.0 },
		{ "a distance at 1 is 50 m", DistanceFromParam( 1.0f ), 50.0 },
		{ "Lumens 500", LumensFromParam( 0.0f ), 500.0 },
		{ "Lumens 50000", LumensFromParam( 1.0f ), 50000.0 },
		{ "Ambient 0 lux", AmbientFromParam( 0.0f ), 0.0 },
		{ "Ambient 1000 lux", AmbientFromParam( 1.0f ), 1000.0 },
		{ "Black Level 0", BlackLevelFromParam( 0.0f ), 0.0 },
		{ "Black Level 2%", BlackLevelFromParam( 1.0f ), 0.02 },
		{ "Exposure at 0.5 is x1", ExposureFromParam( 0.5f ), 1.0 },
		{ "Exposure -4 stops", ExposureFromParam( 0.0f ), 1.0 / 16.0 },
		{ "Exposure +4 stops", ExposureFromParam( 1.0f ), 16.0 },
		{ "Mortar 3 mm", MortarFromParam( 0.0f ), 0.003 },
		{ "Mortar 25 mm", MortarFromParam( 1.0f ), 0.025 },
		{ "Pitch 25 mm", PitchFromParam( 0.0f ), 0.025 },
		{ "Pitch 300 mm", PitchFromParam( 1.0f ), 0.300 },
		{ "Roughness 0.05", RoughnessFromParam( 0.0f ), 0.05 },
		{ "Roughness 0.8", RoughnessFromParam( 1.0f ), 0.8 },
		{ "the default wall, 2.5 m", WallHeightFromParam( InfoOf( PT_WALL_HEIGHT ).defaultValue ), 2.5 },
		{ "the default projector, 1 m up", OffsetFromParam( InfoOf( PT_PROJECTOR_Y ).defaultValue ), 1.0 },
		{ "the default throw, 6 m", DistanceFromParam( InfoOf( PT_THROW ).defaultValue ), 6.0 },
		{ "the default lumens, 10,000", LumensFromParam( InfoOf( PT_LUMENS ).defaultValue ), 10000.0 },
		{ "the default black, 0.05%", BlackLevelFromParam( InfoOf( PT_BLACK_LEVEL ).defaultValue ), 0.0005 },
		{ "the default room, 20 lux", AmbientFromParam( InfoOf( PT_AMBIENT ).defaultValue ), 20.0 },
		{ "the default viewer, 2 m right", OffsetFromParam( InfoOf( PT_VIEWER_X ).defaultValue ), 2.0 },
		{ "the default viewer, 0.8 m down", OffsetFromParam( InfoOf( PT_VIEWER_Y ).defaultValue ), -0.8 },
		{ "the default viewer, 4 m back", DistanceFromParam( InfoOf( PT_VIEWER_DISTANCE ).defaultValue ), 4.0 },
		{ "the default mortar, 10 mm", MortarFromParam( InfoOf( PT_MORTAR ).defaultValue ), 0.010 },
		{ "the default pitch, 76 mm", PitchFromParam( InfoOf( PT_PITCH ).defaultValue ), 0.076 },
		{ "the default roughness, 0.35", RoughnessFromParam( InfoOf( PT_ROUGHNESS ).defaultValue ), 0.35 },
		{ "the default depth, 1", DepthFromParam( InfoOf( PT_DEPTH ).defaultValue ), 1.0 },
		{ "Paper at Crumple 0.3: the first generation whole", PaperWeight( 0.3, 0 ), 1.0 },
		{ "...the second a fifth in", PaperWeight( 0.3, 1 ), 0.2 },
		{ "...the third not yet", PaperWeight( 0.3, 2 ), 0.0 },
	};
	int bad = 0;
	for( const Stated& s : ends )
		if( !near( s.got, s.want ) )
		{
			std::printf( "    %s: got %.9g, want %.9g\n", s.what, s.got, s.want );
			++bad;
		}
	Check( bad == 0, fmt( "%zu stated values and defaults, %d wrong", sizeof( ends ) / sizeof( ends[ 0 ] ), bad ) );

	//The room: W from the raster, the room's ratio, the reliefs, the basis.
	float p[ PT_COUNT ] = {};
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
		p[ id ] = InfoOf( id ).defaultValue;
	int roomBad = 0;
	std::string what;
	for( const Raster& r : { Raster { 1920, 1080 }, Raster { 1024, 768 }, Raster { 500, 1000 } } )
	{
		p[ PT_SURFACE ]    = static_cast< float >( Surface::Drapes );
		p[ PT_AIM ]        = static_cast< float >( Aim::Pointed );
		p[ PT_PROJECTOR_X ] = ParamFromOffset( -3.0 );
		const Room room    = MakeRoom( p, r.w, r.h );
		const double H     = 2.5;
		const double W     = perturb.unitsAspect ? H * r.h / r.w : H * r.w / r.h;
		const V3 axis = { room.axis.x, room.axis.y, room.axis.z }, right = { room.right.x, room.right.y, room.right.z }, up = { room.up.x, room.up.y, room.up.z };
		const V3 C    = { room.lens.x, room.lens.y, room.lens.z };
		const V3 rxu  = { right.y * up.z - right.z * up.y, right.z * up.x - right.x * up.z, right.x * up.y - right.y * up.x };
		auto expect   = [ & ]( bool ok, const std::string& name ) {
			if( !ok )
			{
				++roomBad;
				what += " " + name;
			}
		};
		expect( near( room.W, W ), fmt( "W(%dx%d)", r.w, r.h ) );
		expect( near( room.roomRatio, 20.0 * W * H / 10000.0 ), "room-ratio" );
		expect( near( room.relief, 0.5 * W / 16.0 ), "drape-relief" );
		expect( near( norm( axis ), 1.0 ) && near( norm( right ), 1.0 ) && near( norm( up ), 1.0 ), "unit-basis" );
		expect( std::fabs( dot( axis, right ) ) < 1e-12 && std::fabs( dot( axis, up ) ) < 1e-12 && std::fabs( right.y ) < 1e-12, "square-basis" );
		expect( near( dot( axis, unit( V3 {} - C ) ), 1.0 ) && near( room.design, norm( C ) ), "aimed-at-centre" );
		expect( dot( rxu, axis ) < 0.0 && up.y > 0.0, "right-handed" );
	}
	struct Relief
	{
		Surface kind;
		double want;
	};
	p[ PT_CRUMPLE ]   = 1.0f;
	const Relief reliefs[] = {
		{ Surface::Flat, 0.0 },
		{ Surface::Paper, 2.0 * 0.015 * ( 1.0 + 0.45 + 0.45 * 0.45 + 0.45 * 0.45 * 0.45 ) },
		{ Surface::Brick, 0.008 },
		{ Surface::Corrugated, 0.018 },
		{ Surface::Plate, 0.0015 },
		{ Surface::Weatherboard, 0.015 },
		{ Surface::Tiles, 0.0045 },
		{ Surface::File, 0.1 },
	};
	for( const Relief& rl : reliefs )

	{
		const double got = ReliefOf( rl.kind, p, 4.0 );
		if( !near( got, rl.want, 1e-5 ) )
		{
			++roomBad;
			what += fmt( " relief(%d)=%.6f", static_cast< int >( rl.kind ), got );
		}
	}
	Check( roomBad == 0, fmt( "W = H x aspect at three rasters, A = lux W H / lumens, the Pointed basis square, unit, right-handed and aimed at the"
	                          " centre, and every surface's natural relief (%d wrong:%s)",
	                          roomBad, what.empty() ? " none" : what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --cues (no GL): options, integers and files step; the rest ramp.
//===========================================================================
int runCues( const Perturb& perturb )
{
	std::printf( "\n=== cues: a cue sheet steps options and integers, and ramps standard controls\n" );
	std::istringstream sheet( "0 Surface 0\n60 Surface 5\n0 Pleats 4\n60 Pleats 40\n0 Depth 0\n60 Depth 1\n" );
	std::string error;
	const auto tracks = loadScript( sheet, "cues", error );
	ThrowPlugin plugin;
	int bad = 0;
	std::string what;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		const auto found = tracks.find( p.name );
		if( found == tracks.end() )
			continue;
		const bool ramp       = perturb.cuesRamp ? true : !stepsBetweenCues( p.type );
		const float mid       = valueAt( found->second, 29, ramp );
		const float a         = found->second.front().second, b = found->second.back().second;
		const bool expectRamp = p.type == FF_TYPE_STANDARD;
		const bool ok         = expectRamp ? ( mid > a && mid < b ) : ( mid == a && valueAt( found->second, 60, ramp ) == b );
		bad += !ok;
		what += fmt( " %s %s(%g at frame 29)", p.name.c_str(), expectRamp ? "ramps " : "steps ", mid );
	}
	Check( error.empty() && bad == 0 && tracks.size() == 3, fmt( "%d wrong:%s", bad, what.c_str() ) );
	return Verdict();
}

//===========================================================================
// --names (no GL)
//===========================================================================
int runNames( const Perturb& )
{
	std::printf( "\n=== names: every parameter unique (as Arena addresses them, too) and within FFGL's 16 characters\n" );
	ThrowPlugin plugin;
	std::map< std::string, int > seen, address;
	int longNames = 0, dupes = 0, clashes = 0;
	for( unsigned int i = 0; i < plugin.GetNumParams(); ++i )
	{
		const std::string name = plugin.GetParamName( i ) ? plugin.GetParamName( i ) : "";
		if( name.size() > 16 )
		{
			std::printf( "    too long: %s\n", name.c_str() );
			++longNames;
		}
		if( seen[ name ]++ > 0 )
			++dupes;
		std::string key;
		for( char c : name )
			if( c != ' ' )
				key += static_cast< char >( std::tolower( static_cast< unsigned char >( c ) ) );
		if( address[ key ]++ > 0 )
			++clashes;
	}
	const unsigned int aboutFirst = plugin.GetNumParams() - stoatworks::about::kParamCount;
	const bool aboutLast          = std::string( plugin.GetParamName( aboutFirst ) ) == "About" && aboutFirst == PT_ABOUT_FIRST;
	Check( longNames == 0 && dupes == 0 && clashes == 0 && aboutLast,
	       fmt( "SW Throw: %u parameters, %d too long, %d duplicated, %d clashing addresses; the About block is last (%s)", plugin.GetNumParams(),
	            longNames, dupes, clashes, aboutLast ? "yes" : "NO" ) );
	return Verdict();
}

//===========================================================================
// --bench: GPU time per frame from GL_TIME_ELAPSED.
//===========================================================================
int runBench()
{
	struct Size
	{
		int w, h;
		const char* name;
	};
	const Size sizes[] = { { 1280, 720, "720p" }, { 1920, 1080, "1080p" }, { 3840, 2160, "4K" } };
	struct Case
	{
		const char* what;
		std::function< void( Rig& ) > set;
		bool rebuild;
	};
	const Case cases[] = {
		{ "defaults (Brick, Depth 1)", []( Rig& ) {}, false },
		{ "Drapes, 16 pleats, Depth 1", []( Rig& r ) { r.Option( PT_SURFACE, Surface::Drapes ); }, false },
		{ "Corrugated, Depth 4", []( Rig& r ) {
			 r.Option( PT_SURFACE, Surface::Corrugated );
			 r.Set( PT_DEPTH, 1.0 );
		 }, false },
		{ "Paper, Crumple 1, Depth 4", []( Rig& r ) {
			 r.Option( PT_SURFACE, Surface::Paper );
			 r.Set( PT_CRUMPLE, 1.0 );
			 r.Set( PT_DEPTH, 1.0 );
		 }, false },
		{ "Paper, Seed changed every frame", []( Rig& r ) {
			 r.Option( PT_SURFACE, Surface::Paper );
			 r.Set( PT_CRUMPLE, 1.0 );
		 }, true },
	};
	std::printf( "\n=== bench: GPU time per frame (GL_TIME_ELAPSED), after 30 frames of warm-up, median and worst of 60\n" );
	GLuint query = 0;
	glGenQueries( 1, &query );
	for( const Case& c : cases )
		for( const Size& size : sizes )
		{
			Rig rig;
			if( !rig.Init( size.w, size.h ) )
				return 1;
			c.set( rig );
			if( !rig.Render( 30 ) )
				return 1;
			constexpr int kTimed = 60;
			std::vector< double > times;
			for( int f = 0; f < kTimed; ++f )
			{
				if( c.rebuild )
					rig.Set( PT_SEED, f % 100 );
				glBeginQuery( GL_TIME_ELAPSED, query );
				if( !rig.Render( 1 ) )
					return 1;
				glEndQuery( GL_TIME_ELAPSED );
				GLuint64 ns = 0;
				glGetQueryObjectui64v( query, GL_QUERY_RESULT, &ns );
				times.push_back( static_cast< double >( ns ) * 1e-6 );
			}
			std::sort( times.begin(), times.end() );
			std::printf( "  %-34s %-6s GPU median %6.2f ms, worst %6.2f  (%5.1f%% of a 60 fps frame)\n", c.what, size.name, times[ kTimed / 2 ],
			             times.back(), 100.0 * times[ kTimed / 2 ] / ( 1000.0 / 60.0 ) );
		}
	glDeleteQueries( 1, &query );
	return 0;
}

struct CheckEntry
{
	const char* flag;
	CheckFn run;
	bool offline;///< needs no GL context
};

const std::vector< CheckEntry >& checks()
{
	static const std::vector< CheckEntry > list = {
		{ "units", runUnits, true },       { "flat", runFlat, false },       { "parallax", runParallax, false }, { "lambert", runLambert, false },
		{ "away", runAway, false },        { "shadow", runShadow, false },   { "atlens", runAtLens, false },     { "hotspot", runHotspot, false },
		{ "ambient", runAmbient, false },  { "keystone", runKeystone, false }, { "surfaces", runSurfaces, false }, { "file", runFile, false },
		{ "dirty", runDirty, false },      { "resize", runResize, false },   { "state", runState, false },       { "cues", runCues, true },
		{ "names", runNames, true },
	};
	return list;
}

bool isOffline( const std::string& flag )
{
	for( const CheckEntry& c : checks() )
		if( flag == c.flag )
			return c.offline;
	return false;
}

//===========================================================================
// --negative
//===========================================================================
int runNegative( bool offlineOnly = false )
{
	struct Case
	{
		const char* name;
		CheckFn check;
		Perturb perturb;
		const char* what;
	};
	std::vector< Case > cases;
	auto add = [ & ]( const char* name, CheckFn fn, const char* what, std::function< void( Perturb& ) > set ) {
		Perturb p;
		set( p );
		cases.push_back( { name, fn, p, what } );
	};
	using namespace shaders;
	add( "units", runUnits, "W taken as H x height / width", []( Perturb& p ) { p.unitsAspect = true; } );
	add( "cues", runCues, "ramp every control between keys", []( Perturb& p ) { p.cuesRamp = true; } );
	add( "flat", runFlat, "the projector as a point lamp: no 1 / cos^3", []( Perturb& p ) { p.hooks = kHookNoDesignCos; } );
	add( "flat", runFlat, "every surface white", []( Perturb& p ) { p.hooks = kHookNoAlbedo; } );
	add( "parallax", runParallax, "the viewer's ray marched from the lens", []( Perturb& p ) { p.hooks = kHookViewerAtLens; } );
	add( "parallax", runParallax, "the relief seen straight on, not marched", []( Perturb& p ) { p.hooks = kHookFlatViewer; } );
	add( "lambert", runLambert, "light by the flat wall's angle, not the relief's", []( Perturb& p ) { p.hooks = kHookNoCosine; } );
	add( "lambert", runLambert, "the inverse square to the design screen, not the relief", []( Perturb& p ) { p.hooks = kHookNoInverseSq; } );
	add( "lambert", runLambert, "the projector as a point lamp: no 1 / cos^3", []( Perturb& p ) { p.hooks = kHookNoDesignCos; } );
	//Offset mapping is exact on a level floor, so the plateau cannot see it;
	//a slope can.
	add( "lambert", runLambert, "offset mapping: one sample under the pixel, no march", []( Perturb& p ) { p.hooks = kHookOffsetMap; } );
	add( "away", runAway, "a face turned away lit as if turned to the lens, and no shadow to stop it",
	     []( Perturb& p ) { p.hooks = kHookBackfaceLit | kHookNoShadow; } );
	add( "shadow", runShadow, "no march to the lens", []( Perturb& p ) { p.hooks = kHookNoShadow; } );
	add( "atlens", runAtLens, "the relief seen straight on, not marched", []( Perturb& p ) { p.hooks = kHookFlatViewer; } );
	add( "hotspot", runHotspot, "the highlight where the normal points at the lens", []( Perturb& p ) { p.hooks = kHookSpecAtLens; } );
	add( "ambient", runAmbient, "no room light", []( Perturb& p ) { p.hooks = kHookNoRoom; } );
	add( "keystone", runKeystone, "the projector as a point lamp: no 1 / cos^3", []( Perturb& p ) { p.hooks = kHookNoDesignCos; } );
	add( "dirty", runDirty, "the surface built once and never again", []( Perturb& p ) { p.hooks = kHookStaleSurface; } );
	add( "resize", runResize, "the surface built once and never again", []( Perturb& p ) { p.hooks = kHookStaleSurface; } );

	if( offlineOnly )
		cases.erase( std::remove_if( cases.begin(), cases.end(), []( const Case& c ) { return !isOffline( c.name ); } ), cases.end() );

	int unfalsifiable = 0;
	for( const Case& c : cases )
	{
		std::printf( "\n=== negative control: %s -- %s\n", c.name, c.what );
		const int before = g_failures;
		g_failures       = 0;
		c.check( c.perturb );
		const int observed = g_failures;
		g_failures         = before;
		if( observed > 0 )
			std::printf( "  ok    %s failed %d check%s, as it must\n", c.name, observed, observed == 1 ? "" : "s" );
		else
		{
			std::printf( "  FAIL  %s PASSED against a wrong model -- it cannot fail, so it is not a check\n", c.name );
			++unfalsifiable;
		}
	}
	std::printf( "\nnegative controls: %zu wrong models, %d of them undetected\n", cases.size(), unfalsifiable );
	std::printf( "\n  %s\n", unfalsifiable == 0 ? "PASS" : "FAIL" );
	return unfalsifiable == 0 ? 0 : 1;
}
} // namespace

//---------------------------------------------------------------------------
int main( int argc, char** argv )
{
	std::string outPath = "/tmp/throw.png", clipPath;
	std::vector< std::string > settings;
	int width = 1280, height = 720;
	std::string mode, scriptPath;
	int filmFrames = -1, outFrames = 2;
	bool sizeGiven = false;

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			std::printf( "twtest -- render Throw offline and measure the light that lands\n\n"
			             "  --out PATH        render the card and write a PNG (default /tmp/throw.png)\n"
			             "  --clip PATH       (with --out) a picture to throw instead of the card: any PNG or JPEG, scaled to --size\n"
			             "  --frames N        (with --out) frames rendered before the one written (default 2; nothing here moves with time)\n"
			             "  --size WxH        render size (default 1280x720)\n"
			             "  --set \"Name=V\"    set a parameter by its display name (an option by its name, Surface File by its path)."
			             " Repeatable.\n"
			             "  --list            every parameter and its default\n"
			             "  --pipe            raw RGBA frames in on stdin, out on stdout\n"
			             "  --film N          N frames of the card, raw RGBA on stdout\n"
			             "  --script PATH     cues for --pipe/--film: 'frame Name value'\n\n"
			             "  checks: see tools/verify.sh;  --negative   --bench\n"
			             "  --offline         the checks and negative controls that need no GL context (CI)\n" );
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--clip" && hasNext )
			clipPath = argv[ ++i ];
		else if( argument == "--frames" && hasNext )
			outFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		else if( argument == "--pipe" )
			mode = "pipe";
		else if( argument == "--film" && hasNext )
		{
			mode       = "film";
			filmFrames = std::max( 1, std::atoi( argv[ ++i ] ) );
		}
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--list" )
			mode = "list";
		else if( argument == "--size" && hasNext )
		{
			const std::string value = argv[ ++i ];
			const size_t cross      = value.find( 'x' );
			if( cross != std::string::npos )
			{
				width  = std::atoi( value.substr( 0, cross ).c_str() );
				height = std::atoi( value.substr( cross + 1 ).c_str() );
			}
			sizeGiven = true;
		}
		else if( argument.rfind( "--", 0 ) == 0 )
			mode = argument.substr( 2 );
		else
		{
			std::fprintf( stderr, "unknown argument '%s' (try --help)\n", argument.c_str() );
			return 2;
		}
	}
	if( width <= 0 || height <= 0 )
	{
		std::fprintf( stderr, "--size must be positive\n" );
		return 2;
	}
	//A check given --size runs at that raster alone (the software pass).
	if( sizeGiven )
		kRasters = { { width, height } };

	if( mode == "list" )
	{
		ThrowPlugin plugin;
		std::printf( "%-3s %-18s %-9s %s\n", "id", "name", "kind", "default" );
		for( const NamedParameter& parameter : listParameters( plugin ) )
			std::printf( "%-3u %-18s %-9s %.4f\n", parameter.index, parameter.name.c_str(), kindName( parameter.type ), parameter.value );
		return 0;
	}

	//A reader that hangs up must end --pipe/--film with exit 1 and a message,
	//not SIGPIPE's silent 141: ignored here, the write fails with EPIPE.
	std::signal( SIGPIPE, SIG_IGN );

	if( mode == "offline" )
	{
		int failed = 0;
		for( const CheckEntry& check : checks() )
			if( check.offline )
				failed |= check.run( Perturb {} );
		failed |= runNegative( true );
		std::printf( "\n  offline: the checks that need no GL context. The picture checks were NOT run --\n"
		             "  tools/verify.sh runs them against a real driver, at 320x180 and above, and again on the software renderer.\n"
		             "\n  %s\n",
		             failed == 0 ? "PASS" : "FAIL" );
		return failed == 0 ? 0 : 1;
	}
	for( const CheckEntry& check : checks() )
		if( mode == check.flag && check.offline )
			return check.run( Perturb {} );

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL 4.1 core context\n" );
		return 1;
	}

	int result = 0;
	bool ran   = false;
	for( const CheckEntry& check : checks() )
		if( mode == check.flag )
		{
			result = check.run( Perturb {} );
			ran    = true;
		}

	if( ran )
		;
	else if( mode == "pipe" )
		result = runPipe( width, height, scriptPath, 0, true, settings );
	else if( mode == "film" )
		result = runPipe( width, height, scriptPath, filmFrames, false, settings );
	else if( mode == "negative" )
		result = runNegative();
	else if( mode == "bench" )
		result = runBench();
	else if( !mode.empty() )
	{
		std::fprintf( stderr, "unknown mode --%s (try --help)\n", mode.c_str() );
		result = 2;
	}
	else
	{
		Rig rig;
		const Floats clip = clipPath.empty() ? Floats() : clipCard( clipPath, width, height );
		if( !clipPath.empty() && clip.empty() )
		{
			std::fprintf( stderr, "--clip %s: %s\n", clipPath.c_str(), stbi_failure_reason() );
			result = 2;
		}
		else if( !rig.Init( width, height, clip.empty() ? nullptr : &clip ) )
			result = 1;
		else
		{
			for( const std::string& setting : settings )
			{
				std::string error;
				if( !applySetting( rig.plugin, setting, error ) )
				{
					std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
					return 2;
				}
			}
			if( !rig.Render( outFrames ) )
				result = 1;
			else if( writePng( outPath, width, height, rig.Output() ) )
				std::printf( "wrote %s -- %dx%d\n", outPath.c_str(), width, height );
			else
				result = 1;
		}
	}

	CGLSetCurrentContext( nullptr );
	CGLDestroyContext( context );
	return result;
}
