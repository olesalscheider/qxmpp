#include <QApplication>
#include <QHostAddress>
#include <QHostInfo>
#include <QObject>
#include "QXmppClient.h"
#include "QXmppCallManager.h"

GstElement *pipeline;
bool a = false;

void handleStreamMedia(QXmppCallStream *stream) {
    if (stream->media() == "audio") {
        stream->setReceivePadCallback([](GstPad * receivePad) {
            GstElement *output = gst_parse_bin_from_description("audioresample ! audioconvert ! autoaudiosink", true, nullptr);
            if (!gst_bin_add(GST_BIN(pipeline), output)) {
                qFatal("Failed to add input to pipeline");
                return;
            }
            gst_pad_link(receivePad, gst_element_get_static_pad(output, "sink"));
            gst_element_sync_state_with_parent(output);
        });
        stream->setSendPadCallback([](GstPad * sendPad) {
            GstElement *input = gst_parse_bin_from_description("autoaudiosrc ! audioconvert ! audioresample ! queue max-size-time=1000000", true, nullptr);
            if (!gst_bin_add(GST_BIN(pipeline), input)) {
                qFatal("Failed to add input to pipeline");
                return;
            }
            gst_pad_link(gst_element_get_static_pad(input, "src"), sendPad);
            gst_element_sync_state_with_parent(input);
        });
    } else {
        stream->setSendPadCallback([](GstPad * sendPad) {
            GstElement *input;
	    if (a) input = gst_parse_bin_from_description("autovideosrc ! video/x-raw,width=1280,height=720 ! videoconvert ! video/x-raw,format=I420 ! queue max-size-time=1000000", true, nullptr);
	    else input = gst_parse_bin_from_description("videotestsrc ! video/x-raw,width=640,height=480 ! videoconvert ! video/x-raw,format=I420 ! queue max-size-time=1000000", true, nullptr);
            if (!gst_bin_add(GST_BIN(pipeline), input)) {
                qFatal("Failed to add input to pipeline");
                return;
            }
            gst_pad_link(gst_element_get_static_pad(input, "src"), sendPad);
            gst_element_sync_state_with_parent(input);
        });

        stream->setReceivePadCallback([](GstPad * receivePad) {
            GstElement *output = gst_parse_bin_from_description("autovideosink", true, nullptr);
            if (!gst_bin_add(GST_BIN(pipeline), output)) {
                qFatal("Failed to add input to pipeline");
                return;
            }
            gst_pad_link(receivePad, gst_element_get_static_pad(output, "sink"));
            gst_element_sync_state_with_parent(output);
        });
    }
}


void handleMedia(QXmppCall *call) {
    pipeline = call->pipeline();
    if (call->audioStream()) {
        handleStreamMedia(call->audioStream());
        call->addVideo();
    }
    if (call->videoStream()) {
        handleStreamMedia(call->videoStream());
    }
    QObject::connect(call, &QXmppCall::streamCreated, [&] (QXmppCallStream *stream) {
        handleStreamMedia(stream);
    });
}

int main(int argc, char **argv)
{
    QApplication app (argc, argv);

    if (app.arguments().size() != 3 && app.arguments().size() != 4) {
        qFatal("You need to pass 2 or 3 arguments");
    }
    auto user = app.arguments()[1];
    auto password = app.arguments()[2];

    QXmppClient client;
    client.logger()->setLoggingType(QXmppLogger::StdoutLogging);
    QXmppConfiguration config;
    config.setResource("callee");
    config.setJid(user);
    config.setPassword(password);

    QXmppCallManager *callManager = new QXmppCallManager();
    client.addExtension(callManager);

    QHostInfo stunInfo = QHostInfo::fromName("stun.l.google.com");
    QList<QPair<QHostAddress, quint16>> stunServers;
    for (auto &address : stunInfo.addresses()) {
        stunServers.push_back({address, 19302});
    }
    callManager->setStunServers(stunServers);

    QObject::connect(callManager, &QXmppCallManager::callReceived, [] (QXmppCall *call) {
        call->accept();
        handleMedia(call);
    });
    QString callJid;
    if (app.arguments().size() == 4) {
        callJid = app.arguments()[3];
        config.setResource("caller");
	a = true;
        QObject::connect(&client, &QXmppClient::stateChanged, [&] (QXmppClient::State state) {
            if (state != QXmppClient::State::ConnectedState) {
                return;
            }

            auto call = callManager->call(callJid);
            handleMedia(call);
        });
    }

    client.connectToServer(config);

    return app.exec();
}
