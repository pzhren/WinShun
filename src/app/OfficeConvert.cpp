#include "OfficeConvert.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace Qt::StringLiterals;

namespace ws::office {

namespace {

// ---- talking to Office ---------------------------------------------------
//
// No type library, no generated headers: the members are asked for by name
// (IDispatch), so nothing here depends on which Office version is installed.
// A member that is not there leaves an empty Value, and the conversion gives
// up rather than guess. Every call is on an STA: Word and PowerPoint insist.

class Value {
public:
    Value() { ::VariantInit(&m_v); }
    explicit Value(VARIANT v) { m_v = v; } // takes it over
    Value(const Value& other)
    {
        ::VariantInit(&m_v);
        ::VariantCopy(&m_v, const_cast<VARIANT*>(&other.m_v));
    }
    Value& operator=(const Value& other)
    {
        if (this != &other) {
            ::VariantClear(&m_v);
            ::VariantInit(&m_v);
            ::VariantCopy(&m_v, const_cast<VARIANT*>(&other.m_v));
        }
        return *this;
    }
    Value(Value&& other) noexcept
    {
        m_v = other.m_v;
        ::VariantInit(&other.m_v);
    }
    Value& operator=(Value&& other) noexcept
    {
        if (this != &other) {
            ::VariantClear(&m_v);
            m_v = other.m_v;
            ::VariantInit(&other.m_v);
        }
        return *this;
    }
    ~Value() { ::VariantClear(&m_v); }

    bool ok() const { return m_v.vt != VT_EMPTY && m_v.vt != VT_ERROR; }
    VARIANT* out() { return &m_v; }
    IDispatch* takeDispatch() // the object a call answered with (null: not one)
    {
        if (m_v.vt != VT_DISPATCH || !m_v.pdispVal)
            return nullptr;
        IDispatch* dispatch = m_v.pdispVal;
        m_v.pdispVal = nullptr;
        m_v.vt = VT_EMPTY;
        return dispatch;
    }

private:
    VARIANT m_v;
};

Value i4(LONG number)
{
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_I4;
    v.lVal = number;
    return Value(v);
}

Value boolean(bool on)
{
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = on ? VARIANT_TRUE : VARIANT_FALSE;
    return Value(v);
}

Value text(const QString& string)
{
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_BSTR;
    v.bstrVal = ::SysAllocString(reinterpret_cast<const OLECHAR*>(string.utf16()));
    return Value(v);
}

class Disp {
public:
    Disp() = default;
    explicit Disp(IDispatch* dispatch) : m_d(dispatch) {}
    Disp(const Disp&) = delete;
    Disp& operator=(const Disp&) = delete;
    Disp(Disp&& other) noexcept : m_d(other.m_d) { other.m_d = nullptr; }
    Disp& operator=(Disp&& other) noexcept
    {
        if (this != &other) {
            reset();
            m_d = other.m_d;
            other.m_d = nullptr;
        }
        return *this;
    }
    ~Disp() { reset(); }

    static Disp create(const wchar_t* progId)
    {
        CLSID clsid;
        if (FAILED(::CLSIDFromProgID(progId, &clsid)))
            return {};
        IDispatch* dispatch = nullptr;
        // A local server: Word, Excel and PowerPoint are programs of their own,
        // not libraries in this process.
        if (FAILED(::CoCreateInstance(clsid, nullptr, CLSCTX_LOCAL_SERVER, IID_IDispatch,
                reinterpret_cast<void**>(&dispatch))))
            return {};
        return Disp(dispatch);
    }

    explicit operator bool() const { return m_d != nullptr; }

    // A property, or a member with no arguments that reads as one
    // (Documents, Presentations, Workbooks).
    Value read(const wchar_t* name) { return invoke(name, {}, DISPATCH_PROPERTYGET); }

    Value call(const wchar_t* name, std::vector<Value> args = {})
    {
        return invoke(name, std::move(args), DISPATCH_METHOD);
    }

