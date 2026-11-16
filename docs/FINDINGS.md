# StrayDLSS: findings

Game: Stray, Steam build 12863654, `Stray-Win64-Shipping.exe`, UE **4.27**, project Hk_project,
DirectX 12 (launch option `-dx12`). The exe's code is encrypted on disk by SteamStub, so
everything is found at run time. Engine facts (cvar layout, ray tracing costs, presets) are in
StrayTweaks' `docs/FINDINGS.md`. The game has no upscaler of its own: UE 4 TAA only, at 100%
screen percentage.

Forked 2026-11-02 from TrueColorsDLSS 1.1.0 (UE 4.25: DLSS and FSR in place of the TAA, DLSS-G
and FSR frame generation; its FINDINGS are the reference for the method). Changes for Stray:

- `cvars.hpp` from StrayTweaks (run-time singleton scan, UE 4.27 layout check).
- The TAA is found by its declared registers and binding shapes (True Colors' shader id
  `cs4a48869b` won't match). Velocity may be rg16unorm (4.25) or rgba16unorm (4.26 in The
  Quarry); both are accepted.
- Streamline is told engine 4.27.
- The View buffer offsets are still UE 4.25's (ClipToPrevClip 1952, jitter 2016, view size
  2080); the per-frame layout check tells whether they hold in 4.27.

## To find out (0.0.1)

- Is the TAA taken (log: shader id, first frame evaluated)? If not: F10 capture.
- Do the View offsets hold, or does the layout check refuse?
- DLAA look, then lower modes (TAAU permutation), then frame generation.

## 0.0.1 test (2026-11-02, 3840x2160, DirectX 12, StrayTweaks + RenoDX loaded)

- The TAA was found by its shape: **`cs37876d91`**, colour 3840x2160 -> output 3840x2160. The game
  already runs with `r.TemporalAA.Upsampling=1` (set by 0x03) at `r.ScreenPercentage` 100, so this is
  the TAAU permutation; lower DLSS modes should use the same shader.
- **UE 4.25's View offsets hold in 4.27** (ClipToPrevClip 1952, jitter 2016, view size 2080): the
  layout check passed and every gameplay frame went through DLSS (DLAA, preset K): "upscaled 120,
  tagged 120" per diag window, nothing failed.
