#pragma once

#include "ragoshell/core/domain/HostAndPort.h"
#include <vector>

namespace RagoShell
{
    /**
    * @brief Struct to represent live information of a replica set.
    */
    struct ReplicaSet
    {
        ReplicaSet(std::string const& setName, const HostAndPort primary,
                   std::vector<std::pair<std::string, bool>> const& membersAndHealths, std::string const& errorStr = "");

        ReplicaSet() {};

        std::string const setName;
        HostAndPort const primary;        
        std::string const errorStr;
        
        // pair: {HostNameAndPort, Health} 
        std::vector<std::pair<std::string, bool>> const membersAndHealths;  
            // todo: make first element of pair type HostnameAndPort
    };
}