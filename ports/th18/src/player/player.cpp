#include "player/player.h"
#include "player/bomb.h"

#include <cmath>
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <initializer_list>

#include "core/input.h"
#include "core/rng.h"
#include "core/app.h"
#include "core/game_session.h"
#include "core/thread.h"
#include "presentation/anm.h"
#include "resources/dat.h"
#include "cards/card_manager.h"
#include "gameplay/spell.h"
#include "cards/card_passive.h"
#include "presentation/sound.h"
#include "presentation/msg.h"
#include "stage/enemy.h"
#include "stage/stage.h"
#include "stage/session.h"
#include "stage/hud.h"
#include "projectiles/laser.h"
#include "replay/replay.h"

// 自机（TH18 原版）实现依据：
//   创建       FUN_0045af20 @ 0x45af20
//   载入 SHT   FUN_0045a7a0 @ 0x45a7a0
//   SHT 初始化 FUN_0045a600 @ 0x45a600 / FUN_0045a690 @ 0x45a690
//   销毁       FUN_0045b020 @ 0x45b020
//   主更新     FUN_0045be90 @ 0x45be90
//   移动       FUN_0045b170 @ 0x45b170
// 反编译在 toolchains/re/dump/ 与 $env:TEMP\th18c\dec9.txt。

// 原版对象大小与关键偏移由 FUN_0045af20/FUN_0045b170 的反编译确定。
static_assert(sizeof(Player) == kPlayerSize);
static_assert(offsetof(Player, x) == 0x620);
static_assert(offsetof(Player, state) == 0x476ac);
static_assert(offsetof(Player, trail) == 0x47800);
static_assert(offsetof(Player, sht) == 0x47940);

Player* g_player = nullptr;           // DAT_004cf410
int32_t g_player_character = 0;       // DAT_004cccf4  pl00..pl03
int32_t g_player_shot_type = 0;       // DAT_004cccf8  与角色相加选 SHT 条目
GameStatusBlock g_status = {};        // 0x4cccdc

namespace {
int32_t player_bullet_alloc(const float* pos,float angle,int32_t life,int32_t damage);

const char* const kPlayerShtNames[4] = {"pl00.sht", "pl01.sht", "pl02.sht", "pl03.sht"};

// 自机/子弹共用一个 plXX.anm 资源：FUN_0045a7a0 行 35/51 把同一个 AnmResource 写 +0xC/+0x10。
const char* const kPlayerAnmNames[4] = {"pl00.anm", "pl01.anm", "pl02.anm", "pl03.anm"};
AnmVm* g_option_vm[4] = {};
AnmVm* g_option_max_vm[4] = {};
AnmVm* g_focus_vm;
int g_option_power = -1;
bool g_option_full = false;

inline AnmResource* player_anm(Player* p) {
    return reinterpret_cast<AnmResource*>(
        static_cast<uintptr_t>(static_cast<uint32_t>(p->anm_player)));
}
inline AnmVm* player_anm_vm(Player* p) { return reinterpret_cast<AnmVm*>(p->vm_player); }

// 方向位移表，原版 0x4b7040..0x4b7084：
//   dir 0 静止 (0,0)、1 上 (0,-1)、2 下 (0,1)、3 左 (-1,0)、4 右 (1,0)、
//   5 左上 (-1,-1)、6 右上 (1,-1)、7 左下 (-1,1)、8 右下 (1,1)。
const int32_t kDirection[9][2] = {
    {0, 0}, {0, -1}, {0, 1}, {-1, 0}, {1, 0}, {-1, -1}, {1, -1}, {-1, 1}, {1, 1}};

constexpr int32_t kBoundX = 0x5c00;     // ±184 像素（定点 1px = 128）
constexpr int32_t kMinY = 0x1000;       // 32 像素
constexpr int32_t kMaxY = 0xd800;       // 432 像素
constexpr float kSubPixel = 0.0078125f; // 1/128
constexpr int32_t kSpeedUnit = 128;     // SHT 速度字段 ×128 转定点

inline constexpr uint32_t kFlagDeath = 0x0002;    // +0x4779c bit1 死亡/Bomb 中
inline constexpr uint32_t kFlagScriptX = 0x0180; // +0x4779c bit8/9 脚本横移中

// 按原版偏移直接读写尚未命名字段的整数。
inline void put_i32(Player* p, int32_t offset, int32_t value) {
    *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + offset) = value;
}
inline int32_t get_i32(Player* p, int32_t offset) {
    return *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(p) + offset);
}
inline float get_f32(Player* p, int32_t offset) {
    return *reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(p) + offset);
}
inline void put_f32(Player* p, int32_t offset, float value) {
    *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(p) + offset) = value;
}

void player_options_rebuild(Player* p) {
    const int power = g_status.power / g_status.power_step;
    const bool full = g_status.power == g_status.power_max;
    const auto* sht = static_cast<const uint8_t*>(p->sht);
    if (power != g_option_power || full != g_option_full) {
        static constexpr int option_script[] = {15,17,12,10};
        static constexpr int full_script[] = {16,18,13,12};
        for (int i=0;i<4;++i) {
            if (g_option_vm[i]) anm_vm_free(g_option_vm[i]);
            if (g_option_max_vm[i]) anm_vm_free(g_option_max_vm[i]);
            g_option_vm[i]=g_option_max_vm[i]=nullptr;
            const int q=0x670+i*0xf0;
            put_i32(p,q,i<power?2:0);
            if(i>=power)continue;
            const int slot=power*(power-1)/2+i;
            for(int k=0;k<4;++k) {
                const int source=(k<2?0x40:0x90)+slot*8+(k&1)*4;
                put_i32(p,q+0x64+k*4,int(*reinterpret_cast<const float*>(sht+source)*128));
            }
            if(g_player_character==2) {
                put_f32(p,q+0xa8,*reinterpret_cast<const float*>(sht+0x40+slot*8));
                put_f32(p,q+0xac,*reinterpret_cast<const float*>(sht+0x44+slot*8));
            }
            put_i32(p,q+0xd4,1);
            auto create=[&](int script) {
                AnmVm* vm=anm_vm_alloc(player_anm(p));
                anm_vm_bind_script(vm,player_anm(p),script);
                vm->draw_layer=14;vm->flags3=(vm->flags3&~0x200000u)|0x101000u;
                anm_vm_update(vm);vm->extra_x=0;vm->extra_y=-1000;
                anm_queue_add(vm);return vm;
            };
            g_option_vm[i]=create(option_script[g_player_character]);
            anm_manage_vm(g_option_vm[i],&g_option_vm[i]);
            if(full) {
                g_option_max_vm[i]=create(full_script[g_player_character]);
                anm_manage_vm(g_option_max_vm[i],&g_option_max_vm[i]);
            }
        }
        g_option_power=power;g_option_full=full;
        card_manager_options_reset();
    }
}
void player_options_update(Player* p) {
    player_options_rebuild(p);
    const int power=g_status.power/g_status.power_step;
    p->unk_476f8=power;
    for(int i=0;i<power;++i) {
        const int q=0x670+i*0xf0;
        if(get_i32(p,q)==0)continue;
        int x=p->x_sub+get_i32(p,q+0x64+p->focus_mode*8);
        int y=p->y_sub+get_i32(p,q+0x68+p->focus_mode*8);
        if(p->flags_4779c&2u) {
            x=p->x_sub;y=p->y_sub;
            if(p->follow_stun>29) {
                put_i32(p,q,0);
                for(AnmVm* vm:{g_option_vm[i],g_option_max_vm[i]})if(vm)anm_set_label(vm,1);
                continue;
            }
        } else if(g_player_character==2) {
            const float angle=p->shot_angle+get_f32(p,q+0xa8),radius=get_f32(p,q+0xac);
            x=p->x_sub+int(std::cos(angle)*radius*128);y=p->y_sub+int(std::sin(angle)*radius*128);
        }
        put_i32(p,q+0x54,x);put_i32(p,q+0x58,y);
        if(get_i32(p,q+0xd4)) {
            put_i32(p,q+0x5c,x);put_i32(p,q+0x60,y);put_i32(p,q+0xd4,0);
        } else if(p->follow_factor>29) {
            const int dx=(x-get_i32(p,q+0x5c))*p->follow_factor/100;
            const int dy=(y-get_i32(p,q+0x60))*p->follow_factor/100;
            put_i32(p,q+0x5c,dx==0&&dy==0?x:get_i32(p,q+0x5c)+dx);
            put_i32(p,q+0x60,dx==0&&dy==0?y:get_i32(p,q+0x60)+dy);
        }
        for(AnmVm* vm:{g_option_vm[i],g_option_max_vm[i]})if(vm) {
            vm->extra_x=get_i32(p,q+0x5c)/128.0f;
            vm->extra_y=get_i32(p,q+0x60)/128.0f;
        }
    }
}

// 通常移动：按键 → 方向、聚焦、速度、位移量（定点）。写入 p->direction / focus_mode。
void normal_move_velocity(Player* p, int32_t& dx, int32_t& dy) {
    const uint32_t held = input_battle_state().held;
    int32_t dir;
    if ((held & 0x50) == 0x50) {
        dir = 5;
    } else if ((held & 0x60) == 0x60) {
        dir = 7;
    } else if ((held & 0x90) == 0x90) {
        dir = 6;
    } else if ((held & 0xa0) == 0xa0) {
        dir = 8;
    } else if ((held & 0x20) == 0) {
        if ((held & 0x10) == 0) {
            dir = (held & 0x40) == 0 ? ((held & 0x80) != 0 ? 4 : 0) : 3;
        } else {
            dir = 1;
        }
    } else {
        dir = 2;
    }
    p->direction = dir;

    // 45b170 requires an enemy and four ticks since the movement reset.
    bool focus = false;
    if (!stage::g_enemy_manager || stage::enemy_count()==0 || p->shoot_cooldown < 4) {
        p->follow_factor = 0x1e;
    } else {
        focus = (held & ACTION_SLOW) != 0;
    }
    p->focus_mode = focus ? 1 : 0;
    if(!focus) {
        if(g_focus_vm) {
            anm_set_label(g_focus_vm,1);
            anm_manage_vm(g_focus_vm,nullptr);
            g_focus_vm=nullptr;
        }
        p->focus_handle=0;
    } else {
        if(p->focus_handle==0) {
            AnmResource* resource=anm_get(8,"effect.anm");
            g_focus_vm=anm_vm_alloc(resource);
            anm_vm_bind_script(g_focus_vm,resource,26);
            g_focus_vm->draw_layer=14;
            g_focus_vm->flags3=(g_focus_vm->flags3&~0x200000u)|0x101000u;
            anm_vm_update(g_focus_vm);p->focus_handle=anm_queue_add(g_focus_vm);
            anm_manage_vm(g_focus_vm,&g_focus_vm);
        }
        if(g_focus_vm) {
            const float scale=(p->flags_4779c&0x10)?(p->invuln_alpha-1.0f)*2.0f+1.0f:1.0f;
            g_focus_vm->scale_extra_x=g_focus_vm->scale_extra_y=scale;
            g_focus_vm->flags2|=8u;
        } else p->focus_handle=0;
    }

    const int32_t speed =
        focus ? (dir < 5 ? p->speed_focus : p->speed_diag_focus)
              : (dir < 5 ? p->speed_normal : p->speed_diag);
    dx = static_cast<int32_t>(
        static_cast<float>(kDirection[dir][0] * speed -
                           static_cast<int32_t>(p->shake_x * -128.0f)) *
        p->move_scale);
    dy = static_cast<int32_t>(
        static_cast<float>(kDirection[dir][1] * speed -
                           static_cast<int32_t>(p->shake_y * -128.0f)) *
        p->move_scale);
}



} // namespace

