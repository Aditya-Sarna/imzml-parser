// ---------------------------------------------------------------------------
// ImzMLFile.cpp — public API implementation
//
// Single-pass loading strategy (replaces the old two-phase approach):
//
//   loadImzMLFull()  (in OMLoader.cpp, bioconda TU)
//     └── ImzMLHandler : public OpenMS::Internal::MzMLHandler
//           - MzMLHandler base:  RT, MS level, instrument config,
//                                data processing, scan settings, …
//           - ImzMLHandler override: pixel coords (IMS:1000050/51/52),
//                                    external offset/length (IMS:1000102/103),
//                                    imaging mode, checksum, UUID, …
//           - ImzMLInterceptConsumer: IBD binary decode per spectrum
//
// This file has NO Xerces dependency — all XML work is in OMLoader.cpp.
// ---------------------------------------------------------------------------
#include <OpenMS/FORMAT/ImzMLFile.h>
#include <OMLoader.h>   // bridge: loadImzMLFull() (openms_integration/include_bridge)

#include <filesystem>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace OpenMS
{

// ---------------------------------------------------------------------------
// inferIbdPath_
// ---------------------------------------------------------------------------
std::string ImzMLFile::inferIbdPath_(const std::string& imzml_path)
{
    namespace fs = std::filesystem;
    fs::path p(imzml_path);
    fs::path ibd_exact = p.parent_path() / (p.stem().string() + ".ibd");
    if (fs::exists(ibd_exact)) return ibd_exact.string();

    // Case-insensitive fallback: scan directory for *.ibd with matching stem
    std::string stem_lower = p.stem().string();
    std::transform(stem_lower.begin(), stem_lower.end(), stem_lower.begin(),
                   [](unsigned char c){ return std::tolower(c); });

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(p.parent_path(), ec))
    {
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (ext != ".ibd") continue;

        std::string fstem = entry.path().stem().string();
        std::transform(fstem.begin(), fstem.end(), fstem.begin(),
                       [](unsigned char c){ return std::tolower(c); });
        if (fstem == stem_lower) return entry.path().string();
    }
    return {};
}

