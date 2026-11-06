#include "world_renderer.h"
#include "app_theme.h"
#include "geo.h"
#include "logger.h"
#include "road_network.h"
#include "sign_catalog.h"
#include "tile_cache.h"
#include "workspace.h"
#include "world_layout.h"

#include <QElapsedTimer>
#include <QImage>
#include <QOpenGLTexture>

#include <algorithm>
#include <cmath>

namespace
{
	// ------------------------------------------------------------------
	// Shaders (GLSL 1.20)
	// ------------------------------------------------------------------
	const char *ground_vs = R"(
#version 120
attribute vec2 a_pos;
uniform vec2 u_cam_local;
uniform vec2 u_scale;
varying vec2 v_local;
void main()
{
	gl_Position = vec4(a_pos, 0.0, 1.0);
	v_local = u_cam_local + a_pos / u_scale;
}
)";

	const char *ground_fs = R"(
#version 120
uniform float u_ground;
uniform float u_mpp;
uniform float u_grid;
varying vec2 v_local;

float hash12(vec2 p)
{
	vec3 p3 = fract(vec3(p.xyx) * 0.1031);
	p3 += dot(p3, p3.yzx + 33.33);
	return fract((p3.x + p3.y) * p3.z);
}

float vnoise(vec2 p, float period)
{
	vec2 i = floor(p);
	vec2 f = fract(p);
	vec2 u = f * f * (3.0 - 2.0 * f);
	float a = hash12(mod(i, period));
	float b = hash12(mod(i + vec2(1.0, 0.0), period));
	float c = hash12(mod(i + vec2(0.0, 1.0), period));
	float d = hash12(mod(i + vec2(1.0, 1.0), period));
	return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

// Noise with a cell size in meters. Period 256 m: the pattern does not move.
float nz(vec2 w, float cell) { return vnoise(w / cell, 256.0 / cell); }
// 1 if a cell is large on the screen, 0 if it is smaller than about 2 pixels
float detail(float cell) { return clamp((cell / u_mpp - 2.0) / 6.0, 0.0, 1.0); }

float grid(vec2 w, float step)
{
	vec2 g = abs(fract(w / step + 0.5) - 0.5) * step;
	float d = min(g.x, g.y);
	return 1.0 - smoothstep(0.0, u_mpp * 1.2, d);
}

void main()
{
	vec2 w = v_local;
	float n1 = mix(0.5, nz(w, 64.0), detail(64.0));
	float n2 = mix(0.5, nz(w + 17.0, 16.0), detail(16.0));
	float n3 = mix(0.5, nz(w + 5.0, 4.0), detail(4.0));
	float n4 = mix(0.5, nz(w, 1.0), detail(1.0));
	float n5 = mix(0.5, nz(w + 3.0, 0.25), detail(0.25));
	vec3 col;
	if (u_ground < 0.5)
	{
		// grass
		vec3 a = vec3(0.58, 0.75, 0.47);
		vec3 b = vec3(0.67, 0.81, 0.54);
		vec3 c = vec3(0.51, 0.69, 0.43);
		col = mix(a, b, smoothstep(0.30, 0.70, n1));
		col = mix(col, c, smoothstep(0.55, 0.85, n2) * 0.5);
		col *= 0.95 + 0.10 * n3;
		col += (n4 - 0.5) * 0.06 + (n5 - 0.5) * 0.07;
	}
	else if (u_ground < 1.5)
	{
		// soil
		vec3 a = vec3(0.71, 0.60, 0.47);
		vec3 b = vec3(0.78, 0.67, 0.53);
		col = mix(a, b, smoothstep(0.3, 0.7, n1));
		col *= 0.93 + 0.12 * n2;
		col += (n4 - 0.5) * 0.07 + (n5 - 0.5) * 0.05;
	}
	else if (u_ground < 2.5)
	{
		// sand
		vec3 a = vec3(0.92, 0.86, 0.69);
		vec3 b = vec3(0.96, 0.91, 0.76);
		col = mix(a, b, smoothstep(0.3, 0.7, n1));
		col *= 0.97 + 0.06 * n2;
		col += (n5 - 0.5) * 0.05;
	}
	else if (u_ground < 3.5)
	{
		// snow
		vec3 a = vec3(0.92, 0.95, 0.99);
		vec3 b = vec3(0.98, 0.99, 1.00);
		col = mix(a, b, smoothstep(0.3, 0.7, n2));
		col -= (1.0 - n1) * 0.03;
		col += (n4 - 0.5) * 0.02;
	}
	else if (u_ground < 4.5)
	{
		// concrete slabs (4 m)
		vec3 a = vec3(0.80, 0.80, 0.78);
		col = a * (0.97 + 0.06 * n2) + (n5 - 0.5) * 0.04;
		vec2 g = abs(fract(w / 4.0 + 0.5) - 0.5) * 4.0;
		float joint = 1.0 - smoothstep(0.0, max(u_mpp * 1.2, 0.03), min(g.x, g.y));
		col -= joint * 0.08 * detail(4.0);
	}
	else
	{
		// asphalt
		vec3 a = vec3(0.42, 0.44, 0.47);
		col = a * (0.96 + 0.08 * n2) + (n5 - 0.5) * 0.06 + (n4 - 0.5) * 0.03;
	}
	if (u_grid > 0.5)
	{
		float f10 = clamp((10.0 / u_mpp - 6.0) / 10.0, 0.0, 1.0);
		float f100 = clamp((100.0 / u_mpp - 6.0) / 10.0, 0.0, 1.0);
		col = mix(col, vec3(1.0), grid(w, 10.0) * 0.16 * f10);
		col = mix(col, vec3(1.0), grid(w, 100.0) * 0.32 * f100);
	}
	gl_FragColor = vec4(clamp(col, 0.0, 1.0), 1.0);
}
)";

	const char *flat_vs = R"(
#version 120
attribute vec2 a_pos;
attribute vec4 a_color;
uniform vec2 u_center;
uniform vec2 u_scale;
varying vec4 v_color;
void main()
{
	gl_Position = vec4((a_pos - u_center) * u_scale, 0.0, 1.0);
	v_color = a_color;
}
)";

	const char *flat_fs = R"(
#version 120
varying vec4 v_color;
void main()
{
	gl_FragColor = v_color;
}
)";

	const char *shape_vs = R"(
#version 120
attribute vec2 a_pos;
attribute vec2 a_local;
attribute vec2 a_half;
attribute vec4 a_color;
attribute vec2 a_kind;
uniform vec2 u_center;
uniform vec2 u_scale;
varying vec2 v_local;
varying vec2 v_half;
varying vec4 v_color;
varying vec2 v_kind;
void main()
{
	gl_Position = vec4((a_pos - u_center) * u_scale, 0.0, 1.0);
	v_local = a_local;
	v_half = a_half;
	v_color = a_color;
	v_kind = a_kind;
}
)";

	// Output: premultiplied alpha (blend: ONE, ONE_MINUS_SRC_ALPHA)
	const char *shape_fs = R"(
#version 120
uniform float u_mpp;
varying vec2 v_local;
varying vec2 v_half;
varying vec4 v_color;
varying vec2 v_kind;

float sd_box(vec2 p, vec2 b, float r)
{
	vec2 q = abs(p) - b + vec2(r);
	return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}

float cov(float d, float aa) { return clamp(0.5 - d / aa, 0.0, 1.0); }

void layer(inout vec4 acc, vec3 c, float a)
{
	acc = vec4(c * a, a) + acc * (1.0 - a);
}

