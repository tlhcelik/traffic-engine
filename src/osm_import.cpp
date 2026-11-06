#include "osm_import.h"
#include "geo.h"
#include "i18n.h"
#include "logger.h"
#include "world_model.h"
#include "xml_doc.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

namespace
{
	enum road_class
	{
		rc_none = 0,
		rc_service,
		rc_living,
		rc_residential,
		rc_unclassified,
		rc_tertiary,
		rc_secondary,
		rc_primary,
		rc_trunk,
		rc_motorway
	};

	enum node_flag
	{
		nf_signals = 1,
		nf_stop = 2,
		nf_give_way = 4,
		nf_level_crossing = 8,
		nf_bus_stop = 16,
		nf_border = 32
	};

	struct o_node
	{
		double lat = 0.0;
		double lon = 0.0;
		vec2 p;
		int flags = 0;
		QString name;
	};

	struct o_way
	{
		qint64 id = 0;
		int tags = 0;
		std::vector<qint64> refs;
		road_class cls = rc_none;
		bool link = false;
		bool roundabout = false;
		int oneway = 0;          // 1: in the way direction, -1: against it, 0: two-way
		int lanes = -1;
		int lanes_fwd = -1;
		int lanes_bwd = -1;
		double maxspeed = -1.0;
		QString name;
		int sidewalk = -1;       // bit 0: left, bit 1: right; -1: no tag
		bool cycleway = false;
		int overtaking = -1;
		int level = 0;           // bridge, tunnel, layer: 1 bridge, -1 tunnel
	};

	struct o_seg
	{
		int way = -1;
		std::vector<int> nodes;
		double length = 0.0;
	};

	struct o_cluster
	{
		vec2 pos;
		int flags = 0;
		double radius = 0.0;
		int roads = 0;
		int model_node = -1;
	};

	struct o_road
	{
		int ca = -1;
		int cb = -1;
		std::vector<vec2> pts;
		std::vector<int> osm_nodes;   // nodes of the OSM geometry, in the road direction
		road_class cls = rc_none;
		bool link = false;
		bool roundabout = false;
		int fwd = 1;
		int bwd = 1;
		double speed = 50.0;
		double lane_width = 3.25;
		QString name;
		bool sw_left = true;
		bool sw_right = true;
		bool bike = false;
		bool overtaking = true;
		bool median = false;
		int level = 0;
		int way = -1;                 // index in m_ways
		bool alive = true;
		int model_road = -1;
	};

	// ------------------------------------------------------------------
	// tags
	// ------------------------------------------------------------------
	road_class class_of(QString hw, bool *link)
	{
		*link = false;
		if (hw.endsWith(QLatin1String("_link")))
		{
			hw.chop(5);
			*link = true;
		}
		if (hw == QLatin1String("motorway")) { return rc_motorway; }
		if (hw == QLatin1String("trunk")) { return rc_trunk; }
		if (hw == QLatin1String("primary")) { return rc_primary; }
		if (hw == QLatin1String("secondary")) { return rc_secondary; }
		if (hw == QLatin1String("tertiary")) { return rc_tertiary; }
		if (*link)
		{
			return rc_none;
		}
		if (hw == QLatin1String("unclassified") || hw == QLatin1String("road")) { return rc_unclassified; }
		if (hw == QLatin1String("residential")) { return rc_residential; }
		if (hw == QLatin1String("living_street")) { return rc_living; }
		if (hw == QLatin1String("service")) { return rc_service; }
		return rc_none;
	}

	double parse_speed(const QString &value)
	{
		const QString s = value.trimmed().toLower();
		if (s.isEmpty()) { return -1.0; }
		if (s == QLatin1String("tr:urban")) { return 50.0; }
		if (s == QLatin1String("tr:rural")) { return 90.0; }
		if (s == QLatin1String("tr:trunk")) { return 110.0; }
		if (s == QLatin1String("tr:motorway")) { return 120.0; }
		if (s == QLatin1String("walk")) { return 10.0; }
		if (s == QLatin1String("none")) { return 130.0; }
		static const QRegularExpression re(QStringLiteral("^(\\d+(?:\\.\\d+)?)\\s*(mph)?"));
		const QRegularExpressionMatch m = re.match(s);
		if (!m.hasMatch())
		{
			return -1.0;
		}
		double v = m.captured(1).toDouble();
		if (!m.captured(2).isEmpty())
		{
			v *= 1.609;
		}
		return (v >= 5.0 && v <= 200.0) ? v : -1.0;
	}

	int parse_count(const QString &value)
	{
		bool ok = false;
		const int v = value.section(QLatin1Char(';'), 0, 0).trimmed().toInt(&ok);
		return (ok && v > 0) ? v : -1;
	}

	bool is_yes(const QString &v)
	{
		return v == QLatin1String("yes") || v == QLatin1String("true") || v == QLatin1String("1");
	}

	bool access_denied(const QString &v)
	{
		return v == QLatin1String("no") || v == QLatin1String("private") || v == QLatin1String("agricultural")
			|| v == QLatin1String("forestry") || v == QLatin1String("emergency");
	}

	// Sidewalk value -> bits (left 1, right 2). "separate": a sidewalk is
	// there (it is a separate way in OSM).
	int sidewalk_bits(const QString &v)
	{
		if (v == QLatin1String("both") || v == QLatin1String("yes") || v == QLatin1String("separate")) { return 3; }
		if (v == QLatin1String("left")) { return 1; }
		if (v == QLatin1String("right")) { return 2; }
		return 0;
	}

	bool side_yes(const QString &v)
	{
		return v == QLatin1String("yes") || v == QLatin1String("separate") || v == QLatin1String("both");
	}

	bool bike_lane_value(const QString &v)
	{
		return v == QLatin1String("lane") || v == QLatin1String("track") || v == QLatin1String("opposite_lane")
			|| v == QLatin1String("opposite_track");
	}

	int default_lanes(road_class c, bool oneway)
	{
		switch (c)
		{
			case rc_motorway:
			case rc_trunk:
				return 2;
			case rc_primary:
				return oneway ? 2 : 1;
			default:
				return 1;
		}
	}

	double default_speed(road_class c, bool link)
	{
		switch (c)
		{
			case rc_motorway: return link ? 70.0 : 120.0;
			case rc_trunk: return link ? 50.0 : 90.0;
			case rc_living: return 20.0;
			case rc_service: return 20.0;
			default: return 50.0;
		}
	}

	double default_lane_width(road_class c)
	{
		switch (c)
		{
			case rc_motorway:
			case rc_trunk:
			case rc_primary:
				return 3.5;
			case rc_secondary:
			case rc_tertiary:
				return 3.25;
			default:
				return 3.0;
		}
	}

	// Vehicles per hour and per lane of a traffic source at a dead end (peak hour).
	double source_rate(road_class c)
	{
		switch (c)
		{
			case rc_motorway: return 800.0;
			case rc_trunk: return 480.0;
			case rc_primary: return 280.0;
			case rc_secondary: return 200.0;
			case rc_tertiary: return 150.0;
			case rc_unclassified: return 60.0;
			case rc_residential: return 30.0;
			default: return 10.0;
		}
	}

	// Priority of a road at a junction without lights (higher = main road).
	int priority_rank(const o_road &r)
	{
		if (r.roundabout)
		{
			return 100;
		}
		return static_cast<int>(r.cls) * 2 - (r.link ? 1 : 0);
	}

