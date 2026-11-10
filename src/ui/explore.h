#ifndef R3_EXPLORE_H
#define R3_EXPLORE_H
/* Explorer: browse the library by main genre > subgenre, one random artist at a time.
 * Built from the smartmix index: each artist gets ONE main genre + subgenre (the most common
 * across its tracks), corrected by <SD>/Genres.txt:
 *
 *     # manual corrections (win over the file tags)
 *     Rammstein = Metal > Industrial metal          explicit main genre > subgenre
 *     Ghost = heavy metal; doom metal               or tag values, classified as usual
 *     genre: neue deutsche härte = industrial metal a spelling, for every artist
 *     [musicbrainz]
 *     Some Artist = black metal; viking metal       written by scripts/genres_mb.py: only used
 *                                                   when the tags give no (sub)genre
 * No LVGL here (see main.c for the screens). Indexes are valid until the next rebuild:
 * keep artist NAMES across screens and look them up with explore_find(). */
#include "genre.h"

#define EX_UNSORTED GF_N       /* family slot for artists without a usable genre */
#define EX_NFAM (GF_N+1)

int  explore_refresh(void);              /* build / rebuild when the index changed; 1 = ready */
int  explore_tags_pending(void);         /* tags still being re-read in the background */
int  explore_artists(void);              /* artists in the table */

int  explore_fam_artists(int fam);       /* artists in a family (EX_UNSORTED included) */
const char* explore_fam_name(int fam);   /* English, translate with _() */
/* subgenres of a family holding >= 2 artists, most artists first; returns how many */
int  explore_subs(int fam,int*ids,int cap);
const char* explore_sub_name(int id);
int  explore_sub_artists(int id);
int  explore_sub_fam(int id);             /* main genre of a subgenre */

/* pick a random artist: fam -1 = any, sub -1 = any in the family. Prefers artists you play
 * little and skips those seen recently. Returns an artist index or -1. */
int  explore_pick(int fam,int sub);
int  explore_similar(int a);             /* same subgenre, else same family, close in era */
int  explore_find(const char*name);      /* -1 if unknown */
void explore_seen(int a);                /* remember (no repeat for a while) */

const char* explore_artist(int a);
int  explore_artist_fam(int a);
const char* explore_artist_sub(int a);   /* "" when only the family is known */
int  explore_artist_albums(int a);
int  explore_artist_plays(int a);
/* an album of artist a (least played first, never `not_dir` unless it's the only one):
 * dir = full folder path, title = folder name. Returns 1 if found. */
int  explore_album(int a,const char*not_dir,char*dir,int dcap,char*title,int tcap);
/* which artist (index) owns a full folder path, -1 if none */
int  explore_artist_of_dir(const char*dir);
/* every album folder (path under the SD root, e.g. "Music/Artist/Album") and its artist */
int  explore_nalbums(void);
const char* explore_album_dir(int i);
int  explore_album_artist(int i);
#endif
