#include "i18n.h"
#include "app_paths.h"
#include "logger.h"
#include "xml_doc.h"

#include <QAbstractButton>
#include <QAction>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialog>
#include <QDir>
#include <QDockWidget>
#include <QDoubleSpinBox>
#include <QGroupBox>
#include <QHash>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMutex>
#include <QRegularExpression>
#include <QSet>
#include <QSpinBox>
#include <QTabBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QToolBar>
#include <QToolButton>
#include <QTranslator>
#include <QVariant>
#include <QVector>

#include <algorithm>
#include <memory>

namespace
{
	// An entry with %1 ... %99: it finds the values in a finished message.
	struct pattern
	{
		QRegularExpression re;
		QVector<int> args;   // the number of each capture group
		int literal = 0;     // characters (not spaces) that are not values
		QString text;
	};

	struct dictionary
	{
		QString code = QStringLiteral("en");
		QHash<QString, QString> texts;
		QSet<QString> outputs;      // the translations: a translated text is not missing
		QVector<pattern> patterns;  // for tx_message(), the most specific first
	};

	const QRegularExpression &marker()
	{
		static const QRegularExpression re(QStringLiteral("%([1-9][0-9]?)"));
		return re;
	}

	void make_patterns(dictionary &d)
	{
		for (auto it = d.texts.cbegin(); it != d.texts.cend(); ++it)
		{
			const QString &en = it.key();
			pattern p;
			QString re = QStringLiteral("^");
			int pos = 0;
			auto letters = [](const QString &part) { return part.size() - part.count(QLatin1Char(' ')); };
			QRegularExpressionMatchIterator mi = marker().globalMatch(en);
			while (mi.hasNext())
			{
				const QRegularExpressionMatch m = mi.next();
				const QString part = en.mid(pos, m.capturedStart() - pos);
				re += QRegularExpression::escape(part);
				p.literal += letters(part);
				re += QStringLiteral("(.*?)");
				p.args.append(m.captured(1).toInt());
				pos = m.capturedEnd();
			}
			p.literal += letters(en.mid(pos));
			// Too general patterns ("%1 %2", "%1 km", "P%1") do not identify a message.
			if (p.args.isEmpty() || p.literal < 4)
			{
				continue;
			}
			re += QRegularExpression::escape(en.mid(pos)) + QStringLiteral("$");
			p.re = QRegularExpression(re, QRegularExpression::DotMatchesEverythingOption);
			p.text = it.value();
			d.patterns.append(p);
		}
		std::sort(d.patterns.begin(), d.patterns.end(), [](const pattern &a, const pattern &b) { return a.literal > b.literal; });
	}

	// Puts the values into the translation (%N -> the value of %N in the English text).
	QString fill(const QString &text, const QHash<int, QString> &values)
	{
		QString out;
		int pos = 0;
		QRegularExpressionMatchIterator mi = marker().globalMatch(text);
		while (mi.hasNext())
		{
			const QRegularExpressionMatch m = mi.next();
			out += text.midRef(pos, m.capturedStart() - pos);
			const QString num = m.captured(1);
			if (values.contains(num.toInt()))
			{
				out += values.value(num.toInt());
			}
			else if (num.size() == 2 && values.contains(num.left(1).toInt()))
			{
				out += values.value(num.left(1).toInt()) + num.right(1);
			}
			else
			{
				out += m.captured(0);
			}
			pos = m.capturedEnd();
		}
		out += text.midRef(pos);
		return out;
	}

	// The current dictionary. A new language makes a new object: a reader in
	// another thread (OpenStreetMap worker) keeps its copy.
	std::shared_ptr<const dictionary> g_dict = std::make_shared<dictionary>();
	QMutex g_mutex;
	QSet<QString> g_missing;

	std::shared_ptr<const dictionary> current()
	{
		QMutexLocker lock(&g_mutex);
		return g_dict;
	}

	void report_missing(const QString &source)
	{
		if (!logger::is_on(logger::level_debug))
		{
			return;
		}
		{
			QMutexLocker lock(&g_mutex);
			if (g_missing.contains(source))
			{
				return;
			}
			g_missing.insert(source);
		}
		TE_DEBUG("I18N", QStringLiteral("No translation: %1").arg(QString(source).replace(QLatin1Char('\n'), QStringLiteral("\\n"))));
	}

