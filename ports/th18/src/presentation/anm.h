#pragma once
#include <cstddef>
#include <cstdint>

#include "../resources/anm_file.h"

// ANM 运行时（原版 th18.exe 里通称 AnmManager / AnmVm）。
// 本文件只放结构、枚举与已定位的原版函数地址；实现见 anm.cpp（P1）。
//
// 关键地址（均为 th18.exe 内偏移）：
//   sub_486880  anm_get(idx)            取/加载 ANM 资源，等待贴图上传完成
//   sub_4867e0  anm_load_resource       分配 0x13c 资源对象并调用加载
//   sub_4865b0  anm_load_file           读文件、解析段、外部贴图、分配 VM 数组
//   sub_486920  anm_upload_step         每次推进一段贴图上传（另一线程调用）
//   sub_486bc0  anm_upload_segment      单段贴图上传（D3DXCreateTexture + 像素拷贝）
//   sub_487580  anm_copy_surface        把外部 surface 区域拷进 ANM 贴图
//   sub_488260  anm_draw_layer(this, n) 按绘制层 n 遍历队列并逐个绘制
//   sub_481210  anm_draw_vm             按 VM 类型分派到具体绘制函数
//   sub_488350  anm_queue_add(&handle, vm) 把 VM 插入绘制队列，返回句柄
//   sub_488cf0  anm_queue_remove(handle)   从队列移除
//   sub_478580  anm_execute_script     ANM 脚本解释器（13KB，见文件末尾说明）
//   sub_4885d0  anm_free_vm            释放 VM、归还对象池
//   sub_41b430  anm_vm_copy            VM 结构拷贝（0x60c 字节）
//   sub_488be0  anm_set_label(vm, id)    设置要等待的 label（解释器命中 op5 后清零）
//   sub_488c60  anm_set_label_float(vm, f) 同上，float 版本
//   sub_47e730  anm_flush_batch        提交当前顶点批次
//   sub_47d910  anm_setup_draw_state   设置纹理/FVF/采样状态

// 本层的指令流表。原版资源 +0x120 存脚本流，+0x11c 的 0x44 字节项是
// 独立的 sprite 表；两种编号不能混用。
struct AnmScriptEntry {
    int32_t id;               // script 编号（op 300 用它在表里找脚本）
    const uint8_t* stream;    // 指令流基址（段基址 + script offset）
    uint32_t segment_index;   // 所属段（贴图从 AnmResource::segment_textures 取）
};

// 一个 ANM 资源（原版 0x13c 字节）。管理器在全局对象的 +0x312072c + idx*4 处存指针。
struct AnmResource {
    uint32_t index;              // +0x000 资源编号（加载时传入）
    uint8_t  pad_004[8];         // +0x004 未用填充（原版 name 在 +0x00c）
    uint8_t  name[0xfc];         // +0x00c 名字缓冲区（FUN_0041b310 sprintf）
    uint8_t* file_data;          // +0x108 整份 ANM 文件数据
    uint8_t* vms;                // +0x10c 每 sprite 一个 VM，0x60c 字节/个
    uint32_t segment_count;      // +0x110
    uint32_t sprite_count;       // +0x114
    uint32_t script_count;       // +0x118
    AnmScriptEntry* scripts;     // +0x11c 脚本表（原版 0x44 字节/项；本层已解析）
    void** segment_textures;     // +0x120 每段一张 IDirect3DTexture9*（本层）
    AnmSegment* segments;        // +0x124 段信息（原版 0x18 字节/段；本层用 AnmSegment）
    volatile int32_t upload_state; // +0x128 0 = 空闲；加载线程与等待线程通过它同步
    uint8_t  pad_12c[8];         // +0x12c
    uint32_t ref_count;          // +0x134
    uint8_t  pad_138[4];         // +0x138
};
static_assert(offsetof(AnmResource, name) == 0x00c, "AnmResource::name");
static_assert(offsetof(AnmResource, file_data) == 0x108, "AnmResource::file_data");
static_assert(offsetof(AnmResource, segments) == 0x124, "AnmResource::segments");
static_assert(offsetof(AnmResource, ref_count) == 0x134, "AnmResource::ref_count");
static_assert(sizeof(AnmResource) == 0x13c, "AnmResource size");

