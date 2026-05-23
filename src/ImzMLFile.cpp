// ---------------------------------------------------------------------------
// ImzMLFile.cpp
// Public API for loading imzML files.
// Mirrors OpenMS/src/openms/source/FORMAT/MzMLFile.cpp and XMLFile.cpp.
//
// load() flow:
//   1. Create ImzMLHandler (SAX handler) bound to result containers
//   2. Call handler.parse()      -> populates spectra_ and metadata_
//   3. Infer (or accept) .ibd path
//   4. Open IBDReader
//   5. (Optional) run validate() to surface any issues before first read
// ---------------------------------------------------------------------------
#include "imzml/ImzMLFile.h"
#include "imzml/ImzMLHandler.h"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace imzml
{

// ---------------------------------------------------------------------------
// inferIbdPath_
// Replaces the .imzML extension with .ibd (case-insensitive stem matching).
// Tries the same directory as the .imzML file.
// ---------------------------------------------------------------------------
std::string ImzMLFile::inferIbdPath_(const std::string& imzmlPath)
{
    namespace fs = std::filesystem;

    fs::path p(imzmlPath);
    fs::path candidate = p.parent_path() / (p.stem().string() + ".ibd");

    // Direct match
    if (fs::exists(candidate))
        return candidate.string();

    // Case-insensitive search in same directory
    if (fs::is_directory(p.parent_path()))
    {
        std::string stem = p.stem().string();
        // lowercase stem for comparison
        std::string stemLower = stem;
        std::transform(stemLower.begin(), stemLower.end(), stemLower.begin(),
                       [](unsigned char c){ return std::tolower(c); });

        for (const auto& entry : fs::directory_iterator(p.parent_path()))
        {
            std::string fname = entry.path().filename().string();
            std::string fnameLower = fname;
            std::transform(fnameLower.begin(), fnameLower.end(), fnameLower.begin(),
                           [](unsigned char c){ return std::tolower(c); });

            // Check: same stem, .ibd extension
            fs::path ep(fname);
            std::string extLower = ep.extension().string();
            std::transform(extLower.begin(), extLower.end(), extLower.begin(),
                           [](unsigned char c){ return std::tolower(c); });
            std::string stemLow2 = ep.stem().string();
            std::transform(stemLow2.begin(), stemLow2.end(), stemLow2.begin(),
                           [](unsigned char c){ return std::tolower(c); });

            if (extLower == ".ibd" && stemLow2 == stemLower)
                return (p.parent_path() / fname).string();
        }
    }

    // Fall back: just return the same path with .ibd extension
    return candidate.string();
}

// ---------------------------------------------------------------------------
// load
// ---------------------------------------------------------------------------
void ImzMLFile::load(const std::string& imzmlPath,
                     const std::string& ibdPath)
{
    // Reset state in case this object is reused
    spectra_.clear();
    metadata_ = ImzMLMetadata{};

    // Step 1: SAX-parse the .imzML file
    Internal::ImzMLHandler handler(imzmlPath, metadata_, spectra_);
    handler.parse(); // throws ParseError or FileNotFound on failure

    // Step 2: Resolve .ibd path
    ibdPath_ = ibdPath.empty() ? inferIbdPath_(imzmlPath) : ibdPath;

    // Step 3: Open the binary file
    ibdReader_.open(ibdPath_); // throws std::runtime_error if missing

    // Step 4: Post-parse detection of ImagingMode if not yet set.
    // In some files the mode is encoded only in a referenceableParamGroup
    // that the handler already resolved.  If still unknown after parsing,
    // try to infer from the first spectrum's m/z lengths versus the rest:
    // all equal => continuous, varying => processed.
    if (metadata_.mode == ImagingMode::Unknown && spectra_.size() > 1)
    {
        bool allSameMzOffset = true;
        for (std::size_t i = 1; i < spectra_.size(); ++i)
        {
            if (spectra_[i].mzOffset != spectra_[0].mzOffset)
            {
                allSameMzOffset = false;
                break;
            }
        }
        metadata_.mode = allSameMzOffset ? ImagingMode::Continuous
                                         : ImagingMode::Processed;
    }
}

// ---------------------------------------------------------------------------
// spectrum - bounds-checked access
// ---------------------------------------------------------------------------
const ImzMLSpectrum& ImzMLFile::spectrum(std::size_t index) const
{
    if (index >= spectra_.size())
    {
        throw std::out_of_range("ImzMLFile::spectrum: index " +
                                std::to_string(index) + " out of range (" +
                                std::to_string(spectra_.size()) + " spectra)");
    }
    return spectra_[index];
}

// ---------------------------------------------------------------------------
// getSpectrum - read and decode one spectrum from .ibd
// ---------------------------------------------------------------------------
SpectrumData ImzMLFile::getSpectrum(std::size_t index) const
{
    return ibdReader_.readSpectrum(spectrum(index));
}

// ---------------------------------------------------------------------------
// validate - check all spectrum binary ranges
// ---------------------------------------------------------------------------
std::vector<std::string> ImzMLFile::validate() const
{
    std::vector<std::string> errors;

    if (!ibdReader_.isOpen())
        errors.push_back("IBD file is not open: " + ibdPath_);

    for (const auto& s : spectra_)
    {
        std::string err;

        if (!ibdReader_.validateRange(s.mzOffset, s.mzLength, s.mzDataType, err))
            errors.push_back("Spectrum " + std::to_string(s.index) + " mz: " + err);

        if (!ibdReader_.validateRange(s.intensityOffset, s.intensityLength,
                                      s.intensityDataType, err))
            errors.push_back("Spectrum " + std::to_string(s.index) + " intensity: " + err);
    }
    return errors;
}

// ---------------------------------------------------------------------------
// printSummary - human-readable summary to ostream
// ---------------------------------------------------------------------------
void ImzMLFile::printSummary(std::ostream& os) const
{
    const int W = 24;

    os << "\n--- imzML File Summary ---\n";
    os << std::left;
    os << std::setw(W) << "Spectra:"      << spectra_.size() << "\n";
    os << std::setw(W) << "Imaging mode:";
    switch (metadata_.mode)
    {
        case ImagingMode::Continuous: os << "continuous\n"; break;
        case ImagingMode::Processed:  os << "processed\n";  break;
        default:                      os << "unknown\n";    break;
    }

    const auto& ss = metadata_.scanSettings;
    if (ss.maxCountX > 0)
        os << std::setw(W) << "Grid (X x Y):" << ss.maxCountX << " x " << ss.maxCountY << "\n";
    if (ss.pixelSizeX > 0)
        os << std::setw(W) << "Pixel size (um):" << ss.pixelSizeX << " x " << ss.pixelSizeY << "\n";
    if (!metadata_.uuid.empty())
        os << std::setw(W) << "UUID:" << metadata_.uuid << "\n";
    if (!metadata_.ibdMd5.empty())
        os << std::setw(W) << "IBD MD5:" << metadata_.ibdMd5 << "\n";

    os << std::setw(W) << "IBD file size:" << ibdReader_.fileSize() << " bytes\n";

    // Show first 3 spectra coords
    os << "\nFirst spectra (index, x, y, z, mz_count, int_count):\n";
    std::size_t preview = std::min<std::size_t>(3, spectra_.size());
    for (std::size_t i = 0; i < preview; ++i)
    {
        const auto& s = spectra_[i];
        os << "  [" << s.index << "]  ("
           << s.coord.x << "," << s.coord.y << "," << s.coord.z << ")"
           << "  mz_len=" << s.mzLength
           << "  int_len=" << s.intensityLength
           << "\n";
    }
    if (spectra_.size() > 3)
        os << "  ... (" << spectra_.size() - 3 << " more)\n";

    // Validation summary
    auto errors = validate();
    if (errors.empty())
        os << "\nValidation: OK\n";
    else
    {
        os << "\nValidation errors (" << errors.size() << "):\n";
        for (const auto& e : errors)
            os << "  - " << e << "\n";
    }
    os << "\n";
}

} // namespace imzml
