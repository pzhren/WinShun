#include "Settings.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDir>
#include <QFile>

using namespace Qt::StringLiterals;
using namespace ws;

// The file types' own excluded folders: read from the file, and written
// back unchanged (the 文件类型 page's 排除路径, Settings [FileTypes]
// N\ExcludedFolders). 搜索范围's own exclusions are plain string lists, so
// what matters here is that each type keeps its own.
class FileTypeExclusions : public QObject {
    Q_OBJECT

private:
    QString writeIni(QStringList lines)
    {
        static int n = 0;
        // The profile's settings live in <profile>\WinShun\WinShun.ini.
        const QString folder = QStringLiteral("D:/tmp/winshun-tst-filetypes-%1").arg(++n);
        QDir().mkpath(folder);
        QDir(folder + u"/WinShun"_s).removeRecursively(); // nothing stale from an earlier run
        QDir().mkpath(folder + u"/WinShun"_s);
        const QString file = folder + u"/WinShun/WinShun.ini"_s;
        QFile f(file);
        if (f.open(QFile::WriteOnly | QFile::Text))
            f.write(lines.join(u'\n').toUtf8() + '\n');
        f.close();
        Settings::useProfile(folder);
        return file;
    }

private slots:
    void initTestCase()
    {
        // Without a profile every Settings call would touch the user's file.
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        Settings::useProfile(temp.path());
    }

    void readsThemBack()
    {
        // In the INI file a value's backslash is doubled (QSettings escapes
        // it); the key keeps its single one.
        writeIni({u"[FileTypes]"_s, u"size=2"_s,
            u"1\\Id=pictures"_s, u"1\\Extensions=jpg png"_s,
            u"1\\ExcludedFolders=C:\\\\Windows, C:\\\\Users\\\\me\\\\Pictures"_s,
            u"2\\Name=我的类型"_s, u"2\\Extensions=txt"_s, u"2\\ExcludedFolders=%USERPROFILE%\\\\Desktop"_s});
        Settings settings;
        settings.load();
        QCOMPARE(settings.fileTypes.size(), 2);
        QCOMPARE(settings.fileTypes[0].id, u"pictures"_s);
        QCOMPARE(settings.fileTypes[0].excludedPaths, QStringList({u"C:\\Windows"_s, u"C:\\Users\\me\\Pictures"_s}));
        QCOMPARE(settings.fileTypes[1].name, u"我的类型"_s);
        QCOMPARE(settings.fileTypes[1].excludedPaths, QStringList({u"%USERPROFILE%\\Desktop"_s})); // as written: expanded for the search
    }

    void keepsThemThroughSave()
    {
        const QString file = writeIni({u"[FileTypes]"_s, u"size=1"_s, u"1\\Name=图片"_s, u"1\\Extensions=jpg"_s,
            u"1\\ExcludedFolders=D:\\\\照片"_s});
        QFile written(file);
        written.open(QFile::ReadOnly);
        qInfo() << "ini on disk:" << written.readAll();
        written.close();
        Settings settings;
        settings.load();
        qInfo() << "loaded:" << settings.fileTypes[0].excludedPaths;
        QCOMPARE(settings.fileTypes[0].excludedPaths, QStringList({u"D:\\照片"_s}));
        settings.fileTypes[0].excludedPaths.append(u"E:\\图"_s);
        settings.save();
        Settings back;
        back.load();
        QCOMPARE(back.fileTypes.size(), 1);
        QCOMPARE(back.fileTypes[0].excludedPaths, QStringList({u"D:\\照片"_s, u"E:\\图"_s}));
    }

    void anEmptyListIsFine()
    {
        writeIni({u"[FileTypes]"_s, u"size=1"_s, u"1\\Name=文档"_s, u"1\\Extensions=txt"_s});
        Settings settings;
        settings.load();
        QCOMPARE(settings.fileTypes.size(), 1);
        QVERIFY(settings.fileTypes[0].excludedPaths.isEmpty());
        settings.save();
        Settings back;
        back.load();
        QVERIFY(back.fileTypes[0].excludedPaths.isEmpty());
    }

    void aBuiltInKeepsItsOwn()
    {
        writeIni({u"[FileTypes]"_s, u"size=1"_s, u"1\\Id=videos"_s, u"1\\Extensions=mp4"_s});
        Settings settings;
        settings.load();
        qInfo() << "path:" << Settings::filePath() << "excluded:" << settings.fileTypes[0].excludedPaths;
        QCOMPARE(settings.fileTypes.size(), 1);
        QCOMPARE(settings.fileTypes[0].excludedPaths, QStringList()); // nothing in the file: none
        // Renaming or adding exclusions of a built-in one: its Id stays, so
        // it shows in the language shown (namedFileTypes).
        settings.fileTypes[0].excludedPaths.append(u"D:\\视频"_s);
        settings.save();
        Settings back;
        back.load();
        QCOMPARE(back.fileTypes[0].id, u"videos"_s);
        QCOMPARE(back.fileTypes[0].excludedPaths, QStringList({u"D:\\视频"_s}));
        QCOMPARE(back.namedFileTypes()[0].name, Settings::defaults().namedFileTypes()[2].name); // 视频, in the language shown
    }
};

QTEST_MAIN(FileTypeExclusions)
#include "tst_filetypes.moc"
