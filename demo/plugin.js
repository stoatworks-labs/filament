/**
 * Filament — browser demo.
 *
 * The picture on a wall of incandescent bulbs. The one idea, from `AGENTS.md`
 * and `source/Lamp.h`: a bulb is a tungsten wire heated by current, not an LED
 * with a slow fade. Its light is its temperature, and its temperature follows
 *
 *     C( T ) dT/dt = V( t )^2 / R( T )  -  e s A ( T^4 - T0^4 )  -  k ( T - T0 )
 *
 * Point a matrix of them at the clip, each bulb's dimmer set by its cell, and
 * the look falls out of the terms: a dimmed bulb goes red as well as dark, a
 * flash rises fast and falls slowly, a cold bulb surges, a small filament
 * flickers at twice the mains.
 *
 * This plugin is TEMPORAL: every bulb carries its temperature from frame to
 * frame. So the two halves of this page are not equally faithful, and the
 * split is worth being exact about:
 *
 *   The SHADERS are the plugin's, and the physics lives in them. The eight GLSL
 *   strings below -- the version line, the vertex body, the `kFilament` library
 *   and the drive, thermal, fill, regrid, blur and output bodies -- are
 *   `source/Shaders.cpp`'s, spliced in by `demo/tools/sync_shaders.py` with
 *   their tabs and comments, and assembled the way the plugin assembles them.
 *   The ODE (classical RK4 over the frame's substeps), tungsten's resistivity
 *   table, c_p from the Shomate fit, the mains waveform through the dimmer law
 *   (a Taylor `sinPi`, not GLSL's `sin`) and the Planck colour table all run on
 *   the GPU here as there, over the same ping-ponged RGBA32F state per bulb,
 *   with the thermal pass writing two render targets at once (the new
 *   temperatures, and the light averaged over the substeps). The numbers those
 *   shaders are handed -- Lamp.h's tables and design point, Model.h's picture
 *   constants, the glass tints, the 346-row Planck table -- are copied by the
 *   same script. `demo/tools/check_shaders.py` holds all of it to the C++
 *   character for character, and `tools/verify.sh` runs it.
 *
 *   The CPU half is a PORT, by hand, and nothing checks it but a reader:
 *   Controls.cpp (every slider to its unit), Lamp.cpp (the resistivity rule,
 *   c_p, the mains voltage, the wire sized from the wattage -- its diameter,
 *   length and mass and so C( T ) / P -- the derivative in double, and the
 *   stiffness scan), and Filament::ProcessOpenGL: the frame's seconds and the
 *   mains phase at its start reduced in double (`start x hertz -
 *   floor( start x hertz )`), the substep rule (`ceil( dt / hMax )` with
 *   `hMax = min( half-cycle / 20, 1.5 / stiffness )`, capped at 4096, dt
 *   clamped to [0, 0.25 s]), the lamp cache, the fill, the regrid on a grid
 *   change and the pass order -- in JavaScript doubles as the C++ keeps them
 *   in double, rounded to float (Math.fround) where the plugin hands a float
 *   uniform over or computes in float. `fitest --steady`, `--rise`, `--fall`,
 *   `--ripple`, `--colour` and `--resize` check the C++ originals and have no
 *   idea this page exists.
 *
 *   Three of the lamp's constants are ASSUMPTIONS, not measurements, in the
 *   plugin and so here: emissivity 0.30, the coil radiating from half its
 *   surface, and 15% of the rated power conducted. They size the wire, and so
 *   set the time scale; nothing else.
 *
 * ------------------------------------------------------------- the buffers
 *
 * Per bulb (the grid's raster, never the output's): two RGBA32F state buffers
 * ping-ponged, an RGBA32F drive, an RGBA32F light, and the bloom at RGBA16F
 * (x Nearest, y Linear) -- the plugin's formats and filters. Rendering into a
 * float texture is an extension in WebGL2 (EXT_color_buffer_float). Without
 * it the page refuses to start: an 8-bit temperature would quantise the wall
 * to 256 steps between 0 and 1 K, which is to say it would not run at all.
 * The thermal pass's two targets are checked for completeness and the page
 * says so if a browser will not render both.
 *
 * ------------------------------------------------------------- the clock
 *
 * The plugin reads the host's clock and votes on its unit (readout's scheme).
 * Here the clock is the kit's: `time` in declared seconds, accumulated from
 * frame deltas while playing (the kit caps one delta at 0.1 s), +1/60 on Step.
 * No vote runs. The plugin's own rules then apply unchanged: dt = now - last,
 * clamped to [0, 0.25 s], a nominal 1/60 on the first frame. So a paused page,
 * which renders only when a control moves, renders frames of dt = 0: no
 * substeps, and the light of the instant instead of the shutter's average,
 * which is what the plugin does for a host whose clock stops. Restart sends
 * the kit's clock back to 0; the plugin has no special rule for a clock that
 * runs backwards, and its clamp makes that frame worth 0 s, so the wall keeps
 * its temperatures and carries on. That is the plugin's behaviour, not a reset.
 *
 * ------------------------------------------------------------- what is missing
 *
 * Columns and Rows are FF_TYPE_INTEGER in the plugin (4..160, 2..90) and the
 * kit has no integer control, so -- as teletext, wetplate and the rest did --
 * each is a dropdown of every value in the plugin's range. The About block is
 * absent, as on every page in this suite. The harness-only `Perturb` and
 * `Probe` uniforms are set to what the shipped plugin sets them to: 0.
 */

import { mountDemo } from './vendor/demo.js';
import { Program, PassBuffer, GLError, bindTexture } from './vendor/gl.js';

//=== BEGIN GENERATED by demo/tools/sync_shaders.py from source/ -- do not edit by hand.

// source/Shaders.cpp, verbatim. `assemble` (below the block) is Shaders.cpp's.
const K_VERSION = "#version 410 core\n";

const VERTEX_BODY = `
layout( location = 0 ) in vec4 vPosition;
layout( location = 1 ) in vec2 vUV;

out vec2 uv;

void main()
{
	gl_Position = vPosition;
	uv = vUV;
}
`;

const FILAMENT = `
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

//( v( t ) / V )^2 at \`cycles\` mains cycles in, for a level through the law.
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
`;

const DRIVE_BODY = `
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
`;

const THERMAL_BODY = `
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
`;

const FILL_BODY = `
uniform float Kelvin;

out vec4 fragColor;

void main()
{
	fragColor = vec4( Kelvin, Kelvin, Kelvin, 0.0 );
}
`;

const REGRID_BODY = `
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
`;

const BLUR_BODY = `
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
`;

const OUTPUT_BODY = `
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
`;

// source/Lamp.h: the design point, the assumptions, the tables, the substep rule.
const LAMP_H = {
  kPi: 3.14159265358979323846,
  kRatedKelvin: 2800.0,
  kReferenceAmbient: 298.15,
  kConductionFraction: 0.15,
  kEmissivity: 0.30,
  kCoilRadiatingFraction: 0.5,
  kStefanBoltzmann: 5.670374419e-8,
  kTungstenDensity: 19250.0,
  kTungstenMolarMass: 0.18384,
  kMeltingKelvin: 3695.0,
  kRhoCount: 34,
  kRhoFirst: 300.0,
  kRhoStep: 100.0,
  kShomateSplit: 1900.0,
  kStepsPerHalfCycle: 20,
  kStability: 1.5,
  kMaxSubsteps: 4096,
};
const LAMP_kRho = [5.65, 8.06, 10.56, 13.23, 16.09, 19.00, 21.94, 24.93, 27.94, 30.98, 34.08, 37.19, 40.36, 43.55, 46.78, 50.05, 53.35, 56.67, 60.06, 63.48, 66.91, 70.39, 73.91, 77.49, 81.04, 84.70, 88.33, 92.04, 95.76, 99.54, 103.3, 107.2, 111.1, 115.0];
const LAMP_kShomateLow = [23.95930, 2.639680, 1.257750, -0.254642, -0.048407];
const LAMP_kShomateHigh = [-22.57640, 90.27980, -44.27150, 7.176630, -24.09740];

// source/Model.h: the picture's constants, judged by eye in the plugin.
const MODEL_H = {
  kNominalFrame: 1.0 / 60.0,
  kMaxFrameDelta: 0.25,
  kExposureGain: 2.2,
  kGlassSheen: 0.005,
  kBloomSigmaCells: 1.8,
};
const MODEL_kWall = [0.0016, 0.0015, 0.0014];
const MODEL_kGelRed = [1.0, 0.04, 0.03];
const MODEL_kGelGreen = [0.06, 0.80, 0.12];
const MODEL_kGelBlue = [0.04, 0.20, 1.0];

// source/Controls.h and Controls.cpp: the ranges, the option lists, the glass.
const CONTROLS_H = {
  kMinColumns: 4,
  kMaxColumns: 160,
  kMinRows: 2,
  kMaxRows: 90,
  kGlassCount: 6,
  kGelsCount: 2,
  kDimmerCount: 3,
  kMainsCount: 2,
};
const GLASS_NAMES = ['Clear', 'Frosted', 'Amber', 'Red', 'Green', 'Blue'];
const GELS_NAMES = ['Off', 'RGB'];
const DIMMER_NAMES = ['Linear', 'Square Law', 'Triac'];
const MAINS_NAMES = ['50 Hz', '60 Hz'];
const GLASS_TINTS = [[1.0, 1.0, 1.0], [0.92, 0.92, 0.92], [1.0, 0.52, 0.10], [1.0, 0.05, 0.04], [0.10, 0.85, 0.18], [0.08, 0.25, 1.0]];

