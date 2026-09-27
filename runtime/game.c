/* game.c - the few facts about Catz that pcrecomp's Win16 host runtime needs.
 * See <pcrecomp>/runtime/win16/host/recomp_game.h for what each field does. */
#include "recomp_game.h"

const RecompGame recomp_game = {
    .title      = "Catz",
    .guest_dir  = "C:\\CATZ",
    /* LASTCAT.INI holds "This Last Cat", the pet file loaded at startup. */
    .ini_fallbacks = { "LASTCAT.INI", "KITTENZ.INI", NULL },
    /* The engine builds C:\CATZ\ptzfiles\... paths for its pet data. */
    .path_anchor = "ptzfiles",
    /* A save from before the backdrop key existed has none, and an empty
       name locked the screen after adopting a cat. */
    .ini_defaults = { "playpenBitmap", "PLAIN.BMP", NULL },
    .palette_type = 0x7F03, .palette_id = 10256,   /* PALT 10256 */
    .resource_tags = 1,
    .resource_fallback = 1,     /* the engine asks the WAD for CATZDLL strings */
    .serial_key = "Serial Number",
    .serial_help =
        "[catz] No serial number found. Catz (1996) asks for one at first run.\n"
        "[catz] Supply your own, from your own copy of the game, by creating\n"
        "[catz]   %s/catz.ini\n"
        "[catz] containing:\n"
        "[catz]   [Catz]\n"
        "[catz]   Serial Number=XXXX-XXXX-XXXX\n"
        "[catz] Without it the engine stops on its registration screen.\n",
};
