// SMAA 1x (rawdanger-recomp): Enhanced Subpixel Morphological Antialiasing on the console picture, before it is enlarged to the
// window. A port of SMAA.hlsl (Jimenez, Echevarria, Sousa, Gutierrez; MIT licence, see third_party/smaa/LICENSE.txt) to WebGPU
// compute shaders working on buffers: the textures of the original become buffers read with the same bilinear filtering by
// hand, so the "pseudo gather" tricks of the original (one filtered read giving two edges) work unchanged. Quality: the
// original's HIGH preset (threshold 0.1, 16 search steps, 8 diagonal steps, corner rounding 25), luma edge detection.
//
// Three passes over the picture (width x height pixels, `base` words into the scratch buffer):
//   edges   -> work[0 .. n)        (r: an edge to the left, g: an edge above; 0 or 255)
//   weights -> work[n .. 2n)       (rgba8: the blending weights)
//   blend   -> work[2n .. 3n)      (the antialiased picture, copied back over the scratch picture afterwards)

struct Params {
    width: u32,
    height: u32,
    base: u32,
    quality: u32,   // rawdanger-recomp: 1 low, 2 medium, 3 high (the reference preset), 4 ultra
}

@group(0) @binding(0) var<storage, read> picture: array<u32>;
@group(0) @binding(1) var<storage, read_write> work: array<u32>;
@group(0) @binding(2) var<storage, read> lut: array<u32>;   // AreaTex (160 x 560, rg8) then SearchTex (64 x 16, r8)
@group(0) @binding(3) var<uniform> params: Params;

// the presets of the SMAA reference (SMAA_PRESET_LOW .. ULTRA): the edge threshold and how far the searches go
fn threshold_q() -> f32 { return select(select(select(0.05, 0.1, params.quality == 3u), 0.1, params.quality == 2u), 0.15, params.quality == 1u); }
fn search_q() -> f32 { return select(select(select(32.0, 16.0, params.quality == 3u), 8.0, params.quality == 2u), 4.0, params.quality == 1u); }
fn search_diag_q() -> f32 { return select(select(16.0, 8.0, params.quality == 3u), 0.0, params.quality <= 2u); }
const CORNER_ROUNDING_NORM: f32 = 0.25;
const LOCAL_CONTRAST_FACTOR: f32 = 2.0;
const AREATEX_MAX_DISTANCE: f32 = 16.0;
const AREATEX_MAX_DISTANCE_DIAG: f32 = 20.0;
const AREATEX_SIZE: vec2<f32> = vec2<f32>(160.0, 560.0);
const AREATEX_SUBTEX_SIZE: f32 = 1.0 / 7.0;
const SEARCHTEX_SIZE: vec2<f32> = vec2<f32>(66.0, 33.0);
const SEARCHTEX_PACKED_SIZE: vec2<f32> = vec2<f32>(64.0, 16.0);
const AREA_BYTES: u32 = 160u * 560u * 2u;

fn size() -> vec2<f32> { return vec2<f32>(f32(params.width), f32(params.height)); }
fn rt() -> vec2<f32> { return 1.0 / size(); }
fn count() -> u32 { return params.width * params.height; }

// ---- the "textures" -----------------------------------------------------------------------------------------------------
fn lut_byte(i: u32) -> f32 { return f32((lut[i >> 2u] >> ((i & 3u) * 8u)) & 255u) / 255.0; }

fn area_texel(x: i32, y: i32) -> vec2<f32> {
    let cx = u32(clamp(x, 0, 159));
    let cy = u32(clamp(y, 0, 559));
    let i = (cy * 160u + cx) * 2u;
    return vec2<f32>(lut_byte(i), lut_byte(i + 1u));
}

fn search_texel(x: i32, y: i32) -> f32 {
    let cx = u32(clamp(x, 0, 63));
    let cy = u32(clamp(y, 0, 15));
    return lut_byte(AREA_BYTES + cy * 64u + cx);
}

fn sample_area(uv: vec2<f32>) -> vec2<f32> {
    let p = uv * AREATEX_SIZE - 0.5;
    let o = floor(p);
    let f = p - o;
    let x = i32(o.x);
    let y = i32(o.y);
    return mix(mix(area_texel(x, y), area_texel(x + 1, y), f.x), mix(area_texel(x, y + 1), area_texel(x + 1, y + 1), f.x), f.y);
}