// VM 绘制类型：取自 (vm->flags2 +0x534) >> 26 & 0x1f（sub_481210 的 switch）。
enum AnmVmRenderType : uint32_t {
    ANM_TYPE_00 = 0x00,  // 旋转 quad（sub_47e8f0 + sub_47dce0）
    ANM_TYPE_01 = 0x01,  // 旋转 quad（sub_47ed50 + sub_47dce0）
    ANM_TYPE_02 = 0x02,  // 同 00（uVar3=0）
    ANM_TYPE_03 = 0x03,  // 同 01
    ANM_TYPE_04 = 0x04,  // sub_47f090 后 sub_47dce0
    ANM_TYPE_05 = 0x05,  // sub_47feb0，结束后把 4 个矩阵分量复位为 1.0
    ANM_TYPE_06 = 0x06,  // sub_47f530
    ANM_TYPE_07 = 0x07,  // 距离雾/透明度衰减（用 DAT_004cd5dc 雾参数）
    ANM_TYPE_08 = 0x08,  // 主精灵绘制 sub_480160
    ANM_TYPE_09 = 0x09,  // sub_480f70
    ANM_TYPE_0B = 0x0b,  // sub_4810d0（带贴图槽切换）
    ANM_TYPE_0C = 0x0c,
    ANM_TYPE_0D = 0x0d,
    ANM_TYPE_0E = 0x0e,
    ANM_TYPE_0F = 0x0f,  // 阴影/叠加：切 blend 后走 sub_480160
    ANM_TYPE_10 = 0x10,  // 单 quad（sub_481d90）
    ANM_TYPE_11 = 0x11,  // 曲线/线（sub_482c10）
    ANM_TYPE_12 = 0x12,  // sub_482e70
    ANM_TYPE_13 = 0x13,  // sub_483060
    ANM_TYPE_14 = 0x14,  // sub_482b40
    ANM_TYPE_15 = 0x15,  // sub_482b40
    ANM_TYPE_16 = 0x16,  // sub_482b40
    ANM_TYPE_18 = 0x18,  // sub_480a50
    ANM_TYPE_19 = 0x19,  // sub_480a50
    ANM_TYPE_1A = 0x1a,  // sub_482590
    ANM_TYPE_1B = 0x1b,  // sub_482170
    ANM_TYPE_1C = 0x1c,  // sub_482810
};

