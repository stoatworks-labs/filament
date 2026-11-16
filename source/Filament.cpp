#include "Filament.h"

#include "Controls.h"
#include "Diag.h"
#include "GelTable.h"
#include "Lamp.h"
#include "Model.h"
#include "Organ.h"
#include "PlanckTable.h"
#include "Shaders.h"

#include <ffglex/FFGLScopedFBOBinding.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace ffglex;
using namespace filament;

static CFFGLPluginInfo PluginInfo(
	PluginFactory< Filament >,                                   // Create method
	"FI01",                                                      // Plugin unique ID of maximum length 4.
	"SW Filament",                                               // Plugin name
	2,                                                           // API major version number
	1,                                                           // API minor version number
	0,                                                           // Plugin major version number
	1,                                                           // Plugin minor version number
	FF_EFFECT,                                                   // Plugin type
	"The picture on a wall of incandescent bulbs.\n\nEach bulb is a tungsten filament whose dimmer is set by its cell of the clip, and whose temperature follows the filament's own heat balance: power in through a resistance that rises fifteenfold from cold, out by radiation as T^4 and by conduction. Dim a bulb and it goes red as well as dark; a flash rises fast and falls slowly; a cold bulb surges; a small filament flickers at twice the mains. The light is Planck's law at the filament's temperature through the CIE observer.",// Plugin description
	"Filament FFGL effect"                                       // About
);

namespace
{
/// Frames that must agree before the host's clock unit is settled.
constexpr int kClockVotes = 4;

/// The organ's ramps are tagged with their half-cycle's number modulo this,
/// so a tag is an exact float (and wraps once in 11 hours at 50 Hz).
constexpr int64_t kTagModulus = int64_t( 1 ) << 22;

static_assert( filament::gels::kRowCount == filament::planck::kCount, "the gel table is on the Planck table's grid" );
static_assert( filament::gels::kFirstKelvin == filament::planck::kFirstKelvin && filament::gels::kStepKelvin == filament::planck::kStepKelvin,
               "the gel table is on the Planck table's grid" );

double wallSeconds()
{
	using namespace std::chrono;
	static const steady_clock::time_point start = steady_clock::now();
	return duration_cast< duration< double > >( steady_clock::now() - start ).count();
}

std::string glStringOrUnknown( GLenum name )
{
	const GLubyte* value = glGetString( name );
	return value ? reinterpret_cast< const char* >( value ) : "unknown";
}
} // namespace

