#pragma once
// ----------------------------------------------------------------------------
// OceanFoodGrid — the Ocean's food grid: SpatialGrid (src/core/spatial_grid.hpp,
// Classic's, untouched) whose entries also carry what the per-fish scans read
// of an item: besides the position given to insert() and the item's index, its
// depth, its size (OceanFood::size, the mouth gate's) and, in the eye build, its
// eyeCon — copied once, at the step's grid build, so the smell and the eye read
// the entry they are visiting instead of food[index] at a scattered address.
// The entries are stored field by field (one array per field, all in the grid's
// cell order: slot k of every array is entry k), so a scan over a row of cells
// reads each field contiguously.
//
// Why the copies are the values: food is not moved, changed, added or removed
// between the grid build and the end of the per-fish phase (drift, ageing and
// spawning run before the build; eating runs in the serial phase after it), so
// at every read the copy equals what food[index] holds. Eating still goes by
// index to food[] and its live `alive` flag.
//
// The layout (cols, rows, the clamped cell of a point, the stable counting
// sort), a query's cell rectangle, a wrapped query's nine images in their order
// with their clipping, and the 2-D test against the (translated) query centre
// are SpatialGrid's, expression for expression: every query lists / visits the
// same entries in the same order (tests/ocean_sense_tests.hpp, food_grid,
// checks it against SpatialGrid itself). The geometry (width, height, cols,
// rows, 1 / cell) is the persistent state the harness hashes, as it hashed
// SpatialGrid's; the entries are scratch rebuilt every step.
// ----------------------------------------------------------------------------
#include <algorithm>
#include <cmath>
#include <vector>
#include "../core/vec2.hpp"

class OceanFoodGrid {
public:
    void init(int width, int height, float cellSize) {
        width_   = width;
        height_  = height;
        invCell_ = 1.0f / cellSize;
        cols_    = static_cast<int>(width  * invCell_) + 1;
        rows_    = static_cast<int>(height * invCell_) + 1;
        const std::size_t cells = static_cast<std::size_t>(cols_) * rows_;
        start_.assign(cells + 1, 0);
        cursor_.assign(cells, 0);
        staged_.clear();
        resizeEntries(0);
    }

    // Forget the entries but keep every array's capacity.
    void clear() { staged_.clear(); }

    // Item f (an OceanFood) at index: SpatialGrid::insert(f.pos, index), with
    // its payload.
    template <class Food>
    void insert(const Food& f, int index) {
        const Vec2 pos = f.pos;
        if (std::isnan(pos.x) || std::isnan(pos.y)) return;
        int cx = clampCol(static_cast<int>(pos.x * invCell_));
        int cy = clampRow(static_cast<int>(pos.y * invCell_));
        Staged s;
        s.pos = pos;
        s.depth = f.depth;
        s.size = f.size;
#ifdef EVO_OCEAN_EYE
        s.eyeCon = f.eyeCon;
#endif
        s.index = index;
        s.cell = cy * cols_ + cx;
        staged_.push_back(s);
    }

    // Count per cell, prefix-sum into each cell's start, place in insertion order.
    void build() {
        std::fill(start_.begin(), start_.end(), 0);
        for (const Staged& s : staged_) ++start_[s.cell + 1];
        const std::size_t cells = cursor_.size();
        for (std::size_t c = 0; c < cells; ++c) {
            start_[c + 1] += start_[c];
            cursor_[c] = start_[c];
        }
        resizeEntries(staged_.size());
        for (const Staged& s : staged_) {
            const int k = cursor_[s.cell]++;
            x_[k] = s.pos.x;
            y_[k] = s.pos.y;
            z_[k] = s.depth;
            size_[k] = s.size;
#ifdef EVO_OCEAN_EYE
            con_[k] = s.eyeCon;
#endif
            index_[k] = s.index;
        }
    }

    // The entries by slot (cell order): the position given to insert(), the
    // payload as at the build, the item's index in OceanWorld::food.
    const float* xs() const { return x_.data(); }
    const float* ys() const { return y_.data(); }
    const float* depths() const { return z_.data(); }
    const float* sizes() const { return size_.data(); }
#ifdef EVO_OCEAN_EYE
    const float* eyeCons() const { return con_.data(); }
#endif
    const int* indices() const { return index_.data(); }
    int entryCount() const { return (int)index_.size(); }

