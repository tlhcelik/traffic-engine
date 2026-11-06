#ifndef SIMULATION_H
#define SIMULATION_H

#include "geom.h"
#include "route_planner.h"
#include "stats_collector.h"
#include "vehicle_types.h"

#include <QtGlobal>

#include <algorithm>
#include <deque>
#include <map>
#include <random>
#include <vector>

class world_model;
class road_network;
struct net_lane;
struct net_connector;

// simulation
// ----------
// The traffic simulation of one workspace. Fixed time step (default 0.1 s).
// advance() runs real time: it adds wall time x speed factor and does the
// necessary steps (a maximum number for each frame, so the user interface
// does not stop).
//
// Each step:
//   1. traffic lights (green, yellow, all red, red + yellow)
//   2. new vehicles and pedestrians at the dead-end nodes (demand x daily
//      profile x global traffic density)
//   3. index of the vehicles on each lane and connector (sorted by position)
//   4. lane changes (mandatory for the route, cones, bus stops; optional
//      where overtaking is allowed, with the keep-right rule)
//   5. junction entry: a vehicle enters a junction only with a reservation.
//      It gets the reservation if the light is green (or yellow and it cannot
//      stop), if no vehicle with priority comes (stop and yield signs,
//      right-hand rule, permissive left turn), if no conflicting movement
//      has a reservation, if the exit lane has space (no blocked junction)
//      and if no pedestrian is on the crosswalks.
//   6. acceleration: IDM with the leader and with the fixed obstacles (stop
//      line, cones, closed level crossing, bus stop, occupied crosswalk) and
//      deceleration before lower speed limits and curves
//   7. movement, transfer lane -> connector -> lane, end of the trip
//   8. pedestrians (sidewalks, corners, crosswalks)
//   9. statistics
//
// The simulation reads the world model at each step. Thus, a property change
// (speed limit, light times, demand, ...) has an effect immediately.
// A change of the road geometry needs network_changing() before and
// network_changed() after the new road network is made.

enum class signal_color
{
	off = 0,
	red,
	red_yellow,
	green,
	yellow
};

// Why a vehicle waits or drives slower than it wants (state in the user
// interface, wait chains in the diagnostics). cause_id is the id of the
// element that causes the wait (see cause_target()).
enum class wait_cause
{
	none = 0,
	queue,              // the vehicle ahead (vehicle)
	red_light,          // red, red + yellow or yellow (light)
	stop_sign,          // full stop at the STOP sign (sign)
	give_way,           // a vehicle with priority comes (vehicle)
	conflict,           // a conflicting movement has the junction (vehicle)
	exit_full,          // no space after the junction (vehicle)
	junction_blocked,   // a vehicle stands in the junction on the same path (vehicle)
	pedestrians,        // pedestrians on the crosswalk (pedestrian)
	bus_stop,           // passengers get in and out (object)
	level_crossing,     // the barriers are closed (object)
	lane_blocked,       // cones, barrier or road closure (object)
	merge_courtesy,     // lets a vehicle into the lane (vehicle)
	no_route            // no movement from this lane to the route
};

// The kind of the element in cause_id.
enum class cause_target
{
	none = 0,
	vehicle,
	light,
	sign,
	pedestrian,
	object
};
cause_target target_of(wait_cause c);
// English name of the cause (log lines).
const char *wait_cause_name(wait_cause c);

struct sim_vehicle
{
	int id = -1;
	vehicle_kind kind = vehicle_kind::sedan;
	const vehicle_params *p = nullptr;
	quint32 color = 0xA7C7E7;
	int passengers = 1;
	double v0_factor = 1.0;      // desired speed / speed limit (never above 1)

	// position
	bool on_connector = false;
	int seg = -1;                // lane id or connector id
	double s = 0.0;              // front bumper position on the segment
	double v = 0.0;              // m/s
	double a = 0.0;              // m/s2
	int prev_seg = -1;
	bool prev_on_connector = false;

	// lane change
	int lc_from = -1;
	double lc_progress = 1.0;
	double lc_cooldown = 0.0;
	int courtesy_for = -1;        // gives way to this vehicle (zipper merge)
	double courtesy_until = 0.0;

