"""u7hires: art tooling for the Exult hi-res (6x) terrain overrides.

Modules (see docs-hires/design/DESIGN.md §5 and §8):

* ``io``        read shapes.vga / u7chunks / u7map / palettes.flx; palette 0 via Get_color8
* ``hashing``   CRC32, FNV-1a-64, T1 terrain key
* ``rules``     cycle ranges, the shared P4 definition, engine palette ramps, reduce_mode
* ``fill``      Python port of the engine's paint_tile fill (objs/chunkter.cc)
* ``ctx``       context windows, instance maps, macro sheets, self-wrap windows
* ``quant``     OKLab, uniquified palette, local snap, global nearest, P4 enforcement
* ``scalers``   xBRZ 1.9 / MMPX (ctypes) and Scale2x/Scale3x on index planes
* ``consensus`` per-(shape, frame) weighted mode / OKLab medoid
* ``pngio``     raw-index palette PNG writer/reader (PLTE, tEXt), restamp
* ``pack``      pack writer (loose PNGs, sidecars, pack.txt, bundle), atomic writes
* ``check``     engine-equivalent validator (N1 F1 F2 F3 F4 P0 P4 G1 G2 R1) + offline P2/E1
* ``qa``        QA metrics A1-A3 B1-B4 C1 C2 D1, reports, contact sheets, previews
* ``route3``    xBRZ 6x + local snap + mode consensus (CPU)
* ``route2``    spandrel 4x-NXbrz (GPU) + Lanczos 1.5x + back-projection + snap + consensus
* ``vote``      majority voting over redundant runs (transient hardware errors), pack verification

EA-derived pixels never go into the repository; packs and work data live outside it.
"""

__version__ = "0.1.0"

S_ART = 6          # scale the override art is authored at
TILE = 8           # c_tilesize
TILES_PER_CHUNK = 16
CHUNK_PX = TILE * TILES_PER_CHUNK
NUM_CHUNKS = 192   # chunks per world axis (c_num_chunks)
WORLD_TILES = NUM_CHUNKS * TILES_PER_CHUNK  # 3072
