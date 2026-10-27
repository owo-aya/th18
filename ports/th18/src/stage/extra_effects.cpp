#include "stage/extra_effects.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <list>

#include "core/rng.h"
#include "core/thread.h"
#include "platform/gfx.h"
#include "presentation/anm.h"
#include "stage/background.h"
#include "stage/enemy.h"
#include "stage/stage.h"

namespace {
constexpr float pi=3.14159265358979323846f;
struct Point {float x,y,z;};
struct RibbonVertex {float x,y,z;uint32_t color;float u,v;};
struct AuraVertex {float x,y,z,rhw;uint32_t color;float u,v;};
struct Maple {
    Point points[32],directions[32],shift;
    uint32_t colors[32],head_color;
    alignas(4) uint8_t motion[0x58];
};
// 404fc0/405030: 50 groups of four independent effect150/151 particles.
struct Charge {
    AnmVm* particles[200]{};
    Point middle[200]{},tangent[200]{};
    int phase[200]{};
    int previous=-1,time=0;
    float fraction=0;
};
struct Aura {
    stage::Enemy* enemy;
    float target,radius,phase_x,phase_y;
    uint32_t color;
    AuraVertex vertices[17*17];
};
std::list<Aura> auras;
Thread* aura_render;

float visual_random() {return float(rng_next(card_rng()))*2.3283064e-10f;}
float angle(float value) {
    while(value>pi)value-=2*pi;
    while(value<-pi)value+=2*pi;
    return value;
}
void texture_state(IDirect3DDevice9* device) {
    device->SetRenderState(D3DRS_LIGHTING,FALSE);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_MODULATE);
    device->SetTextureStageState(0,D3DTSS_COLORARG1,D3DTA_TEXTURE);
    device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_MODULATE);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG1,D3DTA_TEXTURE);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_TEXTURETRANSFORMFLAGS,D3DTTFF_DISABLE);
}

ThreadStatus draw_auras(void*) {
    if(auras.empty())return THREAD_CONTINUE;
    auto* device=gfx_device();gfx_select_camera(3);
    device->SetRenderState(D3DRS_LIGHTING,FALSE);
    device->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
    device->SetRenderState(D3DRS_ZENABLE,FALSE);
    device->SetRenderState(D3DRS_FOGENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_ZERO);
    device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_ONE);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLENDALPHA,D3DBLEND_ZERO);
    device->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD);
    device->SetTexture(0,nullptr);
    device->SetTextureStageState(0,D3DTSS_COLOROP,D3DTOP_SELECTARG2);
    device->SetTextureStageState(0,D3DTSS_COLORARG2,D3DTA_DIFFUSE);
    device->SetTextureStageState(0,D3DTSS_ALPHAOP,D3DTOP_SELECTARG2);
    device->SetTextureStageState(0,D3DTSS_ALPHAARG2,D3DTA_DIFFUSE);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    // 455610 (B0x0e): preserve scene RGB and replace its alpha with 255
    // before sampling @R. Background sprites leave their own alpha in it;
    // using that alpha again would suppress the distortion's circle mask.
    const AuraVertex opaque_alpha[4]={
        {32,16,0,1,0xff000000,0,0},{416,16,0,1,0xff000000,0,0},
        {32,464,0,1,0xff000000,0,0},{416,464,0,1,0xff000000,0,0}
    };
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,opaque_alpha,sizeof(AuraVertex));
    // 455530 renders layer35 at B0x0f, after the background has been drawn.
    // 455d80 uses text66's @R texture. The pause-copy rectangle excludes
    // its right/bottom texels, but 42f890 can sample those edge texels.
    IDirect3DTexture9* aura_texture=gfx_sample_scene();
    gfx_select_camera(3);
    texture_state(device);device->SetTexture(0,aura_texture);
    device->SetSamplerState(0,D3DSAMP_MINFILTER,D3DTEXF_POINT);
    device->SetSamplerState(0,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
    device->SetSamplerState(0,D3DSAMP_ADDRESSU,D3DTADDRESS_WRAP);
    device->SetSamplerState(0,D3DSAMP_ADDRESSV,D3DTADDRESS_WRAP);
    device->SetRenderState(D3DRS_ZENABLE,FALSE);
    device->SetRenderState(D3DRS_FOGENABLE,FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);
    // 42b1f0 overrides text66's copy blend (3) with normal alpha blend (0).
    // The zero-alpha vertices outside the circle must preserve the background.
    device->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_SRCALPHA);
    device->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_INVSRCALPHA);
    device->SetRenderState(D3DRS_BLENDOP,D3DBLENDOP_ADD);
    device->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE);
    device->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER);
    device->SetRenderState(D3DRS_ALPHAREF,0);
    device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE,TRUE);
    device->SetRenderState(D3DRS_SRCBLENDALPHA,D3DBLEND_ONE);
    device->SetRenderState(D3DRS_DESTBLENDALPHA,D3DBLEND_ZERO);
    device->SetRenderState(D3DRS_BLENDOPALPHA,D3DBLENDOP_ADD);
    device->SetFVF(D3DFVF_XYZRHW|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    // 42b540 copies adjacent columns into sixteen 34-vertex strips.
    for(const auto& aura:auras)for(int x=0;x<16;++x) {
        AuraVertex strip[34];
        for(int y=0;y<17;++y){strip[y*2]=aura.vertices[x*17+y];strip[y*2+1]=aura.vertices[(x+1)*17+y];}
        device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,32,strip,sizeof(AuraVertex));
    }
    return THREAD_CONTINUE;
}
}

