// rawdanger-recomp (v0.10.5): frame generation, the camera between two game steps.
//
// The game moves and draws its world 20 (or 30) times a second; each picture is shown on 3 (or 2) refreshes of a 60 Hz screen.
// The engine knows, for the picture on screen, its depth (copied when the 3D is finished, one value per game pixel) and the
// game's world-to-screen matrix it was drawn with (exact: the GS's x, y and depth are the matrix's x / w, y / w and z / w, and
// its Q is 1 / w). So each pixel can be moved to where the camera of a moment between two steps would show it: back to the
// world with the inverse of the matrix it was drawn with, then onto the screen with the camera of that moment (one 4 x 4 matrix,
// T, made on the processor). The HUD's boxes stay where they are.
//
// Three passes over the picture's samples: clear the keys; splat (each sample's place in the new picture, the nearest one wins:
// the game's depth is bigger nearer); resolve (the winner writes its colour); fill (a place nobody landed on, behind something
// that moved aside, takes the farthest neighbour's colour). The result is copied back over the picture.

struct Params {
    inv: array<vec4<f32>, 4>,     // the inverse of the matrix the picture was drawn with (rows): (x, y, depth, 1) * inv is the world
    mf: array<vec4<f32>, 4>,      // the camera of the moment shown (rows): (world, 1) * mf is the new place
    size: vec4<u32>,              // width, height (samples), scale, the picture's first word in the scratch buffer
    off: vec4<f32>,               // the scene's window offset (GS pixels, x y), game width, game height
    count: vec4<u32>,             // HUD boxes, the depth snapshot's width (game pixels), the hero's limbs
    motion: vec4<f32>,            // x: how far the hero is moved along its last step's move (the moment - 1: 0 at the step itself)
    boxes: array<vec4<f32>, 32>,  // the HUD's boxes in game pixels: x0 y0 x1 y1
    limb_a: array<vec4<f32>, 12>, // the hero's limbs in the world: one end and the radius
    limb_b: array<vec4<f32>, 12>, // the other end
    move_a: array<vec4<f32>, 12>, // how far each end moved in the last step
    move_b: array<vec4<f32>, 12>,
    caps: array<vec4<f32>, 12>,   // the same limbs on the screen (game pixels): x0 y0 x1 y1
    caps_z: array<vec4<f32>, 12>, // their radius on the screen, the depth at each end, the limb's number (2: the head)
}

@group(0) @binding(0) var<storage, read> picture: array<u32>;
@group(0) @binding(1) var<storage, read_write> work: array<atomic<u32>>;   // keys (n), colours (n), the result (n)
@group(0) @binding(2) var<storage, read> depth: array<u32>;
@group(0) @binding(3) var<uniform> params: Params;
// v0.10.6: the characters and their bones. [0]: how many characters, how many bones; then 64 characters, two each: the middle of its
// bones and a radius round them, then its first bone and how many; then the bones from vec4 129, six each: the parent's place, its
// own, and the four rows of the matrix that takes a point on the bone from the step drawn to the moment shown
@group(0) @binding(4) var<storage, read> bones: array<vec4<f32>>;

// the bone a world point is on (the nearest segment within 35 cm, of a character whose sphere holds it), -1 if none
fn bone_at(p: vec3<f32>) -> i32 {
    let chars = min(u32(bones[0].x), 64u);
    var best = 0.35;
    var found = -1;
    for (var c = 0u; c < chars; c = c + 1u) {
        let sphere = bones[1u + c * 2u];
        if (distance(p, sphere.xyz) > sphere.w) {
            continue;
        }
        let range = bones[2u + c * 2u];
        let first = u32(range.x);
        let count = u32(range.y);
        for (var b = first; b < first + count; b = b + 1u) {
            let at = 129u + b * 6u;
            let a = bones[at].xyz;
            let ab = bones[at + 1u].xyz - a;
            let t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
            let d = length(p - (a + ab * t));
            if (d < best) {
                best = d;
                found = i32(b);
            }
        }
    }
    return found;
}

