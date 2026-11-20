#pragma once

#include <cstdint>
#include <memory>

#include "ragoshell/core/transport/IMongoTransport.h"

namespace mongocxx
{
    inline namespace v_noabi
    {
        class client;
    }
}

namespace RagoShell
{
    /**
     * @brief IMongoTransport over the native mongocxx driver (desktop path).
     *
     * Commands arrive as EJSON and run through run_command on the requested
     * database; results are serialized back as canonical extended JSON so the
     * shape is identical to the WASM transports (docs/WasmBuildReport.md §3).
     */
    class NativeMongoTransport : public IMongoTransport
    {
    public:
        NativeMongoTransport();
        ~NativeMongoTransport() override;

        Result connect(const QString &ejsonSettings) override;
        Result runCommand(const QString &database, const QString &ejsonCommand) override;
        Result listDatabases() override;
        Result listCollections(const QString &database) override;
        Result ping() override;
        Result killOp(const QString &ejsonFilter) override;
        void disconnect() override;
        bool isLive() const override { return true; }

        /// Server-side maxTimeMS bound on every bridged command (8.6 M1).
        static constexpr std::int32_t kMaxOpTimeMs = 50000;

        /// The command runCommand() actually sends, as canonical EJSON:
        /// exactly one int32 maxTimeMS in [1, kMaxOpTimeMs], last key.
        /// Empty on unparseable input. Exposed for tests (review R4).
        static QString withMaxTimeCap(const QString &ejsonCommand);

    private:
        std::unique_ptr<mongocxx::client> _client;
    };
}
