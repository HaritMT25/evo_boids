#pragma once
#include <ostream>
#include <string>
#include "world.hpp"

// Per-run archive of the pairing graph: one row per kept pairing event,
// written to <stem>.births.csv at the end of a headless run when --repro-log is
// set. The in-memory log is bounded and uniformly decimated (World::logBirth,
// shaped exactly like the death-cluster log), so `index` is the true event
// ordinal over the whole run and consecutive rows may skip — the stride is
// reported in the file's comment header so a reader can tell decimation from a
// stretch with no pairings.
//
// The DeathRecord log deliberately carries lineageId but NOT Creature ids or
// the ids of the pair, which is precisely the edge a pairing graph needs; that
// is why this is a separate record type rather than a reuse.
//
// Offline this supports what the live repro_components column cannot: the graph
// at any chosen window, component size distributions, whether components merge
// or split over time, and whether two provenance groups ever exchange an edge.

namespace birthlog {

inline std::string header() {
    return "index,frame,parent1,parent2,lineage1,lineage2";
}

// Write the whole kept log. Returns the number of rows written.
inline std::size_t write(std::ostream& os, const World& world) {
    os << "# evo_boids pairing log — one row per kept pairing event\n"
       << "# events seen: " << world.totalBirthsLogged_
       << "   kept: " << world.birthLog.size()
       << "   decimation stride: " << world.reproStride_ << "\n"
       << "# an edge (parent1,parent2) means these two Creatures produced one successor\n"
       << header() << "\n";
    for (const World::BirthRecord& b : world.birthLog) {
        os << b.index << ','
           << b.frame << ','
           << b.parent1 << ','
           << b.parent2 << ','
           << b.lineage1 << ','
           << b.lineage2 << '\n';
    }
    return world.birthLog.size();
}

}  // namespace birthlog