// source/PlanckTable.h (itself generated by tools/planck_table.py): rows of
// ( r / Y, g / Y, b / Y, log2( Y / Y(2856 K) ) ), 10 K apart.
const PLANCK_FIRST_KELVIN = 250;
const PLANCK_STEP_KELVIN = 10;
const PLANCK_COUNT = 346;
const PLANCK_TABLE = [
  7.43508111, -0.806802974, -0.0497827636, -111.829223, // 250 K
  7.43395467, -0.806466193, -0.049802025, -107.756089, // 260 K
  7.4321989, -0.805941258, -0.049832026, -103.960531, // 270 K
  7.42958205, -0.805158879, -0.0498767076, -100.411924, // 280 K
  7.42583969, -0.804040009, -0.0499405594, -97.0840572, // 290 K
  7.42068786, -0.802499745, -0.0500283948, -93.9544348, // 300 K
  7.41383982, -0.800452371, -0.0501450636, -91.0037014, // 310 K
  7.40502439, -0.79781681, -0.0502951412, -88.2151601, // 320 K
  7.3940029, -0.794521713, -0.0504826404, -85.5743632, // 330 K
  7.38058262, -0.790509468, -0.0507107851, -83.0687625, // 340 K
  7.36462525, -0.785738731, -0.0509818683, -80.6874097, // 350 K
  7.34604997, -0.780185345, -0.0512972024, -78.4207018, // 360 K
  7.32483166, -0.773841809, -0.0516571512, -76.2601649, // 370 K
  7.3009955, -0.766715654, -0.0520612238, -74.1982735, // 380 K
  7.27460914, -0.758827115, -0.0525082076, -72.2283012, // 390 K
  7.24577398, -0.750206511, -0.0529963176, -70.3441975, // 400 K
  7.21461639, -0.740891632, -0.0535233443, -68.5404871, // 410 K
  7.18127975, -0.730925345, -0.0540867889, -66.8121889, // 420 K
  7.14591752, -0.720353542, -0.0546839787, -65.154749, // 430 K
  7.10868775, -0.709223478, -0.0553121608, -63.5639874, // 440 K
  7.06974869, -0.697582472, -0.0559685736, -62.0360531, // 450 K
  7.02925562, -0.685476958, -0.0566504999, -60.5673877, // 460 K
  6.98735862, -0.67295181, -0.057355304, -59.154695, // 470 K
  6.94420106, -0.660049905, -0.058080455, -57.7949151, // 480 K
  6.89991875, -0.646811854, -0.0588235415, -56.485202, // 490 K
  6.85463947, -0.633275873, -0.0595822773, -55.2229048, // 500 K
  6.80848287, -0.619477743, -0.0603545036, -54.0055509, // 510 K
  6.76156055, -0.605450844, -0.061138186, -52.8308314, // 520 K
  6.7139763, -0.591226221, -0.0619314103, -51.6965875, // 530 K
  6.66582645, -0.576832693, -0.062732376, -50.6007991, // 540 K
  6.61720028, -0.562296963, -0.0635393894, -49.5415741, // 550 K
  6.56818037, -0.547643747, -0.0643508568, -48.5171386, // 560 K
  6.51884311, -0.532895896, -0.0651652766, -47.5258282, // 570 K
  6.46925902, -0.518074521, -0.0659812328, -46.5660802, // 580 K
  6.41949321, -0.50319911, -0.066797388, -45.6364257, // 590 K
  6.36960574, -0.488287643, -0.0676124774, -44.7354836, // 600 K
  6.31965198, -0.473356698, -0.0684253027, -43.8619538, // 610 K
  6.26968292, -0.458421548, -0.0692347269, -43.0146121, // 620 K
  6.21974549, -0.443496254, -0.0700396696, -42.1923044, // 630 K
  6.16988286, -0.428593752, -0.0708391021, -41.3939423, // 640 K
  6.12013471, -0.413725931, -0.0716320437, -40.6184983, // 650 K
  6.07053742, -0.398903708, -0.0724175577, -39.8650022, // 660 K
  6.02112439, -0.384137093, -0.0731947484, -39.1325369, // 670 K
  5.97192615, -0.369435255, -0.073962758, -38.420235, // 680 K
  5.92297066, -0.354806581, -0.0747207636, -37.7272755, // 690 K
  5.87428338, -0.340258727, -0.0754679754, -37.0528809, // 700 K
  5.82588754, -0.32579867, -0.0762036337, -36.3963146, // 710 K
  5.77780424, -0.311432754, -0.0769270078, -35.756878, // 720 K
  5.7300526, -0.297166737, -0.0776373935, -35.1339081, // 730 K
  5.6826499, -0.283005824, -0.0783341121, -34.5267756, // 740 K
  5.63561173, -0.268954714, -0.0790165085, -33.9348827, // 750 K
  5.58895207, -0.255017628, -0.0796839506, -33.3576609, // 760 K
  5.54268343, -0.241198343, -0.0803358274, -32.7945698, // 770 K
  5.49681693, -0.227500225, -0.0809715488, -32.2450949, // 780 K
  5.4513624, -0.213926258, -0.0815905442, -31.7087464, // 790 K
  5.40632849, -0.200479066, -0.0821922618, -31.1850575, // 800 K
  5.36172275, -0.187160943, -0.0827761682, -30.6735834, // 810 K
  5.31755166, -0.173973874, -0.0833417475, -30.1738997, // 820 K
  5.27382077, -0.160919557, -0.0838885008, -29.6856016, // 830 K
  5.23053475, -0.147999425, -0.084415946, -29.2083024, // 840 K
  5.18769741, -0.135214661, -0.084923617, -28.7416329, // 850 K
  5.14531181, -0.122566223, -0.0854110637, -28.28524, // 860 K
  5.10338032, -0.110054852, -0.0858778514, -27.8387862, // 870 K
  5.06190462, -0.0976810971, -0.0863235606, -27.4019487, // 880 K
  5.02088581, -0.085445322, -0.0867477869, -26.9744184, // 890 K
  4.98032442, -0.0733477238, -0.0871501405, -26.5558995, // 900 K
  4.94022047, -0.0613883439, -0.0875302462, -26.1461084, // 910 K
  4.90057347, -0.0495670807, -0.0878877431, -25.7447734, // 920 K
  4.86138254, -0.0378837004, -0.0882222847, -25.351634, // 930 K
  4.82264636, -0.0263378482, -0.0885335383, -24.9664403, // 940 K
  4.78436326, -0.0149290575, -0.0888211851, -24.5889525, // 950 K
  4.74653122, -0.00365675956, -0.0890849203, -24.2189404, // 960 K
  4.70914791, 0.00747970794, -0.0893244523, -23.8561827, // 970 K
  4.67221073, 0.018481093, -0.0895395035, -23.500467, // 980 K
  4.63571679, 0.0293482216, -0.0897298092, -23.151589, // 990 K
  4.59966301, 0.0400819909, -0.0898951182, -22.8093525, // 1000 K
  4.56404607, 0.0506833623, -0.0900351923, -22.4735684, // 1010 K
  4.52886245, 0.0611533557, -0.0901498065, -22.1440549, // 1020 K
  4.49410847, 0.0714930435, -0.0902387483, -21.820637, // 1030 K
  4.45978031, 0.0817035456, -0.0903018182, -21.5031463, // 1040 K
  4.42587399, 0.0917860241, -0.0903388293, -21.1914203, // 1050 K
  4.39238541, 0.101741679, -0.0903496069, -20.8853025, // 1060 K
  4.35931038, 0.111571743, -0.0903339887, -20.5846422, // 1070 K
  4.32664458, 0.12127748, -0.0902918247, -20.2892938, // 1080 K
  4.29438364, 0.130860178, -0.0902229766, -19.9991171, // 1090 K
  4.2625231, 0.140321147, -0.0901273183, -19.7139767, // 1100 K
  4.23105846, 0.149661718, -0.0900047349, -19.4337419, // 1110 K
  4.19998515, 0.158883236, -0.0898551233, -19.1582864, // 1120 K
  4.16929857, 0.167987061, -0.0896783917, -18.8874885, // 1130 K
  4.13899407, 0.176974563, -0.0894744592, -18.6212303, // 1140 K
  4.10906701, 0.18584712, -0.0892432561, -18.3593981, // 1150 K
  4.07951269, 0.194606116, -0.0889847232, -18.1018818, // 1160 K
  4.05032644, 0.203252941, -0.088698812, -17.8485751, // 1170 K
  4.02150355, 0.211788986, -0.0883854844, -17.599375, // 1180 K
  3.99303935, 0.220215643, -0.0880447122, -17.3541819, // 1190 K
  3.96492913, 0.228534302, -0.0876764775, -17.1128995, // 1200 K
  3.93716824, 0.236746351, -0.0872807717, -16.8754346, // 1210 K
  3.909752, 0.244853177, -0.0868575961, -16.6416968, // 1220 K
  3.88267579, 0.252856157, -0.086406961, -16.4115986, // 1230 K
  3.85593498, 0.260756666, -0.085928886, -16.1850552, // 1240 K
  3.829525, 0.26855607, -0.0854233996, -15.9619845, // 1250 K
  3.80344128, 0.276255728, -0.0848905386, -15.7423068, // 1260 K
  3.77767931, 0.283856988, -0.0843303487, -15.5259451, // 1270 K
  3.75223458, 0.29136119, -0.0837428834, -15.3128244, // 1280 K
  3.72710266, 0.298769664, -0.0831282045, -15.1028721, // 1290 K
  3.70227913, 0.306083727, -0.0824863813, -14.8960178, // 1300 K
  3.67775962, 0.313304687, -0.0818174907, -14.692193, // 1310 K
  3.6535398, 0.320433838, -0.0811216171, -14.4913314, // 1320 K
  3.6296154, 0.327472462, -0.0803988518, -14.2933686, // 1330 K
  3.60598218, 0.334421827, -0.0796492931, -14.098242, // 1340 K
  3.58263594, 0.34128319, -0.0788730459, -13.9058908, // 1350 K
  3.55957255, 0.348057792, -0.0780702214, -13.716256, // 1360 K
  3.53678792, 0.354746861, -0.0772409374, -13.5292802, // 1370 K
  3.51427801, 0.361351612, -0.0763853173, -13.3449076, // 1380 K
  3.49203882, 0.367873244, -0.0755034906, -13.1630839, // 1390 K
  3.47006641, 0.374312941, -0.0745955922, -12.9837566, // 1400 K
  3.4483569, 0.380671875, -0.0736617625, -12.8068743, // 1410 K
  3.42690644, 0.386951201, -0.0727021471, -12.6323872, // 1420 K
  3.40571125, 0.39315206, -0.0717168964, -12.4602466, // 1430 K
  3.38476759, 0.399275578, -0.0707061658, -12.2904056, // 1440 K
  3.36407178, 0.405322866, -0.0696701152, -12.122818, // 1450 K
  3.34362019, 0.411295021, -0.0686089088, -11.9574391, // 1460 K
  3.32340923, 0.417193122, -0.0675227152, -11.7942255, // 1470 K
  3.30343538, 0.423018237, -0.0664117068, -11.6331346, // 1480 K
  3.28369517, 0.428771418, -0.0652760599, -11.4741253, // 1490 K
  3.26418516, 0.4344537, -0.0641159545, -11.3171572, // 1500 K
  3.24490198, 0.440066106, -0.0629315741, -11.1621912, // 1510 K
  3.2258423, 0.445609642, -0.0617231054, -11.0091891, // 1520 K
  3.20700285, 0.451085302, -0.0604907384, -10.8581136, // 1530 K
  3.18838041, 0.456494064, -0.0592346658, -10.7089285, // 1540 K
  3.1699718, 0.46183689, -0.0579550833, -10.5615983, // 1550 K
  3.15177389, 0.467114732, -0.0566521892, -10.4160885, // 1560 K
  3.1337836, 0.472328523, -0.0553261843, -10.2723656, // 1570 K
  3.11599791, 0.477479186, -0.0539772718, -10.1303967, // 1580 K
  3.09841383, 0.482567628, -0.0526056569, -9.99014969, // 1590 K
  3.08102842, 0.487594743, -0.051211547, -9.85159342, // 1600 K
  3.0638388, 0.49256141, -0.0497951514, -9.71469738, // 1610 K
  3.04684211, 0.497468496, -0.0483566811, -9.5794318, // 1620 K
  3.03003557, 0.502316855, -0.0468963487, -9.44576763, // 1630 K
  3.01341642, 0.507107327, -0.0454143685, -9.31367651, // 1640 K
  2.99698195, 0.511840739, -0.043910956, -9.18313075, // 1650 K
  2.98072948, 0.516517906, -0.042386328, -9.05410329, // 1660 K
  2.96465641, 0.521139629, -0.0408407024, -8.92656772, // 1670 K
  2.94876015, 0.525706698, -0.0392742982, -8.80049824, // 1680 K
  2.93303815, 0.53021989, -0.0376873352, -8.67586962, // 1690 K
  2.91748792, 0.534679969, -0.0360800342, -8.55265723, // 1700 K
  2.90210701, 0.539087689, -0.0344526164, -8.430837, // 1710 K
  2.88689299, 0.54344379, -0.0328053038, -8.31038538, // 1720 K
  2.87184349, 0.547749001, -0.0311383189, -8.19127939, // 1730 K
  2.85695616, 0.552004042, -0.0294518844, -8.07349652, // 1740 K
  2.84222872, 0.556209617, -0.0277462236, -7.95701479, // 1750 K
  2.82765889, 0.560366423, -0.0260215596, -7.84181269, // 1760 K
  2.81324444, 0.564475145, -0.0242781159, -7.72786919, // 1770 K
  2.79898319, 0.568536454, -0.0225161161, -7.61516372, // 1780 K
  2.78487298, 0.572551016, -0.0207357836, -7.50367616, // 1790 K
  2.77091169, 0.576519481, -0.0189373417, -7.3933868, // 1800 K
  2.75709725, 0.580442493, -0.0171210136, -7.28427639, // 1810 K
  2.74342759, 0.584320683, -0.015287022, -7.17632607, // 1820 K
  2.7299007, 0.588154673, -0.0134355896, -7.06951738, // 1830 K
  2.7165146, 0.591945075, -0.0115669384, -6.96383227, // 1840 K
  2.70326733, 0.595692492, -0.00968128996, -6.85925304, // 1850 K
  2.69015699, 0.599397517, -0.00777886549, -6.75576238, // 1860 K
  2.67718167, 0.603060732, -0.00585988547, -6.65334334, // 1870 K
  2.66433952, 0.606682713, -0.00392456972, -6.55197932, // 1880 K
  2.65162871, 0.610264025, -0.00197313738, -6.45165405, // 1890 K
  2.63904746, 0.613805223, -5.80683044e-06, -6.35235161, // 1900 K
  2.62659398, 0.617306855, 0.00197720438, -6.2540564, // 1910 K
  2.61426654, 0.620769461, 0.00397567955, -6.15675312, // 1920 K
  2.60206343, 0.62419357, 0.00598940289, -6.0604268, // 1930 K
  2.58998297, 0.627579705, 0.00801815958, -5.96506277, // 1940 K
  2.57802349, 0.630928379, 0.0100617358, -5.87064663, // 1950 K
  2.56618338, 0.634240097, 0.0121199187, -5.77716429, // 1960 K
  2.55446103, 0.637515359, 0.0141924965, -5.68460193, // 1970 K
  2.54285486, 0.640754652, 0.0162792588, -5.59294601, // 1980 K
  2.53136332, 0.643958461, 0.0183799959, -5.50218323, // 1990 K
  2.51998488, 0.64712726, 0.0204944996, -5.41230058, // 2000 K
  2.50871805, 0.650261515, 0.0226225629, -5.32328528, // 2010 K
  2.49756135, 0.653361686, 0.0247639799, -5.23512482, // 2020 K
  2.48651331, 0.656428228, 0.026918546, -5.1478069, // 2030 K
  2.47557252, 0.659461585, 0.0290860581, -5.06131947, // 2040 K
  2.46473756, 0.662462196, 0.031266314, -4.97565072, // 2050 K
  2.45400704, 0.665430493, 0.0334591134, -4.89078905, // 2060 K
  2.44337962, 0.668366901, 0.0356642568, -4.80672307, // 2070 K
  2.43285393, 0.67127184, 0.0378815466, -4.72344163, // 2080 K
  2.42242868, 0.674145721, 0.0401107862, -4.64093376, // 2090 K
  2.41210254, 0.67698895, 0.0423517807, -4.55918872, // 2100 K
  2.40187425, 0.679801927, 0.0446043365, -4.47819595, // 2110 K
  2.39174255, 0.682585045, 0.0468682616, -4.39794508, // 2120 K
  2.3817062, 0.685338692, 0.0491433652, -4.31842595, // 2130 K
  2.37176398, 0.68806325, 0.0514294584, -4.23962857, // 2140 K
  2.36191468, 0.690759093, 0.0537263535, -4.16154313, // 2150 K
  2.35215714, 0.693426592, 0.0560338644, -4.08416001, // 2160 K
  2.34249018, 0.696066111, 0.0583518064, -4.00746976, // 2170 K
  2.33291266, 0.698678009, 0.0606799967, -3.93146308, // 2180 K
  2.32342346, 0.701262639, 0.0630182536, -3.85613086, // 2190 K
  2.31402146, 0.703820349, 0.0653663971, -3.78146414, // 2200 K
  2.30470558, 0.706351481, 0.0677242491, -3.70745412, // 2210 K
  2.29547473, 0.708856374, 0.0700916324, -3.63409216, // 2220 K
  2.28632787, 0.711335358, 0.0724683721, -3.56136976, // 2230 K
  2.27726394, 0.713788763, 0.0748542942, -3.48927857, // 2240 K
  2.26828192, 0.716216909, 0.0772492268, -3.4178104, // 2250 K
  2.2593808, 0.718620116, 0.0796529993, -3.34695718, // 2260 K
  2.25055958, 0.720998695, 0.0820654427, -3.27671099, // 2270 K
  2.24181729, 0.723352954, 0.0844863897, -3.20706405, // 2280 K
  2.23315296, 0.725683198, 0.0869156745, -3.1380087, // 2290 K
  2.22456565, 0.727989726, 0.0893531329, -3.06953743, // 2300 K
  2.2160544, 0.730272832, 0.0917986023, -3.00164283, // 2310 K
  2.20761831, 0.732532805, 0.0942519216, -2.93431764, // 2320 K
  2.19925646, 0.734769934, 0.0967129315, -2.8675547, // 2330 K
  2.19096796, 0.736984498, 0.099181474, -2.80134699, // 2340 K
  2.18275193, 0.739176776, 0.101657393, -2.73568759, // 2350 K
  2.17460751, 0.741347041, 0.104140533, -2.67056971, // 2360 K
  2.16653383, 0.743495563, 0.106630742, -2.60598665, // 2370 K
  2.15853005, 0.745622607, 0.109127868, -2.54193184, // 2380 K
  2.15059535, 0.747728434, 0.111631761, -2.47839882, // 2390 K
  2.14272891, 0.749813304, 0.114142272, -2.41538121, // 2400 K
  2.13492992, 0.75187747, 0.116659254, -2.35287275, // 2410 K
  2.1271976, 0.753921181, 0.119182563, -2.29086729, // 2420 K
  2.11953115, 0.755944687, 0.121712054, -2.22935876, // 2430 K
  2.11192981, 0.757948228, 0.124247585, -2.16834121, // 2440 K
  2.10439282, 0.759932046, 0.126789015, -2.10780875, // 2450 K
  2.09691944, 0.761896376, 0.129336205, -2.04775562, // 2460 K
  2.08950892, 0.763841451, 0.131889017, -1.98817613, // 2470 K
  2.08216054, 0.765767502, 0.134447315, -1.92906469, // 2480 K
  2.07487358, 0.767674753, 0.137010965, -1.87041579, // 2490 K
  2.06764734, 0.769563429, 0.139579832, -1.81222401, // 2500 K
  2.06048113, 0.771433749, 0.142153786, -1.75448401, // 2510 K
  2.05337426, 0.77328593, 0.144732696, -1.69719055, // 2520 K
  2.04632605, 0.775120185, 0.147316433, -1.64033844, // 2530 K
  2.03933584, 0.776936726, 0.14990487, -1.58392259, // 2540 K
  2.03240297, 0.77873576, 0.15249788, -1.52793799, // 2550 K
  2.0255268, 0.780517491, 0.15509534, -1.47237971, // 2560 K
  2.01870669, 0.782282122, 0.157697126, -1.41724288, // 2570 K
  2.01194201, 0.784029852, 0.160303117, -1.3625227, // 2580 K
  2.00523213, 0.785760877, 0.162913192, -1.30821447, // 2590 K
  1.99857646, 0.787475391, 0.165527232, -1.25431353, // 2600 K
  1.99197439, 0.789173584, 0.168145121, -1.20081532, // 2610 K
  1.98542532, 0.790855645, 0.170766742, -1.14771531, // 2620 K
  1.97892866, 0.792521759, 0.17339198, -1.09500908, // 2630 K
  1.97248385, 0.794172109, 0.176020722, -1.04269224, // 2640 K
  1.96609031, 0.795806876, 0.178652856, -0.990760492, // 2650 K
  1.95974749, 0.797426238, 0.181288271, -0.939209578, // 2660 K
  1.95345481, 0.799030371, 0.183926859, -0.888035317, // 2670 K
  1.94721175, 0.800619447, 0.18656851, -0.837233585, // 2680 K
  1.94101777, 0.802193638, 0.189213119, -0.786800318, // 2690 K
  1.93487232, 0.803753112, 0.191860579, -0.736731511, // 2700 K
  1.92877489, 0.805298035, 0.194510787, -0.687023216, // 2710 K
  1.92272496, 0.806828571, 0.197163639, -0.637671542, // 2720 K
  1.91672203, 0.808344882, 0.199819035, -0.588672656, // 2730 K
  1.91076558, 0.809847127, 0.202476874, -0.540022777, // 2740 K
  1.90485513, 0.811335465, 0.205137056, -0.491718179, // 2750 K
  1.89899018, 0.812810049, 0.207799485, -0.443755189, // 2760 K
  1.89317025, 0.814271033, 0.210464062, -0.396130184, // 2770 K
  1.88739487, 0.815718569, 0.213130693, -0.348839596, // 2780 K
  1.88166356, 0.817152806, 0.215799283, -0.301879902, // 2790 K
  1.87597587, 0.818573889, 0.21846974, -0.255247632, // 2800 K
  1.87033133, 0.819981966, 0.221141971, -0.208939364, // 2810 K
  1.8647295, 0.821377179, 0.223815886, -0.162951721, // 2820 K
  1.85916992, 0.822759669, 0.226491395, -0.117281376, // 2830 K
  1.85365217, 0.824129576, 0.229168409, -0.0719250445, // 2840 K
  1.8481758, 0.825487038, 0.231846842, -0.0268794901, // 2850 K
  1.84274039, 0.82683219, 0.234526607, 0.017858481, // 2860 K
  1.83734552, 0.828165166, 0.237207619, 0.0622920184, // 2870 K
  1.83199077, 0.829486099, 0.239889793, 0.106424229, // 2880 K
  1.82667573, 0.830795119, 0.242573048, 0.150258178, // 2890 K
  1.82139999, 0.832092355, 0.245257301, 0.193796889, // 2900 K
  1.81616315, 0.833377935, 0.247942472, 0.237043345, // 2910 K
  1.81096483, 0.834651983, 0.25062848, 0.280000487, // 2920 K
  1.80580462, 0.835914624, 0.253315247, 0.322671221, // 2930 K
  1.80068214, 0.83716598, 0.256002695, 0.36505841, // 2940 K
  1.79559702, 0.838406172, 0.258690749, 0.407164881, // 2950 K
  1.79054887, 0.839635319, 0.261379331, 0.448993425, // 2960 K
  1.78553733, 0.840853539, 0.264068368, 0.490546792, // 2970 K
  1.78056204, 0.842060947, 0.266757786, 0.5318277, // 2980 K
  1.77562262, 0.843257659, 0.269447513, 0.57283883, // 2990 K
  1.77071872, 0.844443787, 0.272137476, 0.613582827, // 3000 K
  1.76585, 0.845619444, 0.274827605, 0.654062303, // 3010 K
  1.7610161, 0.846784739, 0.27751783, 0.694279834, // 3020 K
  1.75621668, 0.847939782, 0.280208082, 0.734237966, // 3030 K
  1.75145141, 0.849084681, 0.282898294, 0.77393921, // 3040 K
  1.74671994, 0.85021954, 0.285588399, 0.813386045, // 3050 K
  1.74202195, 0.851344467, 0.28827833, 0.852580918, // 3060 K
  1.73735711, 0.852459563, 0.290968022, 0.891526245, // 3070 K
  1.7327251, 0.853564931, 0.293657411, 0.930224411, // 3080 K
  1.7281256, 0.854660672, 0.296346434, 0.968677773, // 3090 K
  1.72355829, 0.855746887, 0.299035029, 1.00688865, // 3100 K
  1.71902287, 0.856823672, 0.301723132, 1.04485935, // 3110 K
  1.71451903, 0.857891127, 0.304410685, 1.08259213, // 3120 K
  1.71004647, 0.858949346, 0.307097626, 1.12008924, // 3130 K
  1.70560489, 0.859998424, 0.309783897, 1.15735288, // 3140 K
  1.70119398, 0.861038456, 0.31246944, 1.19438523, // 3150 K
  1.69681347, 0.862069534, 0.315154196, 1.23118846, // 3160 K
  1.69246306, 0.863091749, 0.317838109, 1.26776469, // 3170 K
  1.68814246, 0.864105191, 0.320521124, 1.30411602, // 3180 K
  1.6838514, 0.86510995, 0.323203185, 1.34024454, // 3190 K
  1.6795896, 0.866106114, 0.325884238, 1.37615229, // 3200 K
  1.67535678, 0.86709377, 0.32856423, 1.41184131, // 3210 K
  1.67115268, 0.868073004, 0.331243107, 1.44731359, // 3220 K
  1.66697701, 0.869043901, 0.333920818, 1.48257112, // 3230 K
  1.66282953, 0.870006544, 0.336597311, 1.51761584, // 3240 K
  1.65870996, 0.870961017, 0.339272536, 1.55244969, // 3250 K
  1.65461805, 0.871907401, 0.341946443, 1.58707457, // 3260 K
  1.65055355, 0.872845778, 0.344618983, 1.62149238, // 3270 K
  1.64651619, 0.873776227, 0.347290107, 1.65570496, // 3280 K
  1.64250574, 0.874698827, 0.349959768, 1.68971417, // 3290 K
  1.63852194, 0.875613656, 0.352627919, 1.72352181, // 3300 K
  1.63456455, 0.876520793, 0.355294513, 1.75712969, // 3310 K
  1.63063334, 0.877420311, 0.357959506, 1.79053958, // 3320 K
  1.62672805, 0.878312288, 0.360622851, 1.82375324, // 3330 K
  1.62284846, 0.879196797, 0.363284505, 1.85677239, // 3340 K
  1.61899433, 0.880073912, 0.365944424, 1.88959875, // 3350 K
  1.61516544, 0.880943706, 0.368602565, 1.92223402, // 3360 K
  1.61136156, 0.88180625, 0.371258886, 1.95467987, // 3370 K
  1.60758245, 0.882661615, 0.373913345, 1.98693794, // 3380 K
  1.60382791, 0.883509872, 0.3765659, 2.01900989, // 3390 K
  1.60009771, 0.88435109, 0.379216513, 2.05089731, // 3400 K
  1.59639164, 0.885185336, 0.381865141, 2.08260182, // 3410 K
  1.59270947, 0.88601268, 0.384511747, 2.11412499, // 3420 K
  1.589051, 0.886833188, 0.387156292, 2.14546838, // 3430 K
  1.58541602, 0.887646925, 0.389798737, 2.17663353, // 3440 K
  1.58180432, 0.888453959, 0.392439045, 2.20762198, // 3450 K
  1.57821569, 0.889254352, 0.395077179, 2.23843522, // 3460 K
  1.57464994, 0.890048169, 0.397713104, 2.26907475, // 3470 K
  1.57110687, 0.890835473, 0.400346782, 2.29954205, // 3480 K
  1.56758626, 0.891616327, 0.402978179, 2.32983857, // 3490 K
  1.56408794, 0.892390792, 0.40560726, 2.35996575, // 3500 K
  1.5606117, 0.893158929, 0.408233991, 2.38992503, // 3510 K
  1.55715736, 0.893920798, 0.410858338, 2.4197178, // 3520 K
  1.55372472, 0.89467646, 0.413480268, 2.44934547, // 3530 K
  1.5503136, 0.895425973, 0.41609975, 2.4788094, // 3540 K
  1.54692381, 0.896169396, 0.41871675, 2.50811098, // 3550 K
  1.54355518, 0.896906785, 0.421331237, 2.53725153, // 3560 K
  1.54020751, 0.897638198, 0.42394318, 2.56623241, // 3570 K
  1.53688062, 0.898363692, 0.426552548, 2.59505491, // 3580 K
  1.53357435, 0.899083322, 0.429159312, 2.62372036, // 3590 K
  1.53028852, 0.899797142, 0.431763442, 2.65223004, // 3600 K
  1.52702295, 0.900505209, 0.434364909, 2.68058523, // 3610 K
  1.52377747, 0.901207575, 0.436963684, 2.70878718, // 3620 K
  1.52055192, 0.901904293, 0.43955974, 2.73683715, // 3630 K
  1.51734611, 0.902595417, 0.442153047, 2.76473637, // 3640 K
  1.5141599, 0.903280998, 0.44474358, 2.79248606, // 3650 K
  1.51099311, 0.903961088, 0.447331312, 2.82008744, // 3660 K
  1.50784559, 0.904635738, 0.449916215, 2.8475417, // 3670 K
  1.50471716, 0.905304998, 0.452498265, 2.87485001, // 3680 K
  1.50160768, 0.905968918, 0.455077436, 2.90201356, // 3690 K
  1.49851698, 0.906627547, 0.457653702, 2.9290335, // 3700 K
];

