import type { Rating } from './types';

export const RATINGS: Rating[] = [
  'all',
  'general',
  'sensitive',
  'questionable',
  'explicit',
];

export function ratingLabel(rating: Rating): string {
  switch (rating) {
    case 'general':
      return 'General';
    case 'sensitive':
      return 'Sensitive';
    case 'questionable':
      return 'Questionable';
    case 'explicit':
      return 'Explicit';
    default:
      return 'All ratings';
  }
}

export function ratingColor(rating: string): string {
  switch (rating) {
    case 'sensitive':
      return '#ccaa6f';
    case 'questionable':
      return '#cc8060';
    case 'explicit':
      return '#cc5555';
    default:
      return '#6fcc6f';
  }
}

export function ratingLetter(rating: string): string {
  switch (rating) {
    case 'sensitive':
      return 'S';
    case 'questionable':
      return 'Q';
    case 'explicit':
      return 'E';
    default:
      return 'G';
  }
}

export function formatScore(score: number | null | undefined): string {
  const n = Number(score);
  if (!Number.isFinite(n)) return '0';
  return n > 0 ? `+${Math.round(n)}` : String(Math.round(n));
}

export function tagBucketColor(bucket: string): string {
  switch (bucket) {
    case 'artist':
      return '#e05561';
    case 'character':
      return '#43d17c';
    case 'copyright':
      return '#b570e0';
    case 'meta':
      return '#e09a3c';
    default:
      return '#e8e8ec';
  }
}

export function categoryColor(category: number): string {
  switch (category) {
    case 1:
      return tagBucketColor('artist');
    case 3:
      return tagBucketColor('copyright');
    case 4:
      return tagBucketColor('character');
    case 5:
      return tagBucketColor('meta');
    default:
      return tagBucketColor('general');
  }
}

export function categoryName(category: number): string {
  switch (category) {
    case 1:
      return 'artist';
    case 3:
      return 'copyright';
    case 4:
      return 'character';
    case 5:
      return 'meta';
    default:
      return 'general';
  }
}

export function tokensOf(query: string): string[] {
  return query.split(/\s+/).filter(Boolean);
}

export function isRatingToken(token: string): boolean {
  const low = token.toLowerCase();
  return low.startsWith('rating:') || low.startsWith('safety:');
}

/** Remove the rating widget's token, then append the currently selected one. */
export function withRating(query: string, rating: Rating): string {
  const kept = tokensOf(query).filter((t) => !isRatingToken(t));
  if (rating !== 'all') kept.push(`rating:${rating}`);
  return kept.join(' ');
}

function sameToken(a: string, b: string): boolean {
  return a.toLowerCase() === b.toLowerCase();
}

/** Add an include/exclude tag, replacing any existing include/exclude twin. */
export function addTag(query: string, tag: string, exclude = false): string {
  const clean = tag.trim().replace(/\s+/g, '_');
  if (!clean) return query.trim();
  const wanted = exclude ? `-${clean}` : clean;
  const kept = tokensOf(query).filter(
    (t) => !sameToken(t, clean) && !sameToken(t, `-${clean}`),
  );
  kept.push(wanted);
  return kept.join(' ');
}

export function removeTag(query: string, tag: string): string {
  return tokensOf(query)
    .filter(
      (t) => !sameToken(t, tag) && !sameToken(t, `-${tag}`),
    )
    .join(' ');
}

/**
 * The token being typed, for autocomplete. Handles a leading `-` and
 * returns an empty string when the user just typed a space.
 */
export function lastToken(text: string): string {
  if (!text || /\s$/.test(text)) return '';
  const match = text.match(/(?:^|\s)(-?[^\s]*)$/);
  if (!match) return '';
  return match[1].replace(/^-/, '');
}

/** Replace the token being typed (preserving its leading `-`) without
 *  dropping earlier tokens. */
export function replaceLastToken(text: string, replacement: string): string {
  const match = text.match(/(^|\s)(-?)([^\s]*)$/);
  if (!match || match.index == null) return replacement;
  const before = text.slice(0, match.index);
  return `${before}${match[1]}${match[2]}${replacement}`;
}

export function formatCount(n: number | null | undefined): string {
  if (n == null || !Number.isFinite(n)) return '';
  if (n >= 1_000_000) {
    return `${(n / 1_000_000).toFixed(n >= 10_000_000 ? 0 : 1)}M`;
  }
  if (n >= 1_000) {
    return `${(n / 1_000).toFixed(n >= 10_000 ? 0 : 1)}k`;
  }
  return String(n);
}

export function formatBytes(n: number | null | undefined): string {
  if (n == null || !Number.isFinite(n) || n <= 0) return '';
  if (n >= 1024 * 1024 * 1024) return `${(n / (1024 ** 3)).toFixed(2)} GiB`;
  if (n >= 1024 * 1024) return `${(n / (1024 ** 2)).toFixed(1)} MiB`;
  if (n >= 1024) return `${(n / 1024).toFixed(1)} KiB`;
  return `${n} B`;
}

export function formatDuration(seconds: number | null | undefined): string {
  if (seconds == null || !Number.isFinite(seconds) || seconds <= 0) return '';
  const total = Math.round(seconds);
  const m = Math.floor(total / 60);
  const s = total % 60;
  return `${m}:${String(s).padStart(2, '0')}`;
}

export function clampText(text: string, max = 160): string {
  const t = text.replace(/\s+/g, ' ').trim();
  return t.length > max ? `${t.slice(0, max - 1)}…` : t;
}

export function altForPost(post: { id: number; tag_string?: string }): string {
  const tags = (post.tag_string || '').split(/\s+/).filter(Boolean).slice(0, 6);
  return tags.length ? tags.join(', ') : `Post ${post.id}`;
}
