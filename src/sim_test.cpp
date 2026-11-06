#include "sim_test.h"
#include "app_paths.h"
#include "demo_world.h"
#include "logger.h"
#include "osm_import.h"
#include "osm_import_dialog.h"
#include "workspace.h"
#include "world_model.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTextStream>
#include <QTime>

#include <map>

namespace
{
	QString arg_value(const QStringList &args, const QString &name)
	{
		const int i = args.indexOf(name);
		return (i >= 0 && i + 1 < args.size()) ? args.at(i + 1) : QString();
	}

	QString hms(double s)
	{
		return QTime(0, 0).addSecs(static_cast<int>(s)).toString(QStringLiteral("HH:mm:ss"));
	}

	bool load_source(workspace &ws, const QString &file, QString *error)
	{
		if (file.isEmpty() || file == QLatin1String("demo"))
		{
			demo_world::build(ws.model());
			ws.model_changed(property_change::change_network);
			return true;
		}
		const QFileInfo fi(file);
		if (fi.suffix().compare(QStringLiteral("osm"), Qt::CaseInsensitive) == 0)
		{
			QFile f(fi.absoluteFilePath());
			if (!f.open(QIODevice::ReadOnly))
			{
				*error = QStringLiteral("Cannot open the file %1").arg(fi.absoluteFilePath());
				return false;
			}
			osm_import_result res;
			if (!osm_import::convert(f.readAll(), osm_bounds(), osm_import_dialog::saved_options(), ws.model(), &res, error))
			{
				return false;
			}
			ws.model().name = fi.completeBaseName();
			ws.model().touch_network();
			ws.model_changed(property_change::change_network);
			return true;
		}
		return ws.load(fi.absoluteFilePath(), error);
	}
}

