#include <cstddef>
#include <algorithm>
#include "stage/ecl_vm.h"

#include <cstring>
#include <list>
#include <string>
#include "gameplay/spell.h"
#include "gameplay/pickups.h"

#include "core/thread.h"
#include "presentation/anm.h"
#include "platform/gfx.h"
#include "resources/anm_file.h"
#include "resources/dat.h"
#include "storage/storage.h"
#include "projectiles/laser.h"
#include "projectiles/bullet_types.h"
#include "stage/enemy.h"
#include "stage/stage.h"
#include "stage/session.h"
#include "stage/background.h"
#include "stage/hud.h"
#include "stage/extra_effects.h"
#include "player/player.h"
#include "player/bomb.h"
#include "cards/card_manager.h"
#include "cards/card_passive.h"
#include "presentation/sound.h"
#include "presentation/msg.h"
#include "replay/replay.h"
#include "core/game_session.h"

namespace laser { void clear_lasers(int32_t arg); }  // 定义在文件末尾（原版 0x00449090）
#include "core/rng.h"
#include <cmath>
#include <chrono>
#include "core/input.h"
#include "core/app.h"
#include "presentation/ingame_menu.h"

// ECL 敌机 VM 与三条最小真实实现（P0 编译门禁用）：
//   stage::thread_current  0x0048dbe0
//   stage::op_create_enemy 0x004369e0
//   laser::clear_lasers    0x00449090
// 其余入口（vm_exec 各 case、碰撞、Boss）按 P1 继续补。

namespace stage {

static const EclHostTable& enemy_host_table();

// 原版 DAT_004cf2d0（EnemyManager*）与 DAT_004cccdc+0x24（难度）。
EnemyManager* g_enemy_manager = nullptr;
int32_t g_difficulty = 0;
static EclThread* g_active_thread = nullptr;
struct BulletExtra {float f[4];int32_t i[4];uint32_t type;int32_t async;uint32_t sub;};
struct EnemyBullet {
    float x,y,speed,angle,radius;int type,color;AnmVm* vm;
    BulletExtra extras[24],effects[32];float timers[32],accel[32][2];
    uint32_t effect_flags;int cursor,jump_count,age;bool appearing,removed;
    float vx,vy;
    int cancel_state;bool frozen,grazed;
    int delay;
    float turn_speed,turn_angle;int turn_count,transform_se;
    float saved_speed,saved_angle,saved_x,saved_y;
    int bounce_count;
    int label;
    bool triggered;
    float size_scale=1.0f,move_speed;
    float collision_time=0;
    alignas(4) uint8_t move_interp[0x58],size_interp[0x30];
    int render_bucket;
};
static constexpr int bullet_render_buckets[]={5,5,5,5,3,3,3,3,4,4,4,3,4,4,4,4,3,3,1,1,2,2,2,1,1,2,2,2,2,1,1,3,0,2,5,4,4,3,2,3,3,3,3,2,1,1,0,0};
static std::list<EnemyBullet> g_enemy_bullets;
static std::list<AnmVm*> g_cancel_effects;
static AnmResource* g_bullet_anm;
static AnmResource* g_effect_anm;
static void bullets_update();
static Thread* g_bullet_logic;
static Thread* g_bullet_render;
static ThreadStatus bullets_logic_task(void*) {
    if(stage_session_started() && !shop_active())bullets_update();
    return THREAD_CONTINUE;
}
static ThreadStatus bullets_render_task(void*) {
    gfx_select_camera(3);
    anm_begin_batch();
    for(int bucket=0;bucket<6;++bucket)
        for(auto& bullet:g_enemy_bullets)
            if(bullet.render_bucket==bucket) {
                // 424ef0..424f68: apply auto-facing after the ANM tick, at draw time.
                // The sprite's zero-angle axis is perpendicular to the motion axis.
                if(bullet.vm->flags3&0x200) {
                    float angle=bullet.angle+1.5707963705062866f;
                    int wraps=0;
                    while(angle>3.1415927410125732f) {
                        angle-=6.2831854820251465f;
                        if(wraps++>0x20)break; // Same shared wrap limit as the original.
                    }
                    while(angle< -3.1415927410125732f) {
                        angle+=6.2831854820251465f;
                        if(wraps++>0x20)break;
                    }
                    bullet.vm->flags2|=4;
                    bullet.vm->rot_z=angle;
                }
                anm_draw_vm(bullet.vm);
            }
    anm_end_batch();
    return THREAD_CONTINUE;
}
static void bullet_fire(Enemy& self,int slot);
static void pickup_create(float x,float y,int kind);
static void enemy_defeat(Enemy& self);
static void enemy_drop_items(Enemy& self);
static void reset_drop_timer(Enemy& e) {
    // 42d110/42d180/42d040 share the native drop-bonus clock reset.
    auto* timer=reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+0x62e4);
    if((timer[4]&1)==0){timer[3]=0;timer[4]|=1;}
    timer[0]=-1;timer[1]=0;*reinterpret_cast<float*>(timer+2)=0;
}
static bool enemy_phase_change(Enemy& self);
static bool enemy_time_change(Enemy& self);
static void enemy_restart(Enemy& self,const char* sub);
static AnmResource* enemy_anm_resource(Enemy& self,int index) {
    if(index==0)return g_bullet_anm;
    if(index==1)return g_effect_anm;
    if(index==7)return stage_background_resource();
    return static_cast<ecl::EclResource*>(self.ecl_res)->animations[index-2];
}
// 43a030：模板初始化后设 Enemy 层，再执行第 0 帧并前插普通 ANM 队列。
static void enemy_set_animation(Enemy& self,int slot,int script,int resource_index) {
    if(self.visual[slot])anm_vm_free(self.visual[slot]);
    auto* resource=enemy_anm_resource(self,resource_index);
    auto* vm=anm_vm_alloc(resource);anm_vm_bind_script(vm,resource,script);
    const int layer=self.animation_layer+7;vm->flags3|=0x1000;
    if(layer>=0){vm->draw_layer=layer;if(layer<24)vm->flags3=(vm->flags3&~0x200000u)|0x101000u;}
    anm_vm_update(vm);vm->node_flags=2;anm_queue_add(vm);self.visual[slot]=vm;anm_manage_vm(vm,&self.visual[slot]);
    if(slot==0){self.subpixel_x=vm->sprite_h*vm->scale_y;self.subpixel_y=vm->sprite_w*vm->scale_x;}
    if(self.flag&0x20)vm->flags2&=~2u;
}
static void bullet_extra_start(EnemyBullet& bullet);
static void bullet_extra_update(EnemyBullet& bullet);
static int g_cancel_count,g_graze_cancel_count;
int32_t enemy_count() {return g_enemy_manager->count;}
// 404d10/404d70：原循环共用计数，保留正负 π 两个端点。
static float bullet_turn_angle(float angle) {
    int counter=0;
    while(angle>3.14159265f){angle-=6.283185307f;if(counter++>0x20)break;}
    while(angle<-3.14159265f){angle+=6.283185307f;if(counter++>0x20)break;}
    return angle;
}

static void motion_scalar(Enemy& e,int offset,float from,float to,int duration,int mode) {
    auto* q=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+offset);
    std::memset(q,0,0x30);q[0]=q[4]=from;q[1]=to;
    auto* i=reinterpret_cast<int32_t*>(q);i[5]=-1;i[9]=1;i[10]=duration;i[11]=mode;
}
static void motion_pair(Enemy& e,int offset,const float* from,const float* to,int duration,int mode) {
    auto* q=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+offset);
    std::memset(q,0,0x44);std::memcpy(q,from,8);std::memcpy(q+2,to,8);std::memcpy(q+8,from,8);
    auto* i=reinterpret_cast<int32_t*>(q);i[10]=-1;i[14]=1;i[15]=duration;i[16]=mode;
}
static void motion_position(Enemy& e,int offset,const Motion& motion,float x,float y,int duration,int mode) {
    auto* q=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+offset);
    std::memset(q,0,0x68);q[0]=q[3]=motion.x;q[1]=q[4]=motion.y;q[2]=q[5]=motion.z;
    q[6]=x;q[7]=y;q[8]=0;
    auto* i=reinterpret_cast<int32_t*>(q);i[15]=-1;i[19]=1;i[20]=duration;i[24]=mode;
}
static void position_evaluate(uint8_t* raw,float out[3]) {
    auto* q=reinterpret_cast<float*>(raw);auto* i=reinterpret_cast<int32_t*>(raw);
    const int duration=i[20];i[15]=i[16];i[18]=0;
    q[17]+=g_speed_scale>0.99f && g_speed_scale<1.01f?1.0f:g_speed_scale;i[16]=int32_t(q[17]);
    const bool done=i[16]>=duration;
    if(done){i[16]=duration;i[15]=duration-1;q[17]=float(duration);i[20]=0;i[19]|=1;}
    for(int axis=0;axis<3;++axis) {
        const int mode=(i[25]&1)?i[21+axis]:i[24];
        if(done)q[axis]=mode==7 || mode==17?q[3+axis]:q[6+axis];
        else if(mode==7){q[3+axis]+=q[6+axis];q[axis]=q[3+axis];}
        else if(mode==17){q[3+axis]+=q[12+axis];q[12+axis]+=q[6+axis];q[axis]=q[3+axis];}
        else if(mode==8) {
            const float t=q[17]/float(duration),u=t-1;
            q[axis]=q[3+axis]*u*u*(2*t+1)+q[6+axis]*t*t*(3-2*t)+q[9+axis]*(1-t)*(1-t)*t+q[12+axis]*u*t*t;
        } else {const float t=easing_ratio(mode,q[17],float(duration));q[axis]=q[3+axis]+(q[6+axis]-q[3+axis])*t;}
        out[axis]=q[axis];
    }
}
static void motion_combine(Enemy& e) {
    auto* final=reinterpret_cast<Motion*>(&e.x);
    final->vel_x=e.absolute_motion.x+e.relative_motion.x-e.x;
    final->vel_y=e.absolute_motion.y+e.relative_motion.y-e.y;
    final->unk_03c=e.absolute_motion.z+e.relative_motion.z-e.z;
    e.x+=final->vel_x;e.y+=final->vel_y;e.z+=final->unk_03c;
    if(e.flag&0x20000) {
        auto* bounds=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+0x620c);
        e.x=std::clamp(e.x,bounds[0]-bounds[2]*0.5f,bounds[0]+bounds[2]*0.5f);
        e.y=std::clamp(e.y,bounds[1]-bounds[3]*0.5f,bounds[1]+bounds[3]*0.5f);
        e.absolute_motion.x=e.x-e.relative_motion.x;e.absolute_motion.y=e.y-e.relative_motion.y;
    }
}
static void ellipse_position(Motion& m) {
    const float q=std::remainder(m.angle-m.unk_028,6.283185307f),x=std::cos(q)*m.angle_add*m.unk_02c,y=std::sin(q)*m.angle_add;
    m.x=m.vel_x+x*std::cos(m.unk_028)-y*std::sin(m.unk_028);m.y=m.vel_y+y*std::cos(m.unk_028)+x*std::sin(m.unk_028);m.z=m.unk_03c;
}
static void motion_advance(Enemy& e,Motion& m,int pos,int angle,int speed,int radial,int ellipse) {
    auto* raw=reinterpret_cast<uint8_t*>(&e);
    if((m.type_flags&15)!=2 && (m.type_flags&15)!=3 && *reinterpret_cast<int32_t*>(raw+angle+0x28)!=0)
        m.angle=std::remainder(interp_eval1(raw+angle),6.283185307f);
    if(*reinterpret_cast<int32_t*>(raw+speed+0x28)!=0)m.speed=interp_eval1(raw+speed);
    if(*reinterpret_cast<int32_t*>(raw+radial+0x3c)!=0)interp_eval2(raw+radial,&m.angle_add);
    if(*reinterpret_cast<int32_t*>(raw+ellipse+0x3c)!=0)interp_eval2(raw+ellipse,&m.unk_028);
    if(*reinterpret_cast<int32_t*>(raw+pos+0x50)!=0) {
        float target[3];position_evaluate(raw+pos,target);
        m.vel_x=target[0]-m.x;m.vel_y=target[1]-m.y;m.unk_03c=target[2]-m.z;
    } else if((m.type_flags&15)==0) {
        m.vel_x=std::cos(m.angle)*m.speed*g_speed_scale;m.vel_y=std::sin(m.angle)*m.speed*g_speed_scale;m.unk_03c=0;
    } else if((m.type_flags&15)==2 || (m.type_flags&15)==3) {
        m.angle_add+=m.angle_speed*g_speed_scale;m.angle=std::remainder(m.angle+m.speed*g_speed_scale,6.283185307f);
    }
    if((m.type_flags&15)==0){m.x+=m.vel_x;m.y+=m.vel_y;m.z+=m.unk_03c;}
    else if((m.type_flags&15)==2){m.x=m.vel_x+std::cos(m.angle)*m.angle_add;m.y=m.vel_y+std::sin(m.angle)*m.angle_add;m.z=m.unk_03c;}
    else if((m.type_flags&15)==3)ellipse_position(m);
}

static int32_t vm_read_int(EclThread*, const ecl::InsHeader*, int32_t);
static float vm_read_number(EclThread*, const ecl::InsHeader*, int32_t);
static int32_t* vm_write_int(EclThread*, const ecl::InsHeader*, int32_t);
static float* vm_write_float(EclThread*, const ecl::InsHeader*, int32_t);

namespace {

const char* ins_format(uint16_t id) {
    for (const ecl::InsInfo& info : ecl::kInsTable) {
        if (info.id == id) return info.format;
    }
    return "";
}

// 原版 0x4366f4 的 702 字节表（id-300 → 175 项跳转表组号）。
uint8_t ins_group(uint16_t id) {
    for (const ecl::InsInfo& info : ecl::kInsTable) {
        if (info.id == id) return info.group;
    }
    return ecl::kInsNoGroup;
}

// 第 index 个参数（0 起，名字参数也算一个）的数据指针；越界返回 nullptr。
// 参数布局：指令头 0x10 字节之后。名字参数 = {u32 长度, 内容[长度]}，
// 其后才是 4 字节参数（S int / f float / 其余按 4 字节处理）。
const uint8_t* param_ptr(const ecl::InsHeader* ins, int32_t index, char* type) {
    const char* fmt = ins_format(ins->id);
    const uint8_t* p = reinterpret_cast<const uint8_t*>(ins) + ecl::kSubHeaderSize;
    for (int32_t i = 0; i < ins->param_count && *fmt != '\0'; ++i, ++fmt) {
        const char c = *fmt;
        if (c == 'm' || c == 'x' || c == 'o' || c == 't') {
            if (i == index) {
                *type = c;
                return p;
            }
            p += 4 + *reinterpret_cast<const uint32_t*>(p);
            continue;
        }
        if (i == index) {
            *type = c;
            return p;
        }
        p += 4;
    }
    return nullptr;
}

// 第 index 个参数按 float 读取（S 型转 float）。VM 用。
float vm_param_float(const ecl::InsHeader* ins, int32_t index) {
    char type = 0;
    const uint8_t* p = param_ptr(ins, index, &type);
    if (p == nullptr) return 0.0f;
    if (type == 'f') return *reinterpret_cast<const float*>(p);
    return static_cast<float>(*reinterpret_cast<const int32_t*>(p));
}

}  // namespace

