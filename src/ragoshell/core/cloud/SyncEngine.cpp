#include "ragoshell/core/cloud/SyncEngine.h"

#include "ragoshell/core/cloud/SyncCollections.h"
#include "ragoshell/core/settings/QueryHistoryStore.h"
#include "ragoshell/core/settings/SettingsManager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>

namespace RagoShell
{
namespace
{
    constexpr int kRequestTimeoutMs = 20000;
    constexpr int kMaxResponseBytes = 1600 * 1024; // mirrors server MaxSyncPutBodyBytes
    constexpr int kMax412Retries = 1;              // merge-once-and-retry per SyncProtocol.md

    QString syncStatePath()
    {
        return configDirectory() + QStringLiteral("sync-state.json");
    }

    QString parseErrorCode(const QByteArray &body)
    {
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(body, &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            return {};
        return doc.object()
            .value(QStringLiteral("error")).toObject()
            .value(QStringLiteral("code")).toString();
    }

    QVariantMap kdfParamsToMap(const SyncCrypto::KdfParams &p)
    {
        QVariantMap m;
        m.insert(QStringLiteral("m"), static_cast<qint64>(p.memoryKiB));
        m.insert(QStringLiteral("t"), static_cast<qint64>(p.iterations));
        m.insert(QStringLiteral("p"), static_cast<qint64>(p.parallelism));
        m.insert(QStringLiteral("salt"), QString::fromLatin1(p.salt.toBase64()));
        return m;
    }

    /// Persisted vault params; false when missing or outside the inbound-envelope bounds.
    /// Never invents a salt: params published next to a key they do not derive would make
    /// every later join fail (docs/SyncClient.md §10 R7).
    bool kdfParamsFromMap(const QVariantMap &m, SyncCrypto::KdfParams *out)
    {
        if (m.isEmpty())
            return false;
        SyncCrypto::KdfParams p;
        p.memoryKiB = static_cast<quint32>(m.value(QStringLiteral("m")).toLongLong());
        p.iterations = static_cast<quint32>(m.value(QStringLiteral("t")).toLongLong());
        p.parallelism = static_cast<quint32>(m.value(QStringLiteral("p")).toLongLong());
        p.salt = QByteArray::fromBase64(m.value(QStringLiteral("salt")).toString().toLatin1());
        return SyncCrypto::parseKdfParams(SyncCrypto::buildKdfParams(p), out);
    }

    const char *const kOversizedProperty = "ragoSyncOversized";

    /// Aborts a reply once more than kMaxResponseBytes are buffered, so a hostile or broken
    /// server cannot grow the read buffer without bound (docs/SyncClient.md §10 R12).
    void capReply(QNetworkReply *reply)
    {
        QObject::connect(reply, &QNetworkReply::readyRead, reply, [reply]() {
            if (reply->bytesAvailable() > kMaxResponseBytes)
            {
                reply->setProperty(kOversizedProperty, true);
                reply->abort();
            }
        });
    }

    /// Body of a finished reply; false when it was cut off by capReply() or is over the cap.
    bool readCapped(QNetworkReply *reply, QByteArray *body)
    {
        body->clear();
        if (reply->property(kOversizedProperty).toBool())
            return false;
        *body = reply->readAll();
        return body->size() <= kMaxResponseBytes;
    }

    /// Collections whose merge rewrites the QueryHistoryStore.
    bool touchesHistory(const QString &collection)
    {
        return collection == QLatin1String("snippets")
               || collection == QLatin1String("query_history");
    }

    QString quotedEtag(const QString &raw)
    {
        // Ensure server-format "\"<hex>\"" quoting for If-Match / If-None-Match headers.
        if (raw.startsWith(QLatin1Char('"')) && raw.endsWith(QLatin1Char('"')))
            return raw;
        return QStringLiteral("\"%1\"").arg(raw);
    }
}

SyncEngine::SyncEngine(CloudClient *cloud,
                       SettingsManager *settings,
                       QueryHistoryStore *history,
                       std::shared_ptr<ICloudCredentialStore> credentialStore,
                       QObject *parent)
    : QObject(parent),
      m_cloud(cloud),
      m_settings(settings),
      m_history(history),
      m_creds(std::move(credentialStore))
{
    loadSyncState();
    setState(syncEnabled() ? State::Idle : State::Disabled);
    if (m_cloud)
        connect(m_cloud, &CloudClient::signedOut, this, [this]() {
            if (m_state == State::Syncing)
                finishRun(false, QStringLiteral("unauthorized"));
            setState(syncEnabled() ? State::Idle : State::Disabled);
        });
}

bool SyncEngine::syncEnabled() const
{
    return m_settings && m_settings->syncEnabled();
}

QStringList SyncEngine::enabledCollections() const
{
    return m_settings ? m_settings->syncCollections() : QStringList();
}

bool SyncEngine::passwordsIncluded() const
{
    return m_settings && m_settings->syncIncludePasswords();
}

QString SyncEngine::keyMode() const
{
    return m_settings ? m_settings->syncKeyMode() : QString();
}

bool SyncEngine::hasConfiguredKey() const
{
    return !keyMode().isEmpty();
}

qint64 SyncEngine::lastSyncAtMs() const
{
    return m_lastSyncAtMs;
}

QString SyncEngine::deviceLabel() const
{
    if (m_cloud && !m_cloud->deviceName().isEmpty())
        return m_cloud->deviceName();
    return CloudClient::defaultDeviceName();
}

qint64 SyncEngine::nowMs() const
{
    return QDateTime::currentMSecsSinceEpoch();
}

void SyncEngine::setState(State s)
{
    if (m_state == s)
        return;
    m_state = s;
    emit stateChanged();
}

void SyncEngine::setCollectionStatus(const QString &collection, const QString &status,
                                     const QString &message)
{
    QVariantMap row;
    row.insert(QStringLiteral("status"), status);
    if (!message.isEmpty())
        row.insert(QStringLiteral("message"), message);
    m_collectionStatus.insert(collection, row);
}

QByteArray SyncEngine::masterKey(QString *error)
{
    if (!m_creds || !m_creds->isAvailable())
    {
        if (error) *error = QStringLiteral("keystore_unavailable");
        return {};
    }
    QString encoded;
    QString readErr;
    if (!m_creds->readSecret(QString::fromLatin1(ICloudCredentialStore::kSyncMasterKeySlot),
                             &encoded, &readErr) || encoded.isEmpty())
    {
        if (error) *error = QStringLiteral("sync_key_locked");
        return {};
    }
    const QByteArray key = QByteArray::fromBase64(encoded.toLatin1());
    if (key.size() != SyncCrypto::kMasterKeyBytes)
    {
        if (error) *error = QStringLiteral("sync_key_corrupt");
        return {};
    }
    return key;
}

bool SyncEngine::keyAvailable()
{
    QString err;
    QByteArray key = masterKey(&err);
    const bool ok = !key.isEmpty();
    SyncCrypto::cleanse(&key);
    return ok;
}

bool SyncEngine::configurePassphrase(const QString &passphrase, QString *error)
{
    if (passphrase.size() < 8)
    {
        if (error) *error = QStringLiteral("passphrase_too_short");
        return false;
    }
    if (!m_creds || !m_creds->isAvailable())
    {
        if (error) *error = QStringLiteral("keystore_unavailable");
        return false;
    }

    SyncCrypto::KdfParams params;
    params.salt = SyncCrypto::generateSalt();
    m_kdfParams = kdfParamsToMap(params);
    QByteArray key = SyncCrypto::deriveMasterKey(passphrase, params);
    if (key.isEmpty())
    {
        if (error) *error = QStringLiteral("kdf_failed");
        return false;
    }
    QString writeErr;
    const bool stored = m_creds->writeSecret(
        QString::fromLatin1(ICloudCredentialStore::kSyncMasterKeySlot),
        QString::fromLatin1(key.toBase64()), &writeErr);
    SyncCrypto::cleanse(&key);
    if (!stored)
    {
        if (error) *error = QStringLiteral("keystore_write_failed");
        return false;
    }
    if (m_settings)
    {
        m_settings->setSyncKeyMode(QStringLiteral("passphrase"));
        m_settings->setSyncEnabled(true);
        if (m_settings->syncCollections().isEmpty())
            m_settings->setSyncCollections(SyncCollections::knownCollections());
        m_settings->save();
    }
    saveSyncState();
    setState(State::Idle);
    return true;
}

bool SyncEngine::configureDeviceKey(QString *error)
{
    if (!m_creds || !m_creds->isAvailable())
    {
        if (error) *error = QStringLiteral("keystore_unavailable");
        return false;
    }
    const QByteArray key = SyncCrypto::generateDeviceKey();
    if (key.isEmpty())
    {
        if (error) *error = QStringLiteral("rng_failed");
        return false;
    }
    SyncCrypto::KdfParams params;
    params.salt = SyncCrypto::generateSalt();
    m_kdfParams = kdfParamsToMap(params);
    QString writeErr;
    const bool stored = m_creds->writeSecret(
        QString::fromLatin1(ICloudCredentialStore::kSyncMasterKeySlot),
        QString::fromLatin1(key.toBase64()), &writeErr);
    if (!stored)
    {
        if (error) *error = QStringLiteral("keystore_write_failed");
        return false;
    }
    if (m_settings)
    {
        m_settings->setSyncKeyMode(QStringLiteral("device"));
        m_settings->setSyncEnabled(true);
        if (m_settings->syncCollections().isEmpty())
            m_settings->setSyncCollections(SyncCollections::knownCollections());
        m_settings->save();
    }
    saveSyncState();
    setState(State::Idle);
    return true;
}

void SyncEngine::setCollections(const QStringList &collections, bool includePasswords)
{
    if (!m_settings)
        return;
    QStringList filtered;
    for (const QString &c : collections)
        if (SyncCollections::isKnownCollection(c))
            filtered.append(c);
    m_settings->setSyncCollections(filtered);
    m_settings->setSyncIncludePasswords(includePasswords && filtered.contains(QStringLiteral("connections")));
    m_settings->save();
}

void SyncEngine::disable()
{
    if (m_creds && m_creds->isAvailable())
        m_creds->removeSecret(QString::fromLatin1(ICloudCredentialStore::kSyncMasterKeySlot));
    if (m_settings)
    {
        m_settings->setSyncEnabled(false);
        m_settings->setSyncKeyMode(QString());
        m_settings->save();
    }
    // Replies of a run still in flight must not touch the state below (§10 R10).
    ++m_runGeneration;
    m_runIndex = -1;
    m_runQueue.clear();
    m_accessToken.clear();
    m_retryTimer.stop();
    m_retryBackoffMs = 0;
    m_offline = false;
    m_states.clear();
    m_kdfParams.clear();
    m_collectionStatus.clear();
    saveSyncState();
    setState(State::Disabled);
}

void SyncEngine::syncNow()
{
    if (!syncEnabled() || m_state == State::Syncing || m_wipeInFlight)
        return;
    if (!m_cloud || !m_cloud->isSignedIn())
    {
        m_lastError = QStringLiteral("unauthorized");
        setState(State::Error);
        return;
    }
    if (!keyAvailable())
    {
        m_lastError = QStringLiteral("sync_key_locked");
        setState(State::Error);
        return;
    }
    m_conflictLabels.clear();
    m_retried412 = 0;
    setState(State::Syncing);
    emit syncStarted();
    startManifestFetch();
}

void SyncEngine::wipeRemote()
{
    // A running sync would re-create collections right after the DELETE (§10 R10).
    if (m_wipeInFlight || m_state == State::Syncing || !m_cloud || !m_cloud->isSignedIn())
    {
        emit wipeFinished(false);
        return;
    }
    m_wipeInFlight = true;
    m_cloud->withValidAccessToken([this](bool ok, const QString &token, const QString &) {
        if (!ok)
        {
            m_wipeInFlight = false;
            emit wipeFinished(false);
            return;
        }
        QNetworkRequest req = buildRequest(QStringLiteral("/v1/sync"), token);
        QNetworkReply *reply = m_nam.deleteResource(req);
        capReply(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply]() {
            reply->deleteLater();
            m_wipeInFlight = false;
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const bool ok = (status == 204 || status == 404)
                            && reply->error() == QNetworkReply::NoError;
            if (ok)
            {
                m_states.clear();
                m_collectionStatus.clear();
                saveSyncState();
            }
            emit wipeFinished(ok);
        });
    });
}

QNetworkRequest SyncEngine::buildRequest(const QString &path, const QString &accessToken) const
{
    QNetworkRequest req(QUrl(m_cloud->effectiveApiUrl() + path));
    req.setRawHeader("Authorization", QByteArray("Bearer ") + accessToken.toUtf8());
    req.setRawHeader("Accept", "application/json");
    req.setTransferTimeout(kRequestTimeoutMs);
    return req;
}

void SyncEngine::startManifestFetch()
{
    const quint64 gen = m_runGeneration;
    m_cloud->withValidAccessToken([this, gen](bool ok, const QString &token, const QString &code) {
        if (gen != m_runGeneration)
            return; // the run ended (sign-out, disable) while the token was refreshing
        if (!ok)
        {
            if (code == QLatin1String("network_error"))
                finishOffline(QStringLiteral("offline"));
            else
                finishRun(false, code.isEmpty() ? QStringLiteral("unauthorized") : code);
            return;
        }
        m_accessToken = token;
        QNetworkReply *reply = m_nam.get(buildRequest(QStringLiteral("/v1/sync"), token));
        capReply(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, gen]() {
            reply->deleteLater();
            if (gen != m_runGeneration)
                return;
            const int status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (status == 0 && reply->error() != QNetworkReply::NoError)
            {
                finishOffline(QStringLiteral("offline"));
                return;
            }
            QByteArray body;
            const bool fits = readCapped(reply, &body);
            handleManifest(status, fits ? body : QByteArray("OVERSIZED"));
        });
    });
}

