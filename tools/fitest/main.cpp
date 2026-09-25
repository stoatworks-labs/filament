/**
	fitest -- render Filament offline, and read the filaments back out of it.

	Every check here drives the REAL plugin class through a headless GL
	context on a synthetic clock, reads the plugin's own state through its
	probe hooks, and sets it against the stated model integrated here,
	independently, in double:

		fitest --out /tmp/frame.png     a picture, on the moving test card
		fitest --list                   every parameter, its kind and default
		fitest --steady                 at a constant RMS the filament settles
		                                where power in balances power out, and
		                                at rated voltage at the rated 2800 K
		fitest --colour                 the light at each temperature sits on
		                                the Planckian locus (this harness's
		                                own CMF integration; McCamy's CCT),
		                                and dimming moves it redward
		fitest --rise                   a step on from cold follows the R( T )
		                                ODE, and beats a constant-R lamp
		fitest --fall                   switch-off follows the radiative plus
		                                conductive solution, not an exponential
		fitest --ripple                 a triac-dimmed filament ripples at
		                                exactly twice the mains, more for a
		                                small lamp than a big one
		fitest --resize                 an output resize and a grid change
		                                mid-fade keep every temperature
		fitest --negative               every check above can FAIL
		fitest --names                  nothing the host will truncate
		fitest --bench                  the render cost and the state held
		fitest --dump-shaders DIR       the exact GLSL the plugin compiles
		fitest --pipe                   raw frames in, raw frames out

	The reference integrates Lamp.h's ODE with Lamp.cpp's constants -- the
	STATED model -- on the plugin's own substep schedule (read back from the
	plugin each frame) and on one sixteen times finer. The first bounds what
	float arithmetic on the GPU may add; the difference between the two is
	the plugin's discretisation error, measured, not assumed. AGENTS.md has
	one line per check on where each tolerance comes from.
*/

#include "Controls.h"
#include "Filament.h"
#include "Lamp.h"
#include "Model.h"
#include "PlanckTable.h"
#include "Shaders.h"

#include <OpenGL/OpenGL.h>
#include <OpenGL/gl3.h>
#include <zlib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <complex>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{
namespace model    = filament::model;
namespace controls = filament::controls;
namespace lamp     = filament::lamp;
namespace planck   = filament::planck;

int g_checks   = 0;
int g_failures = 0;

constexpr double kU = 5.9604644775390625e-8;//2^-24, half a float ulp at 1

/// One float ULP at x: the spacing of floats around x.
double ulpAt( double x )
{
	return std::ldexp( 1.0, std::ilogb( std::max( std::fabs( x ), 1e-30 ) ) - 23 );
}

/// Relative error of one evaluation of the shader's dT/dt, as a fraction of
/// it: ~20 float operations at half an ULP each (1.2e-6), the Shomate sum's
/// cancellation above 1900 K (terms of ~350 summing to ~37: x10 on 5 terms,
/// 3e-6), and the float-rounded constants (6e-8 each). About 5e-6; this is
/// twice that.
constexpr double kDerivativeRel = 1e-5;

//---------------------------------------------------------------------------
// A PNG writer. zlib ships with the OS.
//---------------------------------------------------------------------------
void putU32( std::vector< unsigned char >& out, uint32_t value )
{
	out.push_back( static_cast< unsigned char >( value >> 24 ) );
	out.push_back( static_cast< unsigned char >( value >> 16 ) );
	out.push_back( static_cast< unsigned char >( value >> 8 ) );
	out.push_back( static_cast< unsigned char >( value ) );
}

void putChunk( std::vector< unsigned char >& out, const char* type, const std::vector< unsigned char >& data )
{
	putU32( out, static_cast< uint32_t >( data.size() ) );
	const size_t start = out.size();
	out.insert( out.end(), type, type + 4 );
	out.insert( out.end(), data.begin(), data.end() );
	uLong crc = crc32( 0L, Z_NULL, 0 );
	crc       = crc32( crc, out.data() + start, static_cast< uInt >( 4 + data.size() ) );
	putU32( out, static_cast< uint32_t >( crc ) );
}