int32_t sub_find(void* ecl_resource, const char* name) {
    auto* base = static_cast<const uint8_t*>(ecl_resource);
    const int32_t count = *reinterpret_cast<const int32_t*>(base + 8);
    auto* entries = *reinterpret_cast<const ecl::SubEntry* const*>(base + 0x8c);
    int32_t lo = 0;
    int32_t hi = count - 1;
    while (lo <= hi) {
        const int32_t mid = (lo + hi) >> 1;
        const int cmp = std::strcmp(entries[mid].name, name);
        if (cmp == 0) return mid;
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return -1;
}

void thread_start(EclThread* thread, void* ecl_resource, const char* sub_name) {
    thread->ecl_res = ecl_resource;
    thread->sub_index = sub_find(ecl_resource, sub_name);
    thread->time = 0.0f;
    thread->offset = 0;
    // 原版线程构造把栈顶放在栈区末端；0x0048b3a0 的 push 先 sp -= 4 再写 thread+0xc+sp。
    thread->stack_ptr = 0;
    thread->stack_ret = 0;
    thread->call_sp = 0;
}

const ecl::InsHeader* thread_current(EclThread* thread) {
    if (thread->sub_index == -1 || thread->offset == -1) return nullptr;
    auto* entries = *reinterpret_cast<const ecl::SubEntry* const*>(
        static_cast<const uint8_t*>(thread->ecl_res) + 0x8c);
    const auto* data = static_cast<const uint8_t*>(entries[thread->sub_index].data);
    return reinterpret_cast<const ecl::InsHeader*>(data + ecl::kSubHeaderSize + thread->offset);
}

int32_t arg_int(Enemy& self, int32_t index) {
    EclThread* t = g_active_thread ? g_active_thread : self.thread;
    return vm_read_int(t, thread_current(t), index);
}
float arg_float(Enemy& self, int32_t index) {
    EclThread* t = g_active_thread ? g_active_thread : self.thread;
    return vm_read_number(t, thread_current(t), index);
}
float arg_float_default(Enemy& self, int32_t index, float def) {
    EclThread* t = g_active_thread ? g_active_thread : self.thread;
    return index < thread_current(t)->param_count ? vm_read_number(t, thread_current(t), index) : def;
}
float arg_float_raw(Enemy& self, int32_t index) { return arg_float(self,index); }

Enemy* create_enemy(const char* sub_name, const SpawnArgs& args) {
    EnemyManager* manager = g_enemy_manager;
    if (manager->max_enemies <= manager->count) return nullptr;

    auto* enemy = new Enemy{};
    for(auto& parent:enemy->animation_parent)parent=-1;
    for(auto& slot:enemy->bullets)slot.type=-1;
    auto* base = reinterpret_cast<uint8_t*>(enemy);
    enemy->self = enemy;
    enemy->vtable = reinterpret_cast<void**>(const_cast<EclHostTable*>(&enemy_host_table()));
    enemy->ecl_res = manager->ecl_resource;
    enemy->thread = new EclThread{};
    enemy->thread->owner = enemy;
    enemy->thread->ecl_res = manager->ecl_resource;
    enemy->id = manager->next_id;
    manager->last_id = enemy->id;
    manager->next_id = enemy->id + 1;
    if (manager->next_id == 0) manager->next_id = 2;

    enemy->hurtbox[0]=enemy->hurtbox[1]=24.0f;
    enemy->hitbox[0]=enemy->hitbox[1]=24.0f;
    enemy->x = args.x;
    enemy->y = args.y;
    *reinterpret_cast<float*>(base + 0x12b4) = args.x;
    *reinterpret_cast<float*>(base + 0x12b8) = args.y;
    *reinterpret_cast<int32_t*>(base + 0x12bc) = args.arg2;
    enemy->score = args.score;
    *reinterpret_cast<float*>(base+0x62f8)=32;*reinterpret_cast<float*>(base+0x62fc)=32;
    reset_drop_timer(*enemy);
    enemy->state6318=-1;
    enemy->health = args.health;
    enemy->drop_kind=args.drop_kind;
    enemy->health_max = args.health;
    enemy->health_previous = args.health;
    enemy->anm_index=2;
    *reinterpret_cast<uint8_t*>(base + 0x1030) = static_cast<uint8_t>(1 << (g_difficulty & 0x1f));

    enemy->flag = (args.flags & 1 ? 0x80000u : 0u) |
                  (args.flags2 & 1 ? 0x4000000u : 0u) |
                  (999 < args.health ? 0x40000000u : 0u);
    for (int i = 0; i < 12; ++i) enemy->var[i] = args.vars[i];
    enemy->parent_id = args.parent_id;

    // 42d7d0 先执行首次 42fe80，ECL323 与槽 0 动画先于默认死亡效果。
    enemy->state631c = 1;
    enemy->state6320 = 2;
    enemy->field6324 = 0x40000000u;
    enemy->state6328 = 0;
    enemy->flags632c |= 1;
    enemy->alpha=1.0f;
    thread_start(enemy->thread, manager->ecl_resource, sub_name);
    enemy->thread->rank_mask = static_cast<uint8_t>(1u << g_difficulty);
    enemy_update_presentation(*enemy);
    enemy->death_se=(enemy->id&1)+3;
    if(enemy->death_script==0) {
        enemy->death_script=0x2c;
        if(enemy->primary_anm_index==2)switch(enemy->primary_anm_script) {
        case 5:case 0x19:case 0x35:case 0x5b:enemy->death_script=0x28;break;
        case 10:case 0x38:case 0x5f:enemy->death_script=0x30;break;
        case 15:case 0x67:enemy->death_script=0x34;break;
        case 0x1e:enemy->death_script=0x3a;break;
        case 0x23:enemy->death_script=0x39;break;
        case 0x28:enemy->death_script=0x38;break;
        }
        enemy->death_resource=1;
    }

    // 链表节点内嵌在 enemy+0x1514：+0x00 self、+0x04 next、+0x08 prev。
    EnemyListLink* node = &enemy->link;
    node->self = enemy;
    node->next = nullptr;
    node->prev = nullptr;
    node->field0c = nullptr;
    if (manager->enemy_head == nullptr) {
        manager->enemy_head = node;
    } else {
        EnemyListLink* tail = manager->enemy_tail;
        if (tail->next != nullptr) {
            node->next = tail->next;
            tail->next->prev = node;
        }
        if (tail->field0c != nullptr) tail->field0c = node;
        tail->next = node;
        node->prev = tail;
    }
    manager->enemy_tail = node;
    ++manager->count;

    return enemy;
}

void op_create_enemy(Enemy& self) {
    EnemyManager* manager = g_enemy_manager;
    if (manager->max_enemies <= manager->count) return;

    const ecl::InsHeader* ins = thread_current(g_active_thread ? g_active_thread : self.thread);
    const uint16_t op = ins->id;

    SpawnArgs args{};
    args.x = arg_float(self, 1);
    args.y = arg_float(self, 2);

    // 相对坐标变体：300/309/321/311/304/312/305 以自身坐标为基准。
    switch (op) {
        case 300:
        case 309:
        case 321:
        case 311:
        case 304:
        case 312:
        case 305:
            args.x += self.x;
            args.y += self.y;
            break;
        default:
            break;
    }
    // 311/304/312/305 额外置相对标志位。
    switch (op) {
        case 311:
        case 304:
        case 312:
        case 305:
            args.flags |= 1;
            break;
        default:
            break;
    }
    // 镜像（0x004369e0：vm+0x5130 的 0x80000 位 → 取反标志、x 乘 DAT_004b9418 = -1.0f）。
    if ((self.flag & 0x80000) != 0) {
        args.flags ^= 1;
        args.x = args.x * -1.0f;
    }

    args.arg2 = 0;
    args.health = arg_int(self, 3);
    args.score = arg_int(self, 4);
    args.drop_kind = arg_int(self, 5);
    for (int i = 0; i < 12; ++i) args.vars[i] = self.var[i];
    args.parent_id = self.id;

    // 名字串在指令 +0x14（+0x10 是名字块长度）。
    const char* name = reinterpret_cast<const char*>(ins) + 0x14;
    create_enemy(name, args);
}

namespace {

ThreadStatus enemy_manager_render_noop(void* ctx) {
    return THREAD_CONTINUE;
}

}  // namespace

// 0x0042dc20：敌机管理器构造。
//   分配 0x1a0（memset 后 *flags |= 2）；next_id = 1 / last_id = 0；
//   分配 0x1098 的 EclResourceInf 并按名字加载 ECL → manager+0x188；
//   A 队线程 0x0042df50 优先级 0x1b；B 队空任务优先级 0x17；
//   +0x98 = -1；+0xa4 = 0；+0xa8 |= 1；+0x8c = 99999；+0x164 = 0。
EnemyManager* create_enemy_manager(const char* ecl_name) {
    auto* manager = new EnemyManager{};
    g_enemy_manager = manager;
    manager->flags = 2;
    manager->next_id = 1;
    manager->last_id = 0;
    manager->max_enemies = 99999;
    manager->field98 = -1;
    manager->field_a4 = 0;
    manager->flags_a8 |= 1;
    manager->ecl_resource = ecl_resource_create(ecl_name);
    g_cancel_count=g_graze_cancel_count=0;
    g_bullet_anm=anm_get(7,"bullet.anm");
    g_effect_anm=anm_get(8,"effect.anm");
    extra_effects_start();
    g_bullet_logic=thread_create(bullets_logic_task);thread_set_priority(g_bullet_logic,0x1d);thread_enqueue_logic(g_bullet_logic);
    g_bullet_render=thread_create(bullets_render_task);thread_set_priority(g_bullet_render,0x26);thread_enqueue_render(g_bullet_render);
    pickups_create();

    manager->task_logic = thread_create(enemy_manager_update);
    thread_set_priority(manager->task_logic, 0x1b);
    thread_set_ctx(manager->task_logic, manager);
    thread_enqueue_logic(manager->task_logic);

    manager->task_render = thread_create(enemy_manager_render_noop);
    thread_set_priority(manager->task_render, 0x17);
    thread_enqueue_render(manager->task_render);
    return manager;
}

// 0x0042d210：销毁敌机管理器（停任务线程、释放全部敌机与 ECL 资源）。
void destroy_enemy_manager(EnemyManager* manager) {
    if (manager == nullptr) return;
    while (manager->enemy_head != nullptr) delete_enemy(manager->enemy_head->self);
    extra_effects_stop();
    thread_remove(g_bullet_logic);thread_remove(g_bullet_render);pickups_destroy();
    for(auto& b:g_enemy_bullets)anm_vm_free(b.vm);g_enemy_bullets.clear();
    for(auto* vm:g_cancel_effects)if(vm)anm_vm_free(vm);g_cancel_effects.clear();
    thread_remove(manager->task_logic);
    thread_remove(manager->task_render);
    ecl_resource_destroy(manager->ecl_resource);
    if (g_enemy_manager == manager) g_enemy_manager = nullptr;
    delete manager;
}

// 0x0042df50：敌机管理器每帧更新（A 队 0x1b）。
//   原版门条件：DAT_004cf2e4（场景更新对象）非空且其 +0xb0 的 bit0/bit1/bit2/bit0x400 全清，
//   且 DAT_004cf2a4（练习对象）为空。端口等 core 层接入场景状态后再补这两个全局判断。
//   每帧：清 +0xac/+0xb0 → 遍历 +0x18c 链表（节点 +0x00 = Enemy，+0x04 = next）：
//   非 0x2000000 标志的敌机调 0x0042fe80 表现刷新并清 +0x40000 位，否则 vtable[5](1) 删除；
//   末尾清 +0xa4，再推进波次计数（+0x98 存旧索引，+0x9c/+0xa0 按 DAT_004ccbf0 步进）。
ThreadStatus enemy_manager_update(void* ctx) {
    if(!stage_session_started())return THREAD_CONTINUE;
    if(shop_active())return THREAD_CONTINUE;
    auto* manager = static_cast<EnemyManager*>(ctx);
    for(auto it=g_cancel_effects.begin();it!=g_cancel_effects.end();)if(*it==nullptr)it=g_cancel_effects.erase(it);else ++it;
    manager->field_ac = 0;
    manager->field_b0 = 0;

    EnemyListLink* node = manager->enemy_head;
    while (node != nullptr) {
        EnemyListLink* next = node->next;
        Enemy* enemy = node->self;
        if ((enemy->flag & 0x2000000u) != 0) {
            delete_enemy(enemy);
        } else {
            const int32_t finished=enemy_update_presentation(*enemy);
            if(finished!=0 || (enemy->flag&0x2000000u)!=0)delete_enemy(enemy);
            else enemy->flag &= ~0x40000u;
        }
        node = next;
    }

    if (manager->field_a4 != 0) manager->field_a4 = 0;

    manager->field98 = manager->wave_index;
    const float speed = g_speed_scale;
    if (speed > 0.99f && speed < 1.01f) {
        manager->wave_index += 1;
        manager->wave_frac += 1.0f;
    } else {
        manager->wave_frac += speed;
        manager->wave_index = static_cast<int32_t>(manager->wave_frac);
    }
    return THREAD_CONTINUE;
}

// 0x0042fe80：敌机每帧表现刷新。已确认的数值部分：fade_time（+0x1510）> 0 时按全局速度推进。
// 16 个表现对象（+0x1350）的 alpha 写入（对象 +0x5c4）与查表 0x00488b40 属于 ANM 运行时
// （A 队 P1），接口接上后在此补 alpha 曲线与 +0x6360 bit0 的显隐切换。
int32_t enemy_update_presentation(Enemy& e) {
    if(e.flag&0x40000)return 0;
    e.flag|=0x40000;
    e.previous_x=e.x;e.previous_y=e.y;
    motion_advance(e,e.absolute_motion,0x1524,0x15f4,0x1624,0x16b4,0x173c);
    motion_advance(e,e.relative_motion,0x158c,0x1654,0x1684,0x16f8,0x1780);
    motion_combine(e);
    // 42e5a0：306 启用的左右姿态，使用移动完成后、ECL 本帧执行前的水平速度。
    if(e.flag&0x100000) {
        const float vx=reinterpret_cast<Motion*>(&e.x)->vel_x;
        const int direction=vx<-0.03f?-1:vx>0.03f?1:0;
        if(direction!=e.primary_anim_state) {
            const int offset=direction<0?1:direction>0?2:e.primary_anim_state<0?3:4;
            float x=0,y=0,z=0;
            if(e.visual[0]){x=e.visual[0]->pos_x;y=e.visual[0]->pos_y;z=e.visual[0]->pos_z;anm_vm_free(e.visual[0]);}
            auto* resource=enemy_anm_resource(e,e.primary_anm_index);
            auto* vm=anm_vm_alloc(resource);anm_vm_bind_script(vm,resource,e.primary_base_script+offset);
            const int layer=e.animation_layer+7;vm->flags3|=0x1000;
            if(layer>=0){vm->draw_layer=layer;if(layer<24)vm->flags3=(vm->flags3&~0x200000u)|0x101000u;}
            vm->pos_x=x;vm->pos_y=y;vm->pos_z=z;vm->rot_z=0;
            anm_vm_update(vm);vm->node_flags=8;anm_queue_add(vm);e.visual[0]=vm;anm_manage_vm(vm,&e.visual[0]);e.primary_anim_state=direction;
        }
    }
    if(script_tree_run(e.thread,g_speed_scale)){enemy_defeat(e);return 1;}
    while(e.pending_sub){const char* sub=e.pending_sub;e.pending_sub=nullptr;enemy_restart(e,sub);}
    if((e.thread->sub_index<0 && e.thread->children.empty()) || (e.flag&0x2000000))return 1;
    if(e.special_callback==1) {
        // 438d90: activate labeled bullets outside the boss circle, near the player.
        const float player_radius=*reinterpret_cast<float*>(&e.var[4]);
        const float boss_radius=*reinterpret_cast<float*>(&e.var[5]);
        for(auto& b:g_enemy_bullets)if(b.label==1) {
            if(!b.triggered) {
                auto* boss=find_enemy(g_enemy_manager->boss_id[0]);
                const float bx=b.x-boss->x,by=b.y-boss->y;
                const float px=b.x-g_player->x,py=b.y-g_player->y;
                if(bx*bx+by*by>=boss_radius*boss_radius && px*px+py*py<=player_radius*player_radius) {
                    b.triggered=true;b.cursor=8;
                }
            } else if(b.cursor==11) {
                const float dx=g_player->x-b.x,dy=g_player->y-b.y;
                b.angle=bullet_turn_angle(dx==0 && dy==0?1.57079637f:std::atan2(dy,dx));
                b.speed=0.01f;b.vx=std::cos(b.angle)*b.speed;b.vy=std::sin(b.angle)*b.speed;
            }
        }
    } else if(e.special_callback==2 || e.special_callback==3) {
        laser::spawn_extra_border_enemies(e.special_callback);
    } else if(e.special_callback==4) {
        // 439320: triggering bullets activate the waiting field's ET8 once.
        const float radius=*reinterpret_cast<float*>(&e.var[5]);
        for(const auto& trigger:g_enemy_bullets)if(trigger.label==1)
            for(auto& target:g_enemy_bullets)if(target.label==0 && !target.triggered) {
                const float dx=trigger.x-target.x,dy=trigger.y-target.y;
                if(dx*dx+dy*dy<radius*radius){target.triggered=true;target.effect_flags=0;target.cursor=8;}
            }
    }
    if((e.flag&0x10000000) && bomb_active() && !(e.flag&0x20000000)) {
        e.primary_base_script=*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+0x6364);
        enemy_set_animation(e,0,e.primary_base_script,e.primary_anm_index);e.flag|=0x20000001;
    } else if(!bomb_active() && (e.flag&0x20000000)) {
        e.primary_base_script=*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+0x6368);
        enemy_set_animation(e,0,e.primary_base_script,e.primary_anm_index);e.flag&=~0x20000001u;
    }
    enemy_time_change(e);
    e.flag&=~0x200000u;
    if((e.flag&0x21u)==0) {
        float pos[3]={e.x,e.y,0},hitpos[3];uint32_t hit=0;
        int damage=0;
        if(e.hurtbox[0]>0) {
            damage=player_bullet_hit_test(pos,(e.flag&0x1000)?e.hurtbox:nullptr,
                (e.flag&0x1000)?0:e.hurtbox[0]*0.5f,&hit,hitpos,0,float(e.id),0);
            damage=int(float(damage) * *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(g_player)+0x47980));
        }
        if(g_player->state==0 || g_player->state==2)damage/=5;
        if(msg_active())damage=0;
        if(hit==0)g_enemy_manager->field_b0+=damage;
        else g_enemy_manager->field_ac+=damage<e.health?damage:e.health+(damage-e.health)/4;
        if(bomb_active() && e.alpha<1){if(damage!=0 && e.alpha<=0)sound_play_se_at(36,e.x);damage=int(float(damage)*e.alpha);}
        if(damage!=0) {
            card_passive_damage_event();
            if((spell_manager()->flags&0x21)==0x21)damage/=30;
            e.damage_accum+=damage;
            if((e.flag&16)==0 && e.state6320<1) {
                if((e.flags6238&1)==0)e.health-=damage;
                else {
                    e.health_scaled-=damage;
                    e.health=(e.health_scaled-e.phase_health_threshold*7)/7+e.phase_health_threshold;
                }
            }
            e.health_previous=e.health;e.phase_health_threshold=0;
            for(const auto& event:e.interrupts)if(event.health>=0){e.health_previous-=event.health;e.phase_health_threshold=event.health;break;}
            if(!enemy_phase_change(e) && e.health<=0 && (e.flag&0x80)==0){enemy_defeat(e);return 1;}
            e.flag|=0x200000;
        }
    }
    if(e.flags6238&2){enemy_defeat(e);return 1;}
    extra_aura_update(e);
    if(e.flag&0x2000000)return 1;
    if((e.flag&0x4000022u)==0 && *reinterpret_cast<const int32_t*>(reinterpret_cast<const uint8_t*>(&e)+0x6334)<1 &&
       (!card_manager_owned(Koishi) || (e.flag&0x800000)!=0)) {
        int collision;
        if((e.flag&0x1000)==0)collision=player_collision_circle(e.x,e.y,e.hitbox[0]*0.5f,false);
        else {
            const float half=e.hitbox[1]*0.5f;
            float x=e.x,y=e.y+half;
            if(e.visual[0]) {
                const float rotation=bullet_turn_angle(e.visual[0]->rot_z+1.57079637f);
                x-=std::sin(rotation)*half;y=e.y+std::cos(rotation)*half;
            }
            const float angle=*reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(&e)+0x134c);
            collision=player_collision_line(x,y,angle,e.hitbox[1],e.hitbox[0],false);
        }
        if((e.flag&0x200) && collision==2 && e.phase_time%6==0)player_graze(g_player->x,g_player->y);
    }
    if(e.x < -320 || e.x>320 || e.y>560 || e.y < -160)return 1;
    if(e.visual[0]) {
        auto* vm=e.visual[0];
        if(e.damage_flash!=0){vm->flags2&=~0xe0000u;--e.damage_flash;}
        else if((e.flag&0x200000)!=0 && (e.flag&0x2000)==0) {
            vm->color_alt=0xff0000ff;vm->flags2=(vm->flags2&~0xc0000u)|0x20000u;e.damage_flash=4;
            int se=e.state6318;
            if(se<0) {
                const uint32_t flags=spell_manager()->flags;
                se=(e.flag&0x40800000)!=0 && (flags&9)!=9 && e.health_previous<((flags&1)?200:900)?35:34;
            }
            sound_play_se_at(se,e.x);
        }
    }
    for(int slot=0;slot<14;++slot)if(auto* vm=e.visual[slot]) {
        vm->extra_x=e.x;vm->extra_y=e.y;vm->extra_z=e.z;
        if(!(e.flag&0x4000000u)) {
            vm->extra_x+=e.animation_offset[slot][0];vm->extra_y+=e.animation_offset[slot][1];vm->extra_z+=e.animation_offset[slot][2];
            const int parent=e.animation_parent[slot];
            if(parent>=0 && e.visual[parent]) {
                vm->extra_x+=e.visual[parent]->pos_x;vm->extra_y+=e.visual[parent]->pos_y;vm->extra_z+=e.visual[parent]->pos_z;
            }
        }
        if(vm->flags3&0x200){auto* motion=reinterpret_cast<Motion*>(&e.x);vm->flags2|=4;vm->rot_z=std::atan2(motion->vel_y,motion->vel_x);}
    }
    if(e.fade_time>0)e.fade_time-=g_speed_scale;
    if(e.state6320>0) {
        e.state631c=e.state6320;e.state6328=0;
        float& remaining=*reinterpret_cast<float*>(&e.field6324);
        remaining-=g_speed_scale>0.99f && g_speed_scale<1.01f?1.0f:g_speed_scale;
        e.state6320=int32_t(remaining);
    }
    e.phase_fraction+=g_speed_scale;e.phase_time=int32_t(e.phase_fraction);
    auto* raw=reinterpret_cast<uint8_t*>(&e);
    *reinterpret_cast<int32_t*>(raw+0x62f0)=0;
    *reinterpret_cast<int32_t*>(raw+0x62e4)=*reinterpret_cast<int32_t*>(raw+0x62e8);
    if(g_speed_scale>0.99f && g_speed_scale<1.01f) {
        ++*reinterpret_cast<int32_t*>(raw+0x62e8);
        *reinterpret_cast<float*>(raw+0x62ec)+=1;
    } else {
        *reinterpret_cast<float*>(raw+0x62ec)+=g_speed_scale;
        *reinterpret_cast<int32_t*>(raw+0x62e8)=int32_t(*reinterpret_cast<float*>(raw+0x62ec));
    }
    return 0;
}

