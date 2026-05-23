// ---------------------------------------------------------------------------
// ImzMLWriter.cpp
// Writes imaging mass spectrometry data as imzML 1.1.0 (.imzML + .ibd).
//
// Binary layout in the .ibd file:
//   Continuous mode:
//     [shared m/z array][intensity_0][intensity_1]...[intensity_n]
//   Processed mode:
//     [mz_0][int_0][mz_1][int_1]...[mz_n][int_n]
//
// The .imzML XML is buffered in memory (via SpecEntry records) and flushed
// to disk only in close(), after all spectra have been written to the .ibd.
// This two-phase approach allows correct <scanSettings> counts (maxX, maxY)
// and avoids seeking backwards in the XML stream.
// ---------------------------------------------------------------------------
#include "imzml/ImzMLWriter.h"

#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>
#include <stdexcept>

namespace imzml
{

// ---------------------------------------------------------------------------
// Template helpers: write a scalar value little-endian to an ostream
// ---------------------------------------------------------------------------
namespace
{

template<typename T>
void writeLE(std::ostream& out, T val)
{
    // On all real-world platforms targeted here (x86, x86-64, ARM64) the
    // native byte order is already little-endian, so a direct write works.
    // For a future big-endian port, byteswap here.
    out.write(reinterpret_cast<const char*>(&val), sizeof(T));
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

ImzMLWriter::ImzMLWriter(const std::string& imzmlPath, const Options& opts)
{
    open(imzmlPath, opts);
}

ImzMLWriter::~ImzMLWriter()
{
    if (isOpen())
    {
        try { close(); }
        catch (...) {} // swallow – we're in a destructor
    }
}

// ---------------------------------------------------------------------------
// open
// ---------------------------------------------------------------------------
void ImzMLWriter::open(const std::string& imzmlPath, const Options& opts)
{
    if (isOpen())
        throw std::runtime_error("ImzMLWriter::open: writer is already open");

    namespace fs = std::filesystem;

    opts_      = opts;
    imzmlPath_ = imzmlPath;

    // Derive .ibd path (same stem, .ibd extension)
    fs::path p(imzmlPath);
    ibdPath_ = (p.parent_path() / (p.stem().string() + ".ibd")).string();

    // Create parent directories if needed
    if (p.has_parent_path() && !p.parent_path().empty())
        fs::create_directories(p.parent_path());

    // Open .ibd for writing
    ibdStream_.open(ibdPath_, std::ios::binary | std::ios::trunc);
    if (!ibdStream_)
        throw std::runtime_error("ImzMLWriter::open: cannot create IBD file: " + ibdPath_);

    // Generate UUID if not provided
    if (opts_.uuid.empty())
        opts_.uuid = makeUuid_();

    // Reset state
    entries_.clear();
    sharedMzWritten_ = false;
    sharedMzOffset_  = 0;
    sharedMzLength_  = 0;
}

// ---------------------------------------------------------------------------
// addSpectrum
// ---------------------------------------------------------------------------
void ImzMLWriter::addSpectrum(const Coordinate&          coord,
                               const std::vector<double>& mz,
                               const std::vector<double>& intensity)
{
    if (!isOpen())
        throw std::runtime_error("ImzMLWriter::addSpectrum: writer is not open");

    if (mz.size() != intensity.size())
        throw std::invalid_argument("ImzMLWriter::addSpectrum: mz and intensity must have the same length");

    SpecEntry e;
    e.coord = coord;

    if (opts_.mode == ImagingMode::Continuous)
    {
        // ----------------------------------------------------------------
        // Continuous mode: shared m/z array, then per-spectrum intensity
        // ----------------------------------------------------------------
        if (!sharedMzWritten_)
        {
            // First spectrum: write the shared m/z array
            writeArrayToIbd_(mz, opts_.mzType, sharedMzOffset_, sharedMzLength_);
            sharedMzWritten_ = true;
        }
        else if (mz.size() != sharedMzLength_)
        {
            throw std::invalid_argument(
                "ImzMLWriter::addSpectrum (continuous): m/z length " +
                std::to_string(mz.size()) +
                " differs from shared array length " +
                std::to_string(sharedMzLength_));
        }

        // Every spectrum points to the shared m/z array
        e.mzOffset = sharedMzOffset_;
        e.mzLength = sharedMzLength_;

        // Write this spectrum's intensity array
        writeArrayToIbd_(intensity, opts_.intType, e.intOffset, e.intLength);
    }
    else
    {
        // ----------------------------------------------------------------
        // Processed mode: write m/z + intensity arrays per spectrum
        // ----------------------------------------------------------------
        writeArrayToIbd_(mz,        opts_.mzType,  e.mzOffset,  e.mzLength);
        writeArrayToIbd_(intensity, opts_.intType, e.intOffset, e.intLength);
    }

    entries_.push_back(e);
}

// ---------------------------------------------------------------------------
// close
// ---------------------------------------------------------------------------
void ImzMLWriter::close()
{
    if (!isOpen())
        return;

    ibdStream_.close();
    writeXml_();

    // Reset
    entries_.clear();
    sharedMzWritten_ = false;
    imzmlPath_.clear();
    ibdPath_.clear();
}

// ---------------------------------------------------------------------------
// writeArrayToIbd_
// Write a double vector as the target BinaryDataType to the open ibd stream.
// Sets offsetOut to the byte position before writing.
// ---------------------------------------------------------------------------
void ImzMLWriter::writeArrayToIbd_(const std::vector<double>& data,
                                    BinaryDataType              type,
                                    uint64_t&                   offsetOut,
                                    uint64_t&                   lengthOut)
{
    offsetOut = static_cast<uint64_t>(ibdStream_.tellp());
    lengthOut = static_cast<uint64_t>(data.size());

    switch (type)
    {
        case BinaryDataType::Float32:
            for (double v : data) writeLE<float>(ibdStream_, static_cast<float>(v));
            break;
        case BinaryDataType::Float64:
            for (double v : data) writeLE<double>(ibdStream_, v);
            break;
        case BinaryDataType::Int32:
            for (double v : data) writeLE<int32_t>(ibdStream_, static_cast<int32_t>(v));
            break;
        case BinaryDataType::Int64:
            for (double v : data) writeLE<int64_t>(ibdStream_, static_cast<int64_t>(v));
            break;
        default:
            throw std::invalid_argument("ImzMLWriter: unsupported BinaryDataType");
    }
}

// ---------------------------------------------------------------------------
// cvParamForType_ / nameForType_
// ---------------------------------------------------------------------------
std::string ImzMLWriter::cvParamForType_(BinaryDataType t) const
{
    switch (t)
    {
        case BinaryDataType::Float32: return "MS:1000521";
        case BinaryDataType::Float64: return "MS:1000523";
        case BinaryDataType::Int32:   return "MS:1000519";
        case BinaryDataType::Int64:   return "MS:1000522";
        default:                      return "MS:1000521";
    }
}

std::string ImzMLWriter::nameForType_(BinaryDataType t) const
{
    switch (t)
    {
        case BinaryDataType::Float32: return "32-bit float";
        case BinaryDataType::Float64: return "64-bit float";
        case BinaryDataType::Int32:   return "32-bit integer";
        case BinaryDataType::Int64:   return "64-bit integer";
        default:                      return "32-bit float";
    }
}

// ---------------------------------------------------------------------------
// makeUuid_ – simple random UUID v4 (not cryptographic, sufficient for imzML)
// ---------------------------------------------------------------------------
std::string ImzMLWriter::makeUuid_() const
{
    std::mt19937_64 rng(std::random_device{}());
    std::uniform_int_distribution<uint64_t> dist;

    uint64_t hi = dist(rng);
    uint64_t lo = dist(rng);

    // Version 4, variant 1
    hi = (hi & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
    lo = (lo & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;

    // Format as xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx
    auto hex = [](uint64_t v, int digits) -> std::string {
        std::ostringstream ss;
        ss << std::hex << std::nouppercase << std::setfill('0') << std::setw(digits) << v;
        return ss.str();
    };

    std::string s;
    s +=  hex((hi >> 32) & 0xFFFFFFFF, 8) + "-";
    s +=  hex((hi >> 16) & 0xFFFF,     4) + "-";
    s +=  hex( hi        & 0xFFFF,     4) + "-";
    s +=  hex((lo >> 48) & 0xFFFF,     4) + "-";
    s +=  hex( lo        & 0xFFFFFFFFFFFFull, 12);

    return s;
}

// ---------------------------------------------------------------------------
// writeXml_  –  generate the complete .imzML file
// ---------------------------------------------------------------------------
void ImzMLWriter::writeXml_() const
{
    std::ofstream xml(imzmlPath_);
    if (!xml)
        throw std::runtime_error("ImzMLWriter: cannot write XML: " + imzmlPath_);

    // Compute grid extents from recorded entries
    int32_t maxX = 0, maxY = 0;
    for (const auto& e : entries_)
    {
        maxX = std::max(maxX, e.coord.x);
        maxY = std::max(maxY, e.coord.y);
    }

    const bool isContinuous = (opts_.mode != ImagingMode::Processed);
    const std::string modeAcc  = isContinuous ? "IMS:1000030" : "IMS:1000031";
    const std::string modeName = isContinuous ? "continuous"  : "processed";

    // ------------------------------------------------------------------
    // XML header + cvList
    // ------------------------------------------------------------------
    xml <<
R"(<?xml version="1.0" encoding="utf-8"?>
<mzML xmlns="http://psi.hupo.org/ms/mzml"
      xmlns:cv="http://psi.hupo.org/ms/mzml"
      xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance"
      xsi:schemaLocation="http://psi.hupo.org/ms/mzml http://psidev.info/files/ms/mzML/xsd/mzML1.1.0_idx.xsd"
      version="1.1.0">
  <cvList count="3">
    <cv id="MS"  fullName="Proteomics Standards Initiative Mass Spectrometry Ontology"
        version="4.1.30" URI="https://raw.githubusercontent.com/HUPO-PSI/psi-ms-CV/master/psi-ms.obo"/>
    <cv id="IMS" fullName="Imaging MS Ontology"
        version="0.9.1" URI="https://raw.githubusercontent.com/imzML/imzML/master/imagingMS.obo"/>
    <cv id="UO"  fullName="Unit Ontology"
        version="09:04:2014" URI="https://raw.githubusercontent.com/bio-ontology-research-group/unit-ontology/master/unit.obo"/>
  </cvList>
)";

    // ------------------------------------------------------------------
    // fileDescription
    // ------------------------------------------------------------------
    xml <<
"  <fileDescription>\n"
"    <fileContent>\n"
"      <cvParam accession=\"IMS:1000080\" name=\"universally unique identifier\" value=\"" << opts_.uuid << "\"/>\n"
"      <cvParam accession=\"" << modeAcc << "\" name=\"" << modeName << "\" value=\"\"/>\n"
"      <cvParam accession=\"MS:1000294\" name=\"mass spectrum\" value=\"\"/>\n"
"    </fileContent>\n"
"  </fileDescription>\n";

    // ------------------------------------------------------------------
    // referenceableParamGroupList
    // ------------------------------------------------------------------
    xml <<
"  <referenceableParamGroupList count=\"2\">\n"
"    <referenceableParamGroup id=\"mzArray\">\n"
"      <cvParam accession=\"MS:1000514\" name=\"m/z array\" value=\"\"/>\n"
"      <cvParam accession=\"" << cvParamForType_(opts_.mzType) << "\" name=\"" << nameForType_(opts_.mzType) << "\" value=\"\"/>\n"
"      <cvParam accession=\"IMS:1000101\" name=\"external data\" value=\"true\"/>\n"
"    </referenceableParamGroup>\n"
"    <referenceableParamGroup id=\"intensityArray\">\n"
"      <cvParam accession=\"MS:1000515\" name=\"intensity array\" value=\"\"/>\n"
"      <cvParam accession=\"" << cvParamForType_(opts_.intType) << "\" name=\"" << nameForType_(opts_.intType) << "\" value=\"\"/>\n"
"      <cvParam accession=\"IMS:1000101\" name=\"external data\" value=\"true\"/>\n"
"    </referenceableParamGroup>\n"
"  </referenceableParamGroupList>\n";

    // ------------------------------------------------------------------
    // scanSettingsList
    // ------------------------------------------------------------------
    xml <<
"  <scanSettingsList count=\"1\">\n"
"    <scanSettings id=\"scanSettings1\">\n"
"      <cvParam accession=\"IMS:1000042\" name=\"max count of pixels x\" value=\"" << maxX << "\"/>\n"
"      <cvParam accession=\"IMS:1000043\" name=\"max count of pixels y\" value=\"" << maxY << "\"/>\n";

    if (opts_.pixelSizeX > 0.0)
        xml << "      <cvParam accession=\"IMS:1000046\" name=\"pixel size x\" value=\""
            << opts_.pixelSizeX
            << "\" unitCvRef=\"UO\" unitAccession=\"UO:0000017\" unitName=\"micrometer\"/>\n";
    if (opts_.pixelSizeY > 0.0)
        xml << "      <cvParam accession=\"IMS:1000047\" name=\"pixel size y\" value=\""
            << opts_.pixelSizeY
            << "\" unitCvRef=\"UO\" unitAccession=\"UO:0000017\" unitName=\"micrometer\"/>\n";

    xml <<
"    </scanSettings>\n"
"  </scanSettingsList>\n";

    // ------------------------------------------------------------------
    // instrumentConfigurationList
    // ------------------------------------------------------------------
    std::string instName = opts_.instrumentName.empty() ? "Unknown" : opts_.instrumentName;
    // Use a generic MS instrument CV accession; users can override instrumentName
    xml <<
"  <instrumentConfigurationList count=\"1\">\n"
"    <instrumentConfiguration id=\"IC1\">\n"
"      <cvParam accession=\"MS:1000031\" name=\"instrument model\" value=\"" << instName << "\"/>\n"
"    </instrumentConfiguration>\n"
"  </instrumentConfigurationList>\n";

    // ------------------------------------------------------------------
    // dataProcessingList  (minimal, mandatory element)
    // ------------------------------------------------------------------
    xml <<
"  <dataProcessingList count=\"1\">\n"
"    <dataProcessing id=\"dp1\">\n"
"      <processingMethod order=\"1\" softwareRef=\"sw1\">\n"
"        <cvParam accession=\"MS:1000544\" name=\"Conversion to mzML\" value=\"\"/>\n"
"      </processingMethod>\n"
"    </dataProcessing>\n"
"  </dataProcessingList>\n"
"  <softwareList count=\"1\">\n"
"    <software id=\"sw1\" version=\"1.0\">\n"
"      <cvParam accession=\"MS:1000799\" name=\"custom unreleased software tool\" value=\"imzMLWriter\"/>\n"
"    </software>\n"
"  </softwareList>\n";

    // ------------------------------------------------------------------
    // spectrumList
    // ------------------------------------------------------------------
    xml <<
"  <run>\n"
"    <spectrumList count=\"" << entries_.size() << "\" defaultDataProcessingRef=\"dp1\">\n";

    for (std::size_t i = 0; i < entries_.size(); ++i)
    {
        const auto& e = entries_[i];

        const uint64_t mzEncodedLen  = e.mzLength  * bytesForType(opts_.mzType);
        const uint64_t intEncodedLen = e.intLength * bytesForType(opts_.intType);

        xml <<
"      <spectrum index=\"" << i << "\" defaultArrayLength=\"" << e.intLength << "\" id=\"spectrum=" << (i+1) << "\">\n"
"        <referenceableParamGroupRef ref=\"mzArray\"/>\n"
"        <scanList count=\"1\">\n"
"          <scan>\n"
"            <userParam name=\"[IMS:1000050] position x\" value=\"" << e.coord.x << "\"/>\n"
"            <userParam name=\"[IMS:1000051] position y\" value=\"" << e.coord.y << "\"/>\n"
"            <userParam name=\"[IMS:1000052] position z\" value=\"" << e.coord.z << "\"/>\n"
"            <cvParam accession=\"IMS:1000050\" name=\"position x\" value=\"" << e.coord.x << "\"/>\n"
"            <cvParam accession=\"IMS:1000051\" name=\"position y\" value=\"" << e.coord.y << "\"/>\n"
"            <cvParam accession=\"IMS:1000052\" name=\"position z\" value=\"" << e.coord.z << "\"/>\n"
"          </scan>\n"
"        </scanList>\n"
"        <binaryDataArrayList count=\"2\">\n"
"          <binaryDataArray>\n"
"            <referenceableParamGroupRef ref=\"mzArray\"/>\n"
"            <cvParam accession=\"IMS:1000102\" name=\"external offset\" value=\""     << e.mzOffset     << "\"/>\n"
"            <cvParam accession=\"IMS:1000103\" name=\"external array length\" value=\"" << e.mzLength  << "\"/>\n"
"            <cvParam accession=\"IMS:1000104\" name=\"external encoded length\" value=\"" << mzEncodedLen << "\"/>\n"
"            <binary/>\n"
"          </binaryDataArray>\n"
"          <binaryDataArray>\n"
"            <referenceableParamGroupRef ref=\"intensityArray\"/>\n"
"            <cvParam accession=\"IMS:1000102\" name=\"external offset\" value=\""     << e.intOffset    << "\"/>\n"
"            <cvParam accession=\"IMS:1000103\" name=\"external array length\" value=\"" << e.intLength << "\"/>\n"
"            <cvParam accession=\"IMS:1000104\" name=\"external encoded length\" value=\"" << intEncodedLen << "\"/>\n"
"            <binary/>\n"
"          </binaryDataArray>\n"
"        </binaryDataArrayList>\n"
"      </spectrum>\n";
    }

    xml <<
"    </spectrumList>\n"
"  </run>\n"
"</mzML>\n";

    if (!xml)
        throw std::runtime_error("ImzMLWriter: write error on XML file: " + imzmlPath_);
}

} // namespace imzml
