#include "log_compressor.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>

log_compressor::log_compressor(const QString &log_dir, QObject *parent)
	: QThread(parent)
	, m_log_dir(log_dir)
{
}

QString log_compressor::find_seven_zip() const
{
	const QString app_dir = QCoreApplication::applicationDirPath();
	const QStringList candidates =
	{
		app_dir + QStringLiteral("/7z.exe"),
		app_dir + QStringLiteral("/7za.exe"),
		app_dir + QStringLiteral("/7zr.exe"),
		QStringLiteral("C:/Program Files/7-Zip/7z.exe"),
		QStringLiteral("C:/Program Files (x86)/7-Zip/7z.exe"),
	};
	for (const QString &c : candidates)
	{
		if (QFileInfo::exists(c))
		{
			return c;
		}
	}
	for (const QString &n : { QStringLiteral("7z"), QStringLiteral("7za"), QStringLiteral("7zr") })
	{
		const QString found = QStandardPaths::findExecutable(n);
		if (!found.isEmpty())
		{
			return found;
		}
	}
	return QString();
}

// File name: traffic_engine_YYYY-MM-DD.log or traffic_engine_YYYY-MM-DD.pN.log
// Returns true if the date is before today.
bool log_compressor::is_older_than_today(const QString &file_name) const
{
	static const QRegularExpression rx(QStringLiteral("^traffic_engine_(\\d{4}-\\d{2}-\\d{2})(\\.p\\d+)?\\.log$"));
	const QRegularExpressionMatch m = rx.match(file_name);
	if (!m.hasMatch())
	{
		return false;
	}
	const QDate d = QDate::fromString(m.captured(1), QStringLiteral("yyyy-MM-dd"));
	return d.isValid() && d < QDate::currentDate();
}

bool log_compressor::compress_one(const QString &seven_zip, const QString &log_path, QString &archive_out)
{
	const QString archive = log_path + QStringLiteral(".7z");
	if (QFileInfo::exists(archive))
	{
		QFile::remove(archive);
	}

	// 7z a -t7z -mx=9 -y <archive> <source>
	QProcess proc;
	proc.setProgram(seven_zip);
	proc.setArguments({ QStringLiteral("a"), QStringLiteral("-t7z"), QStringLiteral("-mx=9"), QStringLiteral("-y"), archive, log_path });
	proc.start();
	if (!proc.waitForStarted(15000))
	{
		return false;
	}
	// Large files can take a long time (background thread). Stop when the
	// program stops: the .log file stays, the part of the archive goes.
	QElapsedTimer timer;
	timer.start();
	while (proc.state() != QProcess::NotRunning)
	{
		if (proc.waitForFinished(200))
		{
			break;
		}
		if (isInterruptionRequested() || timer.elapsed() > 30LL * 60 * 1000)
		{
			proc.kill();
			proc.waitForFinished(5000);
			QFile::remove(archive);
			return false;
		}
	}
	if (proc.exitStatus() != QProcess::NormalExit || proc.exitCode() != 0)
	{
		return false;
	}
	const QFileInfo ai(archive);
	if (!ai.exists() || ai.size() <= 0)
	{
		return false;
	}
	// If the .log file cannot be removed, keep the archive too (no data loss).
	QFile::remove(log_path);
	archive_out = archive;
	return true;
}

log_compressor::~log_compressor()
{
	requestInterruption();
	wait();
}

void log_compressor::run()
{
	const QString seven_zip = find_seven_zip();
	const QDir dir(m_log_dir);
	if (seven_zip.isEmpty() || !dir.exists())
	{
		emit compression_done(0);
		return;
	}

	int count = 0;
	const QFileInfoList files = dir.entryInfoList({ QStringLiteral("traffic_engine_*.log") }, QDir::Files, QDir::Name);
	for (const QFileInfo &fi : files)
	{
		if (isInterruptionRequested())
		{
			break; // the program stops
		}
		if (!is_older_than_today(fi.fileName()))
		{
			continue; // do not touch the active log files of today
		}
		QString archive;
		if (compress_one(seven_zip, fi.absoluteFilePath(), archive))
		{
			++count;
			emit file_compressed(archive);
		}
	}
	emit compression_done(count);
}
