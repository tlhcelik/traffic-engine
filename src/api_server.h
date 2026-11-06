#ifndef API_SERVER_H
#define API_SERVER_H

#include <QByteArray>
#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>

class QTcpServer;
class QTcpSocket;
class workspace;

// api_server
// ----------
// The external API: HTTP/1.1 with JSON (docs/api.md). Other programs read
// the state of the simulation and send traffic light phases (external
// algorithm), the global traffic density and run / pause / speed commands.
//
//   GET  /api/v1/status                         program and workspaces
//   GET  /api/v1/workspaces/{w}                 simulation, statistics, lights
//   GET  /api/v1/workspaces/{w}/lights          states of the traffic lights
//   GET  /api/v1/workspaces/{w}/vehicles        positions, speeds, fuel ...
//   POST /api/v1/workspaces/{w}/lights/{id}/phase   {"phase": 2, "duration": 30}
//   POST /api/v1/workspaces/{w}/control         {"running": true, "speed": 4, "reset": false}
//   GET  /api/v1/density, POST /api/v1/density  {"vehicles": 120, "pedestrians": 100}
//   {w}: the tab index (0, 1, ...) or "current".
//
// Settings (config.xml, section api): enabled, port, listen_all (all network
// interfaces; default only 127.0.0.1), token (if not empty: header
// "Authorization: Bearer <token>" or "X-Api-Token: <token>").
// The server runs in the user interface thread: it reads the simulation
// between two frames, without locks. Each request is small.

class api_server : public QObject
{
	Q_OBJECT

public:
	explicit api_server(QObject *parent = nullptr);
	~api_server() override;

	// The workspaces of the main window (all tabs) and the current one.
	void set_sources(std::function<QList<workspace *>()> all, std::function<workspace *()> current);
	// Starts, restarts or stops the server with the settings (config.xml).
	void apply_settings();
	// Starts the server on 127.0.0.1 for this session only (command line
	// --api-port); config.xml does not change.
	bool start_session(int port);
	bool running() const;
	// "http://127.0.0.1:8088" or an empty text.
	QString address() const;
	QString last_error() const { return m_error; }
	quint64 requests() const { return m_requests; }

signals:
	void state_changed();

private:
	void accept_clients();
	void read_client(QTcpSocket *socket);
	void handle(QTcpSocket *socket, const QByteArray &method, const QString &target,
		const QHash<QByteArray, QByteArray> &headers, const QByteArray &body);
	void reply(QTcpSocket *socket, int status, const QJsonObject &obj);
	workspace *find_workspace(const QString &id, QString *error) const;

	QTcpServer *m_server;
	QHash<QTcpSocket *, QByteArray> m_buffers;
	std::function<QList<workspace *>()> m_all;
	std::function<workspace *()> m_current;
	QString m_token;
	QString m_error;
	quint64 m_requests;
};

#endif // API_SERVER_H
