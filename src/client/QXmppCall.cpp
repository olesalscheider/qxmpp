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

#include <QDomElement>
#include <QTimer>

#include <QGlib/Connect>
#include <QGst/ElementFactory>
#include <QGst/Pipeline>
#include <QGst/Structure>

#include "QXmppCall.h"
#include "QXmppCall_p.h"
#include "QXmppCallStream.h"
#include "QXmppCallStream_p.h"
#include "QXmppCallManager.h"
#include "QXmppCallManager_p.h"
#include "QXmppClient.h"
#include "QXmppConstants_p.h"
#include "QXmppJingleIq.h"
#include "QXmppStun.h"
#include "QXmppUtils.h"

#undef emit
#include <QGlib/Connect>
#include <QGlib/Signal>

QXmppCallPrivate::QXmppCallPrivate(QXmppCall *qq)
    : direction(QXmppCall::IncomingDirection),
    manager(0),
    state(QXmppCall::ConnectingState),
    nextId(0),
    q(qq)
{
    qRegisterMetaType<QXmppCall::State>();

    filterGStreamerFormats(videoCodecs);
    filterGStreamerFormats(audioCodecs);

    pipeline = QGst::Pipeline::create();
    rtpbin = QGst::ElementFactory::make("rtpbin");
    if (!rtpbin) {
        qFatal("Failed to create rtpbin");
        return;
    }
    // We do not want to build up latency over time
    rtpbin->setProperty("drop-on-latency", true);
    pipeline->add(rtpbin);
    QGlib::connect(rtpbin, "pad-added", this, &QXmppCallPrivate::padAdded);
    QGlib::connect(rtpbin, "request-pt-map", this, &QXmppCallPrivate::ptMap);
    QGlib::connect(rtpbin, "on-ssrc-active", this, &QXmppCallPrivate::ssrcActive);
    pipeline->setState(QGst::StatePlaying);
}

QXmppCallPrivate::~QXmppCallPrivate()
{
    pipeline->setState(QGst::StateNull);
    for (auto stream : streams) {
        delete stream;
    }
    pipeline.clear();
}

void QXmppCallPrivate::ssrcActive(uint sessionId, uint ssrc)
{
    Q_UNUSED(ssrc)
    auto internalSession = QGlib::emit<QGst::ElementPtr>(rtpbin, "get-session", static_cast<uint>(sessionId));
    //qWarning(internalSession->property("stats").get<QGst::Structure>().toString().toLatin1().data());
    // TODO: implement bitrate controller
}

void QXmppCallPrivate::padAdded(const QGst::PadPtr &pad)
{
    auto nameParts = pad->name().split("_");
    if (nameParts.size() < 4) {
        return;
    }
    if (nameParts[0] == QLatin1String("send") &&
        nameParts[1] == QLatin1String("rtp") &&
        nameParts[2] == QLatin1String("src")) {
        if (nameParts.size() != 4) {
            return;
        }
        int sessionId = nameParts[3].toInt();
        auto stream = findStreamById(sessionId);
        stream->d->addRtpSender(pad);
    } else if (nameParts[0] == QLatin1String("recv") ||
               nameParts[1] == QLatin1String("rtp") ||
               nameParts[2] == QLatin1String("src")) {
        if (nameParts.size() != 6) {
            return;
        }
        int sessionId = nameParts[3].toInt();
        int pt = nameParts[5].toInt();
        auto stream = findStreamById(sessionId);
        if (stream->media() == VIDEO_MEDIA) {
            for (auto &codec : videoCodecs) {
                if (codec.pt == pt) {
                    stream->d->addDecoder(pad, codec);
                    return;
                }
            }
        } else if (stream->media() == AUDIO_MEDIA) {
            for (auto &codec : audioCodecs) {
                if (codec.pt == pt) {
                    stream->d->addDecoder(pad, codec);
                    return;
                }
            }
        }
    }
}

