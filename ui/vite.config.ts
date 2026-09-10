import { defineConfig } from 'vite'
import vue from '@vitejs/plugin-vue'

export default defineConfig({
  plugins: [vue()],

  // Assets are served from the root of the app:// scheme, so absolute paths
  // are correct here. (Under file:// they would not be, which is one of the
  // reasons the app uses a custom scheme instead.)
  base: '/',

  build: {
    outDir: 'dist',
    emptyOutDir: true,
    // The whole UI is a handful of controls; one bundle loads faster than
    // several and keeps the scheme handler trivial.
    cssCodeSplit: false,
    rollupOptions: {
      output: {
        manualChunks: undefined,
        entryFileNames: 'assets/[name].js',
        chunkFileNames: 'assets/[name].js',
        assetFileNames: 'assets/[name].[ext]',
      },
    },
    // No point shipping maps inside an installer; flip to true when debugging.
    sourcemap: false,
  },

  server: {
    port: 5173,
    strictPort: true,
  },
})
