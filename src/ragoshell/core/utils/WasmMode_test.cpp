#include <gtest/gtest.h>

#include <QUrl>

#include "ragoshell/core/BrandInfo.h"
#include "ragoshell/core/engine/JsShellBackend.h"
#include "ragoshell/core/transport/InMemoryDemoTransport.h"
#include "ragoshell/core/utils/UpdatePulse.h"
#include "ragoshell/core/utils/WasmMode.h"

using namespace RagoShell;

TEST(WasmModeTest, NativeBuildsNeverResolveToWasmModes)
{
    EXPECT_EQ(WasmMode::resolveRuntimeMode(false, QString()), WasmRuntimeMode::DesktopOrMobile);
    EXPECT_EQ(WasmMode::resolveRuntimeMode(false, u"playground"), WasmRuntimeMode::DesktopOrMobile);
    EXPECT_EQ(WasmMode::resolveRuntimeMode(false, u"gateway"), WasmRuntimeMode::DesktopOrMobile);
    EXPECT_EQ(WasmMode::resolveRuntimeMode(false, u"  GATEWAY  "), WasmRuntimeMode::DesktopOrMobile);
}

TEST(WasmModeTest, WasmBuildDefaultsAndFailsClosedToPlayground)
{
    EXPECT_EQ(WasmMode::resolveRuntimeMode(true, QString()), WasmRuntimeMode::Playground);
    EXPECT_EQ(WasmMode::resolveRuntimeMode(true, u"   "), WasmRuntimeMode::Playground);
    EXPECT_EQ(WasmMode::resolveRuntimeMode(true, u"playground"), WasmRuntimeMode::Playground);
    EXPECT_EQ(WasmMode::resolveRuntimeMode(true, u"  PLAYGROUND "), WasmRuntimeMode::Playground);
    // Unrecognized values fail closed to the restricted Playground mode.
    EXPECT_EQ(WasmMode::resolveRuntimeMode(true, u"desktop"), WasmRuntimeMode::Playground);
    EXPECT_EQ(WasmMode::resolveRuntimeMode(true, u"hosted"), WasmRuntimeMode::Playground);
}

TEST(WasmModeTest, WasmBuildHonorsExplicitGatewayMode)
{
    EXPECT_EQ(WasmMode::resolveRuntimeMode(true, u"gateway"), WasmRuntimeMode::Gateway);
    EXPECT_EQ(WasmMode::resolveRuntimeMode(true, u"  Gateway\n"), WasmRuntimeMode::Gateway);
}

TEST(WasmModeTest, ResolveDownloadPageUrlEnforcesSafeHttpsFromBrandProductUrl)
{
    const QString brandProd = QString::fromUtf8(BrandInfo::productUrl);
    const QString resolved = WasmMode::resolveDownloadPageUrl(brandProd);
    ASSERT_FALSE(resolved.isEmpty());
    EXPECT_TRUE(UpdatePulse::isSafeDownloadUrl(resolved));
    EXPECT_TRUE(resolved.endsWith(QStringLiteral("/download/")));
    EXPECT_EQ(QUrl(resolved).host(), QString::fromUtf8(BrandInfo::domain));

    EXPECT_EQ(WasmMode::resolveDownloadPageUrl(u"https://example.org///"),
              QStringLiteral("https://example.org/download/"));
    EXPECT_TRUE(WasmMode::resolveDownloadPageUrl(u"").isEmpty());
    EXPECT_TRUE(WasmMode::resolveDownloadPageUrl(u"http://example.org").isEmpty());
    EXPECT_TRUE(WasmMode::resolveDownloadPageUrl(u"javascript:alert(1)").isEmpty());
    EXPECT_TRUE(WasmMode::resolveDownloadPageUrl(u"https://user:pass@example.org").isEmpty());
    EXPECT_TRUE(WasmMode::resolveDownloadPageUrl(u"https://:@example.org").isEmpty());
    EXPECT_TRUE(WasmMode::resolveDownloadPageUrl(u"https://example.org?x=1").isEmpty());
    EXPECT_TRUE(WasmMode::resolveDownloadPageUrl(u"https://example.org#top").isEmpty());
}

