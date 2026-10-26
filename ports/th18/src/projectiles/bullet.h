#pragma once
#include <cstdint>

// TH18 敌弹发射槽（每敌机 16 个，位于 Enemy+0x17c4，每槽 0x488 字节）。
// 槽由 ECL 600..632 指令配置，实际子弹实体的生成/更新入口尚未定位（P1 继续）。
//
// 相关原版函数：
//   0x0042e100  敌机构造：逐槽 memset(0x488) 并把槽 +0x47c 置 -1
//   0x0042fe80  敌机每帧表现刷新（与槽无关，但与 0x1350 的 16 个槽句柄有关）
//   0x00449090  遍历 0x004cf3f4 的激光列表并回调 vtable+0x28 —— opcode 518/613 的消弹
//   0x0042cca0  取当前寄存器值写回（槽初始化/601 前后调）
//   0x00427810  初始化槽 +0x166 处的"坐标对象"
//   0x00413280  按敌机坐标做世界坐标变换
//   0x0043a030 / 0x0043a110 / 0x0043a250 / 0x0043a260   ANM 对象创建/取值辅助
namespace bullet {

constexpr int32_t kSlotCount = 16;
constexpr uint32_t kSlotSize = 0x488;

// 单槽。已确认字段（偏移为槽内相对字节偏移）：
//   0x000 float   602 写入（槽坐标 x）
//   0x004 float   602 写入（槽坐标 y）
//   0x014 float   600 置 0；604 写入
//   0x018 float   604 写入
//   0x01c float   600 置 2.0；605 写入
//   0x020 int32   605 写入（经 LAB_00434231 的转换结果）
//   0x168 float x 601 写入
//   0x16c float y 601 写入
//   0x170 float z 601 写入
//   0x46c..0x478  600 写入的 4 个常量（0x10000/0x20000/0x30000/0x40000 整数值）
//   0x47c int32   构造置 -1，609-612 写子弹类型
struct Slot {
    int32_t f000;
    int32_t f004;
    uint8_t pad008[0x14 - 0x08];
    float f014;
    float f018;
    float f01c;
    int32_t i020;
    uint8_t pad024[0x168 - 0x24];
    float x;
    float y;
    float z;
    uint8_t pad174[0x46c - 0x174];
    uint32_t const46c;
    uint32_t const470;
    uint32_t const474;
    uint32_t const478;
    int32_t type;
    uint8_t tail[0x488 - 0x480];
};

static_assert(sizeof(Slot) == kSlotSize);

// ECL 发射槽指令（FUN_00430d30 的 case 组 0x74..0x8f）。参数格式查 stage/ecl_opcodes.h。
//   600 0x74  S      初始化槽
//   601 0x75  S      由自身坐标 + 发射基点(fire_offset)算槽坐标，写 +0x168..+0x170
//   602 0x76  SS     直接写槽 +0x000/+0x004
//   603 0x77  Sff    写 fire_offset[slot]（+0x6084/+0x6088）
//   604 0x78  Sff    写槽 +0x014/+0x018（经 0x004028f0 转换）
//   605 0x79  Sff    写槽 +0x01c，再经 LAB_00434231 写槽 +0x020
//   606 0x7a  SSS    写槽 +0x46e（u16）与槽 +0x46c 附近
//   607 0x7b  SS     写槽 +0x280 附近（u16）
//   608 0x7c  Sff    写槽 +0x288/+0x28c
//   609 0x7d  ...    填子弹类型与 4-6 个参数（组内 610/611/612 同段不同参数）
//   613 0x7e  S      消弹：0x00449090(1)
//   614 0x7f  SS     整槽 0x488 字节拷贝（+ 同步 fire_anm/fire_offset）
//   615 0x80  S      0x00429250(0x0042d1e0(vm), 1)
//   616 0x81  S      0x00429250(0x0042d1e0(vm), 0)
//   624 0x89  ...    按难度（*(0x4cccd00)=DAT_004ccd00）选参数写槽 +0x01c/+0x020
//   625 0x8a  ...    按难度选参数写槽 +0x46e/+0x46c
//   626..630 0x8b..0x8f、632/637/640、700..711 其余槽/激光相关指令

// 敌弹管理器：DAT_004cf2b4（0x0041b850 构造，0x0041bb50 销毁）。
// 构造流程：读入子弹 ANM 文件（文件名传入）→ 0x00486880 建 ANM（id = (难度&1)+3）→
// 复制数据到 this+0x3454，并按 *(u16*)(数据+2) 项 × 0x60c 字节分配 this+0x3450 的数组；
// this+0x348c = GameState 指针；this+0x3464 = ANM 对象；this+0x230 起 0x59 个 dword
// 从 DAT_004cd478 拷入（子弹默认参数块：+0x238=-1000.0f、+0x244=600.0f）。
// this+4 / this+8 / this+0x349c 是三个任务线程：16 / 3 / 6 步的更新链。
struct Manager {
    uint8_t pad000[0x230];
    int32_t params[0x59];  // +0x230 默认参数块
    uint8_t pad394[0x3450 - 0x230 - 0x59 * 4];
    void* bullets;         // +0x3450 每项 0x60c 字节
    void* data;            // +0x3454 子弹数据副本
    void* entry_table;     // +0x3458
    void* field345c;       // +0x345c
    void* field3460;       // +0x3460
    void* anm;             // +0x3464
    uint8_t pad3468[0x348c - 0x3468];
    void* game_state;      // +0x348c = DAT_004cccdc
    uint8_t pad3490[0x3494 - 0x3490];
    void* file_data;       // +0x3494
    uint32_t file_size;    // +0x3498
    void* task;            // +0x349c
};

inline Manager* manager = nullptr;  // 原版 DAT_004cf2b4

}  // namespace bullet
