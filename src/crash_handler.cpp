#include "crash_handler.h"
#include "app_paths.h"
#include "logger.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <dbghelp.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>

// ---------------------------------------------------------------------------
// The paths are prepared at install time. At crash time, the handler uses only
// these buffers, Win32 and DbgHelp (no Qt, minimum heap use).
// ---------------------------------------------------------------------------
namespace
{
	wchar_t g_crash_dir[MAX_PATH] = L"";
	wchar_t g_log_dir[MAX_PATH] = L"";
	volatile LONG g_in_crash = 0;
	bool g_installed = false;
	bool g_full_dump = false;
	bool g_sym_ok = false;

	const wchar_t *base_name_w(const wchar_t *path)
	{
		const wchar_t *last = path;
		for (const wchar_t *p = path; *p; ++p)
		{
			if (*p == L'\\' || *p == L'/')
			{
				last = p + 1;
			}
		}
		return last;
	}

	const char *exception_name(DWORD code)
	{
		switch (code)
		{
			case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
			case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
			case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
			case EXCEPTION_FLT_DIVIDE_BY_ZERO: return "FLT_DIVIDE_BY_ZERO";
			case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
			case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
			case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
			case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
			case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
			case 0xE06D7363: return "CPP_EXCEPTION";
			case 0xC0000409: return "STACK_BUFFER_OVERRUN";
			case 0: return "NONE";
			default: return "OTHER";
		}
	}

	// Writes text to an open file handle.
	void put(HANDLE h, const char *text)
	{
		if (h == INVALID_HANDLE_VALUE || !text)
		{
			return;
		}
		DWORD n = 0;
		WriteFile(h, text, static_cast<DWORD>(strlen(text)), &n, nullptr);
	}

	// <crash_dir>\traffic_engine_YYYYMMDD_HHMMSS_<pid><ext>
	void make_path(const SYSTEMTIME &st, const wchar_t *ext, wchar_t *out)
	{
		_snwprintf_s(out, MAX_PATH, _TRUNCATE, L"%s\\traffic_engine_%04d%02d%02d_%02d%02d%02d_%lu%s",
			g_crash_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			GetCurrentProcessId(), ext);
	}