	// ------------------------------------------------------------------
	// geometry helpers
	// ------------------------------------------------------------------
	double line_length(const std::vector<vec2> &pts)
	{
		double l = 0.0;
		for (size_t i = 1; i < pts.size(); ++i)
		{
			l += distance(pts[i - 1], pts[i]);
		}
		return l;
	}

	std::vector<vec2> simplify(const std::vector<vec2> &in, double tol)
	{
		if (in.size() <= 2)
		{
			return in;
		}
		std::vector<char> keep(in.size(), 0);
		keep.front() = 1;
		keep.back() = 1;
		std::vector<std::pair<size_t, size_t>> stack;
		stack.push_back(std::make_pair(size_t(0), in.size() - 1));
		while (!stack.empty())
		{
			const std::pair<size_t, size_t> r = stack.back();
			stack.pop_back();
			double dmax = 0.0;
			size_t idx = r.first;
			for (size_t i = r.first + 1; i < r.second; ++i)
			{
				const double d = geom::point_segment_distance(in[i], in[r.first], in[r.second]);
				if (d > dmax)
				{
					dmax = d;
					idx = i;
				}
			}
			if (dmax > tol)
			{
				keep[idx] = 1;
				stack.push_back(std::make_pair(r.first, idx));
				stack.push_back(std::make_pair(idx, r.second));
			}
		}
		std::vector<vec2> out;
		for (size_t i = 0; i < in.size(); ++i)
		{
			if (keep[i])
			{
				out.push_back(in[i]);
			}
		}
		return out;
	}

	// Liang-Barsky: part [t0, t1] of the segment a-b in the rectangle.
	bool clip_segment(const vec2 &a, const vec2 &b, const vec2 &lo, const vec2 &hi, double *t0, double *t1)
	{
		double u0 = 0.0;
		double u1 = 1.0;
		const double dx = b.x - a.x;
		const double dy = b.y - a.y;
		const double p[4] = { -dx, dx, -dy, dy };
		const double q[4] = { a.x - lo.x, hi.x - a.x, a.y - lo.y, hi.y - a.y };
		for (int i = 0; i < 4; ++i)
		{
			if (std::abs(p[i]) < 1e-12)
			{
				if (q[i] < 0.0)
				{
					return false;
				}
				continue;
			}
			const double r = q[i] / p[i];
			if (p[i] < 0.0)
			{
				if (r > u1) { return false; }
				u0 = std::max(u0, r);
			}
			else
			{
				if (r < u0) { return false; }
				u1 = std::min(u1, r);
			}
		}
		*t0 = u0;
		*t1 = u1;
		return true;
	}

	void reverse_road(o_road &r)
	{
		std::reverse(r.pts.begin(), r.pts.end());
		std::reverse(r.osm_nodes.begin(), r.osm_nodes.end());
		std::swap(r.ca, r.cb);
		std::swap(r.fwd, r.bwd);
		std::swap(r.sw_left, r.sw_right);
	}

	bool same_attributes(const o_road &x, const o_road &y)
	{
		return x.fwd == y.fwd && x.bwd == y.bwd && std::abs(x.speed - y.speed) < 1.0 && x.name == y.name
			&& x.sw_left == y.sw_left && x.sw_right == y.sw_right && x.bike == y.bike && x.overtaking == y.overtaking
			&& x.median == y.median && x.cls == y.cls && x.link == y.link && x.roundabout == y.roundabout
			&& std::abs(x.lane_width - y.lane_width) < 0.01 && x.level == y.level;
	}

	struct union_find
	{
		std::vector<int> parent;
		explicit union_find(size_t n) : parent(n)
		{
			for (size_t i = 0; i < n; ++i)
			{
				parent[i] = static_cast<int>(i);
			}
		}
		int find(int x)
		{
			while (parent[x] != x)
			{
				parent[x] = parent[parent[x]];
				x = parent[x];
			}
			return x;
		}
	};

	// ------------------------------------------------------------------
	// converter
	// ------------------------------------------------------------------
	class converter
	{
	public:
		converter(const osm_import_options &opt, osm_import_result &res) : m_opt(opt), m_res(res) {}

		bool parse(const QByteArray &xml, QString *error);
		bool project(const osm_bounds &bounds, QString *error);
		void make_segments();
		void make_clusters();
		void make_roads();
		void remove_duplicates();
		void pair_carriageways();
		void merge_chains();
		void build_model(world_model &m);

	private:
		int border_node(const vec2 &p);
		void clip_run(int way, const std::vector<int> &run, std::vector<std::pair<int, std::vector<int>>> &parts);
		void add_sign(world_model &m, const o_road &r, int dir, const QString &key);

		const osm_import_options &m_opt;
		osm_import_result &m_res;
		std::vector<o_node> m_nodes;
		std::unordered_map<qint64, int> m_node_index;
		std::vector<o_way> m_ways;
		osm_bounds m_data_bounds;
		QString m_remark;
		geo_projection m_proj;
		bool m_clip = false;
		vec2 m_lo;
		vec2 m_hi;
		std::vector<o_seg> m_segs;
		std::vector<int> m_cluster_of;    // node -> cluster (-1: no segment end)
		std::vector<o_cluster> m_clusters;
		std::vector<o_road> m_roads;
	};