// 一个 ANM 实例（原版 0x60c 字节）。字段语义按反编译读写位置整理；
// 未确认的偏移保留原偏移号，避免后续实现时靠猜。
struct AnmVm {
    int32_t  saved_time;         // +0x000 op5 中断前的脚本时间，op7 恢复
    float    uv_offset_u;       // +0x004 本层保存原版+0x7c的UV偏移，避免与sprite_w重叠
    float    uv_offset_v;       // +0x008 本层保存原版+0x80的UV偏移
    float    uv_scale_u;        // +0x00c 本层保存原版+0x6c的UV缩放
    float    uv_scale_v;        // +0x010 本层保存原版+0x70的UV缩放
    int32_t  saved_cursor;       // +0x014 op5 中断前的指令位置
    int32_t  draw_layer;         // +0x018 绘制层号（sub_482260 按它过滤）
    int32_t  anm_index;          // +0x01c 所属 ANM 资源编号
    int32_t  anm_table_index;    // +0x020 管理器资源表下标
    int32_t  script_index;       // +0x024 当前 script
    int32_t  sprite_index;       // +0x028 当前 sprite（sub_478580 用它索引指令流）
    int32_t  script_cursor;      // +0x02c 当前指令字节偏移；-1 = 脚本结束（sub_478580）
    float    pos_x;              // +0x030
    float    pos_y;              // +0x034
    float    pos_z;              // +0x038
    float    rot_a;              // +0x03c 旋转角 x（op 401 写，sub_478580 case 0x12）
    float    rot_b;              // +0x040 旋转角 y
    float    rot_z;              // +0x044 主旋转
    uint8_t  pad_048[0x0c];      // +0x048
    float    scale_x;            // +0x054
    float    scale_y;            // +0x058
    float    scale_z;            // +0x05c
    // +0x060 原版是第二套变换矩阵分量（sub_47d8b0 写 +0x064/+0x068）；
    // 本层把当前精灵的 UV 存进这段（anm_set_sprite 写入，绘制时取用）。
    float    uv[4];              // +0x060 u0,v0,u1,v1
    uint8_t  pad_070[0x04];      // +0x070
    float    rot_x;              // +0x074
    float    rot_y;              // +0x078
    // +0x07c 原版未用；本层存当前精灵像素尺寸（绘制 quad 的半宽高）。
    float    sprite_w;           // +0x07c
    float    sprite_h;           // +0x080
    float    origin_x;          // +0x084 op436：精灵局部原点偏移（缩放/旋转之前减去）。
    float    origin_y;          // +0x088
    uint8_t  pad_08c[0xa8];      // +0x08c
    uint32_t ref_count;          // +0x134
    uint8_t  pad_138[0x5c];      // +0x138
    uint32_t flags;              // +0x194 位 0x100/0x200/0x400/0x800/0x1000/0x1800
    uint8_t  pad_198[0x234];     // +0x198
    float    uv_velocity_u;      // +0x3cc op425，sub_47bce0每帧积分
    float    uv_velocity_v;      // +0x3d0 op426
    float    matrix_src[16];     // +0x3d4 变换源（16 float）
    float    matrix_world[16];   // +0x414 传给 D3DTS_WORLD 的世界矩阵
    uint8_t  pad_454[0x40];      // +0x454
    int32_t  label;              // +0x494 等待的 label（sub_488be0 写入，解释器命中 op5 后清零）
    // +0x498 原版未用；本层存当前脚本表项（op 300 设置，绘制时取段号/贴图）。
    const AnmScriptEntry* script; // +0x498
    uint8_t  pad_49c[0x04];      // +0x49c
    uint16_t sprite_id;          // +0x4a0 sprite 编号（short）
    uint16_t script_sprite_id;  // +0x4a2 op300回调映射前的精灵号
    uint32_t frame_counter;      // +0x4a4 脚本变量 10000 的存储；绘制时按帧减一取 sprite 帧
    uint8_t  pad_4a8[0x7c];      // +0x4a8
    // 颜色：字节 B@0x524 G@0x525 R@0x526 A@0x527（sub_47c750）。
    uint32_t color;              // +0x524
    uint32_t color_alt;          // +0x528 第二套颜色（混合用），字节序同上
    uint8_t  pad_52c[0x08];      // +0x52c
    uint32_t flags2;             // +0x534 bit0/bit1 为绘制开关；bit26..30 = AnmVmRenderType
    uint32_t flags3;             // +0x538 bit 0x180 隐藏；0x40000 忽略父变换；
                                 // 0x1c00000 混合模式（0x400000/0x800000 有额外缩放）；
                                 // bit 4..6 = 旋转顺序
    // +0x53c 原版是第二颜色插值的中间状态；本层把两个可见标志的判定结果存这里。
    float    near_alpha_start;  // +0x53c op439近距透明阈值
    float    near_alpha_end;    // +0x540
    uint32_t handle;             // +0x544 队列句柄（低位 15 bit 编号 + 高位计数）
    uint32_t handle_mask;        // +0x548 句柄低位掩码
    float    expr_stack_a;       // +0x54c 表达式栈 1（sub_402a60/sub_402a80 操作）
    uint32_t time;               // +0x550 当前时间：sub_478580 用 (int) 与指令 time 比较
    float    expr_stack_a2;      // +0x554
    uint8_t  pad_558[0x04];      // +0x558
    uint32_t interp_flags;       // +0x55c
    float    expr_stack_b;       // +0x560 表达式栈 2（+0x560 起，sub_402a80 复位）
    float    interp_b1;          // +0x564
    float    interp_b2;          // +0x568
    uint8_t  pad_56c[0x04];      // +0x56c
    uint32_t node_flags;         // +0x570
    void*    node_self;          // +0x574 绘制队列节点
    void*    node_next;          // +0x578
    void*    node_prev;          // +0x57c
    uint8_t  pad_580[0x40];      // +0x580
    void*    parent;             // +0x5c0 父 VM（绘制时叠加变换）
    void*    callback_object;    // +0x5c4 Bullet/Laser回指；原+0x5fc在本层用作texture
    uint32_t draw_arg;           // +0x5c8 部分绘制类型的附加参数
    uint8_t  pad_5cc[0x0c];      // +0x5cc
    uint32_t callback_index;     // +0x5d8 绘制前回调下标（callbacks 表 0x4b48d0）
    // +0x5dc 原版是绘制后回调下标；本层把绘制队列链表指针存这里。
    AnmVm*   queue_next;         // +0x5dc
    AnmVm** owner_slot;          // +0x5e0 端口持有槽；原生句柄失效在释放时映射为清槽
    uint8_t  pad_5e4[0x08];      // +0x5e4
    uint32_t sprite_callback;    // +0x5ec op300精灵回调（原表0x4b48c0）
    float    extra_x;            // +0x5f0 附加平移
    float    extra_y;            // +0x5f4
    float    extra_z;            // +0x5f8
    // +0x5fc 原版未用；本层存当前贴图（IDirect3DTexture9*）。
    const void* texture;         // +0x5fc
    uint8_t  pad_600[0x0c];      // +0x600
};
static_assert(sizeof(AnmVm) == 0x60c, "AnmVm 必须是 0x60c 字节（原版每 sprite 一个 VM）");
static_assert(offsetof(AnmVm, matrix_src) == 0x3d4, "AnmVm::matrix_src");
static_assert(offsetof(AnmVm, label) == 0x494, "AnmVm::label");
static_assert(offsetof(AnmVm, color) == 0x524, "AnmVm::color");
static_assert(offsetof(AnmVm, flags2) == 0x534, "AnmVm::flags2");
static_assert(offsetof(AnmVm, handle) == 0x544, "AnmVm::handle");