    bool put(const wchar_t* name, Value value)
    {
        if (!m_d)
            return false;
        const DISPID id = idOf(name);
        if (id == DISPID_UNKNOWN)
            return false;
        DISPID putId = DISPID_PROPERTYPUT;
        DISPPARAMS params {};
        params.rgvarg = value.out();
        params.cArgs = 1;
        params.rgdispidNamedArgs = &putId;
        params.cNamedArgs = 1;
        return SUCCEEDED(m_d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, DISPATCH_PROPERTYPUT, &params, nullptr,
            nullptr, nullptr));
    }

private:
    void reset()
    {
        if (m_d) {
            m_d->Release();
            m_d = nullptr;
        }
    }

    DISPID idOf(const wchar_t* name)
    {
        if (!m_d)
            return DISPID_UNKNOWN;
        OLECHAR* which = const_cast<OLECHAR*>(name);
        DISPID id = DISPID_UNKNOWN;
        if (FAILED(m_d->GetIDsOfNames(IID_NULL, &which, 1, LOCALE_USER_DEFAULT, &id)))
            return DISPID_UNKNOWN;
        return id;
    }

    Value invoke(const wchar_t* name, std::vector<Value> args, WORD flags)
    {
        const DISPID id = idOf(name);
        if (id == DISPID_UNKNOWN)
            return {};
        // The arguments go in the reverse of the order they are written in,
        // as copies that own what they hold until the call is over.
        std::vector<Value> ordered(args.rbegin(), args.rend());
        std::vector<VARIANT> reversed;
        reversed.reserve(ordered.size());
        for (Value& value : ordered)
            reversed.push_back(*value.out());
        DISPPARAMS params {};
        params.rgvarg = reversed.empty() ? nullptr : reversed.data();
        params.cArgs = static_cast<UINT>(reversed.size());
        VARIANT result;
        ::VariantInit(&result);
        EXCEPINFO fault {};
        const HRESULT hr = m_d->Invoke(id, IID_NULL, LOCALE_USER_DEFAULT, flags, &params, &result, &fault, nullptr);
        if (fault.bstrSource)
            ::SysFreeString(fault.bstrSource);
        if (fault.bstrDescription)
            ::SysFreeString(fault.bstrDescription);
        if (fault.bstrHelpFile)
            ::SysFreeString(fault.bstrHelpFile);
        if (FAILED(hr)) {
            ::VariantClear(&result);
            return {};
        }
        return Value(result);
    }

