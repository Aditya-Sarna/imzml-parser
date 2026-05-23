# imzML Parser — C++17

SAX-based imzML 1.1.0 parser modelled on the OpenMS `MzMLHandler` architecture.

---

## Architecture

```
include/imzml/
  ImzMLTypes.h        CV accession constants, enums, plain data structs
  XMLHandler.h        Abstract expat SAX base (mirrors OpenMS XMLHandler)
  ImzMLHandler.h      imzML-specific SAX handler (mirrors MzMLHandler)
  IBDReader.h         Binary .ibd decoder with bounds validation
  ImzMLSpectrum.h     Per-spectrum metadata and decoded data structs
  ImzMLFile.h         Public API (mirrors MzMLFile / XMLFile)

src/
  XMLHandler.cpp
  ImzMLHandler.cpp
  IBDReader.cpp
  ImzMLFile.cpp

tests/
  gen_example_data.cpp   Generates 3x3 continuous test dataset
  test_imzml.cpp         Integration test suite (24 assertions)
  data/                  Generated at build time
```

The class hierarchy mirrors OpenMS:

| imzML parser        | OpenMS equivalent       | Role                              |
|---------------------|-------------------------|-----------------------------------|
| `XMLHandler`        | `XMLHandler`            | expat SAX base, attribute helpers |
| `ImzMLHandler`      | `MzMLHandler`           | element/cvParam dispatch          |
| `ImzMLFile`         | `MzMLFile` + `XMLFile`  | load, ibd open, public API        |
| `IBDReader`         | inline decode in Handler| typed binary read + validation    |

---

## Features

| Feature                                         | Status |
|-------------------------------------------------|--------|
| SAX-based XML parsing (expat)                   | Yes    |
| Reads m/z and intensity arrays from .ibd        | Yes    |
| Extracts pixel (x, y, z) coordinates            | Yes    |
| Continuous mode detection                       | Yes    |
| Processed mode detection                        | Yes    |
| float32, float64, int32, int64 data types       | Yes    |
| Binary array bounds validation                  | Yes    |
| referenceableParamGroup resolution              | Yes    |
| Scan settings (grid dims, pixel size)           | Yes    |
| IBD UUID / MD5 / SHA-1 metadata                 | Yes    |
| 3x3 continuous 32-bit float example             | Yes    |
| Cross-validated against pyimzML                 | Yes    |

---

## Dependencies

| Library | Purpose         | Install (macOS)          | Install (Ubuntu)              |
|---------|-----------------|--------------------------|-------------------------------|
| expat   | SAX XML parsing | `brew install expat`     | `apt-get install libexpat1-dev` |

No other dependencies beyond the C++17 standard library.

---

## Build

```bash
# macOS
brew install expat

# Configure
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release

# Build library + tests
make -j$(nproc)

# Generate test data and run tests
./tests/gen_example_data tests/data/
./tests/test_imzml
```

Or with `ctest`:

```bash
# From the build directory (data is generated automatically)
ctest -V
```

---

## Usage

```cpp
#include "imzml/ImzMLFile.h"

int main()
{
    imzml::ImzMLFile f;
    f.load("image.imzML");   // .ibd inferred automatically

    // Print summary
    f.printSummary(std::cout);

    // Validate all binary ranges
    auto errors = f.validate();

    // Iterate spectra
    for (std::size_t i = 0; i < f.size(); ++i)
    {
        const auto& meta = f.spectrum(i);         // coords + offsets
        auto data        = f.getSpectrum(i);       // decode from .ibd

        // data.mz        std::vector<double>
        // data.intensity std::vector<double>
        // meta.coord.x/y/z
    }

    // Imaging mode
    if (f.mode() == imzml::ImagingMode::Continuous)
        // all spectra share the same m/z array offset

    return 0;
}
```

Link against `libimzml_parser.a` and `-lexpat`.

---

## CV Accessions Handled

| Accession   | Meaning                   |
|-------------|---------------------------|
| MS:1000514  | m/z array                 |
| MS:1000515  | intensity array           |
| MS:1000521  | 32-bit float              |
| MS:1000523  | 64-bit float              |
| MS:1000519  | 32-bit integer            |
| MS:1000522  | 64-bit integer            |
| IMS:1000030 | continuous mode           |
| IMS:1000031 | processed mode            |
| IMS:1000050 | position x                |
| IMS:1000051 | position y                |
| IMS:1000052 | position z                |
| IMS:1000080 | universally unique identifier |
| IMS:1000090 | ibd MD5                   |
| IMS:1000091 | ibd SHA-1                 |
| IMS:1000102 | external offset           |
| IMS:1000103 | external array length     |
| IMS:1000104 | external encoded length   |
| IMS:1000042 | max count of pixels x     |
| IMS:1000043 | max count of pixels y     |
| IMS:1000044 | max dimension x           |
| IMS:1000045 | max dimension y           |
| IMS:1000046 | pixel size x              |
| IMS:1000047 | pixel size y              |

---

## Test Output (3x3 continuous, float32)

```
imzML Parser Test Suite
----------------------------------------
[ Load          ]  PASS  File loaded without exception
[ Metadata      ]  PASS  3x3 grid => 9 spectra
                   PASS  Imaging mode is continuous
                   PASS  maxCountX == 3 / maxCountY == 3
[ Pixel Coords  ]  PASS  Spectra 0, 4, 8 at correct positions
[ Binary Meta   ]  PASS  Shared m/z offset (continuous)
                   PASS  float32 data type (mz + intensity)
                   PASS  8399 elements per array
[ Validation    ]  PASS  No out-of-bounds errors
[ Spectrum Data ]  PASS  Decode 8399 peaks, sorted m/z, intensity >= 0
[ Full Iter     ]  PASS  All 9 spectra read, 75591 total peaks

Results: 24/24 passed
```

---

## Future Integration with OpenMS

The handler design is intentionally close to OpenMS's `MzMLHandler` pattern so
that it can be integrated as a drop-in `ImzMLHandler : public XMLHandler` inside
the OpenMS source tree.  The remaining steps for full OpenMS integration are:

1. Replace expat with Xerces-C (`xercesc::DefaultHandler` base)
2. Map `ImzMLSpectrum` to `MSSpectrum` with `MetaDataArrays` for imzML-specific fields
3. Use `PeakFileOptions` for selective loading (ms-level, RT range)
4. Add `IMSDataConsumer` streaming interface
5. Hook into OpenMS `ProgressLogger`