// 全局 ANM 管理器（原版 DAT_0051f65c 是包含它的大对象，字段按字节偏移给出）。
struct AnmManager {
    uint8_t  pad_000[0x0e0];
    uint32_t draw_count;         // +0x0e0 本帧绘制数
    uint8_t  pad_0e4[0x60c];     // +0x0e4
    void*    queue_head;         // +0x6f0 绘制队列头（节点 = vm+0x574）
    void*    queue_tail;         // +0x6f4
    void*    queue_head2;        // +0x6f8 第二队列（背景/前景层）
    void*    queue_tail2;        // +0x6fc
    uint8_t  pad_700[0x3120024];  // 到资源表
    AnmResource** resources;     // +0x312072c 资源表（idx 索引）
    uint8_t  pad_rend[0x310];     // +0x3120730
    uint32_t texture_factor;     // +0x3120e00 缓存的 TEXTUREFACTOR
    uint32_t cached_texture;     // +0x3120e04 缓存的 SetTexture
    uint8_t  fvf_state;          // +0x3120e0a 当前 FVF/顶点格式状态
    uint8_t  tex_stage_state;    // +0x3120e0f 当前 TSS 状态
    uint32_t cached_script;      // +0x3120e14
    uint32_t vertex_buffer;      // +0x3120e18
    uint8_t  pad_batch[0x50];    // +0x3120e1c
    uint32_t batch_pending;      // +0x3120e6c 待提交顶点数
    uint8_t  pad_tail[0x93abf4];  // 到颜色/计数器区
    uint32_t queue_id;           // +0x39724ac 句柄计数
    uint8_t  color_r;            // +0x39724b0 全局颜色乘数
    uint8_t  color_g;            // +0x39724b1
    uint8_t  color_b;            // +0x39724b2
    uint8_t  color_a;            // +0x39724b3
};

