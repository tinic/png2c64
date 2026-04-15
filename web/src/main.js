import { createApp } from 'vue'
import PrimeVue from 'primevue/config'
import Aura from '@primevue/themes/aura'
import 'primeicons/primeicons.css'
import 'primeflex/primeflex.css'
import App from './App.vue'
import { prewarmWasm } from './composables/useWasm.js'

// Start the WASM worker (and therefore the .wasm fetch + streaming compile)
// in parallel with Vue/PrimeVue bootstrap so first conversion feels instant.
prewarmWasm()

const app = createApp(App)
app.use(PrimeVue, {
  theme: {
    preset: Aura,
    options: {
      darkModeSelector: '.dark-mode',
    }
  }
})
app.mount('#app')
