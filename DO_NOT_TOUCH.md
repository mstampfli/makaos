# MakaOS Development Rules

## File reading
Always use Bash to read files. Never use the Read or Explore tools — all kernel source files are too large and will cause an internal error.

## Running builds
Always run builds (`./build.sh`, `./run.sh`, `scripts/build-toolchain.sh`, any long compilation) with `run_in_background: true` and attach the `Monitor` tool to stream stdout. Never run a build as a foreground Bash call with a long timeout — it blocks the conversation and wastes tokens on tail output.

## Code quality — non-negotiable
Every solution must meet all of the following requirements without exception. There are no shortcuts, no "we can fix this later", no temporary hacks:

- **Scalable**: must work correctly at Linux-scale (thousands of processes, connections, file descriptors, IRQs). If it breaks at 64 entries or 4095 PIDs, it is wrong.
- **Extensible**: adding a new device, syscall, subsystem, or driver must not require editing unrelated monolithic code. Unless a refactor has many benefits such as scalability and performance
- **Performant**: hot paths (syscall dispatch, scheduler, network receive, IRQ handling) must be O(1) or O(log n). No O(n) linear scans in hot paths. No busy-wait polling in production code.
- **Efficient**: no wasted memory from oversized static arrays, no redundant work (e.g. double path lookups), no per-character copies where memcpy applies.
- **Correct and safe**: all userspace pointers must be validated before dereference. No static buffers inside functions that are called from multiple tasks. No silent truncation of paths, PIDs, or counts.

If the correct solution is more complex than a quick hack, implement the correct solution. Do not implement the hack and leave a TODO. This kernel competes with Linux — design accordingly.

## Performance is paramount
Performance is the single most important constraint. Every implementation must be optimized for maximum throughput, minimum latency, and zero unnecessary overhead. Never make compromises — always implement the perfect solution for a large-scale OS competing with Linux. No "good enough" implementations, no deferred optimizations, no acceptable regressions. If a design choice trades performance for convenience, choose performance.
