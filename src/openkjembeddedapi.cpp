#include "openkjembeddedapi.h"

#include <algorithm>
#include <iterator>
#include <QCryptographicHash>
#include <QPair>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>
#include <QPasswordDigestor>

namespace {

// Every request this API serves is a short JSON envelope or a bare GET, so these
// caps are orders of magnitude above anything legitimate. They exist so a client
// that never finishes sending can't grow a buffer without bound on the UI thread.
constexpr int kMaxHeaderBytes = 16 * 1024;
constexpr int kMaxBodyBytes = 256 * 1024;

// A request that hasn't completed in this long is abandoned, not slow. SSE clients
// are exempt - being idle is the entire point of that connection.
constexpr qint64 kIdleTimeoutMs = 20000;
constexpr int kIdleSweepIntervalMs = 5000;

// Ceiling on simultaneous sockets. A room full of phones is a few dozen; anything
// past this is something misbehaving, and refusing is better than running the app
// out of descriptors mid-show.
constexpr int kMaxConnections = 256;

constexpr int kLoginFailureLimit = 10;
constexpr qint64 kLoginFailureWindowMs = 5 * 60 * 1000;
constexpr qint64 kLoginLockoutMs = 5 * 60 * 1000;

// PBKDF2 work factor. Deliberately below the usual web-app recommendation: this
// runs on the Qt main thread, so the cost is paid as UI latency during a show.
// 50k puts a login at roughly 50ms while making an offline crack of a leaked
// openkj.sqlite ~50,000x more expensive than the SHA-256 this replaced. The value
// is stored per row, so it can be raised later without invalidating anything.
constexpr int kPbkdf2Iterations = 50000;
constexpr int kPbkdf2KeyBytes = 32;
constexpr int kSaltBytes = 16;

} // namespace

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

    // Rotation edits arrive in bursts (a drag reorder emits per row), so coalesce
    // them into one snapshot instead of pushing a frame per signal.
    m_sseBroadcastTimer.setSingleShot(true);
    m_sseBroadcastTimer.setInterval(250);
    connect(&m_sseBroadcastTimer, &QTimer::timeout, this, &OpenKJEmbeddedApi::broadcastSseSnapshot);

    // Doubles as the keepalive: nothing signals us when a song's remaining time
    // ticks down, so a periodic snapshot is what keeps wait_seconds honest.
    m_sseRefreshTimer.setInterval(20000);
    connect(&m_sseRefreshTimer, &QTimer::timeout, this, &OpenKJEmbeddedApi::broadcastSseSnapshot);

    connect(&m_rotationModel, &TableModelRotation::rotationModified,
            this, &OpenKJEmbeddedApi::scheduleSseBroadcast);
    connect(&m_queueModel, &TableModelQueueSongs::queueModified,
            this, [this](int) { scheduleSseBroadcast(); });

    m_idleSweepTimer.setInterval(kIdleSweepIntervalMs);
    connect(&m_idleSweepTimer, &QTimer::timeout, this, &OpenKJEmbeddedApi::sweepIdleConnections);
}

bool OpenKJEmbeddedApi::start(const quint16 port, const QHostAddress &address)
{
    if (m_server.isListening()) {
        return true;
    }

    ensureLocalModeSchema();
    if (!m_server.listen(address, port)) {
        return false;
    }
    m_idleSweepTimer.start();
    return true;
}

void OpenKJEmbeddedApi::stop()
{
    if (!m_server.isListening()) {
        return;
    }

    m_sseBroadcastTimer.stop();
    m_sseRefreshTimer.stop();
    m_idleSweepTimer.stop();
    m_sseClients.clear();
    m_upNextNotified.clear();

    for (QTcpSocket *socket : m_connections.keys()) {
        socket->disconnectFromHost();
    }

    m_server.close();
}

void OpenKJEmbeddedApi::onNewConnection()
{
    while (m_server.hasPendingConnections()) {
        QTcpSocket *socket = m_server.nextPendingConnection();
        if (m_connections.size() >= kMaxConnections) {
            // Drop it outright rather than accept and queue: at this point the
            // useful thing is to stop consuming sockets, not to explain why.
            socket->abort();
            socket->deleteLater();
            continue;
        }
        m_connections.insert(socket, Connection{{}, QDateTime::currentMSecsSinceEpoch()});
        connect(socket, &QTcpSocket::readyRead, this, &OpenKJEmbeddedApi::onSocketReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &OpenKJEmbeddedApi::onSocketDisconnected);
    }
}

void OpenKJEmbeddedApi::onSocketReadyRead()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) {
        return;
    }
    auto connection = m_connections.find(socket);
    if (connection == m_connections.end()) {
        return;
    }

    connection->lastActivityMs = QDateTime::currentMSecsSinceEpoch();
    connection->buffer.append(socket->readAll());

    // An event-stream socket has nothing left to say; drop anything it sends so
    // the buffer can't grow unbounded while we hold the connection open.
    if (m_sseClients.contains(socket)) {
        connection->buffer.clear();
        return;
    }

    HttpRequest request;
    switch (tryParseHttpRequest(connection->buffer, request)) {
        case ParseResult::Incomplete:
            return;
        case ParseResult::Malformed:
            sendErrorAndClose(socket, 400, "Malformed request");
            return;
        case ParseResult::HeaderTooLarge:
            sendErrorAndClose(socket, 431, "Request header fields too large");
            return;
        case ParseResult::BodyTooLarge:
            sendErrorAndClose(socket, 413, "Request body too large");
            return;
        case ParseResult::Complete:
            break;
    }

    if (request.method == "GET") {
        QString cleanPath;
        QUrlQuery query;
        parseRequestPath(request.path, cleanPath, query);
        if (cleanPath == "/local/events") {
            beginEventStream(socket);
            return;
        }
    }

    m_currentClientKey = socket->peerAddress().toString();
    const QByteArray response = handleRequest(request);
    m_currentClientKey.clear();
    socket->write(response);
    socket->disconnectFromHost();
}

// Sockets are dropped on inactivity rather than on a per-request deadline, so a
// slow upload still succeeds as long as bytes keep arriving.
void OpenKJEmbeddedApi::sweepIdleConnections()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();

    // Collected first because disconnectFromHost() can emit disconnected()
    // synchronously, and that removes the entry we would be iterating over.
    QList<QTcpSocket *> expired;
    for (auto it = m_connections.cbegin(); it != m_connections.cend(); ++it) {
        if (m_sseClients.contains(it.key())) {
            continue;
        }
        if (now - it.value().lastActivityMs >= kIdleTimeoutMs) {
            expired.append(it.key());
        }
    }
    for (QTcpSocket *socket : expired) {
        socket->disconnectFromHost();
    }

    // Piggyback the throttle cleanup: without it, one entry per peer address that
    // ever failed a login would accumulate for the life of the process.
    for (auto it = m_loginThrottle.begin(); it != m_loginThrottle.end();) {
        const bool locked = now < it.value().lockedUntilMs;
        const bool windowLive = now - it.value().windowStartMs < kLoginFailureWindowMs;
        it = (locked || windowLive) ? std::next(it) : m_loginThrottle.erase(it);
    }
}

