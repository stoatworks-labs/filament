#include "Lamp.h"

#include <algorithm>
#include <cmath>

namespace filament::lamp
{

double Resistivity( double kelvin )
{
	//The shader's rule exactly: the segment index clamped to the table, the
	//fraction not, so the end segments extrapolate.
	const double x = ( kelvin - kRhoFirst ) / kRhoStep;
	const int i    = std::clamp( static_cast< int >( std::floor( x ) ), 0, kRhoCount - 2 );
	const double t = x - i;
	return kRho[ i ] + ( kRho[ i + 1 ] - kRho[ i ] ) * t;
}

double HeatCapacity( double kelvin )
{
	const double* a = kelvin < kShomateSplit ? kShomateLow : kShomateHigh;
	const double t  = kelvin / 1000.0;
	const double perMole = a[ 0 ] + a[ 1 ] * t + a[ 2 ] * t * t + a[ 3 ] * t * t * t + a[ 4 ] / ( t * t );
	return perMole / kTungstenMolarMass;
}

double MainsVolts( double hertz )
{
	return hertz > 55.0 ? 120.0 : 230.0;
}

Lamp Make( double watts, double hertz )
{
	Lamp lamp;
	lamp.watts = watts;
	lamp.hertz = hertz;
	lamp.volts = MainsVolts( hertz );

	const double rhoRated   = Resistivity( kRatedKelvin ) * 1e-8;//ohm m
	const double radiated   = ( 1.0 - kConductionFraction ) * watts;
	const double fourth     = std::pow( kRatedKelvin, 4.0 ) - std::pow( kReferenceAmbient, 4.0 );
	const double d3         = 4.0 * rhoRated * radiated * watts
	                  / ( kCoilRadiatingFraction * kEmissivity * kStefanBoltzmann * kPi * kPi * lamp.volts * lamp.volts * fourth );
	lamp.diameter   = std::cbrt( d3 );
	lamp.length     = radiated / ( kCoilRadiatingFraction * kEmissivity * kStefanBoltzmann * kPi * lamp.diameter * fourth );
	lamp.mass       = kTungstenDensity * kPi * lamp.diameter * lamp.diameter / 4.0 * lamp.length;
	lamp.wattsPerKg = watts / lamp.mass;
	return lamp;
}

double SinPi( double x )
{
	x              = std::min( x, 1.0 - x );//sin( pi x ) = sin( pi ( 1 - x ) )
	const double u = kPi * x;
	const double u2 = u * u;
	//u - u^3/3! + u^5/5! - u^7/7! + u^9/9! - u^11/11!, Horner.
	return u * ( 1.0 + u2 * ( -1.0 / 6.0 + u2 * ( 1.0 / 120.0 + u2 * ( -1.0 / 5040.0 + u2 * ( 1.0 / 362880.0 + u2 * ( -1.0 / 39916800.0 ) ) ) ) ) );
}

double DriveSquared( double cycles, int law, double level, int perturb )
{
	level = std::clamp( level, 0.0, 1.0 );
	double amplitude = level, gate = 0.0;
	if( law == kSquareLaw )
		amplitude = std::sqrt( level );
	else if( law == kTriac )
	{
		amplitude = 1.0;
		gate      = 1.0 - level;
	}
	if( perturb & kPerturbDC )
	{
		if( law == kTriac )
			return 1.0 - gate + std::sin( 2.0 * kPi * gate ) / ( 2.0 * kPi );
		return amplitude * amplitude;
	}
	//The half-cycle: v^2 has period one half mains cycle.
	const double half = 2.0 * cycles - std::floor( 2.0 * cycles );
	if( half < gate )
		return 0.0;
	const double s = SinPi( half );
	return 2.0 * amplitude * amplitude * s * s;
}

double Derivative( const Lamp& lamp, double kelvin, double driveSquared, double ambient, int perturb )
{
	const double T        = kelvin;
	const double rhoRated = Resistivity( kRatedKelvin );
	const double rho      = ( perturb & kPerturbConstantR ) ? rhoRated : Resistivity( T );
	const double in       = driveSquared * rhoRated / rho;

	double radiated;
	if( perturb & kPerturbLinearCool )
		radiated = ( 1.0 - kConductionFraction ) * ( T - ambient ) / ( kRatedKelvin - kReferenceAmbient );
	else
	{
		const double T2 = T * T, A2 = ambient * ambient;
		radiated = ( 1.0 - kConductionFraction ) * ( T2 * T2 - A2 * A2 ) / ( std::pow( kRatedKelvin, 4.0 ) - std::pow( kReferenceAmbient, 4.0 ) );
	}
	const double conducted = ( perturb & kPerturbNoConduction ) ? 0.0 : kConductionFraction * ( T - ambient ) / ( kRatedKelvin - kReferenceAmbient );
	return lamp.wattsPerKg * ( in - radiated - conducted ) / HeatCapacity( T );
}

double SteadyKelvin( double meanDriveSquared, double ambient, int perturb )
{
	//Power balance only: C does not matter, so any lamp will do.
	const Lamp any = Make( 60.0, 50.0 );
	double lo = ambient, hi = kMeltingKelvin + 500.0;
	if( meanDriveSquared <= 0.0 )
		return ambient;
	for( int i = 0; i < 200; ++i )
	{
		const double mid = 0.5 * ( lo + hi );
		if( Derivative( any, mid, meanDriveSquared, ambient, perturb ) > 0.0 )
			lo = mid;
		else
			hi = mid;
	}
	return 0.5 * ( lo + hi );
}

double Stiffness( const Lamp& lamp, double ambient )
{
	double worst = 0.0;
	for( double T = std::max( 150.0, ambient ); T <= kMeltingKelvin; T += 5.0 )
	{
		const double e = 0.5;
		const double slope = ( Derivative( lamp, T + e, 2.0, ambient, 0 ) - Derivative( lamp, T - e, 2.0, ambient, 0 ) ) / ( 2.0 * e );
		worst = std::max( worst, std::fabs( slope ) );
		const double slope0 = ( Derivative( lamp, T + e, 0.0, ambient, 0 ) - Derivative( lamp, T - e, 0.0, ambient, 0 ) ) / ( 2.0 * e );
		worst = std::max( worst, std::fabs( slope0 ) );
	}
	return worst;
}

int Substeps( const Lamp& lamp, double ambient, double seconds )
{
	if( seconds <= 0.0 )
		return 0;
	const double halfCycle = 0.5 / lamp.hertz;
	const double hMax      = std::min( halfCycle / kStepsPerHalfCycle, kStability / Stiffness( lamp, ambient ) );
	const int n            = static_cast< int >( std::ceil( seconds / hMax - 1e-9 ) );
	return std::clamp( n, 1, kMaxSubsteps );
}

} // namespace filament::lamp
