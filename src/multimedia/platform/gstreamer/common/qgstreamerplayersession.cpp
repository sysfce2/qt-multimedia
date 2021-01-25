/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <private/qgstreamerplayersession_p.h>
#include <private/qgstreamerbushelper_p.h>

#include <private/qgstreamervideorendererinterface_p.h>
#include <private/qgstutils_p.h>
#include <private/qgstvideorenderersink_p.h>

#include <gst/gstvalue.h>
#include <gst/base/gstbasesrc.h>

#include <QtMultimedia/qmediametadata.h>
#include <QtCore/qdatetime.h>
#include <QtCore/qdebug.h>
#include <QtCore/qsize.h>
#include <QtCore/qtimer.h>
#include <QtCore/qdebug.h>
#include <QtCore/qdir.h>
#include <QtCore/qstandardpaths.h>
#include <qvideorenderercontrol.h>
#include <QUrlQuery>
#include <private/qaudiodeviceinfo_gstreamer_p.h>

//#define DEBUG_PLAYBIN

QT_BEGIN_NAMESPACE

typedef enum {
    GST_PLAY_FLAG_VIDEO         = 0x00000001,
    GST_PLAY_FLAG_AUDIO         = 0x00000002,
    GST_PLAY_FLAG_TEXT          = 0x00000004,
    GST_PLAY_FLAG_VIS           = 0x00000008,
    GST_PLAY_FLAG_SOFT_VOLUME   = 0x00000010,
    GST_PLAY_FLAG_NATIVE_AUDIO  = 0x00000020,
    GST_PLAY_FLAG_NATIVE_VIDEO  = 0x00000040,
    GST_PLAY_FLAG_DOWNLOAD      = 0x00000080,
    GST_PLAY_FLAG_BUFFERING     = 0x000000100
} GstPlayFlags;

QGstreamerPlayerSession::QGstreamerPlayerSession(QObject *parent)
    : QObject(parent)
{
    initPlaybin();
}

void QGstreamerPlayerSession::initPlaybin()
{
    m_playbin = gst_element_factory_make("playbin", nullptr);
    if (m_playbin) {
        //GST_PLAY_FLAG_NATIVE_VIDEO omits configuration of ffmpegcolorspace and videoscale,
        //since those elements are included in the video output bin when necessary.
        int flags = GST_PLAY_FLAG_VIDEO | GST_PLAY_FLAG_AUDIO;
        QByteArray envFlags = qgetenv("QT_GSTREAMER_PLAYBIN_FLAGS");
        if (!envFlags.isEmpty()) {
            flags |= envFlags.toInt();
        }
        g_object_set(G_OBJECT(m_playbin), "flags", flags, nullptr);

        // setup audio output
        m_volumeElement = gst_element_factory_make("volume", "volumeelement");
        Q_ASSERT(m_volumeElement);
        m_audioBin = GST_BIN(gst_bin_new("audio-output-bin"));
        gst_bin_add(m_audioBin, m_volumeElement);

        GstPad *pad = gst_element_get_static_pad(m_volumeElement, "sink");
        gst_element_add_pad(GST_ELEMENT(m_audioBin), gst_ghost_pad_new("sink", pad));
        gst_object_unref(GST_OBJECT(pad));

        qDebug() << "setting up player audio-sink property";
        g_object_set(G_OBJECT(m_playbin), "audio-sink", GST_ELEMENT(m_audioBin), nullptr);

        updateAudioSink();
    }

    static const auto convDesc = qEnvironmentVariable("QT_GSTREAMER_PLAYBIN_CONVERT");
    GError *err = nullptr;
    auto convPipeline = !convDesc.isEmpty() ? convDesc.toLatin1().constData() : "identity";
    auto convElement = gst_parse_launch(convPipeline, &err);
    if (err) {
        qWarning() << "Error:" << convDesc << ":" << QLatin1String(err->message);
        g_clear_error(&err);
    }
    m_videoIdentity = convElement;

    m_nullVideoSink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(G_OBJECT(m_nullVideoSink), "sync", true, nullptr);
    gst_object_ref(GST_OBJECT(m_nullVideoSink));

    m_videoOutputBin = gst_bin_new("video-output-bin");
    // might not get a parent, take ownership to avoid leak
    qt_gst_object_ref_sink(GST_OBJECT(m_videoOutputBin));

    GstElement *videoOutputSink = m_videoIdentity;
#if QT_CONFIG(gstreamer_gl)
    if (QGstUtils::useOpenGL()) {
        videoOutputSink = gst_element_factory_make("glupload", nullptr);
        GstElement *colorConvert = gst_element_factory_make("glcolorconvert", nullptr);
        gst_bin_add_many(GST_BIN(m_videoOutputBin), videoOutputSink, colorConvert, m_videoIdentity, m_nullVideoSink, nullptr);
        gst_element_link_many(videoOutputSink, colorConvert, m_videoIdentity, nullptr);
    } else {
        gst_bin_add_many(GST_BIN(m_videoOutputBin), m_videoIdentity, m_nullVideoSink, nullptr);
    }
#else
    gst_bin_add_many(GST_BIN(m_videoOutputBin), m_videoIdentity, m_nullVideoSink, nullptr);
#endif
    gst_element_link(m_videoIdentity, m_nullVideoSink);

    m_videoSink = m_nullVideoSink;

    // add ghostpads
    GstPad *pad = gst_element_get_static_pad(videoOutputSink, "sink");
    gst_element_add_pad(GST_ELEMENT(m_videoOutputBin), gst_ghost_pad_new("sink", pad));
    gst_object_unref(GST_OBJECT(pad));

    if (m_playbin != 0) {
        // Sort out messages
        setBus(gst_element_get_bus(m_playbin));

        g_object_set(G_OBJECT(m_playbin), "video-sink", m_videoOutputBin, nullptr);

        g_signal_connect(G_OBJECT(m_playbin), "notify::source", G_CALLBACK(playbinNotifySource), this);
        g_signal_connect(G_OBJECT(m_playbin), "element-added",  G_CALLBACK(handleElementAdded), this);

        g_signal_connect(G_OBJECT(m_playbin), "video-changed", G_CALLBACK(handleStreamsChange), this);
        g_signal_connect(G_OBJECT(m_playbin), "audio-changed", G_CALLBACK(handleStreamsChange), this);
        g_signal_connect(G_OBJECT(m_playbin), "text-changed", G_CALLBACK(handleStreamsChange), this);

#if QT_CONFIG(gstreamer_app)
        g_signal_connect(G_OBJECT(m_playbin), "deep-notify::source", G_CALLBACK(configureAppSrcElement), this);
#endif

        m_pipeline = m_playbin;
        gst_object_ref(GST_OBJECT(m_pipeline));
    }
}

QGstreamerPlayerSession::~QGstreamerPlayerSession()
{
    if (m_pipeline) {
        stop();


        delete m_busHelper;
        m_busHelper = nullptr;
        resetElements();
    }
}

template <class T>
static inline void resetGstObject(T *&obj, T *v = nullptr)
{
    if (obj)
        gst_object_unref(GST_OBJECT(obj));

    obj = v;
}

void QGstreamerPlayerSession::resetElements()
{
    setBus(nullptr);
    resetGstObject(m_playbin);
    resetGstObject(m_pipeline);
    resetGstObject(m_nullVideoSink);
    resetGstObject(m_videoOutputBin);

    m_audioSink = nullptr;
    m_volumeElement = nullptr;
    m_videoIdentity = nullptr;
    m_pendingVideoSink = nullptr;
    m_videoSink = nullptr;
}

