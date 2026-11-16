pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// A PDF (or an Office document converted to one) page by page: a page a row,
// drawn only when the pane first looks at it. The pane tells FilePreview the
// width it has to draw at — its own width times the screen's scale, so the
// text stays sharp on a high-density screen — and shows each page as it
// comes (pageReady); the pages off the pane are not drawn at all, and the
// whole document scrolls (no 30-page cut).
Item {
    id: root

    required property FilePreview preview
    property int padding: 12

    // The width a page is drawn at, in device pixels; in steps, so that a
    // pane resized by a pixel does not draw every page again.
    readonly property int renderWidth: {
        const wanted = (list.width - 2 * padding) * Screen.devicePixelRatio
        return Math.ceil(Math.max(200, Math.min(2400, wanted)) / 80) * 80
    }

    // How many times each page has been drawn: a page drawn again is a new
    // picture at a new URL, and the one on the pane is asked for again.
    property var draws: ({})

    Connections {
        target: root.preview
        function onPageReady(index) {
            const next = Object.assign({}, root.draws)
            next[index] = (next[index] || 0) + 1
            root.draws = next
        }
    }

    ListView {
        id: list
        objectName: "pdfPagesList"
        anchors.fill: parent
        clip: true
        model: root.preview.pageCount
        spacing: 8
        topMargin: root.padding
        bottomMargin: root.padding
        boundsBehavior: Flickable.StopAtBounds
        reuseItems: true

        delegate: Rectangle {
            id: page
            required property int index

            width: list.width - 2 * root.padding
            height: Math.max(40, Math.round(width * root.preview.pageAspect(page.index)))
            radius: 2
            color: "white"
            border.width: 1
            border.color: Theme.divider

            function ask() { root.preview.requestPage(page.index, root.renderWidth) }

            Component.onCompleted: page.ask()
            onIndexChanged: page.ask()

            Connections {
                target: root
                function onRenderWidthChanged() { page.ask() }
            }

            Image {
                anchors.fill: parent
                anchors.margins: 1
                asynchronous: true
                cache: false
                // Drawn at this pane's width times the screen's scale: shown
                // at its own device pixels, however wide the pane is.
                fillMode: Image.Stretch
                source: (root.draws[page.index] || 0) > 0
                    ? "image://preview/page/" + page.index + "?w=" + root.renderWidth
                      + "&r=" + root.draws[page.index] : ""
            }

            // Which page this is, faint in the corner: a long document is
            // easy to lose one's place in.
            Text {
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 6
                text: (page.index + 1) + " / " + root.preview.pageCount
                color: Theme.faint
                font.pixelSize: Theme.uiCaption - 1
            }
        }
    }

    ListScrollBar {
        visible: true
        flickable: list
        anchors.right: parent.right
        anchors.top: list.top
        anchors.bottom: list.bottom
    }
}
