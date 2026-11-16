//===========================================================================
// The colour organ's checks (source/Organ.h). Included into main.cpp's
// anonymous namespace after the lamp's checks: Session, Knobs, report and
// the float bounds are shared.
//
// Audio reaches the plugin the way it reaches it in Resolume: through
// earshot. Every check that has music feeds it with earshot's feed() (the
// harness's synthetic track, a WAV, or a sine); the device checks use a
// scripted capture backend installed before any plugin exists, so no step
// here opens an audio device. --coreaudio is the one check that runs with
// the REAL backend, and it proves the organ off never reaches it.
//===========================================================================
namespace organ = filament::organ;

//---------------------------------------------------------------------------
// The scripted capture device (needle's): earshot's worker calls open() and
// close(), and a thread of ours delivers a 1 kHz sine, four times faster
// than real time. Nothing here talks to the OS.
//---------------------------------------------------------------------------
std::mutex g_deviceMutex;
std::vector< std::string > g_deviceNames = { "Test Mic", "Test Silent" };
std::atomic< int > g_opens{ 0 };

class ScriptedBackend final : public earshot::testing::CaptureBackend
{
public:
	~ScriptedBackend() override { close(); }

	earshot::testing::OpenResult open( const char* name, earshot::testing::AudioSink& sink, char* openedName, char* error ) noexcept override
	{
		close();
		std::string wanted = name;
		if( wanted == earshot::kDefaultDevice )
			wanted = "Test Mic";
		{
			std::lock_guard< std::mutex > lock( g_deviceMutex );
			if( std::find( g_deviceNames.begin(), g_deviceNames.end(), wanted ) == g_deviceNames.end() )
			{
				std::snprintf( error, 128, "%s is not connected (scripted)", wanted.c_str() );
				return earshot::testing::OpenResult::NotConnected;
			}
		}
		std::snprintf( openedName, 128, "%s", wanted.c_str() );
		++g_opens;
		sink.begin( 48000, 2 );
		running           = true;
		const bool silent = wanted == "Test Silent";
		worker            = std::thread( [ this, &sink, silent ] {
			std::vector< float > block( 2 * 480 );
			uint64_t n = 0;
			while( running )
			{
				for( int k = 0; k < 480; ++k, ++n )
				{
					const float v  = silent ? 0.0f : static_cast< float >( 0.25 * std::sin( 2.0 * lamp::kPi * 1000.0 * n / 48000.0 ) );
					block[ 2 * k ] = block[ 2 * k + 1 ] = v;
				}
				sink.deliver( block.data(), 480, 2 );
				std::this_thread::sleep_for( std::chrono::microseconds( 2500 ) );
			}
		} );
		return earshot::testing::OpenResult::Opened;
	}
	void close() noexcept override
	{
		running = false;
		if( worker.joinable() )
			worker.join();
	}
	std::vector< earshot::DeviceInfo > enumerate() noexcept override
	{
		std::lock_guard< std::mutex > lock( g_deviceMutex );
		std::vector< earshot::DeviceInfo > list;
		for( const std::string& name : g_deviceNames )
			list.push_back( { name, 2, false } );
		return list;
	}

private:
	std::atomic< bool > running{ false };
	std::thread worker;
};

std::unique_ptr< earshot::testing::CaptureBackend > scriptedFactory()
{
	return std::make_unique< ScriptedBackend >();
}

//---------------------------------------------------------------------------
// Harness-level negative controls (not plugin Perturb bits): planted in the
// device code through Organ's static test flags.
//---------------------------------------------------------------------------
constexpr int kNegListAtConstruction = 1 << 20;///< --coreaudio must fail
constexpr int kNegMenuByIndex        = 1 << 21;///< --organdevices must fail

//---------------------------------------------------------------------------
// The music.
//
// The synthetic track, 120 BPM at 48 kHz, every sample a pure function of
// its index: a kick on every beat (a 120 -> 50 Hz sweep, 180 ms decay), a
// 55 Hz bass note on every off-beat, an A major chord every two seconds, a
// closed hat (differenced noise, 25 ms) on every eighth. Kick and bass for
// the bass lamp, the chord for the mid, the hats for the treble, mixed so
// each band's 90th-percentile control is near the median of Resolume's six
// Beat demo clips (0.055, 0.035, 0.07 at 0 dB through the default bands).
//---------------------------------------------------------------------------
double noiseAt( int64_t n )
{
	uint64_t x = static_cast< uint64_t >( n ) * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull;
	x ^= x >> 31;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 29;
	return static_cast< double >( x >> 11 ) / 4503599627370496.0 - 1.0;
}

double trackAt( int64_t n, int rate )
{
	const double t  = static_cast< double >( n ) / rate;
	const double kb = std::fmod( t, 0.5 );
	const double kick = 0.225 * std::exp( -kb / 0.18 ) * std::sin( 2.0 * lamp::kPi * ( 50.0 * kb + 70.0 * 0.03 * ( 1.0 - std::exp( -kb / 0.03 ) ) ) );
	double bass = 0.0;
	if( t >= 0.25 )
		bass = 0.0875 * std::exp( -std::fmod( t - 0.25, 0.5 ) / 0.15 ) * std::sin( 2.0 * lamp::kPi * 55.0 * t );
	const double cb    = std::fmod( t, 2.0 );
	const double chord = 0.16 * std::exp( -cb / 0.4 ) * ( std::sin( 2.0 * lamp::kPi * 440.0 * t ) + std::sin( 2.0 * lamp::kPi * 554.37 * t ) + std::sin( 2.0 * lamp::kPi * 659.25 * t ) );
	const double hb    = std::fmod( t, 0.125 );
	const double hat   = 1.5 * std::exp( -hb / 0.025 ) * 0.5 * ( noiseAt( n ) - noiseAt( n - 1 ) );
	return 0.6 * ( kick + bass + chord + hat );
}

struct Wav
{
	int rate = 0, channels = 0;
	std::vector< float > samples;//interleaved
};

/// 16/24/32-bit PCM or 32-bit float; anything else is refused.
bool readWav( const std::string& path, Wav& wav, std::string& error )
{
	std::ifstream file( path, std::ios::binary );
	if( !file )
	{
		error = "cannot open " + path;
		return false;
	}
	std::vector< unsigned char > b( ( std::istreambuf_iterator< char >( file ) ), std::istreambuf_iterator< char >() );
	auto u16 = [ & ]( size_t i ) { return static_cast< uint32_t >( b[ i ] | ( b[ i + 1 ] << 8 ) ); };
	auto u32 = [ & ]( size_t i ) { return static_cast< uint32_t >( b[ i ] | ( b[ i + 1 ] << 8 ) | ( b[ i + 2 ] << 16 ) | ( static_cast< uint32_t >( b[ i + 3 ] ) << 24 ) ); };
	if( b.size() < 12 || std::memcmp( b.data(), "RIFF", 4 ) != 0 || std::memcmp( b.data() + 8, "WAVE", 4 ) != 0 )
	{
		error = path + " is not a RIFF WAVE file";
		return false;
	}
	int format = 0, bits = 0;
	for( size_t i = 12; i + 8 <= b.size(); )
	{
		const uint32_t size = u32( i + 4 );
		if( std::memcmp( b.data() + i, "fmt ", 4 ) == 0 && i + 8 + 16 <= b.size() )
		{
			format       = static_cast< int >( u16( i + 8 ) );
			wav.channels = static_cast< int >( u16( i + 10 ) );
			wav.rate     = static_cast< int >( u32( i + 12 ) );
			bits         = static_cast< int >( u16( i + 22 ) );
			if( format == 0xFFFE && size >= 26 )
				format = static_cast< int >( u16( i + 8 + 24 ) );
		}
		else if( std::memcmp( b.data() + i, "data", 4 ) == 0 )
		{
			const size_t end   = std::min( b.size(), i + 8 + size );
			const int bytes    = bits / 8;
			if( wav.channels <= 0 || bytes <= 0 || !( ( format == 1 && ( bits == 16 || bits == 24 || bits == 32 ) ) || ( format == 3 && bits == 32 ) ) )
			{
				error = path + ": only 16/24/32-bit PCM or 32-bit float";
				return false;
			}
			for( size_t p = i + 8; p + static_cast< size_t >( bytes ) <= end; p += static_cast< size_t >( bytes ) )
			{
				float v = 0.0f;
				if( format == 3 )
				{
					const uint32_t raw = u32( p );
					std::memcpy( &v, &raw, 4 );
				}
				else if( bits == 16 )
					v = static_cast< float >( static_cast< int16_t >( u16( p ) ) ) / 32768.0f;
				else if( bits == 24 )
					v = static_cast< float >( static_cast< int32_t >( ( b[ p ] << 8 ) | ( b[ p + 1 ] << 16 ) | ( static_cast< uint32_t >( b[ p + 2 ] ) << 24 ) ) >> 8 ) / 8388608.0f;
				else
					v = static_cast< float >( static_cast< int32_t >( u32( p ) ) ) / 2147483648.0f;
				wav.samples.push_back( v );
			}
		}
		i += 8 + size + ( size & 1u );
	}
	if( wav.samples.empty() || wav.rate <= 0 )
	{
		error = path + ": no audio found";
		return false;
	}
	return true;
}

/// How far ahead of the frame the harness feeds: well inside the organ's
/// 30 ms reserve, and over the tap's 12 samples.
constexpr double kFeedLead = 0.010;

/// What the harness feeds through earshot's feed(): one of the sources,
/// laid out from the first organ frame's start.
struct Audio
{
	enum class Kind
	{
		Track,
		Wav,
		Silent,
		Tone,
	};
	Kind kind     = Kind::Track;
	int rate      = 48000;
	int channels  = 2;
	Wav wav;
	double toneHertz = 1000.0;
	double toneAmp   = 0.5;
	int64_t toneStop = -1;///< the sample a Tone stops at; -1 never
	int64_t fed      = 0;
	bool begun       = false;
	double origin    = 0.0;///< host time of the audio's time 0
	bool wasOn       = false;

	float at( int64_t n, int channel ) const
	{
		switch( kind )
		{
		case Kind::Track: return static_cast< float >( trackAt( n, rate ) );
		case Kind::Silent: return 0.0f;
		case Kind::Tone:
			if( toneStop >= 0 && n >= toneStop )
				return 0.0f;
			return static_cast< float >( toneAmp * std::sin( 2.0 * lamp::kPi * toneHertz * static_cast< double >( n ) / rate ) );
		case Kind::Wav:
		{
			const int64_t frames = static_cast< int64_t >( wav.samples.size() ) / wav.channels;
			const int64_t i      = n % frames;//a short clip loops, as the demo pass did
			return wav.samples[ static_cast< size_t >( i * wav.channels + std::min( channel, wav.channels - 1 ) ) ];
		}
		}
		return 0.0f;
	}

