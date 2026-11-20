#include <gtest/gtest.h>

#include <QFile>
#include <QTemporaryDir>

#include <bsoncxx/json.hpp>

#include "ragoshell/core/utils/CollectionTransfer.h"
#include "ragoshell/core/utils/RecordSplitter.h"

using namespace RagoShell;

namespace
{
    QStringList splitJson(const QString &text, int step = 0)
    {
        JsonDocumentSplitter splitter;
        QStringList out;
        if (step <= 0) {
            out << splitter.feed(text);
        } else {
            for (int i = 0; i < text.size(); i += step)
                out << splitter.feed(text.mid(i, step));
        }
        EXPECT_TRUE(splitter.finish());
        return out;
    }

    QStringList splitCsv(const QString &text, int step = 0)
    {
        CsvRecordSplitter splitter;
        QStringList out;
        if (step <= 0) {
            out << splitter.feed(text);
        } else {
            for (int i = 0; i < text.size(); i += step)
                out << splitter.feed(text.mid(i, step));
        }
        out << splitter.finish();
        return out;
    }

    QString writeFile(QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
    {
        const QString path = dir.filePath(name);
        QFile f(path);
        EXPECT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(bytes);
        return path;
    }
}

TEST(JsonDocumentSplitterTest, finds_the_documents_of_an_array_ndjson_and_a_stream_of_objects)
{
    const QStringList want{ "{\"a\":1}", "{\"b\":{\"c\":[1,2]}}" };
    EXPECT_EQ(splitJson("[\n  {\"a\":1},\n  {\"b\":{\"c\":[1,2]}}\n]"), want);
    EXPECT_EQ(splitJson("{\"a\":1}\n{\"b\":{\"c\":[1,2]}}\n"), want);
    EXPECT_EQ(splitJson(QString(QChar(0xFEFF)) + "{\"a\":1}{\"b\":{\"c\":[1,2]}}"), want);   // BOM, no separator at all
    EXPECT_TRUE(splitJson("[]").isEmpty());
    EXPECT_TRUE(splitJson("   \n").isEmpty());
}

TEST(JsonDocumentSplitterTest, braces_and_quotes_inside_strings_do_not_count)
{
    const QString doc = QStringLiteral(R"json({"s":"} { \" ]","t":'it\'s }',"n":{"x":"}"}})json");
    EXPECT_EQ(splitJson(doc), QStringList{ doc });
    EXPECT_EQ(splitJson("[" + doc + "," + doc + "]"), (QStringList{ doc, doc }));
}

TEST(JsonDocumentSplitterTest, the_chunk_size_makes_no_difference)
{
    const QString text = QStringLiteral("[{\"a\":\"x}y\"},\n{\"b\":[{\"c\":\"\\\\\"}]},{\"d\":{}}]");
    const QStringList whole = splitJson(text);
    ASSERT_EQ(whole.size(), 3);
    for (int step : { 1, 2, 3, 5, 7, 11 })
        EXPECT_EQ(splitJson(text, step), whole) << "step " << step;
}

