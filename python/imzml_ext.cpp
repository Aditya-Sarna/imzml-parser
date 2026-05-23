// ---------------------------------------------------------------------------
// python/imzml_ext.cpp
// nanobind Python bindings for the imzML parser
//
// Exposes:
//   imzml_ext.ImagingMode               -- enum: Unknown / Continuous / Processed
//   imzml_ext.ImzMLMetadata             -- dataset-level metadata
//   imzml_ext.Peak1D                    -- single m/z + intensity peak
//   imzml_ext.MSSpectrum                -- iterable spectrum with pixel coords
//   imzml_ext.MSExperiment              -- iterable collection of spectra
//   imzml_ext.PeakFileOptions           -- loading options (mz range, coord filter…)
//   imzml_ext.ImzMLFile                 -- file loader / validator
//   imzml_ext.SpectrumIndexEntry        -- lightweight IBD index record (no peaks)
//   imzml_ext.OnDiscImzMLExperiment     -- lazy on-disc reader (one spectrum at a time)
//   imzml_ext.load(path, **kw)          -- convenience: load file, return MSExperiment
//   imzml_ext.open(path)                -- convenience: open file, return OnDiscImzMLExperiment
//
// Build: added automatically by CMakeLists.txt (target: imzml_ext)
// ---------------------------------------------------------------------------

#include <nanobind/nanobind.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/tuple.h>
#include <nanobind/make_iterator.h>
#include <nanobind/ndarray.h>

#include <OpenMS/FORMAT/ImzMLFile.h>
#include <OpenMS/FORMAT/OnDiscImzMLExperiment.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/KERNEL/MSExperiment.h>

#include "imzml/ImzMLWriter.h"

namespace nb = nanobind;
using namespace nb::literals;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

// Index-guard that raises IndexError on out-of-bounds access (Python style).
template<typename Container>
static auto checkedGet(Container& c, nb::ssize_t i) -> decltype(c[0])&
{
    nb::ssize_t n = static_cast<nb::ssize_t>(c.size());
    if (i < 0) i += n;
    if (i < 0 || i >= n)
        throw nb::index_error("index out of range");
    return c[static_cast<std::size_t>(i)];
}

// ---------------------------------------------------------------------------
// Mode converters: bridge OpenMS::ImzMLMetadata::ImagingMode ↔ imzml::ImagingMode
// so Python only ever sees the single ImagingMode enum already registered above.
// ---------------------------------------------------------------------------
static imzml::ImagingMode toNativeMode(OpenMS::ImzMLMetadata::ImagingMode m)
{
    switch (m)
    {
        case OpenMS::ImzMLMetadata::ImagingMode::Continuous: return imzml::ImagingMode::Continuous;
        case OpenMS::ImzMLMetadata::ImagingMode::Processed:  return imzml::ImagingMode::Processed;
        default:                                              return imzml::ImagingMode::Unknown;
    }
}

static OpenMS::ImzMLMetadata::ImagingMode fromNativeMode(imzml::ImagingMode m)
{
    switch (m)
    {
        case imzml::ImagingMode::Continuous: return OpenMS::ImzMLMetadata::ImagingMode::Continuous;
        case imzml::ImagingMode::Processed:  return OpenMS::ImzMLMetadata::ImagingMode::Processed;
        default:                             return OpenMS::ImzMLMetadata::ImagingMode::Unknown;
    }
}

// Copy a contiguous sequence into a heap-allocated 1-D numpy array.
// The capsule deleter frees the buffer when Python GC drops the array.
template <typename T, typename Iter>
static nb::ndarray<nb::numpy, T, nb::ndim<1>>
makeNumpyArray(Iter begin, std::size_t n)
{
    T* buf = new T[n ? n : 1];      // new T[0] is UB on some platforms
    std::copy_n(begin, n, buf);
    nb::capsule owner(buf, [](void* p) noexcept { delete[] static_cast<T*>(p); });
    return nb::ndarray<nb::numpy, T, nb::ndim<1>>(buf, {n}, owner);
}