bool writePng( const std::string& path, int width, int height, const std::vector< unsigned char >& rgba )
{
	std::vector< unsigned char > raw;
	raw.reserve( static_cast< size_t >( height ) * ( 1 + static_cast< size_t >( width ) * 4 ) );
	for( int y = 0; y < height; ++y )
	{
		raw.push_back( 0 );
		const unsigned char* row = rgba.data() + static_cast< size_t >( y ) * width * 4;
		raw.insert( raw.end(), row, row + static_cast< size_t >( width ) * 4 );
	}
	uLongf compressedSize = compressBound( static_cast< uLong >( raw.size() ) );
	std::vector< unsigned char > compressed( compressedSize );
	if( compress2( compressed.data(), &compressedSize, raw.data(), static_cast< uLong >( raw.size() ), 6 ) != Z_OK )
		return false;
	compressed.resize( compressedSize );

	std::vector< unsigned char > png = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
	std::vector< unsigned char > ihdr;
	putU32( ihdr, static_cast< uint32_t >( width ) );
	putU32( ihdr, static_cast< uint32_t >( height ) );
	ihdr.push_back( 8 );
	ihdr.push_back( 6 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	ihdr.push_back( 0 );
	putChunk( png, "IHDR", ihdr );
	putChunk( png, "IDAT", compressed );
	putChunk( png, "IEND", {} );

	FILE* file = fopen( path.c_str(), "wb" );
	if( file == nullptr )
		return false;
	const size_t written = fwrite( png.data(), 1, png.size(), file );
	fclose( file );
	return written == png.size();
}

//---------------------------------------------------------------------------
// Pictures, float RGBA, top-first.
//---------------------------------------------------------------------------
using Picture = std::vector< float >;

Picture flat( int W, int H, double r, double g, double b )
{
	Picture p( static_cast< size_t >( W ) * H * 4 );
	for( size_t i = 0; i < p.size(); i += 4 )
	{
		p[ i ]     = static_cast< float >( r );
		p[ i + 1 ] = static_cast< float >( g );
		p[ i + 2 ] = static_cast< float >( b );
		p[ i + 3 ] = 1.0f;
	}
	return p;
}
Picture flat( int W, int H, double level )
{
	return flat( W, H, level, level, level );
}

//---------------------------------------------------------------------------
// GL plumbing.
//---------------------------------------------------------------------------
CGLContextObj createContext()
{
	const CGLPixelFormatAttribute accelerated[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAAccelerated,
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	const CGLPixelFormatAttribute software[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};
	//FITEST_RENDERER=software asks for Apple's software renderer by id, on a
	//Mac that has a GPU. It is what a GPU-less CI runner falls back to, and it
	//is not bit-repeatable frame to frame (repousse's resize check failed CI
	//by one ulp), so a check that would fail only in CI can be run here first.
	const CGLPixelFormatAttribute generic[] = {
		kCGLPFAOpenGLProfile, static_cast< CGLPixelFormatAttribute >( kCGLOGLPVersion_GL4_Core ),
		kCGLPFARendererID, static_cast< CGLPixelFormatAttribute >( kCGLRendererGenericFloatID ),
		kCGLPFAColorSize, static_cast< CGLPixelFormatAttribute >( 24 ),
		kCGLPFAAlphaSize, static_cast< CGLPixelFormatAttribute >( 8 ),
		static_cast< CGLPixelFormatAttribute >( 0 )
	};

	CGLPixelFormatObj format = nullptr;
	GLint formatCount        = 0;
	const char* renderer     = std::getenv( "FITEST_RENDERER" );
	if( renderer != nullptr && std::strcmp( renderer, "software" ) == 0 )
	{
		if( CGLChoosePixelFormat( generic, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
		std::fprintf( stderr, "fitest: FITEST_RENDERER=software, Apple's software renderer\n" );
	}
	else if( CGLChoosePixelFormat( accelerated, &format, &formatCount ) != kCGLNoError || format == nullptr )
	{
		if( CGLChoosePixelFormat( software, &format, &formatCount ) != kCGLNoError || format == nullptr )
			return nullptr;
	}

	CGLContextObj context = nullptr;
	const CGLError error  = CGLCreateContext( format, nullptr, &context );
	CGLDestroyPixelFormat( format );
	if( error != kCGLNoError )
		return nullptr;

	CGLSetCurrentContext( context );
	return context;
}

GLuint makeTexture( int width, int height, GLint internalFormat, GLenum type, const void* pixels )
{
	GLuint texture = 0;
	glGenTextures( 1, &texture );
	glBindTexture( GL_TEXTURE_2D, texture );
	glTexImage2D( GL_TEXTURE_2D, 0, internalFormat, width, height, 0, GL_RGBA, type, pixels );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );
	return texture;
}

GLuint makeFramebuffer( GLuint texture )
{
	GLuint fbo = 0;
	glGenFramebuffers( 1, &fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, fbo );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0 );
	return fbo;
}

template< typename T >
std::vector< T > flipRows( const std::vector< T >& image, int width, int height )
{
	std::vector< T > flipped( image.size() );
	const size_t stride = static_cast< size_t >( width ) * 4;
	for( int y = 0; y < height; ++y )
		std::copy( image.begin() + static_cast< long >( ( height - 1 - y ) * stride ),
		           image.begin() + static_cast< long >( ( height - y ) * stride ),
		           flipped.begin() + static_cast< long >( y * stride ) );
	return flipped;
}

//---------------------------------------------------------------------------
// Parameters by display name.
//---------------------------------------------------------------------------
struct NamedParameter
{
	std::string name;
	unsigned int index;
	unsigned int type;
	float value;
	float low;
	float high;
};

const char* kindName( const NamedParameter& p )
{
	if( p.index >= Filament::PT_ABOUT_FIRST )
		return "about";
	switch( p.type )
	{
	case FF_TYPE_BOOLEAN: return "bool";
	case FF_TYPE_EVENT: return "event";
	case FF_TYPE_OPTION: return "option";
	case FF_TYPE_INTEGER: return "integer";
	case FF_TYPE_BUFFER: return "buffer";
	case FF_TYPE_TEXT: return "text";
	case FF_TYPE_STANDARD: return "standard";
	default: return "other";
	}
}

std::vector< NamedParameter > listParameters( Filament& plugin )
{
	std::vector< NamedParameter > list;
	for( unsigned int i = 0; i < Filament::PT_COUNT; ++i )
	{
		const char* const name = plugin.GetParamName( i );
		NamedParameter p;
		p.name  = name ? name : "?";
		p.index = i;
		p.type  = plugin.GetParamType( i );
		p.value = plugin.GetFloatParameter( i );
		p.low   = 0.0f;
		p.high  = 1.0f;
		//An option's range reads back 0..1 whatever its element count, so
		//the element count is the range; an integer's range is real.
		if( p.type == FF_TYPE_OPTION )
			p.high = static_cast< float >( std::max( 1u, plugin.GetNumParamElements( i ) ) - 1u );
		else if( p.type == FF_TYPE_INTEGER )
		{
			const RangeStruct range = plugin.GetParamRange( i );
			p.low                   = range.min;
			p.high                  = range.max;
		}
		list.push_back( p );
	}
	return list;
}

int indexOfParameter( Filament& plugin, const std::string& name )
{
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.name == name )
			return static_cast< int >( p.index );
	return -1;
}

bool applySetting( Filament& plugin, const std::string& assignment, std::string& error )
{
	const size_t equals = assignment.rfind( '=' );
	if( equals == std::string::npos )
	{
		error = "expected Name=Value";
		return false;
	}
	const std::string name = assignment.substr( 0, equals );
	const int index        = indexOfParameter( plugin, name );
	if( index < 0 )
	{
		error = "no parameter called '" + name + "'";
		return false;
	}
	plugin.SetFloatParameter( static_cast< unsigned int >( index ), std::strtof( assignment.substr( equals + 1 ).c_str(), nullptr ) );
	return true;
}

bool set( Filament& plugin, const char* name, float value )
{
	std::string error;
	char buffer[ 64 ];
	std::snprintf( buffer, sizeof( buffer ), "%.9g", value );
	if( applySetting( plugin, std::string( name ) + "=" + buffer, error ) )
		return true;
	std::fprintf( stderr, "%s\n", error.c_str() );
	return false;
}

/// Every control a check can move. The defaults here are a small plain
/// wall: 8 x 4 clear bulbs, no gels, no glow or bloom, 25 C. Each check
/// moves what it measures.
struct Knobs
{
	int columns   = 8;
	int rows      = 4;
	double watts  = 60.0;
	int law       = lamp::kLinear;
	int mains     = 0;//50 Hz
	double ambient = 298.15;
	int gels      = 0;
	int glass     = 0;
};

void apply( Filament& p, const Knobs& k )
{
	set( p, "Columns", static_cast< float >( k.columns ) );
	set( p, "Rows", static_cast< float >( k.rows ) );
	set( p, "Gap", 0.3f );
	set( p, "Pixel Mode", 0.0f );
	set( p, "Glass", static_cast< float >( k.glass ) );
	set( p, "Gels", static_cast< float >( k.gels ) );
	set( p, "Wattage", controls::WattsParam( k.watts ) );
	set( p, "Dimmer", static_cast< float >( k.law ) );
	set( p, "Mains", static_cast< float >( k.mains ) );
	set( p, "Ambient Temp", controls::AmbientParam( k.ambient ) );
	set( p, "Glow", 0.0f );
	set( p, "Bloom", 0.0f );
	set( p, "Exposure", 0.5f );
	set( p, "Mix", 1.0f );
}

/// The lamp and ambient the plugin will actually run, from the SAME float
/// round-trip the plugin's sliders take -- a 60 W lamp is whatever
/// Watts( WattsParam( 60 ) ) says, to the last bit.
lamp::Lamp lampOf( const Knobs& k )
{
	return lamp::Make( controls::Watts( controls::WattsParam( k.watts ) ), controls::MainsHertz( k.mains ) );
}
double ambientOf( const Knobs& k )
{
	return controls::AmbientKelvin( controls::AmbientParam( k.ambient ) );
}

//---------------------------------------------------------------------------
// A session: the plugin, its input and output, and the clock that drives it.
//---------------------------------------------------------------------------
struct Session
{
	Filament plugin;
	int width  = 0;
	int height = 0;
	double fps = 60.0;
	/// Render into an RGBA32F framebuffer rather than RGBA8. The probes are
	/// read as float so their tolerances can be float-derived.
	bool floatOutput = true;

	GLuint sourceTexture = 0;
	GLuint outputTexture = 0;
	GLuint outputFBO     = 0;
	FFGLTextureStruct inputStruct  = {};
	FFGLTextureStruct* inputs[ 1 ] = { nullptr };
	ProcessOpenGLStruct process    = {};

	void makeTargets()
	{
		sourceTexture = makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr );
		outputTexture = floatOutput ? makeTexture( width, height, GL_RGBA32F, GL_FLOAT, nullptr )
		                            : makeTexture( width, height, GL_RGBA8, GL_UNSIGNED_BYTE, nullptr );
		outputFBO     = makeFramebuffer( outputTexture );

		inputStruct.Width = inputStruct.HardwareWidth = static_cast< FFUInt32 >( width );
		inputStruct.Height = inputStruct.HardwareHeight = static_cast< FFUInt32 >( height );
		inputStruct.Handle                              = sourceTexture;
		inputs[ 0 ]                                     = &inputStruct;

		process.numInputTextures = 1;
		process.inputTextures    = inputs;
		process.HostFBO          = outputFBO;
	}

	void dropTargets()
	{
		if( outputFBO )
			glDeleteFramebuffers( 1, &outputFBO );
		if( outputTexture )
			glDeleteTextures( 1, &outputTexture );
		if( sourceTexture )
			glDeleteTextures( 1, &sourceTexture );
		outputFBO = outputTexture = sourceTexture = 0;
	}

	bool begin( int w, int h )
	{
		width  = w;
		height = h;
		FFGLViewportStruct viewport = {};
		viewport.width              = static_cast< FFUInt32 >( width );
		viewport.height             = static_cast< FFUInt32 >( height );
		if( plugin.InitGL( &viewport ) != FF_SUCCESS )
		{
			std::fprintf( stderr, "InitGL failed -- see the diagnostics log for which shader\n" );
			return false;
		}
		makeTargets();
		return true;
	}

	/// What a host does when the clip or the composition changes size: hand
	/// the SAME instance a differently sized input. No DeInitGL.
	void resize( int w, int h )
	{
		dropTargets();
		width  = w;
		height = h;
		makeTargets();
	}

	void upload( const Picture& pixels )
	{
		const std::vector< float > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_FLOAT, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	void upload( const std::vector< unsigned char >& pixels )
	{
		const std::vector< unsigned char > flipped = flipRows( pixels, width, height );
		glBindTexture( GL_TEXTURE_2D, sourceTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, flipped.data() );
		glBindTexture( GL_TEXTURE_2D, 0 );
	}

	/// A synthetic clock, and it has to be synthetic: frame n is clocked at
	/// n / fps, the unit declared, not inferred.
	bool renderAt( long frame )
	{
		plugin.SetClockScaleForTest( 1.0 );
		plugin.SetTime( static_cast< double >( frame ) / fps );

		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glViewport( 0, 0, width, height );
		glClearColor( 0.0f, 0.0f, 0.0f, 0.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		const bool ok = plugin.ProcessOpenGL( &process ) == FF_SUCCESS;
		if( !ok )
			std::fprintf( stderr, "ProcessOpenGL failed on frame %ld\n", frame );
		return ok;
	}

	bool render( long frame, const std::vector< unsigned char >& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	bool render( long frame, const Picture& pixels )
	{
		upload( pixels );
		return renderAt( frame );
	}

	/// One pixel as floats, GL coordinates (y up).
	void readPixel( int x, int y, float out[ 4 ] )
	{
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( x, y, 1, 1, GL_RGBA, GL_FLOAT, out );
	}

	/// The centre pixel of a bulb, GL coordinates (row 0 at the bottom).
	void cellPixel( int column, int row, int columns, int rows, int& x, int& y ) const
	{
		x = static_cast< int >( std::floor( ( column + 0.5 ) * width / columns ) );
		y = static_cast< int >( std::floor( ( row + 0.5 ) * height / rows ) );
	}

	std::vector< unsigned char > readBack()
	{
		std::vector< unsigned char > pixels( static_cast< size_t >( width ) * height * 4 );
		glBindFramebuffer( GL_FRAMEBUFFER, outputFBO );
		glPixelStorei( GL_PACK_ALIGNMENT, 1 );
		glReadPixels( 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data() );
		return flipRows( pixels, width, height );
	}

	void end()
	{
		plugin.DeInitGL();
		dropTargets();
	}
};

const char* verdict( bool ok )
{
	return ok ? "ok" : "FAIL";
}

int report( bool ok, bool quiet, const char* format, ... ) __attribute__( ( format( printf, 3, 4 ) ) );
int report( bool ok, bool quiet, const char* format, ... )
{
	++g_checks;
	if( !ok )
		++g_failures;
	//Quiet is a negative control's run: its failures are the point, and the
	//summary line says so. --perturb runs the same thing verbosely.
	if( quiet )
		return ok ? 0 : 1;
	va_list args;
	va_start( args, format );
	std::printf( "   %-4s ", verdict( ok ) );
	std::vprintf( format, args );
	std::printf( "\n" );
	va_end( args );
	return ok ? 0 : 1;
}

int tooSmall( const char* what )
{
	std::printf( "   FAIL the raster is too small for %s\n", what );
	++g_checks;
	++g_failures;
	return 1;
}

//---------------------------------------------------------------------------
// The reference: Lamp.h's ODE in double, classical RK4, the drive waveform
// at each stage's time. `step` integrates one frame of the plugin's
// schedule, `fine` the same frame sixteen times finer.
//---------------------------------------------------------------------------
struct Reference
{
	lamp::Lamp lamp;
	int law        = lamp::kLinear;
	double ambient = 298.15;
	int perturb    = 0;

	double T = 0.0;

	/// Integrate [start, start + seconds] in n RK4 steps at `level`, from the
	/// mains phase at `start` (cycles, fractional), as the shader does.
	/// Returns the new T; `floatBound` accumulates what float arithmetic may
	/// add per step (see advance()).
	double advance( double T0, double level, double phase0, double seconds, int n, double* floatBound = nullptr, double* stepMax = nullptr ) const
	{
		double t        = T0;
		const double h  = seconds / n;
		const double hc = h * lamp.hertz;
		for( int k = 0; k < n; ++k )
		{
			const double c0 = phase0 + k * hc;
			const double v1 = lamp::DriveSquared( c0, law, level, perturb );
			const double vm = lamp::DriveSquared( c0 + 0.5 * hc, law, level, perturb );
			const double v4 = lamp::DriveSquared( c0 + hc, law, level, perturb );
			const double k1 = lamp::Derivative( lamp, t, v1, ambient, perturb );
			const double k2 = lamp::Derivative( lamp, t + 0.5 * h * k1, vm, ambient, perturb );
			const double k3 = lamp::Derivative( lamp, t + 0.5 * h * k2, vm, ambient, perturb );
			const double k4 = lamp::Derivative( lamp, t + h * k3, v4, ambient, perturb );
			const double dT = h / 6.0 * ( k1 + 2.0 * k2 + 2.0 * k3 + k4 );
			t               = std::clamp( t + dT, 100.0, 3695.0 );
			//Per step, the GPU may differ by the final add's rounding (one ULP
			//of T, generously: it is half) plus kDerivativeRel of the step.
			if( floatBound )
				*floatBound += ulpAt( t ) + kDerivativeRel * std::fabs( dT );
			if( stepMax )
				*stepMax = std::max( *stepMax, std::fabs( dT ) );
		}
		return t;
	}
};

/// A trajectory check's bookkeeping: the GPU against the plugin's schedule
/// in double (which bounds the float error) and against a sixteen-times
/// finer one (the ODE, to the discretisation error).
struct Tracker
{
	Reference ref;
	double scheme    = 0.0;
	double fine      = 0.0;
	double floatSum  = 0.0;
	double worstFloat = 0.0;   ///< |GPU - scheme| - floatSum, worst (<= 0 is inside)
	double worstGpuScheme = 0.0;
	double worstGpuFine   = 0.0;
	double worstSchemeFine = 0.0;
	int outside = 0;

	void start( double T0 )
	{
		scheme = fine = T0;
	}

	/// After a frame: the plugin's schedule, its drive, and its T.
	void frame( Filament& plugin, double level, double gpu )
	{
		double seconds = 0.0, phase0 = 0.0;
		int n = 0;
		plugin.LastFrameForTest( seconds, n, phase0 );
		if( n > 0 )
		{
			scheme = ref.advance( scheme, level, phase0, seconds, n, &floatSum );
			fine   = ref.advance( fine, level, phase0, seconds, 16 * n );
		}
		const double dg = std::fabs( gpu - scheme );
		worstGpuScheme  = std::max( worstGpuScheme, dg );
		worstGpuFine    = std::max( worstGpuFine, std::fabs( gpu - fine ) );
		worstSchemeFine = std::max( worstSchemeFine, std::fabs( scheme - fine ) );
		if( dg > floatSum )
			++outside;
	}
};

/// One frame of a flat drive through the plugin with the State probe:
/// returns T and the drive level the plugin actually used.
bool stateFrame( Session& s, long frame, int x, int y, double& T, double& level )
{
	if( !s.renderAt( frame ) )
		return false;
	float px[ 4 ];
	s.readPixel( x, y, px );
	T     = px[ 0 ];
	level = px[ 3 ];
	return true;
}

//---------------------------------------------------------------------------
// --steady
//---------------------------------------------------------------------------
int runSteady( int W, int H, int perturb, bool quiet = false )
{
	//A 200 W, 230 V lamp on a sine-wave (Linear) dimmer: small enough a
	//ripple that the rectification bias is millikelvin, big enough to be a
	//real lamp. 1.5 s to settle (its slowest time constant is ~60 ms), then
	//the mean over 0.5 s: 25 mains cycles, 600 frames at 1200 fps.
	const double fps = 1200.0;
	const int settle = 1800, window = 600;
	if( !quiet )
		std::printf( "steady: a 200 W 230 V lamp, Linear dimmer, flat levels, mean T over 25 mains cycles, %dx%d\n", W, H );
	int failures = 0;
	const double levels[ 3 ] = { 1.0, 0.7, 0.4 };
	for( double levelIn : levels )
	{
		Session s;
		s.fps = fps;
		Knobs k;
		k.watts = 200.0;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeState );
		if( !s.begin( W, H ) )
			return 1;
		s.upload( flat( W, H, levelIn ) );
		int x, y;
		s.cellPixel( 3, 1, k.columns, k.rows, x, y );
		double sum = 0.0, lo = 1e9, hi = 0.0, level = 0.0, T = 0.0;
		for( long f = 0; f < settle + window; ++f )
		{
			if( !stateFrame( s, f, x, y, T, level ) )
				return 1;
			if( f >= settle )
			{
				sum += T;
				lo = std::min( lo, T );
				hi = std::max( hi, T );
			}
		}
		double seconds = 0.0, phase0 = 0.0;
		int n = 0;
		s.plugin.LastFrameForTest( seconds, n, phase0 );
		s.end();

		const lamp::Lamp L    = lampOf( k );
		const double ambient  = ambientOf( k );
		const double mean     = sum / window;
		const double pp       = hi - lo;
		const double meanV2   = lamp::DriveSquared( 0.0, lamp::kLinear, level, lamp::kPerturbDC );
		const double predicted = lamp::SteadyKelvin( meanV2, ambient, 0 );
		//(1) The rectification bias of a ripple: the mean of a function of a
		//    rippling T is not the function of the mean. To second order the
		//    offset is |g''/2g'| <dT^2>, with |g''/g'| <= 3/T for a T^4 law and
		//    about the same again for 1/rho( T ): <= 3 pp^2 / ( 4 T ).
		//(2) Float, in a contracting system: each step adds at most one ULP of
		//    T plus kDerivativeRel of the step, and the error decays by h / tau
		//    a step, so it settles below ( ulp + rel step ) tau / h; doubled.
		const double e       = 0.5;
		const double slope   = std::fabs( ( lamp::Derivative( L, predicted + e, meanV2, ambient, 0 ) - lamp::Derivative( L, predicted - e, meanV2, ambient, 0 ) ) / ( 2.0 * e ) );
		const double h       = seconds / std::max( n, 1 );
		const double stepMax = pp * lamp::kPi * 2.0 * L.hertz * h;
		const double floatBound = 2.0 * ( ulpAt( predicted ) + kDerivativeRel * stepMax ) / ( slope * h );
		const double tolerance  = 3.0 * pp * pp / ( 4.0 * predicted ) + floatBound;
		failures += report( std::fabs( mean - predicted ) <= tolerance, quiet,
		                    "level %.2f (V_rms %.3f): mean %.3f K, balance predicts %.3f K, off %.4f (tolerance %.4f: ripple %.2f K pp, float %.4f)",
		                    level, std::sqrt( meanV2 ), mean, predicted, mean - predicted, tolerance, pp, floatBound );
		if( levelIn == 1.0 )
			failures += report( std::fabs( mean - lamp::kRatedKelvin ) <= tolerance + std::fabs( predicted - lamp::kRatedKelvin ), quiet,
			                    "  at rated voltage: %.3f K against the rated %.0f K (balance %.6f K, by construction)", mean, lamp::kRatedKelvin, predicted );
	}
	return failures;
}

//---------------------------------------------------------------------------
// The Planckian locus, integrated HERE from the committed CIE table, in
// double: independent of tools/planck_table.py, the header, the texture and
// the shader's interpolation, all of which --colour checks against it.
//---------------------------------------------------------------------------
struct Cmf
{
	std::vector< double > nm, x, y, z;
};

const Cmf& cmf()
{
	static Cmf table;
	if( table.nm.empty() )
	{
		std::ifstream file( std::string( FILAMENT_SOURCE_DIR ) + "/tools/data/ciexyz31_1.csv" );
		std::string line;
		while( std::getline( file, line ) )
		{
			double n, a, b, c;
			if( std::sscanf( line.c_str(), "%lf,%lf,%lf,%lf", &n, &a, &b, &c ) == 4 )
			{
				table.nm.push_back( n );
				table.x.push_back( a );
				table.y.push_back( b );
				table.z.push_back( c );
			}
		}
	}
	return table;
}

void locusXYZ( double kelvin, double& X, double& Y, double& Z )
{
	const Cmf& t = cmf();
	const double h = 6.62607015e-34, c = 2.99792458e8, k = 1.380649e-23;
	X = Y = Z = 0.0;
	for( size_t i = 0; i < t.nm.size(); ++i )
	{
		const double lam = t.nm[ i ] * 1e-9;
		const double b   = ( 2.0 * h * c * c / std::pow( lam, 5.0 ) ) / std::expm1( h * c / ( lam * k * kelvin ) );
		X += b * t.x[ i ];
		Y += b * t.y[ i ];
		Z += b * t.z[ i ];
	}
}

void locusXY( double kelvin, double& x, double& y )
{
	double X, Y, Z;
	locusXYZ( kelvin, X, Y, Z );
	x = X / ( X + Y + Z );
	y = Y / ( X + Y + Z );
}

/// Linear sRGB back to XYZ: IEC 61966-2-1's matrix, inverted here.
void rgbToXYZ( const double rgb[ 3 ], double& X, double& Y, double& Z )
{
	const double M[ 3 ][ 3 ] = { { 3.2406, -1.5372, -0.4986 }, { -0.9689, 1.8758, 0.0415 }, { 0.0557, -0.2040, 1.0570 } };
	const double det = M[ 0 ][ 0 ] * ( M[ 1 ][ 1 ] * M[ 2 ][ 2 ] - M[ 1 ][ 2 ] * M[ 2 ][ 1 ] ) - M[ 0 ][ 1 ] * ( M[ 1 ][ 0 ] * M[ 2 ][ 2 ] - M[ 1 ][ 2 ] * M[ 2 ][ 0 ] )
	                   + M[ 0 ][ 2 ] * ( M[ 1 ][ 0 ] * M[ 2 ][ 1 ] - M[ 1 ][ 1 ] * M[ 2 ][ 0 ] );
	double inv[ 3 ][ 3 ];
	for( int r = 0; r < 3; ++r )
		for( int c = 0; c < 3; ++c )
		{
			const int r1 = ( c + 1 ) % 3, r2 = ( c + 2 ) % 3, c1 = ( r + 1 ) % 3, c2 = ( r + 2 ) % 3;
			inv[ r ][ c ] = ( M[ r1 ][ c1 ] * M[ r2 ][ c2 ] - M[ r1 ][ c2 ] * M[ r2 ][ c1 ] ) / det;
		}
	X = inv[ 0 ][ 0 ] * rgb[ 0 ] + inv[ 0 ][ 1 ] * rgb[ 1 ] + inv[ 0 ][ 2 ] * rgb[ 2 ];
	Y = inv[ 1 ][ 0 ] * rgb[ 0 ] + inv[ 1 ][ 1 ] * rgb[ 1 ] + inv[ 1 ][ 2 ] * rgb[ 2 ];
	Z = inv[ 2 ][ 0 ] * rgb[ 0 ] + inv[ 2 ][ 1 ] * rgb[ 1 ] + inv[ 2 ][ 2 ] * rgb[ 2 ];
}

/// McCamy (1992), CCT from xy: a published cubic, independent of all of the above.
double mcCamy( double x, double y )
{
	const double n = ( x - 0.3320 ) / ( 0.1858 - y );
	return 449.0 * n * n * n + 3525.0 * n * n + 6823.3 * n + 5520.33;
}

//---------------------------------------------------------------------------
// --colour
//---------------------------------------------------------------------------
int runColour( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "colour: a 60 W lamp at five steady levels; the light's xy against this harness's Planckian locus, %dx%d\n", W, H );
	int failures = 0;
	const double levels[ 5 ] = { 0.45, 0.55, 0.7, 0.85, 1.0 };
	std::vector< double > temps, xs, ccts;
	double Yref;
	{
		double X, Z;
		locusXYZ( 2856.0, X, Yref, Z );
	}
	for( double levelIn : levels )
	{
		Session s;
		s.fps = 240.0;
		Knobs k;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeState );
		if( !s.begin( W, H ) )
			return 1;
		s.upload( flat( W, H, levelIn ) );
		int px, py;
		s.cellPixel( 5, 2, k.columns, k.rows, px, py );
		const long frames = 240;
		double T = 0.0, level = 0.0;
		for( long f = 0; f < frames; ++f )
			if( !stateFrame( s, f, px, py, T, level ) )
				return 1;
		//The same instant again, with the Instant probe: dt = 0, no substeps,
		//the temperatures untouched.
		s.plugin.SetProbeForTest( model::kProbeInstant );
		if( !s.renderAt( frames - 1 ) )
			return 1;
		float out[ 4 ];
		s.readPixel( px, py, out );
		s.end();

		const double rgb[ 3 ] = { out[ 0 ], out[ 1 ], out[ 2 ] };
		double X, Y, Z;
		rgbToXYZ( rgb, X, Y, Z );
		const double x = X / ( X + Y + Z ), y = Y / ( X + Y + Z );
		double lx, ly;
		locusXY( T, lx, ly );

		//Tolerance: linear interpolation between table rows 10 K apart errs
		//by at most h^2 / 8 |f''|, |f''| from this locus's own second
		//difference over the bracket (doubled for the curvature changing
		//across it); plus the float read: the rgb's ~1e-7 relative through
		//the matrix, 1e-6.
		const double hK = planck::kStepKelvin;
		double xm, ym, xp, yp;
		locusXY( T - hK, xm, ym );
		locusXY( T + hK, xp, yp );
		const double interpX = 2.0 * std::fabs( xm - 2.0 * lx + xp ) / 8.0;
		const double interpY = 2.0 * std::fabs( ym - 2.0 * ly + yp ) / 8.0;
		const double tolX = interpX + 1e-6, tolY = interpY + 1e-6;
		failures += report( std::fabs( x - lx ) <= tolX && std::fabs( y - ly ) <= tolY, quiet,
		                    "T %.1f K: xy (%.6f, %.6f), locus (%.6f, %.6f), off (%.1e, %.1e) within (%.1e, %.1e)", T, x, y, lx, ly, x - lx, y - ly, tolX, tolY );

		//Luminance, relative to 2856 K: log2 interpolated linearly, so
		//h^2 / 8 |(log2 Y)''| relative, same doubling; plus 1e-6.
		double Xm, Ym, Zm, Xp, Yp, Zp, Xl, Yl, Zl;
		locusXYZ( T - hK, Xm, Ym, Zm );
		locusXYZ( T, Xl, Yl, Zl );
		locusXYZ( T + hK, Xp, Yp, Zp );
		const double curvature = std::fabs( std::log2( Ym ) - 2.0 * std::log2( Yl ) + std::log2( Yp ) );
		const double tolY_rel  = 2.0 * curvature / 8.0 * std::log( 2.0 ) + 1e-6;
		const double wantY     = Yl / Yref;
		failures += report( std::fabs( Y / wantY - 1.0 ) <= tolY_rel, quiet, "           luminance %.6f of a 2856 K filament, locus %.6f, relative %.1e within %.1e", Y, wantY, Y / wantY - 1.0, tolY_rel );

		//McCamy's cubic, a published approximation with its own error: the
		//rendered colour's CCT must agree with the CCT McCamy gives the TRUE
		//locus at T, to the chromaticity tolerance through McCamy's
		//gradient; and McCamy's own error at T is printed.
		const double cct      = mcCamy( x, y );
		const double cctLocus = mcCamy( lx, ly );
		const double gx       = ( mcCamy( lx + 1e-6, ly ) - mcCamy( lx - 1e-6, ly ) ) / 2e-6;
		const double gy       = ( mcCamy( lx, ly + 1e-6 ) - mcCamy( lx, ly - 1e-6 ) ) / 2e-6;
		const double tolCct   = std::fabs( gx ) * tolX + std::fabs( gy ) * tolY;
		failures += report( std::fabs( cct - cctLocus ) <= tolCct, quiet, "           McCamy CCT %.1f K (McCamy on the true locus %.1f K; its own error at T is %+.1f K), within %.2f K",
		                    cct, cctLocus, cctLocus - T, tolCct );
		temps.push_back( T );
		xs.push_back( x );
		ccts.push_back( cct );
	}
	bool monotone = true;
	for( size_t i = 1; i < temps.size(); ++i )
		monotone = monotone && temps[ i ] > temps[ i - 1 ] && xs[ i ] < xs[ i - 1 ] && ccts[ i ] > ccts[ i - 1 ];
	failures += report( monotone, quiet, "dimming is redward: T %.0f > %.0f > %.0f > %.0f > %.0f K, x rises and CCT falls with every step down",
	                    temps[ 4 ], temps[ 3 ], temps[ 2 ], temps[ 1 ], temps[ 0 ] );
	return failures;
}

