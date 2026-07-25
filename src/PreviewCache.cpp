#include "PreviewCache.h"

#include "Config.h"
#include "MeshRenderingFrameworkAPI.h"
#include "logger.h"

#include <d3d11.h>
#include <wrl/client.h>

namespace TFM
{
    namespace
    {
        constexpr std::uint64_t kPreviewTimeoutFrames = 180;
        constexpr std::uint64_t kStaticValidationDelayFrames = 2;
        constexpr std::uint64_t kValidationIntervalFrames = 4;
        constexpr std::uint32_t kAnimatedPreviewResolution = 1024;
        constexpr std::size_t kAnimatedCalibrationSamples = 9;
        constexpr std::uint8_t kVisibleAlphaThreshold = 16;
        constexpr float kAnimatedAlphaTrimFraction = 0.01f;
        constexpr std::size_t kPrewarmNewEntriesPerFrame = 1;

        struct AlphaBounds
        {
            std::uint32_t left{ 0 };
            std::uint32_t top{ 0 };
            std::uint32_t right{ 0 };
            std::uint32_t bottom{ 0 };
            std::uint32_t textureWidth{ 0 };
            std::uint32_t textureHeight{ 0 };
        };

        [[nodiscard]] std::optional<AlphaBounds> MeasureAlphaBounds(
            ID3D11ShaderResourceView* view,
            std::uint8_t alphaThreshold = kVisibleAlphaThreshold,
            float alphaTrimFraction = 0.0f,
            ID3D11Texture2D* normalizedTexture = nullptr)
        {
            const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!view || !renderer) {
                return std::nullopt;
            }

            auto& rendererData = renderer->GetRuntimeData();
            auto* device = reinterpret_cast<ID3D11Device*>(rendererData.forwarder);
            auto* context = reinterpret_cast<ID3D11DeviceContext*>(rendererData.context);
            if (!device || !context) {
                return std::nullopt;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            if (!resource) {
                return std::nullopt;
            }

            Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
            const auto queryResult = resource.As(std::addressof(source));
            if (FAILED(queryResult) || !source) {
                return std::nullopt;
            }

            D3D11_TEXTURE2D_DESC description{};
            source->GetDesc(std::addressof(description));
            if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM || description.SampleDesc.Count != 1) {
                return std::nullopt;
            }

            auto stagingDescription = description;
            stagingDescription.Usage = D3D11_USAGE_STAGING;
            stagingDescription.BindFlags = 0;
            stagingDescription.CPUAccessFlags =
                D3D11_CPU_ACCESS_READ |
                (normalizedTexture ? D3D11_CPU_ACCESS_WRITE : 0);
            stagingDescription.MiscFlags = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
            const auto createResult = device->CreateTexture2D(
                std::addressof(stagingDescription),
                nullptr,
                staging.GetAddressOf());
            if (FAILED(createResult) || !staging) {
                return std::nullopt;
            }

            context->CopyResource(staging.Get(), source.Get());

            D3D11_MAPPED_SUBRESOURCE mapped{};
            const auto mapResult = context->Map(
                staging.Get(),
                0,
                normalizedTexture ? D3D11_MAP_READ_WRITE : D3D11_MAP_READ,
                0,
                std::addressof(mapped));
            if (FAILED(mapResult)) {
                return std::nullopt;
            }

            AlphaBounds bounds{
                description.Width,
                description.Height,
                0,
                0,
                description.Width,
                description.Height
            };
            std::vector<std::uint64_t> columnAlpha;
            std::vector<std::uint64_t> rowAlpha;
            if (alphaTrimFraction > 0.0f) {
                columnAlpha.resize(description.Width);
                rowAlpha.resize(description.Height);
            }

