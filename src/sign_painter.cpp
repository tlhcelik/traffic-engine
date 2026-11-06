#include "sign_catalog.h"

#include <QFontMetricsF>
#include <QPainter>
#include <QPainterPath>
#include <QRectF>
#include <QTransform>

#include <algorithm>
#include <cmath>
#include <initializer_list>

// sign_painter
// ------------
// Vector drawing of the sign faces (QPainter). The shape (triangle, circle,
// octagon, square, diamond, board, wide board, saltire) comes from the
// catalog; the symbol draws the pictogram. Attributes of the catalog:
//   mirror  the pictogram is mirrored (left / right variants)
//   slash   a red diagonal bar over the pictogram ("end of ...", "no ...")
//   panel   a white panel in a blue information sign (service signs)
// The new pictograms draw in a box that fits the shape (see symbol_box()).

namespace
{
	const QColor sign_red(214, 40, 40);
	const QColor sign_orange(242, 140, 40);
	const QColor sign_blue(29, 95, 168);

	// All coordinates are relative to the unit square [0,1] x [0,1] of the
	// sign face; pt() converts to pixels.
	struct canvas
	{
		QPainter &p;
		QRectF r;
		QPointF pt(double x, double y) const { return QPointF(r.left() + x * r.width(), r.top() + y * r.height()); }
		double len(double v) const { return v * r.width(); }
	};

	// A part of the sign face for a pictogram (unit coordinates).
	struct box
	{
		double x0;
		double y0;
		double x1;
		double y1;
		double x(double u) const { return x0 + u * (x1 - x0); }
		double y(double v) const { return y0 + v * (y1 - y0); }
		double w() const { return x1 - x0; }
		double h() const { return y1 - y0; }
		box sub(double u0, double v0, double u1, double v1) const { return box{ x(u0), y(v0), x(u1), y(v1) }; }
	};

	box symbol_box(const sign_def &def)
	{
		if (def.panel)
		{
			return box{ 0.25, 0.20, 0.75, 0.64 };
		}
		if (def.shape == QLatin1String("triangle"))
		{
			return box{ 0.31, 0.46, 0.69, 0.83 };
		}
		if (def.shape == QLatin1String("square") || def.shape == QLatin1String("rect"))
		{
			return box{ 0.19, 0.19, 0.81, 0.81 };
		}
		return box{ 0.24, 0.26, 0.76, 0.74 };
	}

	// --- small helpers ---
	void pen(canvas &c, const QColor &col, double w, Qt::PenCapStyle cap = Qt::RoundCap)
	{
		c.p.setPen(QPen(col, c.len(w), Qt::SolidLine, cap, Qt::RoundJoin));
		c.p.setBrush(Qt::NoBrush);
	}

	void fill(canvas &c, const QColor &col)
	{
		c.p.setPen(Qt::NoPen);
		c.p.setBrush(col);
	}

	QPointF P(const canvas &c, const box &b, double u, double v)
	{
		return c.pt(b.x(u), b.y(v));
	}

	QPolygonF poly(const canvas &c, const box &b, std::initializer_list<double> uv)
	{
		QPolygonF out;
		for (auto it = uv.begin(); it != uv.end() && it + 1 != uv.end(); it += 2)
		{
			out << P(c, b, *it, *(it + 1));
		}
		return out;
	}

	void line(canvas &c, const box &b, double u0, double v0, double u1, double v1)
	{
		c.p.drawLine(P(c, b, u0, v0), P(c, b, u1, v1));
	}

	void rect(canvas &c, const box &b, double u0, double v0, double u1, double v1, double round = 0.0)
	{
		const QRectF q(P(c, b, u0, v0), P(c, b, u1, v1));
		if (round > 0.0)
		{
			c.p.drawRoundedRect(q, c.len(round * b.w()), c.len(round * b.w()));
		}
		else
		{
			c.p.drawRect(q);
		}
	}

	void disc(canvas &c, const box &b, double u, double v, double rad)
	{
		c.p.drawEllipse(P(c, b, u, v), c.len(rad * b.w()), c.len(rad * b.w()));
	}

	// Text that fits in the unit rectangle (bold, centered).
	void fit_text(canvas &c, const QString &text, const QRectF &unit, const QColor &col, double max_size)
	{
		if (text.isEmpty())
		{
			return;
		}
		QFont font(QStringLiteral("Arial"));
		font.setBold(true);
		const QRectF tr(c.pt(unit.left(), unit.top()), c.pt(unit.right(), unit.bottom()));
		double size = c.len(max_size);
		font.setPixelSize(std::max(1, static_cast<int>(size)));
		QFontMetricsF fm(font);
		const double w = fm.horizontalAdvance(text);
		if (w > tr.width())
		{
			size *= tr.width() / w;
			font.setPixelSize(std::max(1, static_cast<int>(size)));
		}
		c.p.setFont(font);
		c.p.setPen(col);
		c.p.drawText(tr, Qt::AlignCenter, text);
	}

	void arrow(canvas &c, QPointF from, QPointF to, double width, const QColor &col)
	{
		QPen pn(col, c.len(width), Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin);
		c.p.setPen(pn);
		const QPointF d = to - from;
		const double l = std::sqrt(d.x() * d.x() + d.y() * d.y());
		if (l < 1e-6)
		{
			return;
		}
		const QPointF u = d / l;
		const QPointF n(-u.y(), u.x());
		const double head = c.len(width) * 2.2;
		c.p.drawLine(from, to - u * head * 0.9);
		QPolygonF tri;
		tri << to << (to - u * head * 1.4 + n * head) << (to - u * head * 1.4 - n * head);
		c.p.setPen(Qt::NoPen);
		c.p.setBrush(col);
		c.p.drawPolygon(tri);
	}

