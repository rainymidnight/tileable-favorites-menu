#include "SettingsMenu.h"

#include "Config.h"
#include "PreviewCache.h"
#include "SKSEMenuFramework.h"
#include "UI.h"

namespace TFM::SettingsMenu
{
    namespace
    {
        void __stdcall Render()
        {
            constexpr std::array previewModes{
                "Flat SkyUI icons",
                "3D meshes (Experimental)"
            };
            const auto meshesAvailable = PreviewCache::GetSingleton().IsFrameworkAvailable();
            auto selected = Config::Get().previewMode == Config::PreviewMode::kIcons ? 0 : 1;
            ImGuiMCP::BeginDisabled(!meshesAvailable);
            if (ImGuiMCP::Combo(
                    "Favorite icons",
                    std::addressof(selected),
                    previewModes.data(),
                    static_cast<int>(previewModes.size()))) {
                Config::SetPreviewMode(selected == 0 ? Config::PreviewMode::kIcons : Config::PreviewMode::kMeshes);
                UI::ApplyPreviewSettings();
            }
            ImGuiMCP::EndDisabled();

            if (meshesAvailable) {
                ImGuiMCP::BeginDisabled(Config::Get().previewMode != Config::PreviewMode::kMeshes);
                auto previewResolution = static_cast<int>(Config::Get().previewResolution);
                if (ImGuiMCP::SliderInt(
                        "Static mesh resolution",
                        std::addressof(previewResolution),
                        static_cast<int>(Config::kPreviewResolutionMin),
                        static_cast<int>(Config::kPreviewResolutionMax),
                        "%d px")) {
                    Config::SetPreviewResolution(static_cast<std::uint32_t>(previewResolution));
                    UI::ApplyPreviewSettings();
                }
                ImGuiMCP::EndDisabled();
            }

            constexpr std::array tileStyles{
                "Default",
                "Minimal"
            };
            auto tileStyle = Config::Get().tileStyle == Config::TileStyle::kFramed ? 0 : 1;
            if (ImGuiMCP::Combo(
                    "Tile style",
                    std::addressof(tileStyle),
                    tileStyles.data(),
                    static_cast<int>(tileStyles.size()))) {
                Config::SetTileStyle(tileStyle == 0 ? Config::TileStyle::kFramed : Config::TileStyle::kMinimal);
            }

            ImGuiMCP::Separator();
            auto borderPercent = Config::Get().borderScale * 100.0f;
            if (ImGuiMCP::SliderFloat(
                    "Tile border size",
                    std::addressof(borderPercent),
                    Config::kBorderScaleMin * 100.0f,
                    Config::kBorderScaleMax * 100.0f,
                    "%.0f%%")) {
                Config::SetBorderScale(borderPercent / 100.0f);
            }

            auto framePercent = Config::Get().frameScale * 100.0f;
            if (ImGuiMCP::SliderFloat(
                    "Tile size",
                    std::addressof(framePercent),
                    Config::kFrameScaleMin * 100.0f,
                    Config::kFrameScaleMax * 100.0f,
                    "%.0f%%")) {
                Config::SetFrameScale(framePercent / 100.0f);
            }

            auto textSize = static_cast<int>(Config::Get().textSize);
            if (ImGuiMCP::SliderInt(
                    "Text size",
                    std::addressof(textSize),
                    static_cast<int>(Config::kTextSizeMin),
                    static_cast<int>(Config::kTextSizeMax),
                    "%d px")) {
                Config::SetTextSize(static_cast<std::uint32_t>(textSize));
            }

            auto outerMargin = Config::Get().outerMargin;
            if (ImGuiMCP::SliderFloat(
                    "Menu margin",
                    std::addressof(outerMargin),
                    Config::kOuterMarginMin,
                    Config::kOuterMarginMax,
                    "%.0f px")) {
                Config::SetOuterMargin(outerMargin);
            }

            auto gap = Config::Get().gap;
            if (ImGuiMCP::SliderFloat(
                    "Tile gap",
                    std::addressof(gap),
                    Config::kGapMin,
                    Config::kGapMax,
                    "%.0f px")) {
                Config::SetGap(gap);
            }

            auto backdropPercent = Config::Get().backdropAlpha * 100.0f;
            if (ImGuiMCP::SliderFloat(
                    "Backdrop opacity",
                    std::addressof(backdropPercent),
                    Config::kOpacityMin * 100.0f,
                    Config::kOpacityMax * 100.0f,
                    "%.0f%%")) {
                Config::SetBackdropAlpha(backdropPercent / 100.0f);
            }

            auto tilePercent = Config::Get().tileAlpha * 100.0f;
            if (ImGuiMCP::SliderFloat(
                    "Tile opacity",
                    std::addressof(tilePercent),
                    Config::kOpacityMin * 100.0f,
                    Config::kOpacityMax * 100.0f,
                    "%.0f%%")) {
                Config::SetTileAlpha(tilePercent / 100.0f);
            }

            auto closeOnSelection = Config::Get().closeOnSelection;
            if (ImGuiMCP::Checkbox("Close on selection", std::addressof(closeOnSelection))) {
                Config::SetCloseOnSelection(closeOnSelection);
            }

            ImGuiMCP::Separator();
            if (ImGuiMCP::Button("Reset to defaults")) {
                Config::ResetToDefaults();
                UI::ApplyPreviewSettings();
            }
        }
    }

    void Register()
    {
        if (!SKSEMenuFramework::IsInstalled()) {
            return;
        }
        SKSEMenuFramework::SetSection("Tileable Favorites Menu");
        SKSEMenuFramework::AddSectionItem("Settings", Render);
    }
}