            std::uint64_t totalAlpha = 0;
            bool found = false;
            for (std::uint32_t y = 0; y < description.Height; ++y) {
                auto* row = static_cast<std::uint8_t*>(mapped.pData) + mapped.RowPitch * y;
                for (std::uint32_t x = 0; x < description.Width; ++x) {
                    auto* pixel = row + x * 4;
                    auto alpha = pixel[3];
                    if (normalizedTexture &&
                        (pixel[0] != 0 || pixel[1] != 0 || pixel[2] != 0)) {
                        alpha = 255;
                        pixel[3] = alpha;
                    }
                    if (alpha <= alphaThreshold) {
                        continue;
                    }
                    found = true;
                    bounds.left = std::min(bounds.left, x);
                    bounds.top = std::min(bounds.top, y);
                    bounds.right = std::max(bounds.right, x + 1);
                    bounds.bottom = std::max(bounds.bottom, y + 1);
                    if (!columnAlpha.empty()) {
                        const auto alphaWeight = static_cast<std::uint64_t>(alpha - alphaThreshold);
                        columnAlpha[x] += alphaWeight;
                        rowAlpha[y] += alphaWeight;
                        totalAlpha += alphaWeight;
                    }
                }
            }

            context->Unmap(staging.Get(), 0);
            if (normalizedTexture) {
                context->CopyResource(normalizedTexture, staging.Get());
            }
            if (!found) {
                return std::nullopt;
            }
            if (columnAlpha.empty() || totalAlpha == 0) {
                return bounds;
            }

            const auto trimAlpha = static_cast<std::uint64_t>(
                static_cast<double>(totalAlpha) * std::clamp(alphaTrimFraction, 0.0f, 0.49f));
            const auto trimAxis = [trimAlpha](const std::vector<std::uint64_t>& alpha) {
                std::uint64_t accumulated = 0;
                std::uint32_t first = 0;
                for (; first < alpha.size(); ++first) {
                    accumulated += alpha[first];
                    if (accumulated > trimAlpha) {
                        break;
                    }
                }

                accumulated = 0;
                auto last = static_cast<std::uint32_t>(alpha.size());
                while (last > 0) {
                    accumulated += alpha[last - 1];
                    if (accumulated > trimAlpha) {
                        break;
                    }
                    --last;
                }
                return std::pair{ first, last };
            };

            const auto [left, right] = trimAxis(columnAlpha);
            const auto [top, bottom] = trimAxis(rowAlpha);
            if (left < right && top < bottom) {
                bounds.left = left;
                bounds.top = top;
                bounds.right = right;
                bounds.bottom = bottom;
            }
            return bounds;
        }

        [[nodiscard]] bool EnsureStaticTexture(
            ID3D11ShaderResourceView* view,
            Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture,
            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView>& textureView)
        {
            if (!view) {
                return false;
            }
            if (texture && textureView) {
                return true;
            }

            const auto renderer = RE::BSGraphics::Renderer::GetSingleton();
            if (!renderer) {
                return false;
            }

            auto* device = reinterpret_cast<ID3D11Device*>(renderer->GetRuntimeData().forwarder);
            if (!device) {
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11Resource> resource;
            view->GetResource(resource.GetAddressOf());
            Microsoft::WRL::ComPtr<ID3D11Texture2D> source;
            if (!resource || FAILED(resource.As(std::addressof(source))) || !source) {
                return false;
            }

            D3D11_TEXTURE2D_DESC description{};
            source->GetDesc(std::addressof(description));
            if (description.Format != DXGI_FORMAT_R8G8B8A8_UNORM || description.SampleDesc.Count != 1) {
                return false;
            }

            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            description.CPUAccessFlags = 0;
            description.MiscFlags = 0;

            Microsoft::WRL::ComPtr<ID3D11Texture2D> createdTexture;
            if (FAILED(device->CreateTexture2D(
                    std::addressof(description),
                    nullptr,
                    createdTexture.GetAddressOf()))) {
                return false;
            }

            Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> createdView;
            if (FAILED(device->CreateShaderResourceView(
                    createdTexture.Get(),
                    nullptr,
                    createdView.GetAddressOf()))) {
                return false;
            }

            texture = std::move(createdTexture);
            textureView = std::move(createdView);
            return true;
        }

        [[nodiscard]] std::uint32_t GetAnimatedPreviewHeight(std::uint32_t width)
        {
            const auto screen = RE::BSGraphics::Renderer::GetScreenSize();
            if (screen.width == 0 || screen.height == 0) {
                return width;
            }
            return std::max(
                1u,
                static_cast<std::uint32_t>(std::lround(
                    static_cast<double>(width) * screen.height / screen.width)));
        }

        [[nodiscard]] std::optional<float> GetInventoryZoom(RE::TESBoundObject* object)
        {
            const auto modelPath = MeshRenderingFrameworkAPI::Internal::GetModelPathFromBaseObject(object);
            if (!modelPath || !modelPath[0]) {
                return std::nullopt;
            }

            RE::BSModelDB::DBTraits::ArgsType args;
            RE::NiPointer<RE::NiNode> model;
            if (RE::BSModelDB::Demand(modelPath, model, args) != RE::BSResource::ErrorCode::kNone || !model) {
                return std::nullopt;
            }

            const auto marker = netimmerse_cast<RE::BSInvMarker*>(model->GetExtraData("INV"));
            if (!marker) {
                return std::nullopt;
            }
            const auto zoom = marker->zoom;
            return std::isfinite(zoom) && zoom > 0.0f ? std::optional{ zoom } : std::nullopt;
        }

        [[nodiscard]] bool IsAnimatedPreview(const RE::TESForm* form)
        {
            if (!form) {
                return false;
            }
            const auto formType = form->GetFormType();
            return formType == RE::FormType::Spell || formType == RE::FormType::Shout;
        }

    }

