#pragma once
#include <vector>
#include <cmath>
#include "vec2.hpp"

// ============================================================
// SpatialGrid — uniform spatial hash over a fixed [0,width) x [0,height)
// region. Drop-in replacement for the subset of QuadTree that World uses:
//
//     init(width, height, cellSize)   // once, in World::init
//     clear()                         // once per frame (retains capacity)
//     insert(pos, index)              // per entity per frame
//     queryRadius(center, r, out)     // identical RESULT SET to QuadTree
//
// A flat 1-D array of buckets indexed by (cellY * cols + cellX); each bucket
// stores (pos, index) pairs. A radius query touches only the rectangle of
// cells covering the query's bounding box, then distance-tests each point —
// no tree descent, no per-node pointer chasing, and the structure is reused
// across frames instead of being rebuilt with new/delete every tick.
//
// queryRadius returns the same set of indices as QuadTree::queryRadius; only
// the ORDER differs (cell-iteration order vs tree-traversal order). All World
// call sites consume the result order-independently (sums / "set alive=false"
// / nearest-by-strict-<), so the set guarantee is what matters. This is
// asserted directly by the differential test in core_tests.
// ============================================================
class SpatialGrid {
public:
    void init(int width, int height, float cellSize) {
        width_   = width;
        height_  = height;
        invCell_ = 1.0f / cellSize;
        cols_    = static_cast<int>(width  * invCell_) + 1;
        rows_    = static_cast<int>(height * invCell_) + 1;
        buckets_.assign(static_cast<std::size_t>(cols_) * rows_, {});
    }

    // Empty every bucket but keep its allocated capacity, so the steady state
    // does no per-frame heap allocation.
    void clear() {
        for (auto& b : buckets_) b.clear();
    }

    void insert(Vec2 pos, int index) {
        if (std::isnan(pos.x) || std::isnan(pos.y)) return;
        int cx = clampCol(static_cast<int>(pos.x * invCell_));
        int cy = clampRow(static_cast<int>(pos.y * invCell_));
        buckets_[static_cast<std::size_t>(cy) * cols_ + cx].push_back({pos, index});
    }

    void queryRadius(Vec2 center, float radius, std::vector<int>& results) const {
        const float r2 = radius * radius;

        // Cell rectangle covering [center - r, center + r]. floor() is
        // monotonic, so this rectangle is a guaranteed superset of every cell
        // that can hold a point within the radius; the per-point distance test
        // below prunes it to the exact disk.
        int minCx = clampCol(static_cast<int>((center.x - radius) * invCell_));
        int maxCx = clampCol(static_cast<int>((center.x + radius) * invCell_));
        int minCy = clampRow(static_cast<int>((center.y - radius) * invCell_));
        int maxCy = clampRow(static_cast<int>((center.y + radius) * invCell_));

        for (int cy = minCy; cy <= maxCy; ++cy) {
            const std::size_t rowBase = static_cast<std::size_t>(cy) * cols_;
            for (int cx = minCx; cx <= maxCx; ++cx) {
                for (const Entry& e : buckets_[rowBase + cx]) {
                    float dx = e.pos.x - center.x;
                    float dy = e.pos.y - center.y;
                    if (dx * dx + dy * dy <= r2) results.push_back(e.index);
                }
            }
        }
    }

    // Toroidal radius query: the disk wraps across the world seam. Implemented
    // as up to 9 image queries — the query rect is translated by ±width/±height
    // and each translate is clipped against the world before its cells are
    // enumerated, so the padded last column/row is never touched and no cell
    // arithmetic goes modulo. Per-point distances are taken against the SHIFTED
    // centre, which for that image IS the minimum-image distance. With
    // radius < min(width, height)/2 (asserted by the caller's config) a point
    // can be inside the disk in at most one image, so no duplicates are
    // returned. The (0,0) image runs first, so away from the seam the result
    // order is identical to queryRadius.
    void queryRadiusWrapped(Vec2 center, float radius, std::vector<int>& results) const {
        static const int order[9][2] = {
            {0, 0}, {-1, -1}, {-1, 0}, {-1, 1}, {0, -1}, {0, 1}, {1, -1}, {1, 0}, {1, 1}};
        const float r2 = radius * radius;
        const float W = static_cast<float>(width_), H = static_cast<float>(height_);

        for (const auto& im : order) {
            const float cx0 = center.x + im[0] * W;
            const float cy0 = center.y + im[1] * H;
            // Clip the image's query rect against the world; skip empty clips.
            const float lox = cx0 - radius, hix = cx0 + radius;
            const float loy = cy0 - radius, hiy = cy0 + radius;
            if (hix < 0.0f || lox >= W || hiy < 0.0f || loy >= H) continue;

            const int minCx = clampCol(static_cast<int>(std::max(lox, 0.0f) * invCell_));
            const int maxCx = clampCol(static_cast<int>(std::min(hix, W) * invCell_));
            const int minCy = clampRow(static_cast<int>(std::max(loy, 0.0f) * invCell_));
            const int maxCy = clampRow(static_cast<int>(std::min(hiy, H) * invCell_));

            for (int cy = minCy; cy <= maxCy; ++cy) {
                const std::size_t rowBase = static_cast<std::size_t>(cy) * cols_;
                for (int cx = minCx; cx <= maxCx; ++cx) {
                    for (const Entry& e : buckets_[rowBase + cx]) {
                        const float dx = e.pos.x - cx0;
                        const float dy = e.pos.y - cy0;
                        if (dx * dx + dy * dy <= r2) results.push_back(e.index);
                    }
                }
            }
        }
    }

private:
    struct Entry {
        Vec2 pos;
        int  index;
    };

    int   width_   = 0;
    int   height_  = 0;
    int   cols_    = 0;
    int   rows_    = 0;
    float invCell_ = 1.0f;
    std::vector<std::vector<Entry>> buckets_;

    int clampCol(int c) const { return c < 0 ? 0 : (c >= cols_ ? cols_ - 1 : c); }
    int clampRow(int r) const { return r < 0 ? 0 : (r >= rows_ ? rows_ - 1 : r); }
};
