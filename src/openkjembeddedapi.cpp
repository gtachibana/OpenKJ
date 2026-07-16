#include "openkjembeddedapi.h"

#include <algorithm>
#include <QCryptographicHash>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

OpenKJEmbeddedApi::OpenKJEmbeddedApi(TableModelRotation &rotationModel,
                                     TableModelQueueSongs &queueModel,
                                     Settings &settings,
                                     QObject *parent)
    : QObject(parent),
      m_rotationModel(rotationModel),
      m_queueModel(queueModel),
      m_settings(settings)
{
    connect(&m_server, &QTcpServer::newConnection, this, &OpenKJEmbeddedApi::onNewConnection);
}

bool OpenKJEmbeddedApi::start(const quint16 port, const QHostAddress &address)
{
    if (m_server.isListening()) {
        return true;
    }

    ensureLocalModeSchema();
    return m_server.listen(address, port);
}

void OpenKJEmbeddedApi::stop()
{
    if (!m_server.isListening()) {
        return;
    }

    for (QTcpSocket *socket : m_buffers.keys()) {
        socket->disconnectFromHost();
    }

    m_server.close();
}

void OpenKJEmbeddedApi::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        m_buffers.insert(socket, QByteArray());
        connect(socket, &QTcpSocket::readyRead, this, &OpenKJEmbeddedApi::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &OpenKJEmbeddedApi::onSocketDisconnected);
    }
}

void OpenKJEmbeddedApi::onSocketReadyRead()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket || !m_buffers.contains(socket)) {
        return;
    }

    auto &buffer = m_buffers[socket];
    buffer.append(socket->readAll());

    HttpRequest request;
    if (!tryParseHttpRequest(buffer, request)) {
        return;
    }

    const QByteArray response = handleRequest(request);
    socket->write(response);
    socket->disconnectFromHost();
}

void OpenKJEmbeddedApi::onSocketDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) {
        return;
    }

    m_buffers.remove(socket);
    socket->deleteLater();
}

bool OpenKJEmbeddedApi::tryParseHttpRequest(QByteArray &buffer, HttpRequest &request)
{
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return false;
    }

    const QByteArray headerSection = buffer.left(headerEnd);
    const QList<QByteArray> headerLines = headerSection.split('\n');
    if (headerLines.isEmpty()) {
        return false;
    }

    const QList<QByteArray> firstLineParts = headerLines.first().trimmed().split(' ');
    if (firstLineParts.size() < 2) {
        return false;
    }

    request.method = QString::fromUtf8(firstLineParts.at(0)).trimmed().toUpper();
    request.path = QString::fromUtf8(firstLineParts.at(1)).trimmed();
    request.headers = parseHeaders(headerLines.mid(1));

    const int contentLength = request.headers.value("content-length", "0").toInt();
    const int totalNeeded = headerEnd + 4 + contentLength;
    if (buffer.size() < totalNeeded) {
        return false;
    }

    request.body = buffer.mid(headerEnd + 4, contentLength);
    buffer.remove(0, totalNeeded);
    return true;
}

QByteArray OpenKJEmbeddedApi::handleRequest(const HttpRequest &request)
{
    QString cleanPath;
    QUrlQuery query;
    parseRequestPath(request.path, cleanPath, query);

    if (request.method == "GET" && cleanPath == "/health") {
        QJsonObject out;
        out.insert("status", "ok");
        out.insert("mode", m_settings.appModeName());
        return jsonResponse(200, out);
    }

    if (request.method == "GET" && cleanPath == "/stats") {
        QSqlQuery query;
        query.exec("SELECT COUNT(1) FROM dbsongs WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!'");
        int songCount = 0;
        if (query.next()) {
            songCount = query.value(0).toInt();
        }

        QSqlQuery queueQuery;
        queueQuery.exec("SELECT COUNT(1) FROM queuesongs WHERE played = 0");
        int queueCount = 0;
        if (queueQuery.next()) {
            queueCount = queueQuery.value(0).toInt();
        }

        QJsonObject out;
        out.insert("status", "ok");
        out.insert("song_count", songCount);
        out.insert("request_count", queueCount);
        out.insert("accepting", isAccepting());
        out.insert("serial", m_settings.embeddedApiSerial());
        out.insert("mode", m_settings.appModeName());
        return jsonResponse(200, out);
    }

    if (request.method == "GET" && cleanPath.startsWith("/local/")) {
        return handleLocalApiGet(cleanPath, query);
    }

    if (request.method == "POST" && cleanPath == "/api.php") {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            QJsonObject out;
            out.insert("error", "true");
            out.insert("errorString", "Invalid JSON");
            return jsonResponse(400, out);
        }

        return jsonResponse(200, handleApiCommand(doc.object()));
    }

    if (request.method == "POST" && cleanPath == "/browse") {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
            QJsonObject out;
            out.insert("error", "Invalid JSON");
            return jsonResponse(400, out);
        }

        const QJsonObject payload = doc.object();
        const QString by = payload.value("by").toString("artist").trimmed().toLower();
        const QString letter = payload.value("letter").toString("A").trimmed().toUpper();
        const int limit = std::clamp(payload.value("limit").toInt(200), 1, 1000);

        if ((by != "artist" && by != "title") || letter.size() != 1 || letter.at(0) < QChar('A') ||
            letter.at(0) > QChar('Z')) {
            QJsonObject out;
            out.insert("error", "Invalid browse params");
            return jsonResponse(400, out);
        }

        const QString primary = (by == "title") ? "title" : "artist";
        const QString secondary = (by == "title") ? "artist" : "title";

        QSqlQuery query;
        query.prepare(QString("SELECT songid, artist, title, COALESCE(duration, 0) FROM dbsongs "
                              "WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!' "
                              "AND upper(trim(%1)) LIKE :prefix "
                              "ORDER BY upper(%1), upper(%2) LIMIT :limit").arg(primary, secondary));
        query.bindValue(":prefix", letter + "%");
        query.bindValue(":limit", limit);

        QJsonArray songs;
        if (query.exec()) {
            while (query.next()) {
                QJsonObject song;
                song.insert("song_id", query.value(0).toInt());
                song.insert("artist", query.value(1).toString());
                song.insert("title", query.value(2).toString());
                song.insert("duration_seconds", std::max(1, query.value(3).toInt() / 1000));
                songs.append(song);
            }
        }

        QJsonObject out;
        out.insert("songs", songs);
        return jsonResponse(200, out);
    }

    if (request.method == "POST" && cleanPath.startsWith("/local/")) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(request.body, &parseError);
        const QJsonObject payload = (parseError.error == QJsonParseError::NoError && doc.isObject())
            ? doc.object()
            : QJsonObject();
        if (!request.body.trimmed().isEmpty() && (parseError.error != QJsonParseError::NoError || !doc.isObject())) {
            return jsonResponse(400, QJsonObject{{"ok", false}, {"error", "Invalid JSON"}});
        }
        return handleLocalApiPost(cleanPath, payload);
    }

    QJsonObject out;
    out.insert("error", "true");
    out.insert("errorString", "Not Found");
    return jsonResponse(404, out);
}

