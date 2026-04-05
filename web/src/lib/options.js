export const MODES = [
  { value: 'multicolor', label: 'Multicolor (160x200)' },
  { value: 'hires', label: 'Hi-Res (320x200)' },
  { value: 'fli', label: 'FLI Multicolor (160x200)' },
  { value: 'afli', label: 'AFLI Hi-Res (320x200)' },
  { value: 'petscii', label: 'PETSCII (40x25 text)' },
  { value: 'charset-mc', label: 'Charset Multicolor' },
  { value: 'charset-hi', label: 'Charset Hi-Res' },
  { value: 'sprite-mc', label: 'Sprite Multicolor' },
  { value: 'sprite-hi', label: 'Sprite Hi-Res' },
]

export const PALETTES = [
  { value: 'pepto', label: 'Pepto' },
  { value: 'vice', label: 'VICE' },
  { value: 'colodore', label: 'Colodore' },
  { value: 'deekay', label: 'Deekay' },
  { value: 'godot', label: 'Godot' },
  { value: 'c64wiki', label: 'C64 Wiki' },
  { value: 'levy', label: 'Levy' },
]

export const DITHER_METHODS = [
  { group: 'None', items: [{ value: 'none', label: 'None' }] },
  { group: 'Ordered (square)', items: [
    { value: 'bayer4', label: 'Bayer 4x4' },
    { value: 'bayer8', label: 'Bayer 8x8' },
  ]},
  { group: 'Ordered (2:1)', items: [
    { value: 'checker', label: 'Checker' },
    { value: 'bayer2x2', label: 'Bayer 2x2' },
    { value: 'h2x4', label: 'H 2x4' },
    { value: 'clustered', label: 'Clustered Dot' },
  ]},
  { group: 'Lines', items: [
    { value: 'line2', label: 'Line 2' },
    { value: 'line-checker', label: 'Line Checker' },
    { value: 'line4', label: 'Line 4' },
    { value: 'line8', label: 'Line 8' },
  ]},
  { group: 'Error Diffusion', items: [
    { value: 'fs', label: 'Floyd-Steinberg' },
    { value: 'atkinson', label: 'Atkinson' },
    { value: 'sierra', label: 'Sierra Lite' },
  ]},
  { group: 'Error Diffusion (2:1)', items: [
    { value: 'fs-wide', label: 'F-S Wide' },
    { value: 'jarvis', label: 'Jarvis' },
  ]},
  { group: 'Error Diffusion (line)', items: [
    { value: 'line-fs', label: 'Line F-S' },
  ]},
]

export const SLIDERS = [
  { key: 'gamma',          label: 'Gamma',       min: 0.1, max: 5.0, step: 0.05, default: 1.5,
    tip: 'Power curve applied before color matching. >1 darkens midtones, <1 brightens them.' },
  { key: 'ditherStrength', label: 'Strength',    min: 0,   max: 3.0, step: 0.05, default: 1.0,
    tip: 'Dithering intensity. 0 = no dithering effect, 1 = standard, >1 = exaggerated.' },
  { key: 'brightness',     label: 'Brightness',  min: -1,  max: 1.0, step: 0.05, default: 0.0,
    tip: 'Additive lightness shift in perceptual OKLab space.' },
  { key: 'contrast',       label: 'Contrast',    min: 0,   max: 3.0, step: 0.05, default: 1.0,
    tip: 'Scale around perceptual mid-grey. 1.0 = no change, >1 increases contrast.' },
  { key: 'saturation',     label: 'Saturation',  min: 0,   max: 3.0, step: 0.05, default: 1.0,
    tip: 'Chroma scaling in OKLab space. 0 = greyscale, 1 = original, >1 = boosted color.' },
  { key: 'hueShift',       label: 'Hue',         min: -180, max: 180, step: 1.0,  default: 0.0,
    tip: 'Rotate all colors in OKLab. Shifts hues to better match the C64 palette.' },
  { key: 'sharpen',        label: 'Sharpen',     min: -1,  max: 2.0, step: 0.05, default: 0.0,
    tip: 'Negative = blur (reduces noise), positive = sharpen (enhances edges).' },
  { key: 'blackPoint',     label: 'Black Pt',    min: 0,   max: 0.4, step: 0.01, default: 0.0,
    tip: 'Clip the darkest fraction of the image. Deepens blacks.' },
  { key: 'whitePoint',     label: 'White Pt',    min: 0,   max: 0.4, step: 0.01, default: 0.0,
    tip: 'Clip the brightest fraction of the image. Cleans up highlights.' },
]