GstElement *QGstreamerPlayerSession::playbin() const
{
    return m_playbin;
}

#if QT_CONFIG(gstreamer_app)
void QGstreamerPlayerSession::configureAppSrcElement(GObject* object, GObject *orig, GParamSpec *pspec, QGstreamerPlayerSession* self)
{
    Q_UNUSED(object);
    Q_UNUSED(pspec);

    if (!self->appsrc())
        return;

    GstElement *appsrc;
    g_object_get(orig, "source", &appsrc, nullptr);

    if (!self->appsrc()->setup(appsrc))
        qWarning()<<"Could not setup appsrc element";

    g_object_unref(G_OBJECT(appsrc));
}
#endif

void QGstreamerPlayerSession::loadFromStream(const QNetworkRequest &request, QIODevice *appSrcStream)
{
#if QT_CONFIG(gstreamer_app)
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO;
#endif
    m_request = request;
    m_duration = 0;
    m_lastPosition = 0;

    if (!m_appSrc)
        m_appSrc = new QGstAppSrc(this);
    m_appSrc->setStream(appSrcStream);

    if (!parsePipeline() && m_playbin) {
        m_tags.clear();
        emit tagsChanged();

        g_object_set(G_OBJECT(m_playbin), "uri", "appsrc://", nullptr);

        if (!m_streamTypes.isEmpty()) {
            m_streamProperties.clear();
            m_streamTypes.clear();

            emit streamsChanged();
        }
    }
#endif
}

void QGstreamerPlayerSession::loadFromUri(const QNetworkRequest &request)
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO << request.url();
#endif
    m_request = request;
    m_duration = 0;
    m_lastPosition = 0;

#if QT_CONFIG(gstreamer_app)
    if (m_appSrc) {
        m_appSrc->deleteLater();
        m_appSrc = 0;
    }
#endif

    if (!parsePipeline() && m_playbin) {
        m_tags.clear();
        emit tagsChanged();

        g_object_set(G_OBJECT(m_playbin), "uri", m_request.url().toEncoded().constData(), nullptr);

        if (!m_streamTypes.isEmpty()) {
            m_streamProperties.clear();
            m_streamTypes.clear();

            emit streamsChanged();
        }
    }
}

bool QGstreamerPlayerSession::parsePipeline()
{
    if (m_request.url().scheme() != QLatin1String("gst-pipeline")) {
        if (!m_playbin) {
            resetElements();
            initPlaybin();
            updateVideoRenderer();
        }
        return false;
    }

    // Set current surface to video sink before creating a pipeline.
    auto renderer = qobject_cast<QVideoRendererControl *>(m_videoOutput);
    if (renderer)
        QGstVideoRendererSink::setSurface(renderer->surface());

    QString url = m_request.url().toString(QUrl::RemoveScheme);
    QString desc = QUrl::fromPercentEncoding(url.toLatin1().constData());
    GError *err = nullptr;
    GstElement *pipeline = gst_parse_launch(desc.toLatin1().constData(), &err);
    if (err) {
        auto errstr = QLatin1String(err->message);
        qWarning() << "Error:" << desc << ":" << errstr;
        emit error(QMediaPlayer::FormatError, errstr);
        g_clear_error(&err);
    }

    return setPipeline(pipeline);
}

static void gst_foreach(GstIterator *it, const std::function<bool(GstElement *)> &cmp)
{
    GValue value = G_VALUE_INIT;
    while (gst_iterator_next (it, &value) == GST_ITERATOR_OK) {
        auto child = static_cast<GstElement*>(g_value_get_object(&value));
        if (cmp(child))
            break;
    }

    gst_iterator_free(it);
    g_value_unset(&value);
}

bool QGstreamerPlayerSession::setPipeline(GstElement *pipeline)
{
    GstBus *bus = pipeline ? gst_element_get_bus(pipeline) : nullptr;
    if (!bus)
        return false;

    if (m_playbin)
        gst_element_set_state(m_playbin, GST_STATE_NULL);

    resetElements();
    setBus(bus);
    m_pipeline = pipeline;

    if (m_renderer) {
        gst_foreach(gst_bin_iterate_sinks(GST_BIN(pipeline)),
            [this](GstElement *child) {
                if (qstrcmp(GST_OBJECT_NAME(child), "qtvideosink") == 0) {
                    m_renderer->setVideoSink(child);
                    return true;
                }
                return false;
            });
    }

#if QT_CONFIG(gstreamer_app)
    if (m_appSrc) {
        gst_foreach(gst_bin_iterate_sources(GST_BIN(pipeline)),
            [this](GstElement *child) {
                if (qstrcmp(qt_gst_element_get_factory_name(child), "appsrc") == 0) {
                    m_appSrc->setup(child);
                    return true;
                }
                return false;
            });
    }
#endif

    emit pipelineChanged();
    return true;
}

void QGstreamerPlayerSession::setBus(GstBus *bus)
{
    resetGstObject(m_bus, bus);

    // It might still accept gst messages.
    if (m_busHelper)
        m_busHelper->deleteLater();
    m_busHelper = nullptr;

    if (!m_bus)
        return;

    m_busHelper = new QGstreamerBusHelper(m_bus, this);
    m_busHelper->installMessageFilter(this);

    if (m_videoOutput)
        m_busHelper->installMessageFilter(m_videoOutput);
}

qint64 QGstreamerPlayerSession::duration() const
{
    return m_duration;
}

qint64 QGstreamerPlayerSession::position() const
{
    gint64      position = 0;

    if (m_pipeline && qt_gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &position))
        m_lastPosition = position / 1000000;
    return m_lastPosition;
}

qreal QGstreamerPlayerSession::playbackRate() const
{
    return m_playbackRate;
}

void QGstreamerPlayerSession::setPlaybackRate(qreal rate)
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO << rate;
#endif
    if (!qFuzzyCompare(m_playbackRate, rate)) {
        m_playbackRate = rate;
        if (m_pipeline && m_seekable) {
            qint64 from = rate > 0 ? position() : 0;
            qint64 to = rate > 0 ? duration() : position();
            gst_element_seek(m_pipeline, rate, GST_FORMAT_TIME,
                             GstSeekFlags(GST_SEEK_FLAG_FLUSH),
                             GST_SEEK_TYPE_SET, from * 1000000,
                             GST_SEEK_TYPE_SET, to * 1000000);
        }
        emit playbackRateChanged(m_playbackRate);
    }
}

QMediaTimeRange QGstreamerPlayerSession::availablePlaybackRanges() const
{
    QMediaTimeRange ranges;

    if (duration() <= 0)
        return ranges;

    //GST_FORMAT_TIME would be more appropriate, but unfortunately it's not supported.
    //with GST_FORMAT_PERCENT media is treated as encoded with constant bitrate.
    GstQuery* query = gst_query_new_buffering(GST_FORMAT_PERCENT);

    if (!gst_element_query(m_pipeline, query)) {
        gst_query_unref(query);
        return ranges;
    }

    gint64 rangeStart = 0;
    gint64 rangeStop = 0;
    for (guint index = 0; index < gst_query_get_n_buffering_ranges(query); index++) {
        if (gst_query_parse_nth_buffering_range(query, index, &rangeStart, &rangeStop))
            ranges.addInterval(rangeStart * duration() / 100,
                               rangeStop * duration() / 100);
    }

    gst_query_unref(query);

    if (ranges.isEmpty() && !isLiveSource() && isSeekable())
        ranges.addInterval(0, duration());

#ifdef DEBUG_PLAYBIN
    qDebug() << ranges;
#endif

    return ranges;
}

