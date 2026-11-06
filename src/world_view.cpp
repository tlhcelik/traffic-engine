#include "world_view.h"
#include "app_theme.h"
#include "config_common.h"
#include "congestion_dock.h"
#include "debug_tools.h"
#include "i18n.h"
#include "logger.h"
#include "road_network.h"
#include "sign_catalog.h"
#include "tile_cache.h"
#include "workspace.h"
#include "world_layout.h"

#include <QKeyEvent>
#include <QMessageBox>
#include <QMouseEvent>
#include <QOpenGLContext>
#include <QOpenGLDebugLogger>
#include <QPainter>
#include <QPainterPath>
#include <QTime>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

namespace
{
	QString hms(double seconds)
	{
		const int s = static_cast<int>(seconds) % 86400;
		return QTime(0, 0).addSecs(s).toString(QStringLiteral("HH:mm:ss"));
	}

	QString road_label(const world_model &m, int road_id)
	{
		const road_data *r = m.road(road_id);
		return (r && !r->name.isEmpty()) ? r->name : tx("Road %1").arg(road_id);
	}

	// "from the north" etc.: the direction from which the traffic of an arm comes
	QString compass_from(double angle)
	{
		static const char *names[] = { "east", "north-east", "north", "north-west", "west", "south-west", "south", "south-east" };
		double a = angle * 180.0 / M_PI;
		if (a < 0.0)
		{
			a += 360.0;
		}
		const int i = static_cast<int>(std::floor((a + 22.5) / 45.0)) % 8;
		return tx("from the %1").arg(tx(names[i]));
	}

	QString color_name(signal_color c)
	{
		switch (c)
		{
			case signal_color::red: return tx("red");
			case signal_color::red_yellow: return tx("red and yellow");
			case signal_color::green: return tx("green");
			case signal_color::yellow: return tx("yellow");
			default: return tx("off");
		}
	}

	// Turn movement in a junction, for the popup of a vehicle
	QString turn_phrase(turn_kind t)
	{
		switch (t)
		{
			case turn_kind::right: return tx("right turn");
			case turn_kind::left: return tx("left turn");
			case turn_kind::uturn: return tx("U-turn");
			default: return tx("straight on");
		}
	}

	QColor signal_qcolor(signal_color c)
	{
		switch (c)
		{
			case signal_color::red: return QColor(255, 138, 128);
			case signal_color::red_yellow: return QColor(255, 180, 120);
			case signal_color::green: return QColor(143, 209, 158);
			case signal_color::yellow: return QColor(255, 224, 130);
			default: return QColor(200, 200, 200);
		}
	}

	double working_set_mb()
	{
		PROCESS_MEMORY_COUNTERS pmc;
		if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc)))
		{
			return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
		}
		return 0.0;
	}

	void draw_panel(QPainter &p, const QRectF &r, const QColor &border)
	{
		p.setPen(QPen(border, 1.2));
		p.setBrush(QColor(255, 255, 255, 236));
		p.drawRoundedRect(r, 10, 10);
	}
}

world_view::world_view(workspace *ws, QWidget *parent)
	: QOpenGLWidget(parent)
	, m_ws(ws)
	, m_gl_ok(false)
	, m_tool(tool_select)
	, m_sign_key(QStringLiteral("speed_limit"))
	, m_drawing(false)
	, m_drag(drag_none)
	, m_moved(false)
	, m_drag_id(-1)
	, m_drag_index(-1)
	, m_dev(false)
	, m_countdowns(true)
	, m_labels(true)
	, m_grid(false)
	, m_density(true)
	, m_map(true)
	, m_follow(false)
	, m_first_fit(true)
	, m_frames(0)
	, m_fps(0.0)
	, m_frame_ms(0.0)
	, m_gl_logger(nullptr)
{
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	const config_common &cfg = config_common::instance();
	m_dev = cfg.bool_value(QStringLiteral("render/show_dev_overlay")) || debug_tools::is_debug_mode();
	m_countdowns = cfg.bool_value(QStringLiteral("render/show_countdowns"));
	m_labels = cfg.bool_value(QStringLiteral("render/show_labels"));
	m_grid = cfg.bool_value(QStringLiteral("render/show_grid"));
	m_map = cfg.bool_value(QStringLiteral("render/show_map"));
	m_anim.start();
	m_fps_timer.start();
}

world_view::~world_view()
{
	// The base class deletes the context later and sends aboutToBeDestroyed;
	// at that time the members of this class do not exist anymore.
	if (context())
	{
		disconnect(context(), nullptr, this, nullptr);
	}
	if (m_gl_ok && context())
	{
		makeCurrent();
		m_renderer.release();
		doneCurrent();
	}
}

road_data &world_view::road_template()
{
	static road_data t;
	return t;
}

QString world_view::tool_name(int t)
{
	static const char *names[] =
	{
		"Select", "Road", "Traffic light", "Sign", "Cones", "Barrier", "Road closure",
		"Level crossing", "Bus stop", "Traffic demand", "Erase", "Bridge or overpass", "Tunnel or underpass"
	};
	return (t >= 0 && t < tool_count) ? QString::fromLatin1(names[t]) : QString();
}

QString world_view::tool_icon(int t)
{
	static const char *icons[] =
	{
		"select", "road", "light", "sign", "cone", "barrier", "closure", "crossing", "bus_stop", "spawn", "erase", "bridge", "tunnel"
	};
	return (t >= 0 && t < tool_count) ? QString::fromLatin1(icons[t]) : QString();
}

QString world_view::tool_hint(int t)
{
	switch (t)
	{
		case tool_select: return QStringLiteral("Click an element to select it. Ctrl+click: select more roads (or remove a road from the selection). Drag to move the map. In editor mode, drag a node to move it. Shift+click on the selected road adds a control point.");
		case tool_road: return QStringLiteral("Click to set the road points. Click a node or a road to connect to it. Double-click or Enter: finish. Esc: cancel. Shift: 15-degree steps.");
		case tool_light: return QStringLiteral("Click a junction to add a traffic light.");
		case tool_sign: return QStringLiteral("Click a road side to put the sign there. The sign is for the traffic on that side. Select the sign in the Signs panel.");
		case tool_cone: return QStringLiteral("Click a lane to close a part of it with cones.");
		case tool_barrier: return QStringLiteral("Click a lane to close a part of it with a barrier.");
		case tool_closure: return QStringLiteral("Click a road to close it for all vehicles.");
		case tool_crossing: return QStringLiteral("Click a road to add a level crossing with barriers.");
		case tool_bus_stop: return QStringLiteral("Click a road side to add a bus stop on that side.");
		case tool_spawn: return QStringLiteral("Click a dead-end node to set its traffic demand.");
		case tool_erase: return QStringLiteral("Click an element to remove it.");
		case tool_bridge: return QStringLiteral("Draw a bridge or an overpass. Start and end on a node or a road. Click the points beside the road below the bridge, not on it: a click on a road connects to it. Double-click or Enter: finish. Esc: cancel.");
		case tool_tunnel: return QStringLiteral("Draw a tunnel or an underpass. Start and end on a node or a road. Click the points beside the road above the tunnel, not on it: a click on a road connects to it. Double-click or Enter: finish. Esc: cancel.");
		default: return QString();
	}
}

void world_view::set_tool(tool_t t)
{
	if (m_tool == t)
	{
		return;
	}
	cancel_road();
	m_tool = t;
	logger::instance().action(QStringLiteral("Tool: %1").arg(tool_name(t)));
	emit tool_changed(t);
	emit status_message(tx(tool_hint(t)));
	update();
}

void world_view::set_follow(bool on)
{
	m_follow = on;
	update();
}

// ---------------------------------------------------------------------------
// OpenGL
// ---------------------------------------------------------------------------
void world_view::initializeGL()
{
	m_gl_ok = m_renderer.init(&m_gl_error);
	if (debug_tools::is_debug_mode() && context()->hasExtension(QByteArrayLiteral("GL_KHR_debug")))
	{
		m_gl_logger = new QOpenGLDebugLogger(this);
		if (m_gl_logger->initialize())
		{
			connect(m_gl_logger, &QOpenGLDebugLogger::messageLogged, this, [](const QOpenGLDebugMessage &msg)
			{
				if (msg.severity() != QOpenGLDebugMessage::NotificationSeverity)
				{
					TE_DEBUG("GL", msg.message());
				}
			});
			m_gl_logger->startLogging();
		}
	}
	connect(context(), &QOpenGLContext::aboutToBeDestroyed, this, [this]()
	{
		makeCurrent();
		m_renderer.release();
		m_gl_ok = false;
		doneCurrent();
	});
}