TEST(WasmModeTest, DefaultPlaygroundConnectionNameUsesConfiguredProductName)
{
    const QString name = QString::fromUtf8(BrandInfo::productName);
    EXPECT_EQ(WasmMode::defaultPlaygroundConnectionName(name), name + QStringLiteral(" Playground"));
    EXPECT_EQ(WasmMode::defaultPlaygroundConnectionName(u"   "), QStringLiteral("Playground"));
}

TEST(WasmModeTest, StarterScriptExecutesCleanlyAgainstInMemoryDemoTransportAndResetRestoresSeed)
{
    InMemoryDemoTransport::resetSharedStore();
    InMemoryDemoTransport transport(InMemoryDemoTransport::sharedStore());
    ASSERT_TRUE(transport.connect(QStringLiteral("{}")).ok);

    JsShellBackend shell;
    ShellConnectOptions opts;
    opts.dbName = QStringLiteral("catalog");
    opts.batchSize = 10;
    opts.transport = &transport;
    opts.transportAddress = QStringLiteral("demo:27017");
    const ShellReply connReply = shell.connect(opts);
    ASSERT_TRUE(connReply.ok()) << connReply.message.toStdString();

    ShellEvalRequest starterReq;
    starterReq.script = WasmMode::playgroundStarterScript();
    starterReq.dbName = QStringLiteral("catalog");
    starterReq.batchSize = 10;
    const ShellEvalReply initial = shell.eval(starterReq);
    ASSERT_TRUE(initial.ok()) << initial.message.toStdString();
    ASSERT_EQ(initial.items.size(), 1u);
    EXPECT_EQ(initial.items[0].documents.size(), 3u);
    EXPECT_EQ(initial.items[0].queryInfo._info._ns.databaseName(), "catalog");
    EXPECT_EQ(initial.items[0].queryInfo._info._ns.collectionName(), "products");

    // Mutate the shared store (drop products + insert scratch collection), then reset.
    ShellEvalRequest mutateReq;
    mutateReq.script = QStringLiteral(
        "db.getCollection('products').drop(); db.getCollection('scratch').insertOne({ x: 1 })");
    mutateReq.dbName = QStringLiteral("catalog");
    ASSERT_TRUE(shell.eval(mutateReq).ok());

    ShellEvalRequest countDroppedReq;
    countDroppedReq.script = QStringLiteral("db.getCollection('products').countDocuments({})");
    countDroppedReq.dbName = QStringLiteral("catalog");
    const ShellEvalReply afterDrop = shell.eval(countDroppedReq);
    ASSERT_TRUE(afterDrop.ok());
    ASSERT_EQ(afterDrop.items.size(), 1u);
    EXPECT_EQ(afterDrop.items[0].output, "0");

    InMemoryDemoTransport::resetSharedStore();

    const ShellEvalReply afterReset = shell.eval(starterReq);
    ASSERT_TRUE(afterReset.ok()) << afterReset.message.toStdString();
    ASSERT_EQ(afterReset.items.size(), 1u);
    EXPECT_EQ(afterReset.items[0].documents.size(), 3u);

    ShellEvalRequest scratchCheckReq;
    scratchCheckReq.script = QStringLiteral("db.getCollection('scratch').countDocuments({})");
    scratchCheckReq.dbName = QStringLiteral("catalog");
    const ShellEvalReply scratchCheck = shell.eval(scratchCheckReq);
    ASSERT_TRUE(scratchCheck.ok());
    ASSERT_EQ(scratchCheck.items.size(), 1u);
    EXPECT_EQ(scratchCheck.items[0].output, "0");
}