    IDispatch* m_d = nullptr;
};

// WINSHUN_COM_LOG=<file>: each step of a conversion written there, for when
// one fails and there is no console to say why (a WIN32 program has none).
void trace(const char* step)
{
    static const QByteArray path = qgetenv("WINSHUN_COM_LOG");
    if (path.isEmpty())
        return;
    QFile file(QString::fromLocal8Bit(path));
    if (file.open(QIODevice::Append))
        file.write(step + QByteArray("\n", 1));
}

// ---- the three programs --------------------------------------------------

bool wordToPdf(const QString& src, const QString& out)
{
    Disp word = Disp::create(L"Word.Application");
    if (!word)
        return false;
    word.put(L"Visible", boolean(false));
    word.put(L"DisplayAlerts", i4(0));      // wdAlertsNone: no dialog to answer
    word.put(L"AutomationSecurity", i4(3)); // msoAutomationSecurityForceDisable: no macros
    bool made = false;
    {
        Disp documents = Disp(word.read(L"Documents").takeDispatch());
        if (documents) {
            // Open(FileName, ConfirmConversions, ReadOnly, AddToRecentFiles)
            Disp document = Disp(
                documents.call(L"Open", {text(src), boolean(false), boolean(true), boolean(false)}).takeDispatch());
            if (document) {
                // ExportAsFixedFormat(OutputFileName, ExportFormat: wdExportFormatPDF)
                document.call(L"ExportAsFixedFormat", {text(out), i4(17)});
                made = QFile::exists(out);
                document.call(L"Close", {i4(0)}); // wdDoNotSaveChanges
            }
        }
    }
    word.call(L"Quit", {i4(0)});
    return made;
}

bool excelToPdf(const QString& src, const QString& out)
{
    Disp excel = Disp::create(L"Excel.Application");
    if (!excel)
        return false;
    excel.put(L"Visible", boolean(false));
    excel.put(L"DisplayAlerts", boolean(false));
    bool made = false;
    {
        Disp books = Disp(excel.read(L"Workbooks").takeDispatch());
        if (books) {
            // Open(FileName, UpdateLinks: 0, ReadOnly: True)
            Disp book = Disp(books.call(L"Open", {text(src), i4(0), boolean(true)}).takeDispatch());
            if (book) {
                // ExportAsFixedFormat(Type: xlTypePDF, Filename)
                book.call(L"ExportAsFixedFormat", {i4(0), text(out)});
                made = QFile::exists(out);
                book.call(L"Close", {boolean(false)});
            }
        }
    }
    excel.call(L"Quit");
    return made;
}

bool powerPointToPdf(const QString& src, const QString& out)
{
    trace("powerpoint: creating");
    Disp powerpoint = Disp::create(L"PowerPoint.Application");
    if (!powerpoint) {
        trace("powerpoint: not created");
        return false;
    }
    trace("powerpoint: created");
    powerpoint.put(L"DisplayAlerts", i4(1)); // ppAlertsNone
    bool made = false;
    {
        Disp decks = Disp(powerpoint.read(L"Presentations").takeDispatch());
        trace(decks ? "powerpoint: presentations" : "powerpoint: no presentations");
        if (decks) {
            // Open(FileName, ReadOnly: msoTrue, Untitled: msoFalse,
            // WithWindow: msoFalse) — no window of its own over the desktop.
            Disp deck = Disp(decks.call(L"Open", {text(src), i4(-1), i4(0), i4(0)}).takeDispatch());
            trace(deck ? "powerpoint: opened" : "powerpoint: not opened");
            if (deck) {
                // SaveAs(FileName, FileFormat: ppSaveAsPDF)
                deck.call(L"SaveAs", {text(out), i4(32)});
                made = QFile::exists(out);
                trace(made ? "powerpoint: saved" : "powerpoint: not saved");
                deck.call(L"Close");
            }
        }
    }
    powerpoint.call(L"Quit");
    return made;
}

bool wordSuffix(const QString& suffix)
{
    return suffix == u"doc" || suffix == u"docx" || suffix == u"docm" || suffix == u"dot" || suffix == u"dotx"
        || suffix == u"rtf";
}

bool excelSuffix(const QString& suffix)
{
    return suffix == u"xls" || suffix == u"xlsx" || suffix == u"xlsm" || suffix == u"xlsb" || suffix == u"csv";
}

bool powerPointSuffix(const QString& suffix)
{
    return suffix == u"ppt" || suffix == u"pptx" || suffix == u"pptm" || suffix == u"pps" || suffix == u"ppsx";
}

// A program of Office has to be running to talk to: COM starts it. It insists
// on an apartment, and this process is a short-lived one of its own.
struct Apartment {
    Apartment() { m_ok = SUCCEEDED(::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)); }
    ~Apartment()
    {
        if (m_ok)
            ::CoUninitialize();
    }
    bool m_ok = false;
};

// ---- finding the programs ------------------------------------------------

bool registered(const wchar_t* progId) // a program of Office is one COM can start
{
    CLSID clsid;
    return SUCCEEDED(::CLSIDFromProgID(progId, &clsid));
}

QString shortId(const QString& path)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(path.toLower().toUtf8(), QCryptographicHash::Md5).toHex().left(8));
}

// The file a conversion of `file` by `engine` is written to (the folder of
// the cache is made by convert() when something is to be written).
QString cacheTarget(const QFileInfo& file, Engine engine)
{
    QString base = file.completeBaseName();
    if (base.isEmpty())
        base = u"document"_s;
    base.replace(u'/', u'_').replace(u'\\', u'_');
    return QDir(QDir::temp().filePath(u"winshun_office"_s))
        .filePath(u"%1.%2.%3.pdf"_s.arg(
            base, engine == Engine::Office ? u"office"_s : u"lo"_s, shortId(file.absoluteFilePath())));
}

// Made from this version of the document: the same again is a wait for
// nothing (a converter's start is the slow part).
bool fresh(const QString& pdf, const QFileInfo& file)
{
    return QFile::exists(pdf) && QFileInfo(pdf).lastModified() >= file.lastModified();
}

