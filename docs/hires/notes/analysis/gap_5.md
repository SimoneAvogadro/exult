# Gap 5: consistency rules for partial hi-res overrides

**Topics:** animations, composites, actor and weapon offsets, reflections, palette remaps, and a pack validator.

Repo: `/home/simonea/ultima7_exult/exult-hires` (git master `8b6ab6b43`). All `file:line` references point into that tree.
This document builds on `shapes.md` §7, which covers the geometry mapping, the `Shape_frame::hires` attachment, the companion VGA
and PNG formats, and the CRC idea. It does not repeat the loading design. It defines **which overrides may coexist and which
may not**, and **what a pack validator must reject**.

---

## 0. Summary

1. **Low-res frames stay authoritative for every game-logic decision.** That covers emptiness checks, picking, dirty rectangles,
   draw ordering, weapon placement, font advance and animation ranges. A hi-res frame is a *paint-only* substitute. It must
   be geometrically subordinate to its low-res frame: same origin mapping, a bbox contained in S x the low-res bbox, the same
   emptiness, and a silhouette within about 1 low-res pixel. If any of these is violated, picking, dirty rects or z-order go
   visibly wrong (section 2).
2. **With the "top-left sub-pixel" origin convention, hi-res reflection is exact.** `Shape_frame::reflect()` is a transpose
   (`vgafile.cc:73-126`). Transposition commutes with the S x mapping, so an auto-transposed hi-res frame lines up exactly with
   the low-res reflection. Explicit hi-res `f|32` frames are allowed only where the low-res frame is a synthesized reflection,
   and they must follow the same transposed geometry (section 3.5).
3. **Actor and weapon alignment survives automatically** at S-pixel granularity: hi-res position = S x (`wx - actor_x`, `wy - actor_y`),
   with the same swap on reflection (`actors.cc:2163-2234`). The `wihh.dat` table is 1 byte per axis and limited to 0..63
   (`shapevga.cc:822-834`), so it cannot carry hi-res precision. Sub-pixel refinement needs an optional sidecar (section 5).
4. **Partial overrides inside a visual group cause popping between hi-res and NN art.** The engine already defines
   most groups implicitly. These include `Animation_info` cycles (`animate.cc:327-362`), actor walk and attack sequences (`actors.cc:116-160`),
   the actor direction scheme (`objs.cc:106`), weapon in-hand frames (`actors.cc:2177-2212`), barge rotation orbits
   (`shapeinf.cc:521-544`), barge wheel and draft-horse cycles (`barge.cc:640-648`), sprite sequences (`effects.cc:388-443`),
   missile direction frames 8..23 (`effects.cc:619-626,743-749`), paperdoll sets (`Paperdoll_gump.cc:137-160,642-688`),
   gump button pairs (`Gump_button.cc:76-78`) and font glyph sets (`font.cc:281,320`). A validator can derive these groups
   mechanically. The runtime should then activate overrides **per group, all or nothing** (strict mode, the default), with
   a lenient mode for development.
5. **Terrain flats (priority 1) have no frame animation.** Only RLE terrain pieces become `Animated_object` (`chunks.cc:728-737`).
   Flats animate only through palette cycling. Their consistency issues are seams between neighbouring tiles and preserving
   palette-cycling and translucency semantics. These are checked by tile adjacency mined from `u7chunks`, and by index-class
   rules (section 4).
6. **Palette effects apply index-wise** (`shapeid.h:348-356`, `shapeid.cc:603-641`). An indexed hi-res frame therefore gets NPC
   remaps, egg palette shifts, translucency and cycling for free, *provided its indices stay in the same ramp, cycle and xform
   classes as the low-res pixels they replace*. That is a validator rule. Truecolor overrides would break remaps for actors
   and must be forbidden for remappable shapes.

---

## 1. What stays low-res: game logic that reads low-res frames

Every item below reads the `Shape_frame*` returned by `ShapeID::get_shape()` / `Vga_file::get_shape()`. They must keep receiving
the **low-res** frame. Each one sets a constraint on the hi-res art.

