#include "tile_cache.h"
#include "app_paths.h"
#include "config_common.h"
#include "http_client.h"
#include "logger.h"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QRunnable>
#include <QSaveFile>

#include <algorithm>
#include <functional>

namespace
{
	const int memory_tiles = 600;
	const int max_loading = 64;
	const qint64 retry_ms = 60000;

	class task : public QRunnable
	{
	public:
		explicit task(std::function<void()> f) : m_f(std::move(f)) { setAutoDelete(true); }
		void run() override { m_f(); }

	private:
		std::function<void()> m_f;
	};

	QString tile_path(int z, int x, int y)
	{
		return QStringLiteral("%1/%2/%3/%4.png").arg(tile_cache::cache_dir()).arg(z).arg(x).arg(y);
	}

	QString tile_url(QString url, int z, int x, int y)
	{
		static const char subdomains[] = { 'a', 'b', 'c' };
		url.replace(QStringLiteral("{z}"), QString::number(z));
		url.replace(QStringLiteral("{x}"), QString::number(x));
		url.replace(QStringLiteral("{y}"), QString::number(y));
		url.replace(QStringLiteral("{s}"), QString(QLatin1Char(subdomains[(x + y) % 3])));
		return url;
	}
}

tile_cache &tile_cache::instance()
{
	// Never deleted: shutdown() stops the worker threads at the end of the program.
	static tile_cache *inst = new tile_cache();
	return *inst;
}

tile_cache::tile_cache()
	: m_cancel(std::make_shared<http_cancel>())
	, m_memory(memory_tiles)
	, m_revision(0)
	, m_shutdown(false)
{
	m_pool.setMaxThreadCount(2); // tile usage policy: maximum two connections
	m_pool.setExpiryTimeout(30000);
}

QString tile_cache::cache_dir()
{
	return app_paths::data_file(QStringLiteral("cache/tiles"));
}

quint64 tile_cache::key(int z, int x, int y)
{
	return (static_cast<quint64>(z) << 58) | (static_cast<quint64>(x) << 29) | static_cast<quint64>(y);
}

QImage tile_cache::cached(int z, int x, int y) const
{
	const QImage *img = m_memory.object(key(z, x, y));
	return img ? *img : QImage();
}

QImage tile_cache::image(int z, int x, int y)
{
	const quint64 k = key(z, x, y);
	if (const QImage *img = m_memory.object(k))
	{
		return *img;
	}
	if (m_shutdown || m_loading.contains(k) || m_loading.size() >= max_loading || z < 0 || z > 22)
	{
		return QImage();
	}
	const auto failed = m_failed.constFind(k);
	if (failed != m_failed.constEnd() && QDateTime::currentMSecsSinceEpoch() - failed.value() < retry_ms)
	{
		return QImage();
	}
	const int n = 1 << z;
	if (x < 0 || y < 0 || x >= n || y >= n)
	{
		return QImage();
	}
	m_loading.insert(k);
	const config_common &cfg = config_common::instance();
	const QString url = tile_url(cfg.value(QStringLiteral("osm/tile_url")), z, x, y);
	const QString agent = cfg.value(QStringLiteral("osm/user_agent"));
	const int days = std::max(1, cfg.int_value(QStringLiteral("render/map_cache_days")));
	const QString path = tile_path(z, x, y);
	const std::shared_ptr<http_cancel> cancel = m_cancel;
	m_pool.start(new task([this, k, url, agent, days, path, cancel]()
	{
		QImage img;
		const QFileInfo fi(path);
		const bool fresh = fi.exists() && fi.lastModified().daysTo(QDateTime::currentDateTime()) < days;
		if (fi.exists())
		{
			img.load(path); // an old tile is better than no tile (no network)
		}
		if (img.isNull() || !fresh)
		{
			const http_response r = http_client::request(url, agent, QByteArray(), QString(), 20000, cancel, 4 * 1024 * 1024);
			QImage net;
			if (r.ok() && net.loadFromData(r.body))
			{
				img = net;
				QDir().mkpath(fi.absolutePath());
				QSaveFile f(path);
				if (f.open(QIODevice::WriteOnly))
				{
					f.write(r.body);
					f.commit();
				}
			}
			else if (!r.cancelled)
			{
				TE_DEBUG("MAP", QStringLiteral("Tile not loaded: %1 (HTTP %2) %3").arg(url).arg(r.status).arg(r.error));
			}
		}
		if (!img.isNull() && img.format() != QImage::Format_RGBA8888)
		{
			img = img.convertToFormat(QImage::Format_RGBA8888);
		}
		QMetaObject::invokeMethod(this, [this, k, img]() { loaded(k, img); }, Qt::QueuedConnection);
	}));
	return QImage();
}

void tile_cache::loaded(quint64 k, const QImage &img)
{
	m_loading.remove(k);
	if (img.isNull())
	{
		m_failed.insert(k, QDateTime::currentMSecsSinceEpoch());
		return;
	}
	m_failed.remove(k);
	m_memory.insert(k, new QImage(img), 1);
	++m_revision;
	emit tile_ready();
}

void tile_cache::clear()
{
	m_memory.clear();
	m_failed.clear();
	QDir(cache_dir()).removeRecursively();
	++m_revision;
	TE_INFO("MAP", QStringLiteral("The map cache is empty now."));
}

void tile_cache::shutdown()
{
	m_shutdown = true;
	m_cancel->cancel();
	m_pool.clear();
	m_pool.waitForDone(3000);
}
