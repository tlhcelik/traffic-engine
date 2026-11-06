#include "property_panel.h"
#include "i18n.h"
#include "logger.h"
#include "workspace.h"
#include "world_properties.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

property_panel::property_panel(QWidget *parent)
	: QWidget(parent)
	, m_ws(nullptr)
	, m_revision(0)
	, m_content(nullptr)
	, m_layout(nullptr)
	, m_live(nullptr)
{
	QVBoxLayout *root = new QVBoxLayout(this);
	root->setContentsMargins(6, 6, 6, 6);
	m_title = new QLabel(this);
	QFont f = m_title->font();
	f.setBold(true);
	f.setPointSizeF(f.pointSizeF() + 1.0);
	m_title->setFont(f);
	m_title->setWordWrap(true);
	root->addWidget(m_title);
	i18n::set_dynamic(m_title);

	m_scroll = new QScrollArea(this);
	m_scroll->setWidgetResizable(true);
	m_scroll->setFrameShape(QFrame::NoFrame);
	root->addWidget(m_scroll, 1);

	QHBoxLayout *buttons = new QHBoxLayout();
	m_program = new QPushButton(QStringLiteral("Traffic light program..."), this);
	m_remove = new QPushButton(QStringLiteral("Remove"), this);
	buttons->addWidget(m_program);
	buttons->addWidget(m_remove);
	root->addLayout(buttons);
	connect(m_program, &QPushButton::clicked, this, [this]()
	{
		if (m_ref.kind == element_kind::light)
		{
			emit edit_light_program(m_ref.id);
		}
	});
	connect(m_remove, &QPushButton::clicked, this, &property_panel::remove_selected);
	rebuild();
}

void property_panel::set_workspace(workspace *ws)
{
	m_ws = ws;
	rebuild();
}

void property_panel::apply(const QString &key, const QVariant &value)
{
	if (!m_ws)
	{
		return;
	}
	property_change change = property_change::change_none;
	const bool many = !m_roads.empty();
	const bool changed = many ? world_properties::set_roads(m_ws->model(), m_roads, key, value, &change)
		: world_properties::set(m_ws->model(), m_ref, key, value, &change);
	if (changed)
	{
		QString target = world_properties::title(m_ws->model(), m_ref, false);
		if (many)
		{
			QStringList ids;
			for (int id : m_roads)
			{
				ids << QString::number(id);
			}
			target = QStringLiteral("Roads %1").arg(ids.join(QStringLiteral(", ")));
		}
		logger::instance().action(QStringLiteral("Property %1.%2 = %3").arg(target, key, value.toString()));
		m_ws->model_changed(change);
		m_revision = m_ws->model().revision;
		emit edited();
	}
}

