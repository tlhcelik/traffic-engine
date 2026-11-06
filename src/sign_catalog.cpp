#include "sign_catalog.h"
#include "i18n.h"
#include "logger.h"
#include "xml_doc.h"

#include <QPainter>
#include <QPainterPath>
#include <QRectF>

#include <cmath>

namespace
{
	const int cell_size = 128;
	const int atlas_columns_count = 16;

	QColor color_or(const QString &text, const QColor &fallback)
	{
		const QColor c(text.trimmed());
		return c.isValid() ? c : fallback;
	}
}

// ---------------------------------------------------------------------------
// sign_catalog
// ---------------------------------------------------------------------------
sign_catalog::sign_catalog()
	: m_cells(0)
	, m_atlas_cols(atlas_columns_count)
	, m_atlas_cell(cell_size)
	, m_atlas_rev(0)
{
}

sign_catalog &sign_catalog::instance()
{
	static sign_catalog inst;
	return inst;
}

bool sign_catalog::load(const QString &path, QString *error)
{
	m_defs.clear();
	m_index.clear();
	xml_document doc;
	QString err;
	bool ok = doc.load(path, &err);
	if (ok)
	{
		for (const xml_element &e : doc.root().children(QStringLiteral("sign")))
		{
			sign_def d;
			d.key = e.attribute(QStringLiteral("key")).trimmed();
			if (d.key.isEmpty() || m_index.contains(d.key))
			{
				continue;
			}
			d.code = e.attribute(QStringLiteral("code")).trimmed();
			d.name = e.attribute(QStringLiteral("name"), d.key);
			d.name_tr = e.attribute(QStringLiteral("name_tr"));
			d.category = e.attribute(QStringLiteral("category"), QStringLiteral("information"));
			d.shape = e.attribute(QStringLiteral("shape"), QStringLiteral("circle"));
			d.bg = color_or(e.attribute(QStringLiteral("bg")), Qt::white);
			d.border = color_or(e.attribute(QStringLiteral("border")), QColor(214, 40, 40));
			d.fg = color_or(e.attribute(QStringLiteral("fg")), QColor(20, 20, 20));
			d.symbol = e.attribute(QStringLiteral("symbol"), QStringLiteral("none"));
			d.text = e.attribute(QStringLiteral("text"));
			d.mirror = e.attribute_bool(QStringLiteral("mirror"), false);
			d.slash = e.attribute_bool(QStringLiteral("slash"), false);
			d.panel = e.attribute_bool(QStringLiteral("panel"), false);
			d.effect = effect_from_name(e.attribute(QStringLiteral("effect")));
			d.default_value = e.attribute_int(QStringLiteral("value"), 0);
			m_index.insert(d.key, m_defs.size());
			m_defs.append(d);
		}
		ok = !m_defs.isEmpty();
		if (!ok)
		{
			err = QStringLiteral("The file has no signs: %1").arg(path);
		}
	}
	if (!ok)
	{
		TE_WARN("SIGNS", QStringLiteral("The sign catalog cannot be read (%1). The program uses a small built-in list.").arg(err));
		if (error)
		{
			*error = err;
		}
		add_builtin();
	}
	else
	{
		TE_INFO("SIGNS", QStringLiteral("Sign catalog: %1 signs from %2").arg(m_defs.size()).arg(path));
	}
	build_atlas();
	return ok;
}

void sign_catalog::add_builtin()
{
	auto add = [this](const char *key, const char *code, const char *name, const char *cat, const char *shape,
		QColor bg, QColor border, QColor fg, const char *symbol, const char *text, sign_effect eff, int value)
	{
		sign_def d;
		d.key = QString::fromLatin1(key);
		d.code = QString::fromLatin1(code);
		d.name = QString::fromLatin1(name);
		d.category = QString::fromLatin1(cat);
		d.shape = QString::fromLatin1(shape);
		d.bg = bg;
		d.border = border;
		d.fg = fg;
		d.symbol = QString::fromLatin1(symbol);
		d.text = QString::fromLatin1(text);
		d.effect = eff;
		d.default_value = value;
		m_index.insert(d.key, m_defs.size());
		m_defs.append(d);
	};
	const QColor red(214, 40, 40);
	const QColor white(Qt::white);
	const QColor black(20, 20, 20);
	add("give_way", "TT-1", "Give way", "regulatory", "triangle_down", white, red, black, "none", "", sign_effect::yield, 0);
	add("stop", "TT-2", "Stop", "regulatory", "octagon", red, red, white, "text", "DUR", sign_effect::stop, 0);
	add("speed_limit", "", "Maximum speed", "regulatory", "circle", white, red, black, "value", "", sign_effect::speed_limit, 50);
	add("no_entry", "", "No entry", "regulatory", "circle", red, red, white, "bar", "", sign_effect::no_entry, 0);
	add("no_overtaking", "", "No overtaking", "regulatory", "circle", white, red, black, "two_cars", "", sign_effect::no_overtaking, 0);
}