	bool converter::parse(const QByteArray &xml, QString *error)
	{
		xml_document doc;
		if (!doc.load_from_memory(xml, QStringLiteral("OpenStreetMap data"), error))
		{
			return false;
		}
		const xml_element root = doc.root();
		if (root.is_null() || root.name() != QLatin1String("osm"))
		{
			*error = QStringLiteral("The data is not OpenStreetMap XML (the root element <osm> is missing).");
			return false;
		}
		for (xml_element e = root.first_child(); !e.is_null(); e = e.next_sibling())
		{
			const QString kind = e.name();
			if (kind == QLatin1String("node"))
			{
				++m_res.osm_nodes;
				o_node n;
				n.lat = e.attribute_double(QStringLiteral("lat"));
				n.lon = e.attribute_double(QStringLiteral("lon"));
				bool platform = false;
				bool bus = false;
				for (xml_element t = e.first_child(QStringLiteral("tag")); !t.is_null(); t = t.next_sibling(QStringLiteral("tag")))
				{
					const QString k = t.attribute(QStringLiteral("k"));
					const QString v = t.attribute(QStringLiteral("v"));
					if (k == QLatin1String("highway"))
					{
						if (v == QLatin1String("traffic_signals")) { n.flags |= nf_signals; }
						else if (v == QLatin1String("stop")) { n.flags |= nf_stop; }
						else if (v == QLatin1String("give_way")) { n.flags |= nf_give_way; }
						else if (v == QLatin1String("bus_stop")) { n.flags |= nf_bus_stop; }
					}
					else if (k == QLatin1String("railway") && v == QLatin1String("level_crossing")) { n.flags |= nf_level_crossing; }
					else if (k == QLatin1String("public_transport") && v == QLatin1String("platform")) { platform = true; }
					else if (k == QLatin1String("bus") && is_yes(v)) { bus = true; }
					else if (k == QLatin1String("name")) { n.name = v; }
				}
				if (platform && bus)
				{
					n.flags |= nf_bus_stop;
				}
				m_node_index[e.attribute(QStringLiteral("id")).toLongLong()] = static_cast<int>(m_nodes.size());
				m_nodes.push_back(n);
			}
			else if (kind == QLatin1String("way"))
			{
				++m_res.osm_ways;
				o_way w;
				w.id = e.attribute(QStringLiteral("id")).toLongLong();
				QString hw;
				QString access;
				QString motor;
				QString motorcar;
				QString vehicle;
				QString service;
				QString area;
				QString junction;
				QString oneway;
				QString sw;
				QString sw_both;
				QString sw_left;
				QString sw_right;
				QString bridge;
				QString tunnel;
				QString layer;
				for (xml_element t = e.first_child(QStringLiteral("tag")); !t.is_null(); t = t.next_sibling(QStringLiteral("tag")))
				{
					const QString k = t.attribute(QStringLiteral("k"));
					const QString v = t.attribute(QStringLiteral("v"));
					++w.tags;
					if (k == QLatin1String("highway")) { hw = v; }
					else if (k == QLatin1String("name")) { w.name = v; }
					else if (k == QLatin1String("oneway")) { oneway = v; }
					else if (k == QLatin1String("lanes")) { w.lanes = parse_count(v); }
					else if (k == QLatin1String("lanes:forward")) { w.lanes_fwd = parse_count(v); }
					else if (k == QLatin1String("lanes:backward")) { w.lanes_bwd = parse_count(v); }
					else if (k == QLatin1String("maxspeed")) { w.maxspeed = parse_speed(v); }
					else if (k == QLatin1String("sidewalk")) { sw = v; }
					else if (k == QLatin1String("sidewalk:both")) { sw_both = v; }
					else if (k == QLatin1String("sidewalk:left")) { sw_left = v; }
					else if (k == QLatin1String("sidewalk:right")) { sw_right = v; }
					else if (k.startsWith(QLatin1String("cycleway")) && !k.contains(QLatin1Char(':') + QStringLiteral("width")))
					{
						w.cycleway = w.cycleway || bike_lane_value(v);
					}
					else if (k == QLatin1String("overtaking")) { w.overtaking = (v == QLatin1String("no")) ? 0 : 1; }
					else if (k == QLatin1String("bridge")) { bridge = v; }
					else if (k == QLatin1String("tunnel")) { tunnel = v; }
					else if (k == QLatin1String("layer")) { layer = v; }
					else if (k == QLatin1String("junction")) { junction = v; }
					else if (k == QLatin1String("access")) { access = v; }
					else if (k == QLatin1String("motor_vehicle")) { motor = v; }
					else if (k == QLatin1String("motorcar")) { motorcar = v; }
					else if (k == QLatin1String("vehicle")) { vehicle = v; }
					else if (k == QLatin1String("service")) { service = v; }
					else if (k == QLatin1String("area")) { area = v; }
				}
				w.cls = class_of(hw, &w.link);
				if (w.cls == rc_none || is_yes(area))
				{
					continue;
				}
				const QString eff = !motorcar.isEmpty() ? motorcar : (!motor.isEmpty() ? motor : (!vehicle.isEmpty() ? vehicle : access));
				if (access_denied(eff))
				{
					continue;
				}
				if (w.cls == rc_service && (!m_opt.service_roads || service == QLatin1String("parking_aisle")
					|| service == QLatin1String("drive-through") || service == QLatin1String("emergency_access")))
				{
					continue;
				}
				if (!m_opt.residential && (w.cls == rc_residential || w.cls == rc_living || w.cls == rc_unclassified))
				{
					continue;
				}
				w.roundabout = (junction == QLatin1String("roundabout") || junction == QLatin1String("circular"));
				// Level: the layer tag, else 1 for a bridge and -1 for a tunnel
				bool layer_ok = false;
				const int layer_value = layer.toInt(&layer_ok);
				const bool is_bridge = !bridge.isEmpty() && bridge != QLatin1String("no");
				const bool is_tunnel = !tunnel.isEmpty() && tunnel != QLatin1String("no") && tunnel != QLatin1String("building_passage");
				if (layer_ok && layer_value != 0)
				{
					w.level = std::max(-2, std::min(2, layer_value));
				}
				else if (is_bridge)
				{
					w.level = 1;
				}
				else if (is_tunnel)
				{
					w.level = -1;
				}
				if (is_yes(oneway)) { w.oneway = 1; }
				else if (oneway == QLatin1String("-1") || oneway == QLatin1String("reverse")) { w.oneway = -1; }
				else if (oneway != QLatin1String("no") && ((w.cls == rc_motorway && !w.link) || w.roundabout)) { w.oneway = 1; }
				if (!sw.isEmpty()) { w.sidewalk = sidewalk_bits(sw); }
				if (!sw_both.isEmpty()) { w.sidewalk = side_yes(sw_both) ? 3 : 0; }
				if (!sw_left.isEmpty() || !sw_right.isEmpty())
				{
					int bits = w.sidewalk < 0 ? 0 : w.sidewalk;
					if (!sw_left.isEmpty()) { bits = side_yes(sw_left) ? (bits | 1) : (bits & ~1); }
					if (!sw_right.isEmpty()) { bits = side_yes(sw_right) ? (bits | 2) : (bits & ~2); }
					w.sidewalk = bits;
				}
				for (xml_element nd = e.first_child(QStringLiteral("nd")); !nd.is_null(); nd = nd.next_sibling(QStringLiteral("nd")))
				{
					w.refs.push_back(nd.attribute(QStringLiteral("ref")).toLongLong());
				}
				if (w.refs.size() >= 2)
				{
					m_ways.push_back(w);
				}
			}
			else if (kind == QLatin1String("bounds"))
			{
				m_data_bounds.min_lat = e.attribute_double(QStringLiteral("minlat"));
				m_data_bounds.min_lon = e.attribute_double(QStringLiteral("minlon"));
				m_data_bounds.max_lat = e.attribute_double(QStringLiteral("maxlat"));
				m_data_bounds.max_lon = e.attribute_double(QStringLiteral("maxlon"));
			}
			else if (kind == QLatin1String("remark"))
			{
				m_remark = e.text().trimmed();
			}
		}
		m_res.used_ways = static_cast<int>(m_ways.size());
		if (m_ways.empty())
		{
			if (!m_remark.isEmpty())
			{
				*error = QStringLiteral("The server sent a remark and no roads: %1").arg(m_remark);
			}
			else
			{
				*error = QStringLiteral("The data has no roads for vehicles (%1 nodes, %2 ways).").arg(m_res.osm_nodes).arg(m_res.osm_ways);
			}
			return false;
		}
		if (!m_remark.isEmpty())
		{
			m_res.notes << QStringLiteral("Server remark: %1").arg(m_remark);
		}
		return true;
	}

	bool converter::project(const osm_bounds &bounds, QString *error)
	{
		osm_bounds b = bounds.valid() ? bounds : m_data_bounds;
		m_clip = b.valid();
		if (!m_clip)
		{
			// No area: the extent of the road nodes.
			bool first = true;
			for (const o_way &w : m_ways)
			{
				for (qint64 ref : w.refs)
				{
					const auto it = m_node_index.find(ref);
					if (it == m_node_index.end())
					{
						continue;
					}
					const o_node &n = m_nodes[it->second];
					if (first)
					{
						b.min_lat = b.max_lat = n.lat;
						b.min_lon = b.max_lon = n.lon;
						first = false;
					}
					b.min_lat = std::min(b.min_lat, n.lat);
					b.max_lat = std::max(b.max_lat, n.lat);
					b.min_lon = std::min(b.min_lon, n.lon);
					b.max_lon = std::max(b.max_lon, n.lon);
				}
			}
			if (first)
			{
				*error = QStringLiteral("The ways have no nodes in the data.");
				return false;
			}
		}
		m_proj = geo_projection(b.center_lat(), b.center_lon());
		for (o_node &n : m_nodes)
		{
			n.p = m_proj.to_world(n.lat, n.lon);
		}
		m_lo = m_proj.to_world(b.min_lat, b.min_lon);
		m_hi = m_proj.to_world(b.max_lat, b.max_lon);
		if (m_clip)
		{
			m_res.notes << QStringLiteral("Area: %1 x %2 m around %3, %4").arg(m_hi.x - m_lo.x, 0, 'f', 0).arg(m_hi.y - m_lo.y, 0, 'f', 0)
				.arg(b.center_lat(), 0, 'f', 6).arg(b.center_lon(), 0, 'f', 6);
		}
		return true;
	}

