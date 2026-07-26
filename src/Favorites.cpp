#include "Favorites.h"
#include "InventoryIdentity.h"

namespace TFM
{
    namespace
    {
        [[nodiscard]] std::string CategoryFor(const RE::TESForm& form)
        {
            switch (form.GetFormType()) {
            case RE::FormType::Weapon:
                return "WEAPON";
            case RE::FormType::Armor:
                return "ARMOR";
            case RE::FormType::Ammo:
                return "AMMUNITION";
            case RE::FormType::AlchemyItem:
                return "CONSUMABLE";
            case RE::FormType::Scroll:
                return "SCROLL";
            case RE::FormType::Light:
                return "LIGHT";
            case RE::FormType::Spell:
                return "SPELL";
            case RE::FormType::Shout:
                return "SHOUT";
            default:
                return "ITEM";
            }
        }

        [[nodiscard]] std::string FormName(const RE::TESForm& form)
        {
            const auto name = form.GetName();
            if (name && name[0] != '\0') {
                return name;
            }
            return std::format("Favorite {:08X}", form.GetFormID());
        }

        [[nodiscard]] RE::TESBoundObject* PreviewObject(RE::TESForm* form)
        {
            if (!form) {
                return nullptr;
            }

            const auto hasModel = [](RE::TESBoundObject* object) {
                if (!object) {
                    return false;
                }
                const auto model = object->As<RE::TESModel>();
                const auto path = model ? model->GetModel() : nullptr;
                return path && path[0] != '\0';
            };

            if (auto spell = form->As<RE::SpellItem>()) {
                if (hasModel(spell->GetMenuDisplayObject())) {
                    return spell;
                }

                for (const auto effect : spell->effects) {
                    const auto baseEffect = effect ? effect->baseEffect : nullptr;
                    if (!baseEffect) {
                        continue;
                    }

                    const std::array candidates{
                        baseEffect->GetMenuDisplayObject(),
                        static_cast<RE::TESBoundObject*>(baseEffect->data.castingArt),
                        static_cast<RE::TESBoundObject*>(baseEffect->data.projectileBase),
                        static_cast<RE::TESBoundObject*>(baseEffect->data.hitEffectArt),
                        static_cast<RE::TESBoundObject*>(baseEffect->data.enchantEffectArt)
                    };
                    const auto candidate = std::ranges::find_if(candidates, hasModel);
                    if (candidate != candidates.end()) {
                        return *candidate;
                    }
                }

                return nullptr;
            }
            if (auto shout = form->As<RE::TESShout>()) {
                if (auto displayObject = shout->GetMenuDisplayObject(); hasModel(displayObject)) {
                    return displayObject;
                }
                for (const auto& variation : shout->variations) {
                    if (auto preview = PreviewObject(variation.spell)) {
                        return preview;
                    }
                }
                return nullptr;
            }
            if (auto bound = form->As<RE::TESBoundObject>()) {
                return bound;
            }
            return nullptr;
        }

        [[nodiscard]] bool MatchesForm(const RE::TESForm* equipped, const RE::TESForm& form)
        {
            return equipped && equipped->GetFormID() == form.GetFormID();
        }

        [[nodiscard]] EquipState HandEquipState(bool left, bool right)
        {
            if (left && right) {
                return EquipState::kBoth;
            }
            if (left) {
                return EquipState::kLeft;
            }
            return right ? EquipState::kRight : EquipState::kNone;
        }

        [[nodiscard]] EquipState InventoryEquipState(
            RE::PlayerCharacter& player,
            RE::TESForm& form,
            const InventoryIdentity::Match& instances)
        {
            const bool wornLeft = instances.wornLeftExtraList != nullptr;
            const bool wornRight = instances.wornRightExtraList != nullptr;
            if (!wornLeft && !wornRight) {
                return EquipState::kNone;
            }

            const bool leftMatches = MatchesForm(player.GetEquippedObject(true), form);
            const bool rightMatches = MatchesForm(player.GetEquippedObject(false), form);
            if (wornLeft && wornRight && leftMatches && rightMatches) {
                return EquipState::kBoth;
            }
            if (wornLeft) {
                return EquipState::kLeft;
            }
            if (rightMatches) {
                return EquipState::kRight;
            }
            if (leftMatches) {
                return EquipState::kLeft;
            }
            return EquipState::kEquipped;
        }

