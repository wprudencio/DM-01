import Link from "next/link";
import { FIRMWARES, REPO_URL } from "@/lib/firmwares";

const VHS_SQUARES = [
  { cls: "bg-neon", delay: "0s" },
  { cls: "bg-yellow", delay: "1.3s" },
  { cls: "bg-hot", delay: "2.6s" },
  { cls: "bg-cyan", delay: "3.9s" },
];

const GITHUB_PATH =
  "M8 0C3.58 0 0 3.58 0 8c0 3.54 2.29 6.53 5.47 7.59.4.07.55-.17.55-.38 0-.19-.01-.82-.01-1.49-2.01.37-2.53-.49-2.69-.94-.09-.23-.48-.94-.82-1.13-.28-.15-.68-.52-.01-.53.63-.01 1.08.58 1.23.82.72 1.21 1.87.87 2.33.66.07-.52.28-.87.51-1.07-1.78-.2-3.64-.89-3.64-3.95 0-.87.31-1.59.82-2.15-.08-.2-.36-1.02.08-2.12 0 0 .67-.21 2.2.82.64-.18 1.32-.27 2-.27.68 0 1.36.09 2 .27 1.53-1.04 2.2-.82 2.2-.82.44 1.1.16 1.92.08 2.12.51.56.82 1.27.82 2.15 0 3.07-1.87 3.75-3.65 3.95.29.25.54.73.54 1.48 0 1.07-.01 1.93-.01 2.2 0 .21.15.46.55.38A8.012 8.012 0 0 0 16 8c0-4.42-3.58-8-8-8Z";

