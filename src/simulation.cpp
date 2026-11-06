#include "simulation.h"
#include "road_network.h"
#include "world_model.h"
#include "logger.h"

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{
	const double inf = 1.0e9;
	const double yield_time = 4.0;          // s: a vehicle with priority nearer than this: wait
	const double patience_time = 40.0;      // s at the line: after this time the vehicle goes first
	const double lc_gap = 2.0;              // m: free space in front and behind for a lane change
	const double long_vehicle = 8.0;        // m: a longer vehicle (bus) uses the wide conflicts

	const double reroute_time = 8.0;        // s at the end of a lane without a movement
	const double junction_window = 300.0;   // s for "wait at a junction" and "completed trips"

	int key_road(int key) { return key / 2; }
	int key_dir(int key) { return (key % 2 == 0) ? 1 : -1; }
	double sq(double x) { return x * x; }
}

cause_target target_of(wait_cause c)
{
	switch (c)
	{
		case wait_cause::queue:
		case wait_cause::give_way:
		case wait_cause::conflict:
		case wait_cause::exit_full:
		case wait_cause::junction_blocked:
		case wait_cause::merge_courtesy:
			return cause_target::vehicle;
		case wait_cause::red_light: return cause_target::light;
		case wait_cause::stop_sign: return cause_target::sign;
		case wait_cause::pedestrians: return cause_target::pedestrian;
		case wait_cause::bus_stop:
		case wait_cause::level_crossing:
		case wait_cause::lane_blocked:
			return cause_target::object;
		default: return cause_target::none;
	}
}

const char *wait_cause_name(wait_cause c)
{
	switch (c)
	{
		case wait_cause::queue: return "queue";
		case wait_cause::red_light: return "red light";
		case wait_cause::stop_sign: return "stop sign";
		case wait_cause::give_way: return "give way";
		case wait_cause::conflict: return "conflict";
		case wait_cause::exit_full: return "exit full";
		case wait_cause::junction_blocked: return "junction blocked";
		case wait_cause::pedestrians: return "pedestrians";
		case wait_cause::bus_stop: return "bus stop";
		case wait_cause::level_crossing: return "level crossing";
		case wait_cause::lane_blocked: return "lane blocked";
		case wait_cause::merge_courtesy: return "merge courtesy";
		case wait_cause::no_route: return "no route";
		default: return "none";
	}
}

simulation::simulation()
	: m_model(nullptr)
	, m_net(nullptr)
	, m_rng(20261030u)
	, m_seed(20261030u)
	, m_time(0.0)
	, m_dt(0.1)
	, m_accum(0.0)
	, m_actual_speed(0.0)
	, m_last_step_ms(0.0)
	, m_stats_interval(1.0)
	, m_stats_accum(0.0)
	, m_vehicle_factor(1.0)
	, m_ped_factor(1.0)
	, m_gridlock_timeout(180.0)
	, m_gridlock_removed(0)
	, m_next_id(1)
	, m_finished_count(0)
	, m_finished_distance(0.0)
	, m_finished_fuel(0.0)
	, m_finished_co2(0.0)
	, m_finished_stops(0.0)
	, m_finished_stop_time(0.0)
	, m_finished_congested_time(0.0)
	, m_finished_congested_distance(0.0)
	, m_finished_light_wait(0.0)
	, m_cong_window(300.0)
	, m_cong_accum(0.0)
	, m_checks(false)
	, m_stuck_limit(120.0)
	, m_check_accum(0.0)
	, m_issue_counts{ 0, 0, 0, 0 }
{
}

simulation::~simulation() = default;

void simulation::set_time_step(double dt)
{
	m_dt = std::max(0.02, std::min(0.5, dt));
}

double simulation::clock() const
{
	const double start = m_model ? m_model->start_clock : 0.0;
	return std::fmod(start + m_time, 86400.0);
}

double simulation::random01()
{
	return std::uniform_real_distribution<double>(0.0, 1.0)(m_rng);
}

double simulation::random_exp(double mean)
{
	return -mean * std::log(std::max(1e-9, 1.0 - random01()));
}

double simulation::demand_profile(double clock_s)
{
	const double h = std::fmod(clock_s / 3600.0, 24.0);
	auto g = [h](double mu, double sigma) { return std::exp(-0.5 * sq((h - mu) / sigma)); };
	return 0.18 + 0.82 * std::max(std::max(g(8.0, 1.4), g(18.0, 1.6)), 0.55 * g(13.0, 2.8));
}

void simulation::set_demand_factors(double vehicles, double pedestrians)
{
	vehicles = std::max(0.0, std::min(10.0, vehicles));
	pedestrians = std::max(0.0, std::min(10.0, pedestrians));
	if (vehicles != m_vehicle_factor)
	{
		m_vehicle_factor = vehicles;
		m_next_spawn.clear(); // new arrival times with the new rates
	}
	if (pedestrians != m_ped_factor)
	{
		m_ped_factor = pedestrians;
		m_next_ped_spawn.clear();
	}
}

quint32 simulation::vehicle_color(vehicle_kind k)
{
	static const quint32 cars[] = { 0xF5F5F2, 0xC8CDD4, 0x6B7584, 0xE98A8A, 0x8FB3DE, 0x9DD3A8, 0xE8D2A6, 0xC3B4E6, 0xF2B880, 0xA6D8E3 };
	static const quint32 buses[] = { 0x8FC1B5, 0xF2C14E, 0x8FB3DE };
	static const quint32 motos[] = { 0xF28F8F, 0x8FB3DE, 0x5B6575, 0xF2C14E };
	switch (k)
	{
		case vehicle_kind::bus: return buses[m_rng() % 3];
		case vehicle_kind::motorcycle: return motos[m_rng() % 4];
		default: return cars[m_rng() % 10];
	}
}

// ---------------------------------------------------------------------------
// reset and network changes
// ---------------------------------------------------------------------------
void simulation::reset(const world_model *model, const road_network *net)
{
	m_model = model;
	m_net = net;
	m_vehicles.clear();
	m_peds.clear();
	m_stats.clear();
	m_lights.clear();
	m_node_light.clear();
	m_next_spawn.clear();
	m_next_ped_spawn.clear();
	m_stop_waiting.clear();
	m_stop_accum.clear();
	m_finish_times.clear();
	m_junction_waits.clear();
	m_time = 0.0;
	m_accum = 0.0;
	m_stats_accum = 0.0;
	m_next_id = 1;
	m_rng.seed(m_seed);
	m_finished_count = 0;
	m_gridlock_removed = 0;
	m_finished_distance = 0.0;
	m_finished_fuel = 0.0;
	m_finished_co2 = 0.0;
	m_finished_stops = 0.0;
	m_finished_stop_time = 0.0;
	m_finished_congested_time = 0.0;
	m_finished_congested_distance = 0.0;
	m_finished_light_wait = 0.0;
	m_passages.clear();
	m_wait_samples.clear();
	m_queue_samples.clear();
	m_congestion.clear();
	m_cong_accum = 0.0;
	m_issues.clear();
	m_issue_counts[0] = m_issue_counts[1] = m_issue_counts[2] = m_issue_counts[3] = 0;
	m_overlap_seen.clear();
	m_stuck_seen.clear();
	m_check_accum = 0.0;
	if (!m_model || !m_net)
	{
		return;
	}
	m_routes.build(*m_model, *m_net);
	m_reservations.assign(m_net->connectors.size(), 0);
	m_res_long.assign(m_net->connectors.size(), 0);
	m_cw_users.assign(m_net->crosswalks.size(), 0);
	m_on_lane.assign(m_net->lanes.size(), std::vector<int>());
	m_on_conn.assign(m_net->connectors.size(), std::vector<int>());
	index_connectors();
	update_lights(0.0);
	TE_INFO("SIM", QStringLiteral("The simulation starts again. Clock: %1 h.").arg(m_model->start_clock / 3600.0, 0, 'f', 2));
}

void simulation::index_connectors()
{
	m_conn_by_to_lane.assign(m_net->lanes.size(), std::vector<int>());
	for (const net_connector &c : m_net->connectors)
	{
		m_conn_by_to_lane[c.to_lane].push_back(c.id);
	}
}

void simulation::network_changing()
{
	m_anchors.clear();
	if (!m_net)
	{
		return;
	}
	for (const sim_vehicle &v : m_vehicles)
	{
		const int lane_id = v.on_connector ? m_net->connectors[v.seg].to_lane : v.seg;
		const double s = v.on_connector ? 0.0 : v.s;
		const net_lane &l = m_net->lanes[lane_id];
		m_anchors.push_back({ l.road_id, l.dir, l.index, m_net->road_s_from_lane_s(l, s) });
	}
}

void simulation::network_changed()
{
	if (!m_model || !m_net)
	{
		return;
	}
	m_routes.build(*m_model, *m_net);
	m_reservations.assign(m_net->connectors.size(), 0);
	m_res_long.assign(m_net->connectors.size(), 0);
	m_cw_users.assign(m_net->crosswalks.size(), 0);
	m_on_lane.assign(m_net->lanes.size(), std::vector<int>());
	m_on_conn.assign(m_net->connectors.size(), std::vector<int>());
	index_connectors();
	m_peds.clear(); // the pedestrians start again on the new network

	std::vector<sim_vehicle> kept;
	int dropped = 0;
	for (size_t i = 0; i < m_vehicles.size() && i < m_anchors.size(); ++i)
	{
		sim_vehicle v = m_vehicles[i];
		const anchor &a = m_anchors[i];
		const std::vector<int> &ls = m_net->lanes_of(a.road_id, a.dir);
		if (ls.empty())
		{
			++dropped;
			continue;
		}
		const int lane_id = ls[std::min<size_t>(static_cast<size_t>(std::max(0, a.index)), ls.size() - 1)];
		const net_lane &l = m_net->lanes[lane_id];
		v.on_connector = false;
		v.seg = lane_id;
		v.s = std::max(0.0, std::min(l.length(), m_net->lane_s_from_road_s(l, a.road_s)));
		v.prev_seg = -1;
		v.committed = false;
		v.held_conn = -1;
		v.held_prev = -1;
		v.held_next = -1;
		v.next_conn = -1;
		v.lc_from = -1;
		v.lc_progress = 1.0;
		v.route = m_routes.vehicle_route(road_network::dir_key(a.road_id, a.dir), v.dest_node);
		v.route_pos = 0;
		if (v.route.empty())
		{
			++dropped;
			continue;
		}
		plan_next(v);
		kept.push_back(v);
	}
	m_vehicles.swap(kept);
	m_anchors.clear();
	update_lights(0.0);
	TE_DEBUG("SIM", QStringLiteral("The road network changed: %1 vehicles kept, %2 removed.").arg(m_vehicles.size()).arg(dropped));
}

// ---------------------------------------------------------------------------
// time
// ---------------------------------------------------------------------------
int simulation::advance(double wall_dt, double speed, int max_steps)
{
	if (!m_model || !m_net || speed <= 0.0)
	{
		m_actual_speed = 0.0;
		return 0;
	}
	wall_dt = std::min(wall_dt, 0.25); // after a pause of the window: no jump
	m_accum += wall_dt * speed;
	int steps = 0;
	QElapsedTimer t;
	t.start();
	while (m_accum >= m_dt && steps < max_steps)
	{
		step(m_dt);
		m_accum -= m_dt;
		++steps;
	}
	if (m_accum > 2.0 * m_dt)
	{
		m_accum = 2.0 * m_dt; // the computer is too slow for this speed: drop the rest
	}
	if (steps > 0)
	{
		m_last_step_ms = static_cast<double>(t.nsecsElapsed()) / 1.0e6 / steps;
	}
	const double measured = wall_dt > 1e-6 ? steps * m_dt / wall_dt : 0.0;
	m_actual_speed = 0.9 * m_actual_speed + 0.1 * measured;
	return steps;
}

void simulation::step(double dt)
{
	if (!m_model || !m_net)
	{
		return;
	}
	m_time += dt;
	update_lights(dt);
	spawn_vehicles(dt);
	spawn_pedestrians(dt);
	build_index();
	lane_changes(dt);
	build_index();
	junction_entry(dt);
	accelerations();
	move(dt);
	build_index();
	update_pedestrians(dt);
	update_waiting();
	update_positions(dt);
	collect_stats(dt);
	update_congestion(dt);
	if (m_checks)
	{
		run_checks(dt);
	}
}

// ---------------------------------------------------------------------------
// traffic lights
// ---------------------------------------------------------------------------
void simulation::init_light(light_runtime &lr)
{
	const light_data *ld = m_model->light(lr.light_id);
	lr.phase = 0;
	lr.stage = 1;
	lr.phase_time = 0.0;
	if (!ld || ld->phases.empty())
	{
		lr.stage_left = 1.0;
		return;
	}
	lr.stage_left = std::max(1.0, ld->phases[0].green);
	// Start offset (coordination): run the stage machine forward.
	double t = std::max(0.0, ld->offset);
	int guard = 0;
	while (t > 0.0 && guard++ < 1000)
	{
		if (t < lr.stage_left)
		{
			lr.stage_left -= t;
			break;
		}
		t -= lr.stage_left;
		lr.stage_left = 0.0;
		advance_stage(lr);
	}
}