int QGstreamerPlayerSession::activeStream(QMediaStreamsControl::StreamType streamType) const
{
    int streamNumber = -1;
    if (m_playbin) {
        switch (streamType) {
        case QMediaStreamsControl::AudioStream:
            g_object_get(G_OBJECT(m_playbin), "current-audio", &streamNumber, nullptr);
            break;
        case QMediaStreamsControl::VideoStream:
            g_object_get(G_OBJECT(m_playbin), "current-video", &streamNumber, nullptr);
            break;
        case QMediaStreamsControl::SubPictureStream:
            g_object_get(G_OBJECT(m_playbin), "current-text", &streamNumber, nullptr);
            break;
        default:
            break;
        }
    }

    if (streamNumber >= 0)
        streamNumber += m_playbin2StreamOffset.value(streamType,0);

    return streamNumber;
}

void QGstreamerPlayerSession::setActiveStream(QMediaStreamsControl::StreamType streamType, int streamNumber)
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO << streamType << streamNumber;
#endif

    if (streamNumber >= 0)
        streamNumber -= m_playbin2StreamOffset.value(streamType,0);

    if (m_playbin) {
        switch (streamType) {
        case QMediaStreamsControl::AudioStream:
            g_object_set(G_OBJECT(m_playbin), "current-audio", streamNumber, nullptr);
            break;
        case QMediaStreamsControl::VideoStream:
            g_object_set(G_OBJECT(m_playbin), "current-video", streamNumber, nullptr);
            break;
        case QMediaStreamsControl::SubPictureStream:
            g_object_set(G_OBJECT(m_playbin), "current-text", streamNumber, nullptr);
            break;
        default:
            break;
        }
    }
}

int QGstreamerPlayerSession::volume() const
{
    return m_volume;
}

bool QGstreamerPlayerSession::isMuted() const
{
    return m_muted;
}

bool QGstreamerPlayerSession::isAudioAvailable() const
{
    return m_audioAvailable;
}

static GstPadProbeReturn block_pad_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    Q_UNUSED(pad);
    Q_UNUSED(info);
    Q_UNUSED(user_data);
    qDebug() << "block_pad_cb" << pad;
    return GST_PAD_PROBE_OK;
}

void QGstreamerPlayerSession::updateVideoRenderer()
{
#ifdef DEBUG_PLAYBIN
    qDebug() << "Video sink has chaged, reload video output";
#endif

    if (m_videoOutput)
        setVideoRenderer(m_videoOutput);
}

void QGstreamerPlayerSession::setVideoRenderer(QObject *videoOutput)
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO;
#endif
    if (m_videoOutput != videoOutput) {
        if (m_videoOutput) {
            disconnect(m_videoOutput, SIGNAL(sinkChanged()),
                       this, SLOT(updateVideoRenderer()));
            disconnect(m_videoOutput, SIGNAL(readyChanged(bool)),
                   this, SLOT(updateVideoRenderer()));

            m_busHelper->removeMessageFilter(m_videoOutput);
        }

        m_videoOutput = videoOutput;

        if (m_videoOutput) {
            connect(m_videoOutput, SIGNAL(sinkChanged()),
                    this, SLOT(updateVideoRenderer()));
            connect(m_videoOutput, SIGNAL(readyChanged(bool)),
                   this, SLOT(updateVideoRenderer()));

            m_busHelper->installMessageFilter(m_videoOutput);
        }
    }

    m_renderer = qobject_cast<QGstreamerVideoRendererInterface*>(videoOutput);
    emit rendererChanged();

    // No sense to continue if custom pipeline requested.
    if (!m_playbin)
        return;

    GstElement *videoSink = 0;
    if (m_renderer && m_renderer->isReady())
        videoSink = m_renderer->videoSink();

    if (!videoSink)
        videoSink = m_nullVideoSink;

#ifdef DEBUG_PLAYBIN
    qDebug() << "Set video output:" << videoOutput;
    qDebug() << "Current sink:" << (m_videoSink ? GST_ELEMENT_NAME(m_videoSink) : "") <<  m_videoSink
             << "pending:" << (m_pendingVideoSink ? GST_ELEMENT_NAME(m_pendingVideoSink) : "") << m_pendingVideoSink
             << "new sink:" << (videoSink ? GST_ELEMENT_NAME(videoSink) : "") << videoSink;
#endif

    if (m_pendingVideoSink == videoSink ||
        (m_pendingVideoSink == 0 && m_videoSink == videoSink)) {
#ifdef DEBUG_PLAYBIN
        qDebug() << "Video sink has not changed, skip video output reconfiguration";
#endif
        return;
    }

#ifdef DEBUG_PLAYBIN
    qDebug() << "Reconfigure video output";
#endif

    if (m_state == QMediaPlayer::StoppedState) {
#ifdef DEBUG_PLAYBIN
        qDebug() << "The pipeline has not started yet, pending state:" << m_pendingState;
#endif
        //the pipeline has not started yet
        m_pendingVideoSink = 0;
        gst_element_set_state(m_videoSink, GST_STATE_NULL);
        gst_element_set_state(m_playbin, GST_STATE_NULL);

        gst_bin_remove(GST_BIN(m_videoOutputBin), m_videoSink);

        m_videoSink = videoSink;

        gst_bin_add(GST_BIN(m_videoOutputBin), m_videoSink);

        bool linked = gst_element_link(m_videoIdentity, m_videoSink);
        if (!linked)
            qWarning() << "Linking video output element failed";

        if (g_object_class_find_property(G_OBJECT_GET_CLASS(m_videoSink), "show-preroll-frame") != 0) {
            gboolean value = m_displayPrerolledFrame;
            g_object_set(G_OBJECT(m_videoSink), "show-preroll-frame", value, nullptr);
        }

        switch (m_pendingState) {
        case QMediaPlayer::PausedState:
            gst_element_set_state(m_playbin, GST_STATE_PAUSED);
            break;
        case QMediaPlayer::PlayingState:
            gst_element_set_state(m_playbin, GST_STATE_PLAYING);
            break;
        default:
            break;
        }

    } else {
        if (m_pendingVideoSink) {
#ifdef DEBUG_PLAYBIN
            qDebug() << "already waiting for pad to be blocked, just change the pending sink";
#endif
            m_pendingVideoSink = videoSink;
            return;
        }

        m_pendingVideoSink = videoSink;

#ifdef DEBUG_PLAYBIN
        qDebug() << "Blocking the video output pad...";
#endif

        //block pads, async to avoid locking in paused state
        GstPad *srcPad = gst_element_get_static_pad(m_videoIdentity, "src");
        this->pad_probe_id = gst_pad_add_probe(srcPad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BLOCKING), block_pad_cb, this, nullptr);
        gst_object_unref(GST_OBJECT(srcPad));

        //Unpause the sink to avoid waiting until the buffer is processed
        //while the sink is paused. The pad will be blocked as soon as the current
        //buffer is processed.
        if (m_state == QMediaPlayer::PausedState) {
#ifdef DEBUG_PLAYBIN
            qDebug() << "Starting video output to avoid blocking in paused state...";
#endif
            gst_element_set_state(m_videoSink, GST_STATE_PLAYING);
        }
    }
}