//---------------------------------------------------------------------------
// --rise and --fall share a run: from cold to rated, then off.
//---------------------------------------------------------------------------
struct Run
{
	std::vector< double > t, gpu;
	Tracker tracker;
	bool ok = true;
};

/// 60 W 230 V, Linear, at `fps`: `offFrames` at level 0, `onFrames` at
/// level 1, then `darkFrames` at 0 again, probing one bulb every frame.
Run runSwitch( int W, int H, int perturb, double fps, long offFrames, long onFrames, long darkFrames, int referencePerturb )
{
	Run run;
	Session s;
	s.fps = fps;
	Knobs k;
	apply( s.plugin, k );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeState );
	if( !s.begin( W, H ) )
	{
		run.ok = false;
		return run;
	}
	run.tracker.ref.lamp    = lampOf( k );
	run.tracker.ref.law     = lamp::kLinear;
	run.tracker.ref.ambient = ambientOf( k );
	run.tracker.ref.perturb = referencePerturb;
	run.tracker.start( ambientOf( k ) );

	int px, py;
	s.cellPixel( 2, 1, k.columns, k.rows, px, py );
	const Picture black = flat( W, H, 0.0 ), white = flat( W, H, 1.0 );
	int uploaded = -1;
	for( long f = 0; f < offFrames + onFrames + darkFrames; ++f )
	{
		const int want = f >= offFrames && f < offFrames + onFrames ? 1 : 0;
		if( want != uploaded )
		{
			s.upload( want ? white : black );
			uploaded = want;
		}
		double T = 0.0, level = 0.0;
		if( !stateFrame( s, f, px, py, T, level ) )
		{
			run.ok = false;
			break;
		}
		run.tracker.frame( s.plugin, level, T );
		run.t.push_back( f / fps );
		run.gpu.push_back( T );
	}
	s.end();
	return run;
}

