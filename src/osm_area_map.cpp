#include "osm_area_map.h"
#include "config_common.h"
#include "geo.h"
#include "i18n.h"
#include "tile_cache.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace
{
	const double deg = M_PI / 180.0;
	const double world = 2.0 * M_PI * geo_projection::earth_radius; // edge of the Mercator square
	const double handle_px = 7.0;

	double lat_of(double my)
	{
		return (2.0 * std::atan(std::exp(my / geo_projection::earth_radius)) - M_PI / 2.0) / deg;
	}

	double lon_of(double mx)
	{
		return mx / geo_projection::earth_radius / deg;
	}

	// Ground meters for one Mercator meter at this Mercator y
	double ground_scale(double my)
	{
		return std::cos(lat_of(my) * deg);
	}

	// Limits the width hi - lo; the moved side changes (both: the center stays).
	void limit_span(double &lo, double &hi, bool move_lo, bool move_hi, double min_w, double max_w)
	{
		const double w = hi - lo;
		const double target = std::max(min_w, std::min(max_w, w));
		if (std::abs(target - w) < 1e-9)
		{
			return;
		}
		if (move_lo && !move_hi)
		{
			lo = hi - target;
		}
		else if (move_hi && !move_lo)
		{
			hi = lo + target;
		}
		else
		{
			const double c = (lo + hi) / 2.0;
			lo = c - target / 2.0;
			hi = c + target / 2.0;
		}
	}
}

const double osm_area_map::max_size_m = 5000.0;
const double osm_area_map::min_size_m = 100.0;

osm_area_map::osm_area_map(QWidget *parent)
	: QWidget(parent)
	, m_cx(0.0)
	, m_cy(0.0)
	, m_zoom(15.0)
	, m_ax0(0.0)
	, m_ay0(0.0)
	, m_ax1(0.0)
	, m_ay1(0.0)
	, m_drag(drag_none)
	, m_fit_pending(false)
	, m_edges(0)
	, m_start_x(0.0)
	, m_start_y(0.0)
{
	setMouseTracking(true);
	setFocusPolicy(Qt::StrongFocus);
	setAttribute(Qt::WA_OpaquePaintEvent);
	setCursor(Qt::OpenHandCursor);
	connect(&tile_cache::instance(), &tile_cache::tile_ready, this, [this]() { update(); });
}

QSize osm_area_map::sizeHint() const
{
	return QSize(640, 520);
}

QSize osm_area_map::minimumSizeHint() const
{
	return QSize(360, 300);
}

double osm_area_map::mpp() const
{
	return world / (256.0 * std::pow(2.0, m_zoom));
}

QPointF osm_area_map::to_screen(double mx, double my) const
{
	return QPointF(width() / 2.0 + (mx - m_cx) / mpp(), height() / 2.0 - (my - m_cy) / mpp());
}

void osm_area_map::to_mercator(const QPointF &p, double *mx, double *my) const
{
	*mx = m_cx + (p.x() - width() / 2.0) * mpp();
	*my = m_cy - (p.y() - height() / 2.0) * mpp();
}

QRectF osm_area_map::area_rect() const
{
	return QRectF(to_screen(m_ax0, m_ay1), to_screen(m_ax1, m_ay0)).normalized();
}

void osm_area_map::set_area(const osm_bounds &b, bool fit)
{
	m_ax0 = geo_projection::mercator_x(b.min_lon);
	m_ax1 = geo_projection::mercator_x(b.max_lon);
	m_ay0 = geo_projection::mercator_y(b.min_lat);
	m_ay1 = geo_projection::mercator_y(b.max_lat);
	if (m_ax1 < m_ax0)
	{
		std::swap(m_ax0, m_ax1);
	}
	if (m_ay1 < m_ay0)
	{
		std::swap(m_ay0, m_ay1);
	}
	if (fit)
	{
		fit_area();
		m_fit_pending = !isVisible();
	}
	update();
}

void osm_area_map::resizeEvent(QResizeEvent *e)
{
	QWidget::resizeEvent(e);
	if (m_fit_pending && isVisible())
	{
		m_fit_pending = false;
		fit_area();
	}
}

osm_bounds osm_area_map::area() const
{
	osm_bounds b;
	b.min_lat = lat_of(m_ay0);
	b.max_lat = lat_of(m_ay1);
	b.min_lon = lon_of(m_ax0);
	b.max_lon = lon_of(m_ax1);
	return b;
}