//=== END GENERATED

/// Shaders.cpp's `assemble`: the version line, the filament library for the
/// passes that need it, then the body.
const assemble = (body, withFilament) => K_VERSION + (withFilament ? FILAMENT : '') + body;
const VERTEX = assemble(VERTEX_BODY, false);
const DRIVE = assemble(DRIVE_BODY, false);
const THERMAL = assemble(THERMAL_BODY, true);
const FILL = assemble(FILL_BODY, false);
const REGRID = assemble(REGRID_BODY, false);
const BLUR = assemble(BLUR_BODY, false);
const OUTPUT = assemble(OUTPUT_BODY, true);

/// The harness-only hooks, at what the shipped plugin sets them to.
const PERTURB = 0;
const PROBE = 0;

//===========================================================================
// Controls.cpp, ported. What a host parameter means.
//
// The plugin stores every host value as a float; the page's are doubles from
// a slider, so each is rounded through Math.fround first. Watts and
// AmbientKelvin compute in double from that float, as the C++ does; the float
// laws (Gap, Exposure) round through fround as the C++ computes in float.
//===========================================================================

const f = Math.fround;
const clampTo = (v, lo, hi) => Math.min(Math.max(v, lo), hi);
const clamp01f = (value) => clampTo(f(value), 0.0, 1.0);
/// std::lround: half away from zero.
const lround = (value) => (value < 0 ? -Math.round(-value) : Math.round(value));

