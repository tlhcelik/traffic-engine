#include "logger.h"
#include "app_paths.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>

#include <chrono>
#include <cstdio>
#include <cstdlib>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

std::atomic<int> logger::s_effective_level(logger::level_info);

namespace
{
	const char *level_tag(int lvl)
	{
		switch (lvl)
		{
			case logger::level_trace: return "TRACE";
			case logger::level_debug: return "DEBUG";
			case logger::level_info: return "INFO ";
			case logger::level_warn: return "WARN ";
			case logger::level_error: return "ERROR";
			case logger::level_fatal: return "FATAL";
			default: return "INFO ";
		}
	}

	HANDLE as_handle(void *p)
	{
		return static_cast<HANDLE>(p);
	}

	void qt_message_handler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
	{
		logger::level lvl = logger::level_debug;
		switch (type)
		{
			case QtDebugMsg: lvl = logger::level_debug; break;
			case QtInfoMsg: lvl = logger::level_info; break;
			case QtWarningMsg: lvl = logger::level_warn; break;
			case QtCriticalMsg: lvl = logger::level_error; break;
			case QtFatalMsg: lvl = logger::level_fatal; break;
		}
		if (!logger::is_on(lvl) && type != QtFatalMsg)
		{
			return;
		}
		QString text = msg;
		if (ctx.file && logger::instance().sync_mode())
		{
			text += QStringLiteral(" (%1:%2)").arg(QString::fromLatin1(ctx.file)).arg(ctx.line);
		}
		logger::instance().write(lvl, "QT", text);
		if (type == QtFatalMsg)
		{
			logger::instance().flush();
			std::abort(); // the crash handler writes the dump
		}
	}
}

logger::logger()
	: m_enabled(true)
	, m_level(level_info)
	, m_sync(false)
	, m_echo(false)
	, m_guard(true)
	, m_guard_rate(20000)
	, m_max_queue(200000)
	, m_stop(false)
	, m_running(false)
	, m_part(1)
	, m_max_file_size(100LL * 1024LL * 1024LL)
	, m_file(nullptr)
	, m_file_size(0)
	, m_written(0)
	, m_dropped(0)
	, m_lines_this_second(0)
	, m_guard_window_start(QDateTime::currentMSecsSinceEpoch())
	, m_dropped_at_last_check(0)
{
}

logger::~logger()
{
	stop();
	std::lock_guard<std::mutex> lk(m_file_mutex);
	if (m_file)
	{
		CloseHandle(as_handle(m_file));
		m_file = nullptr;
	}
}

logger &logger::instance()
{
	static logger inst;
	return inst;
}

void logger::start(const QString &log_dir)
{
	{
		std::lock_guard<std::mutex> lk(m_file_mutex);
		if (m_file)
		{
			CloseHandle(as_handle(m_file));
			m_file = nullptr;
		}
		m_log_dir = log_dir;
		m_open_date.clear();
		QDir().mkpath(m_log_dir);
	}
	std::lock_guard<std::mutex> lk(m_queue_mutex);
	if (m_running)
	{
		return;
	}
	m_stop = false;
	m_running = true;
	m_thread = std::thread([this]() { thread_main(); });
}

void logger::stop()
{
	{
		std::lock_guard<std::mutex> lk(m_queue_mutex);
		if (!m_running)
		{
			return;
		}
		m_stop = true;
	}
	m_cv.notify_one();
	if (m_thread.joinable())
	{
		m_thread.join();
	}
	std::lock_guard<std::mutex> lk(m_queue_mutex);
	m_running = false;
}

void logger::thread_main()
{
	std::vector<record> batch;
	for (;;)
	{
		{
			std::unique_lock<std::mutex> lk(m_queue_mutex);
			m_cv.wait_for(lk, std::chrono::milliseconds(200), [this]() { return m_stop || m_queue.size() >= 256; });
			batch.swap(m_queue);
			if (m_stop && batch.empty())
			{
				break;
			}
		}
		if (!batch.empty())
		{
			write_records(batch);
			batch.clear();
		}
		check_guard();
	}
}

void logger::update_effective_level()
{
	s_effective_level.store(m_enabled.load() ? m_level.load() : static_cast<int>(level_off));
}

void logger::set_enabled(bool on)
{
	m_enabled = on;
	update_effective_level();
}

bool logger::enabled() const
{
	return m_enabled.load();
}

void logger::set_level(level lvl)
{
	m_level = static_cast<int>(lvl);
	update_effective_level();
}

logger::level logger::min_level() const
{
	return static_cast<level>(m_level.load());
}

void logger::set_sync_mode(bool on)
{
	if (on)
	{
		flush();
	}
	m_sync = on;
}

bool logger::sync_mode() const
{
	return m_sync.load();
}

void logger::set_auto_guard(bool on, int lines_per_second)
{
	m_guard = on;
	m_guard_rate = qMax(1000, lines_per_second);
}

