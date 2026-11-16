#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace earshot
{
class Engine;
namespace testing
{
class CaptureBackend;
}
} // namespace earshot

/**
	The colour organ: a 1970s three-channel light box in front of the wall.

	The music is split by a passive three-way crossover (Linkwitz-Riley,
	second order) into bass, mid and treble. Each band is half-wave rectified
	and averaged by a diode-RC detector (ETI Project 428, 1974), and the
	detector's voltage sets the current of a constant-current source charging
	a ramp capacitor that a programmable unijunction fires a triac from (ETI
	428; Lancaster's Psychedelia I, 1969). The ramp is reset at every mains
	zero crossing, so the triac's firing angle is where the integral of the
	control current over the half-cycle reaches the threshold, and the lamp
	conducts from there to the next zero crossing. Every bulb of the wall
	holds three filaments, one per channel, each behind a LEE gel; the clip's
	cell sets how much of each channel's control current reaches that bulb.
	The filament's own thermal model (Lamp.h) does the rest. Sources:
	~/Projects/resolume/specs/SPEC-filament-organ.md.

	**Who does what.** This file and Organ.cpp are the only code that knows
	earshot exists. The CPU runs the audio in double at the tap rate and
	hands the GPU, per frame, the integral of each channel's control over
	every 1/16 of every mains half-cycle the frame touches (`Rows`). The
	organ's thermal pass (Shaders.cpp, `OrganThermal`) searches that for
	each bulb's firing point and integrates the lamp through the exact
	energy of the phase-cut sine in each RK4 substep.

	**Organ off touches nothing.** The constructor builds the menu from
	entries that need no enumeration. The earshot Engine is built, and the
	devices listed, on the first frame the organ is on (needle's VFD rule).
*/
namespace filament::organ
{

//---------------------------------------------------------------------------
// The circuit, as numbers. Each is sourced in SPEC-filament-organ.md.
//---------------------------------------------------------------------------

/// earshot's waveform tap rate: flat within 0.001 dB to a quarter of it
/// (earshot's Tap.h), so to 8 kHz, the highest corner.
constexpr int kTapRate = 32000;

/// The control integral is handed to the GPU at this many points per mains
/// half-cycle; the slope over each 1/16 is the control's mean over the
/// previous 1/16 (so a firing is never revised by audio that arrives later).
constexpr int kIntervals = 16;
constexpr int kRowPoints = kIntervals + 1;
/// Most half-cycles one frame can touch: kMaxFrameDelta x 120 + 2 is 32.
constexpr int kMaxRows = 64;

/// Linkwitz-Riley second order: two first-order Butterworth sections, a
/// biquad of Q 0.5 (Linkwitz; RaneNote 160). -6.02 dB at the corner.
constexpr double kQ = 0.5;

/// The DC block in front of the crossover. earshot's tap passes DC exactly
/// and DC would light the bass lamp; a period organ took its input through a
/// transformer (ETI 428) or coupling capacitors (Psychedelia I), neither of
/// which states a corner. An assumption.
constexpr double kDcBlockHertz = 10.0;

/// The detector, ETI 428 (Fig. 1, parts list p.79): a 1N914 half-wave
/// rectifier, R16 10k into C12 (1.0 uF red, 0.47 uF green and blue), R19
/// 100k discharging it. The diode is ideal: ETI's pre-bias holds it at its
/// threshold at zero signal.
constexpr double kChargeOhms        = 10.0e3;
constexpr double kDischargeOhms     = 100.0e3;
constexpr double kDetectorFarads[ 3 ] = { 1.0e-6, 0.47e-6, 0.47e-6 };

/// The audio is laid onto the host's clock through a FIFO held this full,
/// so a device's blocks arriving between frames never starve one.
constexpr double kReserveSeconds = 0.030;
/// More than this above the reserve (a device clock faster than the
/// host's, or the 4x scripted backend) and the oldest samples are dropped.
constexpr double kExcessSeconds = 0.120;

//---------------------------------------------------------------------------
// The controls. Host parameters are 0..1; these are what they mean.
//---------------------------------------------------------------------------
constexpr double kLowMinHertz  = 40.0;
constexpr double kLowMaxHertz  = 500.0;
constexpr double kHighMinHertz = 500.0;
constexpr double kHighMaxHertz = 8000.0;
constexpr double kGainMinDb    = -12.0;
constexpr double kGainMaxDb    = 60.0;

double CrossoverLowHertz( float value );  ///< geometric
float CrossoverLowParam( double hertz );
double CrossoverHighHertz( float value ); ///< geometric
float CrossoverHighParam( double hertz );
double GainDb( float value );             ///< linear in dB
float GainParam( double db );

int GelCount();
const char* GelName( int index );

constexpr int kSwitchCount = 2;
const char* SwitchName( int index ); ///< Off, On

/// The organ's channels, in the order of everything indexed by channel.
enum Channel : int
{
	kBass   = 0,
	kMid    = 1,
	kTreble = 2,
};

/// Negative-control bits for the organ, in the plugin's Perturb mask above
/// lamp::Perturb's. Always 0 in the plugin.
enum Perturb : int
{
	kPerturbNoPrewarp   = 64,  ///< the bilinear transform unwarped: --crossover must fail
	kPerturbButterworth = 128, ///< Q 0.707 for 0.5: --crossover must fail
	kPerturbLinearPower = 256, ///< conduction energy (1 - u) of full, not the sine's: --firing must fail
	kPerturbFrameSlope  = 512, ///< the control's slope from the frame's audio, not the last 1/16's: --organframes must fail
	kPerturbFloatClock  = 1024,///< the half-cycle grid from a float clock: --organclock must fail
	kPerturbNoReset     = 2048,///< the ramp never reset at the zero crossing: --firing and --organstep must fail
	kPerturbGelRotated  = 4096,///< each filament lit through the next channel's gel: --organcolour must fail
};

struct Settings
{
	double lowHertz  = 200.0;
	double highHertz = 2000.0;
	double gainDb[ 3 ] = { 0.0, 0.0, 0.0 };
	int    perturb   = 0;
};

//---------------------------------------------------------------------------
// The device menu (woofer's and needle's shape).
//---------------------------------------------------------------------------
constexpr int kInputSlots = 16;

struct Slot
{
	std::string label;///< what the menu shows
	std::string name; ///< what selectDevice is handed ("" for none)
	float       value = 0.0f;
};

enum class Lamp : int
{
	Idle = 0, ///< no input chosen, or the device is opening: nothing to listen to
	Green,    ///< a device delivers
	Red,      ///< open and bit-exactly silent
	RedBlink, ///< chosen but missing, or the backend refused it
};

//---------------------------------------------------------------------------
class Organ
{
public:
	using BackendFactory = std::unique_ptr< earshot::testing::CaptureBackend > ( * )();

