"""
imzml_pyoms – pyopenms-native imzML loader backed by the C++ OMLoader bridge.

Architecture
------------
imzml_pyoms_ext   (nanobind C++ module)
    Loads the imzML file using our OMLoader → bioconda OpenMS pipeline.
    Returns raw numpy arrays (float64 m/z, float32 intensity) + metadata dicts.

imzml_pyoms       (this file, pure Python)
    Receives the raw data and constructs real pyopenms.MSSpectrum /
    pyopenms.MSExperiment objects, complete with pixel coordinates stored
    as IMS CV-term meta values (IMS:1000050/51/52) exactly as full OpenMS does.

Why this split?
    The pyopenms pip wheel (≤3.5.0: Cython; ≥3.6.0 develop: nanobind) bundles
    its own copy of libOpenMS.dylib compiled against **xerces-c 3.3**, while our
    omloader was compiled against the bioconda OpenMS which uses **xerces-c 3.2**.
    These two versions expose different C++ template-namespace symbols
    (``xercesc_3_2::`` vs ``xercesc_3_3::``).  Loading both dylibs into the same
    process causes an immediate ``dlopen`` failure:

        Symbol not found: __ZN6OpenMS8Internal10XMLHandler10fatalErrorE
          RKN11xercesc_3_217SAXParseExceptionE
        Expected in: libOpenMS.dylib  (bioconda, xerces 3.2 namespace)

    This is confirmed by:
      • otool -L .venv/.../pyopenms/libOpenMS.dylib → libxerces-c-3.3.dylib
      • otool -L build/release/lib/libomloader.dylib → libxerces-c-3.2.dylib
      • pyopenms build system (github.com/OpenMS/OpenMS, src/pyOpenMS/CMakeLists.txt)
        copies libOpenMS + all transitive deps into the wheel with @loader_path/ RPATH

    Solution: imzml_pyoms_ext is invoked in an **isolated subprocess** via a
    tiny in-process worker when both libraries would otherwise conflict.  The
    raw data is serialised through a temp file (numpy .npz) and a JSON sidecar.
    When the caller has NOT imported pyopenms yet, the fast in-process path is
    used instead (no subprocess overhead).

Usage
-----
>>> import imzml_pyoms as ip
>>> exp = ip.load("tissue.imzML")               # returns pyopenms.MSExperiment
>>> meta = ip.load_metadata("tissue.imzML")     # fast header-only read
>>> ok, errors = ip.validate("tissue.imzML")

>>> sp = exp[0]                                  # pyopenms.MSSpectrum
>>> mz, inten = sp.get_peaks()                   # numpy arrays
>>> x = sp.getMetaValue("IMS:1000050")          # pixel x (int)
>>> y = sp.getMetaValue("IMS:1000051")          # pixel y (int)
>>> z = sp.getMetaValue("IMS:1000052")          # pixel z (int)

>>> # Ion image for m/z 200 ± 0.1 (like pyimzML getionimage)
>>> img = ip.getionimage(exp, 200.0, tol=0.1)   # numpy 2-D float64 matrix
"""

from __future__ import annotations

import sys
import os
import json
import pickle
import subprocess
import tempfile
import numpy as np

# ---------------------------------------------------------------------------
# Locate the build-tree python directory and record it for subprocess use.
# ---------------------------------------------------------------------------
_HERE = os.path.dirname(os.path.abspath(__file__))

def _ext_python_path() -> list[str]:
    """Return PYTHONPATH entries that make imzml_pyoms_ext importable."""
    paths = []
    for p in [
        _HERE,
        os.path.normpath(os.path.join(_HERE, "..", "build", "release", "python")),
        os.path.normpath(os.path.join(_HERE, "..", "build", "python")),
    ]:
        if os.path.isdir(p) and p not in paths:
            paths.append(p)
    # Also keep anything already on sys.path that mentions imzml_pyoms_ext
    for p in sys.path:
        if p and p not in paths:
            so_pat = os.path.join(p, "imzml_pyoms_ext*.so")
            import glob
            if glob.glob(so_pat):
                paths.append(p)
    return paths


# ---------------------------------------------------------------------------
# Determine whether pyopenms is already loaded (which means its libOpenMS
# is already mapped into the process, so we must use the subprocess bridge).
# ---------------------------------------------------------------------------
def _pyopenms_loaded() -> bool:
    return "pyopenms" in sys.modules