        [[nodiscard]] EquipState MagicEquipState(RE::PlayerCharacter& player, RE::TESForm& form)
        {
            const auto hands = HandEquipState(
                MatchesForm(player.GetEquippedObject(true), form),
                MatchesForm(player.GetEquippedObject(false), form));
            if (hands != EquipState::kNone) {
                return hands;
            }

            return MatchesForm(player.GetActorRuntimeData().selectedPower, form) ?
                EquipState::kEquipped :
                EquipState::kNone;
        }

        [[nodiscard]] std::int8_t HotkeyNumber(const RE::ExtraDataList& extraList)
        {
            const auto hotkey = extraList.GetByType<RE::ExtraHotkey>();
            if (!hotkey) {
                return -1;
            }
            const auto raw = hotkey->hotkey.underlying();
            return raw <= 7 ? static_cast<std::int8_t>(raw + 1) : -1;
        }

        [[nodiscard]] std::uint16_t UniqueID(const RE::ExtraDataList& extraList)
        {
            const auto unique = extraList.GetByType<RE::ExtraUniqueID>();
            return unique ? unique->uniqueID : 0;
        }

        [[nodiscard]] RE::ExtraHotkey* FindInventoryHotkey(
            RE::TESObjectREFR::InventoryItemMap& inventory,
            const ItemKey& key)
        {
            for (auto& [object, data] : inventory) {
                const auto& entry = data.second;
                if (!object || object->GetFormID() != key.formID || data.first <= 0 || !entry || !entry->extraLists) {
                    continue;
                }

                for (auto extraList : *entry->extraLists) {
                    if (!extraList) {
                        continue;
                    }
                    auto hotkey = extraList->GetByType<RE::ExtraHotkey>();
                    if (!hotkey || UniqueID(*extraList) != key.uniqueID) {
                        continue;
                    }
                    return hotkey;
                }
            }
            return nullptr;
        }

        bool UnbindInventorySlot(
            RE::TESObjectREFR::InventoryItemMap& inventory,
            std::uint8_t slot)
        {
            bool changed = false;
            for (auto& [object, data] : inventory) {
                const auto& entry = data.second;
                if (!object || data.first <= 0 || !entry || !entry->extraLists) {
                    continue;
                }
                for (auto extraList : *entry->extraLists) {
                    auto hotkey = extraList ? extraList->GetByType<RE::ExtraHotkey>() : nullptr;
                    if (hotkey && hotkey->hotkey.underlying() == slot) {
                        hotkey->hotkey = RE::ExtraHotkey::Hotkey::kUnbound;
                        changed = true;
                    }
                }
            }
            return changed;
        }

        void AddPhysicalFavorites(
            std::vector<FavoriteItem>& result,
            std::unordered_set<ItemKey, ItemKeyHash>& seen,
            RE::PlayerCharacter& player)
        {
            auto inventory = player.GetInventory();
            for (auto& [object, data] : inventory) {
                auto& entry = data.second;
                if (!object || data.first <= 0 || !entry || !entry->IsFavorited()) {
                    continue;
                }

                bool foundFavoriteList = false;
                if (entry->extraLists) {
                    for (auto extraList : *entry->extraLists) {
                        if (!extraList || !extraList->HasType<RE::ExtraHotkey>()) {
                            continue;
                        }
                        foundFavoriteList = true;
                        const ItemKey key{ object->GetFormID(), UniqueID(*extraList) };
                        if (!seen.insert(key).second) {
                            continue;
                        }

                        auto name = extraList->GetDisplayName(object);
                        FavoriteItem favorite;
                        favorite.key = key;
                        favorite.form = object;
                        favorite.boundObject = object;
                        favorite.name = name && name[0] != '\0' ? name : FormName(*object);
                        favorite.category = CategoryFor(*object);
                        const auto instances = InventoryIdentity::Inspect(*entry, data.first, key);
                        favorite.count = std::max(1, instances.count);
                        favorite.hotkey = HotkeyNumber(*extraList);
                        favorite.equipState = InventoryEquipState(player, *object, instances);
                        result.push_back(std::move(favorite));
                    }
                }

                if (!foundFavoriteList) {
                    const ItemKey key{ object->GetFormID(), 0 };
                    if (seen.insert(key).second) {
                        const auto instances = InventoryIdentity::Inspect(*entry, data.first, key);
                        result.push_back({
                            key,
                            object,
                            object,
                            FormName(*object),
                            CategoryFor(*object),
                            data.first,
                            -1,
                            InventoryEquipState(player, *object, instances) });
                    }
                }
            }
        }

