#pragma once

#include "Types.h"

struct ID3D11ShaderResourceView;

namespace TFM
{
    struct PreviewPlacement
    {
        float u0{ 0.0f };
        float v0{ 0.0f };
        float u1{ 1.0f };
        float v1{ 1.0f };
    };

    struct PreviewResult
    {
        ID3D11ShaderResourceView* texture{ nullptr };
        PreviewPlacement placement{};

        [[nodiscard]] explicit operator bool() const noexcept { return texture != nullptr; }
    };

    class PreviewCache
    {
    public:
        static PreviewCache& GetSingleton();
        ~PreviewCache();

        [[nodiscard]] PreviewResult Get(const FavoriteItem& item) const;
        [[nodiscard]] bool Update(const std::vector<FavoriteItem>& items);
        [[nodiscard]] bool Prewarm(const std::vector<FavoriteItem>& items);
        [[nodiscard]] bool IsFrameworkAvailable() const noexcept;
        [[nodiscard]] bool IsReadyToPresent() const noexcept;
        void BeginFrame() noexcept;
        void SetVisible(bool visible) noexcept;
        void Reconcile(const std::vector<FavoriteItem>& items);
        void Clear();

    private:
        PreviewCache() = default;
        [[nodiscard]] bool UpdateItems(
            const std::vector<FavoriteItem>& items,
            std::size_t maxNewEntries);

        std::unordered_map<RE::FormID, std::unique_ptr<class PreviewSlot>> previews_;
        std::unordered_set<RE::FormID> activeForms_;
        std::uint64_t frame_{ 0 };
        std::atomic_bool readyToPresent_{ false };
        bool visible_{ false };
    };
}
