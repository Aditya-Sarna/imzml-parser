// ---------------------------------------------------------------------------
// ImzMLFile.h
// User-facing API for loading imzML files.
//
// Architecture mirrors OpenMS/include/OpenMS/FORMAT/MzMLFile.h and XMLFile.h:
//   ImzMLFile::load()
//     creates ImzMLHandler handler(...)
//     calls handler.parse()
//   Then opens the .ibd file via IBDReader.
//
// Usage:
//   imzml::ImzMLFile f;
//   f.load("image.imzML");
//
//   for (std::size_t i = 0; i < f.size(); ++i)
//   {
//       auto data = f.getSpectrum(i);
//       auto coord = f.spectrum(i).coord;
//       // data.mz, data.intensity
//   }
// ---------------------------------------------------------------------------
#pragma once

#include "imzml/ImzMLTypes.h"
#include "imzml/ImzMLSpectrum.h"
#include "imzml/IBDReader.h"

#include <string>
#include <vector>
#include <stdexcept>

namespace imzml
{

// ---------------------------------------------------------------------------
// ImzMLFile
// ---------------------------------------------------------------------------
class ImzMLFile
{
public:
    ImzMLFile()  = default;
    ~ImzMLFile() = default;

    // Non-copyable (owns IBDReader file handle)
    ImzMLFile(const ImzMLFile&)            = delete;
    ImzMLFile& operator=(const ImzMLFile&) = delete;

    // Movable
    ImzMLFile(ImzMLFile&&)            noexcept = default;
    ImzMLFile& operator=(ImzMLFile&&) noexcept = default;

    // ------------------------------------------------------------------
    // Load an imzML file.
    //   imzmlPath  - path to .imzML file
    //   ibdPath    - path to .ibd file (if empty, inferred from imzmlPath)
    //
    // Throws Internal::ParseError    on XML parse failure
    //        Internal::FileNotFound  if a file cannot be opened
    //        std::runtime_error      on binary validation errors
    // ------------------------------------------------------------------
    void load(const std::string& imzmlPath,
              const std::string& ibdPath = "");

    // ------------------------------------------------------------------
    // Number of spectra
    // ------------------------------------------------------------------
    std::size_t size() const { return spectra_.size(); }
    bool        empty() const { return spectra_.empty(); }

    // ------------------------------------------------------------------
    // Access spectrum metadata record (pixel coords + binary offsets)
    // ------------------------------------------------------------------
    const ImzMLSpectrum& spectrum(std::size_t index) const;

    // ------------------------------------------------------------------
    // Read and decode one spectrum from .ibd.
    // Throws std::runtime_error on bounds violation or I/O error.
    // ------------------------------------------------------------------
    SpectrumData getSpectrum(std::size_t index) const;

    // ------------------------------------------------------------------
    // Access all spectrum records (for iteration, statistics, etc.)
    // ------------------------------------------------------------------
    const std::vector<ImzMLSpectrum>& spectra() const { return spectra_; }

    // ------------------------------------------------------------------
    // File-level metadata
    // ------------------------------------------------------------------
    const ImzMLMetadata& metadata() const { return metadata_; }

    // ------------------------------------------------------------------
    // Imaging mode (continuous / processed)
    // ------------------------------------------------------------------
    ImagingMode mode() const { return metadata_.mode; }

    // ------------------------------------------------------------------
    // Convenience: validate all spectrum binary ranges against .ibd size.
    // Returns a list of error messages (empty = all OK).
    // ------------------------------------------------------------------
    std::vector<std::string> validate() const;

    // ------------------------------------------------------------------
    // Summary to stdout / ostream for CLI / debugging output.
    // ------------------------------------------------------------------
    void printSummary(std::ostream& os) const;

private:
    // Infer .ibd path from .imzML path (same stem, .ibd extension)
    static std::string inferIbdPath_(const std::string& imzmlPath);

    ImzMLMetadata             metadata_;
    std::vector<ImzMLSpectrum> spectra_;
    IBDReader                  ibdReader_;
    std::string                ibdPath_;
};

} // namespace imzml
