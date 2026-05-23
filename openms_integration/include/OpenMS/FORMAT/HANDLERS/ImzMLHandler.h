// ---------------------------------------------------------------------------
// OpenMS/FORMAT/HANDLERS/ImzMLHandler.h
// Xerces-C SAX2 handler for imzML — mirrors OpenMS MzMLHandler architecture
//
// Architecture:
//   xercesc::DefaultHandler
//     └── OpenMS::Internal::XMLHandler          (XMLHandler.h)
//           └── OpenMS::Internal::ImzMLHandler  (this file)
//
// Mirrors MzMLHandler in OpenMS:
//   - open_tags_  vector<string> tracks element nesting
//   - ref_param_groups_ map resolves referenceableParamGroup cvParams
//   - startElement() / endElement() / characters() overrides
//   - handleCvParam_() centralised CV dispatch method
//   - finaliseSpectrum_() called on </spectrum>
//   - Consumer-based streaming: every parsed spectrum is immediately
//     forwarded to the IMSDataConsumer to avoid in-memory accumulation
// ---------------------------------------------------------------------------
#pragma once

#include <OpenMS/FORMAT/HANDLERS/XMLHandler.h>
#include <OpenMS/KERNEL/MSSpectrum.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/INTERFACES/IMSDataConsumer.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace OpenMS
{
namespace Internal
{

// ---------------------------------------------------------------------------
// CV-accession string constants (IMS + MS ontologies)
// ---------------------------------------------------------------------------
namespace cv
{
    // Binary data types (MS ontology)
    inline constexpr const char* FLOAT32       = "MS:1000521";  // 32-bit float
    inline constexpr const char* FLOAT64       = "MS:1000523";  // 64-bit float
    inline constexpr const char* INT32         = "MS:1000519";  // 32-bit integer
    inline constexpr const char* INT64         = "MS:1000522";  // 64-bit integer

    // Binary data purpose
    inline constexpr const char* MZ_ARRAY      = "MS:1000514";  // m/z array
    inline constexpr const char* INT_ARRAY     = "MS:1000515";  // intensity array

    // Imaging mode (IMS ontology)
    inline constexpr const char* CONTINUOUS    = "IMS:1000030";
    inline constexpr const char* PROCESSED     = "IMS:1000031";

    // IBD layout
    inline constexpr const char* EXTERNAL_DATA  = "IMS:1000101";
    inline constexpr const char* EXTERNAL_OFFSET = "IMS:1000102";
    inline constexpr const char* EXTERNAL_LEN    = "IMS:1000103";
    inline constexpr const char* EXTERNAL_ENC    = "IMS:1000104";

    // IBD checksum
    inline constexpr const char* IBD_SHA1       = "IMS:1000091";
    inline constexpr const char* IBD_MD5        = "IMS:1000090";

    // Pixel coordinates
    inline constexpr const char* POSITION_X     = "IMS:1000050";
    inline constexpr const char* POSITION_Y     = "IMS:1000051";
    inline constexpr const char* POSITION_Z     = "IMS:1000052";

    // Scan settings
    inline constexpr const char* PIXEL_SIZE_X   = "IMS:1000046";
    inline constexpr const char* PIXEL_SIZE_Y   = "IMS:1000047";
    inline constexpr const char* SCAN_PATTERN   = "IMS:1000401";

    // UUID
    inline constexpr const char* UUID           = "IMS:1000080";

    // m/z array purpose (used to distinguish mz vs intensity binaryDataArray)
    inline constexpr const char* MZ_PURPOSE     = "MS:1000514";
    inline constexpr const char* INT_PURPOSE    = "MS:1000515";

    // Polarity
    inline constexpr const char* NEGATIVE_SCAN  = "MS:1000129";
    inline constexpr const char* POSITIVE_SCAN  = "MS:1000130";

    // Physical extents
    inline constexpr const char* MAX_DIM_X      = "IMS:1000044";
    inline constexpr const char* MAX_DIM_Y      = "IMS:1000045";

    // Scan direction / pattern
    inline constexpr const char* SCAN_TOP_DOWN  = "IMS:1000401";
    inline constexpr const char* SCAN_BOTTOM_UP = "IMS:1000402";
    inline constexpr const char* SCAN_FLYBACK   = "IMS:1000413";
    inline constexpr const char* SCAN_MEANDER   = "IMS:1000412";
    inline constexpr const char* SCAN_H_LINE    = "IMS:1000480";
    inline constexpr const char* SCAN_V_LINE    = "IMS:1000481";
    inline constexpr const char* SCAN_LR        = "IMS:1000491";
    inline constexpr const char* SCAN_RL        = "IMS:1000492";
} // namespace cv

// ---------------------------------------------------------------------------
// Per-array metadata accumulated while parsing a binaryDataArray element
// ---------------------------------------------------------------------------
struct ArrayMeta
{
    // Purpose
    bool isMzArray      {false};
    bool isIntArray     {false};

    // Type
    enum DataType { Float32, Float64, Int32, Int64, Unknown } dataType {Unknown};

    // IBD layout
    bool     externalData {false};
    uint64_t externalOffset {0};
    uint64_t externalLength {0};   // element count
    uint64_t externalEncLen {0};   // byte count (optional)

    void reset()
    {
        isMzArray = false; isIntArray = false;
        dataType  = Unknown;
        externalData   = false;
        externalOffset = 0;
        externalLength = 0;
        externalEncLen = 0;
    }
};

// ---------------------------------------------------------------------------
// ImzMLHandler
// ---------------------------------------------------------------------------
class ImzMLHandler : public XMLHandler
{
public:
    // ------------------------------------------------------------------
    // Constructor: consumer-based streaming interface (like OpenMS)
    // ------------------------------------------------------------------
    ImzMLHandler(const std::string&  filename,
                 IMSDataConsumer*    consumer,
                 MSExperiment&       metaHolder,
                 const PeakFileOptions& opts = PeakFileOptions{});

    ~ImzMLHandler() override
    {
        if (ibd_file_) { fclose(ibd_file_); ibd_file_ = nullptr; }
    }

    // Called by ImzMLFile::load() after parse completes
    const MSExperiment& getMetaHolder() const { return meta_; }

    // ------------------------------------------------------------------
    // Xerces SAX2 overrides
    // ------------------------------------------------------------------
    void startElement(const XMLCh* const uri,
                      const XMLCh* const localname,
                      const XMLCh* const qname,
                      const xercesc::Attributes& attrs) override;

    void endElement(const XMLCh* const uri,
                    const XMLCh* const localname,
                    const XMLCh* const qname) override;

    void characters(const XMLCh* const chars,
                    const XMLSize_t    length) override;

private:
    // ------------------------------------------------------------------
    // State machine
    // ------------------------------------------------------------------
    std::vector<std::string> open_tags_;   // mirrors MzMLHandler::open_tags_

    // referenceableParamGroup id → vector of (accession, value) pairs
    using CvPair = std::pair<std::string /*accession*/, std::string /*value*/>;
    std::unordered_map<std::string, std::vector<CvPair>> ref_param_groups_;
    std::string current_ref_group_id_;

    // Spectrum state
    MSSpectrum   current_spectrum_;
    ArrayMeta    current_array_;
    bool         in_spectrum_         {false};
    bool         in_scan_             {false};
    bool         in_binary_data_array_{false};
    bool         in_ref_param_group_  {false};
    std::size_t  spectrum_count_      {0};
    std::size_t  spectra_written_     {0};

    // Dataset-level metadata
    MSExperiment& meta_;
    IMSDataConsumer* consumer_;     // may be null (metadata-only load)
    PeakFileOptions  opts_;

    // IBD file handle
    mutable FILE*    ibd_file_    {nullptr};

    // ------------------------------------------------------------------
    // Private helpers
    // ------------------------------------------------------------------
    void handleCvParam_(const std::string& accession,
                        const std::string& value,
                        const std::string& unit_accession = {});

    void applyRefGroupParams_(const std::string& ref_id);

    void finaliseSpectrum_();
    void decodeArray_(ArrayMeta& a, MSSpectrum& s);

    // Return the current parent tag (second-to-last in open_tags_)
    const std::string& parentTag_() const;

    bool openIBD_();
};

} // namespace Internal
} // namespace OpenMS
