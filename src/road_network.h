#ifndef ROAD_NETWORK_H
#define ROAD_NETWORK_H

#include "geom.h"
#include "world_model.h"

#include <map>
#include <memory>
#include <utility>
#include <vector>

// road_network
// ------------
// The drivable network that is made from the world model (build()).
// The world model is the editable data; the road network is the result:
//
//  net_road      center line, junction trims, cross section (lanes, bike
//                lane, sidewalk, ditch) of one road
//  net_lane      one lane in one direction. Right-hand traffic: lane index 0
//                is the right lane (curb). The path is in the travel direction.
//  net_junction  a node with two or more roads: arms (counterclockwise),
//                radius, surface polygon, corners for the pedestrians
//  net_connector the path of one turn movement in a junction (incoming lane
//                -> outgoing lane) and the list of conflicting connectors
//  net_crosswalk a pedestrian crossing on one arm of a junction
//
// The signs and objects change the lanes: speed limit zones, stop and yield
// signs, no entry, turn bans, no overtaking, blocked lanes (cones, barriers),
// closed roads, level crossings and bus stops.
//
// Picking uses a Boost R-tree of the road segments.

enum class turn_kind
{
	right = 0,
	straight,
	left,
	uturn
};

struct net_zone
{
	double s;       // from this distance on the lane
	double value;   // speed limit (m/s)
};

struct net_block
{
	double s0;
	double s1;
	int object_id;
};

struct net_stop_point
{
	double s;
	int object_id;
};

struct net_lane
{
	int id = -1;
	int road_id = -1;
	int dir = 1;                // +1: a -> b, -1: b -> a
	int index = 0;              // 0 = right lane (curb)
	int lanes_in_dir = 1;
	int from_node = -1;
	int to_node = -1;
	polyline path;
	double width = 3.5;
	double base_limit = 13.9;   // m/s
	std::vector<net_zone> limits;
	std::vector<std::pair<double, double>> no_overtaking;
	std::vector<net_block> blocks;
	std::vector<net_stop_point> bus_stops;
	std::vector<net_stop_point> level_crossings;
	double stop_line = 0.0;     // vehicles stop here before the junction
	double clear_end = 0.0;     // m at the end that still touch another arm (see net_arm::clear)
	int end_crosswalk = -1;
	int start_crosswalk = -1;
	double start_cw_s0 = 0.0;
	double start_cw_s1 = 0.0;
	int left = -1;              // neighbor lanes in the same direction
	int right = -1;
	std::vector<int> out;       // connectors at the end of the lane
	int rank = 1;               // 0: stop or yield sign, 1: normal, 2: priority road
	bool stop_sign = false;
	bool yield_sign = false;
	int sign_id = -1;           // the stop or yield sign
	bool closed = false;        // no entry, road closed
	bool dead_end = false;      // the lane ends at a node with no other road

	double length() const { return path.length(); }
	double limit_at(double s) const;
	bool overtaking_at(double s) const;
	bool blocked_at(double s0, double s1) const;
};

struct net_connector
{
	int id = -1;
	int node_id = -1;
	int from_lane = -1;
	int to_lane = -1;
	int from_road = -1;
	int to_road = -1;
	turn_kind turn = turn_kind::straight;
	polyline path;
	double max_speed = 20.0;    // m/s (curve radius)
	bool allowed = true;        // false: a sign forbids this turn
	std::vector<int> conflicts;
	// More conflicts when a long vehicle (bus) uses one of the two connectors:
	// the body of a long vehicle cuts the curve.
	std::vector<int> conflicts_long;
};

struct net_crosswalk
{
	int id = -1;
	int node_id = -1;
	int road_id = -1;
	int arm = -1;
	vec2 a;                     // end on the clockwise side of the arm
	vec2 b;                     // end on the counterclockwise side of the arm
	std::vector<vec2> polygon;
	double length = 0.0;
	std::vector<int> lanes_in;  // lanes to the junction: the crosswalk is at their end
	std::vector<int> lanes_out; // lanes from the junction: the crosswalk is at their start
};

