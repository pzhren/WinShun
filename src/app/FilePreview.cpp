#include "FilePreview.h"
#include "SystemTheme.h"

#include "FileIconProvider.h"
#include "FileText.h"
#include "platform/ComWorker.h"
#include "platform/ExternalPreview.h"
#include "platform/PreviewHost.h"

#include <winrt/Windows.Data.Pdf.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.Storage.Streams.h>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QList>
#include <QPainter>
#include <QProcess>
#include <QStandardPaths>
#include <QTimer>
#include <QGuiApplication>
#include <QLocale>
#include <QQuickItem>
#include <QQuickWindow>
#include <QScreen>
#include <QTransform>
#include <QXmlStreamReader>
#include <QtMath>
#include <miniz.h>

#include <windows.h>
#include <propvarutil.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <functional>

using Microsoft::WRL::ComPtr;
using namespace Qt::StringLiterals;

namespace ws {

namespace {

constexpr std::size_t kTextBytes = 1u << 20; // the start of a text file shown: 1 MB
constexpr std::size_t kEditBytes = 8u << 20; // a text file opened for editing: 8 MB at most
constexpr std::size_t kOfficeHtmlBytes = 512u << 10; // a Word/sheet/deck preview as HTML: 512 KB at most (a larger
                                                     // document is read as far as that, then the pane scrolls)
constexpr qsizetype kPieceChars = 2000; // a longer line goes in pieces (one row of the list each)
// Markdown and plain text are edited in the pane; anything else shown as text stays read-only.
const auto editableSuffix = [](const QString& s) {
    return s == u"md" || s == u"markdown" || s == u"txt" || s == u"text";
};
// Word 2007+ is a zip of XML parts: read its text so a Word document previews
// without the Office preview handler (which the machine may not have).
const auto officeWordSuffix = [](const QString& s) {
    return s == u"docx" || s == u"docm";
};
// Excel and PowerPoint 2007+ are zips too: their text previews without the
// Office preview handler as well.
const auto officeArchiveSuffix = [](const QString& s) {
    return s == u"xlsx" || s == u"xlsm" || s == u"pptx" || s == u"pptm";
};
// Word, Excel, PowerPoint (old and 2007+ forms): shown by rendering the file
// to a PDF first (OfficeConvert: LibreOffice or the Office installed here),
// then previewing that PDF.
const auto officeSuffix = [](const QString& s) {
    return s == u"doc" || s == u"docx" || s == u"xls" || s == u"xlsx" || s == u"ppt" || s == u"pptx";
};

// A PDF held open, drawn a page at a time: Windows.Data.Pdf, the system's own
// renderer, so no shell preview handler is involved (this machine's PDF
// handler is WPS's, which fails to load). Made and used on the worker thread
// only: that thread holds COM as an STA (ComWorker), so the apartment asked
// for here must be the one it has — the default (an MTA) throws
// RPC_E_CHANGED_MODE there, which used to make every PDF (and every Office
// document behind it) fail to render.
struct PdfDoc {
    winrt::Windows::Data::Pdf::PdfDocument doc {nullptr};
    QVector<qreal> aspects; // height over width, a page each
    int drawnWidth = 0; // the pixel width the pages drawn so far were made at
};

// Opens the file and reads what each page looks like (points, no rendering):
// the pane lays the pages out at their own shape before any is drawn.
static std::shared_ptr<PdfDoc> openPdf(const QString& path)
{
    // Reading the shapes of a long document is a call a page; a thousand-page
    // file is not worth it (the rest keep the last shape seen).
    constexpr int kAspectsRead = 400;
    try {
        winrt::init_apartment(winrt::apartment_type::single_threaded);
        using namespace winrt::Windows::Storage;
        using namespace winrt::Windows::Storage::Streams;
        using namespace winrt::Windows::Data::Pdf;
        // The file is read whole into memory and let go at once: loaded from
        // a stream, never from the path, or the document holds the file open
        // for as long as it lives (LoadFromFileAsync's share mode has no
        // FILE_SHARE_DELETE) and deleting the previewed file fails with "the
        // file is open in another program" — this program.
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly))
            return {};
        const QByteArray bytes = f.readAll();
        f.close();
        if (bytes.isEmpty())
            return {};
        InMemoryRandomAccessStream stream;
        {
            DataWriter writer(stream);
            writer.WriteBytes(winrt::array_view<const uint8_t>(
                reinterpret_cast<const uint8_t*>(bytes.constData()),
                reinterpret_cast<const uint8_t*>(bytes.constData()) + bytes.size()));
            writer.StoreAsync().get();
            writer.FlushAsync().get();
            writer.DetachStream(); // the stream outlives the writer; its destructor would close it
        }
        stream.Seek(0);
        const auto document = PdfDocument::LoadFromStreamAsync(stream).get();
        const uint32_t count = document.PageCount();
        if (count == 0)
            return {};
        auto pdf = std::make_shared<PdfDoc>();
        pdf->doc = document;
        pdf->aspects.reserve(static_cast<int>(std::min<uint32_t>(count, kAspectsRead)));
        for (uint32_t i = 0; i < count && i < kAspectsRead; ++i) {
            const auto page = document.GetPage(i);
            const auto size = page.Size();
            pdf->aspects.append(size.Width > 0 ? size.Height / size.Width : std::sqrt(2.0));
            page.Close();
        }
        return pdf;
    } catch (...) {
        return {};
    }
}

// Draws one page at this pixel width; empty when it cannot be drawn.
static QImage drawPdfPage(const std::shared_ptr<PdfDoc>& pdf, int index, int pixelWidth)
{
    try {
        using namespace winrt::Windows::Data::Pdf;
        using namespace winrt::Windows::Storage::Streams;
        if (!pdf || index < 0 || index >= static_cast<int>(pdf->doc.PageCount()))
            return {};
        const auto page = pdf->doc.GetPage(static_cast<uint32_t>(index));
        PdfPageRenderOptions options;
        options.DestinationWidth(static_cast<uint32_t>(std::max(50, pixelWidth)));
        InMemoryRandomAccessStream stream;
        page.RenderToStreamAsync(stream, options).get();
        stream.Seek(0);
        const auto reader = DataReader(stream.GetInputStreamAt(0));
        reader.LoadAsync(stream.Size()).get();
        std::vector<uint8_t> bytes(static_cast<size_t>(stream.Size()));
        reader.ReadBytes(bytes);
        page.Close();
        QImage image;
        if (!bytes.empty())
            image.loadFromData(bytes.data(), static_cast<int>(bytes.size()));
        return image;
    } catch (...) {
        return {};
    }
}

// The read miniz asks for: a place in the file, some bytes. Backed by the
// QFile the zip is being read through.
static size_t readOfficeZipAt(void* opaque, mz_uint64 ofs, void* buf, size_t n)
{
    auto* file = static_cast<QFile*>(opaque);
    if (!file || !file->seek(static_cast<qint64>(ofs)))
        return 0;
    return static_cast<size_t>(file->read(static_cast<char*>(buf), static_cast<qint64>(n)));
}

// A 2007+ Office file's zip, open so its entries can be read whole. Small
// files go into memory (the common case, and the file itself is let go at
// once); bigger ones are read where they lie through `open` (miniz here is
// built without stdio, so it is a QFile that reads) — the file is held open
// only until mz_zip_reader_end, and no copy of a document of hundreds of
// megabytes is made. `held` owns the bytes when they are in memory; both it
// and `open` must outlive the zip.
static bool openOfficeZip(const QString& path, mz_zip_archive& zip, QByteArray& held, std::unique_ptr<QFile>& open)
{
    open = std::make_unique<QFile>(path);
    if (!open->open(QIODevice::ReadOnly)) {
        open.reset();
        return false;
    }
    const qint64 total = open->size();
    if (total <= 0 || total > (2ll << 30)) { // nothing in, or something absurd
        open.reset();
        return false;
    }
    if (total <= (64 << 20)) {
        held = open->readAll();
        open.reset();
        return !held.isEmpty()
            && mz_zip_reader_init_mem(&zip, held.constData(), static_cast<size_t>(held.size()), 0) != 0;
    }
    std::memset(&zip, 0, sizeof(zip));
    zip.m_pRead = readOfficeZipAt;
    zip.m_pIO_opaque = open.get();
    if (mz_zip_reader_init(&zip, static_cast<mz_uint64>(total), 0) == 0) {
        open.reset();
        return false;
    }
    return true;
}

