#ifndef WORLD_RENDERER_H
#define WORLD_RENDERER_H

#include "geom.h"
#include "mesh_builder.h"
#include "world_model.h"

#include <QElapsedTimer>
#include <QOpenGLBuffer>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <QPointF>
#include <QString>

#include <memory>
#include <unordered_map>
#include <vector>

class QOpenGLTexture;
class workspace;

// world_renderer
// --------------
// Draws one workspace with OpenGL (GLSL 1.20, desktop OpenGL 2.1 or newer).
//
// Layers, from the bottom:
//   1. ground: a procedural material in the fragment shader (grass, soil,
//      sand, snow, concrete, asphalt). The noise is periodic (256 m), thus
//      the pattern is stable at all positions and zoom levels.
//   1b. map: the map tiles (tile_cache) if the workspace has a geographic
//      origin. A tile that is not loaded yet uses a part of a lower zoom tile.
//   2. static mesh: roads, sidewalks, bike lanes, ditches, junctions, lane
//      markings, stop lines, zebra crossings, turn arrows, rails. The mesh
//      is cached in a VBO and made again only when the workspace changes.
//   3. dynamic mesh: traffic density map (zoomed out: one color line for each
//      road direction, from the mean speed / speed limit, smoothed in time),
//      selection, route, editor preview
//   4. shapes: vehicles, pedestrians, cones (signed distance functions:
//      smooth edges at all zoom levels)
//   5. objects above the traffic: signal heads, barriers, trains
//   6. sign faces (texture atlas)
//
// The QPainter overlays (texts, popups, FPS) are in world_view.

struct camera
{
	vec2 center;
	double ppm = 4.0;   // pixels per meter
	int width = 1;      // logical pixels
	int height = 1;

	QPointF to_screen(const vec2 &w) const
	{
		return QPointF(width / 2.0 + (w.x - center.x) * ppm, height / 2.0 - (w.y - center.y) * ppm);
	}
	vec2 to_world(const QPointF &s) const
	{
		return vec2(center.x + (s.x() - width / 2.0) / ppm, center.y - (s.y() - height / 2.0) / ppm);
	}
	double mpp() const { return 1.0 / ppm; }
};

struct render_options
{
	bool editor_mode = false;
	bool show_grid = false;
	bool show_density = true;
	bool show_congestion = false; // level of service of the junctions and the queues
	double density_ppm = 1.6;    // the density map shows below this zoom (px per m)
	bool show_map = true;        // map tiles (only with a geographic origin)
	double map_opacity = 1.0;
	double map_fade = 0.2;       // 0 = original colors, 1 = white
	int map_max_zoom = 19;
	double dpr = 1.0;            // device pixels per logical pixel
	element_ref selection;
	std::vector<int> selection_roads;  // more selected roads (Ctrl+click): all of them, else empty
	element_ref hover;
	double anim_time = 0.0;
	// Road preview of the road tool
	std::vector<vec2> preview;
	bool preview_snap = false;
	vec2 snap_point;
	// Control points of the selected road (editor mode)
	std::vector<vec2> handles;
};

class world_renderer : protected QOpenGLFunctions
{
public:
	world_renderer();
	~world_renderer();

	// Call in initializeGL(). Returns false if OpenGL is too old.
	bool init(QString *error);
	// Call before the OpenGL context is deleted (current context).
	void release();
	void render(const workspace &ws, const camera &cam, const render_options &opt);

	QString gl_info() const { return m_gl_info; }
	int static_vertices() const { return m_static_count; }
	int dynamic_vertices() const { return m_dynamic_count; }
	// Map of the last frame: drawn (attribution text), zoom level, tiles.
	bool map_drawn() const { return m_map_drawn; }
	int map_zoom() const { return m_map_zoom; }
	int map_tiles() const { return static_cast<int>(m_gl_tiles.size()); }
	// True if the density map was drawn in the last frame (legend).
	bool density_drawn() const { return m_density_drawn; }

private:
	struct gl_tile
	{
		std::unique_ptr<QOpenGLTexture> tex;
		quint64 frame = 0;
	};

	void build_static(const workspace &ws);
	void build_dynamic(const workspace &ws, const camera &cam, const render_options &opt);
	void build_density(const workspace &ws, const camera &cam, const render_options &opt);
	void draw_map(const workspace &ws, const camera &cam, const render_options &opt);
	QOpenGLTexture *tile_texture(int z, int x, int y, bool load, int *uploads);
	void draw_flat(QOpenGLBuffer &buf, int count, const camera &cam, int first = 0);
	void draw_flat_data(const flat_mesh &mesh, const camera &cam);
	void draw_shapes(const shape_batch &batch, const camera &cam);
	void draw_signs(const camera &cam);
	void set_camera(QOpenGLShaderProgram &prog, const camera &cam);
	void update_atlas();

	bool m_ok;
	QString m_gl_info;
	QOpenGLShaderProgram m_ground;
	QOpenGLShaderProgram m_flat;
	QOpenGLShaderProgram m_shape;
	QOpenGLShaderProgram m_tex;
	QOpenGLShaderProgram m_tile;
	QOpenGLBuffer m_quad;
	QOpenGLBuffer m_static;
	QOpenGLBuffer m_static_upper;   // bridges (level > 0): drawn after the vehicles below them
	int m_upper_count = 0;
	int m_cover_count = 0;          // the first vertices of m_static: covered parts of tunnels
	QOpenGLBuffer m_stream;
	std::unique_ptr<QOpenGLTexture> m_atlas;
	quint64 m_atlas_rev;
	quint64 m_static_rev;
	const workspace *m_static_ws;
	int m_static_count;
	int m_dynamic_count;

	flat_mesh m_under;
	flat_mesh m_over;
	shape_batch m_shapes;
	shape_batch m_shapes_upper;     // vehicles on bridges
	shape_batch m_shapes_cover;     // vehicles in tunnels (below the ground roads)
	shape_batch m_shapes_top;
	tex_batch m_signs;

	// map tiles
	std::unordered_map<quint64, gl_tile> m_gl_tiles;
	quint64 m_frame;
	bool m_map_drawn;
	int m_map_zoom;

	// density map: smoothed speed ratio for each road direction (dir_key)
	std::vector<double> m_density_ratio;
	std::vector<double> m_density_occ;
	quint64 m_density_rev;
	const workspace *m_density_ws;
	QElapsedTimer m_density_clock;
	bool m_density_drawn;
};

#endif // WORLD_RENDERER_H
