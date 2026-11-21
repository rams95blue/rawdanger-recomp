const SHARPNESS: f32 = 0.14;
// Smooth edges: how slanted an edge must be (smaller direction part / larger one) and how far along it the blend may reach.
const EDGE_SLANT: f32 = 0.04;
const EDGE_REACH: f32 = 6.0;

struct Blit {
    width: u32,
    height: u32,
    current: u32,
    previous: u32,
    blend: u32,
    out_width: u32,
    out_height: u32,
    options: u32,
}

@group(0) @binding(0) var<storage, read> pixels: array<u32>;
@group(0) @binding(1) var<uniform> blit: Blit;

// rawdanger-recomp: rain on the lens (host/lens_rain.cpp). Each drop: a = centre x, y (0 ... 1 over the picture), radius (a share
// of the picture's height), opacity; b = the wet trail it left above it (picture heights), how much it is pulled long while it
// runs, a random 0 ... 1 for variety. A drop works like a small lens: it shows the picture behind it flipped and enlarged (the
// view through a water bead), with a darker rim, a light glint and a heavier, rounder bottom when it runs; the trail bends the
// picture a little and catches a thin line of light. mist: heavy rain fogs the lens, mostly towards its edges. Written for this
// project from how water on a lens looks, not from any other effect's code.
struct LensDrop {
    a: vec4<f32>,
    b: vec4<f32>,
}
struct Lens {
    count: u32,
    mist: f32,
    clock: f32,
    ui_count: u32,
    drops: array<LensDrop, 64>,
    ui: array<vec4<f32>, 32>,   // the game's HUD and texts (x0, y0, x1, y1 over the picture): the rain stays under them
}
@group(0) @binding(2) var<uniform> lens: Lens;

struct VertexOut {
    @builtin(position) position: vec4<f32>,
    @location(0) uv: vec2<f32>,
}

@vertex
fn vs(@builtin(vertex_index) i: u32) -> VertexOut {
    let p = vec2<f32>(f32((i << 1u) & 2u), f32(i & 2u));
    var o: VertexOut;
    o.position = vec4<f32>(p * 2.0 - 1.0, 0.0, 1.0);
    o.uv = vec2<f32>(p.x, 1.0 - p.y);
    return o;
}

fn texel(base: u32, x: i32, y: i32) -> vec4<f32> {
    let cx = u32(clamp(x, 0, i32(blit.width) - 1));
    let cy = u32(clamp(y, 0, i32(blit.height) - 1));
    return unpack4x8unorm(pixels[base + cy * blit.width + cx]);
}

// Three ways to enlarge the picture to the window (bit 2 below). Sharp (the default of bit 1 = 0): every PS2 pixel stays a hard square and only
// its border blends, the way an emulator shows it. Soft (bit 1): a plain smooth blend between neighbouring pixels everywhere,
// which is closest to what a TV did to the picture. Sharp (bit 2): see sharp_scene.
fn sample(base: u32, uv: vec2<f32>) -> vec4<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let t = uv * size;
    var p: vec2<f32>;
    if ((blit.options & 6u) != 0u) {
        p = t - 0.5;
    } else {
        let scale = max(floor(vec2<f32>(f32(blit.out_width), f32(blit.out_height)) / size), vec2<f32>(1.0));
        let d = fract(t) - 0.5;
        let region = 0.5 - 0.5 / scale;
        p = floor(t) + (d - clamp(d, -region, region)) * scale;
    }
    let origin = floor(p);
    let f = p - origin;
    let x = i32(origin.x);
    let y = i32(origin.y);
    let top = mix(texel(base, x, y), texel(base, x + 1, y), f.x);
    let bottom = mix(texel(base, x, y + 1), texel(base, x + 1, y + 1), f.x);
    return mix(top, bottom, f.y);
}

fn scene(uv: vec2<f32>) -> vec3<f32> {
    var c = sample(blit.current, uv).rgb;
    if (blit.blend != 0u) { c = (c + sample(blit.previous, uv).rgb) * 0.5; }
    return c;
}

