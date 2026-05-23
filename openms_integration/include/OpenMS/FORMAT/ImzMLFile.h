// ---------------------------------------------------------------------------
// OpenMS/FORMAT/ImzMLFile.h
// Public API — mirrors OpenMS MzMLFile / XMLFile
//
// Usage:
//   // Load entire dataset into memory
//   OpenMS::MSExperiment exp;
//   OpenMS::PeakFileOptions opts;
//   OpenMS::ImzMLFile f;
//   f.setLogType(OpenMS::ProgressLogger::CMD);
//   f.load("tissue.imzML", exp, opts);
//
//   // Streaming (low-memory) load
//   MyConsumer consumer;
//   f.load("tissue.imzML", consumer, opts);
// ---------------------------------------------------------------------------
#pragma once

#include <OpenMS/CONCEPT/ProgressLogger.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/INTERFACES/IMSDataConsumer.h>

#include <string>

namespace OpenMS
{

class ImzMLFile : public ProgressLogger
{
public:
    ImzMLFile() = default;
    ~ImzMLFile() = default;

    // ------------------------------------------------------------------
    // load with in-memory collection  (mirrors MzMLFile::load)
    // ------------------------------------------------------------------
    void load(const std::string&    imzml_path,
              MSExperiment&         exp,
              const PeakFileOptions& opts = PeakFileOptions{});

    // ------------------------------------------------------------------
    // load with streaming consumer    (mirrors MSDataTransformingConsumer usage)
    // ------------------------------------------------------------------
    void load(const std::string&    imzml_path,
              IMSDataConsumer&      consumer,
              const PeakFileOptions& opts = PeakFileOptions{});

    // ------------------------------------------------------------------
    // load metadata only (no IBD decode) — fast header read
    // ------------------------------------------------------------------
    void loadMetadata(const std::string& imzml_path, MSExperiment& exp);

    // ------------------------------------------------------------------
    // validate only (parse XML + check IBD bounds, no peak vectors kept)
    // Returns true if valid, writes error descriptions to `errors`
    // ------------------------------------------------------------------
    bool validate(const std::string& imzml_path,
                  std::vector<std::string>& errors) const;

private:
    void loadImpl_(const std::string&     imzml_path,
                   IMSDataConsumer*       consumer,    // non-owning, may be null
                   MSExperiment&          meta_holder,
                   const PeakFileOptions& opts);

    // Deduce .ibd path from .imzML path (same base name, case-insensitive)
    static std::string inferIbdPath_(const std::string& imzml_path);
};

} // namespace OpenMS
