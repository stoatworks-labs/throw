#pragma once

#include "Controls.h"
#include "HeightFile.h"
#include "PassBuffer.h"
#include "Room.h"

#include <FFGLSDK.h>

#include <array>
#include <mutex>
#include <string>

// After FFGLSDK.h, which is where FFUInt32 comes from.
#include "StoatworksAboutParams.h"

namespace throwfx
{
/**
	Throw -- the clip thrown by a projector onto a real surface, as an FFGL
	effect.

	**The one idea.** A projector, not a texture map. The picture leaves a
	lens at a real place in a real room and lands on a relief at its own
	angle and distance; the light that lands is what the surface's colour
	sends on; and somebody standing somewhere else sees it. So the shading,
	the shadows, the parallax, the metal's hotspot and the grey where black
	should be are not effects layered on top: they are what that geometry and
	that light do. Brick eats blue because brick is red. The picture slides
	over a fold as the viewer walks because the fold is nearer to them than
	the wall.

	**Three passes** (`Shaders.h`): the surface, only when it changes; its
	occlusion, with it; and the projection, every frame. See AGENTS.md for
	the geometry, the light, every surface's real size and where it came
	from, and what is verified.
*/
class ThrowPlugin : public CFFGLPlugin
{
public:
	ThrowPlugin();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;

	/// The Surface File's path, and the About line, which must accept its own
	/// default: instantiateGL pushes every declared default back through the
	/// setters and deletes the instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;
	char* GetTextParameter( unsigned int index ) override;

	//--- test hooks. Read by twtest; the plugin's own operation never uses
	//--- them, and they are always 0 outside the harness.

	/// Negative-control hooks, a bitmask of `shaders::Hook`.
	void SetHooksForTest( int bits )
	{
		hooks = bits;
	}

	/// How many times the surface has been built since InitGL.
	int SurfaceBuildsForTest() const
	{
		return surfaceBuilds;
	}

	/// The room the last frame used.
	const Room& RoomForTest() const
	{
		return room;
	}

	/// The surface buffer (albedo, height) and the occlusion, for reading back.
	GLuint SurfaceTextureForTest() const
	{
		return surface.TextureID();
	}
	GLuint OcclusionTextureForTest() const
	{
		return occlusion.TextureID();
	}

	/// Whether the Surface File is loaded.
	bool FileLoadedForTest() const
	{
		return file.Loaded();
	}

private:
	bool buildSurface( int width, int height, Surface kind );

	ffglex::FFGLShader surfaceShader;
	ffglex::FFGLShader occlusionShader;
	ffglex::FFGLShader projectShader;
	ffglex::FFGLScreenQuad quad;

	PassBuffer surface;  ///< albedo (linear) and height (metres)
	PassBuffer occlusion;///< the room's light reaching each point
	/// One black texel for FileTexture when no file is loaded. The shader never
	/// reads it then, but a sampler with texture 0 behind it is "unloadable" to
	/// Apple's driver, which says so in the host's log.
	GLuint blank = 0;

	/// Everything the surface depends on. The surface is rebuilt only when
	/// this changes: on a still surface the frame costs the projection alone.
	struct SurfaceKey
	{
		std::array< float, PT_FILE_HOLDS + 1 > params {};
		int width = 0, height = 0;
		unsigned fileGeneration = 0;
		bool operator==( const SurfaceKey& o ) const
		{
			return params == o.params && width == o.width && height == o.height && fileGeneration == o.fileGeneration;
		}
	};
	SurfaceKey builtKey;
	bool built        = false;
	int surfaceBuilds = 0;

	HeightFile file;
	std::mutex fileMutex;     ///< the path arrives on the host's thread
	std::string filePath;     ///< as the host set it
	std::string fileReturn;   ///< GetTextParameter's buffer
	std::string fileLoadedPath;///< what `file` holds
	bool fileFailed      = false;
	int fileHolds        = -1;///< the reading `file`'s texture was built for
	unsigned fileGeneration = 0;

	Room room;
	int hooks = 0;

	/// Zero-initialised: the About block's ids are never stored to.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};

} // namespace throwfx
