#pragma once

#include "Types.h"

namespace TFM::Actions
{
    enum class Hand
    {
        kRight,
        kLeft
    };

    bool Activate(const FavoriteItem& item, Hand hand);
}
