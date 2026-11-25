import type { Rating } from './types';
import { RATINGS } from './query';

export type Route =
  | { kind: 'home'; tags: string; rating: Rating }
  | { kind: 'post'; id: number };

export function parseRating(value: string | null | undefined): Rating {
  const low = (value || '').toLowerCase();
  return (RATINGS as string[]).includes(low) ? (low as Rating) : 'all';
}

export function parseLocation(hash = window.location.hash): Route {
  const raw = hash.replace(/^#/, '') || '/';
  const qIndex = raw.indexOf('?');
  const path = qIndex >= 0 ? raw.slice(0, qIndex) : raw;
  const search = new URLSearchParams(qIndex >= 0 ? raw.slice(qIndex + 1) : '');

  const match = path.match(/^\/post\/(\d+)\/?$/);
  if (match) {
    const id = Number(match[1]);
    if (Number.isSafeInteger(id) && id > 0) return { kind: 'post', id };
  }
  return {
    kind: 'home',
    tags: search.get('tags') || '',
    rating: parseRating(search.get('rating')),
  };
}

export function homeHash(tags: string, rating: Rating): string {
  const search = new URLSearchParams();
  if (tags.trim()) search.set('tags', tags.trim());
  if (rating !== 'all') search.set('rating', rating);
  const query = search.toString();
  return `#/${query ? `?${query}` : ''}`;
}

export function postHash(id: number): string {
  return `#/post/${id}`;
}

export function writeHash(hash: string, push: boolean): void {
  if (window.location.hash === hash) return;
  if (push) {
    window.history.pushState({ box: true }, '', hash);
  } else {
    window.history.replaceState({ box: true }, '', hash);
  }
}

export function goHome(tags: string, rating: Rating, push = true): void {
  writeHash(homeHash(tags, rating), push);
}
