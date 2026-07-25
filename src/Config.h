#pragma once

namespace TFM::Config
{
    inline constexpr float kBorderScaleMin{ 0.50f };
    inline constexpr float kBorderScaleMax{ 1.50f };
    inline constexpr float kFrameScaleMin{ kBorderScaleMin };
    inline constexpr float kFrameScaleMax{ kBorderScaleMax };
    inline constexpr std::uint32_t kTextSizeMin{ 16 };
    inline constexpr std::uint32_t kTextSizeMax{ 32 };
    inline constexpr std::uint32_t kTextSizeDefault{ 24 };
    inline constexpr float kOuterMarginMin{ 0.0f };
    inline constexpr float kOuterMarginMax{ 400.0f };
    inline constexpr float kOuterMarginDefault{ 24.0f };
    inline constexpr std::uint32_t kPreviewResolutionMin{ 128 };
    inline constexpr std::uint32_t kPreviewResolutionMax{ 1024 };
    inline constexpr std::uint32_t kPreviewResolutionDefault{ 320 };
    inline constexpr float kGapMin{ 0.0f };
    inline constexpr float kGapMax{ 40.0f };
    inline constexpr float kGapDefault{ 8.0f };
    inline constexpr float kOpacityMin{ 0.0f };
    inline constexpr float kOpacityMax{ 1.0f };
    inline constexpr float kBackdropAlphaDefault{ 0.18f };
    inline constexpr float kTileAlphaDefault{ 0.62f };

    enum class PreviewMode : std::uint8_t
    {
        kMeshes,
        kIcons
    };

    enum class TileStyle : std::uint8_t
    {
        kFramed,
        kMinimal
    };

    struct Settings
    {
        bool closeOnSelection{ false };
        PreviewMode previewMode{ PreviewMode::kIcons };
        TileStyle tileStyle{ TileStyle::kFramed };
        std::uint32_t previewResolution{ kPreviewResolutionDefault };
        float outerMargin{ kOuterMarginDefault };
        float gap{ kGapDefault };
        float backdropAlpha{ kBackdropAlphaDefault };
        float tileAlpha{ kTileAlphaDefault };
        float borderScale{ 0.8f };
        float frameScale{ 1.0f };
        std::uint32_t textSize{ kTextSizeDefault };
    };

    [[nodiscard]] const Settings& Get();
    void Load();
    void SetCloseOnSelection(bool enabled);
    void SetPreviewMode(PreviewMode mode);
    void UsePreviewModeForSession(PreviewMode mode);
    void SetTileStyle(TileStyle style);
    void SetBorderScale(float scale);
    void SetFrameScale(float scale);
    void SetTextSize(std::uint32_t size);
    void SetOuterMargin(float margin);
    void SetPreviewResolution(std::uint32_t resolution);
    void SetGap(float gap);
    void SetBackdropAlpha(float alpha);
    void SetTileAlpha(float alpha);
    void ResetToDefaults();
}
