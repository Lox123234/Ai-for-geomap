import QtQuick
import QtQuick.Dialogs

// A file dialog that opens where it makes sense (appInfo.dialogFolder: the last folder chosen in
// dialogs of the same kind, the project's folder, Pictures or Documents — never the program's
// folder) and remembers the folder of what was chosen. The instance's own onAccepted runs too.
FileDialog {
    property string kind: "files"
    currentFolder: appInfo.dialogFolder(projects.projectPath, kind)
    onAccepted: appInfo.rememberFolder(kind, fileMode === FileDialog.OpenFiles && selectedFiles.length > 0 ? selectedFiles[0] : selectedFile)
}