void SyncEngine::handleManifest(int status, const QByteArray &body)
{
    if (body == QByteArray("OVERSIZED"))
    {
        finishRun(false, QStringLiteral("response_too_large"));
        return;
    }
    if (status == 401)
    {
        finishRun(false, QStringLiteral("unauthorized"));
        return;
    }
    if (status != 200)
    {
        finishRun(false, QStringLiteral("manifest_http_%1").arg(status));
        return;
    }
    const QJsonObject manifest = QJsonDocument::fromJson(body).object();
    m_writable = manifest.value(QStringLiteral("writable")).toBool();
    m_maxCollections = manifest.value(QStringLiteral("max_collections")).toInt();
    m_maxBlobBytes = manifest.value(QStringLiteral("max_blob_bytes")).toInt();

    m_runQueue = enabledCollections();
    if (m_runQueue.isEmpty())
    {
        finishRun(true);
        return;
    }
    m_runIndex = 0;
    advanceCollection();
}

void SyncEngine::advanceCollection()
{
    if (m_runIndex < 0 || m_runIndex >= m_runQueue.size())
    {
        finishRun(true);
        return;
    }
    const QString collection = m_runQueue.at(m_runIndex);
    QNetworkRequest req =
        buildRequest(QStringLiteral("/v1/sync/%1").arg(collection), m_accessToken);
    const QString savedEtag = m_states.value(collection).etag;
    if (!savedEtag.isEmpty())
        req.setRawHeader("If-None-Match", quotedEtag(savedEtag).toUtf8());
    QNetworkReply *reply = m_nam.get(req);
    capReply(reply);
    const quint64 gen = m_runGeneration;
    connect(reply, &QNetworkReply::finished, this, [this, reply, collection, gen]() {
        reply->deleteLater();
        if (gen != m_runGeneration)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString etag =
            QString::fromLatin1(reply->rawHeader("ETag"));
        if (status == 0 && reply->error() != QNetworkReply::NoError)
        {
            finishOffline(QStringLiteral("offline"));
            return;
        }
        QByteArray got;
        const bool fits = readCapped(reply, &got);
        handleCollectionGet(collection, status, etag, fits ? got : QByteArray("OVERSIZED"));
    });
}

