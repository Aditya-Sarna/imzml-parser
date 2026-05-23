// ---------------------------------------------------------------------------
// ImzMLHandler.cpp — Xerces-C SAX2 implementation
// ---------------------------------------------------------------------------
#include <OpenMS/FORMAT/HANDLERS/ImzMLHandler.h>

#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/PlatformUtils.hpp>

#include <string>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <cassert>
#include <stdexcept>
#include <algorithm>

namespace OpenMS
{
namespace Internal
{

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------
ImzMLHandler::ImzMLHandler(const std::string&     filename,
                            IMSDataConsumer*        consumer,
                            MSExperiment&           metaHolder,
                            const PeakFileOptions& opts)
    : XMLHandler(filename, "1.1.0"),
      meta_(metaHolder),
      consumer_(consumer),
      opts_(opts)
{}

// ---------------------------------------------------------------------------
// Helper: parent tag
// ---------------------------------------------------------------------------
const std::string& ImzMLHandler::parentTag_() const
{
    static const std::string empty;
    if (open_tags_.size() < 2) return empty;
    return open_tags_[open_tags_.size() - 2];
}

// ---------------------------------------------------------------------------
// Open IBD file lazily
// ---------------------------------------------------------------------------
bool ImzMLHandler::openIBD_()
{
    if (ibd_file_) return true;
    const std::string& path = meta_.getImzMLMetadata().ibdFilePath;
    if (path.empty()) return false;
    ibd_file_ = fopen(path.c_str(), "rb");
    return ibd_file_ != nullptr;
}

// ---------------------------------------------------------------------------
// startElement
// ---------------------------------------------------------------------------
void ImzMLHandler::startElement(const XMLCh* const /*uri*/,
                                 const XMLCh* const /*localname*/,
                                 const XMLCh* const qname,
                                 const xercesc::Attributes& attrs)
{
    std::string tag = sm_.convert(qname);
    open_tags_.push_back(tag);

    // ---- mzML root / fileDescription -----------------------------------
    if (tag == "spectrum")
    {
        in_spectrum_ = true;
        current_spectrum_ = MSSpectrum{};

        // spectrum index
        std::string idx_str;
        if (optionalAttributeAsString_(idx_str, attrs,
                xercesc::XMLString::transcode("index")))
        {
            // index attribute is informational
        }
        ++spectrum_count_;
    }
    else if (tag == "spectrumList")
    {
        std::string count_str;
        if (optionalAttributeAsString_(count_str, attrs,
                xercesc::XMLString::transcode("count")))
        {
            try
            {
                std::size_t total = std::stoull(count_str);
                if (consumer_) consumer_->setExpectedSize(total);
            }
            catch (...) {}
        }
        // Deliver meta-only snapshot to consumer before spectra stream
        if (consumer_) consumer_->setExperimentalSettings(meta_);
    }
    else if (tag == "scan")
    {
        in_scan_ = true;
    }
    else if (tag == "binaryDataArray")
    {
        in_binary_data_array_ = true;
        current_array_.reset();
    }
    else if (tag == "referenceableParamGroup")
    {
        in_ref_param_group_ = true;
        optionalAttributeAsString_(current_ref_group_id_, attrs,
            xercesc::XMLString::transcode("id"));
        ref_param_groups_[current_ref_group_id_]; // ensure entry exists
    }
    else if (tag == "referenceableParamGroupRef")
    {
        std::string ref_id;
        if (optionalAttributeAsString_(ref_id, attrs,
                xercesc::XMLString::transcode("ref")))
        {
            applyRefGroupParams_(ref_id);
        }
    }
    else if (tag == "cvParam")
    {
        // Attribute names as XMLCh
        XMLCh* acc_xch   = xercesc::XMLString::transcode("accession");
        XMLCh* val_xch   = xercesc::XMLString::transcode("value");
        XMLCh* unit_xch  = xercesc::XMLString::transcode("unitAccession");

        std::string accession = optionalAttributeAsString_(attrs, acc_xch);
        std::string value     = optionalAttributeAsString_(attrs, val_xch);
        std::string unit_acc  = optionalAttributeAsString_(attrs, unit_xch);

        xercesc::XMLString::release(&acc_xch);
        xercesc::XMLString::release(&val_xch);
        xercesc::XMLString::release(&unit_xch);

        if (!accession.empty())
        {
            if (in_ref_param_group_)
            {
                ref_param_groups_[current_ref_group_id_].emplace_back(accession, value);
            }
            else
            {
                handleCvParam_(accession, value, unit_acc);
            }
        }
    }
    else if (tag == "userParam")
    {
        // imzML sometimes uses userParam for pixel coordinates
        XMLCh* name_xch  = xercesc::XMLString::transcode("name");
        XMLCh* val_xch   = xercesc::XMLString::transcode("value");
        std::string pname = optionalAttributeAsString_(attrs, name_xch);
        std::string pval  = optionalAttributeAsString_(attrs, val_xch);
        xercesc::XMLString::release(&name_xch);
        xercesc::XMLString::release(&val_xch);
        (void)pname; (void)pval; // extend here if needed
    }
}

// ---------------------------------------------------------------------------
// endElement
// ---------------------------------------------------------------------------
void ImzMLHandler::endElement(const XMLCh* const /*uri*/,
                               const XMLCh* const /*localname*/,
                               const XMLCh* const qname)
{
    std::string tag = sm_.convert(qname);

    if (tag == "binaryDataArray")
    {
        in_binary_data_array_ = false;
        if (in_spectrum_ && !opts_.getSkipIBDDecode())
        {
            decodeArray_(current_array_, current_spectrum_);
        }
        // Track global data types (set on first encounter)
        auto dtStr = [](ArrayMeta::DataType dt) -> std::string {
            switch(dt) {
                case ArrayMeta::Float32: return "float32";
                case ArrayMeta::Float64: return "float64";
                case ArrayMeta::Int32:   return "int32";
                case ArrayMeta::Int64:   return "int64";
                default:                 return "unknown";
            }
        };
        if (current_array_.isMzArray && current_array_.dataType != ArrayMeta::Unknown)
        {
            auto& dt = meta_.getImzMLMetadata().mzDataType;
            if (dt == "unknown" || dt.empty()) dt = dtStr(current_array_.dataType);
        }
        if (current_array_.isIntArray && current_array_.dataType != ArrayMeta::Unknown)
        {
            auto& dt = meta_.getImzMLMetadata().intDataType;
            if (dt == "unknown" || dt.empty()) dt = dtStr(current_array_.dataType);
        }
        current_array_.reset();
    }
    else if (tag == "scan")
    {
        in_scan_ = false;
    }
    else if (tag == "spectrum")
    {
        in_spectrum_ = false;
        finaliseSpectrum_();
    }
    else if (tag == "referenceableParamGroup")
    {
        in_ref_param_group_ = false;
        current_ref_group_id_.clear();
    }

    if (!open_tags_.empty()) open_tags_.pop_back();
}

// ---------------------------------------------------------------------------
// characters — not used in imzML (no CDATA in the spec)
// ---------------------------------------------------------------------------
void ImzMLHandler::characters(const XMLCh* const /*chars*/,
                               const XMLSize_t    /*length*/)
{}

// ---------------------------------------------------------------------------
// handleCvParam_ — central dispatch (mirrors MzMLHandler::handleCvParam)
// ---------------------------------------------------------------------------
void ImzMLHandler::handleCvParam_(const std::string& accession,
                                   const std::string& value,
                                   const std::string& /*unit*/)
{
    // ---- Imaging mode (file-level) ------------------------------------
    if (accession == cv::CONTINUOUS)
    {
        meta_.getImzMLMetadata().imagingMode = ImzMLMetadata::ImagingMode::Continuous;
        return;
    }
    if (accession == cv::PROCESSED)
    {
        meta_.getImzMLMetadata().imagingMode = ImzMLMetadata::ImagingMode::Processed;
        return;
    }

    // ---- IBD checksum ------------------------------------------------
    if (accession == cv::IBD_SHA1)
    {
        meta_.getImzMLMetadata().ibdChecksum     = value;
        meta_.getImzMLMetadata().ibdChecksumType = "SHA-1";
        return;
    }
    if (accession == cv::IBD_MD5)
    {
        meta_.getImzMLMetadata().ibdChecksum     = value;
        meta_.getImzMLMetadata().ibdChecksumType = "MD5";
        return;
    }

    // ---- Scan settings -----------------------------------------------
    if (accession == cv::PIXEL_SIZE_X)
    {
        try { meta_.getImzMLMetadata().pixelSizeX = static_cast<uint32_t>(std::stoul(value)); }
        catch (...) {}
        return;
    }
    if (accession == cv::PIXEL_SIZE_Y)
    {
        try { meta_.getImzMLMetadata().pixelSizeY = static_cast<uint32_t>(std::stoul(value)); }
        catch (...) {}
        return;
    }

    // ---- Pixel coordinates (scan context) ----------------------------
    if (in_scan_ && in_spectrum_)
    {
        if (accession == cv::POSITION_X)
        {
            try
            {
                uint32_t x = static_cast<uint32_t>(std::stoul(value));
                current_spectrum_.setCoordX(x);
                if (x > meta_.getImzMLMetadata().maxX)
                    meta_.getImzMLMetadata().maxX = x;
            }
            catch (...) {}
            return;
        }
        if (accession == cv::POSITION_Y)
        {
            try
            {
                uint32_t y = static_cast<uint32_t>(std::stoul(value));
                current_spectrum_.setCoordY(y);
                if (y > meta_.getImzMLMetadata().maxY)
                    meta_.getImzMLMetadata().maxY = y;
            }
            catch (...) {}
            return;
        }
        if (accession == cv::POSITION_Z)
        {
            try
            {
                uint32_t z = static_cast<uint32_t>(std::stoul(value));
                current_spectrum_.setCoordZ(z);
            }
            catch (...) {}
            return;
        }
    }

    // ---- binaryDataArray context ------------------------------------
    if (in_binary_data_array_)
    {
        if (accession == cv::FLOAT32) { current_array_.dataType = ArrayMeta::Float32; return; }
        if (accession == cv::FLOAT64) { current_array_.dataType = ArrayMeta::Float64; return; }
        if (accession == cv::INT32)   { current_array_.dataType = ArrayMeta::Int32;   return; }
        if (accession == cv::INT64)   { current_array_.dataType = ArrayMeta::Int64;   return; }

        if (accession == cv::MZ_ARRAY)    { current_array_.isMzArray  = true; return; }
        if (accession == cv::INT_ARRAY)   { current_array_.isIntArray = true; return; }

        if (accession == cv::EXTERNAL_DATA)
        {
            current_array_.externalData = true;
            return;
        }
        if (accession == cv::EXTERNAL_OFFSET)
        {
            try { current_array_.externalOffset = std::stoull(value); }
            catch (...) {}
            return;
        }
        if (accession == cv::EXTERNAL_LEN)
        {
            try { current_array_.externalLength = std::stoull(value); }
            catch (...) {}
            return;
        }
        if (accession == cv::EXTERNAL_ENC)
        {
            try { current_array_.externalEncLen = std::stoull(value); }
            catch (...) {}
            return;
        }
    }

    // UUID
    if (accession == cv::UUID)
    {
        if (!value.empty()) meta_.getImzMLMetadata().uuid = value;
        return;
    }

    // Polarity
    if (accession == cv::NEGATIVE_SCAN) { meta_.getImzMLMetadata().polarity = "negative"; return; }
    if (accession == cv::POSITIVE_SCAN) { meta_.getImzMLMetadata().polarity = "positive"; return; }

    // Physical extents
    if (accession == cv::MAX_DIM_X)
    {
        try { meta_.getImzMLMetadata().maxDimX = std::stod(value); } catch (...) {}
        return;
    }
    if (accession == cv::MAX_DIM_Y)
    {
        try { meta_.getImzMLMetadata().maxDimY = std::stod(value); } catch (...) {}
        return;
    }

    // Scan pattern / direction
    if (accession == cv::SCAN_TOP_DOWN)  { meta_.getImzMLMetadata().scanPattern = "top down";    return; }
    if (accession == cv::SCAN_BOTTOM_UP) { meta_.getImzMLMetadata().scanPattern = "bottom up";   return; }
    if (accession == cv::SCAN_FLYBACK)   { meta_.getImzMLMetadata().scanDirection = "flyback";   return; }
    if (accession == cv::SCAN_MEANDER)   { meta_.getImzMLMetadata().scanDirection = "meander";   return; }
    if (accession == cv::SCAN_H_LINE)    { meta_.getImzMLMetadata().scanDirection = "horizontal"; return; }
    if (accession == cv::SCAN_V_LINE)    { meta_.getImzMLMetadata().scanDirection = "vertical";   return; }
    if (accession == cv::SCAN_LR)        { meta_.getImzMLMetadata().lineScanDirection = "left-right"; return; }
    if (accession == cv::SCAN_RL)        { meta_.getImzMLMetadata().lineScanDirection = "right-left"; return; }
}

// ---------------------------------------------------------------------------
// applyRefGroupParams_ — expand a referenceableParamGroupRef
// ---------------------------------------------------------------------------
void ImzMLHandler::applyRefGroupParams_(const std::string& ref_id)
{
    auto it = ref_param_groups_.find(ref_id);
    if (it == ref_param_groups_.end()) return;
    for (const auto& [acc, val] : it->second)
        handleCvParam_(acc, val);
}

// ---------------------------------------------------------------------------
// decodeArray_ — read typed binary data from IBD file
// ---------------------------------------------------------------------------
void ImzMLHandler::decodeArray_(ArrayMeta& a, MSSpectrum& s)
{
    if (!a.externalData || a.externalLength == 0) return;
    if (!openIBD_()) return;

    uint64_t offset = a.externalOffset;
    uint64_t count  = a.externalLength;

    // Seek to offset
    if (fseeko(ibd_file_, static_cast<off_t>(offset), SEEK_SET) != 0) return;

    auto readAs = [&](auto* buf, std::size_t n) -> bool
    {
        return fread(buf, sizeof(*buf), n, ibd_file_) == n;
    };

    switch (a.dataType)
    {
        case ArrayMeta::Float32:
        {
            std::vector<float> tmp(count);
            if (!readAs(tmp.data(), count)) return;
            if (a.isMzArray)
            {
                s.mzArray().resize(count);
                for (std::size_t i = 0; i < count; ++i) s.mzArray()[i] = tmp[i];
            }
            else if (a.isIntArray)
            {
                s.intensityArray().assign(tmp.begin(), tmp.end());
            }
            break;
        }
        case ArrayMeta::Float64:
        {
            if (a.isMzArray)
            {
                s.mzArray().resize(count);
                if (!readAs(s.mzArray().data(), count)) return;
            }
            else if (a.isIntArray)
            {
                std::vector<double> tmp(count);
                if (!readAs(tmp.data(), count)) return;
                s.intensityArray().resize(count);
                for (std::size_t i = 0; i < count; ++i) s.intensityArray()[i] = static_cast<float>(tmp[i]);
            }
            break;
        }
        case ArrayMeta::Int32:
        {
            std::vector<int32_t> tmp(count);
            if (!readAs(tmp.data(), count)) return;
            if (a.isMzArray)
            {
                s.mzArray().resize(count);
                for (std::size_t i = 0; i < count; ++i) s.mzArray()[i] = tmp[i];
            }
            else if (a.isIntArray)
            {
                s.intensityArray().resize(count);
                for (std::size_t i = 0; i < count; ++i) s.intensityArray()[i] = static_cast<float>(tmp[i]);
            }
            break;
        }
        case ArrayMeta::Int64:
        {
            std::vector<int64_t> tmp(count);
            if (!readAs(tmp.data(), count)) return;
            if (a.isMzArray)
            {
                s.mzArray().resize(count);
                for (std::size_t i = 0; i < count; ++i) s.mzArray()[i] = static_cast<double>(tmp[i]);
            }
            else if (a.isIntArray)
            {
                s.intensityArray().resize(count);
                for (std::size_t i = 0; i < count; ++i) s.intensityArray()[i] = static_cast<float>(tmp[i]);
            }
            break;
        }
        default:
            break;
    }
}

// ---------------------------------------------------------------------------
// finaliseSpectrum_
// ---------------------------------------------------------------------------
void ImzMLHandler::finaliseSpectrum_()
{
    // Apply coordinate filter
    if (opts_.hasCoordinateFilter() &&
        !opts_.passesCoordinateFilter(current_spectrum_.getCoordX(),
                                      current_spectrum_.getCoordY(),
                                      current_spectrum_.getCoordZ()))
    {
        return;
    }

    // Check max-spectra limit
    if (opts_.getMaxSpectra() > 0 && spectra_written_ >= opts_.getMaxSpectra())
        return;

    // Zip raw arrays → peaks
    if (!opts_.getSkipIBDDecode())
        current_spectrum_.zipArraysToPeaks();

    // Sort by m/z if requested
    if (opts_.getSortMZ() && !opts_.getSkipIBDDecode())
        current_spectrum_.sortByPosition();

    // Deliver to consumer
    if (consumer_)
    {
        consumer_->consumeSpectrum(std::move(current_spectrum_));
    }
    else
    {
        // No consumer: store in meta_
        meta_.push_back(std::move(current_spectrum_));
    }

    ++spectra_written_;
}

} // namespace Internal
} // namespace OpenMS
