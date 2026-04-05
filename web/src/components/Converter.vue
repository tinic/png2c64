<script setup>
import { ref, reactive, watch, nextTick } from 'vue'
import { useWasm } from '../composables/useWasm.js'
import { useImageUpload } from '../composables/useImageUpload.js'
import { MODES, PALETTES, DITHER_METHODS, SLIDERS, EXAMPLES, defaultOptions, isSpriteMode, spriteGridDimensions, hasPrgExport } from '../lib/options.js'

import InputNumber from 'primevue/inputnumber'

import Select from 'primevue/select'
import Slider from 'primevue/slider'
import ToggleSwitch from 'primevue/toggleswitch'
import Button from 'primevue/button'
import ProgressSpinner from 'primevue/progressspinner'
import FileUpload from 'primevue/fileupload'
import Panel from 'primevue/panel'

const { loading: wasmLoading, error: wasmError, convertRGBA, convertPNG, convertPRG } = useWasm()
const { imageBytes, imageName, imageUrl, dragOver, onDrop, onDragOver, onDragLeave, openPicker } = useImageUpload()

// Load dragon example by default once WASM is ready
watch(wasmLoading, (loading) => {
  if (!loading && !imageBytes.value) {
    loadExample(EXAMPLES.find(e => e.name === 'dragon'))
  }
})

const options = reactive(defaultOptions())
const canvasRef = ref(null)
const converting = ref(false)
const resultInfo = ref('')
const errorMsg = ref('')

// Flatten dither methods for Select component
const ditherOptions = DITHER_METHODS.flatMap(g =>
  g.items.map(d => ({ value: d.value, label: d.label, group: g.group }))
)
const groupedDitherOptions = DITHER_METHODS.map(g => ({
  label: g.group,
  items: g.items.map(d => ({ value: d.value, label: d.label }))
}))

function buildWasmOptions() {
  const opts = { ...options }
  if (isSpriteMode(opts.mode)) {
    const dims = spriteGridDimensions(opts.mode, opts.spritesX, opts.spritesY)
    opts.width = dims.width
    opts.height = dims.height
  }
  delete opts.spritesX
  delete opts.spritesY
  return opts
}

let debounceTimer = null

function doConvert() {
  if (!imageBytes.value || wasmLoading.value) return

  clearTimeout(debounceTimer)
  debounceTimer = setTimeout(async () => {
    converting.value = true
    errorMsg.value = ''

    await nextTick()
    await new Promise(r => setTimeout(r, 10))

    try {
      const result = convertRGBA(imageBytes.value, buildWasmOptions())

      if (result.error) {
        errorMsg.value = result.error
        converting.value = false
        return
      }

      const canvas = canvasRef.value
      if (!canvas) return

      canvas.width = result.width
      canvas.height = result.height
      canvas.style.width = `${result.width * 2}px`
      canvas.style.height = `${result.height * 2}px`

      const ctx = canvas.getContext('2d')
      const imgData = new ImageData(
        new Uint8ClampedArray(result.rgba),
        result.width, result.height
      )
      ctx.putImageData(imgData, 0, 0)

      resultInfo.value = `${result.width} x ${result.height}`
    } catch (e) {
      errorMsg.value = e.message
    }

    converting.value = false
  }, 150)
}

watch([imageBytes, () => ({ ...options })], doConvert, { deep: true })

