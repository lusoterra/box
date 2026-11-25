<script lang="ts">
  import { onMount } from 'svelte';
  import BrandMark from './lib/components/BrandMark.svelte';
  import PostGrid from './lib/components/PostGrid.svelte';
  import PostViewer from './lib/components/PostViewer.svelte';
  import SearchBar from './lib/components/SearchBar.svelte';
  import SettingsPanel from './lib/components/SettingsPanel.svelte';
  import {
    fetchPost,
    fetchPostCount,
    fetchPosts,
    getApiBase,
    serverLabel,
    setApiBase,
  } from './lib/api';
  import { addTag, withRating } from './lib/query';
  import {
    homeHash,
    parseLocation,
    postHash,
    writeHash,
    type Route,
  } from './lib/router';
  import {
    DEFAULT_GRID,
    normalizeGrid,
    type GridSettings,
  } from './lib/grid';
  import {
    DEFAULT_PLAYBACK,
    normalizePlayback,
    type PlaybackSettings,
  } from './lib/playback';
  import { DEFAULT_THEME, normalizeTheme, themeFor } from './lib/themes';
  import type { DanbooruPost, Rating } from './lib/types';

  const PAGE_LIMIT = 40;
  const LAST_HOME_KEY = 'box-share-last-home';

  function readStoredHome(): { tags: string; rating: Rating } | null {
    try {
      const raw = localStorage.getItem(LAST_HOME_KEY);
      if (!raw) return null;
      const parsed = parseLocation(raw);
      if (parsed.kind !== 'home') return null;
      return { tags: parsed.tags, rating: parsed.rating };
    } catch {
      return null;
    }
  }

  function readTheme(): string {
    try {
      return normalizeTheme(localStorage.getItem('box-share-theme'));
    } catch {
      return DEFAULT_THEME;
    }
  }

  function readGrid(): GridSettings {
    try {
      const raw = localStorage.getItem('box-share-grid');
      if (raw) return normalizeGrid(JSON.parse(raw) as Partial<GridSettings>);
    } catch {
      // storage or JSON unavailable
    }
    return { ...DEFAULT_GRID };
  }

  function readPlayback(): PlaybackSettings {
    try {
      const raw = localStorage.getItem('box-share-playback');
      if (raw) {
        return normalizePlayback(
          JSON.parse(raw) as Partial<PlaybackSettings>,
        );
      }
    } catch {
      // storage or JSON unavailable
    }
    return { ...DEFAULT_PLAYBACK };
  }

  const first = parseLocation();
  const storedHome = first.kind === 'post' ? readStoredHome() : null;
  const initialTags = first.kind === 'home' ? first.tags : storedHome?.tags ?? '';
  const initialRating =
    first.kind === 'home' ? first.rating : storedHome?.rating ?? 'all';

  let route = $state<Route>(first);
  let query = $state(initialTags);
  let rating = $state<Rating>(initialRating);
  let lastHomeHash = $state(homeHash(initialTags, initialRating));
  let posts = $state<DanbooruPost[]>([]);
  let page = $state(1);
  let hasMore = $state(true);
  let loading = $state(false);
  let loadingMore = $state(false);
  let error = $state('');
  let total = $state<number | null>(null);
  let currentPost = $state<DanbooruPost | null>(null);
  let viewerLoading = $state(false);
  let viewerError = $state('');
  let settingsOpen = $state(false);
  let apiBase = $state(getApiBase());
  let theme = $state(readTheme());
  let grid = $state<GridSettings>(readGrid());
  let playback = $state<PlaybackSettings>(readPlayback());
  let prefersLight = $state(false);

  let searchSeq = 0;
  let postSeq = 0;
  let gridScroll = 0;
  let viewerFromGrid = false;

  let effectiveQuery = $derived(withRating(query, rating));
  let viewerIndex = $derived(
    currentPost ? posts.findIndex((p) => p.id === currentPost?.id) : -1,
  );
  let canPrev = $derived(viewerIndex > 0);
  let canNext = $derived(
    viewerIndex >= 0 && viewerIndex < posts.length - 1,
  );

  $effect(() => {
    const root = document.documentElement;
    if (theme === 'system') root.removeAttribute('data-theme');
    else root.dataset.theme = theme;
    try {
      localStorage.setItem('box-share-theme', theme);
    } catch {
      // storage may be unavailable
    }
  });

  $effect(() => {
    document.title = currentPost
      ? `#${currentPost.id} · Box Share`
      : 'Box Share · Booru Explorer';
  });

  $effect(() => {
    const palette =
      theme === 'system'
        ? themeFor(prefersLight ? 'light' : 'builtin')
        : themeFor(theme);
    const meta = document.querySelector('meta[name="theme-color"]');
    if (meta) meta.setAttribute('content', palette.window);
  });

  $effect(() => {
    try {
      localStorage.setItem('box-share-grid', JSON.stringify(grid));
    } catch {
      // storage unavailable
    }
  });

  $effect(() => {
    try {
      localStorage.setItem(
        'box-share-playback',
        JSON.stringify(playback),
      );
    } catch {
      // storage unavailable
    }
  });

  $effect(() => {
    try {
      localStorage.setItem(LAST_HOME_KEY, lastHomeHash);
    } catch {
      // storage unavailable
    }
  });

  async function runSearch(reset = true): Promise<void> {
    if (!reset && (loading || loadingMore || !hasMore)) return;
    const seq = ++searchSeq;
    const tags = effectiveQuery;
    if (reset) {
      posts = [];
      page = 1;
      hasMore = true;
      total = null;
      error = '';
      loading = true;
      loadingMore = false;
    } else {
      loadingMore = true;
    }
    const nextPage = reset ? 1 : page;
    try {
      const data = await fetchPosts(tags, nextPage, PAGE_LIMIT);
      if (seq !== searchSeq) return;
      posts = reset ? data : [...posts, ...data];
      page = nextPage + 1;
      hasMore = data.length >= PAGE_LIMIT;
      fetchPostCount(tags)
        .then((n) => {
          if (seq === searchSeq) total = n;
        })
        .catch(() => {
          // count endpoint is optional
        });
    } catch (err) {
      if (seq !== searchSeq) return;
      error =
        err instanceof Error ? err.message : 'Could not load the library.';
      hasMore = false;
    } finally {
      if (seq === searchSeq) {
        loading = false;
        loadingMore = false;
      }
    }
  }

  function loadMore(): void {
    if (route.kind !== 'home') return;
    void runSearch(false);
  }

  function submitSearch(text: string): void {
    query = text.trim();
    lastHomeHash = homeHash(query, rating);
    currentPost = null;
    viewerFromGrid = false;
    route = { kind: 'home', tags: query, rating };
    writeHash(lastHomeHash, true);
    void runSearch(true);
  }

  function changeRating(value: Rating): void {
    rating = value;
    lastHomeHash = homeHash(query, rating);
    writeHash(lastHomeHash, true);
    void runSearch(true);
  }

  function openPost(post: DanbooruPost): void {
    gridScroll = window.scrollY;
    lastHomeHash = homeHash(query, rating);
    currentPost = post;
    viewerFromGrid = true;
    viewerLoading = false;
    viewerError = '';
    route = { kind: 'post', id: post.id };
    writeHash(postHash(post.id), true);
  }

  async function loadPost(id: number, force = false): Promise<void> {
    if (!force) {
      const found = posts.find((p) => p.id === id);
      if (found) {
        currentPost = found;
        viewerLoading = false;
        viewerError = '';
        return;
      }
    }
    const seq = ++postSeq;
    viewerLoading = true;
    viewerError = '';
    try {
      const post = await fetchPost(id);
      if (seq !== postSeq) return;
      currentPost = post;
    } catch (err) {
      if (seq !== postSeq) return;
      currentPost = null;
      viewerError =
        err instanceof Error ? err.message : 'Could not load this post.';
    } finally {
      if (seq === postSeq) viewerLoading = false;
    }
  }

  function closeViewer(): void {
    if (viewerFromGrid) {
      viewerFromGrid = false;
      window.history.back();
      return;
    }
    currentPost = null;
    viewerLoading = false;
    viewerError = '';
    route = { kind: 'home', tags: query, rating };
    writeHash(lastHomeHash, false);
    if (posts.length === 0) void runSearch(true);
    requestAnimationFrame(() => window.scrollTo(0, gridScroll));
  }

  function stepPost(delta: number): void {
    if (viewerIndex < 0) return;
    const next = posts[viewerIndex + delta];
    if (!next) return;
    currentPost = next;
    viewerError = '';
    viewerLoading = false;
    route = { kind: 'post', id: next.id };
    writeHash(postHash(next.id), false);
  }

  function searchTag(tag: string, exclude: boolean): void {
    query = addTag(query, tag, exclude);
    lastHomeHash = homeHash(query, rating);
    currentPost = null;
    viewerFromGrid = false;
    route = { kind: 'home', tags: query, rating };
    writeHash(lastHomeHash, true);
    void runSearch(true);
  }

  function refresh(): void {
    if (route.kind === 'post' && currentPost) {
      void loadPost(currentPost.id, true);
    } else {
      void runSearch(true);
    }
  }

  function goBrand(): void {
    if (route.kind === 'post') {
      closeViewer();
      return;
    }
    writeHash(lastHomeHash, true);
  }

  function saveSettings(
    newApi: string,
    newTheme: string,
    newGrid: GridSettings,
    newPlayback: PlaybackSettings,
  ): void {
    const oldApi = getApiBase();
    apiBase = setApiBase(newApi);
    theme = normalizeTheme(newTheme);
    grid = normalizeGrid(newGrid);
    playback = normalizePlayback(newPlayback);
    settingsOpen = false;
    if (oldApi !== getApiBase()) {
      void runSearch(true);
      if (route.kind === 'post' && currentPost) {
        void loadPost(currentPost.id, true);
      }
    }
  }

  function syncFromLocation(): void {
    const next = parseLocation();
    if (next.kind === 'post') {
      route = next;
      const found = posts.some((p) => p.id === next.id);
      viewerFromGrid = found;
      if (currentPost?.id !== next.id) void loadPost(next.id);
      return;
    }

    const changed = next.tags !== query || next.rating !== rating;
    route = next;
    currentPost = null;
    viewerLoading = false;
    viewerError = '';
    viewerFromGrid = false;
    query = next.tags;
    rating = next.rating;
    lastHomeHash = homeHash(next.tags, next.rating);
    if (changed || posts.length === 0) {
      void runSearch(true);
    } else {
      requestAnimationFrame(() => window.scrollTo(0, gridScroll));
    }
  }

  onMount(() => {
    const onLocation = () => syncFromLocation();
    window.addEventListener('popstate', onLocation);
    window.addEventListener('hashchange', onLocation);

    const media = window.matchMedia('(prefers-color-scheme: light)');
    prefersLight = media.matches;
    const onScheme = (event: MediaQueryListEvent) => {
      prefersLight = event.matches;
    };
    media.addEventListener('change', onScheme);

    const onKey = (event: KeyboardEvent) => {
      const target = event.target as HTMLElement | null;
      if (
        target &&
        (target.tagName === 'INPUT' ||
          target.tagName === 'TEXTAREA' ||
          target.tagName === 'SELECT' ||
          target.isContentEditable)
      ) {
        return;
      }
      if (settingsOpen || route.kind === 'post') return;
      if (
        event.key === '/' ||
        (event.ctrlKey && event.key.toLowerCase() === 'l')
      ) {
        event.preventDefault();
        document.getElementById('search-input')?.focus();
        return;
      }
      if (
        event.key === 'Escape' &&
        (query || rating !== 'all')
      ) {
        event.preventDefault();
        rating = 'all';
        submitSearch('');
      }
    };
    window.addEventListener('keydown', onKey);

    if (route.kind === 'post') {
      void loadPost(route.id);
      if (query || rating !== 'all') void runSearch(true);
    } else {
      if (!window.location.hash) writeHash(lastHomeHash, false);
      void runSearch(true);
    }

    return () => {
      window.removeEventListener('popstate', onLocation);
      window.removeEventListener('hashchange', onLocation);
      media.removeEventListener('change', onScheme);
      window.removeEventListener('keydown', onKey);
    };
  });
