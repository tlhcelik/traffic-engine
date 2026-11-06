#ifndef CHART_WIDGET_H
#define CHART_WIDGET_H

#include <QColor>
#include <QImage>
#include <QPointF>
#include <QString>
#include <QWidget>

#include <vector>

// chart_widget
// ------------
// A line chart for one time series. The x axis is the clock time (seconds
// after midnight; more than one day is possible), the y axis is the value.
// A mouse move shows the value at the cursor. render_to() draws the chart to
// any QPainter (screen, PNG image, SVG file).

class chart_widget : public QWidget
{
	Q_OBJECT

public:
	explicit chart_widget(QWidget *parent = nullptr);

	void set_title(const QString &title) { m_title = title; update(); }
	void set_unit(const QString &unit) { m_unit = unit; update(); }
	void set_color(const QColor &c) { m_color = c; update(); }
	void set_data(const std::vector<QPointF> &points);
	QString title() const { return m_title; }

	void render_to(QPainter &p, const QRectF &r) const;
	QSize sizeHint() const override { return QSize(480, 220); }
	QSize minimumSizeHint() const override { return QSize(240, 160); }

protected:
	void paintEvent(QPaintEvent *e) override;
	void mouseMoveEvent(QMouseEvent *e) override;
	void leaveEvent(QEvent *e) override;

private:
	QString m_title;
	QString m_unit;
	QColor m_color;
	std::vector<QPointF> m_pts;
	double m_hover_x;
};

#endif // CHART_WIDGET_H
