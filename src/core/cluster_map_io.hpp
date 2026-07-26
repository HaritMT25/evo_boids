#pragma once
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <array>
#include <map>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include "world.hpp"
#include "cluster_projection.hpp"

// Writes the death-cluster map collected in World::deathLog to:
//   <stem>.deaths.csv  — one row per kept death (index,x,y,epoch,lineage,age,len,cause)
//   <stem>.cluster.svg — a scatter of the projected networks. Each point is coloured
//        either by epoch (= deathIndex / clusterEpoch; cool->warm over the run) or by
//        lineage (clade), per cfg.clusterColor. Axes are the fixed random projection
//        by default, or PCA over the death sketches when cfg.clusterPca (variance-
//        aligned, per-run). Epoch centroids are joined by arrows so the population's
//        drift direction is explicit, and a short lineage summary lists the clades
//        that dominated and for how long. Self-contained SVG; opens in any browser.
//
// No-op when the log is empty. Returns {csvPath, svgPath} (empty on no data/failure).

namespace clustermap {

// 5-stop ramp (deep blue -> red): early deaths cool, late deaths warm.
inline void rampColor(double t, int& r, int& g, int& b) {
    t = std::clamp(t, 0.0, 1.0);
    struct S { double p, r, g, b; };
    static const S stops[] = {
        {0.00,  30,  44, 120}, {0.25,  40, 120, 200}, {0.50,  40, 175, 110},
        {0.75, 235, 165,  45}, {1.00, 215,  50,  50},
    };
    for (int i = 1; i < 5; ++i) if (t <= stops[i].p) {
        const S& a = stops[i - 1]; const S& c = stops[i];
        const double f = (t - a.p) / (c.p - a.p);
        r = (int)std::lround(a.r + f * (c.r - a.r));
        g = (int)std::lround(a.g + f * (c.g - a.g));
        b = (int)std::lround(a.b + f * (c.b - a.b));
        return;
    }
    r = (int)stops[4].r; g = (int)stops[4].g; b = (int)stops[4].b;
}

// Hash a lineage id to a distinct vivid colour (stable across runs).
inline void lineageColor(std::uint64_t id, int& r, int& g, int& b) {
    std::uint64_t h = id * 0x9E3779B97F4A7C15ull;
    h ^= h >> 29; h *= 0xBF58476D1CE4E5B9ull; h ^= h >> 32;
    const double hue = (double)(h % 3600) / 3600.0 * 6.0;   // 0..6
    const double s = 0.62, v = 0.95;
    const int    hi = (int)hue % 6;
    const double f = hue - std::floor(hue);
    const double p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
    double R, G, B;
    switch (hi) {
        case 0: R=v;G=t;B=p; break; case 1: R=q;G=v;B=p; break;
        case 2: R=p;G=v;B=t; break; case 3: R=p;G=q;B=v; break;
        case 4: R=t;G=p;B=v; break; default: R=v;G=p;B=q; break;
    }
    r = (int)std::lround(R * 235); g = (int)std::lround(G * 235); b = (int)std::lround(B * 235);
}

// Cyclic Jacobi eigen-decomposition of a symmetric KxK matrix (K small). On return
// w[] holds eigenvalues and the columns of V[][] the corresponding eigenvectors.
inline void jacobiEigen(double a[CLUSTER_SKETCH_DIM][CLUSTER_SKETCH_DIM],
                        double w[CLUSTER_SKETCH_DIM],
                        double V[CLUSTER_SKETCH_DIM][CLUSTER_SKETCH_DIM]) {
    constexpr int K = CLUSTER_SKETCH_DIM;
    for (int i = 0; i < K; ++i) { for (int j = 0; j < K; ++j) V[i][j] = (i == j); }
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int p = 0; p < K; ++p) for (int q = p + 1; q < K; ++q) off += a[p][q] * a[p][q];
        if (off < 1e-18) break;
        for (int p = 0; p < K; ++p) for (int q = p + 1; q < K; ++q) {
            if (std::fabs(a[p][q]) < 1e-300) continue;
            const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
            const double t = (theta >= 0 ? 1.0 : -1.0) /
                             (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
            const double c = 1.0 / std::sqrt(t * t + 1.0), s = t * c;
            for (int k = 0; k < K; ++k) {
                const double akp = a[k][p], akq = a[k][q];
                a[k][p] = c * akp - s * akq; a[k][q] = s * akp + c * akq;
            }
            for (int k = 0; k < K; ++k) {
                const double apk = a[p][k], aqk = a[q][k];
                a[p][k] = c * apk - s * aqk; a[q][k] = s * apk + c * aqk;
            }
            for (int k = 0; k < K; ++k) {
                const double vkp = V[k][p], vkq = V[k][q];
                V[k][p] = c * vkp - s * vkq; V[k][q] = s * vkp + c * vkq;
            }
        }
    }
    for (int i = 0; i < K; ++i) w[i] = a[i][i];
}

