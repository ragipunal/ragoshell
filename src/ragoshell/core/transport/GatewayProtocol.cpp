#include "ragoshell/core/transport/GatewayProtocol.h"

#include <QJsonArray>
#include <QJsonDocument>

namespace RagoShell
{
    namespace GatewayProtocol
    {
        QString encodeRequest(quint32 id, const QString &op, const QJsonObject &params)
        {
            const QJsonObject obj{{"id", static_cast<double>(id)},
                                  {"op", op},
                                  {"params", params}};
            return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        }

        bool decodeRequest(const QString &frame, quint32 *id, QString *op,
                           QJsonObject *params, QString *error)
        {
            const QJsonDocument doc = QJsonDocument::fromJson(frame.toUtf8());
            if (!doc.isObject()) {
                if (error)
                    *error = QStringLiteral("Malformed JSON frame.");
                return false;
            }
            const QJsonObject obj = doc.object();
            const QJsonValue idVal = obj.value(QStringLiteral("id"));
            const QJsonValue opVal = obj.value(QStringLiteral("op"));
            const double idNum = idVal.toDouble(-1.0);
            if (!idVal.isDouble() || idNum < 0 || idNum > 0xFFFFFFFF
                || qFloor(idNum) != idNum
                || !opVal.isString() || opVal.toString().isEmpty()) {
                if (error)
                    *error = QStringLiteral("Invalid request envelope.");
                return false;
            }
            *id = static_cast<quint32>(idNum);
            *op = opVal.toString();
            *params = obj.value(QStringLiteral("params")).toObject();
            return true;
        }

        QString encodeOk(quint32 id, const QString &resultEjson)
        {
            // resultEjson is already an EJSON document/array — embed it parsed
            // so the frame stays a single JSON value.
            const QJsonDocument inner = QJsonDocument::fromJson(resultEjson.toUtf8());
            QJsonObject obj{{"id", static_cast<double>(id)}, {"ok", true}};
            if (inner.isObject())
                obj.insert(QStringLiteral("result"), inner.object());
            else if (inner.isArray())
                obj.insert(QStringLiteral("result"), inner.array());
            else
                obj.insert(QStringLiteral("result"), resultEjson);
            return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        }

        QString encodeError(quint32 id, const QString &message)
        {
            const QJsonObject obj{{"id", static_cast<double>(id)},
                                  {"ok", false},
                                  {"error", message}};
            return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
        }

        bool decodeResponse(const QString &frame, quint32 expectedId,
                            QString *resultEjson, QString *error,
                            qint64 *gotId)
        {
            if (gotId)
                *gotId = -1;
            const QJsonDocument doc = QJsonDocument::fromJson(frame.toUtf8());
            if (!doc.isObject()) {
                if (error)
                    *error = QStringLiteral("Malformed JSON frame.");
                return false;
            }
            const QJsonObject obj = doc.object();
            const QJsonValue idVal = obj.value(QStringLiteral("id"));
            const double got = idVal.toDouble(-1.0);
            if (gotId && idVal.isDouble() && got >= 0 && got <= 0xFFFFFFFF
                && qFloor(got) == got)
                *gotId = static_cast<qint64>(got);
            if (!idVal.isDouble() || qFloor(got) != got
                || got != static_cast<double>(expectedId)) {
                if (error)
                    *error = QStringLiteral("Response id mismatch.");
                return false;
            }
            if (!obj.value(QStringLiteral("ok")).toBool()) {
                if (error)
                    *error = obj.value(QStringLiteral("error")).toString();
                return false;
            }
            const QJsonValue result = obj.value(QStringLiteral("result"));
            if (result.isObject())
                *resultEjson = QString::fromUtf8(
                    QJsonDocument(result.toObject()).toJson(QJsonDocument::Compact));
            else if (result.isArray())
                *resultEjson = QString::fromUtf8(
                    QJsonDocument(result.toArray()).toJson(QJsonDocument::Compact));
            else
                *resultEjson = result.toString();
            return true;
        }
    }
}
