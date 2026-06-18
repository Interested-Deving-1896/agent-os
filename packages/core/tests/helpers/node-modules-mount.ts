import { join } from "node:path";
import { nodeModulesMount } from "../../src/index.js";

/**
 * Test helper: build the `mounts` entry that exposes `<cwd>/node_modules` at
 * `/root/node_modules` in the VM. This is the explicit, mount-based way to make
 * a host node_modules tree resolvable in the guest; the resolver reads the
 * mounted tree through the kernel VFS.
 */
export function moduleAccessMounts(cwd: string) {
	return [nodeModulesMount(join(cwd, "node_modules"))];
}