	// route
	std::vector<int> route;      // directed road keys
	int route_pos = 0;
	int origin_node = -1;
	int dest_node = -1;
	int next_conn = -1;          // planned connector at the end of the current lane
	int target_index = -1;       // lane index that the route needs (-1: the current lane is good)
	bool committed = false;      // the vehicle has a reservation for next_conn
	int held_conn = -1;          // connector with a reservation until the vehicle leaves it
	int held_prev = -1;          // the last junction on a short link: its rear is still in it
	int held_next = -1;          // the junction after a link that is shorter than the vehicle
	                             // (granted together with held_conn: no stop in the link)
	bool stop_done = false;      // full stop at a stop sign is done
	double stop_wait = 0.0;
	double wait_at_line = 0.0;
	bool gridlock_rerouted = false; // another movement was tried (gridlock rule)
	double stuck_time = 0.0;
	int served_stop = -1;
	int dwell_stop = -1;
	double dwell_left = 0.0;
	bool moving = false;         // hysteresis for the stop count
	bool finished = false;

	// wait state
	wait_cause cause = wait_cause::none;      // the limit of the acceleration now
	int cause_id = -1;
	wait_cause entry_cause = wait_cause::none; // why the junction entry is not possible
	int entry_id = -1;
	double standstill = 0.0;     // s without movement (v < 0.2 m/s)

	// statistics
	double spawn_time = 0.0;
	double distance = 0.0;       // m
	double fuel_l = 0.0;
	double fuel_rate = 0.0;      // L/s now
	double co2_kg = 0.0;
	int stops = 0;
	double stop_time = 0.0;
	double congested_time = 0.0;
	double congested_distance = 0.0;
	double junction_wait = 0.0;
	double light_wait = 0.0;

	// drawing
	vec2 pos;
	vec2 dir = vec2(1.0, 0.0);
};

struct sim_pedestrian
{
	struct leg
	{
		polyline path;
		int crosswalk = -1;
	};

	int id = -1;
	std::vector<leg> legs;
	int leg_index = 0;
	double s = 0.0;
	double speed = 1.3;
	double lateral = 0.0;
	bool waiting = false;
	bool on_crosswalk = false;
	double wait_time = 0.0;
	int origin_node = -1;
	int dest_node = -1;
	double spawn_time = 0.0;
	quint32 color = 0xF7C6D9;
	vec2 pos;
	vec2 dir = vec2(1.0, 0.0);
};

struct light_runtime
{
	int light_id = -1;
	int node_id = -1;
	int phase = 0;
	int stage = 1;               // 0 red+yellow, 1 green, 2 yellow, 3 all red
	double stage_left = 0.0;
	double phase_time = 0.0;
	bool external = false;
	double external_until = 0.0;
	std::map<int, int> waiting;  // incoming road -> waiting vehicles
};

enum class crossing_state
{
	open = 0,
	closing,
	closed,
	opening
};

// Congestion of one approach of a junction (an incoming road). Time window:
// simulation::congestion_window() (default 5 minutes).
struct approach_status
{
	int road_id = -1;
	double queue_m = 0.0;        // now: queue length before the stop line
	int queue_veh = 0;           // now: vehicles in the queue
	double max_queue_m = 0.0;    // longest queue in the time window
	double length = 0.0;         // m: the approach (incoming lanes of the road)
	double delay = 0.0;          // s per vehicle in the time window
	int passages = 0;            // vehicles into the junction in the time window
	bool spillback = false;      // the queue fills the approach: it blocks the junction before it
	wait_cause cause = wait_cause::none; // main cause of the waits in the time window
};

// Congestion of one junction (simulation::congestion()). The level of service
// (LOS) uses the control delay limits of the Highway Capacity Manual 2010:
// with lights A <= 10, B <= 20, C <= 35, D <= 55, E <= 80 s, F above; without
// lights A <= 10, B <= 15, C <= 25, D <= 35, E <= 50 s, F above. A spillback
// gives F.
struct junction_status
{
	int node_id = -1;
	bool signalized = false;
	double delay = 0.0;          // s per vehicle (vehicles through the junction and vehicles in the queues)
	char los = 'A';
	double flow = 0.0;           // vehicles per hour through the junction
	double queue_m = 0.0;        // longest queue of an approach now
	int waiting = 0;             // vehicles in the queues now
	bool spillback = false;
	wait_cause cause = wait_cause::none; // main cause: the largest part of the waiting time
	double cause_share = 0.0;    // 0..1
	int blocking_node = -1;      // exit full: the next junction (the queue comes from there)
	double score = 0.0;          // order of the hotspot list (the worst first)
	std::vector<approach_status> approaches;
};

// A problem that the checks found (set_checks): two vehicles overlap, or a
// vehicle does not move for a long time (with its wait chain).
struct sim_issue
{
	enum kind_t
	{
		overlap = 0,
		stuck,
		deadlock,
		leak            // a reservation count does not agree with the vehicles
	};
	kind_t kind = overlap;
	double time = 0.0;
	int vehicle_a = -1;
	int vehicle_b = -1;
	vec2 pos;
	QString text;                // English (log)
};

