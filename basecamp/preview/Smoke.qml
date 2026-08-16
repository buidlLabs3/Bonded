import QtQuick
import QtQuick.Controls
import BondedInbox 1.0

ApplicationWindow {
    width: 720
    height: 480
    visible: false

    BondedInboxPage {
        id: inbox
        anchors.fill: parent
        connectionState: "online"
        rejectionSink: "test-community-sink"
        pendingMessages: [
            { "id": "smoke-message", "sender": "smoke-sender", "state": "bond verified" }
        ]
        activeTasks: [
            { "id": "smoke-task", "metadata": { "logos": { "skill": "paid-task" } },
              "status": { "state": "TASK_STATE_WORKING" } }
        ]
        approvals: [
            { "id": "smoke-approval", "amount": "3", "recipient": "smoke-recipient" }
        ]
    }

    Timer {
        interval: 100
        running: true
        repeat: false
        onTriggered: Qt.quit()
    }
}
