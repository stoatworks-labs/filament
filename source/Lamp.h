#pragma once

/**
	A tungsten filament lamp as numbers, in double, for the CPU.

	**The ODE.** Per filament, per channel:

	    C( T ) dT/dt = V( t )^2 / R( T ) - e s A ( T^4 - T0^4 ) - k ( T - T0 )

	Every lamp here is a filament designed to dissipate `watts` at `volts`
	RMS with its wire at kRatedKelvin in an ambient of kReferenceAmbient,
	with kConductionFraction of that power leaving by conduction (the lead-in
	wires, the supports, the fill gas) and the rest by radiation. That design
	point fixes both loss terms outright:

	    e s A = ( 1 - fc ) P / ( Tr^4 - Tref^4 ),   k = fc P / ( Tr - Tref )

	and R( T ) = R( Tr ) rho( T ) / rho( Tr ), with R( Tr ) = V^2 / P. So the
	right-hand side divided by P depends only on T, the drive and the ambient;
	the lamp's size enters through C( T ) / P alone.

	**C( T ) from the wire.** A straight wire of diameter d and length L that
	radiates from a fraction kCoilRadiatingFraction of its surface (a coiled
	coil shades itself), at emissivity kEmissivity, and has resistance V^2/P
	hot, is fixed by two equations:

	    rho( Tr ) L / ( pi d^2 / 4 ) = V^2 / P
	    kappa e s pi d L ( Tr^4 - Tref^4 ) = ( 1 - fc ) P

	so d^3 = 4 rho P ( 1 - fc ) P / ( kappa e s pi^2 V^2 ( Tr^4 - Tref^4 ) ).
	Its mass is density x pi d^2 / 4 x L and C( T ) = mass x c_p( T ), with c_p
	from the NIST-JANAF Shomate fit. Mass goes as d P, and d as ( P / V )^(2/3),
	so C / P -- the thermal time scale -- goes as ( P / V )^(2/3): a big lamp
	is slow, a small lamp is quick, and a 120 V lamp is slower than a 230 V
	lamp of the same wattage. None of that is tuned; the three stated
	constants (emissivity, radiating fraction, conduction fraction) are the
	assumptions, and AGENTS.md says so.

	**rho( T )** is the Forsythe and Worthing (1925) table, 300 to 3600 K by
	100 K, as reproduced by the CRC Handbook; linear between points and
	extrapolated linearly from the end segments. The shader carries the same
	34 numbers and the same rule.
*/
namespace filament::lamp
{

constexpr double kPi = 3.14159265358979323846;

/// The design point.
constexpr double kRatedKelvin        = 2800.0;
constexpr double kReferenceAmbient   = 298.15;
constexpr double kConductionFraction = 0.15;

/// Only used to size the wire, and so the heat capacity.
constexpr double kEmissivity            = 0.30;
constexpr double kCoilRadiatingFraction = 0.5;
constexpr double kStefanBoltzmann       = 5.670374419e-8;
constexpr double kTungstenDensity       = 19250.0; ///< kg / m^3
constexpr double kTungstenMolarMass     = 0.18384; ///< kg / mol
constexpr double kMeltingKelvin         = 3695.0;

/// Forsythe and Worthing (1925), resistivity of tungsten in micro-ohm cm at
/// 300, 400, ... 3600 K.
constexpr int kRhoCount        = 34;
constexpr double kRhoFirst     = 300.0;
constexpr double kRhoStep      = 100.0;
constexpr double kRho[ kRhoCount ] = {
	5.65, 8.06, 10.56, 13.23, 16.09, 19.00, 21.94, 24.93, 27.94, 30.98,
	34.08, 37.19, 40.36, 43.55, 46.78, 50.05, 53.35, 56.67, 60.06, 63.48,
	66.91, 70.39, 73.91, 77.49, 81.04, 84.70, 88.33, 92.04, 95.76, 99.54,
	103.3, 107.2, 111.1, 115.0,
};

/// NIST-JANAF (Chase 1998) Shomate coefficients for solid tungsten, c_p in
/// J / ( mol K ) with t = T / 1000: A + B t + C t^2 + D t^3 + E / t^2.
constexpr double kShomateLow[ 5 ]  = { 23.95930, 2.639680, 1.257750, -0.254642, -0.048407 };  ///< 298-1900 K
constexpr double kShomateHigh[ 5 ] = { -22.57640, 90.27980, -44.27150, 7.176630, -24.09740 }; ///< 1900-3680 K
constexpr double kShomateSplit     = 1900.0;

/// Dimmer laws, by menu index.
enum Law : int
{
	kLinear    = 0, ///< V_rms = level x V, a sine-wave dimmer
	kSquareLaw = 1, ///< V_rms^2 = level x V^2: electrical power roughly follows the level
	kTriac     = 2, ///< leading-edge phase cut, firing angle = ( 1 - level ) x pi
};

/// Negative-control bits, shared with Model.h and the shader.
enum Perturb : int
{
	kPerturbConstantR    = 1,  ///< R held at R( Tr ): --rise must fail
	kPerturbLinearCool   = 2,  ///< radiation as a linear term matched at Tr: --fall must fail
	kPerturbResizeClears = 4,  ///< a resize or grid change restarts the filaments cold: --resize must fail
	kPerturbDC           = 8,  ///< drive by the RMS as DC, no mains waveform: --ripple must fail
	kPerturbNoConduction = 16, ///< drop the conduction term: --steady must fail
	kPerturbTableShift   = 32, ///< read the colour table 5% hot: --colour must fail
};

double Resistivity( double kelvin );
double HeatCapacity( double kelvin ); ///< J / ( kg K )

/// Mains voltage for a mains frequency: 50 Hz is a 230 V lamp, 60 Hz a 120 V one.
double MainsVolts( double hertz );

struct Lamp
{
	double watts   = 60.0;
	double volts   = 230.0;
	double hertz   = 50.0;
	double diameter = 0.0; ///< m
	double length   = 0.0; ///< m
	double mass     = 0.0; ///< kg
	double wattsPerKg = 0.0; ///< P / m: the whole lamp's size, as the ODE sees it
};

Lamp Make( double watts, double hertz );

/// ( v( t ) / V )^2, the instantaneous drive, for a level 0..1 through a law,
/// at a time `cycles` measured in mains cycles. The waveform the plugin runs.
double DriveSquared( double cycles, int law, double level, int perturb );

/// dT/dt in K/s.
double Derivative( const Lamp& lamp, double kelvin, double driveSquared, double ambient, int perturb );

/// The temperature where power in balances power out for a mean drive^2 --
/// the steady state the ODE settles to on DC of that RMS. Bisection.
double SteadyKelvin( double meanDriveSquared, double ambient, int perturb );

/// The stiffest |d(dT/dt)/dT| the lamp can present, over every temperature
/// from `ambient` to melting at the peak of the mains cycle. Sets the substep.
double Stiffness( const Lamp& lamp, double ambient );

/// Substeps for a frame of `seconds`: at least kStepsPerHalfCycle per mains
/// half-cycle, and never more than kStability / Stiffness long.
constexpr int kStepsPerHalfCycle = 20;
constexpr double kStability      = 1.5;
constexpr int kMaxSubsteps       = 4096;
int Substeps( const Lamp& lamp, double ambient, double seconds );

/// sin( pi x ) for x in [0, 1], as the shader computes it: folded to
/// [0, 1/2] and a Taylor series to x^11 -- plain arithmetic, the same on every
/// driver, where GLSL's sin carries no accuracy requirement at all.
double SinPi( double x );

} // namespace filament::lamp
