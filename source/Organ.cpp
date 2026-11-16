#include "Organ.h"

#include "Diag.h"
#include "GelTable.h"

#include <earshot/earshot.h>
#include <earshot/testing.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace filament::organ
{
namespace
{
constexpr double kPi = 3.14159265358979323846;

Organ::BackendFactory g_factory = nullptr;

/// FILAMENT_NO_DEVICES=1: never ask the OS for its devices and never hand
/// earshot a device name (earshot's own lookup starts CoreAudio even for a
/// name it then fails to match). verify.sh sets it for oxbow.
bool DevicesDisabled()
{
	const char* v = std::getenv( "FILAMENT_NO_DEVICES" );
	return v != nullptr && *v != '\0' && std::strcmp( v, "0" ) != 0;
}

constexpr const char* kNoneLabel    = "None";
constexpr const char* kDefaultLabel = "System default";
constexpr const char* kEmptySlot    = "(none)";

/// The ring of mono samples between the tap and the host's clock: 8 s.
constexpr size_t kFifoCapacity = 1u << 18;
/// Most samples one readTap call hands over.
constexpr int kTapChunk = 4096;
/// Intervals filled with the last known control when a stretch of the grid
/// had no samples (a clock jump): no more than a few half-cycles' worth.
constexpr int kMaxGapFill = 4 * kIntervals;

float clamp01( float v )
{
	return std::clamp( v, 0.0f, 1.0f );
}
} // namespace

//---------------------------------------------------------------------------
// Controls
//---------------------------------------------------------------------------
double CrossoverLowHertz( float value )
{
	return kLowMinHertz * std::pow( kLowMaxHertz / kLowMinHertz, static_cast< double >( clamp01( value ) ) );
}

float CrossoverLowParam( double hertz )
{
	return clamp01( static_cast< float >( std::log( hertz / kLowMinHertz ) / std::log( kLowMaxHertz / kLowMinHertz ) ) );
}

double CrossoverHighHertz( float value )
{
	return kHighMinHertz * std::pow( kHighMaxHertz / kHighMinHertz, static_cast< double >( clamp01( value ) ) );
}

float CrossoverHighParam( double hertz )
{
	return clamp01( static_cast< float >( std::log( hertz / kHighMinHertz ) / std::log( kHighMaxHertz / kHighMinHertz ) ) );
}

double GainDb( float value )
{
	return kGainMinDb + ( kGainMaxDb - kGainMinDb ) * static_cast< double >( clamp01( value ) );
}

float GainParam( double db )
{
	return clamp01( static_cast< float >( ( db - kGainMinDb ) / ( kGainMaxDb - kGainMinDb ) ) );
}

int GelCount()
{
	return gels::kGelCount;
}

const char* GelName( int index )
{
	return gels::kNames[ std::clamp( index, 0, gels::kGelCount - 1 ) ];
}

const char* SwitchName( int index )
{
	static const char* const names[ kSwitchCount ] = { "Off", "On" };
	return names[ std::clamp( index, 0, kSwitchCount - 1 ) ];
}

//---------------------------------------------------------------------------
// Devices: needle's VFD Input, the same rules.
//---------------------------------------------------------------------------
void Organ::SetBackendFactoryForTest( BackendFactory factory )
{
	g_factory = factory;
}

Organ::Counters& Organ::Calls()
{
	static Counters c;
	return c;
}

bool Organ::negListAtConstruction = false;
bool Organ::negMenuByIndex        = false;

Organ::Organ() :
	ringMean( static_cast< size_t >( kRing ) * 3, 0.0 ), ringIndex( kRing, INT64_MIN )
{
	//The menu that needs no enumeration: None, System default, spares. The
	//devices arrive on the first frame the organ is on.
	slots.push_back( { kNoneLabel, "", 0.0f } );
	slots.push_back( { kDefaultLabel, earshot::kDefaultDevice, earshot::deviceKey( earshot::kDefaultDevice ) } );
	while( static_cast< int >( slots.size() ) < kInputSlots )
		slots.push_back( { kEmptySlot, "", 0.0f } );
	rowData.assign( static_cast< size_t >( kMaxRows ) * kRowPoints * 4, 0.0f );
	if( negListAtConstruction )
		List();//the negative control: what organ off must never do
}

Organ::~Organ() = default;

void Organ::BuildEngine()
{
	if( engine )
		return;
	Calls().engines.fetch_add( 1 );
	earshot::Config c;
	c.maxChannels = 2;
	c.tapRate     = kTapRate;
	if( g_factory != nullptr )
		engine = std::make_unique< earshot::Engine >( c, g_factory() );
	else
		engine = std::make_unique< earshot::Engine >( c );
	if( !engine->ok() )
		diag::error( "earshot could not allocate: the organ will stay dark" );
	fifo.assign( kFifoCapacity, 0.0f );
	tapLeft.assign( kTapChunk, 0.0f );
	tapRight.assign( kTapChunk, 0.0f );
}

void Organ::List()
{
	Calls().lists.fetch_add( 1 );
	std::vector< earshot::DeviceInfo > devices;
	if( g_factory != nullptr )
	{
		//A throwaway backend of the harness's kind: it lists, it opens nothing.
		if( auto lister = g_factory() )
			devices = lister->enumerate();
	}
	else if( !DevicesDisabled() )
		devices = earshot::listDevices();

	slots.clear();
	slots.push_back( { kNoneLabel, "", 0.0f } );
	slots.push_back( { kDefaultLabel, earshot::kDefaultDevice, earshot::deviceKey( earshot::kDefaultDevice ) } );
	for( const earshot::DeviceInfo& d : devices )
	{
		if( static_cast< int >( slots.size() ) >= kInputSlots )
			break;
		const float key = earshot::deviceKey( d.name.c_str() );
		seen[ key ]     = d.name;
		slots.push_back( { d.name.substr( 0, 30 ), d.name, negMenuByIndex ? static_cast< float >( slots.size() ) : key } );
	}
	while( static_cast< int >( slots.size() ) < kInputSlots )
		slots.push_back( { kEmptySlot, "", 0.0f } );
	listed = true;
	++listCount;
	diag::info( "organ audio inputs: " + std::to_string( devices.size() ) + " device(s) listed" );
}

void Organ::Want( float key )
{
	wanted = key;
	if( active )
		Apply();
}

void Organ::Activate()
{
	if( active )
		return;
	active = true;
	BuildEngine();
	if( !listed )
		List();
	Apply();
}

void Organ::Deactivate()
{
	if( !active )
		return;
	active = false;
	if( engine && applied != 0.0f )
	{
		Calls().selects.fetch_add( 1 );
		engine->selectDevice( nullptr );
		applied = 0.0f;
		diag::info( "organ audio input closed: the organ is off" );
	}
	ResetStream();
	generation = 0;
	fromDevice = false;
}

bool Organ::Rescan()
{
	if( !active )
		return false;
	List();
	//A remembered device that was missing may be back: ask again.
	if( wanted != 0.0f && engine )
	{
		Calls().selects.fetch_add( 1 );
		engine->selectDevice( nullptr );
		applied = 0.0f;
		Apply();
	}
	return true;
}

void Organ::Apply()
{
	if( !engine || wanted == applied )
		return;
	std::string name;
	if( wanted == 0.0f )
		name.clear();
	else if( wanted == earshot::deviceKey( earshot::kDefaultDevice ) )
		name = earshot::kDefaultDevice;
	else
	{
		//A key with no device met in this process: ask for a name that cannot
		//match, so earshot says Missing (never the default). Rescan resolves
		//it once the device is back.
		const auto known = seen.find( wanted );
		name = known != seen.end() ? known->second : "(device " + std::to_string( static_cast< long >( wanted ) ) + " not present)";
	}
	applied = wanted;
	if( !name.empty() && DevicesDisabled() )
	{
		diag::info( "organ audio input refused (FILAMENT_NO_DEVICES): " + name );
		return;
	}
	Calls().selects.fetch_add( 1 );
	engine->selectDevice( name.c_str() );
	diag::info( "organ audio input: " + ( name.empty() ? std::string( "none" ) : name ) );
}

Lamp Organ::LampState() const
{
	if( !engine )
		return Lamp::Idle;
	switch( engine->captureState() )
	{
	case earshot::CaptureState::Silent: return Lamp::Red;
	case earshot::CaptureState::Missing:
	case earshot::CaptureState::Failed: return Lamp::RedBlink;
	default: break;
	}
	return fromDevice ? Lamp::Green : Lamp::Idle;
}

void Organ::FeedBegin( int sampleRate, int channels )
{
	BuildEngine();
	engine->feedBegin( sampleRate, channels );
}

void Organ::Feed( const float* interleaved, int frames )
{
	if( engine )
		engine->feed( interleaved, frames );
}

void Organ::RecordForTest( bool on )
{
	recording = on;
}

//---------------------------------------------------------------------------
// The circuit
//---------------------------------------------------------------------------
double Organ::Biquad::Run( double x )
{
	//Transposed direct form II, in double.
	const double y = b0 * x + z1;
	z1             = b1 * x - a1 * y + z2;
	z2             = b2 * x - a2 * y;
	return y;
}

void Organ::Design( const Settings& s )
{
	if( s.lowHertz == designedLow && s.highHertz == designedHigh && rate == designedRate && s.perturb == designedPerturb )
		return;
	designedLow     = s.lowHertz;
	designedHigh    = s.highHertz;
	designedRate    = rate;
	designedPerturb = s.perturb;
	const double fs = static_cast< double >( rate );
	const double Q  = ( s.perturb & kPerturbButterworth ) != 0 ? 1.0 / std::sqrt( 2.0 ) : kQ;

	//The RBJ cookbook's LPF and HPF (W3C Audio EQ Cookbook): the bilinear
	//transform with the corner prewarped, so the digital corner is exactly
	//the analogue one. Coefficients change; the state carries on.
	auto design = [ & ]( Biquad& f, double hertz, bool highPass ) {
		double w0 = 2.0 * kPi * hertz / fs;
		if( ( s.perturb & kPerturbNoPrewarp ) != 0 )
			w0 = 2.0 * std::atan( kPi * hertz / fs );//the analogue corner taken through the bilinear map unwarped
		const double c = std::cos( w0 ), alpha = std::sin( w0 ) / ( 2.0 * Q );
		const double a0 = 1.0 + alpha;
		if( highPass )
		{
			f.b0 = ( 1.0 + c ) / 2.0 / a0;
			f.b1 = -( 1.0 + c ) / a0;
			f.b2 = ( 1.0 + c ) / 2.0 / a0;
		}
		else
		{
			f.b0 = ( 1.0 - c ) / 2.0 / a0;
			f.b1 = ( 1.0 - c ) / a0;
			f.b2 = ( 1.0 - c ) / 2.0 / a0;
		}
		f.a1 = -2.0 * c / a0;
		f.a2 = ( 1.0 - alpha ) / a0;
	};
	design( low, s.lowHertz, false );
	design( midHigh, s.lowHertz, true );
	design( midLow, s.highHertz, false );
	design( high, s.highHertz, true );

	//The DC block: one pole, bilinear, prewarped.
	const double K = std::tan( kPi * kDcBlockHertz / fs );
	dcB0           = 1.0 / ( 1.0 + K );
	dcP            = ( 1.0 - K ) / ( 1.0 + K );

	//The detector: through R16 (charging) while the diode conducts, R19
	//(discharging) always. Exact for an input held over one sample.
	const double dt = 1.0 / fs;
	for( int c = 0; c < 3; ++c )
	{
		const double C        = kDetectorFarads[ c ];
		const double parallel = kChargeOhms * kDischargeOhms / ( kChargeOhms + kDischargeOhms );
		chargeTo[ c ]         = kDischargeOhms / ( kChargeOhms + kDischargeOhms );
		chargeKeep[ c ]       = std::exp( -dt / ( parallel * C ) );
		leakKeep[ c ]         = std::exp( -dt / ( kDischargeOhms * C ) );
	}
}

void Organ::ResetStream()
{
	anchored  = false;
	consumed  = 0;
	fifoHead  = 0;
	fifoCount = 0;
	dcX = dcY = 0.0;
	for( Biquad* f : { &low, &midHigh, &midLow, &high } )
		f->z1 = f->z2 = 0.0;
	std::fill( std::begin( detector ), std::end( detector ), 0.0 );
	ResetGrid();
}

void Organ::ResetGrid()
{
	std::fill( ringIndex.begin(), ringIndex.end(), INT64_MIN );
	closedThrough = INT64_MIN;
	openInterval  = INT64_MIN;
	openCount     = 0;
	std::fill( std::begin( openSum ), std::end( openSum ), 0.0 );
	std::fill( std::begin( lastMean ), std::end( lastMean ), 0.0 );
}

void Organ::Push( const float* left, const float* right, int n )
{
	for( int k = 0; k < n; ++k )
	{
		if( fifoCount == kFifoCapacity )
		{
			fifoHead = ( fifoHead + 1 ) % kFifoCapacity;
			--fifoCount;
			++dropped;
		}
		const float mono                                       = left == nullptr ? 0.0f : 0.5f * ( left[ k ] + right[ k ] );
		fifo[ ( fifoHead + fifoCount ) % kFifoCapacity ] = mono;
		++fifoCount;
	}
}

void Organ::Close( int64_t interval )
{
	double mean[ 3 ];
	for( int c = 0; c < 3; ++c )
		mean[ c ] = interval == openInterval && openCount > 0 ? openSum[ c ] / openCount : lastMean[ c ];
	const size_t slot = static_cast< size_t >( ( ( interval % kRing ) + kRing ) % kRing );
	for( int c = 0; c < 3; ++c )
	{
		ringMean[ slot * 3 + static_cast< size_t >( c ) ] = mean[ c ];
		lastMean[ c ]                                     = mean[ c ];
	}
	ringIndex[ slot ] = interval;
	closedThrough     = interval;
	if( interval == openInterval )
	{
		openInterval = INT64_MIN;
		openCount    = 0;
		std::fill( std::begin( openSum ), std::end( openSum ), 0.0 );
	}
}

void Organ::Process( double x, double time )
{
	//The crossover: a DC block, then Linkwitz-Riley 2 low, mid (high-pass at
	//the low corner, low-pass at the high one) and high.
	const double y = dcB0 * ( x - dcX ) + dcP * dcY;
	dcX            = x;
	dcY            = y;
	const double band[ 3 ] = { low.Run( y ), midLow.Run( midHigh.Run( y ) ), high.Run( y ) };

	//The detectors: a half-wave rectifier on the negative swing (ETI's D4
	//conducts as the filter output goes negative), into the RC node.
	for( int c = 0; c < 3; ++c )
	{
		const double r = std::max( -band[ c ], 0.0 );
		if( r > detector[ c ] )
		{
			const double target = chargeTo[ c ] * r;
			detector[ c ]       = target + ( detector[ c ] - target ) * chargeKeep[ c ];
		}
		else
			detector[ c ] *= leakKeep[ c ];
	}

	if( recording )
	{
		recorded[ 0 ].push_back( y );
		for( int c = 0; c < 3; ++c )
		{
			recorded[ 1 + c ].push_back( band[ c ] );
			recorded[ 4 + c ].push_back( detector[ c ] );
		}
	}

	//Onto the grid: 1/16 of a mains half-cycle per interval, numbered from
	//the host clock's zero.
	const double perInterval = 2.0 * kIntervals * gridHertz;
	const int64_t interval   = ( perturb & kPerturbFloatClock ) != 0
	                               ? static_cast< int64_t >( std::floor( static_cast< double >( static_cast< float >( time ) ) * perInterval ) )
	                               : static_cast< int64_t >( std::floor( time * perInterval ) );
	if( interval != openInterval )
	{
		//Everything before this sample's interval is complete. A few empty
		//intervals take the last control; a long gap stays unknown (zero).
		if( closedThrough != INT64_MIN && interval - 1 > closedThrough && interval - 1 - closedThrough <= kMaxGapFill )
			for( int64_t j = closedThrough + 1; j < interval; ++j )
				Close( j );
		else if( openInterval != INT64_MIN )
			Close( openInterval );
		openInterval = interval;
		openCount    = 0;
		std::fill( std::begin( openSum ), std::end( openSum ), 0.0 );
	}
	for( int c = 0; c < 3; ++c )
		openSum[ c ] += detector[ c ];
	++openCount;
}

//---------------------------------------------------------------------------
void Organ::Advance( double start, double end, double hertz, int64_t firstHalf, int rows, const Settings& s )
{
	perturb = s.perturb;

	//The grid is in half-cycles of this mains: a new frequency starts it again.
	if( hertz != gridHertz )
	{
		gridHertz = hertz;
		ResetGrid();
	}

	//earshot: the frame's analysis, and the tap's new samples.
	bool fresh = false;
	if( engine && engine->ok() )
	{
		const earshot::Frame& f = engine->update( end, nullptr, 0 );
		fromDevice              = f.source == earshot::Source::Device;
		if( f.generation != generation )
		{
			generation = f.generation;
			ResetStream();
		}
		const int tap = engine->tapRate();
		if( tap > 0 && tap != rate )
		{
			rate = tap;
			ResetStream();
		}
		fresh = fromDevice;
	}
	if( rate <= 0 )
		rate = kTapRate;
	Design( s );

	//Laid onto the host's clock: sample n of the stream is at anchor + n / rate,
	//behind kReserveSeconds of zeros so a device's blocks never starve a frame.
	auto preroll = [ & ]() {
		std::vector< float > zeros( static_cast< size_t >( std::lround( kReserveSeconds * rate ) ), 0.0f );
		//Ahead of anything already queued.
		std::vector< float > queued;
		queued.reserve( fifoCount );
		for( size_t k = 0; k < fifoCount; ++k )
			queued.push_back( fifo[ ( fifoHead + k ) % kFifoCapacity ] );
		fifoHead  = 0;
		fifoCount = 0;
		if( !fifo.empty() )
		{
			Push( zeros.data(), zeros.data(), static_cast< int >( zeros.size() ) );
			Push( queued.data(), queued.data(), static_cast< int >( queued.size() ) );
		}
	};
	if( !anchored )
	{
		anchored = true;
		anchor   = start;
		consumed = 0;
		preroll();
	}
	else
	{
		//A host clock that jumped, paused or ran backwards: the stream carries
		//on from this frame's start.
		const double next = anchor + static_cast< double >( consumed ) / rate;
		if( std::fabs( start - next ) > 2.0 / rate )
		{
			anchor = start - static_cast< double >( consumed ) / rate;
			ResetGrid();
		}
	}
	if( fresh && !fifo.empty() )
	{
		for( ;; )
		{
			const int n = engine->readTap( tapLeft.data(), tapRight.data(), kTapChunk );
			if( n <= 0 )
				break;
			Push( tapLeft.data(), tapRight.data(), n );
			if( n < kTapChunk )
				break;
		}
	}

	//Every sample whose time is before the frame's end, once.
	const double due     = std::ceil( ( end - anchor ) * rate - 1e-6 );
	const uint64_t target = due > 0.0 ? static_cast< uint64_t >( due ) : 0;
	const uint64_t want   = target > consumed ? target - consumed : 0;
	double frameSum[ 3 ]  = {};
	bool short_           = false;
	if( recording )
		for( auto& r : recorded )
			r.clear();
	for( uint64_t k = 0; k < want; ++k )
	{
		double x = 0.0;
		if( fifoCount > 0 )
		{
			x        = fifo[ fifoHead ];
			fifoHead = ( fifoHead + 1 ) % kFifoCapacity;
			--fifoCount;
		}
		else
		{
			++underruns;
			short_ = true;
		}
		Process( x, anchor + static_cast< double >( consumed + k ) / rate );
		for( int c = 0; c < 3; ++c )
			frameSum[ c ] += detector[ c ];
	}
	consumed = std::max( consumed, target );
	if( short_ && fromDevice )
		preroll();//re-establish the reserve
	const size_t cap = static_cast< size_t >( ( kReserveSeconds + kExcessSeconds ) * rate );
	if( fifoCount > cap )
	{
		const size_t keep = static_cast< size_t >( std::lround( kReserveSeconds * rate ) );
		dropped += fifoCount - keep;
		fifoHead  = ( fifoHead + ( fifoCount - keep ) ) % kFifoCapacity;
		fifoCount = keep;
	}

	//Every interval that ended before the next sample is complete.
	const double nextTime   = anchor + static_cast< double >( consumed ) / rate;
	const int64_t lastWhole = static_cast< int64_t >( std::floor( nextTime * 2.0 * kIntervals * gridHertz ) ) - 1;
	if( closedThrough == INT64_MIN && openInterval != INT64_MIN && lastWhole >= openInterval )
		Close( openInterval );
	if( closedThrough != INT64_MIN && lastWhole > closedThrough && lastWhole - closedThrough <= kMaxGapFill + kIntervals )
		for( int64_t j = closedThrough + 1; j <= lastWhole; ++j )
			Close( j );

	//The rows: the integral of gain x control from each half-cycle's start.
	//Over interval I the control is the mean over interval I - 1, which is
	//complete before interval I begins, so the integral up to any moment the
	//frame reaches never changes when more audio arrives.
	rowCount     = std::clamp( rows, 1, kMaxRows );
	rowFirstHalf = firstHalf;
	double gain[ 3 ];
	for( int c = 0; c < 3; ++c )
		gain[ c ] = std::pow( 10.0, s.gainDb[ c ] / 20.0 );
	const double frameMean[ 3 ] = { want > 0 ? frameSum[ 0 ] / static_cast< double >( want ) : 0.0,
		                            want > 0 ? frameSum[ 1 ] / static_cast< double >( want ) : 0.0,
		                            want > 0 ? frameSum[ 2 ] / static_cast< double >( want ) : 0.0 };
	for( int r = 0; r < rowCount; ++r )
	{
		const int64_t half = firstHalf + r;
		double D[ 3 ]      = { 0.0, 0.0, 0.0 };
		float* row         = &rowData[ static_cast< size_t >( r ) * kRowPoints * 4 ];
		row[ 0 ] = row[ 1 ] = row[ 2 ] = row[ 3 ] = 0.0f;
		for( int j = 0; j < kIntervals; ++j )
		{
			const int64_t before = half * kIntervals + j - 1;
			const size_t slot    = static_cast< size_t >( ( ( before % kRing ) + kRing ) % kRing );
			const bool known     = closedThrough != INT64_MIN && before <= closedThrough && ringIndex[ slot ] == before;
			for( int c = 0; c < 3; ++c )
			{
				double control = known ? ringMean[ slot * 3 + static_cast< size_t >( c ) ] : 0.0;
				if( ( s.perturb & kPerturbFrameSlope ) != 0 )
					control = frameMean[ c ];
				D[ c ] += forced > 0.0 ? forced / kIntervals : gain[ c ] * control / kIntervals;
			}
			float* p = row + ( j + 1 ) * 4;
			p[ 0 ]   = static_cast< float >( D[ 0 ] );
			p[ 1 ]   = static_cast< float >( D[ 1 ] );
			p[ 2 ]   = static_cast< float >( D[ 2 ] );
			p[ 3 ]   = 0.0f;
		}
	}
}

} // namespace filament::organ
