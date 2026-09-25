#pragma once

#include "Lamp.h"
#include "Model.h"
#include "PassBuffer.h"
#include "StoatworksAboutParams.h"

#include <FFGLSDK.h>

#include <string>

/**
	Filament -- the picture on a wall of incandescent bulbs, as an FFGL effect.

	**The one idea.** A bulb is a tungsten wire heated by current, not an LED
	with a slow fade. Its light is its temperature, and its temperature
	follows C dT/dt = V^2 / R( T ) - e s A ( T^4 - T0^4 ) - k ( T - T0 ). Point
	a matrix of them at the clip, each bulb's dimmer set by its cell, and the
	warm fades, the lag, the inrush and the mains flicker all fall out.

	**Six passes**, in `Shaders.h`, five of them at one texel per bulb. The
	ODE runs on the GPU in RK4 substeps sized to the mains waveform and the
	lamp's stiffness (Lamp.h). Time is frame-relative: the frame's seconds and
	the mains phase at its start are reduced in double here, and nothing
	absolute crosses into GLSL. The temperatures live at the grid's raster,
	so an output resize never touches them, and a grid change carries them
	across. See AGENTS.md for the traps.
*/
class Filament : public CFFGLPlugin
{
public:
	Filament();

	//CFFGLPlugin
	FFResult InitGL( const FFGLViewportStruct* vp ) override;
	FFResult ProcessOpenGL( ProcessOpenGLStruct* pGL ) override;
	FFResult DeInitGL() override;

	FFResult SetFloatParameter( unsigned int index, float value ) override;
	float GetFloatParameter( unsigned int index ) override;
	FFResult SetTime( double time ) override;

	char* GetTextParameter( unsigned int index ) override;

	/// Declared only so the About line can accept its own default.
	/// instantiateGL pushes every declared default back through the setters
	/// and deletes the whole instance if one fails, and CFFGLPlugin's
	/// SetTextParameter is a stub that returns exactly that failure.
	FFResult SetTextParameter( unsigned int index, const char* value ) override;

	/// Clock test hook: the harness DECLARES its unit rather than leaving the
	/// voting to infer one.
	void SetClockScaleForTest( double scale );

	/// Negative-control hooks, a bitmask of `lamp::Perturb`. Always 0 in the
	/// plugin; each bit perturbs the model so a check can be shown to fail.
	void SetPerturbForTest( int bits );

	/// Probe hook: `model::Probe`. The output pass writes raw floats.
	void SetProbeForTest( int probe );

	/// What the last frame integrated: the seconds, the substeps, and the
	/// mains phase (in cycles, fractional) at its start. The harness's
	/// reference integration runs the same schedule in double.
	void LastFrameForTest( double& seconds, int& substeps, double& phase0 ) const;

	/// Bytes of GPU state held across frames: the temperatures (two, ping-
	/// ponged), the drive, the light and the bloom, colour textures only.
	size_t StateBytesForTest() const;

	/// Everything the operator can reach, in the order Resolume shows them.
	enum ParamID : FFUInt32
	{
		//Wall
		PT_COLUMNS,
		PT_ROWS,
		PT_GAP,
		PT_PIXEL_MODE,
		PT_GLASS,
		PT_GELS,

		//Bulb
		PT_WATTAGE,
		PT_DIMMER,
		PT_MAINS,
		PT_AMBIENT,

		//Look
		PT_GLOW,
		PT_BLOOM,
		PT_EXPOSURE,
		PT_MIX,

		//About. FFGL has no window, so the name, the version and the links are
		//parameters the host draws. Last, so no saved composition's ids shift.
		PT_ABOUT_FIRST,
		PT_COUNT = PT_ABOUT_FIRST + stoatworks::about::kParamCount
	};

private:
	/// The host's clock in seconds, whatever unit it arrived in.
	double nowSeconds();

	void setFilamentUniforms( ffglex::FFGLShader& shader, int law, double ambient );

	ffglex::FFGLShader driveShader;
	ffglex::FFGLShader thermalShader;
	ffglex::FFGLShader fillShader;
	ffglex::FFGLShader regridShader;
	ffglex::FFGLShader blurShader;
	ffglex::FFGLShader outputShader;
	ffglex::FFGLScreenQuad quad;

	filament::PassBuffer state[ 2 ]; ///< temperatures, ping-ponged
	int current = 0;                 ///< which of `state` holds the latest
	filament::PassBuffer drive;
	filament::PassBuffer light;      ///< shutter-averaged light per bulb
	filament::PassBuffer bloomX;
	filament::PassBuffer bloomY;
	GLuint planckTexture = 0;

	filament::lamp::Lamp lamp;
	std::string lampKey;
	double stiffness = 0.0;

	int lastWidth  = 0;
	int lastHeight = 0;

	double frameSeconds = 0.0;
	int frameSubsteps   = 0;
	double framePhase0  = 0.0;

	//--- the clock (readout's unit voting) -----------------------------------
	bool hostTimeSeen   = false;
	double clockScale   = 0.0;///< 0 until decided; then 1.0 or 0.001
	double wallStart    = -1.0;
	double lastWallTime = -1.0;
	double lastRawTime  = -1.0;
	int secondsVotes    = 0;
	int millisVotes     = 0;
	double lastNow      = -1.0;
	int clockFrames     = 0;

	int perturb = 0;
	int probe   = 0;

	/// Zero-initialised: the About block's ids are never stored to, so
	/// without this GetFloatParameter hands the host whatever was on the
	/// stack for them.
	float params[ PT_COUNT ] = {};

	/// GetTextParameter hands the host a bare pointer, so the string has to
	/// outlive the call.
	std::string aboutText;
};
