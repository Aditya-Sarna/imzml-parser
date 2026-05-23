// ---------------------------------------------------------------------------
// tools/imzml_cli.cpp
// imzML command-line tool — black & white terminal UI
//
// Subcommands:
//   imzml info     <file.imzML>              -- dataset summary
//   imzml validate <file.imzML>              -- XML + IBD bounds check
//   imzml inspect  <file.imzML> -n N         -- single spectrum table
//                              [--mz-range MIN MAX]
//   imzml export   <file.imzML>              -- dump peaks to stdout
//                              [--format csv|tsv]
//                              [--spectrum N | --all]
//   imzml stats    <file.imzML>              -- per-pixel TIC / peak count
//
// Style: no colour, no emoji, fixed-width columns, box-drawing borders
// ---------------------------------------------------------------------------
#include <OpenMS/FORMAT/ImzMLFile.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/INTERFACES/IMSDataConsumer.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <sstream>
#include <iomanip>
#include <numeric>

// ---------------------------------------------------------------------------
// Terminal width helper
// ---------------------------------------------------------------------------
static int termWidth()
{
#ifdef _WIN32
    return 80;
#else
    const char* cols = getenv("COLUMNS");
    if (cols) return std::max(40, std::atoi(cols));
    return 80;
#endif
}

// ---------------------------------------------------------------------------
// Box-drawing helpers (no emoji, plain ASCII border with Unicode box chars)
// ---------------------------------------------------------------------------
static void printHRule(int width, bool top = false, bool bottom = false)
{
    if (top)    { printf("\u250c"); }
    else if (bottom) { printf("\u2514"); }
    else        { printf("\u251c"); }
    for (int i = 1; i < width - 1; ++i) printf("\u2500");
    if (top)    { printf("\u2510\n"); }
    else if (bottom) { printf("\u2518\n"); }
    else        { printf("\u2524\n"); }
}

static void printRow(const std::string& label, const std::string& value, int width)
{
    // | label (20) : value |
    int inner = width - 2;
    std::string line;
    line.reserve(inner);
    line += " ";
    line += label;
    line += ": ";
    line += value;
    if ((int)line.size() > inner)
        line.resize(inner);
    else
        line.append(inner - line.size(), ' ');
    printf("\u2502%s\u2502\n", line.c_str());
}

static void printTitle(const std::string& title, int width)
{
    printHRule(width, /*top=*/true);
    int inner = width - 2;
    int pad = (inner - (int)title.size()) / 2;
    std::string line(inner, ' ');
    if (pad >= 0 && pad + (int)title.size() <= inner)
        line.replace(pad, title.size(), title);
    printf("\u2502%s\u2502\n", line.c_str());
    printHRule(width);
}

// ---------------------------------------------------------------------------
// Subcommand: info
// ---------------------------------------------------------------------------
static int cmdInfo(const std::string& path)
{
    OpenMS::MSExperiment exp;
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);

    try { f.loadMetadata(path, exp); }
    catch (const std::exception& e)
    {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }

    const auto& meta = exp.getImzMLMetadata();
    int w = std::min(termWidth(), 72);

    printTitle("imzML Dataset Info", w);
    printRow("File",         path, w);
    printRow("IBD file",     meta.ibdFilePath.empty() ? "(not found)" : meta.ibdFilePath, w);

    std::string mode =
        (meta.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Continuous)  ? "Continuous"  :
        (meta.imagingMode == OpenMS::ImzMLMetadata::ImagingMode::Processed)   ? "Processed"   : "Unknown";
    printRow("Imaging mode", mode, w);

    char buf[128];
    snprintf(buf, sizeof(buf), "%u x %u x %u", meta.maxX, meta.maxY, meta.maxZ);
    printRow("Grid (x,y,z)", buf, w);

    snprintf(buf, sizeof(buf), "%zu", exp.size());
    printRow("Spectra",      std::string(buf), w);

    snprintf(buf, sizeof(buf), "%u um", meta.pixelSizeX);
    printRow("Pixel size X", std::string(buf), w);
    snprintf(buf, sizeof(buf), "%u um", meta.pixelSizeY);
    printRow("Pixel size Y", std::string(buf), w);

    if (!meta.ibdChecksum.empty())
    {
        printRow("IBD checksum (" + meta.ibdChecksumType + ")", meta.ibdChecksum, w);
    }

    printHRule(w, /*top=*/false, /*bottom=*/true);
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: validate
// ---------------------------------------------------------------------------
static int cmdValidate(const std::string& path)
{
    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::CMD);
    std::vector<std::string> errors;
    bool ok = f.validate(path, errors);

    int w = std::min(termWidth(), 72);
    printTitle("imzML Validation", w);
    if (ok)
    {
        printRow("Result", "PASS -- no errors found", w);
    }
    else
    {
        printRow("Result", "FAIL", w);
        int i = 1;
        for (const auto& e : errors)
        {
            char lbl[32];
            snprintf(lbl, sizeof(lbl), "Error %d", i++);
            printRow(lbl, e, w);
        }
    }
    printHRule(w, false, true);
    return ok ? 0 : 2;
}

