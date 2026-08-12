#!/usr/bin/env node

import crypto from "node:crypto";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import process from "node:process";
import { spawn } from "node:child_process";
import { setTimeout as delay } from "node:timers/promises";
import { pathToFileURL } from "node:url";

export const EXPLORER_ORIGIN = "https://explorer.testnet.lez.logos.co";
export const NOT_FOUND_MARKERS = ["transaction not found", "block not found"];

export function validatePage(url, text, expected) {
  const parsed = new URL(url);
  if (parsed.origin !== EXPLORER_ORIGIN || !/^\/(transaction|block)\/[A-Za-z0-9]+$/.test(parsed.pathname)) {
    throw new Error("capture URL must be an exact official LEZ transaction or block URL");
  }
  const lowered = text.toLowerCase();
  if (NOT_FOUND_MARKERS.some((marker) => lowered.includes(marker))) {
    throw new Error("official explorer rendered a not-found page");
  }
  const missing = expected.filter((value) => !text.includes(value));
  if (missing.length) {
    throw new Error(`official explorer omitted expected rendered text: ${missing.join(", ")}`);
  }
}

export function parseArguments(argv) {
  const values = { chrome: "/usr/bin/google-chrome", timeout: 60_000, expected: [] };
  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === "--url") values.url = argv[++index];
    else if (argument === "--output-dir") values.outputDir = argv[++index];
    else if (argument === "--name") values.name = argv[++index];
    else if (argument === "--expected") values.expected.push(argv[++index]);
    else if (argument === "--chrome") values.chrome = argv[++index];
    else if (argument === "--timeout-ms") values.timeout = Number(argv[++index]);
    else throw new Error(`unknown argument: ${argument}`);
  }
  if (!values.url || !values.outputDir || !values.name || !values.expected.length) {
    throw new Error("--url, --output-dir, --name, and at least one --expected are required");
  }
  if (!/^[a-z0-9][a-z0-9-]*$/.test(values.name)) throw new Error("capture name is invalid");
  if (!Number.isSafeInteger(values.timeout) || values.timeout <= 0) throw new Error("timeout must be positive");
  validatePage(values.url, values.expected.join("\n"), values.expected);
  return values;
}

export function sha256(value) {
  return crypto.createHash("sha256").update(value).digest("hex");
}

function bounded(operation, timeout, label) {
  return Promise.race([
    operation,
    delay(timeout).then(() => { throw new Error(`DevTools command timed out: ${label}`); }),
  ]);
}

async function waitForDevtools(portFile, chrome, timeout) {
  const deadline = Date.now() + timeout;
  while (Date.now() < deadline) {
    if (fs.existsSync(portFile)) {
      const [port] = fs.readFileSync(portFile, "utf8").trim().split(/\s+/);
      const response = await fetch(`http://127.0.0.1:${port}/json/list`).catch(() => null);
      if (response?.ok) {
        const pages = await response.json();
        const page = pages.find((entry) => entry.type === "page" && entry.webSocketDebuggerUrl);
        if (page) return page.webSocketDebuggerUrl;
      }
    }
    if (chrome.exitCode !== null) throw new Error(`Chrome exited before DevTools was ready: ${chrome.exitCode}`);
    await delay(100);
  }
  throw new Error("Chrome DevTools startup timed out");
}

class Cdp {
  constructor(url) {
    this.socket = new WebSocket(url);
    this.sequence = 0;
    this.pending = new Map();
    this.events = [];
    this.socket.addEventListener("message", (event) => {
      const message = JSON.parse(event.data);
      if (message.id) {
        const operation = this.pending.get(message.id);
        this.pending.delete(message.id);
        if (message.error) operation.reject(new Error(message.error.message));
        else operation.resolve(message.result);
      } else {
        this.events.push(message);
      }
    });
  }

  async open(timeout) {
    await Promise.race([
      new Promise((resolve, reject) => {
        this.socket.addEventListener("open", resolve, { once: true });
        this.socket.addEventListener("error", reject, { once: true });
      }),
      delay(timeout).then(() => { throw new Error("DevTools WebSocket startup timed out"); }),
    ]);
  }

  call(method, params = {}) {
    const id = ++this.sequence;
    return new Promise((resolve, reject) => {
      this.pending.set(id, { resolve, reject });
      this.socket.send(JSON.stringify({ id, method, params }));
    });
  }

  async waitFor(method, timeout, predicate = () => true) {
    const deadline = Date.now() + timeout;
    while (Date.now() < deadline) {
      const index = this.events.findIndex(
        (event) => event.method === method && predicate(event.params)
      );
      if (index >= 0) return this.events.splice(index, 1)[0].params;
      await delay(50);
    }
    throw new Error(`DevTools event timed out: ${method}`);
  }

  close() {
    this.socket.close();
  }
}

async function stopChrome(chrome) {
  if (chrome.exitCode !== null) return;
  chrome.kill("SIGTERM");
  await Promise.race([
    new Promise((resolve) => chrome.once("exit", resolve)),
    delay(5_000),
  ]);
  if (chrome.exitCode === null) {
    chrome.kill("SIGKILL");
    await Promise.race([
      new Promise((resolve) => chrome.once("exit", resolve)),
      delay(5_000),
    ]);
  }
}