    enum class PreviewEntryState : std::uint8_t
    {
        kPending,
        kReady,
        kFailed
    };

    class PreviewEntry
    {
    public:
        PreviewEntry(RE::TESBoundObject* object, std::uint32_t resolution, bool animated, std::uint64_t createdFrame) :
            createdFrame_(createdFrame),
            animated_(animated),
            nextCalibrationFrame_(createdFrame + (animated ? 12 : kStaticValidationDelayFrames)),
            mesh_(object, resolution, animated ? GetAnimatedPreviewHeight(resolution) : resolution)
        {
            if (animated) {
                mesh_.SetAlwaysUpdate(true);
            }
            if (const auto zoom = GetInventoryZoom(object)) {
                mesh_.ScaleUp(*zoom);
            }
        }

        [[nodiscard]] PreviewResult Result() const { return { stableTexture_.Get(), placement_ }; }
        void SetVisible(bool visible)
        {
            visible_ = visible;
            UpdateAnimationState();
        }

        [[nodiscard]] PreviewEntryState Advance(std::uint64_t frame)
        {
            if (!mesh_.IsValid()) {
                return PreviewEntryState::kFailed;
            }

            auto* texture = mesh_.GetResourceView();
            if (!animated_) {
                return AdvanceStatic(texture, frame);
            }

            if (calibrated_) {
                RememberTexture(texture);
                return stableTexture_ ? PreviewEntryState::kReady : PreviewEntryState::kPending;
            }

            if (!texture) {
                if (frame - createdFrame_ >= kPreviewTimeoutFrames) {
                    return PreviewEntryState::kFailed;
                }
                return PreviewEntryState::kPending;
            }

            if (!initialAnimatedRefreshRequested_) {
                RequestRefresh();
                initialAnimatedRefreshRequested_ = true;
                return PreviewEntryState::kPending;
            }
            CalibrateAnimated(texture, frame);
            if (!calibrated_) {
                return PreviewEntryState::kPending;
            }

            RememberTexture(texture);
            return stableTexture_ ? PreviewEntryState::kReady : PreviewEntryState::kPending;
        }

    private:
        [[nodiscard]] PreviewEntryState AdvanceStatic(ID3D11ShaderResourceView* texture, std::uint64_t frame)
        {
            if (!texture) {
                if (frame - createdFrame_ >= kPreviewTimeoutFrames) {
                    return PreviewEntryState::kReady;
                }
                return PreviewEntryState::kPending;
            }

            if (!calibrated_ &&
                (!staticTextureView_ || frame >= nextCalibrationFrame_)) {
                if (EnsureStaticTexture(texture, staticTexture_, staticTextureView_)) {
                    staticBounds_ = MeasureAlphaBounds(
                        texture,
                        0,
                        0.0f,
                        staticTexture_.Get());
                } else {
                    staticBounds_ = MeasureAlphaBounds(texture, 0);
                }
            }

            RememberTexture(staticTextureView_ ? staticTextureView_.Get() : texture);
            ValidateStatic(staticBounds_, frame);
            return stableTexture_ ? PreviewEntryState::kReady : PreviewEntryState::kPending;
        }

