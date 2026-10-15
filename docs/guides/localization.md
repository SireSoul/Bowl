# Localization

Player-facing text lives in string tables, one JSON file per language, looked up by key. The engine picks the
language, falls back to a default language for missing strings, fills in placeholders and handles simple plurals.

## String tables

Put one file per language in `assets/lang/`, named by language code:

```
assets/lang/en.json
assets/lang/es.json
assets/lang/de.json
```

Nested objects become dotted keys:

```json
{
  // comments and trailing commas are allowed
  "menu": {
    "start": "Start",
    "quit": "Quit",
  },
  "greeting": "Hello, {name}!",
  "coins": { "one": "{n} coin", "other": "{n} coins" },
}
```

## Looking strings up

```cpp
Localization& i18n = engine.i18n();

std::string_view start = i18n.tr("menu.start");                       // "Start"
std::string hello = i18n.format("greeting", {{"name", player_name}}); // "Hello, Ada!"
std::string coins = i18n.plural("coins", 3);                         // "3 coins"
bool exists = i18n.has("menu.options");
```

Lookups try the current language, then the fallback language, then return the key itself. A missing string shows up
on screen as `menu.options`, which makes gaps easy to spot.

`format` replaces `{name}` placeholders; placeholders without a value are left as they are. `plural` picks
`key.one` for a count of 1 and `key.other` otherwise, and fills `{n}` with the count. That one/other rule fits
English, Spanish, German and many other languages. For languages with more forms, add extra keys and choose between
them in code. `fill_placeholders(text, args)` is available on its own too.

## Choosing the language

| | |
| --- | --- |
| `AppConfig::language` | The language at startup (`"en"` by default) |
| `AppConfig::fallback_language` | Where missing strings come from (`"en"`) |
| `--lang es` | Override from the command line, for testing translations |
| `i18n.set_language("es")` | Switch while the game runs, from an options menu |

Localization only switches on if the fallback language's file exists (`lang/en.json` by default). Store the player's
choice in settings and apply it in `init`:

```cpp
engine.i18n().set_language(engine.settings().get("language", std::string("en")));
```

## Hot reload

String tables are assets, so editing a `.json` file updates the text in the running game. Translators can work with
the game open.

## Fonts and scripts

Text is UTF-8 throughout. Make sure the font you draw with covers the scripts you translate into; characters missing
from the font are drawn as `?`. Font fallback chains (for CJK and emoji) are on the [roadmap](../roadmap.md).
