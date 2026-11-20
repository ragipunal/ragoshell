#include "gtest/gtest.h"
#include "ragoshell/core/bson/BsonTreeModel.h"
#include "ragoshell/core/bson/BsonTableModel.h"
#include "ragoshell/core/domain/MongoDocument.h"
#include "ragoshell/core/utils/BsonUtils.h"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/json.hpp>
#include <bsoncxx/types.hpp>
#include <chrono>

using namespace RagoShell;

TEST(bson_tree_model_tests, qml_roles_field_path_editability_and_no_truncation)
{
    const std::string longBio(420, 'x');
    const std::string json =
        std::string(R"({"_id":{"$oid":"507f1f77bcf86cd799439011"},"title":"Hello","bio":")") +
        longBio +
        R"(","meta":{"views":15}})";

    std::vector<bsoncxx::document::value> rawDocs;
    rawDocs.push_back(bsoncxx::from_json(json));
    const auto docs = MongoDocument::fromBsonObj(rawDocs);

    BsonTreeModel model(docs);
    ASSERT_EQ(model.rowCount(), 1);

    const QModelIndex docIdx = model.index(0, 0);
    ASSERT_TRUE(docIdx.isValid());
    EXPECT_EQ(model.data(docIdx, BsonTreeModel::BsonDocIndexRole).toInt(), 0);
    EXPECT_FALSE(model.data(docIdx, BsonTreeModel::BsonEditableScalarRole).toBool());

    // Row 0: _id (immutable -> not editableScalar)
    const QModelIndex idIdx = model.index(0, 0, docIdx);
    EXPECT_EQ(model.data(idIdx, BsonTreeModel::BsonFieldPathRole).toString(), QStringLiteral("_id"));
    EXPECT_FALSE(model.data(idIdx, BsonTreeModel::BsonEditableScalarRole).toBool());

    // Row 1: title (editable String scalar)
    const QModelIndex titleIdx = model.index(1, 0, docIdx);
    EXPECT_EQ(model.data(titleIdx, BsonTreeModel::BsonFieldPathRole).toString(), QStringLiteral("title"));
    EXPECT_TRUE(model.data(titleIdx, BsonTreeModel::BsonEditableScalarRole).toBool());

    // Row 2: bio (> 300 chars -> display role caps preview at 300, rawValue preserves full text)
    const QModelIndex bioIdx = model.index(2, 0, docIdx);
    EXPECT_EQ(model.data(bioIdx, BsonTreeModel::BsonValueRole).toString().size(), 300);
    EXPECT_GE(model.data(bioIdx, BsonTreeModel::BsonRawValueRole).toString().size(), 420);

    // Row 3: meta -> fetchMore -> meta.views
    const QModelIndex metaIdx = model.index(3, 0, docIdx);
    ASSERT_TRUE(model.canFetchMore(metaIdx));
    model.fetchMore(metaIdx);
    ASSERT_EQ(model.rowCount(metaIdx), 1);

    const QModelIndex viewsIdx = model.index(0, 0, metaIdx);
    EXPECT_EQ(model.data(viewsIdx, BsonTreeModel::BsonFieldPathRole).toString(), QStringLiteral("meta.views"));
    EXPECT_EQ(model.data(viewsIdx, BsonTreeModel::BsonDocIndexRole).toInt(), 0);
    EXPECT_TRUE(model.data(viewsIdx, BsonTreeModel::BsonEditableScalarRole).toBool());

    // Table proxy must also cap display preview at 300 while exposing full BsonRawValueRole
    BsonTableModelProxy tableProxy;
    tableProxy.setSourceModel(&model);
    EXPECT_EQ(tableProxy.rowCount(), 1);
    EXPECT_EQ(tableProxy.columnCount(QModelIndex()), 4);
    const QModelIndex bioCell = tableProxy.index(0, 2, QModelIndex());
    EXPECT_EQ(tableProxy.data(bioCell, Qt::DisplayRole).toString().size(), 300);
    EXPECT_GE(tableProxy.data(bioCell, BsonTreeModel::BsonRawValueRole).toString().size(), 420);
}

