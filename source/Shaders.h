#pragma once

#include <string>

/**
	The six passes. Everything but the last runs at the grid's raster -- one
	texel per bulb -- so the physics costs the same at any output size.

	1. **drive** -- grid, RGBA32F. Each bulb's level: the mean of its cell of
	   the source, as code values, rgb and luma.

	2. **thermal** -- grid, RGBA32F, ping-ponged. The filament ODE over the
	   frame's seconds in `Substeps` RK4 steps, with the mains waveform
	   through the dimmer law evaluated at each stage's time. Two targets: the
	   new temperatures, and the light averaged over the substeps (a camera's
	   open shutter).

	3. **fill** -- grid. Every filament at the ambient: a cold wall, on a
	   fresh allocation.

	4. **regrid** -- grid. The old grid's temperatures onto a new grid when
	   Columns or Rows change, so the wall does not restart cold.

	5. **blur** -- grid, twice (x then y). The bloom.

	6. **output** -- to the host. Bulbs (a coil in clear glass, a lit globe in
	   frosted), halos from the eight neighbours, the bloom, exposure, the
	   sRGB encode, Mix.

	The ODE, the waveform and the colour table are one GLSL library,
	`kFilament`, compiled into the thermal and output passes. `fitest
	--dump-shaders DIR` writes out exactly the strings the plugin compiles,
	and that is what `tools/verify.sh` hands to glslc.
*/
namespace filament::shaders
{

std::string Vertex();
std::string Drive();
std::string Thermal();
std::string Fill();
std::string Regrid();
std::string Blur();
std::string Output();

} // namespace filament::shaders