	bool write_minidump(const SYSTEMTIME &st, EXCEPTION_POINTERS *ep, wchar_t *out_path)
	{
		out_path[0] = L'\0';
		if (g_crash_dir[0] == L'\0')
		{
			return false;
		}
		make_path(st, L".dmp", out_path);
		HANDLE h = CreateFileW(out_path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			out_path[0] = L'\0';
			return false;
		}
		MINIDUMP_EXCEPTION_INFORMATION mei;
		mei.ThreadId = GetCurrentThreadId();
		mei.ExceptionPointers = ep;
		mei.ClientPointers = FALSE;

		int type = MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules | MiniDumpWithHandleData;
		if (g_full_dump)
		{
			type |= MiniDumpWithFullMemory | MiniDumpWithFullMemoryInfo;
		}
		else
		{
			type |= MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithDataSegs;
		}
		const BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), h,
			static_cast<MINIDUMP_TYPE>(type), ep ? &mei : nullptr, nullptr, nullptr);
		FlushFileBuffers(h);
		CloseHandle(h);
		if (!ok)
		{
			out_path[0] = L'\0';
		}
		return ok != FALSE;
	}

	// Writes the stack trace of the context to the report file.
	void write_stack(HANDLE h, CONTEXT ctx)
	{
		if (!g_sym_ok)
		{
			put(h, "Stack trace: not available (DbgHelp did not start)\r\n");
			return;
		}
		HANDLE process = GetCurrentProcess();
		HANDLE thread = GetCurrentThread();
		STACKFRAME64 frame;
		ZeroMemory(&frame, sizeof(frame));
		frame.AddrPC.Offset = ctx.Rip;
		frame.AddrPC.Mode = AddrModeFlat;
		frame.AddrFrame.Offset = ctx.Rbp;
		frame.AddrFrame.Mode = AddrModeFlat;
		frame.AddrStack.Offset = ctx.Rsp;
		frame.AddrStack.Mode = AddrModeFlat;

		alignas(SYMBOL_INFO) char sym_buf[sizeof(SYMBOL_INFO) + 512];
		char line[1024];
		put(h, "Stack trace:\r\n");
		for (int i = 0; i < 64; ++i)
		{
			if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, &ctx, nullptr,
				SymFunctionTableAccess64, SymGetModuleBase64, nullptr))
			{
				break;
			}
			const DWORD64 addr = frame.AddrPC.Offset;
			if (addr == 0)
			{
				break;
			}
			// Module
			char module[MAX_PATH] = "?";
			IMAGEHLP_MODULE64 mi;
			ZeroMemory(&mi, sizeof(mi));
			mi.SizeOfStruct = sizeof(mi);
			DWORD64 mod_base = 0;
			if (SymGetModuleInfo64(process, addr, &mi))
			{
				strncpy_s(module, mi.ModuleName, _TRUNCATE);
				mod_base = mi.BaseOfImage;
			}
			// Function
			SYMBOL_INFO *sym = reinterpret_cast<SYMBOL_INFO *>(sym_buf);
			ZeroMemory(sym_buf, sizeof(sym_buf));
			sym->SizeOfStruct = sizeof(SYMBOL_INFO);
			sym->MaxNameLen = 511;
			DWORD64 disp = 0;
			const char *fn = "?";
			if (SymFromAddr(process, addr, &disp, sym))
			{
				fn = sym->Name;
			}
			// File and line
			IMAGEHLP_LINE64 li;
			ZeroMemory(&li, sizeof(li));
			li.SizeOfStruct = sizeof(li);
			DWORD ldisp = 0;
			if (SymGetLineFromAddr64(process, addr, &ldisp, &li))
			{
				_snprintf_s(line, sizeof(line), _TRUNCATE, "  #%02d %s!%s +0x%llx  [%s:%lu]\r\n",
					i, module, fn, static_cast<unsigned long long>(disp), li.FileName, li.LineNumber);
			}
			else
			{
				_snprintf_s(line, sizeof(line), _TRUNCATE, "  #%02d %s!%s +0x%llx  (address 0x%016llx, module offset 0x%llx)\r\n",
					i, module, fn, static_cast<unsigned long long>(disp), static_cast<unsigned long long>(addr),
					static_cast<unsigned long long>(addr - mod_base));
			}
			put(h, line);
		}
	}

	void write_report(const SYSTEMTIME &st, const char *kind, DWORD code, const void *addr,
		const CONTEXT &ctx, const wchar_t *dump_path, wchar_t *out_path)
	{
		make_path(st, L".txt", out_path);
		HANDLE h = CreateFileW(out_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			out_path[0] = L'\0';
			return;
		}
		char module_name[MAX_PATH] = "?";
		HMODULE hmod = nullptr;
		if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			static_cast<LPCWSTR>(addr), &hmod) && hmod)
		{
			wchar_t full[MAX_PATH];
			if (GetModuleFileNameW(hmod, full, MAX_PATH))
			{
				WideCharToMultiByte(CP_UTF8, 0, base_name_w(full), -1, module_name, MAX_PATH, nullptr, nullptr);
			}
		}
		char dump_utf8[MAX_PATH * 3] = "(no dump)";
		if (dump_path && dump_path[0])
		{
			WideCharToMultiByte(CP_UTF8, 0, dump_path, -1, dump_utf8, sizeof(dump_utf8), nullptr, nullptr);
		}

		char buf[2048];
		_snprintf_s(buf, sizeof(buf), _TRUNCATE,
			"Traffic Engine crash report\r\n"
			"===========================\r\n"
			"Time      : %04d-%02d-%02d %02d:%02d:%02d\r\n"
			"Version   : %s (%s)\r\n"
			"Kind      : %s\r\n"
			"Exception : 0x%08lX %s\r\n"
			"Address   : 0x%p\r\n"
			"Module    : %s (offset 0x%llx)\r\n"
			"Thread    : %lu\r\n"
			"Dump file : %s\r\n\r\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
			TE_VERSION, TE_BUILD_CONFIG, kind, code, exception_name(code), addr, module_name,
			static_cast<unsigned long long>(reinterpret_cast<const char *>(addr) - reinterpret_cast<const char *>(hmod)),
			GetCurrentThreadId(), dump_utf8);
		put(h, buf);
		write_stack(h, ctx);
		put(h, "\r\nOpen the .dmp file in Visual Studio. Keep traffic_engine.pdb next to traffic_engine.exe.\r\n");
		FlushFileBuffers(h);
		CloseHandle(h);
	}

	// Adds a "=== CRASH ===" line to the log file of today.
	void write_crash_log_line(const SYSTEMTIME &st, const char *kind, DWORD code, const wchar_t *report)
	{
		if (g_log_dir[0] == L'\0')
		{
			return;
		}
		wchar_t log_path[MAX_PATH];
		_snwprintf_s(log_path, MAX_PATH, _TRUNCATE, L"%s\\traffic_engine_%04d-%02d-%02d.log",
			g_log_dir, st.wYear, st.wMonth, st.wDay);
		HANDLE h = CreateFileW(log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
			nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (h == INVALID_HANDLE_VALUE)
		{
			return;
		}
		char report_utf8[MAX_PATH * 3] = "(no report)";
		if (report && report[0])
		{
			WideCharToMultiByte(CP_UTF8, 0, report, -1, report_utf8, sizeof(report_utf8), nullptr, nullptr);
		}
		char line[1400];
		_snprintf_s(line, sizeof(line), _TRUNCATE,
			"%04d-%02d-%02d %02d:%02d:%02d.%03d [FATAL] [CRASH   ] [T%05lu] === CRASH === (%s) code=0x%08lX %s report=%s\n",
			st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
			GetCurrentThreadId(), kind, code, exception_name(code), report_utf8);
		put(h, line);
		FlushFileBuffers(h);
		CloseHandle(h);
	}

	// The common crash path. ep can be null (terminate, abort, ...).
	void handle_crash(const char *kind, EXCEPTION_POINTERS *ep)
	{
		if (InterlockedCompareExchange(&g_in_crash, 1, 0) != 0)
		{
			TerminateProcess(GetCurrentProcess(), 3);
		}
		if (IsDebuggerPresent())
		{
			__debugbreak(); // look at the crash in the debugger
		}

		DWORD code = 0;
		const void *addr = nullptr;
		CONTEXT ctx;
		if (ep && ep->ExceptionRecord && ep->ContextRecord)
		{
			code = ep->ExceptionRecord->ExceptionCode;
			addr = ep->ExceptionRecord->ExceptionAddress;
			ctx = *ep->ContextRecord;
		}
		else
		{
			RtlCaptureContext(&ctx);
			addr = reinterpret_cast<const void *>(ctx.Rip);
		}

		SYSTEMTIME st;
		GetLocalTime(&st);
		wchar_t dump_path[MAX_PATH];
		wchar_t report_path[MAX_PATH];
		write_minidump(st, ep, dump_path);
		write_report(st, kind, code, addr, ctx, dump_path, report_path);
		// The log lines in the queue show the last actions before the crash.
		logger::instance().emergency_flush();
		write_crash_log_line(st, kind, code, report_path);
	}

	LONG WINAPI top_level_filter(EXCEPTION_POINTERS *ep)
	{
		handle_crash("SEH", ep);
		return EXCEPTION_EXECUTE_HANDLER; // end the process without the Windows error dialog
	}

	void on_terminate()
	{
		handle_crash("terminate", nullptr);
		TerminateProcess(GetCurrentProcess(), 3);
	}

	void on_purecall()
	{
		handle_crash("purecall", nullptr);
		TerminateProcess(GetCurrentProcess(), 3);
	}

	void on_invalid_parameter(const wchar_t *, const wchar_t *, const wchar_t *, unsigned int, uintptr_t)
	{
		handle_crash("invalid_parameter", nullptr);
		TerminateProcess(GetCurrentProcess(), 3);
	}

	void on_abort(int)
	{
		handle_crash("abort", nullptr);
		TerminateProcess(GetCurrentProcess(), 3);
	}

	void copy_path(const QString &src, wchar_t *dst)
	{
		const QString n = QDir::toNativeSeparators(src).left(MAX_PATH - 1);
		n.toWCharArray(dst);
		dst[n.size()] = L'\0';
	}
}

