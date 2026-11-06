#ifndef GEOM_H
#define GEOM_H

#include <cmath>
#include <vector>

// geom
// ----
// 2D geometry of the world. Unit: meter. x points to the east, y to the north.
//
// vec2: a point or a vector.
// polyline: a line with points and the cumulative length at each point. It
// gives "the point at distance s", the direction at s, the projection of a
// point and parallel (offset) lines. The lanes and the connectors in a
// junction are polylines.

struct vec2
{
	double x = 0.0;
	double y = 0.0;

	vec2() = default;
	vec2(double ax, double ay) : x(ax), y(ay) {}

	vec2 operator+(const vec2 &o) const { return vec2(x + o.x, y + o.y); }
	vec2 operator-(const vec2 &o) const { return vec2(x - o.x, y - o.y); }
	vec2 operator*(double k) const { return vec2(x * k, y * k); }
	vec2 operator/(double k) const { return vec2(x / k, y / k); }
	vec2 operator-() const { return vec2(-x, -y); }
	vec2 &operator+=(const vec2 &o) { x += o.x; y += o.y; return *this; }
	vec2 &operator-=(const vec2 &o) { x -= o.x; y -= o.y; return *this; }
	vec2 &operator*=(double k) { x *= k; y *= k; return *this; }
	bool operator==(const vec2 &o) const { return x == o.x && y == o.y; }
	bool operator!=(const vec2 &o) const { return !(*this == o); }

	double length() const { return std::sqrt(x * x + y * y); }
	double length_sq() const { return x * x + y * y; }
	vec2 normalized() const
	{
		const double l = length();
		return l > 1e-12 ? vec2(x / l, y / l) : vec2(1.0, 0.0);
	}
	// Normal on the left side (90 degrees counterclockwise)
	vec2 left() const { return vec2(-y, x); }
	// Normal on the right side (90 degrees clockwise)
	vec2 right() const { return vec2(y, -x); }
	double angle() const { return std::atan2(y, x); }
};

inline double dot(const vec2 &a, const vec2 &b) { return a.x * b.x + a.y * b.y; }
inline double cross(const vec2 &a, const vec2 &b) { return a.x * b.y - a.y * b.x; }
inline double distance(const vec2 &a, const vec2 &b) { return (a - b).length(); }
inline vec2 lerp(const vec2 &a, const vec2 &b, double t) { return a + (b - a) * t; }
inline vec2 from_angle(double a) { return vec2(std::cos(a), std::sin(a)); }

// Angle in the range (-pi, pi]
double wrap_angle(double a);

class polyline
{
public:
	polyline() = default;
	explicit polyline(const std::vector<vec2> &points);

	void set_points(const std::vector<vec2> &points);
	const std::vector<vec2> &points() const { return m_pts; }
	const std::vector<double> &cumulative() const { return m_cum; }
	bool empty() const { return m_pts.size() < 2; }
	double length() const { return m_cum.empty() ? 0.0 : m_cum.back(); }
	vec2 front() const { return m_pts.empty() ? vec2() : m_pts.front(); }
	vec2 back() const { return m_pts.empty() ? vec2() : m_pts.back(); }

	// The point at distance s from the start. Outside [0, length], the line
	// continues in the direction of its first or last segment.
	vec2 point_at(double s) const;
	// The unit direction at distance s.
	vec2 tangent_at(double s) const;
	// Point and direction together.
	void sample(double s, vec2 *point, vec2 *tangent) const;
	// Distance s of the nearest point of the line to p. *dist: distance to the line.
	double project(const vec2 &p, double *dist = nullptr) const;
	// A parallel line at distance d (positive = left side).
	polyline offset(double d) const;
	// The part from s0 to s1 (s0 < s1).
	polyline sub(double s0, double s1) const;
	polyline reversed() const;
	// Smallest curve radius in meters. A straight line gives a large value.
	double min_radius() const;

private:
	int segment_at(double s) const;

	std::vector<vec2> m_pts;
	std::vector<double> m_cum;
};

namespace geom
{
	// A smooth curve through the control points (centripetal Catmull-Rom),
	// with a point every "step" meters. It starts at the first control point
	// and ends at the last one.
	std::vector<vec2> smooth_curve(const std::vector<vec2> &ctrl, double step);
	// Cubic Bezier curve with n segments (n + 1 points).
	std::vector<vec2> bezier(const vec2 &p0, const vec2 &p1, const vec2 &p2, const vec2 &p3, int n);
	// True if the segment [a,b] crosses the segment [c,d]. *at: the crossing point.
	bool segments_cross(const vec2 &a, const vec2 &b, const vec2 &c, const vec2 &d, vec2 *at = nullptr);
	double point_segment_distance(const vec2 &p, const vec2 &a, const vec2 &b);
	double segment_segment_distance(const vec2 &a, const vec2 &b, const vec2 &c, const vec2 &d);
	// Smallest distance between two polylines. *sa, *sb: the distance s on each line.
	double polyline_distance(const polyline &a, const polyline &b, double *sa = nullptr, double *sb = nullptr);
	// Convex hull, counterclockwise.
	std::vector<vec2> convex_hull(std::vector<vec2> pts);
	bool point_in_polygon(const vec2 &p, const std::vector<vec2> &poly);
	// Smooth step 0..1 (for animations).
	inline double smooth01(double t)
	{
		t = t < 0.0 ? 0.0 : (t > 1.0 ? 1.0 : t);
		return t * t * (3.0 - 2.0 * t);
	}
}

#endif // GEOM_H
