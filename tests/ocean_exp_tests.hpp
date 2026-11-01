// ocean_exp_tests.hpp — the ALIFE 2027 experiment instruments
// (tools/ocean_eye_exp_support.hpp, protocol v2). Included by
// tests/ocean_tests.cpp inside namespace ot (uses its Test, mkGene); eye build
// only (the driver is). Runs native and wasm under wasm/ocean_parity.sh, whose
// output comparison makes every note below a native == wasm check too.
//   exp_arms_init   every v2 arm's keys, installed after the common
//                   initialization, change only the config section
//   exp_observers   every instrument on (dense chain, diagnostics, encounters)
//                   leaves the run's end state and rng as without; the food
//                   identity / expiry replay hold every step (also with the
//                   spawn cap binding); the dense chain catches a transient
//                   change the end state does not show
//   exp_branches    sham continuation == the uninterrupted run; parent world
//                   and rng untouched; continuation order irrelevant; every
//                   intervention changes the run; branch-spec parsing
#ifdef EVO_OCEAN_EYE
// protocol v2's L12 ecology (experiments/alife2027/v2/common_l12.json)
constexpr const char* kExpL12 =
    R"({"walls":false,"toroidal":false,"appearance":0,"minPopulation":50,"perceptionRadius":100,"interactRadius":36,)"
    R"("pickupRadius":24,"foodSpawnBase":1,"foodSpawnAmp":0,"foodEnergy":15,"targetFood":800,"foodLife":12,)"
    R"("predatorCount":4,"predatorSpeed":110,"predatorPerception":150,"predatorCaptureRadius":14,"predatorEconomy":true,)"
    R"("predatorCooldown":3,"predatorMetabolism":2,"predatorMoveCost":0,"predatorMeal":10,"predatorReproEnergy":60,)"
    R"("predatorMaxPopulation":6,"predatorMinPopulation":2,"predatorInheritance":true})";

struct ExpArm { const char* name; std::vector<std::pair<std::string, int>> keys; };
inline std::vector<ExpArm> expArms() {
    std::vector<ExpArm> a = {
        {"C1", {{"eye", 1}, {"eyeOnly", 1}, {"steerFrame", 1}}}, {"D1", {{"eye", 0}, {"eyeOnly", 1}, {"steerFrame", 1}}},
        {"A0", {{"eye", 0}, {"eyeOnly", 0}, {"steerFrame", 0}}}, {"C0", {{"eye", 1}, {"eyeOnly", 1}, {"steerFrame", 0}}},
        {"D0", {{"eye", 0}, {"eyeOnly", 1}, {"steerFrame", 0}}},
        {"C1nf", {{"eye", 1}, {"eyeOnly", 1}, {"steerFrame", 1}, {"eyeCode", 8}}},
        {"C1nd", {{"eye", 1}, {"eyeOnly", 1}, {"steerFrame", 1}, {"eyeCode", 16}}},
        {"C1po", {{"eye", 1}, {"eyeOnly", 1}, {"steerFrame", 1}, {"eyeCode", 24}}}};
    if (alx::hasLabelFrame()) a.push_back({"A1", {{"eye", 0}, {"eyeOnly", 0}, {"steerFrame", 1}, {"labelFrame", 1}}});
    return a;
}

inline bool expWorld(Test& t, OceanWorld& w, unsigned seed, int W, int H, int pop, const ExpArm* arm) {
    std::string err;
    if (!t.expect(ofp::initWorld(w, seed, W, H, 240.0f, pop, kExpL12, err), "init: %s", err.c_str())) return false;
    if (arm)
        for (const auto& k : arm->keys)
            if (!t.expect(alx::setArmKey(w.ocfg, k.first, k.second, err), "%s: %s", arm->name, err.c_str())) return false;
    return true;
}

