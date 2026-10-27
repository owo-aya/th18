#include "stage/hud.h"

#include <cstdio>
#include <cstring>
#include <cmath>

#include "cards/card_manager.h"
#include "cards/card_profile.h"
#include "core/game_session.h"
#include "player/player.h"
#include "presentation/anm.h"
#include "presentation/text.h"
#include "presentation/sound.h"
#include "gameplay/spell.h"
#include "stage/stage.h"
#include "stage/session.h"
#include "stage/enemy.h"
#include "stage/ecl_vm.h"
#include "gameplay/score_data.h"
#include "replay/replay.h"

namespace stage {
namespace {
AnmVm* g_frame;
AnmVm* g_extend;
AnmVm* g_rank;
AnmVm* g_character;
AnmVm* g_lives[7];
AnmVm* g_bombs[7];
AnmVm* g_timer_positions[2]; // Stage+84/+88, ASCII2/3 countdown digits.
AnmVm* g_active;
CardId g_active_id;
AnmVm* g_cards[256];
AbilityCard* g_card_objects[256];
int g_card_count;
int g_previous_lives[3];
int g_previous_bombs[3];
uint64_t g_high_score;
AnmVm* g_boss_rings[2][7];
AnmVm* g_boss_stars[10];
int g_boss_spell_count;

AnmVm* create(AnmResource* resource, int script, bool second_queue) {
    AnmVm* vm = anm_vm_alloc(resource);
    vm->node_flags = second_queue ? 4u : 0u;
    anm_vm_bind_script(vm, resource, script);
    vm->flags3 |= 0x1000u;
    anm_vm_update(vm);
    anm_queue_add(vm);
    return vm;
}

void set_gauge(AnmVm** cells, int value, int fragments, int maximum) {
    // 0x441f10 / 0x4420e0：写 label（494），七格中的已取得/碎片/空格/超过上限四种中断。
    for (int i = 0; i < 7; ++i) {
        cells[i]->extra_x = static_cast<float>(7 - maximum) * 28.0f;
        int label = i < value ? 2 : (i == value && i < maximum ? 7 + fragments : 3);
        if (i >= maximum)
            label = 5;
        anm_set_label(cells[i], label);
    }
}

void assign_icon(AnmVm* root, int first, int second, int sprite) {
    AnmResource* resource = anm_manager()->resources[root->anm_table_index];
    if (AnmVm* vm = anm_find_child(root, first)) anm_set_sprite(vm, resource, sprite);
    if (AnmVm* vm = anm_find_child(root, second)) anm_set_sprite(vm, resource, sprite);
}

void cooldown_visual(AnmVm* root, int front_script, int back_script, const AbilityCard* card, bool rotated) {
    const float progress = card->flags_50 & 0x20 ? 1.0f :
        1.0f - card->count_f / card->param_48;
    // 0x408890：图案从底部显露，冷却完毕后变绿，发动期间闪红。
    anm_set_label(root,(card->flags_50&0x20) || card->count==0 ? 2 : 3);
    const int scripts[] = {front_script, back_script};
    for (int script : scripts) {
        AnmVm* vm = anm_find_child(root, script);
        if (vm == nullptr) continue;
        // Native +58/+70/+80: scale geometry and UVs, keeping sprite bounds.
        vm->scale_y=vm->uv_scale_v=progress;
        vm->uv_offset_v=0.625f*(1.0f-progress);
        vm->flags2|=0x18u;
        if(rotated) {
            const float rotation=vm->rot_z+root->rot_z;
            const float offset=40.0f*(1.0f-progress);
            vm->pos_x=-std::sin(rotation)*offset;
            vm->pos_y=std::cos(rotation)*offset;
            vm->pos_z=0;
        }
        if (vm->script->id != front_script)
            continue;
        uint32_t rgb;
        if (card->flags_50 & 0x20)
            rgb = card_manager()->visual_frame % 4 < 3 ? 0xff0000u : 0xffffffu;
        else if (card->count == 0)
            rgb = 0x40ff40u;
        else {
            const uint32_t red_blue = static_cast<uint32_t>(progress * 64.0f);
            const uint32_t green = static_cast<uint32_t>(progress * 64.0f + 128.0f);
            rgb = red_blue << 16 | green << 8 | red_blue;
        }
        vm->color = (vm->color & 0xff000000u) | rgb;
    }
}

void number_string(char* output, uint64_t number) {
    char digits[32];
    const int length = std::sprintf(digits, "%llu", static_cast<unsigned long long>(number));
    int write = 0;
    for (int i = 0; i < length; ++i) {
        if (i && (length - i) % 3 == 0)
            output[write++] = ',';
        output[write++] = digits[i];
    }
    output[write] = 0;
}

void text(float x, float y, float size, uint32_t color, uint32_t secondary,
          int align, const char* value) {
    TextManager* manager = text_manager();
    uint8_t* raw = reinterpret_cast<uint8_t*>(manager);
    *reinterpret_cast<int32_t*>(raw + 0x19250) = align;
    *reinterpret_cast<int32_t*>(raw + 0x19254) = 1;
    text_set_life(0);
    // 数码的16/14像素字模按640基准绘制，大标题47像素字模的半尺寸规则不用于HUD。
    text_set_size(size * 2.0f, size * 2.0f);
    const TextPos pos{x * 2.0f, y * 2.0f, 0.0f};
    text_set_style(11);
    text_set_color(secondary);
    text_queue_add(manager, &pos, value);
    text_set_style(10);
    text_set_color(color);
    text_queue_add(manager, &pos, value);
}

void update_boss_hud() {
    // 43bb70: the circular gauges use the original front378..381 scripts.
    AnmResource* front=anm_get(5,"front.anm");
    for(int slot=0;slot<2;++slot) {
        Enemy* boss=find_enemy(g_enemy_manager->boss_id[slot]);
        auto* block=g_stage->hud_blocks+slot*0x54;
        float& shown=*reinterpret_cast<float*>(block);
        if(boss==nullptr) {
            shown=0;
            for(int segment=0;segment<4;++segment)*reinterpret_cast<float*>(block+0x10+segment*8)=0;
        }
        const bool visible=boss && boss->health<100000 && !(boss->flag&0x31) &&
            boss->state6320<1 && !g_stage->dialog_active;
        if(!visible) {
            for(auto*& vm:g_boss_rings[slot]){anm_vm_free(vm);vm=nullptr;}
            *reinterpret_cast<int32_t*>(block+0x4c)=0;
            continue;
        }
        const float target=float(boss->health)/float(boss->health_max);
        if(shown<target)shown+=0.025f;
        if(shown>target)shown=target;
        *reinterpret_cast<float*>(block+4)=target;
        *reinterpret_cast<int32_t*>(block+8)=boss->health;
        if(g_boss_rings[slot][0]==nullptr) {
            for(int n=0;n<7;++n)g_boss_rings[slot][n]=create(front,n<3?0x17a+n:0x17d,false);
            *reinterpret_cast<int32_t*>(block+0x4c)=1;
        }
        for(auto* vm:g_boss_rings[slot])anm_vm_update(vm);
        auto* ring=g_boss_rings[slot][0];ring->flags2|=4;ring->rot_a=-shown*6.283185307f;
        for(int n=0;n<3;++n) {
            auto* vm=g_boss_rings[slot][n];vm->extra_x=boss->x*2;vm->extra_y=boss->y*2;vm->extra_z=boss->z;
        }
        for(int n=0;n<4;++n) {
            const float ratio=*reinterpret_cast<float*>(block+0x10+n*8);
            auto* vm=g_boss_rings[slot][n+3];const bool show=ratio!=0 && ratio<shown;
            vm->flags2=(vm->flags2&~2u)|(show?2u:0u);
            if(show) {
                vm->rot_z=std::remainder(-3.14159265f-ratio*6.283185307f,6.283185307f);vm->flags2|=4;
                vm->extra_x=boss->x*2-std::sin(vm->rot_z)*112;
                vm->extra_y=boss->y*2+std::cos(vm->rot_z)*112;vm->extra_z=boss->z;
            }
        }
        const float dx=boss->x-g_player->x,dy=boss->y-g_player->y;
        int32_t& near=*reinterpret_cast<int32_t*>(block+0x50);
        if((!near && dx*dx+dy*dy<6400) || (near && dx*dx+dy*dy>=9216)) {
            near=!near;for(auto* vm:g_boss_rings[slot])anm_set_label(vm,near?3:2);
        }
    }
    for(int n=0;n<10;++n) {
        if(n<g_boss_spell_count) {
            if(g_boss_stars[n]==nullptr)g_boss_stars[n]=create(front,0x3a+n,false);
        } else if(g_boss_stars[n]) {
            anm_set_label(g_boss_stars[n],1);anm_manage_vm(g_boss_stars[n],nullptr);g_boss_stars[n]=nullptr;
        }
        if(g_boss_stars[n])anm_vm_update(g_boss_stars[n]);
    }
}

bool countdown_visible() {
    return g_stage->field1b8>=0 && find_enemy(g_enemy_manager->boss_id[0]) &&
        !(g_enemy_manager->flags88&1) && !g_stage->dialog_active &&
        !(stage_session_flags()&0x10000u);
}

void update_countdown() {
    // 43bb70: visibility, player proximity, seconds pulses and digit sprites.
    if(!countdown_visible()) {
        for(auto* vm:g_timer_positions)vm->flags2&=~2u;
        g_stage->flags=(g_stage->flags&~0x200u)|0x400u;
        return;
    }
    for(auto* vm:g_timer_positions)vm->flags2|=2u;
    const uint32_t state=g_stage->flags&0x600u;
    const bool bottom=(spell_manager()->flags&0x100)!=0;
    if(state==0) {
        if((!bottom && g_player->y<128) || (bottom && g_player->y>320)) {
            g_stage->flags=(g_stage->flags&~0x400u)|0x200u;
            for(auto* vm:g_timer_positions)anm_set_label(vm,5);
        }
    } else if(state==0x200) {
        if((!bottom && g_player->y<160) || (bottom && g_player->y>288)) {
            for(auto* vm:g_timer_positions)anm_set_label(vm,4);
            g_stage->flags&=~0x600u;
        }
    } else {
        for(auto* vm:g_timer_positions) {
            anm_interrupt(vm,(spell_manager()->flags&1)?2:3);
            anm_interrupt(vm,4);
        }
        g_stage->flags&=~0x600u;
    }
    const int seconds=g_stage->field1b8,previous=g_stage->field1c0;
    if(seconds<previous && seconds<5) {
        for(auto* vm:g_timer_positions)anm_set_label(vm,seconds<2?9:8);
        sound_play_se(seconds<2?12:11);
    } else if(seconds>previous) {
        for(auto* vm:g_timer_positions)anm_set_label(vm,7);
    }
    if(seconds!=previous) {
        auto* ascii=anm_manager()->resources[2];
        anm_set_sprite(g_timer_positions[0],ascii,239+seconds/10);
        anm_set_sprite(g_timer_positions[1],ascii,239+seconds%10);
    }
    g_stage->field1c0=seconds;
}
} // namespace

void hud_boss_segment(int boss,int index,float ratio,uint32_t color) {
    auto* entry=g_stage->hud_blocks+boss*0x54+0x10+index*8;
    *reinterpret_cast<float*>(entry)=ratio;*reinterpret_cast<uint32_t*>(entry+4)=color;
}
void hud_boss_spells(int count) {g_boss_spell_count=count;}

void hud_refresh_lives() {
    set_gauge(g_lives,g_status.lives,g_status.unk_070,g_status.lives_max);
    g_previous_lives[0]=g_status.lives;g_previous_lives[1]=g_status.unk_070;g_previous_lives[2]=g_status.lives_max;
}
void hud_refresh_bombs() {
    set_gauge(g_bombs,g_status.unk_07c,g_status.unk_080,g_status.unk_088);
    g_previous_bombs[0]=g_status.unk_07c;g_previous_bombs[1]=g_status.unk_080;g_previous_bombs[2]=g_status.unk_088;
}

void hud_show_extend() {
    // 4575f0 / 445a80 case 5：405bf0 创建 front53 入第一队列并存入 stage+0xbc。
    // 释放旧通知，再新建并推进初帧（478580），与原版替代语义一致。
    anm_vm_free(g_extend);
    AnmResource* front=anm_get(5,"front.anm");
    g_extend=anm_vm_alloc(front);anm_vm_bind_script(g_extend,front,53);
    g_extend->flags3|=0x1000u;
    g_extend->rot_z=0;
    g_extend->extra_x=0;g_extend->extra_y=0;g_extend->extra_z=0;
    anm_vm_update(g_extend);
    g_extend->node_flags=0;
    anm_queue_add(g_extend);anm_manage_vm(g_extend,&g_extend);
}

void hud_create(int difficulty, int player) {
    g_boss_spell_count=0;
    // 0x43a8b0：front框、七命/七Bomb、难度及人物标识均由原ANM脚本生成。
    AnmResource* front = anm_get(5, "front.anm");
    g_frame = create(front, 0, false);
    g_rank = create(front, 75 + difficulty, false);
    g_character = create(front, 101 + player, false);
    for (int i = 0; i < 7; ++i) {
        g_lives[i] = create(front, 32 + i, true);
        g_bombs[i] = create(front, 40 + i, true);
        g_lives[i]->flags3&=~0x30000u;g_bombs[i]->flags3&=~0x30000u;
        anm_manage_vm(g_lives[i],&g_lives[i]);anm_manage_vm(g_bombs[i],&g_bombs[i]);
    }
    hud_refresh_lives();hud_refresh_bombs();
    AnmResource* ascii = *reinterpret_cast<AnmResource**>(reinterpret_cast<uint8_t*>(text_manager()) + 0x19260);
    for (int i = 0; i < 2; ++i) {
        g_timer_positions[i] = create(ascii, 2 + i, false);
        g_timer_positions[i]->flags2 &= ~2u;
        g_timer_positions[i]->flags3 &= ~0x300000u;
    }
    g_high_score = card_profile_high_score(player, difficulty);
    const bool playback=replay_mode()==ReplayMode::Playback;
    if(session().mode==GameMode::SpellPractice || (playback && (replay_info().mode_flags&2)))
        g_high_score=uint64_t(*reinterpret_cast<uint32_t*>(score_data().character[player]+0x9a8+g_status.spell_id*0xdc))*10;
    else if(session().mode==GameMode::Practice || (playback && (replay_info().mode_flags&1)))
        g_high_score=uint64_t(*reinterpret_cast<uint32_t*>(score_data().character[player]+0x12ef0+(difficulty*9+session().stage)*8))*10;
    hud_refresh_cards();
}

void hud_refresh_cards() {
    if (g_frame == nullptr) return; // 初装/回放恢复早于本关HUD创建。
    if (g_active) { anm_vm_free(g_active); g_active = nullptr; }
    for (int i = 0; i < g_card_count; ++i) anm_vm_free(g_cards[i]);
    AnmResource* cards = anm_get(0x1f, "abcard.anm");
    const int selected = card_manager_selected_id();
    if (selected >= 0) {
        g_active_id = static_cast<CardId>(selected);
        g_active = create(cards, 0, false);
        assign_icon(g_active, 3, 4, card_info_by_id(selected)->text_desc);
    }
    int category_count[3]{};
    for (int i = 0; i < card_manager_equipped_count(); ++i)
        ++category_count[card_info_by_id(card_manager_equipped_id(i))->category];
    int category_index[3]{};
    g_card_count = 0;
    for (int index = 0; index < card_manager_card_count(); ++index) {
        AbilityCard* card=card_manager_card_at(index);
        if(card->id==Null)continue;
        const int i=g_card_count++;
        g_card_objects[i]=card;
        const CardInfo* info = card->info;
        const int category = info->category;
        g_cards[i] = create(cards, 5, true);
        g_cards[i]->extra_x = static_cast<float>(category_count[category] - 1) * 40.0f -
                              category_index[category]++ * 80.0f;
        g_cards[i]->extra_y = static_cast<float>(category) * 130.0f;
        assign_icon(g_cards[i], 6, 7, info->text_desc);
    }
}

void hud_update() {
    // Explicit pickup events always write labels; polling also observes the
    // port's direct stock writes in bomb use, death and continue handling.
    if(g_status.lives!=g_previous_lives[0] || g_status.unk_070!=g_previous_lives[1] || g_status.lives_max!=g_previous_lives[2])hud_refresh_lives();
    if(g_status.unk_07c!=g_previous_bombs[0] || g_status.unk_080!=g_previous_bombs[1] || g_status.unk_088!=g_previous_bombs[2])hud_refresh_bombs();
    update_boss_hud();
    update_countdown();
    anm_vm_update(g_frame);
    anm_vm_update(g_rank);
    anm_vm_update(g_character);
    for (AnmVm* vm : g_timer_positions) anm_vm_update(vm);
}

void hud_update_cards() {
    // 408640 runs at A0x16 after card effects, before the ANM tick.
    if (g_active) {
        cooldown_visual(g_active, 2, 3, card_manager()->special, false);
        anm_vm_update(g_active);
    }
    for (int i = 0; i < g_card_count; ++i) {
        if (g_card_objects[i]->flags_50&8)
            cooldown_visual(g_cards[i], 9, 7, g_card_objects[i], true);
        anm_vm_update(g_cards[i]);
    }
}

void hud_spell_start() {
    for (AnmVm* vm : g_timer_positions) anm_interrupt(vm, 2);
}

void hud_spell_end() {
    for (AnmVm* vm : g_timer_positions) anm_interrupt(vm, 3);
}

void hud_select_card() {
    if (g_frame == nullptr) return;
    g_active_id = static_cast<CardId>(card_manager_selected_id());
    assign_icon(g_active, 3, 4, card_info_by_id(g_active_id)->text_desc);
}

void hud_render() {
    // 43cd00: fractional seconds follow ASCII2's animated position and alpha.
    if(countdown_visible() && !(stage_session_flags()&0x10u)) {
        auto* vm=g_timer_positions[0];
        const float x=vm->pos_x+16,y=vm->pos_y-7;
        TextPos pos{x*2,y*2,0};
        text_set_life(0);text_set_group(2);text_set_style(4);
        text_set_alignment(1,1);text_set_color(vm->color);text_set_size(2,2);
        text_draw_fmt(text_manager(),&pos,".");
        pos.x=(x+8)*2;pos.y=(y+6)*2;text_set_size(1.2f,1.2f);
        text_draw_fmt(text_manager(),&pos,"%.2d",g_stage->timer_centiseconds);
    }
    text_set_group(3);
    const uint32_t alpha = g_lives[0]->color & 0xff000000u;
    char value[64];
    const uint64_t score = static_cast<uint64_t>(session().score) * 10;
    if (g_high_score < score)
        g_high_score = score;
    number_string(value, g_high_score);
    text(620, 42, 1, alpha | 0x707070u, alpha | 0xffffffu, 2, value);
    number_string(value, score);
    text(620, 64, 1, alpha | 0x001080u, alpha | 0xffffffu, 2, value);
    std::sprintf(value, "%3d/3", g_status.unk_070);
    text(576, 120, .6f, alpha, alpha | 0xffffffu, 1, value);
    std::sprintf(value, "%3d/3", g_status.unk_080);
    text(576, 158, .6f, alpha, alpha | 0xffffffu, 1, value);
    std::sprintf(value, "%d.", g_status.power / g_status.power_step);
    text(540, 182, 1, alpha | 0x800000u, alpha | 0xffd0d0u, 1, value);
    std::sprintf(value, "%.2d", g_status.power % g_status.power_step * 100 / g_status.power_step);
    text(560, 189, .6f, alpha | 0x800000u, alpha | 0xffd0d0u, 1, value);
    std::sprintf(value, "/%d.", g_status.power_max / g_status.power_step);
    text(574, 182, 1, alpha | 0x800000u, alpha | 0xffd0d0u, 1, value);
    text(606, 189, .6f, alpha | 0x800000u, alpha | 0xffd0d0u, 1, "00");
    number_string(value, g_status.money);
    text(620, 204, 1, alpha | 0x808000u, alpha | 0xffffd0u, 2, value);
    uint8_t* raw = reinterpret_cast<uint8_t*>(text_manager());
    *reinterpret_cast<int32_t*>(raw + 0x19250) = 1;
    text_set_style(0);
    text_set_color(0xffffffffu);
    text_set_size(1, 1);
    text_set_group(0);
}

void hud_destroy() {
    anm_vm_free(g_extend);g_extend=nullptr;
    for(auto& ring:g_boss_rings)for(auto*& vm:ring){anm_vm_free(vm);vm=nullptr;}
    for(auto*& vm:g_boss_stars){anm_vm_free(vm);vm=nullptr;}
    anm_vm_free(g_frame);
    g_frame = nullptr;
    anm_vm_free(g_rank);
    anm_vm_free(g_character);
    for (AnmVm* vm : g_timer_positions) anm_vm_free(vm);
    for (int i = 0; i < 7; ++i) {
        anm_vm_free(g_lives[i]);
        anm_vm_free(g_bombs[i]);
    }
    if (g_active) {
        anm_vm_free(g_active);
        g_active = nullptr;
    }
    for (int i = 0; i < g_card_count; ++i)
        anm_vm_free(g_cards[i]);
    g_card_count = 0;
}
} // namespace stage