// 00424fe0: fan/ring order and speed layers are read directly from the ECL firing slot.
static void bullet_fire(Enemy& e,int index) {
    const bool fixed=e.fire_target[index][2]>0.9f;
    fire_enemy_bullet_slot(e.bullets[index],(fixed?e.fire_target[index][0]:e.x)+e.fire_offset[index][0],
                          (fixed?e.fire_target[index][1]:e.y)+e.fire_offset[index][1]);
}
void fire_enemy_bullet_slot(const bullet::Slot& slot,float origin_x,float origin_y) {
    const auto* raw=reinterpret_cast<const uint8_t*>(&slot);
    int count=*reinterpret_cast<const int16_t*>(raw+0x46c),layers=*reinterpret_cast<const int16_t*>(raw+0x46e),mode=*reinterpret_cast<const int16_t*>(raw+0x470);
    if(slot.const474&0x20)sound_play_se_at(int(slot.const478),origin_x);
    const float aim=std::atan2(g_player->y-origin_y,g_player->x-origin_x);
    const int type=int(slot.f000),color=int(slot.f004);const BulletType& def=kBulletTypes[type];
    for(int layer=0;layer<layers;layer++)for(int n=0;n<count;n++) {
        float speed=slot.f01c;
        if(layers>1)speed-=(speed-*reinterpret_cast<const float*>(&slot.i020))*float(layer)/float(layers-1);
        float angle=slot.f014;
        switch(mode) {
        case 0:case 1:{float offset=count%2?float((n+1)/2)*slot.f018:(float(n/2)+0.5f)*slot.f018;if(n&1)offset=-offset;angle+=offset;if(mode==0)angle+=aim;break;}
        case 2:case 3:case 4:case 5:angle+=float(n)*6.28318531f/float(count)+slot.f018*layer;if(mode==2||mode==4)angle+=aim;if(mode==4||mode==5)angle+=3.14159265f/float(count);break;
        case 6:case 8:angle+=rng_float_pm1()*slot.f018;if(mode==8)speed=slot.f01c+rng_float01()*(*reinterpret_cast<const float*>(&slot.i020));break;
        case 7:angle+=float(n)*6.28318531f/float(count)+slot.f018*layer;speed=slot.f01c+rng_float01()*(*reinterpret_cast<const float*>(&slot.i020));break;
        }
        AnmVm* vm=anm_vm_alloc(g_bullet_anm);
        const float radius=*reinterpret_cast<const float*>(raw+0x24);
        g_enemy_bullets.push_front(EnemyBullet{origin_x+std::cos(angle)*radius,
            origin_y+std::sin(angle)*radius,speed,angle,def.radius,type,color,vm});
        EnemyBullet& b=g_enemy_bullets.front();
        b.transform_se=slot.type;
        b.render_bucket=bullet_render_buckets[type];
        b.cursor=*reinterpret_cast<const int32_t*>(raw+0x480);
        std::memcpy(b.extras,raw+0x28,sizeof(b.extras));b.vx=std::cos(angle)*speed;b.vy=std::sin(angle)*speed;
        anm_vm_bind_script(vm,g_bullet_anm,def.script);
        vm->callback_object=&b;vm->sprite_callback=1;
        // 424fe0: embedded bullet VMs use playfield coordinates even when
        // their ANM script does not select a draw layer (304).
        vm->flags3=(vm->flags3&~0x200000u)|0x100000u;
        if(b.extras[b.cursor].type==2) {
            const int label=b.extras[b.cursor++].i[0];
            b.appearing=true;b.x-=b.vx*4;b.y-=b.vy*4;
            if(label!=1)anm_set_label(vm,7+label);
        } else anm_set_label(vm,2);
        bullet_extra_start(b);vm->extra_x=b.x;vm->extra_y=b.y;anm_vm_update(vm);
    }
}
int bullet_sprite_for_vm(const AnmVm* vm,int logical) {
    const auto* b=static_cast<const EnemyBullet*>(vm->callback_object);
    return kBulletTypes[b->type].sprite<0?logical:kBulletSprites[b->type][b->color*4+logical];
}
float bullet_scale_for_vm(const AnmVm* vm) {
    return static_cast<const EnemyBullet*>(vm->callback_object)->size_scale;
}