QGst::CapsPtr QXmppCallPrivate::ptMap(uint sessionId, uint pt)
{
    auto stream = findStreamById(sessionId);
    for (auto &payloadType : stream->d->payloadTypes) {
        if (payloadType.id() == pt) {
            return QGst::Caps::fromString(QString("application/x-rtp,media=(string)%1,clock-rate=(int)%2,encoding-name=(string)%3")
                .arg(stream->media()).arg(payloadType.clockrate()).arg(payloadType.name()).toLatin1().data());
        }
    }
    q->warning(QString("Remote party %1 transmits wrong %2 payload for call %3").arg(jid, stream->media(), sid));
    return QGst::CapsPtr();
}

void QXmppCallPrivate::filterGStreamerFormats(QList<GstCodec> &formats)
{
    auto it = formats.begin();
    while (it != formats.end()) {
        bool supported = QGst::ElementFactory::find(it->gstPay) &&
                         QGst::ElementFactory::find(it->gstDepay) &&
                         QGst::ElementFactory::find(it->gstEnc) &&
                         QGst::ElementFactory::find(it->gstDec);
        if (!supported) {
            it = formats.erase(it);
        } else {
            ++it;
        }
    }
}

QXmppCallStream *QXmppCallPrivate::findStreamByMedia(const QString &media)
{
    for (auto stream : streams) {
        if (stream->media() == media) {
            return stream;
        }
    }
    return nullptr;
}

QXmppCallStream *QXmppCallPrivate::findStreamByName(const QString &name)
{
    for (auto stream : streams) {
        if (stream->name() == name) {
            return stream;
        }
    }
    return nullptr;
}

QXmppCallStream *QXmppCallPrivate::findStreamById(const int id)
{
    for (auto stream : streams) {
        if (stream->id() == id) {
            return stream;
        }
    }
    return nullptr;
}

void QXmppCallPrivate::handleAck(const QXmppIq &ack)
{
    const QString id = ack.id();
    for (int i = 0; i < requests.size(); ++i) {
        if (id == requests[i].id()) {
            // process acknowledgement
            const QXmppJingleIq request = requests.takeAt(i);
            q->debug(QString("Received ACK for packet %1").arg(id));

            // handle termination
            if (request.action() == QXmppJingleIq::SessionTerminate)
                q->terminated();
            return;
        }
    }
}

bool QXmppCallPrivate::handleDescription(QXmppCallStream* stream, const QXmppJingleIq::Content &content)
{
    stream->d->payloadTypes = content.payloadTypes();
    auto it = stream->d->payloadTypes.begin();
    bool foundCandidate = false;
    while (it != stream->d->payloadTypes.end()) {
        bool dynamic = it->id() >= 96;
        bool supported = false;
        auto codecs = stream->media() == AUDIO_MEDIA ? audioCodecs : videoCodecs;
        for (auto &codec : codecs) {
            if (dynamic) {
                if (codec.name == it->name() &&
                    codec.clockrate == it->clockrate() &&
                    codec.channels == it->channels()) {
                    if (!foundCandidate) {
                        stream->d->addEncoder(codec);
                        foundCandidate = true;
                    }
                    supported = true;
                    /* Adopt id from other side. */
                    codec.pt = it->id();
                }
            } else {
                if (codec.pt == it->id() &&
                    codec.clockrate == it->clockrate() &&
                    codec.channels == it->channels()) {
                    if (!foundCandidate) {
                        stream->d->addEncoder(codec);
                        foundCandidate = true;
                    }
                    supported = true;
                    /* Keep our name just to be sure */
                    codec.name = it->name();
                }
            }
        }

        if (!supported) {
            it = stream->d->payloadTypes.erase(it);
        } else {
            ++it;
        }
    }

    if (stream->d->payloadTypes.empty()) {
        q->warning(QString("Remote party %1 did not provide any known %2 payloads for call %3").arg(jid, stream->media(), sid));
        return false;
    }

    return true;
}

bool QXmppCallPrivate::handleTransport(QXmppCallStream *stream, const QXmppJingleIq::Content &content)
{
    stream->d->connection->setRemoteUser(content.transportUser());
    stream->d->connection->setRemotePassword(content.transportPassword());
    for (const QXmppJingleCandidate &candidate : content.transportCandidates()) {
        stream->d->connection->addRemoteCandidate(candidate);
    }

    // perform ICE negotiation
    if (!content.transportCandidates().isEmpty()) {
        stream->d->connection->connectToHost();
    }
    return true;
}

