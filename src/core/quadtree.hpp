#pragma once
#include <vector>
#include <cmath>
#include "vec2.hpp"

struct AABB {
    Vec2 center;
    Vec2 half;

    bool contains(Vec2 p) const {
        return p.x >= center.x - half.x && p.x <= center.x + half.x &&
               p.y >= center.y - half.y && p.y <= center.y + half.y;
    }

    bool intersects(const AABB& other) const {
        return !(other.center.x - other.half.x > center.x + half.x ||
                 other.center.x + other.half.x < center.x - half.x ||
                 other.center.y - other.half.y > center.y + half.y ||
                 other.center.y + other.half.y < center.y - half.y);
    }
};

// Generic QuadTree that stores indices into an external array
class QuadTree {
    static constexpr int CAPACITY = 8;
    AABB bounds_;
    std::vector<std::pair<Vec2, int>> points_; // position + index
    QuadTree* children_[4] = {nullptr, nullptr, nullptr, nullptr};
    bool divided_ = false;

    void subdivide() {
        float hw = bounds_.half.x * 0.5f;
        float hh = bounds_.half.y * 0.5f;
        float cx = bounds_.center.x;
        float cy = bounds_.center.y;

        children_[0] = new QuadTree({{cx - hw, cy - hh}, {hw, hh}});
        children_[1] = new QuadTree({{cx + hw, cy - hh}, {hw, hh}});
        children_[2] = new QuadTree({{cx - hw, cy + hh}, {hw, hh}});
        children_[3] = new QuadTree({{cx + hw, cy + hh}, {hw, hh}});
        divided_ = true;
    }

public:
    QuadTree(AABB bounds) : bounds_(bounds) {
        points_.reserve(CAPACITY);
    }

    ~QuadTree() {
        for (int i = 0; i < 4; ++i) delete children_[i];
    }

    bool insert(Vec2 pos, int index) {
        if (!bounds_.contains(pos)) return false;
        if (std::isnan(pos.x) || std::isnan(pos.y)) return false;

        if (static_cast<int>(points_.size()) < CAPACITY && !divided_) {
            points_.emplace_back(pos, index);
            return true;
        }

        if (!divided_) {
            subdivide();
            // Re-insert existing points
            for (auto& p : points_) {
                for (int i = 0; i < 4; ++i) {
                    if (children_[i]->insert(p.first, p.second)) break;
                }
            }
            points_.clear();
        }

        for (int i = 0; i < 4; ++i) {
            if (children_[i]->insert(pos, index)) return true;
        }
        return false;
    }

    void queryRadius(Vec2 center, float radius, std::vector<int>& results) const {
        AABB range = {center, {radius, radius}};
        if (!bounds_.intersects(range)) return;

        float r2 = radius * radius;
        for (const auto& [pos, idx] : points_) {
            float dx = pos.x - center.x;
            float dy = pos.y - center.y;
            if (dx * dx + dy * dy <= r2) {
                results.push_back(idx);
            }
        }

        if (divided_) {
            for (int i = 0; i < 4; ++i) {
                children_[i]->queryRadius(center, radius, results);
            }
        }
    }
};