// SearchTex is read with point filtering (as in the reference, where its sampler is a point sampler): its values are lengths,
// a blend of two neighbours is no length at all (the searches came back short and the edges got hardly any weight)
fn sample_search(uv: vec2<f32>) -> f32 {
    let p = floor(uv * SEARCHTEX_PACKED_SIZE);
    return search_texel(i32(p.x), i32(p.y));
}

fn edge_texel(x: i32, y: i32) -> vec2<f32> {
    let cx = u32(clamp(x, 0, i32(params.width) - 1));
    let cy = u32(clamp(y, 0, i32(params.height) - 1));
    let v = work[cy * params.width + cx];
    return vec2<f32>(f32(v & 255u), f32((v >> 8u) & 255u)) / 255.0;
}

// edgesTex read with bilinear filtering (SMAASampleLevelZero)
fn sample_edges(uv: vec2<f32>) -> vec2<f32> {
    let p = uv * size() - 0.5;
    let o = floor(p);
    let f = p - o;
    let x = i32(o.x);
    let y = i32(o.y);
    return mix(mix(edge_texel(x, y), edge_texel(x + 1, y), f.x), mix(edge_texel(x, y + 1), edge_texel(x + 1, y + 1), f.x), f.y);
}

fn sample_edges_offset(uv: vec2<f32>, o: vec2<f32>) -> vec2<f32> { return sample_edges(uv + o * rt()); }

fn color_texel(x: i32, y: i32) -> vec4<f32> {
    let cx = u32(clamp(x, 0, i32(params.width) - 1));
    let cy = u32(clamp(y, 0, i32(params.height) - 1));
    return unpack4x8unorm(picture[params.base + cy * params.width + cx]);
}

fn sample_color(uv: vec2<f32>) -> vec4<f32> {
    let p = uv * size() - 0.5;
    let o = floor(p);
    let f = p - o;
    let x = i32(o.x);
    let y = i32(o.y);
    return mix(mix(color_texel(x, y), color_texel(x + 1, y), f.x), mix(color_texel(x, y + 1), color_texel(x + 1, y + 1), f.x), f.y);
}

fn weight_texel(x: i32, y: i32) -> vec4<f32> {
    let cx = u32(clamp(x, 0, i32(params.width) - 1));
    let cy = u32(clamp(y, 0, i32(params.height) - 1));
    return unpack4x8unorm(work[count() + cy * params.width + cx]);
}

// ---- pass 1: luma edge detection --------------------------------------------------------------------------------------
// rawdanger-recomp (v0.10.4): the brightness compared on a square-root curve. Raw Danger's pictures are dark (night, rain, the
// colour filter's haze: most of a frame between 5 and 25 % brightness), and on the plain luma almost no edge there passes the
// reference's threshold: SMAA changed 0.4 % of the pixels of a street at night and the stairs on the hero's outline stayed. The
// curve spreads the dark tones (a step from 4 to 9 % reads as 10 points, from 64 to 72 % as 5): dark edges get found, bright
// areas are judged nearly as before.
fn luma(x: i32, y: i32) -> f32 { return sqrt(dot(color_texel(x, y).rgb, vec3<f32>(0.2126, 0.7152, 0.0722))); }

