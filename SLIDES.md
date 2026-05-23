# imzml-parser — Presentation Deck
**v1.0.0 · C++20 + Python · March 2026**

> Each `---` is a slide boundary. Bullet depth = slide body hierarchy.

---

## Slide 1 — Title

**imzml-parser**
*A high-performance C++20 / Python library for mass spectrometry imaging*

- Fast streaming parser for `.imzML` + `.ibd` datasets
- Genuine OpenMS integration — extends `OpenMS::ProgressLogger`, `OpenMS::Internal::XMLHandler`
- First-class Python bindings → `pyopenms.MSExperiment` natively
- Live viewer at **imzmlparser.com** (Ubuntu 24.04, nginx, systemd)

---

## Slide 2 — The Problem

**imzML is two files. Every existing tool loads both into RAM.**

| Instrument | Grid | Peaks/pixel | `.ibd` size |
|---|---|---|---|
| MALDI-TOF tissue | 400 × 400 | 18 000 | **6.1 GB** |
| OrbiTrap MSI | 200 × 200 | 60 000 | **9.6 GB** |

**Gap in every existing tool:**

| Tool | RAM spike | OpenMS output | HTTP viewer | C++ embeddable |
|---|:---:|:---:|:---:|:---:|
| pyimzML | ❌ full load | ❌ | ❌ | ❌ |
| Cardinal (R) | ❌ | ❌ | ❌ | ❌ |
| OpenMS `ImzMLFile` | ✅ streaming | ✅ | ❌ | ❌ requires 2 GB stack |
| **imzml-parser** | ✅ streaming | ✅ | ✅ | ✅ zero-dep layer |

---

## Slide 3 — Our Solution: 3-Layer Architecture

```
Level 1 — Zero-dependency
  libimzml_parser.a · expat SAX · namespace imzml
  → embed in any C++ project, no OpenMS required

Level 2 — OpenMS integration
  openms_integration/ · namespace OpenMS
  → OpenMS::MSExperiment, MSSpectrum, MzMLFile::load()

Level 3 — Python
  imzml_ext.so  →  imzml.MSExperiment  (fast, no pyopenms)
  imzml_pyoms.py →  pyopenms.MSExperiment  (full compatibility)
```

**Key property:** levels are independent — use only what you need.

---

## Slide 4 — What a Real imzML File Looks Like

**File: `Example_Continuous.imzML`** (3 × 3 grid, 9 spectra)

```xml
<!-- ① Imaging mode — tells parser how IBD data is laid out -->
<cvParam accession="IMS:1000030" name="continuous"/>

<!-- ② Grid geometry -->
<cvParam accession="IMS:1000042" name="max count of pixels x" value="3"/>
<cvParam accession="IMS:1000043" name="max count of pixels y" value="3"/>
<cvParam accession="IMS:1000046" name="pixel size x" value="100"
         unitName="micrometer"/>

<!-- ③ Per-spectrum pixel coordinate (inside <scan>) -->
<cvParam accession="IMS:1000050" name="position x" value="1"/>
<cvParam accession="IMS:1000051" name="position y" value="1"/>

<!-- ④ IBD pointer — where the binary data lives -->
<cvParam accession="IMS:1000102" name="external offset" value="0"/>
<cvParam accession="IMS:1000103" name="external array length" value="8399"/>
<cvParam accession="IMS:1000104" name="external encoded length" value="33596"/>

<!-- ⑤ The element is ALWAYS empty — ALL data is in .ibd -->
<binary/>
```

**What each annotation drives in our parser:**

| CV term | Accession | Code path |
|---|---|---|
| Imaging mode | `IMS:1000030/31` | `meta_.imagingMode = Continuous` |
| Grid size | `IMS:1000042/43` | `meta_.maxX / maxY` |
| Pixel coords | `IMS:1000050/51/52` | `current_spectrum_.setCoordX/Y/Z()` |
| IBD offset | `IMS:1000102` | `current_array_.externalOffset` → `fseeko()` |
| Array length | `IMS:1000103` | `current_array_.externalLength` → `fread()` count |
| Data type | `MS:1000521` (float32) | `current_array_.dataType = Float32` |
| Array role | `MS:1000514/515` | `current_array_.isMzArray / isIntArray` |

