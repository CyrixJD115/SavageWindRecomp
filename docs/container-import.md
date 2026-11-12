# Game files from NSP / XCI

*Fork-local (branch `feature/container-import`): this decrypts game packages, which upstream's contribution rules
exclude. Don't open an upstream PR for it.*

The setup page has two ways to bring the game:

- **Game files folder**: a folder with the extracted `exefs/` and `romfs/` (the 1.9.0 update merged in) and,
  optionally, the DLC RomFS folders. This is unchanged.
- **Game packages (NSP / XCI)**: the player's own dump, picked together: the base game (XCI or NSP), the 1.9.0
  update NSP and, optionally, the two DLC NSPs, or one XCI that holds all of them. Choosing a folder that holds
  packages, and no extracted files, does the same.

Packages are read with the player's own `prod.keys`, from the **Decryption keys** row (saved as `prod_keys` in
`savagewind.ini`). If that row is empty, the setup looks in `~/.switch/prod.keys` and then for a `prod.keys` next to
the packages. A `title.keys` next to `prod.keys` is also read, as are any tickets inside the NSPs. That's only needed
for rights-managed packages. No keys are shipped.

## How it works

`runtime/game_import.c` runs the bundled **hactool** (SciresM, ISC) as a child process and does no crypto itself:

1. `hactool -t pfs0 --outdir` / `-t xci --securedir` unpacks each package into `<data root>/import/`.
2. `hactool -t nca` reads each NCA's title ID and content type to pick out the base program (`01007EF00011E000`), the
   update program (`…E800`), DLC 1 (`…F001`) and DLC 2 (`…F002`). Other NCAs are deleted at once to save space.
3. `--exefsdir` extracts the update's ExeFS into the scratch folder. The launcher checks the build ID of `main` against
   `runtime/game_versions.h` **before** it replaces the existing copy.
4. `--basenca=<base> --romfsdir=extracted/romfs <update>` applies the update to the base RomFS (BKTR). The usual
   `check_romfs` and movie-count checks follow.
5. `--romfsdir=extracted/aoc1|aoc2` extracts the DLC.
6. The scratch folder is deleted, and the setup carries on as usual: build the game code, check, precompile shaders.

The keys handed to hactool are a cleaned copy in the scratch folder: only well-formed `name = hex` lines, because
hactool stops at comments, stray NULs and odd-length values. The player's own file is never changed.

Disk: about twice the size of the packages plus 5 GB at the peak. The setup checks this before it starts.

## Building

`make` fetches and builds hactool once with `tools/build_hactool.sh`: the pinned commit plus the three patches in
`tools/hactool/`, the same ones totk-pc-recomp uses. It goes into `build/hactool/`, next to the executable, where
`rt_resource` finds it. `make NO_HACTOOL=1` skips it. The AppImage (`tools/linux/package.sh`) and the Windows zip
(`make dist`) ship that folder as `hactool/`, with its LICENSE.

## Testing

```
SAVAGEWIND_LAUNCHER_EXTRACT=<folder with the packages> build/savagewind   # headless setup, logs [import] lines
```
