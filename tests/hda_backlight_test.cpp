#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/mman.h>
#include <vector>
#include <unistd.h>
#include "drivers/audio/hda.hpp"
#include "drivers/backlight.hpp"
static uint8_t* bar; static volatile bool stop_=false;
extern "C" uint64_t pmm_alloc_contiguous(uint64_t c){ void*p=mmap(0,c*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0); return p==MAP_FAILED?0:(uint64_t)p; }
extern "C" void aos_serial_write(const char*s){ fputs(s,stdout);} extern "C" void aos_serial_write_hex64(uint64_t v){printf("0x%llx",(unsigned long long)v);}
uint64_t timer_ticks(){return 0;}
#include "memory/virtual/vmm.hpp"
namespace aos::vmm{ bool map_mmio(uint64_t,uint64_t,CacheMode){return true;} }
static uint8_t* gpu;
extern "C" uint32_t pci_config_read(uint8_t b,uint8_t s,uint8_t f,uint8_t o){
  if(b==0&&s==5&&f==0){ if(o==0) return 0x293E8086; if(o==0x0C) return 0; if(o==8) return 0x04030000; if(o==4) return 0; if(o==0x10) return (uint32_t)(uint64_t)bar|4; if(o==0x14) return 0; }
  if(b==0&&s==2&&f==0){ if(o==0) return 0x91668086; if(o==0x0C) return 0; if(o==8) return 0x03000000; if(o==4) return 0; if(o==0x10) return (uint32_t)(uint64_t)gpu|4; if(o==0x14) return 0; }
  return 0xFFFFFFFF; }
extern "C" void pci_config_write(uint8_t,uint8_t,uint8_t,uint8_t,uint32_t){}
#define R8(o) (*(volatile uint8_t*)(bar+(o)))
#define R16(o) (*(volatile uint16_t*)(bar+(o)))
#define R32(o) (*(volatile uint32_t*)(bar+(o)))
// ---- fake codec: nid1 AFG; 2 DAC; 3 mixer(in DAC); 4 speaker pin (fixed) -> 3; 5 headphone pin (jack) -> 2; 6 mic pin (input only)
struct Log{uint32_t nid,id,payload;}; static std::vector<Log> vlog; static bool played=false; static int16_t first_nonzero=0; static uint32_t bdl_len0=0;
static uint32_t codec(uint32_t v){
  uint32_t nid=(v>>20)&0xFF; uint32_t id12=(v>>8)&0xFFF; uint32_t id4=(v>>16)&0xF;
  if(id12==0xF00){ uint32_t p=v&0xFF;
    if(p==0x04){ if(nid==0) return (1<<16)|1; if(nid==1) return (2<<16)|5; return 0; }
    if(p==0x05) return nid==1?1:0;
    if(p==0x09){ switch(nid){ case 2: return (0u<<20)|4|8|2; case 3: return (2u<<20)|0x100|4|2|8; case 4: return (4u<<20)|0x100; case 5: return (4u<<20)|0x100; case 6: return (4u<<20)|0x100; } return 0; }
    if(p==0x12) return (1u<<31)|(63<<8)|0;   // amp out caps
    if(p==0x0D) return (63<<8)|10;
    if(p==0x0C){ if(nid==6) return 0x20; return 0x10|0x10000; }
    if(p==0x0E){ if(nid==3) return 1; if(nid==4) return 1; if(nid==5) return 1; return 0; }
    return 0; }
  if(id12==0xF02){ if(nid==3) return 2; if(nid==4) return 3; if(nid==5) return 2; return 0; }
  if(id12==0xF1C){ if(nid==4) return (2u<<30)|(1u<<20); if(nid==5) return (0u<<30)|(2u<<20); if(nid==6) return (0u<<30)|(8u<<20); return 0; }
  vlog.push_back({nid, id12>=0x700?id12:id4, v&(id12>=0x700?0xFF:0xFFFF)}); return 0; }
