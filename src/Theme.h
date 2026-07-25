#pragma once

#include <algorithm>
#include <cstdint>

namespace TFM::Theme
{
    inline constexpr std::uint32_t kBackdropColor = 0x000000;
    inline constexpr float kCornerRadius = 1.5f;
    inline constexpr float kHoverAlphaBoost = 0.08f;
    inline constexpr float kActiveAlphaBoost = 0.04f;
    inline constexpr float kDropTargetAlphaBoost = 0.22f;

    [[nodiscard]] constexpr float TileAlpha(float base, bool hovered, bool active, bool dropTarget) noexcept
    {
        float boost = 0.0f;
        if (dropTarget) {
            boost = kDropTargetAlphaBoost;
        } else if (hovered) {
            boost = kHoverAlphaBoost;
        } else if (active) {
            boost = kActiveAlphaBoost;
        }
        return std::clamp(base + boost, 0.0f, 1.0f);
    }
}
