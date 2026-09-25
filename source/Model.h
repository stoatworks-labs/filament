#pragma once

#include "Lamp.h"

/**
	The picture's numbers: how light becomes pixels, and the test hooks. The
	lamp itself is in Lamp.h; the colour of a temperature in PlanckTable.h
	(generated). Everything here is presentation and is judged by eye, which
	AGENTS.md says.
*/
namespace filament::model
{

/// What a frame is worth in seconds when the host's clock has not moved yet.
constexpr double kNominalFrame  = 1.0 / 60.0;
/// A host that stalls for longer than this is integrated for this long and
/// no longer: the cost of a frame is bounded, and a quarter of a second is
/// longer than any filament here takes to go dark.
constexpr double kMaxFrameDelta = 0.25;

/// Scene-referred light to display: 1 - exp( -kExposureGain x 2^stops x L ),
/// per channel, where L is linear sRGB with a filament at 2856 K reading
/// luminance 1. The gain puts a frosted bulb at its rated temperature just
/// under white at Exposure 0.
constexpr float kExposureGain = 2.2f;

/// The wall between the bulbs, linear. Nearly black: a painted board.
constexpr float kWall[ 3 ] = { 0.0016f, 0.0015f, 0.0014f };

/// What an unlit bulb's glass gives back of the room: a faint disc, so a
/// dark wall still reads as a wall of bulbs.
constexpr float kGlassSheen = 0.005f;

/// The three gels of an RGB wall, linear transmittance, by eye (AGENTS.md).
constexpr float kGelRed[ 3 ]   = { 1.0f, 0.04f, 0.03f };
constexpr float kGelGreen[ 3 ] = { 0.06f, 0.80f, 0.12f };
constexpr float kGelBlue[ 3 ]  = { 0.04f, 0.20f, 1.0f };

/// The bloom's width in cells (a Gaussian's sigma at the grid's raster).
constexpr float kBloomSigmaCells = 1.8f;

/// Negative-control bits: see lamp::Perturb. Always 0 in the plugin.
using lamp::Perturb;

/// Probe hooks for the harness, 0 in the plugin: the output pass writes raw
/// floats for the cell under each pixel instead of the picture.
enum Probe : int
{
	kProbeNone     = 0,
	kProbeState    = 1, ///< ( T red-or-only, T green, T blue, drive luma )
	kProbeInstant  = 2, ///< the filament's linear sRGB now, no glass, no shape
	kProbeShutter  = 3, ///< the same, averaged over the frame's substeps
};

} // namespace filament::model
