#include "projectiles/laser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include "core/rng.h"
#include "platform/gfx.h"
#include "player/player.h"
#include "presentation/anm.h"
#include "presentation/msg.h"
#include "presentation/sound.h"
#include "projectiles/bullet_types.h"
#include "stage/enemy.h"
#include "stage/session.h"
#include "stage/stage.h"

namespace laser {
namespace {
constexpr float pi=3.14159265358979323846f;
struct Path {
    Path* next;Path* prev;
    float start,end;int type;float direction[3],pos[3],angle,speed,accel,angle_step;
};
struct CurveNode {float x,y,z,cached[3],angle,length;};
struct CurveVertex{float x,y,z,rhw;uint32_t color;float u,v;};
struct Laser : Object {
    Type kind;
    union {LineArgs line;InfiniteArgs infinite;CurveArgs curve;};
    AnmVm* body;AnmVm* start;AnmVm* tip;
    uint32_t effects;Extra effect[32];float effect_time[32];
    float accel_x,accel_y,turn_angle,turn_speed;int turn_count;
    int cursor,jump_count,type,color;
    float outside,delay,immune,state_time,graze_time,age;
    std::vector<CurveNode> nodes;
    std::vector<CurveVertex> vertices;
    uint32_t beam_flags;
    Path path;
};

float wrap(float a){return std::remainder(a,2*pi);}
float aim(float x,float y){return std::atan2(g_player->y-y,g_player->x-x);}
void position_vm(AnmVm* vm,float x,float y,float z=0){vm->extra_x=x;vm->extra_y=y;vm->extra_z=z;}
AnmVm* new_vm(Laser* l,int script,int callback) {
    auto* vm=anm_vm_alloc(static_cast<AnmResource*>(manager->bullet_anm));
    anm_vm_bind_script(vm,static_cast<AnmResource*>(manager->bullet_anm),script);
    vm->callback_object=l;vm->sprite_callback=callback;
    return vm;
}
void free_laser(Laser* l) {
    anm_vm_free(l->body);anm_vm_free(l->start);anm_vm_free(l->tip);
    for(Path* p=l->path.next;p;){Path* next=p->next;delete p;p=next;}
    delete l;
}
Extra* extras(Laser& l) {
    if(l.kind==Type::Line)return l.line.extras;
    if(l.kind==Type::Infinite)return l.infinite.extras;
    return l.curve.extras;
}
int transform_se(const Laser& l){return l.kind==Type::Line?l.line.transform_se:l.kind==Type::Infinite?l.infinite.transform_se:l.curve.transform_se;}
void transform_sound(const Laser& l){const int se=transform_se(l);if(se>=0)sound_play_se_at(se,l.x);}
void fire_extra(Laser& l,const Extra& x,const Extra& next);

void line_extras(Laser& l) {
    bool completed;
    do {
        completed=false;
        while(l.cursor<24) {
            Extra& source=extras(l)[l.cursor];const Extra x=source;
            if(x.type==0 || (x.async==0 && l.effects!=0))break;
            const int record=l.cursor++;
            int bit=0;while((x.type&(1u<<bit))==0)++bit;
            l.effect[bit]=x;l.effect_time[bit]=0;
            switch(x.type) {
            case 1:l.effects|=1;break;
            case 4: {
                const float angle=x.f[1]<=-990?l.angle:x.f[1]>=990?aim(l.x,l.y):x.f[1];
                l.accel_x=std::cos(angle)*x.f[0];l.accel_y=std::sin(angle)*x.f[0];l.effects|=4;
                if(record!=0)transform_sound(l);break;
            }
            case 8:l.effects|=8;if(record!=0)transform_sound(l);break;
            case 0x10:l.turn_angle=x.f[0];l.turn_speed=x.f[1]<=-999?l.speed:x.f[1];l.turn_count=0;l.effects|=0x10;break;
            case 0x40: {
                if(x.i[0]>0) {
                    --source.i[0];l.effects|=0x40;
                }break;
            }
            case 0x80:l.immune=float(x.i[0]);break;
            case 0x100:l.outside=float(x.i[0]);break;
            case 0x200:anm_vm_set_script(l.body,static_cast<AnmResource*>(manager->bullet_anm),kBulletTypes[x.i[0]].script+x.i[1]);break;
            case 0x400:l.state=3;break;
            case 0x800:sound_play_se_at(x.i[0],l.x);break;
            case 0x1000:l.effects|=0x1000;break;
            case 0x2000:fire_extra(l,x,l.line.extras[l.cursor++]);break;
            case 0x8000:l.label=x.i[0];break;
            case 0x10000:
                if(x.i[1]<=0 || ++l.jump_count<x.i[1])l.cursor=x.i[0];else l.jump_count=0;
                break;
            case 0x100000:l.body->flags2=(l.body->flags2&~0x1e0u)|(x.i[0]==0?0:0x20);break;
            case 0x80000000:l.effects|=0x80000000u;break;
            }
        }
        for(int bit=0;bit<32;++bit) {
            const uint32_t flag=1u<<bit;if((l.effects&flag)==0)continue;
            const Extra& x=l.effect[bit];float& time=l.effect_time[bit];bool end=false;
            if(flag==4) {
                if(time>=x.i[0])end=true;
                else {l.speed+=x.f[0]*stage::g_speed_scale;l.vx+=l.accel_x*stage::g_speed_scale;l.vy+=l.accel_y*stage::g_speed_scale;if(l.vx!=0||l.vy!=0)l.angle=std::atan2(l.vy,l.vx);time+=stage::g_speed_scale;}
            } else if(flag==0x10 && x.i[2]==0) {
                if(time<x.i[0]) {const float v=l.speed*(1-time/x.i[0]);l.vx=std::cos(l.angle)*v;l.vy=std::sin(l.angle)*v;time+=stage::g_speed_scale;}
                else {l.angle=wrap(l.angle+l.turn_angle);l.speed=l.turn_speed;l.vx=std::cos(l.angle)*l.speed;l.vy=std::sin(l.angle)*l.speed;time=0;end=++l.turn_count>=x.i[1];transform_sound(l);}
            } else if(flag==0x40) {
                const float c=std::cos(l.angle),s=std::sin(l.angle),tx=l.x+c*l.length,ty=l.y+s*l.length;
                const bool edges[]={ty<=0 && (x.i[1]&1),ty>=448 && (x.i[1]&2),tx<=-192 && (x.i[1]&4),tx>=192 && (x.i[1]&8)};
                for(int edge=0;edge<4;++edge)if(edges[edge]) {
                    LineArgs a=l.line;a.offset=0;a.speed=x.f[0]<0?l.speed:x.f[0];
                    if(edge<2){const float y=edge==0?0.0f:448.0f;a.pos[0]=l.x+c*(y-l.y)/s;a.pos[1]=y;a.angle=-l.angle;}
                    else{const float xx=edge==2?-192.0f:192.0f;a.pos[0]=xx;a.pos[1]=l.y+s*(xx-l.x)/c;a.angle=wrap(-l.angle-pi);}
                    if((x.i[1]&0x10)==0)create_laser(0,&a);end=true;transform_sound(l);
                }
            } else if(flag==0x80000000u) {if(time>=x.i[0])end=true;else time+=stage::g_speed_scale;}
            if(end){l.effects&=~flag;completed=true;}
        }
        if(l.effects && l.immune>0)l.immune-=stage::g_speed_scale;
    }while(completed);
}

int cancel_axis(Laser& l,float x,float y,float a,float b,float angle,int kind,bool rectangle,int respect) {
    if(respect && l.immune!=0)return 0;
    const bool line=l.kind==Type::Line;
    if(line?l.length<16:rectangle?l.length<16:l.length<=16)return 0;
    std::vector<bool> hit;
    const float c=std::cos(l.angle),s=std::sin(l.angle),rc=std::cos(angle),rs=std::sin(angle);
    for(float step=8;rectangle?step+8<=l.length:line?step-8<16*std::floor(l.length/16):step+8<l.length;step+=16) {
        const float px=l.x+c*step,py=l.y+s*step,dx=px-x,dy=py-y;
        const bool inside=rectangle?std::fabs(rc*dx+rs*dy)<=a*0.5f && std::fabs(rc*dy-rs*dx)<=b*0.5f:dx*dx+dy*dy<=a*a;
        hit.push_back(inside);
        if(inside)stage::laser_cancel_effect(px,py,kind,l.type,l.color,rectangle);
    }
    const int count=int(std::count(hit.begin(),hit.end(),true));if(count==0)return 0;
    size_t first=0;while(first<hit.size() && hit[first])++first;
    if(line && first!=0) {
        if(first==hit.size()){l.flags=(l.flags&~4u)|2;return count;}
        l.x+=c*float(first*16);l.y+=s*float(first*16);l.length-=float(first*16);
        l.line.max_length=l.length;l.traveled=float(first*16);if(l.length<=24)l.flags=(l.flags&~4u)|2;
    }
    if(!line)first=0;
    size_t run=first;while(run<hit.size() && !hit[run])++run;
    if(!line || run<hit.size()) {
        const float old_x=l.x-c*float(first*16),old_y=l.y-s*float(first*16);
        l.length=float((run-first)*16);
        for(size_t i=run;i<hit.size();) {
            while(i<hit.size() && hit[i])++i;const size_t begin=i;
            while(i<hit.size() && !hit[i])++i;
            if((i-begin)*16<=24)continue;
            LineArgs part{};
            if(line)part=l.line;
            part.pos[0]=old_x+c*float(begin*16);part.pos[1]=old_y+s*float(begin*16);part.pos[2]=l.z;
            part.angle=l.angle;part.max_length=part.start_length=float((i-begin)*16);
            if(!line) {
                part.end_travel=l.infinite.max_length-float(begin*16);part.width=l.width;part.speed=8;part.type=l.type;part.color=l.color;
                if(rectangle){part.flags=(l.infinite.flags>>1)&1;part.se=part.transform_se=-1;}
            }
            if(line || (part.pos[0]>-224 && part.pos[0]<224 && part.pos[1]>-32 && part.pos[1]<480))create_laser(0,&part);
        }
    }
    return count;
}

void axis_collision(Laser& l,bool query) {
    if(l.length<=16 || (l.kind==Type::Line && l.width<=3))return;
    const float fraction=l.kind==Type::Infinite?0.9f:(l.line.flags&2)?1.0f:0.8f;
    const float start=l.kind==Type::Line && (l.line.flags&2)==0?l.length*0.1f:0;
    const float c=std::cos(l.angle),s=std::sin(l.angle);
    const float dx=g_player->x-l.x-c*start,dy=g_player->y-l.y-s*start;
    const float along=c*dx+s*dy;
    const float width=l.width<32?l.width*0.5f:l.kind==Type::Line?(l.width-16)*0.5f:l.width-(l.width+16)/3;
    const float px=l.x+c*(start+std::min(std::max(along,0.0f),l.length*fraction));
    const float py=l.y+s*(start+std::min(std::max(along,0.0f),l.length*fraction));
    const int collision=player_collision_line(l.x+c*start,l.y+s*start,l.angle,l.length*fraction,width,query);
    if(collision==1)cancel_axis(l,g_player->x,g_player->y,32,32,0,0,true,1);
    else if(collision==2 && int(l.graze_time)%3==0)stage::player_graze(px,py);
    l.graze_time+=stage::g_speed_scale;
}

bool update_line(Laser& l) {
    line_extras(l);
    if(l.length<l.line.max_length)l.length=std::min(l.length+l.speed*stage::g_speed_scale,l.line.max_length);
    else {l.traveled+=l.speed*stage::g_speed_scale;l.x+=l.vx*stage::g_speed_scale;l.y+=l.vy*stage::g_speed_scale;}
    if(l.line.end_travel>0 && l.traveled+l.length>l.line.end_travel) {
        l.length=l.line.max_length=l.line.end_travel-l.traveled;if(l.length<=0)return true;
    }
    if(l.outside>0)l.outside-=stage::g_speed_scale;if(l.delay>0)l.delay-=stage::g_speed_scale;
    const float tx=l.x+std::cos(l.angle)*l.length,ty=l.y+std::sin(l.angle)*l.length;
    const bool out0=l.x< -192-l.width||l.x>192+l.width||l.y< -l.width||l.y>448+l.width;
    const bool out1=tx< -192-l.width||tx>192+l.width||ty< -l.width||ty>448+l.width;
    if(l.outside<=0 && l.delay<=0 && out0 && out1)return true;
    l.body->scale_x=l.width/l.body->sprite_h;l.body->scale_y=l.length/l.body->sprite_w;
    anm_vm_update(l.body);anm_vm_update(l.tip);if(l.traveled==0)anm_vm_update(l.start);
    axis_collision(l,false);return false;
}

bool update_infinite(Laser& l) {
    auto& a=l.infinite;
    while(l.cursor<24) {
        const Extra x=a.extras[l.cursor++];if(x.type==0){--l.cursor;break;}
        if(x.type==0x80)l.immune=float(x.i[0]);
        else if(x.type==0x400)l.state=3;
        else if(x.type==0x100000)l.body->flags2=(l.body->flags2&~0x1e0u)|(x.i[0]==0?0:0x20);
    }
    l.length=std::min(l.length+l.speed*stage::g_speed_scale,a.max_length);
    l.angle+=a.angle_step*stage::g_speed_scale;
    l.x=a.origin[0]+a.position_step[0]*stage::g_speed_scale+std::cos(l.angle)*a.axis_offset;
    l.y=a.origin[1]+a.position_step[1]*stage::g_speed_scale+std::sin(l.angle)*a.axis_offset;
    if(a.flags&1) {auto* boss=stage::find_enemy(stage::g_enemy_manager->boss_id[0]);if(boss){l.x+=boss->x;l.y+=boss->y;}}
    switch(l.state) {
    case 3:if(l.state_time>=a.delay){l.state_time=0;l.state=4;}break;
    case 4:
        if(l.state_time<a.expand){l.width=l.state_time*a.max_width/a.expand;break;}
        l.state_time=0;l.state=2;l.width=a.max_width;
        [[fallthrough]];
    case 2:
        if(l.state_time<a.hold)break;
        l.state_time=0;l.state=5;
        [[fallthrough]];
    case 5:if(l.state_time>=a.shrink)return true;else l.width=a.max_width-l.state_time*a.max_width/a.shrink;break;
    }
    if(l.state==4 || l.state==2)axis_collision(l,false);
    l.body->scale_x=l.width/l.body->sprite_h;l.body->scale_y=l.length/l.body->sprite_w;
    anm_vm_update(l.body);anm_vm_update(l.start);l.state_time+=stage::g_speed_scale;
    return false;
}

void clear_one(Laser& l,int kind);
void sample_path(const Path& p,float time,CurveNode& n) {
    const float t=time-p.start,c=std::cos(p.angle),s=std::sin(p.angle);
    n.x=p.pos[0];n.y=p.pos[1];n.z=p.pos[2];n.angle=p.angle;n.length=p.speed;
    if(p.type==0){n.x+=c*p.speed*t;n.y+=s*p.speed*t;}
    else if(p.type==1 && p.angle_step< -990) {
        const float d=(2*p.speed+p.accel*t)*(t+1)*0.5f;n.x+=c*d;n.y+=s*d;n.length=p.speed+p.accel*t;
    } else if(p.type==1) {
        const float vx=c*p.speed+std::cos(p.angle_step)*p.accel,vy=s*p.speed+std::sin(p.angle_step)*p.accel;
        n.x+=vx*t;n.y+=vy*t;n.angle=std::atan2(vy,vx);n.length=std::sqrt(vx*vx+vy*vy);
    } else if(p.type==2) {
        for(int i=0;i<int(std::floor(t));++i){n.x+=std::cos(n.angle)*n.length;n.y+=std::sin(n.angle)*n.length;n.angle+=p.angle_step;n.length+=p.accel;}
        const float fraction=t-std::floor(t);n.x+=fraction*std::cos(n.angle)*n.length;n.y+=fraction*std::sin(n.angle)*n.length;
    }
}
void reverse_path(const Path& p,float time,const CurveNode& prev,CurveNode& n) {
    n=prev;
    if(p.type==0){n.x-=std::cos(p.angle)*p.speed;n.y-=std::sin(p.angle)*p.speed;n.angle=p.angle;n.length=p.speed;}
    else if(p.type==1 && p.angle_step< -990) {
        n.x-=std::cos(prev.angle)*(prev.length-p.accel);n.y-=std::sin(prev.angle)*(prev.length-p.accel);n.length=p.speed-p.accel;
    } else if(p.type==1) {
        const float vx=-std::cos(prev.angle)*prev.length-std::cos(p.angle_step)*p.accel,vy=-std::sin(prev.angle)*prev.length-std::sin(p.angle_step)*p.accel;
        n.x+=vx;n.y+=vy;n.length=std::sqrt(vx*vx+vy*vy);n.angle=std::atan2(vy,vx);
    } else if(p.type==2) {
        const float fraction=time-std::floor(time);
        n.x-=fraction*std::cos(prev.angle)*prev.length;n.y-=fraction*std::sin(prev.angle)*prev.length;
        n.angle=prev.angle-p.angle_step;n.length=prev.length-p.accel;
        n.x-=(1-fraction)*std::cos(n.angle)*n.length;n.y-=(1-fraction)*std::sin(n.angle)*n.length;
    }
}
void curve_geometry(Laser& l) {
    for(size_t i=0;i<l.nodes.size();++i) {
        const float time=l.age-float(i);auto& n=l.nodes[i];
        if(time<0){n.x=l.curve.pos[0];n.y=l.curve.pos[1];n.z=l.curve.pos[2];continue;}
        for(Path* p=&l.path;p;p=p->next)if(p->start<=time && time<p->end) {
            if(i==0)sample_path(*p,time,n);else reverse_path(*p,time,l.nodes[i-1],n);break;
        }
    }
}
void append_path(Laser& l,const Extra& x) {
    Path* prev=&l.path;while(prev->next)prev=prev->next;
    CurveNode base{};sample_path(*prev,float(x.i[1]),base);prev->end=float(x.i[1]);
    auto* p=new Path{};prev->next=p;p->prev=prev;p->start=float(x.i[1]);p->end=x.i[0]<0?999999:p->start+x.i[0];p->type=x.type==4?1:2;
    p->pos[0]=base.x;p->pos[1]=base.y;p->pos[2]=base.z;p->angle=base.angle;p->speed=base.length;p->accel=x.f[0];p->angle_step=x.f[1];
    p->direction[0]=std::cos(p->angle);p->direction[1]=std::sin(p->angle);
    if(x.i[0]>=0) {
        sample_path(*p,p->end,base);auto* tail=new Path{};p->next=tail;tail->prev=p;tail->start=p->end;tail->end=999999;
        tail->pos[0]=base.x;tail->pos[1]=base.y;tail->pos[2]=base.z;tail->angle=base.angle;tail->speed=base.length;
        tail->direction[0]=std::cos(tail->angle);tail->direction[1]=std::sin(tail->angle);
    }
}
void fire_extra(Laser& l,const Extra& x,const Extra& next) {
    bullet::Slot slot{};slot.f000=next.i[0];slot.f004=next.i[1];
    slot.f014=x.f[0]<=-999990?l.angle:x.f[0]>=999990?aim(l.x,l.y):x.f[0];slot.f018=x.f[1];
    slot.f01c=x.f[2]<=-999990?l.speed:x.f[2];std::memcpy(&slot.i020,&x.f[3],4);
    const Extra* saved=l.kind==Type::Line?l.line.extras:l.curve.extras;
    std::memcpy(reinterpret_cast<uint8_t*>(&slot)+0x28,saved,24*sizeof(Extra));
    auto* raw=reinterpret_cast<uint8_t*>(&slot);*reinterpret_cast<int16_t*>(raw+0x46c)=int16_t(x.i[2]);*reinterpret_cast<int16_t*>(raw+0x46e)=int16_t(x.i[3]);
    *reinterpret_cast<int16_t*>(raw+0x470)=int16_t(x.i[0]);*reinterpret_cast<int32_t*>(raw+0x480)=x.i[1];slot.type=-1;
    stage::fire_enemy_bullet_slot(slot,l.x+std::cos(l.angle)*l.length,l.y+std::sin(l.angle)*l.length);
    if(next.i[2]!=0)clear_one(l,1);
}
void curve_extras(Laser& l) {
    bool completed;
    do {
        completed=false;
        while(l.cursor<24) {
            const Extra x=l.curve.extras[l.cursor];
            if(x.type==0 || (x.async==0 && l.effects) || (x.type&l.effects))break;
            ++l.cursor;int bit=0;while((x.type&(1u<<bit))==0)++bit;
            l.effect[bit]=x;l.effect_time[bit]=0;
            switch(x.type) {
            case 1:l.effects|=1;break;
            case 4:case 8:append_path(l,x);break;
            case 0x10:l.turn_angle=x.f[0];l.turn_speed=x.f[1]<=-999?l.speed:x.f[1];l.turn_count=0;l.effects|=0x10;break;
            case 0x40:if(x.i[0]>0){--l.curve.extras[l.cursor-1].i[0];l.effects|=0x40;}break;
            case 0x80:l.immune=float(x.i[0]);break;
            case 0x100:l.effects|=0x100;l.effect_time[bit]=float(x.i[0]);break;
            case 0x200:anm_vm_set_script(l.body,static_cast<AnmResource*>(manager->bullet_anm),kBulletTypes[x.i[0]].script+x.i[1]);break;
            case 0x400:l.state=3;break;
            case 0x800:sound_play_se_at(x.i[0],l.x);break;
            case 0x1000:l.effects|=0x1000;break;
            case 0x2000:fire_extra(l,x,l.curve.extras[l.cursor++]);break;
            case 0x8000:l.label=x.i[0];break;
            case 0x10000:l.cursor=x.i[0];break;
            case 0x100000:l.body->flags2=(l.body->flags2&~0x1e0u)|(x.i[0]==0?0:0x20);break;
            case 0x10000000:l.flags=(l.flags&~0x10u)|((x.i[0]&1)?0x10u:0);break;
            case 0x80000000:if(x.i[0]>0){l.effects|=0x80000000u;l.effect_time[31]=float(x.i[0]);}break;
            }
        }
        if(l.effects==0)break;
        for(int bit=0;bit<32;++bit) {
            const uint32_t flag=1u<<bit;if((l.effects&flag)==0)continue;
            const Extra& x=l.effect[bit];float& t=l.effect_time[bit];bool end=false;
            if(flag==0x100){t-=stage::g_speed_scale;if(t<1)end=true;}
            else if(flag==0x80000000u){if(int(t)<1)end=true;else t-=stage::g_speed_scale;}
            else if(flag==0x10 && x.i[2]==0) {
                if(t<x.i[0]){const float speed=l.speed*(1-t/x.i[0]);l.vx=std::cos(l.angle)*speed;l.vy=std::sin(l.angle)*speed;t+=stage::g_speed_scale;}
                else{l.angle=wrap(l.angle+l.turn_angle);l.speed=l.turn_speed;l.vx=std::cos(l.angle)*l.speed;l.vy=std::sin(l.angle)*l.speed;t=0;end=++l.turn_count>=x.i[1];transform_sound(l);}
            }
            if(end){l.effects&=~flag;completed=true;}
        }
        if(l.immune>0)l.immune-=stage::g_speed_scale;
    }while(completed);
}
void init_curve(Laser& l) {
    auto& a=l.curve;l.type=a.style;l.color=a.color;l.cursor=a.cursor;l.state=l.field14=2;
    l.angle=a.angle;l.width=a.width;l.speed=a.speed;l.x=a.pos[0]+std::cos(a.angle)*a.offset;l.y=a.pos[1]+std::sin(a.angle)*a.offset;l.z=a.pos[2];
    a.pos[0]=l.x;a.pos[1]=l.y;a.offset=0;l.vx=std::cos(l.angle)*l.speed;l.vy=std::sin(l.angle)*l.speed;
    l.body=new_vm(&l,a.style==1?322:142+a.style,a.style==1?0:3);anm_interrupt(l.body,2);
    l.body->flags2=(l.body->flags2&~0x1e0u)|0x20;l.body->flags3=(l.body->flags3&~0x200000u)|0x100000;
    l.tip=new_vm(&l,56+a.color,0);anm_interrupt(l.tip,2);l.tip->flags2=(l.tip->flags2&~0x1e0u)|0x20;l.tip->flags3|=0x100000;
    l.age=a.start_time;l.nodes.resize(a.node_count);
    for(auto& n:l.nodes){n.x=l.x;n.y=l.y;n.z=l.z;n.angle=l.angle;n.length=l.speed;}
    if(a.path_clone) {
        const auto* source=static_cast<const Path*>(a.path_clone);l.path=*source;Path* tail=&l.path;
        for(source=source->next;source;source=source->next){tail->next=new Path(*source);tail=tail->next;}
        a.path_clone=nullptr;l.cursor=99;
    } else {
        l.path.start=0;l.path.end=999999;l.path.type=0;l.path.direction[0]=std::cos(l.angle);l.path.direction[1]=std::sin(l.angle);
        l.path.pos[0]=l.x;l.path.pos[1]=l.y;l.path.pos[2]=l.z;l.path.angle=wrap(l.angle);l.path.speed=l.speed;
    }
    curve_geometry(l);if(a.se>=0)sound_play_se_at(a.se,l.x);
}
int cancel_curve(Laser& l,float x,float y,float a,float b,float angle,int kind,bool rectangle,int respect) {
    if(respect && l.immune!=0)return 0;
    const float c=std::cos(angle),s=std::sin(angle);std::vector<bool> hit(l.nodes.size());int count=0;
    for(size_t i=0;i<l.nodes.size();++i) {
        const auto& n=l.nodes[i];const float dx=n.x-x,dy=n.y-y;
        hit[i]=rectangle?std::fabs(c*dx+s*dy)<=a*0.5f && std::fabs(c*dy-s*dx)<=b*0.5f:dx*dx+dy*dy<=a*a;
        if(hit[i]){++count;if(i%10==0)stage::laser_cancel_effect(n.x,n.y,kind,0,l.color,rectangle);}
    }
    if(count==0)return 0;
    if(count==int(hit.size())){l.flags=(l.flags&~4u)|2;return count;}
    size_t prefix=0;while(prefix<hit.size() && hit[prefix])++prefix;
    if(prefix!=0) {
        l.age-=float(prefix)*stage::g_speed_scale;
        if(!rectangle){l.nodes.erase(l.nodes.begin(),l.nodes.begin()+prefix);l.curve.node_count=int(l.nodes.size());return count;}
        hit.erase(hit.begin(),hit.begin()+prefix);l.nodes.resize(hit.size());
        if(hit.size()<4){l.flags=(l.flags&~4u)|2;return count;}
    }
    size_t head=0;while(head<hit.size() && !hit[head])++head;
    if(rectangle) {
        for(size_t i=head;i<hit.size();) {
            while(i<hit.size() && hit[i])++i;const size_t start=i;while(i<hit.size() && !hit[i])++i;
            if(i-start>3){CurveArgs args=l.curve;args.node_count=int(i-start);args.se=-1;args.path_clone=&l.path;args.start_time=l.age-float(start);create_laser(2,&args);}
        }
        if(head<4)l.flags=(l.flags&~4u)|2;else l.nodes.resize(head);
    } else {
        size_t tail=head;while(tail<hit.size() && hit[tail])++tail;
        if(tail<hit.size())l.nodes.erase(l.nodes.begin(),l.nodes.begin()+tail);else l.nodes.resize(head);
    }
    l.curve.node_count=int(l.nodes.size());return count;
}
void curve_collision(Laser& l,bool query) {
    float distance=0;bool grazed=false;
    for(size_t i=0;i+1<l.nodes.size();++i) {
        const auto& n=l.nodes[i];distance+=n.length;if(distance<16)continue;
        const float c=std::cos(n.angle),s=std::sin(n.angle),px=n.x+c*n.length*0.5f,py=n.y+s*n.length*0.5f;
        const int collision=player_collision_line(n.x,n.y,n.angle,n.length,l.width*0.5f,query);
        if(collision==1){cancel_curve(l,g_player->x,g_player->y,32,32,0,0,true,1);break;}
        if(collision==2 && !grazed && int(l.graze_time)%3==0){stage::player_graze(px,py);grazed=true;}
    }
    l.graze_time+=stage::g_speed_scale;
}
bool update_curve(Laser& l) {
    curve_extras(l);if((l.flags&0x10)==0)curve_geometry(l);
    bool outside=true;for(const auto& n:l.nodes)if(n.x>=-192-l.width && n.x<=192+l.width && n.y>=-l.width && n.y<=448+l.width){outside=false;break;}
    if(l.outside>0)l.outside-=stage::g_speed_scale;
    if(l.outside<=0 && (l.effects&0x100)==0 && outside)return true;
    curve_collision(l,false);anm_vm_update(l.body);anm_vm_update(l.tip);l.age+=stage::g_speed_scale;return false;
}
void draw_curve(Laser& l) {
    auto& vertices=l.vertices;vertices.resize(l.nodes.size()*2);
    const float half_width=l.curve.width*0.5f;
    float shake_x,shake_y;gfx_get_camera_shake(gfx_current_camera(),shake_x,shake_y);
    for(size_t i=0;i<l.nodes.size();++i) {
        const auto& n=l.nodes[i];float normal=n.angle+pi*0.5f;
        if(i)normal=l.nodes[i-1].angle+pi*0.5f+wrap(n.angle-l.nodes[i-1].angle)*0.5f;
        const float x=std::cos(normal)*half_width,y=std::sin(normal)*half_width,u=float(i)/float(l.nodes.size()-1);
        vertices[i*2]={n.x+x+224+shake_x,n.y+y+16+shake_y,0,1,0xffffffff,u,l.body->uv[0]};
        vertices[i*2+1]={n.x-x+224+shake_x,n.y-y+16+shake_y,0,1,0xffffffff,u,l.body->uv[2]};
    }
    auto* dev=gfx_device();dev->SetTexture(0,static_cast<IDirect3DTexture9*>(const_cast<void*>(l.body->texture)));
    dev->SetRenderState(D3DRS_ZENABLE,FALSE);dev->SetRenderState(D3DRS_LIGHTING,FALSE);dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);dev->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);dev->SetRenderState(D3DRS_DESTBLEND,(l.body->flags2&0x1e0)==0x20?D3DBLEND_ONE:D3DBLEND_INVSRCALPHA);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE);dev->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER);dev->SetRenderState(D3DRS_ALPHAREF,0);
    dev->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE);dev->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);dev->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    dev->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_MODULATE);dev->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);dev->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
    dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,UINT(vertices.size()-2),vertices.data(),sizeof(CurveVertex));
    if(int(l.age)<=int(l.nodes.size())){const auto& n=l.nodes.back();position_vm(l.tip,n.x,n.y,n.z);anm_draw_vm(l.tip);}
}

