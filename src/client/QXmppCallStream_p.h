/*
 * Copyright (C) 2008-2019 The QXmpp developers
 *
 * Authors:
 *  Niels Ole Salscheider
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

#ifndef QXMPPCALLSTREAM_P_H
#define QXMPPCALLSTREAM_P_H

#include <QList>
#include <QObject>
#include <QString>

#include <QGst/GhostPad>
#include <QGst/Utils/ApplicationSink>
#include <QGst/Utils/ApplicationSource>

#include "QXmppCall_p.h"
#include "QXmppJingleIq.h"

class QXmppIceConnection;

//  W A R N I N G
//  -------------
//
// This file is not part of the QXmpp API.
// This header file may change from version to version without notice,
// or even be removed.
//
// We mean it.
//

static const int RTP_COMPONENT = 1;
static const int RTCP_COMPONENT = 2;

static const QLatin1String AUDIO_MEDIA("audio");
static const QLatin1String VIDEO_MEDIA("video");

class IceApplicationSink : public QGst::Utils::ApplicationSink
{
public:
    IceApplicationSink(QXmppIceConnection *connection_, int component_);
protected:
    virtual void eos() override;
    virtual QGst::FlowReturn newSample() override;
private:
    QXmppIceConnection *connection;
    int component;
};

class QXmppCallStreamPrivate : public QObject {
Q_OBJECT
public:
    QXmppCallStreamPrivate(QXmppCallStream *parent, QGst::PipelinePtr pipeline_, QGst::ElementPtr rtpbin_,
                           QString media_, QString creator_, QString name_, int id_);
    ~QXmppCallStreamPrivate();

    void datagramReceived(const QByteArray &datagram, QGst::Utils::ApplicationSource &appsrc);

    void addEncoder(QXmppCallPrivate::GstCodec &codec);
    void addDecoder(QGst::PadPtr pad, QXmppCallPrivate::GstCodec &codec);
    void addRtpSender(QGst::PadPtr pad);
    void addRtcpSender(QGst::PadPtr pad);

    QXmppCallStream *q;

    quint32 localSsrc;

    QGst::Utils::ApplicationSource apprtpsrc;
    QGst::Utils::ApplicationSource apprtcpsrc;
    IceApplicationSink *apprtpsink;
    IceApplicationSink *apprtcpsink;

    QGst::PipelinePtr pipeline;
    QGst::ElementPtr rtpbin;
    QGst::GhostPadPtr receivePad;
    QGst::GhostPadPtr internalReceivePad;
    QGst::GhostPadPtr sendPad;
    QGst::GhostPadPtr internalRtpPad;
    QGst::GhostPadPtr internalRtcpPad;
    QGst::BinPtr encoderWrapperBin;
    QGst::BinPtr decoderWrapperBin;
    QGst::BinPtr encoderBin;
    QGst::BinPtr decoderBin;
    QGst::BinPtr iceReceiveBin;
    QGst::BinPtr iceSendBin;

    QXmppIceConnection *connection;
    QString media;
    QString creator;
    QString name;
    int id;

    QList<QXmppJinglePayloadType> payloadTypes;
};

#endif
