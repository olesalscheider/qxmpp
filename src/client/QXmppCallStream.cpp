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

#include <gst/gst.h>
#include <cstring>

#include "QXmppCallStream.h"
#include "QXmppCallStream_p.h"
#include "QXmppCall_p.h"
#include "QXmppStun.h"

QXmppCallStreamPrivate::QXmppCallStreamPrivate(QXmppCallStream *parent, GstElement *pipeline_,
                                               GstElement *rtpbin_, QString media_, QString creator_,
                                               QString name_, int id_)
: QObject(parent), q(parent), pipeline(pipeline_), rtpbin(rtpbin_), media(media_), creator(creator_), name(name_), id(id_)
{
    localSsrc = qrand();

    encoderWrapperBin = gst_bin_new(QStringLiteral("encoderwrapper_%1").arg(id).toLatin1().data());
    decoderWrapperBin = gst_bin_new(QStringLiteral("decoderwrapper_%1").arg(id).toLatin1().data());
    encoderBin = nullptr;
    decoderBin = nullptr;
    iceReceiveBin = gst_bin_new(QStringLiteral("receive_%1").arg(id).toLatin1().data());
    iceSendBin = gst_bin_new(QStringLiteral("send_%1").arg(id).toLatin1().data());
    gst_bin_add_many(GST_BIN(pipeline), encoderWrapperBin, decoderWrapperBin, iceReceiveBin, iceSendBin, nullptr);

    receivePad = gst_ghost_pad_new_no_target(nullptr, GST_PAD_SRC);
    internalReceivePad = gst_ghost_pad_new_no_target(nullptr, GST_PAD_SINK);
    sendPad = gst_ghost_pad_new_no_target(nullptr, GST_PAD_SINK);
    if (!gst_element_add_pad(decoderWrapperBin, receivePad)) {
        qFatal("Falied to add receive pad to decoder wrapper bin");
    }
    if (!gst_element_add_pad(decoderWrapperBin, internalReceivePad)) {
        qFatal("Falied to add internal receive pad to decoder wrapper bin");
    }
    if (!gst_element_add_pad(encoderWrapperBin, sendPad)) {
        qFatal("Falied to add send pad to encoder wrapper bin");
    }

    if (!gst_element_sync_state_with_parent(encoderWrapperBin) ||
        !gst_element_sync_state_with_parent(decoderWrapperBin) ||
        !gst_element_sync_state_with_parent(iceReceiveBin) ||
        !gst_element_sync_state_with_parent(iceSendBin)) {
        qFatal("Failed to sync bin states with parent");
    }

    connection = new QXmppIceConnection(this);
    connection->addComponent(RTP_COMPONENT);
    connection->addComponent(RTCP_COMPONENT);
    apprtpsink = gst_element_factory_make("appsink", nullptr);
    apprtcpsink = gst_element_factory_make("appsink", nullptr);
    if (!apprtpsink || !apprtcpsink) {
        qFatal("Failed to create appsinks");
    }
    g_object_set(apprtpsink, "emit-signals", true, nullptr);
    g_object_set(apprtcpsink, "emit-signals", true, nullptr);
    g_signal_connect_swapped(apprtpsink, "new-sample",
                             G_CALLBACK(+[](QXmppCallStreamPrivate *p, GstElement *appsink) -> GstFlowReturn {
                                 return p->sendDatagram(appsink, RTP_COMPONENT);
                            }), this);
    g_signal_connect_swapped(apprtcpsink, "new-sample",
                             G_CALLBACK(+[](QXmppCallStreamPrivate *p, GstElement *appsink) -> GstFlowReturn {
                                 return p->sendDatagram(appsink, RTCP_COMPONENT);
                            }), this);

    apprtpsrc = gst_element_factory_make("appsrc", nullptr);
    apprtcpsrc = gst_element_factory_make("appsrc", nullptr);
    if (!apprtpsrc || !apprtcpsrc) {
        qFatal("Failed to create appsrcs");
    }

    // TODO check these parameters
    g_object_set(apprtpsink, "max-buffers", 10, "drop", true, nullptr);
    g_object_set(apprtpsrc, "is-live", true, "max-latency", 50000000, "max-bytes", "1000000", nullptr);
    g_object_set(apprtcpsrc, "is-live", true, nullptr);

    connect(connection->component(RTP_COMPONENT), &QXmppIceComponent::datagramReceived,
            [&] (const QByteArray &datagram) {datagramReceived(datagram, apprtpsrc);});
    connect(connection->component(RTCP_COMPONENT), &QXmppIceComponent::datagramReceived,
            [&] (const QByteArray &datagram) {datagramReceived(datagram, apprtcpsrc);});

    internalRtpPad = gst_ghost_pad_new_no_target(nullptr, GST_PAD_SINK);
    internalRtcpPad = gst_ghost_pad_new_no_target(nullptr, GST_PAD_SINK);
    gst_pad_set_active(internalRtpPad, true);
    gst_pad_set_active(internalRtcpPad, true);
    if (!gst_element_add_pad(iceSendBin, internalRtpPad) ||
        !gst_element_add_pad(iceSendBin, internalRtcpPad)) {
        qFatal("Failed to add pads to send bin");
    }

    if (!gst_bin_add(GST_BIN(iceReceiveBin), apprtpsrc) ||
        !gst_bin_add(GST_BIN(iceReceiveBin), apprtcpsrc)) {
        qFatal("Failed to add appsrcs to receive bin");
    }
    if (!gst_element_sync_state_with_parent(apprtpsrc) ||
        !gst_element_sync_state_with_parent(apprtcpsrc)) {
        qFatal("Failed to sync appsrc state with parent");
    }
    if (!gst_element_link_pads(apprtpsrc, "src", rtpbin, QStringLiteral("recv_rtp_sink_%1").arg(id).toLatin1().data()) ||
        !gst_element_link_pads(apprtcpsrc, "src", rtpbin, QStringLiteral("recv_rtcp_sink_%1").arg(id).toLatin1().data())) {
        qFatal("Failed to link receive pads");
    }

    // We need frequent RTCP reports for the bandwidth controller
    GstElement *rtpSession;
    g_signal_emit_by_name(rtpbin, "get-session", static_cast<uint>(id), &rtpSession);
    g_object_set(rtpSession, "rtcp-min-interval", 100000000, nullptr);
}