// Catmull-Rom weights (the cubic that passes through every source pixel and has a little overshoot at edges) for the four source
// pixels at offsets -1, 0, 1, 2 around a position with fraction f. Published in 1974; this is the textbook polynomial.
fn catmull_rom(f: f32) -> vec4<f32> {
    let f2 = f * f;
    let f3 = f2 * f;
    return vec4<f32>(-0.5 * f3 + f2 - 0.5 * f, 1.5 * f3 - 2.5 * f2 + 1.0, -1.5 * f3 + 2.0 * f2 + 0.5 * f, 0.5 * f3 - 0.5 * f2);
}

// Bilinear weights over the four source columns (or rows) -1, 0, 1, 2 around the pixel at floor(p), for a position u (in pixels
// from floor(p), between -1 and 2).
fn linear_weights(u: f32) -> vec4<f32> {
    let b = floor(u);
    let t = u - b;
    if (b < 0.0) { return vec4<f32>(1.0 - t, t, 0.0, 0.0); }
    if (b < 1.0) { return vec4<f32>(0.0, 1.0 - t, t, 0.0); }
    return vec4<f32>(0.0, 0.0, 1.0 - t, t);
}

fn row4(a: vec3<f32>, b: vec3<f32>, c: vec3<f32>, d: vec3<f32>, w: vec4<f32>) -> vec3<f32> {
    return a * w.x + b * w.y + c * w.z + d * w.w;
}

// Sharp look: a Catmull-Rom cubic enlargement (text and the health bar keep their edges, unlike a plain blend), then a light
// contrast-adaptive sharpening: the pixel is pushed away from the average of its four neighbours on the screen, less where the
// neighbourhood is already near black or white (so that edges do not get halos). Written from the public description of the idea
// (the amount follows how much room there is to the nearest of 0 and 1), not from any source code.
// The 4x4 source pixels around the position are read once (16 reads; the soft look reads 4) into named variables (an array that is
// indexed while running was slower on the Intel chip), and the cubic and the four neighbour blends are all taken from them.
fn sharp_scene(uv: vec2<f32>) -> vec3<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let p = uv * size - 0.5;
    let origin = floor(p);
    let f = p - origin;
    let x = i32(origin.x);
    let y = i32(origin.y);
    let t00 = scene_texel(x + -1, y + -1);
    let t01 = scene_texel(x + 0, y + -1);
    let t02 = scene_texel(x + 1, y + -1);
    let t03 = scene_texel(x + 2, y + -1);
    let t10 = scene_texel(x + -1, y + 0);
    let t11 = scene_texel(x + 0, y + 0);
    let t12 = scene_texel(x + 1, y + 0);
    let t13 = scene_texel(x + 2, y + 0);
    let t20 = scene_texel(x + -1, y + 1);
    let t21 = scene_texel(x + 0, y + 1);
    let t22 = scene_texel(x + 1, y + 1);
    let t23 = scene_texel(x + 2, y + 1);
    let t30 = scene_texel(x + -1, y + 2);
    let t31 = scene_texel(x + 0, y + 2);
    let t32 = scene_texel(x + 1, y + 2);
    let t33 = scene_texel(x + 2, y + 2);
    // one screen pixel in source pixels (at most one source pixel, so the neighbours stay inside the 4x4 block)
    let d = clamp(size / vec2<f32>(f32(blit.out_width), f32(blit.out_height)), vec2<f32>(0.0), vec2<f32>(0.99));
    let cx = catmull_rom(f.x);
    let cy = catmull_rom(f.y);
    let lx = linear_weights(f.x);
    let ly = linear_weights(f.y);
    let wxm = linear_weights(f.x - d.x);
    let wxp = linear_weights(f.x + d.x);
    let wyn = linear_weights(f.y - d.y);
    let wys = linear_weights(f.y + d.y);
    let c = row4(t00, t01, t02, t03, cx) * cy.x + row4(t10, t11, t12, t13, cx) * cy.y + row4(t20, t21, t22, t23, cx) * cy.z + row4(t30, t31, t32, t33, cx) * cy.w;
    let n = row4(t00, t01, t02, t03, lx) * wyn.x + row4(t10, t11, t12, t13, lx) * wyn.y + row4(t20, t21, t22, t23, lx) * wyn.z + row4(t30, t31, t32, t33, lx) * wyn.w;
    let s = row4(t00, t01, t02, t03, lx) * wys.x + row4(t10, t11, t12, t13, lx) * wys.y + row4(t20, t21, t22, t23, lx) * wys.z + row4(t30, t31, t32, t33, lx) * wys.w;
    let w = row4(t00, t01, t02, t03, wxm) * ly.x + row4(t10, t11, t12, t13, wxm) * ly.y + row4(t20, t21, t22, t23, wxm) * ly.z + row4(t30, t31, t32, t33, wxm) * ly.w;
    let e = row4(t00, t01, t02, t03, wxp) * ly.x + row4(t10, t11, t12, t13, wxp) * ly.y + row4(t20, t21, t22, t23, wxp) * ly.z + row4(t30, t31, t32, t33, wxp) * ly.w;
    let mn = min(min(min(n, s), min(w, e)), c);
    let mx = max(max(max(n, s), max(w, e)), c);
    let amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3<f32>(0.0001)), vec3<f32>(0.0), vec3<f32>(1.0)));
    let k = -amp * SHARPNESS;
    return (c + (n + s + w + e) * k) / (1.0 + 4.0 * k);
}

