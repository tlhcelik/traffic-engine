#include "xml_doc.h"

#include <QFile>
#include <QSaveFile>

#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/XMLUni.hpp>
#include <xercesc/util/XMLException.hpp>
#include <xercesc/dom/DOM.hpp>
#include <xercesc/parsers/XercesDOMParser.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/framework/MemBufFormatTarget.hpp>
#include <xercesc/sax/HandlerBase.hpp>
#include <xercesc/sax/SAXParseException.hpp>

XERCES_CPP_NAMESPACE_USE

static_assert(sizeof(XMLCh) == sizeof(ushort), "XMLCh must be a 16-bit type (UTF-16).");

namespace
{
	// QString -> XMLCh*. The pointer is valid while the QString lives.
	inline const XMLCh *xc(const QString &s)
	{
		return reinterpret_cast<const XMLCh *>(s.utf16());
	}

	inline QString qs(const XMLCh *x)
	{
		if (!x)
		{
			return QString();
		}
		return QString::fromUtf16(reinterpret_cast<const ushort *>(x));
	}

	// Keeps the first parse error. Xerces calls it during parse().
	class error_collector : public HandlerBase
	{
	public:
		void warning(const SAXParseException &) override {}
		void error(const SAXParseException &e) override { keep(e); }
		void fatalError(const SAXParseException &e) override { keep(e); }

		QString message;

	private:
		void keep(const SAXParseException &e)
		{
			if (!message.isEmpty())
			{
				return;
			}
			message = QStringLiteral("line %1, column %2: %3")
				.arg(e.getLineNumber()).arg(e.getColumnNumber()).arg(qs(e.getMessage()));
		}
	};

	// Removes the text nodes that contain only white space between elements.
	// The serializer indents the output again; the old indentation would make
	// the file larger at each save.
	void strip_whitespace(DOMNode *node)
	{
		bool has_element_child = false;
		for (DOMNode *c = node->getFirstChild(); c; c = c->getNextSibling())
		{
			if (c->getNodeType() == DOMNode::ELEMENT_NODE)
			{
				has_element_child = true;
				break;
			}
		}
		DOMNode *child = node->getFirstChild();
		while (child)
		{
			DOMNode *next = child->getNextSibling();
			if (child->getNodeType() == DOMNode::ELEMENT_NODE)
			{
				strip_whitespace(child);
			}
			else if (has_element_child && child->getNodeType() == DOMNode::TEXT_NODE
				&& qs(child->getNodeValue()).trimmed().isEmpty())
			{
				node->removeChild(child)->release();
			}
			child = next;
		}
	}

	bool name_matches(const DOMNode *n, const QString &name)
	{
		if (n->getNodeType() != DOMNode::ELEMENT_NODE)
		{
			return false;
		}
		if (name.isEmpty())
		{
			return true;
		}
		return XMLString::equals(static_cast<const DOMElement *>(n)->getTagName(), xc(name));
	}
}

// ---------------------------------------------------------------------------
// xml_platform
// ---------------------------------------------------------------------------
xml_platform::xml_platform()
	: m_ok(false)
{
	try
	{
		XMLPlatformUtils::Initialize();
		m_ok = true;
	}
	catch (const XMLException &e)
	{
		m_error = qs(e.getMessage());
	}
}

xml_platform::~xml_platform()
{
	if (m_ok)
	{
		XMLPlatformUtils::Terminate();
	}
}

// ---------------------------------------------------------------------------
// xml_element
// ---------------------------------------------------------------------------
QString xml_element::name() const
{
	return m_e ? qs(m_e->getTagName()) : QString();
}

bool xml_element::has_attribute(const QString &name) const
{
	return m_e && m_e->hasAttribute(xc(name));
}

QString xml_element::attribute(const QString &name, const QString &default_value) const
{
	if (!has_attribute(name))
	{
		return default_value;
	}
	return qs(m_e->getAttribute(xc(name)));
}

int xml_element::attribute_int(const QString &name, int default_value) const
{
	bool ok = false;
	const int v = attribute(name).trimmed().toInt(&ok);
	return ok ? v : default_value;
}

double xml_element::attribute_double(const QString &name, double default_value) const
{
	bool ok = false;
	const double v = attribute(name).trimmed().toDouble(&ok);
	return ok ? v : default_value;
}