//---------------------------------------------------------------------------
Filament::Filament() :
	organ( std::make_unique< organ::Organ >() )
{
	SetMinInputs( 1 );
	SetMaxInputs( 1 );

	//The filaments integrate over host time.
	SetTimeSupported( true );

	//---------------------------------------------------------------------
	// Defaults: a 48 x 27 wall of clear 40 W bulbs on 50 Hz mains through
	// square-law dimmers, a modest halo and bloom. Chosen on Resolume's demo
	// clips (AGENTS.md, "Decisions").
	//---------------------------------------------------------------------
	params[ PT_COLUMNS ]    = 48.0f;
	params[ PT_ROWS ]       = 27.0f;
	params[ PT_GAP ]        = 0.3f;
	params[ PT_PIXEL_MODE ] = 0.0f;
	params[ PT_GLASS ]      = 0.0f;//Clear
	params[ PT_GELS ]       = 0.0f;//Off

	params[ PT_WATTAGE ] = controls::WattsParam( 40.0 );
	params[ PT_DIMMER ]  = 1.0f;//Square Law
	params[ PT_MAINS ]   = 0.0f;//50 Hz
	params[ PT_AMBIENT ] = 0.125f;//25 C

	params[ PT_GLOW ]     = 0.5f;
	params[ PT_BLOOM ]    = 0.35f;
	params[ PT_EXPOSURE ] = 0.5f;
	params[ PT_MIX ]      = 1.0f;

	//The colour organ: off, so a v0.1.0 composition renders as it did. The
	//bands follow ETI 428's stated split: red up to "the A an octave below
	//middle C", read as 220 Hz, and green over the next three octaves, to
	//1760 Hz. The gains were set on Resolume's six Beat demo clips, each
	//played through its own video (SPEC-filament-organ.md, "Defaults").
	params[ PT_ORGAN ]          = 0.0f;//Off
	params[ PT_CROSSOVER_LOW ]  = organ::CrossoverLowParam( 220.0 );
	params[ PT_CROSSOVER_HIGH ] = organ::CrossoverHighParam( 1760.0 );
	params[ PT_BASS_GAIN ]      = organ::GainParam( 48.0 );
	params[ PT_MID_GAIN ]       = organ::GainParam( 51.0 );
	params[ PT_TREBLE_GAIN ]    = organ::GainParam( 46.0 );
	params[ PT_BASS_GEL ]       = 0.0f;//106 Primary Red
	params[ PT_MID_GEL ]        = 1.0f;//139 Primary Green
	params[ PT_TREBLE_GEL ]     = 2.0f;//119 Dark Blue
	params[ PT_AUDIO_INPUT ]    = 0.0f;//None
	params[ PT_RESCAN ]         = 0.0f;
	params[ PT_STATUS_LAMP ]    = 1.0f;//On

	//---------------------------------------------------------------------
	// Declaration. Every STANDARD parameter is a plain 0..1 float: SetParamInfo
	// clamps a STANDARD default into 0..1 before a range can be attached.
	// Columns and Rows are integers with real ranges. Option lists are in
	// their natural order.
	//---------------------------------------------------------------------
	auto declareOptions = [ this ]( unsigned int id, const char* name, int count, const char* ( *nameAt )( int ) ) {
		SetOptionParamInfo( id, name, static_cast< unsigned int >( count ), params[ id ] );
		for( int i = 0; i < count; ++i )
			SetParamElementInfo( id, static_cast< unsigned int >( i ), nameAt( i ), static_cast< float >( i ) );
	};

	SetParamInfo( PT_COLUMNS, "Columns", FF_TYPE_INTEGER, params[ PT_COLUMNS ] );
	SetParamRange( PT_COLUMNS, static_cast< float >( controls::kMinColumns ), static_cast< float >( controls::kMaxColumns ) );
	SetParamInfo( PT_ROWS, "Rows", FF_TYPE_INTEGER, params[ PT_ROWS ] );
	SetParamRange( PT_ROWS, static_cast< float >( controls::kMinRows ), static_cast< float >( controls::kMaxRows ) );
	SetParamInfof( PT_GAP, "Gap", FF_TYPE_STANDARD );
	SetParamInfo( PT_PIXEL_MODE, "Pixel Mode", FF_TYPE_BOOLEAN, false );
	declareOptions( PT_GLASS, "Glass", controls::kGlassCount, controls::GlassName );
	declareOptions( PT_GELS, "Gels", controls::kGelsCount, controls::GelsName );

	SetParamInfof( PT_WATTAGE, "Wattage", FF_TYPE_STANDARD );
	declareOptions( PT_DIMMER, "Dimmer", controls::kDimmerCount, controls::DimmerName );
	declareOptions( PT_MAINS, "Mains", controls::kMainsCount, controls::MainsName );
	SetParamInfof( PT_AMBIENT, "Ambient Temp", FF_TYPE_STANDARD );

	SetParamInfof( PT_GLOW, "Glow", FF_TYPE_STANDARD );
	SetParamInfof( PT_BLOOM, "Bloom", FF_TYPE_STANDARD );
	SetParamInfof( PT_EXPOSURE, "Exposure", FF_TYPE_STANDARD );
	SetParamInfof( PT_MIX, "Mix", FF_TYPE_STANDARD );

	for( FFUInt32 i = PT_COLUMNS; i <= PT_GELS; ++i )
		SetParamGroup( i, "Wall" );
	for( FFUInt32 i = PT_WATTAGE; i <= PT_AMBIENT; ++i )
		SetParamGroup( i, "Bulb" );
	for( FFUInt32 i = PT_GLOW; i <= PT_MIX; ++i )
		SetParamGroup( i, "Look" );

	// The About block. Inline rather than through a helper: SetParamInfo is
	// protected on CFFGLPlugin, so nothing outside the class can call it.
	SetParamInfo( PT_ABOUT_FIRST, "About", FF_TYPE_TEXT, stoatworks::about::defaultText() );
	{
		FFUInt32 aboutId = PT_ABOUT_FIRST + 1;
		for( const auto& b : stoatworks::about::buttons() )
			SetParamInfo( aboutId++, b.label, FF_TYPE_EVENT, false );
	}
	for( FFUInt32 i = PT_ABOUT_FIRST; i < PT_ABOUT_END; ++i )
		SetParamGroup( i, "About" );

	// The colour organ, after About because ids are never renumbered.
	// Declaring the device menu lists NO devices: it holds only the entries
	// that need no enumeration until the first frame the organ is on.
	declareOptions( PT_ORGAN, "Organ", organ::kSwitchCount, organ::SwitchName );
	SetParamInfof( PT_CROSSOVER_LOW, "Crossover Low", FF_TYPE_STANDARD );
	SetParamInfof( PT_CROSSOVER_HIGH, "Crossover High", FF_TYPE_STANDARD );
	SetParamInfof( PT_BASS_GAIN, "Bass Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_MID_GAIN, "Mid Gain", FF_TYPE_STANDARD );
	SetParamInfof( PT_TREBLE_GAIN, "Treble Gain", FF_TYPE_STANDARD );
	declareOptions( PT_BASS_GEL, "Bass Gel", organ::GelCount(), organ::GelName );
	declareOptions( PT_MID_GEL, "Mid Gel", organ::GelCount(), organ::GelName );
	declareOptions( PT_TREBLE_GEL, "Treble Gel", organ::GelCount(), organ::GelName );
	SetOptionParamInfo( PT_AUDIO_INPUT, "Audio Input", organ::kInputSlots, 0.0f );
	publishOrganMenu( false );
	// The keys are 23-bit integers; a host that clamps an option's value to
	// its range must not clamp them.
	SetParamRange( PT_AUDIO_INPUT, 0.0f, 8388608.0f );
	SetParamInfo( PT_RESCAN, "Rescan Inputs", FF_TYPE_EVENT, false );
	declareOptions( PT_STATUS_LAMP, "Status Lamp", organ::kSwitchCount, organ::SwitchName );
	for( FFUInt32 i = PT_ORGAN; i < PT_COUNT; ++i )
		SetParamGroup( i, "Organ" );

	FFGLLog::LogToHost( "Created Filament effect" );

	diag::init();
}

//---------------------------------------------------------------------------
FFResult Filament::InitGL( const FFGLViewportStruct* vp )
{
	diag::info( std::string( "GL vendor=" ) + glStringOrUnknown( GL_VENDOR )
	            + " renderer=" + glStringOrUnknown( GL_RENDERER )
	            + " version=" + glStringOrUnknown( GL_VERSION ) );

	const std::string vertex = shaders::Vertex();
	struct
	{
		FFGLShader* shader;
		std::string fragment;
		const char* name;
	} const stages[] = {
		{ &driveShader, shaders::Drive(), "drive" },
		{ &thermalShader, shaders::Thermal(), "thermal" },
		{ &fillShader, shaders::Fill(), "fill" },
		{ &regridShader, shaders::Regrid(), "regrid" },
		{ &blurShader, shaders::Blur(), "blur" },
		{ &outputShader, shaders::Output(), "output" },
	};

	for( const auto& stage : stages )
	{
		if( stage.shader->Compile( vertex, stage.fragment ) )
			continue;

		//Returning FF_FAIL here is invisible to the operator: the effect
		//simply does nothing in Resolume, with no message anywhere. These two
		//lines are the only record of which pass it was.
		diag::error( std::string( "the " ) + stage.name + " shader failed to compile - the effect will do nothing" );
		FFGLLog::LogToHost( "Filament: shader failed to compile" );
		DeInitGL();
		return FF_FAIL;
	}

	if( !quad.Initialise() )
	{
		diag::error( "quad geometry failed to initialise" );
		FFGLLog::LogToHost( "Filament: quad geometry failed to initialise" );
		DeInitGL();
		return FF_FAIL;
	}

	//The colour table: one row of texels, read with texelFetch only.
	glGenTextures( 1, &planckTexture );
	glBindTexture( GL_TEXTURE_2D, planckTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, planck::kCount, 1, 0, GL_RGBA, GL_FLOAT, planck::kTable );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	current   = 0;
	lastWidth = lastHeight = 0;
	lampKey.clear();

	diag::info( "initialised" );

	//Use base-class init as the success result so it retains the viewport.
	return CFFGLPlugin::InitGL( vp );
}

//---------------------------------------------------------------------------
FFResult Filament::SetTime( double time )
{
	hostTimeSeen = true;
	return CFFGLPlugin::SetTime( time );
}

//The unit voting is readout's, unchanged: the ratio of the host's clock
//delta to a steady clock's names the unit outright, and nothing plausible
//sits between 1 and 1000.
double Filament::nowSeconds()
{
	const double wallNow = wallSeconds();
	if( wallStart < 0.0 )
		wallStart = wallNow;

	if( !hostTimeSeen || hostTime < 0.0 )
		return wallNow - wallStart;

	const double raw = hostTime;

	if( clockScale == 0.0 && lastRawTime >= 0.0 && lastWallTime >= 0.0 )
	{
		const double hostDelta = raw - lastRawTime;
		const double wallDelta = wallNow - lastWallTime;

		//A paused host, a looping clip or a stalled frame tells us nothing.
		if( hostDelta > 0.0 && wallDelta >= 0.0005 )
		{
			const double ratio = hostDelta / wallDelta;
			if( ratio > 0.1 && ratio < 10.0 )
				++secondsVotes;
			else if( ratio > 100.0 && ratio < 10000.0 )
				++millisVotes;

			if( secondsVotes >= kClockVotes || millisVotes >= kClockVotes )
				clockScale = millisVotes > secondsVotes ? 0.001 : 1.0;
		}
	}
	lastRawTime  = raw;
	lastWallTime = wallNow;

	//Until the unit is settled, run on the real clock rather than assume one:
	//wrong in origin but right in rate, where assuming seconds would be a
	//thousand times fast on Resolume.
	return clockScale != 0.0 ? raw * clockScale : wallNow - wallStart;
}

//---------------------------------------------------------------------------
void Filament::setFilamentUniforms( FFGLShader& shader, int law, double ambient )
{
	using namespace lamp;
	float rho[ kRhoCount ];
	for( int i = 0; i < kRhoCount; ++i )
		rho[ i ] = static_cast< float >( kRho[ i ] );
	float low[ 5 ], high[ 5 ];
	for( int i = 0; i < 5; ++i )
	{
		low[ i ]  = static_cast< float >( kShomateLow[ i ] / kTungstenMolarMass );
		high[ i ] = static_cast< float >( kShomateHigh[ i ] / kTungstenMolarMass );
	}
	const double fourth = std::pow( kRatedKelvin, 4.0 ) - std::pow( kReferenceAmbient, 4.0 );

	glUniform1fv( shader.FindUniform( "Rho" ), kRhoCount, rho );
	glUniform1fv( shader.FindUniform( "ShomateLow" ), 5, low );
	glUniform1fv( shader.FindUniform( "ShomateHigh" ), 5, high );
	shader.Set( "RhoRated", static_cast< float >( Resistivity( kRatedKelvin ) ) );
	shader.Set( "WattsPerKg", static_cast< float >( lamp.wattsPerKg ) );
	shader.Set( "RadiatedNorm", static_cast< float >( ( 1.0 - kConductionFraction ) / fourth ) );
	shader.Set( "ConductedNorm", static_cast< float >( kConductionFraction / ( kRatedKelvin - kReferenceAmbient ) ) );
	shader.Set( "LinearNorm", static_cast< float >( ( 1.0 - kConductionFraction ) / ( kRatedKelvin - kReferenceAmbient ) ) );
	shader.Set( "Ambient", static_cast< float >( ambient ) );
	shader.Set( "Law", law );
	shader.Set( "Perturb", perturb );
	shader.Set( "PlanckFirst", static_cast< float >( planck::kFirstKelvin ) );
	shader.Set( "PlanckStep", static_cast< float >( planck::kStepKelvin ) );
	shader.Set( "PlanckCount", planck::kCount );
	shader.Set( "GelRed", model::kGelRed[ 0 ], model::kGelRed[ 1 ], model::kGelRed[ 2 ] );
	shader.Set( "GelGreen", model::kGelGreen[ 0 ], model::kGelGreen[ 1 ], model::kGelGreen[ 2 ] );
	shader.Set( "GelBlue", model::kGelBlue[ 0 ], model::kGelBlue[ 1 ], model::kGelBlue[ 2 ] );
}

//---------------------------------------------------------------------------
FFResult Filament::ProcessOpenGL( ProcessOpenGLStruct* pGL )
{
	if( pGL->numInputTextures < 1 || pGL->inputTextures[ 0 ] == nullptr )
		return FF_FAIL;

	const FFGLTextureStruct& input = *pGL->inputTextures[ 0 ];
	if( input.Width == 0 || input.Height == 0 )
		return FF_FAIL;

	const int width  = static_cast< int >( input.Width );
	const int height = static_cast< int >( input.Height );

	//The host's viewport, read before anything of ours changes it.
	//ScopedFBOBinding restores the framebuffer binding and only that.
	GLint hostViewport[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_VIEWPORT, hostViewport );

	//---------------------------------------------------------------------
	// The clock. The frame's seconds and the mains phase at its start are
	// reduced here in double: Resolume's clock is hundreds of millions of
	// milliseconds, where a float resolves tens of milliseconds, and a
	// 50 Hz waveform needs a tenth of one.
	//---------------------------------------------------------------------
	const double now = nowSeconds();
	double dt        = model::kNominalFrame;
	if( lastNow >= 0.0 )
		dt = std::clamp( now - lastNow, 0.0, model::kMaxFrameDelta );
	lastNow = now;
	if( ++clockFrames == 60 )
		diag::info( "host clock at frame 60: raw=" + std::to_string( hostTime ) + " scale=" + std::to_string( clockScale )
		            + " seconds=" + std::to_string( now ) );

	//---------------------------------------------------------------------
	// What the controls say.
	//---------------------------------------------------------------------
	const int columns    = std::clamp( static_cast< int >( std::lround( params[ PT_COLUMNS ] ) ), controls::kMinColumns, controls::kMaxColumns );
	const int rows       = std::clamp( static_cast< int >( std::lround( params[ PT_ROWS ] ) ), controls::kMinRows, controls::kMaxRows );
	const float gap      = controls::GapFraction( params[ PT_GAP ] );
	const bool pixelMode = params[ PT_PIXEL_MODE ] >= 0.5f;
	const int glass      = controls::OptionIndex( params[ PT_GLASS ], controls::kGlassCount );
	const int gels       = controls::OptionIndex( params[ PT_GELS ], controls::kGelsCount );
	const double watts   = controls::Watts( params[ PT_WATTAGE ] );
	const int law        = controls::OptionIndex( params[ PT_DIMMER ], controls::kDimmerCount );
	const double hertz   = controls::MainsHertz( controls::OptionIndex( params[ PT_MAINS ], controls::kMainsCount ) );
	const double ambient = controls::AmbientKelvin( params[ PT_AMBIENT ] );
	const float glow     = controls::Amount( params[ PT_GLOW ] );
	const float bloom    = controls::Amount( params[ PT_BLOOM ] );
	const float exposure = model::kExposureGain * std::exp2( controls::ExposureStops( params[ PT_EXPOSURE ] ) );
	const float mixAmount = controls::Amount( params[ PT_MIX ] );

	//The colour organ, on the first frame it is asked for and not before:
	//with it off, nothing below touches earshot, CoreAudio or its GL.
	const bool organOn = controls::OptionIndex( params[ PT_ORGAN ], organ::kSwitchCount ) == 1 && ensureOrgan();
	if( !organOn )
	{
		organ->Deactivate();
		rampFresh = true;
	}

	//The lamp, and how finely its ODE has to be stepped: recomputed only when
	//something it depends on moves (Stiffness scans the whole range).
	const std::string key = std::to_string( watts ) + "/" + std::to_string( hertz ) + "/" + std::to_string( ambient );
	if( key != lampKey )
	{
		lamp      = lamp::Make( watts, hertz );
		stiffness = lamp::Stiffness( lamp, ambient );
		lampKey   = key;
		diag::info( "lamp " + std::to_string( watts ) + " W " + std::to_string( lamp.volts ) + " V: wire "
		            + std::to_string( lamp.diameter * 1e6 ) + " um x " + std::to_string( lamp.length ) + " m, "
		            + std::to_string( lamp.mass * 1e6 ) + " mg, stiffness " + std::to_string( stiffness ) + "/s" );
	}
	int substeps = 0;
	if( dt > 0.0 )
	{
		const double hMax = std::min( 0.5 / hertz / lamp::kStepsPerHalfCycle, lamp::kStability / stiffness );
		substeps          = std::clamp( static_cast< int >( std::ceil( dt / hMax - 1e-9 ) ), 1, lamp::kMaxSubsteps );
	}
	const double h      = substeps > 0 ? dt / substeps : 0.0;
	const double start  = now - dt;
	const double phase0 = start * hertz - std::floor( start * hertz );
	frameSeconds  = dt;
	frameSubsteps = substeps;
	framePhase0   = phase0;

	//---------------------------------------------------------------------
	// Buffers, every allocation before anything binds a texture for the
	// frame's passes. The temperatures live at the GRID's raster, so an
	// output resize leaves them alone; a grid change carries them across.
	//---------------------------------------------------------------------
	const bool rasterChanged = lastWidth != 0 && ( lastWidth != width || lastHeight != height );
	lastWidth  = width;
	lastHeight = height;

	PassBuffer& live = state[ current ];
	bool fill        = false;
	if( !live.IsValid() )
	{
		if( !live.Ensure( columns, rows, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
		{
			diag::error( "could not allocate the filament state at " + std::to_string( columns ) + "x" + std::to_string( rows ) );
			return FF_FAIL;
		}
		fill = true;
	}
	else if( static_cast< int >( live.GetWidth() ) != columns || static_cast< int >( live.GetHeight() ) != rows )
	{
		PassBuffer& other = state[ 1 - current ];
		const int oldColumns = static_cast< int >( live.GetWidth() ), oldRows = static_cast< int >( live.GetHeight() );
		if( !other.Ensure( columns, rows, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
		{
			diag::error( "could not allocate the filament state at " + std::to_string( columns ) + "x" + std::to_string( rows ) );
			return FF_FAIL;
		}
		{
			ScopedFBOBinding fbo( other.GetGLID(), ScopedFBOBinding::RB_REVERT );
			other.ResizeViewPort();
			ScopedShaderBinding shader( regridShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( live.TextureID() );
			regridShader.Set( "State", 0 );
			regridShader.Set( "OldGrid", static_cast< float >( oldColumns ), static_cast< float >( oldRows ) );
			regridShader.Set( "NewGrid", static_cast< float >( columns ), static_cast< float >( rows ) );
			quad.Draw();
		}
		current = 1 - current;
		fill    = ( perturb & lamp::kPerturbResizeClears ) != 0;

		//The organ's ramps carry across too, the same way.
		PassBuffer& rampLive  = ramp[ rampCurrent ];
		PassBuffer& rampOther = ramp[ 1 - rampCurrent ];
		if( organOn && !rampFresh && rampLive.IsValid() )
		{
			if( !rampOther.Ensure( columns, rows, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
			{
				diag::error( "could not allocate the organ's ramps at " + std::to_string( columns ) + "x" + std::to_string( rows ) );
				return FF_FAIL;
			}
			ScopedFBOBinding fbo( rampOther.GetGLID(), ScopedFBOBinding::RB_REVERT );
			rampOther.ResizeViewPort();
			ScopedShaderBinding shader( regridShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( rampLive.TextureID() );
			regridShader.Set( "State", 0 );
			regridShader.Set( "OldGrid", static_cast< float >( oldColumns ), static_cast< float >( oldRows ) );
			regridShader.Set( "NewGrid", static_cast< float >( columns ), static_cast< float >( rows ) );
			quad.Draw();
			rampCurrent = 1 - rampCurrent;
		}
	}
	if( rasterChanged && ( perturb & lamp::kPerturbResizeClears ) != 0 )
		fill = true;//the negative control: the photofinish bug, on purpose

	PassBuffer& src = state[ current ];
	PassBuffer& dst = state[ 1 - current ];
	if( !dst.Ensure( columns, rows, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	    || !drive.Ensure( columns, rows, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	    || !light.Ensure( columns, rows, GL_RGBA32F, PassBuffer::Sampling::Nearest )
	    || !bloomX.Ensure( columns, rows, GL_RGBA16F, PassBuffer::Sampling::Nearest )
	    || !bloomY.Ensure( columns, rows, GL_RGBA16F, PassBuffer::Sampling::Linear ) )
	{
		diag::error( "could not allocate the grid buffers at " + std::to_string( columns ) + "x" + std::to_string( rows ) );
		return FF_FAIL;
	}
	if( organOn )
	{
		//The ramps live at the grid's raster. A new allocation, or a first
		//use, starts every ramp at "no half-cycle yet".
		//Only the ramp read this frame has to hold anything; the other is
		//the write target.
		PassBuffer& rampLive = ramp[ rampCurrent ];
		const bool kept      = rampLive.IsValid() && static_cast< int >( rampLive.GetWidth() ) == columns && static_cast< int >( rampLive.GetHeight() ) == rows;
		for( PassBuffer& b : ramp )
			if( !b.Ensure( columns, rows, GL_RGBA32F, PassBuffer::Sampling::Nearest ) )
			{
				diag::error( "could not allocate the organ's ramps at " + std::to_string( columns ) + "x" + std::to_string( rows ) );
				return FF_FAIL;
			}
		rampFresh = rampFresh || !kept;
		if( rampFresh )
		{
			const GLfloat none[ 4 ] = { 0.0f, 0.0f, 0.0f, -1.0f };
			for( PassBuffer& b : ramp )
			{
				ScopedFBOBinding fbo( b.GetGLID(), ScopedFBOBinding::RB_REVERT );
				glClearBufferfv( GL_COLOR, 0, none );
			}
			rampFresh = false;
		}
	}

	if( fill )
	{
		ScopedFBOBinding fbo( src.GetGLID(), ScopedFBOBinding::RB_REVERT );
		src.ResizeViewPort();
		ScopedShaderBinding shader( fillShader.GetGLID() );
		fillShader.Set( "Kelvin", static_cast< float >( ambient ) );
		quad.Draw();
	}

	const FFGLTexCoords maxCoords = GetMaxGLTexCoords( input );

	//---------------------------------------------------------------------
	// 1. Drive: each bulb's level.
	//---------------------------------------------------------------------
	{
		ScopedFBOBinding fbo( drive.GetGLID(), ScopedFBOBinding::RB_REVERT );
		drive.ResizeViewPort();
		ScopedShaderBinding shader( driveShader.GetGLID() );
		ScopedSamplerActivation sampler( 0 );
		Scoped2DTextureBinding texture( input.Handle );
		driveShader.Set( "Source", 0 );
		glUniform2i( driveShader.FindUniform( "SourceSize" ), width, height );
		driveShader.Set( "Grid", static_cast< float >( columns ), static_cast< float >( rows ) );
		quad.Draw();
	}

	//---------------------------------------------------------------------
	// The organ's audio, on the CPU: every sample of the frame through the
	// crossover and the detectors, then each half-cycle's control integral,
	// to a texture the organ's thermal pass searches.
	//---------------------------------------------------------------------
	float tag0     = 0.0f;
	int controlRows = 0;
	if( organOn )
	{
		organ->Activate();
		if( organ->ListCount() != organMenuShown )
			publishOrganMenu( true );
		organ::Settings settings;
		settings.lowHertz    = organ::CrossoverLowHertz( params[ PT_CROSSOVER_LOW ] );
		settings.highHertz   = organ::CrossoverHighHertz( params[ PT_CROSSOVER_HIGH ] );
		settings.gainDb[ 0 ] = organ::GainDb( params[ PT_BASS_GAIN ] );
		settings.gainDb[ 1 ] = organ::GainDb( params[ PT_MID_GAIN ] );
		settings.gainDb[ 2 ] = organ::GainDb( params[ PT_TREBLE_GAIN ] );
		settings.perturb     = perturb;

		//The half-cycles exactly as the shader will number them: from the
		//float Phase0 it is handed, so a row is never a half-cycle off.
		const double phase0f = static_cast< double >( static_cast< float >( phase0 ) );
		double cycle         = std::floor( start * hertz );
		if( ( perturb & organ::kPerturbFloatClock ) != 0 )
			cycle = std::floor( static_cast< double >( static_cast< float >( start ) ) * hertz );//the negative control
		const int64_t firstHalf = 2 * static_cast< int64_t >( cycle ) + static_cast< int64_t >( std::floor( 2.0 * phase0f ) );
		const double cyclesF    = static_cast< double >( static_cast< float >( h * hertz ) ) * substeps;
		const int halves        = static_cast< int >( std::floor( 2.0 * ( phase0f + cyclesF ) ) - std::floor( 2.0 * phase0f ) ) + 2;
		organ->Advance( start, now, hertz, firstHalf, halves, settings );

		controlRows = organ->RowCount();
		glBindTexture( GL_TEXTURE_2D, controlTexture );
		glTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, organ::kRowPoints, controlRows, GL_RGBA, GL_FLOAT, organ->Rows() );
		glBindTexture( GL_TEXTURE_2D, 0 );
		tag0 = static_cast< float >( ( ( firstHalf % kTagModulus ) + kTagModulus ) % kTagModulus );
	}

	//---------------------------------------------------------------------
	// 2. Thermal: the ODE over the frame, to the other state buffer and the
	// light buffer at once. The organ has its own pass (three filaments, the
	// triacs and the gels), so this one is compiled from v0.1.0's bytes.
	//---------------------------------------------------------------------
	if( organOn )
	{
		ScopedFBOBinding fbo( dst.GetGLID(), ScopedFBOBinding::RB_REVERT );
		PassBuffer& rampOut = ramp[ 1 - rampCurrent ];
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, light.TextureID(), 0 );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, rampOut.TextureID(), 0 );
		const GLenum targets[ 3 ] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2 };
		glDrawBuffers( 3, targets );
		dst.ResizeViewPort();
		{
			ScopedShaderBinding shader( organShader.GetGLID() );
			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding stateTexture( src.TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding driveTexture( drive.TextureID() );
			ScopedSamplerActivation sampler2( 2 );
			Scoped2DTextureBinding planckBinding( planckTexture );
			ScopedSamplerActivation sampler3( 3 );
			Scoped2DTextureBinding rampTexture( ramp[ rampCurrent ].TextureID() );
			ScopedSamplerActivation sampler4( 4 );
			Scoped2DTextureBinding controlBinding( controlTexture );
			ScopedSamplerActivation sampler5( 5 );
			Scoped2DTextureBinding gelBinding( gelTexture );

			setFilamentUniforms( organShader, law, ambient );
			organShader.Set( "State", 0 );
			organShader.Set( "Drive", 1 );
			organShader.Set( "Planck", 2 );
			organShader.Set( "Ramp", 3 );
			organShader.Set( "Control", 4 );
			organShader.Set( "Gels", 5 );
			organShader.Set( "Substeps", substeps );
			organShader.Set( "StepSeconds", static_cast< float >( h ) );
			organShader.Set( "StepCycles", static_cast< float >( h * hertz ) );
			organShader.Set( "Phase0", static_cast< float >( phase0 ) );
			organShader.Set( "Tag0", tag0 );
			organShader.Set( "Rows", controlRows );
			glUniform3i( organShader.FindUniform( "GelIndex" ),
			             controls::OptionIndex( params[ PT_BASS_GEL ], organ::GelCount() ),
			             controls::OptionIndex( params[ PT_MID_GEL ], organ::GelCount() ),
			             controls::OptionIndex( params[ PT_TREBLE_GEL ], organ::GelCount() ) );
			quad.Draw();
		}
		const GLenum one[ 1 ] = { GL_COLOR_ATTACHMENT0 };
		glDrawBuffers( 1, one );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0 );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT2, GL_TEXTURE_2D, 0, 0 );
		rampCurrent = 1 - rampCurrent;
	}
	else
	{
		ScopedFBOBinding fbo( dst.GetGLID(), ScopedFBOBinding::RB_REVERT );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, light.TextureID(), 0 );
		const GLenum targets[ 2 ] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
		glDrawBuffers( 2, targets );
		dst.ResizeViewPort();
		{
			ScopedShaderBinding shader( thermalShader.GetGLID() );
			ScopedSamplerActivation sampler0( 0 );
			Scoped2DTextureBinding stateTexture( src.TextureID() );
			ScopedSamplerActivation sampler1( 1 );
			Scoped2DTextureBinding driveTexture( drive.TextureID() );
			ScopedSamplerActivation sampler2( 2 );
			Scoped2DTextureBinding planckBinding( planckTexture );

			setFilamentUniforms( thermalShader, law, ambient );
			thermalShader.Set( "State", 0 );
			thermalShader.Set( "Drive", 1 );
			thermalShader.Set( "Planck", 2 );
			thermalShader.Set( "Substeps", substeps );
			thermalShader.Set( "StepSeconds", static_cast< float >( h ) );
			thermalShader.Set( "StepCycles", static_cast< float >( h * hertz ) );
			thermalShader.Set( "Phase0", static_cast< float >( phase0 ) );
			thermalShader.Set( "Gels", gels );
			quad.Draw();
		}
		//Leave the framebuffer as the SDK made it: one target.
		const GLenum one[ 1 ] = { GL_COLOR_ATTACHMENT0 };
		glDrawBuffers( 1, one );
		glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, GL_TEXTURE_2D, 0, 0 );
	}
	current = 1 - current;
	PassBuffer& latest = state[ current ];

	//---------------------------------------------------------------------
	// 3. Bloom: the light, blurred at the grid's raster, x then y.
	//---------------------------------------------------------------------
	{
		const float sigma = model::kBloomSigmaCells;
		struct
		{
			PassBuffer* target;
			GLuint from;
			int dx, dy;
		} const passes[] = { { &bloomX, light.TextureID(), 1, 0 }, { &bloomY, bloomX.TextureID(), 0, 1 } };
		for( const auto& pass : passes )
		{
			ScopedFBOBinding fbo( pass.target->GetGLID(), ScopedFBOBinding::RB_REVERT );
			pass.target->ResizeViewPort();
			ScopedShaderBinding shader( blurShader.GetGLID() );
			ScopedSamplerActivation sampler( 0 );
			Scoped2DTextureBinding texture( pass.from );
			blurShader.Set( "Light", 0 );
			glUniform2i( blurShader.FindUniform( "Direction" ), pass.dx, pass.dy );
			blurShader.Set( "Sigma", sigma );
			quad.Draw();
		}
	}

	//---------------------------------------------------------------------
	// 4. The wall, straight to the host.
	//---------------------------------------------------------------------
	{
		glBindFramebuffer( GL_FRAMEBUFFER, pGL->HostFBO );
		glViewport( hostViewport[ 0 ], hostViewport[ 1 ], hostViewport[ 2 ], hostViewport[ 3 ] );

		ScopedShaderBinding shader( outputShader.GetGLID() );
		ScopedSamplerActivation sampler0( 0 );
		Scoped2DTextureBinding lightTexture( light.TextureID() );
		ScopedSamplerActivation sampler1( 1 );
		Scoped2DTextureBinding stateTexture( latest.TextureID() );
		ScopedSamplerActivation sampler2( 2 );
		Scoped2DTextureBinding bloomTexture( bloomY.TextureID() );
		ScopedSamplerActivation sampler3( 3 );
		Scoped2DTextureBinding sourceTexture( input.Handle );
		ScopedSamplerActivation sampler4( 4 );
		Scoped2DTextureBinding planckBinding( planckTexture );

		float tint[ 3 ];
		controls::GlassTint( glass, tint );

		setFilamentUniforms( outputShader, law, ambient );
		outputShader.Set( "Light", 0 );
		outputShader.Set( "State", 1 );
		outputShader.Set( "Bloom", 2 );
		outputShader.Set( "Source", 3 );
		outputShader.Set( "Planck", 4 );
		outputShader.Set( "MaxUV", maxCoords.s, maxCoords.t );
		outputShader.Set( "Grid", static_cast< float >( columns ), static_cast< float >( rows ) );
		outputShader.Set( "Size", static_cast< float >( hostViewport[ 2 ] ), static_cast< float >( hostViewport[ 3 ] ) );
		outputShader.Set( "Gap", gap );
		outputShader.Set( "PixelMode", pixelMode ? 1 : 0 );
		//The organ's lamps sit behind a frosted panel, whatever the Glass.
		outputShader.Set( "Frosted", organOn || controls::GlassFrosted( glass ) ? 1 : 0 );
		outputShader.Set( "Glass", tint[ 0 ], tint[ 1 ], tint[ 2 ] );
		outputShader.Set( "Glow", glow );
		outputShader.Set( "BloomAmount", bloom );
		outputShader.Set( "ExposureGain", exposure );
		outputShader.Set( "Wall", model::kWall[ 0 ], model::kWall[ 1 ], model::kWall[ 2 ] );
		outputShader.Set( "Sheen", model::kGlassSheen );
		outputShader.Set( "MixAmount", mixAmount );
		outputShader.Set( "Probe", probe );
		outputShader.Set( "Gels", gels );
		quad.Draw();
	}

	if( organOn && controls::OptionIndex( params[ PT_STATUS_LAMP ], organ::kSwitchCount ) == 1 )
		drawStatusLamp( hostViewport, now );

	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
// The colour organ's GL side.
//---------------------------------------------------------------------------
bool Filament::ensureOrgan()
{
	if( organReady )
		return true;
	if( organFailed )
		return false;

	const std::string vertex = shaders::Vertex();
	if( !organShader.Compile( vertex, shaders::OrganThermal() ) || !lampShader.Compile( vertex, shaders::StatusLamp() ) )
	{
		diag::error( "the organ's shaders failed to compile - the organ stays off" );
		FFGLLog::LogToHost( "Filament: the organ's shaders failed to compile" );
		releaseOrgan();
		organFailed = true;
		return false;
	}

	//The gel table: a row of the Planck grid per gel, read with texelFetch.
	glGenTextures( 1, &gelTexture );
	glBindTexture( GL_TEXTURE_2D, gelTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, gels::kRowCount, gels::kGelCount, 0, GL_RGBA, GL_FLOAT, gels::kTable );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );

	//The control integral: a row per half-cycle, refilled every frame.
	const std::vector< float > zeros( static_cast< size_t >( organ::kRowPoints ) * organ::kMaxRows * 4, 0.0f );
	glGenTextures( 1, &controlTexture );
	glBindTexture( GL_TEXTURE_2D, controlTexture );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA32F, organ::kRowPoints, organ::kMaxRows, 0, GL_RGBA, GL_FLOAT, zeros.data() );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glBindTexture( GL_TEXTURE_2D, 0 );

	organReady = true;
	rampFresh  = true;
	diag::info( "organ initialised" );
	return true;
}

void Filament::releaseOrgan()
{
	organShader.FreeGLResources();
	lampShader.FreeGLResources();
	for( PassBuffer& b : ramp )
		b.Destroy();
	if( gelTexture != 0 )
		glDeleteTextures( 1, &gelTexture );
	if( controlTexture != 0 )
		glDeleteTextures( 1, &controlTexture );
	gelTexture = controlTexture = 0;
	organReady  = false;
	rampCurrent = 0;
	rampFresh   = true;
}

void Filament::publishOrganMenu( bool raise )
{
	const auto& slots = organ->Slots();
	for( int i = 0; i < organ::kInputSlots && i < static_cast< int >( slots.size() ); ++i )
		SetParamElementInfo( PT_AUDIO_INPUT, static_cast< unsigned int >( i ), slots[ static_cast< size_t >( i ) ].label.c_str(),
		                     slots[ static_cast< size_t >( i ) ].value );
	organMenuShown = organ->ListCount();
	if( raise )
		RaiseParamEvent( PT_AUDIO_INPUT, FF_EVENT_FLAG_ELEMENTS );
}

void Filament::drawStatusLamp( const GLint viewport[ 4 ], double now )
{
	float colour[ 3 ] = { 1.0f, 0.62f, 0.05f };//amber: nothing to listen to
	switch( organ->LampState() )
	{
	case organ::Lamp::Green:
		colour[ 0 ] = 0.15f, colour[ 1 ] = 1.0f, colour[ 2 ] = 0.25f;
		break;
	case organ::Lamp::Red:
		colour[ 0 ] = 1.0f, colour[ 1 ] = 0.05f, colour[ 2 ] = 0.02f;
		break;
	case organ::Lamp::RedBlink:
		colour[ 0 ] = now - std::floor( now ) < 0.5 ? 1.0f : 0.15f, colour[ 1 ] = 0.03f, colour[ 2 ] = 0.02f;
		break;
	case organ::Lamp::Idle: break;
	}
	//Top right, a little in from the corner.
	const float radius  = std::max( 3.0f, 0.014f * static_cast< float >( viewport[ 3 ] ) );
	const float cx      = static_cast< float >( viewport[ 0 ] + viewport[ 2 ] ) - 2.2f * radius;
	const float cy      = static_cast< float >( viewport[ 1 ] + viewport[ 3 ] ) - 2.2f * radius;
	const GLint reach   = static_cast< GLint >( std::ceil( radius ) ) + 3;
	const GLboolean scissorWas = glIsEnabled( GL_SCISSOR_TEST );
	const GLboolean blendWas   = glIsEnabled( GL_BLEND );
	GLint box[ 4 ] = { 0, 0, 0, 0 };
	glGetIntegerv( GL_SCISSOR_BOX, box );
	glEnable( GL_SCISSOR_TEST );
	glScissor( static_cast< GLint >( cx ) - reach, static_cast< GLint >( cy ) - reach, 2 * reach, 2 * reach );
	glEnable( GL_BLEND );
	glBlendFuncSeparate( GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE );
	{
		ScopedShaderBinding shader( lampShader.GetGLID() );
		lampShader.Set( "LampColour", colour[ 0 ], colour[ 1 ], colour[ 2 ] );
		lampShader.Set( "LampCentre", cx, cy );
		lampShader.Set( "LampRadius", radius );
		quad.Draw();
	}
	glBlendFunc( GL_ONE, GL_ZERO );
	if( !blendWas )
		glDisable( GL_BLEND );
	glScissor( box[ 0 ], box[ 1 ], box[ 2 ], box[ 3 ] );
	if( !scissorWas )
		glDisable( GL_SCISSOR_TEST );
}

//---------------------------------------------------------------------------
FFResult Filament::DeInitGL()
{
	driveShader.FreeGLResources();
	thermalShader.FreeGLResources();
	fillShader.FreeGLResources();
	regridShader.FreeGLResources();
	blurShader.FreeGLResources();
	outputShader.FreeGLResources();
	quad.Release();
	for( PassBuffer& b : state )
		b.Destroy();
	drive.Destroy();
	light.Destroy();
	bloomX.Destroy();
	bloomY.Destroy();
	releaseOrgan();
	if( planckTexture != 0 )
	{
		glDeleteTextures( 1, &planckTexture );
		planckTexture = 0;
	}
	current = 0;
	return FF_SUCCESS;
}

//---------------------------------------------------------------------------
FFResult Filament::SetFloatParameter( unsigned int index, float value )
{
	if( index >= PT_COUNT )
		return FF_FAIL;

	// An About button is a press, not a value to keep: it opens a browser and
	// nothing about the effect changes.
	if( index >= PT_ABOUT_FIRST && index < PT_ABOUT_END )
		return stoatworks::about::handleParam( index - PT_ABOUT_FIRST, value ) ? FF_SUCCESS : FF_FAIL;

	if( index == PT_RESCAN )
	{
		// On the press, once. Only while the organ is on: listing devices
		// starts CoreAudio, and organ off never does.
		const bool down = value >= 0.5f;
		if( down && !rescanHeld && organ->Rescan() )
			publishOrganMenu( true );
		rescanHeld      = down;
		params[ index ] = value;
		return FF_SUCCESS;
	}
	if( index == PT_AUDIO_INPUT )
		organ->Want( value );// stored; acted on only while the organ is on

	params[ index ] = value;
	return FF_SUCCESS;
}

float Filament::GetFloatParameter( unsigned int index )
{
	if( index >= PT_COUNT )
		return 0.0f;

	return params[ index ];
}

//---------------------------------------------------------------------------
char* Filament::GetTextParameter( unsigned int index )
{
	if( index == PT_ABOUT_FIRST )
	{
		aboutText = stoatworks::about::textParam( 0 );
		return const_cast< char* >( aboutText.c_str() );
	}

	return CFFGLPlugin::GetTextParameter( index );
}

FFResult Filament::SetTextParameter( unsigned int index, const char* value )
{
	// See the declaration: the base class fails, and a failed default deletes
	// the instance. The About line is display-only, so there is genuinely
	// nothing to store -- but it has to say so successfully.
	if( index == PT_ABOUT_FIRST )
		return FF_SUCCESS;

	return CFFGLPlugin::SetTextParameter( index, value );
}

//---------------------------------------------------------------------------
void Filament::SetClockScaleForTest( double scale )
{
	clockScale = scale;
}

void Filament::SetPerturbForTest( int bits )
{
	perturb = bits;
}

void Filament::SetProbeForTest( int p )
{
	probe = p;
}

void Filament::LastFrameForTest( double& seconds, int& substeps, double& phase0 ) const
{
	seconds  = frameSeconds;
	substeps = frameSubsteps;
	phase0   = framePhase0;
}

size_t Filament::StateBytesForTest() const
{
	size_t bytes = 0;
	auto count = [ &bytes ]( const PassBuffer& b, size_t perTexel ) {
		if( b.IsValid() )
			bytes += static_cast< size_t >( b.GetWidth() ) * b.GetHeight() * perTexel;
	};
	count( state[ 0 ], 16 );
	count( state[ 1 ], 16 );
	count( drive, 16 );
	count( light, 16 );
	count( bloomX, 8 );
	count( bloomY, 8 );
	count( ramp[ 0 ], 16 );
	count( ramp[ 1 ], 16 );
	return bytes;
}
