#pragma once
#include <cstdint>
#include "presentation/anm.h"

// 字体/文字系统（HUD、对话、菜单共用）。
// 原版用 ascii.anm 的精灵作为字模：按分辨率选 ascii.anm / ascii_960.anm /
// ascii1280.anm（sub_419390 里比较屏幕宽 DAT_0056aca0 与 DAT_004b9198 / DAT_004b91e0）。
// 文字先入队（sub_419b90），每帧由任务 sub_4198b0 递减寿命并压缩队列，
// 再由 sub_41a2b0 统一绘制。
//
// 关键地址：
//   sub_419390  text_create           分配并初始化 TextManager（0x19278 字节）
//   sub_419690  text_destroy          释放
//   sub_4198b0  text_update_task      任务回调：递减每项计时、紧凑队列、+0x1925c 帧计数
//   sub_419b90  text_queue_add        文字入队
//   sub_419cb0  text_draw_fmt         printf 风格入队（处理两行/副色样式 6、7、10）
//   sub_419dd0  text_draw_number      带千分位逗号的数字（score 等）
//   sub_419f30  text_draw_number2     另一种数字格式（%.2d 分组，卡牌/残机等）
//   sub_41a2b0  text_render           绘制队列里的全部文字（3702 字节）
//   sub_41b1c0  text_set_style        按 id/名次选颜色并微调位置（灰/黑/黄）
//   sub_41b2d0  text_list_contains    在列表里查 id
//   sub_41b310  text_format           sprintf 包装（数字绘制与 ANM 文件名都用它）

struct TextManager;

// 0x487140/0x4873f0：写入VM精灵对应的动态文字纹理。颜色是原COLORREF
// （低字节R），font为原0..11；x和spacing使用原640基准，内部乘2。
// flags3的0x4000关闭描边；centered按原SJIS字节数居中。
void text_write_texture(AnmVm* vm, uint32_t color, uint32_t outline_color,
                        int font, int x, int spacing, const char* sjis);
void text_write_texture_centered(AnmVm* vm, uint32_t color, uint32_t outline_color,
                                 int font, int spacing, const char* sjis);

// 文字绘制位置（sub_419b90 的参数结构）：屏幕坐标，入队时乘屏幕缩放 DAT_0056aca0。
struct TextPos {
    float x;
    float y;
    float z;
};

// 文字队列项：320 项 × 0x138 字节，紧跟在两个 AnmVm（+0x0c、+0x618）之后。
// 队列起点是 TextManager+0xc24；项内布局：
//   +0x000 char text[0x100]        文字内容（SJIS/ASCII 字节）
//   +0x100 float x, y, z           入队时按 DAT_0056aca0 缩放后的位置
//   +0x10c uint32 color            文字色 ARGB（入队时取 TextManager+0x19228）
//   +0x110..+0x11c                 来自 TextManager+0x19230/0x19234/0x19238
//   +0x120 uint32 style            绘制样式（入队时取 TextManager+0x19244）
//   +0x124..+0x128                 来自 TextManager+0x19240/0x19248
//   +0x12c int32  life             寿命/计时，sub_4198b0 每帧减一，<0 丢弃
//   +0x130/+0x134                  来自 TextManager+0x19250/0x19254
struct TextItem {
    char text[0x100];
    float x, y, z;
    uint32_t color;
    float dirty_110;
    float dirty_114;
    float dirty_118;
    float dirty_11c;
    uint32_t style;
    uint32_t dirty_124;
    uint32_t dirty_128;
    int32_t life;
    int32_t dirty_130;
    int32_t dirty_134;
};
static_assert(sizeof(TextItem) == 0x138, "TextItem 必须是 0x138 字节（原版队列步长）");

// TextManager（0x19278 字节，全局 DAT_004cf2ac）：
//   +0x000 uint32 flags            *flags |= 2
//   +0x004 void*  task_update      优先级 5 的任务（FUN_00401530）
//   +0x008 void*  task_other       优先级 0x58
//   +0x00c AnmVm  vm_ascii         字模 VM（sprite 0）
//   +0x18c AnmVm  vm_ascii_alt     第二 VM（sprite 0x62）
//   +0xc24 TextItem queue[320]
//   +0x19224 int32 count           队列项数（上限 0x140）
//   +0x19228 uint32 color          ARGB 当前文字色
//   +0x1922c uint32 color2         副色（样式 6/7/10 时显示第二行）
//   +0x19230 .. +0x19238 float     文字尺寸/缩放参数
//   +0x19244 uint32 style          绘制样式：0 普通；6/7/10 两行模式（转 8/9/11）；2 等
//   +0x19248 uint32
//   +0x1924c int32  life           新入队项的默认寿命
//   +0x19250 / +0x19254 float
//   +0x1925c int32  frame          每帧 +1
//   +0x19260 AnmResource* ascii    字模 ANM（anm_get(2, 名字)）
//   +0x19264 uint32
//   +0x19268 AnmVm* vm             供 anm_set_sprite 改字模用
//   +0x1926c / +0x19270 / +0x19274 三个任务句柄（优先级 0x37/0x48/0x3f）
constexpr uint32_t TEXT_MANAGER_SIZE = 0x19278;
constexpr uint32_t TEXT_QUEUE_OFFSET = 0xc24;
constexpr uint32_t TEXT_QUEUE_CAPACITY = 0x140;

