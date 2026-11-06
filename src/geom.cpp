#include "geom.h"

#include <algorithm>
#include <limits>

double wrap_angle(double a)
{
	while (a <= -M_PI)
	{
		a += 2.0 * M_PI;
	}
	while (a > M_PI)
	{
		a -= 2.0 * M_PI;
	}
	return a;
}

// ---------------------------------------------------------------------------
// polyline
// ---------------------------------------------------------------------------
polyline::polyline(const std::vector<vec2> &points)
{
	set_points(points);
}

void polyline::set_points(const std::vector<vec2> &points)
{
	m_pts.clear();
	m_pts.reserve(points.size());
	// Remove points at the same position (zero-length segments).
	for (const vec2 &p : points)
	{
		if (m_pts.empty() || distance(m_pts.back(), p) > 1e-6)
		{
			m_pts.push_back(p);
		}
	}
	m_cum.assign(m_pts.size(), 0.0);
	for (size_t i = 1; i < m_pts.size(); ++i)
	{
		m_cum[i] = m_cum[i - 1] + distance(m_pts[i - 1], m_pts[i]);
	}
}

int polyline::segment_at(double s) const
{
	const int n = static_cast<int>(m_pts.size());
	if (n < 2)
	{
		return 0;
	}
	if (s <= 0.0)
	{
		return 0;
	}
	if (s >= m_cum.back())
	{
		return n - 2;
	}
	// first index with cum > s, minus one
	const auto it = std::upper_bound(m_cum.begin(), m_cum.end(), s);
	int i = static_cast<int>(it - m_cum.begin()) - 1;
	return std::max(0, std::min(i, n - 2));
}

void polyline::sample(double s, vec2 *point, vec2 *tangent) const
{
	const int n = static_cast<int>(m_pts.size());
	if (n == 0)
	{
		if (point) { *point = vec2(); }
		if (tangent) { *tangent = vec2(1.0, 0.0); }
		return;
	}
	if (n == 1)
	{
		if (point) { *point = m_pts[0]; }
		if (tangent) { *tangent = vec2(1.0, 0.0); }
		return;
	}
	const int i = segment_at(s);
	const vec2 a = m_pts[i];
	const vec2 b = m_pts[i + 1];
	const double seg = m_cum[i + 1] - m_cum[i];
	const vec2 dir = seg > 1e-12 ? (b - a) / seg : vec2(1.0, 0.0);
	if (point)
	{
		*point = a + dir * (s - m_cum[i]);
	}
	if (tangent)
	{
		*tangent = dir;
	}
}

vec2 polyline::point_at(double s) const
{
	vec2 p;
	sample(s, &p, nullptr);
	return p;
}

vec2 polyline::tangent_at(double s) const
{
	vec2 t;
	sample(s, nullptr, &t);
	return t;
}

double polyline::project(const vec2 &p, double *dist) const
{
	double best_d = std::numeric_limits<double>::max();
	double best_s = 0.0;
	for (size_t i = 0; i + 1 < m_pts.size(); ++i)
	{
		const vec2 a = m_pts[i];
		const vec2 ab = m_pts[i + 1] - a;
		const double l2 = ab.length_sq();
		double t = l2 > 1e-12 ? dot(p - a, ab) / l2 : 0.0;
		t = std::max(0.0, std::min(1.0, t));
		const double d = distance(p, a + ab * t);
		if (d < best_d)
		{
			best_d = d;
			best_s = m_cum[i] + t * (m_cum[i + 1] - m_cum[i]);
		}
	}
	if (m_pts.size() == 1)
	{
		best_d = distance(p, m_pts[0]);
	}
	if (dist)
	{
		*dist = best_d;
	}
	return best_s;
}

polyline polyline::offset(double d) const
{
	const size_t n = m_pts.size();
	if (n < 2 || std::abs(d) < 1e-9)
	{
		return *this;
	}
	std::vector<vec2> out;
	out.reserve(n);
	for (size_t i = 0; i < n; ++i)
	{
		vec2 normal;
		if (i == 0)
		{
			normal = (m_pts[1] - m_pts[0]).normalized().left();
		}
		else if (i == n - 1)
		{
			normal = (m_pts[n - 1] - m_pts[n - 2]).normalized().left();
		}
		else
		{
			const vec2 n0 = (m_pts[i] - m_pts[i - 1]).normalized().left();
			const vec2 n1 = (m_pts[i + 1] - m_pts[i]).normalized().left();
			vec2 m = (n0 + n1);
			const double ml = m.length();
			if (ml < 1e-6)
			{
				normal = n1;
			}
			else
			{
				m = m / ml;
				// Miter: 1 / cos(half angle). Limit it for sharp corners.
				const double c = std::max(0.35, dot(m, n1));
				normal = m / c;
			}
		}
		out.push_back(m_pts[i] + normal * d);
	}
	return polyline(out);
}