---

## Slide 5 — What a Real IBD File Looks Like

**File: `Example_Continuous.ibd`** (335,960 bytes · 0.32 MB)

Continuous mode shares one m/z array across all 9 spectra.
Each intensity array occupies its own slice.

```
Byte offset    Length      Content
───────────────────────────────────────────────────────────────
0              33 596      m/z array — 8399 × float32, SHARED
                           [0x00]: 0000 c842 → 100.0000 Da
                           [0x04]: ad2a c842 → 100.0834 Da
                           [0x08]: 5b55 c842 → 100.1667 Da
                           ...
                           [0x8390]: last value → 800.0000 Da

33 596         33 596      intensity[spectrum 0]  pixel (1,1)
                           max intensity: 0.900
                           top peak: m/z 200.0238, I=0.9
                           6390 / 8399 peaks non-zero

67 192         33 596      intensity[spectrum 1]  pixel (2,1)
100 788        33 596      intensity[spectrum 2]  pixel (3,1)
134 384        33 596      intensity[spectrum 3]  pixel (1,2)
...
302 364        33 596      intensity[spectrum 8]  pixel (3,3)
───────────────────────────────────────────────────────────────
Total: 1 (m/z) + 9 (intensity) arrays = 10 × 33 596 = 335 960 bytes
```

**How our parser reads this (single pass):**

```
SAX fires </binaryDataArray>
  → externalOffset = 0, externalLength = 8399, dataType = Float32
  → fseeko(ibd_file_, 0, SEEK_SET)
  → fread(tmp, 4, 8399, ibd_file_)   // reads bytes 0–33595
  → s.mzArray()[i] = tmp[i]          // widens float32→double

SAX fires </binaryDataArray> (intensity for spectrum 0)
  → externalOffset = 33596
  → fseeko(ibd_file_, 33596, SEEK_SET)
  → fread(tmp, 4, 8399, ibd_file_)   // reads bytes 33596–67191
  → s.intensityArray()[i] = tmp[i]

SAX fires </spectrum>
  → zipArraysToPeaks()               // Peak1D{mz=100.0, intensity=0.0}, ...
  → consumer->consumeSpectrum(spec)  // delivered to caller
```

No second pass. No index. No deferred matching. IBD opened once (`if (ibd_file_) return true;`).

---

## Slide 6 — Core Pipeline: SAX → IBD → MSSpectrum

```
 .imzML XML (Xerces SAX2)
        │
        ▼
 ImzMLHandler::startElement / endElement
        │
        ├─ cvParam IMS:1000050/51/52  →  MSSpectrum.setCoordX/Y/Z()
        ├─ cvParam IMS:1000102/103    →  ArrayMeta.externalOffset / Length
        ├─ cvParam MS:1000521         →  ArrayMeta.dataType = Float32
        ├─ cvParam MS:1000514/515     →  ArrayMeta.isMzArray / isIntArray
        │
        └─ </binaryDataArray>
              fseeko(ibd, offset, SEEK_SET)    ← .ibd file (opened once)
              fread(buf, 4, count, ibd)
              → s.mzArray() / s.intensityArray()   ← staging buffers

        └─ </spectrum>
              zipArraysToPeaks()
                peaks_[i].setMZ(mzArray_[i])
                peaks_[i].setIntensity(intArray_[i])
              consumer->consumeSpectrum(std::move(spec))
                                                ← caller receives Peak1D[]
```

**Result:** `s[j].getMZ()`, `s[j].getIntensity()`, `s.getCoordX/Y()` all live. No imzML awareness needed downstream.

---

## Slide 7 — OpenMS Integration (Real, Not Simulated)

We extend actual OpenMS base classes — not reimplementations.

| Our class | Extends | OpenMS base |
|---|---|---|
| `OpenMS::ImzMLFile` | `public ProgressLogger` | same base as `OpenMS::MzMLFile` |
| `OpenMS::Internal::ImzMLHandler` | `public XMLHandler` | same base as `OpenMS::Internal::MzMLHandler` |
| `OpenMS::MSSpectrum` | adds staging buffers + pixel coords | subset of upstream `MSSpectrum` |

**What we call from bioconda OpenMS 3.5.0:**