void QGstreamerPlayerSession::finishVideoOutputChange()
{
    if (!m_playbin || !m_pendingVideoSink)
        return;

#ifdef DEBUG_PLAYBIN
    qDebug() << "finishVideoOutputChange" << m_pendingVideoSink;
#endif

    GstPad *srcPad = gst_element_get_static_pad(m_videoIdentity, "src");

    if (!gst_pad_is_blocked(srcPad)) {
        //pad is not blocked, it's possible to swap outputs only in the null state
        qWarning() << "Pad is not blocked yet, could not switch video sink";
        GstState identityElementState = GST_STATE_NULL;
        gst_element_get_state(m_videoIdentity, &identityElementState, nullptr, GST_CLOCK_TIME_NONE);
        if (identityElementState != GST_STATE_NULL) {
            gst_object_unref(GST_OBJECT(srcPad));
            return; //can't change vo yet, received async call from the previous change
        }
    }

    if (m_pendingVideoSink == m_videoSink) {
        qDebug() << "Abort, no change";
        //video output was change back to the current one,
        //no need to torment the pipeline, just unblock the pad
        if (gst_pad_is_blocked(srcPad))
            gst_pad_remove_probe(srcPad, this->pad_probe_id);

        m_pendingVideoSink = 0;
        gst_object_unref(GST_OBJECT(srcPad));
        return;
    }

    gst_element_set_state(m_videoSink, GST_STATE_NULL);
    gst_element_unlink(m_videoIdentity, m_videoSink);

    gst_bin_remove(GST_BIN(m_videoOutputBin), m_videoSink);

    m_videoSink = m_pendingVideoSink;
    m_pendingVideoSink = 0;

    gst_bin_add(GST_BIN(m_videoOutputBin), m_videoSink);

    bool linked = gst_element_link(m_videoIdentity, m_videoSink);

    if (!linked)
        qWarning() << "Linking video output element failed";

#ifdef DEBUG_PLAYBIN
    qDebug() << "notify the video connector it has to emit a new segment message...";
#endif

    GstState state = GST_STATE_VOID_PENDING;

    switch (m_pendingState) {
    case QMediaPlayer::StoppedState:
        state = GST_STATE_NULL;
        break;
    case QMediaPlayer::PausedState:
        state = GST_STATE_PAUSED;
        break;
    case QMediaPlayer::PlayingState:
        state = GST_STATE_PLAYING;
        break;
    }

    gst_element_set_state(m_videoSink, state);

    // Set state change that was deferred due the video output
    // change being pending
    gst_element_set_state(m_playbin, state);

    //don't have to wait here, it will unblock eventually
    if (gst_pad_is_blocked(srcPad))
        gst_pad_remove_probe(srcPad, this->pad_probe_id);

    gst_object_unref(GST_OBJECT(srcPad));

}

bool QGstreamerPlayerSession::isVideoAvailable() const
{
    return m_videoAvailable;
}

bool QGstreamerPlayerSession::isSeekable() const
{
    return m_seekable;
}

bool QGstreamerPlayerSession::play()
{
    static bool dumpDot = qEnvironmentVariableIsSet("GST_DEBUG_DUMP_DOT_DIR");
    if (dumpDot)
        gst_debug_bin_to_dot_file_with_ts(GST_BIN(m_pipeline), GstDebugGraphDetails(GST_DEBUG_GRAPH_SHOW_ALL), "gst.play");
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO;
#endif

    m_everPlayed = false;
    if (m_pipeline) {
        m_pendingState = QMediaPlayer::PlayingState;
        if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) {
            qWarning() << "GStreamer; Unable to play -" << m_request.url().toString();
            m_pendingState = m_state = QMediaPlayer::StoppedState;
            emit stateChanged(m_state);
        } else {
            return true;
        }
    }

    return false;
}

bool QGstreamerPlayerSession::pause()
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO;
#endif
    if (m_pipeline) {
        m_pendingState = QMediaPlayer::PausedState;
        if (m_pendingVideoSink != 0)
            return true;

        if (gst_element_set_state(m_pipeline, GST_STATE_PAUSED) == GST_STATE_CHANGE_FAILURE) {
            qWarning() << "GStreamer; Unable to pause -" << m_request.url().toString();
            m_pendingState = m_state = QMediaPlayer::StoppedState;
            emit stateChanged(m_state);
        } else {
            return true;
        }
    }

    return false;
}

void QGstreamerPlayerSession::stop()
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO;
#endif
    m_everPlayed = false;
    if (m_pipeline) {

        if (m_renderer)
            m_renderer->stopRenderer();

        gst_element_set_state(m_pipeline, GST_STATE_NULL);

        m_lastPosition = 0;
        QMediaPlayer::State oldState = m_state;
        m_pendingState = m_state = QMediaPlayer::StoppedState;

        finishVideoOutputChange();

        //we have to do it here, since gstreamer will not emit bus messages any more
        setSeekable(false);
        if (oldState != m_state)
            emit stateChanged(m_state);
    }
}

bool QGstreamerPlayerSession::seek(qint64 ms)
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO << ms;
#endif
    //seek locks when the video output sink is changing and pad is blocked
    if (m_pipeline && !m_pendingVideoSink && m_state != QMediaPlayer::StoppedState && m_seekable) {
        ms = qMax(ms,qint64(0));
        qint64 from = m_playbackRate > 0 ? ms : 0;
        qint64 to = m_playbackRate > 0 ? duration() : ms;

        bool isSeeking = gst_element_seek(m_pipeline, m_playbackRate, GST_FORMAT_TIME,
                                          GstSeekFlags(GST_SEEK_FLAG_FLUSH),
                                          GST_SEEK_TYPE_SET, from * 1000000,
                                          GST_SEEK_TYPE_SET, to * 1000000);
        if (isSeeking)
            m_lastPosition = ms;

        return isSeeking;
    }

    return false;
}

void QGstreamerPlayerSession::setVolume(int volume)
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO << volume;
#endif

    if (m_volume != volume) {
        m_volume = volume;

        if (m_volumeElement)
            g_object_set(G_OBJECT(m_volumeElement), "volume", m_volume / 100.0, nullptr);

        emit volumeChanged(m_volume);
    }
}

void QGstreamerPlayerSession::setMuted(bool muted)
{
#ifdef DEBUG_PLAYBIN
        qDebug() << Q_FUNC_INFO << muted;
#endif
    if (m_muted != muted) {
        m_muted = muted;

        if (m_volumeElement)
            g_object_set(G_OBJECT(m_volumeElement), "mute", m_muted ? TRUE : FALSE, nullptr);

        emit mutedStateChanged(m_muted);
    }
}


void QGstreamerPlayerSession::setSeekable(bool seekable)
{
#ifdef DEBUG_PLAYBIN
        qDebug() << Q_FUNC_INFO << seekable;
#endif
    if (seekable != m_seekable) {
        m_seekable = seekable;
        emit seekableChanged(m_seekable);
    }
}

