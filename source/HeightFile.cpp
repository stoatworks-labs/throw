#include "HeightFile.h"

#include <algorithm>
#include <cmath>

//stb_image's one implementation, here and nowhere else. The formats a height
//map or a photograph arrives in; UTF-8 paths on Windows.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_TGA
#define STBI_ONLY_BMP
#define STBI_WINDOWS_UTF8
#include <stb_image.h>

namespace throwfx
{
namespace
{
double decode( double code )
{
	return code <= 0.04045 ? code / 12.92 : std::pow( ( code + 0.055 ) / 1.055, 2.4 );
}
} // namespace

HeightFile::~HeightFile()
{
	//The context may already be gone; the plugin releases in DeInitGL.
	pixels.clear();
}

bool HeightFile::Load( const std::string& path, std::string& reason )
{
	pixels.clear();
	width = height = 0;

	int w = 0, h = 0, channels = 0;
	stbi_us* data = stbi_load_16( path.c_str(), &w, &h, &channels, 4 );
	if( data == nullptr )
	{
		const char* why = stbi_failure_reason();
		reason          = why != nullptr ? why : "unknown";
		return false;
	}
	GLint most = 0;
	glGetIntegerv( GL_MAX_TEXTURE_SIZE, &most );
	if( most > 0 && ( w > most || h > most ) )
	{
		stbi_image_free( data );
		reason = std::to_string( w ) + "x" + std::to_string( h ) + " is larger than this GPU's " + std::to_string( most ) + " texels";
		return false;
	}

	width  = w;
	height = h;
	pixels.resize( static_cast< size_t >( w ) * static_cast< size_t >( h ) * 4u );
	const size_t row = static_cast< size_t >( w ) * 4u;
	for( int y = 0; y < h; ++y )
		std::copy( data + static_cast< size_t >( h - 1 - y ) * row, data + static_cast< size_t >( h - y ) * row,
		           pixels.begin() + static_cast< std::ptrdiff_t >( static_cast< size_t >( y ) * row ) );
	stbi_image_free( data );
	return true;
}

bool HeightFile::Upload( bool photo )
{
	if( pixels.empty() )
		return false;

	const size_t count = static_cast< size_t >( width ) * static_cast< size_t >( height );
	std::vector< float > texels( count * 4u );

	//Luminance from the code values, Rec. 709's weights: a photograph's
	//darks as its recesses, measured the way the eye ranks them.
	double low = 1.0, high = 0.0;
	if( photo )
		for( size_t i = 0; i < count; ++i )
		{
			const std::uint16_t* p = &pixels[ i * 4u ];
			const double y         = ( 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ] ) / 65535.0;
			low                    = std::min( low, y );
			high                   = std::max( high, y );
		}
	const double span = high - low;

	for( size_t i = 0; i < count; ++i )
	{
		const std::uint16_t* p = &pixels[ i * 4u ];
		float* t               = &texels[ i * 4u ];
		if( photo )
		{
			const double y = ( 0.2126 * p[ 0 ] + 0.7152 * p[ 1 ] + 0.0722 * p[ 2 ] ) / 65535.0;
			for( int c = 0; c < 3; ++c )
				t[ c ] = static_cast< float >( decode( p[ c ] / 65535.0 ) );
			t[ 3 ] = span > 0.0 ? static_cast< float >( ( y - low ) / span ) : 1.0f;
		}
		else
		{
			//A grey height map: the red channel is the grey. A colour file read
			//as height takes its red, which is what the user will see in the
			//Height view and can correct.
			t[ 0 ] = t[ 1 ] = t[ 2 ] = 1.0f;
			t[ 3 ]                   = static_cast< float >( p[ 0 ] / 65535.0 );
		}
	}

	//Somebody else's error is not ours to report.
	for( int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i )
	{
	}
	if( texture == 0 )
		glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, width, height, 0, GL_RGBA, GL_FLOAT, texels.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	//Below Scale 1 the file tiles; mirrored, so it tiles without a seam.
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_MIRRORED_REPEAT );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return glGetError() == GL_NO_ERROR;
}

void HeightFile::Release()
{
	if( texture != 0 )
		glDeleteTextures( 1, &texture );
	texture = 0;
}

} // namespace throwfx
