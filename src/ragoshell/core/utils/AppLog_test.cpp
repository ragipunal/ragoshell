#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTemporaryDir>

#include "ragoshell/core/utils/AppLog.h"
#include "ragoshell/core/utils/AppPaths.h"

using namespace RagoShell;

namespace
{
    int g_forwarded = 0;
    QString g_lastForwarded;

    void countingHandler(QtMsgType, const QMessageLogContext &, const QString &message)
    {
        ++g_forwarded;
        g_lastForwarded = message;
    }

    QString readAll(const QString &path)
    {
        QFile f(path);
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
    }

    // Sets one environment variable for a test (null: unset) and puts the old value back.
    class EnvOverride
    {
    public:
        EnvOverride(const char *name, const QString &value)
            : _name(name), _hadValue(qEnvironmentVariableIsSet(name)), _old(qgetenv(name))
        {
            if (value.isNull())
                qunsetenv(_name.constData());
            else
                qputenv(_name.constData(), value.toUtf8());
        }
        ~EnvOverride()
        {
            if (_hadValue)
                qputenv(_name.constData(), _old);
            else
                qunsetenv(_name.constData());
        }

    private:
        QByteArray _name;
        bool _hadValue;
        QByteArray _old;
    };

    // RAGOSHELL_CONFIG_DIR for one test.
    class ConfigDirOverride
    {
    public:
        explicit ConfigDirOverride(const QString &value) : _current("RAGOSHELL_CONFIG_DIR", value) {}

    private:
        EnvOverride _current;
    };

    // Every AppLog test starts and ends with no handler installed.
    class AppLogTest : public ::testing::Test
    {
    protected:
        void SetUp() override
        {
            AppLog::uninstall();
            g_forwarded = 0;
            g_lastForwarded.clear();
        }
        void TearDown() override
        {
            AppLog::uninstall();
            qInstallMessageHandler(nullptr);
        }
    };
}

TEST(AppPathsTest, config_dir_override_wins_and_log_dir_is_below_it)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    ConfigDirOverride guard(tmp.path());

    EXPECT_EQ(appDataDir(), QDir::cleanPath(tmp.path()) + QLatin1Char('/'));
    EXPECT_EQ(appLogDir(), appDataDir() + QStringLiteral("logs/"));
    EXPECT_TRUE(appDataDirOverridden());
}

TEST(AppPathsTest, names_are_derived_from_the_brand)
{
    EXPECT_EQ(appFileName("json"), QStringLiteral("ragoshell.json"));
    EXPECT_EQ(appFileName("key"), QStringLiteral("ragoshell.key"));
    EXPECT_EQ(productEnvName("CONFIG_DIR"), QStringLiteral("RAGOSHELL_CONFIG_DIR"));
#if !defined(Q_OS_IOS) && !defined(Q_OS_ANDROID)
    EXPECT_EQ(defaultAppDataDir(), QDir::homePath() + QStringLiteral("/.ragoshell/"));
#endif
}

TEST(AppPathsTest, without_the_config_dir_variable_the_default_folder_is_used)
{
    ConfigDirOverride guard{QString()};

    EXPECT_EQ(appDataDir(), defaultAppDataDir());
    EXPECT_FALSE(appDataDirOverridden());
}

TEST(AppPathsTest, product_environment_variables_are_read_by_their_suffix)
{
    EnvOverride unset("RAGOSHELL_UNIT_TEST_VALUE", QString());
    EXPECT_TRUE(productEnv("UNIT_TEST_VALUE").isEmpty());
    EXPECT_EQ(productEnvInt("UNIT_TEST_VALUE"), 0);
    {
        EnvOverride number("RAGOSHELL_UNIT_TEST_VALUE", QStringLiteral("0x10"));
        EXPECT_EQ(productEnv("UNIT_TEST_VALUE"), QStringLiteral("0x10"));
        EXPECT_EQ(productEnvInt("UNIT_TEST_VALUE"), 16);
    }
    {
        EnvOverride text("RAGOSHELL_UNIT_TEST_VALUE", QStringLiteral("new"));
        EXPECT_EQ(productEnv("UNIT_TEST_VALUE"), QStringLiteral("new"));
        EXPECT_EQ(productEnvInt("UNIT_TEST_VALUE"), 0);   // not a number
    }
}

TEST_F(AppLogTest, writes_nothing_and_creates_no_file_until_installed)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    AppLog::write(LogSeverity::Error, QStringLiteral("before install"));

    EXPECT_FALSE(AppLog::isInstalled());
    EXPECT_TRUE(AppLog::filePath().isEmpty());
    EXPECT_FALSE(QFileInfo::exists(tmp.filePath(QStringLiteral("ragoshell.log"))));
}

