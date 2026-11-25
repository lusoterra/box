<script lang="ts">
  import Icon from './Icon.svelte';
  import { absoluteMediaUrl } from '../api';
  import {
    fileUrl,
    isVideoPost,
    postAspect,
    postExt,
    previewUrl,
    ratingOf,
  } from '../post';
  import { altForPost, formatDuration, ratingColor } from '../query';
  import type { DanbooruPost } from '../types';

  let { post, onopen }: { post: DanbooruPost; onopen: (post: DanbooruPost) => void } =
    $props();

  let failed = $state(false);
  let useFile = $state(false);
  let loaded = $state(false);

  let src = $derived(absoluteMediaUrl(useFile ? fileUrl(post) : previewUrl(post)));
  let video = $derived(isVideoPost(post));
  let ext = $derived(postExt(post));
  let rating = $derived(ratingOf(post));
  let aspect = $derived(postAspect(post));
  let duration = $derived(formatDuration(post.media_asset?.duration));
  let dims = $derived(
    post.image_width && post.image_height
      ? `${post.image_width}×${post.image_height}`
      : '',
  );

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
  <span class="thumb" style={`aspect-ratio:${aspect}`}>
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

    <span class="shade"></span>

    {#if video}
      <span class="play"><Icon name="play" size={22} /></span>
    {/if}

    <span class="badges">
      <span class="rating" style={`--rating:${ratingColor(rating)}`}></span>
      {#if ext}<span class="ext">{ext.toUpperCase()}</span>{/if}
      {#if duration}<span class="duration">{duration}</span>{/if}
    </span>

    <span class="hover">
      <span>#{post.id}</span>
      {#if dims}<span>{dims}</span>{/if}
    </span>
  </span>
</a>

<style>
  .card {
    display: block;
    color: inherit;
    text-decoration: none;
    border-radius: var(--radius);
    outline-offset: 3px;
  }

  .card:hover {
    text-decoration: none;
  }

  .thumb {
    position: relative;
    display: block;
    width: 100%;
    overflow: hidden;
    border-radius: var(--radius);
    background: var(--panel);
    border: 1px solid var(--border);
    box-shadow: 0 1px 0 rgba(255, 255, 255, 0.02);
    transition: border-color 0.12s ease, transform 0.12s ease;
  }

  .card:hover .thumb {
    border-color: var(--border-strong);
    transform: translateY(-1px);
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
      linear-gradient(135deg, transparent 0 45%, var(--panel-2) 45% 55%, transparent 55%),
      var(--panel);
  }

  .shade {
    position: absolute;
    inset: 0;
    pointer-events: none;
    background: linear-gradient(
      to bottom,
      rgba(0, 0, 0, 0.22),
      transparent 22%,
      transparent 72%,
      rgba(0, 0, 0, 0.55)
    );
    opacity: 0;
    transition: opacity 0.15s ease;
  }

  .card:hover .shade {
    opacity: 1;
  }

  .badges {
    position: absolute;
    top: 6px;
    right: 6px;
    display: flex;
    align-items: center;
    gap: 4px;
    pointer-events: none;
  }

  .rating {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    background: var(--rating);
    box-shadow: 0 0 0 2px rgba(0, 0, 0, 0.45);
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

  .hover {
    position: absolute;
    left: 7px;
    right: 7px;
    bottom: 7px;
    display: flex;
    justify-content: space-between;
    gap: 8px;
    color: rgba(255, 255, 255, 0.86);
    font-size: 10px;
    font-variant-numeric: tabular-nums;
    opacity: 0;
    transition: opacity 0.15s ease;
    pointer-events: none;
    text-shadow: 0 1px 3px rgba(0, 0, 0, 0.7);
  }

  .card:hover .hover {
    opacity: 1;
  }
</style>
