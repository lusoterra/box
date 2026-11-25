<script lang="ts">
  import Icon from './Icon.svelte';
  import { THEMES } from '../themes';

  let {
    value,
    onchange,
  }: { value: string; onchange: (key: string) => void } = $props();

  const ordered = [...THEMES].sort((a, b) => a.order - b.order);
</script>

<div class="themes" role="listbox" aria-label="Theme">
  {#each ordered as theme (theme.key)}
    <button
      type="button"
      class="theme"
      class:selected={value === theme.key}
      class:system={theme.key === 'system'}
      role="option"
      aria-selected={value === theme.key}
      onclick={() => onchange(theme.key)}
      title={theme.label}
      style={`--sw-window:${theme.window};--sw-base:${theme.base};--sw-alternate:${theme.alternate};--sw-accent:${theme.accent};--sw-text:${theme.text};`}
    >
      <span class="swatch" aria-hidden="true">
        <span class="mini">
          <span class="mini-head"></span>
          <span class="mini-body"></span>
          <span class="mini-accent"></span>
        </span>
      </span>
      <span class="label">{theme.label}</span>
      {#if value === theme.key}
        <span class="check"><Icon name="check" size={13} /></span>
      {/if}
    </button>
  {/each}
</div>

<style>
  .themes {
    display: grid;
    grid-template-columns: repeat(auto-fit, minmax(148px, 1fr));
    gap: 7px;
    max-height: 270px;
    overflow: auto;
    padding: 2px;
  }

  .theme {
    position: relative;
    display: flex;
    align-items: center;
    gap: 9px;
    min-width: 0;
    padding: 7px 9px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--panel-2);
    color: var(--text-dim);
    text-align: left;
    font-size: 12px;
  }

  .theme:hover {
    border-color: var(--border-strong);
    color: var(--text);
  }

  .theme.selected {
    border-color: var(--accent);
    background: var(--accent-soft);
    color: var(--text);
  }

  .swatch {
    width: 38px;
    height: 28px;
    flex: 0 0 auto;
    border-radius: 5px;
    background: var(--sw-window);
    border: 1px solid color-mix(in srgb, var(--sw-window) 60%, var(--sw-text) 40%);
    display: grid;
    place-items: center;
    overflow: hidden;
  }

  .system .swatch {
    background: linear-gradient(135deg, #23252e 0 50%, #f5f6f8 50% 100%);
  }

  .mini {
    width: 26px;
    height: 18px;
    border-radius: 3px;
    background: var(--sw-base);
    display: flex;
    flex-direction: column;
    overflow: hidden;
    box-shadow: 0 0 0 1px rgba(0, 0, 0, 0.25);
  }

  .mini-head {
    height: 4px;
    background: var(--sw-alternate);
  }

  .mini-body {
    flex: 1;
    margin: 2px;
    border-radius: 2px;
    background: var(--sw-window);
  }

  .mini-accent {
    height: 3px;
    margin: 0 2px 2px;
    border-radius: 2px;
    background: var(--sw-accent);
  }

  .label {
    min-width: 0;
    line-height: 1.25;
    overflow-wrap: anywhere;
  }

  .check {
    margin-left: auto;
    color: var(--accent-bright);
    flex: 0 0 auto;
  }
</style>
