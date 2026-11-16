#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QMutex>
#include <QObject>
#include <QPointer>
#include <QQuickImageProvider>
#include <QQuickWindow>
#include <QRect>
#include <QSize>
#include <QStringList>
#include <QStringListModel>
#include <QTimer>
#include <QUuid>
#include <QVector>
#include <QtQml/qqmlregistration.h>

#include "OfficeConvert.h"

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QQuickItem;
class QWindow;

namespace ws {

class ComWorker;
class PreviewHost;
namespace extpreview {
enum class Engine;
}

// A look at a result's file without opening it (Alt+P on a row, 预览 in its
// menu; the forum's #157, #162, #225): QuickLook (QL-Win) or Seer when one of
// them runs in the background and the settings leave it to them (预览方式:
// 自动), else Win顺's own window (PreviewWindow.qml): a picture (Windows
// Imaging Component decodes it, any format Windows reads), the start of a
// text file (decoded as the content search decodes), a document through
// the preview handler Explorer's preview pane uses (PreviewHost), else a
// large thumbnail and what is known of the file; a folder, its icon and
// what it holds.
//
// The launcher stays open and keeps the keyboard (the window never takes
// the focus): Alt+P again or Esc closes the preview, the arrows show the
// next row's file in it (follow). Reading the file is never done on the GUI
// thread; a newer request makes an older one's result go unseen.
class FilePreview : public QObject {
    Q_OBJECT
    QML_ELEMENT
    QML_UNCREATABLE("Provided by the application")
    Q_PROPERTY(Kind kind READ kind NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString name READ name NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString path READ path NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString iconUrl READ iconUrl NOTIFY contentChanged FINAL)
    // A line under the name: the type, the size, when written, and per kind
    // the picture's size, the text's encoding ("UTF-8 · 120 行"), …
    Q_PROPERTY(QString summary READ summary NOTIFY contentChanged FINAL)
    // Picture: the decoded picture (pixels); Facts: a thumbnail or the icon.
    Q_PROPERTY(QString imageUrl READ imageUrl NOTIFY contentChanged FINAL)
    Q_PROPERTY(QSize imageSize READ imageSize NOTIFY contentChanged FINAL)
    // A PDF (or an Office document converted to one), shown a page at a time:
    // the pane asks for the pages it shows (requestPage) and draws them as
    // they come (pageReady, imageUrl of the page). pageCount 0: a plain
    // picture, imageUrl.
    Q_PROPERTY(int pageCount READ pageCount NOTIFY contentChanged FINAL)
    // Text: a line a row, long ones in pieces (lineNumberAt: 0 for a piece
    // that goes on from the row before).
    Q_PROPERTY(QStringListModel* lines READ lines CONSTANT FINAL)
    Q_PROPERTY(int lineCount READ lineCount NOTIFY contentChanged FINAL)
    // A text file that is safe to edit in the pane (markdown, plain text):
    // editText is the whole file, saveText writes it back to the disk.
    Q_PROPERTY(bool editable READ editable NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString editText READ editText NOTIFY contentChanged FINAL)
    // RichText: a document (a Word file) as formatted HTML for the pane.
    Q_PROPERTY(QString richText READ richText NOTIFY contentChanged FINAL)
    // Facts: label, value, label, value …
    Q_PROPERTY(QStringList facts READ facts NOTIFY contentChanged FINAL)
    Q_PROPERTY(QString message READ message NOTIFY contentChanged FINAL) // Message: why nothing shows
    Q_PROPERTY(bool slow READ slow NOTIFY slowChanged FINAL) // Loading for a moment already
    // An Office document's pages are still being made (a converter of its own
    // takes seconds to start): a first look at the file — the thumbnail
    // Office saved in it, or its words — shows meanwhile, and the pages
    // replace it when they are ready.
    Q_PROPERTY(bool busy READ busy NOTIFY contentChanged FINAL)

public:
    enum Kind { Loading, Picture, Text, RichText, Handler, Facts, Message };
    Q_ENUM(Kind)

    struct Item {
        QString path;
        QString name;
        bool isDir = false;
    };

    explicit FilePreview(QObject* parent = nullptr);
    ~FilePreview() override;

    // The app's part: the window (made the first time it is wanted, with
    // its frame ready), the launcher it goes beside, the settings.
    void setWindowFactory(std::function<QQuickWindow*()> factory) { m_makeWindow = std::move(factory); }
    void setLauncherWindow(QWindow* launcher);
    void setBuiltInOnly(bool on) { m_builtInOnly = on; } // 预览方式: 内置
    // Embedded: the content is shown inside the launcher's window (the split
    // pane on the right), not in a window of its own; show/follow still load
    // the file, apply updates the properties, but no window is created.
    void setEmbedded(bool on);
    void setDark(bool dark);
    // Text files besides the usual ones (the content search's kinds).
    void setTextExtensions(QStringList extensions);
    // Which program turns an Office document into the PDF the pane draws:
    // LibreOffice, the Office installed here, or whichever there is
    // (OfficeConvert). Read when the next document is opened.
    void setOfficeEngine(office::Engine engine) { m_officeEngine = engine; }

    bool isOpen() const { return m_open; }
    // Shown in QuickLook or Seer, which the user closed there (its own
    // Space or Esc, a click): no longer open here either. Not every key
    // (Seer is asked through a message), only when it matters: Alt+P, Esc.
    void forgetClosedExternal();
    bool isShowing(const QString& path) const;
    void show(const Item& item);
    // Open: the current row's file now (none: a row that is not a file).
    void follow(const std::optional<Item>& item);
    Q_INVOKABLE void close();
    // The window in front belongs to the preview (ours, or the external
    // previewer's): the launcher is not to go for that.
    bool ownsForeground() const;
    bool foregroundIsOurs() const; // ...and it is a window of Win顺's own (handed back to the launcher)
    QQuickWindow* window() const { return m_window; }

