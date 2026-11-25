<script lang="ts">
  import BrandMark from './BrandMark.svelte';
  import Icon from './Icon.svelte';
  import ThemePicker from './ThemePicker.svelte';
  import { defaultApiBase, pingServerAt } from '../api';

  let {
    apiBase,
    theme,
    onclose,
    onsave,
  }: {
    apiBase: string;
    theme: string;
    onclose: () => void;
    onsave: (apiBase: string, theme: string) => void;
  } = $props();

  let api = $state('');
  let selectedTheme = $state('');
  let testing = $state(false);
  let testOk = $state(false);
  let testText = $state('');

  $effect(() => {
    // The panel is created with the current values; keep them in sync if
    // the parent ever changes them while the dialog is open.
    api = apiBase;
    selectedTheme = theme;
  });

  $effect(() => {
    const onKey = (event: KeyboardEvent) => {
      if (event.key === 'Escape') onclose();
    };
    window.addEventListener('keydown', onKey);
    return () => window.removeEventListener('keydown', onKey);
  });

  async function test(): Promise<void> {
    if (testing) return;
    testing = true;
    testOk = false;
    testText = 'Testing…';
    try {
      testText = await pingServerAt(api);
      testOk = true;
    } catch (error) {
      testOk = false;
      testText =
        error instanceof Error ? error.message : 'Could not reach the server.';
    } finally {
      testing = false;
    }
  }

  function save(): void {
    onsave(api, selectedTheme);
  }

  function useDefault(): void {
    api = defaultApiBase();
    testText = '';
  }
</script>

<div
  class="overlay"
  role="presentation"
  onclick={(e) => {
    if (e.target === e.currentTarget) onclose();
  }}
