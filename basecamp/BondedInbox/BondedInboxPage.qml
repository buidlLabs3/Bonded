pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

Page {
    id: root
    objectName: "bondedInboxPage"
    title: qsTr("Bonded Inbox")

    property string connectionState: "offline"
    property string selectedMessageId: ""
    property string rejectionSink: ""
    property bool loading: false
    property var pendingMessages: []
    property var activeTasks: []
    property var approvals: []
    property string lastError: ""
    readonly property bool bridgeAvailable: typeof logos !== "undefined"

    signal refreshRequested()
    signal decisionRequested(string messageId, string decision)
    signal configurationRequested(var changes)
    signal approvalRequested(string proposalId, bool approved)

    function decodedCall(method, arguments) {
        var value = JSON.parse(logos.callModule("bonded_inbox", method, arguments))
        if (typeof value === "string")
            value = JSON.parse(value)
        if (!value.ok)
            throw new Error(value.error && value.error.message
                            ? value.error.message : qsTr("Module request failed"))
        return value.result
    }

    function refreshBackend() {
        if (!bridgeAvailable) {
            refreshRequested()
            return
        }
        loading = true
        try {
            var state = decodedCall("getOwnerState", [])
            pendingMessages = state.messages || []
            activeTasks = state.tasks || []
            approvals = state.approvals || []
            connectionState = state.runtime && state.runtime.state === "ready"
                              ? "online" : "degraded"
            lastError = ""
        } catch (error) {
            connectionState = "offline"
            lastError = error.message
        } finally {
            loading = false
        }
    }

    function decideMessageBackend(messageId, decision) {
        if (!bridgeAvailable) {
            decisionRequested(messageId, decision)
            return
        }
        try {
            decodedCall("decideMessage", [JSON.stringify({
                "message_id": messageId,
                "decision": decision,
                "explicit_owner_action": true
            })])
            selectedMessageId = ""
            refreshBackend()
        } catch (error) {
            lastError = error.message
        }
    }

    function decideSpendingBackend(proposalId, approved) {
        if (!bridgeAvailable) {
            approvalRequested(proposalId, approved)
            return
        }
        try {
            decodedCall("decideSpending", [JSON.stringify({
                "proposal_id": proposalId,
                "approved": approved,
                "now_unix": Math.floor(Date.now() / 1000)
            })])
            refreshBackend()
        } catch (error) {
            lastError = error.message
        }
    }

    function taskSkill(task) {
        return task.metadata && task.metadata.logos ? task.metadata.logos.skill : ""
    }

    function taskState(task) {
        return task.status && task.status.state
                ? task.status.state.replace("TASK_STATE_", "").replace("_", " ") : "UNKNOWN"
    }

    Component.onCompleted: {
        if (bridgeAvailable)
            Qt.callLater(refreshBackend)
    }

    Timer {
        interval: 3000
        running: root.bridgeAvailable
        repeat: true
        onTriggered: root.refreshBackend()
    }

    header: ToolBar {
        RowLayout {
            anchors.fill: parent
            Label {
                text: root.title
                font.bold: true
                Layout.fillWidth: true
                Accessible.name: root.title
            }
            Label {
                text: root.connectionState
                color: root.connectionState === "online" ? "#137333" : "#8a1c1c"
                Accessible.name: qsTr("Connection status: %1").arg(text)
            }
            ToolButton {
                text: "\u21bb"
                onClicked: root.refreshBackend()
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Refresh")
                Accessible.name: qsTr("Refresh")
            }
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        TabBar {
            id: tabs
            Layout.fillWidth: true
            TabButton { text: qsTr("Inbox") }
            TabButton { text: qsTr("Tasks") }
            TabButton { text: qsTr("Approvals") }
            TabButton { text: qsTr("Settings") }
        }

        StackLayout {
            currentIndex: tabs.currentIndex
            Layout.fillWidth: true
            Layout.fillHeight: true

            Item {
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 12
                    TextField {
                        id: search
                        objectName: "messageSearch"
                        placeholderText: qsTr("Search message metadata")
                        Layout.fillWidth: true
                        Accessible.name: placeholderText
                    }
                    BusyIndicator {
                        running: root.loading
                        visible: running
                        Layout.alignment: Qt.AlignHCenter
                        Accessible.name: qsTr("Loading inbox")
                    }
                    Label {
                        visible: !root.loading && root.pendingMessages.length === 0
                        text: root.connectionState === "offline"
                              ? qsTr("Inbox unavailable while offline")
                              : qsTr("No messages awaiting review")
                        wrapMode: Text.Wrap
                        Layout.fillWidth: true
                        horizontalAlignment: Text.AlignHCenter
                    }
                    ListView {
                        objectName: "pendingMessageList"
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true
                        spacing: 1
                        model: root.pendingMessages
                        delegate: ItemDelegate {
                            id: messageDelegate
                            required property var modelData
                            width: ListView.view.width
                            text: messageDelegate.modelData.sender + "  " + messageDelegate.modelData.state
                            Accessible.name: qsTr("Message from %1, status %2")
                                                .arg(messageDelegate.modelData.sender)
                                                .arg(messageDelegate.modelData.state)
                            onClicked: root.selectedMessageId = messageDelegate.modelData.id
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Button {
                            text: qsTr("Accept")
                            enabled: root.selectedMessageId !== ""
                            onClicked: root.decideMessageBackend(root.selectedMessageId, "accepted")
                        }
                        Button {
                            text: qsTr("Reject")
                            enabled: root.selectedMessageId !== ""
                            onClicked: rejectionDialog.open()
                        }
                    }
                }
            }

            ListView {
                objectName: "taskList"
                model: root.activeTasks
                clip: true
                delegate: ItemDelegate {
                    id: taskDelegate
                    required property var modelData
                    width: ListView.view.width
                    text: root.taskSkill(taskDelegate.modelData) + "  "
                          + root.taskState(taskDelegate.modelData)
                    Accessible.name: qsTr("Task %1, status %2")
                                         .arg(root.taskSkill(taskDelegate.modelData))
                                         .arg(root.taskState(taskDelegate.modelData))
                }
            }

            ListView {
                objectName: "approvalList"
                model: root.approvals
                clip: true
                delegate: RowLayout {
                    id: approvalDelegate
                    required property var modelData
                    width: ListView.view.width
                    Label {
                        text: approvalDelegate.modelData.amount + " LEZ to "
                              + approvalDelegate.modelData.recipient
                        Layout.fillWidth: true
                    }
                    Button {
                        text: qsTr("Deny")
                        onClicked: root.decideSpendingBackend(approvalDelegate.modelData.id, false)
                    }
                    Button {
                        text: qsTr("Approve")
                        onClicked: root.decideSpendingBackend(approvalDelegate.modelData.id, true)
                    }
                }
            }

            ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    spacing: 12
                    Label { text: qsTr("Owner settings"); font.bold: true }
                    CheckBox { id: classifier; text: qsTr("Enable local classifier") }
                    SpinBox { id: rateLimit; from: 1; to: 1000; value: 10; editable: true }
                    CheckBox { id: notifications; text: qsTr("Owner action notifications"); checked: true }
                    Button {
                        text: qsTr("Publish signed changes")
                        onClicked: root.configurationRequested({
                            "classifier_enabled": classifier.checked,
                            "rate_limit": rateLimit.value,
                            "owner_notifications": notifications.checked
                        })
                    }
                }
            }
        }
    }

    Dialog {
        id: rejectionDialog
        objectName: "rejectionConfirmation"
        modal: true
        title: qsTr("Confirm bonded rejection")
        standardButtons: Dialog.Cancel | Dialog.Ok
        anchors.centerIn: parent
        width: Math.min(parent.width - 24, 480)
        onAccepted: root.decideMessageBackend(root.selectedMessageId, "rejected")
        contentItem: Label {
            text: qsTr("The bond will be sent to %1. It cannot be paid to the owner.")
                  .arg(root.rejectionSink)
            wrapMode: Text.Wrap
            Accessible.name: text
        }
    }
}
