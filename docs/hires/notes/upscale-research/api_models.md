# Hosted image-editing models (OpenRouter and similar APIs) for 6x Ultima VII art

Research date: **2026-10-04**. Scope: models reachable through OpenRouter, plus a few similar hosted APIs (fal.ai, Magnific, Replicate). The question is whether they can turn U7 8x8 palette tiles (and later RLE sprites) into faithful, seamless, consistent 48x48 (6x) art that can be quantized back to the game palette.

Labels used below:
- **[V]** means I fetched it myself: the page, or the live JSON API.
- **[S]** means it comes from search-result snippets only and was not opened.

Raw data snapshot: `docs-hires/upscale-research/openrouter_image_models_2026-10-04.json`. It holds all 57 OpenRouter image models with per-provider pricing and parameters, pulled from `GET https://openrouter.ai/api/v1/images/models` and `/api/v1/images/models/{id}/endpoints` on 2026-10-04 **[V]**.

Local inputs already extracted (`/home/simonea/ultima7_exult/art_original`):
- 3,885 flat 8x8 tiles (`flat_atlas.png` / `.json`)
- 2,105 chunk PNGs (128x128)
- 10,286 RLE frames
- 12 palettes

---

## 0. TL;DR

1. **OpenRouter has many image-editing models but no dedicated upscaler.** On 2026-10-04 it lists 57 image-output models. 55 of them accept an image input, so they can edit (only `recraft-v4.1-flash` and `ming-image-0.1-design` are text-only). None of them is a super-resolution or upscaling model: there is no ESRGAN, SeedVR2, Topaz, Clarity or Magnific. Every one is a **generative re-renderer**. It regenerates the whole frame, so even "unchanged" pixels move or shift colour. There is no pixel-exact preservation and no palette awareness.
2. **The output sizes are fixed tiers, not exact sizes.** Through OpenRouter you pick `resolution` (512 / 768 / 1K / 1.5K / 2K / 4K, depending on the model) and `aspect_ratio`. You cannot ask for exactly 48x48 or 768x768. The minimum is 512 px (Gemini 3.1 Flash Image) and is usually ~1 MP. A single 8x8 tile is therefore pointless as one request.
   - Work on **chunks or regions** instead: a 128 px chunk becomes 1024 px at 1K, which is 8x. Or pack **tile atlases**.
   - Then **downsample 8x → 6x**. This matches the "render high, then downscale" design you liked.
3. **Best hosted candidates for faithful "detail-up" edits:**
   - **Primary pilot set:**
     - `google/gemini-3.1-flash-image` (Nano Banana 2)
     - `google/gemini-3-pro-image` (Nano Banana Pro), for a golden or style set only
     - `black-forest-labs/flux.2-pro` / `flux.2-klein-4b` (have a seed; klein 4B is also Apache-2.0 open weights that run on a 16 GB card)
     - `bytedance-seed/seedream-4.5` / `seedream-5-0-*` (have a seed, cheap flat price)
     - `qwen/qwen-image-3` (has a seed)
   - **Secondary:** `openai/gpt-image-2` / `gpt-image-2.5-*`. Instruction-following is strong and transparent-background output helps sprites, but there is no seed and it costs more at high quality.
   - **Not suitable:** Recraft (style-transfer, raster or vector), Ming design models, Krea, Riverflow, MAI, Grok and Muse. They are fine generators, but nothing in their parameters or descriptions points to faithful upscaling.
4. **Cost is not the obstacle.** One pass over all 2,105 chunks costs roughly **$38 to $284**, depending on the model (Seedream 5.0 Flash at the low end, Nano Banana Pro at the high end). Packing all 3,885 flat tiles into atlases costs **$1 to $18**. A 360-request pilot costs **$5 to $76**. The real obstacles are **faithfulness, registration (geometry drift), seams, cross-request consistency, palette-cycling indices and reproducibility.**
5. **Hosted models alone cannot meet the hard constraints.** Palette quantization, preserving the cycling indices `0xE0..0xFE`, seam guarantees and alpha masks for sprites must all be **deterministic local post-processing** (section 5). Hosted models can only supply the "detail layer".
6. **Recommendation: a hybrid.**
   - Use OpenRouter for a **cheap pilot** (≤ $25) to see which model family gives the best faithful detail on U7 art.
   - Use **Nano Banana Pro or Nano Banana 2** to build a small hand-approved **golden set / style bible** of ~20-50 chunks.
   - Run the **bulk** pass locally on the RTX 5070 Ti with open weights. Options are FLUX.2 [klein] 4B (also hosted on OpenRouter, so pilot and production use the same model), Qwen-Image-Edit-2511 / Qwen-Image 2.1, or SeedVR2. Local runs give fixed seeds, no deprecation risk, zero marginal cost and unlimited retries.
   - Keep hosted models for hard cases.
7. **The OpenRouter MCP connector is not the right tool for batch work.**
   - In this session it is **not authenticated**: only `authenticate` / `complete_authentication` are exposed.
   - Its `generate-image` tool is described as "Create images from text prompts". Whether it accepts reference images is undocumented.
   - Its OAuth key expires after 7 days and has a $10 default spend limit.
   - Images would come back into the chat context.
   - For thousands of images, use the REST `POST /api/v1/images` endpoint with a normal API key from a Python script (section 7). The MCP is still handy for browsing the catalogue and pricing, and for a one-off visual test.

---

## 1. How image generation and editing works on OpenRouter (Oct 2026)

### 1.1 Two routes

| Route | Endpoint | Image input | Image output | Notes |
|---|---|---|---|---|
| **Images API** (dedicated; announced Jul 2026, updated 24 Sep 2026) | `POST https://openrouter.ai/api/v1/images` | `input_references: [{type:"image_url", image_url:{url:"data:image/png;base64,..." or "https://..."}}]` | `data[].b64_json` + `data[].media_type`, `usage.cost` | Normalized params, per-model capability descriptors, streaming partial images for some models, all-or-nothing billing **[V]** |
| Chat Completions | `POST /api/v1/chat/completions` with `modalities: ["image","text"]` (or `["image"]`) | content part `{type:"image_url", image_url:{url: data-URL}}` | `choices[].message.images[]` | `image_config` is "provider-specific", e.g. `aspect_ratio`, `image_size`. Kept for compatibility. **[V]** for the parameter definitions in the API reference; the mapping details are **[S]** |

For batch work, use the **Images API**. Its parameters are typed and discoverable, and the response is plain base64 PNG.

### 1.2 Images API parameters [V]
Source: https://openrouter.ai/docs/guides/overview/multimodal/image-generation (fetched 2026-10-04; no page date shown).

