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
  │            1. parse containers (PFS0 for NSP, HFS0 for XCI), collect NCAs + CNMTs + tickets
  │            2. classify by CNMT: base 01007ef00011e000, update ...e800, DLC ...f001/f002
  │            3. verify the update's ExeFS main build id == the supported version's
  │            4. extract merged tree: exefs/ (from the update), romfs/ (BKTR base+update), aoc1/ aoc2/
  │            5. write container manifest (sources + digests → cache check)
  └─ build_game_code() / check_game_files() / precompile      (existing code, unchanged)
```

Nothing downstream changes: the game still consumes a plain `extracted/` folder, and the folder path stays the
default and always available.

## 2. New files and modules

All new logic in one self-contained C module (the runtime is C; `launcher.mm` is C++ and calls it through the
`extern "C"` header). One Makefile line adds the sources to `RT_SRC`.

```
runtime/containers/
  containers.h        public API: classification result, extract entry point, progress/cancel, error strings
  containers.c        orchestrator: sources → titles → merged tree + manifest
  keys.c keys.h       prod.keys / title.keys parsing (hactool INI format), key lookup, titlekey unwrap,
                      missing-key reporting per title
  pfs0.c              NSP PFS0 (entry strides 0x20 and 0x18 — NSCB split output uses 0x18; detect by tiling)
  hfs0.c              XCI: gamecard header, root/partition HFS0, secure partition contents
  cnmt.c              CNMT + NACP parsing, title classification, content lists
  nca.c               NCA3 header (XTS, 0xC00 = 6 sectors, BE sector tweaks), sections, ExeFS extract,
                      NPDM extract
  bktr.c              patch RomFS: relocation + subsection tables, indirect addressing over the base NCA
  romfs.c             RomFS (virtual, base+patch) directory walk → folder extraction; DLC RomFS extraction
  crypto/aes.c        AES-128 ECB/CTR/XTS (XTS over 0x200 sectors)
  crypto/sha256.c     SHA-256 (manifest, section hashes, ExeFS header hashes)
  crypto/rsa.c        RSA-2048 bignum + OAEP unwrap (ticket titlekeys) + PKCS1 v1.5 signature check
  crypto/bignum.c     small fixed-size bignum (limbs of uint32/uint64) for RSA
tools/
  make_container_fixtures.py   builds synthetic NSP/XCI fixtures with test keys (no Nintendo data)
  test_containers.c            unit tests: fixtures → classification → extraction → BKTR merge results
