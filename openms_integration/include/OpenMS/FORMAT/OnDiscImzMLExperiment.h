// ---------------------------------------------------------------------------
// OpenMS/FORMAT/OnDiscImzMLExperiment.h
//
// Lazy on-disc imzML reader -- analogous to OpenMS::OnDiscMSExperiment.
//
// Usage:
//   OnDiscImzMLExperiment exp;
//   exp.open("tissue.imzML");               // parse XML index only, O(n_spec)
//   exp.getNrSpectra();                     // instant, no IBD I/O
//   exp.getImzMLMetadata();                 // instant
//   MSSpectrum s = exp.getSpectrum(42);     // one fseek + fread
//   MSSpectrum s = exp[42];                 // operator[] sugar
//   MSSpectrum s = exp.getSpectrumAtCoordinate(3,7); // pixel lookup
//   const SpectrumIndexEntry& e = exp.getSpectrumIndex(42); // no IBD read
//
// Design: PIMPL hides imzml:: native types so openms_integration/include/
// has no dependency on the native include/ tree.
// ---------------------------------------------------------------------------
#pragma once

#include <OpenMS/KERNEL/MSSpectrum.h>
#include <OpenMS/KERNEL/MSExperiment.h>

#include <memory>
#include <string>
#include <cstdint>

namespace OpenMS
{

// ---------------------------------------------------------------------------
// SpectrumIndexEntry
// Lightweight IBD index record -- no peak data, no IBD read required.
// ---------------------------------------------------------------------------
struct SpectrumIndexEntry
{
    int32_t     index{0};       // 0-based position in the file

    uint32_t    x{0};           // pixel column (1-based)
    uint32_t    y{0};           // pixel row    (1-based)
    uint32_t    z{1};           // depth index  (1-based, usually 1)

    uint64_t    mz_offset{0};   // IMS:1000102 -- byte offset in .ibd
    uint64_t    mz_length{0};   // IMS:1000103 -- number of elements
    std::string mz_type;        // "float32" | "float64" | "int32" | "int64"

    uint64_t    int_offset{0};
    uint64_t    int_length{0};
    std::string int_type;
};

// ---------------------------------------------------------------------------
// OnDiscImzMLExperiment
// ---------------------------------------------------------------------------
class OnDiscImzMLExperiment
{
public:
    OnDiscImzMLExperiment();
    ~OnDiscImzMLExperiment();           // in .cpp (unique_ptr<Impl> needs Impl complete)

    OnDiscImzMLExperiment(const OnDiscImzMLExperiment&)            = delete;
    OnDiscImzMLExperiment& operator=(const OnDiscImzMLExperiment&) = delete;

    OnDiscImzMLExperiment(OnDiscImzMLExperiment&&)            noexcept;
    OnDiscImzMLExperiment& operator=(OnDiscImzMLExperiment&&) noexcept;

    // ------------------------------------------------------------------
    // open -- parse XML index, open .ibd; no peak data read yet.
    // ibdPath is optional (inferred from imzmlPath if empty).
    // ------------------------------------------------------------------
    void open(const std::string& imzmlPath,
              const std::string& ibdPath = "");

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    bool        isOpen()       const noexcept;
    std::size_t size()         const noexcept;
    std::size_t getNrSpectra() const noexcept { return size(); }

    // ------------------------------------------------------------------
    // Index access -- no IBD read, O(1)
    // ------------------------------------------------------------------
    const SpectrumIndexEntry& getSpectrumIndex(std::size_t i) const;

    // ------------------------------------------------------------------
    // On-demand decode -- one fseek + fread per call
    // ------------------------------------------------------------------
    MSSpectrum  getSpectrum(std::size_t i) const;
    MSSpectrum  operator[](std::size_t i)  const { return getSpectrum(i); }

    // Pixel coordinate lookup -- O(n) first call, O(1) after.
    MSSpectrum  getSpectrumAtCoordinate(uint32_t x, uint32_t y,
                                        uint32_t z = 1) const;

    // ------------------------------------------------------------------
    // File-level metadata -- instant, no IBD reads
    // ------------------------------------------------------------------
    const ImzMLMetadata& getImzMLMetadata() const noexcept;
    uint32_t             gridWidth()        const noexcept;
    uint32_t             gridHeight()       const noexcept;

private:
    struct Impl;                        // defined in OnDiscImzMLExperiment.cpp
    std::unique_ptr<Impl> pimpl_;
};

} // namespace OpenMS
