#pragma once
#include "ragoshell/core/domain/MongoNamespace.h"

namespace RagoShell
{
    class MongoCollectionInfo
    {
    public:
        MongoCollectionInfo() {}
        MongoCollectionInfo(const std::string &ns);
//        MongoCollectionInfo(mongo::BSONObj stats);

        std::string name() const { return _ns.collectionName(); }
        std::string fullName() const { return _ns.toString(); }
        MongoNamespace ns() const { return _ns; }

        const std::string &storageType() const { return _storageType; }
        void setStorageType(const std::string &type) { _storageType = type; }
        bool isView() const { return _storageType == "view"; }
        bool isTimeSeries() const { return _storageType == "timeseries"; }

        const std::string &viewOn() const { return _viewOn; }
        void setViewOn(const std::string &target) { _viewOn = target; }

        bool isCapped() const { return _capped; }
        void setCapped(bool capped) { _capped = capped; }

        /**
         * @brief Size in bytes
         * It is double, because db.stats()'s "size" field may be double
         * for large values, while Int32 for small.
         */
//        double sizeBytes() const { return _sizeBytes; }

        /**
         * @brief Storage size in bytes
         * It is double, because db.stats()'s "storageSize" field may be double
         * for large values, while Int32 for small.
         */
//        double storageSizeBytes() const { return _storageSizeBytes; }

//        long long count() const { return _count; }

    private:
        MongoNamespace _ns;

        /**
         * @brief Size in bytes
         * It is double, because db.stats()'s "size" field may be double
         * for large values, while Int32 for small.
         */
        double _sizeBytes = 0.0;

        /**
         * @brief Storage size in bytes
         * It is double, because db.stats()'s "storageSize" field may be double
         * for large values, while Int32 for small.
         */
        double _storageSizeBytes = 0.0;

        long long _count = 0;
        std::string _storageType;
        std::string _viewOn;
        bool _capped = false;
    };
}

