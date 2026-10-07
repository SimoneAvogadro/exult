export const meta = {
  name: 'exult-hires-m1a',
  description: 'Implement M0+M1a of the Exult hi-res fork (scaled world buffer, present path, tests) plus the override store in parallel',
  phases: [
    { title: 'M0', detail: 'WP-00 fork/build matrix, WP-01 test infra' },
    { title: 'M1a', detail: 'WP-03 prereqs, WP-04 scaled ibuf, WP-05 present, WP-06 world, WP-07 render-test' },
    { title: 'Store', detail: 'WP-08 override store in a separate worktree' },
    { title: 'Probe', detail: 'WP-02 Windows GPU probe' },
    { title: 'Merge', detail: 'merge store branch, full test run' },
  ],
}

const ROOT = '/home/simonea/ultima7_exult'
const REPO = `${ROOT}/exult-hires`
const DESIGN = `${ROOT}/docs-hires/design/DESIGN.md`

const CONTEXT = `PROJECT: local fork of Exult (Ultima VII engine) adding a high-resolution world render scale S (target 6x the ORIGINAL game resolution: 320x200 game px -> 1920x1200) and hi-res art overrides (terrain first).
AUTHORITATIVE SPEC: ${DESIGN} (revision 2 + §12 review log + §13 user decisions). Read the sections named for your work package fully, plus §2 (invariants), §12 and §13. Where the spec and the code disagree, the code is the truth: adapt sensibly and note it in your summary. The analysis behind it is in ${ROOT}/docs-hires/analysis/ (00_architecture_map.md and detail reports) if you need background.
REPO: ${REPO} (git; upstream master base 8b6ab6b43). Feature branch: 'hires'. Upstream-only bugfix branch: 'upstream-fixes'. NEVER push. Never rewrite published history; never use interactive git.
ENVIRONMENT: WSL2 Ubuntu 20.04, g++ 9.4 (C++17), no sudo. ALWAYS 'source ${ROOT}/deps/env.sh' before building (autotools, pkg-config, SDL3 3.4.18 in deps/prefix). Build trees (out-of-tree): ${ROOT}/build-linux (-O0 debug, existing), ${ROOT}/build-o2 (perf + game tests), ${ROOT}/build-asan (ASan/UBSan; run every ASan process as 'setarch x86_64 -R timeout <secs> ...' because ASan hangs randomly under this kernel's ASLR otherwise), ${ROOT}/build-upstream (from worktree ${ROOT}/exult-upstream = upstream reference + P3), ${ROOT}/build-sdl32 (SDL 3.2.14 lane, prefix ${ROOT}/deps/prefix-3.2). Use 'make -j8' (other tracks may build concurrently). After editing configure.ac / Makefile.am run 'autoreconf -v -i' in the source tree (generated files stay untracked).
GAME DATA (read-only!): Black Gate at /mnt/e/Games/RolePlayingGames/ultima7 (static dir .../static). Never write into that directory: test configs must point patch/mods/savegame/gamedat/hires paths to scratch dirs under ${ROOT}/tmp/. Headless runs: 'env -u DISPLAY -u WAYLAND_DISPLAY SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy' (offscreen + software renderer for presenter tests). WSLg (DISPLAY=:0) is available for windowed smoke runs. Example config: ${ROOT}/run/exult.cfg; '--buildmap 2' renders 144 superchunk PNGs headless deterministically.
CODE RULES: match surrounding style (.clang-format in repo, tabs); keep the S=1 path byte-identical to upstream (+P3); every S>1 behaviour in new functions/files with one-line hooks in existing functions; register every new source file in all build descriptions the spec lists (Makefile.am files, Makefile.common, msvcstuff/vs2019 vcxproj + filters; ios project optional); no EA-derived pixels committed (hash lists only). Temporary files only under ${ROOT}/tmp/.
COMMITS (only when your instructions say so): small logical commits on the right branch, message style like upstream ('Area: summary' + body), and END EVERY COMMIT MESSAGE WITH these two lines:
Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
`