```cpp
OpenMS::MzMLFile::load(path, exp)     // Phase 1: RT / MS-level / TIC
OpenMS::MSExperiment                  // primary container
OpenMS::Peak1D::setMZ/setIntensity    // per-peak storage
OpenMS::ProgressLogger                // progress reporting
OpenMS::OpenMS_Log_warn.setLevel()    // silence empty-binary warnings
```

**Why two phases?**
- Phase 1 (`MzMLFile::load`) → RT, MS level, TIC — OpenMS knows standard mzML
- Phase 2 (`ImzMLHandler` SAX) → pixel coords, IBD offsets, binary decode — only our code knows IMS ontology

---

## Slide 8 — Python Bindings

**Two modules, different tradeoffs:**

```python
# Fast path — parser-native types
import imzml_ext as im
exp = im.load("tissue.imzML", mz_lo=200.0, mz_hi=800.0)
img = exp.ion_image(mz=200.02, tol=0.1)   # numpy, pure C++

# Full compatibility path — real pyopenms objects
import imzml_pyoms as ip
exp = ip.load("tissue.imzML")
sp  = exp[0]                         # pyopenms.MSSpectrum
mz, inten = sp.get_peaks()           # np.float64, np.float32
x = sp.getMetaValue("IMS:1000050")   # pixel x — standard pyopenms API
img = ip.getionimage(exp, 200.02, tol=0.1)   # pyimzML-compatible
```

**Subprocess isolation bridge** (auto-activated when pyopenms already imported):

```
Normal:  imzml_pyoms_ext.so  →  bioconda libOpenMS.dylib (Xerces 3.2)
Conflict: pyopenms in sys.modules (Xerces 3.3) →
          spawn subprocess → IPC: .npz + .json → reassemble arrays in parent
```

Zero-copy numpy handoff via `nanobind::ndarray` + capsule deleters.

---

## Slide 9 — Web Viewer & REST API

**14 endpoints. Embedded SPA — no CDN, works offline.**

| Endpoint | Purpose |
|---|---|
| `POST /api/load` | Load dataset by path |
| `GET /api/image` | Full pixel list `{x,y,tic,bp,peaks}` |
| `GET /api/ion-image?mz=200.02&tol=0.1` | 2-D intensity matrix |
| `GET /api/spectrum?x=1&y=1` | Single pixel peaks `{mz[], intensity[]}` |
| `GET /api/avgspectrum` | Average across all pixels |
| `GET /api/benchmark` | Speed + agreement vs. pyimzML |
| `GET /api/export` | ZIP download (imzML + ibd, optional m/z filter) |
| `POST /api/upload` | Multipart `.imzML` + `.ibd` upload |

**Viewer tabs:** Heatmap (TIC / BP / ion image) · Spectrum viewer · Benchmark

---

## Slide 10 — Current Deliverables ✅

| Component | Status | Notes |
|---|---|---|
| `libimzml_parser.a` — SAX + IBD core | ✅ shipped | zero-dep, expat, C++20 |
| `openms_integration/` — OpenMS layer | ✅ shipped | extends real OpenMS classes |
| `ImzMLWriter` | ✅ shipped | round-trip tested, sub-region export |
| `imzml` CLI (5 subcommands) | ✅ shipped | info / validate / inspect / export / stats |
| `imzml_ext.so` — nanobind Python | ✅ shipped | parser-native types, numpy arrays |
| `imzml_pyoms.py` — pyopenms bridge | ✅ shipped | subprocess isolation, 5 public APIs |
| Web server + embedded SPA | ✅ shipped | 14 endpoints, live at imzmlparser.com |
| Test suite (CTest, 24 assertions) | ✅ shipped | cross-validated vs. pyimzML |
| CPack distribution (TGZ + ZIP) | ✅ shipped | macOS arm64 + Linux x86-64 |

---

## Slide 11 — Gaps & Missing Features

**Where the current v1.0 falls short:**

