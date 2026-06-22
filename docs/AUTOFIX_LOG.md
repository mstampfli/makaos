# MakaOS autonomous fix log

Branch: `autofix` (off `main` @ b00dfe2). The loop never touches `main`; the
user reviews this branch and merges what they trust. Each cron fire appends a
dated one-line note below for continuity.

## Backlog (priority order)
1. **Desktop presentation stall** — taskbar/background don't reliably present;
   "first frame black until input forces a recomposite." Event-delivery layer
   already exonerated (epoll level-triggered, DRM flip-complete delivered,
   AF_UNIX wakes both waitqs). Suspect wlroots repaint/damage scheduling.
2. **slab_test memory corruption** — a kthread exits with a runtime-scribbled
   `cleartid_addr` (e.g. 0x53004300530000) → copy_to_user #GP → RCU stall.
   Self-tests gated off (`MAKAOS_BOOT_SELFTESTS`) until fixed.
3. Crashes/correctness found by exercising the OS.

## Method
hypothesis → instrument/fix → `bash build.sh` → headless boot-test → commit to
`autofix` only if build+boot clean → revert regressions. Never leave `autofix`
broken.

## Iterations
- 2026-06-15 — iter1: started on the presentation stall. Added DRM repaint-loop
  trace (`flipev QUEUED` when a page-flip-complete event is queued, `flipev READ`
  when sway reads it) to drm.c. Goal: count QUEUED vs READ vs PAGE_FLIP after a
  client commits — if READ==QUEUED but flips then stop, the bug is repaint
  scheduling, not event delivery. Building + boot-observing next.
