#ifndef APP_PATHS_H
#define APP_PATHS_H

#include <QString>

// app_paths
// ---------
// One location for the writable data of the program: %APPDATA%\traffic_engine
// (no space in the name). config.xml, log\ and crash\ are in this folder.
// The read-only files (data\, Qt plugins) stay next to the program
// (QCoreApplication::applicationDirPath).

namespace app_paths
{
	// %APPDATA%\traffic_engine. The function makes the folder if it does not exist.
	QString data_dir();
	// A file or a sub-path in data_dir(). The function does not make the file.
	QString data_file(const QString &name);
	// %APPDATA%\traffic_engine\log
	QString log_dir();
	// %APPDATA%\traffic_engine\crash
	QString crash_dir();
	// %APPDATA%\traffic_engine\config.xml
	QString config_file();
	// The read-only data folder next to the program (bin\<config>\data).
	// Use it only after QApplication is made.
	QString app_data_dir();
}

#endif // APP_PATHS_H
