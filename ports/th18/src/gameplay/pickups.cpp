#include "gameplay/pickups.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <list>

#include "cards/card_manager.h"
#include "core/game_session.h"
#include "core/input.h"
#include "core/rng.h"
#include "core/thread.h"
#include "platform/gfx.h"
#include "player/bomb.h"
#include "player/player.h"
#include "presentation/anm.h"
#include "presentation/msg.h"
#include "presentation/sound.h"
#include "stage/hud.h"
#include "stage/enemy.h"
#include "stage/session.h"
#include "stage/stage.h"

namespace stage {
namespace {
// 42aa30: two independent free lists, 600 ordinary items and 4096 cancel points.
struct Pickup {
    AnmVm main, indicator;
    AnmVm* card;
    float x,y,z,vx,vy,vz,speed,angle,pull_speed,time;
    int previous,frame,state,kind,delay,indicator_shown,next,spawn_sound,cancel_value;
};
std::array<Pickup,600+4096> items{};
int free_head[2],created,onscreen,cancel_density,cancel_value;
float movement_factor;
AnmResource* bullets;
AnmResource* cards;
Thread* logic;
Thread* drawing;
std::list<AnmVm*> flashes;

constexpr int item_scripts[20][2]={
    {-1,-1},{112,134},{113,135},{114,136},{115,137},{116,138},{117,139},{118,140},
    {119,141},{120,-1},{121,-1},{122,-1},{123,-1},{124,-1},{125,-1},{-1,-1},
    {3,-1},{4,-1},{5,-1},{6,-1}
};

struct Popup {
    uint8_t digits[12],count;
    bool active;
    float x,y,z,speed,time;
    int previous,frame;
    uint32_t color;
};
std::array<Popup,13> popups{};
int popup_cursor;
AnmVm popup_font{};
AnmResource* ascii;
Thread* popup_logic;
Thread* popup_drawing;

float player_field(int offset) {
    return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_player)+offset);
}
void tick(int& previous,int& frame,float& time) {
    previous=frame;
    const float dt=g_speed_scale;
    if(dt>0.99f && dt<1.01f){++frame;time+=1;}
    else {time+=dt;frame=int(time);}
}
void add_score(uint32_t value) {
    session().score=std::min(session().score+value,999999999u);
    g_status.score=session().score;
}
// 4645f0: ten rotating score slots; digits are stored least significant first.
void popup(const Pickup& item,int value,uint32_t color) {
    if(popup_cursor>9)popup_cursor=0;
    Popup& p=popups[popup_cursor++];
    p.active=true;p.count=0;
    if(value<0){p.digits[p.count++]=10;}
    else {
        do {p.digits[p.count++]=uint8_t(value%10);value/=10;}while(value!=0);
    }
    p.color=color;p.previous=-1;p.frame=0;p.time=0;
    p.x=item.x;p.y=item.y;p.z=item.z;p.speed=1;
}
ThreadStatus update_popups(void*) {
    for(auto& p:popups)if(p.active) {
        p.y-=p.speed*g_speed_scale;p.speed*=0.949999988079071f;
        tick(p.previous,p.frame,p.time);
        if(p.frame>60)p.active=false;
    }
    return THREAD_CONTINUE;
}
ThreadStatus draw_popups(void*) {
    gfx_select_camera(1);
    for(const auto& p:popups)if(p.active) {
        const float spacing=p.frame<8?8.0f/p.time:8.0f;
        popup_font.extra_x=p.x-float(p.count)*spacing*0.5f;
        popup_font.extra_y=p.y;popup_font.extra_z=p.z;
        const float dx=g_player->x-p.x,dy=g_player->y-p.y;
        const int distance=int(dx*dx+dy*dy);
        const int alpha=distance<=4096?128:distance<=16384?128+(distance-4096)*128/12288:255;
        popup_font.color=(p.color&0xffffffu)|(uint32_t(alpha)<<24);
        for(int remaining=p.count;remaining>0;--remaining) {
            const int digit=p.digits[remaining-1];
            const int change=(28-remaining)*2;
            int sprite=-1;
            if(p.frame<change-4 || digit==10)sprite=289+digit;
            else if(p.frame<change)sprite=300+digit;
            else if(p.frame<(30-remaining)*2)sprite=310+digit;
            if(sprite>=0){anm_set_sprite(&popup_font,ascii,sprite);anm_draw_vm(&popup_font);}
            popup_font.extra_x+=spacing;
        }
    }
    return THREAD_CONTINUE;
}
void retire(Pickup& p) {
    anm_vm_free(p.card);p.card=nullptr;p.state=0;
    const int index=int(&p-items.data()),pool=index<600?0:1;
    p.next=free_head[pool];free_head[pool]=index;
}
void bind(AnmVm& vm,int script) {
    anm_vm_bind_script(&vm,bullets,script);
    anm_vm_update(&vm);
}
// 4472b0: the same appearance circle is used for P/coins and valuable drops.
// Its own script chooses layer 16; no item-specific size is imposed.
void appearance(Pickup& p) {
    const bool valuable=p.kind==4 || p.kind==5 || p.kind==6 || p.kind==7 ||
        p.kind==15 || (p.kind>=16 && p.kind<=19);
    if(valuable || p.kind==1 || p.kind==2) {
        AnmResource* resource=anm_get(8,"effect.anm");
        AnmVm* vm=anm_vm_alloc(resource);
        anm_vm_bind_script(vm,resource,101);vm->flags3|=0x1000u;
        vm->extra_x=p.x;vm->extra_y=p.y;vm->extra_z=p.z;
        anm_vm_update(vm);anm_queue_add(vm);
        flashes.push_back(vm);anm_manage_vm(vm,&flashes.back());
        if(valuable)sound_play_se(p.kind==4 || p.kind==5?74:48);
        else if(p.spawn_sound>=0)sound_play_se(p.spawn_sound);
    }
}
void refresh_point_value() {
    const float unit=g_difficulty==0?500.0f:1000.0f;
    g_status.extend_score_1=std::clamp(int((float(g_status.money)*unit+unit)*100.0f),
        g_status.extend_score_2,g_status.extend_score_3);
}
uint32_t point_value(const Pickup& p,bool power,uint32_t& color) {
    const int value=g_status.extend_score_1/100,rounded=value-value%10;
    int award;
    if(g_player->y<=player_field(0x47998) || p.state==3){award=rounded;color=0xffffff00;}
    else {
        const int height=power?int(g_player->y)-int(player_field(0x47998)):
            int(g_player->y-player_field(0x47998));
        award=height*((value%10-value)*9/10)/450+rounded*9/10;
        color=0xffffffff;
    }
    award=award/10*10;
    return award<1?10u:uint32_t(award);
}
void collect(Pickup& p) {
    switch(p.kind) {
    case 1: {
        refresh_point_value();
        uint32_t award=100;
        if(g_status.power<g_status.power_max) {
            if(status_add_power(&g_status,1)) {
                player_refresh_power_options();popup(p,-1,0xffffff40);
                sound_play_se_at(13,p.x);
            }
        } else {
            uint32_t color;award=point_value(p,true,color);
            popup(p,int(award),color);popup(p,int(award),0xffffffff);
        }
        add_score(award/10);break;
    }
    case 2: {
        refresh_point_value();
        uint32_t color;const uint32_t award=point_value(p,false,color);
        popup(p,int(award),color);add_score(award/10);
        ++*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(&g_status)+0x44);
        ++*reinterpret_cast<uint32_t*>(reinterpret_cast<uint8_t*>(&g_status)+0x54);
        ++g_status.money;break;
    }
    case 3: {
        uint32_t award=100;
        if(g_status.power<g_status.power_max) {
            if(status_add_power(&g_status,g_status.power_step)) {
                player_refresh_power_options();sound_play_se_at(13,p.x);
                popup(p,-1,0xffffff40);
            }
        } else {
            // 4463c0 adds 2000 here and another 20000/10 in the common branch.
            add_score(2000);award=20000;popup(p,20000,0xff808080);
            sound_play_se_at(13,p.x);
        }
        add_score(award/10);break;
    }
    case 4:status_add_life_fragment(&g_status);break;
    case 5:
        // A whole-life pickup does not increment the fragment-extend counter.
        if(g_status.lives>=g_status.lives_max)g_status.unk_070=0;
        if(++g_status.lives>g_status.lives_max){g_status.lives=g_status.lives_max;g_status.unk_070=0;}
        hud_refresh_lives();sound_play_se(17);hud_show_extend();break;
    case 6:status_add_bomb_fragment(&g_status);break;
    case 7:status_add_bomb(&g_status);break;
    case 8:
        if(status_add_power(&g_status,g_status.power_max)) {
            player_refresh_power_options();popup(p,-1,0xffffff40);sound_play_se_at(13,p.x);
        }
        break;
    case 9:case 10:case 11:case 12:case 13:case 14:add_score(g_status.money/10);break;
    case 16:case 17:case 18:case 19:
        card_manager_acquire(static_cast<CardId>(item_scripts[p.kind][0]),0);break;
    }
    sound_play_se_at(37,p.x);
    retire(p);
}
bool auto_collect() {
    return ((g_player->state!=2 && g_player->state!=4) && g_player->y<player_field(0x47998)) ||
        (bomb_active() && bomb_elapsed_frames()<60) || msg_active();
}
ThreadStatus update(void*) {
    if(!stage_session_started() || (stage_session_flags()&0x405)!=0 || shop_active())return THREAD_CONTINUE;
    cancel_density=0;onscreen=0;
    for(Pickup& p:items) {
        if(p.state==0)continue;
        if(p.state==5) {
            if(--p.delay>=0)continue;
            p.state=2;bind(p.main,item_scripts[p.kind][0]);
            p.indicator.flags2&=~1u;p.indicator.script_cursor=-1;
            continue;
        }
        if(p.state==1) {
            if(p.delay>0){if(--p.delay==0)appearance(p);continue;}
            if(auto_collect()){p.state=3;p.pull_speed=player_field(0x47988);}
            else {
                p.x+=p.vx*g_speed_scale*movement_factor;p.y+=p.vy*g_speed_scale*movement_factor;
                p.z+=p.vz*g_speed_scale*movement_factor;p.vy+=g_speed_scale*0.03f*movement_factor;
                if(p.frame>31) {
                    if(movement_factor>=1) {
                        p.main.color=0xffffffff;
                        if(p.card)p.card->color=0xffffffff;
                    } else {
                        const float y=float(rng_next(card_rng()))*4.656612873077393e-10f-1;
                        const float x=float(rng_next(card_rng()))*4.656612873077393e-10f-1;
                        p.main.pos_x=x;p.main.pos_y=y;p.main.pos_z=0;
                        p.main.color=0xffffa0ff;
                        if(p.card){p.card->extra_x=x;p.card->extra_y=y;p.card->extra_z=0;p.card->color=0xffa0ffff;}
                    }
                }
                if(p.vy>=0)p.vx=0;
                if(p.vy>2)p.vy=2;
                if(p.y>472 || std::fabs(p.x)>=200){retire(p);continue;}
            }
        } else if(p.state==2) {
            p.x+=p.vx*g_speed_scale;p.y+=p.vy*g_speed_scale;p.z+=p.vz*g_speed_scale;
            p.vy+=g_speed_scale*0.03f;
            if(p.vy>=0){p.state=3;p.pull_speed=player_field(0x47988);}
            else if(p.y>472 || std::fabs(p.x)>=200){retire(p);continue;}
        } else if(p.state==4 && auto_collect()){p.state=3;p.pull_speed=player_field(0x47988);}
        if(p.state==3 || p.state==4) {
            const float dx=g_player->x-p.x,dy=g_player->y-p.y;
            const float angle=dx==0 && dy==0?1.5707963705062866f:std::atan2(dy,dx);
            p.vx=std::cos(angle)*p.pull_speed;p.vy=std::sin(angle)*p.pull_speed;
            p.x+=p.vx*g_speed_scale;p.y+=p.vy*g_speed_scale;p.z+=p.vz*g_speed_scale;
            if(p.pull_speed<12)p.pull_speed+=0.2f;
            if(g_player->state==4){p.state=1;p.vx=p.vy=0;}
        }
        if(g_player->state!=2) {
            const float dx=g_player->x-p.x,dy=g_player->y-p.y,distance=dx*dx+dy*dy;
            const float inner=player_field(0x4798c);
            if(distance<inner*inner){collect(p);continue;}
            if(p.state!=3 && p.state!=4 && !(p.kind>=9 && p.kind<=14)) {
                const float outer=player_field(input_battle_state().held&ACTION_SLOW?0x47990:0x47994);
                if(distance<outer*outer){p.state=4;p.pull_speed=player_field(0x47988)/3;}
            }
        }
        if(p.main.flags2&1)anm_vm_update(&p.main);
        if(p.indicator.flags2&1)anm_vm_update(&p.indicator);
        if(p.card){p.card->extra_x=p.x;p.card->extra_y=p.y;p.card->extra_z=p.z;}
        tick(p.previous,p.frame,p.time);++onscreen;
    }
    if(movement_factor<1)movement_factor+=0.1f;
    flashes.remove(nullptr);
    return THREAD_CONTINUE;
}
ThreadStatus draw(void*) {
    if(stage_session_flags()&4)return THREAD_CONTINUE;
    gfx_select_camera(3);
    for(Pickup& p:items)if(p.state!=0 && (p.main.flags2&1)!=0 && p.delay<1) {
        // 446d60 tests the VM's local Y, not the item's world Y.
        const bool main_visible=p.main.pos_y>=-8;
        p.main.extra_x=p.indicator.extra_x=p.x;
        p.main.extra_y=p.indicator.extra_y=p.y;
        p.main.extra_z=p.indicator.extra_z=p.z;
        if(main_visible){anm_draw_vm(&p.main);p.indicator_shown=0;}
        else {
            if(p.indicator.flags2&1) {
                const float y=p.indicator.pos_y;p.indicator.pos_y=8;
                const uint8_t alpha=y+8<32?uint8_t(int((y+8)*0.03125f*255.0f)):255;
                p.indicator.color=(p.indicator.color&0xffffffu)|(uint32_t(alpha)<<24);
                anm_draw_vm(&p.indicator);
            }
            p.indicator_shown=1;
        }
    }
    return THREAD_CONTINUE;
}
} // namespace

