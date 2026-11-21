// SMAA's two lookup tables (AreaTex 160 x 560 rg8, SearchTex 64 x 16 r8), from the reference implementation (MIT licence,
// third_party/smaa/LICENSE.txt), packed one after the other for the smaa.wgsl compute passes (rawdanger-recomp).
#include <cstddef>
#include <cstring>
#include <vector>
#include "../../../../third_party/smaa/AreaTex.h"
#include "../../../../third_party/smaa/SearchTex.h"

namespace ps2::gs::gpu {
std::vector<unsigned char> smaa_tables() {
    std::vector<unsigned char> out(sizeof areaTexBytes + sizeof searchTexBytes + 4, 0);
    std::memcpy(out.data(), areaTexBytes, sizeof areaTexBytes);
    std::memcpy(out.data() + sizeof areaTexBytes, searchTexBytes, sizeof searchTexBytes);
    out.resize((out.size() + 3) / 4 * 4);
    return out;
}
}  // namespace ps2::gs::gpu
