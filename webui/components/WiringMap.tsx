"use client";

import Link from "next/link";
import { useEffect, useState, type MouseEvent } from "react";

type Group = "power" | "spi" | "ctrl" | "touch";

type Wire = {
  from: string;
  to: [string, string];
  group: Group;
  color: string;
  code: string;
  note: string;
};

type Photo = {
  src: string;
  nat: { w: number; h: number };
  crop?: { x: number; y: number; w: number; h: number };
  box: { x: number; y: number; w: number };
};

type Lightbox = { src: string; alt: string; caption: string };

const BOARD = {
  name: "ESP32-C3-ZERO",
  note: "USB-C · 3V3 LDO",
  pins: ["5V", "GND", "3V3", "GP0", "GP1", "GP2", "GP3", "GP4", "GP5", "GP10"],
};

const MODULES = [
  {
    id: "tft",
    name: 'ST7735 · 1.8" TFT',
    note: "SPI 160×128",
    pins: ["VCC", "GND", "LED", "CS", "RESET", "A0/DC", "SDA", "SCK"],
  },
  { id: "touch", name: "TTP223 · TOUCH", note: "capacitive pad", pins: ["VCC", "GND", "SIG"] },
];

const GROUPS: Record<Group, { label: string; color: string }> = {
  power: { label: "Power", color: "var(--w-power)" },
  spi: { label: "SPI", color: "var(--w-spi)" },
  ctrl: { label: "Control", color: "var(--w-ctrl)" },
  touch: { label: "Touch", color: "var(--w-touch)" },
};

const WIRES: Wire[] = [
  { from: "3V3", to: ["tft", "VCC"], group: "power", color: "#e60000", code: "—", note: "3V3 rail" },
  { from: "GND", to: ["tft", "GND"], group: "power", color: "#232323", code: "—", note: "common ground" },
  { from: "3V3", to: ["tft", "LED"], group: "power", color: "#ff6a00", code: "—", note: "backlight, always on" },
  { from: "3V3", to: ["touch", "VCC"], group: "power", color: "#7a4a00", code: "—", note: "2–5.5 V" },
  { from: "GND", to: ["touch", "GND"], group: "power", color: "#5a5a5a", code: "—", note: "common ground" },
  { from: "GP1", to: ["tft", "SCK"], group: "spi", color: "#00a651", code: "TFT_SCLK", note: "SPI clock" },
  { from: "GP2", to: ["tft", "SDA"], group: "spi", color: "#0066ff", code: "TFT_MOSI", note: "SPI data out" },
  { from: "GP3", to: ["tft", "A0/DC"], group: "ctrl", color: "#7a00ff", code: "TFT_DC", note: "data / command" },
  { from: "GP4", to: ["tft", "RESET"], group: "ctrl", color: "#f00090", code: "TFT_RST", note: "display reset" },
  { from: "GP5", to: ["tft", "CS"], group: "ctrl", color: "#0090a8", code: "TFT_CS", note: "chip select" },
  { from: "GP0", to: ["touch", "SIG"], group: "touch", color: "#65a30d", code: "TOUCH_PIN", note: "tap / hold input" },
];

const IMG: Record<"board" | "tft" | "touch", Photo> = {
  board: { src: "/schema/board.jpeg", nat: { w: 447, h: 447 }, box: { x: 700, y: 240, w: 320 } },
  tft: {
    src: "/schema/tft.jpeg",
    nat: { w: 554, h: 554 },
    crop: { x: 300, y: 300, w: 254, h: 250 },
    box: { x: 60, y: 90, w: 330 },
  },
  touch: {
    src: "/schema/touch.jpg",
    nat: { w: 1250, h: 1250 },
    box: { x: 60, y: 470, w: 300 },
  },
};

function photoScale(m: Photo) {
  const c = m.crop ?? { x: 0, y: 0, w: m.nat.w, h: m.nat.h };
  return m.box.w / c.w;
}

function map(key: keyof typeof IMG, nx: number, ny: number): [number, number] {
  const m = IMG[key];
  const c = m.crop ?? { x: 0, y: 0, w: m.nat.w, h: m.nat.h };
  const scale = photoScale(m);
  return [m.box.x + (nx - c.x) * scale, m.box.y + (ny - c.y) * scale];
}

const boardPad = (side: "L" | "R", i: number) =>
  map("board", side === "L" ? 138 : 316, 133 + i * 25);