        void RememberTexture(ID3D11ShaderResourceView* texture)
        {
            if (!texture || stableTexture_.Get() == texture) {
                return;
            }
            if (stableTexture_ && !MeasureAlphaBounds(texture, animated_ ? kVisibleAlphaThreshold : 0)) {
                return;
            }
            stableTexture_ = texture;
        }

        void RequestRefresh()
        {
            RE::NiMatrix3 orientation;
            orientation.SetEulerAnglesXYZ(0.0f, 0.0f, 0.0f);
            mesh_.SetRotation(orientation);
        }

        void ValidateStatic(const std::optional<AlphaBounds>& bounds, std::uint64_t frame)
        {
            if (calibrated_ || frame < nextCalibrationFrame_) {
                return;
            }
            nextCalibrationFrame_ = frame + kValidationIntervalFrames;
            if (bounds) {
                const auto offsetX =
                    (static_cast<float>(bounds->left + bounds->right) - static_cast<float>(bounds->textureWidth)) * 0.5f;
                const auto offsetY =
                    (static_cast<float>(bounds->top + bounds->bottom) - static_cast<float>(bounds->textureHeight)) * 0.5f;

                constexpr std::uint32_t kMaxPositionAdjustments = 2;
                constexpr float kInitialCenterTolerancePixels = 0.5f;
                constexpr float kResidualCenterTolerancePixels = 2.0f;
                const auto centerTolerancePixels =
                    positionAdjustments_ == 0 ? kInitialCenterTolerancePixels : kResidualCenterTolerancePixels;
                const bool shouldMove =
                    positionAdjustments_ < kMaxPositionAdjustments &&
                    (std::abs(offsetX) > centerTolerancePixels ||
                     std::abs(offsetY) > centerTolerancePixels);
                if (shouldMove) {
                    const auto width = static_cast<float>(bounds->textureWidth);
                    const auto height = static_cast<float>(bounds->textureHeight);
                    const auto aspect = width / height;
                    const auto verticalCorrection = (16.0f / 9.0f) / aspect;
                    const auto pixelsPerWorldX = width / (2.0f * 130.0f);
                    const auto pixelsPerWorldY = height / (2.0f * 73.0f * verticalCorrection);
                    auto position = mesh_.GetPosition();
                    position.x += offsetX / pixelsPerWorldX;
                    position.z += offsetY / pixelsPerWorldY;
                    mesh_.SetPosition(position);
                    ++positionAdjustments_;
                    return;
                }

                calibrated_ = true;
                return;
            }
        }

