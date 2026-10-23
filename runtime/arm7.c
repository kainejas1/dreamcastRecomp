#include "arm7.h"
#include <stdio.h>
#include <string.h>

#define N (1u<<31)
#define Z (1u<<30)
#define C (1u<<29)
#define V (1u<<28)
#define I (1u<<7)
#define F (1u<<6)
#define T (1u<<5)
#define MM 31u
#define USR 0x10u
#define FIQ 0x11u
#define IRQ 0x12u
#define SVC 0x13u
#define ABT 0x17u
#define UND 0x1bu
#define SYS 0x1fu

typedef struct { uint32_t v; unsigned c; } shres;
static uint32_t r[16], cpsr;
static uint32_t spsr_fiq, spsr_irq, spsr_svc, spsr_abt, spsr_und;
static uint32_t user_hi[7], fiq_hi[7], user_sp_lr[2], irq_sp_lr[2], svc_sp_lr[2], abt_sp_lr[2], und_sp_lr[2];
static int fiq_line, stopped;
static arm7_bus bus;

static uint32_t *spsr_for(uint32_t mode) {
    switch (mode & MM) { case FIQ:return &spsr_fiq; case IRQ:return &spsr_irq; case SVC:return &spsr_svc; case ABT:return &spsr_abt; case UND:return &spsr_und; default:return NULL; }
}

static int valid_mode(uint32_t m) {
    switch (m & MM) { case USR: case FIQ: case IRQ: case SVC: case ABT: case UND: case SYS:return 1; default:return 0; }
}

static void save_bank(uint32_t m) {
    m &= MM;
    if (m == FIQ) memcpy(fiq_hi, &r[8], sizeof fiq_hi);
    else {
        memcpy(user_hi, &r[8], 5*sizeof(uint32_t));
        if (m == IRQ) memcpy(irq_sp_lr, &r[13], sizeof irq_sp_lr);
        else if (m == SVC) memcpy(svc_sp_lr, &r[13], sizeof svc_sp_lr);
        else if (m == ABT) memcpy(abt_sp_lr, &r[13], sizeof abt_sp_lr);
        else if (m == UND) memcpy(und_sp_lr, &r[13], sizeof und_sp_lr);
        else memcpy(user_sp_lr, &r[13], sizeof user_sp_lr);
    }
}

static void load_bank(uint32_t m) {
    m &= MM;
    if (m == FIQ) memcpy(&r[8], fiq_hi, sizeof fiq_hi);
    else {
        memcpy(&r[8], user_hi, 5*sizeof(uint32_t));
        if (m == IRQ) memcpy(&r[13], irq_sp_lr, sizeof irq_sp_lr);
        else if (m == SVC) memcpy(&r[13], svc_sp_lr, sizeof svc_sp_lr);
        else if (m == ABT) memcpy(&r[13], abt_sp_lr, sizeof abt_sp_lr);
        else if (m == UND) memcpy(&r[13], und_sp_lr, sizeof und_sp_lr);
        else memcpy(&r[13], user_sp_lr, sizeof user_sp_lr);
    }
}

static void switch_mode(uint32_t m) {
    uint32_t old = cpsr & MM; m &= MM; 
    if (old == m) return; 
    save_bank(old); load_bank(m); cpsr = (cpsr & ~MM) | m;
}

static uint32_t rr(unsigned n, uint32_t pc) { return (n == 15) ? pc+8u : r[n&15u]; }
static uint8_t rd8(uint32_t a) { if(bus.read8)return bus.read8(a,bus.user); fprintf(stderr,"[arm7] unmapped read8 %08x\n",a); return 0xff; }
static uint32_t rd32(uint32_t a) { if(bus.read32)return bus.read32(a,bus.user); fprintf(stderr,"[arm7] unmapped read32 %08x\n",a); return 0xffffffffu; }
static void wr8(uint32_t a,uint8_t v) { if(bus.write8)bus.write8(a,v,bus.user); else fprintf(stderr,"[arm7] unmapped write8 %08x <- %02x\n",a,v); }
static void wr32(uint32_t a,uint32_t v) { if(bus.write32)bus.write32(a,v,bus.user); else fprintf(stderr,"[arm7] unmapped write32 %08x <- %08x\n",a,v); }
static uint32_t ror(uint32_t v,unsigned n) { n &= 31u; return n ? (v>>n)|(v<<(32u-n)) : v; }
static int cond_ok(unsigned q) {
    unsigned n=!!(cpsr&N), z=!!(cpsr&Z), c=!!(cpsr&C), v=!!(cpsr&V);
    switch(q){case 0:return z;case 1:return !z;case 2:return c;case 3:return !c;case 4:return n;case 5:return !n;case 6:return v;case 7:return !v;case 8:return c&&!z;case 9:return !c||z;case 10:return n==v;case 11:return n!=v;case 12:return !z&&n==v;case 13:return z||n!=v;case 14:return 1;default:return 0;}
}

