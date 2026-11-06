#ifndef DEMO_WORLD_H
#define DEMO_WORLD_H

class world_model;

// demo_world
// ----------
// A sample city for the first start and for tests: a main road (2 + 2 lanes)
// with a signalized crossroad, a T-junction with a STOP sign, a junction with
// a give-way sign, a level crossing, a bus stop, cones, speed limit signs and
// traffic sources at all dead ends.

namespace demo_world
{
	void build(world_model &m);
}

#endif // DEMO_WORLD_H
