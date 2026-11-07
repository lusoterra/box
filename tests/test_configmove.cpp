// Config move with stub: settings.ini + boorus.json relocate, default
// folder keeps only the pointer stub. Pure filesystem helpers, no GUI,
// no network (QCoreApplication-safe).
#include "settings.h"

#include <QDir>
#include <QFile>
#include <QSettings>
#include <QTemporaryDir>
#include <QtTest>

class TestConfigMove : public QObject {
    Q_OBJECT
private slots:
    void moveCopiesAndWritesStub() {
        QTemporaryDir from, to;
        QVERIFY(from.isValid());
        QVERIFY(to.isValid());
        {
            QFile f(from.filePath(QStringLiteral("settings.ini")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("[app]\nlanguage=en_GB\n");
            f.close();
        }
        {
            QFile f(from.filePath(QStringLiteral("settings.ini.bak")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("bak");
            f.close();
        }
        {
            QFile f(from.filePath(QStringLiteral("boorus.json")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("[]");
            f.close();
        }
        QVERIFY(moveConfigFiles(from.path(), to.path()).isEmpty());
        QVERIFY(QFile::exists(to.filePath(QStringLiteral("settings.ini"))));
        QVERIFY(QFile::exists(to.filePath(QStringLiteral("settings.ini.bak"))));
        QVERIFY(QFile::exists(to.filePath(QStringLiteral("boorus.json"))));
        // Source files stay (copy semantics); the stub replaces the
        // default location's settings.ini.
        QVERIFY(QFile::exists(from.filePath(QStringLiteral("settings.ini"))));
        QVERIFY(writeConfigStub(from.path(), to.path()).isEmpty());
        QSettings stub(from.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);
        QCOMPARE(stub.value(QStringLiteral("app/configDir")).toString(),
                 to.path());
        // Stub only: no stray settings keys remain.
        QVERIFY(!stub.contains(QStringLiteral("app/language")));
    }

    void sameDirIsNoop() {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        QVERIFY(moveConfigFiles(d.path(), d.path()).isEmpty());
        QVERIFY(writeConfigStub(d.path(), d.path()).isEmpty());
    }

    void emptyDestFails() {
        QTemporaryDir d;
        QVERIFY(d.isValid());
        QVERIFY(!moveConfigFiles(d.path(), QString()).isEmpty());
        QVERIFY(!writeConfigStub(d.path(), QString()).isEmpty());
        QVERIFY(!writeConfigStub(QString(), d.path()).isEmpty());
    }
};

QTEST_MAIN(TestConfigMove)
#include "test_configmove.moc"
