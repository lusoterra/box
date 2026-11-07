# Pending tasks (from user goal)

- Remake install script: skip compiling if ./build/box is new, just copy it over.
- Remove `furry:g` from derpibooru/twibooru; leave safe by default.
- Verify derpi and twibooru use same API; merge engines if so.
- Clean redundant `artist:`/`oc:` tags in derpi/twibooru (leave oc).
- Gelbooru comments: report actual reason for load failure; distinguish "no comments".
- "Open externally" button: stream video to default player if downloaded/cached; add settings for default image viewer/video player + CLI options for single post.
- Option to move config folder.
- Save booru settings as JSON (portable); keep Qt/window settings in INI.
- Move search history from config to DB (whole queries); hide history button when disabled.
- Omnibar 100% width.
- Fix settings menu, especially Browse tab (xvfb).
- Default config path ~/.config/box.
- Convert provided settings.ini to new format under ./config (boorus.json one file); don't commit settings.ini.
- Mass edit tags on stray posts: one window, paste tags, steal tags from another post via URL, ordered by category.
- LOSN import: summary diff of new files.
- Refresh tags/downloads: download post scores where supported; sort by score.
- Soybooru auto captcha solving (Cloudflare -> API pow).
- Gelbooru "video" tag loads only a few videos (3-1) despite site having many; investigate pagination/search.
