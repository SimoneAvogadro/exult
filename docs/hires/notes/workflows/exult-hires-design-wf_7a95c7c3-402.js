export const meta = {
  name: 'exult-hires-design',
  description: 'Design the 6x hi-res render + override system: 3 independent proposals, judge panel, synthesis, adversarial review',
  phases: [
    { title: 'Propose', detail: '3 independent architecture proposals' },
    { title: 'Judge', detail: '3 lenses score all proposals' },
    { title: 'Synthesize', detail: 'final DESIGN.md + work packages' },
    { title: 'Review', detail: 'adversarial verification of key claims' },
    { title: 'Revise', detail: 'apply review findings' },
  ],
}

const REPO = '/home/simonea/ultima7_exult/exult-hires'
const A = '/home/simonea/ultima7_exult/docs-hires/analysis'
const D = '/home/simonea/ultima7_exult/docs-hires/design'
const R = '/home/simonea/ultima7_exult/docs-hires/upscale-research'

const CONTEXT = `PROJECT: a local fork of Exult (Ultima VII engine) at ${REPO} (git master 8b6ab6b43, C++17, SDL3) that renders the game world internally at a high integer render scale (target S=6: a 320x200 game view becomes 1920x1200) and lets mods override shape frames with hi-res art, selectively (single tiles or groups). Terrain flats (shapes 0..149 of shapes.vga, 8x8) are the first priority; later RLE sprites; later UI. When the output window is smaller than the S-x render, the image is downscaled at present time ("render high-res, downscale at the end if the target is lower" - the user's explicit preference). Game logic, hit-testing, mouse picking stay in original game pixels.
USER DECISIONS ALREADY MADE: fork locally from master; target 6x detail; terrain first; selective per-tile/per-group overrides; AI-upscaled art will be produced later (maybe on a Windows PC with an RTX 5070 Ti, work folder E:\\Dati\\Ultima7_Upscale); the deliverable includes code, test suites for new functionality, and 6x tiles.
ENVIRONMENT: WSL2 Ubuntu 20.04, g++ 9.4, no sudo; user-space toolchain in /home/simonea/ultima7_exult/deps (source deps/env.sh: autotools, pkg-config, SDL3 3.4.18 built from source); out-of-tree build at /home/simonea/ultima7_exult/build-linux (configured, -O0 default!); headless runs work with SDL_VIDEO_DRIVER=dummy and config /home/simonea/ultima7_exult/run/exult.cfg (BG data at /mnt/e/Games/RolePlayingGames/ultima7). '--buildmap 2' already renders 144 superchunk PNGs headless deterministically. Original art extracted by ${REPO}/tools/hires/u7art.py into /home/simonea/ultima7_exult/art_original (indexed PNGs of flats, RLE frames, chunk renders, superchunk renders, manifest with usage counts). Windows has Exult 1.12.1 installed (mingw build with SDL3.dll) at /mnt/e/Games/RolePlayingGames/ExultUltimaVII.
INPUT: a thorough consolidated analysis exists: ${A}/00_architecture_map.md (READ IT FULLY FIRST) plus detailed reports in ${A}/*.md (ibuf, present, shapes, world, ui, build, palette, gap_1..gap_7). Art-upscaling research may be (partially) available under ${R}/ (00_recommendation.md if finished) - optional input.
NOTE on prerequisite 'fixes' P1/P2 in the map (chunkter.cc paint_tile neighbour search bound 'tiley + y > 0' and the 12/0 fallback): they change upstream 1x output; P1 looks like a genuine off-by-one. Decide explicitly whether the fork should change 1x behaviour (and keep such changes as separate, upstreamable commits) or keep 1x byte-identical to upstream; determinism (P3 uninitialised memory) is mandatory either way.`