bool QGstreamerPlayerSession::processBusMessage(const QGstreamerMessage &message)
{
    GstMessage* gm = message.rawMessage();
    if (gm) {
        //tag message comes from elements inside playbin, not from playbin itself
        if (GST_MESSAGE_TYPE(gm) == GST_MESSAGE_TAG) {
            GstTagList *tag_list;
            gst_message_parse_tag(gm, &tag_list);

            QMap<QByteArray, QVariant> newTags = QGstUtils::gstTagListToMap(tag_list);
            QMap<QByteArray, QVariant>::const_iterator it = newTags.constBegin();
            for ( ; it != newTags.constEnd(); ++it)
                m_tags.insert(it.key(), it.value()); // overwrite existing tags

            gst_tag_list_free(tag_list);

            emit tagsChanged();
        } else if (GST_MESSAGE_TYPE(gm) == GST_MESSAGE_DURATION) {
            updateDuration();
        }

#ifdef DEBUG_PLAYBIN
        if (m_sourceType == MMSSrc && qstrcmp(GST_OBJECT_NAME(GST_MESSAGE_SRC(gm)), "source") == 0) {
            qDebug() << "Message from MMSSrc: " << GST_MESSAGE_TYPE(gm);
        } else if (m_sourceType == RTSPSrc && qstrcmp(GST_OBJECT_NAME(GST_MESSAGE_SRC(gm)), "source") == 0) {
            qDebug() << "Message from RTSPSrc: " << GST_MESSAGE_TYPE(gm);
        } else {
            qDebug() << "Message from " << GST_OBJECT_NAME(GST_MESSAGE_SRC(gm)) << ":" << GST_MESSAGE_TYPE(gm);
        }
#endif

        if (GST_MESSAGE_TYPE(gm) == GST_MESSAGE_BUFFERING) {
            int progress = 0;
            gst_message_parse_buffering(gm, &progress);
            emit bufferingProgressChanged(progress);
        }

        bool handlePlaybin2 = false;
        if (GST_MESSAGE_SRC(gm) == GST_OBJECT_CAST(m_pipeline)) {
            switch (GST_MESSAGE_TYPE(gm))  {
            case GST_MESSAGE_STATE_CHANGED:
                {
                    GstState    oldState;
                    GstState    newState;
                    GstState    pending;

                    gst_message_parse_state_changed(gm, &oldState, &newState, &pending);

#ifdef DEBUG_PLAYBIN
                    static QStringList states = {
                              QStringLiteral("GST_STATE_VOID_PENDING"),  QStringLiteral("GST_STATE_NULL"),
                              QStringLiteral("GST_STATE_READY"), QStringLiteral("GST_STATE_PAUSED"),
                              QStringLiteral("GST_STATE_PLAYING") };

                    qDebug() << QStringLiteral("state changed: old: %1  new: %2  pending: %3") \
                            .arg(states[oldState]) \
                            .arg(states[newState]) \
                            .arg(states[pending]);
#endif

                    switch (newState) {
                    case GST_STATE_VOID_PENDING:
                    case GST_STATE_NULL:
                        setSeekable(false);
                        finishVideoOutputChange();
                        if (m_state != QMediaPlayer::StoppedState)
                            emit stateChanged(m_state = QMediaPlayer::StoppedState);
                        break;
                    case GST_STATE_READY:
                        setSeekable(false);
                        if (m_state != QMediaPlayer::StoppedState)
                            emit stateChanged(m_state = QMediaPlayer::StoppedState);
                        break;
                    case GST_STATE_PAUSED:
                    {
                        QMediaPlayer::State prevState = m_state;
                        m_state = QMediaPlayer::PausedState;

                        //check for seekable
                        if (oldState == GST_STATE_READY) {
                            if (m_sourceType == SoupHTTPSrc || m_sourceType == MMSSrc) {
                                //since udpsrc is a live source, it is not applicable here
                                m_everPlayed = true;
                            }

                            getStreamsInfo();
                            updateVideoResolutionTag();

                            //gstreamer doesn't give a reliable indication the duration
                            //information is ready, GST_MESSAGE_DURATION is not sent by most elements
                            //the duration is queried up to 5 times with increasing delay
                            m_durationQueries = 5;
                            // This should also update the seekable flag.
                            updateDuration();

                            if (!qFuzzyCompare(m_playbackRate, qreal(1.0))) {
                                qreal rate = m_playbackRate;
                                m_playbackRate = 1.0;
                                setPlaybackRate(rate);
                            }
                        }

                        if (m_state != prevState)
                            emit stateChanged(m_state);

                        break;
                    }
                    case GST_STATE_PLAYING:
                        m_everPlayed = true;
                        if (m_state != QMediaPlayer::PlayingState) {
                            emit stateChanged(m_state = QMediaPlayer::PlayingState);

                            // For rtsp streams duration information might not be available
                            // until playback starts.
                            if (m_duration <= 0) {
                                m_durationQueries = 5;
                                updateDuration();
                            }
                        }

                        break;
                    }
                }
                break;

            case GST_MESSAGE_EOS:
                emit playbackFinished();
                break;

            case GST_MESSAGE_TAG:
            case GST_MESSAGE_STREAM_STATUS:
            case GST_MESSAGE_UNKNOWN:
                break;
            case GST_MESSAGE_ERROR: {
                    GError *err;
                    gchar *debug;
                    gst_message_parse_error(gm, &err, &debug);
                    if (err->domain == GST_STREAM_ERROR && err->code == GST_STREAM_ERROR_CODEC_NOT_FOUND)
                        processInvalidMedia(QMediaPlayer::FormatError, tr("Cannot play stream of type: <unknown>"));
                    else
                        processInvalidMedia(QMediaPlayer::ResourceError, QString::fromUtf8(err->message));
                    qWarning() << "Error:" << QString::fromUtf8(err->message);
                    g_error_free(err);
                    g_free(debug);
                }
                break;
            case GST_MESSAGE_WARNING:
                {
                    GError *err;
                    gchar *debug;
                    gst_message_parse_warning (gm, &err, &debug);
                    qWarning() << "Warning:" << QString::fromUtf8(err->message);
                    g_error_free (err);
                    g_free (debug);
                }
                break;
            case GST_MESSAGE_INFO:
#ifdef DEBUG_PLAYBIN
                {
                    GError *err;
                    gchar *debug;
                    gst_message_parse_info (gm, &err, &debug);
                    qDebug() << "Info:" << QString::fromUtf8(err->message);
                    g_error_free (err);
                    g_free (debug);
                }
#endif
                break;
            case GST_MESSAGE_BUFFERING:
            case GST_MESSAGE_STATE_DIRTY:
            case GST_MESSAGE_STEP_DONE:
            case GST_MESSAGE_CLOCK_PROVIDE:
            case GST_MESSAGE_CLOCK_LOST:
            case GST_MESSAGE_NEW_CLOCK:
            case GST_MESSAGE_STRUCTURE_CHANGE:
            case GST_MESSAGE_APPLICATION:
            case GST_MESSAGE_ELEMENT:
                break;
            case GST_MESSAGE_SEGMENT_START:
                {
                    const GstStructure *structure = gst_message_get_structure(gm);
                    qint64 position = g_value_get_int64(gst_structure_get_value(structure, "position"));
                    position /= 1000000;
                    m_lastPosition = position;
                    emit positionChanged(position);
                }
                break;
            case GST_MESSAGE_SEGMENT_DONE:
                break;
            case GST_MESSAGE_LATENCY:
            case GST_MESSAGE_ASYNC_START:
                break;
            case GST_MESSAGE_ASYNC_DONE:
            {
                gint64      position = 0;
                if (qt_gst_element_query_position(m_pipeline, GST_FORMAT_TIME, &position)) {
                    position /= 1000000;
                    m_lastPosition = position;
                    emit positionChanged(position);
                }
                break;
            }
            case GST_MESSAGE_REQUEST_STATE:
            case GST_MESSAGE_ANY:
                break;
            default:
                break;
            }
        } else if (GST_MESSAGE_TYPE(gm) == GST_MESSAGE_ERROR) {
            GError *err;
            gchar *debug;
            gst_message_parse_error(gm, &err, &debug);
            // If the source has given up, so do we.
            if (qstrcmp(GST_OBJECT_NAME(GST_MESSAGE_SRC(gm)), "source") == 0) {
                bool everPlayed = m_everPlayed;
                // Try and differentiate network related resource errors from the others
                if (!m_request.url().isRelative() && m_request.url().scheme().compare(QLatin1String("file"), Qt::CaseInsensitive) != 0 ) {
                    if (everPlayed ||
                        (err->domain == GST_RESOURCE_ERROR && (
                         err->code == GST_RESOURCE_ERROR_BUSY ||
                         err->code == GST_RESOURCE_ERROR_OPEN_READ ||
                         err->code == GST_RESOURCE_ERROR_READ ||
                         err->code == GST_RESOURCE_ERROR_SEEK ||
                         err->code == GST_RESOURCE_ERROR_SYNC))) {
                        processInvalidMedia(QMediaPlayer::NetworkError, QString::fromUtf8(err->message));
                    } else {
                        processInvalidMedia(QMediaPlayer::ResourceError, QString::fromUtf8(err->message));
                    }
                }
                else
                    processInvalidMedia(QMediaPlayer::ResourceError, QString::fromUtf8(err->message));
            } else if (err->domain == GST_STREAM_ERROR
                       && (err->code == GST_STREAM_ERROR_DECRYPT || err->code == GST_STREAM_ERROR_DECRYPT_NOKEY)) {
                processInvalidMedia(QMediaPlayer::AccessDeniedError, QString::fromUtf8(err->message));
            } else {
                handlePlaybin2 = true;
            }
            if (!handlePlaybin2)
                qWarning() << "Error:" << QString::fromUtf8(err->message);
            g_error_free(err);
            g_free(debug);
        } else if (GST_MESSAGE_TYPE(gm) == GST_MESSAGE_ELEMENT
                   && qstrcmp(GST_OBJECT_NAME(GST_MESSAGE_SRC(gm)), "source") == 0
                   && m_sourceType == UDPSrc
                   && gst_structure_has_name(gst_message_get_structure(gm), "GstUDPSrcTimeout")) {
            //since udpsrc will not generate an error for the timeout event,
            //we need to process its element message here and treat it as an error.
            processInvalidMedia(m_everPlayed ? QMediaPlayer::NetworkError : QMediaPlayer::ResourceError,
                                tr("UDP source timeout"));
        } else {
            handlePlaybin2 = true;
        }

        if (handlePlaybin2) {
            if (GST_MESSAGE_TYPE(gm) == GST_MESSAGE_WARNING) {
                GError *err;
                gchar *debug;
                gst_message_parse_warning(gm, &err, &debug);
                if (err->domain == GST_STREAM_ERROR && err->code == GST_STREAM_ERROR_CODEC_NOT_FOUND)
                    emit error(int(QMediaPlayer::FormatError), tr("Cannot play stream of type: <unknown>"));
                // GStreamer shows warning for HTTP playlists
                if (err && err->message)
                    qWarning() << "Warning:" << QString::fromUtf8(err->message);
                g_error_free(err);
                g_free(debug);
            } else if (GST_MESSAGE_TYPE(gm) == GST_MESSAGE_ERROR) {
                GError *err;
                gchar *debug;
                gst_message_parse_error(gm, &err, &debug);

                // Nearly all errors map to ResourceError
                QMediaPlayer::Error qerror = QMediaPlayer::ResourceError;
                if (err->domain == GST_STREAM_ERROR
                           && (err->code == GST_STREAM_ERROR_DECRYPT
                               || err->code == GST_STREAM_ERROR_DECRYPT_NOKEY)) {
                    qerror = QMediaPlayer::AccessDeniedError;
                }
                processInvalidMedia(qerror, QString::fromUtf8(err->message));
                if (err && err->message)
                    qWarning() << "Error:" << QString::fromUtf8(err->message);

                g_error_free(err);
                g_free(debug);
            }
        }
    }

    return false;
}