// Which converters to try, in order: the one asked for first, the other when
// it cannot (uninstalled, or this document beat it).
void engineOrder(Engine preferred, Engine (&order)[2])
{
    order[0] = Engine::LibreOffice;
    order[1] = Engine::Office;
    if (preferred == Engine::Office) {
        order[0] = Engine::Office;
        order[1] = Engine::LibreOffice;
    } else if (preferred == Engine::Auto && !libreOfficeAvailable()) {
        order[0] = Engine::Office;
        order[1] = Engine::LibreOffice;
    }
}

// Waits for a converter to end, or ends it when the preview has moved on:
// true when it ended by itself.
bool finishOrAbandon(QProcess& process, int limitMs, const std::function<bool()>& stale)
{
    constexpr int kStepMs = 100;
    for (int waited = 0; waited < limitMs; waited += kStepMs) {
        if (process.waitForFinished(kStepMs))
            return process.exitStatus() == QProcess::NormalExit;
        if (stale && stale()) {
            process.kill();
            process.waitForFinished(3000);
            return false;
        }
    }
    process.kill();
    process.waitForFinished(3000);
    return false;
}

// How long a converter is given with a document of this size: the cold start
// (seconds, whatever the document) and then time to read it — about a second
// per megabyte for a deck of pictures (a 95 MB deck of slides: forty seconds
// here), so a fixed ninety is only enough for a small one. A conversion the
// pane has moved on from is ended at once, whatever this says.
int convertLimitMs(const QFileInfo& file, int baseMs)
{
    constexpr qint64 kPerMbMs = 2500;
    constexpr qint64 kCapMs = 900000;
    const qint64 mb = std::max<qint64>(1, (file.size() + (1ll << 20) - 1) >> 20);
    return static_cast<int>(std::min<qint64>(kCapMs, baseMs + mb * kPerMbMs));
}

// The PDF `src` makes through `engine`, from the cache when it is still of
// this version of the document, or empty.
QString convert(const QString& src, Engine engine, const std::function<bool()>& stale)
{
    const QFileInfo file(src);
    const QString folder = QDir::temp().filePath(u"winshun_office"_s);
    if (!QDir().mkpath(folder))
        return {};
    const QString out = cacheTarget(file, engine);
    if (QFile::exists(out)) {
        if (fresh(out, file))
            return out;
        QFile::remove(out);
    }
    if (engine == Engine::LibreOffice) {
        const QString soffice = libreOfficePath();
        if (soffice.isEmpty())
            return {};
        // Its own folder: LibreOffice names the PDF after the document, and
        // two documents of one name must not land on each other.
        const QString work = QDir(folder).filePath(shortId(file.absoluteFilePath()) + u".lo"_s);
        if (!QDir().mkpath(work))
            return {};
        QProcess process;
        process.setProgram(soffice);
        process.setArguments({u"--headless"_s, u"--norestore"_s, u"--convert-to"_s, u"pdf"_s, u"--outdir"_s,
            work, src});
        process.start();
        if (!finishOrAbandon(process, convertLimitMs(file, 90000), stale))
            return {};
        const QString made = QDir(work).filePath(file.completeBaseName() + u".pdf"_s);
        if (!QFile::exists(made) || !QFile::rename(made, out))
            return {};
        return out;
    }
    // Office: a copy of this program, so a converter that hangs can be ended
    // from here (a COM call of its own cannot be waited out).
    QProcess process;
    process.setProgram(QCoreApplication::applicationFilePath());
    process.setArguments({u"--office-to-pdf"_s, src, out});
    process.start();
    if (!process.waitForStarted(10000))
        return {};
    if (!finishOrAbandon(process, convertLimitMs(file, 120000), stale))
        return {};
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QFile::exists(out) ? out : QString();
}

} // namespace

Engine engineFromName(const QString& name)
{
    const QString lower = name.trimmed().toLower();
    if (lower == u"libreoffice")
        return Engine::LibreOffice;
    if (lower == u"office")
        return Engine::Office;
    return Engine::Auto;
}