/// The time T first crosses `level` upward (or downward), by linear
/// interpolation between samples; -1 if never.
double crossing( const std::vector< double >& t, const std::vector< double >& T, double level, bool upward, size_t from = 0 )
{
	for( size_t i = std::max< size_t >( from, 1 ); i < T.size(); ++i )
		if( upward ? ( T[ i - 1 ] < level && T[ i ] >= level ) : ( T[ i - 1 ] > level && T[ i ] <= level ) )
			return t[ i - 1 ] + ( t[ i ] - t[ i - 1 ] ) * ( level - T[ i - 1 ] ) / ( T[ i ] - T[ i - 1 ] );
	return -1.0;
}

/// The same run's T( t ) computed by the fine reference alone (no GPU),
/// for another model: the constant-R lamp, the linear cooler.
std::vector< double > referenceOnly( const Knobs& k, int referencePerturb, double fps, long offFrames, long onFrames, long darkFrames, const std::vector< int >& substeps )
{
	Reference ref;
	ref.lamp    = lampOf( k );
	ref.ambient = ambientOf( k );
	ref.perturb = referencePerturb;
	std::vector< double > out;
	double T = ambientOf( k );
	for( long f = 0; f < offFrames + onFrames + darkFrames; ++f )
	{
		const double level  = f >= offFrames && f < offFrames + onFrames ? 1.0 : 0.0;
		const double dt     = f == 0 ? model::kNominalFrame : 1.0 / fps;
		const double start  = f / fps - dt;
		const double phase0 = start * ref.lamp.hertz - std::floor( start * ref.lamp.hertz );
		T = ref.advance( T, level, phase0, dt, 16 * substeps[ static_cast< size_t >( f ) ] );
		out.push_back( T );
	}
	return out;
}