void extra_effects_start() {
    aura_render=thread_create(draw_auras);aura_render->flags|=2;
    thread_set_priority(aura_render,0x0f);thread_enqueue_render(aura_render);
}
void extra_effects_stop() {
    thread_remove(aura_render);aura_render=nullptr;auras.clear();
}

void extra_maple_spawn(AnmResource* resource) {
    // 407590 / effect4: st07enm script7 supplies the texture; geometry is
    // a 32-section ribbon in the STD camera's world coordinates.
    auto* vm=anm_vm_alloc(resource);anm_vm_set_script(vm,resource,7);
    auto* maple=new Maple{};vm->callback_object=maple;vm->callback_index=7;
    vm->draw_layer=0;vm->flags3=(vm->flags3&~0x300000u)|0x1000u;
    vm->flags2=(vm->flags2&~0x1e0u)|0x2021u;
    Point start{stage_background_anm_value(10016)+stage_background_anm_value(10019)*260,
                stage_background_anm_value(10017)+stage_background_anm_value(10020)*260,
                stage_background_anm_value(10018)+stage_background_anm_value(10021)*260};
    for(int n=0;n<32;++n){maple->points[n]=start;maple->directions[n]={0,1,0};maple->colors[n]=0x80ffffff;}
    maple->head_color=0xffffffff;
    auto* q=reinterpret_cast<float*>(maple->motion);auto* ints=reinterpret_cast<int32_t*>(maple->motion);
    q[0]=start.x;q[1]=start.y;q[2]=start.z;
    q[6]=(visual_random()*2-1)*10;q[7]=visual_random()*10;q[8]=(visual_random()*2-1)*10;
    q[9]=visual_random()*800;if(start.x>0)q[9]=-q[9];q[10]=visual_random()*1000;q[11]=q[8];
    q[3]=start.x;q[4]=start.y+200;q[5]=start.z+visual_random()*200+800;
    ints[15]=-1;ints[19]=1;ints[20]=160;ints[21]=8;
    auto* alpha=reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm)+0x140);
    auto* clock=reinterpret_cast<int32_t*>(alpha);alpha[0]=64;alpha[1]=0;
    clock[5]=-1;clock[6]=0;alpha[7]=0;clock[8]=0;clock[9]=1;clock[10]=160;clock[11]=0;
    anm_manage_vm(vm,nullptr);vm->node_flags=0;anm_queue_add(vm);
}