TEST_F(AppLogTest, install_creates_the_folder_and_lines_carry_time_level_and_text)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString dir = tmp.filePath(QStringLiteral("a/logs"));   // does not exist yet

    ASSERT_TRUE(AppLog::install(dir));
    EXPECT_TRUE(AppLog::isInstalled());
    EXPECT_EQ(AppLog::filePath(), dir + QStringLiteral("/ragoshell.log"));

    AppLog::write(LogSeverity::Info, QStringLiteral("hello log"));
    AppLog::write(LogSeverity::Error, QStringLiteral("it broke"));

    const QStringList lines = readAll(AppLog::filePath()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    ASSERT_EQ(lines.size(), 2);
    EXPECT_TRUE(lines[0].contains(QStringLiteral("[Info] hello log"))) << qPrintable(lines[0]);
    EXPECT_TRUE(lines[1].contains(QStringLiteral("[Error] it broke"))) << qPrintable(lines[1]);
    // "2026-10-28 14:03:21.123 [Info] ..."
    EXPECT_TRUE(QRegularExpression(QStringLiteral(R"(^\d{4}-\d\d-\d\d \d\d:\d\d:\d\d\.\d{3} \[)"))
                    .match(lines[0]).hasMatch()) << qPrintable(lines[0]);
}

TEST_F(AppLogTest, appends_to_the_log_of_an_earlier_run)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());

    ASSERT_TRUE(AppLog::install(tmp.path()));
    AppLog::write(LogSeverity::Info, QStringLiteral("first run"));
    AppLog::uninstall();

    ASSERT_TRUE(AppLog::install(tmp.path()));
    AppLog::write(LogSeverity::Info, QStringLiteral("second run"));

    const QString text = readAll(AppLog::filePath());
    EXPECT_TRUE(text.contains(QStringLiteral("first run")));
    EXPECT_TRUE(text.contains(QStringLiteral("second run")));
}

TEST_F(AppLogTest, qt_messages_are_logged_and_still_reach_the_previous_handler)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qInstallMessageHandler(countingHandler);

    ASSERT_TRUE(AppLog::install(tmp.path()));
    qWarning("disk is %s", "full");
    qInfo("just so you know");

    const QString text = readAll(AppLog::filePath());
    EXPECT_TRUE(text.contains(QStringLiteral("[Warning] disk is full"))) << qPrintable(text);
    EXPECT_TRUE(text.contains(QStringLiteral("[Info] just so you know"))) << qPrintable(text);
    EXPECT_EQ(g_forwarded, 2);
    EXPECT_EQ(g_lastForwarded, QStringLiteral("just so you know"));
}

TEST_F(AppLogTest, uninstall_puts_the_previous_handler_back)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    qInstallMessageHandler(countingHandler);

    ASSERT_TRUE(AppLog::install(tmp.path()));
    const QString path = AppLog::filePath();
    AppLog::uninstall();
    EXPECT_FALSE(AppLog::isInstalled());

    qWarning("after uninstall");
    EXPECT_EQ(g_forwarded, 1);                                   // handler is back
    EXPECT_FALSE(readAll(path).contains(QStringLiteral("after uninstall")));
}

TEST_F(AppLogTest, an_oversized_log_is_rotated_at_install)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString log = tmp.filePath(QStringLiteral("ragoshell.log"));
    {
        QFile f(log);
        ASSERT_TRUE(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(AppLog::kMaxBytes + 1, 'x'));
    }

    ASSERT_TRUE(AppLog::install(tmp.path()));
    AppLog::write(LogSeverity::Info, QStringLiteral("fresh"));

    EXPECT_EQ(QFileInfo(log + QStringLiteral(".1")).size(), AppLog::kMaxBytes + 1);
    EXPECT_LT(QFileInfo(log).size(), 1024);
    EXPECT_TRUE(readAll(log).contains(QStringLiteral("fresh")));
}

TEST_F(AppLogTest, the_log_is_rotated_while_running_and_keeps_one_old_file)
{
    QTemporaryDir tmp;
    ASSERT_TRUE(tmp.isValid());
    const QString log = tmp.filePath(QStringLiteral("ragoshell.log"));
    ASSERT_TRUE(AppLog::install(tmp.path()));

    const QString big(64 * 1024, QLatin1Char('y'));
    for (int i = 0; i < 100; ++i)                  // about 6.4 MB, over the 5 MB limit
        AppLog::write(LogSeverity::Log, big);
    AppLog::write(LogSeverity::Log, QStringLiteral("last line"));

    EXPECT_TRUE(QFileInfo::exists(log + QStringLiteral(".1")));
    EXPECT_FALSE(QFileInfo::exists(log + QStringLiteral(".2")));
    EXPECT_LT(QFileInfo(log).size(), AppLog::kMaxBytes);
    EXPECT_TRUE(readAll(log).contains(QStringLiteral("last line")));
}