namespace crash_handler
{
	void install(bool full_memory_dump)
	{
		if (g_installed)
		{
			return;
		}
		g_installed = true;
		g_full_dump = full_memory_dump;
		copy_path(app_paths::crash_dir(), g_crash_dir);
		copy_path(app_paths::log_dir(), g_log_dir);

		// Load the symbols now (not at crash time). The PDB is next to the program.
		// GetModuleFileNameW works before QApplication is made.
		SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS);
		wchar_t exe_dir[MAX_PATH] = L"";
		if (GetModuleFileNameW(nullptr, exe_dir, MAX_PATH))
		{
			wchar_t *slash = wcsrchr(exe_dir, L'\\');
			if (slash)
			{
				*slash = L'\0';
			}
		}
		g_sym_ok = SymInitializeW(GetCurrentProcess(), exe_dir[0] ? exe_dir : nullptr, TRUE) != FALSE;

		SetUnhandledExceptionFilter(top_level_filter);
		std::set_terminate(on_terminate);
		_set_purecall_handler(on_purecall);
		_set_invalid_parameter_handler(on_invalid_parameter);
		_set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
		std::signal(SIGABRT, on_abort);

		TE_INFO("CRASH", QStringLiteral("Crash handler is installed. Folder: %1, full memory dump: %2, symbols: %3")
			.arg(app_paths::crash_dir()).arg(full_memory_dump ? QStringLiteral("yes") : QStringLiteral("no"))
			.arg(g_sym_ok ? QStringLiteral("loaded") : QStringLiteral("not loaded")));
	}

	void force_test_crash()
	{
		TE_WARN("CRASH", QStringLiteral("A test crash starts now (--crash-test)."));
		volatile int *p = reinterpret_cast<volatile int *>(0);
		*p = 0x1234; // access violation -> top_level_filter
	}

	QString stamp_of(const QString &report_path)
	{
		static const QRegularExpression rx(QStringLiteral("traffic_engine_(\\d{8}_\\d{6})_\\d+\\.txt$"));
		const QRegularExpressionMatch m = rx.match(report_path);
		return m.hasMatch() ? m.captured(1) : QString();
	}

	QStringList reports_after(const QString &stamp)
	{
		QStringList result;
		const QDir dir(app_paths::crash_dir());
		const QFileInfoList files = dir.entryInfoList({ QStringLiteral("traffic_engine_*.txt") }, QDir::Files, QDir::Name | QDir::Reversed);
		for (const QFileInfo &fi : files)
		{
			const QString s = stamp_of(fi.fileName());
			if (!s.isEmpty() && (stamp.isEmpty() || s > stamp))
			{
				result.append(fi.absoluteFilePath());
			}
		}
		return result;
	}
}