const HW_NOTE = `\n\nHARDWARE WARNING (new facts; they override any conflicting instruction above): this PC has unstable RAM (2x32 GB DDR5 run at 6000 MT/s with EXPO). Measured by previous agents: under heavy load bit 26 of 64-bit words flips (silent data corruption), and the machine already went down twice (one Windows bugcheck 0x1A MEMORY_MANAGEMENT reboot); previous agents were killed mid-work. Therefore: (1) keep the load moderate: 'make -j6' at most, build/test the lanes one after another (never several build trees in parallel), at most one ASan process at a time, no unnecessary full rebuilds; (2) a failure that does not reproduce (compiler ICE/segfault, random test or digest mismatch, a crash that goes away) must be re-run at least once before you debug or fix anything; never change code for a failure you cannot reproduce twice; report non-reproducible failures as suspected hardware faults; (3) recompute a golden/hash mismatch once before trusting it; (4) the working tree (and build trees) may contain partial uncommitted work from an interrupted previous attempt of THIS step: inspect 'git status'/'git diff' first and continue from it instead of starting over; if an object file or binary looks corrupted, rebuild just that target.`
const CACHED_LABELS = new Set(['impl:WP-00', 'verify:WP-00#0', 'review:WP-00#0'])
const agentHW = (prompt, opts) => agent(prompt + (CACHED_LABELS.has(opts && opts.label) ? '' : HW_NOTE), opts)

