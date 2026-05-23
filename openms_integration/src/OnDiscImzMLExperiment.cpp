// ---------------------------------------------------------------------------
// OnDiscImzMLExperiment.cpp  -- PIMPL implementation
//
// PIMPL note:  all imzml:: native types live only in this translation unit.
//              onDiscImzMLExperiment.h has no dependency on include/.
//              CMakeLists.txt adds ${CMAKE_CURRENT_SOURCE_DIR}/include as a
//              PRIVATE include to openms_imzml so the #includes below resolve.
// ---------------------------------------------------------------------------
#include <OpenMS/FORMAT/OnDiscImzMLExperiment.h>

// -- native parser (PRIVATE to this TU; on the PRIVATE include path) --------
#include "imzml/ImzMLFile.h"
#include "imzml/ImzMLSpectrum.h"
#include "imzml/ImzMLTypes.h"

// -- std --------------------------------------------------------------------
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <unordered_map>

namespace OpenMS
{

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace detail
{

static constexpr uint64_t encodeCoord(uint32_t x, uint32_t y, uint32_t z) noexcept
{
    // Each axis fits in 20 bits (x/y up to ~1 M pixels, z up to 1 M planes)
    return (static_cast<uint64_t>(z & 0xFFFFF) << 40) |
           (static_cast<uint64_t>(y & 0xFFFFF) << 20) |
            static_cast<uint64_t>(x & 0xFFFFF);
}

static std::string binaryTypeName(imzml::BinaryDataType t) noexcept
{
    switch (t)
    {
        case imzml::BinaryDataType::Float32: return "float32";
        case imzml::BinaryDataType::Float64: return "float64";
        case imzml::BinaryDataType::Int32:   return "int32";
        case imzml::BinaryDataType::Int64:   return "int64";
        default:                             return "unknown";
    }
}

} // namespace detail

// ---------------------------------------------------------------------------
// Impl
// ---------------------------------------------------------------------------
struct OnDiscImzMLExperiment::Impl
{
    imzml::ImzMLFile                                  file;
    ImzMLMetadata                                     meta;
    std::vector<SpectrumIndexEntry>                   index;
    bool                                              open_{false};
    mutable std::unordered_map<uint64_t, std::size_t> coordMap;
    mutable bool                                      coordMapBuilt{false};
};

// ---------------------------------------------------------------------------
// Ctor / dtor / move
// ---------------------------------------------------------------------------
OnDiscImzMLExperiment::OnDiscImzMLExperiment()
    : pimpl_(std::make_unique<Impl>())
{}

OnDiscImzMLExperiment::~OnDiscImzMLExperiment() = default;

OnDiscImzMLExperiment::OnDiscImzMLExperiment(
    OnDiscImzMLExperiment&&) noexcept = default;

OnDiscImzMLExperiment&
OnDiscImzMLExperiment::operator=(OnDiscImzMLExperiment&&) noexcept = default;

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------
void OnDiscImzMLExperiment::open(const std::string& imzmlPath,
                                  const std::string& ibdPath)
{
    pimpl_->open_         = false;
    pimpl_->coordMapBuilt = false;
    pimpl_->coordMap.clear();
    pimpl_->index.clear();

    pimpl_->file.load(imzmlPath, ibdPath);   // parse XML + open .ibd fd

    // ---- Build SpectrumIndexEntry vector from native spectra list ----------
    const auto& native = pimpl_->file.spectra();
    pimpl_->index.reserve(native.size());

    for (const auto& s : native)
    {
        SpectrumIndexEntry e;
        e.index      = s.index;
        e.x          = static_cast<uint32_t>(s.coord.x);
        e.y          = static_cast<uint32_t>(s.coord.y);
        e.z          = static_cast<uint32_t>(s.coord.z);
        e.mz_offset  = s.mzOffset;
        e.mz_length  = s.mzLength;
        e.mz_type    = detail::binaryTypeName(s.mzDataType);
        e.int_offset = s.intensityOffset;
        e.int_length = s.intensityLength;
        e.int_type   = detail::binaryTypeName(s.intensityDataType);
        pimpl_->index.push_back(std::move(e));
    }

    // ---- Promote native ImzMLMetadata → OpenMS::ImzMLMetadata -------------
    const auto& src = pimpl_->file.metadata();
    auto&       dst = pimpl_->meta;

    switch (src.mode)
    {
        case imzml::ImagingMode::Continuous:
            dst.imagingMode = ImzMLMetadata::ImagingMode::Continuous; break;
        case imzml::ImagingMode::Processed:
            dst.imagingMode = ImzMLMetadata::ImagingMode::Processed;  break;
        default:
            dst.imagingMode = ImzMLMetadata::ImagingMode::Unknown;    break;
    }

    dst.uuid          = src.uuid;
    dst.schemaVersion = src.schemaVersion;

    if (!src.ibdSha1.empty())
    {
        dst.ibdChecksum     = src.ibdSha1;
        dst.ibdChecksumType = "SHA-1";
    }
    else if (!src.ibdMd5.empty())
    {
        dst.ibdChecksum     = src.ibdMd5;
        dst.ibdChecksumType = "MD5";
    }

    const auto& ss = src.scanSettings;
    dst.maxX       = static_cast<uint32_t>(ss.maxCountX);
    dst.maxY       = static_cast<uint32_t>(ss.maxCountY);
    dst.maxZ       = 1;
    dst.pixelSizeX = static_cast<uint32_t>(ss.pixelSizeX);
    dst.pixelSizeY = static_cast<uint32_t>(ss.pixelSizeY);
    dst.maxDimX    = ss.maxDimX;
    dst.maxDimY    = ss.maxDimY;

    pimpl_->open_ = true;
}

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
bool        OnDiscImzMLExperiment::isOpen()  const noexcept { return pimpl_->open_; }
std::size_t OnDiscImzMLExperiment::size()    const noexcept { return pimpl_->index.size(); }
uint32_t    OnDiscImzMLExperiment::gridWidth()  const noexcept { return pimpl_->meta.maxX; }
uint32_t    OnDiscImzMLExperiment::gridHeight() const noexcept { return pimpl_->meta.maxY; }

const ImzMLMetadata&
OnDiscImzMLExperiment::getImzMLMetadata() const noexcept
{
    return pimpl_->meta;
}

// ---------------------------------------------------------------------------
// Index access
// ---------------------------------------------------------------------------
const SpectrumIndexEntry&
OnDiscImzMLExperiment::getSpectrumIndex(std::size_t i) const
{
    if (i >= pimpl_->index.size())
        throw std::out_of_range("OnDiscImzMLExperiment::getSpectrumIndex: index "
                                + std::to_string(i) + " >= size "
                                + std::to_string(pimpl_->index.size()));
    return pimpl_->index[i];
}

// ---------------------------------------------------------------------------
// On-demand decode
// ---------------------------------------------------------------------------
MSSpectrum OnDiscImzMLExperiment::getSpectrum(std::size_t i) const
{
    if (i >= pimpl_->index.size())
        throw std::out_of_range("OnDiscImzMLExperiment::getSpectrum: index "
                                + std::to_string(i) + " >= size "
                                + std::to_string(pimpl_->index.size()));

    const imzml::SpectrumData data = pimpl_->file.getSpectrum(i); // 1 IBD read
    const SpectrumIndexEntry& e    = pimpl_->index[i];

    MSSpectrum s;
    s.setCoordX(e.x);
    s.setCoordY(e.y);
    s.setCoordZ(e.z);
    s.setMSLevel(1);

    const std::size_t n = std::min(data.mz.size(), data.intensity.size());
    s.resize(n);
    for (std::size_t j = 0; j < n; ++j)
    {
        s[j].setMZ(data.mz[j]);
        s[j].setIntensity(static_cast<Peak1D::IntensityType>(data.intensity[j]));
    }
    return s;
}

MSSpectrum
OnDiscImzMLExperiment::getSpectrumAtCoordinate(uint32_t x, uint32_t y,
                                                uint32_t z) const
{
    if (!pimpl_->coordMapBuilt)
    {
        pimpl_->coordMap.reserve(pimpl_->index.size());
        for (std::size_t i = 0; i < pimpl_->index.size(); ++i)
        {
            const auto& e = pimpl_->index[i];
            pimpl_->coordMap[detail::encodeCoord(e.x, e.y, e.z)] = i;
        }
        pimpl_->coordMapBuilt = true;
    }

    const auto it = pimpl_->coordMap.find(detail::encodeCoord(x, y, z));
    if (it == pimpl_->coordMap.end())
    {
        std::ostringstream oss;
        oss << "OnDiscImzMLExperiment::getSpectrumAtCoordinate: no spectrum at ("
            << x << ", " << y << ", " << z << ')';
        throw std::out_of_range(oss.str());
    }
    return getSpectrum(it->second);
}

} // namespace OpenMS