void draw_axis(Laser& l) {
    position_vm(l.body,l.x,l.y,l.z);l.body->rot_z=l.angle+pi*0.5f;anm_draw_vm(l.body);
    if(l.kind==Type::Line) {
        position_vm(l.tip,l.x+std::cos(l.angle)*l.length,l.y+std::sin(l.angle)*l.length,l.z);anm_draw_vm(l.tip);
        if(l.traveled==0){position_vm(l.start,l.x,l.y,l.z);anm_draw_vm(l.start);}
    } else {position_vm(l.start,l.x,l.y,l.z);anm_draw_vm(l.start);}
}
void clear_one(Laser& l,int kind) {
    if(l.kind==Type::Beam){l.flags=(l.flags&~4u)|2;return;}
    if(l.kind==Type::Curve){for(size_t i=0;i<l.nodes.size();i+=3)stage::laser_cancel_effect(l.nodes[i].x,l.nodes[i].y,0,0,l.color,false);l.state=1;return;}
    for(float step=8;step+8<l.length;step+=16)stage::laser_cancel_effect(l.x+std::cos(l.angle)*step,l.y+std::sin(l.angle)*step,kind,l.type,l.color,false);
    l.state=1;
}
} // namespace

int sprite_for_vm(const AnmVm* vm,int logical) {
    const auto* l=static_cast<const Laser*>(vm->callback_object);
    if(vm->sprite_callback==3)return 524+l->color;
    return kBulletTypes[l->type].sprite<0?logical:kBulletSprites[l->type][l->color*4+logical];
}

