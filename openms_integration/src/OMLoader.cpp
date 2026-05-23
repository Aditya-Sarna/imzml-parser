// ---------------------------------------------------------------------------
// OMLoader.cpp — real OpenMS::MzMLFile bridge
//
// Compiled with bioconda OpenMS 3.5.0 include paths (C++20).
// MUST NOT include Homebrew Xerces headers (would clash with bioconda
// Xerces 3.2 which OpenMS headers pull in transitively).
//
// This TU is compiled as a separate CMake OBJECT library (openms_loader_obj)
// so that its bioconda-specific include directories are isolated from the
// rest of the openms_imzml library (which uses Homebrew Xerces 3.3).
// ---------------------------------------------------------------------------

// bioconda OpenMS 3.5.0 headers
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/FORMAT/HANDLERS/MzMLHandler.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/KERNEL/MSSpectrum.h>
#include <OpenMS/DATASTRUCTURES/String.h>
#include <OpenMS/CONCEPT/Exception.h>
#include <OpenMS/CONCEPT/LogStream.h>  // LogStream + OpenMS_Log_warn
#include <OpenMS/INTERFACES/IMSDataConsumer.h>
#include <OpenMS/FORMAT/OPTIONS/PeakFileOptions.h>

// Xerces-C (bioconda Xerces 3.2 — pulled in via OpenMS headers)
#include <xercesc/sax2/SAX2XMLReader.hpp>
#include <xercesc/sax2/XMLReaderFactory.hpp>
#include <xercesc/framework/MemBufInputSource.hpp>
#include <xercesc/util/PlatformUtils.hpp>
#include <xercesc/util/XMLString.hpp>
#include <xercesc/util/XMLException.hpp>
#include <xercesc/sax/SAXException.hpp>

// Our bridge header (plain types only — no Xerces)
#include <OMLoader.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <algorithm>

namespace OpenMS
{
namespace Internal
{

OMLoadResult loadWithRealOpenMS(const std::string& path)
{
    OMLoadResult result;
    result.ok = false;

#ifdef __linux__
    // On Linux, OpenMS::MzMLFile::load() segfaults when parsing imzML files
    // due to OpenMS global singleton initialization differences between
    // macOS (two-level namespace dylib) and Linux (flat ELF symbol space).
    // Phase 2 (Xerces SAX2 ImzMLHandler) is sufficient for all functionality:
    // pixel coords, binary data, imaging mode — everything the viewer needs.
    // RT/msLevel enrichment from Phase 1 is skipped gracefully.
    result.ok = false;
    result.error = "Phase 1 (OpenMS::MzMLFile) skipped on Linux — using SAX-only path";
    return result;
#endif

    // OpenMS::MzMLFile uses Xerces LocalFileInputSource internally, which
    // treats its argument as a URI.  Paths with spaces or other URI-special
    // characters cause it to fail with "unable to open primary document
    // entity".  Skip Phase 1 for such paths; Phase 2 (SAX ImzMLHandler)
    // provides all imaging-critical data (pixel coords, binary offsets, mode).
    for (char c : path) {
        if (c == ' ' || c == '#' || c == '%' || c == '?' || c == '&' || c == '+') {
            result.ok = false;
            result.error = "Phase 1 skipped — path contains URI-special characters; using SAX-only path";
            return result;
        }
    }

    try
    {
        // ----------------------------------------------------------------
        // Call the real OpenMS mzML parser.
        // For imzML files:
        //   - All standard mzML metadata is parsed (RT, TIC, scan metadata)
        //   - Peak arrays are empty (external binary data not decoded —
        //     that is the job of ImzMLHandler + IBDReader in the second pass)
        //   - IMS-specific CV params (IMS:1000050 position x/y, external
        //     offset / length) are not preserved by MzMLFile because the IMS
        //     ontology is unknown to OpenMS — those are extracted by the
        //     ImzMLHandler SAX pass.
        //
        // Silence OpenMS warnings during load:
        //   OpenMS prints O(n_spectra) "Float binary data array ... length 0"
        //   warnings because imzML <binary/> tags are empty (all data is in
        //   the external .ibd file).  Redirect OpenMS_Log_warn to a null sink.
        // ----------------------------------------------------------------
        ::OpenMS::MzMLFile mf;
        ::OpenMS::PeakMap  exp;

        // Silence OpenMS progress logging
        mf.setLogType(::OpenMS::ProgressLogger::NONE);

        // Temporarily silence OpenMS warning and error log streams.
        // OpenMS prints O(n_spectra) "Float binary data array ... length 0"
        // warnings for imzML files because <binary/> elements are empty
        // (all binary data lives in the external .ibd file).
        // We silence by setting level to FATAL and removing the default ostream.
        OpenMS::OpenMS_Log_warn.setLevel("FATAL");
        OpenMS::OpenMS_Log_warn.removeAllStreams();
        OpenMS::OpenMS_Log_error.setLevel("FATAL");
        OpenMS::OpenMS_Log_error.removeAllStreams();

        mf.load(path, exp);

        // Restore log streams to defaults (cerr for error, cout for warn)
        OpenMS::OpenMS_Log_warn.setLevel("WARN");
        OpenMS::OpenMS_Log_warn.insert(std::cerr);
        OpenMS::OpenMS_Log_error.setLevel("ERROR");
        OpenMS::OpenMS_Log_error.insert(std::cerr);

        const size_t n = exp.getNrSpectra();
        result.count = n;
        result.spectra.reserve(n);

        for (size_t i = 0; i < n; ++i)
        {
            const ::OpenMS::MSSpectrum& s = exp[i];
            OMSpectrumInfo info;
            info.rt      = s.getRT();
            info.msLevel = s.getMSLevel();

            if (s.metaValueExists("total ion current"))
            {
                info.tic = static_cast<float>(
                    static_cast<double>(
                        s.getMetaValue("total ion current")));
            }
            result.spectra.push_back(info);
        }

        result.ok = true;
    }
    catch (const ::OpenMS::Exception::BaseException& e)
    {
        result.error = std::string("OpenMS exception: ") + e.what();
    }
    catch (const std::exception& e)
    {
        result.error = std::string("std::exception: ") + e.what();
    }
    catch (...)
    {
        result.error = "Unknown exception in loadWithRealOpenMS()";
    }

    return result;
}

} // namespace Internal
} // namespace OpenMS