void OpenKJEmbeddedApi::sendErrorAndClose(QTcpSocket *socket, const int statusCode, const QString &message)
{
    socket->write(jsonResponse(statusCode, QJsonObject{{"ok", false}, {"error", message}}));
    socket->disconnectFromHost();
}

void OpenKJEmbeddedApi::onSocketDisconnected()
{
    auto *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) {
        return;
    }

    m_connections.remove(socket);
    m_sseClients.remove(socket);
    if (m_sseClients.isEmpty()) {
        m_sseRefreshTimer.stop();
        // Nothing drives evaluateUpNext() while no one is subscribed, so the set
        // would otherwise thaw out stale and swallow the first alert after someone
        // reconnects. "Already told" only means anything over a live connection.
        m_upNextNotified.clear();
    }
    socket->deleteLater();
}

void OpenKJEmbeddedApi::beginEventStream(QTcpSocket *socket)
{
    QByteArray headers;
    headers.append("HTTP/1.1 200 OK\r\n");
    headers.append("Content-Type: text/event-stream; charset=utf-8\r\n");
    headers.append("Cache-Control: no-cache, no-store\r\n");
    headers.append("Connection: keep-alive\r\n");
    // Keeps reverse proxies from buffering the stream into uselessness.
    headers.append("X-Accel-Buffering: no\r\n");
    headers.append("Access-Control-Allow-Origin: *\r\n");
    headers.append("\r\n");
    // How long EventSource should wait before reconnecting after a drop.
    headers.append("retry: 3000\n\n");
    socket->write(headers);

    // Exempt from the idle sweep from here on, so let TCP notice a phone that left
    // the building. Without this a half-open stream lingers until the OS default
    // keepalive fires, which is hours away.
    socket->setSocketOption(QAbstractSocket::KeepAliveOption, 1);

    m_sseClients.insert(socket);
    if (!m_sseRefreshTimer.isActive()) {
        m_sseRefreshTimer.start();
    }

    // Send the current state immediately so a fresh subscriber doesn't have to
    // wait for the next rotation change to render anything.
    writeSseFrame(socket, "queue", QJsonDocument(buildQueueResponse()).toJson(QJsonDocument::Compact));
}

void OpenKJEmbeddedApi::scheduleSseBroadcast()
{
    if (m_sseClients.isEmpty()) {
        return;
    }
    if (!m_sseBroadcastTimer.isActive()) {
        m_sseBroadcastTimer.start();
    }
}

void OpenKJEmbeddedApi::broadcastSseSnapshot()
{
    if (m_sseClients.isEmpty()) {
        m_sseRefreshTimer.stop();
        m_upNextNotified.clear();
        return;
    }

    ++m_sseEventId;
    const QByteArray data = QJsonDocument(buildQueueResponse()).toJson(QJsonDocument::Compact);
    const auto clients = m_sseClients;
    for (QTcpSocket *socket : clients) {
        writeSseFrame(socket, "queue", data);
    }

    evaluateUpNext();
}

// The snapshot already carries turns_until, so a phone could in principle work this
// out for itself - but it would have to diff consecutive frames, and the 20 s
// keepalive and every reconnect replay the same numbers. Detecting the edge here
// instead means it is found once, against state the server already holds, and a
// singer gets told exactly once per turn no matter how their phone behaves.
void OpenKJEmbeddedApi::evaluateUpNext()
{
    const int threshold = m_settings.embeddedApiUpNextTurns();
    if (threshold <= 0) {
        m_upNextNotified.clear();
        return;
    }

    QSqlQuery query;
    query.prepare(
        "SELECT rs.name, rs.position, qs.qsongid, d.songid, d.artist, d.title, COALESCE(d.duration, 0) "
        "FROM queuesongs qs "
        "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
        "INNER JOIN dbsongs d ON d.songid = qs.song "
        // Someone who marked themselves away gets skipped in the rotation, so paging
        // them to the mic would be wrong.
        "WHERE qs.played = 0 AND qs.singer != :nowSinger AND COALESCE(rs.paused, 0) = 0 "
        "ORDER BY rs.position ASC, qs.position ASC");
    query.bindValue(":nowSinger", currentSingerId());
    if (!query.exec()) {
        return;
    }

    QSet<QString> seen;
    QSet<QString> inWindow;
    QList<QJsonObject> alerts;
    while (query.next()) {
        const QString singer = query.value(0).toString();
        // First row per singer wins: the ORDER BY puts their next song first, and
        // that is the one the alert should name.
        if (seen.contains(singer)) {
            continue;
        }
        seen.insert(singer);

        const int position = query.value(1).toInt();
        const int turns = m_rotationModel.positionTurnDistance(position);
        // 0 means they are the singer at the mic. The query excludes that singer by
        // id, but the model's idea of who is current can lag the setting by a beat.
        if (turns <= 0 || turns > threshold) {
            continue;
        }

        inWindow.insert(singer);
        if (m_upNextNotified.contains(singer)) {
            continue;
        }
        alerts.append(QJsonObject{
            {"singer", singer},
            {"turns_until", turns},
            {"wait_seconds", m_rotationModel.positionWaitTime(position)},
            {"request_id", query.value(2).toInt()},
            {"song_id", query.value(3).toInt()},
            {"artist", query.value(4).toString()},
            {"title", query.value(5).toString()},
            {"duration_seconds", std::max(1, query.value(6).toInt() / 1000)}
        });
    }

    m_upNextNotified = inWindow;
    if (alerts.isEmpty()) {
        return;
    }

    // Fanned out to every subscriber rather than to one singer: the stream carries
    // no identity, and the queue snapshot on the same connection already names
    // everyone in the rotation, so this adds nothing a client could not read there.
    // The client raises a notification only when "singer" is the user it logged in as.
    const auto clients = m_sseClients;
    for (const QJsonObject &alert : alerts) {
        ++m_sseEventId;
        const QByteArray data = QJsonDocument(alert).toJson(QJsonDocument::Compact);
        for (QTcpSocket *socket : clients) {
            writeSseFrame(socket, "upnext", data);
        }
    }
}

void OpenKJEmbeddedApi::writeSseFrame(QTcpSocket *socket, const QByteArray &eventName, const QByteArray &data)
{
    if (socket->state() != QAbstractSocket::ConnectedState) {
        m_sseClients.remove(socket);
        return;
    }

    // A client that stopped reading would otherwise let the write buffer grow
    // without bound; cut it loose and let EventSource reconnect.
    if (socket->bytesToWrite() > 1024 * 1024) {
        m_sseClients.remove(socket);
        socket->abort();
        return;
    }

    QByteArray frame;
    frame.append("id: ");
    frame.append(QByteArray::number(m_sseEventId));
    frame.append('\n');
    frame.append("event: ");
    frame.append(eventName);
    frame.append('\n');
    // Compact JSON never contains a raw newline, so a single data: line is safe.
    frame.append("data: ");
    frame.append(data);
    frame.append("\n\n");
    socket->write(frame);
    socket->flush();
}