/// Wattage: 10 W to 1000 W, geometric. 0.5 is 100 W.
const wattsOf = (value) => 10.0 * Math.pow(10.0, 2.0 * clamp01f(value));
const wattsParam = (watts) => clamp01f(f(Math.log10(Math.max(watts, 10.0) / 10.0) / 2.0));

/// Ambient Temp: 0 to 200 C, linear, in kelvin. 0.125 is 25 C.
const ambientKelvin = (value) => 273.15 + 200.0 * clamp01f(value);

/// Gap: the fraction of a cell between bulbs, 0 to 0.8, linear.
const gapFraction = (value) => f(f(0.8) * clamp01f(value));

/// Exposure: -4 to +4 stops, linear. 0.5 is 0.
const exposureStops = (value) => f(f(8.0 * clamp01f(value)) - 4.0);

/// Glow, Bloom and Mix are 0..1 amounts.
const amount = (value) => clamp01f(value);

const glassFrosted = (index) => index === 1;
const mainsHertz = (index) => (clampTo(index, 0, CONTROLS_H.kMainsCount - 1) === 1 ? 60.0 : 50.0);
const optionIndex = (value, count) => clampTo(lround(f(value)), 0, count - 1);

//===========================================================================
// Lamp.cpp, ported. The lamp as numbers, in double. The constants and tables
// are the generated copies above (LAMP_H, LAMP_kRho, LAMP_kShomate*).
//===========================================================================

