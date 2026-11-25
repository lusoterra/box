<script lang="ts">
  import BrandMark from './BrandMark.svelte';
  import Icon from './Icon.svelte';
  import ThemePicker from './ThemePicker.svelte';
  import { defaultApiBase, pingServerAt } from '../api';
  import { THUMB_RATIOS, type GridSettings } from '../grid';
  import type { PlaybackSettings } from '../playback';

  let {
    apiBase,
    theme,
    grid,
    playback,
    onclose,
    onsave,
  }: {
    apiBase: string;
    theme: string;
    grid: GridSettings;
    playback: PlaybackSettings;
    onclose: () => void;
    onsave: (
      apiBase: string,
      theme: string,
      grid: GridSettings,
      playback: PlaybackSettings,
    ) => void;
  } = $props();

  let ratioOptions = $derived.by(() => {
    const options = [...THUMB_RATIOS];
    if (
      !options.some(
        (r) => r.num === grid.aspectNum && r.den === grid.aspectDen,
      )
    ) {
      options.push({
        label: `Custom ${grid.aspectNum}:${grid.aspectDen}`,
        num: grid.aspectNum,
        den: grid.aspectDen,
      });
    }
    return options;
  });

  let api = $state('');
  let selectedTheme = $state('');
  let aspectKey = $state('');
  let cardWidth = $state(0);
  let pbMuted = $state(true);
  let pbLoop = $state(true);
  let pbVolume = $state(100);
  let testing = $state(false);
  let testOk = $state(false);
  let testText = $state('');

  $effect(() => {
    // The panel is created with the current values; keep them in sync if
    // the parent ever changes them while the dialog is open.
    api = apiBase;
    selectedTheme = theme;
    aspectKey = `${grid.aspectNum}:${grid.aspectDen}`;
    cardWidth = grid.cardWidth;
    pbMuted = playback.muted;
    pbLoop = playback.loop;
    pbVolume = playback.volume;
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
    const [num, den] = aspectKey.split(':').map(Number);
    onsave(
      api,
      selectedTheme,
      {
        aspectNum: Number.isFinite(num) && num > 0 ? num : 1,
        aspectDen: Number.isFinite(den) && den > 0 ? den : 1,
        cardWidth,
      },
      { muted: pbMuted, loop: pbLoop, volume: pbVolume },
    );
  }

  function useDefault(): void {
    api = defaultApiBase();
    testText = '';
  }

  function useDanbooru(): void {
    api = 'https://danbooru.donmai.us';
    testText = '';
    testOk = false;
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
          When this page is served by Box, leave the default. It uses the
          host and IP you opened the page with, so another computer should
          open the Box LAN address (for example
          <code>http://192.168.1.5:7071/</code>), not
          <code>localhost</code>. A separately hosted copy can point here;
          Box must allow CORS.
        </p>
        <div class="row">
          <input
            class="text-input"
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

        <details class="dev">
          <summary>Testing against another server</summary>
          <p>
            Point the same UI at a public Danbooru server to exercise the
            gallery without a Box share. Posts, tags and media come directly
            from that server. You can also open
            <code>/?api=https://danbooru.donmai.us</code> once; the choice is
            saved in a browser cookie and localStorage. Public boorus behind
            Cloudflare can reject cross-origin browser requests, so use a
            server that allows CORS if the test reports a fetch failure.
          </p>
          <div class="row">
            <button type="button" class="small" onclick={useDanbooru}>
              Use Danbooru
            </button>
            <button type="button" class="small" onclick={useDefault}>
              Use this Box
            </button>
          </div>
        </details>
      </section>

      <section class="block">
        <h3>Grid</h3>
        <p class="hint">
          Thumbnail shape and card width. The ratios match Booru Explorer's
          Appearance → Grid settings; thumbnails are centre-cropped to fill.
        </p>
        <div class="grid-settings">
          <label class="field">
            <span>Thumbnail ratio</span>
            <select bind:value={aspectKey} aria-label="Thumbnail ratio">
              {#each ratioOptions as ratio (ratio.label)}
                <option value={`${ratio.num}:${ratio.den}`}>
                  {ratio.label}
                </option>
              {/each}
            </select>
          </label>

          <label class="field">
            <span>Card size <em>{cardWidth}px</em></span>
            <input
              type="range"
              min="96"
              max="384"
              step="8"
              value={cardWidth}
              oninput={(e) =>
                (cardWidth = Number(e.currentTarget.value))}
              aria-label="Card size"
            />
          </label>
        </div>
      </section>

      <section class="block">
        <h3>Playback</h3>
        <p class="hint">
          Same video defaults as Booru Explorer. The viewer's mute button
          only affects the current video; each new video starts with this
          default.
        </p>
        <label class="check">
          <input type="checkbox" bind:checked={pbMuted} />
          <span>Mute videos by default</span>
        </label>
        <label class="check">
          <input type="checkbox" bind:checked={pbLoop} />
          <span>Loop videos</span>
        </label>
        <label class="field volume-field">
          <span>Default volume <em>{pbVolume}%</em></span>
          <input
            type="range"
            min="0"
            max="100"
            step="1"
            value={pbVolume}
            oninput={(e) => (pbVolume = Number(e.currentTarget.value))}
            aria-label="Default video volume"
          />
        </label>
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

  .grid-settings {
    display: grid;
    grid-template-columns: minmax(180px, 1fr) minmax(180px, 1fr);
    gap: 10px 14px;
    align-items: end;
  }

  .field {
    display: flex;
    flex-direction: column;
    gap: 5px;
    min-width: 0;
    color: var(--muted);
    font-size: 11.5px;
  }

  .field em {
    color: var(--text-dim);
    font-style: normal;
    font-variant-numeric: tabular-nums;
  }

  .field select {
    height: 32px;
    padding: 0 8px;
    border: 1px solid var(--border);
    border-radius: 7px;
    background: var(--panel-2);
    color: var(--text);
    font-size: 12px;
  }

  .field input[type='range'] {
    accent-color: var(--accent);
    width: 100%;
    height: 32px;
    cursor: pointer;
  }

  .check {
    display: flex;
    align-items: center;
    gap: 8px;
    min-height: 26px;
    color: var(--text-dim);
    font-size: 12px;
    cursor: pointer;
  }

  .check input {
    accent-color: var(--accent);
    width: 15px;
    height: 15px;
    cursor: pointer;
  }

  .volume-field {
    margin-top: 6px;
    max-width: 300px;
  }

  .row {
    display: flex;
    gap: 6px;
    align-items: stretch;
  }

  .text-input {
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

  .text-input:focus {
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

  code {
    padding: 1px 4px;
    border-radius: 4px;
    background: var(--panel-2);
    color: var(--text-dim);
    font-family: ui-monospace, SFMono-Regular, Menlo, Consolas, monospace;
    font-size: 11px;
    overflow-wrap: anywhere;
  }

  .dev {
    margin-top: 11px;
    padding-top: 9px;
    border-top: 1px solid var(--border);
  }

  .dev summary {
    cursor: pointer;
    color: var(--muted);
    font-size: 11.5px;
    user-select: none;
  }

  .dev summary:hover {
    color: var(--text-dim);
  }

  .dev p {
    margin: 7px 0 8px;
    color: var(--muted);
    font-size: 11.5px;
    line-height: 1.5;
    max-width: 62ch;
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

    .grid-settings {
      grid-template-columns: 1fr;
    }

    .text-input {
      flex-basis: 100%;
    }

    .brandline {
      display: none;
    }
  }
</style>
