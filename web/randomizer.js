"use strict";
/* evo_boids — 🎲 Randomize: draws a whole setup from tested ranges.
 *
 * Every value is drawn from a range. A range comes from the experiments
 * (docs/evo_boids_flags.md, the research log) where one exists, and from the
 * "try:" best guesses shown in each setting's explanation (TIPS in
 * renderer.js) where none does; RANGES says which. Knobs that only work
 * together are drawn together:
 *  - currents come with patchy, drifting food and a thrust cost: a current
 *    only matters when food is patchy, and fish ignore it while thrust is
 *    free (measured: 80 = they fight the flow, 150 = they ride it);
 *  - predators come with a thrust cost (their chase assumes a prey cruise
 *    of ~82, which the thrust cost sets) and the experiments' arena: uniform,
 *    abundant food and a population cap, so predation, not starvation, does
 *    the selecting;
 *  - the predator economy also comes with a fixed meal: the default (absorb
 *    the prey's reserves) lets predators explode;
 *  - the match gate comes only with its soft ramp: hard, it collapses the
 *    population below a threshold of ~16.
 * No page code here: draw() returns plain values, so tests can sample it. */
(function (root) {
  /* key: [min, max, step, source]. Values are rounded to the step. */
  var RANGES = {
    popFactor:           [0.7, 1.4, 0.05, "around this device's default population"],
    targetFood:          [500, 1200, 50, "try: 300 scarcity, 1200 abundance (scarcity left out)"],
    foodSpawnBase:       [3, 8, 0.5, "try: 2-12"],
    foodSpawnAmp:        [0, 3, 0.5, "default 2; 0 = steady supply"],
    foodEnergy:          [4, 7, 0.5, "try: 3-8 around the default 5"],
    maxAge:              [90, 180, 10, "try: 60-240 around the default 120"],
    maxAgeHunted:        [120, 180, 10, "with predators, at least the default: short lives + 4 hunters hit the floor"],
    reproEnergy:         [18, 30, 1, "try: 15-40"],
    newbornEnergy:       [20, 30, 1, "try: 15-35"],
    mutationMultiplier:  [0.5, 2, 0.1, "try: 0.5 conservative, 2 fast drift"],
    moveCost:            [0.00003, 0.00007, 0.00001, "measured: 3e-5 and 7e-5 both give a healthy cruise ~82"],
    geneCost:            [0.005, 0.01, 0.001, "try: 0.005; the experiments ran 0.01"],
    maxNeighbors:        [6, 8, 1, "starlings track ~6-7 nearest; 7 is the Ballerini number"],
    perceptionRadius:    [80, 160, 10, "try: 160 = the high-density schooling regime"],
    maxTurnRate:         [3, 5, 0.5, "try: 2-6 around the default 4"],
    hunters:             [3, 5, 1, "the arena runs used 6; measured: 3, 4 and 5 all healthy in the arena (30k steps x 4 each)"],
    economyHunters:      [3, 4, 1, "try: 3-4 with the economy on"],
    mealFactor:          [1, 1.1, 0.05, "meal / break-even; break-even = cooldown x (metabolism + thrust cost x 110^2), flags doc. Measured, 30k steps x 4 setups each: 0.9 starves the predators, 1.0-1.1 keeps both alive, 1.2 booms then crashes, 1.3 collapses the prey"],
    predatorMetabolism:  [2, 3, 0.5, "try: 2-3"],
    predatorReproEnergy: [50, 70, 5, "the economy runs used 60; try: 40-80"],
    predatorCooldown:    [3, 5, 0.5, "default 3; 5 eases the chase, the usual binding constraint"],
    arenaFood:           [1200, 2000, 100, "the arena runs used 2000 uniform food"],
    arenaFoodSpawn:      [8, 12, 1, "the arena runs used 12"],
    arenaCap:            [300, 450, 50, "the schooling arena capped at 350 (200 for a lighter run)"],
    economyCap:          [400, 600, 50, "the economy runs capped at 600, so prey can oscillate"],
    foodPatches:         [5, 6, 1, "try: 5-6; the current runs used 6"],
    foodPatchRadius:     [80, 200, 10, "try: 60 tight, 240 loose"],
    currentFight:        [70, 90, 5, "measured: 80 with a thrust cost = fish fight the flow"],
    currentRide:         [130, 160, 5, "measured: 150 with a thrust cost = fish ride it"],
    currentEddies:       [3, 8, 1, "try: 3-8 around the default 5"],
    currentCore:         [100, 250, 10, "try: 100-250 around the default 150"],
    currentDrift:        [0.5, 2, 0.1, "try: 0.5-2 around the default 1"],
    reproMaxDistance:    [16, 18, 0.5, "sweep: 16 with the soft ramp clears collapse, 18 bites gently; <14 collapses"],
    reproSoftWidth:      [4, 5, 0.5, "sweep: width 4 is the mitigation that clears collapse at 16"],
    bodyMutation:        [0.05, 0.2, 0.01, "try: 0.1 default; 0.3 explores fast"]
  };
  var PREDATOR_SPEED = 110;   /* the predatorSpeed default; never randomized */
  /* How often each optional idea is switched on. */
  var CHANCE = {
    predatorsScripted: 0.25, predatorsEconomy: 0.3,   /* else none */
    currents: 0.45, patchyFood: 0.2,                   /* else still water, uniform food */
    thrustCost: 0.5,          /* when nothing above forces it */
    toroidal: 0.8,            /* try: on; cures the wall-hugger phenotype */
    topological: 0.35, triangular: 0.25, widePerception: 0.2,
    geneCost: 0.3, bodyModel: 0.3, matchGate: 0.15, grace: 0.5,
    tuneFood: 0.5, tuneLife: 0.4, tuneMutation: 0.4
  };

  function round(v, step) {
    var r = Math.round(v / step) * step;
    return +r.toFixed(6);   /* no 0.30000000000000004 in the form */
  }

  /* rand: () => [0, 1). basePop: this device's default population.
   * -> { population, form: {...}, adv: {...}, labels: [...] } where form
   * holds the basic fields (by input id) and adv the advanced keys; every key
   * left out stays at its default. */
  function draw(rand, basePop) {
    function chance(p) { return rand() < p; }
    function pick(a) { return a[Math.floor(rand() * a.length)]; }
    function range(key) {
      var r = RANGES[key];
      return round(r[0] + (r[1] - r[0]) * rand(), r[2]);
    }
    var form = {}, adv = {}, labels = [];
    var population = Math.max(50, Math.round(basePop * range("popFactor")));
    var thrust = false;

    var u = rand();
    if (u < CHANCE.predatorsEconomy + CHANCE.predatorsScripted) {
      var economy = u < CHANCE.predatorsEconomy;
      form.predators = range(economy ? "economyHunters" : "hunters");
      adv.predatorCooldown = range("predatorCooldown");
      adv.targetFood = range("arenaFood");
      adv.foodSpawnBase = range("arenaFoodSpawn");
      adv.maxPopulation = range(economy ? "economyCap" : "arenaCap");
      adv.moveCost = range("moveCost");
      thrust = true;
      if (economy) {
        form.predatorEconomy = true;
        adv.predatorMetabolism = range("predatorMetabolism");
        /* a meal relative to what a hunt costs: eating once per cooldown at
         * cruise speed 110 (the default) breaks even at this */
        var breakEven = adv.predatorCooldown * (adv.predatorMetabolism + adv.moveCost * PREDATOR_SPEED * PREDATOR_SPEED);
        adv.predatorMeal = round(range("mealFactor") * breakEven, 0.5);
        adv.predatorReproEnergy = range("predatorReproEnergy");
        adv.minPopulation = 20;
        labels.push(form.predators + " predators that must eat to breed (meal " + adv.predatorMeal + ")");
      } else {
        labels.push(form.predators + " hunting predators");
      }
      labels.push("plenty of food, at most " + adv.maxPopulation + " fish");
      population = Math.min(population, adv.maxPopulation);   /* the cap only stops births */
    } else {
      var e = rand();
      if (e < CHANCE.currents) {
        var ride = chance(0.5);
        form.currentStrength = range(ride ? "currentRide" : "currentFight");
        adv.currentMode = pick([0, 1]);
        if (adv.currentMode === 0) {
          adv.currentEddies = range("currentEddies");
          adv.currentCore = range("currentCore");
        }
        adv.currentDrift = range("currentDrift");
        form.foodPatches = range("foodPatches");
        adv.foodPatchRadius = range("foodPatchRadius");
        adv.foodDrift = true;
        thrust = true;
        labels.push("ocean current " + form.currentStrength + " (" + (adv.currentMode ? "gyre" : "eddies") +
                    ") — fish tend to " + (ride ? "ride it" : "fight it"));
        labels.push(form.foodPatches + " drifting food patches");
      } else if (e < CHANCE.currents + CHANCE.patchyFood) {
        form.foodPatches = range("foodPatches");
        adv.foodPatchRadius = range("foodPatchRadius");
        labels.push(form.foodPatches + " food patches");
      }
      if (chance(CHANCE.tuneFood)) {
        adv.targetFood = range("targetFood");
        adv.foodSpawnBase = range("foodSpawnBase");
        adv.foodSpawnAmp = range("foodSpawnAmp");
        adv.foodEnergy = range("foodEnergy");
        labels.push("food " + adv.targetFood + ", " + adv.foodSpawnBase + "/step");
      }
    }

    if (thrust || chance(CHANCE.thrustCost)) {
      if (adv.moveCost === undefined) adv.moveCost = range("moveCost");
      adv.minSpeed = 0;   /* drifting must be an option once thrust costs */
      labels.push("swimming costs energy");
    }
    if (chance(CHANCE.toroidal)) { form.toroidal = true; labels.push("wrap-around senses"); }
    if (chance(CHANCE.topological)) {
      adv.maxNeighbors = range("maxNeighbors");
      labels.push("each fish follows its " + adv.maxNeighbors + " nearest (like starlings)");
    }
    if (chance(CHANCE.widePerception)) {
      adv.perceptionRadius = range("perceptionRadius");
      labels.push("sight " + adv.perceptionRadius);
    }
    if (chance(CHANCE.triangular)) {
      adv.triangularMotion = true;
      adv.maxTurnRate = range("maxTurnRate");
      labels.push("fast = straight, slow = agile");
    }
    if (chance(CHANCE.tuneLife)) {
      adv.maxAge = range(form.predators ? "maxAgeHunted" : "maxAge");
      adv.reproEnergy = range("reproEnergy");
      adv.newbornEnergy = range("newbornEnergy");
      labels.push("lifespan " + adv.maxAge + " s");
    }
    if (chance(CHANCE.tuneMutation)) {
      adv.mutationMultiplier = range("mutationMultiplier");
      labels.push("mutation ×" + adv.mutationMultiplier);
    }
    if (chance(CHANCE.geneCost)) {
      adv.geneCost = range("geneCost");
      labels.push("big genomes cost energy");
    }
    if (chance(CHANCE.bodyModel)) {
      form.bodyModel = true;
      adv.bodyMutation = range("bodyMutation");
      labels.push("heritable body size");
    }
    if (chance(CHANCE.matchGate)) {
      adv.reproMaxDistance = range("reproMaxDistance");
      adv.reproSoftWidth = range("reproSoftWidth");
      if (chance(CHANCE.grace)) adv.reproGracePeriod = 60;
      labels.push("picky partners (match gate " + adv.reproMaxDistance + ")");
    }
    return { population: population, form: form, adv: adv, labels: labels };
  }

  root.EvoRandom = { RANGES: RANGES, CHANCE: CHANCE, draw: draw };
})(typeof window !== "undefined" ? window : globalThis);
