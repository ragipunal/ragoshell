#include "ragoshell/core/transport/WebSocketGatewayTransport.h"

#include <QEventLoop>
#include <QJsonObject>
#include <QTimer>
#include <QWebSocket>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/transport/GatewayProtocol.h"

namespace RagoShell
{
    WebSocketGatewayTransport::WebSocketGatewayTransport(const QString &url,
                                                         const QString &token)
        : _url(url), _token(token), _opTimeoutMs(GatewayProtocol::kOpTimeoutMs)
    {
    }

    WebSocketGatewayTransport::~WebSocketGatewayTransport() = default;

    IMongoTransport::Result WebSocketGatewayTransport::call(
        const QString &op, const QString &paramKey, const QString &paramValue)
    {
        return call(op, paramKey, paramValue, QString(), QString());
    }

    IMongoTransport::Result WebSocketGatewayTransport::call(
        const QString &op, const QString &keyA, const QString &valueA,
        const QString &keyB, const QString &valueB)
    {
        Result r;
        if (!_socket || _socket->state() != QAbstractSocket::ConnectedState) {
            r.error = QStringLiteral("Gateway socket is not connected.");
            return r;
        }

        QJsonObject params;
        if (!keyA.isEmpty())
            params.insert(keyA, valueA);
        if (!keyB.isEmpty())
            params.insert(keyB, valueB);

        const quint32 id = _nextId++;
        QString reply;
        QString closed;
        bool gotReply = false;
        // The receiver must be &loop, not _socket: connections auto-disconnect
        // when the loop leaves scope; with _socket as receiver the lambda would
        // quit() a dead stack loop on the next frame (use-after-scope crash).
        QEventLoop loop;
        QObject::connect(_socket.get(), &QWebSocket::textMessageReceived,
                         &loop, [&](const QString &frame) {
                             QString ejson, err;
                             qint64 gotId = -1;
                             // Ignore any non-zero mismatched id (late reply to a
                             // timed-out earlier call OR reply to a nested/pipelined
                             // call on the same QThread). Id 0 is the session's own
                             // close error and still ends the call (R10 + W4 H1).
                             if (GatewayProtocol::decodeResponse(frame, id, &ejson, &err, &gotId)) {
                                 reply = ejson;
                                 gotReply = true;
                                 loop.quit();
                             } else if (gotId > 0 && gotId != static_cast<qint64>(id)) {
                                 return;
                             } else if (!err.isEmpty()) {
                                 closed = err;
                                 loop.quit();
                             }
                         });
        QObject::connect(_socket.get(), &QWebSocket::disconnected,
                         &loop, [&] {
                             closed = QStringLiteral("Gateway socket closed.");
                             loop.quit();
                         });
        QTimer::singleShot(_opTimeoutMs, &loop, [&] {
            closed = QStringLiteral("Gateway op timed out.");
            loop.quit();
        });

        _socket->sendTextMessage(GatewayProtocol::encodeRequest(id, op, params));
        loop.exec();

        if (!closed.isEmpty()) {
            r.error = closed;
            return r;
        }
        if (!gotReply) {
            r.error = QStringLiteral("Gateway request interrupted before reply.");
            return r;
        }
        r.ok = true;
        r.ejson = reply;
        return r;
    }