QXmppCallStreamPrivate::~QXmppCallStreamPrivate()
{
    connection->close();

    // Remove elements from pipeline
    if (!gst_bin_remove(GST_BIN(pipeline), encoderWrapperBin) ||
        !gst_bin_remove(GST_BIN(pipeline), decoderWrapperBin) ||
        !gst_bin_remove(GST_BIN(pipeline), iceSendBin) ||
        !gst_bin_remove(GST_BIN(pipeline), iceReceiveBin)) {
        qFatal("Failed to remove bins from pipeline");
    }
}

GstFlowReturn QXmppCallStreamPrivate::sendDatagram(GstElement *appsink, int component)
{
    GstSample *sample;
    g_signal_emit_by_name(appsink, "pull-sample", &sample);
    if (!sample) {
        qFatal("Could not get sample");
        return GST_FLOW_ERROR;
    }

    GstMapInfo mapInfo;
    GstBuffer *buffer = gst_sample_get_buffer(sample);
    if (!buffer) {
        qFatal("Could not get buffer");
        return GST_FLOW_ERROR;
    }
    if (!gst_buffer_map(buffer, &mapInfo, GST_MAP_READ)) {
        qFatal("Could not map buffer");
        return GST_FLOW_ERROR;
    }
    QByteArray datagram;
    datagram.resize(mapInfo.size);
    std::memcpy(datagram.data(), mapInfo.data, mapInfo.size);
    gst_buffer_unmap(buffer, &mapInfo);
    gst_sample_unref(sample);
    if (connection->component(component)->isConnected() &&
        connection->component(component)->sendDatagram(datagram) != datagram.size()) {
        return GST_FLOW_ERROR;
    }
    return GST_FLOW_OK;
}

void QXmppCallStreamPrivate::datagramReceived(const QByteArray &datagram, GstElement *appsrc)
{
    GstBuffer *buffer = gst_buffer_new_and_alloc(datagram.size());
    GstMapInfo mapInfo;
    if (!gst_buffer_map(buffer, &mapInfo, GST_MAP_WRITE)) {
        qFatal("Could not map buffer");
        return;
    }
    std::memcpy(mapInfo.data, datagram.data(), mapInfo.size);
    gst_buffer_unmap(buffer, &mapInfo);
    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buffer, &ret);
    gst_buffer_unref(buffer);
}

void QXmppCallStreamPrivate::addEncoder(QXmppCallPrivate::GstCodec &codec)
{
    // Remove old encoder and payloader if they exist
    if (encoderBin) {
        if (!gst_bin_remove(GST_BIN(encoderWrapperBin), encoderBin)) {
            qFatal("Failed to remove existing encoder bin");
        }
    }
    encoderBin = gst_bin_new("encoder");
    if (!gst_bin_add(GST_BIN(encoderWrapperBin), encoderBin)) {
        qFatal("Failed to add encoder bin to wrapper");
        return;
    }
    if (!gst_element_sync_state_with_parent(encoderBin)) {
        qFatal("Failed to sync encoder bin state with parent");
        return;
    }

    // Create new elements
    GstElement *pay = gst_element_factory_make(codec.gstPay.toLatin1().data(), nullptr);
    if (!pay) {
        qFatal("Failed to create payloader");
        return;
    }
    g_object_set(pay, "pt", codec.pt, "ssrc", localSsrc, nullptr);
    if (!gst_bin_add(GST_BIN(encoderBin), pay)) {
        qFatal("Failed to add payloader to encoder bin");
        return;
    }
    if (!gst_element_sync_state_with_parent(pay)) {
        qFatal("Failed to sync payloader state with parent");
        return;
    }
    GstElement *encoder = gst_element_factory_make(codec.gstEnc.toLatin1().data(), nullptr);
    if (!encoder) {
        qFatal("Failed to create encoder");
        return;
    }
    for (auto &encProp : codec.encProps) {
        g_object_set(encoder, encProp.name.toLatin1().data(), encProp.value, nullptr);
    }
    if (!gst_bin_add(GST_BIN(encoderBin), encoder)) {
        qFatal("Failed to add encoder to encoder bin");
        return;
    }
    if (!gst_element_sync_state_with_parent(encoder)) {
        qFatal("Failed to sync encoder state with parent");
        return;
    }
    if (!gst_element_link_pads(pay, "src", rtpbin, QStringLiteral("send_rtp_sink_%1").arg(id).toLatin1().data()) ||
        !gst_element_link_pads(encoder, "src", pay, "sink")) {
        qFatal("Failed to link pads");
        return;
    }

    if (!gst_ghost_pad_set_target(GST_GHOST_PAD(sendPad), gst_element_get_static_pad(encoder, "sink"))) {
        qFatal("Failed to set send pad");
        return;
    }
    addRtcpSender(gst_element_get_request_pad(rtpbin, QStringLiteral("send_rtcp_src_%1").arg(id).toLatin1().data()));

    Q_EMIT q->sendPadAdded(sendPad);
}

