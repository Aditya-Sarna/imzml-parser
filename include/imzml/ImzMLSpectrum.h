// ---------------------------------------------------------------------------
// ImzMLSpectrum.h
// Data structure representing a single spectrum record from an imzML file.
//
// Each ImzMLSpectrum stores:
//   - pixel coordinates (x, y, z)
//   - m/z binary layout (offset, length, data type)
//   - intensity binary layout (offset, length, data type)
//
// The raw binary data is NOT stored here; it lives in the .ibd file and is
// loaded on demand by IBDReader::readSpectrum().
// ---------------------------------------------------------------------------
#pragma once

#include "imzml/ImzMLTypes.h"

#include <cstdint>
#include <vector>

namespace imzml
{

// ---------------------------------------------------------------------------
// ImzMLSpectrum
// Lightweight metadata record - mirrors the per-spectrum offset table that
// pyimzML stores in mzOffsets, mzLengths, intensityOffsets, intensityLengths.
// ---------------------------------------------------------------------------
struct ImzMLSpectrum
{
    // Pixel position (1-based per imzML spec)
    Coordinate  coord{};

    // m/z binary array info
    uint64_t     mzOffset{0};
    uint64_t     mzLength{0};          // number of elements
    BinaryDataType mzDataType{BinaryDataType::Unknown};

    // Intensity binary array info
    uint64_t     intensityOffset{0};
    uint64_t     intensityLength{0};   // number of elements
    BinaryDataType intensityDataType{BinaryDataType::Unknown};

    // Index in the original file (0-based)
    int32_t      index{0};

    // Validation helpers
    bool hasMzArray()        const { return mzDataType != BinaryDataType::Unknown && mzLength > 0; }
    bool hasIntensityArray() const { return intensityDataType != BinaryDataType::Unknown && intensityLength > 0; }
    bool isValid()           const { return hasMzArray() && hasIntensityArray(); }
};

// ---------------------------------------------------------------------------
// SpectrumData
// Decoded spectrum data returned by IBDReader::readSpectrum().
// Values are always promoted to double for caller convenience.
// ---------------------------------------------------------------------------
struct SpectrumData
{
    std::vector<double> mz;
    std::vector<double> intensity;

    std::size_t size() const { return mz.size(); }
    bool empty()       const { return mz.empty(); }
};

} // namespace imzml
