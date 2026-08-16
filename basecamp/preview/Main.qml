import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import BondedInbox 1.0

ApplicationWindow {
    id: window
    width: 1120
    height: 720
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: qsTr("Bonded Inbox Preview")

    property string activity: qsTr("Fixture backend online")

    function withoutId(items, id) {
        return items.filter(function(item) { return item.id !== id })
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        BondedInboxPage {
            id: inbox
            Layout.fillWidth: true
            Layout.fillHeight: true
            connectionState: "online"
            rejectionSink: "LEZ Community Safety Pool"
            pendingMessages: [
                { "id": "msg-1042", "sender": "lez1...8f3a", "state": "bond verified" },
                { "id": "msg-1049", "sender": "lez1...21cd", "state": "review required" },
                { "id": "msg-1051", "sender": "lez1...c104", "state": "policy checked" }
            ]
            activeTasks: [
                { "id": "task-77", "metadata": { "logos": { "skill": "attachment-scan" } },
                  "status": { "state": "TASK_STATE_COMPLETED" } },
                { "id": "task-81", "metadata": { "logos": { "skill": "paid-task" } },
                  "status": { "state": "TASK_STATE_INPUT_REQUIRED" } }
            ]
            approvals: [
                { "id": "proposal-31", "amount": "3", "recipient": "lez1...bb40" },
                { "id": "proposal-32", "amount": "4", "recipient": "lez1...a112" }
            ]

            onRefreshRequested: {
                loading = true
                refreshTimer.restart()
                window.activity = qsTr("Refreshing inbox")
            }
            onDecisionRequested: function(messageId, decision) {
                pendingMessages = window.withoutId(pendingMessages, messageId)
                selectedMessageId = ""
                window.activity = qsTr("Message %1 %2").arg(messageId).arg(decision)
            }
            onApprovalRequested: function(proposalId, approved) {
                approvals = window.withoutId(approvals, proposalId)
                window.activity = approved
                        ? qsTr("Proposal %1 approved").arg(proposalId)
                        : qsTr("Proposal %1 denied").arg(proposalId)
            }
            onConfigurationRequested: function(changes) {
                window.activity = qsTr("Settings revision prepared: rate limit %1")
                        .arg(changes.rate_limit)
            }
        }

        ToolBar {
            Layout.fillWidth: true
            Label {
                anchors.fill: parent
                anchors.leftMargin: 12
                anchors.rightMargin: 12
                text: window.activity
                verticalAlignment: Text.AlignVCenter
                elide: Text.ElideRight
                Accessible.name: qsTr("Latest activity: %1").arg(text)
            }
        }
    }

    Timer {
        id: refreshTimer
        interval: 450
        onTriggered: {
            inbox.loading = false
            window.activity = qsTr("Inbox refreshed")
        }
    }
}
