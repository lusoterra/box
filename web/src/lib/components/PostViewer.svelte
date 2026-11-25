<script lang="ts">
  import Icon from './Icon.svelte';
  import { absoluteMediaUrl } from '../api';
  import {
    fileUrl,
    isVideoPost,
    postExt,
    previewUrl,
    ratingOf,
    sourceHost,
    tagDisplay,
    tagGroups,
  } from '../post';
  import {
    altForPost,
    formatBytes,
    formatDuration,
    ratingColor,
    tagBucketColor,
  } from '../query';
  import type { DanbooruPost } from '../types';

  let {
    post = null,
    loading = false,
    error = '',
    canPrev = false,
    canNext = false,
    onclose,
    onprev,
    onnext,
    onretry,
    ontag,
  }: {
    post?: DanbooruPost | null;
    loading?: boolean;
    error?: string;
    canPrev?: boolean;
    canNext?: boolean;
    onclose: () => void;
    onprev: () => void;
    onnext: () => void;
    onretry: () => void;
    ontag: (tag: string, exclude: boolean) => void;
  } = $props();

  let viewerEl = $state<HTMLElement | null>(null);
  let videoEl = $state<HTMLVideoElement | null>(null);
  let zoomed = $state(false);
  let muted = $state(true);
  let infoOpen = $state(true);
  let mediaError = $state(false);
  let copied = $state('');

  let media = $derived(post ? absoluteMediaUrl(fileUrl(post)) : '');
  let poster = $derived(post ? absoluteMediaUrl(previewUrl(post)) : '');
  let video = $derived(post ? isVideoPost(post) : false);
  let ext = $derived(post ? postExt(post) : '');
  let rating = $derived(post ? ratingOf(post) : 'general');
  let groups = $derived(post ? tagGroups(post) : []);
  let duration = $derived(
    post ? formatDuration(post.media_asset?.duration) : '',
  );
  let pageUrl = $derived(
    post
      ? `${window.location.href.split('#')[0]}#/post/${post.id}`
      : window.location.href,
  );
  let source = $derived(
    post?.source && /^https?:\/\//i.test(post.source) ? post.source : '',
  );

  let copyTimer: number | undefined;

  $effect(() => {
    const id = post?.id;
    if (!id) return;
    zoomed = false;
    muted = true;
    mediaError = false;
    copied = '';
    infoOpen = window.innerWidth >= 900;
  });

  $effect(() => {
    if (videoEl) videoEl.muted = muted;
  });

  $effect(() => {
    // Track the callbacks/keyboard flags so the handler stays current.
    void onclose;
    void onprev;
    void onnext;
    void onretry;
    void canPrev;
    void canNext;
    void video;
    void zoomed;
    void muted;
    void infoOpen;
    const previousOverflow = document.body.style.overflow;
    document.body.style.overflow = 'hidden';

    const onkeydown = (event: KeyboardEvent) => {
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
      if (event.key === 'Escape') {
        event.preventDefault();
        onclose();
      } else if (event.key === 'ArrowLeft' && canPrev) {
        event.preventDefault();
        onprev();
      } else if (event.key === 'ArrowRight' && canNext) {
        event.preventDefault();
        onnext();
      } else if (event.key === 'f' || event.key === 'F') {
        event.preventDefault();
        void toggleFullscreen();
      } else if (event.key === 'i' || event.key === 'I') {
        event.preventDefault();
        infoOpen = !infoOpen;
      } else if ((event.key === 'm' || event.key === 'M') && video) {
        event.preventDefault();
        muted = !muted;
      } else if (
        (event.key === '+' || event.key === '=') &&
        !video
      ) {
        event.preventDefault();
        zoomed = true;
      } else if (event.key === '-' && !video) {
        event.preventDefault();
        zoomed = false;
      }
    };
    window.addEventListener('keydown', onkeydown);
    return () => {
      document.body.style.overflow = previousOverflow;
      window.removeEventListener('keydown', onkeydown);
    };
  });

  async function toggleFullscreen(): Promise<void> {
    try {
      if (document.fullscreenElement) {
        await document.exitFullscreen();
      } else if (viewerEl) {
        await viewerEl.requestFullscreen();
      }
    } catch {
      // browser refused fullscreen; no UI change needed
    }
  }

  function copy(text: string, label: string): void {
    const done = () => {
      copied = label;
      if (copyTimer) window.clearTimeout(copyTimer);
      copyTimer = window.setTimeout(() => (copied = ''), 1600);
    };
    const fallback = () => {
      const area = document.createElement('textarea');
      area.value = text;
      area.style.position = 'fixed';
      area.style.opacity = '0';
      document.body.appendChild(area);
      area.select();
      try {
        document.execCommand('copy');
      } catch {
        // best effort
      }
      area.remove();
      done();
    };
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(done).catch(fallback);
    } else {
      fallback();
    }
  }

  function close(): void {
    onclose();
  }
