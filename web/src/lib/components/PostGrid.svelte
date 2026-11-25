<script lang="ts">
  import Icon from './Icon.svelte';
  import PostCard from './PostCard.svelte';
  import { formatCount } from '../query';
  import type { DanbooruPost } from '../types';

  let {
    posts,
    loading,
    loadingMore,
    error,
    hasMore,
    total = null,
    onopen,
    onloadmore,
    onretry,
  }: {
    posts: DanbooruPost[];
    loading: boolean;
    loadingMore: boolean;
    error: string;
    hasMore: boolean;
    total?: number | null;
    onopen: (post: DanbooruPost) => void;
    onloadmore: () => void;
    onretry: () => void;
  } = $props();

  let sentinel = $state<HTMLDivElement | null>(null);
  const hasObserver = typeof IntersectionObserver !== 'undefined';

  $effect(() => {
    if (!sentinel || !hasObserver) return;
    const node = sentinel;
    const observer = new IntersectionObserver(
      (entries) => {
        if (entries.some((entry) => entry.isIntersecting)) {
          onloadmore();
        }
      },
      { rootMargin: '900px 0px' },
    );
    observer.observe(node);
    return () => observer.disconnect();
  });

  const skeletons = Array.from({ length: 18 }, (_, i) => i);
</script>

{#if loading && posts.length === 0}
  <div class="grid" aria-busy="true">
    {#each skeletons as n (n)}
      <div class="skeleton" style={`animation-delay:${(n % 6) * 80}ms`}></div>
    {/each}
  </div>
{:else if error && posts.length === 0}
  <div class="state error">
    <Icon name="alert" size={30} />
    <h2>Could not load the library</h2>
    <p>{error}</p>
    <button type="button" onclick={onretry}>Try again</button>
  </div>
{:else if posts.length === 0}
  <div class="state">
    <Icon name="search" size={30} />
    <h2>No posts</h2>
    <p>Try a different tag or clear the search.</p>
  </div>
{:else}
  <div class="grid" aria-busy={loading || loadingMore}>
    {#each posts as post (post.id)}
      <PostCard {post} {onopen} />
    {/each}
  </div>

  {#if error}
    <div class="inline-error">
      <span>{error}</span>
      <button type="button" onclick={onretry}>Retry</button>
    </div>
  {/if}

  {#if hasMore}
    <div bind:this={sentinel} class="sentinel" aria-hidden="true">
      {#if loadingMore}
        <span class="spinner"></span>
        <span>Loading more…</span>
      {:else if !hasObserver}
        <button type="button" onclick={onloadmore}>Load more</button>
      {/if}
    </div>
  {:else}
    <div class="end">
      {#if total != null}
        {formatCount(total)} {total === 1 ? 'post' : 'posts'}
      {:else}
        End of results
      {/if}
    </div>
  {/if}
{/if}

<style>
  .grid {
    display: grid;
    grid-template-columns:
      repeat(auto-fill, minmax(min(100%, var(--card-w, 184px)), 1fr));
    gap: 10px;
    align-items: start;
    padding-bottom: 20px;
  }

  .skeleton {
    aspect-ratio: var(--thumb-aspect, 1);
    border-radius: var(--radius);
    border: 1px solid var(--border);
    background:
      linear-gradient(
        100deg,
        var(--panel) 30%,
        var(--panel-2) 50%,
        var(--panel) 70%
      );
    background-size: 200% 100%;
    animation: box-skeleton 1.3s ease-in-out infinite;
    animation-delay: var(--delay, 0ms);
  }

  @keyframes box-skeleton {
    from {
      background-position: 180% 0;
    }
    to {
      background-position: -20% 0;
    }
  }

  .state {
    min-height: 46vh;
    display: flex;
    flex-direction: column;
    align-items: center;
    justify-content: center;
    gap: 8px;
    color: var(--muted);
    text-align: center;
    padding: 40px 20px;
  }

  .state h2 {
    margin: 8px 0 0;
    color: var(--text);
    font-size: 18px;
    font-weight: 600;
  }

  .state p {
    margin: 0;
    max-width: 480px;
    color: var(--text-dim);
  }

  .state button,
  .inline-error button {
    margin-top: 10px;
    padding: 7px 14px;
    border: 1px solid var(--border-strong);
    border-radius: 7px;
    background: var(--panel);
    color: var(--text);
  }

  .state button:hover,
  .inline-error button:hover {
    border-color: var(--accent);
  }

  .state.error {
    color: var(--danger);
  }

  .inline-error {
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 12px;
    margin: 6px 0 14px;
    padding: 9px 12px;
    border: 1px solid color-mix(in srgb, var(--danger) 45%, var(--border));
    border-radius: var(--radius-sm);
    color: var(--danger);
    font-size: 12px;
    background: color-mix(in srgb, var(--danger) 8%, var(--panel));
  }

  .inline-error button {
    margin: 0;
  }

  .sentinel {
    min-height: 54px;
    display: flex;
    align-items: center;
    justify-content: center;
    gap: 10px;
    color: var(--muted);
    font-size: 12px;
  }

  .sentinel button {
    padding: 7px 14px;
    border: 1px solid var(--border-strong);
    border-radius: 7px;
    background: var(--panel);
    color: var(--text);
  }

  .end {
    padding: 18px 0 30px;
    color: var(--muted);
    font-size: 12px;
    text-align: center;
  }

  @media (max-width: 640px) {
    .grid {
      grid-template-columns: repeat(auto-fill, minmax(min(100%, 140px), 1fr));
      gap: 7px;
    }
  }
</style>