- Camera cuts bind velocity format 90 (UE's 1x1 placeholder): those frames keep the game's TAA, as
  in True Colors.
- r.BasePassOutputsVelocity=1, but the velocity buffer still passed (format accepted).
- Clean exit; both add-ons unloaded. (The crash report in Saved\Crashes is from 08:51, an earlier
  session.)

0.1.0: `kTaaMain` = `cs37876d91`.

## 0.1.0 test (2026-11-02): FSR, lower modes, FSR frame generation

- Started with FSR upscaling + **FSR frame generation 2x**, Balanced (58%): swapchain created through
  FSR, colour space 3 (HDR10, RenoDX), FSR FG context HDR, View buffer check passed (projection
  0.75 / 1.3333, near 1, infinite far). Ran without failures; paused/resumed around camera cuts.
  No pre-HUD picture ("HUD not told apart"), as in True Colors.
- Below 100% the TAAU is another permutation: **`csed0bf55b`** (colour 2228x1256 texture, view
  2228x1253 -> 3840x2160), found by shape.
- Switched live to **DLSS** (Balanced, created as NGX "Quality" mode, preset K): ran.
- One frame logged "TAA view: layout NOT as expected" (right after the resolution change); the
  game's TAA took that frame. Harmless.
- DLSS frame generation was chosen in the overlay (needs a restart: the swapchain is made at
  launch); the user quit; FSR FG shut down in order and the game exited cleanly.

## 0.1.0 test 2 (2026-11-02): DLSS frame generation

- **DLSS-G Dynamic** with DLSS Balanced: Streamline 2.14 started for the game's swapchain, View
  check passed, first frame tagged at submission; Dynamic presented 1 to 6 frames per game frame,
  Reflex available. Streamline's "failed to find global tag Depth / MotionVectors" appear only on
  camera-cut frames (game TAA keeps those, nothing tagged), as in True Colors. Streamline shut down
  in order; clean exit.
- User: **DLSS and FSR "both look good"** (upscaling and frame generation).

## 1.1.0: DLSS Ray Reconstruction (2026-11-09)

Ray Reconstruction (NGX DLSSD, `nvngx_dlssd.dll` 310.9.16 from Streamline 2.14.16's bin/x64)
replaces Super Resolution at the same TAA dispatch when switched on.

- **G-buffer**: found when the base pass binds its render targets (7 here): RT0 HDR scene colour,
  RT1 GBufferA format 24 (rgb10a2: world normal * 0.5 + 0.5), RT2 GBufferB format 87 (bgra8:
  metallic, specular, roughness, shading model id in the low 4 bits of a), RT3 GBufferC format 91
  (bgra8 sRGB: base colour). Remembered by size (2228x1256 at Balanced); UE's render target pool
  hands out different textures from frame to frame, so the latest set is used. Read as shader
  resources at the TAA (no barriers needed in testing).
- **Guides** (`src/guide.hlsl`): diffuse albedo = base colour * (1 - metallic); specular albedo =
  UE's EnvBRDFApprox(lerp(0.08 * specular, base colour, metallic), roughness, N.V); world normal
  with roughness in w (packed mode). Unlit pixels (sky): diffuse 1, specular 0, normal facing the
  camera.
- **Matrices**: View.TranslatedWorldToView at byte 192 (a pure rotation: the camera sits at the
  translated origin) and View.ViewToClipNoAA at 512 (reversed-Z infinite, near 1). Both checked
  every frame (orthonormal rows, no translation; same projection as ViewToClip at 448 minus jitter).
- **Denoisers** (names from the exe): r.Shadow.Denoiser 2, r.Reflections.Denoiser 2,
  r.AmbientOcclusion.Denoiser 2, r.DiffuseIndirect.Denoiser 1, r.RayTracing.GlobalIllumination.Denoiser 1,
  r.RayTracing.SkyLight.Denoiser 1 (game values) set to 0 on the game thread while RR ran in the
  last 120 presents, values and priorities restored after.
- NGX keeps the matrix pointers in its shared parameter block: Super Resolution then read stale
  ones ("WorldToScreenMatrix not invertible", hundreds of lines) after RR was switched off. Fixed:
  RR's matrices live in static storage, and they're cleared before each Super Resolution frame.
- The NVIDIA App's DLSS override (Model Presets) wins over the RR preset hint: the user's forces F.

Test (2026-11-09, 4K HDR, Balanced, RTX 5080, StrayTweaks RT reflections/shadows/AO on): RR ran on
every gameplay frame with DLSS Frame Generation 2x; denoisers off/on with the toggle; guide views
shown; user: looks good. In the main menu no G-buffer set matched (Super Resolution there; RR starts
at the first level load); unmatched 4+ target sets are now logged to find out why.

## DirectX 11 (branch dx11, started 2026-11-16)

Stray runs DirectX 11 unless started with `-dx12`. Plan: the same add-on works on both; DX11 gets
DLSS (NGX D3D11, as in RE2DLSS-DX11) and FSR (through a D3D12 bridge), no frame generation and no
Ray Reconstruction (no ray tracing on DX11).

- 1.2.0-dx11probe: main.cpp reads the command line at load; without `-dx12`/`-d3d12` only
  `src/probe11.cpp` (RE2DLSS-DX11 0.0.1's D3D11 capture, per-context binding tracking, deferred
  command lists) is registered, none of the DX12 code. F10 writes `StrayDLSS-DX11-frame-N.txt`.
  Every compute shader declaring exactly t0-t5 + u0 (the DX12 TAA's shape) is logged the first
  time it's dispatched. Not yet run.

### DX11 probe run 1 (2026-11-16, 3840x2160, HDR back buffer rgb10a2, no deferred contexts)

- **Same TAA shaders as DX12, same bytecode**: `cs37876d91` at 100% (captures 1), `csed0bf55b` at
  50% (capture 2, 1920x1080 -> 3840x2160), dispatched 480 x 270 (8x8 groups over the output).
- Bindings (same registers as DX12): t0 exposure 1x1 rgba32f, t1 scene colour **r11g11b10f**
  (render size), t2 depth d32s8 as r32f_x8x24, t3 velocity rgba16unorm, t4 stencil (same
  texture), t5 history 3840x2160 rgba16f, u0 output 3840x2160 rgba16f (history ping-pong with t5).
  b0 4096-byte $Globals (TAA parameters), b1 8192-byte buffer = View uniform buffer (shared by most
  passes); b2-b4 small. t6+ are leftovers from earlier passes (not declared).
- Next pass reads u0: `ps740c24b1` (bloom/post setup, writes 1920x1080 r11g11b10f).
- Base pass MRT (7 RTs, render size): r11g11b10f scene colour, rgb10a2 GBufferA, bgra8 GBufferB,
  bgra8 sRGB GBufferC, rgba16unorm velocity, 2x bgra8; depth d32s8.
- Stray's DX11 shaders have **no RDEF reflection** (stripped): the probe's register list came up
  empty, so the TAA-shape log never fired. Fixed: probe11 reads declarations from the code
  (dxbc.hpp), as the DX12 path does (built, not yet re-run).
- Plan for 0.x DX11: hook `cs37876d91`/`csed0bf55b` dispatch on the immediate context; DLSS via
  NGX D3D11 into u0 (RE2DLSS-DX11's clean ID3DDeviceContextState swap), jitter/matrices from the
  View buffer's CPU writes at the DX12 offsets (1952/2016/2080), then FSR through the D3D12 bridge.

### 1.2.0-dx11a: DLSS on DX11 (2026-11-16, built, not yet run)

- `src/upscaler11.cpp`: tracks the compute stage's t0-t5, u0, b0-b4 per context and the first
  2112 bytes of every constant buffer of 2112 B-64 KB as the CPU writes them (init data,
  Map/Unmap, UpdateSubresource). At `cs37876d91`/`csed0bf55b` (or another shader declaring exactly
  t0-t5 + u0 whose bindings match): View buffer = the bound cbuffer passing the DX12 layout check
  (logged slot), render size/jitter/ClipToPrevClip at the DX12 offsets; `motion11.hlsl` (cs_5_0:
  motion, mask, r32f depth copy) and DLSS (`dlss11.cpp`, RE2DLSS-DX11's NGX D3D11 wrapper, auto
  exposure) each in a clean ID3DDeviceContextState; the game's dispatch skipped.
- DLSS mode via upscaler.cpp's game-thread cvar code (`rd::ResolutionTick`); shares the ini keys
  Enabled, DlssMode, DlssModel with DX12. F9 toggles.
- The D3D11 capture probe is now opt-in: ReShade.ini `[StrayDLSS] Dx11Capture=1`.

### 1.2.0-dx11a crash, dx11b fix (2026-11-16)

- dx11a: DLSS ran (View buffer found at **b1**, NGX D3D11 ready, DLAA 3840x2160 first frame
  evaluated), then the game died ~0.3 s later, twice: fail-fast 0xc0000409 (code 7, abort) in
  ucrtbase, no UE crash report. Minidump (heuristic stack, %LOCALAPPDATA%\CrashDumps): game ->
  d3d11 -> nvwgf2umx -> d3d11 C++ exception unwinding -> abort, i.e. D3D11 threw inside one of the
  game's own calls. Suspected cause: the ID3DDeviceContextState swap around our work (from
  RE2DLSS-DX11); UE 4's D3D11 RHI keeps queries open across the frame.
- dx11b: no context state swap. The compute stage (shader, cbuffers with offsets, 128 SRVs, 64
  UAVs, samplers) is saved before motion11 + DLSS and put back after. Not yet run.

### dx11b crash: the real cause, dx11c fix (2026-11-16)

- dx11b got through the switch to Balanced (feature 2228x1253 -> 3840x2160) and died ~19 s later
  while the menu loaded, same abort. A real stack walk of the minidump (`tools/mdunwind.py`:
  .pdata unwind from the module files, names from our PDB) showed:
  dlss11::Evaluate -> NVSDK_NGX_D3D11_EvaluateFeature -> NGX model DLL -> d3d11 -> ReShade
  (dxgi.dll) -> **our event handler -> std::_Throw_Cpp_error** -> terminate -> abort.
  NGX's D3D11 calls come back through ReShade into upscaler11's handlers on the same thread, and
  `std::mutex` was already held by the TAA hook ("resource deadlock would occur"). dx11a's crash
  was almost certainly the same; the context state swap was a wrong guess (removed anyway).
- dx11c: `std::recursive_mutex`, and a thread-local guard so bind/push/dispatch events from NGX's
  own work don't overwrite the tracked game bindings or re-enter the TAA hook.
- Usage: `python -I tools/mdunwind.py <dump> E:\StrayDLSS\bin\obj` (dumps in %LOCALAPPDATA%\CrashDumps).

### dx11c test (2026-11-16): works

- No crash. View buffer at b1; DLAA first frame, then Balanced 2228x1253, Performance 1920x1080
  (preset M by Automatic), Quality 2561x1441, DLAA again, all switched live from the overlay.
- 5011 frames upscaled, 6 kept by the game's TAA (camera cuts / loading; one frame where the
  View buffer didn't pass the layout check). User: "appeared to work great". No new crash dump.
- Not yet tested on DX11: F9 toggle, motion quality checked closely, DX12 regression after the
  ResolutionTick refactor.

## 1.2.0 DX12 check (2026-11-16) and 1.2.1

- DX12 with 1.2.0 (`-dx12`, DLSS-G 2x, Ray Reconstruction on): DLAA SR, then RR from the first
  level (denoisers off), Quality, Balanced and Performance switched live; DLSS mode cvars fine
  after the ResolutionTick refactor; Streamline shut down cleanly; user: "looks good".
- **Bug (since 1.1.0)**: our render-size textures (motion, mask, 3 RR guides = 5 per size with RR)
  were never freed and capped at 16. After DLAA -> Quality -> Balanced -> Performance the 17th was
  refused: "motion: no texture (limit)" on every frame, the game's TAA took over, frame generation
  paused.
- 1.2.1: textures unused for 8 presented frames are freed (with their views) when room is needed;
  limit 32. Installed, not yet run.
