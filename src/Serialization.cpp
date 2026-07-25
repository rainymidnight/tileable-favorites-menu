#include "Serialization.h"

#include "Layout.h"
#include "PreviewCache.h"
#include "logger.h"

namespace TFM::Serialization
{
    namespace
    {
        constexpr std::uint32_t FourCC(char a, char b, char c, char d)
        {
            return static_cast<std::uint32_t>(a) |
                (static_cast<std::uint32_t>(b) << 8U) |
                (static_cast<std::uint32_t>(c) << 16U) |
                (static_cast<std::uint32_t>(d) << 24U);
        }

        constexpr auto kPluginID = FourCC('T', 'F', 'M', 'N');
        constexpr auto kLayoutRecord = FourCC('L', 'A', 'Y', 'T');
        constexpr std::uint32_t kVersion = 1;

        void Save(SKSE::SerializationInterface* serialization)
        {
            if (!serialization->OpenRecord(kLayoutRecord, kVersion)) {
                logger::error("Could not open layout serialization record");
                return;
            }
            if (!Layout::GetSingleton().Save(serialization)) {
                logger::error("Could not serialize tile layout");
            }
        }

        void Load(SKSE::SerializationInterface* serialization)
        {
            std::uint32_t type = 0;
            std::uint32_t version = 0;
            std::uint32_t length = 0;
            while (serialization->GetNextRecordInfo(type, version, length)) {
                static_cast<void>(length);
                if (type != kLayoutRecord) {
                    logger::warn("Ignoring unknown serialization record {:08X}", type);
                    continue;
                }
                if (version != kVersion) {
                    logger::warn("Ignoring unsupported layout record version {}", version);
                    continue;
                }
                if (!Layout::GetSingleton().Load(serialization)) {
                    logger::error("Saved tile layout is invalid; it will be rebuilt on next open");
                    Layout::GetSingleton().Clear();
                }
            }
        }

        void Revert(SKSE::SerializationInterface*)
        {
            Layout::GetSingleton().Clear();
            PreviewCache::GetSingleton().Clear();
        }
    }

    bool Register()
    {
        const auto serialization = SKSE::GetSerializationInterface();
        if (!serialization) {
            logger::critical("SKSE serialization interface is unavailable");
            return false;
        }
        serialization->SetUniqueID(kPluginID);
        serialization->SetSaveCallback(Save);
        serialization->SetLoadCallback(Load);
        serialization->SetRevertCallback(Revert);
        return true;
    }
}
