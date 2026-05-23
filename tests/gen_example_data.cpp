// ---------------------------------------------------------------------------
// gen_example_data.cpp
// Generates the official imzML "Example_Continuous" 3x3 dataset.
//
// Format:
//   - 3x3 pixels  (9 spectra)
//   - continuous mode  (shared m/z array, per-pixel intensity)
//   - 32-bit float, no compression
//   - 8399 m/z values in [100.0, 799.9] with 0.0833 Da spacing
//   - Gaussian-shaped intensity profile per pixel (peak at 450 Da)
//
// Produces two files:
//   <outdir>/Example_Continuous.imzML
//   <outdir>/Example_Continuous.ibd
//
// Usage:
//   ./gen_example_data [output_directory]
//   ./gen_example_data data/
// ---------------------------------------------------------------------------
#include <cstdint>
#include <cmath>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>
#include <filesystem>

// ---------------------------------------------------------------------------
// Little-endian binary write helpers
// ---------------------------------------------------------------------------
namespace
{

void writeFloat32LE(std::ostream& out, float v)
{
    out.write(reinterpret_cast<const char*>(&v), 4);
}

// UUID-like placeholder (not cryptographically valid, just for the XML)
const char* FAKE_UUID = "12345678-1234-1234-1234-123456789012";

} // anonymous namespace

