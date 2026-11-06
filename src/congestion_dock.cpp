#include "congestion_dock.h"
#include "app_theme.h"
#include "config_common.h"
#include "i18n.h"
#include "logger.h"
#include "workspace.h"
#include "world_properties.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollBar>
#include <QSet>
#include <QTextStream>
#include <QTreeWidget>
#include <QVBoxLayout>

namespace
{
	const int role_node = Qt::UserRole + 1;
	const int role_road = Qt::UserRole + 2;

	QString road_name(const world_model &m, int road_id)
	{
		const road_data *r = m.road(road_id);
		return (r && !r->name.isEmpty()) ? r->name : tx("Road %1").arg(road_id);
	}

	void set_los_cell(QTreeWidgetItem *it, char los)
	{
		const QColor c = congestion_dock::los_color(los);
		it->setText(1, QString(QLatin1Char(los)));
		it->setBackground(1, c);
		it->setForeground(1, los >= 'E' ? QColor(255, 255, 255) : QColor(30, 34, 40));
		it->setTextAlignment(1, Qt::AlignCenter);
	}
}

QColor congestion_dock::los_color(char los)
{
	return app_theme::los_color(los);
}

QString congestion_dock::cause_text(wait_cause cause)
{
	switch (cause)
	{
		case wait_cause::red_light: return tx("Red light (signal times)");
		case wait_cause::give_way: return tx("Gives way (priority)");
		case wait_cause::conflict: return tx("Conflicting movements");
		case wait_cause::exit_full: return tx("Exit full (queue after the junction)");
		case wait_cause::junction_blocked: return tx("Junction blocked");
		case wait_cause::pedestrians: return tx("Pedestrians");
		case wait_cause::stop_sign: return tx("STOP sign");
		case wait_cause::lane_blocked: return tx("Closed lane");
		case wait_cause::bus_stop: return tx("Bus stop");
		case wait_cause::level_crossing: return tx("Level crossing");
		case wait_cause::queue: return tx("Slow traffic");
		case wait_cause::merge_courtesy: return tx("Vehicles that merge");
		case wait_cause::no_route: return tx("Lane changes");
		default: return QStringLiteral("-");
	}
}

QString congestion_dock::junction_name(const world_model &m, int node_id)
{
	QStringList names;
	for (int rid : m.roads_at_node(node_id))
	{
		const road_data *r = m.road(rid);
		if (r && !r->name.isEmpty() && !names.contains(r->name))
		{
			names << r->name;
		}
	}
	while (names.size() > 2)
	{
		names.removeLast();
	}
	const QString title = world_properties::title(m, element_ref(element_kind::node, node_id));
	return names.isEmpty() ? title : QStringLiteral("%1 - %2").arg(title, names.join(QStringLiteral(" / ")));
}

congestion_dock::congestion_dock(QWidget *parent)
	: QWidget(parent)
	, m_ws(nullptr)
{
	QVBoxLayout *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	m_summary = new QLabel(this);
	m_summary->setWordWrap(true);
	i18n::set_dynamic(m_summary);
	root->addWidget(m_summary);

	QHBoxLayout *row = new QHBoxLayout();
	row->addWidget(new QLabel(QStringLiteral("Time window:"), this));
	m_window = new QComboBox(this);
	m_window->addItem(QStringLiteral("Last minute"), 60);
	m_window->addItem(QStringLiteral("Last 5 minutes"), 300);
	m_window->addItem(QStringLiteral("Last 15 minutes"), 900);
	m_window->setCurrentIndex(1);
	row->addWidget(m_window);
	m_problems = new QCheckBox(QStringLiteral("Only problems (LOS D to F)"), this);
	m_problems->setChecked(true);
	row->addWidget(m_problems);
	row->addStretch(1);
	QPushButton *exp = new QPushButton(QStringLiteral("Export CSV..."), this);
	row->addWidget(exp);
	root->addLayout(row);

	m_tree = new QTreeWidget(this);
	m_tree->setColumnCount(6);
	m_tree->setRootIsDecorated(true);
	m_tree->setUniformRowHeights(true);
	m_tree->setAlternatingRowColors(true);
	m_tree->header()->setStretchLastSection(true);
	m_tree->header()->setSectionResizeMode(0, QHeaderView::Interactive);
	m_tree->setColumnWidth(0, 300);
	m_tree->setColumnWidth(1, 44);
	m_tree->setColumnWidth(2, 80);
	m_tree->setColumnWidth(3, 150);
	m_tree->setColumnWidth(4, 90);
	i18n::set_dynamic(m_tree); // the code sets the texts (tx)
	root->addWidget(m_tree, 1);

	connect(m_window, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this]() { refresh(); });
	connect(m_problems, &QCheckBox::toggled, this, [this]() { refresh(); });
	connect(exp, &QPushButton::clicked, this, &congestion_dock::export_csv);
	connect(m_tree, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *it)
	{
		const int road = it->data(0, role_road).toInt();
		const int node = it->data(0, role_node).toInt();
		emit go_to(road >= 0 ? element_ref(element_kind::road, road) : element_ref(element_kind::node, node));
	});
	retranslate();
}

