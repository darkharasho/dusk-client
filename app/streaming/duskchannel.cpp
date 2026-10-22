#include "duskchannel.h"

#include "SDL_compat.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QThread>

// How long teardown waits for the last messages to leave. Long enough for a
// local socket, short enough that a wedged Dusk cannot hold a closing stream
// hostage.
#define FLUSH_TIMEOUT_MS 500

namespace DuskChannelPrivate {

// Lives on the channel's thread and owns the socket. Everything in here runs
// on that thread; the only things it touches from elsewhere are the atomic
// connected flag and SDL_PushEvent, both safe from anywhere.
class Link : public QObject
{
public:
    Link(const QString& endpoint, const QByteArray& token, QAtomicInt* connected)
        : m_Endpoint(endpoint),
          m_Token(token),
          m_Connected(connected),
          m_Socket(nullptr)
    {
    }

    void open()
    {
        m_Socket = new QLocalSocket(this);

        QObject::connect(m_Socket, &QLocalSocket::connected, this, [this] {
            // The handshake goes first and alone. Dusk acts on nothing
            // until it lands, so anything queued meanwhile follows it.
            QJsonObject ready;
            ready["type"] = "ready";
            ready["token"] = QString::fromLatin1(m_Token);
            writeNow(QJsonDocument(ready).toJson(QJsonDocument::Compact) + '\n');

            for (const QByteArray& line : m_Pending) {
                writeNow(line);
            }
            m_Pending.clear();

            m_Connected->storeRelease(1);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Dusk: control channel connected");
        });

        QObject::connect(m_Socket, &QLocalSocket::readyRead, this, [this] {
            while (m_Socket->canReadLine()) {
                receive(m_Socket->readLine().trimmed());
            }
        });

        QObject::connect(m_Socket, &QLocalSocket::disconnected, this, [this] {
            m_Connected->storeRelease(0);
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION, "Dusk: control channel closed");
        });

        // Qt before 5.15 (the Steam Link's, for one) calls this signal
        // error, and overloads the name, so it has to be picked by type.
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
        QObject::connect(m_Socket, &QLocalSocket::errorOccurred, this,
#else
        QObject::connect(m_Socket,
                         QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error),
                         this,
#endif
                         [this](QLocalSocket::LocalSocketError) {
            // Not fatal to the stream. A Dusk that is not listening any more
            // costs the overlay its link back, and nothing else.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Dusk: control channel error: %s",
                        qPrintable(m_Socket->errorString()));
        });

        // A path on unix; on Windows Qt takes a full \\.\pipe\ name as is.
        m_Socket->connectToServer(m_Endpoint);
    }

    void write(const QByteArray& line)
    {
        if (m_Socket != nullptr && m_Socket->state() == QLocalSocket::ConnectedState) {
            writeNow(line);
        }
        else if (m_Socket != nullptr && m_Socket->state() == QLocalSocket::ConnectingState) {
            m_Pending.append(line);
        }
        // Otherwise there is nobody to tell, which is not an error: it is
        // what happens when Dusk has already gone away.
    }

    void close(const QByteArray& lastLine)
    {
        if (m_Socket == nullptr) {
            return;
        }
        if (m_Socket->state() == QLocalSocket::ConnectedState) {
            writeNow(lastLine);
            m_Socket->waitForBytesWritten(FLUSH_TIMEOUT_MS);
            m_Socket->disconnectFromServer();
        }
        else {
            m_Socket->abort();
        }
        m_Connected->storeRelease(0);
    }