const sign_def *sign_catalog::find(const QString &key) const
{
	const auto it = m_index.constFind(key);
	return it == m_index.constEnd() ? nullptr : &m_defs.at(it.value());
}

QStringList sign_catalog::categories() const
{
	QStringList out;
	for (const sign_def &d : m_defs)
	{
		if (!out.contains(d.category))
		{
			out.append(d.category);
		}
	}
	return out;
}

void sign_catalog::build_atlas()
{
	const int n = m_defs.size();
	// Space for the default faces and for 64 more values.
	const int capacity = n + 64;
	const int rows = qMax(1, (capacity + m_atlas_cols - 1) / m_atlas_cols);
	m_atlas = QImage(m_atlas_cols * m_atlas_cell, rows * m_atlas_cell, QImage::Format_ARGB32_Premultiplied);
	m_atlas.fill(Qt::transparent);
	m_extra_cells.clear();
	QPainter p(&m_atlas);
	for (int i = 0; i < n; ++i)
	{
		sign_def &d = m_defs[i];
		d.atlas_index = i;
		const QRectF cell((i % m_atlas_cols) * m_atlas_cell, (i / m_atlas_cols) * m_atlas_cell, m_atlas_cell, m_atlas_cell);
		sign_painter::paint(p, cell, d, d.default_value);
	}
	p.end();
	m_cells = n;
	++m_atlas_rev;
}

int sign_catalog::atlas_cell(const QString &key, int value)
{
	const sign_def *d = find(key);
	if (!d)
	{
		return -1;
	}
	if (value <= 0 || value == d->default_value)
	{
		return d->atlas_index;
	}
	const QString k = key + QLatin1Char('|') + QString::number(value);
	const auto it = m_extra_cells.constFind(k);
	if (it != m_extra_cells.constEnd())
	{
		return it.value();
	}
	const int capacity = (m_atlas.height() / m_atlas_cell) * m_atlas_cols;
	if (m_cells >= capacity)
	{
		// Make the atlas larger (16 more rows) and keep the old cells.
		QImage bigger(m_atlas.width(), m_atlas.height() + 16 * m_atlas_cell, QImage::Format_ARGB32_Premultiplied);
		bigger.fill(Qt::transparent);
		QPainter bp(&bigger);
		bp.drawImage(0, 0, m_atlas);
		bp.end();
		m_atlas = bigger;
	}
	const int i = m_cells++;
	QPainter p(&m_atlas);
	const QRectF cell((i % m_atlas_cols) * m_atlas_cell, (i / m_atlas_cols) * m_atlas_cell, m_atlas_cell, m_atlas_cell);
	sign_painter::paint(p, cell, *d, value);
	p.end();
	m_extra_cells.insert(k, i);
	++m_atlas_rev;
	return i;
}

QString sign_def::display_name() const
{
	return (i18n::language() == QLatin1String("tr") && !name_tr.isEmpty()) ? name_tr : name;
}