// ===========================================================================
// Single-pass imzML parser: ImzMLHandler (inherits MzMLHandler) +
// ImzMLInterceptConsumer + loadImzMLFull()
//
// All in this TU so it compiles against bioconda Xerces 3.2 / OpenMS 3.5.
// ===========================================================================

namespace
{

// Bring bridge types into anonymous namespace scope
using ::OpenMS::Internal::OMMeta;
using ::OpenMS::Internal::OMSpectrumFull;
using ::OpenMS::Internal::OMSpectrumCallback;

// ---------------------------------------------------------------------------
// Per-binaryDataArray metadata captured from IMS CV params
// ---------------------------------------------------------------------------
struct ArrayMeta
{
    enum DataType { FLOAT32, FLOAT64, INT32, INT64, UNKNOWN } dt { UNKNOWN };
    bool     isMz      { false };
    bool     isInt     { false };
    bool     isExternal{ false };
    uint64_t offset    { 0 };
    uint64_t count     { 0 };    // element count (NOT byte count)

    void reset() { *this = ArrayMeta{}; }
};

// Forward declare so ImzMLHandler can friend it
class ImzMLInterceptConsumer;

// ---------------------------------------------------------------------------
// ImzMLHandler
//
// Inherits OpenMS::Internal::MzMLHandler to get all standard mzML parsing
// for free (RT, MS level, instrument config, data processing, scan settings…).
//
// Overrides startElement / endElement to intercept IMS:* CV params:
//   - pixel coordinates  (IMS:1000050/51/52)
//   - external offset + count  (IMS:1000102/103)
//   - imaging mode, checksum, UUID, scan pattern, etc.
//
// The IBD binary decode is done in ImzMLInterceptConsumer::consumeSpectrum,
// which is called by MzMLHandler when it finalises each </spectrum>.
// ---------------------------------------------------------------------------
class ImzMLHandler final : public ::OpenMS::Internal::MzMLHandler
{
    friend class ImzMLInterceptConsumer;

public:
    ImzMLHandler(::OpenMS::PeakMap&               exp,
                 const std::string&                filename,
                 const ::OpenMS::ProgressLogger&   logger)
        : ::OpenMS::Internal::MzMLHandler(exp, filename, "1.1.0", logger)
    {}

