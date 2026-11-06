// Congestion analysis of the simulation: queues, delays, level of service and
// the main cause of the waits at each junction (simulation::congestion()).

#include "simulation.h"
#include "road_network.h"
#include "world_model.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace
{
	const double queue_speed = 2.0;     // m/s: slower vehicles are in a queue
	const double queue_gap = 12.0;      // m: a larger gap ends the queue
	const double head_range = 15.0;     // m: the first vehicle of a queue is this near to the stop line

	int los_rank(char los)
	{
		return los - 'A';
	}
}

char simulation::los_letter(double delay, bool signalized)
{
	static const double lit[5] = { 10.0, 20.0, 35.0, 55.0, 80.0 };
	static const double unlit[5] = { 10.0, 15.0, 25.0, 35.0, 50.0 };
	const double *limits = signalized ? lit : unlit;
	for (int i = 0; i < 5; ++i)
	{
		if (delay <= limits[i])
		{
			return static_cast<char>('A' + i);
		}
	}
	return 'F';
}

const junction_status *simulation::congestion_at(int node_id) const
{
	for (const junction_status &j : m_congestion)
	{
		if (j.node_id == node_id)
		{
			return &j;
		}
	}
	return nullptr;
}

void simulation::update_congestion(double dt)
{
	m_cong_accum += dt;
	if (m_cong_accum < 1.0 || !m_net)
	{
		return;
	}
	const double sample_dt = m_cong_accum;
	m_cong_accum = 0.0;
	const double since = m_time - m_cong_window;
	while (!m_passages.empty() && m_passages.front().time < since)
	{
		m_passages.pop_front();
	}
	while (!m_wait_samples.empty() && m_wait_samples.front().time < since)
	{
		m_wait_samples.pop_front();
	}

	// Passages in the time window: (node, road) -> (count, sum of the waits)
	std::map<std::pair<int, int>, std::pair<int, double>> passed;
	for (const passage &p : m_passages)
	{
		auto &e = passed[std::make_pair(p.node, p.road)];
		++e.first;
		e.second += p.wait;
	}

	std::vector<junction_status> out;
	for (const auto &kv : m_net->junctions)
	{
		const net_junction &j = kv.second;
		const bool lit = light_at_node(j.node_id) != nullptr;
		if (j.arms.size() < 3 && !lit)
		{
			continue; // a road connection or a dead end, not a junction
		}
		junction_status js;
		js.node_id = j.node_id;
		js.signalized = lit;
		int passages = 0;
		double wait_sum = 0.0;
		for (const net_arm &arm : j.arms)
		{
			// The incoming lanes of this arm (they end at the junction)
			const std::vector<int> &ls = m_net->lanes_of(arm.road_id, arm.at_a ? -1 : 1);
			if (ls.empty())
			{
				continue;
			}
			approach_status as;
			as.road_id = arm.road_id;
			as.length = 1.0e9;
			wait_cause head_cause = wait_cause::none;
			int blocking = -1;
			double now_wait = 0.0;   // junction waits of the vehicles in the queue (lower limit of their delay)
			for (int lid : ls)
			{
				const net_lane &l = m_net->lanes[lid];
				as.length = std::min(as.length, l.stop_line);
				const std::vector<int> &list = m_on_lane[lid];
				double queue = 0.0;
				int count = 0;
				double prev_rear = l.stop_line;
				for (size_t k = 0; k < list.size(); ++k)
				{
					const sim_vehicle &v = m_vehicles[list[k]];
					if (v.v >= queue_speed || prev_rear - v.s > (k == 0 ? head_range : queue_gap))
					{
						break;
					}
					if (k == 0)
					{
						// The cause of the queue: the cause of its first vehicle
						const wait_cause c = v.cause != wait_cause::none ? v.cause : v.entry_cause;
						if (count == 0 && (head_cause == wait_cause::none || l.stop_line - v.s < 6.0))
						{
							head_cause = c;
							if (c == wait_cause::exit_full && v.next_conn >= 0)
							{
								blocking = m_net->lanes[m_net->connectors[v.next_conn].to_lane].to_node;
							}
						}
					}
					++count;
					now_wait += v.junction_wait;
					prev_rear = v.s - v.p->length;
					queue = l.stop_line - prev_rear;
				}
				if (queue > as.queue_m)
				{
					as.queue_m = queue;
				}
				as.queue_veh += count;
			}
			if (as.length > 1.0e8)
			{
				as.length = 0.0;
			}
			as.spillback = as.length > 1.0 && as.queue_m >= 0.9 * as.length;
			// Queue samples: the longest queue in the time window
			std::deque<std::pair<double, double>> &qs = m_queue_samples[std::make_pair(j.node_id, arm.road_id)];
			qs.push_back(std::make_pair(m_time, as.queue_m));
			while (!qs.empty() && qs.front().first < since)
			{
				qs.pop_front();
			}
			for (const auto &q : qs)
			{
				as.max_queue_m = std::max(as.max_queue_m, q.second);
			}
			// Waiting vehicle seconds of this second, with the cause of the queue
			if (as.queue_veh > 0)
			{
				m_wait_samples.push_back({ m_time, j.node_id, arm.road_id, head_cause, as.queue_veh * sample_dt, blocking });
			}
			const auto pit = passed.find(std::make_pair(j.node_id, arm.road_id));
			const int n = pit != passed.end() ? pit->second.first : 0;
			const double w = pit != passed.end() ? pit->second.second : 0.0;
			as.passages = n;
			as.delay = (n + as.queue_veh) > 0 ? (w + now_wait) / (n + as.queue_veh) : 0.0;
			passages += n;
			wait_sum += w + now_wait;
			js.waiting += as.queue_veh;
			js.queue_m = std::max(js.queue_m, as.queue_m);
			js.spillback = js.spillback || as.spillback;
			js.approaches.push_back(as);
		}
		js.delay = (passages + js.waiting) > 0 ? wait_sum / (passages + js.waiting) : 0.0;
		js.flow = passages * 3600.0 / std::max(1.0, std::min(m_cong_window, m_time));
		js.los = los_letter(js.delay, lit);
		if (js.spillback)
		{
			js.los = 'F';
		}
		out.push_back(js);
	}

	// Main causes in the time window: the largest part of the waiting vehicle seconds
	std::map<int, std::map<int, double>> cause_time;           // node -> cause -> veh s
	std::map<std::pair<int, int>, std::map<int, double>> approach_cause;
	std::map<int, std::map<int, double>> blocking_time;        // node -> blocking node -> veh s
	for (const wait_sample &w : m_wait_samples)
	{
		cause_time[w.node][static_cast<int>(w.cause)] += w.veh_s;
		approach_cause[std::make_pair(w.node, w.road)][static_cast<int>(w.cause)] += w.veh_s;
		if (w.blocking_node >= 0)
		{
			blocking_time[w.node][w.blocking_node] += w.veh_s;
		}
	}
	auto largest = [](const std::map<int, double> &m, double *share)
	{
		int best = 0;
		double best_v = 0.0;
		double total = 0.0;
		for (const auto &kv : m)
		{
			total += kv.second;
			if (kv.second > best_v)
			{
				best_v = kv.second;
				best = kv.first;
			}
		}
		if (share)
		{
			*share = total > 0.0 ? best_v / total : 0.0;
		}
		return best;
	};
	for (junction_status &js : out)
	{
		const auto ct = cause_time.find(js.node_id);
		if (ct != cause_time.end())
		{
			js.cause = static_cast<wait_cause>(largest(ct->second, &js.cause_share));
		}
		const auto bt = blocking_time.find(js.node_id);
		if (js.cause == wait_cause::exit_full && bt != blocking_time.end())
		{
			js.blocking_node = largest(bt->second, nullptr);
		}
		for (approach_status &as : js.approaches)
		{
			const auto ac = approach_cause.find(std::make_pair(js.node_id, as.road_id));
			if (ac != approach_cause.end())
			{
				as.cause = static_cast<wait_cause>(largest(ac->second, nullptr));
			}
		}
		// The worst first: level of service, then delay, then queue
		js.score = los_rank(js.los) * 1000.0 + std::min(999.0, js.delay) + std::min(0.99, js.queue_m / 1000.0);
	}
	std::sort(out.begin(), out.end(), [](const junction_status &a, const junction_status &b) { return a.score > b.score; });
	m_congestion.swap(out);
	// Old queue samples of junctions that do not exist anymore
	for (auto it = m_queue_samples.begin(); it != m_queue_samples.end();)
	{
		it = (!it->second.empty() && it->second.back().first < since) ? m_queue_samples.erase(it) : std::next(it);
	}
}
