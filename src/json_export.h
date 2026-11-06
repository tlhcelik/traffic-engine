#ifndef JSON_EXPORT_H
#define JSON_EXPORT_H

#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>

class workspace;

// json_export
// -----------
// A report of the program and of one workspace in JSON format: program
// information, workspace contents, simulation state, the latest statistics,
// the states of the traffic lights (phase, color, remaining time, waiting
// vehicles) and the vehicles (position, speed, fuel, passengers).
// The external API (api_server, docs/api.md) uses the same objects.
// Positions: x, y in meters; lat, lon if the workspace has a geographic origin.

namespace json_export
{
	QJsonObject program_object();
	QJsonObject workspace_object(const workspace &ws);
	QJsonObject simulation_object(const workspace &ws);
	QJsonObject statistics_object(const workspace &ws);
	QJsonArray lights_array(const workspace &ws);
	QJsonArray vehicles_array(const workspace &ws);
	// Congestion analysis: the junctions (the worst first) with their level of
	// service, delay, queues and main cause of the waits.
	QJsonObject congestion_object(const workspace &ws);

	// The full report (File > Export the report).
	QByteArray workspace_report(const workspace &ws, bool include_vehicles);
}

#endif // JSON_EXPORT_H
