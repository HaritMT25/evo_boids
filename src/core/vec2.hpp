#pragma once
#include <cmath>

// ============================================================
// Vec2 — minimal 2D float vector for the simulation core.
//
// Replaces sf::Vector2f so that nothing under src/core depends on SFML.
// It is an aggregate, so all existing brace-initialisation keeps working:
//     Vec2 v{1.0f, 2.0f};
//     Vec2 v = {x, y};
//     c.vel = {0.0f, 0.0f};
//
// The operators/helpers below are additive conveniences for future code;
// the original component-wise math continues to compile unchanged.
// ============================================================
struct Vec2 {
    float x = 0.0f;
    float y = 0.0f;
};

inline Vec2  operator+(Vec2 a, Vec2 b)      { return {a.x + b.x, a.y + b.y}; }
inline Vec2  operator-(Vec2 a, Vec2 b)      { return {a.x - b.x, a.y - b.y}; }
inline Vec2  operator*(Vec2 v, float s)     { return {v.x * s, v.y * s}; }
inline Vec2  operator*(float s, Vec2 v)     { return {v.x * s, v.y * s}; }
inline Vec2& operator+=(Vec2& a, Vec2 b)    { a.x += b.x; a.y += b.y; return a; }
inline Vec2& operator-=(Vec2& a, Vec2 b)    { a.x -= b.x; a.y -= b.y; return a; }

inline float dot(Vec2 a, Vec2 b)           { return a.x * b.x + a.y * b.y; }
inline float lengthSq(Vec2 v)              { return v.x * v.x + v.y * v.y; }
inline float length(Vec2 v)                { return std::sqrt(lengthSq(v)); }

inline Vec2 normalized(Vec2 v) {
    const float len = length(v);
    return (len > 1e-6f) ? Vec2{v.x / len, v.y / len} : Vec2{1.0f, 0.0f};
}
