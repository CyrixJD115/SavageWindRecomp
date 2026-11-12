# Container import: NSP / XCI → extracted game files

*Design for the optional "Load from NSP/XCI" setup path. Branch: `feature/container-import` (fork-local: this
adds decryption support, which upstream's contribution rules exclude — do not open an upstream PR for it).*

## 1. Where it hooks into the current setup flow

The setup lives in `runtime/ui/launcher.mm`. `extract_thread` resolves the folder the player picked with
`find_game()` into `exefs/`, `romfs/` and DLC folders, checks the main module's build id
(`runtime/game_versions.h`), copies everything into the data root's `extracted/` (`romfs_copy_tree`), then builds
the game code (`gamebuild_run`, `runtime/gamebuild.c`) and verifies (`check_game_files`). The game reads the
result in place: NSOs from `extracted/exefs` (`runtime/loader.c`), the RomFS image from `extracted/romfs`
(`runtime/romfs_image.c`), the DLC from `aoc1/` and `aoc2/` (`runtime/services/fs.c`, `runtime/dlc_check.c`).

The cleanest insertion point is **in front of `find_game()`**: when what the player picked is a container (or a
folder of containers), a new self-contained stage classifies the titles, decrypts, applies the BKTR patch and
writes the finished tree straight into `extracted/` — and the rest of the flow continues unchanged from the
`check:` label (`build_game_code()` → checks → shader precompilation):

```
extract_thread
  ├─ picked a folder with exefs/romfs   → find_game() (existing path, untouched)
  ├─ picked .nsp / .xci file(s), or a folder whose search found containers and no exefs/romfs
  │      └─ container_extract(sources, keys, out=extracted/)      ← new stage (runtime/containers/)
  │            1. parse containers in place (PFS0 for NSP, HFS0 for XCI — no decryption needed)
  │            2. decrypt each NCA's header (XTS) and classify: base ...e000, update ...e800 (the program
  │               NCA with a BKTR section), DLC ...f001/f002; keygen per NCA (max(crypto, crypto2), -1);
  │               collect common tickets when present
  │            3. verify against the supported version, then extract:
  │                 hactool --exefsdir=extracted/exefs <update program NCA>
  │                 hactool --basenca=<base program NCA> --romfsdir=extracted/romfs <update program NCA>
  │                 hactool --romfsdir=extracted/aoc<n> <DLC NCA>            (aoc1 = ...f001, aoc2 = ...f002)
  │            4. verify results (ExeFS main build id, romfs System/Version.txt, aoc AocVersion.txt)
  │            5. write container manifest (sources + digests → cache check)
  └─ build_game_code() / check_game_files() / precompile      (existing code, unchanged)
```

The heavy decryption and the BKTR merge are done by a **vendored hactool binary** run as a child process —
the same tool the generator and the compiler already are here (`gamebuild.c` spawns `swgen` and `zig`), and the
approach totk-pc-recomp uses in practice (its `tools/extract.py` drives a vendored hactool with
`--basenca` for exactly this merge). hactool verifies every IVFC level of the merged RomFS itself; our stage
checks its log for `Level n (GOOD)` and treats its two always-failing Nintendo signature lines as noise.
Nothing downstream changes: the game still consumes a plain `extracted/` folder, and the folder path stays the
default and always available.

Validated end to end against real files (2026-11-12): base XCI + update NSP + DLC NSPs of a personal dump →
`exefs/` (main build id `cd57b23fa4bbad65`), merged `romfs/` (System/Version.txt "1.9.0", ~41k files, five
IVFC levels GOOD), `aoc1/` (AocVersion 3.0).

## 2. New files and modules

All new logic in one self-contained C module (the runtime is C; `launcher.mm` is C++ and calls it through the
`extern "C"` header). One Makefile line adds the sources to `RT_SRC`. The module does the light work natively
(container tables, NCA headers, classification, keys sanitizing, orchestration) and runs the vendored hactool
for everything crypto-heavy:

```
runtime/containers/
  containers.h        public API: classification result, extract entry point, progress/cancel, error strings
  containers.c        orchestrator: sources → titles → hactool invocations → merged tree + manifest
  keys.c keys.h       prod.keys parsing, sanitized copy for hactool (drop comments and malformed lines: the
                      user's dump has 30-hex-digit mariko kek sources hactool would exit on), ticket collection
                      (title.keys lines for rights-managed NCAs), missing-key reporting per title
  pfs0.c              NSP PFS0 (entry strides 0x20 and 0x18 — NSCB split output uses 0x18; detect by tiling)
  hfs0.c              XCI: "HEAD" at 0x100, root HFS0 offset (u64) at 0x130, secure partition contents
  nca.c               NCA3 header decrypt (XTS, 0xC00 = 6 sectors of 0x200, sector number big-endian),
                      section table (0x240: u32 media start/end pairs), FS headers (0x400 + i*0x200:
                      partition/fs/crypt types at +2/+3/+4), keygen and key-area checks, classification
  plat_process.h      child process plumbing (reuse the platform spawn/wait already in the runtime)
toolchain/
  hactool/            vendored binary per platform + its ISC license file (built from SciresM/hactool at a
                      pinned commit; totk-pc-recomp's tools/build_hactool.py is the recipe)
tools/
  make_container_fixtures.py   builds synthetic NSP/XCI fixtures with test keys (no Nintendo data)
  test_containers.c            unit tests: fixtures → classification → extraction → merge results
  build_hactool.sh             the vendored build (clone at pin, build, copy binary + license)
```

Crypto needed natively is only AES-128-XTS for NCA headers (one key, six sectors) — a few hundred lines —
since hactool does the rest. No RSA, no CTR/XTS storage paths, no RomFS walker of our own.

Settings (`runtime/settings.h`, savagewind.ini): `keys_prod[1024]`, `keys_title[1024]` (paths only).

## 3. What to reference (not vendor wholesale)

The byte-level details below were validated against real containers (the player's own dump) with a working
prototype; keep them as the spec:

- **PFS0 (NSP)**: 0x10 header (magic, count, string size, pad), entries `u64 off, u64 size, u32 name_off`
  padded to **0x20**; NSCB-split output pads to **0x18** instead — detect by which stride tiles the container
  (`data_start + Σsizes ≤ filesize` and monotonic offsets). Names are NUL-terminated in the string table.
- **XCI**: "HEAD" magic at 0x100, **root HFS0 offset (u64) at 0x130**; partitions update/normal/secure, game
  content in `secure` (HFS0 entries are 0x40 bytes: off u64, size u64, name u32, hashed flag, hashes).
- **NCA3 header**: XTS-encrypted region is **0xC00 = 6 × 0x200 sectors, sector number big-endian** in the
  tweak. Fields: magic 0x200, crypto_type 0x206, kaek_ind 0x207, size u64 0x208 (must equal the container
  entry's size), title id 0x210, crypto_type2 0x220, rights id 0x230, section table (4 × u32 media start +
  media end) 0x240, section SHA-256s 0x280 (each is the hash of that section's 0x200-byte FS header — a free
  header-decryption check), key area 4×16 0x300.
- **FS headers** at 0x400 + i*0x200: partition type +2, fs type +3 (2=PFS0, 3=IVFC), crypt type +4
  (1=none, 2=XTS, 3=CTR, 4=BKTR); **PFS0 sections keep a SHA-256 hash table before the PFS0 header** (the
  superblock at +0x28/+0x30 gives blocksize, levels, hashoff/hashsize, pfsoff/pfssize — the update ExeFS's
  PFS0 sits at +0x8000). Validate decryption at `pfsoff`, never at 0. BKTR sections carry their
  relocation/subsection table offsets in the superblock at +0x100/+0x120 ("BKTR"-magic headers inside the
  section's media range).
- **Keys**: keygen = `max(crypto_type, crypto_type2)`, then **-1** (0 stays 0). Body key = key-area slot 2
  decrypted ECB with `key_area_key_{application|ocean|system}_{keygen}` per `kaek_ind` when `rights_id` is
  zero (card/XCI-style content — the common case, no ticket involved); rights-managed NCAs use
  `titlekek_{keygen}` on the ticket's key block. **Expect mixed keygens within one dump**: NSCB-style
  conversions rewrite base/DLC NCAs to keygen 0 while the update stays at its real generation (BOTW 1.9.0
  dumps show base at 0/0 and update at 2/3) — derive it per NCA.
- **hactool and keys files**: hactool exits on lines it can't parse (comments, trailing notes, a trailing NUL
  Lockpick puts on `*_kek_source`, any key with the wrong hex length) — always hand it a sanitized copy, never
  the player's file. Its two Nintendo signature checks always print (FAIL) — that is noise, not an error;
  trust the IVFC `Level n (GOOD)` lines and the extracted results.

Reference implementations, in order of usefulness:

1. **totk-pc-recomp** (valentderah) — `tools/vendor/macnativegames/extract_exefs.py` is a compact, proven,
   game-agnostic reader of exactly these containers (PFS0/HFS0 with the 0x18-stride quirk, XCI root at 0x130,
   common-ticket parsing, key-area/titlekey selection, ExeFS extraction with full hash verification), and its
   `tools/extract.py` + `build_hactool.py` show the vendored-hactool BKTR merge end to end. Read it first.
2. **hactool** (ISC, SciresM) — the binary we vendor; `nca.h` is the definitive byte layout.
3. **NSC_BUILDER squirrel.py** (MIT) — NSCB's Python; documents the split-NSP conventions the dumps use.
4. **nstool** (MIT) / **LibHac** (MIT) — deeper format semantics if the native scope ever grows.
   (Avoid hac2l: GPLv2.)

## 4. Implementation plan

**Phase 1 — MVP (NSP base + update → merged exefs/romfs):**
1. Vendored hactool build (`tools/build_hactool.sh`: clone SciresM/hactool at the pin totk-pc-recomp uses,
   build, copy binary + ISC license into `toolchain/hactool/` per platform).
2. `keys.c`: parse prod.keys, write the sanitized copy hactool reads, collect common tickets into a
   `title.keys`-format file (hactool's patched builds read them; rights-managed NSPs need it), report missing
   keys by name.
3. `pfs0.c` + `hfs0.c` + `nca.c`: in-place container walk, NCA header decrypt and classification (base =
   program NCA of ...e000; update = program NCA whose FS header crypt type is 4/BKTR; DLC = content type 5,
   aoc1 = ...f001, aoc2 = ...f002), ExeFS build-id check against `GAME_VERSIONS` before extracting anything.
4. `containers.c` orchestrator: extract each Program NCA to a scratch file (both are needed on disk for
   `--basenca`; ~15 GB scratch, check free space like `room_for_build`), run hactool for
   exefs/romfs/aoc folders, verify its IVFC `Level n (GOOD)` lines and the extracted markers
   (`exefs/main` build id, `romfs/System/Version.txt`, `aoc1/System/AocVersion.txt`), write the manifest.
5. `--extract-container` CLI for headless testing; Makefile + `test-containers` target.

**Phase 2 — launcher + UI integration:** picker changes, keys rows, status/failure texts, manifest caching
(§5), i18n tables, `docs/game-files.md` update.

**Phase 3 — hardening:** every §6 edge case gets its own clear message and a fixture test. A native
(BKTR-in-C) extractor remains optional future work; the vendored hactool path is the shipped one.

Testing: `tools/make_container_fixtures.py` writes tiny NSP/XCI fixtures encrypted with keys generated in the
script (synthetic content, no Nintendo data — base/update/DLC "NCAs" with fake RomFS trees and an NSO whose
build id equals a constant the test defines); `tools/test_containers.c` round-trips them (classify → extract →
compare trees). The real-file pipeline was already validated with a prototype (see §1); gameplay validation
runs the player's own dump via `SAVAGEWIND_SETUP_CONTAINERS` → `tools/linux/test_run.sh`.

## 5. CLI flags, UI, caching

- CLI (`runtime/main.c` arg loop, next to `--dump-romfs`):
  - `--keys <prod.keys>` / `--titlekeys <title.keys>` — also stored in settings when given.
  - `--extract-container <files...>` — classify, verify, extract the merged tree into the game folder, print
    the result, exit 0/1 (headless, testable; no window).
- Test hooks (repo convention): `SAVAGEWIND_SETUP_CONTAINERS="a.nsp;b.xci"` and `SAVAGEWIND_KEYS=<path>`
  (mirrors `SAVAGEWIND_SETUP_FROM`).
- UI (game files page, `launcher.mm`): a second row **"Game files from NSP or XCI"** ("Choose files…" —
  `SDL_ShowOpenFileDialog`, filter `nsp;xci`, multi-select) beside the existing folder row; if the picked
  folder holds containers and no `exefs/`+`romfs/`, treat it as a container set (find_game already flags
  `.xci/.nsp` as "unusable" today — that becomes this path instead). New **"Decryption keys"** section:
  `prod.keys` (required for containers), optional `title.keys`, remembered in settings; explanation states
  keys are the player's own and are never shipped or uploaded.
- Caching: the setup writes `extracted/container.manifest` (schema version, per-source path, size, mtime,
  SHA-256) after a successful extraction. "Set up game files" with the same sources (size+mtime match;
  full-hash re-check only if mtimes changed) skips straight to `build_game_code()`. Changed/missing sources
  → re-extract. Add `container.manifest` to `GAME_FILES` (`runtime/game_files.h`) so "Delete game files"
  removes it; check free space first (~16 GB like `room_for_build` does for the build).

## 6. Edge cases

- **No update picked**: base alone can't produce 1.9.0 — refuse with "the update is required", listing the
  titles found. **Update is the wrong version**: the ExeFS build-id check names the found version.
- **Wrong/missing keys**: distinguish (a) no keys file, (b) missing `header_key` / `master_key_xx` /
  `key_area_key_*_xx` / `titlekek_xx` for the keygen generations present (name the key — a dump can mix
  generations across base/update/DLC), (c) wrong keys — NCA magic doesn't decrypt → "these keys don't decrypt
  this content", (d) a rights-managed NCA whose common ticket is missing from the NSP.
- **Keys file quirks** (seen in real dumps): comment lines, trailing notes after the value, a trailing NUL on
  `*_kek_source`, wrong-length values — the sanitized copy for hactool drops or fixes these; never modify the
  player's file.
- **Multi-program / multi-NCA titles**: merge per content NCA id across base+update (program, data, control,
  html, legal); ExeFS from the program NCA only; union of the merged RomFS trees into `romfs/` (patch wins).
- **DLC-only pick**: allowed when `extracted/` is already set up (mirrors today's DLC-only folder path);
  extract each pack's RomFS to `aoc1`/`aoc2` by title id; verify 3.0 via `System/AocVersion.txt`
  (`rt_dlc_check` handles the rest).
- **Compressed inputs**: `.nsz`/`.xcz` → explicit "not supported" (later: decompress via nsz).
- **XCI oddities**: multi-content XCI (update/DLC in secure), update/normal partitions scanned for CNMTs;
  encrypted gamecard key areas need `xci_header_key` (clear message if missing); trimmings/bad sizes
  (header vs file size) refused.
- **Interrupted setup**: extraction writes into `extracted/` after `rt_remove_game_files`, like the copy path
  today — a half-extracted tree fails `check_game_files` and the next setup redoes it; cancel (Esc/B) keeps
  what finished, the manifest is only written on success.
- **Long paths (Windows >260 chars)**: reuse the runtime's existing path handling; RomFS names are short
  enough in practice but the extractor must not create deeper trees than the game's own.
- **Reported, not crashed**: every failure goes through `set_job` with a headline + hint in the player's
  language (i18n tables, `tools/i18n_check.py`).

## Status

- Design: this document (revised after validating the whole pipeline against a real dump).
- Validated end to end on real files (prototype): containers → classification → hactool BKTR merge →
  `exefs/` (build id `cd57b23fa4bbad65`), `romfs/` (Version.txt "1.9.0"), `aoc1/` (AocVersion 3.0).
- Implementation: not started (branch `feature/container-import`).