// One entry of an open zip, whole; empty when it is not there or would be a
// document of its own to hold (a part of an Office file is kilobytes to a
// few megabytes; anything far past that is not worth the RAM).
static QByteArray officeZipEntry(mz_zip_archive& zip, const char* name)
{
    const int idx = mz_zip_reader_locate_file(&zip, name, nullptr, 0);
    if (idx < 0)
        return {};
    mz_zip_archive_file_stat st {};
    if (!mz_zip_reader_file_stat(&zip, idx, &st) || st.m_uncomp_size > (256u << 20))
        return {};
    size_t size = 0;
    if (void* data = mz_zip_reader_extract_to_heap(&zip, idx, &size, 0)) {
        QByteArray out(static_cast<const char*>(data), static_cast<int>(size));
        mz_free(data);
        return out;
    }
    return {};
}

static bool officeWordText(const QString& path, QString& out, int maxChars)
{
    mz_zip_archive zip {};
    QByteArray held;
    std::unique_ptr<QFile> openFile; // the file a big zip is read through; outlives the zip
    if (!openOfficeZip(path, zip, held, openFile))
        return false;
    const QByteArray bytes = officeZipEntry(zip, "word/document.xml");
    if (bytes.isEmpty()) {
        mz_zip_reader_end(&zip);
        return false;
    }

    // <w:t>…</w:t> inside a <w:p>: the document's text, one paragraph a line.
    QString text;
    QXmlStreamReader xml(bytes);
    while (!xml.atEnd() && text.size() < maxChars) {
        xml.readNext();
        if (xml.isStartElement()) {
            if (xml.name() == QLatin1String("p"))
                text += u'\n';
            else if (xml.name() == QLatin1String("t"))
                text += xml.readElementText();
        }
    }
    mz_zip_reader_end(&zip);
    out = text;
    return true;
}
static bool officeWordHtml(const QString& path, QString& out, int maxChars)
{
    mz_zip_archive zip {};
    QByteArray held;
    std::unique_ptr<QFile> openFile; // the file a big zip is read through; outlives the zip
    if (!openOfficeZip(path, zip, held, openFile))
        return false;
    const QByteArray bytes = officeZipEntry(zip, "word/document.xml");
    if (bytes.isEmpty()) {
        mz_zip_reader_end(&zip);
        return false;
    }

    // A Word file as HTML: headings, paragraphs, their run bold/italic, size
    // and colour, plus tables and list items — so the preview reads close to
    // the document, without the Office preview handler or the Office engine.
    const auto escape = [](const QString& s) {
        QString e = s;
        e.replace(QChar(u'&'), u"&amp;"_s).replace(QChar(u'<'), u"&lt;"_s).replace(QChar(u'>'), u"&gt;"_s);
        return e;
    };
    QString html = u"<html><body style='font-family:Segoe UI,serif;'>"_s;
    QString cur; // the paragraph being read, as HTML
    QString tbl; // a table being read, as HTML
    bool inTable = false, inCell = false;
    bool bold = false, italic = false;
    int sizeHalf = 0;   // w:sz, half-points (24 => 12pt)
    QString color;      // w:color, RRGGBB
    int heading = 0;
    QString align;      // w:jc
    bool listItem = false;
    const auto paraStyle = [&] {
        QString s;
        if (!align.isEmpty())
            s += u"text-align:"_s + align + u";"_s;
        return s;
    };
    const auto flushPara = [&] {
        if (cur.isEmpty())
            return;
        QString inner = cur;
        QString wrapped;
        if (heading >= 1 && heading <= 6)
            wrapped = u"<h%1>%2</h%1>"_s.arg(heading).arg(inner);
        else {
            QString pstyle = paraStyle();
            if (listItem) {
                pstyle += u"margin-left:24px;"_s;
                inner = u"•&nbsp;&nbsp;"_s + inner;
            }
            wrapped = u"<p style='%1'>%2</p>"_s.arg(pstyle).arg(inner);
        }
        if (inCell || inTable)
            tbl += wrapped;
        else
            html += wrapped;
        cur.clear();
    };
    QXmlStreamReader xml(bytes);
    while (!xml.atEnd() && html.size() + tbl.size() < maxChars) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QStringView n = xml.name();
            if (n == u"tbl") {
                flushPara();
                inTable = true;
                tbl += u"<table style='border-collapse:collapse;width:100%;'>"_s;
            } else if (n == u"tr") {
                if (inTable)
                    tbl += u"<tr>"_s;
            } else if (n == u"tc") {
                if (inTable) {
                    flushPara();
                    inCell = true;
                    tbl += u"<td style='border:1px solid #bbbbbb;padding:4px 6px;vertical-align:top;'>"_s;
                }
            } else if (n == u"p") {
                flushPara();
                heading = 0;
                align.clear();
                listItem = false;
            } else if (n == u"pStyle") {
                const QString v = xml.attributes().value(u"val").toString();
                if (v.startsWith(u"Heading"))
                    heading = v.mid(7).toInt();
            } else if (n == u"numPr")
                listItem = true;
            else if (n == u"jc")
                align = xml.attributes().value(u"val").toString();
            else if (n == u"b")
                bold = true;
            else if (n == u"i")
                italic = true;
            else if (n == u"sz")
                sizeHalf = xml.attributes().value(u"val").toInt();
            else if (n == u"color")
                color = xml.attributes().value(u"val").toString();
            else if (n == u"t") {
                QString r = escape(xml.readElementText());
                if (bold)
                    r = u"<b>"_s + r + u"</b>"_s;
                if (italic)
                    r = u"<i>"_s + r + u"</i>"_s;
                if (sizeHalf > 0 || !color.isEmpty()) {
                    QString st = u"font-size:"_s + QString::number(sizeHalf / 2.0) + u"pt;"_s;
                    if (!color.isEmpty())
                        st += u"color:#"_s + color + u";"_s;
                    r = u"<span style='%1'>%2</span>"_s.arg(st).arg(r);
                }
                cur += r;
            }
        } else if (xml.isEndElement()) {
            const QStringView n = xml.name();
            if (n == u"b")
                bold = false;
            else if (n == u"i")
                italic = false;
            else if (n == u"r") { // a run is done: its formatting ends with it
                bold = false;
                italic = false;
                sizeHalf = 0;
                color.clear();
            } else if (n == u"p")
                flushPara();
            else if (n == u"tc") {
                if (inTable) {
                    flushPara();
                    tbl += u"</td>"_s;
                    inCell = false;
                }
            } else if (n == u"tr") {
                if (inTable)
                    tbl += u"</tr>"_s;
            } else if (n == u"tbl") {
                if (inTable) {
                    flushPara();
                    tbl += u"</table>"_s;
                    html += tbl;
                    tbl.clear();
                    inTable = false;
                    inCell = false;
                }
            }
        }
    }
    mz_zip_reader_end(&zip);
    html += u"</body></html>"_s;
    out = html;
    return true;
}
// The thumbnail Office kept in a 2007+ file (docProps/thumbnail.jpeg: the
// first slide or page, small, written when the file was saved). Read from the
// zip at once — no converter, no waiting — so the pane has something of the
// document to show while the PDF of it is being made. Null when the file has
// none (not every program that writes one saves a thumbnail).
static QImage officeThumbnail(const QString& path)
{
    mz_zip_archive zip {};
    QByteArray held;
    std::unique_ptr<QFile> openFile; // the file a big zip is read through; outlives the zip
    if (!openOfficeZip(path, zip, held, openFile))
        return {};
    const QByteArray bytes = officeZipEntry(zip, "docProps/thumbnail.jpeg");
    mz_zip_reader_end(&zip);
    QImage image;
    if (!bytes.isEmpty())
        image.loadFromData(bytes);
    return image;
}

