#pragma once

#include <QJsonObject>
#include <QString>

#include "ragoshell/core/utils/AiQueryAssistant.h"

namespace RagoShell::AiQueryAssistant::detail
{
    inline constexpr int kMaxFields = 300;
    inline constexpr int kMaxValuesPerField = 5;
    inline constexpr int kMaxValueLength = 60;
    inline constexpr int kMaxQuestionLength = 4000;
    inline constexpr int kMaxTokens = 16000;
    /// OpenAI-compatible servers reject max_tokens above the model's output
    /// limit (often 4096-8192); the JSON answer is short.
    inline constexpr int kCompatMaxTokens = 4096;

    inline constexpr int kRateLimitCooldownSec = 60;
    inline constexpr int kOverloadCooldownSec = 60;
    inline constexpr int kCreditCooldownSec = 3600;
    inline constexpr int kUnavailableCooldownSec = 12 * 3600;

    bool supportsFallback(const QString &model);
    QString baseUrl(Provider provider);
    QJsonObject outputSchema();
    QJsonObject geminiOutputSchema();
}
