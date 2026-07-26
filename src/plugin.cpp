#include "Config.h"
#include "Serialization.h"
#include "SettingsMenu.h"
#include "UI.h"
#include "logger.h"

namespace
{
    void MessageHandler(SKSE::MessagingInterface::Message* message)
    {
        if (!message) {
            return;
        }
        if (message->type == SKSE::MessagingInterface::kDataLoaded) {
            TFM::Config::Load();
            TFM::UI::Register();
            TFM::SettingsMenu::Register();
        }
    }
}

SKSEPluginLoad(const SKSE::LoadInterface* skse)
{
    SetupLog();
    SKSE::Init(skse);

    if (!TFM::Serialization::Register()) {
        return false;
    }
    const auto messaging = SKSE::GetMessagingInterface();
    if (!messaging || !messaging->RegisterListener(MessageHandler)) {
        logger::critical("Could not register the SKSE messaging listener");
        return false;
    }

    logger::info("Tileable Favorites Menu 1.0.2 loaded");
    return true;
}