polyline polyline::sub(double s0, double s1) const
{
	if (m_pts.size() < 2)
	{
		return *this;
	}
	const double len = length();
	s0 = std::max(0.0, std::min(len, s0));
	s1 = std::max(0.0, std::min(len, s1));
	if (s1 - s0 < 1e-6)
	{
		const vec2 p = point_at(s0);
		const vec2 t = tangent_at(s0);
		return polyline(std::vector<vec2>{ p, p + t * 0.01 });
	}
	std::vector<vec2> out;
	out.push_back(point_at(s0));
	for (size_t i = 1; i + 1 < m_pts.size(); ++i)
	{
		if (m_cum[i] > s0 && m_cum[i] < s1)
		{
			out.push_back(m_pts[i]);
		}
	}
	out.push_back(point_at(s1));
	return polyline(out);
}

polyline polyline::reversed() const
{
	std::vector<vec2> r(m_pts.rbegin(), m_pts.rend());
	return polyline(r);
}

double polyline::min_radius() const
{
	double best = 1e9;
	for (size_t i = 1; i + 1 < m_pts.size(); ++i)
	{
		const vec2 a = m_pts[i - 1];
		const vec2 b = m_pts[i];
		const vec2 c = m_pts[i + 1];
		const double ab = distance(a, b);
		const double bc = distance(b, c);
		const double ca = distance(c, a);
		const double area2 = std::abs(cross(b - a, c - a));
		if (area2 < 1e-9)
		{
			continue;
		}
		const double r = (ab * bc * ca) / (2.0 * area2);
		best = std::min(best, r);
	}
	return best;
}

// ---------------------------------------------------------------------------
// geom
// ---------------------------------------------------------------------------
namespace geom
{
	std::vector<vec2> smooth_curve(const std::vector<vec2> &ctrl_in, double step)
	{
		// Remove duplicate control points
		std::vector<vec2> ctrl;
		for (const vec2 &p : ctrl_in)
		{
			if (ctrl.empty() || distance(ctrl.back(), p) > 0.05)
			{
				ctrl.push_back(p);
			}
		}
		std::vector<vec2> out;
		if (ctrl.size() < 2)
		{
			return ctrl;
		}
		step = std::max(0.2, step);
		if (ctrl.size() == 2)
		{
			const double len = distance(ctrl[0], ctrl[1]);
			const int n = std::max(1, static_cast<int>(std::ceil(len / std::max(step, len / 4.0))));
			for (int i = 0; i <= n; ++i)
			{
				out.push_back(lerp(ctrl[0], ctrl[1], static_cast<double>(i) / n));
			}
			return out;
		}

		const size_t n = ctrl.size();
		// Phantom points at the ends
		std::vector<vec2> p;
		p.reserve(n + 2);
		p.push_back(ctrl[0] * 2.0 - ctrl[1]);
		p.insert(p.end(), ctrl.begin(), ctrl.end());
		p.push_back(ctrl[n - 1] * 2.0 - ctrl[n - 2]);

		out.push_back(ctrl[0]);
		for (size_t i = 1; i + 2 < p.size(); ++i)
		{
			const vec2 p0 = p[i - 1];
			const vec2 p1 = p[i];
			const vec2 p2 = p[i + 1];
			const vec2 p3 = p[i + 2];
			// centripetal parameterization (alpha = 0.5)
			auto knot = [](const vec2 &a, const vec2 &b, double t) { return t + std::max(1e-4, std::sqrt(distance(a, b))); };
			const double t0 = 0.0;
			const double t1 = knot(p0, p1, t0);
			const double t2 = knot(p1, p2, t1);
			const double t3 = knot(p2, p3, t2);
			const double seg_len = distance(p1, p2);
			const int steps = std::max(2, static_cast<int>(std::ceil(seg_len / step)));
			for (int k = 1; k <= steps; ++k)
			{
				const double t = t1 + (t2 - t1) * static_cast<double>(k) / steps;
				const vec2 a1 = p0 * ((t1 - t) / (t1 - t0)) + p1 * ((t - t0) / (t1 - t0));
				const vec2 a2 = p1 * ((t2 - t) / (t2 - t1)) + p2 * ((t - t1) / (t2 - t1));
				const vec2 a3 = p2 * ((t3 - t) / (t3 - t2)) + p3 * ((t - t2) / (t3 - t2));
				const vec2 b1 = a1 * ((t2 - t) / (t2 - t0)) + a2 * ((t - t0) / (t2 - t0));
				const vec2 b2 = a2 * ((t3 - t) / (t3 - t1)) + a3 * ((t - t1) / (t3 - t1));
				out.push_back(b1 * ((t2 - t) / (t2 - t1)) + b2 * ((t - t1) / (t2 - t1)));
			}
		}
		out.back() = ctrl.back();
		return out;
	}