void simulation::advance_stage(light_runtime &lr)
{
	const light_data *ld = m_model->light(lr.light_id);
	if (!ld || ld->phases.empty())
	{
		lr.stage_left = 1.0;
		return;
	}
	const int n = static_cast<int>(ld->phases.size());
	lr.phase = std::min(lr.phase, n - 1);
	const light_phase &ph = ld->phases[lr.phase];
	switch (lr.stage)
	{
		case 1: // green -> yellow
			lr.stage = 2;
			lr.stage_left += std::max(0.0, ph.yellow);
			break;
		case 2: // yellow -> all red
			lr.stage = 3;
			lr.stage_left += std::max(0.0, ph.all_red);
			break;
		case 3: // all red -> red + yellow of the next phase
			lr.phase = (lr.phase + 1) % n;
			lr.stage = 0;
			lr.stage_left += std::max(0.0, ld->red_yellow);
			break;
		default: // red + yellow -> green
			lr.stage = 1;
			lr.phase_time = 0.0;
			lr.stage_left += std::max(1.0, ld->phases[lr.phase].green);
			break;
	}
}

bool simulation::phase_has_road(const light_runtime &lr, int phase, int road_id) const
{
	const light_data *ld = m_model->light(lr.light_id);
	if (!ld || phase < 0 || phase >= static_cast<int>(ld->phases.size()))
	{
		return false;
	}
	const light_phase &ph = ld->phases[phase];
	if (ph.pedestrian_only)
	{
		return false;
	}
	return std::find(ph.green_roads.begin(), ph.green_roads.end(), road_id) != ph.green_roads.end();
}

void simulation::update_lights(double dt)
{
	// Keep the states in step with the model (lights added or removed at runtime)
	for (const auto &kv : m_model->lights)
	{
		auto it = m_lights.find(kv.first);
		if (it == m_lights.end() || it->second.node_id != kv.second.node_id)
		{
			light_runtime lr;
			lr.light_id = kv.first;
			lr.node_id = kv.second.node_id;
			init_light(lr);
			m_lights[kv.first] = lr;
		}
	}
	for (auto it = m_lights.begin(); it != m_lights.end();)
	{
		it = m_model->light(it->first) ? std::next(it) : m_lights.erase(it);
	}
	m_node_light.clear();
	for (const auto &kv : m_lights)
	{
		m_node_light[kv.second.node_id] = kv.first;
	}
	if (dt <= 0.0)
	{
		return;
	}

	for (auto &kv : m_lights)
	{
		light_runtime &lr = kv.second;
		const light_data *ld = m_model->light(lr.light_id);
		if (!ld || ld->phases.empty())
		{
			continue;
		}
		if (lr.phase >= static_cast<int>(ld->phases.size()))
		{
			lr.phase = 0;
			lr.stage = 1;
			lr.stage_left = ld->phases[0].green;
		}
		if (ld->algorithm == light_algorithm::external && lr.external)
		{
			if (m_time < lr.external_until)
			{
				lr.stage = 1;
				lr.stage_left = lr.external_until - m_time;
				continue;
			}
			lr.external = false; // no new data: fixed time again
		}
		lr.stage_left -= dt;
		if (lr.stage == 1)
		{
			lr.phase_time += dt;
			if (ld->algorithm == light_algorithm::actuated)
			{
				const light_phase &ph = ld->phases[lr.phase];
				int here = 0;
				int other = 0;
				const net_junction *j = m_net->junction(lr.node_id);
				if (j)
				{
					for (const net_arm &arm : j->arms)
					{
						for (int lid : m_net->lanes_of(arm.road_id, arm.at_a ? -1 : 1))
						{
							const net_lane &l = m_net->lanes[lid];
							for (int vi : m_on_lane[lid])
							{
								if (vi >= static_cast<int>(m_vehicles.size()))
								{
									continue;
								}
								const double d = l.stop_line - m_vehicles[vi].s;
								if (d >= -1.0 && d < 40.0)
								{
									if (phase_has_road(lr, lr.phase, arm.road_id)) { ++here; } else { ++other; }
								}
							}
						}
					}
				}
				if (lr.phase_time >= ph.min_green)
				{
					if (here == 0 && other > 0)
					{
						lr.stage_left = std::min(lr.stage_left, 0.0); // gap out
					}
					else if (here > 0 && lr.stage_left < 2.0 && lr.phase_time < ph.max_green)
					{
						lr.stage_left = 2.0; // extend the green
					}
				}
				if (lr.phase_time >= ph.max_green)
				{
					lr.stage_left = std::min(lr.stage_left, 0.0);
				}
			}
		}
		int guard = 0;
		while (lr.stage_left <= 0.0 && guard++ < 8)
		{
			advance_stage(lr);
		}
	}
}

const light_runtime *simulation::light_at_node(int node_id) const
{
	const auto it = m_node_light.find(node_id);
	if (it == m_node_light.end())
	{
		return nullptr;
	}
	const auto lt = m_lights.find(it->second);
	return lt == m_lights.end() ? nullptr : &lt->second;
}

const light_runtime *simulation::light_state(int light_id) const
{
	const auto it = m_lights.find(light_id);
	return it == m_lights.end() ? nullptr : &it->second;
}

signal_color simulation::signal_for(int node_id, int road_id) const
{
	const light_runtime *lr = light_at_node(node_id);
	if (!lr || !m_model)
	{
		return signal_color::off;
	}
	const light_data *ld = m_model->light(lr->light_id);
	if (!ld || ld->phases.empty())
	{
		return signal_color::off;
	}
	const bool in = phase_has_road(*lr, lr->phase, road_id);
	switch (lr->stage)
	{
		case 0: return in ? signal_color::red_yellow : signal_color::red;
		case 1: return in ? signal_color::green : signal_color::red;
		case 2: return in ? signal_color::yellow : signal_color::red;
		default: return signal_color::red;
	}
}

double simulation::signal_remaining(int node_id, int road_id) const
{
	const light_runtime *lr = light_at_node(node_id);
	if (!lr || !m_model)
	{
		return 0.0;
	}
	const light_data *ld = m_model->light(lr->light_id);
	if (!ld || ld->phases.empty())
	{
		return 0.0;
	}
	const int n = static_cast<int>(ld->phases.size());
	const bool in = phase_has_road(*lr, lr->phase, road_id);
	if (in && lr->stage == 1)
	{
		return lr->stage_left + std::max(0.0, ld->phases[lr->phase].yellow);
	}
	if (in && (lr->stage == 2 || lr->stage == 0))
	{
		return lr->stage_left;
	}
	// Red: go forward through the stages until this road has green.
	double t = lr->stage_left;
	int phase = lr->phase;
	int stage = lr->stage;
	for (int guard = 0; guard < 4 * n + 4; ++guard)
	{
		switch (stage)
		{
			case 1: stage = 2; t += ld->phases[phase].yellow; break;
			case 2: stage = 3; t += ld->phases[phase].all_red; break;
			case 3: phase = (phase + 1) % n; stage = 0; t += ld->red_yellow; break;
			default:
				stage = 1;
				if (phase_has_road(*lr, phase, road_id))
				{
					return t;
				}
				t += ld->phases[phase].green;
				break;
		}
	}
	return t;
}

int simulation::waiting_at(int node_id, int road_id) const
{
	const light_runtime *lr = light_at_node(node_id);
	if (!lr)
	{
		return 0;
	}
	const auto it = lr->waiting.find(road_id);
	return it == lr->waiting.end() ? 0 : it->second;
}

void simulation::set_external_phase(int light_id, int phase, double duration)
{
	auto it = m_lights.find(light_id);
	const light_data *ld = m_model ? m_model->light(light_id) : nullptr;
	if (it == m_lights.end() || !ld || ld->phases.empty())
	{
		return;
	}
	light_runtime &lr = it->second;
	lr.external = true;
	lr.phase = std::max(0, std::min(phase, static_cast<int>(ld->phases.size()) - 1));
	lr.stage = 1;
	lr.phase_time = 0.0;
	lr.external_until = m_time + std::max(1.0, duration);
	lr.stage_left = lr.external_until - m_time;
	TE_INFO("SIM", QStringLiteral("External phase for light %1: phase %2 for %3 s").arg(light_id).arg(lr.phase + 1).arg(duration));
}

// ---------------------------------------------------------------------------
// objects
// ---------------------------------------------------------------------------
crossing_state simulation::level_crossing(int object_id, double *barrier, double *train_pos) const
{
	if (barrier) { *barrier = 0.0; }
	if (train_pos) { *train_pos = -1.0; }
	const object_data *o = m_model ? m_model->object(object_id) : nullptr;
	if (!o || o->kind != object_kind::level_crossing)
	{
		return crossing_state::open;
	}
	const double period = std::max(60.0, o->train_period);
	const double closed = std::max(10.0, std::min(o->closed_time, period - 20.0));
	const double warn = 8.0;
	const double rise = 4.0;
	double ph = std::fmod(m_time + o->train_offset, period);
	if (ph < 0.0)
	{
		ph += period;
	}
	if (ph >= period - warn)
	{
		if (barrier) { *barrier = (ph - (period - warn)) / warn; }
		return crossing_state::closing;
	}
	if (ph < closed)
	{
		if (barrier) { *barrier = 1.0; }
		if (train_pos)
		{
			const double tp = (ph - 0.15 * closed) / (0.7 * closed);
			*train_pos = (tp >= 0.0 && tp <= 1.0) ? tp : -1.0;
		}
		return crossing_state::closed;
	}
	if (ph < closed + rise)
	{
		if (barrier) { *barrier = 1.0 - (ph - closed) / rise; }
		return crossing_state::opening;
	}
	return crossing_state::open;
}

int simulation::bus_stop_waiting(int object_id) const
{
	const auto it = m_stop_waiting.find(object_id);
	return it == m_stop_waiting.end() ? 0 : it->second;
}

int simulation::crosswalk_users(int crosswalk_id) const
{
	return (crosswalk_id >= 0 && crosswalk_id < static_cast<int>(m_cw_users.size())) ? m_cw_users[crosswalk_id] : 0;
}

// ---------------------------------------------------------------------------
// vehicles: spawn, route, index
// ---------------------------------------------------------------------------
void simulation::spawn_vehicles(double dt)
{
	Q_UNUSED(dt);
	const double profile = demand_profile(clock());
	for (const auto &kv : m_model->nodes)
	{
		const node_data &n = kv.second;
		const net_junction *j = m_net->junction(n.id);
		if (!n.spawn || !j || j->arms.size() != 1)
		{
			continue;
		}
		const double rate = n.vehicles_per_hour * (n.daily_profile ? profile : 1.0) * m_vehicle_factor;
		if (rate < 0.01)
		{
			m_next_spawn.erase(n.id);
			continue;
		}
		auto it = m_next_spawn.find(n.id);
		if (it == m_next_spawn.end())
		{
			m_next_spawn[n.id] = m_time + random_exp(3600.0 / rate);
			continue;
		}
		if (m_time < it->second)
		{
			continue;
		}
		if (try_spawn_vehicle(n.id))
		{
			it->second = m_time + random_exp(3600.0 / rate);
		}
		else
		{
			it->second = m_time + 0.5; // no space now: try again soon
		}
	}
}

