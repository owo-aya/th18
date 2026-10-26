#pragma once
#include <cstdint>

// D3D9 绘制层（原版 th18.exe）。
// 设备来源：移植版统一走 platform/gfx.h 的 gfx_device()，本层不保存第二份设备全局；
// 下面记录原版全局名只为对照反编译。
//   DAT_004ccdf8 = IDirect3DDevice9*
//   DAT_004ccdf4 = IDirect3D9*
//   0x4ccdf0     = 渲染器/状态对象（混合模式切换都传这个地址）
//
// 已确认的设备 vtable 槽（原版按字节偏移直接调用）：
//   +0x05c CreateTexture(w, h, levels, usage, format, pool, &tex, shared)
//   +0x0a4 BeginScene
//   +0x0a8 EndScene
//   +0x0b0 SetTransform(state, matrix)，0x100 = D3DTS_WORLD，0x10 = D3DTS_TEXTURE0
//   +0x0e4 SetRenderState(state, value)，0x3c = D3DRS_TEXTUREFACTOR
//   +0x104 SetTexture(stage, texture)
//   +0x10c SetTextureStageState(stage, type, value)
//   +0x114 SetSamplerState(sampler, type, value)
//   +0x144 DrawPrimitive(primitive_type, start_vertex, primitive_count)
//   +0x14c DrawPrimitiveUP(primitive_type, primitive_count, data, stride)
//   +0x164 SetFVF(fvf)
//   +0x190 SetStreamSource(stream, buffer, offset, stride)
//
// 绘制流水线（原版）：
//   sub_47d910  设置纹理/FVF/采样状态（各绘制函数入口都先调它）
//   sub_47dce0  发一个变色 quad（顶点 0x14 字节：x,y,z,w,two_d?）
//   sub_47e730  提交当前批次（38 处调用；批处理上限见 AnmManager::batch_pending）
//   sub_478580  VM 复位
//   sub_481210  VM 绘制分派（见 presentation/anm.h）
//   sub_480160  主精灵绘制（type 8）
//   sub_480a50  type 0x18/0x19 绘制
//   sub_480f70  type 9/0xc/0xd/0xe 绘制
//   sub_4810d0  type 0xb 绘制（带贴图槽切换）
//   sub_481d90 / sub_482170 / sub_482590 / sub_482810 /
//   sub_482b40 / sub_482c10 / sub_482e70 / sub_483060  单 quad / 曲线类图元
//   sub_47e8f0 / sub_47ed50 / sub_47f090 / sub_47f530 / sub_47feb0  带旋转的 quad 族
//
// 状态切换：
//   sub_454820(0x4ccdf0)  设置混合状态（绘制 3D/sprite 前调用）
//   sub_455ec0(0x4ccdf0)  恢复默认状态
//   sub_454760 / sub_4547a0 / sub_4547e0  成对的状态切换（按 VM 标志位判断）

struct RenderState; // 原版 0x4ccdf0 指向的状态对象，字段未展开

// 顶点格式常量（原版多处 SetFVF(0x102/0x142) 等）。
enum RenderFvf : uint32_t {
    FVF_XYZ_DIFFUSE_TEX1 = 0x102, // XYZ | DIFFUSE | TEX1
    FVF_XYZ_DIFFUSE_TEX2 = 0x142, // 额外 TEX2
};

// 原版 sub_47dce0 的 quad 顶点（0x14 字节）。
struct SpriteVertex {
    float x, y, z;
    uint32_t color;
    float u, v;
};

// sub_47d910: 绘制前设置纹理与顶点格式。
void render_setup_draw_state(int anm_index, int script_index);
// sub_47e730: 提交批次。
void render_flush();
// sub_454820 / sub_455ec0: 混合状态切换与恢复。
void render_set_blend_mode();
void render_reset_state();
