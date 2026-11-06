#ifndef I18N_H
#define I18N_H

#include <QList>
#include <QPair>
#include <QString>

class QObject;
class QWidget;

// i18n
// ----
// The languages of the user interface: English (the texts in the code, STE)
// and Turkish (data/lang/tr.xml, Xerces-C). One dictionary for each language:
// English source text -> translation. A missing translation gives the English
// text (debug log: one line for each missing text).
//
//   tx("...")        a text that the code makes at runtime (status bar,
//                    messages, popups, the texts in the OpenGL view). Use it
//                    before .arg(): tx("Speed: %1 km/h").arg(v).
//   tx_message(m)    a finished English message from a lower layer (errors of
//                    the file, network and import functions; the log keeps the
//                    English text). It finds the dictionary entry with %1 ... %99
//                    that agrees with the message and puts the values into the
//                    translation.
//   retranslate(w)   the fixed texts of a window: labels, buttons, group boxes,
//                    menus, actions, tool tips, status tips, combo box items,
//                    spin box suffixes, tabs, table headers, place holders. Each
//                    object keeps its English source (a dynamic property), thus
//                    a later call changes the language in both directions. If
//                    the code changes a text after the last call, the new text
//                    is the source.
//   set_dynamic(o)   retranslate() does not touch this object and its children
//                    (the code sets their texts with tx(), or they show user data).
//
// A Qt translator uses the same dictionary: the texts of Qt itself (OK, Cancel,
// Yes, No, the context menus of the text fields) are also translated.
// set_language() sends the Qt event LanguageChange.
// The log lines, the JSON report and the API stay in English.

namespace i18n
{
	// "en" or the code of a file data/lang/<code>.xml. Returns false if the
	// file cannot be read (then English).
	bool set_language(const QString &code);
	QString language();
	// The languages: code and native name ("en" English, "tr" Türkçe).
	QList<QPair<QString, QString>> languages();

	QString tx(const char *source);
	QString tx(const QString &source);
	QString tx_message(const QString &message);

	void retranslate(QWidget *root);
	void set_dynamic(QObject *object);
}

inline QString tx(const char *source) { return i18n::tx(source); }
inline QString tx(const QString &source) { return i18n::tx(source); }

#endif // I18N_H
