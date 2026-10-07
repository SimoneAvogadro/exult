export const meta = {
  name: 'exult-hires-m1bc',
  description: 'M1b/M1c of the Exult hi-res fork: per-tile overrides, dump-art, dev loop, Windows build, perf, per-terrain overrides',
  phases: [
    { title: 'M1b', detail: 'WP-09 per-tile composition + real-pack demo, WP-11 dump-art, WP-10 dev loop' },
    { title: 'M1c', detail: 'WP-15 Windows build, WP-16 perf, WP-17 per-terrain overrides' },
    { title: 'Wrap', detail: 'docs and status report' },
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

const HW_NOTE = `\n\nHARDWARE NOTE: this PC had unstable RAM (bit-26 flips under load, a 0x1A bugcheck). The user has since disabled the RAM overclock, but stability is not yet proven. Keep the load moderate ('make -j8' at most, one build tree at a time, one ASan process at a time), re-run any failure that does not reproduce before debugging it, and never change code for a failure you cannot reproduce twice. The working tree may contain partial uncommitted work from an interrupted previous attempt of THIS step: inspect 'git status'/'git diff' first and continue from it.\nSTATE: M0+M1a are done and committed on 'hires' (scaled Image_buffer8, World_presenter with INDEX8 + tracked uploads already implemented (so WP-14 is done), world integration, --render-test harness + game oracles in tests/game, override store hires_png/rules/store + hires_glue + <HIRES> path tag merged from hires-store). Read ${ROOT}/docs-hires/impl/WP-0*.md and M1a-merge.md for what exists. Art packs: ${ROOT}/packs/bg (default, route-3 hybrid xBRZ, 3,885 flats, x6/flats/<SSSS>/SSSS_FF.png), also packs/bg-r3, bg-r2; mirrored at /mnt/e/Dati/Ultima7_Upscale/packs/. Art tooling (Python, u7hires, branch 'hires-art', worktree ${ROOT}/exult-hires-art) is NOT merged into 'hires' yet.`
const agentHW = (prompt, opts) => agent(prompt + HW_NOTE, opts)

const WPS = {
  'WP-09': { title: 'Per-tile composition (M1b engine gate) + first real-art demo', sections: '§3.4 (paint_flats with Hires::flat, precedence, reduction for S<6), §5.6, §6.4 (O4a identity pack, O4b marker pack, toggle test), §9 (WP-09 row)',
    extra: `Wire Hires::flat into Chunk_terrain::paint_flats (put_phys), generation checks, reduction path; mkpack_identity.py (identity + marker packs from NN templates; may be derived from art_original or the store's own tooling if --dump-art does not exist yet) under tools/hires; oracles O4a/O4b/toggle in tests/game with make check-game green (build-o2) and the region oracles once in build-asan. THEN the demo: run --render-test mode=plain overrides=yes with hires_path=${ROOT}/packs/bg at S=6 for the regions in tests/game/regions.txt (Britain, coast, forest, roads, dungeon...) and also overrides=no, writing PNGs (never committed) to ${ROOT}/tmp/demo-m1b/ plus side-by-side crops (NN vs override) and copy them to /mnt/e/Dati/Ultima7_Upscale/demo/ for the user. Check that the engine reports 3,885 loaded and 0 rejected for packs/bg. Commit on 'hires'.` },
  'WP-11': { title: '--dump-art (flats + terrain)', sections: '§4.2 (--dump-art), §5.2 (T1 key), §9 (WP-11 row), §12',
    extra: `dump_art.cc per §4.2: palettes (gpl/act/classes), flats raw-index PNGs with tEXt CRC, flats.txt, NN x6 templates, terrain/<t1>.png 1x flat layers painted by the engine (paint_flats, overrides off), terrain.txt, terrain_tiles.bin, terrain_map.bin, ref.txt. Determinism (two runs identical); parity: flat CRCs equal the u7hires/u7art Python values for all 3,885 flats (use the art worktree ${ROOT}/exult-hires-art tools read-only), and the engine's fill (effective source per cell) equals the Python fill port for all used terrains - report any mismatch precisely. Output to ${ROOT}/art_ref/bg. Commit on 'hires'.` },
  'WP-10': { title: 'Developer loop', sections: '§3.8, §5.7 (hot reload), §9 (WP-10 row), §6.7 (WP-10)',
    extra: `hires_dev.cc with HIRES_TOGGLE (Ctrl-Alt-O), HIRES_RELOAD (Ctrl-Alt-R), HIRES_INSPECT (Ctrl-Alt-I); keys.cc rows; defaultkeys for BG and SI; .reload trigger poll (500 ms, dev mode only); inspector also via --render-test inspect=tx:ty (golden JSON for 3 known tiles). Since keyboard input cannot be automated easily, test the actions through the render-test harness or a small test hook; manual check in WSLg optional. Commit on 'hires'.` },
  'WP-15': { title: 'Windows build (MSYS2 UCRT64 on E:) and measurements', sections: '§7.2, §9 (WP-15 row and decision rules as amended by §13: measure and report only), §6.7 (WP-15)',
    extra: `The user approved installing MSYS2 under E:\\Dati\\Ultima7_Upscale\\msys64 (no admin). Follow §7.2 (and its §12 TB-13 corrections): install from WSL via interop, pacman packages, clone branch 'hires' to /mnt/e/Dati/Ultima7_Upscale/src/exult-hires, build with Makefile.mingw (fix Makefile.mingw/Makefile.common for the new files if needed and commit fixes on 'hires' in ${REPO}, then pull), run hires_unit.exe and hires_present.exe, install to E:\\Dati\\Ultima7_Upscale\\ExultHires (NEVER touch the user's Exult 1.12.1 folder or its config/saves). Write E:\\Dati\\Ultima7_Upscale\\exult-hires.cfg: BG at E:\\Games\\RolePlayingGames\\ultima7, own saves/gamedat under ExultHires, hires_path=E:\\Dati\\Ultima7_Upscale\\packs\\bg, render_scale=art, dev=yes; windowed 1920x1200 with a 320x200 game view, scaler point. Also a launcher E:\\Dati\\Ultima7_Upscale\\Play-ExultHires.bat. Run --render-test with present=1 bench on D3D11/D3D12/Vulkan headless-ish (it opens a window briefly; acceptable) and record perf + pack load time in ${ROOT}/docs-hires/test/windows_matrix.md. Do not start an interactive game session that waits for input. Long downloads/builds: run them in the background and poll; MSYS2 pacman may need two -Syuu rounds.` },
  'WP-16': { title: 'Performance pass', sections: '§9 (WP-16 row), §6.7 (WP-16)',
    extra: `memset runs in the scaled RLE painter, row-batched translucency, fast_paths test against the legacy byte loop; measure with --render-test bench at -O2 (build-o2) before/after, including the 860x300 view at S=6 (the user's fullscreen profile) and record in docs-hires/impl/WP-16.md. O1 and all oracles stay green.` },
  'WP-17': { title: 'Per-terrain overrides', sections: '§3.4 (T1 key cache), §3.5 (Store::terrain), §5.2, §9 (WP-17 row), §6.7 (WP-17)',
    extra: `T1 key cache in Chunk_terrain (invalidated by set_flat/commit_edits), Store::terrain decode into the cache with precedence terrain > tile > NN, reduction, mkpack_identity.py --terrain, O4a (terrain) + precedence region check; T1 parity with --dump-art terrain keys. Commit on 'hires'.` },
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
function treeOf(id) { return `${REPO} (branch ${'hires'})` }

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

phase('M1b')
const results = []
for (const id of ['WP-09', 'WP-11', 'WP-10']) results.push(await runWP(id, 'M1b'))
phase('M1c')
for (const id of ['WP-15', 'WP-16', 'WP-17']) {
  try { results.push(await runWP(id, 'M1c')) } catch (e) { log(String(e)); results.push({ id, error: String(e) }) }
}
phase('Wrap')
const wrap = await agentHW(`${CONTEXT}\n\nYou are the INTEGRATOR. Work packages just completed: ${JSON.stringify(results).slice(0, 12000)}\nIn ${REPO} on 'hires': (1) update docs/hires/DESIGN.md is NOT needed; instead write the user guide docs/hires.md (how to enable render_scale, where packs go, dev keys, accepted visual mismatches per DESIGN §1.4, Windows launcher) and docs/hires_modding.md (palette pal0.gpl/act, naming, groups, rules, live reload) and commit them; (2) run the full test matrix once more (build-o2 make check + make check-game, build-asan make check, build-sdl32 make check) sequentially; (3) write ${ROOT}/docs-hires/impl/M1-status.md: what works, test results, Windows status, perf numbers, known limitations, next steps (M2 sprites, art phase B). Return a <=500-word summary.`, { label: 'wrap', phase: 'Wrap' })
return { results, wrap }
