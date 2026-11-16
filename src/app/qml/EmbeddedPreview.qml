pragma ComponentBehavior: Bound

import QtQuick
import WinShun

// Win顺's preview of a result shown inside the launcher's own window: the
// right-hand pane of the split view (Settings::previewEmbedded). The same
// rendering as PreviewWindow.qml — a picture fitted in (the wheel zooms, a
// double click toggles fitted and its own size), the start of a text file, a
// document through the handler Explorer's preview pane uses (PreviewHost, a
// window of its own over the content area), or a large thumbnail and what is
// known about the file. The content follows the current row (FilePreview).
Item {
    id: root

    required property FilePreview preview

    readonly property int padding: 12

    // The launcher moved under the preview (the host handler's window is a
    // window of its own): push it along so it stays over this pane.
    function reflow() { if (handlerArea.shown) handlerArea.placeGlobal() }

    // ---- the header: the file and what it is -------------------------------

    Item {
        id: header
        width: parent.width
        height: Theme.headerHeight

        Image {
            id: icon
            x: root.padding
            anchors.verticalCenter: parent.verticalCenter
            width: 20
            height: 20
            sourceSize: Qt.size(20 * Screen.devicePixelRatio, 20 * Screen.devicePixelRatio)
            source: root.preview.iconUrl
            asynchronous: true
            smooth: false
        }
        Column {
            anchors.left: icon.right
            anchors.leftMargin: 8
            anchors.right: parent.right
            anchors.rightMargin: root.padding
            y: Math.round((header.height - height) / 2)
            spacing: 1

            Text {
                width: parent.width
                text: root.preview.name
                textFormat: Text.PlainText
                elide: Text.ElideMiddle
                color: Theme.text
                font.pixelSize: Theme.uiTitle
                font.weight: Font.DemiBold
            }
            Text {
                width: parent.width
                visible: text.length > 0
                text: root.preview.summary
                textFormat: Text.PlainText
                elide: Text.ElideRight
                color: Theme.subtext
                font.pixelSize: Theme.uiCaption
            }
        }
        Rectangle {
            anchors.bottom: parent.bottom
            width: parent.width
            height: 1
            color: Theme.divider
        }
    }

    // ---- the content ---------------------------------------------------------

    Item {
        id: body
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        clip: true

        // A picture: fitted in, never larger than its own size; the wheel
        // zooms about the pointer, a double click toggles fitted and its own size.
        Flickable {
            id: picture
            readonly property bool shown: root.preview.kind === FilePreview.Picture
                && root.preview.pageCount === 0
            readonly property size natural: Qt.size(root.preview.imageSize.width / Screen.devicePixelRatio,
                                                    root.preview.imageSize.height / Screen.devicePixelRatio)
            readonly property real fitScale: natural.width > 0 && natural.height > 0
                ? Math.min(1, (width - 2 * root.padding) / natural.width, (height - 2 * root.padding) / natural.height) : 1
            property real zoom: 0 // 0: fitted
            readonly property real scale: zoom > 0 ? zoom : fitScale
            readonly property bool atNative: Math.abs(scale - 1) < 0.0001

            function zoomAt(factor, px, py) { // px, py: in the view
                const old = scale
                const next = Math.max(Math.min(fitScale, 0.05), Math.min(16, old * factor))
                if (Math.abs(next - old) < 0.0001)
                    return
                const cx = (contentX + px - image.x) / old
                const cy = (contentY + py - image.y) / old
                zoom = next
                returnToBounds()
                contentX = Math.max(0, Math.min(contentWidth - width, image.x + cx * next - px))
                contentY = Math.max(0, Math.min(contentHeight - height, image.y + cy * next - py))
            }

            visible: shown
            anchors.fill: parent
            contentWidth: Math.max(width, natural.width * scale + 2 * root.padding)
            contentHeight: Math.max(height, natural.height * scale + 2 * root.padding)
            boundsBehavior: Flickable.StopAtBounds
            interactive: zoom > 0 && (contentWidth > width || contentHeight > height)

            Connections {
                target: root.preview
                function onContentChanged() { picture.zoom = 0; picture.contentX = 0; picture.contentY = 0 }
            }

            Image {
                id: image
                x: Math.max(root.padding, Math.round((picture.contentWidth - width) / 2))
                y: Math.max(root.padding, Math.round((picture.contentHeight - height) / 2))
                width: Math.round(picture.natural.width * picture.scale)
                height: Math.round(picture.natural.height * picture.scale)
                source: picture.shown ? root.preview.imageUrl : ""
                cache: false
                fillMode: Image.Stretch
                smooth: !picture.atNative
                mipmap: !picture.atNative && picture.scale < 1
            }

            WheelHandler {
                acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
                onWheel: (event) => {
                    const steps = event.angleDelta.y / 120
                    if (steps !== 0)
                        picture.zoomAt(Math.pow(1.2, steps), event.x, event.y)
                }
            }
            TapHandler {
                onDoubleTapped: (eventPoint) => {
                    if (picture.zoom > 0)
                        picture.zoom = 0
                    else
                        picture.zoomAt(1 / picture.scale, eventPoint.position.x, eventPoint.position.y)
                }
            }
        }

        // A PDF (or an Office document as one) page by page: a page a row,
        // drawn when it comes into view (PdfPages).
        PdfPages {
            id: pages
            readonly property bool shown: root.preview.kind === FilePreview.Picture
                && root.preview.pageCount > 0
            preview: root.preview
            padding: root.padding
            anchors.fill: parent
            visible: shown
        }

        // The start of a text file, a line a row (long ones in pieces): numbered, monospaced, wrapped.
        // (Markdown and plain text are editable below, so this is the read-only view for the rest.)
        ListView {
            id: lines
            readonly property bool shown: root.preview.kind === FilePreview.Text && !root.preview.editable
            readonly property int digits: Math.max(2, String(root.preview.lineCount).length)
            readonly property real numberWidth: digitMetrics.advanceWidth * digits + 12

            visible: shown
            anchors.fill: parent
            anchors.topMargin: 8
            anchors.bottomMargin: 8
            clip: true
            model: shown ? root.preview.lines : null
            boundsBehavior: Flickable.StopAtBounds
            reuseItems: true

            TextMetrics {
                id: digitMetrics
                font: lineFont.font
                text: "0"
            }
            Text { id: lineFont; visible: false; font.family: "Consolas"; font.pixelSize: Theme.uiCaption + 1 }

            delegate: Item {
                id: line
                required property string display
                required property int index
                readonly property int number: root.preview.lineNumberAt(index)
                width: lines.width
                height: lineText.height

                Text {
                    width: lines.numberWidth
                    horizontalAlignment: Text.AlignRight
                    text: line.number > 0 ? line.number : ""
                    color: Theme.faint
                    font: lineFont.font
                }
                Text {
                    id: lineText
                    x: lines.numberWidth + 12
                    width: lines.width - x - root.padding
                    text: line.display
                    textFormat: Text.PlainText
                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    color: Theme.text
                    font: lineFont.font
                }
            }
        }
        ListScrollBar {
            visible: lines.shown
            flickable: lines
            anchors.right: parent.right
            anchors.top: lines.top
            anchors.bottom: lines.bottom
        }

        // A markdown or plain-text file: edit it right here and save it back.
        Item {
            id: editor
            readonly property bool shown: root.preview.kind === FilePreview.Text && root.preview.editable
            readonly property bool dirty: editArea.text !== root.preview.editText
            visible: shown
            anchors.fill: parent
            anchors.margins: 8
            clip: true

            Flickable {
                id: editScroll
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.bottom: saveBar.top
                contentWidth: editArea.width
                contentHeight: editArea.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                TextEdit {
                    id: editArea
                    width: editScroll.width
                    height: Math.max(editScroll.height, contentHeight)
                    wrapMode: TextEdit.Wrap
                    selectByMouse: true
                    persistentSelection: true
                    cursorVisible: true
                    text: root.preview.editText
                    color: Theme.text
                    selectionColor: Theme.textSelection
                    font.family: "Consolas"
                    font.pixelSize: Theme.uiBody
                    padding: 4
                }
            }
            ListScrollBar {
                visible: true
                flickable: editScroll
                anchors.right: editScroll.right
                anchors.top: editScroll.top
                anchors.bottom: editScroll.bottom
            }

            Rectangle { // the save bar under the editor
                id: saveBar
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 30
                color: Theme.backdrop ? "transparent" : Theme.selection
                Row {
                    anchors.left: parent.left
                    anchors.leftMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    spacing: 8
                    Text {
                        text: editor.dirty ? qsTr("未保存") : qsTr("已保存")
                        color: editor.dirty ? Theme.accent : Theme.subtext
                        font.pixelSize: Theme.uiCaption
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                Rectangle { // Save
                    anchors.right: parent.right
                    anchors.rightMargin: 6
                    anchors.verticalCenter: parent.verticalCenter
                    width: 52
                    height: 22
                    radius: 3
                    color: editor.dirty ? Theme.accent : Theme.selection
                    border.width: 1
                    border.color: editor.dirty ? Theme.accent : Theme.divider
                    Text {
                        anchors.centerIn: parent
                        text: qsTr("保存")
                        color: editor.dirty ? "white" : Theme.subtext
                        font.pixelSize: Theme.uiCaption
                    }
                    MouseArea {
                        anchors.fill: parent
                        enabled: editor.dirty
                        cursorShape: editor.dirty ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: root.preview.saveText(editArea.text)
                    }
                }
            }
        }

        // A document through its preview handler: drawn by the handler in a
        // window of its own placed over this area (PreviewHost).
        Item {
            id: handlerArea
            readonly property bool shown: root.preview.kind === FilePreview.Handler
            anchors.fill: parent
            anchors.margins: 8
            visible: shown
            function placeGlobal() {
                const p = handlerArea.mapToGlobal(Qt.point(0, 0))
                root.preview.setHandlerGlobal(p.x, p.y, handlerArea.width, handlerArea.height)
            }
            onShownChanged: if (shown) { root.preview.setHandlerArea(handlerArea); placeGlobal() }
            onWidthChanged: if (shown) placeGlobal()
            onHeightChanged: if (shown) placeGlobal()
        }

        // An Office document as formatted HTML (Word, sheets, slides): shown
        // on a white "page" like a document, black text that reads as the
        // file, wrapped to the pane's width and scrollable.
        Item {
            id: richPane
            readonly property bool shown: root.preview.kind === FilePreview.RichText
            visible: shown
            anchors.fill: parent
            anchors.margins: 8
            clip: true

            Flickable {
                id: richScroll
                anchors.fill: parent
                contentWidth: page.width
                contentHeight: page.height
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                Rectangle { // the white page the document sits on
                    id: page
                    width: richScroll.width
                    height: Math.max(richScroll.height, richText.height + 2 * root.padding)
                    color: "white"
                    Text {
                        id: richText
                        x: root.padding
                        y: root.padding
                        width: page.width - 2 * root.padding
                        textFormat: Text.RichText
                        text: root.preview.richText
                        color: "black"
                        font.pixelSize: Theme.uiBody
                        wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                    }
                }
            }
            ListScrollBar {
                visible: true
                flickable: richScroll
                anchors.right: richScroll.right
                anchors.top: richScroll.top
                anchors.bottom: richScroll.bottom
            }
        }

        // A thumbnail (or the icon) and what is known about it: a folder, or
        // a file nothing else shows.
        Column {
            id: facts
            readonly property bool shown: root.preview.kind === FilePreview.Facts
            visible: shown
            anchors.centerIn: parent
            width: Math.min(parent.width - 2 * root.padding, 520)
            spacing: 16

            Image {
                id: thumbnail
                anchors.horizontalCenter: parent.horizontalCenter
                readonly property real side: Math.min(256, body.height - factsTable.height - 3 * root.padding)
                width: Math.max(0, Math.min(side, implicitWidth / Screen.devicePixelRatio))
                height: implicitWidth > 0 ? width * implicitHeight / implicitWidth : 0
                visible: status === Image.Ready && implicitWidth > 0
                source: facts.shown ? root.preview.imageUrl : ""
                cache: false
                smooth: true
                mipmap: true
            }
            Grid {
                id: factsTable
                anchors.horizontalCenter: parent.horizontalCenter
                columns: 2
                columnSpacing: 16
                rowSpacing: 6

                Repeater {
                    model: facts.shown ? root.preview.facts : []
                    delegate: Text {
                        required property var modelData
                        required property int index
                        text: modelData
                        textFormat: Text.PlainText
                        color: index % 2 === 0 ? Theme.subtext : Theme.text
                        font.pixelSize: Theme.uiBody
                        horizontalAlignment: index % 2 === 0 ? Text.AlignRight : Text.AlignLeft
                        width: index % 2 === 0 ? implicitWidth : Math.min(implicitWidth, facts.width - 120)
                        wrapMode: index % 2 === 0 ? Text.NoWrap : Text.WrapAtWordBoundaryOrAnywhere
                        maximumLineCount: 3
                        elide: Text.ElideRight
                    }
                }
            }
        }

        // Reading it, a moment after it began; or why there is nothing to show.
        Text {
            anchors.centerIn: parent
            width: parent.width - 2 * root.padding
            horizontalAlignment: Text.AlignHCenter
            visible: text.length > 0
            text: root.preview.kind === FilePreview.Loading ? (root.preview.slow ? qsTr("正在读取…") : "")
                : root.preview.kind === FilePreview.Message ? root.preview.message : ""
            wrapMode: Text.Wrap
            color: Theme.faint
            font.pixelSize: Theme.uiBody
        }

        // An Office document's pages are still being made (its converter
        // starts cold, seconds of it — a deck of a hundred megabytes is the
        // better part of a minute): what shows meanwhile is the file's own
        // thumbnail or its words, and this says so — the pages take its place.
        Rectangle {
            id: busyBadge
            readonly property bool shown: root.preview.busy
            property int seconds: 0
            onShownChanged: {
                seconds = 0
                if (shown)
                    busyTimer.restart()
                else
                    busyTimer.stop()
            }
            visible: shown
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 12
            width: busyLabel.implicitWidth + 24
            height: busyLabel.implicitHeight + 12
            radius: Theme.controlRadius
            color: Theme.card
            border.color: Theme.cardBorder
            z: 10

            Timer {
                id: busyTimer
                interval: 1000
                repeat: true
                running: false
                onTriggered: ++busyBadge.seconds
            }

            Text {
                id: busyLabel
                anchors.centerIn: parent
                text: busyBadge.seconds >= 5
                    ? qsTr("正在渲染页面… %1 秒（大文件要等上一会儿）").arg(busyBadge.seconds)
                    : qsTr("正在渲染页面…")
                color: Theme.subtext
                font.pixelSize: Theme.uiBody
            }
        }
    }
}
