#include "traffic_light_dialog.h"
#include "i18n.h"
#include "logger.h"
#include "workspace.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPainter>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

#include <algorithm>

// One cycle of the program as a colored bar.
class light_timeline : public QWidget
{
public:
	explicit light_timeline(QWidget *parent = nullptr) : QWidget(parent) { setMinimumHeight(54); }
	light_data data;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		p.setRenderHint(QPainter::Antialiasing, true);
		double cycle = 0.0;
		for (const light_phase &ph : data.phases)
		{
			cycle += ph.green + ph.yellow + ph.all_red + data.red_yellow;
		}
		const QRectF r = QRectF(rect()).adjusted(4, 4, -4, -20);
		p.setPen(QPen(QColor(221, 227, 236), 1.0));
		p.setBrush(Qt::white);
		p.drawRoundedRect(r, 6, 6);
		if (cycle <= 0.0)
		{
			return;
		}
		double x = r.left();
		const double k = r.width() / cycle;
		QFont f = font();
		f.setPointSizeF(8.0);
		p.setFont(f);
		for (int i = 0; i < static_cast<int>(data.phases.size()); ++i)
		{
			const light_phase &ph = data.phases[i];
			struct part
			{
				double t;
				QColor c;
			};
			const part parts[4] =
			{
				{ data.red_yellow, QColor(255, 190, 130) },
				{ ph.green, ph.pedestrian_only ? QColor(167, 199, 231) : QColor(143, 209, 158) },
				{ ph.yellow, QColor(255, 224, 130) },
				{ ph.all_red, QColor(255, 138, 128) }
			};
			const double start = x;
			for (const part &pt : parts)
			{
				if (pt.t <= 0.0)
				{
					continue;
				}
				p.setPen(Qt::NoPen);
				p.setBrush(pt.c);
				p.drawRect(QRectF(x, r.top() + 1, pt.t * k, r.height() - 2));
				x += pt.t * k;
			}
			p.setPen(QColor(61, 74, 92));
			p.drawText(QRectF(start, r.bottom() + 2, x - start, 16), Qt::AlignCenter,
				tx("P%1 %2 s").arg(i + 1).arg(ph.green, 0, 'f', 0));
		}
	}
};

