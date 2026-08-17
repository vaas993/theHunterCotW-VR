#pragma once

// Hardware data breakpoints - restored from the crouch-ratchet hunt (the
// instrument that found in one screenshot what eleven hook probes missed),
// with the two lessons that campaign paid for baked in:
//
//   1. WRITE-ONLY (RW=01). The old RW=11 counted the per-frame READS and
//      drowned the writers - "38,206 accesses, identical rates" was reads.
//   2. Armed on ALL THREADS. Debug registers are per-thread and the writer's
//      thread is unknown ahead of time; arming only the caller's thread reads
//      as "nobody writes it" when the writer lives elsewhere.
//
// Current use: find WHO COMPOSES the view-projection matrix (the jitter hunt,
// DLSS_IMPLEMENTATION.md 7e). This costs one CPU exception per write to the
// watched addresses - a measurement, not a mode to play in.

namespace cotwvr {

// Install the vectored exception handler. Safe to call repeatedly.
bool HwbpInit();

// Watch this address (4 bytes, 4-aligned) for WRITES, on every thread of the
// process. Up to two addresses (DR0/DR1).
void HwbpWatch(void* addr, const char* what);

// Periodic report: every distinct writing instruction, module-relative, with
// counts. Call once per frame; it rate-limits itself.
void HwbpReport();

}  // namespace cotwvr
