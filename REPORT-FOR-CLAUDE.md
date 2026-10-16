# konica-native — status & handoff report

*Audience: Claude (or any AI/human picking this up cold). This is a complete, self-contained
snapshot of the project state, the evidence behind every claim, and the open items.*

Generated: 2026-10-16
Runbook in play: `/home/spot/Downloads/KONICA-NATIVE-INSTRUCTIONS(3).md`

---

## 1. What the project is

`konica-native` is a **dependency-free, pure-libc C reimplementation** of the vendor CUPS filter
`245igdirf` shipped with the **KONICA MINOLTA 206** (245i family) driver package.

- It replaces the vendor's closed `Filters/245igdirf` in the CUPS raster chain.
- It must be **byte-identical** to the vendor filter given the same raster input.
- It must build and run on **both glibc and musl**, using only libc (no third-party encoder libs).
- Vendor runtime data (`.hft` halftone tables, LUTs, `.ocm`, `.icm` ICC profiles) is **loaded at
  runtime from the default install path** — it is **never** embedded, copied into the repo, or
  committed.

Repository: **https://github.com/vinod2807/konica-native-musl**
HEAD at report time: **`be198b2`** (branch `main` == `origin/main`, tree clean, repo **public**).

---

## 2. Non-negotiable constraints

1. **Dependency-free / pure libc.** No vendored third-party sources, no embedded vendor tables.
2. **Build flags.** Keep the default `CFLAGS`. `-ffp-contract=off` is **required** for bit-exact
   colour. **Never** use `-march=native`, `-Ofast`, or `-ffast-math`.
3. **No guessing.** Every conclusion comes from real reverse-engineering with evidence.
4. **Never commit the user's real documents** (CV, letter, WordCraft, etc.). Tests generate their
   own PDFs.
5. Full-bleed PPD only: `/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd`
   (the **206** PPD — not 205/225/245i).
6. When printing for tests: use a **temporary test queue**, leave existing queues and `/etc/cups/`
   alone, **one queue at a time** may drive the USB device, and **never** send banner/test-page PDFs.

> The original "never send a byte to a printer" rule was **lifted** by the user for this work.

---

## 3. Environment

**Host machine**
- OS: **ResolutePup64 26.04** (Puppy Linux; `ID=ubuntu`, `VERSION_ID=26.04`), kernel `7.0.14`.
- Firmware/boot: **BIOS/legacy** (no UEFI), GRUB in the MBR.
- RAM: 7.7 GB total (~1.2 GB free; Puppy runs much of the system in RAM). 8 GB swap.
- Working dir for the session: `/root`.

**Printer**
- **KONICA MINOLTA 206** on **USB** (USB ID `132b:232b`, serial `A8A6041029423`).
- Device node: `/dev/bus/usb/001/004` (root:lp 0660) and `/dev/usb/lp0`.
- CUPS device URI: `usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1`.

**Existing CUPS queues (left untouched)**
- `KONICA_MINOLTA_206` — `usb://…`, **vendor** filter (untouched).
- `konica206-native` — default queue, `ipp://localhost:8001`, PDF pass-through.
- `konica206uri`, `konica206uri-ppd`, `CUPS-PDF`, `EPSON_L3260`.
- `KONICA206_NATIVE_TEST` — the **temporary host test queue** created during step 5 (kept, on purpose).

**Host CUPS chain:** `pdftopdf → gstoraster → konica-native → usb` (Ubuntu Ghostscript works here).

**Alpine/musl environment**
- **Alpine 3.24.2 / musl 1.2.5**, recreated as a **chroot** (no Docker) at
  `/initrd/mnt/tmpfs/tmp/opencode/alpine/rootfs`, with `/proc`, `/dev` (devtmpfs), `/sys` mounted.
- It shares the host kernel, so it **sees the host's `/dev`** — including the USB printer.
- **On Alpine, `gstoraster` must be disabled for `application/vnd.cups-raster`** because Alpine's
  Ghostscript 10.05.1 produces **empty rasters** with this PPD; poppler's `pdftoraster` is used
  instead. (The disable is persisted in the chroot's
  `/usr/share/cups/mime/cupsfilters-ghostscript.convs`.)

---

## 4. Disk layout (relevant to a possible future Alpine install)

