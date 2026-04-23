# Code Style

Rules for how code is formatted, named, and laid out. This document is
about mechanics, not architecture — for architectural decisions see
`PRINCIPLES.md`.

These are **strong defaults**. Deviations require an explicit comment
explaining why, in the code itself.

---

## 1. C

### 1.1 Indentation and whitespace

- **8-space indentation.** Use actual spaces, not tabs. Deep nesting is
  a signal that the function is doing too much — the wide indent makes
  that signal impossible to ignore.
- **No trailing whitespace.** Ever.
- **One blank line** between logical blocks inside a function. **Two
  blank lines** between top-level definitions.
- **No blank line** immediately after an opening brace or before a
  closing brace.

### 1.2 Line length

- **100 columns hard limit.**
- Break long function signatures at argument boundaries, aligning
  continuations to just past the opening paren.
- Long string literals may exceed 100 columns if splitting would hurt
  grep-ability (e.g., error messages you want to search for).

### 1.3 Braces (K&R)

- Opening brace on the **same line** as the control statement.
- Closing brace on its own line.
- **Functions are the exception**: opening brace goes on its own line
  (classic K&R, matching Linux kernel style).

```c
int do_work(int n)
{
        if (n < 0) {
                return -EINVAL;
        }

        for (int i = 0; i < n; i++) {
                step(i);
        }
        return 0;
}
```

- **Always use braces**, even for single-statement bodies. No exceptions.

### 1.4 Naming

| Kind              | Convention        | Example                |
| ----------------- | ----------------- | ---------------------- |
| Functions         | `snake_case`      | `process_queue`        |
| Local variables   | `snake_case`      | `entry_count`          |
| Struct/union/enum | `snake_case`      | `struct task_entry`    |
| Typedefs          | `snake_case_t`    | `pid_hash_t`           |
| Macros            | `SCREAMING_SNAKE` | `KPATH_MAX`            |
| Constants (enum)  | `SCREAMING_SNAKE` | `FLAG_READY`           |
| File-static       | `s_` prefix       | `s_pid_ht`             |
| File-local funcs  | `static` + snake  | `static void helper()` |

- **No Hungarian notation.** The type system already tells you the type.
- **Names describe intent, not implementation.** `find_task` is better
  than `hashmap_lookup` as a public API.
- **Abbreviations**: use only if universally understood in the domain
  (`buf`, `len`, `ctx`, `fd`, `irq`, `pid`). Never invent new ones.
- **Avoid `get_`/`set_` for trivial accessors** — `task_pid(t)` not
  `task_get_pid(t)`.

### 1.5 Header guards

Use `#pragma once`. It is supported by every compiler we target. Do not
mix it with `#ifndef` guards.

### 1.6 Include order

In `.c` files, include in this order with a blank line between each
group:

1. The corresponding `.h` file (if any) — proves the header is
   self-contained.
2. Standard library headers (`<stddef.h>`, `<stdint.h>`, ...).
3. Project public headers (`<kernel/foo.h>`).
4. Project private headers (`"internal.h"`).

### 1.7 Commenting

- **Doc comment on every public function.** Format:

  ```c
  /*
   * Look up a task by PID.
   *
   * Returns the task pointer on success, or NULL if no task with the
   * given PID exists. Does not take a reference on the returned task.
   *
   * Context: any. Does not sleep. Caller must hold the tasklist lock
   * if they intend to dereference the result.
   */
  struct task *task_find(pid_t pid);
  ```

- **Context documentation is required for critical paths** (anything in
  the IRQ/scheduler/MM hot paths, or anything whose locking rules are
  non-obvious).
- **Inline comments explain _why_, not _what_.** The code shows what.

  ```c
  /* Bad */
  i++;  /* increment i */

  /* Good */
  i++;  /* skip the sentinel entry at index 0 */
  ```

- **No dead code.** Delete it. Git remembers.
- **`TODO(context)`** — always include enough context to act on it.
  `TODO: fix this` is not acceptable; `TODO: handle ENOMEM path once
  the slab allocator lands` is.

### 1.8 Function size

- **Soft target: fits on one screen** (~50 lines).
- If you need more, the function is probably doing more than one thing.
  Split it — unless splitting would hurt performance on a hot path, in
  which case leave a comment explaining why.

### 1.9 Structs

- Group related fields. Order by access pattern, not alphabetically.
- Put hot fields (read/written on every call) at the front to maximize
  cache-line locality.
- Pad explicitly when it matters; do not rely on the compiler's
  natural padding for anything you depend on.

### 1.10 Types

- **Use fixed-width types** from `<stdint.h>` (`uint32_t`, `int64_t`)
  for anything where size matters.
- **Use `size_t`** for sizes and counts of in-memory objects.
- **Use `ssize_t`** when you need a signed size (e.g., read/write
  return values).
- **`bool`** from `<stdbool.h>`, not `int`-as-boolean, except where the
  kernel ABI demands otherwise.

### 1.11 Error handling

- **Return negative errno values** on failure: `-EINVAL`, `-ENOMEM`,
  `-EACCES`. Return `0` or a non-negative value on success.
