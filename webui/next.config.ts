import type { NextConfig } from "next";
import path from "node:path";

const nextConfig: NextConfig = {
  // The site is fully client-side (WebSerial flashing), so it ships as a
  // static export that wrangler uploads as Workers static assets.
  output: "export",
  devIndicators: false,
  turbopack: {
    root: path.join(__dirname),
  },
};

export default nextConfig;
