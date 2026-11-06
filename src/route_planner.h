#ifndef ROUTE_PLANNER_H
#define ROUTE_PLANNER_H

#include <memory>
#include <utility>
#include <vector>

class world_model;
class road_network;

// route_planner
// -------------
// Shortest routes on the road network (Boost.Graph, Dijkstra).
//
// Vehicles: the graph vertices are the directed roads (key = road_id * 2 +
// (dir > 0 ? 0 : 1)). An edge is an allowed turn movement (connector) from
// one directed road to the next. Thus, turn bans (signs), one-way roads and
// closed roads are correct in all routes. The edge weight is the travel time
// of the next road at its speed limit, plus a small time for the turn.
// The shortest-path trees are kept for each start road (cache).
//
// Pedestrians: an undirected graph of the nodes. An edge is a road with a
// sidewalk on one side or on both sides.

class route_planner
{
public:
	route_planner();
	~route_planner();

	void build(const world_model &model, const road_network &net);

	// Directed road keys from start_key to a road that ends at dest_node.
	// Empty: there is no route.
	std::vector<int> vehicle_route(int start_key, int dest_node) const;

	// (road id, walking direction) from node a to node b. dir +1: node_a -> node_b.
	std::vector<std::pair<int, int>> pedestrian_route(int from_node, int to_node) const;

	// Directed roads that leave the node and are not closed.
	std::vector<int> keys_from_node(int node_id) const;
	int end_node_of_key(int key) const;

private:
	struct impl;
	std::unique_ptr<impl> d;
};

#endif // ROUTE_PLANNER_H
