/* genre.c — see genre.h. Built from the genres found on a real 24k-track library: aliases fix
 * spellings / languages, then keyword rules (checked at word starts, in order) pick the family. */
#include "genre.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char* const FAMNAME[GF_N]={ "Metal","Rock","Punk","Electro","Hip-hop","Jazz / Blues / Soul",
                                        "Pop","Folk / World","Classical / Soundtrack" };
const char* genre_family_name(int f){ return f>=0&&f<GF_N ? FAMNAME[f] : "Unsorted"; }

/* lowercase, fold common accents, '-' '_' '.' -> ' ', collapse spaces */
static void norm(const char*in,char*out,int cap){
    int j=0, sp=1;
    for(const unsigned char*p=(const unsigned char*)in; *p && j<cap-1; p++){
        unsigned c=*p;
        if(c==0xC3 && p[1]){ unsigned d=p[1]; p++;            /* Latin-1 supplement */
            c = (d>=0xA0&&d<=0xA5)||(d>=0x80&&d<=0x85) ? 'a' : (d==0xA7||d==0x87) ? 'c' :
                (d>=0xA8&&d<=0xAB)||(d>=0x88&&d<=0x8B) ? 'e' : (d>=0xAC&&d<=0xAF)||(d>=0x8C&&d<=0x8F) ? 'i' :
                (d>=0xB2&&d<=0xB6)||(d>=0x92&&d<=0x96) ? 'o' : (d>=0xB9&&d<=0xBC)||(d>=0x99&&d<=0x9C) ? 'u' :
                d==0xB1||d==0x91 ? 'n' : d==0x9F ? 's' : ' '; }
        else if(c>=0x80) continue;                            /* other non-ASCII: drop */
        if(c=='-'||c=='_'||c=='.'||c=='('||c==')'||c=='"'||isspace(c)) c=' ';
        c=(unsigned)tolower((int)c);
        if(c==' '){ if(sp) continue; sp=1; } else sp=0;
        out[j++]=(char)c; }
    while(j>0 && out[j-1]==' ') j--;
    out[j]=0;
}

/* not a genre */
static const char* const JUNK[]={ "other","others","inconnu","unknown","genre","misc","miscellaneous","top 250",
    "unclassifiable","general","non music","interview","comedy","enfants","children","default","none","various",
    "blank","podcast","audiobook","speech","spoken word","nr","n a","na","255","divers",0 };

