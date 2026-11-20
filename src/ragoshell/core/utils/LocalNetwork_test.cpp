#include <gtest/gtest.h>

#include "ragoshell/core/utils/LocalNetwork.h"

using RagoShell::anyLocalNetworkHost;
using RagoShell::isLocalNetworkHost;

TEST(LocalNetwork, PrivateIpv4RangesAreLocal)
{
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("10.0.2.2")));
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("192.168.1.20:27017")));
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("172.16.0.1")));
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("172.31.255.255")));
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("169.254.10.1")));
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("100.64.0.1")));
}

TEST(LocalNetwork, PublicAndLoopbackAreNot)
{
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("8.8.8.8")));
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("172.32.0.1")));
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("100.128.0.1")));
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("127.0.0.1")));
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("localhost:27017")));
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("::1")));
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("cluster0.abcde.mongodb.net")));
    EXPECT_FALSE(isLocalNetworkHost(QString()));
}

TEST(LocalNetwork, Ipv6Ranges)
{
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("fd12:3456::1")));
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("[fe80::1]:27017")));
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("2001:4860:4860::8888")));
}

TEST(LocalNetwork, LocalNames)
{
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("nas.local")));
    EXPECT_TRUE(isLocalNetworkHost(QStringLiteral("mongo-box:27017")));
    EXPECT_FALSE(isLocalNetworkHost(QStringLiteral("db.example.com")));
}

TEST(LocalNetwork, AnyHost)
{
    EXPECT_TRUE(anyLocalNetworkHost({QStringLiteral("db.example.com"), QStringLiteral("10.1.1.1:27018")}));
    EXPECT_FALSE(anyLocalNetworkHost({QStringLiteral("db.example.com"), QStringLiteral("8.8.8.8")}));
    EXPECT_FALSE(anyLocalNetworkHost({}));
}
