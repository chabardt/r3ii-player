#ifndef R3_GENRE_H
#define R3_GENRE_H
/* Genre clean-up: one raw tag value ("Heavy Metal", "punkrock", "métal", "B.O.") becomes a
 * main genre (family) + a canonical subgenre name. Pure C, no LVGL (host-tested). */

enum { GF_NONE=-1, GF_METAL, GF_ROCK, GF_PUNK, GF_ELECTRO, GF_HIPHOP, GF_JAZZ, GF_POP, GF_FOLK,
       GF_CLASSICAL, GF_N };

/* English family name (translate with _() where shown) */
const char* genre_family_name(int fam);

/* Classify one value. Returns the family (GF_NONE for junk such as "Other" / "Inconnu" or an
 * unknown word). sub receives the canonical subgenre ("Power metal", "Punk rock"), or "" when
 * the value is just the family itself ("Rock", "Metal", "Electronic"). */
int genre_classify(const char*value, char*sub, int subcap);

/* Split a tag string holding several values ("Heavy Metal;Rock", "Hard Rock / Blues", or the
 * glued "Heavy MetalRock" from old indexes) and call fn for each value. */
void genre_split(const char*tag, void (*fn)(void*u,const char*value), void*u);
#endif