void QXmppCallPrivate::handleRequest(const QXmppJingleIq &iq)
{
    const QXmppJingleIq::Content content = iq.contents().isEmpty() ? QXmppJingleIq::Content() : iq.contents().first();

    if (iq.action() == QXmppJingleIq::SessionAccept) {

        if (direction == QXmppCall::IncomingDirection) {
            q->warning("Ignoring Session-Accept for an incoming call");
            return;
        }

        // send ack
        sendAck(iq);

        // check content description and transport
        QXmppCallStream *stream = findStreamByName(content.name());
        if (!stream ||
            !handleDescription(stream, content) ||
            !handleTransport(stream, content)) {

            // terminate call
            terminate(QXmppJingleIq::Reason::FailedApplication);
            return;
        }

        // check for call establishment
        setState(QXmppCall::ActiveState);

    } else if (iq.action() == QXmppJingleIq::SessionInfo) {

        // notify user
        QTimer::singleShot(0, q, SIGNAL(ringing()));

    } else if (iq.action() == QXmppJingleIq::SessionTerminate) {

        // send ack
        sendAck(iq);

        // terminate
        q->info(QString("Remote party %1 terminated call %2").arg(iq.from(), iq.sid()));
        q->terminated();

    } else if (iq.action() == QXmppJingleIq::ContentAccept) {

        // send ack
        sendAck(iq);

        // check content description and transport
        QXmppCallStream *stream = findStreamByName(content.name());
        if (!stream ||
            !handleDescription(stream, content) ||
            !handleTransport(stream, content)) {

            // FIXME: what action?
            return;
        }

    } else if (iq.action() == QXmppJingleIq::ContentAdd) {

        // send ack
        sendAck(iq);

        // check media stream does not exist yet
        QXmppCallStream *stream = findStreamByName(content.name());
        if (stream)
            return;

        // create media stream
        stream = createStream(content.descriptionMedia(), content.creator(), content.name());
        if (!stream)
            return;
        streams << stream;

        // check content description
        if (!handleDescription(stream, content) ||
            !handleTransport(stream, content)) {

            QXmppJingleIq iq;
            iq.setTo(q->jid());
            iq.setType(QXmppIq::Set);
            iq.setAction(QXmppJingleIq::ContentReject);
            iq.setSid(q->sid());
            iq.reason().setType(QXmppJingleIq::Reason::FailedApplication);
            sendRequest(iq);
            streams.removeAll(stream);
            delete stream;
            return;
        }

         // accept content
        QXmppJingleIq iq;
        iq.setTo(q->jid());
        iq.setType(QXmppIq::Set);
        iq.setAction(QXmppJingleIq::ContentAccept);
        iq.setSid(q->sid());
        iq.addContent(localContent(stream));
        sendRequest(iq);

    } else if (iq.action() == QXmppJingleIq::TransportInfo) {

        // send ack
        sendAck(iq);

        // check content transport
        QXmppCallStream *stream = findStreamByName(content.name());
        if (!stream ||
            !handleTransport(stream, content)) {
            // FIXME: what action?
            return;
        }

    }
}

