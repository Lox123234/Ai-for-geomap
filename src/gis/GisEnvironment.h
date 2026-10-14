#pragma once

#include <QString>
#include <QStringList>

// One-time setup of GDAL/OGR and PROJ: drivers, data folders (proj.db, GDAL data), cache size
// and a quiet error handler (messages are read per call with lastError()).
namespace GisEnvironment {

// Safe to call more than once and from tests. `applicationDirectory` is searched for
// share/proj and share/gdal next to the executable (deployed builds).
bool initialize(const QString &applicationDirectory = {});
bool isInitialized();
QString projDataDirectory();   // folder with proj.db in use, empty if none was found
QString gdalVersion();         // "3.11.4"
QString projVersion();         // "9.8.1"
QStringList problems();        // e.g. "proj.db не найден": CRS operations will fail

// Last GDAL/OGR/PROJ error message of the calling thread (empty if none).
QString lastError();
void clearError();

} // namespace GisEnvironment
