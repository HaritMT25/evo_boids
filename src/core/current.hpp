#pragma once
#include <vector>
#include <cmath>
#include "vec2.hpp"
#include "random.hpp"

// ----------------------------------------------------------------------------
// CurrentField — a time-varying, divergence-free (incompressible) 2D ocean
// current. Two selectable structures, both built from streamfunctions so they
// have no sinks (passive matter is stretched into filaments, never compressed
// to points):
//
//   Mode::Eddies — superposition of moving vortices ("eyes"). Each is a smooth
//     rotational cell; summing several gives many small trapping pockets that
//     wander over time. Matches the "moving eyes" mental model.
//
//   Mode::Gyre — the canonical time-periodic double-gyre: two counter-rotating
//     basins separated by a vertical line that oscillates left/right on a fixed
//     period, so the two halves periodically seal off and reconnect. Cleaner
//     two-region geographic isolation; the standard fluid-transport benchmark.
//
// `strength` is the peak flow speed in world units/sec (0 = off). Both modes are
// divergence-free by construction: velocity is the curl of a scalar streamfunction.
// ----------------------------------------------------------------------------
class CurrentField {
public:
    enum class Mode { Eddies, Gyre };

private:
    struct Vortex {
        Vec2  base;
        float sign;
        float ax, ay, fx, fy, px, py;
    };
    Mode  mode_ = Mode::Eddies;
    std::vector<Vortex> vortices_;
    std::vector<Vec2>   centers_;
    float strength_ = 0.0f;
    float core_     = 150.0f;
    int   w_ = 1, h_ = 1;

    // gyre params
    float gyreEps_   = 0.25f;  // separatrix oscillation amplitude
    float gyreOmega_ = 0.0f;   // angular frequency per frame
    float gyreA_     = 0.0f;   // streamfunction amplitude (world units)
    float gyreT_     = 0.0f;   // current frame time

    static constexpr float PI = 3.14159265358979f;

    Vec2 gyreVel(Vec2 p) const {
        const float xn = 2.0f * p.x / (float)w_;            // [0,2]
        const float s  = std::sin(gyreOmega_ * gyreT_);
        const float a  = gyreEps_ * s;
        const float b  = 1.0f - 2.0f * gyreEps_ * s;
        const float f  = a * xn * xn + b * xn;
        const float dfdx = (2.0f / (float)w_) * (2.0f * a * xn + b);  // d f / d(world x)
        const float sy = std::sin(PI * p.y / (float)h_);
        const float cy = std::cos(PI * p.y / (float)h_);
        const float sf = std::sin(PI * f);
        const float cf = std::cos(PI * f);
        // psi = A sin(pi f) sin(pi y/H);  u = -d psi/dy,  v = d psi/dx
        return { -gyreA_ * sf * (PI / (float)h_) * cy,
                  gyreA_ * cf * PI * dfdx * sy };
    }

public:
    bool active() const { return strength_ > 0.0f; }

    void init(Mode mode, int worldW, int worldH, int count,
              float strength, float core, float drift, float gyreEps) {
        mode_ = mode; w_ = worldW; h_ = worldH;
        strength_ = strength; core_ = (core > 1.0f) ? core : 1.0f;
        vortices_.clear(); centers_.clear();
        gyreEps_   = gyreEps;
        gyreOmega_ = 0.0003f * drift;
        gyreA_     = strength * (float)worldH / PI;   // so peak |u| ~ strength
        if (strength <= 0.0f) return;

        if (mode_ == Mode::Eddies) {
            if (count <= 0) return;
            const float amp = 0.15f * (float)std::min(worldW, worldH);
            for (int i = 0; i < count; ++i) {
                Vortex v;
                v.base = { rng.range(0.0f,(float)worldW), rng.range(0.0f,(float)worldH) };
                v.sign = rng.uint(1) ? 1.0f : -1.0f;
                v.ax = amp * rng.range(0.5f,1.0f);  v.ay = amp * rng.range(0.5f,1.0f);
                const float bf = 0.0003f * drift;
                v.fx = bf * rng.range(0.6f,1.4f);   v.fy = bf * rng.range(0.6f,1.4f);
                v.px = rng.range(0.0f,6.2832f);     v.py = rng.range(0.0f,6.2832f);
                vortices_.push_back(v);
            }
            centers_.resize(vortices_.size());
        }
    }

    void advance(unsigned long long frame) {
        if (strength_ <= 0.0f) return;
        if (mode_ == Mode::Gyre) { gyreT_ = (float)frame; return; }
        const float t = (float)frame;
        for (size_t i = 0; i < vortices_.size(); ++i) {
            const Vortex& v = vortices_[i];
            centers_[i] = { v.base.x + v.ax*std::sin(v.fx*t + v.px),
                            v.base.y + v.ay*std::sin(v.fy*t + v.py) };
        }
    }

    Vec2 velocityAt(Vec2 p) const {
        if (strength_ <= 0.0f) return {0.0f, 0.0f};
        if (mode_ == Mode::Gyre) return gyreVel(p);
        Vec2 v{0.0f, 0.0f};
        const float inv2c2 = 1.0f / (2.0f * core_ * core_);
        const float scale  = strength_ / core_ * 1.6487212707f;  // exp(0.5)
        for (size_t i = 0; i < centers_.size(); ++i) {
            const float dx = p.x - centers_[i].x;
            const float dy = p.y - centers_[i].y;
            const float f  = vortices_[i].sign * scale * std::exp(-(dx*dx+dy*dy)*inv2c2);
            v.x += f*(-dy); v.y += f*dx;
        }
        return v;
    }
};