>
  <div class="panel" role="dialog" aria-modal="true" aria-label="Settings">
    <header>
      <span class="mark"><BrandMark size={28} /></span>
      <div class="titles">
        <h2>Box Share settings</h2>
        <p>Booru Explorer, LAN library</p>
      </div>
      <button
        type="button"
        class="icon-button"
        onclick={onclose}
        aria-label="Close settings"
        title="Close"
      >
        <Icon name="x" size={18} />
      </button>
    </header>

    <div class="body">
      <section class="block">
        <h3>Server</h3>
        <p class="hint">
          The read-only Box share API. Leave the default when this page is
          served by Box. A separately hosted webapp can point at another Box
          instance.
        </p>
        <div class="row">
          <input
            bind:value={api}
            placeholder={defaultApiBase()}
            spellcheck="false"
            autocomplete="off"
            aria-label="API server URL"
          />
          <button type="button" class="small" onclick={useDefault}>
            Default
          </button>
          <button type="button" class="small" onclick={test} disabled={testing}>
            {#if testing}<span class="spinner"></span>{:else}Test{/if}
          </button>
        </div>
        {#if testText}
          <p class="status" class:ok={testOk}>{testText}</p>
        {/if}
      </section>

      <section class="block">
        <h3>Theme</h3>
        <p class="hint">
          Same palettes as Booru Explorer's desktop themes. System follows your
          browser and OS preference.
        </p>
        <ThemePicker
          value={selectedTheme}
          onchange={(key) => (selectedTheme = key)}
        />
      </section>

      <section class="block note">
        <Icon name="eye" size={16} />
        <p>
          This library is read-only. Anyone on the network can browse and fetch
          shared media. There are no accounts, edits, uploads, or favourites.
        </p>
      </section>
    </div>

    <footer>
      <span class="brandline">
        <BrandMark size={18} />
        <span>Booru Explorer</span>
      </span>
      <span class="actions">
        <button type="button" class="ghost" onclick={onclose}>Cancel</button>
        <button type="button" class="primary" onclick={save}>Save</button>
      </span>
    </footer>
  </div>
</div>

<style>
  .overlay {
    position: fixed;
    inset: 0;
    z-index: 120;
    display: grid;
    place-items: center;
    padding: 18px;
    background: rgba(4, 6, 10, 0.62);
    backdrop-filter: blur(3px);
  }

  .panel {
    width: min(720px, 100%);
    max-height: min(88vh, 820px);
    display: flex;
    flex-direction: column;
    border: 1px solid var(--border-strong);
    border-radius: 14px;
    background: var(--bg-elev);
    box-shadow: var(--shadow);
    overflow: hidden;
  }

  header {
    display: flex;
    align-items: center;
    gap: 10px;
    padding: 13px 14px;
    border-bottom: 1px solid var(--border);
    background:
      linear-gradient(180deg, var(--accent-soft), transparent 140%),
      var(--panel);
  }

  .mark {
    display: grid;
    place-items: center;
    width: 40px;
    height: 40px;
    border-radius: 10px;
    background: var(--panel-2);
    border: 1px solid var(--border);
  }

  .titles {
    min-width: 0;
    flex: 1;
  }

  .titles h2 {
    margin: 0;
    font-size: 15px;
    font-weight: 650;
    letter-spacing: 0.01em;
  }

  .titles p {
    margin: 1px 0 0;
    color: var(--muted);
    font-size: 11px;
  }

  .icon-button {
    width: 30px;
    height: 30px;
    display: grid;
    place-items: center;
    border: 0;
    border-radius: 7px;
    background: transparent;
    color: var(--muted);
  }

  .icon-button:hover {
    color: var(--text);
    background: var(--panel-2);
  }

  .body {
    padding: 14px;
    overflow: auto;
    display: flex;
    flex-direction: column;
    gap: 18px;
  }

  .block h3 {
    margin: 0 0 4px;
    font-size: 12px;
    font-weight: 650;
    letter-spacing: 0.06em;
    text-transform: uppercase;
    color: var(--text-dim);
  }

  .hint {
    margin: 0 0 9px;
    color: var(--muted);
    font-size: 12px;
    line-height: 1.5;
    max-width: 62ch;
  }

  .row {
    display: flex;
    gap: 6px;
    align-items: stretch;
  }

  input {
    min-width: 0;
    flex: 1;
    height: 34px;
    padding: 0 10px;
    border: 1px solid var(--border);
    border-radius: 7px;
    background: var(--panel-2);
    color: var(--text);
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 12px;
  }

  input:focus {
    outline: none;
    border-color: var(--accent);
    box-shadow: 0 0 0 3px var(--accent-soft);
  }

  .small {
    height: 34px;
    padding: 0 11px;
    border: 1px solid var(--border);
    border-radius: 7px;
    background: var(--panel-2);
    color: var(--text-dim);
    font-size: 12px;
    display: inline-flex;
    align-items: center;
    gap: 7px;
  }

  .small:hover {
    border-color: var(--border-strong);
    color: var(--text);
  }

  .small:disabled {
    opacity: 0.6;
    cursor: default;
  }

  .small .spinner {
    width: 13px;
    height: 13px;
  }

  .status {
    margin: 7px 0 0;
    font-size: 12px;
    color: var(--danger);
    overflow-wrap: anywhere;
  }

  .status.ok {
    color: var(--ok);
  }

  .note {
    display: flex;
    align-items: flex-start;
    gap: 9px;
    padding: 10px 11px;
    border: 1px solid var(--border);
    border-radius: 8px;
    background: var(--panel-2);
    color: var(--text-dim);
    font-size: 12px;
  }

  .note :global(svg) {
    color: var(--accent-bright);
    flex: 0 0 auto;
    margin-top: 1px;
  }

  .note p {
    margin: 0;
    line-height: 1.5;
  }

  footer {
    display: flex;
    align-items: center;
    justify-content: space-between;
    gap: 12px;
    padding: 10px 14px;
    border-top: 1px solid var(--border);
    background: var(--panel);
  }

  .brandline {
    display: inline-flex;
    align-items: center;
    gap: 6px;
    color: var(--muted);
    font-size: 11px;
  }

  .actions {
    display: inline-flex;
    gap: 7px;
  }

  .ghost,
  .primary {
    height: 32px;
    padding: 0 15px;
    border-radius: 7px;
    border: 1px solid var(--border);
    background: var(--panel-2);
    color: var(--text-dim);
    font-size: 12.5px;
  }

  .ghost:hover {
    color: var(--text);
    border-color: var(--border-strong);
  }

  .primary {
    border-color: var(--accent);
    background: var(--accent);
    color: #fff;
    font-weight: 600;
  }

  .primary:hover {
    filter: brightness(1.08);
  }

  @media (max-width: 560px) {
    .overlay {
      padding: 0;
      place-items: stretch;
    }

    .panel {
      width: 100%;
      max-height: 100vh;
      border-radius: 0;
      border: 0;
    }

    .row {
      flex-wrap: wrap;
    }

    input {
      flex-basis: 100%;
    }

    .brandline {
      display: none;
    }
  }
</style>