	/// For the harness: every Engine built afterwards uses this backend and
	/// lists ITS devices. Set before any plugin exists; never by the plugin.
	static void SetBackendFactoryForTest( BackendFactory factory );

	/// Every call that can start CoreAudio, counted process-wide.
	struct Counters
	{
		std::atomic< int > engines{ 0 };///< earshot::Engine constructed
		std::atomic< int > lists{ 0 };  ///< devices enumerated
		std::atomic< int > selects{ 0 };///< selectDevice called
	};
	static Counters& Calls();

	/// The harness's negative controls for the device code, always false in
	/// the plugin: list the devices in the constructor (--coreaudio must
	/// fail), or value the menu by element index instead of by device name
	/// (--organdevices must fail).
	static bool negListAtConstruction;
	static bool negMenuByIndex;

	Organ();
	~Organ();

	const std::vector< Slot >& Slots() const { return slots; }
	/// Bumps every time the menu is rebuilt, so the plugin knows to tell the host.
	int ListCount() const { return listCount; }
	bool Active() const { return active; }

	/// The Audio Input value the host stored: remembered, acted on only
	/// while the organ is on.
	void Want( float key );
	/// Every organ frame (cheap when nothing changed): build the Engine,
	/// list the devices once, apply the wanted device.
	void Activate();
	/// Organ off: close any device it opened, forget the audio.
	void Deactivate();
	/// Rescan Inputs: list again and ask for the wanted device again. Does
	/// nothing (and touches nothing) unless the organ is on.
	bool Rescan();

	/**
		One frame: every tap sample for host time [start, end) through the
		crossover and detectors, then the control integral for half-cycles
		firstHalf .. firstHalf + rows - 1 of mains at `hertz` (absolute
		half-cycle numbers: floor( 2 x hertz x t )).
	*/
	void Advance( double start, double end, double hertz, int64_t firstHalf, int rows, const Settings& settings );