</script>

<div class="page" class:viewer-open={route.kind === 'post'}>
  <header class="top">
    <a class="brand" href={lastHomeHash} onclick={(e) => { e.preventDefault(); goBrand(); }}>
      <BrandMark size={36} />
      <span class="brand-copy">
        <strong>Box</strong>
        <span>BOoru eXplorer</span>
      </span>
      <span class="share-pill">SHARE</span>
    </a>

    <SearchBar
      {query}
      {rating}
      {total}
      cardWidth={grid.cardWidth}
      loading={loading || loadingMore}
      onsubmit={submitSearch}
      onrating={changeRating}
      onsettings={() => (settingsOpen = true)}
      onrefresh={refresh}
      oncardwidth={(width) => (grid = { ...grid, cardWidth: width })}
    />
  </header>

  <main
    style={`--card-w:${grid.cardWidth}px;--thumb-aspect:${grid.aspectNum} / ${grid.aspectDen}`}
  >
    <PostGrid
      {posts}
      {loading}
      {loadingMore}
      {error}
      {hasMore}
      {total}
      onopen={openPost}
      onloadmore={loadMore}
      onretry={() => runSearch(true)}
    />
  </main>

  <footer class="site-foot">
    <span class="foot-brand">
      <BrandMark size={16} />
      <span>Booru Explorer · read-only LAN library</span>
    </span>
    <span class="server" title="Share API">{serverLabel()}</span>
  </footer>
