#pragma once

#include <cstddef>
#include <memory>

#include <QJsonObject>
#include <QString>

#include "ragoshell/core/transport/IMongoTransport.h"

namespace RagoShell
{
    class InMemoryDemoStore;

    /**
     * @brief BSON-native in-memory transport for the Mod A live playground
     * (docs/WasmBuildReport.md §3–4, docs/TransportShell.md §2.4).
     *
     * Serves the synthetic dataset "catalog" (products, customers, orders) and
     * scratch collections/databases in RAM without any socket or native driver.
     * Documents are stored as `bsoncxx::document::value` and exchanged over the
     * seam as canonical Extended JSON so `ObjectId`, `Int64` (`NumberLong`),
     * `Decimal128` (`NumberDecimal`), `Date` (`ISODate`), `RegExp`, `BinData`,
     * and field order survive round-trips intact.
     *
     * Default construction creates an isolated private store (hermetic for unit
     * tests). Passing `sharedStore()` shares one thread-safe store across the
     * WASM Playground's Explorer worker, Shell tabs, and Schema analyzer.
     */
    class InMemoryDemoTransport final : public IMongoTransport
    {
    public:
        static constexpr std::size_t kMaxDatabases = 8;
        static constexpr std::size_t kMaxCollectionsPerDb = 32;
        static constexpr std::size_t kMaxDocumentsPerCollection = 5000;
        static constexpr std::size_t kMaxIndexesPerCollection = 16;
        static constexpr std::size_t kMaxDocumentBytes = 256 * 1024;
        static constexpr std::size_t kMaxPipelineDocs = 10000;
        static constexpr std::size_t kMaxPipelineStages = 64;
        static constexpr int kMaxDepth = 64;

        InMemoryDemoTransport();
        explicit InMemoryDemoTransport(std::shared_ptr<InMemoryDemoStore> store);
        ~InMemoryDemoTransport() override;

        /// Process-wide thread-safe store used by WASM Playground workers.
        static std::shared_ptr<InMemoryDemoStore> sharedStore();
        /// Restores `sharedStore()` to the factory `catalog` dataset.
        static void resetSharedStore();
        /// Restores this transport's bound store to the factory `catalog` dataset.
        void reset();

        Result connect(const QString &ejsonSettings) override;
        Result runCommand(const QString &database, const QString &ejsonCommand) override;
        Result listDatabases() override;
        Result listCollections(const QString &database) override;
        Result ping() override;
        Result killOp(const QString &ejsonFilter) override;
        void disconnect() override;
        bool isLive() const override { return false; }

        /// Relaxed JSON snapshot of the built-in `catalog` seed (for tests/UI).
        static QJsonObject dataset();

    private:
        bool _connected = false;
        std::shared_ptr<InMemoryDemoStore> _store;
    };
}
