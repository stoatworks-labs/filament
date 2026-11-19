/*
    The page's way in to the colour organ: what the plugin's ProcessOpenGL
    does with `organ::Organ`, as C functions JavaScript can call. (flyback's
    and woofer's glue, cut down to the one class filament's page needs: the
    rest of the plugin's CPU half in the browser is the page's JavaScript port,
    demo/plugin.js and demo/organ.js.)

    Nothing here is the organ, and nothing here does its work. That is
    `source/Organ.cpp` -- the FIFO that lays the audio onto the host's clock,
    the 10 Hz DC block, the Linkwitz-Riley crossover, ETI 428's detectors, the
    grid of 1/16 half-cycles and the control rows -- with its control laws
    (CrossoverLowHertz, CrossoverHighHertz, GainDb), and earshot's snapshot
    under it (8124191, unpatched: its feed(), its 32 kHz waveform tap, its
    capture state), compiled UNMODIFIED into the same WebAssembly module as
    this file (the list is in demo/tools/build-wasm.sh). This file only plays
    the plugin's part:

      - `fo_new` constructs an Organ, as Filament's constructor does;
      - `fo_activate` / `fo_deactivate` are what ProcessOpenGL does with the
        Organ switch (`organ->Activate()` on an organ frame,
        `organ->Deactivate()` on any other);
      - `fo_advance` is ProcessOpenGL's `organ->Advance( start, now, hertz,
        firstHalf, halves, settings )`, with `settings` filled from the host's
        0..1 values by the plugin's own laws, as ProcessOpenGL fills it.

    One thing here is the plugin's HARNESS rather than the plugin: the audio.
    fitest never opens a device. It hands the organ its music through
    earshot's feed() (`Organ::FeedBegin` and `Organ::Feed`, which are
    `Engine::feedBegin` and `Engine::feed`, the path a device's samples take
    inside earshot), before each frame, up to the frame's end plus 10 ms. So
    does this page. `noiseAt`, `trackAt`, `Wav`, `kFeedLead` and `Audio` below
    are tools/fitest/Organ.inc's own, copied character for character (and held
    to it by demo/tools/check_shaders.py): the synthetic 120 BPM track, fitest
    --silent's zeros, and a file the visitor chose, which the page decodes and
    hands over as fitest --wav hands a WAV over (looped).

    No audio device exists in this module: earshot is built with
    EARSHOT_CAPTURE=OFF (src/NoCapture.cpp; miniaudio is not compiled, and
    miniaudio.h is never included), so there is no backend to list or open.
    `fo_prepare` sets FILAMENT_NO_DEVICES=1, verify.sh's switch, so the organ
    never asks earshot for a device list and never hands it a name; nothing
    here calls Organ::Want, so the Audio Input it applies is None, and
    `fo_counters` hands the page Organ.cpp's own count of selectDevice calls.
*/
#include "Lamp.h"
#include "Organ.h"

#include <earshot/earshot.h>
#include <emscripten/emscripten.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

namespace
{
namespace lamp  = filament::lamp;
namespace organ = filament::organ;

//---------------------------------------------------------------------------
// tools/fitest/Organ.inc's music and feeder, verbatim (check_shaders.py
// compares each piece).
//---------------------------------------------------------------------------
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
// One organ and what it hears.
//---------------------------------------------------------------------------
struct Instance
{
	organ::Organ organ;
	Audio audio;
	bool feeding = false;///< the page's None: nothing is fed, as with no Audio Input
	double stats[ 8 ] = {};
	int counters[ 3 ] = {};
};
} // namespace