	int rateNow() const { return kind == Kind::Wav ? wav.rate : rate; }

	/// What an organ frame of host time [start, end) needs: the audio up to
	/// its end plus kFeedLead. The audio's time 0 is the first organ frame's
	/// start; after the organ was off, it carries on where it stopped.
	void feedFrame( organ::Organ& o, double start, double end )
	{
		if( !wasOn )
			origin = start - static_cast< double >( fed ) / rateNow();
		wasOn = true;
		feedTo( o, end - origin + kFeedLead );
	}
	void off() { wasOn = false; }

	/// Feed everything up to `seconds` of audio.
	void feedTo( organ::Organ& o, double seconds )
	{
		if( kind == Kind::Wav )
		{
			rate     = wav.rate;
			channels = 2;
		}
		if( !begun )
		{
			o.FeedBegin( rate, channels );
			begun = true;
		}
		const int64_t target = static_cast< int64_t >( std::ceil( seconds * rate ) );
		std::vector< float > block;
		while( fed < target )
		{
			const int n = static_cast< int >( std::min< int64_t >( 4096, target - fed ) );
			block.resize( static_cast< size_t >( n ) * channels );
			for( int k = 0; k < n; ++k )
				for( int c = 0; c < channels; ++c )
					block[ static_cast< size_t >( k * channels + c ) ] = at( fed + k, c );
			o.Feed( block.data(), n );
			fed += n;
		}
	}
};

//---------------------------------------------------------------------------
// The organ's controls on top of a Knobs wall.
//---------------------------------------------------------------------------
struct OrganKnobs
{
	double low     = 200.0;
	double high    = 2000.0;
	double gain[ 3 ] = { 24.0, 24.0, 24.0 };
	int gel[ 3 ]   = { 0, 1, 2 };
	bool lamp      = false;
};

/// The plugin's own defaults, read back from a fresh instance.
OrganKnobs organDefaults()
{
	Filament p;
	OrganKnobs o;
	o.low       = organ::CrossoverLowHertz( p.GetFloatParameter( Filament::PT_CROSSOVER_LOW ) );
	o.high      = organ::CrossoverHighHertz( p.GetFloatParameter( Filament::PT_CROSSOVER_HIGH ) );
	o.gain[ 0 ] = organ::GainDb( p.GetFloatParameter( Filament::PT_BASS_GAIN ) );
	o.gain[ 1 ] = organ::GainDb( p.GetFloatParameter( Filament::PT_MID_GAIN ) );
	o.gain[ 2 ] = organ::GainDb( p.GetFloatParameter( Filament::PT_TREBLE_GAIN ) );
	return o;
}

void applyOrgan( Filament& p, const Knobs& k, const OrganKnobs& o )
{
	apply( p, k );
	set( p, "Organ", 1.0f );
	set( p, "Crossover Low", organ::CrossoverLowParam( o.low ) );
	set( p, "Crossover High", organ::CrossoverHighParam( o.high ) );
	set( p, "Bass Gain", organ::GainParam( o.gain[ 0 ] ) );
	set( p, "Mid Gain", organ::GainParam( o.gain[ 1 ] ) );
	set( p, "Treble Gain", organ::GainParam( o.gain[ 2 ] ) );
	set( p, "Bass Gel", static_cast< float >( o.gel[ 0 ] ) );
	set( p, "Mid Gel", static_cast< float >( o.gel[ 1 ] ) );
	set( p, "Treble Gel", static_cast< float >( o.gel[ 2 ] ) );
	set( p, "Status Lamp", o.lamp ? 1.0f : 0.0f );
}

/// One organ frame: the audio fed up to the frame's end plus the lead, then
/// the render. Frame 0 starts kNominalFrame before its clock: the plugin's
/// first frame is that long.
bool organFrame( Session& s, Audio* audio, long frame )
{
	if( audio != nullptr )
	{
		const double end   = s.clockOffset + static_cast< double >( frame ) / s.fps;
		const double start = frame == 0 ? end - model::kNominalFrame : end - 1.0 / s.fps;
		audio->feedFrame( s.plugin.OrganForTest(), start, end );
	}
	return s.renderAt( frame );
}

//---------------------------------------------------------------------------
// The reference for a filament behind a triac, in double: Lamp.h's ODE with
// the drive a sine cut at each half-cycle's firing point. Positions are in
// half-cycles from the host clock's zero (w = 2 x hertz x t).
//
//   scheme  the plugin's stated scheme on its own schedule: per substep the
//           mean of ( v / V )^2, written here as hi - lo - ( sin 2 pi hi -
//           sin 2 pi lo ) / 2 pi with std::sin -- the plugin's product form
//           and Taylor sine are not reused -- held over an RK4 step.
//   fine    the ODE itself: RK4 between every zero crossing and firing, at
//           kFinePerHalf steps a half-cycle, the waveform evaluated at each
//           stage. Nothing in it knows the energy has a closed form.
//---------------------------------------------------------------------------
constexpr int kFinePerHalf = 400;

struct TriacReference
{
	lamp::Lamp L;
	double ambient = 298.15;
	std::map< int64_t, double > fire;///< half-cycle -> firing fraction
	double otherwise = 2.0;          ///< every half-cycle not in the map (2 = never)

	double fireAt( int64_t half ) const
	{
		const auto it = fire.find( half );
		return it == fire.end() ? otherwise : it->second;
	}

	static double energy( double lo, double hi )
	{
		if( hi <= lo )
			return 0.0;
		return hi - lo - ( std::sin( 2.0 * lamp::kPi * hi ) - std::sin( 2.0 * lamp::kPi * lo ) ) / ( 2.0 * lamp::kPi );
	}

	double meanOver( double w0, double w1 ) const
	{
		double e = 0.0;
		for( int64_t K = static_cast< int64_t >( std::floor( w0 ) ); static_cast< double >( K ) < w1; ++K )
		{
			const double lo = std::max( w0 - static_cast< double >( K ), 0.0 );
			const double hi = std::min( w1 - static_cast< double >( K ), 1.0 );
			e += energy( std::max( lo, fireAt( K ) ), hi );
		}
		return e / ( w1 - w0 );
	}

	double rk4( double T, double h, double v1, double vm, double v4 ) const
	{
		const double k1 = lamp::Derivative( L, T, v1, ambient, 0 );
		const double k2 = lamp::Derivative( L, T + 0.5 * h * k1, vm, ambient, 0 );
		const double k3 = lamp::Derivative( L, T + 0.5 * h * k2, vm, ambient, 0 );
		const double k4 = lamp::Derivative( L, T + h * k3, v4, ambient, 0 );
		return std::clamp( T + h / 6.0 * ( k1 + 2.0 * k2 + 2.0 * k3 + k4 ), 100.0, 3695.0 );
	}

	/// One frame on the plugin's schedule: n substeps over `seconds` from w0.
	double scheme( double T, double w0, double seconds, int n, double* floatBound ) const
	{
		const double h  = seconds / n;
		const double dw = 2.0 * L.hertz * h;
		for( int k = 0; k < n; ++k )
		{
			const double a  = w0 + k * dw;
			const double v2 = meanOver( a, a + dw );
			const double t  = rk4( T, h, v2, v2, v2 );
			//Float on the GPU: the final add's rounding, kDerivativeRel of the
			//step for dT/dt, and as much again for the energy (~15 float
			//operations at half an ULP, and the firing point's interpolation).
			if( floatBound )
				*floatBound += ulpAt( t ) + 2.0 * kDerivativeRel * std::fabs( t - T );
			T = t;
		}
		return T;
	}

	/// The same frame, finely, through the waveform itself.
	double fine( double T, double w0, double seconds ) const
	{
		const double w1 = w0 + 2.0 * L.hertz * seconds;
		double w        = w0;
		while( w < w1 )
		{
			const int64_t K = static_cast< int64_t >( std::floor( w ) );
			double next     = std::min( static_cast< double >( K + 1 ), w1 );
			const double f  = static_cast< double >( K ) + fireAt( K );
			if( f > w && f < next )
				next = f;
			const double mid     = 0.5 * ( w + next ) - static_cast< double >( K );
			const bool on        = mid >= fireAt( K );
			const int m          = std::max( 2, static_cast< int >( std::ceil( ( next - w ) * kFinePerHalf ) ) );
			const double dw      = ( next - w ) / m;
			const double h       = dw / ( 2.0 * L.hertz );
			auto v2              = [ & ]( double at ) {
				const double s = std::sin( lamp::kPi * ( at - static_cast< double >( K ) ) );
				return on ? 2.0 * s * s : 0.0;
			};
			for( int k = 0; k < m; ++k )
			{
				const double a = w + k * dw;
				T              = rk4( T, h, v2( a ), v2( a + 0.5 * dw ), v2( a + dw ) );
			}
			w = next;
		}
		return T;
	}
};

/// The first u in [0, 1] where a row's integral reaches theta, as the
/// shader's search does it but in double; 2 if never.
double firstCrossing( const float* row, int channel, double theta )
{
	double a = row[ channel ];
	if( a >= theta )
		return 0.0;
	for( int j = 0; j < organ::kIntervals; ++j )
	{
		const double b = row[ ( j + 1 ) * 4 + channel ];
		if( b >= theta )
		{
			const double span = b - a;
			const double t    = span > 0.0 ? std::clamp( ( theta - a ) / span, 0.0, 1.0 ) : 0.0;
			return ( j + t ) / organ::kIntervals;
		}
		a = b;
	}
	return 2.0;
}

/// Every half-cycle's control row, the last (most complete) copy the plugin
/// built of each, and the frames' schedule.
struct OrganLog
{
	std::map< int64_t, std::vector< float > > rows;
	struct FrameRecord
	{
		double start = 0.0, seconds = 0.0;
		int substeps = 0;
		double T[ 3 ] = { 0.0, 0.0, 0.0 };
		double b      = 0.0;
	};
	std::vector< FrameRecord > frames;
	int64_t firstHalf0 = 0;

	void take( Filament& p, double now, const float px[ 4 ] )
	{
		organ::Organ& o = p.OrganForTest();
		if( frames.empty() )
			firstHalf0 = o.FirstHalf();
		for( int r = 0; r < o.RowCount(); ++r )
		{
			const float* row = o.Rows() + static_cast< size_t >( r ) * organ::kRowPoints * 4;
			rows[ o.FirstHalf() + r ].assign( row, row + organ::kRowPoints * 4 );
		}
		FrameRecord f;
		double phase0 = 0.0;
		p.LastFrameForTest( f.seconds, f.substeps, phase0 );
		f.start = now - f.seconds;
		for( int c = 0; c < 3; ++c )
			f.T[ c ] = px[ c ];
		f.b = px[ 3 ];
		frames.push_back( f );
	}
};

