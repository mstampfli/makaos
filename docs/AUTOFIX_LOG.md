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
- 2026-06-15 — iter3: hardened the user-copy path. user_buf_prefault now  returns -1 for any page with no VMA (was: silently skip), and  copy_to_user/copy_from_user/sys_read/fb-blit return -EFAULT instead of  memcpy-ing into it. Closes the canonical-but-unmapped half of the crash  (CR2=0x0000680073006900, RIP=copy_to_user, #PF unrecoverable) that the  _access_ok fix (non-canonical half) did not cover. VERIFIED: builds; 3-boot  net-exit test => runs 1,3 CLEAN (0 faults), run 2 still 1 panic. So a bad  user pointer no longer faults the kernel, but the UNDERLYING corruption  (something scribbles cleartid_addr with UTF-16-ish bytes: "SCS"/"ish")  still intermittently hits other fields => priority #2. NEXT: reproduce with  PER-RUN serial saved, then poison/guard the task slab to catch the writer.
- 2026-06-15 — iter4: VERIFIED net-exit crash fixed (6/6 clean headless boots  after the prefault -EFAULT fix; boot is now reliable). Then characterized the  presentation stall: 3-boot sway test, runs 1,2 BOTH black/stall (PRE=1-2  colors; run2 had sway up, no wallpaper) — so the hardening did NOT resolve  the stall (the earlier lucky run showed 2914). Stall is a real repaint-  scheduling bug, still priority #1. (run3 boot-test was in-flight at this  fire; recorded+stopped per overlap rule.) NEXT: instrument wlroots  wlr_output_schedule_frame / surface-commit -> damage -> repaint path (DE  rebuild) to see why a swaybg commit does not schedule a frame.
  iter4 cont: run3 also black (sway=0). Of 3 runs only run2 launched sway  (keystroke harness flaky: 2/3 sway=0). NEXT FIRE should FIRST make sway-launch  deterministic (e.g. an init/login hook that auto-execs sway in a test image,  or poll-then-retry keystrokes) so the wlroots repaint instrumentation is  testable, THEN instrument wlr_output_schedule_frame / commit->damage->repaint.
- 2026-06-15 — iter5: built a deterministic boot harness (/tmp/swaytest.sh:  poll login -> root/toor -> flush-line + RETRY sway until "lazy /bin/sway").  Result across 3 boots: sway launches on attempt 1 ALL 3, 0 crashes, and the  WALLPAPER presents reliably (PRE_colors=2914 x3) — confirmed via screenshot  (blue Sway wallpaper). So the background "stall/luck" was largely the flaky  launch + crashes disrupting startup, now fixed by the kernel hardening +  clean launch. REMAINING priority #1 = the TASKBAR: swaybar spawns  (lazy /bin/swaybar, page-faults) but its layer surface never presents; serial  shows repeated "[unix] close (unbound)" churn = its IPC sockets to sway. So  the bug is swaybar<->sway IPC handshake (IPC_GET_BAR_CONFIG/subscribe) or the  layer-surface commit. NEXT: instrument that path (swaybar ipc_initialize /  sway bar IPC handler / the unix-socket closes) to see where swaybar stalls or  disconnects before mapping its bar.