const L = LAMP_H;
const RHO = LAMP_kRho;

/// The shader's rule exactly: the segment index clamped to the table, the
/// fraction not, so the end segments extrapolate.
function resistivity(kelvin) {
  const x = (kelvin - L.kRhoFirst) / L.kRhoStep;
  const i = clampTo(Math.floor(x), 0, L.kRhoCount - 2);
  const t = x - i;
  return RHO[i] + (RHO[i + 1] - RHO[i]) * t;
}

/// J / ( kg K ): NIST-JANAF's Shomate fit per mole, over the molar mass.
function heatCapacity(kelvin) {
  const a = kelvin < L.kShomateSplit ? LAMP_kShomateLow : LAMP_kShomateHigh;
  const t = kelvin / 1000.0;
  const perMole = a[0] + a[1] * t + a[2] * t * t + a[3] * t * t * t + a[4] / (t * t);
  return perMole / L.kTungstenMolarMass;
}

/// 50 Hz is a 230 V lamp, 60 Hz a 120 V one.
const mainsVolts = (hertz) => (hertz > 55.0 ? 120.0 : 230.0);

/// lamp::Make: the wire that dissipates `watts` at the mains voltage with its
/// wire at 2800 K, sized by the two equations in Lamp.h, and its mass.
function makeLamp(watts, hertz) {
  const volts = mainsVolts(hertz);
  const rhoRated = resistivity(L.kRatedKelvin) * 1e-8; // ohm m
  const radiated = (1.0 - L.kConductionFraction) * watts;
  const fourth = Math.pow(L.kRatedKelvin, 4.0) - Math.pow(L.kReferenceAmbient, 4.0);
  const d3 = 4.0 * rhoRated * radiated * watts
    / (L.kCoilRadiatingFraction * L.kEmissivity * L.kStefanBoltzmann * L.kPi * L.kPi * volts * volts * fourth);
  const diameter = Math.cbrt(d3);
  const length = radiated / (L.kCoilRadiatingFraction * L.kEmissivity * L.kStefanBoltzmann * L.kPi * diameter * fourth);
  const mass = L.kTungstenDensity * L.kPi * diameter * diameter / 4.0 * length;
  return { watts, hertz, volts, diameter, length, mass, wattsPerKg: watts / mass };
}

/// lamp::Derivative with every Perturb bit at 0: dT/dt in K/s.
function derivative(lamp, T, driveSquared, ambient) {
  const rhoRated = resistivity(L.kRatedKelvin);
  const rho = resistivity(T);
  const into = driveSquared * rhoRated / rho;
  const T2 = T * T;
  const A2 = ambient * ambient;
  const radiated = (1.0 - L.kConductionFraction) * (T2 * T2 - A2 * A2)
    / (Math.pow(L.kRatedKelvin, 4.0) - Math.pow(L.kReferenceAmbient, 4.0));
  const conducted = L.kConductionFraction * (T - ambient) / (L.kRatedKelvin - L.kReferenceAmbient);
  return lamp.wattsPerKg * (into - radiated - conducted) / heatCapacity(T);
}

/// lamp::Stiffness: the stiffest |d(dT/dt)/dT| over every temperature from the
/// ambient to melting, at the peak of the mains cycle and with no drive.
function stiffness(lamp, ambient) {
  let worst = 0.0;
  for (let T = Math.max(150.0, ambient); T <= L.kMeltingKelvin; T += 5.0) {
    const e = 0.5;
    const slope = (derivative(lamp, T + e, 2.0, ambient) - derivative(lamp, T - e, 2.0, ambient)) / (2.0 * e);
    worst = Math.max(worst, Math.abs(slope));
    const slope0 = (derivative(lamp, T + e, 0.0, ambient) - derivative(lamp, T - e, 0.0, ambient)) / (2.0 * e);
    worst = Math.max(worst, Math.abs(slope0));
  }
  return worst;
}

//===========================================================================
// The renderer: Filament::InitGL and Filament::ProcessOpenGL, in their order.
//===========================================================================

/// What the line under the canvas reports. Filled by the renderer.
const telemetry = {
  ticked: false,
  now: 0, dt: 0, substeps: 0, h: 0, phase0: 0,
  columns: 0, rows: 0, lamp: null, stiffness: 0, regrids: 0,
};

/// Driven checks only (see AGENTS.md, "The browser demo"): `afterRender`, if
/// set, is called at the end of each render with the context and the input,
/// before the browser composites -- the only moment the canvas can be read;
/// `fresh()` puts the renderer back to a new instance's state.
const hooks = { afterRender: null, lastInput: null, fresh: null };

