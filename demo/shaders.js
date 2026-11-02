// GENERATED from source/Shaders.cpp by demo/tools/check_shaders.py --write.
// Do not edit: tools/verify.sh fails if a character of this differs from the
// plugin's. The one escape allowed is \` for a backtick inside a comment.
export const VERSION = '#version 410 core\n';

// kQuadVertex, source/Shaders.cpp
export const QUAD_VERTEX = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv          = vUV;
}
`;

// kCommon, source/Shaders.cpp
export const COMMON = `
//Shaders.h's Hook and kMaxSteps, which this text must repeat.
const int kHookViewerAtLens = 1;
const int kHookNoCosine     = 2;
const int kHookNoInverseSq  = 4;
const int kHookNoShadow     = 8;
const int kHookNoAlbedo     = 16;
const int kHookBackfaceLit  = 32;
const int kHookSpecAtLens   = 64;
const int kHookNoRoom       = 128;
const int kHookFlatViewer   = 256;
const int kHookNoDesignCos  = 512;
const int kHookOffsetMap    = 2048;
const int kMaxSteps         = 256;

const float kPi       = 3.14159265358979324;
const float kRootHalf = 0.70710678118654752;

//sRGB, both ways. Light adds and multiplies in linear light.
vec3 toLinear( vec3 code )
{
	vec3 c = max( code, vec3( 0.0 ) );
	return mix( c / 12.92, pow( ( c + 0.055 ) / 1.055, vec3( 2.4 ) ), step( vec3( 0.04045 ), c ) );
}
vec3 toCode( vec3 light )
{
	vec3 l = max( light, vec3( 0.0 ) );
	return mix( l * 12.92, 1.055 * pow( l, vec3( 1.0 / 2.4 ) ) - 0.055, step( vec3( 0.0031308 ), l ) );
}

//mix(), exact at its ends. GLSL does not promise mix( x, y, 1.0 ) == y:
//Apple's software renderer's is a float's last bit off it (slitscan).
vec4 blend( vec4 x, vec4 y, float a )
{
	return a <= 0.0 ? x : a >= 1.0 ? y : mix( x, y, a );
}

//PCG's output mix: exact in 32 bits, the same on every GPU.
uint pcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word  = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}

//[0, 1), 24 bits. The offset keeps a negative cell out of int-to-uint.
float hash01( ivec2 cell, uint salt )
{
	uint a = uint( cell.x + 1048576 );
	uint b = uint( cell.y + 1048576 );
	return float( pcg( a ^ pcg( b ^ pcg( salt ) ) ) >> 8u ) * ( 1.0 / 16777216.0 );
}

//Smooth value noise, [0, 1). (Not noise2: that name is a built-in.)
float valueNoise( vec2 at, uint salt )
{
	vec2 i  = floor( at );
	vec2 f  = at - i;
	vec2 u  = f * f * ( 3.0 - 2.0 * f );
	ivec2 c = ivec2( i );
	float a = hash01( c, salt ), b = hash01( c + ivec2( 1, 0 ), salt );
	float d = hash01( c + ivec2( 0, 1 ), salt ), e = hash01( c + ivec2( 1, 1 ), salt );
	return mix( mix( a, b, u.x ), mix( d, e, u.x ), u.y );
}

//cos( 2 pi t ), t in turns, without the built-in: reduced to [0, pi/2],
//then Taylor to a^14, whose remainder there is under 1e-10.
float cosTurn( float t )
{
	float x = t - floor( t );
	x       = min( x, 1.0 - x );
	float flip = 1.0;
	if( x > 0.25 )
	{
		x    = 0.5 - x;
		flip = -1.0;
	}
	float a  = 6.28318530717958648 * x;
	float a2 = a * a;
	return flip * ( 1.0 + a2 * ( -0.5 + a2 * ( 1.0 / 24.0 + a2 * ( -1.0 / 720.0 + a2 * ( 1.0 / 40320.0
	       + a2 * ( -1.0 / 3628800.0 + a2 * ( 1.0 / 479001600.0 + a2 * ( -1.0 / 87178291200.0 ) ) ) ) ) ) ) );
}
`;

// kSurfaceFragment, source/Shaders.cpp
export const SURFACE_FRAGMENT = `
uniform ivec2 Size;
uniform vec2 Frame;         //W, H: the frame on the wall, metres
uniform int SurfaceIndex;   //Controls.h's Surface
uniform float Scale;
uniform float DepthScale;   //the Depth control
uniform float Relief;       //R: the deepest this surface goes
uniform int Seed;
uniform vec4 PaperWeights;  //each generation's arrival
uniform int BondIndex;
uniform float Mortar;       //metres at Scale 1
uniform int ProfileIndex;
uniform float Pitch;        //metres at Scale 1
uniform int PatternIndex;
uniform float PleatSpacing; //metres
uniform float Sag;
uniform sampler2D FileTexture;
uniform int FileLoaded;

