// ---------------------------------------------------------------------------
// OpenMS/FORMAT/PeakFileOptions.h
// File-loading configuration — mirrors OpenMS PeakFileOptions
//
// Extended for imzML:
//   - coordinate filter (pixel bounding box)
//   - skip IBD decode option (metadata-only load)
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace OpenMS
{

class PeakFileOptions
{
public:
    PeakFileOptions() = default;

    // ------------------------------------------------------------------
    // MS-level filter (identical to OpenMS API)
    // ------------------------------------------------------------------
    void                     setMSLevels(const std::vector<int>& levels) { msLevels_ = levels; }
    const std::vector<int>&  getMSLevels() const                         { return msLevels_; }
    bool                     hasMSLevels() const                         { return !msLevels_.empty(); }
    bool                     containsMSLevel(int lvl) const
    {
        for (int l : msLevels_) if (l == lvl) return true;
        return false;
    }

    // ------------------------------------------------------------------
    // m/z range filter
    // ------------------------------------------------------------------
    void setMZRange(double lo, double hi)  { mzRange_ = {lo, hi}; }
    bool hasMZRange() const                { return mzRange_.has_value(); }
    std::pair<double,double> getMZRange() const { return mzRange_.value_or(std::make_pair(0.0, 0.0)); }

    // ------------------------------------------------------------------
    // Intensity range filter
    // ------------------------------------------------------------------
    void setIntensityRange(float lo, float hi) { intRange_ = {lo, hi}; }
    bool hasIntensityRange() const              { return intRange_.has_value(); }
    std::pair<float,float> getIntensityRange() const { return intRange_.value_or(std::make_pair(0.0f, 0.0f)); }

    // ------------------------------------------------------------------
    // imzML pixel coordinate filter (bounding box in pixel units)
    // ------------------------------------------------------------------
    void setCoordinateFilter(uint32_t xMin, uint32_t xMax,
                             uint32_t yMin, uint32_t yMax,
                             uint32_t zMin = 1, uint32_t zMax = 1)
    {
        coordFilter_ = CoordFilter{xMin, xMax, yMin, yMax, zMin, zMax};
    }

    bool hasCoordinateFilter() const { return coordFilter_.has_value(); }

    bool passesCoordinateFilter(uint32_t x, uint32_t y, uint32_t z) const
    {
        if (!coordFilter_) return true;
        const auto& f = *coordFilter_;
        return (x >= f.xMin && x <= f.xMax &&
                y >= f.yMin && y <= f.yMax &&
                z >= f.zMin && z <= f.zMax);
    }

    // ------------------------------------------------------------------
    // Metadata-only load: parse XML but skip IBD binary decode
    // ------------------------------------------------------------------
    void setSkipIBDDecode(bool skip) { skipIBD_ = skip; }
    bool getSkipIBDDecode() const    { return skipIBD_; }

    // Number of spectra limit (0 = no limit)
    void        setMaxSpectra(std::size_t n) { maxSpectra_ = n; }
    std::size_t getMaxSpectra() const        { return maxSpectra_; }

    // ------------------------------------------------------------------
    // Sort peaks by m/z after decode (default: true)
    // ------------------------------------------------------------------
    void setSortMZ(bool sort) { sortMZ_ = sort; }
    bool getSortMZ()   const  { return sortMZ_; }

private:
    struct CoordFilter
    {
        uint32_t xMin, xMax, yMin, yMax, zMin, zMax;
    };

    std::vector<int>                  msLevels_;
    std::optional<std::pair<double,double>> mzRange_;
    std::optional<std::pair<float,float>>   intRange_;
    std::optional<CoordFilter>        coordFilter_;
    bool                              skipIBD_     {false};
    std::size_t                       maxSpectra_  {0};
    bool                              sortMZ_      {true};
};

} // namespace OpenMS
