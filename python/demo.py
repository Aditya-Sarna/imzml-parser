import sys
sys.path.insert(0, "build/release/python")

import imzml_ext as im

# --- MSExperiment ---
exp = im.load("build/tests/data/Example_Continuous.imzML")
print("=== MSExperiment ===")
print(repr(exp))
print(f"  spectra : {len(exp)}")
print(f"  grid    : {exp.grid_width} x {exp.grid_height}")
m = exp.metadata
print(f"  mode    : {m.imaging_mode}")
print(f"  uuid    : {m.uuid}")
print(f"  pixel   : {m.pixel_size_x} x {m.pixel_size_y} um")

# --- MSSpectrum ---
print("\n=== MSSpectrum (spectrum 0) ===")
s = exp[0]
print(repr(s))
print(f"  coord   : ({s.coord_x}, {s.coord_y}, {s.coord_z})")
print(f"  peaks   : {len(s)}")
print(f"  TIC     : {s.tic:.4f}")
print(f"  base pk : {s.base_peak:.4f} Da")
print(f"  sorted  : {s.is_sorted}")

# --- get_peaks() numpy arrays ---
mz, intensity = s.get_peaks()
print("\n=== get_peaks() ===")
print(f"  mz dtype       : {mz.dtype},  shape: {mz.shape}")
print(f"  intensity dtype: {intensity.dtype}, shape: {intensity.shape}")
print(f"  mz[0:3]        : {mz[:3]}")
print(f"  intensity[0:3] : {intensity[:3]}")

# --- Peak1D iteration ---
print("\n=== Peak1D (first 3 peaks) ===")
for peak in list(s)[:3]:
    print(f"  {repr(peak)}")

# --- OnDiscImzMLExperiment ---
print("\n=== OnDiscImzMLExperiment ===")
disc = im.open("build/tests/data/Example_Continuous.imzML")
print(repr(disc))
print(f"  nr_spectra : {disc.get_nr_spectra()}")

entry = disc.get_spectrum_index(4)
print(f"\n  SpectrumIndexEntry [4]:")
print(f"    coord=({entry.x},{entry.y})  mz_offset={entry.mz_offset}  mz_length={entry.mz_length}  type={entry.mz_type}")

spec4 = disc[4]
print(f"\n  Decoded spectrum 4:")
print(f"    {repr(spec4)}")
mz4, int4 = spec4.get_peaks()
print(f"    base peak mz = {mz4[int4.argmax()]:.4f} Da  intensity = {int4.max():.4f}")

# --- validate ---
print("\n=== validate ===")
ok, errors = im.validate("build/tests/data/Example_Continuous.imzML")
print(f"  ok={ok}  errors={errors}")