        void CalibrateAnimated(ID3D11ShaderResourceView* texture, std::uint64_t frame)
        {
            if (frame < nextCalibrationFrame_) {
                return;
            }
            nextCalibrationFrame_ = frame + kValidationIntervalFrames;

            const auto bounds = MeasureAlphaBounds(
                texture,
                kVisibleAlphaThreshold,
                kAnimatedAlphaTrimFraction);
            if (!bounds) {
                if (frame - createdFrame_ >= kPreviewTimeoutFrames) {
                    calibrated_ = true;
                    UpdateAnimationState();
                }
                return;
            }

            const auto width = static_cast<float>(bounds->textureWidth);
            const auto height = static_cast<float>(bounds->textureHeight);
            animatedBoundsSamples_[animatedBoundsSampleCount_++] = *bounds;
            if (animatedBoundsSampleCount_ < kAnimatedCalibrationSamples) {
                return;
            }

            auto stableLeft = bounds->textureWidth;
            auto stableTop = bounds->textureHeight;
            std::uint32_t stableRight = 0;
            std::uint32_t stableBottom = 0;
            for (const auto& sample : animatedBoundsSamples_) {
                stableLeft = std::min(stableLeft, sample.left);
                stableTop = std::min(stableTop, sample.top);
                stableRight = std::max(stableRight, sample.right);
                stableBottom = std::max(stableBottom, sample.bottom);
            }
            const auto stableWidth = static_cast<float>(stableRight - stableLeft);
            const auto stableHeight = static_cast<float>(stableBottom - stableTop);

            if ((stableWidth > width * 0.9f || stableHeight > height * 0.9f) && scaleAdjustments_ < 2) {
                const auto fit = std::min(width * 0.8f / stableWidth, height * 0.8f / stableHeight);
                mesh_.ScaleUp(std::clamp(fit, 0.5f, 0.9f));
                ++scaleAdjustments_;
                ResetAnimatedBoundsSamples();
                return;
            }

            const auto contentCenterX = static_cast<float>(stableLeft + stableRight) * 0.5f;
            const auto contentCenterY = static_cast<float>(stableTop + stableBottom) * 0.5f;
            const auto offsetX = contentCenterX - width * 0.5f;
            const auto offsetY = contentCenterY - height * 0.5f;
            constexpr float kCenterTolerancePixels = 20.0f;
            if (positionAdjustments_ < 2 &&
                (std::abs(offsetX) > kCenterTolerancePixels || std::abs(offsetY) > kCenterTolerancePixels)) {
                const auto aspect = width / height;
                const auto verticalCorrection = (16.0f / 9.0f) / aspect;
                const auto pixelsPerWorldX = width / (2.0f * 130.0f);
                const auto pixelsPerWorldY = height / (2.0f * 73.0f * verticalCorrection);
                auto position = mesh_.GetPosition();
                position.x += offsetX / pixelsPerWorldX;
                position.z += offsetY / pixelsPerWorldY;
                mesh_.SetPosition(position);
                ++positionAdjustments_;
                ResetAnimatedBoundsSamples();
                return;
            }

            const auto halfWidth = stableWidth * 0.5f;
            const auto halfHeight = stableHeight * 0.5f;
            auto cropHalfExtent = std::max(32.0f, std::max(halfWidth, halfHeight) * 1.35f);
            cropHalfExtent = std::min(cropHalfExtent, std::min(width, height) * 0.5f);

            const auto cropCenterX = std::clamp(contentCenterX, cropHalfExtent, width - cropHalfExtent);
            const auto cropCenterY = std::clamp(contentCenterY, cropHalfExtent, height - cropHalfExtent);
            placement_.u0 = (cropCenterX - cropHalfExtent) / width;
            placement_.v0 = (cropCenterY - cropHalfExtent) / height;
            placement_.u1 = (cropCenterX + cropHalfExtent) / width;
            placement_.v1 = (cropCenterY + cropHalfExtent) / height;
            calibrated_ = true;
            UpdateAnimationState();
        }

        void ResetAnimatedBoundsSamples()
        {
            animatedBoundsSampleCount_ = 0;
        }

        void UpdateAnimationState()
        {
            if (animated_) {
                mesh_.SetAlwaysUpdate(visible_ || !calibrated_);
            }
        }

        std::uint64_t createdFrame_{ 0 };
        bool animated_{ false };
        bool calibrated_{ false };
        bool visible_{ false };
        std::uint64_t nextCalibrationFrame_{ 0 };
        std::uint32_t positionAdjustments_{ 0 };
        std::uint32_t scaleAdjustments_{ 0 };
        std::size_t animatedBoundsSampleCount_{ 0 };
        std::array<AlphaBounds, kAnimatedCalibrationSamples> animatedBoundsSamples_{};
        PreviewPlacement placement_{};
        std::optional<AlphaBounds> staticBounds_;
        Microsoft::WRL::ComPtr<ID3D11Texture2D> staticTexture_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> staticTextureView_;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> stableTexture_;
        MeshRenderingFrameworkAPI::OrbitMesh mesh_;
        bool initialAnimatedRefreshRequested_{ false };
    };

    class PreviewSlot
    {
    public:
        void SetVisible(bool visible)
        {
            visible_ = visible;
            if (entry_) {
                entry_->SetVisible(visible);
            }
        }