void damage_enemies_in_circle(float x,float y,float radius,int32_t damage) {
    for(EnemyListLink* node=g_enemy_manager->enemy_head;node;node=node->next) {
        Enemy& e=*node->self;
        const float dx=e.x-x,dy=e.y-y;
        if((e.flag&0xc000021u)!=0 || (e.flag&0x2000000u)!=0 || dx*dx+dy*dy>radius*radius)
            continue;
        if((e.flag&16)==0 && e.state6320<1)e.health-=damage;
        if(damage!=0)card_passive_damage_event();
        if(!enemy_phase_change(e) && e.health<=0 && (e.flag&0x80)==0){enemy_defeat(e);e.flag|=0x2000000;}
    }
}
Enemy* find_enemy(int32_t id) {
    for(EnemyListLink* node=g_enemy_manager->enemy_head;node;node=node->next)
        if(node->self->id==id)return node->self;
    return nullptr;
}
void clear_enemy_bullets(float x,float y,float radius,bool money) {
    cancel_enemy_bullets_circle(x,y,radius,money?4:0);
}
static void bullet_cancel(EnemyBullet& b,int kind) {
    cancel_bullet_point(b.x,b.y,kind);
    b.cancel_state=4;b.x+=b.vx*g_speed_scale*0.5f;b.y+=b.vy*g_speed_scale*0.5f;
    anm_interrupt(b.vm,1);
}
// 4297a0: unconditional full-field cancellation includes delayed bullets.
static void clear_all_enemy_bullets() {
    for(auto& b:g_enemy_bullets)if(!b.removed && b.cancel_state!=3) {
        if(b.delay==0)sound_play_se_at(0x47,b.x);
        bullet_cancel(b,0);
    }
}
void cancel_bullet_point(float x,float y,int kind) {
    if(kind!=0 && x+32>-192 && x-32<192 && y+32>0 && y-32<448) {
        const int n=++g_cancel_count;
        if(kind==1)pickup_create(x,y,9);
        else if(kind==4) {
            const int mod=n%5;
            const bool coin=mod==0 || (mod==2 && (rng_next()&1u)==0) || (mod==4 && rng_next()%3u==0);
            pickup_create(x,y,coin?2:9);
        }
    }
}
void laser_cancel_effect(float x,float y,int kind,int type,int color,bool rectangle) {
    cancel_bullet_point(x,y,kind);
    int script=-1;
    if(type<0x12 || type==0x22 || type==0x26)script=209+color*2;
    else if(type>=18 && type<(rectangle?31:32))script=257+color*2;
    else if(type>=31 && type<=33)script=281+color*2;
    if(script>=0) {
        auto* vm=anm_vm_alloc(g_bullet_anm);vm->extra_x=x;vm->extra_y=y;
        anm_vm_set_script(vm,g_bullet_anm,script);anm_queue_add(vm);g_cancel_effects.push_back(vm);anm_manage_vm(vm,&g_cancel_effects.back());
    }
}
void player_graze(float x,float y) {
    auto* graze=reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&g_status)+0x30);
    graze[0]=std::min(graze[0]+1,99999999);graze[1]=std::min(graze[1]+1,99999999);
    auto* vm=anm_vm_alloc(g_effect_anm);vm->extra_x=(x+g_player->x)*0.5f;vm->extra_y=(y+g_player->y)*0.5f;
    anm_vm_set_script(vm,g_effect_anm,24);anm_queue_add(vm);g_cancel_effects.push_back(vm);anm_manage_vm(vm,&g_cancel_effects.back());
    sound_play_se_at(42,x);
}
int kaname_cancel_bullets(float x,float y) {return cancel_enemy_bullets_circle(x,y,18,0);}
void cancel_delayed_enemy_bullets_circle(float x,float y,float radius,int kind) {
    for(auto& b:g_enemy_bullets) {
        const float dx=b.x-x,dy=b.y-y,r=radius+b.radius*0.5f;
        if(b.cancel_state==0 && dx*dx+dy*dy<=r*r)bullet_cancel(b,kind);
    }
}
int cancel_ordinary_bullets_circle(float x,float y,float radius,int kind) {
    int count=0;
    for(auto& b:g_enemy_bullets) {
        const float dx=b.x-x,dy=b.y-y,r=radius+b.radius*0.5f;
        if(b.cancel_state==0 && b.delay==0 && dx*dx+dy*dy<=r*r){bullet_cancel(b,kind);++count;}
    }
    return count;
}
int cancel_enemy_bullets_circle(float x,float y,float radius,int kind) {
    const int count=cancel_ordinary_bullets_circle(x,y,radius,kind);
    return count+laser::cancel_circle(x,y,radius,kind);
}
bool cancel_one_enemy_bullet(float x,float y,float radius,int kind) {
    for(auto& b:g_enemy_bullets) {
        const float dx=b.x-x,dy=b.y-y,r=radius+b.radius*0.5f;
        if(b.cancel_state==0 && b.delay==0 && dx*dx+dy*dy<=r*r){bullet_cancel(b,kind);return true;}
    }
    return false;
}
// 4294b0: straight edges include tangency; the four corner checks are strict.
static bool cancel_rectangle_intersects(float x,float y,float half_width,float half_height,float radius) {
    if (!((half_width+radius<std::fabs(x) || half_height<std::fabs(y)) &&
          (half_width<std::fabs(x) || half_height+radius<std::fabs(y)))) return true;
    const float x0=(x-half_width)*(x-half_width),x1=(x+half_width)*(x+half_width);
    const float y0=(y-half_height)*(y-half_height),y1=(y+half_height)*(y+half_height);
    const float r2=radius*radius;
    return y0+x0<r2 || y0+x1<r2 || y1+x0<r2 || y1+x1<r2;
}
void cancel_ordinary_bullets_rectangle(float x,float y,float width,float height,float angle,int kind) {
    const float c=std::cos(angle),s=std::sin(angle);
    for(auto& b:g_enemy_bullets) {
        if(b.cancel_state!=0 || b.delay!=0)continue;
        const float dx=b.x-x,dy=b.y-y;
        const float radius=b.radius*b.size_scale;
        if(cancel_rectangle_intersects(c*dx+s*dy,c*dy-s*dx,width*0.5f,height*0.5f,radius) &&
           cancel_rectangle_intersects(b.x,b.y-224.0f,192.0f,224.0f,radius))bullet_cancel(b,kind);
    }
}
void cancel_enemy_bullets_rectangle(float x,float y,float width,float height,float angle,int kind) {
    cancel_ordinary_bullets_rectangle(x,y,width,height,angle,kind);
    laser::cancel_rectangle(x,y,width,height,angle);
}
void set_enemy_bullets_frozen(bool frozen) {
    for(auto& b:g_enemy_bullets)b.frozen=frozen;
    laser::freeze(frozen);
}
void psyco_reflect_bullets(float x,float y,int timer) {
    const auto* sht=static_cast<const PlayerSht*>(g_player->sht);
    const auto* instruction=reinterpret_cast<const ShtInstruction*>(sht->sub_offset[22]);
    for(auto& b:g_enemy_bullets) {
        const float dx=b.x-x,dy=b.y-y,r=64+b.radius*0.5f;
        if(b.cancel_state==0 && b.delay==0 && dx*dx+dy*dy<=r*r) {
            const float pos[3]={b.x,b.y,0};
            player_attack_spawn(g_player,instruction,0x1600,float(timer),pos,nullptr,card_manager_ability());
            bullet_cancel(b,0);
        }
    }
}
void miko_flash_cancel() {
    uint32_t index=0;
    for(auto& b:g_enemy_bullets) {
        if(b.delay==0 && index%7==0)bullet_cancel(b,0);
        index+=1+rng_next()%3;
    }
}
static void pickup_create(float x,float y,int kind) {
    float angle=-1.57079637f;if(kind==9)angle+=rng_float_pm1()*0.17453292f;
    const float position[3]={x,y,0};
    spawn_item(kind,position,angle,2.2f,0);
}
static void enemy_drop_items(Enemy& e) {
    const float position[3]={e.x,e.y,e.z};
    if(e.drop_kind!=0)spawn_item(e.drop_kind,position,-1.57079637f,2.2f,0);
    auto* drop=reinterpret_cast<uint8_t*>(&e)+0x623c;
    auto* counts=reinterpret_cast<int32_t*>(drop+4);
    float angle=rng_float_pm1()*3.14159265f;
    const float width=*reinterpret_cast<float*>(drop+0xbc),height=*reinterpret_cast<float*>(drop+0xc0);
    for(int kind=1;kind<=19;++kind) {
        int count=counts[kind-1];
        const int duration=*reinterpret_cast<int32_t*>(drop+0xa4),time=*reinterpret_cast<int32_t*>(drop+0xac);
        const int bonus=*reinterpret_cast<int32_t*>(drop+0x54+(kind-1)*4);
        if(duration!=0 && time<duration && bonus>0)count+=bonus-bonus*time/duration;
        for(int n=0;n<count;++n) {
            const float factor=0.5f+rng_float01()*0.5f;
            const float drop_position[3]={e.x+std::cos(angle)*width*factor,e.y+std::sin(angle)*height*factor,e.z};
            spawn_item(kind,drop_position,-1.57079637f,2.2f,0);
            angle=angle+1.57079637f+rng_float_pm1()*3.14159265f*0.25f;
            if(angle>3.14159265f)angle-=6.283185307f;
            if(angle<-3.14159265f)angle+=6.283185307f;
        }
    }
    card_manager_on_drops(position,counts);
    std::memset(counts,0,0x50);
    e.drop_kind=0;
}
static void enemy_defeat(Enemy& e) {
    // 430240：死亡效果与死亡 ECL 在对象析构前执行，效果不依附于 visual 槽。
    if(e.death_se>=0)sound_play_se_at(e.death_se,e.x);
    if(e.death_script>=0) {
        AnmResource* resource=enemy_anm_resource(e,e.death_resource);
        auto* vm=anm_vm_alloc(resource);anm_vm_bind_script(vm,resource,e.death_script);
        vm->draw_layer=3;vm->flags3=(vm->flags3&~0x200000u)|0x101000u;
        vm->extra_x=e.x;vm->extra_y=e.y;vm->extra_z=e.z;
        const float dx=e.x-e.previous_x,dy=e.y-e.previous_y;
        vm->rot_z=dx*dx+dy*dy<0.040000003f?-1.57079637f:std::atan2(dy,dx);
        anm_vm_update(vm);anm_queue_add(vm);g_cancel_effects.push_back(vm);anm_manage_vm(vm,&g_cancel_effects.back());
    }
    enemy_drop_items(e);
    if(e.death_sub && e.death_sub[0]) {
        const char* sub=e.death_sub;
        enemy_restart(e,sub);e.death_sub=nullptr;
    }
}
static void enemy_restart(Enemy& e,const char* sub) {
    script_tree_destroy(e.thread);e.thread=new EclThread{};e.thread->owner=&e;
    thread_start(e.thread,e.ecl_res,sub);e.thread->rank_mask=uint8_t(1u<<g_difficulty);
    e.phase_time=0;e.phase_fraction=0;
    if(script_tree_run(e.thread,g_speed_scale)){enemy_defeat(e);e.flag|=0x2000000u;}
}
static bool enemy_time_change(Enemy& e) {
    for(auto& interrupt:e.interrupts) {
        if(interrupt.health<0 || interrupt.time<=0)continue;
        if(e.flag&0x800000u) {
            const int remaining=interrupt.time-e.phase_time;
            g_stage->field1b8=remaining/60;
            g_stage->timer_centiseconds=(remaining%60)*100/60;
            if(g_stage->field1b8>99)g_stage->field1b8=g_stage->timer_centiseconds=99;
        }
        if(e.phase_time>=interrupt.time) {
            e.health=interrupt.health;interrupt.health=-1;e.flag|=0x1000000;
            // 42ed40: timeout is a failed capture unless this is a survival spell.
            auto* spell=spell_manager();
            if(!(spell->flags&8)) {
                spell->flags|=0x80;
                if((spell->flags&1) && spell->timer>=60) {
                    spell->bonus=0;spell->flags&=~0x22u;
                } else if((spell->flags&1) && bomb_active())spell->flags|=0x20;
                g_enemy_manager->field44=0;
            } else if((spell->flags&9)==9) {
                e.flag&=~0x1000000u;
                *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&g_status)+0x90)+=e.field6828;
            }
            e.field6828=0;
            const char* sub=interrupt.time_sub;
            enemy_restart(e,sub);return true;
        }
        break;
    }
    return false;
}
static bool enemy_phase_change(Enemy& e) {
    for(auto& interrupt:e.interrupts) {
        if(interrupt.health<0)continue;
        if(e.health<=interrupt.health) {
            if(e.field6828!=0 && e.field6824==g_status.unk_008) {
                *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&g_status)+0x90)+=e.field6828;
                e.field6828=0;
            }
            e.health=interrupt.health;interrupt.health=-1;e.flag&=~0x1000000u;
            enemy_restart(e,interrupt.health_sub);return true;
        }
        break;
    }
    return false;
}
static void bullet_extra_start(EnemyBullet& b) {
    while(b.cursor<24) {
        const BulletExtra x=b.extras[b.cursor];
        if(x.type==0 || (x.async==0 && (b.effect_flags&~0x100u)!=0) || (b.effect_flags&x.type))return;
        int bit=0;while((x.type&(1u<<bit))==0)++bit;
        ++b.cursor;b.effects[bit]=x;b.timers[bit]=0;
        if((x.type==4 || x.type==8 || x.type==0x200000) && b.cursor>1 && b.transform_se>=0)
            sound_play_se_at(b.transform_se,b.x);
        switch(x.type) {
        case 0x10: {
            b.effect_flags|=0x10;b.turn_count=0;b.turn_speed=x.f[1]<=-999990?b.speed:x.f[1];
            const float f=x.f[0];float resolved=f;
            if(f<=-999990)resolved=b.angle;
            else if(f>=999990 && f<1999990)resolved=std::atan2(g_player->y-b.y,g_player->x-b.x)+x.f[2];
            else if(f>=2999990 && f<3999990)resolved=std::atan2(g_player->y-b.y,g_player->x-b.x)+rng_float_pm1()*x.f[2];
            else if(f>=3999990 && f<4999990)resolved=b.angle+rng_float_pm1()*x.f[2];
            else if(f>=4999990){auto* boss=find_enemy(g_enemy_manager->boss_id[0]);resolved=std::atan2(boss->y-b.y,boss->x-b.x);}
            b.turn_angle=resolved;
            if(x.i[2]==2) {
                const float dx=g_player->x-b.saved_x,dy=g_player->y-b.saved_y;
                b.turn_angle=std::remainder((dx==0 && dy==0?1.57079637f:std::atan2(dy,dx))+resolved,6.283185307f);
            }
            else if(x.i[2]==3)b.turn_angle=std::remainder(b.saved_angle+b.turn_angle,6.283185307f);
            else if(x.i[2]==5 || x.i[2]==6)b.turn_angle=rng_float_pm1()*x.f[0];
            else if(x.i[2]==7) {
                const float dx=g_player->x-b.x,dy=g_player->y-b.y;
                b.turn_angle=f<=-999990?b.angle:f<990?std::remainder(f,6.283185307f):(dx==0 && dy==0?1.57079637f:std::atan2(dy,dx));
                b.turn_speed=b.speed+rng_float_pm1()*x.f[1];
            }
            break;
        }
        case 0x800000:b.saved_speed=b.speed;b.saved_angle=b.angle;b.saved_x=b.x;b.saved_y=b.y;break;
        case 0x20000: {
            b.effect_flags|=x.type;b.move_speed=b.speed;
            auto* q=reinterpret_cast<float*>(b.move_interp);auto* clock=reinterpret_cast<int32_t*>(q);
            q[0]=b.x;q[1]=b.y;q[2]=0;
            q[3]=x.f[0]+((x.i[1]&0x100)?b.x:0);q[4]=x.f[1]+((x.i[1]&0x100)?b.y:0);q[5]=0;
            q[6]=q[7]=q[8]=q[9]=q[10]=q[11]=0;
            clock[15]=-1;clock[16]=0;q[17]=0;clock[18]=0;clock[19]=1;clock[20]=x.i[0];clock[21]=x.i[1]&0xff;
            break;
        }
        case 0x400000: {
            b.effect_flags|=x.type;
            auto* q=reinterpret_cast<float*>(b.size_interp);auto* clock=reinterpret_cast<int32_t*>(q);
            q[0]=x.f[0];q[1]=x.f[1];q[2]=q[3]=0;
            clock[5]=-1;clock[6]=0;q[7]=0;clock[8]=0;clock[9]=1;clock[10]=x.i[0];clock[11]=x.i[1];
            break;
        }
        case 0x40:b.effect_flags|=0x40;b.bounce_count=0;break;
        case 0x80000:
            b.effect_flags|=x.type;
            b.accel[bit][0]=std::cos(x.f[0])*x.f[1];b.accel[bit][1]=std::sin(x.f[0])*x.f[1];break;
        case 0x80000000:case 0x4000000:
            if(x.i[0]>0){b.effect_flags|=x.type;b.timers[bit]=float(x.i[0]);}
            break;
        case 0x40000000:b.effect_flags|=x.type;break;
        case 0x8000000: {
            // 4270xx..427530: the following ET holds the rest of the laser arguments.
            if(x.i[0]!=0 && x.i[0]!=1)break;
            const BulletExtra next=b.extras[b.cursor++];
            float angle=x.f[0];
            if(angle<=-999990)angle=b.angle;
            else {
                if(angle>=999990 && angle<1999990) {
                    const float dx=g_player->x-b.x,dy=g_player->y-b.y;
                    angle=dx==0 && dy==0?1.57079637f:std::atan2(dy,dx);
                }
                angle=bullet_turn_angle(angle);
            }
            const float speed=x.f[1]<=-999990?b.speed:x.f[1];
            if(x.i[0]==0) {
                laser::LineArgs a{};a.pos[0]=b.x;a.pos[1]=b.y;a.angle=angle;a.speed=speed;
                a.start_length=x.f[2];a.max_length=x.f[3];a.end_travel=next.f[0];a.width=next.f[1];a.offset=next.f[2];
                a.type=x.i[1];a.color=x.i[2];a.cursor=next.i[2];a.flags=1;
                a.se=next.i[0];a.transform_se=next.i[1];std::memcpy(a.extras,b.extras,sizeof(a.extras));
                laser::create_laser(0,&a);
                if(x.i[3]!=0)bullet_cancel(b,0);
            } else {
                laser::InfiniteArgs a{};a.origin[0]=b.x;a.origin[1]=b.y;a.angle=angle;a.growth=speed;
                a.start_length=x.f[2];a.max_length=x.f[3];a.max_width=next.f[0];a.axis_offset=next.f[1];
                a.delay=next.i[0];a.expand=next.i[1];a.hold=next.i[2];a.shrink=next.i[3];
                a.type=x.i[1];a.color=x.i[2];a.flags=(uint32_t(x.i[3])&0xfdu)|2u;a.cursor=(x.i[3]>>8)&255;
                a.se=18;a.transform_se=-1;std::memcpy(a.extras,b.extras,sizeof(a.extras));laser::create_laser(1,&a);
                if(x.i[3]&0x10000)bullet_cancel(b,0);
            }
            break;
        }
        case 0x1000000: {
            // 425c60: ET spawns at the bullet, carrying the four integer and
            // four float parameters as the new enemy's ECL variables.
            SpawnArgs args{};args.x=b.x;args.y=b.y;args.health=10000;
            std::memcpy(args.vars,x.i,sizeof(x.i));
            std::memcpy(args.vars+4,x.f,sizeof(x.f));
            create_enemy(reinterpret_cast<const char*>(x.sub),args);
            break;
        }
        case 2:b.appearing=true;b.x-=b.vx*4;b.y-=b.vy*4;anm_interrupt(b.vm,7+x.i[0]);break;
        case 0x80:b.delay=x.i[0];break;
        case 0x8000:b.label=x.i[0];break;
        case 0x100:b.effect_flags|=0x100;break;
        case 0x200: {
            b.type=x.i[0];b.color=x.i[1]&0x7fff;const auto& def=kBulletTypes[b.type];b.radius=def.radius;b.render_bucket=bullet_render_buckets[b.type];
            // 425c60 ET200 reloads the template and restores the same
            // coordinate mode as creation, before the next ANM update.
            anm_vm_bind_script(b.vm,g_bullet_anm,def.script);
            b.vm->callback_object=&b;b.vm->sprite_callback=1;
            b.vm->flags3=(b.vm->flags3&~0x200000u)|0x100000u;
            if(x.i[1]&0x8000)anm_set_label(b.vm,2);
            break;
        }
        case 0x400:bullet_cancel(b,0);break;
        case 0x800:sound_play_se_at(x.i[0],b.x);break;
        case 0x2000: {
            // 425c60 consumes the following ET4000 as shot type/color data.
            const BulletExtra next=b.extras[b.cursor++];
            bullet::Slot slot{};slot.f000=next.i[0];slot.f004=next.i[1];slot.type=-1;
            slot.f014=x.f[0]<=-999990?b.angle:
                x.f[0]>=999990 && x.f[0]<1999990?std::atan2(g_player->y-b.y,g_player->x-b.x):bullet_turn_angle(x.f[0]);
            slot.f018=x.f[1];slot.f01c=x.f[2]<=-999990?b.speed:x.f[2];
            std::memcpy(&slot.i020,&x.f[3],4);
            auto* raw=reinterpret_cast<uint8_t*>(&slot);std::memcpy(raw+0x28,b.extras,sizeof(b.extras));
            *reinterpret_cast<int16_t*>(raw+0x46c)=int16_t(x.i[2]);
            *reinterpret_cast<int16_t*>(raw+0x46e)=int16_t(x.i[3]);
            *reinterpret_cast<int16_t*>(raw+0x470)=int16_t(x.i[0]);
            *reinterpret_cast<int32_t*>(raw+0x480)=x.i[1];
            fire_enemy_bullet_slot(slot,b.x,b.y);
            if(next.i[2]!=0)bullet_cancel(b,0);
            break;
        }
        case 0x4000:break;
        case 0x40000:
            if(x.f[0]>=990)b.angle=std::atan2(g_player->y-b.y,g_player->x-b.x)+(x.f[0]-1000);
            else if(x.f[0]>=-990)b.angle=x.f[0];
            if(x.f[1]>=-990)b.speed=x.f[1];
            b.vx=std::cos(b.angle)*b.speed;b.vy=std::sin(b.angle)*b.speed;break;
        case 0x100000:b.vm->flags2=(b.vm->flags2&~0x1e0u)|(x.i[0]==2?0x40u:x.i[0]==1?0x20u:0u);break;
        case 0x2000000:b.render_bucket=x.i[0];break;
        case 0x20000000:b.radius=x.f[0]<0?kBulletTypes[b.type].radius:x.f[0];break;
        case 0x10000:
            if(x.i[1]>0){if(b.jump_count==0){b.jump_count=x.i[1];b.cursor=x.i[0];return;}if(b.jump_count==1){b.jump_count=0;break;}--b.jump_count;}
            b.cursor=x.i[0];return;
        default:
            b.effect_flags|=x.type;
            if(x.type==4 || x.type==0x200000) {
                const float angle=x.f[1]<-999990?b.angle:x.f[1];
                const float acceleration=x.type==4?x.f[0]:(x.f[0]-b.speed)/x.i[0];
                b.accel[bit][0]=std::cos(angle)*acceleration;b.accel[bit][1]=std::sin(angle)*acceleration;
            }
            break;
        }
    }
}
static void bullet_extra_update(EnemyBullet& b) {
    bool completed;
    do {
        if(!(b.effect_flags&0x4000000u))bullet_extra_start(b);
        completed=false;
        // 423e10 order matters when acceleration, homing and drift run together.
        for(int bit:{0,2,21,3,4,6,30,17,19,8,31,26}) {
            const uint32_t type=1u<<bit;if((b.effect_flags&type)==0)continue;
            const BulletExtra& x=b.effects[bit];float& time=b.timers[bit];bool end=false;
            switch(type) {
            case 0x10:
                if(int32_t(time)<x.i[0]) {
                    const float speed=b.speed*(1-time/float(x.i[0]));b.vx=std::cos(b.angle)*speed;b.vy=std::sin(b.angle)*speed;
                    time+=g_speed_scale;
                } else {
                    if(b.transform_se>=0)sound_play_se_at(b.transform_se,b.x);
                    ++b.turn_count;
                    switch(x.i[2]) {
                    case 0:case 5:b.angle+=b.turn_angle;break;
                    case 1:case 6:b.angle=std::atan2(g_player->y-b.y,g_player->x-b.x)+b.turn_angle;break;
                    case 2:case 3:case 4:b.angle=b.turn_angle;break;
                    }
                    if(x.i[2]!=7)b.angle=bullet_turn_angle(b.angle);
                    b.speed=b.turn_speed;time=0;
                    b.vx=std::cos(b.angle)*b.speed;b.vy=std::sin(b.angle)*b.speed;
                    if(b.turn_count>=x.i[1])end=true;else time+=g_speed_scale;
                }
                break;
            case 1:
                if(time>=17)end=true;
                else {const float speed=b.speed+5-time*5*0.0625f;b.vx=std::cos(b.angle)*speed;b.vy=std::sin(b.angle)*speed;time+=g_speed_scale;}
                break;
            case 4:case 0x200000:
                if(time>=x.i[0])end=true;
                else {b.vx+=b.accel[bit][0]*g_speed_scale;b.vy+=b.accel[bit][1]*g_speed_scale;b.speed=std::sqrt(b.vx*b.vx+b.vy*b.vy);b.angle=std::atan2(b.vy,b.vx);time+=g_speed_scale;}
                break;
            case 8:
                if(time>=x.i[0])end=true;
                else {b.speed+=x.f[0]*g_speed_scale;b.angle+=x.f[1]*g_speed_scale;b.vx=std::cos(b.angle)*b.speed;b.vy=std::sin(b.angle)*b.speed;time+=g_speed_scale;}
                break;
            case 0x40000000: {
                // 428460: ease velocity toward a player-relative direction, then derive polar motion.
                if(int32_t(time)>=x.i[0]){end=true;break;}
                const float dx=g_player->x-b.x,dy=g_player->y-b.y;
                const float angle=bullet_turn_angle((dx==0 && dy==0?1.57079637f:std::atan2(dy,dx))+x.f[1]);
                b.vx+=(std::cos(angle)*x.f[0]-b.vx)*x.f[2];
                b.vy+=(std::sin(angle)*x.f[0]-b.vy)*x.f[2];
                b.speed=std::sqrt(b.vx*b.vx+b.vy*b.vy);b.angle=bullet_turn_angle(std::atan2(b.vy,b.vx));
                time+=g_speed_scale>0.99f && g_speed_scale<1.01f?1.0f:g_speed_scale;break;
            }
            case 0x80000:
                // 424349..424407 resets this timer each tick; its expiry mask is also literal.
                if(int32_t(time)<x.i[0]) {
                    b.x+=b.accel[bit][0]*g_speed_scale;b.y+=b.accel[bit][1]*g_speed_scale;time=0;
                } else {b.effect_flags&=~9u;completed=true;}
                break;
            case 0x80000000:case 0x4000000:
                if(int32_t(time)<1)end=true;
                else time-=g_speed_scale>0.99f && g_speed_scale<1.01f?1.0f:g_speed_scale;
                break;
            case 0x100:if(time>=x.i[0])end=true;else time+=g_speed_scale;break;
            case 0x20000: {
                auto* q=reinterpret_cast<float*>(b.move_interp);
                if(time>=x.i[0]){b.x=q[3];b.y=q[4];b.speed=b.move_speed;end=true;}
                else {
                    if(time==0){q[0]=b.x;q[1]=b.y;}
                    float position[3];interp_eval3(b.move_interp,position);
                    b.vx=position[0]-b.x;b.vy=position[1]-b.y;
                    if(std::fabs(b.vx)>0.0001f || std::fabs(b.vy)>0.0001f)b.angle=std::atan2(b.vy,b.vx);
                    time+=g_speed_scale;
                }
                break;
            }
            case 0x40: {
                // 427f40: reflect at the selected sides, count one bounce
                // per update (including a corner), then release the next ET.
                const float width=(x.i[1]&0x20)?x.f[1]:384.0f;
                const float height=(x.i[1]&0x20)?x.f[2]:448.0f;
                if(b.x>-width*0.5f && b.x<width*0.5f &&
                   b.y>224-height*0.5f && b.y<224+height*0.5f)break;
                bool bounced=false;
                if((x.i[1]&0x10)==0) {
                    if((x.i[1]&1) && b.y<224-height*0.5f) {
                        b.angle=bullet_turn_angle(-b.angle);b.y=448-height-b.y;bounced=true;
                    }
                    if((x.i[1]&2) && b.y>=224+height*0.5f) {
                        b.angle=bullet_turn_angle(-b.angle);b.y=448+height-b.y;bounced=true;
                    }
                    if((x.i[1]&8) && b.x>=width*0.5f) {
                        b.angle=bullet_turn_angle(-b.angle-3.14159265f);b.x=width-b.x;bounced=true;
                    }
                    if((x.i[1]&4) && b.x<-width*0.5f) {
                        b.angle=bullet_turn_angle(-b.angle-3.14159265f);b.x=-width-b.x;bounced=true;
                    }
                }
                if(x.f[0]>-990)b.speed=x.f[0];
                b.vx=std::cos(b.angle)*b.speed;b.vy=std::sin(b.angle)*b.speed;
                if(bounced){++b.bounce_count;if(b.transform_se>=0)sound_play_se_at(b.transform_se,b.x);}
                end=b.bounce_count>=x.i[0];break;
            }
            }
            if(end) {
                b.effect_flags&=~type;completed=true;
                if(type!=0x40000000u){b.vx=std::cos(b.angle)*b.speed;b.vy=std::sin(b.angle)*b.speed;}
            }
        }
    } while(completed);
}
static int bullet_player_collision(EnemyBullet& b,bool query) {
    if(b.radius<=0)return 0;
    // 424fe0 sets bit 0x10 on ordinary bullets; ET20000000 changes this radius.
    const int collision=player_collision_circle(b.x,b.y,b.radius*b.size_scale,query);
    if(collision==1) {
        if(b.delay!=0)return collision;
    } else {
        if(collision!=2 || b.grazed)return collision;
        b.grazed=true;
        player_graze(b.x,b.y);
        if(!card_manager_owned(Yuyuko) || ++g_graze_cancel_count%6!=0)return collision;
    }
    b.cancel_state=3;anm_interrupt(b.vm,1);
    return collision;
}

