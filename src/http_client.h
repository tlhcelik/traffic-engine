#ifndef HTTP_CLIENT_H
#define HTTP_CLIENT_H

#include <QByteArray>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

// http_client
// -----------
// A small synchronous HTTP and HTTPS client with the Windows WinHTTP API.
// WinHTTP uses the TLS stack, the certificates and the proxy settings of
// Windows. Thus the program does not need OpenSSL DLLs (Qt 5.12 needs
// OpenSSL 1.1 for HTTPS).
//
// request() blocks until the response is complete. Call it in a worker
// thread (QtConcurrent, QThreadPool), never in the user interface thread.
//
// http_cancel: a token that stops the transfers that use it, from any
// thread (for example the Cancel button or the end of the program).

struct http_response
{
	int status = 0;            // HTTP status code (0: no connection)
	QByteArray body;
	QString content_type;
	QString error;             // connection error or "cancelled"
	bool cancelled = false;
	bool ok() const { return status >= 200 && status < 300 && !cancelled; }
};

class http_cancel
{
public:
	http_cancel() = default;
	// Stops all active transfers of this token. Later requests stop immediately.
	void cancel();
	bool cancelled() const;

private:
	friend struct http_cancel_access;
	http_cancel(const http_cancel &) = delete;
	http_cancel &operator=(const http_cancel &) = delete;

	mutable std::mutex m_mutex;
	bool m_cancelled = false;
	std::vector<void *> m_handles;   // active WinHTTP request handles
};

namespace http_client
{
	// GET (post_data empty) or POST. timeout_ms: for each phase (resolve,
	// connect, send, receive). max_bytes: a larger body is an error.
	// received: optional counter of the received bytes (for a progress display).
	http_response request(const QString &url, const QString &user_agent, const QByteArray &post_data = QByteArray(),
		const QString &content_type = QString(), int timeout_ms = 60000, const std::shared_ptr<http_cancel> &cancel = nullptr,
		qint64 max_bytes = 256LL * 1024 * 1024, std::atomic<qint64> *received = nullptr);

	// "application/x-www-form-urlencoded" encoding of one value.
	QByteArray form_encode(const QString &value);
}

#endif // HTTP_CLIENT_H