function createRenderer(gl, quad) {
  const program = (fragment, label) => new Program(gl, VERTEX, fragment, label);
  const driveShader = program(DRIVE, 'drive');
  const thermalShader = program(THERMAL, 'thermal');
  const fillShader = program(FILL, 'fill');
  const regridShader = program(REGRID, 'regrid');
  const blurShader = program(BLUR, 'blur');
  const outputShader = program(OUTPUT, 'output');

  // The colour table: one row of texels, read with texelFetch only.
  const planckTexture = gl.createTexture();
  gl.bindTexture(gl.TEXTURE_2D, planckTexture);
  gl.texStorage2D(gl.TEXTURE_2D, 1, gl.RGBA32F, PLANCK_COUNT, 1);
  gl.pixelStorei(gl.UNPACK_FLIP_Y_WEBGL, false);
  gl.pixelStorei(gl.UNPACK_PREMULTIPLY_ALPHA_WEBGL, false);
  gl.texSubImage2D(gl.TEXTURE_2D, 0, 0, 0, PLANCK_COUNT, 1, gl.RGBA, gl.FLOAT, new Float32Array(PLANCK_TABLE));
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
  gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
  gl.bindTexture(gl.TEXTURE_2D, null);

  // The uniforms setFilamentUniforms hands every pass, rounded to float as
  // the C++'s static_casts round them.
  const rhoF = new Float32Array(RHO);
  const lowF = new Float32Array(LAMP_kShomateLow.map((a) => a / L.kTungstenMolarMass));
  const highF = new Float32Array(LAMP_kShomateHigh.map((a) => a / L.kTungstenMolarMass));
  const fourth = Math.pow(L.kRatedKelvin, 4.0) - Math.pow(L.kReferenceAmbient, 4.0);

  // The buffers, as the plugin's PassBuffer::Ensure( ..., format, sampling ).
  const nearest = { filter: 'nearest' };
  const state = [new PassBuffer(gl, nearest), new PassBuffer(gl, nearest)];
  const drive = new PassBuffer(gl, nearest);
  const light = new PassBuffer(gl, nearest);
  const bloomX = new PassBuffer(gl, nearest);
  const bloomY = new PassBuffer(gl, { filter: 'linear' });
  const isValid = (buffer) => buffer.texture !== null;

  //--- Filament's members ----------------------------------------------------
  let current = 0;
  let lastNow = -1.0;
  let lampKey = '';
  let lamp = null;
  let stiff = 0.0;
  let mrtChecked = false;

  function setFilamentUniforms(shader, law, ambient) {
    shader.setArray('Rho', rhoF, 1);
    shader.setArray('ShomateLow', lowF, 1);
    shader.setArray('ShomateHigh', highF, 1);
    shader.set('RhoRated', resistivity(L.kRatedKelvin));
    shader.set('WattsPerKg', lamp.wattsPerKg);
    shader.set('RadiatedNorm', (1.0 - L.kConductionFraction) / fourth);
    shader.set('ConductedNorm', L.kConductionFraction / (L.kRatedKelvin - L.kReferenceAmbient));
    shader.set('LinearNorm', (1.0 - L.kConductionFraction) / (L.kRatedKelvin - L.kReferenceAmbient));
    shader.set('Ambient', ambient);
    shader.setInt('Law', law);
    shader.setInt('Perturb', PERTURB);
    shader.set('PlanckFirst', PLANCK_FIRST_KELVIN);
    shader.set('PlanckStep', PLANCK_STEP_KELVIN);
    shader.setInt('PlanckCount', PLANCK_COUNT);
    shader.set('GelRed', MODEL_kGelRed[0], MODEL_kGelRed[1], MODEL_kGelRed[2]);
    shader.set('GelGreen', MODEL_kGelGreen[0], MODEL_kGelGreen[1], MODEL_kGelGreen[2]);
    shader.set('GelBlue', MODEL_kGelBlue[0], MODEL_kGelBlue[1], MODEL_kGelBlue[2]);
  }

  const unbind = (...units) => { for (const u of units) bindTexture(gl, u, null); };

  /// Driven checks only: what a freshly instantiated plugin holds -- no state
  /// buffers (so the next frame fills the wall at the ambient), no clock.
  hooks.fresh = () => {
    for (const s of state) s.dispose();
    current = 0;
    lastNow = -1.0;
    lampKey = '';
  };

  return {
    render({ input, params, width: vpW, height: vpH, time }) {
      const p = (id) => params.get(id);
      const width = input.width;
      const height = input.height;
      hooks.lastInput = input;

      //------------------------------------------------------------------
      // The clock. The frame's seconds and the mains phase at its start are
      // reduced here in double; the shaders see Phase0, StepSeconds and
      // StepCycles and nothing absolute.
      //------------------------------------------------------------------
      const now = time;
      let dt = MODEL_H.kNominalFrame;
      if (lastNow >= 0.0) dt = clampTo(now - lastNow, 0.0, MODEL_H.kMaxFrameDelta);
      lastNow = now;

      //------------------------------------------------------------------
      // What the controls say.
      //------------------------------------------------------------------
      const columns = clampTo(lround(f(integerValue('columns', p('columns')))), CONTROLS_H.kMinColumns, CONTROLS_H.kMaxColumns);
      const rows = clampTo(lround(f(integerValue('rows', p('rows')))), CONTROLS_H.kMinRows, CONTROLS_H.kMaxRows);
      const gap = gapFraction(p('gap'));
      const pixelMode = f(p('pixelMode')) >= 0.5;
      const glass = optionIndex(p('glass'), CONTROLS_H.kGlassCount);
      const gels = optionIndex(p('gels'), CONTROLS_H.kGelsCount);
      const watts = wattsOf(p('wattage'));
      const law = optionIndex(p('dimmer'), CONTROLS_H.kDimmerCount);
      const hertz = mainsHertz(optionIndex(p('mains'), CONTROLS_H.kMainsCount));
      const ambient = ambientKelvin(p('ambient'));
      const glow = amount(p('glow'));
      const bloom = amount(p('bloom'));
      const exposure = f(f(MODEL_H.kExposureGain) * f(Math.pow(2.0, exposureStops(p('exposure')))));
      const mixAmount = amount(p('mix'));

      // The lamp, and how finely its ODE has to be stepped: recomputed only
      // when something it depends on moves (the stiffness scans the range).
      const key = `${watts}/${hertz}/${ambient}`;
      if (key !== lampKey) {
        lamp = makeLamp(watts, hertz);
        stiff = stiffness(lamp, ambient);
        lampKey = key;
      }
      let substeps = 0;
      if (dt > 0.0) {
        const hMax = Math.min(0.5 / hertz / L.kStepsPerHalfCycle, L.kStability / stiff);
        substeps = clampTo(Math.trunc(Math.ceil(dt / hMax - 1e-9)), 1, L.kMaxSubsteps);
      }
      const h = substeps > 0 ? dt / substeps : 0.0;
      const start = now - dt;
      const phase0 = start * hertz - Math.floor(start * hertz);

      //------------------------------------------------------------------
      // Buffers, every allocation before anything binds a texture for the
      // frame's passes. The temperatures live at the GRID's raster, so an
      // output resize leaves them alone; a grid change carries them across.
      //------------------------------------------------------------------
      const live = state[current];
      let fill = false;
      if (!isValid(live)) {
        live.ensure(columns, rows, gl.RGBA32F);
        fill = true;
      } else if (live.width !== columns || live.height !== rows) {
        const other = state[1 - current];
        const oldColumns = live.width;
        const oldRows = live.height;
        other.ensure(columns, rows, gl.RGBA32F);
        other.bind();
        regridShader.use();
        bindTexture(gl, 0, live.texture);
        regridShader.setSampler('State', 0);
        regridShader.set('OldGrid', oldColumns, oldRows);
        regridShader.set('NewGrid', columns, rows);
        quad.draw();
        unbind(0);
        current = 1 - current;
        telemetry.regrids += 1;
      }

      const src = state[current];
      const dst = state[1 - current];
      dst.ensure(columns, rows, gl.RGBA32F);
      drive.ensure(columns, rows, gl.RGBA32F);
      light.ensure(columns, rows, gl.RGBA32F);
      bloomX.ensure(columns, rows, gl.RGBA16F);
      bloomY.ensure(columns, rows, gl.RGBA16F);

      if (fill) {
        src.bind();
        fillShader.use();
        fillShader.set('Kelvin', ambient);
        quad.draw();
      }

      //------------------------------------------------------------------
      // 1. Drive: each bulb's level. The kit's clip is exactly the canvas's
      // raster, so SourceSize is the input's size and MaxUV is 1.
      //------------------------------------------------------------------
      drive.bind();
      driveShader.use();
      bindTexture(gl, 0, input.texture);
      driveShader.setSampler('Source', 0);
      gl.uniform2i(driveShader.location('SourceSize'), width, height);
      driveShader.set('Grid', columns, rows);
      quad.draw();
      unbind(0);

      //------------------------------------------------------------------
      // 2. Thermal: the ODE over the frame, to the other state buffer and the
      // light buffer at once -- the light attached as a second colour target
      // on the state buffer's framebuffer, as the plugin attaches it to the
      // SDK's, and taken off again afterwards.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, dst.fbo);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.TEXTURE_2D, light.texture, 0);
      gl.drawBuffers([gl.COLOR_ATTACHMENT0, gl.COLOR_ATTACHMENT1]);
      if (!mrtChecked) {
        const status = gl.checkFramebufferStatus(gl.FRAMEBUFFER);
        if (status !== gl.FRAMEBUFFER_COMPLETE) {
          throw new GLError(`This browser will not render the thermal pass's two RGBA32F targets at once (framebuffer status 0x${status.toString(16)}). The plugin writes the new temperatures and the shutter-averaged light in one pass, and the page will not split it into something the plugin does not do.`);
        }
        mrtChecked = true;
      }
      gl.viewport(0, 0, dst.width, dst.height);
      thermalShader.use();
      bindTexture(gl, 0, src.texture);
      bindTexture(gl, 1, drive.texture);
      bindTexture(gl, 2, planckTexture);
      setFilamentUniforms(thermalShader, law, ambient);
      thermalShader.setSampler('State', 0);
      thermalShader.setSampler('Drive', 1);
      thermalShader.setSampler('Planck', 2);
      thermalShader.setInt('Substeps', substeps);
      thermalShader.set('StepSeconds', h);
      thermalShader.set('StepCycles', h * hertz);
      thermalShader.set('Phase0', phase0);
      thermalShader.setInt('Gels', gels);
      quad.draw();
      unbind(2, 1, 0);
      // Leave the framebuffer as it was made: one target.
      gl.drawBuffers([gl.COLOR_ATTACHMENT0]);
      gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT1, gl.TEXTURE_2D, null, 0);
      current = 1 - current;
      const latest = state[current];

      //------------------------------------------------------------------
      // 3. Bloom: the light, blurred at the grid's raster, x then y.
      //------------------------------------------------------------------
      for (const pass of [{ target: bloomX, from: light, dx: 1, dy: 0 }, { target: bloomY, from: bloomX, dx: 0, dy: 1 }]) {
        pass.target.bind();
        blurShader.use();
        bindTexture(gl, 0, pass.from.texture);
        blurShader.setSampler('Light', 0);
        gl.uniform2i(blurShader.location('Direction'), pass.dx, pass.dy);
        blurShader.set('Sigma', MODEL_H.kBloomSigmaCells);
        quad.draw();
        unbind(0);
      }

      //------------------------------------------------------------------
      // 4. The wall, straight to the canvas. The host's viewport is the
      // whole canvas.
      //------------------------------------------------------------------
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, vpW, vpH);
      outputShader.use();
      bindTexture(gl, 0, light.texture);
      bindTexture(gl, 1, latest.texture);
      bindTexture(gl, 2, bloomY.texture);
      bindTexture(gl, 3, input.texture);
      bindTexture(gl, 4, planckTexture);
      const tint = GLASS_TINTS[clampTo(glass, 0, CONTROLS_H.kGlassCount - 1)];
      setFilamentUniforms(outputShader, law, ambient);
      outputShader.setSampler('Light', 0);
      outputShader.setSampler('State', 1);
      outputShader.setSampler('Bloom', 2);
      outputShader.setSampler('Source', 3);
      outputShader.setSampler('Planck', 4);
      outputShader.set('MaxUV', 1.0, 1.0);
      outputShader.set('Grid', columns, rows);
      outputShader.set('Size', vpW, vpH);
      outputShader.set('Gap', gap);
      outputShader.setInt('PixelMode', pixelMode ? 1 : 0);
      outputShader.setInt('Frosted', glassFrosted(glass) ? 1 : 0);
      outputShader.set('Glass', tint[0], tint[1], tint[2]);
      outputShader.set('Glow', glow);
      outputShader.set('BloomAmount', bloom);
      outputShader.set('ExposureGain', exposure);
      outputShader.set('Wall', MODEL_kWall[0], MODEL_kWall[1], MODEL_kWall[2]);
      outputShader.set('Sheen', MODEL_H.kGlassSheen);
      outputShader.set('MixAmount', mixAmount);
      outputShader.setInt('Probe', PROBE);
      outputShader.setInt('Gels', gels);
      gl.disable(gl.BLEND);
      quad.draw();

      telemetry.ticked = true;
      telemetry.now = now;
      telemetry.dt = dt;
      telemetry.substeps = substeps;
      telemetry.h = h;
      telemetry.phase0 = phase0;
      telemetry.columns = columns;
      telemetry.rows = rows;
      telemetry.lamp = lamp;
      telemetry.stiffness = stiff;
      telemetry.law = law;
      telemetry.gels = gels;

      if (hooks.afterRender) hooks.afterRender(gl, input);
      unbind(4, 3, 2, 1, 0);
    },
  };
}

