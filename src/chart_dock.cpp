#include "chart_dock.h"
#include "app_theme.h"
#include "chart_widget.h"
#include "config_common.h"
#include "i18n.h"
#include "logger.h"
#include "stats_collector.h"
#include "workspace.h"

#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSvgGenerator>
#include <QVBoxLayout>

chart_dock::chart_dock(QWidget *parent)
	: QWidget(parent)
	, m_ws(nullptr)
	, m_lights_revision(0)
{
	QHBoxLayout *root = new QHBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);

	QVBoxLayout *left = new QVBoxLayout();
	QLabel *l1 = new QLabel(QStringLiteral("Charts (select the metrics):"), this);
	m_metrics = new QListWidget(this);
	for (int i = 0; i < stats_collector::metric_count; ++i)
	{
		QListWidgetItem *it = new QListWidgetItem(QStringLiteral("%1 (%2)").arg(tx(stats_collector::metric_name(i)), tx(stats_collector::metric_unit(i))), m_metrics);
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		const bool on = (i == stats_collector::m_vehicles || i == stats_collector::m_mean_speed
			|| i == stats_collector::m_junction_wait || i == stats_collector::m_fuel_per_100km);
		it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
		it->setData(Qt::UserRole, i);
	}
	m_window = new QComboBox(this);
	m_window->addItem(QStringLiteral("Last 10 minutes"), 600);
	m_window->addItem(QStringLiteral("Last hour"), 3600);
	m_window->addItem(QStringLiteral("Last 6 hours"), 6 * 3600);
	m_window->addItem(QStringLiteral("All data"), 0);
	m_window->setCurrentIndex(1);
	m_light = new QComboBox(this);
	i18n::set_dynamic(m_light); // items from the code (tx)
	QPushButton *png = new QPushButton(QStringLiteral("Export PNG..."), this);
	QPushButton *svg = new QPushButton(QStringLiteral("Export SVG..."), this);
	QPushButton *csv = new QPushButton(QStringLiteral("Export CSV..."), this);
	left->addWidget(l1);
	left->addWidget(m_metrics, 1);
	left->addWidget(new QLabel(QStringLiteral("Time window:"), this));
	left->addWidget(m_window);
	left->addWidget(new QLabel(QStringLiteral("Traffic light (waiting vehicles):"), this));
	left->addWidget(m_light);
	left->addWidget(png);
	left->addWidget(svg);
	left->addWidget(csv);
	QWidget *left_host = new QWidget(this);
	left_host->setLayout(left);
	left_host->setFixedWidth(280);
	root->addWidget(left_host);

	QScrollArea *scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	m_host = new QWidget(scroll);
	m_charts_layout = new QVBoxLayout(m_host);
	m_charts_layout->setContentsMargins(0, 0, 0, 0);
	m_charts_layout->setSpacing(8);
	scroll->setWidget(m_host);
	root->addWidget(scroll, 1);

	connect(m_metrics, &QListWidget::itemChanged, this, [this](QListWidgetItem *) { rebuild_charts(); });
	connect(m_window, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { refresh(); });
	connect(m_light, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { rebuild_charts(); });
	connect(png, &QPushButton::clicked, this, [this]() { export_image(false); });
	connect(svg, &QPushButton::clicked, this, [this]() { export_image(true); });
	connect(csv, &QPushButton::clicked, this, &chart_dock::export_csv);
	fill_lights();
	rebuild_charts();
}

void chart_dock::set_workspace(workspace *ws)
{
	m_ws = ws;
	fill_lights();
	rebuild_charts();
}

void chart_dock::fill_lights()
{
	const int current = m_light->currentData().toInt();
	m_light->blockSignals(true);
	m_light->clear();
	m_light->addItem(tx("(no light chart)"), -1);
	if (m_ws)
	{
		for (const auto &kv : m_ws->model().lights)
		{
			m_light->addItem(tx("Traffic light %1 (node %2)").arg(kv.first).arg(kv.second.node_id), kv.first);
		}
		m_lights_revision = m_ws->model().revision;
	}
	const int idx = m_light->findData(current);
	m_light->setCurrentIndex(idx >= 0 ? idx : 0);
	m_light->blockSignals(false);
}

void chart_dock::rebuild_charts()
{
	for (chart_widget *c : m_charts)
	{
		c->deleteLater();
	}
	m_charts.clear();
	m_chart_metric.clear();
	QLayoutItem *item = nullptr;
	while ((item = m_charts_layout->takeAt(0)) != nullptr)
	{
		delete item;
	}
	int color = 0;
	for (int i = 0; i < m_metrics->count(); ++i)
	{
		QListWidgetItem *it = m_metrics->item(i);
		if (it->checkState() != Qt::Checked)
		{
			continue;
		}
		const int metric = it->data(Qt::UserRole).toInt();
		chart_widget *c = new chart_widget(m_host);
		c->set_title(tx(stats_collector::metric_name(metric)));
		c->set_unit(tx(stats_collector::metric_unit(metric)));
		c->set_color(app_theme::pastel(color++).darker(112));
		m_charts.push_back(c);
		m_chart_metric.push_back(metric);
		m_charts_layout->addWidget(c);
	}
	const int light = m_light->currentData().toInt();
	if (light >= 0)
	{
		chart_widget *c = new chart_widget(m_host);
		c->set_title(tx("Waiting vehicles at traffic light %1").arg(light));
		c->set_unit(tx("veh"));
		c->set_color(QColor(240, 150, 140));
		m_charts.push_back(c);
		m_chart_metric.push_back(-1);
		m_charts_layout->addWidget(c);
	}
	m_charts_layout->addStretch(1);
	refresh();
}