int runRise( int W, int H, int perturb, bool quiet = false )
{
	const double fps = 2400.0;
	const long off = 24, on = 960;
	if( !quiet )
		std::printf( "rise: a 60 W 230 V lamp switched on from cold at t = 10 ms, 2400 fps, one bulb probed every frame, %dx%d\n", W, H );
	Run run = runSwitch( W, H, perturb, fps, off, on, 0, 0 );
	if( !run.ok )
		return 1;
	const Tracker& tr = run.tracker;
	int failures      = 0;

	//(1) The GPU runs the stated scheme to float: inside the accumulated
	//    per-step bound on every frame.
	failures += report( tr.outside == 0, quiet, "the GPU runs the stated RK4 schedule: worst |GPU - double| %.4f K, inside the accumulated float bound (%.3f K at the end) on every frame",
	                    tr.worstGpuScheme, tr.floatSum );
	//(2) ... and follows the ODE to the scheme's own discretisation error.
	const double tolT = tr.worstSchemeFine + tr.floatSum;
	failures += report( tr.worstGpuFine <= tolT, quiet, "it follows the R( T ) ODE: worst |GPU - fine| %.4f K (tolerance %.4f: discretisation %.4f measured + float)", tr.worstGpuFine, tolT, tr.worstSchemeFine );

	//(3) The rise time, measured out of the frames: t90 of the temperature
	//    rise, against the fine R( T ) solution and the constant-R lamp.
	std::vector< int > substeps;//the plugin's schedule, for the other model
	{
		const lamp::Lamp L = lampOf( Knobs() );
		for( long f = 0; f < off + on; ++f )
			substeps.push_back( lamp::Substeps( L, ambientOf( Knobs() ), f == 0 ? model::kNominalFrame : 1.0 / fps ) );
	}
	const double T0     = ambientOf( Knobs() );
	const double target = T0 + 0.9 * ( lamp::kRatedKelvin - T0 );
	const double t0     = off / fps - 1.0 / fps;//the drive applies from the frame before the first lit one
	std::vector< double > fineRT  = referenceOnly( Knobs(), 0, fps, off, on, 0, substeps );
	std::vector< double > fineCR  = referenceOnly( Knobs(), lamp::kPerturbConstantR, fps, off, on, 0, substeps );
	const double tGpu = crossing( run.t, run.gpu, target, true ) - t0;
	const double tRT  = crossing( run.t, fineRT, target, true ) - t0;
	const double tCR  = crossing( run.t, fineCR, target, true ) - t0;
	//One frame of interpolation error either way, and the trajectory
	//tolerance over the slope at the crossing.
	size_t i90 = 0;
	while( i90 + 1 < fineRT.size() && fineRT[ i90 ] < target )
		++i90;
	const double slope = ( fineRT[ std::min( i90 + 1, fineRT.size() - 1 ) ] - fineRT[ i90 > 0 ? i90 - 1 : 0 ] ) * fps / 2.0;
	const double tolT90 = 1.0 / fps + tolT / std::max( slope, 1.0 );
	failures += report( tGpu > 0.0 && std::fabs( tGpu - tRT ) <= tolT90, quiet, "t90 (to %.0f K) %.2f ms, the R( T ) ODE says %.2f ms (tolerance %.2f ms)", target, tGpu * 1e3, tRT * 1e3, tolT90 * 1e3 );
	failures += report( tCR - tRT > 4.0 * tolT90, quiet, "a constant-R lamp (R held at hot) would take %.2f ms: the inrush (cold R = hot / %.1f) makes it %.1fx faster",
	                    tCR * 1e3, lamp::Resistivity( lamp::kRatedKelvin ) / lamp::Resistivity( T0 ), tCR / tRT );
	return failures;
}

int runFall( int W, int H, int perturb, bool quiet = false )
{
	const double fps = 2400.0;
	const long off = 12, on = 960, dark = 1440;
	if( !quiet )
		std::printf( "fall: a 60 W 230 V lamp at rated for 0.4 s, then off for 0.6 s, 2400 fps, %dx%d\n", W, H );
	Run run = runSwitch( W, H, perturb, fps, off, on, dark, 0 );
	if( !run.ok )
		return 1;
	const Tracker& tr = run.tracker;
	int failures      = 0;
	failures += report( tr.outside == 0, quiet, "the GPU runs the stated RK4 schedule: worst |GPU - double| %.4f K, inside the accumulated float bound (%.3f K at the end)", tr.worstGpuScheme, tr.floatSum );
	const double tolT = tr.worstSchemeFine + tr.floatSum;
	failures += report( tr.worstGpuFine <= tolT, quiet, "the decay follows the radiative + conductive ODE: worst |GPU - fine| %.4f K (tolerance %.4f)", tr.worstGpuFine, tolT );

	//Not an exponential. The instantaneous time constant tau( T ) =
	//( T - T0 ) / ( -dT/dt ), measured out of the GPU's frames by a central
	//difference, at 2400 K and at 1200 K. An exponential has one tau; T^4
	//radiation makes it grow as the wire cools. The threshold sits between
	//the two models' own predictions (their geometric mean), both computed.
	const double T0   = ambientOf( Knobs() );
	const size_t from = static_cast< size_t >( off + on );
	auto tauAt = [ & ]( const std::vector< double >& T, double level ) {
		for( size_t i = from + 1; i + 1 < T.size(); ++i )
			if( T[ i ] <= level )
			{
				const double rate = ( T[ i - 1 ] - T[ i + 1 ] ) * fps / 2.0;
				return ( T[ i ] - T0 ) / rate;
			}
		return -1.0;
	};
	std::vector< int > substeps;
	{
		const lamp::Lamp L = lampOf( Knobs() );
		for( long f = 0; f < off + on + dark; ++f )
			substeps.push_back( lamp::Substeps( L, T0, f == 0 ? model::kNominalFrame : 1.0 / fps ) );
	}
	std::vector< double > fineT4  = referenceOnly( Knobs(), 0, fps, off, on, dark, substeps );
	std::vector< double > fineLin = referenceOnly( Knobs(), lamp::kPerturbLinearCool, fps, off, on, dark, substeps );
	const double gpuRatio = tauAt( run.gpu, 1200.0 ) / tauAt( run.gpu, 2400.0 );
	const double t4Ratio  = tauAt( fineT4, 1200.0 ) / tauAt( fineT4, 2400.0 );
	const double linRatio = tauAt( fineLin, 1200.0 ) / tauAt( fineLin, 2400.0 );
	const double threshold = std::sqrt( t4Ratio * linRatio );
	failures += report( tauAt( run.gpu, 1200.0 ) > 0.0 && gpuRatio > threshold, quiet,
	                    "not an exponential: tau at 1200 K is %.2fx tau at 2400 K (the T^4 model says %.2fx, a linear cooler %.2fx; threshold %.2fx)",
	                    gpuRatio, t4Ratio, linRatio, threshold );
	const double t1500 = crossing( run.t, run.gpu, 1500.0, false, from ) - ( off + on - 1 ) / fps;
	if( !quiet )
		std::printf( "        (from rated to 1500 K in %.1f ms)\n", t1500 * 1e3 );
	return failures;
}