const PROPOSAL_SCHEMA = {
  type: 'object',
  properties: {
    path: { type: 'string' },
    title: { type: 'string' },
    core_idea: { type: 'string' },
    architecture_decisions: { type: 'array', items: { type: 'string' } },
    work_packages: { type: 'array', items: { type: 'object', properties: { id: { type: 'string' }, name: { type: 'string' }, scope: { type: 'string' }, tests: { type: 'string' }, est_days: { type: 'number' }, depends_on: { type: 'array', items: { type: 'string' } } }, required: ['id', 'name', 'scope', 'tests', 'est_days'] } },
    main_risks: { type: 'array', items: { type: 'string' } },
    deviations_from_map: { type: 'array', items: { type: 'string' }, description: 'where and why you deviate from the D1-D10 decisions in 00_architecture_map.md' },
  },
  required: ['path', 'title', 'core_idea', 'architecture_decisions', 'work_packages', 'main_risks', 'deviations_from_map'],
}

const ANGLES = [
  { key: 'A_minimal', title: 'Minimal-risk, upstream-friendly',
    brief: `Optimise for correctness and smallest, reviewable diff that reaches a playable M1 (world at S with NN fallback + terrain overrides) fast, keeps the S=1 path byte-identical, and could plausibly be upstreamed to the Exult team (they are reworking rendering: layers, SDL textures). Be sceptical of anything clever. Challenge the map where it is over-engineered (e.g. do we really need per-terrain content-hash overrides, CRC gates, strict groups in M1?) and order work so that something visible and testable exists early.` },
  { key: 'B_perf_gpu', title: 'Performance & GPU-first',
    brief: `Optimise for runtime performance and visual quality at 1920x1200..4K, on the actual target (Windows, RTX 5070 Ti, SDL3 D3D11/12/Vulkan renderers) and in WSL. Consider alternatives to the CPU S-x 8-bit buffer: e.g. palette applied on GPU (INDEX8 textures, SDL GPU API/shaders), terrain drawn as GPU texture tiles, a hi-res index buffer uploaded per dirty rect, downscale quality (area filter / mip chain vs LINEAR), smooth sub-pixel scrolling as a bonus. Quantify costs (use the measurements in the map and gap_4; you may write and run small benchmarks in /home/simonea/ultima7_exult/tmp). Keep palette semantics (cycling, day/night, translucency) correct. Decide clearly whether the CPU in-place scaled buffer (map D2-D4) is the right baseline or not.` },
  { key: 'C_art_modder', title: 'Art pipeline & modder-first',
    brief: `Optimise for the end-to-end mod workflow: how a modder (and our own AI-upscale pipeline) extracts, edits, validates, packages and live-tests 6x art; selective overrides (single tile, tile family, chunk/terrain, groups); file layout & naming & manifest format (consider Mesen-style HD pack ideas if present in ${R}); hot-reload for authoring; validation tooling (palette/index rules, edge contract, seams); test strategy (unit tests, golden images via headless renders, oracles NN(S=1)==render(S)); and how the 6x terrain art for Black Gate will actually be produced and QA'd with what we have (CPU-only WSL now: xBRZ-style deterministic baseline; later Windows GPU diffusion/SR models). The engine design must serve this workflow.` },
]

const COMMON = `
Rules:
- Read ${A}/00_architecture_map.md fully, and any detail reports you need. Verify important claims directly in the code at ${REPO} (read-only; do NOT modify the repo). You may create scratch files only under /home/simonea/ultima7_exult/tmp.
- Produce a COMPLETE, implementable design proposal as markdown (aim 3000-7000 words) at the given path (mkdir -p ${D}): goals/non-goals, architecture (components, data structures, class/function changes with file:line anchors), config & CLI, file formats & directory layout for overrides, present/downscale path, terrain path, sprite path (outline), test strategy (unit, golden, oracles; how to run headless; what framework), build/CI (Linux user-space build here, Windows build plan), art production plan for the 6x BG terrain tiles, milestones and work packages with acceptance criteria and estimates, risks.
- Return the structured summary.`

phase('Propose')
const props = await parallel(ANGLES.map((a) => () =>
  agent(`${CONTEXT}\n\nYOU ARE ARCHITECT ${a.key} — ${a.title}.\n${a.brief}\n${COMMON}\nProposal file: ${D}/proposal_${a.key}.md`,
    { label: `propose:${a.key}`, phase: 'Propose', schema: PROPOSAL_SCHEMA })
    .then((p) => (p ? { key: a.key, ...p } : null))))
const okProps = props.filter(Boolean)
log(`${okProps.length} proposals ready`)