    Kind kind() const { return m_kind; }
    QString name() const { return m_item.name; }
    QString path() const { return m_item.path; }
    QString iconUrl() const;
    QString summary() const { return m_summary; }
    QString imageUrl() const { return m_imageUrl; }
    QSize imageSize() const { return m_imageSize; }
    int pageCount() const { return m_pageCount; }
    // A page's height over its width (points): the pane lays a page out at
    // its own shape before it is drawn. 1.414 (A4 upright) when unknown.
    Q_INVOKABLE qreal pageAspect(int index) const;
    // Have this page drawn at this pixel width (the pane's width times the
    // screen's scale), if it is not drawn already; pageReady follows.
    Q_INVOKABLE void requestPage(int index, int pixelWidth);
    QStringListModel* lines() { return &m_lines; }
    int lineCount() const { return m_lineCount; }
    Q_INVOKABLE int lineNumberAt(int row) const { return row >= 0 && row < m_lineNumbers.size() ? m_lineNumbers[row] : 0; }
    bool editable() const { return m_editable; }
    QString editText() const { return m_editText; }
    QString richText() const { return m_richText; }
    Q_INVOKABLE bool saveText(const QString& text); // write the edited text back to the file
    QStringList facts() const { return m_facts; }
    QString message() const { return m_message; }
    bool slow() const { return m_slow; }
    bool busy() const { return m_busy; }

    Q_INVOKABLE void openFile(); // 打开 in the header: as Enter on the row
    // The content area, where a preview handler draws (PreviewWindow.qml).
    Q_INVOKABLE void setHandlerArea(QQuickItem* area);
    // The handler area's rectangle in screen (global) coordinates, logical:
    // computed in QML, where mapToGlobal is correct — the C++ side loses the
    // pane's offset inside the bound EmbeddedPreview component.
    Q_INVOKABLE void setHandlerGlobal(qreal x, qreal y, qreal w, qreal h);

    // "image://preview/<n>": the picture, or the thumbnail, of the file shown.
    QQuickImageProvider* imageProvider();

signals:
    void contentChanged();
    void slowChanged();
    void pageReady(int index); // the page asked for is drawn: imageUrl shows it now
    void openRequested(const QString& path); // 打开: the launcher opens it, as Enter would

private:
    struct Loaded; // what the worker found out
    struct Pdf; // an open PDF: its pages, and the ones drawn so far
    class Images;

    void load(const Item& item);
    void apply(quint64 generation, std::shared_ptr<Loaded> loaded);
    void showWindow(QSize wanted); // logical
    QRect placeBeside(QSize wanted); // the window's place by the launcher (logical)
    void placeHandler();
    void startHandler(quint64 generation, const QUuid& handler);
    void handlerFailed(quint64 generation);
    bool screenRect(QQuickItem* item, int* left, int* top, int* right, int* bottom) const; // physical
    void showExternal(const Item& item);
    void setSlow(bool slow);

    std::function<QQuickWindow*()> m_makeWindow;
    QPointer<QQuickWindow> m_window;
    QPointer<QWindow> m_launcher;
    QPointer<QQuickItem> m_handlerArea;
    qreal m_handlerGlobalX = 0, m_handlerGlobalY = 0, m_handlerGlobalW = 0, m_handlerGlobalH = 0;
    bool m_builtInOnly = false;
    bool m_embedded = false; // the pane inside the launcher, no window of its own
    bool m_dark = false;
    QStringList m_textExtensions;

    bool m_open = false;
    std::optional<extpreview::Engine> m_external; // open in QuickLook or Seer
    QElapsedTimer m_externalSince; // its window comes a moment after it is asked
    Item m_item;
    std::atomic<quint64> m_generation {0};
    ComWorker* m_worker = nullptr; // never deleted: a shell call may still be running at exit
    std::unique_ptr<PreviewHost> m_host;
    std::shared_ptr<Images> m_images;

    Kind m_kind = Loading;
    QString m_summary;
    QString m_imageUrl;
    QSize m_imageSize;
    // A PDF shown page by page: only the pages the pane is looking at are
    // drawn (the worker draws them, one at a time, when asked).
    std::shared_ptr<Pdf> m_pdf;
    int m_pageCount = 0;
    QStringListModel m_lines;
    QVector<int> m_lineNumbers;
    int m_lineCount = 0;
    int m_textRows = 0;
    bool m_editable = false; // a text file shown with an editor (markdown, plain text)
    QString m_editText; // the whole file, for the editor to save
    QString m_richText; // RichText: a document (a Word file) as formatted HTML
    int m_side = -1; // placeBeside: right, left, below, or over the middle; -1 not chosen yet
    QRect m_sideFor; // the launcher's place when it was chosen
    bool m_overLauncher = false; // no room beside it: in front of it
    QStringList m_facts;
    QString m_message;
    bool m_slow = false;
    bool m_busy = false; // an Office document's pages are being made; a first look shows
    QTimer m_slowTimer;
    QTimer m_relayout; // the launcher moved or changed size: placed again
    QString m_noHandlerFor; // a handler failed on it: shown as facts instead
    QString m_handlerPath; // Handler: preview this file path instead of m_item.path (an Office doc rendered to PDF)
    office::Engine m_officeEngine = office::Engine::Auto; // who turns a document into a PDF
};

} // namespace ws