bool simulation::try_spawn_vehicle(int node_id)
{
	const std::vector<int> keys = m_routes.keys_from_node(node_id);
	const node_data *n = m_model->node(node_id);
	if (keys.empty() || !n)
	{
		return false;
	}
	const int start_key = keys[m_rng() % keys.size()];

	// Destination: another dead end, weighted by its demand
	std::vector<std::pair<int, double>> dests;
	double total = 0.0;
	for (const auto &kv : m_model->nodes)
	{
		const net_junction *j = m_net->junction(kv.first);
		if (kv.first == node_id || !kv.second.spawn || !j || j->arms.size() != 1)
		{
			continue;
		}
		const double w = std::max(1.0, kv.second.vehicles_per_hour);
		dests.push_back(std::make_pair(kv.first, w));
		total += w;
	}
	if (dests.empty())
	{
		return false;
	}
	std::vector<int> route;
	int dest = -1;
	for (int attempt = 0; attempt < 4 && route.empty(); ++attempt)
	{
		double r = random01() * total;
		dest = dests.back().first;
		for (const auto &d : dests)
		{
			r -= d.second;
			if (r <= 0.0)
			{
				dest = d.first;
				break;
			}
		}
		route = m_routes.vehicle_route(start_key, dest);
	}
	if (route.empty())
	{
		return false;
	}

	vehicle_kind kind = vehicle_kind::sedan;
	const double r = random01();
	if (r < n->share_bus)
	{
		kind = vehicle_kind::bus;
	}
	else if (r < n->share_bus + n->share_motorcycle)
	{
		kind = vehicle_kind::motorcycle;
	}
	const vehicle_params &p = vehicle_types::params(kind);

	// Lane: a lane with the first movement of the route
	const std::vector<int> &ls = m_net->lanes_of(key_road(start_key), key_dir(start_key));
	if (ls.empty())
	{
		return false;
	}
	std::vector<int> good;
	for (int lid : ls)
	{
		const net_lane &l = m_net->lanes[lid];
		if (l.blocked_at(0.0, p.length + 5.0))
		{
			continue;
		}
		bool ok = route.size() < 2;
		for (int cid : l.out)
		{
			const net_connector &c = m_net->connectors[cid];
			const net_lane &to = m_net->lanes[c.to_lane];
			if (!ok && c.allowed && to.road_id == key_road(route[1]) && to.dir == key_dir(route[1]))
			{
				ok = true;
			}
		}
		if (ok)
		{
			good.push_back(lid);
		}
	}
	if (good.empty())
	{
		good = ls;
	}
	int lane_id = good[m_rng() % good.size()];
	if (kind == vehicle_kind::bus)
	{
		lane_id = good.front(); // buses use the curb lane
	}
	const net_lane &lane = m_net->lanes[lane_id];
	// The vehicle must fit on the lane (a very short road at the border of an
	// imported area): else its body is in the junction.
	if (lane.length() < p.length + 1.0)
	{
		return false;
	}

	// Space at the start of the lane (also from a vehicle that changes from this
	// lane to another lane: it is still partly in this lane)
	double v_init = lane.limit_at(0.0) * 0.7;
	int last = -1;
	const double rear = first_rear(lane_id, &last, 0, nullptr);
	if (last >= 0)
	{
		if (rear < p.length + p.min_gap + 1.0)
		{
			return false;
		}
		v_init = std::min(v_init, m_vehicles[last].v);
	}

	sim_vehicle v;
	v.id = m_next_id++;
	v.kind = kind;
	v.p = &p;
	v.color = vehicle_color(kind);
	int max_pass = p.max_passengers;
	if (kind == vehicle_kind::bus)
	{
		max_pass = std::max(p.min_passengers, static_cast<int>(p.max_passengers * demand_profile(clock())));
	}
	v.passengers = p.min_passengers + static_cast<int>(m_rng() % static_cast<unsigned>(std::max(1, max_pass - p.min_passengers + 1)));
	if (kind == vehicle_kind::sedan && v.passengers > 2 && random01() < 0.6)
	{
		v.passengers = 1 + static_cast<int>(m_rng() % 2); // most cars have one or two persons
	}
	v.v0_factor = 0.88 + 0.12 * random01();
	v.seg = lane_id;
	v.s = std::min(lane.length(), p.length);
	v.v = v_init;
	v.route = route;
	v.route_pos = 0;
	v.origin_node = node_id;
	v.dest_node = dest;
	v.spawn_time = m_time;
	plan_next(v);
	m_vehicles.push_back(v);
	TE_TRACE("SIM", QStringLiteral("Vehicle %1 (%2) starts at node %3 to node %4, %5 roads.")
		.arg(v.id).arg(p.name).arg(node_id).arg(dest).arg(route.size()));
	return true;
}

void simulation::plan_next(sim_vehicle &veh)
{
	veh.next_conn = -1;
	veh.target_index = -1;
	if (veh.on_connector || veh.route_pos + 1 >= static_cast<int>(veh.route.size()))
	{
		return;
	}
	const net_lane &l = m_net->lanes[veh.seg];
	const int next_key = veh.route[veh.route_pos + 1];
	const int nr = key_road(next_key);
	const int nd = key_dir(next_key);
	const int best = choose_connector(l, next_key);
	if (best >= 0)
	{
		veh.next_conn = best;
		return;
	}
	// Another lane of this road has the movement: the vehicle must change lanes.
	int best_index = -1;
	for (int lid : m_net->lanes_of(l.road_id, l.dir))
	{
		const net_lane &l2 = m_net->lanes[lid];
		for (int cid : l2.out)
		{
			const net_connector &c = m_net->connectors[cid];
			const net_lane &to = m_net->lanes[c.to_lane];
			if (c.allowed && to.road_id == nr && to.dir == nd && !to.closed)
			{
				if (best_index < 0 || std::abs(l2.index - l.index) < std::abs(best_index - l.index))
				{
					best_index = l2.index;
				}
			}
		}
	}
	veh.target_index = best_index;
}

int simulation::choose_connector(const net_lane &l, int road_key) const
{
	int best = -1;
	for (int cid : l.out)
	{
		const net_connector &c = m_net->connectors[cid];
		const net_lane &to = m_net->lanes[c.to_lane];
		if (c.allowed && to.road_id == key_road(road_key) && to.dir == key_dir(road_key) && !to.closed)
		{
			if (best < 0 || c.turn == turn_kind::straight)
			{
				best = cid;
			}
		}
	}
	return best;
}

int simulation::exit_blocker(int lane_id, int last) const
{
	if (last >= 0)
	{
		return m_vehicles[last].id;
	}
	// No vehicle in the lane: a vehicle with a reservation to the lane
	for (int cc : m_conn_by_to_lane[lane_id])
	{
		if (m_reservations[cc] > 0 && cc < static_cast<int>(m_holder.size()) && m_holder[cc] >= 0)
		{
			return m_holder[cc];
		}
	}
	return -1;
}

double simulation::exit_space(const sim_vehicle &veh, int lane_id, int *last, int *claims, bool ends_here) const
{
	const net_lane &to = m_net->lanes[lane_id];
	const double rear = first_rear(lane_id, last, 0, nullptr);
	*claims = 0;
	for (int cc : m_conn_by_to_lane[lane_id])
	{
		*claims += m_reservations[cc];
	}
	// The own reservation to this lane is not a claim of another vehicle
	if (veh.held_conn >= 0 && m_net->connectors[veh.held_conn].to_lane == lane_id)
	{
		--*claims;
	}
	if (veh.held_next >= 0 && m_net->connectors[veh.held_next].to_lane == lane_id)
	{
		--*claims;
	}
	*claims = std::max(0, *claims);
	// An empty lane: the vehicle can stop at the stop line (not at the end of the lane)
	return *last >= 0 ? rear : ((ends_here || to.dead_end) ? to.length() : to.stop_line);
}

bool simulation::through_ok(const sim_vehicle &veh, int c2, wait_cause *cause, int *cause_id) const
{
	const net_connector &c = m_net->connectors[c2];
	const net_lane &l = m_net->lanes[c.from_lane];
	*cause = wait_cause::none;
	*cause_id = -1;
	auto fail = [cause, cause_id](wait_cause why, int id)
	{
		*cause = why;
		*cause_id = id;
		return false;
	};
	// The light of the next junction must be green (a yellow light can change
	// to red before the vehicle comes)
	const signal_color col = signal_for(c.node_id, l.road_id);
	if (col != signal_color::off && col != signal_color::green)
	{
		const light_runtime *lr = light_at_node(c.node_id);
		return fail(wait_cause::red_light, lr ? lr->light_id : -1);
	}
	if (col == signal_color::off && l.stop_sign)
	{
		return fail(wait_cause::stop_sign, l.sign_id);
	}
	// No reservation and no waiting vehicle on a conflicting movement (a merge
	// claim stops only the vehicles to the same exit lane)
	for (int ccid : c.conflicts)
	{
		const bool claimed = !m_claimer.empty() && m_claimer[ccid] >= 0
			&& (!m_claim_merge[ccid] || m_net->connectors[ccid].to_lane == c.to_lane);
		if (m_reservations[ccid] > 0 || claimed)
		{
			return fail(wait_cause::conflict, m_reservations[ccid] > 0 ? m_holder[ccid] : m_claimer[ccid]);
		}
	}
	const bool is_long = veh.p->length > long_vehicle;
	for (int ccid : c.conflicts_long)
	{
		if (is_long ? m_reservations[ccid] > 0 : m_res_long[ccid] > 0)
		{
			return fail(wait_cause::conflict, m_holder[ccid]);
		}
	}
	// A vehicle with priority that comes to the next junction
	for (int ccid : c.conflicts)
	{
		const net_connector &cc = m_net->connectors[ccid];
		const net_lane &fl = m_net->lanes[cc.from_lane];
		if (col != signal_color::off && signal_for(c.node_id, fl.road_id) != signal_color::green && signal_for(c.node_id, fl.road_id) != signal_color::yellow)
		{
			continue;
		}
		if (col == signal_color::off && fl.rank < l.rank)
		{
			continue;
		}
		const int wi = front_vehicle_on_lane(fl.id);
		if (wi >= 0)
		{
			const sim_vehicle &w = m_vehicles[wi];
			const double d2 = fl.stop_line - w.s;
			if (w.next_conn == cc.id && w.v > 1.0 && d2 > -1.0 && d2 / std::max(0.1, w.v) < yield_time)
			{
				return fail(wait_cause::give_way, w.id);
			}
		}
	}
	// Another vehicle waits for a long time for space in the lane after the next junction
	const int claimant = c.to_lane < static_cast<int>(m_space_claim.size()) ? m_space_claim[c.to_lane] : -1;
	if (claimant >= 0 && claimant != veh.id)
	{
		return fail(wait_cause::exit_full, claimant);
	}
	// Space after the next junction (a very short last road of the trip: its length)
	int last = -1;
	int claims = 0;
	const net_lane &after = m_net->lanes[c.to_lane];
	const bool ends_here = after.dead_end || veh.route_pos + 3 >= static_cast<int>(veh.route.size());
	const double free_space = exit_space(veh, c.to_lane, &last, &claims, ends_here);
	const double need = ends_here ? std::min(veh.p->length + veh.p->min_gap + 1.0, std::max(1.0, after.length() - 0.5))
		: veh.p->length + veh.p->min_gap + 1.0;
	// A second short link after the next junction: it must be empty (no more space exists)
	const bool short_after = after.stop_line < need && !ends_here;
	if (short_after ? (last >= 0 || claims > 0) : free_space < need * (claims + 1))
	{
		return fail(wait_cause::exit_full, exit_blocker(c.to_lane, last));
	}
	// Pedestrians
	const net_lane &to = m_net->lanes[c.to_lane];
	if (l.end_crosswalk >= 0 && m_cw_users[l.end_crosswalk] > 0)
	{
		return fail(wait_cause::pedestrians, pedestrian_on_crosswalk(l.end_crosswalk));
	}
	if (to.start_crosswalk >= 0 && m_cw_users[to.start_crosswalk] > 0)
	{
		return fail(wait_cause::pedestrians, pedestrian_on_crosswalk(to.start_crosswalk));
	}
	return true;
}

bool simulation::exit_full(const sim_vehicle &veh, int conn) const
{
	const net_lane &to = m_net->lanes[m_net->connectors[conn].to_lane];
	double free_space = to.dead_end ? to.length() : to.stop_line;
	const int last = last_vehicle_on_lane(to.id);
	if (last >= 0)
	{
		const sim_vehicle &w = m_vehicles[last];
		free_space = w.s - w.p->length;
	}
	return free_space < veh.p->length + veh.p->min_gap + 1.0;
}

bool simulation::reroute(sim_vehicle &veh, bool need_space)
{
	if (veh.on_connector)
	{
		return false;
	}
	const net_lane &l = m_net->lanes[veh.seg];
	const int key_now = road_network::dir_key(l.road_id, l.dir);
	std::vector<int> outs = l.out;
	std::shuffle(outs.begin(), outs.end(), m_rng);
	for (int cid : outs)
	{
		const net_connector &c = m_net->connectors[cid];
		const net_lane &to = m_net->lanes[c.to_lane];
		if (!c.allowed || to.closed)
		{
			continue;
		}
		if (need_space && (cid == veh.next_conn || exit_full(veh, cid)))
		{
			continue;
		}
		const int to_key = road_network::dir_key(to.road_id, to.dir);
		std::vector<int> rest = m_routes.vehicle_route(to_key, veh.dest_node);
		if (rest.empty())
		{
			// any other destination
			for (const auto &kv : m_model->nodes)
			{
				const net_junction *j = m_net->junction(kv.first);
				if (j && j->arms.size() == 1 && kv.first != veh.origin_node)
				{
					rest = m_routes.vehicle_route(to_key, kv.first);
					if (!rest.empty())
					{
						veh.dest_node = kv.first;
						break;
					}
				}
			}
		}
		if (rest.empty())
		{
			continue;
		}
		veh.route.clear();
		veh.route.push_back(key_now);
		veh.route.insert(veh.route.end(), rest.begin(), rest.end());
		veh.route_pos = 0;
		plan_next(veh);
		TE_DEBUG("SIM", QStringLiteral("Vehicle %1 has a new route (%2 roads).").arg(veh.id).arg(veh.route.size()));
		return true;
	}
	return false;
}

void simulation::release_commit(sim_vehicle &veh)
{
	if (veh.held_conn >= 0 && veh.held_conn < static_cast<int>(m_reservations.size()))
	{
		unreserve(veh, veh.held_conn);
	}
	veh.held_conn = -1;
	veh.committed = false;
	// The next junction after a short link belongs to the same commitment.
	if (veh.held_next >= 0)
	{
		unreserve(veh, veh.held_next);
		veh.held_next = -1;
	}
}

void simulation::reserve(const sim_vehicle &veh, int conn)
{
	++m_reservations[conn];
	if (veh.p->length > long_vehicle)
	{
		++m_res_long[conn];
	}
}