void property_panel::rebuild()
{
	m_ref = m_ws ? m_ws->selection() : element_ref();
	m_roads = m_ws ? m_ws->selected_roads() : std::vector<int>();
	if (m_roads.size() < 2)
	{
		m_roads.clear();
	}
	m_revision = m_ws ? m_ws->model().revision : 0;
	m_content = new QWidget();
	i18n::set_dynamic(m_content); // tx() below: the values can be user data (names)
	m_layout = new QVBoxLayout(m_content);
	m_layout->setContentsMargins(0, 0, 4, 0);
	m_layout->setSpacing(8);
	m_live = nullptr;

	if (!m_ws)
	{
		m_title->setText(tx("No workspace"));
		m_scroll->setWidget(m_content);
		m_program->setVisible(false);
		m_remove->setVisible(false);
		i18n::retranslate(this);
		return;
	}

	const bool dynamic = m_ref.kind == element_kind::vehicle || m_ref.kind == element_kind::pedestrian;
	m_title->setText(m_roads.empty() ? world_properties::title(m_ws->model(), m_ref) : tx("%1 roads selected").arg(m_roads.size()));
	m_program->setVisible(m_ref.kind == element_kind::light);
	m_remove->setVisible(m_ref.valid() && !dynamic);
	m_remove->setEnabled(m_ws->editor_mode());
	m_remove->setToolTip(!m_ws->editor_mode() ? tx("Remove is possible only in editor mode.")
		: (m_roads.empty() ? tx("Remove the selected element.") : tx("Remove the selected roads.")));

	if (dynamic)
	{
		QGroupBox *box = new QGroupBox(tx("Live data"), m_content);
		QVBoxLayout *bl = new QVBoxLayout(box);
		m_live = new QLabel(box);
		i18n::set_dynamic(m_live);
		m_live->setWordWrap(true);
		// Rich text: the state has a link to the element that causes the wait.
		m_live->setTextFormat(Qt::RichText);
		m_live->setTextInteractionFlags(Qt::TextBrowserInteraction);
		m_live->setOpenExternalLinks(false);
		connect(m_live, &QLabel::linkActivated, this, [this](const QString &link)
		{
			static const struct { const char *name; element_kind kind; } kinds[] =
			{
				{ "vehicle", element_kind::vehicle }, { "light", element_kind::light }, { "sign", element_kind::sign },
				{ "pedestrian", element_kind::pedestrian }, { "object", element_kind::object },
				{ "node", element_kind::node }, { "road", element_kind::road }
			};
			const QStringList parts = link.split(QLatin1Char(':'));
			if (parts.size() != 2)
			{
				return;
			}
			for (const auto &k : kinds)
			{
				if (parts.at(0) == QLatin1String(k.name))
				{
					emit go_to(element_ref(k.kind, parts.at(1).toInt()));
					return;
				}
			}
		});
		bl->addWidget(m_live);
		m_layout->addWidget(box);
		m_layout->addStretch(1);
		m_scroll->setWidget(m_content);
		i18n::retranslate(this);
		refresh();
		return;
	}

	const QList<property_def> defs = m_roads.empty() ? world_properties::list(m_ws->model(), m_ref)
		: world_properties::list_roads(m_ws->model(), m_roads);
	QGroupBox *box = nullptr;
	QFormLayout *form = nullptr;
	QString group;
	for (const property_def &d : defs)
	{
		if (!box || d.group != group)
		{
			group = d.group;
			box = new QGroupBox(i18n::tx_message(group), m_content); // also "Phase %1"
			form = new QFormLayout(box);
			form->setLabelAlignment(Qt::AlignLeft);
			form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
			m_layout->addWidget(box);
		}
		QWidget *editor = nullptr;
		const QString key = d.key;
		switch (d.type)
		{
			case property_def::t_bool:
			{
				QCheckBox *cb = new QCheckBox(box);
				cb->setChecked(d.value.toBool());
				connect(cb, &QCheckBox::toggled, this, [this, key](bool on) { apply(key, on); });
				editor = cb;
				break;
			}
			case property_def::t_int:
			{
				QSpinBox *sb = new QSpinBox(box);
				sb->setRange(static_cast<int>(d.min), static_cast<int>(d.max));
				sb->setValue(d.value.toInt());
				sb->setKeyboardTracking(false);
				if (!d.unit.isEmpty())
				{
					sb->setSuffix(QStringLiteral(" ") + tx(d.unit));
				}
				connect(sb, QOverload<int>::of(&QSpinBox::valueChanged), this, [this, key](int v) { apply(key, v); });
				editor = sb;
				break;
			}
			case property_def::t_double:
			{
				QDoubleSpinBox *sb = new QDoubleSpinBox(box);
				sb->setDecimals(d.decimals);
				sb->setRange(d.min, d.max);
				sb->setSingleStep(d.step);
				sb->setValue(d.value.toDouble());
				sb->setKeyboardTracking(false);
				if (!d.unit.isEmpty())
				{
					sb->setSuffix(QStringLiteral(" ") + tx(d.unit));
				}
				connect(sb, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this, key](double v) { apply(key, v); });
				editor = sb;
				break;
			}
			case property_def::t_string:
			{
				QLineEdit *le = new QLineEdit(d.value.toString(), box);
				connect(le, &QLineEdit::editingFinished, this, [this, key, le]() { apply(key, le->text()); });
				editor = le;
				break;
			}
			case property_def::t_enum:
			{
				QComboBox *cb = new QComboBox(box);
				cb->addItems(d.options);
				cb->setCurrentIndex(d.value.toInt());
				cb->setMaxVisibleItems(20);
				connect(cb, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this, key](int i) { apply(key, i); });
				editor = cb;
				break;
			}
			case property_def::t_info:
			{
				QString text = d.value.toString();
				if (!d.unit.isEmpty())
				{
					text += QStringLiteral(" ") + tx(d.unit);
				}
				QLabel *l = new QLabel(text, box);
				l->setWordWrap(true);
				l->setTextInteractionFlags(Qt::TextSelectableByMouse);
				editor = l;
				break;
			}
		}
		if (d.mixed)
		{
			// More roads with different values: the value of the main road shows
			editor->setToolTip(tx("The selected roads have different values. A change applies to all of them."));
		}
		form->addRow(d.mixed ? tx(d.label) + QStringLiteral(" *") : tx(d.label), editor);
	}
	if (!m_roads.empty())
	{
		QLabel *hint = new QLabel(tx("A change applies to all selected roads. * The roads have different values: "
			"the value of the last selected road shows. Ctrl+click a road to add it to the selection or to remove it."), m_content);
		hint->setWordWrap(true);
		hint->setStyleSheet(QStringLiteral("color: #6B7584;"));
		m_layout->addWidget(hint);
	}
	if (m_ref.kind == element_kind::none)
	{
		QLabel *hint = new QLabel(tx("Select an element in the world to see its properties. "
			"You can change the properties when the flow runs."), m_content);
		hint->setWordWrap(true);
		hint->setStyleSheet(QStringLiteral("color: #6B7584;"));
		m_layout->addWidget(hint);
	}
	m_layout->addStretch(1);
	m_scroll->setWidget(m_content);
	i18n::retranslate(this);
}

void property_panel::refresh()
{
	if (!m_ws)
	{
		return;
	}
	if (m_ws->selection() != m_ref)
	{
		rebuild();
		return;
	}
	if (m_live && m_live_source)
	{
		QString title;
		const QStringList lines = m_live_source(m_ref, &title);
		const QString text = lines.isEmpty() ? tx("The element does not exist anymore.").toHtmlEscaped() : lines.join(QStringLiteral("<br>"));
		if (text != m_live->text())
		{
			m_live->setText(text); // no new text: the link under the mouse stays
		}
		if (!title.isEmpty())
		{
			m_title->setText(title);
		}
		return;
	}
	if (m_ws->model().revision != m_revision)
	{
		QWidget *focus = QApplication::focusWidget();
		if (!focus || !m_content || !m_content->isAncestorOf(focus))
		{
			rebuild();
		}
	}
	m_remove->setEnabled(m_ws->editor_mode());
}
