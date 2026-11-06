#include "traffic_density_widget.h"
#include "config_common.h"
#include "i18n.h"
#include "logger.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSlider>
#include <QSpinBox>

namespace
{
	const int max_percent = 300;
	const char *const keys[2] = { "simulation/traffic_density", "simulation/pedestrian_density" };
	const char *const names[2] = { "vehicles", "pedestrians" };
}

traffic_density_widget::traffic_density_widget(QWidget *parent)
	: QWidget(parent)
{
	QGridLayout *g = new QGridLayout(this);
	g->setContentsMargins(14, 10, 14, 10);
	g->setHorizontalSpacing(10);
	g->setVerticalSpacing(8);

	QLabel *title = new QLabel(QStringLiteral("<b>Traffic density</b> (all workspaces)"), this);
	g->addWidget(title, 0, 0, 1, 3);

	const QString labels[2] = { QStringLiteral("Vehicles"), QStringLiteral("Pedestrians") };
	for (int i = 0; i < 2; ++i)
	{
		row &r = m_rows[i];
		r.key = QString::fromLatin1(keys[i]);
		r.slider = new QSlider(Qt::Horizontal, this);
		r.slider->setRange(0, max_percent);
		r.slider->setSingleStep(5);
		r.slider->setPageStep(25);
		r.slider->setTickPosition(QSlider::TicksBelow);
		r.slider->setTickInterval(50);
		r.slider->setTracking(false); // the value applies when the user releases the slider
		r.slider->setMinimumWidth(220);
		r.spin = new QSpinBox(this);
		r.spin->setRange(0, max_percent);
		r.spin->setSingleStep(5);
		r.spin->setSuffix(QStringLiteral(" %"));
		r.spin->setKeyboardTracking(false);
		g->addWidget(new QLabel(labels[i], this), i + 1, 0);
		g->addWidget(r.slider, i + 1, 1);
		g->addWidget(r.spin, i + 1, 2);

		connect(r.slider, &QSlider::sliderMoved, this, [this, i](int v)
		{
			const QSignalBlocker block(m_rows[i].spin);
			m_rows[i].spin->setValue(v);
		});
		connect(r.slider, &QSlider::valueChanged, this, [this, i](int v) { set_percent(i, v); });
		connect(r.spin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, i](int v) { set_percent(i, v); });
	}

	QHBoxLayout *quick = new QHBoxLayout();
	quick->setSpacing(6);
	for (int p : { 25, 50, 100, 150, 200 })
	{
		QPushButton *b = new QPushButton(QStringLiteral("%1%").arg(p), this);
		b->setToolTip(QStringLiteral("Set the vehicles and the pedestrians to this percent"));
		b->setAutoDefault(false);
		b->setFocusPolicy(Qt::NoFocus);
		connect(b, &QPushButton::clicked, this, [this, p]()
		{
			set_percent(0, p);
			set_percent(1, p);
		});
		quick->addWidget(b);
	}
	g->addLayout(quick, 3, 0, 1, 3);

	QLabel *note = new QLabel(QStringLiteral("100% = the rates of the traffic sources (node properties).\n"
		"The change applies immediately to all open workspaces."), this);
	note->setStyleSheet(QStringLiteral("color: #6B7584;"));
	g->addWidget(note, 4, 0, 1, 3);

	reload();
}

QString traffic_density_widget::button_text()
{
	return tx("Traffic %1%").arg(config_common::instance().int_value(QString::fromLatin1(keys[0])));
}

QString traffic_density_widget::button_tip()
{
	const config_common &cfg = config_common::instance();
	return tx("Traffic density of all workspaces: vehicles %1%, pedestrians %2% of the rates of the traffic sources")
		.arg(cfg.int_value(QString::fromLatin1(keys[0]))).arg(cfg.int_value(QString::fromLatin1(keys[1])));
}

void traffic_density_widget::reload()
{
	const config_common &cfg = config_common::instance();
	for (row &r : m_rows)
	{
		const int v = qBound(0, cfg.int_value(r.key), max_percent);
		const QSignalBlocker b1(r.slider);
		const QSignalBlocker b2(r.spin);
		r.slider->setValue(v);
		r.spin->setValue(v);
	}
}

void traffic_density_widget::set_percent(int index, int percent)
{
	row &r = m_rows[index];
	percent = qBound(0, percent, max_percent);
	{
		const QSignalBlocker b1(r.slider);
		const QSignalBlocker b2(r.spin);
		r.slider->setValue(percent);
		r.spin->setValue(percent);
	}
	config_common &cfg = config_common::instance();
	if (cfg.int_value(r.key) == percent)
	{
		return;
	}
	cfg.set_int(r.key, percent);
	cfg.save();
	logger::instance().action(QStringLiteral("Traffic density of the %1: %2%").arg(QString::fromLatin1(names[index])).arg(percent));
}