in vec2 uv;
out vec4 fragColor;

uint salt( int n )
{
	return uint( Seed ) * 16u + uint( n );
}

//--- Paper ------------------------------------------------------------------
//One generation of crumpled facets: a jittered grid of crease junctions,
//each quad split along a hashed diagonal, every corner at a hashed height.
//Every triangle is flat and every crease straight.
float facetHeight( vec2 P, float cell, float amp, uint s )
{
	vec2 g     = P / cell - 0.5;
	ivec2 base = ivec2( floor( g ) ) - 1;
	vec2 corner[ 16 ];
	float lift[ 16 ];
	for( int j = 0; j < 4; ++j )
		for( int i = 0; i < 4; ++i )
		{
			ivec2 c             = base + ivec2( i, j );
			corner[ j * 4 + i ] = ( vec2( c ) + 0.25 + 0.5 * vec2( hash01( c, s ), hash01( c, s + 1u ) ) ) * cell;
			lift[ j * 4 + i ]   = ( 2.0 * hash01( c, s + 2u ) - 1.0 ) * amp;
		}
	for( int qj = 0; qj < 3; ++qj )
		for( int qi = 0; qi < 3; ++qi )
		{
			int k00    = qj * 4 + qi;
			bool other = hash01( base + ivec2( qi, qj ), s + 3u ) < 0.5;
			ivec3 t0   = other ? ivec3( k00, k00 + 1, k00 + 5 ) : ivec3( k00, k00 + 1, k00 + 4 );
			ivec3 t1   = other ? ivec3( k00, k00 + 5, k00 + 4 ) : ivec3( k00 + 1, k00 + 5, k00 + 4 );
			for( int t = 0; t < 2; ++t )
			{
				ivec3 tri = t == 0 ? t0 : t1;
				vec2 a = corner[ tri.x ], b = corner[ tri.y ], c = corner[ tri.z ];
				vec2 v0 = b - a, v1 = c - a, v2 = P - a;
				float den = v0.x * v1.y - v1.x * v0.y;
				float wb  = ( v2.x * v1.y - v1.x * v2.y ) / den;
				float wc  = ( v0.x * v2.y - v2.x * v0.y ) / den;
				float wa  = 1.0 - wb - wc;
				if( min( wa, min( wb, wc ) ) >= -1e-5 )
					return wa * lift[ tri.x ] + wb * lift[ tri.y ] + wc * lift[ tri.z ];
			}
		}
	return lift[ 5 ];
}

vec4 paperAt( vec2 P )
{
	float cell = 0.12 * Scale, amp = 0.015 * Scale * DepthScale;
	float h = 0.0, top = 0.0;
	for( int l = 0; l < 4; ++l )
	{
		float w = PaperWeights[ l ];
		if( w > 0.0 )
		{
			h += w * facetHeight( P, cell, amp, salt( 4 * l ) );
			top += w * amp;
		}
		cell *= 0.45;
		amp *= 0.45;
	}
	return vec4( 0.80, 0.79, 0.76, h - top );
}