void congestion_dock::set_workspace(workspace *ws)
{
	m_ws = ws;
	refresh();
}

void congestion_dock::retranslate()
{
	m_tree->setHeaderLabels({ tx("Junction"), tx("LOS"), tx("Delay"), tx("Queue now (maximum)"), tx("Flow"), tx("Main cause") });
	m_tree->headerItem()->setToolTip(1, tx("Level of service A to F (Highway Capacity Manual): from the delay per vehicle"));
	m_tree->headerItem()->setToolTip(2, tx("Mean delay per vehicle at the junction (s)"));
	m_tree->headerItem()->setToolTip(5, tx("The largest part of the waiting time of the vehicles in the queues"));
	refresh();
}

void congestion_dock::refresh()
{
	if (!m_ws)
	{
		m_tree->clear();
		m_summary->setText(tx("No workspace"));
		return;
	}
	simulation &sim = m_ws->sim();
	const double window = m_window->currentData().toDouble();
	if (std::abs(sim.congestion_window() - window) > 0.5)
	{
		sim.set_congestion_window(window);
	}
	const world_model &m = m_ws->model();
	const std::vector<junction_status> &all = sim.congestion();

	// Keep the open junctions, the selection and the scroll position
	QSet<int> open;
	for (int i = 0; i < m_tree->topLevelItemCount(); ++i)
	{
		if (m_tree->topLevelItem(i)->isExpanded())
		{
			open.insert(m_tree->topLevelItem(i)->data(0, role_node).toInt());
		}
	}
	int sel_node = -2;
	int sel_road = -2;
	if (QTreeWidgetItem *cur = m_tree->currentItem())
	{
		sel_node = cur->data(0, role_node).toInt();
		sel_road = cur->data(0, role_road).toInt();
	}
	const int scroll = m_tree->verticalScrollBar()->value();
	m_tree->setUpdatesEnabled(false);
	m_tree->clear();

	int hotspots = 0;
	int spillbacks = 0;
	for (const junction_status &j : all)
	{
		hotspots += j.los >= 'E' ? 1 : 0;
		spillbacks += j.spillback ? 1 : 0;
		if (m_problems->isChecked() && j.los < 'D')
		{
			continue;
		}
		QTreeWidgetItem *it = new QTreeWidgetItem(m_tree);
		it->setData(0, role_node, j.node_id);
		it->setData(0, role_road, -1);
		it->setText(0, junction_name(m, j.node_id) + (j.signalized ? QStringLiteral("  ") + tx("(lights)") : QString()));
		set_los_cell(it, j.los);
		it->setText(2, tx("%1 s").arg(j.delay, 0, 'f', 0));
		double max_queue = 0.0;
		for (const approach_status &a : j.approaches)
		{
			max_queue = std::max(max_queue, a.max_queue_m);
		}
		it->setText(3, tx("%1 m (%2 m)").arg(j.queue_m, 0, 'f', 0).arg(max_queue, 0, 'f', 0)
			+ (j.spillback ? QStringLiteral("  ") + tx("spillback") : QString()));
		if (j.spillback)
		{
			it->setForeground(3, QColor(198, 40, 40));
		}
		it->setText(4, tx("%1 veh/h").arg(j.flow, 0, 'f', 0));
		QString cause = cause_text(j.cause);
		if (j.cause != wait_cause::none)
		{
			cause += QStringLiteral(" (%1%)").arg(j.cause_share * 100.0, 0, 'f', 0);
		}
		if (j.blocking_node >= 0)
		{
			cause += QStringLiteral(" ") + tx("from node %1").arg(j.blocking_node);
		}
		it->setText(5, cause);
		for (const approach_status &a : j.approaches)
		{
			QTreeWidgetItem *c = new QTreeWidgetItem(it);
			c->setData(0, role_node, j.node_id);
			c->setData(0, role_road, a.road_id);
			c->setText(0, road_name(m, a.road_id));
			set_los_cell(c, a.spillback ? 'F' : simulation::los_letter(a.delay, j.signalized));
			c->setText(2, tx("%1 s").arg(a.delay, 0, 'f', 0));
			c->setText(3, tx("%1 m (%2 m)").arg(a.queue_m, 0, 'f', 0).arg(a.max_queue_m, 0, 'f', 0)
				+ (a.spillback ? QStringLiteral("  ") + tx("spillback") : QString()));
			if (a.spillback)
			{
				c->setForeground(3, QColor(198, 40, 40));
			}
			c->setText(4, tx("%1 veh/h").arg(a.passages * 3600.0 / std::max(1.0, std::min(sim.congestion_window(), sim.time())), 0, 'f', 0));
			c->setText(5, cause_text(a.cause));
			if (j.node_id == sel_node && a.road_id == sel_road)
			{
				m_tree->setCurrentItem(c);
			}
		}
		it->setExpanded(open.contains(j.node_id));
		if (j.node_id == sel_node && sel_road == -1)
		{
			m_tree->setCurrentItem(it);
		}
	}
	m_tree->setUpdatesEnabled(true);
	m_tree->verticalScrollBar()->setValue(scroll);

	if (all.empty())
	{
		m_summary->setText(tx("No junction with three or more roads or a traffic light."));
	}
	else if (hotspots == 0 && spillbacks == 0)
	{
		m_summary->setText(tx("%1 junctions. No hotspot: all junctions have the level of service A to D.").arg(all.size()));
	}
	else
	{
		const junction_status &w = all.front();
		m_summary->setText(tx("%1 junctions. Hotspots (LOS E or F): %2, spillbacks: %3. The worst: %4 (LOS %5, %6 s per vehicle, %7).")
			.arg(all.size()).arg(hotspots).arg(spillbacks).arg(junction_name(m, w.node_id))
			.arg(QString(QLatin1Char(w.los))).arg(w.delay, 0, 'f', 0).arg(cause_text(w.cause)));
	}
}

