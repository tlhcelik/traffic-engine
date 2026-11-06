#ifndef WORLD_MODEL_H
#define WORLD_MODEL_H

#include "geom.h"

#include <QString>
#include <QtGlobal>

#include <map>
#include <vector>

// world_model
// -----------
// The editable data of one workspace: nodes, roads, traffic lights, signs
// and other objects. The .te file stores this data. The road network
// (road_network) and the simulation are made from it.
//
// All ids are unique in the workspace (one counter for all kinds).
// element_ref identifies one element (kind + id). The selection, the
// property panel and the popups use it.
//
// Revisions: each change increments "revision". A change of the road
// geometry (nodes, roads, lanes, signs, objects) also increments
// "network_revision". The workspace makes the road network again when
// network_revision changes.

enum class ground_material
{
	grass = 0,
	soil,
	sand,
	snow,
	concrete,
	asphalt
};

enum class element_kind
{
	none = 0,
	node,
	road,
	light,
	sign,
	object,
	vehicle,
	pedestrian
};

struct element_ref
{
	element_kind kind = element_kind::none;
	int id = -1;

	element_ref() = default;
	element_ref(element_kind k, int i) : kind(k), id(i) {}
	bool valid() const { return kind != element_kind::none && id >= 0; }
	bool operator==(const element_ref &o) const { return kind == o.kind && id == o.id; }
	bool operator!=(const element_ref &o) const { return !(*this == o); }
};

struct node_data
{
	int id = -1;
	vec2 pos;
	// Traffic source and destination. Only a dead-end node (one road) makes
	// traffic. The rates are the values at the peak hour.
	bool spawn = true;
	double vehicles_per_hour = 300.0;
	double pedestrians_per_hour = 60.0;
	double share_bus = 0.05;
	double share_motorcycle = 0.15;   // the rest are sedan cars
	bool daily_profile = true;        // the demand changes with the clock
};

struct road_data
{
	int id = -1;
	QString name;
	int node_a = -1;
	int node_b = -1;
	std::vector<vec2> shape;      // control points between node_a and node_b
	int lanes_forward = 1;        // lanes in the direction a -> b
	int lanes_backward = 1;       // lanes in the direction b -> a (0 = one-way road)
	double lane_width = 3.5;      // m
	double speed_limit = 50.0;    // km/h
	bool overtaking = true;       // lane changes are allowed (dashed lines)
	bool sidewalk_left = true;    // left side of the direction a -> b
	bool sidewalk_right = true;
	bool bike_lane = false;
	bool ditch = false;
	bool median = false;          // a strip between the two directions
	bool guard_rail = false;
	int level = 0;                // -1 tunnel, 0 ground, 1 bridge: vehicles on other levels
	                              // do not touch; a higher level is drawn on top
};

struct light_phase
{
	std::vector<int> green_roads; // incoming roads with green light
	double green = 25.0;          // s
	double yellow = 3.0;          // s
	double all_red = 2.0;         // s
	double min_green = 8.0;       // s (actuated algorithm)
	double max_green = 45.0;      // s (actuated algorithm)
	bool pedestrian_only = false; // all vehicles red; pedestrians cross
};

enum class light_algorithm
{
	fixed_time = 0,   // the phases in sequence with fixed times
	actuated,         // the green time changes with the vehicles that wait
	external          // the phase comes from an external system (API)
};

struct light_data
{
	int id = -1;
	int node_id = -1;
	light_algorithm algorithm = light_algorithm::fixed_time;
	double offset = 0.0;       // s, start offset (coordination of lights)
	double red_yellow = 1.0;   // s, red and yellow together before green
	std::vector<light_phase> phases;
};

struct sign_data
{
	int id = -1;
	QString key;        // catalog key (see sign_catalog)
	int road_id = -1;
	double s = 0.0;     // distance from node_a along the center line
	int dir = 1;        // +1: for the traffic a -> b, -1: for the traffic b -> a
	int value = 0;      // value of the sign (for example the speed limit in km/h)
};

enum class object_kind
{
	cone = 0,
	barrier,
	road_closure,
	level_crossing,
	bus_stop
};

struct object_data
{
	int id = -1;
	object_kind kind = object_kind::cone;
	int road_id = -1;
	double s = 0.0;            // distance from node_a along the center line
	int dir = 1;               // +1 / -1; 0 = both directions
	int lane = 0;              // blocked lane (0 = right lane), -1 = all lanes
	double length = 12.0;      // m, blocked length (cones, barrier)
	double train_period = 300.0; // s, level crossing: time between two trains
	double closed_time = 40.0;   // s, level crossing: barrier down time
	double train_offset = 0.0;   // s
	double dwell_time = 20.0;    // s, bus stop: stop time of a bus
	QString name;
};

class world_model
{
public:
	world_model();

	void clear();

	// --- elements ---
	std::map<int, node_data> nodes;
	std::map<int, road_data> roads;
	std::map<int, light_data> lights;
	std::map<int, sign_data> signs;
	std::map<int, object_data> objects;

	ground_material ground = ground_material::grass;
	QString name;
	QString description;
	double start_clock = 8.0 * 3600.0;   // s after midnight
	double geo_lat = 0.0;                // geographic origin (OpenStreetMap import, map
	double geo_lon = 0.0;                // background); 0, 0 = no origin
	bool has_geo() const { return geo_lat != 0.0 || geo_lon != 0.0; }

	// --- revisions ---
	quint64 revision = 1;
	quint64 network_revision = 1;
	void touch() { ++revision; }
	void touch_network() { ++revision; ++network_revision; }

	// --- ids ---
	int next_id();
	void reserve_id(int id);

	// --- access ---
	node_data *node(int id);
	const node_data *node(int id) const;
	road_data *road(int id);
	const road_data *road(int id) const;
	light_data *light(int id);
	const light_data *light(int id) const;
	sign_data *sign(int id);
	const sign_data *sign(int id) const;
	object_data *object(int id);
	const object_data *object(int id) const;

	std::vector<int> roads_at_node(int node_id) const;
	int light_at_node(int node_id) const;
	// The smooth center line node_a -> shape -> node_b (point every 1 m).
	polyline road_centerline(const road_data &r) const;

	// --- edit operations (each one updates the revisions) ---
	int add_node(const vec2 &pos);
	int add_road(int node_a, int node_b, const std::vector<vec2> &shape, const road_data &templ);
	// Splits the road at distance s. Returns the id of the new node.
	int split_road(int road_id, double s);
	// Removes the element and all elements that need it.
	void remove(const element_ref &ref);
	// Moves a node. The roads follow it.
	void move_node(int node_id, const vec2 &pos);
	// Adds a traffic light with a default program to the node. Returns the light id.
	int add_light(int node_id);
	// Makes the default program: one phase for each pair of opposite roads.
	void default_light_program(light_data &light) const;
	// Removes the traffic lights that point to roads that do not exist.
	void repair_lights();

	// Total length of all roads (km), for information.
	double total_road_km() const;

private:
	int m_next_id;
};

QString ground_name(ground_material g);
ground_material ground_from_name(const QString &name);
QString object_kind_name(object_kind k);
object_kind object_kind_from_name(const QString &name);
QString light_algorithm_name(light_algorithm a);
light_algorithm light_algorithm_from_name(const QString &name);

#endif // WORLD_MODEL_H
