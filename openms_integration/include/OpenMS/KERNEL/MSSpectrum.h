// ---------------------------------------------------------------------------
// OpenMS/KERNEL/MSSpectrum.h
// imzML MSSpectrum wrapper — uses the real OpenMS::Peak1D kernel type.
//
// Peak1D comes directly from the OpenMS 3.x headers (bioconda build).
// MSSpectrum and MSExperiment are extended here with imzML-specific
// pixel coordinate and metadata APIs that do not exist in stock OpenMS
// (in stock OpenMS these would live in MetaInfoInterface).
// ---------------------------------------------------------------------------
#pragma once

// Real OpenMS Peak1D — getMZ/setMZ/getIntensity/setIntensity/MZLess
#include <OpenMS/KERNEL/Peak1D.h>

#include <vector>
#include <string>
#include <algorithm>
#include <cstdint>

namespace OpenMS
{

// ---------------------------------------------------------------------------
// MSSpectrum
// ---------------------------------------------------------------------------
class MSSpectrum
{
public:
    // Mirrors OpenMS MSSpectrum sub-containers
    struct FloatDataArray
    {
        std::string         name;
        std::vector<float>  data;
    };
    struct IntegerDataArray
    {
        std::string           name;
        std::vector<int32_t>  data;
    };

    // Spectrum types — mirrors OpenMS SpectrumType
    enum SpectrumType { UNKNOWN, PROFILE, CENTROID };

    // ------------------------------------------------------------------
    MSSpectrum() = default;

    // Peaks access — mirrors OpenMS MSSpectrum container interface
    std::size_t       size()  const noexcept { return peaks_.size(); }
    bool              empty() const noexcept { return peaks_.empty(); }
    Peak1D&           operator[](std::size_t i)       { return peaks_[i]; }
    const Peak1D&     operator[](std::size_t i) const { return peaks_[i]; }

    void push_back(const Peak1D& p) { peaks_.push_back(p); }
    void resize(std::size_t n)      { peaks_.resize(n); }
    void clear()                    { peaks_.clear(); mzArray_.clear(); intArray_.clear(); fdArrays_.clear(); idArrays_.clear(); }
    void reserve(std::size_t n)     { peaks_.reserve(n); }

    std::vector<Peak1D>::iterator       begin()       { return peaks_.begin(); }
    std::vector<Peak1D>::iterator       end()         { return peaks_.end(); }
    std::vector<Peak1D>::const_iterator begin() const { return peaks_.begin(); }
    std::vector<Peak1D>::const_iterator end()   const { return peaks_.end(); }

    void sortByPosition()
    {
        std::sort(peaks_.begin(), peaks_.end(), Peak1D::MZLess{});
    }


    // ------------------------------------------------------------------
    // Meta-data
    // ------------------------------------------------------------------
    const std::string& getName()   const { return name_; }
    void               setName(const std::string& n) { name_ = n; }

    int   getMSLevel()  const              { return msLevel_; }
    void  setMSLevel(int lvl)              { msLevel_ = lvl; }

    double getRT() const                   { return rt_; }
    void   setRT(double rt)                { rt_ = rt; }

    SpectrumType getType() const           { return type_; }
    void         setType(SpectrumType t)   { type_ = t; }

    // ------------------------------------------------------------------
    // imzML pixel coordinate extensions
    // (in stock OpenMS these live in a MetaInfoInterface map under keys
    //  "[MS:imzML] pixel x" etc.; we surface them as first-class members
    //  for performance)
    // ------------------------------------------------------------------
    uint32_t getCoordX() const          { return cx_; }
    uint32_t getCoordY() const          { return cy_; }
    uint32_t getCoordZ() const          { return cz_; }
    void     setCoordX(uint32_t x)      { cx_ = x; }
    void     setCoordY(uint32_t y)      { cy_ = y; }
    void     setCoordZ(uint32_t z)      { cz_ = z; }

    // Compound accessor (OpenMS uses getMetaValue)
    void setCoordinate(uint32_t x, uint32_t y, uint32_t z)
    {
        cx_ = x; cy_ = y; cz_ = z;
    }

    // ------------------------------------------------------------------
    // FloatDataArrays / IntegerDataArrays (mirrors OpenMS exactly)
    // ------------------------------------------------------------------
    std::vector<FloatDataArray>&   getFloatDataArrays()         { return fdArrays_; }
    const std::vector<FloatDataArray>&   getFloatDataArrays()   const { return fdArrays_; }
    std::vector<IntegerDataArray>& getIntegerDataArrays()       { return idArrays_; }
    const std::vector<IntegerDataArray>& getIntegerDataArrays() const { return idArrays_; }

    // ------------------------------------------------------------------
    // Raw decoded arrays (populated before peaks are zipped)
    // Used internally by ImzMLHandler, then zipped into peaks_
    // ------------------------------------------------------------------
    std::vector<double>& mzArray()        { return mzArray_; }
    std::vector<float>&  intensityArray() { return intArray_; }

    const std::vector<double>& mzArray()        const { return mzArray_; }
    const std::vector<float>&  intensityArray() const { return intArray_; }

    // Zip raw arrays → peaks_ (clears raw arrays afterwards)
    void zipArraysToPeaks()
    {
        std::size_t n = std::min(mzArray_.size(), intArray_.size());
        peaks_.resize(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            peaks_[i].setMZ(mzArray_[i]);
            peaks_[i].setIntensity(static_cast<Peak1D::IntensityType>(intArray_[i]));
        }
        mzArray_.clear();
        intArray_.clear();
    }

private:
    std::string                   name_;
    int                           msLevel_ {1};
    double                        rt_      {0.0};
    SpectrumType                  type_    {UNKNOWN};

    uint32_t                      cx_ {0}, cy_ {0}, cz_ {1};

    std::vector<Peak1D>           peaks_;
    std::vector<double>           mzArray_;
    std::vector<float>            intArray_;
    std::vector<FloatDataArray>   fdArrays_;
    std::vector<IntegerDataArray> idArrays_;
};

} // namespace OpenMS
