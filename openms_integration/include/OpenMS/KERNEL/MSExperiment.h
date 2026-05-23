// ---------------------------------------------------------------------------
// OpenMS/KERNEL/MSExperiment.h
// Minimal MSExperiment for imzML — mirrors OpenMS MSExperiment
//
// Additions over stock OpenMS:
//   - ImzMLMetadata struct captured during XML parse
//   - gridWidth() / gridHeight() helpers
// ---------------------------------------------------------------------------
#pragma once

#include <OpenMS/KERNEL/MSSpectrum.h>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>

namespace OpenMS
{

// ---------------------------------------------------------------------------
// ImzMLMetadata — dataset-level information not present in stock mzML
// ---------------------------------------------------------------------------
struct ImzMLMetadata
{
    // IDB layout
    enum class ImagingMode { Unknown, Continuous, Processed };

    ImagingMode   imagingMode   {ImagingMode::Unknown};
    std::string   ibdFilePath;
    std::string   ibdChecksum;        // SHA-1 or MD5 hex string from XML
    std::string   ibdChecksumType;    // "SHA-1" or "MD5"

    // Dataset identity
    std::string   uuid;               // IMS:1000080 universally unique identifier
    std::string   schemaVersion;      // mzML/imzML schema version

    uint32_t      maxX {0}, maxY {0}, maxZ {1};  // grid extents (pixels)
    uint32_t      pixelSizeX {1};                // pixel size µm
    uint32_t      pixelSizeY {1};
    double        maxDimX {0.0};     // physical extent µm (IMS:1000044)
    double        maxDimY {0.0};     // physical extent µm (IMS:1000045)

    std::string   scanPattern;       // e.g. "top down", "bottom up"
    std::string   scanDirection;     // e.g. "horizontal", "vertical"
    std::string   lineScanDirection; // e.g. "left right", "right left"

    // Polarity: "positive", "negative", "unknown"
    std::string   polarity {"unknown"};

    // Binary data types for m/z and intensity arrays
    std::string   mzDataType  {"unknown"};  // "float32","float64","int32","int64"
    std::string   intDataType {"unknown"};  // same
};

// ---------------------------------------------------------------------------
// MSExperiment
// ---------------------------------------------------------------------------
class MSExperiment
{
public:
    // Mirrors OpenMS MSExperiment container interface
    std::size_t        size()  const noexcept { return spectra_.size(); }
    bool               empty() const noexcept { return spectra_.empty(); }

    MSSpectrum&        operator[](std::size_t i)       { return spectra_[i]; }
    const MSSpectrum&  operator[](std::size_t i) const { return spectra_[i]; }

    void push_back(MSSpectrum&& s) { spectra_.push_back(std::move(s)); }
    void push_back(const MSSpectrum& s) { spectra_.push_back(s); }
    void reserve(std::size_t n)    { spectra_.reserve(n); }
    void clear()                   { spectra_.clear(); }

    std::vector<MSSpectrum>::iterator       begin()       { return spectra_.begin(); }
    std::vector<MSSpectrum>::iterator       end()         { return spectra_.end(); }
    std::vector<MSSpectrum>::const_iterator begin() const { return spectra_.begin(); }
    std::vector<MSSpectrum>::const_iterator end()   const { return spectra_.end(); }

    void sortSpectra(bool sort_mz = false)
    {
        if (sort_mz)
            for (auto& s : spectra_) s.sortByPosition();
        // (by pixel position is also common in imaging)
    }

    // ------------------------------------------------------------------
    // imzML-specific accessors
    // ------------------------------------------------------------------
    ImzMLMetadata&       getImzMLMetadata()       { return meta_; }
    const ImzMLMetadata& getImzMLMetadata() const { return meta_; }

    uint32_t gridWidth()  const { return meta_.maxX; }
    uint32_t gridHeight() const { return meta_.maxY; }

    // Find spectrum at pixel (x, y, z) — O(n) scan
    const MSSpectrum* findAtCoordinate(uint32_t x, uint32_t y, uint32_t z = 1) const
    {
        for (const auto& s : spectra_)
            if (s.getCoordX() == x && s.getCoordY() == y && s.getCoordZ() == z)
                return &s;
        return nullptr;
    }

private:
    std::vector<MSSpectrum> spectra_;
    ImzMLMetadata           meta_;
};

} // namespace OpenMS
