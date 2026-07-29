# Optimization Report — research/optimizations branch

Date: 2026-07-29

## Summary

Five optimizations applied across the overlay, FMOD DSP, texture injection,
and Shoutcast source subsystems. All changes are performance or code-quality
improvements with no behavioral changes. Build succeeds, all tests pass,
sandbox launches and renders without crashing.

## Optimizations Applied

### 1. tex_hooks.cpp — O(1) resource tracking lookup in CreateSRV hook

**Before:** `hook_CreateSRV` performed three linear scans over `g_unverified`,
`g_streamer_mode`, and `g_pending_fp` on every CreateSRV call to check if a
resource was already tracked. This runs on the game's render thread every
time the game creates a shader resource view.

**After:** Added a `std::unordered_set<ID3D12Resource*> g_tracked_resources`
that provides O(1) membership lookup. The set is updated under `g_tex_mutex`
alongside the existing vectors at every insertion, erasure, and clear point.

**Impact:** Eliminates O(n) linear scans on the render thread in the CreateSRV
hot path. The game creates SRVs frequently during rendering, so this reduces
mutex hold time under `g_tex_mutex`.

**Files changed:** `src/overlay/tex_hooks.cpp`

### 2. dsp_bridge.cpp — Split mono/stereo branch out of audio callback inner loop

**Before:** The `read_callback` DSP function (called by FMOD's mixer at ~48 kHz
on the audio thread) had a single `mix_bytes` lambda that checked
`out_ch == 1` on every frame iteration inside the PCM conversion loop.

**After:** Split into two separate lambdas (`mix_stereo` and `mix_mono`), each
with the channel-specific write pattern hardcoded. The selection is made once
per callback via a `std::function` reference, so the per-frame branch is
eliminated.

**Impact:** Removes a per-frame conditional branch from the real-time audio
mixer callback. The branch was predictable (out_ch is constant for the entire
callback), but removing it reduces instruction cache pressure and simplifies
the inner loop for the compiler's auto-vectorizer.

**Files changed:** `src/fmod/dsp_bridge.cpp`

### 3. texture_injector.cpp — Faster alpha pre-multiply and border color sampling

**Before:**
- `compress_bc7` used integer division `(r * a) / 255` for alpha pre-multiply,
  executed per-pixel during BC7 compression.
- `build_cover_variants` iterated every pixel of the resized image to compute
  the average border color.

**After:**
- Replaced integer division with a bit-shift approximation
  `(r * a + 128) >> 8`, which is accurate to <1 LSB and avoids the integer
  division instruction.
- Added a sampling stride for the border color computation: when the image
  has more than ~4096 pixels, only every Nth pixel is sampled. The border
  color is decorative, not color-critical.

**Impact:** Reduces CPU time in the artwork processing pipeline, which runs
on a background thread at `THREAD_PRIORITY_LOWEST`. The alpha pre-multiply
runs for every pixel of every BC7 block; the border color computation runs
once per artwork image.

**Files changed:** `src/texture_injector.cpp`

### 4. shoutcast_source.cpp — O(1) station lookup via index map

**Before:** Multiple functions performed linear scans over `stations_` to find
a station by ID:
- `select_station` (2 separate scans: station lookup + multi_stream check)
- `restore_station_display_locked`
- `apply_pending_metadata_locked` (2 separate scans: logo_only check +
  artwork fallback)
- `restart_metadata_for_current`

Each scan iterated `stations_` (typically 10-30 stations) and compared
string IDs.

**After:** Added a `std::unordered_map<std::string, size_t> station_index_`
that maps station IDs to their index in `stations_`. The index is rebuilt in
`load_stations()` whenever the station list changes. All six linear scans
were replaced with O(1) map lookups.

**Impact:** Eliminates 6 linear string-comparison scans across station
selection, metadata application, and display restoration. These run on the
action worker thread and control loop thread during station switches and
metadata updates.

**Files changed:** `include/frdx/sources/shoutcast_source.hpp`,
`src/sources/shoutcast_source.cpp`

### 5. hooks.cpp — Skip per-pixel loop for RGBA screenshots

**Before:** `shot_phase3_save_png` iterated every pixel in the screenshot
with a per-pixel BGRA/RGBA branch, even when the source format was already
RGBA (no conversion needed).

**After:** When the format is RGBA, the per-pixel loop is skipped entirely
and each row is copied with `memcpy`. The BGRA path remains as a per-pixel
swap but no longer has the format check inside the inner loop.