void simulation::unreserve(const sim_vehicle &veh, int conn)
{
	if (conn < 0 || conn >= static_cast<int>(m_reservations.size()))
	{
		return;
	}
	m_reservations[conn] = std::max(0, m_reservations[conn] - 1);
	if (veh.p->length > long_vehicle)
	{
		m_res_long[conn] = std::max(0, m_res_long[conn] - 1);
	}
}

void simulation::release(sim_vehicle &veh)
{
	release_commit(veh);
	if (veh.held_prev >= 0 && veh.held_prev < static_cast<int>(m_reservations.size()))
	{
		unreserve(veh, veh.held_prev);
	}
	veh.held_prev = -1;
	if (veh.held_next >= 0)
	{
		unreserve(veh, veh.held_next);
	}
	veh.held_next = -1;
}

bool simulation::uses_connector(const sim_vehicle &veh, int conn) const
{
	const net_connector &h = m_net->connectors[conn];
	if (!veh.on_connector && veh.seg == h.from_lane)
	{
		return true; // before the junction
	}
	if (veh.on_connector && veh.seg == conn)
	{
		return true; // in the junction
	}
	double after = 0.0; // distance of the front after the end of the connector
	if (!veh.on_connector && veh.seg == h.to_lane)
	{
		after = veh.s;
	}
	else if (veh.on_connector && m_net->connectors[veh.seg].from_lane == h.to_lane)
	{
		after = m_net->lanes[h.to_lane].length() + veh.s;
	}
	else
	{
		return false;
	}
	return after <= veh.p->length + 0.5;
}

void simulation::build_index()
{
	if (m_on_lane.size() != m_net->lanes.size())
	{
		m_on_lane.assign(m_net->lanes.size(), std::vector<int>());
	}
	if (m_on_conn.size() != m_net->connectors.size())
	{
		m_on_conn.assign(m_net->connectors.size(), std::vector<int>());
	}
	if (m_leaving.size() != m_net->lanes.size())
	{
		m_leaving.assign(m_net->lanes.size(), std::vector<int>());
	}
	for (auto &v : m_on_lane) { v.clear(); }
	for (auto &v : m_on_conn) { v.clear(); }
	for (auto &v : m_leaving) { v.clear(); }
	for (int i = 0; i < static_cast<int>(m_vehicles.size()); ++i)
	{
		const sim_vehicle &v = m_vehicles[i];
		if (v.finished)
		{
			continue;
		}
		if (v.on_connector)
		{
			m_on_conn[v.seg].push_back(i);
		}
		else
		{
			m_on_lane[v.seg].push_back(i);
			// During a lane change the vehicle is still partly in the old lane.
			if (v.lc_from >= 0 && v.lc_progress < 1.0 && v.lc_from < static_cast<int>(m_leaving.size()))
			{
				m_leaving[v.lc_from].push_back(i);
			}
		}
	}
	auto by_s = [this](int a, int b) { return m_vehicles[a].s > m_vehicles[b].s; };
	for (auto &v : m_on_lane)
	{
		if (v.size() > 1) { std::sort(v.begin(), v.end(), by_s); }
	}
	for (auto &v : m_on_conn)
	{
		if (v.size() > 1) { std::sort(v.begin(), v.end(), by_s); }
	}
	m_gap_cache.assign(m_vehicles.size(), inf);
}

int simulation::front_vehicle_on_lane(int lane_id) const
{
	const std::vector<int> &l = m_on_lane[lane_id];
	return l.empty() ? -1 : l.front();
}

int simulation::last_vehicle_on_lane(int lane_id) const
{
	if (lane_id < 0 || lane_id >= static_cast<int>(m_on_lane.size()))
	{
		return -1;
	}
	const std::vector<int> &l = m_on_lane[lane_id];
	return l.empty() ? -1 : l.back();
}

const sim_vehicle *simulation::find_vehicle(int id) const
{
	for (const sim_vehicle &v : m_vehicles)
	{
		if (v.id == id)
		{
			return &v;
		}
	}
	return nullptr;
}

const sim_pedestrian *simulation::find_pedestrian(int id) const
{
	for (const sim_pedestrian &p : m_peds)
	{
		if (p.id == id)
		{
			return &p;
		}
	}
	return nullptr;
}

std::vector<vec2> simulation::route_line(int vehicle_id) const
{
	std::vector<vec2> out;
	const sim_vehicle *v = find_vehicle(vehicle_id);
	if (!v || !m_net)
	{
		return out;
	}
	const polyline &cur = v->on_connector ? m_net->connectors[v->seg].path : m_net->lanes[v->seg].path;
	out.push_back(cur.point_at(v->s));
	const std::vector<vec2> &cp = cur.points();
	const std::vector<double> &cc = cur.cumulative();
	for (size_t i = 0; i < cp.size(); ++i)
	{
		if (cc[i] > v->s)
		{
			out.push_back(cp[i]);
		}
	}
	const int start = v->on_connector ? v->route_pos + 1 : v->route_pos + 1;
	for (int k = start; k < static_cast<int>(v->route.size()); ++k)
	{
		const std::vector<int> &ls = m_net->lanes_of(key_road(v->route[k]), key_dir(v->route[k]));
		if (ls.empty())
		{
			break;
		}
		const std::vector<vec2> &pts = m_net->lanes[ls.front()].path.points();
		out.insert(out.end(), pts.begin(), pts.end());
	}
	return out;
}

// ---------------------------------------------------------------------------
// vehicles: lane changes
// ---------------------------------------------------------------------------
double simulation::lane_accel_with(int vi, int lane_id, double s_mapped) const
{
	const sim_vehicle &veh = m_vehicles[vi];
	const std::vector<int> &list = m_on_lane[lane_id];
	double gap = inf;
	double lead_v = 0.0;
	for (int wi : list)
	{
		if (wi == vi)
		{
			continue;
		}
		const sim_vehicle &w = m_vehicles[wi];
		if (w.s > s_mapped)
		{
			gap = w.s - w.p->length - s_mapped;
			lead_v = w.v;
		}
		else
		{
			break;
		}
	}
	return idm(veh, desired_speed(veh), gap, veh.v - lead_v);
}

bool simulation::lane_change_safe(int vi, int target_lane, double *gain) const
{
	const sim_vehicle &veh = m_vehicles[vi];
	const net_lane &from = m_net->lanes[veh.seg];
	const net_lane &to = m_net->lanes[target_lane];
	const double s_new = veh.s * to.length() / std::max(0.1, from.length());
	const sim_vehicle *lead = nullptr;
	const sim_vehicle *follow = nullptr;
	for (int wi : m_on_lane[target_lane])
	{
		const sim_vehicle &w = m_vehicles[wi];
		if (w.s > s_new)
		{
			lead = &w;
		}
		else
		{
			follow = &w;
			break;
		}
	}
	// The start of a lane: vehicles in the junction have a reservation for this
	// space, or a vehicle waits for a long time for it (space claim).
	if (s_new - veh.p->length < 15.0)
	{
		for (int cc : m_conn_by_to_lane[target_lane])
		{
			if (m_reservations[cc] > 0)
			{
				return false;
			}
		}
	}
	// A vehicle that changes from the target lane to another lane is still
	// partly in it: its space is not free yet.
	for (int wi : m_leaving[target_lane])
	{
		const sim_vehicle &w = m_vehicles[wi];
		if (wi == vi)
		{
			continue;
		}
		const double ws = w.s * to.length() / std::max(0.1, m_net->lanes[w.seg].length());
		if (ws > s_new - veh.p->length - lc_gap && ws - w.p->length < s_new + lc_gap)
		{
			return false;
		}
	}
	double a_me = idm(veh, desired_speed(veh), inf, 0.0);
	if (lead)
	{
		const double gap = lead->s - lead->p->length - s_new;
		if (gap < lc_gap)
		{
			return false;
		}
		a_me = idm(veh, desired_speed(veh), gap, veh.v - lead->v);
		if (a_me < -veh.p->b_comf)
		{
			return false;
		}
	}
	if (follow)
	{
		const double gap = s_new - veh.p->length - follow->s;
		if (gap < lc_gap)
		{
			return false;
		}
		// Not in front of a vehicle with a reservation for the next junction:
		// it could not use it (and the junction would wait for it).
		if (follow->committed)
		{
			return false;
		}
		const double a_f = idm(*follow, desired_speed(*follow), gap, follow->v - veh.v);
		if (a_f < -follow->p->b_comf)
		{
			return false;
		}
	}
	if (to.blocked_at(s_new - veh.p->length, s_new + 15.0))
	{
		return false;
	}
	if (gain)
	{
		*gain = a_me;
	}
	return true;
}

void simulation::lane_changes(double dt)
{
	for (int i = 0; i < static_cast<int>(m_vehicles.size()); ++i)
	{
		sim_vehicle &veh = m_vehicles[i];
		veh.lc_cooldown -= dt;
		if (veh.finished || veh.on_connector || veh.lc_progress < 1.0 || veh.dwell_left > 0.0)
		{
			continue;
		}
		const net_lane &l = m_net->lanes[veh.seg];
		if (l.lanes_in_dir < 2)
		{
			continue;
		}
		const double dist_end = l.length() - veh.s;
		if (veh.s < veh.p->length + 2.0 || dist_end < 10.0 || veh.committed)
		{
			continue;
		}
		int want = 0;
		bool mandatory = false;

		// 1) the route needs another lane
		if (veh.target_index >= 0 && veh.target_index != l.index)
		{
			want = veh.target_index > l.index ? 1 : -1;
			mandatory = true;
		}
		// 2) cones or a barrier ahead on this lane
		if (!mandatory)
		{
			for (const net_block &b : l.blocks)
			{
				if (b.s1 > veh.s && b.s0 - veh.s < 120.0)
				{
					mandatory = true;
					if (l.left >= 0 && !m_net->lanes[l.left].blocked_at(b.s0, b.s1))
					{
						want = 1;
					}
					else if (l.right >= 0 && !m_net->lanes[l.right].blocked_at(b.s0, b.s1))
					{
						want = -1;
					}
					break;
				}
			}
		}
		// 3) a bus goes to the curb lane before its bus stop
		if (!mandatory && veh.kind == vehicle_kind::bus && l.index > 0)
		{
			const std::vector<int> &ls = m_net->lanes_of(l.road_id, l.dir);
			const net_lane &curb = m_net->lanes[ls.front()];
			for (const net_stop_point &st : curb.bus_stops)
			{
				if (st.object_id != veh.served_stop && st.s > veh.s + 10.0)
				{
					want = -1;
					mandatory = true;
					break;
				}
			}
		}
		// 4) optional lane change (overtaking), keep right
		if (!mandatory && veh.lc_cooldown <= 0.0 && l.overtaking_at(veh.s) && dist_end > 80.0)
		{
			veh.lc_cooldown = 1.0 + 0.5 * random01();
			const double a_cur = veh.a;
			double a_left = -inf;
			double a_right = -inf;
			if (l.left >= 0 && lane_change_safe(i, l.left, nullptr))
			{
				a_left = lane_accel_with(i, l.left, veh.s * m_net->lanes[l.left].length() / l.length());
			}
			if (l.right >= 0 && lane_change_safe(i, l.right, nullptr))
			{
				a_right = lane_accel_with(i, l.right, veh.s * m_net->lanes[l.right].length() / l.length());
			}
			if (a_left > a_cur + 0.6 && veh.target_index < 0)
			{
				want = 1;
			}
			else if (a_right > a_cur - 0.15 && a_right > -inf / 2.0)
			{
				want = -1;
			}
			// Only to a lane that also leads to the next road of the route
			if (want != 0 && veh.next_conn >= 0)
			{
				const int target = want > 0 ? l.left : l.right;
				bool good = false;
				const net_connector &cur = m_net->connectors[veh.next_conn];
				for (int cid : m_net->lanes[target].out)
				{
					const net_connector &c = m_net->connectors[cid];
					good = good || (c.allowed && c.to_road == cur.to_road);
				}
				if (!good && dist_end < 300.0)
				{
					want = 0;
				}
			}
		}
		if (want == 0)
		{
			continue;
		}
		const int target = want > 0 ? l.left : l.right;
		if (target < 0)
		{
			continue;
		}
		if (!lane_change_safe(i, target, nullptr))
		{
			if (mandatory && (veh.v < 3.0 || dist_end < 60.0))
			{
				// Zipper merge: the first vehicle in the target lane that is far enough
				// behind the rear of this vehicle gives way for some seconds (a nearer
				// vehicle cannot make the space: it drives on).
				const double s_new = veh.s * m_net->lanes[target].length() / std::max(0.1, l.length());
				for (int wi : m_on_lane[target])
				{
					sim_vehicle &f = m_vehicles[wi];
					if (f.s <= s_new - veh.p->length - lc_gap)
					{
						if (f.courtesy_for < 0 || f.courtesy_until < m_time)
						{
							f.courtesy_for = veh.id;
							f.courtesy_until = m_time + 6.0;
						}
						break;
					}
				}
			}
			continue;
		}
		const net_lane &to = m_net->lanes[target];
		veh.lc_from = l.id;
		veh.lc_progress = 0.0;
		veh.seg = target;
		veh.s = veh.s * to.length() / std::max(0.1, l.length());
		veh.lc_cooldown = 3.0;
		plan_next(veh);
		TE_TRACE("SIM", QStringLiteral("Vehicle %1 changes to lane %2 (%3).").arg(veh.id).arg(to.index)
			.arg(mandatory ? QStringLiteral("necessary") : QStringLiteral("overtaking or keep right")));
		// Keep the lists correct for the next vehicles of this loop.
		build_index();
	}
}

