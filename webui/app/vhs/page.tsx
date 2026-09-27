import type { Metadata } from "next";
import Gallery from "@/components/Gallery";

export const metadata: Metadata = {
  title: "DM-01 — VHS firmware deck",
  description:
    "DM-01: flash the pomodoro, 3d_cube, neko, flappy and more sketches onto an ESP32-C3 + ST7735 or a CYD + ST7789 straight from the browser with WebSerial — SIGNAL/VHS theme.",
};

export default function VhsPage() {
  return (
    <main className="w-full">
      <Gallery />
    </main>
  );
}
