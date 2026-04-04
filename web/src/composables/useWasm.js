import { ref, shallowRef } from 'vue'

export function useWasm() {
  const module = shallowRef(null)
  const loading = ref(true)
  const error = ref('')

  async function init() {
    try {
      const { default: createPng2C64 } = await import('@wasm/png2c64.js')
      module.value = await createPng2C64({
        locateFile: (path) => {
          if (path.endsWith('.wasm')) {
            return new URL('../../../build-wasm/png2c64.wasm', import.meta.url).href
          }
          return path
        }
      })
      loading.value = false
    } catch (e) {
      error.value = `Failed to load WASM: ${e.message}`
      loading.value = false
    }
  }

  function convertRGBA(imageBytes, options) {
    if (!module.value) throw new Error('WASM not loaded')
    return module.value.convertRGBA(imageBytes, options)
  }

  function convertPNG(imageBytes, options) {
    if (!module.value) throw new Error('WASM not loaded')
    return module.value.convert(imageBytes, options)
  }

  init()

  return { module, loading, error, convertRGBA, convertPNG }
}