```

Settings (`runtime/settings.h`, savagewind.ini): `keys_prod[1024]`, `keys_title[1024]` (paths only).

## 3. What to reference (not vendor wholesale)

The byte-level details below were validated against real containers with a prototype; keep them as the spec:

- **PFS0 (NSP)**: 0x10 header (magic, count, string size, pad), entries `u64 off, u64 size, u32 name_off`
  padded to **0x20**; NSCB-split output pads to **0x18** instead — detect by which stride tiles the container
  (`data_start + Σsizes ≤ filesize` and monotonic offsets).
- **NCA3 header**: XTS-encrypted region is **0xC00 = 6 × 0x200 sectors, sector number big-endian** in the
  tweak. Fields: magic 0x200, crypto_type 0x206, kaek_ind 0x207, size u64 0x208, title id 0x210,
  crypto_type2 0x220, rights id 0x230, section table (4 × u32 media_start + media_end) 0x240, section
  SHA-256s 0x280, key area 4×16 0x300, FS headers 0x200 each at 0x400 (part type +2, fs type +3
  (2=PFS0, 3=IVFC), crypt type +4 (1=none, 3=CTR, 4=BKTR), superblock +8, CTR nonce +0x140, and for BKTR
  sections the relocation/subsection headers at +0x100/+0x120, each pointing at a "BKTR"-magic table inside
  the section's media range).
- **CTR**: counter = nonce (8 bytes as stored) ‖ BE64(absolute byte offset in the NCA >> 4). This is hactool's
  construction and NSCB's (squirrel `Counter.new(64, prefix=nonce, initial_value=(sec_off+off)>>4)`).
- **Keys**: keygen = `max(crypto_type, crypto_type2)`, then **-1** (0 stays 0: revisions 0 and 1 are master key
  0). Body key = key-area slot 2 (card/XCI content, `rights_id` zero) decrypted ECB with
  `key_area_key_{application|ocean|system}_{keygen}` per `kaek_ind`; ticket content (`rights_id` set) unwraps
  the ticket's RSA-OAEP blob with the eticket RSA key decrypted from `eticket_rsa_kek`, falling back to
  `title.keys` entries (`<rights id 32 hex> = <encrypted titlekey>`).
- **BOTW update NCAs** (verified): the update's program NCA carries the **full 1.9.0 ExeFS** as a plain CTR
  PFS0 — no ExeFS patching needed; its RomFS is a **BKTR** section applied over the base's RomFS. BKTR data:
  bucket headers ("BKTR", version, entry count) at the offsets in the FS header; relocation entries
  (virtual offset, physical offset, type: 0 = from patch, 1 = from base), subsection entries carry their own
  CTR restart offsets (counter uses the subsection's physical offset).

Reference implementations, in order of usefulness (read for semantics; reimplement in project style):

1. **hactool** (ISC, SciresM) — `nca.h`/`nca.c` are the definitive byte layout; `crypto/` (aes, xts, sha,
   rsa with BigInt) is small, proven, and license-compatible to adapt nearly verbatim with attribution.
2. **nstool** (MIT, jakcron) + **libtoolchain** — NCA/NSP/XCI/HFS0 object model, `--basenca` BKTR handling,
   clean C++ structure worth mirroring for the module layout.
3. **NSC_BUILDER squirrel.py** (MIT) — bulk NSP/XCI handling, title classification by CNMT, key-management
   patterns; its `Fs/pyNCA3.py` documents the 0x18-stride PFS0 quirk and the CTR counter in Python.
4. **LibHac / hactoolnet** (MIT, Thealexbarney) — the cleanest BKTR model (`IndirectStorage`,
   `AesCtrSubsectionStorage`) for the merge semantics.
   (Avoid hac2l: GPLv2.)

Crypto beyond AES/SHA (RSA-2048 CRT + OAEP for ticket titlekeys) is the only heavy piece; adapting hactool's
`rsa.c`/`BigInt` (ISC) keeps it contained. XCI gamecard-header key areas use `xci_header_key` from prod.keys
when encrypted (rare); refuse with a clear message if it's missing.

## 4. Implementation plan

**Phase 1 — MVP (NSP base + update → merged exefs/romfs):**
1. `keys.c` (parse prod.keys; report missing keys by name), `crypto/` (AES-XTS/CTR, SHA-256).
2. `pfs0.c` (both strides) + `nca.c` header/sections + CTR decrypt; unit-test against self-built fixtures.
3. ExeFS extraction from the update's program NCA (PFS0 → `exefs/`: main, main.npdm, rtld, sdk, subsdk0…),
   build-id check against `GAME_VERSIONS` — fail before writing anything if it isn't 1.9.0.
4. `cnmt.c` classification (base/update/DLC by title id and CNMT type) and NCA-id matching across base and
   update content.
5. `bktr.c` + `romfs.c`: merged virtual RomFS → `romfs/` folder extraction (patch tables from the update's
   FS header, indirect extents served from base or patch NCA).
6. `containers.c` orchestrator + manifest; CLI `--extract-container` for headless testing; Makefile + test
   target `test-containers`.

**Phase 2 — XCI + DLC:** `hfs0.c` (root/partition HFS0, secure partition as a PFS0-equivalent source,
gamecard key areas), DLC extraction to `aoc1`/`aoc2` **by title id** (F001 → aoc1, F002 → aoc2 — the folder
path sorts by file count; containers can do better), `title.keys` fallback path, ticket handling.

**Phase 3 — launcher + UI integration:** picker changes, keys rows, status/failure texts, manifest caching
(§5), i18n tables, `docs/game-files.md` update.

**Phase 4 — hardening:** every §6 edge case gets its own clear message and a fixture test.

Testing: `tools/make_container_fixtures.py` writes tiny NSP/XCI fixtures encrypted with keys generated in the
script (synthetic content, no Nintendo data — base/update/DLC "NCAs" with fake RomFS trees and an NSO whose
build id equals a constant the test defines); `tools/test_containers.c` round-trips them (classify → decrypt →
merge → extract → compare trees, including BKTR indirect extents in both directions). Gameplay validation
needs the player's own dump (see docs/FAQ.md: files must be their own 1.9.0 copy) via
`SAVAGEWIND_SETUP_CONTAINERS` → `tools/linux/test_run.sh`.

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
  `key_area_key_*_xx` / `eticket_rsa_kek` for the titles present (name the key), (c) wrong keys — NCA magic
  doesn't decrypt → "these keys don't decrypt this content" (this also catches re-encrypted repacks, which
  no keys can open), (d) `title.keys` needed for ticket NSPs and absent.
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

- Design: this document.
- Implementation: not started (branch `feature/container-import`).
- Real-file validation: prototype confirmed the format facts above; end-to-end gameplay validation needs the
  player's own 1.9.0 dump (repacked/scene NSP-XCI sets cannot be decrypted by design and are refused).
