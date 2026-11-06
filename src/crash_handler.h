#ifndef CRASH_HANDLER_H
#define CRASH_HANDLER_H

#include <QString>
#include <QStringList>

// crash_handler
// -------------
// Catches each crash and writes the data that is necessary to find the cause.
// No crash is silent.
//
// install() sets these handlers:
//   * SetUnhandledExceptionFilter  -> SEH exceptions (access violation, ...)
//   * std::set_terminate           -> C++ exception that nobody caught
//   * _set_purecall_handler        -> call of a pure virtual function
//   * _set_invalid_parameter_handler -> CRT function with a bad parameter
//   * signal(SIGABRT)              -> abort() (also qFatal)
//
// For each crash, the handler writes to %APPDATA%\traffic_engine\crash\:
//   (a) traffic_engine_YYYYMMDD_HHMMSS_<pid>.dmp  (MiniDumpWriteDump). In debug
//       mode, or if "debug/full_memory_dump" is true, the dump has all memory.
//   (b) traffic_engine_YYYYMMDD_HHMMSS_<pid>.txt  (crash report: exception
//       code, address, module and the stack trace with function names and
//       line numbers from the PDB file)
// Then it writes the queued log lines and a "=== CRASH ===" line to the log.
//
// Open the .dmp file in Visual Studio (File > Open > File, then "Debug with
// Native Only"). The PDB file must be next to the program (bin\<config>).
//
// If a debugger is attached, the handler stops in the debugger first.

namespace crash_handler
{
	// Installs the handlers (one time). Call it after the logger starts.
	void install(bool full_memory_dump);

	// Makes a crash on purpose to test the handler ("--crash-test" argument).
	void force_test_crash();

	// The crash reports (.txt) that are newer than the time stamp
	// "yyyyMMdd_HHmmss" (empty = all). Newest first.
	QStringList reports_after(const QString &stamp);
	// The time stamp of a report file name (yyyyMMdd_HHmmss).
	QString stamp_of(const QString &report_path);
}

#endif // CRASH_HANDLER_H
