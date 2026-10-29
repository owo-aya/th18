#pragma once

struct AchievementMenu;
AchievementMenu* achievement_menu_create(bool returning_from_extra);
bool achievement_menu_update(AchievementMenu* menu);
void achievement_menu_render(const AchievementMenu* menu);
void achievement_menu_destroy(AchievementMenu* menu);
