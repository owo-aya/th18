#pragma once
#include <string>

enum class ReplayMenuMode { Browse, InGameSave, ClearSave };
enum class ReplayMenuAction { None, Back, Start };
struct ReplayMenuResult {
    ReplayMenuAction action;
    std::string filename;
    int stage;
};
struct ReplayMenu;

ReplayMenu* replay_menu_create(ReplayMenuMode mode);
void replay_menu_destroy(ReplayMenu* menu);
ReplayMenuResult replay_menu_update(ReplayMenu* menu);
void replay_menu_render(ReplayMenu* menu);
