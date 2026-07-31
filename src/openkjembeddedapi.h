#ifndef OPENKJEMBEDDEDAPI_H
#define OPENKJEMBEDDEDAPI_H

#include <QObject>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QTimer>
#include <QUrlQuery>
#include "models/tablemodelrotation.h"
#include "models/tablemodelqueuesongs.h"
#include "settings.h"

class OpenKJEmbeddedApi : public QObject
{
    Q_OBJECT

public:
    explicit OpenKJEmbeddedApi(TableModelRotation &rotationModel,
                               TableModelQueueSongs &queueModel,
                               Settings &settings,
                               QObject *parent = nullptr);

    bool start(quint16 port = 5050, const QHostAddress &address = QHostAddress::Any);
    void stop();

    // A local user's rotation row is tied to their account by name and nothing else,
    // so a rename on either side has to move the other or the singer's phone controls
    // stop finding them. These let the desktop rename paths keep the account in step;
    // the singer's own profile edit does the same thing from inside
    // updateLocalUsername(). Renaming a singer who has no account succeeds without
    // doing anything, so callers need not know whether this one is a local user.
    // False means the rename must not go ahead, with the reason written to error.
    [[nodiscard]] bool localUserExists(const QString &username) const;
    bool renameLocalUser(const QString &currentUsername, const QString &nextUsername, QString *error = nullptr);

public slots:
    // Keeps the API's idea of the current key in step with the pipeline. Needed
    // because queuesongs.keychg is not authoritative while a song plays - the KJ can
    // move the desktop spinbox, which retunes without touching the database.
    void setLivePitch(int semitones);
    // Writes that key down against the singer, the way a nudge from their phone
    // already does. Separate from setLivePitch() because only MainWindow can tell a
    // deliberate mid-song adjustment from the setPitchShift() that fires on every
    // song load, and the load-time one must not be saved.
    void commitLivePitch(int semitones);
    // Whether a karaoke track is actually on screen right now. The queuesongs row a
    // singer is "currently" on stays played = 1 after the music ends, so the database
    // alone cannot tell a song in progress from one that just finished - and the skip
    // route has to know the difference before it stops anything.
    void setKaraokePlaying(bool playing);

signals:
    void songSubmitted();
    // Asks MainWindow to retune the running karaoke pipeline. The API has no handle
    // on the media backend, and only MainWindow can keep the KJ's spinbox in step.
    void pitchChangeRequested(int semitones);
    // Asks MainWindow to end the running karaoke track early, on behalf of the singer
    // performing it. Deliberately not the KJ's stop button: that one halts the show,
    // this one lets the rotation advance as if the song had played out.
    void songSkipRequested();
    // A batch of reactions to float up the singer screen. count is how many arrived
    // for this reaction since the last flush; songTotal is the running tally for the
    // performance, which is what the on-screen counter shows.
    void cheerReceived(const QString &reaction, int count, int songTotal);

private:
    struct HttpRequest
    {
        QString method;
        QString path;
        QHash<QString, QString> headers;
        QByteArray body;
    };

    // Per-socket read state. lastActivityMs is what the idle sweep uses to drop
    // connections that opened and then went quiet - a phone that starts a request
    // and sleeps mid-send would otherwise hold its socket and buffer forever.
    struct Connection
    {
        QByteArray buffer;
        qint64 lastActivityMs{0};
    };

    enum class ParseResult
    {
        Incomplete,
        Complete,
        Malformed,
        HeaderTooLarge,
        BodyTooLarge
    };

    // Failed-login bookkeeping for one peer address.
    struct LoginThrottle
    {
        int failures{0};
        qint64 windowStartMs{0};
        qint64 lockedUntilMs{0};
    };

    // Leaky bucket for one peer address, in thousandths of a cheer so the refill can
    // be integer arithmetic. Lets a phone tap a short burst and then settle to a
    // steady rate, instead of either dropping half a genuine ovation or letting one
    // client hold down a button and own the screen.
    struct CheerBucket
    {
        int tokensMilli{0};
        qint64 lastRefillMs{0};
    };

    QTcpServer m_server;
    QHash<QTcpSocket *, Connection> m_connections;
    TableModelRotation &m_rotationModel;
    TableModelQueueSongs &m_queueModel;
    Settings &m_settings;

    // Sockets parked on GET /local/events. They never get a Content-Length or a
    // close - we hold them open and push snapshots until the client goes away.
    QSet<QTcpSocket *> m_sseClients;
    // The subset that asked for delta frames with ?deltas=1. A client not in here
    // keeps getting a full snapshot on every tick, which is what the wire looked
    // like before deltas existed.
    QSet<QTcpSocket *> m_sseDeltaClients;
    QTimer m_sseBroadcastTimer;
    QTimer m_sseRefreshTimer;
    QTimer m_idleSweepTimer;
    quint64 m_sseEventId{0};
    // Hash of the last snapshot with its countdowns and timestamps removed, so a
    // broadcast can tell "the show moved" from "twenty seconds passed".
    QByteArray m_lastSnapshotFingerprint;
    // Last delta frame built, to recognise a tick that would say nothing at all.
    QByteArray m_lastTick;