//---------------------------------------------------------------------------
// --ripple
//---------------------------------------------------------------------------
int runRipple( int W, int H, int perturb, bool quiet = false )
{
	//Triac at level 0.537: an odd firing angle, so no stage of the RK4
	//schedule lands on the gate exactly and float and double agree which
	//side it is on. 2400 fps; 3 s to settle (a 500 W lamp's slowest time
	//constant is ~0.3 s), then a window of exactly 0.5 s.
	const double fps  = 2400.0;
	const long settle = 7200, window = 1200;
	const double level = 0.537;
	if( !quiet )
		std::printf( "ripple: triac at %.3f, 25 W and 500 W, 50 and 60 Hz, T sampled at 2400 fps over 0.5 s, %dx%d\n", level, W, H );
	int failures = 0;
	for( int mains = 0; mains < 2; ++mains )
	{
		const double hz = controls::MainsHertz( mains );
		double pp[ 2 ];
		const double watts[ 2 ] = { 25.0, 500.0 };
		for( int w = 0; w < 2; ++w )
		{
			Session s;
			s.fps = fps;
			Knobs k;
			k.watts   = watts[ w ];
			k.law     = lamp::kTriac;
			k.mains   = mains;
			k.columns = 4;
			k.rows    = 2;
			apply( s.plugin, k );
			s.plugin.SetPerturbForTest( perturb );
			s.plugin.SetProbeForTest( model::kProbeState );
			if( !s.begin( W, H ) )
				return 1;
			s.upload( flat( W, H, level ) );
			int px, py;
			s.cellPixel( 1, 1, k.columns, k.rows, px, py );
			Tracker tr;
			tr.ref.lamp    = lampOf( k );
			tr.ref.law     = lamp::kTriac;
			tr.ref.ambient = ambientOf( k );
			tr.start( ambientOf( k ) );
			std::vector< double > T, schemeSeries;
			double lo = 1e9, hi = 0.0, flo = 1e9, fhi = 0.0;
			for( long f = 0; f < settle + window; ++f )
			{
				double t = 0.0, used = 0.0;
				if( !stateFrame( s, f, px, py, t, used ) )
					return 1;
				tr.frame( s.plugin, used, t );
				if( f >= settle )
				{
					T.push_back( t );
					schemeSeries.push_back( tr.scheme );
					lo  = std::min( lo, t );
					hi  = std::max( hi, t );
					flo = std::min( flo, tr.fine );
					fhi = std::max( fhi, tr.fine );
				}
			}
			s.end();
			pp[ w ] = hi - lo;

			//The spectrum of the window, mean removed. 0.5 s is a whole number
			//of mains cycles, so a periodic steady state has no leakage: every
			//bin that is not a multiple of 2 x mains is empty but for what is
			//left of the settling and what the GPU's own error adds.
			const int binHz = 2;//1 / 0.5 s
			struct Spectrum
			{
				int peakBin     = 0;
				double onGrid   = 0.0;
				double offGrid  = 0.0;
			};
			auto spectrum = [ & ]( const std::vector< double >& series ) {
				Spectrum sp;
				const size_t N = series.size();
				double mean    = 0.0;
				for( double v : series )
					mean += v;
				mean /= N;
				double peak = 0.0;
				for( size_t b = 1; b < N / 2; ++b )
				{
					std::complex< double > acc( 0.0, 0.0 );
					for( size_t i = 0; i < N; ++i )
						acc += ( series[ i ] - mean ) * std::polar( 1.0, -2.0 * lamp::kPi * static_cast< double >( b * i ) / N );
					const double power = std::norm( acc );
					if( power > peak )
					{
						peak       = power;
						sp.peakBin = static_cast< int >( b );
					}
					if( static_cast< int >( b ) * binHz % static_cast< int >( 2 * hz ) == 0 )
						sp.onGrid += power;
					else
						sp.offGrid += power;
				}
				return sp;
			};
			const Spectrum g = spectrum( T ), r = spectrum( schemeSeries );
			double windowError = 0.0;
			for( size_t i = 0; i < T.size(); ++i )
				windowError = std::max( windowError, std::fabs( T[ i ] - schemeSeries[ i ] ) );
			const double peakHz = g.peakBin * binHz;
			failures += report( std::fabs( peakHz - 2.0 * hz ) < 0.5 * binHz && pp[ w ] > 0.5 * ( fhi - flo ), quiet,
			                    "%2.0f Hz mains, %3.0f W: the ripple's strongest line is at %.0f Hz (2 x mains = %.0f), %.2f K peak to peak (the ODE: %.2f K)",
			                    hz, watts[ w ], peakHz, 2.0 * hz, pp[ w ], fhi - flo );
			//Off the harmonics: the double reference's own remainder on the
			//plugin's schedule (the settling left; the schedule itself repeats
			//every mains cycle, so its discretisation lands ON the harmonics)
			//x4, plus what the GPU's float error e can carry -- by Parseval at
			//most N sum e^2 against the ripple's ~N^2 ( pp / 2 )^2 / 2:
			//8 ( e / pp )^2, e the worst |GPU - double| in the window.
			const double offFraction = g.offGrid / ( g.onGrid + g.offGrid );
			const double bound       = 4.0 * r.offGrid / ( r.onGrid + r.offGrid ) + 8.0 * std::pow( windowError / std::max( pp[ w ], 1e-9 ), 2.0 );
			failures += report( offFraction <= bound, quiet, "               %.2e of the power is off the harmonics of %.0f Hz (bound %.1e)", offFraction, 2.0 * hz, bound );
			failures += report( tr.outside == 0, quiet, "               the GPU runs the stated schedule to float: worst %.4f K, bound %.3f K", tr.worstGpuScheme, tr.floatSum );
		}
		failures += report( pp[ 0 ] > pp[ 1 ], quiet, "%2.0f Hz: the 25 W filament ripples %.2f K, the 500 W one %.2f K: %.1fx more on the small lamp", hz, pp[ 0 ], pp[ 1 ], pp[ 0 ] / pp[ 1 ] );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --resize
//---------------------------------------------------------------------------
int runResize( int W, int H, int perturb, bool quiet = false )
{
	//A 16 x 9 wall at full, fading down to 0.3 at frame 15. Run A is left
	//alone; run B has its OUTPUT raster changed to 1.5x at frame 20 and back
	//at 28; run C has its GRID changed to 24 x 13 at frame 20. At frame 40
	//every bulb in B and C must hold A's temperature.
	const int frames = 41;
	if( !quiet )
		std::printf( "resize: a 16x9 wall fading from 1.0 to 0.3; the output resized mid-fade, and the grid changed mid-fade, %dx%d\n", W, H );
	auto run = [ & ]( int variant, std::vector< double >& temps, double& floatBound ) -> bool {
		Session s;
		Knobs k;
		k.columns = 16;
		k.rows    = 9;
		k.law     = lamp::kSquareLaw;
		apply( s.plugin, k );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeState );
		if( !s.begin( W, H ) )
			return false;
		int columns = 16, rows = 9;
		floatBound  = 0.0;
		for( long f = 0; f < frames; ++f )
		{
			if( variant == 1 && f == 20 )
				s.resize( W * 3 / 2, H * 3 / 2 );
			if( variant == 1 && f == 28 )
				s.resize( W, H );
			if( variant == 2 && f == 20 )
			{
				set( s.plugin, "Columns", 24.0f );
				set( s.plugin, "Rows", 13.0f );
				columns = 24;
				rows    = 13;
			}
			if( !s.render( f, flat( s.width, s.height, f < 15 ? 1.0 : 0.3 ) ) )
				return false;
			double seconds = 0.0, phase0 = 0.0;
			int n = 0;
			s.plugin.LastFrameForTest( seconds, n, phase0 );
			floatBound += n * ulpAt( 3000.0 );
		}
		temps.clear();
		for( int r = 0; r < rows; ++r )
			for( int c = 0; c < columns; ++c )
			{
				int x, y;
				s.cellPixel( c, r, columns, rows, x, y );
				float px[ 4 ];
				s.readPixel( x, y, px );
				temps.push_back( px[ 0 ] );
			}
		s.end();
		return true;
	};
	std::vector< double > a, b, c;
	double boundA = 0.0, boundB = 0.0, boundC = 0.0;
	if( !run( 0, a, boundA ) || !run( 1, b, boundB ) || !run( 2, c, boundC ) || a.empty() )
		return 1;
	//The same float operations on the same numbers: identical on a
	//deterministic GPU. The software renderer is not bit-repeatable, so the
	//bound is one ULP of T per substep taken.
	const double tolerance = std::max( boundA, boundB );
	double worstB = 0.0, worstC = 0.0;
	for( size_t i = 0; i < b.size(); ++i )
		worstB = std::max( worstB, std::fabs( b[ i ] - a[ i ] ) );
	for( double v : c )
		worstC = std::max( worstC, std::fabs( v - a[ 0 ] ) );
	int failures = 0;
	failures += report( worstB <= tolerance, quiet, "output resized to %dx%d and back mid-fade: all %zu bulbs hold the unresized run's T (%.2f K), worst %.2e K (tolerance %.1e)",
	                    W * 3 / 2, H * 3 / 2, b.size(), a[ 0 ], worstB, tolerance );
	failures += report( worstC <= tolerance, quiet, "grid changed 16x9 -> 24x13 mid-fade: all %zu new bulbs carry on from the old T, worst %.2e K (tolerance %.1e)", c.size(), worstC, tolerance );
	failures += report( a[ 0 ] > 1000.0, quiet, "the fade was hot when it was resized (T %.1f K at frame %d, not the ambient)", a[ 0 ], frames - 1 );
	return failures;
}

//---------------------------------------------------------------------------
// --negative
//---------------------------------------------------------------------------
int runNegative( int W, int H )
{
	std::printf( "negative controls: each perturbation of the plugin's model must FAIL its check, %dx%d\n", W, H );
	struct Control
	{
		int bits;
		const char* what;
		int ( *check )( int, int, int, bool );
		const char* name;
	};
	const Control list[] = {
		{ lamp::kPerturbConstantR, "R held at its hot value (no inrush)", runRise, "--rise" },
		{ lamp::kPerturbLinearCool, "radiation as a linear term", runFall, "--fall" },
		{ lamp::kPerturbDC, "DC at the same RMS, no mains waveform", runRipple, "--ripple" },
		{ lamp::kPerturbNoConduction, "the conduction term dropped", runSteady, "--steady" },
		{ lamp::kPerturbTableShift, "the colour table read 5% hot", runColour, "--colour" },
		{ lamp::kPerturbResizeClears, "a resize or regrid restarts the wall cold", runResize, "--resize" },
	};
	int failures = 0;
	for( const Control& c : list )
	{
		const int before = g_failures;
		const int checks = g_checks;
		const int failed = c.check( W, H, c.bits, true );
		g_failures       = before;
		g_checks         = checks;
		failures += report( failed > 0, false, "%-42s -> %s fails (%d of its checks)", c.what, c.name, failed );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --names
//---------------------------------------------------------------------------
int runNames()
{
	std::printf( "names: nothing the host will silently truncate; every name unique\n" );
	Filament plugin;
	int failures = 0;
	std::set< std::string > seen;
	for( const NamedParameter& p : listParameters( plugin ) )
	{
		if( p.index >= Filament::PT_ABOUT_FIRST )
			continue;
		failures += report( p.name.size() <= 16, false, "%-16s %2zu characters", p.name.c_str(), p.name.size() );
		failures += report( seen.insert( p.name ).second, false, "%-16s unique", p.name.c_str() );
	}
	failures += report( std::string( "SW Filament" ).size() <= 16, false, "display name 'SW Filament' is %zu characters", std::string( "SW Filament" ).size() );
	return failures;
}

//---------------------------------------------------------------------------
// The moving card, for --out, the sweep, the bench and a default --pipe: a
// row of colour patches, a grey ramp, a white disc on an orbit, a static
// black square, a drifting blue bar, and a flashing patch (on for 6 frames
// in 30) so the time controls have an edge to act on.
//---------------------------------------------------------------------------
std::vector< unsigned char > buildCard( int width, int height, int64_t frame )
{
	std::vector< unsigned char > img( static_cast< size_t >( width ) * height * 4 );
	const double t  = static_cast< double >( frame ) / 60.0;
	const double cx = 0.72 + 0.14 * std::cos( 1.2 * t ), cy = 0.68 + 0.16 * std::sin( 1.2 * t );
	const double barX = std::fmod( 0.05 * t, 1.0 );
	const bool flash  = frame % 30 < 6;
	const double patches[ 8 ][ 3 ] = {
		{ 0.80, 0.10, 0.10 }, { 0.88, 0.67, 0.55 }, { 0.90, 0.85, 0.15 }, { 0.15, 0.60, 0.20 },
		{ 0.20, 0.80, 0.85 }, { 0.15, 0.25, 0.85 }, { 0.53, 0.81, 0.92 }, { 0.95, 0.95, 0.95 },
	};
	for( int y = 0; y < height; ++y )
		for( int x = 0; x < width; ++x )
		{
			const double fx = ( x + 0.5 ) / width, fy = ( y + 0.5 ) / height;
			double r = 0.40, g = 0.40, b = 0.40;
			if( fy > 0.06 && fy < 0.30 )
			{
				const int i = std::clamp( static_cast< int >( ( fx - 0.04 ) / 0.115 ), 0, 7 );
				if( fx > 0.04 && fx < 0.96 && std::fmod( fx - 0.04, 0.115 ) < 0.105 )
				{
					r = patches[ i ][ 0 ];
					g = patches[ i ][ 1 ];
					b = patches[ i ][ 2 ];
				}
			}
			if( fy > 0.36 && fy < 0.46 && fx > 0.04 && fx < 0.96 )
				r = g = b = ( fx - 0.04 ) / 0.92;
			if( fx > 0.08 && fx < 0.28 && fy > 0.56 && fy < 0.92 )
				r = g = b = flash ? 0.98 : 0.02;
			const double dx = ( fx - cx ) * width, dy = ( fy - cy ) * height;
			if( dx * dx + dy * dy < ( height * 0.08 ) * ( height * 0.08 ) )
				r = g = b = 0.98;
			if( std::fabs( fx - barX ) < 0.02 && fy > 0.5 )
			{
				r = 0.2;
				g = 0.3;
				b = 0.9;
			}
			unsigned char* px = img.data() + ( static_cast< size_t >( y ) * width + x ) * 4;
			px[ 0 ]           = static_cast< unsigned char >( std::lround( 255.0 * r ) );
			px[ 1 ]           = static_cast< unsigned char >( std::lround( 255.0 * g ) );
			px[ 2 ]           = static_cast< unsigned char >( std::lround( 255.0 * b ) );
			px[ 3 ]           = 255;
		}
	return img;
}

//---------------------------------------------------------------------------
// --bench
//---------------------------------------------------------------------------
double benchAt( Filament& plugin, int width, int height, int frames, double fps, size_t& stateBytes, int& substeps )
{
	Session session;
	session.floatOutput = false;
	for( const NamedParameter& p : listParameters( plugin ) )
		if( p.index < Filament::PT_ABOUT_FIRST && p.type != FF_TYPE_BUFFER && p.type != FF_TYPE_EVENT && p.type != FF_TYPE_TEXT )
			session.plugin.SetFloatParameter( p.index, p.value );
	session.fps = fps;
	if( !session.begin( width, height ) )
		return -1.0;

	//The card is uploaded once: a host's frame is already on the GPU, and
	//the plugin's cost does not depend on what the frame holds.
	const std::vector< unsigned char > card = buildCard( width, height, 0 );
	const int warmup                        = 20;
	for( int frame = 0; frame < warmup; ++frame )
		session.render( frame, card );
	glFinish();

	double best = 1e9;
	for( int run = 0; run < 3; ++run )
	{
		const auto start = std::chrono::steady_clock::now();
		for( int frame = 0; frame < frames; ++frame )
			session.renderAt( warmup + run * frames + frame );
		glFinish();
		const auto end       = std::chrono::steady_clock::now();
		const double seconds = std::chrono::duration< double >( end - start ).count();
		best                 = std::min( best, seconds * 1000.0 / static_cast< double >( frames ) );
	}
	stateBytes = session.plugin.StateBytesForTest();
	double seconds = 0.0, phase0 = 0.0;
	session.plugin.LastFrameForTest( seconds, substeps, phase0 );
	session.end();
	return best;
}

int runBench( Filament& plugin, int frames, double fps )
{
	struct Size
	{
		const char* name;
		int width, height;
	};
	const Size sizes[] = {
		{ "1280x720  ", 1280, 720 },
		{ "1920x1080 ", 1920, 1080 },
		{ "3840x2160 ", 3840, 2160 },
	};
	const int columns = static_cast< int >( plugin.GetFloatParameter( Filament::PT_COLUMNS ) );
	const int rows    = static_cast< int >( plugin.GetFloatParameter( Filament::PT_ROWS ) );
	std::printf( "%d frames each, best of three runs, after a 20-frame warm-up, glFinish both sides, the card uploaded once, %dx%d bulbs.\n\n", frames, columns, rows );
	std::printf( "resolution     ms/frame   equivalent fps   %% of a 60fps frame   substeps   state held\n" );
	for( const Size& size : sizes )
	{
		size_t bytes    = 0;
		int substeps    = 0;
		const double ms = benchAt( plugin, size.width, size.height, frames, fps, bytes, substeps );
		std::printf( "%s    %7.3f       %8.0f            %5.1f%%          %4d     %6.2f MB\n", size.name, ms, ms > 0.0 ? 1000.0 / ms : 0.0,
		             ms / 16.667 * 100.0, substeps, static_cast< double >( bytes ) / 1048576.0 );
	}
	std::printf( "\nState is the grid's buffers only (two temperatures, the drive, the light, the\n"
	             "bloom), so it does not grow with the output raster. Whatever the settings above\n"
	             "were, they are what was measured; --set measures another.\n" );
	return 0;
}

//---------------------------------------------------------------------------
// --dump-shaders
//---------------------------------------------------------------------------
int dumpShaders( const std::string& dir )
{
	namespace shaders = filament::shaders;
	const std::pair< const char*, std::string > files[] = {
		{ "vertex.vert", shaders::Vertex() }, { "drive.frag", shaders::Drive() }, { "thermal.frag", shaders::Thermal() },
		{ "fill.frag", shaders::Fill() },     { "regrid.frag", shaders::Regrid() }, { "blur.frag", shaders::Blur() },
		{ "output.frag", shaders::Output() },
	};
	for( const auto& f : files )
	{
		std::ofstream out( dir + "/" + f.first );
		if( !out )
		{
			std::fprintf( stderr, "cannot write %s/%s\n", dir.c_str(), f.first );
			return 1;
		}
		out << f.second;
	}
	std::printf( "wrote %zu shaders to %s\n", sizeof( files ) / sizeof( files[ 0 ] ), dir.c_str() );
	return 0;
}

//---------------------------------------------------------------------------
// --pipe cue sheet: one 'frame Name Value' per line, the fleet's format.
//
// A STANDARD parameter ramps linearly between cues. An option, a boolean
// and an integer STEP: they hold the last cue at or before the frame,
// because there is nothing between Clear and Amber to ramp through. An
// event fires on its cue frame only.
//---------------------------------------------------------------------------
using Track = std::vector< std::pair< int, float > >;

std::map< std::string, Track > loadScript( const std::string& path, std::string& error )
{
	std::map< std::string, Track > tracks;
	std::ifstream file( path );
	if( !file )
	{
		error = "cannot open " + path;
		return tracks;
	}
	std::string line;
	int lineNumber = 0;
	while( std::getline( file, line ) )
	{
		++lineNumber;
		const size_t hash = line.find( '#' );
		if( hash != std::string::npos )
			line.erase( hash );
		std::istringstream in( line );
		int frame = 0;
		if( !( in >> frame ) )
			continue;
		std::vector< std::string > words;
		std::string word;
		while( in >> word )
			words.push_back( word );
		if( words.size() < 2 )
		{
			error = path + ":" + std::to_string( lineNumber ) + ": expected `frame Parameter Name value`";
			return {};
		}
		const float value = std::strtof( words.back().c_str(), nullptr );
		words.pop_back();
		std::string name = words.front();
		for( size_t i = 1; i < words.size(); ++i )
			name += " " + words[ i ];
		tracks[ name ].emplace_back( frame, value );
	}
	for( auto& entry : tracks )
		std::sort( entry.second.begin(), entry.second.end() );
	return tracks;
}

float valueAt( const Track& track, int frame, unsigned int type )
{
	if( track.empty() )
		return 0.0f;
	if( type == FF_TYPE_EVENT )
	{
		for( const auto& cue : track )
			if( cue.first == frame )
				return cue.second;
		return 0.0f;
	}
	if( frame <= track.front().first )
		return track.front().second;
	if( frame >= track.back().first )
		return track.back().second;
	for( size_t i = 1; i < track.size(); ++i )
		if( frame <= track[ i ].first )
		{
			const auto& a = track[ i - 1 ];
			const auto& b = track[ i ];
			if( type != FF_TYPE_STANDARD )
				return frame == b.first ? b.second : a.second;
			const float span = static_cast< float >( b.first - a.first );
			const float t    = span > 0.0f ? static_cast< float >( frame - a.first ) / span : 1.0f;
			return a.second + ( b.second - a.second ) * t;
		}
	return track.back().second;
}

//---------------------------------------------------------------------------
void usage()
{
	std::printf(
		"fitest -- render and measure the Filament bulb wall\n"
		"\n"
		"  --out PATH          render the moving card through the plugin (default /tmp/filament.png)\n"
		"  --size WxH          raster (default 1280x720)\n"
		"  --frames N          frames to render before reading back (default 90)\n"
		"  --fps N             synthetic frame rate driving the clock (default 60)\n"
		"  --source card|flat|white|black   what to feed (card moves); --level V for flat\n"
		"  --set \"Name=V\"      set a parameter by its display name (element index for options,\n"
		"                      the integer for Columns and Rows). Repeatable.\n"
		"  --list              every parameter, its kind, default and range\n"
		"\n"
		"  checks that render, at --size:\n"
		"  --steady            constant RMS settles at the power balance; rated at 2800 K\n"
		"  --colour            the light sits on the Planckian locus; dimming is redward\n"
		"  --rise              a cold start follows the R( T ) ODE and beats constant R\n"
		"  --fall              switch-off follows T^4 + conduction, not an exponential\n"
		"  --ripple            triac ripple at exactly 2 x mains, bigger on a small lamp\n"
		"  --resize            an output resize and a grid change keep the temperatures\n"
		"  --negative          every check above can fail\n"
		"  --perturb BITS      run the checks verbosely against a perturbed model (bits in Lamp.h)\n"
		"\n"
		"  checks that need no GL:\n"
		"  --names             nothing the host will silently truncate\n"
		"\n"
		"  --bench             time ProcessOpenGL at 720p, 1080p and 4K, and the state held\n"
		"  --dump-shaders DIR  write the exact GLSL the plugin compiles\n"
		"  --pipe              raw RGBA frames on stdin, raw RGBA frames on stdout\n"
		"  --script PATH       parameter cues for --pipe: 'frame Name Value'\n"
		"  --help\n" );
}
} // namespace

int main( int argc, char** argv )
{
	std::string outPath = "/tmp/filament.png";
	std::string scriptPath;
	std::string dumpDir;
	std::string source = "card";
	double level   = 0.5;
	int width      = 1280;
	int height     = 720;
	int frames     = 90;
	int failRender = -1;
	int perturb    = 0;
	double fps     = 60.0;
	bool wantList  = false;
	bool wantBench = false;
	bool wantPipe  = false;
	bool allowNoGL = false;
	std::vector< std::string > settings;
	std::vector< std::string > checks;

	const std::set< std::string > rendered = { "--steady", "--colour", "--rise", "--fall", "--ripple", "--resize", "--negative" };
	const std::set< std::string > offline  = { "--names" };

	for( int i = 1; i < argc; ++i )
	{
		const std::string argument = argv[ i ];
		const bool hasNext         = i + 1 < argc;
		if( argument == "--help" || argument == "-h" )
		{
			usage();
			return 0;
		}
		else if( argument == "--out" && hasNext )
			outPath = argv[ ++i ];
		else if( argument == "--script" && hasNext )
			scriptPath = argv[ ++i ];
		else if( argument == "--dump-shaders" && hasNext )
			dumpDir = argv[ ++i ];
		else if( argument == "--source" && hasNext )
			source = argv[ ++i ];
		else if( argument == "--level" && hasNext )
			level = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--size" && hasNext )
		{
			const std::string size = argv[ ++i ];
			const size_t x         = size.find( 'x' );
			if( x == std::string::npos )
			{
				std::fprintf( stderr, "--size wants WxH\n" );
				return 2;
			}
			width  = std::atoi( size.substr( 0, x ).c_str() );
			height = std::atoi( size.substr( x + 1 ).c_str() );
		}
		else if( argument == "--frames" && hasNext )
			frames = std::atoi( argv[ ++i ] );
		else if( argument == "--fps" && hasNext )
			fps = std::strtod( argv[ ++i ], nullptr );
		else if( argument == "--set" && hasNext )
			settings.push_back( argv[ ++i ] );
		else if( argument == "--perturb" && hasNext )
			perturb = std::atoi( argv[ ++i ] );
		else if( argument == "--fail-render-at" && hasNext )
			failRender = std::atoi( argv[ ++i ] );//test hook: verify.sh proves --pipe exits 1 on a failed render
		else if( argument == "--list" )
			wantList = true;
		else if( argument == "--bench" )
			wantBench = true;
		else if( argument == "--pipe" )
			wantPipe = true;
		else if( argument == "--allow-no-gl" )
			allowNoGL = true;
		else if( rendered.count( argument ) || offline.count( argument ) )
			checks.push_back( argument );
		else
		{
			std::fprintf( stderr, "unknown argument: %s\n", argument.c_str() );
			usage();
			return 2;
		}
	}

	if( width <= 0 || height <= 0 || frames <= 0 || fps <= 0.0 )
	{
		std::fprintf( stderr, "width, height, frames and fps must all be positive\n" );
		return 2;
	}

	if( !dumpDir.empty() )
		return dumpShaders( dumpDir );

	if( wantList )
	{
		//No GL needed: answered before a context is made, so it works in CI.
		Filament plugin;
		std::printf( "%3s  %-16s  %-9s  %-8s  %s\n", "id", "name", "kind", "default", "range" );
		for( const NamedParameter& p : listParameters( plugin ) )
			std::printf( "%3u  %-16s  %-9s  %.4f    [%g..%g]\n", p.index, p.name.c_str(), kindName( p ), p.value, p.low, p.high );
		return 0;
	}

	if( !checks.empty() )
	{
		bool needGL = false;
		for( const std::string& check : checks )
		{
			if( check == "--names" )
			{
				runNames();
				std::printf( "\n" );
			}
			else
				needGL = true;
		}

		if( needGL )
		{
			CGLContextObj context = createContext();
			if( context == nullptr && allowNoGL )
				std::printf( "   SKIP  could not create an OpenGL 4.1 core context, accelerated or software.\n"
				             "         The rendering checks and their negative controls were NOT run.\n" );
			else if( context == nullptr )
			{
				std::printf( "   FAIL  could not create an OpenGL 4.1 core context\n" );
				++g_failures;
			}
			else
			{
				for( const std::string& check : checks )
				{
					if( check == "--steady" )
						runSteady( width, height, perturb );
					else if( check == "--colour" )
						runColour( width, height, perturb );
					else if( check == "--rise" )
						runRise( width, height, perturb );
					else if( check == "--fall" )
						runFall( width, height, perturb );
					else if( check == "--ripple" )
						runRipple( width, height, perturb );
					else if( check == "--resize" )
						runResize( width, height, perturb );
					else if( check == "--negative" )
						runNegative( width, height );
					else
						continue;
					std::printf( "\n" );
				}
				CGLSetCurrentContext( nullptr );
				CGLDestroyContext( context );
			}
		}
		std::printf( "%d checks, %d failed\n", g_checks, g_failures );
		return g_failures == 0 ? 0 : 1;
	}

	CGLContextObj context = createContext();
	if( context == nullptr )
	{
		std::fprintf( stderr, "could not create an OpenGL context\n" );
		return 1;
	}
	auto finish = [ & ]( int result ) {
		CGLSetCurrentContext( nullptr );
		CGLDestroyContext( context );
		return result;
	};

	Session session;
	session.floatOutput = false;
	session.fps         = fps;
	for( const std::string& setting : settings )
	{
		std::string error;
		if( applySetting( session.plugin, setting, error ) )
			continue;
		std::fprintf( stderr, "--set %s: %s\n", setting.c_str(), error.c_str() );
		return finish( 2 );
	}
	session.plugin.SetPerturbForTest( perturb );

	if( wantBench )
		return finish( runBench( session.plugin, frames < 40 ? 60 : frames, fps ) );

	if( wantPipe )
	{
		//Everything but the video goes to stderr: one stray byte in stdout is
		//a torn frame for the rest of the reel.
		struct Automation
		{
			unsigned int index;
			unsigned int type;
			Track track;
		};
		std::vector< Automation > automation;
		if( !scriptPath.empty() )
		{
			std::string error;
			const std::map< std::string, Track > tracks = loadScript( scriptPath, error );
			if( !error.empty() )
			{
				std::fprintf( stderr, "%s\n", error.c_str() );
				return finish( 2 );
			}
			for( const auto& entry : tracks )
			{
				const int index = indexOfParameter( session.plugin, entry.first );
				if( index < 0 )
				{
					std::fprintf( stderr, "script names '%s', which is not a parameter (try --list)\n", entry.first.c_str() );
					return finish( 2 );
				}
				automation.push_back( { static_cast< unsigned int >( index ), session.plugin.GetParamType( static_cast< unsigned int >( index ) ), entry.second } );
			}
		}

		//A closed stdout must be a failed write we can see, not a SIGPIPE
		//that kills the process with 141 before it can say so.
		std::signal( SIGPIPE, SIG_IGN );

		if( !session.begin( width, height ) )
			return finish( 1 );

		std::vector< unsigned char > frame( static_cast< size_t >( width ) * height * 4 );
		int status = 0;
		for( int index = 0;; ++index )
		{
			size_t got = 0;
			while( got < frame.size() )
			{
				const ssize_t n = read( STDIN_FILENO, frame.data() + got, frame.size() - got );
				if( n <= 0 )
					break;
				got += static_cast< size_t >( n );
			}
			//A partial frame is the end of the stream, never a frame.
			if( got < frame.size() )
			{
				if( got > 0 )
					std::fprintf( stderr, "partial frame at the end (%zu of %zu bytes, %dx%d): dropped\n", got, frame.size(), width, height );
				break;
			}

			//Through the plugin's own setter, so a cue moves what a slider
			//would, and an event is a press.
			for( const Automation& a : automation )
				session.plugin.SetFloatParameter( a.index, valueAt( a.track, index, a.type ) );

			const bool ok = index != failRender && session.render( index, frame );
			if( !ok )
			{
				std::fprintf( stderr, "render failed at frame %d\n", index );
				status = 1;
				break;
			}

			const std::vector< unsigned char > out = session.readBack();
			size_t written                         = 0;
			while( written < out.size() )
			{
				const ssize_t put = write( STDOUT_FILENO, out.data() + written, out.size() - written );
				if( put <= 0 )
					break;
				written += static_cast< size_t >( put );
			}
			//The reader has gone: rendering on into a closed pipe is work
			//nobody will see, and a short frame is worse than none.
			if( written < out.size() )
			{
				std::fprintf( stderr, "stdout closed at frame %d\n", index );
				status = 1;
				break;
			}
		}
		session.end();
		return finish( status );
	}

	if( !session.begin( width, height ) )
		return finish( 1 );
	for( int frame = 0; frame < frames; ++frame )
	{
		bool ok = false;
		if( source == "card" )
			ok = session.render( frame, buildCard( width, height, frame ) );
		else if( source == "flat" )
			ok = session.render( frame, flat( width, height, level ) );
		else if( source == "white" )
			ok = session.render( frame, flat( width, height, 1.0 ) );
		else if( source == "black" )
			ok = session.render( frame, flat( width, height, 0.0 ) );
		else
		{
			std::fprintf( stderr, "unknown --source %s\n", source.c_str() );
			return finish( 2 );
		}
		if( !ok )
			return finish( 1 );
	}

	const std::vector< unsigned char > image = session.readBack();
	session.end();
	if( !writePng( outPath, width, height, image ) )
	{
		std::fprintf( stderr, "could not write %s\n", outPath.c_str() );
		return finish( 1 );
	}
	std::printf( "wrote %s (%dx%d, %d frames)\n", outPath.c_str(), width, height, frames );
	return finish( 0 );
}
