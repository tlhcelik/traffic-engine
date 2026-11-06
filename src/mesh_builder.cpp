#include "mesh_builder.h"

#include <algorithm>
#include <cmath>

namespace
{
	inline flat_vertex fv(const vec2 &p, const QColor &c)
	{
		return flat_vertex{ static_cast<float>(p.x), static_cast<float>(p.y),
			static_cast<float>(c.redF()), static_cast<float>(c.greenF()), static_cast<float>(c.blueF()), static_cast<float>(c.alphaF()) };
	}
}

// ---------------------------------------------------------------------------
// flat_mesh
// ---------------------------------------------------------------------------
void flat_mesh::tri(const vec2 &a, const vec2 &b, const vec2 &c, const QColor &col)
{
	v.push_back(fv(a, col));
	v.push_back(fv(b, col));
	v.push_back(fv(c, col));
}

void flat_mesh::quad(const vec2 &a, const vec2 &b, const vec2 &c, const vec2 &d, const QColor &col)
{
	tri(a, b, c, col);
	tri(a, c, d, col);
}

void flat_mesh::band(const polyline &line, double left, double right, const QColor &col)
{
	if (line.empty())
	{
		return;
	}
	const std::vector<vec2> &pts = line.points();
	const size_t n = pts.size();
	// Offset points (miter joins), the same count as the line points
	std::vector<vec2> l(n);
	std::vector<vec2> r(n);
	for (size_t i = 0; i < n; ++i)
	{
		vec2 normal;
		if (i == 0)
		{
			normal = (pts[1] - pts[0]).normalized().left();
		}
		else if (i == n - 1)
		{
			normal = (pts[n - 1] - pts[n - 2]).normalized().left();
		}
		else
		{
			const vec2 n0 = (pts[i] - pts[i - 1]).normalized().left();
			const vec2 n1 = (pts[i + 1] - pts[i]).normalized().left();
			vec2 m = n0 + n1;
			const double ml = m.length();
			if (ml < 1e-6)
			{
				normal = n1;
			}
			else
			{
				m = m / ml;
				normal = m / std::max(0.35, dot(m, n1));
			}
		}
		l[i] = pts[i] + normal * left;
		r[i] = pts[i] + normal * right;
	}
	for (size_t i = 0; i + 1 < n; ++i)
	{
		quad(r[i], r[i + 1], l[i + 1], l[i], col);
	}
}

void flat_mesh::dashes(const polyline &line, double offset, double width, double dash, double gap, const QColor &col)
{
	const double len = line.length();
	if (len <= 0.0)
	{
		return;
	}
	for (double s = 0.0; s < len; s += dash + gap)
	{
		const double s1 = std::min(len, s + dash);
		if (s1 - s < 0.2)
		{
			break;
		}
		band(line.sub(s, s1), offset + width / 2.0, offset - width / 2.0, col);
	}
}

void flat_mesh::fan(const std::vector<vec2> &convex, const QColor &col)
{
	for (size_t i = 1; i + 1 < convex.size(); ++i)
	{
		tri(convex[0], convex[i], convex[i + 1], col);
	}
}

void flat_mesh::circle(const vec2 &c, double r, const QColor &col, int segments)
{
	segments = std::max(6, segments);
	for (int i = 0; i < segments; ++i)
	{
		const double a0 = 2.0 * M_PI * i / segments;
		const double a1 = 2.0 * M_PI * (i + 1) / segments;
		tri(c, c + from_angle(a0) * r, c + from_angle(a1) * r, col);
	}
}

void flat_mesh::segment(const vec2 &a, const vec2 &b, double width, const QColor &col)
{
	const vec2 d = (b - a).normalized();
	const vec2 n = d.left() * (width / 2.0);
	quad(a - n, b - n, b + n, a + n, col);
}

void flat_mesh::thick_line(const std::vector<vec2> &pts, double width, const QColor &col)
{
	if (pts.size() < 2)
	{
		return;
	}
	band(polyline(pts), width / 2.0, -width / 2.0, col);
}