// 446f40: allocation order is observable (update order and cancel-point delay).
void spawn_item(int32_t kind,const float* position,float angle,float speed,int32_t delay) {
    if(kind>19)return;
    ++created;
    const bool cancel=kind>=9 && kind<=14;
    const int pool=cancel?1:0,index=free_head[pool];
    if(index<0)return; // Original pool exhaustion returns no item.
    Pickup& p=items[index];
    p.x=cancel?position[0]:std::clamp(position[0],-192.0f,192.0f);p.y=position[1];p.z=position[2];
    p.vx=std::cos(angle)*speed;p.vy=std::sin(angle)*speed;p.vz=0;
    p.previous=-1;p.frame=0;p.time=0;p.spawn_sound=-1;
    p.kind=kind==15?6:kind;p.pull_speed=0;p.indicator_shown=0;
    if(cancel) {
        p.state=5;p.cancel_value=cancel_value;p.speed=speed;p.angle=angle;
        p.delay=cancel_density<256?created%4:
            cancel_density<512?created%8+4:cancel_density<1024?created%16+8:created%32+16;
        p.indicator_shown=kind;
    } else {
        p.state=1;p.delay=delay;p.speed=0;
        if(delay==0)appearance(p);
        if(p.kind>=16 && p.kind<=19) {
            p.main.flags2&=~1u;p.main.script_cursor=-1;
            p.indicator.flags2&=~1u;p.indicator.script_cursor=-1;
            p.card=anm_vm_alloc(cards);anm_vm_bind_script(p.card,cards,14);
            p.card->flags3|=0x1000u;anm_vm_update(p.card);
            // 409310 reads CardInfo+30: the 64x80 icon, not the 256x320 preview at +2c.
            anm_set_sprite(anm_find_child(p.card,15),cards,card_info_by_id(item_scripts[p.kind][0])->text_desc);
            p.card->extra_x=p.x;p.card->extra_y=p.y;p.card->extra_z=p.z;p.card->color=0xffffffff;
            anm_queue_add(p.card);anm_manage_vm(p.card,&p.card);
        } else {
            bind(p.main,item_scripts[p.kind][0]);bind(p.indicator,item_scripts[p.kind][1]);
            p.card=nullptr;p.main.color=0xffffffff;
        }
    }
    free_head[pool]=p.next;p.next=-1;
}
void pickups_create() {
    free_head[0]=free_head[1]=-1;created=onscreen=cancel_density=cancel_value=0;movement_factor=1;
    for(int i=0;i<int(items.size());++i) {
        items[i]=Pickup{};
        const int pool=i<600?0:1;items[i].next=free_head[pool];free_head[pool]=i;
    }
    popups={};popup_cursor=0;
    bullets=anm_get(7,"bullet.anm");cards=anm_get(31,"abcard.anm");ascii=anm_get(2,"ascii.anm");
    anm_vm_reset(&popup_font);popup_font.anm_index=popup_font.anm_table_index=2;
    anm_set_sprite(&popup_font,ascii,289);popup_font.flags3=(popup_font.flags3&~0x1100000u)|0xe00000u;
    logic=thread_create(update);thread_set_priority(logic,0x1e);thread_enqueue_logic(logic);
    drawing=thread_create(draw);thread_set_priority(drawing,0x21);thread_enqueue_render(drawing);
    popup_logic=thread_create(update_popups);thread_set_priority(popup_logic,0x15);thread_enqueue_logic(popup_logic);
    popup_drawing=thread_create(draw_popups);thread_set_priority(popup_drawing,0x30);thread_enqueue_render(popup_drawing);
}
void pickups_destroy() {
    thread_remove(logic);thread_remove(drawing);thread_remove(popup_logic);thread_remove(popup_drawing);
    for(auto& p:items){anm_vm_free(p.card);p.card=nullptr;p.state=0;}
    for(auto* vm:flashes)anm_vm_free(vm);
    flashes.clear();
}
} // namespace stage