@compute @workgroup_size(8, 8)
fn edges(@builtin(global_invocation_id) id: vec3<u32>) {
    if (params.quality == 8u) { return; }   // FXAA: the blend pass only
    if (id.x >= params.width || id.y >= params.height) { return; }
    let x = i32(id.x);
    let y = i32(id.y);
    let l = luma(x, y);
    let l_left = luma(x - 1, y);
    let l_top = luma(x, y - 1);
    var delta = vec4<f32>(abs(l - vec2<f32>(l_left, l_top)), 0.0, 0.0);
    var e = step(vec2<f32>(threshold_q()), delta.xy);
    if (dot(e, vec2<f32>(1.0)) == 0.0) {
        work[id.y * params.width + id.x] = 0u;
        return;
    }
    let l_right = luma(x + 1, y);
    let l_bottom = luma(x, y + 1);
    delta = vec4<f32>(delta.xy, abs(l - vec2<f32>(l_right, l_bottom)));
    var max_delta = max(delta.xy, delta.zw);
    let l_leftleft = luma(x - 2, y);
    let l_toptop = luma(x, y - 2);
    delta = vec4<f32>(delta.xy, abs(vec2<f32>(l_left, l_top) - vec2<f32>(l_leftleft, l_toptop)));
    max_delta = max(max_delta, delta.zw);
    let final_delta = max(max_delta.x, max_delta.y);
    e = e * step(vec2<f32>(final_delta), LOCAL_CONTRAST_FACTOR * delta.xy);
    work[id.y * params.width + id.x] = u32(e.x * 255.0) | (u32(e.y * 255.0) << 8u);
}

// ---- pass 2: blending weights -----------------------------------------------------------------------------------------
fn decode_diag(e: vec2<f32>) -> vec2<f32> {
    var r = e;
    r.x = r.x * abs(5.0 * r.x - 5.0 * 0.75);
    return round(r);
}

fn decode_diag4(e: vec4<f32>) -> vec4<f32> {
    var r = e;
    r.x = r.x * abs(5.0 * r.x - 5.0 * 0.75);
    r.z = r.z * abs(5.0 * r.z - 5.0 * 0.75);
    return round(r);
}

struct DiagResult { d: vec2<f32>, e: vec2<f32> }

fn search_diag1(uv: vec2<f32>, dir: vec2<f32>) -> DiagResult {
    var coord = vec4<f32>(uv, -1.0, 1.0);
    let t = vec3<f32>(rt(), 1.0);
    var e = vec2<f32>(0.0);
    loop {
        if (!(coord.z < search_diag_q() - 1.0 && coord.w > 0.9)) { break; }
        let xyz = t * vec3<f32>(dir, 1.0) + coord.xyz;
        coord = vec4<f32>(xyz, coord.w);
        e = sample_edges(coord.xy);
        coord.w = dot(e, vec2<f32>(0.5));
    }
    return DiagResult(coord.zw, e);
}

fn search_diag2(uv: vec2<f32>, dir: vec2<f32>) -> DiagResult {
    var coord = vec4<f32>(uv, -1.0, 1.0);
    coord.x = coord.x + 0.25 * rt().x;
    let t = vec3<f32>(rt(), 1.0);
    var e = vec2<f32>(0.0);
    loop {
        if (!(coord.z < search_diag_q() - 1.0 && coord.w > 0.9)) { break; }
        let xyz = t * vec3<f32>(dir, 1.0) + coord.xyz;
        coord = vec4<f32>(xyz, coord.w);
        e = decode_diag(sample_edges(coord.xy));
        coord.w = dot(e, vec2<f32>(0.5));
    }
    return DiagResult(coord.zw, e);
}

fn area_diag(dist: vec2<f32>, e: vec2<f32>, offset: f32) -> vec2<f32> {
    var uv = vec2<f32>(AREATEX_MAX_DISTANCE_DIAG) * e + dist;
    uv = (1.0 / AREATEX_SIZE) * uv + 0.5 * (1.0 / AREATEX_SIZE);
    uv.x = uv.x + 0.5;
    uv.y = uv.y + AREATEX_SUBTEX_SIZE * offset;
    return sample_area(uv);
}

