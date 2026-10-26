#pragma once

struct AnmVm;
struct IngameMenu;

enum class IngameMenuReason { Pause, GameOver, PracticeClear, ReplayEnd };
enum class IngameMenuAction { None, Resume, Continue, RestartRun, ReturnTitle };

// One controller replaces the old cf40c shell. Runtime owns A0x0c/B0x51 tasks.
IngameMenu* ingame_menu_create();
void ingame_menu_destroy(IngameMenu* menu);

// Runtime owns the original Escape/pause permission and >29-frame gate, and
// calls open at A0x0c. Player death/result/replay end also call open explicitly.
// Set the session's pause flag immediately, before A0x10 can advance battle.
// ranking_index is score_insert_ranking's 0..9 result, or -1 for no name entry.
void ingame_menu_open(IngameMenu* menu, IngameMenuReason reason,
                      int credits, int ranking_index);
bool ingame_menu_active(const IngameMenu* menu);

// A0x0c: consumes physical input_state/input_frame only. Runtime applies the
// returned action to the existing game session; RestartRun uses start_stage.
// The controller also owns the nested pause pages' A7/B50 callbacks.
IngameMenuAction ingame_menu_update(IngameMenu* menu);
// B0x51 queues group-0 text for B0x58. ANM roots have owner slots and advance at
// A0x0b; menu signals from A0x0c take effect on its next pass.
// Drawing uses native passes;
// the menu owner must not call anm_draw_vm or an extra global drawing pass.
void ingame_menu_render(IngameMenu* menu);

// Original 00458480/00487580 copies the paused playfield into this text.anm VM.
// The renderer owner performs that in-memory texture copy after open.
AnmVm* ingame_menu_background(const IngameMenu* menu);