	int converter::border_node(const vec2 &p)
	{
		o_node n;
		n.p = p;
		n.flags = nf_border;
		m_proj.to_geo(p, &n.lat, &n.lon);
		m_nodes.push_back(n);
		return static_cast<int>(m_nodes.size()) - 1;
	}

	void converter::clip_run(int way, const std::vector<int> &run, std::vector<std::pair<int, std::vector<int>>> &parts)
	{
		if (!m_clip)
		{
			parts.push_back(std::make_pair(way, run));
			return;
		}
		std::vector<int> cur;
		auto close = [&]()
		{
			if (cur.size() >= 2)
			{
				parts.push_back(std::make_pair(way, cur));
			}
			cur.clear();
		};
		for (size_t i = 0; i + 1 < run.size(); ++i)
		{
			const vec2 a = m_nodes[run[i]].p;
			const vec2 b = m_nodes[run[i + 1]].p;
			double t0 = 0.0;
			double t1 = 1.0;
			if (!clip_segment(a, b, m_lo, m_hi, &t0, &t1))
			{
				close();
				continue;
			}
			if (cur.empty())
			{
				cur.push_back(t0 > 1e-9 ? border_node(lerp(a, b, t0)) : run[i]);
			}
			if (t1 < 1.0 - 1e-9)
			{
				cur.push_back(border_node(lerp(a, b, t1)));
				close();
			}
			else
			{
				cur.push_back(run[i + 1]);
			}
		}
		close();
	}

	void converter::make_segments()
	{
		// 1) parts of the ways in the area (node indices)
		std::vector<std::pair<int, std::vector<int>>> parts;
		for (size_t wi = 0; wi < m_ways.size(); ++wi)
		{
			std::vector<int> run;
			for (qint64 ref : m_ways[wi].refs)
			{
				const auto it = m_node_index.find(ref);
				if (it == m_node_index.end())
				{
					// A node is not in the data: the way stops here.
					if (run.size() >= 2)
					{
						clip_run(static_cast<int>(wi), run, parts);
					}
					run.clear();
					continue;
				}
				if (run.empty() || run.back() != it->second)
				{
					run.push_back(it->second);
				}
			}
			if (run.size() >= 2)
			{
				clip_run(static_cast<int>(wi), run, parts);
			}
		}

		// 2) split points: shared nodes and the ends of the parts
		std::vector<int> uses(m_nodes.size(), 0);
		std::vector<char> split(m_nodes.size(), 0);
		for (const auto &part : parts)
		{
			for (int n : part.second)
			{
				++uses[n];
			}
			split[part.second.front()] = 1;
			split[part.second.back()] = 1;
		}
		for (size_t i = 0; i < m_nodes.size(); ++i)
		{
			if (uses[i] >= 2)
			{
				split[i] = 1;
			}
		}

		// 3) segments between split points; a closed loop is split in the middle
		for (const auto &part : parts)
		{
			const std::vector<int> &ns = part.second;
			size_t start = 0;
			for (size_t i = 1; i < ns.size(); ++i)
			{
				if (!split[ns[i]] && i + 1 < ns.size())
				{
					continue;
				}
				o_seg s;
				s.way = part.first;
				s.nodes.assign(ns.begin() + static_cast<std::ptrdiff_t>(start), ns.begin() + static_cast<std::ptrdiff_t>(i) + 1);
				start = i;
				if (s.nodes.front() == s.nodes.back())
				{
					if (s.nodes.size() < 3)
					{
						continue;
					}
					const size_t mid = s.nodes.size() / 2;
					o_seg s2;
					s2.way = s.way;
					s2.nodes.assign(s.nodes.begin() + static_cast<std::ptrdiff_t>(mid), s.nodes.end());
					s.nodes.resize(mid + 1);
					m_segs.push_back(s);
					m_segs.push_back(s2);
					continue;
				}
				m_segs.push_back(s);
			}
		}
		for (o_seg &s : m_segs)
		{
			for (size_t i = 1; i < s.nodes.size(); ++i)
			{
				s.length += distance(m_nodes[s.nodes[i - 1]].p, m_nodes[s.nodes[i]].p);
			}
		}
	}