/// Replays a log through the reference, channel by channel: the scheme on
/// the plugin's schedule (and the float bound it accumulates), and the ODE
/// finely, with the firing points the plugin's rows give.
struct Replay
{
	std::vector< double > scheme[ 3 ], fine[ 3 ], floatSum[ 3 ];
};

Replay replay( const OrganLog& log, const lamp::Lamp& L, double ambient, double clock )
{
	Replay out;
	for( int c = 0; c < 3; ++c )
	{
		TriacReference ref;
		ref.L       = L;
		ref.ambient = ambient;
		const double b = log.frames.empty() ? 1.0 : log.frames.back().b;
		for( const auto& entry : log.rows )
			if( b > 0.0 )
				ref.fire[ entry.first ] = firstCrossing( entry.second.data(), c, 0.5 / b );
		double Ts = ambient, Tf = ambient, bound = 0.0;
		for( const OrganLog::FrameRecord& f : log.frames )
		{
			//Half-cycle positions relative to a whole number of cycles near the
			//clock's offset: exact in double whatever the offset.
			const double w0 = 2.0 * L.hertz * ( f.start - clock ) + 2.0 * L.hertz * clock;
			if( f.substeps > 0 )
			{
				Ts = ref.scheme( Ts, w0, f.seconds, f.substeps, &bound );
				Tf = ref.fine( Tf, w0, f.seconds );
			}
			out.scheme[ c ].push_back( Ts );
			out.fine[ c ].push_back( Tf );
			out.floatSum[ c ].push_back( bound );
		}
	}
	return out;
}

//---------------------------------------------------------------------------
// --crossover (no GL): the bands' corners, measured from the organ's own
// crossover with a sine through earshot.
//---------------------------------------------------------------------------
struct ToneGain
{
	double gain[ 3 ] = { 0.0, 0.0, 0.0 };
	double leak      = 0.0;///< the lock-in's worst relative error, from its sample count
};

ToneGain toneThrough( double hertz, double low, double high, int perturb )
{
	organ::Organ o;
	Audio a;
	a.kind      = Audio::Kind::Tone;
	a.toneHertz = hertz;
	a.toneAmp   = 0.5;
	organ::Settings s;
	s.lowHertz  = low;
	s.highHertz = high;
	s.perturb   = perturb;
	o.RecordForTest( true );
	const double fps   = 100.0;
	const int frames   = 100, keep = 60;
	std::vector< double > rec[ 4 ];
	for( int f = 0; f < frames; ++f )
	{
		const double end = f / fps;
		a.feedTo( o, end + 1.0 / fps + organ::kReserveSeconds + kFeedLead );
		o.Advance( end - 1.0 / fps, end, 50.0, 0, 2, s );
		if( f >= frames - keep )
			for( int i = 0; i < 4; ++i )
			{
				const std::vector< double >& r = o.RecordedForTest( i );
				rec[ i ].insert( rec[ i ].end(), r.begin(), r.end() );
			}
	}
	//A lock-in over a whole number of periods (to the nearest sample): the
	//image at -f leaks in by |sin N w| / ( N sin w ) of the amplitude.
	const double R     = o.TapRateForTest();
	const double w     = 2.0 * lamp::kPi * hertz / R;
	const size_t avail = rec[ 0 ].size();
	const double periods = std::floor( static_cast< double >( avail ) * hertz / R );
	const size_t N     = std::min( avail, static_cast< size_t >( std::llround( periods * R / hertz ) ) );
	auto amplitude     = [ & ]( const std::vector< double >& x ) {
		std::complex< double > sum( 0.0, 0.0 );
		for( size_t n = 0; n < N; ++n )
			sum += x[ avail - N + n ] * std::polar( 1.0, -w * static_cast< double >( n ) );
		return 2.0 * std::abs( sum ) / static_cast< double >( N );
	};
	ToneGain g;
	const double in = amplitude( rec[ 0 ] );
	for( int c = 0; c < 3; ++c )
		g.gain[ c ] = amplitude( rec[ 1 + c ] ) / in;
	g.leak = std::fabs( std::sin( N * w ) ) / ( static_cast< double >( N ) * std::fabs( std::sin( w ) ) );
	return g;
}

/// Where band `c`'s gain crosses `level`, by bisection in log frequency
/// between lo and hi (the gain monotonic there); `leak` the worst lock-in
/// error met.
double cornerOf( int c, double level, double lo, double hi, double low, double high, int perturb, double& leak, double& slope )
{
	leak                = 0.0;
	const double gLo    = toneThrough( lo, low, high, perturb ).gain[ c ];
	const bool rising   = gLo < level;
	for( int i = 0; i < 26; ++i )
	{
		const double mid = std::sqrt( lo * hi );
		const ToneGain g = toneThrough( mid, low, high, perturb );
		leak             = std::max( leak, g.leak );
		if( ( g.gain[ c ] < level ) == rising )
			lo = mid;
		else
			hi = mid;
	}
	const double f = std::sqrt( lo * hi );
	//The gain's slope per unit ln f at the corner, measured, to turn a gain
	//error into a frequency error.
	const double d  = 1e-3;
	const double g1 = toneThrough( f * std::exp( -d ), low, high, perturb ).gain[ c ];
	const double g2 = toneThrough( f * std::exp( d ), low, high, perturb ).gain[ c ];
	slope           = std::fabs( g2 - g1 ) / ( 2.0 * d );
	return f;
}