void QXmppCallStreamPrivate::addDecoder(GstPad *pad, QXmppCallPrivate::GstCodec &codec)
{
    // Remove old decoder and depayloader if they exist
    if (decoderBin) {
       if (!gst_bin_remove(GST_BIN(decoderWrapperBin), decoderBin)) {
            qFatal("Failed to remove existing decoder bin");
        }
    }
    decoderBin = gst_bin_new("decoder");
    if (!gst_bin_add(GST_BIN(decoderWrapperBin), decoderBin)) {
        qFatal("Failed to add decoder bin to wrapper");
        return;
    }
    if (!gst_element_sync_state_with_parent(decoderBin)) {
        qFatal("Failed to sync decoder bin state with parent");
        return;
    }

    // Create new elements
    GstElement *depay = gst_element_factory_make(codec.gstDepay.toLatin1().data(), nullptr);
    if (!depay) {
        qFatal("Failed to create depayloader");
        return;
    }
    if (!gst_bin_add(GST_BIN(decoderBin), depay)) {
        qFatal("Failed to add depayloader to decoder bin");
        return;
    }
    if (!gst_element_sync_state_with_parent(depay)) {
        qFatal("Failed to sync depayloader state with parent");
        return;
    }
    GstElement *decoder = gst_element_factory_make(codec.gstDec.toLatin1().data(), nullptr);
    if (!decoder) {
        qFatal("Failed to create decoder");
        return;
    }
    if (!gst_bin_add(GST_BIN(decoderBin), decoder)) {
        qFatal("Failed to add decoder to decoder bin");
        return;
    }
    if (!gst_element_sync_state_with_parent(decoder)) {
        qFatal("Failed to sync decoder state with parent");
        return;
    }

    if (!gst_ghost_pad_set_target(GST_GHOST_PAD(internalReceivePad), gst_element_get_static_pad(depay, "sink")) ||
        gst_pad_link(pad, internalReceivePad) != GST_PAD_LINK_OK ||
        !gst_element_link_pads(depay, "src", decoder, "sink") ||
        !gst_ghost_pad_set_target(GST_GHOST_PAD(receivePad), gst_element_get_static_pad(decoder, "src"))) {
        qFatal("Could not link all decoder pads");
        return;
    }

    Q_EMIT q->receivePadAdded(sendPad);
}

void QXmppCallStreamPrivate::addRtpSender(GstPad *pad)
{
    if (!gst_bin_add(GST_BIN(iceSendBin), apprtpsink)) {
        qFatal("Failed to add rtp sink to send bin");
    }
    if (!gst_element_sync_state_with_parent(apprtpsink)) {
        qFatal("Failed to sync rtp sink with parent");
    }
    if (!gst_ghost_pad_set_target(GST_GHOST_PAD(internalRtpPad), gst_element_get_static_pad(apprtpsink, "sink")) ||
        gst_pad_link(pad, internalRtpPad) != GST_PAD_LINK_OK) {
        qFatal("Failed to link rtp pads");
    }
}

void QXmppCallStreamPrivate::addRtcpSender(GstPad *pad)
{
    if (!gst_bin_add(GST_BIN(iceSendBin), apprtcpsink)) {
        qFatal("Failed to add rtcp sink to send bin");
    }
    if (!gst_element_sync_state_with_parent(apprtcpsink)) {
        qFatal("Failed to sync rtcp sink with parent");
    }
    if (!gst_ghost_pad_set_target(GST_GHOST_PAD(internalRtcpPad), gst_element_get_static_pad(apprtcpsink, "sink")) ||
        gst_pad_link(pad, internalRtcpPad) != GST_PAD_LINK_OK) {
        qFatal("Failed to link rtcp pads");
    }
}

QXmppCallStream::QXmppCallStream(GstElement *pipeline, GstElement *rtpbin,
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