// ---------------------------------------------------------------------------
// vehicles: junction entry
// ---------------------------------------------------------------------------
bool simulation::must_yield(const sim_vehicle &veh, const net_connector &c, int *other) const
{
	const net_lane &l = m_net->lanes[veh.seg];
	const bool lit = light_at_node(c.node_id) != nullptr;
	const vec2 h_me = l.path.tangent_at(l.length());
	const int my_rank = lit ? (c.turn == turn_kind::left ? 1 : 2) : l.rank;
	for (int ccid : c.conflicts)
	{
		const net_connector &cc = m_net->connectors[ccid];
		if (!cc.allowed)
		{
			continue;
		}
		const net_lane &fl = m_net->lanes[cc.from_lane];
		if (fl.road_id == l.road_id && fl.dir == l.dir)
		{
			continue;
		}
		if (lit)
		{
			const signal_color col = signal_for(c.node_id, fl.road_id);
			if (col != signal_color::green && col != signal_color::yellow)
			{
				continue; // the other approach has red
			}
		}
		const int other_rank = lit ? (cc.turn == turn_kind::left ? 1 : 2) : fl.rank;
		if (other_rank < my_rank)
		{
			continue;
		}
		const int wi = front_vehicle_on_lane(fl.id);
		if (wi < 0)
		{
			continue;
		}
		const sim_vehicle &w = m_vehicles[wi];
		if (w.next_conn != cc.id || w.committed)
		{
			continue; // committed vehicles have a reservation (checked separately)
		}
		// Give way only to a vehicle that comes. A waiting vehicle with priority
		// gets its reservation first (the grant order follows the priority);
		// if it cannot go (for example its exit is full), it must not block us.
		const double d2 = fl.stop_line - w.s;
		const bool approaching = w.v > 1.0 && d2 > -1.0 && d2 / std::max(0.1, w.v) < yield_time;
		if (!approaching)
		{
			continue;
		}
		// The other vehicle stops before its line if it can do so comfortably: at
		// a yellow light, or when this vehicle waited too long (it goes first now,
		// the reservation order stops the other one).
		if (d2 > sq(w.v) / (2.0 * w.p->b_comf) + 2.0)
		{
			const bool other_yellow = lit && signal_for(c.node_id, fl.road_id) == signal_color::yellow;
			if (other_yellow || veh.wait_at_line > patience_time)
			{
				continue;
			}
		}
		if (other)
		{
			*other = w.id;
		}
		if (other_rank > my_rank)
		{
			return true;
		}
		// Same rank. Without a light: a left turn gives way to the opposite
		// traffic that goes straight or turns right.
		const vec2 h_other = fl.path.tangent_at(fl.length());
		if (!lit && dot(h_me, h_other) < -0.7)
		{
			const int my_turn = c.turn == turn_kind::left ? 1 : 2;
			const int other_turn = cc.turn == turn_kind::left ? 1 : 2;
			if (other_turn > my_turn)
			{
				return true;
			}
			if (other_turn < my_turn)
			{
				continue;
			}
		}
		// Right-hand rule: the vehicle from the right goes first.
		if (cross(h_me, h_other) > 0.0)
		{
			return true;
		}
	}
	return false;
}

bool simulation::can_enter(const sim_vehicle &veh, bool *must_wait_light, wait_cause *cause, int *cause_id) const
{
	const net_lane &l = m_net->lanes[veh.seg];
	const net_connector &c = m_net->connectors[veh.next_conn];
	const double dist = l.stop_line - veh.s;
	*must_wait_light = false;
	auto fail = [cause, cause_id](wait_cause why, int id)
	{
		*cause = why;
		*cause_id = id;
		return false;
	};

	const signal_color col = signal_for(c.node_id, l.road_id);
	int other = -1;
	if (col != signal_color::off)
	{
		const light_runtime *lr = light_at_node(c.node_id);
		const int light_id = lr ? lr->light_id : -1;
		// A left turner that waited at the line during the green clears the
		// junction in the yellow and the all red time of its phase (if no
		// opposite vehicle has a reservation: see the conflict check).
		const bool clearing = clearing_turn(veh);
		if ((col == signal_color::red || col == signal_color::red_yellow) && !clearing)
		{
			*must_wait_light = true;
			// Only a committed vehicle that cannot stop at all may continue.
			if (!(veh.committed && dist < sq(veh.v) / (2.0 * veh.p->b_max)))
			{
				return fail(wait_cause::red_light, light_id);
			}
		}
		if (col == signal_color::yellow && !clearing)
		{
			// Stop if a comfortable stop is possible.
			if (dist > sq(veh.v) / (2.0 * veh.p->b_comf))
			{
				*must_wait_light = true;
				return fail(wait_cause::red_light, light_id);
			}
		}
		if (must_yield(veh, c, &other))
		{
			return fail(wait_cause::give_way, other);
		}
	}
	else
	{
		if (l.stop_sign && !veh.stop_done)
		{
			return fail(wait_cause::stop_sign, l.sign_id);
		}
		if (must_yield(veh, c, &other))
		{
			return fail(wait_cause::give_way, other);
		}
	}

	// The exit lane must have space for this vehicle and for all vehicles
	// with a reservation to it: do not block the junction. The space is the
	// real space now (a moving queue can stop).
	const net_lane &to = m_net->lanes[c.to_lane];
	// Another vehicle waits for a long time for space in this exit lane: it
	// gets the space first (no starvation of a long vehicle at a slow queue).
	const int claimant = to.id < static_cast<int>(m_space_claim.size()) ? m_space_claim[to.id] : -1;
	if (claimant >= 0 && claimant != veh.id && (veh.v < 0.5 || dist > sq(veh.v) / (2.0 * veh.p->b_comf) + 1.0))
	{
		return fail(wait_cause::exit_full, claimant);
	}
	int last = -1;
	int claims = 0;
	// A vehicle that ends its trip in the exit lane leaves the simulation at its
	// end: it needs no more space than the lane (a very short last road).
	const bool ends_here = to.dead_end || veh.route_pos + 2 >= static_cast<int>(veh.route.size());
	const double free_space = exit_space(veh, to.id, &last, &claims, ends_here);
	const double need = ends_here ? std::min(veh.p->length + veh.p->min_gap + 1.0, std::max(1.0, to.length() - 0.5))
		: veh.p->length + veh.p->min_gap + 1.0;
	if (to.stop_line < need && !ends_here)
	{
		// A link shorter than the vehicle (to the stop line): it must be empty, and the vehicle
		// gets the next junction together with this one (junction_entry).
		if (last >= 0 || claims > 0)
		{
			return fail(wait_cause::exit_full, exit_blocker(to.id, last));
		}
	}
	else
	{
		// A moving queue gives a little more space (1.5 s of its movement), not
		// more: else the queue stops and the vehicle stands in the junction.
		double moving = 0.0;
		if (last >= 0 && m_vehicles[last].v >= 3.0 && m_vehicles[last].a > -0.5)
		{
			moving = m_vehicles[last].v * 1.5;
			// It can stop at its stop line: not more movement than to the stop line
			const sim_vehicle &w = m_vehicles[last];
			if (!w.on_connector && w.seg == to.id && !w.committed)
			{
				moving = std::min(moving, std::max(0.0, to.stop_line - w.s));
			}
		}
		if (free_space + moving < need * (claims + 1))
		{
			return fail(wait_cause::exit_full, exit_blocker(to.id, last));
		}
	}
	for (int wi : m_on_conn[c.id])
	{
		if (m_vehicles[wi].v < 1.0)
		{
			return fail(wait_cause::junction_blocked, m_vehicles[wi].id);
		}
	}
	// Pedestrians on the crosswalks
	if (l.end_crosswalk >= 0 && m_cw_users[l.end_crosswalk] > 0)
	{
		return fail(wait_cause::pedestrians, pedestrian_on_crosswalk(l.end_crosswalk));
	}
	if (to.start_crosswalk >= 0 && m_cw_users[to.start_crosswalk] > 0)
	{
		return fail(wait_cause::pedestrians, pedestrian_on_crosswalk(to.start_crosswalk));
	}
	return true;
}

bool simulation::clearing_turn(const sim_vehicle &veh) const
{
	if (veh.on_connector || veh.next_conn < 0)
	{
		return false;
	}
	const net_lane &l = m_net->lanes[veh.seg];
	const net_connector &c = m_net->connectors[veh.next_conn];
	if ((c.turn != turn_kind::left && c.turn != turn_kind::uturn) || veh.wait_at_line < 2.0 || l.stop_line - veh.s > 1.5)
	{
		return false;
	}
	const light_runtime *lr = light_at_node(c.node_id);
	if (!lr || (lr->stage != 2 && lr->stage != 3))
	{
		return false;
	}
	return phase_has_road(*lr, lr->phase, l.road_id);
}

int simulation::pedestrian_on_crosswalk(int crosswalk_id) const
{
	for (const sim_pedestrian &p : m_peds)
	{
		if (p.on_crosswalk && p.leg_index < static_cast<int>(p.legs.size()) && p.legs[p.leg_index].crosswalk == crosswalk_id)
		{
			return p.id;
		}
	}
	return -1;
}