inline void testExpArmsShareInit() {
    Test t("exp_arms_init");
    const auto arms = expArms();
    OceanWorld ref;
    if (!expWorld(t, ref, 4242, 800, 600, 120, nullptr)) return;
    const ofp::Fingerprint f0 = ofp::fingerprint(ref);
    for (const ExpArm& a : arms) {
        OceanWorld w;
        if (!expWorld(t, w, 4242, 800, 600, 120, &a)) continue;
        const ofp::Fingerprint f = ofp::fingerprint(w);
        t.expect(f.ctr == f0.ctr && f.fish == f0.fish && f.food == f0.food && f.pred == f0.pred && f.sig == f0.sig &&
                     f.cur == f0.cur && f.geo == f0.geo && f.rng == f0.rng,
                 "%s: a non-config section differs from the common initialization", a.name);
        t.expect(std::string(a.name) == "A0" || f.cfg != f0.cfg, "%s: the arm left the config hash unchanged", a.name);
        std::string e;
        OceanConfig oc = w.ocfg;
        t.expect(!alx::setArmKey(oc, "eye", 2, e) && !e.empty(), "%s: eye=2 accepted", a.name);
    }
    std::string e;
    OceanConfig oc;
    t.expect(!alx::setArmKey(oc, "speed", 1, e), "an unknown arm key was accepted");
    t.note("%zu arms (%s): the 8 non-config sections equal the common initialization's", arms.size(),
           alx::hasLabelFrame() ? "A1 included" : "no labelFrame in this engine");
}

inline void testExpObserversNeutral() {
    Test t("exp_observers");
    const ExpArm c1 = expArms()[0];
    OceanWorld a, b;
    if (!expWorld(t, a, 77, 900, 600, 150, &c1) || !expWorld(t, b, 77, 900, 600, 150, &c1)) return;
    alx::FpObserver fa, fb;
    fa.dense = fb.dense = true;
    alx::StepDiag d;
    alx::EncounterDiag enc;
    d.begin(a);
    fa.observe(a, 0, false);
    fb.observe(b, 0, false);
    long long pairSteps = 0;
    for (long s = 1; s <= 300; ++s) {
        // a's instruments run between its updates; b runs bare. One rng: a's
        // update, then b's, each from the rng its own state needs.
        const RandomGen ra = rng;
        d.before(a, 0.016f);
        enc.before(a);
        a.update(0.016f);
        d.after(a, s);
        enc.after(a, s);
        fa.observe(a, s, false);
        if (s % 100 == 0) { pairSteps += enc.pairSteps; enc.resetInterval(); }
        const RandomGen afterA = rng;
        rng = ra;   // b replays the same draws
        b.update(0.016f);
        if (s == 150) {   // a transient change, put back before the next update
            const float keep = b.creatures[0].foodReserves;
            b.creatures[0].foodReserves = std::nextafter(keep, 1e30f);
            fb.observe(b, s, false);
            b.creatures[0].foodReserves = keep;
        } else {
            fb.observe(b, s, false);
        }
        t.expect(afterA.state() == rng.state(), "step %ld: the instrumented step used the rng differently", s);
        rng = afterA;
    }
    t.expect(fa.last == fb.last, "the instrumented run ended in a different state (or rng)");
    t.expect(fa.chain.value() != fb.chain.value(), "the dense chain missed a transient change between printed steps");
    t.expect(d.identityBad == 0 && d.expiryBad == 0, "food identity broken on %lld steps, expiry replay on %lld",
             d.identityBad, d.expiryBad);
    t.expect(d.auc > 0 && d.minPost <= d.n0 && d.minPre <= d.minPost, "diagnostics inconsistent: auc %lld min %lld pre %lld",
             d.auc, d.minPost, d.minPre);
    t.expect(a.totalFoodExpired > 0, "no food expired in 300 steps (the identity was not exercised)");
    // the spawn cap binding: 3 attempts a step, targetFood 20 (cap 40), foodLife 1 s
    OceanWorld c;
    std::string err;
    if (t.expect(ofp::initWorld(c, 78, 600, 400, 240.0f, 60,
                                R"({"foodSpawnBase":3,"foodSpawnAmp":0,"targetFood":20,"foodLife":1,"minPopulation":0})", err),
                 "init: %s", err.c_str())) {
        alx::StepDiag dc;
        dc.begin(c);
        long long attempts = 0;
        for (long s = 1; s <= 400; ++s) { dc.before(c, 0.016f); c.update(0.016f); dc.after(c, s); attempts += 3; }
        t.expect(dc.identityBad == 0 && dc.expiryBad == 0, "capped world: identity %lld, expiry %lld", dc.identityBad, dc.expiryBad);
        t.expect(dc.arrivals < attempts && c.totalFoodExpired > 0, "the cap never bound (%lld of %lld) or nothing expired",
                 dc.arrivals, attempts);
        t.note("capped world: %lld arrivals of %lld attempts, %lld expired, %lld eaten: the identity held every step",
               dc.arrivals, attempts, c.totalFoodExpired, c.totalFoodEaten);
    }
    t.note("300 steps, every instrument on: same end state and rng; %lld pair-steps within the radius; min pop %lld",
           pairSteps, d.minPost);
}