void create_laser(int32_t type,const void* args) {
    if(manager->count>0x1ff)return;
    auto* l=new Laser{};l->kind=static_cast<Type>(type);
    l->prev=manager->tail;manager->tail->next=l;manager->tail=l;++manager->count;
    manager->field79c=std::max(manager->field79c+1,0x10000u);l->label=int(manager->field79c);
    l->outside=30;l->delay=3;
    if(l->kind==Type::Line) {
        l->line=*static_cast<const LineArgs*>(args);auto& a=l->line;l->type=a.type;l->color=a.color;l->cursor=a.cursor;
        l->angle=a.angle;l->speed=a.speed;l->length=a.start_length;l->width=a.width;
        l->x=a.pos[0]+std::cos(l->angle)*a.offset;l->y=a.pos[1]+std::sin(l->angle)*a.offset;l->z=a.pos[2];
        l->vx=std::cos(l->angle)*l->speed;l->vy=std::sin(l->angle)*l->speed;
        l->traveled=a.start_length>a.max_length?0.01f:0;l->state=2;
        l->body=new_vm(l,kBulletTypes[l->type].script,2);anm_interrupt(l->body,2);
        l->start=new_vm(l,l->color+56,0);anm_interrupt(l->start,2);
        l->tip=new_vm(l,l->color+((l->type<0x18||l->type==0x38)?0x5b:0x53),0);
        anm_vm_update(l->tip);
        if(a.se>=0)sound_play_se_at(a.se,l->x);
    } else if(l->kind==Type::Infinite) {
        l->infinite=*static_cast<const InfiniteArgs*>(args);auto& a=l->infinite;
        l->type=a.type;l->color=a.color;l->cursor=a.cursor;l->angle=a.angle;l->length=a.start_length;l->width=2;l->speed=a.growth;l->state=3;l->label=a.label;
        l->body=new_vm(l,kBulletTypes[l->type].script,2);l->start=new_vm(l,l->color+56,0);
        anm_vm_update(l->body);anm_vm_update(l->start);
        if(a.se>=0)sound_play_se_at(a.se,a.origin[0]);
    } else if(l->kind==Type::Curve) {
        l->curve=*static_cast<const CurveArgs*>(args);init_curve(*l);
    }
}
Object* find(int32_t id){for(auto* n=manager->head;n;n=n->next)if(n->label==id)return n;return nullptr;}
void clear_lasers(int32_t kind){for(Object* n=manager->head;n;){Object* next=n->next;if(n->state!=1)clear_one(*static_cast<Laser*>(n),kind);n=next;}}
void clear_phase_lasers() {
    // 448fa0 resets the laser cancellation timer and immunity before clearing.
    for(Object* object=manager->head;object;) {
        Object* next=object->next;auto& laser=*static_cast<Laser*>(object);
        laser.state_time=0;laser.immune=0;clear_one(laser,1);object=next;
    }
}
int cancel_circle(float x,float y,float radius,int32_t kind,int respect) {
    int count=0;for(Object* n=manager->head;n;){Object* next=n->next;
        auto& l=*static_cast<Laser*>(n);
        if(n->state!=1 && l.kind!=Type::Beam)count+=l.kind==Type::Curve?cancel_curve(l,x,y,radius,0,0,kind,false,respect):cancel_axis(l,x,y,radius,0,0,kind,false,respect);
        n=next;
    }return count;
}
int cancel_rectangle(float x,float y,float w,float h,float angle,int respect) {
    int count=0;for(Object* n=manager->head;n;){Object* next=n->next;auto& l=*static_cast<Laser*>(n);
        if(n->state!=1 && (n->flags&1) && l.kind!=Type::Beam)count+=l.kind==Type::Curve?cancel_curve(l,x,y,w,h,angle,0,true,respect):cancel_axis(l,x,y,w,h,angle,0,true,respect);
        n=next;
    }return count;
}
void freeze(bool value){for(auto* n=manager->head;n;n=n->next){if(value)n->flags|=8;else n->flags&=~8u;}}
void set_origin(int id,float x,float y){if(auto* l=static_cast<Laser*>(find(id))){l->x=x;l->y=y;l->z=0;if(l->kind==Type::Infinite){l->infinite.origin[0]=x;l->infinite.origin[1]=y;l->infinite.origin[2]=0;}else if(l->kind==Type::Line){l->line.pos[0]=x;l->line.pos[1]=y;l->line.pos[2]=0;}else if(l->kind==Type::Curve){l->curve.pos[0]=x;l->curve.pos[1]=y;l->curve.pos[2]=0;}}}
void set_position_step(int id,float x,float y){if(auto* l=static_cast<Laser*>(find(id))){if(l->kind==Type::Infinite){l->infinite.position_step[0]=x;l->infinite.position_step[1]=y;l->infinite.position_step[2]=0;}else if(l->kind==Type::Line){l->line.angle=x;l->line.max_length=y;l->line.start_length=0;}else if(l->kind==Type::Curve){l->curve.angle=x;l->curve.width=y;l->curve.speed=0;}}}
void set_angle_step(int id,float v){if(auto* l=static_cast<Laser*>(find(id))){if(l->kind==Type::Infinite)l->infinite.angle_step=v;else if(l->kind==Type::Line)l->line.width=v;else if(l->kind==Type::Curve)std::memcpy(&l->curve.color,&v,4);}}
void clear_tag(int label){for(auto* n=manager->head;n;n=n->next)if(n->label==label){clear_one(*static_cast<Laser*>(n),0);n->label=0;}}
void set_beam_flag(int id,int v){if(auto* l=static_cast<Laser*>(find(id)))if(l->kind==Type::Beam)l->beam_flags=(l->beam_flags&~1u)|(v&1);}