const PINS: Record<string, [number, number]> = {
  "5V": boardPad("L", 0),
  GND: boardPad("L", 1),
  "3V3": boardPad("L", 2),
  GP0: boardPad("L", 3),
  GP1: boardPad("L", 4),
  GP2: boardPad("L", 5),
  GP3: boardPad("L", 6),
  GP4: boardPad("L", 7),
  GP5: boardPad("L", 8),
  GP10: boardPad("R", 4),
};

["VCC", "GND", "CS", "RESET", "A0/DC", "SDA", "SCK", "LED"].forEach((p, i) => {
  PINS["tft:" + p] = map("tft", 452, 346 + i * 16.5);
});

["VCC", "GND", "SIG"].forEach((p, i) => {
  PINS["touch:" + p] = map("touch", [458, 590, 722][i], 311);
});

const WIRE_PATHS = WIRES.map((w) => {
  const [x1, y1] = PINS[w.from];
  const [x2, y2] = PINS[w.to[0] + ":" + w.to[1]];
  const dx = (x2 - x1) * 0.42;
  return {
    d: `M ${x1} ${y1} C ${x1 + dx} ${y1}, ${x2 - dx} ${y2}, ${x2} ${y2}`,
    ends: [
      [x1, y1],
      [x2, y2],
    ] as [number, number][],
  };
});

const FILTERS = [
  { id: "all", label: "All wires" },
  ...Object.entries(GROUPS).map(([id, g]) => ({ id, label: g.label })),
];

function PhotoLayer({ photo }: { photo: Photo }) {
  const c = photo.crop ?? { x: 0, y: 0, w: photo.nat.w, h: photo.nat.h };
  const h = c.h * photoScale(photo);
  return (
    <>
      {photo.crop ? (
        <svg
          x={photo.box.x}
          y={photo.box.y}
          width={photo.box.w}
          height={h}
          viewBox={`${c.x} ${c.y} ${c.w} ${c.h}`}
        >
          <image href={photo.src} x={0} y={0} width={photo.nat.w} height={photo.nat.h} />
        </svg>
      ) : (
        <image href={photo.src} x={photo.box.x} y={photo.box.y} width={photo.box.w} height={h} />
      )}
      <rect className="frame" x={photo.box.x} y={photo.box.y} width={photo.box.w} height={h} />
    </>
  );
}