namespace sim_test
{
	int run(const QStringList &args)
	{
		const double seconds = std::max(1.0, arg_value(args, QStringLiteral("--sim-test")).toDouble());
		const QString file = arg_value(args, QStringLiteral("--sim-file"));
		QString report_path = arg_value(args, QStringLiteral("--sim-report"));
		if (report_path.isEmpty())
		{
			report_path = QDir(app_paths::log_dir()).filePath(QStringLiteral("sim_test.txt"));
		}
		const double stuck = arg_value(args, QStringLiteral("--sim-stuck")).isEmpty() ? 120.0 : arg_value(args, QStringLiteral("--sim-stuck")).toDouble();

		workspace ws;
		QString error;
		if (!load_source(ws, file, &error))
		{
			TE_ERROR("TEST", QStringLiteral("Simulation test: the source cannot be loaded: %1").arg(error));
			return 2;
		}
		const QString save_path = arg_value(args, QStringLiteral("--sim-save"));
		if (!save_path.isEmpty())
		{
			// The loaded workspace (for example the result of an OpenStreetMap import)
			if (ws.save(save_path, &error))
			{
				TE_INFO("TEST", QStringLiteral("Simulation test: workspace saved: %1").arg(save_path));
			}
			else
			{
				TE_ERROR("TEST", QStringLiteral("Simulation test: the workspace cannot be saved: %1").arg(error));
			}
		}
		const QString clock = arg_value(args, QStringLiteral("--sim-clock"));
		if (!clock.isEmpty())
		{
			const QTime t = QTime::fromString(clock, QStringLiteral("H:mm"));
			if (t.isValid())
			{
				ws.model().start_clock = t.msecsSinceStartOfDay() / 1000.0;
			}
		}
		simulation &sim = ws.sim();
		bool density_ok = false;
		const double density = arg_value(args, QStringLiteral("--shot-density")).toDouble(&density_ok);
		bool seed_ok = false;
		const quint32 seed = arg_value(args, QStringLiteral("--sim-seed")).toUInt(&seed_ok);
		if (seed_ok)
		{
			sim.set_seed(seed);
		}
		bool gridlock_ok = false;
		const double gridlock = arg_value(args, QStringLiteral("--sim-gridlock")).toDouble(&gridlock_ok);
		ws.reset_simulation();
		if (density_ok)
		{
			sim.set_demand_factors(density / 100.0, density / 100.0);
		}
		if (gridlock_ok)
		{
			sim.set_gridlock_timeout(gridlock);
		}
		sim.set_checks(true, stuck);

		const road_network &net = ws.network();
		const QString source = file.isEmpty() ? QStringLiteral("demo") : QFileInfo(file).fileName();
		TE_INFO("TEST", QStringLiteral("Simulation test: %1, %2 s, density %3, seed %4, gridlock rule %5 s, stuck limit %6 s. Network: %7 lanes, %8 connectors, %9 junctions.")
			.arg(source).arg(seconds).arg(density_ok ? QStringLiteral("%1%").arg(density) : QStringLiteral("from config.xml"))
			.arg(seed_ok ? QString::number(seed) : QStringLiteral("default")).arg(sim.gridlock_timeout()).arg(stuck)
			.arg(net.lanes.size()).arg(net.connectors.size()).arg(net.junctions.size()));

		QStringList lines;
		lines << QStringLiteral("Traffic Engine simulation test, %1").arg(QDateTime::currentDateTime().toString(Qt::ISODate))
			<< QStringLiteral("Source: %1, simulated time %2 s, density %3, seed %4, gridlock rule %5 s, stuck limit %6 s")
				.arg(file.isEmpty() ? QStringLiteral("demo") : QFileInfo(file).absoluteFilePath()).arg(seconds)
				.arg(density_ok ? QStringLiteral("%1%").arg(density) : QStringLiteral("config.xml"))
				.arg(seed_ok ? QString::number(seed) : QStringLiteral("default")).arg(sim.gridlock_timeout()).arg(stuck)
			<< QStringLiteral("Network: %1 lanes, %2 connectors, %3 junctions").arg(net.lanes.size()).arg(net.connectors.size()).arg(net.junctions.size())
			<< QString();

		QElapsedTimer timer;
		timer.start();
		const double dt = sim.time_step();
		const qint64 steps = static_cast<qint64>(seconds / dt + 0.5);
		const int detail_limit = 300;
		std::map<int, int> shown;
		int max_vehicles = 0;
		double next_progress = 600.0;
		for (qint64 k = 0; k < steps; ++k)
		{
			ws.single_step();
			max_vehicles = std::max(max_vehicles, static_cast<int>(sim.vehicles().size()));
			for (const sim_issue &is : sim.take_issues())
			{
				int &n = shown[is.kind];
				if (n++ < detail_limit)
				{
					lines << QStringLiteral("[%1] %2").arg(hms(is.time)).arg(is.text);
				}
			}
			if (sim.time() >= next_progress)
			{
				next_progress += 600.0;
				int stopped = 0;
				double sum_v = 0.0;
				for (const sim_vehicle &v : sim.vehicles())
				{
					stopped += v.v < 0.3 ? 1 : 0;
					sum_v += v.v;
				}
				const QString p = QStringLiteral("Progress %1: vehicles %2 (stopped %3), mean speed %4 km/h, completed %5, removed (gridlock) %6, overlaps %7, stuck %8, deadlocks %9")
					.arg(hms(sim.time())).arg(sim.vehicles().size()).arg(stopped)
					.arg(sim.vehicles().empty() ? 0.0 : sum_v / sim.vehicles().size() * 3.6, 0, 'f', 1)
					.arg(sim.finished_vehicles()).arg(sim.gridlock_removed())
					.arg(sim.issue_count(sim_issue::overlap)).arg(sim.issue_count(sim_issue::stuck)).arg(sim.issue_count(sim_issue::deadlock));
				lines << p;
				TE_INFO("TEST", p);
			}
		}
		const quint64 overlaps = sim.issue_count(sim_issue::overlap);
		const quint64 stuck_count = sim.issue_count(sim_issue::stuck);
		const quint64 deadlocks = sim.issue_count(sim_issue::deadlock);
		const quint64 leaks = sim.issue_count(sim_issue::leak);
		const QString summary = QStringLiteral("Result: %1 s simulated in %2 s. Vehicles now %3 (maximum %4), completed %5, removed (gridlock) %6. Overlaps %7, stuck %8, deadlocks %9, leaks %10.")
			.arg(seconds).arg(timer.elapsed() / 1000.0, 0, 'f', 1).arg(sim.vehicles().size()).arg(max_vehicles)
			.arg(sim.finished_vehicles()).arg(sim.gridlock_removed()).arg(overlaps).arg(stuck_count).arg(deadlocks).arg(leaks);
		lines << QString() << summary;
		TE_INFO("TEST", QStringLiteral("Simulation test: %1 Report: %2").arg(summary, report_path));

		QDir().mkpath(QFileInfo(report_path).absolutePath());
		QSaveFile out(report_path);
		if (out.open(QIODevice::WriteOnly | QIODevice::Text))
		{
			QTextStream ts(&out);
			ts.setCodec("UTF-8");
			for (const QString &l : lines)
			{
				ts << l << '\n';
			}
			ts.flush();
			out.commit();
		}
		return (overlaps + stuck_count + deadlocks + leaks) > 0 ? 1 : 0;
	}
}