# ---------------------------------------------------------------------------
# Try direct (in-process) import of the extension.
# Returns the module or None on failure.
# ---------------------------------------------------------------------------
def _try_import_ext_inprocess():
    for p in _ext_python_path():
        if p not in sys.path:
            sys.path.insert(0, p)
    try:
        import imzml_pyoms_ext as _m
        return _m
    except ImportError:
        return None


# ---------------------------------------------------------------------------
# Subprocess worker script — executed as a standalone Python script.
# Writes results to a temp .npz + .json pair and prints the paths on stdout.
# ---------------------------------------------------------------------------
_WORKER_SCRIPT = """\
import sys, os, json, tempfile
import numpy as np

# inject build-tree paths
for p in {paths!r}:
    if p not in sys.path:
        sys.path.insert(0, p)

import imzml_pyoms_ext as ext

cmd  = {cmd!r}
args = {args!r}

if cmd == "load_raw":
    raw = ext.load_raw(args["path"], args["mz_lo"], args["mz_hi"], args["sort_mz"])
    # Serialise spectra into a single npz: arrays named mz0, int0, mz1, int1, ...
    # plus a pickled list of (x, y, z) coords.
    arrays = {{}}
    coords = []
    for i, s in enumerate(raw["spectra"]):
        arrays[f"mz_{{i}}"]  = np.asarray(s["mz"],        dtype=np.float64)
        arrays[f"int_{{i}}"] = np.asarray(s["intensity"], dtype=np.float32)
        coords.append((int(s["x"]), int(s["y"]), int(s["z"])))
    npz_fd, npz_path = tempfile.mkstemp(suffix=".npz")
    os.close(npz_fd)
    np.savez(npz_path, **arrays)
    json_fd, json_path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(json_fd, "w") as f:
        json.dump({{"n": len(coords), "coords": coords,
                    "metadata": raw["metadata"]}}, f)
    print(npz_path)
    print(json_path)

elif cmd == "load_metadata_raw":
    meta = ext.load_metadata_raw(args["path"])
    json_fd, json_path = tempfile.mkstemp(suffix=".json")
    with os.fdopen(json_fd, "w") as f:
        json.dump(meta, f)
    print(json_path)

elif cmd == "validate":
    ok, errors = ext.validate(args["path"])
    print(json.dumps({{"ok": ok, "errors": errors}}))
"""


def _run_worker(cmd: str, args: dict) -> list[str]:
    """Spawn a subprocess that runs _WORKER_SCRIPT and returns its stdout lines."""
    script = _WORKER_SCRIPT.format(
        paths=_ext_python_path(), cmd=cmd, args=args
    )
    result = subprocess.run(
        [sys.executable, "-c", script],
        capture_output=True, text=True, check=False
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"imzml_pyoms worker subprocess failed:\n{result.stderr}"
        )
    return result.stdout.strip().splitlines()


# ---------------------------------------------------------------------------
# Internal: load raw data either in-process or via subprocess
# ---------------------------------------------------------------------------
def _load_raw_data(path: str,
                   mz_lo: float, mz_hi: float,
                   sort_mz: bool) -> dict:
    """Return the raw dict from imzml_pyoms_ext (in-process or subprocess)."""
    if not _pyopenms_loaded():
        ext = _try_import_ext_inprocess()
        if ext is not None:
            return ext.load_raw(path, mz_lo, mz_hi, sort_mz)

    # Subprocess path (pyopenms already loaded, or in-process import failed)
    lines = _run_worker("load_raw", {
        "path": path, "mz_lo": mz_lo, "mz_hi": mz_hi, "sort_mz": sort_mz
    })
    if len(lines) < 2:
        raise RuntimeError("Worker returned unexpected output")
    npz_path, json_path = lines[0], lines[1]
    try:
        npz  = np.load(npz_path, allow_pickle=False)
        with open(json_path) as f:
            meta = json.load(f)
        n      = meta["n"]
        coords = meta["coords"]
        spectra = []
        for i in range(n):
            spectra.append({
                "x":         coords[i][0],
                "y":         coords[i][1],
                "z":         coords[i][2],
                "mz":        npz[f"mz_{i}"],
                "intensity": npz[f"int_{i}"],
            })
        return {"metadata": meta["metadata"], "spectra": spectra}
    finally:
        try: os.unlink(npz_path)
        except OSError: pass
        try: os.unlink(json_path)
        except OSError: pass