// 4038a0, as used by 404080: intersect the finite laser axis with one edge.
static bool extra_border_intersection(float ax,float ay,float bx,float by,
                                      float cx,float cy,float dx,float dy,
                                      float& x,float& y) {
    const float side_c=(cy-ay)*(ax-bx)+(ax-cx)*(ay-by);
    const float side_d=(ax-dx)*(ay-by)+(dy-ay)*(ax-bx);
    if(side_d*side_c>0)return false;
    if(side_c==0 && side_d==0) {
        float left=ax,bottom=ay,right=bx,top=by;
        if(bx<ax){left=bx;bottom=by;right=ax;top=ay;}
        float edge_left=cx,edge_bottom=cy,edge_right=dx,edge_top=dy;
        if(dx<cx){edge_left=dx;edge_bottom=dy;edge_right=cx;edge_top=cy;}
        if(edge_right<left || edge_top<bottom || right<edge_left || top<edge_bottom)return false;
    } else if(((ay-cy)*(cx-dx)+(cx-ax)*(cy-dy))*
              ((by-cy)*(cx-dx)+(cx-bx)*(cy-dy))>0)return false;
    const bool axis_sloped=std::fabs(bx-ax)>=0.01f;
    const bool edge_sloped=std::fabs(dx-cx)>=0.01f;
    const float axis_slope=axis_sloped?(by-ay)/(bx-ax):0;
    const float axis_intercept=axis_sloped?ay-((by-ay)*ax)/(bx-ax):ax;
    const float edge_slope=edge_sloped?(dy-cy)/(dx-cx):0;
    const float edge_intercept=edge_sloped?cy-((dy-cy)*cx)/(dx-cx):cx;
    if(axis_sloped) {
        if(edge_sloped) {
            x=(edge_intercept-axis_intercept)/(axis_slope-edge_slope);
            y=((edge_intercept-axis_intercept)*axis_slope)/(axis_slope-edge_slope)+axis_intercept;
        } else {x=cx;y=axis_slope*cx+axis_intercept;}
        return true;
    }
    if(edge_sloped){x=ax;y=edge_slope*ax+edge_intercept;return true;}
    if(std::fabs(ax-cx)<0.001f){x=ax;y=ay;return true;}
    return false;
}

