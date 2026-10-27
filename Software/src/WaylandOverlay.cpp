/*
 * WaylandOverlay.cpp
 *
 *	Draws all LED zones as custom paint on a single fullscreen overlay.
 *	Avoids Wayland compositor position control by keeping everything in one surface.
 */

#include "WaylandOverlay.hpp"
#include "Settings.hpp"
#include <QApplication>
#include <QScreen>
#include <QKeyEvent>
#include <QGuiApplication>
#include <QWindow>
#include <QTimer>
#include <QPainter>
#include <QMap>
#include <QMutex>
#include <QPainterPath>

using namespace SettingsScope;

QMap<QScreen*, WaylandOverlay*> WaylandOverlay::s_overlays;
QMutex WaylandOverlay::s_overlaysMutex;

WaylandOverlay *WaylandOverlay::overlayForScreen(QScreen *screen)
{
	if (!screen) screen = QGuiApplication::primaryScreen();
	QMutexLocker locker(&s_overlaysMutex);
	if (!s_overlays.contains(screen)) {
		s_overlays[screen] = new WaylandOverlay(screen);
	}
	return s_overlays.value(screen);
}

WaylandOverlay *WaylandOverlay::primaryOverlay()
{
	return overlayForScreen(QGuiApplication::primaryScreen());
}

WaylandOverlay::WaylandOverlay(QScreen *screen, QWidget *parent)
	: QWidget(parent)
	, m_screen(screen)
{
	setupUI();
}

WaylandOverlay::~WaylandOverlay() = default;

void WaylandOverlay::setupUI()
{
	setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
	setAttribute(Qt::WA_TranslucentBackground);
	setFocusPolicy(Qt::StrongFocus);
}

void WaylandOverlay::showOverlay()
{
	setGeometry(m_screen->geometry());
	showFullScreen();

	// Load zone positions from INI
	for (auto &zone : m_zones) {
		updateZoneFromSettings(zone.id);
	}

	updateInputRegion();
}

void WaylandOverlay::hideOverlay()
{
	hide();
}

void WaylandOverlay::addZone(const GrabZone &zone)
{
	m_zones.append(zone);
	updateZoneFromSettings(zone.id);
	update();
}

void WaylandOverlay::removeZone(int id)
{
	for (auto it = m_zones.begin(); it != m_zones.end(); ++it) {
		if (it->id == id) { m_zones.erase(it); break; }
	}
	update();
}

void WaylandOverlay::updateZones(const QList<GrabZone> &zones)
{
	m_zones = zones;
	for (auto &zone : m_zones) {
		updateZoneFromSettings(zone.id);
	}
	update();
}

void WaylandOverlay::updateInputRegion()
{
	applyInputRegion();
}

void WaylandOverlay::updateZoneFromSettings(int zoneId)
{
	for (auto &zone : m_zones) {
		if (zone.id == zoneId) {
			zone.geometry.setTopLeft(Settings::getLedPosition(zoneId));
			zone.geometry.setSize(Settings::getLedSize(zoneId));
			zone.coefRed = Settings::getLedCoefRed(zoneId);
			zone.coefGreen = Settings::getLedCoefGreen(zoneId);
			zone.coefBlue = Settings::getLedCoefBlue(zoneId);
			zone.enabled = Settings::isLedEnabled(zoneId);
			break;
		}
	}
}

void WaylandOverlay::applyInputRegion()
{
	QRegion region;
	const int margin = 15;
	for (const auto &zone : m_zones) {
		if (!zone.enabled) continue;
		region += zone.geometry.adjusted(-margin, -margin, margin, margin);
	}
	if (region.isEmpty()) {
		setMask(QRegion());
	} else {
		setMask(region);
	}
}

// ===== Paint =====

