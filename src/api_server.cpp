#include "api_server.h"
#include "config_common.h"
#include "json_export.h"
#include "logger.h"
#include "workspace.h"

#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>

namespace
{
	const int max_request_bytes = 1024 * 1024;

	QByteArray status_text(int status)
	{
		switch (status)
		{
			case 200: return QByteArrayLiteral("OK");
			case 400: return QByteArrayLiteral("Bad Request");
			case 401: return QByteArrayLiteral("Unauthorized");
			case 404: return QByteArrayLiteral("Not Found");
			case 405: return QByteArrayLiteral("Method Not Allowed");
			case 413: return QByteArrayLiteral("Payload Too Large");
			case 415: return QByteArrayLiteral("Unsupported Media Type");
			default: return QByteArrayLiteral("Internal Server Error");
		}
	}

	QJsonObject error_object(const QString &text)
	{
		QJsonObject o;
		o.insert(QStringLiteral("ok"), false);
		o.insert(QStringLiteral("error"), text);
		return o;
	}
}

api_server::api_server(QObject *parent)
	: QObject(parent)
	, m_server(new QTcpServer(this))
	, m_requests(0)
{
	connect(m_server, &QTcpServer::newConnection, this, &api_server::accept_clients);
}

api_server::~api_server()
{
	m_server->close();
}

void api_server::set_sources(std::function<QList<workspace *>()> all, std::function<workspace *()> current)
{
	m_all = std::move(all);
	m_current = std::move(current);
}

void api_server::apply_settings()
{
	const config_common &cfg = config_common::instance();
	const bool enabled = cfg.bool_value(QStringLiteral("api/enabled"));
	const int port = qBound(1, cfg.int_value(QStringLiteral("api/port")), 65535);
	const QHostAddress addr = cfg.bool_value(QStringLiteral("api/listen_all")) ? QHostAddress(QHostAddress::AnyIPv4) : QHostAddress(QHostAddress::LocalHost);
	m_token = cfg.value(QStringLiteral("api/token")).trimmed();
	if (m_server->isListening() && (!enabled || m_server->serverPort() != port || m_server->serverAddress() != addr))
	{
		m_server->close();
		TE_INFO("API", QStringLiteral("The API server stops."));
	}
	m_error.clear();
	if (enabled && !m_server->isListening())
	{
		if (m_server->listen(addr, static_cast<quint16>(port)))
		{
			TE_INFO("API", QStringLiteral("The API server listens at %1 (token: %2).").arg(address(), m_token.isEmpty() ? QStringLiteral("no") : QStringLiteral("yes")));
		}
		else
		{
			m_error = m_server->errorString();
			TE_ERROR("API", QStringLiteral("The API server cannot listen on port %1: %2").arg(port).arg(m_error));
		}
	}
	emit state_changed();
}

bool api_server::start_session(int port)
{
	m_server->close();
	m_token = config_common::instance().value(QStringLiteral("api/token")).trimmed();
	m_error.clear();
	const bool ok = m_server->listen(QHostAddress(QHostAddress::LocalHost), static_cast<quint16>(qBound(1, port, 65535)));
	if (ok)
	{
		TE_INFO("API", QStringLiteral("The API server listens at %1 (this session only).").arg(address()));
	}
	else
	{
		m_error = m_server->errorString();
		TE_ERROR("API", QStringLiteral("The API server cannot listen on port %1: %2").arg(port).arg(m_error));
	}
	emit state_changed();
	return ok;
}

bool api_server::running() const
{
	return m_server->isListening();
}

QString api_server::address() const
{
	if (!m_server->isListening())
	{
		return QString();
	}
	const QString host = m_server->serverAddress() == QHostAddress(QHostAddress::LocalHost) ? QStringLiteral("127.0.0.1") : QStringLiteral("0.0.0.0");
	return QStringLiteral("http://%1:%2").arg(host).arg(m_server->serverPort());
}

