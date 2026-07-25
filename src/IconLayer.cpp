#include "IconLayer.h"

#include "Theme.h"
#include "logger.h"

namespace TFM::IconLayer
{
    namespace
    {
        constexpr auto kMenuName = "TileableFavoritesIconLayer"sv;
        constexpr auto kMovieName = "TileableFavoritesIcons"sv;
        constexpr auto kFrameLibrarySource = "messagebox.swf"sv;
        constexpr auto kFrameLinkage = "MessageBox"sv;
        constexpr auto kFrameBackgroundMember = "Background_mc"sv;
        constexpr auto kEquipLibrarySource = "skyui/inventorylists.swf"sv;
        constexpr auto kEquipLinkage = "EquipIcon"sv;
        constexpr auto kDearDiaryBackgroundSourceMarker = "deardiary"sv;
        constexpr auto kDefaultIconSource = "skyui/icons_item_psychosteve.swf"sv;
        constexpr std::uint32_t kProbeTimeoutFrames = 120;
        constexpr std::uint32_t kFrameMinimumSettleFrames = 3;
        constexpr std::uint32_t kEquipLibraryDepth = 500000;
        constexpr float kIconLibrarySize = 128.0f;
        constexpr float kEquipIconAuthoringSize = 20.0f;
        constexpr float kFrameLibraryParkingX = -4096.0f;
        // Dear Diary's external background has asymmetric transparent authoring margins. These
        // per-axis corrections map its visible painted edge back onto the submitted tile bounds.
        constexpr float kDearDiaryBackgroundScaleX = 1.234f;
        constexpr float kDearDiaryBackgroundScaleY = 1.273f;
        constexpr float kDearDiaryBackgroundOffsetX = -0.003f;
        constexpr float kDearDiaryBackgroundOffsetY = -0.0335f;
        constexpr std::array kMessageBoxChromeMembers{
            "Divider"sv,
            "MessageText"sv,
            "TextDone"sv,
            "TextNo"sv,
            "TextCancel"sv,
            "TextExit"sv,
            "TextBack"sv,
            "TextYes"sv
        };
        struct Descriptor
        {
            std::string source{ kDefaultIconSource };
            std::string label;
            std::uint32_t color{ 0xFFFFFF };
        };

        [[nodiscard]] constexpr std::string_view EquipFrameLabel(EquipState state) noexcept
        {
            switch (state) {
            case EquipState::kEquipped:
                return "Equipped"sv;
            case EquipState::kLeft:
                return "LeftEquip"sv;
            case EquipState::kRight:
                return "RightEquip"sv;
            case EquipState::kBoth:
                return "LeftAndRightEquip"sv;
            default:
                return "None"sv;
            }
        }

        struct LayoutSnapshot
        {
            float screenWidth{ 0.0f };
            float screenHeight{ 0.0f };
            float backdropAlpha{ 0.18f };
            float tileAlpha{ 0.62f };
            bool useFrames{ false };
            float borderScale{ 1.0f };
            float frameScale{ 1.0f };
            std::vector<Tile> tiles;
            std::optional<Rect> dragGhost;
            std::optional<ItemKey> dragGhostItem;
            std::optional<Rect> dragGhostEquipIndicator;

            friend bool operator==(const LayoutSnapshot&, const LayoutSnapshot&) = default;
        };

        struct SharedState
        {
            std::mutex mutex;
            bool registered{ false };
            bool active{ false };
            bool iconsEnabled{ false };
            bool hostReady{ false };
            bool hostFailed{ false };
            bool frameReady{ false };
            bool frameFailed{ false };
            bool presentationReady{ false };
            bool probeRequested{ false };
            bool probeOpening{ false };
            bool restartProbe{ false };
            std::uint32_t probeFrames{ 0 };
            std::string phase{ "disabled" };
            std::string error;
            std::vector<ItemKey> items;
            std::unordered_map<ItemKey, Descriptor, ItemKeyHash> descriptors;
            std::unordered_set<ItemKey, ItemKeyHash> loaded;
            std::unordered_set<ItemKey, ItemKeyHash> failed;
            LayoutSnapshot layout;
            std::uint64_t descriptorRevision{ 1 };
            std::uint64_t layoutRevision{ 1 };
        };

        struct MenuSnapshot
        {
            bool active{ false };
            bool iconsEnabled{ false };
            std::vector<ItemKey> items;
            std::unordered_map<ItemKey, Descriptor, ItemKeyHash> descriptors;
            LayoutSnapshot layout;
            std::uint64_t descriptorRevision{ 0 };
            std::uint64_t layoutRevision{ 0 };
        };

        struct ExtractedDescriptors
        {
            std::unordered_map<RE::FormID, Descriptor> byForm;
            std::uint32_t entryCount{ 0 };
        };

        SharedState& State()
        {
            static SharedState state;
            return state;
        }

        [[nodiscard]] bool HasEveryDescriptor(const SharedState& state)
        {
            return std::ranges::all_of(state.items, [&](const ItemKey& key) {
                return state.descriptors.contains(key);
            });
        }

        [[nodiscard]] bool HasCompleteLayout(const SharedState& state)
        {
            if (state.layout.screenWidth <= 0.0f || state.layout.screenHeight <= 0.0f) {
                return false;
            }
            return std::ranges::all_of(state.items, [&](const ItemKey& key) {
                return std::ranges::find(state.layout.tiles, key, &Tile::key) != state.layout.tiles.end();
            });
        }

        [[nodiscard]] bool IconLoadsSettled(const SharedState& state)
        {
            if (!state.iconsEnabled) {
                return true;
            }
            if (state.probeRequested || state.probeOpening || state.phase == "waiting" || state.phase == "resolving") {
                return false;
            }
            return std::ranges::all_of(state.descriptors, [&](const auto& entry) {
                return state.loaded.contains(entry.first) || state.failed.contains(entry.first);
            });
        }

        [[nodiscard]] bool TryMakePresentationReady(std::uint64_t descriptorRevision, std::uint64_t layoutRevision)
        {
            auto& state = State();
            std::scoped_lock lock(state.mutex);
            if (state.presentationReady) {
                return true;
            }
            if (!state.active || !state.hostReady || (state.layout.useFrames && !state.frameReady) ||
                state.descriptorRevision != descriptorRevision || state.layoutRevision != layoutRevision) {
                return false;
            }
            state.presentationReady = HasCompleteLayout(state) && IconLoadsSettled(state);
            return state.presentationReady;
        }

        [[nodiscard]] MenuSnapshot CaptureMenuSnapshot()
        {
            auto& state = State();
            std::scoped_lock lock(state.mutex);
            return {
                state.active,
                state.iconsEnabled,
                state.items,
                state.descriptors,
                state.layout,
                state.descriptorRevision,
                state.layoutRevision
            };
        }

        void SetHostReady(bool ready, std::string error = {})
        {
            auto& state = State();
            std::scoped_lock lock(state.mutex);
            state.hostReady = ready;
            if (ready) {
                state.hostFailed = false;
            } else if (!error.empty()) {
                state.hostFailed = true;
            }
            if (!ready) {
                state.presentationReady = false;
            }
            if (!error.empty()) {
                state.error = std::move(error);
                state.phase = "failed";
            }
        }

