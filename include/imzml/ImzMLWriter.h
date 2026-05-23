// ---------------------------------------------------------------------------
// ImzMLWriter.h
// Writes imaging mass spectrometry data in imzML 1.1.0 format.
//
// Produces two files:
//   <stem>.imzML  – XML metadata (spectrum coordinates + binary offsets)
//   <stem>.ibd    – binary array data (m/z and/or intensity values)
//
// Supports:
//   - Continuous mode: one shared m/z array + per-pixel intensity arrays
//   - Processed mode:  per-spectrum m/z + intensity arrays
//   - Float32 or Float64 output for both array types
//
// Usage (continuous):
//   imzml::ImzMLWriter w;
//   imzml::ImzMLWriter::Options opts;
//   opts.mode = imzml::ImagingMode::Continuous;
//   opts.pixelSizeX = 100.0;
//   opts.pixelSizeY = 100.0;
//   w.open("output.imzML", opts);
//
//   std::vector<double> mz   = { 100.0, 200.0, 300.0 };
//   std::vector<double> ints = { 0.5, 1.2, 0.3 };
//   w.addSpectrum({1, 1, 1}, mz, ints);
//   w.addSpectrum({2, 1, 1}, mz, ints2);
//   w.close();   // finalises .imzML XML
//
// Note: close() is automatically called by the destructor if not called
//       explicitly, but errors will be silently ignored in that case.
// ---------------------------------------------------------------------------
#pragma once

#include "imzml/ImzMLTypes.h"

#include <string>
#include <vector>
#include <fstream>
#include <cstdint>
#include <optional>

namespace imzml
{

// ---------------------------------------------------------------------------
// ImzMLWriterOptions – configuration passed to ImzMLWriter::open()
// Defined outside the class to avoid C++ default-member-initializer
// restrictions on nested structs.
// ---------------------------------------------------------------------------
struct ImzMLWriterOptions
{
    ImagingMode     mode        = ImagingMode::Continuous;

    // Binary data types for output arrays
    BinaryDataType  mzType      = BinaryDataType::Float32;
    BinaryDataType  intType     = BinaryDataType::Float32;

    // Pixel size in micrometres (0 = omit from XML)
    double          pixelSizeX  = 0.0;
    double          pixelSizeY  = 0.0;

    // UUID written into <fileContent> (auto-generated if empty)
    std::string     uuid;

    // Optional instrument description (written as <instrumentConfiguration>)
    std::string     instrumentName;
};

// ---------------------------------------------------------------------------
// ImzMLWriter
// ---------------------------------------------------------------------------
class ImzMLWriter
{
public:
    using Options = ImzMLWriterOptions;

    // ------------------------------------------------------------------
    // Construct without opening.  Call open() before addSpectrum().
    // ------------------------------------------------------------------
    ImzMLWriter() = default;

    // ------------------------------------------------------------------
    // Construct and open in one step.
    // ------------------------------------------------------------------
    ImzMLWriter(const std::string& imzmlPath, const Options& opts = {});

    // ------------------------------------------------------------------
    // Destructor: calls close() silently if still open.
    // ------------------------------------------------------------------
    ~ImzMLWriter();

    // Non-copyable (owns file handles)
    ImzMLWriter(const ImzMLWriter&)            = delete;
    ImzMLWriter& operator=(const ImzMLWriter&) = delete;

    // ------------------------------------------------------------------
    // Open output files and initialise the writer.
    //   imzmlPath – path to write the .imzML file
    //               (the .ibd file is created with the same stem)
    // Throws std::runtime_error if files cannot be created.
    // ------------------------------------------------------------------
    void open(const std::string& imzmlPath, const Options& opts = {});

    // ------------------------------------------------------------------
    // Append one spectrum.
    //   coord      – pixel position (1-based, x/y/z)
    //   mz         – m/z values (ascending order expected)
    //   intensity  – intensity values (same length as mz)
    //
    // Continuous mode:
    //   The first call writes the shared m/z array to the .ibd file.
    //   Subsequent calls MUST provide an m/z array of the same length
    //   (values are ignored after the first call; only intensities vary).
    //   Throws std::invalid_argument if lengths differ.
    //
    // Processed mode:
    //   Each call writes its own m/z + intensity arrays.
    //
    // Throws std::runtime_error if the writer is not open.
    // ------------------------------------------------------------------
    void addSpectrum(const Coordinate&          coord,
                     const std::vector<double>& mz,
                     const std::vector<double>& intensity);

    // ------------------------------------------------------------------
    // Finalise and close both files.
    // Writes the complete .imzML XML (including all spectrum entries).
    // After close() the writer is reset and may be re-opened.
    // ------------------------------------------------------------------
    void close();

    // ------------------------------------------------------------------
    // Returns true if the writer is currently open.
    // ------------------------------------------------------------------
    bool isOpen() const { return ibdStream_.is_open(); }

    // ------------------------------------------------------------------
    // Number of spectra written so far.
    // ------------------------------------------------------------------
    std::size_t spectraWritten() const { return entries_.size(); }

private:
    // Per-spectrum record accumulated while writing ibd
    struct SpecEntry
    {
        Coordinate  coord{};
        uint64_t    mzOffset{0};
        uint64_t    mzLength{0};
        uint64_t    intOffset{0};
        uint64_t    intLength{0};
    };

    // Helpers
    void     writeArrayToIbd_(const std::vector<double>& data, BinaryDataType type,
                               uint64_t& offsetOut, uint64_t& lengthOut);
    void     writeXml_() const;
    std::string cvParamForType_(BinaryDataType t) const;
    std::string nameForType_(BinaryDataType t) const;
    std::string makeUuid_() const;

    Options                  opts_;
    std::string              imzmlPath_;
    std::string              ibdPath_;
    std::ofstream            ibdStream_;
    std::vector<SpecEntry>   entries_;

    // Continuous: shared m/z written once
    uint64_t  sharedMzOffset_{0};
    uint64_t  sharedMzLength_{0};
    bool      sharedMzWritten_{false};
};

} // namespace imzml