bool status_add_power(GameStatusBlock* s, int32_t value) {
    // FUN_004573f0：加到上限，返回是否跨过 power_step 的整数边界。
    if (s->power_max <= s->power) {
        return false;
    }
    const int32_t old = s->power;
    s->power = old + value;
    if (s->power_max < s->power) {
        s->power = s->power_max;
    }
    return old / s->power_step != s->power / s->power_step;
}

void player_refresh_power_options() {
    g_option_power=-1;
    player_options_rebuild(g_player);
}
void player_start_warp(int side) {
    g_player->flags_4779c=(g_player->flags_4779c&~0x180u)|(side==1?0x80u:0x100u);
    g_player->script_offset=-1;g_player->script_timer=0;g_player->script_value=0;
}
void player_set_shop_active(bool active) {
    if(active)g_player->flags_4779c|=2u;else g_player->flags_4779c&=~2u;
    const int label=active?3:2;
    for(int i=0;i<4;++i)for(AnmVm* vm:{g_option_vm[i],g_option_max_vm[i]})if(vm)anm_set_label(vm,label);
    for(int i=0;i<12;++i) {
        auto* option=reinterpret_cast<uint8_t*>(g_player)+0xa30+i*0xf0;
        for(int j=0;j<2;++j){AnmVm* vm=*reinterpret_cast<AnmVm**>(option+0xb0+j*4);if(vm)anm_set_label(vm,label);}
    }
    g_player->follow_stun=0;
}

bool status_sub_power(GameStatusBlock* s, int32_t value) {
    // FUN_00457480：Power 下限就是 power_step（原版不会掉到 0）。
    if (s->power <= s->power_step) {
        return false;
    }
    s->power = s->power - value;
    if (s->power < s->power_step) {
        s->power = s->power_step;
    }
    return (s->power + value) / s->power_step != s->power / s->power_step;
}

void status_add_life(GameStatusBlock* s) {
    // FUN_004575f0：残机加一，+0x70 清零，+0x74 计 Extend 次数。
    if (s->lives_max <= s->lives) {
        s->unk_070 = 0;
    }
    s->lives += 1;
    if (s->lives_max < s->lives) {
        s->lives = s->lives_max;
        s->unk_070 = 0;
    }
    if(stage::g_stage)stage::hud_refresh_lives();
    sound_play_se(0x11);
    if(stage::g_stage)stage::hud_show_extend();
    s->unk_074 += 1;
}

void status_add_life_fragment(GameStatusBlock* s) {
    if(s->lives>=s->lives_max){s->unk_070=0;return;}
    constexpr int normal[]={3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,3,99999999};
    constexpr int extra[]={3,3,3,3,3,3,3,3,3,99999999};
    const int threshold=stage::g_difficulty==4?extra[s->unk_074]:normal[s->unk_074];
    ++s->unk_070;
    while(s->unk_070>=threshold){s->unk_070-=threshold;status_add_life(s);}
    if(stage::g_stage)stage::hud_refresh_lives();
}
void status_add_bomb(GameStatusBlock* s) {
    ++s->unk_07c;
    if(s->unk_07c>=s->unk_088)s->unk_080=0;
    if(s->unk_07c>s->unk_088)s->unk_07c=s->unk_088;
    else sound_play_se(46);
    if(stage::g_stage)stage::hud_refresh_bombs();
}
void status_add_bomb_fragment(GameStatusBlock* s) {
    if(s->unk_07c>=s->unk_088){s->unk_080=0;return;}
    if(++s->unk_080>2){s->unk_080=0;status_add_bomb(s);}
    if(stage::g_stage)stage::hud_refresh_bombs();
}
void player_reset(Player* p) {
    // FUN_0045a600：把 4+12 个子机对象的 +0xD4 置 1（下一帧贴到自机位置）。
    put_i32(p, 0x744, 1);
    put_i32(p, 0x834, 1);
    put_i32(p, 0x924, 1);
    put_i32(p, 0xa14, 1);
    for (int32_t i = 0; i < 12; ++i) {
        put_i32(p, 0xb04 + i * 0xf0, 1);
    }
}

void player_shot_area_init(Player* p) {
    // FUN_0045a690，原版参数是 Player+0x620，这里换算成 Player 偏移。
    uint8_t* q = reinterpret_cast<uint8_t*>(p) + 0x620;
    // 4 个 0xF0 子机对象（Player+0x670 起）
    *reinterpret_cast<uint32_t*>(q + 0x24) &= ~1u;
    *reinterpret_cast<uint32_t*>(q + 0x38) &= ~1u;
    *reinterpret_cast<uint32_t*>(q + 0x4c) &= ~1u;
    for (int32_t i = 0; i < 4; ++i) {
        uint32_t* pu = reinterpret_cast<uint32_t*>(q + 0x118 + i * 0xf0);
        pu[0] &= ~1u;
        pu[-6] = 0;
        pu[-5] = 0;
    }
    // 12 个 0xF0 子机对象（Player+0xA30 起）
    for (int32_t i = 0; i < 12; ++i) {
        uint32_t* pu = reinterpret_cast<uint32_t*>(q + 0x4d8 + i * 0xf0);
        pu[0] &= ~1u;
        pu[-6] = 0;
        pu[-5] = 0;
    }
    // 0x200 个攻击对象（stride 0xF8）
    for (int32_t i = 0; i < 0x200; ++i) {
        uint32_t* pu = reinterpret_cast<uint32_t*>(q + 0xf6c + i * 0xf8);
        pu[0] &= ~1u;
        pu[5] &= ~1u;
        pu[10] &= ~1u;
        pu[-5] = 0;
    }
    // 0x401 个子弹槽（stride 0x9C）
    for (int32_t i = 0; i < 0x401; ++i) {
        *reinterpret_cast<uint32_t*>(q + 0x1ffc4 + i * 0x9c) &= ~1u;
    }
    auto clear = [q](int32_t player_offset) {
        *reinterpret_cast<uint32_t*>(q + player_offset - 0x620) &= ~1u;
    };
    clear(0x47060);
    clear(0x470a8);
    clear(0x470c0);
    clear(0x470d4);
    clear(0x47164);
    clear(0x47178);
    clear(0x47190);
    clear(0x47304);
    put_i32(p, 0x47090, 0);
    put_i32(p, 0x47094, 0);
}

int32_t player_init_from_sht(Player* p) {
    // 行 35–51：FUN_00486880(9, plXX.anm) 写 +0xC/+0x10；行 74–84 初始化 +0x14 的自机 ANM VM。
    AnmResource* anm = anm_get(40 + g_player_character, kPlayerAnmNames[g_player_character]);
    p->anm_player = static_cast<int32_t>(reinterpret_cast<uintptr_t>(anm));
    p->anm_player2 = p->anm_player;
    AnmVm* vm = player_anm_vm(p);
    anm_vm_bind_script(vm, anm, 0);
    anm_vm_update(vm);

    uint8_t* sht = static_cast<uint8_t*>(
        file_read(kPlayerShtNames[g_player_shot_type + g_player_character], nullptr, false));
    if (sht == nullptr) {
        return -1;
    }
    p->sht = sht;

    // 子程序表：sht+0xE0 每 4 字节一项，值 = 相对偏移 + 0x180，改写成绝对地址。
    const uint16_t sub_count = *reinterpret_cast<uint16_t*>(sht + 2);
    for (uint16_t i = 0; i < sub_count; ++i) {
        uint32_t* slot = reinterpret_cast<uint32_t*>(sht + 0xe0 + i * 4);
        *slot = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(sht) + *slot + 0x180);
    }
    // 原版此处把每条指令 +0x2C/+0x30/+0x34/+0x38 的处理函数索引换成
    // 0x4B4230/0x4B4210/0x4CF414/0x4B41F0 四张原版代码地址表；重建版 SHT VM
    // 落地时换成重建版处理函数表，见 player_sht.h。

    // SHT 字节 0x10..0x1C 的四个 float 是移动速度，×128 写成定点。
    for (int32_t off = 0x10; off < 0x20; off += 4) {
        put_i32(p, 0x477a4 + off,
                static_cast<int32_t>(*reinterpret_cast<float*>(sht + off) * kSpeedUnit));
    }
    *reinterpret_cast<int32_t*>(sht + 0x24) = 100;
    g_status.power_max =
        *reinterpret_cast<int32_t*>(sht + 0x24) * *reinterpret_cast<int32_t*>(sht + 0x20);
    g_status.power_step = *reinterpret_cast<int32_t*>(sht + 0x24);

    // 计时器初值（FUN_0045a7a0 后半段逐段）。
    put_i32(p, 0x476dc, 0);
    put_i32(p, 0x476e0, get_i32(p, 0x476e0) | 1);
    put_i32(p, 0x476d4, -1);
    put_i32(p, 0x476d8, -1082130432); // -1.0f
    put_i32(p, 0x476d0, -2);
    put_i32(p, 0x476f0, 0);
    put_i32(p, 0x476f4, get_i32(p, 0x476f4) | 1);
    put_i32(p, 0x476e8, -1);
    put_i32(p, 0x476ec, -1082130432);
    put_i32(p, 0x476e4, -2);
    put_i32(p, 0x640, 0);
    put_i32(p, 0x644, get_i32(p, 0x644) | 1);
    put_i32(p, 0x638, 0);
    put_i32(p, 0x63c, 0);
    put_i32(p, 0x634, -1);
    put_i32(p, 0x668, 0);
    put_i32(p, 0x66c, get_i32(p, 0x66c) | 1);
    put_i32(p, 0x660, 0);
    put_i32(p, 0x664, 0);
    put_i32(p, 0x65c, -1);
    put_i32(p, 0x47780, 0);
    put_i32(p, 0x47784, get_i32(p, 0x47784) | 1);
    put_i32(p, 0x47778, 0);
    put_i32(p, 0x4777c, 0);
    put_i32(p, 0x47774, -1);
    put_i32(p, 0x47794, 0);
    put_i32(p, 0x47798, get_i32(p, 0x47798) | 1);
    put_i32(p, 0x4778c, 0);
    put_i32(p, 0x47790, 0);
    put_i32(p, 0x47788, -1);
    put_i32(p, 0x477e4, 0x1e);
    put_i32(p, 0x476f8, 0);
    put_i32(p, 0x477ec, 0x3f800000); // 1.0f
    // 子机对象的 +0x60 固定偏移 = -400 像素（定点 -400*128）。
    put_i32(p, 0x6d0, -51200); // 0xffff3800
    put_i32(p, 0x7c0, -51200);
    put_i32(p, 0x8b0, -51200);
    put_i32(p, 0x9a0, -51200);
    for (int32_t i = 0; i < 12; ++i) {
        put_i32(p, 0xa90 + i * 0xf0, -51200);
    }
    put_i32(p, 0x4779c, get_i32(p, 0x4779c) & ~4);
    put_i32(p, 0x47974, 0);
    put_i32(p, 0x4797c, 0x3f800000); // 1.0f
    put_f32(p,0x47980,1.0f); // 45adb8：Enemy 42ed40 消费的本帧伤害倍率
    // 0x400 个攻击/子弹槽的序号表（stride 0xF8）。
    for (int32_t i = 0; i < 0x400; ++i) {
        put_i32(p, 0x1574 + i * 0xf8, i);
    }
    // 45a7a0 的构造位置；正式开局由 45abb0 切到正常状态，复活另走 state 0。
    p->x_sub = 0;
    p->y_sub = 0xc800;
    p->x = 0.0f;
    p->y = 400.0f;
    p->state = 0;
    p->shot_angle=-1.57079637f;
    p->shot_offset=0;
    p->respawn_wait=8;
    put_f32(p,0x47988,5);put_f32(p,0x4798c,30);
    put_f32(p,0x47990,70);put_f32(p,0x47994,70);put_f32(p,0x47998,128);
    player_shot_area_init(p);
    player_reset(p);
    for (int i = 0; i < 33; ++i) { p->trail[i][0] = p->x_sub; p->trail[i][1] = p->y_sub; }
    return 0;
}