void SyncEngine::handleCollectionGet(const QString &collection, int status,
                                     const QString &etagHeader, const QByteArray &body)
{
    if (status == 304)
    {
        // Remote is unchanged — but local edits since our last push still need
        // uploading. Compare fresh export fingerprints against persisted state.
        QString xerr;
        QVariantMap fresh;
        QByteArray localDoc = SyncCollections::exportPayload(
            collection, m_settings, m_history, passwordsIncluded(),
            m_cloud->deviceId(), deviceLabel(),
            m_states.value(collection).entries, &fresh, nowMs(), &xerr);
        if (localDoc.isEmpty())
        {
            setCollectionStatus(collection, QStringLiteral("error"),
                                xerr.isEmpty() ? QStringLiteral("export_failed") : xerr);
            ++m_runIndex;
            advanceCollection();
            return;
        }
        if (fresh == m_states.value(collection).entries)
        {
            setCollectionStatus(collection, QStringLiteral("ok"));
            emit collectionSynced(collection);
            ++m_runIndex;
            advanceCollection();
            return;
        }
        putCollection(collection, localDoc,
                      m_states.value(collection).etag, false, false);
        SyncCrypto::cleanse(&localDoc);
        return;
    }
    if (body == QByteArray("OVERSIZED"))
    {
        setCollectionStatus(collection, QStringLiteral("error"),
                            QStringLiteral("response_too_large"));
        ++m_runIndex;
        advanceCollection();
        return;
    }
    if (status == 401)
    {
        finishRun(false, QStringLiteral("unauthorized"));
        return;
    }

    QString merr;
    QByteArray mkey = masterKey(&merr);
    if (mkey.isEmpty())
    {
        finishRun(false, merr);
        return;
    }

    QString err;
    QByteArray localDoc = SyncCollections::exportPayload(
        collection, m_settings, m_history, passwordsIncluded(),
        m_cloud->deviceId(), deviceLabel(),
        m_states.value(collection).entries, nullptr, nowMs(), &err);
    if (localDoc.isEmpty())
    {
        SyncCrypto::cleanse(&mkey);
        setCollectionStatus(collection, QStringLiteral("error"),
                            err.isEmpty() ? QStringLiteral("export_failed") : err);
        ++m_runIndex;
        advanceCollection();
        return;
    }

    if (status == 404)
    {
        // Create-only push (If-None-Match: *).
        putCollection(collection, localDoc, QString(), true, false);
        SyncCrypto::cleanse(&localDoc);
        SyncCrypto::cleanse(&mkey);
        return;
    }
    if (status != 200)
    {
        SyncCrypto::cleanse(&mkey);
        setCollectionStatus(collection, QStringLiteral("error"),
                            QStringLiteral("get_http_%1").arg(status));
        ++m_runIndex;
        advanceCollection();
        return;
    }

    // Pull path: decrypt envelope → merge → maybe push merged result.
    const QJsonObject env = QJsonDocument::fromJson(body).object();
    const QByteArray ciphertext =
        QByteArray::fromBase64(env.value(QStringLiteral("ciphertext")).toString().toLatin1());
    const QByteArray nonce =
        QByteArray::fromBase64(env.value(QStringLiteral("nonce")).toString().toLatin1());
    const QJsonObject kdf = env.value(QStringLiteral("kdf_params")).toObject();
    if (ciphertext.isEmpty() || nonce.isEmpty() || kdf.isEmpty())
    {
        SyncCrypto::cleanse(&mkey);
        setCollectionStatus(collection, QStringLiteral("error"),
                            QStringLiteral("envelope_malformed"));
        ++m_runIndex;
        advanceCollection();
        return;
    }

    const QByteArray collKey = SyncCrypto::deriveCollectionKey(mkey, collection);
    SyncCrypto::cleanse(&mkey);
    QByteArray plaintext;
    QString derr;
    if (collKey.isEmpty()
        || !SyncCrypto::decryptEnvelope(collKey, collection, ciphertext, nonce,
                                        &plaintext, &derr))
    {
        setCollectionStatus(collection, QStringLiteral("error"),
                            QStringLiteral("decrypt_failed"));
        ++m_runIndex;
        advanceCollection();
        return;
    }

    // kdf_params are pinned at provisioning (configure*/attachToRemoteVault) and
    // are not covered by the GCM tag — never adopt remote params on the pull path
    // (a hostile server could desync every passphrase device).

    auto outcome = SyncCollections::merge(collection, plaintext, localDoc,
                                        m_settings, m_history,
                                        m_cloud->deviceId(), deviceLabel(), nowMs());
    SyncCrypto::cleanse(&plaintext);
    SyncCrypto::cleanse(&localDoc);
    if (!outcome.ok)
    {
        setCollectionStatus(collection, QStringLiteral("error"),
                            outcome.error.isEmpty() ? QStringLiteral("merge_failed")
                                                    : outcome.error);
        ++m_runIndex;
        advanceCollection();
        return;
    }
    if (!outcome.conflictLabels.isEmpty())
        m_conflictLabels.append(outcome.conflictLabels);

    if (outcome.localChanged && m_settings)
        m_settings->save();
    // Owners of the history store rebuild their views and save it now, before
    // sync-state.json records the pulled keys (docs/SyncClient.md §10 R1).
    if (outcome.localChanged && touchesHistory(collection))
        emit historyChanged();

    if (!outcome.remoteNeedsUpdate)
    {
        // Nothing to push: the pulled etag and the merged local state are final.
        commitCollectionState(collection, etagHeader);
        setCollectionStatus(collection, QStringLiteral("ok"));
        emit collectionSynced(collection);
        ++m_runIndex;
        advanceCollection();
        return;
    }
    // The etag is committed by handlePutResult() only once the merged PUT landed: after a
    // failed PUT the next run must pull and merge again instead of getting a 304 and never
    // pushing the local-only rows (docs/SyncClient.md §10 R3).
    putCollection(collection, outcome.mergedPayload, etagHeader, false, false);
}