// PCA the stored sketches to 2-D. Fills px,py. Deterministic eigenvector signs.
inline bool pcaProject(const std::vector<World::DeathRecord>& pts,
                       std::vector<float>& px, std::vector<float>& py) {
    constexpr int K = CLUSTER_SKETCH_DIM;
    const std::size_t N = pts.size();
    if (N < 3) return false;
    double mean[K] = {};
    for (const auto& d : pts) for (int k = 0; k < K; ++k) mean[k] += d.sketch[k];
    for (int k = 0; k < K; ++k) mean[k] /= (double)N;
    double C[K][K] = {};
    for (const auto& d : pts) {
        double c[K]; for (int k = 0; k < K; ++k) c[k] = d.sketch[k] - mean[k];
        for (int i = 0; i < K; ++i) for (int j = i; j < K; ++j) C[i][j] += c[i] * c[j];
    }
    for (int i = 0; i < K; ++i) for (int j = i; j < K; ++j) {
        C[i][j] /= (double)N; C[j][i] = C[i][j];
    }
    double w[K], V[K][K]; jacobiEigen(C, w, V);
    int a = 0, b = 1;                       // indices of the two largest eigenvalues
    for (int k = 1; k < K; ++k) if (w[k] > w[a]) a = k;
    for (int k = 0; k < K; ++k) if (k != a && (b == a || w[k] > w[b])) b = k;
    if (b == a) b = (a == 0 ? 1 : 0);
    auto fixSign = [&](int axis) {           // make largest-|component| positive
        int m = 0; for (int k = 1; k < K; ++k) if (std::fabs(V[k][axis]) > std::fabs(V[m][axis])) m = k;
        if (V[m][axis] < 0) for (int k = 0; k < K; ++k) V[k][axis] = -V[k][axis];
    };
    fixSign(a); fixSign(b);
    px.resize(N); py.resize(N);
    for (std::size_t n = 0; n < N; ++n) {
        double x = 0, y = 0;
        for (int k = 0; k < K; ++k) { const double cc = pts[n].sketch[k] - mean[k]; x += cc * V[k][a]; y += cc * V[k][b]; }
        px[n] = (float)x; py[n] = (float)y;
    }
    return true;
}