| Logic | Code | Constraint on hi-res art |
|---|---|---|
| Actor empty-frame substitution (1-hand <-> 2-hand, fallback to standing) | `actors.cc:803-823` (`change_frame`), `actors.cc:1195-1208` (`get_attack_frames`), table `actors.cc:116-129` | A hi-res frame must be **empty iff the low-res frame is empty**. A hi-res frame supplied for a low-res-empty frame is never shown. |
| Usecode `set_item_frame` refuses empty frames | `usecode/ucinternal.cc:810-826` | Same emptiness rule. |
| Dead body creation checks frame emptiness | `actors.cc:4358-4370` | Same. |
| Screen rect / dirty area of objects | `gamewin.h:826-828`, `gamewin.cc:1223-1253` | Hi-res bbox must be contained in S x low-res bbox, otherwise stale pixels remain. |
| Weapon dirty rect (+4 px slack) | `actors.cc:762-796` (`r.enlarge(c_tilesize/2)` at `:791`), `actors.cc:2131` | Weapon hi-res bbox within S x low-res bbox (+ at most 4 game px). |
| Render ordering (screen overlap test, "bigger area painted second") | `objs/ordinfo.h:67`, `objs/objs.cc:1296-1322`; dependencies built in `chunks.cc:747-790` | If hi-res art bleeds outside S x bbox, overlaps are not ordered, so hi-res pixels get painted over or paint over the wrong neighbour. |
| World picking (pixel exact, "liberal by 1 px") | `gamewin.cc:2104-2112` -> `Shape_frame::has_point` `vgafile.cc:706-742` | Hi-res silhouette must stay within about 1 low-res pixel of the low-res silhouette, otherwise clicks hit invisible pixels or miss visible ones. |
| Gump item and widget picking | `gumps/Gump.cc:281-286`, `gumps/Gump_widget.cc:41`, `gumps/Gump.cc:505-506` | Same as world picking (later phases). |
| Font advance = glyph frame width | `shapes/font.cc:281,320,556,576,599,745` | Hi-res glyph width must be exactly S x low-res width. |
| Animation range = low-res `get_num_frames` + `Animation_info` | `animate.cc:327-362` | A hi-res pack can never add frames to a cycle. |
| Footprint swap on reflection (`dims` index by bit 5) | `shapeinf.h:754-760`, flag set when nframes > 32 at `shapeid.cc:122-131` | Reflected hi-res art must depict the transposed footprint. |
| Visibility early-out uses frame w/h | `vgafile.cc:491-495, 512-516, 550-554` | Painter must use hi-res w/h in hi-res units (rendering detail). |

Corollary: the hi-res lookup belongs **only in the paint path** (`Shape_manager::paint_shape`, `paint_invisible`,
`paint_outline`, `shapeid.h:171-196`, plus flat `copy8` sites in `chunkter.cc:91,130,300`). `get_shape()` must keep returning
the low-res frame. This matches the "attached pointer" design in `shapes.md` §7.1.

---

## 2. Geometric invariants (per frame)

### 2.1 Origin and bbox mapping
A low-res RLE frame covers `x in [-xleft, xright]`, `y in [-yabove, ybelow]` around the origin pixel (0,0) (`vgafile.h:124-146`).
It is painted at screen `(xoff+x, yoff+y)`. Use this convention for hi-res: game pixel `p` covers hi-res `[S*p, S*p+S-1]`,
and the hi-res origin is the top-left sub-pixel of the low-res origin pixel. The canonical hi-res extents are:

```
xleft_h  = S*xleft          xright_h = S*xright + S-1      w_h = S*w
yabove_h = S*yabove         ybelow_h = S*ybelow + S-1      h_h = S*h
```

Flats: the low-res extents are `xleft=yabove=8, xright=ybelow=-1` (`vgafile.cc:444-445`) and the frame is painted at
`(xoff-8, yoff-8)` (`vgafile.cc:532`). The hi-res flat is therefore exactly `8S x 8S` at `(S*xoff-8S, S*yoff-8S)`.

**Rules G1-G3**
- **G1 (exact extents, required for flats and fonts):** `w_h == S*w` and `h_h == S*h`, with the extents given above.
- **G2 (containment, required for every RLE frame):** every opaque hi-res pixel lies inside the canonical S x bbox. A loader may
  accept a smaller bbox (trimmed transparent margins) as long as the hotspot follows the canonical mapping. It must reject
  anything outside.
- **G3 (PNG oFFs):** if a PNG carries an `oFFs`, it must equal the canonical value (`shapes.md` §7.2). Otherwise the
  pack is wrong. Do not "auto-correct", because a silently shifted hotspot is the worst failure (weapons float, feet slide).

