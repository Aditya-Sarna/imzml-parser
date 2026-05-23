// ---------------------------------------------------------------------------
// OpenMS/FORMAT/OMLoader.h
//
// Bridge between the bioconda OpenMS TU (OMLoader.cpp) and the rest of the
// project.  Deliberately free of bioconda-specific types (Xerces 3.2, full
// OpenMS headers, Eigen, etc.) so it can be safely included by any TU.
//
// Two entry points:
//   loadWithRealOpenMS()  — legacy Phase-1 helper (RT/msLevel only)
//   loadImzMLFull()       — single-pass MzMLHandler-based imzML parser
//                           (recommended: standard mzML + IMS + IBD binary)
// ---------------------------------------------------------------------------
#pragma once

#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace OpenMS
{
namespace Internal
{

// ---------------------------------------------------------------------------
// Legacy Phase-1 types (kept for backward compatibility)
// ---------------------------------------------------------------------------

/// Per-spectrum information extracted by real OpenMS::MzMLFile.
struct OMSpectrumInfo
{
    double rt      = 0.0;   ///< Retention time (seconds)
    float  tic     = 0.0f;  ///< Total ion current
    int    msLevel = 1;     ///< MS level
};

/// Result returned from loadWithRealOpenMS().
struct OMLoadResult
{
    std::vector<OMSpectrumInfo> spectra;   ///< One entry per spectrum
    size_t                      count = 0; ///< == spectra.size()
    bool                        ok    = false;
    std::string                 error;     ///< Non-empty on failure
};

/// Load an imzML / mzML file through the real OpenMS::MzMLFile parser.
/// Returns per-spectrum RT & TIC.  Peak arrays are always empty for imzML.
OMLoadResult loadWithRealOpenMS(const std::string& path);

// ---------------------------------------------------------------------------
// Full single-pass imzML parser (MzMLHandler-based)
// ---------------------------------------------------------------------------

/// Per-spectrum data delivered by loadImzMLFull().
/// Contains standard mzML fields (rt, msLevel) AND imzML-specific fields
/// (pixel coordinates, decoded peak arrays from .ibd).
struct OMSpectrumFull
{
    double                rt       = 0.0;
    float                 tic      = 0.0f;
    int                   msLevel  = 1;
    int                   index    = 0;
    uint32_t              x        = 0;
    uint32_t              y        = 0;
    uint32_t              z        = 0;
    std::vector<double>   mz;
    std::vector<float>    intensity;
};

/// Callback invoked once per spectrum during loadImzMLFull().
using OMSpectrumCallback = std::function<void(OMSpectrumFull)>;

/// Dataset-level metadata returned by loadImzMLFull().
struct OMMeta
{
    bool        ok               = false;
    std::string error;

    // Grid
    uint32_t    maxX             = 0;
    uint32_t    maxY             = 0;
    uint32_t    maxZ             = 0;
    uint32_t    pixelSizeX       = 0;
    uint32_t    pixelSizeY       = 0;
    double      maxDimX          = 0.0;
    double      maxDimY          = 0.0;

    // Imaging mode / IBD
    std::string mode;              ///< "continuous" | "processed" | ""
    std::string ibdChecksum;
    std::string ibdChecksumType;   ///< "SHA-1" | "MD5"
    std::string ibdFilePath;

    // Ontology metadata
    std::string uuid;
    std::string polarity;          ///< "positive" | "negative"
    std::string mzDataType;        ///< "float32" | "float64" | ...
    std::string intDataType;
    std::string scanPattern;
    std::string scanDirection;
    std::string lineScanDirection;
    std::string schemaVersion;
};

/// Single-pass imzML loader built on OpenMS::Internal::MzMLHandler.
///
/// - MzMLHandler base handles all standard mzML metadata (RT, MS level,
///   instrument config, data processing, scan settings, …).
/// - ImzMLHandler override intercepts IMS:* CV terms (pixel coords, external
///   binary offset/length, imaging mode, checksum, …) and decodes peak
///   arrays directly from the .ibd file.
/// - Each parsed spectrum is streamed to `callback` immediately — no
///   in-memory accumulation of all spectra.
///
/// Compiled in OMLoader.cpp (bioconda include-path, Xerces 3.2).
OMMeta loadImzMLFull(const std::string&        imzmlPath,
                     const std::string&        ibdPath,
                     const OMSpectrumCallback& callback);

} // namespace Internal
} // namespace OpenMS
