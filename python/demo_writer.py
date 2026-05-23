"""
demo_writer.py  --  exercises every new nanobind binding added to imzml_ext:

  Coordinate           -- 1-based pixel position
  BinaryDataType       -- IBD array encoding enum
  ImzMLWriterOptions   -- configuration struct for ImzMLWriter
  ImzMLWriter          -- low-level writer (also as context manager)
  write()              -- convenience one-call bulk write

Run from the repo root:
  python3.14 python/demo_writer.py
"""
import sys, tempfile, os
sys.path.insert(0, "build/release/python")
import imzml_ext as im
import numpy as np

# ---------------------------------------------------------------------------
# 1. Coordinate
# ---------------------------------------------------------------------------
print("=== Coordinate ===")
c0 = im.Coordinate()
print(f"  default     : {c0!r}")

c1 = im.Coordinate(3, 7)
print(f"  Coordinate(3, 7) : {c1!r}  x={c1.x}  y={c1.y}  z={c1.z}")

c2 = im.Coordinate(5, 2, 3)
print(f"  Coordinate(5, 2, 3) : {c2!r}")

c2.x = 10
print(f"  after c2.x = 10 : {c2!r}")

# ---------------------------------------------------------------------------
# 2. BinaryDataType
# ---------------------------------------------------------------------------
print("\n=== BinaryDataType ===")
for name in ("Unknown", "Float32", "Float64", "Int32", "Int64"):
    val = getattr(im.BinaryDataType, name)
    print(f"  BinaryDataType.{name:7s} = {val!r}")

# ---------------------------------------------------------------------------
# 3. ImzMLWriterOptions
# ---------------------------------------------------------------------------
print("\n=== ImzMLWriterOptions ===")
opts_default = im.ImzMLWriterOptions()
print(f"  default : {opts_default!r}")
print(f"    mode          = {opts_default.mode!r}")
print(f"    mz_type       = {opts_default.mz_type!r}")
print(f"    int_type      = {opts_default.int_type!r}")
print(f"    pixel_size_x  = {opts_default.pixel_size_x}")
print(f"    pixel_size_y  = {opts_default.pixel_size_y}")
print(f"    uuid          = {opts_default.uuid!r}")
print(f"    instrument_name = {opts_default.instrument_name!r}")

opts_custom = im.ImzMLWriterOptions()
opts_custom.mode         = im.ImagingMode.Processed
opts_custom.mz_type      = im.BinaryDataType.Float64
opts_custom.int_type     = im.BinaryDataType.Float32
opts_custom.pixel_size_x = 75.0
opts_custom.pixel_size_y = 75.0
opts_custom.uuid         = "aabbccdd-1234-5678-abcd-000000000001"
opts_custom.instrument_name = "FT-ICR MS"
print(f"\n  custom  : {opts_custom!r}")
print(f"    mode     = {opts_custom.mode!r}")
print(f"    mz_type  = {opts_custom.mz_type!r}")
print(f"    uuid     = {opts_custom.uuid!r}")

# ---------------------------------------------------------------------------
# 4. im.write()  --  convenience bulk write + round-trip via im.open()
# ---------------------------------------------------------------------------
print("\n=== im.write() [Continuous mode, Float32] ===")

mz_shared = list(np.linspace(100.0, 500.0, 5))
intensities = [
    [1.0,  2.5,  3.0,  0.5,  1.2],  # pixel (1,1)
    [0.3,  1.5,  4.2,  2.1,  0.7],  # pixel (2,1)
    [2.0,  0.8,  1.1,  3.3,  0.4],  # pixel (1,2)
    [0.1,  3.7,  2.9,  1.0,  2.5],  # pixel (2,2)
]
coords = [(1,1,1), (2,1,1), (1,2,1), (2,2,1)]

with tempfile.TemporaryDirectory() as d:
    path_cont = os.path.join(d, "cont.imzML")
    opts_cont = im.ImzMLWriterOptions()
    opts_cont.pixel_size_x = 100.0
    opts_cont.pixel_size_y = 100.0

    im.write(path_cont, coords, [mz_shared]*4, intensities, opts_cont)
    print(f"  wrote {path_cont}")

    exp = im.open(path_cont)
    print(f"  read back → {exp!r}")
    for i in range(len(exp)):
        spec = exp[i]
        mz_arr, int_arr = spec.get_peaks()
        print(f"    [{i}] ({spec.coord_x},{spec.coord_y}) "
              f"peaks={len(mz_arr)}  tic={int_arr.sum():.2f}  "
              f"base_peak_mz={mz_arr[int_arr.argmax()]:.2f} Da")

