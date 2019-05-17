/*
 * Copyright (C) 2019 The QXmpp developers
 *
 * Author:
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

#include <QGst/Buffer>
#include <QGst/ElementFactory>
#include <QGst/GhostPad>
#include <QGst/Pipeline>
#include <QGst/Utils/ApplicationSink>
#include <QGst/Utils/ApplicationSource>

#include <cstring>

#include "QXmppCallStream.h"
#include "QXmppCallStream_p.h"
#include "QXmppCall_p.h"
#include "QXmppStun.h"

#undef emit
#include <QGlib/Signal>

QXmppCallStreamPrivate::QXmppCallStreamPrivate(QXmppCallStream *parent, QGst::PipelinePtr pipeline_,
                                               QGst::ElementPtr rtpbin_, QString media_, QString creator_,
                                               QString name_, int id_)
: QObject(parent), q(parent), pipeline(pipeline_), rtpbin(rtpbin_), media(media_), creator(creator_), name(name_), id(id_)
{
    localSsrc = qrand();

    bool status;

    encoderWrapperBin = QGst::Bin::create(QStringLiteral("encoderwrapper_%1").arg(id).toLatin1().data());
    decoderWrapperBin = QGst::Bin::create(QStringLiteral("decoderwrapper_%1").arg(id).toLatin1().data());
    iceReceiveBin = QGst::Bin::create(QStringLiteral("receive_%1").arg(id).toLatin1().data());
    iceSendBin = QGst::Bin::create(QStringLiteral("send_%1").arg(id).toLatin1().data());
    pipeline->add(encoderWrapperBin);
    pipeline->add(decoderWrapperBin);
    pipeline->add(iceReceiveBin);
    pipeline->add(iceSendBin);
    encoderWrapperBin->syncStateWithParent();
    decoderWrapperBin->syncStateWithParent();
    iceReceiveBin->syncStateWithParent();
    iceSendBin->syncStateWithParent();

    receivePad = QGst::GhostPad::create(QGst::PadDirection::PadSrc);
    internalReceivePad = QGst::GhostPad::create(QGst::PadDirection::PadSink);
    sendPad = QGst::GhostPad::create(QGst::PadDirection::PadSink);
    decoderWrapperBin->addPad(receivePad);
    decoderWrapperBin->addPad(internalReceivePad);
    encoderWrapperBin->addPad(sendPad);

    connection = new QXmppIceConnection(this);
    connection->addComponent(RTP_COMPONENT);
    connection->addComponent(RTCP_COMPONENT);
    apprtpsink = new IceApplicationSink(connection, RTP_COMPONENT);
    apprtcpsink = new IceApplicationSink(connection, RTCP_COMPONENT);

    // TODO check these parameters
    apprtpsink->element()->setProperty("max-buffers", 2);
    apprtpsink->element()->setProperty("drop", true);
    apprtpsrc.element()->setProperty("is-live", true);
    apprtpsrc.element()->setProperty("max-latency", 50000000);
    apprtcpsrc.element()->setProperty("is-live", true);

    connect(connection->component(RTP_COMPONENT), &QXmppIceComponent::datagramReceived,
            [&] (const QByteArray &datagram) {datagramReceived(datagram, apprtpsrc);});
    connect(connection->component(RTCP_COMPONENT), &QXmppIceComponent::datagramReceived,
            [&] (const QByteArray &datagram) {datagramReceived(datagram, apprtcpsrc);});

    internalRtpPad = QGst::GhostPad::create(QGst::PadDirection::PadSink);
    internalRtcpPad = QGst::GhostPad::create(QGst::PadDirection::PadSink);
    iceSendBin->addPad(internalRtpPad);
    iceSendBin->addPad(internalRtcpPad);

    status = iceReceiveBin->add(apprtpsrc.element());
    Q_ASSERT(status);
    status = iceReceiveBin->add(apprtcpsrc.element());
    Q_ASSERT(status);
    apprtpsrc.element()->syncStateWithParent();
    apprtcpsrc.element()->syncStateWithParent();
    status = apprtpsrc.element()->link(rtpbin, QStringLiteral("recv_rtp_sink_%1").arg(id).toLatin1().data());
    Q_ASSERT(status);
    status = apprtcpsrc.element()->link(rtpbin, QStringLiteral("recv_rtcp_sink_%1").arg(id).toLatin1().data());
    Q_ASSERT(status);

    // We need frequent RTCP reports for the bandwidth controller
    QGlib::emit<QGst::ElementPtr>(rtpbin, "get-session", static_cast<uint>(id))->setProperty("rtcp-min-interval", 100000000);
}

QXmppCallStreamPrivate::~QXmppCallStreamPrivate()
{
    connection->close();

    // Remove elements from pipeline
    pipeline->remove(encoderWrapperBin);
    pipeline->remove(decoderWrapperBin);
    pipeline->remove(iceSendBin);
    pipeline->remove(iceReceiveBin);

    delete apprtpsink;
    delete apprtcpsink;
}

void QXmppCallStreamPrivate::datagramReceived(const QByteArray &datagram, QGst::Utils::ApplicationSource &appsrc)
{
    QGst::BufferPtr buffer = QGst::Buffer::create(datagram.size());
    QGst::MapInfo mapInfo;
    buffer->map(mapInfo, QGst::MapWrite);
    std::memcpy(mapInfo.data(), datagram.data(), mapInfo.size());
    buffer->unmap(mapInfo);
    appsrc.pushBuffer(buffer);
}

void QXmppCallStreamPrivate::addEncoder(QXmppCallPrivate::GstCodec &codec)
{
    bool status;

    // Remove old encoder and payloader if they exist
    if (!encoderBin.isNull()) {
        encoderWrapperBin->remove(encoderBin);
    }
    encoderBin = QGst::Bin::create("encoder");
    encoderWrapperBin->add(encoderBin);
    encoderBin->syncStateWithParent();

    // Create new elements
    QGst::ElementPtr pay = QGst::ElementFactory::make(codec.gstPay);
    pay->setProperty("pt", codec.pt);
    pay->setProperty("ssrc", localSsrc);
    status = encoderBin->add(pay);
    Q_ASSERT(status);
    pay->syncStateWithParent();
    QGst::ElementPtr encoder = QGst::ElementFactory::make(codec.gstEnc);
    for (auto &encProp : codec.encProps) {
        encoder->setProperty(encProp.name.toLatin1().data(), encProp.value);
    }
    status = encoderBin->add(encoder);
    Q_ASSERT(status);
    encoder->syncStateWithParent();
    QGst::ElementPtr encoderInput;
    if (media == AUDIO_MEDIA) {
        encoderInput = QGst::Bin::fromDescription(
            QStringLiteral("audioconvert ! audioresample ! audiorate ! capsfilter caps=audio/x-raw,rate=%1,channels=%2 ! queue max-size-time=100000000")
            .arg(codec.clockrate).arg(codec.channels).toLatin1().data());
    } else {
        encoderInput = QGst::Bin::fromDescription(
            QStringLiteral("videoconvert ! queue max-size-time=1000000000").toLatin1().data());
    }
    status = encoderBin->add(encoderInput);
    Q_ASSERT(status);
    encoderInput->syncStateWithParent();
    status = pay->link(rtpbin, QStringLiteral("send_rtp_sink_%1").arg(id).toLatin1().data());
    Q_ASSERT(status);
    status = encoder->link(pay, "sink");
    Q_ASSERT(status);
    status = encoderInput->link(encoder, "sink");
    Q_ASSERT(status);
    status = sendPad->setTarget(encoderInput->getStaticPad("sink"));
    Q_ASSERT(status);

    addRtcpSender(rtpbin->getRequestPad(QStringLiteral("send_rtcp_src_%1").arg(id).toLatin1().data()));

    Q_EMIT q->sendPadAdded(sendPad);
}

void QXmppCallStreamPrivate::addDecoder(QGst::PadPtr pad, QXmppCallPrivate::GstCodec &codec)
{
    bool status;

    // Remove old decoder and depayloader if they exist
    if (!decoderBin.isNull()) {
        decoderWrapperBin->remove(decoderBin);
    }
    decoderBin = QGst::Bin::create("decoder");
    decoderWrapperBin->add(decoderBin);
    decoderBin->syncStateWithParent();

    QGst::ElementPtr depay = QGst::ElementFactory::make(codec.gstDepay);
    status = decoderBin->add(depay);
    depay->syncStateWithParent();
    Q_ASSERT(status);
    QGst::ElementPtr decoder = QGst::ElementFactory::make(codec.gstDec);
    status = decoderBin->add(decoder);
    decoder->syncStateWithParent();
    Q_ASSERT(status);
    internalReceivePad->setTarget(depay->getStaticPad("sink"));
    status = pad->link(internalReceivePad) == QGst::PadLinkOk;
    Q_ASSERT(status);
    status = depay->link(decoder, "sink");
    Q_ASSERT(status);
    receivePad->setTarget(decoder->getStaticPad("src"));
    Q_ASSERT(status);

    Q_EMIT q->receivePadAdded(sendPad);
}

void QXmppCallStreamPrivate::addRtpSender(QGst::PadPtr pad)
{
    bool status;
    status = iceSendBin->add(apprtpsink->element());
    apprtpsink->element()->syncStateWithParent();
    Q_ASSERT(status);
    internalRtpPad->setTarget(apprtpsink->element()->getStaticPad("sink"));
    status = pad->link(internalRtpPad) == QGst::PadLinkOk;
    Q_ASSERT(status);
}

void QXmppCallStreamPrivate::addRtcpSender(QGst::PadPtr pad)
{
    bool status;
    status = iceSendBin->add(apprtcpsink->element());
    apprtcpsink->element()->syncStateWithParent();
    Q_ASSERT(status);
    internalRtcpPad->setTarget(apprtcpsink->element()->getStaticPad("sink"));
    status = pad->link(internalRtcpPad) == QGst::PadLinkOk;
    Q_ASSERT(status);
}

IceApplicationSink::IceApplicationSink(QXmppIceConnection *connection_, int component_)
: connection(connection_), component(component_)
{
}

void IceApplicationSink::eos()
{
    connection->close();
}

QGst::FlowReturn IceApplicationSink::newSample()
{
    QGst::SamplePtr sample = pullSample();
    QGst::MapInfo mapInfo;
    sample->buffer()->map(mapInfo, QGst::MapRead);
    QByteArray datagram;
    datagram.resize(mapInfo.size());
    std::memcpy(datagram.data(), mapInfo.data(), mapInfo.size());
    sample->buffer()->unmap(mapInfo);
    if (connection->component(component)->isConnected() &&
        connection->component(component)->sendDatagram(datagram) != datagram.size()) {
        return QGst::FlowError;
    }
    return QGst::FlowOk;
}

QXmppCallStream::QXmppCallStream(QGst::PipelinePtr pipeline, QGst::ElementPtr rtpbin,
                                 QString media, QString creator, QString name, int id)
{
    d = new QXmppCallStreamPrivate(this, pipeline, rtpbin, media, creator, name, id);
}

QString QXmppCallStream::creator() const
{
    return d->creator;
}

QString QXmppCallStream::media() const
{
    return d->media;
}

QString QXmppCallStream::name() const
{
    return d->name;
}

int QXmppCallStream::id() const
{
    return d->id;
}