TEST(JsonDocumentSplitterTest, a_document_that_never_closes_is_an_error)
{
    JsonDocumentSplitter splitter;
    EXPECT_EQ(splitter.feed("[{\"a\":1},{\"b\":").size(), 1);
    QString error;
    EXPECT_FALSE(splitter.finish(&error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_GT(splitter.pendingChars(), 0);
}

TEST(CsvRecordSplitterTest, cuts_records_at_line_ends_outside_quotes)
{
    EXPECT_EQ(splitCsv("a,b\n1,2\n3,4"), (QStringList{ "a,b", "1,2", "3,4" }));
    EXPECT_EQ(splitCsv("a,b\r\n1,2\r\n"), (QStringList{ "a,b", "1,2" }));          // CRLF is one line end
    EXPECT_EQ(splitCsv("a\n\nb\n"), (QStringList{ "a", "", "b" }));                  // the empty line is a record
    EXPECT_EQ(splitCsv("\"x,\ny\",2\n3,\"say \"\"hi\"\"\"\n"),
              (QStringList{ "\"x,\ny\",2", "3,\"say \"\"hi\"\"\"" }));
}

TEST(CsvRecordSplitterTest, a_quote_in_the_middle_of_a_cell_is_just_a_character)
{
    // Quotes only open a quoted cell at the start of a cell (same rule as the CSV parser).
    EXPECT_EQ(splitCsv("5\" pipe,2\nnext,3\n"), (QStringList{ "5\" pipe,2", "next,3" }));
}

TEST(CsvRecordSplitterTest, the_chunk_size_makes_no_difference)
{
    const QString text = QStringLiteral("h1,h2\r\n\"a\nb\",\"q\"\"\"\r\n\"\"\"\",x\n1,2");
    const QStringList whole = splitCsv(text);
    ASSERT_EQ(whole.size(), 4);
    for (int step : { 1, 2, 3, 4, 5, 8 })
        EXPECT_EQ(splitCsv(text, step), whole) << "step " << step;
}

TEST(ForEachDocumentBatchTest, a_big_ndjson_file_arrives_in_batches_of_at_most_500)
{
    QTemporaryDir dir;
    QByteArray bytes;
    const int total = 1800;
    const QByteArray filler = QString(1200, QChar(0x00E9)).toUtf8();     // two-byte characters
    for (int i = 0; i < total; ++i)
        bytes += QByteArray("{\"_id\":") + QByteArray::number(i) + ",\"text\":\"" + filler + "\"}\n";
    ASSERT_GT(bytes.size(), 2 * (1 << 20));       // more than two read chunks, so characters get cut at chunk ends
    const QString path = writeFile(dir, "big.ndjson", bytes);

    int docs = 0, batches = 0;
    size_t largest = 0;
    QString error;
    ASSERT_TRUE(CollectionTransfer::forEachDocumentBatch(path, CollectionTransfer::Format::Ndjson, nullptr,
        [&](std::vector<bsoncxx::document::value> &batch) {
            ++batches;
            docs += static_cast<int>(batch.size());
            largest = std::max(largest, batch.size());
            return true;
        }, &error)) << qPrintable(error);

    EXPECT_EQ(docs, total);
    EXPECT_LE(largest, 500u);
    EXPECT_GE(batches, 4);                        // 1800 / 500
}

TEST(ForEachDocumentBatchTest, json_array_and_csv_files_give_the_same_documents_as_the_whole_file_parsers)
{
    QTemporaryDir dir;
    QString error;

    const QString jsonPath = writeFile(dir, "a.json", "[\n {\"_id\":1,\"s\":\"x\"},\n {\"_id\":2,\"s\":\"{y}\"}\n]\n");
    std::vector<std::string> jsonDocs;
    ASSERT_TRUE(CollectionTransfer::forEachDocumentBatch(jsonPath, CollectionTransfer::Format::JsonArray, nullptr,
        [&](std::vector<bsoncxx::document::value> &batch) {
            for (auto &d : batch) jsonDocs.push_back(bsoncxx::to_json(d.view()));
            return true;
        }, &error)) << qPrintable(error);
    ASSERT_EQ(jsonDocs.size(), 2u);
    EXPECT_NE(jsonDocs[1].find("{y}"), std::string::npos);

    const QByteArray csv = QByteArray("\xEF\xBB\xBF") + "_id,name,note\r\n1,Ann,\"two\nlines\"\r\n\r\n2,Bob,\"say \"\"hi\"\"\"\r\n";
    const QString csvPath = writeFile(dir, "a.csv", csv);
    std::vector<bsoncxx::document::value> whole;
    ASSERT_TRUE(CollectionTransfer::parseCsv(QString::fromUtf8(csv.mid(3)), whole, &error)) << qPrintable(error);
    std::vector<std::string> streamed;
    ASSERT_TRUE(CollectionTransfer::forEachDocumentBatch(csvPath, CollectionTransfer::Format::Csv, nullptr,
        [&](std::vector<bsoncxx::document::value> &batch) {
            for (auto &d : batch) streamed.push_back(bsoncxx::to_json(d.view()));
            return true;
        }, &error)) << qPrintable(error);
    ASSERT_EQ(streamed.size(), whole.size());
    for (size_t i = 0; i < whole.size(); ++i)
        EXPECT_EQ(streamed[i], bsoncxx::to_json(whole[i].view()));
}

TEST(ForEachDocumentBatchTest, errors_are_reported_and_a_cancel_stops_the_read)
{
    QTemporaryDir dir;
    QString error;
    auto never = [](std::vector<bsoncxx::document::value> &) { return true; };

    EXPECT_FALSE(CollectionTransfer::forEachDocumentBatch(dir.filePath("missing.json"),
                 CollectionTransfer::Format::JsonArray, nullptr, never, &error));
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    const QString cut = writeFile(dir, "cut.json", "[{\"a\":1},{\"b\":");
    EXPECT_FALSE(CollectionTransfer::forEachDocumentBatch(cut, CollectionTransfer::Format::JsonArray, nullptr, never, &error));
    EXPECT_FALSE(error.isEmpty());

    error.clear();
    const QString okFile = writeFile(dir, "ok.ndjson", "{\"a\":1}\n{\"a\":2}\n");
    std::atomic<bool> cancel{true};
    EXPECT_FALSE(CollectionTransfer::forEachDocumentBatch(okFile, CollectionTransfer::Format::Ndjson, &cancel, never, &error));
    EXPECT_FALSE(error.isEmpty());

    error.clear();                                // the callback can stop it too
    EXPECT_FALSE(CollectionTransfer::forEachDocumentBatch(okFile, CollectionTransfer::Format::Ndjson, nullptr,
        [&](std::vector<bsoncxx::document::value> &) { error = "stopped"; return false; }, &error));
    EXPECT_EQ(error, QStringLiteral("stopped"));
}