`/dev/sda` — 250 GB, **MBR** (sda1 boot vfat 1 GB · sda2 ext4 34 GB · **~4.3 GB unallocated gap** ·
sda3 ext4 "ResolutePup" = the running Puppy · sda4 extended → sda5 swap 8 GB · sda6 ext4 "easyos" ·
sda7 ext4 37.5 GB, unmounted, ~22 GB used).

`/dev/sdb` — 256 GB SSD, **GPT** (sdb1 bios_grub · sdb3 ext4 "trixie" = Debian, unmounted, ~87 GB
free · sdb2 ntfs "Windows").

**Important:** all 4 MBR primary slots on `sda` are taken, and the 4.3 GB gap is outside the extended
partition — so a new partition on `sda` requires **shrinking** an existing one.

**Status of the "install Alpine on a new partition" idea: NOT started.**
- The user first said **"shrink sda7"**, then **"do not shrink" / "stop"** before any change ran
  (the command failed at `sfdisk: command not found` and exited immediately).
- **No partition was resized, formatted, or created. `sda7` is byte-for-byte untouched.**
- If this is ever revisited: the user's rule is **do not install a bootloader (GRUB/syslinux) in the
  new Alpine install** — it must be booted by the **existing** GRUB (add a menu entry / chainload),
  leaving the MBR alone.
- Only side effect of the aborted attempt: an empty backup dir
  `/initrd/mnt/dev_save/alpine-install-backup/` (and a 0-byte file) on `sda3`. Harmless.

---

## 5. What is verified (runbook steps 1–5)

### Step 1 — build & self-tests
- Clean build; `make check` 0 FAILs.
- `check-lutgen` **PASS**, sha256 `84a424f5cccdbe7070b3f926fe9834b515db69391440ccac1dc5b046622c6d6f`.
- `check-difftest` 60/0; `check-real` OK.

### Step 2 / 2b — byte-identity (no printer)
- Generated + real documents produce **byte-identical** output vs the vendor, including the real
  `WordCraft-test.pdf`, `7.pdf`, `problematic.pdf`.
- Pad-only differences on CV / bus-ticket cases.
- Copies / toner-save / 600 dpi checks pass.

### Step 3 — Alpine/musl port (commit `06566b5`, re-confirmed end-to-end)
- Alpine 3.24.2 / musl 1.2.5, fresh chroot, vendor data at the default path with matching checksums.
- **musl-only binary** (glibc symbol count 0).
- `make check` **62 PASS / 0 FAIL** (reader 27/0, `check-lutgen` PASS, same sha256 as above).
- `check-difftest` 60/0; `verify-on-target.sh` 14/14.
- Two bugs in the runbook's `step3-alpine.sh` were fixed (relative `$DATA`/`$RC`, empty `summary.txt`,
  wrong `check-lutgen` grep) and published as `tools/step3-alpine.sh`.

### Step 4 — CUPS chain, no printer (commit `9aad1a0`)
- Published as `tools/step4-cups-chain.sh`.
- Verified: filters present; gstoraster workaround; **missing `.hft` → exit 1 / 0 bytes** fail-safe;
  valid stream; isolated `cupsd` on `127.0.0.1:8631` + capture backend.

### Step 5 — real prints on the **Ubuntu host** (commit `be198b2`)
- Temporary queue `KONICA206_NATIVE_TEST`; every job `Job completed`, every filter
  `exited with no errors`; **all five sheets user-verified correct on paper**:
  text (230), photo (231, 73 221 B), long-edge duplex (232, 167 401 B), short-edge duplex (233,
  169 007 B), `lp -n 2` (234, 191 608 B ≈ 2×).

---

## 6. Key findings (the non-obvious ones)

### 6.1 Copies need `*cupsManualCopies: True`
- The shipped full-bleed PPD says `*cupsManualCopies: False`.
- With `False`, `lp -n 2` prints **one** copy: `pdftopdf` records `%%PDFTOPDFNumCopies`, but
  `pdftoraster` drops it, and the filter (like the vendor) always emits `@PJL SET COPIES=1`.
- With `*cupsManualCopies: True`, CUPS duplicates the pages itself and the filter stays
  **byte-identical to the vendor** on the resulting 2-page raster.