/* normalized spelling -> canonical name (display form) */
static const char* const ALIAS[][2]={
    {"metal","Metal"},{"metaux","Metal"},{"heavy","Heavy metal"},{"melodic death","Melodic death metal"},
    {"death","Death metal"},{"black","Black metal"},{"thrash","Thrash metal"},{"speed","Speed metal"},
    {"neue deutsche harte","Industrial metal"},{"ndh","Industrial metal"},{"nu metal","Nu metal"},{"numetal","Nu metal"},
    {"rock","Rock"},{"rock & roll","Rock and roll"},{"rock n roll","Rock and roll"},{"rock and roll","Rock and roll"},
    {"rock pop","Pop rock"},{"poprock","Pop rock"},{"pop/rock","Pop rock"},{"prog","Progressive rock"},
    {"progrock","Progressive rock"},{"prog rock","Progressive rock"},{"alternative","Alternative rock"},
    {"alternativ","Alternative rock"},{"alternative rock","Alternative rock"},{"alt rock","Alternative rock"},
    {"indie","Indie rock"},{"lo fi","Lo-fi"},{"lofi","Lo-fi"},{"glam","Glam rock"},{"avant garde","Avant-garde"},
    {"experimental","Experimental rock"},{"noise","Noise rock"},{"mod","Mod revival"},{"classic rock","Classic rock"},
    {"rock francais","Rock français"},{"french rock","Rock français"},{"stoner","Stoner rock"},
    {"punk","Punk"},{"punkrock","Punk rock"},{"punk rock","Punk rock"},{"alternative & punk","Punk rock"},
    {"pop punk","Pop punk"},{"poppunk","Pop punk"},{"post punk","Post-punk"},{"postpunk","Post-punk"},
    {"electronic","Electronic"},{"electronica","Electronic"},{"electro","Electro"},{"electronique","Electronic"},
    {"edm","EDM"},{"ebm","EBM"},{"idm","IDM"},{"dnb","Drum and bass"},{"drum n bass","Drum and bass"},
    {"drum & bass","Drum and bass"},{"breaks","Breakbeat"},{"synthpop","Synth pop"},{"synth pop","Synth pop"},
    {"leftfield","Leftfield"},{"abstract","Abstract electronic"},{"trip hop","Trip hop"},{"triphop","Trip hop"},
    {"hip hop","Hip-hop"},{"hiphop","Hip-hop"},{"hip hop rap","Hip-hop"},{"rap","Hip-hop"},{"rap general","Hip-hop"},
    {"rap et hip hop","Hip-hop"},{"rap & hip hop","Hip-hop"},{"rap francais","Rap français"},{"french rap","Rap français"},
    {"r&b","R&B"},{"rnb","R&B"},{"r and b","R&B"},{"rhythm & blues","R&B"},{"contemporary r&b","R&B"},
    {"general jazz","Jazz"},{"doo wop","Doo-wop"},{"soul","Soul"},{"funk","Funk"},
    {"chanson","Chanson française"},{"chanson francaise","Chanson française"},{"variete","Chanson française"},
    {"variete francaise","Chanson française"},{"chanson et musique de film","Chanson française"},
    {"pop","Pop"},{"europop","Europop"},{"euro pop","Europop"},{"ballad","Ballad"},{"a cappella","A cappella"},
    {"b o","Soundtrack"},{"bo","Soundtrack"},{"ost","Soundtrack"},{"bande originale","Soundtrack"},
    {"soundtrack","Soundtrack"},{"score","Soundtrack"},{"film score","Soundtrack"},{"musical","Musical"},
    {"instru","Instrumental"},{"classique","Classical"},{"musique classique","Classical"},
    {"musiques du monde","World"},{"dream","Dream pop"},{"oldies","Oldies"},{"rap fr","Rap français"},
    {"musique concrete","Musique concrète"},{"psytrance","Psytrance"},{"psy trance","Psytrance"},{"world music","World"},{"world","World"},{"dub","Dub"},
    {0,0} };