fn luma(c: vec3<f32>) -> f32 { return dot(c, vec3<f32>(0.299, 0.587, 0.114)); }

// The colour of one source pixel (no blending between pixels): the cheap read the edge test uses.
fn scene_texel(x: i32, y: i32) -> vec3<f32> {
    var c = texel(blit.current, x, y).rgb;
    if (blit.blend != 0u) { c = (c + texel(blit.previous, x, y).rgb) * 0.5; }
    return c;
}

fn edge_aa(uv: vec2<f32>, center: vec3<f32>) -> vec3<f32> {
    // Fast approximate edge AA, evaluated in image space. Flat regions return
    // unchanged, while edge direction controls subpixel reconstruction.
    // The edge test reads the five source pixels around this one (one read each, instead of a smooth
    // blend per neighbour): most of the picture is flat and stops here, so this is what the picture costs.
    let texel_step = 1.0 / vec2<f32>(f32(blit.width), f32(blit.height));
    let c = vec2<i32>(floor(uv * vec2<f32>(f32(blit.width), f32(blit.height))));
    let nw = luma(scene_texel(c.x - 1, c.y - 1));
    let ne = luma(scene_texel(c.x + 1, c.y - 1));
    let sw = luma(scene_texel(c.x - 1, c.y + 1));
    let se = luma(scene_texel(c.x + 1, c.y + 1));
    let m = luma(scene_texel(c.x, c.y));
    let low = min(m, min(min(nw, ne), min(sw, se)));
    let high = max(m, max(max(nw, ne), max(sw, se)));
    if (high - low < max(0.0312, high * 0.125)) { return center; }
    var direction = vec2<f32>(-((nw + ne) - (sw + se)), (nw + sw) - (ne + se));
    let reduce = max((nw + ne + sw + se) * 0.03125, 0.0078125);
    // The dotted noise the first version left along hard edges (health bar ends, text) came from here: an edge that is nearly
    // straight up/down or left/right got a direction of up to 8 source pixels, so the blend reached far along the edge and pulled in
    // unrelated dark and bright pixels as dots and ticks. A straight edge has no stairs to smooth, so it is left alone, and the
    // reach along a slanted one is at most EDGE_REACH source pixels.
    let ax = abs(direction.x);
    let ay = abs(direction.y);
    if (min(ax, ay) < max(max(ax, ay) * EDGE_SLANT, reduce)) { return center; }
    direction = clamp(direction / (min(ax, ay) + reduce), vec2<f32>(-EDGE_REACH), vec2<f32>(EDGE_REACH)) * texel_step;
    // The blends along the edge are taken from the plain smooth blend (4 reads each; the cubic would cost 16 each), and what they change
    // against the plain blend at this pixel is added to `center`. For the soft and crisp looks `center` is that blend itself, so the
    // result is the blend along the edge; for Sharp the cubic and the sharpening stay (text stays crisp) and the stairs are smoothed on top.
    let plain = scene(uv);
    let a = 0.5 * (scene(uv - direction / 6.0) + scene(uv + direction / 6.0));
    let b = a * 0.5 + 0.25 * (scene(uv - direction * 0.5) + scene(uv + direction * 0.5));
    let lb = luma(b);
    return center + (select(b, a, lb < low || lb > high) - plain);
}

