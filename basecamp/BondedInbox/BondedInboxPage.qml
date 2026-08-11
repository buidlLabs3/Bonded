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

    signal refreshRequested()
    signal decisionRequested(string messageId, string decision)
    signal configurationRequested(var changes)
    signal approvalRequested(string proposalId, bool approved)

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
                onClicked: root.refreshRequested()
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
                            onClicked: root.decisionRequested(root.selectedMessageId, "accepted")
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
                    text: taskDelegate.modelData.skill + "  " + taskDelegate.modelData.state
                    Accessible.name: qsTr("Task %1, status %2")
                                         .arg(taskDelegate.modelData.skill)
                                         .arg(taskDelegate.modelData.state)
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
                        onClicked: root.approvalRequested(approvalDelegate.modelData.id, false)
                    }
                    Button {
                        text: qsTr("Approve")
                        onClicked: root.approvalRequested(approvalDelegate.modelData.id, true)
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
        onAccepted: root.decisionRequested(root.selectedMessageId, "rejected")
        contentItem: Label {
            text: qsTr("The bond will be sent to %1. It cannot be paid to the owner.")
                  .arg(root.rejectionSink)
            wrapMode: Text.Wrap
            Accessible.name: text
        }
    }
}