static void bullets_update() {
    for(auto it=g_enemy_bullets.begin();it!=g_enemy_bullets.end();) {
        auto current=it++;auto& b=*current;
        const float timer_step=g_speed_scale>0.99f && g_speed_scale<1.01f?1.0f:g_speed_scale;
        if(b.frozen && b.cancel_state==0 && (!b.appearing || b.age>=8)) {
            bullet_player_collision(b,true);
            b.collision_time+=timer_step;b.age=int(b.collision_time);
            continue;
        }
        // 423e10 advances scaling once per frame, before the ET execution loop.
        if(b.effect_flags&0x400000) {
            b.size_scale=interp_eval1(b.size_interp);
            if(*reinterpret_cast<int32_t*>(b.size_interp+0x28)==0)b.effect_flags&=~0x400000u;
        }
        if(!b.appearing && b.cancel_state==0)bullet_extra_update(b);
        if(b.removed){anm_vm_free(b.vm);g_enemy_bullets.erase(current);continue;}
        if(b.cancel_state==3) {b.x+=b.vx*g_speed_scale*0.5f;b.y+=b.vy*g_speed_scale*0.5f;}
        else if(b.cancel_state==0 && (b.effect_flags&0x4000000)==0) {
            b.x+=b.vx*g_speed_scale*(b.appearing?0.5f:1.0f);b.y+=b.vy*g_speed_scale*(b.appearing?0.5f:1.0f);
        }
        int collision=0;
        if(b.cancel_state==0 && (!b.appearing || b.age>=8))collision=bullet_player_collision(b,false);
        // 423e10 state 2 enters state 1 in this tick after its appearance VM
        // has made var0 nonzero, unless this tick returned a collision hit.
        if(b.appearing && collision!=1 && anm_vm_get_var(b.vm,10000)!=0 && b.cancel_state==0) {
            b.appearing=false;bullet_extra_update(b);
            if((b.effect_flags&0x4000000)==0) {b.x+=b.vx*g_speed_scale;b.y+=b.vy*g_speed_scale;}
            if(!b.removed)bullet_player_collision(b,false);
        }
        if(b.delay!=0)--b.delay;
        b.vm->extra_x=b.x;b.vm->extra_y=b.y;
        anm_vm_update(b.vm);
        const bool ended=b.vm->script_cursor<0 && (b.vm->flags2&1)==0;
        b.collision_time+=timer_step;b.age=int(b.collision_time);
        if(b.cancel_state!=0 && ended)b.removed=true;
        const float half_width=b.vm->sprite_w*b.size_scale*0.5f,half_height=b.vm->sprite_h*b.size_scale*0.5f;
        if(b.removed || (!(b.effect_flags&0x100) && (b.x+half_width<=-192 || b.x-half_width>=192 || b.y+half_height<=-64 || b.y-half_height>=448))){anm_vm_free(b.vm);g_enemy_bullets.erase(current);}
    }
}

// 0x0042d220：从 manager+0x18c 链表摘除并释放（线程对象 + 敌机本体）。
void delete_enemy(Enemy* enemy) {
    extra_aura_free(*enemy);
    EnemyManager* manager = g_enemy_manager;
    EnemyListLink* node = &enemy->link;
    if (node->prev != nullptr) {
        node->prev->next = node->next;
    } else {
        manager->enemy_head = node->next;
    }
    if (node->next != nullptr) node->next->prev = node->prev;
    if (manager->enemy_tail == node) manager->enemy_tail = node->prev;
    if((*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(enemy)+0x6360)&4)==0) {
        --manager->count;
        if(enemy->flag&0x800000u)manager->boss_id[enemy->boss_index]=0;
    }

    for (AnmVm* vm : enemy->visual) if(vm) {
        if(vm->flags3&0x10000000)anm_manage_vm(vm,nullptr);
        else anm_vm_free(vm);
    }
    script_tree_destroy(enemy->thread);
    delete enemy;
}

// 敌机主机的每帧执行入口（原版 0x00430d30 的 vtable[0]）。
static int32_t enemy_exec(void* host);

int32_t vm_exec(void* vm) {
    // 参数 vm = host（原版 = enemy+0x122c）；+0x5588 是 self 指针（enemy+0x67b4）。
    auto* base = static_cast<uint8_t*>(vm) - 0x122c;
    auto* enemy = *reinterpret_cast<Enemy**>(base + 0x67b4);
    return enemy_exec(enemy);
}

}  // namespace stage

namespace stage {// ---------------- ECL 宿主实现（端口） ----------------
// 敌机变量表：437360/437c90；公共 Boss 槽与 Manager 局部变量供各关共用。
static int32_t enemy_var_int(void* host, int32_t id) {
    Enemy& e = *static_cast<Enemy*>(host);
    auto* base = reinterpret_cast<const uint8_t*>(&e);
    switch (id) {
        case -10000: return static_cast<int32_t>(rng_next() & 0x7fffffffu);
        case -9999: return static_cast<int32_t>(rng_float01());
        case -9997: return static_cast<int32_t>(e.x);
        case -9996: return static_cast<int32_t>(e.y);
        case -9995: return static_cast<int32_t>(*reinterpret_cast<const float*>(base + 0x12b4));
        case -9994: return static_cast<int32_t>(*reinterpret_cast<const float*>(base + 0x12b8));
        case -9993: return static_cast<int32_t>(*reinterpret_cast<const float*>(base + 0x12f8));
        case -9992: return static_cast<int32_t>(*reinterpret_cast<const float*>(base + 0x12fc));
        case -9988: return e.phase_time;
        case -9987: return static_cast<int32_t>(rng_float_pm1());
        case -9986: return (e.flag>>24)&1;
        case -9907: return g_status.spell_id;
        case -9959: return g_difficulty;
        case -9954: return e.health;
        case -9958: return int32_t(std::atan2(reinterpret_cast<const Motion*>(&e.x)->vel_y,reinterpret_cast<const Motion*>(&e.x)->vel_x));
        case -9898: return int32_t(g_enemy_bullets.size());
        case -9957: return 1;
        case -9953:case -9952:case -9951:case -9950:return g_difficulty==id+9953;
        case -9949:return g_enemy_manager->field3c;
        case -9948:return g_enemy_manager->field40;
        case -9947:return g_enemy_manager->field44;
        case -9946:return g_enemy_manager->count;
        case -9945:return g_player_character+g_player_shot_type;
        default:
            if(id>=-9985 && id<=-9978){const int i=id+9985;return i<4?e.var[i]:int32_t(*reinterpret_cast<float*>(&e.var[i]));}
            if(id>=-9926 && id<=-9923)return reinterpret_cast<int32_t*>(g_enemy_manager)[3+id+9926];
            if(id>=-9922 && id<=-9915)return int32_t(reinterpret_cast<float*>(g_enemy_manager)[7+id+9922]);
            if(id>=-9935 && id<=-9932)return int32_t(*reinterpret_cast<const float*>(base+0x14d8+4*(id+9935)));
            if(id>=-9943 && id<=-9936) {
                Enemy* boss=find_enemy(g_enemy_manager->boss_id[0]);
                if(boss) {const int i=id+9943;return i<4?boss->var[i]:int32_t(*reinterpret_cast<float*>(&boss->var[i]));}
            }
            return 0;
    }
}

static float enemy_var_float(void* host,int32_t id) {
    Enemy& e=*static_cast<Enemy*>(host);
    switch(id) {
    case -10000:return float(rng_next()&0x7fffffffu);case -9999:return rng_float01();case -9998:return rng_float_pm1()*3.14159265f;
    case -9997:case -9977:return e.x;case -9996:case -9976:return e.y;
    case -9991:case -9965:return g_player->x;case -9990:case -9964:return g_player->y;
    case -9989:return std::atan2(g_player->y-e.y,g_player->x-e.x);
    case -9987:return rng_float_pm1();case -9959:return float(g_difficulty);case -9954:return float(e.health);
    case -9975:case -9995:return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+0x12b4);
    case -9974:case -9994:return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+0x12b8);
    case -9973:case -9993:return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+0x12f8);
    case -9972:case -9992:return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+0x12fc);
    case -9963:case -9962:{Enemy* boss=find_enemy(g_enemy_manager->boss_id[0]);return boss?(id==-9963?boss->x:boss->y):(id==-9963?0.0f:128.0f);}
    case -9944:return std::hypot(e.x-g_player->x,e.y-g_player->y);
    case -9958:return std::atan2(reinterpret_cast<const Motion*>(&e.x)->vel_y,reinterpret_cast<const Motion*>(&e.x)->vel_x);
    default:
        if(id>=-9985 && id<=-9978){int i=id+9985;return i<4?float(e.var[i]):*reinterpret_cast<float*>(&e.var[i]);}
        if(id>=-9922 && id<=-9915)return reinterpret_cast<float*>(g_enemy_manager)[7+id+9922];
        if(id>=-9935 && id<=-9932)return *reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+0x14d8+4*(id+9935));
        if(id>=-9939 && id<=-9936){Enemy* boss=find_enemy(g_enemy_manager->boss_id[0]);return boss?*reinterpret_cast<float*>(&boss->var[4+id+9939]):0.0f;}
        return float(enemy_var_int(host,id));
    }
}

static int32_t* enemy_var_ptr(void* host, int32_t id) {
    Enemy& e = *static_cast<Enemy*>(host);
    auto* base = reinterpret_cast<uint8_t*>(&e);
    switch (id) {
        case -9997: return reinterpret_cast<int32_t*>(&e.x);
        case -9996: return reinterpret_cast<int32_t*>(&e.y);
        case -9995: return reinterpret_cast<int32_t*>(base + 0x12b4);
        case -9994: return reinterpret_cast<int32_t*>(base + 0x12b8);
        case -9985: return &e.var[0];
        case -9984: return &e.var[1];
        case -9983: return &e.var[2];
        case -9982: return &e.var[3];
        case -9981: return &e.var[4];
        case -9980: return &e.var[5];
        case -9979: return &e.var[6];
        case -9978: return &e.var[7];
        case -9949:return &g_enemy_manager->field3c;
        case -9948:return &g_enemy_manager->field40;
        case -9947:return &g_enemy_manager->field44;
        default:
            if(id>=-9926 && id<=-9923)return reinterpret_cast<int32_t*>(g_enemy_manager)+3+id+9926;
            if(id>=-9943 && id<=-9940){Enemy* boss=find_enemy(g_enemy_manager->boss_id[0]);return &(boss?boss:&e)->var[id+9943];}
            return nullptr;
    }
}

static float* enemy_var_float_ptr(void* host, int32_t id) {
    auto* base=static_cast<uint8_t*>(host);
    switch(id) {
    case -9977:case -9997:return reinterpret_cast<float*>(base+0x1270);
    case -9976:case -9996:return reinterpret_cast<float*>(base+0x1274);
    case -9975:case -9995:return reinterpret_cast<float*>(base+0x12b4);
    case -9974:case -9994:return reinterpret_cast<float*>(base+0x12b8);
    case -9973:case -9993:return reinterpret_cast<float*>(base+0x12f8);
    case -9972:case -9992:return reinterpret_cast<float*>(base+0x12fc);
    }
    if(id>=-9935 && id<=-9932)return reinterpret_cast<float*>(base+0x14d8)+id+9935;
    if(id>=-9922 && id<=-9915)return reinterpret_cast<float*>(g_enemy_manager)+7+id+9922;
    if(id>=-9939 && id<=-9936){Enemy* boss=find_enemy(g_enemy_manager->boss_id[0]);return reinterpret_cast<float*>(&(boss?boss:static_cast<Enemy*>(host))->var[4+id+9939]);}
    return reinterpret_cast<float*>(enemy_var_ptr(host, id));
}