void world_view::resizeGL(int w, int h)
{
	m_cam.width = std::max(1, w);
	m_cam.height = std::max(1, h);
}

void world_view::paintGL()
{
	QElapsedTimer frame;
	frame.start();
	m_cam.width = std::max(1, width());
	m_cam.height = std::max(1, height());
	if (m_first_fit)
	{
		m_first_fit = false;
		zoom_to_fit();
	}
	if (m_follow)
	{
		const element_ref sel = m_ws->selection();
		if (sel.kind == element_kind::vehicle)
		{
			const sim_vehicle *v = m_ws->sim().find_vehicle(sel.id);
			if (v)
			{
				m_cam.center = v->pos;
			}
		}
		else if (sel.kind == element_kind::pedestrian)
		{
			const sim_pedestrian *pd = m_ws->sim().find_pedestrian(sel.id);
			if (pd)
			{
				m_cam.center = pd->pos;
			}
		}
	}

	const config_common &cfg = config_common::instance();
	render_options opt;
	opt.editor_mode = m_ws->editor_mode();
	opt.show_grid = m_grid;
	opt.show_density = m_density;
	opt.show_congestion = m_congestion;
	opt.show_map = m_map;
	opt.map_opacity = qBound(0, cfg.int_value(QStringLiteral("render/map_opacity")), 100) / 100.0;
	opt.map_fade = qBound(0, cfg.int_value(QStringLiteral("render/map_fade")), 90) / 100.0;
	opt.map_max_zoom = qBound(1, cfg.int_value(QStringLiteral("render/map_max_zoom")), 22);
	opt.dpr = devicePixelRatioF();
	opt.selection = m_ws->selection();
	if (m_ws->selected_roads().size() > 1)
	{
		opt.selection_roads = m_ws->selected_roads();
	}
	opt.anim_time = m_anim.elapsed() / 1000.0;
	if (drawing_tool() && m_drawing)
	{
		opt.preview = m_points;
		opt.preview.push_back(m_hover_snap.point);
	}
	if (drawing_tool() && m_hover_snap.snapped)
	{
		opt.preview_snap = true;
		opt.snap_point = m_hover_snap.point;
	}
	if (opt.editor_mode)
	{
		opt.handles = selected_road_handles();
	}

	QPainter p(this);
	p.beginNativePainting();
	if (m_gl_ok)
	{
		m_renderer.render(*m_ws, m_cam, opt);
	}
	else
	{
		QOpenGLFunctions *f = context()->functions();
		f->glClearColor(0.95f, 0.96f, 0.98f, 1.0f);
		f->glClear(GL_COLOR_BUFFER_BIT);
	}
	p.endNativePainting();

	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::TextAntialiasing, true);
	if (!m_gl_ok)
	{
		p.setPen(app_theme::text_dark());
		p.drawText(rect(), Qt::AlignCenter, tx("OpenGL 2.1 is not available. The world cannot be shown.\n%1").arg(m_gl_error));
		return;
	}
	if (m_labels)
	{
		paint_labels(p);
	}
	if (m_countdowns)
	{
		paint_countdowns(p);
	}
	paint_popup(p);
	paint_hud(p);
	if (m_dev)
	{
		paint_dev(p);
	}
	p.end();

	// frame statistics
	m_frame_ms = 0.9 * m_frame_ms + 0.1 * (frame.nsecsElapsed() / 1.0e6);
	++m_frames;
	const qint64 ms = m_fps_timer.elapsed();
	if (ms >= 1000)
	{
		m_fps = m_frames * 1000.0 / ms;
		m_frames = 0;
		m_fps_timer.restart();
	}
}

QImage world_view::snapshot()
{
	return grabFramebuffer();
}

// ---------------------------------------------------------------------------
// camera
// ---------------------------------------------------------------------------
void world_view::zoom_to_fit()
{
	const world_model &m = m_ws->model();
	if (m.nodes.empty())
	{
		m_cam.center = vec2(0.0, 0.0);
		m_cam.ppm = 2.0;
		update();
		return;
	}
	vec2 lo(1e18, 1e18);
	vec2 hi(-1e18, -1e18);
	for (const auto &kv : m.nodes)
	{
		lo.x = std::min(lo.x, kv.second.pos.x);
		lo.y = std::min(lo.y, kv.second.pos.y);
		hi.x = std::max(hi.x, kv.second.pos.x);
		hi.y = std::max(hi.y, kv.second.pos.y);
	}
	const double w = std::max(40.0, hi.x - lo.x) * 1.15;
	const double h = std::max(40.0, hi.y - lo.y) * 1.15;
	m_cam.center = (lo + hi) * 0.5;
	m_cam.ppm = std::max(0.02, std::min(40.0, std::min(m_cam.width / w, m_cam.height / h)));
	update();
}

void world_view::zoom_by(double factor, const QPointF &anchor)
{
	m_first_fit = false; // an explicit view: no automatic fit at the first frame
	const vec2 before = world_at(anchor);
	m_cam.ppm = std::max(0.01, std::min(300.0, m_cam.ppm * factor));
	const vec2 after = world_at(anchor);
	m_cam.center += before - after;
	update();
}

void world_view::center_on(const vec2 &p)
{
	m_first_fit = false;
	m_cam.center = p;
	update();
}

// ---------------------------------------------------------------------------
// input
// ---------------------------------------------------------------------------
void world_view::mousePressEvent(QMouseEvent *e)
{
	setFocus();
	m_press_pos = e->pos();
	m_last_pos = e->pos();
	m_moved = false;
	m_drag = drag_pan;
	m_drag_id = -1;
	m_drag_index = -1;
	if (e->button() == Qt::LeftButton && m_tool == tool_select && m_ws->editor_mode())
	{
		const vec2 w = world_at(e->pos());
		const std::vector<vec2> handles = selected_road_handles();
		for (size_t i = 0; i < handles.size(); ++i)
		{
			if (distance(handles[i], w) <= px(9.0))
			{
				m_drag = drag_handle;
				m_drag_id = m_ws->selection().id;
				m_drag_index = static_cast<int>(i);
				return;
			}
		}
		const int node = world_layout::node_at(m_ws->model(), w, px(10.0));
		if (node >= 0)
		{
			m_drag = drag_node;
			m_drag_id = node;
			m_ws->select(element_ref(element_kind::node, node));
		}
	}
}

void world_view::mouseMoveEvent(QMouseEvent *e)
{
	const vec2 w = world_at(e->pos());
	m_cursor = w;
	if (drawing_tool())
	{
		m_hover_snap = snap_road_point(w, e->modifiers());
	}
	if (e->buttons() != Qt::NoButton && m_drag != drag_none)
	{
		const QPoint d = e->pos() - m_last_pos;
		if ((e->pos() - m_press_pos).manhattanLength() > 3)
		{
			m_moved = true;
		}
		if (m_moved)
		{
			if (m_drag == drag_pan)
			{
				m_cam.center -= vec2(d.x() / m_cam.ppm, -d.y() / m_cam.ppm);
				m_follow = false;
			}
			else if (m_drag == drag_node)
			{
				m_ws->model().move_node(m_drag_id, w);
				m_ws->model_changed(property_change::change_network);
			}
			else if (m_drag == drag_handle)
			{
				road_data *r = m_ws->model().road(m_drag_id);
				if (r && m_drag_index >= 0 && m_drag_index < static_cast<int>(r->shape.size()))
				{
					r->shape[m_drag_index] = w;
					m_ws->model().touch_network();
					m_ws->model_changed(property_change::change_network);
				}
			}
		}
		m_last_pos = e->pos();
	}
	update();
}

void world_view::mouseReleaseEvent(QMouseEvent *e)
{
	const drag_t drag = m_drag;
	m_drag = drag_none;
	if (m_moved)
	{
		if (drag == drag_node)
		{
			logger::instance().action(QStringLiteral("Node %1 moved").arg(m_drag_id));
			emit model_edited();
		}
		else if (drag == drag_handle)
		{
			logger::instance().action(QStringLiteral("Road %1 shape changed").arg(m_drag_id));
			emit model_edited();
		}
		return;
	}
	const vec2 w = world_at(e->pos());
	if (e->button() == Qt::LeftButton)
	{
		click(w, e->modifiers());
	}
	else if (e->button() == Qt::RightButton)
	{
		if (m_drawing)
		{
			if (m_points.size() >= 2)
			{
				finish_road();
			}
			else
			{
				cancel_road();
			}
		}
		else
		{
			m_ws->select(element_ref());
		}
	}
	update();
}

