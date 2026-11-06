#ifndef WORLD_PROPERTIES_H
#define WORLD_PROPERTIES_H

#include "world_model.h"

#include <QList>
#include <QString>
#include <QStringList>
#include <QVariant>

// world_properties
// ----------------
// The properties of the elements for the property panel. Each element kind
// (workspace, node, road, traffic light, sign, object) gives a list of
// property_def. set() writes a new value to the model and tells what the
// change needs:
//   change_none        the simulation reads the model at each step
//   change_attributes  the lanes get new values (speed limit, overtaking)
//   change_network     the road network must be made again (geometry)

struct property_def
{
	enum type_t
	{
		t_bool,
		t_int,
		t_double,
		t_string,
		t_enum,
		t_info
	};

	QString key;
	QString label;
	QString group;
	QString unit;
	type_t type = t_info;
	QVariant value;
	double min = 0.0;
	double max = 0.0;
	double step = 1.0;
	int decimals = 1;
	QStringList options;     // t_enum: shown texts; the value is the index
	bool mixed = false;      // more roads: the roads have different values (the value of the main road shows)
};

enum class property_change
{
	change_none = 0,
	change_attributes,
	change_network
};

namespace world_properties
{
	// The workspace properties have ref.kind == element_kind::none.
	QList<property_def> list(const world_model &m, const element_ref &ref);
	// Returns false if the value did not change.
	bool set(world_model &m, const element_ref &ref, const QString &key, const QVariant &value, property_change *change);
	// translated = false: English (for the log).
	QString title(const world_model &m, const element_ref &ref, bool translated = true);
	// More selected roads (the main road last): a summary, then the road
	// properties with the values of the main road. "mixed": the roads have
	// different values.
	QList<property_def> list_roads(const world_model &m, const std::vector<int> &roads);
	// The value goes to all roads. Returns false if no road changed.
	bool set_roads(world_model &m, const std::vector<int> &roads, const QString &key, const QVariant &value, property_change *change);
}

#endif // WORLD_PROPERTIES_H
