# Translating r3ii-player

The UI is written in English and translated with ordinary gettext `.po` files in
[`lang/`](../lang). You need a text editor (or Poedit / any `.po` editor) and
Python 3 — no gettext tools.

Shipped languages: English (built in), French (`lang/fr.po`).
Pick the language on the player in **Settings > Language**.

## Add a language

1. Copy the template and name it after the language code:

   ```sh
   cp lang/messages.pot lang/de.po
   ```

2. Fill in the header — the code and the language's own name, shown in the menu:

   ```
   "Language: de\n"
   "X-Language-Name: Deutsch\n"
   ```

3. Translate each `msgstr`. Leave one empty and the English text is used.

4. Check, then build:

   ```sh
   python3 scripts/lang.py check     # what is missing, and any %-format mistakes
   sh scripts/build.sh               # compiles lang/*.po into the player
   ```

5. Open a pull request with your `.po` file.

## Rules

- **Keep every `%d`, `%s`, `%u` and `%%`**, in the same order, with the same types.
  `check` fails on a mismatch and the build leaves such a string in English, because
  a wrong format could crash the player. If your language needs another order, number
  them: `%2$s %1$d`.
- Keep `\n` line breaks where they are, roughly; screens are 480 px wide. Short
  labels (buttons, section titles) stay short.
- `#8A6A4A ...#` is a colour tag: translate the text between the marks, keep the marks.
- Names stay as they are: Blockfall, Brickstorm, Coil, Sapper, Moko and its forms,
  Bluetooth, Wi-Fi, VLC, Joy-Con, Game Boy, Doom.
- Upper-case strings (`GAME OVER`, `SETTINGS`-style section titles) are shown in
  upper case on purpose.
- The date line is `weekday day month year`: translate `%s %d %s %d` to reorder it
  (e.g. `%1$s, %2$d. %3$s %4$d` in German).

## Fonts

The UI fonts draw Latin (with accents), Latin Extended, Cyrillic and Vietnamese —
the coverage of Montserrat. Greek, Arabic, Hebrew, CJK and other scripts need an
extra fallback font in `src/ui/fonts.c` (tiny_ttf can load any TTF): open an issue
if you want to work on one.

## For developers

- Wrap user-visible text in `_("...")` (from `i18n.h`). In static tables use
  `N_("...")` and call `_()` where the string is shown.
- Don't build sentences from pieces (`"%s to..."` + `"Move"`): give each sentence its
  own string. Plurals: two strings (`"%d song"` / `"%d songs"`).
- Never pass user data (track names, device names) through `_()`.
- After changing strings: `python3 scripts/lang.py update` refreshes
  `lang/messages.pot` and adds the new strings to every `.po`.
