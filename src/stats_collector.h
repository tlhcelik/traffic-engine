#ifndef STATS_COLLECTOR_H
#define STATS_COLLECTOR_H

#include <QString>
#include <QStringList>

#include <map>
#include <vector>

// stats_collector
// ---------------
// Time series of the simulation for the charts. The simulation writes one
// sample at each interval (simulation time, default 1 s).
//
// Metrics (the charts use the index):
//   vehicles            active vehicles
//   pedestrians         active pedestrians
//   density             vehicles per km of lane
//   mean_speed          km/h, mean of the active vehicles
//   stopped             vehicles that do not move (< 1 km/h)
//   stops_per_vehicle   stop-and-go count per vehicle (all vehicles of the run)
//   mean_stop_time      s, mean duration of one stop
//   junction_wait       s, mean waiting time for one junction (last 5 minutes)
//   light_wait          s, mean waiting time at traffic lights per vehicle
//   congested_time      s, time in slow traffic (< 10 km/h) per vehicle
//   congested_distance  m, distance in slow traffic per vehicle
//   fuel_per_100km      L/100 km, all vehicles of the run
//   fuel_rate           L/h, all active vehicles together
//   co2_rate            kg/h, all active vehicles together
//   co2_total           kg, all vehicles of the run
//   throughput          vehicles per hour that completed their trip (last 5 minutes)
//
// For each traffic light, the collector also keeps the number of waiting vehicles.

struct stats_sample
{
	double time = 0.0;     // s since the start of the run
	double clock = 0.0;    // s after midnight
	std::vector<double> values;   // one value per metric
};

class stats_collector
{
public:
	enum metric
	{
		m_vehicles = 0,
		m_pedestrians,
		m_density,
		m_mean_speed,
		m_stopped,
		m_stops_per_vehicle,
		m_mean_stop_time,
		m_junction_wait,
		m_light_wait,
		m_congested_time,
		m_congested_distance,
		m_fuel_per_100km,
		m_fuel_rate,
		m_co2_rate,
		m_co2_total,
		m_throughput,
		metric_count
	};

	stats_collector();

	void clear();
	void add_sample(const stats_sample &s);
	void add_light_sample(int light_id, double time, double waiting);

	const std::vector<stats_sample> &samples() const { return m_samples; }
	const std::map<int, std::vector<std::pair<double, double>>> &light_samples() const { return m_lights; }
	double last(metric m) const;

	static QString metric_name(int m);
	static QString metric_unit(int m);
	static QString metric_key(int m);
	static QStringList metric_names();

	// CSV of all samples (time, clock, metrics). Separator: ";".
	QString to_csv() const;

private:
	std::vector<stats_sample> m_samples;
	std::map<int, std::vector<std::pair<double, double>>> m_lights;
	std::size_t m_max_samples;
};

#endif // STATS_COLLECTOR_H
