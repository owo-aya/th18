#pragma once

#include "cards/card_id.h"

// 00418de0: caller has already marked the card acquired in the profile.
// kind 0 is an in-game acquisition; nonzero entries wait for the main menu.
void trophy_notify(CardId id, int kind = 0);
// 0046e490: the score/profile owner calls this only on a first achievement.
void trophy_achievement(int id);
void trophy_allow_deferred();
void trophy_shutdown();
