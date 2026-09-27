import type { Metadata } from "next";
import WiringMap from "@/components/WiringMap";
import "./wiring.css";

export const metadata: Metadata = {
  title: "ESP32-C3 · Wiring Map — ST7735 / WS2812 / TTP223",
  description:
    "Pin map for the ESP32-C3-Zero with the 1.8″ ST7735 SPI display and TTP223 touch pad: the exact GP numbers the sketches compile against.",
};

export default function SchemaPage() {
  return <WiringMap />;
}