// 管理器句柄（原版全局对象里含 AnmManager 的那块）。
AnmManager* anm_manager();

// sub_486880: 取（必要时加载）ANM 资源；贴图上传在另一线程，函数内忙等直到完成。
AnmResource* anm_get(int index, const char* name);
// sub_4867e0: 分配并加载资源。
AnmResource* anm_load_resource(int index, const char* name);
// 所有引用VM由各拥有者销毁后卸载该资源，并清管理器的index缓存。
void anm_release_resource(int index);
// sub_4865b0: 解析 ANM 文件。
void anm_load_file(AnmResource* res, const char* name);
// sub_486920: 推进一段贴图上传。
void anm_upload_step(AnmManager* mgr, AnmResource* res);
// 4860c0 / 4858e0：在既有动态贴图中载入DAT图片，并填充透明边缘RGB。
void anm_load_image(AnmResource* res, uint32_t segment_index, const void* data, size_t size);

// sub_488350: 插入绘制队列。
uint32_t anm_queue_add(AnmVm* vm);
// sub_488cf0: 从绘制队列移除。
void anm_queue_remove(uint32_t handle);
// sub_488260: 绘制某一层。
void anm_draw_layer(int layer);
// sub_481210: 绘制单个 VM（按类型分派）。
void anm_draw_vm(AnmVm* vm);
// sub_47d910；loading406d00在此之后改独立alpha状态，再画effect194。
void anm_apply_blend(AnmVm* vm);
// sub_481d20：攻击精灵四角，按绘制变换换算到640×480画面坐标。
void anm_vm_corners(const AnmVm* vm, float corners[4][2]);

// sub_478580: VM 复位。
void anm_vm_reset(AnmVm* vm);
// sub_477b00: 设置 sprite（op 300 调用）。脚本表项 0x44 字节/项，按 sprite id
//   在资源脚本表里查表项，把 UV/尺寸/角度写进 VM；找不到就保持原状。
void anm_set_sprite(AnmVm* vm, AnmResource* res, int sprite);
// sub_488be0: 设置 vm->label(+0x494)。解释器进入时若 label 非 0，就在当前脚本流里
//   找 type 5 且 payload(float) == label 的指令跳过去，然后清 label 和 flags2 的 0x4000 位。
//   游戏逻辑用它在动画之间切换（原版大量调用 sub_488be0）。
void anm_set_label(AnmVm* vm, int label);
// sub_488c60：中断后立即执行根 VM 及其子 VM。
void anm_interrupt(AnmVm* vm, int label);
// sub_4890b0/489230：按脚本号查找子 VM（index 为同号的第几个）。
AnmVm* anm_find_child(AnmVm* vm, int script, int index = 0);
// sub_478580: ANM 脚本解释器（原版 13086 字节，最大的单个函数）。
//   vm->+0x2c 是当前指令的字节偏移；解释器从 sprite 的指令流基址
//   （资源 +0x120 表[sprite_index] 指向的指令流）加该偏移取指令，循环执行
//   `while (instr->time <= vm->time)`，每条执行后 vm->+0x2c += instr->length。
//   解释器里出现的指令语义（已确认部分，type 为 AnmInstr::type）：
//     2   结束脚本：vm->+0x2c = -1
//     3/4 脚本结束/等待：清 +0x534 bit0，进入等待状态
//     5   跳转/循环标记：与 vm->+0x494(float) 比较决定跳转目标
//     6   表达式求值：sub_402a60 压栈，参数 mask bit0 时用 sub_4781e0 取值
//     7   脚本起始：sub_418c40(vm+0x54c, *vm, vm[1])
//     100 赋值：sub_4781e0 取值、sub_478480 取变量地址后写入
//     0x65..0x83、0xc9.. 等为各类属性/变换/精灵指令
//   表达式栈：vm+0x54c（sub_402a60 操作）与 vm+0x560（sub_402a80 复位）。
//   变量访问：sub_4781e0(vm, id) 读、sub_478480(vm, &id, mask) 取地址。
void anm_execute_script(AnmVm* vm);

