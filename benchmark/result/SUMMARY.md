# Benchmark: Andersen vs Conditional Andersen

Phase 1: Pure analysis time (no alias printing).
Phase 2: MayAlias count for cjson (with -print-aliases).

| Program | IR lines | -ander time | -cond-ander time | slowdown |
|---------|----------|-------------|------------------|----------|
| `cjson` | 14882 | 0s | 1s | N/A |
| `bzip2` | 37523 | 1s | 2s | 2.0x |
| `zlib` | 52838 | 2s | 2s | 1.0x |
