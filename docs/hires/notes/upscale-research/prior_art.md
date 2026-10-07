# Prior art: AI-upscaled art packs for classic 2D, tile-based and isometric games (2018-2026)

*Lessons for a 6x (8x8 → 48x48 per tile) override pack for Exult / Ultima VII*

Research date: **2026-10-04**. Scope: community and official projects that put AI-upscaled (or otherwise high-resolution) art into old palette-based games, plus the replacement-pack formats used by emulators and engines. For each project: how the pipeline worked, how it dealt with palettes, seams and transparency, and what players criticised. Model and API choices are covered in the sibling reports `diffusion.md` and `api_models.md`. This file looks only at what other projects did and what we should copy or avoid.

**Verification legend** (applies to every claim below):

| Tag | Meaning |
|---|---|
| **[V]** | Page or source file fetched and read in this session (Oct 2026). Quotes are verbatim. |
| **[S]** | Search-engine snippet only. The page could not be fetched (Cloudflare or Anubis wall, 403). Lower confidence. |
| **[L]** | Checked against local files: `/home/simonea/ultima7_exult/exult-1.12.1` or `/home/simonea/ultima7_exult/art_original/manifest.json`. |
| **[K]** | General knowledge, not re-checked this session. Used only for routine facts such as pip commands. |

---

## 0. TL;DR: the 15 lessons

