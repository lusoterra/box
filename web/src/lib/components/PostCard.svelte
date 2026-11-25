<script lang="ts">
  import Icon from './Icon.svelte';
  import { absoluteMediaUrl } from '../api';
  import {
    fileUrl,
    fileNameOf,
    isVideoPost,
    postExt,
    previewUrl,
    ratingOf,
  } from '../post';
  import {
    altForPost,
    formatDuration,
    formatScore,
    ratingColor,
    ratingLetter,
  } from '../query';
  import type { DanbooruPost } from '../types';

  let {
    post,
    onopen,
  }: { post: DanbooruPost; onopen: (post: DanbooruPost) => void } = $props();

  let failed = $state(false);
  let useFile = $state(false);
  let loaded = $state(false);

  let src = $derived(absoluteMediaUrl(useFile ? fileUrl(post) : previewUrl(post)));
  let video = $derived(isVideoPost(post));
  let ext = $derived(postExt(post));
  let rating = $derived(ratingOf(post));
  let duration = $derived(formatDuration(post.media_asset?.duration));
  let fileName = $derived(fileNameOf(fileUrl(post)) || ext.toUpperCase());

  function open(event: MouseEvent): void {
    if (event.button !== 0 || event.metaKey || event.ctrlKey || event.shiftKey)
      return;
    event.preventDefault();
    onopen(post);
  }
</script>

<a
  class="card"
  href={`#/post/${post.id}`}
  onclick={open}
  title={altForPost(post)}
>
  <span class="thumb">
    {#if failed || !src}
      <span class="placeholder">
        <Icon name="image" size={28} />
      </span>
    {:else}
      <img
        {src}
        alt={altForPost(post)}
        loading="lazy"
        decoding="async"
        draggable="false"
        class:loaded
        onload={() => (loaded = true)}
        onerror={() => {
          if (!useFile && !video) useFile = true;
          else failed = true;
        }}
      />
    {/if}

    {#if video}
      <span class="play"><Icon name="play" size={22} /></span>
    {/if}

    <span class="badges">
      {#if ext}<span class="ext">{ext.toUpperCase()}</span>{/if}
      {#if duration}<span class="duration">{duration}</span>{/if}
    </span>
  </span>

  <span class="info">
    <span
      class="rating-letter"
      style={`--rating:${ratingColor(rating)}`}
      title={rating}
    >{ratingLetter(rating)}</span>
    <span class="score" class:negative={Number(post.score) < 0}>
      {formatScore(post.score)}
    </span>
    <span class="post-id">#{post.id}</span>
  </span>

  <span class="filename" title={fileName}>{fileName}</span>
</a>

<style>
  .card {
    display: flex;
    flex-direction: column;
    min-width: 0;
    color: inherit;
    text-decoration: none;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    overflow: hidden;
    outline-offset: 3px;
    transition: border-color 0.12s ease, transform 0.12s ease;
  }

  .card:hover {
    text-decoration: none;
    border-color: var(--border-strong);
    transform: translateY(-1px);
  }

  .thumb {
    position: relative;
    display: block;
    width: 100%;
    aspect-ratio: var(--thumb-aspect, 1);
    overflow: hidden;
    background: var(--panel-2);
    flex: 0 0 auto;
  }

  img {
    width: 100%;
    height: 100%;
    object-fit: cover;
    opacity: 0;
    transition: opacity 0.2s ease;
  }

  img.loaded {
    opacity: 1;
  }

  .placeholder {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
    color: var(--muted);
    background:
      linear-gradient(
        135deg,
        transparent 0 45%,
        var(--panel-2) 45% 55%,
        transparent 55%
      ),
      var(--panel);
  }

  .badges {
    position: absolute;
    top: 5px;
    right: 5px;
    display: flex;
    align-items: center;
    gap: 4px;
    pointer-events: none;
  }

  .ext,
  .duration {
    padding: 2px 5px;
    border-radius: 4px;
    background: rgba(6, 8, 12, 0.72);
    color: #eef1f7;
    font-size: 9px;
    line-height: 1.2;
    letter-spacing: 0.03em;
    backdrop-filter: blur(4px);
  }

  .duration {
    letter-spacing: 0;
    font-variant-numeric: tabular-nums;
  }

  .play {
    position: absolute;
    inset: 0;
    display: grid;
    place-items: center;
    color: rgba(255, 255, 255, 0.86);
    filter: drop-shadow(0 2px 8px rgba(0, 0, 0, 0.55));
    pointer-events: none;
  }

  .info {
    display: flex;
    align-items: center;
    gap: 6px;
    height: 24px;
    padding: 0 6px;
    border-top: 1px solid var(--border);
    background: var(--panel);
    color: var(--text-dim);
    font-size: 10px;
    font-variant-numeric: tabular-nums;
    flex: 0 0 auto;
  }

  .rating-letter {
    width: 14px;
    height: 14px;
    display: grid;
    place-items: center;
    border: 1px solid color-mix(in srgb, var(--rating) 65%, var(--border));
    border-radius: 3px;
    background: color-mix(in srgb, var(--rating) 12%, transparent);
    color: var(--rating);
    font-size: 9px;
    font-weight: 700;
    line-height: 1;
  }

  .score {
    color: var(--text-dim);
  }

  .score.negative {
    color: var(--danger);
  }

  .post-id {
    margin-left: auto;
    color: var(--muted);
    font-size: 9.5px;
  }

  .filename {
    display: block;
    padding: 2px 6px 5px;
    color: var(--muted);
    font-size: 9.5px;
    line-height: 1.25;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
    flex: 0 0 auto;
  }
</style>