phase('Judge')
const JUDGE_SCHEMA = {
  type: 'object',
  properties: {
    scores: { type: 'array', items: { type: 'object', properties: { proposal: { type: 'string' }, score: { type: 'number', description: '0-10' }, strengths: { type: 'array', items: { type: 'string' } }, weaknesses: { type: 'array', items: { type: 'string' } } }, required: ['proposal', 'score', 'strengths', 'weaknesses'] } },
    winner: { type: 'string' },
    grafts: { type: 'array', items: { type: 'string' }, description: 'best ideas from non-winning proposals to graft onto the winner' },
    must_fix: { type: 'array', items: { type: 'string' }, description: 'errors or gaps present in ALL proposals or in the winner' },
  },
  required: ['scores', 'winner', 'grafts', 'must_fix'],
}
const LENSES = [
  { key: 'correctness', text: 'CORRECTNESS & RISK: will it work without memory corruption, regressions at S=1, palette/translucency/cycling breakage, mouse/hit-test errors, state side effects; are claims verified in code? Spot-check the most load-bearing claims in the code yourself.' },
  { key: 'delivery', text: 'DELIVERY & MAINTAINABILITY: effort realism, incremental milestones with something visible early, testability, size of diff, upstream-compatibility with ongoing Exult rendering work (layers/SDL textures), build on Linux user space and Windows.' },
  { key: 'user_goal', text: 'USER-GOAL FIT: does it deliver what the user asked (6x detail, render high then downscale, selective per-tile/group overrides for terrain first, a usable mod/art workflow incl. AI-upscaled tiles produced later on a Windows GPU, tests for new features), with good visual quality (seams, consistency) and a pleasant modding experience?' },
]
const digest = JSON.stringify(okProps.map((p) => ({ key: p.key, path: p.path, title: p.title, core_idea: p.core_idea, architecture_decisions: p.architecture_decisions, work_packages: p.work_packages, main_risks: p.main_risks, deviations_from_map: p.deviations_from_map })))
const judges = await parallel(LENSES.map((l) => () =>
  agent(`${CONTEXT}\n\nYou are a JUDGE with lens ${l.text}\nThree proposals (summaries below; READ the full proposal files at their paths before judging):\n${digest}\nScore each 0-10 under your lens, pick a winner, list grafts and must-fix items. Be concrete and cite sections/lines.`,
    { label: `judge:${l.key}`, phase: 'Judge', schema: JUDGE_SCHEMA }).then((j) => (j ? { lens: l.key, ...j } : null))))
const okJudges = judges.filter(Boolean)

phase('Synthesize')
const design = await agent(`${CONTEXT}\n\nYou are the LEAD ARCHITECT. Proposals: ${okProps.map((p) => p.path).join(', ')} (read fully). Judge verdicts (JSON): ${JSON.stringify(okJudges)}.\nWrite the FINAL design document ${D}/DESIGN.md (English, precise, implementable; 6000-12000 words). Start from the highest-scoring proposal, graft the best ideas, fix every must-fix item, and keep scope disciplined (M1 must be reachable first). Required sections:
1. Goals, non-goals, user decisions honoured.
2. Architecture overview (diagram in ascii), invariants.
3. Detailed changes per component with file:line anchors (imagewin buffer core, window/present/downscale path incl. SDL version handling, game window/world renderer, terrain cache, override store & loaders, shapes/sprite hook (M2), UI (M3 outline)).
4. Configuration keys, CLI flags (render-test / dump-art etc.), defaults.
5. Override file formats, directory layout, naming, manifest, validation rules (palette/index classes, geometry, edge contract), packaging, hot-reload for authoring.
6. Test strategy: framework choice (vendor a single-header framework if needed), unit tests, golden image tests, oracles, how to run headless in this WSL env, CI integration; list concrete test cases per work package.
7. Build: Linux user-space build here (exact commands with deps/env.sh, optimisation flags), Windows build plan for the user's PC (cross-compile from WSL with mingw-w64 in user space vs MSYS2 on Windows under E:\\Dati\\Ultima7_Upscale; pick one with justification and concrete steps).
8. Art production plan for 6x BG terrain: phase A (autonomous, CPU-only here: deterministic baseline e.g. xBRZ 6x / custom pixel-art scaler + palette quantization + edge contract, producing a complete usable pack), phase B (AI models on Windows GPU or OpenRouter - integrate ${R}/00_recommendation.md if present), QA metrics and tools.
9. Work packages: ordered list with ids, dependencies, scope, files, acceptance tests, estimate; mark which are needed for the first visible demo.
10. Risks and mitigations; open questions for the user (only those that truly need the user; give recommended defaults).
11. Decision log: what was chosen over what and why (including the P1/P2 1x-behaviour decision).
Return a <=700-word summary of the final design including the work package list.`,
  { label: 'synthesize', phase: 'Synthesize' })