void SyncEngine::commitCollectionState(const QString &collection, const QString &etag)
{
    CollectionState &st = m_states[collection];
    st.etag = etag;
    QVariantMap fresh;
    QString xerr;
    SyncCollections::exportPayload(collection, m_settings, m_history, passwordsIncluded(),
                                   m_cloud->deviceId(), deviceLabel(),
                                   st.entries, &fresh, nowMs(), &xerr);
    st.entries = fresh;
}

void SyncEngine::putCollection(const QString &collection, const QByteArray &payloadDoc,
                               const QString &ifMatchEtag, bool createOnly, bool retried)
{
    // Vault-wide kdf_params pinned at configure/attach time — every device sharing the
    // passphrase must re-derive the identical master key from what we publish.
    SyncCrypto::KdfParams params;
    if (!kdfParamsFromMap(m_kdfParams, &params))
    {
        if (keyMode() != QLatin1String("device"))
        {
            // Passphrase vault: never publish made-up params next to the real key
            // (docs/SyncClient.md §10 R7). Recovery: disable sync and join the vault again.
            finishRun(false, QStringLiteral("kdf_params_missing"));
            return;
        }
        // Device-key vault: the params derive nothing; mint a valid set and keep it.
        params = SyncCrypto::KdfParams{};
        params.salt = SyncCrypto::generateSalt();
        m_kdfParams = kdfParamsToMap(params);
        saveSyncState();
    }

    QString merr;
    QByteArray mkey = masterKey(&merr);
    if (mkey.isEmpty())
    {
        finishRun(false, merr);
        return;
    }

    const QByteArray collKey = SyncCrypto::deriveCollectionKey(mkey, collection);
    SyncCrypto::cleanse(&mkey);
    QByteArray ciphertext, nonce;
    QString eerr;
    if (collKey.isEmpty()
        || !SyncCrypto::encryptEnvelope(collKey, collection, payloadDoc,
                                        &ciphertext, &nonce, &eerr))
    {
        setCollectionStatus(collection, QStringLiteral("error"),
                            QStringLiteral("encrypt_failed"));
        ++m_runIndex;
        advanceCollection();
        return;
    }

    QJsonObject wire;
    wire.insert(QStringLiteral("ciphertext"), QString::fromLatin1(ciphertext.toBase64()));
    wire.insert(QStringLiteral("nonce"), QString::fromLatin1(nonce.toBase64()));
    wire.insert(QStringLiteral("kdf_params"), SyncCrypto::buildKdfParams(params));

    QNetworkRequest req =
        buildRequest(QStringLiteral("/v1/sync/%1").arg(collection), m_accessToken);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (createOnly)
        req.setRawHeader("If-None-Match", "*");
    else
        req.setRawHeader("If-Match", quotedEtag(ifMatchEtag).toUtf8());

    const QByteArray body = QJsonDocument(wire).toJson(QJsonDocument::Compact);
    // max_blob_bytes limits the decoded ciphertext server-side, not the base64 JSON body
    // (docs/SyncClient.md §10 R12).
    if (m_maxBlobBytes > 0 && ciphertext.size() > m_maxBlobBytes)
    {
        setCollectionStatus(collection, QStringLiteral("error"),
                            QStringLiteral("sync_blob_too_large"));
        ++m_runIndex;
        advanceCollection();
        return;
    }
    m_lastSentEnvelope = body;
    QNetworkReply *reply = m_nam.put(req, body);
    capReply(reply);
    const quint64 gen = m_runGeneration;
    connect(reply, &QNetworkReply::finished, this,
            [this, reply, collection, payloadDoc, createOnly, retried, gen]() {
        reply->deleteLater();
        if (gen != m_runGeneration)
            return;
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QString etag = QString::fromLatin1(reply->rawHeader("ETag"));
        if (status == 0 && reply->error() != QNetworkReply::NoError)
        {
            finishOffline(QStringLiteral("offline"));
            return;
        }
        QByteArray resBody; // only parsed for an error code; dropped when over the cap
        if (!readCapped(reply, &resBody))
            resBody.clear();
        handlePutResult(collection, status, etag, resBody, createOnly, retried, payloadDoc);
    });
}