OpenKJEmbeddedApi::ParseResult OpenKJEmbeddedApi::tryParseHttpRequest(QByteArray &buffer, HttpRequest &request)
{
    const int headerEnd = buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        // Still no end of headers in sight - either more is coming or the client
        // is never going to send it.
        return buffer.size() > kMaxHeaderBytes ? ParseResult::HeaderTooLarge : ParseResult::Incomplete;
    }
    if (headerEnd > kMaxHeaderBytes) {
        return ParseResult::HeaderTooLarge;
    }

    const QByteArray headerSection = buffer.left(headerEnd);
    const QList<QByteArray> headerLines = headerSection.split('\n');
    if (headerLines.isEmpty()) {
        return ParseResult::Malformed;
    }

    const QList<QByteArray> firstLineParts = headerLines.first().trimmed().split(' ');
    if (firstLineParts.size() < 2) {
        return ParseResult::Malformed;
    }

    request.method = QString::fromUtf8(firstLineParts.at(0)).trimmed().toUpper();
    request.path = QString::fromUtf8(firstLineParts.at(1)).trimmed();
    request.headers = parseHeaders(headerLines.mid(1));

    // Parsed strictly: a garbage Content-Length used to silently read as 0, which
    // left whatever followed it sitting in the buffer to be parsed as a request.
    bool lengthValid = true;
    const qint64 contentLength = request.headers.contains("content-length")
                                         ? request.headers.value("content-length").toLongLong(&lengthValid)
                                         : 0;
    if (!lengthValid || contentLength < 0) {
        return ParseResult::Malformed;
    }
    if (contentLength > kMaxBodyBytes) {
        return ParseResult::BodyTooLarge;
    }

    const qint64 totalNeeded = static_cast<qint64>(headerEnd) + 4 + contentLength;
    if (buffer.size() < totalNeeded) {
        return ParseResult::Incomplete;
    }

    request.body = buffer.mid(headerEnd + 4, static_cast<int>(contentLength));
    buffer.remove(0, static_cast<int>(totalNeeded));
    return ParseResult::Complete;
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

        const QByteArray response = jsonResponse(200, handleApiCommand(doc.object()));
        scheduleSseBroadcast();
        return response;
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

        // artist and title are COLLATE NOCASE columns, so a bare prefix LIKE is already
        // case-insensitive and - unlike upper(trim(col)) - SQLite can turn it into a range
        // seek on idx_dbsongs_artist_title. Values are trimmed on ingest and by the v109
        // migration, so dropping trim() here doesn't lose rows. Sorting on the bare
        // columns keeps the same order while letting the index supply it.
        QSqlQuery query;
        query.prepare(QString("SELECT songid, artist, title, COALESCE(duration, 0) FROM dbsongs "
                              "WHERE discid != '!!DROPPED!!' AND discid != '!!BAD!!' "
                              "AND %1 LIKE :prefix "
                              "ORDER BY %1, %2 LIMIT :limit").arg(primary, secondary));
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
        // Most /local POSTs mutate the queue or rotation, and several of them do it
        // with raw SQL that never trips a model signal - push a snapshot regardless.
        const QByteArray response = handleLocalApiPost(cleanPath, payload);
        scheduleSseBroadcast();
        return response;
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

    // Commands that mutate show state need the admin session token /local/auth/login
    // issues. Gating here rather than inside each command handler keeps the two
    // dispatchers - this one and handleLocalApiPost() - as the only places auth is
    // decided, so a new privileged command can't be added without passing through it.
    static const QSet<QString> privilegedCommands{
        "adminAction", "setAccepting", "setEventSettings", "deleteRequest", "clearRequests"};
    if (privilegedCommands.contains(command) &&
        !isValidAdminSession(payload.value("token").toString().trimmed())) {
        // Callers of this envelope read either shape depending on the command
        // (getEventSettings answers with "ok", the rest with "error"), so answer both.
        return QJsonObject{{"command", command},
                           {"error", "true"},
                           {"errorString", "Admin authentication required"},
                           {"ok", false}};
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
        // No lower() on the column: SQLite's LIKE is already case-insensitive for ASCII
        // (and its lower() only folds ASCII anyway), so the wrapper only cost a function
        // call per row. Terms are still bound lowercased below.
        for (int i = 0; i < terms.size(); ++i) {
            sql += QString("AND (artist || ' ' || title) LIKE :term%1 ").arg(i);
        }
    }
    // Sorting the bare NOCASE columns rather than upper() of them lets
    // idx_dbsongs_artist_title supply the order, so SQLite can stop at LIMIT instead of
    // materializing every match into a temp B-tree first.
    sql += QString("ORDER BY artist, title LIMIT %1").arg(limit);

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

    if (TableModelQueueSongs::singerHasUnplayedSong(singerId, songId)) {
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
            // The live pipeline value, not queuesongs.keychg - the KJ's spinbox can
            // have moved the key without writing to the database. The client needs
            // this to display where the key actually sits.
            nowPlaying.insert("key_change", m_livePitch);
            // False in the gap between songs, when this row is really the song that
            // just finished. Clients gate the self-skip control on it.
            nowPlaying.insert("is_playing", m_karaokePlaying);
            nowPlaying.insert("request_time", QDateTime::currentSecsSinceEpoch());
        }
    }

    QSqlQuery query;
    query.prepare(
        "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0), qs.keychg, "
        "COALESCE(rs.paused, 0), rs.position "
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
            const int singerPosition = query.value(8).toInt();
            QJsonObject request;
            request.insert("request_id", query.value(0).toInt());
            request.insert("singer", query.value(1).toString());
            request.insert("song_id", query.value(2).toInt());
            request.insert("artist", query.value(3).toString());
            request.insert("title", query.value(4).toString());
            request.insert("duration_seconds", std::max(1, query.value(5).toInt() / 1000));
            request.insert("key_change", query.value(6).toInt());
            request.insert("singer_paused", query.value(7).toBool());
            request.insert("turns_until", m_rotationModel.positionTurnDistance(singerPosition));
            request.insert("wait_seconds", m_rotationModel.positionWaitTime(singerPosition));
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
            "SELECT qs.qsongid, rs.name, d.songid, d.artist, d.title, COALESCE(d.duration, 0), qs.keychg, "
            "COALESCE(rs.paused, 0) "
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
                request.insert("singer_paused", remainingQuery.value(7).toBool());
                // These are the current singer's own leftovers - they're at the mic now,
                // so there is no wait to report.
                request.insert("turns_until", 0);
                request.insert("wait_seconds", 0);
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
    if (path == "/local/user/favorites") {
        return jsonResponse(200, listUserFavorites(query));
    }
    if (path == "/local/user/history") {
        return jsonResponse(200, listUserHistory(query));
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
    // The admin-only half of this dispatcher, gated the same way and on the same
    // token as the privileged commands in handleApiCommand(). The user routes below
    // are not listed here: they authorize a specific singer against their own queue
    // entry, which only the individual handler can decide.
    static const QSet<QString> adminPaths{"/local/admin/action", "/local/event-settings"};
    if (adminPaths.contains(path) && !isValidAdminSession(payload.value("token").toString().trimmed())) {
        // 200 + ok:false, matching how every other auth failure on this surface answers -
        // a client that throws on non-2xx would swallow the message.
        return jsonResponse(200, QJsonObject{{"ok", false}, {"error", "Admin authentication required"}});
    }

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
    if (path == "/local/user/favorites") {
        return jsonResponse(200, addUserFavorite(payload));
    }
    if (path == "/local/user/favorites/remove") {
        return jsonResponse(200, removeUserFavorite(payload));
    }
    if (path == "/local/request") {
        return jsonResponse(200, requestSongFromLocalUser(payload));
    }
    if (path == "/local/request/remove") {
        return jsonResponse(200, removeOwnRequest(payload));
    }
    if (path == "/local/request/move") {
        return jsonResponse(200, moveOwnRequest(payload));
    }
    if (path == "/local/user/away") {
        return jsonResponse(200, setOwnAway(payload));
    }
    if (path == "/local/request/key") {
        return jsonResponse(200, setOwnSongKey(payload));
    }
    if (path == "/local/request/skip") {
        return jsonResponse(200, skipOwnSong(payload));
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
        case 413:
            return "Payload Too Large";
        case 431:
            return "Request Header Fields Too Large";
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
        {"supportsLocalMode", true},
        {"supportsFavorites", true},
        {"supportsUserHistory", true},
        {"supportsAwayToggle", true},
        {"supportsReorderOwnQueue", true},
        // The singer at the mic can end their own song early via
        // POST /local/request/skip. Only offer the control while
        // now_playing.is_playing is true and now_playing is theirs.
        {"supportsSelfSkip", true},
        {"supportsEventStream", true},
        {"eventStreamPath", "/local/events"},
        {"supportsUpNextEvents", true},
        {"upNextEventName", "upnext"},
        // 0 when the KJ has turned the alerts off, so a client can hide the
        // notification opt-in rather than offer one that will never fire.
        {"upNextTurns", m_settings.embeddedApiUpNextTurns()}
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

bool OpenKJEmbeddedApi::userOwnsRequest(const int requestId, const QString &normalizedUsername, QString *error) const
{
    const QString owner = requestOwner(requestId);
    if (!owner.isEmpty()) {
        if (owner != normalizedUsername) {
            *error = "You can only change your own songs";
            return false;
        }
        return true;
    }

    // No ownership record - song was likely added via the desktop app.
    // Fall back to verifying the queue entry belongs to this singer.
    QSqlQuery singerCheck;
    singerCheck.prepare(
        "SELECT rs.name FROM queuesongs qs "
        "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
        "WHERE qs.qsongid = :id AND qs.played = 0");
    singerCheck.bindValue(":id", requestId);
    if (!singerCheck.exec() || !singerCheck.next()) {
        *error = "Song not found in queue";
        return false;
    }
    if (normalizeUsername(singerCheck.value(0).toString()) != normalizedUsername) {
        *error = "You can only change your own songs";
        return false;
    }
    return true;
}

QJsonObject OpenKJEmbeddedApi::moveOwnRequest(const QJsonObject &payload)
{
    QString normalized;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &normalized)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    const int requestId = payload.value("entryId").toString().toInt();
    if (requestId <= 0) {
        return QJsonObject{{"ok", false}, {"error", "Missing entryId"}};
    }

    const QString direction = payload.value("direction").toString().trimmed().toLower();
    if (direction != "up" && direction != "down") {
        return QJsonObject{{"ok", false}, {"error", "direction must be 'up' or 'down'"}};
    }

    QString ownershipError;
    if (!userOwnsRequest(requestId, normalized, &ownershipError)) {
        return QJsonObject{{"ok", false}, {"error", ownershipError}};
    }

    // Positions are per-singer, so this only ever reorders the user's own
    // queue - it can't change their place in the rotation.
    if (!moveQueueSongByOffset(requestId, direction == "up" ? -1 : 1)) {
        return QJsonObject{{"ok", false}, {"error", "Could not move that song"}};
    }

    nextSerial();
    return QJsonObject{{"ok", true}};
}

QJsonObject OpenKJEmbeddedApi::setOwnAway(const QJsonObject &payload)
{
    QString normalized;
    QString username;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &normalized, &username)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    if (!m_rotationModel.singerExists(username)) {
        return QJsonObject{{"ok", false}, {"error", "You are not in the rotation"}};
    }

    const bool away = payload.value("away").toBool();
    m_rotationModel.singerSetPaused(m_rotationModel.getSingerByName(username).id, away);
    nextSerial();
    return QJsonObject{{"ok", true}, {"away", away}};
}

void OpenKJEmbeddedApi::setLivePitch(const int semitones)
{
    m_livePitch = std::clamp(semitones, -12, 12);
}

// A key the KJ dials in on the desktop spinbox is as much the singer's key as one
// they set from their phone, but until now only the phone path wrote it down: the
// spinbox retunes the pipeline without going near the database, so the adjustment
// died with the song. MainWindow decides when to call this, because only it can
// tell a deliberate mid-song change from the setPitchShift() that fires on every
// song load - saving that one would overwrite a singer's stored key with whatever
// the track happened to load at.
void OpenKJEmbeddedApi::commitLivePitch(const int semitones)
{
    // Nothing here is meaningful outside a running Local Mode show, and the
    // local_* tables are only guaranteed to exist once start() has run.
    if (!m_server.isListening()) {
        return;
    }

    const int key = std::clamp(semitones, -12, 12);
    m_livePitch = key;

    QString singerName;
    int songId = -1;
    const int qsongId = nowPlayingQueueSongId(&singerName, &songId);
    if (qsongId < 0 || songId <= 0) {
        return;
    }

    setQueueSongKey(qsongId, key);

    // Whoever owns the request, falling back to the rotation name for songs the KJ
    // queued on the singer's behalf - the same resolution the phone path uses.
    const QString owner = requestOwner(qsongId);
    const QString normalized = owner.isEmpty() ? normalizeUsername(singerName) : owner;

    // Walk-ins the KJ typed straight into the rotation have no account to remember
    // anything against, and inventing a row for them would collide with a real
    // singer who later registers that name.
    QSqlQuery isUser;
    isUser.prepare("SELECT 1 FROM local_users WHERE username_normalized = :username");
    isUser.bindValue(":username", normalized);
    if (isUser.exec() && isUser.next()) {
        rememberUserSongKey(normalized, songId, key);
    }

    nextSerial();
    // now_playing.key_change reports the live pipeline value, and nothing else
    // schedules a push when the pipeline retunes - without this the singer's phone
    // lags by up to the 20 s refresh before it shows where the KJ just put them.
    scheduleSseBroadcast();
}

int OpenKJEmbeddedApi::nowPlayingQueueSongId(QString *singerName, int *songId) const
{
    const int singerId = currentSingerId();
    if (singerId < 0) {
        return -1;
    }

    QSqlQuery query;
    query.prepare(
        "SELECT qs.qsongid, rs.name, qs.song "
        "FROM queuesongs qs "
        "INNER JOIN rotationsingers rs ON rs.singerid = qs.singer "
        "WHERE qs.singer = :singerId AND qs.played = 1 "
        "ORDER BY qs.position DESC LIMIT 1");
    query.bindValue(":singerId", singerId);
    if (!query.exec() || !query.next()) {
        return -1;
    }
    if (singerName) {
        *singerName = query.value(1).toString();
    }
    if (songId) {
        *songId = query.value(2).toInt();
    }
    return query.value(0).toInt();
}

int OpenKJEmbeddedApi::callerNowPlayingQueueSongId(const QString &normalizedUsername,
                                                   int *songId,
                                                   QString *error) const
{
    // Between songs the rotation has not moved on yet, so the ownership check below
    // would still name the last singer as the one at the mic. Without this guard a
    // request landing in that gap would act on the track the KJ has just started for
    // somebody else.
    if (!m_karaokePlaying) {
        *error = "Nothing is playing";
        return -1;
    }

    QString singerName;
    const int qsongId = nowPlayingQueueSongId(&singerName, songId);
    if (qsongId <= 0) {
        *error = "Nothing is playing";
        return -1;
    }

    // The now-playing row has played = 1, so userOwnsRequest() cannot vet it. Same
    // two-step check it makes: ownership record first, singer name as the fallback
    // for songs the KJ queued on this singer's behalf.
    const QString owner = requestOwner(qsongId);
    const bool isOwner = owner.isEmpty() ? normalizeUsername(singerName) == normalizedUsername
                                         : owner == normalizedUsername;
    if (!isOwner) {
        *error = "You are not the one singing right now";
        return -1;
    }

    return qsongId;
}

void OpenKJEmbeddedApi::rememberUserSongKey(const QString &normalizedUsername, const int songId, const int keyChange)
{
    if (songId <= 0) {
        return;
    }
    QSqlQuery query;
    query.prepare("INSERT INTO local_user_song_keys (username_normalized, song_id, key_change, updated_at) "
                  "VALUES (:username, :song_id, :key_change, :updated_at) "
                  "ON CONFLICT(username_normalized, song_id) DO UPDATE SET "
                  "key_change = excluded.key_change, updated_at = excluded.updated_at");
    query.bindValue(":username", normalizedUsername);
    query.bindValue(":song_id", songId);
    query.bindValue(":key_change", std::clamp(keyChange, -12, 12));
    query.bindValue(":updated_at", QDateTime::currentSecsSinceEpoch());
    query.exec();
}

int OpenKJEmbeddedApi::preferredUserSongKey(const QString &normalizedUsername, const int songId) const
{
    QSqlQuery query;
    query.prepare("SELECT key_change FROM local_user_song_keys "
                  "WHERE username_normalized = :username AND song_id = :song_id");
    query.bindValue(":username", normalizedUsername);
    query.bindValue(":song_id", songId);
    if (query.exec() && query.next()) {
        return std::clamp(query.value(0).toInt(), -12, 12);
    }
    return 0;
}

QJsonObject OpenKJEmbeddedApi::setOwnSongKey(const QJsonObject &payload)
{
    QString normalized;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &normalized)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    const QString direction = payload.value("direction").toString().trimmed().toLower();
    if (direction != "up" && direction != "down") {
        return QJsonObject{{"ok", false}, {"error", "direction must be 'up' or 'down'"}};
    }

    QString error;
    int songId = 0;
    const int qsongId = callerNowPlayingQueueSongId(normalized, &songId, &error);
    if (qsongId <= 0) {
        return QJsonObject{{"ok", false}, {"error", error}};
    }

    const int current = m_livePitch;
    const int updated = std::clamp(current + (direction == "up" ? 1 : -1), -12, 12);
    if (updated == current) {
        // Already at the stop. Still a success - the key is where they asked for it.
        return QJsonObject{{"ok", true}, {"key_change", updated}, {"at_limit", true}};
    }

    m_livePitch = updated;
    setQueueSongKey(qsongId, updated);
    rememberUserSongKey(normalized, songId, updated);
    emit pitchChangeRequested(updated);
    nextSerial();
    return QJsonObject{{"ok", true}, {"key_change", updated}, {"at_limit", false}};
}

