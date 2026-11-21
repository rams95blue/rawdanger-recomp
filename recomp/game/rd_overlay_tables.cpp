// Default (empty) overlay table - replaced in the runner by the one
// tools/rd_overlay.py generates when you run `python rd.py overlays`.
#include "ps2_runtime.h"
#include "rd_overlay_tables.h"

extern const RdOverlayInfo g_rdOverlays[] = {{"", 0u, 0u, 0u, 0u, nullptr, 0u}};
extern const uint32_t g_rdOverlayCount = 0u;
