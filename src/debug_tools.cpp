#include "debug_tools.h"
#include "logger.h"

#include <QSysInfo>
#include <QThread>

#include <cstdio>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace
{
	bool g_debug_mode = false;
	bool g_console = false;
}

namespace debug_tools
{
	void init(bool debug_mode)
	{
		g_debug_mode = debug_mode;
		// A Debug build has a console (console subsystem). A Release build in
		// debug mode opens one.
		if (GetConsoleWindow() != nullptr)
		{
			g_console = true;
		}
		else if (debug_mode)
		{
			attach_console();
		}
	}

	bool is_debug_mode()
	{
		return g_debug_mode;
	}

	void attach_console()
	{
		if (g_console)
		{
			return;
		}
		if (AllocConsole())
		{
			FILE *f = nullptr;
			freopen_s(&f, "CONOUT$", "w", stdout);
			freopen_s(&f, "CONOUT$", "w", stderr);
			SetConsoleTitleW(L"Traffic Engine - debug log");
			g_console = true;
		}
	}

	QString system_summary()
	{
		MEMORYSTATUSEX ms;
		ms.dwLength = sizeof(ms);
		double total_gb = 0.0;
		double free_gb = 0.0;
		if (GlobalMemoryStatusEx(&ms))
		{
			total_gb = static_cast<double>(ms.ullTotalPhys) / (1024.0 * 1024.0 * 1024.0);
			free_gb = static_cast<double>(ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
		}
		return QStringLiteral("Traffic Engine %1 (%2, MSVC %3) | %4 (kernel %5, %6) | CPU threads: %7 | RAM: %8 GB, free %9 GB | Qt %10")
			.arg(QStringLiteral(TE_VERSION), QStringLiteral(TE_BUILD_CONFIG))
			.arg(_MSC_FULL_VER)
			.arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion(), QSysInfo::currentCpuArchitecture())
			.arg(QThread::idealThreadCount())
			.arg(total_gb, 0, 'f', 1)
			.arg(free_gb, 0, 'f', 1)
			.arg(QString::fromLatin1(qVersion()));
	}

	void assert_failed(const char *expression, const QString &message, const char *file, int line)
	{
		TE_ERROR("ASSERT", QStringLiteral("Check failed: %1 | %2 (%3:%4)")
			.arg(QString::fromLatin1(expression), message, QString::fromLatin1(file)).arg(line));
		if (g_debug_mode && IsDebuggerPresent())
		{
			logger::instance().flush();
			__debugbreak();
		}
	}
}