QJsonObject OpenKJEmbeddedApi::skipOwnSong(const QJsonObject &payload)
{
    QString normalized;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &normalized)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    QString error;
    if (callerNowPlayingQueueSongId(normalized, nullptr, &error) <= 0) {
        return QJsonObject{{"ok", false}, {"error", error}};
    }

    // The queue entry is already played = 1 and the history row was written when the
    // song started, so there is nothing to record here - a song bailed out of halfway
    // still counts as their turn. All that is left is stopping the pipeline.
    emit songSkipRequested();
    nextSerial();
    return QJsonObject{{"ok", true}};
}

void OpenKJEmbeddedApi::setKaraokePlaying(const bool playing)
{
    if (m_karaokePlaying == playing) {
        return;
    }
    m_karaokePlaying = playing;
    // now_playing.is_playing is what gates the skip button on the phone, and a song
    // starting or ending does not necessarily touch the rotation - push the change
    // rather than let it wait out the 20 s refresh.
    scheduleSseBroadcast();
}

QJsonObject OpenKJEmbeddedApi::listUserFavorites(const QUrlQuery &query)
{
    QString normalized;
    if (!isValidUserSession(query.queryItemValue("token").trimmed(), &normalized)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    QSqlQuery select;
    select.prepare("SELECT ds.songid, ds.artist, ds.title, COALESCE(ds.duration, 0) "
                   "FROM local_user_favorites f "
                   "JOIN dbsongs ds ON ds.songid = f.song_id "
                   "WHERE f.username_normalized = :username "
                   "AND ds.discid != '!!DROPPED!!' AND ds.discid != '!!BAD!!' "
                   "ORDER BY ds.artist, ds.title");
    select.bindValue(":username", normalized);

    QJsonArray songs;
    if (select.exec()) {
        while (select.next()) {
            songs.append(QJsonObject{
                {"song_id", select.value(0).toInt()},
                {"artist", select.value(1).toString()},
                {"title", select.value(2).toString()},
                {"duration_seconds", std::max(1, select.value(3).toInt() / 1000)}
            });
        }
    }

    return QJsonObject{{"ok", true}, {"songs", songs}};
}

QJsonObject OpenKJEmbeddedApi::addUserFavorite(const QJsonObject &payload)
{
    QString normalized;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &normalized)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    const int songId = payload.value("songId").toInt();
    QSqlQuery exists;
    exists.prepare("SELECT songid FROM dbsongs WHERE songid = :song_id "
                   "AND discid != '!!DROPPED!!' AND discid != '!!BAD!!'");
    exists.bindValue(":song_id", songId);
    if (!exists.exec() || !exists.next()) {
        return QJsonObject{{"ok", false}, {"error", "Unknown song"}};
    }

    QSqlQuery insert;
    insert.prepare("INSERT INTO local_user_favorites (username_normalized, song_id, created_at) "
                   "VALUES (:username, :song_id, strftime('%s','now')) "
                   "ON CONFLICT(username_normalized, song_id) DO NOTHING");
    insert.bindValue(":username", normalized);
    insert.bindValue(":song_id", songId);
    if (!insert.exec()) {
        return QJsonObject{{"ok", false}, {"error", insert.lastError().text()}};
    }

    return QJsonObject{{"ok", true}};
}