	std::vector<vec2> bezier(const vec2 &p0, const vec2 &p1, const vec2 &p2, const vec2 &p3, int n)
	{
		std::vector<vec2> out;
		n = std::max(1, n);
		out.reserve(n + 1);
		for (int i = 0; i <= n; ++i)
		{
			const double t = static_cast<double>(i) / n;
			const double u = 1.0 - t;
			out.push_back(p0 * (u * u * u) + p1 * (3.0 * u * u * t) + p2 * (3.0 * u * t * t) + p3 * (t * t * t));
		}
		return out;
	}

	bool segments_cross(const vec2 &a, const vec2 &b, const vec2 &c, const vec2 &d, vec2 *at)
	{
		const vec2 r = b - a;
		const vec2 s = d - c;
		const double den = cross(r, s);
		if (std::abs(den) < 1e-12)
		{
			return false;
		}
		const double t = cross(c - a, s) / den;
		const double u = cross(c - a, r) / den;
		if (t < 0.0 || t > 1.0 || u < 0.0 || u > 1.0)
		{
			return false;
		}
		if (at)
		{
			*at = a + r * t;
		}
		return true;
	}

	double point_segment_distance(const vec2 &p, const vec2 &a, const vec2 &b)
	{
		const vec2 ab = b - a;
		const double l2 = ab.length_sq();
		double t = l2 > 1e-12 ? dot(p - a, ab) / l2 : 0.0;
		t = std::max(0.0, std::min(1.0, t));
		return distance(p, a + ab * t);
	}

	double segment_segment_distance(const vec2 &a, const vec2 &b, const vec2 &c, const vec2 &d)
	{
		if (segments_cross(a, b, c, d))
		{
			return 0.0;
		}
		return std::min(std::min(point_segment_distance(a, c, d), point_segment_distance(b, c, d)),
			std::min(point_segment_distance(c, a, b), point_segment_distance(d, a, b)));
	}

	double polyline_distance(const polyline &a, const polyline &b, double *sa, double *sb)
	{
		const std::vector<vec2> &pa = a.points();
		const std::vector<vec2> &pb = b.points();
		double best = std::numeric_limits<double>::max();
		size_t bi = 0;
		size_t bj = 0;
		for (size_t i = 0; i + 1 < pa.size(); ++i)
		{
			for (size_t j = 0; j + 1 < pb.size(); ++j)
			{
				const double dd = segment_segment_distance(pa[i], pa[i + 1], pb[j], pb[j + 1]);
				if (dd < best)
				{
					best = dd;
					bi = i;
					bj = j;
				}
			}
		}
		if (sa && !a.cumulative().empty())
		{
			*sa = a.cumulative()[bi];
		}
		if (sb && !b.cumulative().empty())
		{
			*sb = b.cumulative()[bj];
		}
		return best;
	}

	std::vector<vec2> convex_hull(std::vector<vec2> pts)
	{
		if (pts.size() < 3)
		{
			return pts;
		}
		std::sort(pts.begin(), pts.end(), [](const vec2 &a, const vec2 &b) { return a.x < b.x || (a.x == b.x && a.y < b.y); });
		std::vector<vec2> h(2 * pts.size());
		size_t k = 0;
		for (size_t i = 0; i < pts.size(); ++i)
		{
			while (k >= 2 && cross(h[k - 1] - h[k - 2], pts[i] - h[k - 2]) <= 0)
			{
				--k;
			}
			h[k++] = pts[i];
		}
		for (size_t i = pts.size() - 1, t = k + 1; i > 0; --i)
		{
			while (k >= t && cross(h[k - 1] - h[k - 2], pts[i - 1] - h[k - 2]) <= 0)
			{
				--k;
			}
			h[k++] = pts[i - 1];
		}
		h.resize(k - 1);
		return h;
	}

	bool point_in_polygon(const vec2 &p, const std::vector<vec2> &poly)
	{
		bool inside = false;
		for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
		{
			if (((poly[i].y > p.y) != (poly[j].y > p.y))
				&& (p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x))
			{
				inside = !inside;
			}
		}
		return inside;
	}
}
