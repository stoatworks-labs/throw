#include "Throw.h"

#include "Diag.h"
#include "GLState.h"
#include "Shaders.h"

//FFGLSDK.h includes every other scoped binding and omits this one (SDK
//b1afaf9). The symptom without it is an unknown-type error on
//ScopedFBOBinding and nothing else.
#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <cmath>
#include <string>

using namespace ffglex;
using namespace throwfx;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< ThrowPlugin >,// Create method
	"TW01",                      // Plugin unique ID of maximum length 4.
	"SW Throw",                  // Plugin name
	2,                           // API major version number
	1,                           // API minor version number
	0,                           // Plugin major version number
	1,                           // Plugin minor version number
	FF_EFFECT,                   // Plugin type
	"Throws the clip from a projector onto a real surface: crumpled paper, brick, corrugated sheet, tread plate, drapes, weatherboard, tiles, or your own height map.\n\nIt is a projector, not a texture map. The light leaves a lens at a real place in the room and lands on the relief at its own angle and distance, so every face is lit by how squarely it meets the beam, each recess is shadowed by the edge in front of it, red brick eats the blue, metal throws back a hotspot of the lens, and the room's own light greys the blacks. Stand the viewer off to one side and the picture slides over the relief, because the relief is nearer to them than the wall.\n\nEverything is at its real size. Turn Depth up to see more of it.",// Plugin description
	"Throw FFGL effect"          // About
);

namespace
{
/// glGetString returns nullptr with no current context; a log line must never
/// be the thing that brings the host down.
std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}

/// FFGLShader::Set has no integer-vector overload.
void setInt2( const FFGLShader& shader, const char* name, int x, int y )
{
	glUniform2i( glGetUniformLocation( shader.GetGLID(), name ), x, y );
}

float f( double v )
{
	return static_cast< float >( v );
}

/// The surface in full float: heights are metres, differenced across a
/// texel into a normal, and a millimetre's bevel on a 2.5 m wall is a
/// fraction of half float's step.
constexpr GLint kSurfaceFormat = GL_RGBA32F;

/// The room's share: a fraction, smooth, and the room is 2% of the picture
/// at most defaults. Half float's 2^-11 is far below what it can show.
constexpr GLint kOcclusionFormat = GL_R16F;
} // namespace

//---------------------------------------------------------------------------
ThrowPlugin::ThrowPlugin()
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//---------------------------------------------------------------------
	// Declaration, from the table in Controls.cpp. Every ranged parameter is
	// a plain 0..1 float: SetParamInfo clamps a STANDARD default into 0..1
	// before a range could be attached (SDK b1afaf9). Seed and Pleats are
	// real integers, which SetParamInfo leaves alone.
	//---------------------------------------------------------------------
	for( unsigned int id = 0; id < PT_ABOUT_FIRST; ++id )
	{
		const ControlInfo& info = InfoOf( id );
		params[ id ]            = info.defaultValue;
		switch( info.kind )
		{
		case Kind::Standard: SetParamInfo( id, info.name, FF_TYPE_STANDARD, info.defaultValue ); break;
		case Kind::Integer:
			SetParamInfo( id, info.name, FF_TYPE_INTEGER, info.defaultValue );
			SetParamRange( id, info.low, info.high );
			break;
		case Kind::Option:
			SetOptionParamInfo( id, info.name, static_cast< unsigned int >( info.optionCount ), info.defaultValue );
			for( int i = 0; i < info.optionCount; ++i )
				SetParamElementInfo( id, static_cast< unsigned int >( i ), info.options[ i ], static_cast< float >( i ) );
			break;
		case Kind::File:
			SetFileParamInfo( id, info.name, { "png", "jpg", "jpeg", "tga", "bmp" }, "" );
			break;
		}
		SetParamGroup( id, info.group );
	}

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_COUNT; ++i )
		SetParamGroup( i, "About" );

	FFGLLog::LogToHost( "Created Throw effect" );
}

