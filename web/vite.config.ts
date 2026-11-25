import { defineConfig } from 'vite';
import { svelte } from '@sveltejs/vite-plugin-svelte';
import { viteSingleFile } from 'vite-plugin-singlefile';

// One inlined index.html: the box binary embeds exactly this file.
export default defineConfig({
  plugins: [svelte(), viteSingleFile()],
  base: './',
  build: {
    target: 'es2020',
    cssCodeSplit: false,
    assetsInlineLimit: 100000000,
    outDir: 'dist',
    emptyOutDir: true,
  },
});
