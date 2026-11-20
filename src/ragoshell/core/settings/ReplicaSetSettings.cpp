#include "ragoshell/core/settings/ReplicaSetSettings.h"

#include "ragoshell/core/utils/QtUtils.h"

namespace RagoShell
{

    ReplicaSetSettings::ReplicaSetSettings() 
        : _cachedSetName(""), _members(), _readPreference(ReadPreference::PRIMARY)
    {}

    ReplicaSetSettings::ReplicaSetSettings(const mongocxx::uri& uri)
        : ReplicaSetSettings() 
    {
        _cachedSetName = std::string(uri.replica_set());
        for (auto const& server : uri.hosts()) {
            _members.push_back(std::string(server.name) + ":" + std::to_string(server.port));
        }
    }

    ReplicaSetSettings *ReplicaSetSettings::clone() const 
    {
        ReplicaSetSettings *cloned = new ReplicaSetSettings(*this);
        return cloned;
    }

    QVariant ReplicaSetSettings::toVariant() const 
    {
        QVariantMap map;
        map.insert("setNameUserEntered", QString::fromStdString(_setNameUserEntered));
        map.insert("cachedSetName", QString::fromStdString(_cachedSetName));
        int idx = 0;
        for (std::string const& str : _members) {
            map.insert(QString::number(idx), QtUtils::toQString(str));
            ++idx;
        }
        map.insert("readPreference", static_cast<int>(readPreference()));
        return map;
    }

    void ReplicaSetSettings::fromVariant(const QVariantMap &map) 
    {
        setSetNameUserEntered(map.value("setNameUserEntered").toString().toStdString());
        setCachedSetName(map.value("cachedSetName").toString().toStdString());
        // Extract and set replica members
        std::vector<std::string> vec;
        auto itr = map.begin();
        int idx = 0;
        do
        {
            itr = map.find(QString::number(idx));
            if (map.end() == itr) break;
            vec.push_back(itr->toString().toStdString());
            ++idx;
        } while (map.end() != itr);
        setMembers(vec);
        // Extract and set read reference
        setReadPreference(static_cast<ReadPreference>(map.value("readPreference").toInt()));
    }
    
    void ReplicaSetSettings::setMembers(const std::vector<std::string>& members)
    {
        _members.clear();
        for (auto const& member : members)
            _members.push_back(member);
    }

    void ReplicaSetSettings::setMembers(const std::vector<std::pair<std::string, bool>>& membersAndHealts)
    {
        _members.clear();

        for (auto const& memberAndHealth : membersAndHealts)
            _members.push_back(memberAndHealth.first);
    }

    std::vector<HostAndPort> ReplicaSetSettings::membersToHostAndPort() const 
    {
        std::vector<HostAndPort> membersHostAndPort;
        for (auto const& member : _members)        
            membersHostAndPort.push_back(HostAndPort(member));
        
        return membersHostAndPort;
    }

}
