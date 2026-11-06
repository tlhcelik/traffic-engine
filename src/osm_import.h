#ifndef OSM_IMPORT_H
#define OSM_IMPORT_H

#include <QByteArray>
#include <QString>
#include <QStringList>

class world_model;

// osm_import
// ----------
// OpenStreetMap data (OSM XML) -> world model. The result is a normal
// workspace: all roads, lights, signs and objects are editable.
//
// Source: an .osm file (for example the export of openstreetmap.org) or the
// answer of the Overpass API (overpass_query()).
//
// Steps of convert():
//   1. read the nodes and the ways (Xerces-C DOM)
//   2. select the roads for vehicles (highway = motorway ... living_street,
//      service if the option is on). Skip the roads without access.
//   3. project the nodes (geo_projection, origin = center of the area) and
//      cut the ways at the border of the area
//   4. split the ways at the shared nodes -> segments
//   5. join the junction parts that are nearer than the join distance (dual
//      carriageways, small junction islands) -> one node for each group
//   6. two one-way roads between the same two nodes in opposite directions
//      -> one two-way road with a median (dual carriageway)
//   7. join chains of roads at nodes with two roads (same attributes)
//   8. attributes from the tags: lanes, oneway, maxspeed, name, sidewalk,
//      cycleway, overtaking
//   9. traffic lights (highway=traffic_signals at or near a junction), stop
//      and give way signs, give way signs on the minor roads (road class),
//      level crossings (railway=level_crossing), bus stops (highway=bus_stop)
//  10. traffic sources at the dead ends: the rate comes from the road class
//      and the number of lanes
//
// The data is (c) OpenStreetMap contributors, license ODbL. The workspace
// description contains this note.

struct osm_bounds
{
	double min_lat = 0.0;
	double min_lon = 0.0;
	double max_lat = 0.0;
	double max_lon = 0.0;

	bool valid() const { return max_lat > min_lat && max_lon > min_lon; }
	double center_lat() const { return (min_lat + max_lat) / 2.0; }
	double center_lon() const { return (min_lon + max_lon) / 2.0; }
};

struct osm_import_options
{
	bool residential = true;         // residential, living_street, unclassified
	bool service_roads = false;      // highway=service (no parking aisles)
	double join_distance = 18.0;     // m: junction parts nearer than this become one junction
	bool lights = true;              // traffic lights (highway=traffic_signals)
	bool priority_signs = true;      // give way signs on the minor roads (road class)
	bool bus_stops = true;
	bool level_crossings = true;
	bool sidewalks = true;           // sidewalks on urban roads if OSM has no tag
	double demand_factor = 1.0;      // factor for the rates of the traffic sources
};

struct osm_import_result
{
	int osm_nodes = 0;
	int osm_ways = 0;
	int used_ways = 0;
	int nodes = 0;
	int roads = 0;
	int lights = 0;
	int signs = 0;
	int objects = 0;
	int sources = 0;
	double road_km = 0.0;
	QStringList notes;

	// translated: in the language of the user interface (the log uses English).
	QString summary(bool translated = false) const;
};

namespace osm_import
{
	// Area around a center point (width and height in meters).
	osm_bounds area(double lat, double lon, double width_m, double height_m);
	// Overpass QL query for the roads, their nodes and the bus stops of the area.
	QString overpass_query(const osm_bounds &b, const osm_import_options &opt);
	// Converts OSM XML into the model (the model is cleared first).
	// bounds: the clip area. If it is not valid: the <bounds> element of the
	// data; if there is none, no clip.
	bool convert(const QByteArray &osm_xml, const osm_bounds &bounds, const osm_import_options &opt,
		world_model &model, osm_import_result *result, QString *error);
}

#endif // OSM_IMPORT_H
