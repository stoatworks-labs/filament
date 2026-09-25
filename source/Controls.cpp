#include "Controls.h"

#include <algorithm>
#include <cmath>

namespace filament::controls
{
namespace
{
float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}
} // namespace

double Watts( float value )
{
	return 10.0 * std::pow( 10.0, 2.0 * static_cast< double >( clamp01( value ) ) );
}

float WattsParam( double watts )
{
	return clamp01( static_cast< float >( std::log10( std::max( watts, 10.0 ) / 10.0 ) / 2.0 ) );
}

double AmbientKelvin( float value )
{
	return 273.15 + 200.0 * static_cast< double >( clamp01( value ) );
}

float AmbientParam( double kelvin )
{
	return clamp01( static_cast< float >( ( kelvin - 273.15 ) / 200.0 ) );
}

float GapFraction( float value )
{
	return 0.8f * clamp01( value );
}

float ExposureStops( float value )
{
	return 8.0f * clamp01( value ) - 4.0f;
}

float ExposureParam( float stops )
{
	return clamp01( ( stops + 4.0f ) / 8.0f );
}

float Amount( float value )
{
	return clamp01( value );
}

const char* GlassName( int index )
{
	static const char* const names[ kGlassCount ] = { "Clear", "Frosted", "Amber", "Red", "Green", "Blue" };
	return names[ std::clamp( index, 0, kGlassCount - 1 ) ];
}

void GlassTint( int index, float rgb[ 3 ] )
{
	//Linear transmittance, chosen by eye as the look of each glass, not
	//measured from a filter: see AGENTS.md. Clear is exactly 1 so the
	//colour checks read the filament's own light.
	static const float tints[ kGlassCount ][ 3 ] = {
		{ 1.0f, 1.0f, 1.0f },    //Clear
		{ 0.92f, 0.92f, 0.92f }, //Frosted: a little lost in the frosting
		{ 1.0f, 0.52f, 0.10f },  //Amber
		{ 1.0f, 0.05f, 0.04f },  //Red
		{ 0.10f, 0.85f, 0.18f }, //Green
		{ 0.08f, 0.25f, 1.0f },  //Blue
	};
	const int i = std::clamp( index, 0, kGlassCount - 1 );
	rgb[ 0 ]    = tints[ i ][ 0 ];
	rgb[ 1 ]    = tints[ i ][ 1 ];
	rgb[ 2 ]    = tints[ i ][ 2 ];
}

bool GlassFrosted( int index )
{
	return index == 1;
}

const char* GelsName( int index )
{
	static const char* const names[ kGelsCount ] = { "Off", "RGB" };
	return names[ std::clamp( index, 0, kGelsCount - 1 ) ];
}

const char* DimmerName( int index )
{
	static const char* const names[ kDimmerCount ] = { "Linear", "Square Law", "Triac" };
	return names[ std::clamp( index, 0, kDimmerCount - 1 ) ];
}

const char* MainsName( int index )
{
	static const char* const names[ kMainsCount ] = { "50 Hz", "60 Hz" };
	return names[ std::clamp( index, 0, kMainsCount - 1 ) ];
}

double MainsHertz( int index )
{
	return std::clamp( index, 0, kMainsCount - 1 ) == 1 ? 60.0 : 50.0;
}

int OptionIndex( float value, int count )
{
	return std::clamp( static_cast< int >( std::lround( value ) ), 0, count - 1 );
}

} // namespace filament::controls
