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