	void converter::make_clusters()
	{
		std::vector<int> deg(m_nodes.size(), 0);
		for (const o_seg &s : m_segs)
		{
			++deg[s.nodes.front()];
			++deg[s.nodes.back()];
		}
		union_find uf(m_nodes.size());
		std::vector<vec2> lo(m_nodes.size());
		std::vector<vec2> hi(m_nodes.size());
		for (size_t i = 0; i < m_nodes.size(); ++i)
		{
			lo[i] = m_nodes[i].p;
			hi[i] = m_nodes[i].p;
		}
		std::vector<size_t> order(m_segs.size());
		for (size_t i = 0; i < order.size(); ++i)
		{
			order[i] = i;
		}
		std::sort(order.begin(), order.end(), [this](size_t a, size_t b) { return m_segs[a].length < m_segs[b].length; });
		// Signalized ends: the node or a node of the segment in 30 m has traffic
		// signals. Two signalized junction parts in 45 m are one junction with
		// one controller (for example two dual carriageways that cross).
		std::vector<char> sig(m_nodes.size(), 0);
		for (const o_seg &s : m_segs)
		{
			for (int end = 0; end < 2; ++end)
			{
				double d = 0.0;
				for (size_t i = 0; i < s.nodes.size() && d < 30.0; ++i)
				{
					const size_t k = end == 0 ? i : s.nodes.size() - 1 - i;
					if (i > 0)
					{
						const size_t prev = end == 0 ? k - 1 : k + 1;
						d += distance(m_nodes[s.nodes[prev]].p, m_nodes[s.nodes[k]].p);
					}
					if (d < 30.0 && (m_nodes[s.nodes[k]].flags & nf_signals))
					{
						sig[end == 0 ? s.nodes.front() : s.nodes.back()] = 1;
						break;
					}
				}
			}
		}
		const double join = std::max(0.0, m_opt.join_distance);
		const double max_size = std::max(12.0, join * 2.5);
		const double always = std::min(join, 6.0);
		const double sig_join = 45.0;
		const double sig_max_size = 75.0;
		for (size_t k : order)
		{
			const o_seg &s = m_segs[k];
			if (s.length >= std::max(join, sig_join))
			{
				break;
			}
			const int a = s.nodes.front();
			const int b = s.nodes.back();
			// Keep the dead ends at the border (traffic sources) and real dead-end roads.
			if ((m_nodes[a].flags | m_nodes[b].flags) & nf_border)
			{
				continue;
			}
			if ((deg[a] == 1 || deg[b] == 1) && s.length >= 5.0)
			{
				continue;
			}
			const bool signal_pair = sig[a] && sig[b] && deg[a] >= 3 && deg[b] >= 3;
			if (s.length >= join && !signal_pair)
			{
				continue;
			}
			const int ra = uf.find(a);
			const int rb = uf.find(b);
			if (ra == rb)
			{
				continue;
			}
			const vec2 l(std::min(lo[ra].x, lo[rb].x), std::min(lo[ra].y, lo[rb].y));
			const vec2 h(std::max(hi[ra].x, hi[rb].x), std::max(hi[ra].y, hi[rb].y));
			if (s.length >= always && distance(l, h) > (signal_pair ? sig_max_size : max_size))
			{
				continue; // the group would be too large (a chain of junctions)
			}
			uf.parent[rb] = ra;
			lo[ra] = l;
			hi[ra] = h;
		}

		// Boxes: four junctions in a cycle of short segments (two dual
		// carriageways that cross) become one junction.
		const double box_len = 35.0;
		const double box_max_size = 60.0;
		std::map<int, std::vector<int>> adj;
		for (const o_seg &s : m_segs)
		{
			const int a = s.nodes.front();
			const int b = s.nodes.back();
			if (a != b && s.length < box_len && deg[a] >= 3 && deg[b] >= 3)
			{
				adj[a].push_back(b);
				adj[b].push_back(a);
			}
		}
		int boxes = 0;
		for (const auto &kv : adj)
		{
			const int a = kv.first;
			const std::vector<int> &na = kv.second;
			for (size_t i = 0; i < na.size(); ++i)
			{
				for (size_t j = i + 1; j < na.size(); ++j)
				{
					const int b = na[i];
					const int d = na[j];
					if (b == d)
					{
						continue;
					}
					// A node c (not a) next to b and to d
					const std::vector<int> &nd = adj.at(d);
					for (int c : adj.at(b))
					{
						if (c == a || c == d || std::find(nd.begin(), nd.end(), c) == nd.end())
						{
							continue;
						}
						vec2 l = m_nodes[a].p;
						vec2 h = m_nodes[a].p;
						for (int n : { b, c, d })
						{
							l = vec2(std::min(l.x, m_nodes[n].p.x), std::min(l.y, m_nodes[n].p.y));
							h = vec2(std::max(h.x, m_nodes[n].p.x), std::max(h.y, m_nodes[n].p.y));
						}
						if (distance(l, h) > box_max_size)
						{
							continue;
						}
						bool merged = false;
						for (int n : { b, c, d })
						{
							const int ra = uf.find(a);
							const int rn = uf.find(n);
							if (ra != rn)
							{
								uf.parent[rn] = ra;
								merged = true;
							}
						}
						boxes += merged ? 1 : 0;
					}
				}
			}
		}
		if (boxes > 0)
		{
			m_res.notes << QStringLiteral("Junction boxes joined (crossings of dual carriageways): %1").arg(boxes);
		}

		m_cluster_of.assign(m_nodes.size(), -1);
		std::map<int, int> root_cluster;
		std::vector<std::vector<int>> members;
		for (size_t i = 0; i < m_nodes.size(); ++i)
		{
			if (deg[i] == 0)
			{
				continue;
			}
			const int r = uf.find(static_cast<int>(i));
			auto it = root_cluster.find(r);
			if (it == root_cluster.end())
			{
				it = root_cluster.insert(std::make_pair(r, static_cast<int>(m_clusters.size()))).first;
				m_clusters.push_back(o_cluster());
				members.push_back(std::vector<int>());
			}
			m_cluster_of[i] = it->second;
			members[it->second].push_back(static_cast<int>(i));
		}
		for (size_t c = 0; c < m_clusters.size(); ++c)
		{
			o_cluster &cl = m_clusters[c];
			vec2 sum;
			for (int n : members[c])
			{
				sum += m_nodes[n].p;
				cl.flags |= m_nodes[n].flags;
			}
			cl.pos = sum / static_cast<double>(members[c].size());
			for (int n : members[c])
			{
				cl.radius = std::max(cl.radius, distance(cl.pos, m_nodes[n].p));
			}
		}
	}

	void converter::make_roads()
	{
		for (const o_seg &s : m_segs)
		{
			int ca = m_cluster_of[s.nodes.front()];
			int cb = m_cluster_of[s.nodes.back()];
			if (ca < 0 || cb < 0)
			{
				continue;
			}
			if (ca == cb)
			{
				// A part inside a junction: its signals and signs belong to the junction.
				for (int n : s.nodes)
				{
					m_clusters[ca].flags |= (m_nodes[n].flags & (nf_signals | nf_stop | nf_give_way));
				}
				continue;
			}
			const o_way &w = m_ways[s.way];
			o_road r;
			r.way = s.way;
			r.osm_nodes = s.nodes;
			const bool reversed = (w.oneway < 0);
			if (reversed)
			{
				std::reverse(r.osm_nodes.begin(), r.osm_nodes.end());
				std::swap(ca, cb);
			}
			r.ca = ca;
			r.cb = cb;
			const o_cluster &ka = m_clusters[ca];
			const o_cluster &kb = m_clusters[cb];
			std::vector<vec2> pts;
			pts.push_back(ka.pos);
			for (size_t i = 1; i + 1 < r.osm_nodes.size(); ++i)
			{
				const vec2 p = m_nodes[r.osm_nodes[i]].p;
				if (distance(p, ka.pos) <= ka.radius + 2.0 || distance(p, kb.pos) <= kb.radius + 2.0)
				{
					continue;
				}
				pts.push_back(p);
			}
			pts.push_back(kb.pos);
			r.pts = simplify(pts, 0.8);
			if (line_length(r.pts) < 1.5)
			{
				continue;
			}

			r.cls = w.cls;
			r.link = w.link;
			r.roundabout = w.roundabout;
			const bool oneway = (w.oneway != 0);
			if (oneway)
			{
				const int dir_lanes = reversed ? w.lanes_bwd : w.lanes_fwd;
				r.fwd = dir_lanes > 0 ? dir_lanes : (w.lanes > 0 ? w.lanes : default_lanes(w.cls, true));
				r.bwd = 0;
			}
			else if (w.lanes_fwd > 0 || w.lanes_bwd > 0)
			{
				r.fwd = w.lanes_fwd > 0 ? w.lanes_fwd : std::max(1, w.lanes - w.lanes_bwd);
				r.bwd = w.lanes_bwd > 0 ? w.lanes_bwd : std::max(1, w.lanes - w.lanes_fwd);
			}
			else if (w.lanes > 0)
			{
				r.fwd = std::max(1, (w.lanes + 1) / 2);
				r.bwd = std::max(1, w.lanes / 2);
			}
			else
			{
				r.fwd = default_lanes(w.cls, false);
				r.bwd = r.fwd;
			}
			r.fwd = std::max(1, std::min(4, r.fwd));
			r.bwd = std::max(0, std::min(4, r.bwd));
			r.speed = w.maxspeed > 0.0 ? w.maxspeed : default_speed(w.cls, w.link);
			r.lane_width = default_lane_width(w.cls);
			r.name = w.name;
			r.level = w.level;
			int sw = w.sidewalk;
			if (sw < 0)
			{
				const bool urban = (w.cls != rc_motorway && w.cls != rc_trunk && w.cls != rc_service && !w.link);
				sw = (m_opt.sidewalks && urban) ? 3 : 0;
			}
			r.sw_left = (sw & 1) != 0;
			r.sw_right = (sw & 2) != 0;
			if (reversed)
			{
				std::swap(r.sw_left, r.sw_right);
			}
			r.bike = w.cycleway;
			r.overtaking = (w.overtaking != 0);
			m_roads.push_back(r);
		}
	}

