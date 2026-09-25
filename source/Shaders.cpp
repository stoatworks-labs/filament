#include "Shaders.h"

namespace filament::shaders
{
namespace
{
const char* const kVersion = "#version 410 core\n";

//---------------------------------------------------------------------------
// The vertex shader every pass shares.
//---------------------------------------------------------------------------
const char* const kVertexBody = R"(
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
)";

//---------------------------------------------------------------------------
// The filament. A fragment, not a shader: no #version, no main. The thermal
// pass and the output pass are each assembled around this one string, so
// there is one ODE, one waveform and one colour table in the plugin.
//---------------------------------------------------------------------------
const char* const kFilament = R"(
uniform int Perturb;          //negative-control hooks; always 0 in the plugin

//The lamp, as Lamp.h states it. Every constant is the CPU's, handed down.
uniform float Rho[ 34 ];      //Forsythe and Worthing, micro-ohm cm, 300..3600 K by 100
uniform float RhoRated;       //rho( 2800 K )
uniform float WattsPerKg;     //P / m: the lamp's size
uniform float RadiatedNorm;   //( 1 - fc ) / ( Tr^4 - Tref^4 )
uniform float ConductedNorm;  //fc / ( Tr - Tref )
uniform float LinearNorm;     //( 1 - fc ) / ( Tr - Tref ), the negative control's
uniform float Ambient;        //T0, kelvin
uniform float ShomateLow[ 5 ];  //c_p in J / ( kg K ), t = T / 1000, below 1900 K
uniform float ShomateHigh[ 5 ]; //and above
uniform int Law;              //0 linear, 1 square law, 2 triac

//The colour of a temperature: rows of ( r / Y, g / Y, b / Y, log2 Y ).
uniform sampler2D Planck;
uniform float PlanckFirst;
uniform float PlanckStep;
uniform int PlanckCount;

const float kPi = 3.14159265358979;

//Resistivity: linear between the table's points, the end segments
//extrapolated. Lamp.cpp's Resistivity() is the same rule.
float resistivity( float T )
{
	float x = ( T - 300.0 ) / 100.0;
	int i   = clamp( int( floor( x ) ), 0, 32 );
	float t = x - float( i );
	return Rho[ i ] + ( Rho[ i + 1 ] - Rho[ i ] ) * t;
}

float heatCapacity( float T )
{
	float t = T / 1000.0;
	if( T < 1900.0 )
		return ShomateLow[ 0 ] + ShomateLow[ 1 ] * t + ShomateLow[ 2 ] * t * t + ShomateLow[ 3 ] * t * t * t + ShomateLow[ 4 ] / ( t * t );
	return ShomateHigh[ 0 ] + ShomateHigh[ 1 ] * t + ShomateHigh[ 2 ] * t * t + ShomateHigh[ 3 ] * t * t * t + ShomateHigh[ 4 ] / ( t * t );
}

//sin( pi x ) for x in [0, 1]: folded, then Taylor to x^11. Plain arithmetic,
//so every driver computes the same waveform; GLSL's sin has no accuracy
//requirement at all (4.10 s8.1).
float sinPi( float x )
{
	x = min( x, 1.0 - x );
	float u  = kPi * x;
	float u2 = u * u;
	return u * ( 1.0 + u2 * ( -1.0 / 6.0 + u2 * ( 1.0 / 120.0 + u2 * ( -1.0 / 5040.0 + u2 * ( 1.0 / 362880.0 + u2 * ( -1.0 / 39916800.0 ) ) ) ) ) );
}

//( v( t ) / V )^2 at `cycles` mains cycles in, for a level through the law.
float driveSquared( float cycles, float level )
{
	level = clamp( level, 0.0, 1.0 );
	float amplitude = level;
	float gate      = 0.0;
	if( Law == 1 )
		amplitude = sqrt( level );
	else if( Law == 2 )
	{
		amplitude = 1.0;
		gate      = 1.0 - level;
	}
	if( ( Perturb & 8 ) != 0 )
	{
		//The negative control: DC at the same RMS, no waveform at all.
		if( Law == 2 )
		{
			float s2 = 2.0 * gate <= 1.0 ? sinPi( 2.0 * gate ) : -sinPi( 2.0 * gate - 1.0 );
			return 1.0 - gate + s2 / ( 2.0 * kPi );
		}
		return amplitude * amplitude;
	}
	float h = fract( 2.0 * cycles );//v^2 repeats every half cycle
	if( h < gate )
		return 0.0;
	float s = sinPi( h );
	return 2.0 * amplitude * amplitude * s * s;
}

//dT/dt in K/s: power in through R( T ), radiated as T^4, conducted as T.
float derivative( float T, float v2 )
{
	float rho  = ( Perturb & 1 ) != 0 ? RhoRated : resistivity( T );
	float pin  = v2 * RhoRated / rho;
	float T2   = T * T;
	float A2   = Ambient * Ambient;
	float rad  = ( Perturb & 2 ) != 0 ? LinearNorm * ( T - Ambient ) : RadiatedNorm * ( T2 * T2 - A2 * A2 );
	float cond = ( Perturb & 16 ) != 0 ? 0.0 : ConductedNorm * ( T - Ambient );
	return WattsPerKg * ( pin - rad - cond ) / heatCapacity( T );
}

//The light of a filament at T, linear sRGB, 2856 K at luminance 1.
//Interpolated by hand from two texelFetches: a hardware linear filter on a
//float texture interpolates with as few as 8 bits of weight on some GPUs.
vec3 emission( float T )
{
	if( ( Perturb & 32 ) != 0 )
		T *= 1.05;
	float x = clamp( ( T - PlanckFirst ) / PlanckStep, 0.0, float( PlanckCount - 1 ) - 0.0001 );
	int i   = int( floor( x ) );
	float t = x - float( i );
	vec4 a  = texelFetch( Planck, ivec2( i, 0 ), 0 );
	vec4 b  = texelFetch( Planck, ivec2( i + 1, 0 ), 0 );
	vec4 m  = a + ( b - a ) * t;
	return m.rgb * exp2( m.a );
}
)";

