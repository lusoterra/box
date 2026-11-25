export type Rating =
  | 'all'
  | 'general'
  | 'sensitive'
  | 'questionable'
  | 'explicit';

export interface MediaAsset {
  duration?: number | null;
  has_audio?: boolean;
  has_audio_evidence?: boolean;
  pixel_hash?: string | null;
  blurhash?: string | null;
}

/** The Danbooru-ish post shape exposed by the box share server. */
export interface DanbooruPost {
  id: number;
  created_at?: string;
  updated_at?: string;
  file_url?: string;
  large_file_url?: string;
  sample_url?: string;
  preview_file_url?: string;
  file_ext?: string;
  file_size?: number;
  image_width?: number;
  image_height?: number;
  media_asset?: MediaAsset | null;
  tag_string?: string;
  tag_string_general?: string;
  tag_string_artist?: string;
  tag_string_character?: string;
  tag_string_copyright?: string;
  tag_string_meta?: string;
  rating?: string;
  score?: number;
  source?: string;
  md5?: string;
  sha256?: string;
  has_large?: boolean;
  has_children?: boolean;
  is_video?: boolean;
  status?: string;
}

export interface TagSuggestion {
  name: string;
  category: number;
  post_count: number;
}

export interface TagGroup {
  title: string;
  bucket: 'artist' | 'character' | 'copyright' | 'general' | 'meta';
  tags: string[];
}
