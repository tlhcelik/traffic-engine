#include "icon_factory.h"
#include "app_theme.h"

#include <QHash>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace
{
	const QColor ink(0x3D, 0x4A, 0x5C);

	QColor tile_color(const QString &n)
	{
		// mint 0, sky 1, peach 2, lavender 3, lemon 4, coral 5, sage 6, rose 7, aqua 8, sand 9
		static const QHash<QString, int> map =
		{
			{ QStringLiteral("select"), 1 }, { QStringLiteral("road"), 1 }, { QStringLiteral("light"), 0 },
			{ QStringLiteral("sign"), 4 }, { QStringLiteral("cone"), 2 }, { QStringLiteral("barrier"), 2 },
			{ QStringLiteral("closure"), 5 }, { QStringLiteral("crossing"), 9 }, { QStringLiteral("bus_stop"), 8 },
			{ QStringLiteral("spawn"), 6 }, { QStringLiteral("erase"), 5 }, { QStringLiteral("new"), 3 },
			{ QStringLiteral("open"), 3 }, { QStringLiteral("save"), 3 }, { QStringLiteral("editor"), 2 },
			{ QStringLiteral("run"), 6 }, { QStringLiteral("pause"), 6 }, { QStringLiteral("step"), 6 },
			{ QStringLiteral("reset"), 6 }, { QStringLiteral("faster"), 0 }, { QStringLiteral("slower"), 0 },
			{ QStringLiteral("charts"), 8 }, { QStringLiteral("settings"), 9 }, { QStringLiteral("dev"), 7 },
			{ QStringLiteral("fit"), 8 }, { QStringLiteral("follow"), 8 }, { QStringLiteral("json"), 3 },
			{ QStringLiteral("program"), 0 }, { QStringLiteral("delete"), 5 }, { QStringLiteral("demo"), 4 },
			{ QStringLiteral("screenshot"), 8 }, { QStringLiteral("properties"), 1 }, { QStringLiteral("signs"), 4 },
			{ QStringLiteral("density"), 2 }, { QStringLiteral("osm"), 6 }, { QStringLiteral("map"), 6 },
			{ QStringLiteral("api"), 3 }, { QStringLiteral("language"), 3 }, { QStringLiteral("congestion"), 5 },
			{ QStringLiteral("bridge"), 1 }, { QStringLiteral("tunnel"), 1 },
		};
		return app_theme::pastel(map.value(n, 1));
	}

	// The glyph area is the unit square; p() maps it to pixels.
	struct glyph
	{
		QPainter &g;
		double s;
		QPointF p(double x, double y) const { return QPointF(x * s, y * s); }
		QRectF r(double x0, double y0, double x1, double y1) const { return QRectF(p(x0, y0), p(x1, y1)); }
		void pen(double w, const QColor &c = ink)
		{
			g.setPen(QPen(c, w * s, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
			g.setBrush(Qt::NoBrush);
		}
		void fill(const QColor &c = ink)
		{
			g.setPen(Qt::NoPen);
			g.setBrush(c);
		}
		void poly(std::initializer_list<QPointF> pts)
		{
			g.drawPolygon(QPolygonF(QVector<QPointF>(pts)));
		}
	};

	void draw_glyph(QPainter &painter, const QString &n, double size)
	{
		glyph g{ painter, size };
		const double w = 0.075;
		if (n == QLatin1String("select"))
		{
			g.fill();
			g.poly({ g.p(0.32, 0.18), g.p(0.32, 0.78), g.p(0.46, 0.64), g.p(0.56, 0.84), g.p(0.64, 0.80), g.p(0.54, 0.60), g.p(0.72, 0.60) });
		}
		else if (n == QLatin1String("road"))
		{
			g.pen(w);
			painter.drawLine(g.p(0.28, 0.84), g.p(0.42, 0.16));
			painter.drawLine(g.p(0.72, 0.84), g.p(0.58, 0.16));
			g.pen(w * 0.8);
			for (int i = 0; i < 3; ++i)
			{
				const double y = 0.74 - i * 0.24;
				painter.drawLine(g.p(0.5, y), g.p(0.5, y - 0.10));
			}
		}
		else if (n == QLatin1String("bridge"))
		{
			// a deck with a parapet over an arch
			g.fill();
			painter.drawRect(g.r(0.12, 0.38, 0.88, 0.48));
			g.pen(w * 0.6);
			painter.drawLine(g.p(0.12, 0.28), g.p(0.88, 0.28));
			for (const double x : { 0.20, 0.35, 0.50, 0.65, 0.80 })
			{
				painter.drawLine(g.p(x, 0.28), g.p(x, 0.38));
			}
			g.pen(w);
			QPainterPath arch;
			arch.moveTo(g.p(0.20, 0.84));
			arch.quadTo(g.p(0.5, 0.44), g.p(0.80, 0.84));
			painter.drawPath(arch);
			painter.drawLine(g.p(0.20, 0.48), g.p(0.20, 0.84));
			painter.drawLine(g.p(0.80, 0.48), g.p(0.80, 0.84));
		}
		else if (n == QLatin1String("tunnel"))
		{
			// a portal in a hill, a road into it
			g.fill(QColor(118, 150, 98));
			QPainterPath hill;
			hill.moveTo(g.p(0.08, 0.80));
			hill.quadTo(g.p(0.5, 0.02), g.p(0.92, 0.80));
			hill.closeSubpath();
			painter.drawPath(hill);
			g.fill(QColor(200, 196, 186));
			QPainterPath frame;
			frame.moveTo(g.p(0.26, 0.80));
			frame.lineTo(g.p(0.26, 0.56));
			frame.arcTo(g.r(0.26, 0.32, 0.74, 0.80), 180.0, -180.0);
			frame.lineTo(g.p(0.74, 0.80));
			frame.closeSubpath();
			painter.drawPath(frame);
			g.fill();
			QPainterPath hole;
			hole.moveTo(g.p(0.34, 0.80));
			hole.lineTo(g.p(0.34, 0.58));
			hole.arcTo(g.r(0.34, 0.42, 0.66, 0.74), 180.0, -180.0);
			hole.lineTo(g.p(0.66, 0.80));
			hole.closeSubpath();
			painter.drawPath(hole);
			g.pen(w * 0.6, Qt::white);
			painter.drawLine(g.p(0.5, 0.92), g.p(0.5, 0.84));
		}
		else if (n == QLatin1String("light") || n == QLatin1String("program"))
		{
			if (n == QLatin1String("program"))
			{
				g.pen(w);
				painter.drawEllipse(g.r(0.18, 0.18, 0.82, 0.82));
				painter.drawLine(g.p(0.5, 0.5), g.p(0.5, 0.28));
				painter.drawLine(g.p(0.5, 0.5), g.p(0.66, 0.58));
				return;
			}
			g.fill();
			painter.drawRoundedRect(g.r(0.36, 0.14, 0.64, 0.86), 0.08 * size, 0.08 * size);
			const QColor lamps[3] = { QColor(255, 120, 110), QColor(255, 214, 102), QColor(120, 210, 140) };
			for (int i = 0; i < 3; ++i)
			{
				g.fill(lamps[i]);
				painter.drawEllipse(g.p(0.5, 0.27 + i * 0.23), 0.085 * size, 0.085 * size);
			}
		}
		else if (n == QLatin1String("sign") || n == QLatin1String("signs"))
		{
			g.fill(QColor(214, 40, 40));
			g.poly({ g.p(0.5, 0.16), g.p(0.86, 0.80), g.p(0.14, 0.80) });
			g.fill(Qt::white);
			g.poly({ g.p(0.5, 0.33), g.p(0.71, 0.70), g.p(0.29, 0.70) });
			g.fill();
			painter.drawRect(g.r(0.475, 0.42, 0.525, 0.58));
			painter.drawEllipse(g.p(0.5, 0.64), 0.03 * size, 0.03 * size);
		}
		else if (n == QLatin1String("cone"))
		{
			g.fill(QColor(250, 140, 70));
			g.poly({ g.p(0.5, 0.14), g.p(0.70, 0.76), g.p(0.30, 0.76) });
			g.fill(Qt::white);
			g.poly({ g.p(0.42, 0.40), g.p(0.58, 0.40), g.p(0.62, 0.52), g.p(0.38, 0.52) });
			g.fill(ink);
			painter.drawRoundedRect(g.r(0.20, 0.76, 0.80, 0.84), 0.03 * size, 0.03 * size);
		}
		else if (n == QLatin1String("barrier") || n == QLatin1String("closure"))
		{
			g.fill(ink);
			painter.drawRect(g.r(0.22, 0.30, 0.28, 0.84));
			painter.drawRect(g.r(0.72, 0.30, 0.78, 0.84));
			for (int i = 0; i < 4; ++i)
			{
				g.fill(i % 2 == 0 ? QColor(230, 80, 70) : QColor(255, 255, 255));
				painter.drawRect(g.r(0.14 + i * 0.18, 0.38, 0.32 + i * 0.18, 0.54));
			}
			if (n == QLatin1String("closure"))
			{
				g.fill(QColor(214, 40, 40));
				painter.drawEllipse(g.p(0.5, 0.22), 0.13 * size, 0.13 * size);
				g.fill(Qt::white);
				painter.drawRect(g.r(0.42, 0.20, 0.58, 0.24));
			}
		}
		else if (n == QLatin1String("crossing"))
		{
			painter.save();
			painter.translate(g.p(0.5, 0.46));
			for (int k = 0; k < 2; ++k)
			{
				painter.rotate(k == 0 ? 45.0 : -90.0);
				for (int i = 0; i < 4; ++i)
				{
					g.fill(i % 2 == 0 ? QColor(214, 40, 40) : QColor(255, 255, 255));
					painter.drawRect(QRectF(-0.34 * size + i * 0.17 * size, -0.05 * size, 0.17 * size, 0.10 * size));
				}
			}
			painter.restore();
			g.fill();
			painter.drawRect(g.r(0.47, 0.60, 0.53, 0.86));
		}
		else if (n == QLatin1String("bus_stop"))
		{
			g.fill();
			painter.drawRoundedRect(g.r(0.22, 0.16, 0.78, 0.76), 0.08 * size, 0.08 * size);
			g.fill(Qt::white);
			painter.drawRoundedRect(g.r(0.28, 0.24, 0.72, 0.48), 0.04 * size, 0.04 * size);
			painter.drawEllipse(g.p(0.33, 0.64), 0.04 * size, 0.04 * size);
			painter.drawEllipse(g.p(0.67, 0.64), 0.04 * size, 0.04 * size);
			g.fill();
			painter.drawRect(g.r(0.26, 0.76, 0.34, 0.86));
			painter.drawRect(g.r(0.66, 0.76, 0.74, 0.86));
		}
		else if (n == QLatin1String("spawn"))
		{
			g.pen(w);
			painter.drawEllipse(g.r(0.16, 0.36, 0.44, 0.64));
			painter.drawLine(g.p(0.44, 0.5), g.p(0.80, 0.5));
			painter.drawLine(g.p(0.68, 0.38), g.p(0.80, 0.5));
			painter.drawLine(g.p(0.68, 0.62), g.p(0.80, 0.5));
			painter.drawLine(g.p(0.30, 0.42), g.p(0.30, 0.58));
			painter.drawLine(g.p(0.22, 0.5), g.p(0.38, 0.5));
		}
		else if (n == QLatin1String("erase") || n == QLatin1String("delete"))
		{
			if (n == QLatin1String("delete"))
			{
				g.pen(w);
				painter.drawLine(g.p(0.24, 0.28), g.p(0.76, 0.28));
				painter.drawLine(g.p(0.42, 0.20), g.p(0.58, 0.20));
				painter.drawRoundedRect(g.r(0.30, 0.34, 0.70, 0.84), 0.05 * size, 0.05 * size);
				painter.drawLine(g.p(0.43, 0.44), g.p(0.43, 0.74));
				painter.drawLine(g.p(0.57, 0.44), g.p(0.57, 0.74));
				return;
			}
			painter.save();
			painter.translate(g.p(0.5, 0.5));
			painter.rotate(-40.0);
			g.fill(QColor(240, 120, 130));
			painter.drawRoundedRect(QRectF(-0.30 * size, -0.14 * size, 0.30 * size, 0.28 * size), 0.04 * size, 0.04 * size);
			g.fill(Qt::white);
			painter.drawRoundedRect(QRectF(0.0, -0.14 * size, 0.30 * size, 0.28 * size), 0.04 * size, 0.04 * size);
			g.pen(w * 0.6);
			painter.drawRoundedRect(QRectF(-0.30 * size, -0.14 * size, 0.60 * size, 0.28 * size), 0.04 * size, 0.04 * size);
			painter.restore();
			g.pen(w * 0.6);
			painter.drawLine(g.p(0.20, 0.84), g.p(0.80, 0.84));
		}
		else if (n == QLatin1String("new") || n == QLatin1String("demo"))
		{
			g.pen(w);
			QPainterPath page;
			page.moveTo(g.p(0.28, 0.16));
			page.lineTo(g.p(0.58, 0.16));
			page.lineTo(g.p(0.72, 0.30));
			page.lineTo(g.p(0.72, 0.84));
			page.lineTo(g.p(0.28, 0.84));
			page.closeSubpath();
			painter.drawPath(page);
			if (n == QLatin1String("demo"))
			{
				g.fill();
				painter.drawEllipse(g.p(0.5, 0.52), 0.10 * size, 0.10 * size);
				return;
			}
			painter.drawLine(g.p(0.5, 0.42), g.p(0.5, 0.68));
			painter.drawLine(g.p(0.37, 0.55), g.p(0.63, 0.55));
		}
		else if (n == QLatin1String("open"))
		{
			g.fill();
			QPainterPath f;
			f.moveTo(g.p(0.16, 0.28));
			f.lineTo(g.p(0.40, 0.28));
			f.lineTo(g.p(0.46, 0.36));
			f.lineTo(g.p(0.80, 0.36));
			f.lineTo(g.p(0.80, 0.78));
			f.lineTo(g.p(0.16, 0.78));
			f.closeSubpath();
			painter.drawPath(f);
			g.fill(Qt::white);
			painter.drawRect(g.r(0.22, 0.44, 0.74, 0.48));
		}
		else if (n == QLatin1String("save"))
		{
			g.fill();
			painter.drawRoundedRect(g.r(0.20, 0.18, 0.80, 0.82), 0.06 * size, 0.06 * size);
			g.fill(Qt::white);
			painter.drawRect(g.r(0.32, 0.22, 0.64, 0.40));
			painter.drawRoundedRect(g.r(0.30, 0.54, 0.70, 0.78), 0.03 * size, 0.03 * size);
		}
		else if (n == QLatin1String("editor"))
		{
			painter.save();
			painter.translate(g.p(0.5, 0.5));
			painter.rotate(45.0);
			g.fill();
			painter.drawRect(QRectF(-0.10 * size, -0.34 * size, 0.20 * size, 0.50 * size));
			g.fill(QColor(255, 200, 150));
			painter.drawRect(QRectF(-0.10 * size, -0.40 * size, 0.20 * size, 0.07 * size));
			g.fill();
			QPolygonF tip;
			tip << QPointF(-0.10 * size, 0.18 * size) << QPointF(0.10 * size, 0.18 * size) << QPointF(0.0, 0.36 * size);
			painter.drawPolygon(tip);
			painter.restore();
		}
		else if (n == QLatin1String("run"))
		{
			g.fill();
			g.poly({ g.p(0.34, 0.20), g.p(0.78, 0.5), g.p(0.34, 0.80) });
		}
		else if (n == QLatin1String("pause"))
		{
			g.fill();
			painter.drawRoundedRect(g.r(0.30, 0.22, 0.44, 0.78), 0.03 * size, 0.03 * size);
			painter.drawRoundedRect(g.r(0.56, 0.22, 0.70, 0.78), 0.03 * size, 0.03 * size);
		}
		else if (n == QLatin1String("step"))
		{
			g.fill();
			g.poly({ g.p(0.26, 0.24), g.p(0.62, 0.5), g.p(0.26, 0.76) });
			painter.drawRoundedRect(g.r(0.64, 0.24, 0.76, 0.76), 0.03 * size, 0.03 * size);
		}
		else if (n == QLatin1String("reset"))
		{
			g.pen(w);
			painter.drawArc(g.r(0.22, 0.22, 0.78, 0.78), 60 * 16, 290 * 16);
			g.fill();
			g.poly({ g.p(0.60, 0.12), g.p(0.78, 0.26), g.p(0.56, 0.34) });
		}
		else if (n == QLatin1String("faster") || n == QLatin1String("slower"))
		{
			g.fill();
			if (n == QLatin1String("faster"))
			{
				g.poly({ g.p(0.18, 0.26), g.p(0.50, 0.5), g.p(0.18, 0.74) });
				g.poly({ g.p(0.48, 0.26), g.p(0.80, 0.5), g.p(0.48, 0.74) });
			}
			else
			{
				g.poly({ g.p(0.82, 0.26), g.p(0.50, 0.5), g.p(0.82, 0.74) });
				g.poly({ g.p(0.52, 0.26), g.p(0.20, 0.5), g.p(0.52, 0.74) });
			}
		}
		else if (n == QLatin1String("charts"))
		{
			g.pen(w);
			painter.drawLine(g.p(0.20, 0.18), g.p(0.20, 0.80));
			painter.drawLine(g.p(0.20, 0.80), g.p(0.84, 0.80));
			QPolygonF line;
			line << g.p(0.26, 0.66) << g.p(0.42, 0.48) << g.p(0.56, 0.58) << g.p(0.78, 0.28);
			painter.drawPolyline(line);
		}
		else if (n == QLatin1String("settings"))
		{
			g.fill();
			painter.save();
			painter.translate(g.p(0.5, 0.5));
			for (int i = 0; i < 8; ++i)
			{
				painter.rotate(45.0);
				painter.drawRoundedRect(QRectF(-0.07 * size, -0.36 * size, 0.14 * size, 0.16 * size), 0.02 * size, 0.02 * size);
			}
			painter.drawEllipse(QPointF(0, 0), 0.24 * size, 0.24 * size);
			g.fill(tile_color(n));
			painter.drawEllipse(QPointF(0, 0), 0.10 * size, 0.10 * size);
			painter.restore();
		}
		else if (n == QLatin1String("dev"))
		{
			g.pen(w);
			painter.drawArc(g.r(0.16, 0.24, 0.84, 0.92), 0, 180 * 16);
			painter.drawLine(g.p(0.5, 0.58), g.p(0.68, 0.36));
			g.fill();
			painter.drawEllipse(g.p(0.5, 0.58), 0.05 * size, 0.05 * size);
		}
		else if (n == QLatin1String("fit"))
		{
			g.pen(w);
			const double a = 0.20;
			const double b = 0.80;
			const double c = 0.16;
			painter.drawPolyline(QPolygonF() << g.p(a, a + c) << g.p(a, a) << g.p(a + c, a));
			painter.drawPolyline(QPolygonF() << g.p(b - c, a) << g.p(b, a) << g.p(b, a + c));
			painter.drawPolyline(QPolygonF() << g.p(b, b - c) << g.p(b, b) << g.p(b - c, b));
			painter.drawPolyline(QPolygonF() << g.p(a + c, b) << g.p(a, b) << g.p(a, b - c));
		}
		else if (n == QLatin1String("follow"))
		{
			g.pen(w);
			painter.drawEllipse(g.r(0.26, 0.26, 0.74, 0.74));
			painter.drawLine(g.p(0.5, 0.12), g.p(0.5, 0.30));
			painter.drawLine(g.p(0.5, 0.70), g.p(0.5, 0.88));
			painter.drawLine(g.p(0.12, 0.5), g.p(0.30, 0.5));
			painter.drawLine(g.p(0.70, 0.5), g.p(0.88, 0.5));
			g.fill();
			painter.drawEllipse(g.p(0.5, 0.5), 0.06 * size, 0.06 * size);
		}
		else if (n == QLatin1String("json"))
		{
			QFont f(QStringLiteral("Consolas"));
			f.setBold(true);
			f.setPixelSize(static_cast<int>(size * 0.42));
			painter.setFont(f);
			painter.setPen(ink);
			painter.drawText(QRectF(0, 0, size, size), Qt::AlignCenter, QStringLiteral("{ }"));
		}
		else if (n == QLatin1String("screenshot"))
		{
			g.fill();
			painter.drawRoundedRect(g.r(0.16, 0.30, 0.84, 0.78), 0.06 * size, 0.06 * size);
			painter.drawRect(g.r(0.38, 0.22, 0.62, 0.32));
			g.fill(Qt::white);
			painter.drawEllipse(g.p(0.5, 0.54), 0.15 * size, 0.15 * size);
			g.fill();
			painter.drawEllipse(g.p(0.5, 0.54), 0.08 * size, 0.08 * size);
		}
		else if (n == QLatin1String("osm") || n == QLatin1String("map"))
		{
			// folded map
			QPainterPath m;
			m.moveTo(g.p(0.14, 0.28));
			m.lineTo(g.p(0.38, 0.20));
			m.lineTo(g.p(0.62, 0.28));
			m.lineTo(g.p(0.86, 0.20));
			m.lineTo(g.p(0.86, 0.76));
			m.lineTo(g.p(0.62, 0.84));
			m.lineTo(g.p(0.38, 0.76));
			m.lineTo(g.p(0.14, 0.84));
			m.closeSubpath();
			g.fill(QColor(255, 255, 255, 230));
			painter.drawPath(m);
			g.pen(w * 0.6);
			painter.drawPath(m);
			painter.drawLine(g.p(0.38, 0.20), g.p(0.38, 0.76));
			painter.drawLine(g.p(0.62, 0.28), g.p(0.62, 0.84));
			if (n == QLatin1String("osm"))
			{
				// location pin
				QPainterPath pin;
				pin.addEllipse(g.p(0.5, 0.42), 0.15 * size, 0.15 * size);
				QPainterPath tip;
				tip.moveTo(g.p(0.37, 0.48));
				tip.lineTo(g.p(0.63, 0.48));
				tip.lineTo(g.p(0.5, 0.74));
				tip.closeSubpath();
				g.fill(QColor(226, 88, 88));
				painter.drawPath(pin.united(tip));
				g.fill(Qt::white);
				painter.drawEllipse(g.p(0.5, 0.42), 0.06 * size, 0.06 * size);
			}
			else
			{
				g.pen(w * 0.8, QColor(120, 170, 225));
				QPolygonF road;
				road << g.p(0.20, 0.66) << g.p(0.40, 0.52) << g.p(0.60, 0.60) << g.p(0.80, 0.40);
				painter.drawPolyline(road);
			}
		}
		else if (n == QLatin1String("congestion"))
		{
			// crossroad with a hotspot and a queue
			g.pen(w);
			painter.drawLine(g.p(0.5, 0.14), g.p(0.5, 0.86));
			painter.drawLine(g.p(0.14, 0.5), g.p(0.86, 0.5));
			g.fill(QColor(214, 70, 70));
			painter.drawEllipse(g.p(0.5, 0.5), 0.17 * size, 0.17 * size);
			g.fill(QColor(242, 145, 80));
			for (double y : { 0.66, 0.78 })
			{
				painter.drawRoundedRect(g.r(0.42, y - 0.04, 0.58, y + 0.04), 0.02 * size, 0.02 * size);
			}
		}
		else if (n == QLatin1String("language"))
		{
			// globe
			g.pen(w * 0.7);
			painter.drawEllipse(g.r(0.18, 0.18, 0.82, 0.82));
			painter.drawEllipse(g.r(0.36, 0.18, 0.64, 0.82));
			painter.drawLine(g.p(0.18, 0.5), g.p(0.82, 0.5));
			painter.drawLine(g.p(0.5, 0.18), g.p(0.5, 0.82));
			QPainterPath top;
			top.moveTo(g.p(0.25, 0.33));
			top.quadTo(g.p(0.5, 0.40), g.p(0.75, 0.33));
			painter.drawPath(top);
			QPainterPath bottom;
			bottom.moveTo(g.p(0.25, 0.67));
			bottom.quadTo(g.p(0.5, 0.60), g.p(0.75, 0.67));
			painter.drawPath(bottom);
		}
		else if (n == QLatin1String("api"))
		{
			// two arrows (data in and out)
			g.pen(w);
			painter.drawLine(g.p(0.20, 0.38), g.p(0.78, 0.38));
			painter.drawLine(g.p(0.62, 0.24), g.p(0.78, 0.38));
			painter.drawLine(g.p(0.80, 0.62), g.p(0.22, 0.62));
			painter.drawLine(g.p(0.38, 0.76), g.p(0.22, 0.62));
		}
		else if (n == QLatin1String("density"))
		{
			// two lanes full of cars
			g.pen(w * 0.5);
			painter.drawLine(g.p(0.12, 0.5), g.p(0.88, 0.5));
			const QColor cars[] = { ink, QColor(240, 120, 130), QColor(120, 170, 225) };
			int k = 0;
			for (double x : { 0.12, 0.40, 0.68 })
			{
				g.fill(cars[k++ % 3]);
				painter.drawRoundedRect(g.r(x, 0.24, x + 0.21, 0.42), 0.05 * size, 0.05 * size);
			}
			for (double x : { 0.26, 0.54 })
			{
				g.fill(cars[k++ % 3]);
				painter.drawRoundedRect(g.r(x, 0.58, x + 0.21, 0.76), 0.05 * size, 0.05 * size);
			}
		}
		else if (n == QLatin1String("properties"))
		{
			g.pen(w);
			for (int i = 0; i < 3; ++i)
			{
				const double y = 0.30 + i * 0.20;
				painter.drawLine(g.p(0.22, y), g.p(0.78, y));
			}
			g.fill();
			painter.drawEllipse(g.p(0.36, 0.30), 0.06 * size, 0.06 * size);
			painter.drawEllipse(g.p(0.62, 0.50), 0.06 * size, 0.06 * size);
			painter.drawEllipse(g.p(0.44, 0.70), 0.06 * size, 0.06 * size);
		}
	}
}

namespace icon_factory
{
	QPixmap pixmap(const QString &name, int size)
	{
		QPixmap pm(size, size);
		pm.fill(Qt::transparent);
		QPainter p(&pm);
		p.setRenderHint(QPainter::Antialiasing, true);
		p.setRenderHint(QPainter::TextAntialiasing, true);
		const QColor base = tile_color(name);
		const QRectF tile(size * 0.04, size * 0.04, size * 0.92, size * 0.92);
		QLinearGradient grad(tile.topLeft(), tile.bottomLeft());
		grad.setColorAt(0.0, base.lighter(108));
		grad.setColorAt(1.0, base.darker(104));
		p.setPen(QPen(base.darker(118), size * 0.02));
		p.setBrush(grad);
		p.drawRoundedRect(tile, size * 0.22, size * 0.22);
		p.translate(size * 0.04, size * 0.04);
		p.scale(0.92, 0.92);
		draw_glyph(p, name, size);
		return pm;
	}

	QIcon icon(const QString &name)
	{
		static QHash<QString, QIcon> cache;
		const auto it = cache.constFind(name);
		if (it != cache.constEnd())
		{
			return it.value();
		}
		QIcon ic;
		for (int s : { 16, 24, 32, 48, 64, 96 })
		{
			ic.addPixmap(pixmap(name, s));
		}
		cache.insert(name, ic);
		return ic;
	}
}