function downloadPNG() {
  if (!imageBytes.value) return
  try {
    const result = convertPNG(imageBytes.value, buildWasmOptions())
    if (result.error) {
      errorMsg.value = result.error
      return
    }
    const blob = new Blob([result.png], { type: 'image/png' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '-c64.png'
    a.click()
    URL.revokeObjectURL(url)
  } catch (e) {
    errorMsg.value = e.message
  }
}

function downloadPRG() {
  if (!imageBytes.value) return
  try {
    const result = convertPRG(imageBytes.value, buildWasmOptions())
    if (result.error) {
      errorMsg.value = result.error
      return
    }
    const blob = new Blob([result.prg], { type: 'application/octet-stream' })
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    a.download = (imageName.value || 'image').replace(/\.[^.]+$/, '') + '-c64.prg'
    a.click()
    URL.revokeObjectURL(url)
  } catch (e) {
    errorMsg.value = e.message
  }
}

function resetOptions() {
  Object.assign(options, defaultOptions())
}

async function loadExample(example) {
  const resp = await fetch(`/examples/${example.file}`)
  const buf = await resp.arrayBuffer()
  imageBytes.value = new Uint8Array(buf)
  imageName.value = example.file
  // Update imageUrl for the thumbnail preview
  const blob = new Blob([buf], { type: example.file.endsWith('.jpeg') ? 'image/jpeg' : 'image/png' })
  imageUrl.value = URL.createObjectURL(blob)
}

function onFileSelect(event) {
  const file = event.files?.[0]
  if (!file) return
  file.arrayBuffer().then(buf => {
    imageBytes.value = new Uint8Array(buf)
    imageName.value = file.name
  })
}
</script>

<template>
  <div>
    <!-- Loading -->
    <div v-if="wasmLoading" class="flex align-items-center justify-content-center py-8 gap-3">
      <ProgressSpinner style="width: 2rem; height: 2rem" />
      <span class="text-color-secondary">Loading converter...</span>
    </div>
    <div v-else-if="wasmError" class="text-center py-8 text-red-400">{{ wasmError }}</div>

    <!-- Main layout -->
    <div v-else class="grid">
      <!-- Controls sidebar -->
      <div class="col-12 md:col-4 lg:col-3">
        <div class="flex flex-column gap-3">

          <!-- Upload -->
          <Panel header="Image">
            <div
              class="drop-zone border-2 border-round-lg cursor-pointer overflow-hidden"
              :class="{
                'border-primary': dragOver,
                'border-dashed p-4 text-center': !imageUrl,
                'border-transparent': imageUrl && !dragOver,
              }"
              @drop="onDrop"
              @dragover="onDragOver"
              @dragleave="onDragLeave"
              @click="openPicker"
            >
              <template v-if="imageUrl">
                <img :src="imageUrl" class="original-preview w-full border-round" />
                <div class="text-xs text-color-secondary mt-2 px-1 flex justify-content-between">
                  <span class="white-space-nowrap overflow-hidden text-overflow-ellipsis">{{ imageName }}</span>
                  <span class="white-space-nowrap ml-2">Click to change</span>
                </div>
              </template>
              <template v-else>
                <i class="pi pi-image text-4xl text-color-secondary mb-2"></i>
                <div class="font-semibold text-sm">Drop image here</div>
                <div class="text-xs text-color-secondary">or click to browse</div>
              </template>
            </div>
            <div class="mt-3">
              <label class="block text-xs text-color-secondary font-semibold mb-2">Examples</label>
              <div class="flex flex-wrap gap-1">
                <div
                  v-for="ex in EXAMPLES" :key="ex.name"
                  class="example-thumb cursor-pointer border-round overflow-hidden"
                  :class="{ 'ring-1 ring-primary': imageName === ex.file }"
                  @click="loadExample(ex)"
                  :title="ex.name"
                >
                  <img :src="`/examples/${ex.file}`" :alt="ex.name" />
                </div>
              </div>
            </div>
          </Panel>

          <!-- Mode / Palette / Dither -->
          <Panel header="Settings">
            <div class="flex flex-column gap-3">
              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="VIC-II graphics mode. Multicolor: 160x200, 4 colors/cell. Hires: 320x200, 2 colors/cell.">Mode</label>
                <div class="col-8">
                  <Select v-model="options.mode" :options="MODES" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <template v-if="isSpriteMode(options.mode)">
                <div class="grid align-items-center">
                  <label class="col-4 text-xs text-color-secondary font-semibold" title="Number of sprites horizontally in the sheet.">Sprites X</label>
                  <div class="col-8">
                    <InputNumber v-model="options.spritesX" :min="1" :max="32" showButtons class="w-full input-sm" />
                  </div>
                </div>
                <div class="grid align-items-center">
                  <label class="col-4 text-xs text-color-secondary font-semibold" title="Number of sprites vertically in the sheet.">Sprites Y</label>
                  <div class="col-8">
                    <InputNumber v-model="options.spritesY" :min="1" :max="32" showButtons class="w-full input-sm" />
                  </div>
                </div>
              </template>

              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="VIC-II color palette. Different palettes match different emulators and measurements.">Palette</label>
                <div class="col-8">
                  <Select v-model="options.palette" :options="PALETTES" optionValue="value" optionLabel="label" class="w-full" />
                </div>
              </div>

              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Dithering algorithm. Ordered methods use fixed patterns; error diffusion propagates quantization error to neighbors.">Dither</label>
                <div class="col-8">
                  <Select
                    v-model="options.dither"
                    :options="groupedDitherOptions"
                    optionValue="value"
                    optionLabel="label"
                    optionGroupLabel="label"
                    optionGroupChildren="items"
                    class="w-full"
                  />
                </div>
              </div>
            </div>
          </Panel>

          <!-- Adjustments -->
          <Panel header="Adjustments">
            <div class="flex flex-column gap-3">
              <div v-for="s in SLIDERS" :key="s.key" class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold white-space-nowrap" :title="s.tip">{{ s.label }}</label>
                <div class="col-6">
                  <Slider v-model="options[s.key]" :min="s.min" :max="s.max" :step="s.step" class="w-full" />
                </div>
                <span class="col-2 text-xs font-mono font-semibold text-right">{{ options[s.key].toFixed(2) }}</span>
              </div>

              <div class="grid align-items-center">
                <label class="col-4 text-xs text-color-secondary font-semibold" title="Alternate scan direction per row in error diffusion. Reduces directional banding.">Serpentine</label>
                <div class="col-8">
                  <ToggleSwitch v-model="options.serpentine" />
                </div>
              </div>
            </div>
          </Panel>

          <!-- Actions -->
          <div class="flex flex-column gap-2">
            <div class="flex gap-2">
              <Button label="PNG" icon="pi pi-download" class="flex-1" :disabled="!imageBytes || converting" @click="downloadPNG" />
              <Button label="PRG" icon="pi pi-download" class="flex-1" severity="info" :disabled="!imageBytes || converting || !hasPrgExport(options.mode)" @click="downloadPRG" />
            </div>
            <Button label="Reset" icon="pi pi-refresh" severity="secondary" outlined class="w-full" @click="resetOptions" />
          </div>

        </div>
      </div>

      <!-- Preview (sticky) -->
      <div class="col-12 md:col-8 lg:col-9 preview-col">
        <div v-if="!imageBytes" class="surface-card border-round-lg flex align-items-center justify-content-center" style="min-height: 500px;">
          <div class="text-center text-color-secondary">
            <i class="pi pi-upload text-5xl mb-3 block"></i>
            <div>Upload an image to get started</div>
          </div>
        </div>

        <div v-else class="flex flex-column gap-2">
          <div class="preview-container surface-card border-round-lg overflow-hidden relative">
            <canvas ref="canvasRef" class="preview-canvas" />
            <div v-if="converting" class="overlay flex align-items-center justify-content-center">
              <ProgressSpinner style="width: 2rem; height: 2rem" />
            </div>
          </div>
          <div class="flex justify-content-between px-1">
            <span class="text-xs text-color-secondary">{{ resultInfo }}</span>
            <span v-if="errorMsg" class="text-xs text-red-400">{{ errorMsg }}</span>
          </div>
        </div>
      </div>
    </div>
  </div>
</template>

<style scoped>
:deep(.p-select),
:deep(.p-select-label),
:deep(.p-select-option),
:deep(.p-select-option-group) {
  font-size: 0.75rem;
}

:deep(.input-sm .p-inputnumber-input) {
  font-size: 0.75rem;
  padding: 0.4rem 0.5rem;
}

.preview-col {
  position: sticky;
  top: 1rem;
  align-self: start;
}

.drop-zone {
  transition: border-color 0.15s, background 0.15s;
}
.drop-zone:hover {
  border-color: var(--p-primary-color) !important;
}

.original-preview {
  display: block;
  max-height: 200px;
  object-fit: contain;
}

.example-thumb {
  width: 48px;
  height: 36px;
  transition: opacity 0.15s;
}
.example-thumb:hover {
  opacity: 0.8;
}
.example-thumb img {
  width: 100%;
  height: 100%;
  object-fit: cover;
  display: block;
}

.preview-container {
  display: inline-block;
  background: #000;
  padding: 1rem;
}

.preview-canvas {
  display: block;
  image-rendering: pixelated;
  image-rendering: crisp-edges;
}

.overlay {
  position: absolute;
  inset: 0;
  background: rgba(0, 0, 0, 0.5);
}
</style>
