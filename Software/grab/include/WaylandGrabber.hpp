/*
 * WaylandGrabber.hpp
 *
 *  Created on: 10.10.2026
 *     Project: Lightpack (Prismatik)
 *
 *  Wayland screen grabber using xdg-desktop-portal + PipeWire.
 *  Targets KDE Plasma (KWin) with xdg-desktop-portal-kde.
 *
 *  Copyright (c) 2026 Lightpack contributors
 *
 *  Lightpack is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  Lightpack is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "GrabberBase.hpp"
#include "../src/enums.hpp"

#ifdef WAYLAND_GRAB_SUPPORT

#include <QThread>
#include "../src/debug.h"

/* Opaque PipeWire worker — all PipeWire types hidden in .cpp */
class PipeWireWorker : public QThread
{
    Q_OBJECT
public:
    explicit PipeWireWorker(QObject *parent = nullptr) : QThread(parent) {}

    void run() override;

    void setFd(int fd);
    void setBufferSize(int width, int height, int stride);
    void setFrameBuffer(void *buf);

signals:
    void frameReady();

public:
    int m_fd;
    int m_bufWidth;
    int m_bufHeight;
    int m_bufStride;
    void *m_frameBuffer; /* set from run() — used by _pw_stream_process */
    void *m_pwStream;   /* opaque pw_stream* — set from run() */
};

class WaylandGrabber : public GrabberBase
{
    Q_OBJECT
public:
    WaylandGrabber(QObject *parent, GrabberContext *context);
    virtual ~WaylandGrabber();

    DECLARE_GRABBER_NAME("WaylandGrabber")

protected:
    virtual GrabResult grabScreens();
    virtual bool reallocate(const QList<ScreenInfo> &screens);
    virtual QList<ScreenInfo> * screensWithWidgets(QList<ScreenInfo> *result, const QList<GrabWidget *> &grabWidgets);

private slots:
    void onScreencastSessionReady(void *result);  /* GAsyncResult* */
    void onSessionStartReady(void *result);       /* GAsyncResult* */
    void onPortalSessionClosed();
    void onPipeWireFrameReady();

private:
    void initPortal();
    void cleanupPortal();
    void onPipeWireData();

private:
    // Portal state
    void *m_portal;       /* XdpPortal* */
    void *m_session;      /* XdpSession* */

    // PipeWire worker thread (stored for cleanup)
    PipeWireWorker *m_pwWorker;

    // Frame buffer (largest screen)
    unsigned char *m_frameBuffer;
    int m_frameWidth;
    int m_frameHeight;
    int m_frameStride;
    bool m_frameReady;

    // Main loop integration
    void *m_mainLoop;     /* GMainLoop* */
    bool m_portalInitialized;
};

#endif // WAYLAND_GRAB_SUPPORT
