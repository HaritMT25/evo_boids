#pragma once
#include <SFML/Graphics.hpp>
#include <string>
#include <cstdint>
#include <vector>
#include "render/renderer.hpp"

// ============================================================
// SfmlRenderer — the ONLY place in the project that names sf::*.
// Owns the window, textures, font and frame clock, and translates between
// the SFML-free core types and SFML at the draw/input boundary.
// ============================================================
class SfmlRenderer : public IRenderer {
public:
    SfmlRenderer(int width, int height, const std::string& title);

    bool  isOpen() const override;
    float frameDelta() override;
    InputEvents poll() override;
    void  render(const World& world, const HudInfo& hud) override;

private:
    void drawCreatures(const std::vector<Creature>& creatures);
    void drawFood(const std::vector<Food>& food);
    void drawPredators(const std::vector<Predator>& predators);
    void drawCurrentField(const World& world);   // faint flow-field overlay
    void drawHud(const HudInfo& hud);

    // Click-to-inspect: resolve a pending click to the nearest creature, then draw
    // its neural net "firing live" in a translucent top-right panel.
    void resolveSelection(const World& world);
    const Creature* findSelected(const World& world) const;
    void drawSelectionRing(const Creature& c);
    void drawNeuralOverlay(const Creature& c);

    sf::RenderWindow window_;
    sf::Texture circleTexture_;
    sf::Texture bgTexture_;
    sf::Sprite  bgSprite_;
    sf::Font    font_;
    bool hasBg_   = false;
    bool hasFont_ = false;

    sf::Clock frameClock_;

    // edge-detection state for keyboard input
    bool spaceWasPressed_ = false;
    bool upWasPressed_    = false;
    bool downWasPressed_  = false;

    // Selection state (live neural inspector).
    std::uint64_t selectedId_   = 0;      // 0 = nothing selected
    bool          hasPendingClick_ = false;
    int           clickX_ = 0, clickY_ = 0;
};