void logger::set_max_file_size(qint64 bytes)
{
	std::lock_guard<std::mutex> lk(m_file_mutex);
	if (bytes > 1024)
	{
		m_max_file_size = bytes;
	}
}

void logger::set_console_echo(bool on)
{
	m_echo = on;
}

QString logger::log_dir() const
{
	std::lock_guard<std::mutex> lk(m_file_mutex);
	return m_log_dir;
}

QString logger::current_file() const
{
	std::lock_guard<std::mutex> lk(m_file_mutex);
	return m_file_path;
}

int logger::queue_size() const
{
	std::lock_guard<std::mutex> lk(m_queue_mutex);
	return static_cast<int>(m_queue.size());
}

void logger::write(level lvl, const char *category, const QString &message)
{
	if (!is_on(lvl))
	{
		return;
	}
	record r;
	r.ms = QDateTime::currentMSecsSinceEpoch();
	r.thread_id = static_cast<quint32>(GetCurrentThreadId());
	r.lvl = static_cast<int>(lvl);
	r.category = category ? category : "APP";
	r.message = message;
	m_lines_this_second.fetch_add(1, std::memory_order_relaxed);

	bool direct = m_sync.load(std::memory_order_relaxed);
	bool notify = false;
	if (!direct)
	{
		std::lock_guard<std::mutex> lk(m_queue_mutex);
		if (!m_running)
		{
			direct = true; // no writer thread yet (or after stop): write now
		}
		else if (m_queue.size() >= m_max_queue)
		{
			m_dropped.fetch_add(1);
			return;
		}
		else
		{
			m_queue.push_back(std::move(r));
			notify = (m_queue.size() == 256);
		}
	}
	if (direct)
	{
		std::vector<record> one;
		one.push_back(std::move(r));
		write_records(one);
		return;
	}
	if (notify)
	{
		m_cv.notify_one();
	}
}

void logger::action(const QString &message)
{
	TE_INFO("ACTION", message);
}

void logger::file_event(const QString &what, const QString &path)
{
	TE_INFO("FILE", QStringLiteral("%1 | %2").arg(what, path));
}

void logger::settings_change(const QString &key, const QString &old_value, const QString &new_value)
{
	TE_INFO("SETTINGS", QStringLiteral("%1: '%2' -> '%3'").arg(key, old_value, new_value));
}

void logger::info(const QString &message)
{
	TE_INFO("APP", message);
}

void logger::warn(const QString &message)
{
	TE_WARN("APP", message);
}

void logger::error(const QString &message)
{
	TE_ERROR("APP", message);
}

void logger::flush()
{
	std::vector<record> batch;
	{
		std::lock_guard<std::mutex> lk(m_queue_mutex);
		batch.swap(m_queue);
	}
	if (!batch.empty())
	{
		write_records(batch);
	}
	std::lock_guard<std::mutex> lk(m_file_mutex);
	if (m_file)
	{
		FlushFileBuffers(as_handle(m_file));
	}
}

void logger::emergency_flush()
{
	std::unique_lock<std::mutex> q(m_queue_mutex, std::try_to_lock);
	if (!q.owns_lock())
	{
		return;
	}
	std::vector<record> batch;
	batch.swap(m_queue);
	q.unlock();

	std::unique_lock<std::mutex> f(m_file_mutex, std::try_to_lock);
	if (!f.owns_lock() || !m_file)
	{
		return;
	}
	for (const record &r : batch)
	{
		const QByteArray line = format_record(r);
		DWORD n = 0;
		WriteFile(as_handle(m_file), line.constData(), static_cast<DWORD>(line.size()), &n, nullptr);
	}
	FlushFileBuffers(as_handle(m_file));
}