export default function WiringMap() {
  const [active, setActive] = useState("all");
  const [selected, setSelected] = useState<number | null>(null);
  const [lightbox, setLightbox] = useState<Lightbox | null>(null);

  useEffect(() => {
    const onKey = (e: KeyboardEvent) => {
      if (e.key === "Escape") setLightbox(null);
    };
    window.addEventListener("keydown", onKey);
    return () => window.removeEventListener("keydown", onKey);
  }, []);

  const toggle = (i: number) => setSelected((prev) => (prev === i ? null : i));

  const marks = (i: number, group: Group) => {
    const parts: string[] = [];
    if (selected === i) parts.push("sel");
    else if (selected !== null) parts.push("dim");
    if (active !== "all" && group !== active) parts.push("off");
    return parts.join(" ");
  };

  const zoom = (e: MouseEvent<HTMLImageElement>) => {
    const img = e.currentTarget;
    const caption = img.closest("figure")?.querySelector("figcaption")?.textContent ?? "";
    setLightbox({ src: img.src, alt: img.alt, caption });
  };

  const stop = (e: MouseEvent) => e.stopPropagation();

  return (
    <div className="wiring-site" onClick={() => setSelected(null)}>
      <header className="bar">
        <div className="brand">
          <span className="squares" aria-hidden="true">
            <i />
            <i />
            <i />
            <i />
          </span>
          <Link className="home" href="/" title="DM-01 — home">
            DM-01
          </Link>
          <span className="pip" aria-hidden="true" />
        </div>
        <span className="tag">ST7735 160×128 · TTP223</span>
      </header>

      <div className="wrap">
        <section className="hero">
          <p className="kicker">Signal · Hardware</p>
          <h1>WIRING MAP</h1>
          <p className="lede">
            ESP32-C3-Zero (its WS2812 RGB LED is on the board) with the 1.8″ ST7735 SPI display
            and a TTP223 capacitive touch pad. This is the exact pin map the sketches compile
            against — <code>TFT_SCLK 1</code>, <code>TFT_MOSI 2</code>, <code>TFT_DC 3</code>,{" "}
            <code>TFT_RST 4</code>, <code>TFT_CS 5</code>, <code>TOUCH_PIN 0</code>,{" "}
            <code>LED_PIN 10</code>.
          </p>
        </section>

        <div className="rule" />

        <section>
          <h2>
            <span className="n">01</span> · Connections
          </h2>
          <p className="sub">
            The wires land on the pins in the photos. Click a wire — or a table row — to isolate
            it; filter by group.
          </p>

          <div className="filters">
            {FILTERS.map((f) => (
              <button
                key={f.id}
                type="button"
                className={active === f.id ? "on" : ""}
                onClick={() => setActive(f.id)}
              >
                {f.label}
              </button>
            ))}
          </div>

          <div className="board-frame" id="diagram">
            <svg
              id="wires"
              viewBox="0 0 1240 810"
              role="img"
              aria-label="Wiring diagram: ESP32-C3-Zero to the display and touch pad"
            >
              {(Object.keys(IMG) as (keyof typeof IMG)[]).map((key) => (
                <PhotoLayer key={key} photo={IMG[key]} />
              ))}

              {WIRES.map((w, i) => (
                <g
                  key={i}
                  onClick={(e) => {
                    stop(e);
                    toggle(i);
                  }}
                >
                  <path
                    className={`wire-bed ${marks(i, w.group)}`}
                    d={WIRE_PATHS[i].d}
                    data-i={i}
                    data-group={w.group}
                  />
                  <path
                    className={`wire ${marks(i, w.group)}`}
                    d={WIRE_PATHS[i].d}
                    stroke={w.color}
                    style={{ color: w.color }}
                    data-i={i}
                    data-group={w.group}
                  />
                  {WIRE_PATHS[i].ends.map(([x, y], j) => (
                    <g key={j}>
                      <circle
                        className={marks(i, w.group)}
                        cx={x}
                        cy={y}
                        r={5.2}
                        fill="#f4f6ec"
                        data-i={i}
                        data-group={w.group}
                      />
                      <circle
                        className={`wire-dot ${marks(i, w.group)}`}
                        cx={x}
                        cy={y}
                        r={3.6}
                        fill={w.color}
                        data-i={i}
                        data-group={w.group}
                      />
                    </g>
                  ))}
                </g>
              ))}

              {BOARD.pins.slice(0, 9).map((p, i) => {
                const [x, y] = boardPad("L", i);
                return (
                  <text key={p} x={x - 8} y={y + 4} textAnchor="end" className="dark">
                    {p}
                  </text>
                );
              })}
              <text
                x={boardPad("R", 4)[0]}
                y={boardPad("R", 4)[1] + 4}
                textAnchor="start"
                className="dark"
              >
                GP10
              </text>
              <text
                x={PINS["tft:VCC"][0]}
                y={PINS["tft:VCC"][1] + 4}
                textAnchor="start"
                className="light"
              >
                VCC
              </text>
              {["touch:VCC", "touch:GND", "touch:SIG"].map((pin, i) => (
                <text
                  key={pin}
                  x={PINS[pin][0]}
                  y={PINS[pin][1] + 4}
                  textAnchor="end"
                  className="light"
                >
                  {i + 1}
                </text>
              ))}
            </svg>
          </div>

          <table>
            <thead>
              <tr>
                <th style={{ width: "34%" }}>ESP32-C3-Zero</th>
                <th style={{ width: "26%" }}>Module pin</th>
                <th style={{ width: "18%" }}>Sketch define</th>
                <th style={{ width: "22%" }}>Note</th>
              </tr>
            </thead>
            <tbody>
              {WIRES.map((w, i) => {
                const mod = MODULES.find((m) => m.id === w.to[0]);
                return (
                  <tr
                    key={i}
                    className={marks(i, w.group)}
                    data-i={i}
                    data-group={w.group}
                    onClick={(e) => {
                      stop(e);
                      toggle(i);
                    }}
                  >
                    <td>
                      <span className="pinchip">{w.from}</span>
                    </td>
                    <td>
                      <span className="dot" style={{ background: w.color }} />
                      <span className="pinchip">
                        {mod?.name.split(" · ")[0] ?? w.to[0]} · {w.to[1]}
                      </span>
                    </td>
                    <td>
                      <span className="code">{w.code}</span>
                    </td>
                    <td className="note">{w.note}</td>
                  </tr>
                );
              })}
            </tbody>
          </table>
        </section>

        <div className="rule" />

        <section>
          <h2>
            <span className="n">02</span> · Reference
          </h2>
          <p className="sub">The board, with the used pins highlighted.</p>

          <div className="hero-media">
            <figure className="shot">
              {/* eslint-disable-next-line @next/next/no-img-element */}
              <img
                src="/schema/pinout.webp"
                alt="ESP32-C3-Zero pinout with the used pins highlighted"
                width={960}
                height={420}
                loading="lazy"
                decoding="async"
                onClick={zoom}
              />
              <figcaption>Pinout — used pins highlighted</figcaption>
            </figure>
          </div>
        </section>

        <div className="rule" />

        <section>
          <h2>
            <span className="n">03</span> · Modules
          </h2>
          <p className="sub">Pin names as printed on the boards in the photos.</p>
          <div className="mods">
            <figure className="shot">
              {/* eslint-disable-next-line @next/next/no-img-element */}
              <img
                src="/schema/tft.jpeg"
                alt="1.8 inch ST7735 SPI 128x160 display module, front and back"
                width={554}
                height={554}
                loading="lazy"
                decoding="async"
                onClick={zoom}
              />
              <figcaption>1.8″ TFT SPI 128×160 · V1.1</figcaption>
              <ul>
                <li>
                  <span>VCC</span>
                  <span>→ 3V3</span>
                </li>
                <li>
                  <span>GND</span>
                  <span>→ GND</span>
                </li>
                <li>
                  <span>LED</span>
                  <span>→ 3V3 (backlight, always on)</span>
                </li>
                <li>
                  <span>CS</span>
                  <span>→ GP5 · TFT_CS</span>
                </li>
                <li>
                  <span>RESET</span>
                  <span>→ GP4 · TFT_RST</span>
                </li>
                <li>
                  <span>A0 / DC</span>
                  <span>→ GP3 · TFT_DC</span>
                </li>
                <li>
                  <span>SDA</span>
                  <span>→ GP2 · TFT_MOSI</span>
                </li>
                <li>
                  <span>SCK</span>
                  <span>→ GP1 · TFT_SCLK</span>
                </li>
                <li>
                  <span className="dead">SDO / MISO</span>
                  <span className="dead">not connected</span>
                </li>
                <li>
                  <span className="dead">SD_CS · SD_MOSI · SD_MISO · SD_SCK</span>
                  <span className="dead">SD card unused</span>
                </li>
              </ul>
            </figure>

            <figure className="shot">
              {/* eslint-disable-next-line @next/next/no-img-element */}
              <img
                src="/schema/touch.jpg"
                alt="TTP223 capacitive touch switch module"
                width={1250}
                height={1250}
                loading="lazy"
                decoding="async"
                onClick={zoom}
              />
              <figcaption>TTP223 · capacitive touch</figcaption>
              <ul>
                <li>
                  <span>VCC</span>
                  <span>→ 3V3 (2–5.5 V)</span>
                </li>
                <li>
                  <span>GND</span>
                  <span>→ GND</span>
                </li>
                <li>
                  <span>SIG</span>
                  <span>→ GP0 · TOUCH_PIN</span>
                </li>
              </ul>
            </figure>
          </div>
        </section>

        <footer>
          <b>Notes</b>
          <ul>
            <li>
              The TFT runs write-only on SPI (GP1 SCK, GP2 MOSI) — MISO is not wired, so the
              display is never read back.
            </li>
            <li>
              Backlight is tied to 3V3: the sketches don&apos;t PWM it, so it&apos;s on whenever
              the board is powered.
            </li>
            <li>
              GP0 is the touch input (TTP223 SIG) and also a capacitive-touch capable pad on the
              C3; GP9 stays the onboard BOOT button.
            </li>
            <li>
              GP10 drives the C3-Zero&apos;s on-board WS2812 — no wiring, it&apos;s part of the
              board (nightlight in the pong clock, status flashes elsewhere).
            </li>
            <li>
              GND is shared: the display and the touch module return to the same rail. 5V stays
              unconnected — display and touch run on 3V3.
            </li>
            <li>
              The TTP223 pad order varies between batches — the square pad is pin 1. Check the
              silkscreen before powering (pads are numbered 1 · 2 · 3 in the diagram).
            </li>
          </ul>
        </footer>
      </div>

      {lightbox && (
        <div
          id="lightbox"
          className="on"
          role="dialog"
          aria-modal="true"
          aria-label="Image preview"
          onClick={() => setLightbox(null)}
        >
          <div>
            {/* eslint-disable-next-line @next/next/no-img-element */}
            <img src={lightbox.src} alt={lightbox.alt} />
            <p>{lightbox.caption}</p>
          </div>
        </div>
      )}
    </div>
  );
}