	QString lookup(const dictionary &d, const QString &source)
	{
		if (d.code == QLatin1String("en") || source.trimmed().isEmpty())
		{
			return source;
		}
		const auto it = d.texts.constFind(source);
		if (it != d.texts.constEnd())
		{
			return it.value();
		}
		if (!d.outputs.contains(source))
		{
			report_missing(source);
		}
		return source;
	}

	// The texts of Qt itself (standard buttons, context menus) come here.
	class te_translator : public QTranslator
	{
	public:
		explicit te_translator(QObject *parent) : QTranslator(parent) {}

		QString translate(const char *context, const char *source, const char *disambiguation, int n) const override
		{
			Q_UNUSED(context);
			Q_UNUSED(disambiguation);
			Q_UNUSED(n);
			if (!source || !*source)
			{
				return QString();
			}
			return current()->texts.value(QString::fromUtf8(source));
		}

		bool isEmpty() const override
		{
			return current()->texts.isEmpty();
		}
	};

	te_translator *g_translator = nullptr;

	const char *const p_skip = "_te_i18n_skip";
	const int role_src = Qt::UserRole + 900;
	const int role_out = Qt::UserRole + 901;

	bool skipped(const QObject *o)
	{
		for (const QObject *p = o; p; p = p->parent())
		{
			if (p->property(p_skip).toBool())
			{
				return true;
			}
		}
		return false;
	}

	// The translation of a fixed text; "Text (Ctrl+X)" keeps the key.
	QString ui_text(const dictionary &d, const QString &src)
	{
		if (d.code == QLatin1String("en") || src.trimmed().isEmpty())
		{
			return src;
		}
		const auto it = d.texts.constFind(src);
		if (it != d.texts.constEnd())
		{
			return it.value();
		}
		static const QRegularExpression with_key(QStringLiteral("^(.+) \\(([^()]+)\\)$"));
		const QRegularExpressionMatch m = with_key.match(src);
		if (m.hasMatch())
		{
			const auto it2 = d.texts.constFind(m.captured(1));
			if (it2 != d.texts.constEnd())
			{
				return QStringLiteral("%1 (%2)").arg(it2.value(), m.captured(2));
			}
		}
		if (!d.outputs.contains(src))
		{
			report_missing(src);
		}
		return src;
	}

	// One text of an object. The English source stays in a dynamic property;
	// a text that the code changed after the last call is the new source.
	template <typename Get, typename Set>
	void handle(QObject *o, const char *name, const dictionary &d, Get get, Set set)
	{
		const QString now = get();
		const QByteArray sp = QByteArray("_te_src_") + name;
		const QByteArray op = QByteArray("_te_out_") + name;
		const QVariant src_v = o->property(sp.constData());
		const QVariant out_v = o->property(op.constData());
		if (now.isEmpty() && !src_v.isValid())
		{
			return;
		}
		QString src = now;
		if (src_v.isValid() && out_v.isValid() && out_v.toString() == now)
		{
			src = src_v.toString();
		}
		const QString t = ui_text(d, src);
		o->setProperty(sp.constData(), src);
		o->setProperty(op.constData(), t);
		if (t != now)
		{
			set(t);
		}
	}

	// The same rule for one item of a model (combo box item, table header).
	template <typename Get, typename Set, typename GetData, typename SetData>
	void handle_item(const dictionary &d, Get get, Set set, GetData get_data, SetData set_data)
	{
		const QString now = get();
		const QVariant src_v = get_data(role_src);
		const QVariant out_v = get_data(role_out);
		QString src = now;
		if (src_v.isValid() && out_v.isValid() && out_v.toString() == now)
		{
			src = src_v.toString();
		}
		const QString t = ui_text(d, src);
		set_data(role_src, src);
		set_data(role_out, t);
		if (t != now)
		{
			set(t);
		}
	}

	// The text without "&" and "...": the tool tip of an action without an own tool tip.
	QString stripped(QString s)
	{
		s.remove(QStringLiteral("..."));
		for (int i = 0; i < s.size(); ++i)
		{
			if (s.at(i) == QLatin1Char('&'))
			{
				s.remove(i, 1);
			}
		}
		return s.trimmed();
	}