double osm_area_map::area_width_m() const
{
	return (m_ax1 - m_ax0) * ground_scale((m_ay0 + m_ay1) / 2.0);
}

double osm_area_map::area_height_m() const
{
	return (m_ay1 - m_ay0) * ground_scale((m_ay0 + m_ay1) / 2.0);
}

void osm_area_map::fit_area()
{
	m_cx = (m_ax0 + m_ax1) / 2.0;
	m_cy = (m_ay0 + m_ay1) / 2.0;
	// Before the first show the widget has no real size: use the size hint.
	const double w = std::max(200, isVisible() ? width() : sizeHint().width());
	const double h = std::max(200, isVisible() ? height() : sizeHint().height());
	const double need = std::max((m_ax1 - m_ax0) / (w * 0.72), (m_ay1 - m_ay0) / (h * 0.72));
	if (need > 0.0)
	{
		m_zoom = std::max(2.0, std::min(19.5, std::log2(world / (256.0 * need))));
	}
	update();
}

int osm_area_map::edges_at(const QPointF &p) const
{
	const QRectF r = area_rect();
	if (!r.adjusted(-handle_px, -handle_px, handle_px, handle_px).contains(p))
	{
		return 0;
	}
	int e = 0;
	if (std::abs(p.x() - r.left()) <= handle_px)
	{
		e |= edge_left;
	}
	else if (std::abs(p.x() - r.right()) <= handle_px)
	{
		e |= edge_right;
	}
	if (std::abs(p.y() - r.top()) <= handle_px)
	{
		e |= edge_top;
	}
	else if (std::abs(p.y() - r.bottom()) <= handle_px)
	{
		e |= edge_bottom;
	}
	return e;
}

void osm_area_map::update_cursor(const QPointF &p)
{
	const int e = edges_at(p);
	if ((e == (edge_left | edge_top)) || (e == (edge_right | edge_bottom)))
	{
		setCursor(Qt::SizeFDiagCursor);
	}
	else if ((e == (edge_right | edge_top)) || (e == (edge_left | edge_bottom)))
	{
		setCursor(Qt::SizeBDiagCursor);
	}
	else if (e == edge_left || e == edge_right)
	{
		setCursor(Qt::SizeHorCursor);
	}
	else if (e == edge_top || e == edge_bottom)
	{
		setCursor(Qt::SizeVerCursor);
	}
	else if (area_rect().contains(p))
	{
		setCursor(Qt::SizeAllCursor);
	}
	else
	{
		setCursor(Qt::OpenHandCursor);
	}
}

void osm_area_map::limit_area(int moved_edges)
{
	const double k = std::max(0.05, ground_scale((m_ay0 + m_ay1) / 2.0));
	const double min_w = min_size_m / k;
	const double max_w = max_size_m / k;
	limit_span(m_ax0, m_ax1, (moved_edges & edge_left) != 0, (moved_edges & edge_right) != 0, min_w, max_w);
	limit_span(m_ay0, m_ay1, (moved_edges & edge_bottom) != 0, (moved_edges & edge_top) != 0, min_w, max_w);
}

void osm_area_map::zoom_at(double steps, const QPointF &p)
{
	double mx = 0.0;
	double my = 0.0;
	to_mercator(p, &mx, &my);
	m_zoom = std::max(2.0, std::min(20.0, m_zoom + steps));
	m_cx = mx - (p.x() - width() / 2.0) * mpp();
	m_cy = my + (p.y() - height() / 2.0) * mpp();
	update();
}