void flat_mesh::rect(const vec2 &center, const vec2 &dir, double half_len, double half_wid, const QColor &col)
{
	const vec2 d = dir.normalized();
	const vec2 n = d.left();
	const vec2 a = center - d * half_len - n * half_wid;
	const vec2 b = center + d * half_len - n * half_wid;
	const vec2 c = center + d * half_len + n * half_wid;
	const vec2 e = center - d * half_len + n * half_wid;
	quad(a, b, c, e, col);
}

// ---------------------------------------------------------------------------
// shape_batch
// ---------------------------------------------------------------------------
void shape_batch::add(const vec2 &center, const vec2 &dir, double half_len, double half_wid, double margin,
	const QColor &col, int kind, double extra)
{
	const vec2 d = dir.normalized();
	const vec2 n = d.left();
	const double ex = half_len + margin;
	const double ey = half_wid + margin;
	const float r = static_cast<float>(col.redF());
	const float g = static_cast<float>(col.greenF());
	const float b = static_cast<float>(col.blueF());
	const float a = static_cast<float>(col.alphaF());
	auto corner = [&](double lx, double ly)
	{
		const vec2 p = center + d * lx + n * ly;
		return shape_vertex{ static_cast<float>(p.x), static_cast<float>(p.y),
			static_cast<float>(lx), static_cast<float>(ly),
			static_cast<float>(half_len), static_cast<float>(half_wid),
			r, g, b, a, static_cast<float>(kind), static_cast<float>(extra) };
	};
	const shape_vertex v0 = corner(-ex, -ey);
	const shape_vertex v1 = corner(ex, -ey);
	const shape_vertex v2 = corner(ex, ey);
	const shape_vertex v3 = corner(-ex, ey);
	v.push_back(v0);
	v.push_back(v1);
	v.push_back(v2);
	v.push_back(v0);
	v.push_back(v2);
	v.push_back(v3);
}

// ---------------------------------------------------------------------------
// tex_batch
// ---------------------------------------------------------------------------
void tex_batch::add(const vec2 &center, double half_size, const QRectF &uv, double alpha)
{
	const float a = static_cast<float>(alpha);
	const float x0 = static_cast<float>(center.x - half_size);
	const float x1 = static_cast<float>(center.x + half_size);
	const float y0 = static_cast<float>(center.y - half_size);
	const float y1 = static_cast<float>(center.y + half_size);
	// The image y axis goes down, the world y axis goes up.
	const float u0 = static_cast<float>(uv.left());
	const float u1 = static_cast<float>(uv.right());
	const float t0 = static_cast<float>(uv.top());
	const float t1 = static_cast<float>(uv.bottom());
	const tex_vertex bl{ x0, y0, u0, t1, a };
	const tex_vertex br{ x1, y0, u1, t1, a };
	const tex_vertex tr{ x1, y1, u1, t0, a };
	const tex_vertex tl{ x0, y1, u0, t0, a };
	v.push_back(bl);
	v.push_back(br);
	v.push_back(tr);
	v.push_back(bl);
	v.push_back(tr);
	v.push_back(tl);
}

void tex_batch::add_rect(const vec2 &lo, const vec2 &hi, const QRectF &uv, double alpha)
{
	const float a = static_cast<float>(alpha);
	const float x0 = static_cast<float>(lo.x);
	const float x1 = static_cast<float>(hi.x);
	const float y0 = static_cast<float>(lo.y);
	const float y1 = static_cast<float>(hi.y);
	const float u0 = static_cast<float>(uv.left());
	const float u1 = static_cast<float>(uv.right());
	const float t0 = static_cast<float>(uv.top());
	const float t1 = static_cast<float>(uv.bottom());
	const tex_vertex bl{ x0, y0, u0, t1, a };
	const tex_vertex br{ x1, y0, u1, t1, a };
	const tex_vertex tr{ x1, y1, u1, t0, a };
	const tex_vertex tl{ x0, y1, u0, t0, a };
	v.push_back(bl);
	v.push_back(br);
	v.push_back(tr);
	v.push_back(bl);
	v.push_back(tr);
	v.push_back(tl);
}
