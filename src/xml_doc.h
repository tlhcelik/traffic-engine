#ifndef XML_DOC_H
#define XML_DOC_H

#include <QString>
#include <QList>
#include <QByteArray>

#include <xercesc/util/XercesDefs.hpp>

XERCES_CPP_NAMESPACE_BEGIN
class DOMElement;
class DOMDocument;
XERCES_CPP_NAMESPACE_END

// xml_doc
// -------
// A small wrapper around the Xerces-C DOM. All XML files of the program
// (config.xml, .te workspace files, the sign catalog) use it.
//
// xml_platform: starts Xerces-C one time (XMLPlatformUtils::Initialize) and
// stops it when the instance is deleted. Make one instance at the start of
// main(), before the first XML operation.
//
// xml_element: a light handle (a pointer) to a DOM element. The document owns
// the element. Do not keep a handle after the document is deleted.
//
// xml_document: makes, loads and saves a DOM document. The save is atomic
// (QSaveFile): a crash during the save does not damage the old file.
//
// Example:
//   xml_document doc;
//   doc.create("config");
//   xml_element e = doc.root().add_child("logging");
//   e.set_attribute("level", "info");
//   doc.save(path, &error);

class xml_platform
{
public:
	xml_platform();
	~xml_platform();
	bool ok() const { return m_ok; }
	QString error() const { return m_error; }

private:
	xml_platform(const xml_platform &) = delete;
	xml_platform &operator=(const xml_platform &) = delete;

	bool m_ok;
	QString m_error;
};

class xml_element
{
public:
	xml_element() : m_e(nullptr) {}
	explicit xml_element(XERCES_CPP_NAMESPACE::DOMElement *e) : m_e(e) {}

	bool is_null() const { return m_e == nullptr; }
	QString name() const;

	bool has_attribute(const QString &name) const;
	QString attribute(const QString &name, const QString &default_value = QString()) const;
	int attribute_int(const QString &name, int default_value = 0) const;
	double attribute_double(const QString &name, double default_value = 0.0) const;
	bool attribute_bool(const QString &name, bool default_value = false) const;

	void set_attribute(const QString &name, const QString &value);
	void set_attribute(const QString &name, const char *value);
	void set_attribute(const QString &name, int value);
	void set_attribute(const QString &name, double value);
	void set_attribute(const QString &name, bool value);

	QString text() const;
	void set_text(const QString &value);

	// Child elements. An empty name matches all elements.
	xml_element first_child(const QString &name = QString()) const;
	xml_element next_sibling(const QString &name = QString()) const;
	QList<xml_element> children(const QString &name = QString()) const;
	xml_element add_child(const QString &name);
	void remove_children(const QString &name);

	// Text of the first child element with this name.
	QString child_text(const QString &name, const QString &default_value = QString()) const;
	// Sets the text of the first child element with this name (adds it if necessary).
	void set_child_text(const QString &name, const QString &value);

	XERCES_CPP_NAMESPACE::DOMElement *native() const { return m_e; }

private:
	XERCES_CPP_NAMESPACE::DOMElement *m_e;
};

class xml_document
{
public:
	xml_document();
	~xml_document();

	// Makes a new empty document with this root element.
	bool create(const QString &root_name);
	// Loads a file. On an error, the function returns false and sets *error.
	bool load(const QString &path, QString *error = nullptr);
	bool load_from_memory(const QByteArray &data, const QString &source_name, QString *error = nullptr);
	// Saves the document (UTF-8, indented). The write is atomic.
	bool save(const QString &path, QString *error = nullptr) const;
	QByteArray to_bytes() const;

	bool is_null() const { return m_doc == nullptr; }
	xml_element root() const;

private:
	xml_document(const xml_document &) = delete;
	xml_document &operator=(const xml_document &) = delete;

	void release();

	XERCES_CPP_NAMESPACE::DOMDocument *m_doc;
};

#endif // XML_DOC_H