bool extra_maple_update(AnmVm* vm) {
    auto& maple=*static_cast<Maple*>(vm->callback_object);
    stage_background_apply_camera_delta(vm);
    const Point delta{vm->extra_x,vm->extra_y,vm->extra_z};vm->extra_x=vm->extra_y=vm->extra_z=0;
    for(int n=31;n>0;--n) {
        const Point old=maple.points[n],previous=maple.points[n-1];
        maple.directions[n]={previous.x-old.x,previous.y-old.y,previous.z-old.z};
        maple.points[n]={previous.x+delta.x,previous.y+delta.y,previous.z+delta.z};
        maple.colors[n]=maple.colors[n-1];
    }
    maple.shift.x+=delta.x;maple.shift.y+=delta.y;maple.shift.z+=delta.z;
    float pos[3];interp_eval3(maple.motion,pos);
    maple.points[0]={pos[0]+maple.shift.x,pos[1]+maple.shift.y,pos[2]+maple.shift.z};
    maple.directions[0]={maple.points[0].x-maple.points[1].x,maple.points[0].y-maple.points[1].y,maple.points[0].z-maple.points[1].z};
    maple.colors[0]=maple.head_color;maple.head_color=0xffffff|(vm->color&0xff000000u);
    return (maple.colors[31]&0xff000000u)==0;
}
void extra_maple_draw(AnmVm* vm) {
    auto& maple=*static_cast<Maple*>(vm->callback_object);RibbonVertex vertices[64];
    for(int n=0;n<32;++n) {
        const Point p=maple.points[n],d=maple.directions[n];
        const float length=std::hypot(d.x,d.y);
        // D3DXVec3Normalize returns zero for the initial zero-length tangent.
        const float nx=length==0?0:d.y/length*5,ny=length==0?0:-d.x/length*5;
        const float u=vm->uv[0]+(vm->uv[2]-vm->uv[0])*float(n+1)/32;
        vertices[n*2]={p.x-nx,p.y-ny,p.z,maple.colors[n],u,vm->uv[3]};
        vertices[n*2+1]={p.x+nx,p.y+ny,p.z,maple.colors[n],u,vm->uv[1]};
    }
    auto* device=gfx_device();D3DMATRIX world{};world._11=world._22=world._33=world._44=1;
    device->SetTransform(D3DTS_WORLD,&world);texture_state(device);anm_apply_blend(vm);
    const DWORD filter=(vm->flags3&0x2000)?D3DTEXF_POINT:D3DTEXF_LINEAR;
    device->SetSamplerState(0,D3DSAMP_MINFILTER,filter);
    device->SetSamplerState(0,D3DSAMP_MAGFILTER,filter);
    const D3DTEXTUREADDRESS address[]={D3DTADDRESS_WRAP,D3DTADDRESS_CLAMP,D3DTADDRESS_MIRROR};
    device->SetSamplerState(0,D3DSAMP_ADDRESSU,address[(vm->flags3>>2)&3]);
    device->SetSamplerState(0,D3DSAMP_ADDRESSV,address[vm->flags3&3]);
    device->SetTexture(0,static_cast<IDirect3DTexture9*>(const_cast<void*>(vm->texture)));
    device->SetRenderState(D3DRS_ZENABLE,TRUE);device->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
    device->SetRenderState(D3DRS_ZFUNC,D3DCMP_ALWAYS);
    device->SetFVF(D3DFVF_XYZ|D3DFVF_DIFFUSE|D3DFVF_TEX1);
    device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,62,vertices,sizeof(RibbonVertex));
}
void extra_maple_free(AnmVm* vm) {delete static_cast<Maple*>(vm->callback_object);vm->callback_object=nullptr;}