void main()
{
	float aa = max(u_mpp * 1.2, 0.002);
	vec2 p = v_local;
	vec2 h = v_half;
	float kind = v_kind.x;
	float extra = v_kind.y;
	vec3 base = v_color.rgb;
	vec4 acc = vec4(0.0);

	if (kind < 1.5)
	{
		// car (0) or bus (1)
		bool bus = kind > 0.5;
		float r = bus ? h.y * 0.35 : h.y * 0.62;
		float ds = sd_box(p - vec2(-0.20, -0.28), h, r);
		layer(acc, vec3(0.0), (1.0 - smoothstep(-0.30, 0.55, ds)) * 0.26);
		float d = sd_box(p, h, r);
		float body = cov(d, aa);
		float edge = 1.0 - smoothstep(0.0, 0.16, -d);
		vec3 c = mix(base, base * 0.72, edge);
		if (!bus)
		{
			float wf = sd_box(p - vec2(h.x * 0.30, 0.0), vec2(h.x * 0.15, h.y * 0.78), 0.18);
			float wr = sd_box(p - vec2(-h.x * 0.63, 0.0), vec2(h.x * 0.09, h.y * 0.72), 0.14);
			float glass = max(cov(wf, aa), cov(wr, aa));
			c = mix(c, vec3(0.22, 0.27, 0.34), glass * 0.9);
			float roof = sd_box(p - vec2(-h.x * 0.16, 0.0), vec2(h.x * 0.29, h.y * 0.72), 0.3);
			c = mix(c, min(base * 1.10 + 0.06, vec3(1.0)), cov(roof, aa) * 0.55);
		}
		else
		{
			float wb = sd_box(vec2(p.x + h.x * 0.06, abs(p.y) - h.y * 0.80), vec2(h.x * 0.84, h.y * 0.12), 0.05);
			c = mix(c, vec3(0.25, 0.30, 0.38), cov(wb, aa) * 0.85);
			float wf = sd_box(p - vec2(h.x * 0.93, 0.0), vec2(h.x * 0.045, h.y * 0.80), 0.05);
			c = mix(c, vec3(0.22, 0.27, 0.34), cov(wf, aa));
			float unit = sd_box(p - vec2(-h.x * 0.35, 0.0), vec2(h.x * 0.16, h.y * 0.42), 0.12);
			c = mix(c, base * 0.86 + 0.10, cov(unit, aa) * 0.8);
		}
		// lights: front (pale yellow), rear (red; bright when braking)
		float hl = length(vec2(p.x - h.x * 0.94, abs(p.y) - h.y * 0.62)) - min(0.17, h.y * 0.2);
		c = mix(c, vec3(1.0, 0.97, 0.80), cov(hl, aa) * 0.9);
		float tl = length(vec2(p.x + h.x * 0.95, abs(p.y) - h.y * 0.62)) - min(0.16, h.y * 0.2);
		c = mix(c, extra > 0.5 ? vec3(1.0, 0.18, 0.15) : vec3(0.62, 0.16, 0.16), cov(tl, aa));
		layer(acc, c, body * v_color.a);
		if (extra > 0.5)
		{
			float glow = length(vec2(p.x + h.x * 1.05, abs(p.y) - h.y * 0.62));
			layer(acc, vec3(1.0, 0.25, 0.2), (1.0 - smoothstep(0.0, 0.9, glow)) * 0.35);
		}
	}
	else if (kind < 2.5)
	{
		// motorcycle with rider
		float ds = sd_box(p - vec2(-0.12, -0.18), vec2(h.x, h.y * 0.45), h.y * 0.4);
		layer(acc, vec3(0.0), (1.0 - smoothstep(-0.2, 0.4, ds)) * 0.25);
		float d = sd_box(p, vec2(h.x, h.y * 0.42), h.y * 0.38);
		layer(acc, mix(base, base * 0.75, 1.0 - smoothstep(0.0, 0.08, -d)), cov(d, aa));
		float sh = sd_box(p - vec2(-h.x * 0.10, 0.0), vec2(h.x * 0.16, h.y * 0.95), h.y * 0.5);
		layer(acc, vec3(0.30, 0.33, 0.40), cov(sh, aa));
		float helmet = length(p - vec2(-h.x * 0.05, 0.0)) - h.y * 0.55;
		layer(acc, vec3(0.24, 0.25, 0.30), cov(helmet, aa));
		layer(acc, vec3(0.75, 0.80, 0.88), cov(length(p - vec2(h.x * 0.02, h.y * 0.12)) - h.y * 0.18, aa) * 0.7);
	}
	else if (kind < 3.5)
	{
		// pedestrian: shoulders and head
		float ds = length(p - vec2(-0.08, -0.10)) - 0.26;
		layer(acc, vec3(0.0), (1.0 - smoothstep(-0.1, 0.25, ds)) * 0.22);
		float body = length(vec2(p.x * 1.6, p.y)) - 0.26;
		layer(acc, base, cov(body, aa));
		layer(acc, base * 0.75, cov(abs(body + 0.03) - 0.03, aa) * 0.4);
		float head = length(p - vec2(0.02, 0.0)) - 0.12;
		layer(acc, vec3(0.36, 0.27, 0.20), cov(head, aa));
	}
	else if (kind < 4.5)
	{
		// disc; extra = glow
		float d = length(p) - h.x;
		if (extra > 0.0)
		{
			float g = exp(-max(d, 0.0) / max(h.x * 0.9, 0.05));
			layer(acc, base, g * 0.55 * extra * step(0.0, d));
		}
		layer(acc, base, cov(d, aa) * v_color.a);
		float hl = length(p - vec2(-h.x * 0.25, h.x * 0.30)) - h.x * 0.30;
		layer(acc, vec3(1.0), cov(hl, aa) * 0.25 * v_color.a);
	}
	else if (kind < 6.5)
	{
		// traffic cone (top view)
		float ds = length(p - vec2(-0.08, -0.10)) - h.x;
		layer(acc, vec3(0.0), (1.0 - smoothstep(-0.1, 0.25, ds)) * 0.25);
		float d = length(p) - h.x;
		layer(acc, vec3(0.98, 0.56, 0.26), cov(d, aa));
		float ring = abs(length(p) - h.x * 0.58) - h.x * 0.13;
		layer(acc, vec3(1.0), cov(ring, aa));
		layer(acc, vec3(0.93, 0.44, 0.20), cov(length(p) - h.x * 0.24, aa));
	}
	else if (kind < 7.5)
	{
		// ring: radius h.x, width h.y
		float d = abs(length(p) - h.x) - h.y * 0.5;
		layer(acc, base, cov(d, aa) * v_color.a);
	}
	else
	{
		// rounded rectangle with a darker edge; extra = corner radius
		float ds = sd_box(p - vec2(-0.10, -0.14), h, extra);
		layer(acc, vec3(0.0), (1.0 - smoothstep(-0.1, 0.3, ds)) * 0.18 * v_color.a);
		float d = sd_box(p, h, extra);
		layer(acc, base * 0.78, cov(d, aa) * v_color.a);
		layer(acc, base, cov(d + min(0.10, h.y * 0.25), aa) * v_color.a);
	}
	gl_FragColor = acc;
}
)";

	const char *tex_vs = R"(
#version 120
attribute vec2 a_pos;
attribute vec2 a_uv;
attribute float a_alpha;
uniform vec2 u_center;
uniform vec2 u_scale;
varying vec2 v_uv;
varying float v_alpha;
void main()
{
	gl_Position = vec4((a_pos - u_center) * u_scale, 0.0, 1.0);
	v_uv = a_uv;
	v_alpha = a_alpha;
}
)";

	const char *tex_fs = R"(
#version 120
uniform sampler2D u_tex;
varying vec2 v_uv;
varying float v_alpha;
void main()
{
	vec4 c = texture2D(u_tex, v_uv);
	gl_FragColor = vec4(c.rgb * c.a, c.a) * v_alpha;
}
)";

	// Map tiles: a little less color and lighter (pastel), premultiplied alpha.
	const char *tile_fs = R"(
#version 120
uniform sampler2D u_tex;
uniform float u_fade;
varying vec2 v_uv;
varying float v_alpha;
void main()
{
	vec3 c = texture2D(u_tex, v_uv).rgb;
	float g = dot(c, vec3(0.299, 0.587, 0.114));
	c = mix(c, vec3(g), 0.3);
	c = mix(c, vec3(1.0), u_fade);
	gl_FragColor = vec4(c * v_alpha, v_alpha);
}
)";

	bool build_program(QOpenGLShaderProgram &prog, const char *vs, const char *fs, const char *name, QString *error)
	{
		if (!prog.addShaderFromSourceCode(QOpenGLShader::Vertex, vs)
			|| !prog.addShaderFromSourceCode(QOpenGLShader::Fragment, fs)
			|| !prog.link())
		{
			if (error)
			{
				*error = QStringLiteral("Shader %1: %2").arg(QString::fromLatin1(name), prog.log());
			}
			return false;
		}
		return true;
	}

	QColor mix_color(const QColor &a, const QColor &b, double t)
	{
		t = std::max(0.0, std::min(1.0, t));
		return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
			a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
	}

	QColor with_alpha(QColor c, double a)
	{
		c.setAlphaF(std::max(0.0, std::min(1.0, a)));
		return c;
	}

	// Lane arrow marking at point p (t: travel direction, n: left)
	void lane_arrow(flat_mesh &m, const vec2 &p, const vec2 &t, bool left, bool straight, bool right, const QColor &col)
	{
		const vec2 n = t.left();
		m.rect(p - t * 1.5, t, 1.5, 0.12, col);
		if (straight || (!left && !right))
		{
			m.rect(p + t * 0.5, t, 0.5, 0.12, col);
			m.tri(p + t * 1.0 + n * 0.45, p + t * 1.0 - n * 0.45, p + t * 2.2, col);
		}
		auto branch = [&](double side)
		{
			const vec2 a = p - t * 0.2;
			const vec2 b = p + t * 0.6 + n * (0.9 * side);
			m.segment(a, b, 0.24, col);
			const vec2 dir = (b - a).normalized();
			const vec2 bn = dir.left();
			m.tri(b + bn * 0.45, b - bn * 0.45, b + dir * 1.0, col);
		};
		if (left)
		{
			branch(1.0);
		}
		if (right)
		{
			branch(-1.0);
		}
	}

	QRectF atlas_uv(int cell, const sign_catalog &cat)
	{
		const QImage &img = cat.atlas();
		const int cols = cat.atlas_columns();
		const int size = cat.atlas_cell();
		const double w = img.width();
		const double h = img.height();
		const double x = (cell % cols) * size;
		const double y = (cell / cols) * size;
		return QRectF((x + 0.5) / w, (y + 0.5) / h, (size - 1.0) / w, (size - 1.0) / h);
	}
}

world_renderer::world_renderer()
	: m_ok(false)
	, m_quad(QOpenGLBuffer::VertexBuffer)
	, m_static(QOpenGLBuffer::VertexBuffer)
	, m_stream(QOpenGLBuffer::VertexBuffer)
	, m_atlas_rev(0)
	, m_static_rev(~0ULL)
	, m_static_ws(nullptr)
	, m_static_count(0)
	, m_dynamic_count(0)
	, m_frame(0)
	, m_map_drawn(false)
	, m_map_zoom(0)
	, m_density_rev(~0ULL)
	, m_density_ws(nullptr)
	, m_density_drawn(false)
{
	m_density_clock.start();
}

world_renderer::~world_renderer() = default;

bool world_renderer::init(QString *error)
{
	initializeOpenGLFunctions();
	const char *vendor = reinterpret_cast<const char *>(glGetString(GL_VENDOR));
	const char *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
	const char *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
	m_gl_info = QStringLiteral("%1 | %2 | OpenGL %3")
		.arg(QString::fromLatin1(vendor ? vendor : "?"), QString::fromLatin1(renderer ? renderer : "?"), QString::fromLatin1(version ? version : "?"));

	QString err;
	if (!build_program(m_ground, ground_vs, ground_fs, "ground", &err)
		|| !build_program(m_flat, flat_vs, flat_fs, "flat", &err)
		|| !build_program(m_shape, shape_vs, shape_fs, "shape", &err)
		|| !build_program(m_tex, tex_vs, tex_fs, "sign", &err)
		|| !build_program(m_tile, tex_vs, tile_fs, "tile", &err))
	{
		TE_ERROR("RENDER", err);
		if (error)
		{
			*error = err;
		}
		return false;
	}
	const float quad[] = { -1.f, -1.f, 1.f, -1.f, 1.f, 1.f, -1.f, -1.f, 1.f, 1.f, -1.f, 1.f };
	m_quad.create();
	m_quad.bind();
	m_quad.allocate(quad, sizeof(quad));
	m_quad.release();
	m_static.create();
	m_static.setUsagePattern(QOpenGLBuffer::StaticDraw);
	m_static_upper.create();
	m_static_upper.setUsagePattern(QOpenGLBuffer::StaticDraw);
	m_stream.create();
	m_stream.setUsagePattern(QOpenGLBuffer::StreamDraw);
	m_ok = true;
	TE_INFO("RENDER", QStringLiteral("OpenGL is ready: %1").arg(m_gl_info));
	return true;
}

