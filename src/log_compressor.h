#ifndef LOG_COMPRESSOR_H
#define LOG_COMPRESSOR_H

#include <QThread>
#include <QString>

// log_compressor
// --------------
// Compresses the old log files in a background thread. The program starts it
// at startup; the user interface does not stop. The thread compresses each
// traffic_engine_*.log file that is OLDER THAN TODAY with 7-Zip (-mx=9). If
// the archive is good, the thread removes the .log file.
//
// 7-Zip comes from the program folder (the build links cots\7zip there) or
// from the 7-Zip installation. If there is no 7-Zip, the thread stops and
// the log files stay as they are.
//
// Use:
//   log_compressor *c = new log_compressor(log_dir, this);
//   connect(c, &QThread::finished, c, &QObject::deleteLater);
//   c->start();

class log_compressor : public QThread
{
	Q_OBJECT

public:
	explicit log_compressor(const QString &log_dir, QObject *parent = nullptr);
	// The program stops: the 7-Zip process stops (its .log file stays) and the
	// thread ends before its object (else Qt stops the program with an error).
	~log_compressor() override;

signals:
	// The thread compressed a file (information).
	void file_compressed(const QString &archive_path);
	// The work is complete: number of compressed files.
	void compression_done(int count);

protected:
	void run() override;

private:
	QString find_seven_zip() const;
	bool is_older_than_today(const QString &file_name) const;
	bool compress_one(const QString &seven_zip, const QString &log_path, QString &archive_out);

	QString m_log_dir;
};

#endif // LOG_COMPRESSOR_H
