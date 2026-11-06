#include "chart_widget.h"
#include "i18n.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTime>

#include <algorithm>
#include <cmath>

namespace
{
	const double margin_left = 58.0;
	const double margin_right = 16.0;
	const double margin_top = 30.0;
	const double margin_bottom = 26.0;

	double nice_step(double range, int target)
	{
		if (range <= 0.0)
		{
			return 1.0;
		}
		const double raw = range / std::max(1, target);
		const double mag = std::pow(10.0, std::floor(std::log10(raw)));
		const double norm = raw / mag;
		double step = 10.0;
		if (norm < 1.5) { step = 1.0; }
		else if (norm < 3.0) { step = 2.0; }
		else if (norm < 7.0) { step = 5.0; }
		return step * mag;
	}

	QString clock_text(double seconds, bool with_seconds)
	{
		const int s = static_cast<int>(std::floor(seconds));
		const int day = s / 86400;
		const QString t = QTime(0, 0).addSecs(s % 86400).toString(with_seconds ? QStringLiteral("HH:mm:ss") : QStringLiteral("HH:mm"));
		return day > 0 ? QStringLiteral("+%1d %2").arg(day).arg(t) : t;
	}

	QString value_text(double v)
	{
		const double a = std::abs(v);
		if (a >= 100.0) { return QString::number(v, 'f', 0); }
		if (a >= 10.0) { return QString::number(v, 'f', 1); }
		return QString::number(v, 'f', 2);
	}
}

chart_widget::chart_widget(QWidget *parent)
	: QWidget(parent)
	, m_color(120, 170, 225)
	, m_hover_x(-1.0)
{
	setMouseTracking(true);
	setMinimumHeight(170);
}

void chart_widget::set_data(const std::vector<QPointF> &points)
{
	m_pts = points;
	update();
}

