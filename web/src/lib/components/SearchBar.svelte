<script lang="ts">
  import Icon from './Icon.svelte';
  import { fetchTagSuggestions } from '../api';
  import {
    RATINGS,
    categoryColor,
    formatCount,
    lastToken,
    ratingLabel,
    replaceLastToken,
  } from '../query';
  import type { Rating, TagSuggestion } from '../types';

  let {
    query = '',
    rating = 'all',
    total = null,
    loading = false,
    cardWidth = 184,
    onsubmit,
    onrating,
    onsettings,
    onrefresh,
    oncardwidth,
  }: {
    query?: string;
    rating?: Rating;
    total?: number | null;
    loading?: boolean;
    cardWidth?: number;
    onsubmit: (query: string) => void;
    onrating: (rating: Rating) => void;
    onsettings: () => void;
    onrefresh: () => void;
    oncardwidth: (width: number) => void;
  } = $props();

  let text = $state('');
  let input = $state<HTMLInputElement | null>(null);
  let suggestions = $state<TagSuggestion[]>([]);
  let suggestOpen = $state(false);
  let highlight = $state(-1);
  let focused = $state(false);
  let suggestSeq = 0;
  let suggestTimer: number | undefined;

  $effect(() => {
    text = query;
  });

  $effect(() => {
    const value = text;
    if (suggestTimer) window.clearTimeout(suggestTimer);
    const token = lastToken(value);
    const seq = ++suggestSeq;
    if (token.length < 2) {
      suggestions = [];
      suggestOpen = false;
      return;
    }
    suggestTimer = window.setTimeout(async () => {
      try {
        const rows = await fetchTagSuggestions(token, 24);
        if (seq !== suggestSeq) return;
        suggestions = rows;
        highlight = rows.length ? 0 : -1;
        suggestOpen = focused && rows.length > 0;
      } catch {
        if (seq !== suggestSeq) return;
        suggestions = [];
        suggestOpen = false;
      }
    }, 160);
    return () => {
      if (suggestTimer) window.clearTimeout(suggestTimer);
    };
  });

  function submit(): void {
    suggestOpen = false;
    onsubmit(text.trim());
  }

  function accept(name: string): void {
    text = replaceLastToken(text, name);
    suggestions = [];
    suggestOpen = false;
    highlight = -1;
    input?.focus();
    onsubmit(text.trim());
  }

  function onkeydown(event: KeyboardEvent): void {
    if (event.key === 'ArrowDown' && suggestOpen) {
      event.preventDefault();
      highlight = (highlight + 1) % suggestions.length;
      return;
    }
    if (event.key === 'ArrowUp' && suggestOpen) {
      event.preventDefault();
      highlight =
        (highlight - 1 + suggestions.length) % suggestions.length;
      return;
    }
    if (event.key === 'Escape') {
      if (suggestOpen) {
        event.preventDefault();
        suggestOpen = false;
      }
      return;
    }
    if (event.key === 'Enter') {
      if (suggestOpen && highlight >= 0 && suggestions[highlight]) {
        event.preventDefault();
        accept(suggestions[highlight].name);
      }
    }
  }

  function clear(): void {
    text = '';
    suggestions = [];
    suggestOpen = false;
    onsubmit('');
    input?.focus();
  }
</script>