void congestion_dock::export_csv()
{
	if (!m_ws)
	{
		return;
	}
	config_common &cfg = config_common::instance();
	const QString path = QFileDialog::getSaveFileName(this, tx("Export the congestion data"),
		cfg.value(QStringLiteral("general/last_dir")) + QStringLiteral("/congestion.csv"), tx("CSV file (*.csv)"));
	if (path.isEmpty())
	{
		return;
	}
	cfg.set_value(QStringLiteral("general/last_dir"), QFileInfo(path).absolutePath());
	const world_model &m = m_ws->model();
	const simulation &sim = m_ws->sim();
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly | QIODevice::Text))
	{
		return;
	}
	QTextStream ts(&f);
	ts.setCodec("UTF-8");
	ts << "type;node;road;name;signalized;los;delay_s;flow_veh_h;queue_m;max_queue_m;waiting;spillback;cause;cause_share;blocking_node\n";
	auto esc = [](QString s) { return s.replace(QLatin1Char(';'), QLatin1Char(',')); };
	for (const junction_status &j : sim.congestion())
	{
		double max_queue = 0.0;
		for (const approach_status &a : j.approaches)
		{
			max_queue = std::max(max_queue, a.max_queue_m);
		}
		ts << "junction;" << j.node_id << ";;" << esc(junction_name(m, j.node_id)) << ';' << (j.signalized ? 1 : 0) << ';'
			<< QLatin1Char(j.los) << ';' << QString::number(j.delay, 'f', 1) << ';' << QString::number(j.flow, 'f', 0) << ';'
			<< QString::number(j.queue_m, 'f', 1) << ';' << QString::number(max_queue, 'f', 1) << ';' << j.waiting << ';'
			<< (j.spillback ? 1 : 0) << ';' << wait_cause_name(j.cause) << ';' << QString::number(j.cause_share, 'f', 2) << ';'
			<< j.blocking_node << '\n';
		for (const approach_status &a : j.approaches)
		{
			ts << "approach;" << j.node_id << ';' << a.road_id << ';' << esc(road_name(m, a.road_id)) << ';' << (j.signalized ? 1 : 0) << ';'
				<< QLatin1Char(a.spillback ? 'F' : simulation::los_letter(a.delay, j.signalized)) << ';' << QString::number(a.delay, 'f', 1) << ';'
				<< QString::number(a.passages * 3600.0 / std::max(1.0, std::min(sim.congestion_window(), sim.time())), 'f', 0) << ';'
				<< QString::number(a.queue_m, 'f', 1) << ';' << QString::number(a.max_queue_m, 'f', 1) << ';' << a.queue_veh << ';'
				<< (a.spillback ? 1 : 0) << ';' << wait_cause_name(a.cause) << ";;\n";
		}
	}
	ts.flush();
	f.commit();
	TE_INFO("FILE", QStringLiteral("Congestion data exported: %1").arg(path));
}
