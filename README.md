# Continuum OS — Reference Implementation

A real, buildable, tested implementation of the architecture described in
**ARCH-001** (Continuum OS Architecture) and **ARCH-002** (Continuum
Reference Build): a distributed operating system whose only job is
running LLM inference across a cluster of cheap boards, behind an
OpenAI-compatible API.

Every daemon in ARCH-002's component catalog is implemented in C, builds
cleanly on Ubuntu 26.04, and is exercised by a real end-to-end test that
spawns the actual compiled binaries (not mocks) and talks to them over
the real wire protocol. As of this writing: **9,000 lines of our own C**
(excluding vendored dependencies), **20/20 tests passing**, including one
that drives a real HTTP request all the way from `POST /v1/chat/completions`
through the scheduler and swap-paged tensor loading to a generated token
stream and back.

## Quick start

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

To see the whole pipeline run against a real (if tiny and untrained) toy
model, see [Running the demo](#running-the-demo) below.

## What's really here

| Component | ARCH-002 role | Status |
|---|---|---|
| `kernel/continuumd` | The Continuum Kernel — PID 1, process supervision, cgroup placement, boot-processor heartbeat | Built, tested |
| `services/node-agentd` | Local DRAM-pool bookkeeping, remote page serving, health telemetry | Built, e2e tested |
| `services/swapd` | Demand paging — real `userfaultfd` + documented fallback, shared DRAM pool, LRU eviction | Built, e2e tested |
| `services/shard-execd` | Model loading through swapd, toy transformer forward/decode loop | Built, e2e tested |
| `services/schedulerd` | Routes by model name, relays streamed tokens, **TCP to compute nodes** | Built, e2e tested |
| `services/api-gatewayd` | OpenAI-compatible HTTP: `/v1/chat/completions` (streaming + non-streaming), `/v1/models` | Built, e2e tested |
| `services/page-directoryd` | Cluster-wide page directory, replicated via Raft | Built, e2e tested (real 3-node cluster) |
| `services/membershipd` | Control-plane membership, replicated via Raft | Built, e2e tested (real 3-node cluster) |
| `services/consoled` | Operator console — HTTP + polled JSON telemetry | Built, e2e tested |
| `firmware/boot_processor` | RP2040 boot-processor firmware | Written, **not compiled** (no Pico SDK/toolchain in this environment) |
| `image/` | Buildroot defconfig, kernel config fragment, per-role config templates | Written, **not run** (no Buildroot checkout attempted — see below) |

Foundational libraries (`lib/`): `wire` (the framed binary protocol),
`kvcache` (prefix-cache trie), `pagetable` + `pageclient` (the shared
DRAM pool), `raftnet` (wires vendored `raft.c` to the wire protocol),
`raftkv` (a replicated KV store built on `raftnet` — what page-directoryd
and membershipd both actually are), `toymodel` (the weight-file format).

Vendored, real dependencies (`third_party/`, ~1.7MB total): `raft.c`
(willemt/raft, BSD), `picohttpparser` (MIT), `jsmn` (MIT). `civetweb` was
also vendored for `consoled` per ARCH-002, but its cloned HEAD commit
turned out to have a genuine syntax error in its request parser — see
`services/consoled/src/main.c`'s module comment for what replaced it.

## Running the demo

```bash
# 1. Generate a toy model (byte-level vocab, one attention block — see
#    lib/toymodel/include/toymodel_format.h for why it's a toy, not a
#    real checkpoint format).
./build/tools/gen_toy_weights /tmp/demo.weights

# 2. Start swapd (owns the tensor region, demand-pages from the file above)
cat > /tmp/swapd.conf <<EOF
node_id 1
weight_file /tmp/demo.weights
tensor_region_bytes $(stat -c%s /tmp/demo.weights)
page_size 4096
dram_capacity_pages $(($(stat -c%s /tmp/demo.weights) / 4096))
unix_socket /tmp/swapd.sock
EOF
./build/services/swapd/swapd --config /tmp/swapd.conf &

# 3. Start shard-execd (loads the model THROUGH swapd, one page at a time)
cat > /tmp/shard.conf <<EOF
node_id 1
weight_file /tmp/demo.weights
page_size 4096
swapd_unix_socket /tmp/swapd.sock
listen_port 7301
EOF
./build/services/shard-execd/shard-execd --config /tmp/shard.conf &

# 4. Start schedulerd (routes by model name to shard-execd over TCP)
cat > /tmp/sched.conf <<EOF
listen_unix_socket /tmp/sched.sock
route toy-model 127.0.0.1:7301
EOF
./build/services/schedulerd/schedulerd --config /tmp/sched.conf &

# 5. Start api-gatewayd (the OpenAI-compatible endpoint)
cat > /tmp/gw.conf <<EOF
listen_port 8080
scheduler_unix_socket /tmp/sched.sock
model toy-model
EOF
./build/services/api-gatewayd/api-gatewayd --config /tmp/gw.conf &

sleep 1

# 6. Talk to it exactly like the OpenAI API
curl -s http://localhost:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"toy-model","messages":[{"role":"user","content":"Hi"}],"max_tokens":16,"temperature":0}'
```

The response is a genuine `chat.completion` JSON object. The *content*
will be gibberish bytes — this is an untrained, randomly-initialized toy
model (see the scope note below) — but every hop before that (HTTP
parsing, JSON, routing, TCP to a compute node, swap-paged tensor
loading, the forward pass, token streaming) is real. Kill the four
background processes when done (`kill %1 %2 %3 %4`), and see
`services/*/test/*_e2e_test.c` for the same flow driven automatically.

## Scope decisions (read this before assuming a gap is a bug)

This reference build makes a number of deliberate, documented
simplifications relative to ARCH-001/ARCH-002. Each is explained in
detail in the relevant source file's module comment; this is the index:

- **No real tokenizer / no real model.** "Tokens" are raw bytes
  (`lib/toymodel/include/toymodel_format.h`); the model is a
  single-layer, randomly-initialized toy transformer, not a trained
  checkpoint. This build proves the *architecture* — paging, wire
  protocol, consensus, HTTP surface — not inference quality.
- **No pipeline-parallel layer splitting.** Each shard-execd fully owns
  its model end to end; ARCH-001's activation-hop pipelining across
  compute nodes is not implemented (see `services/shard-execd/src/main.c`).
  What *is* distributed: independent models/shards can run on independent
  nodes behind the scheduler, and paging/consensus/wire protocol are all
  genuinely network-capable.
- **`userfaultfd` has a documented fallback.** Unprivileged `userfaultfd`
  is disabled on the machine this was built on
  (`vm.unprivileged_userfaultfd=0`, no root). `services/swapd/src/paging_engine.c`
  implements the real mechanism and automatically falls back to explicit
  page-in when the kernel refuses — same observable behavior, different
  mechanism. Verify which mode is active from a running swapd's startup log.
- **KV cache stays in shard-execd's own memory**, not routed through
  swapd's paged tensor region (`services/shard-execd/include/toy_model.h`).
  `lib/kvcache`'s prefix-trie *is* wired in and exercised for prefix
  dedup, just not for the tier-aware KV swapping ARCH-001 §09 describes.
- **No durable Raft persistence.** `lib/raftnet` keeps the log
  purely in-memory (`lib/raftnet/include/raftnet.h`'s module comment) —
  correct for consensus, but a full quorum restart loses history.
- **io_uring is a synchronous `pread()` for now.** ARCH-002 names
  io_uring for swapd's local NVMe reads; `paging_engine.c` uses `pread()`
  instead. The demand-paging *mechanism* (userfaultfd) is real; this is
  purely an I/O-backend choice, swappable later without touching the
  fault-handling logic.
- **`civetweb` → `picohttpparser`.** See `services/consoled/src/main.c`.
- **The boot processor and Buildroot image are unverified.** Neither
  a Pico SDK/`arm-none-eabi-gcc` toolchain nor a Buildroot checkout was
  available in the environment this was built in. Both are real, complete
  source/config (see `firmware/boot_processor/` and `image/`), just never
  compiled or run.

## Real bugs found and fixed while building this

Worth knowing about, since they're the kind of thing that's easy to miss
without exactly this kind of end-to-end testing:

1. **A permanent Raft split-vote livelock.** `rand()` was never seeded
   anywhere in the codebase. glibc's `rand()` with no `srand()` call is a
   fixed, deterministic sequence — identical in every process. Sibling
   nodes forked back-to-back (this repo's own multi-process e2e tests,
   and any real process-manager-driven cluster startup) picked the exact
   same "random" election timeout every round, forever. Fixed once in
   `raftnet_start()` with a `getpid()`-differentiated seed
   (`lib/raftnet/src/raftnet.c`). Found because `pagedir_e2e_test` timed
   out consistently under `ctest` (multi-process) but passed instantly
   run standalone (a shell script starting each node with enough
   scheduling jitter to accidentally break the tie) — the discrepancy
   was the clue.
2. **A blocking-fd `timerfd` drain-loop hang.** `node-agentd` and `swapd`
   created their tick timers without `TFD_NONBLOCK`, but used a
   `while (read(tickfd,...)==8) {}` drain loop that only terminates
   correctly on a non-blocking fd. On a blocking one, the second `read()`
   blocks until the *next* tick, forever — health reports and swap
   eviction sweeps silently never ran. `kernel/continuumd` had the right
   flag already; the other two didn't. Found via `console_e2e_test`
   never seeing any node telemetry arrive.
3. **A jsmn object-skip bug.** jsmn marks an object *key* string's `size`
   field as 1 ("one value follows") — a hand-rolled recursive token-skip
   that didn't special-case object keys double-counted and walked past
   the wrong tokens, silently returning the wrong field for anything
   after the first key in a JSON object. Fixed in
   `services/api-gatewayd/src/json_util.c`. Found by `json_util_test`.
4. **A non-blocking-listen-socket bug in shard-execd.** `sock_unix_listen()`
   (shared by every daemon) sets `O_NONBLOCK`; the epoll-driven daemons
   need that, but shard-execd's simple single-threaded `accept()` loop
   didn't handle `EAGAIN` — it silently exited right after logging
   "ready." Found by `shard_execd_e2e_test`.
5. **schedulerd routed to shard-execd over a Unix socket** — which can
   never work once those two are on different physical boards, exactly
   the deployment ARCH-002 §01 describes. See the commit that switched
   this to TCP for the full explanation.

## Build system notes

- **Language/toolchain**: C17, targeting glibc on this dev machine
  (`CONTINUUM_STATIC=OFF` by default). ARCH-002 §02 specifies musl +
  static linking for the actual Pi 5 target; the CMake is structured for
  that (`CONTINUUM_STATIC` option, `-mcpu=cortex-a76` auto-applied when
  `CMAKE_SYSTEM_PROCESSOR` is aarch64) but this repo has only been
  exercised with a native x86_64/glibc toolchain — cross-compiling to
  aarch64-musl is unverified.
- **`add_subdirectory` order matters**: `tools/` is added before
  `services/` in the top-level `CMakeLists.txt` specifically so that
  `if(TARGET gen_toy_weights)`-style guards in a service's tests see it
  already defined — a generator expression like `$<TARGET_FILE:x>`
  resolves fine regardless of order, but `if(TARGET x)` does not.
- **Test hygiene**: every multi-process e2e test derives its ports from
  `getpid()` and cleans up its spawned children on both success *and*
  failure (`cleanup_and_exit()` — see `pagedir_e2e_test.c`), so a test
  killed by a timeout can't leave orphaned processes squatting on fixed
  ports and cascading into the next run's failure. Run tests with a
  clean `/dev/shm/continuum-*` and no stray `swapd`/`node-agentd`/etc.
  processes if you're debugging by hand — see the git history around the
  Raft livelock fix for what that cascade looks like when it isn't clean.

## Directory layout

```
continuum-os/
├── kernel/           continuumd — the Continuum Kernel, PID 1
├── firmware/         boot_processor — RP2040, Pico SDK, separate build
├── services/         one directory per daemon, each with include/ src/ test/
├── lib/              shared libraries (wire, kvcache, pagetable, raftnet, raftkv, ...)
├── protocols/        wire-frame payload structs shared across service boundaries
├── third_party/      vendored raft.c, picohttpparser, jsmn (real deps, MIT/BSD)
├── tools/            gen_toy_weights
├── image/            Buildroot defconfig, kernel config fragment, config templates
└── CMakeLists.txt    top-level super-build
```