void api_server::accept_clients()
{
	while (QTcpSocket *s = m_server->nextPendingConnection())
	{
		m_buffers.insert(s, QByteArray());
		connect(s, &QTcpSocket::readyRead, this, [this, s]() { read_client(s); });
		connect(s, &QTcpSocket::disconnected, this, [this, s]()
		{
			m_buffers.remove(s);
			s->deleteLater();
		});
	}
}

void api_server::read_client(QTcpSocket *s)
{
	if (!m_buffers.contains(s))
	{
		return;
	}
	QByteArray &buf = m_buffers[s];
	buf += s->readAll();
	if (buf.size() > max_request_bytes)
	{
		buf.clear();
		reply(s, 413, error_object(QStringLiteral("The request is too large.")));
		return;
	}
	const int head_end = buf.indexOf("\r\n\r\n");
	if (head_end < 0)
	{
		return; // the header is not complete yet
	}
	const QList<QByteArray> lines = buf.left(head_end).split('\n');
	const QList<QByteArray> request = lines.value(0).trimmed().split(' ');
	if (request.size() < 2)
	{
		buf.clear();
		reply(s, 400, error_object(QStringLiteral("Incorrect request line.")));
		return;
	}
	QHash<QByteArray, QByteArray> headers;
	for (int i = 1; i < lines.size(); ++i)
	{
		const int colon = lines.at(i).indexOf(':');
		if (colon > 0)
		{
			headers.insert(lines.at(i).left(colon).trimmed().toLower(), lines.at(i).mid(colon + 1).trimmed());
		}
	}
	const int length = qMax(0, headers.value("content-length").toInt());
	if (buf.size() < head_end + 4 + length)
	{
		return; // the body is not complete yet
	}
	const QByteArray body = buf.mid(head_end + 4, length);
	buf.clear();
	++m_requests;
	handle(s, request.at(0).toUpper(), QString::fromUtf8(request.at(1)), headers, body);
}

workspace *api_server::find_workspace(const QString &id, QString *error) const
{
	const QList<workspace *> all = m_all ? m_all() : QList<workspace *>();
	if (id == QLatin1String("current") || id.isEmpty())
	{
		workspace *ws = m_current ? m_current() : nullptr;
		if (!ws)
		{
			*error = QStringLiteral("No workspace is open.");
		}
		return ws;
	}
	bool ok = false;
	const int index = id.toInt(&ok);
	if (!ok || index < 0 || index >= all.size())
	{
		*error = QStringLiteral("No workspace %1. Use an index from 0 to %2, or \"current\".").arg(id).arg(all.size() - 1);
		return nullptr;
	}
	return all.at(index);
}