// These only apply to error diffusion dither methods
export const DIFFUSION_SLIDERS = [
  { key: 'errorClamp',     label: 'Error Clamp', min: 0,   max: 2.0, step: 0.05, default: 0.8,
    tip: 'Max error accumulation per channel. Lower = fewer stray pixels, higher = more detail.' },
  { key: 'adaptive',       label: 'Adaptive',    min: 0,   max: 1.0, step: 0.05, default: 0.0,
    tip: 'Contrast-adaptive error diffusion. Reduces dithering in detailed areas, keeps it in flat areas.' },
]

export const EXAMPLES = [
  { name: 'alien', file: 'alien.png', opts: { gamma: 0.8, ditherStrength: 0.8 } },
  { name: 'dog', file: 'dog.png', opts: { gamma: 3.0, ditherStrength: 1.5 } },
  { name: 'dragon', file: 'dragon.png', opts: { gamma: 1.0, ditherStrength: 1.5, matchRange: true } },
  { name: 'face', file: 'face.png', opts: { gamma: 3.0, ditherStrength: 1.0 } },
  { name: 'fantasy', file: 'fantasy.png', opts: { gamma: 1.0, ditherStrength: 1.5, matchRange: true } },
  { name: 'game', file: 'game.png', opts: { gamma: 2.0, ditherStrength: 1.5 } },
  { name: 'golden3', file: 'golden3.jpeg', opts: { gamma: 2.0, ditherStrength: 1.5, matchRange: true } },
  { name: 'head', file: 'head.png', opts: { mode: 'petscii', gamma: 3.3, ditherStrength: 1.6, blackPoint: 0.09, whitePoint: 0.06 } },
  { name: 'monster', file: 'monster.jpeg', opts: { gamma: 3.0, ditherStrength: 1.5 } },
  { name: 'ship', file: 'ship.jpeg', opts: { gamma: 2.0, ditherStrength: 1.5, matchRange: true } },
]

export function defaultOptions() {
  const opts = {
    mode: 'multicolor',
    palette: 'colodore',
    dither: 'checker',
    serpentine: true,
    matchRange: false,
    width: 0,
    height: 0,
    spritesX: 1,
    spritesY: 1,
  }
  for (const s of [...SLIDERS, ...DIFFUSION_SLIDERS]) opts[s.key] = s.default
  return opts
}

export function isSpriteMode(mode) {
  return mode === 'sprite-hi' || mode === 'sprite-mc'
}

export function hasPrgExport(mode) {
  return mode === 'multicolor' || mode === 'hires' || mode === 'fli' || mode === 'afli' || mode === 'petscii'
}

export function isCharsetMode(mode) {
  return mode === 'charset-hi' || mode === 'charset-mc'
}

const ERROR_DIFFUSION = new Set(['fs', 'atkinson', 'sierra', 'fs-wide', 'jarvis', 'line-fs'])

export function isErrorDiffusion(dither) {
  return ERROR_DIFFUSION.has(dither)
}

// Compute width/height from sprite grid for the WASM API
export function spriteGridDimensions(mode, sx, sy) {
  if (mode === 'sprite-hi')  return { width: sx * 24, height: sy * 21 }
  if (mode === 'sprite-mc')  return { width: sx * 12, height: sy * 21 }
  return { width: 0, height: 0 }
}
