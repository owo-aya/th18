#pragma once

// TitleInf's mode-2 card selector (00413810 / 00413af0).
struct CardSelect;
CardSelect* card_select_create(int bank);
void card_select_destroy(CardSelect* selector);
void card_select_update(CardSelect* selector);
void card_select_render(CardSelect* selector);
void card_select_focus(CardSelect* selector, bool focused);
void card_select_stage_select(CardSelect* selector);
void card_select_open(CardSelect* selector);
bool card_select_closed(const CardSelect* selector);
