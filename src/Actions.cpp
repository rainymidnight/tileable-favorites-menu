#include "Actions.h"

namespace TFM::Actions
{
    namespace
    {
        struct InventoryMatch
        {
            RE::TESBoundObject* object{ nullptr };
            RE::ExtraDataList* extraList{ nullptr };
            std::int32_t count{ 0 };
            bool worn{ false };
            bool wornLeft{ false };
            bool wornRight{ false };
        };

        [[nodiscard]] RE::BGSEquipSlot* LeftHandSlot()
        {
            using Function = decltype(&LeftHandSlot);
            static const REL::Relocation<Function> function{ RELOCATION_ID(23150, 23607) };
            return function();
        }

        [[nodiscard]] RE::BGSEquipSlot* RightHandSlot()
        {
            using Function = decltype(&RightHandSlot);
            static const REL::Relocation<Function> function{ RELOCATION_ID(23151, 23608) };
            return function();
        }

        [[nodiscard]] RE::BGSEquipSlot* VoiceSlot()
        {
            using Function = decltype(&VoiceSlot);
            static const REL::Relocation<Function> function{ RELOCATION_ID(23153, 23610) };
            return function();
        }

        void UnequipSpell(
            RE::ActorEquipManager& manager,
            RE::PlayerCharacter& player,
            RE::SpellItem& spell,
            int hand)
        {
            using Function = void (RE::ActorEquipManager::*)(RE::Actor*, RE::SpellItem*, int);
            static const REL::Relocation<Function> function{ RELOCATION_ID(37947, 38903) };
            function(std::addressof(manager), std::addressof(player), std::addressof(spell), hand);
        }

        void UnequipShout(
            RE::ActorEquipManager& manager,
            RE::PlayerCharacter& player,
            RE::TESShout& shout)
        {
            using Function = void (RE::ActorEquipManager::*)(RE::Actor*, RE::TESShout*);
            static const REL::Relocation<Function> function{ RELOCATION_ID(37948, 38904) };
            function(std::addressof(manager), std::addressof(player), std::addressof(shout));
        }

        [[nodiscard]] bool MatchesUniqueID(const RE::ExtraDataList& list, std::uint16_t uniqueID)
        {
            const auto unique = list.GetByType<RE::ExtraUniqueID>();
            return uniqueID == 0 ? unique == nullptr : unique && unique->uniqueID == uniqueID;
        }

        [[nodiscard]] InventoryMatch FindInventoryMatch(
            RE::TESObjectREFR::InventoryItemMap& inventory,
            const ItemKey& key)
        {
            for (auto& [object, data] : inventory) {
                if (!object || object->GetFormID() != key.formID || data.first <= 0 || !data.second) {
                    continue;
                }

                InventoryMatch match{
                    object,
                    nullptr,
                    data.first,
                    data.second->IsWorn(),
                    data.second->IsWorn(true),
                    data.second->IsWorn(false) };
                if (!data.second->extraLists) {
                    return key.uniqueID == 0 ? match : InventoryMatch{};
                }

                RE::ExtraDataList* firstFavorite = nullptr;
                for (auto list : *data.second->extraLists) {
                    if (!list) {
                        continue;
                    }
                    if (!firstFavorite && list->HasType<RE::ExtraHotkey>()) {
                        firstFavorite = list;
                    }
                    if (MatchesUniqueID(*list, key.uniqueID) && list->HasType<RE::ExtraHotkey>()) {
                        match.extraList = list;
                        match.count = std::max(1, list->GetCount());
                        match.wornLeft = list->HasType<RE::ExtraWornLeft>();
                        match.wornRight = list->HasType<RE::ExtraWorn>();
                        match.worn = match.wornLeft || match.wornRight;
                        return match;
                    }
                }
                if (key.uniqueID == 0) {
                    match.extraList = firstFavorite;
                    return match;
                }
            }
            return {};
        }

        [[nodiscard]] bool IsPower(const RE::SpellItem& spell)
        {
            const auto type = spell.GetSpellType();
            return type == RE::MagicSystem::SpellType::kAbility ||
                type == RE::MagicSystem::SpellType::kPower ||
                type == RE::MagicSystem::SpellType::kLesserPower;
        }

        bool ActivateSpell(RE::PlayerCharacter& player, RE::SpellItem& spell, Hand hand)
        {
            const auto manager = RE::ActorEquipManager::GetSingleton();
            if (!manager) {
                return false;
            }

            if (IsPower(spell) || spell.GetEquipSlot() == VoiceSlot()) {
                const auto selected = player.GetActorRuntimeData().selectedPower;
                if (selected && selected->GetFormID() == spell.GetFormID()) {
                    UnequipSpell(*manager, player, spell, 2);
                } else {
                    manager->EquipSpell(std::addressof(player), std::addressof(spell), VoiceSlot());
                }
                return true;
            }

            const bool left = hand == Hand::kLeft;
            const auto equipped = player.GetEquippedObject(left);
            if (equipped && equipped->GetFormID() == spell.GetFormID()) {
                UnequipSpell(*manager, player, spell, left ? 0 : 1);
            } else {
                manager->EquipSpell(
                    std::addressof(player),
                    std::addressof(spell),
                    left ? LeftHandSlot() : RightHandSlot());
            }
            return true;
        }