Player* player_create() {
    // FUN_0045af20：分配后整个对象清零，再载 SHT。
    Player* p = new Player{};
    g_player = p;
    if (player_init_from_sht(p) != 0) {
        player_destroy(p);
        return nullptr;
    }
    return p;
}

void player_destroy(Player* p) {
    // FUN_0045b020
    if (p->thread_logic != nullptr) {
        thread_remove(static_cast<Thread*>(p->thread_logic));
    }
    if (p->thread_render != nullptr) {
        thread_remove(static_cast<Thread*>(p->thread_render));
    }
    for(int i=0;i<4;++i){if(g_option_vm[i])anm_vm_free(g_option_vm[i]);if(g_option_max_vm[i])anm_vm_free(g_option_max_vm[i]);g_option_vm[i]=g_option_max_vm[i]=nullptr;}
    g_option_power=-1;
    if(g_focus_vm)anm_vm_free(g_focus_vm);
    g_player = nullptr;
    // 攻击对象各自的子弹 ANM VM 由 anm_vm_alloc 分配，销毁前归还。
    AttackObject* pool = player_attack_pool(p);
    for (int32_t i = 0; i < kAttackObjectCount; ++i) {
        if (pool[i].anm_vm != nullptr) {
            anm_vm_free(pool[i].anm_vm);
        }
    }
    std::free(p->sht);
    delete p;
}

void player_shot_update(Player* p) {
    // FUN_0045b170
    int32_t dx = 0;
    int32_t dy = 0;
    const int previous_dx=p->unk_47934;

    if ((p->flags_4779c & kFlagScriptX) == 0) {
        normal_move_velocity(p, dx, dy);
    } else {
        // 45b170: border warp advances its timer before the 1/8/16 events.
        const bool left = (p->flags_4779c & kFlagScriptX) == 0x80;
        if(p->script_timer==0)player_set_shop_active(true);
        put_i32(p,0x477ac,0);
        p->script_offset=p->script_timer;
        const float speed=stage::g_speed_scale;
        const bool unit=speed>0.99f && speed<1.01f;
        p->script_value+=unit?1.0f:speed;
        p->script_timer=unit?p->script_timer+1:int32_t(p->script_value);
        if(p->script_timer==1)sound_play_se(left?81:80);
        int32_t vx = left ? -0x100 : 0x100;
        if (p->script_timer == 8) {
            vx = left ? 0xc000 : -0xc000;
            player_reset(p);
            for(auto& point:p->trail){point[0]=p->x_sub;point[1]=p->y_sub;}
        }
        if (p->script_timer > 0xf) {
            player_set_shop_active(false);p->flags_4779c&=~0x180u;
            p->script_offset=-1;p->script_timer=0;p->script_value=0;
            normal_move_velocity(p, dx, dy);
        } else {
            dx = vx;
            dy = 0;
        }
    }

    int script=-1;
    if(dx<0&&previous_dx>=0)script=1;
    else if(dx>0&&previous_dx<=0)script=3;
    else if(dx==0&&previous_dx<0)script=2;
    else if(dx==0&&previous_dx>0)script=4;
    if(script>=0)anm_vm_set_script(player_anm_vm(p),player_anm(p),script);
    // 保存方向/速度，按慢动作系数换成浮点速度；方向非 0 时把当前速度存成上一帧。
    p->unk_47934 = dx;
    p->unk_47938 = dy;
    p->vel_x = static_cast<float>(dx) * stage::g_speed_scale;
    p->vel_y = static_cast<float>(dy) * stage::g_speed_scale;
    if (p->direction != 0) {
        p->vel_x_prev = p->vel_x;
        p->vel_y_prev = p->vel_y;
    }
    const int32_t step_x = static_cast<int32_t>(p->vel_x);
    const int32_t step_y = static_cast<int32_t>(p->vel_y);
    p->vel_x_int_dc = step_x;
    p->vel_y_int_e0 = step_y;
    int32_t nx = p->x_sub + step_x;
    p->x_sub = nx;
    int32_t ny = p->y_sub + step_y;
    p->y_sub = ny;

    if ((p->flags_4779c & kFlagScriptX) == 0) {
        if (nx < -kBoundX) {
            p->x_sub = -kBoundX;
            nx = -kBoundX;
        } else if (nx > kBoundX) {
            p->x_sub = kBoundX;
            nx = kBoundX;
        }
        if (ny < kMinY) {
            p->y_sub = kMinY;
            ny = kMinY;
        } else if (ny > kMaxY) {
            p->y_sub = kMaxY;
            ny = kMaxY;
        }
    }
    p->x = static_cast<float>(nx) * kSubPixel;
    p->y = static_cast<float>(ny) * kSubPixel;
    if(g_focus_vm) {
        g_focus_vm->extra_x=p->x;g_focus_vm->extra_y=p->y;g_focus_vm->extra_z=p->z;
    } else p->focus_handle=0;

    // 位置历史：移动时整表后移一格，第 0 格写当前位置。
    if (dx != 0 || dy != 0) {
        for (int32_t i = 0x20; i > 0; --i) {
            p->trail[i][0] = p->trail[i - 1][0];
            p->trail[i][1] = p->trail[i - 1][1];
        }
    }
    p->trail[0][0] = p->x_sub;
    p->trail[0][1] = p->y_sub;
    if ((p->flags_4779c & kFlagDeath) != 0) {
        p->follow_stun += 1;
    }
    player_options_update(p);
    card_manager_on_player_move();
    card_manager_player_update();
    // 原版随后调 FUN_0045bc90 更新 4+12 个子机对象（需要 ANM 句柄），
    // 以及拖尾/受击表现句柄；等 ANM 运行时接入后补。
    if (p->follow_stun > 0x1d) {
        put_i32(p, 0x476f8, 0);
    }
}

