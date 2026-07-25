#pragma once

namespace TFM::UI
{
    bool Register();
    void Open();
    void Close();
    void ApplyPreviewSettings();
    [[nodiscard]] bool IsOpen();
}