/* family keyword rules — checked in this order, a keyword must start a word */
static const struct { int fam; const char*kw; } RULE[]={
    {GF_METAL,"metal"},{GF_METAL,"grindcore"},{GF_METAL,"djent"},{GF_METAL,"doom"},{GF_METAL,"sludge"},{GF_METAL,"deathcore"},
    {GF_PUNK,"punk"},{GF_PUNK,"hardcore"},{GF_PUNK,"emo"},{GF_PUNK,"ska"},{GF_PUNK,"oi"},
    {GF_ELECTRO,"electro"},{GF_ELECTRO,"electronic"},{GF_ELECTRO,"techno"},{GF_ELECTRO,"house"},{GF_ELECTRO,"trance"},
    {GF_ELECTRO,"edm"},{GF_ELECTRO,"ebm"},{GF_ELECTRO,"idm"},{GF_ELECTRO,"drum and bass"},{GF_ELECTRO,"dubstep"},
    {GF_ELECTRO,"ambient"},{GF_ELECTRO,"trip hop"},{GF_ELECTRO,"big beat"},{GF_ELECTRO,"breakbeat"},{GF_ELECTRO,"synth"},
    {GF_ELECTRO,"downtempo"},{GF_ELECTRO,"industrial"},{GF_ELECTRO,"chill"},{GF_ELECTRO,"dance"},{GF_ELECTRO,"leftfield"},
    {GF_ELECTRO,"abstract"},{GF_ELECTRO,"psytrance"},{GF_ELECTRO,"musique concrete"},{GF_ELECTRO,"garage house"},{GF_ELECTRO,"jungle"},{GF_ELECTRO,"eurodance"},{GF_ELECTRO,"disco house"},
    {GF_HIPHOP,"hip hop"},{GF_HIPHOP,"rap"},{GF_HIPHOP,"grime"},{GF_HIPHOP,"trap"},{GF_HIPHOP,"turntablism"},
    {GF_ROCK,"rock"},{GF_ROCK,"grunge"},{GF_ROCK,"britpop"},{GF_ROCK,"shoegaze"},{GF_ROCK,"new wave"},{GF_ROCK,"indie"},
    {GF_ROCK,"garage"},{GF_ROCK,"psychedel"},{GF_ROCK,"stoner"},{GF_ROCK,"lo fi"},{GF_ROCK,"avant garde"},{GF_ROCK,"mod revival"},
    {GF_ROCK,"glam"},{GF_ROCK,"surf"},{GF_ROCK,"rockabilly"},{GF_ROCK,"psychobilly"},{GF_ROCK,"post rock"},{GF_ROCK,"math rock"},
    {GF_JAZZ,"jazz"},{GF_JAZZ,"blues"},{GF_JAZZ,"soul"},{GF_JAZZ,"funk"},{GF_JAZZ,"r&b"},{GF_JAZZ,"swing"},{GF_JAZZ,"gospel"},
    {GF_JAZZ,"motown"},{GF_JAZZ,"doo wop"},{GF_JAZZ,"bebop"},{GF_JAZZ,"ragtime"},
    {GF_POP,"pop"},{GF_POP,"chanson"},{GF_POP,"disco"},{GF_POP,"variete"},{GF_POP,"schlager"},{GF_POP,"a cappella"},{GF_POP,"ballad"},
    {GF_POP,"singer songwriter"},{GF_POP,"easy listening"},{GF_POP,"europop"},{GF_POP,"oldies"},
    {GF_FOLK,"folk"},{GF_FOLK,"world"},{GF_FOLK,"celtic"},{GF_FOLK,"country"},{GF_FOLK,"bluegrass"},{GF_FOLK,"reggae"},
    {GF_FOLK,"dub"},{GF_FOLK,"latin"},{GF_FOLK,"bossa"},{GF_FOLK,"flamenco"},{GF_FOLK,"salsa"},{GF_FOLK,"afro"},{GF_FOLK,"americana"},
    {GF_CLASSICAL,"classical"},{GF_CLASSICAL,"baroque"},{GF_CLASSICAL,"opera"},{GF_CLASSICAL,"soundtrack"},
    {GF_CLASSICAL,"orchestr"},{GF_CLASSICAL,"instrumental"},{GF_CLASSICAL,"musical"},{GF_CLASSICAL,"piano"},
    {GF_CLASSICAL,"symphon"},{GF_CLASSICAL,"chamber"},{GF_CLASSICAL,"romantic"},
    {GF_NONE,0} };

/* the family word alone: no subgenre */
static const struct { int fam; const char*w; } GENERIC[]={
    {GF_METAL,"metal"},{GF_ROCK,"rock"},{GF_PUNK,"punk"},{GF_ELECTRO,"electronic"},{GF_HIPHOP,"hip hop"},
    {GF_POP,"pop"},{GF_NONE,0} };

static int word_has(const char*s,const char*kw){
    size_t n=strlen(kw);
    for(const char*p=s; (p=strstr(p,kw)); p++) if(p==s || p[-1]==' ') return 1;
    (void)n; return 0; }

/* "franÃ§aise": UTF-8 that was read as Latin-1 and encoded again (older indexes). If every
 * character is below U+0100 and their bytes form UTF-8, return those bytes. */