    // Singers already told they are coming up, so a redraw, a keepalive tick, or a
    // phone reconnecting does not ping the same person twice. A name leaves the set
    // once the singer leaves the alert window - taking the mic does that - which
    // re-arms them for their next turn around the rotation.
    QSet<QString> m_upNextNotified;

    // Address of the client currently being served - the socket peer, or the address
    // it was forwarded from when the peer is a trusted proxy. The handlers are plain
    // JSON-in/JSON-out functions with no socket access, and only the auth ones care
    // who is calling; stashing it here beats threading it through every signature.
    // Safe because handling is synchronous and single-threaded.
    QString m_currentClientKey;
    QHash<QString, LoginThrottle> m_loginThrottle;

    // Reactions banked since the last flush, keyed by reaction name.
    QHash<QString, int> m_cheerPending;
    QTimer m_cheerFlushTimer;
    QHash<QString, CheerBucket> m_cheerBuckets;
    // Reactions for the performance in progress. Reset when a song starts rather than
    // when one ends, so the applause that lands after the music stops still counts
    // toward the song it was for.
    int m_cheerSongTotal{0};

    // Live pipeline key, fed by setLivePitch(). Authoritative while a song plays.
    int m_livePitch{0};
    // Fed by setKaraokePlaying(). False in the gap between one singer's song ending
    // and the next one starting, which the queuesongs rows cannot express.
    bool m_karaokePlaying{false};

    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void sweepIdleConnections();

    void beginEventStream(QTcpSocket *socket, const QUrlQuery &query);
    void scheduleSseBroadcast();
    void broadcastSseSnapshot();
    // Hash of everything in a snapshot except the values that move on their own.
    // Two snapshots with the same fingerprint describe the same show.
    static QByteArray snapshotFingerprint(const QJsonObject &snapshot);
    // The countdowns alone, keyed by request id, for when that is all that changed.
    // A few hundred bytes against the tens of kilobytes of a full snapshot, which
    // matters once the stream is crossing a tunnel rather than the room's wifi.
    static QJsonObject snapshotTick(const QJsonObject &snapshot);
    // Finds singers who have just come within the KJ's alert window and pushes one
    // "upnext" frame for each. Runs off the back of every snapshot broadcast.
    void evaluateUpNext();
    void writeSseFrame(QTcpSocket *socket, const QByteArray &eventName, const QByteArray &data);
    // A bare comment, which EventSource discards. Sent in place of a frame when a
    // tick would carry no news, so the connection still sees traffic - an idle
    // stream is exactly what a tunnel or proxy eventually reaps.
    void writeSseKeepalive(QTcpSocket *socket);
    // Drops a stream that has closed or stopped reading. False means the caller
    // must not write: the socket is already out of the client sets.
    bool sseSocketWritable(QTcpSocket *socket);

    void sendErrorAndClose(QTcpSocket *socket, int statusCode, const QString &message);
    // What the rate limiters key on. Behind a proxy every request arrives from the
    // proxy's own address, which would collapse the whole room into one bucket, so a
    // forwarded client address is preferred - but only from a hop we trust, since
    // otherwise it is just a header the client picked.
    QString clientKeyForRequest(QTcpSocket *socket, const HttpRequest &request);
    bool isTrustedProxy(const QHostAddress &peer);
    // Rejects further login attempts from m_currentClientKey once it has failed too
    // many times in a row. Applies to the shared admin password as much as to singer
    // accounts - the admin password is the more valuable of the two to guess.
    bool loginThrottled(int *retryAfterSecs);
    void noteLoginFailure();
    void clearLoginFailures();

    ParseResult tryParseHttpRequest(QByteArray &buffer, HttpRequest &request);
    QByteArray handleRequest(const HttpRequest &request);
    QJsonObject handleApiCommand(const QJsonObject &payload);
    QByteArray handleLocalApiGet(const QString &path, const QUrlQuery &query);
    QByteArray handleLocalApiPost(const QString &path, const QJsonObject &payload);

    QByteArray jsonResponse(int statusCode, const QJsonObject &object) const;
    static QByteArray statusText(int code);
    static QHash<QString, QString> parseHeaders(const QList<QByteArray> &headerLines);
    static void parseRequestPath(const QString &path, QString &cleanPath, QUrlQuery &query);

    int nextSerial();
    bool isAccepting() const;
    void setAccepting(bool accepting);

    QJsonObject commandSearch(const QJsonObject &payload);
    QJsonObject commandSubmitRequest(const QJsonObject &payload);
    QJsonObject commandGetRequests();
    QJsonObject commandDeleteRequest(const QJsonObject &payload);
    QJsonObject commandSetAccepting(const QJsonObject &payload);
    QJsonObject commandAdminAction(const QJsonObject &payload);

