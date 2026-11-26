pragma ComponentBehavior: Bound
import QtQuick
import WinShun

// Adding or changing one of the footer's engines (网页搜索 › 搜索框底部的
// 引擎): its name, as its button in the launcher's footer says it, and the
// address it searches, with %s where the words typed in the box go (without
// one, its button opens the page as it is). SettingsEditor checks and saves
// it, or says what is wrong. Enter saves, Esc puts it away.
FocusScope {
    id: form

    required property SettingsEditor editor
    property int index: -1 // the one being changed; -1: a new one
    property var shortcut: ({ name: "", url: "" })
    property string error

    signal finished()

    function close() {
        finished()
    }

    function save() {
        form.error = form.editor.saveFooterEngine(form.index, nameField.text, urlField.text)
        if (form.error.length === 0)
            form.close()
    }

    Rectangle { // modal backdrop: page clicks and wheel events stay here
        anchors.fill: parent
        color: Qt.rgba(0, 0, 0, Theme.dark ? 0.5 : 0.3)
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.AllButtons
            hoverEnabled: true
            onWheel: (wheel) => wheel.accepted = true
        }
    }

    Rectangle {
        id: card
        anchors.centerIn: parent
        width: Math.min(560, parent.width - 32)
        height: Math.min(parent.height - 24, heading.implicitHeight + fields.implicitHeight + footer.implicitHeight + 20 + 16 + 12)
        radius: Theme.largeCardRadius
        color: Theme.menuBackground
        border.width: 1
        border.color: Theme.windowEdge

        Keys.onEscapePressed: form.close()

        Text {
            id: heading
            x: 20
            y: 20
            width: parent.width - 40
            text: form.index < 0 ? qsTr("添加底部引擎") : qsTr("编辑底部引擎")
            color: Theme.text
            font.pixelSize: Theme.fontTitle
            font.weight: Font.DemiBold
        }

        Column {
            id: fields
            x: 20
            anchors.top: heading.bottom
            anchors.topMargin: 12
            width: parent.width - 40
            spacing: 10

            Column {
                width: parent.width
                spacing: 4

                Text {
                    width: parent.width
                    text: qsTr("名称")
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
                InputBox {
                    id: nameField
                    width: parent.width
                    text: form.shortcut.name
                    placeholder: qsTr("谷歌学术")
                    onAccepted: urlField.takeFocus()
                }
            }

            Column {
                width: parent.width
                spacing: 4

                Text {
                    width: parent.width
                    text: qsTr("网址")
                    color: Theme.text
                    font.pixelSize: Theme.fontCaption
                }
                InputBox {
                    id: urlField
                    width: parent.width
                    text: form.shortcut.url
                    placeholder: qsTr("https://scholar.google.com/scholar?q=%s")
                    onAccepted: form.save()
                }
                Text {
                    width: parent.width
                    text: qsTr("要搜的文字用 %s 代替；网址里没有 %s 时，按这个网址直接打开")
                    color: Theme.subtext
                    font.pixelSize: Theme.fontCaption
                    wrapMode: Text.Wrap
                    lineHeight: 1.15
                }
            }
        }

        Column {
            id: footer
            x: 20
            width: parent.width - 40
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 16
            spacing: 6

            Text {
                objectName: "footerEngineFormError"
                visible: form.error.length > 0
                width: parent.width
                text: form.error
                textFormat: Text.PlainText
                color: Theme.danger
                font.pixelSize: Theme.fontCaption
                wrapMode: Text.Wrap
            }

            Row {
                anchors.right: parent.right
                spacing: 8

                FlatButton {
                    objectName: "footerEngineFormCancel"
                    text: qsTr("取消")
                    onClicked: form.close()
                }
                FlatButton {
                    id: saveButton
                    objectName: "footerEngineFormSave"
                    highlighted: true
                    text: form.index < 0 ? qsTr("添加") : qsTr("保存")
                    onClicked: form.save()
                }
            }
        }

        Component.onCompleted: nameField.takeFocus()
    }
}
