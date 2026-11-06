// Checks of the simulation (debug mode and --sim-test): overlapping vehicles
// and vehicles that do not move for a long time (wait chains, deadlocks).

#include "simulation.h"
#include "road_network.h"
#include "world_model.h"
#include "logger.h"

#include <cmath>
#include <set>
#include <unordered_map>

namespace
{
	const double overlap_repeat = 30.0;     // s: the same pair again after this time
	const double stuck_repeat = 300.0;      // s: the same vehicle again after this time
	const double cell_size = 14.0;          // m: grid of the overlap check (more than a bus)

	struct box
	{
		vec2 c;
		vec2 u;        // length axis
		vec2 w;        // width axis
		double hl;
		double hw;
	};

	// Separating axis test of two rectangles. Returns the smallest
	// penetration depth (> 0: the rectangles intersect).
	double penetration(const box &a, const box &b)
	{
		const vec2 axes[4] = { a.u, a.w, b.u, b.w };
		const vec2 d = b.c - a.c;
		double depth = 1.0e9;
		for (const vec2 &ax : axes)
		{
			const double ra = a.hl * std::abs(dot(a.u, ax)) + a.hw * std::abs(dot(a.w, ax));
			const double rb = b.hl * std::abs(dot(b.u, ax)) + b.hw * std::abs(dot(b.w, ax));
			const double dist = std::abs(dot(d, ax));
			depth = std::min(depth, ra + rb - dist);
			if (depth <= 0.0)
			{
				return depth;
			}
		}
		return depth;
	}

	qint64 cell_key(int x, int y)
	{
		return (static_cast<qint64>(x) << 32) ^ static_cast<qint64>(static_cast<quint32>(y));
	}
}

void simulation::set_checks(bool on, double stuck_limit)
{
	m_checks = on;
	m_stuck_limit = std::max(10.0, stuck_limit);
	m_overlap_seen.clear();
	m_stuck_seen.clear();
}

std::vector<sim_issue> simulation::take_issues()
{
	std::vector<sim_issue> out;
	out.swap(m_issues);
	return out;
}

void simulation::run_checks(double dt)
{
	check_overlaps();
	m_check_accum += dt;
	if (m_check_accum >= 1.0)
	{
		m_check_accum = 0.0;
		check_stuck();
		for (auto it = m_overlap_seen.begin(); it != m_overlap_seen.end();)
		{
			it = (m_time - it->second > overlap_repeat) ? m_overlap_seen.erase(it) : std::next(it);
		}
	}
}

QString simulation::wait_chain(int vehicle_id, bool *cycle, wait_cause *root) const
{
	if (cycle)
	{
		*cycle = false;
	}
	QString out;
	std::set<int> seen;
	int cur = vehicle_id;
	for (int hop = 0; hop < 40; ++hop)
	{
		const sim_vehicle *v = find_vehicle(cur);
		if (!v)
		{
			out += QStringLiteral("%1 (not in the simulation)").arg(cur);
			break;
		}
		if (root)
		{
			*root = v->cause;
		}
		seen.insert(cur);
		out += QStringLiteral("%1 (%2, %3 s)").arg(v->id).arg(QString::fromLatin1(wait_cause_name(v->cause))).arg(v->standstill, 0, 'f', 0);
		if (m_net)
		{
			// Position: [lane id/length/s] or [connector id at node, held reservations]
			out += v->on_connector
				? QStringLiteral(" [junction %1 path %2, held %3/%4]").arg(m_net->connectors[v->seg].node_id).arg(v->seg).arg(v->held_conn).arg(v->held_prev)
				: QStringLiteral(" [lane %1 len %2 s %3, held %4/%5]").arg(v->seg).arg(m_net->lanes[v->seg].length(), 0, 'f', 0)
					.arg(v->s, 0, 'f', 0).arg(v->held_conn).arg(v->held_prev);
		}
		const cause_target t = target_of(v->cause);
		if (t != cause_target::vehicle || v->cause_id < 0)
		{
			switch (t)
			{
				case cause_target::light: out += QStringLiteral(" -> light %1").arg(v->cause_id); break;
				case cause_target::sign: out += QStringLiteral(" -> sign %1").arg(v->cause_id); break;
				case cause_target::pedestrian: out += QStringLiteral(" -> pedestrian %1").arg(v->cause_id); break;
				case cause_target::object: out += QStringLiteral(" -> object %1").arg(v->cause_id); break;
				default: break;
			}
			break;
		}
		if (seen.count(v->cause_id))
		{
			out += QStringLiteral(" -> %1: closed chain (deadlock)").arg(v->cause_id);
			if (cycle)
			{
				*cycle = true;
			}
			break;
		}
		out += QStringLiteral(" -> ");
		cur = v->cause_id;
	}
	return out;
}

