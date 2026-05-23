// ---------------------------------------------------------------------------
// ImzMLHandler.cpp
// SAX handler for .imzML files.
//
// Parsing logic follows the imzML 1.1.0 specification and mirrors the
// element-dispatch pattern in OpenMS MzMLHandler::startElement().
//
// Key XML elements handled:
//   <mzML>                        - schema version
//   <fileDescription>             - imaging mode cv params
//   <referenceableParamGroup>     - shared param groups (data types)
//   <referenceableParamGroupRef>  - reference to data type group
//   <cvParam>                     - all CV metadata
//   <scanSettingsList/scanSettings> - pixel grid metadata
//   <spectrumList>                - spectrum count
//   <spectrum>                    - per-spectrum context
//   <scanList/scan>               - pixel coordinates
//   <binaryDataArrayList>         - contains mz + intensity arrays
//   <binaryDataArray>             - one array (mz or intensity)
// ---------------------------------------------------------------------------
#include "imzml/ImzMLHandler.h"

#include <sstream>
#include <stdexcept>
#include <cassert>
#include <iostream>

namespace imzml
{
namespace Internal
{

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ImzMLHandler::ImzMLHandler(const std::string&          filename,
                             ImzMLMetadata&              metadata,
                             std::vector<ImzMLSpectrum>& spectra)
    : XMLHandler(filename)
    , metadata_(metadata)
    , spectra_(spectra)
{}

// ---------------------------------------------------------------------------
// parentTag_ / grandParentTag_
// Mirrors open_tags_[open_tags_.size()-2] pattern in OpenMS MzMLHandler
// ---------------------------------------------------------------------------
const std::string& ImzMLHandler::parentTag_() const
{
    static const std::string empty{};
    if (openTags_.size() < 2)
        return empty;
    return openTags_[openTags_.size() - 2];
}

const std::string& ImzMLHandler::grandParentTag_() const
{
    static const std::string empty{};
    if (openTags_.size() < 3)
        return empty;
    return openTags_[openTags_.size() - 3];
}

// ---------------------------------------------------------------------------
// dataTypeFromAccession_
// Maps a CV accession string to BinaryDataType
// ---------------------------------------------------------------------------
BinaryDataType ImzMLHandler::dataTypeFromAccession_(const std::string& acc)
{
    if (acc == cv::FLOAT32) return BinaryDataType::Float32;
    if (acc == cv::FLOAT64) return BinaryDataType::Float64;
    if (acc == cv::INT32)   return BinaryDataType::Int32;
    if (acc == cv::INT64)   return BinaryDataType::Int64;
    // Handle older/alternative accessions
    if (acc == "IMS:1000141") return BinaryDataType::Float32; // not standard but seen
    if (acc == "IMS:1000142") return BinaryDataType::Float64;
    return BinaryDataType::Unknown;
}

// ---------------------------------------------------------------------------
// startElement
// The master dispatch: push tag, then handle element-specific logic.
// Structure mirrors MzMLHandler::startElement():
//   1. push to openTags_
//   2. switch on tag name
//   3. inside <cvParam>, delegate to handleCvParam_()
// ---------------------------------------------------------------------------
void ImzMLHandler::startElement(const std::string& tag,
                                 const XML_Char**   attrs)
{
    openTags_.push_back(tag);

    // ----------------------------------------------------------------
    // <mzML>
    // ----------------------------------------------------------------
    if (tag == "mzML")
    {
        metadata_.schemaVersion = optionalAttributeAsString(attrs, "version", "");
        return;
    }

    // ----------------------------------------------------------------
    // <referenceableParamGroup> - shared parameter template
    // ----------------------------------------------------------------
    if (tag == "referenceableParamGroup")
    {
        currentRefGroupId_ = attributeAsString(attrs, "id");
        refParamGroups_[currentRefGroupId_] = {};
        return;
    }

    // ----------------------------------------------------------------
    // <spectrum> - begin parsing a new spectrum record
    // ----------------------------------------------------------------
    if (tag == "spectrum")
    {
        inSpectrum_       = true;
        inMzArray_        = false;
        inIntArray_       = false;
        currentSpectrum_  = ImzMLSpectrum{};
        currentSpectrum_.index = currentSpectrumIndex_++;
        currentArray_     = BinaryArrayMeta{};
        return;
    }

    // ----------------------------------------------------------------
    // <scanList> / <scan>
    // ----------------------------------------------------------------
    if (tag == "scanList") { inScanList_ = true;  return; }
    if (tag == "scan")     {                       return; }

    // ----------------------------------------------------------------
    // <binaryDataArrayList> - contains two binaryDataArray children
    // ----------------------------------------------------------------
    if (tag == "binaryDataArrayList") { return; }

    // ----------------------------------------------------------------
    // <binaryDataArray> - one array within the spectrum
    // Reset which array we are filling; content type is determined by
    // the cvParams inside it.
    // ----------------------------------------------------------------
    if (tag == "binaryDataArray")
    {
        currentArray_ = BinaryArrayMeta{};
        inMzArray_    = false;
        inIntArray_   = false;
        return;
    }

    // ----------------------------------------------------------------
    // <referenceableParamGroupRef> inside a <binaryDataArray>
    // Resolves data type from the shared ref groups
    // ----------------------------------------------------------------
    if (tag == "referenceableParamGroupRef")
    {
        if (parentTag_() == "binaryDataArray")
        {
            std::string refId = attributeAsString(attrs, "ref");
            auto it = refParamGroups_.find(refId);
            if (it != refParamGroups_.end())
            {
                for (const auto& cp : it->second)
                {
                    // Data type cv
                    BinaryDataType dt = dataTypeFromAccession_(cp.accession);
                    if (dt != BinaryDataType::Unknown)
                        currentArray_.dataType = dt;

                    // Array role
                    if (cp.accession == cv::MZ_ARRAY)
                    {
                        currentArray_.isMzArray = true;
                        inMzArray_              = true;
                    }
                    if (cp.accession == cv::INTENSITY_ARRAY)
                    {
                        currentArray_.isIntensityArray = true;
                        inIntArray_                    = true;
                    }
                    if (cp.accession == cv::CONTINUOUS)
                        metadata_.mode = ImagingMode::Continuous;
                    if (cp.accession == cv::PROCESSED)
                        metadata_.mode = ImagingMode::Processed;
                }
            }
        }
        return;
    }

    // ----------------------------------------------------------------
    // <cvParam> - the workhorse element
    // ----------------------------------------------------------------
    if (tag == "cvParam")
    {
        std::string accession = optionalAttributeAsString(attrs, "accession", "");
        std::string name      = optionalAttributeAsString(attrs, "name",      "");
        std::string value     = optionalAttributeAsString(attrs, "value",     "");

        // Store in referenceableParamGroup if we are inside one
        if (!currentRefGroupId_.empty() && parentTag_() == "referenceableParamGroup")
        {
            CvParam cp;
            cp.accession = accession;
            cp.name      = name;
            cp.value     = value;
            refParamGroups_[currentRefGroupId_].push_back(cp);
        }

        handleCvParam_(accession, name, value);
        return;
    }
}

// ---------------------------------------------------------------------------
// endElement
// Pop the tag stack; finalise spectrum when we see </spectrum>.
// ---------------------------------------------------------------------------
void ImzMLHandler::endElement(const std::string& tag)
{
    if (!openTags_.empty())
        openTags_.pop_back();

    // ----------------------------------------------------------------
    // End of <referenceableParamGroup>
    // ----------------------------------------------------------------
    if (tag == "referenceableParamGroup")
    {
        currentRefGroupId_.clear();
        return;
    }

    // ----------------------------------------------------------------
    // End of <binaryDataArray>: commit the current array meta to the
    // spectrum being built.
    // ----------------------------------------------------------------
    if (tag == "binaryDataArray" && inSpectrum_)
    {
        if (currentArray_.isMzArray || inMzArray_)
        {
            currentSpectrum_.mzOffset   = currentArray_.offset;
            currentSpectrum_.mzLength   = currentArray_.length;
            currentSpectrum_.mzDataType = currentArray_.dataType;
        }
        else if (currentArray_.isIntensityArray || inIntArray_)
        {
            currentSpectrum_.intensityOffset   = currentArray_.offset;
            currentSpectrum_.intensityLength   = currentArray_.length;
            currentSpectrum_.intensityDataType = currentArray_.dataType;
        }
        inMzArray_  = false;
        inIntArray_ = false;
        currentArray_ = BinaryArrayMeta{};
        return;
    }

    // ----------------------------------------------------------------
    // End of <scanList>
    // ----------------------------------------------------------------
    if (tag == "scanList")
    {
        inScanList_ = false;
        return;
    }

    // ----------------------------------------------------------------
    // End of <spectrum>: push the completed record
    // ----------------------------------------------------------------
    if (tag == "spectrum")
    {
        finalizeSpectrum_();
        inSpectrum_ = false;
        return;
    }
}

// ---------------------------------------------------------------------------
// characters - not heavily used in imzML; left for completeness
// ---------------------------------------------------------------------------
void ImzMLHandler::characters(const std::string& /*chars*/) {}

// ---------------------------------------------------------------------------
// handleCvParam_
// Dispatch CV accession values onto the parser state machine.
// Mirrors MzMLHandler::handleCVParam_() with imzML-specific accessions.
// ---------------------------------------------------------------------------
void ImzMLHandler::handleCvParam_(const std::string& accession,
                                   const std::string& name,
                                   const std::string& value)
{
    const std::string& parent = parentTag_();
    const std::string& gp     = grandParentTag_();

    // ----------------------------------------------------------------
    // Imaging mode (in <fileDescription> or <referenceableParamGroup>)
    // ----------------------------------------------------------------
    if (accession == cv::CONTINUOUS)
    {
        metadata_.mode = ImagingMode::Continuous;
        return;
    }
    if (accession == cv::PROCESSED)
    {
        metadata_.mode = ImagingMode::Processed;
        return;
    }

    // ----------------------------------------------------------------
    // IBD identification
    // ----------------------------------------------------------------
    if (accession == cv::UUID)
    {
        if (!value.empty()) metadata_.uuid = value;
    }
    if (accession == cv::IBD_MD5)  { metadata_.ibdMd5  = value; return; }
    if (accession == cv::IBD_SHA1) { metadata_.ibdSha1 = value; return; }

    // ----------------------------------------------------------------
    // Scan settings (pixel grid dimensions, pixel size)
    // ----------------------------------------------------------------
    if (accession == cv::MAX_COUNT_X)
    {
        if (!value.empty()) metadata_.scanSettings.maxCountX = std::stoi(value);
        return;
    }
    if (accession == cv::MAX_COUNT_Y)
    {
        if (!value.empty()) metadata_.scanSettings.maxCountY = std::stoi(value);
        return;
    }
    if (accession == cv::MAX_DIM_X)
    {
        if (!value.empty()) metadata_.scanSettings.maxDimX = std::stod(value);
        return;
    }
    if (accession == cv::MAX_DIM_Y)
    {
        if (!value.empty()) metadata_.scanSettings.maxDimY = std::stod(value);
        return;
    }
    if (accession == cv::PIXEL_SIZE_X)
    {
        if (!value.empty()) metadata_.scanSettings.pixelSizeX = std::stod(value);
        return;
    }
    if (accession == cv::PIXEL_SIZE_Y)
    {
        if (!value.empty()) metadata_.scanSettings.pixelSizeY = std::stod(value);
        return;
    }

    // ----------------------------------------------------------------
    // Per-spectrum pixel coordinates (inside <scan>)
    // ----------------------------------------------------------------
    if (inSpectrum_ && inScanList_)
    {
        if (accession == cv::POSITION_X && !value.empty())
        {
            currentSpectrum_.coord.x = std::stoi(value);
            return;
        }
        if (accession == cv::POSITION_Y && !value.empty())
        {
            currentSpectrum_.coord.y = std::stoi(value);
            return;
        }
        if (accession == cv::POSITION_Z && !value.empty())
        {
            currentSpectrum_.coord.z = std::stoi(value);
            return;
        }
    }

    // ----------------------------------------------------------------
    // Binary array layout (inside <binaryDataArray>)
    // ----------------------------------------------------------------
    if (inSpectrum_ && parent == "binaryDataArray")
    {
        // External offset in .ibd
        if (accession == cv::EXTERNAL_OFFSET && !value.empty())
        {
            currentArray_.offset = static_cast<uint64_t>(std::stoull(value));
            return;
        }
        // Array length (number of elements)
        if (accession == cv::ARRAY_LENGTH && !value.empty())
        {
            currentArray_.length = static_cast<uint64_t>(std::stoull(value));
            return;
        }
        // Encoded length (optional; total bytes in .ibd)
        if (accession == cv::ENCODED_LENGTH && !value.empty())
        {
            currentArray_.encodedLength = static_cast<uint64_t>(std::stoull(value));
            return;
        }

        // Data type
        BinaryDataType dt = dataTypeFromAccession_(accession);
        if (dt != BinaryDataType::Unknown)
        {
            currentArray_.dataType = dt;
            return;
        }

        // Array role: m/z or intensity
        if (accession == cv::MZ_ARRAY)
        {
            currentArray_.isMzArray = true;
            inMzArray_              = true;
            return;
        }
        if (accession == cv::INTENSITY_ARRAY)
        {
            currentArray_.isIntensityArray = true;
            inIntArray_                    = true;
            return;
        }
    }

    // suppress unused-parameter warning
    (void)name;
    (void)gp;
}

// ---------------------------------------------------------------------------
// finalizeSpectrum_
// Called when </spectrum> is seen. Validates and stores the record.
// ---------------------------------------------------------------------------
void ImzMLHandler::finalizeSpectrum_()
{
    // Sanity: if data type not resolved from per-spectrum cvParams,
    // we may still have it unset (happens in continuous mode when the
    // data type is only in the referenceableParamGroup).  We leave it
    // as Unknown here; ImzMLFile::validate() will report it.
    spectra_.push_back(currentSpectrum_);
    currentSpectrum_ = ImzMLSpectrum{};
}

} // namespace Internal
} // namespace imzml