fn diag_weights(uv: vec2<f32>, e: vec2<f32>) -> vec2<f32> {
    var weights = vec2<f32>(0.0);
    var d = vec4<f32>(0.0);
    var end = vec2<f32>(0.0);
    if (e.x > 0.0) {
        let r = search_diag1(uv, vec2<f32>(-1.0, 1.0));
        d.x = r.d.x;
        d.z = r.d.y;
        end = r.e;
        d.x = d.x + select(0.0, 1.0, end.y > 0.9);
    }
    let r2 = search_diag1(uv, vec2<f32>(1.0, -1.0));
    d.y = r2.d.x;
    d.w = r2.d.y;
    if (d.x + d.y > 2.0) {
        let coords = vec4<f32>(-d.x + 0.25, d.x, d.y, -d.y - 0.25) * vec4<f32>(rt(), rt()) + uv.xyxy;
        var c = vec4<f32>(sample_edges_offset(coords.xy, vec2<f32>(-1.0, 0.0)), sample_edges_offset(coords.zw, vec2<f32>(1.0, 0.0)));
        let dec = decode_diag4(c);
        c = vec4<f32>(dec.y, dec.x, dec.w, dec.z);   // c.yxwz = decode(c.xyzw)
        var cc = vec2<f32>(2.0) * c.xz + c.yw;
        if (step(0.9, d.z) > 0.0) { cc.x = 0.0; }
        if (step(0.9, d.w) > 0.0) { cc.y = 0.0; }
        weights = weights + area_diag(d.xy, cc, 0.0);
    }
    let r3 = search_diag2(uv, vec2<f32>(-1.0, -1.0));
    d.x = r3.d.x;
    d.z = r3.d.y;
    if (sample_edges_offset(uv, vec2<f32>(1.0, 0.0)).x > 0.0) {
        let r4 = search_diag2(uv, vec2<f32>(1.0, 1.0));
        d.y = r4.d.x;
        d.w = r4.d.y;
        d.y = d.y + select(0.0, 1.0, r4.e.y > 0.9);
    } else {
        d.y = 0.0;
        d.w = 0.0;
    }
    if (d.x + d.y > 2.0) {
        let coords = vec4<f32>(-d.x, -d.x, d.y, d.y) * vec4<f32>(rt(), rt()) + uv.xyxy;
        var c = vec4<f32>(0.0);
        c.x = sample_edges_offset(coords.xy, vec2<f32>(-1.0, 0.0)).y;
        c.y = sample_edges_offset(coords.xy, vec2<f32>(0.0, -1.0)).x;
        let zw = sample_edges_offset(coords.zw, vec2<f32>(1.0, 0.0));
        c.z = zw.y;
        c.w = zw.x;
        var cc = vec2<f32>(2.0) * c.xz + c.yw;
        if (step(0.9, d.z) > 0.0) { cc.x = 0.0; }
        if (step(0.9, d.w) > 0.0) { cc.y = 0.0; }
        let a = area_diag(d.xy, cc, 0.0);
        weights = weights + vec2<f32>(a.y, a.x);
    }
    return weights;
}

fn search_length(e: vec2<f32>, offset: f32) -> f32 {
    var scale = SEARCHTEX_SIZE * vec2<f32>(0.5, -1.0);
    var bias = SEARCHTEX_SIZE * vec2<f32>(offset, 1.0);
    scale = scale + vec2<f32>(-1.0, 1.0);
    bias = bias + vec2<f32>(0.5, -0.5);
    scale = scale / SEARCHTEX_PACKED_SIZE;
    bias = bias / SEARCHTEX_PACKED_SIZE;
    return sample_search(scale * e + bias);
}

fn search_x_left(start: vec2<f32>, end: f32) -> f32 {
    var uv = start;
    var e = vec2<f32>(0.0, 1.0);
    loop {
        if (!(uv.x > end && e.y > 0.8281 && e.x == 0.0)) { break; }
        e = sample_edges(uv);
        uv = -vec2<f32>(2.0, 0.0) * rt() + uv;
    }
    let offset = -(255.0 / 127.0) * search_length(e, 0.0) + 3.25;
    return rt().x * offset + uv.x;
}

fn search_x_right(start: vec2<f32>, end: f32) -> f32 {
    var uv = start;
    var e = vec2<f32>(0.0, 1.0);
    loop {
        if (!(uv.x < end && e.y > 0.8281 && e.x == 0.0)) { break; }
        e = sample_edges(uv);
        uv = vec2<f32>(2.0, 0.0) * rt() + uv;
    }
    let offset = -(255.0 / 127.0) * search_length(e, 0.5) + 3.25;
    return -rt().x * offset + uv.x;
}

fn search_y_up(start: vec2<f32>, end: f32) -> f32 {
    var uv = start;
    var e = vec2<f32>(1.0, 0.0);
    loop {
        if (!(uv.y > end && e.x > 0.8281 && e.y == 0.0)) { break; }
        e = sample_edges(uv);
        uv = -vec2<f32>(0.0, 2.0) * rt() + uv;
    }
    let offset = -(255.0 / 127.0) * search_length(e.yx, 0.0) + 3.25;
    return rt().y * offset + uv.y;
}

