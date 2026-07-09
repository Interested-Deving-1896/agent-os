import { readFileSync } from "node:fs";
import { fileURLToPath } from "node:url";
import { describe, expect, it } from "vitest";

const sourcePath = fileURLToPath(
	new URL("../src/test-runtime.ts", import.meta.url),
);

describe("native sidecar build invalidation", () => {
	it("tracks every local crate that can change the native sidecar binary", () => {
		const source = readFileSync(sourcePath, "utf8");
		for (const crate of [
			"bridge",
			"build-support",
			"execution",
			"kernel",
			"native-sidecar",
			"native-sidecar-core",
			"sidecar-protocol",
			"v8-runtime",
			"vfs",
			"vm-config",
		]) {
			expect(source).toContain(`path.join(REPO_ROOT, "crates/${crate}")`);
		}
		for (const input of [
			"packages/build-tools/bridge-src",
			"packages/build-tools/package.json",
			"packages/build-tools/scripts/build-v8-bridge.mjs",
			"packages/core/fixtures/base-filesystem.json",
			"packages/runtime-core/fixtures/base-filesystem.json",
			"pnpm-lock.yaml",
		]) {
			expect(source).toContain(`path.join(REPO_ROOT, "${input}")`);
		}
		expect(source).not.toContain('path.join(REPO_ROOT, "crates/sidecar")');
	});
});
