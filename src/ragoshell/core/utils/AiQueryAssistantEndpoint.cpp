// Base addresses of the OpenAI-compatible provider: presets, the address
// check and the origin a saved key belongs to (docs: B6, threats T1-T3, T12).
#include "ragoshell/core/utils/AiQueryAssistantInternal.h"

#include <algorithm>

#include <QHostAddress>
#include <QRegularExpression>
#include <QUrl>

#include "ragoshell/core/utils/LocalNetwork.h"

namespace RagoShell::AiQueryAssistant
{
    namespace
    {
        bool isLoopbackHost(const QString &host)
        {
            if (host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
                return true;
            QHostAddress address;
            return address.setAddress(host) && address.isLoopback();
        }

        // Plain http beyond loopback: only a literal private address or a
        // .local (mDNS) name. A single-label name resolves through DNS search
        // suffixes or LLMNR and could be answered by another host.
        bool isPrivateHttpHost(const QString &host)
        {
            QHostAddress address;
            if (address.setAddress(host)) {
                // 100.64.0.0/10 (carrier-grade NAT) is shared with other
                // customers of the provider: not "your" network.
                if (address.isInSubnet(QHostAddress(QStringLiteral("100.64.0.0")), 10))
                    return false;
                return isLocalNetworkHost(host);
            }
            return host.endsWith(QLatin1String(".local"), Qt::CaseInsensitive) && isLocalNetworkHost(host);
        }

        QString stripTrailingSlashes(QString path)
        {
            while (path.endsWith(QLatin1Char('/')))
                path.chop(1);
            return path;
        }
    }

    bool hasConfigurableBaseUrl(Provider provider)
    {
        return provider == Provider::OpenAiCompatible || provider == Provider::Ollama;
    }

    bool usesOpenAiApi(Provider provider)
    {
        return provider == Provider::OpenAiCompatible || provider == Provider::Ollama;
    }

    bool usesApiKey(Provider provider)
    {
        return provider != Provider::Ollama;
    }

    QString defaultBaseUrl(Provider provider)
    {
        return provider == Provider::Ollama ? QStringLiteral("http://localhost:11434/v1") : QString();
    }

    HostScope hostScope(const QString &url)
    {
        const QString host = QUrl(url, QUrl::StrictMode).host();
        if (host.isEmpty())
            return HostScope::Internet;
        if (isLoopbackHost(host))
            return HostScope::ThisDevice;
        return isPrivateHttpHost(host) ? HostScope::LocalNetwork : HostScope::Internet;
    }

    int defaultTimeoutSec(Provider provider)
    {
        return provider == Provider::Ollama ? 180 : 120;
    }

    int clampTimeoutSec(int seconds)
    {
        return std::clamp(seconds, kMinTimeoutSec, kMaxTimeoutSec);
    }

    QList<Preset> openAiPresets()
    {
        // Addresses checked against the providers' documentation on
        // 2026-11-22. LM Studio's local server needs no key.
        return {
            {QStringLiteral("openai"), QStringLiteral("OpenAI"), QStringLiteral("https://api.openai.com/v1"),
             QStringLiteral("https://platform.openai.com/api-keys"), {QStringLiteral("OPENAI_API_KEY")}},
            {QStringLiteral("openrouter"), QStringLiteral("OpenRouter"), QStringLiteral("https://openrouter.ai/api/v1"),
             QStringLiteral("https://openrouter.ai/settings/keys"), {QStringLiteral("OPENROUTER_API_KEY")}},
            {QStringLiteral("deepseek"), QStringLiteral("DeepSeek"), QStringLiteral("https://api.deepseek.com"),
             QStringLiteral("https://platform.deepseek.com/api_keys"), {QStringLiteral("DEEPSEEK_API_KEY")}},
            {QStringLiteral("lmstudio"), QStringLiteral("LM Studio"), QStringLiteral("http://localhost:1234/v1"),
             QString(), {}},
        };
    }

