/*
 * WaylandGrabber.cpp
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

#ifdef WAYLAND_GRAB_SUPPORT

/* Include portal.h FIRST, with glib 'public' macro undef. */
#include "portal-compat.h"
#include <portal.h>

/* PipeWire headers */
#include <pipewire/pipewire.h>
#include <pipewire/core.h>
#include <pipewire/loop.h>
#include <pipewire/stream.h>
#include <pipewire/properties.h>
#include <pipewire/keys.h>
#include <pipewire/context.h>

/* SPA headers */
#include <spa/param/param.h>
#include <spa/param/format.h>
#include <spa/param/video/format.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/raw-utils.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/utils/defs.h>

#include "WaylandGrabber.hpp"

#include <QApplication>
#include <QScreen>

#include <cstring>
#include <algorithm>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <cstdio>
#include <atomic>
#include <QImage>

/* ------------------------------------------------------------------ */
/*  C callback wrappers for GSignal (no lambdas with GCallback)      */
/* ------------------------------------------------------------------ */

static void
_portalSessionClosedCallback(XdpSession * /*session*/, gpointer userData)
{
    auto *self = static_cast<WaylandGrabber *>(userData);
    QMetaObject::invokeMethod(self, "onPortalSessionClosed");
}

/* ------------------------------------------------------------------ */
/*  PipeWireWorker — runs pw_loop in a background thread              */
/* ------------------------------------------------------------------ */

void PipeWireWorker::setFd(int fd) { m_fd = fd; }
void PipeWireWorker::setNodeId(uint32_t nodeId) { m_nodeId = nodeId; }
void PipeWireWorker::setBufferSize(int width, int height, int stride) {
    m_bufWidth = width;
    m_bufHeight = height;
    m_bufStride = stride;
}
void PipeWireWorker::setFrameBuffer(void *buf) { m_frameBuffer = buf; }

/* ------------------------------------------------------------------ */
/*  Static helpers — called from the PW thread (no Qt)                */
/* ------------------------------------------------------------------ */

static void
_pw_stream_state_changed(void *userData,
                         enum pw_stream_state old,
                         enum pw_stream_state state,
                         const char *error)
{
    auto *self = static_cast<PipeWireWorker *>(userData);
    Q_UNUSED(old);

    if (state == PW_STREAM_STATE_ERROR) {
        qCritical() << "PipeWireWorker: stream error:" << (error ? error : "(unknown)");
    } else if (state == PW_STREAM_STATE_STREAMING) {
        /* Stream is ready to receive/process frames */
        QMetaObject::invokeMethod(self, "frameReady");
    }
}