void SyncEngine::handlePutResult(const QString &collection, int status, const QString &etagHeader,
                                 const QByteArray &body, bool createOnly, bool retried,
                                 const QByteArray &payloadDoc)
{
    if (status == 200 || status == 201)
    {
        commitCollectionState(collection, etagHeader);
        setCollectionStatus(collection, QStringLiteral("ok"));
        emit collectionSynced(collection);
        ++m_runIndex;
        advanceCollection();
        return;
    }

    const QString code = parseErrorCode(body);
    if (status == 403 || code == QLatin1String("sync_entitlement_required"))
    {
        markBlocked(QStringLiteral("sync_entitlement_required"));
        return;
    }
    if (status == 401)
    {
        finishRun(false, QStringLiteral("unauthorized"));
        return;
    }
    if (status == 409 || code == QLatin1String("sync_collection_limit_reached"))
    {
        setCollectionStatus(collection, QStringLiteral("error"),
                            QStringLiteral("sync_collection_limit_reached"));
        ++m_runIndex;
        advanceCollection();
        return;
    }
    if (status == 413 || code == QLatin1String("sync_blob_too_large")
        || code == QLatin1String("payload_too_large"))
    {
        setCollectionStatus(collection, QStringLiteral("error"),
                            QStringLiteral("sync_blob_too_large"));
        ++m_runIndex;
        advanceCollection();
        return;
    }
    if (status == 412 && !retried && m_retried412 < kMax412Retries)
    {
        // One merge-and-retry: re-GET current remote, merge again, PUT once more.
        ++m_retried412;
        QNetworkRequest req =
            buildRequest(QStringLiteral("/v1/sync/%1").arg(collection), m_accessToken);
        QNetworkReply *reply = m_nam.get(req);
        capReply(reply);
        const quint64 gen = m_runGeneration;
        connect(reply, &QNetworkReply::finished, this, [this, reply, collection, gen]() {
            reply->deleteLater();
            if (gen != m_runGeneration)
                return;
            const int s = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            const QString etag = QString::fromLatin1(reply->rawHeader("ETag"));
            if (s == 0 && reply->error() != QNetworkReply::NoError)
            {
                finishOffline(QStringLiteral("offline"));
                return;
            }
            if (s != 200)
            {
                setCollectionStatus(collection, QStringLiteral("error"),
                                    QStringLiteral("sync_conflict"));
                ++m_runIndex;
                advanceCollection();
                return;
            }
            QByteArray retryBody;
            if (!readCapped(reply, &retryBody))
            {
                setCollectionStatus(collection, QStringLiteral("error"),
                                    QStringLiteral("response_too_large"));
                ++m_runIndex;
                advanceCollection();
                return;
            }
            QString merr;
            QByteArray mkey = masterKey(&merr);
            const QByteArray collKey = mkey.isEmpty()
                ? QByteArray()
                : SyncCrypto::deriveCollectionKey(mkey, collection);
            SyncCrypto::cleanse(&mkey);
            const QJsonObject env = QJsonDocument::fromJson(retryBody).object();
            QByteArray plain;
            QString derr;
            const QByteArray ct = QByteArray::fromBase64(
                env.value(QStringLiteral("ciphertext")).toString().toLatin1());
            const QByteArray nc = QByteArray::fromBase64(
                env.value(QStringLiteral("nonce")).toString().toLatin1());
            QByteArray localDoc = SyncCollections::exportPayload(
                collection, m_settings, m_history, passwordsIncluded(),
                m_cloud->deviceId(), deviceLabel(),
                m_states.value(collection).entries, nullptr, nowMs(), nullptr);
            if (collKey.isEmpty()
                || !SyncCrypto::decryptEnvelope(collKey, collection, ct, nc, &plain, &derr))
            {
                setCollectionStatus(collection, QStringLiteral("error"),
                                    QStringLiteral("decrypt_failed"));
                ++m_runIndex;
                advanceCollection();
                return;
            }
            auto outcome = SyncCollections::merge(collection, plain, localDoc,
                                                  m_settings, m_history,
                                                  m_cloud->deviceId(), deviceLabel(), nowMs());
            SyncCrypto::cleanse(&plain);
            SyncCrypto::cleanse(&localDoc);
            if (!outcome.ok)
            {
                setCollectionStatus(collection, QStringLiteral("error"),
                                    QStringLiteral("merge_failed"));
                ++m_runIndex;
                advanceCollection();
                return;
            }
            if (!outcome.conflictLabels.isEmpty())
                m_conflictLabels.append(outcome.conflictLabels);
            if (outcome.localChanged && m_settings)
                m_settings->save();
            if (outcome.localChanged && touchesHistory(collection))
                emit historyChanged(); // §10 R1, as on the first pull
            putCollection(collection, outcome.mergedPayload, etag, false, true);
        });
        return;
    }

    setCollectionStatus(collection, QStringLiteral("error"),
                        code.isEmpty()
                            ? QStringLiteral("put_http_%1").arg(status)
                            : code);
    ++m_runIndex;
    advanceCollection();
}

