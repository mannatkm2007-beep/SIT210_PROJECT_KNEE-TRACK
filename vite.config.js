import { defineConfig, loadEnv } from "vite";
import react from "@vitejs/plugin-react";

// Dev only: forwards /api and /ws to the Pi. Set PI_URL in the .env file (see .env).
export default defineConfig(({ mode }) => {
  const env = loadEnv(mode, process.cwd(), "");
  const PI = env.PI_URL || "http://harsimran.local:5000";
  return {
    plugins: [react()],
    server: {
      proxy: {
        "/api": { target: PI, changeOrigin: true },
        "/ws": { target: PI, changeOrigin: true, ws: true },
      },
    },
  };
});
