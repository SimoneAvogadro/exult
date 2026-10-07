export const meta = {
  name: 'u7-art-phase-a',
  description: 'Build the 6x terrain art pipeline (tooling, QA) and produce route-3 (xBRZ, CPU) and route-2 (NXbrz, WSL CUDA) packs for all BG flats',
  phases: [
    { title: 'Tooling', detail: 'u7hires Python package + tests (worktree hires-art)' },
    { title: 'Produce', detail: 'route 3 (xBRZ) and route 2 (NXbrz GPU) full runs' },
    { title: 'QA', detail: 'QA gates, contact sheets, comparison report' },
  ],
}

const ROOT = '/home/simonea/ultima7_exult'
const REPO = `${ROOT}/exult-hires`
const WT = `${ROOT}/exult-hires-art`
const DESIGN = `${ROOT}/docs-hires/design/DESIGN.md`
const REC = `${ROOT}/docs-hires/upscale-research/00_recommendation.md`

const CONTEXT = `PROJECT: hi-res (6x) terrain art for an Exult (Ultima VII) fork. Engine contract for override art: ${DESIGN} §5 (layout x6/flats/SSSS_FF.png 48x48, raw palette indices, PLTE = palette 0 via the engine's Get_color8 rule incl. clamp, no 0xFF in flats, cycling indices only per rule P4 (in-tile, clamped neighbourhood), optional tEXt Exult-Src-CRC32 guard, pack.txt with palette_crc32, groups = sub-directories), §8 (art plan, QA gates; §12 TB-3/TB-4/TB-7/TB-9 corrections), §13 user decisions (faithful look; non-commercial NXbrz OK for a private pack; packs canonical on ext4 at ${ROOT}/packs and mirrored to /mnt/e/Dati/Ultima7_Upscale/packs; Q8: tiles with B1 85-97% accepted but flagged). Research and measured baselines: ${REC} and the reports + code in ${ROOT}/docs-hires/upscale-research/ (algorithmic_code/u7algo.py has nearest_index, local_snap, unique_rgb_palette, xbrz_rgb, scale2x/3x, mmpx2x_idx; built libs: ${ROOT}/tmp/algo/xbrz19/libxbrz19.so, ${ROOT}/tmp/algo/mmpx/libmmpx.so; SR models in ${ROOT}/tmp/sr_bakeoff/models (SHA-256 checked); CUDA venv with torch 2.14.1+cu130 + spandrel: ${ROOT}/tmp/factcheck/venv_cu130 (verify it imports spandrel; if not, 'uv pip install spandrel==0.4.2' into it)).
INPUTS: original BG data read-only at /mnt/e/Games/RolePlayingGames/ultima7/static (copy shapes.vga, u7chunks, u7map, palettes.flx to ${ROOT}/art_work/static_cache/ on ext4 first for speed). Existing extraction: ${ROOT}/art_original (made by tools/hires/u7art.py; note its v2-header check is wrong but BG uses v1 chunks so its output is valid; it does NOT emulate the engine's under-RLE flat fill).
ENGINE FILL: for context renders emulate objs/chunkter.cc paint_tile exactly (read ${REPO}/objs/chunkter.cc lines ~86-133 at commit 8b6ab6b43: for RLE cells, search 3x3 neighbours with the quirky bound 'tiley + y > 0', skip 12/0 in that pass, then full-chunk scan without the 12/0 skip; zero-fill otherwise). The engine will later provide '--dump-art' as the source of truth; your Python port must be easy to cross-check against it (keep it in one function with a docstring).
WORKSPACE: the main repo working tree ${REPO} is being modified by another workflow; DO NOT touch it. Use a separate worktree ${WT} on branch 'hires-art' (create it from branch 'hires' if it exists, else from master: 'git -C ${REPO} worktree add ${WT} -b hires-art <base>'). Python package goes to ${WT}/tools/hires/u7hires/, tests to ${WT}/tools/hires/tests/ (pytest), CLI scripts in ${WT}/tools/hires/. Use the venv ${ROOT}/tools-venv (CPU; install pytest/numpy/pillow/scipy with 'VIRTUAL_ENV=${ROOT}/tools-venv uv pip install ...' if missing) and the CUDA venv for GPU steps. Work data on ext4: ${ROOT}/art_work/{ctx,raw,qa,...}. Packs: ${ROOT}/packs/<name>/x6/... Never commit EA-derived pixels; commit only code/tests/docs on 'hires-art' with messages ending in:
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>

CPU budget: another workflow builds C++ concurrently; use at most 8 worker processes. Temporary files only under ${ROOT}/tmp or ${ROOT}/art_work.`