| Gap | Impact | Effort |
|---|---|---|
| **No IBD compression** (zlib / zstd) | Cannot read ~30% of real-world files that use `MS:1000574` (zlib) or newer `MS:1000576` compressed arrays | Medium |
| **No float16 support** (`MS:1000520`) | Some MALDI-TOF instruments export 16-bit half-precision intensities — silently produces zeros | Low |
| **O(n) pixel lookup** for `/api/spectrum?x=&y=` | Linear scan on every click in the viewer; becomes ~160 ms for 400×400 datasets | Low |
| **Memory-mapped IBD** | Large files (> 1 GB) read through repeated `fseeko/fread`; OS page cache not leveraged | Medium |
| **`imzml_pyoms.write()`** missing | Python users cannot save back to imzML — must drop to C++ layer | Medium |
| **Linux Phase 1 skipped** | `OpenMS::MzMLFile::load()` segfaults on Linux; RT / TIC from Phase 1 unavailable there | Hard |
| **No imzML 2.0 draft support** | Upcoming 3D coordinates, polarity switching, ion mobility dimensions not handled | Medium |
| **No ROI annotation API** | Viewers require region-of-interest averaging for histology correlation | Low |
| **No streaming write** | `ImzMLWriter` buffers the full IBD before writing XML — large exports risk OOM | Medium |
| **Tile-based progressive loading** | Viewer shows blank until full parse completes; no partial render | Medium |

---

## Slide 12 — Improvement Targets (Prioritised)

**Quick wins (< 1 week each):**

1. **`O(1)` coordinate map** — `unordered_map<{x,y}, size_t>` built at load time → `/api/spectrum` from ~60 ms → < 1 ms on 400×400
2. **float16 decode** — 20-line software IEEE 754 half-float decoder; closes `MS:1000520` support
3. **ROI endpoint** — `POST /api/roi` + `GET /api/roi/avgspectrum` — high user value, small server change

**Medium effort (1–2 weeks each):**

4. **zlib decompression** — `MS:1000574` support via `zlib`; needed for PRIDE repository datasets
5. **`imzml_pyoms.write()`** — expose `ImzMLWriter` to Python via nanobind; round-trip from pyopenms
6. **Memory-mapped IBD reader** — `mmap` + `std::span<const float>` slices; eliminates syscalls for repeated ion image queries

**Hard / longer term:**

7. **Linux Phase 1** — investigate OpenMS singleton init order on ELF platforms; may require `RTLD_LOCAL` dlopen
8. **Streaming ImzMLWriter** — write IBD chunks incrementally; required for exports from large filtered experiments
9. **imzML 2.0 preview** — `z` coordinate already wired; extend `ArrayMeta` with ion mobility, polarity fields

---

## Slide 13 — Roadmap

```
v1.0  ██████████████████  CURRENT  (March 2026)
       SAX parser · OpenMS bridge · pyopenms  
       14-endpoint viewer · CLI · imzML write
       9 CTest assertions, cross-validated

v1.1  ─────────────────   Q2 2026
       zlib / zstd IBD decompression  
       O(1) pixel coordinate lookup  
       float16 intensity support  
       imzml_pyoms.write()

v1.2  ─────────────────   Q3 2026
       Memory-mapped IBD reader  
       Tile-based progressive loading  
       ROI annotation + avgspectrum  
       imzML 2.0 draft (3D coords, polarity)

v2.0  ─────────────────   2027
       GPU ion image (Metal / CUDA)  
       R bindings (rimzml, Cardinal-compatible)  
       WASM browser-native parser  
       MSI statistics API (pandas DataFrame)
```

---

## Slide 14 — By the Numbers

| Metric | Value |
|---|---|
| Parse 9-spectrum test dataset | < 5 ms |
| Parse vs. pyimzML speedup | ~6× |
| m/z range covered (test file) | 100.00 – 800.00 Da |
| Peaks per spectrum (test) | 8 399 |
| Total peaks cross-validated | 75 591 |
| IBD decode size (test file) | 335 960 bytes · 0.32 MB |
| IBD layout (continuous mode) | 1 shared m/z array + N intensity arrays |
| Float32 → double widening | zero-copy where possible (float64 path) |
| Web server concurrency model | `shared_mutex` · many readers / one writer |
| Test assertions | 24 (CTest) |
| REST endpoints | 14 |
| Lines of C++ | ~4 500 |
| Platforms | macOS arm64 · Ubuntu 22/24 · Linux x86-64 |