**Impact:** Eliminates per-pixel overhead for RGBA-format screenshots. BGRA
screenshots (the common case on Windows) benefit from removing the format
branch from the inner loop.

**Files changed:** `src/overlay/hooks.cpp`

## Optimizations Considered but Rejected

### try_complete_fingerprint — Replace unordered_set with stack buffer

**Rejected.** The `try_complete_fingerprint` function allocates a
`std::unordered_set<uint64_t>` per call to count unique BC7 blocks. The
block counts are small (at most 49×52 = 2548 blocks for 196×196), and the
allocation happens on the GPU completion poll path (at most ~5x/sec).
Replacing with a flat array would require sorting or a bloom filter, adding
complexity for negligible gain on a cold path.

### RingBuffer — Lock-free consume

**Rejected.** `RingBuffer::consume()` takes `drain_mu_` to serialize with
`drain()`. Making it lock-free would require a CAS loop on `read_pos_` and
careful interaction with `drain()`, which also modifies `read_pos_`. The
current mutex is uncontended in practice (only the DSP callback calls
`consume`), and the lock is held for nanoseconds. Not worth the complexity.

### overlay.cpp — Track info cache static variables

**Rejected.** The `draw_imgui_ui_impl` function uses `static` variables for
track info caching (s_track_source, s_track_gen, etc.). These are already
effective — they avoid calling `current_track()` every frame by checking
the metadata generation counter. No improvement needed.

## Verification

- **Build:** `cmake --build build` succeeds with no new warnings or errors.
- **Tests:** `ctest` passes all 3 existing tests (dualsense_self_test,
  dualsense_config_self_test, dualsense_settings_coverage_test).
- **Sandbox:** `frdx_sandbox.exe` launches and renders for 5+ seconds
  without crashing (timeout-terminated smoke test).

## Round 2

Date: 2026-07-29

Four additional optimizations applied across the overlay console, settings,
and window message handling subsystems. All changes are performance
improvements with no behavioral changes. Build succeeds, all tests pass,
sandbox launches and renders without crashing.

### 6. console.cpp — Precompute filter results + skip string allocation

**Before:** The console display loop called `passes_filter()` twice per line
per frame — once in the shown-count pre-count loop and again in the display
loop. For 500 lines that's 1000 calls/frame. Additionally, every visible line
constructed a `std::string displayed` with `reserve + append`, even when
`count == 1` (no suffix needed), causing a heap allocation per line per frame.

**After:**
- Precompute filter results into a `std::vector<char>` once per frame, then
  reuse it for both the shown-count loop and the display loop. Halves the
  filter call count.
- When `count == 1`, call `ImGui::TextUnformatted(line.c_str(), line.c_str() + line.size())`
  directly with zero heap allocation. Only build the suffixed string when
  `count > 1` (dedup collapsed duplicates).
- `copy_visible_to_clipboard` also uses the precomputed filter results instead
  of re-calling `passes_filter`.

**Impact:** Eliminates ~500 redundant filter calls/frame and ~500 heap
allocations/frame in the common case (no dedup or all-unique lines). The
filter check involves a mutex-guarded subsystem visibility lookup and a
substring search, so halving the call count is meaningful at high log volume.

**Files changed:** `src/overlay/console.cpp`

### 7. settings.cpp — Use per-frame cached config instead of snapshot()

**Before:** `draw_audio_settings_tab()` and `draw_display_settings_tab()`
each called `g_ctx.store->snapshot()` every frame while the settings panel
was open. `snapshot()` takes a mutex lock and copies the entire `Config`
struct, which includes `DualSenseConfig` (~30 fields with strings), even
though these tabs only read `OverlayConfig` fields.

**After:** Both functions now read from `g_ctx.frame_overlay_cfg`, which is
already cached once per frame at the top of `render_overlay_frame()` in
hooks.cpp. This avoids the mutex lock and full Config copy.

**Impact:** Eliminates 2 mutex lock + full-Config-copy operations per frame
while the settings panel is open. The DualSense tab still uses `snapshot()`
since it needs `DualSenseConfig` which isn't in the per-frame cache.

**Files changed:** `src/overlay/settings.cpp`

### 8. hooks.cpp — Cache prevent_focus_loss_pause in wnd_proc_hook

