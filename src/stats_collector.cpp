#include "stats_collector.h"

#include <QTime>

stats_collector::stats_collector()
	: m_max_samples(7 * 24 * 3600)   // one week at one sample per second
{
}

void stats_collector::clear()
{
	m_samples.clear();
	m_lights.clear();
}

void stats_collector::add_sample(const stats_sample &s)
{
	if (m_samples.size() >= m_max_samples)
	{
		// Keep the memory limited: remove every second sample of the older half.
		std::vector<stats_sample> thin;
		thin.reserve(m_samples.size());
		const std::size_t half = m_samples.size() / 2;
		for (std::size_t i = 0; i < m_samples.size(); ++i)
		{
			if (i >= half || i % 2 == 0)
			{
				thin.push_back(m_samples[i]);
			}
		}
		m_samples.swap(thin);
	}
	m_samples.push_back(s);
}

void stats_collector::add_light_sample(int light_id, double time, double waiting)
{
	std::vector<std::pair<double, double>> &v = m_lights[light_id];
	if (v.size() >= m_max_samples)
	{
		v.erase(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2));
	}
	v.push_back(std::make_pair(time, waiting));
}

double stats_collector::last(metric m) const
{
	if (m_samples.empty() || static_cast<std::size_t>(m) >= m_samples.back().values.size())
	{
		return 0.0;
	}
	return m_samples.back().values[m];
}

QString stats_collector::metric_name(int m)
{
	switch (m)
	{
		case m_vehicles: return QStringLiteral("Vehicles");
		case m_pedestrians: return QStringLiteral("Pedestrians");
		case m_density: return QStringLiteral("Vehicle density");
		case m_mean_speed: return QStringLiteral("Mean speed");
		case m_stopped: return QStringLiteral("Stopped vehicles");
		case m_stops_per_vehicle: return QStringLiteral("Stops per vehicle");
		case m_mean_stop_time: return QStringLiteral("Mean stop duration");
		case m_junction_wait: return QStringLiteral("Wait at a junction");
		case m_light_wait: return QStringLiteral("Wait at traffic lights");
		case m_congested_time: return QStringLiteral("Time in congestion");
		case m_congested_distance: return QStringLiteral("Distance in congestion");
		case m_fuel_per_100km: return QStringLiteral("Mean fuel consumption");
		case m_fuel_rate: return QStringLiteral("Fuel rate (all vehicles)");
		case m_co2_rate: return QStringLiteral("CO2 emission rate");
		case m_co2_total: return QStringLiteral("CO2 emission total");
		case m_throughput: return QStringLiteral("Completed trips");
		default: return QStringLiteral("?");
	}
}

QString stats_collector::metric_unit(int m)
{
	switch (m)
	{
		case m_vehicles: return QStringLiteral("veh");
		case m_pedestrians: return QStringLiteral("ped");
		case m_density: return QStringLiteral("veh/km");
		case m_mean_speed: return QStringLiteral("km/h");
		case m_stopped: return QStringLiteral("veh");
		case m_stops_per_vehicle: return QStringLiteral("stops/veh");
		case m_mean_stop_time: return QStringLiteral("s");
		case m_junction_wait: return QStringLiteral("s");
		case m_light_wait: return QStringLiteral("s/veh");
		case m_congested_time: return QStringLiteral("s/veh");
		case m_congested_distance: return QStringLiteral("m/veh");
		case m_fuel_per_100km: return QStringLiteral("L/100 km");
		case m_fuel_rate: return QStringLiteral("L/h");
		case m_co2_rate: return QStringLiteral("kg/h");
		case m_co2_total: return QStringLiteral("kg");
		case m_throughput: return QStringLiteral("veh/h");
		default: return QString();
	}
}

QString stats_collector::metric_key(int m)
{
	static const char *keys[metric_count] =
	{
		"vehicles", "pedestrians", "density", "mean_speed", "stopped", "stops_per_vehicle",
		"mean_stop_time", "junction_wait", "light_wait", "congested_time", "congested_distance",
		"fuel_per_100km", "fuel_rate", "co2_rate", "co2_total", "throughput"
	};
	return (m >= 0 && m < metric_count) ? QString::fromLatin1(keys[m]) : QString();
}

QStringList stats_collector::metric_names()
{
	QStringList out;
	for (int i = 0; i < metric_count; ++i)
	{
		out << metric_name(i);
	}
	return out;
}

QString stats_collector::to_csv() const
{
	QString csv = QStringLiteral("time_s;clock");
	for (int i = 0; i < metric_count; ++i)
	{
		QString unit = metric_unit(i);
		unit.replace(QLatin1Char('/'), QStringLiteral("_per_")).replace(QLatin1Char(' '), QLatin1Char('_'));
		csv += QStringLiteral(";%1_%2").arg(metric_key(i), unit);
	}
	csv += QLatin1Char('\n');
	for (const stats_sample &s : m_samples)
	{
		const QTime t = QTime(0, 0).addSecs(static_cast<int>(s.clock) % 86400);
		csv += QString::number(s.time, 'f', 1) + QLatin1Char(';') + t.toString(QStringLiteral("HH:mm:ss"));
		for (double v : s.values)
		{
			csv += QLatin1Char(';') + QString::number(v, 'f', 3);
		}
		csv += QLatin1Char('\n');
	}
	return csv;
}
