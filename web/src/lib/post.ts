import type { DanbooruPost, TagGroup } from './types';

const VIDEO_EXTENSIONS = new Set([
  'mp4',
  'webm',
  'm4v',
  'mov',
  'mkv',
  'ogv',
]);

export function extOfUrl(url: string | undefined): string {
  if (!url) return '';
  const clean = url.split(/[?#]/, 1)[0] ?? '';
  const slash = clean.lastIndexOf('/');
  const name = slash >= 0 ? clean.slice(slash + 1) : clean;
  const dot = name.lastIndexOf('.');
  return dot >= 0 ? name.slice(dot + 1).toLowerCase() : '';
}

export function postExt(post: DanbooruPost): string {
  return (post.file_ext || extOfUrl(post.file_url)).toLowerCase();
}

export function fileUrl(post: DanbooruPost): string {
  return post.file_url || post.large_file_url || post.sample_url || '';
}

export function sampleUrl(post: DanbooruPost): string {
  return post.large_file_url || post.sample_url || fileUrl(post);
}

export function previewUrl(post: DanbooruPost): string {
  return post.preview_file_url || sampleUrl(post) || fileUrl(post);
}

export function isVideoPost(post: DanbooruPost): boolean {
  if (post.is_video === true) return true;
  if (VIDEO_EXTENSIONS.has(postExt(post))) return true;
  return typeof post.media_asset?.duration === 'number';
}

export function isAnimatedImage(post: DanbooruPost): boolean {
  const ext = postExt(post);
  return ext === 'gif' || ext === 'webp' || ext === 'jxl';
}

export function postAspect(post: DanbooruPost, fallback = 0.75): number {
  const width = Number(post.image_width) || 0;
  const height = Number(post.image_height) || 0;
  if (width <= 0 || height <= 0) return fallback;
  return Math.min(2.4, Math.max(0.45, width / height));
}

export function normalizedTags(blob: string | undefined): string[] {
  if (!blob) return [];
  return blob
    .split(/\s+/)
    .map((t) => t.trim())
    .filter(Boolean);
}

export function tagGroups(post: DanbooruPost): TagGroup[] {
  const seen = new Set<string>();
  const make = (
    title: string,
    bucket: TagGroup['bucket'],
    blob: string | undefined,
  ): TagGroup | null => {
    const tags: string[] = [];
    for (const tag of normalizedTags(blob)) {
      if (seen.has(tag)) continue;
      seen.add(tag);
      tags.push(tag);
    }
    return tags.length ? { title, bucket, tags } : null;
  };

  const groups: Array<TagGroup | null> = [
    make('Artist', 'artist', post.tag_string_artist),
    make('Character', 'character', post.tag_string_character),
    make('Copyright', 'copyright', post.tag_string_copyright),
    make(
      'General',
      'general',
      post.tag_string_general || post.tag_string,
    ),
    make('Meta', 'meta', post.tag_string_meta),
  ];
  return groups.filter((g): g is TagGroup => g !== null);
}

export function tagDisplay(tag: string): string {
  return tag.replace(/_/g, ' ');
}

export function ratingOf(post: DanbooruPost): string {
  const r = (post.rating || '').toLowerCase();
  if (r === 'safe' || r === 'g') return 'general';
  if (r === 's') return 'sensitive';
  if (r === 'q') return 'questionable';
  if (r === 'e') return 'explicit';
  return r || 'general';
}

export function sourceHost(source: string | undefined): string {
  if (!source) return '';
  try {
    return new URL(source).host.replace(/^www\./, '');
  } catch {
    return source;
  }
}