QString engineName(Engine engine)
{
    switch (engine) {
    case Engine::LibreOffice: return u"libreoffice"_s;
    case Engine::Office: return u"office"_s;
    case Engine::Auto: break;
    }
    return u"auto"_s;
}

QString libreOfficePath()
{
    const QString onPath = QStandardPaths::findExecutable(u"soffice"_s);
    if (!onPath.isEmpty())
        return onPath;
    for (const QString& path : {u"C:/Program Files/LibreOffice/program/soffice.exe"_s,
             u"C:/Program Files (x86)/LibreOffice/program/soffice.exe"_s}) {
        if (QFile::exists(path))
            return path;
    }
    return {};
}

bool libreOfficeAvailable()
{
    return !libreOfficePath().isEmpty();
}

bool officeAvailable()
{
    return registered(L"Word.Application") || registered(L"Excel.Application")
        || registered(L"PowerPoint.Application");
}

bool available(Engine engine)
{
    switch (engine) {
    case Engine::LibreOffice: return libreOfficeAvailable();
    case Engine::Office: return officeAvailable();
    case Engine::Auto: return libreOfficeAvailable() || officeAvailable();
    }
    return false;
}

QString toPdf(const QString& src, Engine preferred, const std::function<bool()>& stale)
{
    const QFileInfo file(src);
    if (!file.exists() || file.isDir())
        return {};
    Engine order[2];
    engineOrder(preferred, order);
    for (Engine engine : order) {
        if (stale && stale())
            return {};
        if (!available(engine))
            continue;
        const QString pdf = convert(src, engine, stale);
        if (!pdf.isEmpty())
            return pdf;
    }
    return {};
}

QString cachedPdf(const QString& src, Engine preferred)
{
    const QFileInfo file(src);
    if (!file.exists() || file.isDir())
        return {};
    Engine order[2];
    engineOrder(preferred, order);
    for (Engine engine : order) {
        const QString pdf = cacheTarget(file, engine);
        if (fresh(pdf, file))
            return pdf;
    }
    return {};
}

int convertCommandLine(const QStringList& args)
{
    const int at = args.indexOf(u"--office-to-pdf"_s);
    if (at < 0 || at + 2 >= args.size())
        return 2;
    const QString src = QDir::toNativeSeparators(args.at(at + 1));
    const QString out = QDir::toNativeSeparators(args.at(at + 2));
    if (!QFile::exists(src))
        return 3;
    const Apartment apartment;
    const QString suffix = QFileInfo(src).suffix().toLower();
    bool made = false;
    if (wordSuffix(suffix))
        made = wordToPdf(src, out);
    else if (excelSuffix(suffix))
        made = excelToPdf(src, out);
    else if (powerPointSuffix(suffix))
        made = powerPointToPdf(src, out);
    return made && QFile::exists(out) ? 0 : 1;
}

bool startLibreOfficeInstall()
{
    const auto shellOpen = [](const wchar_t* file, const wchar_t* arguments) {
        return reinterpret_cast<std::intptr_t>(
                   ::ShellExecuteW(nullptr, L"open", file, arguments, nullptr, SW_SHOWNORMAL))
            > 32;
    };
    // winget first: one click, no browser to walk through. It is an app
    // execution alias under WindowsApps, which is a file of nothing to start
    // with, so the full path is looked for before the name is handed to the
    // shell.
    QString winget = QDir::fromNativeSeparators(QString::fromLocal8Bit(qgetenv("LOCALAPPDATA")))
        + u"/Microsoft/WindowsApps/winget.exe"_s;
    if (!QFile::exists(winget))
        winget = QStandardPaths::findExecutable(u"winget"_s);
    if (!winget.isEmpty()) {
        const QString arguments = u"install --id TheDocumentFoundation.LibreOffice -e --silent "
                                  "--disable-interactivity --accept-package-agreements --accept-source-agreements"_s;
        if (shellOpen(reinterpret_cast<const wchar_t*>(winget.utf16()), reinterpret_cast<const wchar_t*>(arguments.utf16())))
            return true;
    }
    return shellOpen(L"https://www.libreoffice.org/download/download-libreoffice/", nullptr);
}

} // namespace ws::office