// ---------------------------------------------------------------------------
// loadImpl_  — core logic
// ---------------------------------------------------------------------------
void ImzMLFile::loadImpl_(const std::string&     imzml_path,
                           IMSDataConsumer*        consumer,
                           MSExperiment&           meta_holder,
                           const PeakFileOptions& opts)
{
    // Deduce IBD path
    const std::string ibd_path = inferIbdPath_(imzml_path);
    if (ibd_path.empty() && !opts.getSkipIBDDecode())
    {
        throw std::runtime_error(
            "IBD binary data file (.ibd) not found. "
            "Ensure the .ibd file is in the same directory as the .imzML "
            "file and that both share the same base name.");
    }
    meta_holder.getImzMLMetadata().ibdFilePath = ibd_path;

    // ------------------------------------------------------------------
    // Build the spectrum callback.
    // Each OMSpectrumFull (from the MzMLHandler-based single-pass parse)
    // is converted to our local MSSpectrum and delivered to the consumer
    // or accumulated in meta_holder.
    // ------------------------------------------------------------------
    int spectrum_idx = 0;
    const bool skip_decode = opts.getSkipIBDDecode();

    const Internal::OMSpectrumCallback callback =
        [&](Internal::OMSpectrumFull sf)
    {
        MSSpectrum s;
        s.setRT(sf.rt);
        s.setMSLevel(sf.msLevel);
        s.setCoordX(sf.x);
        s.setCoordY(sf.y);
        s.setCoordZ(sf.z);

        // Apply pixel coordinate filter early (before IBD decode)
        if (opts.hasCoordinateFilter() &&
            !opts.passesCoordinateFilter(sf.x, sf.y, sf.z))
            return;

        if (!skip_decode)
        {
            const std::size_t n = std::min(sf.mz.size(), sf.intensity.size());
            s.resize(n);
            for (std::size_t i = 0; i < n; ++i)
            {
                s[i].setMZ(sf.mz[i]);
                s[i].setIntensity(sf.intensity[i]);
            }
            if (opts.getSortMZ()) s.sortByPosition();
        }

        if (consumer) consumer->consumeSpectrum(std::move(s));
        else          meta_holder.push_back(std::move(s));
        ++spectrum_idx;
    };

    // ------------------------------------------------------------------
    // Single-pass parse (MzMLHandler base + IMS override + IBD decode).
    // Passing empty ibdPath when skip_decode is true prevents IBD open.
    // ------------------------------------------------------------------
    const std::string parse_ibd = skip_decode ? std::string{} : ibd_path;
    Internal::OMMeta meta = Internal::loadImzMLFull(imzml_path, parse_ibd, callback);

    if (!meta.ok)
        throw std::runtime_error(meta.error);

    // ------------------------------------------------------------------
    // Populate experiment-level ImzMLMetadata from parse result
    // ------------------------------------------------------------------
    auto& m = meta_holder.getImzMLMetadata();
    m.ibdFilePath       = ibd_path;
    m.maxX              = meta.maxX;
    m.maxY              = meta.maxY;
    m.maxZ              = meta.maxZ;
    m.pixelSizeX        = meta.pixelSizeX  ? meta.pixelSizeX  : 1;
    m.pixelSizeY        = meta.pixelSizeY  ? meta.pixelSizeY  : 1;
    m.maxDimX           = meta.maxDimX;
    m.maxDimY           = meta.maxDimY;
    m.ibdChecksum       = meta.ibdChecksum;
    m.ibdChecksumType   = meta.ibdChecksumType;
    m.uuid              = meta.uuid;
    m.polarity          = meta.polarity.empty() ? "unknown" : meta.polarity;
    m.mzDataType        = meta.mzDataType.empty()  ? "unknown" : meta.mzDataType;
    m.intDataType       = meta.intDataType.empty() ? "unknown" : meta.intDataType;
    m.scanPattern       = meta.scanPattern;
    m.scanDirection     = meta.scanDirection;
    m.lineScanDirection = meta.lineScanDirection;
    m.schemaVersion     = meta.schemaVersion;

    if      (meta.mode == "continuous") m.imagingMode = ImzMLMetadata::ImagingMode::Continuous;
    else if (meta.mode == "processed")  m.imagingMode = ImzMLMetadata::ImagingMode::Processed;
    else                                m.imagingMode = ImzMLMetadata::ImagingMode::Unknown;


}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
void ImzMLFile::load(const std::string&     imzml_path,
                      MSExperiment&          exp,
                      const PeakFileOptions& opts)
{
    exp.clear();
    SimpleCollectingConsumer collector(exp);
    loadImpl_(imzml_path, &collector, exp, opts);
}

void ImzMLFile::load(const std::string&     imzml_path,
                      IMSDataConsumer&        consumer,
                      const PeakFileOptions& opts)
{
    MSExperiment meta;
    loadImpl_(imzml_path, &consumer, meta, opts);
}

void ImzMLFile::loadMetadata(const std::string& imzml_path, MSExperiment& exp)
{
    PeakFileOptions opts;
    opts.setSkipIBDDecode(true);
    exp.clear();
    SimpleCollectingConsumer collector(exp);
    loadImpl_(imzml_path, &collector, exp, opts);
}

bool ImzMLFile::validate(const std::string& imzml_path,
                          std::vector<std::string>& errors) const
{
    errors.clear();
    try
    {
        MSExperiment exp;
        PeakFileOptions opts;
        opts.setSkipIBDDecode(false);
        SimpleCollectingConsumer c(exp);
        const_cast<ImzMLFile*>(this)->loadImpl_(imzml_path, &c, exp, opts);
    }
    catch (const std::exception& e)
    {
        errors.push_back(e.what());
        return false;
    }
    return errors.empty();
}

} // namespace OpenMS