void QGstreamerPlayerSession::getStreamsInfo()
{
    if (!m_playbin)
        return;

    QList< QMap<QString,QVariant> > oldProperties = m_streamProperties;
    QList<QMediaStreamsControl::StreamType> oldTypes = m_streamTypes;
    QMap<QMediaStreamsControl::StreamType, int> oldOffset = m_playbin2StreamOffset;

    //check if video is available:
    bool haveAudio = false;
    bool haveVideo = false;
    m_streamProperties.clear();
    m_streamTypes.clear();
    m_playbin2StreamOffset.clear();

    gint audioStreamsCount = 0;
    gint videoStreamsCount = 0;
    gint textStreamsCount = 0;

    g_object_get(G_OBJECT(m_playbin), "n-audio", &audioStreamsCount, nullptr);
    g_object_get(G_OBJECT(m_playbin), "n-video", &videoStreamsCount, nullptr);
    g_object_get(G_OBJECT(m_playbin), "n-text", &textStreamsCount, nullptr);

    haveAudio = audioStreamsCount > 0;
    haveVideo = videoStreamsCount > 0;

    m_playbin2StreamOffset[QMediaStreamsControl::AudioStream] = 0;
    m_playbin2StreamOffset[QMediaStreamsControl::VideoStream] = audioStreamsCount;
    m_playbin2StreamOffset[QMediaStreamsControl::SubPictureStream] = audioStreamsCount+videoStreamsCount;

    for (int i=0; i<audioStreamsCount; i++)
        m_streamTypes.append(QMediaStreamsControl::AudioStream);

    for (int i=0; i<videoStreamsCount; i++)
        m_streamTypes.append(QMediaStreamsControl::VideoStream);

    for (int i=0; i<textStreamsCount; i++)
        m_streamTypes.append(QMediaStreamsControl::SubPictureStream);

    for (int i=0; i<m_streamTypes.count(); i++) {
        QMediaStreamsControl::StreamType streamType = m_streamTypes[i];
        QMap<QString, QVariant> streamProperties;

        int streamIndex = i - m_playbin2StreamOffset[streamType];

        GstTagList *tags = 0;
        switch (streamType) {
        case QMediaStreamsControl::AudioStream:
            g_signal_emit_by_name(G_OBJECT(m_playbin), "get-audio-tags", streamIndex, &tags);
            break;
        case QMediaStreamsControl::VideoStream:
            g_signal_emit_by_name(G_OBJECT(m_playbin), "get-video-tags", streamIndex, &tags);
            break;
        case QMediaStreamsControl::SubPictureStream:
            g_signal_emit_by_name(G_OBJECT(m_playbin), "get-text-tags", streamIndex, &tags);
            break;
        default:
            break;
        }
        if (tags && GST_IS_TAG_LIST(tags)) {
            gchar *languageCode = 0;
            if (gst_tag_list_get_string(tags, GST_TAG_LANGUAGE_CODE, &languageCode))
                streamProperties[QMediaMetaData::Language] = QString::fromUtf8(languageCode);

            //qDebug() << "language for setream" << i << QString::fromUtf8(languageCode);
            g_free (languageCode);
            gst_tag_list_free(tags);
        }

        m_streamProperties.append(streamProperties);
    }

    bool emitAudioChanged = (haveAudio != m_audioAvailable);
    bool emitVideoChanged = (haveVideo != m_videoAvailable);

    m_audioAvailable = haveAudio;
    m_videoAvailable = haveVideo;

    if (emitAudioChanged) {
        emit audioAvailableChanged(m_audioAvailable);
    }
    if (emitVideoChanged) {
        emit videoAvailableChanged(m_videoAvailable);
    }

    if (oldProperties != m_streamProperties || oldTypes != m_streamTypes || oldOffset != m_playbin2StreamOffset)
        emit streamsChanged();
}