QJsonObject OpenKJEmbeddedApi::handleApiCommand(const QJsonObject &payload)
{
    const QString command = payload.value("command").toString();
    if (command.isEmpty()) {
        QJsonObject out;
        out.insert("error", "true");
        out.insert("errorString", "Missing command");
        return out;
    }

    if (command == "connectionTest") {
        QJsonObject out;
        out.insert("command", command);
        out.insert("connection", "ok");
        return out;
    }

    if (command == "getSerial") {
        QJsonObject out;
        out.insert("command", command);
        out.insert("serial", m_settings.embeddedApiSerial());
        out.insert("error", "false");
        return out;
    }

    if (command == "venueAccepting" || command == "getAccepting") {
        QJsonObject out;
        out.insert("command", command);
        out.insert("accepting", isAccepting());
        out.insert("venue_id", 0);
        return out;
    }

    if (command == "setAccepting") {
        return commandSetAccepting(payload);
    }

    if (command == "adminAction") {
        return commandAdminAction(payload);
    }

    if (command == "getVenues") {
        QJsonObject venue;
        venue.insert("venue_id", 0);
        venue.insert("accepting", isAccepting());
        venue.insert("name", "OpenKJ");
        venue.insert("url_name", "none");

        QJsonArray venues;
        venues.append(venue);

        QJsonObject out;
        out.insert("command", command);
        out.insert("error", "false");
        out.insert("venues", venues);
        return out;
    }

    if (command == "search") {
        return commandSearch(payload);
    }

    if (command == "submitRequest") {
        return commandSubmitRequest(payload);
    }

    if (command == "getRequests") {
        return commandGetRequests();
    }

    if (command == "getCapabilities") {
        return buildCapabilities();
    }

    if (command == "getEventSettings") {
        return buildEventSettings();
    }

    if (command == "setEventSettings") {
        if (!isValidAdminSession(payload.value("token").toString().trimmed())) {
            return QJsonObject{{"ok", false}, {"error", "Admin authentication required"}};
        }

        const QString appName = payload.value("appName").toString().trimmed();
        const QString tagline = payload.value("tagline").toString().trimmed();
        QSqlQuery query;
        query.prepare("INSERT INTO local_event_settings (settings_id, app_name, tagline) VALUES (1, :app_name, :tagline) "
                      "ON CONFLICT(settings_id) DO UPDATE SET app_name = excluded.app_name, tagline = excluded.tagline");
        query.bindValue(":app_name", appName.isEmpty() ? "OpenKJ" : appName);
        query.bindValue(":tagline", tagline);
        if (!query.exec()) {
            return QJsonObject{{"ok", false}, {"error", query.lastError().text()}};
        }
        return buildEventSettings();
    }

    if (command == "deleteRequest") {
        return commandDeleteRequest(payload);
    }

    if (command == "clearRequests") {
        QSqlQuery query;
        query.exec("DELETE FROM queuesongs WHERE played = 0");
        nextSerial();

        QJsonObject out;
        out.insert("command", command);
        out.insert("error", "false");
        out.insert("serial", m_settings.embeddedApiSerial());
        return out;
    }

    if (command == "addSongs" || command == "clearDatabase") {
        // Not needed when OpenKJ serves its own catalog. Keep compatibility with success response.
        QJsonObject out;
        out.insert("command", command);
        out.insert("error", "false");
        out.insert("serial", m_settings.embeddedApiSerial());
        return out;
    }

    QJsonObject out;
    out.insert("command", command);
    out.insert("error", "true");
    out.insert("errorString", QString("Unsupported command: %1").arg(command));
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandSearch(const QJsonObject &payload)
{
    const QString searchString = payload.value("searchString").toString().trimmed();
    const int limit = std::clamp(payload.value("limit").toInt(100), 1, 500);

    QStringList terms = searchString.split(' ', Qt::SkipEmptyParts);
    QSqlQuery query;

    QString sql = "SELECT songid, artist, title, COALESCE(duration, 0) FROM dbsongs "
                  "WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!' ";
    if (!terms.isEmpty()) {
        for (int i = 0; i < terms.size(); ++i) {
            sql += QString("AND lower(artist || ' ' || title) LIKE :term%1 ").arg(i);
        }
    }
    sql += QString("ORDER BY upper(artist), upper(title) LIMIT %1").arg(limit);

    query.prepare(sql);
    for (int i = 0; i < terms.size(); ++i) {
        query.bindValue(QString(":term%1").arg(i), QString("%%1%").arg(terms.at(i).toLower()));
    }

    QJsonArray songs;
    if (query.exec()) {
        while (query.next()) {
            const QString artist = query.value(1).toString();
            const QString title = query.value(2).toString();
            const QString combined = (artist + " " + title).toLower();
            if (combined.contains("wvocal") || combined.contains("w-vocal") || combined.contains("vocals")) {
                continue;
            }

            QJsonObject song;
            song.insert("song_id", query.value(0).toInt());
            song.insert("artist", artist);
            song.insert("title", title);
            song.insert("duration_seconds", std::max(1, query.value(3).toInt() / 1000));
            songs.append(song);
        }
    }

    QJsonObject out;
    out.insert("command", "search");
    out.insert("songs", songs);
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandSubmitRequest(const QJsonObject &payload)
{
    if (!isAccepting()) {
        QJsonObject out;
        out.insert("command", "submitRequest");
        out.insert("error", "true");
        out.insert("errorString", "Requests not accepted");
        return out;
    }

    bool ok = false;
    int songId = payload.value("songId").toVariant().toInt(&ok);
    if (!ok && payload.value("songId").isString()) {
        songId = payload.value("songId").toString().toInt(&ok);
    }
    const QString singerName = payload.value("singerName").toString().trimmed();

    if (!ok || songId < 1 || singerName.isEmpty()) {
        QJsonObject out;
        out.insert("command", "submitRequest");
        out.insert("error", "true");
        out.insert("errorString", "Missing songId or singerName");
        return out;
    }

    int singerId = -1;
    if (m_rotationModel.singerExists(singerName)) {
        singerId = m_rotationModel.getSingerByName(singerName).id;
    } else {
        singerId = m_rotationModel.singerAdd(singerName, m_settings.lastSingerAddPositionType());
    }

    QSqlQuery dupeCheck;
    dupeCheck.prepare("SELECT 1 FROM queuesongs WHERE singer = :singerId AND song = :songId AND played = 0 LIMIT 1");
    dupeCheck.bindValue(":singerId", singerId);
    dupeCheck.bindValue(":songId", songId);
    if (dupeCheck.exec() && dupeCheck.next()) {
        QJsonObject out;
        out.insert("command", "submitRequest");
        out.insert("error", "true");
        out.insert("errorString", "This song is already in your queue");
        return out;
    }

    m_queueModel.songAddSlot(songId, singerId, 0);
    nextSerial();
    emit songSubmitted();

    QJsonObject out;
    out.insert("command", "submitRequest");
    out.insert("error", "false");
    out.insert("success", true);
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandGetRequests()
{
    const int nowSingerId = currentSingerId();

    QJsonObject nowPlaying;
    int nowPlayingId = -1;
    if (nowSingerId >= 0) {
        QSqlQuery nowQuery;
        nowQuery.prepare(
            "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0) "
            "FROM queuesongs qs "
            "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
            "INNER JOIN dbsongs d ON d.songid = qs.song "
            "WHERE qs.singer = :singerId AND qs.played = 1 "
            "ORDER BY qs.position DESC LIMIT 1");
        nowQuery.bindValue(":singerId", nowSingerId);
        if (nowQuery.exec() && nowQuery.next()) {
            nowPlayingId = nowQuery.value(0).toInt();
            nowPlaying.insert("request_id", nowPlayingId);
            nowPlaying.insert("singer", nowQuery.value(1).toString());
            nowPlaying.insert("song_id", nowQuery.value(2).toInt());
            nowPlaying.insert("artist", nowQuery.value(3).toString());
            nowPlaying.insert("title", nowQuery.value(4).toString());
            nowPlaying.insert("duration_seconds", std::max(1, nowQuery.value(5).toInt() / 1000));
            nowPlaying.insert("request_time", QDateTime::currentSecsSinceEpoch());
        }
    }

    QSqlQuery query;
    query.prepare(
        "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0), qs.keychg "
        "FROM queuesongs qs "
        "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
        "INNER JOIN dbsongs d ON d.songid = qs.song "
        "WHERE qs.played = 0 AND qs.singer != :nowSinger "
        "ORDER BY rs.position ASC, qs.position ASC");
    query.bindValue(":nowSinger", nowSingerId);

    QJsonArray requests;
    QJsonArray upNext;
    if (query.exec()) {
        while (query.next()) {
            QJsonObject request;
            request.insert("request_id", query.value(0).toInt());
            request.insert("singer", query.value(1).toString());
            request.insert("song_id", query.value(2).toInt());
            request.insert("artist", query.value(3).toString());
            request.insert("title", query.value(4).toString());
            request.insert("duration_seconds", std::max(1, query.value(5).toInt() / 1000));
            request.insert("key_change", query.value(6).toInt());
            request.insert("request_time", QDateTime::currentSecsSinceEpoch());
            requests.append(request);
            upNext.append(request);
        }
    }

    // Include the current singer's remaining queued songs (beyond the one currently playing)
    // so their personal queue is visible in the web UI.
    if (nowSingerId >= 0) {
        QSqlQuery remainingQuery;
        remainingQuery.prepare(
            "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0), qs.keychg "
            "FROM queuesongs qs "
            "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
            "INNER JOIN dbsongs d ON d.songid = qs.song "
            "WHERE qs.played = 0 AND qs.singer = :singerId AND qs.qsongid != :nowPlayingId "
            "ORDER BY qs.position ASC");
        remainingQuery.bindValue(":singerId", nowSingerId);
        remainingQuery.bindValue(":nowPlayingId", nowPlayingId);
        if (remainingQuery.exec()) {
            while (remainingQuery.next()) {
                QJsonObject request;
                request.insert("request_id", remainingQuery.value(0).toInt());
                request.insert("singer", remainingQuery.value(1).toString());
                request.insert("song_id", remainingQuery.value(2).toInt());
                request.insert("artist", remainingQuery.value(3).toString());
                request.insert("title", remainingQuery.value(4).toString());
                request.insert("duration_seconds", std::max(1, remainingQuery.value(5).toInt() / 1000));
                request.insert("key_change", remainingQuery.value(6).toInt());
                request.insert("request_time", QDateTime::currentSecsSinceEpoch());
                requests.append(request);
                upNext.append(request);
            }
        }
    }

    QJsonArray recentlyPlayed;
    if (nowSingerId >= 0) {
        QSqlQuery playedQuery;
        playedQuery.prepare(
            "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0) "
            "FROM queuesongs qs "
            "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
            "INNER JOIN dbsongs d ON d.songid = qs.song "
            "WHERE qs.played = 1 "
            "ORDER BY qs.qsongid DESC LIMIT 30");
        if (playedQuery.exec()) {
            while (playedQuery.next()) {
                QJsonObject item;
                item.insert("request_id", playedQuery.value(0).toInt());
                item.insert("singer", playedQuery.value(1).toString());
                item.insert("song_id", playedQuery.value(2).toInt());
                item.insert("artist", playedQuery.value(3).toString());
                item.insert("title", playedQuery.value(4).toString());
                item.insert("duration_seconds", std::max(1, playedQuery.value(5).toInt() / 1000));
                item.insert("played_at", QDateTime::currentSecsSinceEpoch());
                recentlyPlayed.append(item);
            }
        }
    }

    QJsonObject out;
    out.insert("command", "getRequests");
    out.insert("error", "false");
    out.insert("serial", m_settings.embeddedApiSerial());
    out.insert("requests", requests);
    out.insert("now_playing", nowPlaying);
    out.insert("up_next", upNext);
    out.insert("recently_played", recentlyPlayed);
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandDeleteRequest(const QJsonObject &payload)
{
    bool ok = false;
    const int requestId = payload.value("request_id").toVariant().toInt(&ok);
    if (!ok) {
        QJsonObject out;
        out.insert("command", "deleteRequest");
        out.insert("error", "true");
        out.insert("errorString", "Missing request_id");
        return out;
    }

    removeQueueSongById(requestId);
    nextSerial();

    QJsonObject out;
    out.insert("command", "deleteRequest");
    out.insert("error", "false");
    out.insert("serial", m_settings.embeddedApiSerial());
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandSetAccepting(const QJsonObject &payload)
{
    bool accepting = payload.value("accepting").toBool(true);
    if (payload.value("accepting").isDouble()) {
        accepting = payload.value("accepting").toInt() != 0;
    }

    setAccepting(accepting);
    nextSerial();

    QJsonObject out;
    out.insert("command", "setAccepting");
    out.insert("error", "false");
    out.insert("venue_id", 0);
    out.insert("accepting", accepting);
    out.insert("serial", m_settings.embeddedApiSerial());
    return out;
}

QJsonObject OpenKJEmbeddedApi::commandAdminAction(const QJsonObject &payload)
{
    const QString action = payload.value("action").toString().trimmed();
    if (action.isEmpty()) {
        return QJsonObject{{"command", "adminAction"}, {"error", "true"}, {"errorString", "Missing action"}};
    }

    bool success = false;
    if (action == "remove_song") {
        success = removeQueueSongById(payload.value("request_id").toVariant().toInt());
    } else if (action == "move_song_up") {
        success = moveQueueSongByOffset(payload.value("request_id").toVariant().toInt(), -1);
    } else if (action == "move_song_down") {
        success = moveQueueSongByOffset(payload.value("request_id").toVariant().toInt(), 1);
    } else if (action == "set_key_change") {
        int requestId = payload.value("request_id").toVariant().toInt();
        if (requestId <= 0) {
            QSqlQuery query;
            query.prepare("SELECT qsongid FROM queuesongs WHERE singer = :singer AND played = 0 ORDER BY position LIMIT 1");
            query.bindValue(":singer", currentSingerId());
            if (query.exec() && query.next()) {
                requestId = query.value(0).toInt();
            }
        }
        success = requestId > 0 && setQueueSongKey(requestId, payload.value("value").toInt());
    } else if (action == "skip_song") {
        const int singerId = currentSingerId();
        success = false;
        if (singerId >= 0) {
            QSqlQuery query;
            query.prepare("SELECT qsongid FROM queuesongs WHERE singer = :singer AND played = 0 ORDER BY position LIMIT 1");
            query.bindValue(":singer", singerId);
            if (query.exec() && query.next()) {
                QSqlQuery upd;
                upd.prepare("UPDATE queuesongs SET played = 1 WHERE qsongid = :id");
                upd.bindValue(":id", query.value(0).toInt());
                success = upd.exec();
            }
        }
    } else if (action == "rotation_next") {
        const int nextId = nextSingerId(1);
        if (nextId >= 0) {
            m_settings.setCurrentRotationPosition(nextId);
            success = true;
        }
    } else if (action == "rotation_previous") {
        const int prevId = nextSingerId(-1);
        if (prevId >= 0) {
            m_settings.setCurrentRotationPosition(prevId);
            success = true;
        }
    } else if (action == "set_volume") {
        m_settings.setAudioVolume(std::clamp(payload.value("value").toInt(60), 0, 100));
        success = true;
    } else if (action == "lock_queue") {
        setAccepting(false);
        success = true;
    } else if (action == "unlock_queue") {
        setAccepting(true);
        success = true;
    }

    if (!success) {
        return QJsonObject{
            {"command", "adminAction"},
            {"error", "true"},
            {"errorString", QString("Action failed or unsupported: %1").arg(action)}
        };
    }

    nextSerial();
    return QJsonObject{{"command", "adminAction"}, {"error", "false"}, {"serial", m_settings.embeddedApiSerial()}};
}

QByteArray OpenKJEmbeddedApi::handleLocalApiGet(const QString &path, const QUrlQuery &query)
{
    if (path == "/local/queue") {
        return jsonResponse(200, buildQueueResponse());
    }
    if (path == "/local/songs") {
        const QString q = query.queryItemValue("q");
        const int limit = std::clamp(query.queryItemValue("limit").toInt(), 1, 500);
        return jsonResponse(200, commandSearch(QJsonObject{{"searchString", q}, {"limit", limit}}));
    }
    if (path == "/local/user/me") {
        return jsonResponse(200, currentLocalUser(query));
    }
    if (path == "/local/auth/me") {
        return jsonResponse(200, currentAdmin(query));
    }
    if (path == "/local/capabilities") {
        return jsonResponse(200, buildCapabilities());
    }
    if (path == "/local/event-settings") {
        return jsonResponse(200, buildEventSettings());
    }

    return jsonResponse(404, QJsonObject{{"ok", false}, {"error", "Not Found"}});
}

QByteArray OpenKJEmbeddedApi::handleLocalApiPost(const QString &path, const QJsonObject &payload)
{
    if (path == "/local/user/register") {
        return jsonResponse(200, registerLocalUser(payload));
    }
    if (path == "/local/user/login") {
        return jsonResponse(200, loginLocalUser(payload));
    }
    if (path == "/local/user/logout") {
        return jsonResponse(200, logoutLocalUser(payload));
    }
    if (path == "/local/user/profile/name") {
        return jsonResponse(200, updateLocalUsername(payload));
    }
    if (path == "/local/user/profile/password") {
        return jsonResponse(200, updateLocalPassword(payload));
    }
    if (path == "/local/request") {
        return jsonResponse(200, requestSongFromLocalUser(payload));
    }
    if (path == "/local/request/remove") {
        return jsonResponse(200, removeOwnRequest(payload));
    }
    if (path == "/local/auth/login") {
        return jsonResponse(200, loginAdmin(payload));
    }
    if (path == "/local/auth/logout") {
        return jsonResponse(200, logoutAdmin(payload));
    }
    if (path == "/local/admin/action") {
        return jsonResponse(200, runAdminActionRest(payload));
    }
    if (path == "/local/event-settings") {
        const QString appName = payload.value("appName").toString().trimmed();
        const QString tagline = payload.value("tagline").toString().trimmed();
        QSqlQuery query;
        query.prepare("INSERT INTO local_event_settings (settings_id, app_name, tagline) VALUES (1, :app_name, :tagline) "
                      "ON CONFLICT(settings_id) DO UPDATE SET app_name = excluded.app_name, tagline = excluded.tagline");
        query.bindValue(":app_name", appName.isEmpty() ? "OpenKJ" : appName);
        query.bindValue(":tagline", tagline);
        const bool ok = query.exec();
        return jsonResponse(ok ? 200 : 400,
                            ok ? buildEventSettings()
                               : QJsonObject{{"ok", false}, {"error", query.lastError().text()}});
    }

    return jsonResponse(404, QJsonObject{{"ok", false}, {"error", "Not Found"}});
}

void OpenKJEmbeddedApi::parseRequestPath(const QString &path, QString &cleanPath, QUrlQuery &query)
{
    const QUrl url(path);
    cleanPath = url.path();
    query = QUrlQuery(url);
}

QByteArray OpenKJEmbeddedApi::jsonResponse(const int statusCode, const QJsonObject &object) const
{
    const QByteArray body = QJsonDocument(object).toJson(QJsonDocument::Compact);
    QByteArray response;
    response.append("HTTP/1.1 ");
    response.append(QByteArray::number(statusCode));
    response.append(" ");
    response.append(statusText(statusCode));
    response.append("\r\n");
    response.append("Content-Type: application/json; charset=utf-8\r\n");
    response.append("Content-Length: ");
    response.append(QByteArray::number(body.size()));
    response.append("\r\n");
    response.append("Connection: close\r\n\r\n");
    response.append(body);
    return response;
}

QByteArray OpenKJEmbeddedApi::statusText(const int code)
{
    switch (code) {
        case 200:
            return "OK";
        case 400:
            return "Bad Request";
        case 401:
            return "Unauthorized";
        case 404:
            return "Not Found";
        case 500:
            return "Internal Server Error";
        default:
            return "Error";
    }
}

QHash<QString, QString> OpenKJEmbeddedApi::parseHeaders(const QList<QByteArray> &headerLines)
{
    QHash<QString, QString> headers;
    for (const auto &lineRaw : headerLines) {
        const QByteArray line = lineRaw.trimmed();
        const int sep = line.indexOf(':');
        if (sep <= 0) {
            continue;
        }

        const QString key = QString::fromUtf8(line.left(sep)).trimmed().toLower();
        const QString value = QString::fromUtf8(line.mid(sep + 1)).trimmed();
        headers.insert(key, value);
    }

    return headers;
}

int OpenKJEmbeddedApi::nextSerial()
{
    int serial = QRandomGenerator::global()->bounded(100000);
    if (serial == m_settings.embeddedApiSerial()) {
        serial = (serial + 1) % 100000;
    }
    m_settings.setEmbeddedApiSerial(serial);
    return serial;
}

bool OpenKJEmbeddedApi::isAccepting() const
{
    return m_settings.embeddedApiAccepting();
}

void OpenKJEmbeddedApi::setAccepting(const bool accepting)
{
    m_settings.setEmbeddedApiAccepting(accepting);
}

bool OpenKJEmbeddedApi::removeQueueSongById(const int qsongId)
{
    QSqlQuery find;
    find.prepare("SELECT singer FROM queuesongs WHERE qsongid = :id LIMIT 1");
    find.bindValue(":id", qsongId);
    int singerId = -1;
    if (find.exec() && find.next()) {
        singerId = find.value(0).toInt();
    }

    QSqlQuery del;
    del.prepare("DELETE FROM queuesongs WHERE qsongid = :id");
    del.bindValue(":id", qsongId);
    const bool removed = del.exec();

    if (removed) {
        QSqlQuery ownerCleanup;
        ownerCleanup.prepare("DELETE FROM local_request_owners WHERE request_id = :id");
        ownerCleanup.bindValue(":id", qsongId);
        ownerCleanup.exec();
    }

    if (removed && singerId >= 0) {
        normalizeSingerQueuePositions(singerId);
        if (m_queueModel.getSingerId() == singerId) {
            m_queueModel.loadSinger(singerId);
        }
    }

    return removed;
}

void OpenKJEmbeddedApi::normalizeSingerQueuePositions(const int singerId)
{
    QSqlQuery query;
    query.prepare("SELECT qsongid FROM queuesongs WHERE singer = :singerId ORDER BY position ASC, qsongid ASC");
    query.bindValue(":singerId", singerId);
    if (!query.exec()) {
        return;
    }

    QList<int> ids;
    while (query.next()) {
        ids.append(query.value(0).toInt());
    }

    QSqlQuery tx;
    tx.exec("BEGIN TRANSACTION");
    QSqlQuery upd;
    upd.prepare("UPDATE queuesongs SET position = :position WHERE qsongid = :id");
    for (int i = 0; i < ids.size(); ++i) {
        upd.bindValue(":position", i);
        upd.bindValue(":id", ids.at(i));
        upd.exec();
    }
    tx.exec("COMMIT");
}

bool OpenKJEmbeddedApi::moveQueueSongByOffset(const int qsongId, const int offset)
{
    QSqlQuery find;
    find.prepare("SELECT singer, position FROM queuesongs WHERE qsongid = :id LIMIT 1");
    find.bindValue(":id", qsongId);
    if (!find.exec() || !find.next()) {
        return false;
    }

    const int singerId = find.value(0).toInt();
    const int oldPos = find.value(1).toInt();
    const int newPos = oldPos + offset;
    if (newPos < 0) {
        return false;
    }

    QSqlQuery countQ;
    countQ.prepare("SELECT COUNT(1) FROM queuesongs WHERE singer = :singer");
    countQ.bindValue(":singer", singerId);
    int count = 0;
    if (countQ.exec() && countQ.next()) {
        count = countQ.value(0).toInt();
    }
    if (newPos >= count) {
        return false;
    }

    QSqlQuery tx;
    tx.exec("BEGIN TRANSACTION");
    if (offset < 0) {
        QSqlQuery shift;
        shift.prepare("UPDATE queuesongs SET position = position + 1 WHERE singer = :singer AND position >= :newPos AND position < :oldPos");
        shift.bindValue(":singer", singerId);
        shift.bindValue(":newPos", newPos);
        shift.bindValue(":oldPos", oldPos);
        shift.exec();
    } else {
        QSqlQuery shift;
        shift.prepare("UPDATE queuesongs SET position = position - 1 WHERE singer = :singer AND position <= :newPos AND position > :oldPos");
        shift.bindValue(":singer", singerId);
        shift.bindValue(":newPos", newPos);
        shift.bindValue(":oldPos", oldPos);
        shift.exec();
    }

    QSqlQuery upd;
    upd.prepare("UPDATE queuesongs SET position = :newPos WHERE qsongid = :id");
    upd.bindValue(":newPos", newPos);
    upd.bindValue(":id", qsongId);
    const bool ok = upd.exec();
    tx.exec(ok ? "COMMIT" : "ROLLBACK");

    if (ok && m_queueModel.getSingerId() == singerId) {
        m_queueModel.loadSinger(singerId);
    }
    return ok;
}

bool OpenKJEmbeddedApi::setQueueSongKey(const int qsongId, const int keyChange)
{
    QSqlQuery query;
    query.prepare("UPDATE queuesongs SET keychg = :key WHERE qsongid = :id");
    query.bindValue(":key", std::clamp(keyChange, -12, 12));
    query.bindValue(":id", qsongId);
    return query.exec();
}

int OpenKJEmbeddedApi::currentSingerId() const
{
    return m_settings.currentRotationPosition();
}

int OpenKJEmbeddedApi::nextSingerId(const int direction) const
{
    QSqlQuery query;
    query.exec("SELECT singerid, position FROM rotationsingers ORDER BY position ASC");

    QList<QPair<int, int>> singers;
    while (query.next()) {
        singers.append({query.value(0).toInt(), query.value(1).toInt()});
    }
    if (singers.isEmpty()) {
        return -1;
    }

    const int curId = currentSingerId();
    int idx = 0;
    for (int i = 0; i < singers.size(); ++i) {
        if (singers.at(i).first == curId) {
            idx = i;
            break;
        }
    }

    const int nextIdx = (idx + direction + singers.size()) % singers.size();
    return singers.at(nextIdx).first;
}

QJsonObject OpenKJEmbeddedApi::buildQueueResponse()
{
    QJsonObject queue = commandGetRequests();
    queue.insert("ok", true);
    queue.insert("queueLocked", !isAccepting());
    return queue;
}

QJsonObject OpenKJEmbeddedApi::buildCapabilities() const
{
    return QJsonObject{
        {"ok", true},
        {"mode", m_settings.appModeName()},
        {"supportsRemoveOwnSong", true},
        {"supportsLocalUsers", true},
        {"supportsAdminAuth", true},
        {"supportsLocalMode", true}
    };
}

QJsonObject OpenKJEmbeddedApi::buildEventSettings() const
{
    QSqlQuery query;
    query.exec("SELECT app_name, tagline FROM local_event_settings WHERE settings_id = 1");
    QString appName{"OpenKJ"};
    QString tagline;
    if (query.next()) {
        appName = query.value(0).toString().trimmed();
        tagline = query.value(1).toString().trimmed();
        if (appName.isEmpty()) {
            appName = "OpenKJ";
        }
    }

    return QJsonObject{{"ok", true}, {"appName", appName}, {"tagline", tagline}};
}

bool OpenKJEmbeddedApi::ensureLocalModeSchema()
{
    QSqlQuery query;
    const QStringList statements{
        "CREATE TABLE IF NOT EXISTS local_users ("
        "username_normalized TEXT PRIMARY KEY,"
        "username TEXT NOT NULL,"
        "password_hash BLOB NOT NULL,"
        "created_at TEXT NOT NULL DEFAULT CURRENT_TIMESTAMP)",
        "CREATE TABLE IF NOT EXISTS local_user_sessions ("
        "token TEXT PRIMARY KEY,"
        "username_normalized TEXT NOT NULL,"
        "expires_at INTEGER NOT NULL,"
        "FOREIGN KEY(username_normalized) REFERENCES local_users(username_normalized) ON DELETE CASCADE)",
        "CREATE TABLE IF NOT EXISTS local_admin_sessions ("
        "token TEXT PRIMARY KEY,"
        "expires_at INTEGER NOT NULL)",
        "CREATE TABLE IF NOT EXISTS local_request_owners ("
        "request_id INTEGER PRIMARY KEY,"
        "username_normalized TEXT NOT NULL,"
        "FOREIGN KEY(username_normalized) REFERENCES local_users(username_normalized) ON DELETE CASCADE)",
        "CREATE TABLE IF NOT EXISTS local_event_settings ("
        "settings_id INTEGER PRIMARY KEY CHECK(settings_id = 1),"
        "app_name TEXT NOT NULL DEFAULT 'OpenKJ',"
        "tagline TEXT NOT NULL DEFAULT '')"
    };

    for (const auto &statement : statements) {
        if (!query.exec(statement)) {
            return false;
        }
    }

    query.exec("INSERT OR IGNORE INTO local_event_settings (settings_id, app_name, tagline) VALUES (1, 'OpenKJ', '')");
    query.exec("DELETE FROM local_user_sessions WHERE expires_at <= strftime('%s','now')");
    query.exec("DELETE FROM local_admin_sessions WHERE expires_at <= strftime('%s','now')");
    return true;
}

QString OpenKJEmbeddedApi::normalizeUsername(const QString &username)
{
    return username.trimmed().toLower();
}

QByteArray OpenKJEmbeddedApi::hashPassword(const QString &password)
{
    return QCryptographicHash::hash(password.trimmed().toUtf8(), QCryptographicHash::Sha256);
}

QString OpenKJEmbeddedApi::createUserSession(const QString &normalizedUsername)
{
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 expiresAt = QDateTime::currentSecsSinceEpoch() + (60 * 60 * 24 * 30);
    QSqlQuery query;
    query.prepare("INSERT INTO local_user_sessions (token, username_normalized, expires_at) VALUES (:token, :username, :expires_at)");
    query.bindValue(":token", token);
    query.bindValue(":username", normalizedUsername);
    query.bindValue(":expires_at", expiresAt);
    query.exec();
    return token;
}

QString OpenKJEmbeddedApi::createAdminSession()
{
    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const qint64 expiresAt = QDateTime::currentSecsSinceEpoch() + (60 * 60 * 24 * 7);
    QSqlQuery query;
    query.prepare("INSERT INTO local_admin_sessions (token, expires_at) VALUES (:token, :expires_at)");
    query.bindValue(":token", token);
    query.bindValue(":expires_at", expiresAt);
    query.exec();
    return token;
}

bool OpenKJEmbeddedApi::isValidUserSession(const QString &token, QString *normalizedUsername, QString *username)
{
    if (token.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query;
    query.prepare("SELECT u.username_normalized, u.username "
                  "FROM local_user_sessions s "
                  "JOIN local_users u ON u.username_normalized = s.username_normalized "
                  "WHERE s.token = :token AND s.expires_at > :now");
    query.bindValue(":token", token.trimmed());
    query.bindValue(":now", QDateTime::currentSecsSinceEpoch());
    if (!query.exec() || !query.next()) {
        return false;
    }

    if (normalizedUsername) {
        *normalizedUsername = query.value(0).toString();
    }
    if (username) {
        *username = query.value(1).toString();
    }
    return true;
}

bool OpenKJEmbeddedApi::isValidAdminSession(const QString &token)
{
    if (token.trimmed().isEmpty()) {
        return false;
    }

    QSqlQuery query;
    query.prepare("SELECT token FROM local_admin_sessions WHERE token = :token AND expires_at > :now");
    query.bindValue(":token", token.trimmed());
    query.bindValue(":now", QDateTime::currentSecsSinceEpoch());
    return query.exec() && query.next();
}

QJsonObject OpenKJEmbeddedApi::registerLocalUser(const QJsonObject &payload)
{
    ensureLocalModeSchema();
    const QString username = payload.value("username").toString().trimmed();
    const QString password = payload.value("password").toString();
    const QString normalized = normalizeUsername(username);

    if (username.size() < 2 || username.size() > 24 || password.size() < 4) {
        return QJsonObject{{"ok", false}, {"error", "Username or password does not meet requirements"}};
    }

    QSqlQuery exists;
    exists.prepare("SELECT username_normalized FROM local_users WHERE username_normalized = :username");
    exists.bindValue(":username", normalized);
    if (exists.exec() && exists.next()) {
        return QJsonObject{{"ok", false}, {"error", "Username is already registered"}};
    }

    QSqlQuery insert;
    insert.prepare("INSERT INTO local_users (username_normalized, username, password_hash) "
                   "VALUES (:username_normalized, :username, :password_hash)");
    insert.bindValue(":username_normalized", normalized);
    insert.bindValue(":username", username);
    insert.bindValue(":password_hash", hashPassword(password));
    if (!insert.exec()) {
        return QJsonObject{{"ok", false}, {"error", insert.lastError().text()}};
    }

    return QJsonObject{{"ok", true}, {"username", username}, {"token", createUserSession(normalized)}};
}

QJsonObject OpenKJEmbeddedApi::loginLocalUser(const QJsonObject &payload)
{
    ensureLocalModeSchema();
    const QString username = payload.value("username").toString().trimmed();
    const QString password = payload.value("password").toString();
    const QString normalized = normalizeUsername(username);

    QSqlQuery query;
    query.prepare("SELECT username, password_hash FROM local_users WHERE username_normalized = :username");
    query.bindValue(":username", normalized);
    if (!query.exec() || !query.next()) {
        return QJsonObject{{"ok", false}, {"error", "Invalid username or password"}};
    }

    const QByteArray expected = query.value(1).toByteArray();
    if (expected != hashPassword(password)) {
        return QJsonObject{{"ok", false}, {"error", "Invalid username or password"}};
    }

    return QJsonObject{{"ok", true}, {"username", query.value(0).toString()}, {"token", createUserSession(normalized)}};
}

QJsonObject OpenKJEmbeddedApi::logoutLocalUser(const QJsonObject &payload)
{
    return QJsonObject{{"ok", removeUserSessionToken(payload.value("token").toString().trimmed())}};
}

QJsonObject OpenKJEmbeddedApi::currentLocalUser(const QUrlQuery &query)
{
    QString normalized;
    QString username;
    const bool authenticated = isValidUserSession(query.queryItemValue("token"), &normalized, &username);
    Q_UNUSED(normalized)
    return QJsonObject{{"ok", true}, {"authenticated", authenticated}, {"username", authenticated ? username : QString()}};
}

QJsonObject OpenKJEmbeddedApi::updateLocalUsername(const QJsonObject &payload)
{
    QString currentNormalized;
    QString currentUsername;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &currentNormalized, &currentUsername)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    const QString nextUsername = payload.value("username").toString().trimmed();
    const QString nextNormalized = normalizeUsername(nextUsername);
    if (nextUsername.size() < 2 || nextUsername.size() > 24) {
        return QJsonObject{{"ok", false}, {"error", "Invalid username"}};
    }

    QSqlQuery exists;
    exists.prepare("SELECT username_normalized FROM local_users WHERE username_normalized = :username");
    exists.bindValue(":username", nextNormalized);
    if (exists.exec() && exists.next() && exists.value(0).toString() != currentNormalized) {
        return QJsonObject{{"ok", false}, {"error", "Username is already registered"}};
    }

    QSqlQuery tx;
    tx.exec("BEGIN TRANSACTION");
    QSqlQuery updateUser;
    updateUser.prepare("UPDATE local_users SET username_normalized = :next_normalized, username = :username "
                       "WHERE username_normalized = :current_normalized");
    updateUser.bindValue(":next_normalized", nextNormalized);
    updateUser.bindValue(":username", nextUsername);
    updateUser.bindValue(":current_normalized", currentNormalized);
    const bool userOk = updateUser.exec();

    QSqlQuery updateSessions;
    updateSessions.prepare("UPDATE local_user_sessions SET username_normalized = :next_normalized "
                           "WHERE username_normalized = :current_normalized");
    updateSessions.bindValue(":next_normalized", nextNormalized);
    updateSessions.bindValue(":current_normalized", currentNormalized);
    const bool sessionsOk = updateSessions.exec();

    QSqlQuery updateRequests;
    updateRequests.prepare("UPDATE local_request_owners SET username_normalized = :next_normalized "
                           "WHERE username_normalized = :current_normalized");
    updateRequests.bindValue(":next_normalized", nextNormalized);
    updateRequests.bindValue(":current_normalized", currentNormalized);
    const bool requestsOk = updateRequests.exec();

    tx.exec(userOk && sessionsOk && requestsOk ? "COMMIT" : "ROLLBACK");
    if (!(userOk && sessionsOk && requestsOk)) {
        return QJsonObject{{"ok", false}, {"error", "Could not update username"}};
    }

    return QJsonObject{{"ok", true}, {"username", nextUsername}};
}

QJsonObject OpenKJEmbeddedApi::updateLocalPassword(const QJsonObject &payload)
{
    QString currentNormalized;
    QString currentUsername;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &currentNormalized, &currentUsername)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    QSqlQuery query;
    query.prepare("SELECT password_hash FROM local_users WHERE username_normalized = :username");
    query.bindValue(":username", currentNormalized);
    if (!query.exec() || !query.next()) {
        return QJsonObject{{"ok", false}, {"error", "Unknown user"}};
    }

    if (query.value(0).toByteArray() != hashPassword(payload.value("currentPassword").toString())) {
        return QJsonObject{{"ok", false}, {"error", "Current password is incorrect"}};
    }

    const QString nextPassword = payload.value("newPassword").toString();
    if (nextPassword.size() < 4) {
        return QJsonObject{{"ok", false}, {"error", "New password is too short"}};
    }

    QSqlQuery update;
    update.prepare("UPDATE local_users SET password_hash = :password_hash WHERE username_normalized = :username");
    update.bindValue(":password_hash", hashPassword(nextPassword));
    update.bindValue(":username", currentNormalized);
    return QJsonObject{{"ok", update.exec()}};
}

QJsonObject OpenKJEmbeddedApi::loginAdmin(const QJsonObject &payload)
{
    const QString password = payload.value("password").toString();
    if (!m_settings.chkPassword(password)) {
        return QJsonObject{{"ok", false}, {"error", "Invalid admin password"}};
    }
    return QJsonObject{{"ok", true}, {"token", createAdminSession()}};
}

QJsonObject OpenKJEmbeddedApi::logoutAdmin(const QJsonObject &payload)
{
    return QJsonObject{{"ok", removeAdminSessionToken(payload.value("token").toString().trimmed())}};
}

QJsonObject OpenKJEmbeddedApi::currentAdmin(const QUrlQuery &query)
{
    const bool authenticated = isValidAdminSession(query.queryItemValue("token"));
    return QJsonObject{{"ok", true}, {"authenticated", authenticated}};
}

QJsonObject OpenKJEmbeddedApi::requestSongFromLocalUser(const QJsonObject &payload)
{
    QString normalized;
    QString username;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &normalized, &username)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    QJsonObject legacyPayload;
    legacyPayload.insert("songId", payload.value("songId"));
    legacyPayload.insert("singerName", username);
    const QJsonObject response = commandSubmitRequest(legacyPayload);
    if (response.value("error").toString() == "false" || response.value("success").toBool()) {
        QSqlQuery query;
        query.prepare("SELECT MAX(qsongid) FROM queuesongs qs "
                      "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
                      "WHERE rs.name = :name AND qs.song = :song AND qs.played = 0");
        query.bindValue(":name", username);
        query.bindValue(":song", payload.value("songId").toInt());
        if (query.exec() && query.next()) {
            recordRequestOwner(query.value(0).toInt(), normalized);
        }
        return QJsonObject{{"ok", true}};
    }

    return QJsonObject{{"ok", false}, {"error", response.value("errorString").toString("Could not queue song")}};
}

QJsonObject OpenKJEmbeddedApi::removeOwnRequest(const QJsonObject &payload)
{
    QString normalized;
    QString username;
    Q_UNUSED(username)
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &normalized, &username)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    const int requestId = payload.value("entryId").toString().toInt();
    if (requestId <= 0) {
        return QJsonObject{{"ok", false}, {"error", "Missing entryId"}};
    }

    const QString owner = requestOwner(requestId);
    if (!owner.isEmpty() && owner != normalized) {
        return QJsonObject{{"ok", false}, {"error", "You can only remove your own songs"}};
    }
    if (owner.isEmpty()) {
        // No ownership record — song was likely added via the desktop app.
        // Fall back to verifying the queue entry belongs to this singer.
        QSqlQuery singerCheck;
        singerCheck.prepare(
            "SELECT rs.name FROM queuesongs qs "
            "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
            "WHERE qs.qsongid = :id AND qs.played = 0");
        singerCheck.bindValue(":id", requestId);
        if (!singerCheck.exec() || !singerCheck.next()) {
            return QJsonObject{{"ok", false}, {"error", "Song not found in queue"}};
        }
        if (normalizeUsername(singerCheck.value(0).toString()) != normalized) {
            return QJsonObject{{"ok", false}, {"error", "You can only remove your own songs"}};
        }
    }

    if (!removeQueueSongById(requestId)) {
        return QJsonObject{{"ok", false}, {"error", "Song not found in queue"}};
    }

    QSqlQuery cleanup;
    cleanup.prepare("DELETE FROM local_request_owners WHERE request_id = :request_id");
    cleanup.bindValue(":request_id", requestId);
    cleanup.exec();
    nextSerial();
    return QJsonObject{{"ok", true}};
}

QJsonObject OpenKJEmbeddedApi::runAdminActionRest(const QJsonObject &payload)
{
    QJsonObject legacyPayload;
    legacyPayload.insert("action", payload.value("type").toString());
    if (payload.contains("entryId")) {
        legacyPayload.insert("request_id", payload.value("entryId").toString().toInt());
    }
    if (payload.contains("value")) {
        legacyPayload.insert("value", payload.value("value"));
    }

    const QJsonObject response = commandAdminAction(legacyPayload);
    if (response.value("error").toString() == "false") {
        return QJsonObject{{"ok", true}};
    }
    return QJsonObject{{"ok", false}, {"error", response.value("errorString").toString("Admin action failed")}};
}

bool OpenKJEmbeddedApi::recordRequestOwner(const int requestId, const QString &normalizedUsername)
{
    if (requestId <= 0 || normalizedUsername.isEmpty()) {
        return false;
    }

    QSqlQuery query;
    query.prepare("INSERT INTO local_request_owners (request_id, username_normalized) VALUES (:request_id, :username) "
                  "ON CONFLICT(request_id) DO UPDATE SET username_normalized = excluded.username_normalized");
    query.bindValue(":request_id", requestId);
    query.bindValue(":username", normalizedUsername);
    return query.exec();
}

QString OpenKJEmbeddedApi::requestOwner(const int requestId) const
{
    QSqlQuery query;
    query.prepare("SELECT username_normalized FROM local_request_owners WHERE request_id = :request_id");
    query.bindValue(":request_id", requestId);
    if (query.exec() && query.next()) {
        return query.value(0).toString();
    }
    return {};
}

bool OpenKJEmbeddedApi::removeUserSessionToken(const QString &token)
{
    QSqlQuery query;
    query.prepare("DELETE FROM local_user_sessions WHERE token = :token");
    query.bindValue(":token", token);
    return query.exec();
}

bool OpenKJEmbeddedApi::removeAdminSessionToken(const QString &token)
{
    QSqlQuery query;
    query.prepare("DELETE FROM local_admin_sessions WHERE token = :token");
    query.bindValue(":token", token);
    return query.exec();
}