    bool removeQueueSongById(int qsongId);
    void normalizeSingerQueuePositions(int singerId);
    bool moveQueueSongByOffset(int qsongId, int offset);
    bool setQueueSongKey(int qsongId, int keyChange);
    int currentSingerId() const;
    int nextSingerId(int direction) const;
    QJsonObject buildQueueResponse();
    QJsonObject buildCapabilities() const;
    QJsonObject buildEventSettings() const;
    bool ensureLocalModeSchema();
    static QString normalizeUsername(const QString &username);
    // Legacy scheme: a single unsalted SHA-256 pass. Only still here to verify
    // accounts created before the PBKDF2 switch; those get upgraded on next login.
    static QByteArray legacyHashPassword(const QString &password);
    static QByteArray derivePassword(const QString &password, const QByteArray &salt, int iterations);
    static QByteArray randomSalt();
    static bool constantTimeEquals(const QByteArray &a, const QByteArray &b);
    // Checks a password against the stored credential and, when it matched a legacy
    // hash, rewrites the row in the current scheme so the weak hash stops existing.
    bool verifyPassword(const QString &normalizedUsername, const QString &password);
    bool storePassword(const QString &normalizedUsername, const QString &password);
    QString createUserSession(const QString &normalizedUsername);
    QString createAdminSession();
    bool isValidUserSession(const QString &token, QString *normalizedUsername = nullptr, QString *username = nullptr);
    bool isValidAdminSession(const QString &token);
    QJsonObject registerLocalUser(const QJsonObject &payload);
    QJsonObject loginLocalUser(const QJsonObject &payload);
    QJsonObject logoutLocalUser(const QJsonObject &payload);
    QJsonObject currentLocalUser(const QUrlQuery &query);
    QJsonObject updateLocalUsername(const QJsonObject &payload);
    // Moves every row keyed on a username - sessions, request ownership, favorites,
    // saved keys - plus the sung-song history, from one name to the other. Shared by
    // the singer's own profile edit and by renameLocalUser(). Does not touch the
    // rotation: the two callers differ on whether they still need to.
    bool migrateLocalUserRows(const QString &currentNormalized, const QString &nextUsername);
    QJsonObject updateLocalPassword(const QJsonObject &payload);
    QJsonObject loginAdmin(const QJsonObject &payload);
    QJsonObject logoutAdmin(const QJsonObject &payload);
    QJsonObject currentAdmin(const QUrlQuery &query);
    QJsonObject requestSongFromLocalUser(const QJsonObject &payload);
    QJsonObject removeOwnRequest(const QJsonObject &payload);
    QJsonObject moveOwnRequest(const QJsonObject &payload);
    QJsonObject setOwnAway(const QJsonObject &payload);
    // Nudges the key of the song the caller is singing right now by one semitone.
    // Relative rather than absolute so a stale client cannot yank the pitch away
    // from wherever the KJ has just put it.
    QJsonObject setOwnSongKey(const QJsonObject &payload);
    // Ends the caller's own performance early. Only ever reachable by whoever is at
    // the mic, so a singer can bail out of a song they picked badly without having to
    // catch the KJ's eye.
    QJsonObject skipOwnSong(const QJsonObject &payload);
    // Takes one reaction from the room. Deliberately open to anyone who can reach the
    // server: the audience applauding is the entire point, and most of a bar has no
    // account. A token is accepted but not required, and buys nothing except that the
    // rate limit is charged the same either way.
    QJsonObject recordCheer(const QJsonObject &payload);
    // Hands the batch since the last flush to the singer screen and to the event
    // stream. Batched because thirty phones tapping at once is the success case, and
    // each tap must not become its own repaint and its own SSE frame.
    void flushCheers();
    // True when this peer has spent its bucket. Separate from the login throttle: that
    // one locks an address out for minutes after a burst of failures, which is the
    // wrong shape here - a cheering room should be smoothed, not punished.
    bool cheerThrottled();
    // qsongid of the entry currently being sung, or -1. Unlike the queue entries the
    // other user routes touch, this row has played = 1.
    int nowPlayingQueueSongId(QString *singerName = nullptr, int *songId = nullptr) const;
    // The same entry, but only if the caller is the one singing it. -1 with *error set
    // otherwise. Every route that acts on the performance in progress - rather than on
    // a queued request - needs exactly this, so the between-songs guard and the
    // ownership rules live here rather than in each handler. songId may be null.
    int callerNowPlayingQueueSongId(const QString &normalizedUsername, int *songId, QString *error) const;
    void rememberUserSongKey(const QString &normalizedUsername, int songId, int keyChange);
    int preferredUserSongKey(const QString &normalizedUsername, int songId) const;
    // True when the queue entry belongs to this user, either by ownership
    // record or - for songs the KJ added on their behalf - by singer name.
    bool userOwnsRequest(int requestId, const QString &normalizedUsername, QString *error) const;
    QJsonObject listUserFavorites(const QUrlQuery &query);
    QJsonObject addUserFavorite(const QJsonObject &payload);
    QJsonObject removeUserFavorite(const QJsonObject &payload);
    QJsonObject listUserHistory(const QUrlQuery &query);
    QJsonObject runAdminActionRest(const QJsonObject &payload);
    bool recordRequestOwner(int requestId, const QString &normalizedUsername);
    QString requestOwner(int requestId) const;
    bool removeUserSessionToken(const QString &token);
    bool removeAdminSessionToken(const QString &token);
};

#endif
