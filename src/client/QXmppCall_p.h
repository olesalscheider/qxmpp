/*
 * Copyright (C) 2008-2019 The QXmpp developers
 *
 * Author:
 *  Jeremy Lainé
 *
 * Source:
 *  https://github.com/qxmpp-project/qxmpp
 *
 * This file is a part of QXmpp library.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 */

#ifndef QXMPPCALL_P_H
#define QXMPPCALL_P_H

#include <QList>

#include "QXmppJingleIq.h"
#include "QXmppCall.h"

//  W A R N I N G
//  -------------
//
// This file is not part of the QXmpp API.
// This header file may change from version to version without notice,
// or even be removed.
//
// We mean it.
//

class QXmppCallStream;

class QXmppCallPrivate : public QObject
{
Q_OBJECT
public:
    struct GstCodec {
        int pt;
        QString name;
        int channels;
        int clockrate;
        QString gstPay;
        QString gstDepay;
        QString gstEnc;
        QString gstDec;
        struct Property {
            QString name;
            int value;
        };
        // Use e.g. gst-inspect-1.0 x264enc to find good encoder settings for live streaming
        QList<Property> encProps;
    };

    QXmppCallPrivate(QXmppCall *qq);
    ~QXmppCallPrivate();

    void ssrcActive(uint sessionId, uint ssrc);
    void padAdded(const QGst::PadPtr &pad);
    QGst::CapsPtr ptMap(uint sessionId, uint pt);
    void filterGStreamerFormats(QList<GstCodec> &formats);

    QXmppCallStream *createStream(const QString &media, const QString &creator, const QString &name);
    QXmppCallStream *findStreamByMedia(const QString &media);
    QXmppCallStream *findStreamByName(const QString &name);
    QXmppCallStream *findStreamById(const int id);
    QXmppJingleIq::Content localContent(QXmppCallStream *stream) const;

    void handleAck(const QXmppIq &iq);
    bool handleDescription(QXmppCallStream *stream, const QXmppJingleIq::Content &content);
    void handleRequest(const QXmppJingleIq &iq);
    bool handleTransport(QXmppCallStream *stream, const QXmppJingleIq::Content &content);
    void setState(QXmppCall::State state);
    bool sendAck(const QXmppJingleIq &iq);
    bool sendInvite();
    bool sendRequest(const QXmppJingleIq &iq);
    void terminate(QXmppJingleIq::Reason::Type reasonType);

    QXmppCall::Direction direction;
    QString jid;
    QString ownJid;
    QXmppCallManager *manager;
    QList<QXmppJingleIq> requests;
    QString sid;
    QXmppCall::State state;

    QGst::PipelinePtr pipeline;
    QGst::ElementPtr rtpbin;

    // Media streams
    QList<QXmppCallStream*> streams;
    int nextId;

    // Supported codecs
    QList<GstCodec> videoCodecs = {
        {.pt = 101, .name = "H265", .channels = 1, .clockrate = 90000, .gstPay = "rtph265pay", .gstDepay = "rtph265depay", .gstEnc = "x265enc", .gstDec = "avdec_h265", .encProps = {{"tune", 4}, {"speed-preset", 1}, {"bitrate", 256}}},
        // vp9enc seems to be very slow. Don't offer it for now.
        //{.pt = 100, .name = "VP9", .channels = 1, .clockrate = 90000, .gstPay = "rtpvp9pay", .gstDepay = "rtpvp9depay", .gstEnc = "vp9enc", .gstDec = "vp9dec", .encProps = {{"deadline", 20000}, {"target-bitrate", 256000}}},
        {.pt = 99, .name = "H264", .channels = 1, .clockrate = 90000, .gstPay = "rtph264pay", .gstDepay = "rtph264depay", .gstEnc = "x264enc", .gstDec = "avdec_h264", .encProps = {{"tune", 4}, {"speed-preset", 1}, {"bitrate", 256}}},
        {.pt = 98, .name = "VP8", .channels = 1, .clockrate = 90000, .gstPay = "rtpvp8pay", .gstDepay = "rtpvp8depay", .gstEnc = "vp8enc", .gstDec = "vp8dec", .encProps = {{"deadline", 20000}, {"target-bitrate", 256000}}}
    };

    QList<GstCodec> audioCodecs = {
        {.pt = 97, .name = "OPUS", .channels = 2, .clockrate = 48000, .gstPay = "rtpopuspay", .gstDepay = "rtpopusdepay", .gstEnc = "opusenc", .gstDec = "opusdec"},
        {.pt = 97, .name = "OPUS", .channels = 1, .clockrate = 48000, .gstPay = "rtpopuspay", .gstDepay = "rtpopusdepay", .gstEnc = "opusenc", .gstDec = "opusdec"},
        {.pt = 96, .name = "SPEEX", .channels = 1, .clockrate = 48000, .gstPay = "rtpspeexpay", .gstDepay = "rtpspeexdepay", .gstEnc = "speexenc", .gstDec = "speexdec"},
        {.pt = 96, .name = "SPEEX", .channels = 1, .clockrate = 44100, .gstPay = "rtpspeexpay", .gstDepay = "rtpspeexdepay", .gstEnc = "speexenc", .gstDec = "speexdec"},
        {.pt = 8, .name = "PCMA", .channels = 1, .clockrate = 8000, .gstPay = "rtppcmapay", .gstDepay = "rtppcmadepay", .gstEnc = "alawenc", .gstDec = "alawdec"},
        {.pt = 0, .name = "PCMU", .channels = 1, .clockrate = 8000, .gstPay = "rtppcmupay", .gstDepay = "rtppcmudepay", .gstEnc = "mulawenc", .gstDec = "mulawdec"}
    };

private:
    QXmppCall *q;
};

#endif