const WPS = {
  'WP-00': { title: 'Fork and build matrix', sections: '§7.1, §9 (WP-00 row), §6.6, §12 (TB-6, TB-11), §13',
    extra: `Create branch 'hires' from 8b6ab6b43 (current master == 8b6ab6b43) and branch 'upstream-fixes' from the same commit. On 'hires' commit: tools/hires/u7art.py (already in the tree, untracked), .gitignore additions per §7.1, and a snapshot copy of the design at docs/hires/DESIGN.md (copy of ${DESIGN}). Create build-o2 and build-asan (configure flags per §7.1; use the setarch/timeout wrapper rule), the exult-upstream worktree at 8b6ab6b43 and build-upstream (P3 is cherry-picked there later in WP-03), and the SDL 3.2.14 lane: build SDL 3.2.14 from source into ${ROOT}/deps/prefix-3.2 (same cmake flags style as 3.4.18 in deps/src: -DSDL_WAYLAND=OFF -DSDL_X11_XTEST=OFF, CMAKE_INCLUDE_PATH/LIBRARY_PATH pointing at deps/sysroot) and configure ${ROOT}/build-sdl32 against it (PKG_CONFIG_PATH with prefix-3.2 first). Build exult in build-o2, build-asan, build-upstream, build-sdl32. Record reference buildmap SHA-256 lists for '--buildmap 2' from build-upstream run twice (must be identical; if unmodified upstream is non-deterministic because of the uninitialised flats cache (P3), document that and record anyway) under ${ROOT}/tmp/goldens-wp00/. Write a short ${ROOT}/docs-hires/impl/WP-00.md describing the build matrix and exact commands.` },
  'WP-01': { title: 'Test infrastructure', sections: '§6.1, §6.2 (test_ibuf_golden), §6.6 (ci.sh skeleton), §9 (WP-01 row), §12 (CC-5, CC-6, TB-1, TB-12, TB-14)',
    extra: `Vendor doctest (single header, MIT; download the latest release header from github.com/doctest/doctest, record version + SHA-256 in tests/README). Create the tests/ tree, autotools 'make check' (tests/Makefile.am, SUBDIRS, AC_CONFIG_FILES), Makefile.common targets, link plan per §12 CC-5/CC-6 (link the convenience libraries the way tools/ipack does). Write test_ibuf_golden and RECORD tests/data/ibuf_golden.txt on the UNMODIFIED upstream ibuf8.cc (before any WP-04 change). tests/game/ skeleton (test.cfg.in, scripts that exit 77 without U7_BG_STATIC). Python: tools/hires/requirements.txt (pytest, numpy, pillow pinned) installed with 'uv pip install' into ${ROOT}/tools-venv; a trivial pytest smoke test. tools/hires/ci.sh skeleton (builds build-o2 + build-asan, runs make check in both with the ASan wrapper, writes ${ROOT}/tmp/ci-<date>.log). 'make check' must be green in build-o2, build-asan and build-sdl32.` },
  'WP-03': { title: 'Prerequisite commits', sections: '§3.4 (flat_source.h, paint_flats), §3.2.3 (main-buffer ownership / RAII guard: only the parts that are needed at S=1 - the S>1 tail comes in WP-05), §9 (WP-03 row), §11 (D-02, D-24), §12 (CC-1, CC-8, MS-4, PS-1 P11 palette alpha, PS-6)',
    extra: `On 'hires': P3 zero-fill of the flats cache; find_flat_source() + paint_flats refactor that is byte-identical at S=1 (pinned by test_flat_source including the P1/P2 quirks); write_minimap using a local 1x buffer; Figure_queue_size per §3.4; main-buffer wiring (create_surface/free_surface/destructor act on main_ibuf via an RAII guard) - behaviour-neutral at S=1; Import_png8 failure leak fix; P11 palette alpha 255 (output-neutral). One commit per item. Cherry-pick P3 (and P11 if output-neutral) onto the exult-upstream worktree so build-upstream = reference; rebuild build-upstream and re-record the buildmap SHA-256 reference lists (2 runs identical) to ${ROOT}/tmp/goldens-ref/. Then verify build-o2 buildmap == reference. On 'upstream-fixes' (from 8b6ab6b43): P1, P2, P5 as separate commits each with a test (the test infra commit from WP-01 may need to be cherry-picked there first). Keep 'hires' free of P1/P2/P5.` },
  'WP-04': { title: 'Scaled Image_buffer8', sections: '§3.1 entirely, §6.2 (test_ibuf_scaled O1 incl. tracker_complete, mixed-scale get/put/blit, copy clipping with canaries, put_phys, get_pixel8, fill_static RNG parity, create_another), §9 (WP-04 row), §12 (MS-2, MS-5, MS-6 put_phys part, MS-9, TB-12)',
    extra: `pixel_scale in Image_buffer, new constructors, one-line hooks in every ibuf8.cc primitive, bodies in new imagewin/ibuf8_scaled.cc, Write_tracker, mixed-scale semantics, raw-reader audit (mini_screenshot via get_pixel8 etc.). test_ibuf_golden MUST stay unchanged (S=1 byte-identical). ASan clean (with wrapper). Commit on 'hires'.` },
  'WP-05': { title: 'Scale policy and present path', sections: '§3.2 entirely (3.2.1-3.2.7), §4.1, §4.2 (--render-scale only), §6.2 (test_world_scale), §6.3 (hires_present, data-free), §9 (WP-05 row), §11 (D-04..D-08, D-18, D-24..D-28), §12 (CC-1..CC-4, CC-7, MS-1, MS-3, PS-1..PS-9), §13 (max_world_mpx default 40, no automatic performance drop)',
    extra: `world_scale.h (policy art/auto/force/off, filter ladder), world_present.{h,cc} (ARGB+LUT baseline; INDEX8 path may be stubbed behind SDL_VERSION_ATLEAST(3,4,0) and completed in WP-14 - but if it is cheap, implement it now), create_world_scaled_surfaces, show() hook + show_world_scaled (scene/pushed semantics), guard-band early-outs, screenshot hook, rotatecolours change, render-reset event watch, config keys + --render-scale, fail-soft latch, S-cycle safety (pixel_scale/tracker reset on every path). Unit + data-free present tests green, also in build-sdl32 (ARGB-only). Manual smoke: run the game windowed under WSLg (DISPLAY=:0) with render_scale=art (or --render-scale force:6) for ~20 s via 'timeout', confirm log shows S=6 and no crash; also headless with dummy driver. Commit on 'hires'.` },
  'WP-06': { title: 'World integration (first visible demo)', sections: '§3.3, §3.4 (cache by scale: get_rendered_flats(scale), render_flats(scale)), §9 (WP-06 row)',
    extra: `get_rendered_flats(scale) + generation; paint_chunk_flats -> blit + clip skip; BuildGameMap forces render_scale off; resize toast; perf scopes. Buildmap golden (S=1) unchanged vs reference. Demo: run the game at S=6 windowed under WSLg for ~30 s (timeout) without crash; capture a paletted screenshot if feasible (the engine's screenshot path writes an S x indexed image at S>1) or defer visual capture to WP-07's --render-test. Commit on 'hires'.` },
  'WP-07': { title: '--render-test harness and goldens (M1a gate)', sections: '§4.2 (--render-test), §6.4 (O0, O2, O6, O7, present read-back, determinism, pushed_resize, S-cycle resize oracle), §6.5, §9 (WP-07 row), §12 (MS-1 S-cycle oracle, MS-2, CC-1 pushed_resize)',
    extra: `Implement render_test.cc, regions.txt (choose regions from ${ROOT}/art_original/superchunks and the engine buildmap PNGs), tests/game scripts (buildmap_golden.sh, render_regions.sh, regen_goldens.sh), perf baseline at -O2 (build-o2). 'make check-game' with U7_BG_STATIC=/mnt/e/Games/RolePlayingGames/ultima7/static must be green, also run the region oracles once under build-asan with the wrapper. ALSO write demo images (never committed) for the user to ${ROOT}/tmp/demo-m1a/: for 3 interesting regions (a town e.g. Britain, a coast with water, a forest) the S=1 render and the S=6 render (mode=plain, PNG). Commit on 'hires'.` },
  'WP-08': { title: 'Hi-res override store (parallel track)', sections: '§3.5, §5 entirely (layout, keys, PNG contract, manifest, validation rules, reduction, packaging incl. the bundle format of §5.7), §6.2 (test_hires_png, test_hires_rules, test_hires_store, test_editor_fixtures), §9 (WP-08 row), §11 (D-10..D-14), §12 (MS-6, MS-7, TB-2, TB-4, TB-9)',
    extra: `Work in a SEPARATE git worktree so the main track is not disturbed: 'git -C ${REPO} worktree add ${ROOT}/exult-hires-store -b hires-store hires' (branch from the current 'hires' tip, which contains WP-00/WP-01). Build out-of-tree in ${ROOT}/build-store (configure like build-o2) and ${ROOT}/build-store-asan (like build-asan). Implement shapes/hires_png, hires_rules, hires_store (+ bundle reader), the engine glue hires_glue.{h,cc} (config read, <HIRES> path tag in gamemgr/modmgr.cc, effective palette 0, Src_provider, invalidation hooks in shapeid.cc) but do NOT wire Hires::flat into the terrain composition (that is WP-09, after merge). Tests with synthetic data only (fixtures under tests/data/hires/, no EA pixels). 'make check' green in build-store and build-store-asan (wrapper). Commit on branch 'hires-store' only.` },
}

