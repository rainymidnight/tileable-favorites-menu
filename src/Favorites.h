#pragma once

#include "Types.h"

namespace TFM
{
    class Favorites
    {
    public:
        static Favorites& GetSingleton();

        void Refresh();
        bool AssignHotkey(const ItemKey& key, std::int8_t hotkey);
        [[nodiscard]] const std::vector<FavoriteItem>& Items() const noexcept;
        [[nodiscard]] const FavoriteItem* Find(const ItemKey& key) const;

    private:
        Favorites() = default;
        std::vector<FavoriteItem> items_;
    };
}