//===========================================================================
// The controls, read out of Filament::Filament(). Same names, same groups,
// same order, same defaults, same dropdown elements. Absent: the About block.
//===========================================================================

/// FF_TYPE_INTEGER is exempt from the 0..1 clamp, so the plugin stores Columns
/// and Rows as the integers themselves. The kit has no integer control, so each
/// is a dropdown of every value in the plugin's range; `integerValue` turns the
/// dropdown's index back into the integer.
const INTEGER_RANGES = {
  columns: [CONTROLS_H.kMinColumns, CONTROLS_H.kMaxColumns],
  rows: [CONTROLS_H.kMinRows, CONTROLS_H.kMaxRows],
};
const INTEGER_ELEMENTS = {};
for (const [id, [low, high]] of Object.entries(INTEGER_RANGES)) {
  INTEGER_ELEMENTS[id] = [];
  for (let v = low; v <= high; v += 1) INTEGER_ELEMENTS[id].push(String(v));
}
function integerValue(id, index) {
  const [low, high] = INTEGER_RANGES[id];
  return Math.min(high, Math.max(low, low + Math.round(index)));
}
const integerIndex = (id, value) => value - INTEGER_RANGES[id][0];

const integer = (id, name, value, group, hint) => ({ id, name, type: 'option', elements: INTEGER_ELEMENTS[id], default: integerIndex(id, value), group, hint });
const std = (id, name, def, group, extra = {}) => ({ id, name, type: 'standard', default: def, group, ...extra });
const opt = (id, name, elements, def, group, hint) => ({ id, name, type: 'option', elements, default: def, group, hint });
const bool = (id, name, def, group, hint) => ({ id, name, type: 'boolean', default: def, group, hint });

const wattsText = (v) => {
  const w = wattsOf(v);
  return `${w < 100 ? w.toFixed(1) : w.toFixed(0)} W`;
};
const signed = (x, digits) => `${x >= 0 ? '+' : '−'}${Math.abs(x).toFixed(digits)}`;