void world_view::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (e->button() == Qt::LeftButton && drawing_tool() && m_drawing && m_points.size() >= 2)
	{
		m_end = snap_result();
		finish_road();
		update();
		return;
	}
	if (e->button() == Qt::LeftButton && m_tool == tool_select)
	{
		const element_ref sel = m_ws->selection();
		if (sel.kind == element_kind::vehicle || sel.kind == element_kind::pedestrian)
		{
			set_follow(true);
			emit status_message(tx("The view follows the selected element. Drag the map to stop."));
		}
	}
}

void world_view::wheelEvent(QWheelEvent *e)
{
	const double steps = e->angleDelta().y() / 120.0;
	zoom_by(std::pow(1.18, steps), e->posF());
	e->accept();
}

void world_view::keyPressEvent(QKeyEvent *e)
{
	switch (e->key())
	{
		case Qt::Key_Escape:
			if (m_drawing)
			{
				cancel_road();
			}
			else if (m_tool != tool_select)
			{
				set_tool(tool_select);
			}
			else
			{
				m_ws->select(element_ref());
			}
			break;
		case Qt::Key_Return:
		case Qt::Key_Enter:
			if (m_drawing && m_points.size() >= 2)
			{
				m_end = snap_result();
				finish_road();
			}
			break;
		case Qt::Key_Delete:
			delete_selection();
			break;
		default:
			QOpenGLWidget::keyPressEvent(e);
			return;
	}
	update();
}

// ---------------------------------------------------------------------------
// tools
// ---------------------------------------------------------------------------
bool world_view::require_editor()
{
	if (!m_ws->editor_mode())
	{
		m_ws->set_editor_mode(true);
		emit status_message(tx("Editor mode is on. The flow is stopped."));
	}
	return true;
}

void world_view::edited(property_change change, const QString &action)
{
	m_ws->model_changed(change);
	logger::instance().action(action);
	emit model_edited();
	update();
}

void world_view::click(const vec2 &w, Qt::KeyboardModifiers mods)
{
	switch (m_tool)
	{
		case tool_select:
		{
			// Shift+click on the selected road: add a control point
			const element_ref sel = m_ws->selection();
			if ((mods & Qt::ShiftModifier) && sel.kind == element_kind::road && m_ws->editor_mode())
			{
				road_data *r = m_ws->model().road(sel.id);
				if (r && m_ws->network().road_at(w, px(6.0)) == sel.id)
				{
					const polyline c = m_ws->model().road_centerline(*r);
					const double s_new = c.project(w);
					size_t at = 0;
					while (at < r->shape.size() && c.project(r->shape[at]) < s_new)
					{
						++at;
					}
					r->shape.insert(r->shape.begin() + static_cast<std::ptrdiff_t>(at), w);
					m_ws->model().touch_network();
					edited(property_change::change_network, QStringLiteral("Control point added to road %1").arg(sel.id));
					return;
				}
			}
			const element_ref ref = world_layout::pick(*m_ws, w, px(8.0), true, m_ws->editor_mode());
			if (mods & Qt::ControlModifier)
			{
				// Ctrl+click on a road: the road goes into the selection or out of it.
				// A Ctrl+click on another place does not change the selection.
				if (ref.kind == element_kind::road)
				{
					m_ws->toggle_road(ref.id);
					const size_t n = m_ws->selected_roads().size();
					if (n > 1)
					{
						emit status_message(tx("%1 roads are selected. A change in the property panel applies to all of them.").arg(n));
					}
				}
				break;
			}
			m_ws->select(ref);
			if (ref.kind != element_kind::vehicle && ref.kind != element_kind::pedestrian)
			{
				m_follow = false;
			}
			break;
		}
		case tool_road:
		case tool_bridge:
		case tool_tunnel:
			require_editor();
			road_click(w, mods);
			break;
		case tool_light:
			require_editor();
			place_light(w);
			break;
		case tool_sign:
			require_editor();
			place_sign(w);
			break;
		case tool_cone:
			require_editor();
			place_object(static_cast<int>(object_kind::cone), w);
			break;
		case tool_barrier:
			require_editor();
			place_object(static_cast<int>(object_kind::barrier), w);
			break;
		case tool_closure:
			require_editor();
			place_object(static_cast<int>(object_kind::road_closure), w);
			break;
		case tool_crossing:
			require_editor();
			place_object(static_cast<int>(object_kind::level_crossing), w);
			break;
		case tool_bus_stop:
			require_editor();
			place_object(static_cast<int>(object_kind::bus_stop), w);
			break;
		case tool_spawn:
		{
			const int node = world_layout::node_at(m_ws->model(), w, px(14.0));
			if (node >= 0 && m_ws->model().roads_at_node(node).size() == 1)
			{
				m_ws->select(element_ref(element_kind::node, node));
				emit status_message(tx("Set the traffic demand of node %1 in the Properties panel.").arg(node));
			}
			else
			{
				emit status_message(tx("Click a dead-end node. Only a dead end makes traffic."));
			}
			break;
		}
		case tool_erase:
			require_editor();
			erase_at(w);
			break;
		default:
			break;
	}
}

world_view::snap_result world_view::snap_road_point(const vec2 &w, Qt::KeyboardModifiers mods) const
{
	snap_result r;
	r.point = w;
	const world_model &m = m_ws->model();
	const int node = world_layout::node_at(m, w, px(12.0));
	if (node >= 0)
	{
		r.node = node;
		r.point = m.node(node)->pos;
		r.snapped = true;
		return r;
	}
	double s = 0.0;
	const int rid = m_ws->network().road_at(w, px(4.0), &s, nullptr);
	if (rid >= 0)
	{
		const net_road *nr = m_ws->network().road(rid);
		const double len = nr->center.length();
		const road_data *rd = m.road(rid);
		if (s < 4.0)
		{
			r.node = rd->node_a;
			r.point = m.node(rd->node_a)->pos;
		}
		else if (s > len - 4.0)
		{
			r.node = rd->node_b;
			r.point = m.node(rd->node_b)->pos;
		}
		else
		{
			r.road = rid;
			r.road_s = s;
			r.point = nr->center.point_at(s);
		}
		r.snapped = true;
		return r;
	}
	if ((mods & Qt::ShiftModifier) && !m_points.empty())
	{
		const vec2 prev = m_points.back();
		const vec2 d = w - prev;
		const double step = 15.0 * M_PI / 180.0;
		const double a = std::round(d.angle() / step) * step;
		r.point = prev + from_angle(a) * d.length();
	}
	if (mods & Qt::ControlModifier)
	{
		r.point = vec2(std::round(r.point.x), std::round(r.point.y));
	}
	return r;
}

void world_view::road_click(const vec2 &w, Qt::KeyboardModifiers mods)
{
	const snap_result sr = snap_road_point(w, mods);
	if (!m_drawing)
	{
		m_drawing = true;
		m_points.clear();
		m_points.push_back(sr.point);
		m_start = sr;
		m_end = snap_result();
		emit status_message(tx("Click the next points. Double-click or Enter to finish the road."));
		return;
	}
	if (distance(sr.point, m_points.back()) < 1.0)
	{
		return; // the same point again
	}
	if (sr.node >= 0 || sr.road >= 0)
	{
		if (sr.node >= 0 && sr.node == m_start.node && m_points.size() < 2)
		{
			return;
		}
		m_points.push_back(sr.point);
		m_end = sr;
		finish_road();
		return;
	}
	m_points.push_back(sr.point);
}

void world_view::cancel_road()
{
	m_drawing = false;
	m_points.clear();
	m_start = snap_result();
	m_end = snap_result();
	update();
}

