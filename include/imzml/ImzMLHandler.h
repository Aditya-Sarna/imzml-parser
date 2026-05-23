// ---------------------------------------------------------------------------
// ImzMLHandler.h
// SAX handler for .imzML files.
//
// Mirrors OpenMS/include/OpenMS/FORMAT/HANDLERS/MzMLHandler.h:
//   - Maintains a stack of open element names (open_tags_)
//   - Dispatches cvParam handling via handleCvParam_()
//   - Populates ImzMLExperiment passed by reference from ImzMLFile
// ---------------------------------------------------------------------------
#pragma once

#include "imzml/XMLHandler.h"
#include "imzml/ImzMLTypes.h"
#include "imzml/ImzMLSpectrum.h"

#include <string>
#include <vector>
#include <unordered_map>

namespace imzml
{
namespace Internal
{

// ---------------------------------------------------------------------------
// ImzMLHandler
//
// State machine driven by SAX events.
// Context tags tracked with open_tags_ vector (same pattern as MzMLHandler).
// ---------------------------------------------------------------------------
class ImzMLHandler : public XMLHandler
{
public:
    // ------------------------------------------------------------------
    // Constructor
    //   filename   - path to the .imzML file (passed to XMLHandler)
    //   metadata   - destination for file-level metadata
    //   spectra    - destination for per-spectrum records
    // ------------------------------------------------------------------
    ImzMLHandler(const std::string&         filename,
                 ImzMLMetadata&             metadata,
                 std::vector<ImzMLSpectrum>& spectra);

    ~ImzMLHandler() override = default;

protected:
    // SAX callbacks (overrides of XMLHandler virtual methods)
    void startElement(const std::string& tag,
                      const XML_Char**   attrs) override;

    void endElement(const std::string& tag) override;

    void characters(const std::string& chars) override;

private:
    // ------------------------------------------------------------------
    // cvParam dispatch (called from startElement when tag == "cvParam")
    // Mirrors MzMLHandler::handleCVParam_()
    // ------------------------------------------------------------------
    void handleCvParam_(const std::string& accession,
                        const std::string& name,
                        const std::string& value);

    // ------------------------------------------------------------------
    // Helpers
    // ------------------------------------------------------------------
    const std::string& parentTag_() const;
    const std::string& grandParentTag_() const;

    // Resolve BinaryDataType from a CV accession string
    static BinaryDataType dataTypeFromAccession_(const std::string& acc);

    // Finalize a <spectrum> element once we see its closing tag
    void finalizeSpectrum_();

    // ------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------
    ImzMLMetadata&              metadata_;
    std::vector<ImzMLSpectrum>& spectra_;

    // Stack of open element names - same pattern as MzMLHandler::open_tags_
    std::vector<std::string>    openTags_;

    // ------------------------------------------------------------------
    // Per-spectrum parsing state (reset on each <spectrum> open tag)
    // ------------------------------------------------------------------
    ImzMLSpectrum               currentSpectrum_;

    // Referenceably param groups: id -> list of CvParams
    // (used for continuous mode where a shared refGroup holds data type)
    std::unordered_map<std::string, std::vector<CvParam>> refParamGroups_;
    std::string                 currentRefGroupId_;

    // Which binary array are we currently inside?
    bool                        inMzArray_{false};
    bool                        inIntArray_{false};
    BinaryArrayMeta             currentArray_;

    // referenceableParamGroupRef within a binaryDataArray
    // Maps group-id -> BinaryDataType (resolved once seen in refParamGroups)
    std::string                 mzRefGroupId_;
    std::string                 intRefGroupId_;

    // Spectrum-level temporary storage
    bool                        inSpectrum_{false};
    bool                        inScanList_{false};
    int                         currentSpectrumIndex_{0};

    // Character data accumulator
    std::string                 charData_;
};

} // namespace Internal
} // namespace imzml