namespace {

inline PlayerBullet* player_bullet_pool(Player* p) {
    return reinterpret_cast<PlayerBullet*>(reinterpret_cast<uint8_t*>(p) + kPlayerBulletPoolOffset);
}

// 45f0f0：局部 x 轴沿子弹方向，+0x14 为长度，+0x18 为厚度。
bool circle_vs_obb(const float* circle, float radius, float angle, const float* obb,
                   float half_w, float half_h) {
    const float dx = circle[0] - obb[0];
    const float dy = circle[1] - obb[1];
    const float c = std::cos(angle);
    const float s = std::sin(angle);
    const float lx = dx * c + dy * s;
    const float ly = dx * s - dy * c;
    if ((std::fabs(lx) <= half_w + radius && std::fabs(ly) <= half_h) ||
        (std::fabs(lx) <= half_w && std::fabs(ly) <= half_h + radius)) return true;
    const float x0 = lx - half_w, x1 = lx + half_w;
    const float y0 = ly - half_h, y1 = ly + half_h;
    const float r2 = radius * radius;
    return x0*x0 + y0*y0 < r2 || x1*x1 + y0*y0 < r2 ||
           x0*x0 + y1*y1 < r2 || x1*x1 + y1*y1 < r2;
}

// 两个旋转 OBB（局部 x 轴半长 hw，局部 y 轴半长 hh）的 SAT 相交。
bool obb_vs_obb(const float* ap, float aa, float ahw, float ahh,
                const float* bp, float ba, float bhw, float bhh) {
    const float dx = bp[0] - ap[0];
    const float dy = bp[1] - ap[1];
    const float ac = std::cos(aa);
    const float as = std::sin(aa);
    const float bc = std::cos(ba);
    const float bs = std::sin(ba);
    const float axes[4][2] = {{ac, as}, {-as, ac}, {bc, bs}, {-bs, bc}};
    for (int32_t i = 0; i < 4; ++i) {
        const float ax = axes[i][0];
        const float ay = axes[i][1];
        const float proj_d = std::fabs(dx * ax + dy * ay);
        const float proj_a = ahw * std::fabs(ac * ax + as * ay) +
                             ahh * std::fabs(-as * ax + ac * ay);
        const float proj_b = bhw * std::fabs(bc * ax + bs * ay) +
                             bhh * std::fabs(-bs * ax + bc * ay);
        if (proj_a + proj_b < proj_d) {
            return false;
        }
    }
    return true;
}

// 4038a0 / 404080：激光轴与敌机矩形边的交点。
bool player_laser_edge(float ax, float ay, float bx, float by,
                       float cx, float cy, float dx, float dy, float* point) {
    const float side_c = (cy-ay)*(ax-bx) + (ax-cx)*(ay-by);
    const float side_d = (ax-dx)*(ay-by) + (dy-ay)*(ax-bx);
    if (side_d * side_c > 0) return false;
    if (side_c == 0 && side_d == 0) {
        float left=ax, bottom=ay, right=bx, top=by;
        if (bx<ax) { left=bx; bottom=by; right=ax; top=ay; }
        float edge_left=cx, edge_bottom=cy, edge_right=dx, edge_top=dy;
        if (dx<cx) { edge_left=dx; edge_bottom=dy; edge_right=cx; edge_top=cy; }
        if (edge_right<left || edge_top<bottom || right<edge_left || top<edge_bottom) return false;
    } else if (((ay-cy)*(cx-dx)+(cx-ax)*(cy-dy))*
               ((by-cy)*(cx-dx)+(cx-bx)*(cy-dy)) > 0) return false;
    const bool axis_sloped = std::fabs(bx-ax) >= 0.01f;
    const bool edge_sloped = std::fabs(dx-cx) >= 0.01f;
    const float axis_slope = axis_sloped ? (by-ay)/(bx-ax) : 0;
    const float axis_intercept = axis_sloped ? ay-((by-ay)*ax)/(bx-ax) : ax;
    const float edge_slope = edge_sloped ? (dy-cy)/(dx-cx) : 0;
    const float edge_intercept = edge_sloped ? cy-((dy-cy)*cx)/(dx-cx) : cx;
    if (axis_sloped) {
        if (edge_sloped) {
            point[0] = (edge_intercept-axis_intercept)/(axis_slope-edge_slope);
            point[1] = ((edge_intercept-axis_intercept)*axis_slope)/(axis_slope-edge_slope)+axis_intercept;
        } else { point[0]=cx; point[1]=axis_slope*cx+axis_intercept; }
        return true;
    }
    if (edge_sloped) { point[0]=ax; point[1]=edge_slope*ax+edge_intercept; return true; }
    if (std::fabs(ax-cx)<0.001f) { point[0]=ax; point[1]=ay; return true; }
    return false;
}

// 460320：命中裁短、label 2、每两帧火花、每四帧返回一次伤害。
int32_t player_laser_hit(AttackObject& a, PlayerBullet& b, const float* pos,
                         const float* size, float radius, float enemy_angle) {
    a.laser_hit = 1;
    if (a.laser_hit_anm == 0) {
        anm_set_label(a.anm_vm, 2);
        a.laser_hit_anm = 1;
    }
    const float c = std::cos(a.motion.angle), s = std::sin(a.motion.angle);
    const float dx = pos[0]-a.motion.x, dy = pos[1]-a.motion.y;
    if (size == nullptr) {
        const float along = dx*c + dy*s;
        const float across = dy*c - dx*s;
        const float r = radius + b.half_height*0.5f;
        const float ratio = across/r;
        a.half_width = std::fmax(0.0f, along - float(std::sqrt(double(1.0f-ratio*ratio)))*r + 8.0f);
        b.half_width = a.half_width;
    } else {
        const float hw = (size[0]+b.half_height)*0.5f;
        const float hh = (size[1]+b.half_height)*0.5f;
        float corners[4][2] = {{-hw,-hh},{-hw,hh},{hw,hh},{hw,-hh}};
        const float ec = std::cos(enemy_angle), es = std::sin(enemy_angle);
        for (auto& corner : corners) {
            const float x = corner[0], y = corner[1];
            if (a.motion.angle != 0.0f) {
                corner[0] = x*ec-y*es;
                corner[1] = y*ec+x*es;
            }
            corner[0] += pos[0]; corner[1] += pos[1];
        }
        const float ax=a.motion.x-c*1000, ay=a.motion.y-s*1000;
        const float bx=a.motion.x+c*1000, by=a.motion.y+s*1000;
        float points[2][2]; int count=0;
        for (int i=0; i<4 && count<2; ++i) {
            const auto& from=corners[i]; const auto& to=corners[(i+1)%4];
            if (player_laser_edge(ax,ay,bx,by,from[0],from[1],to[0],to[1],points[count])) ++count;
        }
        if (count == 0) {
            a.half_width = std::fmax(0.0f, float(std::sqrt(double(dx*dx+dy*dy)))-24.0f);
            b.half_width = a.half_width+16.0f;
        } else {
            int near=0;
            if (count==2 && (ax-points[1][0])*(ax-points[1][0])+(ay-points[1][1])*(ay-points[1][1]) <=
                            (ax-points[0][0])*(ax-points[0][0])+(ay-points[0][1])*(ay-points[0][1])) near=1;
            const float x=points[near][0]-a.motion.x, y=points[near][1]-a.motion.y;
            const float angle=std::remainder(std::atan2(y,x)-a.motion.angle,6.28318530718f);
            a.half_width = std::fabs(angle)>=1.57079637f ? 8.0f : float(std::sqrt(double(x*x+y*y)))+8.0f;
            b.half_width = a.half_width;
        }
    }
    const int frame = static_cast<int32_t>(a.life_i);
    if (frame == a.life_prev) return 0;
    if ((frame & 1) == 0) {
        const auto* sht = static_cast<const PlayerSht*>(g_player->sht);
        const auto* instruction = reinterpret_cast<const ShtInstruction*>(sht->sub_offset[a.command>>8])+(a.command&255);
        const int script = *reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(instruction)+0x26);
        auto* resource = static_cast<AnmResource*>(a.anm_manager);
        AnmVm* spark = anm_vm_alloc(resource);
        anm_vm_set_script(spark, resource, script);
        spark->extra_x = a.motion.x+c*a.half_width;
        spark->extra_y = a.motion.y+s*a.half_width;
        spark->extra_z = 0;
        spark->rot_z = a.motion.angle;
        spark->flags2 |= 4u;
        auto* block = reinterpret_cast<uint8_t*>(spark)+0x90;
        std::memset(block,0,0x58);
        auto* values = reinterpret_cast<float*>(block);
        auto* fields = reinterpret_cast<int32_t*>(block);
        values[3]=c*64.0f; values[4]=s*64.0f;
        fields[15]=-1; fields[19]=1; fields[20]=20; fields[21]=4;
        anm_manage_vm(spark,nullptr);
        anm_queue_add(spark);
    }
    return (frame & 3) == 0 ? b.damage : 0;
}

// 自机子弹伤害对分数的影响（原版 DAT_004cccfc，0x45f0f0 行 201–209）。
int32_t g_damage_score = 0;

} // namespace

int32_t player_bullet_count() {
    return kPlayerBulletCapacity;
}

const PlayerBullet* player_bullet_at(int32_t index) {
    return player_bullet_pool(g_player) + index;
}

int32_t player_bullet_hit_test(const float* pos, const float* size, float radius,
                               uint32_t* out_hit, float* out_pos, int32_t mode,
                               float enemy_id, float enemy_angle) {
    // FUN_0045f0f0：计时器本帧未推进（hitstop）时不判定，且不改 out_hit（原版行 34）。
    Player* p = g_player;
    if (p->timer_state == p->unk_634) {
        return 0;
    }
    int32_t total = 0;
    int32_t grouped_damage[5]={};
    PlayerBullet* bullets = player_bullet_pool(p);
    for (int32_t i = 0; i < kPlayerBulletCapacity; ++i) {
        PlayerBullet& b = bullets[i];
        if ((b.flags & 1u) == 0) {
            continue;
        }
        // 行 62–64：命中窗口 = 寿命相对上一帧推进且在命中间隔上、冷却已过。
        if (b.life == b.life_prev || b.hit_interval == 0 || b.life % b.hit_interval != 0 ||
            b.hit_cooldown >= 1) {
            continue;
        }
        const float* bpos = reinterpret_cast<const float*>(b.motion);
        bool hit;
        if ((b.flags & 2u) != 0) {
            // 激光/圆：子弹圆心 + hit_radius（行 151–175）。
            if (size == nullptr) {
                const float dx = bpos[0] - pos[0];
                const float dy = bpos[1] - pos[1];
                const float r = b.hit_radius + radius;
                hit = dx * dx + dy * dy <= r * r;
            } else {
                hit = circle_vs_obb(bpos, b.hit_radius, enemy_angle, pos,
                                    size[0] * 0.5f, size[1] * 0.5f);
            }
        } else if (size == nullptr) {
            // 圆 vs 子弹 OBB（行 65–92）。
            hit = circle_vs_obb(pos, radius, b.angle, bpos, b.half_width * 0.5f,
                                b.half_height * 0.5f);
        } else {
            // OBB vs OBB（行 95–148）。
            hit = obb_vs_obb(bpos, b.angle, b.half_width * 0.5f, b.half_height * 0.5f,
                             pos, enemy_angle, size[0] * 0.5f, size[1] * 0.5f);
        }
        if (!hit) {
            continue;
        }
        if(b.hit_type>=1 && b.hit_type<=4) {
            if(b.damage<grouped_damage[b.hit_type])continue;
            total-=grouped_damage[b.hit_type];
        }
        if (enemy_id != 0.0f) {
            if (static_cast<float>(b.last_hit_enemy) == enemy_id) {
                continue;
            }
            b.last_hit_enemy = static_cast<int32_t>(enemy_id);
        }
        if (out_hit != nullptr && (b.flags & 4u) != 0) {
            *out_hit = 1;
        }
        int32_t damage = b.damage;
        // 45f6a0 按攻击对象的命中处理器返回本次伤害。
        if (mode == 0) {
            b.damage_accum += damage;
            if(b.kind==1) {
                AttackObject& a=player_attack_pool(p)[b.attack_id];
                if(a.handler_d==0x40c4f0)card_passive_shot_hit(&a);
                else if(a.handler_d==2)damage=player_laser_hit(a,b,pos,size,radius,enemy_angle);
                else {
                    if(a.handler_d==1) {
                        // 45f9d0 consumes 4cf280 before creation, then RGBA after
                        // effect149's initial ANM tick, and finally calls 45f6f0.
                        float angle=a.motion.angle+(float(rng_next(card_rng()))*4.6566129e-10f-1.0f)*0.349065840f;
                        for(int turn=0;turn<2;++turn) {
                            while(angle>3.141592741f)angle-=6.283185482f;
                            while(angle< -3.141592741f)angle+=6.283185482f;
                            if(turn==0)angle+=3.141592741f;
                        }
                        AnmResource* resource=anm_get(8,"effect.anm");
                        AnmVm* effect=anm_vm_alloc(resource);
                        anm_vm_bind_script(effect,resource,149);effect->flags3|=0x1000u;
                        effect->extra_x=a.motion.x;effect->extra_y=a.motion.y;effect->extra_z=a.motion.z;
                        effect->rot_z=angle;anm_vm_update(effect);anm_queue_add(effect);
                        auto* rgba=reinterpret_cast<uint8_t*>(&effect->color);
                        rgba[2]=uint8_t((rng_next(card_rng())&127)+127);
                        rgba[1]=uint8_t((rng_next(card_rng())&63)+64);
                        rgba[0]=uint8_t((rng_next(card_rng())&63)+64);
                        rgba[3]=uint8_t((rng_next(card_rng())&63)+96);
                        anm_manage_vm(effect,nullptr);
                    }
                    if(a.handler_d==3 || a.handler_d==4) {
                        const auto* sht=static_cast<const PlayerSht*>(p->sht);
                        const auto* instruction=reinterpret_cast<const ShtInstruction*>(sht->sub_offset[a.command>>8])+(a.command&255);
                        const int index=player_bullet_alloc(&a.motion.x,0,a.handler_d==3?10:20,instruction->damage);
                        PlayerBullet& splash=bullets[index-1];splash.flags=3;splash.hit_radius=24;
                        splash.unk_008=a.handler_d==3?1:*reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(instruction)+0x4c);
                        splash.hit_interval=4;
                        if(a.handler_d==3){auto* motion=reinterpret_cast<Motion*>(splash.motion);motion->speed=0.3f;motion->angle=-1.57079637f;}
                    }
                    a.active=2;
                    if(a.handler_d==3) {
                        a.motion.speed=0.3f;
                        anm_set_label(a.anm_vm,1);
                        sound_play_se_at(65,a.motion.x);
                    } else {
                        a.motion.z=0.1f;a.motion.speed*=0.125f;
                        anm_set_label(a.anm_vm,1);
                        a.anm_vm->extra_x=a.motion.x;a.anm_vm->extra_y=a.motion.y;a.anm_vm->extra_z=a.motion.z;
                    }
                    b.flags&=~1u;a.bullet_index=0;
                }
            } else if (b.kind == 3) {
                bomb_marisa_hit(pos); // Native returns -1: retain the hitbox's base damage.
            }
        }
        if(b.hit_type>=1 && b.hit_type<=4)grouped_damage[b.hit_type]=damage;
        total += damage;
        if (out_pos != nullptr) {
            out_pos[0] = bpos[0];
            out_pos[1] = bpos[1];
            out_pos[2] = bpos[2];
        }
        // 行 196–199：贯穿累计超过上限则消弹。
        if (b.damage_max < 9999999 && b.damage_accum >= b.damage_max) {
            b.flags &= ~1u;
            b.damage = 0;
        }
    }
    // 行 201–213：伤害受 SHT[0x28] 上限夹住，并累加分数计数器。
    const int32_t cap = *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(p)+0x47984);
    if (total > cap) {
        total = cap;
    }
    if (mode == 0 && total != 0) {
        g_damage_score += (total / 10 + 10) / 10;
        session().score += (total / 10 + 10) / 10;
        if (g_damage_score > 999999999) {
            g_damage_score = 999999999;
        }
    }
    return total;
}

