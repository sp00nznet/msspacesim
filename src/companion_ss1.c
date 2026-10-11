/* SS1.EXE, lifted beside the game (tools/lift.py COMPANIONS, prefix ss1_):
 * spacesim.exe boots it, and it runs the game with its music player loaded. */
#include "lifted.h"

extern const uint16_t g_ss1_load_seg, g_ss1_psp_seg;
extern const uint16_t g_ss1_entry_cs, g_ss1_entry_ip, g_ss1_entry_ss, g_ss1_entry_sp;
extern const unsigned g_ss1_func_count, g_ss1_image_len, g_ss1_nrelocs;
extern const RecompFunc g_ss1_funcs[];
extern const uint8_t g_ss1_image[];
extern const uint16_t g_ss1_relocs[][2];

const Program *companion(void)
{
    static Program p;
    if (!p.id)
        p = (Program){ "ss1", g_ss1_funcs, g_ss1_func_count, g_ss1_load_seg, g_ss1_psp_seg,
                       g_ss1_entry_cs, g_ss1_entry_ip, g_ss1_entry_ss, g_ss1_entry_sp,
                       g_ss1_image, g_ss1_image_len, g_ss1_relocs, g_ss1_nrelocs };
    return &p;
}
