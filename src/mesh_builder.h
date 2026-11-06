#ifndef MESH_BUILDER_H
#define MESH_BUILDER_H

#include "geom.h"

#include <QColor>
#include <QRectF>

#include <vector>

// mesh_builder
// ------------
// CPU vertex lists for the OpenGL renderer (world coordinates in meters).
//
// flat_mesh   colored triangles: roads, sidewalks, markings, junctions
// shape_batch quads that the shape shader fills with smooth shapes (signed
//             distance functions): vehicles, pedestrians, lamps, cones, rings.
//             The edges are smooth at all zoom levels (no pixel art).
// tex_batch   textured quads (sign faces from the sign atlas)

struct flat_vertex
{
	float x, y;
	float r, g, b, a;
};

struct shape_vertex
{
	float x, y;      // world position
	float lx, ly;    // local position in meters (x: forward, y: left)
	float hx, hy;    // half length, half width
	float r, g, b, a;
	float kind, extra;
};

struct tex_vertex
{
	float x, y;
	float u, v;
	float a;
};

class flat_mesh
{
public:
	std::vector<flat_vertex> v;

	void clear() { v.clear(); }
	bool empty() const { return v.empty(); }
	void tri(const vec2 &a, const vec2 &b, const vec2 &c, const QColor &col);
	// a-b-c-d in order around the quad
	void quad(const vec2 &a, const vec2 &b, const vec2 &c, const vec2 &d, const QColor &col);
	// A band along the line between two lateral offsets (positive = left).
	void band(const polyline &line, double left, double right, const QColor &col);
	// Dashes along the line at a lateral offset.
	void dashes(const polyline &line, double offset, double width, double dash, double gap, const QColor &col);
	// Convex polygon (triangle fan)
	void fan(const std::vector<vec2> &convex, const QColor &col);
	void circle(const vec2 &c, double r, const QColor &col, int segments = 20);
	void segment(const vec2 &a, const vec2 &b, double width, const QColor &col);
	void thick_line(const std::vector<vec2> &pts, double width, const QColor &col);
	// Rectangle with a center, a direction and half sizes
	void rect(const vec2 &center, const vec2 &dir, double half_len, double half_wid, const QColor &col);
};

class shape_batch
{
public:
	enum kind_t
	{
		k_car = 0,
		k_bus = 1,
		k_motorcycle = 2,
		k_pedestrian = 3,
		k_disc = 4,
		k_cone = 6,
		k_ring = 7,
		k_rounded = 8
	};

	std::vector<shape_vertex> v;

	void clear() { v.clear(); }
	bool empty() const { return v.empty(); }
	// margin: extra space around the shape (shadow, glow)
	void add(const vec2 &center, const vec2 &dir, double half_len, double half_wid, double margin,
		const QColor &col, int kind, double extra = 0.0);
};

class tex_batch
{
public:
	std::vector<tex_vertex> v;

	void clear() { v.clear(); }
	bool empty() const { return v.empty(); }
	// Square aligned with the screen (signs are always upright).
	void add(const vec2 &center, double half_size, const QRectF &uv, double alpha);
	// Rectangle lo (south-west) .. hi (north-east). uv: image coordinates (top = north).
	void add_rect(const vec2 &lo, const vec2 &hi, const QRectF &uv, double alpha);
};

#endif // MESH_BUILDER_H