	void turn_arrow(canvas &c, bool left, double width, const QColor &col)
	{
		// up, then turn to the side
		QPainterPath path;
		const double sx = left ? 0.62 : 0.38;
		path.moveTo(c.pt(sx, 0.80));
		path.lineTo(c.pt(sx, 0.52));
		path.quadTo(c.pt(sx, 0.38), c.pt(left ? 0.48 : 0.52, 0.38));
		c.p.setPen(QPen(col, c.len(width), Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
		c.p.setBrush(Qt::NoBrush);
		c.p.drawPath(path);
		arrow(c, c.pt(0.50, 0.38), c.pt(left ? 0.24 : 0.76, 0.38), width, col);
	}

	void uturn_arrow(canvas &c, double width, const QColor &col)
	{
		QPainterPath path;
		path.moveTo(c.pt(0.62, 0.80));
		path.lineTo(c.pt(0.62, 0.42));
		path.arcTo(QRectF(c.pt(0.34, 0.28), c.pt(0.62, 0.56)), 0.0, 180.0);
		c.p.setPen(QPen(col, c.len(width), Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
		c.p.setBrush(Qt::NoBrush);
		c.p.drawPath(path);
		arrow(c, c.pt(0.34, 0.45), c.pt(0.34, 0.70), width, col);
	}

	void red_slash(canvas &c, const QColor &col)
	{
		c.p.setPen(QPen(col, c.len(0.09), Qt::SolidLine, Qt::FlatCap));
		c.p.drawLine(c.pt(0.24, 0.24), c.pt(0.76, 0.76));
	}

	void end_stripes(canvas &c)
	{
		c.p.setPen(QPen(QColor(90, 90, 90), c.len(0.035), Qt::SolidLine, Qt::FlatCap));
		for (int i = -2; i <= 2; ++i)
		{
			const double o = i * 0.07;
			c.p.drawLine(c.pt(0.22 + o, 0.78 + o), c.pt(0.78 + o, 0.22 + o));
		}
	}

	void car_shape(canvas &c, double cx, double cy, double w, const QColor &col)
	{
		// top view of a car, nose up
		const double h = w * 1.9;
		QPainterPath body;
		body.addRoundedRect(QRectF(c.pt(cx - w / 2, cy - h / 2), c.pt(cx + w / 2, cy + h / 2)), c.len(w * 0.35), c.len(w * 0.35));
		c.p.setPen(Qt::NoPen);
		c.p.setBrush(col);
		c.p.drawPath(body);
		c.p.setBrush(QColor(255, 255, 255, 170));
		c.p.drawRect(QRectF(c.pt(cx - w * 0.33, cy - h * 0.30), c.pt(cx + w * 0.33, cy - h * 0.12)));
	}

	void person(canvas &c, double cx, double cy, double sc, const QColor &col)
	{
		c.p.setPen(Qt::NoPen);
		c.p.setBrush(col);
		c.p.drawEllipse(c.pt(cx, cy - 0.20 * sc), c.len(0.055 * sc), c.len(0.055 * sc));
		c.p.setPen(QPen(col, c.len(0.06 * sc), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		c.p.drawLine(c.pt(cx, cy - 0.13 * sc), c.pt(cx - 0.02 * sc, cy + 0.06 * sc));
		c.p.drawLine(c.pt(cx - 0.02 * sc, cy + 0.06 * sc), c.pt(cx - 0.09 * sc, cy + 0.22 * sc));
		c.p.drawLine(c.pt(cx - 0.02 * sc, cy + 0.06 * sc), c.pt(cx + 0.07 * sc, cy + 0.22 * sc));
		c.p.drawLine(c.pt(cx, cy - 0.08 * sc), c.pt(cx - 0.09 * sc, cy + 0.02 * sc));
		c.p.drawLine(c.pt(cx, cy - 0.08 * sc), c.pt(cx + 0.08 * sc, cy));
	}

	void wheel_vehicle(canvas &c, const QColor &col, bool motor)
	{
		c.p.setPen(QPen(col, c.len(0.045), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
		c.p.setBrush(Qt::NoBrush);
		c.p.drawEllipse(c.pt(0.32, 0.62), c.len(0.11), c.len(0.11));
		c.p.drawEllipse(c.pt(0.70, 0.62), c.len(0.11), c.len(0.11));
		QPolygonF frame;
		frame << c.pt(0.32, 0.62) << c.pt(0.46, 0.45) << c.pt(0.62, 0.45) << c.pt(0.70, 0.62);
		c.p.drawPolyline(frame);
		c.p.drawLine(c.pt(0.46, 0.45), c.pt(0.50, 0.62));
		c.p.drawLine(c.pt(0.62, 0.45), c.pt(0.60, 0.36));
		if (motor)
		{
			c.p.setBrush(col);
			c.p.drawRoundedRect(QRectF(c.pt(0.43, 0.47), c.pt(0.60, 0.56)), c.len(0.02), c.len(0.02));
		}
	}

	// ------------------------------------------------------------------
	// pictograms in a box (u, v in [0, 1])
	// ------------------------------------------------------------------
	void pic_person(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		disc(c, b, 0.50, 0.12, 0.10);
		pen(c, col, 0.11 * b.w());
		line(c, b, 0.50, 0.26, 0.47, 0.60);
		line(c, b, 0.47, 0.60, 0.34, 0.95);
		line(c, b, 0.47, 0.60, 0.64, 0.95);
		line(c, b, 0.50, 0.33, 0.33, 0.55);
		line(c, b, 0.50, 0.33, 0.67, 0.50);
	}

	void pic_car_side(canvas &c, const box &b, const QColor &col, const QColor &bg)
	{
		fill(c, col);
		rect(c, b, 0.0, 0.42, 1.0, 0.82, 0.08);
		c.p.drawPolygon(poly(c, b, { 0.18, 0.44, 0.30, 0.08, 0.70, 0.08, 0.84, 0.44 }));
		fill(c, bg);
		c.p.drawPolygon(poly(c, b, { 0.26, 0.42, 0.34, 0.16, 0.48, 0.16, 0.48, 0.42 }));
		c.p.drawPolygon(poly(c, b, { 0.53, 0.42, 0.53, 0.16, 0.67, 0.16, 0.76, 0.42 }));
		fill(c, col);
		disc(c, b, 0.22, 0.84, 0.12);
		disc(c, b, 0.78, 0.84, 0.12);
		fill(c, bg);
		disc(c, b, 0.22, 0.84, 0.05);
		disc(c, b, 0.78, 0.84, 0.05);
	}

	void pic_car_front(canvas &c, const box &b, const QColor &col, const QColor &bg)
	{
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.22, 0.42, 0.30, 0.12, 0.70, 0.12, 0.78, 0.42 }));
		rect(c, b, 0.08, 0.40, 0.92, 0.78, 0.10);
		rect(c, b, 0.14, 0.76, 0.30, 0.95, 0.04);
		rect(c, b, 0.70, 0.76, 0.86, 0.95, 0.04);
		fill(c, bg);
		c.p.drawPolygon(poly(c, b, { 0.30, 0.40, 0.36, 0.20, 0.64, 0.20, 0.70, 0.40 }));
		disc(c, b, 0.22, 0.58, 0.07);
		disc(c, b, 0.78, 0.58, 0.07);
	}

	void pic_truck(canvas &c, const box &b, const QColor &col, const QColor &bg)
	{
		fill(c, col);
		rect(c, b, 0.0, 0.10, 0.68, 0.76, 0.02);
		c.p.drawPolygon(poly(c, b, { 0.71, 0.76, 0.71, 0.30, 0.90, 0.30, 1.0, 0.50, 1.0, 0.76 }));
		fill(c, bg);
		c.p.drawPolygon(poly(c, b, { 0.76, 0.36, 0.88, 0.36, 0.95, 0.50, 0.76, 0.50 }));
		fill(c, col);
		for (double u : { 0.14, 0.32, 0.86 })
		{
			disc(c, b, u, 0.84, 0.10);
		}
		fill(c, bg);
		for (double u : { 0.14, 0.32, 0.86 })
		{
			disc(c, b, u, 0.84, 0.04);
		}
	}

	void pic_bus_side(canvas &c, const box &b, const QColor &col, const QColor &bg)
	{
		fill(c, col);
		rect(c, b, 0.0, 0.18, 1.0, 0.80, 0.06);
		fill(c, bg);
		for (int i = 0; i < 4; ++i)
		{
			rect(c, b, 0.06 + i * 0.20, 0.28, 0.20 + i * 0.20, 0.48);
		}
		rect(c, b, 0.88, 0.28, 0.95, 0.66);
		fill(c, col);
		disc(c, b, 0.20, 0.84, 0.10);
		disc(c, b, 0.78, 0.84, 0.10);
	}

	void pic_tram(canvas &c, const box &b, const QColor &col, const QColor &bg)
	{
		pen(c, col, 0.035 * b.w());
		line(c, b, 0.40, 0.18, 0.55, 0.02);
		line(c, b, 0.55, 0.02, 0.62, 0.18);
		line(c, b, 0.20, 0.02, 0.80, 0.02);
		fill(c, col);
		rect(c, b, 0.0, 0.18, 1.0, 0.82, 0.08);
		fill(c, bg);
		for (int i = 0; i < 4; ++i)
		{
			rect(c, b, 0.07 + i * 0.22, 0.28, 0.23 + i * 0.22, 0.50);
		}
		fill(c, col);
		rect(c, b, 0.0, 0.88, 1.0, 0.95);
	}

	void pic_tractor(canvas &c, const box &b, const QColor &col, const QColor &bg)
	{
		fill(c, col);
		rect(c, b, 0.36, 0.42, 0.98, 0.66, 0.04);
		pen(c, col, 0.06 * b.w(), Qt::SquareCap);
		line(c, b, 0.20, 0.06, 0.20, 0.45);
		line(c, b, 0.20, 0.06, 0.50, 0.06);
		line(c, b, 0.50, 0.06, 0.50, 0.42);
		line(c, b, 0.80, 0.42, 0.80, 0.18);
		fill(c, col);
		disc(c, b, 0.28, 0.68, 0.26);
		disc(c, b, 0.84, 0.80, 0.14);
		fill(c, bg);
		disc(c, b, 0.28, 0.68, 0.10);
		disc(c, b, 0.84, 0.80, 0.05);
	}

	void pic_cycle(canvas &c, const box &b, const QColor &col, bool engine)
	{
		pen(c, col, 0.07 * b.w());
		const double r = 0.20;
		c.p.drawEllipse(P(c, b, 0.20, 0.70), c.len(r * b.w()), c.len(r * b.w()));
		c.p.drawEllipse(P(c, b, 0.80, 0.70), c.len(r * b.w()), c.len(r * b.w()));
		c.p.drawPolyline(poly(c, b, { 0.20, 0.70, 0.42, 0.38, 0.70, 0.38, 0.80, 0.70 }));
		line(c, b, 0.42, 0.38, 0.50, 0.70);
		line(c, b, 0.70, 0.38, 0.66, 0.20);
		if (engine)
		{
			fill(c, col);
			rect(c, b, 0.40, 0.46, 0.62, 0.62, 0.03);
		}
	}

	void pic_horse(canvas &c, const box &b, const QColor &col, bool rider)
	{
		fill(c, col);
		c.p.drawEllipse(QRectF(P(c, b, 0.18, 0.40), P(c, b, 0.78, 0.66)));
		c.p.drawPolygon(poly(c, b, { 0.66, 0.46, 0.80, 0.18, 0.96, 0.24, 0.92, 0.32, 0.80, 0.34, 0.76, 0.52 }));
		pen(c, col, 0.07 * b.w());
		line(c, b, 0.26, 0.60, 0.20, 0.96);
		line(c, b, 0.36, 0.62, 0.38, 0.96);
		line(c, b, 0.64, 0.62, 0.62, 0.96);
		line(c, b, 0.72, 0.58, 0.80, 0.94);
		line(c, b, 0.20, 0.46, 0.06, 0.66);
		if (rider)
		{
			fill(c, col);
			disc(c, b, 0.50, 0.04, 0.07);
			pen(c, col, 0.08 * b.w());
			line(c, b, 0.50, 0.12, 0.48, 0.40);
			line(c, b, 0.50, 0.20, 0.66, 0.30);
		}
	}

	void pic_cow(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		rect(c, b, 0.12, 0.30, 0.74, 0.66, 0.10);
		c.p.drawPolygon(poly(c, b, { 0.72, 0.34, 0.92, 0.36, 0.98, 0.52, 0.84, 0.56, 0.74, 0.50 }));
		pen(c, col, 0.07 * b.w());
		for (double u : { 0.18, 0.30, 0.58, 0.68 })
		{
			line(c, b, u, 0.62, u, 0.94);
		}
		line(c, b, 0.86, 0.34, 0.82, 0.22);
		line(c, b, 0.92, 0.36, 0.98, 0.24);
		line(c, b, 0.12, 0.34, 0.04, 0.60);
	}

	void pic_deer(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.10, 0.50, 0.30, 0.36, 0.62, 0.38, 0.78, 0.24, 0.90, 0.30, 0.74, 0.50, 0.60, 0.62, 0.22, 0.62 }));
		disc(c, b, 0.86, 0.24, 0.07);
		pen(c, col, 0.06 * b.w());
		line(c, b, 0.22, 0.60, 0.06, 0.94);
		line(c, b, 0.30, 0.60, 0.30, 0.96);
		line(c, b, 0.58, 0.60, 0.74, 0.92);
		line(c, b, 0.64, 0.56, 0.90, 0.80);
		pen(c, col, 0.04 * b.w());
		line(c, b, 0.84, 0.18, 0.76, 0.02);
		line(c, b, 0.80, 0.10, 0.70, 0.08);
		line(c, b, 0.88, 0.18, 0.96, 0.02);
	}

	void pic_plane(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		QPainterPath f;
		f.addRoundedRect(QRectF(P(c, b, 0.05, 0.42), P(c, b, 0.95, 0.56)), c.len(0.06 * b.w()), c.len(0.06 * b.w()));
		c.p.drawPath(f);
		c.p.drawPolygon(poly(c, b, { 0.40, 0.46, 0.58, 0.06, 0.70, 0.06, 0.62, 0.46 }));
		c.p.drawPolygon(poly(c, b, { 0.40, 0.52, 0.58, 0.94, 0.70, 0.94, 0.62, 0.52 }));
		c.p.drawPolygon(poly(c, b, { 0.06, 0.48, 0.12, 0.24, 0.20, 0.24, 0.18, 0.48 }));
	}

	void pic_windsock(canvas &c, const box &b, const QColor &col)
	{
		pen(c, col, 0.07 * b.w(), Qt::FlatCap);
		line(c, b, 0.12, 0.04, 0.12, 0.98);
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.12, 0.10, 0.94, 0.26, 0.94, 0.40, 0.12, 0.46 }));
		fill(c, sign_red);
		c.p.drawPolygon(poly(c, b, { 0.40, 0.15, 0.62, 0.19, 0.62, 0.42, 0.40, 0.44 }));
	}

	void pic_rocks(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.62, 0.0, 1.0, 0.0, 1.0, 1.0, 0.86, 1.0 }));
		c.p.drawPolygon(poly(c, b, { 0.30, 0.22, 0.44, 0.18, 0.48, 0.32, 0.34, 0.36 }));
		c.p.drawPolygon(poly(c, b, { 0.14, 0.48, 0.30, 0.44, 0.34, 0.60, 0.18, 0.64 }));
		c.p.drawPolygon(poly(c, b, { 0.40, 0.70, 0.60, 0.66, 0.64, 0.84, 0.42, 0.88 }));
		c.p.drawPolygon(poly(c, b, { 0.0, 0.94, 0.30, 0.92, 0.36, 1.0, 0.0, 1.0 }));
	}

	void pic_gravel(canvas &c, const box &b, const QColor &col, const QColor &bg)
	{
		pic_car_side(c, b.sub(0.0, 0.40, 0.62, 0.96), col, bg);
		fill(c, col);
		disc(c, b, 0.72, 0.60, 0.05);
		disc(c, b, 0.84, 0.48, 0.04);
		disc(c, b, 0.94, 0.66, 0.05);
		disc(c, b, 0.80, 0.78, 0.04);
		disc(c, b, 0.92, 0.88, 0.04);
	}

	void pic_snowflake(canvas &c, const box &b, const QColor &col)
	{
		pen(c, col, 0.07 * b.w());
		const QPointF ctr = P(c, b, 0.5, 0.5);
		const double rad = c.len(0.46 * std::min(b.w(), b.h()));
		for (int i = 0; i < 3; ++i)
		{
			const double a = i * M_PI / 3.0 + M_PI / 2.0;
			const QPointF d(std::cos(a) * rad, std::sin(a) * rad);
			c.p.drawLine(ctr - d, ctr + d);
			for (int s : { -1, 1 })
			{
				const QPointF tip = ctr + d * (0.62 * s);
				const QPointF base = ctr + d * (0.36 * s);
				const QPointF n(-d.y() * 0.28, d.x() * 0.28);
				c.p.drawLine(base + n, tip);
				c.p.drawLine(base - n, tip);
			}
		}
	}

	void pic_tire(canvas &c, const box &b, const QColor &col)
	{
		pen(c, col, 0.10 * b.w());
		c.p.drawEllipse(QRectF(P(c, b, 0.14, 0.06), P(c, b, 0.86, 0.94)));
		pen(c, col, 0.05 * b.w());
		for (double v : { 0.18, 0.36, 0.54, 0.72 })
		{
			line(c, b, 0.20, v, 0.80, v + 0.12);
		}
	}

	void pic_handcart(canvas &c, const box &b, const QColor &col)
	{
		pic_person(c, b.sub(0.0, 0.0, 0.42, 1.0), col);
		pen(c, col, 0.06 * b.w());
		line(c, b, 0.30, 0.44, 0.48, 0.52);
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.46, 0.40, 1.0, 0.40, 0.92, 0.70, 0.52, 0.70 }));
		disc(c, b, 0.74, 0.84, 0.10);
	}

	void pic_tent(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.04, 0.92, 0.50, 0.08, 0.96, 0.92 }));
		fill(c, Qt::white);
		c.p.drawPolygon(poly(c, b, { 0.36, 0.92, 0.50, 0.54, 0.64, 0.92 }));
	}

	void pic_caravan(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		rect(c, b, 0.04, 0.18, 0.84, 0.76, 0.10);
		disc(c, b, 0.40, 0.82, 0.12);
		pen(c, col, 0.05 * b.w());
		line(c, b, 0.84, 0.66, 1.0, 0.66);
		fill(c, Qt::white);
		rect(c, b, 0.16, 0.30, 0.40, 0.48);
		rect(c, b, 0.54, 0.30, 0.72, 0.62);
	}

	void pic_house(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.04, 0.46, 0.50, 0.06, 0.96, 0.46 }));
		rect(c, b, 0.16, 0.44, 0.84, 0.94);
		fill(c, Qt::white);
		rect(c, b, 0.42, 0.62, 0.58, 0.94);
	}

	void pic_bed(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		rect(c, b, 0.04, 0.20, 0.12, 0.92);
		rect(c, b, 0.04, 0.58, 0.96, 0.72);
		rect(c, b, 0.88, 0.58, 0.96, 0.92);
		disc(c, b, 0.26, 0.48, 0.09);
		rect(c, b, 0.38, 0.44, 0.94, 0.58, 0.04);
	}

	void pic_fork_knife(canvas &c, const box &b, const QColor &col)
	{
		pen(c, col, 0.07 * b.w(), Qt::FlatCap);
		line(c, b, 0.32, 0.40, 0.32, 0.96);
		for (double u : { 0.22, 0.32, 0.42 })
		{
			line(c, b, u, 0.06, u, 0.36);
		}
		line(c, b, 0.20, 0.38, 0.44, 0.38);
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.64, 0.96, 0.64, 0.04, 0.80, 0.20, 0.76, 0.52, 0.70, 0.56, 0.70, 0.96 }));
	}

	void pic_cup(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.12, 0.30, 0.72, 0.30, 0.64, 0.84, 0.20, 0.84 }));
		rect(c, b, 0.04, 0.86, 0.80, 0.94, 0.02);
		pen(c, col, 0.07 * b.w());
		c.p.drawArc(QRectF(P(c, b, 0.60, 0.36), P(c, b, 0.92, 0.62)), -90 * 16, 180 * 16);
		pen(c, col, 0.04 * b.w());
		line(c, b, 0.32, 0.22, 0.36, 0.04);
		line(c, b, 0.50, 0.22, 0.54, 0.04);
	}

	void pic_faucet(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		rect(c, b, 0.06, 0.20, 0.64, 0.36, 0.03);
		rect(c, b, 0.48, 0.20, 0.64, 0.52, 0.03);
		rect(c, b, 0.20, 0.06, 0.30, 0.20);
		rect(c, b, 0.10, 0.04, 0.40, 0.10, 0.02);
		c.p.drawEllipse(QRectF(P(c, b, 0.48, 0.64), P(c, b, 0.64, 0.90)));
	}

	void pic_picnic(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		c.p.drawPolygon(poly(c, b, { 0.50, 0.02, 0.86, 0.46, 0.14, 0.46 }));
		rect(c, b, 0.46, 0.44, 0.54, 0.62);
		rect(c, b, 0.04, 0.66, 0.96, 0.74);
		pen(c, col, 0.06 * b.w(), Qt::FlatCap);
		line(c, b, 0.14, 0.74, 0.08, 0.98);
		line(c, b, 0.86, 0.74, 0.92, 0.98);
		line(c, b, 0.30, 0.86, 0.70, 0.86);
	}

	void pic_hiker(canvas &c, const box &b, const QColor &col)
	{
		pic_person(c, b.sub(0.10, 0.0, 0.70, 1.0), col);
		fill(c, col);
		rect(c, b, 0.48, 0.24, 0.66, 0.50, 0.04);
		pen(c, col, 0.04 * b.w());
		line(c, b, 0.20, 0.50, 0.12, 1.0);
	}

	void pic_flame(canvas &c, const box &b)
	{
		QPainterPath f;
		f.moveTo(P(c, b, 0.50, 0.02));
		f.cubicTo(P(c, b, 0.70, 0.30), P(c, b, 0.96, 0.50), P(c, b, 0.80, 0.82));
		f.cubicTo(P(c, b, 0.70, 1.0), P(c, b, 0.30, 1.0), P(c, b, 0.20, 0.82));
		f.cubicTo(P(c, b, 0.06, 0.56), P(c, b, 0.36, 0.40), P(c, b, 0.50, 0.02));
		fill(c, sign_red);
		c.p.drawPath(f);
		QPainterPath i;
		i.moveTo(P(c, b, 0.50, 0.44));
		i.cubicTo(P(c, b, 0.62, 0.60), P(c, b, 0.72, 0.72), P(c, b, 0.62, 0.88));
		i.cubicTo(P(c, b, 0.56, 0.96), P(c, b, 0.40, 0.96), P(c, b, 0.36, 0.86));
		i.cubicTo(P(c, b, 0.30, 0.70), P(c, b, 0.46, 0.60), P(c, b, 0.50, 0.44));
		fill(c, sign_orange);
		c.p.drawPath(i);
	}

	void pic_swimmer(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		disc(c, b, 0.20, 0.36, 0.09);
		pen(c, col, 0.09 * b.w());
		line(c, b, 0.30, 0.42, 0.70, 0.50);
		line(c, b, 0.40, 0.44, 0.56, 0.24);
		line(c, b, 0.70, 0.50, 0.92, 0.42);
		pen(c, col, 0.05 * b.w());
		for (double v : { 0.70, 0.86 })
		{
			QPainterPath w;
			w.moveTo(P(c, b, 0.0, v));
			w.cubicTo(P(c, b, 0.16, v - 0.08), P(c, b, 0.34, v + 0.08), P(c, b, 0.50, v));
			w.cubicTo(P(c, b, 0.66, v - 0.08), P(c, b, 0.84, v + 0.08), P(c, b, 1.0, v));
			c.p.drawPath(w);
		}
	}

	void pic_phone(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		QPainterPath h;
		h.moveTo(P(c, b, 0.10, 0.24));
		h.cubicTo(P(c, b, 0.30, 0.02), P(c, b, 0.70, 0.02), P(c, b, 0.90, 0.24));
		h.lineTo(P(c, b, 0.74, 0.40));
		h.lineTo(P(c, b, 0.62, 0.28));
		h.lineTo(P(c, b, 0.38, 0.28));
		h.lineTo(P(c, b, 0.26, 0.40));
		h.closeSubpath();
		c.p.drawPath(h);
		c.p.drawPolygon(poly(c, b, { 0.30, 0.46, 0.70, 0.46, 0.86, 0.94, 0.14, 0.94 }));
		fill(c, Qt::white);
		disc(c, b, 0.50, 0.70, 0.12);
	}

	void pic_wrench(canvas &c, const box &b, const QColor &col)
	{
		c.p.save();
		c.p.translate(P(c, b, 0.5, 0.5));
		c.p.rotate(-45.0);
		const double s = c.len(b.w());
		fill(c, col);
		c.p.drawRoundedRect(QRectF(-0.08 * s, -0.10 * s, 0.16 * s, 0.56 * s), 0.04 * s, 0.04 * s);
		c.p.drawEllipse(QPointF(0, -0.24 * s), 0.20 * s, 0.20 * s);
		fill(c, Qt::white);
		c.p.drawRect(QRectF(-0.07 * s, -0.48 * s, 0.14 * s, 0.24 * s));
		c.p.restore();
	}

	void pic_fuel(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		rect(c, b, 0.12, 0.06, 0.62, 0.94, 0.06);
		rect(c, b, 0.04, 0.88, 0.70, 0.98);
		fill(c, Qt::white);
		rect(c, b, 0.20, 0.16, 0.54, 0.40, 0.02);
		pen(c, col, 0.06 * b.w());
		c.p.drawPolyline(poly(c, b, { 0.62, 0.30, 0.82, 0.40, 0.82, 0.80, 0.92, 0.80, 0.92, 0.20 }));
	}

	void pic_first_aid(canvas &c, const box &b)
	{
		fill(c, sign_red);
		rect(c, b, 0.38, 0.06, 0.62, 0.94);
		rect(c, b, 0.06, 0.38, 0.94, 0.62);
	}

	void pic_camera(canvas &c, const box &b, const QColor &col)
	{
		fill(c, col);
		rect(c, b, 0.10, 0.26, 0.76, 0.66, 0.06);
		c.p.drawPolygon(poly(c, b, { 0.76, 0.36, 0.96, 0.26, 0.96, 0.66, 0.76, 0.56 }));
		rect(c, b, 0.36, 0.66, 0.46, 0.96);
		fill(c, Qt::white);
		disc(c, b, 0.30, 0.46, 0.10);
	}

	void pic_tunnel(canvas &c, const box &b, const QColor &col, bool water)
	{
		fill(c, col);
		QPainterPath t;
		t.moveTo(P(c, b, 0.0, 1.0));
		t.lineTo(P(c, b, 0.0, 0.40));
		t.cubicTo(P(c, b, 0.0, -0.06), P(c, b, 1.0, -0.06), P(c, b, 1.0, 0.40));
		t.lineTo(P(c, b, 1.0, 1.0));
		t.lineTo(P(c, b, 0.80, 1.0));
		t.lineTo(P(c, b, 0.80, 0.46));
		t.cubicTo(P(c, b, 0.80, 0.14), P(c, b, 0.20, 0.14), P(c, b, 0.20, 0.46));
		t.lineTo(P(c, b, 0.20, 1.0));
		t.closeSubpath();
		c.p.drawPath(t);
		if (water)
		{
			pen(c, col, 0.05 * b.w());
			QPainterPath w;
			w.moveTo(P(c, b, 0.26, 0.60));
			w.cubicTo(P(c, b, 0.36, 0.52), P(c, b, 0.44, 0.68), P(c, b, 0.50, 0.60));
			w.cubicTo(P(c, b, 0.56, 0.52), P(c, b, 0.66, 0.68), P(c, b, 0.74, 0.60));
			c.p.drawPath(w);
		}
	}

	void pic_stairs(canvas &c, const box &b, const QColor &col, bool down)
	{
		pic_person(c, b.sub(down ? 0.0 : 0.46, 0.0, down ? 0.54 : 1.0, 0.70), col);
		fill(c, col);
		QPolygonF s;
		if (down)
		{
			s = poly(c, b, { 0.30, 0.74, 0.30, 0.80, 0.50, 0.80, 0.50, 0.88, 0.70, 0.88, 0.70, 0.96, 1.0, 0.96, 1.0, 1.0, 0.30, 1.0 });
		}
		else
		{
			s = poly(c, b, { 0.0, 1.0, 0.0, 0.96, 0.30, 0.96, 0.30, 0.88, 0.50, 0.88, 0.50, 0.80, 0.70, 0.80, 0.70, 0.74, 0.70, 1.0 });
		}
		c.p.drawPolygon(s);
	}

	// Red and white (or other) diagonal stripes in a board.
	void board_stripes(canvas &c, const QRectF &unit, int stripes, bool full, const QColor &a, const QColor &b2)
	{
		c.p.save();
		const QRectF area(c.pt(unit.left(), unit.top()), c.pt(unit.right(), unit.bottom()));
		QPainterPath clip;
		clip.addRect(area);
		c.p.setClipPath(clip);
		fill(c, b2);
		c.p.drawRect(area);
		fill(c, a);
		const double w = area.width();
		if (full)
		{
			const double step = w * 0.5;
			for (double y = area.top() - w; y < area.bottom() + w; y += step)
			{
				QPolygonF band;
				band << QPointF(area.left(), y + w * 0.5) << QPointF(area.right(), y) << QPointF(area.right(), y + step * 0.5)
					<< QPointF(area.left(), y + w * 0.5 + step * 0.5);
				c.p.drawPolygon(band);
			}
		}
		else
		{
			for (int i = 0; i < stripes; ++i)
			{
				const double yc = area.top() + area.height() * (0.75 - i * 0.25);
				QPolygonF band;
				band << QPointF(area.left(), yc + w * 0.32) << QPointF(area.right(), yc - w * 0.32)
					<< QPointF(area.right(), yc - w * 0.02) << QPointF(area.left(), yc + w * 0.62);
				c.p.drawPolygon(band);
			}
		}
		c.p.restore();
	}

	void draw_symbol(canvas &c, const sign_def &def, int value)
	{
		const QString &sym = def.symbol;
		const QColor fg = def.fg;
		const QColor bg = def.panel ? QColor(Qt::white) : def.bg;
		const box b = symbol_box(def);
		QFont font(QStringLiteral("Arial"));
		font.setBold(true);

		if (sym == QLatin1String("text") || sym == QLatin1String("value") || sym == QLatin1String("end_value"))
		{
			QString text = def.text;
			if (sym != QLatin1String("text"))
			{
				text = QString::number(value > 0 ? value : def.default_value);
			}
			if (def.panel || text.size() > 3)
			{
				fit_text(c, text, QRectF(b.x0, b.y0, b.w(), b.h()), fg, def.panel ? 0.22 : 0.30);
				return;
			}
			font.setPixelSize(static_cast<int>(c.len(text.size() <= 2 ? 0.42 : (text.size() == 3 ? 0.34 : 0.26))));
			c.p.setFont(font);
			c.p.setPen(sym == QLatin1String("end_value") ? QColor(110, 110, 110) : fg);
			QRectF tr = c.r;
			if (def.shape == QLatin1String("triangle"))
			{
				tr.setTop(tr.top() + tr.height() * 0.22);
			}
			c.p.drawText(tr, Qt::AlignCenter, text);
			if (sym == QLatin1String("end_value"))
			{
				end_stripes(c);
			}
		}
		else if (sym == QLatin1String("value_school"))
		{
			fit_text(c, QString::number(value > 0 ? value : def.default_value), QRectF(0.22, 0.20, 0.56, 0.42), fg, 0.36);
			fit_text(c, QStringLiteral("OKUL"), QRectF(0.26, 0.60, 0.48, 0.14), fg, 0.12);
		}
		else if (sym == QLatin1String("bar"))
		{
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(fg);
			c.p.drawRect(QRectF(c.pt(0.20, 0.42), c.pt(0.80, 0.58)));
		}
		else if (sym == QLatin1String("arrow_up"))
		{
			arrow(c, c.pt(0.5, 0.80), c.pt(0.5, 0.20), 0.11, fg);
		}
		else if (sym == QLatin1String("arrow_left"))
		{
			turn_arrow(c, true, 0.10, fg);
		}
		else if (sym == QLatin1String("arrow_right"))
		{
			turn_arrow(c, false, 0.10, fg);
		}
		else if (sym == QLatin1String("arrow_right_h"))
		{
			arrow(c, c.pt(0.20, 0.5), c.pt(0.80, 0.5), 0.11, fg);
		}
		else if (sym == QLatin1String("arrow_up_right"))
		{
			arrow(c, c.pt(0.42, 0.82), c.pt(0.42, 0.18), 0.09, fg);
			QPainterPath br;
			br.moveTo(c.pt(0.42, 0.60));
			br.quadTo(c.pt(0.42, 0.44), c.pt(0.56, 0.44));
			c.p.setPen(QPen(fg, c.len(0.09), Qt::SolidLine, Qt::FlatCap));
			c.p.setBrush(Qt::NoBrush);
			c.p.drawPath(br);
			arrow(c, c.pt(0.54, 0.44), c.pt(0.80, 0.44), 0.09, fg);
		}
		else if (sym == QLatin1String("arrow_left_right"))
		{
			c.p.setPen(QPen(fg, c.len(0.09), Qt::SolidLine, Qt::FlatCap));
			c.p.drawLine(c.pt(0.5, 0.82), c.pt(0.5, 0.52));
			arrow(c, c.pt(0.5, 0.48), c.pt(0.18, 0.48), 0.09, fg);
			arrow(c, c.pt(0.5, 0.48), c.pt(0.82, 0.48), 0.09, fg);
		}
		else if (sym == QLatin1String("keep_right"))
		{
			arrow(c, c.pt(0.30, 0.30), c.pt(0.72, 0.72), 0.11, fg);
		}
		else if (sym == QLatin1String("either_side"))
		{
			arrow(c, c.pt(0.5, 0.26), c.pt(0.26, 0.74), 0.08, fg);
			arrow(c, c.pt(0.5, 0.26), c.pt(0.74, 0.74), 0.08, fg);
		}
		else if (sym == QLatin1String("roundabout"))
		{
			c.p.setPen(QPen(fg, c.len(0.08), Qt::SolidLine, Qt::FlatCap));
			c.p.setBrush(Qt::NoBrush);
			for (int i = 0; i < 3; ++i)
			{
				c.p.drawArc(QRectF(c.pt(0.26, 0.26), c.pt(0.74, 0.74)), (i * 120 + 20) * 16, 70 * 16);
				const double a = (i * 120 + 90) * M_PI / 180.0;
				const QPointF tip = c.pt(0.5 + 0.24 * std::cos(a), 0.5 - 0.24 * std::sin(a));
				QPolygonF tri;
				tri << tip + QPointF(std::cos(a), -std::sin(a)) * c.len(0.08)
					<< tip - QPointF(std::cos(a), -std::sin(a)) * c.len(0.08)
					<< tip + QPointF(std::sin(a), std::cos(a)) * c.len(0.10);
				c.p.setPen(Qt::NoPen);
				c.p.setBrush(fg);
				c.p.drawPolygon(tri);
				c.p.setPen(QPen(fg, c.len(0.08), Qt::SolidLine, Qt::FlatCap));
				c.p.setBrush(Qt::NoBrush);
			}
		}
		else if (sym == QLatin1String("ban_left") || sym == QLatin1String("ban_right"))
		{
			turn_arrow(c, sym == QLatin1String("ban_left"), 0.09, QColor(20, 20, 20));
			red_slash(c, def.border);
		}
		else if (sym == QLatin1String("ban_uturn"))
		{
			uturn_arrow(c, 0.09, QColor(20, 20, 20));
			red_slash(c, def.border);
		}
		else if (sym == QLatin1String("uturn_info"))
		{
			uturn_arrow(c, 0.09, fg);
		}
		else if (sym == QLatin1String("two_cars") || sym == QLatin1String("end_two_cars"))
		{
			const bool end = (sym == QLatin1String("end_two_cars"));
			car_shape(c, 0.36, 0.52, 0.17, end ? QColor(120, 120, 120) : sign_red);
			car_shape(c, 0.64, 0.52, 0.17, end ? QColor(120, 120, 120) : QColor(25, 25, 25));
			if (end)
			{
				end_stripes(c);
			}
		}
		else if (sym == QLatin1String("truck_car") || sym == QLatin1String("end_truck_car"))
		{
			const bool end = (sym == QLatin1String("end_truck_car"));
			pic_truck(c, box{ 0.18, 0.40, 0.50, 0.62 }, end ? QColor(120, 120, 120) : sign_red, bg);
			pic_car_side(c, box{ 0.54, 0.42, 0.82, 0.62 }, end ? QColor(120, 120, 120) : QColor(25, 25, 25), bg);
			if (end)
			{
				end_stripes(c);
			}
		}
		else if (sym == QLatin1String("end"))
		{
			end_stripes(c);
		}
		else if (sym == QLatin1String("pedestrian"))
		{
			person(c, 0.5, def.shape == QLatin1String("triangle") ? 0.60 : 0.52, 1.25, fg);
		}
		else if (sym == QLatin1String("pedestrian_zebra"))
		{
			person(c, 0.5, 0.58, 1.05, fg);
			fill(c, fg);
			for (int i = 0; i < 3; ++i)
			{
				c.p.drawRect(QRectF(c.pt(0.38 + i * 0.09, 0.80), c.pt(0.43 + i * 0.09, 0.84)));
			}
		}
		else if (sym == QLatin1String("children"))
		{
			person(c, 0.40, 0.62, 1.0, fg);
			person(c, 0.60, 0.66, 0.85, fg);
		}
		else if (sym == QLatin1String("info_pedestrian") || sym == QLatin1String("info_children"))
		{
			fill(c, Qt::white);
			c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.5, 0.16, 0.86, 0.80, 0.14, 0.80 }));
			if (sym == QLatin1String("info_pedestrian"))
			{
				person(c, 0.5, 0.58, 1.0, QColor(20, 20, 20));
			}
			else
			{
				person(c, 0.44, 0.60, 0.85, QColor(20, 20, 20));
				person(c, 0.58, 0.64, 0.70, QColor(20, 20, 20));
			}
		}
		else if (sym == QLatin1String("ped_zone"))
		{
			person(c, 0.40, 0.48, 1.0, fg);
			person(c, 0.62, 0.54, 0.75, fg);
		}
		else if (sym == QLatin1String("living"))
		{
			pic_house(c, b.sub(0.0, 0.0, 0.40, 0.50), fg);
			pic_person(c, b.sub(0.46, 0.0, 0.80, 0.56), fg);
			pic_car_side(c, b.sub(0.10, 0.62, 0.90, 1.0), fg, def.bg);
		}
		else if (sym == QLatin1String("ped_bike_shared"))
		{
			pic_person(c, b.sub(0.34, 0.0, 0.66, 0.44), fg);
			c.p.setPen(QPen(fg, c.len(0.03)));
			c.p.drawLine(P(c, b, 0.05, 0.50), P(c, b, 0.95, 0.50));
			pic_cycle(c, b.sub(0.20, 0.56, 0.80, 1.0), fg, false);
		}
		else if (sym == QLatin1String("ped_bike_split"))
		{
			pic_person(c, b.sub(0.06, 0.20, 0.42, 0.86), fg);
			c.p.setPen(QPen(fg, c.len(0.03)));
			c.p.drawLine(P(c, b, 0.50, 0.04), P(c, b, 0.50, 0.96));
			pic_cycle(c, b.sub(0.56, 0.34, 0.98, 0.74), fg, false);
		}
		else if (sym == QLatin1String("bicycle"))
		{
			wheel_vehicle(c, fg, false);
		}
		else if (sym == QLatin1String("motorcycle"))
		{
			wheel_vehicle(c, fg, true);
		}
		else if (sym == QLatin1String("moped"))
		{
			pic_cycle(c, b.sub(0.06, 0.14, 0.94, 0.86), fg, false);
			fill(c, fg);
			disc(c, b, 0.50, 0.66, 0.07);
		}
		else if (sym == QLatin1String("car") || sym == QLatin1String("bus") || sym == QLatin1String("trailer"))
		{
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(fg);
			const double x0 = sym == QLatin1String("bus") ? 0.18 : 0.24;
			const double x1 = sym == QLatin1String("bus") ? 0.82 : 0.76;
			c.p.drawRoundedRect(QRectF(c.pt(x0, 0.38), c.pt(x1, 0.62)), c.len(0.04), c.len(0.04));
			if (sym == QLatin1String("car"))
			{
				c.p.drawRoundedRect(QRectF(c.pt(0.34, 0.28), c.pt(0.64, 0.40)), c.len(0.04), c.len(0.04));
			}
			c.p.setBrush(bg);
			c.p.drawEllipse(c.pt(x0 + 0.12, 0.63), c.len(0.06), c.len(0.06));
			c.p.drawEllipse(c.pt(x1 - 0.12, 0.63), c.len(0.06), c.len(0.06));
			if (sym == QLatin1String("trailer"))
			{
				c.p.setBrush(fg);
				c.p.drawRect(QRectF(c.pt(0.10, 0.50), c.pt(0.24, 0.53)));
			}
		}
		else if (sym == QLatin1String("car_side"))
		{
			pic_car_side(c, b.sub(0.04, 0.24, 0.96, 0.80), fg, bg);
		}
		else if (sym == QLatin1String("car_front"))
		{
			pic_car_front(c, b.sub(0.12, 0.14, 0.88, 0.86), fg, bg);
		}
		else if (sym == QLatin1String("truck"))
		{
			pic_truck(c, b.sub(0.0, 0.24, 1.0, 0.80), fg, bg);
		}
		else if (sym == QLatin1String("bus_side"))
		{
			pic_bus_side(c, b.sub(0.0, 0.22, 1.0, 0.80), fg, bg);
		}
		else if (sym == QLatin1String("tram"))
		{
			pic_tram(c, b.sub(0.0, 0.16, 1.0, 0.86), fg, bg);
		}
		else if (sym == QLatin1String("tractor"))
		{
			pic_tractor(c, b.sub(0.04, 0.16, 0.96, 0.86), fg, bg);
		}
		else if (sym == QLatin1String("explosive") || sym == QLatin1String("dangerous") || sym == QLatin1String("water_pollution"))
		{
			pic_truck(c, b.sub(0.0, 0.22, 1.0, 0.70), fg, bg);
			if (sym == QLatin1String("explosive"))
			{
				fill(c, sign_orange);
				QPolygonF star;
				const QPointF ctr = P(c, b, 0.34, 0.40);
				for (int i = 0; i < 16; ++i)
				{
					const double a = i * M_PI / 8.0;
					const double rad = c.len((i % 2 ? 0.07 : 0.15) * b.w());
					star << ctr + QPointF(std::cos(a) * rad, std::sin(a) * rad);
				}
				c.p.drawPolygon(star);
			}
			else if (sym == QLatin1String("dangerous"))
			{
				fill(c, sign_orange);
				rect(c, b, 0.18, 0.30, 0.50, 0.50, 0.02);
				pen(c, QColor(20, 20, 20), 0.015);
				rect(c, b, 0.18, 0.30, 0.50, 0.50, 0.02);
			}
			else
			{
				pen(c, QColor(40, 90, 170), 0.04 * b.w());
				QPainterPath w;
				w.moveTo(P(c, b, 0.0, 0.90));
				w.cubicTo(P(c, b, 0.16, 0.82), P(c, b, 0.34, 0.98), P(c, b, 0.50, 0.90));
				w.cubicTo(P(c, b, 0.66, 0.82), P(c, b, 0.84, 0.98), P(c, b, 1.0, 0.90));
				c.p.drawPath(w);
			}
		}
		else if (sym == QLatin1String("car_moto"))
		{
			pic_car_side(c, b.sub(0.12, 0.04, 0.88, 0.46), fg, bg);
			pic_cycle(c, b.sub(0.24, 0.54, 0.76, 0.98), fg, true);
		}
		else if (sym == QLatin1String("car_cart"))
		{
			pic_car_side(c, b.sub(0.12, 0.04, 0.88, 0.46), fg, bg);
			pen(c, fg, 0.05 * b.w());
			c.p.drawEllipse(P(c, b, 0.60, 0.80), c.len(0.14 * b.w()), c.len(0.14 * b.w()));
			line(c, b, 0.20, 0.66, 0.74, 0.66);
		}
		else if (sym == QLatin1String("handcart"))
		{
			pic_handcart(c, b.sub(0.06, 0.12, 0.94, 0.88), fg);
		}
		else if (sym == QLatin1String("cart"))
		{
			c.p.setPen(QPen(fg, c.len(0.05), Qt::SolidLine, Qt::RoundCap));
			c.p.setBrush(Qt::NoBrush);
			c.p.drawEllipse(c.pt(0.62, 0.62), c.len(0.12), c.len(0.12));
			c.p.drawLine(c.pt(0.30, 0.50), c.pt(0.74, 0.50));
			c.p.drawLine(c.pt(0.30, 0.50), c.pt(0.24, 0.68));
		}
		else if (sym == QLatin1String("horse"))
		{
			pic_horse(c, b.sub(0.08, 0.10, 0.92, 0.92), fg, true);
		}
		else if (sym == QLatin1String("cow"))
		{
			pic_cow(c, b.sub(0.0, 0.10, 1.0, 0.94), fg);
		}
		else if (sym == QLatin1String("deer"))
		{
			pic_deer(c, b.sub(0.0, 0.0, 1.0, 1.0), fg);
		}
		else if (sym == QLatin1String("plane"))
		{
			pic_plane(c, b.sub(0.0, 0.0, 1.0, 1.0), fg);
		}
		else if (sym == QLatin1String("wind"))
		{
			pic_windsock(c, b.sub(0.06, 0.0, 0.94, 1.0), fg);
		}
		else if (sym == QLatin1String("rocks"))
		{
			pic_rocks(c, b.sub(0.06, 0.0, 0.94, 1.0), fg);
		}
		else if (sym == QLatin1String("gravel"))
		{
			pic_gravel(c, b, fg, bg);
		}
		else if (sym == QLatin1String("snowflake"))
		{
			pic_snowflake(c, b.sub(0.14, 0.04, 0.86, 0.96), fg);
		}
		else if (sym == QLatin1String("chains"))
		{
			pic_tire(c, b.sub(0.14, 0.04, 0.86, 0.96), fg);
		}
		else if (sym == QLatin1String("slope_down") || sym == QLatin1String("slope_up"))
		{
			const bool down = (sym == QLatin1String("slope_down"));
			fill(c, fg);
			c.p.drawPolygon(down ? poly(c, b, { 0.0, 0.40, 1.0, 1.0, 0.0, 1.0 }) : poly(c, b, { 0.0, 1.0, 1.0, 0.40, 1.0, 1.0 }));
			fit_text(c, def.text.isEmpty() ? QStringLiteral("%10") : def.text,
				QRectF(b.x(down ? 0.46 : 0.0), b.y(0.0), b.w() * 0.54, b.h() * 0.38), fg, 0.12);
		}
		else if (sym == QLatin1String("soft_verge"))
		{
			pic_car_side(c, b.sub(0.0, 0.10, 0.62, 0.56), fg, bg);
			pen(c, fg, 0.07 * b.w(), Qt::FlatCap);
			c.p.drawPolyline(poly(c, b, { 0.0, 0.66, 0.62, 0.66, 0.62, 0.90, 1.0, 0.90 }));
		}
		else if (sym == QLatin1String("queue"))
		{
			car_shape(c, 0.5, 0.56, 0.10, fg);
			car_shape(c, 0.5, 0.76, 0.13, fg);
		}
		else if (sym == QLatin1String("cross_priority"))
		{
			c.p.setPen(QPen(fg, c.len(0.10), Qt::SolidLine, Qt::FlatCap));
			c.p.drawLine(c.pt(0.50, 0.40), c.pt(0.50, 0.84));
			c.p.setPen(QPen(fg, c.len(0.05), Qt::SolidLine, Qt::FlatCap));
			c.p.drawLine(c.pt(0.32, 0.62), c.pt(0.68, 0.62));
		}
		else if (sym == QLatin1String("side_right") || sym == QLatin1String("side_right_acute"))
		{
			c.p.setPen(QPen(fg, c.len(0.10), Qt::SolidLine, Qt::FlatCap));
			c.p.drawLine(c.pt(0.46, 0.40), c.pt(0.46, 0.84));
			c.p.setPen(QPen(fg, c.len(0.05), Qt::SolidLine, Qt::FlatCap));
			if (sym == QLatin1String("side_right"))
			{
				c.p.drawLine(c.pt(0.46, 0.62), c.pt(0.68, 0.62));
			}
			else
			{
				c.p.drawLine(c.pt(0.46, 0.66), c.pt(0.66, 0.52));
			}
		}
		else if (sym == QLatin1String("merge_right"))
		{
			c.p.setPen(QPen(fg, c.len(0.09), Qt::SolidLine, Qt::FlatCap));
			c.p.drawLine(c.pt(0.44, 0.40), c.pt(0.44, 0.84));
			QPainterPath m;
			m.moveTo(c.pt(0.66, 0.84));
			m.quadTo(c.pt(0.66, 0.66), c.pt(0.48, 0.56));
			c.p.setPen(QPen(fg, c.len(0.06), Qt::SolidLine, Qt::FlatCap));
			c.p.setBrush(Qt::NoBrush);
			c.p.drawPath(m);
		}
		else if (sym == QLatin1String("saltire2"))
		{
			// two or more tracks: a small second cross under the main cross
			const QPointF ctr = c.pt(0.5, 0.90);
			for (double angle : { 45.0, -45.0 })
			{
				QTransform t;
				t.translate(ctr.x(), ctr.y());
				t.rotate(angle);
				QPainterPath o;
				o.addRect(QRectF(-c.len(0.14), -c.len(0.06), c.len(0.28), c.len(0.12)));
				QPainterPath i;
				i.addRect(QRectF(-c.len(0.12), -c.len(0.04), c.len(0.24), c.len(0.08)));
				fill(c, def.border);
				c.p.drawPath(t.map(o));
				fill(c, def.bg);
				c.p.drawPath(t.map(i));
			}
		}
		else if (sym == QLatin1String("stripes1") || sym == QLatin1String("stripes2") || sym == QLatin1String("stripes3"))
		{
			board_stripes(c, QRectF(0.33, 0.07, 0.34, 0.86), sym.right(1).toInt(), false, sign_red, Qt::white);
		}
		else if (sym == QLatin1String("hazard_red") || sym == QLatin1String("hazard_black") || sym == QLatin1String("hazard_yellow"))
		{
			const QColor a = sym == QLatin1String("hazard_red") ? sign_red : QColor(20, 20, 20);
			const QColor b2 = sym == QLatin1String("hazard_yellow") ? QColor(250, 196, 30) : QColor(Qt::white);
			board_stripes(c, QRectF(0.33, 0.07, 0.34, 0.86), 0, true, a, b2);
		}
		else if (sym == QLatin1String("island_board"))
		{
			board_stripes(c, QRectF(0.33, 0.07, 0.17, 0.86), 0, true, QColor(20, 20, 20), Qt::white);
			c.p.save();
			c.p.translate(c.pt(1.0, 0.0));
			c.p.scale(-1.0, 1.0);
			board_stripes(c, QRectF(0.33, 0.07, 0.17, 0.86), 0, true, QColor(20, 20, 20), Qt::white);
			c.p.restore();
		}
		else if (sym == QLatin1String("chevron"))
		{
			fill(c, Qt::white);
			for (int i = 0; i < 3; ++i)
			{
				const double x = 0.20 + i * 0.22;
				c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { x, 0.34, x + 0.10, 0.34, x + 0.20, 0.50, x + 0.10, 0.66, x, 0.66, x + 0.10, 0.50 }));
			}
		}
		else if (sym == QLatin1String("horn"))
		{
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(fg);
			QPolygonF h;
			h << c.pt(0.28, 0.44) << c.pt(0.44, 0.44) << c.pt(0.70, 0.30) << c.pt(0.70, 0.70) << c.pt(0.44, 0.56) << c.pt(0.28, 0.56);
			c.p.drawPolygon(h);
			red_slash(c, def.border);
		}
		else if (sym == QLatin1String("oncoming") || sym == QLatin1String("priority_oncoming"))
		{
			// TT-3: give way (red arrow = own direction); B-37: priority (white arrow = own direction)
			const bool own_priority = (sym == QLatin1String("priority_oncoming"));
			arrow(c, c.pt(0.38, 0.22), c.pt(0.38, 0.80), own_priority ? 0.06 : 0.08, own_priority ? sign_red : QColor(20, 20, 20));
			arrow(c, c.pt(0.62, 0.80), c.pt(0.62, 0.22), own_priority ? 0.09 : 0.06, own_priority ? fg : sign_red);
		}
		else if (sym == QLatin1String("width") || sym == QLatin1String("height"))
		{
			const bool w = (sym == QLatin1String("width"));
			fill(c, fg);
			if (w)
			{
				c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.18, 0.40, 0.28, 0.50, 0.18, 0.60 }));
				c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.82, 0.40, 0.72, 0.50, 0.82, 0.60 }));
				fit_text(c, def.text, QRectF(0.29, 0.38, 0.42, 0.24), fg, 0.20);
			}
			else
			{
				c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.40, 0.18, 0.50, 0.28, 0.60, 0.18 }));
				c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.40, 0.82, 0.50, 0.72, 0.60, 0.82 }));
				fit_text(c, def.text, QRectF(0.24, 0.36, 0.52, 0.28), fg, 0.20);
			}
		}
		else if (sym == QLatin1String("length") || sym == QLatin1String("axle") || sym == QLatin1String("distance"))
		{
			if (sym == QLatin1String("distance"))
			{
				pic_truck(c, box{ 0.20, 0.30, 0.46, 0.50 }, fg, bg);
				pic_car_side(c, box{ 0.54, 0.32, 0.80, 0.50 }, fg, bg);
			}
			else
			{
				pic_truck(c, box{ 0.24, 0.28, 0.76, 0.52 }, fg, bg);
			}
			fit_text(c, def.text, QRectF(0.26, 0.56, 0.48, 0.20), fg, 0.18);
		}
		else if (sym == QLatin1String("adr_left") || sym == QLatin1String("adr_up"))
		{
			if (sym == QLatin1String("adr_up"))
			{
				arrow(c, c.pt(0.5, 0.80), c.pt(0.5, 0.24), 0.09, fg);
			}
			else
			{
				arrow(c, c.pt(0.74, 0.62), c.pt(0.22, 0.62), 0.09, fg);
			}
			fill(c, sign_orange);
			c.p.drawRect(QRectF(c.pt(0.54, 0.26), c.pt(0.76, 0.42)));
			c.p.setPen(QPen(QColor(20, 20, 20), c.len(0.015)));
			c.p.setBrush(Qt::NoBrush);
			c.p.drawRect(QRectF(c.pt(0.54, 0.26), c.pt(0.76, 0.42)));
		}
		else if (sym == QLatin1String("bend_right") || sym == QLatin1String("bend_left"))
		{
			const bool right = (sym == QLatin1String("bend_right"));
			QPainterPath path;
			path.moveTo(c.pt(right ? 0.42 : 0.58, 0.82));
			path.lineTo(c.pt(right ? 0.42 : 0.58, 0.58));
			path.quadTo(c.pt(right ? 0.42 : 0.58, 0.44), c.pt(right ? 0.56 : 0.44, 0.40));
			c.p.setPen(QPen(fg, c.len(0.07), Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
			c.p.setBrush(Qt::NoBrush);
			c.p.drawPath(path);
			arrow(c, c.pt(right ? 0.54 : 0.46, 0.41), c.pt(right ? 0.66 : 0.34, 0.37), 0.07, fg);
		}
		else if (sym == QLatin1String("double_bend"))
		{
			QPainterPath path;
			path.moveTo(c.pt(0.44, 0.84));
			path.cubicTo(c.pt(0.44, 0.66), c.pt(0.60, 0.66), c.pt(0.56, 0.52));
			path.cubicTo(c.pt(0.52, 0.42), c.pt(0.44, 0.44), c.pt(0.46, 0.36));
			c.p.setPen(QPen(fg, c.len(0.07), Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
			c.p.setBrush(Qt::NoBrush);
			c.p.drawPath(path);
			arrow(c, c.pt(0.46, 0.40), c.pt(0.48, 0.30), 0.07, fg);
		}
		else if (sym.startsWith(QLatin1String("narrows")))
		{
			const bool l = sym != QLatin1String("narrows_right");
			const bool r = sym != QLatin1String("narrows_left");
			c.p.setPen(QPen(fg, c.len(0.06), Qt::SolidLine, Qt::FlatCap));
			QPolygonF left;
			left << c.pt(0.36, 0.84) << c.pt(0.36, 0.64) << (l ? c.pt(0.44, 0.52) : c.pt(0.36, 0.52)) << (l ? c.pt(0.44, 0.38) : c.pt(0.36, 0.38));
			QPolygonF right;
			right << c.pt(0.64, 0.84) << c.pt(0.64, 0.64) << (r ? c.pt(0.56, 0.52) : c.pt(0.64, 0.52)) << (r ? c.pt(0.56, 0.38) : c.pt(0.64, 0.38));
			c.p.drawPolyline(left);
			c.p.drawPolyline(right);
		}
		else if (sym == QLatin1String("bridge"))
		{
			c.p.setPen(QPen(fg, c.len(0.06), Qt::SolidLine, Qt::FlatCap));
			c.p.drawLine(c.pt(0.24, 0.76), c.pt(0.42, 0.76));
			c.p.drawLine(c.pt(0.58, 0.76), c.pt(0.76, 0.76));
			c.p.drawLine(c.pt(0.42, 0.76), c.pt(0.48, 0.48));
			c.p.drawLine(c.pt(0.58, 0.76), c.pt(0.52, 0.48));
		}
		else if (sym == QLatin1String("quay"))
		{
			car_shape(c, 0.5, 0.52, 0.14, fg);
			c.p.setPen(QPen(QColor(40, 90, 170), c.len(0.04)));
			for (int i = 0; i < 2; ++i)
			{
				QPainterPath w;
				const double y = 0.74 + i * 0.07;
				w.moveTo(c.pt(0.26, y));
				w.cubicTo(c.pt(0.36, y - 0.04), c.pt(0.44, y + 0.04), c.pt(0.52, y));
				w.cubicTo(c.pt(0.60, y - 0.04), c.pt(0.68, y + 0.04), c.pt(0.76, y));
				c.p.setBrush(Qt::NoBrush);
				c.p.drawPath(w);
			}
		}
		else if (sym == QLatin1String("hump"))
		{
			QPainterPath h;
			h.moveTo(c.pt(0.22, 0.76));
			h.lineTo(c.pt(0.36, 0.76));
			h.quadTo(c.pt(0.50, 0.46), c.pt(0.64, 0.76));
			h.lineTo(c.pt(0.78, 0.76));
			c.p.setPen(QPen(fg, c.len(0.07), Qt::SolidLine, Qt::FlatCap, Qt::RoundJoin));
			c.p.setBrush(Qt::NoBrush);
			c.p.drawPath(h);
		}
		else if (sym == QLatin1String("works"))
		{
			person(c, 0.46, 0.62, 1.0, fg);
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(fg);
			QPolygonF heap;
			heap << c.pt(0.56, 0.80) << c.pt(0.66, 0.66) << c.pt(0.76, 0.80);
			c.p.drawPolygon(heap);
		}
		else if (sym == QLatin1String("signals"))
		{
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(QColor(30, 30, 30));
			c.p.drawRoundedRect(QRectF(c.pt(0.42, 0.40), c.pt(0.58, 0.84)), c.len(0.04), c.len(0.04));
			const QColor lamp[3] = { sign_red, QColor(245, 190, 30), QColor(40, 160, 80) };
			for (int i = 0; i < 3; ++i)
			{
				c.p.setBrush(lamp[i]);
				c.p.drawEllipse(c.pt(0.50, 0.48 + i * 0.14), c.len(0.05), c.len(0.05));
			}
		}
		else if (sym == QLatin1String("slippery"))
		{
			car_shape(c, 0.5, 0.48, 0.14, fg);
			c.p.setPen(QPen(fg, c.len(0.035), Qt::SolidLine, Qt::RoundCap));
			c.p.setBrush(Qt::NoBrush);
			for (int i = 0; i < 2; ++i)
			{
				QPainterPath s;
				const double x = 0.40 + i * 0.20;
				s.moveTo(c.pt(x, 0.86));
				s.cubicTo(c.pt(x - 0.06, 0.80), c.pt(x + 0.06, 0.76), c.pt(x, 0.70));
				c.p.drawPath(s);
			}
		}
		else if (sym == QLatin1String("train"))
		{
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(fg);
			c.p.drawRect(QRectF(c.pt(0.26, 0.50), c.pt(0.70, 0.72)));
			c.p.drawRect(QRectF(c.pt(0.52, 0.40), c.pt(0.70, 0.52)));
			c.p.drawRect(QRectF(c.pt(0.30, 0.42), c.pt(0.38, 0.52)));
			c.p.setBrush(bg);
			c.p.drawEllipse(c.pt(0.36, 0.74), c.len(0.05), c.len(0.05));
			c.p.drawEllipse(c.pt(0.60, 0.74), c.len(0.05), c.len(0.05));
		}
		else if (sym == QLatin1String("train_info"))
		{
			pic_tram(c, b.sub(0.0, 0.20, 1.0, 0.86), fg, bg);
		}
		else if (sym == QLatin1String("fence"))
		{
			c.p.setPen(Qt::NoPen);
			for (int i = 0; i < 4; ++i)
			{
				c.p.setBrush(i % 2 ? sign_red : fg);
				c.p.drawRect(QRectF(c.pt(0.24 + i * 0.13, 0.56), c.pt(0.37 + i * 0.13, 0.68)));
			}
			c.p.setBrush(fg);
			c.p.drawRect(QRectF(c.pt(0.26, 0.68), c.pt(0.30, 0.80)));
			c.p.drawRect(QRectF(c.pt(0.70, 0.68), c.pt(0.74, 0.80)));
		}
		else if (sym == QLatin1String("two_way"))
		{
			arrow(c, c.pt(0.42, 0.82), c.pt(0.42, 0.40), 0.06, fg);
			arrow(c, c.pt(0.58, 0.40), c.pt(0.58, 0.82), 0.06, fg);
		}
		else if (sym == QLatin1String("two_way_info"))
		{
			arrow(c, c.pt(0.40, 0.80), c.pt(0.40, 0.20), 0.08, fg);
			arrow(c, c.pt(0.60, 0.20), c.pt(0.60, 0.80), 0.08, fg);
		}
		else if (sym == QLatin1String("exclamation"))
		{
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(fg);
			c.p.drawRoundedRect(QRectF(c.pt(0.46, 0.36), c.pt(0.54, 0.68)), c.len(0.03), c.len(0.03));
			c.p.drawEllipse(c.pt(0.50, 0.76), c.len(0.045), c.len(0.045));
		}
		else if (sym == QLatin1String("cross"))
		{
			c.p.setPen(QPen(fg, c.len(0.08), Qt::SolidLine, Qt::FlatCap));
			c.p.drawLine(c.pt(0.50, 0.36), c.pt(0.50, 0.84));
			c.p.drawLine(c.pt(0.30, 0.60), c.pt(0.70, 0.60));
		}
		else if (sym == QLatin1String("priority") || sym == QLatin1String("end_priority"))
		{
			QPolygonF d;
			d << c.pt(0.5, 0.24) << c.pt(0.76, 0.5) << c.pt(0.5, 0.76) << c.pt(0.24, 0.5);
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(sym == QLatin1String("priority") ? QColor(250, 196, 30) : QColor(200, 200, 200));
			c.p.drawPolygon(d);
			if (sym == QLatin1String("end_priority"))
			{
				end_stripes(c);
			}
		}
		else if (sym == QLatin1String("letter"))
		{
			if (def.panel)
			{
				fit_text(c, def.text, QRectF(b.x0, b.y0, b.w(), b.h()), fg, 0.36);
				return;
			}
			font.setPixelSize(static_cast<int>(c.len(0.62)));
			c.p.setFont(font);
			c.p.setPen(fg);
			c.p.drawText(c.r, Qt::AlignCenter, def.text);
		}
		else if (sym == QLatin1String("one_way"))
		{
			arrow(c, c.pt(0.5, 0.82), c.pt(0.5, 0.20), 0.12, fg);
		}
		else if (sym == QLatin1String("dead_end"))
		{
			c.p.setPen(Qt::NoPen);
			c.p.setBrush(fg);
			c.p.drawRect(QRectF(c.pt(0.42, 0.40), c.pt(0.58, 0.84)));
			c.p.setBrush(sign_red);
			c.p.drawRect(QRectF(c.pt(0.26, 0.24), c.pt(0.74, 0.40)));
		}
		else if (sym == QLatin1String("dead_end_side"))
		{
			fill(c, fg);
			c.p.drawRect(QRectF(c.pt(0.30, 0.18), c.pt(0.44, 0.84)));
			c.p.drawRect(QRectF(c.pt(0.44, 0.44), c.pt(0.70, 0.56)));
			fill(c, sign_red);
			c.p.drawRect(QRectF(c.pt(0.70, 0.36), c.pt(0.80, 0.64)));
		}
		else if (sym == QLatin1String("indirect_left"))
		{
			c.p.setPen(QPen(fg, c.len(0.07), Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
			c.p.setBrush(Qt::NoBrush);
			QPolygonF path;
			path << c.pt(0.40, 0.84) << c.pt(0.40, 0.24) << c.pt(0.72, 0.24) << c.pt(0.72, 0.56);
			c.p.drawPolyline(path);
			arrow(c, c.pt(0.72, 0.56), c.pt(0.22, 0.56), 0.07, fg);
		}
		else if (sym == QLatin1String("lanes"))
		{
			arrow(c, c.pt(0.5, 0.80), c.pt(0.5, 0.24), 0.07, fg);
			arrow(c, c.pt(0.30, 0.80), c.pt(0.30, 0.24), 0.07, fg);
			QPainterPath r;
			r.moveTo(c.pt(0.70, 0.80));
			r.lineTo(c.pt(0.70, 0.48));
			c.p.setPen(QPen(fg, c.len(0.07), Qt::SolidLine, Qt::FlatCap));
			c.p.drawPath(r);
			arrow(c, c.pt(0.70, 0.50), c.pt(0.84, 0.30), 0.07, fg);
		}
		else if (sym == QLatin1String("speed_panel"))
		{
			fit_text(c, QStringLiteral("50"), QRectF(0.20, 0.16, 0.60, 0.20), fg, 0.18);
			fit_text(c, QStringLiteral("90 / 110"), QRectF(0.16, 0.40, 0.68, 0.20), fg, 0.16);
			fit_text(c, QStringLiteral("120"), QRectF(0.20, 0.64, 0.60, 0.20), fg, 0.18);
		}
		else if (sym == QLatin1String("motorway"))
		{
			fill(c, fg);
			c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.22, 0.84, 0.40, 0.42, 0.46, 0.42, 0.40, 0.84 }));
			c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.60, 0.84, 0.54, 0.42, 0.60, 0.42, 0.78, 0.84 }));
			c.p.drawRect(QRectF(c.pt(0.20, 0.26), c.pt(0.80, 0.36)));
			c.p.drawRect(QRectF(c.pt(0.26, 0.36), c.pt(0.32, 0.44)));
			c.p.drawRect(QRectF(c.pt(0.68, 0.36), c.pt(0.74, 0.44)));
		}
		else if (sym == QLatin1String("escape"))
		{
			fill(c, fg);
			c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.30, 0.86, 0.40, 0.18, 0.48, 0.18, 0.46, 0.86 }));
			c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.46, 0.62, 0.70, 0.26, 0.80, 0.30, 0.52, 0.72 }));
			fill(c, QColor(240, 200, 120));
			c.p.drawPolygon(poly(c, box{ 0, 0, 1, 1 }, { 0.62, 0.38, 0.70, 0.26, 0.80, 0.30, 0.72, 0.44 }));
		}
		else if (sym == QLatin1String("first_aid"))
		{
			pic_first_aid(c, b.sub(0.14, 0.06, 0.86, 0.94));
		}
		else if (sym == QLatin1String("wrench"))
		{
			pic_wrench(c, b.sub(0.08, 0.04, 0.92, 0.96), fg);
		}
		else if (sym == QLatin1String("phone"))
		{
			pic_phone(c, b.sub(0.10, 0.06, 0.90, 0.96), fg);
		}
		else if (sym == QLatin1String("fuel"))
		{
			pic_fuel(c, b.sub(0.14, 0.04, 0.86, 0.96), fg);
		}
		else if (sym == QLatin1String("bed"))
		{
			pic_bed(c, b.sub(0.04, 0.14, 0.96, 0.90), fg);
		}
		else if (sym == QLatin1String("fork_knife"))
		{
			pic_fork_knife(c, b.sub(0.14, 0.04, 0.86, 0.96), fg);
		}
		else if (sym == QLatin1String("cup"))
		{
			pic_cup(c, b.sub(0.10, 0.04, 0.90, 0.96), fg);
		}
		else if (sym == QLatin1String("faucet"))
		{
			pic_faucet(c, b.sub(0.10, 0.06, 0.90, 0.96), fg);
		}
		else if (sym == QLatin1String("picnic"))
		{
			pic_picnic(c, b.sub(0.04, 0.04, 0.96, 0.96), fg);
		}
		else if (sym == QLatin1String("hiker"))
		{
			pic_hiker(c, b.sub(0.14, 0.02, 0.86, 0.98), fg);
		}
		else if (sym == QLatin1String("tent"))
		{
			pic_tent(c, b.sub(0.08, 0.10, 0.92, 0.94), fg);
		}
		else if (sym == QLatin1String("caravan"))
		{
			pic_caravan(c, b.sub(0.04, 0.16, 0.96, 0.90), fg);
		}
		else if (sym == QLatin1String("tent_caravan"))
		{
			pic_tent(c, b.sub(0.0, 0.20, 0.44, 0.86), fg);
			pic_caravan(c, b.sub(0.48, 0.30, 1.0, 0.86), fg);
		}
		else if (sym == QLatin1String("hostel"))
		{
			pic_house(c, b.sub(0.12, 0.06, 0.88, 0.94), fg);
		}
		else if (sym == QLatin1String("flame"))
		{
			pic_flame(c, b.sub(0.18, 0.02, 0.82, 0.98));
		}
		else if (sym == QLatin1String("swimmer"))
		{
			pic_swimmer(c, b.sub(0.02, 0.10, 0.98, 0.94), fg);
		}
		else if (sym == QLatin1String("underpass") || sym == QLatin1String("overpass"))
		{
			pic_stairs(c, b.sub(0.08, 0.04, 0.92, 0.96), fg, sym == QLatin1String("underpass"));
		}
		else if (sym == QLatin1String("tunnel") || sym == QLatin1String("tunnel_water"))
		{
			pic_tunnel(c, b.sub(0.10, 0.12, 0.90, 0.88), fg, sym == QLatin1String("tunnel_water"));
		}
		else if (sym == QLatin1String("camera"))
		{
			pic_camera(c, b.sub(0.08, 0.10, 0.92, 0.90), fg);
		}
		else if (sym == QLatin1String("park_sidewalk") || sym == QLatin1String("park_half") || sym == QLatin1String("park_garage")
			|| sym == QLatin1String("park_metro") || sym == QLatin1String("park_tram"))
		{
			fit_text(c, QStringLiteral("P"), QRectF(0.20, 0.10, 0.60, 0.46), fg, 0.46);
			if (sym == QLatin1String("park_garage"))
			{
				c.p.setPen(QPen(fg, c.len(0.05), Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
				c.p.setBrush(Qt::NoBrush);
				QPolygonF roof;
				roof << c.pt(0.18, 0.30) << c.pt(0.50, 0.10) << c.pt(0.82, 0.30);
				c.p.drawPolyline(roof);
			}
			else if (sym == QLatin1String("park_metro"))
			{
				fit_text(c, QStringLiteral("M"), QRectF(0.30, 0.58, 0.40, 0.28), fg, 0.24);
			}
			else if (sym == QLatin1String("park_tram"))
			{
				pic_tram(c, box{ 0.26, 0.58, 0.74, 0.84 }, fg, def.bg);
			}
			else
			{
				const bool half = (sym == QLatin1String("park_half"));
				pic_car_side(c, box{ 0.26, half ? 0.60 : 0.56, 0.74, half ? 0.80 : 0.76 }, fg, def.bg);
				fill(c, fg);
				c.p.drawRect(QRectF(c.pt(half ? 0.50 : 0.20, 0.80), c.pt(0.80, 0.84)));
			}
		}
		else if (sym == QLatin1String("no_parking") || sym == QLatin1String("no_stopping"))
		{
			c.p.setPen(QPen(def.border, c.len(0.09), Qt::SolidLine, Qt::FlatCap));
			c.p.drawLine(c.pt(0.22, 0.22), c.pt(0.78, 0.78));
			if (sym == QLatin1String("no_stopping"))
			{
				c.p.drawLine(c.pt(0.78, 0.22), c.pt(0.22, 0.78));
			}
		}
	}

	// Path of the outer edge and of the inner face of a shape.
	void shape_paths(const sign_def &def, const QRectF &r, QPainterPath *outer, QPainterPath *inner)
	{
		const double rw = r.width();
		const QString &sh = def.shape;
		if (sh == QLatin1String("triangle") || sh == QLatin1String("triangle_down"))
		{
			const bool down = (sh == QLatin1String("triangle_down"));
			auto tri = [&](double inset)
			{
				QPainterPath t;
				const double h = (r.height() - 2 * inset) * 0.866;
				const double top = r.top() + (r.height() - h) / 2.0 + (down ? 0.0 : inset * 0.3);
				if (!down)
				{
					t.moveTo(r.center().x(), top);
					t.lineTo(r.right() - inset, top + h);
					t.lineTo(r.left() + inset, top + h);
				}
				else
				{
					t.moveTo(r.left() + inset, top);
					t.lineTo(r.right() - inset, top);
					t.lineTo(r.center().x(), top + h);
				}
				t.closeSubpath();
				return t;
			};
			*outer = tri(0.0);
			*inner = tri(rw * 0.13);
		}
		else if (sh == QLatin1String("octagon"))
		{
			auto oct = [&](double inset)
			{
				QPainterPath o;
				const QPointF c = r.center();
				const double rad = (rw / 2.0 - inset) / std::cos(M_PI / 8.0);
				for (int i = 0; i < 8; ++i)
				{
					const double a = M_PI / 8.0 + i * M_PI / 4.0;
					const QPointF q(c.x() + rad * std::cos(a), c.y() + rad * std::sin(a));
					if (i == 0) { o.moveTo(q); } else { o.lineTo(q); }
				}
				o.closeSubpath();
				return o;
			};
			*outer = oct(0.0);
			*inner = oct(rw * 0.06);
		}
		else if (sh == QLatin1String("diamond"))
		{
			auto dia = [&](double inset)
			{
				QPainterPath d;
				const QPointF c = r.center();
				const double h = rw / 2.0 - inset;
				d.moveTo(c.x(), c.y() - h);
				d.lineTo(c.x() + h, c.y());
				d.lineTo(c.x(), c.y() + h);
				d.lineTo(c.x() - h, c.y());
				d.closeSubpath();
				return d;
			};
			*outer = dia(0.0);
			*inner = dia(rw * 0.05);
		}
		else if (sh == QLatin1String("board"))
		{
			// tall board (markers before level crossings, obstacles)
			const QRectF br(r.left() + rw * 0.30, r.top() + rw * 0.04, rw * 0.40, rw * 0.92);
			outer->addRoundedRect(br, rw * 0.03, rw * 0.03);
			inner->addRect(br.adjusted(rw * 0.025, rw * 0.025, -rw * 0.025, -rw * 0.025));
		}
		else if (sh == QLatin1String("board_wide"))
		{
			// wide board (bend direction markers)
			const QRectF br(r.left() + rw * 0.04, r.top() + rw * 0.28, rw * 0.92, rw * 0.44);
			outer->addRoundedRect(br, rw * 0.03, rw * 0.03);
			inner->addRect(br.adjusted(rw * 0.025, rw * 0.025, -rw * 0.025, -rw * 0.025));
		}
		else if (sh == QLatin1String("saltire"))
		{
			// St. Andrew's cross (level crossing)
			for (double angle : { 45.0, -45.0 })
			{
				QTransform t;
				t.translate(r.center().x(), r.center().y() - rw * 0.06);
				t.rotate(angle);
				QPainterPath o;
				o.addRect(QRectF(-rw * 0.46, -rw * 0.09, rw * 0.92, rw * 0.18));
				QPainterPath i;
				i.addRect(QRectF(-rw * 0.44, -rw * 0.065, rw * 0.88, rw * 0.13));
				*outer = outer->united(t.map(o));
				*inner = inner->united(t.map(i));
			}
		}
		else if (sh == QLatin1String("square") || sh == QLatin1String("rect"))
		{
			outer->addRoundedRect(r, rw * 0.12, rw * 0.12);
			inner->addRoundedRect(r.adjusted(rw * 0.05, rw * 0.05, -rw * 0.05, -rw * 0.05), rw * 0.08, rw * 0.08);
		}
		else // circle
		{
			outer->addEllipse(r);
			inner->addEllipse(r.adjusted(rw * 0.11, rw * 0.11, -rw * 0.11, -rw * 0.11));
		}
	}
}

