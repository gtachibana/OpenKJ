#ifndef OPENKJEMBEDDEDAPI_H
#define OPENKJEMBEDDEDAPI_H

#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHash>
#include <QJsonObject>
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

signals:
    void songSubmitted();

private:
    struct HttpRequest
    {
        QString method;
        QString path;
        QHash<QString, QString> headers;
        QByteArray body;
    };

    QTcpServer m_server;
    QHash<QTcpSocket *, QByteArray> m_buffers;
    TableModelRotation &m_rotationModel;
    TableModelQueueSongs &m_queueModel;
    Settings &m_settings;

    void onNewConnection();
    void onSocketReadyRead();
    void onSocketDisconnected();

    bool tryParseHttpRequest(QByteArray &buffer, HttpRequest &request);
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
    static QByteArray hashPassword(const QString &password);
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
