#include "UI.h"

#include "Actions.h"
#include "Config.h"
#include "Favorites.h"
#include "IconLayer.h"
#include "Layout.h"
#include "PreviewCache.h"
#include "SKSEMenuFramework.h"
#include "Theme.h"
#include "logger.h"

namespace TFM::UI
{
    namespace ImGui = ImGuiMCP;
    using namespace ImGuiMCP;

    namespace
    {
        SKSEMenuFramework::Model::WindowInterface* window = nullptr;
        std::unique_ptr<SKSEMenuFramework::Model::InputEvent> inputRegistration;
        std::unique_ptr<SKSEMenuFramework::Model::HudElement> meshPrewarmRegistration;
        ItemKey focusedItem{};
        struct DragState
        {
            ItemKey source{};
            ItemKey target{};
            Rect sourceRect{};
            Rect targetRect{};
            DropPosition position{ DropPosition::kCenter };
            ImGui::ImVec2 pressPosition{};
            ImGui::ImVec2 grabOffset{};
            bool active{ false };
        };
        DragState dragState{};
        std::unordered_map<ItemKey, Rect, ItemKeyHash> animatedRects;
        bool favoritesButtonDown = false;
        bool registered = false;
        bool nativeBlurActive = false;
        bool centerCursorOnNextFrame = false;
        std::uint8_t scaleformCursorRequestCooldown = 0;
        std::atomic_bool replacementAvailable{ false };
        std::atomic_bool equipmentStateChanged{ false };
        std::atomic_uint64_t inputHotkeyTarget{ 0 };
        std::atomic_uint64_t pendingHotkeyTarget{ 0 };
        std::atomic_int pendingHotkey{ 0 };
        bool initialMeshRevealComplete = false;
        bool incrementalMeshRevealAllowed = false;
        bool meshPrewarmReady = false;
        bool meshPrewarmRefreshRequested = true;
        std::uint8_t postActivationRefreshFrames = 0;
        float refreshTimer = 0.0f;
        constexpr std::uint8_t kPostActivationRefreshFrames = 12;
        constexpr float kDragThreshold = 10.0f;
        constexpr float kDropCenterInset = 0.18f;
        constexpr float kLayoutAnimationSpeed = 18.0f;
        constexpr float kItemLabelHorizontalPadding = 12.0f;
        constexpr float kDefaultEquipIndicatorSize = 24.0f;
        constexpr float kDefaultHotkeyIndicatorSize = 26.0f;
        constexpr float kDefaultItemIndicatorGap = 6.0f;
        constexpr std::uint8_t kScaleformCursorRetryFrames = 30;
        class PlayerEquipEventSink final : public RE::BSTEventSink<RE::TESEquipEvent>
        {
        public:
            static PlayerEquipEventSink& GetSingleton()
            {
                static PlayerEquipEventSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::TESEquipEvent* event,
                RE::BSTEventSource<RE::TESEquipEvent>*) override
            {
                const auto player = RE::PlayerCharacter::GetSingleton();
                if (event && player && event->actor.get() == player) {
                    equipmentStateChanged.store(true, std::memory_order_release);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            PlayerEquipEventSink() = default;
        };

        constexpr ImGui::ImU32 kText = IM_COL32(236, 236, 236, 255);
        constexpr ImGui::ImU32 kHotkeyBackground = IM_COL32(12, 14, 18, 210);

        [[nodiscard]] ImGui::ImVec2 Min(const Rect& rect) { return { rect.x, rect.y }; }
        [[nodiscard]] ImGui::ImVec2 Max(const Rect& rect) { return { rect.Right(), rect.Bottom() }; }

        [[nodiscard]] std::uint64_t PackItemKey(const ItemKey& key)
        {
            return (static_cast<std::uint64_t>(key.formID) << 16U) | key.uniqueID;
        }

        [[nodiscard]] ItemKey UnpackItemKey(std::uint64_t packed)
        {
            return {
                static_cast<RE::FormID>(packed >> 16U),
                static_cast<std::uint16_t>(packed & 0xFFFFU)
            };
        }

        void SetNativeBlur(bool enabled)
        {
            if (enabled == nativeBlurActive) {
                return;
            }
            const auto blur = RE::UIBlurManager::GetSingleton();
            if (!blur) {
                return;
            }
            if (enabled) {
                blur->IncrementBlurCount();
            } else {
                blur->DecrementBlurCount();
            }
            nativeBlurActive = enabled;
        }

        [[nodiscard]] bool Contains(const Rect& rect, const ImGui::ImVec2& point)
        {
            return point.x >= rect.x && point.x <= rect.Right() &&
                point.y >= rect.y && point.y <= rect.Bottom();
        }

        [[nodiscard]] DropPosition DropPositionFor(const Rect& rect, const ImGui::ImVec2& point)
        {
            const auto x = std::clamp((point.x - rect.x) / std::max(1.0f, rect.width), 0.0f, 1.0f);
            const auto y = std::clamp((point.y - rect.y) / std::max(1.0f, rect.height), 0.0f, 1.0f);
            if (x >= kDropCenterInset && x <= 1.0f - kDropCenterInset &&
                y >= kDropCenterInset && y <= 1.0f - kDropCenterInset) {
                return DropPosition::kCenter;
            }

            const std::array distances{ x, 1.0f - x, y, 1.0f - y };
            const auto nearest = static_cast<std::size_t>(
                std::ranges::min_element(distances) - distances.begin());
            constexpr std::array positions{
                DropPosition::kLeft,
                DropPosition::kRight,
                DropPosition::kTop,
                DropPosition::kBottom
            };
            return positions[nearest];
        }

        [[nodiscard]] Rect Interpolate(const Rect& current, const Rect& target, float amount)
        {
            return {
                std::lerp(current.x, target.x, amount),
                std::lerp(current.y, target.y, amount),
                std::lerp(current.width, target.width, amount),
                std::lerp(current.height, target.height, amount)
            };
        }

        [[nodiscard]] bool NearlyEqual(const Rect& left, const Rect& right)
        {
            constexpr float kTolerance = 0.25f;
            return std::abs(left.x - right.x) <= kTolerance &&
                std::abs(left.y - right.y) <= kTolerance &&
                std::abs(left.width - right.width) <= kTolerance &&
                std::abs(left.height - right.height) <= kTolerance;
        }

        [[nodiscard]] std::vector<LeafRect> AnimateLayout(const std::vector<LeafRect>& target)
        {
            std::unordered_set<ItemKey, ItemKeyHash> active;
            active.reserve(target.size());
            std::vector<LeafRect> result;
            result.reserve(target.size());

            const auto deltaTime = std::clamp(ImGui::GetIO()->DeltaTime, 0.0f, 0.05f);
            const auto amount = 1.0f - std::exp(-kLayoutAnimationSpeed * deltaTime);
            for (const auto& leaf : target) {
                active.insert(leaf.item);
                auto [entry, inserted] = animatedRects.try_emplace(leaf.item, leaf.rect);
                if (!inserted) {
                    entry->second = Interpolate(entry->second, leaf.rect, amount);
                    if (NearlyEqual(entry->second, leaf.rect)) {
                        entry->second = leaf.rect;
                    }
                }
                result.push_back({ leaf.item, entry->second });
            }
            std::erase_if(animatedRects, [&](const auto& entry) {
                return !active.contains(entry.first);
            });
            return result;
        }

        void ResetLayoutAnimation()
        {
            animatedRects.clear();
        }

        [[nodiscard]] std::optional<Rect> DirectionalDropCue()
        {
            if (!dragState.active || !dragState.target.IsValid() ||
                dragState.position == DropPosition::kCenter) {
                return std::nullopt;
            }

            auto cue = dragState.targetRect;
            if (dragState.position == DropPosition::kLeft || dragState.position == DropPosition::kRight) {
                cue.width *= 0.5f;
                if (dragState.position == DropPosition::kRight) {
                    cue.x += cue.width;
                }
            } else {
                cue.height *= 0.5f;
                if (dragState.position == DropPosition::kBottom) {
                    cue.y += cue.height;
                }
            }
            return cue;
        }

        [[nodiscard]] ImGui::ImVec2 MeasureText(float size, std::string_view text)
        {
            return ImGui::ImFontManger::CalcTextSizeA(
                ImGui::GetFont(),
                size,
                std::numeric_limits<float>::max(),
                0.0f,
                text.data(),
                text.data() + text.size(),
                nullptr);
        }

        [[nodiscard]] std::string BuildItemLabel(
            std::string_view name,
            std::uint32_t count,
            float fontSize,
            float maximumWidth)
        {
            const auto suffix = count > 1 ? std::format(" ({})", count) : std::string{};
            auto visibleName = std::string(name);
            auto label = visibleName + suffix;
            if (MeasureText(fontSize, label).x <= maximumWidth) {
                return label;
            }

            constexpr auto ellipsis = "..."sv;
            while (!visibleName.empty()) {
                auto characterStart = visibleName.size() - 1;
                while (characterStart > 0 &&
                    (static_cast<unsigned char>(visibleName[characterStart]) & 0xC0U) == 0x80U) {
                    --characterStart;
                }
                visibleName.erase(characterStart);
                label = visibleName + ellipsis.data() + suffix;
                if (MeasureText(fontSize, label).x <= maximumWidth) {
                    return label;
                }
            }

            return suffix.empty() ? std::string(ellipsis) : suffix.substr(1);
        }

        struct ItemLabelLayout
        {
            std::string text;
            float x{ 0.0f };
            float y{ 0.0f };
            Rect equipIndicator{};
            Rect hotkeyIndicator{};
        };

        struct RenderTile
        {
            LeafRect leaf{};
            FavoriteItem item;
            Rect preview{};
            ItemLabelLayout label{};
            bool hovered{ false };
            bool dropTarget{ false };
        };

        struct RenderGhost
        {
            ItemKey key{};
            FavoriteItem item;
            Rect bounds{};
            Rect preview{};
            ItemLabelLayout label{};
        };

        struct RenderScene
        {
            std::vector<RenderTile> tiles;
            std::optional<RenderGhost> ghost;
            std::optional<Rect> dropCue;
        };

        [[nodiscard]] ItemLabelLayout CalculateItemLabelLayout(
            const FavoriteItem& item,
            const Rect& bounds,
            const Rect& preview)
        {
            const bool showEquipIndicator = item.equipState != EquipState::kNone;
            const bool showHotkeyIndicator = item.hotkey > 0;
            const auto fontSize = static_cast<float>(Config::Get().textSize);
            const auto indicatorScale = fontSize / static_cast<float>(Config::kTextSizeDefault);
            const auto equipIndicatorSize = kDefaultEquipIndicatorSize * indicatorScale;
            const auto hotkeyIndicatorSize = kDefaultHotkeyIndicatorSize * indicatorScale;
            const auto indicatorGap = kDefaultItemIndicatorGap * indicatorScale;
            const auto leadingWidth = showEquipIndicator ? equipIndicatorSize + indicatorGap : 0.0f;
            const auto trailingWidth = showHotkeyIndicator ? indicatorGap + hotkeyIndicatorSize : 0.0f;
            const auto maximumTextWidth = std::max(
                1.0f,
                bounds.width - kItemLabelHorizontalPadding * 2.0f - leadingWidth - trailingWidth);
            auto text = BuildItemLabel(item.name, item.count, fontSize, maximumTextWidth);
            const auto measuredText = MeasureText(fontSize, text);
            const auto groupLeft = bounds.CenterX() - (leadingWidth + measuredText.x + trailingWidth) * 0.5f;
            const auto labelY = preview.Bottom() + 2.0f;

            Rect equipIndicator{};
            if (showEquipIndicator) {
                equipIndicator = {
                    groupLeft,
                    labelY + (measuredText.y - equipIndicatorSize) * 0.5f,
                    equipIndicatorSize,
                    equipIndicatorSize
                };
            }
            Rect hotkeyIndicator{};
            if (showHotkeyIndicator) {
                hotkeyIndicator = {
                    groupLeft + leadingWidth + measuredText.x + indicatorGap,
                    labelY + (measuredText.y - hotkeyIndicatorSize) * 0.5f,
                    hotkeyIndicatorSize,
                    hotkeyIndicatorSize
                };
            }
            return {
                std::move(text),
                groupLeft + leadingWidth,
                labelY,
                equipIndicator,
                hotkeyIndicator
            };
        }

        void DrawText(ImGui::ImDrawList* drawList, float size, float x, float y, ImGui::ImU32 color, std::string_view text)
        {
            ImGui::ImDrawListManager::AddText(
                drawList,
                ImGui::GetFont(),
                size,
                { x, y },
                color,
                text.data(),
                text.data() + text.size());
        }

        void DrawHotkeyIndicator(
            ImGui::ImDrawList* drawList,
            std::int8_t hotkey,
            const Rect& indicator)
        {
            if (hotkey < 1 || hotkey > 8 || indicator.width <= 0.0f || indicator.height <= 0.0f) {
                return;
            }

            const ImGui::ImVec2 center{ indicator.CenterX(), indicator.CenterY() };
            const auto radius = std::max(1.0f, std::min(indicator.width, indicator.height) * 0.46f);
            ImGui::ImDrawListManager::AddCircleFilled(drawList, center, radius, kHotkeyBackground, 20);
            ImGui::ImDrawListManager::AddCircle(
                drawList,
                center,
                radius,
                kText,
                20,
                std::max(1.0f, indicator.height * 0.055f));

            const char character = static_cast<char>('0' + hotkey);
            const std::string_view text{ std::addressof(character), 1 };
            const auto fontSize = std::max(1.0f, indicator.height * 0.68f);
            const auto textSize = MeasureText(fontSize, text);
            DrawText(
                drawList,
                fontSize,
                center.x - textSize.x * 0.5f,
                center.y - textSize.y * 0.5f,
                kText,
                text);
        }

        [[nodiscard]] const LeafRect* FindLeaf(const std::vector<LeafRect>& leaves, const ItemKey& key)
        {
            const auto match = std::ranges::find(leaves, key, &LeafRect::item);
            return match == leaves.end() ? nullptr : std::addressof(*match);
        }

        [[nodiscard]] ItemKey FindNeighbor(
            const std::vector<LeafRect>& leaves,
            const ItemKey& current,
            Direction direction)
        {
            const auto source = FindLeaf(leaves, current);
            if (!source) {
                return leaves.empty() ? ItemKey{} : leaves.front().item;
            }

            ItemKey best{};
            auto bestScore = std::numeric_limits<float>::max();
            for (const auto& candidate : leaves) {
                if (candidate.item == current) {
                    continue;
                }
                const auto dx = candidate.rect.CenterX() - source->rect.CenterX();
                const auto dy = candidate.rect.CenterY() - source->rect.CenterY();
                const bool valid =
                    (direction == Direction::kLeft && dx < -1.0f) ||
                    (direction == Direction::kRight && dx > 1.0f) ||
                    (direction == Direction::kUp && dy < -1.0f) ||
                    (direction == Direction::kDown && dy > 1.0f);
                if (!valid) {
                    continue;
                }

                const auto primary = direction == Direction::kLeft || direction == Direction::kRight ? std::abs(dx) : std::abs(dy);
                const auto secondary = direction == Direction::kLeft || direction == Direction::kRight ? std::abs(dy) : std::abs(dx);
                const auto score = primary + secondary * 0.45f;
                if (score < bestScore) {
                    bestScore = score;
                    best = candidate.item;
                }
            }
            return best;
        }

        [[nodiscard]] std::optional<Direction> PressedGamepadDirection()
        {
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true)) {
                return Direction::kLeft;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true)) {
                return Direction::kRight;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadUp, true)) {
                return Direction::kUp;
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadDown, true)) {
                return Direction::kDown;
            }
            return std::nullopt;
        }

        [[nodiscard]] std::int8_t HotkeyNumber(
            const RE::BSFixedString& userEvent,
            const RE::UserEvents& userEvents)
        {
            const std::array<const RE::BSFixedString*, 8> events{
                std::addressof(userEvents.hotkey1),
                std::addressof(userEvents.hotkey2),
                std::addressof(userEvents.hotkey3),
                std::addressof(userEvents.hotkey4),
                std::addressof(userEvents.hotkey5),
                std::addressof(userEvents.hotkey6),
                std::addressof(userEvents.hotkey7),
                std::addressof(userEvents.hotkey8)
            };
            for (std::size_t index = 0; index < events.size(); ++index) {
                if (userEvent == *events[index]) {
                    return static_cast<std::int8_t>(index + 1);
                }
            }
            return -1;
        }

        [[nodiscard]] ItemKey HotkeyTarget(const std::vector<LeafRect>& leaves)
        {
            const auto mouse = ImGui::GetIO()->MousePos;
            const auto hovered = std::ranges::find_if(leaves, [&](const LeafRect& leaf) {
                return Contains(leaf.rect, mouse);
            });
            return hovered != leaves.end() ? hovered->item : focusedItem;
        }

        void RefreshFavorites()
        {
            auto& favorites = Favorites::GetSingleton();
            favorites.Refresh();
            Layout::GetSingleton().Reconcile(favorites.Items());
            PreviewCache::GetSingleton().Reconcile(favorites.Items());
            IconLayer::Reconcile(favorites.Items());
            if (!favorites.Find(focusedItem)) {
                focusedItem = favorites.Items().empty() ? ItemKey{} : favorites.Items().front().key;
            }
            if (dragState.source.IsValid() && !favorites.Find(dragState.source)) {
                dragState = {};
            }
        }

        void __stdcall PrewarmMeshes()
        {
            if (!replacementAvailable.load(std::memory_order_acquire) || !window || window->IsOpen ||
                Config::Get().previewMode != Config::PreviewMode::kMeshes ||
                (meshPrewarmReady && !meshPrewarmRefreshRequested)) {
                return;
            }
            const auto ui = RE::UI::GetSingleton();
            if (!RE::PlayerCharacter::GetSingleton() || !ui || ui->IsApplicationMenuOpen()) {
                return;
            }

            if (std::exchange(meshPrewarmRefreshRequested, false)) {
                RefreshFavorites();
            }

            auto& cache = PreviewCache::GetSingleton();
            cache.SetVisible(false);
            cache.BeginFrame();
            meshPrewarmReady = cache.Prewarm(Favorites::GetSingleton().Items());
        }

        [[nodiscard]] Rect PreviewAreaFor(const LeafRect& leaf)
        {
            const auto labelHeight = std::clamp(leaf.rect.height * 0.24f, 48.0f, 76.0f);
            return {
                leaf.rect.x + 8.0f,
                leaf.rect.y + 8.0f,
                std::max(1.0f, leaf.rect.width - 16.0f),
                std::max(1.0f, leaf.rect.height - labelHeight - 12.0f)
            };
        }

        [[nodiscard]] Rect GhostPreviewAreaFor(const Rect& bounds)
        {
            return {
                bounds.x + 8.0f,
                bounds.y + 8.0f,
                std::max(1.0f, bounds.width - 16.0f),
                std::max(1.0f, bounds.height - 66.0f)
            };
        }

        [[nodiscard]] std::optional<Rect> DragGhostRect()
        {
            if (!dragState.active) {
                return std::nullopt;
            }
            constexpr float kMaximumGhostWidth = 320.0f;
            constexpr float kMaximumGhostHeight = 150.0f;
            const auto scale = std::min({
                1.0f,
                kMaximumGhostWidth / std::max(1.0f, dragState.sourceRect.width),
                kMaximumGhostHeight / std::max(1.0f, dragState.sourceRect.height)
            });
            const auto mouse = ImGui::GetIO()->MousePos;
            return Rect{
                mouse.x - dragState.grabOffset.x * scale,
                mouse.y - dragState.grabOffset.y * scale,
                dragState.sourceRect.width * scale,
                dragState.sourceRect.height * scale
            };
        }

        [[nodiscard]] RenderScene BuildRenderScene(const std::vector<LeafRect>& leaves, bool trackHover)
        {
            RenderScene scene;
            scene.tiles.reserve(leaves.size());
            for (const auto& leaf : leaves) {
                const auto item = Favorites::GetSingleton().Find(leaf.item);
                if (!item) {
                    continue;
                }
                const auto preview = PreviewAreaFor(leaf);
                scene.tiles.push_back({
                    .leaf = leaf,
                    .item = *item,
                    .preview = preview,
                    .label = CalculateItemLabelLayout(*item, leaf.rect, preview),
                    .hovered = trackHover && ImGui::IsMouseHoveringRect(Min(leaf.rect), Max(leaf.rect), true),
                    .dropTarget = dragState.active && dragState.target == leaf.item
                });
            }

            if (const auto bounds = DragGhostRect()) {
                const auto item = Favorites::GetSingleton().Find(dragState.source);
                if (item) {
                    const auto preview = GhostPreviewAreaFor(*bounds);
                    scene.ghost = RenderGhost{
                        .key = dragState.source,
                        .item = *item,
                        .bounds = *bounds,
                        .preview = preview,
                        .label = CalculateItemLabelLayout(*item, *bounds, preview)
                    };
                }
            }
            scene.dropCue = DirectionalDropCue();
            return scene;
        }

        void SubmitScaleformLayout(
            const ImGui::ImVec2& screen,
            const RenderScene& scene,
            const Config::Settings& settings)
        {
            std::optional<Rect> dragGhostEquipIndicator;
            if (scene.ghost) {
                if (scene.ghost->item.equipState != EquipState::kNone) {
                    dragGhostEquipIndicator = scene.ghost->label.equipIndicator;
                }
            }

            std::vector<IconLayer::Tile> iconTiles;
            iconTiles.reserve(scene.tiles.size());
            for (const auto& tile : scene.tiles) {
                iconTiles.push_back({
                    .key = tile.leaf.item,
                    .bounds = tile.leaf.rect,
                    .preview = tile.preview,
                    .hovered = tile.hovered,
                    .dropTarget = tile.dropTarget,
                    .equipState = tile.item.equipState,
                    .equipIndicator = tile.label.equipIndicator
                });
            }

            IconLayer::SubmitLayout(
                screen.x,
                screen.y,
                iconTiles,
                scene.ghost ? std::optional{ scene.ghost->bounds } : std::nullopt,
                scene.ghost ? std::optional{ scene.ghost->key } : std::nullopt,
                dragGhostEquipIndicator,
                settings.backdropAlpha,
                settings.tileAlpha,
                settings.tileStyle == Config::TileStyle::kFramed,
                settings.borderScale,
                settings.frameScale);
        }

        void DrawCachedPreview(
            ImGui::ImDrawList* drawList,
            const FavoriteItem& item,
            const Rect& area,
            ImGui::ImU32 tint = IM_COL32_WHITE)
        {
            const auto& cache = PreviewCache::GetSingleton();
            const auto preview = cache.Get(item);
            if (!preview) {
                return;
            }

            const auto imageSize = std::max(1.0f, std::min(area.width, area.height));
            const ImGui::ImVec2 imageMin{
                area.CenterX() - imageSize * 0.5f,
                area.CenterY() - imageSize * 0.5f
            };
            const ImGui::ImVec2 imageMax{ imageMin.x + imageSize, imageMin.y + imageSize };
            ImGui::ImDrawListManager::AddImage(
                drawList,
                reinterpret_cast<ImGui::ImTextureID>(preview.texture),
                imageMin,
                imageMax,
                { preview.placement.u0, preview.placement.v0 },
                { preview.placement.u1, preview.placement.v1 },
                tint);
        }

        void Activate(const FavoriteItem* item, Actions::Hand hand)
        {
            if (!item || !Actions::Activate(*item, hand)) {
                return;
            }
            if (Config::Get().closeOnSelection) {
                Close();
                return;
            }
            RefreshFavorites();
            postActivationRefreshFrames = kPostActivationRefreshFrames;
        }

        [[nodiscard]] bool UpdateDrag(const std::vector<LeafRect>& leaves)
        {
            if (!dragState.source.IsValid()) {
                return false;
            }

            const auto mouse = ImGui::GetIO()->MousePos;
            if (!dragState.active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                const auto dx = mouse.x - dragState.pressPosition.x;
                const auto dy = mouse.y - dragState.pressPosition.y;
                dragState.active = dx * dx + dy * dy >= kDragThreshold * kDragThreshold;
            }

            dragState.target = {};
            dragState.targetRect = {};
            dragState.position = DropPosition::kCenter;
            if (dragState.active) {
                for (const auto& leaf : leaves) {
                    if (leaf.item != dragState.source && Contains(leaf.rect, mouse)) {
                        dragState.target = leaf.item;
                        dragState.targetRect = leaf.rect;
                        dragState.position = DropPositionFor(leaf.rect, mouse);
                        break;
                    }
                }
            }

            if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
                const auto source = dragState.source;
                const auto target = dragState.target;
                const auto position = dragState.position;
                const auto wasDragging = dragState.active;
                dragState = {};
                if (wasDragging) {
                    if (target.IsValid() && Layout::GetSingleton().ApplyDrop({ source, target, position })) {
                        focusedItem = source;
                        return true;
                    }
                } else {
                    Activate(Favorites::GetSingleton().Find(source), Actions::Hand::kRight);
                }
            } else if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                dragState = {};
            }
            return false;
        }

        void HandleMenuInput(const std::vector<LeafRect>& leaves)
        {
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) {
                Close();
                return;
            }
            if (const auto direction = PressedGamepadDirection()) {
                const auto neighbor = FindNeighbor(leaves, focusedItem, *direction);
                if (neighbor.IsValid()) {
                    focusedItem = neighbor;
                }
            }

            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceDown, false)) {
                Activate(Favorites::GetSingleton().Find(focusedItem), Actions::Hand::kRight);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)) {
                Activate(Favorites::GetSingleton().Find(focusedItem), Actions::Hand::kLeft);
            }
        }

        [[nodiscard]] std::vector<LeafRect> PreviewLayout(
            const std::vector<LeafRect>& committed,
            const Rect& bounds,
            float gap)
        {
            if (!dragState.active || !dragState.target.IsValid()) {
                return committed;
            }
            const auto preview = Layout::GetSingleton().CalculatePreview(
                bounds,
                gap,
                { dragState.source, dragState.target, dragState.position });
            return preview ? std::move(*preview) : committed;
        }

        void DrawEmpty(ImGui::ImDrawList* drawList, const Rect& bounds)
        {
            const auto message = "No favorites yet";
            const auto fontSize = static_cast<float>(Config::Get().textSize);
            const auto messageSize = MeasureText(fontSize, message);
            DrawText(
                drawList,
                fontSize,
                bounds.CenterX() - messageSize.x * 0.5f,
                bounds.CenterY() - messageSize.y * 0.5f,
                kText,
                message);
        }

        void DrawDropCue(ImGui::ImDrawList* drawList, const std::optional<Rect>& cue)
        {
            if (!cue) {
                return;
            }
            ImGui::ImDrawListManager::AddRectFilled(
                drawList,
                Min(*cue),
                Max(*cue),
                IM_COL32(238, 232, 214, 32),
                0.0f,
                0);
        }

        void DrawTile(ImGui::ImDrawList* drawList, const RenderTile& tile)
        {
            const auto& item = tile.item;

            const auto& settings = Config::Get();
            const auto usingIcons = settings.previewMode == Config::PreviewMode::kIcons;

            if (!usingIcons) {
                DrawCachedPreview(drawList, item, tile.preview);
            }

            DrawText(
                drawList,
                static_cast<float>(settings.textSize),
                tile.label.x,
                tile.label.y,
                kText,
                tile.label.text);
            DrawHotkeyIndicator(drawList, item.hotkey, tile.label.hotkeyIndicator);

            const auto dragSource = dragState.active && dragState.source == item.key;
            if (dragSource) {
                ImGui::ImDrawListManager::AddRectFilled(
                    drawList,
                    Min(tile.leaf.rect),
                    Max(tile.leaf.rect),
                    IM_COL32(5, 7, 10, 172),
                    Theme::kCornerRadius,
                    0);
            }

            ImGui::SetCursorScreenPos(Min(tile.leaf.rect));
            const auto buttonID = std::format("##TFM_{:08X}_{}", item.key.formID, item.key.uniqueID);
            static_cast<void>(ImGui::InvisibleButton(
                buttonID.c_str(),
                { tile.leaf.rect.width, tile.leaf.rect.height }));
            if (!dragState.source.IsValid() && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
                focusedItem = item.key;
                dragState.source = item.key;
                dragState.sourceRect = tile.leaf.rect;
                dragState.pressPosition = ImGui::GetIO()->MousePos;
                dragState.grabOffset = {
                    dragState.pressPosition.x - tile.leaf.rect.x,
                    dragState.pressPosition.y - tile.leaf.rect.y
                };
            }
            if (!dragState.active && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right, false)) {
                focusedItem = item.key;
                Activate(std::addressof(item), Actions::Hand::kLeft);
            }
        }

        void DrawDragOverlay(ImGui::ImDrawList* drawList, const std::optional<RenderGhost>& ghost)
        {
            if (!ghost) {
                return;
            }
            if (Config::Get().previewMode == Config::PreviewMode::kMeshes) {
                DrawCachedPreview(
                    drawList,
                    ghost->item,
                    ghost->preview,
                    IM_COL32(255, 255, 255, 235));
            }
            DrawText(
                drawList,
                static_cast<float>(Config::Get().textSize),
                ghost->label.x,
                ghost->label.y,
                kText,
                ghost->label.text);
            DrawHotkeyIndicator(drawList, ghost->item.hotkey, ghost->label.hotkeyIndicator);
        }

        void RestoreVanillaAfterPresentationFailure()
        {
            if (!replacementAvailable.exchange(false, std::memory_order_acq_rel)) {
                return;
            }

            logger::error("Tileable favorites presentation failed; restoring the vanilla FavoritesMenu for this session");
            Close();
            favoritesButtonDown = false;
            if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                queue->AddMessage(RE::FavoritesMenu::MENU_NAME.data(), RE::UI_MESSAGE_TYPE::kShow, nullptr);
            }
        }

        [[nodiscard]] bool SynchronizeScaleformCursor()
        {
            const auto ui = RE::UI::GetSingleton();
            if (!ui) {
                return false;
            }

            const auto cursorMenu = ui->GetMenu<RE::CursorMenu>();
            if (cursorMenu && cursorMenu->OnStack()) {
                scaleformCursorRequestCooldown = 0;
                if (cursorMenu->uiMovie) {
                    const auto mouse = ImGui::GetIO()->MousePos;
                    cursorMenu->uiMovie->NotifyMouseState(mouse.x, mouse.y, 0);
                    return true;
                }
                return false;
            }

            if (ui->IsMenuOpen(RE::FavoritesMenu::MENU_NAME)) {
                return false;
            }
            if (scaleformCursorRequestCooldown > 0) {
                --scaleformCursorRequestCooldown;
                return false;
            }

            if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                queue->AddMessage(RE::CursorMenu::MENU_NAME.data(), RE::UI_MESSAGE_TYPE::kShow, nullptr);
                scaleformCursorRequestCooldown = kScaleformCursorRetryFrames;
            }
            return false;
        }

        void HideScaleformCursor()
        {
            scaleformCursorRequestCooldown = 0;
            if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                queue->AddMessage(RE::CursorMenu::MENU_NAME.data(), RE::UI_MESSAGE_TYPE::kHide, nullptr);
            }
        }

        void __stdcall Render()
        {
            if (!window || !window->IsOpen) {
                return;
            }

            const bool equipEventReceived = equipmentStateChanged.exchange(false, std::memory_order_acq_rel);
            const bool postActivationRefresh = postActivationRefreshFrames > 0;
            if (postActivationRefresh) {
                --postActivationRefreshFrames;
            }
            bool hotkeyChanged = false;
            if (const auto number = pendingHotkey.exchange(0, std::memory_order_acq_rel); number > 0) {
                const auto target = UnpackItemKey(pendingHotkeyTarget.load(std::memory_order_acquire));
                if (target.IsValid() && Favorites::GetSingleton().AssignHotkey(target, static_cast<std::int8_t>(number))) {
                    focusedItem = target;
                    hotkeyChanged = true;
                }
            }
            refreshTimer += std::max(0.0f, ImGui::GetIO()->DeltaTime);
            if (hotkeyChanged || equipEventReceived || postActivationRefresh || refreshTimer >= 0.5f) {
                refreshTimer = 0.0f;
                RefreshFavorites();
            }

            auto& previewCache = PreviewCache::GetSingleton();

            const auto screen = ImGui::GetIO()->DisplaySize;
            if (centerCursorOnNextFrame) {
                centerCursorOnNextFrame = false;
                ImGui::TeleportMousePos({ screen.x * 0.5f, screen.y * 0.5f });
            }
            if (SynchronizeScaleformCursor()) {
                ImGui::SetMouseCursor(ImGuiMouseCursor_None);
            }
            const auto& settings = Config::Get();
            previewCache.BeginFrame();

            const Rect bounds{
                settings.outerMargin,
                settings.outerMargin,
                std::max(1.0f, screen.x - settings.outerMargin * 2.0f),
                std::max(1.0f, screen.y - settings.outerMargin * 2.0f)
            };
            auto leaves = Layout::GetSingleton().Calculate(bounds, settings.gap);
            inputHotkeyTarget.store(PackItemKey(HotkeyTarget(leaves)), std::memory_order_release);

            bool layoutSubmitted = false;
            std::optional<RenderScene> renderScene;
            auto presentationState = IconLayer::GetPresentationState();
            if (presentationState == IconLayer::PresentationState::kFailed) {
                RestoreVanillaAfterPresentationFailure();
                return;
            }
            if (presentationState != IconLayer::PresentationState::kReady) {
                renderScene = BuildRenderScene(leaves, false);
                SubmitScaleformLayout(screen, *renderScene, settings);
                layoutSubmitted = true;
                presentationState = IconLayer::GetPresentationState();
                if (presentationState == IconLayer::PresentationState::kFailed) {
                    RestoreVanillaAfterPresentationFailure();
                    return;
                }
            }
            if (settings.previewMode == Config::PreviewMode::kMeshes) {
                const bool meshesReady = previewCache.Update(Favorites::GetSingleton().Items());
                if (!initialMeshRevealComplete) {
                    if (!meshesReady && !incrementalMeshRevealAllowed) {
                        return;
                    }
                    initialMeshRevealComplete = true;
                }
            }
            if (presentationState != IconLayer::PresentationState::kReady) {
                return;
            }

            ImGui::SetNextWindowPos({ 0.0f, 0.0f }, ImGuiCond_Always);
            ImGui::SetNextWindowSize(screen, ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGui::SetNextWindowFocus();
            constexpr auto flags =
                ImGuiWindowFlags_NoDecoration |
                ImGuiWindowFlags_NoMove |
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoScrollbar |
                ImGuiWindowFlags_NoScrollWithMouse |
                ImGuiWindowFlags_NoSavedSettings |
                ImGuiWindowFlags_NoBackground;

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0f, 0.0f });
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            if (ImGui::Begin("##TileableFavoritesMenu", nullptr, flags)) {
                const auto drawList = ImGui::GetWindowDrawList();
                HandleMenuInput(leaves);
                if (IsOpen()) {
                    if (UpdateDrag(leaves)) {
                        leaves = Layout::GetSingleton().Calculate(bounds, settings.gap);
                    }
                    if (!renderScene) {
                        const auto preview = PreviewLayout(leaves, bounds, settings.gap);
                        const auto animated = AnimateLayout(preview);
                        inputHotkeyTarget.store(PackItemKey(HotkeyTarget(animated)), std::memory_order_release);
                        renderScene = BuildRenderScene(animated, true);
                    }
                    if (!layoutSubmitted) {
                        SubmitScaleformLayout(screen, *renderScene, settings);
                    }
                    if (renderScene->tiles.empty()) {
                        DrawEmpty(drawList, bounds);
                    } else {
                        DrawDropCue(drawList, renderScene->dropCue);
                        for (const auto& tile : renderScene->tiles) {
                            DrawTile(drawList, tile);
                            if (!IsOpen()) {
                                break;
                            }
                        }
                    }
                    if (IsOpen()) {
                        DrawDragOverlay(drawList, renderScene->ghost);
                    }
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
        }

        bool __stdcall ProcessInput(RE::InputEvent* event)
        {
            const auto button = event ? event->AsButtonEvent() : nullptr;
            const auto userEvents = RE::UserEvents::GetSingleton();
            if (!button || !userEvents || !window || !replacementAvailable.load(std::memory_order_acquire)) {
                return false;
            }

            if (window->IsOpen) {
                const auto number = HotkeyNumber(button->QUserEvent(), *userEvents);
                if (number > 0) {
                    if (button->IsDown()) {
                        pendingHotkeyTarget.store(inputHotkeyTarget.load(std::memory_order_acquire), std::memory_order_release);
                        pendingHotkey.store(number, std::memory_order_release);
                    }
                    return true;
                }
            }

            if (button->QUserEvent() != userEvents->favorites) {
                return false;
            }

            if (!IsOpen()) {
                if (button->IsDown()) {
                    favoritesButtonDown = true;
                } else if (button->IsUp()) {
                    favoritesButtonDown = false;
                }

                // Let Skyrim's FavoritesHandler decide whether the vanilla menu may open.
                // VanillaMenuSink replaces it with this menu only after that succeeds.
                return false;
            }

            if (button->IsDown()) {
                if (!favoritesButtonDown) {
                    favoritesButtonDown = true;
                    Close();
                }
                return true;
            }
            if (button->IsUp()) {
                favoritesButtonDown = false;
                return true;
            }
            return IsOpen();
        }

        class VanillaMenuSink final : public RE::BSTEventSink<RE::MenuOpenCloseEvent>
        {
        public:
            static VanillaMenuSink& GetSingleton()
            {
                static VanillaMenuSink singleton;
                return singleton;
            }

            RE::BSEventNotifyControl ProcessEvent(
                const RE::MenuOpenCloseEvent* event,
                RE::BSTEventSource<RE::MenuOpenCloseEvent>*) override
            {
                if (!event || !window || !replacementAvailable.load(std::memory_order_acquire)) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (!event->opening) {
                    meshPrewarmRefreshRequested = true;
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (event->menuName != "FavoritesMenu") {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (IconLayer::HandleFavoritesMenuOpened()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                Open();
                if (IconLayer::HandleFavoritesMenuOpened()) {
                    return RE::BSEventNotifyControl::kContinue;
                }
                if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                    queue->AddMessage(RE::FavoritesMenu::MENU_NAME.data(), RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                }
                return RE::BSEventNotifyControl::kContinue;
            }

        private:
            VanillaMenuSink() = default;
        };
    }

    bool Register()
    {
        if (registered) {
            return window != nullptr;
        }
        registered = true;

        if (Config::Get().previewMode == Config::PreviewMode::kMeshes &&
            !PreviewCache::GetSingleton().IsFrameworkAvailable()) {
            Config::UsePreviewModeForSession(Config::PreviewMode::kIcons);
            logger::warn("Mesh Rendering Framework is unavailable; using flat icons for this session");
        }

        if (!SKSEMenuFramework::IsInstalled() || !GetMenuFrameworkModule()) {
            logger::error("SKSE Menu Framework is unavailable; vanilla FavoritesMenu will remain active");
            return false;
        }

        if (!IconLayer::Register()) {
            logger::error("Scaleform tile layer registration failed; vanilla FavoritesMenu will remain active");
            return false;
        }

        window = SKSEMenuFramework::AddWindow(Render, true);
        if (!window) {
            logger::error("SKSE Menu Framework rejected the tileable favorites window; vanilla fallback remains active");
            return false;
        }
        inputRegistration.reset(SKSEMenuFramework::AddInputEvent(ProcessInput));
        meshPrewarmRegistration.reset(SKSEMenuFramework::AddHudElement(PrewarmMeshes));
        if (const auto skyrimUI = RE::UI::GetSingleton()) {
            skyrimUI->AddEventSink<RE::MenuOpenCloseEvent>(std::addressof(VanillaMenuSink::GetSingleton()));
        }
        if (const auto events = RE::ScriptEventSourceHolder::GetSingleton()) {
            events->AddEventSink<RE::TESEquipEvent>(std::addressof(PlayerEquipEventSink::GetSingleton()));
        }
        replacementAvailable.store(true, std::memory_order_release);
        logger::info("Tileable favorites window registered (menu framework {:.2f})", SKSEMenuFramework::GetMenuFrameworkVersion());
        return true;
    }

    void Open()
    {
        if (!window || !replacementAvailable.load(std::memory_order_acquire)) {
            return;
        }
        const auto wasOpen = window->IsOpen.load();
        if (!wasOpen) {
            ResetLayoutAnimation();
            centerCursorOnNextFrame = true;
            scaleformCursorRequestCooldown = 0;
        }
        PreviewCache::GetSingleton().SetVisible(Config::Get().previewMode == Config::PreviewMode::kMeshes);
        IconLayer::SetActive(true, Config::Get().previewMode == Config::PreviewMode::kIcons);
        RefreshFavorites();
        if (!wasOpen) {
            const auto useMeshes = Config::Get().previewMode == Config::PreviewMode::kMeshes;
            initialMeshRevealComplete = !useMeshes;
            incrementalMeshRevealAllowed = useMeshes && std::ranges::any_of(
                Favorites::GetSingleton().Items(),
                [](const FavoriteItem& item) {
                    return static_cast<bool>(PreviewCache::GetSingleton().Get(item));
                });
        }
        dragState = {};
        postActivationRefreshFrames = 0;
        equipmentStateChanged.store(false, std::memory_order_release);
        pendingHotkey.store(0, std::memory_order_release);
        inputHotkeyTarget.store(PackItemKey(focusedItem), std::memory_order_release);
        refreshTimer = 0.0f;
        window->IsOpen = true;
        if (!wasOpen) {
            SetNativeBlur(true);
        }
    }

    void Close()
    {
        const auto wasOpen = window && window->IsOpen.exchange(false);
        IconLayer::SetActive(false, false);
        PreviewCache::GetSingleton().SetVisible(false);
        dragState = {};
        centerCursorOnNextFrame = false;
        ResetLayoutAnimation();
        initialMeshRevealComplete = false;
        incrementalMeshRevealAllowed = false;
        postActivationRefreshFrames = 0;
        pendingHotkey.store(0, std::memory_order_release);
        inputHotkeyTarget.store(0, std::memory_order_release);
        if (wasOpen) {
            HideScaleformCursor();
            SetNativeBlur(false);
        }
    }

    void ApplyPreviewSettings()
    {
        PreviewCache::GetSingleton().Clear();
        const auto useIcons = Config::Get().previewMode == Config::PreviewMode::kIcons;
        initialMeshRevealComplete = useIcons;
        incrementalMeshRevealAllowed = false;
        PreviewCache::GetSingleton().SetVisible(IsOpen() && !useIcons);
        IconLayer::SetActive(IsOpen(), useIcons);
        meshPrewarmReady = false;
        meshPrewarmRefreshRequested = true;
        if (IsOpen()) {
            RefreshFavorites();
        }
    }

    bool IsOpen()
    {
        return window && window->IsOpen;
    }
}