int simulation::level_of(const sim_vehicle &v) const
{
	const int road = v.on_connector ? m_net->connectors[v.seg].from_road : m_net->lanes[v.seg].road_id;
	const road_data *rd = m_model ? m_model->road(road) : nullptr;
	return rd ? rd->level : 0;
}

void simulation::check_overlaps()
{
	if (!m_net || m_vehicles.size() < 2)
	{
		return;
	}
	std::vector<box> boxes(m_vehicles.size());
	std::unordered_map<qint64, std::vector<int>> grid;
	grid.reserve(m_vehicles.size() * 2);
	for (size_t i = 0; i < m_vehicles.size(); ++i)
	{
		const sim_vehicle &v = m_vehicles[i];
		box &b = boxes[i];
		b.c = v.pos;
		b.u = v.dir;
		b.w = v.dir.left();
		// Small tolerances: the drawing of a turn is not exact to some centimeters.
		b.hl = std::max(0.2, v.p->length * 0.5 - 0.25);
		b.hw = std::max(0.2, v.p->width * 0.5 - 0.15);
		if (v.finished)
		{
			continue;
		}
		grid[cell_key(static_cast<int>(std::floor(v.pos.x / cell_size)), static_cast<int>(std::floor(v.pos.y / cell_size)))].push_back(static_cast<int>(i));
	}
	for (const auto &kv : grid)
	{
		const int cx = static_cast<int>(kv.first >> 32);
		const int cy = static_cast<int>(static_cast<qint32>(kv.first & 0xFFFFFFFF));
		for (int i : kv.second)
		{
			for (int dx = -1; dx <= 1; ++dx)
			{
				for (int dy = -1; dy <= 1; ++dy)
				{
					const auto it = grid.find(cell_key(cx + dx, cy + dy));
					if (it == grid.end())
					{
						continue;
					}
					for (int j : it->second)
					{
						if (j <= i)
						{
							continue;
						}
						const double depth = penetration(boxes[i], boxes[j]);
						if (depth <= 0.0)
						{
							continue;
						}
						const sim_vehicle &a = m_vehicles[i];
						const sim_vehicle &b = m_vehicles[j];
						if (level_of(a) != level_of(b))
						{
							continue; // a bridge over a road or a tunnel under it
						}
						const std::pair<int, int> key(std::min(a.id, b.id), std::max(a.id, b.id));
						const auto seen = m_overlap_seen.find(key);
						if (seen != m_overlap_seen.end())
						{
							seen->second = m_time;
							continue;
						}
						m_overlap_seen[key] = m_time;
						auto describe = [this](const sim_vehicle &v)
						{
							const QString where = v.on_connector
								? QStringLiteral("connector %1 (node %2, lane %3 -> %4)").arg(v.seg).arg(m_net->connectors[v.seg].node_id)
									.arg(m_net->connectors[v.seg].from_lane).arg(m_net->connectors[v.seg].to_lane)
								: QStringLiteral("lane %1 (road %2, length %3, stop line %4, to node %5)").arg(v.seg).arg(m_net->lanes[v.seg].road_id)
									.arg(m_net->lanes[v.seg].length(), 0, 'f', 1).arg(m_net->lanes[v.seg].stop_line, 0, 'f', 1).arg(m_net->lanes[v.seg].to_node);
							return QStringLiteral("vehicle %1 (%2) on %3 s %4 v %5 a %6, next %7, committed %8, held %9, lane change %10, prev %11%12")
								.arg(v.id).arg(v.p->name).arg(where).arg(v.s, 0, 'f', 2).arg(v.v, 0, 'f', 2).arg(v.a, 0, 'f', 2)
								.arg(v.next_conn).arg(v.committed ? QStringLiteral("yes") : QStringLiteral("no")).arg(v.held_conn)
								.arg(v.lc_progress < 1.0 ? QStringLiteral("%1 from lane %2").arg(v.lc_progress, 0, 'f', 2).arg(v.lc_from) : QStringLiteral("no"))
								.arg(v.prev_on_connector ? QStringLiteral("connector ") : QStringLiteral("lane ")).arg(v.prev_seg);
						};
						sim_issue is;
						is.kind = sim_issue::overlap;
						is.time = m_time;
						is.vehicle_a = a.id;
						is.vehicle_b = b.id;
						is.pos = (a.pos + b.pos) * 0.5;
						// Two roads without a common node: a bridge or a tunnel (other level)?
						QString roads;
						if (!a.on_connector && !b.on_connector && m_model)
						{
							const road_data *ra = m_model->road(m_net->lanes[a.seg].road_id);
							const road_data *rb = m_model->road(m_net->lanes[b.seg].road_id);
							if (ra && rb && ra->id != rb->id)
							{
								const bool common = ra->node_a == rb->node_a || ra->node_a == rb->node_b
									|| ra->node_b == rb->node_a || ra->node_b == rb->node_b;
								roads = common ? QStringLiteral(" [roads with a common node]") : QStringLiteral(" [roads without a common node]");
							}
						}
						// The movements (connectors) of the two vehicles and their relation:
						// an overlap without a conflict shows a missing rule.
						auto movements = [](const sim_vehicle &v)
						{
							std::vector<int> c;
							auto add = [&c](int id)
							{
								if (id >= 0 && std::find(c.begin(), c.end(), id) == c.end())
								{
									c.push_back(id);
								}
							};
							if (v.on_connector)
							{
								add(v.seg);
							}
							if (v.prev_on_connector)
							{
								add(v.prev_seg);
							}
							add(v.held_conn);
							add(v.held_prev);
							add(v.held_next);
							return c;
						};
						QStringList relations;
						for (int ca : movements(a))
						{
							for (int cb : movements(b))
							{
								const net_connector &x = m_net->connectors[ca];
								const net_connector &y = m_net->connectors[cb];
								QString r;
								if (ca == cb)
								{
									r = QStringLiteral("same");
								}
								else if (x.node_id != y.node_id)
								{
									r = QStringLiteral("other nodes %1/%2").arg(x.node_id).arg(y.node_id);
								}
								else if (std::find(x.conflicts.begin(), x.conflicts.end(), cb) != x.conflicts.end())
								{
									r = QStringLiteral("conflict");
								}
								else if (std::find(x.conflicts_long.begin(), x.conflicts_long.end(), cb) != x.conflicts_long.end())
								{
									r = QStringLiteral("long conflict");
								}
								else
								{
									r = x.from_lane == y.from_lane ? QStringLiteral("same lane") : QStringLiteral("no conflict");
								}
								relations << QStringLiteral("%1-%2 %3 (reserved %4/%5, long %6/%7)").arg(ca).arg(cb).arg(r)
									.arg(m_reservations[ca]).arg(m_reservations[cb]).arg(m_res_long[ca]).arg(m_res_long[cb]);
							}
						}
						is.text = QStringLiteral("Overlap %1 m at %2, %3%4: %5 | %6. Movements: %7")
							.arg(depth, 0, 'f', 2).arg(is.pos.x, 0, 'f', 1).arg(is.pos.y, 0, 'f', 1).arg(roads, describe(a), describe(b))
							.arg(relations.isEmpty() ? QStringLiteral("none") : relations.join(QStringLiteral("; ")));
						++m_issue_counts[sim_issue::overlap];
						TE_DEBUG("CHECK", is.text);
						m_issues.push_back(is);
					}
				}
			}
		}
	}
}

