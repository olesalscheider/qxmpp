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

#include <QGst/Init>

#include "QXmppCallManager.h"
#include "QXmppCallManager_p.h"
#include "QXmppCall.h"
#include "QXmppCall_p.h"
#include "QXmppClient.h"
#include "QXmppConstants_p.h"
#include "QXmppJingleIq.h"
#include "QXmppStun.h"
#include "QXmppUtils.h"


QXmppCallManagerPrivate::QXmppCallManagerPrivate(QXmppCallManager *qq)
    : stunPort(0),
    turnPort(0),
    q(qq)
{
    // Initialize GStreamer
    QGst::init();
}

QXmppCall *QXmppCallManagerPrivate::findCall(const QString &sid) const
{
    foreach (QXmppCall *call, calls)
        if (call->sid() == sid)
           return call;
    return 0;
}

QXmppCall *QXmppCallManagerPrivate::findCall(const QString &sid, QXmppCall::Direction direction) const
{
    foreach (QXmppCall *call, calls)
        if (call->sid() == sid && call->direction() == direction)
           return call;
    return 0;
}

/// Constructs a QXmppCallManager object to handle incoming and outgoing
/// Voice-Over-IP calls.
///

QXmppCallManager::QXmppCallManager()
{
    d = new QXmppCallManagerPrivate(this);
}

/// Destroys the QXmppCallManager object.

QXmppCallManager::~QXmppCallManager()
{
    delete d;
}

/// \cond
QStringList QXmppCallManager::discoveryFeatures() const
{
    return QStringList()
        << ns_jingle            // XEP-0166 : Jingle
        << ns_jingle_rtp        // XEP-0167 : Jingle RTP Sessions
        << ns_jingle_rtp_audio
        << ns_jingle_rtp_video
        << ns_jingle_ice_udp;    // XEP-0176 : Jingle ICE-UDP Transport Method
}

bool QXmppCallManager::handleStanza(const QDomElement &element)
{
    if(element.tagName() == "iq")
    {
        // XEP-0166: Jingle
        if (QXmppJingleIq::isJingleIq(element))
        {
            QXmppJingleIq jingleIq;
            jingleIq.parse(element);
            _q_jingleIqReceived(jingleIq);
            return true;
        }
    }

    return false;
}

void QXmppCallManager::setClient(QXmppClient *client)
{
    bool check;
    Q_UNUSED(check);

    QXmppClientExtension::setClient(client);

    check = connect(client, SIGNAL(disconnected()),
                    this, SLOT(_q_disconnected()));
    Q_ASSERT(check);

    check = connect(client, SIGNAL(iqReceived(QXmppIq)),
                    this, SLOT(_q_iqReceived(QXmppIq)));
    Q_ASSERT(check);

    check = connect(client, SIGNAL(presenceReceived(QXmppPresence)),
                    this, SLOT(_q_presenceReceived(QXmppPresence)));
    Q_ASSERT(check);
}
/// \endcond

/// Initiates a new outgoing call to the specified recipient.
///
/// \param jid

QXmppCall *QXmppCallManager::call(const QString &jid)
{
    bool check;
    Q_UNUSED(check);

    if (jid.isEmpty()) {
        warning("Refusing to call an empty jid");
        return 0;
    }

    if (jid == client()->configuration().jid()) {
        warning("Refusing to call self");
        return 0;
    }

    QXmppCall *call = new QXmppCall(jid, QXmppCall::OutgoingDirection, this);
    QXmppCallStream *stream = call->d->createStream("audio", "initiator", "microphone");
    call->d->streams << stream;
    call->d->sid = QXmppUtils::generateStanzaHash();

    // register call
    d->calls << call;
    check = connect(call, SIGNAL(destroyed(QObject*)),
                    this, SLOT(_q_callDestroyed(QObject*)));
    Q_ASSERT(check);
    emit callStarted(call);

    call->d->sendInvite();

    return call;
}