QJsonObject OpenKJEmbeddedApi::removeUserFavorite(const QJsonObject &payload)
{
    QString normalized;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &normalized)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    QSqlQuery remove;
    remove.prepare("DELETE FROM local_user_favorites WHERE username_normalized = :username AND song_id = :song_id");
    remove.bindValue(":username", normalized);
    remove.bindValue(":song_id", payload.value("songId").toInt());
    if (!remove.exec()) {
        return QJsonObject{{"ok", false}, {"error", remove.lastError().text()}};
    }

    return QJsonObject{{"ok", true}};
}

QJsonObject OpenKJEmbeddedApi::listUserHistory(const QUrlQuery &query)
{
    QString normalized;
    if (!isValidUserSession(query.queryItemValue("token").trimmed(), &normalized)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    const int limit = std::clamp(query.queryItemValue("limit").toInt(), 1, 500);

    // A local user's rotation singer name is their username (see
    // requestSongFromLocalUser), which is how history rows tie back to them.
    // The join to dbsongs is what makes a past song re-requestable; it can miss
    // if the file has since left the library, hence the availability flag.
    QSqlQuery select;
    select.prepare("SELECT COALESCE(ds.songid, 0), hs.artist, hs.title, hs.plays, hs.lastplay, "
                   "COALESCE(ds.duration, 0) "
                   "FROM historySongs hs "
                   "JOIN historySingers hsing ON hsing.id = hs.historySinger "
                   "LEFT JOIN dbsongs ds ON ds.path = hs.filepath "
                   "AND ds.discid != '!!DROPPED!!' AND ds.discid != '!!BAD!!' "
                   "WHERE lower(hsing.name) = :username "
                   "ORDER BY hs.lastplay DESC LIMIT :limit");
    select.bindValue(":username", normalized);
    select.bindValue(":limit", limit);

    QJsonArray songs;
    if (select.exec()) {
        while (select.next()) {
            const int songId = select.value(0).toInt();
            songs.append(QJsonObject{
                {"song_id", songId},
                {"artist", select.value(1).toString()},
                {"title", select.value(2).toString()},
                {"plays", select.value(3).toInt()},
                {"last_played", select.value(4).toDateTime().toString(Qt::ISODate)},
                {"duration_seconds", std::max(1, select.value(5).toInt() / 1000)},
                {"available", songId > 0}
            });
        }
    }

    return QJsonObject{{"ok", true}, {"songs", songs}};
}

bool OpenKJEmbeddedApi::ensureLocalModeSchema()
{
    QSqlQuery query;
    const QStringList statements{
        "CREATE TABLE IF NOT EXISTS local_users ("
        "username_normalized TEXT PRIMARY KEY,"
        "username TEXT NOT NULL,"
        "password_hash BLOB NOT NULL,"
        "password_salt BLOB,"
        // Defaults describe a pre-PBKDF2 row, which is exactly what an existing
        // table's rows are once the ALTER below backfills them.
        "password_algo TEXT NOT NULL DEFAULT 'sha256',"
        "password_iterations INTEGER NOT NULL DEFAULT 0,"
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
        "tagline TEXT NOT NULL DEFAULT '')",
        "CREATE TABLE IF NOT EXISTS local_user_favorites ("
        "username_normalized TEXT NOT NULL,"
        "song_id INTEGER NOT NULL,"
        "created_at INTEGER NOT NULL,"
        "PRIMARY KEY (username_normalized, song_id),"
        "FOREIGN KEY(username_normalized) REFERENCES local_users(username_normalized) ON DELETE CASCADE)",
        // The local-user counterpart of regularSongs.keychg: the key a singer last
        // settled on for a song, replayed the next time they request it.
        "CREATE TABLE IF NOT EXISTS local_user_song_keys ("
        "username_normalized TEXT NOT NULL,"
        "song_id INTEGER NOT NULL,"
        "key_change INTEGER NOT NULL,"
        "updated_at INTEGER NOT NULL,"
        "PRIMARY KEY (username_normalized, song_id),"
        "FOREIGN KEY(username_normalized) REFERENCES local_users(username_normalized) ON DELETE CASCADE)"
    };

    for (const auto &statement : statements) {
        if (!query.exec(statement)) {
            return false;
        }
    }

    // Tables created before the PBKDF2 switch are missing the three columns the new
    // scheme needs. This file manages its own schema rather than going through the
    // PRAGMA user_version migrations in dbInit(), so the upgrade is done by hand.
    QSet<QString> localUserColumns;
    QSqlQuery columns;
    if (columns.exec("PRAGMA table_info(local_users)")) {
        while (columns.next()) {
            localUserColumns.insert(columns.value(1).toString());
        }
    }
    const QList<QPair<QString, QString>> passwordColumns{
        {"password_salt", "BLOB"},
        {"password_algo", "TEXT NOT NULL DEFAULT 'sha256'"},
        {"password_iterations", "INTEGER NOT NULL DEFAULT 0"}
    };
    for (const auto &[name, definition] : passwordColumns) {
        if (!localUserColumns.contains(name)) {
            query.exec(QString("ALTER TABLE local_users ADD COLUMN %1 %2").arg(name, definition));
        }
    }

    query.exec("INSERT OR IGNORE INTO local_event_settings (settings_id, app_name, tagline) VALUES (1, 'OpenKJ', '')");
    query.exec("DELETE FROM local_user_sessions WHERE expires_at <= strftime('%s','now')");
    query.exec("DELETE FROM local_admin_sessions WHERE expires_at <= strftime('%s','now')");
    return true;
}

bool OpenKJEmbeddedApi::loginThrottled(int *retryAfterSecs)
{
    const auto entry = m_loginThrottle.constFind(m_currentClientKey);
    if (entry == m_loginThrottle.constEnd()) {
        return false;
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now >= entry->lockedUntilMs) {
        return false;
    }
    if (retryAfterSecs) {
        *retryAfterSecs = static_cast<int>((entry->lockedUntilMs - now + 999) / 1000);
    }
    return true;
}

void OpenKJEmbeddedApi::noteLoginFailure()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    auto &entry = m_loginThrottle[m_currentClientKey];

    // Failures only count as a run if they land close together, so a singer who
    // fumbles their password once a week never accumulates a lockout.
    if (entry.windowStartMs == 0 || now - entry.windowStartMs >= kLoginFailureWindowMs) {
        entry.windowStartMs = now;
        entry.failures = 0;
    }
    if (++entry.failures >= kLoginFailureLimit) {
        entry.lockedUntilMs = now + kLoginLockoutMs;
        entry.failures = 0;
        entry.windowStartMs = now;
    }
}