private:
    void writeNow(const QByteArray& line)
    {
        m_Socket->write(line);
        m_Socket->flush();
    }

    void receive(const QByteArray& line)
    {
        // Some writers end with a blank line; it is not worth a diagnosis.
        if (line.isEmpty()) {
            return;
        }

        QJsonParseError error;
        QJsonDocument doc = QJsonDocument::fromJson(line, &error);
        if (!doc.isObject()) {
            // A desynchronised line stream does not recover on its own, so
            // stop listening rather than act on half a protocol. Dusk does
            // the same in the other direction.
            SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                        "Dusk: unreadable message, closing the channel: %s",
                        qPrintable(error.errorString()));
            m_Socket->abort();
            m_Connected->storeRelease(0);
            return;
        }

        QJsonObject message = doc.object();
        QString type = message.value("type").toString();

        if (type == "disconnect") {
            // Same path as the quit key combo and the overlay's own
            // Disconnect, so there is one teardown however it is asked for.
            SDL_Event event;
            SDL_zero(event);
            event.type = SDL_QUIT;
            event.quit.timestamp = SDL_GetTicks();
            SDL_PushEvent(&event);
        }
        else if (type == "show_overlay") {
            pushUserEvent(SDL_CODE_DUSK_SHOW_OVERLAY, 0);
        }
        else if (type == "host_display") {
            // Sunshine numbers its outputs from one, on Ctrl+Alt+Shift+F1..F12.
            bool ok = false;
            int id = message.value("id").toString().toInt(&ok);
            if (ok && id >= 1 && id <= 12) {
                pushUserEvent(SDL_CODE_DUSK_HOST_DISPLAY, id - 1);
            }
            else {
                SDL_LogWarn(SDL_LOG_CATEGORY_APPLICATION,
                            "Dusk: no host display %s",
                            qPrintable(message.value("id").toString()));
            }
        }
        else if (type == "clipboard") {
            // Only plain text is understood; other formats are from a newer
            // Dusk. The text is deliberately kept out of every log line.
            if (message.value("format").toString() == "text") {
                pushTextEvent(SDL_CODE_DUSK_CLIPBOARD, message.value("text").toString());
            }
        }
        else if (type == "notice") {
            pushTextEvent(SDL_CODE_DUSK_NOTICE, message.value("text").toString());
        }
        else if (type == "set_quality") {
            // Changing quality in flight means renegotiating the stream,
            // which this client cannot do yet. Said rather than silently
            // dropped, so it is obvious from a log which side is behind.
            SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
                        "Dusk: set_quality is not supported by this client yet");
        }
        // Anything else is from a newer Dusk. Ignored, not fatal, so the two
        // can be upgraded independently.
    }

    static void pushUserEvent(int code, int value)
    {
        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_USEREVENT;
        event.user.code = code;
        event.user.data1 = (void*)(intptr_t)value;
        SDL_PushEvent(&event);
    }

    // The text goes in a heap QByteArray the stream loop deletes. If the
    // event never makes it onto the queue, nobody else will, so it goes here.
    static void pushTextEvent(int code, const QString& text)
    {
        QByteArray* payload = new QByteArray(text.toUtf8());

        SDL_Event event;
        SDL_zero(event);
        event.type = SDL_USEREVENT;
        event.user.code = code;
        event.user.data1 = payload;
        if (SDL_PushEvent(&event) < 0) {
            delete payload;
        }
    }

    QString m_Endpoint;
    QByteArray m_Token;
    QAtomicInt* m_Connected;
    QLocalSocket* m_Socket;
    QList<QByteArray> m_Pending;
};

}

using DuskChannelPrivate::Link;

namespace {

QByteArray encode(const QJsonObject& message)
{
    return QJsonDocument(message).toJson(QJsonDocument::Compact) + '\n';
}

}

DuskChannel* DuskChannel::startFromEnvironment()
{
    QString endpoint = qEnvironmentVariable("DUSK_SESSION_ENDPOINT");
    QByteArray token = qgetenv("DUSK_SESSION_TOKEN");
    if (endpoint.isEmpty() || token.isEmpty()) {
        return nullptr;
    }
    return new DuskChannel(endpoint, token);
}

DuskChannel::DuskChannel(const QString& endpoint, const QByteArray& token)
    : m_Thread(new QThread()),
      m_Link(new Link(endpoint, token, &m_Connected))
{
    m_Thread->setObjectName("DuskChannel");
    m_Link->moveToThread(m_Thread);
    m_Thread->start();

    Link* link = m_Link;
    QMetaObject::invokeMethod(m_Link, [link] { link->open(); }, Qt::QueuedConnection);
}

DuskChannel::~DuskChannel()
{
    // Tell Dusk the session is over before the socket goes, so it hears it
    // from us rather than inferring it from the connection dropping.
    QJsonObject bye;
    bye["type"] = "disconnected";
    QByteArray lastLine = encode(bye);

    Link* link = m_Link;
    QMetaObject::invokeMethod(m_Link, [link, lastLine] {
        link->close(lastLine);
        delete link;
    }, Qt::BlockingQueuedConnection);

    m_Thread->quit();
    m_Thread->wait();
    delete m_Thread;
}

void DuskChannel::sendOverlayAction(const char* action)
{
    QJsonObject message;
    message["type"] = "overlay_action";
    message["action"] = action;
    post(encode(message));
}

void DuskChannel::sendStats(double fps, double bitrateKbps, double rttMs,
                            double decodeMs, double packetLossPercent)
{
    QJsonObject message;
    message["type"] = "stats";
    if (fps >= 0) {
        message["fps"] = fps;
    }
    if (bitrateKbps >= 0) {
        message["bitrateKbps"] = qRound(bitrateKbps);
    }
    if (rttMs >= 0) {
        message["rttMs"] = rttMs;
    }
    if (decodeMs >= 0) {
        message["decodeMs"] = decodeMs;
    }
    if (packetLossPercent >= 0) {
        message["packetLoss"] = packetLossPercent;
    }
    post(encode(message));
}

void DuskChannel::sendClipboard(const QString& text)
{
    QJsonObject message;
    message["type"] = "clipboard";
    message["format"] = "text";
    message["text"] = text;
    post(encode(message));
}

bool DuskChannel::isConnected() const
{
    return m_Connected.loadAcquire() != 0;
}

void DuskChannel::post(const QByteArray& line)
{
    Link* link = m_Link;
    QMetaObject::invokeMethod(m_Link, [link, line] { link->write(line); },
                              Qt::QueuedConnection);
}
