#pragma once
// ocean_step_tests.hpp — the per-fish movement's arithmetic, pinned bit for bit,
// included by tests/ocean_tests.cpp just before main (namespace ot: Test,
// initQuiet, addFish are that file's).
//
//   step_edges   (1) the body trail (Fish::updateBody): one divide per
//                segment, ratio = segLen / dist, then dx * ratio, each segment
//                after the one before it moved — replayed here and compared
//                bit for bit, on a fixture where (dx / dist) * segLen gives
//                other bits (dx = dy = 5, segLen 4: 0x403504f4 vs 0x403504f3);
//                (2) the heading is normalised by division: an unwired fish
//                heading (3, fl(sqrt 40)) — length exactly 7 — moves with
//                heading (3 / 7, sqrt 40 / 7), where 3 * (1 / 7) would give
//                0x3edb6db8 instead of 0x3edb6db7
namespace ot {

void testStepEdges() {
    Test t("step_edges");
    auto bits = [](float f) { std::uint32_t u; std::memcpy(&u, &f, 4); return u; };
    // (1) The trail.
    {
        Fish f;
        f.pos = {100.0f, 100.0f};
        f.bodySize = 1.0f;
        f.bodyElong = 1.0f;
        for (int i = 0; i < Fish::BODY_LEN; ++i) f.body[i] = {100.0f + 5.0f * (float)i, 100.0f + 5.0f * (float)i};
        Vec2 want[Fish::BODY_LEN];
        for (int i = 0; i < Fish::BODY_LEN; ++i) want[i] = f.body[i];
        want[0] = f.pos;
        const float segLen = 4.0f * f.bodySize * f.bodyElong;
        for (int i = 1; i < Fish::BODY_LEN; ++i) {
            const float dx = want[i].x - want[i - 1].x, dy = want[i].y - want[i - 1].y;
            const float dist = std::sqrt(dx * dx + dy * dy);
            if (dist > segLen) {
                const float ratio = segLen / dist;
                want[i].x = want[i - 1].x + dx * ratio;
                want[i].y = want[i - 1].y + dy * ratio;
            }
        }
        f.updateBody();
        for (int i = 0; i < Fish::BODY_LEN; ++i)
            t.expect(bits(f.body[i].x) == bits(want[i].x) && bits(f.body[i].y) == bits(want[i].y),
                     "trail point %d = (%.9g, %.9g), the one-divide replay gives (%.9g, %.9g)", i, f.body[i].x, f.body[i].y,
                     want[i].x, want[i].y);
        const float d = std::sqrt(50.0f);
        t.expect(bits(5.0f * (4.0f / d)) == 0x403504f4u && bits((5.0f / d) * 4.0f) == 0x403504f3u,
                 "fixture: the two trail formulas no longer differ here (%08x %08x)", bits(5.0f * (4.0f / d)),
                 bits((5.0f / d) * 4.0f));
    }
    // (2) The heading's normalisation.
    {
        OceanConfig oc;
        OceanWorld w;
        initQuiet(w, 501, 240.0f, oc, /*capture*/ true);
        const int v = addFish(w, {600.0f, 500.0f}, 120.0f, {}, 0.5f);
        const float s40 = std::sqrt(40.0f);
        w.creatures[v].vel = {3.0f, s40};
        w.creatures[v].interactTimer = 1e9f;
        t.expect(std::sqrt(3.0f * 3.0f + s40 * s40) == 7.0f, "fixture: |(3, sqrt 40)| is not 7 in float");
        w.update(0.016f);
        const Vec2 got = w.creatures[0].vel;
        t.expect(bits(got.x) == bits(3.0f / 7.0f) && bits(got.x) == 0x3edb6db7u && bits(got.y) == bits(s40 / 7.0f),
                 "heading (%.9g, %.9g) = %08x %08x; (3 / 7, sqrt 40 / 7) = %08x %08x", got.x, got.y, bits(got.x), bits(got.y),
                 bits(3.0f / 7.0f), bits(s40 / 7.0f));
        t.expect(bits(3.0f * (1.0f / 7.0f)) == 0x3edb6db8u, "fixture: 3 x (1 / 7) is no longer a different float");
    }
}

}  // namespace ot
