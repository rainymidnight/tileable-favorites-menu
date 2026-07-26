#include "Actions.h"
#include "InventoryIdentity.h"

namespace TFM::Actions
{
    namespace
    {
        struct InventoryMatch
        {
            RE::TESBoundObject* object{ nullptr };
            InventoryIdentity::Match instances;
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

        [[nodiscard]] InventoryMatch FindInventoryMatch(
            RE::PlayerCharacter& player,
            const ItemKey& key)
        {
            auto inventory = player.GetInventory();
            for (auto& [object, data] : inventory) {
                if (!object || object->GetFormID() != key.formID || data.first <= 0 || !data.second) {
                    continue;
                }

                auto instances = InventoryIdentity::Inspect(*data.second, data.first, key);
                if (instances.count <= 0) {
                    return {};
                }
                return { object, instances };
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

            auto match = FindInventoryMatch(player, item.key);
            if (!match.object || match.instances.count <= 0) {
                return false;
            }

            const auto formType = match.object->GetFormType();
            if (formType == RE::FormType::AlchemyItem || formType == RE::FormType::Ingredient) {
                EquipPhysicalFavorite(
                    *manager,
                    player,
                    *match.object,
                    match.instances.equipExtraList);
                RefreshPhysicalEquipment(player);
                return true;
            }

            if (formType == RE::FormType::Armor || formType == RE::FormType::Light || formType == RE::FormType::Ammo) {
                const auto wornExtraList = match.instances.wornRightExtraList ?
                    match.instances.wornRightExtraList :
                    match.instances.wornLeftExtraList;
                if (wornExtraList) {
                    UnequipPhysicalFavorite(*manager, player, *match.object, wornExtraList);
                } else {
                    EquipPhysicalFavorite(
                        *manager,
                        player,
                        *match.object,
                        match.instances.equipExtraList);
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
            const auto equippedCount =
                static_cast<std::int32_t>(match.instances.wornLeftExtraList != nullptr) +
                static_cast<std::int32_t>(match.instances.wornRightExtraList != nullptr);
            const bool hasUnequippedCopy =
                match.instances.count > equippedCount;

            if (const auto wornExtraList = left ?
                    match.instances.wornLeftExtraList :
                    match.instances.wornRightExtraList;
                wornExtraList) {
                UnequipPhysicalFavorite(*manager, player, *match.object, wornExtraList, slot);
            } else {
                if (const auto oppositeExtraList = left ?
                        match.instances.wornRightExtraList :
                        match.instances.wornLeftExtraList;
                    oppositeExtraList && !hasUnequippedCopy) {
                    UnequipPhysicalFavorite(
                        *manager,
                        player,
                        *match.object,
                        oppositeExtraList,
                        oppositeSlot);

                    // UnequipObject may rebuild ExtraDataLists, so reacquire before equip.
                    match = FindInventoryMatch(player, item.key);
                    if (!match.object || match.instances.count <= 0) {
                        RefreshPhysicalEquipment(player);
                        return true;
                    }
                }
                EquipPhysicalFavorite(
                    *manager,
                    player,
                    *match.object,
                    match.instances.equipExtraList,
                    slot);
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