void SyncEngine::finishOffline(const QString &what)
{
    m_offline = true;
    finishRun(false, what.isEmpty() ? QStringLiteral("offline") : what);
    scheduleOfflineRetry();
}

void SyncEngine::scheduleOfflineRetry()
{
    // Exponential backoff while sync stays enabled and signed in (30s → 5min cap).
    if (!syncEnabled() || !m_cloud || !m_cloud->isSignedIn())
        return;
    m_retryBackoffMs = m_retryBackoffMs <= 0 ? 30000
                                             : qMin(m_retryBackoffMs * 2, 5 * 60 * 1000);
    m_retryTimer.setSingleShot(true);
    disconnect(&m_retryTimer, nullptr, this, nullptr);
    connect(&m_retryTimer, &QTimer::timeout, this, [this]() {
        if (m_state == State::Error && syncEnabled())
            syncNow();
    });
    m_retryTimer.start(m_retryBackoffMs);
}

void SyncEngine::finishAttach(const QString &error, bool ok)
{
    m_attachPassphrase.fill(QLatin1Char('\0'));
    m_attachPassphrase.clear();
    m_attachError = error;
    m_attachOk = ok;
    m_attachDone = true;
}

void SyncEngine::markBlocked(const QString &reason)
{
    // Ends the run like finishRun(): stale replies are ignored and the state (incl. the
    // kdf params pinned by a join just before) reaches disk (§10 R7, R10).
    ++m_runGeneration;
    m_runIndex = -1;
    m_runQueue.clear();
    m_accessToken.clear();
    m_lastError = reason;
    saveSyncState();
    setState(State::Blocked);
    emit syncFinished(false);
}