void QGstreamerPlayerSession::updateVideoResolutionTag()
{
    if (!m_videoIdentity)
        return;

#ifdef DEBUG_PLAYBIN
        qDebug() << Q_FUNC_INFO;
#endif
    QSize size;
    QSize aspectRatio;
    GstPad *pad = gst_element_get_static_pad(m_videoIdentity, "src");
    GstCaps *caps = qt_gst_pad_get_current_caps(pad);

    if (caps) {
        const GstStructure *structure = gst_caps_get_structure(caps, 0);
        gst_structure_get_int(structure, "width", &size.rwidth());
        gst_structure_get_int(structure, "height", &size.rheight());

        gint aspectNum = 0;
        gint aspectDenum = 0;
        if (!size.isEmpty() && gst_structure_get_fraction(
                    structure, "pixel-aspect-ratio", &aspectNum, &aspectDenum)) {
            if (aspectDenum > 0)
                aspectRatio = QSize(aspectNum, aspectDenum);
        }
        gst_caps_unref(caps);
    }

    gst_object_unref(GST_OBJECT(pad));

    QSize currentSize = m_tags.value("resolution").toSize();
    QSize currentAspectRatio = m_tags.value("pixel-aspect-ratio").toSize();

    if (currentSize != size || currentAspectRatio != aspectRatio) {
        if (aspectRatio.isEmpty())
            m_tags.remove("pixel-aspect-ratio");

        if (size.isEmpty()) {
            m_tags.remove("resolution");
        } else {
            m_tags.insert("resolution", QVariant(size));
            if (!aspectRatio.isEmpty())
                m_tags.insert("pixel-aspect-ratio", QVariant(aspectRatio));
        }

        emit tagsChanged();
    }
}

void QGstreamerPlayerSession::updateDuration()
{
    gint64 gstDuration = 0;
    int duration = 0;

    if (m_pipeline && qt_gst_element_query_duration(m_pipeline, GST_FORMAT_TIME, &gstDuration))
        duration = gstDuration / 1000000;

    if (m_duration != duration) {
        m_duration = duration;
        emit durationChanged(m_duration);
    }

    gboolean seekable = false;
    if (m_duration > 0) {
        m_durationQueries = 0;
        GstQuery *query = gst_query_new_seeking(GST_FORMAT_TIME);
        if (gst_element_query(m_pipeline, query))
            gst_query_parse_seeking(query, 0, &seekable, 0, 0);
        gst_query_unref(query);
    }
    setSeekable(seekable);

    if (m_durationQueries > 0) {
        //increase delay between duration requests
        int delay = 25 << (5 - m_durationQueries);
        QTimer::singleShot(delay, this, SLOT(updateDuration()));
        m_durationQueries--;
    }
#ifdef DEBUG_PLAYBIN
        qDebug() << Q_FUNC_INFO << m_duration;
#endif
}

void QGstreamerPlayerSession::playbinNotifySource(GObject *o, GParamSpec *p, gpointer d)
{
    Q_UNUSED(p);

    GstElement *source = 0;
    g_object_get(o, "source", &source, nullptr);
    if (source == 0)
        return;

#ifdef DEBUG_PLAYBIN
    qDebug() << "Playbin source added:" << G_OBJECT_CLASS_NAME(G_OBJECT_GET_CLASS(source));
#endif

    // Set Headers
    const QByteArray userAgentString("User-Agent");

    QGstreamerPlayerSession *self = reinterpret_cast<QGstreamerPlayerSession *>(d);

    // User-Agent - special case, souphhtpsrc will always set something, even if
    // defined in extra-headers
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "user-agent") != 0) {
        g_object_set(G_OBJECT(source), "user-agent",
                     self->m_request.rawHeader(userAgentString).constData(), nullptr);
    }

    // The rest
    if (g_object_class_find_property(G_OBJECT_GET_CLASS(source), "extra-headers") != 0) {
        GstStructure *extras = qt_gst_structure_new_empty("extras");

        const auto rawHeaderList = self->m_request.rawHeaderList();
        for (const QByteArray &rawHeader : rawHeaderList) {
            if (rawHeader == userAgentString) // Filter User-Agent
                continue;

            GValue headerValue;

            memset(&headerValue, 0, sizeof(GValue));
            g_value_init(&headerValue, G_TYPE_STRING);

            g_value_set_string(&headerValue,
                               self->m_request.rawHeader(rawHeader).constData());

            gst_structure_set_value(extras, rawHeader.constData(), &headerValue);
        }

        if (gst_structure_n_fields(extras) > 0)
            g_object_set(G_OBJECT(source), "extra-headers", extras, nullptr);

        gst_structure_free(extras);
    }

    //set timeout property to 30 seconds
    const int timeout = 30;
    if (qstrcmp(G_OBJECT_CLASS_NAME(G_OBJECT_GET_CLASS(source)), "GstUDPSrc") == 0) {
        quint64 convertedTimeout = timeout;
        // Gst 1.x -> nanosecond
        convertedTimeout *= 1000000000;
        g_object_set(G_OBJECT(source), "timeout", convertedTimeout, nullptr);
        self->m_sourceType = UDPSrc;
        //The udpsrc is always a live source.
        self->m_isLiveSource = true;

        QUrlQuery query(self->m_request.url());
        const QString var = QLatin1String("udpsrc.caps");
        if (query.hasQueryItem(var)) {
            GstCaps *caps = gst_caps_from_string(query.queryItemValue(var).toLatin1().constData());
            g_object_set(G_OBJECT(source), "caps", caps, nullptr);
            gst_caps_unref(caps);
        }
    } else if (qstrcmp(G_OBJECT_CLASS_NAME(G_OBJECT_GET_CLASS(source)), "GstSoupHTTPSrc") == 0) {
        //souphttpsrc timeout unit = second
        g_object_set(G_OBJECT(source), "timeout", guint(timeout), nullptr);
        self->m_sourceType = SoupHTTPSrc;
        //since gst_base_src_is_live is not reliable, so we check the source property directly
        gboolean isLive = false;
        g_object_get(G_OBJECT(source), "is-live", &isLive, nullptr);
        self->m_isLiveSource = isLive;
    } else if (qstrcmp(G_OBJECT_CLASS_NAME(G_OBJECT_GET_CLASS(source)), "GstMMSSrc") == 0) {
        self->m_sourceType = MMSSrc;
        self->m_isLiveSource = gst_base_src_is_live(GST_BASE_SRC(source));
        g_object_set(G_OBJECT(source), "tcp-timeout", G_GUINT64_CONSTANT(timeout*1000000), nullptr);
    } else if (qstrcmp(G_OBJECT_CLASS_NAME(G_OBJECT_GET_CLASS(source)), "GstRTSPSrc") == 0) {
        //rtspsrc acts like a live source and will therefore only generate data in the PLAYING state.
        self->m_sourceType = RTSPSrc;
        self->m_isLiveSource = true;
        g_object_set(G_OBJECT(source), "buffer-mode", 1, nullptr);
    } else {
        self->m_sourceType = UnknownSrc;
        self->m_isLiveSource = gst_base_src_is_live(GST_BASE_SRC(source));
    }

#ifdef DEBUG_PLAYBIN
    if (self->m_isLiveSource)
        qDebug() << "Current source is a live source";
    else
        qDebug() << "Current source is a non-live source";