// rawdanger-recomp: FSR 1 upscale filter (the look with bits 1 and 2 both set). EASU, the edge-adaptive spatial upsampling half of
// AMD FidelityFX Super Resolution 1 (MIT licence, (c) Advanced Micro Devices), written out for this shader: 12 source pixels around the
// position, the local edge direction and length from their brightness, then a Lanczos-like lobe stretched along the edge and
// clamped to the nearest four pixels (no halos). RCAS, FSR's second pass, is left out: it needs the enlarged picture in a texture,
// and the picture is drawn straight to the window here; the enlargement alone already keeps edges clean without the blur of Soft.
fn easu_luma(c: vec3<f32>) -> f32 { return c.b * 0.5 + (c.r * 0.5 + c.g); }

fn easu_set(w: f32, la: f32, lb: f32, lc: f32, ld: f32, le: f32, dir: ptr<function, vec2<f32>>, len: ptr<function, f32>) {
    let dc = ld - lc;
    let cb = lc - lb;
    var lx = max(abs(dc), abs(cb));
    lx = select(1.0 / lx, 0.0, lx <= 0.0);
    let dx = ld - lb;
    lx = clamp(abs(dx) * lx, 0.0, 1.0);
    let ec = le - lc;
    let ca = lc - la;
    var ly = max(abs(ec), abs(ca));
    ly = select(1.0 / ly, 0.0, ly <= 0.0);
    let dy = le - la;
    ly = clamp(abs(dy) * ly, 0.0, 1.0);
    *dir = *dir + vec2<f32>(dx, dy) * w;
    *len = *len + (lx * lx + ly * ly) * w;
}

fn easu_tap(off: vec2<f32>, dir: vec2<f32>, len2: vec2<f32>, lob: f32, clp: f32, c: vec3<f32>, acc: ptr<function, vec4<f32>>) {
    var v = vec2<f32>(off.x * dir.x + off.y * dir.y, off.x * -dir.y + off.y * dir.x) * len2;
    let d2 = min(dot(v, v), clp);
    var wb = 0.4 * d2 - 1.0;
    var wa = lob * d2 - 1.0;
    wb = wb * wb;
    wa = wa * wa;
    wb = 1.5625 * wb - 0.5625;
    let w = wb * wa;
    *acc = *acc + vec4<f32>(c * w, w);
}