void simulation::junction_entry(double dt)
{
	struct request
	{
		int vi;
		int prio;      // approach rank and turn: the higher value goes first
		double wait;
		double dist;
		bool can_stop; // the vehicle can still stop comfortably before the line
		bool can_stop_hard = false; // the vehicle can stop with firm braking
	};
	std::vector<request> requests;
	// Space claims: the first vehicle at a stop line that waits longer than the
	// patience time for space in its exit lane (also during its red time). The
	// other vehicles do not enter that lane until it is in (the longest wait wins).
	m_space_claim.assign(m_net->lanes.size(), -1);
	std::vector<double> claim_wait(m_net->lanes.size(), 0.0);
	for (size_t lid = 0; lid < m_on_lane.size(); ++lid)
	{
		if (m_on_lane[lid].empty())
		{
			continue;
		}
		const sim_vehicle &v = m_vehicles[m_on_lane[lid].front()];
		if (v.next_conn < 0 || v.committed || v.wait_at_line < patience_time || m_net->lanes[lid].stop_line - v.s > 6.0)
		{
			continue;
		}
		const int to = m_net->connectors[v.next_conn].to_lane;
		int last = -1;
		int claims = 0;
		const double space = exit_space(v, to, &last, &claims, v.route_pos + 2 >= static_cast<int>(v.route.size()));
		if (m_net->lanes[to].stop_line < v.p->length + v.p->min_gap + 1.0 || space >= (v.p->length + v.p->min_gap + 1.0) * (claims + 1))
		{
			continue; // a short link (other rule), or the space is there
		}
		if (m_space_claim[to] < 0 || claim_wait[to] < v.wait_at_line)
		{
			m_space_claim[to] = v.id;
			claim_wait[to] = v.wait_at_line;
		}
	}
	// The reservations follow the order of the lane: a vehicle behind a vehicle
	// without a reservation cannot use its reservation; it gives it back.
	for (size_t lid = 0; lid < m_on_lane.size(); ++lid)
	{
		bool open = false;
		for (int vi : m_on_lane[lid])
		{
			sim_vehicle &v = m_vehicles[vi];
			if (!v.committed)
			{
				open = true;
			}
			else if (open)
			{
				release_commit(v);
			}
		}
	}
	// A vehicle that holds a reservation of each connector (for the wait causes)
	m_holder.assign(m_net->connectors.size(), -1);
	for (const sim_vehicle &h : m_vehicles)
	{
		if (h.finished)
		{
			continue;
		}
		if (h.held_prev >= 0 && h.held_prev < static_cast<int>(m_holder.size()))
		{
			m_holder[h.held_prev] = h.id;
		}
		if (h.held_conn >= 0 && h.held_conn < static_cast<int>(m_holder.size()))
		{
			m_holder[h.held_conn] = h.id;
		}
		if (h.held_next >= 0 && h.held_next < static_cast<int>(m_holder.size()))
		{
			m_holder[h.held_next] = h.id;
		}
	}
	for (size_t lid = 0; lid < m_on_lane.size(); ++lid)
	{
		const std::vector<int> &list = m_on_lane[lid];
		const net_lane &l = m_net->lanes[lid];
		if (list.empty() || l.dead_end)
		{
			continue;
		}
		// Platoon: a vehicle can ask for a reservation when all vehicles ahead
		// of it on this lane have one. Thus, a queue starts to move together.
		bool blocked = false;
		for (size_t k = 0; k < list.size(); ++k)
		{
			const int vi = list[k];
			sim_vehicle &veh = m_vehicles[vi];
			const double dist = l.stop_line - veh.s;
			if (k == 0)
			{
				if (veh.next_conn < 0)
				{
					// No movement from this lane: wait, then a new route.
					const bool last_road = veh.route_pos + 1 >= static_cast<int>(veh.route.size());
					veh.entry_cause = last_road ? wait_cause::none : wait_cause::no_route;
					veh.entry_id = -1;
					if (!last_road && dist < 5.0 && veh.v < 0.5)
					{
						veh.stuck_time += dt;
						if (veh.stuck_time > reroute_time)
						{
							veh.stuck_time = 0.0;
							reroute(veh);
						}
					}
					break;
				}
				veh.stuck_time = 0.0;
				if (l.stop_sign && !veh.stop_done && dist < 3.0 && veh.v < 0.3)
				{
					veh.stop_wait += dt;
					if (veh.stop_wait >= 1.0)
					{
						veh.stop_done = true;
					}
				}
				if (dist < 6.0 && veh.v < 0.5)
				{
					veh.wait_at_line += dt;
				}
			}
			if (veh.next_conn < 0)
			{
				blocked = true;
			}
			const double range = std::max(25.0, veh.v * 4.0 + 10.0);
			if (dist > range)
			{
				veh.entry_cause = wait_cause::none;
				veh.entry_id = -1;
				break; // the vehicles behind are farther
			}
			// Distance for a comfortable stop. A vehicle that stands (at its line)
			// can always stay: 0.
			const double stop_dist = veh.v < 0.5 ? 0.0 : sq(veh.v) / (2.0 * veh.p->b_comf) + 1.0;
			if (blocked)
			{
				// A vehicle ahead cannot go: the vehicles behind cannot go either.
				veh.entry_cause = wait_cause::none;
				veh.entry_id = -1;
				if (veh.committed && dist > stop_dist)
				{
					release_commit(veh);
				}
				continue;
			}
			bool light_wait = false;
			wait_cause why = wait_cause::none;
			int why_id = -1;
			const bool ok = can_enter(veh, &light_wait, &why, &why_id);
			veh.entry_cause = ok ? wait_cause::none : why;
			veh.entry_id = ok ? -1 : why_id;
			// Diagnostics (debug log): why does the first vehicle wait so long?
			if (!ok && k == 0 && logger::is_on(logger::level_debug) && veh.wait_at_line > 30.0
				&& std::fmod(veh.wait_at_line, 30.0) < dt)
			{
				TE_DEBUG("SIM", QStringLiteral("Vehicle %1 waits %2 s at the end of lane %3 (road %4): %5 (%6)")
					.arg(veh.id).arg(veh.wait_at_line, 0, 'f', 0).arg(lid).arg(l.road_id)
					.arg(QString::fromLatin1(wait_cause_name(why))).arg(why_id));
			}
			// Gridlock rule (set_gridlock_timeout): a loop of full lanes or a
			// priority deadlock must not stop the network for ever. Also a
			// vehicle that can enter but does not get its reservation (a loop
			// of conflicts with the vehicles in the junctions).
			if (k == 0 && !light_wait && m_gridlock_timeout > 0.0 && veh.wait_at_line > m_gridlock_timeout * 0.5)
			{
				if (!ok && !veh.gridlock_rerouted && exit_full(veh, veh.next_conn))
				{
					veh.gridlock_rerouted = true;
					if (veh.committed)
					{
						release_commit(veh);
					}
					if (reroute(veh, true))
					{
						break; // the new movement asks in the next step
					}
				}
				if (veh.wait_at_line > m_gridlock_timeout)
				{
					release(veh);
					veh.finished = true;
					++m_gridlock_removed;
					TE_DEBUG("SIM", QStringLiteral("Gridlock: vehicle %1 leaves the simulation after %2 s at the end of lane %3 (road %4).")
						.arg(veh.id).arg(veh.wait_at_line, 0, 'f', 0).arg(lid).arg(l.road_id));
					break;
				}
			}
			if (veh.committed)
			{
				// The vehicle can still stop comfortably: it gives the reservation
				// back. Not on a short link with its rear in the last junction.
				if (!ok && dist > stop_dist && veh.held_prev < 0)
				{
					release_commit(veh);
					blocked = true;
				}
				continue;
			}
			if (ok)
			{
				const net_connector &c = m_net->connectors[veh.next_conn];
				const int move_rank = c.turn == turn_kind::left ? 1 : 2;
				int prio = light_at_node(c.node_id) ? move_rank : l.rank * 3 + move_rank;
				if (veh.wait_at_line > patience_time)
				{
					prio += 10; // waited too long: before the other vehicles (no starvation)
				}
				if (clearing_turn(veh))
				{
					prio = 100; // clears the junction at the end of its green
				}
				request rq = { vi, prio, veh.wait_at_line, dist, dist > stop_dist };
				rq.can_stop_hard = veh.v < 0.5 || dist > sq(veh.v) / (2.0 * 0.75 * veh.p->b_max) + 1.0;
				requests.push_back(rq);
			}
			break; // the next vehicle asks in the next step, if this one gets the reservation
		}
	}
	std::sort(requests.begin(), requests.end(), [](const request &a, const request &b)
	{
		if (a.prio != b.prio)
		{
			return a.prio > b.prio;
		}
		return a.wait > b.wait || (a.wait == b.wait && a.dist < b.dist);
	});
	// Fair order: a request that must wait for a conflicting movement keeps a
	// claim on its connector for this step. A later request (lower priority or
	// shorter wait) that conflicts with a claim also waits, if it can still stop.
	// Thus, a continuous flow on one movement cannot keep a waiting vehicle out
	// of the junction for ever.
	m_claimer.assign(m_net->connectors.size(), -1);
	m_claim_long.assign(m_net->connectors.size(), 0);
	m_claim_wait.assign(m_net->connectors.size(), 0.0);
	m_claim_merge.assign(m_net->connectors.size(), 0);
	// merge_only: the vehicle cannot go now for another reason (no space after
	// the junction): its claim stops only the vehicles to the same exit lane,
	// not the crossing traffic.
	auto claim = [this](int conn, const sim_vehicle &v, bool merge_only)
	{
		if (m_claimer[conn] < 0)
		{
			m_claimer[conn] = v.id;
			m_claim_long[conn] = v.p->length > long_vehicle ? 1 : 0;
			m_claim_wait[conn] = v.wait_at_line;
			m_claim_merge[conn] = merge_only ? 1 : 0;
		}
	};
	for (const request &r : requests)
	{
		sim_vehicle &veh = m_vehicles[r.vi];
		const net_connector &c = m_net->connectors[veh.next_conn];
		const bool is_long = veh.p->length > long_vehicle;
		int conflict = -1;
		int other = -1;
		for (int ccid : c.conflicts)
		{
			if (m_reservations[ccid] > 0)
			{
				conflict = ccid;
				other = m_holder[ccid];
				break;
			}
		}
		// A long vehicle cuts the curve: the wide conflicts, if this vehicle or
		// the other vehicle is long
		for (size_t k = 0; conflict < 0 && k < c.conflicts_long.size(); ++k)
		{
			const int ccid = c.conflicts_long[k];
			if (is_long ? m_reservations[ccid] > 0 : m_res_long[ccid] > 0)
			{
				conflict = ccid;
				other = m_holder[ccid];
			}
		}
		if (conflict < 0 && (r.can_stop || r.can_stop_hard))
		{
			// The claim of a vehicle that waited very long also stops a vehicle
			// that can stop only with firm braking (else a dense platoon never
			// leaves a gap).
			auto blocks = [&](int ccid)
			{
				if (m_claim_merge[ccid] && m_net->connectors[ccid].to_lane != c.to_lane)
				{
					return false; // a merge claim does not stop the crossing traffic
				}
				return r.can_stop || m_claim_wait[ccid] > 2.0 * patience_time;
			};
			for (int ccid : c.conflicts)
			{
				if (m_claimer[ccid] >= 0 && blocks(ccid))
				{
					conflict = ccid;
					other = m_claimer[ccid];
					break;
				}
			}
			for (size_t k = 0; conflict < 0 && k < c.conflicts_long.size(); ++k)
			{
				const int ccid = c.conflicts_long[k];
				if (m_claimer[ccid] >= 0 && (is_long || m_claim_long[ccid]) && blocks(ccid))
				{
					conflict = ccid;
					other = m_claimer[ccid];
				}
			}
		}
		if (conflict >= 0)
		{
			claim(c.id, veh, false);
			veh.entry_cause = wait_cause::conflict;
			veh.entry_id = other;
			if (logger::is_on(logger::level_debug) && veh.wait_at_line > 30.0 && std::fmod(veh.wait_at_line, 30.0) < m_dt)
			{
				TE_DEBUG("SIM", QStringLiteral("Vehicle %1 waits %2 s: connector %3 has a reservation or a claim of vehicle %4")
					.arg(veh.id).arg(veh.wait_at_line, 0, 'f', 0).arg(conflict).arg(other));
			}
			continue;
		}
		// A link shorter than the vehicle (for example between the two carriageways
		// of a crossing road): the vehicle gets this junction only together with
		// the next one. Thus, it never stops with its rear in this junction.
		int c2 = -1;
		const net_lane &link = m_net->lanes[c.to_lane];
		if (link.stop_line < veh.p->length + veh.p->min_gap + 1.0 && !link.dead_end && veh.route_pos + 2 < static_cast<int>(veh.route.size()))
		{
			c2 = choose_connector(link, veh.route[veh.route_pos + 2]);
			wait_cause why2 = wait_cause::none;
			int id2 = -1;
			if (c2 >= 0 && !through_ok(veh, c2, &why2, &id2))
			{
				// Only a conflict at the next junction gets a full claim there; the
				// first junction stays open for the crossing traffic.
				const bool conflict2 = why2 == wait_cause::conflict || why2 == wait_cause::give_way;
				claim(c.id, veh, true);
				claim(c2, veh, !conflict2);
				veh.entry_cause = why2;
				veh.entry_id = id2;
				continue;
			}
		}
		veh.entry_cause = wait_cause::none;
		veh.entry_id = -1;
		// On a short road the rear of the vehicle can still be in the last
		// junction: keep that reservation until the rear leaves it.
		if (veh.held_conn >= 0)
		{
			if (veh.held_prev >= 0)
			{
				unreserve(veh, veh.held_prev);
			}
			veh.held_prev = veh.held_conn;
			veh.held_conn = -1;
		}
		veh.committed = true;
		veh.held_conn = veh.next_conn;
		reserve(veh, veh.next_conn);
		m_holder[veh.next_conn] = veh.id;
		if (c2 >= 0)
		{
			veh.held_next = c2;
			reserve(veh, c2);
			m_holder[c2] = veh.id;
		}
	}
}

// ---------------------------------------------------------------------------
// vehicles: acceleration
// ---------------------------------------------------------------------------
double simulation::idm(const sim_vehicle &veh, double v0, double gap, double dv, double s0) const
{
	const vehicle_params &p = *veh.p;
	const double v = veh.v;
	const double free_term = std::pow(v / std::max(0.5, v0), 4.0);
	double inter = 0.0;
	if (gap < inf / 2.0)
	{
		const double s_min = s0 >= 0.0 ? s0 : p.min_gap;
		const double s_star = s_min + std::max(0.0, v * p.headway + v * dv / (2.0 * std::sqrt(p.a_max * p.b_comf)));
		inter = s_star / std::max(0.05, gap);
	}
	return p.a_max * (1.0 - free_term - inter * inter);
}

double simulation::desired_speed(const sim_vehicle &veh) const
{
	double limit = 0.0;
	if (veh.on_connector)
	{
		const net_connector &c = m_net->connectors[veh.seg];
		limit = std::min(c.max_speed, m_net->lanes[c.to_lane].limit_at(0.0));
	}
	else
	{
		limit = m_net->lanes[veh.seg].limit_at(veh.s);
	}
	return std::min(veh.p->max_speed, limit * veh.v0_factor);
}

double simulation::speed_constraint_accel(const sim_vehicle &veh) const
{
	double a = inf;
	const double v = veh.v;
	if (veh.on_connector)
	{
		return a;
	}
	const net_lane &l = m_net->lanes[veh.seg];
	for (const net_zone &z : l.limits)
	{
		const double vt = z.value * veh.v0_factor;
		if (z.s > veh.s && vt < v)
		{
			a = std::min(a, (sq(vt) - sq(v)) / (2.0 * std::max(0.5, z.s - veh.s)));
		}
	}
	if (veh.next_conn >= 0)
	{
		const double vt = m_net->connectors[veh.next_conn].max_speed * veh.v0_factor;
		if (vt < v)
		{
			a = std::min(a, (sq(vt) - sq(v)) / (2.0 * std::max(0.5, l.length() - veh.s)));
		}
	}
	return a;
}