- **Never return raw numeric codes** like `-1` or `-42`.
- **Cleanup via `goto` labels** is encouraged for resource
  acquisition/release. Early return is preferred for pure validation
  before any resource is acquired.

  ```c
  int setup(void)
  {
          int err;

          if (bad_input())
                  return -EINVAL;       /* early return — no resources yet */

          err = acquire_a();
          if (err)
                  return err;

          err = acquire_b();
          if (err)
                  goto fail_b;

          err = acquire_c();
          if (err)
                  goto fail_c;

          return 0;

  fail_c:
          release_b();
  fail_b:
          release_a();
          return err;
  }
  ```

- **Label names** describe what failed, not what they clean up:
  `fail_b` (meaning "we got past step b, then failed").

### 1.12 Anti-patterns (do NOT do these)

- **Nested ternaries.** Use `if`/`else`.
- **`goto` for non-cleanup control flow.** Only for error/cleanup
  paths.
- **Magic numbers.** Use named constants or enums.
- **Variable-length arrays (VLAs) on the stack** in kernel code. Use
  bounded stack buffers or heap allocation.
- **`strcpy`, `sprintf`, `gets`.** Use bounded versions.
- **Long boolean expressions without parens.** Parens are free.
- **Mixing tabs and spaces.** Spaces only.
- **Commenting out code instead of deleting it.**
- **Catch-all `void *` arguments** when a concrete type would work.

---

## 2. NASM / x86 assembly

### 2.1 Syntax

- **Intel syntax** (NASM native). No AT&T.
- **Lowercase mnemonics and registers**: `mov rax, rbx`, not
  `MOV RAX, RBX`.
- **Uppercase for symbolic constants and macros** defined via `%define`
  or `equ`.

### 2.2 Indentation and columns

- **8 spaces** for the instruction column (matches C indent).
- **Operand column** aligned at ~16 chars from line start when
  practical.
- **Comment column** at ~40 chars.

```nasm
        mov     rax, [rsp + 8]          ; argv
        test    rax, rax
        jz      .no_args
```

### 2.3 Labels

- **Global labels**: `snake_case` matching the C convention.
- **Local labels**: `.snake_case`, leading dot (NASM local-label
  syntax).
- **One instruction per line.**

### 2.4 Commenting

- **Every non-trivial block** gets a comment explaining the intent.
- **Register usage contract** at the top of every function: which
  registers are inputs, which are clobbered, which hold the return
  value.

  ```nasm
  ; uint64_t read_msr(uint32_t msr)
  ;   In:  edi = MSR index
  ;   Out: rax = value
  ;   Clobbers: rcx, rdx
  read_msr:
          mov     ecx, edi
          rdmsr
          shl     rdx, 32
          or      rax, rdx
          ret
  ```

### 2.5 Anti-patterns

- **Uppercase mnemonics** (`MOV`, `PUSH`). Lowercase only.
- **Mixing Intel and AT&T** in the same project.
- **Undocumented register clobbers.**
- **Magic hex constants** without a name or comment (`0x80000001` →
  `CPUID_EXT_FEATURE`).

---

## 3. Files and directories

### 3.1 File names

- **Lowercase, snake_case**: `page_alloc.c`, `virtio_blk.h`.
- **Match the primary type or subsystem** the file defines.
- **One subsystem per `.c`/`.h` pair** is the default. Split into
  multiple files only when complexity justifies it (rule of thumb: a
  single file exceeding ~1500 lines is probably too much).

### 3.2 Directory layout

- Group files by **subsystem**, not by file kind. `kernel/mm/` is
  right; `kernel/headers/` is wrong.
- A subsystem's private headers stay in its directory. Only headers
  meant to be used by other subsystems go into the shared include path.

### 3.3 Build artifacts

- **Never commit build artifacts.** `.o`, `.bin`, `.iso`, and `build/`
  are gitignored.

---

## 4. Git hygiene

- **Feature branches**, merged when complete and tested.
- **Branch naming**: `<kind>/<short-desc>`, e.g., `feat/svcmgr-dag`,
  `fix/pipe-sigpipe`, `refactor/unveil-inline`.
- **Commit messages**:
  - Subject line ≤ 72 characters, imperative mood ("add X", not
    "added X" or "adds X").
  - Blank line, then a body explaining _why_ (not what — the diff shows
    what).
  - Reference issues/tickets when applicable.
- **Atomic commits**: one logical change per commit. A commit that
  mixes a bug fix and an unrelated refactor gets rejected.

---

## 5. Summary checklist

Before submitting a change, verify:

- [ ] 8-space indent, K&R braces, 100-col limit.
- [ ] Naming matches the table in §1.4.
- [ ] `#pragma once` in every header.
- [ ] Includes ordered per §1.6.
- [ ] Doc comment on every public function.
- [ ] Error paths return `-errno`.
- [ ] `goto` used only for cleanup.
- [ ] No dead/commented-out code.
- [ ] No anti-patterns from §1.12 or §2.5.
- [ ] Commit message is atomic and in imperative mood.