void SyncEngine::finishRun(bool ok, const QString &error)
{
    // Replies still in flight for this run are ignored from here on (§10 R10).
    ++m_runGeneration;
    m_runIndex = -1;
    m_runQueue.clear();
    m_accessToken.clear();
    if (!error.isEmpty())
        m_lastError = error;
    if (ok)
    {
        m_lastSyncAtMs = nowMs();
        m_lastError.clear();
        m_offline = false;
        m_retryBackoffMs = 0;
        m_retryTimer.stop();
    }
    else if (m_lastError == QLatin1String("unauthorized"))
    {
        // Revoked/expired session: no retry loop — the device must re-authenticate.
        m_retryTimer.stop();
        m_retryBackoffMs = 0;
    }
    saveSyncState();
    setState(ok ? State::Idle : State::Error);
    if (!m_conflictLabels.isEmpty())
        emit conflictsResolved(m_conflictLabels);
    emit syncFinished(ok);
}

void SyncEngine::loadSyncState()
{
    QFile f(syncStatePath());
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QJsonObject root = QJsonDocument::fromJson(f.readAll()).object();
    m_lastSyncAtMs = root.value(QStringLiteral("lastSyncAtMs")).toInteger(0);
    m_kdfParams =
        root.value(QStringLiteral("kdfParams")).toObject().toVariantMap();
    const QJsonObject cols = root.value(QStringLiteral("collections")).toObject();
    for (auto it = cols.begin(); it != cols.end(); ++it)
    {
        const QJsonObject c = it.value().toObject();
        CollectionState st;
        st.etag = c.value(QStringLiteral("etag")).toString();
        st.entries = c.value(QStringLiteral("entries")).toObject().toVariantMap();
        st.cleared = c.value(QStringLiteral("cleared")).toObject().toVariantMap();
        st.doc = c.value(QStringLiteral("doc")).toObject().toVariantMap();
        m_states.insert(it.key(), st);
    }
}

bool SyncEngine::saveSyncState()
{
    QJsonObject cols;
    for (auto it = m_states.begin(); it != m_states.end(); ++it)
    {
        QJsonObject c;
        c.insert(QStringLiteral("etag"), it.value().etag);
        c.insert(QStringLiteral("entries"),
                 QJsonObject::fromVariantMap(it.value().entries));
        c.insert(QStringLiteral("cleared"),
                 QJsonObject::fromVariantMap(it.value().cleared));
        c.insert(QStringLiteral("doc"), QJsonObject::fromVariantMap(it.value().doc));
        cols.insert(it.key(), c);
    }
    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("lastSyncAtMs"), m_lastSyncAtMs);
    root.insert(QStringLiteral("kdfParams"),
                QJsonObject::fromVariantMap(m_kdfParams));
    root.insert(QStringLiteral("collections"), cols);

    QSaveFile f(syncStatePath());
    if (!f.open(QIODevice::WriteOnly))
        return false;
    f.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    return f.commit();
}