fn in_hud(gx: f32, gy: f32) -> bool {
    let n = min(params.count.x, 32u);
    for (var i = 0u; i < n; i = i + 1u) {
        let b = params.boxes[i];
        if (gx >= b.x - 2.0 && gx < b.z + 2.0 && gy >= b.y - 2.0 && gy < b.w + 2.0) {
            return true;
        }
    }
    return false;
}

// The depth at a game pixel, with thin near things left out: the rain's streaks write their depth one or two pixels wide; a pixel
// whose neighbours two to the left and two to the right are both much farther is taken as what is behind it
fn zat(x: i32, y: i32) -> u32 {
    let gw = i32(params.count.y);
    let gh = i32(params.off.w);
    let cx = clamp(x, 0, gw - 1);
    let cy = clamp(y, 0, gh - 1);
    let z = depth[u32(cy * gw + cx)];
    let zl = depth[u32(cy * gw + clamp(cx - 2, 0, gw - 1))];
    let zr = depth[u32(cy * gw + clamp(cx + 2, 0, gw - 1))];
    let margin = max(z / 4u, 256u);
    if (zl + margin < z && zr + margin < z) {
        return max(zl, zr);
    }
    return z;
}

struct Landing {
    ok: bool,
    bone: i32,    // the bone it moved with, -1 none (the debug view)
    x: i32,       // the top left of the 2 x 2 samples it covers
    y: i32,
    key: u32,
}

