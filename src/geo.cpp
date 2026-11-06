#include "geo.h"

#include <algorithm>
#include <cmath>

namespace
{
	const double deg = M_PI / 180.0;
	const double max_lat = 85.05112878;   // limit of the Web Mercator square
}

const double geo_projection::earth_radius = 6378137.0;

geo_projection::geo_projection(double lat0, double lon0)
	: m_valid(true)
	, m_lat0(std::max(-max_lat, std::min(max_lat, lat0)))
	, m_lon0(lon0)
	, m_k(std::cos(m_lat0 * deg))
	, m_mx0(mercator_x(lon0))
	, m_my0(mercator_y(m_lat0))
{
}

double geo_projection::mercator_x(double lon)
{
	return earth_radius * lon * deg;
}

double geo_projection::mercator_y(double lat)
{
	lat = std::max(-max_lat, std::min(max_lat, lat));
	return earth_radius * std::log(std::tan(M_PI / 4.0 + lat * deg / 2.0));
}

vec2 geo_projection::to_world(double lat, double lon) const
{
	return mercator_to_world(mercator_x(lon), mercator_y(lat));
}

void geo_projection::to_geo(const vec2 &p, double *lat, double *lon) const
{
	double mx = 0.0;
	double my = 0.0;
	world_to_mercator(p, &mx, &my);
	if (lon)
	{
		*lon = mx / earth_radius / deg;
	}
	if (lat)
	{
		*lat = (2.0 * std::atan(std::exp(my / earth_radius)) - M_PI / 2.0) / deg;
	}
}

vec2 geo_projection::mercator_to_world(double mx, double my) const
{
	return vec2((mx - m_mx0) * m_k, (my - m_my0) * m_k);
}

void geo_projection::world_to_mercator(const vec2 &p, double *mx, double *my) const
{
	*mx = p.x / m_k + m_mx0;
	*my = p.y / m_k + m_my0;
}

double geo_projection::tile_size(int z)
{
	return 2.0 * M_PI * earth_radius / static_cast<double>(1 << z);
}

int geo_projection::tile_x(double mx, int z)
{
	const int n = 1 << z;
	const int x = static_cast<int>(std::floor((mx + M_PI * earth_radius) / tile_size(z)));
	return std::max(0, std::min(n - 1, x));
}

int geo_projection::tile_y(double my, int z)
{
	const int n = 1 << z;
	const int y = static_cast<int>(std::floor((M_PI * earth_radius - my) / tile_size(z)));
	return std::max(0, std::min(n - 1, y));
}

double geo_projection::tile_left(int x, int z)
{
	return -M_PI * earth_radius + x * tile_size(z);
}

double geo_projection::tile_top(int y, int z)
{
	return M_PI * earth_radius - y * tile_size(z);
}