static shres shift(uint32_t x,unsigned type,unsigned amount,int imm,unsigned carry) {
    shres o={x,carry};
    if(imm){
        if(type==0){if(amount){o.c=(x>>(32u-amount))&1u;o.v=x<<amount;}}
        else if(type==1){if(!amount)amount=32;if(amount<32){o.c=(x>>(amount-1u))&1u;o.v=x>>amount;}else{o.c=x>>31;o.v=0;}}
        else if(type==2){if(!amount)amount=32;o.c=(x>>(amount>=32?31u:amount-1u))&1u;o.v=amount>=32?((x&N)?0xffffffffu:0):(uint32_t)((int32_t)x>>amount);}
        else if(!amount){o.c=x&1u;o.v=(carry<<31)|(x>>1);}else{o.v=ror(x,amount);o.c=o.v>>31;}
        return o;
    }
    if(!amount)return o;
    if(type==0){if(amount<32){o.c=(x>>(32u-amount))&1u;o.v=x<<amount;}else if(amount==32){o.v=0;o.c=x&1u;}else{o.v=0;o.c=0;}}
    else if(type==1){if(amount<32){o.c=(x>>(amount-1u))&1u;o.v=x>>amount;}else if(amount==32){o.v=0;o.c=x>>31;}else{o.v=0;o.c=0;}}
    else if(type==2){if(amount>=32){o.c=x>>31;o.v=(x&N)?0xffffffffu:0;}else{o.c=(x>>(amount-1u))&1u;o.v=(uint32_t)((int32_t)x>>amount);}}
    else {unsigned k=amount&31u;if(!k){o.v=x;o.c=x>>31;}else{o.v=ror(x,k);o.c=o.v>>31;}}
    return o;
}
static shres op2(uint32_t w,uint32_t pc) {
    unsigned carry=!!(cpsr&C);
    if(w&(1u<<25)){unsigned rot=((w>>8)&15u)*2u;shres o={ror(w&255u,rot),carry};if(rot)o.c=o.v>>31;return o;}
    uint32_t x=rr(w&15u,pc);unsigned type=(w>>5)&3u;
    if(w&(1u<<4))return shift(x,type,rr((w>>8)&15u,pc)&255u,0,carry);
    return shift(x,type,(w>>7)&31u,1,carry);
}
static uint32_t set_nz(uint32_t x,uint32_t flags){flags&=~(N|Z);if(x&N)flags|=N;if(!x)flags|=Z;return flags;}
static uint32_t flags_add(uint32_t a,uint32_t b,uint32_t ci,uint32_t res,uint32_t f){uint64_t sum=(uint64_t)a+b+ci;f=set_nz(res,f)&~(C|V);if(sum>>32)f|=C;if((~(a^b)&(a^res)&N))f|=V;return f;}
static uint32_t flags_sub(uint32_t a,uint32_t b,uint32_t bi,uint32_t res,uint32_t f){uint64_t rhs=(uint64_t)b+bi;f=set_nz(res,f)&~(C|V);if((uint64_t)a>=rhs)f|=C;if(((a^b)&(a^res)&N))f|=V;return f;}
static void restore_spsr(void){uint32_t *s=spsr_for(cpsr&MM);if(!s)return;uint32_t f=*s;if(!valid_mode(f&MM)){fprintf(stderr,"[arm7] invalid SPSR mode %x\n",f&MM);return;}switch_mode(f&MM);cpsr=f&~T;}
static void set_pc(uint32_t target,int restore){if(restore)restore_spsr();r[15]=target&~3u;}
static void exception(uint32_t mode,uint32_t vector,uint32_t lr,int mask_f){uint32_t old=cpsr;uint32_t *s=spsr_for(mode);if(s)*s=old;switch_mode(mode);r[14]=lr;cpsr|=I;if(mask_f)cpsr|=F;cpsr&=~T;r[15]=vector;}
static void undef(uint32_t pc,uint32_t w){fprintf(stderr,"[arm7] undefined instruction %08x at %08x; stopping ARM7\n",w,pc);stopped=1;}
static void data_proc(uint32_t w,uint32_t pc){
    unsigned op=(w>>21)&15u,s=(w>>20)&1u,rn=(w>>16)&15u,rd=(w>>12)&15u;
    uint32_t a=rr(rn,pc),old=cpsr;shres sh=op2(w,pc);uint32_t b=sh.v,res=0;int wr=1,kind=0;
    switch(op){case 0:res=a&b;break;case 1:res=a^b;break;case 2:res=a-b;kind=1;break;case 3:res=b-a;kind=2;break;case 4:res=a+b;kind=3;break;case 5:res=a+b+!!(old&C);kind=4;break;case 6:res=a-b-!(old&C);kind=5;break;case 7:res=b-a-!(old&C);kind=6;break;case 8:res=a&b;wr=0;s=1;break;case 9:res=a^b;wr=0;s=1;break;case 10:res=a-b;wr=0;s=1;kind=1;break;case 11:res=a+b;wr=0;s=1;kind=3;break;case 12:res=a|b;break;case 13:res=b;break;case 14:res=a&~b;break;case 15:res=~b;break;}
    if(s){if(kind==1)cpsr=flags_sub(a,b,0,res,cpsr);else if(kind==2)cpsr=flags_sub(b,a,0,res,cpsr);else if(kind==3)cpsr=flags_add(a,b,0,res,cpsr);else if(kind==4)cpsr=flags_add(a,b,!!(old&C),res,cpsr);else if(kind==5)cpsr=flags_sub(a,b,!(old&C),res,cpsr);else if(kind==6)cpsr=flags_sub(b,a,!(old&C),res,cpsr);else{cpsr=set_nz(res,cpsr);cpsr=(cpsr&~C)|(sh.c?C:0);}}
    if(wr){if(rd==15)set_pc(res,s);else r[rd]=res;}
}
static void multiply(uint32_t w){unsigned acc=(w>>21)&1u,s=(w>>20)&1u,rd=(w>>16)&15u,rn=(w>>12)&15u,rs=(w>>8)&15u,rm=w&15u;uint32_t v=r[rm]*r[rs];if(acc)v+=r[rn];r[rd]=v;if(s)cpsr=set_nz(v,cpsr);}
static uint32_t transfer_offset(uint32_t w,uint32_t pc){if(!(w&(1u<<25)))return w&0xfffu;unsigned rm=w&15u,t=(w>>5)&3u,amt=(w>>7)&31u;return shift(rr(rm,pc),t,amt,1,!!(cpsr&C)).v;}
static void single_transfer(uint32_t w,uint32_t pc){
    unsigned p=(w>>24)&1u,u=(w>>23)&1u,b=(w>>22)&1u,wb=(w>>21)&1u,l=(w>>20)&1u,rn=(w>>16)&15u,rd=(w>>12)&15u;
    uint32_t base=rr(rn,pc),off=transfer_offset(w,pc),idx=u?base+off:base-off,addr=p?idx:base;int writeback=!p||wb;
    if(l){uint32_t value;if(b)value=rd8(addr);else value=ror(rd32(addr&~3u),(addr&3u)*8u);if(writeback&&rn!=rd)r[rn]=idx;if(rd==15)set_pc(value,0);else r[rd]=value;}
    else{uint32_t value=rd==15?pc+12u:r[rd];if(b)wr8(addr,(uint8_t)value);else wr32(addr&~3u,value);if(writeback)r[rn]=idx;}
}
static unsigned count_regs(uint32_t x){unsigned n=0;x&=65535u;while(x){x&=x-1u;++n;}return n;}
static void block_transfer(uint32_t w,uint32_t pc){
    unsigned p=(w>>24)&1u,u=(w>>23)&1u,s=(w>>22)&1u,wb=(w>>21)&1u,l=(w>>20)&1u,rn=(w>>16)&15u;uint32_t list=w&65535u;unsigned count=count_regs(list);
    if(!count){list=1u<<15;count=16;}
    uint32_t base=rr(rn,pc),addr=u?base+(p?4u:0u):base-4u*count+(p?0u:4u),loaded_pc=0;int has_pc=0;
    int user_transfer=s&&!(list&(1u<<15));
    for(unsigned k=0;k<16;k++)if(list&(1u<<k)){
        if(l){uint32_t val=rd32(addr&~3u);if(k==15){loaded_pc=val;has_pc=1;}else if(user_transfer&&k>=8){if(k<=12)user_hi[k-8]=val;else user_sp_lr[k-13]=val;}else r[k]=val;}
        else{uint32_t val;if(k==15)val=pc+12u;else if(user_transfer&&k>=8){if(k<=12)val=user_hi[k-8];else val=user_sp_lr[k-13];}else val=r[k];wr32(addr&~3u,val);}addr+=4u;
    }
    if(wb&&!(l&&(list&(1u<<rn))))r[rn]=u?base+4u*count:base-4u*count;   // ldm into the base: the loaded value wins
    if(has_pc)set_pc(loaded_pc,s!=0);
}
static void branch(uint32_t w,uint32_t pc){int32_t off=(int32_t)(w<<8)>>6;if(w&(1u<<24))r[14]=pc+4u;r[15]=pc+8u+(uint32_t)off;}
static void mrs(uint32_t w){unsigned rd=(w>>12)&15u;uint32_t *s=(w&(1u<<22))?spsr_for(cpsr&MM):NULL;r[rd]=(w&(1u<<22))?(s?*s:cpsr):cpsr;}
static void msr(uint32_t w,uint32_t pc){uint32_t val;if(w&(1u<<25))val=ror(w&255u,((w>>8)&15u)*2u);else val=rr(w&15u,pc);unsigned f=(w>>16)&15u;uint32_t mask=0;if(f&1)mask|=0xffu;if(f&2)mask|=0xff00u;if(f&4)mask|=0xff0000u;if(f&8)mask|=0xff000000u;
    if(w&(1u<<22)){uint32_t *s=spsr_for(cpsr&MM);if(s)*s=(*s&~mask)|(val&mask);return;}
    if ((cpsr & MM) == USR) mask &= 0xff000000u;
    uint32_t next = (cpsr & ~mask) | (val & mask);
    next &= ~T;
    if (!valid_mode(next & MM)) {
        fprintf(stderr, "[arm7] ignored invalid CPSR mode %x\n", next & MM);
        next = (next & ~MM) | (cpsr & MM);
    }
    switch_mode(next & MM);
    cpsr = next;
}
static int execute_one(void){
    uint32_t pc=r[15],w=rd32(pc&~3u);r[15]=pc+4u;
    if(!cond_ok(w>>28))return 1;
    if((w&0x0e000000u)==0x0a000000u){branch(w,pc);return 1;}
    if((w&0x0f000000u)==0x0f000000u){exception(SVC,8u,pc+4u,0);return 1;}
    if((w&0x0c000000u)==0x04000000u){single_transfer(w,pc);return 1;}
    if((w&0x0e000000u)==0x08000000u){block_transfer(w,pc);return 1;}
    if((w&0x0fc000f0u)==0x00000090u){multiply(w);return 1;}
    if((w&0x0fb00ff0u)==0x01000090u){unsigned by=(w>>22)&1u,rn=(w>>16)&15u,rd=(w>>12)&15u,rm=w&15u;uint32_t a=r[rn];if(by){uint8_t old=rd8(a);wr8(a,(uint8_t)r[rm]);r[rd]=old;}else{uint32_t old=rd32(a&~3u);wr32(a&~3u,r[rm]);r[rd]=ror(old,(a&3u)*8u);}return 1;}
    if((w&0x0fbf0fffu)==0x010f0000u){mrs(w);return 1;}
    if((w&0x0db0f000u)==0x0120f000u){msr(w,pc);return 1;}
    if((w&0x0c000000u)==0){if(!(w&(1u<<25))&&(w&(1u<<4))&&(w&(1u<<7))){undef(pc,w);return 1;}data_proc(w,pc);return 1;}
    undef(pc,w);return 1;
}
void arm7_set_bus(const arm7_bus *b){if(b)bus=*b;else memset(&bus,0,sizeof bus);}
void arm7_reset(void){stopped=0;memset(r,0,sizeof r);memset(&spsr_fiq,0,sizeof spsr_fiq);memset(&spsr_irq,0,sizeof spsr_irq);memset(&spsr_svc,0,sizeof spsr_svc);memset(&spsr_abt,0,sizeof spsr_abt);memset(&spsr_und,0,sizeof spsr_und);memset(user_hi,0,sizeof user_hi);memset(fiq_hi,0,sizeof fiq_hi);memset(user_sp_lr,0,sizeof user_sp_lr);memset(irq_sp_lr,0,sizeof irq_sp_lr);memset(svc_sp_lr,0,sizeof svc_sp_lr);memset(abt_sp_lr,0,sizeof abt_sp_lr);memset(und_sp_lr,0,sizeof und_sp_lr);fiq_line=0;cpsr=SVC|I|F;r[15]=0;}
// take a pending FIQ before each instruction; lr = where to go on + 4 (the
// handler does sub lr, lr, #4)
int arm7_run(int n){int done=0;while(done<n&&!stopped){if(fiq_line&&!(cpsr&F))exception(FIQ,0x1cu,r[15]+4u,1);done+=execute_one();}return done;}
void arm7_fiq(int level){fiq_line=level!=0;}
uint32_t arm7_reg(int i){return r[i&15];}
uint32_t arm7_cpsr(void){return cpsr;}