inline void testExpBranches() {
    Test t("exp_branches");
    const ExpArm c1 = expArms()[0];
    OceanWorld w;
    if (!expWorld(t, w, 91, 900, 600, 150, &c1)) return;
    for (int s = 0; s < 200; ++s) w.update(0.016f);
    std::vector<alx::BranchSpec> specs;
    std::string err;
    t.expect(alx::parseBranchSpecs("sham:;blind:eye=0;foodcloak:eyeCode=16;fishcloak:eyeCode=8;frameswap:steerFrame=0", specs, err) &&
                 specs.size() == 5 && specs[1].changes.size() == 1 && specs[0].changes.empty(),
             "spec parse: %s", err.c_str());
    std::vector<alx::BranchSpec> bad;
    t.expect(!alx::parseBranchSpecs("a:eye=1;a:", bad, err) && !alx::parseBranchSpecs("x:eye", bad, err) &&
                 !alx::parseBranchSpecs("x:eye=1z", bad, err) && !alx::parseBranchSpecs("", bad, err),
             "a bad spec was accepted");
    const ofp::Fingerprint before = ofp::fingerprint(w);
    const RandomGen parentRng = rng;
    std::vector<alx::BranchResult> fwd(specs.size()), rev(specs.size());
    for (std::size_t k = 0; k < specs.size(); ++k)
        t.expect(alx::runContinuation(w, parentRng, specs[k], 200, 120, 40, 50, true, 0.016f, fwd[k], err), "%s", err.c_str());
    t.expect(ofp::fingerprint(w) == before, "the parent (world or rng) changed");
    for (std::size_t k = specs.size(); k-- > 0;)
        t.expect(alx::runContinuation(w, parentRng, specs[k], 200, 120, 40, 50, true, 0.016f, rev[k], err), "%s", err.c_str());
    long differ = 0;
    for (std::size_t k = 0; k < specs.size(); ++k) {
        t.expect(fwd[k].chain.value() == rev[k].chain.value() && fwd[k].eaten == rev[k].eaten && fwd[k].births == rev[k].births,
                 "%s: the result depends on the order", specs[k].name.c_str());
        t.expect(fwd[k].identityBad == 0 && fwd[k].expiryBad == 0 && fwd[k].nonFinite == 0, "%s: food identity / finite",
                 specs[k].name.c_str());
        if (k > 0) differ += fwd[k].chain.value() != fwd[0].chain.value();
    }
    t.expect(differ == (long)specs.size() - 1, "only %ld of the %zu interventions changed the run", differ, specs.size() - 1);
    long compared = 0, same = 0;   // the uninterrupted run passes the sham's states
    std::size_t next = 1;
    for (long s = 1; s <= 120; ++s) {
        w.update(0.016f);
        if (next < fwd[0].fps.size() && fwd[0].fps[next].first == s) {
            ++compared;
            same += ofp::fingerprint(w) == fwd[0].fps[next].second;
            ++next;
        }
    }
    t.expect(compared == 3 && same == compared, "sham vs the uninterrupted run: %ld of %ld fingerprints equal", same, compared);
    t.note("5 continuations x 120 steps from step 200: sham == uninterrupted at %ld checkpoints, order-independent, "
           "parent kept; sham ate %lld, blind %lld",
           compared, fwd[0].eaten, fwd[1].eaten);
}
#endif