void osm_area_map::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), QColor(232, 236, 241));
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);

	// --- tiles (a lower zoom tile while a tile loads) ---
	const int max_zoom = qBound(1, config_common::instance().int_value(QStringLiteral("render/map_max_zoom")), 22);
	const int z = std::max(0, std::min(max_zoom, static_cast<int>(std::lround(m_zoom + std::log2(std::max(1.0, devicePixelRatioF()))))));
	double x0 = 0.0;
	double y0 = 0.0;
	double x1 = 0.0;
	double y1 = 0.0;
	to_mercator(QPointF(0, height()), &x0, &y0);
	to_mercator(QPointF(width(), 0), &x1, &y1);
	const int tx0 = geo_projection::tile_x(x0, z);
	const int tx1 = geo_projection::tile_x(x1, z);
	const int ty0 = geo_projection::tile_y(y1, z);
	const int ty1 = geo_projection::tile_y(y0, z);
	tile_cache &tc = tile_cache::instance();
	const double size = geo_projection::tile_size(z);
	for (int ty = ty0; ty <= ty1; ++ty)
	{
		for (int tx = tx0; tx <= tx1; ++tx)
		{
			// Integer edges: the neighbor tiles have the same edge (no gaps).
			const QPointF a = to_screen(geo_projection::tile_left(tx, z), geo_projection::tile_top(ty, z));
			const QPointF b = to_screen(geo_projection::tile_left(tx, z) + size, geo_projection::tile_top(ty, z) - size);
			const int left = static_cast<int>(std::floor(a.x()));
			const int top = static_cast<int>(std::floor(a.y()));
			const QRect target(left, top, static_cast<int>(std::floor(b.x())) - left, static_cast<int>(std::floor(b.y())) - top);
			QImage img = tc.image(z, tx, ty);
			QRectF source(0.0, 0.0, img.width(), img.height());
			for (int dz = 1; img.isNull() && dz <= 6 && z - dz >= 0; ++dz)
			{
				img = tc.cached(z - dz, tx >> dz, ty >> dz);
				if (!img.isNull())
				{
					const double cell = img.width() / static_cast<double>(1 << dz);
					source = QRectF((tx - ((tx >> dz) << dz)) * cell, (ty - ((ty >> dz) << dz)) * cell, cell, cell);
				}
			}
			if (!img.isNull())
			{
				p.drawImage(target, img, source);
			}
		}
	}

	// --- the area ---
	p.setRenderHint(QPainter::Antialiasing, true);
	const QRectF ar = area_rect();
	QPainterPath outside;
	outside.addRect(QRectF(rect()));
	QPainterPath inner;
	inner.addRect(ar);
	p.fillPath(outside.subtracted(inner), QColor(30, 40, 55, 70));
	const bool large = area_width_m() > max_size_m + 1.0 || area_height_m() > max_size_m + 1.0;
	const QColor edge = large ? QColor(214, 70, 70) : QColor(70, 130, 200);
	p.setPen(QPen(edge, 2.0));
	p.setBrush(QColor(edge.red(), edge.green(), edge.blue(), 28));
	p.drawRect(ar);
	p.setPen(QPen(edge, 1.5));
	p.setBrush(Qt::white);
	const double xs[3] = { ar.left(), ar.center().x(), ar.right() };
	const double ys[3] = { ar.top(), ar.center().y(), ar.bottom() };
	for (int i = 0; i < 3; ++i)
	{
		for (int j = 0; j < 3; ++j)
		{
			if (i != 1 || j != 1)
			{
				p.drawRect(QRectF(xs[i] - 4.0, ys[j] - 4.0, 8.0, 8.0));
			}
		}
	}

	// --- size of the area ---
	QFont f = font();
	f.setPointSizeF(9.0);
	f.setBold(true);
	p.setFont(f);
	const QString size_text = QStringLiteral("%1 x %2 m").arg(qRound(area_width_m())).arg(qRound(area_height_m()));
	const QFontMetricsF fm(f);
	const QRectF label(ar.left(), ar.top() - fm.height() - 8.0 < 2.0 ? ar.top() + 4.0 : ar.top() - fm.height() - 8.0,
		fm.horizontalAdvance(size_text) + 14.0, fm.height() + 6.0);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 225));
	p.drawRoundedRect(label, 5.0, 5.0);
	p.setPen(edge.darker(130));
	p.drawText(label, Qt::AlignCenter, size_text);

	// --- help (top left) and attribution (bottom right) ---
	QFont sf = font();
	sf.setPointSizeF(8.0);
	p.setFont(sf);
	const QFontMetricsF sm(sf);
	const QString help = tx("Drag: move the map.  Wheel: zoom.  Shift + drag or right drag: draw an area.  Drag the area or its edges: move it or change its size.");
	const QRectF hr(6.0, 6.0, std::min(width() - 12.0, sm.horizontalAdvance(help) + 14.0), sm.height() + 6.0);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 215));
	p.drawRoundedRect(hr, 5.0, 5.0);
	p.setPen(QColor(60, 70, 85));
	p.drawText(hr.adjusted(7, 0, -7, 0), Qt::AlignVCenter | Qt::AlignLeft, sm.elidedText(help, Qt::ElideRight, hr.width() - 14.0));
	const QString att = tx("Map data © OpenStreetMap contributors");
	const QRectF atr(width() - sm.horizontalAdvance(att) - 18.0, height() - sm.height() - 10.0, sm.horizontalAdvance(att) + 12.0, sm.height() + 4.0);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(255, 255, 255, 215));
	p.drawRoundedRect(atr, 4.0, 4.0);
	p.setPen(QColor(60, 70, 85));
	p.drawText(atr, Qt::AlignCenter, att);
}

