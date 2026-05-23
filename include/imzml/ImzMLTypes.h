// ---------------------------------------------------------------------------
// ImzMLTypes.h
// Core types, enumerations, and controlled-vocabulary (CV) accession constants
// for the imzML parser.
//
// Architecture mirrors OpenMS/include/OpenMS/FORMAT/HANDLERS/MzMLHandler.h:
//   - ImzMLFile       (public API)  == MzMLFile
//   - ImzMLHandler    (SAX handler) == MzMLHandler
//   - IBDReader                     == inline binary decode helpers in MzMLHandler
// ---------------------------------------------------------------------------
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

namespace imzml
{

// ---------------------------------------------------------------------------
// Binary data type
// MS:1000521  32-bit float
// MS:1000523  64-bit float
// MS:1000519  32-bit integer
// MS:1000522  64-bit integer
// ---------------------------------------------------------------------------
enum class BinaryDataType
{
    Unknown,
    Float32,   // MS:1000521
    Float64,   // MS:1000523
    Int32,     // MS:1000519
    Int64      // MS:1000522
};

// ---------------------------------------------------------------------------
// Imaging mode
// IMS:1000030  continuous
// IMS:1000031  processed
// ---------------------------------------------------------------------------
enum class ImagingMode
{
    Unknown,
    Continuous,  // IMS:1000030  - shared m/z array
    Processed    // IMS:1000031  - per-spectrum m/z array
};

// ---------------------------------------------------------------------------
// Return value of BinaryDataType::bytes()
// ---------------------------------------------------------------------------
inline std::size_t bytesForType(BinaryDataType t)
{
    switch (t)
    {
        case BinaryDataType::Float32: return 4;
        case BinaryDataType::Float64: return 8;
        case BinaryDataType::Int32:   return 4;
        case BinaryDataType::Int64:   return 8;
        default:                      return 0;
    }
}

// ---------------------------------------------------------------------------
// Controlled-vocabulary accessions used during SAX parsing.
// (cv_name -> accession string)
//
// MS ontology
// ---------------------------------------------------------------------------
namespace cv
{
    // Data types
    constexpr const char* FLOAT32          = "MS:1000521";
    constexpr const char* FLOAT64          = "MS:1000523";
    constexpr const char* INT32            = "MS:1000519";
    constexpr const char* INT64            = "MS:1000522";

    // Array types
    constexpr const char* MZ_ARRAY         = "MS:1000514";
    constexpr const char* INTENSITY_ARRAY  = "MS:1000515";

    // Imaging mode (IMS ontology)
    constexpr const char* CONTINUOUS       = "IMS:1000030";
    constexpr const char* PROCESSED        = "IMS:1000031";

    // IBD binary layout (IMS ontology)
    constexpr const char* EXTERNAL_OFFSET  = "IMS:1000102";
    constexpr const char* ARRAY_LENGTH     = "IMS:1000103";
    constexpr const char* ENCODED_LENGTH   = "IMS:1000104";   // optional

    // Pixel coordinates (IMS ontology)
    constexpr const char* POSITION_X       = "IMS:1000050";
    constexpr const char* POSITION_Y       = "IMS:1000051";
    constexpr const char* POSITION_Z       = "IMS:1000052";

    // Scan settings (IMS ontology)
    constexpr const char* MAX_COUNT_X      = "IMS:1000042";
    constexpr const char* MAX_COUNT_Y      = "IMS:1000043";
    constexpr const char* MAX_DIM_X        = "IMS:1000044";
    constexpr const char* MAX_DIM_Y        = "IMS:1000045";
    constexpr const char* PIXEL_SIZE_X     = "IMS:1000046";
    constexpr const char* PIXEL_SIZE_Y     = "IMS:1000047";

    // IBD identification
    constexpr const char* UUID             = "IMS:1000080";
    constexpr const char* IBD_MD5          = "IMS:1000090";
    constexpr const char* IBD_SHA1         = "IMS:1000091";
}

// ---------------------------------------------------------------------------
// Parsed representation of a single <cvParam> element
// ---------------------------------------------------------------------------
struct CvParam
{
    std::string accession;
    std::string name;
    std::string value;
    std::string unitAccession;

    bool empty() const { return accession.empty(); }
};

// ---------------------------------------------------------------------------
// Pixel coordinate (1-based, matching the imzML spec)
// ---------------------------------------------------------------------------
struct Coordinate
{
    int32_t x{1};
    int32_t y{1};
    int32_t z{1};
};

// ---------------------------------------------------------------------------
// Per-spectrum binary info extracted from <binaryDataArray> elements
// ---------------------------------------------------------------------------
struct BinaryArrayMeta
{
    uint64_t      offset{0};       // IMS:1000102  byte offset in .ibd
    uint64_t      length{0};       // IMS:1000103  number of elements
    uint64_t      encodedLength{0};// IMS:1000104  encoded bytes (optional; 0 = infer)
    BinaryDataType dataType{BinaryDataType::Unknown};
    bool          isMzArray{false};
    bool          isIntensityArray{false};
};

// ---------------------------------------------------------------------------
// Global scan settings metadata parsed from <scanSettingsList>
// ---------------------------------------------------------------------------
struct ScanSettings
{
    int32_t  maxCountX{0};
    int32_t  maxCountY{0};
    double   maxDimX{0.0};
    double   maxDimY{0.0};
    double   pixelSizeX{0.0};
    double   pixelSizeY{0.0};
};

// ---------------------------------------------------------------------------
// File-level metadata
// ---------------------------------------------------------------------------
struct ImzMLMetadata
{
    std::string  uuid;
    std::string  ibdMd5;
    std::string  ibdSha1;
    std::string  schemaVersion;
    ImagingMode  mode{ImagingMode::Unknown};
    ScanSettings scanSettings;
};

} // namespace imzml