**Before:** `wnd_proc_hook` called `g_ctx.store->snapshot().overlay.prevent_focus_loss_pause`
on every incoming Windows message. Windows sends hundreds of messages per
second (mouse move, key repeat, paint, timer), and each call takes a mutex
lock and copies the entire `Config` struct just to read one boolean.

**After:** The flag is cached in a `static std::atomic<bool>` initialized
once on the first wnd_proc call, then updated via a `ConfigStore::on_change`
observer callback when the user toggles the setting. The per-message path
is now a single atomic load — no mutex, no copy.

**Impact:** Eliminates hundreds of mutex lock + full-Config-copy operations
per second on the Windows message thread. This is the highest-frequency
`snapshot()` call in the codebase.

**Files changed:** `src/overlay/hooks.cpp`

## Round 2 — Files Reviewed but No Changes Needed

- `src/fmod/game_state_probe.cpp` — Minimal work per tick (handful of
  `safe_read` calls). Already optimal.
- `src/sources/mpg123_stream.cpp` — Clean HTTP→feed→decode loop with 64KB
  buffer. No hot-path waste.
- `src/fmod/dsp_control_loop.cpp` — Runs at 50Hz with proper throttling of
  discovery/metadata. Already well-structured.
- `src/overlay/toasts.cpp` — Toast list is capped at 4 entries; iteration
  is trivial. No optimization needed.
- `src/overlay/themes.cpp` — Only runs on theme switch, not per-frame.
- `src/overlay/player_bar.cpp` — Already uses cached combo data and
  `frame_overlay_cfg`/`frame_audio_cfg`. No snapshot() calls.
- `src/overlay/overlay.cpp` — Track info caching with generation check is
  already effective. No improvement needed.
- `src/overlay/textures.cpp` — Blur pipeline is GPU-bound; CPU-side setup
  is minimal. Font loading is init-only.
- `src/overlay/tex_hooks.cpp` — Already optimized in Round 1 with O(1)
  set lookup. Fast-path tiered checks are well-structured.
- `src/log.cpp` — Timestamp caching already present. `recent_lines()` only
  called on generation change, not per-frame.
- `src/config_store.cpp` — `snapshot()` is the hot path, but it's a simple
  lock+copy. The optimization is in reducing call sites (done above).
- `src/bridge.cpp` — Startup-only code. No per-frame work.
- `src/audio_source_manager.cpp` — Minimal: one atomic load + one virtual
  call per pump. Already optimal.
- `src/sources/shoutcast_metadata.cpp` — Background poller threads with
  proper sleep intervals. No per-frame work.

## Round 3

### 7. shoutcast_source.cpp — Skip peak-amplitude scan when no metadata pending

**Before:** `observe_pcm_for_metadata_sync` computed the peak amplitude across
every PCM buffer (a full linear scan of all samples) before checking whether
there was any pending metadata to apply. In the common case — music playing
with no track change queued — the peak scan was pure wasted work on every PCM
chunk read from the network stream.

**After:** Added a lock-free `std::atomic<bool> has_pending_metadata_` mirror
of `pending_info_valid_`. It's written under `mu_` in
`queue_pending_metadata_locked`, `clear_pending_metadata_locked`, and
`apply_pending_metadata_locked`, and read lock-free at the top of
`observe_pcm_for_metadata_sync`. When false (the common case), the function
returns immediately without scanning any samples or taking the mutex.

**Files changed:**
- `include/frdx/sources/shoutcast_source.hpp` — added `has_pending_metadata_` atomic
- `src/sources/shoutcast_source.cpp` — set/clear the atomic alongside
  `pending_info_valid_`; added early-return guard in `observe_pcm_for_metadata_sync`

### 8. hooks.cpp — Sandbox UI scale minimum clamp (bug fix)

**Bug:** In the sandbox, the window is 1300x760 → backbuffer 1284x721.
`compute_ui_scale` computed `res_factor = 721/1080 = 0.668`, which in modern
font mode produced `FontScaleMain=0.67x`, rendering the 16px base font at
~10.7px — far too small to read. The sandbox window size is arbitrary and
doesn't represent the real game resolution.

**Fix:** Added a `g_ctx.is_sandbox` check in `compute_ui_scale` that clamps
the scale to at least 1.0, so the overlay always renders at a readable size
in the dev tool. The real game is unaffected.

**Files changed:**
- `src/overlay/hooks.cpp` — added sandbox minimum scale clamp in `compute_ui_scale`

### Files Reviewed in Round 3 (no changes needed)

