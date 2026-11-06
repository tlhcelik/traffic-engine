#ifndef DEBUG_TOOLS_H
#define DEBUG_TOOLS_H

#include <QString>

// debug_tools
// -----------
// The debug mode of the program. The debug mode is on if:
//   * the program is a Debug build, or
//   * the command line has "--debug", or
//   * "debug/enabled" is true in config.xml (Settings > Debug).
//
// In debug mode:
//   * the log level is TRACE and each log line goes to the file immediately
//     (no line is lost if the program crashes),
//   * a Release build opens a console window that shows the log lines,
//   * the crash dumps contain all memory,
//   * the developer overlay (FPS, timings) is on at startup,
//   * TE_ASSERT stops in the debugger if a debugger is attached,
//   * the OpenGL debug messages go to the log.

namespace debug_tools
{
	// Call it one time at startup, before the first log line.
	void init(bool debug_mode);
	bool is_debug_mode();

	// Opens a console window for the log lines (Release build in debug mode).
	void attach_console();

	// One line with the system data (Windows, CPU, memory, Qt, build).
	QString system_summary();

	// Called by TE_ASSERT. Writes the error to the log; in debug mode with a
	// debugger, it stops in the debugger.
	void assert_failed(const char *expression, const QString &message, const char *file, int line);
}

#define TE_ASSERT(cond, msg) \
	do { if (!(cond)) { debug_tools::assert_failed(#cond, (msg), __FILE__, __LINE__); } } while (0)

#endif // DEBUG_TOOLS_H