// sub_419390: 创建文字系统并设全局 DAT_004cf2ac；失败返回 null（原版走异常清理）。
TextManager* text_create();

// 全局文字管理器（原版 DAT_004cf2ac）：text_create 成功后设置，text_destroy 清空。
// HUD/关卡会话通过它访问同一份队列与字模。
TextManager* text_manager();

// sub_419690: 释放文字系统（释放任务、VM、ANM 句柄、对象本身）。
void text_destroy(TextManager* text);

// sub_4198b0: 任务回调。每项 life 减一，life<0 的项被后面项覆盖（保持顺序）。
void text_update_task(TextManager* text);

// sub_42cb90 / sub_42cb60：右下 loading 根 VM（ascii.anm script 17）。
// 坐标采用原版 640x480 基准；每帧由 text_update_task 推进，停止时中断到 label 1。
void text_loading_start(float x, float y);
void text_loading_stop();

// sub_419b90: 把 pos 与字符串入队（原版会把 pos 按屏幕缩放 DAT_0056aca0 放大）。
void text_queue_add(TextManager* text, const TextPos* pos, const char* str);

// sub_419cb0: printf 风格入队。样式 6/7/10 时会先用副色再排一行主色文字。
void text_draw_fmt(TextManager* text, const TextPos* pos, const char* fmt, ...);

// sub_419dd0: 画数字（每 3 位加逗号）；用当前文字色。
void text_draw_number(TextManager* text, const TextPos* pos, uint32_t value);

// sub_419f30: 画数字（另一种分组方式，%d/%.2d 系列）；param3 参与低位计算。
void text_draw_number2(TextManager* text, const TextPos* pos, uint32_t value, int param3);

// sub_41a2b0: 绘制队列中的全部文字（按 style 选择字模 VM/颜色/两行处理）。
void text_render(TextManager* text, uint32_t group);
void text_set_group(uint32_t group);

// sub_41b1c0: 按 id 在表里选颜色（灰底灰字/黑/黄），并可选按 param5 微调 pos。
//   param3 结构：+0x00 int id，+0x24+int 列表，+0xd4 int 列表长度。
void text_set_style(TextPos* pos, int id, const void* table, uint32_t flags, int param5);

// sub_41b2d0: 表内查 id，返回是否命中。
bool text_list_contains(const void* table, int id);

// sub_41b310: sprintf 包装。原版调用约定把第一个格式化参数放在 this 槽位，
// 因此 C++ 侧直接写成可变参数版本，调用处按 fmt 顺序传参。
void text_format(char* out, const char* fmt, ...);

// 入队前设置本帧的文字属性（对应原版直接写 TextManager 的字段）：
//   text_set_color  -> +0x19228 ARGB 颜色
//   text_set_life   -> +0x1924c 寿命（帧数；入队后 text_update_task 每帧减一）
//   text_set_size   -> +0x19230/+0x19234 横/竖向缩放（字模原始像素 1.0）
//   text_set_style  -> +0x19244 绘制样式（0 普通；6/8、7/9 为大字模）
void text_set_color(uint32_t argb);
void text_set_secondary_color(uint32_t argb);
void text_set_alignment(int horizontal, int vertical); // 0中、1左/上、2右/下
void text_set_life(int frames);
void text_set_size(float scale_x, float scale_y);
void text_set_style(uint32_t style);

// 字模 ANM 里文字用的精灵编号（对话/场景文字用 anm 资源下标 6 的 stNNlogo.anm，
// 而 HUD 字模来自本文件的 ascii.anm）。
// 对话/场景文字与气泡由 sub_43a8b0 创建 7 个立绘 VM + 7 个名字 VM + 7 个文字 VM，
// 全部跑 text.anm 风格的逐字脚本。