    ~ImzMLHandler() override
    {
        if (ibd_) { fclose(ibd_); ibd_ = nullptr; }
    }

    bool openIBD(const std::string& path)
    {
        ibd_ = fopen(path.c_str(), "rb");
        return ibd_ != nullptr;
    }

    ::OpenMS::Internal::OMMeta& metaOut() { return meta_out_; }

    // ---------------------------------------------------------------
    // SAX2 overrides
    // ---------------------------------------------------------------
    void startElement(const XMLCh* uri, const XMLCh* localname,
                      const XMLCh* qname,
                      const xercesc::Attributes& attrs) override
    {
        const std::string tag = sm_.convert(qname);

        // --- Track IMS state BEFORE calling base ---
        if (tag == "spectrum")
        {
            in_spectrum_  = true;
            cur_x_ = cur_y_ = cur_z_ = 0;
            cur_mz_meta_.reset();
            cur_int_meta_.reset();
        }
        if (tag == "scan"            && in_spectrum_) in_scan_ = true;
        if (tag == "binaryDataArray" && in_spectrum_)
        {
            in_bda_ = true;
            cur_array_.reset();
        }
        if (tag == "referenceableParamGroup")
        {
            in_ref_group_ = true;
            ::OpenMS::String id_str_om;
            optionalAttributeAsString_(id_str_om, attrs, "id");
            cur_ref_id_ = static_cast<std::string>(id_str_om);
            ref_groups_[cur_ref_id_]; // ensure entry exists
        }
        if (tag == "referenceableParamGroupRef")
        {
            ::OpenMS::String ref_id_om;
            optionalAttributeAsString_(ref_id_om, attrs, "ref");
            applyRefGroup_(static_cast<std::string>(ref_id_om));
        }

        // Intercept <cvParam> for IMS:* terms before base sees them
        if (tag == "cvParam")
        {
            ::OpenMS::String acc_om, val_om;
            optionalAttributeAsString_(acc_om, attrs, "accession");
            optionalAttributeAsString_(val_om, attrs, "value");
            std::string acc(acc_om), val(val_om);

            if (in_ref_group_)
                ref_groups_[cur_ref_id_].push_back({acc, val});
            else
                handleIMSCvParam_(acc, val);
            // Fall through: also let MzMLHandler base process it
            // (it knows MS:* terms; it silently ignores IMS:* terms)
        }

        // Let MzMLHandler handle everything (instrument, RT, MS level, …)
        ::OpenMS::Internal::MzMLHandler::startElement(uri, localname, qname, attrs);
    }