//--- Brick ------------------------------------------------------------------
//UK metric: 215 x 65 mm, headers 102.5, Mortar joints raked 8 mm deep, the
//arrises rounded 2 mm. Each brick set back a little, tilted a little, and
//fired a little differently.
vec4 brickAt( vec2 P )
{
	float k      = Scale;
	float bh     = 0.065 * k, bl = 0.215 * k, hl = 0.1025 * k, jt = Mortar * k;
	float course = bh + jt;
	float row    = floor( P.y / course );
	float yIn    = P.y - row * course;
	bool odd     = ( int( row ) & 1 ) != 0;
	float smod = bl + jt, hmod = hl + jt;
	float len = bl, idx = 0.0, xIn = 0.0;
	if( BondIndex == 2 )
	{
		//Flemish: header and stretcher in every course, each header centred
		//on the stretcher below.
		float pair = smod + hmod;
		float x    = P.x - ( odd ? 0.5 * pair : 0.0 );
		float n    = floor( x / pair );
		float xr   = x - n * pair;
		if( xr < smod )
		{
			xIn = xr;
			idx = 2.0 * n;
		}
		else
		{
			xIn = xr - smod;
			len = hl;
			idx = 2.0 * n + 1.0;
		}
	}
	else
	{
		float unit = smod, offset = 0.0;
		if( BondIndex == 0 && odd )
			offset = 0.5 * smod;
		else if( BondIndex == 1 && odd )
		{
			//English: a course of headers, a quarter brick along.
			unit   = hmod;
			len    = hl;
			offset = 0.25 * smod;
		}
		float x = P.x - offset;
		idx     = floor( x / unit );
		xIn     = x - idx * unit;
	}
	ivec2 id     = ivec2( int( idx ), int( row ) );
	float recess = 0.008 * k * DepthScale;
	if( xIn < len && yIn < bh )
	{
		float d     = min( min( xIn, len - xIn ), min( yIn, bh - yIn ) );
		float r     = 0.002 * k;
		float e     = max( r - d, 0.0 );
		float arris = r - sqrt( max( r * r - e * e, 0.0 ) );
		float back  = ( 0.0013 + 0.001 * hash01( id, salt( 1 ) ) ) * k;
		vec2 tilt   = ( vec2( hash01( id, salt( 2 ) ), hash01( id, salt( 3 ) ) ) - 0.5 ) * 0.012;
		float grain = 0.0004 * k * ( 2.0 * valueNoise( P / ( 0.005 * k ), salt( 4 ) ) - 1.0 );
		float face  = -back + dot( tilt, vec2( xIn - 0.5 * len, yIn - 0.5 * bh ) ) + grain;
		float tone  = 0.75 + 0.45 * hash01( id, salt( 5 ) );
		vec3 fired  = mix( vec3( 0.40, 0.13, 0.07 ), vec3( 0.30, 0.14, 0.09 ), hash01( id, salt( 6 ) ) );
		float grit  = 0.92 + 0.16 * valueNoise( P / ( 0.002 * k ), salt( 7 ) );
		return vec4( fired * tone * grit, max( ( face - arris ) * DepthScale, -recess ) );
	}
	float grit = 0.9 + 0.2 * valueNoise( P / ( 0.0015 * k ), salt( 8 ) );
	return vec4( vec3( 0.25, 0.24, 0.22 ) * grit, -recess );
}

//--- Corrugated -------------------------------------------------------------
//Ribs running up the wall, a crest at x = 0. Sine: corrugated iron. Box:
//crests 15% of the pitch, webs 25% each, valleys 35%.
vec4 corrugatedAt( vec2 P )
{
	float x = P.x / ( Pitch * Scale );
	float h;
	if( ProfileIndex == 0 )
		h = -Relief * ( 0.5 - 0.5 * cosTurn( x ) );
	else
	{
		float u = abs( fract( x + 0.5 ) - 0.5 );
		h       = -Relief * clamp( ( u - 0.075 ) / 0.25, 0.0, 1.0 );
	}
	float spangle = 0.85 + 0.3 * valueNoise( P / ( 0.012 * Scale ), salt( 9 ) );
	return vec4( vec3( 0.17, 0.18, 0.19 ) * spangle, h );
}

//--- Plate ------------------------------------------------------------------
float shoulder( float s )
{
	s = clamp( s, 0.0, 1.0 );
	return 1.0 - ( 1.0 - s ) * ( 1.0 - s );
}

//30 mm cells, each lug turned the other way from its neighbours'.
vec4 plateAt( vec2 P )
{
	vec2 g    = P / ( 0.030 * Scale );
	ivec2 ci  = ivec2( floor( g ) );
	vec2 l    = g - floor( g ) - 0.5;
	bool turn = ( ( ci.x + ci.y ) & 1 ) != 0;
	float lug;
	if( PatternIndex == 0 )
	{
		//Diamond: a lozenge 0.8 of a cell tip to tip, 0.2 across, at +-45.
		vec2 q  = kRootHalf * ( turn ? vec2( l.x + l.y, l.y - l.x ) : vec2( l.x - l.y, l.x + l.y ) );
		float t = 1.0 - ( abs( q.x ) / 0.40 + abs( q.y ) / 0.10 );
		lug     = shoulder( t / 0.35 );
	}
	else if( PatternIndex == 1 )
	{
		//Five-Bar: five round-ended bars a cell, across in one, along in the next.
		vec2 q    = turn ? l.yx : l;
		float bar = clamp( floor( q.y / 0.18 + 0.5 ), -2.0, 2.0 );
		vec2 d    = vec2( max( abs( q.x ) - 0.33, 0.0 ), q.y - bar * 0.18 );
		lug       = shoulder( ( 1.0 - length( d ) / 0.05 ) / 0.6 );
	}
	else
	{
		//Crosshatch: ridges along both diagonals.
		vec2 d  = 0.5 * vec2( g.x + g.y, g.x - g.y );
		float r = min( abs( fract( d.x ) - 0.5 ), abs( fract( d.y ) - 0.5 ) );
		lug     = shoulder( ( 1.0 - r / 0.08 ) / 0.5 );
	}
	float wear = 0.9 + 0.2 * valueNoise( P / ( 0.01 * Scale ), salt( 10 ) );
	return vec4( vec3( 0.10 ) * wear, -Relief * ( 1.0 - lug ) );
}