namespace {

constexpr float kTwoPiF = 6.28318548f;
constexpr float kPiF = 3.14159265f;

int32_t& player_bullet_cursor(Player* p) {
    return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + 0x20570);
}

// 0x45DFA0 的池分配部分：返回 1 起的槽序号。
int32_t player_bullet_alloc(const float* pos, float angle, int32_t life, int32_t damage) {
    Player* p = g_player;
    PlayerBullet* pool = player_bullet_pool(p);
    int32_t index = 0;
    int32_t cursor = player_bullet_cursor(p);
    for (int32_t attempt = 0; attempt < kPlayerBulletCapacity; ++attempt) {
        const int32_t next = (cursor + 1 < kPlayerBulletCapacity) ? cursor + 1 : 0;
        if ((pool[next].flags & 1u) == 0) {
            index = next;
            break;
        }
        cursor = next;
    }
    PlayerBullet* b = &pool[index];
    b->flags = 1u;
    b->hit_radius=0;b->unk_008=0;
    std::memset(b->motion, 0, sizeof(b->motion));
    std::memcpy(b->motion, pos, 3 * sizeof(float));
    while (angle > kPiF) {
        angle -= kTwoPiF;
    }
    while (angle < -kPiF) {
        angle += kTwoPiF;
    }
    b->angle = angle;
    b->unk_010 = 0.0f;
    b->life = life;
    b->life_prev = life - 1;
    b->life_f = static_cast<float>(life);
    b->damage = damage;
    b->damage_accum = 0;
    b->damage_max = 9999999;
    b->hit_interval = 1;
    b->last_hit_enemy = 0;
    b->hit_cooldown = 0;
    b->hit_type = 0;
    b->attack_id = 0;
    b->kind = 0;
    player_bullet_cursor(p) = index;
    return index + 1;
}

// 0x45A550：把第一射击计时器复位到指定值。
void player_script_a_start(Player* p, int32_t start) {
    if ((p->flags_476e0 & 1u) == 0) {
        p->unk_476dc = 0;
        p->flags_476e0 |= 1;
    }
    p->script_a_timer = start;
    p->script_a = start - 1;
    p->script_a_value = static_cast<float>(start);
}

// 0x405990：计时器推进（双字段：索引/计时 + 浮点累计）。
void player_timer_advance(int32_t* index_field, int32_t* timer_field, float* value_field) {
    *index_field = *timer_field;
    index_field[3]=0;
    const float speed=stage::g_speed_scale;
    if(speed>0.99f && speed<1.01f){++*timer_field;*value_field+=1.0f;}
    else {*value_field+=speed;*timer_field=int32_t(*value_field);}
}

// 0x452BF0：按速度从计时器减去周期，供主/副射击计时器循环。
void player_timer_subtract(int32_t* index_field, int32_t* timer_field, float* value_field,
                                  int32_t amount) {
    *index_field = *timer_field;
    index_field[3]=0;
    const float speed=stage::g_speed_scale;
    *value_field-=float(amount)*(speed>0.99f && speed<1.01f?1.0f:speed);
    *timer_field=int32_t(*value_field);
}

// 0x45EA00 触发一条指令：按当前子程序选表并生成攻击对象。
void player_fire_instruction(Player* p, const PlayerSht* sht, int32_t script, uint32_t index,
                             int32_t timer_a) {
    const ShtInstruction* instr = reinterpret_cast<const ShtInstruction*>(
        reinterpret_cast<const uint8_t*>(sht->sub_offset[script]) +
        static_cast<intptr_t>(index) * kShtInstructionSize);
    void* option = nullptr;
    if ((instr->option_index&15) != 0)
        option=reinterpret_cast<uint8_t*>(p)+0x580+(instr->option_index&15)*0xf0;
    player_attack_spawn(p, instr, script << 8 | static_cast<int32_t>(index),
                        static_cast<float>(timer_a), &p->x, option);
}

} // namespace

PlayerBullet* player_damage_circle(const float*pos,float radius,float radius_per_frame,int life,int damage) {
    const int index=player_bullet_alloc(pos,0,life,damage);
    PlayerBullet* b=player_bullet_pool(g_player)+index-1;
    b->flags=3;b->hit_radius=radius;b->unk_008=radius_per_frame;
    return b;
}

PlayerBullet* player_damage_rectangle(const float*pos,float angle,float width,float height,int life,int damage) {
    const int index=player_bullet_alloc(pos,angle,life,damage);
    PlayerBullet* b=player_bullet_pool(g_player)+index-1;
    b->half_width=width;b->half_height=height;
    return b;
}

void player_motion_init(Motion* m) {
    // FUN_00402BF0 case 0（玩家子弹/攻击对象默认类型）：速度向量 = 角度方向 × 速度 × 慢动作。
    const float v = m->speed * stage::g_speed_scale;
    m->vel_x = std::cos(m->angle) * v;
    m->vel_y = std::sin(m->angle) * v;
}

void player_motion_step(Motion* m) {
    // FUN_00402CC0 case 0：坐标 += 每帧位移。
    m->x += m->vel_x;
    m->y += m->vel_y;
    m->z += m->unk_03c;
    // 402cc0 -> 404b40 -> floor: quantize after every motion step.
    m->x=std::floor(m->x*100.0f)/100.0f;
    m->y=std::floor(m->y*100.0f)/100.0f;
}

AttackObject* player_attack_pool(Player* p) {
    return reinterpret_cast<AttackObject*>(reinterpret_cast<uint8_t*>(p) + kAttackObjectOffset);
}

int32_t player_attack_spawn(Player* p, const ShtInstruction* instr, int32_t command,
                            float timer_a, const float* base_pos, void* option,
                            AnmResource* resource) {
    const int group=instr->option_index<16?instr->option_index:instr->option_index>>4;
    if(instr->kind==2 && get_i32(p,0x476fc+group*4)!=0)return 0;
    // FUN_0045E930 的空槽扫描（原版展开 4 槽一组，等价于找 active == 0）。
    AttackObject* pool = player_attack_pool(p);
    int32_t slot = -1;
    for (int32_t i = 0; i < kAttackObjectCount; ++i) {
        if (pool[i].active == 0) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return 0;
    }
    AttackObject* a = &pool[slot];
    a->active = 1;
    a->flags |= 1u;
    a->flags_018 |= 1u;
    a->life_prev = -1;
    a->life = 0.0f;
    a->life_i = 0.0f;
    a->id = slot;
    a->command = static_cast<uint32_t>(command);
    a->damage = instr->damage;
    a->half_width = instr->unk_00c;
    a->half_height = instr->unk_010;
    a->param = uint32_t(reinterpret_cast<uintptr_t>(option));
    a->handler_fire=instr->handler_a;a->handler_update=instr->handler_b;
    a->handler_c=instr->handler_c;a->handler_d=instr->handler_d;
    a->unk_090=0;
    a->laser_hit=0;
    a->laser_hit_anm=0;
    if(instr->kind==2)put_i32(p,0x476fc+group*4,g_status.power/g_status.power_step);
    a->anm_handle = 0;
    a->bullet_index = 0;
    // 子机序号 0：直接用自机位置；非 0 需要子机系统（0x45E320 行 120–131）。
    a->motion.x = base_pos[0];
    a->motion.y = base_pos[1];
    a->motion.z = base_pos[2];
    const int initial_option=instr->option_index&15;
    if(initial_option!=0) {
        auto* q=reinterpret_cast<uint8_t*>(p)+0x580+initial_option*0xf0;
        a->motion.x=*reinterpret_cast<int32_t*>(q+0x5c)/128.0f;
        a->motion.y=*reinterpret_cast<int32_t*>(q+0x60)/128.0f;
        a->motion.z=0;
    }
    a->motion.speed = instr->speed;
    a->motion.angle = instr->angle;
    a->motion.type_flags = 0;
    // 行 143–236：角度包裹到 (-π, π]（kind 2 的随机偏移等依赖 SHT 处理器，暂不展开）。
    while (a->motion.angle > kPiF) {
        a->motion.angle -= kTwoPiF;
    }
    while (a->motion.angle < -kPiF) {
        a->motion.angle += kTwoPiF;
    }
    player_motion_init(&a->motion);
    // 行 240–250：位置补偿（下一步运动后落到指令偏移处）；kind 4/5/6 直接加偏移。
    if (instr->kind == 4 || instr->kind == 5 || instr->kind == 6) {
        a->motion.x += instr->offset_x;
        a->motion.y += instr->offset_y;
    } else {
        a->motion.x += instr->offset_x - a->motion.vel_x;
        a->motion.y += instr->offset_y - a->motion.vel_y;
    }
    // 行 251–276：创建子弹 ANM。资源 = 自机 +0x10（plXX.anm），脚本号 = 指令 +0x22。
    AnmResource* res = resource ? resource : player_anm(p);
    AnmVm* vm = anm_vm_alloc(res);
    anm_vm_set_script(vm, res, instr->anm_script);
    vm->extra_x = a->motion.x;
    vm->extra_y = a->motion.y;
    vm->extra_z = a->motion.z;
    // 行 265–268：脚本跑过一格后 flags3 的 0x200 置起来时，用指令角度作 VM 旋转。
    if ((vm->flags3 & 0x200u) != 0) {
        vm->rot_z = instr->angle;
    }
    a->anm_vm = vm;
    a->anm_manager = res;
    anm_manage_vm(vm, &a->anm_vm);
    a->anm_handle = anm_queue_add(vm);
    card_manager_on_shot(a);
    // 行 277–280：卡牌 vtable+0x28 的发射回调（卡管理器实例接入后补）。
    // 行 290–301：分配子弹并回填攻击对象字段。
    a->bullet_index = player_bullet_alloc(&a->motion.x, a->motion.angle, 9999999, a->damage);
    PlayerBullet* b = player_bullet_pool(p) + (a->bullet_index - 1);
    b->kind = 1;
    b->damage_max = 10000000;
    b->attack_id = a->id;
    b->half_width = a->half_width;
    b->half_height = a->half_height;
    if(instr->handler_a==2) {
        a->half_width=0;b->half_width=0;a->flags&=~1u;
        sound_play_se_at(20,p->x);
    }
    else if(instr->handler_a==3) {
        const auto* q=reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(a->param));
        a->motion.x=*reinterpret_cast<const int32_t*>(q+0x5c)/128.0f;
        a->motion.y=*reinterpret_cast<const int32_t*>(q+0x60)/128.0f;
        a->motion.angle += p->shot_angle;
        while (a->motion.angle > kPiF) {
            a->motion.angle -= kTwoPiF;
        }
        while (a->motion.angle < -kPiF) {
            a->motion.angle += kTwoPiF;
        }
        a->motion.angle += p->shot_offset * (*reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(instr) + 0x4c));
        while (a->motion.angle > kPiF) {
            a->motion.angle -= kTwoPiF;
        }
        while (a->motion.angle < -kPiF) {
            a->motion.angle += kTwoPiF;
        }
    }
    // 460da0：复用攻击槽时清除上一发蛇弹的锁敌/转向状态。
    else if(instr->handler_a==4)a->flags&=~0x3cu;
    else if(instr->handler_a==5)a->motion.angle=get_f32(p,0x479cc);
    else if(instr->handler_a==6)a->motion.angle=rng_float01()*kPiF-kPiF;
    else if(instr->handler_a==7) {
        const float dx=a->motion.x-p->x,dy=a->motion.y-p->y;
        a->motion.angle=(dx==0 && dy==0?1.57079637f:std::atan2(dy,dx))+rng_float_pm1()*0.03490658477f;
    }
    if(instr->se_id>=0)sound_play_se_at(instr->se_id,a->motion.x);
    // 行 302–313：发射回调返回非 0 时撤销这次发射。
    // 行 311–313：指令音效（SE 系统接入后补）。
    return a->bullet_index;
}