        bool ActivateShout(RE::PlayerCharacter& player, RE::TESShout& shout)
        {
            const auto manager = RE::ActorEquipManager::GetSingleton();
            if (!manager) {
                return false;
            }
            const auto selected = player.GetActorRuntimeData().selectedPower;
            if (selected && selected->GetFormID() == shout.GetFormID()) {
                UnequipShout(*manager, player, shout);
            } else {
                manager->EquipShout(std::addressof(player), std::addressof(shout));
            }
            return true;
        }

        [[nodiscard]] bool IsTwoHanded(const RE::TESObjectWEAP& weapon)
        {
            const auto type = weapon.GetWeaponType();
            return type == RE::WEAPON_TYPE::kTwoHandSword ||
                type == RE::WEAPON_TYPE::kTwoHandAxe ||
                type == RE::WEAPON_TYPE::kBow ||
                type == RE::WEAPON_TYPE::kCrossbow;
        }

        void EquipPhysicalFavorite(
            RE::ActorEquipManager& manager,
            RE::PlayerCharacter& player,
            RE::TESBoundObject& object,
            RE::ExtraDataList* extraList,
            const RE::BGSEquipSlot* slot = nullptr)
        {
            // FavoritesMenu performs physical equipment changes synchronously. The default
            // ActorEquipManager path queues them, and that queue cannot advance while this
            // replacement menu has the game paused.
            manager.EquipObject(
                std::addressof(player),
                std::addressof(object),
                extraList,
                1,
                slot,
                false,
                false,
                true,
                false);
        }

        void UnequipPhysicalFavorite(
            RE::ActorEquipManager& manager,
            RE::PlayerCharacter& player,
            RE::TESBoundObject& object,
            RE::ExtraDataList* extraList,
            const RE::BGSEquipSlot* slot = nullptr)
        {
            static_cast<void>(manager.UnequipObject(
                std::addressof(player),
                std::addressof(object),
                extraList,
                1,
                slot,
                false,
                false,
                true,
                false));
        }

        void RefreshPhysicalEquipment(RE::PlayerCharacter& player)
        {
            // FavoritesMenu immediately applies the process' pending 3D flags after a
            // physical equipment change. Without this call the inventory state changes
            // while a paused menu is open, but the actor's geometry waits for gameplay
            // to resume before catching up.
            if (const auto process = player.GetActorRuntimeData().currentProcess) {
                process->Update3DModel(std::addressof(player));
            }
        }

        bool ActivateBoundObject(RE::PlayerCharacter& player, const FavoriteItem& item, Hand requestedHand)
        {
            const auto manager = RE::ActorEquipManager::GetSingleton();
            if (!manager) {
                return false;
            }

            auto inventory = player.GetInventory();
            auto match = FindInventoryMatch(inventory, item.key);
            if (!match.object || match.count <= 0) {
                return false;
            }

            const auto formType = match.object->GetFormType();
            if (formType == RE::FormType::AlchemyItem || formType == RE::FormType::Ingredient) {
                EquipPhysicalFavorite(*manager, player, *match.object, match.extraList);
                RefreshPhysicalEquipment(player);
                return true;
            }

            if (formType == RE::FormType::Armor || formType == RE::FormType::Light || formType == RE::FormType::Ammo) {
                if (match.worn) {
                    UnequipPhysicalFavorite(*manager, player, *match.object, match.extraList);
                } else {
                    EquipPhysicalFavorite(*manager, player, *match.object, match.extraList);
                }
                RefreshPhysicalEquipment(player);
                return true;
            }

            bool left = requestedHand == Hand::kLeft;
            if (const auto weapon = match.object->As<RE::TESObjectWEAP>(); weapon && IsTwoHanded(*weapon)) {
                left = false;
            }
            const auto slot = left ? LeftHandSlot() : RightHandSlot();
            const auto oppositeSlot = left ? RightHandSlot() : LeftHandSlot();
            const auto equipped = player.GetEquippedObject(left);
            const auto oppositeEquipped = player.GetEquippedObject(!left);
            const bool selectedInstanceEquipped = match.extraList ?
                (left ? match.wornLeft : match.wornRight) :
                (equipped && equipped->GetFormID() == match.object->GetFormID());
            const bool selectedInstanceInOppositeHand = match.extraList ?
                (left ? match.wornRight : match.wornLeft) :
                (match.count == 1 && oppositeEquipped && oppositeEquipped->GetFormID() == match.object->GetFormID());
            if (selectedInstanceEquipped) {
                UnequipPhysicalFavorite(*manager, player, *match.object, match.extraList, slot);
            } else {
                if (selectedInstanceInOppositeHand) {
                    UnequipPhysicalFavorite(*manager, player, *match.object, match.extraList, oppositeSlot);
                }
                EquipPhysicalFavorite(*manager, player, *match.object, match.extraList, slot);
            }
            RefreshPhysicalEquipment(player);
            return true;
        }

    }

    bool Activate(const FavoriteItem& item, Hand hand)
    {
        const auto player = RE::PlayerCharacter::GetSingleton();
        if (!player || !item.form) {
            return false;
        }

        if (item.form->GetFormType() == RE::FormType::Scroll) {
            return ActivateBoundObject(*player, item, hand);
        }
        if (auto spell = item.form->As<RE::SpellItem>()) {
            return ActivateSpell(*player, *spell, hand);
        }
        if (auto shout = item.form->As<RE::TESShout>()) {
            return ActivateShout(*player, *shout);
        }
        return ActivateBoundObject(*player, item, hand);
    }

}
