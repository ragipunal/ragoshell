#include <gtest/gtest.h>

#include <iostream>

#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <bsoncxx/builder/basic/document.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/uri.hpp>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

#include "ragoshell/core/settings/ConnectionSettings.h"
#include "ragoshell/core/utils/CollectionTransfer.h"

using namespace RagoShell;

namespace
{
    // Peak working set of this process in MB; -1 when the platform has no cheap way to ask.
    double peakMemoryMb()
    {
#ifdef Q_OS_WIN
        PROCESS_MEMORY_COUNTERS counters;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
            return static_cast<double>(counters.PeakWorkingSetSize) / (1024.0 * 1024.0);
#endif
        return -1;
    }
}

// Live: a local MongoDB (the Docker container of dev/mongo-test). Skips without one.
// Imports 100,000 documents (about 30 MB of NDJSON and of CSV), exports them again in
// every format, and checks that the counts match and that the memory use stays far below
// what holding the file (as text, as parsed documents) would cost.
TEST(CollectionTransferLiveTest, a_hundred_thousand_documents_import_and_export_with_flat_memory)
{
    try {
        static mongocxx::instance instance{};
    } catch (const std::exception &) {
    }
    const std::string dbName = "rago_transfer_live";
    try {
        mongocxx::client probe{mongocxx::uri{"mongodb://localhost:27017/?serverSelectionTimeoutMS=2000"}};
        probe[dbName].drop();
    } catch (const std::exception &ex) {
        std::cerr << "[ SKIP ] MongoDB not reachable on localhost:27017: " << ex.what() << "\n";
        return;
    }

    ConnectionSettings conn(/*isClone=*/false);
    conn.setUuid("uuid-live-transfer");
    conn.setServerHost("localhost");
    conn.setServerPort(27017);

    const int total = 100000;
    QTemporaryDir dir;
    const QString ndjson = dir.filePath("in.ndjson");
    const QString csvIn = dir.filePath("in.csv");
    {
        QFile nd(ndjson), csv(csvIn);
        ASSERT_TRUE(nd.open(QIODevice::WriteOnly));
        ASSERT_TRUE(csv.open(QIODevice::WriteOnly));
        const QByteArray pad(180, 'x');
        csv.write("n,name,note\n");
        for (int i = 0; i < total; ++i) {
            nd.write("{\"n\":" + QByteArray::number(i) + ",\"name\":\"name" + QByteArray::number(i)
                     + "\",\"note\":\"" + pad + "\",\"nested\":{\"a\":" + QByteArray::number(i % 7) + "}}\n");
            csv.write(QByteArray::number(i) + ",name" + QByteArray::number(i) + ",\"" + pad + ", with comma\"\n");
        }
    }
    const double fileMb = static_cast<double>(QFileInfo(ndjson).size()) / (1024.0 * 1024.0);
    std::cerr << "[ INFO ] input files: " << fileMb << " MB (NDJSON)\n";

    QString error;
    const double before = peakMemoryMb();

    int progress = 0;
    const std::function<void(int)> onProgress = [&progress](int n) { progress = n; };
    ASSERT_EQ(CollectionTransfer::importFileToCollection(conn, "rago_transfer_live", "ndjson", ndjson,
                  CollectionTransfer::Format::Ndjson, false, false, nullptr, onProgress, &error), total)
        << qPrintable(error);
    EXPECT_EQ(progress, total);
    ASSERT_EQ(CollectionTransfer::importFileToCollection(conn, "rago_transfer_live", "csv", csvIn,
                  CollectionTransfer::Format::Csv, true, false, nullptr, nullptr, &error), total) << qPrintable(error);

    for (const auto &[format, ext] : { std::pair{ CollectionTransfer::Format::Ndjson, "ndjson" },
                                       std::pair{ CollectionTransfer::Format::JsonArray, "json" },
                                       std::pair{ CollectionTransfer::Format::Csv, "csv" } }) {
        const QString out = dir.filePath(QStringLiteral("out.") + ext);
        ASSERT_EQ(CollectionTransfer::exportCollectionToFile(conn, "rago_transfer_live", "ndjson", out, format,
                      QString(), 0, nullptr, nullptr, &error), total) << ext << ": " << qPrintable(error);
        EXPECT_GT(QFileInfo(out).size(), fileMb * 1024 * 1024 / 2) << ext;
    }

    // A drop-and-replace with a broken file must leave the collection as it was.
    const QString broken = dir.filePath("broken.ndjson");
    {
        QFile f(broken);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("{\"ok\":1}\n{\"broken\":");
    }
    EXPECT_EQ(CollectionTransfer::importFileToCollection(conn, "rago_transfer_live", "ndjson", broken,
                  CollectionTransfer::Format::Ndjson, true, false, nullptr, nullptr, &error), -1);
    mongocxx::client client{mongocxx::uri{"mongodb://localhost:27017"}};
    EXPECT_EQ(client["rago_transfer_live"]["ndjson"].count_documents({}), total);

    const double after = peakMemoryMb();
    if (before >= 0) {
        std::cerr << "[ INFO ] peak working set grew by " << (after - before) << " MB\n";
        // Holding the 30 MB file as text and as documents would cost several times its size.
        EXPECT_LT(after - before, 120.0);
    }
    client["rago_transfer_live"].drop();
}
