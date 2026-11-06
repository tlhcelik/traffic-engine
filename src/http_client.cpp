#include "http_client.h"
#include "logger.h"

#include <QElapsedTimer>
#include <QUrl>

#include <algorithm>
#include <string>

#include <windows.h>
#include <winhttp.h>

#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_DECOMPRESSION
#define WINHTTP_OPTION_DECOMPRESSION 118
#define WINHTTP_DECOMPRESSION_FLAG_ALL 0x00000003
#endif

// Access to the private handle list of http_cancel (only for request()).
struct http_cancel_access
{
	// Adds the handle. Returns false if the token is already cancelled.
	static bool add(http_cancel &c, HINTERNET h)
	{
		std::lock_guard<std::mutex> lock(c.m_mutex);
		if (c.m_cancelled)
		{
			return false;
		}
		c.m_handles.push_back(h);
		return true;
	}

	// Removes the handle. Returns true if the caller must close it (cancel()
	// did not close it).
	static bool remove(http_cancel &c, HINTERNET h)
	{
		std::lock_guard<std::mutex> lock(c.m_mutex);
		auto it = std::find(c.m_handles.begin(), c.m_handles.end(), h);
		if (it == c.m_handles.end())
		{
			return false;
		}
		c.m_handles.erase(it);
		return true;
	}
};

void http_cancel::cancel()
{
	std::lock_guard<std::mutex> lock(m_mutex);
	m_cancelled = true;
	// Closing a request handle stops a blocking WinHTTP call in another thread
	// (ERROR_WINHTTP_OPERATION_CANCELLED).
	for (void *h : m_handles)
	{
		WinHttpCloseHandle(static_cast<HINTERNET>(h));
	}
	m_handles.clear();
}

bool http_cancel::cancelled() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	return m_cancelled;
}

namespace
{
	QString win_error(DWORD code)
	{
		wchar_t *buf = nullptr;
		DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_HMODULE | FORMAT_MESSAGE_IGNORE_INSERTS,
			GetModuleHandleW(L"winhttp.dll"), code, 0, reinterpret_cast<wchar_t *>(&buf), 0, nullptr);
		if (n == 0)
		{
			n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
				nullptr, code, 0, reinterpret_cast<wchar_t *>(&buf), 0, nullptr);
		}
		QString text = n > 0 ? QString::fromWCharArray(buf, static_cast<int>(n)).trimmed() : QString();
		if (buf)
		{
			LocalFree(buf);
		}
		return QStringLiteral("%1 (error %2)").arg(text.isEmpty() ? QStringLiteral("Network error") : text).arg(code);
	}

	struct handle_guard
	{
		HINTERNET h = nullptr;
		~handle_guard()
		{
			if (h)
			{
				WinHttpCloseHandle(h);
			}
		}
	};
}

namespace http_client
{
	QByteArray form_encode(const QString &value)
	{
		return QUrl::toPercentEncoding(value);
	}

	http_response request(const QString &url, const QString &user_agent, const QByteArray &post_data,
		const QString &content_type, int timeout_ms, const std::shared_ptr<http_cancel> &cancel, qint64 max_bytes,
		std::atomic<qint64> *received)
	{
		http_response out;
		QElapsedTimer timer;
		timer.start();
		if (cancel && cancel->cancelled())
		{
			out.cancelled = true;
			out.error = QStringLiteral("cancelled");
			return out;
		}

		const std::wstring wurl = url.toStdWString();
		URL_COMPONENTS uc;
		ZeroMemory(&uc, sizeof(uc));
		uc.dwStructSize = sizeof(uc);
		// Length -1 and no buffer: the pointers point into wurl.
		uc.dwSchemeLength = static_cast<DWORD>(-1);
		uc.dwHostNameLength = static_cast<DWORD>(-1);
		uc.dwUrlPathLength = static_cast<DWORD>(-1);
		uc.dwExtraInfoLength = static_cast<DWORD>(-1);
		if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.size()), 0, &uc))
		{
			out.error = QStringLiteral("Incorrect address: %1").arg(url);
			return out;
		}
		const std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
		std::wstring object(uc.lpszUrlPath, uc.dwUrlPathLength);
		object.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
		if (object.empty())
		{
			object = L"/";
		}
		const bool secure = (uc.nScheme == INTERNET_SCHEME_HTTPS);
		const std::wstring agent = user_agent.toStdWString();

		handle_guard session;
		session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session.h)
		{
			session.h = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		}
		if (!session.h)
		{
			out.error = win_error(GetLastError());
			return out;
		}
		WinHttpSetTimeouts(session.h, timeout_ms, timeout_ms, timeout_ms, timeout_ms);
		DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
