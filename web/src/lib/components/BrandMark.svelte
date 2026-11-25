<script lang="ts">
  let { size = 32 }: { size?: number } = $props();
  const uid = `box-${Math.random().toString(36).slice(2, 8)}`;
</script>

<svg
  width={size}
  height={size}
  viewBox="0 0 512 512"
  role="img"
  aria-label="Booru Explorer"
  class="mark"
>
  <defs>
    <linearGradient id={`${uid}-top`} x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#efc79c" />
      <stop offset="1" stop-color="#e2b184" />
    </linearGradient>
    <linearGradient id={`${uid}-left`} x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#dcae82" />
      <stop offset="1" stop-color="#cf9b70" />
    </linearGradient>
    <linearGradient id={`${uid}-right`} x1="0" y1="0" x2="0" y2="1">
      <stop offset="0" stop-color="#d2a076" />
      <stop offset="1" stop-color="#c28d63" />
    </linearGradient>
  </defs>
  <g transform="translate(256 256) scale(0.94) translate(-256 -272)">
    <g>
      <path d="M256 52 430 172 256 292 82 172Z" fill={`url(#${uid}-top)`} />
      <path d="M82 172 256 292 256 492 82 372Z" fill={`url(#${uid}-left)`} />
      <path d="M430 172 256 292 256 492 430 372Z" fill={`url(#${uid}-right)`} />
    </g>
    <g
      fill="none"
      stroke="#3a3a3a"
      stroke-width="22"
      stroke-linejoin="round"
      stroke-linecap="round"
    >
      <path d="M256 52 430 172 256 292 82 172Z" />
      <path d="M82 172 256 292 256 492 82 372Z" />
      <path d="M430 172 256 292 256 492 430 372Z" />
    </g>
  </g>
</svg>

<style>
  .mark {
    display: block;
    flex: 0 0 auto;
    filter: drop-shadow(0 2px 5px rgba(0, 0, 0, 0.25));
  }
</style>