static bool officeArchiveHtml(const QString& path, QString& out, int maxChars)
{
    mz_zip_archive zip {};
    QByteArray held;
    std::unique_ptr<QFile> openFile; // the file a big zip is read through; outlives the zip
    if (!openOfficeZip(path, zip, held, openFile))
        return false;

    const QString suffix = QFileInfo(path).suffix().toLower();
    const bool isXlsx = suffix == u"xlsx" || suffix == u"xlsm";
    const auto escape = [](const QString& s) {
        QString e = s;
        e.replace(QChar(u'&'), u"&amp;"_s).replace(QChar(u'<'), u"&lt;"_s).replace(QChar(u'>'), u"&gt;"_s);
        return e;
    };
    QString html = u"<html><body style='font-family:Segoe UI,serif;'>"_s;
    const mz_uint count = mz_zip_reader_get_num_files(&zip);

    if (isXlsx) {
        // Shared strings first: cells reference them by index.
        QStringList shared;
        for (mz_uint i = 0; i < count; ++i) {
            mz_zip_archive_file_stat st {};
            if (!mz_zip_reader_file_stat(&zip, i, &st))
                continue;
            const QString name = QString::fromUtf8(st.m_filename);
            if (!name.endsWith(u".xml") || !name.contains(u"sharedStrings"))
                continue;
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (!data)
                continue;
            QXmlStreamReader x(QByteArray(static_cast<const char*>(data), static_cast<int>(size)));
            QString item;
            while (!x.atEnd()) {
                x.readNext();
                if (x.isStartElement()) {
                    if (x.name() == u"si")
                        item.clear();
                    else if (x.name() == u"t")
                        item += x.readElementText();
                } else if (x.isEndElement() && x.name() == u"si")
                    shared.append(item);
            }
            mz_free(data);
        }
        // Each worksheet as a table.
        for (mz_uint i = 0; i < count && html.size() < maxChars; ++i) {
            mz_zip_archive_file_stat st {};
            if (!mz_zip_reader_file_stat(&zip, i, &st))
                continue;
            const QString name = QString::fromUtf8(st.m_filename);
            if (!name.endsWith(u".xml") || !name.contains(u"worksheets"))
                continue;
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (!data)
                continue;
            QXmlStreamReader x(QByteArray(static_cast<const char*>(data), static_cast<int>(size)));
            html += u"<table style='border-collapse:collapse;width:100%;'>"_s;
            QString cell;
            bool sharedRef = false, hasCell = false;
            while (!x.atEnd() && html.size() < maxChars) {
                x.readNext();
                if (x.isStartElement()) {
                    const QStringView n = x.name();
                    if (n == u"row")
                        html += u"<tr>"_s;
                    else if (n == u"c") {
                        cell.clear();
                        hasCell = false;
                        sharedRef = (x.attributes().value(u"t") == u"s");
                    } else if (n == u"v" || n == u"t") {
                        cell = x.readElementText();
                        hasCell = true;
                    }
                } else if (x.isEndElement()) {
                    const QStringView n = x.name();
                    if (n == u"c") {
                        QString val = cell;
                        if (sharedRef && hasCell) {
                            const int idx = val.toInt();
                            if (idx >= 0 && idx < shared.size())
                                val = shared.at(idx);
                        }
                        html += u"<td style='border:1px solid #bbbbbb;padding:2px 6px;'>"_s + escape(val) + u"</td>"_s;
                    } else if (n == u"row")
                        html += u"</tr>"_s;
                }
            }
            html += u"</table>"_s;
            mz_free(data);
        }
    } else {
        // Each slide of the deck as a block of paragraphs.
        for (mz_uint i = 0; i < count && html.size() < maxChars; ++i) {
            mz_zip_archive_file_stat st {};
            if (!mz_zip_reader_file_stat(&zip, i, &st))
                continue;
            const QString name = QString::fromUtf8(st.m_filename);
            if (!name.endsWith(u".xml") || !name.contains(u"slides"))
                continue;
            size_t size = 0;
            void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
            if (!data)
                continue;
            QXmlStreamReader x(QByteArray(static_cast<const char*>(data), static_cast<int>(size)));
            html += u"<div style='border-bottom:1px solid #dddddd;padding:8px 0;'>"_s;
            QString para;
            bool inPara = false;
            while (!x.atEnd() && html.size() < maxChars) {
                x.readNext();
                if (x.isStartElement()) {
                    const QStringView n = x.name();
                    if (n == u"p") {
                        inPara = true;
                        para.clear();
                    } else if (n == u"t" && inPara)
                        para += x.readElementText();
                } else if (x.isEndElement()) {
                    if (x.name() == u"p" && inPara) {
                        if (!para.isEmpty())
                            html += u"<p>"_s + escape(para) + u"</p>"_s;
                        inPara = false;
                    }
                }
            }
            html += u"</div>"_s;
            mz_free(data);
        }
    }
    mz_zip_reader_end(&zip);
    html += u"</body></html>"_s;
    out = html;
    return !html.isEmpty();
}
constexpr int kLargestPicture = 4096; // pixels on the longer side; larger ones are scaled down as decoded
constexpr int kThumbnailPx = 512;
constexpr int kFolderItemsCounted = 10000;

// Kinds Windows Imaging Component reads, with the codecs it comes with or a
// common add-on (HEIF, WebP, AVIF, RAW image extensions from the Store).
bool isPicture(const QString& suffix)
{
    static const QStringList kTypes {u"jpg"_s, u"jpeg"_s, u"jpe"_s, u"jfif"_s, u"png"_s, u"gif"_s, u"bmp"_s, u"dib"_s,
        u"tif"_s, u"tiff"_s, u"ico"_s, u"webp"_s, u"heic"_s, u"heif"_s, u"avif"_s, u"jxl"_s, u"jxr"_s, u"wdp"_s, u"hdp"_s,
        u"dds"_s, u"dng"_s, u"cr2"_s, u"cr3"_s, u"nef"_s, u"arw"_s, u"orf"_s, u"rw2"_s, u"raf"_s};
    return kTypes.contains(suffix);
}

// Text whatever handler a program may have put on it (an editor's, a web
// view's): read here at once. The content search's kinds join these.
bool isText(const QString& suffix, const QStringList& more)
{
    static const QStringList kTypes {u"txt"_s, u"text"_s, u"md"_s, u"markdown"_s, u"log"_s, u"ini"_s, u"cfg"_s,
        u"conf"_s, u"config"_s, u"json"_s, u"jsonc"_s, u"json5"_s, u"xml"_s, u"yaml"_s, u"yml"_s, u"toml"_s, u"csv"_s,
        u"tsv"_s, u"c"_s, u"h"_s, u"cc"_s, u"cpp"_s, u"cxx"_s, u"hpp"_s, u"hh"_s, u"hxx"_s, u"inl"_s, u"cs"_s, u"java"_s,
        u"kt"_s, u"kts"_s, u"py"_s, u"pyw"_s, u"js"_s, u"mjs"_s, u"cjs"_s, u"ts"_s, u"tsx"_s, u"jsx"_s, u"go"_s,
        u"rs"_s, u"rb"_s, u"php"_s, u"lua"_s, u"pl"_s, u"swift"_s, u"dart"_s, u"sh"_s, u"bash"_s, u"zsh"_s, u"ps1"_s,
        u"psm1"_s, u"psd1"_s, u"bat"_s, u"cmd"_s, u"sql"_s, u"html"_s, u"htm"_s, u"css"_s, u"scss"_s, u"less"_s,
        u"vue"_s, u"svelte"_s, u"qml"_s, u"cmake"_s, u"gradle"_s, u"properties"_s, u"reg"_s, u"srt"_s, u"ass"_s,
        u"vtt"_s, u"lrc"_s, u"tex"_s, u"bib"_s, u"rst"_s, u"adoc"_s, u"diff"_s, u"patch"_s, u"gitignore"_s,
        u"gitattributes"_s, u"editorconfig"_s, u"env"_s, u"nfo"_s, u"inf"_s, u"manifest"_s, u"sln"_s, u"vcxproj"_s,
        u"csproj"_s, u"props"_s, u"targets"_s, u"svg"_s};
    return kTypes.contains(suffix) || more.contains(suffix);
}

QString sizeText(qint64 bytes)
{
    return QLocale().formattedDataSize(bytes, 1, QLocale::DataSizeTraditionalFormat);
}

QString timeText(const FILETIME& time)
{
    ULARGE_INTEGER t;
    t.LowPart = time.dwLowDateTime;
    t.HighPart = time.dwHighDateTime;
    if (t.QuadPart == 0)
        return {};
    const qint64 ms = static_cast<qint64>((t.QuadPart - 116444736000000000ULL) / 10000);
    return QDateTime::fromMSecsSinceEpoch(ms).toLocalTime().toString(u"yyyy-MM-dd HH:mm"_s);
}