QString sign_catalog::effect_text(sign_effect e)
{
	switch (e)
	{
		case sign_effect::none: return tx("Information only");
		case sign_effect::speed_limit: return tx("Speed limit");
		case sign_effect::end_speed_limit: return tx("End of the speed limit");
		case sign_effect::stop: return tx("Stop");
		case sign_effect::yield: return tx("Give way");
		case sign_effect::priority_road: return tx("Priority road");
		case sign_effect::end_priority_road: return tx("End of the priority road");
		case sign_effect::no_entry: return tx("No entry");
		case sign_effect::road_closed: return tx("Road closed");
		case sign_effect::no_overtaking: return tx("No overtaking");
		case sign_effect::end_no_overtaking: return tx("End of no overtaking");
		case sign_effect::no_left: return tx("No left turn");
		case sign_effect::no_right: return tx("No right turn");
		case sign_effect::no_uturn: return tx("No U-turn");
		case sign_effect::only_straight: return tx("Straight on only");
		case sign_effect::only_left: return tx("Left turn only");
		case sign_effect::only_right: return tx("Right turn only");
		case sign_effect::end_all: return tx("End of all prohibitions and limits");
		case sign_effect::straight_or_right: return tx("Straight on or right only");
		case sign_effect::straight_or_left: return tx("Straight on or left only");
		case sign_effect::left_or_right: return tx("Left or right only");
	}
	return tx("Information only");
}

sign_effect sign_catalog::effect_from_name(const QString &name)
{
	static const QMap<QString, sign_effect> map =
	{
		{ QStringLiteral("speed_limit"), sign_effect::speed_limit },
		{ QStringLiteral("end_speed_limit"), sign_effect::end_speed_limit },
		{ QStringLiteral("stop"), sign_effect::stop },
		{ QStringLiteral("yield"), sign_effect::yield },
		{ QStringLiteral("priority_road"), sign_effect::priority_road },
		{ QStringLiteral("end_priority_road"), sign_effect::end_priority_road },
		{ QStringLiteral("no_entry"), sign_effect::no_entry },
		{ QStringLiteral("road_closed"), sign_effect::road_closed },
		{ QStringLiteral("no_overtaking"), sign_effect::no_overtaking },
		{ QStringLiteral("end_no_overtaking"), sign_effect::end_no_overtaking },
		{ QStringLiteral("no_left"), sign_effect::no_left },
		{ QStringLiteral("no_right"), sign_effect::no_right },
		{ QStringLiteral("no_uturn"), sign_effect::no_uturn },
		{ QStringLiteral("only_straight"), sign_effect::only_straight },
		{ QStringLiteral("only_left"), sign_effect::only_left },
		{ QStringLiteral("only_right"), sign_effect::only_right },
		{ QStringLiteral("end_all"), sign_effect::end_all },
		{ QStringLiteral("straight_or_right"), sign_effect::straight_or_right },
		{ QStringLiteral("straight_or_left"), sign_effect::straight_or_left },
		{ QStringLiteral("left_or_right"), sign_effect::left_or_right },
	};
	return map.value(name.trimmed().toLower(), sign_effect::none);
}

QString sign_catalog::effect_name(sign_effect e)
{
	switch (e)
	{
		case sign_effect::none: return QStringLiteral("none");
		case sign_effect::speed_limit: return QStringLiteral("speed_limit");
		case sign_effect::end_speed_limit: return QStringLiteral("end_speed_limit");
		case sign_effect::stop: return QStringLiteral("stop");
		case sign_effect::yield: return QStringLiteral("yield");
		case sign_effect::priority_road: return QStringLiteral("priority_road");
		case sign_effect::end_priority_road: return QStringLiteral("end_priority_road");
		case sign_effect::no_entry: return QStringLiteral("no_entry");
		case sign_effect::road_closed: return QStringLiteral("road_closed");
		case sign_effect::no_overtaking: return QStringLiteral("no_overtaking");
		case sign_effect::end_no_overtaking: return QStringLiteral("end_no_overtaking");
		case sign_effect::no_left: return QStringLiteral("no_left");
		case sign_effect::no_right: return QStringLiteral("no_right");
		case sign_effect::no_uturn: return QStringLiteral("no_uturn");
		case sign_effect::only_straight: return QStringLiteral("only_straight");
		case sign_effect::only_left: return QStringLiteral("only_left");
		case sign_effect::only_right: return QStringLiteral("only_right");
		case sign_effect::end_all: return QStringLiteral("end_all");
		case sign_effect::straight_or_right: return QStringLiteral("straight_or_right");
		case sign_effect::straight_or_left: return QStringLiteral("straight_or_left");
		case sign_effect::left_or_right: return QStringLiteral("left_or_right");
	}
	return QStringLiteral("none");
}