double simulation::leader_gap(int vi, double *leader_v, int *leader) const
{
	const sim_vehicle &veh = m_vehicles[vi];
	double gap = inf;
	*leader_v = 0.0;
	int lead = -1;
	auto consider = [&](const sim_vehicle &w, double g)
	{
		if (g < gap)
		{
			gap = g;
			*leader_v = w.v;
			lead = static_cast<int>(&w - m_vehicles.data());
		}
	};
	auto result = [&]()
	{
		if (leader)
		{
			*leader = lead;
		}
		return gap;
	};
	// Zipper merge: a vehicle in the next lane that must come into this lane
	if (veh.courtesy_for >= 0 && veh.courtesy_until > m_time && !veh.on_connector)
	{
		const sim_vehicle *mv = find_vehicle(veh.courtesy_for);
		const net_lane &own = m_net->lanes[veh.seg];
		if (mv && !mv->on_connector)
		{
			const net_lane &ml = m_net->lanes[mv->seg];
			if (ml.left == own.id || ml.right == own.id)
			{
				// Keep the space for the lane change behind the rear of the other
				// vehicle. A vehicle that is already beside it cannot make space:
				// it drives on (the next vehicle behind gives way).
				const double ms = mv->s * own.length() / std::max(0.1, ml.length());
				const double behind = ms - mv->p->length - veh.s;
				if (behind >= lc_gap - 0.3)
				{
					consider(*mv, behind - 1.0);
				}
			}
		}
	}
	if (!veh.on_connector)
	{
		const net_lane &l = m_net->lanes[veh.seg];
		// A vehicle that changes from this lane to another lane: it is still
		// partly in this lane (the drawing moves it in 2.5 s).
		for (int wi : m_leaving[l.id])
		{
			const sim_vehicle &w = m_vehicles[wi];
			if (wi == vi)
			{
				continue;
			}
			const double ws = w.s * l.length() / std::max(0.1, m_net->lanes[w.seg].length());
			if (ws > veh.s)
			{
				consider(w, ws - w.p->length - veh.s);
			}
		}
		const std::vector<int> &list = m_on_lane[l.id];
		const auto it = std::find(list.begin(), list.end(), vi);
		if (it != list.begin() && it != list.end())
		{
			const sim_vehicle &w = m_vehicles[*(it - 1)];
			consider(w, w.s - w.p->length - veh.s);
			return result();
		}
		const double to_end = l.length() - veh.s;
		const net_connector *mine = veh.next_conn >= 0 ? &m_net->connectors[veh.next_conn] : nullptr;
		// Vehicles that left this lane: on my connector, with the rear still at the
		// end of the lane, or on a path that is still near my path
		for (int cid : l.out)
		{
			const std::vector<int> &cl = m_on_conn[cid];
			if (cl.empty())
			{
				continue;
			}
			const sim_vehicle &w = m_vehicles[cl.back()];
			const double rear = w.s - w.p->length;
			if (cid == veh.next_conn || rear < 2.0 || (mine && paths_close(*mine, m_net->connectors[cid], rear, veh.p->width + w.p->width)))
			{
				consider(w, to_end + rear);
			}
		}
		if (mine && gap >= inf / 2.0)
		{
			int w = -1;
			const double rear = first_rear(mine->to_lane, &w, 2, &veh);
			if (w >= 0)
			{
				consider(m_vehicles[w], to_end + mine->path.length() + rear);
			}
		}
	}
	else
	{
		const net_connector &c = m_net->connectors[veh.seg];
		const std::vector<int> &list = m_on_conn[c.id];
		const auto it = std::find(list.begin(), list.end(), vi);
		if (it != list.begin() && it != list.end())
		{
			const sim_vehicle &w = m_vehicles[*(it - 1)];
			consider(w, w.s - w.p->length - veh.s);
			return result();
		}
		// Vehicles on the other connectors from the same lane, until the paths separate
		for (int cid : m_net->lanes[c.from_lane].out)
		{
			if (cid == c.id)
			{
				continue;
			}
			for (int wi : m_on_conn[cid])
			{
				const sim_vehicle &w = m_vehicles[wi];
				const double rear = w.s - w.p->length;
				if (rear > veh.s - 0.5 && paths_close(c, m_net->connectors[cid], rear, veh.p->width + w.p->width))
				{
					consider(w, rear - veh.s);
				}
			}
		}
		// The exit lane: also the vehicles whose rear is still in it (short links)
		int w = -1;
		const double rear = first_rear(c.to_lane, &w, 2, &veh);
		if (w >= 0)
		{
			consider(m_vehicles[w], c.path.length() - veh.s + rear);
		}
	}
	return result();
}

bool simulation::paths_close(const net_connector &a, const net_connector &b, double s, double widths) const
{
	if (s <= 0.0)
	{
		return true;
	}
	const vec2 pa = a.path.point_at(std::min(s, a.path.length()));
	const vec2 pb = b.path.point_at(std::min(s, b.path.length()));
	return (pa - pb).length() < widths * 0.5 + 0.5;
}

double simulation::first_rear(int lane_id, int *who, int depth, const sim_vehicle *veh) const
{
	const net_lane &l = m_net->lanes[lane_id];
	double best = inf;
	*who = -1;
	auto take = [&](int wi, double rear)
	{
		if (rear < best)
		{
			best = rear;
			*who = wi;
		}
	};
	const int last = last_vehicle_on_lane(lane_id);
	if (last >= 0)
	{
		take(last, m_vehicles[last].s - m_vehicles[last].p->length);
	}
	if (lane_id < static_cast<int>(m_leaving.size()))
	{
		for (int wi : m_leaving[lane_id])
		{
			const sim_vehicle &w = m_vehicles[wi];
			take(wi, w.s * l.length() / std::max(0.1, m_net->lanes[w.seg].length()) - w.p->length);
		}
	}
	// The connector to the next road of the route of veh after this lane
	int pref = -1;
	if (veh)
	{
		const int k = veh->route_pos + 2;
		if (k < static_cast<int>(veh->route.size()))
		{
			for (int cid : l.out)
			{
				const net_lane &to = m_net->lanes[m_net->connectors[cid].to_lane];
				if (road_network::dir_key(to.road_id, to.dir) == veh->route[k])
				{
					pref = cid;
					break;
				}
			}
		}
	}
	for (int cid : l.out)
	{
		const net_connector &c = m_net->connectors[cid];
		const std::vector<int> &cl = m_on_conn[cid];
		if (!cl.empty())
		{
			const sim_vehicle &w = m_vehicles[cl.back()];
			const double rear = l.length() + w.s - w.p->length;
			if (rear < l.length() + 0.5 || cid == pref)
			{
				take(cl.back(), rear);
			}
		}
		else if (cid == pref && depth > 0 && best >= inf / 2.0 && l.length() + c.path.length() < 60.0)
		{
			// A short link: look into the next lane
			int w2 = -1;
			const double r2 = first_rear(c.to_lane, &w2, depth - 1, nullptr);
			if (w2 >= 0)
			{
				take(w2, l.length() + c.path.length() + r2);
			}
		}
	}
	return best;
}

double simulation::obstacle_gap(int vi, wait_cause *cause, int *cause_id) const
{
	const sim_vehicle &veh = m_vehicles[vi];
	double g = inf;
	wait_cause why = wait_cause::none;
	int why_id = -1;
	auto take = [&](double d, wait_cause c, int id)
	{
		if (d < g)
		{
			g = d;
			why = c;
			why_id = id;
		}
	};
	if (!veh.on_connector)
	{
		const net_lane &l = m_net->lanes[veh.seg];
		const bool last_road = veh.route_pos + 1 >= static_cast<int>(veh.route.size());
		// Stop line of the junction (no reservation = stop): the cause comes from the junction entry
		if (!l.dead_end && !(last_road && l.dead_end) && !veh.committed)
		{
			take(l.stop_line - veh.s - 0.2, veh.entry_cause, veh.entry_id);
		}
		// Cones and barriers
		for (const net_block &b : l.blocks)
		{
			if (b.s1 > veh.s - 0.5 && b.s0 > veh.s - veh.p->length)
			{
				take(b.s0 - veh.s - 1.0, wait_cause::lane_blocked, b.object_id);
			}
		}
		// Level crossings (warning lights and barriers)
		for (const net_stop_point &lc : l.level_crossings)
		{
			const double stop_s = lc.s - 6.0;
			if (stop_s > veh.s - 0.5)
			{
				const crossing_state st = level_crossing(lc.object_id, nullptr, nullptr);
				if (st == crossing_state::closed || st == crossing_state::closing)
				{
					const double d = stop_s - veh.s;
					if (st == crossing_state::closed || d > sq(veh.v) / (2.0 * veh.p->b_max))
					{
						take(d, wait_cause::level_crossing, lc.object_id);
					}
				}
			}
		}
		// Bus stop
		if (veh.kind == vehicle_kind::bus)
		{
			for (const net_stop_point &st : l.bus_stops)
			{
				if (st.object_id != veh.served_stop && st.s > veh.s - 1.0)
				{
					take(st.s - veh.s, wait_cause::bus_stop, st.object_id);
					break;
				}
			}
		}
		// Crosswalk at the start of the lane
		if (l.start_crosswalk >= 0 && veh.s < l.start_cw_s0 && m_cw_users[l.start_crosswalk] > 0)
		{
			take(l.start_cw_s0 - veh.s - 0.5, wait_cause::pedestrians, pedestrian_on_crosswalk(l.start_crosswalk));
		}
	}
	else
	{
		const net_connector &c = m_net->connectors[veh.seg];
		const net_lane &to = m_net->lanes[c.to_lane];
		if (to.start_crosswalk >= 0 && m_cw_users[to.start_crosswalk] > 0)
		{
			take(c.path.length() - veh.s + to.start_cw_s0 - 0.5, wait_cause::pedestrians, pedestrian_on_crosswalk(to.start_crosswalk));
		}
	}
	if (cause)
	{
		*cause = why;
	}
	if (cause_id)
	{
		*cause_id = why_id;
	}
	return g;
}

void simulation::accelerations()
{
	for (int i = 0; i < static_cast<int>(m_vehicles.size()); ++i)
	{
		sim_vehicle &veh = m_vehicles[i];
		if (veh.finished)
		{
			continue;
		}
		const double v0 = desired_speed(veh);
		double lead_v = 0.0;
		int lead = -1;
		const double gap_l = leader_gap(i, &lead_v, &lead);
		const double a_lead = idm(veh, v0, gap_l, veh.v - lead_v);
		double a = a_lead;
		wait_cause obstacle = wait_cause::none;
		int obstacle_id = -1;
		const double gap_o = obstacle_gap(i, &obstacle, &obstacle_id);
		double a_obst = inf;
		if (gap_o < inf / 2.0)
		{
			// Fixed obstacle: stop near it (stop line, bus stop), not s0 before it.
			a_obst = idm(veh, v0, gap_o, veh.v, 0.4);
			a = std::min(a, a_obst);
		}
		a = std::min(a, speed_constraint_accel(veh));
		if (veh.dwell_left > 0.0)
		{
			a = -veh.p->b_max;
		}
		veh.a = std::max(-veh.p->b_max, std::min(veh.p->a_max, a));
		m_gap_cache[i] = std::min(gap_l, gap_o);

		// The cause of a wait or a slow down: the vehicle ahead or the fixed obstacle
		// that limits the acceleration (not the free road, not a curve).
		veh.cause = wait_cause::none;
		veh.cause_id = -1;
		const double a_free = idm(veh, v0, inf, 0.0);
		const bool limited = veh.v < 1.0 || a < a_free - 0.5;
		if (veh.dwell_left > 0.0)
		{
			veh.cause = wait_cause::bus_stop;
			veh.cause_id = veh.dwell_stop;
		}
		else if (limited && a_obst <= a_lead && obstacle != wait_cause::none)
		{
			veh.cause = obstacle;
			veh.cause_id = obstacle_id;
		}
		else if (limited && lead >= 0 && a_lead < a_free - 0.1)
		{
			const sim_vehicle &w = m_vehicles[lead];
			const bool courtesy = veh.courtesy_for == w.id && veh.courtesy_until > m_time && !veh.on_connector
				&& !w.on_connector && w.seg != veh.seg;
			veh.cause = courtesy ? wait_cause::merge_courtesy : wait_cause::queue;
			veh.cause_id = w.id;
		}
	}
}

// ---------------------------------------------------------------------------
// vehicles: movement
// ---------------------------------------------------------------------------
void simulation::finish(sim_vehicle &veh)
{
	release(veh);
	veh.finished = true;
	++m_finished_count;
	m_finished_distance += veh.distance;
	m_finished_fuel += veh.fuel_l;
	m_finished_co2 += veh.co2_kg;
	m_finished_stops += veh.stops;
	m_finished_stop_time += veh.stop_time;
	m_finished_congested_time += veh.congested_time;
	m_finished_congested_distance += veh.congested_distance;
	m_finished_light_wait += veh.light_wait;
	m_finish_times.push_back(m_time);
	TE_TRACE("SIM", QStringLiteral("Vehicle %1 completed its trip: %2 m, %3 s, %4 L fuel, %5 stops.")
		.arg(veh.id).arg(veh.distance, 0, 'f', 0).arg(m_time - veh.spawn_time, 0, 'f', 0)
		.arg(veh.fuel_l, 0, 'f', 3).arg(veh.stops));
}

