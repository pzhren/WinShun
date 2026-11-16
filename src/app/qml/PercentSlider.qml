import QtQuick
import WinShun

// A 0…100 slider with its value beside it, in the Windows 11 look: a thin
// track, the part taken in the accent colour and a round thumb. Dragged,
// clicked, wheeled, and stepped from the keyboard (←/→ by `step`).
// It never changes `value` itself: bind it and handle moved().
Item {
    id: slider

    property int value: 0
    property int step: 5
    property string valueSuffix: "%"
    signal moved(int value)

    readonly property int from: 0
    readonly property int to: 100

    implicitWidth: 260
    implicitHeight: 32
    activeFocusOnTab: true
    opacity: enabled ? 1 : 0.45

    function setFromX(x) {
        const span = track.width
        if (span <= 0)
            return
        const ratio = Math.max(0, Math.min(1, (x - track.x) / span))
        const next = Math.round(slider.from + ratio * (slider.to - slider.from))
        if (next !== slider.value)
            slider.moved(next)
    }

    Keys.onLeftPressed: slider.moved(Math.max(slider.from, slider.value - slider.step))
    Keys.onRightPressed: slider.moved(Math.min(slider.to, slider.value + slider.step))
    Keys.onDownPressed: slider.moved(Math.max(slider.from, slider.value - slider.step * 2))
    Keys.onUpPressed: slider.moved(Math.min(slider.to, slider.value + slider.step * 2))
    Keys.onPressed: (event) => {
        if (event.key === Qt.Key_Home) {
            slider.moved(slider.from)
            event.accepted = true
        } else if (event.key === Qt.Key_End) {
            slider.moved(slider.to)
            event.accepted = true
        }
    }

    Rectangle { // the track
        id: track
        anchors.left: parent.left
        anchors.right: valueText.left
        anchors.rightMargin: 16
        anchors.verticalCenter: parent.verticalCenter
        height: 4
        radius: Theme.smallRadius * 2
        color: Theme.track

        Rectangle { // the part taken
            width: Math.round(parent.width * (slider.value - slider.from) / Math.max(1, slider.to - slider.from))
            height: parent.height
            radius: parent.radius
            color: slider.enabled ? Theme.accentFill : Theme.subtext
        }
    }

    Rectangle { // the thumb, at the value
        readonly property bool big: area.pressed || area.containsMouse || slider.activeFocus

        x: track.x + Math.round(track.width * (slider.value - slider.from) / Math.max(1, slider.to - slider.from)) - width / 2
        anchors.verticalCenter: parent.verticalCenter
        width: big ? 20 : 16
        height: width
        radius: width / 2
        color: slider.enabled ? Theme.accentFill : Theme.subtext
        border.width: big ? 2 : 0
        border.color: Theme.card

        Behavior on width { NumberAnimation { duration: 90; easing.type: Easing.OutCubic } }
    }

    Text { // the value, in a column of its own so the track keeps its length
        id: valueText
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        width: 44
        horizontalAlignment: Text.AlignRight
        text: slider.value + slider.valueSuffix
        color: Theme.text
        font.pixelSize: Theme.fontCaption
    }

    Rectangle { // keyboard focus
        visible: slider.activeFocus
        anchors.fill: parent
        anchors.margins: -3
        radius: Theme.controlRadius + 3
        color: "transparent"
        border.width: 2
        border.color: Theme.accent
    }

    MouseArea {
        id: area
        anchors.left: track.left
        anchors.right: track.right
        anchors.leftMargin: -10 // the thumb reaches past the track
        anchors.rightMargin: -10
        anchors.top: parent.top
        anchors.bottom: parent.bottom
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton
        onPressed: (mouse) => {
            slider.forceActiveFocus()
            slider.setFromX(area.x + mouse.x)
        }
        onPositionChanged: (mouse) => {
            if (pressed)
                slider.setFromX(area.x + mouse.x)
        }
        onWheel: (wheel) => {
            const delta = wheel.angleDelta.y > 0 ? slider.step : -slider.step
            slider.moved(Math.max(slider.from, Math.min(slider.to, slider.value + delta)))
        }
    }
}