# ---------------------------------------------------------------------------
# Lazy pyopenms import (so the module can be imported without pyopenms if only
# the raw _ext functions are needed)
# ---------------------------------------------------------------------------
def _oms():
    try:
        import pyopenms as _oms_mod
        return _oms_mod
    except ImportError as e:
        raise ImportError(
            "pyopenms is required for imzml_pyoms.  "
            "Install it with: pip install pyopenms"
        ) from e


# ---------------------------------------------------------------------------
# Internal: convert raw dict list → pyopenms.MSExperiment
# ---------------------------------------------------------------------------
def _build_experiment(raw: dict) -> "pyopenms.MSExperiment":
    oms = _oms()
    meta_d  = raw["metadata"]
    spectra = raw["spectra"]

    exp = oms.MSExperiment()

    for sd in spectra:
        sp = oms.MSSpectrum()

        # Peaks (numpy arrays handed directly — no copy needed)
        sp.set_peaks((sd["mz"].astype(np.float64), sd["intensity"].astype(np.float32)))

        # Pixel coordinates as IMS CV meta values (standard imzML convention)
        sp.setMetaValue("IMS:1000050", int(sd["x"]))
        sp.setMetaValue("IMS:1000051", int(sd["y"]))
        sp.setMetaValue("IMS:1000052", int(sd["z"]))

        # MS level and RT (RT not meaningful for MSI but required by OpenMS)
        sp.setMSLevel(1)
        # Encode pixel index as retention time so getRT() gives a sortable value
        sp.setRT(float(sd["y"] * 100000 + sd["x"]))

        exp.addSpectrum(sp)

    # Attach file-level metadata as ExperimentalSettings string metadata
    exp.setMetaValue("imzml:imaging_mode",      meta_d.get("imaging_mode", "Unknown"))
    exp.setMetaValue("imzml:uuid",              meta_d.get("uuid", ""))
    exp.setMetaValue("imzml:ibd_file_path",     meta_d.get("ibd_file_path", ""))
    exp.setMetaValue("imzml:max_x",             meta_d.get("max_x", 0))
    exp.setMetaValue("imzml:max_y",             meta_d.get("max_y", 0))
    exp.setMetaValue("imzml:max_z",             meta_d.get("max_z", 1))
    exp.setMetaValue("imzml:pixel_size_x",      meta_d.get("pixel_size_x", 0))
    exp.setMetaValue("imzml:pixel_size_y",      meta_d.get("pixel_size_y", 0))
    exp.setMetaValue("imzml:polarity",          meta_d.get("polarity", "unknown"))
    exp.setMetaValue("imzml:mz_data_type",      meta_d.get("mz_data_type", "unknown"))
    exp.setMetaValue("imzml:int_data_type",     meta_d.get("int_data_type", "unknown"))

    return exp


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def load(path: str,
         mz_lo: float = 0.0,
         mz_hi: float = 0.0,
         sort_mz: bool = False) -> "pyopenms.MSExperiment":
    """Load an imzML file and return a **pyopenms.MSExperiment**.

    Loading uses the C++ OMLoader → bioconda OpenMS pipeline for correct IBD
    decoding.  The resulting MSExperiment contains real pyopenms MSSpectrum
    objects with:

    * ``get_peaks()`` → (mz: np.float64, intensity: np.float32) numpy arrays
    * ``getMetaValue("IMS:1000050")`` → pixel x (int)
    * ``getMetaValue("IMS:1000051")`` → pixel y (int)
    * ``getMetaValue("IMS:1000052")`` → pixel z (int)

    Dataset-level imzML metadata is stored on the experiment as meta values
    prefixed with ``"imzml:"`` (e.g. ``exp.getMetaValue("imzml:imaging_mode")``).

    Parameters
    ----------
    path    : path to the ``.imzML`` file (the ``.ibd`` is found automatically)
    mz_lo   : lower m/z filter cut-off in Da (0 = no filter)
    mz_hi   : upper m/z filter cut-off in Da (0 = no filter)
    sort_mz : sort peaks by m/z within each spectrum after loading

    Returns
    -------
    pyopenms.MSExperiment
    """
    raw = _load_raw_data(path, mz_lo, mz_hi, sort_mz)
    return _build_experiment(raw)