bool xml_element::attribute_bool(const QString &name, bool default_value) const
{
	const QString v = attribute(name).trimmed().toLower();
	if (v == QLatin1String("1") || v == QLatin1String("true") || v == QLatin1String("yes"))
	{
		return true;
	}
	if (v == QLatin1String("0") || v == QLatin1String("false") || v == QLatin1String("no"))
	{
		return false;
	}
	return default_value;
}

void xml_element::set_attribute(const QString &name, const QString &value)
{
	if (m_e)
	{
		m_e->setAttribute(xc(name), xc(value));
	}
}

void xml_element::set_attribute(const QString &name, const char *value)
{
	set_attribute(name, QString::fromUtf8(value));
}

void xml_element::set_attribute(const QString &name, int value)
{
	set_attribute(name, QString::number(value));
}

void xml_element::set_attribute(const QString &name, double value)
{
	set_attribute(name, QString::number(value, 'g', 12));
}

void xml_element::set_attribute(const QString &name, bool value)
{
	set_attribute(name, value ? QStringLiteral("true") : QStringLiteral("false"));
}

QString xml_element::text() const
{
	return m_e ? qs(m_e->getTextContent()) : QString();
}

void xml_element::set_text(const QString &value)
{
	if (m_e)
	{
		m_e->setTextContent(xc(value));
	}
}

xml_element xml_element::first_child(const QString &name) const
{
	if (!m_e)
	{
		return xml_element();
	}
	for (DOMNode *n = m_e->getFirstChild(); n; n = n->getNextSibling())
	{
		if (name_matches(n, name))
		{
			return xml_element(static_cast<DOMElement *>(n));
		}
	}
	return xml_element();
}

xml_element xml_element::next_sibling(const QString &name) const
{
	if (!m_e)
	{
		return xml_element();
	}
	for (DOMNode *n = m_e->getNextSibling(); n; n = n->getNextSibling())
	{
		if (name_matches(n, name))
		{
			return xml_element(static_cast<DOMElement *>(n));
		}
	}
	return xml_element();
}

QList<xml_element> xml_element::children(const QString &name) const
{
	QList<xml_element> list;
	for (xml_element c = first_child(name); !c.is_null(); c = c.next_sibling(name))
	{
		list.append(c);
	}
	return list;
}

xml_element xml_element::add_child(const QString &name)
{
	if (!m_e)
	{
		return xml_element();
	}
	DOMElement *c = m_e->getOwnerDocument()->createElement(xc(name));
	m_e->appendChild(c);
	return xml_element(c);
}

void xml_element::remove_children(const QString &name)
{
	if (!m_e)
	{
		return;
	}
	DOMNode *n = m_e->getFirstChild();
	while (n)
	{
		DOMNode *next = n->getNextSibling();
		if (name_matches(n, name))
		{
			m_e->removeChild(n)->release();
		}
		n = next;
	}
}

QString xml_element::child_text(const QString &name, const QString &default_value) const
{
	const xml_element c = first_child(name);
	return c.is_null() ? default_value : c.text();
}

void xml_element::set_child_text(const QString &name, const QString &value)
{
	xml_element c = first_child(name);
	if (c.is_null())
	{
		c = add_child(name);
	}
	c.set_text(value);
}

// ---------------------------------------------------------------------------
// xml_document
// ---------------------------------------------------------------------------
xml_document::xml_document()
	: m_doc(nullptr)
{
}

xml_document::~xml_document()
{
	release();
}

void xml_document::release()
{
	if (m_doc)
	{
		m_doc->release();
		m_doc = nullptr;
	}
}

bool xml_document::create(const QString &root_name)
{
	release();
	try
	{
		const QString features = QStringLiteral("Core");
		DOMImplementation *impl = DOMImplementationRegistry::getDOMImplementation(xc(features));
		if (!impl)
		{
			return false;
		}
		m_doc = impl->createDocument(nullptr, xc(root_name), nullptr);
	}
	catch (const DOMException &)
	{
		m_doc = nullptr;
	}
	catch (const XMLException &)
	{
		m_doc = nullptr;
	}
	return m_doc != nullptr;
}