### 2.2 Emptiness and silhouette
- **G4 (emptiness):** `hires.is_empty() == lowres.is_empty()` (`vgafile.h:154-156` defines empty as "no scanlines").
- **G5 (silhouette, warning then error):** let `M` be the low-res opaque mask and `M1` its 1-pixel dilation (this mirrors
  `has_point`'s `scanx-1 .. scanx+scanlen` slack, `vgafile.cc:721`). Let `H` be the hi-res mask box-downsampled by S
  ("any sub-pixel opaque"). Require `H ⊆ M1`, so no visible hi-res pixel is unclickable. Also require coverage
  `|H ∩ M| / |M| >= 0.9` (tunable), so that clicks on low-res pixels mostly hit visible art.
- **G6 (flats are opaque):** flats are copied with `copy8`, with no transparency (`chunkter.cc:91`). Index 255 in a flat is
  drawn as a colour. A hi-res flat must be fully opaque, and a PNG with alpha or tRNS on a flat is rejected.

### 2.3 Why reflection commutes (proof sketch)
`Shape_frame::reflect()` swaps the extents (`xleft'=yabove`, `yabove'=xleft`, `xright'=ybelow`, `ybelow'=xright`,
`vgafile.cc:86-89`) and writes pixel `(x,y)` to `(y,x)` (`vgafile.cc:105,116,118`). For a hi-res frame that follows G1/G2:
`xleft_h' = yabove_h = S*yabove = S*xleft'` and `xright_h' = ybelow_h = S*ybelow+S-1 = S*xright'+S-1`. NN upscaling maps
hi-res `(hx,hy)` to low-res `(floor(hx/S), floor(hy/S))`, and transposition commutes with that. **So the auto-transposed hi-res
frame satisfies G1/G2 relative to the low-res reflected frame, with no extra data.** The only cost is a temporary
`max(w,h)^2` buffer (`vgafile.cc:77-92`).

The convention matters here. A centred convention (`S*p + S/2`) would shift by `S/2` on reflection.

---

## 3. Inventory of multi-frame and multi-shape couplings

### 3.1 Frame animators (`Animation_info`)
- Created for animated shapes: `Animator::create` (`animate.cc:269-280`) picks `Frame_animator` if `nframes > 1`,
  `Wiggle_animator` if there is a single frame (position jitter only, `animate.cc:546-569`), or `Sfx_animator` if the shape is not animated.
- Owners: terrain RLE pieces (`chunks.cc:728-737`: `info.is_animated() ? Animated_object : Terrain_game_object`),
  IREG and IFIX objects (`animate.cc:601-658`). Each `paint()` calls `want_animation()` and then paints the current frame
  (`animate.cc:592-595,618-621,655-658`).
- **Group definition** (`Frame_animator::Initialize`, `animate.cc:327-362`): the reflect bit is masked off (`:330-331`).
  `cnt = aniinf->get_frame_count()` (default from TFA = all frames, `aniinf.cc:66-96`; `get_animation_info_safe` defaults to type
  0 with all frames, `shapeinf.cc:217-223`). The cycle is `[first_frame, first_frame+nframes)` with
  `first_frame = last_frame - last_frame % nframes`, clipped to the shape's frame count (`:341-349`). The rotate flag is added back
  (`:360-361`), so a reflected animated object cycles through `f|32` frames, which are transposes.
- Frame selection modes (`animate.cc:382-423`): `FA_TIMESYNCHED` (all instances in lock-step, so a missing frame "blinks"
  across the whole screen at once), `FA_HOURLY` (clocks: frame = hour), `FA_NON_LOOPING`, `FA_LOOPING` (with
  freeze-first/recycle), `FA_RANDOM_FRAMES`. Every frame of the cycle is reachable in every mode.
- `Frame_animator::get_next_frame` re-initialises if usecode moved the frame out of range (`:372-375`). That means usecode can
  jump between cycles of the same shape (state changes such as lit/unlit torches built as separate cycles).

**Rule A1:** for animated shapes, an override set is valid only if it covers **whole cycles** `[k*cnt, (k+1)*cnt)` (clipped).
Cycles are independent: covering cycle 0 but not cycle 1 is a *state* difference and acceptable as a warning, not a popping error.

### 3.2 Terrain flats (shapes < 150, `!is_rle()`)
- Flats are baked once per chunk into the 128x128 `rendered_flats` cache (`chunkter.cc:248-268`) via `paint_tile`
  (`chunkter.cc:86-133`), and blitted per chunk (`gamerend.cc:519-531`). **Nothing re-renders them per frame.** Flats never
  frame-animate: `set_terrain` only creates objects for RLE pieces (`chunks.cc:730`). Water and lava animate purely through palette
  rotation (`gamewin.cc:1063-1068`: ranges `E0-E7` (dir 1), `E8-EF`, `F0-F3`, `F4-F7`, `F8-FB`, `FC-FE`).
- Flats are never reflected. The frame number is masked `&31` (`vgafile.cc:443`, `:912-914`), so the override key is `(shape, frame&31)`.
- All frames of one shape are the same type (flat or RLE), as asserted at write time (`vgafile.cc:947-949`).
- Fill under RLE terrain pieces: `paint_tile` paints a **neighbouring flat** (skipping the void tile 12:0) under every RLE
  terrain piece (`chunkter.cc:92-131`). Hi-res must run the same selection and use the chosen neighbour's hi-res, so the gap
  fill matches.
- Coupling between flats is **spatial**. Tiles abut on an 8 px grid and many shapes are designed to tile with each other
  (coast and transition sets). Two failure modes:
  (a) a hi-res tile next to an NN tile: hard style seams;
  (b) two hi-res tiles upscaled independently: colour or edge discontinuities at the tile border. This also happens when
  *everything* is overridden, if the upscaler saw each 8x8 tile alone.

**Rule T1 (pipeline):** upscale flats *in context*. Use padded tiles (with real neighbours from the map, or wrap-around for
self-tiling frames), or upscale whole chunk mosaics and cut them back into tiles. **Rule T2 (validator):** mine the set of
horizontally and vertically adjacent `(shape:frame, shape:frame)` pairs from `u7chunks`/`u7map` (the same data
`Chunk_terrain` reads, `chunkter.cc:139-162`). For each pair where at least one side is overridden, compute the seam error
(mean colour difference across the shared hi-res edge) and compare it with the NN baseline. Flag pairs whose ratio exceeds a
threshold. **Rule T3:** recommend (warn) "family completeness": the shapes connected by frequent adjacencies should be
overridden together. Families are clusters of the adjacency graph, not a hard-coded list.

### 3.3 Actors (body shapes)
- Direction encoding: `rotate[8] = {0,0,48,48,16,16,32,32}` (`objs/objs.cc:106`). North = frames 0-15, south = 16-31,
  east = `16|32` (transpose of south), west = `0|32` (transpose of north). `get_dir_framenum` (`objs/objs.h:208-215`).
  Actor shapes normally have 32 real frames, so east and west are always synthesized reflections (`vgafile.cc:915-917`).
- Walking: NPC `{0,1,0,2,0}` and avatar `{0,1,2}` per direction (`actors.cc:134-147`). Attacks: `{3,4,5,6}`, `{3,7,8,9}`, etc.
  (`actors.cc:152-160`). Substitution of missing frames: `visible_frames` (`actors.cc:116-129`).
- Outline (hit, charmed, poisoned, and so on) is drawn around the **actor frame only**, after the weapon (`actors.cc:2093-2111`).
  The low-res outline algorithm marks the ends of scanlines and the first and last lines (`vgafile.cc:640-699`). An outline from
  the low-res mask drawn over hi-res art will not hug the hi-res silhouette. The outline must be computed from the art actually
  painted (hi-res mask, stroke about S/2..S hi-res px).
- Invisible actors: `paint_rle_transformed` over the shape's own pixel area (`vgafile.cc:596-634`). Same requirement: use the hi-res mask.
- Actors are always painted with translucency enabled (`Actor::paint` -> `paint_shape(xoff, yoff, true)`, `actors.cc:2090`),
  **unless** a palette transform is set. The remap path ignores translucency (`shapeid.h:176-181`).

**Rule A2:** an actor shape is overridden as a unit for frames `0..31` (the `|32` variants follow automatically from section 2.3).
A partial actor causes per-step popping while walking (`0<->1<->2`) and per-swing popping in combat (`3..9`). Strict mode
disables the whole actor shape if any non-empty low-res frame in 0..31 lacks a hi-res frame.

### 3.4 Actor + weapon composite
Flow (`actors.cc:2118-2147`, `2163-2234`):
1. `get_weapon_offset(myframe & 0x1f)` gives `(actor_x, actor_y)`, the hand point of the actor frame
   (`shapeinf.h:983-992`; 32 frames x 2 bytes, loaded from `wihh.dat`, `shapevga.cc:807-838`; values >63 are normalised to 255 = "no
   weapon", `:830-832`).
2. The weapon frame comes from the actor frame: 4/7/22/25 -> 4, 5/8/21/24 -> 3, 6/9/20/23 -> 2, casting 14/30 -> 5, 15 -> 6, 31 -> 7,
   else 1 (`:2177-2211`). Then `|= myframe & 32` (`:2212`).
3. `info(weapon).get_weapon_offset(weapon_frame & 0xf)` gives `(wx, wy)`, the grip point (`:2215-2217`).
4. `weapon_x = wx - actor_x`, `weapon_y = wy - actor_y`, swapped if the actor is reflected (`:2223-2228`).
5. Weapon painted at `actor_origin + (weapon_x, weapon_y)` on top of the actor (`:2133-2143`). Invisible weapons use `paint_invisible`.
   The weapon gets a fresh `ShapeID`, so **no palette transform is applied to weapons** (`:2124`).
6. Dirty rect: low-res weapon bbox shifted by the offsets, +4 px (`actors.cc:776-792`).

The hand point is the low-res pixel at `origin - (actor_x, actor_y)` and the grip is at `weapon_origin - (wx, wy)`. Under the
top-left convention, both map to the top-left sub-pixel of their pixel, so **hi-res offset = S x low-res offset is exact
alignment** when both arts keep the canonical hotspot (G1-G3). Mixed cases (hi-res actor + NN weapon, or the reverse) also align.

Consistency issues that remain:
- **Rule W1:** a weapon's in-hand frames `{1,2,3,4}` (and `{5,6,7}` for the casting shape, `Actor::casting_shape`,
  `actors.cc:749-756`) form one group. They swap every attack. Ground frame 0 is independent.
- **Rule W2:** actor attack frames `3..9` and weapon frames `2..4` are displayed *simultaneously*. Mixing hi-res actor and NN weapon is
  allowed (alignment is exact) but is a style warning.
- **Rule W3 (art):** the hi-res hand must be drawn so that the grip sits on the S x S cell at `S*(-actor_x, -actor_y)`.
  Precision finer than S hi-res px needs the sidecar described in section 5.
- **Rule W4:** an explicit hi-res reflected actor frame (if allowed at all, see section 3.5) must keep the hand at the
  transposed position, because the engine always swaps the offsets (`:2226-2228`).

### 3.5 Reflections (`frame | 32`)
- Synthesized only when `framenum >= nframes_in_file && (framenum & 32)` (`vgafile.cc:915-918`). In shapes with >32 real
  frames, `f|32` may be a real frame, and those shapes get `no_bit5_frame_reflection` for dims (`shapeid.cc:122-131`,
  `shapeinf.h:754-760`). Reflections exist only for RLE (flats are masked `&31`).
- Users of reflection: actor east/west (section 3.3), objects rotated in ES or by barges (`shapeinf.cc:521-544`: non-barge objects
  `frame ^ (quads%2)<<5`), weapon frames in hand (`actors.cc:2212`), dead bodies (`actors.cc:4360`), animated objects (section 3.1).

**Rules R1-R4**
- **R1 (default):** never ship hi-res for a synthesized reflection. Derive it by `Shape_frame::reflect()` of the hi-res base
  frame (exact, section 2.3).
- **R2 (explicit reflected override, opt-in):** allowed only when `f|32 >= nframes_lowres` (a synthesized low-res reflection) **and**
  the base `f` also has a hi-res frame. It must satisfy G1-G5 against the *transposed* low-res frame. Use case: fix
  handedness (shields, belts) that transposition mirrors. If the base `f` has no hi-res frame, reject: one direction would be
  hi-res and the opposite NN.
- **R3:** for shapes with >32 frames, `f|32 < nframes_lowres` is a real frame and follows the normal rules. Never auto-transpose it.
- **R4:** cache key normalisation (`shapes.md` §7.4): flats use `frame&31`, RLE uses the requested frame. A hi-res reflection is
  cached at `frames[f|32]` in the companion `Shape`, exactly as low-res (`vgafile.cc:817-823`).

### 3.6 Barges (ships, carts, flying carpet)
- Rotation by `get_rotated_frame` (`shapeinf.cc:521-544`; callers `barge.cc:455,486,513`). For barge parts, 90 degrees =
  `(f^32)^(1|3)` and 180 degrees = `f^2`. Starting from frame 0 the orbit is `{0, 33, 2, 35}`, i.e. base frames
  `{0,1,2,3}` (1 and 3 are seen transposed). Seats rotate `dir = f%4` (`:526-528`).
- Animation without `Animation_info`: cart wheel `((f+1)&3)|(f&32)` and draft horse `((f+4)&15)|(f&32)` (`barge.cc:640-648`).
- A ship is dozens of IREG objects (deck, rails, sails, wheel) moving together. Partial overrides are very visible.

**Rule B1:** for shapes with `barge_type != generic` or `is_barge_part()`, the group is the 4-frame quad `f & ~3 .. +3`
(seats and parts). Wheels use frames `0..3`, draft animals `0..15`. **Rule B2 (warn):** all shapes that occur together in a
barge (from IREG data: objects grouped by `Barge_object`) should be overridden together.

### 3.7 Sprites and missiles (`sprites.vga`, and `shapes.vga` for missiles)
- `Sprites_effect` cycles all frames of the sprite (`effects.cc:388-443`, reps -1/-2/-3/n). **Group = whole sprite shape.**
  `has_trans` is always true for `SF_SPRITES_VGA` (`shapeid.cc:567-569`).
- Projectiles: if a shape has >=24 frames, frames `8..23` are the 16 directions (`effects.cc:619-626`). Rotating missiles
  (axes, boomerangs) cycle `8..23` (`effects.cc:743-749`). **Group = frames 8..23.** Explosions cycle all frames (`effects.cc:974`).

### 3.8 Paperdolls and gumps (later phases, same mechanics)
- The paperdoll is a composite at hard-coded gump-relative positions (`Paperdoll_gump.cc:137-160`): body, belt, head (with
  or without helm), arms (by arm type), and items (`paint_body/belt/head/arms/object`, `:574-688`), each with its own hotspot in
  `paperdol.vga`. Hotspot rules G1-G3 keep the seams (neck, shoulders) aligned. **Group = one `Paperdoll_npc` set**
  (body frame + head frames + arms frames + belt) and **per item: all frames that can be selected for one spot** (gender
  variants `f/f+1`, `Paperdoll_gump.cc:619-622`; arm-type variants `:634-637`).
- Gump buttons: pressed = `prev_frame+1` (`Gump_button.cc:76-78`). **Group = frame pair.**
- Fonts: glyph frames rendered side by side, advance = low-res width (`font.cc:281,320`). **Group = whole font shape.**
- Faces (`faces.vga`): frames switch during conversation. **Group = whole face shape.** Cursors (`pointers.shp`, remapped in
  `mouse.cc:221-225`): direction arrows. **Group = whole shape.**

### 3.9 Structures built from many shapes
Walls, roofs, floors of buildings, mountains and fences are independent objects, and the engine has no notion of "a
building". Consistency is a pack-authoring concern. The validator can only *warn*. Two options: (a) co-occurrence mining from
IFIX/IREG (shapes whose instances frequently overlap or abut on screen), or (b) artist-declared groups in the pack manifest.

---

## 4. Palette consistency rules

The hi-res override should be **8-bit indexed with the game palette** (`shapes.md` §4.5). Every palette effect then
works through the existing LUT paths, *as long as index classes are preserved*:

| Effect | Code | How it touches indices | Rule |
|---|---|---|---|
| NPC remap by `palette_transform` (shift, xform, ramp remap) | `shapeid.h:309-356`, `shapeid.cc:603-641`; set by cheat screen `cheat_screen_actors.cc:903-954`, jukebox egg `objs/egg.cc:161` (shift +147), checkbox `gumps/Modal_gump.cc:197` | 256-entry LUT, `paint_rle_remapped` | **P1:** remappable shapes (actors, eggs, gump checkmark) must be indexed, never truecolor. **P2 (warn):** each hi-res pixel's palette ramp (`Palette::get_ramps`, `palette.cc:581+`) should equal the ramp of its low-res parent pixel, or of a 3x3 neighbour. Otherwise a ramp remap recolours different regions (for example, skin turning into "cloth" colour). |
| Translucency (shadows, glass, ghosts) | xforms for indices `0xff - xfcnt .. 0xfe` (`vgafile.cc:556-583`); enabled per shape `has_translucency()` or always for sprites (`shapeid.cc:562-569`); actors forced on (`actors.cc:2090`) | Pixels in the xform range blend | **P3:** in shapes that paint translucently, the hi-res frame may use xform indices only where the low-res parent (or a 1-px neighbour) does. In other shapes, these indices are cycling colours (see P4). |
| Palette cycling (water, lava, magic) | `gamewin.cc:1063-1068` ranges `E0-E7, E8-EF, F0-F3, F4-F7, F8-FB, FC-FE` | Colours rotate by index | **P4:** a hi-res pixel in cycling range R requires a low-res parent pixel (or 1-px neighbour) in R. Conversely, a low-res pixel in R should keep at least one hi-res sub-pixel in R. Otherwise the animated region shrinks or grows and phases mismatch at NN-neighbour seams. Pixels in `E0..FE` from a palette-mapped upscaler are almost always wrong (nearest-colour drift) and must be remapped to the closest *non-special* index. |
| Invisible / outline / hit flash | `vgafile.cc:596-699`, special pixels `shapeid.h:121-128` | Uses the mask only | Uses the hi-res mask (section 3.3). |

Note: when `palette_transform != 0`, translucency is skipped entirely (`shapeid.h:176-177`). This is the existing behaviour
and must not be "fixed" by the hi-res path, so that low-res and hi-res stay identical.

---

## 5. Hi-res weapon and hand anchors (optional sidecar)

`wihh.dat` holds 1 byte per axis, the values used are 0..63 (`shapevga.cc:822-834`), and it is written back by ES (`shapewrite.cc:465-477`).
It cannot express hi-res precision, and changing its format would break ES and other tools. Proposal:

- Default: hi-res offset = `S * (wx - actor_x, wy - actor_y)`, swapped on reflection. This needs no data and is exact for NN and for
  hotspot-faithful art.
- Optional sidecar `hires/x<S>/wihh_fine.txt` (or a Flex entry in the companion VGA) with signed fine deltas
  `(dx, dy)` in hi-res px in `[-(S-1), S-1]`, per `(actor shape, frame&0x1f)` and per `(weapon shape, weapon_frame&0xf)`.
  Effective hi-res displacement = `S*(wx-ax) + (dwx - dax)`, and the same for y. Swap x/y when `myframe & 32`, mirroring
  `actors.cc:2226-2228`.
- The fine delta must not move the weapon outside the existing dirty slack. `|delta| <= S-1` stays well inside the 4 game px
  (`actors.cc:791`).
- Only usable when *both* actor and weapon have hi-res frames. If either is NN, ignore the fine delta for that pair (it
  describes art that is not being drawn).

---

## 6. Normative rule set and runtime policy

### 6.1 Group derivation (mechanical, from engine data)
For each `(file, shape)` the validator (and the runtime, at pack load) computes groups:

1. `shapes.vga` animated (`info.is_animated()`): cycles of `cnt = aniinf.frame_count` (or all frames) as in section 3.1.
2. Actor shapes (any shape used as an NPC or monster body: NPC table shapes, `monster.dat`, avatar skins): frames 0..31 as one group.
3. Weapon shapes (`wihh` entry present): `{1,2,3,4}`. Casting-frames shape: `{1..7}`. Frame 0 alone.
4. Barge types and parts: quads of 4 frames. Wheel `{0..3}`, draft animal `{0..15}`.
5. Missile shapes with >=24 frames: `{8..23}`.
6. `sprites.vga`, `faces.vga`, `fonts.vga`/font shapes, `pointers.shp`: whole shape.
7. `gumps.vga`: frame pairs for button shapes (where known), otherwise whole shape (warn only).
8. `paperdol.vga`: per-NPC set and per-item selectable frames (from `Paperdoll_npc` and `Paperdoll_item` info).
9. Everything else: each frame is its own group (static objects), plus advisory families from adjacency and co-occurrence
   mining (flats T2/T3, structures section 3.9, barges B2).
10. Low-res-empty frames are excluded from groups (G4). Synthesized reflections are excluded (R1).

### 6.2 Activation policy (runtime)
- **strict (default):** at pack load, a group becomes *active* only if every member frame has a valid override (G1-G6 and P1
  pass, and the source-CRC matches the current low-res frame, `shapes.md` §7.3). Otherwise every member of the group is drawn
  NN. This prevents popping by construction.
- **lenient (dev, `hires_partial=lenient`):** per-frame activation, with log lines for incomplete groups.
- **CRC mismatch** (a mod or patch changed the low-res frame, ES live edit followed by `reload_shapes`, `shapeid.cc:420-423`): the
  frame is invalid, so under strict mode the whole group falls back. Imported shapes (`vgafile.h:357-377`, `shapeid.cc:205-246`)
  are keyed by logical number but checked by CRC of the actual source frame.
- The group decision is computed once, stored in the hi-res store as `state = active | inactive(reason)`, and consulted by the paint
  hook. There is no per-paint group logic.

### 6.3 Severity table (validator output)

| ID | Check | Severity |
|---|---|---|
| G1 | flat / font extents exactly S x | error |
| G2 | opaque pixels inside canonical S x bbox | error |
| G3 | oFFs equals canonical hotspot | error |
| G4 | emptiness matches | error |
| G5 | silhouette within 1-px dilation; coverage >= 90 % | warn (error if fewer than 50 % coverage or pixels outside the dilation) |
| G6 | flats opaque | error |
| R2 | explicit reflected frame without hi-res base / for a real frame | error |
| R3 | override for `f|32` treated as reflection although real | error |
| A1/A2/W1/B1/sprite/missile/font groups incomplete | | error in strict profile, warn in lenient |
| W2/B2/T3/section 3.9 families incomplete | | info/warn |
| P1 | truecolor override for remappable shape | error |
| P2 | ramp mismatch ratio > threshold | warn |
| P3/P4 | xform / cycling index class violations | error above a small tolerance (for example >0.5 % of pixels), else warn |
| T2 | seam error ratio vs NN baseline above threshold | warn |
| CRC | source CRC missing or mismatching | error (pack built against other data) |
| extra | override for nonexistent frame (>= nframes and not a reflection) | error |
| dup | same key in loose PNG and companion VGA with different content | warn (PNG wins, `shapes.md` §7.2) |

### 6.4 Validator shape (tooling)
- Implement it as a C++ tool beside `tools/ipack` so that it can reuse `Vga_file`, `Shape_frame`, `Shapes_vga_file::read_info`,
  `Palette` and `Chunk_terrain` parsing. A Python version would have to re-implement RLE, TFA and `shape_info.txt` parsing.
  Run it as `exult_hirescheck --game <dir> --pack <hires-root> --scale 6 --profile strict --report out.json`.
- Inputs: the base game static dir plus patch (the same `<STATIC>/<PATCH>` resolution as the engine), the pack (PNG tree and/or
  companion VGA), and the optional manifest with declared groups.
- Outputs: a JSON report (per key: status, failed rules, metrics) and a summary. Exit code != 0 on errors, for CI.
- The same group and rule code should live in a small library (`shapes/hires_rules.{h,cc}`) used by **both** the engine loader (strict
  activation) and the tool, so the two can never disagree.
- Test suite (unit, no game data needed): synthetic frames for G1-G6; the transpose-commutation property (random RLE frame,
  S in {2,3,6}, `reflect(upscale(f)) == upscale(reflect(f))`); weapon offset math (S x and swap); group derivation for
  `Animation_info` cycles including clipping (`animate.cc:341-349`); barge orbit closure; strict-mode fallback when one
  member is missing or has a bad CRC; P3/P4 index-class checks. Integration tests (optional, game data present): run the
  validator over an NN-generated pack (every check must pass trivially, because NN is the identity of the rules), then mutate
  single frames and assert the expected rule IDs.

---

## 7. Hooks: where this lands in code

- `shapes/vgafile.{h,cc}`: `Shape::read` / `reflect` semantics for the companion store (R1-R4); `Shape_frame::reflect` reused on
  hi-res frames.
- `shapeid.h:171-196` (`Shape_manager::paint_shape/paint_invisible/paint_outline`): select the hi-res frame only when the
  group is active; outline and invisible paths use the hi-res mask.
- `shapeid.h:348-356` (`ShapeID::paint_shape`): unchanged LUT logic; the table is applied to hi-res indices (P1).
- `objs/chunkter.cc:86-133, 248-268`: hi-res flat cache, with the same neighbour selection for the RLE gap fill.
- `actors.cc:2118-2147, 2163-2234`: scale offsets by S at paint time only (keep `figure_weapon_pos` in game px), plus the optional fine
  delta. `actors.cc:762-796` stays in game px.
- `objs/animate.cc`: no change. Animation groups are read by the rules library only.
- `objs/barge.cc`, `effects.cc`, `gumps/Paperdoll_gump.cc`, `shapes/font.cc`: no logic change. Group definitions are mirrored in the
  rules library (keep in sync, see Risks).
- `shapes/shapevga.cc:807-838`: unchanged. Hi-res anchors go in a sidecar.

---

## 8. Risks and open points

- **Implicit groups are scattered.** Barge wheel and horse cycles, missile frames 8..23, weapon frame mapping and actor
  sequences are hard-coded in several files. The rules library duplicates that knowledge. If upstream changes it, groups silently
  drift. Mitigation: unit tests that pin the tables, and, where cheap, expose the engine constants (for example, move the weapon
  frame switch into a shared function).
- **Actor identification:** knowing "which shapes are actor bodies" needs NPC data, `monster.dat` and skins. Shapes used as bodies
  only by usecode may be missed. Fallback heuristic: shapes with `wihh` actor offsets, or with 32 frames whose frame 13 is
  a lying pose, should also be treated as actors.
- **Silhouette thresholds** (G5, P2-P4, T2) need tuning on real AI-upscaled data. Start with warnings, then promote to errors.
- **Strict mode can hide a lot of art** while a pack is being built (one missing frame disables a whole actor). The report must
  list "frames blocking group X" clearly.
- **Diffusion upscalers ignore the palette.** Re-quantisation that avoids the special ranges (`E0..FE` cycling, `F4..FE` xforms)
  except where the low-res parent has them is mandatory in the art pipeline. Otherwise P3/P4 will fail en masse.
- **Explicit reflected frames** (R2) also need hi-res-aware dims or footprint art. The footprint swap is engine-driven
  (`shapeinf.h:754-760`), so art that is not a true transpose may look misplaced relative to the footprint.
- **Truecolor overrides** (if ever wanted for non-remappable static objects) would need a separate paint path that bypasses
  `palette_transform`, translucency xforms and cycling. Keep them out of scope for v1.