extern "C"
{
/// Before the first organ: FILAMENT_NO_DEVICES=1, verify.sh's switch (and the
/// harness's for oxbow), so Organ::List never asks earshot for the devices and
/// Organ::Apply never hands it a name. A build with no capture backend could
/// open nothing anyway; this keeps the organ from asking.
EMSCRIPTEN_KEEPALIVE void fo_prepare()
{
	setenv( "FILAMENT_NO_DEVICES", "1", 1 );
}

/// The Organ Filament's constructor makes: a menu of None, System default and
/// spares, no Engine, no device list.
EMSCRIPTEN_KEEPALIVE Instance* fo_new()
{
	return new Instance();
}

EMSCRIPTEN_KEEPALIVE void fo_delete( Instance* instance )
{
	delete instance;
}

/// What the harness will feed: 0 nothing at all (the page's None: no Audio
/// Input, nothing reaches earshot), 1 the synthetic track (fitest's default),
/// 2 zeros (fitest --silent), 3 the file handed to fo_wav (fitest --wav).
/// Set once, before the first feed; the page makes a new Instance to change it.
EMSCRIPTEN_KEEPALIVE void fo_source( Instance* instance, int source )
{
	instance->feeding = source != 0;
	instance->audio.kind = source == 2 ? Audio::Kind::Silent : source == 3 ? Audio::Kind::Wav : Audio::Kind::Track;
}

/// A file the page decoded: `frames` interleaved frames of `channels` at
/// `rate`, kept as fitest's readWav keeps a WAV.
EMSCRIPTEN_KEEPALIVE void fo_wav( Instance* instance, const float* interleaved, int frames, int rate, int channels )
{
	Wav& wav     = instance->audio.wav;
	wav.rate     = rate;
	wav.channels = channels;
	wav.samples.assign( interleaved, interleaved + static_cast< size_t >( frames ) * static_cast< size_t >( channels ) );
}

/// fitest's feedOrgan, for a frame of host time [start, end): with the Organ
/// On, the audio up to the frame's end plus kFeedLead (`feedFrame`); with it
/// Off, `off()`, so the audio carries on where it stopped when it comes back.
EMSCRIPTEN_KEEPALIVE void fo_feed_frame( Instance* instance, double start, double end )
{
	if( instance->feeding )
		instance->audio.feedFrame( instance->organ, start, end );
}

EMSCRIPTEN_KEEPALIVE void fo_feed_off( Instance* instance )
{
	instance->audio.off();
}

EMSCRIPTEN_KEEPALIVE void fo_activate( Instance* instance )
{
	instance->organ.Activate();
}

EMSCRIPTEN_KEEPALIVE void fo_deactivate( Instance* instance )
{
	instance->organ.Deactivate();
}

/// ProcessOpenGL's organ block: Settings from the host's values through
/// Organ.cpp's laws (Perturb 0, as the shipped plugin), then Advance. The
/// half-cycle number arrives as a double (exact below 2^53).
EMSCRIPTEN_KEEPALIVE void fo_advance( Instance* instance, double start, double end, double hertz, double firstHalf, int rows, float low, float high,
                                      float bass, float mid, float treble )
{
	organ::Settings settings;
	settings.lowHertz    = organ::CrossoverLowHertz( low );
	settings.highHertz   = organ::CrossoverHighHertz( high );
	settings.gainDb[ 0 ] = organ::GainDb( bass );
	settings.gainDb[ 1 ] = organ::GainDb( mid );
	settings.gainDb[ 2 ] = organ::GainDb( treble );
	settings.perturb     = 0;
	instance->organ.Advance( start, end, hertz, static_cast< int64_t >( firstHalf ), rows, settings );
}

/// The last Advance's rows: RowCount() x kRowPoints texels of RGBA float, the
/// texture the organ's thermal pass searches.
EMSCRIPTEN_KEEPALIVE const float* fo_rows( Instance* instance )
{
	return instance->organ.Rows();
}

EMSCRIPTEN_KEEPALIVE int fo_row_count( Instance* instance )
{
	return instance->organ.RowCount();
}

EMSCRIPTEN_KEEPALIVE double fo_first_half( Instance* instance )
{
	return static_cast< double >( instance->organ.FirstHalf() );
}

/// organ::Lamp: 0 Idle (amber), 1 Green, 2 Red (steady), 3 RedBlink.
EMSCRIPTEN_KEEPALIVE int fo_lamp( Instance* instance )
{
	return static_cast< int >( instance->organ.LampState() );
}

/// What the line under the picture reports: seconds fed, the FIFO's
/// underruns and drops (samples), the tap's rate, earshot's capture state.
EMSCRIPTEN_KEEPALIVE const double* fo_stats( Instance* instance )
{
	const Audio& a      = instance->audio;
	instance->stats[ 0 ] = a.begun ? static_cast< double >( a.fed ) / a.rateNow() : 0.0;
	instance->stats[ 1 ] = static_cast< double >( instance->organ.UnderrunsForTest() );
	instance->stats[ 2 ] = static_cast< double >( instance->organ.DroppedForTest() );
	instance->stats[ 3 ] = static_cast< double >( instance->organ.TapRateForTest() );
	earshot::Engine* engine = instance->organ.EngineForTest();
	instance->stats[ 4 ] = engine != nullptr ? static_cast< double >( static_cast< int >( engine->captureState() ) ) : -1.0;
	instance->stats[ 5 ] = instance->organ.Active() ? 1.0 : 0.0;
	return instance->stats;
}

/// Organ::Calls(), process-wide: Engines built, device lists made, and
/// selectDevice calls.
EMSCRIPTEN_KEEPALIVE const int* fo_counters( Instance* instance )
{
	organ::Organ::Counters& c = organ::Organ::Calls();
	instance->counters[ 0 ]   = c.engines.load();
	instance->counters[ 1 ]   = c.lists.load();
	instance->counters[ 2 ]   = c.selects.load();
	return instance->counters;
}

/// Organ.cpp's laws, for the cross-check against the page's readouts: 0
/// CrossoverLowHertz, 1 CrossoverHighHertz, 2 GainDb; and their inverses.
EMSCRIPTEN_KEEPALIVE double fo_law( int which, float value )
{
	return which == 0 ? organ::CrossoverLowHertz( value ) : which == 1 ? organ::CrossoverHighHertz( value ) : organ::GainDb( value );
}

EMSCRIPTEN_KEEPALIVE float fo_law_inverse( int which, double value )
{
	return which == 0 ? organ::CrossoverLowParam( value ) : which == 1 ? organ::CrossoverHighParam( value ) : organ::GainParam( value );
}
} // extern "C"
