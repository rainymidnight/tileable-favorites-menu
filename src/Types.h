#pragma once

namespace TFM
{
    struct ItemKey
    {
        RE::FormID formID{ 0 };
        std::uint16_t uniqueID{ 0 };

        [[nodiscard]] bool IsValid() const noexcept { return formID != 0; }

        friend bool operator==(const ItemKey&, const ItemKey&) = default;
    };

    struct ItemKeyHash
    {
        [[nodiscard]] std::size_t operator()(const ItemKey& key) const noexcept
        {
            return (static_cast<std::size_t>(key.formID) << 16U) ^ key.uniqueID;
        }
    };

    enum class EquipState : std::uint8_t
    {
        kNone,
        kEquipped,
        kLeft,
        kRight,
        kBoth
    };

    struct FavoriteItem
    {
        ItemKey key{};
        RE::TESForm* form{ nullptr };
        RE::TESBoundObject* boundObject{ nullptr };
        std::string name;
        std::string category;
        std::int32_t count{ 1 };
        std::int8_t hotkey{ -1 };
        EquipState equipState{ EquipState::kNone };
    };

    enum class Axis : std::uint8_t
    {
        kVertical,
        kHorizontal
    };

    enum class Direction : std::uint8_t
    {
        kLeft,
        kRight,
        kUp,
        kDown
    };

    struct Rect
    {
        float x{ 0.0f };
        float y{ 0.0f };
        float width{ 0.0f };
        float height{ 0.0f };

        [[nodiscard]] float Right() const noexcept { return x + width; }
        [[nodiscard]] float Bottom() const noexcept { return y + height; }
        [[nodiscard]] float CenterX() const noexcept { return x + width * 0.5f; }
        [[nodiscard]] float CenterY() const noexcept { return y + height * 0.5f; }

        friend bool operator==(const Rect&, const Rect&) = default;
    };
}
