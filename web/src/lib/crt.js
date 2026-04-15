// CRT post-processing effects applied to canvas ImageData.
// Simulates PAL composite signal (chroma low-pass + line averaging),
// scanlines, phosphor bloom, and vignette. Display-only — does not
// affect exported files.

export function applyCRT(ctx, width, height, pixelScale = 4) {
  const data = ctx.getImageData(0, 0, width, height)
  const px = data.data

  // 1. PAL bandwidth simulation.
  //
  // PAL: luma ~5 MHz (sharp), chroma U/V ~1.3 MHz (blurry). On top of
  // the bandwidth limit, a real PAL decoder uses a 1-H delay-line to
  // average chroma between consecutive scanlines (the property the
  // standard's name refers to). The vertical chroma averaging is what
  // gives PAL its characteristic look — colour fringes spread both
  // horizontally and vertically while luma stays crisp.
  //
  // Pipeline:
  //   per row: RGB -> YUV, horizontal chroma blur, slight luma softening
  //   global:  vertical 2-tap average on chroma (the PAL delay-line)
  //   per row: YUV -> RGB
  const hRadius = Math.max(2, Math.round(width / 160)) // ~4px at 640w
  const yRadius = Math.max(1, Math.round(hRadius / 3))
  const lumaY  = new Float32Array(width * height)
  const chromU = new Float32Array(width * height)
  const chromV = new Float32Array(width * height)

  // Per-row: split + horizontal low-pass.
  for (let y = 0; y < height; y++) {
    const rowOff = y * width * 4
    const yRow = new Float32Array(width)
    const uRow = new Float32Array(width)
    const vRow = new Float32Array(width)
    for (let x = 0; x < width; x++) {
      const i = rowOff + x * 4
      const r = px[i] / 255, g = px[i + 1] / 255, b = px[i + 2] / 255
      yRow[x] =  0.299 * r + 0.587 * g + 0.114 * b
      uRow[x] = -0.147 * r - 0.289 * g + 0.436 * b
      vRow[x] =  0.615 * r - 0.515 * g - 0.100 * b
    }
    const ySoft = boxBlur(yRow, yRadius)
    const uBlur = boxBlur(uRow, hRadius)
    const vBlur = boxBlur(vRow, hRadius)
    const off = y * width
    for (let x = 0; x < width; x++) {
      lumaY[off + x]  = ySoft[x]
      chromU[off + x] = uBlur[x]
      chromV[off + x] = vBlur[x]
    }
  }

  // PAL delay-line: average chroma with the next scanline. Every
  // logical scanline pair shares the same averaged chroma — this is
  // the property real PAL receivers use to cancel V-phase errors,
  // and the visible side-effect is vertical chroma blur.
  const uAvg = new Float32Array(width * height)
  const vAvg = new Float32Array(width * height)
  for (let y = 0; y < height; y++) {
    const ny = (y + 1 < height) ? y + 1 : y
    const off  = y * width
    const noff = ny * width
    for (let x = 0; x < width; x++) {
      uAvg[off + x] = 0.5 * (chromU[off + x] + chromU[noff + x])
      vAvg[off + x] = 0.5 * (chromV[off + x] + chromV[noff + x])
    }
  }

  // Recombine YUV -> RGB.
  for (let y = 0; y < height; y++) {
    const rowOff = y * width * 4
    const off = y * width
    for (let x = 0; x < width; x++) {
      const i = rowOff + x * 4
      const yy = lumaY[off + x]
      const u  = uAvg[off + x]
      const v  = vAvg[off + x]
      px[i]     = clamp255((yy + 1.140 * v) * 255)
      px[i + 1] = clamp255((yy - 0.395 * u - 0.581 * v) * 255)
      px[i + 2] = clamp255((yy + 2.032 * u) * 255)
    }
  }

  // 2. RGB phosphor mask (aperture-grille style).
  //
  // Each source column spans `pixelScale` canvas columns; modulate
  // R/G/B emission across those columns as three cosines 120° apart
  // so a white source pixel shows a visible triplet — red-tinted on
  // the left, green in the middle, blue on the right. Sum of the
  // three cosines is zero so average brightness is preserved.
  const maskDepth = 0.22
  const maskR = new Float32Array(pixelScale)
  const maskG = new Float32Array(pixelScale)
  const maskB = new Float32Array(pixelScale)
  for (let p = 0; p < pixelScale; p++) {
    const phase = (2 * Math.PI * p) / pixelScale
    maskR[p] = 1 + maskDepth * Math.cos(phase)
    maskG[p] = 1 + maskDepth * Math.cos(phase - (2 * Math.PI) / 3)
    maskB[p] = 1 + maskDepth * Math.cos(phase - (4 * Math.PI) / 3)
  }
  for (let y = 0; y < height; y++) {
    const rowOff = y * width * 4
    for (let x = 0; x < width; x++) {
      const p = x % pixelScale
      const i = rowOff + x * 4
      px[i]     = clamp255(px[i]     * maskR[p])
      px[i + 1] = clamp255(px[i + 1] * maskG[p])
      px[i + 2] = clamp255(px[i + 2] * maskB[p])
    }
  }

  // 3. Scanlines aligned to source pixel rows.
  //
  // Each source row maps to `pixelScale` canvas rows. Real CRT beams
  // are bright in the centre of a scanline and fade at the top/bottom
  // edges. Use a sin² profile across each source row so the canvas
  // rows nearest a source-row boundary are dim and the rows in the
  // middle are at full brightness — gives a clean horizontal-line
  // pattern without the screen-door noise of every-other-canvas-row.
  // depth = how dark the dimmest row gets (0 = no scanlines, 1 = full).
  const depth = 0.45
  const factors = new Float32Array(pixelScale)
  for (let i = 0; i < pixelScale; i++) {
    const t = (i + 0.5) / pixelScale       // [0,1] within source row
    const beam = Math.sin(t * Math.PI)     // 0 → 1 → 0
    factors[i] = 1.0 - depth + depth * beam * beam
  }
  for (let y = 0; y < height; y++) {
    const f = factors[y % pixelScale]
    if (f >= 0.999) continue
    const rowOff = y * width * 4
    for (let x = 0; x < width; x++) {
      const i = rowOff + x * 4
      px[i]     = px[i] * f
      px[i + 1] = px[i + 1] * f
      px[i + 2] = px[i + 2] * f
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