void world_renderer::release()
{
	m_atlas.reset();
	m_gl_tiles.clear();
	m_tile.removeAllShaders();
	m_quad.destroy();
	m_static.destroy();
	m_static_upper.destroy();
	m_stream.destroy();
	m_ground.removeAllShaders();
	m_flat.removeAllShaders();
	m_shape.removeAllShaders();
	m_tex.removeAllShaders();
	m_ok = false;
}

void world_renderer::set_camera(QOpenGLShaderProgram &prog, const camera &cam)
{
	prog.setUniformValue("u_center", static_cast<float>(cam.center.x), static_cast<float>(cam.center.y));
	prog.setUniformValue("u_scale", static_cast<float>(2.0 * cam.ppm / cam.width), static_cast<float>(2.0 * cam.ppm / cam.height));
}

void world_renderer::update_atlas()
{
	sign_catalog &cat = sign_catalog::instance();
	if (m_atlas && m_atlas_rev == cat.atlas_revision())
	{
		return;
	}
	m_atlas.reset(new QOpenGLTexture(cat.atlas(), QOpenGLTexture::GenerateMipMaps));
	m_atlas->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
	m_atlas->setMagnificationFilter(QOpenGLTexture::Linear);
	m_atlas->setWrapMode(QOpenGLTexture::ClampToEdge);
	m_atlas_rev = cat.atlas_revision();
}