const IMPL_SCHEMA = { type: 'object', properties: {
  summary: { type: 'string' }, files_changed: { type: 'array', items: { type: 'string' } },
  tests_run: { type: 'array', items: { type: 'object', properties: { cmd: { type: 'string' }, result: { type: 'string' } }, required: ['cmd', 'result'] } },
  deviations_from_spec: { type: 'array', items: { type: 'string' } }, open_issues: { type: 'array', items: { type: 'string' } } },
  required: ['summary', 'files_changed', 'tests_run', 'deviations_from_spec', 'open_issues'] }
const VERIFY_SCHEMA = { type: 'object', properties: {
  pass: { type: 'boolean' }, checks: { type: 'array', items: { type: 'object', properties: { check: { type: 'string' }, ok: { type: 'boolean' }, evidence: { type: 'string' } }, required: ['check', 'ok', 'evidence'] } },
  failures: { type: 'array', items: { type: 'string' } } }, required: ['pass', 'checks', 'failures'] }
const REVIEW_SCHEMA = { type: 'object', properties: {
  findings: { type: 'array', items: { type: 'object', properties: { where: { type: 'string' }, problem: { type: 'string' }, evidence: { type: 'string' }, severity: { type: 'string', enum: ['blocker', 'major', 'minor'] }, fix: { type: 'string' } }, required: ['where', 'problem', 'evidence', 'severity', 'fix'] } } },
  required: ['findings'] }

