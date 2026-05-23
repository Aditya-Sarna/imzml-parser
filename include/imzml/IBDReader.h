// ---------------------------------------------------------------------------
// IBDReader.h
// Reads binary data arrays from the .ibd file associated with an imzML file.
//
// Design mirrors the binary-decode methods in OpenMS MzMLHandler:
//   - Seeks to specified byte offset
//   - Reads raw bytes
//   - Interprets them as the correct numeric type
//   - Returns data as double (or optionally as typed vector)
//
// The IBDReader validates:
//   - That the .ibd file is open
//   - That the requested range is within the file bounds
//   - That array metadata (offset + length * sizeof(type)) does not overflow
// ---------------------------------------------------------------------------
#pragma once

#include "imzml/ImzMLTypes.h"
#include "imzml/ImzMLSpectrum.h"

#include <string>
#include <fstream>
#include <vector>
#include <cstdint>

namespace imzml
{

// ---------------------------------------------------------------------------
// IBDReader
// ---------------------------------------------------------------------------
class IBDReader
{
public:
    // ------------------------------------------------------------------
    // Construct without opening (call open() manually)
    // ------------------------------------------------------------------
    IBDReader() = default;

    // ------------------------------------------------------------------
    // Construct and open in one step
    // ------------------------------------------------------------------
    explicit IBDReader(const std::string& ibdPath);

    ~IBDReader();

    // Non-copyable (owns file handle)
    IBDReader(const IBDReader&)            = delete;
    IBDReader& operator=(const IBDReader&) = delete;

    // Movable
    IBDReader(IBDReader&&)            noexcept;
    IBDReader& operator=(IBDReader&&) noexcept;

    // ------------------------------------------------------------------
    // Open / close
    // ------------------------------------------------------------------
    void open(const std::string& ibdPath);
    void close();
    bool isOpen() const { return stream_.is_open(); }

    // ------------------------------------------------------------------
    // File size access (used for bounds validation)
    // ------------------------------------------------------------------
    uint64_t fileSize() const { return fileSize_; }

    // ------------------------------------------------------------------
    // Read a single spectrum (m/z + intensity arrays) from an ImzMLSpectrum.
    // Validates bounds before any I/O.
    // Throws std::runtime_error on failure.
    // ------------------------------------------------------------------
    SpectrumData readSpectrum(const ImzMLSpectrum& spec) const;

    // ------------------------------------------------------------------
    // Low-level: read one binary array and decode into double vector.
    //   offset   - byte offset in .ibd
    //   length   - number of elements
    //   dataType - numeric type
    // ------------------------------------------------------------------
    std::vector<double> readArray(uint64_t       offset,
                                  uint64_t       length,
                                  BinaryDataType dataType) const;

    // ------------------------------------------------------------------
    // Same as readArray but returns strongly-typed float32 vector
    // (avoids the double conversion when caller wants raw float)
    // ------------------------------------------------------------------
    std::vector<float> readArrayFloat32(uint64_t offset,
                                        uint64_t length) const;

    std::vector<float> readArrayFloat64AsFloat(uint64_t offset,
                                               uint64_t length) const;

    // ------------------------------------------------------------------
    // Validate that a range [offset, offset + bytes) lies within the file.
    // Returns false (and fills errMsg) if out-of-bounds.
    // ------------------------------------------------------------------
    bool validateRange(uint64_t    offset,
                       uint64_t    numElements,
                       BinaryDataType dataType,
                       std::string&   errMsg) const;

private:
    // Template decode: read N values of type T starting at offset
    template<typename T>
    std::vector<double> decodeAs_(uint64_t offset, uint64_t length) const;

    mutable std::ifstream stream_;
    uint64_t              fileSize_{0};
    std::string           path_;
};

} // namespace imzml