phase('Review')
const REVIEW_SCHEMA = {
  type: 'object',
  properties: {
    findings: { type: 'array', items: { type: 'object', properties: { claim_or_section: { type: 'string' }, problem: { type: 'string' }, evidence: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'major', 'minor'] }, fix: { type: 'string' } }, required: ['claim_or_section', 'problem', 'evidence', 'severity', 'fix'] } },
  },
  required: ['findings'],
}
const REVIEWERS = [
  { key: 'code_claims', text: 'Verify every load-bearing claim about EXISTING code in DESIGN.md (file:line anchors, behaviours, call paths). Try to REFUTE them by reading the code. Report only real problems.' },
  { key: 'memory_safety', text: 'Hunt for memory-safety and unit-mixing bugs the design would introduce (logical vs physical coords, guard band, pitch, clipping, render-target push/pop, resize/re-creation of surfaces & textures, cached pointers, eviction). Think through concrete call sequences.' },
  { key: 'present_sdl', text: 'Verify the present/downscale path against SDL 3.2 and 3.4 APIs actually available (headers in /home/simonea/ultima7_exult/deps/prefix/include/SDL3) and Exult\'s show()/UpdateRect/composite_layers code; check palette rotation/fades reach the screen, screenshots, layers on top, letterboxing, HiDPI, texture size limits. You may write a tiny SDL test program in /home/simonea/ultima7_exult/tmp and run it with the dummy/offscreen driver.' },
  { key: 'tests_build', text: 'Check the test strategy and build plan are executable in THIS environment (WSL, no sudo, deps/env.sh) and that the Windows build plan is realistic; check the art plan phase A can really be done CPU-only here and produces valid packs under the format rules. Try the critical commands where cheap.' },
]
const reviews = await parallel(REVIEWERS.map((r) => () =>
  agent(`${CONTEXT}\n\nYou are an ADVERSARIAL REVIEWER (${r.key}). Read ${D}/DESIGN.md fully. ${r.text} Default to skepticism, but only report issues you can support with evidence.`,
    { label: `review:${r.key}`, phase: 'Review', schema: REVIEW_SCHEMA }).then((x) => (x ? { reviewer: r.key, ...x } : null))))
const allFindings = reviews.filter(Boolean).flatMap((r) => r.findings.map((f) => ({ reviewer: r.reviewer, ...f })))
log(`review findings: ${allFindings.length} (blockers ${allFindings.filter((f) => f.severity === 'blocker').length}, major ${allFindings.filter((f) => f.severity === 'major').length})`)

phase('Revise')
const revised = await agent(`${CONTEXT}\n\nYou are the LEAD ARCHITECT again. Adversarial review findings on ${D}/DESIGN.md (JSON): ${JSON.stringify(allFindings)}\nFor each finding: verify it in the code yourself; if valid, fix DESIGN.md accordingly; if invalid, note why. Append a section '## 12. Review log' to DESIGN.md listing every finding with disposition (fixed / rejected + reason). Keep the document coherent (update work packages and estimates if needed). Return: a <=600-word summary of the final design, the final ordered work package list (id, name, est days, deps), the list of open questions for the user with recommended defaults, and the count of findings fixed/rejected.`,
  { label: 'revise', phase: 'Revise' })

return { proposals: okProps.map((p) => ({ key: p.key, path: p.path, title: p.title })), judges: okJudges.map((j) => ({ lens: j.lens, winner: j.winner, scores: j.scores.map((s) => [s.proposal, s.score]) })), design_summary: design, review_findings: allFindings.length, revised }
