# GeoMap AI: camera optimisation of an aligned job (Обработка → Оптимизация камер), run by the
# Python of the bundled OpenDroneMap with its OpenSfM. Bundle adjustment of the reconstruction as
# OpenSfM ends its own alignment: aligned to the GPS of the photos and the control points, bundled,
# outliers removed, bundled again.
#
#   python optimize_cameras.py <opensfm folder> <use control points 0|1> <refine calibration 0|1> [<opensfm package>]
#
# Writes reconstruction.json in the folder; prints one line "GEOMAP_RESULT {json}".
import json
import sys


def main():
    if len(sys.argv) > 4:
        sys.path.insert(0, sys.argv[4])
    from opensfm import dataset
    from opensfm import reconstruction as orec

    path, use_gcp, refine = sys.argv[1], sys.argv[2] == "1", sys.argv[3] == "1"
    data = dataset.DataSet(path)
    data.config["bundle_use_gcp"] = use_gcp
    data.config["optimize_camera_parameters"] = refine
    reconstructions = data.load_reconstruction()
    tracks = data.load_tracks_manager()
    cameras = data.load_camera_models()
    rigs = data.load_rig_cameras()
    gcp = data.load_ground_control_points() if use_gcp else []
    shots = points = 0
    for r in reconstructions:
        # The tracks bring back observations the alignment had dropped: a robust first bundle,
        # the outliers out by their reprojection errors, a clean second bundle.
        r.add_correspondences_from_tracks_manager(tracks)
        orec.align_reconstruction(r, gcp, data.config)
        orec.bundle(r, cameras, rigs, gcp, data.config)
        orec.remove_outliers(r, data.config)
        orec.bundle(r, cameras, rigs, gcp, data.config)
        orec.remove_outliers(r, data.config)
        shots += len(r.shots)
        points += len(r.points)
    data.save_reconstruction(reconstructions)
    print("GEOMAP_RESULT " + json.dumps({"shots": shots, "points": points, "gcp": len(gcp)}), flush=True)


if __name__ == "__main__":
    main()
