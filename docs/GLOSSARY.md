# GLOSSARY - spoken term -> code concept

- booru tab = browse tab: `BrowserTab.isRemote` (`src/ui/mainwindow.h:47`);
  remote search tab over one or more configured sites.
- sort box = `m_sortBox` sort combo (`src/ui/mainwindow.h:274`); native tag
  via `sortSearchTag()`, rest client-side `sortPosts()`
  (`src/ui/mainwindow.cpp:2205`).
- rating box = `m_ratingBox` rating filter (`src/ui/mainwindow.h:272`);
  values `all/general/sensitive/questionable/explicit`.
- pills = filter pills in `m_pillBar`/`FlowLayout` (`src/ui/mainwindow.h:281`,
  `src/ui/flowlayout.h`); removable active-filter chips. cards = grid cells
  painted by `GalleryDelegate` (`src/ui/mainwindow.h:67`).
- viewer = `ViewerPage`, same-window post view (`src/ui/viewerpage.h:170`);
  stills `ImageView`, GIFs `GifView`, mp4/webm `VideoView`/`VideoPane`.
  Double-click / right-click / F11 toggles fullscreen (`MainWindow::
  setViewerFullscreen` + `ViewerPage::setImmersive`); Esc leaves fullscreen
  first, arrows keep stepping.
- annotation note popup = left-click a note marker opens `Annotation note`
  (`ViewerPage::showNotePopup`, `src/ui/viewerpage.cpp`), a larger
  selectable-text dialog replacing the hover tooltip.
- artist commentary = Danbooru `artist_commentary` thread
  (`PostMeta::commentaryUrl/parseCommentary`, `src/remote/postmeta.cpp`),
  translation preferred, rendered as ARTIST COMMENTARY beside NOTES/COMMENTS.
- clickable media errors = red video label, viewer error note and failed
  Downloads rows all open the redacted-URL probe dialog
  (`VideoPane::errorDetailsRequested`, `ViewerPage::showErrorNote`,
  `DownloadsDialog`).
- grid snap/stagger = `grid/snap` fills the viewport with whole columns
  (`MainWindow::applyGridSnap`); `grid/staggered` offsets odd columns half
  a card (`GalleryDelegate::m_staggered`). Videos never drag out.
- stray = hand-tagged file in `<downloadDir>/_Stray/` as `_Stray_<md5>.<ext>`
  (`src/library/stray.h:14`); via File -> Download/Add Stray
  (`straydialog`). Pasted booru post links are resolved through the public
  API/HTML (`src/remote/strayresolve.h`) so the sidecar keeps the real tags,
  and `Favourite only` stores the link without downloading the media.
- mascot = home-page image (`home/mascot*`, `src/settings.h`); shipped
  default is `マニフェスト.jpg` (empty path), Remove hides it, Reset
  restores the default; any grid file can replace it, rendered by
  `WelcomePage`.
- multibooru = one browse tab over several `sites` (`BrowserTab.sites`);
  results merged client-side; `♥ Favourites` + `⭳ Downloads` join as virtual
  sources (`src/ui/mainwindow.cpp:221`).
- sidecar = `<media>.json` next to a file: gallery-dl native/flat object or
  LOSN `<stem>.json`; parsed by `MetaParser`. Download sidecars =
  pretty-printed raw API objects written by `completeJob`.
- drag mode = `DragLinkMode` (`src/engines/boorutypes.h`): direct media
  URL vs post page for post drags; global `dragLinkMode`, per-booru
  `BooruSite.dragLinkMode` (Inherit follows global).
- search history = removed (whole-query replay + clock button). Recent
  search tokens still feed suggestions (`historyTokens`,
  `src/settings.h:50`); the omnibar keeps only the ★ favourite-tags button.
- sort orders = `id-desc/id-asc/score/rating/added/...` (`BrowserTab.sort`,
  `src/ui/mainwindow.h:57`); `added` has no server spellings (client-side).
- OR-groups = `~(...)` query syntax (`src/library/queryor.h:1-9`); one search
  per alternative on engines without native OR, unioned client-side.
- explicit-rating healing = `inferExplicit()` upgrades under-tagged posts to
  `explicit` + sets `ratingInferred` (`src/library/explicittags.h:122`,
  `src/post.h:19`); baked into scan cache via `explicitTagsHash()`.
- skipAutoClassify = per-entry opt-out of tag categorization
  (`RemotePosts::skipAutoClassify`, `src/remote/remotesource.h:147`).
- custom tag categories = `customTagCategories` + `tagCustomCategories`
  (Preferences → Tags): named groups with manual `tag -> group`
  assignments; assigned tags render in their own sidebar/viewer group.
- disabled booru = `BooruSite::enabled=false`: config/credentials and
  old tabs/favourites keep resolving, but every picker/menu hides it.
- variants = viewer `Variants` button: builds a parent/child/id OR group
  and returns to the grid, so only the post family is searched.
- merge tag categories = `TagCategoryMerge` rows (`src/library/tagcategories.h`,
  Preferences → Tags): `source1, source2 → Target`, scoped to one booru
  or all; a classic target joins/renames that bucket, anything else is
  one custom group. `*` matches every category.
- themes = `Theme::availableThemes/colorsFor` (`src/ui/theme.cpp`): INI
  files in `themes/` (embedded) or `~/.config/box/themes/` (drop-in),
  plus native. See `docs/UI-TOUR.md` for the key list.
- media cache / cache nothing = on-disk thumbnails + full-quality browsing
  files controlled by `MediaCache` (`src/library/cachepolicy.h`): global
  `cache/mode` (`all/sfw/none`) narrowed per `BooruSite::cacheMode`.
  Explicit downloads are never affected; in SFW mode, downloaded NSFW
  media is cacheable (`Post::downloaded`) while a favourites-only row is
  not; favourites DB/scan index stay.
- delete cached media = `CacheJanitor::purgeMedia()`: SFW mode removes
  NSFW + unknown/legacy entries and keeps provably SFW files; Nothing mode
  removes all `thumbs/` + `full/` media. SFW-only caching treats an
  empty/unknown rating as NSFW, requires a safe/general rating or
  `rating:g`/`safety:safe`-style tag, and refuses blacklisted posts
  (blacklist beats the downloaded exception).
- localBlacklist = per-entry flag: skip server-side `-blacklist`
  exclusions, filter locally (`BooruSite::localBlacklist`).
- relink = `relinkFavourites()`: match unlinked fav rows to files on disk
  (`src/ui/mainwindow.cpp:2642`).
- refresh-tags / update sidecars = `PostRefresher` single-post re-fetch
  (`src/remote/remotesource.h:193`); bulk = `SidecarBatch`
  (`src/library/sidecarrefresh.h:29`).
- tag buckets = artist/character/copyright/general/meta (+ Szurubooru native
  `customTags`, `src/post.h:29`); waterfall = streaming `resolved()`
  verdicts filling the sidebar (`src/ui/mainwindow.h:130`).
- safe-search defaults = fresh installs ship Safebooru-only `browseSites`
  (`src/settings.h:64`); booru manager = prefs registry editor
  (`boorus/order` + `boorus/<key>`, `src/settings.cpp:265`).
- paranoia mode = retired (ini key removed on save, `src/settings.cpp:246`).
