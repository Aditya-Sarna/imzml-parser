// ---------------------------------------------------------------------------
// python/imzml_pyoms_ext.cpp
// nanobind extension: loads imzML via our OMLoader C++ bridge and returns
// raw spectrum data as plain Python structures (numpy arrays + dicts).
//
// Why this split?
//   pyopenms is built with pybind11.  nanobind and pybind11 use different
//   ABI conventions for wrapping C++ types, so a nanobind module CANNOT
//   directly return/accept pyopenms MSSpectrum objects.
//
//   Solution: this module returns raw numpy arrays and Python dicts.
//   The companion imzml_pyoms.py wrapper receives these and constructs
//   real pyopenms.MSSpectrum / pyopenms.MSExperiment Python objects.
//
// Exported:
//   imzml_pyoms_ext.load_raw(path, mz_lo, mz_hi, sort_mz)
//     -> { "metadata": {...}, "spectra": [{"x","y","z","mz","intensity"}, ...] }
//
//   imzml_pyoms_ext.load_metadata_raw(path)
//     -> {"imaging_mode", "uuid", "max_x", "max_y", ...}
//
//   imzml_pyoms_ext.validate(path)
//     -> (ok: bool, errors: list[str])
// ---------------------------------------------------------------------------

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/pair.h>
#include <nanobind/ndarray.h>

#include <OpenMS/FORMAT/ImzMLFile.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/KERNEL/MSExperiment.h>

#include <string>
#include <vector>
#include <cstring>

namespace nb = nanobind;
using namespace nb::literals;

// ---------------------------------------------------------------------------
// Helper: convert ImzMLMetadata into a Python dict
// ---------------------------------------------------------------------------
static nb::dict metadataToDict(const OpenMS::ImzMLMetadata& m)
{
    nb::dict d;
    const char* mode_str =
        m.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Continuous ? "Continuous" :
        m.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Processed  ? "Processed"  : "Unknown";
    d["imaging_mode"]        = mode_str;
    d["uuid"]                = m.uuid;
    d["ibd_file_path"]       = m.ibdFilePath;
    d["ibd_checksum"]        = m.ibdChecksum;
    d["ibd_checksum_type"]   = m.ibdChecksumType;
    d["max_x"]               = (int)m.maxX;
    d["max_y"]               = (int)m.maxY;
    d["max_z"]               = (int)m.maxZ;
    d["pixel_size_x"]        = (int)m.pixelSizeX;
    d["pixel_size_y"]        = (int)m.pixelSizeY;
    d["max_dim_x"]           = m.maxDimX;
    d["max_dim_y"]           = m.maxDimY;
    d["polarity"]            = m.polarity;
    d["mz_data_type"]        = m.mzDataType;
    d["int_data_type"]       = m.intDataType;
    d["scan_pattern"]        = m.scanPattern;
    d["scan_direction"]      = m.scanDirection;
    d["line_scan_direction"] = m.lineScanDirection;
    return d;
}

// ---------------------------------------------------------------------------
// Helper: convert one MSSpectrum into a Python dict with numpy arrays
// mz  → float64 ndarray
// intensity → float32 ndarray
// ---------------------------------------------------------------------------
static nb::dict spectrumToDict(const OpenMS::MSSpectrum& s)
{
    const std::size_t n = s.size();

    // Allocate owned arrays that numpy will free via the capsule.
    double* mz_ptr  = new double[n];
    float*  int_ptr = new float[n];

    for (std::size_t i = 0; i < n; ++i) {
        mz_ptr[i]  = s[i].getMZ();
        int_ptr[i] = s[i].getIntensity();
    }

    auto mz_arr = nb::ndarray<nb::numpy, double, nb::ndim<1>>(
        mz_ptr, {n},
        nb::capsule(mz_ptr, [](void* p) noexcept { delete[] static_cast<double*>(p); })
    );
    auto int_arr = nb::ndarray<nb::numpy, float, nb::ndim<1>>(
        int_ptr, {n},
        nb::capsule(int_ptr, [](void* p) noexcept { delete[] static_cast<float*>(p); })
    );

    nb::dict d;
    d["x"]         = (int)s.getCoordX();
    d["y"]         = (int)s.getCoordY();
    d["z"]         = (int)s.getCoordZ();
    d["mz"]        = mz_arr;
    d["intensity"] = int_arr;
    return d;
}

// ---------------------------------------------------------------------------
NB_MODULE(imzml_pyoms_ext, m)
{
    m.doc() =
        "nanobind bridge for imzML loading — returns raw numpy arrays "
        "for consumption by imzml_pyoms.py which builds pyopenms objects.";

    // -----------------------------------------------------------------------
    // load_raw: full load → list of spectrum dicts + metadata dict
    // -----------------------------------------------------------------------
    m.def("load_raw",
          [](const std::string& path,
             double mz_lo, double mz_hi,
             bool   sort_mz) -> nb::dict
          {
              OpenMS::ImzMLFile loader;
              loader.setLogType(OpenMS::ProgressLogger::NONE);

              OpenMS::PeakFileOptions opts;
              if (mz_lo < mz_hi) opts.setMZRange(mz_lo, mz_hi);
              if (sort_mz)        opts.setSortMZ(true);

              OpenMS::MSExperiment exp;
              loader.load(path, exp, opts);

              // Build spectra list
              nb::list spectra;
              const std::size_t N = exp.size();
              for (std::size_t i = 0; i < N; ++i)
                  spectra.append(spectrumToDict(exp[i]));

              nb::dict result;
              result["metadata"] = metadataToDict(exp.getImzMLMetadata());
              result["spectra"]  = spectra;
              return result;
          },
          "path"_a,
          "mz_lo"_a  = 0.0,
          "mz_hi"_a  = 0.0,
          "sort_mz"_a = false,
          R"doc(
Load an imzML file and return raw numpy spectrum data.

Returns a dict:
  {
    "metadata": { "imaging_mode", "max_x", "max_y", ... },
    "spectra":  [ {"x", "y", "z", "mz": np.float64, "intensity": np.float32}, ... ]
  }

Use imzml_pyoms.load() to get real pyopenms MSSpectrum/MSExperiment objects.
)doc"
    );

    // -----------------------------------------------------------------------
    // load_metadata_raw: header only, no IBD decode
    // -----------------------------------------------------------------------
    m.def("load_metadata_raw",
          [](const std::string& path) -> nb::dict
          {
              OpenMS::ImzMLFile loader;
              loader.setLogType(OpenMS::ProgressLogger::NONE);
              OpenMS::MSExperiment exp;
              loader.loadMetadata(path, exp);
              return metadataToDict(exp.getImzMLMetadata());
          },
          "path"_a,
          "Parse imzML header only (no IBD reads). Returns metadata dict.");

    // -----------------------------------------------------------------------
    // validate: XML + IBD bounds check
    // -----------------------------------------------------------------------
    m.def("validate",
          [](const std::string& path)
              -> std::pair<bool, std::vector<std::string>>
          {
              OpenMS::ImzMLFile loader;
              loader.setLogType(OpenMS::ProgressLogger::NONE);
              std::vector<std::string> errors;
              bool ok = loader.validate(path, errors);
              return {ok, errors};
          },
          "path"_a,
          "Validate an imzML file. Returns (ok: bool, errors: list[str]).");
}
