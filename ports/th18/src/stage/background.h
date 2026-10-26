#pragma once
struct AnmResource;
struct AnmVm;

// Original STD object/instance/camera runtime (0041b850, 0041d260, 0041cee0).
void stage_background_create(const char* std_name);
void stage_background_destroy();
void stage_background_update();
void stage_background_render();
void stage_background_render_upper();
// B0x05/layer0 and B0x09/layer2 keep the STD view/projection.
void stage_background_render_anm(int layer);
// 477ea0 / 478580 read the camera published by the STD A0x12 update.
float stage_background_anm_value(int variable);
void stage_background_apply_camera_delta(AnmVm* vm);
// The two live STD depth surfaces must leave DEFAULT pool before display Reset.
void stage_background_release_depth();
void stage_background_create_depth();
// 0041e2d0: resume at an STD opcode16 marker, using that marker's script time.
void stage_background_interrupt(int label);
// Original background+3474 bit0, changed by Spell start/end.
void stage_background_set_visible(bool visible);
void stage_background_retain_current();
bool stage_background_has_previous();
void stage_background_begin_transition();
AnmResource* stage_background_resource();