TEST(bson_tree_model_tests, sanitizes_c0_and_c1_control_codes_while_preserving_turkish_utf8)
{
    // Exact reproduction of ip.ipdocfiles:
    //  1) Valid Turkish UTF-8 ("ÜLKE TV_20241215_0.jpeg", "AKİT TV_20241215_0.mp4")
    //  2) Mojibake containing U+009C STRING TERMINATOR ("Ã\u009cLKE TV_20241215_0.jpeg")
    //     which previously truncated to just "Ã" in Windows DirectWrite / Qt Text!
    const std::string json =
        R"({"_id":"a24c12d1-359c-4469-a543-1d66d487684e",)"
        R"("validTr":"\u00dcLKE TV & AK\u0130T TV \u015e\u011e\u00c7\u00d6\u0131",)"
        R"("mojibakeSt":"\u00c3\u009cLKE TV_20241215_0.jpeg",)"
        R"("embeddedNul":"head\u0000tail"})";

    std::vector<bsoncxx::document::value> rawDocs;
    rawDocs.push_back(bsoncxx::from_json(json));
    const auto docs = MongoDocument::fromBsonObj(rawDocs);

    BsonTreeModel model(docs);
    ASSERT_EQ(model.rowCount(), 1);
    const QModelIndex docIdx = model.index(0, 0);

    // Row 1: valid Turkish UTF-8 must remain 100% intact
    const QModelIndex validIdx = model.index(1, 0, docIdx);
    const QString expectedValid = QString::fromUtf8(
        "\xC3\x9CLKE TV & AK\xC4\xB0T TV \xC5\x9E\xC4\x9E\xC3\x87\xC3\x96\xC4\xB1");
    EXPECT_EQ(model.data(validIdx, BsonTreeModel::BsonValueRole).toString(), expectedValid);

    // Row 2: U+009C STRING TERMINATOR must be replaced with U+FFFD in display roles
    // while keeping the full suffix "LKE TV_20241215_0.jpeg" intact (and rawValue untouched)
    const QModelIndex mojibakeIdx = model.index(2, 0, docIdx);
    const QString treeDisp = model.data(mojibakeIdx, BsonTreeModel::BsonValueRole).toString();
    const QString expectedMojibake =
        QString(QChar(0x00C3)) + QChar::ReplacementCharacter + QStringLiteral("LKE TV_20241215_0.jpeg");
    EXPECT_FALSE(treeDisp.contains(QChar(0x009C)));
    EXPECT_TRUE(treeDisp.endsWith(QStringLiteral("LKE TV_20241215_0.jpeg")));
    EXPECT_EQ(treeDisp, expectedMojibake);
    EXPECT_TRUE(model.data(mojibakeIdx, BsonTreeModel::BsonRawValueRole).toString().contains(QChar(0x009C)));

    // Row 3: embedded U+0000 NUL must also be sanitized in display roles
    const QModelIndex nulIdx = model.index(3, 0, docIdx);
    EXPECT_EQ(model.data(nulIdx, BsonTreeModel::BsonValueRole).toString(),
              QStringLiteral("head") + QChar::ReplacementCharacter + QStringLiteral("tail"));

    // Table proxy DisplayRole & ToolTipRole must likewise strip U+009C and preserve suffix
    BsonTableModelProxy tableProxy;
    tableProxy.setSourceModel(&model);
    const QModelIndex tableMojibake = tableProxy.index(0, 2, QModelIndex());
    const QString tableDisp = tableProxy.data(tableMojibake, Qt::DisplayRole).toString();
    EXPECT_FALSE(tableDisp.contains(QChar(0x009C)));
    EXPECT_EQ(tableDisp, expectedMojibake);
}

