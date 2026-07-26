#pragma once
#include <cstdint>

// ============================================================
// Color — minimal RGBA colour for the simulation core.
//
// Replaces sf::Color. The int-taking constructor mirrors SFML's, so
// expressions like Color(100 + (hash & 0x7F), ...) keep working without
// narrowing-conversion errors. The SFML renderer converts this to
// sf::Color at the boundary.
// ============================================================
struct Color {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;

    Color() = default;
    Color(int r_, int g_, int b_, int a_ = 255)
        : r(static_cast<uint8_t>(r_)),
          g(static_cast<uint8_t>(g_)),
          b(static_cast<uint8_t>(b_)),
          a(static_cast<uint8_t>(a_)) {}
};
