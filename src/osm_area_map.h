#ifndef OSM_AREA_MAP_H
#define OSM_AREA_MAP_H

#include "osm_import.h"

#include <QPointF>
#include <QWidget>

// osm_area_map
// ------------
// A live map (OpenStreetMap tiles from tile_cache) in the import dialog. The
// user selects the area of the import on it. Mouse:
//   drag                           move the map
//   mouse wheel                    zoom in and out at the cursor
//   Shift + drag, right drag       draw a new area
//   drag in the area               move the area
//   drag an edge or a corner       change the size of the area
//   double-click                   the area moves to this point (same size)
// The area is a rectangle in Web Mercator (as the tiles). area() gives the
// geographic bounds. The size of the area is 100 m to max_size_m on each side.

class osm_area_map : public QWidget
{
	Q_OBJECT

public:
	static const double max_size_m;
	static const double min_size_m;

	explicit osm_area_map(QWidget *parent = nullptr);

	// Sets the area (no signal). fit: the view shows the full area.
	void set_area(const osm_bounds &b, bool fit);
	osm_bounds area() const;
	// Size of the area in meters (at its center).
	double area_width_m() const;
	double area_height_m() const;
	// Moves the view so that the full area is visible.
	void fit_area();

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

signals:
	// The user changed the area (also during the drag: live update).
	void area_changed(const osm_bounds &b);

protected:
	void paintEvent(QPaintEvent *e) override;
	void mousePressEvent(QMouseEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void mouseReleaseEvent(QMouseEvent *e) override;
	void mouseDoubleClickEvent(QMouseEvent *e) override;
	void wheelEvent(QWheelEvent *e) override;
	void resizeEvent(QResizeEvent *e) override;

private:
	enum drag_t
	{
		drag_none,
		drag_pan,
		drag_draw,
		drag_move,
		drag_resize
	};
	enum edge_t
	{
		edge_left = 1,
		edge_right = 2,
		edge_top = 4,
		edge_bottom = 8
	};

	double mpp() const;   // Mercator meters per logical pixel
	QPointF to_screen(double mx, double my) const;
	void to_mercator(const QPointF &p, double *mx, double *my) const;
	QRectF area_rect() const;
	int edges_at(const QPointF &p) const;
	void update_cursor(const QPointF &p);
	// Limits the size (min / max meters), keeps the center or the fixed edges.
	void limit_area(int moved_edges);
	void zoom_at(double steps, const QPointF &p);

	double m_cx;          // view center (Mercator meters)
	double m_cy;
	double m_zoom;        // zoom level (fractional; 256-pixel tiles)
	double m_ax0;         // area (Mercator meters), x0 < x1, y0 < y1 (y to the north)
	double m_ay0;
	double m_ax1;
	double m_ay1;
	drag_t m_drag;
	bool m_fit_pending;   // fit the area again when the widget gets its real size
	int m_edges;
	QPointF m_last;
	double m_start_x;
	double m_start_y;
};

#endif // OSM_AREA_MAP_H
