import { ref } from 'vue'

export function useImageUpload() {
  const imageBytes = ref(null)
  const imageName = ref('')
  const dragOver = ref(false)

  async function readFile(file) {
    const buf = await file.arrayBuffer()
    return new Uint8Array(buf)
  }

  async function handleFiles(files) {
    if (!files.length) return
    const file = files[0]
    if (!file.type.startsWith('image/')) return
    imageBytes.value = await readFile(file)
    imageName.value = file.name
  }

  function onDrop(e) {
    e.preventDefault()
    dragOver.value = false
    handleFiles(e.dataTransfer.files)
  }

  function onDragOver(e) {
    e.preventDefault()
    dragOver.value = true
  }

  function onDragLeave() {
    dragOver.value = false
  }

  function openPicker() {
    const input = document.createElement('input')
    input.type = 'file'
    input.accept = 'image/*'
    input.onchange = () => handleFiles(input.files)
    input.click()
  }

  return { imageBytes, imageName, dragOver, onDrop, onDragOver, onDragLeave, openPicker }
}