    IMongoTransport::Result WebSocketGatewayTransport::connect(const QString &ejsonSettings)
    {
        Result r;
        if (!_socket || _socket->state() == QAbstractSocket::UnconnectedState
                      || _socket->state() == QAbstractSocket::ClosingState) {
            _socket = std::make_unique<QWebSocket>(
                QString::fromUtf8(BrandInfo::productUrl),
                QWebSocketProtocol::VersionLatest);
            // Qt accepts ~2 GiB per message by default; a hostile or broken
            // gateway must not be able to make the client buffer that
            // (2026-11-04 review R2). The gateway never sends more.
            _socket->setMaxAllowedIncomingMessageSize(
                static_cast<quint64>(GatewayProtocol::kMaxResponseBytes));
            _socket->setMaxAllowedIncomingFrameSize(
                static_cast<quint64>(GatewayProtocol::kMaxResponseBytes));
            _socket->open(QUrl(_url));
        }

        // Wait for the socket to come up before hello (skipped when a live
        // link is reused — connecting again must not wait for a signal that
        // already fired).
        if (_socket->state() != QAbstractSocket::ConnectedState) {
            QEventLoop loop;
            QString why;
            QObject::connect(_socket.get(), &QWebSocket::connected, &loop, &QEventLoop::quit);
            QObject::connect(_socket.get(), &QWebSocket::disconnected, &loop, [&] {
                why = QStringLiteral("Gateway socket refused the connection.");
                loop.quit();
            });
            QObject::connect(_socket.get(), &QWebSocket::errorOccurred, &loop,
                             [&why, &loop](QAbstractSocket::SocketError) {
                                 if (why.isEmpty())
                                     why = QStringLiteral("Gateway socket error.");
                                 loop.quit();
                             });
            QTimer::singleShot(GatewayProtocol::kHelloTimeoutMs, &loop, [&] {
                why = QStringLiteral("Gateway connect timed out.");
                loop.quit();
            });
            loop.exec();
            if (!why.isEmpty() || _socket->state() != QAbstractSocket::ConnectedState) {
                r.error = why.isEmpty() ? QStringLiteral("Gateway socket is not connected.")
                                        : why;
                return r;
            }
        }

        // Pairing handshake — always sent; agents without a token ignore it.
        {
            QEventLoop loop;
            QString ejson, err;
            bool gotHello = false;
            const quint32 id = _nextId++;
            QObject::connect(_socket.get(), &QWebSocket::textMessageReceived,
                             &loop, [&](const QString &frame) {
                                 qint64 gotId = -1;
                                 if (GatewayProtocol::decodeResponse(frame, id, &ejson, &err, &gotId)) {
                                     gotHello = true;
                                     loop.quit();
                                 } else if (gotId > 0 && gotId != static_cast<qint64>(id)) {
                                     err.clear(); // late/mismatched reply on a reused link (R10 + W4 H1)
                                 } else if (!err.isEmpty()) {
                                     loop.quit();
                                 }
                             });
            QObject::connect(_socket.get(), &QWebSocket::disconnected, &loop, [&] {
                err = QStringLiteral("Gateway closed during handshake.");
                loop.quit();
            });
            QTimer::singleShot(GatewayProtocol::kHelloTimeoutMs, &loop, [&] {
                err = QStringLiteral("Gateway handshake timed out.");
                loop.quit();
            });
            QJsonObject params{{"token", _token}};
            _socket->sendTextMessage(
                GatewayProtocol::encodeRequest(id, QString::fromUtf8(GatewayProtocol::Op::Hello),
                                               params));
            loop.exec();
            if (!err.isEmpty() || !gotHello) {
                r.error = err.isEmpty() ? QStringLiteral("Gateway handshake interrupted.") : err;
                return r;
            }
        }

        return call(QString::fromUtf8(GatewayProtocol::Op::Connect),
                    QStringLiteral("settings"), ejsonSettings);
    }

    IMongoTransport::Result WebSocketGatewayTransport::runCommand(
        const QString &database, const QString &ejsonCommand)
    {
        return call(QString::fromUtf8(GatewayProtocol::Op::RunCommand),
                    QStringLiteral("database"), database,
                    QStringLiteral("command"), ejsonCommand);
    }

    IMongoTransport::Result WebSocketGatewayTransport::listDatabases()
    {
        return call(QString::fromUtf8(GatewayProtocol::Op::ListDatabases),
                    QString(), QString());
    }

    IMongoTransport::Result WebSocketGatewayTransport::listCollections(const QString &database)
    {
        return call(QString::fromUtf8(GatewayProtocol::Op::ListCollections),
                    QStringLiteral("database"), database);
    }

    IMongoTransport::Result WebSocketGatewayTransport::ping()
    {
        return call(QString::fromUtf8(GatewayProtocol::Op::Ping), QString(), QString());
    }

    IMongoTransport::Result WebSocketGatewayTransport::killOp(const QString &ejsonFilter)
    {
        return call(QString::fromUtf8(GatewayProtocol::Op::KillOp),
                    QStringLiteral("filter"), ejsonFilter);
    }

    void WebSocketGatewayTransport::disconnect()
    {
        if (_socket && _socket->state() == QAbstractSocket::ConnectedState)
            call(QString::fromUtf8(GatewayProtocol::Op::Disconnect), QString(), QString());
        if (_socket)
            _socket->close();
    }
}
