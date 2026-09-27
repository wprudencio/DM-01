"use client";

import Link from "next/link";
import { useCallback, useRef, useState, useSyncExternalStore } from "react";
import type { Transport as TransportType } from "esptool-js";
import { FIRMWARES, REPO_URL, type Firmware, type Variant } from "@/lib/firmwares";

// QUARTZ editions, keyed "<firmware>:<variant>". A shot swaps the DM-01
// preview for the light-LCD screen; a bin powers Flash + the download link.
type QuartzReady = { shot?: string; bin?: string };

const QUARTZ_READY: Record<string, QuartzReady> = {
  "pomodoro:c3": {
    shot: "/screenshots/pomodoro_quartz.png",
    bin: "/firmware/pomodoro_quartz/c3.bin",
  },
  "pomodoro:cyd": {
    shot: "/screenshots/pomodoro_quartz_cyd.png",
    bin: "/firmware/pomodoro_quartz/cyd.bin",
  },
  "3d_cube:c3": {
    shot: "/screenshots/3d_cube_quartz.png",
    bin: "/firmware/3d_cube_quartz/c3.bin",
  },
  "3d_cube:cyd": {
    shot: "/screenshots/3d_cube_quartz_cyd.png",
    bin: "/firmware/3d_cube_quartz/cyd.bin",
  },
  "flappy:c3": {
    shot: "/screenshots/flappy_quartz.png",
    bin: "/firmware/flappy_quartz/c3.bin",
  },
  "flappy:cyd": {
    shot: "/screenshots/flappy_quartz_cyd.png",
    bin: "/firmware/flappy_quartz/cyd.bin",
  },
  "neko:c3": {
    shot: "/screenshots/neko_quartz.png",
    bin: "/firmware/neko_quartz/c3.bin",
  },
  "neko:cyd": {
    shot: "/screenshots/neko_quartz_cyd.png",
    bin: "/firmware/neko_quartz/cyd.bin",
  },
  "pong_clock:c3": {
    shot: "/screenshots/pong_clock_quartz.png",
    bin: "/firmware/pong_clock_quartz/c3.bin",
  },
  "pong_clock:cyd": {
    shot: "/screenshots/pong_clock_quartz_cyd.png",
    bin: "/firmware/pong_clock_quartz/cyd.bin",
  },
  "btc_ticker:c3": {
    shot: "/screenshots/btc_ticker_quartz.png",
    bin: "/firmware/btc_ticker_quartz/c3.bin",
  },
  "btc_ticker:cyd": {
    shot: "/screenshots/btc_ticker_quartz_cyd.png",
    bin: "/firmware/btc_ticker_quartz/cyd.bin",
  },
  "dvd:c3": {
    shot: "/screenshots/dvd_quartz.png",
    bin: "/firmware/dvd_quartz/c3.bin",
  },
  "dvd:cyd": {
    shot: "/screenshots/dvd_quartz_cyd.png",
    bin: "/firmware/dvd_quartz/cyd.bin",
  },
  "asteroids:c3": {
    shot: "/screenshots/asteroids_quartz.png",
    bin: "/firmware/asteroids_quartz/c3.bin",
  },
  "asteroids:cyd": {
    shot: "/screenshots/asteroids_quartz_cyd.png",
    bin: "/firmware/asteroids_quartz/cyd.bin",
  },
  "hn:c3": {
    shot: "/screenshots/hn_quartz.png",
    bin: "/firmware/hn_quartz/c3.bin",
  },
  "hn:cyd": {
    shot: "/screenshots/hn_quartz_cyd.png",
    bin: "/firmware/hn_quartz/cyd.bin",
  },
  "github_squares:c3": {
    shot: "/screenshots/github_squares_quartz.png",
    bin: "/firmware/github_squares_quartz/c3.bin",
  },
  "github_squares:cyd": {
    shot: "/screenshots/github_squares_quartz_cyd.png",
    bin: "/firmware/github_squares_quartz/cyd.bin",
  },
};

const GITHUB_PATH =
  "M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.012 8.012 0 0 0 16 8c0-4.42-3.58-8-8-8Z";

function quartzShot(fw: Firmware, variant: Variant): string {
  return QUARTZ_READY[`${fw.id}:${variant.id}`]?.shot ?? variant.screenshot;
}