- This is a **PPD-level** matter, not a filter gap. (The earlier runbook claim that "CUPS does the
  copying" was wrong for this PPD.)

### 6.2 Geometry is keyed only on the raster header
- `page_size` points → `base = floor(pt*600/72)`; `dim_from_points()`; `RESOLUTION=600` forced.
- `DUPLEX`/`BINDING` come per-job from page 1; 1 bpp pass-through.
- BIE pad bytes are deterministic zeros (masked by `prnscan.py padcmp`).
- `copies` / `argv[4]` are ignored by the vendor too → `COPIES=1` is faithful.

### 6.3 Rasterizer affects output size (expected, not a bug)
- **Ubuntu host** uses Ghostscript **`gstoraster`**; **Alpine** uses poppler **`pdftoraster`**.
- Different rasterizer → different raster → different compressed PRN byte count.
- The filter is deterministic *given the raster*; the byte-exactness work (steps 2/3) is about the
  filter, not the rasterizer.

---

## 7. NEW: printing from the Alpine/musl chroot (this session)

There is **no separate Alpine machine** here, but the Alpine chroot shares the host's `/dev`, so a
**musl `cupsd` inside the chroot can drive the USB printer directly**. This closes the runbook's last
item ("Not yet done: a print from the Alpine machine itself").

**Setup:** chroot `cupsd` on `127.0.0.1:8631` (isolated; host `cupsd` on `:631` untouched), queue
`KONICA206_CHROOT` with URI `usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1` and the
**musl** filter at `/usr/local/lib/konica/konica-native`.

**Result — all jobs `Job completed`, every filter `exited with no errors`:**

| Job | Sheet | Bytes sent to USB |
|---|---|---|
| 11 | text (1 page) | 103 538 |
| 12 | photo (1 page) | 70 701 |
| 13 | duplex **long-edge** (2 pp) | 172 325 |
| 14 | duplex **short-edge** (2 pp) | 174 151 |
| 15 | `lp -n 2` text (**2 sheets**) | 206 936 |

`206 936 ≈ 2 × 103 538` → confirms two copies with `*cupsManualCopies: True`.

Representative log lines (chroot `/var/log/cups/error_log`, job 11):

```
[Job 11] 3 filters for job:
[Job 11] pdftopdf        (application/pdf -> application/vnd.cups-pdf, cost 66)
[Job 11] pdftoraster     (application/vnd.cups-pdf -> application/vnd.cups-raster, cost 100)
[Job 11] /usr/local/lib/konica/konica-native (application/vnd.cups-raster -> printer, cost 0)
[Job 11] Started backend /usr/lib/cups/backend/usb (PID 54835)
[Job 11] Printing on printer with URI: usb://KONICA%20MINOLTA/206?serial=A8A6041029423&interface=1
[Job 11] PID 54832 (pdftopdf)       exited with no errors.
[Job 11] PID 54833 (pdftoraster)    exited with no errors.
[Job 11] PID 54834 (konica-native)  exited with no errors.
[Job 11] Sent 103538 bytes...
[Job 11] PID 54835 (backend/usb)    exited with no errors.
[Job 11] Job completed.
```

Note the byte counts differ from the host step-5 run because of the rasterizer difference (§6.3).

**Cleanup / current state:** chroot `cupsd` stopped; only the host `cupsd` remains; USB device free;
host queue idle. The queue `KONICA206_CHROOT` is kept; the chroot's original `cupsd.conf` is backed up
at `/etc/cups/cupsd.conf.chroot-print.orig` (inside the chroot).

---

## 8. Repo scripts (published)

- `tools/step3-alpine.sh` — Alpine/musl build + verification in a chroot.
- `tools/step4-cups-chain.sh` — CUPS raster chain on Alpine **without a printer** (capture backend;
  isolated `cupsd` on `:8631`; `lp -n 1` vs `-n 2`; the `cupsManualCopies` fix).
- Chroot **real-print** scripts used this session live inside the chroot at
  `/work/print-chroot.sh` and `/work/print-chroot-set.sh` (not yet in the repo).

---

## 9. Docs updated & published

- `docs/ALPINE-PORT-STATUS.md` §3.4/§3.5/§7/§8
- `docs/ALPINE.md` (install now tested + copies note)
- `HANDOFF.md` §1/§2/§3.6/§12.4
- `README.md`

Commits pushed: `be474d4 → … → da1070d → 06566b5 → 9aad1a0 → be198b2`.

---

## 10. Open items / possible next steps

1. **Nothing required** — runbook steps 1–5 are done, including a print from Alpine (chroot).
2. Optional: turn the chroot real-print into `tools/step5-chroot-print.sh` and document it; commit.
3. Optional: extra prints (real photo `photo_real_a4.pdf`, a Letter page) or a vendor-vs-native A/B
   print.
4. Optional: run the runbook's step 6 **rollback** (delete the test queue/filter) — intentionally
   **not** done; the user chose to keep the test queue and installed filter.
5. Optional: revisit "install Alpine on a partition" — **only** if the user re-approves; must **not**
   install a bootloader; must boot via the existing GRUB.

---

## 11. Exact file/path map

| Path | What |
|---|---|
| `/home/spot/Downloads/KONICA-NATIVE-INSTRUCTIONS(3).md` | latest runbook |
| `/home/spot/Downloads/konica-native-musl/` | the repo (HEAD `be198b2`) |
| `…/konica-native-musl/tools/step3-alpine.sh`, `tools/step4-cups-chain.sh` | published scripts |
| `…/konica-native-musl/docs/{ALPINE.md,ALPINE-PORT-STATUS.md}`, `HANDOFF.md`, `README.md` | docs |
| `/var/lib/legacy-printer-app/ppd/KonicaMinolta-206-fullbleed.ppd` | source PPD (`*cupsManualCopies: False`) |
| `/var/lib/konica-native/KonicaMinolta-206-native.ppd`, `…-manual.ppd` | generated native PPDs (host) |
| `/etc/cups/ppd/KONICA206_NATIVE_TEST.ppd` | host test-queue PPD (cupsFilter → konica-native) |
| `/usr/local/lib/konica/konica-native` | installed **glibc** filter (host) |
| `/usr/local/lib/konica/KonicaMinolta/245igdi/` | vendor tree (`.hft`, ICC, vendor filter `Filters/245igdirf`) |
| `/initrd/mnt/tmpfs/tmp/opencode/alpine/rootfs/` | **Alpine 3.24.2 / musl** chroot |
| `…/rootfs/usr/local/lib/konica/konica-native` | **musl** filter (chroot) |
| `…/rootfs/var/lib/konica-native/KonicaMinolta-206-native.ppd` | chroot native PPD |
| `…/rootfs/work/print-chroot.sh`, `…/print-chroot-set.sh` | chroot real-print scripts |
| `…/rootfs/work/{text,photo,twopage}.pdf` | chroot test inputs |
| `/tmp/mkr/docs/{text,photo,twopage,letter,photo_real_a4,photo_real_letter}.pdf` | host test inputs |
| `/root/konica-corpus-full` | 60-case corpus + `manifest.tsv` |
| `/var/log/cups/error_log` | host per-job evidence (host LogLevel restored to `warn`) |

---

## 12. Useful reference facts

**Vendor invocation env:** `PPD`, `CONTENT_TYPE`/`FINAL_CONTENT_TYPE=application/vnd.cups-raster`,
`CUPS_SERVERROOT=/etc/cups`, `CUPS_DATADIR=/usr/share/cups`,
`LD_LIBRARY_PATH=/usr/local/lib/konica/lib`.
**argv:** `245igdirf 1 <user> <title> <copies> "$OPTS"`.
**Default OPTS:** `PageSize=A4 media=A4 Duplex=None sides=one-sided Duplexer=true MediaType=Plain back-side-orientation=Rotated`.

**Raster header (v1/v2/v3 share; `b` = buffer after 4-byte sync):** `MediaType`@128,
`Duplex`@272 (file 276), `HWResolution`@276/280, `PageSize`@352/356, `Tumble`@368 (file 372),
`cupsWidth`@372, `cupsHeight`@376, `BitsPerColor`@384, `BitsPerPixel`@388, `BytesPerLine`@392,
`ColorSpace`@400, `NumColors`@420, `cupsPageSize`@428/432, `PageSizeName`@1732. Header 1796 B
(v2/v3), 420 B (v1). Magic `tSaR` / `2SaR` / `3SaR`.

**Gotchas:** `KONICA_*` env vars do **not** apply under CUPS (filters run with a scrubbed env; data
must be at the default path). CUPS backends run unprivileged (`lp`) — capture output must go to a
world-accessible dir. `page_log` shows `total 0` for this chain (normal; `pdftopdf` logs
"will not log pages in page_log"). Don't edit `tests/fuzz-difftest.sh` mid-run (`sh` re-reads
incrementally → syntax error). `tools/mkraster` has no Makefile rule (`check-reader.sh` builds it on
demand).