<div class="search-shell">
  <form class="search" onsubmit={(e) => { e.preventDefault(); submit(); }}>
    <span class="lead" aria-hidden="true"><Icon name="search" size={17} /></span>
    <input
      bind:this={input}
      id="search-input"
      bind:value={text}
      onkeydown={onkeydown}
      onfocus={() => {
        focused = true;
        suggestOpen = suggestions.length > 0;
      }}
      onblur={() => {
        focused = false;
        suggestOpen = false;
      }}
      placeholder="Search tags, e.g. 1girl -classroom"
      autocomplete="off"
      spellcheck="false"
      aria-label="Search tags"
    />
    {#if text}
      <button
        type="button"
        class="icon-button clear"
        onclick={clear}
        title="Clear search"
        aria-label="Clear search"
      >
        <Icon name="x" size={15} />
      </button>
    {/if}
    <button class="submit" type="submit" disabled={loading}>
      {#if loading}<span class="spinner"></span>{:else}Search{/if}
    </button>
  </form>

  {#if suggestOpen && suggestions.length}
    <div class="suggestions" role="listbox">
      {#each suggestions as suggestion, index (suggestion.name)}
        <button
          type="button"
          class="suggestion"
          class:active={index === highlight}
          role="option"
          aria-selected={index === highlight}
          onmousedown={(e) => e.preventDefault()}
          onclick={() => accept(suggestion.name)}
          onmouseenter={() => (highlight = index)}
        >
          <span
            class="dot"
            style={`background:${categoryColor(suggestion.category)}`}
          ></span>
          <span class="name">{suggestion.name}</span>
          {#if suggestion.post_count > 0}
            <span class="count">{formatCount(suggestion.post_count)}</span>
          {/if}
        </button>
      {/each}
    </div>
  {/if}

  <div class="tools">
    <label class="rating-select" title="Content rating">
      <Icon name="filter" size={15} />
      <select
        value={rating}
        onchange={(e) => onrating(e.currentTarget.value as Rating)}
        aria-label="Content rating"
      >
        {#each RATINGS as option}
          <option value={option}>{ratingLabel(option)}</option>
        {/each}
      </select>
    </label>

    {#if total != null}
      <span class="total" title="Matching posts">
        {formatCount(total)} {total === 1 ? 'post' : 'posts'}
      </span>
    {/if}

    <span class="readonly" title="Read-only LAN library">read only</span>

    <button
      type="button"
      class="icon-button"
      onclick={onrefresh}
      title="Refresh results"
      aria-label="Refresh results"
    >
      <Icon name="refresh" size={16} />
    </button>
    <button
      type="button"
      class="icon-button"
      onclick={onsettings}
      title="Server and appearance settings"
      aria-label="Settings"
    >
      <Icon name="settings" size={16} />
    </button>

    <label class="size-slider" title="Grid card size">
      <Icon name="grid" size={14} />
      <input
        type="range"
        min="96"
        max="384"
        step="8"
        value={cardWidth}
        oninput={(e) => oncardwidth(Number(e.currentTarget.value))}
        aria-label="Grid card size"
      />
    </label>
  </div>
</div>

<style>
  .search-shell {
    position: relative;
    flex: 1 1 520px;
    min-width: 0;
    display: flex;
    flex-direction: column;
    gap: 7px;
  }

  .search {
    display: flex;
    align-items: center;
    gap: 0;
    height: 38px;
    background: var(--panel);
    border: 1px solid var(--border);
    border-radius: var(--radius);
    box-shadow: 0 1px 0 rgba(255, 255, 255, 0.03);
    transition: border-color 0.12s ease, box-shadow 0.12s ease;
  }

  .search:focus-within {
    border-color: var(--accent);
    box-shadow: 0 0 0 3px var(--accent-soft);
  }

  .lead {
    display: grid;
    place-items: center;
    padding: 0 10px 0 12px;
    color: var(--muted);
  }

  input {
    min-width: 0;
    flex: 1;
    height: 100%;
    border: 0;
    outline: none;
    background: transparent;
    color: var(--text);
    font-size: 14px;
  }

  input::placeholder {
    color: var(--muted);
  }

  .submit {
    height: 30px;
    margin: 0 4px 0 6px;
    padding: 0 13px;
    border: 1px solid var(--accent);
    border-radius: 7px;
    background: var(--accent);
    color: #fff;
    font-size: 13px;
    font-weight: 600;
    display: inline-flex;
    align-items: center;
    justify-content: center;
    min-width: 68px;
  }

  .submit:hover:not(:disabled) {
    filter: brightness(1.1);
  }

  .submit:disabled {
    opacity: 0.65;
    cursor: default;
  }

  .submit .spinner {
    width: 14px;
    height: 14px;
    border-color: rgba(255, 255, 255, 0.35);
    border-top-color: #fff;
  }

  .icon-button {
    width: 28px;
    height: 28px;
    display: grid;
    place-items: center;
    border: 0;
    border-radius: 6px;
    background: transparent;
    color: var(--muted);
    flex: 0 0 auto;
  }

  .icon-button:hover {
    color: var(--text);
    background: var(--panel-2);
  }

  .clear {
    margin-right: 1px;
  }

  .tools {
    display: flex;
    align-items: center;
    gap: 5px;
    min-width: 0;
    flex-wrap: wrap;
  }

  .rating-select {
    display: inline-flex;
    align-items: center;
    gap: 5px;
    height: 27px;
    padding: 0 6px 0 8px;
    border: 1px solid var(--border);
    border-radius: 7px;
    background: var(--panel);
    color: var(--muted);
  }

  .rating-select select {
    border: 0;
    outline: none;
    background: transparent;
    color: var(--text-dim);
    font-size: 12px;
    padding: 0 2px 0 0;
    cursor: pointer;
  }

  .total {
    margin-left: 4px;
    color: var(--text-dim);
    font-size: 12px;
    white-space: nowrap;
  }

  .readonly {
    margin-left: 2px;
    padding: 2px 7px;
    border: 1px solid var(--border);
    border-radius: 99px;
    color: var(--muted);
    font-size: 10px;
    letter-spacing: 0.04em;
    text-transform: uppercase;
  }

  .size-slider {
    margin-left: auto;
    display: inline-flex;
    align-items: center;
    gap: 6px;
    padding: 0 2px;
    color: var(--muted);
    flex: 0 0 auto;
  }

  .size-slider input {
    width: 92px;
    accent-color: var(--accent);
    cursor: pointer;
  }

  .suggestions {
    position: absolute;
    z-index: 40;
    top: 42px;
    left: 0;
    right: 0;
    max-height: min(52vh, 420px);
    overflow: auto;
    padding: 5px;
    border: 1px solid var(--border-strong);
    border-radius: var(--radius);
    background: var(--bg-elev);
    box-shadow: var(--shadow);
  }

  .suggestion {
    width: 100%;
    display: flex;
    align-items: center;
    gap: 8px;
    padding: 7px 8px;
    border: 0;
    border-radius: 6px;
    background: transparent;
    color: var(--text);
    text-align: left;
    font-size: 13px;
  }

  .suggestion.active,
  .suggestion:hover {
    background: var(--accent-soft);
  }

  .dot {
    width: 7px;
    height: 7px;
    border-radius: 50%;
    flex: 0 0 auto;
  }

  .name {
    flex: 1;
    overflow: hidden;
    text-overflow: ellipsis;
    white-space: nowrap;
  }

  .count {
    color: var(--muted);
    font-variant-numeric: tabular-nums;
    font-size: 11px;
  }

  @media (max-width: 640px) {
    .search-shell {
      flex-basis: 100%;
    }

    .readonly {
      display: none;
    }

    .size-slider {
      display: none;
    }
  }
</style>
