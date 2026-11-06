#include "app_paths.h"

#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>

namespace app_paths
{
	QString data_dir()
	{
		// %APPDATA% (for example C:\Users\<user>\AppData\Roaming). If the variable
		// is not set, use the generic data location of Qt, then the program folder.
		QString base = qEnvironmentVariable("APPDATA");
		if (base.isEmpty())
		{
			base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
		}
		if (base.isEmpty())
		{
			base = QCoreApplication::applicationDirPath();
		}
		const QString dir = QDir(base).filePath(QStringLiteral("traffic_engine"));
		QDir().mkpath(dir);
		return dir;
	}

	QString data_file(const QString &name)
	{
		return QDir(data_dir()).filePath(name);
	}

	QString log_dir()
	{
		const QString dir = data_file(QStringLiteral("log"));
		QDir().mkpath(dir);
		return dir;
	}

	QString crash_dir()
	{
		const QString dir = data_file(QStringLiteral("crash"));
		QDir().mkpath(dir);
		return dir;
	}

	QString config_file()
	{
		return data_file(QStringLiteral("config.xml"));
	}

	QString app_data_dir()
	{
		return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("data"));
	}
}