void chart_widget::render_to(QPainter &p, const QRectF &r) const
{
	p.save();
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setPen(QPen(QColor(221, 227, 236), 1.0));
	p.setBrush(Qt::white);
	p.drawRoundedRect(r.adjusted(0.5, 0.5, -0.5, -0.5), 10, 10);

	QFont tf = p.font();
	tf.setBold(true);
	tf.setPointSizeF(9.5);
	p.setFont(tf);
	p.setPen(QColor(61, 74, 92));
	const QString head = m_unit.isEmpty() ? m_title : QStringLiteral("%1 (%2)").arg(m_title, m_unit);
	p.drawText(QRectF(r.left() + 12, r.top() + 4, r.width() - 24, margin_top - 6), Qt::AlignLeft | Qt::AlignVCenter, head);

	const QRectF plot(r.left() + margin_left, r.top() + margin_top, r.width() - margin_left - margin_right, r.height() - margin_top - margin_bottom);
	if (plot.width() < 20 || plot.height() < 20)
	{
		p.restore();
		return;
	}
	QFont lf = p.font();
	lf.setBold(false);
	lf.setPointSizeF(8.0);
	p.setFont(lf);

	if (m_pts.size() < 2)
	{
		p.setPen(QColor(150, 158, 170));
		p.drawText(plot, Qt::AlignCenter, tx("No data yet. Start the flow (Space)."));
		p.restore();
		return;
	}

	double x0 = m_pts.front().x();
	double x1 = m_pts.back().x();
	if (x1 - x0 < 1.0)
	{
		x1 = x0 + 1.0;
	}
	double y0 = 0.0;
	double y1 = 0.0;
	for (const QPointF &q : m_pts)
	{
		y0 = std::min(y0, q.y());
		y1 = std::max(y1, q.y());
	}
	if (y1 - y0 < 1e-9)
	{
		y1 = y0 + 1.0;
	}
	const double ystep = nice_step(y1 - y0, 4);
	y1 = std::ceil(y1 * 1.05 / ystep) * ystep;
	y0 = std::floor(y0 / ystep) * ystep;
	auto sx = [&](double x) { return plot.left() + (x - x0) / (x1 - x0) * plot.width(); };
	auto sy = [&](double y) { return plot.bottom() - (y - y0) / (y1 - y0) * plot.height(); };

	// grid and axis labels
	p.setPen(QPen(QColor(236, 240, 245), 1.0));
	for (double y = y0; y <= y1 + ystep * 0.01; y += ystep)
	{
		p.setPen(QPen(QColor(236, 240, 245), 1.0));
		p.drawLine(QPointF(plot.left(), sy(y)), QPointF(plot.right(), sy(y)));
		p.setPen(QColor(120, 130, 145));
		p.drawText(QRectF(r.left() + 2, sy(y) - 8, margin_left - 8, 16), Qt::AlignRight | Qt::AlignVCenter, value_text(y));
	}
	static const double steps[] = { 10, 30, 60, 120, 300, 600, 900, 1800, 3600, 7200, 10800, 21600, 43200, 86400 };
	double xstep = steps[0];
	for (double s : steps)
	{
		xstep = s;
		if ((x1 - x0) / s <= 6.0)
		{
			break;
		}
	}
	const bool secs = xstep < 60.0;
	for (double x = std::ceil(x0 / xstep) * xstep; x <= x1; x += xstep)
	{
		p.setPen(QPen(QColor(236, 240, 245), 1.0));
		p.drawLine(QPointF(sx(x), plot.top()), QPointF(sx(x), plot.bottom()));
		p.setPen(QColor(120, 130, 145));
		p.drawText(QRectF(sx(x) - 40, plot.bottom() + 4, 80, 16), Qt::AlignCenter, clock_text(x, secs));
	}
	p.setPen(QPen(QColor(200, 208, 220), 1.0));
	p.drawLine(plot.bottomLeft(), plot.bottomRight());
	p.drawLine(plot.bottomLeft(), plot.topLeft());

	// the line (decimated to about two points per pixel)
	const std::size_t n = m_pts.size();
	const std::size_t stride = std::max<std::size_t>(1, n / static_cast<std::size_t>(std::max(1.0, plot.width() * 2.0)));
	QPainterPath line;
	bool first = true;
	for (std::size_t i = 0; i < n; i += stride)
	{
		const QPointF q(sx(m_pts[i].x()), sy(m_pts[i].y()));
		if (first) { line.moveTo(q); first = false; } else { line.lineTo(q); }
	}
	line.lineTo(sx(m_pts.back().x()), sy(m_pts.back().y()));
	QPainterPath area = line;
	area.lineTo(sx(m_pts.back().x()), plot.bottom());
	area.lineTo(sx(m_pts.front().x()), plot.bottom());
	area.closeSubpath();
	QColor fill = m_color;
	fill.setAlpha(60);
	p.setPen(Qt::NoPen);
	p.setBrush(fill);
	p.drawPath(area);
	p.setPen(QPen(m_color.darker(115), 2.0));
	p.setBrush(Qt::NoBrush);
	p.drawPath(line);

	// value at the cursor
	if (m_hover_x >= plot.left() && m_hover_x <= plot.right())
	{
		const double xv = x0 + (m_hover_x - plot.left()) / plot.width() * (x1 - x0);
		auto it = std::lower_bound(m_pts.begin(), m_pts.end(), xv, [](const QPointF &a, double b) { return a.x() < b; });
		if (it == m_pts.end())
		{
			--it;
		}
		const QPointF q(sx(it->x()), sy(it->y()));
		p.setPen(QPen(QColor(150, 160, 175), 1.0, Qt::DashLine));
		p.drawLine(QPointF(q.x(), plot.top()), QPointF(q.x(), plot.bottom()));
		p.setPen(Qt::NoPen);
		p.setBrush(m_color.darker(120));
		p.drawEllipse(q, 4, 4);
		const QString label = QStringLiteral("%1   %2 %3").arg(clock_text(it->x(), true), value_text(it->y()), m_unit);
		const QFontMetrics fm(lf);
		const double w = fm.horizontalAdvance(label) + 14;
		QRectF box(q.x() + 8, plot.top() + 2, w, 20);
		if (box.right() > plot.right())
		{
			box.moveRight(q.x() - 8);
		}
		p.setPen(QPen(QColor(221, 227, 236), 1.0));
		p.setBrush(QColor(255, 253, 231, 240));
		p.drawRoundedRect(box, 6, 6);
		p.setPen(QColor(61, 74, 92));
		p.drawText(box, Qt::AlignCenter, label);
	}
	p.restore();
}

void chart_widget::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	render_to(p, QRectF(rect()));
}

void chart_widget::mouseMoveEvent(QMouseEvent *e)
{
	m_hover_x = e->pos().x();
	update();
}

void chart_widget::leaveEvent(QEvent *)
{
	m_hover_x = -1.0;
	update();
}
