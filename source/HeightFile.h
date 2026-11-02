#pragma once

#include <FFGLSDK.h>

#include <cstdint>
#include <string>
#include <vector>

namespace throwfx
{
/**
    The Surface File: the user's own height map or photograph, decoded once
    and held as a texture the surface pass samples.

    **Height** reads the grey code value as height, white at the front, black
    at the back, and leaves the surface white. **Photo** reads the colour as
    the albedo (sRGB, decoded to linear) and its code-value luminance as the
    height, stretched to the picture's own range, so a photograph of a wall
    gets the wall's darks as its recesses. Neither is a measurement of the
    real surface: it is the user's say-so, and AGENTS.md says so.

    Decoded with stb_image at 16 bits a channel (an 8-bit file is widened,
    a 16-bit PNG keeps its precision: a height map's steps are a terrace of
    highlights). The decoded file is kept, so changing File Holds does not
    read the disk again. Rows are flipped to GL's order, bottom first.

    Everything here runs on the GL thread inside ProcessOpenGL: the decode
    stalls the frame it happens in, once.
*/
class HeightFile
{
public:
	~HeightFile();

	/// Read and decode `path`. False, with the reason, if it will not: the
	/// plugin then shows Flat, which is the honest picture of no surface.
	bool Load( const std::string& path, std::string& reason );

	/// Build the texture for this reading of the file. Needs a GL context.
	bool Upload( bool photo );

	void Release();

	bool Loaded() const
	{
		return texture != 0;
	}
	GLuint Texture() const
	{
		return texture;
	}
	int Width() const
	{
		return width;
	}
	int Height() const
	{
		return height;
	}

private:
	std::vector< std::uint16_t > pixels;///< RGBA, bottom row first
	int width   = 0;
	int height  = 0;
	GLuint texture = 0;
};

} // namespace throwfx
