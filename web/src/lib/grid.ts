export interface GridSettings {
  aspectNum: number;
  aspectDen: number;
  cardWidth: number;
}

export interface ThumbRatio {
  label: string;
  num: number;
  den: number;
}

// Mirrors Preferences -> Appearance -> Grid -> Thumbnail ratio in box.
export const THUMB_RATIOS: ThumbRatio[] = [
  { label: 'Square 1:1', num: 1, den: 1 },
  { label: 'Classic 5:4', num: 5, den: 4 },
  { label: 'Standard 4:3', num: 4, den: 3 },
  { label: 'Photo 3:2', num: 3, den: 2 },
  { label: 'Wide 16:9', num: 16, den: 9 },
  { label: 'Ultrawide 21:9', num: 21, den: 9 },
  { label: 'Portrait 4:5', num: 4, den: 5 },
  { label: 'Portrait 2:3', num: 2, den: 3 },
  { label: 'Portrait 3:4', num: 3, den: 4 },
  { label: 'Tall 9:16', num: 9, den: 16 },
];

export const DEFAULT_GRID: GridSettings = {
  aspectNum: 1,
  aspectDen: 1,
  cardWidth: 184,
};

export function normalizeGrid(value: Partial<GridSettings> | null): GridSettings {
  const aspectNum = Math.round(Number(value?.aspectNum));
  const aspectDen = Math.round(Number(value?.aspectDen));
  const cardWidth = Math.round(Number(value?.cardWidth));
  return {
    aspectNum:
      Number.isFinite(aspectNum) && aspectNum > 0 && aspectNum <= 100
        ? aspectNum
        : DEFAULT_GRID.aspectNum,
    aspectDen:
      Number.isFinite(aspectDen) && aspectDen > 0 && aspectDen <= 100
        ? aspectDen
        : DEFAULT_GRID.aspectDen,
    cardWidth:
      Number.isFinite(cardWidth) && cardWidth > 0
        ? Math.min(384, Math.max(96, cardWidth))
        : DEFAULT_GRID.cardWidth,
  };
}

export function ratioValue(ratio: ThumbRatio): string {
  return `${ratio.num}:${ratio.den}`;
}

export function parseRatio(value: string): { num: number; den: number } {
  const [num, den] = value.split(':').map(Number);
  return {
    num: Number.isFinite(num) && num > 0 ? num : 1,
    den: Number.isFinite(den) && den > 0 ? den : 1,
  };
}

export function ratioLabel(num: number, den: number): string {
  return THUMB_RATIOS.find((r) => r.num === num && r.den === den)?.label ??
    `Custom ${num}:${den}`;
}
