// ---------------------------------------------------------------------------
// openms_integration/tests/test_openms_imzml.cpp
// Integration tests for the OpenMS-API imzML layer
//
// Uses same hand-rolled assert macros as the standalone test so there
// is no external test-framework dependency.
// ---------------------------------------------------------------------------
#include <OpenMS/FORMAT/ImzMLFile.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/INTERFACES/IMSDataConsumer.h>

#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <string>
#include <stdexcept>

// ---------------------------------------------------------------------------
// Minimal test harness
// ---------------------------------------------------------------------------
static int g_pass = 0, g_fail = 0;

#define EXPECT_EQ(a, b) do { \
    if ((a) == (b)) { ++g_pass; } \
    else { ++g_fail; fprintf(stderr, "FAIL %s:%d  %s == %s  (%s)\n", \
           __FILE__, __LINE__, #a, #b, \
           (std::to_string(a) + " vs " + std::to_string(b)).c_str()); } \
} while(0)

#define EXPECT_TRUE(e) do { \
    if (e) { ++g_pass; } \
    else { ++g_fail; fprintf(stderr, "FAIL %s:%d  expected true: %s\n", \
           __FILE__, __LINE__, #e); } \
} while(0)

#define EXPECT_NEAR(a, b, eps) do { \
    double _a = (a), _b = (b), _e = (eps); \
    if (std::fabs(_a - _b) <= _e) { ++g_pass; } \
    else { ++g_fail; fprintf(stderr, "FAIL %s:%d  |%g - %g| = %g > %g\n", \
           __FILE__, __LINE__, _a, _b, std::fabs(_a-_b), _e); } \
} while(0)

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

// Test 1: basic load
static void testLoad(const std::string& imzml_path)
{
    OpenMS::MSExperiment exp;
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    f.load(imzml_path, exp);

    EXPECT_EQ(exp.size(), std::size_t(9));             // 3x3 grid
    EXPECT_TRUE(!exp.empty());
}

// Test 2: metadata
static void testMetadata(const std::string& imzml_path)
{
    OpenMS::MSExperiment exp;
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    f.loadMetadata(imzml_path, exp);

    const auto& meta = exp.getImzMLMetadata();
    EXPECT_TRUE(meta.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Continuous);
    EXPECT_EQ(meta.maxX, uint32_t(3));
    EXPECT_EQ(meta.maxY, uint32_t(3));
}

// Test 3: pixel coordinates
static void testPixelCoords(const std::string& imzml_path)
{
    OpenMS::MSExperiment exp;
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    f.load(imzml_path, exp);

    // First spectrum must be at (1,1,1)
    EXPECT_EQ(exp[0].getCoordX(), uint32_t(1));
    EXPECT_EQ(exp[0].getCoordY(), uint32_t(1));
    EXPECT_EQ(exp[0].getCoordZ(), uint32_t(1));

    // Centre spectrum at (2,2,1)
    EXPECT_EQ(exp[4].getCoordX(), uint32_t(2));
    EXPECT_EQ(exp[4].getCoordY(), uint32_t(2));
}

// Test 4: peaks decode + sort
static void testPeaksDecode(const std::string& imzml_path)
{
    OpenMS::MSExperiment exp;
    OpenMS::PeakFileOptions opts;
    opts.setSortMZ(true);
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    f.load(imzml_path, exp, opts);

    EXPECT_TRUE(exp[0].size() > 0u);

    // m/z array should be sorted ascending
    const auto& s = exp[0];
    bool sorted = true;
    for (std::size_t i = 1; i < s.size(); ++i)
        if (s[i].getMZ() < s[i-1].getMZ()) { sorted = false; break; }
    EXPECT_TRUE(sorted);

    // First m/z should be >= 100
    EXPECT_TRUE(s[0].getMZ() >= 100.0);
}

// Test 5: coordinate filter
static void testCoordFilter(const std::string& imzml_path)
{
    OpenMS::MSExperiment exp;
    OpenMS::PeakFileOptions opts;
    opts.setCoordinateFilter(1, 1, 1, 1, 1, 1);  // only pixel (1,1,1)
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    f.load(imzml_path, exp, opts);

    EXPECT_EQ(exp.size(), std::size_t(1));
    EXPECT_EQ(exp[0].getCoordX(), uint32_t(1));
    EXPECT_EQ(exp[0].getCoordY(), uint32_t(1));
}

// Test 6: metadata-only load (no IBD decode)
static void testMetadataOnly(const std::string& imzml_path)
{
    OpenMS::MSExperiment exp;
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    f.loadMetadata(imzml_path, exp);

    // Spectra should have no peaks (IBD skipped)
    EXPECT_TRUE(exp.size() == 0u || exp[0].empty());
}

// Test 7: streaming consumer
static void testStreamingConsumer(const std::string& imzml_path)
{
    struct CountConsumer : public OpenMS::IMSDataConsumer
    {
        std::size_t count {0};
        double      totalTIC {0.0};
        void setExpectedSize(std::size_t) override {}
        void setExperimentalSettings(const OpenMS::MSExperiment&) override {}
        void consumeSpectrum(OpenMS::MSSpectrum s) override
        {
            ++count;
            for (const auto& p : s) totalTIC += p.getIntensity();
        }
    };

    CountConsumer consumer;
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    OpenMS::PeakFileOptions opts;
    f.load(imzml_path, consumer, opts);

    EXPECT_EQ(consumer.count, std::size_t(9));
    EXPECT_TRUE(consumer.totalTIC > 0.0);
}

// Test 8: validate
static void testValidate(const std::string& imzml_path)
{
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    std::vector<std::string> errors;
    bool ok = f.validate(imzml_path, errors);
    EXPECT_TRUE(ok);
    EXPECT_EQ(errors.size(), std::size_t(0));
}

// Test 9: findAtCoordinate
static void testFindAtCoordinate(const std::string& imzml_path)
{
    OpenMS::MSExperiment exp;
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    f.load(imzml_path, exp);

    const OpenMS::MSSpectrum* s = exp.findAtCoordinate(3, 3, 1);
    EXPECT_TRUE(s != nullptr);
    if (s)
    {
        EXPECT_EQ(s->getCoordX(), uint32_t(3));
        EXPECT_EQ(s->getCoordY(), uint32_t(3));
    }
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        fprintf(stderr, "Usage: %s <path/to/Example_Continuous.imzML>\n", argv[0]);
        return 1;
    }
    const std::string path = argv[1];

    try
    {
        testLoad(path);
        testMetadata(path);
        testPixelCoords(path);
        testPeaksDecode(path);
        testCoordFilter(path);
        testMetadataOnly(path);
        testStreamingConsumer(path);
        testValidate(path);
        testFindAtCoordinate(path);
    }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Unexpected exception: %s\n", e.what());
        ++g_fail;
    }

    int total = g_pass + g_fail;
    fprintf(stderr, "\n[OpenMS integration] %d / %d tests passed\n", g_pass, total);
    return g_fail > 0 ? 1 : 0;
}
