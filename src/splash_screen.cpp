#include "splash_screen.h"
#include "i18n.h"

#include <QApplication>
#include <QCoreApplication>
#include <QEasingCurve>
#include <QEventLoop>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>

#include <cmath>

namespace
{
	const int card_w = 580;
	const int card_h = 360;
	const int margin = 14;          // space for the shadow of the card
}

splash_screen::splash_screen(int fade_ms, int hold_ms, QWidget *parent)
	: QWidget(parent)
	, m_fade_ms(std::max(0, fade_ms))
	, m_hold_ms(std::max(0, hold_ms))
	, m_anim(new QPropertyAnimation(this, "windowOpacity", this))
	, m_tick(new QTimer(this))
	, m_icon(QStringLiteral(":/icons/traffic_engine.png"))
	, m_phase(0.0)
	, m_hold_done(false)
	, m_init_done(false)
	, m_fading_out(false)
{
	// Frameless, on top, not in the task bar, transparent corners
	setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::SplashScreen);
	setAttribute(Qt::WA_TranslucentBackground, true);
	resize(card_w + 2 * margin, card_h + 2 * margin);
	if (QScreen *scr = QApplication::primaryScreen())
	{
		const QRect geo = scr->availableGeometry();
		move(geo.center().x() - width() / 2, geo.center().y() - height() / 2);
	}
	setWindowOpacity(0.0);
	m_tick->setInterval(30);
	connect(m_tick, &QTimer::timeout, this, [this]()
	{
		m_phase = std::fmod(m_phase + 0.004, 1.0);
		update();
	});
}

void splash_screen::start()
{
	show();
	raise();
	m_tick->start();
	disconnect(m_anim, &QPropertyAnimation::finished, nullptr, nullptr);
	connect(m_anim, &QPropertyAnimation::finished, this, [this]()
	{
		emit ready_to_init();
		QTimer::singleShot(m_hold_ms, this, [this]()
		{
			m_hold_done = true;
			maybe_fade_out();
		});
	});
	fade(0.0, 1.0);
}

void splash_screen::fade(double from, double to)
{
	m_anim->stop();
	m_anim->setDuration(std::max(1, m_fade_ms));
	m_anim->setStartValue(from);
	m_anim->setEndValue(to);
	m_anim->setEasingCurve(QEasingCurve::InOutCubic);
	m_anim->start();
}

void splash_screen::set_status(const QString &text)
{
	m_status = text;
	update();
	QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
}

void splash_screen::finish()
{
	m_init_done = true;
	maybe_fade_out();
}

void splash_screen::maybe_fade_out()
{
	if (!m_hold_done || !m_init_done || m_fading_out)
	{
		return;
	}
	m_fading_out = true;
	disconnect(m_anim, &QPropertyAnimation::finished, nullptr, nullptr);
	connect(m_anim, &QPropertyAnimation::finished, this, [this]()
	{
		m_tick->stop();
		emit finished();
		close();
	});
	fade(1.0, 0.0);
}

