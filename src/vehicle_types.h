#ifndef VEHICLE_TYPES_H
#define VEHICLE_TYPES_H

#include <QString>

// vehicle_types
// -------------
// The parameters of the vehicle types and the fuel model.
//
// Car following: Intelligent Driver Model (IDM). The values are near real
// vehicles: a sedan accelerates 0-100 km/h in about 12 s with a_max = 2.2 m/s2
// (comfortable driving), a city bus in about 25 s.
//
// Fuel: a power model. The engine power is
//   P = m*a*v + 0.5*rho*CdA*v^3 + m*g*Cr*v + P_aux   (only positive power)
// fuel (L/s) = idle + P / (efficiency * energy of fuel per liter).
// CO2 = fuel * 2.31 kg/L (gasoline) or 2.68 kg/L (diesel).

enum class vehicle_kind
{
	sedan = 0,
	bus,
	motorcycle,
	pedestrian
};

struct vehicle_params
{
	QString name;
	double length;       // m
	double width;        // m
	double max_speed;    // m/s (design speed)
	double a_max;        // m/s2, maximum acceleration (IDM a)
	double b_comf;       // m/s2, comfortable deceleration (IDM b)
	double b_max;        // m/s2, emergency deceleration
	double headway;      // s, time gap (IDM T)
	double min_gap;      // m, gap when stopped (IDM s0)
	double mass;         // kg (with the average load)
	double cda;          // m2, drag coefficient x front area
	double rolling;      // rolling resistance coefficient
	double idle_lph;     // L/h fuel at idle
	double aux_kw;       // kW for lights, air conditioning, ...
	double efficiency;   // engine and drive train efficiency
	double mj_per_liter; // energy of the fuel
	double co2_per_liter;// kg CO2 per liter
	bool diesel;
	int min_passengers;
	int max_passengers;
};

namespace vehicle_types
{
	const vehicle_params &params(vehicle_kind k);
	QString kind_name(vehicle_kind k);
	// Fuel rate in L/s for speed v (m/s) and acceleration a (m/s2).
	double fuel_rate(const vehicle_params &p, double v, double a);
}

#endif // VEHICLE_TYPES_H