namespace sign_painter
{
	void paint(QPainter &p, const QRectF &rect, const sign_def &def, int value)
	{
		p.save();
		p.setRenderHint(QPainter::Antialiasing, true);
		p.setRenderHint(QPainter::TextAntialiasing, true);
		const double w = rect.width();
		const QRectF r = rect.adjusted(w * 0.03, w * 0.03, -w * 0.03, -w * 0.03);
		const double rw = r.width();
		QPainterPath outer;
		QPainterPath inner;
		shape_paths(def, r, &outer, &inner);
		const QString &sh = def.shape;
		p.setPen(Qt::NoPen);
		p.setBrush(def.border);
		p.drawPath(outer);
		p.setBrush(def.bg);
		p.drawPath(inner);
		if (sh == QLatin1String("octagon") || sh == QLatin1String("square") || sh == QLatin1String("rect"))
		{
			// thin white line inside the edge
			p.setPen(QPen(QColor(255, 255, 255, 200), rw * 0.018));
			p.setBrush(Qt::NoBrush);
			p.drawPath(inner);
		}
		else if (sh == QLatin1String("diamond") || sh == QLatin1String("saltire"))
		{
			// thin dark edge: a white sign is also visible on a light ground
			p.setPen(QPen(QColor(60, 70, 85, 200), rw * 0.012));
			p.setBrush(Qt::NoBrush);
			p.drawPath(outer);
		}
		canvas c{ p, r };
		if (def.panel)
		{
			// white panel of a service sign
			p.setPen(Qt::NoPen);
			p.setBrush(Qt::white);
			p.drawRoundedRect(QRectF(c.pt(0.17, 0.14), c.pt(0.83, 0.70)), rw * 0.05, rw * 0.05);
		}
		if (def.mirror)
		{
			p.save();
			p.translate(r.center());
			p.scale(-1.0, 1.0);
			p.translate(-r.center());
			draw_symbol(c, def, value);
			p.restore();
		}
		else
		{
			draw_symbol(c, def, value);
		}
		if (def.slash)
		{
			p.setPen(QPen(QColor(214, 40, 40), rw * 0.09, Qt::SolidLine, Qt::FlatCap));
			p.drawLine(c.pt(0.76, 0.22), c.pt(0.24, 0.78));
		}
		p.restore();
	}

	QImage image(const sign_def &def, int value, int size)
	{
		QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
		img.fill(Qt::transparent);
		QPainter p(&img);
		paint(p, QRectF(0, 0, size, size), def, value);
		return img;
	}
}