void splash_screen::paintEvent(QPaintEvent *event)
{
	Q_UNUSED(event);
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	p.setRenderHint(QPainter::SmoothPixmapTransform, true);

	// Shadow and card
	const QRectF card(margin, margin, card_w, card_h);
	for (int i = margin; i > 0; i -= 2)
	{
		p.setPen(Qt::NoPen);
		p.setBrush(QColor(0, 0, 0, 6));
		p.drawRoundedRect(card.adjusted(-i * 0.5, -i * 0.3, i * 0.5, i * 0.8), 18 + i * 0.5, 18 + i * 0.5);
	}
	QLinearGradient bg(card.topLeft(), card.bottomRight());
	bg.setColorAt(0.0, QColor(31, 42, 68));
	bg.setColorAt(1.0, QColor(36, 82, 128));
	p.setBrush(bg);
	p.drawRoundedRect(card, 18, 18);

	// Icon and name
	const double left = card.left() + 34;
	if (!m_icon.isNull())
	{
		p.drawPixmap(QRectF(left, card.top() + 34, 64, 64), m_icon, m_icon.rect());
	}
	QFont title = font();
	title.setPointSizeF(25.0);
	title.setBold(true);
	p.setFont(title);
	p.setPen(QColor(255, 255, 255));
	p.drawText(QRectF(left + 82, card.top() + 30, card_w - 150, 44), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("Traffic Engine"));
	QFont sub = font();
	sub.setPointSizeF(10.5);
	p.setFont(sub);
	p.setPen(QColor(190, 210, 235));
	p.drawText(QRectF(left + 84, card.top() + 72, card_w - 150, 24), Qt::AlignLeft | Qt::AlignVCenter,
		tx("Traffic simulation on real roads"));

	// A small road: two lanes, dashed center line, moving vehicles, one light
	const QRectF road(card.left() + 24, card.top() + 136, card_w - 48, 64);
	p.setPen(Qt::NoPen);
	p.setBrush(QColor(72, 78, 90));
	p.drawRoundedRect(road, 6, 6);
	p.save();
	p.setClipRect(road.adjusted(6, 0, -6, 0));
	p.setBrush(QColor(240, 240, 232, 220));
	for (double x = road.left() - std::fmod(m_phase * 900.0, 36.0); x < road.right(); x += 36.0)
	{
		p.drawRect(QRectF(x, road.center().y() - 1.2, 18, 2.4));
	}
	p.restore();
	// Light at the right end of the upper lane: red in the second half of the cycle
	const bool red = std::fmod(m_phase * 4.0, 1.0) > 0.55;
	const QPointF light(road.right() - 40, road.top() - 12);
	p.setBrush(QColor(40, 44, 52));
	p.drawRoundedRect(QRectF(light.x() - 7, light.y() - 16, 14, 30), 4, 4);
	p.setBrush(red ? QColor(240, 80, 70) : QColor(90, 30, 30));
	p.drawEllipse(light + QPointF(0, -8), 4.2, 4.2);
	p.setBrush(red ? QColor(30, 70, 40) : QColor(90, 220, 120));
	p.drawEllipse(light + QPointF(0, 5), 4.2, 4.2);
	// Vehicles: lane 1 to the right (they wait at the red light), lane 2 to the left
	struct car
	{
		double offset;
		double length;
		QColor color;
		bool lower;
	};
	const car cars[] =
	{
		{ 0.00, 30, QColor(143, 179, 222), false }, { 0.21, 26, QColor(232, 138, 138), false },
		{ 0.47, 44, QColor(242, 193, 78), false }, { 0.70, 26, QColor(157, 211, 168), false },
		{ 0.10, 28, QColor(245, 245, 242), true }, { 0.38, 26, QColor(195, 180, 230), true },
		{ 0.63, 30, QColor(242, 184, 128), true }, { 0.86, 26, QColor(166, 216, 227), true }
	};
	const double span = road.width() + 80;
	for (const car &c : cars)
	{
		const double u = std::fmod(m_phase * 3.0 + c.offset, 1.0);
		double x = c.lower ? road.right() + 40 - u * span : road.left() - 40 + u * span;
		const double y = c.lower ? road.center().y() + 9 : road.top() + 9;
		if (!c.lower && red)
		{
			x = std::min(x, light.x() - 26 - c.length - 40 * c.offset);
		}
		if (x + c.length < road.left() + 2 || x > road.right() - 2)
		{
			continue;
		}
		const QRectF body(x, y, c.length, 14);
		p.save();
		p.setClipRect(road.adjusted(2, 2, -2, -2));
		p.setBrush(c.color);
		p.drawRoundedRect(body, 4, 4);
		p.setBrush(QColor(40, 50, 66, 140));
		p.drawRoundedRect(QRectF(c.lower ? x + 5 : x + c.length - 13, y + 2.5, 8, 9), 2, 2);
		p.restore();
	}

	// The main features under the road
	QFont small = font();
	small.setPointSizeF(9.0);
	p.setFont(small);
	p.setPen(QColor(160, 185, 215));
	p.drawText(QRectF(card.left() + 24, road.bottom() + 14, card_w - 48, 20), Qt::AlignHCenter | Qt::AlignVCenter,
		tx("OpenStreetMap import  -  Traffic lights  -  Congestion analysis  -  External API"));

	// Status, version, progress bar
	p.setPen(QColor(205, 220, 240));
	const QRectF status(card.left() + 24, card.bottom() - 98, card_w - 48, 22);
	p.drawText(status, Qt::AlignHCenter | Qt::AlignVCenter, QFontMetrics(small).elidedText(m_status, Qt::ElideMiddle, static_cast<int>(status.width())));
	p.setPen(QColor(150, 175, 205));
	p.drawText(QRectF(card.left() + 24, card.bottom() - 74, card_w - 48, 18), Qt::AlignHCenter | Qt::AlignVCenter,
		tx("Version %1").arg(QCoreApplication::applicationVersion()));
	const double bar_w = card_w * 0.55;
	const QRectF track(card.center().x() - bar_w / 2.0, card.bottom() - 42, bar_w, 5);
	QPainterPath clip;
	clip.addRoundedRect(track, 2.5, 2.5);
	p.save();
	p.setClipPath(clip);
	p.fillRect(track, QColor(255, 255, 255, 45));
	const double seg = bar_w * 0.32;
	const double x = track.left() - seg + std::fmod(m_phase * 6.0, 1.0) * (bar_w + seg);
	p.fillRect(QRectF(x, track.top(), seg, track.height()), QColor(110, 190, 255));
	p.restore();
}