// ---------------------------------------------------------------------------
// static mesh
// ---------------------------------------------------------------------------
void world_renderer::build_static(const workspace &ws)
{
	QElapsedTimer timer;
	timer.start();
	const world_model &m = ws.model();
	const road_network &net = ws.network();
	flat_mesh ditch;
	flat_mesh side;
	flat_mesh bike;
	flat_mesh asphalt;
	flat_mesh median;
	flat_mesh marks;
	flat_mesh rails;

	QColor c_ditch(118, 146, 98);
	QColor c_side(222, 222, 214);
	QColor c_curb(186, 186, 180);
	QColor c_bike(214, 150, 136);
	QColor c_asphalt(98, 103, 112);
	QColor c_median(150, 190, 125);
	QColor c_mark(246, 246, 240, 235);
	const QColor c_yellow(248, 206, 84, 240);
	const QColor c_rail(70, 72, 78);
	const QColor c_sleeper(150, 120, 90);
	QColor c_guard(190, 196, 204);
	// Bridges and tunnels (road_network::build_levels): the ramps at the ends
	// are ground roads. The deck of a bridge goes to a second mesh, drawn after
	// the vehicles below it. The covered part of a tunnel is pale and goes to
	// a first mesh, drawn before the ground roads: the roads above stay clear.
	flat_mesh up_shadow;
	flat_mesh up_side;
	flat_mesh up_asphalt;
	flat_mesh up_marks;
	flat_mesh under;
	const QColor c_wall(150, 150, 144);
	const QColor c_portal(200, 196, 186);
	const QColor c_portal_edge(108, 106, 100);
	const QColor c_slope(128, 142, 98);
	const QColor c_slope_edge(100, 112, 78);
	const QColor c_parapet(176, 180, 188);
	const QColor c_parapet_edge(58, 62, 70);
	const QColor c_joint(78, 82, 90);
	const QColor c_hidden(64, 68, 76, 150);

	// One part of a road: mode 0 ground, 1 bridge deck, -1 covered part of a tunnel
	auto draw_part = [&](const net_road &r, const road_data *rd, const polyline &t, int mode)
	{
		auto col = [mode](QColor c)
		{
			if (mode < 0)
			{
				c.setAlpha(c.alpha() * 45 / 100); // under the ground: pale
			}
			return c;
		};
		flat_mesh &m_side = mode > 0 ? up_side : (mode < 0 ? under : side);
		flat_mesh &m_bike = mode > 0 ? up_side : (mode < 0 ? under : bike);
		flat_mesh &m_asphalt = mode > 0 ? up_asphalt : (mode < 0 ? under : asphalt);
		flat_mesh &m_median = mode > 0 ? up_side : (mode < 0 ? under : median);
		flat_mesh &m_marks = mode > 0 ? up_marks : (mode < 0 ? under : marks);
		const double el = r.edge_left;
		const double er = r.edge_right;
		const double bl = el + r.bike_width;
		const double br = er - r.bike_width;
		if (r.ditch_width > 0.0 && mode == 0)
		{
			ditch.band(t, r.outer_left, r.outer_left - r.ditch_width, c_ditch);
			ditch.band(t, r.outer_right + r.ditch_width, r.outer_right, c_ditch);
		}
		if (r.sidewalk_left)
		{
			m_side.band(t, bl + r.sidewalk_width, bl, col(c_side));
			m_side.band(t, bl + 0.18, bl, col(c_curb));
		}
		if (r.sidewalk_right)
		{
			m_side.band(t, br, br - r.sidewalk_width, col(c_side));
			m_side.band(t, br, br - 0.18, col(c_curb));
		}
		if (r.bike_width > 0.0)
		{
			m_bike.band(t, bl, el, col(c_bike));
			m_bike.band(t, er, br, col(c_bike));
			m_marks.band(t, el + 0.08, el - 0.08, col(c_mark));
			m_marks.band(t, er + 0.08, er - 0.08, col(c_mark));
		}
		m_asphalt.band(t, el, er, col(c_asphalt));
		if (r.median_width > 0.0)
		{
			m_median.band(t, r.median_width / 2.0, -r.median_width / 2.0, col(c_median));
			m_median.band(t, r.median_width / 2.0, r.median_width / 2.0 - 0.18, col(c_curb));
			m_median.band(t, -r.median_width / 2.0 + 0.18, -r.median_width / 2.0, col(c_curb));
		}

		// Markings
		const double lw = std::max(2.0, rd->lane_width);
		m_marks.band(t, el - 0.25, el - 0.40, col(c_mark));
		m_marks.band(t, er + 0.40, er + 0.25, col(c_mark));
		const int lf = static_cast<int>(r.lanes_fwd.size());
		const int lb = static_cast<int>(r.lanes_bwd.size());
		const bool two_way = lf > 0 && lb > 0;
		if (two_way && r.median_width <= 0.0)
		{
			if (rd->overtaking)
			{
				m_marks.dashes(t, 0.0, 0.15, 3.0, 5.0, col(c_mark));
			}
			else
			{
				m_marks.band(t, 0.21, 0.07, col(c_mark));
				m_marks.band(t, -0.07, -0.21, col(c_mark));
			}
		}
		auto dividers = [&](int count, double start)
		{
			for (int i = 1; i < count; ++i)
			{
				const double lat = start + i * lw;
				if (rd->overtaking)
				{
					m_marks.dashes(t, lat, 0.13, 3.0, 5.0, col(c_mark));
				}
				else
				{
					m_marks.band(t, lat + 0.065, lat - 0.065, col(c_mark));
				}
			}
		};
		if (two_way)
		{
			dividers(lf, -(r.median_width / 2.0 + lf * lw));
			dividers(lb, r.median_width / 2.0);
		}
		else
		{
			const int l = lf + lb;
			dividers(l, -(l * lw) / 2.0);
		}
		if (rd->guard_rail && mode == 0)
		{
			rails.band(t, r.outer_left - 0.10, r.outer_left - 0.35, c_guard);
			rails.band(t, r.outer_right + 0.35, r.outer_right + 0.10, c_guard);
		}
	};

	for (const auto &kv : net.roads)
	{
		const net_road &r = kv.second;
		const road_data *rd = m.road(r.road_id);
		const polyline &t = r.trimmed;
		if (t.empty() || !rd)
		{
			continue;
		}
		if (r.level == 0)
		{
			draw_part(r, rd, t, 0);
			continue;
		}
		const double len = t.length();
		const double s0 = r.level_s0;
		const double s1 = r.level_s1;
		const bool ramp_a = s0 > 0.05;
		const bool ramp_b = s1 < len - 0.05;
		const double wl = r.outer_left;
		const double wr = r.outer_right;
		if (ramp_a)
		{
			draw_part(r, rd, t.sub(0.0, s0), 0);
		}
		if (ramp_b)
		{
			draw_part(r, rd, t.sub(s1, len), 0);
		}
		if (s1 - s0 > 0.05)
		{
			draw_part(r, rd, t.sub(s0, s1), r.level > 0 ? 1 : -1);
		}
		if (r.level < 0)
		{
			// Tunnel. The covered part: dashed edges (the road is under the ground).
			if (s1 - s0 > 0.05)
			{
				const polyline cover = t.sub(s0, s1);
				under.dashes(cover, wl - 0.15, 0.30, 2.0, 2.0, c_hidden);
				under.dashes(cover, wr + 0.15, 0.30, 2.0, 2.0, c_hidden);
			}
			// The ramps: an open cut with walls, darker to the portal
			auto ramp = [&](double a, double b, bool portal_at_b)
			{
				const polyline cut = t.sub(a, b);
				side.band(cut, wl + 0.6, wl, c_wall);
				side.band(cut, wr, wr - 0.6, c_wall);
				for (int k = 0; k < 3; ++k)
				{
					const double x0 = portal_at_b ? a + (b - a) * k / 3.0 : b - (b - a) * (k + 1) / 3.0;
					const double x1 = portal_at_b ? a + (b - a) * (k + 1) / 3.0 : b - (b - a) * k / 3.0;
					rails.band(t.sub(x0, x1), r.edge_left, r.edge_right, QColor(18, 22, 30, 20 + 22 * k));
				}
			};
			// The portal: a dark opening and a concrete frame over the road (drawn
			// after the vehicles: they go under it)
			auto portal = [&](double s, bool cover_after)
			{
				const double m0 = cover_after ? s : std::max(0.0, s - 2.5);
				const double m1 = cover_after ? std::min(len, s + 2.5) : s;
				rails.band(t.sub(m0, m1), r.edge_left, r.edge_right, QColor(12, 14, 20, 170));
				up_side.band(t.sub(std::max(0.0, s - 0.9), std::min(len, s + 0.9)), wl + 1.6, wr - 1.6, c_portal);
				const double f0 = cover_after ? s + 0.6 : s - 0.9;
				const double f1 = cover_after ? s + 0.9 : s - 0.6;
				up_side.band(t.sub(std::max(0.0, f0), std::min(len, f1)), wl + 1.6, wr - 1.6, c_portal_edge);
			};
			if (ramp_a)
			{
				ramp(0.0, s0, true);
				portal(s0, true);
			}
			if (ramp_b)
			{
				ramp(s1, len, false);
				portal(s1, false);
			}
		}
		else
		{
			// Bridge. The ramps: earth embankments with slopes.
			auto embankment = [&](double a, double b)
			{
				const polyline e = t.sub(a, b);
				ditch.band(e, wl + 2.0, wl, c_slope);
				ditch.band(e, wl + 2.0, wl + 1.75, c_slope_edge);
				ditch.band(e, wr, wr - 2.0, c_slope);
				ditch.band(e, wr - 1.75, wr - 2.0, c_slope_edge);
			};
			if (ramp_a)
			{
				embankment(0.0, s0);
			}
			if (ramp_b)
			{
				embankment(s1, len);
			}
			if (s1 - s0 > 0.05)
			{
				const polyline deck = t.sub(s0, s1);
				// Shadow on the ground and on the roads below it (light from the north-west)
				std::vector<vec2> shadow;
				for (const vec2 &p : deck.points())
				{
					shadow.push_back(p + vec2(2.2, -3.0));
				}
				up_shadow.band(polyline(shadow), wl + 0.6, wr - 0.6, QColor(14, 18, 26, 110));
				// Parapets with a dark outer edge
				up_side.band(deck, wl + 0.6, wl, c_parapet);
				up_side.band(deck, wl + 0.6, wl + 0.42, c_parapet_edge);
				up_side.band(deck, wr, wr - 0.6, c_parapet);
				up_side.band(deck, wr - 0.42, wr - 0.6, c_parapet_edge);
				// Joints at the ends of the deck (the abutments)
				for (const double s : { s0, s1 })
				{
					if (s > 0.05 && s < len - 0.05)
					{
						up_marks.band(t.sub(std::max(0.0, s - 0.2), std::min(len, s + 0.2)), wl + 0.6, wr - 0.6, c_joint);
					}
				}
			}
		}
	}

	// Junction surfaces and sidewalk corners
	for (const auto &kv : net.junctions)
	{
		const net_junction &j = kv.second;
		if (j.arms.size() < 2 || j.polygon.size() < 3)
		{
			continue;
		}
		asphalt.fan(j.polygon, c_asphalt);
		const size_t n = j.arms.size();
		for (size_t k = 0; k < n; ++k)
		{
			const net_arm &a1 = j.arms[k];
			const net_arm &a2 = j.arms[(k + 1) % n];
			const net_road &r1 = net.roads.at(a1.road_id);
			const net_road &r2 = net.roads.at(a2.road_id);
			const bool has1 = a1.at_a ? r1.sidewalk_left : r1.sidewalk_right;
			const bool has2 = a2.at_a ? r2.sidewalk_right : r2.sidewalk_left;
			if (!has1 || !has2)
			{
				continue;
			}
			vec2 p1;
			vec2 t1;
			vec2 p2;
			vec2 t2;
			r1.center.sample(a1.at_a ? r1.trim_a : r1.center.length() - r1.trim_b, &p1, &t1);
			r2.center.sample(a2.at_a ? r2.trim_a : r2.center.length() - r2.trim_b, &p2, &t2);
			const vec2 u1 = a1.at_a ? t1 : -t1;
			const vec2 u2 = a2.at_a ? t2 : -t2;
			const double e1 = (a1.at_a ? r1.edge_left : -r1.edge_right) + r1.bike_width;
			const double e2 = (a2.at_a ? -r2.edge_right : r2.edge_left) + r2.bike_width;
			const vec2 in1 = p1 + u1.left() * e1;
			const vec2 out1 = p1 + u1.left() * (e1 + r1.sidewalk_width);
			const vec2 in2 = p2 + u2.right() * e2;
			const vec2 out2 = p2 + u2.right() * (e2 + r2.sidewalk_width);
			side.quad(in1, out1, out2, in2, c_side);
			// rounded outer corner: a small bulge away from the junction
			const vec2 mid = (out1 + out2) * 0.5;
			const vec2 bulge = mid + (mid - j.pos).normalized() * (distance(out1, out2) * 0.22);
			side.tri(out1, bulge, out2, c_side);
		}
	}

	// Stop lines, yield lines and lane arrows
	for (const net_lane &l : net.lanes)
	{
		if (l.dead_end)
		{
			continue;
		}
		const net_junction *j = net.junction(l.to_node);
		if (!j || j->arms.size() < 2)
		{
			continue;
		}
		const bool control = j->arms.size() >= 3 || j->light_id >= 0 || l.stop_sign || l.yield_sign;
		if (!control)
		{
			continue;
		}
		if (l.yield_sign && j->light_id < 0)
		{
			vec2 p;
			vec2 t;
			l.path.sample(l.stop_line + 0.4, &p, &t);
			const vec2 n = t.left();
			for (double y = -l.width / 2.0 + 0.45; y <= l.width / 2.0 - 0.45; y += 0.9)
			{
				const vec2 c = p + n * y;
				marks.tri(c + n * 0.3, c - n * 0.3, c - t * 0.55, c_mark);
			}
		}
		else
		{
			const double s0 = std::max(0.0, l.stop_line + 0.15);
			const double s1 = std::min(l.length(), l.stop_line + 0.60);
			if (s1 > s0)
			{
				marks.band(l.path.sub(s0, s1), l.width / 2.0 - 0.05, -(l.width / 2.0 - 0.05), c_mark);
			}
		}
		if (l.length() > 28.0 && !l.out.empty())
		{
			bool left = false;
			bool straight = false;
			bool right = false;
			for (int cid : l.out)
			{
				const net_connector &c = net.connectors[cid];
				if (!c.allowed)
				{
					continue;
				}
				left = left || c.turn == turn_kind::left;
				straight = straight || c.turn == turn_kind::straight;
				right = right || c.turn == turn_kind::right;
			}
			vec2 p;
			vec2 t;
			l.path.sample(l.stop_line - 9.0, &p, &t);
			lane_arrow(marks, p, t, left, straight, right, c_mark);
		}
	}

	// Zebra crossings
	for (const net_crosswalk &cw : net.crosswalks)
	{
		if (cw.polygon.size() != 4)
		{
			continue;
		}
		const vec2 a0 = cw.polygon[0];
		const vec2 a1 = cw.polygon[1];
		const vec2 b0 = cw.polygon[3];
		const vec2 along = a1 - a0;
		const vec2 across = b0 - a0;
		const double width = across.length();
		if (width < 1.0)
		{
			continue;
		}
		const vec2 nrm = across / width;
		const vec2 start = a0 + along * 0.5;
		for (double y = 0.35; y + 0.5 <= width - 0.2; y += 1.0)
		{
			marks.rect(start + nrm * (y + 0.25), along.normalized(), along.length() / 2.0 - 0.1, 0.25, c_mark);
		}
	}

	// Objects with markings: bus stops, level crossings
	for (const auto &kv : m.objects)
	{
		const object_data &o = kv.second;
		const net_road *r = net.road(o.road_id);
		if (!r)
		{
			continue;
		}
		if (o.kind == object_kind::bus_stop)
		{
			const std::vector<int> &ls = net.lanes_of(o.road_id, o.dir > 0 ? 1 : -1);
			if (ls.empty())
			{
				continue;
			}
			const net_lane &l = net.lanes[ls.front()];
			const double s = net.lane_s_from_road_s(l, o.s);
			const polyline box = l.path.sub(s - 7.5, s + 7.5);
			const double hw = l.width / 2.0 - 0.25;
			marks.band(box, hw, hw - 0.15, c_yellow);
			marks.band(box, -hw + 0.15, -hw, c_yellow);
			for (double e : { s - 7.5, s + 7.5 })
			{
				vec2 p;
				vec2 t;
				l.path.sample(e, &p, &t);
				marks.segment(p + t.left() * hw, p - t.left() * hw, 0.15, c_yellow);
			}
			// zigzag
			vec2 prev = l.path.point_at(s - 7.0) + l.path.tangent_at(s - 7.0).left() * (hw - 0.4);
			int k = 0;
			for (double z = s - 6.0; z <= s + 7.0; z += 1.0, ++k)
			{
				const double side_off = (k % 2 == 0) ? -(hw - 0.4) : (hw - 0.4);
				const vec2 next = l.path.point_at(z) + l.path.tangent_at(z).left() * side_off;
				marks.segment(prev, next, 0.12, c_yellow);
				prev = next;
			}
		}
		else if (o.kind == object_kind::level_crossing)
		{
			vec2 c;
			vec2 t;
			r->center.sample(o.s, &c, &t);
			const vec2 n = t.left();
			const double a = r->outer_right - 30.0;
			const double b = r->outer_left + 30.0;
			for (double y = a; y <= b; y += 0.65)
			{
				if (y > r->edge_right - 0.2 && y < r->edge_left + 0.2)
				{
					continue; // no sleepers in the road surface
				}
				rails.rect(c + n * y, t, 1.3, 0.11, c_sleeper);
			}
			rails.segment(c + n * a + t * 0.72, c + n * b + t * 0.72, 0.12, c_rail);
			rails.segment(c + n * a - t * 0.72, c + n * b - t * 0.72, 0.12, c_rail);
		}
	}

	flat_mesh all;
	all.v.reserve(under.v.size() + ditch.v.size() + side.v.size() + bike.v.size() + asphalt.v.size() + median.v.size() + marks.v.size() + rails.v.size());
	for (const flat_mesh *part : { &under, &ditch, &side, &bike, &asphalt, &median, &marks, &rails })
	{
		all.v.insert(all.v.end(), part->v.begin(), part->v.end());
	}
	m_static.bind();
	m_static.allocate(all.v.empty() ? nullptr : all.v.data(), static_cast<int>(all.v.size() * sizeof(flat_vertex)));
	m_static.release();
	m_static_count = static_cast<int>(all.v.size());
	m_cover_count = static_cast<int>(under.v.size());
	flat_mesh upper;
	for (const flat_mesh *part : { &up_shadow, &up_side, &up_asphalt, &up_marks })
	{
		upper.v.insert(upper.v.end(), part->v.begin(), part->v.end());
	}
	m_static_upper.bind();
	m_static_upper.allocate(upper.v.empty() ? nullptr : upper.v.data(), static_cast<int>(upper.v.size() * sizeof(flat_vertex)));
	m_static_upper.release();
	m_upper_count = static_cast<int>(upper.v.size());
	m_static_rev = ws.view_revision();
	m_static_ws = &ws;
	TE_DEBUG("RENDER", QStringLiteral("Static mesh: %1 vertices in %2 ms").arg(m_static_count).arg(timer.elapsed()));
}

