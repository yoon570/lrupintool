#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <time.h>
#include <math.h>
#include <errno.h>

#ifndef PAGE_SIZE
#  define PAGE_SIZE 4096
#endif

/* ───────── RNG: 64-bit xorshift* ───────── */
static inline uint64_t xorshift64(uint64_t *s){
    uint64_t x=*s; x^=x>>12; x^=x<<25; x^=x>>27;
    return *s=x*0x2545F4914F6CDD1DULL;
}
static inline double rng_double(uint64_t *seed){
    return (xorshift64(seed)>>11)* (1.0/9007199254740992.0);
}

/* ───────── CLI config ───────── */
typedef enum { AM_SPLIT, AM_LINEAR, AM_UNIFORM } AccessMode;
typedef struct {
    AccessMode mode;
    double hot_pages_frac;     /* split */
    double hot_access_frac;    /* split */
    int    reverse;            /* linear */
    int    use_mmap;
    int    progress_step;
} cfg_t;

static void parse_args(int argc,char**argv,long*pages,long*iters,cfg_t*cfg){
    if(argc<3){
        fprintf(stderr,
         "Usage: %s <pages> <iters> [options]\n"
         "  --mode split|linear|uniform   (default split)\n"
         "  --hot-pages f   --hot-accesses f   (split)\n"
         "  --reverse                         (linear)\n"
         "  --mmap            --progress n\n",argv[0]); exit(1);
    }
    *pages=atol(argv[1]); *iters=atol(argv[2]);
    if(*pages<=0||*iters<=0){fprintf(stderr,"pages/iters >0\n"); exit(1);}

    cfg->mode=AM_SPLIT; cfg->hot_pages_frac=0.20; cfg->hot_access_frac=0.80;
    cfg->reverse=0; cfg->use_mmap=0; cfg->progress_step=20;

    for(int i=3;i<argc;++i){
        if(!strcmp(argv[i],"--mode") && i+1<argc){
            ++i;
            if(!strcmp(argv[i],"split"))   cfg->mode=AM_SPLIT;
            else if(!strcmp(argv[i],"linear")) cfg->mode=AM_LINEAR;
            else if(!strcmp(argv[i],"uniform"))cfg->mode=AM_UNIFORM;
            else{fprintf(stderr,"bad --mode\n"); exit(1);}
        }else if(!strcmp(argv[i],"--hot-pages")&&i+1<argc){
            cfg->hot_pages_frac=atof(argv[++i]);
        }else if(!strcmp(argv[i],"--hot-accesses")&&i+1<argc){
            cfg->hot_access_frac=atof(argv[++i]);
        }else if(!strcmp(argv[i],"--reverse")){
            cfg->reverse=1;
        }else if(!strcmp(argv[i],"--mmap")){
            cfg->use_mmap=1;
        }else if(!strcmp(argv[i],"--progress")&&i+1<argc){
            cfg->progress_step=atoi(argv[++i]);
        }else{fprintf(stderr,"unknown option %s\n",argv[i]); exit(1);}
    }
    if(cfg->hot_pages_frac<=0||cfg->hot_pages_frac>=1||
       cfg->hot_access_frac<=0||cfg->hot_access_frac>=1){
        fprintf(stderr,"fractions must be in (0,1)\n"); exit(1);
    }
}

/* ───────── samplers ───────── */
static inline long samp_split(uint64_t*s,long hs,long hp,long cp,double A){
    return (rng_double(s)<A)? hs+(long)(rng_double(s)*hp)
                            : (long)(rng_double(s)*cp);
}
static inline long samp_linear(uint64_t*s,long n,int rev){
    uint64_t sum=(uint64_t)n*(n+1)/2, r=xorshift64(s)%sum;
    long idx=(long)floor((sqrt(8.0*r+1)-1)/2); if(idx>=n) idx=n-1;
    return rev? n-1-idx: idx;
}
static inline long samp_uniform(uint64_t*s,long n){
    return (long)(rng_double(s)*n);
}

/* ───────── touch helper ───────── */
static inline void touch(volatile char*p){volatile char v=*p;(void)v;}

/* ───────── main ───────── */
int main(int argc,char**argv){
    long pages,iters; cfg_t cfg; parse_args(argc,argv,&pages,&iters,&cfg);

    long hot_pages=(long)(pages*cfg.hot_pages_frac+0.5);
    if(hot_pages<1)hot_pages=1; if(hot_pages>pages)hot_pages=pages;
    long cold_pages=pages-hot_pages, hot_start=cold_pages;

    size_t bytes=(size_t)pages*PAGE_SIZE;
    volatile char*region = cfg.use_mmap
        ? (volatile char*)mmap(NULL,bytes,PROT_READ|PROT_WRITE,
                               MAP_PRIVATE|MAP_ANONYMOUS,-1,0)
        : (volatile char*)malloc(bytes);
    if(region==MAP_FAILED||!region){perror("alloc"); return 1;}

    for(long i=0;i<pages;++i) touch(region+i*PAGE_SIZE); /* fault in */

    uint64_t seed=(uint64_t)time(NULL)^(uintptr_t)&seed;
    int last_blk=-1;

    for(long i=0;i<iters;++i){
        long pg;
        switch(cfg.mode){
            case AM_SPLIT:   pg=samp_split(&seed,hot_start,hot_pages,
                                           cold_pages,cfg.hot_access_frac);break;
            case AM_LINEAR:  pg=samp_linear(&seed,pages,cfg.reverse);break;
            default:         pg=samp_uniform(&seed,pages);
        }
        touch(region+pg*PAGE_SIZE);

        if(cfg.progress_step){
            int pct=(int)(((i+1)*100)/iters);
            if(pct/cfg.progress_step!=last_blk&&pct%cfg.progress_step==0){
                last_blk=pct/cfg.progress_step; printf("%d%% ",pct); fflush(stdout);
            }
        }
    }
    if(cfg.progress_step)putchar('\n');

    if(cfg.use_mmap) munmap((void*)region,bytes); else free((void*)region);
    return 0;
}