static int32_t enemy_exec(void* host) {
    auto& e=*static_cast<Enemy*>(host);const ecl::InsHeader* ins=thread_current(g_active_thread?g_active_thread:e.thread);
    const int op=ins->id;
    if(ins_group(op)==6) {
        // 430d30 case 6: 309..312 consume the spawn without creating an
        // enemy while boss slot 0 resolves; the stage script keeps its time.
        if(!find_enemy(g_enemy_manager->boss_id[0]))op_create_enemy(e);
        return 0;
    }
    if(ins_group(op)==0) {op_create_enemy(e);return 0;}
    switch(op) {
    case 302:e.anm_index=arg_int(e,0);break;
    case 303: {
        const int slot=arg_int(e,0),script=arg_int(e,1);
        if(e.visual[slot]){anm_vm_free(e.visual[slot]);e.visual[slot]=nullptr;}
        if(script>=0){enemy_set_animation(e,slot,script,e.anm_index);if(slot==0){e.primary_anm_index=e.anm_index;e.primary_anm_script=script;}}
        break;
    }
    case 306: {
        int slot=arg_int(e,0),script=arg_int(e,1);enemy_set_animation(e,slot,script,e.anm_index);
        if(slot==0){e.flag|=0x100000;e.primary_anm_index=e.anm_index;e.primary_anm_script=e.primary_base_script=script;e.primary_anim_state=0;}
        break;
    }
    case 307:case 308:case 338: {
        // 430d30 case5 -> 488770: no Enemy position, original script layer,
        // one initial update, then prepend to the ordinary ANM queue.
        AnmResource* resource=enemy_anm_resource(e,arg_int(e,0));
        AnmVm* vm=anm_vm_alloc(resource);
        anm_vm_bind_script(vm,resource,arg_int(e,1));
        vm->flags3|=0x1000u;
        if(op!=308) {
            vm->extra_x=e.x+(op==338?arg_float(e,2):0);
            vm->extra_y=e.y+(op==338?arg_float(e,3):0);
            vm->extra_z=e.z;
        }
        vm->rot_z=op==338?arg_float(e,4):0.0f;
        anm_vm_update(vm);
        vm->node_flags=2;
        anm_queue_add(vm);
        g_cancel_effects.push_back(vm);
        anm_manage_vm(vm,&g_cancel_effects.back());
        break;
    }
    case 313:enemy_set_animation(e,arg_int(e,0),e.primary_base_script+5,e.anm_index);break;
    case 316: {
        const int offset=arg_int(e,1);
        enemy_set_animation(e,arg_int(e,0),e.primary_base_script+(offset<0?0:offset+5),e.anm_index);break;
    }
    case 319: {
        // 436cf0: direct Z rotation of an existing enemy animation slot.
        auto* vm=e.visual[arg_int(e,0)];
        if(vm){vm->rot_z=arg_float(e,1);vm->flags2|=4;}
        break;
    }
    case 317: {
        if(auto* vm=e.visual[arg_int(e,0)])anm_set_label(vm,int16_t(arg_int(e,1)));
        break;
    }
    case 320: {
        const int slot=arg_int(e,0);
        e.animation_offset[slot][0]=arg_float(e,1);e.animation_offset[slot][1]=arg_float(e,2);e.animation_offset[slot][2]=0;
        break;
    }
    case 322:e.animation_parent[arg_int(e,0)]=arg_int(e,1);break;
    case 326:case 328: {
        const int slot=arg_int(e,0);auto* vm=e.visual[slot];
        if(vm==nullptr)break; // 436cf0 ignores an animation slot whose VM has ended.
        const int duration=arg_int(e,1),mode=arg_int(e,2);
        auto* raw=reinterpret_cast<uint8_t*>(vm)+(op==326?0xe8:0x140);
        auto* q=reinterpret_cast<float*>(raw);auto* ints=reinterpret_cast<int32_t*>(raw);
        if(op==326) {
            q[0]=float(vm->color&0xff);q[1]=float((vm->color>>8)&0xff);q[2]=float((vm->color>>16)&0xff);
            q[5]=float(arg_int(e,3)&0xff);q[4]=float(arg_int(e,4)&0xff);q[3]=float(arg_int(e,5)&0xff);
            q[6]=q[7]=q[8]=q[9]=q[10]=q[11]=0;
            ints[15]=-1;ints[16]=0;q[17]=0;ints[18]=0;ints[19]|=1;ints[20]=duration;ints[21]=mode;
        } else {
            q[0]=float(vm->color>>24);q[1]=float(arg_int(e,3)&0xff);q[2]=q[3]=0;
            ints[5]=-1;ints[6]=0;q[7]=0;ints[8]=0;ints[9]|=1;ints[10]=duration;ints[11]=mode;
        }
        break;
    }
    case 318:
        enemy_set_animation(e,0,e.primary_base_script,e.anm_index);e.flag&=~0x100000u;
        e.primary_anm_script=e.primary_base_script;e.primary_anim_state=0;e.primary_anm_index=e.anm_index;break;
    case 323:e.death_resource=arg_int(e,0);e.death_script=arg_int(e,1);break;
    case 334: {
        const int effect=arg_int(e,0);
        if(effect==4)extra_maple_spawn(enemy_anm_resource(e,3));
        break;
    }
    case 552:e.animation_layer=arg_int(e,0);break;
    case 400:case 402: {
        auto& m=op==400?e.absolute_motion:e.relative_motion;
        const float x=arg_float(e,0),y=arg_float(e,1);
        if(x>-999999)m.x=x;if(y>-999999)m.y=y;m.type_flags&=~15u;motion_combine(e);break;
    }
    case 401:case 403: {
        auto& m=op==401?e.absolute_motion:e.relative_motion;const int offset=op==401?0x1524:0x158c,duration=arg_int(e,0);
        if(duration<=0)*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+offset+0x50)=0;
        else {
            const float x=arg_float(e,2),y=arg_float(e,3);
            motion_position(e,offset,m,x<=-999999?m.x:x,y<=-999999?m.y:y,duration,arg_int(e,1));m.type_flags&=~15u;
        }
        break;
    }
    case 404:case 406: {
        auto& m=op==404?e.absolute_motion:e.relative_motion;float angle=arg_float(e,0);const float speed=arg_float(e,1);
        if(angle>-999999){if(e.flag&0x80000)angle=std::remainder(3.14159265f-angle,6.283185307f);m.angle=angle;}
        if(speed>-999999)m.speed=speed;m.type_flags&=~15u;break;
    }
    case 424:e.flag=(e.flag&~0x80000u)|((uint32_t(arg_int(e,0))<<19)&0x80000u);break;
    case 408:case 410: {
        auto& m=op==408?e.absolute_motion:e.relative_motion;
        const float angle=arg_float(e,0),speed=arg_float(e,1),radius=arg_float(e,2),growth=arg_float(e,3);
        if((m.type_flags&15)!=2){m.vel_x=m.x;m.vel_y=m.y;m.unk_03c=m.z;}
        if(angle>-999999)m.angle=angle;
        if(speed>-999999)m.speed=speed;
        if(radius>-999999)m.angle_add=radius;
        if(growth>-999999)m.angle_speed=growth;
        m.type_flags=(m.type_flags&~15u)|2;
        m.x=m.vel_x+std::cos(m.angle)*m.angle_add;m.y=m.vel_y+std::sin(m.angle)*m.angle_add;m.z=m.unk_03c;
        motion_combine(e);break;
    }
    case 409:case 411: {
        // 430d40 group 0x1b: interpolate orbital speed and (radius, radius growth).
        auto& m=op==409?e.absolute_motion:e.relative_motion;
        const int s=op==409?0x1624:0x1684,r=op==409?0x16b4:0x16f8;
        const float speed=arg_float(e,2),radius=arg_float(e,3),growth=arg_float(e,4);
        const int duration=arg_int(e,0),mode=arg_int(e,1);
        if(duration>0) {
            const float from[2]={m.angle_add,m.angle_speed};
            const float to[2]={radius<=-999999?m.angle_add:radius,growth<=-999999?m.angle_speed:growth};
            motion_scalar(e,s,m.speed,speed<=-999999?m.speed:speed,duration,mode);
            motion_pair(e,r,from,to,duration,mode);
            m.type_flags=(m.type_flags&~13u)|2;
            m.x=m.vel_x+std::cos(m.angle)*m.angle_add;m.y=m.vel_y+std::sin(m.angle)*m.angle_add;m.z=m.unk_03c;
            motion_combine(e);
        } else {
            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+s+0x28)=0;
            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+r+0x3c)=0;
        }
        break;
    }
    case 425:case 426: {
        auto& m=op==425?e.absolute_motion:e.relative_motion;
        const int offset=op==425?0x1524:0x158c;
        const float x=arg_float(e,3),y=arg_float(e,4);
        motion_position(e,offset,m,x<=-999999?m.x:x,y<=-999999?m.y:y,arg_int(e,0),8);
        auto* q=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+offset);
        q[9]=arg_float(e,1);q[10]=arg_float(e,2);q[12]=arg_float(e,5);q[13]=arg_float(e,6);
        m.type_flags&=~15u;break;
    }
    case 427: {
        auto& absolute=e.absolute_motion;auto& relative=e.relative_motion;
        absolute.x+=relative.x;absolute.y+=relative.y;absolute.z+=relative.z;
        relative.x=relative.y=relative.z=0;
        absolute.speed=relative.speed=0;
        absolute.vel_x=absolute.vel_y=absolute.unk_03c=0;
        relative.vel_x=relative.vel_y=relative.unk_03c=0;
        absolute.type_flags&=~15u;relative.type_flags&=~15u;
        auto* raw=reinterpret_cast<uint8_t*>(&e);
        for(int offset:{0x1524,0x158c})*reinterpret_cast<int32_t*>(raw+offset+0x50)=0;
        for(int offset:{0x15f4,0x1624,0x1654,0x1684})*reinterpret_cast<int32_t*>(raw+offset+0x28)=0;
        for(int offset:{0x16b4,0x16f8,0x173c,0x1780})*reinterpret_cast<int32_t*>(raw+offset+0x3c)=0;
        break;
    }
    case 405:case 407: {
        auto& m=op==405?e.absolute_motion:e.relative_motion;const int a=op==405?0x15f4:0x1654,s=a+0x30,duration=arg_int(e,0);
        if(duration<=0){*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+a+0x28)=0;*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+s+0x28)=0;}
        else {
            const int mode=arg_int(e,1);float from=std::remainder(m.angle,6.283185307f),angle=arg_float(e,2),speed=arg_float(e,3);
            if(angle<=-999999)angle=mode==7?0:from;
            else if(e.flag&0x80000)angle=mode==7?-angle:std::remainder(3.14159265f-angle,6.283185307f);
            if(std::fabs(from-angle)>=3.14159265f){if(angle<=from)angle+=6.283185307f;else from+=6.283185307f;}
            if(speed<=-999999)speed=mode==7?0:m.speed;
            motion_scalar(e,a,from,angle,duration,mode);motion_scalar(e,s,m.speed,speed,duration,mode);m.type_flags&=~15u;
        }
        break;
    }
    case 412:case 413: {
        auto& m=op==412?e.absolute_motion:e.relative_motion;const int a=op==412?0x15f4:0x1654,s=a+0x30;
        const auto* b=reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(&e)+0x620c);
        float angle;
        if(e.x<b[0]-b[2]*0.25f)angle=rng_float_pm1()*3.14159265f/3;
        else if(e.x>b[0]+b[2]*0.25f)angle=std::remainder(rng_float_pm1()*3.14159265f/3+3.14159265f,6.283185307f);
        else {
            angle=rng_float_pm1()*3.14159265f*0.25f;const uint32_t n=rng_next16()%3;
            if((g_player->x<=e.x && n!=0) || (g_player->x>e.x && n==0))angle+=3.14159265f;
        }
        if(e.y<b[1]-b[3]*0.25f)angle=std::fabs(angle);else if(e.y>b[1]+b[3]*0.25f)angle=-std::fabs(angle);
        if(std::fabs(angle+1.57079637f)<0.05f)angle=angle>=-1.57079637f?-1.52079637f:-1.62079637f;
        else if(std::fabs(angle-1.57079637f)<0.05f)angle=angle>=1.57079637f?1.62079637f:1.52079637f;
        const int duration=arg_int(e,0),old_speed_mode=*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+s+0x2c);
        motion_scalar(e,a,angle,angle,duration,arg_int(e,1));motion_scalar(e,s,arg_float(e,2),0,duration,old_speed_mode);m.type_flags&=~15u;break;
    }
    case 420:case 422: {
        auto& m=op==420?e.absolute_motion:e.relative_motion;
        if((m.type_flags&15)!=2){m.vel_x=m.x;m.vel_y=m.y;m.unk_03c=m.z;}
        float* fields[]={&m.angle,&m.speed,&m.angle_add,&m.angle_speed,&m.unk_028,&m.unk_02c};
        for(int n=0;n<6;++n){const float value=arg_float(e,n);if(value>-999999)*fields[n]=value;}
        m.type_flags=(m.type_flags&~15u)|3;ellipse_position(m);motion_combine(e);break;
    }
    case 421:case 423: {
        auto& m=op==421?e.absolute_motion:e.relative_motion;const int s=op==421?0x1624:0x1684,r=op==421?0x16b4:0x16f8,p=op==421?0x173c:0x1780;
        const int duration=arg_int(e,0);
        if(duration<=0){*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+s+0x28)=0;*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+r+0x3c)=0;*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+p+0x3c)=0;}
        else {
            const int mode=arg_int(e,1);float values[5];const float old[]={m.speed,m.angle_add,m.angle_speed,m.unk_028,m.unk_02c};
            for(int n=0;n<5;++n){const float value=arg_float(e,n+2);values[n]=value<=-999999?old[n]:value;}
            motion_scalar(e,s,m.speed,values[0],duration,mode);motion_pair(e,r,old+1,values+1,duration,mode);motion_pair(e,p,old+3,values+3,duration,mode);
            m.vel_x=m.x;m.vel_y=m.y;m.unk_03c=m.z;m.type_flags=(m.type_flags&~15u)|3;ellipse_position(m);motion_combine(e);
        }
        break;
    }
    case 441:case 443: {
        auto& m=op==441?e.absolute_motion:e.relative_motion;const int offset=op==441?0x15f4:0x1654,duration=arg_int(e,0);
        if(duration<=0)*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+offset+0x28)=0;
        else {
            const int mode=arg_int(e,1);const float from=std::remainder(m.angle,6.283185307f);float target=arg_float(e,2);
            if(target<=-999999)target=mode==7?0:from;else if(e.flag&0x80000)target=mode==7?-target:std::remainder(3.14159265f-target,6.283185307f);
            target=from+std::remainder(target-from,6.283185307f);motion_scalar(e,offset,from,target,duration,mode);m.type_flags&=~15u;
        }
        break;
    }
    case 445:case 447: {
        auto& m=op==445?e.absolute_motion:e.relative_motion;const int offset=op==445?0x1624:0x1684,duration=arg_int(e,0);
        if(duration<=0)*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+offset+0x28)=0;
        else {const int mode=arg_int(e,1);float target=arg_float(e,2);if(target<=-999999)target=mode==7?0:m.speed;motion_scalar(e,offset,m.speed,target,duration,mode);m.type_flags&=~15u;}
        break;
    }
    case 500:e.hurtbox[0]=arg_float(e,0);e.hurtbox[1]=arg_float(e,1);break;
    case 501:e.hitbox[0]=arg_float(e,0);e.hitbox[1]=arg_float(e,1);break;
    case 502:
        e.flag|=arg_int(e,0);if(e.flag&0x20)for(auto* vm:e.visual)if(vm)vm->flags2&=~2u;break;
    case 503:
        e.flag&=~arg_int(e,0);if((e.flag&0x20)==0)for(auto* vm:e.visual)if(vm)vm->flags2|=2u;break;
    case 504: {
        auto* bounds=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e)+0x620c);for(int n=0;n<4;++n)bounds[n]=arg_float(e,n);e.flag|=0x20000;break;
    }
    case 505:e.flag&=~0x20000u;break;
    case 506: {
        auto* drop=reinterpret_cast<uint8_t*>(&e)+0x623c;
        std::memset(drop+4,0,0x50);*reinterpret_cast<int32_t*>(drop+0xa4)=0;
        reset_drop_timer(e);break;
    }
    case 507:*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+0x623c+arg_int(e,0)*4)=arg_int(e,1);break;
    case 508:{auto* drop=reinterpret_cast<uint8_t*>(&e)+0x623c;*reinterpret_cast<float*>(drop+0xbc)=arg_float(e,0);*reinterpret_cast<float*>(drop+0xc0)=arg_float(e,1);break;}
    case 509:
        // 430d30 class36 skips ordinary drops in Spell Practice (42a990).
        if(session().mode==GameMode::SpellPractice ||
            (replay_mode()==ReplayMode::Playback && (replay_info().mode_flags&2)!=0))break;
        [[fallthrough]];
    case 562:enemy_drop_items(e);break; // class67: explicit dropItemsSp is unconditional.
    case 510:e.drop_kind=arg_int(e,0);break;
    case 511:
        e.health=e.health_max=e.health_previous=arg_int(e,0);
        e.health_scaled=e.health*7;
        if(e.flag&0x800000u)e.flag|=0x40000000u;
        break;
    case 513:e.phase_time=0;e.phase_fraction=0;break;
    case 512: {
        const int slot=arg_int(e,0);
        g_enemy_manager->flags88&=~1u;
        if(slot<0) {
            if(e.flag&0x800000u)g_enemy_manager->boss_id[e.boss_index]=0;
            e.flag&=~0x800000u;
        } else {
            e.flag|=0x800000u;g_enemy_manager->boss_id[slot]=e.id;e.boss_index=slot;
        }
        break;
    }
    case 514: {
        auto& event=e.interrupts[arg_int(e,0)];event.health=arg_int(e,1);event.time=arg_int(e,2);
        event.health_sub=event.time_sub=reinterpret_cast<const char*>(ins)+32;break;
    }
    case 515:
        e.state631c=-1;e.state6320=arg_int(e,0);
        *reinterpret_cast<float*>(&e.field6324)=float(e.state6320);
        e.state6328=0;e.flags632c|=1;break;
    case 516:sound_play_se_at(arg_int(e,0),e.x);break;
    case 517:stage_screen_shake(arg_int(e,0),arg_int(e,1),arg_int(e,2));break;
    case 518: {
        int entry=arg_int(e,0);
        if(session().stage==6 && entry==0 && replay_mode()!=ReplayMode::Playback && card_manager_owned(Blank))entry=4;
        msg_start(entry);g_stage->dialog_active=msg_active();
        clear_all_enemy_bullets();laser::clear_lasers(0);
        [[fallthrough]]; // 430d40 also performs the 525 enemy cleanup.
    }
    case 525: {
        // 430710: clear vulnerable enemies and scripted familiars without
        // their normal drops; boss/hidden controllers survive unless bit100.
        for(auto* node=g_enemy_manager->enemy_head;node;) {
            auto* next=node->next;auto& target=*node->self;
            if((target.flag&0xc004a0u)==0 || (target.flag&0x100)) {
                auto* raw=reinterpret_cast<uint8_t*>(&target);
                std::memset(raw+0x6240,0,0xc0);target.drop_kind=0;
                *reinterpret_cast<float*>(raw+0x62f8)=*reinterpret_cast<float*>(raw+0x62fc)=32;
                *reinterpret_cast<int32_t*>(raw+0x62e4)=-1;
                *reinterpret_cast<uint32_t*>(raw+0x62f4)=1;
                target.previous_x=0;target.previous_y=192;target.field6828=0;
                enemy_defeat(target);
                for(auto*& vm:target.visual)if(vm) {
                    if(vm->flags3&0x10000000)anm_manage_vm(vm,nullptr);
                    else anm_vm_free(vm);
                    vm=nullptr;
                }
                target.flag|=0x2000000;
            }
            node=next;
        }
        auto* manager=g_enemy_manager;manager->field_a4=0;manager->field98=manager->wave_index;
        manager->wave_frac+=g_speed_scale;manager->wave_index=int32_t(manager->wave_frac);break;
    }
    case 519:if(!msg_wait_released())return -1;break;
    case 520:for(int i=0;i<3;++i)if(find_enemy(g_enemy_manager->boss_id[i]))return -1;break;
    case 522:case 537:case 538:case 539: {
        const uint32_t size=*reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(ins)+0x1c);
        std::string name(reinterpret_cast<const char*>(ins)+0x20,size);uint8_t key=0x77,step=7;
        for(char& c:name){c=char(uint8_t(c)^key);key+=step;step+=0x10;}
        int id=arg_int(e,0);if(op>=537)id+=g_difficulty-(op-537);
        spell_start(id,name.c_str(),arg_int(e,1),arg_int(e,2));
        e.flags6238|=1;e.health_scaled=e.health*7;break;
    }
    case 523: {
        auto* spell=spell_manager();
        if((spell->flags&3)==3 && card_manager_owned(Kiseru))spawn_item(4,&e.x,-1.57079637f,2.2f,60);
        spell_end();e.flags6238&=~1u;break;
    }
    case 521:e.interrupts[arg_int(e,0)].time_sub=reinterpret_cast<const char*>(ins)+24;break;
    case 524: {
        const int wave=arg_int(e,0);stage_session_set_wave(wave);e.field6824=wave;e.field6828=0;break;
    }
    case 527:hud_boss_segment(e.boss_index,arg_int(e,0),arg_float(e,1)/float(e.health_max),uint32_t(arg_int(e,2)));break;
    case 540:hud_boss_spells(arg_int(e,0));break;
    case 545:laser::clear_phase_lasers();break;
    case 542:spell_manager()->flags|=8;break;
    case 546: {
        const int invulnerable=arg_int(e,0);auto* raw=reinterpret_cast<uint8_t*>(&e);
        e.flag=(e.flag&~0x30000001u)|((uint32_t(invulnerable)<<28)&0x10000000u);
        *reinterpret_cast<int32_t*>(raw+0x6364)=arg_int(e,1);
        *reinterpret_cast<int32_t*>(raw+0x6368)=e.primary_base_script;break;
    }
    case 547:g_speed_scale=arg_float(e,0);break;
    case 548: {
        const int delay[4]={arg_int(e,0),arg_int(e,1),arg_int(e,2),arg_int(e,3)};
        // 42aa20 maps Extra to the fourth difficulty-dependent value.
        g_active_thread->time-=float(delay[g_difficulty<3?g_difficulty:3]);break;
    }
    case 554: {
        if(session().mode!=GameMode::SpellPractice && session().mode!=GameMode::Replay) {
            auto* resource=static_cast<AnmResource*>(g_stage->logo_anm);
            // 441ed0 -> 405bf0: ordinary queued VM; the script selects its origin.
            auto* vm=anm_vm_alloc(resource);vm->flags3|=0x1000u;
            anm_vm_set_script(vm,resource,0);anm_queue_add(vm);
            g_cancel_effects.push_back(vm);anm_manage_vm(vm,&g_cancel_effects.back());
        }
        break;
    }
    case 573:*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+0x628c+arg_int(e,0)*4)=arg_int(e,1);break;
    case 569: {
        int& count=*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&g_status)+0x8c);
        if(e.field6828==0){e.field6828=arg_int(e,0);if(e.field6828>0)count+=e.field6828;}
        else if(e.field6828==1){e.field6828=arg_int(e,0);count+=e.field6828-1;}
        break;
    }
    case 568:e.flags6238=(e.flags6238&~1u)|(uint32_t(arg_int(e,0))&1u);break;
    case 570:
        if(e.field6828!=0 && e.field6824==g_status.unk_008) {
            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&g_status)+0x90)+=e.field6828;e.field6828=0;
        }
        break;
    case 574: {
        auto* raw=reinterpret_cast<uint8_t*>(&e);
        *reinterpret_cast<int32_t*>(raw+0x62e0)=arg_int(e,0);
        reset_drop_timer(e);break;
    }
    case 613:clear_all_enemy_bullets();laser::clear_lasers(1);break;
    case 535: {int values[4];for(int i=0;i<4;i++)values[i]=arg_int(e,i+1);*vm_write_int(g_active_thread,ins,0)=values[g_difficulty<3?g_difficulty:3];break;}
    case 536: {float values[4];for(int i=0;i<4;i++)values[i]=arg_float(e,i+1);*vm_write_float(g_active_thread,ins,0)=values[g_difficulty<3?g_difficulty:3];break;}
    case 556:e.death_sub=reinterpret_cast<const char*>(ins)+20;break;
    case 566:return 1; // 430f52: die -> 430240, after unwinding the executing ECL tree.
    case 800: {
        Enemy* target=find_enemy(g_enemy_manager->boss_id[arg_int(e,0)]);
        if(target)target->pending_sub=reinterpret_cast<const char*>(ins)+24;
        break;
    }
    case 802: {
        const int slot=arg_int(e,0);
        for(int i=0;i<3;++i)if(i!=e.boss_index) {
            Enemy* target=find_enemy(g_enemy_manager->boss_id[i]);
            if(target && target->interrupts[slot].health>=0)target->pending_sub=target->interrupts[slot].health_sub;
        }
        break;
    }
    case 600:{int i=arg_int(e,0);std::memset(&e.bullets[i],0,sizeof(bullet::Slot));e.bullet_extra_cursor[i]=0;auto* raw=reinterpret_cast<uint8_t*>(&e.bullets[i]);*reinterpret_cast<int16_t*>(raw+0x46c)=1;*reinterpret_cast<int16_t*>(raw+0x46e)=1;e.bullets[i].f01c=2.0f;e.bullets[i].const474=0x23;e.bullets[i].const478=21;e.bullets[i].type=38;e.fire_offset[i][0]=e.fire_offset[i][1]=0;e.fire_target[i][0]=e.fire_target[i][1]=e.fire_target[i][2]=0;break;}
    case 601:bullet_fire(e,arg_int(e,0));break;
    case 602:{int i=arg_int(e,0);e.bullets[i].f000=arg_int(e,1);e.bullets[i].f004=arg_int(e,2);break;}
    case 603:{int i=arg_int(e,0);e.fire_offset[i][0]=arg_float(e,1);e.fire_offset[i][1]=arg_float(e,2);break;}
    case 604:{int i=arg_int(e,0);e.bullets[i].f014=arg_float(e,1);e.bullets[i].f018=arg_float(e,2);break;}
    case 605:{int i=arg_int(e,0);e.bullets[i].f01c=arg_float(e,1);*reinterpret_cast<float*>(&e.bullets[i].i020)=arg_float(e,2);break;}
    case 606:{int i=arg_int(e,0);auto* raw=reinterpret_cast<uint8_t*>(&e.bullets[i]);*reinterpret_cast<int16_t*>(raw+0x46c)=int16_t(arg_int(e,1));*reinterpret_cast<int16_t*>(raw+0x46e)=int16_t(arg_int(e,2));break;}
    case 607:{int i=arg_int(e,0);auto* raw=reinterpret_cast<uint8_t*>(&e.bullets[i]);*reinterpret_cast<int16_t*>(raw+0x470)=int16_t(arg_int(e,1));break;}
    case 608:{auto& slot=e.bullets[arg_int(e,0)];slot.const478=uint32_t(arg_int(e,1));slot.type=arg_int(e,2);break;}
    case 700: {
        auto* raw=reinterpret_cast<uint8_t*>(&e.bullets[arg_int(e,0)]);
        for(int i=0;i<3;++i)*reinterpret_cast<float*>(raw+8+4*i)=arg_float(e,i+1);
        *reinterpret_cast<float*>(raw+0x448)=arg_float(e,4);break;
    }
    case 701: {
        auto* raw=reinterpret_cast<uint8_t*>(&e.bullets[arg_int(e,0)]);
        for(int i=0;i<5;++i)*reinterpret_cast<int32_t*>(raw+0x458+4*i)=arg_int(e,i+1);break;
    }
    case 702:case 703:case 711:case 713: {
        const int index=arg_int(e,0);const auto& slot=e.bullets[index];const auto* raw=reinterpret_cast<const uint8_t*>(&slot);
        const bool fixed=e.fire_target[index][2]>0.9f;
        const float x=(fixed?e.fire_target[index][0]:e.x)+e.fire_offset[index][0],y=(fixed?e.fire_target[index][1]:e.y)+e.fire_offset[index][1];const float angle=std::remainder(slot.f014,6.283185307f);
        const float width=*reinterpret_cast<const float*>(raw+0x448),offset=*reinterpret_cast<const float*>(raw+0x24);
        const auto flags=*reinterpret_cast<const uint32_t*>(raw+0x468);const int se=int(slot.const478),transform=slot.type;
        if(op==702) {
            laser::LineArgs a{};a.pos[0]=x;a.pos[1]=y;a.angle=angle;
            a.type=slot.f000;a.color=slot.f004;a.start_length=*reinterpret_cast<const float*>(raw+8);a.max_length=*reinterpret_cast<const float*>(raw+0xc);a.end_travel=*reinterpret_cast<const float*>(raw+0x10);
            a.width=width;a.speed=slot.f01c;a.offset=offset;a.flags=flags|1;a.se=se;a.transform_se=transform;std::memcpy(a.extras,raw+0x28,sizeof(a.extras));laser::create_laser(0,&a);
        } else if(op==703) {
            laser::InfiniteArgs a{};a.origin[0]=x;a.origin[1]=y;a.angle=angle;a.max_length=*reinterpret_cast<const float*>(raw+0xc);a.start_length=*reinterpret_cast<const float*>(raw+8);
            a.max_width=width;a.growth=slot.f01c;a.delay=*reinterpret_cast<const int32_t*>(raw+0x458);a.expand=*reinterpret_cast<const int32_t*>(raw+0x45c);a.hold=*reinterpret_cast<const int32_t*>(raw+0x460);a.shrink=*reinterpret_cast<const int32_t*>(raw+0x464);
            a.type=slot.f000;a.color=slot.f004;a.axis_offset=offset;a.flags=flags|2;a.se=se;a.transform_se=transform;a.label=arg_int(e,1);
            std::memcpy(a.extras,raw+0x28,sizeof(a.extras));laser::create_laser(1,&a);
        } else if(op==711) {
            laser::CurveArgs a{};a.pos[0]=x;a.pos[1]=y;a.angle=angle;a.width=width;a.speed=slot.f01c;a.style=slot.f000;a.color=slot.f004;
            a.node_count=*reinterpret_cast<const int32_t*>(raw+0x458);a.offset=offset;a.flags|=1;a.se=se;a.transform_se=transform;std::memcpy(a.extras,raw+0x28,sizeof(a.extras));laser::create_laser(2,&a);
        } else {
            uint8_t args[0x468]{};*reinterpret_cast<float*>(args)=x;*reinterpret_cast<float*>(args+4)=y;*reinterpret_cast<float*>(args+0x18)=angle;
            *reinterpret_cast<float*>(args+0x20)=*reinterpret_cast<const float*>(raw+0x10);*reinterpret_cast<float*>(args+0x24)=width;
            *reinterpret_cast<int32_t*>(args+0x28)=slot.f004;*reinterpret_cast<float*>(args+0x2c)=offset;*reinterpret_cast<int32_t*>(args+0x30)=*reinterpret_cast<const int32_t*>(raw+0x458);*reinterpret_cast<int32_t*>(args+0x38)=arg_int(e,1);
            std::memcpy(args+0x3c,raw+0x28,0x420);laser::create_laser(3,args);
        }break;
    }
    case 704:laser::set_origin(arg_int(e,0),arg_float(e,1),arg_float(e,2));break;
    case 705:laser::set_position_step(arg_int(e,0),arg_float(e,1),arg_float(e,2));break;
    case 706:if(auto* l=laser::find(arg_int(e,0)))l->speed=arg_float(e,1);break;
    case 707:if(auto* l=laser::find(arg_int(e,0)))l->width=arg_float(e,1);break;
    case 708:if(auto* l=laser::find(arg_int(e,0)))l->angle=arg_float(e,1);break;
    case 709:laser::set_angle_step(arg_int(e,0),arg_float(e,1));break;
    case 710:laser::clear_tag(arg_int(e,0));break;
    case 712:cancel_ordinary_bullets_rectangle(e.x,e.y,arg_float(e,0),arg_float(e,1),e.visual[0]->rot_z,1);break;
    case 714:laser::set_beam_flag(arg_int(e,0),arg_int(e,1));break;
    case 609:case 610:case 611:case 612: {
        const int slot=arg_int(e,0);int arg=1;
        const int index=op>=611?e.bullet_extra_cursor[slot]:arg_int(e,arg++);
        auto* x=reinterpret_cast<BulletExtra*>(reinterpret_cast<uint8_t*>(&e.bullets[slot])+0x28)+index;
        x->async=arg_int(e,arg++);x->type=uint32_t(arg_int(e,arg++));
        x->i[0]=arg_int(e,arg++);x->i[1]=arg_int(e,arg++);
        if(op==610||op==612){x->i[2]=arg_int(e,arg++);x->i[3]=arg_int(e,arg++);}
        x->f[0]=arg_float(e,arg++);x->f[1]=arg_float(e,arg++);
        if(op==610||op==612){x->f[2]=arg_float(e,arg++);x->f[3]=arg_float(e,arg++);}
        e.bullet_extra_cursor[slot]=index+1;break;
    }
    case 614:{const int to=arg_int(e,0),from=arg_int(e,1);e.bullets[to]=e.bullets[from];e.bullet_extra_cursor[to]=e.bullet_extra_cursor[from];std::memcpy(e.fire_offset[to],e.fire_offset[from],sizeof(e.fire_offset[to]));std::memcpy(e.fire_target[to],e.fire_target[from],sizeof(e.fire_target[to]));break;}
    case 615:case 616: {
        const float radius=arg_float(e,0);const int kind=op==615?1:0;
    // 429250 ignores ordinary-bullet immunity; 449010 respects laser ET80.
        for(auto& b:g_enemy_bullets) {
            const float dx=b.x-e.x,dy=b.y-e.y,r=radius+b.radius*0.5f;
            if(b.cancel_state==0 && dx*dx+dy*dy<=r*r)bullet_cancel(b,kind);
        }
        laser::cancel_circle(e.x,e.y,radius,kind,1);break;
    }
    case 624:{int i=arg_int(e,0),rank=g_difficulty<3?g_difficulty:3;float values[8];for(int n=0;n<8;n++)values[n]=arg_float(e,n+1);e.bullets[i].f01c=values[rank];*reinterpret_cast<float*>(&e.bullets[i].i020)=values[4+rank];break;}
    case 625:{int i=arg_int(e,0),rank=g_difficulty<3?g_difficulty:3;int values[8];for(int n=0;n<8;n++)values[n]=arg_int(e,n+1);auto* raw=reinterpret_cast<uint8_t*>(&e.bullets[i]);*reinterpret_cast<int16_t*>(raw+0x46c)=int16_t(values[rank]);*reinterpret_cast<int16_t*>(raw+0x46e)=int16_t(values[4+rank]);break;}
    case 626: {
        const int slot=arg_int(e,0);const float angle=arg_float(e,1),radius=arg_float(e,2);
        e.fire_offset[slot][0]=std::cos(angle)*radius;e.fire_offset[slot][1]=std::sin(angle)*radius;break;
    }
    case 627:*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(&e.bullets[arg_int(e,0)])+0x24)=arg_float(e,1);break;
    case 628: {
        const int slot=arg_int(e,0);e.fire_target[slot][0]=arg_float(e,1);e.fire_target[slot][1]=arg_float(e,2);
        e.fire_target[slot][2]=e.fire_target[slot][0]>=-990.0f?1.0f:0.0f;break;
    }
    case 630:stage_background_interrupt(arg_int(e,0));break;
    case 629:extra_aura_set(e,arg_float(e,0),uint32_t(arg_int(e,1)));break;
    case 632:
        e.special_callback=arg_int(e,0);
        *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(&e)+0x6818)=0;break;
    case 637: {
        const int effect=arg_int(e,0);
        if(effect==2 || effect==3)laser::spawn_extra_border_enemies(effect);
        break;
    }
    case 640: {
        const int slot=arg_int(e,0),index=arg_int(e,1);
        auto* extra=reinterpret_cast<BulletExtra*>(reinterpret_cast<uint8_t*>(&e.bullets[slot])+0x28)+index;
        extra->sub=reinterpret_cast<uint32_t>(reinterpret_cast<const char*>(ins)+0x1c);
        break;
    }
    }
    return 0;
}