QString typeName(const QString& path, bool isDir)
{
    SHFILEINFOW info {};
    const DWORD attributes = isDir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    // By the name alone (SHGFI_USEFILEATTRIBUTES): never touches the file.
    if (::SHGetFileInfoW(reinterpret_cast<const wchar_t*>(path.utf16()), attributes, &info, sizeof info,
            SHGFI_TYPENAME | SHGFI_USEFILEATTRIBUTES))
        return QString::fromWCharArray(info.szTypeName);
    return {};
}

// The first frame, decoded by Windows Imaging Component and turned upright
// as the camera says (EXIF orientation); at most kLargestPicture on its
// longer side. `original`: its size as stored.
QImage decodePicture(const std::wstring& path, QSize* original)
{
    ComPtr<IWICImagingFactory> wic;
    if (FAILED(::CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))))
        return {};
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)))
        return {};
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame)))
        return {};
    UINT width = 0;
    UINT height = 0;
    if (FAILED(frame->GetSize(&width, &height)) || width == 0 || height == 0)
        return {};
    USHORT orientation = 1;
    ComPtr<IWICMetadataQueryReader> metadata;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
        PROPVARIANT value;
        ::PropVariantInit(&value);
        // The property system's name reads it from JPEG, TIFF and HEIF alike.
        if (SUCCEEDED(metadata->GetMetadataByName(L"System.Photo.Orientation", &value)) && value.vt == VT_UI2)
            orientation = value.uiVal;
        ::PropVariantClear(&value);
    }
    ComPtr<IWICBitmapSource> source = frame;
    UINT w = width;
    UINT h = height;
    if (std::max(width, height) > static_cast<UINT>(kLargestPicture)) {
        const double scale = static_cast<double>(kLargestPicture) / std::max(width, height);
        w = std::max(1u, static_cast<UINT>(width * scale + 0.5));
        h = std::max(1u, static_cast<UINT>(height * scale + 0.5));
        ComPtr<IWICBitmapScaler> scaler;
        if (FAILED(wic->CreateBitmapScaler(&scaler))
            || FAILED(scaler->Initialize(frame.Get(), w, h, WICBitmapInterpolationModeHighQualityCubic)))
            return {};
        source = scaler;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(wic->CreateFormatConverter(&converter))
        || FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0,
            WICBitmapPaletteTypeMedianCut)))
        return {};
    QImage image(static_cast<int>(w), static_cast<int>(h), QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()
        || FAILED(converter->CopyPixels(nullptr, static_cast<UINT>(image.bytesPerLine()), static_cast<UINT>(image.sizeInBytes()),
            image.bits())))
        return {};
    if (original)
        *original = QSize(static_cast<int>(width), static_cast<int>(height));
    switch (orientation) {
    case 2: return image.mirrored(true, false);
    case 3: return image.mirrored(true, true);
    case 4: return image.mirrored(false, true);
    case 5: return image.transformed(QTransform().rotate(90)).mirrored(true, false);
    case 6: return image.transformed(QTransform().rotate(90));
    case 7: return image.transformed(QTransform().rotate(90)).mirrored(false, true);
    case 8: return image.transformed(QTransform().rotate(270));
    default: return image;
    }
}


// The file's Zone.Identifier stream says it came from the internet (or an
// untrusted site): ZoneId 3 or 4.
bool fromTheInternet(const std::wstring& path)
{
    const HANDLE file = ::CreateFileW((path + L":Zone.Identifier").c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    char buffer[1024] {};
    DWORD read = 0;
    const BOOL ok = ::ReadFile(file, buffer, sizeof buffer - 1, &read, nullptr);
    ::CloseHandle(file);
    if (!ok)
        return false;
    const std::string_view text(buffer, read);
    const std::size_t at = text.find("ZoneId=");
    if (at == std::string_view::npos || at + 7 >= text.size())
        return false;
    const char zone = text[at + 7];
    return zone == '3' || zone == '4';
}

// What a folder holds directly: its folders and its files, counted up to a
// limit (a folder of a million files would take long).
std::pair<int, int> countFolder(const QString& path, bool* more, const std::function<bool()>& stale)
{
    int folders = 0;
    int files = 0;
    *more = false;
    const std::wstring pattern = (QString(path).replace(u'/', u'\\') + (path.endsWith(u'\\') ? u"*"_s : u"\\*"_s)).toStdWString();
    WIN32_FIND_DATAW data;
    const HANDLE find = ::FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
        FIND_FIRST_EX_LARGE_FETCH);
    if (find == INVALID_HANDLE_VALUE)
        return {0, 0};
    do {
        const std::wstring_view name(data.cFileName);
        if (name == L"." || name == L"..")
            continue;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            ++folders;
        else
            ++files;
        if (folders + files >= kFolderItemsCounted) {
            *more = true;
            break;
        }
    } while (!stale() && ::FindNextFileW(find, &data));
    ::FindClose(find);
    return {folders, files};
}

} // namespace

// What the worker found out about a file.
struct FilePreview::Loaded {
    Kind kind = Message;
    QImage image; // Picture; Facts: the thumbnail or icon
    QSize imageSize; // Picture: as decoded
    QString summary;
    QStringList facts;
    QStringList lines;
    QVector<int> numbers;
    int lineCount = 0;
    int rows = 0; // Text: lines as shown, long ones wrapped (about)
    QString message;
    QUuid handler; // Handler: the preview handler's CLSID
    QString handlerPath; // Handler: preview this path instead of item.path (an Office doc rendered to PDF)
    QString editText; // Text when editable: the whole file, for the pane's editor
    bool editable = false;
    QString richText; // RichText: a Word file as formatted HTML
    std::shared_ptr<Pdf> pdf; // Picture: a PDF (or an Office document as one), a page at a time
    int pageCount = 0; // Picture: its pages (0: a plain picture)
    // An Office document's first look at it (its thumbnail, its words): the
    // pages are still being made, and replace this when they are.
    bool busy = false;
};

// A PDF held open: the document itself (drawn on the worker thread) and the
// pages drawn so far (kept here so the pane's pictures come from memory).
struct FilePreview::Pdf : PdfDoc {
};

// The pictures QML shows ("image://preview/<n>"): only the latest one is
// kept; and a PDF's pages ("image://preview/page/<index>?r=<n>"), as many as
// the pane has looked at lately.
class FilePreview::Images : public QQuickImageProvider {
public:
    Images()
        : QQuickImageProvider(QQuickImageProvider::Image)
    {
    }
    QImage requestImage(const QString& id, QSize* size, const QSize&) override
    {
        QMutexLocker lock(&m_mutex);
        QImage image;
        if (id.startsWith(u"page/"_s)) {
            const QString path = id.mid(5, id.indexOf(u'?') < 0 ? -1 : id.indexOf(u'?') - 5);
            bool ok = false;
            const int index = path.toInt(&ok);
            if (ok)
                image = m_pages.value(index);
        } else if (id == m_id) {
            image = m_image;
        }
        if (size)
            *size = image.size();
        return image;
    }
    void set(const QString& id, QImage image)
    {
        QMutexLocker lock(&m_mutex);
        m_id = id;
        m_image = std::move(image);
    }
    // A document was opened (or closed): the pages of the one before it go,
    // and a page of that one arriving late is dropped rather than shown.
    void setGeneration(quint64 generation)
    {
        QMutexLocker lock(&m_mutex);
        m_generation = generation;
        m_pages.clear();
        m_order.clear();
    }
    void setPage(quint64 generation, int index, const QImage& image)
    {
        QMutexLocker lock(&m_mutex);
        if (generation != m_generation || image.isNull())
            return;
        constexpr int kPagesKept = 12; // a pane shows two or three; the rest are ahead
        if (!m_pages.contains(index)) {
            m_order.append(index);
            while (m_order.size() > kPagesKept) {
                const int oldest = m_order.takeFirst();
                m_pages.remove(oldest);
            }
        }
        m_pages.insert(index, image);
    }
    bool hasPage(int index)
    {
        QMutexLocker lock(&m_mutex);
        return m_pages.contains(index);
    }

private:
    QMutex m_mutex;
    QString m_id;
    QImage m_image;
    quint64 m_generation = 0;
    QHash<int, QImage> m_pages;
    QList<int> m_order; // oldest first, to let the ones off the pane go
};