function quartzBin(fw: Firmware, variant: Variant): string | undefined {
  return QUARTZ_READY[`${fw.id}:${variant.id}`]?.bin;
}

type Phase = "idle" | "connecting" | "writing" | "resetting" | "done" | "error";

type LogLine = { ts: string; text: string; tone: "dim" | "ok" | "warn" };

const PHASE_TEXT: Record<Phase, string> = {
  idle: "Ready",
  connecting: "Connecting",
  writing: "Writing",
  resetting: "Resetting",
  done: "Done",
  error: "Error",
};

const TONE: Record<LogLine["tone"], string> = {
  dim: "text-lcd-dim",
  ok: "text-lcd-ink",
  warn: "text-lcd-accent",
};

const SUBSCRIBE = () => () => {};
const GET_SNAPSHOT = () => "serial" in navigator;
const GET_SERVER_SNAPSHOT = () => true;

async function withTimeout(
  promise: Promise<unknown>,
  ms: number,
): Promise<"done" | "timeout"> {
  return Promise.race([
    promise.then(() => "done" as const).catch(() => "done" as const),
    new Promise<"timeout">((resolve) => setTimeout(() => resolve("timeout"), ms)),
  ]);
}

function chipMatches(variant: Variant, chip: string): boolean {
  const c = chip.toUpperCase();
  if (variant.id === "c3") {
    return c.startsWith("ESP32-C3") || c.startsWith("ESP8685") || c.startsWith("ESP8686");
  }
  return /^ESP32-(D0WD|D2WD|U4WDH|PICO|S0WD)/.test(c);
}

function logTone(line: string): LogLine["tone"] {
  const upper = line.toUpperCase();
  if (
    upper.includes("ERROR") ||
    upper.includes("FAILED") ||
    upper.includes("WRONG") ||
    upper.includes("CANCELLED")
  ) {
    return "warn";
  }
  if (upper.startsWith("FLASH OK") || upper.startsWith("CHIP")) return "ok";
  return "dim";
}

type CardProps = {
  fw: Firmware;
  variant: Variant;
  phase: Phase;
  progress: number;
  isActive: boolean;
  busy: boolean;
  supported: boolean;
  logs: LogLine[];
  index: string;
  onSelect: (fwId: string, variantId: string) => void;
  onFlash: (fw: Firmware, variant: Variant) => void;
};

