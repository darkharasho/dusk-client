// The control channel between this client and Dusk.
//
// This is a fork addition, not upstream Moonlight. Dusk launches the client
// with two environment variables -- DUSK_SESSION_ENDPOINT, a unix socket
// path or a Windows named pipe, and DUSK_SESSION_TOKEN, a one-session
// secret -- and listens on that endpoint for the length of one stream. The
// protocol is newline-delimited JSON in both directions and is specified on
// Dusk's side, in src-tauri/src/session/protocol.rs; read that rather than
// trusting a summary here.
//
// Environment rather than arguments because Moonlight rejects arguments it
// does not recognise: a client that has never heard of Dusk ignores the
// variables and streams exactly as before, and so does this one when they
// are absent.
//
// # Threading
//
// The stream hijacks the main thread for its SDL loop and suspends Qt event
// processing until it ends, so a socket living there would never be read.
// The connection lives on a thread of its own with its own event loop.
// Messages to Dusk are queued across to it; messages from Dusk come back as
// SDL events, which is the one thing the stream loop is guaranteed to read
// and SDL_PushEvent is safe from any thread.

#pragma once

#include <QAtomicInt>
#include <QByteArray>
#include <QString>

class QThread;

namespace DuskChannelPrivate { class Link; }

// SDL_USEREVENT codes the channel posts to the stream loop. Kept well clear
// of upstream's, which count up from 100.
#define SDL_CODE_DUSK_SHOW_OVERLAY 200
#define SDL_CODE_DUSK_HOST_DISPLAY 201
// Text for this machine's clipboard, and a line for the overlay. Both carry
// their text in data1 as a heap QByteArray (UTF-8) that the stream loop owns
// and deletes -- on every path that drops the event, too.
#define SDL_CODE_DUSK_CLIPBOARD 202
#define SDL_CODE_DUSK_NOTICE 203
// Posted by the stream's own timer to hide a notice; data2 says which.
#define SDL_CODE_DUSK_NOTICE_CLEAR 204
#define DUSK_NOTICE_MS 3000

class DuskChannel
{
public:
    /// Connects to Dusk if Dusk launched this client, otherwise returns
    /// nullptr and costs nothing.
    static DuskChannel* startFromEnvironment();

    /// Says goodbye, gives the last messages a moment to leave, and stops
    /// the thread. Blocks briefly; call it from session teardown.
    ~DuskChannel();

    /// The person used the overlay: "disconnect", "settings" or "quality".
    /// Intents rather than commands -- Dusk decides what they mean.
    void sendOverlayAction(const char* action);

    /// How the stream is doing over the last measurement window. A negative
    /// value means "not measured" and is left out, so Dusk shows what it has
    /// rather than a misleading zero. Safe from any thread.
    void sendStats(double fps, double bitrateKbps, double rttMs,
                   double decodeMs, double packetLossPercent);

    /// Text copied on this machine, for the host's clipboard. Dusk enforces
    /// the size limit and says so with a notice. Safe from any thread; the
    /// text is never logged.
    void sendClipboard(const QString& text);

    /// Frees the text payloads of clipboard and notice events still in SDL's
    /// queue, and drops those events. Call after the channel is destroyed,
    /// when the stream loop will no longer be there to delete them.
    static void discardQueuedPayloads();

    /// Whether Dusk has the other end. Messages sent before the connection
    /// is up are held and delivered after the handshake, so this is for
    /// deciding what to offer, not for deciding whether to send.
    bool isConnected() const;

private:
    DuskChannel(const QString& endpoint, const QByteArray& token);

    void post(const QByteArray& line);

    QAtomicInt m_Connected;
    QThread* m_Thread;
    DuskChannelPrivate::Link* m_Link;
};
