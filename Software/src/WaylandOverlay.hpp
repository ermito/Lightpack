/*
 * WaylandOverlay.hpp
 *
 *	Single fullscreen overlay window that draws all LED zones as custom paint.
 *	Avoids per-widget top-level positioning issues on Wayland.
 */

#pragma once

#include <QWidget>
#include <QScreen>
#include <QRegion>
#include <QMap>
#include <QMutex>
#include <QColor>
#include "GrabZone.hpp"

class WaylandOverlay : public QWidget
{
	Q_OBJECT
public:
	explicit WaylandOverlay(QScreen *screen, QWidget *parent = nullptr);
	~WaylandOverlay() override;

	static WaylandOverlay *overlayForScreen(QScreen *screen);
	static WaylandOverlay *primaryOverlay();

	void showOverlay();
	void hideOverlay();
	bool isOverlayVisible() const { return isVisible(); }

	QScreen *screen() const { return m_screen; }

	// Zone management
	void addZone(const GrabZone &zone);
	void removeZone(int id);
	void updateZones(const QList<GrabZone> &zones);
	QList<GrabZone> zones() const { return m_zones; }

	// Input region
	void updateInputRegion();

signals:
	void zoneResizedOrMoved(int id, const QRect &newGeometry);
	void zoneResizedOrMoveCompleted(int id, const QRect &finalGeometry);
	void zoneColorChanged(int id, const QColor &color);
	void overlayHidden();

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

private:
	void setupUI();
	void applyInputRegion();
	void updateZoneFromSettings(int zoneId);

	QScreen *m_screen;
	QList<GrabZone> m_zones;

	// Drag/resize state
	int m_draggingZoneId = -1;
	QPoint m_dragStartPos;
	QPoint m_dragZoneStartPos;
	GrabZone::ResizeEdge m_resizeEdge = GrabZone::None;

	static QMap<QScreen*, WaylandOverlay*> s_overlays;
	static QMutex s_overlaysMutex;
};
