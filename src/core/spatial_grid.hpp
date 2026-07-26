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