QByteArray logger::format_record(const record &r) const
{
	QString msg = r.message;
	msg.replace(QLatin1Char('\n'), QStringLiteral(" | ")).remove(QLatin1Char('\r'));
	const QString line = QStringLiteral("%1 [%2] [%3] [T%4] %5\n")
		.arg(QDateTime::fromMSecsSinceEpoch(r.ms).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")))
		.arg(QLatin1String(level_tag(r.lvl)))
		.arg(QString::fromLatin1(r.category).leftJustified(8, QLatin1Char(' ')))
		.arg(r.thread_id, 5, 10, QLatin1Char('0'))
		.arg(msg);
	return line.toUtf8();
}

void logger::write_records(const std::vector<record> &records)
{
	QByteArray buf;
	buf.reserve(static_cast<int>(records.size()) * 96);
	for (const record &r : records)
	{
		buf += format_record(r);
	}

	std::lock_guard<std::mutex> lk(m_file_mutex);
	ensure_open();
	if (m_file)
	{
		DWORD n = 0;
		WriteFile(as_handle(m_file), buf.constData(), static_cast<DWORD>(buf.size()), &n, nullptr);
		m_file_size += n;
		rotate_if_needed();
	}
	if (m_echo.load())
	{
		std::fwrite(buf.constData(), 1, static_cast<size_t>(buf.size()), stderr);
		std::fflush(stderr);
	}
	m_written.fetch_add(records.size());
}

QString logger::make_file_path(const QString &date, int part) const
{
	const QString base = QStringLiteral("traffic_engine_%1").arg(date);
	if (part <= 1)
	{
		return QDir(m_log_dir).filePath(base + QStringLiteral(".log"));
	}
	return QDir(m_log_dir).filePath(QStringLiteral("%1.p%2.log").arg(base).arg(part));
}

void logger::ensure_open()
{
	const QString today = QDate::currentDate().toString(QStringLiteral("yyyy-MM-dd"));
	if (m_file && m_open_date == today)
	{
		return;
	}
	if (m_file)
	{
		CloseHandle(as_handle(m_file));
		m_file = nullptr;
	}
	if (m_log_dir.isEmpty())
	{
		m_log_dir = app_paths::log_dir();
	}
	QDir().mkpath(m_log_dir);

	// New day: find the last part that is not full (the program can start
	// more than one time on the same day).
	m_open_date = today;
	m_part = 1;
	for (int p = 1; p < 10000; ++p)
	{
		const QFileInfo fi(make_file_path(today, p));
		if (!fi.exists() || fi.size() < m_max_file_size)
		{
			m_part = p;
			break;
		}
	}
	m_file_path = make_file_path(today, m_part);
	const std::wstring w = QDir::toNativeSeparators(m_file_path).toStdWString();
	HANDLE h = CreateFileW(w.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h == INVALID_HANDLE_VALUE)
	{
		m_file = nullptr;
		return;
	}
	m_file = h;
	LARGE_INTEGER size;
	m_file_size = GetFileSizeEx(h, &size) ? size.QuadPart : 0;
}

void logger::rotate_if_needed()
{
	if (!m_file || m_file_size < m_max_file_size)
	{
		return;
	}
	CloseHandle(as_handle(m_file));
	m_file = nullptr;
	m_part++;
	m_file_path = make_file_path(m_open_date, m_part);
	const std::wstring w = QDir::toNativeSeparators(m_file_path).toStdWString();
	HANDLE h = CreateFileW(w.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
		nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (h != INVALID_HANDLE_VALUE)
	{
		m_file = h;
		m_file_size = 0;
	}
}

void logger::check_guard()
{
	const qint64 now = QDateTime::currentMSecsSinceEpoch();
	const qint64 span = now - m_guard_window_start;
	if (span < 1000)
	{
		return;
	}
	const quint64 lines = m_lines_this_second.exchange(0);
	const double per_second = static_cast<double>(lines) * 1000.0 / static_cast<double>(span);
	m_guard_window_start = now;
	const quint64 dropped = m_dropped.load();
	const bool overflow = (dropped != m_dropped_at_last_check);
	m_dropped_at_last_check = dropped;

	if (!m_guard.load() || m_level.load() >= level_warn)
	{
		return;
	}
	if (per_second > m_guard_rate.load() || overflow)
	{
		m_level = static_cast<int>(level_warn);
		update_effective_level();
		record r;
		r.ms = now;
		r.thread_id = static_cast<quint32>(GetCurrentThreadId());
		r.lvl = level_warn;
		r.category = "LOG";
		r.message = QStringLiteral("Logging slows the program (%1 lines/s, %2 lines lost). "
			"The log level is now WARN. You can change it in Settings.")
			.arg(qRound(per_second)).arg(dropped);
		std::vector<record> one;
		one.push_back(std::move(r));
		write_records(one);
		emit guard_triggered(static_cast<int>(level_warn));
	}
}

logger::level logger::level_from_string(const QString &text, level default_level)
{
	const QString t = text.trimmed().toLower();
	if (t == QLatin1String("trace")) { return level_trace; }
	if (t == QLatin1String("debug")) { return level_debug; }
	if (t == QLatin1String("info")) { return level_info; }
	if (t == QLatin1String("warn") || t == QLatin1String("warning")) { return level_warn; }
	if (t == QLatin1String("error")) { return level_error; }
	if (t == QLatin1String("fatal")) { return level_fatal; }
	if (t == QLatin1String("off")) { return level_off; }
	return default_level;
}

QString logger::level_name(level lvl)
{
	switch (lvl)
	{
		case level_trace: return QStringLiteral("trace");
		case level_debug: return QStringLiteral("debug");
		case level_info: return QStringLiteral("info");
		case level_warn: return QStringLiteral("warn");
		case level_error: return QStringLiteral("error");
		case level_fatal: return QStringLiteral("fatal");
		case level_off: return QStringLiteral("off");
	}
	return QStringLiteral("info");
}

void logger::install_qt_handler()
{
	qInstallMessageHandler(qt_message_handler);
}