inline std::pair<std::string, std::string>
write(const World& world, const std::string& stem) {
    const auto& pts = world.deathLog;
    if (pts.empty()) return {"", ""};

    const std::filesystem::path base(stem);
    if (base.has_parent_path()) std::filesystem::create_directories(base.parent_path());
    const std::string csvPath = stem + ".deaths.csv";
    const std::string svgPath = stem + ".cluster.svg";

    const int epochLen = std::max(1, world.cfg.clusterEpoch);
    const long long maxIndex = pts.back().index;
    const int maxEpoch = (int)(maxIndex / epochLen);
    const bool byLineage = (world.cfg.clusterColor == 1);

    // Plot coordinates: PCA (per-run, variance-aligned) or the fixed random map.
    std::vector<float> X(pts.size()), Y(pts.size());
    bool usedPca = false;
    if (world.cfg.clusterPca) usedPca = pcaProject(pts, X, Y);
    if (!usedPca) for (std::size_t i = 0; i < pts.size(); ++i) { X[i] = pts[i].x; Y[i] = pts[i].y; }

    // --- CSV (always the stable fixed-map x,y, plus lineage; PCA is a view choice) ---
    if (std::ofstream csv{csvPath}) {
        csv << "index,x,y,epoch,lineage,age,genome_len,cause\n";
        for (const auto& d : pts)
            csv << d.index << ',' << d.x << ',' << d.y << ',' << (d.index / epochLen)
                << ',' << d.lineageId << ',' << d.age << ',' << d.genomeLen << ',' << d.cause << '\n';
    }

    // --- SVG ---
    float minX = X[0], maxX = X[0], minY = Y[0], maxY = Y[0];
    for (std::size_t i = 0; i < pts.size(); ++i) {
        minX = std::min(minX, X[i]); maxX = std::max(maxX, X[i]);
        minY = std::min(minY, Y[i]); maxY = std::max(maxY, Y[i]);
    }
    const float spanX = std::max(1e-6f, maxX - minX), spanY = std::max(1e-6f, maxY - minY);
    const int W = 920, H = 920, pad = 60, plotW = W - 2 * pad, plotH = H - 2 * pad;
    auto sx = [&](float x) { return pad + (x - minX) / spanX * plotW; };
    auto sy = [&](float y) { return pad + (maxY - y) / spanY * plotH; };

    std::ofstream svg{svgPath};
    if (!svg) return {csvPath, ""};
    svg << "<svg xmlns='http://www.w3.org/2000/svg' width='" << W << "' height='" << H
        << "' viewBox='0 0 " << W << ' ' << H << "'>\n"
        << "<rect width='" << W << "' height='" << H << "' fill='#0f1e19'/>\n"
        << "<rect x='" << pad << "' y='" << pad << "' width='" << plotW << "' height='"
        << plotH << "' fill='none' stroke='#2a3f38' stroke-width='1'/>\n"
        << "<defs><marker id='ar' markerWidth='8' markerHeight='8' refX='6' refY='3' orient='auto'>"
        << "<path d='M0,0 L6,3 L0,6 Z' fill='#eef'/></marker></defs>\n";

    // Points.
    for (std::size_t i = 0; i < pts.size(); ++i) {
        int r, g, b;
        if (byLineage) lineageColor(pts[i].lineageId, r, g, b);
        else rampColor(maxEpoch > 0 ? (double)(pts[i].index / epochLen) / maxEpoch : 0.0, r, g, b);
        svg << "<circle cx='" << (int)std::lround(sx(X[i])) << "' cy='" << (int)std::lround(sy(Y[i]))
            << "' r='2.3' fill='rgb(" << r << ',' << g << ',' << b << ")' fill-opacity='0.55'/>\n";
    }

    // Epoch centroids + drift arrows (in plot coords), regardless of colour mode.
    const int nep = maxEpoch + 1;
    std::vector<double> cx(nep, 0), cy(nep, 0); std::vector<long long> cn(nep, 0);
    for (std::size_t i = 0; i < pts.size(); ++i) {
        const int e = (int)(pts[i].index / epochLen);
        cx[e] += X[i]; cy[e] += Y[i]; ++cn[e];
    }
    for (int e = 0; e < nep; ++e) if (cn[e]) { cx[e] /= cn[e]; cy[e] /= cn[e]; }
    for (int e = 1; e < nep; ++e) {
        if (!cn[e] || !cn[e - 1]) continue;
        svg << "<line x1='" << (int)std::lround(sx((float)cx[e-1])) << "' y1='" << (int)std::lround(sy((float)cy[e-1]))
            << "' x2='" << (int)std::lround(sx((float)cx[e])) << "' y2='" << (int)std::lround(sy((float)cy[e]))
            << "' stroke='#dde' stroke-width='1.6' stroke-opacity='0.8' marker-end='url(#ar)'/>\n";
    }
    for (int e = 0; e < nep; ++e) if (cn[e]) {
        int r, g, b; rampColor(nep > 1 ? (double)e / (nep - 1) : 0.0, r, g, b);
        svg << "<circle cx='" << (int)std::lround(sx((float)cx[e])) << "' cy='" << (int)std::lround(sy((float)cy[e]))
            << "' r='5' fill='rgb(" << r << ',' << g << ',' << b << ")' stroke='#fff' stroke-width='1'/>\n";
    }

    // Title.
    svg << "<text x='" << pad << "' y='34' fill='#cfe' font-family='monospace' font-size='17'>"
        << "evo_boids death-cluster &#183; " << pts.size() << " of " << (maxIndex + 1)
        << " deaths &#183; " << (usedPca ? "PCA axes" : "random axes")
        << " &#183; colour=" << (byLineage ? "lineage" : "epoch")
        << " &#183; arrows=epoch drift</text>\n";

    // Epoch legend (only meaningful for epoch colour; always show the drift ramp).
    const int lx = pad, ly = H - 26, lw = plotW, lh = 12, nseg = 64;
    for (int i = 0; i < nseg; ++i) {
        int r, g, b; rampColor((double)i / (nseg - 1), r, g, b);
        svg << "<rect x='" << (lx + i * lw / nseg) << "' y='" << ly << "' width='" << (lw / nseg + 1)
            << "' height='" << lh << "' fill='rgb(" << r << ',' << g << ',' << b << ")'/>\n";
    }
    svg << "<text x='" << lx << "' y='" << (ly - 6) << "' fill='#9ab' font-family='monospace' font-size='12'>death 0</text>\n"
        << "<text x='" << (lx + lw - 92) << "' y='" << (ly - 6) << "' fill='#9ab' font-family='monospace' font-size='12'>death "
        << (maxIndex + 1) << "</text>\n"
        << "<text x='" << lx << "' y='" << (ly + lh + 14) << "' fill='#9ab' font-family='monospace' font-size='11'>"
        << "band every " << epochLen << " deaths (" << nep << " bands)</text>\n";

    // Lineage summary: top clades by kept-point count + the epoch span they cover
    // ("how long a lineage dominated"). Reads the decimated sample.
    std::map<std::uint64_t, std::array<long long, 3>> lin;  // id -> {count, minEpoch, maxEpoch}
    for (const auto& d : pts) {
        const int e = (int)(d.index / epochLen);
        auto it = lin.find(d.lineageId);
        if (it == lin.end()) lin[d.lineageId] = {1, e, e};
        else { it->second[0]++; it->second[1] = std::min(it->second[1], (long long)e); it->second[2] = std::max(it->second[2], (long long)e); }
    }
    std::vector<std::pair<std::uint64_t, std::array<long long, 3>>> top(lin.begin(), lin.end());
    std::sort(top.begin(), top.end(), [](auto& a, auto& b) { return a.second[0] > b.second[0]; });
    svg << "<text x='" << pad << "' y='" << (pad + 14) << "' fill='#9cc' font-family='monospace' font-size='11'>"
        << lin.size() << " lineages &#183; top clades (share, epochs):</text>\n";
    for (int i = 0; i < (int)std::min<std::size_t>(5, top.size()); ++i) {
        int r, g, b; lineageColor(top[i].first, r, g, b);
        const double share = 100.0 * top[i].second[0] / pts.size();
        std::ostringstream line; line.setf(std::ios::fixed); line.precision(1);
        line << "#" << top[i].first << "  " << share << "%  ep " << top[i].second[1] << "-" << top[i].second[2];
        svg << "<text x='" << pad << "' y='" << (pad + 30 + i * 15) << "' fill='rgb(" << r << ',' << g << ',' << b
            << ")' font-family='monospace' font-size='11'>" << line.str() << "</text>\n";
    }

    svg << "</svg>\n";
    return {csvPath, svgPath};
}

}  // namespace clustermap