//---------------------------------------------------------------------------
// Pass 1: drive. One texel per bulb: the mean of an 8 x 8 grid of source
// pixels across the bulb's cell, texelFetch so the host's filter plays no
// part. rgb for a gelled wall, luma (the code values' Rec. 709 weights) for
// a white one. The code value is the fader: the clip says how far up each
// dimmer is.
//---------------------------------------------------------------------------
const char* const kDriveBody = R"(
uniform sampler2D Source;
uniform ivec2 SourceSize;
uniform vec2 Grid;

out vec4 fragColor;

void main()
{
	vec2 cell = floor( gl_FragCoord.xy );
	vec3 sum  = vec3( 0.0 );
	for( int j = 0; j < 8; ++j )
		for( int i = 0; i < 8; ++i )
		{
			vec2 f  = ( cell + ( vec2( i, j ) + 0.5 ) / 8.0 ) / Grid;
			ivec2 p = clamp( ivec2( floor( f * vec2( SourceSize ) ) ), ivec2( 0 ), SourceSize - 1 );
			sum += clamp( texelFetch( Source, p, 0 ).rgb, 0.0, 1.0 );
		}
	vec3 level = sum / 64.0;
	fragColor  = vec4( level, dot( level, vec3( 0.2126, 0.7152, 0.0722 ) ) );
}
)";

//---------------------------------------------------------------------------
// Pass 2: thermal. One texel per bulb, ping-ponged: the ODE integrated over
// the frame's seconds in `Substeps` classical RK4 steps of `StepSeconds`,
// with the mains waveform evaluated at each stage's own time. Writes the new
// temperatures to target 0 and the light averaged over the substeps -- what a
// camera's open shutter collects -- to target 1.
//---------------------------------------------------------------------------
const char* const kThermalBody = R"(
uniform sampler2D State;       //( T, T, T, drive ) last frame
uniform sampler2D Drive;
uniform int Substeps;
uniform float StepSeconds;
uniform float StepCycles;      //StepSeconds x mains hertz
uniform float Phase0;          //mains cycles at the start of the frame, fractional, reduced in double
uniform int Gels;              //0 one white filament, 1 three gelled ones
uniform vec3 GelRed;
uniform vec3 GelGreen;
uniform vec3 GelBlue;

layout( location = 0 ) out vec4 stateOut;
layout( location = 1 ) out vec4 lightOut;

float step1( float T, float level, int k )
{
	float c0 = Phase0 + float( k ) * StepCycles;
	float k1 = derivative( T, driveSquared( c0, level ) );
	float v2 = driveSquared( c0 + 0.5 * StepCycles, level );
	float k2 = derivative( T + 0.5 * StepSeconds * k1, v2 );
	float k3 = derivative( T + 0.5 * StepSeconds * k2, v2 );
	float k4 = derivative( T + StepSeconds * k3, driveSquared( c0 + StepCycles, level ) );
	T += StepSeconds / 6.0 * ( k1 + 2.0 * k2 + 2.0 * k3 + k4 );
	return clamp( T, 100.0, 3695.0 );
}