/// Sets the STUN server to use to determine server-reflexive addresses
/// and ports.
///
/// \param host The address of the STUN server.
/// \param port The port of the STUN server.

void QXmppCallManager::setStunServer(const QHostAddress &host, quint16 port)
{
    d->stunHost = host;
    d->stunPort = port;
}

/// Sets the TURN server to use to relay packets in double-NAT configurations.
///
/// \param host The address of the TURN server.
/// \param port The port of the TURN server.

void QXmppCallManager::setTurnServer(const QHostAddress &host, quint16 port)
{
    d->turnHost = host;
    d->turnPort = port;
}

/// Sets the \a user used for authentication with the TURN server.
///
/// \param user

void QXmppCallManager::setTurnUser(const QString &user)
{
    d->turnUser = user;
}

/// Sets the \a password used for authentication with the TURN server.
///
/// \param password

void QXmppCallManager::setTurnPassword(const QString &password)
{
    d->turnPassword = password;
}

/// Handles call destruction.

void QXmppCallManager::_q_callDestroyed(QObject *object)
{
    d->calls.removeAll(static_cast<QXmppCall*>(object));
}

/// Handles disconnection from server.

void QXmppCallManager::_q_disconnected()
{
    foreach (QXmppCall *call, d->calls)
        call->d->terminate(QXmppJingleIq::Reason::Gone);
}

/// Handles acknowledgements.
///

void QXmppCallManager::_q_iqReceived(const QXmppIq &ack)
{
    if (ack.type() != QXmppIq::Result)
        return;

    // find request
    foreach (QXmppCall *call, d->calls)
        call->d->handleAck(ack);
}

/// Handles a Jingle IQ.
///

void QXmppCallManager::_q_jingleIqReceived(const QXmppJingleIq &iq)
{
    bool check;
    Q_UNUSED(check);

    if (iq.type() != QXmppIq::Set)
        return;

    if (iq.action() == QXmppJingleIq::SessionInitiate)
    {
        // build call
        QXmppCall *call = new QXmppCall(iq.from(), QXmppCall::IncomingDirection, this);
        call->d->sid = iq.sid();

        const QXmppJingleIq::Content content = iq.contents().isEmpty() ? QXmppJingleIq::Content() : iq.contents().first();
        QXmppCallStream *stream = call->d->createStream(content.descriptionMedia(), content.creator(), content.name());
        if (!stream)
            return;
        call->d->streams << stream;

        // send ack
        call->d->sendAck(iq);

        // check content description and transport
        if (!call->d->handleDescription(stream, content) ||
            !call->d->handleTransport(stream, content)) {

            // terminate call
            call->d->terminate(QXmppJingleIq::Reason::FailedApplication);
            call->terminated();
            delete call;
            return;
        }

        // register call
        d->calls << call;
        check = connect(call, SIGNAL(destroyed(QObject*)),
                        this, SLOT(_q_callDestroyed(QObject*)));
        Q_ASSERT(check);

        // send ringing indication
        QXmppJingleIq ringing;
        ringing.setTo(call->jid());
        ringing.setType(QXmppIq::Set);
        ringing.setAction(QXmppJingleIq::SessionInfo);
        ringing.setSid(call->sid());
        ringing.setRinging(true);
        call->d->sendRequest(ringing);

        // notify user
        emit callReceived(call);
        return;

    } else {

        // for all other requests, require a valid call
        QXmppCall *call = d->findCall(iq.sid());
        if (!call) {
            warning(QString("Remote party %1 sent a request for an unknown call %2").arg(iq.from(), iq.sid()));
            return;
        }
        call->d->handleRequest(iq);
    }
}

/// Handles a presence.

void QXmppCallManager::_q_presenceReceived(const QXmppPresence &presence)
{
    if (presence.type() != QXmppPresence::Unavailable)
        return;

    foreach (QXmppCall *call, d->calls) {
        if (presence.from() == call->jid()) {
            // the remote party has gone away, terminate call
            call->d->terminate(QXmppJingleIq::Reason::Gone);
        }
    }
}