void OpenKJEmbeddedApi::clearLoginFailures()
{
    m_loginThrottle.remove(m_currentClientKey);
}

QString OpenKJEmbeddedApi::normalizeUsername(const QString &username)
{
    return username.trimmed().toLower();
}

QByteArray OpenKJEmbeddedApi::legacyHashPassword(const QString &password)
{
    return QCryptographicHash::hash(password.trimmed().toUtf8(), QCryptographicHash::Sha256);
}

QByteArray OpenKJEmbeddedApi::derivePassword(const QString &password, const QByteArray &salt, const int iterations)
{
    // Trimmed to match the legacy scheme, so an upgraded account keeps accepting
    // the password its owner has been typing all along.
    return QPasswordDigestor::deriveKeyPbkdf2(QCryptographicHash::Sha256,
                                              password.trimmed().toUtf8(),
                                              salt,
                                              iterations,
                                              kPbkdf2KeyBytes);
}

QByteArray OpenKJEmbeddedApi::randomSalt()
{
    static_assert(kSaltBytes % sizeof(quint32) == 0, "Salt must be a whole number of 32-bit words");
    // system() rather than global(): this seeds a credential, so it needs the OS
    // CSPRNG and not the shared deterministic-seedable generator.
    quint32 words[kSaltBytes / sizeof(quint32)];
    QRandomGenerator::system()->fillRange(words);
    return QByteArray(reinterpret_cast<const char *>(words), kSaltBytes);
}