int runCrossover( int, int, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "crossover: a sine through earshot's tap into the organ's crossover; each band's -6.02 dB point found by bisection (no GL)\n" );
	int failures = 0;
	struct Pair
	{
		double low, high;
	};
	//60 Hz / 6 kHz: a decade and more apart, so the mid band's own corners
	//are measurable too. The plugin's defaults: the low and high bands.
	const OrganKnobs defaults = organDefaults();
	const Pair pairs[]        = { { 60.0, 6000.0 }, { defaults.low, defaults.high } };
	for( size_t pi = 0; pi < sizeof( pairs ) / sizeof( pairs[ 0 ] ); ++pi )
	{
		const Pair& p = pairs[ pi ];
		struct Corner
		{
			int band;
			double set;
			const char* what;
			double bias;///< the other section's attenuation at this corner, bounded
		};
		std::vector< Corner > corners = { { 0, p.low, "bass low-pass", 0.0 }, { 2, p.high, "treble high-pass", 0.0 } };
		if( pi == 0 )
		{
			//The mid band is HP( f1 ) x LP( f2 ): at f1 the LP section adds
			//|LP_f2( f1 )| >= 1 / ( 1 + ( f1 / f2 )^2 ) (the LR2 magnitude, as a
			//bound only), which moves the -6.02 dB point by at most 2 ( f1 / f2 )^2.
			const double r = p.low / p.high;
			corners.push_back( { 1, p.low, "mid high-pass", 2.0 * r * r } );
			corners.push_back( { 1, p.high, "mid low-pass", 2.0 * r * r } );
		}
		for( const Corner& c : corners )
		{
			double leak = 0.0, slope = 0.0;
			const double f = cornerOf( c.band, 0.5, c.set / 2.0, c.set * 2.0, p.low, p.high, perturb, leak, slope );
			//Two lock-in estimates per gain, a bracket of 4 ln 2 / 2^26 in ln f,
			//and the float tap (2^-24 a sample, averaged: far below the leak).
			const double gainErr = 2.0 * leak + 1e-6;
			const double tol     = gainErr / std::max( slope, 1e-9 ) + 4.0 * std::log( 2.0 ) / 67108864.0 + c.bias;
			const double rel     = f / c.set - 1.0;
			failures += report( std::fabs( rel ) <= tol, quiet, "%-16s set %7.2f Hz: -6.02 dB at %9.4f Hz, off %+.2e (tolerance %.1e: lock-in %.1e over a gain slope %.3f/ln f%s)",
			                    c.what, c.set, f, rel, tol, gainErr, slope, c.bias > 0.0 ? ", the other section" : "" );
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --detector (no GL): each band's detector releases with ETI 428's R19 C.
//---------------------------------------------------------------------------
int runDetector( int, int, int perturb, bool quiet = false )
{
	//A tone in the middle of each band for 0.3 s, stopped at a zero crossing
	//(a whole number of periods at 48 kHz), then silence. Once the band's own
	//ring-down is below the detector the diode is off and the node discharges
	//through R19 alone: ln v falls on a straight line of slope -1 / ( R19 C ).
	//The slope is fitted to the organ's own detector samples (recorded at the
	//tap rate) over the 150 ms from 40 ms after the band last swung past 1%
	//of its level.
	const OrganKnobs defaults = organDefaults();
	const double tones[ 3 ]   = { 100.0, 600.0, 6000.0 };//480, 80 and 8 samples a period
	const char* names[ 3 ]    = { "bass", "mid", "treble" };
	if( !quiet )
		std::printf( "detector: a tone in each band stopped at a zero crossing; the release fitted to the organ's own detector (no GL)\n" );
	int failures = 0;
	for( int c = 0; c < 3; ++c )
	{
		organ::Organ o;
		Audio a;
		a.kind      = Audio::Kind::Tone;
		a.toneHertz = tones[ c ];
		a.toneAmp   = 0.5;
		a.toneStop  = 14400;//0.3 s
		organ::Settings s;
		s.lowHertz  = defaults.low;
		s.highHertz = defaults.high;
		s.perturb   = perturb;
		o.RecordForTest( true );
		const double fps = 100.0;
		std::vector< double > band, node;
		for( int f = 0; f < 80; ++f )
		{
			const double end = f / fps;
			a.feedTo( o, end + 1.0 / fps + organ::kReserveSeconds + kFeedLead );
			o.Advance( end - 1.0 / fps, end, 50.0, 0, 2, s );
			const std::vector< double >& b = o.RecordedForTest( 1 + c );
			const std::vector< double >& d = o.RecordedForTest( 4 + c );
			band.insert( band.end(), b.begin(), b.end() );
			node.insert( node.end(), d.begin(), d.end() );
		}
		const double R = o.TapRateForTest();
		double level   = 0.0;
		for( double v : band )
			level = std::max( level, std::fabs( v ) );
		size_t last = 0;
		for( size_t n = 0; n < band.size(); ++n )
			if( std::fabs( band[ n ] ) > 0.01 * level )
				last = n;
		const size_t from = last + static_cast< size_t >( 0.040 * R );
		const size_t to   = from + static_cast< size_t >( 0.150 * R );
		if( to > node.size() || node[ to - 1 ] <= 0.0 )
		{
			failures += report( false, quiet, "%-6s detector: the recording ended before the window (%zu of %zu samples)", names[ c ], node.size(), to );
			continue;
		}
		//Least squares of ln v against t, and the diode's margin: how close the
		//band's tail came to the node over the window (it conducts only when
		//-band exceeds the node).
		double st = 0.0, sy = 0.0, stt = 0.0, sty = 0.0, margin = 0.0;
		const double N = static_cast< double >( to - from );
		for( size_t n = from; n < to; ++n )
		{
			const double t = static_cast< double >( n - from ) / R, y = std::log( node[ n ] );
			st += t;
			sy += y;
			stt += t * t;
			sty += t * y;
			margin = std::max( margin, std::fabs( band[ n ] ) / node[ n ] );
		}
		const double slope = ( N * sty - st * sy ) / ( N * stt - st * st );
		const double icept = ( sy - slope * st ) / N;
		double resid       = 0.0;
		for( size_t n = from; n < to; ++n )
			resid = std::max( resid, std::fabs( std::log( node[ n ] ) - ( icept + slope * static_cast< double >( n - from ) / R ) ) );
		const double tau  = -1.0 / slope;
		const double want = organ::kDischargeOhms * organ::kDetectorFarads[ c ];
		//A slope error e over the window's length Tw tilts the line by e Tw / 2
		//at its ends; the fit's own residual bounds what it can hide. Plus the
		//double arithmetic of the sums (N x 2^-52 relative).
		const double Tw  = ( N - 1.0 ) / R;
		const double tol = 2.0 * resid / ( Tw * std::fabs( slope ) ) + N * std::ldexp( 1.0, -52 );
		failures += report( std::fabs( tau / want - 1.0 ) <= tol && margin < 1.0, quiet,
		                    "%-6s %5.0f Hz tone: release tau %.4f ms; R19 x C%s = %.4f ms; off %+.1e (tolerance %.1e: fit residual %.1e over %.0f ms); the diode stayed off (|band| at most %.1e of the node)", names[ c ],
		                    tones[ c ], tau * 1e3, c == 0 ? " (1.0 uF)" : " (0.47 uF)", want * 1e3, tau / want - 1.0, tol, resid, Tw * 1e3, margin );
	}
	return failures;
}

//---------------------------------------------------------------------------
// --firing: steady control, steady lamp; its RMS out of its temperature.
//---------------------------------------------------------------------------
/// The ( V / V_rated )^2 that holds a lamp steady at T: the power balance
/// solved for the drive (dT/dt is linear in it).
double drivenAt( double T, double ambient )
{
	const lamp::Lamp any = lamp::Make( 60.0, 50.0 );
	const double d0      = lamp::Derivative( any, T, 0.0, ambient, 0 );
	const double d1      = lamp::Derivative( any, T, 1.0, ambient, 0 );
	return -d0 / ( d1 - d0 );
}

int runFiring( int W, int H, int perturb, bool quiet = false )
{
	//runSteady's lamp and window: 200 W 230 V, the mean over 25 mains cycles
	//at 1200 fps. A steady control on every channel, so the triacs fire at a
	//fixed point of every half-cycle. A lamp held at 150 degrees sits near
	//1200 K, where it relaxes at about 0.5 /s: it settles at 60 fps for as
	//long as its own relaxation rate says (to 1 mK), then the window.
	const double settleFps = 60.0, windowFps = 1200.0;
	const int window       = 600;
	if( !quiet )
		std::printf( "firing: a 200 W 230 V lamp on each triac, a steady control, mean T over 25 mains cycles; the lamp's RMS read back out of its temperature, %dx%d\n", W, H );
	int failures = 0;
	const double degrees[] = { 30.0, 60.0, 90.0, 120.0, 150.0 };
	for( double deg : degrees )
	{
		Knobs k;
		k.watts                = 200.0;
		const lamp::Lamp L     = lampOf( k );
		const double ambient   = ambientOf( k );
		const double control   = 0.5 / ( deg / 180.0 );

		Session s;
		s.fps = settleFps;
		applyOrgan( s.plugin, k, OrganKnobs() );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeState );
		if( !s.begin( W, H ) )
			return 1;
		s.upload( flat( W, H, 1.0 ) );
		s.plugin.OrganForTest().ForceControlForTest( control );
		int x, y;
		s.cellPixel( 3, 1, k.columns, k.rows, x, y );

		//The reference, on the plugin's own schedule: the scheme (exact
		//energy per substep) and the ODE through the cut sine, finely.
		TriacReference ref;
		ref.L       = L;
		ref.ambient = ambient;
		double Ts = ambient, Tf = ambient;
		double u = 1.0, closed = 0.0, predicted = ambient, slopeT = 1.0;
		long settle = 0;
		double sum[ 3 ] = {}, sumScheme = 0.0, sumFine = 0.0, lo = 1e9, hi = 0.0, h = 0.0;
		for( long f = 0; f < settle + window || f == 0; ++f )
		{
			if( settle > 0 && f == settle )
			{
				//From here at 1200 fps, the clock carrying on.
				s.fps         = windowFps;
				s.clockOffset = static_cast< double >( settle ) / settleFps - static_cast< double >( settle ) / windowFps;
			}
			if( !s.renderAt( f ) )
				return 1;
			float px[ 4 ];
			s.readPixel( x, y, px );
			double seconds = 0.0, phase0 = 0.0;
			int n = 0;
			s.plugin.LastFrameForTest( seconds, n, phase0 );
			const double now   = s.clockOffset + static_cast< double >( f ) / s.fps;
			const double w0    = 2.0 * L.hertz * ( now - seconds );
			if( f == 0 )
			{
				//The firing point the ramp law gives for this control and this
				//cell (b, read back: the white card's luma through the drive pass).
				const double b = px[ 3 ];
				u              = std::min( 0.5 / ( control * b ), 1.0 );
				const double alpha = lamp::kPi * u;
				closed             = 1.0 - alpha / lamp::kPi + std::sin( 2.0 * alpha ) / ( 2.0 * lamp::kPi );
				predicted          = lamp::SteadyKelvin( closed, ambient, 0 );
				const double e     = 0.5;
				slopeT             = std::fabs( ( lamp::Derivative( L, predicted + e, closed, ambient, 0 ) - lamp::Derivative( L, predicted - e, closed, ambient, 0 ) ) / ( 2.0 * e ) );
				//The linear relaxation at the end point is the slowest part of a
				//rise from cold (the drive outweighs the losses below it).
				const double seconds_ = std::max( 1.5, std::log( std::max( predicted - ambient, 1.0 ) / 1e-3 ) / slopeT );
				settle                = static_cast< long >( std::ceil( seconds_ * settleFps ) );
				ref.otherwise         = u;
				//The half-cycle the run starts in: its ramp starts at the start.
				const int64_t K0 = static_cast< int64_t >( std::floor( w0 ) );
				const double uIn = w0 - static_cast< double >( K0 );
				ref.fire[ K0 ]   = uIn + u < 1.0 ? uIn + u : 2.0;
			}
			if( n > 0 )
			{
				Ts = ref.scheme( Ts, w0, seconds, n, nullptr );
				Tf = ref.fine( Tf, w0, seconds );
			}
			if( f >= settle && settle > 0 )
			{
				for( int c = 0; c < 3; ++c )
					sum[ c ] += px[ c ];
				sumScheme += Ts;
				sumFine += Tf;
				lo = std::min( lo, static_cast< double >( px[ 0 ] ) );
				hi = std::max( hi, static_cast< double >( px[ 0 ] ) );
				h  = seconds / std::max( n, 1 );
			}
		}
		s.end();

		const double schemeMean = sumScheme / window, fineMean = sumFine / window;
		//The tolerance, each part measured or bounded:
		//  bias  what reading an RMS out of a mean T costs in the ODE itself
		//        (T^4 is convex and R( T ) moves within a cycle): the fine
		//        ODE's mean against the static balance, on the same schedule;
		//  disc  the scheme's discretisation: its mean against the fine ODE's;
		//  float runSteady's bound for float in a contracting system.
		const double pp         = hi - lo;
		const double bias       = std::fabs( fineMean - predicted );
		const double disc       = std::fabs( schemeMean - fineMean );
		const double stepMax    = pp * lamp::kPi * 2.0 * L.hertz * h;
		const double floatBound = 2.0 * ( ulpAt( predicted ) + 2.0 * kDerivativeRel * stepMax ) / ( slopeT * h );
		const double tolerance  = bias + disc + floatBound;
		for( int c = 0; c < 3; ++c )
		{
			const double mean  = sum[ c ] / window;
			const double rms   = std::sqrt( drivenAt( mean, ambient ) );
			//The RMS tolerance is the temperature's, through d( RMS ) / dT.
			const double dRms  = std::fabs( std::sqrt( drivenAt( mean + 0.5, ambient ) ) - std::sqrt( drivenAt( mean - 0.5, ambient ) ) );
			const char* name[] = { "bass", "mid", "treble" };
			failures += report( std::fabs( mean - predicted ) <= tolerance, quiet,
			                    "%3.0f deg, %-6s: mean %.3f K; the closed form's RMS %.5f predicts %.3f K; off %+.4f (tolerance %.4f: bias %.4f + disc %.4f + float %.4f; ripple %.2f K pp; settled %.1f s)",
			                    deg, name[ c ], mean, std::sqrt( closed ), predicted, mean - predicted, tolerance, bias, disc, floatBound, pp, static_cast< double >( settle ) / settleFps );
			if( c == 0 )
				failures += report( std::fabs( rms - std::sqrt( closed ) ) <= tolerance * dRms, quiet, "        the lamp's RMS out of its T %.5f against 1 - a/pi + sin 2a / 2pi's %.5f (tolerance %.5f)", rms,
				                    std::sqrt( closed ), tolerance * dRms );
		}
	}
	return failures;
}

//---------------------------------------------------------------------------
// --organstep: a step of control from cold, against the ODE.
//---------------------------------------------------------------------------
int runOrganStep( int W, int H, int perturb, bool quiet = false )
{
	//runRise's lamp and clock: 60 W 230 V at 2400 fps. A control of 1 from
	//frame 24 on: the triac fires at 90 degrees, the ramp starting at the
	//step mid half-cycle.
	const double fps = 2400.0;
	const long off = 24, on = 960;
	const double control = 1.0;
	if( !quiet )
		std::printf( "organ step: a 60 W 230 V lamp behind a triac, a steady control from cold at t = 10 ms, 2400 fps, against the ODE through the cut sine, %dx%d\n", W, H );
	Session s;
	s.fps = fps;
	Knobs k;
	applyOrgan( s.plugin, k, OrganKnobs() );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeState );
	if( !s.begin( W, H ) )
		return 1;
	s.upload( flat( W, H, 1.0 ) );
	int x, y;
	s.cellPixel( 2, 1, k.columns, k.rows, x, y );

	TriacReference ref;
	ref.L       = lampOf( k );
	ref.ambient = ambientOf( k );
	const double hz = ref.L.hertz;
	double Ts = ref.ambient, Tf = ref.ambient, floatSum = 0.0;
	double worstFloat = 0.0, worstGpuFine = 0.0, worstSchemeFine = 0.0;
	int outside = 0;
	std::vector< double > t, gpu, fineT;
	for( long f = 0; f < off + on; ++f )
	{
		if( f == off )
			s.plugin.OrganForTest().ForceControlForTest( control );
		if( !s.renderAt( f ) )
			return 1;
		float px[ 4 ];
		s.readPixel( x, y, px );
		double seconds = 0.0, phase0 = 0.0;
		int n = 0;
		s.plugin.LastFrameForTest( seconds, n, phase0 );
		const double start = f / fps - seconds;
		const double w0    = 2.0 * hz * start;
		if( f == off )
		{
			//The ramp law: every half-cycle after the step fires where b x the
			//integral of the control reaches 1/2; the one the step lands in
			//starts its ramp at the step.
			const double b    = px[ 3 ];
			const double u    = 0.5 / ( control * b );
			const int64_t K0  = static_cast< int64_t >( std::floor( w0 ) );
			const double uIn  = w0 - static_cast< double >( K0 );
			if( uIn + u < 1.0 )
				ref.fire[ K0 ] = uIn + u;
			for( int64_t K = K0 + 1; K < K0 + 4 + static_cast< int64_t >( 2.0 * hz * ( on + 2 ) / fps ); ++K )
				ref.fire[ K ] = u;
		}
		if( n > 0 )
		{
			Ts = ref.scheme( Ts, w0, seconds, n, &floatSum );
			Tf = ref.fine( Tf, w0, seconds );
		}
		const double g = px[ 0 ];
		worstFloat      = std::max( worstFloat, std::fabs( g - Ts ) );
		worstGpuFine    = std::max( worstGpuFine, std::fabs( g - Tf ) );
		worstSchemeFine = std::max( worstSchemeFine, std::fabs( Ts - Tf ) );
		if( std::fabs( g - Ts ) > floatSum )
			++outside;
		t.push_back( f / fps );
		gpu.push_back( g );
		fineT.push_back( Tf );
	}
	s.end();
	int failures = 0;
	failures += report( outside == 0, quiet, "the GPU runs the stated scheme (exact energy per substep, RK4): worst |GPU - double| %.4f K, inside the accumulated float bound (%.3f K at the end) on every frame",
	                    worstFloat, floatSum );
	const double tolT = worstSchemeFine + floatSum;
	failures += report( worstGpuFine <= tolT, quiet, "it follows the ODE through the cut sine: worst |GPU - fine| %.4f K (tolerance %.4f: discretisation %.4f measured + float)", worstGpuFine, tolT, worstSchemeFine );
	//t90 of the rise, against the fine solution.
	const double T0      = ambientOf( k );
	const double steady  = fineT.back();
	const double target  = T0 + 0.9 * ( steady - T0 );
	const double t0      = off / fps - 1.0 / fps;
	const double tGpu    = crossing( t, gpu, target, true ) - t0;
	const double tFine   = crossing( t, fineT, target, true ) - t0;
	size_t i90           = 0;
	while( i90 + 1 < fineT.size() && fineT[ i90 ] < target )
		++i90;
	const double slope   = ( fineT[ std::min( i90 + 1, fineT.size() - 1 ) ] - fineT[ i90 > 0 ? i90 - 1 : 0 ] ) * fps / 2.0;
	const double tolT90  = 1.0 / fps + tolT / std::max( slope, 1.0 );
	failures += report( tGpu > 0.0 && std::fabs( tGpu - tFine ) <= tolT90, quiet, "t90 (to %.0f K of a %.0f K steady state) %.2f ms; the ODE says %.2f ms (tolerance %.2f ms)", target, steady, tGpu * 1e3, tFine * 1e3, tolT90 * 1e3 );
	return failures;
}

