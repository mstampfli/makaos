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
- 2026-06-15 — iter2: ROOT-CAUSED the boot-time #GP that blocked login. The  `net` service does socket(AF_INET) which returns -ENETDOWN when no net dev  is present, so net exits; sys_exit -> task_notify_cleartid -> copy_to_user  on a bad (non-canonical) cleartid_addr #GP'd the KERNEL (RIP in copy_to_user,  CS=0x08). Cause: _access_ok only rejected addr>=HHDM_OFFSET, letting  non-canonical user pointers (the inter-half gap, e.g. 0x0053004300530000)  through -> memcpy #GP instead of -EFAULT. FIX: _access_ok rejects anything  above the canonical user ceiling 0x00007FFFFFFFFFFF. VERIFIED: boot #GP gone,  root/toor login works, sway+swaybg launch, 0 crashes. Also fixes the slab  corruption #GP (same copy_to_user path).  Bonus (priority #1 data): flip trace shows flipQUEUED==flipREAD==3 — sway  READS every page-flip-complete, so event delivery is fine; the presentation  stall is in REPAINT SCHEDULING after a client commit (swaybg commits the  wallpaper but no repaint is scheduled). NEXT: trace wlroots' damage/  schedule_frame path after a surface commit. NOTE: the bad cleartid_addr  itself (corruption vs net setting it) is a separate latent bug to chase;  _access_ok now makes it non-fatal regardless.
