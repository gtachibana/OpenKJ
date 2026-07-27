#ifndef OPENKJEMBEDDEDAPI_H
#define OPENKJEMBEDDEDAPI_H

#include <QObject>
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

public slots:
    // Keeps the API's idea of the current key in step with the pipeline. Needed
    // because queuesongs.keychg is not authoritative while a song plays - the KJ can
    // move the desktop spinbox, which retunes without touching the database.
    void setLivePitch(int semitones);

signals:
    void songSubmitted();
    // Asks MainWindow to retune the running karaoke pipeline. The API has no handle
    // on the media backend, and only MainWindow can keep the KJ's spinbox in step.
    void pitchChangeRequested(int semitones);

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

    QTcpServer m_server;
    QHash<QTcpSocket *, Connection> m_connections;
    TableModelRotation &m_rotationModel;
    TableModelQueueSongs &m_queueModel;
    Settings &m_settings;

    // Sockets parked on GET /local/events. They never get a Content-Length or a
    // close - we hold them open and push snapshots until the client goes away.
    QSet<QTcpSocket *> m_sseClients;
    QTimer m_sseBroadcastTimer;
    QTimer m_sseRefreshTimer;
    QTimer m_idleSweepTimer;
    quint64 m_sseEventId{0};

    // Peer address of the request currently being handled. The handlers are plain
    // JSON-in/JSON-out functions with no socket access, and only the auth ones care
    // who is calling; stashing it here beats threading it through every signature.
    // Safe because handling is synchronous and single-threaded.
    QString m_currentClientKey;
    QHash<QString, LoginThrottle> m_loginThrottle;

    // Live pipeline key, fed by setLivePitch(). Authoritative while a song plays.
    int m_livePitch{0};

    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();
    void sweepIdleConnections();

    void beginEventStream(QTcpSocket *socket);
    void scheduleSseBroadcast();
    void broadcastSseSnapshot();
    void writeSseFrame(QTcpSocket *socket, const QByteArray &eventName, const QByteArray &data);

    void sendErrorAndClose(QTcpSocket *socket, int statusCode, const QString &message);
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
    // qsongid of the entry currently being sung, or -1. Unlike the queue entries the
    // other user routes touch, this row has played = 1.
    int nowPlayingQueueSongId(QString *singerName = nullptr, int *songId = nullptr) const;
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