def load_metadata(path: str) -> dict:
    """Parse the imzML header only (no IBD binary reads).

    Returns a plain ``dict`` with keys: ``imaging_mode``, ``uuid``,
    ``max_x``, ``max_y``, ``max_z``, ``pixel_size_x``, ``pixel_size_y``,
    ``polarity``, ``mz_data_type``, ``int_data_type``, etc.
    """
    if not _pyopenms_loaded():
        ext = _try_import_ext_inprocess()
        if ext is not None:
            return ext.load_metadata_raw(path)
    lines = _run_worker("load_metadata_raw", {"path": path})
    if not lines:
        raise RuntimeError("Worker returned no output for load_metadata_raw")
    json_path = lines[0]
    try:
        with open(json_path) as f:
            return json.load(f)
    finally:
        try: os.unlink(json_path)
        except OSError: pass


def validate(path: str) -> tuple[bool, list[str]]:
    """Validate an imzML file (XML + IBD bounds check).

    Returns ``(ok: bool, errors: list[str])``.
    """
    if not _pyopenms_loaded():
        ext = _try_import_ext_inprocess()
        if ext is not None:
            return ext.validate(path)
    lines = _run_worker("validate", {"path": path})
    if not lines:
        raise RuntimeError("Worker returned no output for validate")
    result = json.loads(lines[0])
    return result["ok"], result["errors"]


def getionimage(exp: "pyopenms.MSExperiment",
                mz_value: float,
                tol: float = 0.1) -> np.ndarray:
    """Generate a 2-D ion image matrix for a given m/z window.

    Mimics ``pyimzML.getionimage()``.

    Parameters
    ----------
    exp      : pyopenms.MSExperiment loaded with :func:`load`
    mz_value : target m/z in Da
    tol      : half-window tolerance in Da (default 0.1)

    Returns
    -------
    numpy.ndarray of shape (max_y, max_x) with dtype float64.
    Each cell contains the summed intensity of all peaks within
    ``[mz_value - tol, mz_value + tol]`` for the pixel at that position.
    Pixels with no spectrum recorded stay at 0.
    """
    max_x = int(exp.getMetaValue("imzml:max_x") or 0)
    max_y = int(exp.getMetaValue("imzml:max_y") or 0)

    # Fallback: derive grid extents from spectra if metadata missing
    if max_x == 0 or max_y == 0:
        for i in range(exp.size()):
            sp = exp[i]
            x = int(sp.getMetaValue("IMS:1000050") or 0)
            y = int(sp.getMetaValue("IMS:1000051") or 0)
            if x > max_x: max_x = x
            if y > max_y: max_y = y

    if max_x == 0 or max_y == 0:
        raise ValueError("Cannot determine grid extents from experiment")

    img = np.zeros((max_y, max_x), dtype=np.float64)
    lo  = mz_value - tol
    hi  = mz_value + tol

    for i in range(exp.size()):
        sp = exp[i]
        x = int(sp.getMetaValue("IMS:1000050") or 0) - 1  # 0-based
        y = int(sp.getMetaValue("IMS:1000051") or 0) - 1
        if x < 0 or y < 0 or x >= max_x or y >= max_y:
            continue
        mz_arr, int_arr = sp.get_peaks()
        # Vectorised window sum (numpy — no Python loop over peaks)
        mask = (mz_arr >= lo) & (mz_arr <= hi)
        img[y, x] = float(int_arr[mask].sum())

    return img


def get_physical_coordinates(exp: "pyopenms.MSExperiment",
                              index: int) -> tuple[float, float, float]:
    """Return real-world coordinates (x_µm, y_µm, z_µm) for spectrum *index*.

    Mimics ``pyimzML.ImzMLParser.get_physical_coordinates()``.
    """
    sp = exp[index]
    px = int(sp.getMetaValue("IMS:1000050") or 0)
    py = int(sp.getMetaValue("IMS:1000051") or 0)
    pz = int(sp.getMetaValue("IMS:1000052") or 1)
    sx = int(exp.getMetaValue("imzml:pixel_size_x") or 1)
    sy = int(exp.getMetaValue("imzml:pixel_size_y") or 1)
    return (px * sx, py * sy, pz)