fn search_y_down(start: vec2<f32>, end: f32) -> f32 {
    var uv = start;
    var e = vec2<f32>(1.0, 0.0);
    loop {
        if (!(uv.y < end && e.x > 0.8281 && e.y == 0.0)) { break; }
        e = sample_edges(uv);
        uv = vec2<f32>(0.0, 2.0) * rt() + uv;
    }
    let offset = -(255.0 / 127.0) * search_length(e.yx, 0.5) + 3.25;
    return -rt().y * offset + uv.y;
}

fn area(dist: vec2<f32>, e1: f32, e2: f32, offset: f32) -> vec2<f32> {
    var uv = vec2<f32>(AREATEX_MAX_DISTANCE) * round(4.0 * vec2<f32>(e1, e2)) + dist;
    uv = (1.0 / AREATEX_SIZE) * uv + 0.5 * (1.0 / AREATEX_SIZE);
    uv.y = AREATEX_SUBTEX_SIZE * offset + uv.y;
    return sample_area(uv);
}

fn corner_horizontal(weights: vec2<f32>, uv: vec4<f32>, d: vec2<f32>) -> vec2<f32> {
    let left_right = step(d.xy, d.yx);
    var rounding = (1.0 - CORNER_ROUNDING_NORM) * left_right;
    rounding = rounding / (left_right.x + left_right.y);
    var factor = vec2<f32>(1.0);
    factor.x = factor.x - rounding.x * sample_edges_offset(uv.xy, vec2<f32>(0.0, 1.0)).x;
    factor.x = factor.x - rounding.y * sample_edges_offset(uv.zw, vec2<f32>(1.0, 1.0)).x;
    factor.y = factor.y - rounding.x * sample_edges_offset(uv.xy, vec2<f32>(0.0, -2.0)).x;
    factor.y = factor.y - rounding.y * sample_edges_offset(uv.zw, vec2<f32>(1.0, -2.0)).x;
    return weights * clamp(factor, vec2<f32>(0.0), vec2<f32>(1.0));
}

fn corner_vertical(weights: vec2<f32>, uv: vec4<f32>, d: vec2<f32>) -> vec2<f32> {
    let left_right = step(d.xy, d.yx);
    var rounding = (1.0 - CORNER_ROUNDING_NORM) * left_right;
    rounding = rounding / (left_right.x + left_right.y);
    var factor = vec2<f32>(1.0);
    factor.x = factor.x - rounding.x * sample_edges_offset(uv.xy, vec2<f32>(1.0, 0.0)).y;
    factor.x = factor.x - rounding.y * sample_edges_offset(uv.zw, vec2<f32>(1.0, 1.0)).y;
    factor.y = factor.y - rounding.x * sample_edges_offset(uv.xy, vec2<f32>(-2.0, 0.0)).y;
    factor.y = factor.y - rounding.y * sample_edges_offset(uv.zw, vec2<f32>(-2.0, 1.0)).y;
    return weights * clamp(factor, vec2<f32>(0.0), vec2<f32>(1.0));
}

