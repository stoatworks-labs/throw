#pragma once

#include <string>

/**
	The passes.

	1. **surface** -- only when something it depends on changes: the surface
	   into an RGBA32F buffer the frame's size, albedo in rgb (linear sRGB),
	   height in metres in a (0 at the front plane, down to -Relief).
	   Procedural surfaces are supersampled 2x2; a File is copied texel for
	   texel when its raster is the frame's.
	2. **occlusion** -- with the surface: how much of the room's light reaches
	   each point, horizon-based, against the tangent plane (so any plane is
	   exactly 1).
	3. **project** -- to the host, every frame: for each pixel, the point of
	   the relief the viewer sees, the projector ray that lands there, whether
	   anything is in the way, and the light it brings.

	The fragment shaders are assembled from pieces at InitGL by `Assemble()`,
	the one place the order is written. tools/glslc.sh's ASSEMBLED table
	mirrors it, so the text it compiles is the text the plugin runs.

	No sin, cos, tan or atan built-in anywhere (GLSL promises nothing about
	their accuracy): the corrugation and the drapes' folds use `cosTurn`, a
	reduced-range Taylor series good to 1e-10; the projector's basis comes
	from the CPU.
*/
namespace throwfx::shaders
{

extern const char* const kVersion;
extern const char* const kQuadVertex;
extern const char* const kCommon;
extern const char* const kSurfaceFragment;
extern const char* const kOcclusionFragment;
extern const char* const kProjectFragment;

enum class Pass
{
	Surface,
	Occlusion,
	Project
};
std::string Assemble( Pass pass );

/// Most steps a march takes, the viewer's or the shadow's. Each takes one a
/// texel of its horizontal travel up to this.
constexpr int kMaxSteps = 256;

/// Negative-control hooks: bits of the `Hooks` uniform. A shipped instance
/// sets 0, and every branch they guard is then the shipped model. Each one
/// exists so a harness check can be shown to fail against a wrong model. The
/// GLSL repeats these values in kCommon.
enum Hook : int
{
	kHookViewerAtLens  = 1 << 0,///< the viewer's march from the lens: no parallax
	kHookNoCosine      = 1 << 1,///< light by the flat wall's angle, not the relief's
	kHookNoInverseSq   = 1 << 2,///< the ray's length to the design screen, not to the relief
	kHookNoShadow      = 1 << 3,///< no march to the lens
	kHookNoAlbedo      = 1 << 4,///< every surface white
	kHookBackfaceLit   = 1 << 5,///< |n.w|: a face turned away lit as if it were turned to the lens
	kHookSpecAtLens    = 1 << 6,///< the highlight where the normal points at the lens, not at the half vector
	kHookNoRoom        = 1 << 7,///< no room light
	kHookFlatViewer    = 1 << 8,///< the viewer sees the relief straight on: no march, no shift
	kHookNoDesignCos   = 1 << 9,///< no 1 / cos^3: the projector as a point lamp
	kHookStaleSurface  = 1 << 10,///< CPU only: the surface built once and never again
	kHookOffsetMap     = 1 << 11 ///< offset mapping: shifted by the height under the pixel, not marched
};

} // namespace throwfx::shaders
