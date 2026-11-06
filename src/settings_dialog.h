#ifndef SETTINGS_DIALOG_H
#define SETTINGS_DIALOG_H

#include <QDialog>
#include <QMap>

class QWidget;

// settings_dialog
// ---------------
// The settings of the program (config.xml): general, logging, debug,
// rendering, simulation and OpenStreetMap. The logging settings apply
// immediately; some other settings need a new start of the program (the
// dialog shows a note).

class settings_dialog : public QDialog
{
	Q_OBJECT

public:
	explicit settings_dialog(QWidget *parent = nullptr);

private:
	void add_bool(QWidget *page, const QString &key, const QString &label);
	void add_int(QWidget *page, const QString &key, const QString &label, int min, int max, const QString &suffix = QString());
	void add_double(QWidget *page, const QString &key, const QString &label, double min, double max, double step, int decimals, const QString &suffix = QString());
	void add_choice(QWidget *page, const QString &key, const QString &label, const QStringList &values, const QStringList &texts);
	void add_text(QWidget *page, const QString &key, const QString &label);
	void apply();

	QMap<QString, QWidget *> m_editors;
};

#endif // SETTINGS_DIALOG_H