    // SpatialGrid::queryRadius / queryRadiusWrapped: the indices, in scan order.
    void queryRadius(Vec2 center, float radius, std::vector<int>& results) const {
        const int minCx = clampCol(static_cast<int>((center.x - radius) * invCell_));
        const int maxCx = clampCol(static_cast<int>((center.x + radius) * invCell_));
        const int minCy = clampRow(static_cast<int>((center.y - radius) * invCell_));
        const int maxCy = clampRow(static_cast<int>((center.y + radius) * invCell_));
        collect(center.x, center.y, radius * radius, minCx, maxCx, minCy, maxCy, results);
    }
    void queryRadiusWrapped(Vec2 center, float radius, std::vector<int>& results) const {
        forEachImage(center, radius, [&](float cx0, float cy0, int minCx, int maxCx, int minCy, int maxCy) {
            collect(cx0, cy0, radius * radius, minCx, maxCx, minCy, maxCy, results);
        });
    }

    // SpatialGrid::forEachInRadius / forEachInRadiusWrapped, handing the visitor
    // the slot of each entry those visit, in the same order: bounded,
    // f(k, dx, dy, q2) with the 2-D test's own offset from the query centre
    // (dx = x[k] - center.x, dy likewise) and q2 = dx * dx + dy * dy, the sum it
    // compared (q2 <= r²); wrapped, f(k).
    template <class F>
    void forEachInRadius(Vec2 center, float radius, F&& f) const {
        const float r2 = radius * radius;
        const float* X = x_.data();
        const float* Y = y_.data();
        forEachRun(center, radius, [&](int b, int e) {
            for (int k = b; k < e; ++k) {
                const float dx = X[k] - center.x;
                const float dy = Y[k] - center.y;
                const float q2 = dx * dx + dy * dy;
                if (q2 <= r2) f(k, dx, dy, q2);
            }
        });
    }
    template <class F>
    void forEachInRadiusWrapped(Vec2 center, float radius, F&& f) const {
        const float r2 = radius * radius;
        const float* X = x_.data();
        const float* Y = y_.data();
        forEachRunWrapped(center, radius, [&](int b, int e, float cx0, float cy0) {
            for (int k = b; k < e; ++k) {
                const float dx = X[k] - cx0;
                const float dy = Y[k] - cy0;
                if (dx * dx + dy * dy <= r2) f(k);
            }
        });
    }
    // The wrapped query's images (SpatialGrid's nine, in its order, each
    // clipped, the empty ones skipped), row by row: f(b, e, cx0, cy0) for the
    // slots [b, e) of each row's run of cells and the image's translated centre
    // (cx0, cy0) — the centre its 2-D test is against. A cell can fall in two
    // images' runs (a world under 2 cells wide past R); its entries are then
    // candidates twice, within r of at most one of the two centres.
    template <class F>
    void forEachRunWrapped(Vec2 center, float radius, F&& f) const {
        forEachImage(center, radius, [&](float cx0, float cy0, int minCx, int maxCx, int minCy, int maxCy) {
            for (int cy = minCy; cy <= maxCy; ++cy) {
                const std::size_t rowBase = static_cast<std::size_t>(cy) * cols_;
                f(start_[rowBase + minCx], start_[rowBase + maxCx + 1], cx0, cy0);
            }
        });
    }
    // The bounded query's cell rectangle, row by row: f(b, e) for the slots
    // [b, e) of each row's run of cells (SpatialGrid::forEachInRadius's runs,
    // before its 2-D test), top row first.
    template <class F>
    void forEachRun(Vec2 center, float radius, F&& f) const {
        const int minCx = clampCol(static_cast<int>((center.x - radius) * invCell_));
        const int maxCx = clampCol(static_cast<int>((center.x + radius) * invCell_));
        const int minCy = clampRow(static_cast<int>((center.y - radius) * invCell_));
        const int maxCy = clampRow(static_cast<int>((center.y + radius) * invCell_));
        for (int cy = minCy; cy <= maxCy; ++cy) {
            const std::size_t rowBase = static_cast<std::size_t>(cy) * cols_;
            f(start_[rowBase + minCx], start_[rowBase + maxCx + 1]);
        }
    }

private:
    struct Staged {
        Vec2  pos;
        float depth, size;
#ifdef EVO_OCEAN_EYE
        float eyeCon;
#endif
        int   index, cell;
    };