struct net_arm
{
	int road_id = -1;
	bool at_a = true;           // the road starts at this junction
	double angle = 0.0;         // direction from the junction (radians)
	vec2 dir;
	int crosswalk = -1;
	double trim = 0.0;          // m of the road in the junction area
	double clear = 0.0;         // m after the trim that still touch another arm (short
	                            // road): the stop line is before this part
};

struct net_junction
{
	int node_id = -1;
	vec2 pos;
	double radius = 0.0;
	std::vector<vec2> polygon;
	std::vector<net_arm> arms;      // counterclockwise
	std::vector<vec2> corners;      // corner k: between arm k and arm k+1
	std::vector<int> connectors;
	int light_id = -1;
};

struct net_road
{
	int road_id = -1;
	polyline center;            // full center line a -> b
	double trim_a = 0.0;
	double trim_b = 0.0;
	polyline trimmed;           // center line without the junction areas
	// Lateral positions from the center line (left of a -> b is positive)
	double edge_left = 0.0;     // carriageway
	double edge_right = 0.0;
	double outer_left = 0.0;    // with bike lane, sidewalk and ditch
	double outer_right = 0.0;
	double median_width = 0.0;
	double bike_width = 0.0;
	double sidewalk_width = 2.0;
	double ditch_width = 0.0;
	bool sidewalk_left = false;
	bool sidewalk_right = false;
	std::vector<int> lanes_fwd; // lane ids, index order (0 = curb)
	std::vector<int> lanes_bwd;
	polyline sidewalk_left_line;  // center of the sidewalk, direction a -> b
	polyline sidewalk_right_line;
	// Bridge (level > 0) or tunnel (level < 0): the raised or covered part of
	// the trimmed center line is level_s0 to level_s1. Before and after it are
	// ramps at the ground level (an end at a junction with roads of another
	// level). The part contains each crossing with a road of another level.
	int level = 0;
	double level_s0 = 0.0;
	double level_s1 = 0.0;
};

class road_network
{
public:
	road_network();
	~road_network();
	road_network(const road_network &) = delete;
	road_network &operator=(const road_network &) = delete;

	void build(const world_model &model);
	quint64 built_revision() const { return m_built_revision; }
	// Speed limits, overtaking and sign effects from the model, without a
	// new geometry (the vehicles keep their positions).
	void refresh_attributes(const world_model &model);

	std::vector<net_lane> lanes;
	std::vector<net_connector> connectors;
	std::vector<net_crosswalk> crosswalks;
	std::map<int, net_road> roads;
	std::map<int, net_junction> junctions;
	double total_lane_km = 0.0;

	const net_road *road(int id) const;
	const net_junction *junction(int node_id) const;
	// Lanes of a road in one direction (index order, 0 = curb).
	const std::vector<int> &lanes_of(int road_id, int dir) const;
	static int dir_key(int road_id, int dir) { return road_id * 2 + (dir > 0 ? 0 : 1); }

	// Distance on the center line (from node_a) <-> distance on a lane.
	double lane_s_from_road_s(const net_lane &lane, double road_s) const;
	double road_s_from_lane_s(const net_lane &lane, double lane_s) const;

	// The road at the point. *s: distance on the center line from node_a.
	// *lateral: signed distance from the center line (left of a -> b is positive).
	int road_at(const vec2 &p, double tolerance, double *s = nullptr, double *lateral = nullptr) const;
	// The lane at a lateral position of a road. *dir: direction of the lane.
	int lane_from_lateral(int road_id, double lateral) const;

private:
	void build_roads(const world_model &m);
	void build_junctions(const world_model &m);
	void build_levels(const world_model &m);
	void build_lanes(const world_model &m);
	void build_connectors();
	void build_conflicts();
	void build_crosswalks();
	void build_turn_clearance();
	void apply_signs(const world_model &m);
	void apply_objects(const world_model &m);
	void build_index();

	struct index_impl;
	std::unique_ptr<index_impl> m_index;
	std::map<int, std::vector<int>> m_lanes_by_dir;
	std::vector<int> m_empty;
	quint64 m_built_revision;
};

QString turn_name(turn_kind t);

#endif // ROAD_NETWORK_H
