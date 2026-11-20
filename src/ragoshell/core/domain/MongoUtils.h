#pragma once

#include <QString>

namespace RagoShell
{
    namespace MongoUtils
    {
        QString buildNiceSizeString(double sizeBytes);
        std::string buildPasswordHash(const std::string &username, const std::string &password);
    }
}