class simulation
{
public:
	simulation();
	~simulation();

	// A new run: no vehicles, lights at the start, clock = start clock of the model.
	void reset(const world_model *model, const road_network *net);
	// Call before and after the road network is made again (geometry change).
	void network_changing();
	void network_changed();

	// Real time: wall_dt seconds x speed. Returns the number of steps.
	int advance(double wall_dt, double speed, int max_steps);
	void step(double dt);

	double time() const { return m_time; }
	double clock() const;
	// Seed of the random numbers (reset() starts with it again).
	void set_seed(quint32 seed) { m_seed = seed; m_rng.seed(seed); }
	double time_step() const { return m_dt; }
	void set_time_step(double dt);
	double actual_speed() const { return m_actual_speed; }
	double last_step_ms() const { return m_last_step_ms; }
	void set_stats_interval(double s) { m_stats_interval = s > 0.1 ? s : 1.0; }

	// Global traffic density (toolbar button "Traffic density"): factors for
	// the rates of all traffic sources (1.0 = the rates of the nodes). A change
	// applies immediately: the next arrival times are calculated again.
	void set_demand_factors(double vehicles, double pedestrians);
	double vehicle_demand_factor() const { return m_vehicle_factor; }
	double pedestrian_demand_factor() const { return m_ped_factor; }
	// Daily demand profile (0.18 at night, 1.0 at the peak hours 08:00 and 18:00).
	static double demand_profile(double clock_s);
	// Gridlock rule: the first vehicle of a lane that waits longer than this
	// time (not for a red light) leaves the simulation. After half of the time
	// with a full exit, it tries another movement. 0 = off.
	void set_gridlock_timeout(double s) { m_gridlock_timeout = s > 0.0 ? std::max(30.0, s) : 0.0; }
	double gridlock_timeout() const { return m_gridlock_timeout; }
	quint64 gridlock_removed() const { return m_gridlock_removed; }

	const std::vector<sim_vehicle> &vehicles() const { return m_vehicles; }
	const std::vector<sim_pedestrian> &pedestrians() const { return m_peds; }
	const sim_vehicle *find_vehicle(int id) const;
	const sim_pedestrian *find_pedestrian(int id) const;
	// Polyline of the rest of the route of a vehicle (for the drawing).
	std::vector<vec2> route_line(int vehicle_id) const;
	quint64 finished_vehicles() const { return m_finished_count; }

	// --- traffic lights ---
	const light_runtime *light_state(int light_id) const;
	signal_color signal_for(int node_id, int road_id) const;
	// Seconds until the color of the approach changes (green or yellow: until
	// red; red: until green).
	double signal_remaining(int node_id, int road_id) const;
	int waiting_at(int node_id, int road_id) const;
	// External phase (API): holds the phase for "duration" seconds.
	void set_external_phase(int light_id, int phase, double duration);

	// --- objects ---
	crossing_state level_crossing(int object_id, double *barrier, double *train_pos) const;
	int bus_stop_waiting(int object_id) const;
	int crosswalk_users(int crosswalk_id) const;

	stats_collector &stats() { return m_stats; }
	const stats_collector &stats() const { return m_stats; }

	// --- congestion analysis (sim_congestion.cpp) ---
	// The junctions with their congestion, the worst first (junctions with
	// three or more roads or a traffic light). Updated each simulated second.
	const std::vector<junction_status> &congestion() const { return m_congestion; }
	const junction_status *congestion_at(int node_id) const;
	void set_congestion_window(double seconds) { m_cong_window = std::max(30.0, seconds); }
	double congestion_window() const { return m_cong_window; }
	static char los_letter(double delay, bool signalized);

	// --- checks (debug mode, --sim-test) ---
	// Overlap: the rectangles of two vehicles intersect. Stuck: a vehicle does
	// not move for stuck_limit seconds (not for a red light): the issue has
	// the wait chain (vehicle -> cause -> vehicle ...); a closed chain is a
	// deadlock.
	void set_checks(bool on, double stuck_limit = 120.0);
	bool checks() const { return m_checks; }
	std::vector<sim_issue> take_issues();
	quint64 issue_count(sim_issue::kind_t k) const { return m_issue_counts[k]; }
	// Wait chain of a vehicle as text (English), for example
	// "12 (give way) -> 45 (exit full) -> 77 (queue) -> 12: deadlock".
	// Level of the road of the vehicle (bridge 1, tunnel -1; a junction: the incoming road).
	int level_of(const sim_vehicle &v) const;
	// *root: the cause at the end of the chain.
	QString wait_chain(int vehicle_id, bool *cycle = nullptr, wait_cause *root = nullptr) const;

private:
	struct anchor
	{
		int road_id;
		int dir;
		int index;
		double road_s;
	};