// ---------------------------------------------------------------------------
// dynamic content
// ---------------------------------------------------------------------------
void world_renderer::build_dynamic(const workspace &ws, const camera &cam, const render_options &opt)
{
	m_under.clear();
	m_over.clear();
	m_shapes.clear();
	m_shapes_top.clear();
	m_signs.clear();

	const world_model &m = ws.model();
	const road_network &net = ws.network();
	const simulation &sim = ws.sim();
	const double mpp = cam.mpp();
	const vec2 lo = cam.to_world(QPointF(0, cam.height));
	const vec2 hi = cam.to_world(QPointF(cam.width, 0));
	auto visible = [&](const vec2 &p, double r)
	{
		return p.x + r >= lo.x && p.x - r <= hi.x && p.y + r >= lo.y && p.y - r <= hi.y;
	};
	const QColor accent(120, 170, 225);
	const QColor accent_fill(167, 199, 231, 95);

	// --- traffic density map (zoomed out) ---
	build_density(ws, cam, opt);

	const double ring_w = std::max(0.25, 2.0 * mpp);
	// --- congestion analysis: queues on the approaches, level of service of the junctions ---
	if (opt.show_congestion)
	{
		const road_network &cn = ws.network();
		for (const junction_status &js : ws.sim().congestion())
		{
			const net_junction *j = cn.junction(js.node_id);
			if (!j || !visible(j->pos, j->radius + 300.0))
			{
				continue;
			}
			for (const approach_status &a : js.approaches)
			{
				if (a.queue_m < 3.0)
				{
					continue;
				}
				const char los = a.spillback ? 'F' : simulation::los_letter(a.delay, js.signalized);
				QColor qc = app_theme::los_color(std::max('C', los));
				qc.setAlpha(a.spillback ? 210 : 170);
				for (const net_arm &arm : j->arms)
				{
					if (arm.road_id != a.road_id)
					{
						continue;
					}
					for (int lid : cn.lanes_of(a.road_id, arm.at_a ? -1 : 1))
					{
						const net_lane &l = cn.lanes[lid];
						const double s1 = l.stop_line;
						const double s0 = std::max(0.0, s1 - a.queue_m);
						if (s1 - s0 < 1.0)
						{
							continue;
						}
						const polyline part = l.path.sub(s0, s1);
						m_under.thick_line(part.points(), std::max(l.width * 0.75, 4.0 * mpp), qc);
					}
				}
			}
			if (js.los >= 'C' || js.spillback)
			{
				QColor fill = app_theme::los_color(js.los);
				fill.setAlpha(js.los >= 'E' ? 120 : 80);
				const double r = std::max(j->radius + 4.0, 10.0 * mpp);
				m_shapes_top.add(j->pos, vec2(1, 0), r, r, 0.1, fill, shape_batch::k_disc);
				m_shapes_top.add(j->pos, vec2(1, 0), r, std::max(0.4, 2.5 * mpp), 0.2, app_theme::los_color(js.los), shape_batch::k_ring);
			}
		}
	}

	// --- selection highlight ---
	for (int rid : opt.selection_roads)
	{
		// More selected roads (the main road is in the switch below)
		const net_road *r = net.road(rid);
		if (r && rid != opt.selection.id)
		{
			m_under.band(r->center, r->outer_left + 0.6, r->outer_right - 0.6, accent_fill);
		}
	}
	const element_ref &sel = opt.selection;
	switch (sel.kind)
	{
		case element_kind::road:
		{
			const net_road *r = net.road(sel.id);
			if (r)
			{
				m_under.band(r->center, r->outer_left + 0.6, r->outer_right - 0.6, accent_fill);
			}
			break;
		}
		case element_kind::node:
		{
			const node_data *n = m.node(sel.id);
			if (n)
			{
				m_shapes_top.add(n->pos, vec2(1, 0), std::max(3.5, 12.0 * mpp), ring_w, ring_w, accent, shape_batch::k_ring);
			}
			break;
		}
		case element_kind::light:
		{
			const light_data *l = m.light(sel.id);
			const net_junction *j = l ? net.junction(l->node_id) : nullptr;
			if (j)
			{
				if (j->polygon.size() >= 3)
				{
					m_under.fan(j->polygon, accent_fill);
				}
				m_shapes_top.add(j->pos, vec2(1, 0), j->radius + 3.0, ring_w, ring_w, accent, shape_batch::k_ring);
			}
			break;
		}
		case element_kind::sign:
		case element_kind::object:
		{
			vec2 pos;
			vec2 dir;
			double hl = 1.0;
			bool ok = false;
			if (sel.kind == element_kind::sign && m.sign(sel.id))
			{
				ok = world_layout::sign_pose(m, net, *m.sign(sel.id), &pos, &dir);
				hl = 1.2;
			}
			else if (sel.kind == element_kind::object && m.object(sel.id))
			{
				ok = world_layout::object_pose(m, net, *m.object(sel.id), &pos, &dir, &hl);
			}
			if (ok)
			{
				m_shapes_top.add(pos, vec2(1, 0), std::max(2.0, std::min(hl, 12.0) + 1.0), ring_w, ring_w, accent, shape_batch::k_ring);
			}
			break;
		}
		case element_kind::vehicle:
		{
			const sim_vehicle *v = sim.find_vehicle(sel.id);
			if (v)
			{
				const std::vector<vec2> route = sim.route_line(v->id);
				m_under.thick_line(route, std::max(0.8, 3.0 * mpp), QColor(240, 130, 120, 170));
				m_shapes_top.add(v->pos, vec2(1, 0), v->p->length / 2.0 + 1.5, ring_w, ring_w, accent, shape_batch::k_ring);
			}
			break;
		}
		case element_kind::pedestrian:
		{
			const sim_pedestrian *p = sim.find_pedestrian(sel.id);
			if (p)
			{
				std::vector<vec2> pts;
				pts.push_back(p->pos);
				for (size_t k = static_cast<size_t>(p->leg_index); k < p->legs.size(); ++k)
				{
					const std::vector<vec2> &lp = p->legs[k].path.points();
					pts.insert(pts.end(), lp.begin(), lp.end());
				}
				m_under.thick_line(pts, std::max(0.4, 2.0 * mpp), QColor(240, 130, 120, 170));
				m_shapes_top.add(p->pos, vec2(1, 0), 1.2, ring_w, ring_w, accent, shape_batch::k_ring);
			}
			break;
		}
		default:
			break;
	}

	// --- editor preview of a new road ---
	if (opt.preview.size() >= 2)
	{
		const std::vector<vec2> pts = geom::smooth_curve(opt.preview, 1.0);
		m_under.thick_line(pts, 7.0, QColor(98, 103, 112, 150));
		m_under.dashes(polyline(pts), 0.0, std::max(0.2, 1.5 * mpp), 3.0, 3.0, QColor(255, 255, 255, 210));
	}
	if (opt.preview_snap)
	{
		m_shapes_top.add(opt.snap_point, vec2(1, 0), std::max(2.0, 9.0 * mpp), ring_w, ring_w, QColor(110, 200, 140), shape_batch::k_ring);
	}

	// --- vehicles --- (on a bridge: drawn after the bridge; in a tunnel: pale)
	m_shapes_upper.clear();
	m_shapes_cover.clear();
	const road_network &rn = ws.network();
	for (const sim_vehicle &v : sim.vehicles())
	{
		if (!visible(v.pos, v.p->length))
		{
			continue;
		}
		// Level of the vehicle: on a ramp 0, on the raised or covered part the
		// level of the road; in a junction 0 (or the level of two roads that
		// continue there without a ramp)
		int level = 0;
		if (!v.on_connector)
		{
			const net_road *nr = rn.road(rn.lanes[v.seg].road_id);
			if (nr && nr->level != 0)
			{
				const double s = nr->trimmed.project(v.pos);
				if (s >= nr->level_s0 && s <= nr->level_s1)
				{
					level = nr->level;
				}
			}
		}
		else
		{
			const net_connector &c = rn.connectors[v.seg];
			const net_road *ra = rn.road(c.from_road);
			const net_road *rb = rn.road(c.to_road);
			auto covered_end = [&ws, &c](const net_road *nr)
			{
				const road_data *d = ws.model().road(nr->road_id);
				const double len = nr->trimmed.length();
				return d && ((d->node_a == c.node_id && nr->level_s0 <= 0.05) || (d->node_b == c.node_id && nr->level_s1 >= len - 0.05));
			};
			if (ra && rb && ra->level != 0 && ra->level == rb->level && covered_end(ra) && covered_end(rb))
			{
				level = ra->level;
			}
		}
		shape_batch &batch = level > 0 ? m_shapes_upper : (level < 0 ? m_shapes_cover : m_shapes);
		QColor col = QColor::fromRgb(v.color);
		if (level < 0)
		{
			col.setAlpha(90);
		}
		if (v.p->length * cam.ppm < 5.0)
		{
			batch.add(v.pos, v.dir, 2.0 * mpp, 2.0 * mpp, mpp, col, shape_batch::k_disc);
			continue;
		}
		int kind = shape_batch::k_car;
		if (v.kind == vehicle_kind::bus)
		{
			kind = shape_batch::k_bus;
		}
		else if (v.kind == vehicle_kind::motorcycle)
		{
			kind = shape_batch::k_motorcycle;
		}
		const double braking = (v.a < -0.8 || v.v < 0.3) ? 1.0 : 0.0;
		batch.add(v.pos, v.dir, v.p->length / 2.0, v.p->width / 2.0, 0.9, col, kind, braking);
	}

	// --- pedestrians ---
	for (const sim_pedestrian &p : sim.pedestrians())
	{
		if (!visible(p.pos, 1.0))
		{
			continue;
		}
		const QColor col = QColor::fromRgb(p.color);
		if (cam.ppm < 4.0)
		{
			m_shapes.add(p.pos, p.dir, 1.2 * mpp, 1.2 * mpp, mpp, col, shape_batch::k_disc);
		}
		else
		{
			m_shapes.add(p.pos, p.dir, 0.30, 0.30, 0.30, col, shape_batch::k_pedestrian);
		}
	}

	// --- objects ---
	for (const auto &kv : m.objects)
	{
		const object_data &o = kv.second;
		const net_road *r = net.road(o.road_id);
		if (!r)
		{
			continue;
		}
		vec2 c;
		vec2 t;
		r->center.sample(o.s, &c, &t);
		if (!visible(c, 60.0))
		{
			continue;
		}
		const vec2 n = t.left();
		switch (o.kind)
		{
			case object_kind::cone:
			case object_kind::barrier:
			{
				for (int dir : { 1, -1 })
				{
					if (o.dir != 0 && o.dir != dir)
					{
						continue;
					}
					const std::vector<int> &ls = net.lanes_of(o.road_id, dir);
					for (int lid : ls)
					{
						const net_lane &l = net.lanes[lid];
						if (o.lane >= 0 && l.index != std::min<int>(o.lane, static_cast<int>(ls.size()) - 1))
						{
							continue;
						}
						const double s = net.lane_s_from_road_s(l, o.s);
						const double s0 = s - o.length / 2.0;
						const double s1 = s + o.length / 2.0;
						if (o.kind == object_kind::cone)
						{
							// taper: the first cones are on the lane edge
							for (double x = s0; x <= s1 + 0.01; x += 2.5)
							{
								vec2 p;
								vec2 tt;
								l.path.sample(x, &p, &tt);
								const double k = std::min(1.0, (x - s0) / 6.0);
								const double lat = (l.width / 2.0 - 0.4) * (1.0 - k);
								m_shapes.add(p + tt.right() * lat, tt, 0.36, 0.36, 0.25, QColor(250, 140, 70), shape_batch::k_cone);
							}
						}
						else
						{
							for (double x = s0; x < s1; x += 2.2)
							{
								vec2 p;
								vec2 tt;
								l.path.sample(x + 1.0, &p, &tt);
								m_shapes.add(p, tt, 1.0, 0.32, 0.2, QColor(214, 216, 220), shape_batch::k_rounded, 0.12);
							}
						}
					}
				}
				break;
			}
			case object_kind::road_closure:
			{
				const double half = (r->edge_left - r->edge_right) / 2.0;
				const vec2 mid = c + n * ((r->edge_left + r->edge_right) / 2.0);
				const int stripes = std::max(2, static_cast<int>(half * 2.0 / 0.8));
				const double sl = half * 2.0 / stripes;
				for (int k = 0; k < stripes; ++k)
				{
					const vec2 p = mid + n * (-half + sl * (k + 0.5));
					m_over.rect(p, n, sl / 2.0, 0.35, (k % 2 == 0) ? QColor(230, 80, 70) : QColor(250, 250, 250));
				}
				const sign_def *d = sign_catalog::instance().find(QStringLiteral("closed_to_vehicles"));
				if (d && cam.ppm >= 0.7)
				{
					const int cell = sign_catalog::instance().atlas_cell(d->key, 0);
					m_signs.add(c + n * (r->edge_right - 1.5) - t * 1.5, std::max(0.8, 11.0 * mpp), atlas_uv(cell, sign_catalog::instance()), 1.0);
				}
				break;
			}
			case object_kind::level_crossing:
			{
				double barrier = 0.0;
				double train_pos = -1.0;
				const crossing_state st = sim.level_crossing(o.id, &barrier, &train_pos);
				const bool blink = std::fmod(opt.anim_time, 1.0) < 0.5;
				for (int dir : { 1, -1 })
				{
					// The barrier for the traffic in "dir" is on the right side of that traffic.
					const double edge = dir > 0 ? r->edge_right : r->edge_left;
					const vec2 base = c + n * (edge + (dir > 0 ? -0.8 : 0.8)) - t * (4.0 * dir);
					const vec2 to_center = n * (dir > 0 ? 1.0 : -1.0);
					const double len = (std::abs(edge) + 0.8) * std::max(0.08, barrier);
					const int parts = std::max(1, static_cast<int>(len / 0.6));
					for (int k = 0; k < parts; ++k)
					{
						const vec2 p = base + to_center * (len * (k + 0.5) / parts);
						m_over.rect(p, to_center, len / parts / 2.0, 0.14, (k % 2 == 0) ? QColor(230, 70, 60) : QColor(250, 250, 250));
					}
					m_shapes_top.add(base, t, 0.32, 0.32, 0.1, QColor(60, 64, 72), shape_batch::k_disc);
					if (st == crossing_state::closing || st == crossing_state::closed)
					{
						m_shapes_top.add(base - to_center * 0.6, t, 0.25, 0.25, 0.6, blink ? QColor(255, 70, 60) : QColor(120, 40, 40),
							shape_batch::k_disc, blink ? 1.0 : 0.0);
					}
				}
				if (train_pos >= 0.0)
				{
					const double total = 180.0;
					const vec2 head = c + n * (-total / 2.0 + total * train_pos + 40.0);
					for (int k = 0; k < 4; ++k)
					{
						const vec2 p = head - n * (k * 19.0 + 9.0);
						m_shapes_top.add(p, n, 9.0, 1.5, 1.0, k == 0 ? QColor(232, 120, 110) : QColor(150, 180, 220), shape_batch::k_bus);
					}
				}
				break;
			}
			case object_kind::bus_stop:
			{
				vec2 pos;
				vec2 dir;
				double hl = 1.0;
				if (world_layout::object_pose(m, net, o, &pos, &dir, &hl))
				{
					const vec2 out = dir.right();
					m_shapes_top.add(pos + out * 0.6, dir, 2.4, 0.7, 0.3, QColor(160, 196, 232), shape_batch::k_rounded, 0.25);
					const int waiting = std::min(8, sim.bus_stop_waiting(o.id));
					for (int k = 0; k < waiting; ++k)
					{
						const vec2 p = pos + dir * (-2.0 + 0.55 * k) - out * 0.2;
						m_shapes.add(p, -out, 0.3, 0.3, 0.25, QColor::fromRgb(0xF7C6D9), shape_batch::k_pedestrian);
					}
				}
				break;
			}
		}
	}

	// --- signal heads ---
	for (const world_layout::signal_head &h : world_layout::signal_heads(m, net))
	{
		if (!visible(h.pos, 5.0))
		{
			continue;
		}
		const double k = std::max(1.0, 14.0 * mpp / 1.6);
		const signal_color col = sim.signal_for(h.node_id, h.road_id);
		m_shapes_top.add(h.pos, h.dir, 0.8 * k, 0.32 * k, 0.2 * k, QColor(48, 52, 60), shape_batch::k_rounded, 0.15 * k);
		const bool red = col == signal_color::red || col == signal_color::red_yellow;
		const bool yellow = col == signal_color::yellow || col == signal_color::red_yellow;
		const bool green = col == signal_color::green;
		struct lamp
		{
			double x;
			QColor color;
			bool on;
		};
		const lamp lamps[3] =
		{
			{ 0.5, QColor(255, 85, 75), red },
			{ 0.0, QColor(255, 205, 60), yellow },
			{ -0.5, QColor(90, 215, 120), green }
		};
		for (const lamp &l : lamps)
		{
			const QColor c = l.on ? l.color : mix_color(l.color, QColor(40, 40, 46), 0.72);
			m_shapes_top.add(h.pos + h.dir * (l.x * k), h.dir, 0.21 * k, 0.21 * k, l.on ? 0.55 * k : 0.05, c, shape_batch::k_disc, l.on ? 1.0 : 0.0);
		}
	}

	// --- node handles (editor mode) ---
	if (opt.editor_mode)
	{
		const double r = std::max(0.9, 5.0 * mpp);
		for (const auto &kv : m.nodes)
		{
			if (!visible(kv.second.pos, r))
			{
				continue;
			}
			const net_junction *j = net.junction(kv.first);
			const bool dead_end = j && j->arms.size() == 1;
			const QColor ring = (dead_end && kv.second.spawn) ? QColor(90, 180, 120) : QColor(70, 80, 95);
			m_shapes_top.add(kv.second.pos, vec2(1, 0), r, r, 0.1, QColor(255, 255, 255, 235), shape_batch::k_disc);
			m_shapes_top.add(kv.second.pos, vec2(1, 0), r, std::max(0.15, 1.6 * mpp), 0.2, ring, shape_batch::k_ring);
		}
		const double hr = std::max(0.6, 4.0 * mpp);
		for (const vec2 &p : opt.handles)
		{
			m_shapes_top.add(p, vec2(1, 0), hr, hr, 0.1, accent, shape_batch::k_disc);
		}
	}

	// --- signs ---
	if (cam.ppm >= 0.7)
	{
		sign_catalog &cat = sign_catalog::instance();
		const double half = std::max(0.7, 11.0 * mpp);
		for (const auto &kv : m.signs)
		{
			vec2 pos;
			vec2 dir;
			if (!world_layout::sign_pose(m, net, kv.second, &pos, &dir) || !visible(pos, half))
			{
				continue;
			}
			const int cell = cat.atlas_cell(kv.second.key, kv.second.value);
			if (cell < 0)
			{
				continue;
			}
			m_shapes.add(pos, vec2(1, 0), 0.12, 0.12, 0.05, QColor(80, 84, 92), shape_batch::k_disc);
			m_signs.add(pos + vec2(0.0, half * 0.9), half, atlas_uv(cell, cat), 1.0);
		}
	}

	m_dynamic_count = static_cast<int>(m_under.v.size() + m_over.v.size() + m_shapes.v.size() + m_shapes_top.v.size() + m_signs.v.size());
}

