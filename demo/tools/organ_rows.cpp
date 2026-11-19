/*
    The colour organ's rows from a NATIVE build of the C++ the page's
    WebAssembly is built from, for demo/tools/crosscheck.py. Not served
    (demo/.assetsignore), not part of the plugin.

    crosscheck.py compiles this with the Mac's clang at the plugin's Release
    flags (-std=c++17 -O3 -DNDEBUG), beside the same files build-wasm.sh hands
    em++ -- source/Organ.cpp and Diag.cpp, earshot's sources with
    EARSHOT_CAPTURE=OFF's NoCapture.cpp, and demo/wasm/glue.cpp itself (an
    emscripten.h stub turns EMSCRIPTEN_KEEPALIVE into nothing) -- and replays
    the page's organ frames through it, call for call: the same source, the
    same feed, the same Activate and Advance with the same arguments. So the
    only difference between its rows and the page's is the compiler and the
    machine: arm64 clang with Apple's libm, against WebAssembly with musl's.

    stdin:  "source N" (glue.cpp's fo_source), then one line per organ frame:
            "start now hertz firstHalf halves low high bass mid treble"
            (doubles as %.17g, the five host values as %.9g floats).
    stdout: per frame, "rows R firstHalf F lamp L", then the R x 17 x 4 floats
            of the rows as their IEEE bits in hex, one row per line.
*/
#include <cstdint>
#include <cstdio>
#include <cstring>

// glue.cpp's Instance is file-local; its C functions take it as an opaque
// pointer, and so does this.
extern "C"
{
void fo_prepare();
void* fo_new();
void fo_source( void* instance, int source );
void fo_feed_frame( void* instance, double start, double end );
void fo_activate( void* instance );
void fo_advance( void* instance, double start, double end, double hertz, double firstHalf, int rows, float low, float high, float bass,
                 float mid, float treble );
const float* fo_rows( void* instance );
int fo_row_count( void* instance );
double fo_first_half( void* instance );
int fo_lamp( void* instance );
}

int main()
{
	fo_prepare();
	void* organ = fo_new();
	int source  = 1;
	if( std::scanf( " source %d", &source ) != 1 )
	{
		std::fprintf( stderr, "expected 'source N' first\n" );
		return 2;
	}
	fo_source( organ, source );
	double start, now, hertz, firstHalf;
	int halves;
	float low, high, bass, mid, treble;
	while( std::scanf( "%lf %lf %lf %lf %d %f %f %f %f %f", &start, &now, &hertz, &firstHalf, &halves, &low, &high, &bass, &mid, &treble ) == 10 )
	{
		fo_feed_frame( organ, start, now );
		fo_activate( organ );
		fo_advance( organ, start, now, hertz, firstHalf, halves, low, high, bass, mid, treble );
		const int rows = fo_row_count( organ );
		std::printf( "rows %d firstHalf %.17g lamp %d\n", rows, fo_first_half( organ ), fo_lamp( organ ) );
		const float* data = fo_rows( organ );
		for( int r = 0; r < rows; ++r )
		{
			for( int k = 0; k < 17 * 4; ++k )
			{
				uint32_t bits;
				std::memcpy( &bits, &data[ r * 17 * 4 + k ], sizeof bits );
				std::printf( k ? " %08x" : "%08x", bits );
			}
			std::printf( "\n" );
		}
	}
	return 0;
}