	void translate_widget(QWidget *w, const dictionary &d)
	{
		if (qobject_cast<QDialog *>(w) || qobject_cast<QDockWidget *>(w) || qobject_cast<QToolBar *>(w))
		{
			handle(w, "wtitle", d, [w]() { return w->windowTitle(); }, [w](const QString &t) { w->setWindowTitle(t); });
		}
		// A tool button of an action copies the texts of the action.
		const QToolButton *tb = qobject_cast<QToolButton *>(w);
		if (tb && tb->defaultAction())
		{
			return;
		}
		handle(w, "tip", d, [w]() { return w->toolTip(); }, [w](const QString &t) { w->setToolTip(t); });
		handle(w, "status", d, [w]() { return w->statusTip(); }, [w](const QString &t) { w->setStatusTip(t); });
		if (QLabel *l = qobject_cast<QLabel *>(w))
		{
			handle(w, "text", d, [l]() { return l->text(); }, [l](const QString &t) { l->setText(t); });
		}
		else if (QAbstractButton *b = qobject_cast<QAbstractButton *>(w))
		{
			handle(w, "text", d, [b]() { return b->text(); }, [b](const QString &t) { b->setText(t); });
		}
		if (QGroupBox *g = qobject_cast<QGroupBox *>(w))
		{
			handle(w, "gtitle", d, [g]() { return g->title(); }, [g](const QString &t) { g->setTitle(t); });
		}
		if (QMenu *m = qobject_cast<QMenu *>(w))
		{
			handle(w, "mtitle", d, [m]() { return m->title(); }, [m](const QString &t) { m->setTitle(t); });
		}
		if (QLineEdit *e = qobject_cast<QLineEdit *>(w))
		{
			handle(w, "placeholder", d, [e]() { return e->placeholderText(); }, [e](const QString &t) { e->setPlaceholderText(t); });
		}
		if (QSpinBox *s = qobject_cast<QSpinBox *>(w))
		{
			handle(w, "suffix", d, [s]() { return s->suffix(); }, [s](const QString &t) { s->setSuffix(t); });
			handle(w, "special", d, [s]() { return s->specialValueText(); }, [s](const QString &t) { s->setSpecialValueText(t); });
		}
		if (QDoubleSpinBox *s = qobject_cast<QDoubleSpinBox *>(w))
		{
			handle(w, "suffix", d, [s]() { return s->suffix(); }, [s](const QString &t) { s->setSuffix(t); });
			handle(w, "special", d, [s]() { return s->specialValueText(); }, [s](const QString &t) { s->setSpecialValueText(t); });
		}
		if (QComboBox *c = qobject_cast<QComboBox *>(w))
		{
			if (!c->isEditable())
			{
				for (int i = 0; i < c->count(); ++i)
				{
					handle_item(d, [c, i]() { return c->itemText(i); }, [c, i](const QString &t) { c->setItemText(i, t); },
						[c, i](int role) { return c->itemData(i, role); }, [c, i](int role, const QVariant &v) { c->setItemData(i, v, role); });
				}
			}
		}
		if (QTabWidget *tw = qobject_cast<QTabWidget *>(w))
		{
			QTabBar *bar = tw->tabBar();
			for (int i = 0; i < tw->count(); ++i)
			{
				// tab data: a map with the source and the last output
				handle_item(d, [tw, i]() { return tw->tabText(i); }, [tw, i](const QString &t) { tw->setTabText(i, t); },
					[bar, i](int role) { return bar->tabData(i).toMap().value(QString::number(role)); },
					[bar, i](int role, const QVariant &v)
					{
						QVariantMap map = bar->tabData(i).toMap();
						map.insert(QString::number(role), v);
						bar->setTabData(i, map);
					});
			}
		}
		if (QTableWidget *t = qobject_cast<QTableWidget *>(w))
		{
			for (int i = 0; i < t->columnCount(); ++i)
			{
				QTableWidgetItem *it = t->horizontalHeaderItem(i);
				if (!it)
				{
					continue;
				}
				handle_item(d, [it]() { return it->text(); }, [it](const QString &s) { it->setText(s); },
					[it](int role) { return it->data(role); }, [it](int role, const QVariant &v) { it->setData(role, v); });
			}
		}
	}
}

