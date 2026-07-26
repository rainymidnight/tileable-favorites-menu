#pragma once

#include "Types.h"

namespace TFM::InventoryIdentity
{
    // ExtraDataLists are owned by the game. Treat this as a short-lived view and
    // reacquire it after every operation that can mutate the actor's inventory.
    struct Match
    {
        RE::ExtraDataList* equipExtraList{ nullptr };
        RE::ExtraDataList* wornLeftExtraList{ nullptr };
        RE::ExtraDataList* wornRightExtraList{ nullptr };
        std::int32_t count{ 0 };
    };

    [[nodiscard]] inline Match Inspect(
        const RE::InventoryEntryData& entry,
        std::int32_t totalCount,
        const ItemKey& key)
    {
        Match match;
        if (!entry.extraLists) {
            if (key.uniqueID == 0) {
                match.count = totalCount;
            }
            return match;
        }

        bool foundFavorite = false;
        std::int32_t listedCount = 0;
        for (auto extraList : *entry.extraLists) {
            if (!extraList) {
                continue;
            }

            const auto listCount = std::max<std::int32_t>(1, extraList->GetCount());
            listedCount += listCount;
            const auto unique = extraList->GetByType<RE::ExtraUniqueID>();
            const bool matches = key.uniqueID == 0 ?
                unique == nullptr :
                unique && unique->uniqueID == key.uniqueID;
            if (!matches) {
                continue;
            }

            match.count += listCount;
            const bool wornLeft = extraList->HasType<RE::ExtraWornLeft>();
            const bool wornRight = extraList->HasType<RE::ExtraWorn>();
            if (wornLeft && !match.wornLeftExtraList) {
                match.wornLeftExtraList = extraList;
            }
            if (wornRight && !match.wornRightExtraList) {
                match.wornRightExtraList = extraList;
            }

            const bool firstFavorite =
                !foundFavorite && extraList->HasType<RE::ExtraHotkey>();
            if (firstFavorite) {
                foundFavorite = true;
            }
            if (!wornLeft && !wornRight &&
                (!match.equipExtraList || firstFavorite)) {
                match.equipExtraList = extraList;
            }
        }

        if (key.uniqueID == 0) {
            match.count += std::max<std::int32_t>(0, totalCount - listedCount);
        }
        return match;
    }
}