TEST(bson_tree_model_tests, caps_huge_binary_and_megabyte_strings_without_splitting_utf8)
{
    using bsoncxx::builder::basic::kvp;

    // 2 MB binary payload (simulating MP4/JPEG in ipdocfiles) + 120 KB Turkish UTF-8 string
    std::vector<uint8_t> bigBin(2 * 1024 * 1024, 0xAB);
    bsoncxx::types::b_binary binVal{
        bsoncxx::binary_sub_type::k_binary,
        static_cast<uint32_t>(bigBin.size()),
        bigBin.data()
    };

    std::string bigTurkishText;
    bigTurkishText.reserve(120 * 1024);
    while (bigTurkishText.size() < 100 * 1024)
        bigTurkishText += "\xC3\x9CLKE TV \xC5\x9E\xC4\x9E\xC4\xB0\xC3\x87\xC3\x96 ";

    bsoncxx::builder::basic::document builder;
    builder.append(kvp("_id", "doc-large-1"));
    builder.append(kvp("filename", "AK\xC4\xB0T TV_20241215_0.mp4"));
    builder.append(kvp("payload", binVal));
    builder.append(kvp("ocrText", bigTurkishText));

    std::vector<bsoncxx::document::value> rawDocs;
    rawDocs.push_back(builder.extract());
    const auto docs = MongoDocument::fromBsonObj(rawDocs);

    BsonTreeModel model(docs);
    ASSERT_EQ(model.rowCount(), 1);
    const QModelIndex docIdx = model.index(0, 0);

    // Row 1: short filename remains inline-editable
    const QModelIndex fileIdx = model.index(1, 0, docIdx);
    EXPECT_TRUE(model.data(fileIdx, BsonTreeModel::BsonEditableScalarRole).toBool());

    // Row 3: >16 KB string is capped in BsonTreeItem RAM and guarded against inline single-line truncation
    const QModelIndex ocrIdx = model.index(3, 0, docIdx);
    const QString rawOcr = model.data(ocrIdx, BsonTreeModel::BsonRawValueRole).toString();
    EXPECT_LE(rawOcr.toUtf8().size(), 16384);
    EXPECT_FALSE(rawOcr.contains(QChar::ReplacementCharacter)); // UTF-8 boundary preserved cleanly!
    EXPECT_EQ(model.data(ocrIdx, BsonTreeModel::BsonValueRole).toString().size(), 300);
    EXPECT_FALSE(model.data(ocrIdx, BsonTreeModel::BsonEditableScalarRole).toBool());

    // Viewer JSON preview with maxFieldPreviewBytes = 4096 must stay tiny (< 20 KB) instead of > 2.7 MB Base64
    const std::string previewJson = BsonUtils::jsonString(
        docs[0]->bsonObj(), TenGen, 1, DefaultEncoding, Utc, /*isArray=*/false, /*maxFieldPreviewBytes=*/4096);
    EXPECT_LT(previewJson.size(), 20000u);
    EXPECT_NE(previewJson.find("bytes total, truncated in viewer"), std::string::npos);
}

TEST(bson_tree_model_tests, builds_fifty_megabyte_batch_zero_copy_without_blocking)
{
    using bsoncxx::builder::basic::kvp;
    using bsoncxx::builder::basic::make_document;

    // 50 documents x 1 MB binary + 14 metadata fields.
    // Previously BsonTreeItem deep-copied the entire root BSON document per child field
    // (~800 MB copied synchronously on the GUI thread). With zero-copy views this takes < 30 ms.
    std::vector<uint8_t> oneMb(1024 * 1024, 0x5A);
    bsoncxx::types::b_binary binVal{
        bsoncxx::binary_sub_type::k_binary,
        static_cast<uint32_t>(oneMb.size()),
        oneMb.data()
    };

    std::vector<bsoncxx::document::value> rawDocs;
    rawDocs.reserve(50);
    for (int i = 0; i < 50; ++i) {
        bsoncxx::builder::basic::document b;
        b.append(kvp("_id", "id-" + std::to_string(i)));
        b.append(kvp("docId", static_cast<int64_t>(100000 + i)));
        b.append(kvp("filename", "clip_" + std::to_string(i) + ".mp4"));
        b.append(kvp("contentType", "video/mp4"));
        b.append(kvp("channel", "TV_" + std::to_string(i % 7)));
        b.append(kvp("status", 1));
        b.append(kvp("durationSec", 125));
        b.append(kvp("meta", make_document(kvp("fps", 25), kvp("codec", "h264"))));
        b.append(kvp("payload", binVal));
        rawDocs.push_back(b.extract());
    }
    const auto docs = MongoDocument::fromBsonObj(rawDocs);

    const auto t0 = std::chrono::steady_clock::now();
    {
        BsonTreeModel treeModel(docs);
        BsonTableModelProxy tableProxy;
        tableProxy.setSourceModel(&treeModel);

        ASSERT_EQ(treeModel.rowCount(), 50);
        ASSERT_EQ(tableProxy.rowCount(), 50);
        EXPECT_EQ(tableProxy.columnCount(QModelIndex()), 9);

        // Expand nested 'meta' subdocument on first and last document and verify views stay valid
        for (int row : {0, 49}) {
            const QModelIndex docIdx = treeModel.index(row, 0);
            const QModelIndex metaIdx = treeModel.index(7, 0, docIdx);
            ASSERT_TRUE(treeModel.canFetchMore(metaIdx));
            treeModel.fetchMore(metaIdx);
            ASSERT_EQ(treeModel.rowCount(metaIdx), 2);
            const QModelIndex codecIdx = treeModel.index(1, 0, metaIdx);
            EXPECT_EQ(treeModel.data(codecIdx, BsonTreeModel::BsonFieldPathRole).toString(),
                      QStringLiteral("meta.codec"));
        }
    }
    const auto elapsedMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(elapsedMs, 1000);
}
