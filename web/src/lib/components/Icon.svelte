<script lang="ts">
  let {
    name,
    size = 18,
    stroke = 1.8,
  }: { name: string; size?: number; stroke?: number } = $props();
</script>

<svg
  width={size}
  height={size}
  viewBox="0 0 24 24"
  fill="none"
  stroke="currentColor"
  stroke-width={stroke}
  stroke-linecap="round"
  stroke-linejoin="round"
  aria-hidden="true"
>
  {#if name === 'search'}
    <circle cx="11" cy="11" r="7" />
    <path d="m20 20-3.4-3.4" />
  {:else if name === 'x'}
    <path d="M18 6 6 18M6 6l12 12" />
  {:else if name === 'settings'}
    <path
      d="M12 15.2a3.2 3.2 0 1 0 0-6.4 3.2 3.2 0 0 0 0 6.4Z"
    />
    <path
      d="M19.4 15a1.7 1.7 0 0 0 .34 1.87l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.7 1.7 0 0 0-1.87-.34 1.7 1.7 0 0 0-1 1.55V21a2 2 0 1 1-4 0v-.09a1.7 1.7 0 0 0-1.1-1.55 1.7 1.7 0 0 0-1.87.34l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06A1.7 1.7 0 0 0 4.6 15a1.7 1.7 0 0 0-1.55-1H3a2 2 0 1 1 0-4h.09A1.7 1.7 0 0 0 4.6 8.9a1.7 1.7 0 0 0-.34-1.87l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06A1.7 1.7 0 0 0 8.96 4.6 1.7 1.7 0 0 0 10 3.05V3a2 2 0 1 1 4 0v.09a1.7 1.7 0 0 0 1.1 1.55 1.7 1.7 0 0 0 1.87-.34l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.7 1.7 0 0 0-.34 1.87V9a1.7 1.7 0 0 0 1.55 1H21a2 2 0 1 1 0 4h-.09a1.7 1.7 0 0 0-1.51 1Z"
    />
  {:else if name === 'chevron-left'}
    <path d="m15 18-6-6 6-6" />
  {:else if name === 'chevron-right'}
    <path d="m9 18 6-6-6-6" />
  {:else if name === 'arrow-left'}
    <path d="M19 12H5M12 19l-7-7 7-7" />
  {:else if name === 'arrow-right'}
    <path d="M5 12h14M12 5l7 7-7 7" />
  {:else if name === 'download'}
    <path d="M12 3v12M7 10l5 5 5-5" />
    <path d="M5 21h14" />
  {:else if name === 'external'}
    <path d="M15 3h6v6M10 14 21 3" />
    <path d="M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6" />
  {:else if name === 'copy'}
    <rect x="9" y="9" width="11" height="11" rx="2" />
    <path d="M5 15H4a1 1 0 0 1-1-1V4a1 1 0 0 1 1-1h10a1 1 0 0 1 1 1v1" />
  {:else if name === 'maximize'}
    <path d="M8 3H5a2 2 0 0 0-2 2v3M16 3h3a2 2 0 0 1 2 2v3M8 21H5a2 2 0 0 1-2-2v-3M16 21h3a2 2 0 0 0 2-2v-3" />
  {:else if name === 'minimize'}
    <path d="M8 3v3a2 2 0 0 1-2 2H3M16 3v3a2 2 0 0 0 2 2h3M8 21v-3a2 2 0 0 0-2-2H3M16 21v-3a2 2 0 0 1 2-2h3" />
  {:else if name === 'zoom-in'}
    <circle cx="11" cy="11" r="7" />
    <path d="m20 20-3.4-3.4M11 8v6M8 11h6" />
  {:else if name === 'zoom-out'}
    <circle cx="11" cy="11" r="7" />
    <path d="m20 20-3.4-3.4M8 11h6" />
  {:else if name === 'volume'}
    <path d="M11 5 6 9H3v6h3l5 4V5Z" />
    <path d="M15.54 8.46a5 5 0 0 1 0 7.07M18.36 5.64a9 9 0 0 1 0 12.72" />
  {:else if name === 'volume-off'}
    <path d="M11 5 6 9H3v6h3l5 4V5Z" />
    <path d="m16 9 5 5M21 9l-5 5" />
  {:else if name === 'play'}
    <path d="M7 4.5v15l13-7.5Z" fill="currentColor" stroke="none" />
  {:else if name === 'pause'}
    <path d="M8 4v16M16 4v16" stroke-width="3" />
  {:else if name === 'image'}
    <rect x="3" y="4" width="18" height="16" rx="2" />
    <circle cx="8.5" cy="9" r="1.5" />
    <path d="m21 15-5-5L5 20" />
  {:else if name === 'refresh'}
    <path d="M20 11a8 8 0 0 0-14.9-4M4 4v6h6" />
    <path d="M4 13a8 8 0 0 0 14.9 4M20 20v-6h-6" />
  {:else if name === 'alert'}
    <path d="M12 3 2 21h20L12 3Z" />
    <path d="M12 9v5M12 18h.01" />
  {:else if name === 'tag'}
    <path d="M20.6 13.4 11 3.8H4v7l9.6 9.6a2 2 0 0 0 2.8 0l4.2-4.2a2 2 0 0 0 0-2.8Z" />
    <circle cx="7.5" cy="7.5" r="1" fill="currentColor" stroke="none" />
  {:else if name === 'info'}
    <circle cx="12" cy="12" r="9" />
    <path d="M12 11v5M12 8h.01" />
  {:else if name === 'grid'}
    <rect x="3" y="3" width="7" height="7" rx="1" />
    <rect x="14" y="3" width="7" height="7" rx="1" />
    <rect x="3" y="14" width="7" height="7" rx="1" />
    <rect x="14" y="14" width="7" height="7" rx="1" />
  {:else if name === 'check'}
    <path d="m5 12 4 4L19 6" />
  {:else if name === 'link'}
    <path d="M10 13a5 5 0 0 0 7.5.5l2-2a5 5 0 0 0-7-7L11 6" />
    <path d="M14 11a5 5 0 0 0-7.5-.5l-2 2a5 5 0 0 0 7 7L13 18" />
  {:else if name === 'eye'}
    <path d="M2 12s3.5-7 10-7 10 7 10 7-3.5 7-10 7S2 12 2 12Z" />
    <circle cx="12" cy="12" r="3" />
  {:else if name === 'eye-off'}
    <path d="M3 3l18 18" />
    <path d="M10.6 5.2A11 11 0 0 1 12 5c6.5 0 10 7 10 7a17 17 0 0 1-3.2 4.2M6.1 6.8A17 17 0 0 0 2 12s3.5 7 10 7a10.8 10.8 0 0 0 5.2-1.3" />
    <path d="M9.9 9.9a3 3 0 0 0 4.2 4.2" />
  {:else if name === 'filter'}
    <path d="M3 5h18l-7 8v6l-4 2v-8L3 5Z" />
  {/if}
</svg>
