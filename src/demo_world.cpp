#include "demo_world.h"
#include "i18n.h"
#include "world_model.h"

namespace
{
	int add_sign(world_model &m, const char *key, int road, double s, int dir, int value = 0)
	{
		sign_data sd;
		sd.id = m.next_id();
		sd.key = QString::fromLatin1(key);
		sd.road_id = road;
		sd.s = s;
		sd.dir = dir;
		sd.value = value;
		m.signs[sd.id] = sd;
		return sd.id;
	}

	int add_object(world_model &m, object_kind kind, int road, double s, int dir)
	{
		object_data o;
		o.id = m.next_id();
		o.kind = kind;
		o.road_id = road;
		o.s = s;
		o.dir = dir;
		m.objects[o.id] = o;
		return o.id;
	}
}

namespace demo_world
{
	void build(world_model &m)
	{
		m.clear();
		// Data of the workspace: in the language of the user interface at the time of the build
		m.name = tx("Demo city");
		m.description = tx("A sample city: crossroad with traffic lights, T-junction with a STOP sign, level crossing and bus stop.");
		m.start_clock = 7.5 * 3600.0;

		// Nodes
		const int w = m.add_node(vec2(-420.0, 0.0));
		const int j1 = m.add_node(vec2(-220.0, 0.0));
		const int c = m.add_node(vec2(0.0, 0.0));
		const int j2 = m.add_node(vec2(240.0, 0.0));
		const int e = m.add_node(vec2(430.0, 0.0));
		const int s = m.add_node(vec2(0.0, -360.0));
		const int n = m.add_node(vec2(0.0, 360.0));
		const int n2 = m.add_node(vec2(240.0, 280.0));
		const int s1 = m.add_node(vec2(-230.0, -300.0));

		// Main road: 2 + 2 lanes, 60 km/h
		road_data main;
		main.lanes_forward = 2;
		main.lanes_backward = 2;
		main.speed_limit = 60.0;
		main.name = tx("Ataturk Avenue");
		const int r_w = m.add_road(w, j1, {}, main);
		const int r_wc = m.add_road(j1, c, {}, main);
		const int r_ce = m.add_road(c, j2, {}, main);
		const int r_e = m.add_road(j2, e, {}, main);

		// Cross road: 1 + 1 lanes, 50 km/h
		road_data cross;
		cross.name = tx("Cumhuriyet Street");
		const int r_s = m.add_road(s, c, {}, cross);
		const int r_n = m.add_road(c, n, { vec2(20.0, 180.0) }, cross);

		// Side roads
		road_data side;
		side.name = tx("School Street");
		side.speed_limit = 40.0;
		const int r_n2 = m.add_road(j2, n2, { vec2(260.0, 120.0) }, side);
		road_data side2;
		side2.name = tx("Station Road");
		side2.sidewalk_right = false;
		side2.ditch = true;
		const int r_s1 = m.add_road(j1, s1, { vec2(-190.0, -140.0) }, side2);

		// Demand: the main road ends have more traffic
		for (int id : { w, e })
		{
			m.node(id)->vehicles_per_hour = 550.0;
			m.node(id)->pedestrians_per_hour = 40.0;
		}
		for (int id : { s, n })
		{
			m.node(id)->vehicles_per_hour = 320.0;
			m.node(id)->pedestrians_per_hour = 80.0;
		}
		m.node(n2)->vehicles_per_hour = 180.0;
		m.node(n2)->pedestrians_per_hour = 120.0;
		m.node(s1)->vehicles_per_hour = 150.0;
		m.node(s1)->pedestrians_per_hour = 10.0;
		m.node(w)->share_bus = 0.08;

		// Traffic light at the crossroad (default program: main road, then cross road)
		const int lid = m.add_light(c);
		if (light_data *l = m.light(lid))
		{
			if (l->phases.size() >= 2)
			{
				l->phases[0].green = 30.0;
				l->phases[1].green = 20.0;
			}
		}

		// STOP at the T-junction (School Street -> main road): traffic b -> a
		add_sign(m, "stop", r_n2, 14.0, -1);
		add_sign(m, "children", r_n2, 60.0, 1);
		add_sign(m, "speed_limit", r_n2, 40.0, 1, 30);
		// Give way at Station Road -> main road
		add_sign(m, "give_way", r_s1, 14.0, -1);
		add_sign(m, "level_crossing_barrier", r_s1, 80.0, 1);
		add_sign(m, "level_crossing_barrier", r_s1, 230.0, -1);
		// Speed limits on the main road
		add_sign(m, "speed_limit", r_w, 25.0, 1, 70);
		add_sign(m, "speed_limit", r_e, 165.0, -1, 70);
		add_sign(m, "speed_limit", r_wc, 30.0, 1, 50);
		add_sign(m, "no_overtaking", r_ce, 20.0, 1);
		add_sign(m, "priority_road", r_e, 150.0, -1);

		// Objects
		const int lc = add_object(m, object_kind::level_crossing, r_s1, 150.0, 0);
		m.object(lc)->train_period = 180.0;
		m.object(lc)->closed_time = 35.0;
		const int bs = add_object(m, object_kind::bus_stop, r_n, 150.0, 1);
		m.object(bs)->name = tx("Market");
		const int cn = add_object(m, object_kind::cone, r_w, 95.0, 1);
		m.object(cn)->lane = 0;
		m.object(cn)->length = 18.0;
		Q_UNUSED(r_s);
		m.touch_network();
	}
}