//---------------------------------------------------------------------------
// --organframes: the music at 30, 60 and 144 fps.
//---------------------------------------------------------------------------
struct OrganTrace
{
	OrganLog log;
	Replay ref;
	bool ok = true;
};

/// What the music checks feed the wall: a flat 0.2, about the mean luma of
/// Resolume's Beat demo clips at the bulb grid (0.12-0.21), the level the
/// default gains were set on. On white the defaults hold every lamp on.
constexpr double kMusicLevel = 0.2;

/// The synthetic track through the organ with its defaults on a flat wall of
/// kMusicLevel, `seconds` long at `fps`, the clock starting at `clock`.
OrganTrace organTrace( int W, int H, int perturb, double fps, double seconds, double clock )
{
	OrganTrace out;
	Session s;
	s.fps         = fps;
	s.clockOffset = clock;
	Knobs k;
	applyOrgan( s.plugin, k, organDefaults() );
	s.plugin.SetPerturbForTest( perturb );
	s.plugin.SetProbeForTest( model::kProbeState );
	if( !s.begin( W, H ) )
	{
		out.ok = false;
		return out;
	}
	s.upload( flat( W, H, kMusicLevel ) );
	int x, y;
	s.cellPixel( 2, 1, k.columns, k.rows, x, y );
	Audio audio;
	const long frames = static_cast< long >( std::llround( seconds * fps ) );
	for( long f = 0; f <= frames; ++f )
	{
		if( !organFrame( s, &audio, f ) )
		{
			out.ok = false;
			break;
		}
		float px[ 4 ];
		s.readPixel( x, y, px );
		out.log.take( s.plugin, clock + f / fps, px );
	}
	s.end();
	out.ref = replay( out.log, lampOf( k ), ambientOf( k ), clock );
	return out;
}

int runOrganFrames( int W, int H, int perturb, bool quiet = false )
{
	const double seconds = 2.0;
	if( !quiet )
		std::printf( "organ frames: the synthetic track through the organ at 30, 60 and 144 fps, a 60 W lamp per channel, %dx%d\n", W, H );
	const double rates[ 3 ] = { 30.0, 60.0, 144.0 };
	OrganTrace runs[ 3 ];
	for( int i = 0; i < 3; ++i )
	{
		runs[ i ] = organTrace( W, H, perturb, rates[ i ], seconds, 0.0 );
		if( !runs[ i ].ok )
			return 1;
	}
	int failures = 0;
	//(1) The control: every half-cycle complete in all three runs, row for
	//    row, bit for bit.
	{
		const int64_t lastWhole = runs[ 0 ].log.rows.rbegin()->first - 3;
		size_t compared = 0, differ = 0;
		for( const auto& entry : runs[ 0 ].log.rows )
		{
			if( entry.first > lastWhole )
				break;
			bool all = true;
			for( int i = 1; i < 3; ++i )
			{
				const auto it = runs[ i ].log.rows.find( entry.first );
				all           = all && it != runs[ i ].log.rows.end() && it->second == entry.second;
			}
			++compared;
			differ += all ? 0 : 1;
		}
		failures += report( compared > 150 && differ == 0, quiet, "the control integral of every half-cycle (%zu of them) is bit-identical at 30, 60 and 144 fps: %zu differ", compared, differ );
	}
	//(2) Each run follows the stated scheme on its own schedule, to float.
	for( int i = 0; i < 3; ++i )
	{
		double worst = 0.0;
		int outside  = 0;
		const OrganTrace& r = runs[ i ];
		for( size_t f = 0; f < r.log.frames.size(); ++f )
			for( int c = 0; c < 3; ++c )
			{
				const double d = std::fabs( r.log.frames[ f ].T[ c ] - r.ref.scheme[ c ][ f ] );
				worst          = std::max( worst, d );
				if( d > r.ref.floatSum[ c ][ f ] )
					++outside;
			}
		failures += report( outside == 0, quiet, "%3.0f fps: the GPU runs the stated scheme on the music, worst |GPU - double| %.4f K, inside the float bound on every frame (%.3f K at the end)", rates[ i ], worst,
		                    r.ref.floatSum[ 0 ].back() );
	}
	//(3) At the moments all three share, the three frame rates agree to their
	//    own measured discretisation and float.
	const double moments[] = { 0.5, 1.0, 1.5, 2.0 };
	double worst = 0.0, worstTol = 0.0, worstFine = 0.0;
	bool ok      = true;
	for( double at : moments )
		for( int c = 0; c < 3; ++c )
		{
			double T[ 3 ], tol[ 3 ], fine[ 3 ];
			for( int i = 0; i < 3; ++i )
			{
				const size_t f = static_cast< size_t >( std::llround( at * rates[ i ] ) );
				T[ i ]         = runs[ i ].log.frames[ f ].T[ c ];
				fine[ i ]      = runs[ i ].ref.fine[ c ][ f ];
				tol[ i ]       = std::fabs( runs[ i ].ref.scheme[ c ][ f ] - fine[ i ] ) + runs[ i ].ref.floatSum[ c ][ f ];
			}
			for( int i = 0; i < 3; ++i )
				for( int j = i + 1; j < 3; ++j )
				{
					const double d = std::fabs( T[ i ] - T[ j ] );
					const double t = tol[ i ] + tol[ j ] + std::fabs( fine[ i ] - fine[ j ] );
					ok             = ok && d <= t;
					worst          = std::max( worst, d );
					worstTol       = std::max( worstTol, t );
					worstFine      = std::max( worstFine, std::fabs( fine[ i ] - fine[ j ] ) );
				}
		}
	failures += report( ok, quiet, "at t = 0.5, 1, 1.5 and 2 s the three rates agree: worst %.4f K between any two (each pair inside its own discretisation + float, the largest %.4f K; the fine solutions %.2e K apart)",
	                    worst, worstTol, worstFine );
	return failures;
}

