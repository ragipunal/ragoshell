#include "gtest/gtest.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QUrl>

#include "ragoshell/core/utils/AppPaths.h"
#include "ragoshell/core/utils/QtUtils.h"

using namespace RagoShell;

TEST(AppPaths, ImportsCredentialFileWithOwnerOnlyPermissionsAndContentHash)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString srcPath = temp.filePath(QStringLiteral("id_ed25519"));
    const QByteArray keyBytes("-----BEGIN OPENSSH PRIVATE KEY-----\ntest-key-material\n-----END OPENSSH PRIVATE KEY-----\n");
    {
        QFile f(srcPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        ASSERT_EQ(f.write(keyBytes), keyBytes.size());
    }

    const QString credsDir = temp.filePath(QStringLiteral("sandbox/credentials"));
    QString err;
    const QString imported = importCredentialFileToDir(QUrl::fromLocalFile(srcPath), credsDir, &err);
    ASSERT_FALSE(imported.isEmpty()) << err.toStdString();
    EXPECT_TRUE(err.isEmpty());
    EXPECT_TRUE(imported.startsWith(QDir::cleanPath(credsDir) + QLatin1Char('/')));
    EXPECT_TRUE(imported.endsWith(QStringLiteral("_id_ed25519")));

    QFile check(imported);
    ASSERT_TRUE(check.open(QIODevice::ReadOnly));
    EXPECT_EQ(check.readAll(), keyBytes);

#ifdef Q_OS_UNIX
    const QFileDevice::Permissions perms = QFileInfo(imported).permissions();
    EXPECT_TRUE(perms.testFlag(QFileDevice::ReadOwner));
    EXPECT_TRUE(perms.testFlag(QFileDevice::WriteOwner));
    EXPECT_FALSE(perms.testFlag(QFileDevice::ReadGroup));
    EXPECT_FALSE(perms.testFlag(QFileDevice::WriteGroup));
    EXPECT_FALSE(perms.testFlag(QFileDevice::ReadOther));
    EXPECT_FALSE(perms.testFlag(QFileDevice::WriteOther));
#endif

    // Re-importing identical key material reuses the deterministic path cleanly.
    const QString reimported = importCredentialFileToDir(QUrl::fromLocalFile(srcPath), credsDir, &err);
    EXPECT_EQ(reimported, imported);
}

TEST(AppPaths, SanitizesTraversalDotsAndSpecialCharactersInLeafName)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString oddPath = temp.filePath(QStringLiteral("..hidden key$file!.pem"));
    {
        QFile f(oddPath);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write("CERT-DATA");
    }

    const QString credsDir = temp.filePath(QStringLiteral("creds"));
    QString err;
    const QString imported = importCredentialFileToDir(QUrl::fromLocalFile(oddPath), credsDir, &err);
    ASSERT_FALSE(imported.isEmpty()) << err.toStdString();
    EXPECT_TRUE(imported.startsWith(QDir::cleanPath(credsDir) + QLatin1Char('/')));
    const QString fileName = QFileInfo(imported).fileName();
    EXPECT_FALSE(fileName.contains(QLatin1Char('$')));
    EXPECT_FALSE(fileName.contains(QLatin1Char('!')));
    EXPECT_FALSE(fileName.contains(QLatin1String("..")));
    EXPECT_TRUE(fileName.endsWith(QStringLiteral("hidden_key_file_.pem")));
}

TEST(AppPaths, RejectsEmptyAndMissingCredentialFiles)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());

    const QString credsDir = temp.filePath(QStringLiteral("creds"));
    QString err;
    EXPECT_TRUE(importCredentialFileToDir(
                    QUrl::fromLocalFile(temp.filePath(QStringLiteral("missing.pem"))),
                    credsDir, &err)
                    .isEmpty());
    EXPECT_FALSE(err.isEmpty());

    const QString emptyFile = temp.filePath(QStringLiteral("empty.pem"));
    {
        QFile f(emptyFile);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    }
    err.clear();
    EXPECT_TRUE(importCredentialFileToDir(QUrl::fromLocalFile(emptyFile), credsDir, &err).isEmpty());
    EXPECT_FALSE(err.isEmpty());
}

TEST(QtUtilsSmartQuotes, NormalizesAllCurlySingleAndDoubleQuotesWhilePreservingOtherUnicode)
{
    const QString ascii = QStringLiteral("db.products.find({ \"sku\": 'CAM-001', `tag`: 1 })");
    EXPECT_FALSE(QtUtils::containsSmartQuotes(ascii));
    EXPECT_EQ(QtUtils::normalizeSmartQuotes(ascii), ascii);

    // U+2018..U+201B -> ' and U+201C..U+201F -> "
    const QString curly = QString::fromUtf16(
        u"{ \u201Csku\u201D: \u2018CAM\u2019, \u201Ealt\u201F: \u201Aok\u201B }");
    EXPECT_TRUE(QtUtils::containsSmartQuotes(curly));
    EXPECT_EQ(QtUtils::normalizeSmartQuotes(curly),
              QStringLiteral("{ \"sku\": 'CAM', \"alt\": 'ok' }"));

    // Non-quote Unicode (Uzbek U+02BB modifier letter turned comma, Turkish, CJK) must stay intact.
    const QString nonQuotes = QString::fromUtf16(u"o\u02BBzbek \u011F\u00FC\u015F\u0131\u00F6\u00E7\u0130 \u65E5\u672C");
    EXPECT_FALSE(QtUtils::containsSmartQuotes(nonQuotes));
    EXPECT_EQ(QtUtils::normalizeSmartQuotes(nonQuotes), nonQuotes);
}