traffic_light_dialog::traffic_light_dialog(workspace *ws, int light_id, QWidget *parent)
	: QDialog(parent)
	, m_ws(ws)
	, m_light_id(light_id)
	, m_filling(false)
{
	resize(860, 600);
	const light_data *l = ws->model().light(light_id);
	if (l)
	{
		m_edit = *l;
	}

	QVBoxLayout *root = new QVBoxLayout(this);
	QGroupBox *gen = new QGroupBox(QStringLiteral("General"), this);
	QFormLayout *form = new QFormLayout(gen);
	m_algorithm = new QComboBox(gen);
	m_algorithm->addItems({ QStringLiteral("Fixed time"), QStringLiteral("Actuated (vehicle detection)"), QStringLiteral("External (API)") });
	m_algorithm->setCurrentIndex(static_cast<int>(m_edit.algorithm));
	m_offset = new QDoubleSpinBox(gen);
	m_offset->setRange(0.0, 600.0);
	m_offset->setSuffix(QStringLiteral(" s"));
	m_offset->setValue(m_edit.offset);
	m_red_yellow = new QDoubleSpinBox(gen);
	m_red_yellow->setRange(0.0, 3.0);
	m_red_yellow->setSingleStep(0.5);
	m_red_yellow->setSuffix(QStringLiteral(" s"));
	m_red_yellow->setValue(m_edit.red_yellow);
	form->addRow(QStringLiteral("Algorithm"), m_algorithm);
	form->addRow(QStringLiteral("Start offset (coordination)"), m_offset);
	form->addRow(QStringLiteral("Red and yellow before green"), m_red_yellow);
	root->addWidget(gen);

	QHBoxLayout *mid = new QHBoxLayout();
	QGroupBox *phases = new QGroupBox(QStringLiteral("Phases (in sequence)"), this);
	QVBoxLayout *pl = new QVBoxLayout(phases);
	m_table = new QTableWidget(phases);
	m_table->setColumnCount(7);
	m_table->setHorizontalHeaderLabels({ QStringLiteral("Green for"), QStringLiteral("Green"), QStringLiteral("Yellow"),
		QStringLiteral("All red"), QStringLiteral("Min green"), QStringLiteral("Max green"), QStringLiteral("Pedestrians only") });
	m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
	m_table->setSelectionMode(QAbstractItemView::SingleSelection);
	m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	m_table->verticalHeader()->setDefaultSectionSize(30);
	pl->addWidget(m_table);
	QHBoxLayout *btns = new QHBoxLayout();
	QPushButton *add = new QPushButton(QStringLiteral("Add phase"), phases);
	QPushButton *rem = new QPushButton(QStringLiteral("Remove phase"), phases);
	QPushButton *up = new QPushButton(QStringLiteral("Up"), phases);
	QPushButton *down = new QPushButton(QStringLiteral("Down"), phases);
	QPushButton *def = new QPushButton(QStringLiteral("Default program"), phases);
	btns->addWidget(add);
	btns->addWidget(rem);
	btns->addWidget(up);
	btns->addWidget(down);
	btns->addStretch(1);
	btns->addWidget(def);
	pl->addLayout(btns);
	mid->addWidget(phases, 3);

	QGroupBox *roads = new QGroupBox(QStringLiteral("Roads with green in the selected phase"), this);
	QVBoxLayout *rl = new QVBoxLayout(roads);
	m_roads = new QListWidget(roads);
	rl->addWidget(m_roads);
	mid->addWidget(roads, 1);
	root->addLayout(mid, 1);

	m_cycle = new QLabel(this);
	i18n::set_dynamic(m_cycle);
	m_timeline = new light_timeline(this);
	root->addWidget(m_cycle);
	root->addWidget(m_timeline);

	QDialogButtonBox *bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel, this);
	root->addWidget(bb);

	connect(bb, &QDialogButtonBox::accepted, this, [this]() { if (apply()) { accept(); } });
	connect(bb, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(bb->button(QDialogButtonBox::Apply), &QPushButton::clicked, this, [this]() { apply(); });
	connect(m_algorithm, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int i)
	{
		m_edit.algorithm = static_cast<light_algorithm>(i);
		update_summary();
	});
	connect(m_offset, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) { m_edit.offset = v; });
	connect(m_red_yellow, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v)
	{
		m_edit.red_yellow = v;
		update_summary();
	});
	connect(m_table, &QTableWidget::currentCellChanged, this, [this](int, int, int, int) { fill_roads(); });
	connect(m_roads, &QListWidget::itemChanged, this, [this](QListWidgetItem *)
	{
		if (m_filling)
		{
			return;
		}
		const int k = current_phase();
		if (k < 0)
		{
			return;
		}
		std::vector<int> ids;
		for (int i = 0; i < m_roads->count(); ++i)
		{
			if (m_roads->item(i)->checkState() == Qt::Checked)
			{
				ids.push_back(m_roads->item(i)->data(Qt::UserRole).toInt());
			}
		}
		m_edit.phases[k].green_roads = ids;
		// Only the text cell changes: the list must not be cleared in its own signal.
		QStringList names;
		for (int i = 0; i < m_roads->count(); ++i)
		{
			if (m_roads->item(i)->checkState() == Qt::Checked)
			{
				names << m_roads->item(i)->text();
			}
		}
		if (m_table->item(k, 0) && !m_edit.phases[k].pedestrian_only)
		{
			m_table->item(k, 0)->setText(names.join(QStringLiteral(", ")));
		}
		update_summary();
	});
	connect(add, &QPushButton::clicked, this, [this]()
	{
		light_phase ph;
		m_edit.phases.push_back(ph);
		fill_table();
		m_table->selectRow(static_cast<int>(m_edit.phases.size()) - 1);
	});
	connect(rem, &QPushButton::clicked, this, [this]()
	{
		const int k = current_phase();
		if (k >= 0 && m_edit.phases.size() > 1)
		{
			m_edit.phases.erase(m_edit.phases.begin() + k);
			fill_table();
			m_table->selectRow(std::min(k, static_cast<int>(m_edit.phases.size()) - 1));
		}
	});
	connect(up, &QPushButton::clicked, this, [this]()
	{
		const int k = current_phase();
		if (k > 0)
		{
			std::swap(m_edit.phases[k], m_edit.phases[k - 1]);
			fill_table();
			m_table->selectRow(k - 1);
		}
	});
	connect(down, &QPushButton::clicked, this, [this]()
	{
		const int k = current_phase();
		if (k >= 0 && k + 1 < static_cast<int>(m_edit.phases.size()))
		{
			std::swap(m_edit.phases[k], m_edit.phases[k + 1]);
			fill_table();
			m_table->selectRow(k + 1);
		}
	});
	connect(def, &QPushButton::clicked, this, [this]()
	{
		m_ws->model().default_light_program(m_edit);
		fill_table();
		m_table->selectRow(0);
	});

	fill_table();
	m_table->selectRow(0);

	i18n::retranslate(this);
	setWindowTitle(tx("Traffic light %1 - program").arg(light_id));
}

int traffic_light_dialog::current_phase() const
{
	const int k = m_table->currentRow();
	return (k >= 0 && k < static_cast<int>(m_edit.phases.size())) ? k : -1;
}

