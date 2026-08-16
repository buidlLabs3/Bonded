#!/usr/bin/env node

import assert from "node:assert/strict";
import fs from "node:fs";
import os from "node:os";
import path from "node:path";
import test from "node:test";

import {
  capturePaths,
  parseArguments,
  sha256,
  validatePage,
  waitForRenderedPage,
} from "../tools/lez_explorer_capture.mjs";

const tx = "fc88b2bad2b51026fb97c6cc8b4943ead59f8a3cc0e515f9f058f9e49fb11ea9";

test("rendered explorer pages require exact official URLs and text", () => {
  const url = `https://explorer.testnet.lez.logos.co/transaction/${tx}`;
  validatePage(url, `${tx}\nProgram Deployment Transaction\n368324 bytes`, [tx, "368324 bytes"]);
  assert.throws(() => validatePage("https://example.com/transaction/x", "x", ["x"]), /exact official/);
  assert.throws(() => validatePage(url, "Transaction not found", [tx]), /not-found/);
  assert.throws(() => validatePage(url, "loading", [tx]), /omitted/);
});

test("arguments and hashes are deterministic and fail closed", () => {
  const url = `https://explorer.testnet.lez.logos.co/transaction/${tx}`;
  const parsed = parseArguments([
    "--url", url,
    "--output-dir", "/tmp/capture",
    "--name", "deployment-transaction",
    "--expected", tx,
  ]);
  assert.equal(parsed.url, url);
  assert.deepEqual(parsed.expected, [tx]);
  assert.equal(parsed.timeout, 60_000);
  assert.equal(sha256(Buffer.from("abc")), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
  assert.throws(() => parseArguments(["--url", url]), /required/);
  assert.throws(() => parseArguments([
    "--url", url,
    "--output-dir", "/tmp/capture",
    "--name", "../escape",
    "--expected", tx,
  ]), /name/);
});

test("distinct immutable captures may share one evidence directory", () => {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "bonded-capture-test-"));
  try {
    const first = capturePaths(root, "transaction");
    fs.writeFileSync(first.png, "first");
    fs.writeFileSync(first.sidecar, "{}");
    const second = capturePaths(root, "block");
    assert.equal(second.directory, root);
    assert.throws(() => capturePaths(root, "transaction"), /immutable/);

    const symlink = `${root}-link`;
    fs.symlinkSync(root, symlink);
    try {
      assert.throws(() => capturePaths(symlink, "other"), /real directory/);
    } finally {
      fs.unlinkSync(symlink);
    }
  } finally {
    fs.rmSync(root, { recursive: true, force: true });
  }
});

test("render readiness uses exact DOM state instead of a load event", async () => {
  const url = `https://explorer.testnet.lez.logos.co/transaction/${tx}`;
  const states = [
    { text: "Loading", ready: "interactive", url },
    { text: `${tx}\nProgram Deployment Transaction`, ready: "complete", url },
  ];
  const cdp = {
    async call(method) {
      assert.equal(method, "Runtime.evaluate");
      return { result: { value: JSON.stringify(states.shift()) } };
    },
  };
  const rendered = await waitForRenderedPage(cdp, {
    url,
    expected: [tx, "Program Deployment Transaction"],
    timeout: 5_000,
  });
  assert.equal(rendered.ready, "complete");
  assert.equal(states.length, 0);
});
