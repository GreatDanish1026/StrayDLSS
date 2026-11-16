// Stray's DirectX 11 renderer: the frame capture probe (probe11.cpp).
#pragma once

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace probe11 {

// Registers the D3D11 capture events (instead of the DX12 upscaler and frame generation).
void Register(HMODULE module);
void Unregister();
// The overlay in DirectX 11 mode.
void DrawOverlay();

}  // namespace probe11