static const EclHostTable kEnemyHost = {
    enemy_exec, enemy_var_int, enemy_var_ptr, enemy_var_float, enemy_var_float_ptr,
};

static const EclHostTable& enemy_host_table() { return kEnemyHost; }

struct StackValue { uint32_t bits; bool floating; };
static StackValue stack_pop(EclThread* t) {
    t->stack_ptr -= 8;
    return {*reinterpret_cast<uint32_t*>(t->stack+t->stack_ptr+4), t->stack[t->stack_ptr]=='f'};
}
static float stack_float(StackValue v) { return v.floating ? *reinterpret_cast<float*>(&v.bits) : float(int32_t(v.bits)); }
static int32_t stack_int(StackValue v) { return v.floating ? int32_t(*reinterpret_cast<float*>(&v.bits)) : int32_t(v.bits); }
static void stack_push_int(EclThread* t,int32_t v) {
    t->stack[t->stack_ptr]='i'; *reinterpret_cast<int32_t*>(t->stack+t->stack_ptr+4)=v; t->stack_ptr+=8;
}
static void stack_push_float(EclThread* t,float v) {
    t->stack[t->stack_ptr]='f'; *reinterpret_cast<float*>(t->stack+t->stack_ptr+4)=v; t->stack_ptr+=8;
}
static int32_t vm_read_int(EclThread* t,const ecl::InsHeader* ins,int32_t index) {
    char type=0;const uint8_t* p=param_ptr(ins,index,&type);
    int32_t v=type=='f' ? int32_t(*reinterpret_cast<const float*>(p)) : *reinterpret_cast<const int32_t*>(p);
    if((ins->param_mask & (1u<<index))==0) return v;
    if(v>=0) return *reinterpret_cast<int32_t*>(t->stack+t->stack_ret+v);
    if(v>=-100) {
        const int offset=t->stack_ptr+v*8;
        return stack_int({*reinterpret_cast<uint32_t*>(t->stack+offset+4),t->stack[offset]=='f'});
    }
    auto* host=*reinterpret_cast<EclHostTable**>(t->owner);return host->var_int(t->owner,v);
}
static float vm_read_number(EclThread* t,const ecl::InsHeader* ins,int32_t index) {
    char type=0;const uint8_t* p=param_ptr(ins,index,&type);
    if(type!='f') return float(vm_read_int(t,ins,index));
    float v=*reinterpret_cast<const float*>(p);
    if((ins->param_mask & (1u<<index))==0) return v;
    if(v>=0) return *reinterpret_cast<float*>(t->stack+t->stack_ret+int32_t(v));
    if(v>=-100) {
        const int offset=t->stack_ptr+int32_t(v)*8;
        return stack_float({*reinterpret_cast<uint32_t*>(t->stack+offset+4),t->stack[offset]=='f'});
    }
    auto* host=*reinterpret_cast<EclHostTable**>(t->owner);return host->var_float(t->owner,int32_t(v));
}
static float vm_read_float(EclThread* t,const ecl::InsHeader* ins,int32_t index) { return vm_read_number(t,ins,index); }
static int32_t* vm_write_int(EclThread* t,const ecl::InsHeader* ins,int32_t index) {
    char type=0;const uint8_t* p=param_ptr(ins,index,&type);
    int32_t v=type=='f'?int32_t(*reinterpret_cast<const float*>(p)):*reinterpret_cast<const int32_t*>(p);
    if(v>=0) return reinterpret_cast<int32_t*>(t->stack+t->stack_ret+v);
    auto* host=*reinterpret_cast<EclHostTable**>(t->owner);return host->var_ptr(t->owner,v);
}
static float* vm_write_float(EclThread* t,const ecl::InsHeader* ins,int32_t index) {return reinterpret_cast<float*>(vm_write_int(t,ins,index));}

static void call_arguments(EclThread* from,const ecl::InsHeader* ins,EclThread* to,int skip) {
    const auto* data=reinterpret_cast<const uint8_t*>(ins)+16;
    const uint8_t* p=data+4+*reinterpret_cast<const uint32_t*>(data)+skip*4;
    std::vector<uint32_t> arguments;
    for(int i=1+skip;i<ins->param_count;++i,p+=8) {
        const bool floating=p[0]=='f'||p[0]=='g';
        uint32_t bits=*reinterpret_cast<const uint32_t*>(p+4);
        StackValue value{bits,floating};
        if(ins->param_mask&(1u<<i)) {
            const int id=floating?int(*reinterpret_cast<float*>(&bits)):int32_t(bits);
            if(id>=0)value.bits=*reinterpret_cast<uint32_t*>(from->stack+from->stack_ret+id);
            else if(id>=-100)value=stack_pop(from);
            else {
                auto* table=*reinterpret_cast<EclHostTable**>(from->owner);
                if(floating){float f=table->var_float(from->owner,id);value={*reinterpret_cast<uint32_t*>(&f),true};}
                else value={uint32_t(table->var_int(from->owner,id)),false};
            }
        }
        if(p[1]=='f'){float f=stack_float(value);arguments.push_back(*reinterpret_cast<uint32_t*>(&f));}
        else arguments.push_back(uint32_t(stack_int(value)));
    }
    for(size_t i=0;i<arguments.size();++i)*reinterpret_cast<uint32_t*>(to->stack+to->stack_ptr+i*4)=arguments[i];
}

// 角度归一到 (-π, π]：0x0048b3a0 的 case 0x51/0x52/0x5e 共用。
static float normalize_angle(float angle) {
    constexpr float kPi = 3.14159265358979f;
    constexpr float k2Pi = 6.28318530717959f;
    for (int i = 0; i < 0x20 && angle > kPi; ++i) angle -= k2Pi;
    for (int i = 0; i < 0x20 && angle < -kPi; ++i) angle += k2Pi;
    return angle;
}