void api_server::handle(QTcpSocket *s, const QByteArray &method, const QString &target,
	const QHash<QByteArray, QByteArray> &headers, const QByteArray &body)
{
	TE_DEBUG("API", QStringLiteral("%1 %2 (%3 bytes)").arg(QString::fromLatin1(method), target).arg(body.size()));
	if (!m_token.isEmpty())
	{
		const QByteArray expected = m_token.toUtf8();
		if (headers.value("authorization") != "Bearer " + expected && headers.value("x-api-token") != expected)
		{
			reply(s, 401, error_object(QStringLiteral("The token is missing or not correct.")));
			return;
		}
	}
	const QStringList parts = QUrl(target).path().split(QLatin1Char('/'), QString::SkipEmptyParts);
	if (parts.size() < 2 || parts.at(0) != QLatin1String("api") || parts.at(1) != QLatin1String("v1"))
	{
		reply(s, 404, error_object(QStringLiteral("Unknown path. Use /api/v1/... (see docs/api.md).")));
		return;
	}
	const bool get = (method == "GET");
	const bool post = (method == "POST" || method == "PUT");
	if (!get && !post)
	{
		reply(s, 405, error_object(QStringLiteral("Use GET or POST.")));
		return;
	}
	QJsonObject in;
	if (post)
	{
		// A JSON content type is necessary: a web page cannot send a "simple"
		// cross-site request to the local API.
		if (!headers.value("content-type").toLower().startsWith("application/json"))
		{
			reply(s, 415, error_object(QStringLiteral("Send the body with \"Content-Type: application/json\".")));
			return;
		}
		QJsonParseError pe;
		const QJsonDocument d = QJsonDocument::fromJson(body.isEmpty() ? QByteArrayLiteral("{}") : body, &pe);
		if (pe.error != QJsonParseError::NoError || !d.isObject())
		{
			reply(s, 400, error_object(QStringLiteral("The body is not a JSON object: %1").arg(pe.errorString())));
			return;
		}
		in = d.object();
	}

	const QString area = parts.value(2);
	auto workspace_list = [this]()
	{
		QJsonArray list;
		const QList<workspace *> all = m_all ? m_all() : QList<workspace *>();
		workspace *cur = m_current ? m_current() : nullptr;
		for (int i = 0; i < all.size(); ++i)
		{
			QJsonObject w = json_export::workspace_object(*all.at(i));
			w.insert(QStringLiteral("index"), i);
			w.insert(QStringLiteral("current"), all.at(i) == cur);
			w.insert(QStringLiteral("simulation"), json_export::simulation_object(*all.at(i)));
			list.append(w);
		}
		return list;
	};

	if (area == QLatin1String("status") && get)
	{
		QJsonObject o;
		o.insert(QStringLiteral("ok"), true);
		o.insert(QStringLiteral("program"), json_export::program_object());
		o.insert(QStringLiteral("workspaces"), workspace_list());
		reply(s, 200, o);
		return;
	}
	if (area == QLatin1String("density"))
	{
		config_common &cfg = config_common::instance();
		if (post)
		{
			if (in.contains(QStringLiteral("vehicles")))
			{
				cfg.set_int(QStringLiteral("simulation/traffic_density"), qBound(0, qRound(in.value(QStringLiteral("vehicles")).toDouble()), 300));
			}
			if (in.contains(QStringLiteral("pedestrians")))
			{
				cfg.set_int(QStringLiteral("simulation/pedestrian_density"), qBound(0, qRound(in.value(QStringLiteral("pedestrians")).toDouble()), 300));
			}
			cfg.save();
			logger::instance().action(QStringLiteral("API: traffic density %1%, pedestrians %2%")
				.arg(cfg.int_value(QStringLiteral("simulation/traffic_density"))).arg(cfg.int_value(QStringLiteral("simulation/pedestrian_density"))));
		}
		QJsonObject o;
		o.insert(QStringLiteral("ok"), true);
		o.insert(QStringLiteral("vehicles"), cfg.int_value(QStringLiteral("simulation/traffic_density")));
		o.insert(QStringLiteral("pedestrians"), cfg.int_value(QStringLiteral("simulation/pedestrian_density")));
		reply(s, 200, o);
		return;
	}
	if (area == QLatin1String("workspaces"))
	{
		if (parts.size() == 3 && get)
		{
			QJsonObject o;
			o.insert(QStringLiteral("ok"), true);
			o.insert(QStringLiteral("workspaces"), workspace_list());
			reply(s, 200, o);
			return;
		}
		QString error;
		workspace *ws = find_workspace(parts.value(3), &error);
		if (!ws)
		{
			reply(s, 404, error_object(error));
			return;
		}
		const QString item = parts.value(4);
		if (item.isEmpty() && get)
		{
			QJsonObject o;
			o.insert(QStringLiteral("ok"), true);
			o.insert(QStringLiteral("workspace"), json_export::workspace_object(*ws));
			o.insert(QStringLiteral("simulation"), json_export::simulation_object(*ws));
			o.insert(QStringLiteral("statistics"), json_export::statistics_object(*ws));
			o.insert(QStringLiteral("traffic_lights"), json_export::lights_array(*ws));
			o.insert(QStringLiteral("congestion"), json_export::congestion_object(*ws));
			reply(s, 200, o);
			return;
		}
		if (item == QLatin1String("lights") && parts.size() == 5 && get)
		{
			QJsonObject o;
			o.insert(QStringLiteral("ok"), true);
			o.insert(QStringLiteral("clock"), json_export::simulation_object(*ws).value(QStringLiteral("clock")));
			o.insert(QStringLiteral("traffic_lights"), json_export::lights_array(*ws));
			reply(s, 200, o);
			return;
		}
		if (item == QLatin1String("lights") && parts.size() == 7 && parts.at(6) == QLatin1String("phase") && post)
		{
			bool ok = false;
			const int id = parts.at(5).toInt(&ok);
			const light_data *ld = ok ? ws->model().light(id) : nullptr;
			if (!ld)
			{
				reply(s, 404, error_object(QStringLiteral("No traffic light %1 in this workspace.").arg(parts.at(5))));
				return;
			}
			const int phase = in.value(QStringLiteral("phase")).toInt(0);
			if (phase < 1 || phase > static_cast<int>(ld->phases.size()))
			{
				reply(s, 400, error_object(QStringLiteral("\"phase\" must be 1 to %1.").arg(ld->phases.size())));
				return;
			}
			const double duration = qBound(1.0, in.value(QStringLiteral("duration")).toDouble(30.0), 3600.0);
			ws->sim().set_external_phase(id, phase - 1, duration);
			logger::instance().action(QStringLiteral("API: light %1 phase %2 for %3 s").arg(id).arg(phase).arg(duration));
			QJsonObject o;
			o.insert(QStringLiteral("ok"), true);
			o.insert(QStringLiteral("light"), id);
			o.insert(QStringLiteral("phase"), phase);
			o.insert(QStringLiteral("duration_s"), duration);
			o.insert(QStringLiteral("algorithm"), light_algorithm_name(ld->algorithm));
			reply(s, 200, o);
			return;
		}
		if (item == QLatin1String("congestion") && parts.size() == 5 && get)
		{
			QJsonObject o = json_export::congestion_object(*ws);
			o.insert(QStringLiteral("ok"), true);
			o.insert(QStringLiteral("clock"), json_export::simulation_object(*ws).value(QStringLiteral("clock")));
			reply(s, 200, o);
			return;
		}
		if (item == QLatin1String("vehicles") && get)
		{
			const QJsonArray v = json_export::vehicles_array(*ws);
			QJsonObject o;
			o.insert(QStringLiteral("ok"), true);
			o.insert(QStringLiteral("count"), v.size());
			o.insert(QStringLiteral("vehicles"), v);
			reply(s, 200, o);
			return;
		}
		if (item == QLatin1String("control") && post)
		{
			if (in.contains(QStringLiteral("running")))
			{
				ws->set_running(in.value(QStringLiteral("running")).toBool());
			}
			if (in.contains(QStringLiteral("speed")))
			{
				ws->set_speed(in.value(QStringLiteral("speed")).toDouble(1.0));
			}
			if (in.value(QStringLiteral("reset")).toBool(false))
			{
				ws->reset_simulation();
			}
			QJsonObject o;
			o.insert(QStringLiteral("ok"), true);
			o.insert(QStringLiteral("simulation"), json_export::simulation_object(*ws));
			reply(s, 200, o);
			return;
		}
	}
	reply(s, 404, error_object(QStringLiteral("Unknown path or method: %1 %2 (see docs/api.md).").arg(QString::fromLatin1(method), target)));
}

void api_server::reply(QTcpSocket *s, int status, const QJsonObject &obj)
{
	const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
	QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + status_text(status) + "\r\n";
	head += "Content-Type: application/json; charset=utf-8\r\n";
	head += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
	head += "Cache-Control: no-store\r\n";
	head += "Connection: close\r\n\r\n";
	s->write(head);
	s->write(body);
	s->disconnectFromHost();
}