	// steps
	void update_lights(double dt);
	void spawn_vehicles(double dt);
	void spawn_pedestrians(double dt);
	void build_index();
	void lane_changes(double dt);
	void junction_entry(double dt);
	void accelerations();
	void move(double dt);
	void update_pedestrians(double dt);
	void update_waiting();
	void collect_stats(double dt);
	void update_positions(double dt);

	// vehicles
	bool try_spawn_vehicle(int node_id);
	void plan_next(sim_vehicle &veh);
	// A new route from the current lane. need_space: only a movement (not the
	// planned one) whose exit lane has space.
	bool reroute(sim_vehicle &veh, bool need_space = false);
	// True if the exit lane of the connector has no space for the vehicle.
	bool exit_full(const sim_vehicle &veh, int conn) const;
	void release(sim_vehicle &veh);
	void release_commit(sim_vehicle &veh);
	// Reservation counts of a connector (all vehicles and long vehicles)
	void reserve(const sim_vehicle &veh, int conn);
	void unreserve(const sim_vehicle &veh, int conn);
	// True while the vehicle is before the connector, on it, or its rear is still in it.
	bool uses_connector(const sim_vehicle &veh, int conn) const;
	void finish(sim_vehicle &veh);
	// leader: index of the vehicle ahead (-1: none).
	double leader_gap(int vi, double *leader_v, int *leader = nullptr) const;
	// The nearest rear of a vehicle in the lane, from the start of the lane:
	// vehicles on the lane, vehicles on the next connectors whose rear is still in
	// the lane, and (short links, depth) the vehicles after the connector to the
	// next road of the route of veh. inf: no vehicle (*who = -1).
	double first_rear(int lane_id, int *who, int depth, const sim_vehicle *veh) const;
	// True if two connectors from the same lane are still near each other at s.
	bool paths_close(const net_connector &a, const net_connector &b, double s, double widths) const;
	// The nearest fixed obstacle: *cause and *cause_id tell what it is.
	double obstacle_gap(int vi, wait_cause *cause = nullptr, int *cause_id = nullptr) const;
	// IDM acceleration. s0 < 0: the minimum gap of the vehicle type (leader);
	// fixed obstacles (stop line, bus stop) use a small value.
	double idm(const sim_vehicle &veh, double v0, double gap, double dv, double s0 = -1.0) const;
	double desired_speed(const sim_vehicle &veh) const;
	double speed_constraint_accel(const sim_vehicle &veh) const;
	// *cause, *cause_id: why the entry is not possible.
	bool can_enter(const sim_vehicle &veh, bool *must_wait_light, wait_cause *cause, int *cause_id) const;
	int pedestrian_on_crosswalk(int crosswalk_id) const;
	// A left turner that waited at the line during its green: it may leave the
	// junction during the yellow and the all red time of its phase.
	bool clearing_turn(const sim_vehicle &veh) const;
	// The connector from the lane to the road key (straight first). -1: none.
	int choose_connector(const net_lane &l, int road_key) const;
	// A link shorter than the vehicle: can the vehicle pass the next junction
	// (connector c2 at the end of the link) now? *cause: why not.
	bool through_ok(const sim_vehicle &veh, int c2, wait_cause *cause, int *cause_id) const;
	// Space for the vehicle in the lane: the free length behind the last
	// vehicle, minus the space of the reservations to the lane.
	// Free space in the lane for veh: to the rear of the last vehicle; an empty
	// lane: to its stop line (ends_here: the trip of veh ends in this lane, to
	// the end of the lane).
	double exit_space(const sim_vehicle &veh, int lane_id, int *last, int *claims, bool ends_here = false) const;
	// The vehicle that takes the space of a lane: the last vehicle (index last),
	// or a vehicle with a reservation to the lane. -1: none.
	int exit_blocker(int lane_id, int last) const;
	bool must_yield(const sim_vehicle &veh, const net_connector &c, int *other = nullptr) const;
	bool lane_change_safe(int vi, int target_lane, double *gain) const;
	double lane_accel_with(int vi, int lane_id, double s_mapped) const;
	int front_vehicle_on_lane(int lane_id) const;
	int last_vehicle_on_lane(int lane_id) const;
	double crosswalk_gap_ahead(const sim_vehicle &veh) const;