function SketchCard({
  fw,
  variant,
  phase,
  progress,
  isActive,
  busy,
  supported,
  logs,
  index,
  onSelect,
  onFlash,
}: CardProps) {
  const showState = isActive && (busy || phase === "done" || phase === "error");
  const label =
    isActive && busy ? "Flashing…" : isActive && phase === "done" ? "Flash again" : "Flash";

  const isError = isActive && phase === "error";
  const shot = quartzShot(fw, variant);
  const bin = quartzBin(fw, variant);

  return (
    <article className="flex flex-col border border-lcd-edge bg-lcd-hi/30 p-5 transition-colors hover:border-lcd-ink">
      <div className="flex items-baseline justify-between gap-3 font-pixel text-[10px] tracking-[0.14em] text-lcd-dim uppercase">
        <span>
          <span className="text-lcd-accent">{index}</span> · {fw.tagline}
        </span>
        <span>{variant.chip}</span>
      </div>

      <div className="relative mt-4 border border-lcd-edge bg-lcd p-1">
        <div
          aria-hidden
          className="pointer-events-none absolute inset-0 z-10 opacity-50"
          style={{
            background:
              "repeating-linear-gradient(180deg, rgba(28,34,24,0.08) 0 1px, transparent 1px 3px)",
          }}
        />
        {/* eslint-disable-next-line @next/next/no-img-element */}
        <img
          src={shot}
          alt={`${fw.name} on ${variant.board}`}
          width={variant.id === "c3" ? 480 : 640}
          height={variant.id === "c3" ? 384 : 480}
          loading="lazy"
          decoding="async"
          className="pixelated block w-full"
        />
      </div>

      <h2 className="mt-4 font-pixel text-[14px] tracking-[0.06em] text-lcd-ink uppercase">
        {fw.name}
      </h2>
      <p className="mt-2 text-[13px] leading-[1.65] text-lcd-ink/80">{fw.description}</p>

      <div className="mt-4 grid grid-cols-2 gap-2">
        {fw.variants.map((item) => {
          const on = item.id === variant.id;
          return (
            <button
              key={item.id}
              type="button"
              disabled={busy}
              onClick={() => onSelect(fw.id, item.id)}
              className={`flex items-center gap-2.5 border px-3 py-2 text-left transition disabled:cursor-not-allowed ${
                on ? "border-lcd-ink bg-lcd-ink/10" : "border-lcd-edge hover:border-lcd-ink"
              }`}
            >
              <svg
                width="18"
                height="14"
                viewBox="0 0 18 14"
                aria-hidden="true"
                className={`shrink-0 ${on ? "text-lcd-ink" : "text-lcd-dim"}`}
              >
                <rect x="0.5" y="0.5" width="17" height="13" fill="none" stroke="currentColor" />
                {item.id === "c3" ? (
                  <rect x="4" y="3" width="10" height="8" fill="currentColor" opacity="0.4" />
                ) : (
                  <rect x="2.5" y="3.5" width="13" height="7" fill="currentColor" opacity="0.4" />
                )}
              </svg>
              <span className="min-w-0">
                <span
                  className={`block font-pixel text-[10px] tracking-[0.14em] uppercase ${
                    on ? "text-lcd-ink" : "text-lcd-dim"
                  }`}
                >
                  {item.label}
                </span>
                <span className="block truncate text-[9px] tracking-[0.06em] text-lcd-dim uppercase">
                  {item.board}
                </span>
              </span>
            </button>
          );
        })}
      </div>

      <div className="mt-3 flex items-center gap-4">
        <button
          type="button"
          onClick={() => onFlash(fw, variant)}
          disabled={!supported || busy || !bin}
          className="flex-1 border-2 border-lcd-ink bg-lcd-ink px-5 py-2.5 font-pixel text-[11px] tracking-[0.14em] text-lcd uppercase transition hover:brightness-110 disabled:cursor-not-allowed disabled:opacity-40"
        >
          {label}
        </button>
        {bin ? (
          <a
            href={bin}
            download={`${fw.id}-quartz-${variant.id}.bin`}
            className="text-[12px] tracking-[0.08em] text-lcd-dim uppercase underline-offset-4 transition-colors hover:text-lcd-ink hover:underline"
          >
            .bin ↓
          </a>
        ) : (
          <span className="font-pixel text-[10px] tracking-[0.08em] text-lcd-ghost uppercase">
            soon
          </span>
        )}
      </div>

      {showState && (
        <div className="mt-4 flex flex-col gap-2">
          <div className="flex items-baseline justify-between font-pixel text-[10px] tracking-[0.12em] uppercase">
            <span className={isError ? "text-lcd-accent" : "text-lcd-dim"}>
              {PHASE_TEXT[phase]}
            </span>
            <span className={isError ? "text-lcd-accent" : "text-lcd-ink"}>{progress}%</span>
          </div>
          <div className="relative h-1 bg-lcd-ghost/40">
            <div
              className={`h-full transition-[width] duration-200 ${
                isError ? "bg-lcd-accent" : "bg-lcd-ink"
              }`}
              style={{ width: `${progress}%` }}
            />
            {busy && <div aria-hidden className="animate-shimmer absolute inset-0" />}
          </div>
          {logs.length > 0 && (
            <details className="mt-1" open={busy}>
              <summary className="cursor-pointer text-[11px] tracking-[0.08em] text-lcd-dim uppercase select-none hover:text-lcd-ink">
                Event log · {logs.length}
              </summary>
              <div className="mt-2 max-h-36 overflow-y-auto border border-lcd-edge bg-lcd-hi/50 p-2">
                {logs.map((line, i) => (
                  <div
                    key={i}
                    className={`text-[11px] leading-[1.7] tracking-[0.02em] ${TONE[line.tone]}`}
                  >
                    <span className="mr-1.5 text-lcd-dim">{line.ts}</span>
                    {line.text}
                  </div>
                ))}
              </div>
            </details>
          )}
        </div>
      )}
    </article>
  );
}

