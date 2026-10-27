/*
 * GrabZone.cpp
 */

#include "GrabZone.hpp"

QRect GrabZone::resizeHandleRect(ResizeEdge edge) const
{
	const int handleSize = 12;
	QRect g = geometry;
	switch (edge) {
		case Right:    return QRect(g.right() - handleSize / 2, g.center().y() - handleSize / 2, handleSize, handleSize);
		case Left:     return QRect(g.left() - handleSize / 2, g.center().y() - handleSize / 2, handleSize, handleSize);
		case Top:      return QRect(g.center().x() - handleSize / 2, g.top() - handleSize / 2, handleSize, handleSize);
		case Bottom:   return QRect(g.center().x() - handleSize / 2, g.bottom() - handleSize / 2, handleSize, handleSize);
		case RightDown:return QRect(g.right() - handleSize / 2, g.bottom() - handleSize / 2, handleSize, handleSize);
		case RightUp:  return QRect(g.right() - handleSize / 2, g.top() - handleSize / 2, handleSize, handleSize);
		case LeftDown: return QRect(g.left() - handleSize / 2, g.bottom() - handleSize / 2, handleSize, handleSize);
		case LeftUp:   return QRect(g.left() - handleSize / 2, g.top() - handleSize / 2, handleSize, handleSize);
		case None:     return QRect();
	}
	return QRect();
}

bool GrabZone::containsResizeHandle(const QPoint &pos) const
{
	return hitResizeHandle(pos) != None;
}

GrabZone::ResizeEdge GrabZone::hitResizeHandle(const QPoint &pos) const
{
	// Check corners first (they're smaller)
	for (auto edge : {RightDown, RightUp, LeftDown, LeftUp}) {
		if (resizeHandleRect(edge).contains(pos)) return edge;
	}
	// Then edges
	for (auto edge : {Right, Left, Top, Bottom}) {
		if (resizeHandleRect(edge).contains(pos)) return edge;
	}
	return None;
}