void traffic_light_dialog::fill_table()
{
	m_filling = true;
	m_table->setRowCount(static_cast<int>(m_edit.phases.size()));
	const world_model &m = m_ws->model();
	for (int k = 0; k < static_cast<int>(m_edit.phases.size()); ++k)
	{
		light_phase &ph = m_edit.phases[k];
		QStringList names;
		for (int rid : ph.green_roads)
		{
			const road_data *r = m.road(rid);
			names << ((r && !r->name.isEmpty()) ? r->name : tx("Road %1").arg(rid));
		}
		QTableWidgetItem *it = new QTableWidgetItem(ph.pedestrian_only ? tx("(pedestrians)") : names.join(QStringLiteral(", ")));
		it->setFlags(it->flags() & ~Qt::ItemIsEditable);
		m_table->setItem(k, 0, it);
		m_table->setVerticalHeaderItem(k, new QTableWidgetItem(tx("P%1").arg(k + 1)));
		auto spin = [this, k](int col, double value, double min, double max, double step, double light_phase::*field)
		{
			QDoubleSpinBox *sb = new QDoubleSpinBox(m_table);
			sb->setRange(min, max);
			sb->setSingleStep(step);
			sb->setDecimals(1);
			sb->setSuffix(tx(" s"));
			sb->setValue(value);
			sb->setFrame(false);
			connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, k, field](double v)
			{
				if (k < static_cast<int>(m_edit.phases.size()))
				{
					m_edit.phases[k].*field = v;
					update_summary();
				}
			});
			m_table->setCellWidget(k, col, sb);
		};
		spin(1, ph.green, 3.0, 300.0, 1.0, &light_phase::green);
		spin(2, ph.yellow, 0.0, 6.0, 0.5, &light_phase::yellow);
		spin(3, ph.all_red, 0.0, 10.0, 0.5, &light_phase::all_red);
		spin(4, ph.min_green, 3.0, 120.0, 1.0, &light_phase::min_green);
		spin(5, ph.max_green, 5.0, 300.0, 1.0, &light_phase::max_green);
		QCheckBox *cb = new QCheckBox(m_table);
		cb->setChecked(ph.pedestrian_only);
		connect(cb, &QCheckBox::toggled, this, [this, k](bool on)
		{
			if (k < static_cast<int>(m_edit.phases.size()))
			{
				m_edit.phases[k].pedestrian_only = on;
				update_summary();
			}
		});
		m_table->setCellWidget(k, 6, cb);
	}
	m_filling = false;
	fill_roads();
	update_summary();
}

void traffic_light_dialog::fill_roads()
{
	m_filling = true;
	m_roads->clear();
	const int k = current_phase();
	const world_model &m = m_ws->model();
	for (int rid : m.roads_at_node(m_edit.node_id))
	{
		const road_data *r = m.road(rid);
		const bool incoming = (r->node_b == m_edit.node_id) ? r->lanes_forward > 0 : r->lanes_backward > 0;
		if (!incoming)
		{
			continue;
		}
		QListWidgetItem *it = new QListWidgetItem(r->name.isEmpty() ? tx("Road %1").arg(rid) : tx("%1 (road %2)").arg(r->name).arg(rid), m_roads);
		it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
		it->setData(Qt::UserRole, rid);
		bool on = false;
		if (k >= 0)
		{
			const std::vector<int> &g = m_edit.phases[k].green_roads;
			on = std::find(g.begin(), g.end(), rid) != g.end();
		}
		it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
	}
	m_roads->setEnabled(k >= 0);
	m_filling = false;
}

void traffic_light_dialog::update_summary()
{
	double cycle = 0.0;
	for (const light_phase &ph : m_edit.phases)
	{
		cycle += ph.green + ph.yellow + ph.all_red + m_edit.red_yellow;
	}
	QString text = tx("Cycle time: %1 s, %2 phases.").arg(cycle, 0, 'f', 0).arg(m_edit.phases.size());
	if (m_edit.algorithm == light_algorithm::actuated)
	{
		text += QLatin1Char(' ') + tx("Actuated: the green time is between the minimum and the maximum green, it changes with the waiting vehicles.");
	}
	else if (m_edit.algorithm == light_algorithm::external)
	{
		text += QLatin1Char(' ') + tx("External: the phases come from the API. Without data, the light uses the fixed times.");
	}
	m_cycle->setText(text);
	m_cycle->setWordWrap(true);
	m_timeline->data = m_edit;
	m_timeline->update();
}

bool traffic_light_dialog::apply()
{
	light_data *l = m_ws->model().light(m_light_id);
	if (!l)
	{
		return false;
	}
	for (const light_phase &ph : m_edit.phases)
	{
		if (ph.green_roads.empty() && !ph.pedestrian_only)
		{
			fill_table();
			m_cycle->setText(tx("Each phase must have one or more roads with green, or it must be a pedestrian phase."));
			return false;
		}
	}
	*l = m_edit;
	m_ws->model().touch();
	m_ws->model_changed(property_change::change_none);
	logger::instance().action(QStringLiteral("Traffic light %1: new program (%2 phases)").arg(m_light_id).arg(m_edit.phases.size()));
	return true;
}