        void AddMagicFavorites(
            std::vector<FavoriteItem>& result,
            std::unordered_set<ItemKey, ItemKeyHash>& seen,
            RE::PlayerCharacter& player)
        {
            const auto magicFavorites = RE::MagicFavorites::GetSingleton();
            if (!magicFavorites) {
                return;
            }

            for (auto form : magicFavorites->spells) {
                if (!form) {
                    continue;
                }
                const ItemKey key{ form->GetFormID(), 0 };
                if (!seen.insert(key).second) {
                    continue;
                }

                std::int8_t hotkey = -1;
                for (std::uint32_t index = 0; index < magicFavorites->hotkeys.size(); ++index) {
                    if (magicFavorites->hotkeys[index] == form) {
                        hotkey = static_cast<std::int8_t>(index + 1);
                        break;
                    }
                }

                FavoriteItem favorite;
                favorite.key = key;
                favorite.form = form;
                favorite.boundObject = PreviewObject(form);
                favorite.name = FormName(*form);
                favorite.category = CategoryFor(*form);
                favorite.hotkey = hotkey;
                favorite.equipState = MagicEquipState(player, *form);
                result.push_back(std::move(favorite));
            }
        }
    }

    Favorites& Favorites::GetSingleton()
    {
        static Favorites singleton;
        return singleton;
    }

    void Favorites::Refresh()
    {
        items_.clear();
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) {
            return;
        }

        std::unordered_set<ItemKey, ItemKeyHash> seen;
        AddPhysicalFavorites(items_, seen, *player);
        AddMagicFavorites(items_, seen, *player);
        std::ranges::sort(items_, [](const FavoriteItem& left, const FavoriteItem& right) {
            if (left.category != right.category) {
                return left.category < right.category;
            }
            if (left.name != right.name) {
                return left.name < right.name;
            }
            if (left.key.formID != right.key.formID) {
                return left.key.formID < right.key.formID;
            }
            return left.key.uniqueID < right.key.uniqueID;
        });
    }

    bool Favorites::AssignHotkey(const ItemKey& key, std::int8_t hotkey)
    {
        if (hotkey < 1 || hotkey > 8) {
            return false;
        }

        const auto item = Find(key);
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!item || !item->form || !player) {
            return false;
        }

        const auto slot = static_cast<std::uint8_t>(hotkey - 1);
        auto inventory = player->GetInventory();
        auto inventoryHotkey = FindInventoryHotkey(inventory, key);

        auto magicFavorites = RE::MagicFavorites::GetSingleton();
        const bool magicFavorite = magicFavorites &&
            std::ranges::find(magicFavorites->spells, item->form) != magicFavorites->spells.end();
        if (!inventoryHotkey && !magicFavorite) {
            return false;
        }

        const bool alreadyAssigned = inventoryHotkey ?
            inventoryHotkey->hotkey.underlying() == slot :
            slot < magicFavorites->hotkeys.size() && magicFavorites->hotkeys[slot] == item->form;

        bool inventoryChanged = UnbindInventorySlot(inventory, slot);
        if (magicFavorites && slot < magicFavorites->hotkeys.size()) {
            magicFavorites->hotkeys[slot] = nullptr;
        }

        if (!alreadyAssigned) {
            if (inventoryHotkey) {
                inventoryHotkey->hotkey = static_cast<RE::ExtraHotkey::Hotkey>(slot);
                inventoryChanged = true;
            } else {
                for (auto& assigned : magicFavorites->hotkeys) {
                    if (assigned == item->form) {
                        assigned = nullptr;
                    }
                }
                if (magicFavorites->hotkeys.size() < 8) {
                    magicFavorites->hotkeys.resize(8, nullptr);
                }
                magicFavorites->hotkeys[slot] = item->form;
            }
        }

        if (inventoryChanged) {
            if (const auto changes = player->GetInventoryChanges(true)) {
                changes->changed = true;
            }
        }
        return true;
    }

    const std::vector<FavoriteItem>& Favorites::Items() const noexcept
    {
        return items_;
    }

    const FavoriteItem* Favorites::Find(const ItemKey& key) const
    {
        const auto match = std::ranges::find(items_, key, &FavoriteItem::key);
        return match == items_.end() ? nullptr : std::addressof(*match);
    }

}