static void
_pw_stream_param_changed(void *userData, uint32_t id, const struct spa_pod *param)
{
    auto *self = static_cast<PipeWireWorker *>(userData);
    Q_UNUSED(self);

    if (id != SPA_PARAM_EnumFormat && id != SPA_PARAM_Format) {
        return;
    }

    struct spa_video_info_raw videoInfo;
    memset(&videoInfo, 0, sizeof(videoInfo));

    if (spa_format_video_raw_parse(param, &videoInfo) < 0) {
        qWarning() << "PipeWireWorker: failed to parse video format";
        return;
    }

    DEBUG_LOW_LEVEL << "PipeWireWorker: video format:"
                    << "size=" << videoInfo.size.width << "x" << videoInfo.size.height
                    << "format=" << videoInfo.format
                    << "framerate=" << videoInfo.framerate.num << "/" << videoInfo.framerate.denom;

    /* We expect BGRA or ARGB */
    if (videoInfo.format != SPA_VIDEO_FORMAT_BGRA &&
        videoInfo.format != SPA_VIDEO_FORMAT_ARGB &&
        videoInfo.format != SPA_VIDEO_FORMAT_RGBA) {
        qWarning() << "PipeWireWorker: unsupported format" << videoInfo.format
                   << "(expected BGRA/ARGB/RGBA)";
        return;
    }

    /* Update buffer size if changed — update via userData */
    if (videoInfo.size.width != static_cast<uint32_t>(self->m_bufWidth) ||
        videoInfo.size.height != static_cast<uint32_t>(self->m_bufHeight)) {
        self->m_bufWidth  = static_cast<int>(videoInfo.size.width);
        self->m_bufHeight = static_cast<int>(videoInfo.size.height);
        self->m_bufStride = self->m_bufWidth * 4;
        DEBUG_LOW_LEVEL << "PipeWireWorker: buffer size updated to"
                        << self->m_bufWidth << "x" << self->m_bufHeight;
    }

    /* Send update params: ParamMeta (Header) + ParamBuffers (MemPtr).
     * This is the OBS Studio pattern — after format negotiation, tell
     * PipeWire what metadata and buffer types we support. */
    uint8_t updateBuf[1024];
    struct spa_pod_builder ub = SPA_POD_BUILDER_INIT(updateBuf, sizeof(updateBuf));
    const struct spa_pod *updateParams[2];
    int nUpdate = 0;

    /* ParamMeta: Header */
    struct spa_pod_frame uf1;
    spa_pod_builder_push_object(&ub, &uf1, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
    spa_pod_builder_add(&ub, SPA_PARAM_META_type,    SPA_POD_Id(SPA_META_Header),
                        SPA_PARAM_META_size,    SPA_POD_Int(sizeof(struct spa_meta_header)),
                        0);
    updateParams[nUpdate++] = reinterpret_cast<const struct spa_pod *>(spa_pod_builder_pop(&ub, &uf1));

    /* ParamBuffers: accept MemPtr buffers */
    struct spa_pod_frame uf2;
    spa_pod_builder_push_object(&ub, &uf2, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
    spa_pod_builder_add(&ub, SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(1 << SPA_DATA_MemPtr), 0);
    updateParams[nUpdate++] = reinterpret_cast<const struct spa_pod *>(spa_pod_builder_pop(&ub, &uf2));

    if (nUpdate > 0) {
        struct pw_stream *stream = static_cast<struct pw_stream *>(self->m_pwStream);
        if (stream) {
            pw_stream_update_params(stream, updateParams, nUpdate);
            DEBUG_LOW_LEVEL << "PipeWireWorker: sent update params (meta + buffers)";
        }
    }
}

/* Debug frame-saving toggle. Written on the GUI thread (setSaveFramesEnabled),
 * read on the worker thread (_pw_stream_process). */
static std::atomic<bool> s_saveFramesEnabled(false);

/* Event-source callback — invoked on the worker thread when stopWorker()
 * (GUI thread) signals the source. Sets the flag that makes run() exit its
 * main loop and tear down the PipeWire objects on the worker thread. */
static void
_pw_loop_stop_event(void *data, uint64_t count)
{
    Q_UNUSED(count);
    auto *self = static_cast<PipeWireWorker *>(data);
    self->m_stopRequested = true;
}

static void
_pw_stream_process(void *userData)
{
    auto *self = static_cast<PipeWireWorker *>(userData);
    struct pw_stream *stream = static_cast<struct pw_stream *>(self->m_pwStream);
    unsigned char *dstBuf = static_cast<unsigned char *>(self->m_frameBuffer);

    struct pw_buffer *buffer = pw_stream_dequeue_buffer(stream);
    if (!buffer) {
        return;
    }

    struct spa_buffer *spaBuf = buffer->buffer;
    if (spaBuf->n_datas < 1) {
        pw_stream_queue_buffer(stream, buffer);
        return;
    }

    struct spa_data *data = &spaBuf->datas[0];

    /* CRITICAL: Reset data->data so we always re-mmap fresh.
     * PipeWire reuses buffers — the data pointer from a previous
     * frame may be stale or already unmapped. */
    if (data->data && data->data != MAP_FAILED) {
        munmap(data->data, data->maxsize);
    }
    data->data = nullptr;

    if (!(data->flags & SPA_DATA_FLAG_READABLE) || data->maxsize <= 0) {
        pw_stream_queue_buffer(stream, buffer);
        return;
    }

    /* Map the data if not already mapped */
    if (!data->data) {
        data->data = mmap(NULL, data->maxsize, PROT_READ, MAP_PRIVATE, static_cast<int>(data->fd), data->mapoffset);
        if (data->data == MAP_FAILED) {
            data->data = nullptr;
            pw_stream_queue_buffer(stream, buffer);
            return;
        }
    }

    /* Determine frame size from buffer chunk */
    struct spa_chunk *chunk = data->chunk;
    if (!chunk) {
        pw_stream_queue_buffer(stream, buffer);
        return;
    }

    /* Copy frame data — handle stride differences */
    int srcStride = self->m_bufWidth * 4;
    int dstStride = self->m_bufStride;
    int frameHeight = self->m_bufHeight;

    /* Calculate actual frame dimensions from data */
    int actualBytes = chunk->size;
    if (actualBytes <= 0 || static_cast<uint32_t>(actualBytes) > data->maxsize) {
        actualBytes = static_cast<int>(data->maxsize);
    }

    int actualHeight = actualBytes / srcStride;
    if (actualHeight > frameHeight) actualHeight = frameHeight;

    const unsigned char *src = static_cast<const unsigned char *>(data->data);

    if (dstBuf && srcStride == dstStride && actualHeight == frameHeight) {
        /* Direct copy — same stride */
        memcpy(dstBuf, src, actualBytes);
    } else if (dstBuf) {
        /* Copy line by line — stride mismatch */
        for (int y = 0; y < actualHeight; y++) {
            memcpy(dstBuf + y * dstStride, src + y * srcStride, qMin(srcStride, dstStride));
        }
    }

    /* Unmap */
    if (data->data && data->data != MAP_FAILED) {
        munmap(data->data, data->maxsize);
        data->data = nullptr;
    }

    /* Debug: save the frame as PNG when enabled (worker thread).
     * QImage::Format_ARGB32 matches the BGRA memory layout of the
     * negotiated SPA_VIDEO_FORMAT_BGRA stream. The counter wraps so at
     * most ~100 recent frames stay on disk (a ring). */
    if (s_saveFramesEnabled && dstBuf) {
        const int w = self->m_bufWidth;
        const int h = self->m_bufHeight;
        const int stride = self->m_bufStride;
        if (w > 0 && h > 0 && stride >= w * 4) {
            static uint32_t s_dbgFrameCounter = 0;
            static bool s_dbgDirCreated = false;
            if (!s_dbgDirCreated) {
                mkdir("/tmp/prismatic-frames", 0755);
                s_dbgDirCreated = true;
            }
            QImage img(w, h, QImage::Format_ARGB32);
            const uchar *src = reinterpret_cast<const uchar *>(self->m_frameBuffer);
            for (int y = 0; y < h; ++y) {
                memcpy(img.scanLine(y), src + y * stride, w * 4);
            }
            char path[256];
            snprintf(path, sizeof(path), "/tmp/prismatic-frames/frame_%06u.png",
                     s_dbgFrameCounter);
            img.save(path);
            s_dbgFrameCounter = (s_dbgFrameCounter + 1) % 100;
        }
    }

    /* Signal that a frame is available */
    QMetaObject::invokeMethod(self, "frameReady");

    /* Queue buffer back */
    pw_stream_queue_buffer(stream, buffer);
}

static const struct pw_stream_events pw_stream_events_versioned = {
    .version = PW_VERSION_STREAM_EVENTS,
    .state_changed = _pw_stream_state_changed,
    .param_changed = _pw_stream_param_changed,
    .process = _pw_stream_process,
};

/* Core callbacks — run on the PW loop thread */
static void
_pw_core_info(void *userData, const struct pw_core_info *info)
{
    Q_UNUSED(userData);
    DEBUG_LOW_LEVEL << "PipeWireWorker: core info version" << info->version;
}

static void
_pw_core_done(void *userData, uint32_t id, int seq)
{
    auto *self = static_cast<PipeWireWorker *>(userData);
    Q_UNUSED(seq);
    if (id == PW_ID_CORE) {
        self->m_coreSyncDone = true;
        DEBUG_LOW_LEVEL << "PipeWireWorker: core sync done";
    }
}

static void
_pw_core_error(void *userData, uint32_t id, int seq, int res, const char *message)
{
    Q_UNUSED(userData);
    Q_UNUSED(id);
    Q_UNUSED(seq);
    qWarning() << "PipeWireWorker: core error res=" << res << (message ? message : "");
}

static const struct pw_core_events pw_core_events_versioned = {
    .version = PW_VERSION_CORE_EVENTS,
    .info = _pw_core_info,
    .done = _pw_core_done,
    .error = _pw_core_error,
};

void PipeWireWorker::run()
{
    /* PipeWire objects — all accessed only from run() thread */
    struct pw_loop *pwLoop    = nullptr;
    struct pw_context *pwContext = nullptr;
    struct pw_core *pwCore = nullptr;
    struct pw_stream *pwStream = nullptr;
    struct spa_hook streamListener;
    memset(&streamListener, 0, sizeof(streamListener));

    /* Create PipeWire objects */
    pwLoop = pw_loop_new(nullptr);
    if (!pwLoop) {
        qCritical() << "PipeWireWorker: failed to create pw_loop";
        return;
    }
    m_pwLoop = pwLoop; /* expose to stopWorker() for pw_loop_signal_event (GUI thread) */

    pwContext = pw_context_new(pwLoop, nullptr, 0);
    if (!pwContext) {
        qCritical() << "PipeWireWorker: failed to create pw_context";
        pw_loop_destroy(pwLoop);
        return;
    }

    /* Connect to PipeWire remote via portal FD */
    pwCore = pw_context_connect_fd(pwContext, m_fd, nullptr, 0);
    if (!pwCore) {
        qCritical() << "PipeWireWorker: failed to connect pw_core";
        pw_context_destroy(pwContext);
        pw_loop_destroy(pwLoop);
        return;
    }

    /* Synchronize with the core (OBS pattern).
     *
     * pw_core_hello() is fire-and-forget: it sends the hello message but does
     * NOT wait for the daemon to become ready, so pw_stream_connect() below
     * would run before the core is up and the stream would hang on CONNECTING
     * (no frames ever). OBS instead waits for the core .done event via
     * pw_core_sync(). We use the same approach with a single-threaded loop:
     * add the core listener, request a sync, then pump the loop until the
     * .done callback sets m_coreSyncDone. */
    struct spa_hook coreListener;
    memset(&coreListener, 0, sizeof(coreListener));
    pw_core_add_listener(pwCore, &coreListener, &pw_core_events_versioned, this);

    m_coreSyncDone = false;
    m_syncId = pw_core_sync(pwCore, PW_ID_CORE, m_syncId);
    {
        int attempts = 0;
        while (!m_coreSyncDone && attempts < 50) {
            if (pw_loop_iterate(pwLoop, 100) < 0) {
                qWarning() << "PipeWireWorker: core sync loop failed";
                break;
            }
            attempts++;
        }
    }
    if (!m_coreSyncDone) {
        qWarning() << "PipeWireWorker: core sync did not complete in time";
    }

    /* Create stream properties */
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        PW_KEY_ACCESS, "portal",
        nullptr
    );

    /* PipeWire node ID from portal — use m_nodeId if set, otherwise PW_ID_ANY */
    uint32_t targetNodeId = (m_nodeId > 0) ? m_nodeId : PW_ID_ANY;

    /* Create stream.
     *
     * pw_stream_new() stores `props` by reference (it does NOT copy it) — the
     * stream owns the pointer and frees it itself on pw_stream_destroy(). Do
     * NOT call pw_properties_free(props) here: pw_stream_connect() reads
     * stream->props to build and send the connect message, so freeing props
     * leaves a dangling pointer and segfaults mid-connect. */
    pwStream = pw_stream_new(pwCore, "lightpack-wayland-grab", props);

    if (!pwStream) {
        qCritical() << "PipeWireWorker: failed to create pw_stream";
        pw_core_disconnect(pwCore);
        pw_context_destroy(pwContext);
        pw_loop_destroy(pwLoop);
        return;
    }

    /* Add stream listener */
    pw_stream_add_listener(pwStream, &streamListener,
                           &pw_stream_events_versioned, this);

    /* Set pointers accessible from static callbacks */
    this->m_pwStream = pwStream;
    /* m_frameBuffer is set via setFrameBuffer() from the grabber before the loop starts */

    /* Build video format POD params — offer multiple formats for compatibility.
     * OBS Studio pattern: use CHOICE_RANGE for size/framerate so the remote
     * can pick the best match. Use SPA_PARAM_EnumFormat for format proposals. */
    uint8_t podBuffer[2048];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(podBuffer, sizeof(podBuffer));

    /* OBS pattern: CHOICE_RANGE lets remote pick best resolution/framerate.
     * Min=(1,1), max=(8192,4320), preferred=our screen size. */
    // OBS uses plain spa_rectangle / spa_fraction here: the pod-builder
    // choice-range collect reads ->width/->height (->num/->denom) at the
    // *plain* struct offsets. The spa_pod_* wrapper types embed an 8-byte
    // spa_pod header first, so using them would make the collect read
    // value.x/y and advertise a 0x0 size / garbage framerate to the server.
    struct spa_rectangle preferredRect = SPA_RECTANGLE(1920, 1080);
    struct spa_rectangle minRect       = SPA_RECTANGLE(1, 1);
    struct spa_rectangle maxRect       = SPA_RECTANGLE(8192, 4320);
    struct spa_fraction preferredFrac  = SPA_FRACTION(30, 1);
    struct spa_fraction minFrac        = SPA_FRACTION(0, 1);
    struct spa_fraction maxFrac        = SPA_FRACTION(360, 1);

    /* Offer BGRA only — the single packed-RGB format Lightpack can process.
     *
     * The old code offered I420/NV12/BGRA in that order. The portal remote
     * (kwin) tends to pick the first acceptable format, and when it picked
     * I420/NV12 the _pw_stream_param_changed() callback rejected them and
     * returned WITHOUT sending pw_stream_update_params(), so the stream
     * never reached STREAMING. OBS offers only processable packed-RGB
     * formats; we mirror that by offering only BGRA. */
    struct spa_pod_frame fBGRA;
    spa_pod_builder_push_object(&builder, &fBGRA, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&builder, SPA_FORMAT_mediaType,       SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype,              SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                        SPA_FORMAT_VIDEO_format,              SPA_POD_Id(SPA_VIDEO_FORMAT_BGRA),
                        SPA_FORMAT_VIDEO_size,                SPA_POD_CHOICE_RANGE_Rectangle(&preferredRect, &minRect, &maxRect),
                        SPA_FORMAT_VIDEO_framerate,           SPA_POD_CHOICE_RANGE_Fraction(&preferredFrac, &minFrac, &maxFrac),
                        0);
    const struct spa_pod *paramBGRA = reinterpret_cast<const struct spa_pod *>(spa_pod_builder_pop(&builder, &fBGRA));

    /* Connect stream for capture (input direction) — offer BGRA only */
    const struct spa_pod *params[1] = { paramBGRA };
    int ret = pw_stream_connect(pwStream,
                                 PW_DIRECTION_INPUT,
                                 targetNodeId,
                                 static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
                                 params, 1);
    if (ret < 0) {
        qCritical() << "PipeWireWorker: pw_stream_connect failed:" << ret;
        pw_stream_destroy(pwStream);
        pw_core_disconnect(pwCore);
        pw_context_destroy(pwContext);
        pw_loop_destroy(pwLoop);
        return;
    }

    DEBUG_LOW_LEVEL << "PipeWireWorker: stream connected, starting loop";

    /* Cross-thread stop: an event source lets stopWorker() (GUI thread) wake
     * the blocked pw_loop_iterate below via pw_loop_signal_event. Created here
     * (after all fallible setup) so it is torn down in the single post-loop
     * cleanup. */
    m_eventSource = pw_loop_add_event(pwLoop, _pw_loop_stop_event, this);

    /* Run the PipeWire main loop until stopWorker() is called from the GUI
     * thread (which sets m_stopRequested and breaks the loop) or the loop
     * errors out. All PipeWire objects are destroyed below, on this thread. */
    while (!m_stopRequested) {
        if (pw_loop_iterate(pwLoop, -1) < 0) {
            break;
        }
    }

    /* Cleanup */
    DEBUG_LOW_LEVEL << "PipeWireWorker: stopping loop";
    if (pwStream) {
        pw_stream_destroy(pwStream);
        pwStream = nullptr;
    }
    if (pwCore) {
        pw_core_disconnect(pwCore);
        pwCore = nullptr;
    }
    if (pwContext) {
        pw_context_destroy(pwContext);
        pwContext = nullptr;
    }
    /* Remove the stop-event source before destroying the loop. */
    if (m_eventSource) {
        pw_loop_destroy_source(pwLoop, static_cast<struct spa_source *>(m_eventSource));
        m_eventSource = nullptr;
    }
    if (pwLoop) {
        pw_loop_destroy(pwLoop);
        pwLoop = nullptr;
    }
    m_pwLoop = nullptr;
}

/* Called from the GUI thread (cleanupPortal). Requests the worker thread to
 * stop: sets the flag so run() exits its loop, and breaks the PipeWire loop
 * (pw_loop_break is designed for cross-thread use). The worker thread performs
 * all PipeWire teardown inside run(); the GUI thread only waits. */
void PipeWireWorker::stopWorker()
{
    m_stopRequested = true;
    /* Wake the blocked pw_loop_iterate (run()) so it re-checks m_stopRequested
     * and exits; the event callback also sets the flag on the worker thread. */
    if (m_pwLoop && m_eventSource) {
        pw_loop_signal_event(static_cast<struct pw_loop *>(m_pwLoop),
                             static_cast<struct spa_source *>(m_eventSource));
    }
}

/* ------------------------------------------------------------------ */
/*  WaylandGrabber                                                    */
/* ------------------------------------------------------------------ */

WaylandGrabber::WaylandGrabber(QObject *parent, GrabberContext *context)
    : GrabberBase(parent, context)
    , m_portal(nullptr)
    , m_session(nullptr)
    , m_pwWorker(nullptr)
    , m_frameBuffer(nullptr)
    , m_frameWidth(0)
    , m_frameHeight(0)
    , m_frameStride(0)
    , m_frameReady(false)
    , m_mainLoop(nullptr)
    , m_portalInitialized(false)
{
    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "WaylandGrabber created";
}

void WaylandGrabber::setSaveFramesEnabled(bool enabled)
{
    s_saveFramesEnabled = enabled;
    if (enabled) {
        /* Best-effort create the debug-frame directory (also done in
         * _pw_stream_process on the first saved frame). */
        mkdir("/tmp/prismatic-frames", 0755);
    }
}

void WaylandGrabber::startGrabbing()
{
    /* Initialize the portal and start the screencast session once, before
     * the grab timer starts. This eventually shows the system permission
     * dialog. The portal is driven by the grab timer, which iterates the
     * GMainContext in grabScreens() to dispatch the async callbacks. */
    if (!m_portalInitialized) {
        initPortal();
    }
    GrabberBase::startGrabbing();
}

void WaylandGrabber::stopGrabbing()
{
    GrabberBase::stopGrabbing();
    cleanupPortal();
}

WaylandGrabber::~WaylandGrabber()
{
    cleanupPortal();
    if (m_frameBuffer) {
        free(m_frameBuffer);
        m_frameBuffer = nullptr;
    }
}

/* ------------------------------------------------------------------ */
/*  screensWithWidgets — enumerate monitors via QScreen               */
/* ------------------------------------------------------------------ */

QList<ScreenInfo> *
WaylandGrabber::screensWithWidgets(QList<ScreenInfo> *result,
                                    const QList<GrabWidget *> &grabWidgets)
{
    result->clear();

    auto screens = QApplication::screens();
    for (int i = 0; i < screens.size(); ++i) {
        QScreen *screen = screens[i];
        ScreenInfo info;
        intptr_t handle = i;
        info.handle = reinterpret_cast<void *>(handle);
        info.rect = screen->geometry();

        for (int k = 0; k < grabWidgets.size(); ++k) {
            if (info.rect.intersects(grabWidgets[k]->deviceFrameGeometry())) {
                result->append(info);
                break;
            }
        }
    }

    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "found" << result->size() << "screens";
    return result;
}

/* ------------------------------------------------------------------ */
/*  reallocate — prepare frame buffer                                 */
/* ------------------------------------------------------------------ */

bool WaylandGrabber::reallocate(const QList<ScreenInfo> &screens)
{
    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "reallocating for" << screens.size() << "screens";

    int maxW = 0, maxH = 0;
    for (const auto &s : screens) {
        maxW = qMax(maxW, s.rect.width());
        maxH = qMax(maxH, s.rect.height());
    }

    if (maxW <= 0 || maxH <= 0) {
        qCritical() << Q_FUNC_INFO << "invalid screen dimensions";
        return false;
    }

    int newStride = maxW * 4; /* BGRA = 4 bytes/pixel */
    int newSize = newStride * maxH;

    if (m_frameBuffer) {
        if (m_frameWidth == maxW && m_frameHeight == maxH && m_frameStride == newStride) {
            DEBUG_LOW_LEVEL << Q_FUNC_INFO << "buffer already correct size";
            return true;
        }
        free(m_frameBuffer);
    }

    m_frameBuffer = static_cast<unsigned char *>(malloc(newSize));
    if (!m_frameBuffer) {
        qCritical() << Q_FUNC_INFO << "failed to allocate frame buffer" << newSize;
        return false;
    }

    memset(m_frameBuffer, 0, newSize);

    m_frameWidth  = maxW;
    m_frameHeight = maxH;
    m_frameStride = newStride;
    m_frameReady  = false;

    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "allocated" << newSize << "bytes"
                    << maxW << "x" << maxH << "stride" << newStride;
    return true;
}

/* ------------------------------------------------------------------ */
/*  grabScreens — check if frame is ready from PipeWire               */
/*  Run one iteration of the GMainContext to process portal events.   */
/* ------------------------------------------------------------------ */

GrabResult WaylandGrabber::grabScreens()
{
    /* Process portal events. The GMainLoop is created in initPortal() and
     * driven manually from this grab timer — g_main_loop_run() is never
     * called, so g_main_loop_is_running() is always false here. Iterate the
     * loop's context directly to dispatch the portal async callbacks
     * (create_screencast_session / session_start completions). */
    if (m_mainLoop) {
        g_main_context_iteration(g_main_loop_get_context(static_cast<GMainLoop *>(m_mainLoop)), false);
    }

    if (!m_frameReady) {
        return GrabResultFrameNotReady;
    }

    /* Frame is ready — reset for next cycle */
    m_frameReady = false;
    return GrabResultOk;
}

/* ------------------------------------------------------------------ */
/*  Slot: PipeWire frame ready — set m_frameReady                     */
/* ------------------------------------------------------------------ */

void WaylandGrabber::onPipeWireFrameReady()
{
    m_frameReady = true;
}

/* ------------------------------------------------------------------ */
/*  Portal: initialize                                                */
/* ------------------------------------------------------------------ */

void WaylandGrabber::initPortal()
{
    if (m_portalInitialized) {
        DEBUG_LOW_LEVEL << Q_FUNC_INFO << "portal already initialized";
        return;
    }

    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "initializing portal (libportal)";

    /* Create portal instance */
    m_portal = xdp_portal_new();
    if (!m_portal) {
        qCritical() << Q_FUNC_INFO << "failed to create XdpPortal";
        emit frameGrabAttempted(GrabResultError);
        return;
    }

    /* Create GMainLoop for portal event processing */
    m_mainLoop = g_main_loop_new(nullptr, FALSE);
    if (!m_mainLoop) {
        qCritical() << Q_FUNC_INFO << "failed to create main loop";
        emit frameGrabAttempted(GrabResultError);
        return;
    }

    /* Create screencast session */
    xdp_portal_create_screencast_session(
        static_cast<XdpPortal *>(m_portal),
        XDP_OUTPUT_MONITOR,              /* outputs: monitors only */
        XDP_SCREENCAST_FLAG_MULTIPLE,    /* flags: multi-monitor */
        XDP_CURSOR_MODE_HIDDEN,          /* cursor: hidden */
        XDP_PERSIST_MODE_NONE,           /* persist: none */
        nullptr,                         /* restore_token */
        nullptr,                         /* cancellable */
        [](GObject *, GAsyncResult *result, gpointer userData) {
            auto *self = static_cast<WaylandGrabber *>(userData);
            QMetaObject::invokeMethod(self, "onScreencastSessionReady",
                                      Q_ARG(void *, result));
        },
        this);

    m_portalInitialized = true;
}

/* ------------------------------------------------------------------ */
/*  Slot: Screencast session created → Start (shows permission dialog) */
/* ------------------------------------------------------------------ */

void WaylandGrabber::onScreencastSessionReady(void *result)
{
    GError *error = nullptr;

    m_session = xdp_portal_create_screencast_session_finish(static_cast<XdpPortal *>(m_portal), static_cast<GAsyncResult *>(result), &error);

    if (error) {
        qWarning() << "WaylandGrabber: failed to create screencast session:"
                   << error->message;
        g_error_free(error);
        emit frameGrabAttempted(GrabResultError);
        return;
    }

    if (!m_session) {
        qCritical() << Q_FUNC_INFO << "screencast session is null";
        emit frameGrabAttempted(GrabResultError);
        return;
    }

    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "screencast session created";

    /* Connect to session signals */
    g_signal_connect_data(static_cast<XdpSession *>(m_session), "closed",
                          G_CALLBACK(_portalSessionClosedCallback),
                          g_object_ref(this),
                          [](gpointer userData, GClosure *) {
                              g_object_unref(userData);
                          },
                          G_CONNECT_AFTER);

    /* Start session — this shows the system permission dialog */
    xdp_session_start(static_cast<XdpSession *>(m_session),
                      nullptr, /* parent (nullptr for Wayland) */
                      nullptr, /* cancellable */
                      [](GObject *, GAsyncResult *result, gpointer userData) {
                          auto *self = static_cast<WaylandGrabber *>(userData);
                          QMetaObject::invokeMethod(self, "onSessionStartReady",
                                                    Q_ARG(void *, result));
                      },
                      this);
}

/* ------------------------------------------------------------------ */
/*  Slot: Session started → Get streams + PipeWire FD                 */
/* ------------------------------------------------------------------ */

void WaylandGrabber::onSessionStartReady(void *result)
{
    GError *error = nullptr;
    gboolean started = xdp_session_start_finish(static_cast<XdpSession *>(m_session), static_cast<GAsyncResult *>(result), &error);

    if (error) {
        qWarning() << "WaylandGrabber: session start failed:" << error->message;
        g_error_free(error);
        emit frameGrabAttempted(GrabResultError);
        return;
    }

    if (!started) {
        qCritical() << Q_FUNC_INFO << "session start returned false (user cancelled?)";
        emit frameGrabAttempted(GrabResultError);
        return;
    }

    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "session started successfully";

    /* Get stream info from portal */
    GVariant *streamsVar = xdp_session_get_streams(static_cast<XdpSession *>(m_session));
    guint selectedNodeId = 0;
    if (streamsVar) {
        guint nStreams = g_variant_n_children(streamsVar);
        DEBUG_LOW_LEVEL << Q_FUNC_INFO << "streams:" << nStreams;

        for (guint i = 0; i < nStreams; ++i) {
            guint nodeId = 0;
            GVariant *propsVar = nullptr;

            g_variant_get(g_variant_get_child_value(streamsVar, i),
                          "(u@a{sv})", &nodeId, &propsVar);

            guint64 pipeWireSerial = 0;
            guint sourceType = 0;

            GVariantIter propIter;
            const char *key = nullptr;
            GVariant *value = nullptr;
            g_variant_get(propsVar, "a{sv}", &propIter);
            while (g_variant_iter_loop(&propIter, "{sv}", &key, &value)) {
                if (strcmp(key, "pipewire-serial") == 0) {
                    pipeWireSerial = g_variant_get_uint64(value);
                } else if (strcmp(key, "source_type") == 0) {
                    sourceType = g_variant_get_uint32(value);
                }
                g_variant_unref(value);
            }
            g_variant_unref(propsVar);

            DEBUG_LOW_LEVEL << "  stream[" << i << "]:"
                            << "nodeId=" << nodeId
                            << "serial=" << pipeWireSerial
                            << "type=" << sourceType;

            /* OBS: the KDE portal sometimes reports more than one stream and
             * only the LAST one is the actual capture node — keep overwriting
             * so the final value is the last stream's node id. */
            selectedNodeId = nodeId;
        }

        g_variant_unref(streamsVar);
    }

    /* Get PipeWire remote FD */
    int pwFd = xdp_session_open_pipewire_remote(static_cast<XdpSession *>(m_session));
    if (pwFd < 0) {
        qCritical() << Q_FUNC_INFO << "failed to get PipeWire remote FD";
        emit frameGrabAttempted(GrabResultError);
        return;
    }

    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "PipeWire remote FD:" << pwFd;

    /* Start PipeWire worker thread */
    m_pwWorker = new PipeWireWorker(this);
    m_pwWorker->setFd(pwFd);
    m_pwWorker->setNodeId(selectedNodeId);
    m_pwWorker->setBufferSize(m_frameWidth, m_frameHeight, m_frameStride);
    m_pwWorker->setFrameBuffer(m_frameBuffer);

    /* Connect frame ready signal to grabber's slot */
    connect(m_pwWorker, &PipeWireWorker::frameReady,
            this, &WaylandGrabber::onPipeWireFrameReady);

    /* Start the worker — it will run pw_loop_iterate() in background */
    m_pwWorker->start();

    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "PipeWire worker thread started";

    emit frameGrabAttempted(GrabResultOk);
}

/* ------------------------------------------------------------------ */
/*  Slot: Session closed                                              */
/* ------------------------------------------------------------------ */

void WaylandGrabber::onPortalSessionClosed()
{
    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "portal session closed";

    /* Quit the portal main loop */
    if (m_mainLoop && g_main_loop_is_running(static_cast<GMainLoop *>(m_mainLoop))) {
        g_main_loop_quit(static_cast<GMainLoop *>(m_mainLoop));
    }
}

/* ------------------------------------------------------------------ */
/*  Cleanup                                                           */
/* ------------------------------------------------------------------ */

void WaylandGrabber::cleanupPortal()
{
    /* Stop the PipeWire worker. The worker thread performs all PipeWire
     * teardown (destroy stream/core/context/loop) inside run(); the GUI
     * thread only requests a stop and waits. Do NOT destroy PW objects here —
     * that races the worker thread driving the loop and caused the crash. */
    if (m_pwWorker) {
        m_pwWorker->stopWorker();
        m_pwWorker->wait(3000);
        if (m_pwWorker->isRunning()) {
            m_pwWorker->terminate();
            m_pwWorker->wait();
        }
        delete m_pwWorker;
        m_pwWorker = nullptr;
    }

    /* Quit the portal main loop */
    if (m_mainLoop && g_main_loop_is_running(static_cast<GMainLoop *>(m_mainLoop))) {
        g_main_loop_quit(static_cast<GMainLoop *>(m_mainLoop));
    }

    /* Close portal session */
    if (m_session) {
        xdp_session_close(static_cast<XdpSession *>(m_session));
        m_session = nullptr;
    }

    /* Destroy portal */
    if (m_portal) {
        g_object_unref(static_cast<XdpPortal *>(m_portal));
        m_portal = nullptr;
    }

    /* Destroy main loop */
    if (m_mainLoop) {
        g_main_loop_unref(static_cast<GMainLoop *>(m_mainLoop));
        m_mainLoop = nullptr;
    }

    m_frameReady = false;
    m_portalInitialized = false;

    DEBUG_LOW_LEVEL << Q_FUNC_INFO << "portal cleaned up";
}

#endif // WAYLAND_GRAB_SUPPORT
