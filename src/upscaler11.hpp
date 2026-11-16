// DLSS on Stray's DirectX 11 renderer (upscaler11.cpp).
#pragma once

#include <string>

namespace upscaler11 {

void Register();
void DrawOverlay();
// One line for the top of the overlay; *warn when it's a problem.
std::string Headline(bool* warn);

}  // namespace upscaler11