</script>

<div class="viewer" bind:this={viewerEl}>
  <header class="bar">
    <button
      type="button"
      class="icon-btn back"
      onclick={close}
      title="Back to grid"
      aria-label="Back to grid"
    >
      <Icon name="arrow-left" size={19} />
    </button>

    {#if post}
      <span class="post-id">#{post.id}</span>
      <span
        class="rating"
        style={`--rating:${ratingColor(rating)}`}
        title={rating}
      >
        {rating}
      </span>
      {#if ext}<span class="ext">{ext.toUpperCase()}</span>{/if}
    {/if}

    <span class="spacer"></span>

    <div class="pager">
      <button
        type="button"
        class="icon-btn"
        onclick={onprev}
        disabled={!canPrev}
        title="Previous post (Left arrow)"
        aria-label="Previous post"
      >
        <Icon name="chevron-left" size={20} />
      </button>
      <button
        type="button"
        class="icon-btn"
        onclick={onnext}
        disabled={!canNext}
        title="Next post (Right arrow)"
        aria-label="Next post"
      >
        <Icon name="chevron-right" size={20} />
      </button>
    </div>

    {#if post}
      <div class="actions">
        {#if video}
          <button
            type="button"
            class="icon-btn"
            onclick={() => (muted = !muted)}
            title={muted ? 'Unmute video (M)' : 'Mute video (M)'}
            aria-label={muted ? 'Unmute video' : 'Mute video'}
          >
            <Icon name={muted ? 'volume-off' : 'volume'} size={17} />
          </button>
        {:else}
          <button
            type="button"
            class="icon-btn"
            onclick={() => (zoomed = !zoomed)}
            title={zoomed ? 'Fit image (-)' : 'Actual size (+)'}
            aria-label={zoomed ? 'Fit image' : 'Actual size'}
          >
            <Icon name={zoomed ? 'zoom-out' : 'zoom-in'} size={17} />
          </button>
        {/if}
        <button
          type="button"
          class="icon-btn"
          class:active={infoOpen}
          onclick={() => (infoOpen = !infoOpen)}
          title="Toggle tags and metadata (I)"
          aria-label="Toggle tags and metadata"
        >
          <Icon name="info" size={17} />
        </button>
        <button
          type="button"
          class="icon-btn"
          onclick={toggleFullscreen}
          title="Fullscreen (F)"
          aria-label="Fullscreen"
        >
          <Icon name="maximize" size={17} />
        </button>
        {#if media}
          <a
            class="icon-btn"
            href={media}
            target="_blank"
            rel="noopener noreferrer"
            title="Open original"
            aria-label="Open original"
          >
            <Icon name="external" size={17} />
          </a>
          <a
            class="icon-btn"
            href={media}
            download
            title="Download"
            aria-label="Download"
          >
            <Icon name="download" size={17} />
          </a>
        {/if}
      </div>
    {/if}
  </header>

  <div class="body" class:with-info={post && infoOpen}>
    <div class="stage" class:zoomed>
      {#if loading && !post}
        <div class="center">
          <span class="spinner big"></span>
          <p>Loading post…</p>
        </div>
      {:else if error}
        <div class="center error">
          <Icon name="alert" size={30} />
          <p>{error}</p>
          <button type="button" onclick={onretry}>Try again</button>
        </div>
      {:else if !post}
        <div class="center error">
          <Icon name="alert" size={30} />
          <p>Post not found.</p>
          <button type="button" onclick={close}>Back to grid</button>
        </div>
      {:else if mediaError}
        <div class="center error">
          <Icon name="alert" size={30} />
          <p>Could not load this media.</p>
          {#if media}
            <a href={media} target="_blank" rel="noopener noreferrer">
              Open original
            </a>
          {/if}
        </div>
      {:else if video}
        {#key post.id}
          <video
            bind:this={videoEl}
            src={media}
            poster={poster || undefined}
            controls
            autoplay
            loop
            playsinline
            preload="metadata"
            onerror={() => (mediaError = true)}
            title={`Post ${post.id}`}
          >
            <track
              kind="captions"
              src="data:text/vtt;charset=utf-8,WEBVTT%0A%0A"
              srclang="en"
              label="No captions"
            />
          </video>
        {/key}
      {:else}
        {#key post.id}
          <img
            src={media}
            alt={altForPost(post)}
            draggable="false"
            onerror={() => (mediaError = true)}
          />
        {/key}
      {/if}
    </div>

    {#if post && infoOpen}
      <aside class="info">
        <section class="tags-block">
          <h2>Tags</h2>
          {#if groups.length === 0}
            <p class="empty">No tags recorded.</p>
          {:else}
            {#each groups as group (group.bucket)}
              <div class="tag-group">
                <h3 style={`color:${tagBucketColor(group.bucket)}`}>
                  {group.title}
                  <span>{group.tags.length}</span>
                </h3>
                <div class="tags">
                  {#each group.tags as tag (tag)}
                    <span class="tag" style={`--tag:${tagBucketColor(group.bucket)}`}>
                      <button
                        type="button"
                        class="tag-main"
                        onclick={() => ontag(tag, false)}
                        title={`Include ${tag}`}
                      >
                        {tagDisplay(tag)}
                      </button>
                      <button
                        type="button"
                        class="tag-minus"
                        onclick={() => ontag(tag, true)}
                        title={`Exclude ${tag}`}
                        aria-label={`Exclude ${tag}`}
                      >
                        -
                      </button>
                    </span>
                  {/each}
                </div>
              </div>
            {/each}
          {/if}
        </section>

        <section class="meta-block">
          <h2>Details</h2>
          <dl>
            <div>
              <dt>Type</dt>
              <dd>
                {ext ? ext.toUpperCase() : 'unknown'}
                {#if duration}· {duration}{/if}
              </dd>
            </div>
            {#if post.image_width && post.image_height}
              <div>
                <dt>Size</dt>
                <dd>{post.image_width} × {post.image_height}</dd>
              </div>
            {/if}
            {#if post.file_size}
              <div>
                <dt>File</dt>
                <dd>{formatBytes(post.file_size)}</dd>
              </div>
            {/if}
            <div>
              <dt>Rating</dt>
              <dd style={`color:${ratingColor(rating)}`}>{rating}</dd>
            </div>
            {#if post.score != null}
              <div>
                <dt>Score</dt>
                <dd>{post.score}</dd>
              </div>
            {/if}
            {#if post.created_at}
              <div>
                <dt>Added</dt>
                <dd>{new Date(post.created_at).toLocaleString()}</dd>
              </div>
            {/if}
            {#if source}
              <div>
                <dt>Source</dt>
                <dd class="source">
                  <a
                    href={source}
                    target="_blank"
                    rel="noopener noreferrer"
                    title={source}
                  >
                    {sourceHost(source)}
                  </a>
                </dd>
              </div>
            {/if}
            {#if post.md5}
              <div>
                <dt>MD5</dt>
                <dd class="hash" title={post.md5}>{post.md5.slice(0, 12)}…</dd>
              </div>
            {/if}
          </dl>
        </section>

        <section class="share-block">
          <button type="button" class="wide" onclick={() => copy(pageUrl, 'page')}>
            <Icon name="copy" size={15} />
            {copied === 'page' ? 'Copied page link' : 'Copy page link'}
          </button>
          <button type="button" class="wide" onclick={() => copy(media, 'media')}>
            <Icon name="link" size={15} />
            {copied === 'media' ? 'Copied media URL' : 'Copy media URL'}
          </button>
          {#if media}
            <a
              class="wide"
              href={media}
              target="_blank"
              rel="noopener noreferrer"
            >
              <Icon name="external" size={15} />
              Open original
            </a>
            <a class="wide" href={media} download>
              <Icon name="download" size={15} />
              Download
            </a>
          {/if}
        </section>

        <p class="footnote">
          Read-only LAN share · Box / Booru Explorer
        </p>
      </aside>
    {/if}
  </div>
</div>

<style>
  .viewer {
    position: fixed;
    inset: 0;
    z-index: 100;
    display: flex;
    flex-direction: column;
    background: var(--bg);
    color: var(--text);
  }

  .bar {
    flex: 0 0 auto;
    height: 54px;
    display: flex;
    align-items: center;
    gap: 7px;
    padding: 0 10px;
    border-bottom: 1px solid var(--border);
    background: color-mix(in srgb, var(--panel) 92%, transparent);
    backdrop-filter: blur(8px);
  }

  .spacer {
    flex: 1;
  }

  .post-id {
    color: var(--text-dim);
    font-size: 12px;
    font-variant-numeric: tabular-nums;
  }

  .rating {
    padding: 2px 7px;
    border: 1px solid color-mix(in srgb, var(--rating) 55%, var(--border));
    border-radius: 99px;
    color: var(--rating);
    background: color-mix(in srgb, var(--rating) 10%, transparent);
    font-size: 10px;
    letter-spacing: 0.03em;
  }

  .ext {
    color: var(--muted);
    font-size: 10px;
    letter-spacing: 0.04em;
  }

  .pager,
  .actions {
    display: flex;
    align-items: center;
    gap: 2px;
  }

  .actions {
    padding-left: 7px;
    margin-left: 3px;
    border-left: 1px solid var(--border);
  }

  .icon-btn {
    width: 32px;
    height: 32px;
    display: grid;
    place-items: center;
    border: 0;
    border-radius: 7px;
    background: transparent;
    color: var(--text-dim);
    text-decoration: none;
  }

  .icon-btn:hover {
    color: var(--text);
    background: var(--panel-2);
    text-decoration: none;
  }

  .icon-btn:disabled {
    opacity: 0.32;
    cursor: default;
    background: transparent;
  }

  .icon-btn.active {
    color: var(--accent-bright);
    background: var(--accent-soft);
  }

  .back {
    color: var(--text);
  }

  .body {
    flex: 1;
    min-height: 0;
    display: flex;
  }

  .stage {
    position: relative;
    flex: 1;
    min-width: 0;
    min-height: 0;
    display: flex;
    align-items: center;
    justify-content: center;
    overflow: hidden;
    padding: 10px;
    background:
      radial-gradient(circle at 50% 40%, var(--panel-2), transparent 70%),
      var(--bg);
  }

  .stage img {
    max-width: 100%;
    max-height: 100%;
    width: auto;
    height: auto;
    object-fit: contain;
    border-radius: 4px;
    box-shadow: 0 10px 40px rgba(0, 0, 0, 0.35);
    cursor: zoom-in;
  }

  .stage video {
    max-width: 100%;
    max-height: 100%;
    border-radius: 4px;
    background: #000;
    box-shadow: 0 10px 40px rgba(0, 0, 0, 0.4);
  }

  .stage.zoomed {
    overflow: auto;
    align-items: flex-start;
    justify-content: flex-start;
  }

  .stage.zoomed img {
    max-width: none;
    max-height: none;
    margin: auto;
    cursor: zoom-out;
  }

  .center {
    margin: auto;
    display: flex;
    flex-direction: column;
    align-items: center;
    gap: 9px;
    padding: 20px;
    color: var(--muted);
    text-align: center;
  }

  .center.error {
    color: var(--danger);
  }

  .center p {
    margin: 0;
    max-width: 520px;
  }

  .center button {
    margin-top: 5px;
    padding: 7px 14px;
    border: 1px solid var(--border-strong);
    border-radius: 7px;
    background: var(--panel);
    color: var(--text);
  }

  .info {
    width: 370px;
    flex: 0 0 auto;
    overflow: auto;
    border-left: 1px solid var(--border);
    background: var(--bg-elev);
    padding: 14px;
    display: flex;
    flex-direction: column;
    gap: 18px;
  }

  .info h2 {
    margin: 0 0 8px;
    font-size: 11px;
    font-weight: 650;
    letter-spacing: 0.08em;
    text-transform: uppercase;
    color: var(--muted);
  }

  .tag-group + .tag-group {
    margin-top: 11px;
  }

  .tag-group h3 {
    margin: 0 0 5px;
    font-size: 10.5px;
    font-weight: 650;
    letter-spacing: 0.03em;
  }

  .tag-group h3 span {
    margin-left: 4px;
    color: var(--muted);
    font-weight: 400;
  }

  .tags {
    display: flex;
    flex-wrap: wrap;
    gap: 4px;
  }

  .tag {
    display: inline-flex;
    align-items: stretch;
    max-width: 100%;
    border: 1px solid color-mix(in srgb, var(--tag) 35%, var(--border));
    border-radius: 5px;
    background: color-mix(in srgb, var(--tag) 8%, var(--panel));
    overflow: hidden;
  }

  .tag-main,
  .tag-minus {
    border: 0;
    background: transparent;
    color: var(--tag);
    font-size: 11px;
    line-height: 1.2;
    padding: 4px 5px;
  }

  .tag-main {
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .tag-main:hover {
    background: color-mix(in srgb, var(--tag) 14%, transparent);
  }

  .tag-minus {
    border-left: 1px solid color-mix(in srgb, var(--tag) 25%, var(--border));
    color: var(--muted);
    padding: 4px 6px;
    font-weight: 700;
  }

  .tag-minus:hover {
    color: var(--text);
    background: color-mix(in srgb, var(--danger) 18%, transparent);
  }

  .empty {
    margin: 0;
    color: var(--muted);
    font-size: 12px;
  }

  dl {
    margin: 0;
    display: flex;
    flex-direction: column;
    gap: 5px;
  }

  dl div {
    display: flex;
    align-items: baseline;
    justify-content: space-between;
    gap: 12px;
    padding-bottom: 5px;
    border-bottom: 1px solid color-mix(in srgb, var(--border) 60%, transparent);
    font-size: 12px;
  }

  dt {
    color: var(--muted);
    flex: 0 0 auto;
  }

  dd {
    margin: 0;
    color: var(--text-dim);
    text-align: right;
    min-width: 0;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  dd.source a {
    color: var(--accent-bright);
  }

  dd.hash {
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 10.5px;
  }

  .share-block {
    display: grid;
    grid-template-columns: 1fr 1fr;
    gap: 6px;
  }

  .wide {
    display: inline-flex;
    align-items: center;
    justify-content: center;
    gap: 6px;
    min-height: 32px;
    padding: 0 9px;
    border: 1px solid var(--border);
    border-radius: 7px;
    background: var(--panel-2);
    color: var(--text-dim);
    font-size: 11.5px;
    text-align: center;
    text-decoration: none;
  }

  .wide:hover {
    color: var(--text);
    border-color: var(--border-strong);
    text-decoration: none;
  }

  .footnote {
    margin: auto 0 0;
    padding-top: 10px;
    color: var(--muted);
    font-size: 10.5px;
    text-align: center;
  }

  @media (max-width: 900px) {
    .body {
      flex-direction: column;
    }

    .stage {
      flex: 1 1 55%;
      padding: 5px;
    }

    .info {
      width: auto;
      flex: 0 1 45%;
      min-height: 0;
      border-left: 0;
      border-top: 1px solid var(--border);
      padding: 11px;
      gap: 14px;
    }

    .share-block {
      grid-template-columns: 1fr;
    }

    .footnote {
      margin-top: 4px;
    }

    .bar {
      height: 50px;
      padding: 0 6px;
    }

    .ext {
      display: none;
    }
  }

  @media (max-width: 560px) {
    .post-id {
      display: none;
    }
  }
</style>