namespace {
Point charge_polar(float direction,float radius) {
    return {std::cos(direction)*radius,std::sin(direction)*radius,0};
}
Point charge_add(Point a,Point b) {return {a.x+b.x,a.y+b.y,a.z+b.z};}
Point charge_sub(Point a,Point b) {return {a.x-b.x,a.y-b.y,a.z-b.z};}
Point charge_scale(Point a,float scale) {return {a.x*scale,a.y*scale,a.z*scale};}
Point charge_normalize(Point a) {
    const float length=std::sqrt(a.x*a.x+a.y*a.y+a.z*a.z);
    return length==0?Point{}:charge_scale(a,1.0f/length);
}
Point charge_jitter(float radius) {
    // 405030 uses the visual RNG (4cf288), angle before distance.
    const float direction=(float(rng_next(card_rng()))*4.6566129e-10f-1.0f)*pi;
    return charge_polar(direction,visual_random()*radius);
}
void charge_path(AnmVm* vm,int duration,Point start,Point end,Point tangent_start,Point tangent_end) {
    // 405ad0 / ANM position interpolator: cubic Hermite, mode 8.
    auto* raw=reinterpret_cast<uint8_t*>(vm)+0x90;
    std::memcpy(raw,&start,12);std::memcpy(raw+12,&end,12);
    std::memcpy(raw+24,&tangent_start,12);std::memcpy(raw+36,&tangent_end,12);
    auto* ints=reinterpret_cast<int32_t*>(raw);
    ints[15]=-1;ints[16]=0;ints[17]=0;ints[18]=0;ints[19]|=1;
    ints[20]=duration;ints[21]=8;
}
}

void extra_charge_start(AnmVm* vm) {
    vm->callback_object=new Charge{};
    vm->callback_index=2;
}
bool extra_charge_update(AnmVm* vm) {
    auto& charge=*static_cast<Charge*>(vm->callback_object);
    const Point center{vm->extra_x,vm->extra_y,vm->extra_z};
    const float curve=*reinterpret_cast<float*>(reinterpret_cast<uint8_t*>(vm)+0x4cc);
    const int duration=*reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(vm)+0x4a4);
    const Point source=charge_add(center,charge_polar(vm->rot_z,300));
    const Point middle=charge_add(center,charge_polar(vm->rot_z+curve,150));
    if(charge.time!=charge.previous && charge.time<50) {
        for(int n=0;n<4;++n) {
            const int slot=charge.time*4+n;
            auto* particle=anm_vm_alloc(anm_manager()->resources[8]);
            anm_vm_set_script(particle,anm_manager()->resources[8],n==3?151:150);
            particle->flags3|=0x1000u;
            anm_queue_add(particle);anm_manage_vm(particle,&charge.particles[slot]);
            particle->color=vm->color;
            if(n==3) {
                uint32_t rgb=0;
                for(int shift=0;shift<24;shift+=8)
                    rgb|=uint32_t(uint8_t(300-((vm->color>>shift)&255)))<<shift;
                particle->color=(vm->color&0xff000000u)|rgb;
            }
            *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(particle)+0x4a4)=duration;
        }
    }
    int active=0;
    for(int n=0;n<200;++n) {
        auto* particle=charge.particles[n];
        if(!particle)continue;
        if(charge.phase[n]==0) {
            const Point start=charge_add(source,charge_jitter(150));
            const Point end=charge_add(middle,charge_jitter(50));
            const Point direction=charge_normalize(charge_sub(end,start));
            const Point bend=charge_normalize(charge_add(direction,charge_normalize(charge_sub(center,end))));
            const Point tangent_end=charge_scale(bend,visual_random()*200+200);
            const Point tangent_start=charge_scale(direction,visual_random()*100+100);
            charge_path(particle,duration,start,end,tangent_start,tangent_end);
            charge.middle[n]=end;charge.tangent[n]=tangent_end;charge.phase[n]=1;
        } else if(charge.phase[n]==1 && particle->elapsed>=duration+1) {
            const Point end=charge_add(center,charge_jitter(20));
            const Point tangent_end=charge_jitter(20);
            charge_path(particle,duration,charge.middle[n],end,charge.tangent[n],tangent_end);
            charge.phase[n]=2;
        }
        ++active;
    }
    if(active==0)return true;
    charge.previous=charge.time;
    charge.fraction+=stage::g_speed_scale>0.99f && stage::g_speed_scale<1.01f?1.0f:stage::g_speed_scale;
    charge.time=int(charge.fraction);
    return false;
}
void extra_charge_label(AnmVm* vm,int label) {
    if(label!=1)return;
    auto& charge=*static_cast<Charge*>(vm->callback_object);
    charge.previous=charge.time;
    charge.fraction+=300*(stage::g_speed_scale>0.99f && stage::g_speed_scale<1.01f?1.0f:stage::g_speed_scale);
    charge.time=int(charge.fraction);
}
void extra_charge_free(AnmVm* vm) {
    auto* charge=static_cast<Charge*>(vm->callback_object);
    for(auto* particle:charge->particles)if(particle) {
        // 4058c0 marks particles deleted; the ANM pass reclaims them later.
        particle->script_cursor=-1;particle->flags2&=~1u;
        anm_manage_vm(particle,nullptr);
    }
    delete charge;vm->callback_object=nullptr;vm->callback_index=0;
}