# ---------------------------------------------------------------------------
# 5. ImzMLWriter   --  low-level API (processed mode, Float64 m/z)
# ---------------------------------------------------------------------------
print("\n=== ImzMLWriter [Processed mode, Float64 mz] ===")

spectra_data = [
    (im.Coordinate(1, 1), [100.0, 150.3, 200.7],          [5.0, 2.1, 8.4]),
    (im.Coordinate(2, 1), [100.0, 155.1, 210.2, 350.9],   [1.0, 0.5, 3.3, 6.7]),
    (im.Coordinate(3, 1), [101.0, 202.0],                  [4.0, 4.0]),
]

with tempfile.TemporaryDirectory() as d:
    path_proc = os.path.join(d, "proc.imzML")

    opts_proc = im.ImzMLWriterOptions()
    opts_proc.mode         = im.ImagingMode.Processed
    opts_proc.mz_type      = im.BinaryDataType.Float64
    opts_proc.int_type     = im.BinaryDataType.Float32
    opts_proc.pixel_size_x = 50.0
    opts_proc.pixel_size_y = 50.0

    w = im.ImzMLWriter()
    print(f"  before open : {w!r}  is_open={w.is_open}")
    w.open(path_proc, opts_proc)
    print(f"  after open  : {w!r}  is_open={w.is_open}")

    for coord, mz, ints in spectra_data:
        w.add_spectrum(coord, mz, ints)
        print(f"  added ({coord.x},{coord.y})  peaks={len(mz)}  "
              f"spectra_written={w.spectra_written}")

    w.close()
    print(f"  after close : {w!r}  is_open={w.is_open}")

    # Read back
    exp2 = im.open(path_proc)
    print(f"\n  read back → {exp2!r}")
    for i in range(len(exp2)):
        s = exp2[i]
        mz_arr, int_arr = s.get_peaks()
        print(f"    [{i}] ({s.coord_x},{s.coord_y})  peaks={len(mz_arr)}"
              f"  tic={int_arr.sum():.2f}  mz={list(np.round(mz_arr, 1))}")

# ---------------------------------------------------------------------------
# 6. ImzMLWriter   --  context-manager (`with` statement)
# ---------------------------------------------------------------------------
print("\n=== ImzMLWriter [context manager] ===")

with tempfile.TemporaryDirectory() as d:
    path_ctx = os.path.join(d, "ctx.imzML")

    opts_ctx = im.ImzMLWriterOptions()
    opts_ctx.pixel_size_x = 25.0
    opts_ctx.pixel_size_y = 25.0

    with im.ImzMLWriter() as w:
        print(f"  inside with : {w!r}")
        w.open(path_ctx, opts_ctx)
        mz = [200.0, 400.0, 600.0]
        w.add_spectrum(im.Coordinate(1, 1), mz, [3.0, 1.5, 2.0])
        w.add_spectrum(im.Coordinate(2, 1), mz, [0.5, 4.0, 1.0])
        print(f"  before __exit__ : spectra_written={w.spectra_written}")
    # __exit__ called close() automatically
    print(f"  after `with` : is_open={w.is_open}")

    exp3 = im.open(path_ctx)
    print(f"  read back → {exp3!r}")
    for i in range(len(exp3)):
        s = exp3[i]
        mz_arr, int_arr = s.get_peaks()
        print(f"    [{i}] ({s.coord_x},{s.coord_y})  tic={int_arr.sum():.2f}")

# ---------------------------------------------------------------------------
# 7. SpectrumIndexEntry -- verify IBD offsets for written file
# ---------------------------------------------------------------------------
print("\n=== SpectrumIndexEntry (from Processed file) ===")

with tempfile.TemporaryDirectory() as d:
    path_idx = os.path.join(d, "idx.imzML")
    opts_idx = im.ImzMLWriterOptions()
    opts_idx.mode    = im.ImagingMode.Processed
    opts_idx.mz_type = im.BinaryDataType.Float64  # 8 bytes/element

    mz_a = [100.0, 200.0]
    mz_b = [150.0, 250.0, 350.0]
    im.write(path_idx,
             [(1,1,1), (2,1,1)],
             [mz_a,   mz_b],
             [[1.0, 2.0], [3.0, 4.0, 5.0]],
             opts_idx)

    disc = im.open(path_idx)
    for i in range(disc.get_nr_spectra()):
        e = disc.get_spectrum_index(i)
        print(f"  [{i}] {e!r}")
        print(f"       mz_offset={e.mz_offset}  mz_length={e.mz_length}  "
              f"mz_type={e.mz_type!r}")
        print(f"       int_offset={e.int_offset}  int_length={e.int_length}  "
              f"int_type={e.int_type!r}")
