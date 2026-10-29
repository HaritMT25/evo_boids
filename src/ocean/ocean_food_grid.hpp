#pragma once
// ----------------------------------------------------------------------------
// OceanFoodGrid — the Ocean's food grid: SpatialGrid (src/core/spatial_grid.hpp,
// Classic's, untouched) whose entries also carry what the per-fish scans read
// of an item: besides the position given to insert() and the item's index, its
// depth, its size (OceanFood::size, the mouth gate's) and, in the eye build, its
// eyeCon — copied once, at the step's grid build, so the smell and the eye read
// the entry they are visiting instead of food[index] at a scattered address.
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
    struct Entry {
        Vec2  pos;      // the position given to insert()
        float depth;    // the item's depth, size and (eye build) contrast, as at the build
        float size;
#ifdef EVO_OCEAN_EYE
        float eyeCon;
#endif
        int   index;    // its index in OceanWorld::food
    };

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
        entries_.clear();
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
        Entry e;
        e.pos = pos;
        e.depth = f.depth;
        e.size = f.size;
#ifdef EVO_OCEAN_EYE
        e.eyeCon = f.eyeCon;
#endif
        e.index = index;
        staged_.push_back({e, cy * cols_ + cx});
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
        entries_.resize(staged_.size());
        for (const Staged& s : staged_) entries_[cursor_[s.cell]++] = s.e;
    }

    // SpatialGrid::queryRadius / queryRadiusWrapped: the indices, in scan order.
    void queryRadius(Vec2 center, float radius, std::vector<int>& results) const {
        const int minCx = clampCol(static_cast<int>((center.x - radius) * invCell_));
        const int maxCx = clampCol(static_cast<int>((center.x + radius) * invCell_));
        const int minCy = clampRow(static_cast<int>((center.y - radius) * invCell_));
        const int maxCy = clampRow(static_cast<int>((center.y + radius) * invCell_));
        collect(center.x, center.y, radius * radius, minCx, maxCx, minCy, maxCy, results);
    }
    void queryRadiusWrapped(Vec2 center, float radius, std::vector<int>& results) const {
        static const int order[9][2] = {
            {0, 0}, {-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};
        const float r2 = radius * radius;
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
            collect(cx0, cy0, r2, minCx, maxCx, minCy, maxCy, results);
        }
    }

    // SpatialGrid::forEachInRadius / forEachInRadiusWrapped, handing the visitor
    // the whole entry: f(const Entry&) for exactly the entries those visit, in
    // the same order.
    template <class F>
    void forEachInRadius(Vec2 center, float radius, F&& f) const {
        const float r2 = radius * radius;
        int minCx = clampCol(static_cast<int>((center.x - radius) * invCell_));
        int maxCx = clampCol(static_cast<int>((center.x + radius) * invCell_));
        int minCy = clampRow(static_cast<int>((center.y - radius) * invCell_));
        int maxCy = clampRow(static_cast<int>((center.y + radius) * invCell_));
        for (int cy = minCy; cy <= maxCy; ++cy) {
            const std::size_t rowBase = static_cast<std::size_t>(cy) * cols_;
            const Entry* e   = entries_.data() + start_[rowBase + minCx];
            const Entry* end = entries_.data() + start_[rowBase + maxCx + 1];
            for (; e != end; ++e) {
                float dx = e->pos.x - center.x;
                float dy = e->pos.y - center.y;
                if (dx * dx + dy * dy <= r2) f(*e);
            }
        }
    }
    template <class F>
    void forEachInRadiusWrapped(Vec2 center, float radius, F&& f) const {
        static const int order[9][2] = {
            {0, 0}, {-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};
        const float r2 = radius * radius;
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
            for (int cy = minCy; cy <= maxCy; ++cy) {
                const std::size_t rowBase = static_cast<std::size_t>(cy) * cols_;
                const Entry* e   = entries_.data() + start_[rowBase + minCx];
                const Entry* end = entries_.data() + start_[rowBase + maxCx + 1];
                for (; e != end; ++e) {
                    const float dx = e->pos.x - cx0;
                    const float dy = e->pos.y - cy0;
                    if (dx * dx + dy * dy <= r2) f(*e);
                }
            }
        }
    }

private:
    struct Staged {
        Entry e;
        int   cell;
    };

    int   width_   = 0;
    int   height_  = 0;
    int   cols_    = 0;
    int   rows_    = 0;
    float invCell_ = 1.0f;
    std::vector<int>    start_;    // cells + 1: cell c is entries_[start_[c], start_[c + 1])
    std::vector<int>    cursor_;   // build() scratch, one per cell
    std::vector<Staged> staged_;   // this step's inserts, in insertion order
    std::vector<Entry>  entries_;  // the same entries, ordered by cell

    int clampCol(int c) const { return c < 0 ? 0 : (c >= cols_ ? cols_ - 1 : c); }
    int clampRow(int r) const { return r < 0 ? 0 : (r >= rows_ ? rows_ - 1 : r); }

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
            const Entry* e   = entries_.data() + start_[rowBase + minCx];
            const Entry* end = entries_.data() + start_[rowBase + maxCx + 1];
            for (; e != end; ++e) {
                const float dx = e->pos.x - x0;
                const float dy = e->pos.y - y0;
                out[n] = e->index;
                n += (dx * dx + dy * dy <= r2);
            }
        }
        results.resize(first + n);
    }
};
