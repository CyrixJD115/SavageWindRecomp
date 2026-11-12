// Game files from the player's own NSP / XCI dump (the setup page's "Game packages"; runtime/ui/launcher.mm).
// The packages are unpacked, their NCAs told apart and decrypted with the player's own prod.keys, and the 1.9.0
// update applied over the base game's RomFS (BKTR) - all of that by the bundled hactool (SciresM, ISC: hactool/ next
// to the executable, tools/build_hactool.sh). This only runs it and picks which NCA is which. The result is the same
// exefs/ romfs/ aoc1/ aoc2/ a folder of extracted files gives. No keys are shipped: the player chooses theirs.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { GI_BASE, GI_UPDATE, GI_DLC1, GI_DLC2, GI_PARTS };

typedef enum {
    GI_OK = 0,
    GI_E_NO_TOOL,      // hactool isn't next to the executable (or in build/hactool/ for a source build)
    GI_E_NO_KEYS,      // the keys file can't be read
    GI_E_KEYS_INCOMPLETE,  // no header_key in it (why: what's missing)
    GI_E_KEYS_WRONG,   // the NCA headers don't decrypt with these keys
    GI_E_COMPRESSED,   // .nsz / .xcz
    GI_E_TITLEKEY,     // a rights-managed NCA without its ticket or title.keys entry
    GI_E_TOOL,         // hactool failed (why: the end of its output)
    GI_E_CANCELLED,
} GiResult;

typedef struct {
    char work[1100];             // the scratch folder: unpacked NCAs, the cleaned keys
    char nca[GI_PARTS][1100];    // the NCAs found ("" = not among the packages)
    uint64_t nca_bytes[GI_PARTS];
    char other[400];             // other titles found (for the message when base or update is missing)
} GiSet;

// true: a file name ending in .nsp / .xci / .nsz / .xcz
bool gi_is_package(const char *path);
// The keys: `explicit_path` if set, else prod.keys in ~/.switch (where hactool and emulators keep it). "" = none.
void gi_find_keys(const char *explicit_path, char *out, size_t n);
bool gi_tool_present(void);

// Unpacks the packages into `work` (removed first) and finds the base game, update and DLC NCAs in them. done/total:
// bytes, for the progress bar (total = what the packages hold).
GiResult gi_open(GiSet *s, const char *const *packages, int n, const char *keys, const char *work,
                 void (*progress)(uint64_t done, uint64_t total), char *why, size_t wn);
// The update's ExeFS / the update applied over the base RomFS / a DLC's RomFS, into out_dir (made by hactool)
GiResult gi_exefs(GiSet *s, const char *out_dir, char *why, size_t wn);
GiResult gi_romfs(GiSet *s, const char *out_dir, void (*progress)(uint64_t done, uint64_t total), char *why, size_t wn);
GiResult gi_dlc(GiSet *s, int part, const char *out_dir, void (*progress)(uint64_t done, uint64_t total), char *why,
                size_t wn);
void gi_drop(GiSet *s, int part);  // deletes that unpacked NCA (room for what follows)
void gi_close(GiSet *s);           // deletes the scratch folder
void gi_cancel(void);              // (any thread) ends the running hactool; the call returns GI_E_CANCELLED

#ifdef __cplusplus
}
#endif
