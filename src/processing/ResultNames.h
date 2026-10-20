#pragma once

#include <QString>

// The name a layer of a known result gets on the map and in the 3D scene, instead of its file name:
// "Ортофотоплан ODM · 16.10 14:01" for <project>/jobs/20261016-140120/odm_orthophoto/odm_orthophoto.tif,
// "DSM по LiDAR · scan" for <project>/lidar/scan_dsm.tif. Empty for any other file.
namespace resultnames {
QString titleOf(const QString &path);
}