namespace i18n
{
	bool set_language(const QString &code_in)
	{
		const QString code = code_in.trimmed().toLower();
		std::shared_ptr<dictionary> d = std::make_shared<dictionary>();
		bool ok = true;
		if (!code.isEmpty() && code != QLatin1String("en"))
		{
			const QString path = app_paths::app_data_dir() + QStringLiteral("/lang/") + code + QStringLiteral(".xml");
			xml_document doc;
			QString err;
			if (doc.load(path, &err) && doc.root().name() == QLatin1String("language"))
			{
				d->code = code;
				for (xml_element e = doc.root().first_child(QStringLiteral("entry")); !e.is_null(); e = e.next_sibling(QStringLiteral("entry")))
				{
					const QString en = e.attribute(QStringLiteral("en"));
					const QString text = e.attribute(QStringLiteral("text"));
					if (!en.isEmpty() && !text.isEmpty())
					{
						d->texts.insert(en, text);
						d->outputs.insert(text);
					}
				}
				make_patterns(*d);
			}
			else
			{
				ok = false;
				TE_WARN("I18N", QStringLiteral("The language file %1 cannot be read (%2). The language is English.").arg(path, err));
			}
		}
		{
			QMutexLocker lock(&g_mutex);
			g_dict = d;
			g_missing.clear();
		}
		// LanguageChange for the texts of Qt: remove and install the translator.
		if (QCoreApplication::instance())
		{
			if (!g_translator)
			{
				g_translator = new te_translator(QCoreApplication::instance());
			}
			QCoreApplication::removeTranslator(g_translator);
			if (d->code != QLatin1String("en"))
			{
				QCoreApplication::installTranslator(g_translator);
			}
		}
		TE_INFO("I18N", QStringLiteral("Language: %1 (%2 texts)").arg(d->code).arg(d->texts.size()));
		return ok;
	}

	QString language()
	{
		return current()->code;
	}

	QList<QPair<QString, QString>> languages()
	{
		QList<QPair<QString, QString>> out;
		out.append(qMakePair(QStringLiteral("en"), QStringLiteral("English")));
		const QDir dir(app_paths::app_data_dir() + QStringLiteral("/lang"));
		for (const QString &f : dir.entryList({ QStringLiteral("*.xml") }, QDir::Files, QDir::Name))
		{
			xml_document doc;
			if (doc.load(dir.filePath(f)) && doc.root().name() == QLatin1String("language"))
			{
				const QString code = doc.root().attribute(QStringLiteral("code"), QFileInfo(f).completeBaseName());
				out.append(qMakePair(code, doc.root().attribute(QStringLiteral("name"), code)));
			}
		}
		return out;
	}

	QString tx(const char *source)
	{
		const std::shared_ptr<const dictionary> d = current();
		if (d->code == QLatin1String("en"))
		{
			return QString::fromUtf8(source);
		}
		return lookup(*d, QString::fromUtf8(source));
	}

	QString tx(const QString &source)
	{
		const std::shared_ptr<const dictionary> d = current();
		return lookup(*d, source);
	}

	QString tx_message(const QString &message)
	{
		const std::shared_ptr<const dictionary> d = current();
		if (d->code == QLatin1String("en") || message.trimmed().isEmpty())
		{
			return message;
		}
		const auto it = d->texts.constFind(message);
		if (it != d->texts.constEnd())
		{
			return it.value();
		}
		if (d->outputs.contains(message))
		{
			return message;
		}
		for (const pattern &p : d->patterns)
		{
			const QRegularExpressionMatch m = p.re.match(message);
			if (!m.hasMatch())
			{
				continue;
			}
			QHash<int, QString> values;
			for (int i = 0; i < p.args.size(); ++i)
			{
				if (!values.contains(p.args.at(i)))
				{
					values.insert(p.args.at(i), m.captured(i + 1));
				}
			}
			return fill(p.text, values);
		}
		report_missing(message);
		return message;
	}

	void retranslate(QWidget *root)
	{
		if (!root || skipped(root))
		{
			return;
		}
		const std::shared_ptr<const dictionary> d = current();
		QList<QWidget *> widgets = root->findChildren<QWidget *>();
		widgets.prepend(root);
		for (QWidget *w : widgets)
		{
			if (!skipped(w))
			{
				translate_widget(w, *d);
			}
		}
		for (QAction *a : root->findChildren<QAction *>())
		{
			// The action of a menu and the view action of a dock follow their title.
			if (a->isSeparator() || a->menu() || qobject_cast<QDockWidget *>(a->parent()) || skipped(a))
			{
				continue;
			}
			handle(a, "text", *d, [a]() { return a->text(); }, [a](const QString &t) { a->setText(t); });
			// Without an own tool tip, the action shows its text: nothing to translate.
			if (a->toolTip() != stripped(a->text()))
			{
				handle(a, "tip", *d, [a]() { return a->toolTip(); }, [a](const QString &t) { a->setToolTip(t); });
			}
			handle(a, "status", *d, [a]() { return a->statusTip(); }, [a](const QString &t) { a->setStatusTip(t); });
		}
	}

	void set_dynamic(QObject *object)
	{
		if (object)
		{
			object->setProperty(p_skip, true);
		}
	}
}
