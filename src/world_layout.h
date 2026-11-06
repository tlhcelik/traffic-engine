#ifndef WORLD_LAYOUT_H
#define WORLD_LAYOUT_H

#include "geom.h"
#include "world_model.h"

#include <vector>

class workspace;
class road_network;

// world_layout
// ------------
// Positions of the visible elements in the world: signal heads, signs and
// objects. The renderer, the picking and the popups use the same functions.

namespace world_layout
{
	struct signal_head
	{
		int light_id = -1;
		int node_id = -1;
		int road_id = -1;      // incoming road (approach)
		vec2 pos;
		vec2 dir;              // travel direction of the approach
	};

	std::vector<signal_head> signal_heads(const world_model &m, const road_network &net);

	// Position of a sign at the road side (right side of its traffic).
	bool sign_pose(const world_model &m, const road_network &net, const sign_data &s, vec2 *pos, vec2 *dir);

	// Position and direction of an object. *half_length: half of its length on the road.
	bool object_pose(const world_model &m, const road_network &net, const object_data &o, vec2 *pos, vec2 *dir, double *half_length);

	// The element at the point (tolerance in meters). include_dynamic: vehicles and pedestrians too.
	element_ref pick(const workspace &ws, const vec2 &p, double tolerance, bool include_dynamic, bool prefer_nodes);

	// The node near the point, or -1.
	int node_at(const world_model &m, const vec2 &p, double tolerance);
}

#endif // WORLD_LAYOUT_H