void world_view::finish_road()
{
	if (!m_drawing || m_points.size() < 2)
	{
		cancel_road();
		return;
	}
	world_model &m = m_ws->model();
	double length = 0.0;
	for (size_t i = 1; i < m_points.size(); ++i)
	{
		length += distance(m_points[i - 1], m_points[i]);
	}
	if (length < 3.0)
	{
		emit status_message(tx("The road is too short. A road must be 3 m or longer."));
		cancel_road();
		return;
	}
	// Splits: the split with the larger distance first (if both are on the same road)
	int a = m_start.node;
	int b = m_end.node;
	if (m_start.road >= 0 && m_end.road >= 0 && m_start.road == m_end.road)
	{
		if (m_end.road_s > m_start.road_s)
		{
			b = m.split_road(m_end.road, m_end.road_s);
			a = m.split_road(m_start.road, m_start.road_s);
		}
		else
		{
			a = m.split_road(m_start.road, m_start.road_s);
			b = m.split_road(m_end.road, m_end.road_s);
		}
	}
	else
	{
		if (m_start.road >= 0)
		{
			a = m.split_road(m_start.road, m_start.road_s);
		}
		if (m_end.road >= 0)
		{
			b = m.split_road(m_end.road, m_end.road_s);
		}
	}
	const double rate = config_common::instance().double_value(QStringLiteral("simulation/default_spawn_rate"));
	if (a < 0)
	{
		a = m.add_node(m_points.front());
		m.node(a)->vehicles_per_hour = rate;
	}
	if (b < 0)
	{
		b = m.add_node(m_points.back());
		m.node(b)->vehicles_per_hour = rate;
	}
	if (a == b)
	{
		emit status_message(tx("The start and the end of the road are the same node. The road is not added."));
		m.touch_network();
		m_ws->model_changed(property_change::change_network);
		cancel_road();
		return;
	}
	const std::vector<vec2> shape(m_points.begin() + 1, m_points.end() - 1);
	road_data t = road_template();
	if (m_tool == tool_bridge)
	{
		t.level = 1;
	}
	else if (m_tool == tool_tunnel)
	{
		t.level = -1;
	}
	const int rid = m.add_road(a, b, shape, t);
	cancel_road();
	edited(property_change::change_network, QStringLiteral("Road %1 added (node %2 -> node %3, %4 m, level %5)").arg(rid).arg(a).arg(b).arg(length, 0, 'f', 0).arg(t.level));
	m_ws->select(element_ref(element_kind::road, rid));
	emit status_message(tx("Road %1 is added. Click to start the next road.").arg(rid));
}

void world_view::place_light(const vec2 &w)
{
	world_model &m = m_ws->model();
	int node = world_layout::node_at(m, w, px(16.0));
	if (node < 0)
	{
		for (const auto &kv : m_ws->network().junctions)
		{
			if (kv.second.polygon.size() >= 3 && geom::point_in_polygon(w, kv.second.polygon))
			{
				node = kv.first;
				break;
			}
		}
	}
	if (node < 0 || m.roads_at_node(node).size() < 2)
	{
		emit status_message(tx("Click a junction (a node with two or more roads)."));
		return;
	}
	const bool had = m.light_at_node(node) >= 0;
	const int lid = m.add_light(node);
	if (lid < 0)
	{
		return;
	}
	if (!had)
	{
		edited(property_change::change_network, QStringLiteral("Traffic light %1 added at node %2").arg(lid).arg(node));
	}
	m_ws->select(element_ref(element_kind::light, lid));
	emit status_message(tx("Traffic light %1. Set its program in the Properties panel or with Tools > Traffic light program.").arg(lid));
}

void world_view::place_sign(const vec2 &w)
{
	const sign_def *d = sign_catalog::instance().find(m_sign_key);
	if (!d)
	{
		emit status_message(tx("Select a sign in the Signs panel first."));
		return;
	}
	double s = 0.0;
	double lateral = 0.0;
	const int rid = m_ws->network().road_at(w, px(10.0), &s, &lateral);
	if (rid < 0)
	{
		emit status_message(tx("Click near a road. The sign is for the traffic on the side that you click."));
		return;
	}
	world_model &m = m_ws->model();
	sign_data sd;
	sd.id = m.next_id();
	sd.key = d->key;
	sd.road_id = rid;
	sd.s = s;
	sd.dir = lateral < 0.0 ? 1 : -1;
	sd.value = d->default_value;
	m.signs[sd.id] = sd;
	m.touch_network();
	edited(property_change::change_network, QStringLiteral("Sign %1 (%2) added on road %3").arg(sd.id).arg(d->name).arg(rid));
	m_ws->select(element_ref(element_kind::sign, sd.id));
}

void world_view::place_object(int kind, const vec2 &w)
{
	double s = 0.0;
	double lateral = 0.0;
	const int rid = m_ws->network().road_at(w, px(8.0), &s, &lateral);
	if (rid < 0)
	{
		emit status_message(tx("Click a road."));
		return;
	}
	world_model &m = m_ws->model();
	object_data o;
	o.id = m.next_id();
	o.kind = static_cast<object_kind>(kind);
	o.road_id = rid;
	o.s = s;
	switch (o.kind)
	{
		case object_kind::cone:
		case object_kind::barrier:
		{
			const int lid = m_ws->network().lane_from_lateral(rid, lateral);
			if (lid >= 0)
			{
				const net_lane &l = m_ws->network().lanes[lid];
				o.dir = l.dir;
				o.lane = l.index;
			}
			o.length = o.kind == object_kind::cone ? 15.0 : 24.0;
			break;
		}
		case object_kind::road_closure:
			o.dir = 0;
			break;
		case object_kind::level_crossing:
			o.dir = 0;
			o.train_offset = 30.0;
			break;
		case object_kind::bus_stop:
			o.dir = lateral < 0.0 ? 1 : -1;
			o.name = tx("Bus stop %1").arg(o.id);
			break;
	}
	m.objects[o.id] = o;
	m.touch_network();
	edited(property_change::change_network, QStringLiteral("%1 %2 added on road %3").arg(object_kind_name(o.kind)).arg(o.id).arg(rid));
	m_ws->select(element_ref(element_kind::object, o.id));
}

void world_view::erase_at(const vec2 &w)
{
	const element_ref ref = world_layout::pick(*m_ws, w, px(8.0), false, true);
	if (!ref.valid() || ref.kind == element_kind::vehicle || ref.kind == element_kind::pedestrian)
	{
		emit status_message(tx("There is no element at this point."));
		return;
	}
	const QString name = world_properties::title(m_ws->model(), ref);
	const QString log_name = world_properties::title(m_ws->model(), ref, false);
	m_ws->model().remove(ref);
	edited(property_change::change_network, QStringLiteral("Removed: %1").arg(log_name));
	emit status_message(tx("Removed: %1").arg(name));
}