function wpText(id) { const w = WPS[id]; return `WORK PACKAGE ${id} — ${w.title}\nSpec sections: ${w.sections}\nSpecifics: ${w.extra}` }
function treeOf(id) { return id === 'WP-08' ? `${ROOT}/exult-hires-store (branch hires-store)` : `${REPO} (branch ${'hires'})` }

async function runWP(id, phaseName) {
  const impl = await agentHW(`${CONTEXT}\n\nYou are the IMPLEMENTER.\n${wpText(id)}\nImplement it completely in ${treeOf(id)}, build, and run the acceptance tests yourself until they pass. Do NOT commit yet (a later step commits after independent review), except where the specifics explicitly require commits on other branches/worktrees (e.g. upstream-fixes, exult-upstream). Write implementation notes to ${ROOT}/docs-hires/impl/${id}.md (what was done, deviations, how to test).`,
    { label: `impl:${id}`, phase: phaseName, schema: IMPL_SCHEMA })
  if (!impl) throw new Error(`${id}: implementer failed`)
  let last = { ver: null, rev: null }
  for (let round = 0; round < 3; round++) {
    const [ver, rev] = await parallel([
      () => agentHW(`${CONTEXT}\n\nYou are an INDEPENDENT VERIFIER for ${id}. Do not trust the implementer's claims.\n${wpText(id)}\nImplementer summary: ${JSON.stringify(impl)}\nIn ${treeOf(id)}: rebuild the affected build trees from the current working tree, run every acceptance test of ${id} listed in DESIGN.md §6.7/§9 and the specifics above, plus the full 'make check' in build-o2 (or build-store for WP-08) and the ASan tree with the wrapper, and check that previously recorded goldens (S=1) are unchanged. Do not modify source files (you may create scratch files under ${ROOT}/tmp). Report pass=true only if everything required passes, with command + output evidence.`,
        { label: `verify:${id}#${round}`, phase: phaseName, schema: VERIFY_SCHEMA }),
      () => agentHW(`${CONTEXT}\n\nYou are an ADVERSARIAL CODE REVIEWER for ${id}.\n${wpText(id)}\nReview the uncommitted diff in ${treeOf(id)} ('git diff' and new untracked files; for branches with commits made in this WP also review those commits). Hunt for real defects: memory safety (logical vs physical units, pitch, clipping, guard band, overruns), behaviour changes at S=1 (must be byte-identical to upstream+P3), divergence from DESIGN.md that matters, missing tests for required behaviour, missing build registration (Makefile.am, Makefile.common, vcxproj+filters), resource leaks, platform issues (Windows/mingw, SDL 3.2 vs 3.4). Read the surrounding code to confirm each finding; report only findings you can support with evidence. Do not modify files.`,
        { label: `review:${id}#${round}`, phase: phaseName, schema: REVIEW_SCHEMA }),
    ])
    last = { ver, rev }
    const blocking = ((rev && rev.findings) || []).filter((f) => f.severity !== 'minor')
    const minors = ((rev && rev.findings) || []).filter((f) => f.severity === 'minor')
    log(`${id} round ${round}: verify ${ver ? (ver.pass ? 'PASS' : 'FAIL') : 'n/a'}, findings blocking=${blocking.length} minor=${minors.length}`)
    if (ver && ver.pass && blocking.length === 0) {
      if (minors.length) {
        await agentHW(`${CONTEXT}\n\nYou are the FIXER for ${id}. Minor review findings: ${JSON.stringify(minors)}\nIn ${treeOf(id)}: fix those that are real and cheap; skip others with a one-line reason. Rebuild and re-run 'make check' (and the ${id} acceptance tests) to make sure nothing regressed.`,
          { label: `fix-minor:${id}`, phase: phaseName })
      }
      break
    }
    if (round === 2) throw new Error(`${id}: still failing after 3 rounds: ${JSON.stringify({ failures: ver && ver.failures, blocking })}`)
    await agentHW(`${CONTEXT}\n\nYou are the FIXER for ${id}.\n${wpText(id)}\nVerifier report: ${JSON.stringify(ver)}\nReview findings: ${JSON.stringify(rev)}\nIn ${treeOf(id)}: verify each finding against the code; fix every real blocker/major (and real minors when cheap); make every failing check pass. Rebuild and re-run the ${id} acceptance tests and 'make check'. Do not commit. Summarise what you fixed and what you rejected (with reason).`,
      { label: `fix:${id}#${round}`, phase: phaseName })
  }
  const done = await agentHW(`${CONTEXT}\n\nYou are the FINALIZER for ${id}. In ${treeOf(id)}: review 'git status'/'git diff', make sure no scratch files, build outputs or EA-derived data are staged, then commit the work of ${id} on its branch as one or a few logical commits (follow the COMMITS rules, including the two trailer lines). Update ${ROOT}/docs-hires/impl/${id}.md with the final state (commits, tests, known limitations). Return a 5-10 line summary including commit hashes.`,
    { label: `commit:${id}`, phase: phaseName })
  return { id, impl: impl.summary, deviations: impl.deviations_from_spec, open_issues: impl.open_issues, verify: last.ver && last.ver.pass, commit: done }
}

