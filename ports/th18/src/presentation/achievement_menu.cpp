#include "presentation/achievement_menu.h"

#include <cstdlib>
#include <cstring>
#include <windows.h>

#include "core/app.h"
#include "core/game_session.h"
#include "core/input.h"
#include "core/thread.h"
#include "platform/gfx.h"
#include "gameplay/score_data.h"
#include "presentation/anm.h"
#include "presentation/ending.h"
#include "presentation/sound.h"
#include "presentation/text.h"
#include "replay/replay.h"
#include "resources/dat.h"
#include "stage/stage.h"

struct AchievementMenu {
    char text[30][7][256]{}; // 46e230: name, three locked lines, three unlocked lines.
    AnmVm* heading=nullptr;
    AnmVm* words[4]{};
    int state=1;
    int timer=1;
    int selection=0;
    int description=0;
    int description_frame=0;
    Thread* cover_logic=nullptr;
    Thread* cover_render=nullptr;
    float cover_time=0;
};

namespace {
int selected_achievement=0; // DAT_004cf430
uint8_t previous_keys[256]{};
int secret_cursor=0,secret_time=0; // DAT_00570a84 / DAT_00570a80 persist across menus.
constexpr char locked_name[]="\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48"
                             "\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48\x81\x48";

void read_line(char*& cursor,char* out) {
    char* first=cursor;
    while(*cursor!='\r' && *cursor!='\n')++cursor;
    std::memcpy(out,first,cursor-first);out[cursor-first]=0;
    while(*cursor=='\r' || *cursor=='\n')++cursor;
}

void create_vm(AnmVm*& slot,int script) {
    auto* resource=anm_get(0x10,"title.anm");
    auto* vm=anm_vm_alloc(resource);vm->flags3|=0x1000u;
    anm_vm_set_script(vm,resource,script);anm_queue_add(vm);anm_manage_vm(vm,&slot);
}

// 464860, reached only by the original MASTERSDREAM sequence at entry 12.
void unlock_secret_records() {
    auto& score=score_data();
    std::memset(score.state+0x16,0x11,16);
    for(int spell=0;spell<97;++spell) {
        int32_t& attempts=*reinterpret_cast<int32_t*>(score.character[4]+0x998+spell*0xdc);
        if(attempts<99999)++attempts;
    }
    for(int player=0;player<4;++player)for(int rank=0;rank<6;++rank) {
        ++*reinterpret_cast<int32_t*>(score.character[player]+0x64d0+rank*4);
        ++*reinterpret_cast<int32_t*>(score.character[player]+0x64ec+rank*4);
        for(int stage=0;stage<8;++stage)score.character[player][0x12efd+rank*0x48+stage*8]=1;
    }
    std::memset(score.state+0xd0,1,56);
}

void secret_input() {
    const auto pressed=input_state().pressed;
    if(pressed&0x80103u){secret_cursor=0;secret_time=0;}
    // The native helper accepts DirectInput scan codes and maps the Win32 path
    // to the same alphabet. Keep key edges independent of configurable actions.
    uint8_t keys[256];GetKeyboardState(keys);
    const char sequence[]="MASTERSDREAM";
    if(secret_cursor<12) {
        const int expected=sequence[secret_cursor];
        if((keys[expected]&0x80) && !(previous_keys[expected]&0x80)) {
            ++secret_cursor;secret_time=0;
        } else {
            for(int key='A';key<='Z';++key)
                if((keys[key]&0x80) && !(previous_keys[key]&0x80))secret_cursor=0;
        }
    } else {
        unlock_secret_records();sound_play_se(17);secret_cursor=0;
    }
    std::memcpy(previous_keys,keys,sizeof(keys));
    if(++secret_time>300)secret_cursor=secret_time=0;
}

void update_description(AchievementMenu* menu) {
    const bool unlocked=score_data().state[0x50+menu->description]!=0;
    const int frame=menu->description_frame++;
    if(frame==0) {
        text_write_texture_centered(menu->words[0],0xffffff,0,4,0,
            unlocked?menu->text[menu->description][0]:locked_name);
        anm_set_label(menu->words[0],4);
        for(int i=1;i<4;++i)anm_set_label(menu->words[i],5);
    } else if(frame==3 || frame==5 || frame==7) {
        const int line=(frame-1)/2;
        text_write_texture_centered(menu->words[line],0xffffff,0,4,0,
            menu->text[menu->description][line+(unlocked?3:0)]);
        anm_set_label(menu->words[line],4);
    }
}

// 476060 mode 9 -> 4759e0/475a70 holds an opaque black cover for 30 ticks.
ThreadStatus update_return_cover(void* context) {
    auto* menu=static_cast<AchievementMenu*>(context);
    if(int(menu->cover_time)>=30) {
        thread_remove(menu->cover_render);menu->cover_render=nullptr;menu->cover_logic=nullptr;
        return THREAD_DONE;
    }
    menu->cover_time+=stage::g_speed_scale;
    return THREAD_CONTINUE;
}

ThreadStatus render_return_cover(void*) {
    gfx_select_camera(2);
    auto* device=gfx_device();
    struct Vertex {float x,y,z,rhw;uint32_t color;};
    const Vertex vertices[]={{0,0,0,1,0xff000000},{640,0,0,1,0xff000000},
                             {0,480,0,1,0xff000000},{640,480,0,1,0xff000000}};
    device->SetTexture(0,nullptr);
    device->SetRenderState(D3DRS_ZENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,FALSE);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG2);
    device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG2);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(Vertex));
    return THREAD_CONTINUE;
}
}

