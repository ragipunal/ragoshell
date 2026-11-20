#pragma once

#include <QString>
#include <QVariant>
#include <QVariantMap>

#include <set>
#include <vector>

#include <mongocxx/uri.hpp>

#include "ragoshell/core/domain/HostAndPort.h"

namespace RagoShell
{
    class ReplicaSetSettings
    {
    public:
        // Persisted as int; keep values stable.
        enum class ReadPreference
        {
            PRIMARY             = 0,
            PRIMARY_PREFERRED   = 1,
            SECONDARY           = 2,
            SECONDARY_PREFERRED = 3,
            NEAREST             = 4
        };

        ReplicaSetSettings();
        
        ReplicaSetSettings(const mongocxx::uri& uri);

        /**
         * Clones Replica Set settings.
         */
        ReplicaSetSettings* clone() const;

        /**
         * Converts to QVariantMap
         */
        QVariant toVariant() const;
        void fromVariant(const QVariantMap &map);

        // Getters
        std::string const& setNameUserEntered() { return _setNameUserEntered; }
        std::string const& cachedSetName() { return _cachedSetName; }
        std::vector<std::string> const& members() const { return _members; }
        std::vector<HostAndPort> membersToHostAndPort() const;

        std::set<HostAndPort> membersToHostAndPortAsSet() const
        {
            auto const& members = membersToHostAndPort();
            return std::set<HostAndPort>(members.begin(), members.end());
        }
        
        ReadPreference readPreference() const { return _readPreference; }

        // Setters
        void setSetNameUserEntered(const std::string& setName) { _setNameUserEntered = setName; }
        void setCachedSetName(const std::string& setName) { _cachedSetName = setName; }
        void setMembers(const std::vector<std::string>& members);
        void setMembers(const std::vector<std::pair<std::string,bool>>& membersAndHealts);
        void deleteAllMembers() { _members.clear(); }
        void setReadPreference(ReadPreference readPreference) { _readPreference = readPreference; }


    private:
        std::string _setNameUserEntered;
        std::string _cachedSetName;
        std::vector<std::string> _members;
        ReadPreference _readPreference = ReadPreference::PRIMARY;
    };
}