static void* dev(void*){
  uint32_t crp=0, rwp=0;
  while(!stop_){
    if(!(R32(0x08)&1)) R16(0x0E)=0; else if(!R16(0x0E)) R16(0x0E)=1;
    if(R16(0x58)&0x8000){ R16(0x58)=0; rwp=0; }
    if(!(R16(0x4A)&0x8000) && (R8(0x4C)&2)){
      uint32_t wp=R16(0x48)&0xFF; uint32_t* corb=(uint32_t*)(*(volatile uint32_t*)(bar+0x40)|((uint64_t)*(volatile uint32_t*)(bar+0x44)<<32));
      uint32_t* rirb=(uint32_t*)(*(volatile uint32_t*)(bar+0x50)|((uint64_t)*(volatile uint32_t*)(bar+0x54)<<32));
      while(crp!=wp){ crp=(crp+1)%256; uint32_t r=codec(corb[crp]); rwp=(rwp+1)%256; rirb[rwp*2]=r; rirb[rwp*2+1]=0; __sync_synchronize(); R16(0x58)=rwp; R16(0x4A)=crp; } }
    // stream 0 of output: ISS=4 -> 0x100
    uint32_t ctl=R32(0x100)&0xFFFFFF;
    if(ctl&2){ if(!played){ uint32_t* bdl=(uint32_t*)(uint64_t)R32(0x118); uint8_t* buf=(uint8_t*)(uint64_t)bdl[0]; bdl_len0=bdl[2]; int16_t* s=(int16_t*)buf; for(int i=0;i<2000;i++) if(s[2*i]){first_nonzero=s[2*i];break;} played=true; } }
    else if(ctl&1){ /* in reset */ }
  }
  return 0; }
#define CHECK(c) do{ if(!(c)){printf("FAIL line %d: %s\n",__LINE__,#c); return 1;} }while(0)
static int count(uint32_t nid,uint32_t id){ int n=0; for(auto&l:vlog) if(l.nid==nid&&l.id==id) n++; return n; }
int main(){
  bar=(uint8_t*)mmap(0,65536,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  gpu=(uint8_t*)mmap(0,0x200000,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT,-1,0);
  R16(0x00)=0x4401; R8(0x4E)=0x42; R8(0x5E)=0x42;
  pthread_t th; pthread_create(&th,0,dev,0);
  CHECK(aos::audio::init());
  CHECK(aos::audio::available());
  printf("pins wired: %d\n",aos::audio::pin_count());
  CHECK(aos::audio::pin_count()==2);          // speaker + headphone, mic ignored
  CHECK(count(4,0x707)==1 && count(5,0x707)==1 && count(6,0x707)==0);
  CHECK(count(2,0x706)==1);                    // stream tag set on the DAC
  CHECK(count(2,0x2)==1);                      // format set
  CHECK(count(5,0x701)==0);                    // single connection: no select verb
  // volume: DAC amp gain 63*60/100=38 (rounded)
  size_t before=vlog.size(); aos::audio::set_volume(50);
  bool found=false; for(size_t i=before;i<vlog.size();i++) if(vlog[i].nid==2&&vlog[i].id==0x3){ uint32_t p=vlog[i].payload; printf("amp payload 0x%x\n",p); if((p&0x7F)==32 && !(p&0x80) && (p&0xB000)==0xB000) found=true; }
  CHECK(found);
  before=vlog.size(); aos::audio::set_mute(true);
  found=false; for(size_t i=before;i<vlog.size();i++) if(vlog[i].nid==2&&vlog[i].id==0x3&&(vlog[i].payload&0x80)) found=true;
  CHECK(found);
  aos::audio::tone(880,100); usleep(50000); CHECK(!played);  // muted: no playback
  aos::audio::set_mute(false);
  aos::audio::tone(880,100); usleep(100000);
  CHECK(played); CHECK(bdl_len0==24576); printf("first sample %d\n",first_nonzero);
  aos::audio::poll();
  // ---- backlight
  uint32_t* reg=(uint32_t*)(gpu+0xC8254); *reg=(1000u<<16)|1000u;
  CHECK(aos::backlight::init());
  aos::backlight::set(50); CHECK((*reg&0xFFFF)==500 && (*reg>>16)==1000);
  aos::backlight::set(0);  CHECK((*reg&0xFFFF)==50);   // clamped to 5%
  aos::backlight::set(100); CHECK((*reg&0xFFFF)==1000);
  stop_=true; printf("HDA+BACKLIGHT TEST OK\n"); return 0; }