    int   width_   = 0;
    int   height_  = 0;
    int   cols_    = 0;
    int   rows_    = 0;
    float invCell_ = 1.0f;
    std::vector<int>    start_;    // cells + 1: cell c is slots [start_[c], start_[c + 1])
    std::vector<int>    cursor_;   // build() scratch, one per cell
    std::vector<Staged> staged_;   // this step's inserts, in insertion order
    // The entries, by slot (ordered by cell): position, payload, index.
    std::vector<float>  x_, y_, z_, size_;
#ifdef EVO_OCEAN_EYE
    std::vector<float>  con_;
#endif
    std::vector<int>    index_;

    int clampCol(int c) const { return c < 0 ? 0 : (c >= cols_ ? cols_ - 1 : c); }
    int clampRow(int r) const { return r < 0 ? 0 : (r >= rows_ ? rows_ - 1 : r); }

    void resizeEntries(std::size_t n) {
        x_.resize(n); y_.resize(n); z_.resize(n); size_.resize(n);
#ifdef EVO_OCEAN_EYE
        con_.resize(n);
#endif
        index_.resize(n);
    }

    // SpatialGrid's wrapped query: the nine images in its order, each query
    // rect translated, clipped against the world and skipped when empty;
    // f(cx0, cy0, minCx, maxCx, minCy, maxCy) per image kept.
    template <class F>
    void forEachImage(Vec2 center, float radius, F&& f) const {
        static const int order[9][2] = {
            {0, 0}, {-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};
        const float W = static_cast<float>(width_), H = static_cast<float>(height_);
        for (const auto& im : order) {
            const float cx0 = center.x + im[0] * W;
            const float cy0 = center.y + im[1] * H;
            const float lox = cx0 - radius, hix = cx0 + radius;
            const float loy = cy0 - radius, hiy = cy0 + radius;
            if (hix < 0.0f || lox >= W || hiy < 0.0f || loy >= H) continue;
            const int minCx = clampCol(static_cast<int>(std::max(lox, 0.0f) * invCell_));
            const int maxCx = clampCol(static_cast<int>(std::min(hix, W) * invCell_));
            const int minCy = clampRow(static_cast<int>(std::max(loy, 0.0f) * invCell_));
            const int maxCy = clampRow(static_cast<int>(std::min(hiy, H) * invCell_));
            f(cx0, cy0, minCx, maxCx, minCy, maxCy);
        }
    }

    // SpatialGrid::collect: every candidate's index written, the cursor advanced
    // on a hit.
    void collect(float x0, float y0, float r2, int minCx, int maxCx, int minCy, int maxCy,
                 std::vector<int>& results) const {
        std::size_t candidates = 0;
        for (int cy = minCy; cy <= maxCy; ++cy) {
            const std::size_t rowBase = static_cast<std::size_t>(cy) * cols_;
            candidates += static_cast<std::size_t>(start_[rowBase + maxCx + 1] - start_[rowBase + minCx]);
        }
        const std::size_t first = results.size();
        results.resize(first + candidates);
        int* out = results.data() + first;
        std::size_t n = 0;
        for (int cy = minCy; cy <= maxCy; ++cy) {
            const std::size_t rowBase = static_cast<std::size_t>(cy) * cols_;
            const int e = start_[rowBase + maxCx + 1];
            for (int k = start_[rowBase + minCx]; k < e; ++k) {
                const float dx = x_[k] - x0;
                const float dy = y_[k] - y0;
                out[n] = index_[k];
                n += (dx * dx + dy * dy <= r2);
            }
        }
        results.resize(first + n);
    }
};