//--- Drapes -----------------------------------------------------------------
//Folds PleatSpacing apart, a crest at x = 0. Gathered at the top; Sag lets
//them wander and deepen towards the floor.
vec4 drapesAt( vec2 P )
{
	float s      = PleatSpacing;
	float yTop   = clamp( 0.5 - P.y / Frame.y, 0.0, 1.0 );
	float wander = 0.6 * ( 2.0 * valueNoise( vec2( P.x / ( 2.0 * s ), P.y / ( 0.4 * Frame.y ) ), salt( 11 ) ) - 1.0 );
	float phase  = P.x / s + Sag * wander * pow( yTop, 1.5 );
	float fold   = clamp( 0.5 - 0.5 * cosTurn( phase ), 0.0, 1.0 );
	float deep   = ( 1.0 + 0.5 * Sag * yTop ) / ( 1.0 + 0.5 * Sag );
	float weave  = 0.97 + 0.06 * valueNoise( P / 0.003, salt( 12 ) );
	return vec4( vec3( 0.70, 0.66, 0.56 ) * weave, -Relief * deep * pow( fold, 1.25 ) );
}

//--- Weatherboard -----------------------------------------------------------
//Each board's lip proud at its foot, its top tucked under the next one up.
vec4 weatherboardAt( vec2 P )
{
	float b     = 0.150 * Scale;
	float row   = floor( P.y / b );
	float t     = P.y / b - row;
	float tone  = 0.94 + 0.12 * hash01( ivec2( 0, int( row ) ), salt( 13 ) );
	float grain = 0.97 + 0.06 * valueNoise( vec2( P.x / 0.08, P.y / 0.002 ), salt( 14 ) );
	return vec4( vec3( 0.62, 0.63, 0.60 ) * tone * grain, -Relief * t );
}

//--- Tiles ------------------------------------------------------------------
//200 x 100 mm in running bond, 3 mm grout, a 6 mm bevel 3 mm deep.
vec4 tilesAt( vec2 P )
{
	float k   = Scale;
	float tl  = 0.200 * k, th = 0.100 * k, gw = 0.003 * k;
	float row = floor( P.y / ( th + gw ) );
	float yIn = P.y - row * ( th + gw );
	bool odd  = ( int( row ) & 1 ) != 0;
	float x   = P.x - ( odd ? 0.5 * ( tl + gw ) : 0.0 );
	float idx = floor( x / ( tl + gw ) );
	float xIn = x - idx * ( tl + gw );
	if( xIn < tl && yIn < th )
	{
		float d    = min( min( xIn, tl - xIn ), min( yIn, th - yIn ) );
		float e    = max( 1.0 - d / ( 0.006 * k ), 0.0 );
		float drop = 0.003 * k * DepthScale * ( 1.0 - sqrt( max( 1.0 - e * e, 0.0 ) ) );
		float tone = 0.97 + 0.06 * hash01( ivec2( int( idx ), int( row ) ), salt( 15 ) );
		return vec4( vec3( 0.78, 0.78, 0.76 ) * tone, -drop );
	}
	return vec4( 0.30, 0.29, 0.27, -Relief );
}

vec4 surfaceAt( vec2 P )
{
	if( SurfaceIndex == 1 )
		return paperAt( P );
	if( SurfaceIndex == 2 )
		return brickAt( P );
	if( SurfaceIndex == 3 )
		return corrugatedAt( P );
	if( SurfaceIndex == 4 )
		return plateAt( P );
	if( SurfaceIndex == 5 )
		return drapesAt( P );
	if( SurfaceIndex == 6 )
		return weatherboardAt( P );
	if( SurfaceIndex == 7 )
		return tilesAt( P );
	return vec4( 1.0, 1.0, 1.0, 0.0 );
}

//--- File -------------------------------------------------------------------
int mirrored( int k, int n )
{
	int period = 2 * n;
	k -= period * int( floor( float( k ) / float( period ) ) );
	return k < n ? k : period - 1 - k;
}

