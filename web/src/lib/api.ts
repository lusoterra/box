import type { DanbooruPost, TagSuggestion } from './types';

const API_KEY = 'box-share-api';
const REQUEST_TIMEOUT_MS = 20_000;

export class ApiError extends Error {
  status: number;
  body: string;

  constructor(message: string, status = 0, body = '') {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.body = body;
  }
}

export function defaultApiBase(): string {
  if (import.meta.env.DEV) return 'http://127.0.0.1:7071';
  return window.location.origin;
}

export function normalizeApiBase(value: string): string {
  let base = value.trim();
  if (!base) return defaultApiBase();
  if (!/^https?:\/\//i.test(base)) base = `http://${base}`;
  return base.replace(/\/+$/, '');
}

export function getApiBase(): string {
  try {
    const stored = localStorage.getItem(API_KEY);
    if (stored != null) return normalizeApiBase(stored);
  } catch {
    // storage can be disabled; fall through to the origin
  }
  return normalizeApiBase(defaultApiBase());
}

export function setApiBase(value: string): string {
  const normalized = normalizeApiBase(value);
  try {
    if (!value.trim()) localStorage.removeItem(API_KEY);
    else localStorage.setItem(API_KEY, normalized);
  } catch {
    // ignore quota/privacy failures
  }
  return normalized;
}

export function apiUrl(path: string): string {
  const base = getApiBase();
  return `${base}${path.startsWith('/') ? path : `/${path}`}`;
}

/** Turn a relative media path from the API into an absolute URL. */
export function absoluteMediaUrl(url: string | undefined): string {
  if (!url) return '';
  if (/^(?:https?:|data:|blob:)/i.test(url)) return url;
  const base = getApiBase();
  if (url.startsWith('//')) {
    return `${base.split('//', 1)[0]}//${url.replace(/^\/+/, '')}`;
  }
  if (url.startsWith('/')) return `${base}${url}`;
  return `${base}/${url.replace(/^\/+/, '')}`;
}

export function serverLabel(): string {
  try {
    const url = new URL(getApiBase());
    return url.host;
  } catch {
    return getApiBase();
  }
}

async function requestJson<T>(path: string): Promise<T> {
  const controller = new AbortController();
  const timeout = window.setTimeout(
    () => controller.abort(),
    REQUEST_TIMEOUT_MS,
  );
  let response: Response;
  try {
    response = await fetch(apiUrl(path), {
      method: 'GET',
      headers: { Accept: 'application/json' },
      signal: controller.signal,
      credentials: 'omit',
      cache: 'no-cache',
    });
  } catch (error) {
    if (controller.signal.aborted) {
      throw new ApiError('The server did not answer in time.');
    }
    throw new ApiError(
      error instanceof Error ? error.message : 'Could not reach the server.',
    );
  } finally {
    window.clearTimeout(timeout);
  }

  if (!response.ok) {
    let body = '';
    try {
      body = (await response.text()).slice(0, 300);
    } catch {
      // ignore
    }
    if (response.status === 404) {
      throw new ApiError('Not found on this server.', 404, body);
    }
    throw new ApiError(
      body || `Server returned HTTP ${response.status}.`,
      response.status,
      body,
    );
  }

  try {
    return (await response.json()) as T;
  } catch {
    throw new ApiError('The server returned invalid JSON.', response.status);
  }
}

function queryString(
  tags: string,
  page?: number,
  limit?: number,
): string {
  const params = new URLSearchParams();
  if (tags.trim()) params.set('tags', tags.trim());
  if (page != null) params.set('page', String(page));
  if (limit != null) params.set('limit', String(limit));
  const q = params.toString();
  return q ? `?${q}` : '';
}

export async function fetchPosts(
  tags: string,
  page: number,
  limit: number,
): Promise<DanbooruPost[]> {
  const data = await requestJson<
    DanbooruPost[] | { posts?: DanbooruPost[] }
  >(`/posts.json${queryString(tags, page, limit)}`);
  if (Array.isArray(data)) return data;
  if (data && Array.isArray(data.posts)) return data.posts;
  throw new ApiError('Unexpected posts response shape.');
}

export async function fetchPost(id: number): Promise<DanbooruPost> {
  const data = await requestJson<DanbooruPost | { post?: DanbooruPost }>(
    `/posts/${encodeURIComponent(String(id))}.json`,
  );
  if (data && typeof data === 'object' && 'id' in data) {
    return data as DanbooruPost;
  }
  if (data && 'post' in data && data.post) return data.post;
  throw new ApiError('Unexpected post response shape.');
}

interface RawTagSuggestion {
  name?: string;
  tag?: string;
  category?: number | string;
  post_count?: number | string;
  count?: number | string;
}

export async function fetchTagSuggestions(
  prefix: string,
  limit = 24,
): Promise<TagSuggestion[]> {
  if (!prefix.trim()) return [];
  const params = new URLSearchParams();
  params.set('search[name_matches]', `${prefix.trim()}*`);
  params.set('search[order]', 'count');
  params.set('limit', String(limit));
  const data = await requestJson<
    RawTagSuggestion[] | { tags?: RawTagSuggestion[] }
  >(`/tags.json?${params.toString()}`);
  const rows = Array.isArray(data) ? data : data?.tags;
  if (!Array.isArray(rows)) return [];
  const out: TagSuggestion[] = [];
  for (const row of rows) {
    const name = (row.name || row.tag || '').trim();
    if (!name) continue;
    const category = Number(row.category ?? 0) || 0;
    const postCount = Number(row.post_count ?? row.count ?? 0) || 0;
    out.push({ name, category, post_count: postCount });
  }
  return out;
}

export async function fetchPostCount(
  tags: string,
): Promise<number | null> {
  try {
    const data = await requestJson<
      | { counts?: { posts?: number }; count?: number }
      | { posts?: number }
    >(`/counts/posts.json${queryString(tags)}`);
    if (!data) return null;
    if ('counts' in data && data.counts) {
      const n = Number(data.counts.posts);
      return Number.isFinite(n) ? n : null;
    }
    if ('count' in data && data.count != null) {
      const n = Number(data.count);
      return Number.isFinite(n) ? n : null;
    }
    return null;
  } catch (error) {
    if (error instanceof ApiError && error.status === 404) return null;
    throw error;
  }
}

/** Test a typed URL before saving it. */
export async function pingServerAt(base: string): Promise<string> {
  const normalized = normalizeApiBase(base);
  const controller = new AbortController();
  const timeout = window.setTimeout(
    () => controller.abort(),
    REQUEST_TIMEOUT_MS,
  );
  try {
    const response = await fetch(`${normalized}/posts.json?limit=1`, {
      method: 'GET',
      headers: { Accept: 'application/json' },
      signal: controller.signal,
      credentials: 'omit',
      cache: 'no-cache',
    });
    if (!response.ok) {
      const body = (await response.text()).slice(0, 200);
      throw new ApiError(
        body || `Server returned HTTP ${response.status}.`,
        response.status,
        body,
      );
    }
    const data = (await response.json()) as
      | DanbooruPost[]
      | { posts?: DanbooruPost[] };
    const rows = Array.isArray(data) ? data : data?.posts;
    return `Connected (${rows?.length ? 'posts available' : 'empty library'})`;
  } catch (error) {
    if (controller.signal.aborted) {
      throw new ApiError('The server did not answer in time.');
    }
    throw error instanceof Error
      ? error
      : new ApiError('Could not reach the server.');
  } finally {
    window.clearTimeout(timeout);
  }
}