	/// rows x kRowPoints texels, RGBA: the integral, from the half-cycle's
	/// start, of gain x detector voltage per channel (rgb), in units where a
	/// steady control of 1 fires at 90 degrees.
	const float* Rows() const { return rowData.data(); }
	int RowCount() const { return rowCount; }
	/// The absolute half-cycle number of row 0 of the last Advance.
	int64_t FirstHalf() const { return rowFirstHalf; }

	Lamp LampState() const;

	//-- harness ----------------------------------------------------------
	earshot::Engine* EngineForTest() { return engine.get(); }
	void FeedBegin( int sampleRate, int channels );
	void Feed( const float* interleaved, int frames );
	/// Keep the crossover's input and outputs of the next Advance calls.
	void RecordForTest( bool on );
	/// Every row a steady control of `control` on every channel (gain and
	/// audio ignored): a channel with b = 1 fires at 1 / ( 2 control ) of the
	/// half-cycle. 0 or less: the audio again.
	void ForceControlForTest( double control ) { forced = control; }
	/// What the last Advance consumed: [0] the crossover's input, [1..3] its
	/// bass, mid and treble outputs, [4..6] the three detectors.
	const std::vector< double >& RecordedForTest( int which ) const { return recorded[ which ]; }
	int TapRateForTest() const { return rate; }
	/// Samples the FIFO padded with zeros, and dropped, since the organ came on.
	uint64_t UnderrunsForTest() const { return underruns; }
	uint64_t DroppedForTest() const { return dropped; }

private:
	void BuildEngine();
	void List();
	void Apply();
	void ResetStream();
	void Design( const Settings& s );
	void Push( const float* left, const float* right, int n );
	void Process( double sample, double time );
	void Close( int64_t interval );
	void ResetGrid();

	struct Biquad
	{
		double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;
		double z1 = 0.0, z2 = 0.0;
		double Run( double x );
	};

	std::unique_ptr< earshot::Engine > engine;
	std::vector< Slot >                slots;
	std::map< float, std::string >     seen;///< every device key met, and its name
	float wanted    = 0.0f;
	float applied   = 0.0f;
	bool  active    = false;
	bool  listed    = false;
	int   listCount = 0;
	bool  fromDevice = false;
	uint32_t generation = 0;

	//-- the stream on the host's clock --
	int      rate      = 0;     ///< the tap's actual rate
	bool     anchored  = false;
	double   anchor    = 0.0;   ///< host time of sample 0
	uint64_t consumed  = 0;     ///< samples taken from the FIFO since the anchor
	std::vector< float > fifo;  ///< a ring of mono samples
	size_t   fifoHead  = 0, fifoCount = 0;
	std::vector< float > tapLeft, tapRight;
	uint64_t underruns = 0, dropped = 0;

	//-- the circuit --
	double designedLow = -1.0, designedHigh = -1.0;
	int    designedRate = 0, designedPerturb = -1;
	double dcB0 = 1.0, dcP = 0.0, dcX = 0.0, dcY = 0.0;
	Biquad low, midHigh, midLow, high;
	double chargeTo[ 3 ]   = {};///< the fraction of the input the node charges to
	double chargeKeep[ 3 ] = {};///< exp( -dt / tau ) charging
	double leakKeep[ 3 ]   = {};///< exp( -dt / tau ) discharging
	double detector[ 3 ]   = {};

	//-- the half-cycle grid --
	double  gridHertz  = 0.0;
	int64_t openInterval = INT64_MIN;
	double  openSum[ 3 ] = {};
	int     openCount  = 0;
	double  lastMean[ 3 ] = {};
	static constexpr int kRing = 4096;
	std::vector< double >  ringMean;  ///< kRing x 3, the control's mean per 1/16
	std::vector< int64_t > ringIndex; ///< which interval each slot holds
	int64_t closedThrough = INT64_MIN;///< the last interval whose mean is known

	std::vector< float > rowData;
	int rowCount = 0;
	int64_t rowFirstHalf = 0;

	bool recording = false;
	std::vector< double > recorded[ 7 ];
	int perturb = 0;
	double forced = 0.0;
};

} // namespace filament::organ
