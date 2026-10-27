/*
 * GrabZone.hpp
 *
 *	Data structure for a single LED zone (replaces per-widget hierarchy).
 *	Used on Wayland: all zones are drawn in a single overlay window.
 */

#pragma once

#include <QRect>
#include <QColor>

struct GrabZone {
	int id = 0;
	bool enabled = true;
	QRect geometry; // parent-relative (overlay is fullscreen from 0,0, so = global)
	QColor backgroundColor = Qt::white;
	QColor textColor = Qt::black;
	double coefRed = 1.0;
	double coefGreen = 1.0;
	double coefBlue = 1.0;

	// Visual state
	bool highlighted = false;
	bool dragging = false;
	enum ResizeEdge { None, Right, Left, Top, Bottom, RightDown, RightUp, LeftDown, LeftUp };
	ResizeEdge resizeEdge = None;

	// Border radius for rounded corners
	static constexpr int Radius = 4;
	// Border width
	static constexpr int BorderWidth = 2;

	QRect resizeHandleRect(ResizeEdge edge) const;
	bool containsResizeHandle(const QPoint &pos) const;
	ResizeEdge hitResizeHandle(const QPoint &pos) const;
};