    const Preset *presetForBaseUrl(const QString &baseUrl)
    {
        static const QList<Preset> presets = openAiPresets();
        for (const Preset &p : presets)
            if (p.baseUrl == baseUrl)
                return &p;
        return nullptr;
    }

    BaseUrlCheck checkBaseUrl(const QString &input, bool hasKey, bool allowPrivateHttp)
    {
        BaseUrlCheck c;
        const QString text = input.trimmed();
        if (text.isEmpty())
            return c;   // Empty
        if (text.size() > kMaxBaseUrlLength) {
            c.error = BaseUrlError::TooLong;
            return c;
        }
        for (const QChar ch : text) {
            if (ch.unicode() < 0x20 || ch.unicode() == 0x7f || ch.isSpace()) {
                c.error = BaseUrlError::Malformed;
                return c;
            }
        }

        QUrl url(text, QUrl::StrictMode);
        const QString scheme = url.scheme().toLower();
        if (!url.isValid() || url.isRelative() || (scheme != QLatin1String("https") && scheme != QLatin1String("http"))
            || url.host().isEmpty()) {
            c.error = BaseUrlError::Malformed;
            return c;
        }
        if (!url.userName().isEmpty() || !url.password().isEmpty() || text.contains(QLatin1Char('@'))) {
            c.error = BaseUrlError::HasCredentials;
            return c;
        }
        if (url.hasQuery() || url.hasFragment()) {
            c.error = BaseUrlError::HasQueryOrFragment;
            return c;
        }

        if (scheme == QLatin1String("http")) {
            const QString host = url.host();
            if (!isLoopbackHost(host)) {
                if (!isPrivateHttpHost(host))
                    c.error = BaseUrlError::PlainHttpPublic;
                else if (!allowPrivateHttp)
                    c.error = BaseUrlError::PlainHttpBlocked;
                else if (hasKey)
                    c.error = BaseUrlError::PlainHttpWithKey;
                if (c.error != BaseUrlError::Empty)
                    return c;
            }
        }

        url.setScheme(scheme);
        url.setPath(stripTrailingSlashes(url.path()));
        c.normalized = url.toString(QUrl::FullyEncoded);
        c.error = BaseUrlError::None;
        return c;
    }

    QString urlOrigin(const QString &url)
    {
        const QUrl u(url, QUrl::StrictMode);
        const QString scheme = u.scheme().toLower();
        if (!u.isValid() || u.host().isEmpty() || (scheme != QLatin1String("https") && scheme != QLatin1String("http")))
            return {};
        const int port = u.port(scheme == QLatin1String("https") ? 443 : 80);
        const QString host = u.host(QUrl::FullyEncoded).toLower();
        return QStringLiteral("%1://%2:%3")
            .arg(scheme, host.contains(QLatin1Char(':')) ? QLatin1Char('[') + host + QLatin1Char(']') : host)
            .arg(port);
    }

    QString urlHost(const QString &url)
    {
        // A look-alike internationalized name is shown as its punycode
        // ("xn--…") form, so "data goes to" cannot pass for another host.
        const QUrl u(url, QUrl::StrictMode);
        const QString encoded = u.host(QUrl::FullyEncoded);
        return encoded.contains(QLatin1String("xn--")) ? encoded : u.host();
    }

    QString redactSecrets(const QString &text)
    {
        // Key-looking runs (sk-…, AIza…, Bearer …, any long token) never reach
        // the log, even when a server echoes the key in an error.
        static const QRegularExpression keys(
            QStringLiteral("(Bearer\\s+\\S+|\\b(?:sk|pk|rk)[-_][-_A-Za-z0-9]{6,}|\\bAIza[-_A-Za-z0-9]{20,}|[A-Za-z0-9_\\-]{32,})"));
        QString out = text;
        out.replace(keys, QStringLiteral("[redacted]"));
        return out;
    }
}