// Compares in time independent of where the first difference falls. Overkill for a
// LAN, but the alternative is leaking hash bytes to anything that can time us.
bool OpenKJEmbeddedApi::constantTimeEquals(const QByteArray &a, const QByteArray &b)
{
    if (a.size() != b.size() || a.isEmpty()) {
        return false;
    }
    quint8 diff = 0;
    for (int i = 0; i < a.size(); ++i) {
        diff |= static_cast<quint8>(a.at(i)) ^ static_cast<quint8>(b.at(i));
    }
    return diff == 0;
}

bool OpenKJEmbeddedApi::verifyPassword(const QString &normalizedUsername, const QString &password)
{
    QSqlQuery query;
    query.prepare("SELECT password_hash, password_salt, password_algo, password_iterations "
                  "FROM local_users WHERE username_normalized = :username");
    query.bindValue(":username", normalizedUsername);
    if (!query.exec() || !query.next()) {
        return false;
    }

    const QByteArray stored = query.value(0).toByteArray();
    const QByteArray salt = query.value(1).toByteArray();
    const QString algo = query.value(2).toString();
    const int iterations = query.value(3).toInt();

    if (algo == QLatin1String("pbkdf2-sha256") && !salt.isEmpty() && iterations > 0) {
        return constantTimeEquals(stored, derivePassword(password, salt, iterations));
    }

    // Legacy row. Verify the old way, then immediately re-store under the current
    // scheme; after one login per user no unsalted hash is left in the database.
    if (!constantTimeEquals(stored, legacyHashPassword(password))) {
        return false;
    }
    storePassword(normalizedUsername, password);
    return true;
}

bool OpenKJEmbeddedApi::storePassword(const QString &normalizedUsername, const QString &password)
{
    const QByteArray salt = randomSalt();
    QSqlQuery update;
    update.prepare("UPDATE local_users SET password_hash = :hash, password_salt = :salt, "
                   "password_algo = 'pbkdf2-sha256', password_iterations = :iterations "
                   "WHERE username_normalized = :username");
    update.bindValue(":hash", derivePassword(password, salt, kPbkdf2Iterations));
    update.bindValue(":salt", salt);
    update.bindValue(":iterations", kPbkdf2Iterations);
    update.bindValue(":username", normalizedUsername);
    return update.exec();
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

    const QByteArray salt = randomSalt();
    QSqlQuery insert;
    insert.prepare("INSERT INTO local_users (username_normalized, username, password_hash, "
                   "password_salt, password_algo, password_iterations) "
                   "VALUES (:username_normalized, :username, :password_hash, "
                   ":password_salt, 'pbkdf2-sha256', :password_iterations)");
    insert.bindValue(":username_normalized", normalized);
    insert.bindValue(":username", username);
    insert.bindValue(":password_hash", derivePassword(password, salt, kPbkdf2Iterations));
    insert.bindValue(":password_salt", salt);
    insert.bindValue(":password_iterations", kPbkdf2Iterations);
    if (!insert.exec()) {
        return QJsonObject{{"ok", false}, {"error", insert.lastError().text()}};
    }

    return QJsonObject{{"ok", true}, {"username", username}, {"token", createUserSession(normalized)}};
}

QJsonObject OpenKJEmbeddedApi::loginLocalUser(const QJsonObject &payload)
{
    ensureLocalModeSchema();
    int retryAfter = 0;
    if (loginThrottled(&retryAfter)) {
        return QJsonObject{{"ok", false},
                           {"error", QString("Too many failed attempts. Try again in %1 seconds.").arg(retryAfter)},
                           {"retryAfter", retryAfter}};
    }

    const QString username = payload.value("username").toString().trimmed();
    const QString password = payload.value("password").toString();
    const QString normalized = normalizeUsername(username);

    QSqlQuery query;
    query.prepare("SELECT username FROM local_users WHERE username_normalized = :username");
    query.bindValue(":username", normalized);
    const bool userExists = query.exec() && query.next();
    if (!userExists || !verifyPassword(normalized, password)) {
        noteLoginFailure();
        return QJsonObject{{"ok", false}, {"error", "Invalid username or password"}};
    }

    clearLoginFailures();
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
    const bool inRotation = authenticated && m_rotationModel.singerExists(username);
    return QJsonObject{
        {"ok", true},
        {"authenticated", authenticated},
        {"username", authenticated ? username : QString()},
        {"inRotation", inRotation},
        {"away", inRotation && m_rotationModel.getSingerByName(username).paused}
    };
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

    // Everything this singer can do from their phone - away, self-skip, key nudges -
    // finds their rotation row by username, so the row has to follow the rename. If
    // somebody else is already sitting in the rotation under the new name it can't,
    // and renaming anyway would leave them holding a name that matches nothing.
    const bool inRotation = m_rotationModel.singerExists(currentUsername);
    if (nextNormalized != currentNormalized && m_rotationModel.singerExists(nextUsername)) {
        return QJsonObject{{"ok", false}, {"error", "A singer by that name is already in the rotation"}};
    }

    if (!migrateLocalUserRows(currentNormalized, nextUsername)) {
        return QJsonObject{{"ok", false}, {"error", "Could not update username"}};
    }

    if (inRotation) {
        const auto &singer = m_rotationModel.getSingerByName(currentUsername);
        if (singer.id != -1)
            m_rotationModel.singerSetName(singer.id, nextUsername);
    }

    return QJsonObject{{"ok", true}, {"username", nextUsername}};
}