export default function QuartzGallery() {
  const [activeId, setActiveId] = useState<string | null>(null);
  const [phase, setPhase] = useState<Phase>("idle");
  const [progress, setProgress] = useState(0);
  const [logs, setLogs] = useState<LogLine[]>([]);
  const [selection, setSelection] = useState<Record<string, string>>(() =>
    Object.fromEntries(FIRMWARES.map((fw) => [fw.id, fw.variants[0].id])),
  );
  const supported = useSyncExternalStore(SUBSCRIBE, GET_SNAPSHOT, GET_SERVER_SNAPSHOT);
  const running = useRef(false);
  const partial = useRef("");

  const busy = phase === "connecting" || phase === "writing" || phase === "resetting";

  const append = useCallback((line: string) => {
    const entry: LogLine = {
      ts: new Date().toTimeString().slice(0, 8),
      text: line,
      tone: logTone(line),
    };
    setLogs((prev) => {
      const next = [...prev, entry];
      return next.length > 300 ? next.slice(-300) : next;
    });
  }, []);

  const selectVariant = useCallback((fwId: string, variantId: string) => {
    setSelection((prev) => ({ ...prev, [fwId]: variantId }));
  }, []);

  const flash = useCallback(
    async (fw: Firmware, variant: Variant) => {
      const bin = quartzBin(fw, variant);
      if (running.current || !bin) return;
      running.current = true;
      partial.current = "";
      setActiveId(fw.id);
      setLogs([]);
      setProgress(0);
      setPhase("connecting");

      let transport: TransportType | null = null;

      try {
        const port = await navigator.serial.requestPort();
        const { ESPLoader, Transport } = await import("esptool-js");
        transport = new Transport(port, false);
        transport.setDeviceLostCallback(() => append("PORT CLOSED (USB RESET)"));

        const loader = new ESPLoader({
          transport,
          baudrate: variant.baud,
          terminal: {
            clean: () => setLogs([]),
            writeLine: (data: string) => append(data),
            write: (data: string) => {
              partial.current += data;
              const lines = partial.current.split("\n");
              partial.current = lines.pop() ?? "";
              lines.filter((line) => line.trim()).forEach(append);
            },
          },
        });

        append(`${fw.name} QUARTZ ${variant.label} — CONNECTING...`);
        const chip = await loader.main();
        append(`CHIP ${chip}`);

        if (!chipMatches(variant, chip)) {
          await withTimeout(loader.after("hard_reset"), 5000);
          throw new Error(`WRONG CHIP ${chip} — ${variant.label} NEEDS ${variant.chip}`);
        }

        const res = await fetch(bin, { cache: "no-store" });
        if (!res.ok) throw new Error(`FIRMWARE FETCH FAILED (${res.status})`);
        const data = new Uint8Array(await res.arrayBuffer());
        append(`IMAGE ${(data.length / 1048576).toFixed(0)} MB @ 0x0`);

        setPhase("writing");
        await loader.writeFlash({
          fileArray: [{ data, address: 0x0 }],
          flashSize: "keep",
          flashMode: "keep",
          flashFreq: "keep",
          eraseAll: false,
          compress: true,
          reportProgress: (_fileIndex, written, total) => {
            setProgress(total > 0 ? Math.min(100, Math.round((written / total) * 100)) : 0);
          },
        });

        setPhase("resetting");
        append("RESETTING...");
        const reset = await withTimeout(
          loader.after("custom_reset", false, "R1|W200|R0|W200"),
          5000,
        );
        if (reset === "timeout") append("NO RESET ACK — BOARD REBOOTS ON ITS OWN");
        setProgress(100);
        setPhase("done");
        append("FLASH OK");
      } catch (err) {
        const error = err as Error;
        if (error?.name === "NotFoundError") {
          append("CANCELLED");
          setPhase("idle");
          setActiveId(null);
        } else {
          append(`ERROR ${error?.message ?? String(err)}`);
          setPhase("error");
        }
      } finally {
        if (transport) {
          await withTimeout(transport.disconnect(), 3000);
        }
        running.current = false;
      }
    },
    [append],
  );

  return (
    <div className="quartz-site lcd-dots flex min-h-screen w-full flex-col">
      <header className="sticky top-0 z-50 grid h-14 shrink-0 grid-cols-[1fr_auto_1fr] items-center gap-4 border-b border-lcd-edge bg-lcd/90 px-5 backdrop-blur sm:px-8">
        <div className="flex items-center gap-3">
          <Link
            href="/"
            title="Home"
            className="flex items-center gap-3 transition-opacity hover:opacity-80"
          >
            <span className="grid h-5 w-5 shrink-0 grid-cols-2 gap-[2px] border border-lcd-edge p-[2px]">
              <span className="bg-lcd-ink" />
              <span className="bg-lcd-ghost" />
              <span className="bg-lcd-ghost" />
              <span className="lcd-blink bg-lcd-accent" />
            </span>
            <span className="font-pixel text-[13px] tracking-[0.08em] text-lcd-ink uppercase">
              Quartz
            </span>
          </Link>
        </div>

        <span className="hidden font-pixel text-[10px] tracking-[0.32em] text-lcd-dim uppercase sm:block">
          Firmware Deck
        </span>

        <div className="flex items-center justify-end gap-4">
          <Link
            href="/schema"
            className="font-pixel text-[10px] tracking-[0.18em] text-lcd-dim uppercase transition-colors hover:text-lcd-ink"
          >
            Wiring
          </Link>
          <Link
            href="/vhs"
            className="font-pixel text-[10px] tracking-[0.18em] text-lcd-dim uppercase transition-colors hover:text-lcd-ink"
          >
            VHS ↗
          </Link>
          <a
            href={REPO_URL}
            target="_blank"
            rel="noreferrer"
            aria-label="GitHub repository"
            title="GitHub"
            className="text-lcd-dim transition-colors hover:text-lcd-ink"
          >
            <svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor" aria-hidden="true">
              <path d={GITHUB_PATH} />
            </svg>
          </a>
        </div>
      </header>

      <div className="flex flex-1 flex-col">
        <header className="relative isolate flex flex-col items-center gap-4 overflow-hidden px-5 pt-10 pb-8 text-center sm:pt-14">
          <p className="font-pixel text-[10px] tracking-[0.4em] text-lcd-accent uppercase">
            Light-LCD firmware deck
          </p>
          <h1 className="font-pixel text-[64px] leading-none text-lcd-ink sm:text-[92px]">
            Quartz
          </h1>
          <div aria-hidden className="flex flex-col items-center">
            <div className="h-px w-64 bg-lcd-accentdk" />
            <div className="h-2 w-64 bg-lcd-accent" />
          </div>
          <p className="max-w-md text-[13px] leading-[1.7] text-lcd-ink/80">
            Flash the QUARTZ light-LCD editions for ESP32-C3 and CYD straight over
            WebSerial — no toolchain, no drivers.
          </p>
          <div className="mt-1 flex items-center gap-6">
            <a
              href="#sketches"
              className="border-2 border-lcd-ink bg-lcd-ink px-6 py-3 font-pixel text-[11px] tracking-[0.14em] text-lcd uppercase transition hover:brightness-110"
            >
              Browse firmwares
            </a>
            <a
              href={REPO_URL}
              target="_blank"
              rel="noreferrer"
              className="text-[12px] tracking-[0.1em] text-lcd-dim uppercase underline-offset-4 transition-colors hover:text-lcd-ink hover:underline"
            >
              GitHub
            </a>
          </div>
          <div aria-hidden className="h-14" />
        </header>

        <div
          id="sketches"
          className="mx-auto grid w-full max-w-[1000px] scroll-mt-20 gap-6 px-5 pb-16 sm:px-8 md:grid-cols-2"
        >
          {FIRMWARES.map((fw, i) => (
            <SketchCard
              key={fw.id}
              fw={fw}
              variant={fw.variants.find((item) => item.id === selection[fw.id]) ?? fw.variants[0]}
              phase={phase}
              progress={progress}
              isActive={activeId === fw.id}
              busy={busy}
              supported={supported}
              logs={logs}
              index={`0${i + 1}`}
              onSelect={selectVariant}
              onFlash={flash}
            />
          ))}
        </div>

        {!supported && (
          <div className="mx-auto w-full max-w-[1000px] px-5 pb-10 sm:px-8">
            <p className="border border-lcd-edge bg-lcd-hi/40 px-4 py-3 text-center text-[13px] text-lcd-ink/80">
              WebSerial needs Chrome, Edge or Opera.
            </p>
          </div>
        )}
      </div>

      <footer className="flex h-12 shrink-0 items-center justify-center border-t border-lcd-edge bg-lcd/90 px-5">
        <p className="font-pixel text-[10px] tracking-[0.16em] text-lcd-dim uppercase">
          Quartz · Close serial monitors before flashing ·{" "}
          <span className="text-lcd-accent">Hold BOOT if stuck</span>
        </p>
      </footer>
    </div>
  );
}