void WaylandOverlay::paintEvent(QPaintEvent *)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	const int borderW = GrabZone::BorderWidth;

	for (const auto &zone : m_zones) {
		if (!zone.enabled) continue;

		const QRect &g = zone.geometry;

		// Fill the zone with backgroundColor
		painter.setBrush(zone.backgroundColor);
		painter.setPen(Qt::NoPen);
		painter.drawRect(g);

		// Border on top
		painter.setPen(QPen(zone.highlighted ? QColor(255, 255, 255) : zone.backgroundColor.darker(150), borderW));
		painter.drawRect(g);

		// Permanent resize handles (small squares at corners)
		painter.setPen(Qt::white);
		painter.setBrush(Qt::white);
		const QRect &br = zone.resizeHandleRect(GrabZone::RightDown);
		painter.drawRect(br);
		const QRect &bl = zone.resizeHandleRect(GrabZone::LeftUp);
		painter.drawRect(bl);

		// Text: LED ID
		painter.setPen(zone.textColor);
		painter.setFont(QFont(QStringLiteral("Arial"), 10, QFont::Bold));
		QString text = QString("LED %1").arg(zone.id + 1);
		painter.drawText(g.adjusted(4, 0, -4, 0), Qt::AlignCenter | Qt::TextSingleLine, text);
	}
}

// ===== Mouse =====

void WaylandOverlay::mousePressEvent(QMouseEvent *event)
{
	const QPoint pos = event->pos();

	// Check resize handles first (reverse order: corners first)
	for (auto it = m_zones.crbegin(); it != m_zones.crend(); ++it) {
		if (!it->enabled) continue;
		if (it->resizeHandleRect(GrabZone::RightDown).contains(pos) ||
			it->resizeHandleRect(GrabZone::RightUp).contains(pos) ||
			it->resizeHandleRect(GrabZone::LeftDown).contains(pos) ||
			it->resizeHandleRect(GrabZone::LeftUp).contains(pos) ||
			it->resizeHandleRect(GrabZone::Right).contains(pos) ||
			it->resizeHandleRect(GrabZone::Left).contains(pos) ||
			it->resizeHandleRect(GrabZone::Top).contains(pos) ||
			it->resizeHandleRect(GrabZone::Bottom).contains(pos)) {
			m_draggingZoneId = it->id;
			m_resizeEdge = it->hitResizeHandle(pos);
			m_dragStartPos = pos;
			m_dragZoneStartPos = it->geometry.topLeft();
			setCursor(Qt::SizeAllCursor);
			return;
		}
	}

	// Check zone body (reverse order: top-most first)
	for (auto it = m_zones.crbegin(); it != m_zones.crend(); ++it) {
		if (!it->enabled) continue;
		if (it->geometry.contains(pos)) {
			if (event->buttons() == Qt::RightButton) {
				return;
			}
			// Left button: start drag
			m_draggingZoneId = it->id;
			m_resizeEdge = GrabZone::None;
			m_dragStartPos = pos;
			m_dragZoneStartPos = it->geometry.topLeft();
			setCursor(Qt::ClosedHandCursor);
			return;
		}
	}
}

