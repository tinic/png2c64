// Web Worker: runs png2c64 WASM off the main thread so the UI thread is
// free to render the busy spinner during conversion.
// Messages: { id, fn, args } -> { id, result } | { id, error }

let Module = null
let ready = false
let initError = null

async function init() {
  try {
    const { default: createPng2C64 } = await import('@wasm/png2c64.js')
    Module = await createPng2C64({
      locateFile: (path) => {
        if (path.endsWith('.wasm')) {
          return new URL('../../../build-wasm/png2c64.wasm', import.meta.url).href
        }
        return path
      },
    })
    ready = true
    self.postMessage({ type: 'ready' })
  } catch (e) {
    initError = e.message || String(e)
    self.postMessage({ type: 'error', error: initError })
  }
}

const initPromise = init()

self.onmessage = async (e) => {
  const { id, fn, args } = e.data
  try {
    await initPromise
    if (!Module) throw new Error(initError || 'WASM module not loaded')

    let result
    switch (fn) {
      case 'convertRGBA':
        result = Module.convertRGBA(args[0], args[1])
        break
      case 'convertErrorMapRGBA':
        result = Module.convertErrorMapRGBA(args[0], args[1])
        break
      case 'convert':
        result = Module.convert(args[0], args[1])
        break
      case 'convertPRG':
        result = Module.convertPRG(args[0], args[1])
        break
      case 'convertHeader':
        result = Module.convertHeader(args[0], args[1], args[2])
        break
      case 'convertRaw':
        result = Module.convertRaw(args[0], args[1], args[2])
        break
      default:
        throw new Error(`Unknown function: ${fn}`)
    }

    // Build plain reply with transferable buffers for the bulk arrays.
    const reply = {
      width: result.width,
      height: result.height,
      error: result.error,
    }
    const transfers = []

    for (const key of ['rgba', 'png', 'prg', 'data']) {
      if (result[key]) {
        const arr = new Uint8Array(result[key])
        const buf = arr.buffer.slice(arr.byteOffset, arr.byteOffset + arr.byteLength)
        reply[key] = buf
        transfers.push(buf)
      }
    }
    if (result.header) reply.header = result.header

    self.postMessage({ id, result: reply }, transfers)
  } catch (err) {
    self.postMessage({ id, error: err.message || String(err) })
  }
}