void chart_dock::refresh()
{
	if (!m_ws)
	{
		return;
	}
	if (m_ws->model().revision != m_lights_revision && m_ws->model().lights.size() + 1 != static_cast<size_t>(m_light->count()))
	{
		fill_lights();
	}
	const stats_collector &st = m_ws->sim().stats();
	const std::vector<stats_sample> &samples = st.samples();
	const double window = m_window->currentData().toDouble();
	const double start_clock = m_ws->model().start_clock;
	const double t_end = samples.empty() ? 0.0 : samples.back().time;
	for (size_t k = 0; k < m_charts.size(); ++k)
	{
		std::vector<QPointF> pts;
		const int metric = m_chart_metric[k];
		if (metric >= 0)
		{
			pts.reserve(samples.size());
			for (const stats_sample &s : samples)
			{
				if (window > 0.0 && s.time < t_end - window)
				{
					continue;
				}
				pts.push_back(QPointF(start_clock + s.time, s.values[metric]));
			}
		}
		else
		{
			const int light = m_light->currentData().toInt();
			const auto it = st.light_samples().find(light);
			if (it != st.light_samples().end())
			{
				for (const auto &s : it->second)
				{
					if (window > 0.0 && s.first < t_end - window)
					{
						continue;
					}
					pts.push_back(QPointF(start_clock + s.first, s.second));
				}
			}
		}
		m_charts[k]->set_data(pts);
	}
}

void chart_dock::export_image(bool svg)
{
	if (m_charts.empty())
	{
		QMessageBox::information(this, tx("Export"), tx("Select one or more metrics first."));
		return;
	}
	config_common &cfg = config_common::instance();
	const QString filter = svg ? tx("SVG image (*.svg)") : tx("PNG image (*.png)");
	const QString path = QFileDialog::getSaveFileName(this, tx("Export the charts"),
		cfg.value(QStringLiteral("general/last_dir")) + (svg ? QStringLiteral("/charts.svg") : QStringLiteral("/charts.png")), filter);
	if (path.isEmpty())
	{
		return;
	}
	cfg.set_value(QStringLiteral("general/last_dir"), QFileInfo(path).absolutePath());
	const int w = 1200;
	const int h = 300;
	const int total = h * static_cast<int>(m_charts.size()) + 10 * static_cast<int>(m_charts.size() - 1);
	bool ok = false;
	if (svg)
	{
		QSvgGenerator gen;
		gen.setFileName(path);
		gen.setSize(QSize(w, total));
		gen.setViewBox(QRect(0, 0, w, total));
		gen.setTitle(QStringLiteral("Traffic Engine charts"));
		QPainter p(&gen);
		for (size_t k = 0; k < m_charts.size(); ++k)
		{
			m_charts[k]->render_to(p, QRectF(0, k * (h + 10), w, h));
		}
		ok = p.end();
	}
	else
	{
		QImage img(w * 2, total * 2, QImage::Format_ARGB32_Premultiplied);
		img.fill(QColor(244, 246, 250));
		QPainter p(&img);
		p.scale(2.0, 2.0);
		for (size_t k = 0; k < m_charts.size(); ++k)
		{
			m_charts[k]->render_to(p, QRectF(0, k * (h + 10), w, h));
		}
		p.end();
		ok = img.save(path);
	}
	if (ok)
	{
		logger::instance().file_event(QStringLiteral("Charts exported"), path);
	}
	else
	{
		QMessageBox::warning(this, tx("Export"), tx("Cannot write the file:\n%1").arg(path));
	}
}

void chart_dock::export_csv()
{
	if (!m_ws)
	{
		return;
	}
	config_common &cfg = config_common::instance();
	const QString path = QFileDialog::getSaveFileName(this, tx("Export the chart data"),
		cfg.value(QStringLiteral("general/last_dir")) + QStringLiteral("/statistics.csv"), tx("CSV file (*.csv)"));
	if (path.isEmpty())
	{
		return;
	}
	cfg.set_value(QStringLiteral("general/last_dir"), QFileInfo(path).absolutePath());
	QString csv = m_ws->sim().stats().to_csv();
	for (const auto &kv : m_ws->sim().stats().light_samples())
	{
		csv += QStringLiteral("\ntraffic_light_%1;time_s;waiting_vehicles\n").arg(kv.first);
		for (const auto &s : kv.second)
		{
			csv += QStringLiteral(";%1;%2\n").arg(s.first, 0, 'f', 1).arg(s.second, 0, 'f', 0);
		}
	}
	QSaveFile f(path);
	if (f.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		f.write(csv.toUtf8());
		if (f.commit())
		{
			logger::instance().file_event(QStringLiteral("Statistics exported"), path);
			return;
		}
	}
	QMessageBox::warning(this, tx("Export"), tx("Cannot write the file:\n%1").arg(path));
}

void chart_dock::retranslate()
{
	for (int i = 0; i < m_metrics->count(); ++i)
	{
		QListWidgetItem *it = m_metrics->item(i);
		const int metric = it->data(Qt::UserRole).toInt();
		m_metrics->blockSignals(true);
		it->setText(QStringLiteral("%1 (%2)").arg(tx(stats_collector::metric_name(metric)), tx(stats_collector::metric_unit(metric))));
		m_metrics->blockSignals(false);
	}
	fill_lights();
	rebuild_charts();
}