QXmppCallStream *QXmppCallPrivate::createStream(const QString &media, const QString &creator, const QString &name)
{
    bool check;
    Q_UNUSED(check);
    Q_ASSERT(manager);

    if (media != AUDIO_MEDIA && media != VIDEO_MEDIA) {
        q->warning(QString("Unsupported media type %1").arg(media));
        return nullptr;
    }

    if (!QGst::ElementFactory::find("rtpbin")) {
        q->warning("The rtpbin GStreamer plugin is missing. Calls are not possible.");
        return nullptr;
    }

    QXmppCallStream *stream = new QXmppCallStream(pipeline, rtpbin, media, creator, name, ++nextId);

    // Fill local payload payload types
    auto &codecs = media == AUDIO_MEDIA ? audioCodecs : videoCodecs;
    for (auto &codec : codecs) {
        QXmppJinglePayloadType payloadType;
        payloadType.setId(codec.pt);
        payloadType.setName(codec.name);
        payloadType.setChannels(codec.channels);
        payloadType.setClockrate(codec.clockrate);
        stream->d->payloadTypes.append(payloadType);
    }

    // ICE connection
    stream->d->connection->setIceControlling(direction == QXmppCall::OutgoingDirection);
    stream->d->connection->setStunServer(manager->d->stunHost, manager->d->stunPort);
    stream->d->connection->setTurnServer(manager->d->turnHost, manager->d->turnPort);
    stream->d->connection->setTurnUser(manager->d->turnUser);
    stream->d->connection->setTurnPassword(manager->d->turnPassword);
    stream->d->connection->bind(QXmppIceComponent::discoverAddresses());

    // connect signals
    check = QObject::connect(stream->d->connection, SIGNAL(localCandidatesChanged()),
        q, SLOT(localCandidatesChanged()));
    Q_ASSERT(check);

    check = QObject::connect(stream->d->connection, SIGNAL(disconnected()),
        q, SLOT(hangup()));
    Q_ASSERT(check);

    Q_EMIT q->streamCreated(stream);

    return stream;
}

QXmppJingleIq::Content QXmppCallPrivate::localContent(QXmppCallStream *stream) const
{
    QXmppJingleIq::Content content;
    content.setCreator(stream->creator());
    content.setName(stream->name());
    content.setSenders("both");

    // description
    content.setDescriptionMedia(stream->media());
    content.setDescriptionSsrc(stream->d->localSsrc);
    content.setPayloadTypes(stream->d->payloadTypes);

    // transport
    content.setTransportUser(stream->d->connection->localUser());
    content.setTransportPassword(stream->d->connection->localPassword());
    content.setTransportCandidates(stream->d->connection->localCandidates());

    return content;
}

/// Sends an acknowledgement for a Jingle IQ.
///

bool QXmppCallPrivate::sendAck(const QXmppJingleIq &iq)
{
    QXmppIq ack;
    ack.setId(iq.id());
    ack.setTo(iq.from());
    ack.setType(QXmppIq::Result);
    return manager->client()->sendPacket(ack);
}

bool QXmppCallPrivate::sendInvite()
{
    // create audio stream
    QXmppCallStream *stream = findStreamByMedia(AUDIO_MEDIA);
    Q_ASSERT(stream);

    QXmppJingleIq iq;
    iq.setTo(jid);
    iq.setType(QXmppIq::Set);
    iq.setAction(QXmppJingleIq::SessionInitiate);
    iq.setInitiator(ownJid);
    iq.setSid(sid);
    iq.addContent(localContent(stream));
    return sendRequest(iq);
}

/// Sends a Jingle IQ and adds it to outstanding requests.
///

bool QXmppCallPrivate::sendRequest(const QXmppJingleIq &iq)
{
    requests << iq;
    return manager->client()->sendPacket(iq);
}

void QXmppCallPrivate::setState(QXmppCall::State newState)
{
    if (state != newState)
    {
        state = newState;
        Q_EMIT q->stateChanged(state);

        if (state == QXmppCall::ActiveState)
            Q_EMIT q->connected();
        else if (state == QXmppCall::FinishedState)
            Q_EMIT q->finished();
    }
}

/// Request graceful call termination

void QXmppCallPrivate::terminate(QXmppJingleIq::Reason::Type reasonType)
{
    if (state == QXmppCall::DisconnectingState ||
        state == QXmppCall::FinishedState)
        return;

    // hangup call
    QXmppJingleIq iq;
    iq.setTo(jid);
    iq.setType(QXmppIq::Set);
    iq.setAction(QXmppJingleIq::SessionTerminate);
    iq.setSid(sid);
    iq.reason().setType(reasonType);
    sendRequest(iq);
    setState(QXmppCall::DisconnectingState);

    // schedule forceful termination in 5s
    QTimer::singleShot(5000, q, SLOT(terminated()));
}