//---------------------------------------------------------------------------
// --organclock: the same music with the host's clock at 499,000 s.
//---------------------------------------------------------------------------
int runOrganClock( int W, int H, int perturb, bool quiet = false )
{
	//503,316.25 s is a host clock 5.8 days in, where a float resolves 31 ms:
	//exact in binary, a whole number of half-cycles (50,331,625), and 23
	//half-cycles short of 12 x 2^22, so the ramps' half-cycle tag wraps
	//during the run.
	const double offset = 503316.25;
	if( !quiet )
		std::printf( "organ clock: the synthetic track at 60 fps with the host clock from 0 and from %.0f s, %dx%d\n", offset, W, H );
	OrganTrace a = organTrace( W, H, perturb, 60.0, 1.5, 0.0 );
	OrganTrace b = organTrace( W, H, perturb, 60.0, 1.5, offset );
	if( !a.ok || !b.ok )
		return 1;
	int failures = 0;
	size_t compared = 0, differ = 0;
	const int64_t lastWhole = a.log.rows.rbegin()->first - 3;
	for( const auto& entry : a.log.rows )
	{
		if( entry.first > lastWhole )
			break;
		const auto it = b.log.rows.find( entry.first - a.log.firstHalf0 + b.log.firstHalf0 );
		++compared;
		differ += it != b.log.rows.end() && it->second == entry.second ? 0 : 1;
	}
	failures += report( compared > 100 && differ == 0, quiet, "every half-cycle's control (%zu) is bit-identical with the clock at %.2f s: %zu differ", compared, offset, differ );
	const int64_t lastHalf = b.log.rows.rbegin()->first;
	failures += report( b.log.firstHalf0 % 4194304 > lastHalf % 4194304, quiet, "the ramps' tag wrapped at 2^22 during the run (half-cycle %lld to %lld)", static_cast< long long >( b.log.firstHalf0 ),
	                    static_cast< long long >( lastHalf ) );
	double worst = 0.0;
	bool ok      = true;
	for( size_t f = 0; f < a.log.frames.size() && f < b.log.frames.size(); ++f )
		for( int c = 0; c < 3; ++c )
		{
			const double d = std::fabs( a.log.frames[ f ].T[ c ] - b.log.frames[ f ].T[ c ] );
			worst          = std::max( worst, d );
			ok             = ok && d <= a.ref.floatSum[ c ][ f ] + b.ref.floatSum[ c ][ f ];
		}
	failures += report( ok, quiet, "every frame's temperatures agree: worst %.2e K (inside both runs' float bounds, %.3f K at the end)", worst, a.ref.floatSum[ 0 ].back() + b.ref.floatSum[ 0 ].back() );
	failures += report( a.log.frames.back().T[ 0 ] > 1000.0, quiet, "and the bass lamp was lit (%.1f K at the end)", a.log.frames.back().T[ 0 ] );
	return failures;
}

//---------------------------------------------------------------------------
// --organresize: an output resize and a grid change mid-music.
//---------------------------------------------------------------------------
int runOrganResize( int W, int H, int perturb, bool quiet = false )
{
	//runResize's three runs, with the organ on and the synthetic track: A
	//left alone, B's output 1.5x at frame 20 and back at 28, C's grid
	//16x9 -> 24x13 at frame 20. The cell level drops from 0.25 to 0.15 at 15.
	const int frames = 41;
	if( !quiet )
		std::printf( "organ resize: the organ on the synthetic track; the output resized and the grid changed mid-music, %dx%d\n", W, H );
	auto run = [ & ]( int variant, std::vector< double >& temps, double& floatBound ) -> bool {
		Session s;
		Knobs k;
		k.columns = 16;
		k.rows    = 9;
		applyOrgan( s.plugin, k, organDefaults() );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeState );
		if( !s.begin( W, H ) )
			return false;
		Audio audio;
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
			s.upload( flat( s.width, s.height, f < 15 ? 1.25 * kMusicLevel : 0.75 * kMusicLevel ) );
			if( !organFrame( s, &audio, f ) )
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
				for( int i = 0; i < 3; ++i )
					temps.push_back( px[ i ] );
			}
		s.end();
		return true;
	};
	std::vector< double > a, b, c;
	double boundA = 0.0, boundB = 0.0, boundC = 0.0;
	if( !run( 0, a, boundA ) || !run( 1, b, boundB ) || !run( 2, c, boundC ) || a.empty() )
		return 1;
	const double tolerance = std::max( boundA, boundB );
	double worstB = 0.0, worstC = 0.0;
	for( size_t i = 0; i < b.size() && i < a.size(); ++i )
		worstB = std::max( worstB, std::fabs( b[ i ] - a[ i ] ) );
	for( size_t i = 0; i < c.size(); ++i )
		worstC = std::max( worstC, std::fabs( c[ i ] - a[ i % 3 ] ) );
	int failures = 0;
	failures += report( worstB <= tolerance, quiet, "output resized to %dx%d and back mid-music: all %zu filaments hold the unresized run's T (bass %.1f, mid %.1f, treble %.1f K), worst %.2e K (tolerance %.1e)",
	                    W * 3 / 2, H * 3 / 2, b.size(), a[ 0 ], a[ 1 ], a[ 2 ], worstB, tolerance );
	failures += report( worstC <= tolerance, quiet, "grid changed 16x9 -> 24x13 mid-music: all %zu new filaments and their ramps carry on, worst %.2e K (tolerance %.1e)", c.size(), worstC, tolerance );
	failures += report( a[ 0 ] > 1000.0, quiet, "the music had the bass lamp lit when it was resized (%.1f K at frame %d)", a[ 0 ], frames - 1 );
	return failures;
}

//---------------------------------------------------------------------------
// --organcolour: each filament's light is its own gel's, at its own T.
//---------------------------------------------------------------------------
/// What the organ's light at one bulb must be, in double from the committed
/// tables: for each channel, gel row g at T interpolated on the Planck grid,
/// times 2^log2Y( T ). `bound` gets the float error of the GPU's sum (below).
void gelLight( const double T[ 3 ], const int gel[ 3 ], double out[ 3 ], double bound[ 3 ] )
{
	constexpr double eps = 1.0 / 8388608.0;//2^-23, a float's unit roundoff x 2
	for( int i = 0; i < 3; ++i )
		out[ i ] = bound[ i ] = 0.0;
	for( int c = 0; c < 3; ++c )
	{
		const double x  = std::clamp( ( T[ c ] - planck::kFirstKelvin ) / planck::kStepKelvin, 0.0, planck::kCount - 1 - 0.0001 );
		const int k     = static_cast< int >( std::floor( x ) );
		const double t  = x - k;
		const float* ga = filament::gels::kTable[ gel[ c ] ][ k ];
		const float* gb = filament::gels::kTable[ gel[ c ] ][ k + 1 ];
		const double la = planck::kTable[ k ][ 3 ], lb = planck::kTable[ k + 1 ][ 3 ];
		const double ex = la + ( lb - la ) * t;
		const double L  = std::exp2( ex );
		//The GPU's float arithmetic, op by op: x = ( T - first ) / step (two
		//roundings), the two lerps (three each), exp2 to GLSL 4.10's stated
		//precision (s4.7.1: 3 + 2|x| ULP), the product and the three-way sum.
		const double dx = 2.0 * eps * x;
		for( int i = 0; i < 3; ++i )
		{
			const double rgb   = ga[ i ] + ( gb[ i ] - ga[ i ] ) * t;
			const double slope = std::fabs( gb[ i ] - ga[ i ] ) + std::fabs( rgb ) * std::log( 2.0 ) * std::fabs( lb - la );
			out[ i ] += rgb * L;
			bound[ i ] += L * ( slope * dx + 3.0 * eps * ( std::fabs( ga[ i ] ) + std::fabs( gb[ i ] ) )
			                    + std::fabs( rgb ) * ( std::log( 2.0 ) * 3.0 * eps * ( std::fabs( la ) + std::fabs( lb ) ) + ( 3.0 + 2.0 * std::fabs( ex ) ) * eps + 2.0 * eps ) );
		}
	}
	for( int i = 0; i < 3; ++i )
		bound[ i ] += 2.0 * eps * std::fabs( out[ i ] );
}

int runOrganColour( int W, int H, int perturb, bool quiet = false )
{
	//The synthetic track on a flat wall at the music level, three runs with
	//the nine gels three to a run, so every row of the table is read and
	//each through a different channel. At four frames a frame of zero length
	//follows (the same clock again): no substeps, so the light written is
	//the light at the temperatures stored, read through the shutter probe.
	const int assignments[ 3 ][ 3 ] = { { 0, 1, 2 }, { 3, 4, 5 }, { 6, 7, 8 } };
	const long marks[]              = { 15, 30, 45, 60 };
	if( !quiet )
		std::printf( "organ colour: the synthetic track; each filament's light against the gel table at its own T, all nine gels, %dx%d\n", W, H );
	int failures = 0;
	double worstRatio = 0.0, worstOff = 0.0, worstBound = 0.0, spread = 0.0;
	int samples = 0;
	bool ran = true;
	for( const auto& gel : assignments )
	{
		Session s;
		Knobs k;
		k.columns = 16;
		k.rows    = 9;
		OrganKnobs o = organDefaults();
		for( int c = 0; c < 3; ++c )
			o.gel[ c ] = gel[ c ];
		applyOrgan( s.plugin, k, o );
		s.plugin.SetPerturbForTest( perturb );
		s.plugin.SetProbeForTest( model::kProbeState );
		if( !s.begin( W, H ) )
		{
			ran = false;
			break;
		}
		s.upload( flat( W, H, kMusicLevel ) );
		int x, y;
		s.cellPixel( 5, 4, k.columns, k.rows, x, y );
		Audio audio;
		size_t mark = 0;
		for( long f = 0; f <= marks[ 3 ] && ran; ++f )
		{
			ran = organFrame( s, &audio, f );
			if( !ran || mark >= 4 || f != marks[ mark ] )
				continue;
			++mark;
			float state[ 4 ], light[ 4 ];
			s.readPixel( x, y, state );
			s.plugin.SetProbeForTest( model::kProbeShutter );
			ran = s.renderAt( f );//dt = 0
			s.readPixel( x, y, light );
			s.plugin.SetProbeForTest( model::kProbeState );
			const double T[ 3 ] = { state[ 0 ], state[ 1 ], state[ 2 ] };
			double want[ 3 ], bound[ 3 ];
			gelLight( T, gel, want, bound );
			spread = std::max( spread, std::max( { T[ 0 ], T[ 1 ], T[ 2 ] } ) - std::min( { T[ 0 ], T[ 1 ], T[ 2 ] } ) );
			for( int i = 0; i < 3; ++i )
			{
				const double off = std::fabs( light[ i ] - want[ i ] );
				if( off / bound[ i ] > worstRatio )
				{
					worstRatio = off / bound[ i ];
					worstOff   = off / std::max( std::fabs( want[ i ] ), 1e-30 );
					worstBound = bound[ i ] / std::max( std::fabs( want[ i ] ), 1e-30 );
				}
			}
			++samples;
		}
		s.end();
	}
	if( !ran )
		return 1;
	failures += report( worstRatio <= 1.0, quiet, "at %d moments, every channel of the light is the gel table's for each filament's own T and gel: worst %.2e relative against a float bound of %.2e (%.2f of it)", samples, worstOff,
	                    worstBound, worstRatio );
	//Without different temperatures a gel swapped between channels would
	//sum to the same light and the check could not see it.
	failures += report( spread > 100.0, quiet, "and the three filaments were apart: up to %.0f K between them", spread );
	return failures;
}