1. **Nobody has shipped an AI hi-res pack for Ultima VII, Exult, U8 or the Underworlds.** The closest analogues are the Build-engine upscale packs, Doom Neural Upscale, *Diablo 1 4K* (in development, Oct 2026), Project-IE-4k for GemRB, and VCMI's HD-mod support for Heroes III.
2. **Every pack that players praise had heavy manual work behind it.** Moguri redrew "all 11k layer edges" by hand. hidfan hand-fixed all Doom transparency masks. The RE Seamless HD Project rebuilt every mask with a custom tool. Fully automatic ESRGAN output on pixel art draws "vaseline" and "smeared" complaints (ResetEra thread, July 2025).
3. **Upscale past the target, then come down.** hidfan went 8x → 2x ("the 4x and 8x versions have too much funny AI artistic style artifacts"). Leilu's Baldur's Gate pack went x4 → x1. Moguri 9.0 "mixed with the original result". For us that means producing at 8x or 12x, area-downsampling to 6x, and quantising last. This matches the "render high, downscale at the end" idea the user already liked.
4. **AI output drifts in colour and contrast, so pin it back to the original.** hidfan: "the contrast is changed, bright details are brighter, and dark one are darker too". Fixes in prior art: manual "letting the original texture color appear", per-light-ramp quantisation (diablo1-4k), and the colour-fix step that diffusion SR tools use (StableSR `--colorfix_type wavelet|adain`).
5. **Special palette indices must never go through the network.** Examples: Duke 3D fullbrights (upscaled separately as black-masked copies, then "keyed in"), Quake fullbrights (need a "glow map"), Fallout and OpenTTD palette animation, VCMI player colours and shadows (split into `-overlay` and `-shadow` images), and Raze "indexed hightiles" (HD art stored as palette indices and recoloured by the engine). U7's 0xE0-0xFF block falls in the same top-of-palette range that every one of these engines had to special-case.
6. **Transparency: do not GAN-upscale binary masks.** Options seen in prior art: threshold them (ESRGAN fork `--binary-alpha` / `--ternary-alpha`); regenerate masks from the upscaled image (RE SHDP: "The original mask textures are not used"); clamp HD alpha to the 1x mask dilated by 1 px, and keep hit-testing on 1x data (diablo1-4k).
7. **Seams are two different problems.**
   * (a) Textures that tile with themselves: use wrap padding (ESRGAN fork `--seamless tile`, chaiNNer padding `WRAP`, or Doom's "tile 3x, upscale, crop the centre").
   * (b) Tiles designed to meet specific neighbours, such as U7 shorelines and road edges: upscale in context and re-cut, as Moguri did when restitching FF9 backgrounds and as the Infinity Engine packs did with TIS cuts. **Wrap padding is wrong for (b).** It invents a neighbour (the tile's own opposite edge) that never occurs on the map.
8. **Keys: content hash vs. asset identity.** Emulators key by content hash plus palette hash because they have no stable asset IDs (Dolphin, DuckStation, PPSSPP, Mesen for CHR-RAM games, diablo1-4k). Engines with stable IDs key by identity (Daggerfall Unity `archive_record-frame.png`, Build tile numbers, VCMI file names, Mesen for CHR-ROM games). Exult has a stable `(shape, frame)` identity, so key by identity and add a content hash as a guard, so a pack built for BG is not silently applied to SI or to a modded `shapes.vga`.
9. **Per-scale folders with automatic fallback let a partial pack ship and work.** VCMI uses `sprites2x/3x/4x`; anything missing is scaled with xBRZ. diablo1-4k uses `hd/<S>x/` and picks the highest S at or below the maximum, else 1x. Mesen's "default tile" and Dolphin's `$` wildcard give palette-agnostic fallbacks.
10. **A dump/builder mode is essential.** Mesen's HD Pack Builder records the tiles actually seen in play, can sort sheets by usage frequency, and can save a screenshot where each tile first appeared. Dolphin, DuckStation and diablo1-4k (`DVX_HD_DUMP`) dump exactly what the engine would look up.
11. **Keep game logic at 1x and only render at Sx.** Counter-example: Project-IE-4k rewrote coordinates in ARE/WED/BCS files and needed an `UpScaleFactor` for movement speed, projectiles and overlays in a GemRB fork. After 5 years, "BG2 is still not playable". Good examples: the DevilutionX GL1 renderer draws S× textures in the same logical quad, and OpenTTD treats zoom levels as render-only.
12. **Pixel-art ESRGAN models are a 2019-2021 phenomenon.** OpenModelDB lists no new pixel-art-tagged model after 2021. Newer models (2023-2025: DAT2, RealPLKSR, MoSR, TSCUNet) target game textures and screenshots. Most pixel-art models are **CC-BY-NC(-SA)**. That matters only if we distribute a pack, and prior art says packs derived from game data should be built locally by the user anyway (diablo1-4k, Project-IE-4k).
13. **Diffusion re-rendering reached a flagship mod in 2024.** Moguri 9.0 (11 Aug 2024) used "Upscale re-rendered of all backgrounds, aided by Stable Diffusion, mixed with the original result", on top of an ESRGAN base plus manual edge work. The Fallout sprite experiments (TheGamer, 1 Oct 2022) show the hallucination risk: generative models pull towards genre priors. The Vault Dweller became a Fallout 3 jumpsuit.
14. **Grain, dark areas and small text are where AI fails.** The Baldur's Gate pack reported "The bedding looks more like silk instead of wool. The walls are no longer as porous." RE SHDP reported "melting artefacts on dark corners" and "Small texts ... melting garbage". U7 grass, dirt and swamp are mostly dithered grain, and signs, books and runes are text-like. Plan for both.
15. **Mesen's HD-pack format is the best-documented template for an override manifest.** It is a plain-text `hires.txt` with `<scale>`, PNG sheets and `<tile>` rules keyed by tile data + palette. The first matching rule wins. It has named conditions (`tileNearby`, `frameRange`, `memoryCheck`, `randomBackground`), negation and AND, a "default tile" fallback across palettes, `<background>` full-image replacements with priority levels, and a recorder that produces the skeleton pack (section 3.1).

---

## 1. Why the prior art applies: U7 facts it maps onto

* Terrain ("flats", shapes 0-149) are 8x8 indexed tiles. The local extraction counts **3885 flat frames, 3120 used on the map, 369 of which contain reserved indices (0xE0-0xFF)**, plus **10286 RLE frames** and **2105 chunk templates used on the map** [L] (`art_original/manifest.json`, `summary`).
* Exult palette cycling ranges [L] (`exult-1.12.1/gamewin.cc:1043-1048`): `rotate_colors(0xfc,3)`, `(0xf8,4)`, `(0xf4,4)`, `(0xf0,4)`, `(0xe8,8)`, `(0xe0,8)`. Translucent indices 0xEE-0xFE apply only in RLE frames painted translucently. See `docs-hires/analysis/palette.md`.
* Other games reserved the same top-of-palette block:
  * Quake: fullbrights 224-255 [S].
  * Fallout 2: animated 229-254 [V, `fallout2-ce/src/cycle.cc`: slime 229, monitors 233, slow fire 238, fast fire 243, shoreline 248, alarm 254].
  * OpenTTD: animated 227-254 (`PALETTE_ANIM_START = 227`, 28 colours) [V source].
  * Duke 3D: fullbrights 240-254 [V].

  Every project below that kept these effects had to treat that block as a separate class.

---

## 2. Case studies

### 2.1 Doom Neural Upscale 2x (hidfan et al., 2018-2019) [V]

Sources: Doomworld thread (archived copy fetched): first post **2 Feb 2018**, v0.95 **Dec 2018** (TechSpot), v1.0 edited **13 Sep 2019**. https://www.doomworld.com/forum/topic/99021-doom-neural-upscale-2x-v-10/ ; https://www.techspot.com/news/77805-using-neural-networks-visually-overhaul-doom.html

**Pipeline (verbatim from the first post):**

> "The process was to pack doom textures into different 1024x1024 pngs (7 iamges), then get the 8x upscaled versions (using 2 different techniques), then blend those results together as they both have qualitys and issues, downsize to 4096 with bicubic supersampling to blend some noise, then downsize to 2048x2048 with nearest neighbour supersampling to keep the sharpness feeling of original doom textures. the 4x and 8x versions have too much funny AI artistic style artifacts to be used"

* Tools: "Nvidia GameWorks's Super Resolution and Topaz AI Gigapixel" (letsenhance.io and waifu2x were tried and dropped). Output: 2x of all textures and sprites, as PNG overrides in a GZDoom pk3.
* **Colour drift:** "the contrast is changed, bright details are brighter, and dark one are darker too, this had to be cleaned as well by removing them and leting the original texture color appear."
* **Transparency:** "all transparency masks have to be manually enhanced (because AI don't know what to do with binary Black&White yet)".
* **Integration pitfalls** from replies: switching from paletted lumps to PNG means cyan must become real alpha; `/textures/` in a pk3 overrides both flats and composite textures (Gez).
* **Seams:** [S] a ZDoom-forum snippet suggests tiling textures 2x or 3x before upscaling, then cropping the centre ("could be scripted in Photoshop").
* hidfan also released "the untouched 8x and 6x sources, direct from the AI" so others could re-cook. That is a good habit worth copying: keep raw model outputs.

**Lessons for us:** atlas packing gives the model context, but the borders between atlas cells contaminate each other, so pad them. Upscale far beyond the target and come down. Correct colour drift against the original. Handle alpha separately.

### 2.2 Build-engine packs (Duke 3D, Blood, Shadow Warrior, Powerslave, Witchaven; 2019-2021) and indexed hightiles [V]

Source: upscale.wiki "Upscaling Build Engine Games" (last edited 30 Oct 2021, archived copy). https://upscale.wiki/wiki/Upscaling_Build_Engine_Games . The project list (upscale.wiki/wiki/Projects, Dec 2021) names Phredreeke's packs ("primarily Manga109 2x upscales", plus Dedither, Detoon and Fatality models for Shadow Warrior) and Yoswin's (Gigapixel 2-8x).

* **Extract with original indices** (ART2TGA plus the game's `PALETTE.DAT`) instead of GUI exporters that "no longer retain the original color indices".
* **Replacement definition:** `texture 1170 { pal 0 { file "filename.png" nocompress nodownsize }}`. `pal 0` also covers any palette not explicitly defined.
* **Palette swaps.** "Swap palette before upscaling": no mismatch risk, but the upscaling work multiplies. "Palettise your upscaled image and swap palette afterwards": more colours for the upscaler, but "there's the risk of mismatching".
* **Indexed hightiles** (Raze and special EDuke32/NBlood builds): `texture 0020 { pal 0 { file "upscale/tile0020.png" indexed }}`. "you don't have to supply separate palette variants for each image, they are instead recolored in-engine." **This is the closest existing analogue to an indexed 6x U7 pack.**
* **Fullbright / reserved indices (Duke 240-254):** "I recommend making a copy of the game's palette excluding the fullbright colors and applying that to your sprites. In my upscales I made a separate copy of the sprites using fullbrights where everything but the fullbrights are made black, I then upscaled that and keyed in the result on the normal upscale."
* **Narrow colour segments (Blood, cultist palettes 48-79):** "I made multiple palettised copies of affected upscaled sprites, each with a limited segment of the palette available, which were then combined using a series of masks."

**Lessons:** this is exactly the U7 problem: indexed hi-res, reserved ranges, and segments that must not mix. Copy two techniques: the key-in technique for cycling pixels, and segment-limited palettisation.

### 2.3 Quake and Quake 2 [V]/[S]

* Calinou's *quake2-neural-upscale*: ESRGAN (old-arch) with the **Manga109Attempt** model; repo created Dec 2019, last push Feb 2020 [V]. https://github.com/Calinou/quake2-neural-upscale
* func_msgboard ESRGAN thread [V], https://www.celephais.net/board/view_thread.php?id=61666 :
  * jokerman82, 2019-03-09: "fullbright colors are not conserved when upscaling, so these would need an upscaled 'glow map'".
  * Icaro, 2019-03-14: "Don't expect any spectacular change. The difference is very subtile!"
* Engines support `_glow` / `_luma` companion textures for fullbrights [S] (https://quakewiki.org/wiki/DP_GFX_EXTERNALTEXTURES). Quake fullbrights are the last 32 indices, 224-255 [S].

**Lesson:** special-index regions become a separate map (glow, emissive or cycling) produced from the index plane, not left to the network.

### 2.4 Final Fantasy VII and IX (Remako 2019; Moguri 2018-2024)

* **Remako HD (CaptRobau, 2019):** pre-rendered backgrounds 4x with Topaz Gigapixel [S] (DSOGaming, PC Gamer, VentureBeat snippets).
* **Moguri Mod (FF9)**. Official site [V] https://sites.google.com/view/moguri-mod/home , Wikipedia [V], 80.lv [V] (16 Aug 2024):
  * Version 9.0 released **11 Aug 2024**: "a faithful revamp of Final Fantasy 9 ... aided by AI (ESRGAN and Stable Diffusion)"; "Upscale re-rendered of all backgrounds, aided by Stable Diffusion, mixed with the original result"; "Manual redraw of all 11k layer edges and area names".
  * [S] Earlier coverage gives the scale: 740 backgrounds, 5388 animation frames, 1200 light layers. It also notes that backgrounds are stored cut into pieces, "upscaling causes seams to appear or other alignment issues when the backgrounds are reassembled, much of which had to be fine-tuned by hand", and that a sharper but noisier upscale "was mixed manually with the original upscale".
* Reception: the ResetEra thread (5 Jul 2025, below) singles out FF7/8/9 Moguri as the acceptable exception because of the manual corrections [V].

**Lessons:** reassemble the cut-up pieces, upscale the whole image, then re-cut (context!). Treat SD as an assist whose output is blended back, not as the final pixels. Manual edge work is unavoidable where layers meet.

### 2.5 Resident Evil Seamless HD Project (RE1-3, 2019-2023) [V]

Source: https://www.reshdp.com/re1/ ; repo https://github.com/MoArtis/ResidentEvilSeamlessHdProject

* Workflow: "Game data analysis, PC to GameCube texture matching, analysis of mask special cases, mask alpha layers vectorization, texture upscaling, texture recreation."
* Key idea: "a tool that analyzes the game data to **regenerate completely new mask textures** from the upscaled background textures. The original mask textures are not used or processed in any way."
* Failure modes: "The algorithm has an especially hard time with dark areas ... Expect to see a lot of 'melting' artefacts on dark corners and distant parts of the backgrounds." "Small texts will also end up being processed as melting garbage. We replaced them when the result was too distracting."
* [S] Models: "multiple non-mainstream AI models through cupscale/ESRGAN and manual adjustments".

**Lessons:** derive masks and maps (transparency, cycling, translucency) from the HD result plus the 1x class map. Never upscale the masks themselves. Budget manual replacement for text-like art (U7 signs, books, runes, gravestones).

### 2.6 Infinity Engine: AI Denoised Areas (2022) and Project-IE-4k (2020-2025)

**EE AI Denoised Areas** (Leilu, Gibberlings3, **6 May 2022**) [V, archived thread]. https://www.gibberlings3.net/forums/topic/35513-beta-ee-ai-denoised-areas/

* "The area images are AI upscaled x4 and then downscaled back to the original resolution." The final output is 1x (a supersampled denoise), because the author found WED/TIS/PVRZ too complex to raise the in-game resolution.
* Seams: problems "because of the way the images of the game areas are cut" were reduced with argent77's help. Remaining: "visible lines and tiles for all the areas containing for example opening doors or water surfaces". These are the overlay and animated-tile areas, the IE equivalent of U7's cycling water.
* Over-smoothing: "Some elements of the decor lose too much grain"; "grain was necessary. The bedding looks more like silk instead of wool. The walls are no longer as porous."

**Project-IE-4k (pie4k)** (Goddard; created Aug 2020, last push **25 Sep 2025**, GPL-3.0) [V]. https://github.com/Goddard/Project-IE-4k

* "Allows extracting, upscaling, and assembling from all game assets to a new override-X4 directory for use in GemRB."
* It converts 2DA, GAM, MOS, BAM, TIS, ARE, BCS, BMP, CHU, PLT, PNG, PVRZ, WED, WMP and MVE. Upscaling runs through an NCNN service (Vulkan). The README compares upscayl-ultrasharp, ultramix_balanced, RealESRGAN_General_x4_v3, realesr-animevideov3, 4x_NMKD-Siax and others.
* Pain points:
  * "BAM and PLT color configuration is also slightly off" (palette-based recolouring breaks).
  * GemRB "needs some changes to support an UpScaleFactor. Things like movement speed, projectile speed, and size of some overlays ... will need to be increased".
  * "Overall the BG2 is still not playable."
  * About 3 hours to process BG2 on an RTX 4090.

**Lessons:**
* Overlay and animated tiles are where seams show.
* Never scale world coordinates. Keep logic at 1x and render at 6x. Exult's design already separates these.
* Palette-recolour formats (BAM/PLT ≈ U7 NPC palette transforms) need explicit handling.
* A batch "complete" command with resume support is valuable.

### 2.7 Diablo 1: DevilutionX GL1 renderer and *diablo1-4k* (2025-2026) [V]

Sources: https://github.com/glebxxx/diablo1-4k , issues #3 and #10 (both opened **2 Oct 2026**, still open); upstream draft PR diasurgical/DevilutionX#8586 "Implement OpenGL 1.1 based rendering" (as described in issue #10).

This is a design in progress, not a shipped pack, but it is the **most complete public "engine contract" for an S× override of an 8-bit tile/sprite engine** and is almost exactly our problem:

* **Rendering:** draw an S× texture in the same *logical* quad (`glOrtho(0, logicalW, logicalH, 0)` with the viewport at physical size). Game logic stays 1x.
* **Key = content hash (FNV-1a 64)** over a canonical byte stream: kind, w, h, every pixel's palette index (after TRN remap; `0x100` = transparent), then for each *used* index its base-palette RGB, "except indices in the level's animated (colour-cycling) range, which contribute the index only". Light-table and infravision TRNs are excluded and become a tint at draw time.
* **Layout:** `mods/<pack>/hd/<S>x/<kk>/<key16>.png`, PNG RGBA straight alpha, "**exactly S× the 1× size, same anchor**". Optional `hd/manifest.tsv` (key → asset, frame, TRN, palette) for tools.
* **Lookup:** "exact key at the highest available `S` ≤ `HD Max Scale`", else 1x. A size mismatch is rejected and logged once.
* **Alpha:** premultiplied; "**Clamp HD alpha to the 1× mask dilated by 1 px**, so halos never reach outside the clickable silhouette. **Mouse picking stays on 1× data**."
* **Colour cycling:** "textures using animated indices keep the 1× path in this prototype (log a counter). The 'HD with holes' approach is a later item."
* **Dump mode:** `DVX_HD_DUMP=<dir>` writes, on every cache miss, the 1x PNG named by its key plus a `manifest.tsv` line. "What is dumped is exactly what gets upscaled and loaded."
* **LRU budget** for HD textures (default 1024 MB).
* **Quantiser** (`quantize.py`, issue #3): "map RGBA (e.g. AI-upscaled) back to a 256-colour palette two ways: nearest colour, and **per-light-ramp (restrict each pixel to the ramp of its source pixel; keep colour-cycling ranges reserved)**".
* **Tests:** with synthetic packs only, no game data. Key vectors are shared between the C++ engine and the Python tool.
* **Policy:** "packs are built locally from your own game files and must never be published." Users upscale locally with Topaz or Real-ESRGAN.

Also: "HD Diablo" (ModDB; work in progress on DevilutionX) [S]. The Belzebub "Diablo HD mod" changes resolution, not art [S].

**Lessons:** copy almost all of this contract. For Exult the key can be `(file, shape, frame)` with the hash as a guard. Ramp-constrained quantisation is the right default for U7, which has about 17 ramps (`Palette::get_ramps`). Use the dilated-alpha clamp and 1x hit-testing.

### 2.8 Fallout 1 and 2 [V]/[S]

* Palette animation (fallout2-ce `src/cycle.cc`) [V]: slime 229-232, monitors 233-237, slow fire 238-242, fast fire 243-247, shoreline 248-253, alarm 254. Like U7, all near the top of the palette.
* AI sprite experiments with Stable Diffusion (TheGamer, **1 Oct 2022**) [V], https://www.thegamer.com/fallout-classic-sprites-upscaled-ai/ : "The Vault Dweller, for instance, has a blue jacket and pants with a yellow shirt, not a jumpsuit". The generative model replaced the 1997 design with the Fallout 3 one. For simple outfits "the upscale works fairly well".
* [S] Witchunter42's "Remastering Classic Fallouts" blog: 2x (960p) and 3x (1440p) gave "good to great results"; going toward 4K did not.

**Lesson:** generative models drift toward genre priors, and Britannia would become generic fantasy. Keep diffusion denoise low or structure-locked, and always validate by downscaling back to 1x.

### 2.9 Heroes of Might and Magic III: VCMI 1.6 HD graphics (2025) [V]

Source: https://vcmi.eu/modders/HD_Graphics/ ; changelog https://vcmi.eu/ChangeLog/ [S]

* Folders next to the 1x ones: `sprites2x|3x|4x`, `data2x|3x|4x`, `video2x|3x|4x`, "same name and folder structure". "All images that are missing in the upscaled folders are scaled with the selected upscaling filter instead of using prescaled images" (xBRZ, run in background threads since 1.6.3-1.6.4 [S]).
* Palette tricks become separate images:
  * `-shadow` suffix: shadow layer.
  * `-overlay` suffix: "a transparent image with white" that VCMI colourises (flags, hover highlight).
  * Player-colour suffixes `red|blue|tan|...|neutral`.
  * "ensure your base image ... is rgb/rgba image, and not indexed".
* Ubisoft's official HoMM3 HD Edition (2015) reworked "25,000 sprites and bitmaps ... by hand" [S].

**Lessons:** per-scale folders plus algorithmic fallback; special palette semantics become separate layers.

### 2.10 OpenTTD: 32bpp sprites, extra zoom levels, masks and palette animation [V]

Sources: NML docs https://newgrf-specs.tt-wiki.net/wiki/NML:Alternative_sprites ; OpenTTD wiki https://wiki.openttd.org/en/Archive/Community/Users/Planetmaker/About%2032bpp ; source `src/blitter/32bpp_anim.cpp` (master, Oct 2026).

* `alternative_sprites(block_name, zoom_level, type[, filename[, mask_filename]])`, with zoom levels `ZOOM_LEVEL_IN_4X ... ZOOM_LEVEL_OUT_8X` and `BIT_DEPTH_8BPP|32BPP`. The 8bpp normal sprite is always required, as the fallback.
* Mask sprite: "The mask sprite must have the exact same dimensions as the preceeding 32bpp sprite": "index 0 = transparent and the 32bpp sprite will be drawn there" and otherwise "the 32bpp sprite then only defines alpha channel and intensity".
* **Palette animation in true colour**: the `32bpp-anim` blitter keeps a per-pixel 16-bit *anim buffer* (low byte = palette index, high byte = brightness). On each palette animation step it re-renders pixels with `colour >= PALETTE_ANIM_START` (227, 28 colours) as `AdjustBrightness(LookupColourInPalette(colour), brightness)`.

**Lesson:** if we ever move to true-colour hi-res art, this is the proven pattern for keeping U7's water and lava cycling: RGBA plus an index-and-intensity side plane.

### 2.11 The Ultima family

* **Exult** has runtime scalers only: Point, Interlaced, Bilinear, BilinearPlus, 2xSaI, SuperEagle, Super2xSaI, Scale2x, Hq2x-4x, 2xBR-4xBR [L] (`imagewin/imagewin.cc:72-85`). None reaches 6x natively. No AI art pack was found. Custom-art modding exists, for example G. W. Chapman's custom U7 shapes as PNG/.shp (Ultima Codex, Sep 2024) [S].
* Ultima Codex (Aug 2019) speculated about AI upscaling for UnderworldExporter. It noted most Ultima art is "probably too low-resolution to really make it work" [S]. That piece predates 2024-2026 models and context-based pipelines.
* **Pentagram (U8, now in ScummVM)** supports higher resolutions and "High-quality antialiasing" but has no art packs [S]. **UnderworldExporter** moved from Unity to Godot in Jan 2024; no HD packs [S].

### 2.12 Others, briefly

* **OpenRA Tiberian Dawn HD**: loads the C&C Remastered Collection art from the user's own install, with a classic/remastered toggle. Playtest 20241116 brought "significantly improved support for HD art assets" [V] (https://www.openra.net/news/playtest-20241116/). This is the "read assets from the user's install, never redistribute" pattern again.
* **Daggerfall Unity**: replacement textures keyed by identity, `archive_record-frame.png`, with optional `_Normal` and `_Emission` maps [S].
* **Minecraft packs as training data**: `2x-Faithful` was trained on the hand-made Faithful 32x pack against vanilla 16x. `8x-Sphax-Alpha-NN` used the Sphax 256x pack as HR and nearest-neighbour downscales as LR [V, OpenModelDB data]. These are **paired data where the LR side is true pixel art**, which is what makes them behave on pixel art.

---

## 3. Emulator and engine replacement-pack formats

### 3.1 Mesen HD packs (NES): the design to borrow from [V]

Sources: https://www.mesen.ca/docs/hdpacks.html ; https://github.com/SourMesen/Mesen/blob/master/Docs/content/hdpacks/_index.md ; the active fork by the HD-pack maintainer, https://github.com/mkwong98/Mesen (release **0.9.9-260511, 11 May 2026**: "play packs with version number up to 109"; documented format 106).

**Files.** A folder named after the ROM, inside `HdPacks/` (or a zip), containing PNG sheets plus `hires.txt`.

**Tags (format 106):**

| Tag | Syntax | Purpose |
|---|---|---|
| `<ver>` | `<ver>106` | Format version, with changelog-driven parsing (`if Version >= 105 ...`) |
| `<scale>` | `<scale>4` | Integer scale for the whole pack. Docs: "suggested to use scales between 1x and 4x"; above 8-10x "will probably have a very hard time running" |
| `<supportedRom>`, `<patch>` | `<patch>[file],[sha1]` | Bind the pack and IPS/BPS patches to a ROM by SHA-1 |
| `<overscan>` | `top,right,bottom,left` | |
| `<img>` | `<img>Tileset01.png` | Sheets are indexed from 0 in order of appearance |
| `<tile>` | `<tile>[img],[tile data],[palette data],[x],[y],[brightness],[default Y/N]` | The core replacement rule |
| `<condition>` | `<condition>[name],[type],[params...]` | Named predicates |
| `<addition>` | `<addition>[tile],[pal],[dx],[dy],[extra tile],[extra pal]` | Draws an *extra* tile next to a matching sprite (fork feature) |
| `<background>` | `<background>[png],[brightness],[hscroll ratio],[vscroll ratio],[priority 0-39],[left],[top]` | Full-image replacement layer with alpha. Parallax. Priority bands relative to bg-priority sprites, bg tiles and fg sprites |
| `<options>` | `disableSpriteLimit, disableOriginalTiles` | |
| `<bgm>`, `<sfx>` | `<bgm>[album],[track],[file.ogg]` | Audio replacement through memory-mapped registers |

**Keys.**
* CHR-ROM games: the tile's *index* in CHR ROM (hex), so identity keying.
* CHR-RAM games: "a 32-character hexadecimal string representing all 16 bytes of the tile", so content keying.
* Palette: always "an 8-character hexadecimal string representing all 4 bytes of the palette"; sprites start with `FF`.

**Rule resolution.**
* "The first matching rule (in the order they are written in the `hires.txt` file) will be used ... conditional tiles MUST be placed before tiles with no conditions."
* **Default tile:** "Whenever a tile appears on the screen that matches the tile data, but has no rules matching its palette data, the default tile will be used instead."
* **Brightness:** reuses one HD tile for fade levels (values above 1.0 allowed since v105).

**Conditions.**
* Built-ins: `hmirror`, `vmirror`, `bgpriority`, `sppalette0-3`.
* `tileNearby` / `spriteNearby` (x/y offsets in pixels, e.g. `<condition>c,tileNearby,-8,0,[tile],[pal]`).
* `tileAtPosition` / `spriteAtPosition`.
* `memoryCheck` / `memoryCheckConstant` and PPU variants, with an optional mask.
* `frameRange` (`frame % divisor >= value`), `spriteFrameRange`.
* `randomBackground` (fork: "randomly replace background tiles with a given probability", e.g. `0.1`).
* Combine with `&` and negate with `!`: `[cond1&!cond2]<tile>...`.

**HD Pack Builder (recorder).**
* "record gameplay of a game from start to finish, attempting to trigger every possible animation", writing PNG sheets plus `hires.txt`.
* Options:
  * Scale/Filter: "Prescale" for plain scaling, or a scaling filter.
  * "Group blank tiles".
  * "Sort pages by usage frequency".
  * "Ignore tiles at the edges of the screen".
  * **"Save frames which the tiles are first shown"**, so artists see the context of each tile.
  * BG and sprite tile type selection.

**Why it fits Exult.**
* U7 flats behave like CHR-ROM tiles (stable identity `(shape, frame)`).
* The U7 palette changes globally (day/night, effects) the way NES palettes do, so a "default tile" across palettes is natural.
* `tileNearby` maps directly onto "this shoreline frame next to that grass frame".
* `<background>` maps onto "replace a whole 128x128 chunk template with a 768x768 painting".
* `randomBackground` maps onto breaking up grass repetition with variants.
* The recorder maps onto "walk Britannia and dump every (shape, frame, neighbour-context) actually drawn".

**What not to copy.**
* Pixel-offset conditions evaluated per pixel. U7 terrain is static per chunk template, so neighbour context can be resolved offline when the pack is built.
* A single global integer scale with no per-scale fallback. VCMI and diablo1-4k do better here.

### 3.2 Dolphin (GameCube/Wii) [V, source]

`Source/Core/VideoCommon/TextureInfo.cpp` and `HiresTextures.cpp` (master, Oct 2026):

* Name = `tex1_{W}x{H}[_m]_{texhash:016x}[_{tluthash:016x}]_{format}`, with XXH64 hashes.
* For C4/C8/C14 paletted textures the TLUT hash covers **only the palette range actually used** (min..max index present in the texture). Palette entries the texture never uses don't break the match.
* Lookup tries the exact name, then `..._{texhash}_$_{fmt}` (any palette), then `..._$_{tluthash}_{fmt}` (any texture with this palette).
* Optional `arb_mipmap` (artist-supplied mip levels).
* The wiki guide is Anubis-protected and was not fetched.

### 3.3 DuckStation (PS1): Texture Replacement 2.0, merged 29 Sep 2024 [V]

https://github.com/stenzek/duckstation/wiki/Texture-Replacement

* Name `texupload-P4-AAAAAAAAAAAAAAAA-BBBBBBBBBBBBBBBB-64x256-0-192-64x64-P0-14`: texture-index hash, palette hash, size of the original VRAM upload, offset, sub-texture size, and **palette range hashed**.
* `ReducePaletteRange` "Reduces the size of palettes (i.e. CLUTs) to only those indices that are used". Replacements can be png, jpg or webp. The `config.yaml` "aliases" section maps a source ID to a file, so one image can serve many keys.
* The docs warn it "is not going to be compatible with all games".

### 3.4 PPSSPP (PSP) [V]

https://www.ppsspp.org/docs/reference/texture-replacement/

* `textures.ini`: `[options]` (`hash = quick|xxh32|xxh64`, `ignoreAddress`, `reduceHash`), `[hashes]` (`hash = file.png`; an empty value skips the texture), `[hashranges]`, `[filtering]` (per-texture linear/nearest/auto).
* Key format `AAAAAAAACCCCCCCCTTTTTTTT`: address, CLUT hash, texture hash.

### 3.5 GLideN64 / Rice (N64) and GBE+ (Game Boy) [S]

* Rice names `GAME#CRC#FMT#SIZE[#PALCRC]_all.png`. Historically colour and alpha could be supplied as separate `_rgb.png` and `_a.png`, which is separate alpha again.
* GBE+ "manifest" maps hash → image file with a type (DMG/GBC sprite or BG tile) and an optional VRAM address.

### 3.6 Comparison

| System | Key | Palette in key | Scale | Context conditions | Fallback | Recorder/dump | Special colours |
|---|---|---|---|---|---|---|---|
| Mesen HD packs | CHR index or 16-byte tile data | full 4-byte palette | one integer per pack | yes (rich) | default tile across palettes; original tile | HD Pack Builder (frequency sort, first-seen screenshots) | brightness factor |
| Dolphin | XXH64 of texels | hash of *used* TLUT range | any (integer multiple) | no | `$` wildcards | dump | — |
| DuckStation | hash of indices + sub-rect | hash of used CLUT range (optional) | any | no | aliases | dump with many tunables | — |
| PPSSPP | address + CLUT + texels | CLUT hash | any | no | ini mapping | dump to `new/` | — |
| Build/Raze | tile number | `pal N` per palette, or **indexed** | any | no | `pal 0` | — | tints; indexed recolour |
| VCMI | file name | n/a (RGBA) | 2/3/4 folders | no | xBRZ for missing | — | `-shadow`, `-overlay`, player colours |
| OpenTTD | sprite number | n/a | zoom levels | no | 8bpp normal always present | — | 8bpp mask; anim buffer |
| diablo1-4k (2026 design) | FNV-1a of indices + used RGB (cycling: index only) | yes, used colours only | `hd/<S>x` | no | highest S ≤ max, else 1x | `DVX_HD_DUMP` | cycling → 1x for now; light TRN → tint |
| Daggerfall Unity | archive_record-frame | n/a | any | no | original | — | `_Emission` |

---

## 4. A Mesen-inspired manifest for Exult (illustrative sketch, not a spec)

The engine design belongs in the main design doc. This sketch only shows how the prior art combines. Choices and where they come from:

* Identity keys guarded by a content hash (Mesen CHR-ROM, DFU; hash guard as in Dolphin and diablo1-4k).
* Per-scale folders with fallback to Exult's own xBR/Hq scaler or to point scaling (VCMI, diablo1-4k).
* Chunk-level "background" overrides that solve in-chunk seams (Mesen `<background>`).
* Offline-resolved neighbour variants (Mesen `tileNearby`).
* Random variants (Mesen `randomBackground`).
* Explicit pixel-class planes (Raze indexed, OpenTTD mask, VCMI overlay).

```text
# hires.txt (line based, '#' comments, first matching rule wins as in Mesen)
<ver>1
<game>BG static/shapes.vga sha1=3f1c...        # cf. Mesen <supportedRom>/<patch> sha1
<scale>6                                       # pack native scale; engine may downscale (6->2,3)
<format>indexed                                # indexed (8-bit, game palette) | rgba (+ .cls plane)
<img>flats_000.png                             # sheet #0: 8-bit PNG, palette = game palette 0
<img>chunks_000.png                            # sheet #1

# flat terrain frame: <flat>img, shape, frame, x, y [, src_hash]
<flat>0,0x0012,3,0,0,fnv=9a1c0e44b2d6f001      # hash of the 1x indices; mismatch => rule ignored + log
# context variant resolved offline by the pack builder (Mesen tileNearby analogue)
<condition>shoreN,flatNearby,0,-1,0x0016,*     # tile above is water shape 0x16, any frame
[shoreN]<flat>0,0x0012,3,48,0
# whole chunk-template override (Mesen <background> analogue): 128x128 -> 768x768
<chunk>1,0x01A2,0,0
# cheap anti-repetition (Mesen randomBackground analogue; seeded by world tile coords, deterministic)
<condition>var25,random,0.25
[var25]<flat>0,0x0012,3,96,0
<options>fallback=engine_scaler                # missing entries -> runtime xBR/point at 6x
```

Rules taken from the prior art:

* **Size contract:** "exactly S× the 1× size, same anchor" (diablo1-4k). Reject and log otherwise.
* **Cycling and translucent pixels:** in an `indexed` pack they stay as indices, which is how Raze indexed hightiles work. In an `rgba` pack they need a class/anim plane (OpenTTD anim buffer, VCMI overlay).
* **Hit-testing and outlines** use 1x data, and HD alpha is clamped to the dilated 1x mask (diablo1-4k).
* **Builder mode:** dump every `(shape, frame, neighbour signature)` drawn while walking the map, with use counts and a screenshot of where each first appeared (Mesen builder, `DVX_HD_DUMP`).

---

## 5. Tooling the ESRGAN community converged on (status Oct 2026)

| Tool | Version / date | Licence | Notes for us |
|---|---|---|---|
| **chaiNNer** (node GUI, Win/macOS/Linux) | v0.25.0 (19 Oct 2025): "Update Torch & Torchvision to support RTX 50 series", "Users with RTX 5000-series GPUs should install this update and update PyTorch through the dependency manager"; v0.25.1 (23 Oct 2025); repo active Oct 2026 [V] | GPL-3.0 | upscale.wiki recommends it as the main tool. Relevant nodes (source checked): **PyTorch Upscale** (auto tile size with overlap; **Padding: none / reflect-mirror / wrap / replicate**; "Separate Alpha"), **Resize Pixel Art** (AdvMAME, Eagle, SaI, HQx), **Quantize to Reference**, **Dither (Palette)** (None / Diffusion / Riemersma), **Palette from Image**, **Apply Palette**, **Pad**. Model chaining and interpolation. https://github.com/chaiNNer-org/chaiNNer |
| **ESRGAN fork** (JoeyBallentine ← BlueAmulet ← xinntao) | unmaintained; README says use chaiNNer [V] | — | Documents the community's alpha and seam toolbox: `--seamless tile\|mirror\|replicate\|alpha_pad`, `--binary-alpha --alpha-threshold`, `--ternary-alpha --alpha-boundary-offset`, `--alpha-mode bg_difference\|separate\|swapping`, model chaining `1xA.pth>4xB.pth`, interpolation `A:50&B:50`. https://github.com/JoeyBallentine/ESRGAN |
| **Spandrel** (Python model loader) | 0.4.2, 21 Feb 2026 [V] | MIT | Loads 50+ architectures (ESRGAN, Real-ESRGAN, DAT, SPAN, RealPLKSR, MoSR, FDAT, ...) from .pth/.safetensors. `ModelLoader().load_from_file(path)`. The scripting backbone for a reproducible batch pipeline. https://pypi.org/project/spandrel/ |
| **traiNNer-redux** | active; Apache-2.0 [V] | Apache-2.0 | For training a custom U7 model on paired data (section 6.7). https://github.com/the-database/traiNNer-redux |
| **Upscayl / Real-ESRGAN-ncnn-vulkan** | — | AGPL / BSD [K] | Used by Project-IE-4k through NCNN. Easy, but limited model control. |
| **OpenModelDB** (successor of the upscale.wiki model DB) | data repo last commit 14 Aug 2026 [V] | — | https://openmodeldb.info , https://github.com/OpenModelDB/open-model-database . upscale.wiki itself says "We have moved to https://openmodeldb.info/". |

**Windows install on the RTX 5070 Ti (sm_120)** [K]:
* chaiNNer: install ≥ v0.25.1, open *Dependency Manager*, install or update PyTorch. v0.25 ships RTX 50 support, per its release notes.
* Scripted pipeline in `E:\Dati\Ultima7_Upscale`:

```bat
py -3.12 -m venv E:\Dati\Ultima7_Upscale\venv
E:\Dati\Ultima7_Upscale\venv\Scripts\pip install torch torchvision --index-url https://download.pytorch.org/whl/cu128
E:\Dati\Ultima7_Upscale\venv\Scripts\pip install spandrel pillow numpy
```

  Any cu128 or newer wheel index works on Blackwell. VRAM is a non-issue for this data. A 64nf/23nb RRDB ESRGAN is about 64 MB of weights. 8x8 tiles, 128x128 chunks and even 1024² superchunks at 4x fit easily in 16 GB with chaiNNer or Spandrel tiling. DAT2-class models are about 10x slower but still fine.

### 5.1 Models the game-upscale community used (from OpenModelDB data, read locally from the repo) [V]

**Pixel-art and sprite models (all ESRGAN RRDB, 2019-2021):**

| Model | Date | Author | Licence | Purpose (from the DB) | Note for U7 |
|---|---|---|---|---|---|
| 4x-PixelPerfectV4 | 2020-11-16 | Mutin Choler | **WTFPL** | "Sprite Upscaler" | The only permissive dedicated sprite model |
| 4x-Fatality / 4x-Fatal-Pixels / 4x-Fatality-MK2 | 2019-07 / 2020-06 / 2020-11 | Twittman | CC-BY-NC-SA-4.0 | "Upscales medium resolution Sprites, dithered or undithered" | The classic sprite family |
| 4x-NXbrz / 4x-xbrz / 4x-xbrz-dd / 4x-scalenx | 2019-2021 | archerpolation, lyonhrt | CC-BY-NC(-SA) | Learned imitations of xBRZ and ScaleNx; `-dd` also dedithers | Predictable, "algorithmic" look |
| 8x-MS-Unpainter (+De-Dither) | 2021-03-24 | foolhardy | CC-BY-NC-SA-4.0 | "general pixel art, and general dithered pixel art" | Native 8x, good for the 8x → 6x route |
| 8x-Arzenal-v1-1 | 2021-02-10 | computerk | CC-BY-NC-SA-4.0 | "Smooth general pixel art, Minecraft textures" | 8x |
| 2x-Faithful / 2x-Faithful-Lite / 2x-FakeFaith-Lite | 2019-2020 | joey | CC-BY-NC(-SA) | Trained on the hand-made Faithful 32x pack against vanilla | Most faithful in spirit; only 2x |
| 8x-Sphax-Alpha-NN, 4x-PocketMonsters-Alpha, 2x-Gen5-Alpha | 2021-02 | joey | CC-BY-NC-SA-4.0 | "Pixel Art with Tranparency / Alpha Channel"; LR = nearest-neighbour downscale | Alpha-aware; matched degradation |
| 4x-UltraFArt-v3 (Fine/Smooth/Photo), 4x-BigFArt, 8x-HugePaint, 4x-Lady0101 | 2019-2021 | dinjerr | CC-BY-NC(-SA) / none | "Larger-scaled pixels to digital painting" | **Painterly by design**: the oil-painting look users complain about |
| 4x-Rebout / Rebout-Blend | 2019-2020 | lyonhrt | CC-BY-NC-SA-4.0 | Character sprites (KOF '94 Rebout) | |
| 1x-PixelSharpen | 2021-03-19 | dinjerr | CC-BY-NC-SA-4.0 | "Restores blurry/upscaled pixel art" | Useful post-pass |

**Newer game-texture and general models (2023-2025)** that projects used or are likely to use:

| Model | Date | Arch | Licence | Note |
|---|---|---|---|---|
| 4x-UltraSharpV2 | 2025-05-23 | DAT2 (Lite: RealPLKSR) | CC-BY-NC-SA-4.0 | Strong general model; the "ultrasharp" family appears in the Project-IE-4k comparisons |
| 4x-PBRify-UpscalerV4 / 4x-PBRify-RPLKSRd-V3 | 2025-05-19 / 2024-09-23 | DAT2 / RealPLKSR-DySample | **CC0-1.0** | "take existing game textures from older 2000s era games, and upscale them". Permissive |
| 4x-TextureDAT2-otf | 2023-12-13 | DAT2 | CC-BY-4.0 | Game-texture upscaler (GTA V dataset) |
| 2x-AoMR-mosr | 2024-09-21 | MoSR | CC-BY-4.0 | Game textures (Age of Mythology Retold) |
| 4x-Ground | 2021-06-23 | ESRGAN | **WTFPL** | "Upscales ground textures". Directly relevant to grass and dirt |
| 4x-HDCube (1-3) | 2022 | ESRGAN | CC0 / NC-SA | DB warns it is "not suitable for pixel art, small icons and text under 16 pixel" |

Observation: OpenModelDB lists **no new pixel-art-tagged model since 2021**. The community's pixel-art effort stalled, and modern effort went to game textures and diffusion.

---

## 6. Technique catalogue mapped to U7 problems

### 6.1 Seams

| Situation | Prior-art technique | Use for U7? |
|---|---|---|
| Texture tiles with itself (uniform grass/water fill frames) | Wrap padding (`--seamless tile`, chaiNNer `WRAP`); Doom's "tile 3x, upscale, crop centre" [S] | **Yes**, for frames that genuinely self-tile. Verify on the map: the same frame must meet itself |
| Tile designed to meet specific neighbours (shorelines, road edges, cliff transitions) | Reassemble the whole picture, upscale, re-cut (Moguri restitching; BG AI Denoised Areas TIS cuts) | **Yes**, through chunk or superchunk renders. Harvest per-tile results from many instances (median/consensus), or keep per-chunk-template overrides |
| Internal tiling of large images inside the upscaler | Overlap-and-blend tiling (chaiNNer auto tiles "with overlap ... seamlessly recombining"); Ultimate SD Upscale "Seams fix" passes: band pass, half-tile offset, offset plus intersections; "Tile Padding", mask blur "8-16 on 32px padding" [V wiki] | Yes, for superchunk-scale context renders |
| Atlas packing (hidfan's 1024² sheets) | Pad cells with replicated or context pixels | Yes, if we batch tiles into sheets |
| Overlays and animated tiles (IE doors and water) | Still showed seams in the BG pack (2022) | Cycling water and shores need the most validation |

### 6.2 Back to the palette

* **Nearest colour** in a perceptual space, restricted to the static range 0x01-0xDF. Exult's own `find_color` searches only below 0xE0 (see `analysis/palette.md`).
* **Ramp-constrained:** "restrict each pixel to the ramp of its source pixel" (diablo1-4k). For U7, each 6x pixel inherits its source 1x pixel's ramp (about 17 ramps), which keeps hue identity and stops colours bleeding between neighbours.
* **Segment-limited with masks** (Blood): palettise separately per colour family, then composite with masks.
* **Swap before vs. after** (Build guide): for palette-dependent variants, choose explicitly.
* **Dithering:** error diffusion makes seams shimmer and differ per tile. Prior art (Doom, Build) doesn't use it. chaiNNer offers None/Diffusion/Riemersma. Default to none or ordered. Restore grain on purpose instead (6.6).

### 6.3 Palette cycling (water, lava, magic)

Options from the prior art:

1. **Indexed hi-res** (Raze indexed hightiles): cycling pixels stay indices 0xE0-0xFE, and the engine's palette rotation animates them for free. Build a class map from the 1x source upscaled with nearest neighbour or a learned mask; never let the network choose indices in that range.
2. **Key-in** (Duke): upscale a copy where only cycling pixels are kept and everything else is black. Threshold that to decide which HD pixels are cycling. Assign the cycling indices by following the source pixel's phase pattern.
3. **RGBA plus anim plane** (OpenTTD): store index plus intensity per pixel and re-render on palette ticks.
4. **Defer** (diablo1-4k prototype): frames with cycling indices stay on the 1x path. For U7 that is 369 of 3885 flats, mostly water and shores, which are the most visible tiles. Acceptable only as a stopgap.

### 6.4 Transparency and translucency (RLE sprites)

* Upscale alpha separately (chaiNNer "Separate Alpha"; fork `--alpha-mode separate`), then **binarise** it (`--binary-alpha`). Better still, regenerate the mask from the 1x mask through a pixel-art scaler and clamp it (RE SHDP regenerates masks; diablo1-4k clamps to the dilated 1x mask).
* Alpha padding before upscaling, so the transparent-key colour (U7 index 255) does not bleed into edges. upscale.wiki: "most of the newer upscaling apps such as Cupscale have alpha-padding support built-in" [V].
* U7 translucent indices 0xEE-0xFE in translucent RLE frames are a class, like cycling. Keep them as indices in indexed packs.

### 6.5 Supersample, then downscale

* hidfan: 8x → bicubic to 4x → nearest to 2x. Leilu: 4x → 1x. Moguri blends with the original.
* For 6x: produce **12x** (for example a 4x model, then a 3x pass or a second 4x pass with a resize) and **box-downsample 2:1 to 6x**. Or produce 8x with a native 8x model (MS-Unpainter, Arzenal) and area-resample to 6x.
* Clean integer ratios avoid ringing. Quantise only at the very end.
* This is the same principle as rendering the world at 6x and downscaling for 2x/3x display targets.

### 6.6 Colour, contrast and grain lock

* Check: downscale the HD tile to 1x and compare it with the original indices (back-projection). Reject or repair tiles where the error is high.
* Fix low-frequency drift by transferring the original's low frequencies (StableSR's wavelet colour fix, IJCV 2024 [V]) or by ramp-constrained quantisation (6.2).
* Grain loss (BG: "silk instead of wool"; Doom: "denoising work was needed"): pick a model that keeps texture, or re-inject grain from the upscaled original dither pattern. Never ship output that looks smooth to the eye without comparing it with the original side by side.

### 6.7 Consistency across 3885 + 10286 frames

* Same model, same settings, same post-pass for each class (terrain, walls, objects, NPCs). Prior packs mixed models per texture category (Phredreeke: Manga109, Dedither, Detoon, Fatality).
* Model interpolation and chaining (`A:50&B:50`, `1x>4x`) is the community's way to tune a look once and apply it everywhere.
* **Train our own model on paired data.** The Faithful and Sphax examples show that matched degradation (LR = nearest-downscaled pixel art) is what keeps models faithful on pixel art. No hi-res U7 ground truth exists, but pairs could be built from licensed hi-res fantasy tilesets reduced to the U7 palette with nearest-neighbour downscaling. Tool: traiNNer-redux.

### 6.8 Repetition and variation

U7 grass and dirt repeat a few frames. Mesen's `randomBackground` shows packs can add deterministic variety: seed by world coordinates so the result is stable across frames and saves. Optional, and only if it doesn't change meaning.

### 6.9 Manual-curation budget

Plan for it. Moguri: 11k layer edges. hidfan: all masks. RE SHDP: text and worst mask offenders. Mesen packs are mostly hand-drawn, with the builder producing the skeleton. Build a review UI: side-by-side 1x/6x, in-context chunk view, accept or override per frame.

---

## 7. What reviewers say looks bad (catalogue)

| Complaint | Where | Date | Quote / evidence |
|---|---|---|---|
| "Vaseline", smeared, mushed | ResetEra "Does anyone ... like pixel smoothing/upscale shaders/AI upscaling mods?" [V] | 5 Jul 2025 | "why would you want these games to look like you smeared vaseline on your screen?"; pixel art leaves "a lot for your imagination to fill in and AI doesn't have an imagination". Exceptions praised: FF Moguri and RE backgrounds with human refinement |
| Oil-painting / painterly look | General community and press [S] | 2019-2024 | Low-res textures "turn into weird oil paintings". OpenModelDB models such as BigFArt and UltraFArt are *designed* to turn "pixels to digital painting" [V] |
| Lost grain, wrong materials | BG AI Denoised Areas [V] | May 2022 | "The bedding looks more like silk instead of wool. The walls are no longer as porous." |
| AI style artefacts at high factors | Doom Neural Upscale [V] | 2018 | "the 4x and 8x versions have too much funny AI artistic style artifacts to be used" |
| Contrast and colour shift | Doom Neural Upscale [V] | 2018 | "bright details are brighter, and dark one are darker too" |
| Melting in dark areas; text garbage | RE SHDP [V] | 2019-2023 | "melting artefacts on dark corners"; "Small texts ... melting garbage" |
| Seams at cut lines, overlays and water | Moguri [S]; BG pack [V] | 2019-2022 | "seams ... when the backgrounds are reassembled"; "visible lines and tiles ... opening doors or water surfaces" |
| Hallucinated design changes | Fallout SD sprites [V] | Oct 2022 | Vault Dweller turned into a jumpsuit |
| Subtle gain on already-detailed art | Quake [V] | Mar 2019 | "Don't expect any spectacular change" |
| Distinct "AI style" | Doomworld reply (exl) [V] | Feb 2018 | "There is a definite 'style' to how it extrapolates detail" |

---

## 8. Pitfalls and risks (from the prior art)

* Wrap padding on neighbour-dependent tiles produces seams that are only visible on the map.
* Quantising with error diffusion produces per-tile noise and shimmer at tile edges, and it fights palette cycling.
* Letting the network output cycling or translucent indices (or colours close to them) gives random water pixels on land, or static pixels inside water.
* Over-smoothing removes the dithered grain that *is* U7's texture vocabulary.
* Painterly models (UltraFArt, BigFArt, HugePaint) give an oil-painting look across the whole set. Gigapixel-style "detail" invents micro-texture.
* Diffusion at high denoise invents content: genre-prior drift, as in the Fallout jumpsuit.
* Masks upscaled with GANs produce soft or halo edges. Hit-testing on HD alpha changes which objects are clickable.
* Scaling world coordinates or data files instead of only rendering at Sx leads to the Project-IE-4k situation.
* Distribution: most pixel-art models are CC-BY-NC(-SA), and the art is derived from copyrighted game data. Prior art ships tools and pipelines, not art. Build packs locally from the user's own files. Permissive model options: PixelPerfectV4 (WTFPL), 4x-Ground (WTFPL), PBRify (CC0), TextureDAT2/AoMR (CC-BY-4.0).
* Content keys too narrow: diablo1-4k keys by used colours only, excluding animated ones, so a palette tweak doesn't orphan the pack. Exult has global palettes that change at runtime (day/night). Key by identity plus 1x index hash, never by RGB.
* Many upscale.wiki pages, the Dolphin and ZDoom wikis, ModDB and Doomworld are behind Cloudflare or Anubis. Keep local copies of any documentation we rely on.

---

## 9. Recommendations for our pipeline (derived from the prior art)

1. **Pack format:** use a Mesen-style manifest (section 4) with identity keys plus hash guard, `<scale>` and per-scale folders (VCMI/diablo1-4k), first-match rules, offline-resolved neighbour conditions, chunk-template overrides, and a fallback to Exult's runtime scaler.
2. **Indexed 6x art first.** This is what the Raze indexed hightiles prove. Cycling (0xE0-0xFE) and translucent (0xEE-0xFE in translucent RLE frames) pixels keep their indices, decided by a class map, never by the network. Keep the OpenTTD anim-buffer pattern for a later true-colour mode.
3. **Context upscaling for terrain:** render chunk templates or superchunks at 1x with their real neighbours (apron), upscale, cut. For each `(shape, frame)`, pick the consensus across instances. Where a frame's appearance must differ by context, emit conditional variants or chunk-level overrides.
4. **Supersample and quantise last:** produce at 12x (or 8x), box/area-downsample to 6x, run a ramp-constrained quantiser, apply no error-diffusion dither, and run a back-projection check against the 1x original.
5. **Masks:** derive them from 1x masks (pixel-art scaler or nearest neighbour plus a smoothing rule), binarise, clamp to the dilated 1x mask, and keep picking at 1x.
6. **Builder/dump mode in Exult:** list every drawn `(file, shape, frame)` with use counts, neighbour signatures and first-seen screenshots. That list drives the upscaling batch and the review UI.
7. **Tooling:** chaiNNer ≥ 0.25.1 for interactive look development (supports RTX 50). A Spandrel 0.4.x Python batch script (cu128+ PyTorch) for reproducible runs in `E:\Dati\Ultima7_Upscale`. Keep raw model outputs, as hidfan did.
8. **Model shortlist to A/B on real U7 terrain** (all from OpenModelDB; trial cost is minutes):
   * Faithful-leaning: 4x-PixelPerfectV4 (WTFPL), 4x-Fatality-MK2, 2x-Faithful, 4x-NXbrz.
   * 8x route: 8x-MS-Unpainter(-De-Dither), 8x-Arzenal.
   * Ground textures: 4x-Ground.
   * Modern general: 4x-UltraSharpV2, 4x-PBRify-RPLKSRd-V3.
   * Post-pass: 1x-PixelSharpen.
   * Diffusion refinement only at low denoise (≤ 0.2, per the Ultimate SD Upscale guidance) and blended, as in Moguri 9.0. See `diffusion.md`.
9. **Budget manual review.** Text-like tiles and the shoreline/water families get hand attention first.

---

## 10. Sources

| # | URL | What | Status | Source date |
|---|---|---|---|---|
| 1 | https://www.mesen.ca/docs/hdpacks.html | Mesen HD pack docs | [V] | format 106 |
| 2 | https://github.com/SourMesen/Mesen/blob/master/Docs/content/hdpacks/_index.md | HD pack doc source and changelog | [V] | 102-106 |
| 3 | https://github.com/mkwong98/Mesen (docs, `Core/HdPackLoader.cpp`, releases) | HD-pack fork: `<addition>`, `randomBackground`, `spriteFrameRange`, packs ≤ v109 | [V] | release 2026-05-11 |
| 4 | https://www.doomworld.com/forum/topic/99021-doom-neural-upscale-2x-v-10/ (archived copy) | Doom Neural Upscale first post and replies | [V] | 2018-02-02 … 2019-09-13 |
| 5 | https://www.techspot.com/news/77805-using-neural-networks-visually-overhaul-doom.html | Doom 8x → 2x, masks | [V] | Dec 2018 |
| 6 | https://forum.zdoom.org/viewtopic.php?f=46&t=65699&start=15 | tile 3x before upscale, crop centre | [S] | ~2019 |
| 7 | https://upscale.wiki/wiki/Upscaling_Build_Engine_Games (archived) | Build packs, indexed hightiles, fullbright key-in | [V] | edited 2021-10-30 |
| 8 | https://upscale.wiki/wiki/Projects (archived) | Project list (Phredreeke, Yoswin, hidfan, BFBB, Muramasa) | [V] | edited 2021-12-27 |
| 9 | https://upscale.wiki/wiki/Main_Page and /Texture_preparation_and_clean-up and /Texture_injection_and_dumping (archived) | chaiNNer recommendation; alpha padding; emulator list | [V] | 2023 |
| 10 | https://github.com/JoeyBallentine/ESRGAN | seamless and alpha flags | [V] | unmaintained |
| 11 | https://github.com/chaiNNer-org/chaiNNer (releases API; node sources) | v0.25.0/0.25.1, RTX 50; padding, quantise and dither nodes | [V] | 2025-10-19 / 2025-10-23 |
| 12 | https://openmodeldb.info ; https://github.com/OpenModelDB/open-model-database | model metadata (dates, licences, descriptions) | [V] (repo data read locally) | repo commit 2026-08-14 |
| 13 | https://openmodeldb.info/models/4x-Fatal-Pixels ; https://openmodeldb.info/models/4x-PixelPerfectV4 | model pages | [V] | 2020 |
| 14 | https://pypi.org/project/spandrel/ | Spandrel 0.4.2 | [V] | 2026-02-21 |
| 15 | https://github.com/the-database/traiNNer-redux | training framework | [V] | active |
| 16 | https://sites.google.com/view/moguri-mod/home | Moguri 9.0 (ESRGAN + SD, 11k edges) | [V] | 2024-08-11 |
| 17 | https://80.lv/articles/final-fantasy-9-s-upscale-mod-received-a-surprise-update | Moguri 9.0 changelog quote | [V] | 2024-08-16 |
| 18 | https://en.wikipedia.org/wiki/Moguri_Mod | Moguri history | [V] | — |
| 19 | https://www.pcgamer.com/for-final-fantasy-9s-20th-anniversary-play-it-with-the-beautiful-moguri-ai-upscale-mod/ and grokipedia | seams on restitching; 740 bgs / 11k layers | [S] | 2020 |
| 20 | https://www.dsogaming.com/news/final-fantasy-vii-remako-hd-graphics-mod-esrgan-ai-enhanced-texture-pack-has-been-fully-released/ | Remako 4x Gigapixel | [S] | 2019 |
| 21 | https://www.reshdp.com/re1/ ; https://github.com/MoArtis/ResidentEvilSeamlessHdProject | mask regeneration, melting | [V] | 2019-2023 |
| 22 | https://www.gibberlings3.net/forums/topic/35513-beta-ee-ai-denoised-areas/ (archived) | BG AI Denoised Areas | [V] | 2022-05-06 |
| 23 | https://github.com/Goddard/Project-IE-4k | pie4k README | [V] | pushed 2025-09-25 |
| 24 | https://github.com/glebxxx/diablo1-4k (+ issues #3, #10) | Diablo HD contract | [V] | 2026-10-02 |
| 25 | https://www.moddb.com/mods/hd-diablo ; DevilutionX issue #8181 | HD Diablo; Belzebub request | [S] | 2025-2026 |
| 26 | https://raw.githubusercontent.com/alexbatalov/fallout2-ce/main/src/cycle.cc | Fallout cycling indices | [V] | current |
| 27 | https://www.thegamer.com/fallout-classic-sprites-upscaled-ai/ | Fallout SD sprites | [V] | 2022-10-01 |
| 28 | https://fallout.fandom.com/wiki/User_blog:Witchunter42/Remastering_Classic_Fallouts_pt._2_-_Item_%26_Character_Sprites | 2x/3x good, 4x not | [S] | — |
| 29 | https://vcmi.eu/modders/HD_Graphics/ | VCMI HD layout | [V] | VCMI 1.6 (2025) |
| 30 | https://vcmi.eu/ChangeLog/ ; heroes3wog.net 1.6.0 changelog | xBRZ, background threads | [S] | 2025 |
| 31 | https://newgrf-specs.tt-wiki.net/wiki/NML:Alternative_sprites ; https://wiki.openttd.org/en/Archive/Community/Users/Planetmaker/About%2032bpp | 32bpp, zoom, masks | [V] | — |
| 32 | https://raw.githubusercontent.com/OpenTTD/OpenTTD/master/src/blitter/32bpp_anim.cpp ; `src/gfx_type.h` | anim buffer, PALETTE_ANIM_START=227 | [V] | master Oct 2026 |
| 33 | https://raw.githubusercontent.com/dolphin-emu/dolphin/master/Source/Core/VideoCommon/TextureInfo.cpp ; `HiresTextures.cpp` | Dolphin naming and wildcards | [V] | master Oct 2026 |
| 34 | https://github.com/stenzek/duckstation/wiki/Texture-Replacement ; PR #3244 | DuckStation TR 2.0 | [V]/[S] | merged 2024-09-29 |
| 35 | https://www.ppsspp.org/docs/reference/texture-replacement/ | PPSSPP textures.ini | [V] | — |
| 36 | https://github.com/shonumi/gbe-plus/wiki/GBE--Manifest-Files ; GLideN64 Rice naming (OpenEmu #4244) | GBE+ manifest; Rice `_all/_rgb/_a` | [S] | — |
| 37 | https://github.com/Calinou/quake2-neural-upscale | Quake 2 ESRGAN Manga109Attempt | [V] | 2019-12 … 2020-02 |
| 38 | https://www.celephais.net/board/view_thread.php?id=61666&start=29&end=40 | Quake fullbright glow-map remark | [V] | 2019-03 |
| 39 | https://quakewiki.org/wiki/Quake_palette ; https://quakewiki.org/wiki/DP_GFX_EXTERNALTEXTURES | fullbrights 224-255; `_glow`/`_luma` | [S] | — |
| 40 | https://www.resetera.com/threads/does-anyone-here-actually-use-or-even-like-pixel-smoothing-upscale-shaders-ai-upscaling-mods.1236435/ | Reviewer sentiment | [V] | 2025-07-05 |
| 41 | https://forums.dfworkshop.net/viewtopic.php?t=1642 ; DFU TextureReplacement API docs | Daggerfall naming, `_Emission` | [S] | — |
| 42 | https://www.openra.net/news/playtest-20241116/ | OpenRA TD HD | [V] | 2024-11-16 |
| 43 | https://ultimacodex.com/2019/08/speculation-could-underworld-exporter-add-ai-texture-sprite-upscaling/ | Ultima AI speculation | [S] | 2019-08 |
| 44 | https://pentagram.sourceforge.net/ ; https://github.com/hankmorgan/UnderworldExporter | U8 / UW ports | [S] | — |
| 45 | https://github.com/IceClear/StableSR | colour-fix wavelet/adain | [V] | IJCV 2024 |
| 46 | https://github.com/Coyote-A/ultimate-upscale-for-automatic1111/wiki/FAQ | seam-fix passes; denoise 0.15-0.20 to avoid changes | [V] | — |
| 47 | https://jcgt.org/published/0010/02/04/paper.pdf (MMPX) | palette-preserving 2x pixel-art magnifier (exact palette, transparency, single-pixel features) | [S] | JCGT 2021 |
| 48 | local: `exult-1.12.1/gamewin.cc:1043-1048`, `imagewin/imagewin.cc`; `art_original/manifest.json` | U7 cycling ranges, Exult scalers, frame counts | [L] | — |

---

## Fact-check

Adversarial check run on **2026-10-04**, independently of the original research. Method: every claim below was re-fetched from its primary source (GitHub HTML pages and Atom feeds, raw source files, PyPI JSON, the PyTorch wheel indexes, OpenModelDB pages, and a **fresh clone** of `OpenModelDB/open-model-database` at commit `782aac0`, 2026-08-14). The U7 counts were recomputed from the raw `shapes.vga`, not taken from `manifest.json`. The GitHub REST API was rate-limited, so release and commit dates come from the `releases.atom` and `commits/*.atom` feeds. OpenRouter model ids and prices are not claimed in this report (see `api_models.md`), so they were not checked here.

Verdicts: **CONFIRMED**, **REFUTED** (the claim as written is false; a correction is given), and **UNVERIFIABLE** (no primary evidence either way).

| # | Claim (as stated in this report) | Verdict | Primary evidence (fetched 2026-10-04) |
|---|---|---|---|
| 1 | chaiNNer v0.25.0 (2025-10-19) added RTX 50 support; v0.25.1 is from 2025-10-23. The PyTorch Upscale node has none/reflect/wrap/replicate padding and "Separate Alpha". | **CONFIRMED** (one correction) | `releases.atom`: v0.25.0 `2025-10-19T22:07:30Z`, v0.25.1 `2025-10-23T00:16:57Z`. v0.25.0 notes: "Update Torch & Torchvision to support RTX 50 series"; "Users with RTX 5000-series GPUs should install this update and update PyTorch through the dependency manager". `backend/src/nodes/impl/upscale/basic_upscale.py`: `PaddingType {NONE, REFLECT_MIRROR, WRAP, REPLICATE}`. `upscale_image.py`: `BoolInput("Separate Alpha")`. **Correction:** "repo active Oct 2026" is overstated. The last commit on `main` is 2026-07-31, and no release has followed v0.25.1 in almost a year. |
| 2 | Spandrel 0.4.2 was released 2026-02-21 under MIT. It loads ESRGAN, DAT, RealPLKSR, SPAN, MoSR and FDAT. | **CONFIRMED** (two nuances) | PyPI JSON: 0.4.2 uploaded `2026-02-21T01:52:25`, `License :: OSI Approved :: MIT License`. The README lists ESRGAN (RRDBNet), DAT, PLKSR/RealPLKSR, MoSR and FDAT. `architectures/PLKSR/__init__.py` detects DySample (`use_dysample = "to_img.init_pos" in state_dict`), so 4x-PBRify-RPLKSRd-V3 loads, even though its OpenModelDB page still says "Currently compatible only with ComfyUI". **Nuances:** only the repo is MIT; "the code of implemented architectures ... is bound by their original respective licenses", and non-commercial architectures live in the separate `spandrel_extra_arches` package. The figure "50+ architectures" is not in the README. |
| 3 | Blackwell (sm_120, RTX 5070 Ti) needs PyTorch ≥ 2.7 built with CUDA ≥ 12.8. `pip install torch torchvision --index-url .../whl/cu128` works. | **CONFIRMED** (version caveat) | PyTorch 2.7 blog (updated 2025-05-15): "support for the NVIDIA Blackwell GPU architecture and pre-built wheels for CUDA 12.8". The index `download.pytorch.org/whl/cu128/torch/` has Windows wheels up to **torch 2.11.0** (cp310-cp314). **Caveat:** the newest Windows build is **torch 2.14.1**, published only on the **cu130** and **cu132** indexes. The cu129 index stops at 2.9.0. The cu128 command therefore installs a three-minor-versions-old torch. For a new venv, prefer `--index-url https://download.pytorch.org/whl/cu130`. |
| 4 | Model metadata and licences: 4x-PixelPerfectV4 (WTFPL, 2020-11-16, Mutin Choler, 63.9 MB, 300 anime images), 4x-Ground (WTFPL, 2021-06-23), the Fatality family, 8x-MS-Unpainter (+De-Dither), 8x-Arzenal, 2x-Faithful, NXbrz/xbrz and 1x-PixelSharpen (all CC-BY-NC(-SA)), 4x-UltraSharpV2 (2025-05-23, CC-BY-NC-SA-4.0, DAT2, Lite variant = RealPLKSR), PBRify-RPLKSRd-V3 (2024-09-23, CC0-1.0), TextureDAT2-otf (2023-12-13, CC-BY-4.0) | **CONFIRMED** | Model pages on openmodeldb.info, cross-checked against the fresh data clone. 2x-Faithful is CC-BY-NC-4.0 and xbrz is CC-BY-NC-4.0 (NC without SA); the others are as stated. 8x-MS-Unpainter-De-Dither exists as its own entry (2021-03-24, CC-BY-NC-SA-4.0). Two additions: 4x-Ground's author is `tldr_coder`, and its dataset is "Ground textures sourced from Google" (photographic, not pixel art). PixelPerfectV4 is a "Sprite Upscaler" trained on "Random anime images", so it lacks the matched-degradation property that section 6.7 credits for faithfulness. |
| 5 | "4x-PixelPerfectV4 ... the only permissive dedicated sprite model" (also §5.1 and §8) | **REFUTED** (narrowly) | All 53 `pixel-art`-tagged models in the fresh clone were enumerated. Other non-NC entries: **4x-Skyrim-Alpha** (CC0-1.0, 2019-02-21, "Pixel Art with Tranparency / Alpha Channel"), **1x-ArtClarity** (WTFPL, 2021-08-05, a 1x "texture retaining denoiser and sharpener", a permissive alternative to the NC 1x-PixelSharpen post-pass), **4x-HDCube** (CC0, but not meant for pixel art, see #6), and 4x-BS-Deviance, 4x-BS-DevianceMIP and 4x-FSDedither-Manga (GPL-3.0: copyleft, but commercial use allowed). PixelPerfectV4 remains the only permissive **4x model whose stated purpose is sprites**. |
| 6 | "OpenModelDB lists no new pixel-art-tagged model after 2021" (TL;DR 12, §5.1) | **REFUTED** (literally) | In the fresh clone, **4x-HDCube** (2022-05-10, CC0-1.0) carries the `pixel-art` tag. Its own description says it "is not suitable for pixel art, small icons and text under 16 pixel". The substance holds: no model *intended for* pixel art has been added since 2021, and a web search found no 2025-2026 pixel-art SR model elsewhere. Reword the claim to "no new model aimed at pixel art since 2021". |
| 7 | §2.11: Exult's runtime scalers include 2xBR-4xBR, and "None reaches 6x natively." | **REFUTED** (in part) | `exult-1.12.1/imagewin/imagewin.cc:115-134`: Point (`0xFFFFFFFF`), Interlaced (`0xFFFFFFFE`) and Bilinear (`0xFFFFFFFF`) accept any factor, **6x included**. Only the edge-aware scalers (2xSaI, SuperEagle, Super2xSaI, Scale2x, Hq2x/3x/4x, 2xBR/3xBR/4xBR) are limited to 2, 3 or 4 (`SCALE_BIT(n)`). Correct statement: "no *edge-aware* Exult scaler reaches 6x; Point and Bilinear do". This matters for the fallback design: a 6x point fallback already exists. |
| 8 | MMPX is 2x only, so it must be iterated. xBR/xBRZ are the algorithmic fallback (VCMI). | **CONFIRMED** (and one decision-relevant addition) | MMPX abstract (Roblox publications page): it magnifies "by a factor of two in each dimension". The authors ship C++/JS/GLSL code. "MIT" appears only in the README of a C99 port (`ITotalJustice/mmpx`); the original licence text was not read, so treat the licence as unverified. **Addition:** **xBRZ itself scales 2x to 6x in one pass.** `xbrz.h` has `const int SCALE_FACTOR_MAX = 6;` and `scale(size_t factor, //valid range: 2 - SCALE_FACTOR_MAX` (DOSBox-X and OpenXcom copies). It is GPL-3.0 (SourceForge, last update 2026-02-23). A native 6x xBRZ fallback or class-map scaler therefore needs no 2x→4x→downscale chain. |
| 9 | diablo1-4k contract: issue #10 opened 2026-10-02 and still open; FNV-1a 64 key; `hd/<S>x/<kk>/<key16>.png`; "exactly S× the 1× size"; "highest S ≤ HD Max Scale"; alpha clamp to the dilated 1x mask; picking on 1x; cycling stays on the 1x path; `DVX_HD_DUMP`; LRU budget; upstream PR #8586; packs "must never be published"; licence Sustainable Use License v1.0 | **CONFIRMED** | Issue #10 (title "[Graphics] HD texture pack loader prototype on the GL1 renderer (upstream #8586) ..."), opened 2 Oct 2026, open. All the quoted phrases are present. The repo sidebar shows "Sustainable Use License v1.0". **Context to weigh:** the repo is a macOS-native port with 10 commits, nothing has shipped, and the Sustainable Use License is not an OSI open-source licence. Borrow the design, not the code. `quantize.py` (issue #3) was not re-read. |
| 10 | Mesen HD packs: format **106**, docs advise 1-4x; the mkwong98 fork release 0.9.9-260511 (2026-05-11) plays packs "up to 109"; conditions include `randomBackground` | **CONFIRMED** (two corrections) | Fork release page: "0.9.9-260511 (May 11, 2026) – Update the emulator to play packs with version number up to 109". Doc source (`SourMesen/Mesen` master and `mkwong98/Mesen` master, `Docs/content/hdpacks/_index.md`): "Example: `<ver>106`", changelog "Version 106", and "It is suggested to use scales between 1x and 4x ... Anything above 8-10x will probably have a very hard time running". **Corrections:** (a) the website **mesen.ca/docs/hdpacks.html shows `<ver>105`** and its changelog stops at Version 105, so format 106 is documented only in the GitHub sources. (b) `randomBackground` (and `<addition>`) exist **only in the mkwong98 fork**, not in upstream Mesen. |
| 11 | Engine analogues: Raze `texture N { pal P { file "x.png" indexed }}`; OpenTTD `PALETTE_ANIM_START = 227` with 28 colours; VCMI 1.6 `sprites2x/3x/4x`, xBRZ for missing files, `-shadow` and `-overlay` | **CONFIRMED** | Raze `source/core/defparser.cpp` (master): `else if (sc.Compare("indexed")) indexed = true;` is passed to `tileSetHightileReplacement(..., indexed)`. The in-engine recolouring was not traced in the renderer. OpenTTD `src/gfx_type.h` (master): `PALETTE_ANIM_SIZE = 28`, `PALETTE_ANIM_START = 227`. VCMI HD_Graphics page: folders 2x/3x/4x (**max 4x**), "All images that are missing in the upscaled folders are scaled with the selected upscaling filter", plus the `-shadow` and `-overlay` suffixes. VCMI `ChangeLog.md`: 1.5.7→1.6.0 "Implemented xBRZ upscaling filter"; 1.6.3→1.6.4 "xbrz image upscaling is now performed in background threads". |
| 12 | U7: 3885 flat frames, 369 of them with reserved indices (0xE0-0xFF); cycling ranges `rotate_colors(0xfc,3) … (0xe0,8)` | **CONFIRMED** | Independent recount from `/mnt/e/Games/RolePlayingGames/ultima7/static/shapes.vga` (FLX, shapes 0-149, 64-byte 8x8 frames): **3885 frames, 369 with any byte ≥ 0xE0**. This matches `manifest.json`. `gamewin.cc:1043-1048` matches the six `rotate_colors` calls quoted in §1. |

**Corrections to apply to the body of this report:**

* §2.11: replace "None reaches 6x natively" with "no edge-aware scaler reaches 6x (Hq and xBR stop at 4x); Point, Interlaced and Bilinear accept 6x".
* TL;DR 12 and §5.1: replace "no new pixel-art-tagged model since 2021" with "no new model aimed at pixel art since 2021 (4x-HDCube, 2022, carries the tag but says it is unsuitable)".
* §5.1 table and §8: PixelPerfectV4 is "the only permissive 4x *sprite* model". Add **4x-Skyrim-Alpha (CC0, alpha-channel pixel art)** and **1x-ArtClarity (WTFPL, 1x post-pass)** to the permissive list.
* §5 install snippet: `cu128` works, but it caps torch at 2.11.0. Use `cu130` (torch 2.14.1) unless a specific dependency needs CUDA 12.8.
* §5 table: chaiNNer's last release is v0.25.1 (2025-10-23) and its last commit is 2026-07-31, so it is not "active Oct 2026".
* §3.1: format 106 is in the GitHub doc sources, while the mesen.ca page still shows 105. `randomBackground` and `<addition>` are mkwong98-fork-only.
* New design input: **xBRZ supports 6x in a single pass** (GPL-3.0). It is a candidate both for the runtime fallback and for scaling the class map and mask planes to 6x, without iterating MMPX.

**Unverifiable or not re-checked in this pass:** the original MMPX code licence (only a port's README says MIT); the diablo1-4k `quantize.py` details (issue #3); the Moguri 9.0 details and the DuckStation TR 2.0 merge date (not decision-critical, so not re-fetched); whether Raze recolours indexed hightiles in the renderer (only the parser was read).