void WaylandOverlay::mouseMoveEvent(QMouseEvent *event)
{
	const QPoint pos = event->pos();

	if (m_draggingZoneId >= 0) {
		int idx = -1;
		for (int i = 0; i < m_zones.size(); ++i) {
			if (m_zones[i].id == m_draggingZoneId) { idx = i; break; }
		}
		if (idx < 0) return;

		QRect newRect = m_zones[idx].geometry;

		if (m_resizeEdge == GrabZone::None) {
			// Move — fixed reference point
			newRect.moveTopLeft(m_dragZoneStartPos + (pos - m_dragStartPos));
		} else {
			// Resize — accumulate delta to handle missed mouseMove events
			QPoint delta = pos - m_dragStartPos;
			switch (m_resizeEdge) {
				case GrabZone::Right:    newRect.setRight(newRect.right() + delta.x()); break;
				case GrabZone::Left:     newRect.setLeft(newRect.left() + delta.x()); break;
				case GrabZone::Top:      newRect.setTop(newRect.top() + delta.y()); break;
				case GrabZone::Bottom:   newRect.setBottom(newRect.bottom() + delta.y()); break;
				case GrabZone::RightDown: newRect.setBottomRight(newRect.bottomRight() + delta); break;
				case GrabZone::RightUp:  newRect.setTopRight(newRect.topRight() + QPoint(delta.x(), -delta.y())); break;
				case GrabZone::LeftDown: newRect.setBottomLeft(newRect.bottomLeft() + QPoint(-delta.x(), delta.y())); break;
				case GrabZone::LeftUp:   newRect.setTopLeft(newRect.topLeft() + delta); break;
				case GrabZone::None:     break;
			}
			m_dragStartPos = pos;
		}

		// Clamp to screen
		const QRect screen = m_screen->geometry();
		if (newRect.left() < screen.left()) newRect.setLeft(screen.left());
		if (newRect.top() < screen.top()) newRect.setTop(screen.top());
		if (newRect.right() > screen.right()) newRect.setRight(screen.right());
		if (newRect.bottom() > screen.bottom()) newRect.setBottom(screen.bottom());

		// Minimum size
		if (newRect.width() < 50) newRect.setWidth(50);
		if (newRect.height() < 50) newRect.setHeight(50);

		m_zones[idx].geometry = newRect;
		emit zoneResizedOrMoved(m_draggingZoneId, newRect);
		update();
	} else {
		// Hover: show resize cursors
		bool hovering = false;
		for (const auto &zone : m_zones) {
			if (!zone.enabled) continue;
			if (zone.resizeHandleRect(GrabZone::RightDown).contains(pos) ||
				zone.resizeHandleRect(GrabZone::RightUp).contains(pos) ||
				zone.resizeHandleRect(GrabZone::LeftDown).contains(pos) ||
				zone.resizeHandleRect(GrabZone::LeftUp).contains(pos)) {
				setCursor(Qt::SizeFDiagCursor); hovering = true; break;
			}
			if (zone.resizeHandleRect(GrabZone::Right).contains(pos) ||
				zone.resizeHandleRect(GrabZone::Left).contains(pos)) {
				setCursor(Qt::SizeHorCursor); hovering = true; break;
			}
			if (zone.resizeHandleRect(GrabZone::Top).contains(pos) ||
				zone.resizeHandleRect(GrabZone::Bottom).contains(pos)) {
				setCursor(Qt::SizeVerCursor); hovering = true; break;
			}
			if (zone.geometry.contains(pos)) {
				setCursor(Qt::ClosedHandCursor); hovering = true; break;
			}
		}
		if (!hovering) {
			setCursor(Qt::ArrowCursor);
		}
	}
}

void WaylandOverlay::mouseReleaseEvent(QMouseEvent *)
{
	if (m_draggingZoneId >= 0) {
		int idx = -1;
		for (int i = 0; i < m_zones.size(); ++i) {
			if (m_zones[i].id == m_draggingZoneId) { idx = i; break; }
		}
		if (idx >= 0) {
			// Save to INI
			Settings::setLedPosition(m_zones[idx].id, m_zones[idx].geometry.topLeft());
			Settings::setLedSize(m_zones[idx].id, m_zones[idx].geometry.size());
			emit zoneResizedOrMoveCompleted(m_zones[idx].id, m_zones[idx].geometry);
		}
		m_draggingZoneId = -1;
		m_resizeEdge = GrabZone::None;
		setCursor(Qt::ArrowCursor);
	}
}

void WaylandOverlay::wheelEvent(QWheelEvent *event)
{
	// Cycle colors on the zone under the cursor
	const QPoint pos = event->position().toPoint();
	for (int i = 0; i < m_zones.size(); ++i) {
		auto &zone = m_zones[i];
		if (!zone.enabled) continue;
		if (zone.geometry.contains(pos)) {
			// Cycle through predefined colors
			static const QList<QColor> colors = {
				Qt::red, Qt::green, Qt::blue, Qt::yellow,
				Qt::darkRed, Qt::darkGreen, Qt::darkBlue, Qt::darkYellow,
				QColor(0, 242, 123), Qt::magenta, Qt::cyan, Qt::white
			};
			int currentIdx = colors.indexOf(zone.backgroundColor);
			if (currentIdx < 0) currentIdx = 11; // default white
			currentIdx += (event->angleDelta().y() > 0) ? 1 : -1;
			if (currentIdx >= colors.size()) currentIdx = 0;
			if (currentIdx < 0) currentIdx = colors.size() - 1;
			zone.backgroundColor = colors[currentIdx];
			emit zoneColorChanged(zone.id, zone.backgroundColor);
			update();
			return;
		}
	}
}

void WaylandOverlay::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Escape) {
		hideOverlay();
	} else {
		QWidget::keyPressEvent(event);
	}
}