// sub_4885d0: 释放 VM：摘出绘制队列、调用 +0x5dc 回调、归还对象池。
//   池槽编号 = vm->handle_mask(+0x548)，槽步长 0x624，池起点 manager+0xd0c；
//   观察到的 VM 分配大小是 0x60c（sub_4865b0），差的 0x18 是池槽头部。
void anm_free_vm(AnmVm* vm);

// 脚本变量访问：ANM 指令的参数表达式用 id 引用 VM 字段。
// sub_4781e0(vm, id) 返回变量值；sub_478480(vm, &id, mask) 返回变量地址（写入用）。
// id 映射（TH18，0x2710 = 10000 起）：
//   10000 -> +0x4a4  frame_counter
//   0x2711 -> +0x4a8   0x2712 -> +0x4ac   0x2713 -> +0x4b0
//   0x2714 -> +0x4b4 (float)  0x2715 -> +0x4b8  0x2716 -> +0x4bc  0x2717 -> +0x4c0
//   0x2718 -> +0x4d0   0x2719 -> +0x4d4
//   0x2726 -> sub_4615a0(&DAT_004cf280, vm->+0x4e0)   随机值/全局表
//   0x272b -> +0x4d8 (float)  0x272c -> +0x4dc  0x272d -> +0x4e0
//   0x2731 -> +0x4c4 (float)  0x2732 -> +0x4c8  0x2733 -> +0x4cc
uint32_t anm_vm_get_var(AnmVm* vm, uint32_t id);
uint32_t* anm_vm_var_ptr(AnmVm* vm, uint32_t* id, uint8_t mask);

// 资源里第 index 个 VM（每 sprite 一个，步长 sizeof(AnmVm)）。
inline AnmVm* anm_vm_at(AnmResource* res, int index)
{
    return reinterpret_cast<AnmVm*>(res->vms) + index;
}

// sub_489320: 从 ANM 管理器 VM 池取一个 VM（原版子代理 B/C 的子弹、特效、激光共用）。
// 端口不模拟原版的池数组，直接分配 sizeof(AnmVm)，用完调 anm_vm_free 归还。
// 返回的 VM 已 anm_vm_reset，并绑定 res 的资源表下标。
AnmVm* anm_vm_alloc(AnmResource* res);
// 释放 anm_vm_alloc 分配的 VM（先摘绘制队列）。
void anm_vm_free(AnmVm* vm);
// 已入队的原托管VM交给A22/A0b；空slot为管理器持有的独立根。
void anm_manage_vm(AnmVm* vm, AnmVm** owner_slot);
// sub_407420: 设脚本号：vm->script 指向资源脚本表第 script_id 项，游标与时间归零，
// 并置 flags2 bit0（允许绘制）。脚本表项由 op300（anm_set_sprite）写 UV/尺寸。
void anm_vm_set_script(AnmVm* vm, AnmResource* res, int script_id);
// 先载入-1静态初始化，保留第0帧/appearance给Bullet的ETEX创建序列。
void anm_vm_bind_script(AnmVm* vm, AnmResource* res, int script_id);
// 原458480→487580：取text59当前精灵区域，复制场景游戏区并将alpha置255。
void anm_copy_game_region(AnmVm* target);

// 设脚本时立即执行第 0 帧。以后每帧推进根 VM 一次，子 VM 由此递归推进；
// anm_set_label / anm_vm_free 同样作用于它用 op500 创建的子 VM。
void anm_vm_update(AnmVm* vm);
// op506的独立发射VM由原第一队列A0x22推进，场景卸载时按资源销毁。
void anm_delete_independent(int resource_index);
// 原版 ANM/STD 共用的0x58字节三向量插值块及缓动函数。
void interp_eval3(uint8_t* base, float out[3]);
void interp_eval2(uint8_t* base, float out[2]);
float interp_eval1(uint8_t* base);
float easing_ratio(uint32_t type, float time, float duration);

// 屏幕尺寸（正交投影用；原版全局 DAT_0056aca0 是屏幕宽高缩放）。
void anm_set_screen(float width, float height);