async function removeProfile(profile) {
  let lastError;
  for (let attempt = 0; attempt < 20; attempt += 1) {
    try {
      fs.rmSync(profile, { recursive: true, force: true, maxRetries: 3, retryDelay: 100 });
      return;
    } catch (error) {
      lastError = error;
      await delay(100);
    }
  }
  throw lastError;
}

export async function capture(options) {
  const outputDir = path.resolve(options.outputDir);
  if (fs.existsSync(outputDir)) throw new Error("output directory already exists; evidence is immutable");
  const profile = fs.mkdtempSync(path.join(os.tmpdir(), "bonded-lez-chrome-"));
  fs.chmodSync(profile, 0o700);
  const chrome = spawn(options.chrome, [
    "--headless=new",
    "--no-sandbox",
    "--disable-gpu",
    "--disable-extensions",
    "--disable-background-networking",
    "--no-first-run",
    "--no-default-browser-check",
    "--remote-debugging-port=0",
    `--user-data-dir=${profile}`,
    "about:blank",
  ], { stdio: "ignore" });
  let cdp;
  let png;
  let record;
  try {
    const socket = await waitForDevtools(path.join(profile, "DevToolsActivePort"), chrome, options.timeout);
    cdp = new Cdp(socket);
    await cdp.open(options.timeout);
    await bounded(
      Promise.all([cdp.call("Page.enable"), cdp.call("Runtime.enable"), cdp.call("Network.enable")]),
      options.timeout,
      "protocol enable",
    );
    cdp.events.length = 0;
    const navigation = await bounded(
      cdp.call("Page.navigate", { url: options.url }), options.timeout, "page navigation"
    );
    if (!navigation.errorText) {
      await cdp.waitFor(
        "Page.frameNavigated",
        options.timeout,
        ({ frame }) => frame.id === navigation.frameId && frame.url === options.url,
      );
      await cdp.waitFor("Page.loadEventFired", options.timeout);
    }
    await delay(2_000);
    const evaluation = await bounded(cdp.call("Runtime.evaluate", {
      expression: "JSON.stringify({text: document.body.innerText, ready: document.readyState, url: location.href})",
      returnByValue: true,
    }), options.timeout, "DOM evaluation");
    const rendered = JSON.parse(evaluation.result.value);
    if (rendered.ready !== "complete" || rendered.url !== options.url) {
      throw new Error("explorer navigation did not complete on the exact requested URL");
    }
    validatePage(options.url, rendered.text, options.expected);
    const metrics = await bounded(cdp.call("Page.getLayoutMetrics"), options.timeout, "layout metrics");
    const width = Math.ceil(metrics.cssContentSize.width);
    const height = Math.ceil(metrics.cssContentSize.height);
    if (width < 320 || height < 200 || width > 8_192 || height > 65_535) {
      throw new Error(`explorer content dimensions are outside capture bounds: ${width}x${height}`);
    }
    const screenshot = await bounded(cdp.call("Page.captureScreenshot", {
      format: "png",
      captureBeyondViewport: true,
      fromSurface: true,
      clip: { x: 0, y: 0, width, height, scale: 1 },
    }), options.timeout, "full-page screenshot");
    const version = await bounded(cdp.call("Browser.getVersion"), options.timeout, "browser version");
    png = Buffer.from(screenshot.data, "base64");
    record = {
      schema_version: 1,
      url: options.url,
      expected_rendered_text: options.expected,
      rendered_text_sha256: sha256(Buffer.from(rendered.text, "utf8")),
      screenshot: `${options.name}.png`,
      screenshot_sha256: sha256(png),
      screenshot_bytes: png.length,
      dimensions: { width, height },
      browser: version.product,
      browser_protocol_version: version.protocolVersion,
      fresh_profile: true,
      navigation_error: navigation.errorText || null,
      observed_at_utc: new Date().toISOString().replace(/\.\d{3}Z$/, "Z"),
    };
  } finally {
    cdp?.close();
    await stopChrome(chrome);
    await removeProfile(profile);
  }
  fs.mkdirSync(path.dirname(outputDir), { recursive: true, mode: 0o755 });
  fs.mkdirSync(outputDir, { mode: 0o755 });
  try {
    fs.writeFileSync(path.join(outputDir, record.screenshot), png, { flag: "wx", mode: 0o644 });
    fs.writeFileSync(path.join(outputDir, `${options.name}.json`), `${JSON.stringify(record, null, 2)}\n`, { flag: "wx", mode: 0o644 });
  } catch (error) {
    fs.rmSync(outputDir, { recursive: true, force: true });
    throw error;
  }
  return record;
}

if (process.argv[1] && import.meta.url === pathToFileURL(path.resolve(process.argv[1])).href) {
  try {
    console.log(JSON.stringify(await capture(parseArguments(process.argv.slice(2)))));
  } catch (error) {
    console.error(error.message);
    process.exitCode = 2;
  }
}