	// pedestrians
	bool try_spawn_pedestrian(int node_id);
	bool build_pedestrian_legs(sim_pedestrian &p, const std::vector<std::pair<int, int>> &route);
	bool crossing_allowed(int crosswalk_id, double speed) const;

	// lights
	void init_light(light_runtime &lr);
	void advance_stage(light_runtime &lr);
	const light_runtime *light_at_node(int node_id) const;
	bool phase_has_road(const light_runtime &lr, int phase, int road_id) const;

	void index_connectors();
	// congestion (sim_congestion.cpp)
	void update_congestion(double dt);
	// checks (sim_checks.cpp)
	void run_checks(double dt);
	void check_overlaps();
	void check_stuck();
	double random01();
	double random_exp(double mean);
	quint32 vehicle_color(vehicle_kind k);

	const world_model *m_model;
	const road_network *m_net;
	route_planner m_routes;
	stats_collector m_stats;
	std::mt19937 m_rng;
	quint32 m_seed;

	std::vector<sim_vehicle> m_vehicles;
	std::vector<sim_pedestrian> m_peds;
	std::map<int, int> m_vehicle_index;            // id -> index (rebuilt each step)
	std::vector<std::vector<int>> m_on_lane;       // vehicle indices, front first
	std::vector<std::vector<int>> m_on_conn;
	std::vector<int> m_reservations;               // per connector
	std::vector<int> m_res_long;                   // per connector: reservations of long vehicles
	std::vector<int> m_holder;                     // per connector: a vehicle id with a reservation
	std::vector<int> m_claimer;                    // per connector: a vehicle that waits for it (this step)
	std::vector<char> m_claim_long;                // per connector: the claimer is a long vehicle
	std::vector<double> m_claim_wait;              // per connector: wait time of the claimer at its line
	std::vector<char> m_claim_merge;               // per connector: the claim stops only the same exit lane
	std::vector<int> m_space_claim;                // per lane: a vehicle that waits long for space in it
	std::vector<std::vector<int>> m_leaving;       // per lane: vehicles that change from it to another lane
	std::vector<std::vector<int>> m_conn_by_to_lane; // connectors that end at each lane
	std::vector<int> m_cw_users;                   // per crosswalk
	std::vector<double> m_gap_cache;               // per vehicle (leader or obstacle)
	std::map<int, light_runtime> m_lights;         // light id -> state
	std::map<int, int> m_node_light;               // node id -> light id
	std::map<int, double> m_next_spawn;            // node id -> time
	std::map<int, double> m_next_ped_spawn;
	std::map<int, int> m_stop_waiting;             // bus stop -> waiting passengers
	std::map<int, double> m_stop_accum;
	std::vector<anchor> m_anchors;

	double m_time;
	double m_dt;
	double m_accum;
	double m_actual_speed;
	double m_last_step_ms;
	double m_stats_interval;
	double m_stats_accum;
	double m_vehicle_factor;
	double m_ped_factor;
	double m_gridlock_timeout;
	quint64 m_gridlock_removed;
	int m_next_id;

	// statistics of finished vehicles
	quint64 m_finished_count;
	double m_finished_distance;
	double m_finished_fuel;
	double m_finished_co2;
	double m_finished_stops;
	double m_finished_stop_time;
	double m_finished_congested_time;
	double m_finished_congested_distance;
	double m_finished_light_wait;
	std::vector<double> m_finish_times;            // for the throughput
	std::vector<std::pair<double, double>> m_junction_waits;   // (time, wait)

	// congestion
	struct passage
	{
		double time;
		int node;
		int road;
		double wait;             // s: junction wait of the vehicle at this junction
	};
	struct wait_sample
	{
		double time;
		int node;
		int road;
		wait_cause cause;
		double veh_s;            // vehicle seconds in the queue in this second
		int blocking_node;
	};
	std::deque<passage> m_passages;
	std::deque<wait_sample> m_wait_samples;
	std::map<std::pair<int, int>, std::deque<std::pair<double, double>>> m_queue_samples; // (node, road) -> (time, queue m)
	std::vector<junction_status> m_congestion;
	double m_cong_window;
	double m_cong_accum;

	// checks
	bool m_checks;
	double m_stuck_limit;
	double m_check_accum;
	std::vector<sim_issue> m_issues;
	quint64 m_issue_counts[4];
	std::map<std::pair<int, int>, double> m_overlap_seen;   // vehicle pair -> last report time
	std::map<int, double> m_stuck_seen;                     // vehicle -> standstill at the last report
};

#endif // SIMULATION_H
