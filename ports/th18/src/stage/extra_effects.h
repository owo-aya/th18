#pragma once
#include <cstdint>

struct AnmVm;
struct AnmResource;
namespace stage {struct Enemy;}

void extra_effects_start();
void extra_effects_stop();
void extra_maple_spawn(AnmResource* resource);
bool extra_maple_update(AnmVm* vm);
void extra_maple_draw(AnmVm* vm);
void extra_maple_free(AnmVm* vm);
void extra_charge_start(AnmVm* vm);
bool extra_charge_update(AnmVm* vm);
void extra_charge_label(AnmVm* vm, int label);
void extra_charge_free(AnmVm* vm);
void extra_aura_set(stage::Enemy& enemy,float radius,uint32_t color);
void extra_aura_update(stage::Enemy& enemy);
void extra_aura_free(stage::Enemy& enemy);