//---------------------------------------------------------------------------
// --coreaudio: organ off never reaches earshot, nor CoreAudio.
//---------------------------------------------------------------------------
/// Images loaded in this process whose path names audio. The frameworks
/// earshot links (CoreAudio, AudioToolbox and what they pull in) load at
/// launch whatever happens; the HAL starting loads many more: on this Mac
/// earshot::listDevices() added 85 images, AudioUnit's and CoreMediaIO's
/// plug-ins and AVFAudio among them (measured 2026-11-16), and building an
/// earshot::Engine added none. So the measure is the images ADDED since main
/// began (g_audioAtStart), and listDevices is its positive control.
std::set< std::string > g_audioAtStart;

std::set< std::string > audioImages()
{
	std::set< std::string > out;
	for( uint32_t i = 0; i < _dyld_image_count(); ++i )
	{
		const std::string name = _dyld_get_image_name( i );
		if( name.find( "Audio" ) != std::string::npos || name.find( "CoreMediaIO" ) != std::string::npos )
			out.insert( name );
	}
	return out;
}

/// Threads of this process whose name says CoreAudio's client side runs:
/// listDevices() starts Apple's caulk messenger threads
/// ("caulk.messenger.shared:17", "...:high") on this Mac (measured
/// 2026-11-16, in a probe linked as fitest is: AppKit already holds every
/// audio framework, so no image is added there and the threads are the sign).
int g_audioThreadsAtStart = 0;

int audioThreads()
{
	thread_act_array_t list = nullptr;
	mach_msg_type_number_t n = 0;
	if( task_threads( mach_task_self(), &list, &n ) != KERN_SUCCESS )
		return -1;
	int count = 0;
	for( mach_msg_type_number_t i = 0; i < n; ++i )
	{
		const pthread_t t = pthread_from_mach_thread_np( list[ i ] );
		char name[ 256 ]  = {};
		if( t != nullptr )
			pthread_getname_np( t, name, sizeof( name ) );
		const std::string s = name;
		if( s.find( "caulk" ) != std::string::npos || s.find( "audio" ) != std::string::npos || s.find( "Audio" ) != std::string::npos || s.find( "HAL" ) != std::string::npos )
			++count;
		mach_port_deallocate( mach_task_self(), list[ i ] );
	}
	vm_deallocate( mach_task_self(), reinterpret_cast< vm_address_t >( list ), n * sizeof( thread_act_t ) );
	return count;
}

int runCoreAudio( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "coreaudio: the organ off, the REAL backend, every other control moved, Audio Input set, Rescan pressed: nothing reaches earshot or CoreAudio, %dx%d\n", W, H );
	const char* gate        = std::getenv( "FILAMENT_NO_DEVICES" );
	const std::string saved = gate != nullptr ? gate : "";
	unsetenv( "FILAMENT_NO_DEVICES" );
	organ::Organ::SetBackendFactoryForTest( nullptr );
	organ::Organ::negListAtConstruction = ( perturb & kNegListAtConstruction ) != 0;
	const organ::Organ::Counters& calls = organ::Organ::Calls();
	const int engines0 = calls.engines.load(), lists0 = calls.lists.load(), selects0 = calls.selects.load();
	int failures = 0;
	std::set< std::string > before, after;
	bool built = false;
	{
		Session s;
		s.fps = 60.0;
		if( !s.begin( W, H ) )
			return 1;
		s.upload( buildCard( W, H, 0 ) );
		s.renderAt( 0 );
		before = audioImages();
		long frame = 1;
		const std::vector< NamedParameter > params = listParameters( s.plugin );
		for( const NamedParameter& p : params )
		{
			if( p.index == Filament::PT_ORGAN || ( p.index >= Filament::PT_ABOUT_FIRST && p.index < Filament::PT_ABOUT_END ) )
				continue;//About buttons open a browser
			const float values[] = { p.low, p.high, 0.5f * ( p.low + p.high ) };
			for( float v : values )
			{
				s.plugin.SetFloatParameter( p.index, v );
				s.renderAt( frame++ );
			}
		}
		s.plugin.SetFloatParameter( Filament::PT_AUDIO_INPUT, earshot::deviceKey( "Test Mic" ) );
		s.plugin.SetFloatParameter( Filament::PT_AUDIO_INPUT, earshot::deviceKey( earshot::kDefaultDevice ) );
		s.plugin.SetFloatParameter( Filament::PT_RESCAN, 1.0f );
		s.plugin.SetFloatParameter( Filament::PT_RESCAN, 0.0f );
		for( int f = 0; f < 20; ++f )
			s.renderAt( frame++ );
		after = audioImages();
		built = s.plugin.OrganForTest().EngineForTest() != nullptr;
		s.end();
	}
	organ::Organ::negListAtConstruction = false;
	const int engines = calls.engines.load() - engines0, lists = calls.lists.load() - lists0, selects = calls.selects.load() - selects0;
	failures += report( engines == 0 && lists == 0 && selects == 0 && !built, quiet,
	                    "every control at both ends and the middle, Audio Input set to two device keys, Rescan pressed, frames drawn: earshot Engines built %d, device lists %d, selects %d (all must be 0)", engines, lists,
	                    selects );
	//Against the images present when main began, not when this check began:
	//run first in its own process (verify.sh does), so an image loaded by
	//anything at all in the process would show.
	auto added = [ & ]( const std::set< std::string >& now ) {
		size_t n = 0;
		for( const std::string& image : now )
			n += g_audioAtStart.count( image ) ? 0 : 1;
		return n;
	};
	const int threads = audioThreads();
	failures += report( added( before ) == 0 && added( after ) == 0 && threads == g_audioThreadsAtStart, quiet,
	                    "no audio image loaded and no audio thread started since main began (%zu images linked at launch, %zu added; %d audio threads at launch, %d now)", g_audioAtStart.size(),
	                    added( after ), g_audioThreadsAtStart, threads );
	if( !quiet )
	{
		//The probe can see it: earshot's own listing, outside the plugin.
		const std::vector< earshot::DeviceInfo > devices = earshot::listDevices();
		const size_t images                              = added( audioImages() );
		const int listedThreads                          = audioThreads();
		failures += report( images > 0 || listedThreads > threads, false, "the probe's positive control: earshot::listDevices() (%zu devices) adds %zu audio images and %d audio threads", devices.size(), images,
		                    listedThreads - threads );
	}
	if( !saved.empty() )
		setenv( "FILAMENT_NO_DEVICES", saved.c_str(), 1 );
	organ::Organ::SetBackendFactoryForTest( scriptedFactory );
	return failures;
}

//---------------------------------------------------------------------------
// --organdevices: the menu, the states and the lamp, on the scripted backend.
//---------------------------------------------------------------------------
/// An option element's value, as the host reads it (the SDK hands it back
/// as the element's "default").
float elementValue( Filament& p, unsigned int param, unsigned int element )
{
	const FFMixed mixed = p.GetParamElementDefault( param, element );
	float value         = 0.0f;
	std::memcpy( &value, &mixed.UIntValue, sizeof( value ) );
	return value;
}

