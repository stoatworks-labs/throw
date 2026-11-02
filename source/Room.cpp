#include "Room.h"

#include <algorithm>
#include <cmath>

namespace throwfx
{
namespace
{
Vec3 sub( const Vec3& a, const Vec3& b )
{
	return { a.x - b.x, a.y - b.y, a.z - b.z };
}
Vec3 cross( const Vec3& a, const Vec3& b )
{
	return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
}
double length( const Vec3& a )
{
	return std::sqrt( a.x * a.x + a.y * a.y + a.z * a.z );
}
Vec3 unit( const Vec3& a )
{
	const double l = length( a );
	return l > 0.0 ? Vec3 { a.x / l, a.y / l, a.z / l } : Vec3 { 0.0, 0.0, -1.0 };
}
} // namespace

double PaperWeight( double crumple, int layer )
{
	return std::clamp( 4.0 * crumple - static_cast< double >( layer ), 0.0, 1.0 );
}

double ReliefOf( Surface surface, const float* p, double frameWidth )
{
	const double scale = ScaleFromParam( p[ PT_SCALE ] );
	const double depth = DepthFromParam( p[ PT_DEPTH ] );
	switch( surface )
	{
	case Surface::Flat: return 0.0;
	case Surface::Paper:
	{
		//Every corner of every generation is within +-amp of its plane, so
		//the sum spans twice the weighted amplitudes.
		const double crumple = std::clamp( static_cast< double >( p[ PT_CRUMPLE ] ), 0.0, 1.0 );
		double sum = 0.0, amp = facts::kPaperRelief;
		for( int l = 0; l < facts::kPaperLayers; ++l, amp *= facts::kPaperShrink )
			sum += PaperWeight( crumple, l ) * amp;
		return 2.0 * sum * scale * depth;
	}
	case Surface::Brick: return facts::kJointRecess * scale * depth;
	case Surface::Corrugated:
	{
		const auto profile = static_cast< Profile >( OptionIndex( p[ PT_PROFILE ], static_cast< int >( Profile::Count ) ) );
		const double ratio = profile == Profile::Sine ? facts::kSineDepthPerPitch : facts::kBoxDepthPerPitch;
		return ratio * PitchFromParam( p[ PT_PITCH ] ) * scale * depth;
	}
	case Surface::Plate: return facts::kLugHeight * scale * depth;
	case Surface::Drapes:
	{
		const double pleats = std::clamp( std::round( static_cast< double >( p[ PT_PLEATS ] ) ), 2.0, 60.0 );
		return facts::kFoldDepthPerSpacing * scale * frameWidth / pleats * depth;
	}
	case Surface::Weatherboard: return facts::kBoardLip * scale * depth;
	case Surface::Tiles: return facts::kTileRecess * scale * depth;
	case Surface::File: return facts::kFileRelief * scale * depth;
	case Surface::Count: break;
	}
	return 0.0;
}

Room MakeRoom( const float* p, int width, int height )
{
	Room room;
	room.H = WallHeightFromParam( p[ PT_WALL_HEIGHT ] );
	room.W = room.H * static_cast< double >( std::max( width, 1 ) ) / static_cast< double >( std::max( height, 1 ) );

	room.lens = { OffsetFromParam( p[ PT_PROJECTOR_X ] ), OffsetFromParam( p[ PT_PROJECTOR_Y ] ), DistanceFromParam( p[ PT_THROW ] ) };
	room.eye  = { OffsetFromParam( p[ PT_VIEWER_X ] ), OffsetFromParam( p[ PT_VIEWER_Y ] ), DistanceFromParam( p[ PT_VIEWER_DISTANCE ] ) };
	room.aim  = static_cast< Aim >( OptionIndex( p[ PT_AIM ], static_cast< int >( Aim::Count ) ) );

	if( room.aim == Aim::LensShift )
	{
		//Square to the wall; it lights the wall itself evenly.
		room.axis   = { 0.0, 0.0, -1.0 };
		room.right  = { 1.0, 0.0, 0.0 };
		room.up     = { 0.0, 1.0, 0.0 };
		room.design = room.lens.z;
	}
	else
	{
		//At the frame's centre, horizon level: right is square to the axis and
		//to the world's up, and up completes a right-handed view (right x up
		//points back at the lens, the dice trap).
		const Vec3 toCentre = sub( Vec3 {}, room.lens );
		room.axis           = unit( toCentre );
		room.right          = unit( cross( room.axis, Vec3 { 0.0, 1.0, 0.0 } ) );
		room.up             = cross( room.right, room.axis );
		room.design         = length( toCentre );
	}

	const auto surface = static_cast< Surface >( OptionIndex( p[ PT_SURFACE ], static_cast< int >( Surface::Count ) ) );
	room.relief        = ReliefOf( surface, p, room.W );

	//The projector's lumens over its picture, W x H on its design screen; the
	//room's lux on the wall. Their ratio is all the picture needs.
	room.roomRatio  = AmbientFromParam( p[ PT_AMBIENT ] ) * room.W * room.H / LumensFromParam( p[ PT_LUMENS ] );
	room.blackLevel = BlackLevelFromParam( p[ PT_BLACK_LEVEL ] );
	room.gain       = ExposureFromParam( p[ PT_EXPOSURE ] );

	const double rough = RoughnessFromParam( p[ PT_ROUGHNESS ] );
	room.alpha         = rough * rough;
	switch( surface )
	{
	case Surface::Corrugated:
		room.glaze = Glaze::All;
		std::copy( std::begin( facts::kZincF0 ), std::end( facts::kZincF0 ), room.f0 );
		break;
	case Surface::Plate:
		room.glaze = Glaze::All;
		std::copy( std::begin( facts::kAluminiumF0 ), std::end( facts::kAluminiumF0 ), room.f0 );
		break;
	case Surface::Tiles:
		room.glaze = Glaze::Tiles;
		room.f0[ 0 ] = room.f0[ 1 ] = room.f0[ 2 ] = facts::kGlazeF0;
		//Halfway between the bevel's foot and the grout.
		room.glazeFloor = 0.5 * ( facts::kBevelDrop + facts::kTileRecess ) * ScaleFromParam( p[ PT_SCALE ] ) * DepthFromParam( p[ PT_DEPTH ] );
		break;
	default: room.glaze = Glaze::None; break;
	}

	const double pleats = std::clamp( std::round( static_cast< double >( p[ PT_PLEATS ] ) ), 2.0, 60.0 );
	room.pleatSpacing   = ScaleFromParam( p[ PT_SCALE ] ) * room.W / pleats;
	const double crumple = std::clamp( static_cast< double >( p[ PT_CRUMPLE ] ), 0.0, 1.0 );
	for( int l = 0; l < facts::kPaperLayers; ++l )
		room.paperWeights[ l ] = PaperWeight( crumple, l );
	return room;
}

} // namespace throwfx
