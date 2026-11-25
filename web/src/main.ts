import { mount } from 'svelte';
import './app.css';
import App from './App.svelte';
import { consumeApiOverride } from './lib/api';

// A testing/deep-link override: /?api=https://danbooru.donmai.us
// It is persisted in a cookie + localStorage before the app reads it.
consumeApiOverride();

const target = document.getElementById('app');
if (!target) throw new Error('Missing #app element');

const app = mount(App, { target });
export default app;