fn fsr_scene(uv: vec2<f32>) -> vec3<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    var pp = uv * size - 0.5;
    let fp = floor(pp);
    pp = pp - fp;
    let x = i32(fp.x);
    let y = i32(fp.y);
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    let b = scene_texel(x, y - 1);
    let c = scene_texel(x + 1, y - 1);
    let e = scene_texel(x - 1, y);
    let f = scene_texel(x, y);
    let g = scene_texel(x + 1, y);
    let h = scene_texel(x + 2, y);
    let i = scene_texel(x - 1, y + 1);
    let j = scene_texel(x, y + 1);
    let k = scene_texel(x + 1, y + 1);
    let l = scene_texel(x + 2, y + 1);
    let n = scene_texel(x, y + 2);
    let o = scene_texel(x + 1, y + 2);
    let bl = easu_luma(b); let cl = easu_luma(c); let el = easu_luma(e); let fl = easu_luma(f);
    let gl = easu_luma(g); let hl = easu_luma(h); let il = easu_luma(i); let jl = easu_luma(j);
    let kl = easu_luma(k); let ll = easu_luma(l); let nl = easu_luma(n); let ol = easu_luma(o);
    var dir = vec2<f32>(0.0);
    var len = 0.0;
    easu_set((1.0 - pp.x) * (1.0 - pp.y), bl, el, fl, gl, jl, &dir, &len);
    easu_set(pp.x * (1.0 - pp.y), cl, fl, gl, hl, kl, &dir, &len);
    easu_set((1.0 - pp.x) * pp.y, fl, il, jl, kl, nl, &dir, &len);
    easu_set(pp.x * pp.y, gl, jl, kl, ll, ol, &dir, &len);
    let dr2 = dot(dir, dir);
    if (dr2 < 1.0 / 32768.0) {
        dir = vec2<f32>(1.0, 0.0);
    } else {
        dir = dir * inverseSqrt(dr2);
    }
    len = len * 0.5;
    len = len * len;
    let stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    let len2 = vec2<f32>(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    let lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    let clp = 1.0 / lob;
    var acc = vec4<f32>(0.0);
    easu_tap(vec2<f32>(0.0, -1.0) - pp, dir, len2, lob, clp, b, &acc);
    easu_tap(vec2<f32>(1.0, -1.0) - pp, dir, len2, lob, clp, c, &acc);
    easu_tap(vec2<f32>(-1.0, 1.0) - pp, dir, len2, lob, clp, i, &acc);
    easu_tap(vec2<f32>(0.0, 1.0) - pp, dir, len2, lob, clp, j, &acc);
    easu_tap(vec2<f32>(0.0, 0.0) - pp, dir, len2, lob, clp, f, &acc);
    easu_tap(vec2<f32>(-1.0, 0.0) - pp, dir, len2, lob, clp, e, &acc);
    easu_tap(vec2<f32>(1.0, 1.0) - pp, dir, len2, lob, clp, k, &acc);
    easu_tap(vec2<f32>(2.0, 1.0) - pp, dir, len2, lob, clp, l, &acc);
    easu_tap(vec2<f32>(2.0, 0.0) - pp, dir, len2, lob, clp, h, &acc);
    easu_tap(vec2<f32>(1.0, 0.0) - pp, dir, len2, lob, clp, g, &acc);
    easu_tap(vec2<f32>(1.0, 2.0) - pp, dir, len2, lob, clp, o, &acc);
    easu_tap(vec2<f32>(0.0, 2.0) - pp, dir, len2, lob, clp, n, &acc);
    let lo = min(min(f, g), min(j, k));
    let hi = max(max(f, g), max(j, k));
    return min(hi, max(lo, acc.rgb / max(acc.a, 0.0001)));
}

// A plain smooth blend of the four source pixels around p (in source pixels, from the corner of the first).
fn smooth_at(p: vec2<f32>) -> vec3<f32> {
    let origin = floor(p);
    let f = p - origin;
    let x = i32(origin.x);
    let y = i32(origin.y);
    let top = mix(scene_texel(x, y), scene_texel(x + 1, y), f.x);
    let bottom = mix(scene_texel(x, y + 1), scene_texel(x + 1, y + 1), f.x);
    return mix(top, bottom, f.y);
}

// A picture bigger than the window (an upscaled one in a smaller window): the looks above enlarge, and reading a few source pixels
// per screen pixel skips the rest, which put jagged steps along thin edges such as text. Here every screen pixel is the average of
// the source area it covers (2 to 6 smooth reads each way), the clean reduction that upscaling is for; it needs no edge smoothing.
fn reduced_scene(uv: vec2<f32>) -> vec3<f32> {
    let size = vec2<f32>(f32(blit.width), f32(blit.height));
    let footprint = size / vec2<f32>(f32(blit.out_width), f32(blit.out_height));
    let n = vec2<i32>(clamp(ceil(footprint) + 1.0, vec2<f32>(2.0), vec2<f32>(6.0)));
    let centre = uv * size - 0.5;
    var sum = vec3<f32>(0.0);
    for (var j = 0; j < n.y; j = j + 1) {
        for (var i = 0; i < n.x; i = i + 1) {
            let offset = ((vec2<f32>(f32(i), f32(j)) + 0.5) / vec2<f32>(n) - 0.5) * footprint;
            sum = sum + smooth_at(centre + offset);
        }
    }
    return sum / f32(n.x * n.y);
}