- `model`, `prompt` (required)
- `resolution`: `512`, `768`, `1K`, `1.5K`, `2K`, `4K` (each model supports a subset)
- `aspect_ratio`: `1:1`, `16:9`, `9:16`, `4:3`, `3:4`, `3:2`, `2:3`, `4:5`, `5:4`, `1:2`, `2:1`, `1:4`, `4:1`, `1:8`, `8:1`, `9:21`, `21:9`, `auto`
- `size`: tier (`"2K"`) or explicit pixels (`"2048x2048"`). **Only `inclusionai/ming-*` declares `size` support** in the live catalogue. For every other model, exact pixel sizes are not available through OpenRouter.
- `quality` (`auto|low|medium|high`, plus `xhigh|max` on GPT Image 2.5), `output_format` (`png|jpeg|webp|svg`), `background` (`auto|transparent|opaque`), `output_compression`, `n` (1-10), `seed` (where supported), `stream`
- `provider`: `{only, order, ignore, sort, allow_fallbacks, options:{<slug>:{...passthrough...}}}`. Passthrough examples from the live endpoints:
  - FLUX.2: `steps`, `guidance`, `safety_tolerance`
  - Gemini: `cachedContent`
  - OpenAI: `moderation`
  - Krea: `creativity`, `strength`, ...
  - Recraft: `style`, `controls`, ...