void main()
{
	ivec2 p     = ivec2( gl_FragCoord.xy );
	vec4 state  = texelFetch( State, p, 0 );
	vec4 drive  = texelFetch( Drive, p, 0 );
	int count   = Gels == 1 ? 3 : 1;
	vec3 T      = state.rgb;
	vec3 light  = vec3( 0.0 );
	vec3 gels[ 3 ] = vec3[ 3 ]( GelRed, GelGreen, GelBlue );

	for( int c = 0; c < 3; ++c )
	{
		if( c >= count )
			break;
		float level = Gels == 1 ? drive[ c ] : drive.a;
		vec3 tint   = Gels == 1 ? gels[ c ] : vec3( 1.0 );
		float t     = T[ c ];
		vec3 sum    = vec3( 0.0 );
		for( int k = 0; k < Substeps; ++k )
		{
			t = step1( t, level, k );
			sum += emission( t );
		}
		T[ c ] = t;
		light += tint * ( Substeps > 0 ? sum / float( Substeps ) : emission( t ) );
	}
	if( Gels != 1 )
		T = vec3( T.r );
	stateOut = vec4( T, drive.a );
	lightOut = vec4( light, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 3: fill. Every filament at the ambient: a cold wall.
//---------------------------------------------------------------------------
const char* const kFillBody = R"(
uniform float Kelvin;

out vec4 fragColor;

void main()
{
	fragColor = vec4( Kelvin, Kelvin, Kelvin, 0.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 4: regrid. The filaments of an old grid onto a new one: each new bulb
// takes the temperature of the old bulb under its centre, so a change of
// Columns or Rows mid-fade does not restart the wall cold.
//---------------------------------------------------------------------------
const char* const kRegridBody = R"(
uniform sampler2D State;
uniform vec2 OldGrid;
uniform vec2 NewGrid;

out vec4 fragColor;

void main()
{
	vec2 f  = gl_FragCoord.xy / NewGrid;
	ivec2 p = clamp( ivec2( floor( f * OldGrid ) ), ivec2( 0 ), ivec2( OldGrid ) - 1 );
	fragColor = texelFetch( State, p, 0 );
}
)";

//---------------------------------------------------------------------------
// Pass 5: bloom. A Gaussian along one axis at the grid's raster; run twice.
//---------------------------------------------------------------------------
const char* const kBlurBody = R"(
uniform sampler2D Light;
uniform ivec2 Direction;
uniform float Sigma;           //cells

out vec4 fragColor;

void main()
{
	ivec2 p    = ivec2( gl_FragCoord.xy );
	ivec2 size = textureSize( Light, 0 );
	vec3 sum   = vec3( 0.0 );
	float w    = 0.0;
	for( int i = -8; i <= 8; ++i )
	{
		ivec2 q  = clamp( p + Direction * i, ivec2( 0 ), size - 1 );
		float g  = exp( -0.5 * float( i * i ) / ( Sigma * Sigma ) );
		sum += g * texelFetch( Light, q, 0 ).rgb;
		w += g;
	}
	fragColor = vec4( sum / w, 1.0 );
}
)";

//---------------------------------------------------------------------------
// Pass 6: the wall, to the host. Each pixel sums the bulb of its own cell and
// the halos of the eight around it, adds the bloom, exposes, and encodes.
//---------------------------------------------------------------------------
const char* const kOutputBody = R"(
uniform sampler2D Light;       //shutter-averaged linear light per bulb
uniform sampler2D State;       //for the probes
uniform sampler2D Bloom;       //the blurred light, bilinear
uniform sampler2D Source;      //the host's input, for Mix and its alpha
uniform vec2 MaxUV;
uniform vec2 Grid;
uniform vec2 Size;             //output pixels
uniform float Gap;             //fraction of a cell between bulbs
uniform int PixelMode;
uniform int Frosted;
uniform vec3 Glass;            //linear transmittance
uniform float Glow;
uniform float BloomAmount;
uniform float ExposureGain;    //kExposureGain x 2^stops
uniform vec3 Wall;
uniform float Sheen;           //an unlit bulb's glass
uniform float MixAmount;
uniform int Probe;
uniform int Gels;
uniform vec3 GelRed;
uniform vec3 GelGreen;
uniform vec3 GelBlue;

in vec2 uv;
out vec4 fragColor;

float srgbEncode( float v )
{
	v = clamp( v, 0.0, 1.0 );
	return v <= 0.0031308 ? v * 12.92 : 1.055 * pow( v, 1.0 / 2.4 ) - 0.055;
}

//Distance from q to a short horizontal segment: the coil, in bulb radii.
float coil( vec2 q )
{
	vec2 d = vec2( max( abs( q.x ) - 0.34, 0.0 ), q.y );
	return length( d );
}

void main()
{
	ivec2 grid = ivec2( Grid );
	vec2 g     = uv * Grid;
	ivec2 cell = clamp( ivec2( floor( g ) ), ivec2( 0 ), grid - 1 );

	if( Probe == 1 )
	{
		fragColor = texelFetch( State, cell, 0 );
		return;
	}
	if( Probe == 2 )
	{
		vec4 s = texelFetch( State, cell, 0 );
		vec3 e = Gels == 1 ? GelRed * emission( s.r ) + GelGreen * emission( s.g ) + GelBlue * emission( s.b ) : emission( s.r );
		fragColor = vec4( e, 1.0 );
		return;
	}
	if( Probe == 3 )
	{
		fragColor = texelFetch( Light, cell, 0 );
		return;
	}

	vec2 cellPx = Size / Grid;
	float rCell = 0.5 * min( cellPx.x, cellPx.y );
	float rb    = max( rCell * ( 1.0 - Gap ), 0.5 );
	vec2 local  = ( g - vec2( cell ) - 0.5 ) * cellPx;//pixels from the own bulb's centre
	float aa    = 1.0 / rb;//one pixel, in bulb radii

	vec3 light = vec3( 0.0 );
	if( PixelMode == 1 )
	{
		//One flat pixel per bulb: a square of the cell, less the gap.
		vec2 halfSize = 0.5 * cellPx * ( 1.0 - Gap );
		vec2 cover    = clamp( halfSize - abs( local ) + 0.5, 0.0, 1.0 );
		vec3 e        = texelFetch( Light, cell, 0 ).rgb * Glass;
		light += e * cover.x * cover.y;
		for( int dy = -1; dy <= 1; ++dy )
			for( int dx = -1; dx <= 1; ++dx )
			{
				ivec2 n = cell + ivec2( dx, dy );
				if( any( lessThan( n, ivec2( 0 ) ) ) || any( greaterThanEqual( n, grid ) ) )
					continue;
				vec2 q    = max( abs( local - vec2( dx, dy ) * cellPx ) - halfSize, 0.0 ) / rCell;
				light += Glow * 0.35 * exp( -dot( q, q ) / 0.18 ) * texelFetch( Light, n, 0 ).rgb * Glass;
			}
	}
	else
	{
		for( int dy = -1; dy <= 1; ++dy )
			for( int dx = -1; dx <= 1; ++dx )
			{
				ivec2 n = cell + ivec2( dx, dy );
				if( any( lessThan( n, ivec2( 0 ) ) ) || any( greaterThanEqual( n, grid ) ) )
					continue;
				vec3 e  = texelFetch( Light, n, 0 ).rgb * Glass;
				vec2 q  = ( local - vec2( dx, dy ) * cellPx ) / rb;
				float d = length( q );
				float inside = 1.0 - smoothstep( 1.0 - aa, 1.0 + aa, d );
				float body;
				if( Frosted == 1 )
					body = inside * 1.05 * ( 1.0 - 0.35 * d * d );
				else
				{
					//The glass lit from within, and the coil itself, hot.
					float c = coil( q );
					body    = inside * 0.22 + 2.6 * exp( -c * c / ( 0.012 + aa * aa ) );
				}
				//The halo: light scattered by the glass and thrown forward by
				//the reflector, falling off over about a bulb's radius past
				//the glass. Glow is how much of it there is.
				float halo = Glow * ( 0.30 * exp( -d * d / 2.2 ) + 0.10 * exp( -d * d / 9.0 ) );
				light += e * ( body + halo );
				if( dx == 0 && dy == 0 )
					light += Sheen * inside * ( 0.6 + 0.4 * smoothstep( 0.5, 1.0, d ) );
			}
	}

	light += BloomAmount * 0.6 * texture( Bloom, uv ).rgb * Glass;
	vec3 v   = ( Wall + light ) * ExposureGain;
	vec3 lit = vec3( srgbEncode( 1.0 - exp( -v.r ) ), srgbEncode( 1.0 - exp( -v.g ) ), srgbEncode( 1.0 - exp( -v.b ) ) );

	//The wall paints the whole frame, so it is opaque: alpha 1 at Mix 1,
	//whatever the source's was. Resolume's demo clips carry alpha.
	vec4 source = texture( Source, uv * MaxUV );
	fragColor   = vec4( mix( source.rgb, lit, MixAmount ), mix( source.a, 1.0, MixAmount ) );
}
)";

std::string assemble( const char* body, bool withFilament )
{
	std::string s = kVersion;
	if( withFilament )
		s += kFilament;
	s += body;
	return s;
}
} // namespace

std::string Vertex()
{
	return assemble( kVertexBody, false );
}
std::string Drive()
{
	return assemble( kDriveBody, false );
}
std::string Thermal()
{
	return assemble( kThermalBody, true );
}
std::string Fill()
{
	return assemble( kFillBody, false );
}
std::string Regrid()
{
	return assemble( kRegridBody, false );
}
std::string Blur()
{
	return assemble( kBlurBody, false );
}
std::string Output()
{
	return assemble( kOutputBody, true );
}

} // namespace filament::shaders