//---------------------------------------------------------------------------
FFResult ThrowPlugin::InitGL( const FFGLViewportStruct* vp )
{
	diag::init();
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR ) + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = std::string( shaders::kVersion ) + shaders::kQuadVertex;
	struct
	{
		FFGLShader* shader;
		shaders::Pass pass;
		const char* name;
	} const stages[] = {
		{ &surfaceShader, shaders::Pass::Surface, "surface" },
		{ &occlusionShader, shaders::Pass::Occlusion, "occlusion" },
		{ &projectShader, shaders::Pass::Project, "project" },
	};
	for( const auto& stage : stages )
	{
		const std::string fragment = shaders::Assemble( stage.pass );
		if( stage.shader->Compile( vertex.c_str(), fragment.c_str() ) )
			continue;
		//FF_FAIL is invisible to an operator: the effect simply does nothing.
		//This line is the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Throw: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}
	{
		const float black[ 4 ] = { 0.0f, 0.0f, 0.0f, 1.0f };
		glGenTextures( 1, &blank );
		glBindTexture( GL_TEXTURE_2D, blank );
		glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, 1, 1, 0, GL_RGBA, GL_FLOAT, black );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	built         = false;
	surfaceBuilds = 0;
	{
		//A context's textures went with it: read the file again.
		std::lock_guard< std::mutex > lock( fileMutex );
		fileLoadedPath.clear();
		fileFailed = false;
	}
	fileHolds = -1;

	diag::info( "initialised" );
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
bool ThrowPlugin::buildSurface( int width, int height, Surface kind )
{
	//Every allocation before anything binds a texture: FFGLFBO sizes its
	//colour texture under a scoped binding, and every ffglex Scoped*
	//binding CLEARS to 0 on exit.
	if( !surface.Ensure( width, height, kSurfaceFormat, PassBuffer::Sampling::Linear )
	    || !occlusion.Ensure( width, height, kOcclusionFormat, PassBuffer::Sampling::Linear ) )
	{
		diag::error( "could not allocate the surface at " + std::to_string( width ) + "x" + std::to_string( height ) );
		return false;
	}

	const float scale = f( ScaleFromParam( params[ PT_SCALE ] ) );
	{
		ScopedFBOBinding fbo( surface.GetGLID(), ScopedFBOBinding::RB_REVERT );
		surface.ResizeViewPort();
		ScopedShaderBinding shader( surfaceShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( file.Loaded() ? file.Texture() : blank );
		surfaceShader.Set( "FileTexture", 0 );
		surfaceShader.Set( "FileLoaded", file.Loaded() ? 1 : 0 );
		setInt2( surfaceShader, "Size", width, height );
		surfaceShader.Set( "Frame", f( room.W ), f( room.H ) );
		surfaceShader.Set( "SurfaceIndex", static_cast< int >( kind ) );
		surfaceShader.Set( "Scale", scale );
		surfaceShader.Set( "DepthScale", f( DepthFromParam( params[ PT_DEPTH ] ) ) );
		surfaceShader.Set( "Relief", f( room.relief ) );
		surfaceShader.Set( "Seed", static_cast< int >( std::clamp( std::lround( params[ PT_SEED ] ), 0L, 99L ) ) );
		surfaceShader.Set( "PaperWeights", f( room.paperWeights[ 0 ] ), f( room.paperWeights[ 1 ] ), f( room.paperWeights[ 2 ] ),
		                   f( room.paperWeights[ 3 ] ) );
		surfaceShader.Set( "BondIndex", OptionIndex( params[ PT_BOND ], static_cast< int >( Bond::Count ) ) );
		surfaceShader.Set( "Mortar", f( MortarFromParam( params[ PT_MORTAR ] ) ) );
		surfaceShader.Set( "ProfileIndex", OptionIndex( params[ PT_PROFILE ], static_cast< int >( Profile::Count ) ) );
		surfaceShader.Set( "Pitch", f( PitchFromParam( params[ PT_PITCH ] ) ) );
		surfaceShader.Set( "PatternIndex", OptionIndex( params[ PT_PATTERN ], static_cast< int >( Pattern::Count ) ) );
		surfaceShader.Set( "PleatSpacing", f( room.pleatSpacing ) );
		surfaceShader.Set( "Sag", std::clamp( params[ PT_SAG ], 0.0f, 1.0f ) );
		quad.Draw();
	}
	{
		ScopedFBOBinding fbo( occlusion.GetGLID(), ScopedFBOBinding::RB_REVERT );
		occlusion.ResizeViewPort();
		ScopedShaderBinding shader( occlusionShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( surface.TextureID() );
		occlusionShader.Set( "Surface", 0 );
		setInt2( occlusionShader, "Size", width, height );
		occlusionShader.Set( "Frame", f( room.W ), f( room.H ) );
		occlusionShader.Set( "Relief", f( room.relief ) );
		quad.Draw();
	}
	++surfaceBuilds;
	return true;
}

//---------------------------------------------------------------------------
FFResult ThrowPlugin::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& picture = *pGL->inputTextures[ 0 ];
	if( picture.Width == 0 || picture.Height == 0 )
		return FF_FAIL;

	ScopedGLState state;
	glDisable( GL_BLEND );
	//The host's viewport, before anything of ours changes it:
	//ScopedFBOBinding restores the framebuffer binding and only that.
	const GLint* hostViewport = state.saved.viewport;

	const int width  = static_cast< int >( picture.Width );
	const int height = static_cast< int >( picture.Height );
	const auto kind  = static_cast< Surface >( OptionIndex( params[ PT_SURFACE ], static_cast< int >( Surface::Count ) ) );

	//---------------------------------------------------------------------
	// The file. Read when it is chosen, not when its path arrives: a
	// composition restores the path whether or not File is the surface.
	//---------------------------------------------------------------------
	if( kind == Surface::File )
	{
		std::string path;
		bool failed = false;
		{
			std::lock_guard< std::mutex > lock( fileMutex );
			path   = filePath;
			failed = fileFailed && path == fileLoadedPath;
		}
		if( !failed && path != fileLoadedPath )
		{
			std::string reason;
			bool ok = false;
			if( !path.empty() )
			{
				ok = file.Load( path, reason );
				if( !ok )
					diag::warn( "Surface File will not load: " + path + " (" + reason + "); showing Flat" );
			}
			if( !ok )
				file.Release();
			fileHolds = -1;
			++fileGeneration;
			std::lock_guard< std::mutex > lock( fileMutex );
			fileLoadedPath = path;
			fileFailed     = !ok && !path.empty();
		}
		const int holds = OptionIndex( params[ PT_FILE_HOLDS ], static_cast< int >( FileHolds::Count ) );
		if( !fileFailed && !fileLoadedPath.empty() && holds != fileHolds )
		{
			if( !file.Upload( holds == static_cast< int >( FileHolds::Photo ) ) )
				diag::warn( "the Surface File would not upload" );
			fileHolds = holds;
			++fileGeneration;
		}
	}

	room = MakeRoom( params, width, height );
	//No file, nothing behind the front plane.
	if( kind == Surface::File && !file.Loaded() )
		room.relief = 0.0;

	SurfaceKey key;
	std::copy( params, params + key.params.size(), key.params.begin() );
	key.width          = width;
	key.height         = height;
	key.fileGeneration = fileGeneration;
	const bool stale = ( hooks & shaders::kHookStaleSurface ) != 0 && built && surface.IsValid();
	if( !stale && ( !built || !( key == builtKey ) || !surface.IsValid() ) )
	{
		if( !buildSurface( width, height, kind ) )
			return FF_FAIL;
		builtKey = key;
		built    = true;
	}

	//---------------------------------------------------------------------
	// The projection, onto the host's buffer.
	//---------------------------------------------------------------------
	glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
	glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );
	{
		ScopedShaderBinding shader( projectShader.GetGLID() );
		bindUnit( 0, picture.Handle );
		bindUnit( 1, surface.TextureID() );
		bindUnit( 2, occlusion.TextureID() );

		const FFGLTexCoords maxUV = GetMaxGLTexCoords( picture );
		projectShader.Set( "InputTexture", 0 );
		projectShader.Set( "Surface", 1 );
		projectShader.Set( "Occlusion", 2 );
		projectShader.Set( "MaxUV", maxUV.s, maxUV.t );
		setInt2( projectShader, "Size", width, height );
		setInt2( projectShader, "ViewOrigin", hostViewport[ 0 ], hostViewport[ 1 ] );
		setInt2( projectShader, "ViewSize", std::max( hostViewport[ 2 ], 1 ), std::max( hostViewport[ 3 ], 1 ) );
		projectShader.Set( "Frame", f( room.W ), f( room.H ) );
		projectShader.Set( "Relief", f( room.relief ) );
		projectShader.Set( "Lens", f( room.lens.x ), f( room.lens.y ), f( room.lens.z ) );
		projectShader.Set( "Eye", f( room.eye.x ), f( room.eye.y ), f( room.eye.z ) );
		projectShader.Set( "AimIndex", static_cast< int >( room.aim ) );
		projectShader.Set( "Axis", f( room.axis.x ), f( room.axis.y ), f( room.axis.z ) );
		projectShader.Set( "Right", f( room.right.x ), f( room.right.y ), f( room.right.z ) );
		projectShader.Set( "Up", f( room.up.x ), f( room.up.y ), f( room.up.z ) );
		projectShader.Set( "Design", f( room.design ) );
		projectShader.Set( "BlackLevel", f( room.blackLevel ) );
		projectShader.Set( "RoomRatio", f( room.roomRatio ) );
		projectShader.Set( "Gain", f( room.gain ) );
		projectShader.Set( "GlazeIndex", static_cast< int >( room.glaze ) );
		projectShader.Set( "GlazeFloor", f( room.glazeFloor ) );
		projectShader.Set( "F0", f( room.f0[ 0 ] ), f( room.f0[ 1 ] ), f( room.f0[ 2 ] ) );
		projectShader.Set( "Alpha", f( room.alpha ) );
		projectShader.Set( "ViewIndex", OptionIndex( params[ PT_VIEW ], static_cast< int >( View::Count ) ) );
		projectShader.Set( "MixAmount", std::clamp( params[ PT_MIX ], 0.0f, 1.0f ) );
		projectShader.Set( "Hooks", hooks );
		quad.Draw();
		unbindTextureUnits( 3 );
	}

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult ThrowPlugin::DeInitGL()
{
	surfaceShader.FreeGLResources();
	occlusionShader.FreeGLResources();
	projectShader.FreeGLResources();
	quad.Release();

	surface.Destroy();
	occlusion.Destroy();
	file.Release();
	if( blank != 0 )
		glDeleteTextures( 1, &blank );
	blank = 0;
	built = false;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult ThrowPlugin::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// The About buttons open a browser and store nothing.
	if( index >= PT_ABOUT_FIRST )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	// A path, not a number.
	if( index == PT_FILE )
		return FF_SUCCESS;

	params[ index ] = value;
	return FF_SUCCESS;
}

float ThrowPlugin::GetFloatParameter( unsigned int index )
{
	if( index >= PT_ABOUT_FIRST )
		return 0.0f;
	return params[ index ];
}

FFResult ThrowPlugin::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only; it has to say so
	// successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;
	if( index == PT_FILE )
	{
		std::lock_guard< std::mutex > lock( fileMutex );
		filePath = value != nullptr ? value : "";
		return FF_SUCCESS;
	}
	return CFFGLPlugin::SetTextParameter( index, value );
}

char* ThrowPlugin::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}
	if( index == PT_FILE )
	{
		std::lock_guard< std::mutex > lock( fileMutex );
		fileReturn = filePath;
		return const_cast< char* >( fileReturn.c_str() );
	}
	return CFFGLPlugin::GetTextParameter( index );
}