	void converter::remove_duplicates()
	{
		// Two ways on the same line between the same junctions: an error in the
		// OSM data (for example an old way that was not deleted). The vehicles
		// of the two roads would drive through each other: keep one road, the
		// way with more tags (the same number: the newer way, higher id).
		// Two one-way roads in opposite directions are a dual carriageway
		// (pair_carriageways joins them).
		const double max_gap = 3.0; // m, less than a lane width
		std::map<std::pair<int, int>, std::vector<int>> groups;
		for (size_t i = 0; i < m_roads.size(); ++i)
		{
			const o_road &r = m_roads[i];
			groups[std::make_pair(std::min(r.ca, r.cb), std::max(r.ca, r.cb))].push_back(static_cast<int>(i));
		}
		auto max_distance = [](const polyline &a, const polyline &b)
		{
			double worst = 0.0;
			const int n = std::max(8, static_cast<int>(a.length() / 2.0));
			for (int k = 0; k <= n; ++k)
			{
				double d = 0.0;
				b.project(a.point_at(a.length() * k / n), &d);
				worst = std::max(worst, d);
			}
			return worst;
		};
		QStringList removed;
		for (auto &kv : groups)
		{
			const std::vector<int> &g = kv.second;
			for (size_t i = 0; i < g.size(); ++i)
			{
				for (size_t j = i + 1; j < g.size(); ++j)
				{
					o_road &x = m_roads[g[i]];
					o_road &y = m_roads[g[j]];
					if (!x.alive || !y.alive || x.level != y.level)
					{
						continue;
					}
					if (x.bwd == 0 && y.bwd == 0 && x.ca == y.cb)
					{
						continue;
					}
					const polyline px(x.pts);
					const polyline py(y.pts);
					if (max_distance(px, py) > max_gap || max_distance(py, px) > max_gap)
					{
						continue;
					}
					const o_way &wx = m_ways[x.way];
					const o_way &wy = m_ways[y.way];
					const bool keep_x = (wx.tags != wy.tags) ? (wx.tags > wy.tags) : (wx.id > wy.id);
					o_road &drop = keep_x ? y : x;
					drop.alive = false;
					removed << QStringLiteral("%1 (kept %2)").arg(m_ways[drop.way].id).arg(keep_x ? wx.id : wy.id);
				}
			}
		}
		removed.removeDuplicates();
		if (!removed.isEmpty())
		{
			m_res.notes << QStringLiteral("Duplicate ways on the same line removed (error in the data): %1").arg(removed.join(QStringLiteral(", ")));
		}
	}

	void converter::pair_carriageways()
	{
		std::map<std::pair<int, int>, std::vector<int>> groups;
		for (size_t i = 0; i < m_roads.size(); ++i)
		{
			const o_road &r = m_roads[i];
			if (r.bwd == 0 && !r.roundabout)
			{
				groups[std::make_pair(std::min(r.ca, r.cb), std::max(r.ca, r.cb))].push_back(static_cast<int>(i));
			}
		}
		int pairs = 0;
		for (auto &kv : groups)
		{
			std::vector<int> &g = kv.second;
			for (size_t i = 0; i < g.size(); ++i)
			{
				o_road &x = m_roads[g[i]];
				if (!x.alive)
				{
					continue;
				}
				for (size_t j = i + 1; j < g.size(); ++j)
				{
					o_road &y = m_roads[g[j]];
					if (!y.alive || x.ca != y.cb || x.cb != y.ca || x.level != y.level)
					{
						continue;
					}
					const polyline px(x.pts);
					std::vector<vec2> yrev(y.pts.rbegin(), y.pts.rend());
					const polyline py(yrev);
					const double lx = px.length();
					const double ly = py.length();
					if (lx < 1.0 || ly < 1.0 || lx / ly > 1.7 || ly / lx > 1.7)
					{
						continue;
					}
					double sep = 0.0;
					for (int k = 1; k <= 5; ++k)
					{
						double d = 0.0;
						py.project(px.point_at(lx * k / 6.0), &d);
						sep += d / 5.0;
					}
					if (sep > 50.0)
					{
						continue;
					}
					// One two-way road on the middle line
					const int n = std::max(8, static_cast<int>(std::max(lx, ly) / 4.0));
					std::vector<vec2> mid;
					for (int k = 0; k <= n; ++k)
					{
						const double t = static_cast<double>(k) / n;
						mid.push_back((px.point_at(t * lx) + py.point_at(t * ly)) * 0.5);
					}
					mid.front() = m_clusters[x.ca].pos;
					mid.back() = m_clusters[x.cb].pos;
					x.pts = simplify(mid, 0.8);
					x.bwd = y.fwd;
					x.median = sep > 4.0;
					x.sw_left = y.sw_right;
					x.bike = x.bike || y.bike;
					x.overtaking = x.overtaking && y.overtaking;
					x.speed = std::max(x.speed, y.speed);
					x.lane_width = std::max(x.lane_width, y.lane_width);
					x.cls = std::max(x.cls, y.cls);
					x.link = x.link && y.link;
					if (x.name.isEmpty())
					{
						x.name = y.name;
					}
					std::vector<int> yn(y.osm_nodes.rbegin(), y.osm_nodes.rend());
					x.osm_nodes.insert(x.osm_nodes.end(), yn.begin(), yn.end());
					y.alive = false;
					++pairs;
					break;
				}
			}
		}
		if (pairs > 0)
		{
			m_res.notes << QStringLiteral("Dual carriageways joined into two-way roads: %1").arg(pairs);
		}
	}

	void converter::merge_chains()
	{
		std::vector<std::vector<int>> at(m_clusters.size());
		for (size_t i = 0; i < m_roads.size(); ++i)
		{
			if (m_roads[i].alive)
			{
				at[m_roads[i].ca].push_back(static_cast<int>(i));
				at[m_roads[i].cb].push_back(static_cast<int>(i));
			}
		}
		bool changed = true;
		int merges = 0;
		while (changed)
		{
			changed = false;
			for (size_t c = 0; c < m_clusters.size(); ++c)
			{
				if (at[c].size() != 2 || (m_clusters[c].flags & (nf_signals | nf_stop | nf_give_way | nf_border | nf_level_crossing)))
				{
					continue;
				}
				const int xi = at[c][0];
				const int yi = at[c][1];
				if (xi == yi)
				{
					continue;
				}
				o_road x = m_roads[xi];
				o_road y = m_roads[yi];
				if (x.cb != static_cast<int>(c))
				{
					reverse_road(x);
				}
				if (y.ca != static_cast<int>(c))
				{
					reverse_road(y);
				}
				if (x.ca == y.cb || !same_attributes(x, y))
				{
					continue;
				}
				x.pts.insert(x.pts.end(), y.pts.begin() + 1, y.pts.end());
				x.osm_nodes.insert(x.osm_nodes.end(), y.osm_nodes.begin(), y.osm_nodes.end());
				x.cb = y.cb;
				if (x.fwd == 0)
				{
					reverse_road(x); // keep one-way roads in the a -> b direction
				}
				m_roads[xi] = x;
				m_roads[yi].alive = false;
				at[c].clear();
				for (int &r : at[y.cb])
				{
					if (r == yi)
					{
						r = xi;
					}
				}
				++merges;
				changed = true;
			}
		}
		Q_UNUSED(merges);
	}