export default function Home() {
  return (
    <main className="landing-site flex min-h-screen w-full flex-col">
      <nav
        aria-label="Primary"
        className="flex flex-wrap items-center gap-x-3 gap-y-1.5 border-b border-line bg-[#0b0b0b] px-6 py-2.5 sm:px-10"
      >
        <Link
          href="/"
          title="DM-01 — home"
          className="font-pixel text-[13px] tracking-[0.08em] text-ink uppercase transition-opacity hover:opacity-80"
        >
          DM-01
        </Link>
        <a
          href={REPO_URL}
          target="_blank"
          rel="noreferrer"
          title="GitHub — create your own apps"
          className="ml-auto flex items-center gap-2 font-pixel text-[10px] tracking-[0.26em] text-dim uppercase transition-colors hover:text-neon"
        >
          Create your own apps
          <svg
            viewBox="0 0 16 16"
            width="16"
            height="16"
            fill="currentColor"
            aria-hidden="true"
            className="shrink-0"
          >
            <path d={GITHUB_PATH} />
          </svg>
        </a>
      </nav>

      <div className="grid flex-1 grid-cols-1 lg:grid-cols-2">
      <Link
        href="/vhs"
        className="group relative flex flex-col gap-10 overflow-hidden border-b border-line bg-[#0b0b0b] p-6 sm:p-10 lg:border-r lg:border-b-0"
      >
        <div aria-hidden className="vhs-scan pointer-events-none absolute inset-0" />
        <div
          aria-hidden
          className="pointer-events-none absolute inset-0"
          style={{
            background:
              "radial-gradient(760px 420px at 26% -8%, rgba(236,0,140,0.16), transparent 62%)",
          }}
        />

        <div className="relative flex items-center gap-3">
          <span className="grid h-5 w-5 shrink-0 grid-cols-2 gap-[2px] border border-line2 p-[2px]">
            {VHS_SQUARES.map((s) => (
              <span
                key={s.cls}
                className={`animate-tape ${s.cls}`}
                style={{ animationDelay: s.delay }}
              />
            ))}
          </span>
          <span className="aberrate font-pixel text-[10px] tracking-[0.3em] text-hot uppercase">
            Signal edition
          </span>
          <span aria-hidden className="animate-pip h-2 w-2 bg-hot" />
        </div>

        <div className="relative flex flex-col items-start gap-5">
          <h1 className="wordmark font-pixel text-[56px] leading-none sm:text-[80px] lg:min-h-[109px]">VHS</h1>
          <p className="w-full max-w-sm text-[13px] leading-[1.7] text-mid lg:min-h-[89px]">
            The original deck: every sketch on the dark SIGNAL theme — white ink, magenta
            and signal green, VHS scanlines and chromatic glitch bursts.
          </p>

          <div className="relative mt-1 w-full max-w-[420px] border border-line bg-[#080808] p-1">
            <div
              aria-hidden
              className="pointer-events-none absolute inset-0 z-10 opacity-60"
              style={{
                background:
                  "repeating-linear-gradient(180deg, rgba(255,255,255,0.07) 0 1px, transparent 1px 3px)",
              }}
            />
            {/* eslint-disable-next-line @next/next/no-img-element */}
            <img
              src="/screenshots/hn_cyd.png"
              alt="Hacker News on the CYD in the VHS theme"
              width={640}
              height={480}
              fetchPriority="high"
              decoding="async"
              className="pixelated block w-full transition-transform duration-500 group-hover:scale-[1.015]"
            />
          </div>

          <span className="mt-1 border-2 border-transparent bg-gradient-to-r from-neon to-hot px-6 py-3 font-pixel text-[11px] tracking-[0.14em] text-void uppercase transition group-hover:brightness-110">
            Enter the deck →
          </span>
        </div>

        <div className="relative mt-auto flex flex-wrap items-center gap-x-6 gap-y-1 text-[10px] tracking-[0.16em] text-dim uppercase">
          <span>{FIRMWARES.length} firmwares</span>
          <span>ESP32-C3 + CYD</span>
          <span className="text-neon">Dark deck</span>
        </div>
      </Link>

      <Link
        href="/quartz"
        className="group lcd-dots relative flex flex-col gap-10 overflow-hidden bg-lcd p-6 text-lcd-ink sm:p-10"
      >
        <div className="relative flex items-center gap-3">
          <span className="grid h-5 w-5 shrink-0 grid-cols-2 gap-[2px] border border-lcd-edge p-[2px]">
            <span className="bg-lcd-ink" />
            <span className="bg-lcd-ghost" />
            <span className="bg-lcd-ghost" />
            <span className="lcd-blink bg-lcd-accent" />
          </span>
          <span className="font-pixel text-[10px] tracking-[0.3em] text-lcd-accent uppercase">
            Light-LCD edition
          </span>
        </div>

        <div className="relative flex flex-col items-start gap-5">
          <h1 className="font-pixel text-[56px] leading-none text-lcd-ink sm:text-[80px]">
            Quartz
          </h1>
          <div aria-hidden className="flex flex-col">
            <div className="h-px w-56 bg-lcd-accentdk" />
            <div className="h-2 w-56 bg-lcd-accent" />
          </div>
          <p className="w-full max-w-sm text-[13px] leading-[1.7] text-lcd-ink/80 lg:min-h-[89px]">
            The watch-face deck: the same sketches redrawn on a flat light LCD — dark ink,
            ghost 7-seg digits and the red alarm sweep. No glitch over the readout.
          </p>

          <div className="relative mt-1 w-full max-w-[420px] border border-lcd-edge bg-lcd p-1">
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
              src="/screenshots/pong_clock_quartz_cyd.png"
              alt="Pong Clock on the CYD in the QUARTZ light-LCD theme"
              width={640}
              height={480}
              fetchPriority="high"
              decoding="async"
              className="pixelated block w-full transition-transform duration-500 group-hover:scale-[1.015]"
            />
          </div>

          <span className="mt-1 border-2 border-lcd-ink bg-lcd-ink px-6 py-3 font-pixel text-[11px] tracking-[0.14em] text-lcd uppercase transition group-hover:brightness-110">
            Enter the deck →
          </span>
        </div>

        <div className="relative mt-auto flex flex-wrap items-center gap-x-6 gap-y-1 font-pixel text-[10px] tracking-[0.14em] text-lcd-dim uppercase">
          <span>{FIRMWARES.length} firmwares</span>
          <span>ESP32-C3 + CYD</span>
          <span className="text-lcd-accent">Light LCD</span>
        </div>
      </Link>
      </div>

      <Link
        href="/schema"
        title="Wiring map: ESP32-C3-Zero, ST7735 display and TTP223 touch pad"
        className="group relative flex flex-col gap-3 overflow-hidden bg-hot px-6 py-5 text-void sm:flex-row sm:items-center sm:justify-between sm:gap-6 sm:px-10"
      >
        <div className="flex items-center gap-4">
          <span aria-hidden className="animate-pip h-3 w-3 shrink-0 bg-void" />
          <div className="flex flex-col gap-1">
            <span className="font-pixel text-[10px] tracking-[0.3em] uppercase">
              Start here — new build?
            </span>
            <span className="font-pixel text-[18px] leading-tight tracking-[0.04em] uppercase sm:text-[22px]">
              Wiring map · ESP32-C3 + ST7735 + TTP223
            </span>
            <span className="text-[12px] leading-snug font-medium opacity-80">
              11 wires, interactive diagram, the exact GP numbers the sketches
              compile against.
            </span>
          </div>
        </div>
        <span className="inline-flex w-fit shrink-0 items-center gap-2 border-2 border-void px-5 py-2.5 font-pixel text-[11px] tracking-[0.14em] uppercase transition group-hover:bg-void group-hover:text-hot">
          View the wires
          <span aria-hidden className="transition-transform duration-200 group-hover:translate-x-1">
            →
          </span>
        </span>
      </Link>
    </main>
  );
}