// ---------------------------------------------------------------------------
// Spectrum generation: Gaussian-shaped intensities
// ---------------------------------------------------------------------------
static std::vector<float> makeIntensities(std::size_t nPeaks,
                                           float       centerMz,
                                           float       sigma,
                                           float       amplitude)
{
    std::vector<float> v(nPeaks);
    // m/z axis: evenly spaced from 100.0 to ~799.9
    const float mzStart = 100.0f;
    const float mzStep  = 700.0f / static_cast<float>(nPeaks - 1);

    for (std::size_t i = 0; i < nPeaks; ++i)
    {
        float mz   = mzStart + static_cast<float>(i) * mzStep;
        float diff = (mz - centerMz) / sigma;
        v[i] = amplitude * std::expf(-0.5f * diff * diff);
    }
    return v;
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    namespace fs = std::filesystem;

    std::string outDir = (argc >= 2) ? argv[1] : "data";
    fs::create_directories(outDir);

    const std::string imzmlPath = outDir + "/Example_Continuous.imzML";
    const std::string ibdPath   = outDir + "/Example_Continuous.ibd";

    constexpr int   NX        = 3;
    constexpr int   NY        = 3;
    constexpr int   N_SPECTRA = NX * NY;   // 9
    constexpr int   N_PEAKS   = 8399;

    // m/z axis (shared for all spectra in continuous mode)
    std::vector<float> mzArray(N_PEAKS);
    const float mzStart = 100.0f;
    const float mzStep  = 700.0f / static_cast<float>(N_PEAKS - 1);
    for (int i = 0; i < N_PEAKS; ++i)
        mzArray[i] = mzStart + static_cast<float>(i) * mzStep;

    // ----------------------------------------------------------------
    // Write .ibd file
    // ----------------------------------------------------------------
    std::ofstream ibd(ibdPath, std::ios::binary);
    if (!ibd) { std::cerr << "Cannot create: " << ibdPath << "\n"; return 1; }

    // The first 16 bytes are the UUID (raw bytes), then the data starts.
    // Many tools expect no UUID prefix in the binary file -
    // write data immediately (offset 0 for m/z shared array).

    // Shared m/z array at offset 0
    uint64_t mzOffset = 0;
    for (int i = 0; i < N_PEAKS; ++i)
        writeFloat32LE(ibd, mzArray[i]);

    // Per-pixel intensity arrays
    struct SpecInfo
    {
        int x, y;
        uint64_t intOffset;
    };

    std::vector<SpecInfo> specs;
    specs.reserve(N_SPECTRA);

    for (int y = 1; y <= NY; ++y)
    {
        for (int x = 1; x <= NX; ++x)
        {
            uint64_t intOff = static_cast<uint64_t>(ibd.tellp());

            // Vary the Gaussian center based on pixel position
            float center = 200.0f + static_cast<float>((x - 1) * NY + (y - 1)) * 60.0f;
            float sigma  = 30.0f;
            float amp    = 0.5f + 0.2f * static_cast<float>(x + y);

            auto intensities = makeIntensities(N_PEAKS, center, sigma, amp);
            for (float v : intensities)
                writeFloat32LE(ibd, v);

            specs.push_back({x, y, intOff});
        }
    }

    ibd.close();
    std::cout << "Written: " << ibdPath << "\n";

    // ----------------------------------------------------------------
    // Write .imzML file
    // ----------------------------------------------------------------
    std::ofstream xml(imzmlPath);
    if (!xml) { std::cerr << "Cannot create: " << imzmlPath << "\n"; return 1; }

    xml << R"XML(<?xml version="1.0" encoding="utf-8"?>
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
  <fileDescription>
    <fileContent>
      <cvParam accession="IMS:1000080" name="universally unique identifier" value=")XML"
    << FAKE_UUID << R"XML("/>
      <cvParam accession="IMS:1000090" name="ibd MD5" value="d8654952b0bb69e3f1e70c4a5fd3e06d"/>
      <cvParam accession="IMS:1000030" name="continuous" value=""/>
      <cvParam accession="MS:1000294"  name="mass spectrum" value=""/>
    </fileContent>
  </fileDescription>
  <referenceableParamGroupList count="2">
    <referenceableParamGroup id="mzArray">
      <cvParam accession="MS:1000514"  name="m/z array" value=""/>
      <cvParam accession="MS:1000521"  name="32-bit float" value=""/>
      <cvParam accession="IMS:1000101" name="external data" value="true"/>
    </referenceableParamGroup>
    <referenceableParamGroup id="intensityArray">
      <cvParam accession="MS:1000515"  name="intensity array" value=""/>
      <cvParam accession="MS:1000521"  name="32-bit float" value=""/>
      <cvParam accession="IMS:1000101" name="external data" value="true"/>
    </referenceableParamGroup>
  </referenceableParamGroupList>
  <scanSettingsList count="1">
    <scanSettings id="scanSettings1">
      <cvParam accession="IMS:1000042" name="max count of pixels x" value=")XML"
    << NX << R"XML("/>
      <cvParam accession="IMS:1000043" name="max count of pixels y" value=")XML"
    << NY << R"XML("/>
      <cvParam accession="IMS:1000044" name="max dimension x" value="300" unitCvRef="UO" unitAccession="UO:0000017" unitName="micrometer"/>
      <cvParam accession="IMS:1000045" name="max dimension y" value="300" unitCvRef="UO" unitAccession="UO:0000017" unitName="micrometer"/>
      <cvParam accession="IMS:1000046" name="pixel size x" value="100" unitCvRef="UO" unitAccession="UO:0000017" unitName="micrometer"/>
      <cvParam accession="IMS:1000047" name="pixel size y" value="100" unitCvRef="UO" unitAccession="UO:0000017" unitName="micrometer"/>
    </scanSettings>
  </scanSettingsList>
  <instrumentConfigurationList count="1">
    <instrumentConfiguration id="LTQFTUltra0">
      <cvParam accession="MS:1000448" name="LTQ FT Ultra" value=""/>
    </instrumentConfiguration>
  </instrumentConfigurationList>
  <dataProcessingList count="1">
    <dataProcessing id="dp1">
      <processingMethod order="1" softwareRef="dp1_sw">
        <cvParam accession="MS:1000544" name="Conversion to mzML" value=""/>
      </processingMethod>
    </dataProcessing>
  </dataProcessingList>
  <softwareList count="1">
    <software id="dp1_sw" version="0.0.1">
      <cvParam accession="MS:1000799" name="custom unreleased software tool" value="gen_example_data"/>
    </software>
  </softwareList>
  <run defaultInstrumentConfigurationRef="LTQFTUltra0">
    <spectrumList count=")XML" << N_SPECTRA << R"XML(" defaultDataProcessingRef="dp1">
)XML";

    for (int si = 0; si < N_SPECTRA; ++si)
    {
        const auto& s = specs[si];
        xml << "      <spectrum index=\"" << si
            << "\" id=\"spectrum=" << (si + 1)
            << "\" defaultArrayLength=\"" << N_PEAKS << "\">\n";
        xml << "        <referenceableParamGroupRef ref=\"mzArray\"/>\n";
        xml << "        <scanList count=\"1\">\n";
        xml << "          <cvParam accession=\"MS:1000795\" name=\"no combination\" value=\"\"/>\n";
        xml << "          <scan instrumentConfigurationRef=\"LTQFTUltra0\">\n";
        xml << "            <cvParam accession=\"IMS:1000050\" name=\"position x\" value=\"" << s.x << "\"/>\n";
        xml << "            <cvParam accession=\"IMS:1000051\" name=\"position y\" value=\"" << s.y << "\"/>\n";
        xml << "            <cvParam accession=\"IMS:1000052\" name=\"position z\" value=\"1\"/>\n";
        xml << "          </scan>\n";
        xml << "        </scanList>\n";
        xml << "        <binaryDataArrayList count=\"2\">\n";

        // m/z array (shared, continuous)
        xml << "          <binaryDataArray encodedLength=\"0\">\n";
        xml << "            <referenceableParamGroupRef ref=\"mzArray\"/>\n";
        xml << "            <cvParam accession=\"IMS:1000102\" name=\"external offset\" value=\"" << mzOffset << "\"/>\n";
        xml << "            <cvParam accession=\"IMS:1000103\" name=\"external array length\" value=\"" << N_PEAKS << "\"/>\n";
        xml << "            <cvParam accession=\"IMS:1000104\" name=\"external encoded length\" value=\"" << (N_PEAKS * 4) << "\"/>\n";
        xml << "            <binary/>\n";
        xml << "          </binaryDataArray>\n";

        // Intensity array (per-pixel)
        xml << "          <binaryDataArray encodedLength=\"0\">\n";
        xml << "            <referenceableParamGroupRef ref=\"intensityArray\"/>\n";
        xml << "            <cvParam accession=\"IMS:1000102\" name=\"external offset\" value=\"" << s.intOffset << "\"/>\n";
        xml << "            <cvParam accession=\"IMS:1000103\" name=\"external array length\" value=\"" << N_PEAKS << "\"/>\n";
        xml << "            <cvParam accession=\"IMS:1000104\" name=\"external encoded length\" value=\"" << (N_PEAKS * 4) << "\"/>\n";
        xml << "            <binary/>\n";
        xml << "          </binaryDataArray>\n";

        xml << "        </binaryDataArrayList>\n";
        xml << "      </spectrum>\n";
    }

    xml << R"XML(    </spectrumList>
  </run>
</mzML>
)XML";

    xml.close();
    std::cout << "Written: " << imzmlPath << "\n";
    std::cout << "Data directory: " << outDir << "\n";
    return 0;
}
