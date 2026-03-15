import { defineConfig } from "vite";
import react from "@vitejs/plugin-react";

export default defineConfig({
  plugins: [react()],
  build: {
    outDir: "dist",
    assetsDir: "assets",
  },
  define: {
    __TRANSPORT__: JSON.stringify(process.env.VITE_TRANSPORT || "auto"),
  },
});