    void endElement(const XMLCh* uri, const XMLCh* localname,
                    const XMLCh* qname) override
    {
        const std::string tag = sm_.convert(qname);

        if (tag == "binaryDataArray" && in_spectrum_)
        {
            // Commit the IMS array metadata we captured from cvParams
            if      (cur_array_.isMz)  cur_mz_meta_  = cur_array_;
            else if (cur_array_.isInt) cur_int_meta_ = cur_array_;
            cur_array_.reset();
            in_bda_ = false;
        }
        if (tag == "scan")    in_scan_     = false;
        if (tag == "spectrum")
        {
            // Commit per-spectrum IMS metadata BEFORE resetting cur_* fields
            // (MzMLHandler batches delivery: consumeSpectrum fires after full
            // spectrumList, so cur_x/y/z would be 0 by then without this store.)
            SpecIMS ims;
            ims.x        = cur_x_;
            ims.y        = cur_y_;
            ims.z        = cur_z_;
            ims.mz_meta  = cur_mz_meta_;
            ims.int_meta = cur_int_meta_;
            spec_ims_.push_back(ims);

            in_spectrum_ = false;
        }
        if (tag == "referenceableParamGroup")
        {
            in_ref_group_ = false;
            cur_ref_id_.clear();
        }

        // Let MzMLHandler do its thing (delivers spectrum to consumer
        // via consumeSpectrum after the full spectrumList is parsed)
        ::OpenMS::Internal::MzMLHandler::endElement(uri, localname, qname);

        if (tag == "spectrum")
        {
            // Reset IMS state for next spectrum
            cur_x_ = cur_y_ = cur_z_ = 0;
            cur_mz_meta_.reset();
            cur_int_meta_.reset();
        }
    }

private:
    // ---------------------------------------------------------------
    // IMS CV param dispatch
    // ---------------------------------------------------------------
    void handleIMSCvParam_(const std::string& acc, const std::string& val)
    {
        // -- Pixel coordinates (inside <scan> inside <spectrum>) --
        if (in_scan_ && in_spectrum_)
        {
            if (acc == "IMS:1000050") { try { cur_x_ = (uint32_t)std::stoul(val); } catch (...) {} return; }
            if (acc == "IMS:1000051") { try { cur_y_ = (uint32_t)std::stoul(val); } catch (...) {} return; }
            if (acc == "IMS:1000052") { try { cur_z_ = (uint32_t)std::stoul(val); } catch (...) {} return; }
        }

        // -- binaryDataArray context --
        if (in_bda_)
        {
            if (acc == "MS:1000521") { cur_array_.dt = ArrayMeta::FLOAT32;     return; }
            if (acc == "MS:1000523") { cur_array_.dt = ArrayMeta::FLOAT64;     return; }
            if (acc == "MS:1000519") { cur_array_.dt = ArrayMeta::INT32;       return; }
            if (acc == "MS:1000522") { cur_array_.dt = ArrayMeta::INT64;       return; }
            if (acc == "MS:1000514") { cur_array_.isMz      = true;            return; }
            if (acc == "MS:1000515") { cur_array_.isInt     = true;            return; }
            if (acc == "IMS:1000101"){ cur_array_.isExternal= true;            return; }
            if (acc == "IMS:1000102"){ try { cur_array_.offset = std::stoull(val); } catch (...) {} return; }
            if (acc == "IMS:1000103"){ try { cur_array_.count  = std::stoull(val); } catch (...) {} return; }
        }

        // -- Dataset-level --
        if (acc == "IMS:1000030") { meta_out_.mode = "continuous";  return; }
        if (acc == "IMS:1000031") { meta_out_.mode = "processed";   return; }
        if (acc == "IMS:1000091") { meta_out_.ibdChecksum = val; meta_out_.ibdChecksumType = "SHA-1"; return; }
        if (acc == "IMS:1000090") { meta_out_.ibdChecksum = val; meta_out_.ibdChecksumType = "MD5";   return; }
        if (acc == "IMS:1000046") { try { meta_out_.pixelSizeX = (uint32_t)std::stoul(val); } catch (...) {} return; }
        if (acc == "IMS:1000047") { try { meta_out_.pixelSizeY = (uint32_t)std::stoul(val); } catch (...) {} return; }
        if (acc == "IMS:1000080") { if (!val.empty()) meta_out_.uuid    = val; return; }
        if (acc == "IMS:1000044") { try { meta_out_.maxDimX = std::stod(val); } catch (...) {} return; }
        if (acc == "IMS:1000045") { try { meta_out_.maxDimY = std::stod(val); } catch (...) {} return; }
        if (acc == "MS:1000129") { meta_out_.polarity = "negative"; return; }
        if (acc == "MS:1000130") { meta_out_.polarity = "positive"; return; }
        if (acc == "IMS:1000401") { meta_out_.scanPattern      = "top down";    return; }
        if (acc == "IMS:1000402") { meta_out_.scanPattern      = "bottom up";   return; }
        if (acc == "IMS:1000413") { meta_out_.scanDirection    = "flyback";     return; }
        if (acc == "IMS:1000412") { meta_out_.scanDirection    = "meander";     return; }
        if (acc == "IMS:1000480") { meta_out_.scanDirection    = "horizontal";  return; }
        if (acc == "IMS:1000481") { meta_out_.scanDirection    = "vertical";    return; }
        if (acc == "IMS:1000491") { meta_out_.lineScanDirection = "left-right"; return; }
        if (acc == "IMS:1000492") { meta_out_.lineScanDirection = "right-left"; return; }
    }

