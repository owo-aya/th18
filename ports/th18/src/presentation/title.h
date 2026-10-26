#pragma once
#include <cstdint>

// FUN_004531b0 / 00452cb0：声明画面，持有 sig.anm 的脚本 0。
struct TitleScene;
TitleScene* title_scene_create();
void title_scene_destroy(TitleScene* scene);

// FUN_00464cb0 / 00464b80 / 00465aa0：声明画面显示180帧后进入标题菜单；
// 按菜单状态创建 title.anm 根VM，子动画由脚本自身创建。
struct TitleMenu;
TitleMenu* title_menu_create();
void title_menu_destroy(TitleMenu* menu);
// ASCII 19's label-1 fade continues after the title menu leaves for gameplay.
void title_finish_ambient_step();
