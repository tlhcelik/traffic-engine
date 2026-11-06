#ifndef SIGN_PALETTE_H
#define SIGN_PALETTE_H

#include <QWidget>

class QComboBox;
class QLineEdit;
class QListWidget;

// sign_palette
// ------------
// The list of the traffic signs of the catalog (icons, codes, names). A
// filter by category and a search field help to find a sign. A click on a
// sign selects it for the sign tool (F4).

class sign_palette : public QWidget
{
	Q_OBJECT

public:
	explicit sign_palette(QWidget *parent = nullptr);
	void reload();

signals:
	void sign_chosen(const QString &key);

private:
	void fill();

	QComboBox *m_category;
	QLineEdit *m_search;
	QListWidget *m_list;
};

#endif // SIGN_PALETTE_H