const demo = mountDemo({
  name: 'Filament',
  pluginId: 'FI01',
  kind: 'effect',
  tagline:
    'The picture on a wall of incandescent bulbs. Each bulb is a tungsten filament whose dimmer is set by its cell of the clip, and whose temperature follows the filament’s own heat balance: power in through a resistance that rises fifteenfold from cold, out by radiation as T⁴ and by conduction. Dim a bulb and it goes red as well as dark; a flash rises fast and falls slowly; a cold bulb surges; a small filament flickers at twice the mains. The light is Planck’s law at the filament’s temperature through the CIE observer.',
  repo: 'https://github.com/stoatworks-labs/filament',
  page: 'https://stoatworks-labs.com/software/filament/',

  // The stock sentence says "same maths", which is most of the truth here:
  // the physics runs in the plugin's shaders, but the clock, the lamp sizing
  // and the substep rule are a hand port, and three constants are assumptions.
  blurb:
    'It is Filament’s own GLSL — the drive, thermal, bloom and output passes and the filament library they share — ported from the repository to WebGL2, so the heat balance itself runs on your GPU as it does in the plugin: RK4 substeps, tungsten’s resistivity table, its heat capacity, the mains waveform through the dimmer and the Planck colour table, over a float temperature per bulb carried from frame to frame. The CPU half — the clock and the mains phase, the lamp’s wire sized from its wattage, the substep rule and every control’s law — is ported to JavaScript by hand, and nothing checks that port but a reader. The emissivity (0.30), the coil radiating from half its surface and the 15% conducted are the plugin’s assumptions, not measurements. It runs on generated clips in this page, with the plugin’s own parameters and no install.',

  // Every per-bulb buffer is a float render target, as in the plugin.
  needFloat: true,

  params: [
    integer('columns', 'Columns', 48, 'Wall',
      'Bulbs across, 4 to 160. FF_TYPE_INTEGER in the plugin; a dropdown of the same values here. A change carries every filament’s temperature across to the new grid (the bulb under each new bulb’s centre), so it does not restart the wall cold.'),
    integer('rows', 'Rows', 27, 'Wall',
      'Bulbs down, 2 to 90. FF_TYPE_INTEGER in the plugin; a dropdown of the same values here.'),
    std('gap', 'Gap', 0.3, 'Wall', {
      display: (v) => `${(100 * gapFraction(v)).toFixed(0)}% of a cell`,
      hint: 'The fraction of a cell between bulbs, 0 to 80%, linear.',
    }),
    bool('pixelMode', 'Pixel Mode', 0, 'Wall',
      'Each bulb drawn as a flat square pixel of its cell, on the same grid, instead of a bulb with a coil in it. The filaments are the same.'),
    opt('glass', 'Glass', GLASS_NAMES, 0, 'Wall',
      'Clear shows the coil; Frosted a lit globe; Amber, Red, Green and Blue tint the glass (transmittances chosen by eye in the plugin, not measured from filters).'),
    opt('gels', 'Gels', GELS_NAMES, 0, 'Wall',
      'Off: one white filament per cell, driven by the cell’s luma. RGB: three filaments per cell behind red, green and blue gels, each driven by its own channel, drawn as one bulb. A blue gel over tungsten is dim, because tungsten has little blue; that is left in.'),

    std('wattage', 'Wattage', wattsParam(40.0), 'Bulb', {
      display: wattsText,
      hint: '10 W to 1000 W, geometric (0.5 is 100 W). Not a brightness: every lamp at rated reads the same. A bigger filament has more mass per watt, so it is slower to heat and to cool and ripples less. The line under the picture shows the wire this sizes.',
    }),
    opt('dimmer', 'Dimmer', DIMMER_NAMES, 1, 'Bulb',
      'The dimmer law from the cell’s level to the voltage. Linear: V_rms = level (a sine-wave dimmer; very contrasty, light goes as ~V^3.4). Square Law: V_rms² = level, power roughly following the fader. Triac: a leading-edge phase cut at firing angle (1 − level) π, which ripples most.'),
    opt('mains', 'Mains', MAINS_NAMES, 0, 'Bulb',
      '50 Hz is a 230 V lamp, 60 Hz a 120 V one. It sets the ripple’s frequency (twice the mains) and, through the voltage, the filament’s size: a 120 V lamp is thicker and slower than a 230 V one of the same wattage.'),
    std('ambient', 'Ambient Temp', 0.125, 'Bulb', {
      display: (v) => `${(ambientKelvin(v) - 273.15).toFixed(1)} °C`,
      hint: '0 to 200 °C, linear (0.125 is 25 °C): the temperature an unlit filament cools towards.',
    }),

    std('glow', 'Glow', 0.5, 'Look', {
      hint: 'How much of each bulb’s halo there is: light scattered by the glass and thrown forward by the reflector.',
    }),
    std('bloom', 'Bloom', 0.35, 'Look', {
      hint: 'The light blurred across the grid (a Gaussian of 1.8 cells), as a lens and the air would.',
    }),
    std('exposure', 'Exposure', 0.5, 'Look', {
      display: (v) => `${signed(exposureStops(v), 2)} stops`,
      hint: '−4 to +4 stops, linear; 0.5 is 0. Scene light to the picture is 1 − exp(−2.2 × 2^stops × L) per channel, then sRGB.',
    }),
    std('mix', 'Mix', 1.0, 'Look', {
      hint: 'The wall against the input. Alpha is mix(source alpha, 1, Mix): the wall paints the whole frame, so it is opaque at 1.',
    }),
  ],

  // Lights on black shows the physics best: a light that arrives lights its
  // bulbs within tens of milliseconds, and one that leaves leaves them glowing
  // red for a fifth of a second. The ramps show the warm fade.
  sources: ['spot', 'scene', 'ramp', 'bars', 'grid', 'alpha'],

  // The plugin ships no factory presets. These are the page's own, expressed
  // entirely in the plugin's parameters and reachable with the controls.
  presets: {
    '10 W on a triac (quick, flickery)': { wattage: wattsParam(10.0), dimmer: 2 },
    '1000 W (slow and heavy)': { wattage: wattsParam(1000.0) },
    'Linear dimmers (contrasty)': { dimmer: 0 },
    '60 Hz, 120 V lamps': { mains: 1 },
    'Frosted glass': { glass: 1 },
    'Amber glass': { glass: 2 },
    'RGB gels': { gels: 1 },
    'Pixel Mode, fine grid': { pixelMode: 1, gap: 0.1, columns: integerIndex('columns', 96), rows: integerIndex('rows', 54) },
    'Big wall, 160 × 90': { columns: integerIndex('columns', 160), rows: integerIndex('rows', 90), gap: 0.2 },
    'Few big bulbs, 16 × 9': { columns: integerIndex('columns', 16), rows: integerIndex('rows', 9) },
  },

  differences: [
    'The physics runs in the plugin’s own shaders, and those are not a port. The drive, thermal, fill, regrid, blur and output bodies and the filament library they share — resistivity, c_p, the Taylor sinPi, the mains waveform through the dimmer law, the RK4 derivative, the colour lookup — are the plugin’s GLSL, assembled as Shaders.cpp assembles them; the numbers they are handed (Lamp.h’s resistivity table, Shomate coefficients and design point, Model.h’s constants, the glass tints and the 346-row Planck table) are copied by a script. demo/tools/check_shaders.py fails the repository’s verify script if a character of any of it drifts.',
    'The CPU half is a PORT, not the plugin’s own code: Controls.cpp’s laws, Lamp.cpp’s wire sizing (diameter, length and mass from the wattage and the mains voltage, so C(T)/P), its derivative in double and its stiffness scan, and Filament::ProcessOpenGL’s clock (the frame’s seconds and the mains phase at its start reduced in double), its substep rule (ceil(dt / hMax), hMax = min(half-cycle / 20, 1.5 / stiffness), capped at 4096, dt clamped to 0–0.25 s), its lamp cache, the fill, the regrid on a grid change and the pass order. All of it is ported by hand, in JavaScript doubles as the plugin keeps them, rounded to float where the plugin hands a float over. Nothing checks a port but a reader; the repository’s fitest --steady, --rise, --fall, --ripple, --colour and --resize check the C++ and have never heard of this page.',
    'Three of the lamp’s constants are assumptions, in the plugin and so here: the emissivity 0.30, the coil radiating from half its surface, and 15% of the rated power conducted. They are round figures, not read from a table, and they size the wire and so the time constants — nothing else. The model has not been measured against a real lamp; its orders of magnitude are right (a 60 W 230 V wire comes out 34 µm × 0.92 m).',
    'The per-bulb buffers are the plugin’s: two RGBA32F temperature buffers ping-ponged, an RGBA32F drive and light, the bloom at RGBA16F; the thermal pass writes two render targets at once. WebGL2 renders into float textures only with EXT_color_buffer_float, and the page refuses to start without it, or if a browser will not render the two targets together, rather than fall back to something the plugin does not do.',
    'The clock is the kit’s, in declared seconds; the plugin’s unit vote and wall-clock fallback never run. Everything downstream is the plugin’s rule: dt is the frame delta clamped to 0–0.25 s (the kit itself caps a delta at 0.1 s, so a stalled tab integrates at most 0.1 s), a nominal 1/60 on the first frame. A paused page renders only when a control moves, and each such frame is worth 0 s: no substeps, the light of the instant rather than the shutter’s average, as the plugin does when a host’s clock stops. Restart sends the clock back to 0, which the plugin’s clamp also reads as a frame of 0 s, so the wall keeps its temperatures; Step adds exactly 1/60 s.',
    'The light is shutter-averaged over each frame’s substeps, as in the plugin, so what you see depends on the browser’s frame rate as a camera’s picture depends on its shutter: at 60 fps a 10 W bulb on a triac at 50 Hz keeps a small 20 Hz beat; at 60 Hz mains and 60 fps there is none. A browser that renders at 120 fps averages half as long.',
    'Columns and Rows are FF_TYPE_INTEGER in the plugin, 4–160 and 2–90, with real ranges. The kit has no integer control, so each is a dropdown of the same values.',
    'The plugin stores each host value as a float; the page’s sliders are doubles, so every value is rounded through Math.fround before its law is applied, and the defaults are the plugin’s float defaults (Wattage 0.30103, which is 40 W).',
    'Output alpha is mix(source alpha, 1, Mix), as in the plugin: opaque at Mix 1. The page has no backdrop menu, so at Mix below 1 on the transparency clip the transparent area shows the page’s black.',
    'The harness-only Perturb and Probe uniforms are set to what the shipped plugin sets them to, 0. The six negative controls and the raw temperature probes fitest reads through them are not on this page. The About block is absent, as on every page in this suite.',
    'The plugin’s proof — a constant RMS settling at the power balance (2800 K at rated), the light on the Planckian locus and redward as it dims, a cold start following R(T) (t90 65 ms for 60 W), switch-off following T⁴ plus conduction, triac ripple at exactly twice the mains and bigger on a small lamp, temperatures surviving a resize and a regrid — is an offline harness in the repository, at two rasters and on a software renderer. Nothing on this page measures anything; the line under the picture reports what the ported lamp and substep rule are doing.',
  ],

  createRenderer,
});

// For a driven check (AGENTS.md, "The browser demo"): the kit's state and
// redraw, the telemetry, and the hooks, so a script can pause, set the clock
// to n / 60 and render one frame at a time, as `fitest --pipe --fps 60` does.
window.__filamentDemo = { demo, telemetry, hooks };

//---------------------------------------------------------------------------
// Under the canvas: a line reporting what the ported lamp and substep rule are
// doing. Skipped in embed mode, where there is no reader.
//---------------------------------------------------------------------------
if (demo && !new URLSearchParams(window.location.search).has('embed')) {
  const stage = document.querySelector('.stage');
  if (stage) {
    const line = document.createElement('p');
    line.className = 'stage__status';
    stage.append(line);
    setInterval(() => {
      if (!telemetry.ticked || !telemetry.lamp) return;
      const t = telemetry;
      const lampNow = t.lamp;
      line.textContent =
        `Lamp: ${lampNow.watts < 100 ? lampNow.watts.toFixed(1) : lampNow.watts.toFixed(0)} W at ${lampNow.volts.toFixed(0)} V ${lampNow.hertz.toFixed(0)} Hz, `
        + `a wire ${(lampNow.diameter * 1e6).toFixed(1)} µm × ${lampNow.length.toFixed(2)} m, ${(lampNow.mass * 1e6).toFixed(2)} mg; `
        + `stiffness ${t.stiffness.toFixed(0)}/s. This frame: ${t.dt.toFixed(4)} s in ${t.substeps} RK4 substep${t.substeps === 1 ? '' : 's'}`
        + `${t.substeps ? ` of ${(t.h * 1000).toFixed(3)} ms` : ' (a stopped clock shows the instant)'}, mains phase ${t.phase0.toFixed(3)} cycles at its start; `
        + `${t.columns} × ${t.rows} = ${t.columns * t.rows} bulbs${t.gels === 1 ? ' × 3 gelled filaments' : ''}, ${DIMMER_NAMES[t.law]} dimmers; clock ${t.now.toFixed(3)} s.`;
    }, 250);
  }
}
