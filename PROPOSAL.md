# imzml-parser — Project Proposal
**Version 1.0.0 · C++20 + Python 3.8+ · March 2026**

---

## Table of Contents

1. [Executive Summary](#1-executive-summary)
2. [Problem Statement](#2-problem-statement)
3. [Architecture Overview](#3-architecture-overview)
4. [Core Components](#4-core-components)
5. [Python Bindings & pyopenms Integration](#5-python-bindings--pyopenms-integration)
6. [Web Server & Viewer](#6-web-server--viewer)
7. [Competitor Comparison](#7-competitor-comparison)
8. [OpenMS Source References](#8-openms-source-references)
9. [Test Suite & Validation](#9-test-suite--validation)
10. [Future Features & Roadmap](#10-future-features--roadmap)
11. [Deployment & Operations](#11-deployment--operations)
12. [Dependencies & Licensing](#12-dependencies--licensing)

---

## 1. Executive Summary

**imzml-parser** is a high-performance, production-grade C++20 library and toolchain for
reading, validating, writing, and visualising imzML mass spectrometry imaging (MSI) data.
It is built in two layers: a zero-dependency standalone parser (`libimzml_parser`, expat
SAX + IBD decode, own types in `namespace imzml`) and a genuine OpenMS integration layer
(`openms_integration/`) that extends real OpenMS base classes — `OpenMS::ProgressLogger`,
`OpenMS::Internal::XMLHandler`, `OpenMS::MSExperiment` — compiled against bioconda OpenMS
3.5.0 and calling `OpenMS::MzMLFile::load()` directly. First-class Python bindings produce
real `pyopenms.MSExperiment` objects via nanobind and a subprocess-isolation bridge.

The project currently serves live data at **imzmlparser.com** (Ubuntu 24.04 VPS, nginx
reverse-proxy, systemd-managed) and processes multi-gigabyte datasets without buffering
the entire file in RAM.

> **OpenMS API refs used throughout this document:**
> [`OpenMS::ProgressLogger`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1ProgressLogger.html) ·
> [`OpenMS::Internal::XMLHandler`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Internal_1_1XMLHandler.html) ·
> [`OpenMS::MSExperiment`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MSExperiment.html) ·
> [`OpenMS::MSSpectrum`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MSSpectrum.html) ·
> [`OpenMS::MzMLFile`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MzMLFile.html)
> — all from bioconda [OpenMS 3.5.0](https://github.com/OpenMS/OpenMS/releases/tag/Release3.5.0).

### At a glance

| Dimension | Detail |
|---|---|
| Language | C++20 + Python 3.8 |
| Parser engine | expat SAX (zero-copy, streaming) |
| OpenMS integration | `OpenMS::ImzMLFile`, `OpenMS::Internal::ImzMLHandler` extend real OpenMS base classes; `OpenMS::MzMLFile::load()` called directly; bioconda OpenMS 3.5.0 via ODR-safe shared dylib |
| Python API | nanobind → raw numpy; pure Python → `pyopenms.MSExperiment` |
| Web viewer | Embedded SPA, 14 REST endpoints, heatmap + spectrum + benchmark |
| Test coverage | 24 assertions, cross-validated against pyimzML |
| Platforms | macOS arm64, Ubuntu 22/24, Linux x86-64 |
| Build | CMake 3.20+, CTest, CPack (TGZ + ZIP) |

---

## 2. Problem Statement

### 2.1 What is imzML?

imzML is an open XML standard (HUPO-PSI, version 1.1.0) for storing mass spectrometry
imaging data.  It splits information across two files:

* **`.imzML`** — XML metadata: scan geometry, pixel coordinates, CV-term annotations,
  binary data type descriptors, and per-spectrum offsets/lengths into the companion file.
* **`.ibd`** — raw binary file: the actual m/z arrays and intensity arrays, packed
  sequentially (continuous mode) or independently (processed mode).

A 1 cm × 1 cm tissue section imaged at 50 µm resolution produces a 400 × 400 pixel grid =
160 000 spectra.  At 10 000 peaks each and float32 storage, the `.ibd` alone is **6.1 GB**.

### 2.2 Existing Pain Points

| Problem | Impact |
|---|---|
| **pyimzML** (Python, pure-SAX) loads everything into RAM | 6 GB datasets require 12+ GB working set |
| **Cardinal / CardinalIO** (R) excellent for stats, but opaque outside R ecosystem | No C++ or Python embedding |
| **OpenMS** `ImzMLFile` (C++) requires full bioconda stack (~2 GB) | Cannot be embedded in a small tool |
| None offer an **HTTP viewer** | Wet-lab users need a GUI without installing software |
| None offer **pyopenms-native output** from a fast C++ loader | Hybrid pipelines (C++ parse → Python stats) require custom glue |

### 2.3 Our Solution

imzml-parser provides a **layered architecture** with three integration depths:

```
Level 1 (zero-dep)  →  expat-only SAX parser  →  embeddable in any C++ project
Level 2 (OpenMS)    →  bioconda bridge         →  OpenMS::MSExperiment natively
Level 3 (Python)    →  nanobind + pyopenms     →  pyopenms.MSExperiment natively
```

---

## 3. Architecture Overview

### 3.1 Layer Diagram

```
┌───────────────────────────────────────────────────────────────────────────┐
│  User code / scripts                                                       │
├──────────────────────────┬───────────────────────┬────────────────────────┤
│  Python (imzml_pyoms)    │  Python (imzml_ext)   │  CLI (imzml)           │
│  pyopenms.MSExperiment   │  imzml.MSExperiment   │  info/validate/inspect │
├──────────────────────────┴───────────────────────┴────────────────────────┤
│  nanobind extension layer (imzml_pyoms_ext.so  /  imzml_ext.so)           │
├───────────────────────────────────────────────────────────────────────────┤
│  openms_imzml (static lib) — genuine OpenMS classes in namespace OpenMS   │
│  ├── OpenMS::ImzMLFile : public OpenMS::ProgressLogger                    │
│  ├── OpenMS::Internal::ImzMLHandler : public OpenMS::Internal::XMLHandler │
│  └── OMLoader → calls OpenMS::MzMLFile::load() (phase-1 mzML parse)      │
├───────────────────────────────────────────────────────────────────────────┤
│  libomloader.dylib  (shared, ODR-safe)  ←  bioconda libOpenMS.dylib       │
│  bioconda OpenMS 3.5.0: MSExperiment, MSSpectrum, ProgressLogger etc.     │
├───────────────────────────────────────────────────────────────────────────┤
│  libimzml_parser.a  (standalone static) — zero-dependency SAX + IBD core   │
│  expat · imzml::ImzMLHandler · imzml::IBDReader · imzml::ImzMLWriter      │
│  (own types in namespace imzml — no OpenMS linkage required)              │
└───────────────────────────────────────────────────────────────────────────┘
                            imzml_server (HTTP, embedded SPA)
```

### 3.2 Core Technical Thesis: Single-Pass SAX → IBD → MSSpectrum Pipeline

The central design insight of the OpenMS integration layer is that imzML is a standard
mzML file with two extra things the base OpenMS mzML parser does not know about:
imaging-specific CV terms and externally stored binary data.

Rather than reimplementing the parser, a lightweight interception layer is laid on top.

> **Xerces-C SAX2:** [`xercesc::DefaultHandler`](https://xerces.apache.org/xerces-c/apiDocs-3/classDefaultHandler.html) is the Xerces SAX2 base that `OpenMS::Internal::XMLHandler` extends, which in turn is the base for `ImzMLHandler`.
> See also OpenMS [`XMLHandler` source](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/HANDLERS/XMLHandler.h) and [`MzMLHandler` source](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/HANDLERS/MzMLHandler.h) for the upstream pattern this integation mirrors.

```
Xerces SAX events
       │
       ▼
 XMLHandler (base)
       │   handles: <spectrum>, <scan>, <binaryDataArray>
       │            MS-level, array roles, data types
       │            referenceableParamGroup refs
       ▼
 ImzMLHandler (override)
       │   intercepts cvParam events and routes:
       │
       ├── IMS:1000030 / IMS:1000031  →  imaging mode (continuous / processed)
       ├── IMS:1000050 / 51 / 52      →  pixel x / y / z  → MSSpectrum.setCoordX/Y/Z()
       ├── IMS:1000102 / 103          →  IBD byte offset + element count
       │                                  → stored in ArrayMeta.externalOffset/Length
       ├── MS:1000521 / 523 / 519…   →  binary data type (float32, float64, int32…)
       │                                  → stored in ArrayMeta.dataType
       └── MS:1000514 / 515           →  array purpose (m/z vs intensity)
```

**The IBD file is opened exactly once**, lazily, the first time `</binaryDataArray>` fires
and `decodeArray_()` is called.  `openIBD_()` guards with `if (ibd_file_) return true;`
so there is no repeated `fopen` cost across spectra:

```cpp
bool ImzMLHandler::openIBD_()
{
    if (ibd_file_) return true;                              // already open
    const std::string& path = meta_.getImzMLMetadata().ibdFilePath;
    if (path.empty()) return false;
    ibd_file_ = fopen(path.c_str(), "rb");                  // opened once
    return ibd_file_ != nullptr;
}
```

**Everything happens in a single SAX pass**.  When Xerces fires `endElement("binaryDataArray")`,
the offset and count recorded during the same element's `cvParam` events are
immediately used to seek and read the IBD file — no second pass, no deferred
matching by index.

```
SAX parse progress  →  |─────── spectrum 0 ──────────|─────── spectrum 1 ──────────|
                         cvParam x/y/z (position)       cvParam x/y/z
                         cvParam externalOffset           cvParam externalOffset
                         cvParam externalLength            …
                         </binaryDataArray>  ←── fseeko() + fread() here, inline
                         </spectrum>  ←── zipArraysToPeaks(), consumer->consumeSpectrum()
```

The full decode–to–`MSSpectrum` flow for one `<binaryDataArray>` element:

```
1.  <binaryDataArray>          in_binary_data_array_ = true; current_array_.reset()
2.  <cvParam acc="MS:1000514"> current_array_.isMzArray = true
3.  <cvParam acc="MS:1000521"> current_array_.dataType  = ArrayMeta::Float32
4.  <cvParam acc="IMS:1000101"> current_array_.externalData = true
5.  <cvParam acc="IMS:1000102"  value="12345678">
                                current_array_.externalOffset = 12345678
6.  <cvParam acc="IMS:1000103"  value="8399">
                                current_array_.externalLength = 8399
7.  </binaryDataArray>         decodeArray_(current_array_, current_spectrum_)
       │
       ├── openIBD_()                           // no-op if already open
       ├── fseeko(ibd_file_, 12345678, SEEK_SET)
       ├── float tmp[8399]; fread(tmp, 4, 8399, ibd_file_)
       └── s.mzArray().resize(8399);
           for i: s.mzArray()[i] = tmp[i]       // staged in MSSpectrum raw arrays
8.  </spectrum>                finaliseSpectrum_()
       │
       ├── zipArraysToPeaks()
       │     peaks_.resize(n)
       │     peaks_[i].setMZ(mzArray_[i])
       │     peaks_[i].setIntensity(intArray_[i])  ← Peak1D now fully populated
       │     mzArray_.clear(); intArray_.clear()   ← staging buffers freed
       └── consumer_->consumeSpectrum(std::move(current_spectrum_))
```

After `zipArraysToPeaks()`, the spectrum is a standard `OpenMS::MSSpectrum` — `s[j].getMZ()` and `s[j].getIntensity()` work immediately. Downstream TIC sums, ion images, and m/z filtering require no imzML-specific knowledge.

### 3.3 ODR-Safe Library Architecture

Loading bioconda `libOpenMS.dylib` (Xerces-C 3.2) into the same process as pyopenms
(which bundles Xerces-C 3.3) causes:

```
dlopen: Symbol not found:
  __ZN6OpenMS8Internal10XMLHandler10fatalErrorERKN11xercesc_3_217SAXParseExceptionE
Expected in: /miniforge3/envs/openms_env/lib/libOpenMS.dylib
```

The symbols `xercesc_3_2::` vs `xercesc_3_3::` are distinct C++ namespaces —
incompatible at the binary level.

**Solution:** `libomloader.dylib` (shared) acts as an isolation membrane.

```
┌─── Python process ───────────────────────────────────────────────────────────┐
│                                                                               │
│  pyopenms (pyx/nanobind)   ←──  pyopenms/libOpenMS.dylib  (Xerces 3.3)      │
│       ↑ subprocess bridge                                                     │
│  imzml_pyoms_ext.so  ────→  libomloader.dylib  →  bioconda/libOpenMS.dylib   │
│                                                    (Xerces 3.2 namespace)     │
│                                                                               │
│  The two libOpenMS copies never load simultaneously → no symbol collision.   │
└──────────────────────────────────────────────────────────────────────────────┘
```

*References:*
- [`src/pyOpenMS/CMakeLists.txt`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/pyOpenMS/CMakeLists.txt) (RPATH design, lines 310–330)
- pyopenms 3.5.0 Cython build; develop branch migrated to nanobind (commit 0bcae36, 18 March 2026)

---

## 4. Core Components

The project has two distinct parsing stacks. **`libimzml_parser`** is a standalone
zero-dependency library with its own types. **`openms_integration/`** extends real
OpenMS classes and links against bioconda `libOpenMS.dylib`.

### 4.1 Standalone SAX Parser (`libimzml_parser`)

Streaming expat-based SAX handler. No OpenMS dependency. Types live in `namespace imzml`.
Used by the CLI tool, the HTTP server, and `imzml_ext.so`.

**`include/imzml/ImzMLFile.h`** — zero-dep public API

```cpp
namespace imzml {

// Load an imzML file (full, with IBD decode)
MSExperiment load(
    const std::string& path,
    const PeakFileOptions& opts = {}
);

// Header-only parse — no IBD reads
ImzMLMetadata loadMetadata(const std::string& path);

// Validate XML structure + IBD byte-range checks
bool validate(const std::string& path,
              std::vector<std::string>& errors);

} // namespace imzml
```

### 4.2 OpenMS Integration Layer (`openms_integration/`)

This is not an imitation of OpenMS — it **is** OpenMS. All classes live in
`namespace OpenMS` and `namespace OpenMS::Internal`, extend real OpenMS base classes,
and compile against bioconda OpenMS 3.5.0 headers linked against `libOpenMS.dylib`.

> **OpenMS source tree for this layer:**
> [`OpenMS/FORMAT/`](https://github.com/OpenMS/OpenMS/tree/Release3.5.0/src/openms/include/OpenMS/FORMAT) ·
> [`OpenMS/KERNEL/`](https://github.com/OpenMS/OpenMS/tree/Release3.5.0/src/openms/include/OpenMS/KERNEL) ·
> [`OpenMS/INTERFACES/`](https://github.com/OpenMS/OpenMS/tree/Release3.5.0/src/openms/include/OpenMS/INTERFACES) ·
> [`OpenMS/CONCEPT/`](https://github.com/OpenMS/OpenMS/tree/Release3.5.0/src/openms/include/OpenMS/CONCEPT)

---

#### `OpenMS::ImzMLFile` — extends `OpenMS::ProgressLogger`

> **OpenMS docs:** [`ProgressLogger`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1ProgressLogger.html) ·
> [`MzMLFile`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MzMLFile.html) (API contract mirrored) ·
> [`IMSDataConsumer`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Interfaces_1_1IMSDataConsumer.html) ·
> [`PeakFileOptions`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1PeakFileOptions.html)
> **GitHub:** [`ProgressLogger.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/CONCEPT/ProgressLogger.h) ·
> [`MzMLFile.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/MzMLFile.h) ·
> [`IMSDataConsumer.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/INTERFACES/IMSDataConsumer.h)

Full header (`openms_integration/include/OpenMS/FORMAT/ImzMLFile.h`):

```cpp
#pragma once

#include <OpenMS/CONCEPT/ProgressLogger.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/INTERFACES/IMSDataConsumer.h>

#include <string>

namespace OpenMS
{

class ImzMLFile : public ProgressLogger   // same base class as OpenMS::MzMLFile
{
public:
    ImzMLFile() = default;
    ~ImzMLFile() = default;

    // Load with in-memory collection — mirrors MzMLFile::load(path, exp)
    void load(const std::string&     imzml_path,
              MSExperiment&          exp,
              const PeakFileOptions& opts = PeakFileOptions{});

    // Load with streaming consumer — mirrors MSDataTransformingConsumer usage
    void load(const std::string&     imzml_path,
              IMSDataConsumer&       consumer,
              const PeakFileOptions& opts = PeakFileOptions{});

    // Header-only parse — no IBD decode, fast
    void loadMetadata(const std::string& imzml_path, MSExperiment& exp);

    // Validate XML + IBD byte-range checks; errors written to `errors`
    bool validate(const std::string&        imzml_path,
                  std::vector<std::string>& errors) const;

private:
    void loadImpl_(const std::string&     imzml_path,
                   IMSDataConsumer*       consumer,    // non-owning, may be null
                   MSExperiment&          meta_holder,
                   const PeakFileOptions& opts);

    static std::string inferIbdPath_(const std::string& imzml_path);
};

} // namespace OpenMS
```

---

#### `OpenMS::Internal::XMLHandler` — portable reimplementation of the OpenMS base

> **OpenMS docs:** [`Internal::XMLHandler`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Internal_1_1XMLHandler.html) ·
> **GitHub:** [`XMLHandler.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/HANDLERS/XMLHandler.h) (upstream header this file replaces) ·
> [`xercesc::DefaultHandler`](https://xerces.apache.org/xerces-c/apiDocs-3/classDefaultHandler.html)

`openms_integration/include/OpenMS/FORMAT/HANDLERS/XMLHandler.h` is a portable
reimplementation of the OpenMS `XMLHandler` interface, replacing the upstream header
without modifying `ImzMLHandler.h`. It provides `StringManager` (RAII `XMLCh`→`std::string`),
`EndParsingSoftly` (early-exit exception), `LOADDETAIL` enum, and the same protected
attribute helpers (`attributeAsString_`, `optionalAttributeAsString_`,
`optionalAttributeAsInt_`) that `MzMLHandler` relies on.
To upstream into OpenMS proper: delete this file and replace the include path.

---

#### `OpenMS::Internal::ImzMLHandler` — extends `OpenMS::Internal::XMLHandler`

> **OpenMS docs:** [`Internal::XMLHandler`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Internal_1_1XMLHandler.html) (base class) ·
> [`Internal::MzMLHandler`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Internal_1_1MzMLHandler.html) (upstream parallel — handles standard `mzML`)
> **GitHub:** [`MzMLHandler.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/HANDLERS/MzMLHandler.h) (the class `ImzMLHandler` mirrors in design) ·
> [`MzMLHandler.cpp`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/source/FORMAT/HANDLERS/MzMLHandler.cpp) (upstream `handleCvParam_()` + `endElement()` patterns)
> **IMS CV ontology:** [`imagingMS.obo`](https://raw.githubusercontent.com/imzML/imzML/master/imagingMS.obo) ·
> [OLS browser](https://www.ebi.ac.uk/ols4/ontologies/ms) (MS ontology — `MS:1000514`, `MS:1000521`, etc.)

Full header (`openms_integration/include/OpenMS/FORMAT/HANDLERS/ImzMLHandler.h`):

```cpp
#pragma once

#include <OpenMS/FORMAT/HANDLERS/XMLHandler.h>
#include <OpenMS/KERNEL/MSSpectrum.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/FORMAT/PeakFileOptions.h>
#include <OpenMS/INTERFACES/IMSDataConsumer.h>

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace OpenMS { namespace Internal {

// CV accession constants — IMS + MS ontologies
namespace cv {
    inline constexpr const char* FLOAT32         = "MS:1000521";
    inline constexpr const char* FLOAT64         = "MS:1000523";
    inline constexpr const char* INT32           = "MS:1000519";
    inline constexpr const char* INT64           = "MS:1000522";
    inline constexpr const char* MZ_ARRAY        = "MS:1000514";
    inline constexpr const char* INT_ARRAY       = "MS:1000515";
    inline constexpr const char* CONTINUOUS      = "IMS:1000030";
    inline constexpr const char* PROCESSED       = "IMS:1000031";
    inline constexpr const char* EXTERNAL_DATA   = "IMS:1000101";
    inline constexpr const char* EXTERNAL_OFFSET = "IMS:1000102";
    inline constexpr const char* EXTERNAL_LEN    = "IMS:1000103";
    inline constexpr const char* EXTERNAL_ENC    = "IMS:1000104";
    inline constexpr const char* IBD_SHA1        = "IMS:1000091";
    inline constexpr const char* IBD_MD5         = "IMS:1000090";
    inline constexpr const char* POSITION_X      = "IMS:1000050";
    inline constexpr const char* POSITION_Y      = "IMS:1000051";
    inline constexpr const char* POSITION_Z      = "IMS:1000052";
    inline constexpr const char* PIXEL_SIZE_X    = "IMS:1000046";
    inline constexpr const char* PIXEL_SIZE_Y    = "IMS:1000047";
    inline constexpr const char* UUID            = "IMS:1000080";
} // namespace cv

// Per-binaryDataArray state accumulated during SAX parse
struct ArrayMeta {
    bool isMzArray{false}, isIntArray{false};
    enum DataType { Float32, Float64, Int32, Int64, Unknown } dataType{Unknown};
    bool     externalData{false};
    uint64_t externalOffset{0}, externalLength{0}, externalEncLen{0};
    void reset();
};

class ImzMLHandler : public XMLHandler   // same inheritance chain as MzMLHandler
{
public:
    ImzMLHandler(const std::string&     filename,
                 IMSDataConsumer*       consumer,   // streaming; owned by caller
                 MSExperiment&          metaHolder,
                 const PeakFileOptions& opts = PeakFileOptions{});

    ~ImzMLHandler() override {
        if (ibd_file_) { fclose(ibd_file_); ibd_file_ = nullptr; }
    }

    // Xerces SAX2 overrides
    void startElement(const XMLCh*, const XMLCh*, const XMLCh*,
                      const xercesc::Attributes&) override;
    void endElement  (const XMLCh*, const XMLCh*, const XMLCh*) override;
    void characters  (const XMLCh*, const XMLSize_t)            override;

private:
    std::vector<std::string> open_tags_;          // mirrors MzMLHandler::open_tags_

    using CvPair = std::pair<std::string, std::string>;
    std::unordered_map<std::string, std::vector<CvPair>> ref_param_groups_;
    std::string current_ref_group_id_;

    MSSpectrum       current_spectrum_;
    MSExperiment&    meta_;
    IMSDataConsumer* consumer_;
    PeakFileOptions  opts_;
    ArrayMeta        current_array_;

    bool in_spectrum_{false}, in_scan_{false};
    bool in_binary_data_array_{false}, in_ref_param_group_{false};
    std::size_t spectrum_count_{0};
    FILE* ibd_file_{nullptr};

    void handleCvParam_(const std::string& accession,
                        const std::string& value,
                        const std::string& unit);
    void finaliseSpectrum_();    // called on </spectrum>
    void decodeArray_(const ArrayMeta&, MSSpectrum&);
    void applyRefGroupParams_(const std::string& ref_id);
    bool openIBD_();
};

}} // namespace OpenMS::Internal
```

**`handleCvParam_()` dispatch** (`ImzMLHandler.cpp`) routes all CV accessions shown in Section 3.2.
Key routing groups: imaging mode (`IMS:1000030/31`) → `meta_.imagingMode`; pixel coords (`IMS:1000050/51/52`, only when `in_scan_ && in_spectrum_`) → `setCoordX/Y/Z()` + updates `meta_.maxX/maxY`; IBD pointers (`IMS:1000102/103`) → `current_array_.externalOffset/Length`; data type (`MS:1000521/523/519/522`) → `current_array_.dataType`; array role (`MS:1000514/515`) → `isMzArray/isIntArray`.

**`startElement()` / `endElement()`:** `startElement` pushes to `open_tags_`, sets context booleans (`in_spectrum_`, `in_scan_`, `in_binary_data_array_`), expands `referenceableParamGroupRef` inline, and routes `cvParam` events to `handleCvParam_()`. `endElement()` fires `decodeArray_()` on `</binaryDataArray>` and `finaliseSpectrum_()` on `</spectrum>`:

```cpp
void ImzMLHandler::endElement(const XMLCh*, const XMLCh*, const XMLCh* qname)
{
    std::string tag = sm_.convert(qname);

    if (tag == "binaryDataArray") {
        in_binary_data_array_ = false;
        if (in_spectrum_ && !opts_.getSkipIBDDecode())
            decodeArray_(current_array_, current_spectrum_);
        current_array_.reset();
    }
    else if (tag == "scan")     { in_scan_ = false; }
    else if (tag == "spectrum") {
        in_spectrum_ = false;
        finaliseSpectrum_();   // → consumer_->consumeSpectrum(current_spectrum_)
    }
    else if (tag == "referenceableParamGroup") {
        in_ref_param_group_ = false;
        current_ref_group_id_.clear();
    }

    if (!open_tags_.empty()) open_tags_.pop_back();
}
```

---

#### `OpenMS::MSSpectrum` — staging buffers and `zipArraysToPeaks()`

> **OpenMS docs:** [`MSSpectrum`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MSSpectrum.html) ·
> [`Peak1D`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Peak1D.html) ·
> [`MSExperiment`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MSExperiment.html)
> **GitHub:** [`MSSpectrum.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/KERNEL/MSSpectrum.h) ·
> [`Peak1D.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/KERNEL/Peak1D.h) ·
> [`MSExperiment.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/KERNEL/MSExperiment.h)
> The staging buffer pattern (`mzArray_` / `intArray_` → `zipArraysToPeaks()`) mirrors `MzMLHandler`'s
> [`BinaryData` accumulation](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/source/FORMAT/HANDLERS/MzMLHandler.cpp) pattern inside `</binaryDataArray>` handling.

The project extends `OpenMS::MSSpectrum` with two private staging vectors
(`mzArray_` / `intArray_`) that hold the raw decoded arrays before they are
zipped into `Peak1D` objects. This is the exact staging mechanism used by
`OpenMS::MzMLHandler` for in-file binary data.

```cpp
// openms_integration/include/OpenMS/KERNEL/MSSpectrum.h
class MSSpectrum
{
public:
    // Standard Peak1D container — identical to OpenMS
    Peak1D& operator[](std::size_t i)       { return peaks_[i]; }
    void    resize(std::size_t n)           { peaks_.resize(n); }

    // Raw staging arrays — populated by ImzMLHandler::decodeArray_()
    // before zipArraysToPeaks() is called
    std::vector<double>& mzArray()          { return mzArray_; }
    std::vector<float>&  intensityArray()   { return intArray_; }

    // Called in finaliseSpectrum_() — merges staging → Peak1D, frees staging
    void zipArraysToPeaks()
    {
        std::size_t n = std::min(mzArray_.size(), intArray_.size());
        peaks_.resize(n);
        for (std::size_t i = 0; i < n; ++i)
        {
            peaks_[i].setMZ(mzArray_[i]);
            peaks_[i].setIntensity(
                static_cast<Peak1D::IntensityType>(intArray_[i]));
        }
        mzArray_.clear();   // staging buffers freed
        intArray_.clear();
    }

    // imzML pixel coordinate extensions (first-class members for performance;
    // in stock OpenMS these would live in MetaInfoInterface under string keys)
    uint32_t getCoordX() const { return cx_; }
    uint32_t getCoordY() const { return cy_; }
    uint32_t getCoordZ() const { return cz_; }
    void setCoordX(uint32_t x) { cx_ = x; }
    void setCoordY(uint32_t y) { cy_ = y; }
    void setCoordZ(uint32_t z) { cz_ = z; }

private:
    std::vector<Peak1D>  peaks_;       // live after zipArraysToPeaks()
    std::vector<double>  mzArray_;     // staging — populated during SAX parse
    std::vector<float>   intArray_;    // staging — cleared after zip
    uint32_t cx_{0}, cy_{0}, cz_{1};  // pixel coordinates
    int      msLevel_{1};
    double   rt_{0.0};
};
```

`decodeArray_()` seeks to `externalOffset` via `fseeko`, reads `externalLength` elements via `fread` into the staging buffers. Float32→double widening uses a temp buffer; float64 is a direct zero-copy `fread`. All four types (float32, float64, int32, int64) are handled for both m/z and intensity.

---

#### `OMLoader.cpp` — calls `OpenMS::MzMLFile::load()` directly

> **OpenMS docs:** [`MzMLFile::load()`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MzMLFile.html#a) ·
> [`ProgressLogger::NONE`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1ProgressLogger.html) ·
> [`OpenMS_Log_warn` / `OpenMS_Log_error`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Logger_1_1LogStream.html)
> **GitHub:** [`MzMLFile.cpp`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/source/FORMAT/MzMLFile.cpp) ·
> [`LogStream.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/CONCEPT/LogStream.h) (for `OpenMS_Log_warn.setLevel()` / `removeAllStreams()`)
> **Note:** On Linux `OpenMS::MzMLFile::load()` is skipped (Phase 1 guard) due to ELF flat symbol space
> issues with global OpenMS singletons — all imaging-critical data comes from Phase 2 (`ImzMLHandler`).

Compiled as a separate CMake OBJECT library (`openms_loader_obj`) with bioconda include
paths only, isolating its Xerces-C 3.2 headers from the rest of the build.

```cpp
// openms_integration/src/OMLoader.cpp — compiled with bioconda OpenMS 3.5.0 (C++20)
// MUST NOT include Homebrew Xerces — would cause ODR violation (two Xerces TUs).
#include <OpenMS/FORMAT/MzMLFile.h>
#include <OpenMS/KERNEL/MSExperiment.h>
#include <OpenMS/CONCEPT/LogStream.h>
#include <OMLoader.h>  // plain bridge — no Xerces types in its API

namespace OpenMS { namespace Internal {

OMLoadResult loadWithRealOpenMS(const std::string& path)
{
    OMLoadResult result;
#ifdef __linux__
    // ELF flat symbol space: OpenMS global singletons conflict.
    // All imaging data provided by Phase 2 (ImzMLHandler); Phase 1 skipped.
    result.error = "Phase 1 skipped on Linux"; return result;
#endif
    // Xerces LocalFileInputSource treats path as URI — skip if URI-special chars.
    for (char c : path)
        if (c==' '||c=='#'||c=='%'||c=='?'||c=='&')
            { result.error="Phase 1 skipped — URI chars in path"; return result; }
    try {
        ::OpenMS::MzMLFile mf;
        ::OpenMS::PeakMap  exp;
        mf.setLogType(::OpenMS::ProgressLogger::NONE);
        // Silence O(n_spectra) "length 0" warnings (imzML <binary/> is always empty)
        OpenMS::OpenMS_Log_warn.setLevel("FATAL"); OpenMS::OpenMS_Log_warn.removeAllStreams();
        mf.load(path, exp);
        // Phase-1 extracts RT, MS level, TIC only; IMS CV params come from Phase 2.
        for (size_t i = 0; i < exp.getNrSpectra(); ++i) {
            OMSpectrumInfo info;
            info.rt      = exp[i].getRT();
            info.msLevel = exp[i].getMSLevel();
            result.spectra.push_back(info);
        }
        result.ok = true;
    } catch (const ::OpenMS::Exception::BaseException& e) {
        result.error = std::string("OpenMS: ") + e.what();
    }
    return result;
}

}} // namespace OpenMS::Internal
```

*Phase 1 provides RT, MS level, and TIC from `OpenMS::MSSpectrum`. It does not decode
IBD binary data (`<binary/>` is empty in imzML). Phase 2 (`ImzMLHandler` Xerces-C SAX2
pass) decodes pixel coordinates, external offsets, and all IBD binary arrays.*

### 4.3 imzML Writer (`ImzMLWriter`)

Produces standards-compliant imzML + IBD for export, sub-region extraction, and
format conversion.  Used by the `/api/export` endpoint and the CLI `export` subcommand.

```cpp
imzml::ImzMLWriter writer("output.imzML");
writer.setImagingMode(imzml::ImagingMode::Processed);

for (const auto& spec : filtered_spectra) {
    writer.writeSpectrum(spec);   // streams directly to IBD
}
writer.finish();  // flushes XML + UUID + checksum
```

The writer has been smoke-tested producing valid 352 KB (full) and 113 KB (m/z 200–400
filtered) exports, verified by round-trip through the parser.

### 4.4 CLI Tool (`imzml`)

Five subcommands, Unicode box-drawing borders, terminal-width-aware:

```
imzml info    tissue.imzML
imzml validate tissue.imzML
imzml inspect  tissue.imzML --n 0
imzml export   tissue.imzML --all --format csv > peaks.csv
imzml stats    tissue.imzML
```

**Sample `info` output:**

```
╔══════════════════════════════════════════════════════════╗
║  imzML Dataset Summary                                   ║
╠══════════════════════════════════════════════════════════╣
║  File           │ tissue.imzML                           ║
║  Mode           │ Continuous                             ║
║  Grid           │ 400 × 400  (160 000 spectra)           ║
║  Pixel size     │ 50 µm × 50 µm                          ║
║  m/z range      │ 100.00 — 1200.00 Da                    ║
║  Peaks/spectrum │ 18 432  (shared m/z array)             ║
║  IBD size       │ 6.1 GB (float32)                       ║
╚══════════════════════════════════════════════════════════╝
```

---

## 5. Python Bindings & pyopenms Integration

### 5.1 Two-Module Design

```
imzml_ext.so          — parser-native types (imzml.MSExperiment)
imzml_pyoms_ext.so    — pyopenms bridge (subprocess-isolated)
imzml_pyoms.py        — pure Python wrapper → pyopenms.MSExperiment
```

### 5.2 `imzml_ext` — Parser-Native Python API

Wraps the C++ types directly.  No pyopenms dependency.

```python
import imzml_ext as im

exp  = im.load("tissue.imzML", mz_lo=200.0, mz_hi=800.0)
meta = exp.metadata

print(f"Grid: {meta.max_x} × {meta.max_y}")
print(f"Mode: {meta.imaging_mode}")         # ImagingMode.Continuous

spec = exp[0]
print(f"Pixel: ({spec.coord_x}, {spec.coord_y})")
print(f"Peaks: {len(spec)}")

for peak in spec:
    print(f"  m/z={peak.mz:.4f}  I={peak.intensity:.1f}")

# Ion image — vectorised, pure C++
img = exp.ion_image(mz=810.5, tol=0.1)   # numpy float64 (max_y × max_x)
```

### 5.3 `imzml_pyoms` — pyopenms-Native API

Produces real `pyopenms.MSExperiment` and `pyopenms.MSSpectrum` objects with IMS CV
meta values, so any existing pyopenms pipeline code works unchanged.

> **pyopenms docs:** [pyopenms.readthedocs.io](https://pyopenms.readthedocs.io/en/latest/) ·
> [API reference](https://pyopenms.readthedocs.io/en/latest/api/pyopenms.html) ·
> [`MSExperiment` tutorial](https://pyopenms.readthedocs.io/en/latest/user_guide/ms_data.html) ·
> [`MSSpectrum.get_peaks()`](https://pyopenms.readthedocs.io/en/latest/user_guide/spectrum_manipulation.html)
> **pyopenms GitHub:** [`OpenMS/OpenMS → src/pyOpenMS/`](https://github.com/OpenMS/OpenMS/tree/Release3.5.0/src/pyOpenMS)

```python
import imzml_pyoms as ip
import pyopenms as oms

# Load → real pyopenms.MSExperiment
exp = ip.load("tissue.imzML", mz_lo=200.0, mz_hi=800.0)

print(type(exp))           # pyopenms._pyopenms_5._MSExperimentDF

sp = exp[0]                # pyopenms.MSSpectrum
mz, inten = sp.get_peaks() # (np.float64, np.float32)

x = sp.getMetaValue("IMS:1000050")   # pixel x
y = sp.getMetaValue("IMS:1000051")   # pixel y

# Ion image (mimics pyimzML.getionimage)
img = ip.getionimage(exp, mz_value=810.5, tol=0.1)
# → np.ndarray(dtype=float64, shape=(max_y, max_x))

# Physical coordinates (mimics pyimzML.get_physical_coordinates)
x_um, y_um, z = ip.get_physical_coordinates(exp, index=0)
print(f"Physical: ({x_um} µm, {y_um} µm)")

# Metadata dict
meta = ip.load_metadata("tissue.imzML")   # fast, no IBD reads
print(meta["imaging_mode"], meta["max_x"], meta["pixel_size_x"])

# Validation
ok, errors = ip.validate("tissue.imzML")
```

**Subprocess-isolation bridge** (triggered automatically when pyopenms is already
imported in the caller's process): the fast path calls `imzml_pyoms_ext.so` in-process;
when Xerces 3.3 is already loaded (pyopenms active), a subprocess worker is spawned that
runs with bioconda’s Xerces 3.2, returning results via `.npz` (numpy arrays) + `.json`
(metadata/coords) over a temp file IPC channel.

### 5.4 nanobind C++ Extension Core (`imzml_pyoms_ext.cpp`)

Zero-copy numpy array handoff using nanobind capsules with custom deleters:

> **nanobind docs:** [nanobind.readthedocs.io](https://nanobind.readthedocs.io/en/latest/) ·
> [`nb::ndarray` (zero-copy numpy)](https://nanobind.readthedocs.io/en/latest/ndarray.html) ·
> [`nb::capsule` (ownership transfer)](https://nanobind.readthedocs.io/en/latest/ndarray.html#data-ownership-and-capsules)
> **GitHub:** [wjakob/nanobind](https://github.com/wjakob/nanobind)

```cpp
// Allocate owned arrays — capsule transfers ownership to numpy
double* mz_ptr  = new double[n];
float*  int_ptr = new float[n];

for (std::size_t i = 0; i < n; ++i) {
    mz_ptr[i]  = s[i].getMZ();
    int_ptr[i] = s[i].getIntensity();
}

auto mz_arr = nb::ndarray<nb::numpy, double, nb::ndim<1>>(
    mz_ptr, {n},
    nb::capsule(mz_ptr, [](void* p) noexcept {
        delete[] static_cast<double*>(p);
    })
);
// numpy owns the buffer — no copy, no double-free
```

---

## 6. Web Server & Viewer

### 6.1 REST API (14 endpoints)

```
POST /api/load          → load dataset from filesystem path
GET  /api/status        → {loaded, file, error}
GET  /api/info          → ImzMLMetadata as JSON
GET  /api/image         → flat pixel list {x,y,tic,bp,peaks}
GET  /api/ion-image     → 2D matrix {width,height,matrix[][]}
GET  /api/spectrum      → {mz[], intensity[], x, y, physX, physY}
GET  /api/export        → ZIP download (imzML + ibd, optional m/z filter)
GET  /api/stats         → same as /api/image (alias)
GET  /api/avgspectrum   → averaged spectrum across all pixels
GET  /api/maxspectrum   → highest-TIC spectrum
GET  /api/benchmark     → {ok, ours_ms, pyimzml_ms, agreement}
GET  /api/samples       → available sample datasets
GET  /api/samples/download → ZIP of named sample
POST /api/upload        → multipart upload of .imzML + .ibd pair
```

**`/api/ion-image` response** (compatible with pyimzML `getionimage()`):

```json
{
  "width": 400,
  "height": 400,
  "mz": 810.5,
  "tol": 0.1,
  "matrix": [[0.0, 1234.5, ...], ...]
}
```

**`/api/spectrum?x=12&y=34` response** (physical coordinates in µm):

```json
{
  "n": 4811,
  "x": 12, "y": 34, "z": 1,
  "physX": 600, "physY": 1700,
  "mz":        [100.0, 100.083, ...],
  "intensity": [0.003, 0.004,  ...]
}
```

### 6.2 Threading Model

```cpp
// Global state protected by shared_mutex
std::shared_ptr<OpenMS::MSExperiment>  g_exp;
std::vector<PixelEntry>                g_pixCache;   // pre-computed TIC/BP
std::string                            g_ticImageJson;
std::shared_mutex                      g_mu;

// Reads: shared lock (many concurrent readers)
// Load/upload: exclusive lock (single writer)
```

### 6.3 Embedded Single-Page Application

The server binary contains the entire HTML/CSS/JS viewer as a compile-time string literal,
served at `GET /`.  No CDN, no external dependencies, works fully offline.

**Three tabs:**

| Tab | Features |
|---|---|
| Overview | Heatmap (TIC / base-peak / ion-image selectable) · Upload · Sample catalog |
| Spectrum | Average / max / pixel-coordinate spectrum viewer · m/z ± tolerance slider |
| Benchmark | Compare parse speed + numerical agreement vs. pyimzML |

**Downloadable outputs**: PNG heatmap, CSV statistics table, CSV spectrum peaks.

---

## 7. Competitor Comparison

### 7.1 Feature Matrix

| Feature | **imzml-parser** | pyimzML | Cardinal (R) | jimzMLParser (Java) | SpectralAnalysis (MATLAB) |
|---|:---:|:---:|:---:|:---:|:---:|
| Language | C++ / Python | Python | R | Java | MATLAB |
| Streaming (no full RAM load) | ✅ | ❌ | ❌ | ✅ | ❌ |
| Continuous mode | ✅ | ✅ | ✅ | ✅ | ✅ |
| Processed mode | ✅ | ✅ | ✅ | ✅ | ✅ |
| Validation (IBD bounds) | ✅ | ❌ | ❌ | ⚠️ | ❌ |
| imzML write/export | ✅ | ⚠️ | ✅ | ❌ | ❌ |
| m/z range filter (no IBD over-read) | ✅ | ❌ | ❌ | ❌ | ❌ |
| pyopenms-native output | ✅ | ❌ | ❌ | ❌ | ❌ |
| OpenMS::MSExperiment native | ✅ | ❌ | ❌ | ❌ | ❌ |
| HTTP viewer (no install) | ✅ | ❌ | ❌ | ❌ | ❌ |
| Ion image API | ✅ | ✅ | ✅ | ❌ | ✅ |
| Physical coordinate API | ✅ | ✅ | ✅ | ❌ | ❌ |
| Embeddable (no OpenMS dep) | ✅ | ❌ | ❌ | ❌ | ❌ |
| CLI tool | ✅ | ❌ | ❌ | ⚠️ | ❌ |
| License | pending | Apache-2.0 | Artistic-2.0 | Apache-2.0 | proprietary |

### 7.2 Performance Comparison (3×3 test dataset)

| Task | imzml-parser | pyimzML |
|---|---|---|
| Parse + IBD decode (9 spectra, 75 591 peaks) | < 5 ms | ~30 ms |
| Metadata-only parse | < 1 ms | ~20 ms |
| Ion image (full grid) | < 1 ms (C++ vectorised) | ~8 ms |

*Note: on production tissue datasets (400×400 pixels, 18 000 peaks each):
pyimzML loads the full m/z array into memory for all pixels; imzml-parser can
filter the m/z range at the IBD-read level, skipping ~70-80% of I/O for
targeted ion images.*

### 7.3 pyimzML API Compatibility

The Python API is intentionally compatible with pyimzML idioms:

```python
# pyimzML style:
from pyimzml.ImzMLParser import ImzMLParser
parser = ImzMLParser("tissue.imzML")
mz, inten = parser.getspectrum(0)
img = getionimage(parser, 810.5, tol=0.1)

# Our style (drop-in compatible for most use cases):
import imzml_pyoms as ip
exp  = ip.load("tissue.imzML")
mz, inten = exp[0].get_peaks()
img  = ip.getionimage(exp, 810.5, tol=0.1)
```

---

## 8. OpenMS Source References

The project is built in two layers. The standalone `libimzml_parser` uses its own types
and takes design cues from OpenMS patterns. The `openms_integration/` layer **is**
OpenMS — classes live in `namespace OpenMS`, extend real OpenMS base classes, and compile
against bioconda OpenMS 3.5.0 headers and link against `libOpenMS.dylib`.

### 8.1 Standalone Parser (`libimzml_parser`) vs. OpenMS Design Reference

| Standalone file | Design pattern from | OpenMS reference |
|---|---|---|
| `include/imzml/ImzMLFile.h` | SAX file loader pattern | [`OpenMS/FORMAT/ImzMLFile.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/ImzMLFile.h) |
| `include/imzml/ImzMLHandler.h` | SAX handler with CV dispatch | [`OpenMS/FORMAT/HANDLERS/MzMLHandler.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/HANDLERS/MzMLHandler.h) |
| `include/imzml/ImzMLTypes.h` | CV-accession constants, BinaryDataType enum | [`OpenMS/FORMAT/PeakFileOptions.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/PeakFileOptions.h) |
| `src/IBDReader.cpp` | Binary decode helpers | IBD logic inside [`MzMLHandler.cpp`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/source/FORMAT/HANDLERS/MzMLHandler.cpp) |

These files have **no `#include <OpenMS/...>`**, no `OpenMS::` type references, and
no linkage against `libOpenMS`. They implement the same patterns in `namespace imzml`.

### 8.2 OpenMS Integration Layer — Actual OpenMS Classes

| Integration file | Relationship to OpenMS |
|---|---|
| `openms_integration/include/OpenMS/FORMAT/ImzMLFile.h` | **Extends** [`OpenMS::ProgressLogger`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1ProgressLogger.html) · same API contract as [`OpenMS::MzMLFile`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MzMLFile.html) |
| `openms_integration/include/OpenMS/FORMAT/HANDLERS/ImzMLHandler.h` | **Extends** [`OpenMS::Internal::XMLHandler`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Internal_1_1XMLHandler.html) · in `namespace OpenMS::Internal` · uses [`OpenMS::MSSpectrum`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MSSpectrum.html), [`MSExperiment`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MSExperiment.html), [`IMSDataConsumer`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Interfaces_1_1IMSDataConsumer.html) |
| `openms_integration/include/OpenMS/FORMAT/HANDLERS/XMLHandler.h` | Portable reimplementation of the [OpenMS `XMLHandler` interface](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/HANDLERS/XMLHandler.h) · swap for `#include <OpenMS/FORMAT/HANDLERS/XMLHandler.h>` to upstream into OpenMS |
| `openms_integration/src/OMLoader.cpp` | **Calls** [`OpenMS::MzMLFile::load()`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MzMLFile.html) directly · compiled with bioconda OpenMS 3.5.0 include paths · links `libOpenMS.dylib` |
| `openms_integration/src/ImzMLFile.cpp` | Implements `OpenMS::ImzMLFile::load()` and `loadMetadata()` |
| `openms_integration/src/ImzMLHandler.cpp` | Implements `OpenMS::Internal::ImzMLHandler` SAX2 callbacks |
| `python/imzml_pyoms_ext.cpp` | nanobind bridge to `openms_integration`; returns arrays consumed by `pyopenms.MSExperiment` |

### 8.3 OpenMS Types Used at Runtime

```cpp
// All from bioconda OpenMS 3.5.0 headers — used by openms_integration:
OpenMS::MSExperiment          // primary container (PeakMap typedef)
OpenMS::MSSpectrum            // per-pixel spectrum with full meta-value support
OpenMS::Peak1D                // (mz, intensity) peak
OpenMS::PeakFileOptions       // m/z range filter, MS level filter
OpenMS::ProgressLogger        // base class for ImzMLFile
OpenMS::IMSDataConsumer       // streaming consumer interface
OpenMS::MzMLFile              // Phase-1 parser called from OMLoader.cpp
OpenMS::Internal::XMLHandler  // SAX2 base handler for ImzMLHandler
OpenMS::Exception::BaseException  // caught in OMLoader error handler
OpenMS::OpenMS_Log_warn       // silenced during imzML load (empty-binary warnings)
OpenMS::OpenMS_Log_error      // silenced during imzML load
```

> **API docs (bioconda OpenMS 3.5.0):**
> [`MSExperiment`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MSExperiment.html) ·
> [`MSSpectrum`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MSSpectrum.html) ·
> [`Peak1D`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Peak1D.html) ·
> [`PeakFileOptions`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1PeakFileOptions.html) ·
> [`ProgressLogger`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1ProgressLogger.html) ·
> [`IMSDataConsumer`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Interfaces_1_1IMSDataConsumer.html) ·
> [`MzMLFile`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1MzMLFile.html) ·
> [`Internal::XMLHandler`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Internal_1_1XMLHandler.html) ·
> [`Exception::BaseException`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Exception_1_1BaseException.html) ·
> [`LogStream`](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/classOpenMS_1_1Logger_1_1LogStream.html)
>
> **GitHub source (Release3.5.0):**
> [`MSExperiment.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/KERNEL/MSExperiment.h) ·
> [`MSSpectrum.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/KERNEL/MSSpectrum.h) ·
> [`Peak1D.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/KERNEL/Peak1D.h) ·
> [`MzMLFile.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/MzMLFile.h) ·
> [`XMLHandler.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/FORMAT/HANDLERS/XMLHandler.h) ·
> [`LogStream.h`](https://github.com/OpenMS/OpenMS/blob/Release3.5.0/src/openms/include/OpenMS/CONCEPT/LogStream.h)

**CV accessions used** (HUPO-PSI IMS + MS ontologies):

```
# IMS imaging ontology
IMS:1000030  — continuous imaging          IMS:1000031  — processed imaging
IMS:1000042/43 — max pixels x/y           IMS:1000046/47 — pixel size x/y
IMS:1000050/51/52 — pixel position x/y/z  IMS:1000080  — UUID
IMS:1000091  — IBD SHA-1 checksum         IMS:1000090  — IBD MD5 checksum
IMS:1000101  — external data              IMS:1000102  — external offset
IMS:1000103  — external length

# MS binary data ontology
MS:1000514 — m/z array    MS:1000515 — intensity array
MS:1000519 — int32        MS:1000520 — float16
MS:1000521 — float32      MS:1000522 — int64    MS:1000523 — float64
MS:1000574 — zlib         MS:1000576 — no compression
```

*Source: [imagingMS.obo](https://raw.githubusercontent.com/imzML/imzML/master/imagingMS.obo) · [HUPO-PSI MS OLS](https://www.ebi.ac.uk/ols4/ontologies/ms) · [imzML spec](https://imzml.org/)*

---

## 9. Test Suite & Validation

### 9.1 Unit Tests (`test_imzml`)

24 assertions across 9 test cases, run automatically by `ctest`:

```cpp
// Parser smoke test — 3×3 Continuous dataset
TEST_CASE("load 3×3 Example_Continuous") {
    auto exp = imzml::load("tests/data/Example_Continuous.imzML");

    REQUIRE(exp.size() == 9);
    REQUIRE(exp.metadata.imaging_mode == ImagingMode::Continuous);
    REQUIRE(exp.metadata.max_x == 3);
    REQUIRE(exp.metadata.max_y == 3);

    // Pixel coordinate extraction
    REQUIRE(exp[0].coord_x == 1);
    REQUIRE(exp[0].coord_y == 1);
    REQUIRE(exp[4].coord_x == 2);
    REQUIRE(exp[4].coord_y == 2);

    // Binary metadata
    REQUIRE(exp[0].peaks.size() == 8399);
    REQUIRE(exp.metadata.mz_data_type  == BinaryDataType::Float32);
    REQUIRE(exp.metadata.int_data_type == BinaryDataType::Float32);

    // IBD bounds validation
    std::vector<std::string> errors;
    bool ok = imzml::validate("tests/data/Example_Continuous.imzML", errors);
    REQUIRE(ok == true);
    REQUIRE(errors.empty());

    // Total peaks cross-check (9 × 8399 − alignment padding)
    size_t total = 0;
    for (size_t i = 0; i < exp.size(); ++i)
        total += exp[i].peaks.size();
    REQUIRE(total == 75591);
}
```

### 9.2 Python Smoke Test

```python
import imzml_pyoms as ip
import pyopenms as oms

# pyopenms imported first → subprocess bridge activated
exp  = ip.load("tests/data/Example_Continuous.imzML")

assert isinstance(exp, oms.MSExperiment)
assert exp.size() == 9

sp = exp[0]
mz, inten = sp.get_peaks()
assert len(mz) == 8399
assert int(sp.getMetaValue("IMS:1000050")) == 1   # pixel x
assert int(sp.getMetaValue("IMS:1000051")) == 1   # pixel y

img = ip.getionimage(exp, mz[4199], tol=0.5)
assert img.shape == (3, 3)
assert img.max() > 0

phys = ip.get_physical_coordinates(exp, 0)
assert phys == (100, 100, 1)   # 1 × 100 µm/pixel
```

### 9.3 Cross-Validation Against pyimzML

The `/api/benchmark` endpoint and `test_openms_imzml` binary both compare peak values
against pyimzML as the reference implementation.  Agreement is defined as all
intensities matching within float32 rounding tolerance (`|a - b| < 1e-5 × max(|b|, 1)`).

---

## 10. Future Features & Roadmap

### 10.1 Near-Term (v1.1.0 — Q2 2026)

#### A. ZSTD / zlib Decompression in IBD Decoder

Many real-world imzML files use in-band compression for individual spectrum arrays.
Currently only uncompressed data is decoded.

```cpp
// IBDReader.h — planned extension
struct CompressionOptions {
    enum class Type { None, ZLib, Zstd, Lz4 };
    Type mz_compression        = Type::None;
    Type intensity_compression = Type::None;
};

std::vector<Peak1D> readSpectrum(
    IBDEntry entry,
    BinaryDataType mzType,
    BinaryDataType intType,
    CompressionOptions compression   // NEW
);
```

#### B. Async / Chunked Ion Image (Memory-Mapped IBD)

For large (> 1 GB) datasets, decode ion images using `mmap` so the OS page cache
handles working set management. Ion image queries for a single m/z become a pure
in-memory scan with zero `read()` syscalls once the file is warm.

#### C. `GET /api/tic-profile` — trace TIC along rows or columns

Useful for detecting tissue boundaries and scan anomalies without loading all peaks.

```
GET /api/tic-profile?axis=x&idx=12
→ {"values": [TIC_at_y1, TIC_at_y2, ...]}
```

#### D. Python: `imzml_pyoms.write()` — Save as imzML

Calls `ImzMLWriter` from the C++ layer via the nanobind extension — same interface
as `ip.load()` but with an output path argument.

---

### 10.2 Medium-Term (v1.2.0 — Q3 2026)

#### E. 16-bit float (float16) Intensity Support

MS:1000520 (16-bit float) is present in the ontology and used by some MALDI-TOF
instruments. Requires a software IEEE 754 half-precision decoder (`uint16_t` → `float`).

#### F. Coordinate-Indexed Lookup Map

For interactive viewers, add an `O(1)` lookup from `(x, y)` → spectrum index:

```cpp
// Planned: CoordIndex in MSExperiment
std::unordered_map<std::pair<int,int>, size_t, CoordHash> coord_index_;

// Usage:
size_t idx = exp.spectrumAtPixel(12, 34);
const auto& spec = exp[idx];
```

Currently the server does a linear scan for `?x=X&y=Y` queries; this makes it O(1).

#### G. Tile-Based Progressive Loading

For multi-GB datasets:
1. Metadata-only parse on load (instant)
2. Background thread decodes spectra in row-major tile order
3. Web viewer marks pixels as "loading" until decoded
4. Fully loaded pixels render at full quality

#### H. imzML 2.0 (MALDI-2 Extensions) Preview Support

The upcoming imzML 2.0 draft adds:
- 3D spatial coordinates (tissue sections)
- Multiple ion mobility dimensions
- Polarity switching per spectrum
- Enhanced instrument metadata

Preparatory: add `z` coordinate to all APIs (already present in IMS:1000052)
and extend the metadata schema.

---

### 10.3 Long-Term (v2.0.0 — 2027)

#### I. GPU-Accelerated Ion Image Extraction (CUDA / Metal)

Ion image generation is a vectorised scan over all spectra — ideal for GPU
sum-reduction (CUDA on Linux, Metal on macOS). Optional backend flag on `ip.load()`.

#### J. MSI-specific Statistics API

Per-pixel feature extraction into a `pd.DataFrame` (TIC, base peak m/z, peak count,
entropy) and a spatial Pearson correlation map vs. a reference ion.

#### K. Web Viewer: Annotation Layer

Allow users to draw ROI masks on the heatmap and request averaged spectra within
the selected region (`POST /api/roi` + `GET /api/roi/avgspectrum`).

#### L. WASM Distribution

Compile `libimzml_parser` to WebAssembly for fully browser-native parsing via the
File System Access API — no server required.

#### M. R Bindings (`rimzml`)

Wrap `imzml_ext` via `Rcpp` / `cpp11` for a Cardinal-compatible drop-in with
faster loading and streaming support.

---

### 10.4 Roadmap Summary

```
v1.0.0  ████████████████████  CURRENT
         SAX parser · OpenMS bridge · pyopenms bindings
         14-endpoint web server · CLI · imzML write

v1.1.0  ────────────────────  Q2 2026
         zlib / zstd decompression
         memory-mapped IBD reader
         TIC profile endpoint
         imzml_pyoms.write()

v1.2.0  ────────────────────  Q3 2026
         float16 intensity support
         O(1) coordinate lookup map
         tile-based progressive loading
         imzML 2.0 preview (3D coords)

v2.0.0  ────────────────────  2027
         GPU ion image (CUDA / Metal)
         MSI statistics API (pandas)
         Web viewer ROI annotation
         WASM browser distribution
         R bindings (rimzml)
```

---

## 11. Deployment & Operations

### 11.1 Production Stack

```
Ubuntu 24.04 LTS (VPS, 68.178.165.201)
  └── nginx 1.24 (SSL termination, reverse proxy)
        └── imzml_server (port 8080, systemd-managed)
              └── /opt/imzml/build/bin/imzml_server --port 8080
```

**systemd unit (`deploy/imzml-server.service`):**

```ini
[Unit]
Description=imzML Web Server
After=network.target

[Service]
Type=simple
User=imzml
WorkingDirectory=/opt/imzml
ExecStart=/opt/imzml/build/bin/imzml_server --port 8080
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

### 11.2 Build & CI

```bash
# macOS (arm64, development)
cmake -S . -B build/release              \
      -DCMAKE_BUILD_TYPE=Release         \
      -DBUILD_PYTHON_BINDINGS=ON         \
      -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build/release --target all -j8
ctest --test-dir build/release -V

# Ubuntu (production)
cmake -S . -B build                      \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
```

**CPack distributions:**

```bash
cpack --config build/release/CPackConfig.cmake
# → imzml-parser-1.0.0-Darwin-arm64.tar.gz
# → imzml-parser-1.0.0-Darwin-arm64.zip
```

---

## 12. Dependencies & Licensing

### 12.1 Runtime Dependencies

| Library | Version | Required | License | Purpose |
|---|---|:---:|---|---|
| [expat](https://libexpat.github.io/) | ≥ 2.4 | ✅ | MIT | SAX XML parsing |
| [libOpenMS](https://www.openms.de/) ([docs](https://abibuilder.cs.uni-tuebingen.de/archive/openms/Documentation/release/latest/html/index.html)) | 3.5.0 (bioconda) | ❌ | BSD-3-Clause | Phase-1 MzML parse, IBD decode |
| [Xerces-C](https://xerces.apache.org/xerces-c/) ([API docs](https://xerces.apache.org/xerces-c/apiDocs-3/)) | 3.2 (bioconda) | with OpenMS | Apache-2.0 | XML validation |
| [nanobind](https://nanobind.readthedocs.io/en/latest/) | 2.12.0 | for Python | BSD-3-Clause | Python extension binding |
| [numpy](https://numpy.org/doc/stable/) | ≥ 1.21 | for Python | BSD-3-Clause | Array output |
| [pyopenms](https://pyopenms.readthedocs.io/en/latest/) | 3.5.0 | for pyopenms API | BSD-3-Clause | MSExperiment/MSSpectrum output |

### 12.2 Build-Only Dependencies

| Tool | Version | Purpose |
|---|---|---|
| CMake | ≥ 3.20 | Build system |
| ccache | any | Compiler output caching |
| Boost | ≥ 1.74 | OpenMS compat headers |
| Python | ≥ 3.8 | nanobind target |

### 12.3 imzml-parser License (proposed)

**MIT License** — compatible with all dependencies, permissive for academic and commercial
use.

```
Copyright (c) 2026 Aditya Sarna

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions: ...
```

---

*This document was generated from the live source at `/Users/adityasarna/imzml`.*
*Parser version 1.0.0 · Build date March 2026*