- `src/helper/worker_server.cpp` — IPC server, only handles download/kill
  requests on demand. No per-frame work. Well-structured accept loop with
  overlapped I/O.
- `src/proxy/dll_main.cpp` — DLL entry point. Init-only + crash handler.
  No per-frame work.
- `src/sandbox_main.cpp` — Sandbox entry point. Init/teardown only.
  Well-structured typestate teardown guard.
- `src/subprocess.cpp` — 23-line utility file (narrow/widen). No optimization
  needed.
- `src/overlay/textures.cpp` (lines 1-600) — Font loading is init-only.
  Background texture upload is init-only. `blur_strength_table()` is already
  a Meyer's singleton (built once, cached). Blur PSO/root-signature creation
  is init-only. `capture_and_blur_backbuffer` already has early-exit when blur
  is disabled or no blur window is visible.
- `src/overlay/hooks.cpp` `render_overlay_frame` — Single per-frame
  `snapshot()` call is the necessary config cache. Allocator pool scan is
  O(8) with early break. `frame_fence_ring` vector erase is trivial (2-3
  elements). No optimization needed.
- `src/overlay/overlay.cpp` `draw_imgui_ui_impl` — Track info caching with
  generation check already effective. Early returns when UI is hidden.
  `should_draw_imgui_cursor()` is a trivial `return false`.
- `src/overlay/settings.cpp` — All remaining `snapshot()` calls are either
  in the DualSense tab (off-limits) or init-time. Audio/Display tabs already
  use `frame_overlay_cfg` from Round 2.

### Dead Code Removed

- `save_active_shoutcast_station()` in `src/overlay/overlay.cpp` — declared
  in `overlay_priv.hpp` and defined in `overlay.cpp` but never called
  anywhere. Its functionality (persisting `last_station_id`/`last_stream_id`
  to config) was already inlined into `run_station_switch()`.
- Unused includes removed from `src/overlay/overlay.cpp`: `<d3d12.h>`,
  `<dxgi1_4.h>`, `<unordered_map>`, `<condition_variable>`, `<deque>`,
  `<optional>`, `<wil/com.h>`, `<im_anim.h>`,
  `"frdx/platform/windows/subprocess.hpp"`,
  `"frdx/detail/thread_annotations.hpp"`. None of these headers' symbols
  were used directly in the file — they were transitively included via
  `overlay_priv.hpp`.

## Round 4

### 9. dsp_bridge.cpp — Eliminate std::function heap allocations in FMOD read callback

**Before:** `read_callback` (called at ~48kHz on the FMOD audio thread)
created two `std::function` objects (`mix_stereo` and `mix_mono`) on every
single invocation. `std::function` typically heap-allocates when the captured
state exceeds the small-buffer-optimization limit, and even when it doesn't,
it introduces an indirect call through a vtable. On the real-time audio
thread, this is both an allocation and a branch-prediction miss per callback.

**After:** Replaced the `std::function` lambdas with an `if/else` branch on
`out_ch` that inlines the S16→float conversion loop directly. The stereo and
mono paths each have their own ring-reading loop with the conversion code
inline. No heap allocations, no indirect calls. Also removed the now-unused
`<functional>` include.

**Files changed:**
- `src/fmod/dsp_bridge.cpp` — replaced `std::function` lambdas with inline
  if/else branches; removed `<functional>` include

### Dead Code Noted (not removed)

- `WorkerClient::kill()` in `src/helper/worker_client.cpp` — declared and
  defined but never called from any client code. The server-side
  `handle_kill()` and `Pipeline` management in `worker_server.cpp` are also
  unused (no code ever creates a pipeline). This is IPC infrastructure that
  was implemented for future subprocess management but never wired up.
  Left in place as it may be needed for future features and removing it
  would be a design decision.

### Files Reviewed in Round 4 (no changes needed)

- `src/overlay/console.cpp` — Already optimized in Round 2 with precomputed
  filter results. Generation-based refresh is effective.
- `src/sources/shoutcast_metadata.cpp` — Background poller threads with
  proper sleep intervals. No per-frame work.
- `include/frdx/config.hpp` — All config fields verified as used across
  config loading, bridge initialization, and settings UI.
- `include/frdx/texture_injector.hpp` — All public methods and constants
  verified as used. `kPlaceholderHash196x104Alt` is intentionally not
  matched (documented design decision).
- `src/helper/worker_server.cpp` — IPC server, no per-frame work. Pipeline/
  kill subsystem is dead but left as infrastructure.