// ---------------------------------------------------------------------------
// drawing
// ---------------------------------------------------------------------------
void world_renderer::draw_flat(QOpenGLBuffer &buf, int count, const camera &cam, int first)
{
	if (count <= 0)
	{
		return;
	}
	m_flat.bind();
	set_camera(m_flat, cam);
	buf.bind();
	const int pos = m_flat.attributeLocation("a_pos");
	const int col = m_flat.attributeLocation("a_color");
	m_flat.enableAttributeArray(pos);
	m_flat.enableAttributeArray(col);
	m_flat.setAttributeBuffer(pos, GL_FLOAT, 0, 2, sizeof(flat_vertex));
	m_flat.setAttributeBuffer(col, GL_FLOAT, 2 * sizeof(float), 4, sizeof(flat_vertex));
	glDrawArrays(GL_TRIANGLES, first, count);
	m_flat.disableAttributeArray(pos);
	m_flat.disableAttributeArray(col);
	buf.release();
	m_flat.release();
}

void world_renderer::draw_flat_data(const flat_mesh &mesh, const camera &cam)
{
	if (mesh.v.empty())
	{
		return;
	}
	m_stream.bind();
	m_stream.allocate(mesh.v.data(), static_cast<int>(mesh.v.size() * sizeof(flat_vertex)));
	m_stream.release();
	draw_flat(m_stream, static_cast<int>(mesh.v.size()), cam);
}

