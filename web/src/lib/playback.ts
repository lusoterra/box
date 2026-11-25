export interface PlaybackSettings {
  muted: boolean;
  loop: boolean;
  volume: number;
}

// Mirrors box's Preferences -> Appearance -> Video playback defaults.
export const DEFAULT_PLAYBACK: PlaybackSettings = {
  muted: true,
  loop: true,
  volume: 100,
};

export function normalizePlayback(
  value: Partial<PlaybackSettings> | null,
): PlaybackSettings {
  const volume = Math.round(Number(value?.volume));
  return {
    muted:
      typeof value?.muted === 'boolean'
        ? value.muted
        : DEFAULT_PLAYBACK.muted,
    loop:
      typeof value?.loop === 'boolean' ? value.loop : DEFAULT_PLAYBACK.loop,
    volume:
      Number.isFinite(volume) && volume >= 0
        ? Math.min(100, volume)
        : DEFAULT_PLAYBACK.volume,
  };
}