void player_attack_update(Player* p) {
    // FUN_0045EDB0：逐帧推进攻击对象，并把位置/角度/判定盒/伤害同步给子弹。
    AttackObject* pool = player_attack_pool(p);
    PlayerBullet* bullets = player_bullet_pool(p);
    for (int32_t i = 0; i < kAttackObjectCount; ++i) {
        AttackObject* a = &pool[i];
        if (a->active == 0) {
            continue;
        }
        PlayerBullet* b = a->bullet_index != 0 ? &bullets[a->bullet_index - 1] : nullptr;
        if (a->anm_vm == nullptr) {
            a->active = 0;
            a->anm_handle = 0;
            if (b != nullptr) b->flags &= ~1u;
            continue;
        }
        if(a->handler_update==1 && a->active==1) {
            stage::Enemy* target=a->unk_090?stage::find_enemy(a->unk_090):nullptr;
            // 45f7c0 only searches when the saved ID was zero on entry.
            if(a->unk_090==0 && stage::g_enemy_manager) {
                float nearest=256.0f*256.0f;
                for(auto* node=stage::g_enemy_manager->enemy_head;node;node=node->next) {
                    auto* e=node->self;if(e->flag&0xc000021)continue;
                    const float dx=e->x-a->motion.x,dy=e->y-a->motion.y,d=dx*dx+dy*dy;
                    if(d<nearest){nearest=d;target=e;}
                }
                a->unk_090=target?target->id:0;
            }
            if(target && (target->flag&0xc000021)==0) {
                const float target_angle=std::atan2(target->y-a->motion.y,target->x-a->motion.x);
                float delta=target_angle-a->motion.angle;
                if(delta>kPiF)delta=target_angle-(a->motion.angle+kTwoPiF);
                else if(a->motion.angle-target_angle>kPiF)delta=target_angle-(a->motion.angle-kTwoPiF);
                if(a->life_i>=60)a->motion.speed+=0.2f;
                else {
                    if(std::fabs(delta)<0.261799395f)a->motion.speed=std::fmin(a->motion.speed+0.2f,16.0f);
                    else if(std::fabs(delta)>=0.785398185f)a->motion.speed=std::fmax(a->motion.speed-0.2f,4.0f);
                    a->motion.angle+=delta*0.08f;
                    while(a->motion.angle>kPiF)a->motion.angle-=kTwoPiF;
                    while(a->motion.angle< -kPiF)a->motion.angle+=kTwoPiF;
                }
            } else {
                a->unk_090=0;
                a->motion.speed=std::fmin(a->motion.speed+0.1f,16.0f);
            }
        } else if(a->handler_update==3 && a->active==1)a->motion.speed+=0.3f;
        else if(a->handler_update==4 && a->active==1) {
            if((a->flags&0x3c)==0) {
                for(auto* node=stage::g_enemy_manager->enemy_head;node;node=node->next) {
                    stage::Enemy& e=*node->self;
                    if(e.flag&0xc000021)continue;
                    if(std::fabs(e.y-a->motion.y)>16 || std::fabs(e.x-a->motion.x)<16)continue;
                    a->flags=(a->flags&~0x38u)|4;anm_interrupt(a->anm_vm,2);a->motion.speed=0;a->unk_01c=0;
                    auto* target=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(a)+0xd4);target[0]=e.x;target[1]=e.y;target[2]=0;
                    break;
                }
            }
            if((a->flags&0x3c)==4 && a->unk_01c++==4) {
                const auto* target=reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(a)+0xd4);
                // 460eee：只按目标的左右位置转为水平飞行。
                a->motion.angle=a->motion.x>target[0]?-kPiF:0.0f;a->motion.speed=14;
                a->flags=(a->flags&~0x34u)|8;
            }
        }
        if(a->handler_update==5 && a->active==1 && (a->motion.x < -192 || a->motion.x>192))
            a->motion.angle=std::remainder(-kPiF-a->motion.angle,kTwoPiF);
        if(a->handler_update==2) {
            auto* option=reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(a->param));
            a->motion.x=*reinterpret_cast<const int32_t*>(option+0x5c)/128.0f;
            a->motion.y=*reinterpret_cast<const int32_t*>(option+0x60)/128.0f;
            const auto* sht=static_cast<const PlayerSht*>(p->sht);
            const auto* instruction=reinterpret_cast<const ShtInstruction*>(sht->sub_offset[a->command>>8])+(a->command&255);
            const float target=p->focus_mode?-1.57079637f:instruction->angle;
            const float delta=std::remainder(target-a->motion.angle,kTwoPiF);
            a->motion.angle=std::fabs(delta)>=0.001f?target:
                std::remainder(a->motion.angle+delta*0.1f,kTwoPiF);
            if(a->active==1) {
                sound_set_se_pan(20,p->x);
                if(a->half_width<512.0f) {
                    a->half_width+=18.0f;
                    b->half_width=a->half_width;
                }
                a->anm_vm->sprite_w=a->half_width;
                a->anm_vm->rot_x=a->half_width;
                a->anm_vm->uv_scale_u=a->half_width/512.0f;
                a->anm_vm->flags2|=0x18u;
                auto* motion=reinterpret_cast<Motion*>(b->motion);
                motion->x=a->motion.x+std::cos(a->motion.angle)*a->half_width*0.5f;
                motion->y=a->motion.y+std::sin(a->motion.angle)*a->half_width*0.5f;
                motion->z=0;
                if(a->laser_hit==0 && a->laser_hit_anm==1) {
                    anm_set_label(a->anm_vm,3);
                    a->laser_hit_anm=0;
                }
                const int group=instruction->option_index<16?instruction->option_index:instruction->option_index>>4;
                if(p->script_a_timer<0 || p->state==2 || p->state==4 || (p->flags_4779c&0x40)!=0 || msg_active() || (instruction->option_index!=0 &&
                   (instruction->option_index-1>=get_i32(p,0x476f8) || p->focus_mode || get_i32(p,0x476fc+group*4)!=g_status.power/g_status.power_step))) {
                    a->active=2;anm_set_label(a->anm_vm,1);put_i32(p,0x476fc+group*4,0);
                    if(a->bullet_index)bullets[a->bullet_index-1].flags&=~1u;
                    a->bullet_index=0;
                    sound_stop_se(20);
                }
                a->laser_hit=0;
            }
        }
        player_motion_init(&a->motion);
        player_motion_step(&a->motion);
        const auto* instr = reinterpret_cast<const ShtInstruction*>(
            static_cast<const PlayerSht*>(p->sht)->sub_offset[a->command >> 8]) + (a->command & 255);
        float corners[4][2];
        bool keep = true;
        if (a->life >= 15.0f && instr->kind != 2) {
            keep = false;
            anm_vm_corners(a->anm_vm, corners);
            for (const auto& corner : corners) {
                if (32.0f < corner[0] && corner[0] < 416.0f &&
                    16.0f < corner[1] && corner[1] < 464.0f) {
                    keep = true;
                    break;
                }
            }
        }
        if (!keep || (a->anm_vm->script_cursor < 0 && (a->anm_vm->flags2 & 1u) == 0)) {
            a->active = 0;
            a->anm_handle = 0;
            anm_vm_free(a->anm_vm);
            a->anm_vm = nullptr;
            if (b != nullptr) {
                b->flags &= ~1u;
            }
            continue;
        }
        if (a->anm_vm != nullptr) {
            a->anm_vm->extra_x = a->motion.x;
            a->anm_vm->extra_y = a->motion.y;
            a->anm_vm->extra_z = a->motion.z;
            if(a->anm_vm->flags3&0x200u)a->anm_vm->rot_z=a->motion.angle;
        }
        if (a->bullet_index != 0 && (a->flags & 1u) != 0) {
            b = &bullets[a->bullet_index - 1];
            Motion* bm = reinterpret_cast<Motion*>(b->motion);
            bm->x = a->motion.x;
            bm->y = a->motion.y;
            bm->z = a->motion.z;
            b->angle = a->motion.angle;
            b->half_width = a->half_width;
            b->half_height = a->half_height;
            b->damage = a->damage;
        }
        a->life += stage::g_speed_scale;
        a->life_prev = static_cast<int32_t>(a->life_i);
        a->life_i = static_cast<float>(static_cast<int32_t>(a->life));
    }
    // 行 246：每帧清掉 0x4779C 的 0x40 标记。
    p->flags_4779c &= ~0x40u;
}

