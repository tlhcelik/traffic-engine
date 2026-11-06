#include "sign_palette.h"
#include "i18n.h"
#include "sign_catalog.h"

#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPixmap>
#include <QVBoxLayout>

#include <algorithm>

namespace
{
	QString category_label(const QString &c)
	{
		if (c == QLatin1String("warning")) { return tx("Warning signs (T)"); }
		if (c == QLatin1String("regulatory")) { return tx("Regulatory signs (TT)"); }
		if (c == QLatin1String("mandatory")) { return tx("Mandatory signs"); }
		if (c == QLatin1String("information")) { return tx("Information signs (B)"); }
		if (c == QLatin1String("parking")) { return tx("Parking signs (P)"); }
		return c;
	}
}

sign_palette::sign_palette(QWidget *parent)
	: QWidget(parent)
{
	QVBoxLayout *l = new QVBoxLayout(this);
	l->setContentsMargins(6, 6, 6, 6);
	m_category = new QComboBox(this);
	i18n::set_dynamic(m_category); // items from the code (tx)
	m_search = new QLineEdit(this);
	m_search->setPlaceholderText(QStringLiteral("Search (name, Turkish name or code)"));
	m_search->setClearButtonEnabled(true);
	m_list = new QListWidget(this);
	m_list->setViewMode(QListView::IconMode);
	m_list->setIconSize(QSize(52, 52));
	m_list->setGridSize(QSize(92, 96));
	m_list->setResizeMode(QListView::Adjust);
	m_list->setMovement(QListView::Static);
	m_list->setWordWrap(true);
	m_list->setUniformItemSizes(true);
	QLabel *hint = new QLabel(QStringLiteral("Click a sign, then click a road side in the world (F4)."), this);
	hint->setWordWrap(true);
	hint->setStyleSheet(QStringLiteral("color: #6B7584;"));
	l->addWidget(m_category);
	l->addWidget(m_search);
	l->addWidget(m_list, 1);
	l->addWidget(hint);
	connect(m_category, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { fill(); });
	connect(m_search, &QLineEdit::textChanged, this, [this](const QString &) { fill(); });
	connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *it)
	{
		emit sign_chosen(it->data(Qt::UserRole).toString());
	});
	reload();
}

void sign_palette::reload()
{
	const QString selected = m_category->currentData().toString();
	m_category->blockSignals(true);
	m_category->clear();
	m_category->addItem(tx("All signs"), QString());
	for (const QString &c : sign_catalog::instance().categories())
	{
		m_category->addItem(category_label(c), c);
	}
	m_category->setCurrentIndex(std::max(0, m_category->findData(selected)));
	m_category->blockSignals(false);
	fill();
}

void sign_palette::fill()
{
	m_list->clear();
	const QString cat = m_category->currentData().toString();
	const QString q = m_search->text().trimmed();
	for (const sign_def &d : sign_catalog::instance().all())
	{
		if (!cat.isEmpty() && d.category != cat)
		{
			continue;
		}
		if (!q.isEmpty() && !d.name.contains(q, Qt::CaseInsensitive) && !d.name_tr.contains(q, Qt::CaseInsensitive)
			&& !d.code.contains(q, Qt::CaseInsensitive))
		{
			continue;
		}
		QListWidgetItem *it = new QListWidgetItem(QIcon(QPixmap::fromImage(sign_painter::image(d, d.default_value, 104))),
			d.code.isEmpty() ? d.display_name() : d.code + QLatin1Char('\n') + d.display_name());
		it->setData(Qt::UserRole, d.key);
		it->setToolTip(tx("<b>%1</b><br>Code: %2<br>English: %3<br>Turkish: %4<br>Effect: %5")
			.arg(d.display_name().toHtmlEscaped(), d.code.isEmpty() ? tx("not confirmed yet") : d.code,
				d.name.toHtmlEscaped(), d.name_tr.toHtmlEscaped(), sign_catalog::effect_text(d.effect)));
		m_list->addItem(it);
	}
}
