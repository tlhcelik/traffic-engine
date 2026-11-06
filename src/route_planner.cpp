#include "route_planner.h"
#include "road_network.h"
#include "world_model.h"

#include <algorithm>
#include <limits>
#include <map>
#include <set>

#include <boost/graph/adjacency_list.hpp>
#include <boost/graph/dijkstra_shortest_paths.hpp>

namespace
{
	typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::directedS,
		boost::no_property, boost::property<boost::edge_weight_t, double>> dgraph;
	typedef boost::adjacency_list<boost::vecS, boost::vecS, boost::undirectedS,
		boost::no_property, boost::property<boost::edge_weight_t, double>> ugraph;

	struct tree
	{
		std::vector<std::size_t> pred;
		std::vector<double> dist;
	};

	template <typename G>
	tree run_dijkstra(const G &g, std::size_t source)
	{
		tree t;
		const std::size_t n = boost::num_vertices(g);
		t.pred.resize(n);
		t.dist.assign(n, std::numeric_limits<double>::max());
		boost::dijkstra_shortest_paths(g, source,
			boost::predecessor_map(boost::make_iterator_property_map(t.pred.begin(), boost::get(boost::vertex_index, g)))
			.distance_map(boost::make_iterator_property_map(t.dist.begin(), boost::get(boost::vertex_index, g))));
		return t;
	}
}

struct route_planner::impl
{
	// vehicles
	dgraph g;
	std::map<int, std::size_t> key_to_v;
	std::vector<int> v_key;
	std::vector<int> v_end_node;
	std::map<int, std::vector<int>> keys_from_node;
	mutable std::map<std::size_t, tree> cache;

	// pedestrians
	ugraph pg;
	std::map<int, std::size_t> node_to_pv;
	std::vector<int> pv_node;
	std::map<std::pair<int, int>, std::pair<int, double>> road_between;   // (node, node) -> (road, length)
	std::map<int, int> road_node_a;
	mutable std::map<std::size_t, tree> pcache;
};

route_planner::route_planner()
	: d(new impl)
{
}

route_planner::~route_planner() = default;

void route_planner::build(const world_model &model, const road_network &net)
{
	d.reset(new impl);

	// --- vehicle graph ---
	for (const auto &kv : net.roads)
	{
		const road_data *rd = model.road(kv.first);
		for (int dir : { 1, -1 })
		{
			const std::vector<int> &ls = net.lanes_of(kv.first, dir);
			if (ls.empty() || net.lanes[ls.front()].closed)
			{
				continue;
			}
			const int key = road_network::dir_key(kv.first, dir);
			const std::size_t v = d->v_key.size();
			d->key_to_v[key] = v;
			d->v_key.push_back(key);
			d->v_end_node.push_back(dir > 0 ? rd->node_b : rd->node_a);
			d->keys_from_node[dir > 0 ? rd->node_a : rd->node_b].push_back(key);
		}
	}
	d->g = dgraph(d->v_key.size());
	std::set<std::pair<std::size_t, std::size_t>> seen;
	for (const net_connector &c : net.connectors)
	{
		if (!c.allowed)
		{
			continue;
		}
		const net_lane &from = net.lanes[c.from_lane];
		const net_lane &to = net.lanes[c.to_lane];
		const auto fi = d->key_to_v.find(road_network::dir_key(from.road_id, from.dir));
		const auto ti = d->key_to_v.find(road_network::dir_key(to.road_id, to.dir));
		if (fi == d->key_to_v.end() || ti == d->key_to_v.end())
		{
			continue;
		}
		if (!seen.insert(std::make_pair(fi->second, ti->second)).second)
		{
			continue;
		}
		double penalty = 0.0;
		if (c.turn == turn_kind::right)
		{
			penalty = 2.0;
		}
		else if (c.turn == turn_kind::left)
		{
			penalty = 5.0;
		}
		const double w = to.length() / std::max(1.0, to.base_limit) + penalty;
		boost::add_edge(fi->second, ti->second, w, d->g);
	}

	// --- pedestrian graph ---
	for (const auto &kv : model.nodes)
	{
		const std::size_t v = d->pv_node.size();
		d->node_to_pv[kv.first] = v;
		d->pv_node.push_back(kv.first);
	}
	d->pg = ugraph(d->pv_node.size());
	for (const auto &kv : net.roads)
	{
		const road_data *rd = model.road(kv.first);
		if (!kv.second.sidewalk_left && !kv.second.sidewalk_right)
		{
			continue;
		}
		const double len = kv.second.center.length();
		const auto key = std::make_pair(std::min(rd->node_a, rd->node_b), std::max(rd->node_a, rd->node_b));
		const auto it = d->road_between.find(key);
		if (it != d->road_between.end() && it->second.second <= len)
		{
			continue;
		}
		d->road_between[key] = std::make_pair(kv.first, len);
		d->road_node_a[kv.first] = rd->node_a;
		boost::add_edge(d->node_to_pv[rd->node_a], d->node_to_pv[rd->node_b], len, d->pg);
	}
}

