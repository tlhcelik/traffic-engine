#ifndef WORLD_VIEW_H
#define WORLD_VIEW_H

#include "world_renderer.h"
#include "world_properties.h"

#include <QElapsedTimer>
#include <QOpenGLWidget>
#include <QStringList>

class QOpenGLDebugLogger;
class workspace;
struct sim_vehicle;

// world_view
// ----------
// The OpenGL view of one workspace (one tab) and the editor tools.
//
// Mouse: wheel = zoom at the cursor; drag (left, middle or right button) =
// move the map; click = action of the current tool. In editor mode, a
// drag on a node moves the node; a drag on a control point of the selected
// road changes the road shape (Ctrl+click on the road adds a point).
//
// Overlays (QPainter on the OpenGL frame): countdowns of the traffic lights,
// road names, the popup of the selected element (live data), the developer
// details (FPS, timings) at the top right, the clock, the editor banner, a
// scale bar, the hint of the current tool, the legend of the density map
// and the attribution of the map data.

class world_view : public QOpenGLWidget
{
	Q_OBJECT

public:
	enum tool_t
	{
		tool_select = 0,
		tool_road,
		tool_light,
		tool_sign,
		tool_cone,
		tool_barrier,
		tool_closure,
		tool_crossing,
		tool_bus_stop,
		tool_spawn,
		tool_erase,
		tool_bridge,        // a road on level 1 (the tools after tool_erase keep F1 .. F11 the same)
		tool_tunnel,        // a road on level -1
		tool_count
	};

	explicit world_view(workspace *ws, QWidget *parent = nullptr);
	~world_view() override;

	workspace *ws() const { return m_ws; }

	void set_tool(tool_t t);
	tool_t tool() const { return m_tool; }
	static QString tool_name(int t);
	static QString tool_icon(int t);
	static QString tool_hint(int t);
	// The road, bridge and tunnel tools draw a road with points
	bool drawing_tool() const { return m_tool == tool_road || m_tool == tool_bridge || m_tool == tool_tunnel; }

	void set_sign_key(const QString &key) { m_sign_key = key; }
	QString sign_key() const { return m_sign_key; }

	void set_dev_overlay(bool on) { m_dev = on; update(); }
	bool dev_overlay() const { return m_dev; }
	void set_show_countdowns(bool on) { m_countdowns = on; update(); }
	void set_show_labels(bool on) { m_labels = on; update(); }
	void set_show_grid(bool on) { m_grid = on; update(); }
	void set_show_density(bool on) { m_density = on; update(); }
	// Congestion analysis: level of service of the junctions, queues, labels
	void set_show_congestion(bool on) { m_congestion = on; update(); }
	// Map tiles under the world (only for a workspace with a geographic origin).
	void set_show_map(bool on) { m_map = on; update(); }
	void set_follow(bool on);
	bool follow() const { return m_follow; }

	void zoom_to_fit();
	void zoom_by(double factor, const QPointF &anchor);
	void center_on(const vec2 &p);
	double fps() const { return m_fps; }
	const camera &cam() const { return m_cam; }
	QImage snapshot();

	// Properties of the new roads of the road tool.
	static road_data &road_template();

	// Live information lines of an element (also used by the property panel).
	// html: the lines are rich text; the state of a vehicle has a link to the
	// element that causes the wait ("vehicle:12", "light:3", "object:7", ...).
	QStringList popup_lines(const element_ref &ref, vec2 *anchor, QString *title, bool html = false) const;
	// Text of the state of a vehicle (why it waits, with a link in rich text).
	QString vehicle_state(const sim_vehicle &v, bool html) const;

signals:
	void status_message(const QString &text);
	void tool_changed(int tool);
	void model_edited();

protected:
	void initializeGL() override;
	void resizeGL(int w, int h) override;
	void paintGL() override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void keyPressEvent(QKeyEvent *e) override;

private:
	enum drag_t
	{
		drag_none,
		drag_pan,
		drag_node,
		drag_handle
	};

	struct snap_result
	{
		vec2 point;
		int node = -1;
		int road = -1;
		double road_s = 0.0;
		bool snapped = false;
	};

	vec2 world_at(const QPointF &pos) const { return m_cam.to_world(pos); }
	double px(double pixels) const { return pixels / m_cam.ppm; }
	bool require_editor();
	void click(const vec2 &w, Qt::KeyboardModifiers mods);
	void road_click(const vec2 &w, Qt::KeyboardModifiers mods);
	snap_result snap_road_point(const vec2 &w, Qt::KeyboardModifiers mods) const;
	void finish_road();
	void cancel_road();
	void place_light(const vec2 &w);
	void place_sign(const vec2 &w);
	void place_object(int kind, const vec2 &w);
	void erase_at(const vec2 &w);
	void delete_selection();
	void edited(property_change change, const QString &action);
	std::vector<vec2> selected_road_handles() const;

	void paint_countdowns(QPainter &p);
	void paint_labels(QPainter &p);
	void paint_popup(QPainter &p);
	void paint_dev(QPainter &p);
	void paint_hud(QPainter &p);

	workspace *m_ws;
	world_renderer m_renderer;
	camera m_cam;
	bool m_gl_ok;
	QString m_gl_error;
	tool_t m_tool;
	QString m_sign_key;

	// road tool
	bool m_drawing;
	std::vector<vec2> m_points;
	snap_result m_start;
	snap_result m_end;
	vec2 m_cursor;
	snap_result m_hover_snap;

	// mouse
	drag_t m_drag;
	QPoint m_press_pos;
	QPoint m_last_pos;
	bool m_moved;
	int m_drag_id;
	int m_drag_index;

	// options
	bool m_dev;
	bool m_countdowns;
	bool m_labels;
	bool m_grid;
	bool m_density;
	bool m_congestion = false;
	bool m_map;
	bool m_follow;
	bool m_first_fit;

	// frame timing
	QElapsedTimer m_anim;
	QElapsedTimer m_fps_timer;
	int m_frames;
	double m_fps;
	double m_frame_ms;
	QOpenGLDebugLogger *m_gl_logger;
};

#endif // WORLD_VIEW_H
