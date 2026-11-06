#ifndef GEO_H
#define GEO_H

#include "geom.h"

// geo
// ---
// Geographic coordinates (WGS 84 latitude and longitude) <-> world
// coordinates (meters, x to the east, y to the north).
//
// The world uses a local conformal projection: Web Mercator (EPSG:3857, the
// projection of the OpenStreetMap map tiles) multiplied by the cosine of the
// latitude of the origin. Thus:
//   * the scale is correct at the origin (error less than 0.1 % in 5 km),
//   * a map tile is an exact square in the world (no warp of the images).
//
// Tiles: zoom z has 2^z x 2^z tiles of 256 pixels. Tile (0, 0) is at the
// north-west corner of the Mercator square.

class geo_projection
{
public:
	geo_projection() = default;
	// Origin of the world (the point 0, 0).
	geo_projection(double lat0, double lon0);

	bool valid() const { return m_valid; }
	double lat0() const { return m_lat0; }
	double lon0() const { return m_lon0; }

	vec2 to_world(double lat, double lon) const;
	void to_geo(const vec2 &p, double *lat, double *lon) const;

	// Web Mercator meters <-> world meters
	vec2 mercator_to_world(double mx, double my) const;
	void world_to_mercator(const vec2 &p, double *mx, double *my) const;
	// World meters for one Mercator meter (cos(lat0)).
	double scale() const { return m_k; }

	static const double earth_radius;   // m (WGS 84 / Web Mercator sphere)
	static double mercator_x(double lon);
	static double mercator_y(double lat);
	// Edge length of one tile at zoom z (Mercator meters).
	static double tile_size(int z);
	// Tile index that contains the Mercator point.
	static int tile_x(double mx, int z);
	static int tile_y(double my, int z);
	// North-west corner of a tile (Mercator meters).
	static double tile_left(int x, int z);
	static double tile_top(int y, int z);

private:
	bool m_valid = false;
	double m_lat0 = 0.0;
	double m_lon0 = 0.0;
	double m_k = 1.0;
	double m_mx0 = 0.0;
	double m_my0 = 0.0;
};

#endif // GEO_H