- Discovery: `GET /api/v1/images/models`, then `GET /api/v1/images/models/{id}/endpoints`. These give definitive pricing and parameters per provider, and they are public: no key was needed **[V]**.
- Billing: "A generation is either completed and billed in full, or it fails and is not billed". Partial stream previews are free **[V]**.
- Privacy routing: `provider.data_collection: "deny"` and `provider.zdr: true` are documented for routing **[S]** (https://openrouter.ai/docs/guides/routing/provider-selection). I did not verify that the Images endpoint honours them.

Edit example. This is from OpenRouter's Nano Banana tutorial (https://openrouter.ai/blog/tutorials/nano-banana/, updated 2026-09-24) **[V]**:
```python
resp = requests.post("https://openrouter.ai/api/v1/images",
    headers={"Authorization": f"Bearer {api_key}"},
    json={"model": "google/gemini-3.1-flash-image",
          "prompt": "Add a red wool scarf ... Keep everything else the same.",
          "input_references": [{"type": "image_url", "image_url": {"url": source_data_url}}]})
png = base64.b64decode(resp.json()["data"][0]["b64_json"])
```
The tutorial also gives:
- Accepted input types: PNG, JPEG, WebP, HEIC, HEIF.
- "Large files can time out or fail".
- Advice to edit in small steps with one instruction per call.

### 1.3 OpenRouter MCP connector [V]
Source: https://openrouter.ai/docs/guides/overview/mcp-server (no date shown).

- Remote server `https://mcp.openrouter.ai/mcp`, connected with OAuth (PKCE). The dedicated key expires after **7 days** and has a **$10 default spend limit**, which you can edit when you approve it.
- Tools:
  - `send-message`, `generate-image`
  - `list-models`, `get-model`, `list-model-endpoints`, `list-providers`
  - `list-benchmarks`, `get-credits`, `get-generation`, `search-docs`, ...
- The docs do not give a schema for `generate-image`. The tool is described as "Create images from text prompts", so support for reference or input images is **unverified**.
- In this Claude Code session the connector appears only as `authenticate` / `complete_authentication`, which means it is **not authorized yet**. I did not start the OAuth flow because it needs you in the browser.
- **Use it for:** catalogue and pricing lookups, plus one or two eyeball tests.
- **Do not use it for:** batch runs. Images returned into the LLM context are costly and lossy for automation.
- Third-party MCPs that wrap the Images API with editing exist, but none is needed: a 60-line Python script is simpler and auditable. Examples: `pinkpixel-dev/pixara-mcp`, `jtxmp/openrouter-image-gen-mcp`, `stabgan/openrouter-mcp-multimodal` **[S]**.

---

## 2. OpenRouter image-model catalogue, snapshot 2026-10-04 [V]

The columns below come from the live JSON:
- **Refs** is the `input_references` range. Max ≥ 1 means the model can edit.
- **Seed** is "yes" when the model declares `seed` support.
- **Price** is the per-endpoint pricing record. "tok" means per output-image token.

Google's per-image figures are from the Gemini pricing page (updated 2026-10-01) **[V]**:

| Model id (added to OR) | Refs | Res tiers | Seed | Price (output; input where billed) | Note |
|---|---|---|---|---|---|
| `google/gemini-3.1-flash-image` "Nano Banana 2" (2026-06-18; preview 2026-02-26) | 0-14 | 512, 1K, 2K, 4K | – | $60/M tok: **512 $0.045, 1K $0.067, 2K $0.101, 4K $0.151** | Best price/quality editor; also via AI Studio and Vertex |
| `google/gemini-3.1-flash-lite-image` "Nano Banana 2 Lite" (2026-06-30) | 0-14 | 1K | – | **$0.0336 / 1K image** | Cheapest Google model; 1K only |
| `google/gemini-3-pro-image` "Nano Banana Pro" (2026-06-18; preview 2025-11-20) | 0-14 | 1K, 2K, 4K (Vertex: 1K, 2K) | – | **1K/2K $0.134, 4K $0.24**; input ~$0.0011 | Highest fidelity of the Google models; "thinking" (renders up to 2 interim images) |
| `google/gemini-2.5-flash-image` "Nano Banana" (2025-10-07) | 0-3 | – | – | $0.039 | **Google shut it down on 2026-10-02** (pricing page). Still listed on OR; do not use |
| `openai/gpt-image-2` (2026-06-24) | 0-16 | aspect only | – | in $8/M, out $30/M tok (≈ $0.006 low to $0.21 high per 1024² image, per the OR blog) | Always high input fidelity; min 655,360 px (OpenAI docs) |
| `openai/gpt-image-2.5-sunburst` / `-flare` (2026-09-09) | 0-16 | aspect only | – | same token rates; quality up to `xhigh`, `max` | "Sunburst" is the editing-precision tier; transparent background supported |
| `openai/gpt-image-1`, `-1-mini`, `gpt-5-image(-mini)`, `gpt-5.4-image-2` | 0-16 | aspect only | – | token-based | Older or LLM-wrapped variants |
| `black-forest-labs/flux.2-pro` (2025-11-25) | 0-8 | aspect only (~1-4 MP) | yes | $0.03/MP out (BFL direct: $0.03 first MP + $0.015 per extra MP in+out **[S]**) | Passthrough `steps`, `guidance`, `safety_tolerance` |
| `black-forest-labs/flux.2-flex` (2025-11-25) | 0-8 | aspect only | yes | $0.06/MP in + $0.06/MP out | Exposes `steps` / `guidance`: the most "img2img-like" control |
| `black-forest-labs/flux.2-max` (2025-12-16) | 0-8 | aspect only | yes | $0.07/MP out | Top FLUX.2 tier |
| `black-forest-labs/flux.2-klein-4b` (2026-01-14) | 0-4 | aspect only | yes | **$0.014/MP** | **Open weights, Apache-2.0, ~13 GB VRAM (FP8/NVFP4 smaller)** **[S]**; same model locally |
| `black-forest-labs/flux-3-image` (2026-10-01) | 0-10 | 768, 1K, 1.5K, 2K, 4K | – | 768 $0.041, 1K $0.048, 1.5K $0.07, 2K $0.10, 4K $0.607 (50% promo at launch) | Brand new; no seed parameter declared |
| `bytedance-seed/seedream-4.5` (2025-12-23) | 0-14 | 1K, 2K, 4K | yes | **$0.04 flat**, input free | Description stresses "editing consistency ... preservation of subject details" |
| `bytedance-seed/seedream-5-0-flash` (2026-10-01) | 0-14 | 1K, 2K | yes | **$0.018 flat** | Cheapest editor with a seed |
| `bytedance-seed/seedream-5-0-pro` (2026-08-12) / `-lite` (2026-08-13) | 0-14 | 1K, 2K / 2K, 4K | yes | $0.045 (+$0.003 input) / $0.035 | Lite does web-connected retrieval (irrelevant here) |
| `qwen/qwen-image-3` / `-3-pro` (2026-08-05) | 0-4 | 1K, 2K | yes | $0.03 (pro $0.04 at 1K / $0.075 at 2K) + $0.003 input | Qwen-Image 3.0 is **closed weights** **[S]**; open siblings exist (section 4) |
| `x-ai/grok-imagine-image-2.0` / `-quality` | 0-3 | 1K, 2K | – | $0.04-0.08 + $0.01 input | Photoreal focus |
| `microsoft/mai-image-2.5(-pro)`, `mai-image-2.6(-flash)` | 0-1 / 0-5 | aspect only | – | token-based | Azure-hosted; photoreal/design focus |
| `krea/krea-2-*` (2026-07-20) | 0-1 | 1K | yes | (no pricing record) | Passthrough `creativity` / `strength` (interesting, but 1K only and 1 ref) |
| `sourceful/riverflow-v2(.5)-*` | 0-4 / 0-10 | 1K-4K | – | $0.02-0.33; v2 charges **$0.20 per reference image** | Transparent background supported |
| `recraft/recraft-v3/v4/v4.1(-pro/-utility/-vector/-flash)`, `recraft-v4-styles*` | 0-1 / 1-10 | aspect only | styles: `random_seed` passthrough | $0.007-$0.30 | Style-reference generators and SVG output; **not** faithful editors. Recraft's actual upscalers are **not** on OR (they are on fal, section 4) |
| `meta/muse-image` (2026-08-26) | ? | – | – | token-based | "Agentic, reasons before it renders"; parameters not declared |
| `inclusionai/ming-image-0.1-design(-layer)` | 0-1 | `size` | – | $0 (Novita) | Graphic design and layer decomposition; not relevant |
| `openrouter/auto(-beta)` | – | – | – | – | Router; do not use for reproducible asset work |

Observations from the snapshot:
- **There is no dedicated upscaler on OpenRouter.** Each model is a text- or reference-conditioned generator.
- **Seed is only declared** by FLUX.2 (pro/flex/max/klein), Seedream (4.5 and 5.0), Qwen Image 3 and Krea 2. Gemini and OpenAI have **no seed**, so their output is non-deterministic.
- **Exact output size is not controllable** through OR for any useful model. You get tier × aspect. Typical results:
  - Gemini 1:1: 1K = 1024², 2K = 2048², 4K = 4096².
  - GPT Image: fixed or custom sizes on the native API (multiples of 16, 655,360 to 8,294,400 px). Through OR only `aspect_ratio` + `quality` are exposed.
  - FLUX.2 on BFL's native API accepts `width` / `height` (min 64) and `disable_pup` (turns off prompt up-sampling) **[V]** (https://docs.bfl.ai/api-reference/models/generate-or-edit-an-image-with-flux2-%5Bpro%5D). OR does **not** expose those. Use the native API if you need exact dimensions or want to stop the prompt being rewritten.
- **Model churn is fast.** Of the 57 models, 23 were added in the last ~3 months, and Nano Banana v1 was switched off on 2 Oct 2026. Archive every raw output with its model id, date, prompt and seed. Treat outputs as **assets**, not as a pipeline you can reproduce later.

---

## 3. Suitability of the shortlisted hosted models for U7 tiles

The criteria come from the brief: (a) faithful, with no hallucinated content; (b) seamless; (c) style consistent across ~3-10k items; (d) palette-cycling indices preserved; (e) cost and determinism.

| Model | Faithfulness / registration | Detail quality on stylised art | Consistency across requests | Determinism | Fit for U7 tiles |
|---|---|---|---|---|---|
| **Gemini 3.1 Flash Image (NB2)** | Good at "keep everything the same" edits, but the whole frame is regenerated, so expect small geometric drift and colour shifts. Earlier Gemini versions had aspect-ratio and size drift bugs (e.g. 2048×1024 input → 1472×704 output) **[S]**. | Strong. Widely used for "creative upscale" of illustrations **[S]**. | Medium. Pass 1-3 fixed **style-reference images** (golden chunks) in every request (up to 14 refs). | No seed. | **Medium-High** for chunk or region context. Low for per-pixel exactness. |
| **Gemini 3 Pro Image (NBP)** | Best of the Google models; "thinking" improves adherence. | Best. A ComfyUI "creative upscale" template exists **[V]** (https://comfy.org/workflows/utility_nanobanana_pro_illustration_upscale-33747bd52ad5/). | Medium-High with refs. | No seed. | **High for building a golden set**. Too pricey for bulk ($0.134/img). |
| **FLUX.2 pro / flex / klein 4B** | Good structure preservation; flex lets you tune `steps` / `guidance`. | Good. Klein is weaker but fast. | Medium-High: seed + same prompt + refs. | **Seed yes**. Klein can also run locally, so you can reproduce it exactly. | **High as the bridge model**: pilot hosted, then go local. |
| **Seedream 4.5 / 5.0** | Vendor stresses edit consistency; flat price; up to 14 refs. | Good, tends toward a polished look. | Medium-High (seed). | Seed yes. | **Medium-High on cost.** Cheapest serious editor ($0.018-0.04). |
| **Qwen Image 3 (/pro)** | Qwen-Image-Edit is known for precise edits; v3 hosted only. | Good; "details as small as 10px" claim. | Medium-High (seed). | Seed yes. | **Medium.** The open-weight Qwen-Image-Edit-2511 / Qwen-Image 2.1 can run locally. |
| **GPT Image 2 / 2.5 Sunburst** | Always high input fidelity, but it re-renders. OpenAI lists "precise element placement in structured compositions" as a known limitation **[V]**. | Very strong style capture. | Medium. | No seed. | **Medium.** Useful for **sprites** (transparent background). Costly at high quality. |
| Recraft, Krea, Riverflow, MAI, Grok, Muse, Ming | Not built for faithful edits. Recraft "styles" are explicitly style-transfer generators. | – | – | – | **Low** |

General behaviour of every model above:
- **They regenerate. They do not upscale.** Without careful prompting they "improve" the art. Typical changes are new flowers or rocks, cleaner shorelines, modern lighting, perspective shading, and "fake pixel art" with an uneven grid and extra colours. Pixel-art-specific blog posts say the same and recommend "prompt for the style, fix the grid after" with grid detection plus palette quantization **[V]** (SpriteCook, 2026-03-19).
- **Every Gemini image carries a SynthID watermark** **[V]**. It is invisible and gets largely destroyed by palette quantization and downsampling. It is still a perturbation, and it means the output is AI-generated by declaration.
- **Content filters.** Terrain is harmless. RLE sprites include corpses, blood and hanged bodies, which may be refused or sanitised. Use `moderation: "low"` (OpenAI) or `safety_tolerance` (FLUX) where available.

---

## 4. Similar hosted APIs outside OpenRouter (dedicated upscalers)

These are "real" super-resolution or upscaling services. Most are faithful (non-generative) or have a fidelity knob. None of them knows about palettes.

| Service / model | Kind | Price | Fidelity control | Fit | Source |
|---|---|---|---|---|---|
| fal `fal-ai/seedvr/upscale/image/seamless` (SeedVR2) | One-step diffusion restoration; "retaining seamless tiling" | **$0.0025/MP** | `upscale_factor` (float), `target_resolution` 720p-2160p, `seed`, `noise_scale` (0.1) | **Medium-High**: faithful, seamless-aware, seeded. Weights are Apache-2.0 (3B/7B), and ComfyUI runs FP8 in 12-16 GB, so it can run locally on the 5070 Ti | [V] fal API schema; [S] HF license / VRAM |
| fal `fal-ai/recraft/upscale/crisp` | Faithful upscaler | $0.004/img | none exposed | Medium. Very cheap, but trained on photos and faces | [V] |
| fal `topaz/upscale/image/precision` / `generative` / `creative` | Topaz models (Standard V2, Wonder 3.5, Bloom 2) | $0.08 / $0.24 / $0.96 per 24 MP | model choice | Medium (precision) / Low (creative) | [V] fal list, 2026-08-26 |
| fal `fal-ai/clarity-upscaler` | SD-based tiled img2img upscaler | $0.03/MP | prompt, creativity, resemblance | Medium. The classic "steerable" upscaler | [V] fal list |
| fal `fal-ai/aura-sr`, `fal-ai/drct-super-resolution` | GAN / transformer SR | ~$0.0008/s / $0.0045/MP | none | Low-Medium: photo-trained, smears pixel art | [V] fal list |
| **Magnific API** (Freepik, rebranded Apr 2026) | Generative upscaler | pay per output area | `scale_factor` 2/4/8/16, **`optimized_for: videogame_assets`**, `creativity` / `resemblance` / `hdr` / `fractality` in −10..10, `engine`, prompt | **Medium-High**. The closest hosted product to a "faithful detail-up with a resemblance knob". Not on OR | [V] docs.magnific.com |
| Replicate `retro-diffusion/rd-tile` / `rd-plus` | Pixel-art **generators** (grid-aligned, limited palette), tilesets with transitions | per run | style presets | **Low for 6x**: they output low-res pixel art, which is the opposite of the goal. Possibly useful later for brand-new tiles | [V] replicate page; [S] Nov 2025 launch |

Pitfalls of photo-trained super-resolution on 8 px art:
- Fed raw 8x8 or 128x128 input, ESRGAN-, DRCT- and SeedVR-style models smear the art into "oil paint" or amplify dithering.
- The usual fix is to feed a **4x-8x nearest-neighbour or xBR pre-upscale**, sometimes with a light blur, and let the model "restore" that.
- These models also max out at 4x per pass. To reach 6x, run 8x (NN 2x → 4x model) and **downsample to 6x**.

---

## 5. Hard requirements vs. hosted models: what has to happen locally

| Requirement | Can a hosted model do it? | Required local handling |
|---|---|---|
| Exact 48x48 per tile / 768x768 per chunk | No: only tier sizes, min 512-1024 px | Send **8x input** (128 px chunk → 1024 at 1K; 256 px window → 2048 at 2K). Crop, then **area-downsample 8x → 6x** (1024 → 768). This is the "supersample then downscale" design. |
| Registration (no drift) | Not guaranteed | Pre-upscale the input to the **output size** so the model never resizes geometry, and keep gutters or registration marks in atlases. Then, after generation: <ul><li>**downsample the output to source resolution and compare** (OKLab ΔE per pixel)</li><li>**phase-correlate** to detect shifts</li><li>reject and retry when the shift is > 0.25 source px or mean ΔE exceeds a threshold</li></ul> |
| Palette (indices 1..0xDF of palette 0) | No: RGB out, many colours | **Local quantization** to palette 0 in OKLab / CIEDE2000. Optionally **ramp-constrained**: each output pixel may only take colours from the ramp of the corresponding source pixel's index (`Palette::get_ramps`), which stops hue drift. Dithering is optional (probably off for terrain). |
| Palette-cycling indices `0xE0..0xFE` (water sparkles, lava, fire, "void") | No | Build a **mask from the source indices** and upscale it with NN or xBR on the index map. Inside the mask, write the cycling index (NN or xBR-upscaled from the source) and ignore the model's pixels. Optionally let the model's luminance pick among the 8 indices of the same cycle range, to keep detail while keeping animation. Also keep `0x00` / `0xFF` semantics (transparent keys). |
| Seamless tiles / chunks | Partially, with context windows | <ol><li>Generate on **overlapping context windows** (chunk plus 32-64 source px of real neighbours), crop the centre, and feather the overlaps.</li><li>Unique chunk *types* are reused with different neighbours across the 192×192-chunk world, so also add a **seam guard band**: the outer 1-2 output px ring of each tile or chunk comes from a deterministic upscale (NN / xBR) of the original border pixels, cross-faded into the generative interior. The original art already tiles at 1x, so the deterministic ring tiles too.</li><li>Run an automatic seam check: compare edge strips of every adjacent pair that actually occurs in the map.</li></ol> |
| Style consistency across thousands of requests | Partially | Use a fixed prompt template, a fixed model id and a fixed seed where available. Pass **the same 1-3 approved "golden" chunks as style references** in every request. Process tiles of one terrain family in the same batch or atlas. Run a colour-statistics check per family. |
| Determinism / reproducibility | Only partly (seeded models), and model versions change | Archive raw outputs and request JSON. Prefer seeded models. Plan for the local open-weight path. |
| RLE sprite transparency | Partly (GPT Image `background: transparent`, Riverflow) | Composite the sprite on a flat key colour (e.g. magenta, or a mid-grey not in the sprite). Take the **final alpha from an NN/xBR upscale of the original mask**, optionally eroded or dilated by 1 px. Never trust model alpha for the shape outline. Also keep NPC palette-remap regions in index space. |

Engine-design note: hosted models work best on **context-rich regions** (chunks or windows) and worst on isolated 8x8 tiles. You can still get per-shape/frame overrides (48x48 per flat tile) in two ways:
- **(a)** Upscale tiles inside a canonical 3x3 neighbourhood (the most frequent neighbours on the map), packed into atlases, and keep the centre.
- **(b)** Upscale chunks and then *derive* per-tile art by voting across occurrences.

The alternative is **per-chunk overrides**: 2,105 × 768² ≈ 1.24 Gpx at 8-bit, which compresses to a few hundred MB. That is simpler for the generator but heavier for the engine. Decide this before running a bulk pass.

---

## 6. Cost model (from live OR prices, 2026-10-04)

Scenarios:
- **S1**: one request per chunk, 1K output (8x), no margin. 2,105 requests.
- **S2**: chunk + 64 px real-neighbour margin = 256 px window → 2K output (8x), centre crop. 2,105 requests.
- **S3**: all 3,885 flat tiles in 3x3-neighbourhood cells (24 px + 8 px gutter = 32 px) packed 8×8 per 256 px atlas → 2K output. 61 requests.
- **S4**: pilot, 20 chunks × 6 models × 3 prompt variants at 1K. 360 requests.

Retries (expect 1.5-3x) are **not** included.

| Model | 1K edit | 2K edit | S1 | S2 | S3 | S4 |
|---|---|---|---|---|---|---|
| gemini-3.1-flash-image (NB2) | $0.067 | $0.101 | $141 | $213 | $6 | $24 |
| gemini-3.1-flash-lite-image | $0.034 | n/a | $71 | n/a | n/a | $12 |
| gemini-3-pro-image (NBP) | $0.135 | $0.135 | $284 | $284 | $8 | $49 |
| gpt-image-2 medium (~estimate) | ~$0.05 | n/a via OR tiers | ~$105 | – | – | ~$18 |
| gpt-image-2 high (~estimate) | ~$0.21 | – | ~$442 | – | – | ~$76 |
| flux.2-pro (OR $0.03/MP out) | $0.03 | $0.12 | $63 | $253 | $7 | $11 |
| flux.2-klein-4b | $0.014 | $0.056 | $29 | $118 | $3 | $5 |
| flux.2-flex ($0.06/MP in+out) | $0.12 | $0.30 | $253 | $632 | $18 | $43 |
| flux-3-image (list price) | $0.048 | $0.10 | $101 | $210 | $6 | $17 |
| seedream-4.5 | $0.04 | $0.04 | $84 | $84 | $2 | $14 |
| seedream-5-0-flash | $0.018 | $0.018 | $38 | $38 | $1 | $6 |
| seedream-5-0-pro | $0.048 | $0.048 | $101 | $101 | $3 | $17 |
| qwen-image-3 | $0.033 | $0.033 | $69 | $69 | $2 | $12 |
| qwen-image-3-pro | $0.043 | $0.078 | $91 | $164 | $5 | $15 |

For comparison:
- RLE sprites: 10,286 frames, many small. Packed into atlases this is a few hundred requests, roughly $10-60 per model and pass.
- Google's own Batch API is 50% cheaper (NB2: $0.022-0.076 per image) **[V]**. It is not available through OpenRouter.
- Local open-weight inference on the 5070 Ti costs electricity only.

---

## 7. Suggested pilot (hosted), then the decision

1. **Inputs.** Choose 20 representative chunks:
   - grass, dense forest floor
   - water with sparkles, shoreline transitions
   - road / grass edges, desert / sand, swamp
   - stone and wood floors, carpets, lava
   - "void"

   For each one, build a **256 px window** (chunk + 64 px real neighbours from the map). Make two input variants:
   - (a) **NN 8x** (2048 px)
   - (b) **xBR/hq4x → bicubic to 8x** (pre-smoothed)
2. **Models.** Run NB2 at 2K, NBP at 2K, FLUX.2-pro (seed fixed; also flex with `guidance` / `steps` sweeps), Seedream 4.5 / 5.0 Flash (seed), Qwen Image 3 (seed) and GPT Image 2.5 Sunburst (high).
3. **Prompt template (faithful detail-up).** Keep it short and declarative. Also pass 1-2 approved golden chunks as extra `input_references` once you have them.
   > "Top-down tile map from a 1992 VGA role-playing game (Ultima VII), shown enlarged; each 8×8 block is one original pixel. Re-render it as a high-resolution hand-painted game texture **at exactly the same framing**. Keep every shape, edge, shoreline, path and colour region in the same place and the same colour. Do not add, remove or move anything; no new objects, plants, rocks, text, borders, lighting changes or perspective. Only replace the blocky pixels with fine natural detail (grass blades, soil grain, stone texture, wood grain, water ripples) using the original colours."
   - Add family-specific hints only (e.g. "water: keep the small white sparkle specks as tiny bright dots").
   - Avoid words like "enhance", "beautify" or "realistic". They invite re-imagining.
4. **Automatic scoring** (local Python, in `E:\Dati\Ultima7_Upscale\pilot\` or WSL). For each output:
   1. Check the output size and crop the centre 1024 px.
   2. Area-downsample to 128 px. Compute mean and P95 OKLab ΔE against the source RGB, plus SSIM on luminance, plus a phase-correlation shift.
   3. Palette-quantize the 768 px (6x) version and count how many pixels fall back to cycling indices from the mask.
   4. Run a seam check against neighbour windows.
   5. Put everything on a contact sheet for a human A/B: source NN 6x vs. xBR 6x vs. model 6x.
5. **Decision gate.**
   - **Keep:** a model whose P95 shift is ≤ 0.25 source px and whose mean ΔE is low enough that quantized output keeps ramp identity (e.g. ≥ 95 % of pixels map to the same ramp as their source pixel) on ≥ 90 % of windows, *and* which wins the visual A/B.
   - **If a hosted model wins,** consider producing only the golden set and hard cases with it.
   - **For the bulk,** replicate the winning look locally: FLUX.2 klein 4B (same weights as hosted), Qwen-Image-Edit-2511 / Qwen-Image 2.1, or SeedVR2 with a style LoRA trained on the golden set. That gives determinism and unlimited retries.

Minimal batch client (Images API). It does not depend on the MCP:
```python
import base64, json, os, pathlib, time, requests
API = "https://openrouter.ai/api/v1/images"
KEY = os.environ["OPENROUTER_API_KEY"]

def data_url(p):
    return "data:image/png;base64," + base64.b64encode(pathlib.Path(p).read_bytes()).decode()

def edit(src_png, model, prompt, *, resolution="2K", aspect="1:1", seed=None,
         refs=(), passthrough=None, out_dir="out"):
    body = {
        "model": model, "prompt": prompt, "n": 1, "aspect_ratio": aspect,
        "input_references": [{"type": "image_url", "image_url": {"url": data_url(p)}}
                             for p in (src_png, *refs)],
        "provider": {"allow_fallbacks": False},
    }
    if resolution: body["resolution"] = resolution     # only if model declares it
    if seed is not None: body["seed"] = seed           # only if model declares it
    if passthrough:                                    # e.g. {"black-forest-labs": {"guidance": 2.5, "steps": 40}}
        body["provider"]["options"] = passthrough
    r = requests.post(API, headers={"Authorization": f"Bearer {KEY}"}, json=body, timeout=600)
    r.raise_for_status()
    j = r.json()
    out = pathlib.Path(out_dir) / model.replace("/", "__")
    out.mkdir(parents=True, exist_ok=True)
    stem = pathlib.Path(src_png).stem + (f"_s{seed}" if seed is not None else "") + f"_{int(time.time())}"
    (out / f"{stem}.png").write_bytes(base64.b64decode(j["data"][0]["b64_json"]))
    meta = {"request": {k: v for k, v in body.items() if k != "input_references"},
            "usage": j.get("usage"), "src": str(src_png), "refs": list(map(str, refs))}
    (out / f"{stem}.json").write_text(json.dumps(meta, indent=1))
    return out / f"{stem}.png"
```
Before sending a parameter, check `supported_parameters` for the model in `openrouter_image_models_2026-10-04.json`. Do not send `resolution` to OpenAI or FLUX.2, and do not send `seed` to Gemini or OpenAI. Use moderate concurrency (4-8 parallel). Failed generations are not billed.

---

## 8. Risks and pitfalls (hosted path)

- **Hallucination / meaning change.** Models add rocks, flowers or tufts, "fix" deliberate irregularities, straighten coastlines, or turn an 8 px detail into a different object. Mitigations:
  - an explicit negative prompt
  - pre-smoothed input
  - local ΔE / registration rejection
  - ramp-constrained quantization
  - human review of contact sheets per terrain family
- **Geometry drift and resizing.** Outputs can be offset, slightly zoomed or have the wrong aspect. Gemini has a history of aspect and size bugs **[S]**. OR exposes no exact size. Mitigations: square inputs at exactly the tier size, gutters / registration marks in atlases, and automatic phase-correlation rejection.
- **Seams** between independently generated chunks or tiles, and between unique chunk types whose neighbours vary across the map. Mitigations: overlapping context windows, the seam guard band and a seam check over all neighbour pairs that occur in the map.
- **Inconsistency between requests** (lighting, saturation, detail scale). Mitigations: fixed refs (golden set), fixed seed, batching by family, per-family colour-histogram normalisation before quantization.
- **No palette or index awareness.** Cycling water and lava, translucency indices `0xEE..0xFE` in RLE frames, `0x00` / `0xFF` keys and NPC remap ranges must all be restored locally. A hosted model cannot keep them.
- **Non-determinism and deprecation.** Gemini and OpenAI have no seed. Models disappear: Nano Banana v1 shut down 2026-10-02. Prices and behaviour change silently between versions, so archive everything.
- **Moderation.** Gore sprites may be refused or sanitised. Retries cost time; refusals cost nothing.
- **Watermarking.** SynthID is present in all Gemini outputs. It is mostly destroyed by quantization, but releases should still note that the art is AI-generated.
- **Legal / ToS.** The source art is copyrighted (Origin/EA). Uploading it to third-party providers and distributing derivatives is a grey area. It is fine for personal use, but check before publishing an Exult mod pack. Use `provider.data_collection: "deny"` / `zdr: true` where supported **[S]**, and prefer providers that do not train on paid API data.
- **The MCP connector is not suited to batch work.** It is not authorized in this session. The OAuth key expires after 7 days with a $10 default cap, and reference-image support is unverified. Use REST with your own key, and set a hard credit limit on that key in the OpenRouter dashboard.

---

## 9. Sources

Verified by fetching (2026-10-04):
- OpenRouter Images API guide: https://openrouter.ai/docs/guides/overview/multimodal/image-generation (no date shown)
- OpenRouter live catalogue JSON: https://openrouter.ai/api/v1/images/models and `/api/v1/images/models/{id}/endpoints` (snapshot saved); also https://openrouter.ai/api/v1/models?output_modalities=image
- OpenRouter models page (partial render): https://openrouter.ai/models?output_modalities=image
- OpenRouter chat-completions API reference (`modalities`, `image_config`, `seed`): https://openrouter.ai/docs/api/api-reference/chat/send-chat-completion-request
- OpenRouter "Every modality, one API" (2026-07-16, updated 2026-09-24): https://openrouter.ai/blog/insights/every-modality-one-api/
- OpenRouter "Image generation models" guide (2026-07-27, updated 2026-09-24): https://openrouter.ai/blog/tutorials/image-generation-models/
- OpenRouter Nano Banana editing tutorial (updated 2026-09-24): https://openrouter.ai/blog/tutorials/nano-banana/
- OpenRouter MCP server docs: https://openrouter.ai/docs/guides/overview/mcp-server
- OpenRouter model page NB2: https://openrouter.ai/google/gemini-3.1-flash-image
- Gemini API pricing (updated 2026-10-01): https://ai.google.dev/gemini-api/docs/pricing
- Gemini image generation docs: https://ai.google.dev/gemini-api/docs/image-generation
- OpenAI image generation guide (GPT Image 2.5; no date shown): https://developers.openai.com/api/docs/guides/image-generation
- BFL FLUX.2 [pro] API reference: https://docs.bfl.ai/api-reference/models/generate-or-edit-an-image-with-flux2-%5Bpro%5D ; FLUX.2 editing overview: https://docs.bfl.ai/flux_2/flux2_image_editing
- fal "10 Best Image-to-Image Upscalers in 2026" (2026-08-26): https://fal.ai/learn/tools/image-to-image-upscalers
- fal SeedVR2 seamless upscaler (page and API schema): https://fal.ai/models/fal-ai/seedvr/upscale/image/seamless , https://fal.ai/models/fal-ai/seedvr/upscale/image/seamless/api
- fal Recraft Crisp Upscale: https://fal.ai/models/fal-ai/recraft/upscale/crisp
- Magnific Creative Upscaler API: https://docs.magnific.com/api-reference/image-upscaler-creative/post-image-upscaler
- Replicate Retro Diffusion rd-tile: https://replicate.com/retro-diffusion/rd-tile
- SpriteCook, "Turning Nano Banana 2 AI images into actual pixel art" (2026-03-19): https://www.spritecook.ai/blog/nanobanana-pixel-art-for-games
- Comfy.org Nano Banana Pro creative-upscale workflow (~early 2026): https://comfy.org/workflows/utility_nanobanana_pro_illustration_upscale-33747bd52ad5/

Snippet-only (search results, not opened):
- GPT Image 2 sizes / `input_fidelity` behaviour (Apr 2026): https://docs.apiyi.com/en/news/gpt-image-2-launch , https://wavespeed.ai/blog/posts/gpt-image-2-api-guide/
- Gemini aspect-ratio drift reports (2025): https://discuss.ai.google.dev/t/gemini-2-5-flash-nano-banana-auto-aspect-ratio-issue-output-image-has-different-aspect-ratio/108225 , https://piunikaweb.com/2025/09/30/gemini-aspect-ratio-bug-fix-in-the-works/
- FLUX.2 [klein] (released 2026-01-15, 4B Apache-2.0, ~13 GB VRAM, FP8/NVFP4): https://bfl.ai/blog/flux2-klein-towards-interactive-visual-intelligence , https://huggingface.co/black-forest-labs/FLUX.2-klein-4B
- FLUX.2 [pro] per-MP pricing detail: https://fal.ai/models/fal-ai/flux-2-pro/edit
- Qwen-Image 3.0 closed weights (checked 2026-08-13); Qwen-Image 2.1 Apache-2.0 (2026-09-20); Qwen-Image-Edit-2511 (2025-12-23): https://freeimggen.com/blog/qwen-image-3-has-no-open-weights/ , https://apidog.com/blog/what-is-qwen-image-2-1/ , https://github.com/QwenLM/Qwen-Image
- SeedVR2 Apache-2.0, 3B/7B, ComfyUI VRAM tiers: https://huggingface.co/numz/SeedVR2_comfyUI , https://docs.comfy.org/tutorials/utility/seedvr2
- Magnific pricing / rebrand (2026): https://oakgen.ai/blog/magnific-ai-freepik-rebrand-pricing
- Retro Diffusion on Replicate (2025-11-19): https://replicate.com/blog/retro-diffusions-pixel-art-models-are-now-on-replicate
- OpenRouter provider routing `data_collection` / `zdr`: https://openrouter.ai/docs/guides/routing/provider-selection
- Third-party OpenRouter image MCPs: https://github.com/pinkpixel-dev/pixara-mcp , https://github.com/jtxmp/openrouter-image-gen-mcp , https://github.com/stabgan/openrouter-mcp-multimodal

---

## Fact-check (adversarial pass, 2026-10-04)

Method: for each claim that matters for a decision, I tried to **refute** it from a primary source. These were the live OpenRouter JSON (`GET /api/v1/images/models` and `/api/v1/images/models/{id}/endpoints`, re-pulled independently on 2026-10-04), vendor docs and model cards. Where only third-party pages or nothing at all was found, the verdict is *unverifiable*.

| # | Claim (from the candidates / report) | Verdict | Evidence |
|---|---|---|---|
| 1 | OpenRouter lists 57 image-output models. 55 accept image input (not `recraft-v4.1-flash` or `ming-image-0.1-design`). None is a dedicated SR/upscaler. 23 were added in the last ~3 months. FLUX Kontext is no longer listed. | **Confirmed** | Live `/api/v1/images/models`: count = 57. `input_modalities` lacks `image` only for those two ids. 23 models have `created` ≥ 2026-07-04. No id contains "kontext". |
| 2 | Images API = `POST /api/v1/images` with `input_references`, tiers 512/768/1K/1.5K/2K/4K, `b64_json` response, all-or-nothing billing. Exact pixel `size` is declared only by `inclusionai/ming-*`. | **Confirmed** | OR image-generation guide (fetched 2026-10-04): "A generation is either completed and billed in full, or it fails and is not billed". In the live catalogue `size` appears only in the two `ming` models. |
| 3 | Seed is declared only by FLUX.2 (pro/flex/max/klein), Seedream 4.5/5.0, Qwen Image 3 (/pro) and Krea 2. Gemini and OpenAI have no seed. | **Confirmed** | Live `supported_parameters`. Note also that `black-forest-labs/flux-3-image` (2026-10-01) has **no** seed. |
| 4 | NB2 `google/gemini-3.1-flash-image`: $60/M output tokens = 512 $0.045 / 1K $0.067 / 2K $0.101 / 4K $0.151; 0-14 refs; no seed. NBP `gemini-3-pro-image`: 1K/2K $0.134, 4K $0.24; the Vertex endpoint offers only 1K/2K. | **Confirmed** (one nuance) | Gemini pricing page "Last updated 2026-10-01" quotes exactly these per-image figures ($120/M for Pro). OR endpoints: `cost_usd 6e-05` / `0.00012` per output token, and NBP-Vertex `resolution` = `["1K","2K"]`. **Nuance:** OR allows 14 refs, but Google's image-generation doc splits them. For 3.1 Flash: "Up to 10 images of objects with high-fidelity", "Up to 4 ... characters", "Up to 3 images to be used as style references". Pro: "Up to 6 images of objects with high-fidelity". Keep golden style refs at ≤ 3. |
| 5 | Google shut down Gemini 2.5 Flash Image on 2026-10-02 (still listed on OR). | **Confirmed** (date) | The pricing page says it "is deprecated and will be shut down on October 2, 2026". The live OR endpoint list still shows a Google Vertex endpoint, so "do not use" stands. I did not observe a failing call myself. |
| 6 | FLUX.2 [klein] 4B: Apache-2.0, ~13 GB VRAM, FP8/NVFP4 variants, released 2026-01-15. On OR at $0.014/MP with seed, ≤ 4 refs, passthrough `steps` / `guidance` / `safety_tolerance`. | **Confirmed** | BFL blog (2026-01-15): "4B variants: Apache 2.0", "fits in ~13GB VRAM (RTX 3090/4070 and above)", FP8 and NVFP4 cut VRAM by "up to 40%" and "up to 55%". HF model card: Apache 2.0. OR endpoint: `megapixel 0.014`, `seed`, refs 0-4, `allowed_passthrough_parameters` = steps, guidance, safety_tolerance (the same for flux.2-pro and flex). **Caveat:** the **9B** klein variants are FLUX **Non-Commercial** License, not Apache. NVFP4 runs natively on Blackwell (5070 Ti), which helps. |
| 7 | Seedream: 4.5 $0.04 flat (1K/2K/4K, input free); 5.0 Flash $0.018 (1K/2K); **5.0 Pro $0.045 + $0.003/input image** (and $0.048 at 2K in the cost table). | **Partly refuted** (5.0 Pro at 2K) | The live OR endpoint for `seedream-5-0-pro` has **two** output prices: `0.045` and `0.09` with `"variant":"high_resolution"`. A third-party breakdown (AtlasCloud blog, 2026) puts the threshold at > 2.36 MP. A 1:1 2K output (2048², 4.2 MP) would then bill **$0.09**, so section 6 S2 for 5.0 Pro is about **$196, not $101** (S1 at 1K is unchanged). The 4.5 and 5.0 Flash figures are confirmed by OR endpoints (`0.04` / `0.018`, input `0`). |
| 8 | Qwen: Qwen-Image 3.0 is closed weights; the open siblings **Qwen-Image-Edit-2511 and Qwen-Image 2.1 are Apache-2.0** and run locally. | **Refuted for Qwen-Image 2.1**; Edit-2511 confirmed with a caveat; 3.0 closed is unverifiable (no primary source) | The HF model card `Qwen/Qwen-Image-2.1` shows **"Qwen Research License Agreement"** (7B), and HF discussions #6 and #9 complain that the licence is research-only. `Qwen/Qwen-Image-Edit-2511` card: "Apache 2.0", but it is **20B parameters**. The BF16 weights alone are ~40 GB (my arithmetic), so a 16 GB card needs GGUF Q4/Q5 or FP8 plus offload. "3.0 closed" rests only on third-party blogs (freeimggen, checked 2026-08-13), which report no HF or ModelScope repo. OR pricing for qwen-image-3 ($0.03 1K/2K + $0.003 input) and -pro ($0.04 / $0.075) is confirmed. |
| 9 | GPT Image 2 / 2.5 Sunburst are "useful for RLE sprites because [they support] a transparent background". Pricing $8/M in, $30/M out. No seed. "Precise element placement" is a known limitation. | **Refuted for gpt-image-2** (transparency); the rest is confirmed | The live OR endpoint for `openai/gpt-image-2` has `background` enum = `["auto","opaque"]`, with **no `transparent`**. `gpt-image-2.5-sunburst` / `-flare` have `["auto","transparent","opaque"]`. The OpenAI image guide documents `background: "transparent"` only for the 2.5 models, and community and API reports quote "Transparent background is not supported for this model" for gpt-image-2. OpenAI pricing: text input $5, image input $8, image output $30 per 1M tokens. The $5/M text-input line is missing from the report but is negligible. The guide says: "may have difficulty placing elements precisely in structured or layout-sensitive compositions". The 655,360 px minimum appears in the guide for the newer (2.5) models; for gpt-image-2 it is unverified on a primary page. |
| 10 | fal SeedVR2 seamless: $0.0025/MP; `upscale_factor` (float), `target_resolution` 720p-2160p, `seed`, `noise_scale`; Apache-2.0 weights 3B/7B; runs locally in ComfyUI "FP8 in 12-16 GB". | **Confirmed**, except VRAM detail **unverifiable** | fal page: "$0.0025 per megapixel", "upscale images, retaining seamless tiling". The API schema lists `upscale_mode` factor/target, `upscale_factor` default 2, `target_resolution` 720p/1080p/1440p/2160p, `seed`, `noise_scale` 0.1. HF `ByteDance-Seed/SeedVR2-3B`: Apache 2.0, one-step. The numz ComfyUI node (Apache-2.0, v2.5.24, 2025-12-24) gives only coarse tiers ("moderate (12-16GB)") achieved through BlockSwap, VAE tiling and GGUF, and mentions SageAttention 3 for Blackwell. I found no FP8-specific VRAM figure. Comfy docs list FP16 / FP8 e4m3fn / INT8 variants without numbers. |
| 11 | Magnific Creative Upscaler API: `scale_factor` 2/4/8/16, `optimized_for=videogame_assets`, creativity/resemblance/hdr/fractality −10..10, engines illusio/sharpy/sparkle, output ≤ 25.3 MP, priced per output area. | **Confirmed**, pricing **unverifiable** | docs.magnific.com: "2x, 4x, 8x, 16x"; `videogame_assets` is in the `optimized_for` enum; "Valid values range [-10, 10], default 0"; engines `automatic`, `magnific_illusio`, `magnific_sharpy`, `magnific_sparkle`; "can't exceed maximum allowed size of 25.3 million pixels". No price is on that page. |
| 12 | OpenRouter MCP: OAuth key expires after 7 days with a $10 default cap; `generate-image` is text-to-image, and reference-image input is undocumented. | **Confirmed** | OR MCP docs: "The key expires after 7 days", default spend cap "$10" (editable). `generate-image`: "Generate an image from a text prompt and get it back inline as an image content block". No input-image parameter is documented. |
| 13 | RTX 5070 Ti (Blackwell, sm_120) needs CUDA 12.8+ / PyTorch ≥ 2.7. | **Confirmed** | PyTorch 2.7 release blog (Apr 2025): Blackwell support with pre-built CUDA 12.8 wheels (`--index-url .../whl/cu128`). Older cu12.4/12.6 wheels lack sm_120 kernels. |

### Corrections to apply to the report body
- **Qwen-Image 2.1 is not Apache-2.0.** It uses the *Qwen Research License*. This affects TL;DR §6, §3, §4 and §7: "Qwen-Image-Edit-2511 / Qwen-Image 2.1". It is probably acceptable for a private, non-commercial experiment, but it is not a clean choice for a distributable Exult mod pack. Prefer **Qwen-Image-Edit-2511 (Apache-2.0, 20B, needs GGUF/FP8 plus offload on 16 GB)** or **FLUX.2 klein 4B (Apache-2.0)**, and avoid the klein 9B variants (non-commercial).
- **Transparent background is only on `gpt-image-2.5-sunburst` / `-flare`, not on `gpt-image-2`.** Change §3 and the §5 "RLE sprite transparency" row. The local alpha-from-source-mask approach stays mandatory regardless.
- **Seedream 5.0 Pro costs $0.09 per image at a 1:1 2K output**, not $0.045. In §6, S2 rises to about $196 and S3 to about $6. Its 1K figures are unchanged.
- **Gemini reference budget.** The 14 refs are not all "high-fidelity". Google's docs say 3.1 Flash takes ≤ 10 object refs plus ≤ 4 character refs and ≤ 3 *style* refs, and Pro takes ≤ 6 high-fidelity object refs. The style-reference plan (1-3 golden chunks) fits within these limits.
- **SeedVR2 local VRAM.** Restate as "fits 12-16 GB with BlockSwap / VAE tiling / GGUF (numz node v2.5.24)". There is no primary source for an FP8-specific number.

### Fact-check sources (all fetched 2026-10-04)
- OpenRouter live catalogue and endpoints: https://openrouter.ai/api/v1/images/models , https://openrouter.ai/api/v1/images/models/{id}/endpoints (gemini-3.1-flash-image, gemini-3-pro-image, gemini-2.5-flash-image, flux.2-klein-4b/pro/flex/max, seedream-4.5/5-0-flash/5-0-pro, qwen-image-3(/pro), gpt-image-2, gpt-image-2.5-sunburst/flare)
- OpenRouter Images API guide: https://openrouter.ai/docs/guides/overview/multimodal/image-generation ; MCP docs: https://openrouter.ai/docs/guides/overview/mcp-server
- Gemini pricing (last updated 2026-10-01): https://ai.google.dev/gemini-api/docs/pricing ; Gemini image generation: https://ai.google.dev/gemini-api/docs/image-generation
- BFL FLUX.2 [klein] blog (2026-01-15): https://bfl.ai/blog/flux2-klein-towards-interactive-visual-intelligence ; HF: https://huggingface.co/black-forest-labs/FLUX.2-klein-4B ; BFL FLUX.2 [pro] API reference (width/height ≥ 64, `disable_pup`, `input_image`..`input_image_8`): https://docs.bfl.ai/api-reference/models/generate-or-edit-an-image-with-flux2-%5Bpro%5D
- Seedream 5.0 Pro high-resolution threshold (third-party, 2026): https://www.atlascloud.ai/blog/ai-updates/seedream-5-0-pro-price
- Qwen: https://huggingface.co/Qwen/Qwen-Image-2.1 (Qwen Research License), https://huggingface.co/Qwen/Qwen-Image-2.1/discussions/6 , https://huggingface.co/Qwen/Qwen-Image-Edit-2511 (Apache 2.0, 20B), https://freeimggen.com/blog/qwen-image-3-has-no-open-weights/ (third-party)
- OpenAI image guide: https://developers.openai.com/api/docs/guides/image-generation ; pricing: https://developers.openai.com/api/docs/pricing ; gpt-image-2 transparency reports: https://community.openai.com/t/is-transparent-background-support-broken-in-gpt-image-2/1402707 , https://help.apiyi.com/en/gpt-image-2-transparent-background-not-supported-en.html
- fal SeedVR2 seamless: https://fal.ai/models/fal-ai/seedvr/upscale/image/seamless (+ `/api`) ; HF: https://huggingface.co/ByteDance-Seed/SeedVR2-3B ; ComfyUI node: https://github.com/numz/ComfyUI-SeedVR2_VideoUpscaler ; Comfy docs: https://docs.comfy.org/tutorials/utility/seedvr2
- Magnific API: https://docs.magnific.com/api-reference/image-upscaler-creative/post-image-upscaler
- PyTorch 2.7 (Blackwell / cu128): https://pytorch.org/blog/pytorch-2-7/