        [[nodiscard]] bool Update(const FavoriteItem& item, std::uint64_t frame)
        {
            if (object_ != item.boundObject) {
                object_ = item.boundObject;
                entry_.reset();
                terminalFailure_ = false;
            }

            if (terminalFailure_) {
                return true;
            }

            if (!entry_) {
                const bool animated = IsAnimatedPreview(item.form);
                const auto resolution = animated ? kAnimatedPreviewResolution : Config::Get().previewResolution;
                entry_ = std::make_unique<PreviewEntry>(object_, resolution, animated, frame);
                entry_->SetVisible(visible_);
            }

            switch (entry_->Advance(frame)) {
            case PreviewEntryState::kReady:
                return true;
            case PreviewEntryState::kFailed:
                entry_.reset();
                terminalFailure_ = true;
                logger::error("Mesh preview for {:08X} ({}) failed", item.key.formID, item.name);
                return true;
            case PreviewEntryState::kPending:
            default:
                return false;
            }
        }

        [[nodiscard]] PreviewResult Result() const
        {
            return entry_ ? entry_->Result() : PreviewResult{};
        }

    private:
        RE::TESBoundObject* object_{ nullptr };
        std::unique_ptr<PreviewEntry> entry_;
        bool visible_{ false };
        bool terminalFailure_{ false };
    };

    PreviewCache& PreviewCache::GetSingleton()
    {
        static PreviewCache singleton;
        return singleton;
    }

    PreviewCache::~PreviewCache() = default;

    PreviewResult PreviewCache::Get(const FavoriteItem& item) const
    {
        const auto preview = previews_.find(item.key.formID);
        return preview != previews_.end() ? preview->second->Result() : PreviewResult{};
    }

    bool PreviewCache::Update(const std::vector<FavoriteItem>& items)
    {
        return UpdateItems(items, std::numeric_limits<std::size_t>::max());
    }

    bool PreviewCache::Prewarm(const std::vector<FavoriteItem>& items)
    {
        return UpdateItems(items, kPrewarmNewEntriesPerFrame);
    }

    bool PreviewCache::UpdateItems(
        const std::vector<FavoriteItem>& items,
        std::size_t maxNewEntries)
    {
        if (!IsFrameworkAvailable()) {
            readyToPresent_.store(true, std::memory_order_release);
            return true;
        }

        bool ready = true;
        std::size_t newEntries = 0;
        std::unordered_set<RE::FormID> updated;
        updated.reserve(items.size());
        for (const auto& item : items) {
            if (!item.key.IsValid() || !updated.insert(item.key.formID).second) {
                continue;
            }

            if (!item.boundObject || !MeshRenderingFrameworkAPI::HasRenderableModel(item.boundObject)) {
                continue;
            }
            auto preview = previews_.find(item.key.formID);
            if (preview == previews_.end()) {
                if (newEntries >= maxNewEntries) {
                    ready = false;
                    continue;
                }
                preview = previews_.emplace(item.key.formID, std::make_unique<PreviewSlot>()).first;
                preview->second->SetVisible(visible_);
                ++newEntries;
            }
            if (!preview->second->Update(item, frame_)) {
                ready = false;
            }
        }
        readyToPresent_.store(ready, std::memory_order_release);
        return ready;
    }

    bool PreviewCache::IsFrameworkAvailable() const noexcept
    {
        return GetModuleHandleW(L"MeshRenderingFramework") != nullptr;
    }

    bool PreviewCache::IsReadyToPresent() const noexcept
    {
        return readyToPresent_.load(std::memory_order_acquire);
    }

    void PreviewCache::BeginFrame() noexcept
    {
        ++frame_;
    }

    void PreviewCache::SetVisible(bool visible) noexcept
    {
        visible_ = visible;
        for (const auto& [_, preview] : previews_) {
            preview->SetVisible(visible);
        }
    }

    void PreviewCache::Reconcile(const std::vector<FavoriteItem>& items)
    {
        std::unordered_set<RE::FormID> activeForms;
        activeForms.reserve(items.size());
        for (const auto& item : items) {
            activeForms.insert(item.key.formID);
        }

        const bool activeSetChanged = activeForms != activeForms_;
        if (!activeSetChanged) {
            return;
        }

        std::erase_if(previews_, [&](const auto& preview) {
            return !activeForms.contains(preview.first);
        });
        activeForms_ = std::move(activeForms);
        readyToPresent_.store(false, std::memory_order_release);
    }

    void PreviewCache::Clear()
    {
        previews_.clear();
        activeForms_.clear();
        frame_ = 0;
        readyToPresent_.store(false, std::memory_order_release);
    }
}