// 439020 / 4391a0 (ECL637): Extra spell 6 emits its two enemy scripts at
// the laser-axis boundary intersections. The native list includes every type.
void spawn_extra_border_enemies(int effect) {
    constexpr float corners[4][2]={{-192,0},{-192,448},{192,448},{192,0}};
    for(Object* object=manager->head;object;object=object->next) {
        const float vx=std::cos(object->angle),vy=std::sin(object->angle);
        const float ax=object->x-vx*1000,ay=object->y-vy*1000;
        const float bx=object->x+vx*1000,by=object->y+vy*1000;
        float points[2][2];int count=0;
        for(int edge=0;edge<4 && count<2;++edge) {
            const auto& from=corners[edge];const auto& to=corners[(edge+1)%4];
            if(extra_border_intersection(ax,ay,bx,by,from[0],from[1],to[0],to[1],
                                         points[count][0],points[count][1]))++count;
        }
        if(count==0)continue;
        int near_index=0,far_index=count-1;
        if(count==2 &&
           (ax-points[1][0])*(ax-points[1][0])+(ay-points[1][1])*(ay-points[1][1])<=
           (ax-points[0][0])*(ax-points[0][0])+(ay-points[0][1])*(ay-points[0][1])) {
            near_index=1;far_index=0;
        }
        const int selected=(points[far_index][1]-object->y)*vy+(points[far_index][0]-object->x)*vx>=1.0f?far_index:near_index;
        stage::SpawnArgs args{};
        args.x=points[selected][0];args.y=points[selected][1];args.health=10000;
        stage::create_enemy(effect==2?"BossCard6_et":"BossCard6_et2",args);
    }
}

