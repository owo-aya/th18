#include <windows.h>
#include <cstdint>

#include "core/input.h"
#include "platform/input.h"
#include "storage/storage.h"

// 默认键位表（原版从 th18.cfg 读取，P1 接入配置后替换）。
struct KeyMap {
    int vk;
    uint32_t bit;
};

static const KeyMap kFixedKeys[] = {
    {VK_UP, ACTION_UP},
    {VK_DOWN, ACTION_DOWN},
    {VK_LEFT, ACTION_LEFT},
    {VK_RIGHT, ACTION_RIGHT},
    {VK_RETURN, ACTION_MENU_OK},
};

uint32_t input_sample_device() {
    uint8_t ks[256];
    GetKeyboardState(ks);

    uint32_t bits = 0;
    for (const KeyMap& km : kFixedKeys) {
        if (ks[km.vk] & 0x80) {
            bits |= km.bit;
        }
    }
    const auto* mappings=reinterpret_cast<const uint16_t*>(storage_config().data+0x2c);
    constexpr uint32_t actions[10]={ACTION_SHOT,ACTION_BOMB,ACTION_SLOW,ACTION_MENU_CANCEL,
        ACTION_UP,ACTION_DOWN,ACTION_LEFT,ACTION_RIGHT,ACTION_CARD,ACTION_CARD_CYCLE};
    for(int i=0;i<10;++i)if(mappings[i]!=0xffff && (ks[mappings[i]]&0x80)!=0)bits|=actions[i];
    return bits;
}
