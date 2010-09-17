/*
 * Copyright (C) 2008-2010 The QXmpp developers
 *
 * Author:
 *  Jeremy Lainé
 *
 * Source:
 *  http://code.google.com/p/qxmpp
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

#ifndef QXMPPPUBSUBIQ_H
#define QXMPPPUBSUBIQ_H

#include "QXmppIq.h"

/// \brief The QXmppPubSubItem class represents a publish-subscribe item
/// as defined by XEP-0060: Publish-Subscribe.
///

class QXmppPubSubItem
{
public:
    QString id() const;
    void setId(const QString &id);

    QXmppElement contents() const;
    void setContents(const QXmppElement &contents);

    /// \cond
    void parse(const QDomElement &element);
    void toXml(QXmlStreamWriter *writer) const;
    /// \endcond

private:
    QString m_id;
    QXmppElement m_contents;
};

/// \brief The QXmppPubSubIq class represents an IQ used for the
/// publish-subscribe mechanisms defined by XEP-0060: Publish-Subscribe.
///
/// \ingroup Stanzas

class QXmppPubSubIq : public QXmppIq
{
public:
    enum QueryType
    {
        AffiliationsQuery,
        DefaultQuery,
        ItemsQuery,
        PublishQuery,
        RetractQuery,
        SubscribeQuery,
        SubscriptionQuery,
        SubscriptionsQuery,
        UnsubscribeQuery,
    };

    QXmppPubSubIq::QueryType queryType() const;
    void setQueryType(QXmppPubSubIq::QueryType queryType);

    QString queryJid() const;
    void setQueryJid(const QString &jid);

    QString queryNode() const;
    void setQueryNode(const QString &node);

    QList<QXmppPubSubItem> items() const;
    void setItems(const QList<QXmppPubSubItem> &items);

    QString subscriptionId() const;
    void setSubscriptionId(const QString &id);

    static bool isPubSubIq(const QDomElement &element);

protected:
    /// \cond
    void parseElementFromChild(const QDomElement&);
    void toXmlElementFromChild(QXmlStreamWriter *writer) const;
    /// \endcond

private:
    QXmppPubSubIq::QueryType m_queryType;
    QString m_queryJid;
    QString m_queryNode;
    QList<QXmppPubSubItem> m_items;
    QString m_subscriptionId;
    QString m_subscriptionType;
};

#endif