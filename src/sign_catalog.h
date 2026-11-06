#ifndef SIGN_CATALOG_H
#define SIGN_CATALOG_H

#include <QColor>
#include <QImage>
#include <QList>
#include <QMap>
#include <QString>

// sign_catalog
// ------------
// The list of the traffic signs (data\signs\tr_signs.xml, Xerces-C). Each
// sign has:
//   key       stable internal name. The .te file stores this key.
//   code      official code of the Turkish regulation (for example "TT-2").
//             Empty if the code is not confirmed yet.
//   name      STE English name, name_tr: Turkish name
//   category  warning (T), regulatory (TT), mandatory, information (B), parking (P)
//   shape, colors and symbol: the look of the sign face (sign_painter draws it)
//   effect    what the sign changes in the traffic (speed limit, stop, ...)
//
// sign_painter (src/sign_painter.cpp) draws a sign face with QPainter
// (vector, smooth at all sizes). The OpenGL world puts the faces in one
// texture atlas.

enum class sign_effect
{
	none = 0,
	speed_limit,
	end_speed_limit,
	stop,
	yield,
	priority_road,
	end_priority_road,
	no_entry,
	road_closed,
	no_overtaking,
	end_no_overtaking,
	no_left,
	no_right,
	no_uturn,
	only_straight,
	only_left,
	only_right,
	end_all,
	straight_or_right,
	straight_or_left,
	left_or_right
};

struct sign_def
{
	QString key;
	QString code;
	QString name;
	QString name_tr;
	QString category;
	QString shape;      // triangle, triangle_down, circle, octagon, square, rect, diamond,
	                    // board, board_wide, saltire
	QColor bg;
	QColor border;
	QColor fg;
	QString symbol;     // see sign_painter (src/sign_painter.cpp)
	QString text;       // text for the symbols "text", "letter" and the value signs (for example "3.5 m")
	bool mirror = false; // the pictogram is mirrored (left / right variant)
	bool slash = false;  // red diagonal bar ("end of ...")
	bool panel = false;  // white panel of a blue service sign
	sign_effect effect = sign_effect::none;
	int default_value = 0;
	int atlas_index = -1;

	// The code (if it is known) and the name in the language of the user interface.
	QString label() const { return code.isEmpty() ? display_name() : code + QStringLiteral("  ") + display_name(); }
	// The name in the language of the user interface (name_tr in Turkish).
	QString display_name() const;
};

class sign_catalog
{
public:
	static sign_catalog &instance();

	// Loads the file. On an error, the catalog has a small built-in list.
	bool load(const QString &path, QString *error = nullptr);
	bool loaded() const { return !m_defs.isEmpty(); }

	const QList<sign_def> &all() const { return m_defs; }
	const sign_def *find(const QString &key) const;
	QStringList categories() const;

	// All faces in one image (cell x cell pixels each). The first cells have
	// the default values (atlas_index). atlas_cell() adds a cell for another
	// value (for example a speed limit of 70) when it is necessary.
	int atlas_cell(const QString &key, int value);
	const QImage &atlas() const { return m_atlas; }
	int atlas_columns() const { return m_atlas_cols; }
	int atlas_cell() const { return m_atlas_cell; }
	quint64 atlas_revision() const { return m_atlas_rev; }

	static sign_effect effect_from_name(const QString &name);
	static QString effect_name(sign_effect e);
	// Description of the effect for the user (translated).
	static QString effect_text(sign_effect e);

private:
	sign_catalog();
	void build_atlas();
	void add_builtin();

	QList<sign_def> m_defs;
	QMap<QString, int> m_index;
	QMap<QString, int> m_extra_cells;   // "key|value" -> cell
	int m_cells;
	QImage m_atlas;
	int m_atlas_cols;
	int m_atlas_cell;
	quint64 m_atlas_rev;
};

class QPainter;
class QRectF;

namespace sign_painter
{
	// Draws the sign face in the rectangle. value: shown by the symbol "value".
	void paint(QPainter &p, const QRectF &rect, const sign_def &def, int value);
	// An image of the sign face (for icons and lists).
	QImage image(const sign_def &def, int value, int size);
}

#endif // SIGN_CATALOG_H
