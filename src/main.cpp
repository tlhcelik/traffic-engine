#include "main_window.h"
#include "app_paths.h"
#include "config_common.h"
#include "crash_handler.h"
#include "debug_tools.h"
#include "i18n.h"
#include "log_compressor.h"
#include "logger.h"
#include "app_theme.h"
#include "sign_catalog.h"
#include "sim_test.h"
#include "splash_screen.h"
#include "tile_cache.h"
#include "xml_doc.h"

#include <QApplication>
#include <QFileInfo>
#include <QIcon>
#include <QPainter>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSurfaceFormat>
#include <QTimer>

#include <cstring>
#include <memory>

namespace
{
	bool has_arg(int argc, char *argv[], const char *name)
	{
		for (int i = 1; i < argc; ++i)
		{
			if (std::strcmp(argv[i], name) == 0)
			{
				return true;
			}
		}
		return false;
	}

	// The value after "name" ("--shot-delay 5"), or an empty text.
	QString arg_value(const QStringList &args, const QString &name)
	{
		const int i = args.indexOf(name);
		return (i >= 0 && i + 1 < args.size()) ? args.at(i + 1) : QString();
	}
}

int main(int argc, char *argv[])
{
	// Xerces-C must start before the first XML operation (config.xml).
	xml_platform xml;

	// --- 1) Configuration. Before QApplication: the OpenGL format needs it. ---
	config_common &cfg = config_common::instance();
	const bool config_ok = xml.ok() && cfg.load(app_paths::config_file());

	// --- 2) Debug mode: Debug build, "--debug" or config "debug/enabled" ---
	bool debug_mode = cfg.bool_value(QStringLiteral("debug/enabled")) || has_arg(argc, argv, "--debug");
#ifdef _DEBUG
	debug_mode = true;
#endif
	debug_tools::init(debug_mode);

	// --- 3) Logger ---
	logger &log = logger::instance();
	log.set_max_file_size(qMax(1, cfg.int_value(QStringLiteral("logging/max_file_mb"))) * 1024LL * 1024LL);
	log.set_enabled(cfg.bool_value(QStringLiteral("logging/enabled")) || debug_mode);
	log.set_level(debug_mode ? logger::level_trace : logger::level_from_string(cfg.value(QStringLiteral("logging/level"))));
	log.set_auto_guard(cfg.bool_value(QStringLiteral("logging/auto_guard")) && !debug_mode,
		cfg.int_value(QStringLiteral("logging/guard_lines_per_second")));
	log.set_sync_mode(debug_mode);
	log.set_console_echo(debug_mode);
	log.start(app_paths::log_dir());
	logger::install_qt_handler();

	// --- 4) Crash handler. Early: it also catches crashes during the startup. ---
	crash_handler::install(debug_mode || cfg.bool_value(QStringLiteral("debug/full_memory_dump")));
	if (has_arg(argc, argv, "--crash-test"))
	{
		crash_handler::force_test_crash(); // does not return
	}

	// --- 5) OpenGL: hardware OpenGL, shared contexts (workspace tabs), MSAA, vsync ---
	QApplication::setAttribute(Qt::AA_UseDesktopOpenGL);
	QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
	QApplication::setAttribute(Qt::AA_EnableHighDpiScaling);
	QApplication::setAttribute(Qt::AA_UseHighDpiPixmaps);
	{
		QSurfaceFormat fmt;
		fmt.setRenderableType(QSurfaceFormat::OpenGL);
		fmt.setSamples(qBound(0, cfg.int_value(QStringLiteral("render/msaa_samples")), 16));
		fmt.setSwapInterval(cfg.bool_value(QStringLiteral("render/vsync")) ? 1 : 0);
		fmt.setStencilBufferSize(8);
		if (debug_mode)
		{
			fmt.setOption(QSurfaceFormat::DebugContext);
		}
		QSurfaceFormat::setDefaultFormat(fmt);
	}

	QApplication app(argc, argv);
	QApplication::setApplicationName(QStringLiteral("Traffic Engine"));
	QApplication::setOrganizationName(QStringLiteral("Traffic Engine"));
	QApplication::setApplicationVersion(QStringLiteral(TE_VERSION));
	QApplication::setWindowIcon(QIcon(QStringLiteral(":/icons/traffic_engine.png")));

	TE_INFO("APP", QStringLiteral("=== Traffic Engine starts ==="));
	TE_INFO("APP", debug_tools::system_summary());
	TE_INFO("APP", QStringLiteral("Debug mode: %1. Configuration: %2. Log folder: %3")
		.arg(debug_mode ? QStringLiteral("on") : QStringLiteral("off"), cfg.config_file_path(), app_paths::log_dir()));
	if (!xml.ok())
	{
		TE_ERROR("CONFIG", QStringLiteral("Xerces-C did not start: %1").arg(xml.error()));
	}
	else if (!config_ok)
	{
		TE_WARN("CONFIG", QStringLiteral("config.xml cannot be read (%1). The program uses the default values.").arg(cfg.last_error()));
	}

	// Workspace files (*.te) and OpenStreetMap files (*.osm) from the command line
	QStringList files;
	const QStringList args = QApplication::arguments();
	for (int i = 1; i < args.size(); ++i)
	{
		const QFileInfo fi(args.at(i));
		const QString suffix = fi.suffix().toLower();
		if ((suffix == QLatin1String("te") || suffix == QLatin1String("osm")) && fi.exists())
		{
			files.append(fi.absoluteFilePath());
		}
	}

	// --- 6) Single instance: a second start sends its files to the first window.
	//        "--new-instance" starts a separate instance.
	const QString server_name = QStringLiteral("TrafficEngine-") + QString::fromLocal8Bit(qgetenv("USERNAME"));
	if (!has_arg(argc, argv, "--new-instance") && !has_arg(argc, argv, "--sim-test"))
	{
		QLocalSocket probe;
		probe.connectToServer(server_name);
		if (probe.waitForConnected(300))
		{
			probe.write(files.join(QLatin1Char('\n')).toUtf8());
			probe.flush();
			probe.waitForBytesWritten(500);
			probe.disconnectFromServer();
			TE_INFO("APP", QStringLiteral("Another instance is open. The files go to that instance: %1").arg(files.join(QStringLiteral(", "))));
			log.stop();
			return 0;
		}
	}

	app_theme::apply(app);

	// Language of the user interface: config general/language; --lang <code> for this session only
	const QString lang_arg = arg_value(args, QStringLiteral("--lang"));
	i18n::set_language(lang_arg.isEmpty() ? cfg.value(QStringLiteral("general/language")) : lang_arg);

	// Traffic sign catalog (data\signs\tr_signs.xml next to the program)
	sign_catalog::instance().load(app_paths::app_data_dir() + QStringLiteral("/signs/tr_signs.xml"));

	// --sign-sheet <file.png>: all sign faces with their codes in one image (check of the catalog)
	const QString sheet = arg_value(args, QStringLiteral("--sign-sheet"));
	if (!sheet.isEmpty())
	{
		const QList<sign_def> &defs = sign_catalog::instance().all();
		const int cols = 12;
		const int cell = 112;
		const int rows = (defs.size() + cols - 1) / cols;
		QImage img(cols * cell, rows * (cell + 26), QImage::Format_ARGB32_Premultiplied);
		img.fill(QColor(236, 240, 245));
		QPainter p(&img);
		p.setRenderHint(QPainter::Antialiasing, true);
		QFont f(QStringLiteral("Arial"));
		f.setPixelSize(13);
		p.setFont(f);
		for (int i = 0; i < defs.size(); ++i)
		{
			const QRectF r((i % cols) * cell, (i / cols) * (cell + 26), cell, cell + 26);
			sign_painter::paint(p, QRectF(r.left() + 8, r.top() + 4, cell - 16, cell - 16), defs.at(i), defs.at(i).default_value);
			p.setPen(QColor(40, 46, 58));
			p.drawText(QRectF(r.left(), r.bottom() - 30, cell, 26), Qt::AlignCenter, defs.at(i).code.isEmpty() ? defs.at(i).key : defs.at(i).code);
		}
		p.end();
		const bool saved = img.save(sheet);
		TE_INFO("SIGNS", QStringLiteral("Sign sheet: %1 signs -> %2 (%3)").arg(defs.size()).arg(sheet, saved ? QStringLiteral("saved") : QStringLiteral("not saved")));
		log.stop();
		return saved ? 0 : 1;
	}

	// --splash-shot <file.png>: draws the splash screen into an image and closes (check of the drawing)
	const QString splash_shot = arg_value(args, QStringLiteral("--splash-shot"));
	if (!splash_shot.isEmpty())
	{
		splash_screen splash(0, 0);
		splash.start();
		splash.set_status(tx("Open the start workspace..."));
		bool saved = false;
		QTimer::singleShot(1500, &app, [&]()
		{
			saved = splash.grab().save(splash_shot);
			app.quit();
		});
		app.exec();
		TE_INFO("TEST", QStringLiteral("Splash shot: %1 (%2)").arg(splash_shot, saved ? QStringLiteral("saved") : QStringLiteral("not saved")));
		log.stop();
		return saved ? 0 : 1;
	}

	// --sim-test <seconds>: simulation test without a window (see sim_test.h)
	if (args.contains(QStringLiteral("--sim-test")))
	{
		const int rc = sim_test::run(args);
		log.stop();
		return rc;
	}

	std::unique_ptr<main_window> window;
	QStringList pending; // files of a second start before the main window exists

	QLocalServer server;
	QLocalServer::removeServer(server_name);
	server.listen(server_name);
	QObject::connect(&server, &QLocalServer::newConnection, &app, [&server, &window, &pending]()
	{
		QLocalSocket *c = server.nextPendingConnection();
		if (!c)
		{
			return;
		}
		c->waitForReadyRead(500);
		const QStringList list = QString::fromUtf8(c->readAll()).split(QLatin1Char('\n'), QString::SkipEmptyParts);
		c->disconnectFromServer();
		c->deleteLater();
		if (!window)
		{
			pending << list;
			return;
		}
		window->setWindowState((window->windowState() & ~Qt::WindowMinimized) | Qt::WindowActive);
		window->show();
		window->raise();
		window->activateWindow();
		if (!list.isEmpty())
		{
			window->open_files(list);
		}
	});

	// Test mode: --screenshot <file.png> [--shot-delay s] [--shot-speed x] [--shot-dev]
	//            [--shot-select light|vehicle|charts|editor|density|program|lang|osm|osm_search|settings]
	//            [--shot-zoom px_per_m] [--shot-density percent] [--lang en|tr]
	const QString shot = arg_value(args, QStringLiteral("--screenshot"));
	// The first content of the main window: files, start workspace, API, import, test
	auto open_content = [&](splash_screen *splash)
	{
		auto status = [splash](const QString &text)
		{
			if (splash)
			{
				splash->set_status(text);
			}
		};
		status(files.isEmpty() ? tx("Open the start workspace...") : tx("Open the files..."));
		if (!files.isEmpty())
		{
			window->open_files(files);
		}
		else
		{
			window->open_start_workspace();
		}
		if (!pending.isEmpty())
		{
			window->open_files(pending);
			pending.clear();
		}
		// --api-port <port>: the external API on 127.0.0.1 for this session (config.xml does not change)
		const int api_port = arg_value(args, QStringLiteral("--api-port")).toInt();
		if (api_port > 0)
		{
			status(tx("Start the API server..."));
			window->start_api_session(api_port);
		}
		// --osm-area "lat,lon,size_m": import an area from the Overpass API (no dialog)
		const QStringList area = arg_value(args, QStringLiteral("--osm-area")).split(QLatin1Char(','));
		if (area.size() == 3)
		{
			window->import_osm_area(area.at(0).toDouble(), area.at(1).toDouble(), qBound(100.0, area.at(2).toDouble(), 5000.0));
		}
		if (!shot.isEmpty())
		{
			const double delay = arg_value(args, QStringLiteral("--shot-delay")).toDouble();
			const double speed = arg_value(args, QStringLiteral("--shot-speed")).toDouble();
			bool density_ok = false;
			const double density = arg_value(args, QStringLiteral("--shot-density")).toDouble(&density_ok);
			window->schedule_screenshot(QFileInfo(shot).absoluteFilePath(), delay > 0.0 ? delay : 5.0, speed > 0.0 ? speed : 1.0,
				args.contains(QStringLiteral("--shot-dev")), arg_value(args, QStringLiteral("--shot-select")),
				arg_value(args, QStringLiteral("--shot-zoom")).toDouble(), density_ok ? density : -1.0);
			// --shot-size 1280x720, --shot-node <id>, --shot-center "x,y" (m)
			const QStringList size = arg_value(args, QStringLiteral("--shot-size")).split(QLatin1Char('x'));
			const QSize window_size = size.size() == 2 ? QSize(size.at(0).toInt(), size.at(1).toInt()) : QSize();
			bool node_ok = false;
			const int node = arg_value(args, QStringLiteral("--shot-node")).toInt(&node_ok);
			const QStringList center = arg_value(args, QStringLiteral("--shot-center")).split(QLatin1Char(','));
			window->set_shot_view(window_size, node_ok ? node : -1,
				center.size() == 2 ? QPointF(center.at(0).toDouble(), center.at(1).toDouble()) : QPointF(), center.size() == 2);
			// --shot-frames <n> --shot-interval <s>: images of the view for an animation
			const int frames = arg_value(args, QStringLiteral("--shot-frames")).toInt();
			if (frames > 0)
			{
				const double interval = arg_value(args, QStringLiteral("--shot-interval")).toDouble();
				window->set_shot_frames(frames, interval > 0.0 ? interval : 0.1);
			}
		}
	};
	auto after_show = [&window]()
	{
		QTimer::singleShot(600, window.get(), [&window]() { window->check_previous_crash(); });
	};

	// Splash screen (config general/show_splash; not in the test modes, not with --no-splash).
	// The main window starts while the splash is visible (the same flow as in Asan Map).
	const bool use_splash = shot.isEmpty() && !args.contains(QStringLiteral("--no-splash"))
		&& cfg.bool_value(QStringLiteral("general/show_splash"));
	if (use_splash)
	{
		splash_screen *splash = new splash_screen(cfg.int_value(QStringLiteral("general/splash_fade_ms")),
			cfg.int_value(QStringLiteral("general/splash_hold_ms")));
		QObject::connect(splash, &splash_screen::ready_to_init, &app, [&, splash]()
		{
			splash->set_status(tx("Make the main window..."));
			window.reset(new main_window());
			open_content(splash);
			splash->set_status(tx("Ready"));
			splash->finish();
		});
		QObject::connect(splash, &splash_screen::finished, &app, [&]()
		{
			window->show_restored();
			after_show();
		});
		QObject::connect(splash, &splash_screen::finished, splash, &QObject::deleteLater);
		splash->start();
	}
	else
	{
		window.reset(new main_window());
		window->show_restored();
		open_content(nullptr);
		after_show();
	}

	// Old log files -> 7z, in a background thread
	if (cfg.bool_value(QStringLiteral("logging/compress_old")))
	{
		log_compressor *compressor = new log_compressor(app_paths::log_dir(), &app);
		QObject::connect(compressor, &log_compressor::compression_done, [](int count)
		{
			if (count > 0)
			{
				TE_INFO("LOG", QStringLiteral("Old log files are compressed: %1 file(s).").arg(count));
			}
		});
		QObject::connect(compressor, &QThread::finished, compressor, &QObject::deleteLater);
		compressor->start(QThread::LowestPriority);
	}

	const int rc = app.exec();
	tile_cache::instance().shutdown();
	TE_INFO("APP", QStringLiteral("=== Traffic Engine stops (exit code %1) ===").arg(rc));
	cfg.save();
	log.stop();
	return rc;
}