    void applyRefGroup_(const std::string& id)
    {
        auto it = ref_groups_.find(id);
        if (it == ref_groups_.end()) return;
        for (const auto& kv : it->second)
            handleIMSCvParam_(kv.first, kv.second);
    }

    // ---------------------------------------------------------------
    // State (accessible by ImzMLInterceptConsumer via friend)
    // ---------------------------------------------------------------
    FILE*       ibd_          { nullptr };
    OMMeta      meta_out_;

    // per-spectrum
    bool        in_spectrum_  { false };
    bool        in_scan_      { false };
    bool        in_bda_       { false };
    uint32_t    cur_x_        { 0 };
    uint32_t    cur_y_        { 0 };
    uint32_t    cur_z_        { 0 };
    ArrayMeta   cur_array_;
    ArrayMeta   cur_mz_meta_;
    ArrayMeta   cur_int_meta_;

    // Per-spectrum IMS metadata stored at endElement("spectrum"), indexed by
    // spectrum order.  Because MzMLHandler batches spectrum delivery (calls
    // consumeSpectrum after the full spectrumList is parsed), we cannot rely
    // on live cur_x_/y_/z_ in consumeSpectrum — they will have been reset.
    struct SpecIMS {
        uint32_t  x {0}, y {0}, z {1};
        ArrayMeta mz_meta;
        ArrayMeta int_meta;
    };
    std::vector<SpecIMS> spec_ims_;  // one entry per spectrum, in XML order

    // referenceableParamGroup tracking
    using CvPair = std::pair<std::string, std::string>;
    std::unordered_map<std::string, std::vector<CvPair>> ref_groups_;
    std::string cur_ref_id_;
    bool        in_ref_group_ { false };
};

// ---------------------------------------------------------------------------
// ImzMLInterceptConsumer
//
// Implements bioconda IMSDataConsumer.  Sits between MzMLHandler and the
// user callback.  When MzMLHandler finalises a spectrum:
//   1. RT and MS level come from the bioconda MSSpectrum (parsed by base).
//   2. Pixel coords come from ImzMLHandler's tracked IMS state.
//   3. Peak arrays are decoded from the .ibd file using the IMS offsets.
//   4. Everything is bundled into OMSpectrumFull and sent to the callback.
// ---------------------------------------------------------------------------
class ImzMLInterceptConsumer final : public ::OpenMS::Interfaces::IMSDataConsumer
{
public:
    ImzMLInterceptConsumer(ImzMLHandler&                              handler,
                           const ::OpenMS::Internal::OMSpectrumCallback& callback)
        : handler_(handler), callback_(callback)
    {}

    void setExpectedSize(size_t, size_t) override {}

    void setExperimentalSettings(const ::OpenMS::ExperimentalSettings&) override {}

    void consumeSpectrum(::OpenMS::MSSpectrum& s) override
    {
        // MzMLHandler batches spectrum delivery — consumeSpectrum fires after
        // the entire spectrumList is parsed.  Per-spectrum IMS metadata was
        // stored in handler_.spec_ims_ in document order; use spec_idx_ to
        // retrieve the correct entry.
        const int idx = spec_idx_++;
        const ImzMLHandler::SpecIMS* ims = nullptr;
        if (idx < (int)handler_.spec_ims_.size())
            ims = &handler_.spec_ims_[idx];

        ::OpenMS::Internal::OMSpectrumFull out;
        out.rt      = s.getRT();
        out.msLevel = static_cast<int>(s.getMSLevel());
        out.index   = idx;
        out.x       = ims ? ims->x : 0;
        out.y       = ims ? ims->y : 0;
        out.z       = ims ? ims->z : 1;

        // Update max grid dimensions
        auto& m = handler_.meta_out_;
        if (out.x > m.maxX) m.maxX = out.x;
        if (out.y > m.maxY) m.maxY = out.y;
        if (out.z > m.maxZ) m.maxZ = out.z;

        // Decode peak arrays from .ibd
        if (handler_.ibd_ && ims)
        {
            decodeMz (ims->mz_meta,  out.mz);
            decodeInt(ims->int_meta, out.intensity);

            // Record first-seen data types in metadata
            if (m.mzDataType.empty())  m.mzDataType  = dtStr(ims->mz_meta.dt);
            if (m.intDataType.empty()) m.intDataType = dtStr(ims->int_meta.dt);
        }

        callback_(std::move(out));
    }

