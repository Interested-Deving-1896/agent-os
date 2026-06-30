import { controlFamily } from "./control.js";
import { dnsFamily } from "./dns.js";
import { fsFamily } from "./fs.js";
import { netFamily } from "./net.js";
import { perfFindingsFamily } from "./perf-findings.js";
import { pipesFamily } from "./pipes.js";
import { processFamily } from "./process.js";

export const allFamilies = [
	processFamily,
	netFamily,
	fsFamily,
	dnsFamily,
	pipesFamily,
	controlFamily,
	perfFindingsFamily,
];

export const allOps = allFamilies.flat();
