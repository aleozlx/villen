#!/usr/bin/env node
// Generate Villen's standalone UI SVGs.  Keeping the geometry here makes the
// set easy to extend without letting individual icon files drift in weight or
// palette.
import { mkdirSync, writeFileSync } from "node:fs";

const out = new URL("../rc/ui/", import.meta.url);
mkdirSync(out, { recursive: true });

// Escape text destined for XML — a future title containing & < > " or ' would
// otherwise produce an invalid SVG.
const xmlEscape = (s) =>
  s.replace(/[&<>"']/g, (c) => ({ "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;", "'": "&apos;" }[c]));

// `id` is a per-file slug so the <title>/aria-labelledby ids stay unique when
// several icons are inlined into one document (and an active variant never reuses
// its base icon's id). Internal defs ids (e.g. gradients) follow the same rule by
// prefixing with the icon name.
const shell = (id, title, body, active = false) => `<?xml version="1.0" encoding="UTF-8"?>
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 64 64" role="img" aria-labelledby="${id}-title" color="${active ? "#15181D" : "#F0A34A"}">
  <title id="${id}-title">${xmlEscape(title)}</title>
  <style>.s{fill:none;stroke:currentColor;stroke-width:4;stroke-linecap:round;stroke-linejoin:round}.f{fill:currentColor}.a{fill:#E2603B}.muted{fill:none;stroke:currentColor;stroke-width:3;stroke-linecap:round;stroke-linejoin:round}</style>
  ${active ? '<circle cx="32" cy="32" r="29" fill="#E2603B"/>' : ""}
  ${body}
</svg>
`;

const icons = {
  // Launcher tiles ----------------------------------------------------------
  "engine-chess": ["Chess with archbishop", '<g class="s"><rect x="11" y="11" width="42" height="42" rx="5"/><path d="M25 11v42M39 11v42M11 25h42M11 39h42"/><path d="M32 18v6M27 24h10l-2 7c4 3 6 8 6 13H23c0-5 2-10 6-13l-2-7z"/></g>'],
  "engine-filter": ["Camera morphology filter", '<g class="s"><path d="M10 22h13l4-6h10l4 6h13v28H10z"/><circle cx="32" cy="36" r="10"/><path d="M22 36h20M32 26v20"/></g><path class="a" d="M48 13h7v7h-7z"/>'],
  "engine-snake": ["Snake and apple", '<g class="s"><path d="M13 41c0-12 13-15 18-8 4 6-4 12-9 7-4-4 2-10 10-8 8 1 14 8 11 16-2 7-12 8-17 2"/><path d="M45 20c5-6 12-1 9 5-3 6-12 3-11-3 1-4 5-4 7-1"/><path d="M49 17c0-4 3-6 6-7"/></g><circle class="a" cx="15" cy="41" r="3"/>'],
  "engine-canvas": ["Paint brush and canvas", '<g class="s"><path d="M17 47c4 2 10 1 12-4L50 22l-8-8-21 21c-5 0-8 5-4 12z"/><path d="M37 19l8 8"/><path d="M14 16l3-3M14 28h4M25 14v4"/></g><path class="a" d="M16 45c4 2 8 1 10-3l-8-8c-4 2-5 7-2 11z"/>'],
  "engine-jam": ["Step sequencer and note", '<g class="s"><rect x="10" y="16" width="30" height="32" rx="4"/><path d="M20 16v32M30 16v32M10 27h30M10 38h30"/><path d="M48 18v22a6 6 0 1 1-4-5V23l10-3v17a6 6 0 1 1-4-5V18z"/></g><path class="a" d="M12 18h6v7h-6zM22 29h6v7h-6zM32 40h6v6h-6z"/>'],
  "engine-chat": ["Local chat", '<g class="s"><path d="M10 15h44v28H27L16 51v-8h-6z"/><path d="M21 29h1M31 29h1M41 29h1"/></g><path class="a" d="M47 10l2 5 5 2-5 2-2 5-2-5-5-2 5-2z"/>'],

  // Persistent shell chrome -------------------------------------------------
  home: ["Home", '<g class="s"><path d="M9 30L32 11l23 19v23H39V39H25v14H9z"/></g>'],
  "system-info": ["System information", '<g class="s"><circle cx="32" cy="32" r="22"/><path d="M32 29v15M32 20h.01"/></g>'],
  power: ["Quit or power", '<g class="s"><path d="M32 9v23M20 16a21 21 0 1 0 24 0"/></g>'],
  players: ["Connected players", '<g class="s"><circle cx="25" cy="24" r="8"/><path d="M10 51c1-10 8-16 15-16s14 6 15 16M43 18a7 7 0 0 1 0 13M47 36c5 3 7 8 7 15"/></g>'],
  "seat-connected": ["Connected seat", '<circle class="a" cx="32" cy="32" r="20"/><path d="M21 32l7 7 15-17" fill="none" stroke="#15181D" stroke-width="5" stroke-linecap="round" stroke-linejoin="round"/>'],
  "seat-disconnected": ["Disconnected seat", '<circle class="a" cx="32" cy="32" r="20"/><path d="M24 24l16 16M40 24L24 40" fill="none" stroke="#15181D" stroke-width="5" stroke-linecap="round"/>'],
  "seat-open": ["Open seat", '<g class="s"><circle cx="32" cy="32" r="20"/><path d="M32 21v22M21 32h22"/></g>'],

  // Jam controls ------------------------------------------------------------
  "jam-play": ["Play", '<path class="f" d="M21 14l28 18-28 18z"/>'],
  "jam-stop": ["Stop", '<rect class="f" x="18" y="18" width="28" height="28" rx="3"/>'],
  "jam-pause": ["Pause", '<rect class="f" x="18" y="16" width="10" height="32" rx="3"/><rect class="f" x="36" y="16" width="10" height="32" rx="3"/>'],
  "jam-mute": ["Mute", '<g class="s"><path d="M12 27h11l12-10v30L23 37H12z"/><path d="M45 25l10 14M55 25L45 39"/></g>'],
  "jam-tempo": ["Metronome tempo", '<g class="s"><path d="M19 52h26L38 13H26zM32 23l7 18"/><path d="M14 52h36"/></g><circle class="a" cx="40" cy="42" r="3"/>'],
  "jam-kick": ["Kick drum", '<g class="s"><circle cx="30" cy="34" r="17"/><path d="M47 27h7v14h-7M17 48l-5 6M42 49l4 5"/></g><circle class="a" cx="30" cy="34" r="6"/>'],
  "jam-snare": ["Snare drum", '<g class="s"><path d="M14 27h36l-4 20H18zM12 25h40M20 47v7M44 47v7"/><path d="M20 34h24"/></g>'],
  "jam-hat": ["Hi-hat cymbals", '<g class="s"><path d="M13 29h38L32 17zM13 35h38L32 47zM32 47v8"/></g>'],
  "jam-bass": ["Bass instrument", '<g class="s"><path d="M21 46c-5 0-8-4-8-8s3-8 8-8c4 0 7 3 7 7l19-19 5 5-19 19c0 4-3 7-7 7z"/><path d="M42 19l4 4M46 15l4 4"/></g>'],
  "jam-synth": ["Synthesizer", '<g class="s"><rect x="10" y="21" width="44" height="25" rx="4"/><path d="M18 21v14M25 21v14M32 21v14M39 21v14M46 21v14"/><circle cx="18" cy="40" r="2"/><circle cx="26" cy="40" r="2"/></g>'],

  // Canvas controls ---------------------------------------------------------
  brush: ["Brush", '<g class="s"><path d="M14 50c6 3 14 0 15-8L51 20l-7-7-22 22c-7 0-10 7-8 15z"/><path d="M39 18l7 7"/></g>'],
  eraser: ["Eraser", '<g class="s"><path d="M19 48L11 40l24-24 14 14-18 18zM19 48h28"/></g>'],
  palette: ["Color palette", '<g class="s"><path d="M32 12C18 12 9 22 9 34c0 11 9 19 20 19h5c5 0 7-6 3-9-2-3 0-6 4-6h4c7 0 10-5 10-11 0-9-9-15-23-15z"/><circle cx="20" cy="31" r="2"/><circle cx="29" cy="23" r="2"/><circle cx="39" cy="27" r="2"/></g>'],
  clear: ["Clear canvas", '<g class="s"><path d="M17 49h30M22 45L13 36l20-20 12 12-17 17z"/><path d="M44 16l5 5"/></g><path class="a" d="M49 42l5 9H44z"/>'],
  undo: ["Undo", '<g class="s"><path d="M27 17L13 31l14 14M15 31h23c9 0 13 6 13 14"/></g>'],
  "pan-zoom": ["Pan and zoom", '<g class="s"><path d="M32 8v17M32 8l-6 6M32 8l6 6M32 56V39M32 56l-6-6M32 56l6-6M8 32h17M8 32l6-6M8 32l6 6M56 32H39M56 32l-6-6M56 32l-6 6"/><circle cx="32" cy="32" r="8"/></g>'],

  // Filter controls ---------------------------------------------------------
  "camera-record": ["Camera record", '<g class="s"><rect x="10" y="18" width="44" height="31" rx="5"/><circle cx="32" cy="34" r="10"/><path d="M20 18l3-6h18l3 6"/></g><circle class="a" cx="50" cy="14" r="6"/>'],
  "privacy-lock": ["Private on-device processing", '<g class="s"><rect x="13" y="28" width="38" height="25" rx="4"/><path d="M21 28v-7a11 11 0 0 1 22 0v7"/></g><circle class="a" cx="32" cy="40" r="3"/>'],
  "pipeline-add": ["Add pipeline operator", '<g class="s"><circle cx="16" cy="32" r="6"/><circle cx="32" cy="32" r="6"/><path d="M22 32h4M38 32h12M50 23v18M41 32h18"/></g>'],
  "pipeline-remove": ["Remove pipeline operator", '<g class="s"><circle cx="16" cy="32" r="6"/><circle cx="32" cy="32" r="6"/><path d="M22 32h4M38 32h16M45 32h18"/></g>'],
  erode: ["Erode operator", '<g class="s"><path d="M32 13l19 19-19 19-19-19z"/><path d="M32 23v18M23 32h18"/></g><path class="a" d="M32 28l4 4-4 4-4-4z"/>'],
  dilate: ["Dilate operator", '<g class="s"><path d="M32 13l19 19-19 19-19-19z"/><path d="M32 19v26M19 32h26"/></g>'],
  open: ["Open morphology operator", '<g class="s"><path d="M16 32a16 16 0 0 1 28-11"/><path d="M48 19v10H38"/><path d="M48 32a16 16 0 0 1-28 11"/><path d="M16 45V35h10"/></g>'],
  close: ["Close morphology operator", '<g class="s"><path d="M48 32a16 16 0 0 0-28-11"/><path d="M16 19v10h10"/><path d="M16 32a16 16 0 0 0 28 11"/><path d="M48 45V35H38"/></g>'],
  gradient: ["Gradient operator", '<defs><linearGradient id="gradient-fill" x1="0" x2="1"><stop stop-color="#15181D"/><stop offset="1" stop-color="#F0A34A"/></linearGradient></defs><rect x="12" y="18" width="40" height="28" rx="4" fill="url(#gradient-fill)"/><path class="s" d="M12 52h40"/>'],
  "top-hat": ["Top-hat morphology operator", '<g class="s"><path d="M19 27V17h26v10M14 29h36v8H14zM24 37v10h16V37"/></g>'],

  // Chat controls and badges ------------------------------------------------
  send: ["Send message", '<path class="f" d="M8 10l48 22L8 54l8-19z"/><path d="M16 35h23" fill="none" stroke="#15181D" stroke-width="3" stroke-linecap="round"/>'],
  "stop-generation": ["Stop generation", '<circle class="s" cx="32" cy="32" r="22"/><rect class="f" x="25" y="25" width="14" height="14" rx="2"/>'],
  "new-conversation": ["New conversation", '<g class="s"><path d="M12 14h30v21H24l-8 8v-8h-4z"/><path d="M49 38v16M41 46h16"/></g>'],
  "model-llama": ["Llama model badge", '<rect class="s" x="8" y="16" width="48" height="32" rx="16"/><text x="32" y="38" text-anchor="middle" fill="currentColor" font-family="sans-serif" font-size="18" font-weight="700">L</text>'],
  "model-qwen": ["Qwen model badge", '<rect class="s" x="8" y="16" width="48" height="32" rx="16"/><text x="32" y="38" text-anchor="middle" fill="currentColor" font-family="sans-serif" font-size="18" font-weight="700">Q</text>'],
  "model-mistral": ["Mistral model badge", '<rect class="s" x="8" y="16" width="48" height="32" rx="16"/><text x="32" y="38" text-anchor="middle" fill="currentColor" font-family="sans-serif" font-size="18" font-weight="700">M</text>'],

  // Snake and shared state --------------------------------------------------
  apple: ["Apple", '<g class="s"><path d="M32 22c-6-8-17-3-17 10 0 13 8 21 17 21s17-8 17-21c0-13-11-18-17-10z"/><path d="M32 22c0-7 4-10 10-11"/></g><path class="a" d="M39 13c4-4 9-4 11-1-3 4-7 5-11 4z"/>'],
  "snake-head": ["Snake head", '<g class="s"><path d="M16 27c0-9 7-15 16-15s16 6 16 15v13c0 8-7 12-16 12s-16-4-16-12z"/><path d="M24 32h1M39 32h1M28 42h8"/></g><path class="a" d="M30 20h4v4h-4z"/>'],
  idle: ["No engine running", '<g class="s"><rect x="9" y="19" width="31" height="26" rx="5"/><path d="M20 45v5M31 45v5M15 50h20"/><path d="M16 29h17M47 21c8 3 10 10 6 16-3 5-9 5-12 2 6 0 7-5 4-8-3-3-6-2-8 0 1-6 4-9 10-10z"/></g><path class="a" d="M49 16l2 5 5 2-5 2-2 5-2-5-5-2 5-2z"/>'],
  spinner: ["Loading", '<path d="M32 10a22 22 0 1 1-16 7" fill="none" stroke="currentColor" stroke-width="5" stroke-linecap="round"/><path class="a" d="M14 10l7 1-5 5z"/>'],
  "error-offline": ["Error or offline", '<g class="s"><path d="M32 10l24 43H8z"/></g><path d="M32 25v13M32 45h.01" fill="none" stroke="currentColor" stroke-width="5" stroke-linecap="round"/>'],
  "system-deck": ["Handheld system", '<g class="s"><rect x="17" y="19" width="30" height="26" rx="4"/><path d="M17 25H9v14h8M47 25h8v14h-8"/><circle cx="13" cy="32" r="2"/><path d="M51 29v6M48 32h6"/></g>'],
  "system-apu": ["APU", '<g class="s"><rect x="19" y="19" width="26" height="26" rx="3"/><path d="M25 19v-6M32 19v-6M39 19v-6M25 45v6M32 45v6M39 45v6M19 25h-6M19 32h-6M19 39h-6M45 25h6M45 32h6M45 39h6"/></g><path class="a" d="M27 27h10v10H27z"/>'],
  "system-gpu": ["GPU", '<g class="s"><rect x="12" y="20" width="40" height="24" rx="3"/><circle cx="26" cy="32" r="7"/><path d="M52 27h4v10h-4M19 20v-5M29 20v-5M39 20v-5"/></g>'],
  wifi: ["Wi-Fi", '<g class="s"><path d="M10 25a31 31 0 0 1 44 0M17 33a21 21 0 0 1 30 0M24 41a11 11 0 0 1 16 0"/></g><circle class="a" cx="32" cy="51" r="3"/>'],
  "qr-frame": ["Scan to join frame", '<g class="s"><path d="M9 25V9h16M39 9h16v16M55 39v16H39M25 55H9V39"/></g><path class="a" d="M25 25h14v14H25z"/>'],
};

for (const [name, [title, body]] of Object.entries(icons)) {
  writeFileSync(new URL(`${name}.svg`, out), shell(name, title, body));
}

// Launcher tiles get a second, clearly active/inverted state for selected tiles.
for (const name of Object.keys(icons).filter((name) => name.startsWith("engine-"))) {
  const [title, body] = icons[name];
  writeFileSync(new URL(`${name}-active.svg`, out), shell(`${name}-active`, `${title} active`, body, true));
}