    void consumeChromatogram(::OpenMS::MSChromatogram&) override {}

private:
    ImzMLHandler&                                 handler_;
    const ::OpenMS::Internal::OMSpectrumCallback& callback_;
    int                                           spec_idx_ { 0 };

    static std::string dtStr(ArrayMeta::DataType dt)
    {
        switch (dt) {
            case ArrayMeta::FLOAT32: return "float32";
            case ArrayMeta::FLOAT64: return "float64";
            case ArrayMeta::INT32:   return "int32";
            case ArrayMeta::INT64:   return "int64";
            default:                 return "unknown";
        }
    }

    void decodeMz(const ArrayMeta& m, std::vector<double>& out)
    {
        if (!m.isExternal || m.count == 0) return;
        if (fseeko(handler_.ibd_, (off_t)m.offset, SEEK_SET) != 0) return;
        out.resize(m.count);
        switch (m.dt) {
            case ArrayMeta::FLOAT32: {
                std::vector<float> tmp(m.count);
                if (fread(tmp.data(), 4, m.count, handler_.ibd_) == m.count)
                    for (size_t i = 0; i < m.count; ++i) out[i] = tmp[i];
                break;
            }
            case ArrayMeta::FLOAT64:
                if (fread(out.data(), 8, m.count, handler_.ibd_) != m.count) out.clear();
                break;
            case ArrayMeta::INT32: {
                std::vector<int32_t> tmp(m.count);
                if (fread(tmp.data(), 4, m.count, handler_.ibd_) == m.count)
                    for (size_t i = 0; i < m.count; ++i) out[i] = tmp[i];
                break;
            }
            case ArrayMeta::INT64: {
                std::vector<int64_t> tmp(m.count);
                if (fread(tmp.data(), 8, m.count, handler_.ibd_) == m.count)
                    for (size_t i = 0; i < m.count; ++i) out[i] = (double)tmp[i];
                break;
            }
            default: out.clear(); break;
        }
    }

    void decodeInt(const ArrayMeta& m, std::vector<float>& out)
    {
        if (!m.isExternal || m.count == 0) return;
        if (fseeko(handler_.ibd_, (off_t)m.offset, SEEK_SET) != 0) return;
        out.resize(m.count);
        switch (m.dt) {
            case ArrayMeta::FLOAT32:
                if (fread(out.data(), 4, m.count, handler_.ibd_) != m.count) out.clear();
                break;
            case ArrayMeta::FLOAT64: {
                std::vector<double> tmp(m.count);
                if (fread(tmp.data(), 8, m.count, handler_.ibd_) == m.count)
                    for (size_t i = 0; i < m.count; ++i) out[i] = (float)tmp[i];
                break;
            }
            case ArrayMeta::INT32: {
                std::vector<int32_t> tmp(m.count);
                if (fread(tmp.data(), 4, m.count, handler_.ibd_) == m.count)
                    for (size_t i = 0; i < m.count; ++i) out[i] = (float)tmp[i];
                break;
            }
            case ArrayMeta::INT64: {
                std::vector<int64_t> tmp(m.count);
                if (fread(tmp.data(), 8, m.count, handler_.ibd_) == m.count)
                    for (size_t i = 0; i < m.count; ++i) out[i] = (float)tmp[i];
                break;
            }
            default: out.clear(); break;
        }
    }
};

} // anonymous namespace