	void converter::add_sign(world_model &m, const o_road &r, int dir, const QString &key)
	{
		const road_data *rd = m.road(r.model_road);
		if (!rd)
		{
			return;
		}
		for (const auto &kv : m.signs)
		{
			if (kv.second.road_id == r.model_road && kv.second.dir == dir)
			{
				return; // one stop or give way sign for each approach
			}
		}
		const double len = m.road_centerline(*rd).length();
		sign_data s;
		s.id = m.next_id();
		s.key = key;
		s.road_id = r.model_road;
		s.dir = dir;
		s.s = dir > 0 ? std::max(len * 0.5, len - 9.0) : std::min(len * 0.5, 9.0);
		m.signs[s.id] = s;
		++m_res.signs;
	}

	void converter::build_model(world_model &m)
	{
		m.clear();
		m.geo_lat = m_proj.lat0();
		m.geo_lon = m_proj.lon0();
		m.name = QStringLiteral("OpenStreetMap import");
		m.description = QStringLiteral("Map data (c) OpenStreetMap contributors, ODbL. Imported %1.")
			.arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")));

		// --- nodes and roads ---
		for (const o_road &r : m_roads)
		{
			if (r.alive)
			{
				++m_clusters[r.ca].roads;
				++m_clusters[r.cb].roads;
			}
		}
		for (o_cluster &c : m_clusters)
		{
			if (c.roads > 0)
			{
				c.model_node = m.add_node(c.pos);
			}
		}
		for (o_road &r : m_roads)
		{
			if (!r.alive)
			{
				continue;
			}
			road_data t;
			t.name = r.name;
			t.lanes_forward = r.fwd;
			t.lanes_backward = r.bwd;
			t.lane_width = r.lane_width;
			t.speed_limit = r.speed;
			t.overtaking = r.overtaking;
			t.level = r.level;
			t.sidewalk_left = r.sw_left;
			t.sidewalk_right = r.sw_right;
			t.bike_lane = r.bike;
			t.median = r.median;
			t.guard_rail = (r.cls == rc_motorway && !r.link);
			std::vector<vec2> shape;
			if (r.pts.size() > 2)
			{
				shape.assign(r.pts.begin() + 1, r.pts.end() - 1);
			}
			r.model_road = m.add_road(m_clusters[r.ca].model_node, m_clusters[r.cb].model_node, shape, t);
		}

		// --- features on the roads: signals, stop / give way, level crossings ---
		struct sign_request
		{
			int road;
			int dir;
			QString key;
		};
		std::vector<sign_request> explicit_signs;
		for (size_t ri = 0; ri < m_roads.size(); ++ri)
		{
			const o_road &r = m_roads[ri];
			if (!r.alive || r.model_road < 0)
			{
				continue;
			}
			const polyline line(r.pts);
			const double len = line.length();
			for (size_t k = 1; k + 1 < r.osm_nodes.size(); ++k)
			{
				const o_node &n = m_nodes[r.osm_nodes[k]];
				if (!(n.flags & (nf_signals | nf_stop | nf_give_way | nf_level_crossing)))
				{
					continue;
				}
				const double s = line.project(n.p);
				// A signal on a one-way road is for the junction ahead (its stop
				// line can be far from the center of a large square). On a two-way
				// road: the nearer junction.
				const bool one_way = (r.bwd == 0);
				const bool near_a = one_way ? false : (s < len - s);
				o_cluster &end_cluster = m_clusters[near_a ? r.ca : r.cb];
				// Distance from the edge of the junction group
				const double dist_end = std::max(0.0, (near_a ? s : len - s) - end_cluster.radius);
				if ((n.flags & nf_signals) && dist_end < (one_way ? 70.0 : 45.0))
				{
					end_cluster.flags |= nf_signals;
				}
				if (n.flags & nf_signals)
				{
					TE_DEBUG("OSM", QStringLiteral("Signal at %1, %2: road %3 (%4 m, lanes %5+%6), %7 m from the junction at the %8 end (%9 roads)")
						.arg(n.p.x, 0, 'f', 0).arg(n.p.y, 0, 'f', 0).arg(r.model_road).arg(len, 0, 'f', 0).arg(r.fwd).arg(r.bwd)
						.arg(dist_end, 0, 'f', 0).arg(near_a ? QStringLiteral("A") : QStringLiteral("B")).arg(end_cluster.roads));
				}
				if ((n.flags & (nf_stop | nf_give_way)) && dist_end < 30.0)
				{
					const int dir = near_a ? -1 : 1;
					if ((dir > 0 ? r.fwd : r.bwd) > 0)
					{
						explicit_signs.push_back({ static_cast<int>(ri), dir, (n.flags & nf_stop) ? QStringLiteral("stop") : QStringLiteral("give_way") });
					}
				}
				if ((n.flags & nf_level_crossing) && m_opt.level_crossings && s > 8.0 && len - s > 8.0)
				{
					const road_data *rd = m.road(r.model_road);
					const polyline center = rd ? m.road_centerline(*rd) : line;
					object_data o;
					o.id = m.next_id();
					o.kind = object_kind::level_crossing;
					o.road_id = r.model_road;
					o.s = center.project(n.p);
					o.dir = 0;
					o.lane = -1;
					o.length = 8.0;
					m.objects[o.id] = o;
					++m_res.objects;
				}
			}
		}

		// --- traffic lights ---
		if (m_opt.lights)
		{
			for (const o_cluster &c : m_clusters)
			{
				if (!(c.flags & nf_signals))
				{
					continue;
				}
				const int lid = (c.roads >= 3 && c.model_node >= 0) ? m.add_light(c.model_node) : -1;
				TE_DEBUG("OSM", QStringLiteral("Signalized junction at %1, %2: %3 roads, node %4, light %5")
					.arg(c.pos.x, 0, 'f', 0).arg(c.pos.y, 0, 'f', 0).arg(c.roads).arg(c.model_node).arg(lid));
				if (lid >= 0)
				{
					++m_res.lights;
				}
			}
		}

		// --- stop and give way signs ---
		for (const sign_request &q : explicit_signs)
		{
			const o_road &r = m_roads[q.road];
			const o_cluster &c = m_clusters[q.dir > 0 ? r.cb : r.ca];
			if (c.roads >= 3 && m.light_at_node(c.model_node) < 0)
			{
				add_sign(m, r, q.dir, q.key);
			}
		}
		std::vector<std::vector<int>> at(m_clusters.size());
		for (size_t i = 0; i < m_roads.size(); ++i)
		{
			if (m_roads[i].alive && m_roads[i].model_road >= 0)
			{
				at[m_roads[i].ca].push_back(static_cast<int>(i));
				at[m_roads[i].cb].push_back(static_cast<int>(i));
			}
		}
		for (size_t ci = 0; ci < m_clusters.size(); ++ci)
		{
			const o_cluster &c = m_clusters[ci];
			if (c.roads < 3 || c.model_node < 0 || m.light_at_node(c.model_node) >= 0)
			{
				continue;
			}
			const bool tagged = (c.flags & (nf_stop | nf_give_way)) != 0;
			if (!tagged && !m_opt.priority_signs)
			{
				continue;
			}
			int max_rank = 0;
			for (int ri : at[ci])
			{
				max_rank = std::max(max_rank, priority_rank(m_roads[ri]));
			}
			for (int ri : at[ci])
			{
				const o_road &r = m_roads[ri];
				const int dir = (r.cb == static_cast<int>(ci)) ? 1 : -1;
				if ((dir > 0 ? r.fwd : r.bwd) <= 0 || priority_rank(r) >= max_rank)
				{
					continue;
				}
				add_sign(m, r, dir, (c.flags & nf_stop) ? QStringLiteral("stop") : QStringLiteral("give_way"));
			}
		}

		// --- bus stops: the nearest road (not a motorway) in 25 m ---
		if (m_opt.bus_stops)
		{
			std::vector<std::pair<int, polyline>> lines;
			for (const o_road &r : m_roads)
			{
				if (r.alive && r.model_road >= 0 && r.cls != rc_motorway && !r.link)
				{
					lines.push_back(std::make_pair(static_cast<int>(&r - &m_roads[0]), m.road_centerline(*m.road(r.model_road))));
				}
			}
			for (const o_node &n : m_nodes)
			{
				if (!(n.flags & nf_bus_stop))
				{
					continue;
				}
				int best = -1;
				double best_d = 25.0;
				double best_s = 0.0;
				for (size_t i = 0; i < lines.size(); ++i)
				{
					double d = 0.0;
					const double s = lines[i].second.project(n.p, &d);
					if (d < best_d)
					{
						best_d = d;
						best = static_cast<int>(i);
						best_s = s;
					}
				}
				if (best < 0)
				{
					continue;
				}
				const o_road &r = m_roads[lines[best].first];
				const polyline &center = lines[best].second;
				const double len = center.length();
				if (len < 30.0)
				{
					continue;
				}
				const double s = std::max(12.0, std::min(len - 12.0, best_s));
				const double lateral = cross(center.tangent_at(s), n.p - center.point_at(s));
				int dir = lateral <= 0.0 ? 1 : -1;
				if ((dir > 0 ? r.fwd : r.bwd) <= 0)
				{
					dir = -dir;
				}
				bool duplicate = false;
				for (const auto &kv : m.objects)
				{
					const object_data &o = kv.second;
					if (o.kind == object_kind::bus_stop && o.road_id == r.model_road && o.dir == dir && std::abs(o.s - s) < 30.0)
					{
						duplicate = true;
						break;
					}
				}
				if (duplicate)
				{
					continue;
				}
				object_data o;
				o.id = m.next_id();
				o.kind = object_kind::bus_stop;
				o.road_id = r.model_road;
				o.s = s;
				o.dir = dir;
				o.lane = 0;
				o.name = n.name.isEmpty() ? QStringLiteral("Bus stop") : n.name;
				m.objects[o.id] = o;
				++m_res.objects;
			}
		}

		// --- traffic sources at the dead ends ---
		for (size_t ci = 0; ci < m_clusters.size(); ++ci)
		{
			const o_cluster &c = m_clusters[ci];
			if (c.roads != 1 || c.model_node < 0 || at[ci].empty())
			{
				continue;
			}
			const o_road &r = m_roads[at[ci].front()];
			const bool starts_here = (r.ca == static_cast<int>(ci));
			const int lanes_out = starts_here ? r.fwd : r.bwd;
			const int lanes_in = starts_here ? r.bwd : r.fwd;
			node_data *nd = m.node(c.model_node);
			nd->spawn = true;
			nd->vehicles_per_hour = std::round(source_rate(r.cls) * std::max(lanes_out, lanes_in) * std::max(0.0, m_opt.demand_factor));
			nd->pedestrians_per_hour = (r.sw_left || r.sw_right) ? 30.0 : 0.0;
			nd->share_bus = r.cls >= rc_secondary ? 0.04 : 0.02;
			nd->share_motorcycle = 0.12;
			++m_res.sources;
		}

		int signal_nodes = 0;
		for (const o_node &n : m_nodes)
		{
			signal_nodes += (n.flags & nf_signals) ? 1 : 0;
		}
		m_res.notes << QStringLiteral("Traffic signal nodes in the data: %1. Traffic lights: %2 (a light needs a junction with three or more roads).")
			.arg(signal_nodes).arg(m_res.lights);
		m_res.nodes = static_cast<int>(m.nodes.size());
		m_res.roads = static_cast<int>(m.roads.size());
		m_res.road_km = m.total_road_km();
		m.touch_network();
	}
}