int runOrganDevices( int W, int H, int perturb, bool quiet = false )
{
	if( !quiet )
		std::printf( "organ devices: the menu, the states and the lamp on a scripted backend (no audio device is touched), %dx%d\n", W, H );
	const char* gate        = std::getenv( "FILAMENT_NO_DEVICES" );
	const std::string saved = gate != nullptr ? gate : "";
	unsetenv( "FILAMENT_NO_DEVICES" );
	organ::Organ::SetBackendFactoryForTest( scriptedFactory );
	organ::Organ::negMenuByIndex = ( perturb & kNegMenuByIndex ) != 0;
	{
		std::lock_guard< std::mutex > lock( g_deviceMutex );
		g_deviceNames = { "Test Mic", "Test Silent" };
	}
	int failures = 0;
	const organ::Organ::Counters& calls = organ::Organ::Calls();
	auto session = std::make_unique< Session >();
	Session& s   = *session;
	s.fps        = 60.0;
	s.floatOutput = false;
	if( !s.begin( W, H ) )
		return 1;
	s.upload( flat( W, H, 0.0 ) );
	Filament& p = s.plugin;
	long frame  = 0;
	auto draw   = [ & ]() {
		s.renderAt( frame++ );
		return s.readBack();
	};
	//The lamp's centre, by the plugin's own placement (Filament.cpp).
	const float radius = std::max( 3.0f, 0.014f * static_cast< float >( H ) );
	const int lx = static_cast< int >( static_cast< float >( W ) - 2.2f * radius );
	const int ly = H - 1 - static_cast< int >( static_cast< float >( H ) - 2.2f * radius );//readBack is top-down
	auto lamp    = [ & ]( const std::vector< unsigned char >& img ) {
		const unsigned char* px = img.data() + ( static_cast< size_t >( ly ) * W + lx ) * 4;
		return std::vector< int >{ px[ 0 ], px[ 1 ], px[ 2 ] };
	};
	auto lampIs = [ & ]( const std::vector< int >& l, double r, double g, double b ) {
		auto enc = [ & ]( double v ) { return static_cast< int >( std::lround( 255.0 * v ) ); };
		return std::abs( l[ 0 ] - enc( r ) ) <= 2 && std::abs( l[ 1 ] - enc( g ) ) <= 2 && std::abs( l[ 2 ] - enc( b ) ) <= 2;
	};
	auto lampText = [ & ]( const std::vector< int >& l ) { return "(" + std::to_string( l[ 0 ] ) + ", " + std::to_string( l[ 1 ] ) + ", " + std::to_string( l[ 2 ] ) + ")"; };
	auto state    = [ & ]() {
		earshot::Engine* e = p.OrganForTest().EngineForTest();
		return e != nullptr ? e->captureState() : earshot::CaptureState::Closed;
	};
	auto waitFor = [ & ]( std::function< bool() > done, double seconds ) {
		const auto start = std::chrono::steady_clock::now();
		while( std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count() < seconds )
		{
			draw();
			if( done() )
				return true;
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
		return false;
	};
	auto keyOf = [ & ]( const char* label ) {
		for( unsigned int e = 0; e < p.GetNumParamElements( Filament::PT_AUDIO_INPUT ); ++e )
			if( std::string( p.GetParamElementName( Filament::PT_AUDIO_INPUT, e ) ) == label )
				return elementValue( p, Filament::PT_AUDIO_INPUT, e );
		return -1.0f;
	};
	const int lists0 = calls.lists.load(), engines0 = calls.engines.load();

	//(a) Before the first organ frame: nothing listed, nothing built.
	{
		bool ok = p.GetNumParamElements( Filament::PT_AUDIO_INPUT ) == static_cast< unsigned >( organ::kInputSlots );
		ok      = ok && std::string( p.GetParamElementName( Filament::PT_AUDIO_INPUT, 0 ) ) == "None" && elementValue( p, Filament::PT_AUDIO_INPUT, 0 ) == 0.0f &&
		     std::string( p.GetParamElementName( Filament::PT_AUDIO_INPUT, 1 ) ) == "System default" &&
		     elementValue( p, Filament::PT_AUDIO_INPUT, 1 ) == earshot::deviceKey( earshot::kDefaultDevice );
		for( int e = 2; e < organ::kInputSlots; ++e )
			ok = ok && std::string( p.GetParamElementName( Filament::PT_AUDIO_INPUT, static_cast< unsigned >( e ) ) ) == "(none)";
		draw();
		failures += report( ok && p.OrganForTest().EngineForTest() == nullptr && calls.lists.load() == lists0, quiet,
		                    "(a) organ off: the menu is None, System default and 14 (none), and nothing has listed or built" );
	}
	//(b) The first organ frame lists, once, and tells the host.
	{
		ParamEventStruct events[ Filament::PT_COUNT ];
		p.ConsumeParamEvents( events, Filament::PT_COUNT );
		set( p, "Organ", 1.0f );
		set( p, "Status Lamp", 1.0f );
		const std::vector< unsigned char > img = draw();
		const FFUInt32 n = p.ConsumeParamEvents( events, Filament::PT_COUNT );
		bool raised      = false;
		for( FFUInt32 e = 0; e < n; ++e )
			raised = raised || ( events[ e ].ParameterNumber == Filament::PT_AUDIO_INPUT && ( events[ e ].eventFlags & FF_EVENT_FLAG_ELEMENTS ) != 0 );
		const char* names[] = { "None", "System default", "Test Mic", "Test Silent" };
		const float keys[]  = { 0.0f, earshot::deviceKey( earshot::kDefaultDevice ), earshot::deviceKey( "Test Mic" ), earshot::deviceKey( "Test Silent" ) };
		bool ok             = true;
		for( unsigned int e = 0; e < p.GetNumParamElements( Filament::PT_AUDIO_INPUT ); ++e )
		{
			const std::string label = p.GetParamElementName( Filament::PT_AUDIO_INPUT, e );
			const float value       = elementValue( p, Filament::PT_AUDIO_INPUT, e );
			ok                      = ok && ( e < 4 ? label == names[ e ] && value == keys[ e ] : label == "(none)" && value == 0.0f );
		}
		failures += report( ok && raised && calls.lists.load() == lists0 + 1 && calls.engines.load() == engines0 + 1, quiet,
		                    "(b) the first organ frame lists once and raises FF_EVENT_FLAG_ELEMENTS: Test Mic and Test Silent by name, each valued by its earshot::deviceKey" );
		const auto l = lamp( img );
		failures += report( lampIs( l, 1.0, 0.62, 0.05 ), quiet, "(c) no input chosen: the lamp amber %s", lampText( l ).c_str() );
	}
	//(d) A device that delivers.
	p.SetFloatParameter( Filament::PT_AUDIO_INPUT, keyOf( "Test Mic" ) );
	{
		const bool running = waitFor( [ & ] { return state() == earshot::CaptureState::Running && p.OrganForTest().LampState() == organ::Lamp::Green; }, 5.0 );
		const auto l       = lamp( draw() );
		failures += report( running && lampIs( l, 0.15, 1.0, 0.25 ), quiet, "(d) Test Mic: %s, the lamp green %s", earshot::toString( state() ), lampText( l ).c_str() );
	}
	//(e) A device that delivers zeros.
	p.SetFloatParameter( Filament::PT_AUDIO_INPUT, keyOf( "Test Silent" ) );
	{
		const bool silent = waitFor( [ & ] { return state() == earshot::CaptureState::Silent; }, 8.0 );
		const auto l      = lamp( draw() );
		failures += report( silent && lampIs( l, 1.0, 0.05, 0.02 ), quiet, "(e) Test Silent: %s, the lamp steady red %s", earshot::toString( state() ), lampText( l ).c_str() );
	}
	//(f) A saved device that is not here: Missing, nothing in its place, the
	//lamp blinking at 1 Hz on the host's clock.
	const float gone       = earshot::deviceKey( "Gone Mic" );
	const int opensBefore  = g_opens.load();
	p.SetFloatParameter( Filament::PT_AUDIO_INPUT, gone );
	{
		const bool missing = waitFor( [ & ] { return state() == earshot::CaptureState::Missing; }, 5.0 );
		frame              = ( frame / 60 + 1 ) * 60 + 6;//0.1 s into a second: on
		const auto l1      = lamp( draw() );
		frame += 29;//0.6 s: off
		const auto l2 = lamp( draw() );
		failures += report( missing && g_opens.load() == opensBefore && lampIs( l1, 1.0, 0.03, 0.02 ) && lampIs( l2, 0.15, 0.03, 0.02 ), quiet,
		                    "(f) a saved key with no device: %s, nothing opened in its place, the lamp blinking red %s then %s", earshot::toString( state() ), lampText( l1 ).c_str(), lampText( l2 ).c_str() );
	}
	//(g) The device comes back; Rescan lists it and reopens it by name.
	{
		std::lock_guard< std::mutex > lock( g_deviceMutex );
		g_deviceNames.push_back( "Gone Mic" );
	}
	p.SetFloatParameter( Filament::PT_RESCAN, 1.0f );
	p.SetFloatParameter( Filament::PT_RESCAN, 0.0f );
	{
		const bool listed  = keyOf( "Gone Mic" ) == gone;
		const bool running = waitFor( [ & ] { return state() == earshot::CaptureState::Running; }, 5.0 );
		earshot::Status st;
		p.OrganForTest().EngineForTest()->status( st );
		failures += report( listed && running && std::string( st.opened ) == "Gone Mic", quiet, "(g) Rescan with Gone Mic connected: listed by its key, reopened by name (opened \"%s\")", st.opened );
	}
	//(h) Organ off closes the device; Rescan does nothing while off; on again
	//reopens it.
	{
		set( p, "Organ", 0.0f );
		draw();
		const bool closed = waitFor( [ & ] { return state() == earshot::CaptureState::Closed; }, 5.0 );
		const int lists   = calls.lists.load();
		p.SetFloatParameter( Filament::PT_RESCAN, 1.0f );
		p.SetFloatParameter( Filament::PT_RESCAN, 0.0f );
		draw();
		const bool still = calls.lists.load() == lists;
		set( p, "Organ", 1.0f );
		const bool back = waitFor( [ & ] { return state() == earshot::CaptureState::Running; }, 5.0 );
		failures += report( closed && still && back, quiet, "(h) organ off closes the device (%s), Rescan while off lists nothing, organ on reopens it", closed ? "Closed" : "still open" );
	}
	s.end();
	session.reset();
	//(i) A composition saved with a device: the key arrives with the organ
	//off, and the device opens on the first organ frame, by name.
	{
		Session s2;
		s2.fps = 60.0;
		if( !s2.begin( W, H ) )
			return 1;
		s2.upload( flat( W, H, 0.0 ) );
		const int sel = calls.selects.load();
		s2.plugin.SetFloatParameter( Filament::PT_AUDIO_INPUT, earshot::deviceKey( "Test Mic" ) );
		s2.renderAt( 0 );
		const bool quietOff = calls.selects.load() == sel && s2.plugin.OrganForTest().EngineForTest() == nullptr;
		set( s2.plugin, "Organ", 1.0f );
		bool running     = false;
		const auto start = std::chrono::steady_clock::now();
		for( long f = 1; !running && std::chrono::duration< double >( std::chrono::steady_clock::now() - start ).count() < 5.0; ++f )
		{
			s2.renderAt( f );
			earshot::Engine* e = s2.plugin.OrganForTest().EngineForTest();
			running            = e != nullptr && e->captureState() == earshot::CaptureState::Running;
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
		earshot::Status st;
		if( s2.plugin.OrganForTest().EngineForTest() )
			s2.plugin.OrganForTest().EngineForTest()->status( st );
		failures += report( quietOff && running && std::string( st.opened ) == "Test Mic", quiet, "(i) a saved Test Mic key: nothing selected while the organ is off, opened by name on its first frame" );
		s2.end();
	}
	//(j) FILAMENT_NO_DEVICES refuses a device name before earshot sees it.
	{
		setenv( "FILAMENT_NO_DEVICES", "1", 1 );
		Session s3;
		s3.fps = 60.0;
		if( !s3.begin( W, H ) )
			return 1;
		s3.upload( flat( W, H, 0.0 ) );
		set( s3.plugin, "Organ", 1.0f );
		s3.plugin.SetFloatParameter( Filament::PT_AUDIO_INPUT, earshot::deviceKey( "Test Mic" ) );
		const int sel = calls.selects.load();
		for( long f = 0; f < 20; ++f )
		{
			s3.renderAt( f );
			std::this_thread::sleep_for( std::chrono::milliseconds( 5 ) );
		}
		earshot::Engine* e = s3.plugin.OrganForTest().EngineForTest();
		failures += report( calls.selects.load() == sel && e != nullptr && e->captureState() == earshot::CaptureState::Closed && s3.plugin.OrganForTest().LampState() == organ::Lamp::Idle, quiet,
		                    "(j) FILAMENT_NO_DEVICES=1: a chosen device is never handed to earshot (Closed, lamp amber)" );
		s3.end();
		unsetenv( "FILAMENT_NO_DEVICES" );
	}
	organ::Organ::negMenuByIndex = false;
	if( !saved.empty() )
		setenv( "FILAMENT_NO_DEVICES", saved.c_str(), 1 );
	return failures;
}
