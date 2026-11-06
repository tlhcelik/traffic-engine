#ifndef ICON_FACTORY_H
#define ICON_FACTORY_H

#include <QIcon>
#include <QPixmap>
#include <QString>

// icon_factory
// ------------
// The icons of the tools and the actions. Each icon is a tile with rounded
// corners in a pastel color and a dark slate symbol. The icons are vector
// drawings (QPainter): they are sharp at all sizes and screen scales.
//
// Names: select, road, light, sign, cone, barrier, closure, crossing,
// bus_stop, spawn, erase, new, open, save, editor, run, pause, step, reset,
// faster, slower, charts, settings, dev, fit, follow, json, program, delete,
// demo, screenshot, properties, signs.

namespace icon_factory
{
	QIcon icon(const QString &name);
	QPixmap pixmap(const QString &name, int size);
}

#endif // ICON_FACTORY_H