void player_shoot_update(Player* p) {
    // FUN_0045EA00：SHT 射击脚本驱动。
    uint32_t held = input_battle_state().held;
    if(msg_active())held&=~ACTION_SHOT;
    const PlayerSht* sht = static_cast<const PlayerSht*>(p->sht);
    if(p->focus_mode==0) {
        if((held&ACTION_LEFT)!=0) {
            if(p->shot_angle>-2.35619449f)p->shot_angle+=(-2.35619449f-p->shot_angle)*0.08f;
        } else if((held&ACTION_RIGHT)!=0) {
            if(p->shot_angle<-0.785398185f)p->shot_angle+=(-0.785398185f-p->shot_angle)*0.08f;
        } else if((held&ACTION_DOWN)!=0) {
            p->shot_angle+=(-1.57079637f-p->shot_angle)*0.08f;
            if(p->shot_offset<1.0f)p->shot_offset+=0.05f;
        } else if((held&ACTION_UP)!=0) {
            p->shot_angle+=(-1.57079637f-p->shot_angle)*0.08f;
            if(p->shot_offset>-0.3f)p->shot_offset-=0.05f;
        } else {
            p->shot_angle+=(-1.57079637f-p->shot_angle)*0.05f;
        }
        if((held&(ACTION_UP|ACTION_DOWN))==0)p->shot_offset*=0.9f;
    }
    if (p->state != 1) {
        p->unk_47944 = 0;
        p->flag_47948 = 0;
        return;
    }
    // 行 66–107：计时器未启动时，按住射击键则启动并推进第一次触发。
    if (p->script_a_timer < 0) {
        if ((held & ACTION_SHOT) == 0) {
            goto update_secondary_timer;
        } else {
            if (p->script_b_timer < 0) {
                if ((p->flags_476f4 & 1u) == 0) {
                    p->unk_476f0 = 0;
                    p->flags_476f4 |= 1;
                }
                p->script_b_timer = 0;
                p->script_b_value = 0.0f;
                p->script_b = -1;
            }
            player_script_a_start(p, 0);
        }
    }
    // 行 108–152：计时器推进的那一帧扫整个子程序指令表。
    if (p->script_a_timer != p->script_a) {
        const int32_t timer_a = p->script_a_timer;
        const int32_t timer_b = p->script_b_timer;
        int32_t script = g_status.power / g_status.power_step;
        if (p->focus_mode != 0) {
            script += 1 + *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(sht) + 0x20);
        }
        const ShtInstruction* instr =
            reinterpret_cast<const ShtInstruction*>(sht->sub_offset[script]);
        uint32_t index = 0;
        while (static_cast<int8_t>(instr->timer_count) >= 0) {
            bool fire = false;
            if (instr->timer_count == 0) {
                fire = true;
            } else if (instr->timer_b_count == 0) {
                fire = timer_a % instr->timer_count == instr->timer_slot;
            } else {
                fire = timer_b % instr->timer_b_count == instr->timer_b_slot;
            }
            if (fire) {
                player_fire_instruction(p, sht, script, index, timer_a);
            }
            instr = reinterpret_cast<const ShtInstruction*>(
                reinterpret_cast<const uint8_t*>(instr) + kShtInstructionSize);
            ++index;
        }
        card_manager_on_shoot(timer_a, timer_b);
    }
    // 行 158–175：第一计时器推进/复位。
    if (p->script_a_timer < 0xe) {
        player_timer_advance(&p->script_a, &p->script_a_timer, &p->script_a_value);
    } else if ((held & ACTION_SHOT) == 0) {
        if ((p->flags_476e0 & 1u) == 0) {
            p->unk_476dc = 0;
            p->flags_476e0 |= 1;
        }
        p->script_a_timer = -1;
        p->script_a_value = -1.0f;
        p->script_a = -2;
    } else {
        player_timer_subtract(&p->script_a, &p->script_a_timer, &p->script_a_value, 0xe);
    }
update_secondary_timer:
    // 原45ed1a：停止的主计时器不扫SHT、不推进，副计时器仍走自己的尾段。
    if (p->script_b_timer < 0) {
        return;
    }
    if (p->script_b_timer < 0x77) {
        player_timer_advance(&p->script_b, &p->script_b_timer, &p->script_b_value);
        return;
    }
    if ((held & ACTION_SHOT) != 0) {
        player_timer_subtract(&p->script_b, &p->script_b_timer, &p->script_b_value, 0x77);
        return;
    }
    if ((p->flags_476f4 & 1u) == 0) {
        p->unk_476f0 = 0;
        p->flags_476f4 |= 1;
    }
    p->script_b_timer = -1;
    p->script_b_value = -1.0f;
    p->script_b = -2;
}

namespace {
// 45d3a0 / 45d090 / 45d590 use the original timer latch and previous-frame value.
void player_state_timer(Player* p, int frame) {
    if ((p->flags_644 & 1) == 0) { p->flags_640=0; p->flags_644|=1; }
    p->unk_634=frame-1; p->timer_state=frame; p->timer_float=float(frame);
}
void player_invulnerability(Player* p, int frames) {
    if ((p->flags_47784 & 1) == 0) { p->flags_47780=0; p->flags_47784|=1; }
    p->anim_id=frames-1; p->invuln=frames; p->invuln_value=float(frames);
}
void player_tick_timer(Player* p, int offset) {
    const float speed=stage::g_speed_scale;
    const bool unit=speed>0.99f && speed<1.01f;
    put_i32(p,offset,get_i32(p,offset+4));
    put_i32(p,offset+12,0);
    const float time=get_f32(p,offset+8)+(unit?1.0f:speed);
    put_f32(p,offset+8,time);
    put_i32(p,offset+4,unit?get_i32(p,offset+4)+1:int(time));
}
void player_hit_effect(Player* p, int script) {
    AnmResource* resource=anm_get(8,"effect.anm");
    AnmVm* vm=anm_vm_alloc(resource);
    anm_vm_bind_script(vm,resource,script);
    vm->flags3|=0x1000u;
    // These scripts use layer 20's game origin; the renderer supplies 224/16.
    vm->extra_x=p->x; vm->extra_y=p->y; vm->extra_z=p->z;
    vm->rot_z=0;
    anm_vm_update(vm);
    anm_queue_add(vm); anm_manage_vm(vm,nullptr);
}
void player_confirm_death(Player* p) {
    player_hit_effect(p,28);
    --g_status.lives;
    if (g_status.lives>=0)
        g_status.unk_07c=std::clamp(g_status.unk_084,0,g_status.unk_088);
    p->state=2; player_state_timer(p,0); player_invulnerability(p,180);
    anm_vm_bind_script(player_anm_vm(p),player_anm(p),0);
    anm_vm_update(player_anm_vm(p));
    // 488be0(handle,1) schedules the exit label; A22 advances it once this tick.
    for (int i=0;i<4;++i) {
        put_i32(p,0x670+i*0xf0,0);
        for (AnmVm** slot : {&g_option_vm[i],&g_option_max_vm[i]})
            if (*slot) { anm_set_label(*slot,1); anm_manage_vm(*slot,nullptr); }
    }
    p->unk_476f8=0; g_option_power=-1;
    for (int i=0;i<12;++i) {
        const int offset=0xa30+i*0xf0;
        put_i32(p,offset,0);
        for (int j=0;j<2;++j) {
            auto** slot=reinterpret_cast<AnmVm**>(reinterpret_cast<uint8_t*>(p)+offset+0xb0+j*4);
            if (*slot) { anm_set_label(*slot,1); anm_manage_vm(*slot,nullptr); *slot=nullptr; }
        }
    }
    spell_on_sun_card();
    card_passive_clear_damage_events();
    ++stage::g_enemy_manager->field3c;
    stage::g_enemy_manager->field44=0;
    if (g_status.miss<999999) ++g_status.miss;
}
}

int player_collision_circle(float x, float y, float radius, bool query) {
    Player* p=g_player;
    const float dx=p->x-x,dy=p->y-y,distance=dx*dx+dy*dy;
    float size=get_f32(p,p->focus_mode?0x479a0:0x4799c);
    if(p->flags_4779c&0x10)size=size*p->invuln_alpha*3.6f;
    if(distance<size*size+radius*radius) {
        if(!msg_active()) {
            if(query)return 2;
            if(p->state!=2 && p->state!=3 && p->state!=4) {
                if(p->invuln<1)player_hit();
                return 1;
            }
        }
    } else {
        const float graze=std::max(40.0f,radius/2.5f)+size;
        if(distance<graze*graze+radius*radius)return 2;
    }
    return 0;
}

int player_collision_line(float x, float y, float angle, float length, float width, bool query) {
    Player* p=g_player;
    const float c=std::cos(-angle),s=std::sin(-angle),dx=p->x-x,dy=p->y-y;
    const float along=c*dx-s*dy,side=s*dx+c*dy;
    const float hx=get_f32(p,p->focus_mode?0x479b0:0x479a4);
    const float hy=get_f32(p,p->focus_mode?0x479b4:0x479a8);
    const float half=width*0.5f;
    if(along-hx*16<=length && side-hy*16<=half && along+hx*16>=0 && side+hy*16>=-half) {
        if(length<along-hx || half<side-hy || along+hx<0 || side+hy<-half)return 2;
        if(!msg_active()) {
            if(query)return 2;
            if(p->state!=2 && p->state!=3 && p->state!=4 && p->invuln<1) {
                player_hit();
                return 1;
            }
        }
    }
    return 0;
}

void player_hit() {
    Player* p=g_player;
    if (p->state==2 || p->state==3 || p->state==4 || p->invuln>0 || msg_active())
        return;
    if ((p->flags_4779c&8)==0) sound_play_se(2);
    player_hit_effect(p,29);
    spell_on_sun_card();
    p->state=4; player_state_timer(p,0); player_invulnerability(p,6);
    anm_vm_bind_script(player_anm_vm(p),player_anm(p),0);
    anm_vm_update(player_anm_vm(p));
    p->respawn_wait=8;
    card_manager_on_hit();
}

