// ---------------------------------------------------------------------------
// OpenMS/INTERFACES/IMSDataConsumer.h
// Streaming consumer interface — mirrors OpenMS IMSDataConsumer<ExperimentType>
//
// Implement this interface to receive spectra one-by-one as they are decoded
// from the imzML/IBD file pair without buffering the entire dataset in memory.
//
// Usage:
//   class MyConsumer : public OpenMS::IMSDataConsumer
//   {
//   public:
//       void consumeSpectrum(OpenMS::MSSpectrum s) override
//       {
//           // process spectrum, write to disk, etc.
//       }
//       void setExpectedSize(std::size_t /*n*/) override {}
//       void setExperimentalSettings(const OpenMS::MSExperiment& exp) override
//       {
//           meta_ = exp.getImzMLMetadata();
//       }
//   };
//
//   MyConsumer c;
//   OpenMS::ImzMLFile f;
//   f.load("tissue.imzML", c, opts);
//
// ---------------------------------------------------------------------------
#pragma once

#include <OpenMS/KERNEL/MSSpectrum.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <cstddef>

namespace OpenMS
{

class IMSDataConsumer
{
public:
    virtual ~IMSDataConsumer() = default;

    // Called once before any spectra are delivered.
    // `totalSpectra` is the count declared in the imzML header
    // (may be 0 if not yet known).
    virtual void setExpectedSize(std::size_t totalSpectra) = 0;

    // Called once immediately before the first consumeSpectrum() call.
    // Delivers the fully populated MSExperiment (with ImzMLMetadata, grid
    // extents, etc.) but with an EMPTY spectra container — the individual
    // spectra are delivered via consumeSpectrum().
    virtual void setExperimentalSettings(const MSExperiment& meta) = 0;

    // Called for every spectrum in load order.
    // The spectrum is passed by value so the consumer can std::move it if
    // needed.  The handler may reuse the MSSpectrum object after the call
    // returns so the consumer must not retain a pointer into it.
    virtual void consumeSpectrum(MSSpectrum spectrum) = 0;
};

// ---------------------------------------------------------------------------
// SimpleCollectingConsumer — reference implementation: accumulates all
// spectra into an MSExperiment.  Mirrors OpenMS's own BufferingCollector.
// ---------------------------------------------------------------------------
class SimpleCollectingConsumer : public IMSDataConsumer
{
public:
    explicit SimpleCollectingConsumer(MSExperiment& exp) : exp_(exp) {}

    void setExpectedSize(std::size_t n) override
    {
        exp_.reserve(n);
    }

    void setExperimentalSettings(const MSExperiment& meta) override
    {
        exp_.getImzMLMetadata() = meta.getImzMLMetadata();
    }

    void consumeSpectrum(MSSpectrum s) override
    {
        exp_.push_back(std::move(s));
    }

private:
    MSExperiment& exp_;
};

} // namespace OpenMS
