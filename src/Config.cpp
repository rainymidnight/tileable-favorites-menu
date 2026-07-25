#include "Config.h"
#include "logger.h"

namespace TFM::Config
{
    namespace
    {
        Settings settings;
        constexpr auto kPath = L"Data\\SKSE\\Plugins\\TileableFavoritesMenu.ini";

        [[nodiscard]] bool ReadBool(const wchar_t* section, const wchar_t* key, bool fallback)
        {
            return GetPrivateProfileIntW(section, key, fallback ? 1 : 0, kPath) != 0;
        }

        [[nodiscard]] std::uint32_t ReadUInt(const wchar_t* section, const wchar_t* key, std::uint32_t fallback)
        {
            const auto value = GetPrivateProfileIntW(section, key, static_cast<int>(fallback), kPath);
            return value < 0 ? fallback : static_cast<std::uint32_t>(value);
        }

        [[nodiscard]] PreviewMode ReadPreviewMode(PreviewMode fallback)
        {
            std::array<wchar_t, 64> value{};
            const auto fallbackString = fallback == PreviewMode::kIcons ? L"Icons" : L"Meshes";
            GetPrivateProfileStringW(
                L"Rendering",
                L"PreviewMode",
                fallbackString,
                value.data(),
                static_cast<DWORD>(value.size()),
                kPath);

            std::wstring normalized(value.data());
            std::ranges::transform(normalized, normalized.begin(), [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
            if (normalized == L"icons") {
                return PreviewMode::kIcons;
            }
            if (normalized == L"meshes") {
                return PreviewMode::kMeshes;
            }
            logger::warn("Unknown PreviewMode value; using {}", fallback == PreviewMode::kIcons ? "Icons" : "Meshes");
            return fallback;
        }

        [[nodiscard]] TileStyle ReadTileStyle(TileStyle fallback)
        {
            std::array<wchar_t, 64> value{};
            const auto fallbackString = fallback == TileStyle::kMinimal ? L"Minimal" : L"Framed";
            GetPrivateProfileStringW(
                L"Appearance",
                L"TileStyle",
                fallbackString,
                value.data(),
                static_cast<DWORD>(value.size()),
                kPath);

            std::wstring normalized(value.data());
            std::ranges::transform(normalized, normalized.begin(), [](wchar_t character) {
                return static_cast<wchar_t>(std::towlower(character));
            });
            if (normalized == L"framed") {
                return TileStyle::kFramed;
            }
            if (normalized == L"minimal") {
                return TileStyle::kMinimal;
            }
            logger::warn("Unknown TileStyle value; using {}", fallback == TileStyle::kMinimal ? "Minimal" : "Framed");
            return fallback;
        }

        [[nodiscard]] float ReadFloat(const wchar_t* section, const wchar_t* key, float fallback)
        {
            std::array<wchar_t, 64> value{};
            const auto fallbackString = std::format(L"{}", fallback);
            GetPrivateProfileStringW(section, key, fallbackString.c_str(), value.data(), static_cast<DWORD>(value.size()), kPath);
            try {
                return std::stof(value.data());
            } catch (...) {
                return fallback;
            }
        }

        void FlushSettings()
        {
            WritePrivateProfileStringW(nullptr, nullptr, nullptr, kPath);
        }

        void PersistValue(
            const wchar_t* section,
            const wchar_t* key,
            const wchar_t* value,
            std::string_view settingName)
        {
            if (!WritePrivateProfileStringW(section, key, value, kPath)) {
                logger::warn("Could not persist {} to Data\\SKSE\\Plugins\\TileableFavoritesMenu.ini", settingName);
                return;
            }
            FlushSettings();
        }

        void PersistFloat(const wchar_t* section, const wchar_t* key, float value, std::string_view settingName)
        {
            const auto serialized = std::format(L"{:.3f}", value);
            PersistValue(section, key, serialized.c_str(), settingName);
        }
    }

    const Settings& Get()
    {
        return settings;
    }

    void Load()
    {
        settings.closeOnSelection = ReadBool(L"General", L"CloseOnSelection", settings.closeOnSelection);
        settings.previewMode = ReadPreviewMode(settings.previewMode);
        settings.tileStyle = ReadTileStyle(settings.tileStyle);
        settings.previewResolution = std::clamp(
            ReadUInt(L"Rendering", L"PreviewResolution", settings.previewResolution),
            kPreviewResolutionMin,
            kPreviewResolutionMax);
        settings.outerMargin = std::clamp(
            ReadFloat(L"Layout", L"OuterMargin", settings.outerMargin),
            kOuterMarginMin,
            kOuterMarginMax);
        settings.gap = std::clamp(ReadFloat(L"Layout", L"Gap", settings.gap), kGapMin, kGapMax);
        settings.backdropAlpha = std::clamp(
            ReadFloat(L"Layout", L"BackdropAlpha", settings.backdropAlpha),
            kOpacityMin,
            kOpacityMax);
        settings.tileAlpha = std::clamp(
            ReadFloat(L"Layout", L"TileAlpha", settings.tileAlpha),
            kOpacityMin,
            kOpacityMax);
        settings.borderScale = std::clamp(
            ReadFloat(L"Appearance", L"BorderScale", settings.borderScale),
            kBorderScaleMin,
            kBorderScaleMax);
        settings.frameScale = std::clamp(
            ReadFloat(L"Appearance", L"FrameScale", settings.frameScale),
            kFrameScaleMin,
            kFrameScaleMax);
        settings.textSize = std::clamp(
            ReadUInt(L"Appearance", L"TextSize", settings.textSize),
            kTextSizeMin,
            kTextSizeMax);

        logger::info(
            "Settings loaded (closeOnSelection={}, previewMode={}, tileStyle={}, meshPreview={}px, borderScale={:.2f}, frameScale={:.2f}, textSize={}px)",
            settings.closeOnSelection,
            settings.previewMode == PreviewMode::kIcons ? "Icons" : "Meshes",
            settings.tileStyle == TileStyle::kMinimal ? "Minimal" : "Framed",
            settings.previewResolution,
            settings.borderScale,
            settings.frameScale,
            settings.textSize);
    }

    void SetCloseOnSelection(bool enabled)
    {
        if (settings.closeOnSelection == enabled) {
            return;
        }

        settings.closeOnSelection = enabled;
        PersistValue(L"General", L"CloseOnSelection", enabled ? L"1" : L"0", "CloseOnSelection");
    }

    void SetPreviewMode(PreviewMode mode)
    {
        if (settings.previewMode == mode) {
            return;
        }

        settings.previewMode = mode;
        const auto value = mode == PreviewMode::kIcons ? L"Icons" : L"Meshes";
        PersistValue(L"Rendering", L"PreviewMode", value, "PreviewMode");
    }

    void UsePreviewModeForSession(PreviewMode mode)
    {
        settings.previewMode = mode;
    }

    void SetTileStyle(TileStyle style)
    {
        if (settings.tileStyle == style) {
            return;
        }

        settings.tileStyle = style;
        const auto value = style == TileStyle::kMinimal ? L"Minimal" : L"Framed";
        PersistValue(L"Appearance", L"TileStyle", value, "TileStyle");
    }

    void SetBorderScale(float scale)
    {
        const auto clamped = std::clamp(scale, kBorderScaleMin, kBorderScaleMax);
        if (settings.borderScale == clamped) {
            return;
        }

        settings.borderScale = clamped;
        PersistFloat(L"Appearance", L"BorderScale", clamped, "BorderScale");
    }

    void SetFrameScale(float scale)
    {
        const auto clamped = std::clamp(scale, kFrameScaleMin, kFrameScaleMax);
        if (settings.frameScale == clamped) {
            return;
        }

        settings.frameScale = clamped;
        PersistFloat(L"Appearance", L"FrameScale", clamped, "FrameScale");
    }

    void SetTextSize(std::uint32_t size)
    {
        const auto clamped = std::clamp(size, kTextSizeMin, kTextSizeMax);
        if (settings.textSize == clamped) {
            return;
        }

        settings.textSize = clamped;
        const auto serialized = std::format(L"{}", clamped);
        PersistValue(L"Appearance", L"TextSize", serialized.c_str(), "TextSize");
    }

    void SetOuterMargin(float margin)
    {
        const auto clamped = std::clamp(margin, kOuterMarginMin, kOuterMarginMax);
        if (settings.outerMargin == clamped) {
            return;
        }

        settings.outerMargin = clamped;
        PersistFloat(L"Layout", L"OuterMargin", clamped, "OuterMargin");
    }

    void SetPreviewResolution(std::uint32_t resolution)
    {
        const auto clamped = std::clamp(resolution, kPreviewResolutionMin, kPreviewResolutionMax);
        if (settings.previewResolution == clamped) {
            return;
        }

        settings.previewResolution = clamped;
        const auto serialized = std::format(L"{}", clamped);
        PersistValue(L"Rendering", L"PreviewResolution", serialized.c_str(), "PreviewResolution");
    }

    void SetGap(float gap)
    {
        const auto clamped = std::clamp(gap, kGapMin, kGapMax);
        if (settings.gap == clamped) {
            return;
        }

        settings.gap = clamped;
        PersistFloat(L"Layout", L"Gap", clamped, "Gap");
    }

    void SetBackdropAlpha(float alpha)
    {
        const auto clamped = std::clamp(alpha, kOpacityMin, kOpacityMax);
        if (settings.backdropAlpha == clamped) {
            return;
        }

        settings.backdropAlpha = clamped;
        PersistFloat(L"Layout", L"BackdropAlpha", clamped, "BackdropAlpha");
    }

    void SetTileAlpha(float alpha)
    {
        const auto clamped = std::clamp(alpha, kOpacityMin, kOpacityMax);
        if (settings.tileAlpha == clamped) {
            return;
        }

        settings.tileAlpha = clamped;
        PersistFloat(L"Layout", L"TileAlpha", clamped, "TileAlpha");
    }

    void ResetToDefaults()
    {
        settings = Settings{};

        PersistValue(L"General", L"CloseOnSelection", L"0", "CloseOnSelection");
        PersistValue(L"Rendering", L"PreviewMode", L"Icons", "PreviewMode");
        const auto previewResolution = std::format(L"{}", settings.previewResolution);
        PersistValue(
            L"Rendering",
            L"PreviewResolution",
            previewResolution.c_str(),
            "PreviewResolution");
        PersistValue(L"Appearance", L"TileStyle", L"Framed", "TileStyle");
        PersistFloat(L"Appearance", L"BorderScale", settings.borderScale, "BorderScale");
        PersistFloat(L"Appearance", L"FrameScale", settings.frameScale, "FrameScale");
        const auto textSize = std::format(L"{}", settings.textSize);
        PersistValue(L"Appearance", L"TextSize", textSize.c_str(), "TextSize");
        PersistFloat(L"Layout", L"OuterMargin", settings.outerMargin, "OuterMargin");
        PersistFloat(L"Layout", L"Gap", settings.gap, "Gap");
        PersistFloat(L"Layout", L"BackdropAlpha", settings.backdropAlpha, "BackdropAlpha");
        PersistFloat(L"Layout", L"TileAlpha", settings.tileAlpha, "TileAlpha");

        logger::info("Settings reset to defaults");
    }
}