void world_renderer::draw_shapes(const shape_batch &batch, const camera &cam)
{
	if (batch.v.empty())
	{
		return;
	}
	m_stream.bind();
	m_stream.allocate(batch.v.data(), static_cast<int>(batch.v.size() * sizeof(shape_vertex)));
	m_shape.bind();
	set_camera(m_shape, cam);
	m_shape.setUniformValue("u_mpp", static_cast<float>(cam.mpp()));
	const int stride = sizeof(shape_vertex);
	const int a_pos = m_shape.attributeLocation("a_pos");
	const int a_local = m_shape.attributeLocation("a_local");
	const int a_half = m_shape.attributeLocation("a_half");
	const int a_color = m_shape.attributeLocation("a_color");
	const int a_kind = m_shape.attributeLocation("a_kind");
	const int locs[5] = { a_pos, a_local, a_half, a_color, a_kind };
	for (int l : locs)
	{
		if (l >= 0)
		{
			m_shape.enableAttributeArray(l);
		}
	}
	m_shape.setAttributeBuffer(a_pos, GL_FLOAT, 0, 2, stride);
	m_shape.setAttributeBuffer(a_local, GL_FLOAT, 2 * sizeof(float), 2, stride);
	m_shape.setAttributeBuffer(a_half, GL_FLOAT, 4 * sizeof(float), 2, stride);
	m_shape.setAttributeBuffer(a_color, GL_FLOAT, 6 * sizeof(float), 4, stride);
	m_shape.setAttributeBuffer(a_kind, GL_FLOAT, 10 * sizeof(float), 2, stride);
	glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(batch.v.size()));
	for (int l : locs)
	{
		if (l >= 0)
		{
			m_shape.disableAttributeArray(l);
		}
	}
	m_shape.release();
	m_stream.release();
}

void world_renderer::draw_signs(const camera &cam)
{
	if (m_signs.v.empty())
	{
		return;
	}
	update_atlas();
	m_stream.bind();
	m_stream.allocate(m_signs.v.data(), static_cast<int>(m_signs.v.size() * sizeof(tex_vertex)));
	m_tex.bind();
	set_camera(m_tex, cam);
	glActiveTexture(GL_TEXTURE0);
	m_atlas->bind(0);
	m_tex.setUniformValue("u_tex", 0);
	const int stride = sizeof(tex_vertex);
	const int a_pos = m_tex.attributeLocation("a_pos");
	const int a_uv = m_tex.attributeLocation("a_uv");
	const int a_alpha = m_tex.attributeLocation("a_alpha");
	m_tex.enableAttributeArray(a_pos);
	m_tex.enableAttributeArray(a_uv);
	m_tex.enableAttributeArray(a_alpha);
	m_tex.setAttributeBuffer(a_pos, GL_FLOAT, 0, 2, stride);
	m_tex.setAttributeBuffer(a_uv, GL_FLOAT, 2 * sizeof(float), 2, stride);
	m_tex.setAttributeBuffer(a_alpha, GL_FLOAT, 4 * sizeof(float), 1, stride);
	glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(m_signs.v.size()));
	m_tex.disableAttributeArray(a_pos);
	m_tex.disableAttributeArray(a_uv);
	m_tex.disableAttributeArray(a_alpha);
	m_atlas->release(0);
	m_tex.release();
	m_stream.release();
}

// ---------------------------------------------------------------------------
// density map
// ---------------------------------------------------------------------------
void world_renderer::build_density(const workspace &ws, const camera &cam, const render_options &opt)
{
	const double dt = m_density_clock.restart() / 1000.0;
	m_density_drawn = false;
	const road_network &net = ws.network();
	if (!opt.show_density || cam.ppm >= opt.density_ppm || net.lanes.empty())
	{
		return;
	}
	// Index: road_network::dir_key(road, dir). New network: start again.
	int max_road = 0;
	for (const auto &kv : net.roads)
	{
		max_road = std::max(max_road, kv.first);
	}
	const size_t n = static_cast<size_t>(max_road) * 2 + 2;
	if (m_density_ws != &ws || m_density_rev != net.built_revision() || m_density_ratio.size() != n)
	{
		m_density_ws = &ws;
		m_density_rev = net.built_revision();
		m_density_ratio.assign(n, 1.0);
		m_density_occ.assign(n, 0.0);
	}
	std::vector<double> sum(n, 0.0);
	std::vector<int> count(n, 0);
	for (const sim_vehicle &v : ws.sim().vehicles())
	{
		if (v.on_connector)
		{
			continue;
		}
		const net_lane &l = net.lanes[v.seg];
		const int k = road_network::dir_key(l.road_id, l.dir);
		sum[k] += v.v / std::max(1.0, l.limit_at(v.s));
		++count[k];
	}
	// Smoothing in time (about 2 s): the colors do not flicker.
	const double alpha = 1.0 - std::exp(-std::max(0.0, std::min(dt, 1.0)) / 2.0);
	const double mpp = cam.mpp();
	const vec2 lo = cam.to_world(QPointF(0, cam.height));
	const vec2 hi = cam.to_world(QPointF(cam.width, 0));
	const QColor free_c(120, 200, 120, 225);
	const QColor slow_c(250, 210, 90, 230);
	const QColor heavy_c(242, 145, 80, 235);
	const QColor jam_c(214, 70, 70, 240);
	const double step = std::max(4.0, 6.0 * mpp);
	for (const auto &kv : net.roads)
	{
		const net_road &r = kv.second;
		const double len = r.trimmed.length();
		if (len < 1.0)
		{
			continue;
		}
		const bool two_way = !r.lanes_fwd.empty() && !r.lanes_bwd.empty();
		for (int dir : { 1, -1 })
		{
			const std::vector<int> &ids = dir > 0 ? r.lanes_fwd : r.lanes_bwd;
			if (ids.empty())
			{
				continue;
			}
			const int k = road_network::dir_key(r.road_id, dir);
			// Low density: free flow, also with one slow vehicle (for example at a red light).
			const double lane_km = std::max(0.01, len / 1000.0 * static_cast<double>(ids.size()));
			const double occ = count[k] / lane_km;
			const double ratio = count[k] > 0 ? sum[k] / count[k] : 1.0;
			const double w = std::min(1.0, occ / 12.0);
			const double target = 1.0 - w * (1.0 - ratio);
			m_density_ratio[k] += (target - m_density_ratio[k]) * alpha;
			m_density_occ[k] += (occ - m_density_occ[k]) * alpha;

			// Culling with the bounding box of the end points and the length
			const vec2 a = r.trimmed.front();
			const vec2 b = r.trimmed.back();
			const double margin = len;
			if (std::max(a.x, b.x) + margin < lo.x || std::min(a.x, b.x) - margin > hi.x
				|| std::max(a.y, b.y) + margin < lo.y || std::min(a.y, b.y) - margin > hi.y)
			{
				continue;
			}
			const double q = m_density_ratio[k];
			const QColor col = q > 0.6 ? mix_color(slow_c, free_c, (q - 0.6) / 0.3)
				: (q > 0.3 ? mix_color(heavy_c, slow_c, (q - 0.3) / 0.3) : mix_color(jam_c, heavy_c, q / 0.3));
			// One line for each direction, on the side of the traffic (right-hand traffic)
			const double half = (dir > 0 ? -r.edge_right : r.edge_left);
			const double off = two_way ? std::max(half * 0.5, 2.6 * mpp) * (dir > 0 ? -1.0 : 1.0) : 0.0;
			const double width = std::max(half * (two_way ? 0.9 : 1.6), 3.4 * mpp);
			std::vector<vec2> pts;
			for (double s = 0.0; s < len; s += step)
			{
				vec2 p;
				vec2 t;
				r.trimmed.sample(s, &p, &t);
				pts.push_back(p + t.left() * off);
			}
			vec2 p;
			vec2 t;
			r.trimmed.sample(len, &p, &t);
			pts.push_back(p + t.left() * off);
			m_under.thick_line(pts, width, col);
			m_density_drawn = true;
		}
	}
}

