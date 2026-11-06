#ifndef OSM_IMPORT_DIALOG_H
#define OSM_IMPORT_DIALOG_H

#include "osm_import.h"

#include <QDialog>
#include <QObject>

#include <functional>
#include <memory>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTimer;
template <typename T> class QFutureWatcher;
class http_cancel;
class osm_area_map;
class world_model;
struct http_response;

// osm_job
// -------
// Runs one OpenStreetMap import in a worker thread: the download from the
// Overpass API (or the read of an .osm file), then osm_import::convert().
// The user interface stays active. finished() comes in the user interface
// thread. cancel() stops the download immediately.
//
// The last download is also saved as %APPDATA%\traffic_engine\osm\last_download.osm
// (an import again with other options does not need a new download).

class osm_job : public QObject
{
	Q_OBJECT

public:
	explicit osm_job(QObject *parent = nullptr);
	~osm_job() override;

	void start_download(const osm_bounds &bounds, const osm_import_options &opt);
	void start_file(const QString &path, const osm_import_options &opt);
	void cancel();
	bool busy() const;
	// For the progress display, for example "Download: 1.2 MB".
	QString progress_text() const;

	// Results (after finished(true))
	std::shared_ptr<world_model> model() const;
	osm_import_result result() const { return m_result; }
	QString error() const { return m_error; }

signals:
	void finished(bool ok);

private:
	struct state;
	void start(const std::function<void()> &work);
	void done();

	std::shared_ptr<state> m_state;
	QFutureWatcher<void> *m_watcher;
	osm_import_result m_result;
	QString m_error;
};

// osm_import_dialog
// -----------------
// File > Import from OpenStreetMap. The user selects an area on the live map
// (osm_area_map: draw, move, change the size), with a place search
// (Nominatim), a place from the list or a center and a size, or an .osm file.
// The map and the fields update each other. Import downloads and converts the
// data; the dialog shows the progress. After a success, model() is the new world.

class osm_import_dialog : public QDialog
{
	Q_OBJECT

public:
	explicit osm_import_dialog(QWidget *parent = nullptr);
	~osm_import_dialog() override;

	std::shared_ptr<world_model> model() const;
	osm_import_result result() const;
	// Name for the new workspace (the place or the file name).
	QString place_name() const;
	// The options of the last import (config.xml), for an import without the dialog.
	static osm_import_options saved_options();

protected:
	void reject() override;

private:
	void start_import();
	void job_done(bool ok);
	void update_state();
	void save_settings() const;
	osm_import_options options() const;
	// The map and the fields: one updates the other.
	void fields_to_map(bool fit);
	void map_to_fields(const osm_bounds &b);
	// Place search (Nominatim) in a worker thread.
	void find_place();
	void place_found();

	osm_job *m_job;
	QTimer *m_progress_timer;
	osm_area_map *m_map;
	QLineEdit *m_search;
	QPushButton *m_find;
	QFutureWatcher<http_response> *m_search_watcher;
	std::shared_ptr<http_cancel> m_search_cancel;
	bool m_syncing;
	QRadioButton *m_src_area;
	QRadioButton *m_src_file;
	QComboBox *m_place;
	QLineEdit *m_coords;
	QDoubleSpinBox *m_lat;
	QDoubleSpinBox *m_lon;
	QSpinBox *m_width;
	QSpinBox *m_height;
	QLineEdit *m_file;
	QPushButton *m_browse;
	QCheckBox *m_residential;
	QCheckBox *m_service;
	QCheckBox *m_lights;
	QCheckBox *m_priority;
	QCheckBox *m_bus_stops;
	QCheckBox *m_crossings;
	QCheckBox *m_sidewalks;
	QSpinBox *m_join;
	QSpinBox *m_demand;
	QLabel *m_status;
	QProgressBar *m_progress;
	QPushButton *m_import;
	QString m_place_name;
};

#endif // OSM_IMPORT_DIALOG_H