static const char* fix_double_utf8(const char*in,char*out,int cap){
    int j=0, multi=0; const unsigned char*p=(const unsigned char*)in;
    while(*p){ unsigned cp;
        if(*p<0x80) cp=*p++;
        else if((*p&0xE0)==0xC0 && (p[1]&0xC0)==0x80){ cp=((p[0]&0x1Fu)<<6)|(p[1]&0x3Fu); p+=2; }
        else return in;
        if(cp>0xFF || j>=cap-1) return in;
        out[j++]=(char)cp; }
    out[j]=0;
    for(int k=0;k<j;){ unsigned char c=(unsigned char)out[k]; int l= c<0x80?1 : (c>>5)==6?2 : (c>>4)==14?3 : 0;
        if(!l||k+l>j) return in; for(int m=1;m<l;m++) if(((unsigned char)out[k+m]&0xC0)!=0x80) return in;
        if(l>1) multi=1; k+=l; }
    return multi ? out : in; }

int genre_classify(const char*value,char*sub,int subcap){
    char k[96], c[96], fx[96]; sub[0]=0;
    value=fix_double_utf8(value,fx,sizeof fx);
    /* "(17)Rock" / "(17)" — old ID3v1 numeric wrapper */
    while(*value==' ') value++;
    if(*value=='('){ const char*e=strchr(value,')'); if(e && e[1]) value=e+1; }
    norm(value,k,sizeof k); if(!k[0]) return GF_NONE;
    for(int i=0;JUNK[i];i++) if(!strcmp(k,JUNK[i])) return GF_NONE;
    { int all=1; for(const char*p=k;*p;p++) if(!isdigit((unsigned char)*p)&&*p!=' ') all=0; if(all) return GF_NONE; }
    /* canonical display name: alias, else the value itself (trimmed, first letter up, rest down) */
    const char*canon=0;
    for(int i=0;ALIAS[i][0];i++) if(!strcmp(k,ALIAS[i][0])){ canon=ALIAS[i][1]; break; }
    if(canon) snprintf(c,sizeof c,"%s",canon);
    else { int j=0, sp=1; for(const unsigned char*p=(const unsigned char*)value; *p && j<(int)sizeof c-1; p++){
            unsigned char ch=*p; if(isspace(ch)){ if(sp) continue; sp=1; c[j++]=' '; continue; } sp=0;
            c[j]=(char)(ch<0x80 ? (j==0?toupper(ch):tolower(ch)) : ch); j++; }
           while(j>0&&c[j-1]==' ') j--;
           c[j]=0; }
    char ck[96]; norm(c,ck,sizeof ck);
    int fam=GF_NONE;
    for(int i=0;RULE[i].kw;i++) if(word_has(ck,RULE[i].kw)){ fam=RULE[i].fam; break; }
    if(fam==GF_NONE) return GF_NONE;
    for(int i=0;GENERIC[i].w;i++) if(GENERIC[i].fam==fam && !strcmp(ck,GENERIC[i].w)) return fam;   /* sub stays "" */
    snprintf(sub,subcap,"%s",c);
    return fam;
}

void genre_split(const char*tag,void(*fn)(void*,const char*),void*u){
    char part[128]; int j=0;
    for(const char*p=tag;;p++){
        int end = !*p || *p==';' || *p=='/' || *p==',' || *p=='|' || *p=='\\';
        /* glued values from old indexes: "Heavy MetalRock" -> "Heavy Metal" + "Rock" */
        int glue = !end && j>0 && islower((unsigned char)part[j-1]) && isupper((unsigned char)*p);
        if(end||glue){ part[j]=0; char*s=part; while(*s==' ') s++;
            int n=(int)strlen(s); while(n>0&&s[n-1]==' ') s[--n]=0;
            if(*s) fn(u,s);
            j=0;
            if(!*p) break;
            if(end) continue; }
        if(j<(int)sizeof part-1) part[j++]=*p;
    }
}
