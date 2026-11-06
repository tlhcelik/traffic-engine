#ifndef TILE_CACHE_H
#define TILE_CACHE_H

#include <QCache>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QThreadPool>

#include <memory>

class http_cancel;

// tile_cache
// ----------
// The map tiles (raster tiles, 256 x 256 PNG, OpenStreetMap by default) for
// the background of the workspaces with a geographic origin.
//
//   memory  the last 600 tiles (QImage)
//   disk    %APPDATA%\traffic_engine\cache\tiles\<z>\<x>\<y>.png. A tile is
//           used for "render/map_cache_days" days, then it is loaded again.
//   network maximum two downloads at the same time (tile usage policy of
//           openstreetmap.org), with the user agent of the settings
//           (osm/user_agent) and the address osm/tile_url.
//
// image() does not block: it returns the tile if it is in the memory. If
// not, it starts a load (disk or network) and returns a null image.
// tile_ready() comes when a tile is available (the views draw again).
// A tile that fails is not tried again for 60 s.

class tile_cache : public QObject
{
	Q_OBJECT

public:
	static tile_cache &instance();

	// The tile, or a null image (the load starts). Call it in the user interface thread.
	QImage image(int z, int x, int y);
	// The tile if it is in the memory, without a load.
	QImage cached(int z, int x, int y) const;
	// Number of tiles that are loading now.
	int pending() const { return m_loading.size(); }
	quint64 revision() const { return m_revision; }
	// Removes all tiles from the memory and the disk.
	void clear();
	// Stops the downloads (end of the program).
	void shutdown();
	static QString cache_dir();
	static quint64 key(int z, int x, int y);

signals:
	void tile_ready();

private:
	tile_cache();
	void loaded(quint64 key, const QImage &img);

	QThreadPool m_pool;
	std::shared_ptr<http_cancel> m_cancel;
	QCache<quint64, QImage> m_memory;
	QSet<quint64> m_loading;
	QHash<quint64, qint64> m_failed;     // key -> time of the failure (ms)
	quint64 m_revision;
	bool m_shutdown;
};

#endif // TILE_CACHE_H
