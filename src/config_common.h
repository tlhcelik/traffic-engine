#ifndef CONFIG_COMMON_H
#define CONFIG_COMMON_H

#include <QObject>
#include <QMap>
#include <QString>
#include <QStringList>

// config_common
// -------------
// The configuration of the program. The file is
// %APPDATA%\traffic_engine\config.xml (Xerces-C, see xml_doc).
//
// Each value has a key "section/name" (for example "logging/level"). The XML
// file has one element for each section and one attribute for each name:
//   <config version="1">
//     <logging enabled="true" level="info" .../>
//     <render msaa_samples="4" .../>
//     <recent_files><file>C:/a.te</file></recent_files>
//   </config>
//
// All keys have a default value (see defaults()). An unknown key in the file
// is ignored. A missing key gets the default value.
//
// set_value() sends changed(key). The modules that use a value connect to this
// signal and apply the new value at runtime. Call save() to write the file.

class config_common : public QObject
{
	Q_OBJECT

public:
	static config_common &instance();

	// Loads the file. If the file does not exist, the function writes a new
	// file with the default values.
	bool load(const QString &file_path);
	bool save();
	QString config_file_path() const;
	QString last_error() const;

	QString value(const QString &key) const;
	bool bool_value(const QString &key) const;
	int int_value(const QString &key) const;
	double double_value(const QString &key) const;

	void set_value(const QString &key, const QString &value);
	void set_bool(const QString &key, bool value);
	void set_int(const QString &key, int value);
	void set_double(const QString &key, double value);

	QStringList keys() const;
	static const QMap<QString, QString> &defaults();

	// Recently used workspace files (newest first, maximum 10)
	QStringList recent_files() const;
	void add_recent_file(const QString &path);
	void remove_recent_file(const QString &path);

signals:
	void changed(const QString &key);
	void recent_files_changed();

private:
	config_common();
	config_common(const config_common &) = delete;
	config_common &operator=(const config_common &) = delete;

	QString m_path;
	QString m_error;
	QMap<QString, QString> m_values;
	QStringList m_recent;
};

#endif // CONFIG_COMMON_H