fn lens_mist(uv: vec2<f32>, picture: vec3<f32>, on_ui: bool) -> vec3<f32> {
    // a soft look through fogged glass: the picture spread over a few pixels, a little lifted and cooler, more at the edges.
    // Over the game's HUD and texts only the lift (no spreading): the same brightness as around them, the letters stay sharp.
    let px = vec2<f32>(2.5 / max(f32(blit.out_width), 1.0), 2.5 / max(f32(blit.out_height), 1.0));
    var spread = picture;
    if (!on_ui) {
        spread = (scene(clamp(uv + vec2<f32>(px.x, px.y), vec2<f32>(0.0), vec2<f32>(1.0))) +
                  scene(clamp(uv + vec2<f32>(-px.x, px.y), vec2<f32>(0.0), vec2<f32>(1.0))) +
                  scene(clamp(uv + vec2<f32>(px.x, -px.y), vec2<f32>(0.0), vec2<f32>(1.0))) +
                  scene(clamp(uv + vec2<f32>(-px.x, -px.y), vec2<f32>(0.0), vec2<f32>(1.0)))) * 0.25;
    }
    let edge = smoothstep(0.2, 0.75, length((uv - vec2<f32>(0.5)) * vec2<f32>(1.0, 1.25)));
    let fogged = spread * 0.9 + vec3<f32>(0.05, 0.06, 0.075);
    return mix(picture, fogged, clamp(lens.mist, 0.0, 1.0) * (0.2 + 0.8 * edge) * 0.65);
}

// rawdanger-recomp: contrast-adaptive sharpening (options bits 5-6: 0 off, 1 low, 2 medium, 3 high). The picture against the
// mean of its four neighbours one source pixel away, pushed apart by an amount that shrinks where the neighbourhood already has
// strong contrast (no halos on hard edges, more on soft detail), kept inside the neighbourhood's range. Written for this project.
fn sharpen(uv: vec2<f32>, c: vec3<f32>, level: u32) -> vec3<f32> {
    let px = vec2<f32>(1.0 / max(f32(blit.width), 1.0), 1.0 / max(f32(blit.height), 1.0));
    let n = scene(clamp(uv - vec2<f32>(0.0, px.y), vec2<f32>(0.0), vec2<f32>(1.0)));
    let s = scene(clamp(uv + vec2<f32>(0.0, px.y), vec2<f32>(0.0), vec2<f32>(1.0)));
    let w = scene(clamp(uv - vec2<f32>(px.x, 0.0), vec2<f32>(0.0), vec2<f32>(1.0)));
    let e = scene(clamp(uv + vec2<f32>(px.x, 0.0), vec2<f32>(0.0), vec2<f32>(1.0)));
    let mn = min(c, min(min(n, s), min(w, e)));
    let mx = max(c, max(max(n, s), max(w, e)));
    let room = clamp(min(mn, vec3<f32>(1.0) - mx) / max(mx, vec3<f32>(0.0001)), vec3<f32>(0.0), vec3<f32>(1.0));
    let strength = select(select(1.2, 0.8, level == 2u), 0.45, level == 1u);
    let k = sqrt(room) * strength;
    let detail = c - (n + s + w + e) * 0.25;
    return clamp(c + detail * k, mn, mx);
}

fn over_ui(uv: vec2<f32>) -> bool {
    let n = min(lens.ui_count, 32u);
    for (var i = 0u; i < n; i = i + 1u) {
        let b = lens.ui[i];
        if (uv.x >= b.x && uv.y >= b.y && uv.x <= b.z && uv.y <= b.w) { return true; }
    }
    return false;
}

