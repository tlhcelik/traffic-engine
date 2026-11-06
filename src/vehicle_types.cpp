#include "vehicle_types.h"

#include <algorithm>

namespace
{
	const vehicle_params g_sedan =
	{
		QStringLiteral("Sedan"),
		4.6, 1.8, 50.0,
		2.2, 3.0, 8.0, 1.4, 2.0,
		1450.0, 0.66, 0.012,
		0.7, 1.0, 0.22, 34.2, 2.31, false,
		1, 5
	};

	const vehicle_params g_bus =
	{
		QStringLiteral("Bus"),
		12.0, 2.55, 25.0,
		1.1, 2.0, 6.0, 1.8, 3.0,
		14500.0, 6.0, 0.008,
		2.2, 6.0, 0.34, 35.8, 2.68, true,
		8, 80
	};

	const vehicle_params g_motorcycle =
	{
		QStringLiteral("Motorcycle"),
		2.2, 0.8, 55.0,
		3.2, 3.5, 9.0, 1.1, 1.6,
		260.0, 0.55, 0.020,
		0.35, 0.2, 0.20, 34.2, 2.31, false,
		1, 2
	};

	const vehicle_params g_pedestrian =
	{
		QStringLiteral("Pedestrian"),
		0.5, 0.6, 1.6,
		1.0, 1.5, 3.0, 0.6, 0.4,
		75.0, 0.0, 0.0,
		0.0, 0.0, 1.0, 1.0, 0.0, false,
		1, 1
	};
}

namespace vehicle_types
{
	const vehicle_params &params(vehicle_kind k)
	{
		switch (k)
		{
			case vehicle_kind::sedan: return g_sedan;
			case vehicle_kind::bus: return g_bus;
			case vehicle_kind::motorcycle: return g_motorcycle;
			case vehicle_kind::pedestrian: return g_pedestrian;
		}
		return g_sedan;
	}

	QString kind_name(vehicle_kind k)
	{
		return params(k).name;
	}

	double fuel_rate(const vehicle_params &p, double v, double a)
	{
		if (p.mj_per_liter <= 1.0)
		{
			return 0.0;
		}
		const double rho = 1.2;
		const double g = 9.81;
		const double idle = p.idle_lph / 3600.0;
		double power = p.mass * a * v + 0.5 * rho * p.cda * v * v * v + p.mass * g * p.rolling * v;
		if (v > 0.5)
		{
			power += p.aux_kw * 1000.0;
		}
		// Braking or coasting: the engine runs at idle (fuel cut-off is not modeled).
		power = std::max(0.0, power);
		return idle + power / (p.efficiency * p.mj_per_liter * 1.0e6);
	}
}
