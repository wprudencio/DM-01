import type { Metadata } from "next";
import { Geist_Mono, Silkscreen } from "next/font/google";
import "./globals.css";

const geistMono = Geist_Mono({
  subsets: ["latin"],
  display: "swap",
  variable: "--font-geist-mono",
});

const silkscreen = Silkscreen({
  subsets: ["latin"],
  weight: ["400", "700"],
  display: "swap",
  variable: "--font-silkscreen",
});

export const metadata: Metadata = {
  title: "DM-01 — Web Flasher",
  description:
    "DM-01: flash the pomodoro, 3d_cube, neko and flappy sketches onto an ESP32-C3 + ST7735 or a CYD + ST7789 straight from the browser with WebSerial.",
};

export default function RootLayout({ children }: LayoutProps<"/">) {
  return (
    <html
      lang="en"
      className={`${geistMono.variable} ${silkscreen.variable} h-full antialiased`}
    >
      <body className="min-h-full">{children}</body>
    </html>
  );
}