void simulation::move(double dt)
{
	for (int i = 0; i < static_cast<int>(m_vehicles.size()); ++i)
	{
		sim_vehicle &veh = m_vehicles[i];
		if (veh.finished)
		{
			continue;
		}
		const vehicle_params &p = *veh.p;

		// Bus at a bus stop
		if (veh.kind == vehicle_kind::bus && !veh.on_connector)
		{
			const net_lane &l = m_net->lanes[veh.seg];
			if (veh.dwell_left > 0.0)
			{
				veh.dwell_left -= dt;
				veh.v = 0.0;
				veh.a = 0.0;
				if (veh.dwell_left <= 0.0)
				{
					veh.served_stop = veh.dwell_stop;
					const int alight = static_cast<int>(m_rng() % static_cast<unsigned>(std::max(1, veh.passengers / 3 + 1)));
					const int board = std::min(bus_stop_waiting(veh.dwell_stop), p.max_passengers - (veh.passengers - alight));
					veh.passengers = std::max(1, veh.passengers - alight + std::max(0, board));
					m_stop_waiting[veh.dwell_stop] = 0;
					veh.dwell_stop = -1;
				}
			}
			else
			{
				for (const net_stop_point &st : l.bus_stops)
				{
					if (st.object_id != veh.served_stop && std::abs(st.s - veh.s) < 1.2 && veh.v < 0.3)
					{
						const object_data *o = m_model->object(st.object_id);
						veh.dwell_left = (o ? o->dwell_time : 20.0) * (0.7 + 0.6 * random01());
						veh.dwell_stop = st.object_id;
						break;
					}
				}
			}
		}

		const double a = veh.a;
		const double v0 = veh.v;
		double v1 = v0 + a * dt;
		double ds = 0.0;
		if (v1 < 0.0)
		{
			ds = a < 0.0 ? -v0 * v0 / (2.0 * a) : 0.0;
			v1 = 0.0;
		}
		else
		{
			ds = v0 * dt + 0.5 * a * dt * dt;
		}
		ds = std::max(0.0, ds);
		// Guard: never into the leader or over a stop point (no accidents, no red light runs)
		const double gmax = m_gap_cache[i];
		if (ds > gmax)
		{
			ds = std::max(0.0, gmax);
			v1 = std::min(v1, ds / dt);
		}
		veh.s += ds;
		veh.v = v1;
		veh.distance += ds;

		// Fuel and statistics
		const double fuel = vehicle_types::fuel_rate(p, v1, a);
		veh.fuel_rate = fuel;
		veh.fuel_l += fuel * dt;
		veh.co2_kg += fuel * dt * p.co2_per_liter;
		if (veh.v > 2.0)
		{
			veh.moving = true;
		}
		if (veh.moving && veh.v < 0.3)
		{
			++veh.stops;
			veh.moving = false;
		}
		if (veh.v < 0.3)
		{
			veh.stop_time += dt;
		}
		veh.standstill = veh.v < 0.2 ? veh.standstill + dt : 0.0;
		if (veh.v < 10.0 / 3.6)
		{
			veh.congested_time += dt;
			veh.congested_distance += ds;
		}
		if (!veh.on_connector)
		{
			const net_lane &l = m_net->lanes[veh.seg];
			if (!l.dead_end && l.stop_line - veh.s < 60.0 && veh.v < 2.0)
			{
				veh.junction_wait += dt;
				if (light_at_node(l.to_node))
				{
					veh.light_wait += dt;
				}
			}
		}
		if (veh.lc_progress < 1.0)
		{
			veh.lc_progress = std::min(1.0, veh.lc_progress + dt / 2.5);
		}

		// Segment transfers
		for (int guard = 0; guard < 4; ++guard)
		{
			if (!veh.on_connector)
			{
				const net_lane &l = m_net->lanes[veh.seg];
				if (veh.s < l.length())
				{
					break;
				}
				const bool last_road = veh.route_pos + 1 >= static_cast<int>(veh.route.size());
				if (l.dead_end || (last_road && veh.next_conn < 0))
				{
					finish(veh);
					break;
				}
				if (veh.next_conn >= 0 && veh.committed)
				{
					veh.s -= l.length();
					veh.prev_seg = l.id;
					veh.prev_on_connector = false;
					veh.on_connector = true;
					veh.seg = veh.next_conn;
					veh.committed = false;
					veh.lc_from = -1;
					veh.lc_progress = 1.0;
					m_junction_waits.push_back(std::make_pair(m_time, veh.junction_wait));
					m_passages.push_back({ m_time, m_net->connectors[veh.next_conn].node_id, l.road_id, veh.junction_wait });
					veh.junction_wait = 0.0;
					veh.wait_at_line = 0.0;
					veh.gridlock_rerouted = false;
					veh.stop_done = false;
					veh.stop_wait = 0.0;
					veh.entry_cause = wait_cause::none;
					veh.entry_id = -1;
					continue;
				}
				veh.s = l.length() - 0.01; // no reservation: wait (not normal)
				veh.v = 0.0;
				break;
			}
			const net_connector &c = m_net->connectors[veh.seg];
			if (veh.s < c.path.length())
			{
				break;
			}
			veh.s -= c.path.length();
			veh.prev_seg = c.id;
			veh.prev_on_connector = true;
			veh.on_connector = false;
			veh.seg = c.to_lane;
			++veh.route_pos;
			veh.served_stop = -1;
			plan_next(veh);
			// After a short link: the reservation of the next junction is ready.
			if (veh.held_next >= 0)
			{
				if (veh.next_conn == veh.held_next)
				{
					if (veh.held_prev >= 0)
					{
						unreserve(veh, veh.held_prev);
					}
					veh.held_prev = veh.held_conn;
					veh.held_conn = veh.held_next;
					veh.committed = true;
				}
				else
				{
					unreserve(veh, veh.held_next);
				}
				veh.held_next = -1;
			}
		}
		// Free a reservation when the rear of the vehicle left the connector.
		if (!veh.finished && veh.held_prev >= 0 && !uses_connector(veh, veh.held_prev))
		{
			unreserve(veh, veh.held_prev);
			veh.held_prev = -1;
		}
		if (!veh.finished && veh.held_conn >= 0 && !uses_connector(veh, veh.held_conn))
		{
			release_commit(veh);
		}
	}
	m_vehicles.erase(std::remove_if(m_vehicles.begin(), m_vehicles.end(),
		[](const sim_vehicle &v) { return v.finished; }), m_vehicles.end());
}

void simulation::update_waiting()
{
	for (auto &kv : m_lights)
	{
		kv.second.waiting.clear();
	}
	for (size_t lid = 0; lid < m_on_lane.size(); ++lid)
	{
		if (m_on_lane[lid].empty())
		{
			continue;
		}
		const net_lane &l = m_net->lanes[lid];
		const auto it = m_node_light.find(l.to_node);
		if (it == m_node_light.end())
		{
			continue;
		}
		int count = 0;
		for (int vi : m_on_lane[lid])
		{
			const sim_vehicle &v = m_vehicles[vi];
			if (v.v < 1.5 && l.stop_line - v.s < 80.0)
			{
				++count;
			}
		}
		m_lights[it->second].waiting[l.road_id] += count;
	}
	// Bus stops: passengers come to the stop
	for (const auto &kv : m_model->objects)
	{
		if (kv.second.kind != object_kind::bus_stop)
		{
			continue;
		}
		double &acc = m_stop_accum[kv.first];
		acc += m_dt * 0.02 * demand_profile(clock()) * 3.0;  // about 3.6 persons per minute at the peak
		while (acc >= 1.0)
		{
			acc -= 1.0;
			if (m_stop_waiting[kv.first] < 60)
			{
				++m_stop_waiting[kv.first];
			}
		}
	}
}

void simulation::update_positions(double dt)
{
	Q_UNUSED(dt);
	for (sim_vehicle &v : m_vehicles)
	{
		const polyline &cur = v.on_connector ? m_net->connectors[v.seg].path : m_net->lanes[v.seg].path;
		vec2 front = cur.point_at(v.s);
		const double rear_s = v.s - v.p->length;
		vec2 rear;
		if (rear_s >= 0.0 || v.prev_seg < 0)
		{
			rear = cur.point_at(rear_s);
		}
		else
		{
			const polyline &prev = v.prev_on_connector ? m_net->connectors[v.prev_seg].path : m_net->lanes[v.prev_seg].path;
			const double prev_s = prev.length() + rear_s;
			if (prev_s < 0.0 && v.prev_on_connector)
			{
				// A short connector: the rear is still in the lane before it. (The
				// line of the curve must not continue to the back: the rear of a
				// bus would turn into the next lane.)
				const polyline &before = m_net->lanes[m_net->connectors[v.prev_seg].from_lane].path;
				rear = before.point_at(before.length() + prev_s);
			}
			else
			{
				rear = prev.point_at(prev_s);
			}
		}
		if (v.lc_from >= 0 && v.lc_progress < 1.0 && !v.on_connector)
		{
			const net_lane &from = m_net->lanes[v.lc_from];
			const net_lane &to = m_net->lanes[v.seg];
			const double k = from.length() / std::max(0.1, to.length());
			const vec2 off = from.path.point_at(v.s * k) - front;
			const double w = 1.0 - geom::smooth01(v.lc_progress);
			// The rear follows a little later: the vehicle turns into the new lane.
			const double w_rear = 1.0 - geom::smooth01(v.lc_progress - 0.15);
			rear = rear + (from.path.point_at(rear_s * k) - cur.point_at(rear_s)) * w_rear;
			front = front + off * w;
		}
		v.pos = (front + rear) * 0.5;
		const vec2 d = front - rear;
		if (d.length_sq() > 1e-6)
		{
			v.dir = d.normalized();
		}
	}
}

void simulation::collect_stats(double dt)
{
	m_stats_accum += dt;
	if (m_stats_accum < m_stats_interval)
	{
		return;
	}
	m_stats_accum = 0.0;

	stats_sample s;
	s.time = m_time;
	s.clock = clock();
	s.values.assign(stats_collector::metric_count, 0.0);

	const double n = static_cast<double>(m_vehicles.size());
	double sum_v = 0.0;
	double stopped = 0.0;
	double fuel_rate = 0.0;
	double co2_rate = 0.0;
	double dist = m_finished_distance;
	double fuel = m_finished_fuel;
	double co2 = m_finished_co2;
	double stops = m_finished_stops;
	double stop_time = m_finished_stop_time;
	double cong_t = m_finished_congested_time;
	double cong_d = m_finished_congested_distance;
	double light_wait = m_finished_light_wait;
	for (const sim_vehicle &v : m_vehicles)
	{
		sum_v += v.v;
		stopped += v.v < 0.3 ? 1.0 : 0.0;
		fuel_rate += v.fuel_rate;
		co2_rate += v.fuel_rate * v.p->co2_per_liter;
		dist += v.distance;
		fuel += v.fuel_l;
		co2 += v.co2_kg;
		stops += v.stops;
		stop_time += v.stop_time;
		cong_t += v.congested_time;
		cong_d += v.congested_distance;
		light_wait += v.light_wait;
	}
	const double all = std::max(1.0, n + static_cast<double>(m_finished_count));

	// Old entries of the time windows
	while (!m_finish_times.empty() && m_finish_times.front() < m_time - junction_window)
	{
		m_finish_times.erase(m_finish_times.begin());
	}
	while (!m_junction_waits.empty() && m_junction_waits.front().first < m_time - junction_window)
	{
		m_junction_waits.erase(m_junction_waits.begin());
	}
	double jw = 0.0;
	for (const auto &w : m_junction_waits)
	{
		jw += w.second;
	}

	s.values[stats_collector::m_vehicles] = n;
	s.values[stats_collector::m_pedestrians] = static_cast<double>(m_peds.size());
	s.values[stats_collector::m_density] = n / std::max(0.001, m_net->total_lane_km);
	s.values[stats_collector::m_mean_speed] = n > 0.0 ? sum_v / n * 3.6 : 0.0;
	s.values[stats_collector::m_stopped] = stopped;
	s.values[stats_collector::m_stops_per_vehicle] = stops / all;
	s.values[stats_collector::m_mean_stop_time] = stops > 0.0 ? stop_time / stops : 0.0;
	s.values[stats_collector::m_junction_wait] = m_junction_waits.empty() ? 0.0 : jw / m_junction_waits.size();
	s.values[stats_collector::m_light_wait] = light_wait / all;
	s.values[stats_collector::m_congested_time] = cong_t / all;
	s.values[stats_collector::m_congested_distance] = cong_d / all;
	s.values[stats_collector::m_fuel_per_100km] = dist > 100.0 ? fuel / (dist / 1000.0) * 100.0 : 0.0;
	s.values[stats_collector::m_fuel_rate] = fuel_rate * 3600.0;
	s.values[stats_collector::m_co2_rate] = co2_rate * 3600.0;
	s.values[stats_collector::m_co2_total] = co2;
	s.values[stats_collector::m_throughput] = static_cast<double>(m_finish_times.size()) * 3600.0 / std::min(junction_window, std::max(1.0, m_time));
	m_stats.add_sample(s);

	for (const auto &kv : m_lights)
	{
		int total = 0;
		for (const auto &w : kv.second.waiting)
		{
			total += w.second;
		}
		m_stats.add_light_sample(kv.first, m_time, total);
	}
}