// ---------------------------------------------------------------------------
NB_MODULE(imzml_ext, m)
{
    m.doc() = "nanobind bindings for the imzML parser (OpenMS integration layer)";

    // -----------------------------------------------------------------------
    // ImagingMode enum
    // -----------------------------------------------------------------------
    nb::enum_<OpenMS::ImzMLMetadata::ImagingMode>(m, "ImagingMode",
            "imzML acquisition mode (Continuous or Processed)")
        .value("Unknown",    OpenMS::ImzMLMetadata::ImagingMode::Unknown)
        .value("Continuous", OpenMS::ImzMLMetadata::ImagingMode::Continuous)
        .value("Processed",  OpenMS::ImzMLMetadata::ImagingMode::Processed)
        .export_values();

    // -----------------------------------------------------------------------
    // ImzMLMetadata
    // -----------------------------------------------------------------------
    nb::class_<OpenMS::ImzMLMetadata>(m, "ImzMLMetadata",
            "Dataset-level metadata parsed from an imzML file header.")
        .def(nb::init<>())
        .def_rw("imaging_mode",       &OpenMS::ImzMLMetadata::imagingMode,
                "Continuous or Processed acquisition mode.")
        .def_rw("ibd_file_path",      &OpenMS::ImzMLMetadata::ibdFilePath,
                "Path to the binary IBD file.")
        .def_rw("ibd_checksum",       &OpenMS::ImzMLMetadata::ibdChecksum,
                "SHA-1 or MD5 hex checksum of the IBD file.")
        .def_rw("ibd_checksum_type",  &OpenMS::ImzMLMetadata::ibdChecksumType,
                "'SHA-1' or 'MD5'.")
        .def_rw("uuid",               &OpenMS::ImzMLMetadata::uuid,
                "Universally unique identifier of the dataset.")
        .def_rw("schema_version",     &OpenMS::ImzMLMetadata::schemaVersion)
        .def_rw("max_x",              &OpenMS::ImzMLMetadata::maxX,
                "Grid width in pixels (fastest-changing axis).")
        .def_rw("max_y",              &OpenMS::ImzMLMetadata::maxY,
                "Grid height in pixels.")
        .def_rw("max_z",              &OpenMS::ImzMLMetadata::maxZ,
                "Grid depth in pixels (typically 1 for 2-D datasets).")
        .def_rw("pixel_size_x",       &OpenMS::ImzMLMetadata::pixelSizeX,
                "Pixel pitch in µm along the x-axis.")
        .def_rw("pixel_size_y",       &OpenMS::ImzMLMetadata::pixelSizeY,
                "Pixel pitch in µm along the y-axis.")
        .def_rw("max_dim_x",          &OpenMS::ImzMLMetadata::maxDimX,
                "Physical extent µm along x (IMS:1000044).")
        .def_rw("max_dim_y",          &OpenMS::ImzMLMetadata::maxDimY,
                "Physical extent µm along y (IMS:1000045).")
        .def_rw("scan_pattern",       &OpenMS::ImzMLMetadata::scanPattern)
        .def_rw("scan_direction",     &OpenMS::ImzMLMetadata::scanDirection)
        .def_rw("line_scan_direction",&OpenMS::ImzMLMetadata::lineScanDirection)
        .def_rw("polarity",           &OpenMS::ImzMLMetadata::polarity,
                "'positive', 'negative', or 'unknown'.")
        .def_rw("mz_data_type",       &OpenMS::ImzMLMetadata::mzDataType,
                "Binary type of m/z array: 'float32', 'float64', etc.")
        .def_rw("int_data_type",      &OpenMS::ImzMLMetadata::intDataType,
                "Binary type of intensity array.")
        .def("__repr__", [](const OpenMS::ImzMLMetadata& self) {
            char buf[256];
            const char* mode =
                self.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Continuous ? "Continuous" :
                self.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Processed  ? "Processed"  : "Unknown";
            std::snprintf(buf, sizeof(buf),
                "ImzMLMetadata(mode=%s, grid=%ux%u, pixel=%uumx%uum)",
                mode, self.maxX, self.maxY, self.pixelSizeX, self.pixelSizeY);
            return std::string(buf);
        });

    // -----------------------------------------------------------------------
    // Peak1D  (read-only view — spectrum owns the storage)
    // -----------------------------------------------------------------------
    nb::class_<OpenMS::Peak1D>(m, "Peak1D", "A single peak: m/z + intensity.")
        .def_prop_ro("mz",        &OpenMS::Peak1D::getMZ,
                     "m/z value (Da).")
        .def_prop_ro("intensity", &OpenMS::Peak1D::getIntensity,
                     "Intensity (arbitrary units).")
        .def("__repr__", [](const OpenMS::Peak1D& p) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Peak1D(mz=%.6f, intensity=%.4f)",
                          p.getMZ(), (double)p.getIntensity());
            return std::string(buf);
        });

    // -----------------------------------------------------------------------
    // MSSpectrum
    // -----------------------------------------------------------------------
    nb::class_<OpenMS::MSSpectrum>(m, "MSSpectrum",
            "Spectrum: ordered list of Peak1D plus pixel coordinates.")
        .def(nb::init<>())
        // Container protocol
        .def("__len__",    &OpenMS::MSSpectrum::size)
        .def("__getitem__",
             [](OpenMS::MSSpectrum& s, nb::ssize_t i) -> OpenMS::Peak1D& {
                 return checkedGet(s, i);
             }, nb::rv_policy::reference_internal)
        .def("__iter__",
             [](OpenMS::MSSpectrum& s) {
                 return nb::make_iterator(nb::type<OpenMS::MSSpectrum>(), "peak_iterator",
                                          s.begin(), s.end());
             }, nb::keep_alive<0, 1>())
        // imzML pixel coordinates
        .def_prop_ro("coord_x", &OpenMS::MSSpectrum::getCoordX,
                     "Pixel column (1-indexed).")
        .def_prop_ro("coord_y", &OpenMS::MSSpectrum::getCoordY,
                     "Pixel row (1-indexed).")
        .def_prop_ro("coord_z", &OpenMS::MSSpectrum::getCoordZ,
                     "Depth index (typically 1).")
        // Metadata
        .def_prop_ro("ms_level",  &OpenMS::MSSpectrum::getMSLevel)
        .def_prop_ro("name",      &OpenMS::MSSpectrum::getName)
        // pyOpenMS-compatible: get_peaks() -> (mz_array, intensity_array)
        .def("get_peaks", [](const OpenMS::MSSpectrum& s) {
            const std::size_t n = s.size();
            double* mz  = new double[n ? n : 1];
            float*  ints = new float[n ? n : 1];
            for (std::size_t i = 0; i < n; ++i) {
                mz[i]   = s[i].getMZ();
                ints[i] = s[i].getIntensity();
            }
            nb::capsule mz_own(mz,   [](void* p) noexcept { delete[] static_cast<double*>(p); });
            nb::capsule in_own(ints, [](void* p) noexcept { delete[] static_cast<float*>(p); });
            auto mz_arr = nb::ndarray<nb::numpy, double, nb::ndim<1>>(mz,   {n}, mz_own);
            auto in_arr = nb::ndarray<nb::numpy, float,  nb::ndim<1>>(ints, {n}, in_own);
            return nb::make_tuple(mz_arr, in_arr);
         }, "Return (mz_array, intensity_array) as numpy float64/float32 arrays.")
        // pyOpenMS-compatible: set_peaks((mz_array, intensity_array))
        .def("set_peaks", [](OpenMS::MSSpectrum& s,
                              const std::vector<double>& mz,
                              const std::vector<float>& intensity) {
            const std::size_t n = std::min(mz.size(), intensity.size());
            s.clear();
            s.resize(n);
            for (std::size_t i = 0; i < n; ++i) {
                s[i].setMZ(mz[i]);
                s[i].setIntensity(intensity[i]);
            }
         }, "mz"_a, "intensity"_a,
         "Set peaks from mz_array (float64) and intensity_array (float32). "
         "Both accept numpy arrays or Python lists.")
        // Numpy-friendly raw arrays (also exposed as pyOpenMS-style names)
        .def("get_mz_array",
             [](const OpenMS::MSSpectrum& s) {
                 const std::size_t n = s.size();
                 double* buf = new double[n ? n : 1];
                 for (std::size_t i = 0; i < n; ++i) buf[i] = s[i].getMZ();
                 nb::capsule own(buf, [](void* p) noexcept { delete[] static_cast<double*>(p); });
                 return nb::ndarray<nb::numpy, double, nb::ndim<1>>(buf, {n}, own);
             }, "m/z array as numpy float64.")
        .def("get_intensity_array",
             [](const OpenMS::MSSpectrum& s) {
                 const std::size_t n = s.size();
                 float* buf = new float[n ? n : 1];
                 for (std::size_t i = 0; i < n; ++i) buf[i] = s[i].getIntensity();
                 nb::capsule own(buf, [](void* p) noexcept { delete[] static_cast<float*>(p); });
                 return nb::ndarray<nb::numpy, float, nb::ndim<1>>(buf, {n}, own);
             }, "Intensity array as numpy float32.")
        // Backwards-compatible aliases (return numpy arrays, not lists)
        .def("mz_array",        [](const OpenMS::MSSpectrum& s) {
                 const std::size_t n = s.size();
                 double* buf = new double[n ? n : 1];
                 for (std::size_t i = 0; i < n; ++i) buf[i] = s[i].getMZ();
                 nb::capsule own(buf, [](void* p) noexcept { delete[] static_cast<double*>(p); });
                 return nb::ndarray<nb::numpy, double, nb::ndim<1>>(buf, {n}, own);
             }, "Alias for get_mz_array().")
        .def("intensity_array", [](const OpenMS::MSSpectrum& s) {
                 const std::size_t n = s.size();
                 float* buf = new float[n ? n : 1];
                 for (std::size_t i = 0; i < n; ++i) buf[i] = s[i].getIntensity();
                 nb::capsule own(buf, [](void* p) noexcept { delete[] static_cast<float*>(p); });
                 return nb::ndarray<nb::numpy, float, nb::ndim<1>>(buf, {n}, own);
             }, "Alias for get_intensity_array().")
        .def_prop_ro("tic", [](const OpenMS::MSSpectrum& s) {
            double t = 0.0;
            for (std::size_t i = 0; i < s.size(); ++i)
                t += s[i].getIntensity();
            return t;
         }, "Total ion current for this spectrum.")
        .def_prop_ro("base_peak", [](const OpenMS::MSSpectrum& s) -> double {
            if (s.empty()) return 0.0;
            std::size_t best = 0;
            for (std::size_t i = 1; i < s.size(); ++i)
                if (s[i].getIntensity() > s[best].getIntensity()) best = i;
            return s[best].getMZ();
         }, "m/z of the most intense peak (base peak m/z).")
        // pyOpenMS-compatible extra methods
        .def_prop_ro("size",
            [](const OpenMS::MSSpectrum& s) { return s.size(); },
            "Number of peaks (same as len(spec)).")
        .def_prop_rw("rt",
            &OpenMS::MSSpectrum::getRT,
            &OpenMS::MSSpectrum::setRT,
            "Retention time (s). Typically 0 for imzML spectra.")
        .def_prop_ro("native_id",
            &OpenMS::MSSpectrum::getName,
            "Native spectrum identifier string.")
        .def_prop_ro("is_sorted",
            [](const OpenMS::MSSpectrum& s) {
                for (std::size_t i = 1; i < s.size(); ++i)
                    if (s[i].getMZ() < s[i-1].getMZ()) return false;
                return true;
            }, "True if m/z values are non-decreasing.")
        .def("sort_by_position",
            &OpenMS::MSSpectrum::sortByPosition,
            "Sort peaks by ascending m/z (in-place).")
        .def("sortByPosition",          // camelCase alias (pyOpenMS style)
            &OpenMS::MSSpectrum::sortByPosition)
        .def_prop_ro("contains_im_data",
            [](const OpenMS::MSSpectrum&) { return false; },
            "Always False for imzML spectra (no ion mobility arrays).")
        .def("getMSLevel",   &OpenMS::MSSpectrum::getMSLevel,
             "camelCase alias for ms_level.")
        .def("getRT",        &OpenMS::MSSpectrum::getRT,
             "camelCase alias for rt.")
        .def("setRT",        &OpenMS::MSSpectrum::setRT, "rt"_a)
        .def("getName",      &OpenMS::MSSpectrum::getName,
             "camelCase alias for native_id.")
        .def("setName",      &OpenMS::MSSpectrum::setName, "name"_a)
        .def("__repr__", [](const OpenMS::MSSpectrum& s) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "MSSpectrum(x=%u, y=%u, z=%u, peaks=%zu)",
                s.getCoordX(), s.getCoordY(), s.getCoordZ(), s.size());
            return std::string(buf);
        });

    // -----------------------------------------------------------------------
    // MSExperiment
    // -----------------------------------------------------------------------
    nb::class_<OpenMS::MSExperiment>(m, "MSExperiment",
            "In-memory collection of MSSpectrum objects from an imzML file.")
        // Container protocol
        .def("__len__",    &OpenMS::MSExperiment::size)
        .def("__getitem__",
             [](OpenMS::MSExperiment& e, nb::ssize_t i) -> OpenMS::MSSpectrum& {
                 return checkedGet(e, i);
             }, nb::rv_policy::reference_internal)
        .def("__iter__",
             [](OpenMS::MSExperiment& e) {
                 return nb::make_iterator(nb::type<OpenMS::MSExperiment>(),
                                          "spectrum_iterator",
                                          e.begin(), e.end());
             }, nb::keep_alive<0, 1>())
        // imzML metadata
        .def_prop_ro("metadata",
             nb::overload_cast<>(&OpenMS::MSExperiment::getImzMLMetadata),
             nb::rv_policy::reference_internal,
             "ImzMLMetadata for this dataset.")
        .def_prop_ro("grid_width",  &OpenMS::MSExperiment::gridWidth,
                     "Pixel columns (max x coordinate seen in any spectrum).")
        .def_prop_ro("grid_height", &OpenMS::MSExperiment::gridHeight,
                     "Pixel rows (max y coordinate seen in any spectrum).")
        // Lookup
        .def("find_at_coordinate",
             [](const OpenMS::MSExperiment& e, uint32_t x, uint32_t y, uint32_t z) {
                 const OpenMS::MSSpectrum* s = e.findAtCoordinate(x, y, z);
                 if (!s) throw nb::value_error("no spectrum at requested coordinate");
                 return s;
             }, "x"_a, "y"_a, "z"_a = 1u,
             nb::rv_policy::reference_internal,
             "Return the spectrum at pixel (x, y, z). Raises ValueError if absent.")
        .def("__repr__", [](const OpenMS::MSExperiment& e) {
            char buf[128];
            const auto& m = e.getImzMLMetadata();
            std::snprintf(buf, sizeof(buf),
                "MSExperiment(spectra=%zu, grid=%ux%u)",
                e.size(), m.maxX, m.maxY);
            return std::string(buf);
        });

    // -----------------------------------------------------------------------
    // PeakFileOptions
    // -----------------------------------------------------------------------
    nb::class_<OpenMS::PeakFileOptions>(m, "PeakFileOptions",
            "Options controlling how an imzML file is loaded.")
        .def(nb::init<>())
        .def("set_mz_range", &OpenMS::PeakFileOptions::setMZRange,
             "lo"_a, "hi"_a,
             "Discard peaks outside [lo, hi] Da.")
        .def("has_mz_range", &OpenMS::PeakFileOptions::hasMZRange)
        .def("get_mz_range", &OpenMS::PeakFileOptions::getMZRange)
        .def("set_intensity_range", &OpenMS::PeakFileOptions::setIntensityRange,
             "lo"_a, "hi"_a,
             "Discard peaks with intensity outside [lo, hi].")
        .def("set_coordinate_filter",
             [](OpenMS::PeakFileOptions& o,
                uint32_t x_min, uint32_t x_max,
                uint32_t y_min, uint32_t y_max,
                uint32_t z_min, uint32_t z_max) {
                 o.setCoordinateFilter(x_min, x_max, y_min, y_max, z_min, z_max);
             },
             "x_min"_a, "x_max"_a, "y_min"_a, "y_max"_a,
             "z_min"_a = 1u, "z_max"_a = 1u,
             "Load only pixels whose (x,y,z) lies within the given bounding box.")
        .def("has_coordinate_filter", &OpenMS::PeakFileOptions::hasCoordinateFilter);

    // -----------------------------------------------------------------------
    // ImzMLFile
    // -----------------------------------------------------------------------
    nb::class_<OpenMS::ImzMLFile>(m, "ImzMLFile",
            "imzML file reader/validator backed by the OpenMS integration layer.")
        .def(nb::init<>())
        .def("load",
             [](OpenMS::ImzMLFile& f, const std::string& path,
                OpenMS::MSExperiment& exp, OpenMS::PeakFileOptions& opts) {
                 f.setLogType(OpenMS::ProgressLogger::NONE);
                 f.load(path, exp, opts);
             },
             "path"_a, "exp"_a, "opts"_a,
             "Load all spectra into *exp* using loading *opts*.")
        .def("load_metadata",
             [](OpenMS::ImzMLFile& f, const std::string& path,
                OpenMS::MSExperiment& exp) {
                 f.setLogType(OpenMS::ProgressLogger::NONE);
                 f.loadMetadata(path, exp);
             },
             "path"_a, "exp"_a,
             "Parse file header into *exp* without reading any IBD peak data.")
        .def("validate",
             [](OpenMS::ImzMLFile& f, const std::string& path)
                 -> std::pair<bool, std::vector<std::string>> {
                 f.setLogType(OpenMS::ProgressLogger::NONE);
                 std::vector<std::string> errors;
                 bool ok = f.validate(path, errors);
                 return {ok, errors};
             },
             "path"_a,
             "Validate XML and IBD bounds. Returns (ok, [error_messages]).");

    // -----------------------------------------------------------------------
    // Convenience top-level functions
    // -----------------------------------------------------------------------
    m.def("load",
          [](const std::string& path,
             double mz_lo, double mz_hi,
             bool sort_mz) -> OpenMS::MSExperiment {
              OpenMS::ImzMLFile f;
              f.setLogType(OpenMS::ProgressLogger::NONE);
              OpenMS::PeakFileOptions opts;
              if (mz_lo < mz_hi) opts.setMZRange(mz_lo, mz_hi);
              if (sort_mz) opts.setSortMZ(true);
              OpenMS::MSExperiment exp;
              f.load(path, exp, opts);
              return exp;
          },
          "path"_a,
          "mz_lo"_a = 0.0,
          "mz_hi"_a = 0.0,
          "sort_mz"_a = false,
          R"doc(
Load an imzML file and return an MSExperiment.

Parameters
----------
path     : path to the .imzML file
mz_lo    : lower m/z filter bound (0 = disabled)
mz_hi    : upper m/z filter bound (0 = disabled)
sort_mz  : whether to sort peaks by m/z within each spectrum

Example
-------
>>> import imzml_ext as im
>>> exp = im.load("tissue.imzML")
>>> print(exp.metadata)
>>> for spec in exp:
...     print(spec.coord_x, spec.coord_y, spec.tic())
)doc"
    );

    m.def("load_metadata",
          [](const std::string& path) -> OpenMS::MSExperiment {
              OpenMS::ImzMLFile f;
              f.setLogType(OpenMS::ProgressLogger::NONE);
              OpenMS::MSExperiment exp;
              f.loadMetadata(path, exp);
              return exp;
          },
          "path"_a,
          "Parse the imzML header only (no IBD decode). Fast metadata read.");

    m.def("validate",
          [](const std::string& path)
              -> std::pair<bool, std::vector<std::string>> {
              OpenMS::ImzMLFile f;
              f.setLogType(OpenMS::ProgressLogger::NONE);
              std::vector<std::string> errors;
              bool ok = f.validate(path, errors);
              return {ok, errors};
          },
          "path"_a,
          "Validate an imzML file. Returns (ok, [error_messages]).");

    // -----------------------------------------------------------------------
    // SpectrumIndexEntry
    // Lightweight IBD index record — returned by OnDiscImzMLExperiment.
    // No peak data; useful for iterating the grid without any IBD I/O.
    // -----------------------------------------------------------------------
    nb::class_<OpenMS::SpectrumIndexEntry>(m, "SpectrumIndexEntry",
            "Lightweight per-spectrum IBD index record (no peak data).")
        .def(nb::init<>())
        .def_ro("index",      &OpenMS::SpectrumIndexEntry::index,
                "0-based spectrum index in the file.")
        .def_ro("x",          &OpenMS::SpectrumIndexEntry::x,
                "Pixel column (1-indexed).")
        .def_ro("y",          &OpenMS::SpectrumIndexEntry::y,
                "Pixel row (1-indexed).")
        .def_ro("z",          &OpenMS::SpectrumIndexEntry::z,
                "Depth index (typically 1).")
        .def_ro("mz_offset",  &OpenMS::SpectrumIndexEntry::mz_offset,
                "Byte offset of the m/z array in the .ibd file (IMS:1000102).")
        .def_ro("mz_length",  &OpenMS::SpectrumIndexEntry::mz_length,
                "Number of m/z elements (IMS:1000103).")
        .def_ro("mz_type",    &OpenMS::SpectrumIndexEntry::mz_type,
                "m/z data type: 'float32', 'float64', 'int32', or 'int64'.")
        .def_ro("int_offset", &OpenMS::SpectrumIndexEntry::int_offset,
                "Byte offset of the intensity array in the .ibd file.")
        .def_ro("int_length", &OpenMS::SpectrumIndexEntry::int_length,
                "Number of intensity elements.")
        .def_ro("int_type",   &OpenMS::SpectrumIndexEntry::int_type,
                "Intensity data type: 'float32', 'float64', 'int32', or 'int64'.")
        .def("__repr__", [](const OpenMS::SpectrumIndexEntry& e) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "SpectrumIndexEntry(x=%u, y=%u, mz_offset=%llu, mz_length=%llu)",
                e.x, e.y,
                (unsigned long long)e.mz_offset,
                (unsigned long long)e.mz_length);
            return std::string(buf);
        });

    // -----------------------------------------------------------------------
    // OnDiscImzMLExperiment
    // Lazy on-disc reader: parse XML index once, load spectra on demand.
    // Analogous to OpenMS::OnDiscMSExperiment backed by IndexedMzMLFile.
    // -----------------------------------------------------------------------
    nb::class_<OpenMS::OnDiscImzMLExperiment>(m, "OnDiscImzMLExperiment",
        R"doc(
Lazy on-disc imzML reader — analogous to OnDiscMSExperiment.

open() parses the imzML XML to build an in-memory spectrum index
(pixel coordinates + IBD byte offsets/lengths) without reading any
binary peak data.  The .ibd file is opened and kept alive for
subsequent on-demand spectrum reads.

getSpectrum(i) / exp[i] perform exactly ONE fseek + fread per array
— only the requested spectrum is decoded.

Example
-------
>>> exp = im.open("tissue.imzML")           # parse index only
>>> print(exp.get_nr_spectra())              # instant
>>> spec = exp[42]                           # decode spectrum 42 now
>>> entry = exp.get_spectrum_index(42)       # IBD metadata, no decode
>>> print(entry.mz_offset, entry.mz_length)
>>> spec2 = exp.get_spectrum_at_coordinate(3, 7)  # pixel (x=3, y=7)
)doc")
        .def(nb::init<>())
        // open
        .def("open",
             [](OpenMS::OnDiscImzMLExperiment& e,
                const std::string& path,
                const std::string& ibd_path) {
                 e.open(path, ibd_path);
             },
             "path"_a, "ibd_path"_a = "",
             "Parse imzML XML index and open .ibd for on-demand reads. "
             "ibd_path is optional; inferred from path if empty.")
        // pyOpenMS-compatible alias: openFile(filename)
        .def("openFile",
             [](OpenMS::OnDiscImzMLExperiment& e, const std::string& path) {
                 e.open(path);
             },
             "path"_a,
             "Alias for open(path). Matches the pyOpenMS OnDiscMSExperiment API.")
        // state
        .def_prop_ro("is_open",       &OpenMS::OnDiscImzMLExperiment::isOpen)
        .def("__len__",               &OpenMS::OnDiscImzMLExperiment::size)
        .def("get_nr_spectra",        &OpenMS::OnDiscImzMLExperiment::getNrSpectra,
             "Number of spectra in the file (instant, no IBD I/O).")
        .def("getNrSpectra",           &OpenMS::OnDiscImzMLExperiment::getNrSpectra,
             "camelCase alias for get_nr_spectra() (pyOpenMS style).")
        // metadata
        .def_prop_ro("metadata",
             &OpenMS::OnDiscImzMLExperiment::getImzMLMetadata,
             nb::rv_policy::reference_internal,
             "File-level ImzMLMetadata (instant, no IBD I/O).")
        .def_prop_ro("grid_width",  &OpenMS::OnDiscImzMLExperiment::gridWidth,
                     "Pixel columns.")
        .def_prop_ro("grid_height", &OpenMS::OnDiscImzMLExperiment::gridHeight,
                     "Pixel rows.")
        // pyOpenMS-compatible: getExperimentalSettings() returns the metadata
        .def("getExperimentalSettings",
             &OpenMS::OnDiscImzMLExperiment::getImzMLMetadata,
             nb::rv_policy::reference_internal,
             "Return file-level ImzMLMetadata. "
             "Matches pyOpenMS OnDiscMSExperiment.getExperimentalSettings().")
        // index (no IBD reads)
        .def("get_spectrum_index",
             [](const OpenMS::OnDiscImzMLExperiment& e, nb::ssize_t i)
                 -> const OpenMS::SpectrumIndexEntry& {
                 nb::ssize_t n = static_cast<nb::ssize_t>(e.size());
                 if (i < 0) i += n;
                 if (i < 0 || i >= n)
                     throw nb::index_error("index out of range");
                 return e.getSpectrumIndex(static_cast<std::size_t>(i));
             },
             nb::rv_policy::reference_internal,
             "Return the IBD index entry for spectrum i (no IBD read, O(1)).")
        // on-demand spectrum reads
        .def("get_spectrum",
             [](const OpenMS::OnDiscImzMLExperiment& e, nb::ssize_t i)
                 -> OpenMS::MSSpectrum {
                 nb::ssize_t n = static_cast<nb::ssize_t>(e.size());
                 if (i < 0) i += n;
                 if (i < 0 || i >= n)
                     throw nb::index_error("index out of range");
                 return e.getSpectrum(static_cast<std::size_t>(i));
             },
             "Decode and return spectrum i from the .ibd file (one fseek+fread).")
        // camelCase alias matching pyOpenMS OnDiscMSExperiment.getSpectrum()
        .def("getSpectrum",
             [](const OpenMS::OnDiscImzMLExperiment& e, nb::ssize_t i)
                 -> OpenMS::MSSpectrum {
                 nb::ssize_t n = static_cast<nb::ssize_t>(e.size());
                 if (i < 0) i += n;
                 if (i < 0 || i >= n)
                     throw nb::index_error("index out of range");
                 return e.getSpectrum(static_cast<std::size_t>(i));
             },
             "camelCase alias for get_spectrum() (pyOpenMS style).")
        .def("__getitem__",
             [](const OpenMS::OnDiscImzMLExperiment& e, nb::ssize_t i)
                 -> OpenMS::MSSpectrum {
                 nb::ssize_t n = static_cast<nb::ssize_t>(e.size());
                 if (i < 0) i += n;
                 if (i < 0 || i >= n)
                     throw nb::index_error("index out of range");
                 return e.getSpectrum(static_cast<std::size_t>(i));
             },
             "exp[i] — decode spectrum i on demand.")
        .def("get_spectrum_at_coordinate",
             [](const OpenMS::OnDiscImzMLExperiment& e,
                uint32_t x, uint32_t y, uint32_t z) -> OpenMS::MSSpectrum {
                 try {
                     return e.getSpectrumAtCoordinate(x, y, z);
                 } catch (const std::out_of_range& ex) {
                     throw nb::value_error(ex.what());
                 }
             },
             "x"_a, "y"_a, "z"_a = 1u,
             "Decode the spectrum at pixel (x, y[, z]). O(n) first call, O(1) after.")
        .def("__repr__", [](const OpenMS::OnDiscImzMLExperiment& e) {
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "OnDiscImzMLExperiment(spectra=%zu, grid=%ux%u, open=%s)",
                e.size(), e.gridWidth(), e.gridHeight(),
                e.isOpen() ? "true" : "false");
            return std::string(buf);
        });

    // -----------------------------------------------------------------------
    // Convenience: im.open(path) -> OnDiscImzMLExperiment
    // -----------------------------------------------------------------------
    m.def("open",
          [](const std::string& path) -> OpenMS::OnDiscImzMLExperiment {
              OpenMS::OnDiscImzMLExperiment exp;
              exp.open(path);
              return exp;
          },
          "path"_a,
          R"doc(
Parse the imzML XML index and return an OnDiscImzMLExperiment.
No IBD peak data is read.  Spectrum[i] is decoded lazily on first access.

Example
-------
>>> import imzml_ext as im
>>> exp = im.open("tissue.imzML")
>>> print(exp.get_nr_spectra(), exp.grid_width, exp.grid_height)
>>> spec = exp[0]                              # decode first spectrum now
>>> for i in range(len(exp)):
...     entry = exp.get_spectrum_index(i)      # no IBD read
...     if entry.x == 10 and entry.y == 10:
...         spec = exp.get_spectrum(i)         # decode only this one
...         break
)doc"
    );

    // -----------------------------------------------------------------------
    // Coordinate
    // 1-based pixel position used by ImzMLWriter.add_spectrum().
    // -----------------------------------------------------------------------
    nb::class_<imzml::Coordinate>(m, "Coordinate",
            "1-based pixel position (x, y, z) for an imzML spectrum.")
        .def(nb::init<>())
        .def(nb::init<int32_t, int32_t, int32_t>(), "x"_a, "y"_a, "z"_a = 1,
             "Construct a coordinate. z defaults to 1 for 2-D datasets.")
        .def_rw("x", &imzml::Coordinate::x, "Column index (1-indexed).")
        .def_rw("y", &imzml::Coordinate::y, "Row index (1-indexed).")
        .def_rw("z", &imzml::Coordinate::z, "Depth index (typically 1).")
        .def("__repr__", [](const imzml::Coordinate& c) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "Coordinate(x=%d, y=%d, z=%d)", c.x, c.y, c.z);
            return std::string(buf);
        });

    // -----------------------------------------------------------------------
    // BinaryDataType
    // Controls on-disc encoding for m/z and intensity arrays in the .ibd file.
    // -----------------------------------------------------------------------
    nb::enum_<imzml::BinaryDataType>(m, "BinaryDataType",
            "Binary encoding type for IBD array data.")
        .value("Unknown", imzml::BinaryDataType::Unknown)
        .value("Float32", imzml::BinaryDataType::Float32,
               "32-bit IEEE float (MS:1000521). Most compact for typical data.")
        .value("Float64", imzml::BinaryDataType::Float64,
               "64-bit IEEE float (MS:1000523). Full double precision.")
        .value("Int32",   imzml::BinaryDataType::Int32,
               "32-bit signed integer (MS:1000519).")
        .value("Int64",   imzml::BinaryDataType::Int64,
               "64-bit signed integer (MS:1000522).")
        .export_values();

    // -----------------------------------------------------------------------
    // ImzMLWriterOptions
    // Configuration struct passed to ImzMLWriter.open().
    // -----------------------------------------------------------------------
    nb::class_<imzml::ImzMLWriterOptions>(m, "ImzMLWriterOptions",
            "Configuration for ImzMLWriter: mode, array types, pixel size, UUID.")
        .def(nb::init<>())
        // mode: exposed using the shared ImagingMode enum already in this module
        .def_prop_rw("mode",
            [](const imzml::ImzMLWriterOptions& o) {
                return fromNativeMode(o.mode);
            },
            [](imzml::ImzMLWriterOptions& o, OpenMS::ImzMLMetadata::ImagingMode m) {
                o.mode = toNativeMode(m);
            },
            "Acquisition mode: ImagingMode.Continuous (shared m/z) or "
            "ImagingMode.Processed (per-spectrum m/z).")
        .def_rw("mz_type",  &imzml::ImzMLWriterOptions::mzType,
                "Binary encoding for m/z arrays (default: BinaryDataType.Float32).")
        .def_rw("int_type", &imzml::ImzMLWriterOptions::intType,
                "Binary encoding for intensity arrays (default: BinaryDataType.Float32).")
        .def_rw("pixel_size_x", &imzml::ImzMLWriterOptions::pixelSizeX,
                "Pixel pitch in µm along x-axis (0 = omit from XML).")
        .def_rw("pixel_size_y", &imzml::ImzMLWriterOptions::pixelSizeY,
                "Pixel pitch in µm along y-axis (0 = omit from XML).")
        .def_rw("uuid", &imzml::ImzMLWriterOptions::uuid,
                "Dataset UUID written into <fileContent>. Auto-generated if empty.")
        .def_rw("instrument_name", &imzml::ImzMLWriterOptions::instrumentName,
                "Optional instrument name written as <instrumentConfiguration>.")
        .def("__repr__", [](const imzml::ImzMLWriterOptions& o) {
            const char* mode_str =
                o.mode == imzml::ImagingMode::Continuous ? "Continuous" :
                o.mode == imzml::ImagingMode::Processed  ? "Processed"  : "Unknown";
            char buf[128];
            std::snprintf(buf, sizeof(buf),
                "ImzMLWriterOptions(mode=%s, mz_type=%s, pixel=%.1fx%.1f um)",
                mode_str,
                o.mzType == imzml::BinaryDataType::Float64 ? "Float64" : "Float32",
                o.pixelSizeX, o.pixelSizeY);
            return std::string(buf);
        });

    // -----------------------------------------------------------------------
    // ImzMLWriter
    // Writes imzML + IBD files in continuous or processed mode.
    // Supports the Python context-manager protocol (with statement).
    // -----------------------------------------------------------------------
    nb::class_<imzml::ImzMLWriter>(m, "ImzMLWriter",
        R"doc(
Write imzML + IBD files in continuous or processed mode.

The writer produces two files:
  <stem>.imzML  — XML metadata with spectrum coordinates and binary offsets
  <stem>.ibd    — binary array data (m/z and intensity values)

Supports the context-manager protocol::

    opts = im.ImzMLWriterOptions()
    opts.mode = im.ImagingMode.Continuous
    opts.pixel_size_x = 100.0
    opts.pixel_size_y = 100.0

    with im.ImzMLWriter() as w:
        w.open("output.imzML", opts)
        w.add_spectrum(im.Coordinate(1, 1), mz, intensity_1)
        w.add_spectrum(im.Coordinate(2, 1), mz, intensity_2)
    # close() called automatically on __exit__
)doc")
        .def(nb::init<>())
        .def("open",
             [](imzml::ImzMLWriter& w,
                const std::string& path,
                const imzml::ImzMLWriterOptions& opts) {
                 w.open(path, opts);
             },
             "path"_a, "opts"_a = imzml::ImzMLWriterOptions{},
             "Open output files. path should end with .imzML; the .ibd is inferred.")
        .def("add_spectrum",
             [](imzml::ImzMLWriter& w,
                const imzml::Coordinate& coord,
                const std::vector<double>& mz,
                const std::vector<double>& intensity) {
                 w.addSpectrum(coord, mz, intensity);
             },
             "coord"_a, "mz"_a, "intensity"_a,
             "Append one spectrum. In Continuous mode the first m/z array is "
             "shared; subsequent calls only need matching-length intensity arrays.")
        .def("close",
             &imzml::ImzMLWriter::close,
             "Finalise the .imzML XML and close both output files. "
             "Called automatically by __exit__.")
        .def_prop_ro("is_open",
             &imzml::ImzMLWriter::isOpen,
             "True while output files are open.")
        .def_prop_ro("spectra_written",
             &imzml::ImzMLWriter::spectraWritten,
             "Number of spectra appended so far.")
        .def("__repr__", [](const imzml::ImzMLWriter& w) {
            char buf[64];
            std::snprintf(buf, sizeof(buf),
                "ImzMLWriter(open=%s, spectra=%zu)",
                w.isOpen() ? "true" : "false", w.spectraWritten());
            return std::string(buf);
        })
        // Context-manager support: `with im.ImzMLWriter() as w: ...`
        .def("__enter__",
             [](imzml::ImzMLWriter& w) -> imzml::ImzMLWriter& { return w; },
             nb::rv_policy::reference)
        .def("__exit__",
             [](imzml::ImzMLWriter& w,
                nb::object /*exc_type*/,
                nb::object /*exc_val*/,
                nb::object /*exc_tb*/) {
                 if (w.isOpen()) w.close();
             },
             nb::arg("exc_type").none(),
             nb::arg("exc_val").none(),
             nb::arg("exc_tb").none());

    // -----------------------------------------------------------------------
    // Convenience: im.write(path, coords, mz_arrays, int_arrays, opts)
    // -----------------------------------------------------------------------
    m.def("write",
          [](const std::string& path,
             const std::vector<std::tuple<int32_t, int32_t, int32_t>>& coords,
             const std::vector<std::vector<double>>& mz_arrays,
             const std::vector<std::vector<double>>& int_arrays,
             imzml::ImzMLWriterOptions opts) {
              if (coords.size() != mz_arrays.size() ||
                  coords.size() != int_arrays.size())
                  throw nb::value_error(
                      "coords, mz_arrays, and int_arrays must have the same length");
              imzml::ImzMLWriter w;
              w.open(path, opts);
              for (std::size_t i = 0; i < coords.size(); ++i)
              {
                  imzml::Coordinate c;
                  c.x = std::get<0>(coords[i]);
                  c.y = std::get<1>(coords[i]);
                  c.z = std::get<2>(coords[i]);
                  w.addSpectrum(c, mz_arrays[i], int_arrays[i]);
              }
              w.close();
          },
          "path"_a,
          "coords"_a,
          "mz_arrays"_a,
          "int_arrays"_a,
          "opts"_a = imzml::ImzMLWriterOptions{},
          R"doc(
Write an imzML file in one call.

Parameters
----------
path       : output path ending with .imzML
coords     : list of (x, y, z) tuples — 1-based pixel positions
mz_arrays  : list of float64 lists/arrays — one per spectrum
int_arrays : list of float64 lists/arrays — one per spectrum
opts       : ImzMLWriterOptions (default: Continuous mode, Float32 encoding)

Example
-------
>>> import imzml_ext as im
>>> mz = [100.0, 200.0, 300.0]
>>> im.write(
...     "output.imzML",
...     [(1,1,1), (2,1,1), (1,2,1)],
...     [mz, mz, mz],
...     [[1.0, 2.0, 0.5], [0.3, 1.5, 2.2], [0.8, 0.9, 1.1]],
... )
)doc"
    );
}