void player_update(Player* p) {
    // 45be90: an expired deathbomb window is resolved before testing Bomb input.
    if (p->state==4) {
        if (p->timer_state>=p->respawn_wait) {
            if (card_manager_on_death()!=0) goto player_tick_tail;
            player_confirm_death(p);
        } else if ((input_battle_state().pressed&ACTION_BOMB)!=0 && bomb_try_activate()) {
            p->state=1; player_state_timer(p,60);
            // Native jumps to the common tail; no normal movement this tick.
            goto player_tick_tail;
        }
    }
    switch (p->state) {
    case 0: {
        p->y_sub=0xf000-p->timer_state*0x2800/60;
        p->y=float(p->y_sub)/128.0f;
        player_reset(p);
        for (auto& point:p->trail) { point[0]=p->x_sub; point[1]=p->y_sub; }
        if (p->timer_state<30) {
            const float radius=float(p->timer_state)*512.0f/30.0f+64.0f;
            laser::cancel_circle(p->respawn_x,p->respawn_y,radius,0,1);
            laser::cancel_circle(p->respawn_x,p->respawn_y,radius*0.25f,0,0);
        } else {
            // 45bf45: spell 0x60 also removes delayed bullets within 128 pixels.
            SpellManager* spell=spell_manager();
            if ((spell->flags&1)!=0 && spell->id==0x60)
                stage::cancel_delayed_enemy_bullets_circle(p->x,p->y,128.0f,0);
            stage::clear_enemy_bullets(p->x,p->y,640.0f,false);
        }
        if (p->timer_state<60) break;
        p->state=1; player_state_timer(p,0);
        [[fallthrough]];
    }
    case 1:
        if ((input_battle_state().pressed&ACTION_BOMB)!=0) bomb_try_activate();
        card_manager_handle_input();
        player_shot_update(p);
        break;
    case 2:
        if (p->timer_state==3) {
            status_sub_power(&g_status,g_status.power_step);
            const float angle=std::atan2(-224.0f,-p->x);
            for (int i=0;i<7;++i)
                stage::spawn_item(1,&p->x,float(i)*kPiF/28.0f+angle-kPiF/8.0f,3.0f,0);
            g_status.money-=std::min(g_status.money/3,100);
            card_manager_after_death_penalty();
            player_refresh_power_options();
        }
        if (p->timer_state>=30) {
            if (g_status.lives<0 && p->timer_state==30) {
                if (replay_mode()!=ReplayMode::Playback) laser::open_game_over();
                // 405990 advances once here, then the common tail advances again.
                player_tick_timer(p,0x634);
            } else {
                p->state=0;
                stage::g_speed_scale=1.0f;
                player_damage_circle(&p->x,32.0f,16.0f,30,150);
                p->respawn_x=p->x; p->respawn_y=p->y; p->respawn_z=p->z;
                p->x=0; p->y=480; p->x_sub=0; p->y_sub=480*128;
                player_reset(p);
                player_invulnerability(p,280); player_state_timer(p,0);
            }
        }
        break;
    case 3:
        if (p->timer_state==15) laser::clear_lasers(1);
        break;
    }
player_tick_tail:
    // The hitbox pool is advanced before the visual and the player clocks.
    PlayerBullet* bullets=player_bullet_pool(p);
    const float speed=stage::g_speed_scale;
    const float step=speed>0.99f && speed<1.01f ? 1.0f : speed;
    for (int32_t i=0;i<kPlayerBulletCapacity;++i) {
        PlayerBullet& b=bullets[i];
        if ((b.flags&1u)==0) continue;
        auto* motion=reinterpret_cast<Motion*>(b.motion);
        player_motion_init(motion); player_motion_step(motion);
        b.hit_radius+=b.unk_008;
        b.angle+=b.unk_010;
        int wraps=0;
        while (b.angle>kPiF) { b.angle-=kTwoPiF; if (wraps++>=0x21) break; }
        while (b.angle< -kPiF) { b.angle+=kTwoPiF; if (wraps++>=0x21) break; }
        b.last_hit_enemy=0;
        b.unk_06c=0;
        b.life_prev=b.life; b.life_f-=step; b.life=int32_t(b.life_f);
        --b.hit_cooldown;
        if (b.life<1) b.flags&=~1u;
    }
    if (p->alpha>0) {
        p->flags_47794=0; p->anim_id2=p->alpha;
        const float time=get_f32(p,0x47790)-step;
        put_f32(p,0x47790,time); p->alpha=int(time);
    }
    AnmVm* vm=player_anm_vm(p);
    vm->flags2&=~0xe0000u;
    if (p->invuln>0) {
        p->flags_47780=0; p->anim_id=p->invuln;
        p->invuln_value-=step; p->invuln=int32_t(p->invuln_value);
        if (p->timer_state!=p->unk_634 && p->timer_state%3==0) {
            vm->color_alt=0xff0000ff; vm->flags2|=0x20000u;
        }
    } else if (p->timer_state%8<4) {
        if (p->flags_4779c&0x20) { vm->color_alt=0xffff0000; vm->flags2|=0x20000u; }
        else if (p->move_scale>1.01f) { vm->color_alt=0xffffff00; vm->flags2|=0x20000u; }
    }
    p->move_scale=1.0f;
    p->shake_x=p->shake_y=p->shake_z=0;
    anm_vm_update(vm);
    float scale=1.0f;
    if (p->flags_4779c&0x10) {
        if (get_i32(p,0x47974)!=0) p->invuln_alpha=interp_eval1(reinterpret_cast<uint8_t*>(p)+0x4794c);
        scale=get_i32(p,0x47974)!=0 && p->timer_state%3==0 ? 1.0f : p->invuln_alpha;
    }
    vm->flags2|=8u; vm->scale_x=vm->scale_y=scale;
    for (int offset:{0x634,0x648,0x65c}) player_tick_timer(p,offset);
    p->flags_4779c&=~0x200u;
    if(!msg_active() && stage::enemy_count()!=0 && (stage_session_flags()&0x4000)==0 &&
       p->shot_timer>0x13 && (p->flags_4779c&0x194)==0)player_shoot_update(p);
    else {
        if((p->flags_476e0&1)==0){p->unk_476dc=0;p->flags_476e0|=1;}
        p->script_a_timer=-1;p->script_a_value=-1;p->script_a=-2;
        if((p->flags_476f4&1)==0){p->unk_476f0=0;p->flags_476f4|=1;}
        p->script_b_timer=-1;p->script_b_value=-1;p->script_b=-2;
        p->unk_47944=0;p->flag_47948=0;sound_stop_se(30);sound_stop_se(55);
    }
    player_attack_update(p);
}

ThreadStatus player_logic_task(void* ctx) {
    if(!stage_session_started())return THREAD_CONTINUE;
    // 原版 LAB_0045caa0：暂停对象非 0 时直接返回 1，否则进 FUN_0045be90。
    if(stage::shop_active())return THREAD_CONTINUE;
    player_update(static_cast<Player*>(ctx));
    return THREAD_CONTINUE;
}

ThreadStatus player_render_task(void* ctx) {
    if(!stage_session_started())return THREAD_CONTINUE;
    // 原版 LAB_0045cac0：state 2（死亡演出）直接跳过绘制。
    player_render_update(static_cast<Player*>(ctx));
    return THREAD_CONTINUE;
}

void player_render_update(Player* p) {
    // LAB_0045cac0：死亡演出（state 2）不画；把世界坐标写进 VM 的附加平移后绘制。
    if (p->state == 2) {
        return;
    }
    AnmVm* vm = player_anm_vm(p);
    vm->extra_x = p->x;
    vm->extra_y = p->y;
    vm->extra_z = p->z;
    vm->flags3 = (vm->flags3 | 0x100000u) & ~0x200000u;
    anm_draw_vm(vm);
}

void player_stage_start() {
    // 443e60 / 4443c0 -> 45abb0: stage entry is not the death respawn state.
    // Keep the constructor/replay position; only a confirmed death moves it
    // to Y=480 and starts the state-0 ascent.
    Player* p=g_player;
    p->state=1;
    for(int offset:{0x476d0,0x476e4,0x634,0x648,0x65c,0x47914}) {
        const int frame=(offset==0x476d0 || offset==0x476e4)?-1:0;
        if((get_i32(p,offset+16)&1)==0) {
            put_i32(p,offset+12,0);put_i32(p,offset+16,get_i32(p,offset+16)|1);
        }
        put_i32(p,offset,frame-1);put_i32(p,offset+4,frame);put_f32(p,offset+8,float(frame));
    }
    p->flags_4779c&=~9u;
    if(g_focus_vm)anm_vm_free(g_focus_vm);
    p->focus_handle=0;
    anm_queue_remove(p->handle_476b4);p->handle_476b4=0;
    stage::hud_refresh_lives();stage::hud_refresh_bombs();
    // 416d50 schedules label 2 without advancing the option animations here.
    p->flags_4779c&=~2u;p->follow_stun=0;
    for(int i=0;i<4;++i)for(AnmVm* vm:{g_option_vm[i],g_option_max_vm[i]})if(vm)anm_set_label(vm,2);
    for(int i=0;i<12;++i)for(int j=0;j<2;++j) {
        AnmVm* vm=*reinterpret_cast<AnmVm**>(reinterpret_cast<uint8_t*>(p)+0xae0+i*0xf0+j*4);
        if(vm)anm_set_label(vm,2);
    }
    player_refresh_power_options();
    p->flags_4779c&=~4u;p->move_scale=1;
    std::memset(p->unk_476fc,0,sizeof(p->unk_476fc));
    for(int offset:{0x479bc,0x479c0,0x479c4,0x20570,0x47974})put_i32(p,offset,0);
    p->invuln_alpha=1;put_f32(p,0x47980,1);
    for(auto& point:p->trail){point[0]=p->x_sub;point[1]=p->y_sub;}
    p->respawn_wait=8;p->shot_angle=-1.57079637f;p->shot_offset=0;
    for(int i=0;i<4;++i)put_i32(p,0x477b4+i*4,
        int32_t(*reinterpret_cast<float*>(static_cast<uint8_t*>(p->sht)+0x10+i*4)*128.0f));
    const float parameters[]={5,30,70,70,128,3,3,1.5f,1.5f,5,1.5f,1.5f,5};
    for(int i=0;i<13;++i)put_f32(p,0x47988+i*4,parameters[i]);
    // The same native slot-8 callbacks used at shop exit reapply card overrides.
    card_manager_shop_exit();
}

void player_system_init(uint32_t difficulty, uint32_t stage,bool new_run) {
    // 4424e0: original mode bits 0x10/0x20 also survive in replay flags.
    const bool playback = replay_mode() == ReplayMode::Playback;
    const bool practice = session().mode == GameMode::Practice ||
        (playback && (replay_info().mode_flags & 1) != 0);
    const bool spell_practice = session().mode == GameMode::SpellPractice ||
        (playback && (replay_info().mode_flags & 2) != 0);
    if(new_run) {
    g_status = GameStatusBlock{};
    g_status.current_stage=int32_t(stage);
    g_status.start_stage=int32_t(app_context()->start_stage);
    g_status.player=app_context()->selected_player;
    g_status.shot=0;g_status.score=0;g_status.difficulty=int32_t(difficulty);
    g_status.spell_id=-1;
    g_player_character = app_context()->selected_player;
    g_player_shot_type = 0;
    g_status.money = 0;
    g_status.power = 0;
    g_status.power_step = 1;
    g_status.lives_max = 7;
    g_status.unk_07c = spell_practice ? 0 : 3;
    g_status.unk_088 = 7;
    g_status.lives = spell_practice ? 0 : practice ?
        (app_context()->practice_lives == 0 ? 7 : app_context()->practice_lives - 1) : 2;
    g_status.unk_084 = 3;
    static constexpr int max_point_value[]={200000,500000,700000,1000000,500000};
    g_status.extend_score_2=1000000;
    g_status.extend_score_3=max_point_value[difficulty]*100;
    g_status.extend_score_1=g_status.extend_score_2;
    }
    g_status.current_stage=int32_t(stage);
    g_status.counter_00c=g_status.counter_010=g_status.counter_014=0;
    g_player_character=g_status.player;g_player_shot_type=g_status.shot;

    g_player = player_create();

    // Spell Practice always starts at four power steps, including stages 1/7.
    if(new_run) {
    const int32_t st = static_cast<int32_t>(stage);
    int32_t power = g_status.power_step * ((spell_practice || (1 < st && st != 7)) ? 4 : 1);
    if (g_status.power_max < power) {
        power = g_status.power_max;
    } else if (power < g_status.power_step) {
        power = g_status.power_step;
    }
    g_status.power = power;
    }
    if(!new_run)card_manager_player_reset();
    player_refresh_power_options();

    // 行 199–203：stage == 7 且未持有 MAGATAMA2 时加卡（卡管理器实例接入后在此调
    // card_manager_owns(CardId::Magatama2) / card_manager_add(...) 与 FUN_004094c0）。

    // 原版 0x45A7A0 载入 SHT 时注册两个线程（0x17 逻辑 / 0x1D 渲染）；
    // 渲染线程要画 +0x14 的 ANM VM，等 A 的 anm 运行时实装后再注册。
    Thread* t = thread_create(player_logic_task);
    thread_set_ctx(t, g_player);
    thread_set_priority(t, 0x17);
    thread_enqueue_logic(t);
    g_player->thread_logic = t;
    Thread* r = thread_create(player_render_task);
    thread_set_ctx(r, g_player);
    thread_set_priority(r, 0x1D);
    thread_enqueue_render(r);
    g_player->thread_render = r;
}

void player_system_shutdown() {
    if (g_player != nullptr) {
        player_destroy(g_player);
    }
}