// ---------------------------------------------------------------------------
// map tiles
// ---------------------------------------------------------------------------
QOpenGLTexture *world_renderer::tile_texture(int z, int x, int y, bool load, int *uploads)
{
	const quint64 k = tile_cache::key(z, x, y);
	auto it = m_gl_tiles.find(k);
	if (it != m_gl_tiles.end())
	{
		it->second.frame = m_frame;
		return it->second.tex.get();
	}
	if (*uploads >= 8)
	{
		return nullptr; // the other uploads in the next frames (no stutter)
	}
	tile_cache &tc = tile_cache::instance();
	const QImage img = load ? tc.image(z, x, y) : tc.cached(z, x, y);
	if (img.isNull())
	{
		return nullptr;
	}
	gl_tile t;
	t.tex.reset(new QOpenGLTexture(img, QOpenGLTexture::GenerateMipMaps));
	t.tex->setMinificationFilter(QOpenGLTexture::LinearMipMapLinear);
	t.tex->setMagnificationFilter(QOpenGLTexture::Linear);
	t.tex->setWrapMode(QOpenGLTexture::ClampToEdge);
	t.frame = m_frame;
	++*uploads;
	QOpenGLTexture *raw = t.tex.get();
	m_gl_tiles.emplace(k, std::move(t));
	return raw;
}

void world_renderer::draw_map(const workspace &ws, const camera &cam, const render_options &opt)
{
	m_map_drawn = false;
	const world_model &m = ws.model();
	if (!opt.show_map || !m.has_geo() || opt.map_opacity <= 0.01)
	{
		return;
	}
	++m_frame;
	const geo_projection proj(m.geo_lat, m.geo_lon);
	// Zoom level: one tile pixel is about one screen pixel.
	const double px_per_m = cam.ppm * std::max(1.0, opt.dpr);
	const double z0_m = geo_projection::tile_size(0) * proj.scale();
	int z = static_cast<int>(std::ceil(std::log2(std::max(1e-9, z0_m * px_per_m / 256.0)) - 0.3));
	z = std::max(1, std::min(std::max(1, opt.map_max_zoom), z));
	const vec2 lo = cam.to_world(QPointF(0, cam.height));
	const vec2 hi = cam.to_world(QPointF(cam.width, 0));
	double mx0 = 0.0;
	double my0 = 0.0;
	double mx1 = 0.0;
	double my1 = 0.0;
	proj.world_to_mercator(lo, &mx0, &my0);
	proj.world_to_mercator(hi, &mx1, &my1);
	int tx0 = 0;
	int tx1 = 0;
	int ty0 = 0;
	int ty1 = 0;
	for (;;)
	{
		tx0 = geo_projection::tile_x(mx0, z);
		tx1 = geo_projection::tile_x(mx1, z);
		ty0 = geo_projection::tile_y(my1, z);
		ty1 = geo_projection::tile_y(my0, z);
		if ((tx1 - tx0 + 1) * (ty1 - ty0 + 1) <= 200 || z <= 1)
		{
			break;
		}
		--z;
	}

	int uploads = 0;
	tex_batch batch;
	std::vector<QOpenGLTexture *> textures;
	for (int ty = ty0; ty <= ty1; ++ty)
	{
		for (int tx = tx0; tx <= tx1; ++tx)
		{
			QRectF uv(0.0, 0.0, 1.0, 1.0);
			QOpenGLTexture *t = tile_texture(z, tx, ty, true, &uploads);
			// Not loaded yet: a part of a lower zoom tile
			for (int dz = 1; !t && dz <= 8 && z - dz >= 0; ++dz)
			{
				t = tile_texture(z - dz, tx >> dz, ty >> dz, false, &uploads);
				if (t)
				{
					const double cells = static_cast<double>(1 << dz);
					uv = QRectF((tx - ((tx >> dz) << dz)) / cells, (ty - ((ty >> dz) << dz)) / cells, 1.0 / cells, 1.0 / cells);
				}
			}
			if (!t)
			{
				continue;
			}
			const double size = geo_projection::tile_size(z);
			const double left = geo_projection::tile_left(tx, z);
			const double top = geo_projection::tile_top(ty, z);
			batch.add_rect(proj.mercator_to_world(left, top - size), proj.mercator_to_world(left + size, top), uv, opt.map_opacity);
			textures.push_back(t);
		}
	}
	m_map_zoom = z;
	if (!textures.empty())
	{
		glEnable(GL_BLEND);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		m_stream.bind();
		m_stream.allocate(batch.v.data(), static_cast<int>(batch.v.size() * sizeof(tex_vertex)));
		m_tile.bind();
		set_camera(m_tile, cam);
		m_tile.setUniformValue("u_tex", 0);
		m_tile.setUniformValue("u_fade", static_cast<float>(opt.map_fade));
		const int stride = sizeof(tex_vertex);
		const int a_pos = m_tile.attributeLocation("a_pos");
		const int a_uv = m_tile.attributeLocation("a_uv");
		const int a_alpha = m_tile.attributeLocation("a_alpha");
		m_tile.enableAttributeArray(a_pos);
		m_tile.enableAttributeArray(a_uv);
		m_tile.enableAttributeArray(a_alpha);
		m_tile.setAttributeBuffer(a_pos, GL_FLOAT, 0, 2, stride);
		m_tile.setAttributeBuffer(a_uv, GL_FLOAT, 2 * sizeof(float), 2, stride);
		m_tile.setAttributeBuffer(a_alpha, GL_FLOAT, 4 * sizeof(float), 1, stride);
		glActiveTexture(GL_TEXTURE0);
		for (size_t i = 0; i < textures.size(); ++i)
		{
			textures[i]->bind(0);
			glDrawArrays(GL_TRIANGLES, static_cast<GLint>(i * 6), 6);
		}
		textures.back()->release(0);
		m_tile.disableAttributeArray(a_pos);
		m_tile.disableAttributeArray(a_uv);
		m_tile.disableAttributeArray(a_alpha);
		m_tile.release();
		m_stream.release();
		m_map_drawn = true;
	}

	// Keep the GPU memory small: remove the tiles that were not used for the longest time.
	if (m_gl_tiles.size() > 320)
	{
		std::vector<std::pair<quint64, quint64>> age;
		age.reserve(m_gl_tiles.size());
		for (const auto &kv : m_gl_tiles)
		{
			age.push_back(std::make_pair(kv.second.frame, kv.first));
		}
		std::sort(age.begin(), age.end());
		for (size_t i = 0; i + 240 < age.size(); ++i)
		{
			m_gl_tiles.erase(age[i].second);
		}
	}
}

void world_renderer::render(const workspace &ws, const camera &cam, const render_options &opt)
{
	if (!m_ok)
	{
		return;
	}
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_SCISSOR_TEST);

	if (m_static_ws != &ws || m_static_rev != ws.view_revision())
	{
		build_static(ws);
	}
	build_dynamic(ws, cam, opt);

	// 1) ground (opaque)
	glDisable(GL_BLEND);
	{
		const double period = 6400.0; // multiple of the noise period (256 m) and of the grid (100 m)
		const double ox = std::floor(cam.center.x / period) * period;
		const double oy = std::floor(cam.center.y / period) * period;
		m_ground.bind();
		m_ground.setUniformValue("u_cam_local", static_cast<float>(cam.center.x - ox), static_cast<float>(cam.center.y - oy));
		m_ground.setUniformValue("u_scale", static_cast<float>(2.0 * cam.ppm / cam.width), static_cast<float>(2.0 * cam.ppm / cam.height));
		m_ground.setUniformValue("u_ground", static_cast<float>(static_cast<int>(ws.model().ground)));
		m_ground.setUniformValue("u_mpp", static_cast<float>(cam.mpp()));
		m_ground.setUniformValue("u_grid", opt.show_grid ? 1.0f : 0.0f);
		m_quad.bind();
		const int a_pos = m_ground.attributeLocation("a_pos");
		m_ground.enableAttributeArray(a_pos);
		m_ground.setAttributeBuffer(a_pos, GL_FLOAT, 0, 2, 2 * sizeof(float));
		glDrawArrays(GL_TRIANGLES, 0, 6);
		m_ground.disableAttributeArray(a_pos);
		m_quad.release();
		m_ground.release();
	}

	// 1b) map tiles (geographic origin only)
	draw_map(ws, cam, opt);

	// 2..6) the alpha of the frame buffer stays 1 (separate alpha blending)
	glEnable(GL_BLEND);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	// The covered parts of the tunnels and their vehicles, then the ground
	// roads over them
	draw_flat(m_static, m_cover_count, cam);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	draw_shapes(m_shapes_cover, cam);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	draw_flat(m_static, m_static_count - m_cover_count, cam, m_cover_count);
	draw_flat_data(m_under, cam);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	draw_shapes(m_shapes, cam);
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	// Bridges and their vehicles over the roads and the vehicles below them
	if (m_upper_count > 0)
	{
		draw_flat(m_static_upper, m_upper_count, cam);
		glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
		draw_shapes(m_shapes_upper, cam);
		glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	}
	draw_flat_data(m_over, cam);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	draw_shapes(m_shapes_top, cam);
	draw_signs(cam);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