//The file's own bilinear, mirrored past its edges, in float. A filter's
//weights are 8-bit on a GPU, and Apple's software renderer's are not exact
//even at a texel's centre; at the frame's raster this has to be a copy.
vec4 fileAt( vec2 fragment )
{
	ivec2 n = textureSize( FileTexture, 0 );
	//In the file's texels, written so that at Scale 1 and the frame's raster
	//it is the fragment's own index, exactly.
	vec2 x  = ( fragment - 0.5 * vec2( Size ) ) * ( vec2( n ) / vec2( Size ) ) / Scale + 0.5 * vec2( n ) - 0.5;
	vec2 i0 = floor( x );
	vec2 f  = x - i0;
	ivec2 a = ivec2( mirrored( int( i0.x ), n.x ), mirrored( int( i0.y ), n.y ) );
	ivec2 b = ivec2( mirrored( int( i0.x ) + 1, n.x ), mirrored( int( i0.y ) + 1, n.y ) );
	vec4 lo = mix( texelFetch( FileTexture, a, 0 ), texelFetch( FileTexture, ivec2( b.x, a.y ), 0 ), f.x );
	vec4 hi = mix( texelFetch( FileTexture, ivec2( a.x, b.y ), 0 ), texelFetch( FileTexture, b, 0 ), f.x );
	return mix( lo, hi, f.y );
}

void main()
{
	vec2 at = gl_FragCoord.xy / vec2( Size );
	vec4 s  = vec4( 1.0, 1.0, 1.0, 0.0 );
	if( SurfaceIndex == 8 )
	{
		//The file fills the frame at Scale 1 and tiles, mirrored, below it. At
		//the frame's own raster its texels are these texels: a copy.
		if( FileLoaded != 0 )
		{
			vec4 f = fileAt( gl_FragCoord.xy );
			s      = vec4( f.rgb, -Relief * ( 1.0 - f.a ) );
		}
	}
	else if( SurfaceIndex != 0 )
	{
		//2x2 inside the texel: a joint narrower than a texel still shows.
		vec2 P  = ( at - 0.5 ) * Frame;
		vec2 tx = 0.25 * Frame / vec2( Size );
		s       = 0.25 * ( surfaceAt( P + vec2( -tx.x, -tx.y ) ) + surfaceAt( P + vec2( tx.x, -tx.y ) )
		             + surfaceAt( P + vec2( -tx.x, tx.y ) ) + surfaceAt( P + vec2( tx.x, tx.y ) ) );
	}
	fragColor = vec4( s.rgb, clamp( s.a, -Relief, 0.0 ) );
}
`;

// kOcclusionFragment, source/Shaders.cpp
export const OCCLUSION_FRAGMENT = `
uniform sampler2D Surface;
uniform ivec2 Size;
uniform vec2 Frame;
uniform float Relief;

in vec2 uv;
out vec4 fragColor;

const vec2 kDirections[ 8 ] = vec2[ 8 ]( vec2( 1.0, 0.0 ), vec2( kRootHalf, kRootHalf ), vec2( 0.0, 1.0 ),
                                         vec2( -kRootHalf, kRootHalf ), vec2( -1.0, 0.0 ), vec2( -kRootHalf, -kRootHalf ),
                                         vec2( 0.0, -1.0 ), vec2( kRootHalf, -kRootHalf ) );

float heightAt( vec2 texel )
{
	return texture( Surface, texel / vec2( Size ) ).a;
}