void simulation::check_stuck()
{
	// The reservation counts must agree with the vehicles that hold them (a
	// leaked reservation blocks a junction for ever).
	std::vector<int> expected(m_reservations.size(), 0);
	for (const sim_vehicle &v : m_vehicles)
	{
		if (v.finished)
		{
			continue;
		}
		for (int c : { v.held_conn, v.held_prev, v.held_next })
		{
			if (c >= 0 && c < static_cast<int>(expected.size()))
			{
				++expected[c];
			}
		}
	}
	for (size_t c = 0; c < expected.size(); ++c)
	{
		if (expected[c] == m_reservations[c])
		{
			continue;
		}
		const std::pair<int, int> key(-1, static_cast<int>(c));
		const auto seen = m_overlap_seen.find(key);
		if (seen != m_overlap_seen.end())
		{
			seen->second = m_time;
			continue;
		}
		m_overlap_seen[key] = m_time;
		sim_issue is;
		is.kind = sim_issue::leak;
		is.time = m_time;
		is.text = QStringLiteral("Reservation leak: connector %1 (node %2) has %3 reservations, the vehicles hold %4.")
			.arg(c).arg(m_net->connectors[c].node_id).arg(m_reservations[c]).arg(expected[c]);
		++m_issue_counts[sim_issue::leak];
		TE_DEBUG("CHECK", is.text);
		m_issues.push_back(is);
	}

	for (const sim_vehicle &v : m_vehicles)
	{
		if (v.finished)
		{
			continue;
		}
		if (v.standstill < m_stuck_limit)
		{
			m_stuck_seen.erase(v.id);
			continue;
		}
		const auto seen = m_stuck_seen.find(v.id);
		if (seen != m_stuck_seen.end() && v.standstill < seen->second + stuck_repeat)
		{
			continue;
		}
		bool cycle = false;
		wait_cause root = wait_cause::none;
		const QString chain = wait_chain(v.id, &cycle, &root);
		// A queue behind a red light: a long cycle is not an error (3 x the limit).
		if (!cycle && root == wait_cause::red_light && v.standstill < 3.0 * m_stuck_limit)
		{
			continue;
		}
		m_stuck_seen[v.id] = v.standstill;
		QString where;
		if (v.on_connector)
		{
			const net_connector &c = m_net->connectors[v.seg];
			where = QStringLiteral("connector %1 at node %2").arg(v.seg).arg(c.node_id);
		}
		else
		{
			const net_lane &l = m_net->lanes[v.seg];
			where = QStringLiteral("lane %1 (road %2, to node %3, %4 m before the stop line)").arg(v.seg).arg(l.road_id).arg(l.to_node)
				.arg(l.stop_line - v.s, 0, 'f', 1);
		}
		sim_issue is;
		is.kind = cycle ? sim_issue::deadlock : sim_issue::stuck;
		is.time = m_time;
		is.vehicle_a = v.id;
		is.pos = v.pos;
		is.text = QStringLiteral("%1: vehicle %2 (%3) does not move for %4 s on %5. Wait chain: %6")
			.arg(cycle ? QStringLiteral("Deadlock") : QStringLiteral("Stuck")).arg(v.id).arg(v.p->name)
			.arg(v.standstill, 0, 'f', 0).arg(where, chain);
		++m_issue_counts[is.kind];
		TE_DEBUG("CHECK", is.text);
		m_issues.push_back(is);
	}
}