bool OpenKJEmbeddedApi::migrateLocalUserRows(const QString &currentNormalized, const QString &nextUsername)
{
    const QString nextNormalized = normalizeUsername(nextUsername);

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

    // OR REPLACE on both of these: they are keyed on (username_normalized, song_id),
    // and the ON DELETE CASCADE in their schema never fires because nothing turns on
    // PRAGMA foreign_keys - so rows orphaned by an earlier rename can still be
    // sitting under the name being moved into. Without it the primary key collides,
    // allOk goes false, and the singer is told the rename failed for no visible
    // reason. Dropping the orphan is the right resolution: the live user's own
    // preferences win.
    QSqlQuery updateFavorites;
    updateFavorites.prepare("UPDATE OR REPLACE local_user_favorites SET username_normalized = :next_normalized "
                            "WHERE username_normalized = :current_normalized");
    updateFavorites.bindValue(":next_normalized", nextNormalized);
    updateFavorites.bindValue(":current_normalized", currentNormalized);
    const bool favoritesOk = updateFavorites.exec();

    QSqlQuery updateSongKeys;
    updateSongKeys.prepare("UPDATE OR REPLACE local_user_song_keys SET username_normalized = :next_normalized "
                           "WHERE username_normalized = :current_normalized");
    updateSongKeys.bindValue(":next_normalized", nextNormalized);
    updateSongKeys.bindValue(":current_normalized", currentNormalized);
    const bool songKeysOk = updateSongKeys.exec();

    // Sung-song history is keyed by rotation singer name, which for a local
    // user is their username. Carry it across so a rename doesn't orphan it.
    // historySingers.name is UNIQUE, so only do this when the new name isn't
    // already taken - merging two singers' history is not something to do
    // silently behind a profile edit.
    QSqlQuery historyTaken;
    historyTaken.prepare("SELECT id FROM historySingers WHERE lower(name) = :name");
    historyTaken.bindValue(":name", nextNormalized);
    if (!(historyTaken.exec() && historyTaken.next())) {
        QSqlQuery renameHistorySinger;
        renameHistorySinger.prepare("UPDATE historySingers SET name = :next WHERE lower(name) = :current");
        renameHistorySinger.bindValue(":next", nextUsername);
        renameHistorySinger.bindValue(":current", currentNormalized);
        renameHistorySinger.exec();
    }

    const bool allOk = userOk && sessionsOk && requestsOk && favoritesOk && songKeysOk;
    tx.exec(allOk ? "COMMIT" : "ROLLBACK");
    return allOk;
}

bool OpenKJEmbeddedApi::localUserExists(const QString &username) const
{
    QSqlQuery query;
    query.prepare("SELECT 1 FROM local_users WHERE username_normalized = :username");
    query.bindValue(":username", normalizeUsername(username));
    // A failed exec means the Local Mode tables were never created on this install,
    // which is just a longer way of saying there is no such account.
    return query.exec() && query.next();
}

bool OpenKJEmbeddedApi::renameLocalUser(const QString &currentUsername, const QString &nextUsername, QString *error)
{
    const QString currentNormalized = normalizeUsername(currentUsername);
    const QString nextNormalized = normalizeUsername(nextUsername);

    if (!localUserExists(currentUsername))
        return true;

    if (nextNormalized != currentNormalized && localUserExists(nextUsername)) {
        if (error)
            *error = QString("A singer account named %1 is already registered.").arg(nextUsername);
        return false;
    }

    // The account has to end up on the same name as the singer's rotation entry, so
    // moving it onto a name somebody else already occupies in the rotation is exactly
    // the split this is here to prevent.
    if (nextNormalized != currentNormalized && m_rotationModel.singerExists(nextUsername)) {
        if (error)
            *error = QString("Another singer named %1 is already in the rotation.").arg(nextUsername);
        return false;
    }

    if (!migrateLocalUserRows(currentNormalized, nextUsername)) {
        if (error)
            *error = "The singer's account could not be updated.";
        return false;
    }

    return true;
}

QJsonObject OpenKJEmbeddedApi::updateLocalPassword(const QJsonObject &payload)
{
    QString currentNormalized;
    QString currentUsername;
    if (!isValidUserSession(payload.value("token").toString().trimmed(), &currentNormalized, &currentUsername)) {
        return QJsonObject{{"ok", false}, {"error", "Authentication required"}};
    }

    int retryAfter = 0;
    if (loginThrottled(&retryAfter)) {
        return QJsonObject{{"ok", false},
                           {"error", QString("Too many failed attempts. Try again in %1 seconds.").arg(retryAfter)},
                           {"retryAfter", retryAfter}};
    }

    if (!verifyPassword(currentNormalized, payload.value("currentPassword").toString())) {
        noteLoginFailure();
        return QJsonObject{{"ok", false}, {"error", "Current password is incorrect"}};
    }
    clearLoginFailures();

    const QString nextPassword = payload.value("newPassword").toString();
    if (nextPassword.size() < 4) {
        return QJsonObject{{"ok", false}, {"error", "New password is too short"}};
    }

    return QJsonObject{{"ok", storePassword(currentNormalized, nextPassword)}};
}

QJsonObject OpenKJEmbeddedApi::loginAdmin(const QJsonObject &payload)
{
    int retryAfter = 0;
    if (loginThrottled(&retryAfter)) {
        return QJsonObject{{"ok", false},
                           {"error", QString("Too many failed attempts. Try again in %1 seconds.").arg(retryAfter)},
                           {"retryAfter", retryAfter}};
    }

    const QString password = payload.value("password").toString();
    if (!m_settings.chkPassword(password)) {
        noteLoginFailure();
        return QJsonObject{{"ok", false}, {"error", "Invalid admin password"}};
    }

    clearLoginFailures();
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
            const int qsongId = query.value(0).toInt();
            recordRequestOwner(qsongId, normalized);
            // Replay the key this singer last settled on for this song, the way
            // regularSongs.keychg does for the KJ's regulars.
            const int preferredKey = preferredUserSongKey(normalized, payload.value("songId").toInt());
            if (preferredKey != 0) {
                setQueueSongKey(qsongId, preferredKey);
            }
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

    QString ownershipError;
    if (!userOwnsRequest(requestId, normalized, &ownershipError)) {
        return QJsonObject{{"ok", false}, {"error", ownershipError}};
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