// 0x0048b3a0：脚本 VM。只有 id < 0x5f 的指令在本函数里执行；其余指令转调
// owner 的 vtable[0]（EnemyInf 为 0x00430d30 的 adjustor thunk，ECX = owner+0x122c）。
// 需要的第一关指令：0（空操作）/1（结束）/0x16（空操作）/0x17（时间回退 = 等待 N 帧）/
// 0x57（两点夹角写变量，待补）/ 其余高号全部转调 0x00430d30。
int32_t script_vm_run(EclThread* t,float delta) {
    auto* resource=static_cast<ecl::EclResource*>(t->ecl_res);
    while(t->sub_index>=0 && t->offset>=0) {
        const ecl::InsHeader* ins=thread_current(t);
        if(float(ins->time)>t->time) break;
        if((t->rank_mask & ins->rank_mask)==0) { t->offset+=ins->size;continue; }
        bool jump=false;
        switch(ins->id) {
        case 0:case 22:case 30:case 31:break;
        case 1:t->sub_index=-1;t->offset=-1;return -1;
        case 10:
            if(t->call_sp==0) {t->sub_index=-1;t->offset=-1;return -1;}
            --t->call_sp;t->sub_index=t->call_sub[t->call_sp];t->offset=t->call_off[t->call_sp];
            t->time=t->call_time[t->call_sp];t->stack_ptr=t->call_stack[t->call_sp];t->stack_ret=t->call_base[t->call_sp];jump=true;break;
        case 11: {
            call_arguments(t,ins,t,0);
            const int n=t->call_sp++;
            t->call_sub[n]=t->sub_index;t->call_off[n]=t->offset+ins->size;t->call_time[n]=t->time;
            t->call_stack[n]=t->stack_ptr;t->call_base[n]=t->stack_ret;
            t->sub_index=sub_find(t->ecl_res,reinterpret_cast<const char*>(ins)+20);t->offset=0;t->time=0;jump=true;break;
        }
        case 12:case 13:case 14: {
            bool taken=ins->id==12;
            if(ins->id!=12) {const bool yes=stack_int(stack_pop(t))!=0;taken=ins->id==13?!yes:yes;}
            if(taken) {const auto* v=reinterpret_cast<const int32_t*>(ins+1);t->offset+=v[0];t->time=float(v[1]);jump=true;}
            break;
        }
        case 15:case 16: {
            auto* child=new EclThread{};child->owner=t->owner;thread_start(child,t->ecl_res,reinterpret_cast<const char*>(ins)+20);
            child->id=ins->id==16?float(vm_read_int(t,ins,1)):-1;
            call_arguments(t,ins,child,ins->id==16?1:0);
            child->rank_mask=t->rank_mask;t->children.push_back(child);break;
        }
        case 17:case 18:case 19:case 20: {
            const float id=float(vm_read_int(t,ins,0));
            std::vector<EclThread*> pending{t->owner->thread};
            for(size_t i=0;i<pending.size();++i) {
                EclThread* child=pending[i];for(auto* c:child->children)pending.push_back(c);
                if(child->id!=id)continue;
                if(ins->id==17){child->offset=child->sub_index=-1;}
                else if(ins->id==18)child->flags1204|=1;
                else if(ins->id==19)child->flags1204&=~1u;
                else child->field101c=vm_read_number(t,ins,1);
                break;
            }
            break;
        }
        case 21:for(auto* child:t->owner->thread->children){child->offset=child->sub_index=-1;}break;
        case 23:t->time-=float(vm_read_int(t,ins,0));break;
        case 24:t->time-=vm_read_number(t,ins,0);break;
        case 40:{const int old=t->stack_ptr;t->stack_ptr+=vm_read_int(t,ins,0);*reinterpret_cast<int32_t*>(t->stack+t->stack_ptr)=t->stack_ret;t->stack_ptr+=4;t->stack_ret=old;break;}
        case 41:{t->stack_ptr-=4;const int frame=t->stack_ret;t->stack_ret=*reinterpret_cast<int32_t*>(t->stack+t->stack_ptr);t->stack_ptr=frame;break;}
        case 42:{int v=vm_read_int(t,ins,0);int raw=*reinterpret_cast<const int32_t*>(ins+1);if((ins->param_mask&1)&&raw>=-100&&raw<0)t->stack_ptr-=8;stack_push_int(t,v);break;}
        case 43:{const StackValue v=stack_pop(t);*vm_write_int(t,ins,0)=stack_int(v);break;}
        case 44:{float v=vm_read_number(t,ins,0);float raw=*reinterpret_cast<const float*>(ins+1);if((ins->param_mask&1)&&raw>=-100&&raw<0)t->stack_ptr-=8;stack_push_float(t,v);break;}
        case 45:{const StackValue v=stack_pop(t);*vm_write_float(t,ins,0)=stack_float(v);break;}
        case 50:case 51:case 52:case 53:case 54:case 55:case 56:case 57:case 58:
        case 59:case 60:case 61:case 62:case 63:case 64:case 65:case 66:case 67:case 68:case 69:case 70:
        case 73:case 74:case 75:case 76:case 77: {
            const StackValue b=stack_pop(t),a=stack_pop(t);const int ai=stack_int(a),bi=stack_int(b);const float af=stack_float(a),bf=stack_float(b);
            switch(ins->id) {
            case 50:stack_push_int(t,ai+bi);break;case 51:stack_push_float(t,af+bf);break;
            case 52:stack_push_int(t,ai-bi);break;case 53:stack_push_float(t,af-bf);break;
            case 54:stack_push_int(t,ai*bi);break;case 55:stack_push_float(t,af*bf);break;
            case 56:stack_push_int(t,ai/bi);break;case 57:stack_push_float(t,af/bf);break;case 58:stack_push_int(t,ai%bi);break;
            case 59:stack_push_int(t,ai==bi);break;case 60:stack_push_int(t,af==bf);break;case 61:stack_push_int(t,ai!=bi);break;case 62:stack_push_int(t,af!=bf);break;
            case 63:stack_push_int(t,ai<bi);break;case 64:stack_push_int(t,af<bf);break;case 65:stack_push_int(t,ai<=bi);break;case 66:stack_push_int(t,af<=bf);break;
            case 67:stack_push_int(t,ai>bi);break;case 68:stack_push_int(t,af>bf);break;case 69:stack_push_int(t,ai>=bi);break;case 70:stack_push_int(t,af>=bf);break;
            case 73:stack_push_int(t,ai||bi);break;case 74:stack_push_int(t,ai&&bi);break;case 75:stack_push_int(t,ai^bi);break;case 76:stack_push_int(t,ai|bi);break;case 77:stack_push_int(t,ai&bi);break;
            }break;
        }
        case 71:case 72:{const StackValue a=stack_pop(t);stack_push_int(t,ins->id==71?stack_int(a)==0:stack_float(a)==0);break;}
        case 78:{int32_t* v=vm_write_int(t,ins,0);const int old=*v;--*v;stack_push_int(t,old);break;}
        case 79:case 80:{const float v=stack_float(stack_pop(t));stack_push_float(t,ins->id==79?std::sin(v):std::cos(v));break;}
        case 81:{const float angle=vm_read_number(t,ins,2),speed=vm_read_number(t,ins,3);*vm_write_float(t,ins,0)=std::cos(angle)*speed;*vm_write_float(t,ins,1)=std::sin(angle)*speed;break;}
        case 82:{float* v=vm_write_float(t,ins,0);*v=normalize_angle(*v);break;}
        case 84:{const float v=stack_float(stack_pop(t));stack_push_float(t,-v);break;}
        case 87:{float x1=vm_read_number(t,ins,1),y1=vm_read_number(t,ins,2),x2=vm_read_number(t,ins,3),y2=vm_read_number(t,ins,4);*vm_write_float(t,ins,0)=std::atan2(y2-y1,x2-x1);break;}
        case 94: {
            // 48d0c8: ellipse coordinates, with the major axis rotated by argument 4.
            const float angle=vm_read_number(t,ins,2),radius=vm_read_number(t,ins,3);
            const float rotation=vm_read_number(t,ins,4),ratio=vm_read_number(t,ins,5);
            const float delta=bullet_turn_angle(angle-rotation);
            const float x=std::cos(delta)*radius*ratio,y=std::sin(delta)*radius;
            const float s=std::sin(rotation),c=std::cos(rotation);
            *vm_write_float(t,ins,0)=x*c-y*s;*vm_write_float(t,ins,1)=y*c+x*s;break;
        }
        case 91: {
            const int slot=vm_read_int(t,ins,0);
            t->task_instruction[slot][0]=t->sub_index;t->task_instruction[slot][1]=t->offset;
            auto* raw=t->task_slots+slot*0x30;
            interp_eval1(raw);
            auto* values=reinterpret_cast<float*>(raw);auto* ints=reinterpret_cast<int32_t*>(raw);
            ints[10]=vm_read_int(t,ins,2);ints[11]=vm_read_int(t,ins,3);
            const float to=vm_read_number(t,ins,4),from=vm_read_number(t,ins,5);
            values[0]=from;values[1]=to;values[2]=values[3]=0;
            *vm_write_float(t,ins,1)=from;
            ints[5]=-1;ints[6]=0;values[7]=0;ints[8]=0;ints[9]|=1;
            t->task_a[slot]=t->stack_ret;break;
        }
        default: {
            EclThread* previous=g_active_thread;g_active_thread=t;
            auto* host=*reinterpret_cast<EclHostTable**>(t->owner);const int status=host->exec(t->owner);g_active_thread=previous;
            if(status==-1 || t->owner->pending_sub)return 0;
            if(status==1)return 1;
            break;
        }
        }
        if (!(ins->id==11 || ins->id==15 || ins->id==16 || (ins->id >= 42 && ins->id <= 45) || (ins->id >= 50 && ins->id <= 80)))
            t->stack_ptr -= ins->stack_refs;
        if(!jump)t->offset+=ins->size;
    }
    t->time+=delta;
    // 48c826: interpolation keeps writing its original destination even
    // while the VM is in a different subroutine or waiting for its next frame.
    for(int slot=0;slot<8;++slot) {
        auto* raw=t->task_slots+slot*0x30;
        if(*reinterpret_cast<int32_t*>(raw+0x28)==0)continue;
        const auto* data=static_cast<const uint8_t*>(resource->subs[t->task_instruction[slot][0]].data);
        const auto* setup=reinterpret_cast<const ecl::InsHeader*>(data+0x10+t->task_instruction[slot][1]);
        const int id=int(*reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(setup)+0x14));
        auto* host=*reinterpret_cast<EclHostTable**>(t->owner);
        float* destination=id<0?host->var_float_ptr(t->owner,id):
            reinterpret_cast<float*>(t->stack+t->task_a[slot]+id);
        *destination=interp_eval1(raw);
    }
    return t->sub_index<0?-1:0;
}
bool script_tree_run(EclThread* t,float delta) {
    if((t->flags1204&1)==0 && script_vm_run(t,delta)==1)return true;
    if(t->owner->pending_sub)return false;
    for(size_t i=0;i<t->children.size();) {
        EclThread* child=t->children[i];
        if(script_tree_run(child,delta))return true;
        if(child->sub_index<0 && child->children.empty()) {delete child;t->children.erase(t->children.begin()+i);} else ++i;
    }
    return false;
}
void script_tree_destroy(EclThread* t) {for(EclThread* child:t->children)script_tree_destroy(child);delete t;}

}  // namespace stage
namespace laser {
static IngameMenu* g_ingame_menu;
static float g_pause_speed;
static std::chrono::steady_clock::time_point g_pause_started;
static IngameMenuReason g_menu_reason;
static int g_game_over_track;
static double g_game_over_position;

static void open_menu(IngameMenuReason reason) {
    g_menu_reason=reason;
    g_pause_speed=stage::g_speed_scale;stage::g_speed_scale=1;
    stage_session_set_paused(true);g_pause_started=std::chrono::steady_clock::now();
    sound_pause_se();if(g_status.spell_id<0) {
        bgm_pause();
        if(reason==IngameMenuReason::GameOver){g_game_over_track=bgm_current_track();g_game_over_position=bgm_position();bgm_play(17);}
    }
    if(msg_active())msg_set_paused(true);
    const int credits=session().difficulty==::Difficulty::Extra?0:std::max(0,5-g_status.continues);
    ingame_menu_open(g_ingame_menu,reason,credits,-1);
    anm_copy_game_region(ingame_menu_background(g_ingame_menu));
}
void open_game_over() {open_menu(IngameMenuReason::GameOver);}
void open_replay_end() {open_menu(IngameMenuReason::ReplayEnd);}
void open_practice_clear() {open_menu(IngameMenuReason::PracticeClear);}
static void resume_menu() {
    if(spell_manager()->flags&0x40)
        spell_manager()->excluded_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-g_pause_started).count();
    stage::g_speed_scale=g_pause_speed;stage_session_set_paused(false);
    sound_resume_se();if(g_status.spell_id<0) {
        if(g_menu_reason==IngameMenuReason::GameOver)bgm_play_at(g_game_over_track,g_game_over_position);
        else bgm_resume();
    }
    if(msg_active())msg_set_paused(stage::shop_active());
}

// 0x004424e0 内联构造（DAT_004cf40c = 0x3f8 激光管理器）。
// 字段初值取自原版：+0x03c/+0x114=999、+0x104=1、+0x1dc=1、+0x00c/+0x020=-1、
// +0x01c/+0x030 的 bit0 置 1；随后注册 A 队 0x00457be0(prio 0x0c) 与 B 队 0x00458470(prio 0x51)。
LaserManager* create_laser_manager() {
    auto* mgr = new LaserManager{};
    mgr->flags = 2;
    mgr->frame_a = -1;
    mgr->flags01c = 1;
    mgr->frame_b = -1;
    mgr->flags030 = 1;
    mgr->field03c = 999;
    mgr->field104 = 1;
    mgr->field114 = 999;
    mgr->field1dc = 1;
    laser_manager = mgr;
    g_ingame_menu=ingame_menu_create();

    mgr->task_logic = thread_create(laser_update_task);
    thread_set_priority(mgr->task_logic, 0x0c);
    thread_set_ctx(mgr->task_logic, mgr);
    thread_enqueue_logic(mgr->task_logic);

    mgr->task_render = thread_create(laser_render_task);
    thread_set_priority(mgr->task_render, 0x51);
    thread_set_ctx(mgr->task_render, mgr);
    thread_enqueue_render(mgr->task_render);
    return mgr;
}

void destroy_laser_manager(LaserManager* mgr) {
    if (mgr == nullptr) return;
    thread_remove(mgr->task_logic);
    thread_remove(mgr->task_render);
    ingame_menu_destroy(g_ingame_menu);g_ingame_menu=nullptr;
    if (laser_manager == mgr) laser_manager = nullptr;
    delete mgr;
}

// 0x00457a60（A 队 0x00457be0 = JMP 本函数）：每帧推进两组波次计数。
// 原版开头还有：结束状态（+0x1ec）为 1..3 时调 0x00458e40（关卡结束/符卡演出，未移植）、
// 录像/暂停门控（DAT_004cccc8 bit6、会话 +0xb0 bit0x10000、DAT_004c5f8c、DAT_004ca21c
// bit8、DAT_004cd624 bit4）。这些属 core/结束流程，P1 未移植，先走正常游戏路径。
ThreadStatus laser_update_task(void* ctx) {
    LaserManager* mgr = static_cast<LaserManager*>(ctx);
    if(!stage_session_started())return THREAD_CONTINUE;
    if(ingame_menu_active(g_ingame_menu)) {
        const auto action=ingame_menu_update(g_ingame_menu);
        if(action==IngameMenuAction::Resume)resume_menu();
        else if(action==IngameMenuAction::Continue) {
            g_status.lives=2;g_status.unk_070=0;g_status.unk_07c=std::min(3,g_status.unk_084);g_status.unk_080=0;
            g_status.power=g_status.power_step;status_add_power(&g_status,g_status.power_step<<2);player_refresh_power_options();
            if(g_status.continues<9)++g_status.continues;session().score=g_status.score=0;
            resume_menu();
        } else if(action==IngameMenuAction::ReturnTitle || action==IngameMenuAction::RestartRun) {
            sound_resume_se();stage::g_speed_scale=g_pause_speed;
            app_context()->next_state=action==IngameMenuAction::ReturnTitle?0x10:(replay_mode()==ReplayMode::Playback?0xb:0xa);
        }
    } else if(stage_session_can_pause() && (input_state().pressed&ACTION_MENU_CANCEL)!=0) {
        open_menu(IngameMenuReason::Pause);
    }
    if (mgr->sequence_mode != 0 && static_cast<uint32_t>(mgr->sequence_mode - 1) < 3) {
        // 0x00458e40 未移植。
    }
    if (mgr->field018 != 0) mgr->field018 = 0;
    mgr->frame_a = mgr->wave_a;
    if (stage::g_speed_scale > 0.99f && stage::g_speed_scale < 1.01f) {
        mgr->wave_a += 1;
        mgr->frac_a += 1.0f;
    } else {
        mgr->frac_a += stage::g_speed_scale;
        mgr->wave_a = static_cast<int32_t>(mgr->frac_a);
    }
    if (mgr->field02c != 0) mgr->field02c = 0;
    mgr->frame_b = mgr->wave_b;
    if (stage::g_speed_scale > 0.99f && stage::g_speed_scale < 1.01f) {
        mgr->wave_b += 1;
        mgr->frac_b += 1.0f;
    } else {
        mgr->frac_b += stage::g_speed_scale;
        mgr->wave_b = static_cast<int32_t>(mgr->frac_b);
    }
    return THREAD_CONTINUE;
}

// 0x00458090（B 队 0x00458470 = JMP 本函数）：符卡名/首领名 HUD 绘制。
// 原版用 text 管理器（DAT_004cf2ac）+0x19240 置 1、+0x1e8 目标句柄查表画名字；
// 等 A 队 text 管理器访问器与目标实体表接上后补。
ThreadStatus laser_render_task(void* ctx) {
    ingame_menu_render(g_ingame_menu);
    return THREAD_CONTINUE;
}

}  // namespace laser
