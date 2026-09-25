#pragma once

/**
	Host parameters are 0..1; these are what they mean.

	`SetParamInfo` clamps a STANDARD default into 0..1 *before* returning, and
	`SetParamRange` can only be called afterwards, so every slider here is a
	plain 0..1 float and the conversions live in this one file, which the
	plugin and the harness both use. Columns and Rows are `FF_TYPE_INTEGER`
	and hold real integers. Options are mapped by INDEX: an option parameter's
	range reads back 0..1 from the SDK whatever its element count.

	Positions a check needs to land on exactly do: Wattage at a multiple of
	1/4 is a power of ten of watts times 10 (0.5 is exactly 100 W), Ambient
	0.125 is exactly 25 C, Exposure 0.5 is exactly 0 stops.
*/
namespace filament::controls
{

/// Wattage: 10 W to 1000 W, geometric. 0.5 is 100 W.
double Watts( float value );
float WattsParam( double watts );

/// Ambient Temp: 0 to 200 C, linear, in kelvin. 0.125 is 25 C.
double AmbientKelvin( float value );
float AmbientParam( double kelvin );

/// Gap: the fraction of a cell between bulbs, 0 to 0.8, linear.
float GapFraction( float value );

/// Exposure: -4 to +4 stops, linear. 0.5 is 0.
float ExposureStops( float value );
float ExposureParam( float stops );

/// Glow and Bloom are 0..1 amounts; Mix is 0..1.
float Amount( float value );

constexpr int kMinColumns = 4;
constexpr int kMaxColumns = 160;
constexpr int kMinRows    = 2;
constexpr int kMaxRows    = 90;

constexpr int kGlassCount = 6;
const char* GlassName( int index );
/// Linear transmittance of the glass, and whether it diffuses (frosted).
void GlassTint( int index, float rgb[ 3 ] );
bool GlassFrosted( int index );

constexpr int kGelsCount = 2;
const char* GelsName( int index );

constexpr int kDimmerCount = 3;
const char* DimmerName( int index );

constexpr int kMainsCount = 2;
const char* MainsName( int index );
double MainsHertz( int index );

int OptionIndex( float value, int count );

} // namespace filament::controls