        void SetFrameReady(bool ready, std::string error = {})
        {
            auto& state = State();
            std::scoped_lock lock(state.mutex);
            if (state.frameReady != ready) {
                state.frameReady = ready;
                ++state.layoutRevision;
            }
            if (ready) {
                state.frameFailed = false;
            } else if (!error.empty()) {
                state.frameFailed = true;
            }
            if (!ready) {
                state.presentationReady = false;
            }
            if (!error.empty()) {
                state.error = std::move(error);
                state.phase = "failed";
            }
        }

        void ReportLoaded(const ItemKey& key)
        {
            auto& state = State();
            std::scoped_lock lock(state.mutex);
            state.loaded.insert(key);
            state.failed.erase(key);
        }

        void ReportFailed(const ItemKey& key, std::string error)
        {
            auto& state = State();
            std::scoped_lock lock(state.mutex);
            state.loaded.erase(key);
            state.failed.insert(key);
            state.error = std::move(error);
        }

        [[nodiscard]] std::optional<RE::GFxValue> GetEntryList(RE::FavoritesMenu& menu)
        {
            RE::GFxValue list;
            auto& root = menu.GetRuntimeData().root;
            if (root.IsObject()) {
                root.GetMember("itemList", std::addressof(list));
            }

            if (!list.IsObject() && menu.uiMovie) {
                constexpr std::array paths{
                    "_root.Menu_mc.itemList",
                    "_root.itemList",
                    "Menu_mc.itemList"
                };
                for (const auto path : paths) {
                    if (menu.uiMovie->GetVariable(std::addressof(list), path) && list.IsObject()) {
                        break;
                    }
                }
            }
            if (!list.IsObject()) {
                return std::nullopt;
            }

            RE::GFxValue entries;
            list.GetMember("_entryList", std::addressof(entries));
            if (!entries.IsArray()) {
                list.GetMember("entryList", std::addressof(entries));
            }
            return entries.IsArray() ? std::optional{ entries } : std::nullopt;
        }

        [[nodiscard]] ExtractedDescriptors ExtractDescriptors(RE::FavoritesMenu& menu)
        {
            ExtractedDescriptors result;
            const auto entries = GetEntryList(menu);
            if (!entries) {
                return result;
            }

            result.entryCount = entries->GetArraySize();
            for (std::uint32_t index = 0; index < result.entryCount; ++index) {
                RE::GFxValue entry;
                entries->GetElement(index, std::addressof(entry));
                if (!entry.IsObject()) {
                    continue;
                }

                RE::GFxValue formIDValue;
                RE::GFxValue labelValue;
                entry.GetMember("formId", std::addressof(formIDValue));
                entry.GetMember("iconLabel", std::addressof(labelValue));
                if (!formIDValue.IsNumber() || !labelValue.IsString()) {
                    continue;
                }

                Descriptor descriptor;
                descriptor.label = labelValue.GetString();

                RE::GFxValue sourceValue;
                entry.GetMember("iconSource", std::addressof(sourceValue));
                if (sourceValue.IsString() && sourceValue.GetString()[0] != '\0') {
                    descriptor.source = sourceValue.GetString();
                }

                RE::GFxValue colorValue;
                entry.GetMember("iconColor", std::addressof(colorValue));
                if (colorValue.IsNumber()) {
                    descriptor.color = static_cast<std::uint32_t>(colorValue.GetNumber());
                }

                result.byForm.insert_or_assign(
                    static_cast<RE::FormID>(formIDValue.GetNumber()),
                    std::move(descriptor));
            }
            return result;
        }

        [[nodiscard]] bool HasEveryExpectedForm(
            const std::vector<ItemKey>& expected,
            const std::unordered_map<RE::FormID, Descriptor>& descriptors)
        {
            return std::ranges::all_of(expected, [&](const ItemKey& key) {
                return descriptors.contains(key.formID);
            });
        }

        void FinishProbe(
            const std::vector<ItemKey>& expected,
            const ExtractedDescriptors& extracted,
            bool timedOut)
        {
            auto& state = State();
            {
                std::scoped_lock lock(state.mutex);
                if (state.items != expected) {
                    state.restartProbe = true;
                } else {
                    state.descriptors.clear();
                    for (const auto& key : expected) {
                        if (const auto found = extracted.byForm.find(key.formID); found != extracted.byForm.end()) {
                            state.descriptors.emplace(key, found->second);
                        }
                    }
                    state.loaded.clear();
                    state.failed.clear();
                    state.presentationReady = false;
                    ++state.descriptorRevision;
                    state.phase = timedOut && state.descriptors.empty() ? "failed" : "ready";
                    if (timedOut) {
                        state.error = std::format(
                            "Favorites icon probe timed out ({} list entries, {} resolved icons)",
                            extracted.entryCount,
                            state.descriptors.size());
                    } else {
                        state.error.clear();
                    }
                }
                state.probeRequested = state.restartProbe;
                state.probeOpening = false;
                state.restartProbe = false;
                state.probeFrames = 0;
            }

            if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                queue->AddMessage(RE::FavoritesMenu::MENU_NAME.data(), RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
            }
        }

        void CancelProbe()
        {
            bool hideFavorites = false;
            {
                auto& state = State();
                std::scoped_lock lock(state.mutex);
                hideFavorites = state.probeOpening;
                state.probeRequested = false;
                state.probeOpening = false;
                state.restartProbe = false;
                state.probeFrames = 0;
                state.phase = state.active ? "ready" : "disabled";
            }
            if (hideFavorites) {
                if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                    queue->AddMessage(RE::FavoritesMenu::MENU_NAME.data(), RE::UI_MESSAGE_TYPE::kForceHide, nullptr);
                }
            }
        }

        void AdvanceProbe()
        {
            std::vector<ItemKey> expected;
            bool active = false;
            bool requestOpen = false;
            bool probing = false;
            std::uint32_t frames = 0;
            {
                auto& state = State();
                std::scoped_lock lock(state.mutex);
                active = state.active && state.iconsEnabled;
                if (active && state.items.empty() &&
                    (state.probeRequested || state.probeOpening || !state.descriptors.empty() || state.phase != "ready")) {
                    state.descriptors.clear();
                    state.loaded.clear();
                    state.failed.clear();
                    state.presentationReady = false;
                    state.probeRequested = false;
                    state.probeOpening = false;
                    state.phase = "ready";
                    ++state.descriptorRevision;
                }
                if (active && state.probeRequested && !state.probeOpening && !state.items.empty()) {
                    state.probeRequested = false;
                    state.probeOpening = true;
                    state.probeFrames = 0;
                    state.phase = "resolving";
                    state.error.clear();
                    requestOpen = true;
                }
                probing = state.probeOpening;
                if (probing) {
                    frames = ++state.probeFrames;
                    expected = state.items;
                }
            }

            if (!active) {
                CancelProbe();
                return;
            }
            if (requestOpen) {
                if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                    queue->AddMessage(RE::FavoritesMenu::MENU_NAME.data(), RE::UI_MESSAGE_TYPE::kShow, nullptr);
                }
            }
            if (!probing) {
                return;
            }

            ExtractedDescriptors extracted;
            if (const auto ui = RE::UI::GetSingleton()) {
                if (const auto favoritesMenu = ui->GetMenu<RE::FavoritesMenu>()) {
                    if (favoritesMenu->uiMovie) {
                        favoritesMenu->uiMovie->SetVisible(false);
                    }
                    extracted = ExtractDescriptors(*favoritesMenu);
                }
            }