void main()
{
	if( Relief <= 0.0 )
	{
		fragColor = vec4( 1.0 );
		return;
	}
	vec2 at      = gl_FragCoord.xy;
	float metres = Frame.y / float( Size.y );
	float h0     = heightAt( at );
	vec2 slope   = vec2( heightAt( at + vec2( 1.0, 0.0 ) ) - heightAt( at - vec2( 1.0, 0.0 ) ),
                       heightAt( at + vec2( 0.0, 1.0 ) ) - heightAt( at - vec2( 0.0, 1.0 ) ) ) / ( 2.0 * metres );
	//Four times the relief, in metres: the same reach at every raster, so
	//the same wall at 720p and at 4K is occluded alike.
	float reach  = max( 4.0 * Relief / metres, 1.0 );
	float sum    = 0.0;
	for( int d = 0; d < 8; ++d )
	{
		vec2 dir    = kDirections[ d ];
		float along = dot( slope, dir );
		float rise  = 0.0;
		for( int k = 1; k <= 8; ++k )
		{
			float dist = reach * float( k ) / 8.0;
			vec2 q     = at + dir * dist;
			//Beyond the frame is unknown, not a held edge: no horizon there.
			if( any( lessThan( q, vec2( 0.0 ) ) ) || any( greaterThan( q, vec2( Size ) ) ) )
				break;
			rise = max( rise, ( heightAt( q ) - h0 ) / ( dist * metres ) - along );
		}
		//A plane's own rounding is not a horizon.
		rise = rise < 1e-4 ? 0.0 : rise;
		sum += 1.0 / ( 1.0 + rise * rise );
	}
	fragColor = vec4( sum / 8.0, 0.0, 0.0, 1.0 );
}
`;

// kProjectFragment, source/Shaders.cpp
export const PROJECT_FRAGMENT = `
uniform sampler2D InputTexture;
uniform vec2 MaxUV;
uniform sampler2D Surface;  //albedo, height
uniform sampler2D Occlusion;//the room's light reaching each point
uniform ivec2 Size;         //the surface's raster
uniform ivec2 ViewOrigin;   //the host's viewport
uniform ivec2 ViewSize;
uniform vec2 Frame;         //W, H
uniform float Relief;       //R
uniform vec3 Lens;          //the projector's lens
uniform vec3 Eye;           //the viewer
uniform int AimIndex;       //0 Lens Shift, 1 Pointed
uniform vec3 Axis;          //the projector's basis (Pointed)
uniform vec3 Right;
uniform vec3 Up;
uniform float Design;       //distance to the screen it lights evenly
uniform float BlackLevel;
uniform float RoomRatio;    //A: the room's lux over the projector's
uniform float Gain;         //Exposure
uniform int GlazeIndex;     //0 none, 1 all, 2 tiles
uniform float GlazeFloor;   //Tiles: deeper than this is grout
uniform vec3 F0;
uniform float Alpha;        //GGX alpha
uniform int ViewIndex;
uniform float MixAmount;
uniform int Hooks;

in vec2 uv;
out vec4 fragColor;

float heightAt( vec2 at )
{
	return texture( Surface, at ).a;
}

//The clip as light: linear, and premultiplied, so a transparent pixel sends
//nothing.
vec3 clipLight( vec2 at )
{
	vec4 c = texture( InputTexture, at * MaxUV );
	return toLinear( c.rgb ) * c.a;
}

//The surface's own bilinear, in float. A GPU's filter weights are 8-bit: a
//512th of the difference between texels, which on a 40-degree slope moves a
//normal's light by 1e-3. The march steps on the filter; the hit and the
//normal come from here.
float heightExact( vec2 at )
{
	vec2 x    = at * vec2( Size ) - 0.5;
	vec2 i0   = floor( x );
	vec2 f    = x - i0;
	ivec2 a   = clamp( ivec2( i0 ), ivec2( 0 ), Size - 1 );
	ivec2 b   = clamp( ivec2( i0 ) + 1, ivec2( 0 ), Size - 1 );
	float h00 = texelFetch( Surface, a, 0 ).a;
	float h10 = texelFetch( Surface, ivec2( b.x, a.y ), 0 ).a;
	float h01 = texelFetch( Surface, ivec2( a.x, b.y ), 0 ).a;
	float h11 = texelFetch( Surface, b, 0 ).a;
	return mix( mix( h00, h10, f.x ), mix( h01, h11, f.x ), f.y );
}

//The slope of the bilinear cell \`at\` is in, metres per metre: the surface
//itself, faceted at the texel. A point a ray reached from above faces the
//ray's source on it, by construction.
vec2 slopeExact( vec2 at, float texelM )
{
	vec2 x    = at * vec2( Size ) - 0.5;
	vec2 i0   = floor( x );
	vec2 f    = x - i0;
	ivec2 a   = clamp( ivec2( i0 ), ivec2( 0 ), Size - 1 );
	ivec2 b   = clamp( ivec2( i0 ) + 1, ivec2( 0 ), Size - 1 );
	float h00 = texelFetch( Surface, a, 0 ).a;
	float h10 = texelFetch( Surface, ivec2( b.x, a.y ), 0 ).a;
	float h01 = texelFetch( Surface, ivec2( a.x, b.y ), 0 ).a;
	float h11 = texelFetch( Surface, b, 0 ).a;
	return vec2( mix( h10 - h00, h11 - h01, f.y ), mix( h01 - h00, h11 - h10, f.x ) ) / texelM;
}

