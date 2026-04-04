<script setup>
import Converter from './components/Converter.vue'
</script>

<template>
  <div class="surface-ground min-h-screen">
    <div class="max-w-screen-xl mx-auto p-4">
      <header class="mb-5 pb-4 border-bottom-1 surface-border">
        <div class="flex align-items-center gap-3">
          <div class="logo">C64</div>
          <div>
            <h1 class="m-0 title">png2c64</h1>
            <p class="m-0 mt-1 text-sm text-color-secondary">
              Convert images to Commodore 64 VIC-II formats with perceptual color matching
            </p>
          </div>
        </div>
      </header>
      <Converter />
    </div>
  </div>
</template>

<style scoped>
.logo {
  display: flex;
  align-items: center;
  justify-content: center;
  width: 3rem;
  height: 3rem;
  border-radius: 0.75rem;
  background: linear-gradient(135deg, #6366f1, #8b5cf6);
  color: #fff;
  font-size: 0.85rem;
  font-weight: 800;
  letter-spacing: -0.03em;
  flex-shrink: 0;
}

.title {
  font-size: 1.5rem;
  font-weight: 700;
  letter-spacing: -0.03em;
  background: linear-gradient(135deg, #e0e0e0, #a0a0a0);
  -webkit-background-clip: text;
  -webkit-text-fill-color: transparent;
  background-clip: text;
}
</style>