            if (HasEveryExpectedForm(expected, extracted.byForm)) {
                FinishProbe(expected, extracted, false);
            } else if (frames >= kProbeTimeoutFrames) {
                logger::warn(
                    "Favorites icon probe timed out after {} frames ({} of {} forms resolved)",
                    frames,
                    extracted.byForm.size(),
                    expected.size());
                FinishProbe(expected, extracted, true);
            }
        }

        [[nodiscard]] ItemKey KeyFromListener(const RE::GFxValue& listener)
        {
            RE::GFxValue formID;
            RE::GFxValue uniqueID;
            listener.GetMember("formID", std::addressof(formID));
            listener.GetMember("uniqueID", std::addressof(uniqueID));
            return {
                formID.IsNumber() ? static_cast<RE::FormID>(formID.GetNumber()) : RE::FormID{ 0 },
                uniqueID.IsNumber() ? static_cast<std::uint16_t>(uniqueID.GetNumber()) : std::uint16_t{ 0 }
            };
        }

        void ApplyColor(const RE::GFxValue& icon, std::uint32_t rgb)
        {
            icon.VisitMembers([rgb]([[maybe_unused]] const char* name, const RE::GFxValue& value) {
                if (!value.IsDisplayObject()) {
                    return;
                }
                RE::GRenderer::Cxform transform;
                transform.matrix[0][0] = 0.0f;
                transform.matrix[1][0] = 0.0f;
                transform.matrix[2][0] = 0.0f;
                transform.matrix[0][1] = static_cast<float>((rgb >> 16U) & 0xFFU);
                transform.matrix[1][1] = static_cast<float>((rgb >> 8U) & 0xFFU);
                transform.matrix[2][1] = static_cast<float>(rgb & 0xFFU);
                const_cast<RE::GFxValue&>(value).SetCxform(transform);
            });
        }

        class LoadInitHandler final : public RE::GFxFunctionHandler
        {
        public:
            void Call(Params& params) override
            {
                if (!params.thisPtr || params.argCount < 1 || !params.args[0].IsDisplayObject()) {
                    return;
                }

                auto& icon = params.args[0];
                RE::GFxValue label;
                params.thisPtr->GetMember("iconLabel", std::addressof(label));
                if (label.IsString()) {
                    icon.Invoke("gotoAndStop", nullptr, std::addressof(label), 1);
                }

                RE::GFxValue color;
                params.thisPtr->GetMember("iconColor", std::addressof(color));
                ApplyColor(icon, color.IsNumber() ? static_cast<std::uint32_t>(color.GetNumber()) : 0xFFFFFF);
                icon.SetMember("_visible", true);
                ReportLoaded(KeyFromListener(*params.thisPtr));
            }
        };

        class LoadErrorHandler final : public RE::GFxFunctionHandler
        {
        public:
            void Call(Params& params) override
            {
                if (!params.thisPtr) {
                    return;
                }
                std::string reason = "Scaleform MovieClipLoader could not load an icon source";
                if (params.argCount >= 2 && params.args[1].IsString()) {
                    reason = std::format("Scaleform icon load failed: {}", params.args[1].GetString());
                }
                const auto key = KeyFromListener(*params.thisPtr);
                logger::warn("{} for {:08X}", reason, key.formID);
                ReportFailed(key, std::move(reason));
            }
        };

        void InvokePoint(RE::GFxValue& clip, const char* method, double x, double y)
        {
            std::array args{ RE::GFxValue(x), RE::GFxValue(y) };
            clip.Invoke(method, args);
        }

        void InvokeCurve(RE::GFxValue& clip, double controlX, double controlY, double anchorX, double anchorY)
        {
            std::array args{
                RE::GFxValue(controlX),
                RE::GFxValue(controlY),
                RE::GFxValue(anchorX),
                RE::GFxValue(anchorY)
            };
            clip.Invoke("curveTo", args);
        }

        void DrawRoundRect(RE::GFxValue& clip, const Rect& rect, float radius, std::uint32_t color, float alpha)
        {
            const auto clampedRadius = std::clamp(radius, 0.0f, std::min(rect.width, rect.height) * 0.5f);
            std::array fillArgs{ RE::GFxValue(static_cast<double>(color)), RE::GFxValue(static_cast<double>(alpha)) };
            clip.Invoke("beginFill", fillArgs);

            const auto left = static_cast<double>(rect.x);
            const auto top = static_cast<double>(rect.y);
            const auto right = static_cast<double>(rect.Right());
            const auto bottom = static_cast<double>(rect.Bottom());
            const auto r = static_cast<double>(clampedRadius);
            InvokePoint(clip, "moveTo", left + r, top);
            InvokePoint(clip, "lineTo", right - r, top);
            if (r > 0.0) {
                InvokeCurve(clip, right, top, right, top + r);
            }
            InvokePoint(clip, "lineTo", right, bottom - r);
            if (r > 0.0) {
                InvokeCurve(clip, right, bottom, right - r, bottom);
            }
            InvokePoint(clip, "lineTo", left + r, bottom);
            if (r > 0.0) {
                InvokeCurve(clip, left, bottom, left, bottom - r);
            }
            InvokePoint(clip, "lineTo", left, top + r);
            if (r > 0.0) {
                InvokeCurve(clip, left, top, left + r, top);
            }
            clip.Invoke("endFill");
        }

        class IconLayerMenu final : public RE::IMenu
        {
        public:
            IconLayerMenu()
            {
                depthPriority = 12;
                menuFlags.set(
                    RE::UI_MENU_FLAGS::kRequiresUpdate,
                    RE::UI_MENU_FLAGS::kTopmostRenderedMenu,
                    RE::UI_MENU_FLAGS::kUsesCursor,
                    RE::UI_MENU_FLAGS::kAdvancesUnderPauseMenu,
                    RE::UI_MENU_FLAGS::kRendersUnderPauseMenu);

                const auto manager = RE::BSScaleformManager::GetSingleton();
                const bool loaded = manager && manager->LoadMovieEx(
                    this,
                    kMovieName,
                    RE::GFxMovieView::ScaleModeType::kShowAll,
                    0.0f,
                    [](RE::GFxMovieDef* definition) {
                        definition->SetState(RE::GFxState::StateType::kLog, RE::make_gptr<RE::GFxLog>().get());
                    });
                if (!loaded || !uiMovie) {
                    logger::error("Could not load Interface/{}.swf", kMovieName);
                    SetHostReady(false, std::format("Could not load Interface/{}.swf", kMovieName));
                    return;
                }
                uiMovie->SetVisible(true);
                uiMovie->SetBackgroundAlpha(0.0f);
                Initialize();
            }

            ~IconLayerMenu() override
            {
                SetFrameReady(false);
                SetHostReady(false);
            }

            void PostCreate() override
            {
                RE::IMenu::PostCreate();
                Initialize();
            }

            void AdvanceMovie(float interval, std::uint32_t currentTime) override
            {
                RE::IMenu::AdvanceMovie(interval, currentTime);
                AdvanceProbe();
                Synchronize();
            }

            static RE::IMenu* Create()
            {
                return new IconLayerMenu();
            }

        private:
            struct IconSlot
            {
                ItemKey key{};
                RE::GFxValue clip;
                RE::GFxValue loader;
                RE::GFxValue listener;
            };

            struct FrameInstance
            {
                RE::GFxValue clip;
                RE::GFxValue background;
                std::string source;
                std::uint32_t ageFrames{ 0 };
                std::uint32_t stableFrames{ 0 };
                float contentScaleX{ 1.0f };
                float contentScaleY{ 1.0f };
                float contentOffsetX{ 0.0f };
                float contentOffsetY{ 0.0f };
                bool ready{ false };

                [[nodiscard]] bool IsValid() const
                {
                    return clip.IsDisplayObject() && background.IsDisplayObject();
                }
            };

            struct FrameSlot
            {
                ItemKey key{};
                FrameInstance frame;
            };

            struct EquipSlot
            {
                ItemKey key{};
                EquipState state{ EquipState::kNone };
                RE::GFxValue clip;
            };

            class FrameLoadInitHandler final : public RE::GFxFunctionHandler
            {
            public:
                explicit FrameLoadInitHandler(IconLayerMenu& owner) : owner_(owner) {}

                void Call(Params& params) override
                {
                    if (params.argCount < 1 || !params.args[0].IsDisplayObject()) {
                        owner_.FailFrameLibrary("messagebox.swf loaded without a display object");
                        return;
                    }
                    owner_.AcceptFrameLibrary(params.args[0]);
                }

            private:
                IconLayerMenu& owner_;
            };

            class FrameLoadErrorHandler final : public RE::GFxFunctionHandler
            {
            public:
                explicit FrameLoadErrorHandler(IconLayerMenu& owner) : owner_(owner) {}

                void Call(Params& params) override
                {
                    std::string reason = "Scaleform could not load the installed messagebox.swf";
                    if (params.argCount >= 2 && params.args[1].IsString()) {
                        reason = std::format("messagebox.swf load failed: {}", params.args[1].GetString());
                    }
                    owner_.FailFrameLibrary(std::move(reason));
                }

            private:
                IconLayerMenu& owner_;
            };

            class EquipLoadInitHandler final : public RE::GFxFunctionHandler
            {
            public:
                explicit EquipLoadInitHandler(IconLayerMenu& owner) : owner_(owner) {}

                void Call(Params& params) override
                {
                    if (params.argCount < 1 || !params.args[0].IsDisplayObject()) {
                        owner_.FailEquipLibrary("inventorylists.swf loaded without a display object");
                        return;
                    }
                    owner_.AcceptEquipLibrary(params.args[0]);
                }

            private:
                IconLayerMenu& owner_;
            };

            class EquipLoadErrorHandler final : public RE::GFxFunctionHandler
            {
            public:
                explicit EquipLoadErrorHandler(IconLayerMenu& owner) : owner_(owner) {}

                void Call(Params& params) override
                {
                    std::string reason = "Scaleform could not load skyui/inventorylists.swf";
                    if (params.argCount >= 2 && params.args[1].IsString()) {
                        reason = std::format("inventorylists.swf load failed: {}", params.args[1].GetString());
                    }
                    owner_.FailEquipLibrary(std::move(reason));
                }

            private:
                IconLayerMenu& owner_;
            };

            void Initialize()
            {
                if (initialized_ || !uiMovie || !uiMovie->GetVariable(std::addressof(root_), "_root") || !root_.IsObject()) {
                    return;
                }

                std::array backgroundArgs{ RE::GFxValue("tfmBackground"), RE::GFxValue(1.0) };
                root_.Invoke("createEmptyMovieClip", std::addressof(background_), backgroundArgs);
                if (!background_.IsDisplayObject()) {
                    SetHostReady(false, "Could not create the Scaleform background clip");
                    return;
                }

                std::array libraryArgs{ RE::GFxValue("tfmMessageBoxLibrary"), RE::GFxValue(10.0) };
                root_.Invoke("createEmptyMovieClip", std::addressof(frameLibrary_), libraryArgs);
                if (!frameLibrary_.IsDisplayObject()) {
                    SetHostReady(false, "Could not create the message-box host clip");
                    return;
                }

                std::array equipLibraryArgs{
                    RE::GFxValue("tfmEquipIconLibrary"),
                    RE::GFxValue(static_cast<double>(kEquipLibraryDepth))
                };
                root_.Invoke("createEmptyMovieClip", std::addressof(equipLibrary_), equipLibraryArgs);
                const bool hasEquipLibraryHost = equipLibrary_.IsDisplayObject();

                loadInitHandler_ = RE::make_gptr<LoadInitHandler>();
                loadErrorHandler_ = RE::make_gptr<LoadErrorHandler>();
                frameLoadInitHandler_ = RE::make_gptr<FrameLoadInitHandler>(*this);
                frameLoadErrorHandler_ = RE::make_gptr<FrameLoadErrorHandler>(*this);
                equipLoadInitHandler_ = RE::make_gptr<EquipLoadInitHandler>(*this);
                equipLoadErrorHandler_ = RE::make_gptr<EquipLoadErrorHandler>(*this);

                uiMovie->CreateObject(std::addressof(frameListener_));
                RE::GFxValue frameInitFunction;
                uiMovie->CreateFunction(std::addressof(frameInitFunction), frameLoadInitHandler_.get());
                frameListener_.SetMember("onLoadInit", frameInitFunction);
                RE::GFxValue frameErrorFunction;
                uiMovie->CreateFunction(std::addressof(frameErrorFunction), frameLoadErrorHandler_.get());
                frameListener_.SetMember("onLoadError", frameErrorFunction);

                uiMovie->CreateObject(std::addressof(frameLoader_), "MovieClipLoader");
                if (!frameListener_.IsObject() || !frameLoader_.IsObject()) {
                    SetHostReady(false, "Could not create the message-box MovieClipLoader");
                    return;
                }
                std::array listenerArgs{ frameListener_ };
                frameLoader_.Invoke("addListener", listenerArgs);

                bool canLoadEquipLibrary = false;
                if (hasEquipLibraryHost) {
                    uiMovie->CreateObject(std::addressof(equipListener_));
                    uiMovie->CreateObject(std::addressof(equipLoader_), "MovieClipLoader");
                    if (equipListener_.IsObject() && equipLoader_.IsObject()) {
                        RE::GFxValue equipInitFunction;
                        uiMovie->CreateFunction(std::addressof(equipInitFunction), equipLoadInitHandler_.get());
                        equipListener_.SetMember("onLoadInit", equipInitFunction);
                        RE::GFxValue equipErrorFunction;
                        uiMovie->CreateFunction(std::addressof(equipErrorFunction), equipLoadErrorHandler_.get());
                        equipListener_.SetMember("onLoadError", equipErrorFunction);
                        std::array equipListenerArgs{ equipListener_ };
                        equipLoader_.Invoke("addListener", equipListenerArgs);
                        canLoadEquipLibrary = true;
                    }
                }

                initialized_ = true;
                root_.SetMember("_visible", false);
                SetHostReady(true);
                SetFrameReady(false);
                if (canLoadEquipLibrary) {
                    std::array equipLoadArgs{ RE::GFxValue(kEquipLibrarySource.data()), equipLibrary_ };
                    equipLoader_.Invoke("loadClip", equipLoadArgs);
                } else {
                    FailEquipLibrary("Could not create the Scaleform equipment-icon library loader");
                }
            }

            [[nodiscard]] FrameInstance AttachFrame(std::string_view name, std::uint32_t depth)
            {
                FrameInstance instance;
                const auto instanceName = std::string(name);
                std::array args{
                    RE::GFxValue(kFrameLinkage.data()),
                    RE::GFxValue(instanceName.c_str()),
                    RE::GFxValue(static_cast<double>(depth))
                };
                frameLibrary_.Invoke("attachMovie", std::addressof(instance.clip), args);
                if (!instance.clip.IsDisplayObject() ||
                    !instance.clip.GetMember(kFrameBackgroundMember.data(), std::addressof(instance.background)) ||
                    !instance.background.IsDisplayObject()) {
                    if (instance.clip.IsDisplayObject()) {
                        instance.clip.Invoke("removeMovieClip");
                    }
                    return {};
                }

                for (const auto memberName : kMessageBoxChromeMembers) {
                    RE::GFxValue member;
                    if (instance.clip.GetMember(memberName.data(), std::addressof(member)) && member.IsDisplayObject()) {
                        member.SetMember("_visible", false);
                    }
                }
                instance.clip.SetMember("enabled", false);
                instance.clip.SetMember("tabEnabled", false);
                instance.clip.SetMember("_visible", false);
                instance.background.SetMember("_visible", false);
                return instance;
            }

            [[nodiscard]] RE::GFxValue AttachEquipIcon(
                std::string_view name,
                std::uint32_t depth,
                EquipState state)
            {
                RE::GFxValue clip;
                const auto instanceName = std::string(name);
                std::array args{
                    RE::GFxValue(kEquipLinkage.data()),
                    RE::GFxValue(instanceName.c_str()),
                    RE::GFxValue(static_cast<double>(depth))
                };
                equipLibrary_.Invoke("attachMovie", std::addressof(clip), args);
                if (!clip.IsDisplayObject()) {
                    return {};
                }

                RE::GFxValue frameLabel(EquipFrameLabel(state).data());
                clip.Invoke("gotoAndStop", nullptr, std::addressof(frameLabel), 1);
                clip.SetMember("enabled", false);
                clip.SetMember("tabEnabled", false);
                clip.SetMember("_visible", false);
                return clip;
            }

            void AcceptFrameLibrary(const RE::GFxValue& library)
            {
                frameLibrary_ = library;
                // messagebox.swf is a complete menu rather than a component library. Park its
                // authored root off-stage and instantiate only the exported MessageBox timeline.
                frameLibrary_.SetMember("_x", static_cast<double>(kFrameLibraryParkingX));

                // The MessageBox linkage normally constructs a full menu controller. This movie is
                // isolated from Skyrim's real MessageBoxMenu, so removing its class binding avoids
                // registering keyboard and GameDelegate callbacks once per favorites tile. Timeline
                // clip actions remain intact, including Dear Diary's UI-specific background loader.
                RE::GFxValue nullClass;
                nullClass.SetNull();
                std::array unregisterArgs{ RE::GFxValue(kFrameLinkage.data()), nullClass };
                RE::GFxValue unregisterResult;
                if (!uiMovie->Invoke(
                        "_global.Object.registerClass",
                        std::addressof(unregisterResult),
                        unregisterArgs.data(),
                        static_cast<std::uint32_t>(unregisterArgs.size()))) {
                    logger::warn("Could not remove the MessageBox controller binding from the isolated frame library");
                }

                auto probe = AttachFrame("tfmMessageBackgroundProbe", 1);
                if (!probe.IsValid()) {
                    FailFrameLibrary("Installed messagebox.swf does not expose MessageBox.Background_mc");
                    return;
                }
                probe.clip.Invoke("removeMovieClip");
                frameReady_ = true;
                SetFrameReady(true);
                logger::info("Loaded the installed Scaleform MessageBox.Background_mc tile design");
            }

            void AcceptEquipLibrary(const RE::GFxValue& library)
            {
                equipLibrary_ = library;
                auto probe = AttachEquipIcon("tfmEquipIconProbe", 1, EquipState::kBoth);
                if (!probe.IsDisplayObject()) {
                    FailEquipLibrary("Installed skyui/inventorylists.swf does not export EquipIcon");
                    return;
                }
                probe.Invoke("removeMovieClip");

                equipLibraryReady_ = true;
                equipLibrarySettled_ = true;
                nextEquipDepth_ = 100;
                layoutRevision_ = 0;
                logger::info("Loaded the installed Scaleform EquipIcon component library");
            }

            void FailEquipLibrary(std::string reason)
            {
                equipLibraryReady_ = false;
                equipLibrarySettled_ = true;
                ClearEquipIcons();
                layoutRevision_ = 0;
                logger::warn("Equipment indicators unavailable: {}", reason);
            }

            void FailFrameLibrary(std::string reason)
            {
                frameReady_ = false;
                ClearFrames();
                logger::error("{}", reason);
                SetFrameReady(false, std::move(reason));
            }

            void Synchronize()
            {
                if (!initialized_) {
                    Initialize();
                }
                if (!initialized_) {
                    return;
                }

                auto snapshot = CaptureMenuSnapshot();
                root_.SetMember("_visible", false);
                if (!snapshot.active) {
                    return;
                }

                if (snapshot.descriptorRevision != descriptorRevision_) {
                    if (snapshot.iconsEnabled) {
                        RebuildIcons(snapshot);
                    } else {
                        ClearIcons();
                    }
                    descriptorRevision_ = snapshot.descriptorRevision;
                    layoutRevision_ = 0;
                }
                const bool layoutChanged = snapshot.layoutRevision != layoutRevision_;
                if (layoutChanged) {
                    DrawBackdrop(snapshot.layout);
                    if (snapshot.iconsEnabled) {
                        PositionIcons(snapshot.layout);
                    }
                    layoutRevision_ = snapshot.layoutRevision;
                }
                const bool framesReady = SynchronizeFrames(snapshot.layout, layoutChanged);
                const bool equipIconsReady = SynchronizeEquipIcons(snapshot.layout, layoutChanged);
                const bool sceneReady = framesReady && equipIconsReady;
                root_.SetMember(
                    "_visible",
                    sceneReady && TryMakePresentationReady(descriptorRevision_, layoutRevision_));
            }

            void ClearIcons()
            {
                for (auto& slot : slots_) {
                    if (slot.clip.IsDisplayObject()) {
                        slot.clip.Invoke("removeMovieClip");
                    }
                }
                slots_.clear();
            }

            void ClearEquipIcons()
            {
                for (auto& slot : equipSlots_) {
                    if (slot.clip.IsDisplayObject()) {
                        slot.clip.Invoke("removeMovieClip");
                    }
                }
                equipSlots_.clear();
                nextEquipDepth_ = 100;
            }

            void ClearFrames()
            {
                for (auto& slot : frameSlots_) {
                    if (slot.frame.clip.IsDisplayObject()) {
                        slot.frame.clip.Invoke("removeMovieClip");
                    }
                }
                frameSlots_.clear();
                if (ghostFrame_.clip.IsDisplayObject()) {
                    ghostFrame_.clip.Invoke("removeMovieClip");
                }
                ghostFrame_ = {};
                nextFrameDepth_ = 100;
            }

            void RebuildIcons(const MenuSnapshot& snapshot)
            {
                ClearIcons();
                std::uint32_t slotIndex = 0;
                for (const auto& key : snapshot.items) {
                    const auto descriptor = snapshot.descriptors.find(key);
                    if (descriptor == snapshot.descriptors.end()) {
                        continue;
                    }

                    IconSlot slot;
                    slot.key = key;
                    const auto clipName = std::format("tfmIcon{}", slotIndex);
                    std::array clipArgs{
                        RE::GFxValue(clipName.c_str()),
                        RE::GFxValue(static_cast<double>(1000U + slotIndex))
                    };
                    root_.Invoke("createEmptyMovieClip", std::addressof(slot.clip), clipArgs);
                    if (!slot.clip.IsDisplayObject()) {
                        ReportFailed(key, "Could not create an icon MovieClip");
                        ++slotIndex;
                        continue;
                    }
                    slot.clip.SetMember("_visible", false);

                    uiMovie->CreateObject(std::addressof(slot.listener));
                    slot.listener.SetMember("formID", static_cast<double>(key.formID));
                    slot.listener.SetMember("uniqueID", static_cast<double>(key.uniqueID));
                    slot.listener.SetMember("iconLabel", descriptor->second.label.c_str());
                    slot.listener.SetMember("iconColor", static_cast<double>(descriptor->second.color));

                    RE::GFxValue initFunction;
                    uiMovie->CreateFunction(std::addressof(initFunction), loadInitHandler_.get());
                    slot.listener.SetMember("onLoadInit", initFunction);
                    RE::GFxValue errorFunction;
                    uiMovie->CreateFunction(std::addressof(errorFunction), loadErrorHandler_.get());
                    slot.listener.SetMember("onLoadError", errorFunction);

                    uiMovie->CreateObject(std::addressof(slot.loader), "MovieClipLoader");
                    if (!slot.loader.IsObject()) {
                        ReportFailed(key, "Could not create MovieClipLoader");
                        slot.clip.Invoke("removeMovieClip");
                        ++slotIndex;
                        continue;
                    }
                    std::array listenerArgs{ slot.listener };
                    slot.loader.Invoke("addListener", listenerArgs);
                    std::array loadArgs{ RE::GFxValue(descriptor->second.source.c_str()), slot.clip };
                    slot.loader.Invoke("loadClip", loadArgs);
                    slots_.push_back(std::move(slot));
                    ++slotIndex;
                }
            }

            [[nodiscard]] Rect ToStageRect(const Rect& rect, const LayoutSnapshot& layout, const RE::GRectF& visible) const
            {
                const auto scaleX = (visible.right - visible.left) / std::max(1.0f, layout.screenWidth);
                const auto scaleY = (visible.bottom - visible.top) / std::max(1.0f, layout.screenHeight);
                return {
                    visible.left + rect.x * scaleX,
                    visible.top + rect.y * scaleY,
                    rect.width * scaleX,
                    rect.height * scaleY
                };
            }

            void DrawBackdrop(const LayoutSnapshot& layout)
            {
                if (!uiMovie || !background_.IsDisplayObject() || layout.screenWidth <= 0.0f || layout.screenHeight <= 0.0f) {
                    return;
                }
                background_.Invoke("clear");
                const auto visible = uiMovie->GetVisibleFrameRect();
                const Rect screen{ visible.left, visible.top, visible.right - visible.left, visible.bottom - visible.top };
                DrawRoundRect(
                    background_,
                    screen,
                    0.0f,
                    Theme::kBackdropColor,
                    std::clamp(layout.backdropAlpha, 0.0f, 1.0f) * 100.0f);

                if (!layout.useFrames) {
                    for (const auto& tile : layout.tiles) {
                        DrawRoundRect(
                            background_,
                            ToStageRect(tile.bounds, layout, visible),
                            Theme::kCornerRadius,
                            Theme::kBackdropColor,
                            Theme::TileAlpha(
                                layout.tileAlpha,
                                tile.hovered,
                                tile.equipState != EquipState::kNone,
                                tile.dropTarget) *
                                100.0f);
                    }
                    if (layout.dragGhost) {
                        DrawRoundRect(
                            background_,
                            ToStageRect(*layout.dragGhost, layout, visible),
                            Theme::kCornerRadius,
                            Theme::kBackdropColor,
                            std::max(layout.tileAlpha, 0.90f) * 100.0f);
                    }
                }
            }

            [[nodiscard]] bool RefreshFrameReadiness(FrameInstance& frame)
            {
                if (!frame.IsValid()) {
                    return false;
                }

                ++frame.ageFrames;
                RE::GFxValue sourceValue;
                std::string source;
                if (frame.background.GetMember("_url", std::addressof(sourceValue)) && sourceValue.IsString()) {
                    source = sourceValue.GetString();
                }
                if (source != frame.source) {
                    frame.source = std::move(source);
                    auto normalizedSource = frame.source;
                    std::ranges::transform(normalizedSource, normalizedSource.begin(), [](unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    });
                    const bool isDearDiary = normalizedSource.contains(kDearDiaryBackgroundSourceMarker);
                    frame.contentScaleX = isDearDiary ? kDearDiaryBackgroundScaleX : 1.0f;
                    frame.contentScaleY = isDearDiary ? kDearDiaryBackgroundScaleY : 1.0f;
                    frame.contentOffsetX = isDearDiary ? kDearDiaryBackgroundOffsetX : 0.0f;
                    frame.contentOffsetY = isDearDiary ? kDearDiaryBackgroundOffsetY : 0.0f;
                    frame.ageFrames = 0;
                    frame.stableFrames = 0;
                    frame.ready = false;
                } else {
                    ++frame.stableFrames;
                }

                if (frame.ready) {
                    return false;
                }

                bool bytesSettled = true;
                RE::GFxValue bytesLoaded;
                RE::GFxValue bytesTotal;
                if (frame.background.Invoke("getBytesLoaded", std::addressof(bytesLoaded)) && bytesLoaded.IsNumber() &&
                    frame.background.Invoke("getBytesTotal", std::addressof(bytesTotal)) && bytesTotal.IsNumber() &&
                    bytesTotal.GetNumber() > 0.0) {
                    bytesSettled = bytesLoaded.GetNumber() >= bytesTotal.GetNumber();
                }

                RE::GFxValue width;
                RE::GFxValue height;
                const bool hasContent =
                    frame.background.GetMember("_width", std::addressof(width)) && width.IsNumber() && width.GetNumber() > 0.0 &&
                    frame.background.GetMember("_height", std::addressof(height)) && height.IsNumber() && height.GetNumber() > 0.0;

                if (frame.ageFrames < kFrameMinimumSettleFrames || frame.stableFrames < 2 || !bytesSettled || !hasContent) {
                    return false;
                }

                frame.ready = true;
                return true;
            }

            void PositionFrame(FrameInstance& frame, const Rect& bounds, float alpha, const LayoutSnapshot& layout)
            {
                if (!uiMovie || !frame.IsValid() || !frame.ready) {
                    return;
                }

                const auto borderScale = std::clamp(layout.borderScale, 0.25f, 4.0f);
                const auto stageRect = ToStageRect(bounds, layout, uiMovie->GetVisibleFrameRect());
                frame.clip.SetMember(
                    "_x",
                    static_cast<double>((stageRect.CenterX() - kFrameLibraryParkingX) / borderScale));
                frame.clip.SetMember("_y", static_cast<double>(stageRect.CenterY() / borderScale));
                frame.clip.SetMember("_xscale", 100.0);
                frame.clip.SetMember("_yscale", 100.0);
                frame.background.SetMember(
                    "_x",
                    static_cast<double>(stageRect.width / borderScale * frame.contentOffsetX * layout.frameScale));
                frame.background.SetMember(
                    "_y",
                    static_cast<double>(stageRect.height / borderScale * frame.contentOffsetY * layout.frameScale));
                frame.background.SetMember(
                    "_width",
                    static_cast<double>(stageRect.width / borderScale * frame.contentScaleX * layout.frameScale));
                frame.background.SetMember(
                    "_height",
                    static_cast<double>(stageRect.height / borderScale * frame.contentScaleY * layout.frameScale));
                frame.clip.SetMember("_alpha", static_cast<double>(std::clamp(alpha, 0.0f, 1.0f) * 100.0f));
                frame.background.SetMember("_visible", true);
                frame.clip.SetMember("_visible", true);
            }

            [[nodiscard]] bool SynchronizeFrames(const LayoutSnapshot& layout, bool layoutChanged)
            {
                if (layout.screenWidth <= 0.0f || layout.screenHeight <= 0.0f) {
                    return false;
                }
                if (!layout.useFrames) {
                    if (!frameSlots_.empty() || ghostFrame_.IsValid()) {
                        ClearFrames();
                    }
                    return true;
                }
                if (!frameLibraryLoadStarted_) {
                    frameLibraryLoadStarted_ = true;
                    std::array loadArgs{ RE::GFxValue(kFrameLibrarySource.data()), frameLibrary_ };
                    frameLoader_.Invoke("loadClip", loadArgs);
                    return false;
                }
                if (!frameReady_ || !frameLibrary_.IsDisplayObject()) {
                    return false;
                }

                const auto borderScale = std::clamp(layout.borderScale, 0.25f, 4.0f);
                frameLibrary_.SetMember("_xscale", static_cast<double>(borderScale * 100.0f));
                frameLibrary_.SetMember("_yscale", static_cast<double>(borderScale * 100.0f));

                for (auto slot = frameSlots_.begin(); slot != frameSlots_.end();) {
                    if (std::ranges::find(layout.tiles, slot->key, &Tile::key) != layout.tiles.end()) {
                        ++slot;
                        continue;
                    }
                    if (slot->frame.clip.IsDisplayObject()) {
                        slot->frame.clip.Invoke("removeMovieClip");
                    }
                    slot = frameSlots_.erase(slot);
                }

                for (const auto& tile : layout.tiles) {
                    auto found = std::ranges::find(frameSlots_, tile.key, &FrameSlot::key);
                    if (found == frameSlots_.end()) {
                        auto clip = AttachFrame(
                            std::format("tfmMessageBackground{}", nextFrameDepth_),
                            nextFrameDepth_++);
                        if (!clip.IsValid()) {
                            FailFrameLibrary("Could not instantiate MessageBox.Background_mc for a favorites tile");
                            return false;
                        }
                        frameSlots_.push_back({ tile.key, std::move(clip) });
                        found = std::prev(frameSlots_.end());
                    }
                    const bool becameReady = RefreshFrameReadiness(found->frame);
                    if (layoutChanged || becameReady) {
                        PositionFrame(
                            found->frame,
                            tile.bounds,
                            Theme::TileAlpha(
                                layout.tileAlpha,
                                tile.hovered,
                                tile.equipState != EquipState::kNone,
                                tile.dropTarget),
                            layout);
                    }
                }

                if (!ghostFrame_.IsValid()) {
                    ghostFrame_ = AttachFrame("tfmDragMessageBackground", 900000);
                    if (!ghostFrame_.IsValid()) {
                        FailFrameLibrary("Could not instantiate MessageBox.Background_mc for the drag preview");
                        return false;
                    }
                }
                const bool ghostBecameReady = RefreshFrameReadiness(ghostFrame_);
                if (layout.dragGhost && (layoutChanged || ghostBecameReady)) {
                    PositionFrame(ghostFrame_, *layout.dragGhost, std::max(layout.tileAlpha, 0.90f), layout);
                } else if (!layout.dragGhost) {
                    ghostFrame_.clip.SetMember("_visible", false);
                }

                return std::ranges::all_of(frameSlots_, [](const FrameSlot& slot) {
                    return slot.frame.ready;
                });
            }

            void PositionEquipIcon(EquipSlot& slot, const Rect& indicator, const LayoutSnapshot& layout)
            {
                if (!uiMovie || !slot.clip.IsDisplayObject() ||
                    layout.screenWidth <= 0.0f || layout.screenHeight <= 0.0f) {
                    return;
                }

                const auto visible = uiMovie->GetVisibleFrameRect();
                const auto scaleX = (visible.right - visible.left) / std::max(1.0f, layout.screenWidth);
                const auto scaleY = (visible.bottom - visible.top) / std::max(1.0f, layout.screenHeight);
                const auto uniformScale = std::min(scaleX, scaleY);
                const auto iconSize = std::max(1.0f, std::min(indicator.width, indicator.height));
                const auto iconScale = iconSize * uniformScale / kEquipIconAuthoringSize * 100.0f;

                RE::GFxValue::DisplayInfo display;
                display.SetPosition(
                    visible.left + indicator.x * scaleX,
                    visible.top + indicator.y * scaleY);
                display.SetScale(iconScale, iconScale);
                slot.clip.SetDisplayInfo(display);
                slot.clip.SetMember("_visible", true);
            }

            [[nodiscard]] bool SynchronizeEquipIcons(const LayoutSnapshot& layout, bool layoutChanged)
            {
                if (!equipLibrarySettled_) {
                    return false;
                }
                if (!equipLibraryReady_ || !equipLibrary_.IsDisplayObject()) {
                    return true;
                }

                for (auto slot = equipSlots_.begin(); slot != equipSlots_.end();) {
                    const auto tile = std::ranges::find(layout.tiles, slot->key, &Tile::key);
                    if (tile != layout.tiles.end() && tile->equipState != EquipState::kNone) {
                        ++slot;
                        continue;
                    }
                    if (slot->clip.IsDisplayObject()) {
                        slot->clip.Invoke("removeMovieClip");
                    }
                    slot = equipSlots_.erase(slot);
                }

                for (const auto& tile : layout.tiles) {
                    if (tile.equipState == EquipState::kNone) {
                        continue;
                    }

                    auto found = std::ranges::find(equipSlots_, tile.key, &EquipSlot::key);
                    bool created = false;
                    bool stateChanged = false;
                    if (found == equipSlots_.end()) {
                        auto clip = AttachEquipIcon(
                            std::format("tfmEquipIcon{}", nextEquipDepth_),
                            nextEquipDepth_++,
                            tile.equipState);
                        if (!clip.IsDisplayObject()) {
                            FailEquipLibrary("Could not instantiate EquipIcon for a favorites tile");
                            return true;
                        }
                        equipSlots_.push_back({ tile.key, tile.equipState, std::move(clip) });
                        found = std::prev(equipSlots_.end());
                        created = true;
                    } else if (found->state != tile.equipState) {
                        RE::GFxValue frameLabel(EquipFrameLabel(tile.equipState).data());
                        found->clip.Invoke("gotoAndStop", nullptr, std::addressof(frameLabel), 1);
                        found->state = tile.equipState;
                        stateChanged = true;
                    }

                    if (layoutChanged || created || stateChanged) {
                        auto indicator = tile.equipIndicator;
                        if (layout.dragGhostItem == tile.key && layout.dragGhostEquipIndicator) {
                            indicator = *layout.dragGhostEquipIndicator;
                        }
                        PositionEquipIcon(*found, indicator, layout);
                    }
                }
                return true;
            }

            void PositionIcons(const LayoutSnapshot& layout)
            {
                if (!uiMovie || layout.screenWidth <= 0.0f || layout.screenHeight <= 0.0f) {
                    return;
                }
                const auto visible = uiMovie->GetVisibleFrameRect();
                const auto scaleX = (visible.right - visible.left) / std::max(1.0f, layout.screenWidth);
                const auto scaleY = (visible.bottom - visible.top) / std::max(1.0f, layout.screenHeight);
                const auto uniformScale = std::min(scaleX, scaleY);

                for (auto& slot : slots_) {
                    const auto found = std::ranges::find(layout.tiles, slot.key, &Tile::key);
                    if (found == layout.tiles.end() || !slot.clip.IsDisplayObject()) {
                        slot.clip.SetMember("_visible", false);
                        continue;
                    }

                    auto preview = found->preview;
                    if (layout.dragGhost && layout.dragGhostItem == slot.key) {
                        preview = {
                            layout.dragGhost->x + 8.0f,
                            layout.dragGhost->y + 8.0f,
                            std::max(1.0f, layout.dragGhost->width - 16.0f),
                            std::max(1.0f, layout.dragGhost->height - 66.0f)
                        };
                    }

                    const auto available = std::max(1.0f, std::min(preview.width, preview.height));
                    const auto targetPixels = std::min(220.0f, std::max(std::min(48.0f, available), available * 0.68f));
                    const auto targetStage = targetPixels * uniformScale;
                    const auto centerX = visible.left + preview.CenterX() * scaleX;
                    const auto centerY = visible.top + preview.CenterY() * scaleY;

                    RE::GFxValue::DisplayInfo display;
                    display.SetPosition(centerX - targetStage * 0.5f, centerY - targetStage * 0.5f);
                    const auto iconScale = targetStage / kIconLibrarySize * 100.0f;
                    display.SetScale(iconScale, iconScale);
                    slot.clip.SetDisplayInfo(display);
                }
            }

            bool initialized_{ false };
            bool frameReady_{ false };
            bool frameLibraryLoadStarted_{ false };
            bool equipLibraryReady_{ false };
            bool equipLibrarySettled_{ false };
            std::uint32_t nextFrameDepth_{ 100 };
            std::uint32_t nextEquipDepth_{ 100 };
            std::uint64_t descriptorRevision_{ 0 };
            std::uint64_t layoutRevision_{ 0 };
            RE::GFxValue root_;
            RE::GFxValue background_;
            RE::GFxValue frameLibrary_;
            RE::GFxValue frameLoader_;
            RE::GFxValue frameListener_;
            RE::GFxValue equipLibrary_;
            RE::GFxValue equipLoader_;
            RE::GFxValue equipListener_;
            FrameInstance ghostFrame_;
            RE::GPtr<LoadInitHandler> loadInitHandler_;
            RE::GPtr<LoadErrorHandler> loadErrorHandler_;
            RE::GPtr<FrameLoadInitHandler> frameLoadInitHandler_;
            RE::GPtr<FrameLoadErrorHandler> frameLoadErrorHandler_;
            RE::GPtr<EquipLoadInitHandler> equipLoadInitHandler_;
            RE::GPtr<EquipLoadErrorHandler> equipLoadErrorHandler_;
            std::vector<IconSlot> slots_;
            std::vector<FrameSlot> frameSlots_;
            std::vector<EquipSlot> equipSlots_;
        };
    }

    bool Register()
    {
        auto& state = State();
        {
            std::scoped_lock lock(state.mutex);
            if (state.registered) {
                return true;
            }
        }

        const auto ui = RE::UI::GetSingleton();
        if (!ui) {
            logger::error("Could not register the Scaleform icon layer: Skyrim UI is unavailable");
            return false;
        }
        ui->Register(kMenuName, IconLayerMenu::Create);
        {
            std::scoped_lock lock(state.mutex);
            state.registered = true;
        }
        return true;
    }

    void SetActive(bool active, bool iconsEnabled)
    {
        bool updateHost = false;
        bool cancelProbe = false;
        {
            auto& state = State();
            std::scoped_lock lock(state.mutex);
            const auto wasActive = state.active;
            const auto modeChanged = state.iconsEnabled != iconsEnabled;
            state.active = active;
            state.iconsEnabled = active && iconsEnabled;
            if (active && !wasActive) {
                state.layout = {};
            }
            ++state.layoutRevision;
            if (active && (!wasActive || modeChanged)) {
                state.loaded.clear();
                state.failed.clear();
                state.presentationReady = false;
                ++state.descriptorRevision;
                if (state.iconsEnabled) {
                    state.phase = state.descriptors.empty() ? "waiting" : "ready";
                    state.probeRequested = !state.items.empty() && !HasEveryDescriptor(state);
                } else {
                    state.probeRequested = false;
                    state.phase = "ready";
                }
            } else if (!active) {
                state.presentationReady = false;
                state.probeRequested = false;
            }
            cancelProbe = !state.active || !state.iconsEnabled;
            updateHost = state.registered;
        }

        if (cancelProbe) {
            CancelProbe();
        }
        if (updateHost) {
            if (const auto queue = RE::UIMessageQueue::GetSingleton()) {
                queue->AddMessage(
                    kMenuName.data(),
                    active ? RE::UI_MESSAGE_TYPE::kShow : RE::UI_MESSAGE_TYPE::kHide,
                    nullptr);
            }
        }
    }

    void Reconcile(const std::vector<FavoriteItem>& items)
    {
        std::vector<ItemKey> keys;
        keys.reserve(items.size());
        for (const auto& item : items) {
            keys.push_back(item.key);
        }

        auto& state = State();
        std::scoped_lock lock(state.mutex);
        if (keys == state.items) {
            return;
        }
        state.items = std::move(keys);
        state.descriptors.clear();
        state.loaded.clear();
        state.failed.clear();
        state.presentationReady = false;
        ++state.descriptorRevision;
        if (state.probeOpening) {
            state.restartProbe = true;
        } else {
            state.probeRequested = state.active && state.iconsEnabled;
        }
        state.phase = state.active ? (state.iconsEnabled ? "waiting" : "ready") : "disabled";
        state.error.clear();
    }

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
        float frameScale)
    {
        LayoutSnapshot next;
        next.screenWidth = screenWidth;
        next.screenHeight = screenHeight;
        next.backdropAlpha = backdropAlpha;
        next.tileAlpha = tileAlpha;
        next.useFrames = useFrames;
        next.borderScale = borderScale;
        next.frameScale = frameScale;
        next.tiles.assign(tiles.begin(), tiles.end());
        next.dragGhost = dragGhost;
        next.dragGhostItem = dragGhostItem;
        next.dragGhostEquipIndicator = dragGhostEquipIndicator;

        auto& state = State();
        std::scoped_lock lock(state.mutex);
        if (state.layout != next) {
            state.layout = std::move(next);
            ++state.layoutRevision;
        }
    }

    bool HandleFavoritesMenuOpened()
    {
        bool isProbe = false;
        {
            auto& state = State();
            std::scoped_lock lock(state.mutex);
            if (state.active && state.iconsEnabled) {
                isProbe = state.probeOpening;
                if (!isProbe && state.probeRequested && !state.items.empty()) {
                    state.probeRequested = false;
                    state.probeOpening = true;
                    state.probeFrames = 0;
                    state.phase = "resolving";
                    state.error.clear();
                    isProbe = true;
                }
            }
        }
        if (!isProbe) {
            return false;
        }

        if (const auto ui = RE::UI::GetSingleton()) {
            if (const auto menu = ui->GetMenu<RE::FavoritesMenu>(); menu && menu->uiMovie) {
                menu->uiMovie->SetVisible(false);
            }
        }
        return true;
    }

    PresentationState GetPresentationState()
    {
        auto& state = State();
        std::scoped_lock lock(state.mutex);
        if (!state.active) {
            return PresentationState::kPending;
        }
        if (state.hostFailed || (state.layout.useFrames && state.frameFailed)) {
            return PresentationState::kFailed;
        }
        return state.presentationReady ? PresentationState::kReady : PresentationState::kPending;
    }

}