QString osm_import_result::summary(bool translated) const
{
	const char *const format = "%1 roads (%2 km), %3 nodes, %4 traffic lights, %5 signs, %6 objects, %7 traffic sources. "
		"Data: %8 OSM nodes, %9 ways, %10 roads for vehicles.";
	return (translated ? tx(format) : QString::fromLatin1(format))
		.arg(roads).arg(road_km, 0, 'f', 1).arg(nodes).arg(lights).arg(signs).arg(objects).arg(sources)
		.arg(osm_nodes).arg(osm_ways).arg(used_ways);
}

namespace osm_import
{
	osm_bounds area(double lat, double lon, double width_m, double height_m)
	{
		const geo_projection p(lat, lon);
		osm_bounds b;
		p.to_geo(vec2(-width_m / 2.0, -height_m / 2.0), &b.min_lat, &b.min_lon);
		p.to_geo(vec2(width_m / 2.0, height_m / 2.0), &b.max_lat, &b.max_lon);
		return b;
	}

	QString overpass_query(const osm_bounds &b, const osm_import_options &opt)
	{
		QStringList types = { QStringLiteral("motorway"), QStringLiteral("motorway_link"), QStringLiteral("trunk"), QStringLiteral("trunk_link"),
			QStringLiteral("primary"), QStringLiteral("primary_link"), QStringLiteral("secondary"), QStringLiteral("secondary_link"),
			QStringLiteral("tertiary"), QStringLiteral("tertiary_link"), QStringLiteral("road") };
		if (opt.residential)
		{
			types << QStringLiteral("unclassified") << QStringLiteral("residential") << QStringLiteral("living_street");
		}
		if (opt.service_roads)
		{
			types << QStringLiteral("service");
		}
		const QString bbox = QStringLiteral("%1,%2,%3,%4").arg(b.min_lat, 0, 'f', 7).arg(b.min_lon, 0, 'f', 7)
			.arg(b.max_lat, 0, 'f', 7).arg(b.max_lon, 0, 'f', 7);
		return QStringLiteral("[out:xml][timeout:120];\n"
			"(\n"
			"  way[\"highway\"~\"^(%1)$\"](%2);\n"
			"  node[\"highway\"=\"bus_stop\"](%2);\n"
			");\n"
			"(._;>;);\n"
			"out body;\n").arg(types.join(QLatin1Char('|')), bbox);
	}

	bool convert(const QByteArray &osm_xml, const osm_bounds &bounds, const osm_import_options &opt,
		world_model &model, osm_import_result *result, QString *error)
	{
		QElapsedTimer timer;
		timer.start();
		osm_import_result local;
		osm_import_result &res = result ? *result : local;
		res = osm_import_result();
		QString err;
		converter conv(opt, res);
		if (!conv.parse(osm_xml, &err) || !conv.project(bounds, &err))
		{
			if (error)
			{
				*error = err;
			}
			TE_WARN("OSM", QStringLiteral("Import failed: %1").arg(err));
			return false;
		}
		conv.make_segments();
		conv.make_clusters();
		conv.make_roads();
		conv.remove_duplicates();
		conv.pair_carriageways();
		conv.merge_chains();
		conv.build_model(model);
		if (res.roads == 0)
		{
			if (error)
			{
				*error = QStringLiteral("No road is in the area.");
			}
			return false;
		}
		TE_INFO("OSM", QStringLiteral("Import done in %1 ms: %2").arg(timer.elapsed()).arg(res.summary()));
		for (const QString &n : res.notes)
		{
			TE_INFO("OSM", n);
		}
		return true;
	}
}