// ---------------------------------------------------------------------------
// Subcommand: inspect
// ---------------------------------------------------------------------------
static int cmdInspect(const std::string& path, int n,
                       double mzLo, double mzHi)
{
    OpenMS::MSExperiment exp;
    OpenMS::PeakFileOptions opts;
    opts.setSortMZ(true);
    if (mzLo < mzHi) opts.setMZRange(mzLo, mzHi);

    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);

    try { f.load(path, exp, opts); }
    catch (const std::exception& e)
    { fprintf(stderr, "Error: %s\n", e.what()); return 1; }

    if (n < 0 || (std::size_t)n >= exp.size())
    {
        fprintf(stderr, "Error: spectrum index %d out of range [0, %zu)\n",
                n, exp.size());
        return 1;
    }

    const auto& s = exp[(std::size_t)n];
    int w = std::min(termWidth(), 60);
    char title[64];
    snprintf(title, sizeof(title), "Spectrum %d  (x=%u y=%u z=%u)",
             n, s.getCoordX(), s.getCoordY(), s.getCoordZ());
    printTitle(title, w);

    // Header row
    char hdr[64];
    snprintf(hdr, sizeof(hdr), "%-8s  %-16s  %-12s", "#", "m/z", "intensity");
    printRow(std::string(hdr), "", w);
    printHRule(w);

    // Data rows
    int shown = 0;
    const int MAX_ROWS = 200;
    for (std::size_t i = 0; i < s.size() && shown < MAX_ROWS; ++i)
    {
        const auto& pk = s[i];
        if (mzLo < mzHi && (pk.getMZ() < mzLo || pk.getMZ() > mzHi)) continue;
        char row[64];
        snprintf(row, sizeof(row), "%-8zu  %-16.6f  %-12.1f", i, pk.getMZ(), pk.getIntensity());
        printRow(std::string(row), "", w);
        ++shown;
    }
    if (shown == MAX_ROWS)
        printRow("...", "(truncated to 200 rows)", w);

    printHRule(w, false, true);
    printf("  Peaks shown: %d / %zu\n", shown, s.size());
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: export
// ---------------------------------------------------------------------------
static int cmdExport(const std::string& path, int specIdx, bool allSpectra,
                      char sep)
{
    OpenMS::MSExperiment exp;
    OpenMS::PeakFileOptions opts;
    opts.setSortMZ(true);

    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::NONE);
    try { f.load(path, exp, opts); }
    catch (const std::exception& e)
    { fprintf(stderr, "Error: %s\n", e.what()); return 1; }

    // Print header
    printf("spectrum_index%cx%cy%cz%cmz%cintensity\n",
           sep,sep,sep,sep,sep);

    auto writeSpectrum = [&](int idx, const OpenMS::MSSpectrum& s)
    {
        for (const auto& pk : s)
        {
            printf("%d%c%u%c%u%c%u%c%.6f%c%.4f\n",
                   idx, sep, s.getCoordX(), sep, s.getCoordY(), sep,
                   s.getCoordZ(), sep, pk.getMZ(), sep, pk.getIntensity());
        }
    };

    if (allSpectra)
    {
        for (std::size_t i = 0; i < exp.size(); ++i)
            writeSpectrum((int)i, exp[i]);
    }
    else
    {
        if (specIdx < 0 || (std::size_t)specIdx >= exp.size())
        {
            fprintf(stderr, "Error: spectrum %d out of range\n", specIdx);
            return 1;
        }
        writeSpectrum(specIdx, exp[(std::size_t)specIdx]);
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Subcommand: stats
// ---------------------------------------------------------------------------
static int cmdStats(const std::string& path)
{
    OpenMS::MSExperiment exp;
    OpenMS::PeakFileOptions opts;
    opts.setSortMZ(false); // don't need sorted for TIC

    OpenMS::ImzMLFile f;
    f.setLogType(OpenMS::ProgressLogger::CMD);
    try { f.load(path, exp, opts); }
    catch (const std::exception& e)
    { fprintf(stderr, "Error: %s\n", e.what()); return 1; }

    int w = std::min(termWidth(), 72);
    printTitle("Spectrum Statistics", w);

    // Header
    char hdr[80];
    snprintf(hdr, sizeof(hdr), "%-6s %-6s %-6s %-6s %-14s %-12s",
             "idx","x","y","z","TIC","peaks");
    printRow(std::string(hdr), "", w);
    printHRule(w);

    double total_tic = 0.0;
    std::size_t total_peaks = 0;

    for (std::size_t i = 0; i < exp.size(); ++i)
    {
        const auto& s = exp[i];
        double tic = 0.0;
        for (const auto& p : s) tic += p.getIntensity();
        total_tic += tic;
        total_peaks += s.size();

        char row[80];
        snprintf(row, sizeof(row),
                 "%-6zu %-6u %-6u %-6u %-14.3f %-12zu",
                 i, s.getCoordX(), s.getCoordY(), s.getCoordZ(),
                 tic, s.size());
        printRow(std::string(row), "", w);
    }

    printHRule(w);
    char sum[80];
    snprintf(sum, sizeof(sum), "Total TIC: %.3f  Total peaks: %zu  Spectra: %zu",
             total_tic, total_peaks, exp.size());
    printRow(std::string(sum), "", w);
    printHRule(w, false, true);
    return 0;
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------
static void usage(const char* prog)
{
    fprintf(stderr,
        "Usage: %s <subcommand> <file.imzML> [options]\n"
        "\n"
        "Subcommands:\n"
        "  info     <file>                  Print dataset summary\n"
        "  validate <file>                  Validate XML and IBD bounds\n"
        "  inspect  <file> -n N             Print peaks of spectrum N\n"
        "                  [--mz-range LO HI]\n"
        "  export   <file> [--spectrum N | --all]\n"
        "                  [--format csv|tsv]  Export peaks to stdout\n"
        "  stats    <file>                  Per-pixel TIC and peak counts\n"
        "\n", prog);
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------
int main(int argc, char* argv[])
{
    if (argc < 3)
    {
        usage(argv[0]);
        return 1;
    }

    const std::string sub  = argv[1];
    const std::string path = argv[2];

    if (sub == "info")    return cmdInfo(path);
    if (sub == "validate") return cmdValidate(path);

    if (sub == "inspect")
    {
        int n = 0;
        double lo = 0.0, hi = 0.0;
        for (int i = 3; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "-n") == 0 && i + 1 < argc)
                n = std::atoi(argv[++i]);
            else if (std::strcmp(argv[i], "--mz-range") == 0 && i + 2 < argc)
            { lo = std::atof(argv[i+1]); hi = std::atof(argv[i+2]); i += 2; }
        }
        return cmdInspect(path, n, lo, hi);
    }

    if (sub == "export")
    {
        int specIdx = 0;
        bool allSpectra = false;
        char sep = ',';
        for (int i = 3; i < argc; ++i)
        {
            if (std::strcmp(argv[i], "--spectrum") == 0 && i + 1 < argc)
                specIdx = std::atoi(argv[++i]);
            else if (std::strcmp(argv[i], "--all") == 0)
                allSpectra = true;
            else if (std::strcmp(argv[i], "--format") == 0 && i + 1 < argc)
            {
                ++i;
                if (std::strcmp(argv[i], "tsv") == 0) sep = '\t';
            }
        }
        return cmdExport(path, specIdx, allSpectra, sep);
    }

    if (sub == "stats") return cmdStats(path);

    fprintf(stderr, "Unknown subcommand: %s\n", sub.c_str());
    usage(argv[0]);
    return 1;
}
