#include <gtest/gtest.h>

#include <QFile>
#include <QLockFile>
#include <QTemporaryDir>

#include "ragoshell/core/mcp/McpConfig.h"

using namespace RagoShell;

TEST(McpConfigTest, missing_file_means_disabled_with_defaults)
{
    QTemporaryDir dir;
    McpConfig config;
    config.enabled = true;
    QString error;
    ASSERT_TRUE(McpConfig::load(dir.filePath("mcp.json"), config, &error)) << error.toStdString();
    EXPECT_FALSE(config.enabled);
    EXPECT_TRUE(config.allowedConnections.isEmpty());
    EXPECT_EQ(config.maxDocuments, McpConfig::kDefaultMaxDocuments);
    EXPECT_EQ(config.maxTimeMs, McpConfig::kDefaultMaxTimeMs);
}

TEST(McpConfigTest, save_and_load_round_trip)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("sub/mcp.json");
    McpConfig config;
    config.enabled = true;
    config.allowedConnections = {"11111111-2222-3333-4444-555555555555"};
    config.maxDocuments = 20;
    config.maxTimeMs = 5000;
    ASSERT_TRUE(config.save(path));

    McpConfig loaded;
    ASSERT_TRUE(McpConfig::load(path, loaded));
    EXPECT_TRUE(loaded.enabled);
    EXPECT_EQ(loaded.allowedConnections, config.allowedConnections);
    EXPECT_EQ(loaded.maxDocuments, 20);
    EXPECT_EQ(loaded.maxTimeMs, 5000);
    EXPECT_TRUE(loaded.isAllowed("11111111-2222-3333-4444-555555555555"));
    EXPECT_FALSE(loaded.isAllowed("other"));
}

TEST(McpConfigTest, hand_edited_limits_are_clamped)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("mcp.json");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(R"({"enabled": true, "allowedConnections": ["a", "a", " ", "b"],
                   "maxDocuments": 1000000, "maxTimeMS": 0, "maxResultBytes": 1})");
    file.close();

    McpConfig config;
    ASSERT_TRUE(McpConfig::load(path, config));
    EXPECT_EQ(config.allowedConnections, QStringList({"a", "b"}));
    EXPECT_EQ(config.maxDocuments, McpConfig::kHardMaxDocuments);
    EXPECT_EQ(config.maxTimeMs, 100);
    EXPECT_EQ(config.maxResultBytes, 4 * 1024);
}

TEST(McpConfigTest, invalid_file_fails_closed)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("mcp.json");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write(R"({"enabled": true, "allowedConnections": [)");
    file.close();

    McpConfig config;
    QString error;
    EXPECT_FALSE(McpConfig::load(path, config, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(config.enabled);
    EXPECT_TRUE(config.allowedConnections.isEmpty());
}

TEST(McpConfigTest, modify_reads_the_file_on_disk_and_writes_it_back)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("mcp.json");
    McpConfig first;
    first.enabled = true;
    first.allowedConnections = {"a"};
    ASSERT_TRUE(first.save(path));

    // Someone else (the command line) adds "b" in the meantime: it is kept.
    McpConfig cli;
    ASSERT_TRUE(McpConfig::load(path, cli));
    cli.allowedConnections.append("b");
    ASSERT_TRUE(cli.save(path));

    QString error;
    ASSERT_TRUE(McpConfig::modify(path, [](McpConfig &c) {
        c.allowedConnections.append("c");
        c.maxDocuments = 1000000;
    }, &error)) << error.toStdString();

    McpConfig loaded;
    ASSERT_TRUE(McpConfig::load(path, loaded));
    EXPECT_TRUE(loaded.enabled);
    EXPECT_EQ(loaded.allowedConnections, QStringList({"a", "b", "c"}));
    EXPECT_EQ(loaded.maxDocuments, McpConfig::kHardMaxDocuments);   // clamped before writing
}

TEST(McpConfigTest, modify_never_replaces_a_broken_file)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("mcp.json");
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{ \"enabled\": true, broken");
    file.close();

    bool called = false;
    QString error;
    EXPECT_FALSE(McpConfig::modify(path, [&](McpConfig &c) {
        called = true;
        c.enabled = true;
    }, &error));
    EXPECT_FALSE(called);
    EXPECT_FALSE(error.isEmpty());

    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    EXPECT_EQ(file.readAll(), QByteArray("{ \"enabled\": true, broken"));
}

TEST(McpConfigTest, modify_creates_a_missing_file)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("sub/mcp.json");
    ASSERT_TRUE(McpConfig::modify(path, [](McpConfig &c) { c.allowedConnections = {"x"}; }));
    McpConfig loaded;
    ASSERT_TRUE(McpConfig::load(path, loaded));
    EXPECT_FALSE(loaded.enabled);   // allowing a connection does not turn the server on
    EXPECT_TRUE(loaded.isAllowed("x"));
}

TEST(McpConfigTest, modify_waits_for_the_lock_of_another_writer)
{
    QTemporaryDir dir;
    const QString path = dir.filePath("mcp.json");
    QLockFile other(path + ".lock");
    ASSERT_TRUE(other.lock());

    QString error;
    // Held by "another program" for the whole wait: refused, nothing written.
    EXPECT_FALSE(McpConfig::modify(path, [](McpConfig &c) { c.enabled = true; }, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(QFile::exists(path));

    other.unlock();
    EXPECT_TRUE(McpConfig::modify(path, [](McpConfig &c) { c.enabled = true; }, &error)) << error.toStdString();
}

TEST(McpConfigTest, keep_only_existing_connections)
{
    McpConfig config;
    config.allowedConnections = {"a", "gone", "b"};
    config.keepOnly({"b", "a", "c"});
    EXPECT_EQ(config.allowedConnections, QStringList({"a", "b"}));
    config.keepOnly({});
    EXPECT_TRUE(config.allowedConnections.isEmpty());
}