std::vector<int> route_planner::vehicle_route(int start_key, int dest_node) const
{
	std::vector<int> out;
	const auto si = d->key_to_v.find(start_key);
	if (si == d->key_to_v.end())
	{
		return out;
	}
	const std::size_t s = si->second;
	if (d->v_end_node[s] == dest_node)
	{
		out.push_back(start_key);
		return out;
	}
	auto ci = d->cache.find(s);
	if (ci == d->cache.end())
	{
		ci = d->cache.emplace(s, run_dijkstra(d->g, s)).first;
	}
	const tree &t = ci->second;
	std::size_t best = s;
	double best_d = std::numeric_limits<double>::max();
	for (std::size_t v = 0; v < d->v_key.size(); ++v)
	{
		if (d->v_end_node[v] == dest_node && t.dist[v] < best_d)
		{
			best_d = t.dist[v];
			best = v;
		}
	}
	if (best == s || best_d == std::numeric_limits<double>::max())
	{
		return out;
	}
	for (std::size_t v = best; ; v = t.pred[v])
	{
		out.push_back(d->v_key[v]);
		if (v == s || t.pred[v] == v)
		{
			break;
		}
	}
	if (d->v_key[s] != out.back())
	{
		return std::vector<int>();
	}
	std::reverse(out.begin(), out.end());
	return out;
}

std::vector<std::pair<int, int>> route_planner::pedestrian_route(int from_node, int to_node) const
{
	std::vector<std::pair<int, int>> out;
	const auto fi = d->node_to_pv.find(from_node);
	const auto ti = d->node_to_pv.find(to_node);
	if (fi == d->node_to_pv.end() || ti == d->node_to_pv.end() || from_node == to_node)
	{
		return out;
	}
	auto ci = d->pcache.find(fi->second);
	if (ci == d->pcache.end())
	{
		ci = d->pcache.emplace(fi->second, run_dijkstra(d->pg, fi->second)).first;
	}
	const tree &t = ci->second;
	if (t.dist[ti->second] == std::numeric_limits<double>::max())
	{
		return out;
	}
	std::vector<int> nodes;
	for (std::size_t v = ti->second; ; v = t.pred[v])
	{
		nodes.push_back(d->pv_node[v]);
		if (v == fi->second || t.pred[v] == v)
		{
			break;
		}
	}
	if (nodes.back() != from_node)
	{
		return out;
	}
	std::reverse(nodes.begin(), nodes.end());
	for (size_t i = 0; i + 1 < nodes.size(); ++i)
	{
		const auto key = std::make_pair(std::min(nodes[i], nodes[i + 1]), std::max(nodes[i], nodes[i + 1]));
		const auto it = d->road_between.find(key);
		if (it == d->road_between.end())
		{
			return std::vector<std::pair<int, int>>();
		}
		const int rid = it->second.first;
		out.push_back(std::make_pair(rid, d->road_node_a[rid] == nodes[i] ? 1 : -1));
	}
	return out;
}

std::vector<int> route_planner::keys_from_node(int node_id) const
{
	const auto it = d->keys_from_node.find(node_id);
	return it == d->keys_from_node.end() ? std::vector<int>() : it->second;
}

int route_planner::end_node_of_key(int key) const
{
	const auto it = d->key_to_v.find(key);
	return it == d->key_to_v.end() ? -1 : d->v_end_node[it->second];
}
