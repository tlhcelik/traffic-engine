#ifndef LOGGER_H
#define LOGGER_H

#include <QObject>
#include <QString>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

// logger
// ------
// Thread-safe singleton log writer with low cost for the caller.
//
// Performance:
//  * The TE_LOG macros do a lock-free level check first. If the level is off,
//    the program does not make the message text (no cost).
//  * The caller only puts the line in a queue. A background thread formats
//    the lines and writes them to the file (one write for each batch).
//  * Auto guard: if the program makes too many log lines (more than
//    guard_lines_per_second) or the queue becomes full, the logger sets the
//    level to WARN and sends guard_triggered(). The user can set the level
//    again in the settings.
//  * set_enabled(false) stops all logging (no cost at all).
//
// Debug mode (set_sync_mode(true)): each line goes to the file immediately.
// Thus, no line is lost if the program crashes.
//
// Files: <log_dir>/traffic_engine_YYYY-MM-DD.log. If a file is larger than
// the maximum size, the logger opens a part file: ..._YYYY-MM-DD.p2.log
//
// Line format:
//   2026-10-30 14:05:01.123 [INFO ] [SIM     ] [T01234] message
//
// Use:
//   TE_INFO("SIM", QStringLiteral("Vehicle %1 is added").arg(id));
//   logger::instance().action("Toolbar: road tool");

class logger : public QObject
{
	Q_OBJECT

public:
	enum level
	{
		level_trace = 0,
		level_debug,
		level_info,
		level_warn,
		level_error,
		level_fatal,
		level_off
	};

	static logger &instance();

	// Fast check before the caller makes the message text. No lock.
	static bool is_on(level lvl)
	{
		return static_cast<int>(lvl) >= s_effective_level.load(std::memory_order_relaxed);
	}

	// Starts the writer thread. Call it one time, before the first log line.
	void start(const QString &log_dir);
	// Writes all queued lines and stops the writer thread (end of main).
	void stop();

	void set_enabled(bool on);
	bool enabled() const;
	void set_level(level lvl);
	level min_level() const;
	void set_sync_mode(bool on);
	bool sync_mode() const;
	void set_auto_guard(bool on, int lines_per_second = 20000);
	void set_max_file_size(qint64 bytes);
	// Also writes each line to stderr (console). Used in debug mode.
	void set_console_echo(bool on);

	QString log_dir() const;
	QString current_file() const;

	void write(level lvl, const char *category, const QString &message);

	// Helpers (category in the line)
	void action(const QString &message);
	void file_event(const QString &what, const QString &path);
	void settings_change(const QString &key, const QString &old_value, const QString &new_value);
	void info(const QString &message);
	void warn(const QString &message);
	void error(const QString &message);

	// Waits until the writer thread wrote all queued lines.
	void flush();
	// For the crash handler only: writes the queued lines without a wait.
	// If another thread holds the queue lock, the function does nothing.
	void emergency_flush();

	quint64 written_count() const { return m_written.load(); }
	quint64 dropped_count() const { return m_dropped.load(); }
	int queue_size() const;

	static level level_from_string(const QString &text, level default_level = level_info);
	static QString level_name(level lvl);

	// Sends qDebug/qWarning/qCritical/qFatal to the logger (category QT).
	static void install_qt_handler();

signals:
	// The auto guard changed the level. new_level is a logger::level value.
	void guard_triggered(int new_level);

private:
	struct record
	{
		qint64 ms;
		quint32 thread_id;
		int lvl;
		const char *category;
		QString message;
	};

	logger();
	~logger() override;
	logger(const logger &) = delete;
	logger &operator=(const logger &) = delete;

	void thread_main();
	void update_effective_level();
	void write_records(const std::vector<record> &records);   // writer thread / sync mode
	QByteArray format_record(const record &r) const;
	void ensure_open();
	void rotate_if_needed();
	QString make_file_path(const QString &date, int part) const;
	void check_guard();

	static std::atomic<int> s_effective_level;   // level_off if disabled

	std::atomic<bool> m_enabled;
	std::atomic<int> m_level;
	std::atomic<bool> m_sync;
	std::atomic<bool> m_echo;
	std::atomic<bool> m_guard;
	std::atomic<int> m_guard_rate;

	mutable std::mutex m_queue_mutex;
	std::condition_variable m_cv;
	std::vector<record> m_queue;
	std::size_t m_max_queue;
	bool m_stop;
	bool m_running;
	std::thread m_thread;

	// File state. Only the thread that holds m_file_mutex uses it.
	mutable std::mutex m_file_mutex;
	QString m_log_dir;
	QString m_open_date;
	QString m_file_path;
	int m_part;
	qint64 m_max_file_size;
	void *m_file;          // Win32 HANDLE (append mode)
	qint64 m_file_size;

	std::atomic<quint64> m_written;
	std::atomic<quint64> m_dropped;
	std::atomic<quint64> m_lines_this_second;
	qint64 m_guard_window_start;
	quint64 m_dropped_at_last_check;
};

#define TE_LOG(lvl, cat, msg) \
	do { if (logger::is_on(lvl)) { logger::instance().write((lvl), (cat), (msg)); } } while (0)
#define TE_TRACE(cat, msg) TE_LOG(logger::level_trace, cat, msg)
#define TE_DEBUG(cat, msg) TE_LOG(logger::level_debug, cat, msg)
#define TE_INFO(cat, msg)  TE_LOG(logger::level_info, cat, msg)
#define TE_WARN(cat, msg)  TE_LOG(logger::level_warn, cat, msg)
#define TE_ERROR(cat, msg) TE_LOG(logger::level_error, cat, msg)

#endif // LOGGER_H