// where the sample (x, y) lands, and its key (bigger nearer; the HUD the biggest)
fn land(x: u32, y: u32) -> Landing {
    var r: Landing;
    r.ok = false;
    r.bone = -1;
    let w = params.size.x;
    let h = params.size.y;
    let s = f32(params.size.z);
    let gx = (f32(x) + 0.5) / s;
    let gy = (f32(y) + 0.5) / s;
    if (in_hud(gx, gy)) {
        r.ok = true;
        r.x = i32(x);
        r.y = i32(y);
        r.key = 0xFFFFFFFFu;
        return r;
    }
    let ix = i32(gx);
    let iy = i32(gy);
    let z = zat(ix, iy);
    // a sample of the background right beside something nearer carries some of its colour (its edge is blended, blurred, has a
    // glow): it does not land, the gap it leaves is filled from the background further out (no halo of the near thing)
    let margin = max(z / 4u, 256u);
    for (var k = 0u; k < 4u; k = k + 1u) {
        let dx = select(select(0, 2, k == 1u), -2, k == 0u);
        let dy = select(select(0, 2, k == 3u), -2, k == 2u);
        if (zat(ix + dx, iy + dy) > z + margin) {
            return r;
        }
    }
    // The hero's hair and other see-through parts write no depth: their pixels have the depth of what is behind and would move
    // like the background (a copy of the hair left behind). A pixel on one of the hero's limbs on the screen whose depth is farther
    // than the limb takes the limb's depth: it moves with the hero.
    var zf = f32(z);
    let limbs_on_screen = min(params.count.z, 12u);
    for (var i = 0u; i < limbs_on_screen; i = i + 1u) {
        let c = params.caps[i];
        let a = c.xy;
        let ab = c.zw - a;
        let t = clamp(dot(vec2<f32>(gx, gy) - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        let d = length(vec2<f32>(gx, gy) - (a + ab * t));
        let zc = mix(params.caps_z[i].y, params.caps_z[i].z, t);
        // only the head's (the hair): elsewhere the hero writes its depth, and the background beside a limb is not the limb
        if (params.caps_z[i].w == 2.0 && d < params.caps_z[i].x * 1.1 && zf < zc * 0.8) {
            zf = zc;
        }
    }
    let px = gx + params.off.x;
    let py = gy + params.off.y;
    let hw = px * params.inv[0] + py * params.inv[1] + zf * params.inv[2] + params.inv[3];
    if (abs(hw.w) < 1e-20) {
        return r;
    }
    var p = hw.xyz / hw.w;
    // v0.10.6: on a character: moved with its bone, turn and all, to the moment shown
    if (u32(bones[0].y) > 0u) {
        let b = bone_at(p);
        r.bone = b;
        if (b >= 0) {
            let at = 129u + u32(b) * 6u;
            p = p.x * bones[at + 2u].xyz + p.y * bones[at + 3u].xyz + p.z * bones[at + 4u].xyz + bones[at + 5u].xyz;
        }
    }
    // without the bones (a game that does not send them): the hero moves with the limb it is on
    let limbs = select(min(params.count.z, 12u), 0u, u32(bones[0].y) > 0u);
    var best = 1e30;
    var moved = vec3<f32>(0.0);
    for (var i = 0u; i < limbs; i = i + 1u) {
        let a = params.limb_a[i].xyz;
        let ab = params.limb_b[i].xyz - a;
        let t = clamp(dot(p - a, ab) / max(dot(ab, ab), 1e-6), 0.0, 1.0);
        let d = length(p - (a + ab * t));
        if (d < params.limb_a[i].w * 1.6 && d < best) {
            best = d;
            moved = mix(params.move_a[i].xyz, params.move_b[i].xyz, t);
        }
    }
    p = p + moved * params.motion.x;
    let q = p.x * params.mf[0] + p.y * params.mf[1] + p.z * params.mf[2] + params.mf[3];
    if (!(q.w > 1e-6)) {
        return r;
    }
    let nx = (q.x / q.w - params.off.x) * s;
    let ny = (q.y / q.w - params.off.y) * s;
    if (!(nx >= -1.0 && ny >= -1.0 && nx < f32(w) && ny < f32(h))) {
        return r;
    }
    r.ok = true;
    r.x = i32(floor(nx - 0.5));
    r.y = i32(floor(ny - 0.5));
    r.key = (u32(zf) >> 8u) + 1u;
    return r;
}

@compute @workgroup_size(64)
fn clear(@builtin(global_invocation_id) gid: vec3<u32>) {
    let i = gid.x + gid.y * 64u * 1024u;
    let n = params.size.x * params.size.y;
    if (i >= n) {
        return;
    }
    atomicStore(&work[i], 0u);
}

// each sample covers 2 x 2 places where it lands, so a picture stretched by the move has no cracks; the nearest wins each place
@compute @workgroup_size(8, 8)
fn splat(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.size.x || gid.y >= params.size.y) {
        return;
    }
    let l = land(gid.x, gid.y);
    if (!l.ok) {
        return;
    }
    let w = i32(params.size.x);
    let h = i32(params.size.y);
    let size = select(2, 1, l.key == 0xFFFFFFFFu);
    for (var dy = 0; dy < size; dy = dy + 1) {
        for (var dx = 0; dx < size; dx = dx + 1) {
            let x = l.x + dx;
            let y = l.y + dy;
            if (x >= 0 && y >= 0 && x < w && y < h) {
                atomicMax(&work[u32(y * w + x)], l.key);
            }
        }
    }
}

@compute @workgroup_size(8, 8)
fn resolve(@builtin(global_invocation_id) gid: vec3<u32>) {
    if (gid.x >= params.size.x || gid.y >= params.size.y) {
        return;
    }
    let l = land(gid.x, gid.y);
    if (!l.ok) {
        return;
    }
    let w = i32(params.size.x);
    let h = i32(params.size.y);
    let n = params.size.x * params.size.y;
    var colour = picture[params.size.w + gid.y * params.size.x + gid.x];
    if (params.motion.y > 0.5) {   // debug: the depth instead of the colour (bright near)
        let g = min(l.key * 2u, 255u);
        colour = g | (g << 8u) | (g << 16u);
    }
    if (params.motion.w > 0.5 && l.bone >= 0) {   // debug: the pixels moved with a bone, a colour per bone over the picture
        let hsh = u32(l.bone) * 2654435761u;
        let tint = vec3<u32>((hsh >> 8u) & 255u, (hsh >> 16u) & 255u, (hsh >> 24u) & 255u);
        let c = vec3<u32>(colour & 255u, (colour >> 8u) & 255u, (colour >> 16u) & 255u);
        let m = (c + tint * 3u) / 4u;
        colour = m.x | (m.y << 8u) | (m.z << 16u);
    }
    let size = select(2, 1, l.key == 0xFFFFFFFFu);
    for (var dy = 0; dy < size; dy = dy + 1) {
        for (var dx = 0; dx < size; dx = dx + 1) {
            let x = l.x + dx;
            let y = l.y + dy;
            if (x >= 0 && y >= 0 && x < w && y < h) {
                let at = u32(y * w + x);
                if (atomicLoad(&work[at]) == l.key) {
                    atomicStore(&work[n + at], colour);
                }
            }
        }
    }
}

// A place nobody landed on: what was behind something that moved aside. The nearest landed place to the left and to the right (up
// to 48 game pixels away; above and below when neither has one), and of those the farthest from the camera: the background,
// stretched over the gap.
@compute @workgroup_size(8, 8)
fn fill(@builtin(global_invocation_id) gid: vec3<u32>) {
    let w = params.size.x;
    let h = params.size.y;
    if (gid.x >= w || gid.y >= h) {
        return;
    }
    let n = w * h;
    let i = gid.y * w + gid.x;
    var colour = 0u;
    if (atomicLoad(&work[i]) != 0u) {
        colour = atomicLoad(&work[n + i]);
    } else {
        let reach = i32(params.size.z * 48u);
        var best = 0xFFFFFFFFu;
        var found = false;
        let dirs = array<vec2<i32>, 4>(vec2<i32>(-1, 0), vec2<i32>(1, 0), vec2<i32>(0, -1), vec2<i32>(0, 1));
        // left and right first (the camera mostly turns about the up axis: what opens is beside things), up and down only when
        // neither side has anything
        for (var d = 0u; d < 4u; d = d + 1u) {
            if (d == 2u && found) {
                break;
            }
            for (var step = 1; step <= reach; step = step + 1) {
                let x = i32(gid.x) + dirs[d].x * step;
                let y = i32(gid.y) + dirs[d].y * step;
                if (x < 0 || y < 0 || x >= i32(w) || y >= i32(h)) {
                    break;
                }
                let j = u32(y) * w + u32(x);
                let k = atomicLoad(&work[j]);
                if (k != 0u) {
                    // further in than the first landed place: its edge may still carry the near thing's colour (blended edges, the
                    // game's motion blur trailing behind a moving character)
                    let x2 = clamp(x + dirs[d].x * i32(params.size.z) * 6, 0, i32(w) - 1);
                    let y2 = clamp(y + dirs[d].y * i32(params.size.z) * 6, 0, i32(h) - 1);
                    let j2 = u32(y2) * w + u32(x2);
                    let k2 = atomicLoad(&work[j2]);
                    let use_j = select(j, j2, k2 != 0u && k2 != 0xFFFFFFFFu && k2 <= k);
                    let use_k = select(k, k2, k2 != 0u && k2 != 0xFFFFFFFFu && k2 <= k);
                    if (use_k != 0xFFFFFFFFu && use_k < best) {
                        best = use_k;
                        colour = atomicLoad(&work[n + use_j]);
                        found = true;
                    }
                    break;
                }
            }
        }
        if (!found) {
            colour = picture[params.size.w + i];
        }
        if (params.motion.z > 0.5) {   // debug: the holes plainly (magenta)
            colour = 0x00FF00FFu;
        }
    }
    atomicStore(&work[2u * n + i], colour);
}