QXmppCall::QXmppCall(const QString &jid, QXmppCall::Direction direction, QXmppCallManager *parent)
    : QXmppLoggable(parent)
{
    d = new QXmppCallPrivate(this);
    d->direction = direction;
    d->jid = jid;
    d->ownJid = parent->client()->configuration().jid();
    d->manager = parent;
}

QXmppCall::~QXmppCall()
{
    delete d;
}

/// Call this method if you wish to accept an incoming call.
///

void QXmppCall::accept()
{
    if (d->direction == IncomingDirection && d->state == ConnectingState)
    {
        Q_ASSERT(d->streams.size() == 1);
        QXmppCallStream *stream = d->streams.first();

        // accept incoming call
        QXmppJingleIq iq;
        iq.setTo(d->jid);
        iq.setType(QXmppIq::Set);
        iq.setAction(QXmppJingleIq::SessionAccept);
        iq.setResponder(d->ownJid);
        iq.setSid(d->sid);
        iq.addContent(d->localContent(stream));
        d->sendRequest(iq);

        // notify user
        d->manager->callStarted(this);

        // check for call establishment
        d->setState(QXmppCall::ActiveState);
    }
}

/// Returns the GStreamer pipeline.

QGst::PipelinePtr QXmppCall::pipeline() const
{
    return d->pipeline;
}

/// Returns the RTP stream for the audio data.

QXmppCallStream *QXmppCall::audioStream() const
{
    return d->findStreamByMedia(AUDIO_MEDIA);
}

/// Returns the RTP stream for the video data.

QXmppCallStream *QXmppCall::videoStream() const
{
    return d->findStreamByMedia(VIDEO_MEDIA);
}

void QXmppCall::terminated()
{
    // close streams
    for (auto stream : d->streams) {
        stream->d->connection->close();
    }

    // update state
    d->setState(QXmppCall::FinishedState);
}

/// Returns the call's direction.
///

QXmppCall::Direction QXmppCall::direction() const
{
    return d->direction;
}

/// Hangs up the call.
///

void QXmppCall::hangup()
{
    d->terminate(QXmppJingleIq::Reason::None);
}

/// Sends a transport-info to inform the remote party of new local candidates.
///

void QXmppCall::localCandidatesChanged()
{
    // find the stream
    QXmppIceConnection *conn = qobject_cast<QXmppIceConnection*>(sender());
    QXmppCallStream *stream = 0;
    for (auto ptr : d->streams) {
        if (ptr->d->connection == conn) {
            stream = ptr;
            break;
        }
    }
    if (!stream)
        return;

    QXmppJingleIq iq;
    iq.setTo(d->jid);
    iq.setType(QXmppIq::Set);
    iq.setAction(QXmppJingleIq::TransportInfo);
    iq.setSid(d->sid);
    iq.addContent(d->localContent(stream));
    d->sendRequest(iq);
}

/// Returns the remote party's JID.
///

QString QXmppCall::jid() const
{
    return d->jid;
}

/// Returns the call's session identifier.
///

QString QXmppCall::sid() const
{
    return d->sid;
}

/// Returns the call's state.
///
/// \sa stateChanged()

QXmppCall::State QXmppCall::state() const
{
    return d->state;
}

/// Starts sending video to the remote party.

void QXmppCall::addVideo()
{
    if (d->state != QXmppCall::ActiveState) {
        warning("Cannot add video, call is not active");
        return;
    }

    QXmppCallStream *stream = d->findStreamByMedia(VIDEO_MEDIA);
    if (stream) {
        return;
    }

    // create video stream
    QLatin1String creator = (d->direction == QXmppCall::OutgoingDirection) ? QLatin1String("initiator") : QLatin1String("responder");
    stream = d->createStream(VIDEO_MEDIA, creator, QLatin1String("webcam"));
    d->streams << stream;

    // build request
    QXmppJingleIq iq;
    iq.setTo(d->jid);
    iq.setType(QXmppIq::Set);
    iq.setAction(QXmppJingleIq::ContentAdd);
    iq.setSid(d->sid);
    iq.addContent(d->localContent(stream));
    d->sendRequest(iq);
}