@compute @workgroup_size(8, 8)
fn weights(@builtin(global_invocation_id) id: vec3<u32>) {
    if (params.quality == 8u) { return; }
    if (id.x >= params.width || id.y >= params.height) { return; }
    let r = rt();
    let uv = (vec2<f32>(id.xy) + 0.5) * r;
    let pix = uv * size();
    let off0 = r.xyxy * vec4<f32>(-0.25, -0.125, 1.25, -0.125) + uv.xyxy;
    let off1 = r.xyxy * vec4<f32>(-0.125, -0.25, -0.125, 1.25) + uv.xyxy;
    let off2 = r.xxyy * vec4<f32>(-2.0, 2.0, -2.0, 2.0) * search_q() + vec4<f32>(off0.xz, off1.yw);
    var w = vec4<f32>(0.0);
    var e = edge_texel(i32(id.x), i32(id.y));
    if (e.y > 0.0) {  // an edge above
        let dw = diag_weights(uv, e);
        w = vec4<f32>(dw, w.zw);
        if (w.x == -w.y) {
            var coords = vec3<f32>(search_x_left(off0.xy, off2.x), off1.y, 0.0);
            var d = vec2<f32>(coords.x, 0.0);
            let e1 = sample_edges(coords.xy).x;
            coords.z = search_x_right(off0.zw, off2.y);
            d.y = coords.z;
            d = abs(round(size().xx * d - pix.xx));
            let sqrt_d = sqrt(d);
            let e2 = sample_edges_offset(coords.zy, vec2<f32>(1.0, 0.0)).x;
            var a = area(sqrt_d, e1, e2, 0.0);
            coords.y = uv.y;
            a = corner_horizontal(a, coords.xyzy, d);
            w = vec4<f32>(a, w.zw);
        } else {
            e.x = 0.0;  // a diagonal: no vertical work
        }
    }
    if (e.x > 0.0) {  // an edge to the left
        var coords = vec3<f32>(off0.x, search_y_up(off1.xy, off2.z), 0.0);
        var d = vec2<f32>(coords.y, 0.0);
        let e1 = sample_edges(coords.xy).y;
        coords.z = search_y_down(off1.zw, off2.w);
        d.y = coords.z;
        d = abs(round(size().yy * d - pix.yy));
        let sqrt_d = sqrt(d);
        let e2 = sample_edges_offset(coords.xz, vec2<f32>(0.0, 1.0)).y;
        var a = area(sqrt_d, e1, e2, 0.0);
        coords.x = uv.x;
        a = corner_vertical(a, coords.xyxz, d);
        w = vec4<f32>(w.xy, a);
    }
    work[count() + id.y * params.width + id.x] = pack4x8unorm(clamp(w, vec4<f32>(0.0), vec4<f32>(1.0)));
}

