#pragma once

#include "Types.h"

namespace TFM
{
    struct LayoutNode
    {
        ItemKey item{};
        Axis axis{ Axis::kVertical };
        float ratio{ 0.5f };
        std::unique_ptr<LayoutNode> first;
        std::unique_ptr<LayoutNode> second;

        [[nodiscard]] bool IsLeaf() const noexcept { return !first && !second; }
    };

    struct LeafRect
    {
        ItemKey item{};
        Rect rect{};
    };

    enum class DropPosition : std::uint8_t
    {
        kCenter,
        kLeft,
        kRight,
        kTop,
        kBottom
    };

    struct LayoutDrop
    {
        ItemKey source{};
        ItemKey target{};
        DropPosition position{ DropPosition::kCenter };
    };

    class Layout
    {
    public:
        static Layout& GetSingleton();

        void Reconcile(const std::vector<FavoriteItem>& favorites);
        void Clear();
        [[nodiscard]] std::vector<LeafRect> Calculate(const Rect& bounds, float gap);
        [[nodiscard]] std::optional<std::vector<LeafRect>> CalculatePreview(
            const Rect& bounds,
            float gap,
            const LayoutDrop& drop);
        bool ApplyDrop(const LayoutDrop& drop);

        bool Save(SKSE::SerializationInterface* serialization) const;
        bool Load(SKSE::SerializationInterface* serialization);

    private:
        Layout() = default;

        std::unique_ptr<LayoutNode> root_;
        mutable std::mutex mutex_;
    };
}
