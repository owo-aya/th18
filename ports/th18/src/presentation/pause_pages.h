#pragma once

struct PauseHelp;
struct PauseOptions;

// Update returns true when the original exit sequence has finished.
// IngameMenu owns the A7 update and B50 text callbacks and destroys each page.

// 00444f10, called by 00458e40 choice 4 with its pause-page offset of 32.
PauseHelp* pause_help_create();
void pause_help_destroy(PauseHelp* page);
bool pause_help_update(PauseHelp* page);

// 00456880, called by choice 5 at (60,100,0), with pause origin +0x130 = 1.
PauseOptions* pause_options_create();
void pause_options_destroy(PauseOptions* page);
bool pause_options_update(PauseOptions* page);
void pause_options_render(PauseOptions* page);
