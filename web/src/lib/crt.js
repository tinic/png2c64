// CRT post-processing effects applied to canvas ImageData.
// Simulates PAL bandwidth, scanlines, phosphor bloom, and vignette.
// Display-only — does not affect exported files.

export function applyCRT(ctx, width, height) {
  const data = ctx.getImageData(0, 0, width, height)
  const px = data.data

  // 1. PAL bandwidth simulation
  // Luma has ~5 MHz bandwidth (sharp), chroma has ~1.3 MHz (blurry).
  // Separate into Y (luma) and UV (chroma), blur UV horizontally,
  // then recombine. This gives the characteristic PAL color bleed.
  const radius = Math.max(2, Math.round(width / 160)) // ~4px at 640w
  for (let y = 0; y < height; y++) {
    // Extract luma and chroma for this row
    const rowOff = y * width * 4
    const lumaR = new Float32Array(width)
    const chromaU = new Float32Array(width)
    const chromaV = new Float32Array(width)

    for (let x = 0; x < width; x++) {
      const i = rowOff + x * 4
      const r = px[i] / 255, g = px[i + 1] / 255, b = px[i + 2] / 255
      // RGB -> YUV (BT.601)
      lumaR[x] = 0.299 * r + 0.587 * g + 0.114 * b
      chromaU[x] = -0.147 * r - 0.289 * g + 0.436 * b
      chromaV[x] = 0.615 * r - 0.515 * g - 0.100 * b
    }

    // Box blur chroma channels horizontally
    const blurU = boxBlur(chromaU, radius)
    const blurV = boxBlur(chromaV, radius)

    // Slight luma softening (much less than chroma)
    const softY = boxBlur(lumaR, Math.max(1, Math.round(radius / 3)))

    // Recombine YUV -> RGB
    for (let x = 0; x < width; x++) {
      const i = rowOff + x * 4
      const yy = softY[x]
      const u = blurU[x]
      const v = blurV[x]
      px[i]     = clamp255((yy + 1.140 * v) * 255)
      px[i + 1] = clamp255((yy - 0.395 * u - 0.581 * v) * 255)
      px[i + 2] = clamp255((yy + 2.032 * u) * 255)
    }
  }

  // 2. Scanlines: darken every other row
  for (let y = 0; y < height; y++) {
    if (y % 2 === 1) {
      const rowOff = y * width * 4
      for (let x = 0; x < width; x++) {
        const i = rowOff + x * 4
        px[i]     = px[i] * 0.55
        px[i + 1] = px[i + 1] * 0.55
        px[i + 2] = px[i + 2] * 0.55
      }
    }
  }

  // 3. Phosphor glow: bright pixels bleed slightly
  const tmp = new Uint8ClampedArray(px)
  for (let y = 0; y < height; y++) {
    for (let x = 1; x < width - 1; x++) {
      const i = (y * width + x) * 4
      for (let c = 0; c < 3; c++) {
        const glow = Math.max(0, tmp[i - 4 + c] - 200) * 0.06 +
                     Math.max(0, tmp[i + 4 + c] - 200) * 0.06
        px[i + c] = Math.min(255, px[i + c] + glow)
      }
    }
  }

  // 4. Vignette
  const cx = width / 2, cy = height / 2
  for (let y = 0; y < height; y++) {
    for (let x = 0; x < width; x++) {
      const dx = (x - cx) / cx, dy = (y - cy) / cy
      const v = 1.0 - (dx * dx + dy * dy) * 0.25
      const i = (y * width + x) * 4
      px[i]     = px[i] * v
      px[i + 1] = px[i + 1] * v
      px[i + 2] = px[i + 2] * v
    }
  }

  ctx.putImageData(data, 0, 0)
}

function clamp255(v) {
  return v < 0 ? 0 : v > 255 ? 255 : v | 0
}

// 1D box blur on a Float32Array row
function boxBlur(arr, radius) {
  const n = arr.length
  const out = new Float32Array(n)
  const diam = radius * 2 + 1
  let sum = 0
  // Seed with first `radius` elements (mirrored)
  for (let i = -radius; i <= radius; i++) {
    sum += arr[Math.max(0, Math.min(n - 1, i))]
  }
  for (let x = 0; x < n; x++) {
    out[x] = sum / diam
    // Slide window
    const addIdx = Math.min(n - 1, x + radius + 1)
    const remIdx = Math.max(0, x - radius)
    sum += arr[addIdx] - arr[remIdx]
  }
  return out
}