const HW_NOTE = `\n\nHARDWARE WARNING (new facts; they override any conflicting instruction above): this PC has unstable RAM (2x32 GB DDR5 at 6000 MT/s EXPO). Under heavy load bit 26 of 64-bit words flips (silent corruption; previous agents measured it and added --redundancy/--retries and the vote/verify tools, see ${WT}/tools/hires/README.md 'production recipe on this machine'), and the PC already crashed twice (Windows bugcheck 0x1A MEMORY_MANAGEMENT). Therefore: (1) use at most 4 CPU worker processes and never run the GPU route and the CPU route at the same time; (2) produce packs with the redundancy/vote recipe so every published tile is confirmed by agreement; (3) re-run any non-reproducible failure before debugging, never change code for a failure you cannot reproduce twice; (4) earlier partial outputs exist from interrupted runs (packs/bg-r2-nxbrz-none, packs/bg-r3-*, art_work/raw/*, and uncommitted edits in ${WT}/tools/hires/u7hires/route2.py and tools/hires/tests/test_route2_robust.py): inspect them first (git diff), finish or revert the code edits (commit them on 'hires-art' if good), and only reuse earlier outputs after verifying them (vote/verify), otherwise regenerate.`
const CACHED_LABELS = new Set(['impl:tooling', 'verify:tooling#0'])
const agentHW = (prompt, opts) => agent(prompt + (CACHED_LABELS.has(opts && opts.label) ? '' : HW_NOTE), opts)
async function seq(thunks) { const out = []; for (const t of thunks) { try { out.push(await t()) } catch (e) { log('step failed: ' + e); out.push(null) } } return out }

const VERIFY_SCHEMA = { type: 'object', properties: { pass: { type: 'boolean' }, checks: { type: 'array', items: { type: 'object', properties: { check: { type: 'string' }, ok: { type: 'boolean' }, evidence: { type: 'string' } }, required: ['check', 'ok', 'evidence'] } }, failures: { type: 'array', items: { type: 'string' } } }, required: ['pass', 'checks', 'failures'] }

phase('Tooling')
const tooling = await agentHW(`${CONTEXT}\n\nTASK: implement the art tooling package u7hires (DESIGN §8.1-§8.3 and WP-12 scope, minus the parts that need the engine dump):
- io: read shapes.vga/u7chunks/u7map/palettes.flx (reuse/port tools/hires/u7art.py), palette 0 with the engine's exact 6->8 bit rule (Get_color8 incl. clamp at 255), world tile grid 3072x3072 (shape, frame&31) and terrain numbers per chunk;
- fill: python port of the engine paint_tile fill (see ENGINE FILL);
- ctx: context windows per used terrain (128px + 16-32px apron from the most frequent real neighbours) with a per-pixel instance map, macro sheets for the 8x4 torus shapes, self-wrap fallback for unused frames;
- quant: palette tools (uniquified palette, OKLab, local snap among 3x3 parent indices with optional ramp constraint, global nearest over 0x01-0xDF, cycling-mask handling per P4: cycling indices only where the 1x parent pixel or a clamped in-tile neighbour has an index in the same cycle range E0-E7,E8-EF,F0-F3,F4-F7,F8-FB,FC-FE; never 0xFF);
- consensus: per-(shape,frame) mode (index routes) / OKLab medoid (RGB routes);
- pack: writer for x6/flats/<SSSS>/SSSS_FF.png (8-bit palette PNG, raw indices, PLTE = full palette 0, tEXt Exult-Src-CRC32 = CRC32 of the 64-byte 1x flat, Exult-Origin), JSON sidecars (route, params, model sha256, instances, disagreement, qa), pack.txt (game=BG, scale=6, palette_crc32 = CRC32 of the 768-byte 8-bit palette 0, edge, route), atomic write (tmp + rename) and touch x6/.reload; publish.sh mirroring a pack to /mnt/e/Dati/Ultima7_Upscale/packs/<name>;
- check: hirescheck.py validator implementing the engine rules N1,F1,F2,F3,F4,P0,P4,G1,R1 + offline P2 (ramps) with the same rule IDs;
- qa: metrics A1-A3, B1-B4, C1 (seam ratio on world renders assembled from final tiles vs region-level render), C2 (edge-pair dE over adjacent pairs occurring on the map), D1; report.json + markdown; contact sheets (1x NN | candidate, per material family) and 6x world previews of ~12 representative chunks (coast, town, roads, forest floor, swamp, dungeon floor) downscaled also to 1280x800-equivalent.
- routes: route3 (xBRZ 6x on uniquified palette via the built libxbrz19.so (ctypes) + local snap + mode consensus; material-boundary hybrid variant) and route2 (spandrel 4x-NXbrz on GPU -> Lanczos 1.5x -> back-projection colour lock -> local snap -> consensus; batch windows; fp16 ok) as CLI entry points, deterministic, with progress logging.
pytest suite with synthetic data (no EA pixels committed) covering: fill port on crafted chunks, quantizer never emits >=0xE0 outside P4 nor 0xFF, PNG writer round-trip (raw indices, PLTE, tEXt), CRC vectors, consensus, validator rule IDs on crafted PNGs. Also a smoke test on real data marked to skip when the static dir is missing. Commit on 'hires-art'. Return a summary of modules, CLI commands and test results.`,
  { label: 'impl:tooling', phase: 'Tooling' })