void extra_aura_free(stage::Enemy& enemy) {
    for(auto it=auras.begin();it!=auras.end();++it)if(it->enemy==&enemy){auras.erase(it);break;}
}
void extra_aura_set(stage::Enemy& enemy,float radius,uint32_t color) {
    extra_aura_free(enemy);
    if(radius<=0)return;
    // 42b1f0 appends each new aura's strips to the ANM draw queue.
    auras.push_back(Aura{});auto& aura=auras.back();aura.enemy=&enemy;aura.target=radius;aura.radius=16;aura.color=color;
}
void extra_aura_update(stage::Enemy& enemy) {
    for(auto& aura:auras)if(aura.enemy==&enemy) {
        // 42f890 evaluates the old radius, then expands it by two world units.
        const float radius=aura.radius,diameter=radius*2+40;
        if(radius<aura.target)aura.radius+=stage::g_speed_scale*2;
        const float cx=enemy.x+224,cy=enemy.y+16;
        float phase_x=aura.phase_x,phase_y=aura.phase_y;
        for(int x=0;x<17;++x)for(int y=0;y<17;++y) {
            float sx=cx-radius-20+diameter*x/16,sy=cy-radius-20+diameter*y/16;
            float dx=sx-cx,dy=sy-cy,distance=dx*dx+dy*dy;
            float px=sx,py=sy;uint32_t color=0xffffff;
            if(distance<=radius*radius) {
                const float ratio=(radius*radius-distance)/(radius*radius);
                // 42f890 tests the XY radius, then normalizes the 3D delta
                // from the source grid (Z=0) to the enemy's world position.
                const float length=std::sqrt(distance+enemy.z*enemy.z);
                const float normal_x=length==0?0:dx/length,normal_y=length==0?0:dy/length;
                px+=normal_x*ratio*32+std::sin(phase_x)*ratio*8;
                py+=normal_y*ratio*32+std::sin(phase_y)*ratio*8;
                color=0xff000000;
                for(int shift:{0,8,16})color|=uint32_t(255-float(255-((aura.color>>shift)&255))*ratio)<<shift;
            }
            phase_x=angle(phase_x+pi/32);phase_y=angle(phase_y-pi/64);
            if(sx<=32){sx=33;px=33;}else if(sx>=416){sx=415;px=415;}
            if(sy<=16){sy=17;py=17;}else if(sy>=464){sy=463;py=463;}
            aura.vertices[x*17+y]={px,py,0,1,color,sx/640,sy/480};
        }
        aura.phase_x=angle(aura.phase_x+stage::g_speed_scale*pi/16);
        aura.phase_y=angle(aura.phase_y+stage::g_speed_scale*pi/32);
        break;
    }
}