void world_view::delete_selection()
{
	const element_ref ref = m_ws->selection();
	if (!ref.valid() || ref.kind == element_kind::vehicle || ref.kind == element_kind::pedestrian)
	{
		return;
	}
	if (!m_ws->editor_mode())
	{
		emit status_message(tx("Remove is possible only in editor mode (Ctrl+E)."));
		return;
	}
	const std::vector<int> roads = m_ws->selected_roads();
	if (roads.size() > 1)
	{
		// More selected roads: all of them, after a question
		const auto answer = QMessageBox::question(this, tx("Remove roads"), tx("Remove the %1 selected roads?").arg(roads.size()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
		{
			return;
		}
		QStringList ids;
		for (int id : roads)
		{
			m_ws->model().remove(element_ref(element_kind::road, id));
			ids << QString::number(id);
		}
		m_ws->select(element_ref());
		edited(property_change::change_network, QStringLiteral("Removed: roads %1").arg(ids.join(QStringLiteral(", "))));
		return;
	}
	const QString log_name = world_properties::title(m_ws->model(), ref, false);
	m_ws->model().remove(ref);
	m_ws->select(element_ref());
	edited(property_change::change_network, QStringLiteral("Removed: %1").arg(log_name));
}

std::vector<vec2> world_view::selected_road_handles() const
{
	const element_ref sel = m_ws->selection();
	if (sel.kind != element_kind::road)
	{
		return std::vector<vec2>();
	}
	const road_data *r = m_ws->model().road(sel.id);
	return r ? r->shape : std::vector<vec2>();
}

// ---------------------------------------------------------------------------
// overlays
// ---------------------------------------------------------------------------
void world_view::paint_countdowns(QPainter &p)
{
	if (m_cam.ppm < 1.8)
	{
		return;
	}
	QFont f = font();
	f.setPointSizeF(8.5);
	f.setBold(true);
	p.setFont(f);
	const QFontMetrics fm(f);
	for (const world_layout::signal_head &h : world_layout::signal_heads(m_ws->model(), m_ws->network()))
	{
		const signal_color c = m_ws->sim().signal_for(h.node_id, h.road_id);
		if (c == signal_color::off)
		{
			continue;
		}
		const double rem = m_ws->sim().signal_remaining(h.node_id, h.road_id);
		const QString text = QString::number(static_cast<int>(std::ceil(std::max(0.0, rem))));
		const vec2 anchor = h.pos + h.dir.right() * (std::max(1.6, 16.0 / m_cam.ppm));
		const QPointF sp = m_cam.to_screen(anchor);
		const double w = fm.horizontalAdvance(text) + 10.0;
		const QRectF r(sp.x() - w / 2.0, sp.y() - 9.0, w, 18.0);
		if (!rect().intersects(r.toAlignedRect()))
		{
			continue;
		}
		p.setPen(QPen(signal_qcolor(c).darker(130), 1.0));
		p.setBrush(signal_qcolor(c));
		p.drawRoundedRect(r, 7, 7);
		p.setPen(QColor(40, 46, 56));
		p.drawText(r, Qt::AlignCenter, text);
	}
}

void world_view::paint_labels(QPainter &p)
{
	if (m_cam.ppm < 1.2)
	{
		return;
	}
	QFont f = font();
	f.setPointSizeF(8.5);
	p.setFont(f);
	const QFontMetrics fm(f);
	for (const auto &kv : m_ws->model().roads)
	{
		if (kv.second.name.isEmpty())
		{
			continue;
		}
		const net_road *r = m_ws->network().road(kv.first);
		if (!r || r->center.length() * m_cam.ppm < fm.horizontalAdvance(kv.second.name) + 40)
		{
			continue;
		}
		vec2 c;
		vec2 t;
		r->center.sample(r->center.length() / 2.0, &c, &t);
		const QPointF sp = m_cam.to_screen(c + t.left() * (r->outer_left + 2.0));
		if (!rect().contains(sp.toPoint()))
		{
			continue;
		}
		double angle = -t.angle() * 180.0 / M_PI;
		if (angle > 90.0) { angle -= 180.0; }
		if (angle < -90.0) { angle += 180.0; }
		p.save();
		p.translate(sp);
		p.rotate(angle);
		QPainterPath path;
		path.addText(-fm.horizontalAdvance(kv.second.name) / 2.0, fm.ascent() / 2.0, f, kv.second.name);
		p.setPen(QPen(QColor(255, 255, 255, 220), 3.0));
		p.setBrush(Qt::NoBrush);
		p.drawPath(path);
		p.setPen(Qt::NoPen);
		p.setBrush(app_theme::text_dark());
		p.drawPath(path);
		p.restore();
	}
}

QString world_view::vehicle_state(const sim_vehicle &v, bool html) const
{
	const world_model &m = m_ws->model();
	const road_network &net = m_ws->network();
	// The element that causes the wait: a link in rich text ("vehicle:12")
	auto target = [&](cause_target t, int id) -> QString
	{
		QString text;
		QString kind;
		switch (t)
		{
			case cause_target::vehicle: text = tx("vehicle %1").arg(id); kind = QStringLiteral("vehicle"); break;
			case cause_target::light: text = tx("traffic light %1").arg(id); kind = QStringLiteral("light"); break;
			case cause_target::sign: text = tx("sign %1").arg(id); kind = QStringLiteral("sign"); break;
			case cause_target::pedestrian: text = tx("pedestrian %1").arg(id); kind = QStringLiteral("pedestrian"); break;
			case cause_target::object: text = world_properties::title(m, element_ref(element_kind::object, id)); kind = QStringLiteral("object"); break;
			default: return QString();
		}
		if (id < 0)
		{
			return QString();
		}
		return html ? QStringLiteral("<a href=\"%1:%2\">%3</a>").arg(kind).arg(id).arg(text.toHtmlEscaped()) : text;
	};
	auto esc = [html](const QString &s) { return html ? s.toHtmlEscaped() : s; };
	// "%1" in the text is the link to the element
	auto with = [&](const char *text, cause_target t, int id)
	{
		const QString link = target(t, id);
		const QString base = tx(text);
		const int at = base.indexOf(QStringLiteral("%1"));
		if (at < 0)
		{
			return esc(base);
		}
		return esc(base.left(at)) + (link.isEmpty() ? esc(tx("another element")) : link) + esc(base.mid(at + 2));
	};

	QString state;
	const bool waiting = v.v < 2.0 || v.standstill > 0.0;
	switch (waiting ? v.cause : wait_cause::none)
	{
		case wait_cause::queue: state = with("Waits behind %1", cause_target::vehicle, v.cause_id); break;
		case wait_cause::red_light: state = with("Waits for the green light (%1)", cause_target::light, v.cause_id); break;
		case wait_cause::stop_sign: state = with("Stops at the STOP sign (%1)", cause_target::sign, v.cause_id); break;
		case wait_cause::give_way: state = with("Gives way to %1", cause_target::vehicle, v.cause_id); break;
		case wait_cause::conflict: state = with("Waits: %1 has the junction", cause_target::vehicle, v.cause_id); break;
		case wait_cause::exit_full: state = with("Waits: no space after the junction (%1)", cause_target::vehicle, v.cause_id); break;
		case wait_cause::junction_blocked: state = with("Waits: %1 stands in the junction", cause_target::vehicle, v.cause_id); break;
		case wait_cause::pedestrians: state = with("Waits for the pedestrians on the crosswalk (%1)", cause_target::pedestrian, v.cause_id); break;
		case wait_cause::bus_stop: state = esc(tx("At the bus stop (%1 s)").arg(std::ceil(v.dwell_left))); break;
		case wait_cause::level_crossing: state = with("Waits at the level crossing (%1)", cause_target::object, v.cause_id); break;
		case wait_cause::lane_blocked: state = with("Waits: the lane is closed (%1)", cause_target::object, v.cause_id); break;
		case wait_cause::merge_courtesy: state = with("Lets %1 into the lane", cause_target::vehicle, v.cause_id); break;
		case wait_cause::no_route: state = esc(tx("Waits: no movement to the route from this lane")); break;
		default:
			if (v.on_connector)
			{
				state = esc(tx("In the junction (%1)").arg(turn_phrase(net.connectors[v.seg].turn)));
			}
			else if (v.v < 0.5)
			{
				state = esc(tx("Waits"));
			}
			else
			{
				state = esc(v.lc_progress < 1.0 ? tx("Drives and changes the lane") : tx("Drives"));
			}
			break;
	}
	if (v.standstill >= 2.0 && v.cause != wait_cause::bus_stop)
	{
		state += esc(tx(" (%1 s)").arg(v.standstill, 0, 'f', 0));
	}
	return state;
}

QStringList world_view::popup_lines(const element_ref &ref, vec2 *anchor, QString *title, bool html) const
{
	QStringList out;
	const world_model &m = m_ws->model();
	const road_network &net = m_ws->network();
	const simulation &sim = m_ws->sim();
	*title = world_properties::title(m, ref);
	int rich_line = -1;           // a line that is already rich text
	switch (ref.kind)
	{
		case element_kind::vehicle:
		{
			const sim_vehicle *v = sim.find_vehicle(ref.id);
			if (!v)
			{
				return out;
			}
			*anchor = v->pos;
			*title = QStringLiteral("%1 %2").arg(tx(v->p->name)).arg(v->id);
			double limit = 0.0;
			if (v->on_connector)
			{
				limit = net.lanes[net.connectors[v->seg].to_lane].base_limit;
			}
			else
			{
				limit = net.lanes[v->seg].limit_at(v->s);
			}
			const QString state = vehicle_state(*v, html);
			double left = (v->on_connector ? net.connectors[v->seg].path.length() : net.lanes[v->seg].length()) - v->s;
			for (int k = v->route_pos + 1; k < static_cast<int>(v->route.size()); ++k)
			{
				const std::vector<int> &ls = net.lanes_of(v->route[k] / 2, (v->route[k] % 2 == 0) ? 1 : -1);
				if (!ls.empty())
				{
					left += net.lanes[ls.front()].length() + 12.0;
				}
			}
			const double kmh = v->v * 3.6;
			QString fuel_now;
			if (v->v > 1.0)
			{
				fuel_now = tx("%1 L/100 km").arg(v->fuel_rate / v->v * 100000.0, 0, 'f', 1);
			}
			else
			{
				fuel_now = tx("%1 L/h (stopped)").arg(v->fuel_rate * 3600.0, 0, 'f', 2);
			}
			out << tx("Speed: %1 km/h (limit %2 km/h)").arg(kmh, 0, 'f', 1).arg(limit * 3.6, 0, 'f', 0)
				<< tx("Acceleration: %1 m/s2").arg(v->a, 0, 'f', 2)
				<< tx("Fuel now: %1").arg(fuel_now)
				<< tx("Fuel used: %1 L   CO2: %2 kg").arg(v->fuel_l, 0, 'f', 3).arg(v->co2_kg, 0, 'f', 3)
				<< tx("Passengers: %1").arg(v->passengers)
				<< tx("Route: node %1 to node %2, %3 km left").arg(v->origin_node).arg(v->dest_node).arg(left / 1000.0, 0, 'f', 2)
				<< tx("Trip: %1 km in %2").arg(v->distance / 1000.0, 0, 'f', 2).arg(hms(sim.time() - v->spawn_time))
				<< tx("Stops: %1 (stopped %2 s), congestion %3 s").arg(v->stops).arg(v->stop_time, 0, 'f', 0).arg(v->congested_time, 0, 'f', 0);
			rich_line = out.size();
			out << (html ? tx("State: %1").toHtmlEscaped().arg(state) : tx("State: %1").arg(state));
			break;
		}
		case element_kind::pedestrian:
		{
			const sim_pedestrian *pd = sim.find_pedestrian(ref.id);
			if (!pd)
			{
				return out;
			}
			*anchor = pd->pos;
			QString state = tx("Walks");
			if (pd->waiting)
			{
				state = tx("Waits at the crosswalk (%1 s)").arg(pd->wait_time, 0, 'f', 0);
			}
			else if (pd->on_crosswalk)
			{
				state = tx("Crosses the road");
			}
			out << tx("Speed: %1 km/h").arg(pd->speed * 3.6, 0, 'f', 1)
				<< tx("Route: node %1 to node %2").arg(pd->origin_node).arg(pd->dest_node)
				<< tx("State: %1").arg(state);
			break;
		}
		case element_kind::light:
		{
			const light_data *l = m.light(ref.id);
			const light_runtime *lr = sim.light_state(ref.id);
			if (!l || !lr)
			{
				return out;
			}
			*anchor = m.node(l->node_id) ? m.node(l->node_id)->pos : vec2();
			static const char *stages[] = { "red and yellow", "green", "yellow", "all red" };
			static const char *algos[] = { "Fixed time", "Actuated", "External (API)" };
			double cycle = 0.0;
			for (const light_phase &ph : l->phases)
			{
				cycle += ph.green + ph.yellow + ph.all_red + l->red_yellow;
			}
			out << tx("Algorithm: %1").arg(tx(algos[static_cast<int>(l->algorithm)]))
				<< tx("Phase %1 of %2: %3, %4 s left").arg(lr->phase + 1).arg(l->phases.size())
					.arg(tx(stages[std::max(0, std::min(3, lr->stage))])).arg(std::max(0.0, lr->stage_left), 0, 'f', 0)
				<< tx("Cycle time: %1 s").arg(cycle, 0, 'f', 0);
			int total = 0;
			const net_junction *j = net.junction(l->node_id);
			if (j)
			{
				for (const net_arm &arm : j->arms)
				{
					if (net.lanes_of(arm.road_id, arm.at_a ? -1 : 1).empty())
					{
						continue;
					}
					const signal_color c = sim.signal_for(l->node_id, arm.road_id);
					const int w = sim.waiting_at(l->node_id, arm.road_id);
					total += w;
					out << tx("%1 (%2): %3, %4 s, %5 waiting").arg(road_label(m, arm.road_id), compass_from(arm.angle), color_name(c))
						.arg(std::ceil(sim.signal_remaining(l->node_id, arm.road_id))).arg(w);
				}
			}
			out << tx("Total waiting: %1 vehicles").arg(total);
			break;
		}
		case element_kind::road:
		{
			const road_data *r = m.road(ref.id);
			const net_road *nr = net.road(ref.id);
			if (!r || !nr)
			{
				return out;
			}
			*anchor = nr->center.point_at(nr->center.length() / 2.0);
			int count = 0;
			double sum = 0.0;
			for (const sim_vehicle &v : sim.vehicles())
			{
				if (!v.on_connector && net.lanes[v.seg].road_id == r->id)
				{
					++count;
					sum += v.v;
				}
			}
			double lane_km = 0.0;
			for (int dir : { 1, -1 })
			{
				for (int lid : net.lanes_of(r->id, dir))
				{
					lane_km += net.lanes[lid].length() / 1000.0;
				}
			}
			out << tx("Length: %1 m").arg(nr->center.length(), 0, 'f', 0)
				<< tx("Lanes: A to B %1, B to A %2").arg(r->lanes_forward).arg(r->lanes_backward)
				<< tx("Speed limit: %1 km/h").arg(r->speed_limit, 0, 'f', 0)
				<< tx("Vehicles now: %1").arg(count)
				<< tx("Mean speed: %1 km/h").arg(count ? sum / count * 3.6 : 0.0, 0, 'f', 1)
				<< tx("Density: %1 veh/km").arg(lane_km > 0.0 ? count / lane_km : 0.0, 0, 'f', 1);
			break;
		}
		case element_kind::node:
		{
			const node_data *n = m.node(ref.id);
			if (!n)
			{
				return out;
			}
			*anchor = n->pos;
			const size_t roads = m.roads_at_node(n->id).size();
			out << tx("Roads: %1").arg(roads);
			// Congestion analysis of the junction (time window of the simulation)
			if (const junction_status *js = sim.congestion_at(n->id))
			{
				out << tx("Level of service: %1 (%2 s delay per vehicle)").arg(QString(QLatin1Char(js->los))).arg(js->delay, 0, 'f', 0)
					<< tx("Flow: %1 veh/h").arg(js->flow, 0, 'f', 0)
					<< tx("Queue now: %1 m (%2 vehicles)").arg(js->queue_m, 0, 'f', 0).arg(js->waiting);
				if (js->cause != wait_cause::none)
				{
					out << tx("Main cause: %1 (%2%)").arg(congestion_dock::cause_text(js->cause)).arg(js->cause_share * 100.0, 0, 'f', 0);
				}
				if (js->spillback)
				{
					out << tx("Spillback: a queue fills its road and blocks the junction before it");
				}
			}
			if (roads == 1)
			{
				const double prof = n->daily_profile ? simulation::demand_profile(sim.clock()) : 1.0;
				out << tx("Traffic source: %1").arg(n->spawn ? tx("yes") : tx("no"))
					<< tx("Demand now: %1 veh/h, %2 ped/h")
						.arg(n->vehicles_per_hour * prof * sim.vehicle_demand_factor(), 0, 'f', 0)
						.arg(n->pedestrians_per_hour * prof * sim.pedestrian_demand_factor(), 0, 'f', 0);
				if (sim.vehicle_demand_factor() != 1.0 || sim.pedestrian_demand_factor() != 1.0)
				{
					out << tx("Global traffic density: vehicles %1%, pedestrians %2%")
						.arg(sim.vehicle_demand_factor() * 100.0, 0, 'f', 0).arg(sim.pedestrian_demand_factor() * 100.0, 0, 'f', 0);
				}
			}
			const int lid = m.light_at_node(n->id);
			if (lid >= 0)
			{
				out << tx("Traffic light: %1").arg(lid);
			}
			break;
		}
		case element_kind::sign:
		{
			const sign_data *s = m.sign(ref.id);
			const sign_def *d = s ? sign_catalog::instance().find(s->key) : nullptr;
			vec2 dir;
			if (!s || !world_layout::sign_pose(m, net, *s, anchor, &dir))
			{
				return out;
			}
			out << tx("Code: %1").arg(d && !d->code.isEmpty() ? d->code : tx("(not confirmed yet)"))
				<< (i18n::language() == QLatin1String("tr") ? tx("English name: %1").arg(d ? d->name : QString()) : tx("Turkish name: %1").arg(d ? d->name_tr : QString()))
				<< tx("Effect: %1").arg(d ? sign_catalog::effect_text(d->effect) : QString())
				<< tx("For: traffic from %1").arg(s->dir > 0 ? tx("A to B") : tx("B to A"));
			if (d && d->effect == sign_effect::speed_limit)
			{
				out << tx("Value: %1 km/h").arg(s->value > 0 ? s->value : d->default_value);
			}
			break;
		}
		case element_kind::object:
		{
			const object_data *o = m.object(ref.id);
			vec2 dir;
			double hl = 0.0;
			if (!o || !world_layout::object_pose(m, net, *o, anchor, &dir, &hl))
			{
				return out;
			}
			switch (o->kind)
			{
				case object_kind::level_crossing:
				{
					static const char *states[] = { "open", "barriers go down", "closed (train)", "barriers go up" };
					const crossing_state st = sim.level_crossing(o->id, nullptr, nullptr);
					const double period = std::max(60.0, o->train_period);
					double ph = std::fmod(sim.time() + o->train_offset, period);
					if (ph < 0.0) { ph += period; }
					out << tx("State: %1").arg(tx(states[static_cast<int>(st)]))
						<< tx("Next train in: %1 s").arg(period - ph, 0, 'f', 0)
						<< tx("Barrier down time: %1 s").arg(o->closed_time, 0, 'f', 0);
					break;
				}
				case object_kind::bus_stop:
					out << tx("Name: %1").arg(o->name)
						<< tx("Waiting passengers: %1").arg(sim.bus_stop_waiting(o->id))
						<< tx("Stop time of a bus: %1 s").arg(o->dwell_time, 0, 'f', 0);
					break;
				case object_kind::cone:
				case object_kind::barrier:
					out << tx("Blocked lane: %1").arg(o->lane < 0 ? tx("all") : QString::number(o->lane))
						<< tx("Blocked length: %1 m").arg(o->length, 0, 'f', 0);
					break;
				case object_kind::road_closure:
					out << tx("Closed for: %1").arg(o->dir == 0 ? tx("both directions") : (o->dir > 0 ? tx("traffic from A to B") : tx("traffic from B to A")));
					break;
			}
			break;
		}
		default:
			break;
	}
	if (html)
	{
		for (int i = 0; i < out.size(); ++i)
		{
			if (i != rich_line)
			{
				out[i] = out[i].toHtmlEscaped();
			}
		}
	}
	return out;
}

void world_view::paint_popup(QPainter &p)
{
	const element_ref sel = m_ws->selection();
	if (!sel.valid())
	{
		return;
	}
	vec2 anchor;
	QString title;
	QStringList lines = popup_lines(sel, &anchor, &title);
	const std::vector<int> roads = m_ws->selected_roads();
	if (roads.size() > 1 && !lines.isEmpty())
	{
		// More selected roads: a summary at the main road
		const world_model &m = m_ws->model();
		const road_network &net = m_ws->network();
		double length = 0.0;
		for (int id : roads)
		{
			if (const road_data *r = m.road(id))
			{
				length += m.road_centerline(*r).length();
			}
		}
		int count = 0;
		for (const sim_vehicle &v : m_ws->sim().vehicles())
		{
			if (!v.on_connector && std::find(roads.begin(), roads.end(), net.lanes[v.seg].road_id) != roads.end())
			{
				++count;
			}
		}
		title = tx("%1 roads selected").arg(roads.size());
		lines = QStringList() << tx("Total length: %1 m").arg(length, 0, 'f', 0) << tx("Vehicles now: %1").arg(count);
	}
	if (lines.isEmpty())
	{
		return;
	}
	QFont tf = font();
	tf.setPointSizeF(9.5);
	tf.setBold(true);
	QFont bf = font();
	bf.setPointSizeF(9.0);
	const QFontMetrics tm(tf);
	const QFontMetrics bm(bf);
	double w = tm.horizontalAdvance(title);
	for (const QString &l : lines)
	{
		w = std::max(w, static_cast<double>(bm.horizontalAdvance(l)));
	}
	w += 24.0;
	const double h = tm.height() + 10.0 + lines.size() * (bm.height() + 2.0) + 12.0;
	const QPointF a = m_cam.to_screen(anchor);
	QPointF tl(a.x() + 28.0, a.y() - h - 18.0);
	tl.setX(std::max(8.0, std::min(tl.x(), width() - w - 8.0)));
	tl.setY(std::max(36.0, std::min(tl.y(), height() - h - 40.0)));
	const QRectF r(tl, QSizeF(w, h));
	p.setPen(QPen(QColor(120, 170, 225), 1.5));
	const QPointF corner(r.left() < a.x() ? std::min(a.x(), r.right()) : r.left(), r.bottom() < a.y() ? r.bottom() : r.top());
	p.drawLine(a, QPointF(std::max(r.left(), std::min(a.x(), r.right())), r.bottom() < a.y() ? r.bottom() : r.top()));
	Q_UNUSED(corner);
	p.setBrush(QColor(120, 170, 225));
	p.drawEllipse(a, 3.0, 3.0);
	draw_panel(p, r, QColor(167, 199, 231));
	QRectF head(r.left(), r.top(), r.width(), tm.height() + 8.0);
	QPainterPath hp;
	hp.addRoundedRect(head, 10, 10);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(220, 234, 248));
	p.drawPath(hp);
	p.setPen(app_theme::text_dark());
	p.setFont(tf);
	p.drawText(head.adjusted(12, 0, -8, 0), Qt::AlignVCenter | Qt::AlignLeft, title);
	p.setFont(bf);
	double y = head.bottom() + 6.0;
	for (const QString &l : lines)
	{
		p.drawText(QRectF(r.left() + 12.0, y, r.width() - 16.0, bm.height()), Qt::AlignLeft | Qt::AlignVCenter, l);
		y += bm.height() + 2.0;
	}
}

void world_view::paint_dev(QPainter &p)
{
	const simulation &sim = m_ws->sim();
	const road_network &net = m_ws->network();
	const logger &log = logger::instance();
	QStringList lines;
	lines << QStringLiteral("FPS: %1 (frame %2 ms)").arg(m_fps, 0, 'f', 1).arg(m_frame_ms, 0, 'f', 2)
		<< QStringLiteral("Simulation: %1 ms/step, step %2 s").arg(sim.last_step_ms(), 0, 'f', 3).arg(sim.time_step(), 0, 'f', 2)
		<< QStringLiteral("Speed: x%1 (real x%2)").arg(m_ws->speed(), 0, 'f', 1).arg(m_ws->flowing() ? sim.actual_speed() : 0.0, 0, 'f', 1)
		<< QStringLiteral("Vehicles: %1, pedestrians: %2, done: %3, removed (gridlock): %4").arg(sim.vehicles().size())
			.arg(sim.pedestrians().size()).arg(sim.finished_vehicles()).arg(sim.gridlock_removed())
		<< QStringLiteral("Demand: profile %1, density vehicles %2%, pedestrians %3%").arg(simulation::demand_profile(sim.clock()), 0, 'f', 2)
			.arg(sim.vehicle_demand_factor() * 100.0, 0, 'f', 0).arg(sim.pedestrian_demand_factor() * 100.0, 0, 'f', 0)
		<< QStringLiteral("Network: %1 lanes, %2 connectors, %3 junctions").arg(net.lanes.size()).arg(net.connectors.size()).arg(net.junctions.size())
		<< QStringLiteral("Vertices: static %1, dynamic %2").arg(m_renderer.static_vertices()).arg(m_renderer.dynamic_vertices())
		<< QStringLiteral("Camera: %1 px/m, center %2, %3").arg(m_cam.ppm, 0, 'f', 2).arg(m_cam.center.x, 0, 'f', 0).arg(m_cam.center.y, 0, 'f', 0)
		<< (m_ws->model().has_geo()
			? QStringLiteral("Map: %1, zoom %2, %3 textures, %4 loading").arg(m_renderer.map_drawn() ? QStringLiteral("on") : QStringLiteral("off"))
				.arg(m_renderer.map_zoom()).arg(m_renderer.map_tiles()).arg(tile_cache::instance().pending())
			: QStringLiteral("Map: no geographic origin"))
		<< QStringLiteral("Log: %1, queue %2, lost %3").arg(log.enabled() ? logger::level_name(log.min_level()) : QStringLiteral("off"))
			.arg(log.queue_size()).arg(log.dropped_count())
		<< QStringLiteral("Memory: %1 MB").arg(working_set_mb(), 0, 'f', 0)
		<< m_renderer.gl_info();
	QFont f(QStringLiteral("Consolas"));
	f.setPointSizeF(8.5);
	p.setFont(f);
	const QFontMetrics fm(f);
	double w = 0.0;
	for (const QString &l : lines)
	{
		w = std::max(w, static_cast<double>(fm.horizontalAdvance(l)));
	}
	w += 20.0;
	const double h = lines.size() * (fm.height() + 1.0) + 14.0;
	const QRectF r(width() - w - 10.0, 10.0, w, h);
	p.setPen(QPen(QColor(213, 198, 242), 1.2));
	p.setBrush(QColor(40, 46, 58, 215));
	p.drawRoundedRect(r, 10, 10);
	double y = r.top() + 7.0;
	for (int i = 0; i < lines.size(); ++i)
	{
		p.setPen(i == 0 ? QColor(168, 230, 207) : QColor(235, 238, 245));
		p.drawText(QRectF(r.left() + 10.0, y, w - 14.0, fm.height()), Qt::AlignLeft | Qt::AlignVCenter, lines.at(i));
		y += fm.height() + 1.0;
	}
}

void world_view::paint_hud(QPainter &p)
{
	const simulation &sim = m_ws->sim();
	// clock box (top left)
	QFont f = font();
	f.setPointSizeF(10.0);
	f.setBold(true);
	p.setFont(f);
	const QFontMetrics fm(f);
	QString state = m_ws->editor_mode() ? tx("Editor") : (m_ws->running() ? tx("Run") : tx("Pause"));
	const QString text = QStringLiteral("%1   x%2   %3").arg(hms(sim.clock())).arg(m_ws->speed(), 0, 'g', 3).arg(state);
	const QRectF cr(10, 10, fm.horizontalAdvance(text) + 24.0, fm.height() + 12.0);
	draw_panel(p, cr, QColor(168, 230, 207));
	p.setPen(app_theme::text_dark());
	p.drawText(cr, Qt::AlignCenter, text);

	// editor banner (top center)
	if (m_ws->editor_mode())
	{
		const QString b = tx("EDITOR MODE - THE FLOW IS STOPPED");
		const double bw = fm.horizontalAdvance(b) + 30.0;
		const QRectF br((width() - bw) / 2.0, 10, bw, fm.height() + 12.0);
		p.setPen(QPen(QColor(240, 170, 110), 1.2));
		p.setBrush(QColor(255, 211, 182, 240));
		p.drawRoundedRect(br, 10, 10);
		p.setPen(QColor(120, 70, 40));
		p.drawText(br, Qt::AlignCenter, b);
	}

	// scale bar (bottom left)
	const double target_px = 120.0;
	const double meters = target_px / m_cam.ppm;
	const double mag = std::pow(10.0, std::floor(std::log10(meters)));
	double nice = mag;
	for (double k : { 1.0, 2.0, 5.0, 10.0 })
	{
		if (k * mag <= meters)
		{
			nice = k * mag;
		}
	}
	const double bar_px = nice * m_cam.ppm;
	QFont sf = font();
	sf.setPointSizeF(8.5);
	p.setFont(sf);
	const QString label = nice >= 1000.0 ? QStringLiteral("%1 km").arg(nice / 1000.0) : QStringLiteral("%1 m").arg(nice);
	const QRectF sb(10, height() - 62, bar_px + 70.0, 22);
	draw_panel(p, sb, QColor(220, 225, 232));
	p.setPen(QPen(app_theme::text_dark(), 2.0));
	const double y = sb.center().y() + 3.0;
	p.drawLine(QPointF(sb.left() + 8.0, y), QPointF(sb.left() + 8.0 + bar_px, y));
	p.drawLine(QPointF(sb.left() + 8.0, y - 4.0), QPointF(sb.left() + 8.0, y + 1.0));
	p.drawLine(QPointF(sb.left() + 8.0 + bar_px, y - 4.0), QPointF(sb.left() + 8.0 + bar_px, y + 1.0));
	p.drawText(QRectF(sb.left() + bar_px + 14.0, sb.top(), 60.0, sb.height()), Qt::AlignVCenter | Qt::AlignLeft, label);

	// legend of the density map (above the scale bar)
	if (m_renderer.density_drawn())
	{
		struct item
		{
			const char *text;
			QColor color;
		};
		const item items[] =
		{
			{ "Free", QColor(120, 200, 120) }, { "Slow", QColor(250, 210, 90) },
			{ "Heavy", QColor(242, 145, 80) }, { "Stopped", QColor(214, 70, 70) }
		};
		const QFontMetrics lm(sf);
		const QString title = tx("Traffic:");
		double lw = lm.horizontalAdvance(title) + 22.0;
		for (const item &it : items)
		{
			lw += 22.0 + lm.horizontalAdvance(tx(it.text));
		}
		const QRectF lr(10, height() - 90, lw, 22);
		draw_panel(p, lr, QColor(220, 225, 232));
		p.setPen(app_theme::text_dark());
		double x = lr.left() + 8.0;
		p.drawText(QRectF(x, lr.top(), 80.0, lr.height()), Qt::AlignVCenter | Qt::AlignLeft, title);
		x += lm.horizontalAdvance(title) + 10.0;
		for (const item &it : items)
		{
			p.setPen(Qt::NoPen);
			p.setBrush(it.color);
			p.drawRoundedRect(QRectF(x, lr.center().y() - 4.0, 16.0, 8.0), 4.0, 4.0);
			x += 20.0;
			p.setPen(app_theme::text_dark());
			const QString t = tx(it.text);
			p.drawText(QRectF(x, lr.top(), 80.0, lr.height()), Qt::AlignVCenter | Qt::AlignLeft, t);
			x += lm.horizontalAdvance(t) + 6.0;
		}
	}

	// congestion analysis: labels at the hotspots and the legend of the level of service
	if (m_congestion)
	{
		QFont lf = font();
		lf.setPointSizeF(8.5);
		lf.setBold(true);
		p.setFont(lf);
		const QFontMetrics fm(lf);
		int shown = 0;
		for (const junction_status &js : m_ws->sim().congestion())
		{
			if (js.los < 'D' && !js.spillback)
			{
				break; // the list is sorted: the worst first
			}
			const net_junction *j = m_ws->network().junction(js.node_id);
			if (!j || ++shown > 40)
			{
				continue;
			}
			const QPointF c = m_cam.to_screen(j->pos);
			const QString text = tx("LOS %1  %2 s").arg(QString(QLatin1Char(js.los))).arg(js.delay, 0, 'f', 0)
				+ (js.spillback ? QStringLiteral("  ") + tx("spillback") : QString());
			const double w = fm.horizontalAdvance(text) + 14.0;
			const QRectF r(c.x() - w / 2.0, c.y() - std::max(26.0, (j->radius + 6.0) * m_cam.ppm) - 22.0, w, 20.0);
			if (!rect().adjusted(-50, -50, 50, 50).intersects(r.toRect()))
			{
				continue;
			}
			p.setPen(Qt::NoPen);
			p.setBrush(app_theme::los_color(js.los));
			p.drawRoundedRect(r, 7, 7);
			p.setPen(js.los >= 'E' ? QColor(255, 255, 255) : QColor(30, 34, 40));
			p.drawText(r, Qt::AlignCenter, text);
		}
		p.setFont(sf);
		const QFontMetrics lm(sf);
		const QString title = tx("Level of service:");
		double lw = lm.horizontalAdvance(title) + 20.0 + 6 * 24.0 + lm.horizontalAdvance(tx("Queue")) + 40.0;
		const QRectF lr(10, height() - (m_renderer.density_drawn() ? 118 : 90), lw, 22);
		draw_panel(p, lr, QColor(220, 225, 232));
		p.setPen(app_theme::text_dark());
		double x = lr.left() + 8.0;
		p.drawText(QRectF(x, lr.top(), 160.0, lr.height()), Qt::AlignVCenter | Qt::AlignLeft, title);
		x += lm.horizontalAdvance(title) + 8.0;
		for (char los = 'A'; los <= 'F'; ++los)
		{
			const QRectF chip(x, lr.center().y() - 7.0, 20.0, 14.0);
			p.setPen(Qt::NoPen);
			p.setBrush(app_theme::los_color(los));
			p.drawRoundedRect(chip, 4.0, 4.0);
			p.setPen(los >= 'E' ? QColor(255, 255, 255) : QColor(30, 34, 40));
			p.drawText(chip, Qt::AlignCenter, QString(QLatin1Char(los)));
			x += 24.0;
		}
		x += 6.0;
		p.setPen(Qt::NoPen);
		p.setBrush(app_theme::los_color('E'));
		p.drawRoundedRect(QRectF(x, lr.center().y() - 3.0, 18.0, 6.0), 3.0, 3.0);
		p.setPen(app_theme::text_dark());
		p.drawText(QRectF(x + 22.0, lr.top(), 80.0, lr.height()), Qt::AlignVCenter | Qt::AlignLeft, tx("Queue"));
	}

	// attribution of the map data (bottom right), required by the OpenStreetMap license
	if (m_renderer.map_drawn())
	{
		const QString att = tx("Map data © OpenStreetMap contributors");
		const QFontMetrics am(sf);
		const double aw = am.horizontalAdvance(att) + 16.0;
		const QRectF ar(width() - aw - 10.0, height() - 62, aw, 22);
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(255, 255, 255, 200));
		p.drawRoundedRect(ar, 6, 6);
		p.setPen(QColor(70, 80, 95));
		p.drawText(ar, Qt::AlignCenter, att);
	}

	// tool hint (bottom)
	const QString hint = tx(tool_hint(m_tool));
	const QFontMetrics hm(sf);
	const double hw = std::min(static_cast<double>(width()) - 20.0, static_cast<double>(hm.horizontalAdvance(hint)) + 20.0);
	const QRectF hr(10, height() - 34, hw, 24);
	p.setPen(QPen(QColor(220, 225, 232), 1.0));
	p.setBrush(QColor(255, 255, 255, 215));
	p.drawRoundedRect(hr, 8, 8);
	p.setPen(QColor(70, 80, 95));
	p.drawText(hr.adjusted(10, 0, -6, 0), Qt::AlignVCenter | Qt::AlignLeft, hm.elidedText(hint, Qt::ElideRight, static_cast<int>(hw - 16)));
}