void osm_area_map::mousePressEvent(QMouseEvent *e)
{
	m_fit_pending = false;
	m_last = e->localPos();
	double mx = 0.0;
	double my = 0.0;
	to_mercator(m_last, &mx, &my);
	m_edges = 0;
	if (e->button() == Qt::RightButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ShiftModifier)))
	{
		m_drag = drag_draw;
		m_start_x = mx;
		m_start_y = my;
		m_ax0 = m_ax1 = mx;
		m_ay0 = m_ay1 = my;
		setCursor(Qt::CrossCursor);
	}
	else if (e->button() == Qt::LeftButton)
	{
		m_edges = edges_at(m_last);
		if (m_edges != 0)
		{
			m_drag = drag_resize;
		}
		else if (area_rect().contains(m_last))
		{
			m_drag = drag_move;
		}
		else
		{
			m_drag = drag_pan;
			setCursor(Qt::ClosedHandCursor);
		}
	}
	update();
}

void osm_area_map::mouseMoveEvent(QMouseEvent *e)
{
	const QPointF pos = e->localPos();
	if (m_drag == drag_none)
	{
		update_cursor(pos);
		return;
	}
	double mx = 0.0;
	double my = 0.0;
	double lx = 0.0;
	double ly = 0.0;
	to_mercator(pos, &mx, &my);
	to_mercator(m_last, &lx, &ly);
	const double dx = mx - lx;
	const double dy = my - ly;
	switch (m_drag)
	{
		case drag_pan:
			m_cx -= dx;
			m_cy -= dy;
			break;
		case drag_draw:
			m_ax0 = std::min(m_start_x, mx);
			m_ax1 = std::max(m_start_x, mx);
			m_ay0 = std::min(m_start_y, my);
			m_ay1 = std::max(m_start_y, my);
			m_edges = (mx < m_start_x ? edge_left : edge_right) | (my < m_start_y ? edge_bottom : edge_top);
			limit_area(m_edges);
			break;
		case drag_move:
			m_ax0 += dx;
			m_ax1 += dx;
			m_ay0 += dy;
			m_ay1 += dy;
			break;
		case drag_resize:
			if (m_edges & edge_left) { m_ax0 += dx; }
			if (m_edges & edge_right) { m_ax1 += dx; }
			if (m_edges & edge_top) { m_ay1 += dy; }
			if (m_edges & edge_bottom) { m_ay0 += dy; }
			if (m_ax1 < m_ax0)
			{
				std::swap(m_ax0, m_ax1);
				if (m_edges & (edge_left | edge_right)) { m_edges ^= (edge_left | edge_right); }
			}
			if (m_ay1 < m_ay0)
			{
				std::swap(m_ay0, m_ay1);
				if (m_edges & (edge_top | edge_bottom)) { m_edges ^= (edge_top | edge_bottom); }
			}
			limit_area(m_edges);
			break;
		default:
			break;
	}
	m_last = pos;
	if (m_drag != drag_pan)
	{
		emit area_changed(area());
	}
	update();
}

void osm_area_map::mouseReleaseEvent(QMouseEvent *e)
{
	const drag_t d = m_drag;
	m_drag = drag_none;
	if (d == drag_draw || d == drag_move || d == drag_resize)
	{
		limit_area(d == drag_move ? 0 : m_edges);
		emit area_changed(area());
	}
	update_cursor(e->localPos());
	update();
}

void osm_area_map::mouseDoubleClickEvent(QMouseEvent *e)
{
	if (e->button() != Qt::LeftButton)
	{
		return;
	}
	double mx = 0.0;
	double my = 0.0;
	to_mercator(e->localPos(), &mx, &my);
	const double w = m_ax1 - m_ax0;
	const double h = m_ay1 - m_ay0;
	m_ax0 = mx - w / 2.0;
	m_ax1 = mx + w / 2.0;
	m_ay0 = my - h / 2.0;
	m_ay1 = my + h / 2.0;
	limit_area(0);
	emit area_changed(area());
	update();
}

void osm_area_map::wheelEvent(QWheelEvent *e)
{
	const double steps = e->angleDelta().y() / 120.0;
	if (std::abs(steps) > 1e-6)
	{
		zoom_at(steps * 0.5, e->posF());
	}
	e->accept();
}
