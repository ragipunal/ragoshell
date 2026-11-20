
#include "ragoshell/core/mongodb/ReplicaSet.h"

namespace RagoShell
{
    ReplicaSet::ReplicaSet(std::string const& setName, const HostAndPort primary,
                           std::vector<std::pair<std::string, bool>> const& membersAndHealths, 
                           std::string const& errorStr)
                           : setName(setName), primary(primary), membersAndHealths(membersAndHealths), 
                           errorStr(errorStr)
    {}
}
