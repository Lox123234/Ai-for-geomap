import QtQuick
import QtQuick.Dialogs

// A folder dialog that opens where it makes sense (see AppFileDialog) and remembers the folder
// chosen for dialogs of the same kind.
FolderDialog {
    property string kind: "folders"
    currentFolder: appInfo.dialogFolder(projects.projectPath, kind)
    onAccepted: appInfo.rememberFolder(kind, selectedFolder)
}