#endif

    if (self->m_videoSink)
        g_object_set(G_OBJECT(self->m_videoSink), "sync", !self->m_isLiveSource, nullptr);

    gst_object_unref(source);
}

bool QGstreamerPlayerSession::isLiveSource() const
{
    return m_isLiveSource;
}

GstAutoplugSelectResult QGstreamerPlayerSession::handleAutoplugSelect(GstBin *bin, GstPad *pad, GstCaps *caps, GstElementFactory *factory, QGstreamerPlayerSession *session)
{
    Q_UNUSED(bin);
    Q_UNUSED(pad);
    Q_UNUSED(caps);

    GstAutoplugSelectResult res = GST_AUTOPLUG_SELECT_TRY;

    // if VAAPI is available and can be used to decode but the current video sink cannot handle
    // the decoded format, don't use it
    const gchar *factoryName = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (g_str_has_prefix(factoryName, "vaapi")) {
        GstPad *sinkPad = gst_element_get_static_pad(session->m_videoSink, "sink");
        GstCaps *sinkCaps = gst_pad_query_caps(sinkPad, nullptr);

        if (!gst_element_factory_can_src_any_caps(factory, sinkCaps))
            res = GST_AUTOPLUG_SELECT_SKIP;

        gst_object_unref(sinkPad);
        gst_caps_unref(sinkCaps);
    }

    return res;
}

void QGstreamerPlayerSession::handleElementAdded(GstBin *bin, GstElement *element, QGstreamerPlayerSession *session)
{
    Q_UNUSED(bin);
    //we have to configure queue2 element to enable media downloading
    //and reporting available ranges,
    //but it's added dynamically to playbin2

    gchar *elementName = gst_element_get_name(element);

    if (g_str_has_prefix(elementName, "queue2")) {
        // Disable on-disk buffering.
        g_object_set(G_OBJECT(element), "temp-template", nullptr, nullptr);
    } else if (g_str_has_prefix(elementName, "uridecodebin") ||
        g_str_has_prefix(elementName, "decodebin")) {
        //listen for queue2 element added to uridecodebin/decodebin2 as well.
        //Don't touch other bins since they may have unrelated queues
        g_signal_connect(element, "element-added",
                         G_CALLBACK(handleElementAdded), session);
    }

    g_free(elementName);
}

void QGstreamerPlayerSession::handleStreamsChange(GstBin *bin, gpointer user_data)
{
    Q_UNUSED(bin);

    QGstreamerPlayerSession* session = reinterpret_cast<QGstreamerPlayerSession*>(user_data);
    QMetaObject::invokeMethod(session, "getStreamsInfo", Qt::QueuedConnection);
}

//doing proper operations when detecting an invalidMedia: change media status before signal the erorr
void QGstreamerPlayerSession::processInvalidMedia(QMediaPlayer::Error errorCode, const QString& errorString)
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO;
#endif
    emit invalidMedia();
    stop();
    emit error(int(errorCode), errorString);
}

void QGstreamerPlayerSession::showPrerollFrames(bool enabled)
{
#ifdef DEBUG_PLAYBIN
    qDebug() << Q_FUNC_INFO << enabled;
#endif
    if (enabled != m_displayPrerolledFrame && m_videoSink &&
            g_object_class_find_property(G_OBJECT_GET_CLASS(m_videoSink), "show-preroll-frame") != 0) {

        gboolean value = enabled;
        g_object_set(G_OBJECT(m_videoSink), "show-preroll-frame", value, nullptr);
        m_displayPrerolledFrame = enabled;
    }
}

// This function is similar to stop(),
// but does not set m_everPlayed, m_lastPosition,
// and setSeekable() values.
void QGstreamerPlayerSession::endOfMediaReset()
{
    if (m_renderer)
        m_renderer->stopRenderer();

    gst_element_set_state(m_pipeline, GST_STATE_PAUSED);

    QMediaPlayer::State oldState = m_state;
    m_pendingState = m_state = QMediaPlayer::StoppedState;

    finishVideoOutputChange();

    if (oldState != m_state)
        emit stateChanged(m_state);
}

void QGstreamerPlayerSession::setAudioOutputDevice(const QAudioDeviceInfo &audioDevice)
{
    if (m_audioDevice == audioDevice)
        return;

    m_audioDevice = audioDevice;
    if (m_audioSink)
        updateAudioSink();
}

void QGstreamerPlayerSession::updateAudioSink()
{
    QAudioDeviceInfo info = m_audioDevice;
    auto *deviceInfo = static_cast<const QGStreamerAudioDeviceInfo *>(m_audioDevice.handle());

    GstElement *audioSink = nullptr;
    if (deviceInfo && deviceInfo->gstDevice)
        audioSink = gst_device_create_element(deviceInfo->gstDevice , "audiosink");

    if (!audioSink)
        audioSink = gst_element_factory_make("autoaudiosink", "audiosink");

    m_pendingAudioSink = audioSink;
    if (m_state == QMediaPlayer::StoppedState) {
        finishAudioOutputChange();
    } else {
        GstPad *srcPad = gst_element_get_static_pad(m_volumeElement, "src");
        gst_pad_add_probe(srcPad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BLOCKING),
                          change_audio_sink_cb, this, nullptr);
        gst_object_unref(GST_OBJECT(srcPad));

        // set the pipeline into paused state
        if (m_state == QMediaPlayer::PlayingState)
            gst_element_set_state(m_pipeline, GST_STATE_PAUSED);
        //Unpause the sink to avoid waiting until the buffer is processed
        //while the sink is paused. The pad will be blocked as soon as the current
        //buffer is processed.
        gst_element_set_state(m_audioSink, GST_STATE_PLAYING);

    }
}

GstPadProbeReturn QGstreamerPlayerSession::change_audio_sink_cb(GstPad *pad, GstPadProbeInfo *info, gpointer user_data)
{
    if (gst_pad_is_blocked(pad)) {
        gst_pad_remove_probe(pad, GST_PAD_PROBE_INFO_ID(info));
        QGstreamerPlayerSession *s = static_cast<QGstreamerPlayerSession *>(user_data);

        s->finishAudioOutputChange();
    }
    return GST_PAD_PROBE_OK;
}


void QGstreamerPlayerSession::finishAudioOutputChange()
{
    auto *newAudioSink = m_pendingAudioSink;
    if (!newAudioSink)
        return;
    if (!g_atomic_pointer_compare_and_exchange(&m_pendingAudioSink, newAudioSink, nullptr))
        return;

    if (newAudioSink != m_audioSink) {
        if (m_audioSink) {
            gst_element_set_state(m_audioSink, GST_STATE_NULL);
            gst_bin_remove(m_audioBin, m_audioSink);
        }
        m_audioSink = newAudioSink;

        gst_bin_add(m_audioBin, m_audioSink);
        gst_element_sync_state_with_parent(m_audioSink);
        gst_element_link(m_volumeElement, m_audioSink);

        GstState state = GST_STATE_VOID_PENDING;

        switch (m_pendingState) {
        case QMediaPlayer::StoppedState:
            state = GST_STATE_NULL;
            break;
        case QMediaPlayer::PausedState:
            state = GST_STATE_PAUSED;
            break;
        case QMediaPlayer::PlayingState:
            state = GST_STATE_PLAYING;
            break;
        }
        if (m_state != QMediaPlayer::StoppedState)
            gst_element_set_state (m_pipeline, state);
    }
}


QT_END_NAMESPACE
