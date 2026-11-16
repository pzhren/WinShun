#pragma once

#include <QString>
#include <QStringList>

#include <functional>

// The programs that turn an Office document into a PDF the preview pane can
// draw page by page (Windows.Data.Pdf does the drawing): LibreOffice's
// headless converter, or the Office installed on this machine (Word, Excel,
// PowerPoint, driven through COM). The pane asks for a PDF; this decides
// which program makes it.
namespace ws::office {

enum class Engine {
    Auto, // LibreOffice when it is there, Office otherwise
    LibreOffice,
    Office,
};

Engine engineFromName(const QString& name); // auto | libreoffice | office
QString engineName(Engine engine);

QString libreOfficePath(); // soffice.exe, empty when it is not installed
bool libreOfficeAvailable();
bool officeAvailable(); // Word, Excel or PowerPoint registered for automation
bool available(Engine engine);

// The PDF of `src`, or an empty string (no engine, or both failed). Cached in
// %TEMP%\winshun_office by name, path and engine, and made again when the
// document is written to. Runs a program of its own; called on the preview's
// worker thread, never the interface's.
//
// `stale` is asked every so often while a converter runs: when it answers true
// the converter is ended there and then, and the PDF of nothing comes back.
// Moving through a list of documents must not leave a queue of conversions
// behind it — each converter starts cold (seconds), so a preview nobody is
// waiting for any more is worth ending at once.
QString toPdf(const QString& src, Engine preferred, const std::function<bool()>& stale = {});

// The PDF of `src` made by a converter before, when it is still of this
// version of the document; empty otherwise. Asking this first makes a
// document previewed again show at once, where toPdf would start a
// converter to find out what is already there.
QString cachedPdf(const QString& src, Engine preferred);

// --office-to-pdf <src> <out>: a copy of this program (so a converter that
// hangs can be ended from outside, which COM cannot be). 0 when the PDF is
// there. Nothing of Qt is started for it — no window, no settings.
int convertCommandLine(const QStringList& args);

// Starts installing LibreOffice — winget, or its download page when winget is
// not there. True when something was started.
bool startLibreOfficeInstall();

} // namespace ws::office
