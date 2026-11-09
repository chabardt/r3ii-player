#ifndef R3_I18N_H
#define R3_I18N_H
/* UI translations. Write user-visible text in English inside _("...") — it is looked up in the
 * current language's table (from lang/<code>.po, compiled by scripts/lang_gen.py) and falls back
 * to the English text when there is no translation. N_("...") only marks a string (for static
 * tables); translate it with _() where it is shown. See docs/TRANSLATING.md. */
const char* i18n_tr(const char*english);
#define _(s)  i18n_tr(s)
#define N_(s) (s)

int  i18n_count(void);                 /* languages, English (index 0) included */
const char* i18n_code(int i);          /* "en", "fr", ... */
const char* i18n_name(int i);          /* native name: "English", "Français", ... */
int  i18n_current(void);
void i18n_set(int i);                  /* takes effect for screens built afterwards; saved */
void i18n_load(void);                  /* restore the saved choice (call at start) */
#endif