phase('M0')
const r00 = await runWP('WP-00', 'M0')
const r01 = await runWP('WP-01', 'M0')

const trackA = async () => {
  const out = []
  for (const id of ['WP-03', 'WP-04', 'WP-05', 'WP-06', 'WP-07']) out.push(await runWP(id, 'M1a'))
  return out
}
const trackB = async () => [await runWP('WP-08', 'Store')]
const trackC = async () => {
  const probe = await agentHW(`${CONTEXT}\n\nWORK PACKAGE WP-02 — Windows GPU probe (DESIGN.md §7.2 step 6, §9 WP-02 row, WP-02 acceptance). A Windows benchmark was prepared at ${ROOT}/tmp/b_perf_gpu/ (bench_win.c, bench_win.exe if built, SDL3.dll). If bench_win.exe is missing, check whether it can be cross-built with the llvm-mingw toolchain mentioned in ${ROOT}/tmp/b_perf_gpu (read its notes); if that is not feasible within ~30 minutes, skip and report. Copy bench_win.exe + SDL3.dll to /mnt/e/Dati/Ultima7_Upscale/probe/ and run it from WSL via Windows interop (e.g. 'cd /mnt/e/Dati/Ultima7_Upscale/probe && ./bench_win.exe direct3d11 1920 1200 1280 800 200', likewise direct3d12, vulkan, gpu), each under 'timeout 120'. It may briefly open a window on the user's desktop; that is acceptable. Collect the JSON/console output, write ${ROOT}/docs-hires/impl/WP-02.md with results (INDEX8 read-back exactness per renderer, timings) and a recommendation for present_format on Windows. Do not modify the repo.`,
    { label: 'impl:WP-02', phase: 'Probe' })
  return [{ id: 'WP-02', commit: probe }]
}

const [ra, rb, rc] = await parallel([trackA, trackB, trackC])

phase('Merge')
let merge = null
if (ra && rb) {
  merge = await agentHW(`${CONTEXT}\n\nYou are the INTEGRATOR. Merge branch 'hires-store' (WP-08, worktree ${ROOT}/exult-hires-store) into 'hires' in ${REPO} (a normal merge commit with the trailer lines). Resolve conflicts (likely in build descriptions: Makefile.am, Makefile.common, vcxproj/filters, tests/Makefile.am, configure.ac, exult.cc) keeping both sides' intent. Then: autoreconf, rebuild build-o2, build-asan (wrapper), build-sdl32; run 'make check' in all three and 'make check-game' (U7_BG_STATIC=/mnt/e/Games/RolePlayingGames/ultima7/static) in build-o2; all must pass. Fix integration issues if any (commit them). Remove the worktree only if the merge succeeded and the branch is fully merged. Write ${ROOT}/docs-hires/impl/M1a-merge.md and return a summary with the final 'git log --oneline 8b6ab6b43..hires'.`,
    { label: 'merge:store', phase: 'Merge' })
} else {
  log('Skipping merge: a track failed')
}
return { m0: [r00, r01], trackA: ra, trackB: rb, probe: rc, merge }