let tver = null
for (let round = 0; round < 3; round++) {
  tver = await agentHW(`${CONTEXT}\n\nINDEPENDENT VERIFIER for the art tooling in ${WT}/tools/hires (implementer summary: ${JSON.stringify(tooling).slice(0, 4000)}). Run the pytest suite; run route3 and the validator on a sample of 50 real frames (incl. water frames with cycling indices, shore transitions, grass) end to end into a scratch pack under ${ROOT}/tmp/art_verify/; check with an independent script (not the package's own code) that the PNGs satisfy the engine contract (8-bit palette, 48x48, PLTE equals palette 0 computed independently with v*255//63 clamped, no 0xFF, cycling only per P4, tEXt CRC equals CRC32 of the source 64 bytes); check determinism (two runs, identical bytes); check the fill port against the engine logic by reading chunkter.cc yourself. Report pass only if all hold.`,
    { label: `verify:tooling#${round}`, phase: 'Tooling', schema: VERIFY_SCHEMA })
  if (tver && tver.pass) break
  if (round === 2) throw new Error('tooling failed verification: ' + JSON.stringify(tver && tver.failures))
  await agentHW(`${CONTEXT}\n\nFIXER for the art tooling in ${WT}. Verifier report: ${JSON.stringify(tver)}. Fix every real failure, re-run pytest and the failing checks, commit fixes on 'hires-art'.`, { label: `fix:tooling#${round}`, phase: 'Tooling' })
}

phase('Produce')
const [r3, r2] = await seq([
  () => agentHW(`${CONTEXT}\n\nPRODUCTION RUN — ROUTE 3 (xBRZ, CPU). Using the verified tooling in ${WT}/tools/hires: build context windows for all used terrains (art_work/ctx), run route 3 (default variant) for ALL 3,885 BG flat frames (used frames from map context windows; unused frames via macro sheets / self-wrap), consensus, quantize, write pack ${ROOT}/packs/bg-r3/ (x6/flats/<SSSS>/SSSS_FF.png + sidecars + pack.txt). Also produce the material-boundary hybrid variant as ${ROOT}/packs/bg-r3h/ if it is cheap (<1h). Run hirescheck on the packs (must report 0 errors), run QA (report + contact sheets + previews into ${ROOT}/art_work/qa/bg-r3/). Record runtime. Return counts (frames written, rejected, flagged by B1<97%), QA aggregates and paths.`,
    { label: 'produce:route3', phase: 'Produce' }),
  () => agentHW(`${CONTEXT}\n\nPRODUCTION RUN — ROUTE 2 (4x-NXbrz via spandrel on the RTX 5070 Ti from WSL). Using the verified tooling in ${WT}/tools/hires and the CUDA venv: if context windows are not yet in ${ROOT}/art_work/ctx, build them yourself into ${ROOT}/art_work/ctx-r2 (do not race with the route-3 run on the same directory). Run route 2 for ALL 3,885 BG flats, write pack ${ROOT}/packs/bg-r2/; also run 8x-Arzenal-v1-1 (box 8->6) on a representative subset of ~300 frames (water, grass, dirt, roads, floors, shores) into ${ROOT}/packs/bg-r2a-subset/ for look comparison. hirescheck 0 errors; QA report + contact sheets + previews into ${ROOT}/art_work/qa/bg-r2/. Record GPU time and VRAM (nvidia-smi). Return counts, QA aggregates and paths.`,
    { label: 'produce:route2', phase: 'Produce' }),
])

phase('QA')
const report = await agentHW(`${CONTEXT}\n\nCOMPARISON & REPORT. Route 3 result: ${JSON.stringify(r3).slice(0, 3000)}\nRoute 2 result: ${JSON.stringify(r2).slice(0, 3000)}\nCompare the packs in ${ROOT}/packs (bg-r3, bg-r3h if present, bg-r2, bg-r2a-subset) with the QA reports in ${ROOT}/art_work/qa/. Build side-by-side comparison sheets per material family (1x NN | r3 | r2 | arzenal subset) and 6x previews of the representative chunks for each pack, into ${ROOT}/art_work/qa/compare/ (PNG). LOOK at a selection of them yourself (Read the PNG files) and judge visually (faithfulness, worms/blobs, seams, water sparkles, roads/floors). Write ${ROOT}/docs-hires/art/phase_a_report.md: method, metrics table per pack, visual assessment with the paths of the most telling images, recommended default pack per family, known defects, and next steps (diffusion refine for which families). Then publish the recommended default pack: copy it to ${ROOT}/packs/bg (the active pack name) and mirror bg plus the compared packs to /mnt/e/Dati/Ultima7_Upscale/packs/ with publish.sh (bundle not available yet: loose PNGs are fine for now), plus copy the comparison sheets to /mnt/e/Dati/Ultima7_Upscale/art_compare/ for the user. Return a <=400-word summary with paths of the 6 most representative comparison images.`,
  { label: 'report', phase: 'QA' })

return { tooling_verified: tver && tver.pass, route3: r3, route2: r2, report }