bool xml_document::load(const QString &path, QString *error)
{
	QFile f(path);
	if (!f.open(QIODevice::ReadOnly))
	{
		if (error)
		{
			*error = QStringLiteral("Cannot open the file %1: %2").arg(path, f.errorString());
		}
		return false;
	}
	const QByteArray data = f.readAll();
	return load_from_memory(data, path, error);
}

bool xml_document::load_from_memory(const QByteArray &data, const QString &source_name, QString *error)
{
	release();

	XercesDOMParser parser;
	parser.setValidationScheme(XercesDOMParser::Val_Never);
	parser.setDoNamespaces(false);
	parser.setDoSchema(false);
	parser.setLoadExternalDTD(false);
	parser.setCreateEntityReferenceNodes(false);
	// Security: do not read external entities.
	parser.setDisableDefaultEntityResolution(true);

	error_collector handler;
	parser.setErrorHandler(&handler);

	const QByteArray id = source_name.toUtf8();
	MemBufInputSource src(reinterpret_cast<const XMLByte *>(data.constData()),
		static_cast<XMLSize_t>(data.size()), id.constData(), false);

	QString fail;
	try
	{
		parser.parse(src);
	}
	catch (const XMLException &e)
	{
		fail = qs(e.getMessage());
	}
	catch (const DOMException &e)
	{
		fail = qs(e.getMessage());
	}
	catch (...)
	{
		fail = QStringLiteral("unknown parser error");
	}
	if (fail.isEmpty() && (parser.getErrorCount() > 0 || !handler.message.isEmpty()))
	{
		fail = handler.message.isEmpty() ? QStringLiteral("the XML is not valid") : handler.message;
	}
	if (!fail.isEmpty())
	{
		if (error)
		{
			*error = QStringLiteral("XML error in %1: %2").arg(source_name, fail);
		}
		return false;
	}

	m_doc = parser.adoptDocument();
	if (!m_doc || !m_doc->getDocumentElement())
	{
		release();
		if (error)
		{
			*error = QStringLiteral("XML error in %1: the document has no root element").arg(source_name);
		}
		return false;
	}
	strip_whitespace(m_doc->getDocumentElement());
	return true;
}

QByteArray xml_document::to_bytes() const
{
	if (!m_doc)
	{
		return QByteArray();
	}
	QByteArray result;
	DOMLSSerializer *ser = nullptr;
	DOMLSOutput *out = nullptr;
	try
	{
		const QString features = QStringLiteral("LS");
		DOMImplementation *impl = DOMImplementationRegistry::getDOMImplementation(xc(features));
		if (!impl)
		{
			return QByteArray();
		}
		ser = impl->createLSSerializer();
		DOMConfiguration *cfg = ser->getDomConfig();
		if (cfg->canSetParameter(XMLUni::fgDOMWRTFormatPrettyPrint, true))
		{
			cfg->setParameter(XMLUni::fgDOMWRTFormatPrettyPrint, true);
		}
		out = impl->createLSOutput();
		MemBufFormatTarget target;
		out->setByteStream(&target);
		// DOMLSOutput keeps the pointer: the string must live until write() ends.
		const QString encoding = QStringLiteral("UTF-8");
		out->setEncoding(xc(encoding));
		ser->write(m_doc, out);
		result = QByteArray(reinterpret_cast<const char *>(target.getRawBuffer()), static_cast<int>(target.getLen()));
	}
	catch (const DOMException &)
	{
		result.clear();
	}
	catch (const XMLException &)
	{
		result.clear();
	}
	if (out)
	{
		out->release();
	}
	if (ser)
	{
		ser->release();
	}
	return result;
}

bool xml_document::save(const QString &path, QString *error) const
{
	const QByteArray bytes = to_bytes();
	if (bytes.isEmpty())
	{
		if (error)
		{
			*error = QStringLiteral("Cannot write the XML document.");
		}
		return false;
	}
	QSaveFile f(path);
	if (!f.open(QIODevice::WriteOnly))
	{
		if (error)
		{
			*error = QStringLiteral("Cannot open the file %1: %2").arg(path, f.errorString());
		}
		return false;
	}
	f.write(bytes);
	if (!f.commit())
	{
		if (error)
		{
			*error = QStringLiteral("Cannot save the file %1: %2").arg(path, f.errorString());
		}
		return false;
	}
	return true;
}

xml_element xml_document::root() const
{
	return m_doc ? xml_element(m_doc->getDocumentElement()) : xml_element();
}