Manager* create_manager() {
    auto* m=new Manager{};manager=m;m->field79c=0x10000;m->tail=reinterpret_cast<Object*>(&m->root_vtable);m->bullet_anm=anm_get(7,"bullet.anm");
    m->task_logic=thread_create(update_task);thread_set_priority(m->task_logic,0x1c);thread_set_ctx(m->task_logic,m);thread_enqueue_logic(m->task_logic);
    m->task_render=thread_create(render_task);thread_set_priority(m->task_render,0x24);thread_set_ctx(m->task_render,m);thread_enqueue_render(m->task_render);return m;
}
void destroy_manager(Manager* m) {
    thread_remove(m->task_logic);thread_remove(m->task_render);
    for(Object* n=m->head;n;){Object* next=n->next;free_laser(static_cast<Laser*>(n));n=next;}
    manager=nullptr;delete m;
}
int32_t update(Manager* m) {
    for(Object* n=m->head;n;){Object* next=n->next;auto& l=*static_cast<Laser*>(n);
        if(n->flags&6)n->flags=(((n->flags+2)^n->flags)&6)^n->flags;
        bool remove=(n->flags&6)>=4 || n->state==1;
        if(!remove) {
            if(n->flags&8){if(l.kind==Type::Line || l.kind==Type::Infinite)axis_collision(l,true);else if(l.kind==Type::Curve)curve_collision(l,true);}
            else {
                remove=l.kind==Type::Line?update_line(l):l.kind==Type::Infinite?update_infinite(l):l.kind==Type::Curve?update_curve(l):false;
                if(!remove){n->field24=0;n->wave0=n->wave1;n->wave_frac+=stage::g_speed_scale;n->wave1=int(n->wave_frac);n->flags|=1;}
            }
        }
        if(remove){--m->count;n->prev->next=next;if(next)next->prev=n->prev;if(m->tail==n)m->tail=n->prev;free_laser(&l);}n=next;
    }return 1;
}
ThreadStatus update_task(void* ctx) {
    if(!stage_session_started())return THREAD_CONTINUE;
    const uint32_t flags=stage_session_flags();
    if((flags&0x405)==0 && !stage::shop_active()) {const float speed=stage::g_speed_scale;if(flags&2)stage::g_speed_scale=0;update(static_cast<Manager*>(ctx));stage::g_speed_scale=speed;}
    return THREAD_CONTINUE;
}
ThreadStatus render_task(void* ctx) {
    if(stage_session_flags()&4)return THREAD_CONTINUE;
    for(Object* n=static_cast<Manager*>(ctx)->head;n;n=n->next)if(n->state!=1) {auto& l=*static_cast<Laser*>(n);if(l.kind==Type::Line||l.kind==Type::Infinite)draw_axis(l);else if(l.kind==Type::Curve)draw_curve(l);}
    return THREAD_CONTINUE;
}
} // namespace laser