// ---------------------------------------------------------------------------
// loadImzMLFull — public API (declared in OMLoader.h)
// ---------------------------------------------------------------------------
namespace OpenMS
{
namespace Internal
{

OMMeta loadImzMLFull(const std::string&        imzmlPath,
                     const std::string&        ibdPath,
                     const OMSpectrumCallback& callback)
{
    OMMeta result;

    // Xerces initialisation (idempotent after first call)
    try { xercesc::XMLPlatformUtils::Initialize(); }
    catch (const xercesc::XMLException& e) {
        result.error = std::string("Xerces init failed: ") +
                       xercesc::XMLString::transcode(e.getMessage());
        return result;
    }

    // Silence OpenMS warnings (O(n) "Float binary data array … length 0")
    ::OpenMS::OpenMS_Log_warn.setLevel("FATAL");
    ::OpenMS::OpenMS_Log_warn.removeAllStreams();
    ::OpenMS::OpenMS_Log_error.setLevel("FATAL");
    ::OpenMS::OpenMS_Log_error.removeAllStreams();

    ::OpenMS::PeakMap        exp;
    ::OpenMS::ProgressLogger logger;
    logger.setLogType(::OpenMS::ProgressLogger::NONE);

    ImzMLHandler handler(exp, imzmlPath, logger);

    if (!ibdPath.empty())
    {
        if (!handler.openIBD(ibdPath)) {
            result.error = "Cannot open IBD file: " + ibdPath;
            // Restore log streams before returning
            ::OpenMS::OpenMS_Log_warn.setLevel("WARN");
            ::OpenMS::OpenMS_Log_warn.insert(std::cerr);
            ::OpenMS::OpenMS_Log_error.setLevel("ERROR");
            ::OpenMS::OpenMS_Log_error.insert(std::cerr);
            return result;
        }
    }
    handler.metaOut().ibdFilePath = ibdPath;

    ImzMLInterceptConsumer consumer(handler, callback);
    handler.setMSDataConsumer(&consumer);

    // Read file into memory (avoids Xerces URI / space handling issues)
    std::vector<char> xmlbuf;
    {
        FILE* f = fopen(imzmlPath.c_str(), "rb");
        if (!f) {
            result.error = "Cannot open imzML file: " + imzmlPath;
            return result;
        }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (fsz > 0) { xmlbuf.resize((size_t)fsz); fread(xmlbuf.data(), 1, (size_t)fsz, f); }
        fclose(f);
    }

    // Create SAX2 reader
    std::unique_ptr<xercesc::SAX2XMLReader> reader{
        xercesc::XMLReaderFactory::createXMLReader()};
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreValidation,              false);
    reader->setFeature(xercesc::XMLUni::fgSAX2CoreNameSpaces,              true);
    reader->setFeature(xercesc::XMLUni::fgXercesLoadExternalDTD,           false);
    reader->setFeature(xercesc::XMLUni::fgXercesDisableDefaultEntityResolution, true);
    reader->setFeature(xercesc::XMLUni::fgXercesSchema,                    false);
    reader->setFeature(xercesc::XMLUni::fgXercesSchemaFullChecking,        false);
    reader->setContentHandler(&handler);
    reader->setErrorHandler(&handler);

    xercesc::MemBufInputSource src(
        reinterpret_cast<const XMLByte*>(xmlbuf.data()),
        (XMLSize_t)xmlbuf.size(),
        imzmlPath.c_str());

    try {
        reader->parse(src);
    }
    catch (const ::OpenMS::Internal::XMLHandler::EndParsingSoftly&) {
        // Intentional early exit — not an error
    }
    catch (const xercesc::XMLException& e) {
        result.error = std::string("XML error: ") + xercesc::XMLString::transcode(e.getMessage());
        return result;
    }
    catch (const xercesc::SAXException& e) {
        result.error = std::string("SAX error: ") + xercesc::XMLString::transcode(e.getMessage());
        return result;
    }
    catch (const std::exception& e) {
        result.error = e.what();
        return result;
    }

    // Restore log streams
    ::OpenMS::OpenMS_Log_warn.setLevel("WARN");
    ::OpenMS::OpenMS_Log_warn.insert(std::cerr);
    ::OpenMS::OpenMS_Log_error.setLevel("ERROR");
    ::OpenMS::OpenMS_Log_error.insert(std::cerr);

    result = handler.metaOut();
    result.ok = true;
    return result;
}

} // namespace Internal
} // namespace OpenMS
