#pragma once

#include "Types.h"

namespace TFM::IconLayer
{
    enum class PresentationState : std::uint8_t
    {
        kPending,
        kReady,
        kFailed
    };

    struct Tile
    {
        ItemKey key{};
        Rect bounds{};
        Rect preview{};
        bool hovered{ false };
        bool dropTarget{ false };
        EquipState equipState{ EquipState::kNone };
        Rect equipIndicator{};

        friend bool operator==(const Tile&, const Tile&) = default;
    };

    bool Register();
    void SetActive(bool active, bool iconsEnabled);
    void Reconcile(const std::vector<FavoriteItem>& items);
    void SubmitLayout(
        float screenWidth,
        float screenHeight,
        std::span<const Tile> tiles,
        std::optional<Rect> dragGhost,
        std::optional<ItemKey> dragGhostItem,
        std::optional<Rect> dragGhostEquipIndicator,
        float backdropAlpha,
        float tileAlpha,
        bool useFrames,
        float borderScale,
        float frameScale);
    [[nodiscard]] bool HandleFavoritesMenuOpened();
    [[nodiscard]] PresentationState GetPresentationState();
}