AchievementMenu* achievement_menu_create(bool returning_from_extra) {
    auto* menu=new AchievementMenu{};
    size_t size;char* data=static_cast<char*>(dat_read("trophy.txt",&size));
    char* cursor=data;char* end=data+size;
    while(cursor<end && *cursor!='\\') {
        char line[256];
        if(*cursor=='@') {
            read_line(++cursor,line);const int id=std::atoi(line);
            for(int i=0;i<7;++i)read_line(cursor,menu->text[id][i]);
        } else read_line(cursor,line);
    }
    std::free(data);
    menu->selection=menu->description=selected_achievement;
    create_vm(menu->heading,0x30);
    for(int i=0;i<4;++i)create_vm(menu->words[i],0x69+i);
    // 46d320 performs the first description update on the entry tick.
    update_description(menu);
    if(returning_from_extra) {
        menu->cover_logic=thread_create(update_return_cover);menu->cover_logic->flags|=2;
        thread_set_ctx(menu->cover_logic,menu);thread_set_priority(menu->cover_logic,0x14);
        thread_enqueue_logic(menu->cover_logic);
        menu->cover_render=thread_create(render_return_cover);menu->cover_render->flags|=2;
        thread_set_priority(menu->cover_render,0x5b);thread_enqueue_render(menu->cover_render);
    }
    return menu;
}

bool achievement_menu_update(AchievementMenu* menu) {
    if(menu->state==1) {
        const FrameInput input=input_frame();const uint32_t move=input.pressed|input.repeat;
        const int previous=menu->selection;
        if(move&ACTION_UP)menu->selection=(menu->selection+20)%30;
        if(move&ACTION_DOWN)menu->selection=(menu->selection+10)%30;
        if(move&ACTION_LEFT)menu->selection+=menu->selection%10==0?9:-1;
        if(move&ACTION_RIGHT)menu->selection+=menu->selection%10==9?-9:1;
        if(menu->selection!=previous) {
            sound_play_se(10);menu->description=menu->selection;menu->description_frame=0;
        }
        if((input.pressed&(ACTION_SHOT|ACTION_MENU_OK)) &&
           score_data().state[0x50+menu->selection]!=0 && menu->selection<16) {
            sound_play_se(7);bgm_fade(0.5f);
            if(menu->selection<12) {
                ending_create(menu->selection,true);
                return false;
            }
            menu->state=2;menu->timer=0;
        } else if(input.pressed&(ACTION_BOMB|ACTION_MENU_CANCEL)) {
            menu->state=3;menu->timer=0;sound_play_se(9);
            anm_set_label(menu->heading,1);
            for(AnmVm* word:menu->words)anm_set_label(word,1);
        }
        if(menu->selection==12)secret_input();
    } else if(menu->state==2) {
        if(menu->timer==10)app_begin_stage_loading();
        if(menu->timer>39) {
            auto* app=app_context();
            app->selected_player=menu->selection-12;
            app->selected_spell=-1;
            app->extra_ending_view=1;
            app->return_to_achievements=true;
            app->start_stage=app->current_stage=7;
            selected_achievement=menu->selection;
            replay_close();session().mode=GameMode::Full;
            app->next_state=7;
        }
    } else if(menu->state==3 && menu->timer>19) {
        selected_achievement=menu->selection;
        return true;
    }
    if(menu->description_frame<8)update_description(menu);
    ++menu->timer;
    return false;
}

void achievement_menu_render(const AchievementMenu* menu) {
    if(menu->state!=1)return;
    text_set_group(0);text_set_life(0);text_set_style(4);text_set_shadow(true);
    text_set_alignment(1,1);text_set_size(1,1);
    for(int i=0;i<30;++i) {
        TextPos pos{260.0f+76.0f*(i%10),200.0f+68.0f*(i/10),0};
        text_set_color(i==menu->selection?0xffffffffu:0xff707070u);
        text_draw_fmt(text_manager(),&pos,score_data().state[0x50+i]?"%2d":"--",i+1);
    }
    text_set_style(0);text_set_shadow(false);
}

void achievement_menu_destroy(AchievementMenu* menu) {
    thread_remove(menu->cover_logic);thread_remove(menu->cover_render);
    anm_vm_free(menu->heading);
    for(AnmVm* word:menu->words)anm_vm_free(word);
    delete menu;
}