// ---- rawdanger-recomp: FXAA (quality 8): Timothy Lottes' FXAA 3.11 edge walk, 12 steps, on the picture at its own size -----
fn fx_luma(c: vec3<f32>) -> f32 { return sqrt(dot(c, vec3<f32>(0.299, 0.587, 0.114))); }
fn fx_l(uv: vec2<f32>) -> f32 { return fx_luma(sample_color(uv).rgb); }
fn fxaa(x: i32, y: i32, uv: vec2<f32>, r: vec2<f32>) -> vec4<f32> {
    let c = color_texel(x, y);
    let lm = fx_luma(c.rgb);
    let ln = fx_luma(color_texel(x, y - 1).rgb);
    let ls = fx_luma(color_texel(x, y + 1).rgb);
    let lw = fx_luma(color_texel(x - 1, y).rgb);
    let le = fx_luma(color_texel(x + 1, y).rgb);
    let lo = min(lm, min(min(ln, ls), min(lw, le)));
    let hi = max(lm, max(max(ln, ls), max(lw, le)));
    let range = hi - lo;
    if (range < max(0.0312, hi * 0.125)) { return c; }
    let lnw = fx_luma(color_texel(x - 1, y - 1).rgb);
    let lne = fx_luma(color_texel(x + 1, y - 1).rgb);
    let lsw = fx_luma(color_texel(x - 1, y + 1).rgb);
    let lse = fx_luma(color_texel(x + 1, y + 1).rgb);
    let edge_h = abs(-2.0 * lw + lnw + lsw) + abs(-2.0 * lm + ln + ls) * 2.0 + abs(-2.0 * le + lne + lse);
    let edge_v = abs(-2.0 * ln + lnw + lne) + abs(-2.0 * lm + lw + le) * 2.0 + abs(-2.0 * ls + lsw + lse);
    let horizontal = edge_h >= edge_v;
    let l1 = select(lw, ln, horizontal);
    let l2 = select(le, ls, horizontal);
    let g1 = abs(l1 - lm);
    let g2 = abs(l2 - lm);
    let steep = g1 >= g2;
    var step_len = select(r.x, r.y, horizontal);
    var l_avg = 0.5 * (l2 + lm);
    var grad = 0.25 * max(g1, g2);
    if (steep) { step_len = -step_len; l_avg = 0.5 * (l1 + lm); }
    var cur = uv;
    if (horizontal) { cur.y = cur.y + step_len * 0.5; } else { cur.x = cur.x + step_len * 0.5; }
    let off = select(vec2<f32>(0.0, r.y), vec2<f32>(r.x, 0.0), horizontal);
    var u1 = cur - off;
    var u2 = cur + off;
    var e1 = fx_l(u1) - l_avg;
    var e2 = fx_l(u2) - l_avg;
    var d1 = abs(e1) >= grad;
    var d2 = abs(e2) >= grad;
    let steps = array<f32, 12>(1.0, 1.0, 1.0, 1.0, 1.0, 1.5, 2.0, 2.0, 2.0, 2.0, 4.0, 8.0);
    for (var i = 1; i < 12; i = i + 1) {
        if (d1 && d2) { break; }
        if (!d1) { u1 = u1 - off * steps[i]; e1 = fx_l(u1) - l_avg; d1 = abs(e1) >= grad; }
        if (!d2) { u2 = u2 + off * steps[i]; e2 = fx_l(u2) - l_avg; d2 = abs(e2) >= grad; }
    }
    let dist1 = select(uv.y - u1.y, uv.x - u1.x, horizontal);
    let dist2 = select(u2.y - uv.y, u2.x - uv.x, horizontal);
    let closer1 = dist1 < dist2;
    let dist = min(dist1, dist2);
    let thickness = dist1 + dist2;
    let pix_off = -dist / max(thickness, 1e-6) + 0.5;
    let lm_smaller = lm < l_avg;
    let correct = select((e2 < 0.0) != lm_smaller, (e1 < 0.0) != lm_smaller, closer1);
    var final_off = select(0.0, pix_off, correct);
    let la = (2.0 * (ln + ls + lw + le) + lnw + lne + lsw + lse) / 12.0;
    let sub = clamp(abs(la - lm) / range, 0.0, 1.0);
    let sub2 = (-2.0 * sub + 3.0) * sub * sub;
    final_off = max(final_off, sub2 * sub2 * 0.75);
    var fuv = uv;
    if (horizontal) { fuv.y = fuv.y + final_off * step_len; } else { fuv.x = fuv.x + final_off * step_len; }
    return vec4<f32>(sample_color(fuv).rgb, c.a);
}

// ---- pass 3: neighbourhood blending -----------------------------------------------------------------------------------
@compute @workgroup_size(8, 8)
fn blend(@builtin(global_invocation_id) id: vec3<u32>) {
    if (id.x >= params.width || id.y >= params.height) { return; }
    let x = i32(id.x);
    let y = i32(id.y);
    let r = rt();
    let uv = (vec2<f32>(id.xy) + 0.5) * r;
    if (params.quality == 8u) {
        work[2u * count() + id.y * params.width + id.x] = pack4x8unorm(fxaa(x, y, uv, r));
        return;
    }
    var a: vec4<f32>;
    a.x = weight_texel(x + 1, y).w;   // right
    a.y = weight_texel(x, y + 1).y;   // bottom (the original's "top", in its flipped texture coordinates)
    let here = weight_texel(x, y);
    a.w = here.x;
    a.z = here.z;
    var out: vec4<f32>;
    if (dot(a, vec4<f32>(1.0)) < 1e-5) {
        out = color_texel(x, y);
    } else {
        let h = max(a.x, a.z) > max(a.y, a.w);
        var blending_offset = vec4<f32>(0.0, a.y, 0.0, a.w);
        var blending_weight = a.yw;
        if (h) {
            blending_offset = vec4<f32>(a.x, 0.0, a.z, 0.0);
            blending_weight = a.xz;
        }
        blending_weight = blending_weight / dot(blending_weight, vec2<f32>(1.0));
        let coord = blending_offset * vec4<f32>(r, -r) + uv.xyxy;
        out = blending_weight.x * sample_color(coord.xy) + blending_weight.y * sample_color(coord.zw);
        out.w = 1.0;
    }
    work[2u * count() + id.y * params.width + id.x] = pack4x8unorm(clamp(out, vec4<f32>(0.0), vec4<f32>(1.0)));
}
