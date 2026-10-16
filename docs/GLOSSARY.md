# GLOSSARY — spoken term -> code concept

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
- stray = hand-tagged file in `<downloadDir>/_Stray/` as `_Stray_<md5>.<ext>`
  (`src/library/stray.h:14`); via File -> Download/Add Stray (`straydialog`).
- mascot = home-page image (`home/mascot*` settings, `src/settings.h:98`);
  any grid file can be set as mascot; rendered by `WelcomePage`.
- multibooru = one browse tab over several `sites` (`BrowserTab.sites`);
  results merged client-side; `♥ Favourites` + `⭳ Downloads` join as virtual
  sources (`src/ui/mainwindow.cpp:221`).
- sidecar = `<media>.json` next to a file: gallery-dl native/flat object or
  LOSN `<stem>.json`; parsed by `MetaParser`. Download sidecars =
  pretty-printed raw API objects written by `completeJob`.
- sort orders = `id-desc/id-asc/score/rating/added/...` (`BrowserTab.sort`,
  `src/ui/mainwindow.h:57`); `added` has no server spellings (client-side).
- OR-groups = `~(...)` query syntax (`src/library/queryor.h:1-9`); one search
  per alternative on engines without native OR, unioned client-side.
- explicit-rating healing = `inferExplicit()` upgrades under-tagged posts to
  `explicit` + sets `ratingInferred` (`src/library/explicittags.h:122`,
  `src/post.h:19`); baked into scan cache via `explicitTagsHash()`.
- skipAutoClassify = per-entry opt-out of tag categorization
  (`RemotePosts::skipAutoClassify`, `src/remote/remotesource.h:147`).
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