FilePreview::FilePreview(QObject* parent)
    : QObject(parent)
    , m_worker(new ComWorker)
    , m_host(std::make_unique<PreviewHost>())
    , m_images(std::make_shared<Images>())
{
    connect(m_host.get(), &PreviewHost::failed, this, &FilePreview::handlerFailed);
    m_slowTimer.setSingleShot(true);
    m_slowTimer.setInterval(250);
    connect(&m_slowTimer, &QTimer::timeout, this, [this] {
        setSlow(true);
        // Taking a while: the pane says so (正在读取…). A window of its own
        // is only for the preview beside the launcher (预览嵌入右侧 off) —
        // shown while embedded, it would sit beside the pane showing the
        // same file twice (this used to happen on every load past 250 ms).
        if (m_open && !m_external && !m_embedded && m_kind == Loading && (!m_window || !m_window->isVisible()))
            showWindow({});
    });
}

FilePreview::~FilePreview()
{
    // m_worker is left running: a thumbnail or a picture codec may still be
    // busy in it at exit, and nothing should wait for that.
}

QQuickImageProvider* FilePreview::imageProvider()
{
    // The engine takes ownership of what it is given: a forwarder, so that
    // the pictures stay ours.
    class Forward : public QQuickImageProvider {
    public:
        explicit Forward(std::shared_ptr<Images> images)
            : QQuickImageProvider(QQuickImageProvider::Image)
            , m_images(std::move(images))
        {
        }
        QImage requestImage(const QString& id, QSize* size, const QSize& requested) override
        {
            return m_images->requestImage(id, size, requested);
        }

    private:
        std::shared_ptr<Images> m_images;
    };
    return new Forward(m_images);
}

void FilePreview::setLauncherWindow(QWindow* launcher)
{
    m_launcher = launcher;
    // It grows into the search panel and back, gets rows as one types, is
    // dragged: the preview goes along (once a frame at most).
    m_relayout.setSingleShot(true);
    m_relayout.setInterval(0);
    connect(&m_relayout, &QTimer::timeout, this, [this] {
        if (m_open && !m_external && m_window && m_window->isVisible() && m_kind != Loading)
            showWindow(m_kind == Picture ? m_imageSize : QSize());
    });
    for (auto signal : {&QWindow::xChanged, &QWindow::yChanged, &QWindow::widthChanged, &QWindow::heightChanged})
        connect(launcher, signal, &m_relayout, qOverload<>(&QTimer::start));
}

void FilePreview::setDark(bool dark)
{
    m_dark = dark;
}

void FilePreview::setEmbedded(bool on)
{
    if (m_embedded == on)
        return;
    m_embedded = on;
    if (on)
        m_builtInOnly = true; // embedded: no external window (QuickLook/Seer)
}

void FilePreview::setTextExtensions(QStringList extensions)
{
    for (QString& e : extensions)
        e = e.toLower();
    m_textExtensions = std::move(extensions);
}

QString FilePreview::iconUrl() const
{
    return m_item.path.isEmpty() ? QString() : FileIconProvider::iconUrl(m_item.path, m_item.isDir);
}

bool FilePreview::isShowing(const QString& path) const
{
    return m_open && m_item.path.compare(path, Qt::CaseInsensitive) == 0;
}

void FilePreview::show(const Item& item)
{
    if (!m_builtInOnly && !m_embedded) {
        if (const std::optional<extpreview::Engine> engine = extpreview::running()) {
            if (m_open && !m_external)
                close(); // ours, opened while none ran
            if (extpreview::show(*engine, item.path)) {
                m_open = true;
                m_external = engine;
                m_externalSince.start();
                m_item = item;
                return;
            }
        }
    }
    if (m_external)
        close();
    m_open = true;
    load(item);
}

void FilePreview::follow(const std::optional<Item>& item)
{
    if (!m_open)
        return;
    if (m_external) {
        // As QuickLook does on Explorer's selection: the next file, while it is up.
        if (item && m_item.path.compare(item->path, Qt::CaseInsensitive) != 0 && extpreview::switchTo(*m_external, item->path))
            m_item = *item;
        return;
    }
    if (!item) {
        ++m_generation;
        m_host->hide();
        m_item = {};
        m_kind = Message;
        m_busy = false;
        m_message = tr("这一行没有可预览的文件");
        m_summary.clear();
        m_slowTimer.stop();
        setSlow(false);
        emit contentChanged();
        return;
    }
    if (m_item.path.compare(item->path, Qt::CaseInsensitive) != 0)
        load(*item);
}

void FilePreview::close()
{
    if (!m_open)
        return;
    m_open = false;
    m_side = -1;
    ++m_generation;
    m_slowTimer.stop();
    if (m_external) {
        extpreview::close(*m_external);
        m_external.reset();
    }
    if (m_host)
        m_host->hide();
    if (m_window)
        m_window->hide();
    m_item = {};
    m_lines.setStringList({});
    m_lineNumbers.clear();
    m_images->set({}, {});
    m_images->setGeneration(m_generation.load()); // the pages drawn go with it
    m_pdf.reset();
    m_pageCount = 0;
    m_busy = false;
    m_imageUrl.clear();
    emit contentChanged();
}

void FilePreview::forgetClosedExternal()
{
    if (!m_open || !m_external || m_externalSince.elapsed() < 1500 || extpreview::visible(*m_external))
        return;
    m_open = false;
    m_external.reset();
    m_item = {};
}

bool FilePreview::foregroundIsOurs() const
{
    const HWND front = ::GetForegroundWindow();
    return m_open && !m_external && front
        && ((m_window && front == reinterpret_cast<HWND>(m_window->winId())) || m_host->owns(front));
}

bool FilePreview::ownsForeground() const
{
    if (!m_open)
        return false;
    const HWND front = ::GetForegroundWindow();
    if (!front)
        return false;
    if (m_external)
        return extpreview::owns(*m_external, front);
    return (m_window && front == reinterpret_cast<HWND>(m_window->winId())) || (m_host && m_host->owns(front));
}

void FilePreview::openFile()
{
    if (!m_item.path.isEmpty())
        emit openRequested(m_item.path);
}