#ifdef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
		protocols |= WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;
#endif
		if (!WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols)))
		{
			protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
			WinHttpSetOption(session.h, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof(protocols));
		}
		DWORD decompression = WINHTTP_DECOMPRESSION_FLAG_ALL;
		WinHttpSetOption(session.h, WINHTTP_OPTION_DECOMPRESSION, &decompression, sizeof(decompression));

		handle_guard connection;
		connection.h = WinHttpConnect(session.h, host.c_str(), uc.nPort, 0);
		if (!connection.h)
		{
			out.error = win_error(GetLastError());
			return out;
		}
		const bool post = !post_data.isEmpty();
		HINTERNET req = WinHttpOpenRequest(connection.h, post ? L"POST" : L"GET", object.c_str(), nullptr,
			WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
		if (!req)
		{
			out.error = win_error(GetLastError());
			return out;
		}
		// The token can close the request handle (cancel). Register it.
		if (cancel && !http_cancel_access::add(*cancel, req))
		{
			WinHttpCloseHandle(req);
			out.cancelled = true;
			out.error = QStringLiteral("cancelled");
			return out;
		}
		auto close_request = [&cancel, req]()
		{
			if (!cancel || http_cancel_access::remove(*cancel, req))
			{
				WinHttpCloseHandle(req);
			}
		};

		std::wstring headers;
		if (post)
		{
			headers = L"Content-Type: " + (content_type.isEmpty() ? std::wstring(L"application/x-www-form-urlencoded") : content_type.toStdWString()) + L"\r\n";
		}
		bool ok = WinHttpSendRequest(req, headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : headers.c_str(),
			headers.empty() ? 0 : static_cast<DWORD>(-1L),
			post ? const_cast<char *>(post_data.constData()) : WINHTTP_NO_REQUEST_DATA,
			static_cast<DWORD>(post_data.size()), static_cast<DWORD>(post_data.size()), 0) != FALSE;
		ok = ok && WinHttpReceiveResponse(req, nullptr) != FALSE;
		if (!ok)
		{
			const DWORD code = GetLastError();
			close_request();
			if (cancel && cancel->cancelled())
			{
				out.cancelled = true;
				out.error = QStringLiteral("cancelled");
			}
			else
			{
				out.error = win_error(code);
			}
			return out;
		}

		DWORD status = 0;
		DWORD size = sizeof(status);
		WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
			&status, &size, WINHTTP_NO_HEADER_INDEX);
		out.status = static_cast<int>(status);
		wchar_t ctype[256];
		DWORD ctype_size = sizeof(ctype);
		if (WinHttpQueryHeaders(req, WINHTTP_QUERY_CONTENT_TYPE, WINHTTP_HEADER_NAME_BY_INDEX, ctype, &ctype_size, WINHTTP_NO_HEADER_INDEX))
		{
			out.content_type = QString::fromWCharArray(ctype, static_cast<int>(ctype_size / sizeof(wchar_t)));
		}

		for (;;)
		{
			DWORD avail = 0;
			if (!WinHttpQueryDataAvailable(req, &avail))
			{
				const DWORD code = GetLastError();
				if (cancel && cancel->cancelled())
				{
					out.cancelled = true;
					out.error = QStringLiteral("cancelled");
				}
				else
				{
					out.error = win_error(code);
				}
				break;
			}
			if (avail == 0)
			{
				break;
			}
			const int old = out.body.size();
			if (old + static_cast<qint64>(avail) > max_bytes)
			{
				out.error = QStringLiteral("The answer is too large (more than %1 MB).").arg(max_bytes / (1024 * 1024));
				break;
			}
			out.body.resize(old + static_cast<int>(avail));
			DWORD read = 0;
			if (!WinHttpReadData(req, out.body.data() + old, avail, &read))
			{
				out.body.resize(old);
				out.error = (cancel && cancel->cancelled()) ? QStringLiteral("cancelled") : win_error(GetLastError());
				out.cancelled = cancel && cancel->cancelled();
				break;
			}
			out.body.resize(old + static_cast<int>(read));
			if (received)
			{
				received->store(out.body.size());
			}
		}
		close_request();
		TE_DEBUG("NET", QStringLiteral("%1 %2: status %3, %4 bytes, %5 ms%6").arg(post ? QStringLiteral("POST") : QStringLiteral("GET"))
			.arg(url.left(160)).arg(out.status).arg(out.body.size()).arg(timer.elapsed())
			.arg(out.error.isEmpty() ? QString() : QStringLiteral(", ") + out.error));
		if (!out.error.isEmpty() && out.status >= 200 && out.status < 300)
		{
			out.status = 0; // incomplete body: not a success
		}
		return out;
	}
}