//Smooth: central differences two texels wide, so shading does not facet.
vec3 normalAt( vec2 at, float texelM )
{
	vec2 e     = 1.0 / vec2( Size );
	vec2 slope = vec2( heightExact( at + vec2( e.x, 0.0 ) ) - heightExact( at - vec2( e.x, 0.0 ) ),
	                   heightExact( at + vec2( 0.0, e.y ) ) - heightExact( at - vec2( 0.0, e.y ) ) ) / ( 2.0 * texelM );
	return normalize( vec3( -slope, 1.0 ) );
}

void main()
{
	vec2 uv0     = ( gl_FragCoord.xy - vec2( ViewOrigin ) ) / vec2( ViewSize );
	vec2 p       = ( uv0 - 0.5 ) * Frame;
	vec4 src     = texture( InputTexture, uv0 * MaxUV );
	float texelM = Frame.y / float( Size.y );

	//-----------------------------------------------------------------
	// 1. What the viewer sees. Its ray through p is at p - z * toward at
	//    height z; marched from the front plane down to -R, a step a texel,
	//    then bisected, then a secant: exact on a plane.
	//-----------------------------------------------------------------
	vec3 eye     = ( Hooks & kHookViewerAtLens ) != 0 ? Lens : Eye;
	vec2 toward  = ( p - eye.xy ) / eye.z;
	float zHit   = 0.0;
	float fa     = -heightAt( uv0 );
	if( ( Hooks & kHookFlatViewer ) != 0 )
	{
		zHit   = -fa;
		toward = vec2( 0.0 );
	}
	else if( ( Hooks & kHookOffsetMap ) != 0 )
		zHit = -fa;
	else if( fa > 0.0 )
	{
		int steps = int( clamp( ceil( 2.0 * Relief * length( toward ) / texelM ), 1.0, float( kMaxSteps ) ) );
		float za = 0.0, zb = -Relief, fb = 0.0;
		for( int i = 1; i <= steps; ++i )
		{
			float z = -Relief * float( i ) / float( steps );
			float f = z - heightAt( uv0 - z * toward / Frame );
			if( f <= 0.0 )
			{
				zb = z;
				fb = f;
				break;
			}
			za = z;
			fa = f;
		}
		fa = za - heightExact( uv0 - za * toward / Frame );
		fb = zb - heightExact( uv0 - zb * toward / Frame );
		for( int k = 0; k < 8; ++k )
		{
			float zm = 0.5 * ( za + zb );
			float fm = zm - heightExact( uv0 - zm * toward / Frame );
			if( fm <= 0.0 )
			{
				zb = zm;
				fb = fm;
			}
			else
			{
				za = zm;
				fa = fm;
			}
		}
		zHit = fa - fb > 0.0 ? za + fa * ( zb - za ) / ( fa - fb ) : zb;
	}
	vec2 shift = -zHit * toward;
	vec2 uvS   = uv0 + shift / Frame;
	vec3 S     = vec3( p + shift, zHit );
	vec3 n     = normalAt( uvS, texelM );
	//Smoothing rounds a crease, and can turn a point the eye sees away from
	//the eye. There the cell's own slope is the surface it saw.
	if( dot( n, eye - S ) <= 0.0 )
		n = normalize( vec3( -slopeExact( uvS, texelM ), 1.0 ) );

	//-----------------------------------------------------------------
	// 2. The projector ray that lands there, and what it brings.
	//-----------------------------------------------------------------
	vec3 toLens = Lens - S;
	float r     = length( toLens );
	vec3 w      = toLens / r;
	float cosA, ahead;
	vec2 quv;
	if( AimIndex == 0 )
	{
		//Square to the wall: the ray meets the front plane at
		//S + ( S - C ) S.z / ( C.z - S.z ), as an offset from p.
		cosA  = w.z;
		ahead = Lens.z - S.z;
		quv   = uv0 + ( shift + ( S.xy - Lens.xy ) * ( S.z / ( Lens.z - S.z ) ) ) / Frame;
	}
	else
	{
		vec3 a = S - Lens;
		ahead  = dot( a, Axis );
		cosA   = ahead / r;
		quv    = 0.5 + Design * vec2( dot( a, Right ), dot( a, Up ) ) / ( ahead * Frame );
	}
	bool inside = ahead > 0.0 && all( greaterThanEqual( quv, vec2( 0.0 ) ) ) && all( lessThanEqual( quv, vec2( 1.0 ) ) );

	//It was built to light its design screen evenly, so it sends 1 / cos^3
	//off axis; the relief takes that at its own angle and distance.
	float facing = dot( n, w );
	float lit    = ( Hooks & kHookBackfaceLit ) != 0 ? abs( facing ) : max( facing, 0.0 );
	if( ( Hooks & kHookNoCosine ) != 0 )
		lit = max( w.z, 0.0 );
	float reach2 = ( Hooks & kHookNoInverseSq ) != 0 ? Design * Design / ( cosA * cosA ) : r * r;
	float spread = ( Hooks & kHookNoDesignCos ) != 0 ? 1.0 : cosA * cosA * cosA;
	float g      = inside ? Design * Design * lit / ( reach2 * spread ) : 0.0;

	//-----------------------------------------------------------------
	// 3. Is anything in the way: the same height field, from S up to the
	//    front plane along the ray, two steps a texel. Within two texels of
	//    S the ray is near the surface it left, closer than the filter's
	//    8-bit weights can tell on a steep face, so there the heights are
	//    the exact bilinear.
	//-----------------------------------------------------------------
	float vis = 1.0;
	if( g > 0.0 && S.z < 0.0 && ( Hooks & kHookNoShadow ) == 0 )
	{
		vec2 run   = ( Lens.xy - S.xy ) * ( -S.z / ( Lens.z - S.z ) );
		int steps  = int( clamp( ceil( 2.0 * length( run ) / texelM ), 2.0, float( kMaxSteps ) ) );
		float near = 2.0 * texelM / max( length( run ), 1e-9 );
		float bias = 1e-3 * Relief;
		for( int i = 1; i < steps; ++i )
		{
			float t  = float( i ) / float( steps );
			vec2 q   = uvS + run * t / Frame;
			float hq = t < near ? heightExact( q ) : heightAt( q );
			if( hq > S.z * ( 1.0 - t ) + bias )
			{
				vis = 0.0;
				break;
			}
		}
	}

	//-----------------------------------------------------------------
	// 4. Light.
	//-----------------------------------------------------------------
	vec3 emitted  = inside ? vec3( BlackLevel ) + ( 1.0 - BlackLevel ) * clipLight( quv ) : vec3( 0.0 );
	vec3 albedo   = ( Hooks & kHookNoAlbedo ) != 0 ? vec3( 1.0 ) : texture( Surface, uvS ).rgb;
	float room    = ( Hooks & kHookNoRoom ) != 0 ? 0.0 : RoomRatio * texture( Occlusion, uvS ).r;
	vec3 incident = g * vis * emitted;
	vec3 light    = albedo * ( incident + room );
	float glaze   = GlazeIndex == 0 ? 0.0 : GlazeIndex == 1 ? 1.0 : ( S.z > -GlazeFloor ? 1.0 : 0.0 );
	if( glaze > 0.0 )
	{
		//The lens is a small bright source: GGX, height-correlated Smith,
		//Schlick. f times pi, against a white matte screen's 1 / pi.
		vec3 v      = normalize( Eye - S );
		vec3 hv     = normalize( w + v );
		float nl    = max( facing, 0.0 );
		float nv    = max( dot( n, v ), 1e-4 );
		float nh    = ( Hooks & kHookSpecAtLens ) != 0 ? nl : max( dot( n, hv ), 0.0 );
		float vh    = clamp( dot( v, hv ), 0.0, 1.0 );
		float a2    = Alpha * Alpha;
		float dd    = nh * nh * ( a2 - 1.0 ) + 1.0;
		float D     = a2 / ( kPi * dd * dd );
		float smith = 0.5 / ( nl * sqrt( nv * nv * ( 1.0 - a2 ) + a2 ) + nv * sqrt( nl * nl * ( 1.0 - a2 ) + a2 ) );
		float m     = 1.0 - vh;
		vec3 F      = F0 + ( 1.0 - F0 ) * ( m * m * m * m * m );
		light += glaze * ( kPi * D * smith * F * incident + F0 * room );
	}
	light *= Gain;

	vec3 shown;
	if( ViewIndex == 1 )
		shown = vec3( Relief > 0.0 ? 1.0 + heightAt( uv0 ) / Relief : 1.0 );
	else if( ViewIndex == 2 )
		shown = 0.5 + 0.5 * normalAt( uv0, texelM );
	else if( ViewIndex == 3 )
		shown = vec3( g * vis );
	else if( ViewIndex == 4 )
		shown = vec3( g > 0.0 ? vis : 0.5 );//white lit, black shadowed, grey turned away
	else if( ViewIndex == 5 )
		shown = toCode( texture( Surface, uv0 ).rgb );
	else
		shown = toCode( light );
	fragColor = blend( src, vec4( shown, 1.0 ), MixAmount );
}
`;