</div>

{#if settingsOpen}
  <SettingsPanel
    {apiBase}
    {theme}
    {grid}
    {playback}
    onclose={() => (settingsOpen = false)}
    onsave={saveSettings}
  />
{/if}

{#if route.kind === 'post'}
  <PostViewer
    post={currentPost}
    loading={viewerLoading}
    error={viewerError}
    {canPrev}
    {canNext}
    {playback}
    onclose={closeViewer}
    onprev={() => stepPost(-1)}
    onnext={() => stepPost(1)}
    onretry={() => route.kind === 'post' && loadPost(route.id, true)}
    ontag={searchTag}
  />
{/if}

<style>
  .page {
    min-height: 100vh;
    display: flex;
    flex-direction: column;
  }

  .top {
    position: sticky;
    top: 0;
    z-index: 30;
    display: flex;
    align-items: flex-start;
    gap: 16px;
    padding: 10px 16px 11px;
    border-bottom: 1px solid var(--border);
    background: color-mix(in srgb, var(--bg) 88%, transparent);
    backdrop-filter: blur(12px);
  }

  .brand {
    display: flex;
    align-items: center;
    gap: 9px;
    flex: 0 0 auto;
    min-height: 38px;
    padding-top: 2px;
    color: var(--text);
    text-decoration: none;
  }

  .brand:hover {
    text-decoration: none;
  }

  .brand-copy {
    display: flex;
    flex-direction: column;
    line-height: 1.15;
  }

  .brand-copy strong {
    font-size: 16px;
    font-weight: 700;
    letter-spacing: 0.015em;
  }

  .brand-copy span {
    color: var(--muted);
    font-size: 9px;
    letter-spacing: 0.14em;
    text-transform: uppercase;
  }

  .share-pill {
    align-self: center;
    margin-left: 2px;
    padding: 2px 6px;
    border: 1px solid var(--border);
    border-radius: 99px;
    color: var(--muted);
    font-size: 8.5px;
    letter-spacing: 0.12em;
  }

  main {
    flex: 1;
    width: 100%;
    max-width: 1800px;
    margin: 0 auto;
    padding: 14px 16px 0;
  }

  .site-foot {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    width: 100%;
    max-width: 1800px;
    margin: 0 auto;
    padding: 16px 16px 20px;
    color: var(--muted);
    font-size: 11px;
  }

  .foot-brand {
    display: inline-flex;
    align-items: center;
    gap: 7px;
    min-width: 0;
  }

  .server {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 10.5px;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  @media (max-width: 760px) {
    .top {
      flex-wrap: wrap;
      gap: 8px;
      padding: 8px 10px 9px;
    }

    .brand {
      min-height: 32px;
    }

    .brand-copy strong {
      font-size: 15px;
    }

    main {
      padding: 10px 9px 0;
    }

    .site-foot {
      padding: 14px 10px 18px;
      flex-direction: column;
      align-items: flex-start;
      gap: 5px;
    }
  }
</style>
