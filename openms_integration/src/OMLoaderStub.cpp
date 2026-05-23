// ---------------------------------------------------------------------------
// OMLoaderStub.cpp — fallback when bioconda OpenMS is not available
//
// Provides a stub loadWithRealOpenMS() that returns an empty result so
// ImzMLFile.cpp compiles and links even without bioconda installed.
//
// This file is compiled into openms_imzml only when HAS_BIOCONDA_OPENMS=FALSE.
// When bioconda IS available, the real implementation in OMLoader.cpp
// (compiled as openms_loader_obj) provides this function instead.
// ---------------------------------------------------------------------------
#include <OMLoader.h>

namespace OpenMS
{
namespace Internal
{

OMLoadResult loadWithRealOpenMS(const std::string& /*path*/)
{
    OMLoadResult r;
    r.ok    = false;
    r.count = 0;
    r.error = "OpenMS::MzMLFile not available (bioconda env not found at build time)";
    return r;
}

} // namespace Internal
} // namespace OpenMS