bool SyncEngine::attachToRemoteVault(const QString &passphrase, QString *error)
{
    // Pull one remote envelope, derive the key from its kdf_params, verify by decrypting.
    // Runs synchronously through a nested request chain driven by the caller's event loop.
    if (!m_cloud || !m_cloud->isSignedIn() || !m_creds || !m_creds->isAvailable())
    {
        if (error) *error = QStringLiteral("unauthorized");
        return false;
    }
    if (passphrase.size() < 8)
    {
        if (error) *error = QStringLiteral("passphrase_too_short");
        return false;
    }

    m_attachDone = false;
    m_attachOk = false;
    m_attachError.clear();
    m_attachPassphrase = passphrase;

    m_cloud->withValidAccessToken([this](bool ok, const QString &token, const QString &) {
        if (!ok)
        {
            finishAttach(QStringLiteral("unauthorized"), false);
            return;
        }
        QNetworkReply *reply = m_nam.get(buildRequest(QStringLiteral("/v1/sync"), token));
        capReply(reply);
        connect(reply, &QNetworkReply::finished, this, [this, reply, token]() {
            reply->deleteLater();
            QByteArray mbody;
            const bool fits = readCapped(reply, &mbody);
            if (reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
                || !fits)
            {
                finishAttach(QStringLiteral("manifest_fetch_failed"), false);
                return;
            }
            const QJsonObject manifest =
                QJsonDocument::fromJson(mbody).object();
            const QJsonArray cols = manifest.value(QStringLiteral("collections")).toArray();
            if (cols.isEmpty())
            {
                // No remote vault: configure as a fresh passphrase vault.
                finishAttach(QString(), true); // empty vault: caller falls back to configurePassphrase
                return;
            }
            const QString first =
                cols.first().toObject().value(QStringLiteral("collection")).toString();
            QNetworkReply *get = m_nam.get(
                buildRequest(QStringLiteral("/v1/sync/%1").arg(first), token));
            capReply(get);
            connect(get, &QNetworkReply::finished, this, [this, get, first]() {
                get->deleteLater();
                QByteArray gbody;
                const bool fits = readCapped(get, &gbody);
                if (get->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() != 200
                    || !fits)
                {
                    finishAttach(QStringLiteral("blob_fetch_failed"), false);
                    return;
                }
                const QJsonObject env =
                    QJsonDocument::fromJson(gbody).object();
                SyncCrypto::KdfParams params;
                if (!SyncCrypto::parseKdfParams(
                        env.value(QStringLiteral("kdf_params")).toObject(), &params, &m_attachError))
                {
                        finishAttach(m_attachError, false);
                    return;
                }
                QByteArray key = SyncCrypto::deriveMasterKey(m_attachPassphrase, params);
                const QByteArray collKey = SyncCrypto::deriveCollectionKey(key, first);
                QByteArray plain;
                QString derr;
                const QByteArray ct = QByteArray::fromBase64(
                    env.value(QStringLiteral("ciphertext")).toString().toLatin1());
                const QByteArray nc = QByteArray::fromBase64(
                    env.value(QStringLiteral("nonce")).toString().toLatin1());
                const bool ok = !collKey.isEmpty()
                                && SyncCrypto::decryptEnvelope(collKey, first, ct, nc,
                                                               &plain, &derr);
                SyncCrypto::cleanse(&plain);
                if (!ok)
                {
                    SyncCrypto::cleanse(&key);
                    finishAttach(QStringLiteral("passphrase_mismatch"), false);
                    return;
                }
                QString werr;
                m_attachOk = m_creds->writeSecret(
                    QString::fromLatin1(ICloudCredentialStore::kSyncMasterKeySlot),
                    QString::fromLatin1(key.toBase64()), &werr);
                SyncCrypto::cleanse(&key);
                if (m_attachOk)
                {
                    // Pin the vault's params next to the stored key and put them on disk
                    // now: a crash before the first run would otherwise leave none
                    // (docs/SyncClient.md §10 R7).
                    m_kdfParams = kdfParamsToMap(params);
                    saveSyncState();
                }
                if (m_attachOk && m_settings)
                {
                    m_settings->setSyncKeyMode(QStringLiteral("passphrase"));
                    m_settings->setSyncEnabled(true);
                    if (m_settings->syncCollections().isEmpty())
                        m_settings->setSyncCollections(SyncCollections::knownCollections());
                    m_settings->save();
                }
                finishAttach(m_attachError, m_attachOk);
            });
        });
    });

    // Pump the event loop until the attach chain resolves (wizard calls this modally).
    while (!m_attachDone)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);

    if (m_attachOk)
        setState(State::Idle);
    if (error) *error = m_attachError;
    return m_attachOk;
}

} // namespace RagoShell
