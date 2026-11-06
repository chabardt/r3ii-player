/* i18n.c — see i18n.h. The tables live in lang_tables.c (generated from lang/*.po). */
#include "i18n.h"
#include "lang_tables.h"
#include <stdio.h>
#include <string.h>

#define LANG_FILE "/usr/data/r3_lang"
static int g_lang;                     /* 0 = English (no table) */

int  i18n_count(void){ return 1+LANG_N; }
const char* i18n_code(int i){ return i<=0||i>LANG_N ? "en" : LANGS[i-1].code; }
const char* i18n_name(int i){ return i<=0||i>LANG_N ? "English" : LANGS[i-1].name; }
int  i18n_current(void){ return g_lang; }

const char* i18n_tr(const char*s){
    if(!g_lang || !s || !*s) return s;
    const lang_t*L=&LANGS[g_lang-1]; int lo=0, hi=L->n-1;      /* entries are sorted by msgid */
    while(lo<=hi){ int m=(lo+hi)/2, c=strcmp(s,L->e[m].id);
        if(!c) return L->e[m].str; if(c<0) hi=m-1; else lo=m+1; }
    return s;
}
void i18n_set(int i){
    g_lang = i<0||i>LANG_N ? 0 : i;
    FILE*f=fopen(LANG_FILE,"w"); if(f){ fprintf(f,"%s\n",i18n_code(g_lang)); fclose(f); } }
void i18n_load(void){
    char c[16]=""; FILE*f=fopen(LANG_FILE,"r"); if(!f) return;
    if(fscanf(f,"%15s",c)!=1) c[0]=0; fclose(f);
    for(int i=1;i<=LANG_N;i++) if(!strcmp(c,LANGS[i-1].code)) g_lang=i; }