fn drops_over(uv: vec2<f32>, picture: vec3<f32>, on_ui: bool) -> vec3<f32> {
    var c = picture;
    if (lens.mist > 0.01) { c = lens_mist(uv, c, on_ui); }
    if (on_ui) { return c; }   // the drops stay under the game's 2D
    let aspect = f32(blit.out_width) / max(f32(blit.out_height), 1.0);
    let n = min(lens.count, 64u);
    for (var i = 0u; i < n; i = i + 1u) {
        let d = lens.drops[i].a;
        let e = lens.drops[i].b;
        let r = max(d.z, 0.0001);
        let rel = (uv - d.xy) * vec2<f32>(aspect, 1.0);
        // v0.8.1: water as it looks on a real lens: nearly clear, the picture behind it only bent a little and a touch brighter, a
        // soft edge, one small bright highlight; no dark ring. The trail: a short wet smear along the way the drop came (e.w: its
        // slant, picture widths across per height down), the picture in it slightly smeared, fading fast.
        if (e.x > 0.002 && rel.y < 0.0) {
            let along = -rel.y;
            let off = rel.x + rel.y * e.w * aspect;   // across the trail
            if (along < e.x) {
                let t = along / e.x;
                let half_w = r * (0.55 - 0.35 * t);
                let dx = abs(off) / max(half_w, 0.0001);
                if (dx < 1.0) {
                    let bend = vec2<f32>(off * 0.35 / aspect, -0.002);
                    let smear = (scene(clamp(uv - bend, vec2<f32>(0.0), vec2<f32>(1.0))) +
                                 scene(clamp(uv - bend + vec2<f32>(0.0, 0.004), vec2<f32>(0.0), vec2<f32>(1.0)))) * 0.5;
                    let wet = smear * 1.03 + vec3<f32>(0.012);
                    c = mix(c, wet, d.w * (1.0 - t) * (1.0 - t) * (1.0 - dx * dx) * 0.55);
                }
            }
        }
        // the bead: rounder at its bottom while it runs, pulled along its path
        var q = rel;
        q.y = q.y / select(mix(1.0, e.y, 0.4), e.y, q.y < 0.0);
        q.x = q.x - q.y * e.w * aspect * 0.5;
        let dist = length(q) / r;
        if (dist >= 1.0) { continue; }
        let normal = q / r;
        let h = sqrt(max(0.0, 1.0 - dist * dist));   // the bead's height: a dome
        // the view through it: a small upside-down picture of what is around it (a bead is a tiny lens), squeezed at its edge
        let through = d.xy - normal * vec2<f32>(r / aspect, r) * 2.2 * (1.0 - 0.45 * dist * dist);
        var view = scene(clamp(through, vec2<f32>(0.0), vec2<f32>(1.0)));
        view = view * (1.05 + 0.08 * h) + vec3<f32>(0.02);
        // the edge: a little darker where the light leaves the water sideways, a light crescent at its bottom
        let edge = smoothstep(0.72, 0.97, dist);
        view = view * (1.0 - 0.3 * edge * (1.0 - 0.6 * max(normal.y, 0.0)));
        view = view + vec3<f32>(edge * max(normal.y, 0.0) * max(normal.y, 0.0) * 0.12);
        // one small highlight up and left, from the sky
        let spec = pow(max(dot(normalize(vec3<f32>(normal, h)), normalize(vec3<f32>(-0.45, -0.6, 0.66))), 0.0), 40.0);
        view = view + vec3<f32>(spec * 0.45);
        let alpha = d.w * (1.0 - smoothstep(0.82, 1.0, dist));
        c = mix(c, view, alpha);
    }
    return c;
}

@fragment
fn fs(in: VertexOut) -> @location(0) vec4<f32> {
    if ((blit.options & 128u) != 0u) { return vec4<f32>(0.0, 0.0, 0.0, 1.0); }   // rawdanger-recomp: a scene the player does not see
    var c: vec3<f32>;
    if (blit.width > blit.out_width + blit.out_width / 16u || blit.height > blit.out_height + blit.out_height / 16u) {
        c = reduced_scene(in.uv);
    } else {
        if ((blit.options & 6u) == 6u) {
            c = fsr_scene(in.uv);
        } else if ((blit.options & 4u) != 0u) {
            c = sharp_scene(in.uv);
        } else {
            c = scene(in.uv);
        }
        if ((blit.options & 1u) != 0u) { c = edge_aa(in.uv, c); }
        let sharpen_level = (blit.options >> 5u) & 3u;
        if (sharpen_level != 0u) { c = sharpen(in.uv, c, sharpen_level); }
    }
    if (lens.count > 0u || lens.mist > 0.01) { c = drops_over(in.uv, c, over_ui(in.uv)); }
    return vec4<f32>(clamp(c, vec3<f32>(0.0), vec3<f32>(1.0)), 1.0);
}