bool FilePreview::saveText(const QString& text)
{
    if (!m_editable || m_item.isDir || m_item.path.isEmpty())
        return false;
    QFile file(m_item.path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray bytes = text.toUtf8();
    const bool ok = file.write(bytes) == bytes.size();
    file.close();
    if (ok)
        m_editText = text;
    return ok;
}

void FilePreview::setSlow(bool slow)
{
    if (m_slow == slow)
        return;
    m_slow = slow;
    emit slowChanged();
}

void FilePreview::load(const Item& item)
{
    const quint64 generation = ++m_generation;
    m_item = item;
    m_kind = Loading;
    m_message.clear();
    m_summary.clear();
    m_facts.clear();
    // The pages of the file before this one go now; a page of it arriving
    // late (the worker still drawing it) is dropped rather than shown.
    m_pdf.reset();
    m_pageCount = 0;
    m_busy = false;
    m_images->setGeneration(generation);
    setSlow(false);
    m_slowTimer.start();
    if (m_host)
        m_host->hide();
    emit contentChanged();

    const QStringList textTypes = m_textExtensions;
    // Who turns a document into the PDF the pane draws (OfficeConvert): the
    // setting as it is now, for this file.
    const office::Engine officeEngine = m_officeEngine;
    // A handler failed on it a moment ago: what is known instead.
    const bool noHandler = m_noHandlerFor.compare(item.path, Qt::CaseInsensitive) == 0;
    QPointer<FilePreview> self(this);
    const auto stale = [self, generation] { return !self || self->m_generation.load() != generation; };
    m_worker->post([self, item, textTypes, officeEngine, noHandler, generation, stale] {
        if (stale())
            return;
        auto loaded = std::make_shared<Loaded>();
        const std::wstring native = QString(item.path).replace(u'/', u'\\').toStdWString();
        WIN32_FILE_ATTRIBUTE_DATA data {};
        if (!::GetFileAttributesExW(native.c_str(), GetFileExInfoStandard, &data)) {
            loaded->message = QCoreApplication::translate("ws::FilePreview", "找不到这个文件，可能已被移动或删除");
        } else {
            const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            const qint64 bytes = (static_cast<qint64>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            const QString suffix = isDir ? QString() : QFileInfo(item.path).suffix().toLower();
            const QString type = typeName(item.path, isDir);
            const QString modified = timeText(data.ftLastWriteTime);
            QStringList summary {type};
            if (!isDir)
                summary.append(sizeText(bytes));
            summary.append(modified);
            const auto fact = [&](const QString& label, const QString& value) {
                if (!value.isEmpty())
                    loaded->facts << label << value;
            };
            constexpr DWORD kNotHere = FILE_ATTRIBUTE_OFFLINE | FILE_ATTRIBUTE_RECALL_ON_OPEN | FILE_ATTRIBUTE_RECALL_ON_DATA_ACCESS;
            const bool cloudOnly = (data.dwFileAttributes & kNotHere) != 0;
            bool done = false;
            if (isDir) {
                bool more = false;
                const auto [folders, files] = countFolder(item.path, &more, stale);
                const QString holds = folders + files == 0 ? QCoreApplication::translate("ws::FilePreview", "空文件夹")
                    : more ? QCoreApplication::translate("ws::FilePreview", "%1 项以上").arg(kFolderItemsCounted)
                    : folders == 0 ? QCoreApplication::translate("ws::FilePreview", "%n 个文件", nullptr, files)
                    : files == 0 ? QCoreApplication::translate("ws::FilePreview", "%n 个文件夹", nullptr, folders)
                                 : QCoreApplication::translate("ws::FilePreview", "%1 个文件夹，%2 个文件").arg(folders).arg(files);
                summary.insert(1, holds);
                fact(QCoreApplication::translate("ws::FilePreview", "包含"), holds);
            } else if (!cloudOnly && isPicture(suffix)) {
                QSize original;
                QImage image = decodePicture(native, &original);
                if (!image.isNull()) {
                    loaded->kind = Picture;
                    loaded->imageSize = image.size();
                    summary.insert(1, u"%1 × %2"_s.arg(original.width()).arg(original.height()));
                    loaded->image = std::move(image);
                    done = true;
                }
            } else if (!cloudOnly && (suffix == u"pdf" || officeSuffix(suffix) || officeWordSuffix(suffix)
                     || officeArchiveSuffix(suffix))) {
                // PDFs are drawn page by page with Windows.Data.Pdf — the
                // system's own renderer, so no shell preview handler is
                // involved (this machine's PDF handler is WPS's, which fails
                // to load, and the pane stays embedded). An Office document
                // goes to a PDF first, through LibreOffice or the Office
                // installed here (OfficeConvert); without either (or should
                // that fail), a 2007+ file's text is read straight out of its
                // XML parts as HTML for the pane's rich-text view. This branch
                // stands before the text one: a document is never text, and
                // the text branch would otherwise take it (it catches files
                // nothing else would show) and leave the documents unseen.
                // A converter is only asked for files under a ceiling: past
                // it the conversion would outlast any wait and the PDF it
                // makes would be a load of memory of its own — those take
                // the text way below instead. (The wait itself is given in
                // proportion to the size: OfficeConvert's convertLimitMs.)
                constexpr qint64 kOfficeConvertMaxBytes = 512ll << 20;
                QString pdfPath;
                if (suffix == u"pdf")
                    pdfPath = item.path;
                else if (bytes <= kOfficeConvertMaxBytes) {
                    // Made before, of this same version of the document: shown
                    // straight away, no converter to wait for.
                    pdfPath = office::cachedPdf(item.path, officeEngine);
                    if (pdfPath.isEmpty()) {
                        // A converter starts cold: seconds of nothing on the
                        // pane. Something of the document goes up first — the
                        // thumbnail Office kept in it, or failing that its
                        // words — and the pages replace it when they come.
                        auto first = std::make_shared<Loaded>();
                        if (const QImage thumb = officeThumbnail(item.path); !thumb.isNull()) {
                            first->kind = Picture;
                            first->image = thumb;
                            first->imageSize = thumb.size();
                        } else if (officeWordSuffix(suffix) || officeArchiveSuffix(suffix)) {
                            QString html;
                            if (officeWordSuffix(suffix)
                                ? officeWordHtml(item.path, html, static_cast<int>(kOfficeHtmlBytes))
                                : officeArchiveHtml(item.path, html, static_cast<int>(kOfficeHtmlBytes))) {
                                first->kind = RichText;
                                first->richText = std::move(html);
                            }
                        }
                        if (first->kind != Message) {
                            first->summary = summary.join(u"  ·  "_s);
                            first->busy = true;
                            QMetaObject::invokeMethod(self, [self, generation, first] {
                                if (self)
                                    self->apply(generation, first);
                            }, Qt::QueuedConnection);
                        }
                        // Moving on to another row ends this conversion where
                        // it stands: a queue of them behind the pane's own
                        // choices would take a minute to drain.
                        pdfPath = office::toPdf(item.path, officeEngine, stale);
                    }
                }
                if (!pdfPath.isEmpty() && QFile::exists(pdfPath)) {
                    // Opened, not drawn: the pane draws the pages it looks at
                    // (requestPage), at its own width and the screen's scale.
                    std::shared_ptr<PdfDoc> opened = openPdf(pdfPath);
                    if (opened) {
                        const int pages = static_cast<int>(opened->doc.PageCount());
                        auto pdf = std::make_shared<Pdf>();
                        pdf->doc = opened->doc;
                        pdf->aspects = opened->aspects;
                        loaded->kind = Picture;
                        loaded->pdf = std::move(pdf);
                        loaded->pageCount = pages;
                        summary.insert(summary.size() - 1,
                            QCoreApplication::translate("ws::FilePreview", "%n 页", nullptr, pages));
                        done = true;
                    }
                }
                if (!done && (officeWordSuffix(suffix) || officeArchiveSuffix(suffix))) {
                    QString html;
                    if (officeWordSuffix(suffix) ? officeWordHtml(item.path, html, static_cast<int>(kOfficeHtmlBytes))
                                                 : officeArchiveHtml(item.path, html, static_cast<int>(kOfficeHtmlBytes))) {
                        loaded->kind = RichText;
                        loaded->richText = std::move(html);
                        // No converter ran (there is none, or it failed, or the
                        // document is too big for one): the words are here, the
                        // page is not. Said under the name, so the plain look is
                        // explained (and the settings say what to install).
                        summary.insert(1, QCoreApplication::translate("ws::FilePreview",
                            bytes > kOfficeConvertMaxBytes && suffix != u"pdf"
                                ? "文字版（文件较大，不渲染页面）"
                                : "文字版"));
                        done = true;
                    }
                }
            } else if (!cloudOnly && (isText(suffix, textTypes) || suffix.isEmpty() || noHandler || !PreviewHost::handlerFor(item.path))) {
                // A text kind; or no extension, or nothing else would show
                // it: text if it reads as text.
                const bool edit = !cloudOnly && editableSuffix(suffix);
                const filetext::Head head = filetext::readHead(native, edit ? kEditBytes : kTextBytes);
                if (head.status == filetext::Status::Ok && head.text.isEmpty()) {
                    loaded->kind = Message;
                    loaded->message = QCoreApplication::translate("ws::FilePreview", "空文件");
                    done = true;
                } else if (head.status == filetext::Status::Ok) {
                    loaded->kind = Text;
                    if (edit && !head.cut) { // small enough: edit it in the pane
                        loaded->editable = true;
                        loaded->editText = head.text;
                    }
                    const QStringList lines = head.text.split(u'\n');
                    int count = static_cast<int>(lines.size());
                    if (count > 1 && lines.last().isEmpty())
                        --count; // the end of the last line, not a line
                    loaded->lineCount = count;
                    for (int i = 0; i < count; ++i) {
                        const QString& line = lines[i];
                        qsizetype at = 0;
                        do { // in pieces, never splitting a surrogate pair
                            qsizetype n = std::min(kPieceChars, line.size() - at);
                            if (at + n < line.size() && line[at + n - 1].isHighSurrogate())
                                --n;
                            loaded->lines.append(line.mid(at, n));
                            loaded->numbers.append(at == 0 ? i + 1 : 0);
                            loaded->rows += std::max<int>(1, static_cast<int>((n + 99) / 100));
                            at += n;
                        } while (at < line.size());
                    }
                    QString encoding;
                    switch (head.encoding) {
                    case filetext::HeadEncoding::Utf8: encoding = u"UTF-8"_s; break;
                    case filetext::HeadEncoding::Utf8Bom: encoding = u"UTF-8 BOM"_s; break;
                    case filetext::HeadEncoding::Utf16LE: encoding = u"UTF-16 LE"_s; break;
                    case filetext::HeadEncoding::Utf16BE: encoding = u"UTF-16 BE"_s; break;
                    case filetext::HeadEncoding::Ansi:
                        encoding = head.codePage == 936 ? u"GBK"_s : head.codePage == 950 ? u"Big5"_s : u"ANSI %1"_s.arg(head.codePage);
                        break;
                    }
                    summary.insert(1, encoding);
                    summary.insert(2, head.cut ? QCoreApplication::translate("ws::FilePreview", "前 %1 行").arg(count)
                                               : QCoreApplication::translate("ws::FilePreview", "%1 行").arg(count));
                    done = true;
                }
            }
            // Downloaded from the internet (the mark of the web): Explorer's
            // preview pane does not hand such a file to its handler (a
            // document can make it fetch from a server, and so give away the
            // user's sign-in, KB5070960); nor does Win顺.
            const bool fromInternet = !done && !isDir && !cloudOnly && fromTheInternet(native);
            if (!done && !isDir && !cloudOnly && !noHandler && !fromInternet) {
                if (const std::optional<QUuid> handler = PreviewHost::handlerFor(item.path)) {
                    loaded->kind = Handler;
                    loaded->handler = *handler;
                    done = true;
                }
            }
            if (!done) {
                loaded->kind = Facts;
                fact(QCoreApplication::translate("ws::FilePreview", "类型"), type);
                if (!isDir)
                    fact(QCoreApplication::translate("ws::FilePreview", "大小"), sizeText(bytes) + (bytes >= 1024 ? u" (%1 B)"_s.arg(QLocale().toString(bytes)) : QString()));
                fact(QCoreApplication::translate("ws::FilePreview", "修改时间"), modified);
                fact(QCoreApplication::translate("ws::FilePreview", "创建时间"), timeText(data.ftCreationTime));
                fact(QCoreApplication::translate("ws::FilePreview", "位置"), QFileInfo(item.path).absolutePath().replace(u'/', u'\\'));
                if (cloudOnly)
                    fact(QCoreApplication::translate("ws::FilePreview", "状态"), QCoreApplication::translate("ws::FilePreview", "仅在云端，未下载到这台电脑"));
                else if (noHandler)
                    fact(QCoreApplication::translate("ws::FilePreview", "说明"),
                        QCoreApplication::translate("ws::FilePreview", "预览程序没能显示这个文件"));
                else if (fromInternet && PreviewHost::handlerFor(item.path))
                    fact(QCoreApplication::translate("ws::FilePreview", "说明"),
                        QCoreApplication::translate("ws::FilePreview", "从网上下载的文件，为安全起见不预览内容（资源管理器也是这样）"));
                else if (!stale())
                    loaded->image = FileIconProvider::largeImage(item.path, kThumbnailPx);
            }
            summary.removeAll(QString());
            loaded->summary = summary.join(u"  ·  "_s);
        }
        QMetaObject::invokeMethod(self, [self, generation, loaded] {
            if (self)
                self->apply(generation, loaded);
        }, Qt::QueuedConnection);
    });
}

void FilePreview::apply(quint64 generation, std::shared_ptr<Loaded> loaded)
{
    if (generation != m_generation.load() || !m_open)
        return;
    m_slowTimer.stop();
    setSlow(false);
    m_kind = loaded->kind;
    m_busy = loaded->busy;
    m_summary = loaded->summary;
    m_facts = loaded->facts;
    m_message = loaded->message;
    m_imageSize = loaded->imageSize;
    m_pdf = std::move(loaded->pdf);
    m_pageCount = loaded->pageCount;
    m_lineNumbers = loaded->numbers;
    m_lineCount = loaded->lineCount;
    m_textRows = loaded->rows;
    m_lines.setStringList(loaded->lines);
    m_editable = loaded->editable;
    m_editText = loaded->editText;
    m_richText = loaded->richText;
    m_handlerPath = loaded->handlerPath;
    const QString id = QString::number(generation);
    m_imageUrl = loaded->image.isNull() ? QString() : u"image://preview/"_s + id;
    m_images->set(id, std::move(loaded->image));
    QSize wanted;
    if (m_kind == Picture && m_pageCount > 0) {
        // A document: a page's shape, at the size a page reads well at.
        const qreal aspect = pageAspect(0);
        wanted = QSize(720, std::clamp(static_cast<int>(720 * aspect), 360, 1400));
    } else if (m_kind == Picture) {
        wanted = m_imageSize;
    }
    if (!m_embedded)
        showWindow(wanted); // embedded: no window of its own, the launcher's pane shows the content
    else if (m_window)
        m_window->hide(); // the pane is the preview: a window left over from another setting goes
    emit contentChanged();
    if (m_kind == Handler)
        startHandler(generation, loaded->handler);
}

// ---- a PDF's pages -----------------------------------------------------------------

qreal FilePreview::pageAspect(int index) const
{
    constexpr qreal kA4 = 1.4142135623730951; // upright, when nothing better is known
    if (!m_pdf || m_pdf->aspects.isEmpty())
        return kA4;
    const int last = static_cast<int>(m_pdf->aspects.size()) - 1;
    // Pages past the ones whose shape was read take the last shape seen.
    return m_pdf->aspects[std::clamp(index, 0, last)];
}

void FilePreview::requestPage(int index, int pixelWidth)
{
    const std::shared_ptr<Pdf> pdf = m_pdf;
    if (!pdf || m_kind != Picture || index < 0 || index >= m_pageCount || pixelWidth <= 0)
        return;
    pixelWidth = std::clamp(pixelWidth, 200, 2400);
    const quint64 generation = m_generation.load();
    QPointer<FilePreview> self(this);
    m_worker->post([self, pdf, index, pixelWidth, generation] {
        if (!self || self->m_generation.load() != generation)
            return;
        // The pane was resized: what was drawn is another width, and goes
        // (the pages on the pane ask for themselves again at the new one).
        if (pdf->drawnWidth != pixelWidth) {
            pdf->drawnWidth = pixelWidth;
            self->m_images->setGeneration(generation);
        }
        if (self->m_images->hasPage(index))
            return; // already drawn at this width
        const QImage page = drawPdfPage(pdf, index, pixelWidth);
        if (page.isNull())
            return;
        self->m_images->setPage(generation, index, page);
        QMetaObject::invokeMethod(self, [self, index] {
            if (self)
                emit self->pageReady(index);
        }, Qt::QueuedConnection);
    });
}

// ---- the window --------------------------------------------------------------------

void FilePreview::showWindow(QSize wanted)
{
    if (!m_window) {
        if (!m_makeWindow)
            return;
        m_window = m_makeWindow();
        if (!m_window)
            return;
        // Moved without changing size: the handler's popup goes along too
        // (a new size reaches it from PreviewWindow.qml).
        connect(m_window, &QWindow::xChanged, this, &FilePreview::placeHandler);
        connect(m_window, &QWindow::yChanged, this, &FilePreview::placeHandler);
    }
    const QRect place = placeBeside(wanted);
    if (place.isValid() && m_window->geometry() != place)
        m_window->setGeometry(place);
    const auto hwnd = reinterpret_cast<HWND>(m_window->winId());
    if (!m_window->isVisible()) {
        // Not activated: the launcher keeps the focus (and so stays open).
        m_window->setProperty("_q_showWithoutActivating", true);
        m_window->show();
        m_window->setProperty("_q_showWithoutActivating", QVariant());
    }
    // Beside the launcher: just under it, both on top of other windows.
    // Over it (no room beside it): in front of it.
    if (m_overLauncher)
        ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    else if (m_launcher && m_launcher->isVisible())
        ::SetWindowPos(hwnd, reinterpret_cast<HWND>(m_launcher->winId()), 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    placeHandler(); // the handler's popup just above it again
}

// The window's place on the launcher's screen, all in logical pixels: beside
// the launcher where there is the most room for `wanted` (right, left or
// below; the work area, not the taskbar), as large as fits there; with no
// room beside it worth the name, over the middle of the screen, under it.
QRect FilePreview::placeBeside(QSize wanted)
{
    QScreen* screen = m_launcher ? m_launcher->screen() : QGuiApplication::primaryScreen();
    if (!screen)
        return {};
    const QRect area = screen->availableGeometry();
    const QRect launcher = m_launcher && m_launcher->isVisible() ? m_launcher->geometry() : QRect();
    constexpr int kGap = 8;
    constexpr int kHeader = 64; // the header row and a margin, about
    constexpr int kPad = 32;
    const int tallest = area.height() - 2 * kGap;
    // What it would like to be.
    QSize want;
    switch (m_kind) {
    case Picture:
        want = QSize(std::max(360, wanted.width() > 0 ? qCeil(wanted.width() / screen->devicePixelRatio()) + kPad : 720),
            std::max(280, wanted.height() > 0 ? qCeil(wanted.height() / screen->devicePixelRatio()) + kPad + kHeader : 540));
        break;
    case Text: {
        // As tall as the text, about (a line of the code font, by 界面大小).
        const QString size = SystemTheme::uiSize();
        const int line = size == u"compact" ? 18 : size == u"large" ? 22 : 20;
        want = QSize(820, std::clamp(kHeader + kPad + m_textRows * line, 280, tallest * 85 / 100));
        break;
    }
    case Handler: want = QSize(900, tallest * 90 / 100); break;
    case Loading:
        if (m_window && m_window->isVisible())
            return m_window->geometry(); // stays as it is until it is known what comes
        want = QSize(560, 460);
        break;
    default: want = QSize(560, 460); break;
    }
    want = want.boundedTo(QSize(area.width() - 2 * kGap, tallest));
    // `want` within `room`; a picture keeps its proportions (narrowed, it
    // is not as tall either: no bands of nothing above and below it).
    const auto fit = [&](QSize room) {
        const QSize bounded = want.boundedTo(room).expandedTo(QSize(std::min(360, room.width()), std::min(280, room.height())));
        if (m_kind != Picture || wanted.isEmpty())
            return bounded;
        const QSizeF picture = QSizeF(wanted) / screen->devicePixelRatio();
        const qreal scale = std::min({1.0, (bounded.width() - kPad) / picture.width(), (bounded.height() - kPad - kHeader) / picture.height()});
        return QSize(std::max(360, qCeil(picture.width() * scale) + kPad), std::max(280, qCeil(picture.height() * scale) + kPad + kHeader))
            .boundedTo(bounded);
    };

    QList<QRect> rooms;
    if (launcher.isValid()) {
        const int top = std::max(area.top() + kGap, launcher.top());
        rooms << QRect(QPoint(launcher.right() + 1 + kGap, top), QPoint(area.right() - kGap, area.bottom() - kGap))
              << QRect(QPoint(area.left() + kGap, top), QPoint(launcher.left() - 1 - kGap, area.bottom() - kGap))
              << QRect(QPoint(std::max(area.left() + kGap, launcher.left()), launcher.bottom() + 1 + kGap),
                     QPoint(area.right() - kGap, area.bottom() - kGap));
    }
    // The side: the one chosen when the preview opened, while the launcher
    // is where it was (the window jumping from side to side as the arrows
    // go through small and large files is hard to follow); else the one
    // with the most room for a document (not for this file, which may be
    // a small picture before a long text).
    const auto usable = [](const QRect& room) { return room.width() >= 360 && room.height() >= 280; };
    // (Its height is not where it is: it changes with the number of rows.)
    const bool moved = m_sideFor.topLeft() != launcher.topLeft() || m_sideFor.width() != launcher.width();
    if (m_side < 0 || moved || (m_side < rooms.size() && !usable(rooms[m_side]))) {
        const QSize document = QSize(820, tallest * 85 / 100).boundedTo(QSize(area.width() - 2 * kGap, tallest));
        qint64 bestArea = 0;
        m_side = static_cast<int>(rooms.size()); // none: over the middle
        // Right or left before below: the rows grow downwards as one types.
        for (int i = 0; i < rooms.size(); ++i) {
            if (!usable(rooms[i]) || (i == 2 && m_side < rooms.size()))
                continue;
            const QSize fits = document.boundedTo(rooms[i].size());
            const qint64 a = static_cast<qint64>(fits.width()) * fits.height();
            if (a > bestArea && a * 2 >= static_cast<qint64>(document.width()) * document.height()) {
                bestArea = a;
                m_side = i;
            }
        }
        m_sideFor = launcher;
    }
    m_overLauncher = m_side >= rooms.size();
    if (m_overLauncher) {
        // Too little room beside it (the search panel, a launcher in the
        // middle of a small screen): over the launcher's right part, in
        // front of it, so that the names on the left stay in sight.
        // At most 60 % of its width: the names stay in sight.
        QRect over(QPoint(), fit(launcher.isValid() ? QSize(std::max(560, launcher.width() * 6 / 10), want.height()) : want));
        if (launcher.isValid()) {
            over.moveRight(std::min(launcher.right() - 24, area.right() - kGap));
            over.moveTop(std::clamp(launcher.center().y() - want.height() / 2, area.top() + kGap, area.bottom() - kGap - want.height()));
            if (over.left() < area.left() + kGap)
                over.moveLeft(area.left() + kGap);
        } else {
            over.moveCenter(area.center());
        }
        return over;
    }
    const QRect& room = rooms[m_side];
    QRect best(room.topLeft(), fit(room.size()));
    // Up against the launcher: from its right edge rightwards, from its left edge leftwards.
    if (room.right() < launcher.left())
        best.moveRight(room.right());
    return best;
}

bool FilePreview::screenRect(QQuickItem* item, int* left, int* top, int* right, int* bottom) const
{
    if (!item)
        return false;
    // The host window: our own (preview window), or the launcher's when embedded.
    QWindow* host = nullptr;
    if (m_embedded)
        host = m_launcher;
    else
        host = m_window;
    if (!host)
        return false;
    // The area's global rectangle comes from QML: mapToScene()/mapToGlobal()
    // on the C++ side stop inside the bound EmbeddedPreview component and lose
    // the pane's offset. Logical global × the window's scale = physical.
    const qreal dpr = host->devicePixelRatio();
    const qreal gx = m_handlerGlobalX, gy = m_handlerGlobalY;
    const qreal gw = m_handlerGlobalW, gh = m_handlerGlobalH;
    *left = qRound(gx * dpr);
    *top = qRound(gy * dpr);
    *right = qRound((gx + gw) * dpr);
    *bottom = qRound((gy + gh) * dpr);
    return *right > *left && *bottom > *top;
}

void FilePreview::setHandlerArea(QQuickItem* area)
{
    m_handlerArea = area;
    placeHandler();
}

void FilePreview::setHandlerGlobal(qreal x, qreal y, qreal w, qreal h)
{
    m_handlerGlobalX = x;
    m_handlerGlobalY = y;
    m_handlerGlobalW = w;
    m_handlerGlobalH = h;
    placeHandler();
}

void FilePreview::placeHandler()
{
    int left, top, right, bottom;
    if (m_kind == Handler && screenRect(m_handlerArea, &left, &top, &right, &bottom))
        m_host->move(left, top, right, bottom);
}

void FilePreview::startHandler(quint64 generation, const QUuid& handler)
{
    const auto showPath = [this]() -> const QString& {
        return m_handlerPath.isEmpty() ? m_item.path : m_handlerPath;
    };
    int left, top, right, bottom;
    const HWND host = m_embedded ? (m_launcher ? reinterpret_cast<HWND>(m_launcher->winId()) : nullptr)
                                 : (m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr);
    if (!host || !screenRect(m_handlerArea, &left, &top, &right, &bottom)) {
        // The embedded pane's size settles a few frames after the handler's
        // kind flips (the launcher just finished its search): retry until the
        // QML-computed global rectangle is ready rather than giving up.
        std::function<void(int)> retry;
        retry = [this, generation, handler, &retry](int attempt) {
            if (generation != m_generation.load() || m_kind != Handler)
                return;
            if (attempt >= 6) { handlerFailed(generation); return; }
            int l, t, r, b;
            const HWND h = m_embedded ? (m_launcher ? reinterpret_cast<HWND>(m_launcher->winId()) : nullptr)
                                      : (m_window ? reinterpret_cast<HWND>(m_window->winId()) : nullptr);
            if (h && screenRect(m_handlerArea, &l, &t, &r, &b))
                m_host->show(generation, m_handlerPath.isEmpty() ? m_item.path : m_handlerPath,
                             handler, l, t, r, b, h, m_dark);
            else
                QTimer::singleShot(40, this, [this, generation, handler, &retry, attempt] {
                    retry(attempt + 1);
                });
        };
        retry(0);
        return;
    }
    m_host->show(generation, showPath(), handler, left, top, right, bottom, host, m_dark);
}

// The handler could not be made, would not take the file, or did not answer
// in time: what is known of the file instead (a thumbnail, its facts).
void FilePreview::handlerFailed(quint64 generation)
{
    if (generation != m_generation.load() || !m_open || m_external)
        return;
    m_noHandlerFor = m_item.path;
    load(m_item);
}

} // namespace ws