// SPDX-License-Identifier: GPL-3.0-or-later
#include "gel3dview.h"

#include <epoxy/gl.h>

#include <algorithm>
#include <cmath>

namespace oc::gtk {

namespace {

struct SceneLook {
    float plate[4], stain[4], band[4], lip[4], sigma[4];
};

// plate: the plate's colour (rgb), monochrome (w). stain: the free stain's
// glow (rgb) and strength (w). band: the DNA's glow and how much brighter
// than the free stain. lip: the cut edges' shine. sigma: what the stained
// gel drinks per metre (rgb) and how much it scatters (w).
const SceneLook &lookOf(Gel3DScene scene)
{
    static const SceneLook looks[] = {
        {{0.02f, 0.0f, 0.75f, 0}, {1.0f, 0.10f, 0.50f, 0.7f}, {1.0f, 0.22f, 0.60f, 13}, {0.55f, 0.12f, 0.9f, 0.5f},
         {8, 40, 6, 18}},
        {{0.0f, 0.10f, 0.70f, 0}, {0.30f, 0.95f, 0.40f, 0.38f}, {0.50f, 1.0f, 0.35f, 14}, {0.35f, 0.9f, 0.55f, 0.45f},
         {10, 8, 34, 18}},
        {{0.06f, 0.06f, 0.07f, 1}, {0.6f, 0.6f, 0.6f, 0.7f}, {1.0f, 1.0f, 1.0f, 16}, {0.8f, 0.8f, 0.8f, 0.5f},
         {12, 12, 12, 18}},
    };
    return looks[std::clamp(int(scene), 0, 2)];
}

// Enough for a clean picture; then the GPU rests until something changes.
constexpr uint32_t kMaxSamples = 160;
// Traced pixels at most: the picture is soft and grainy anyway, and a
// laptop's GPU has to keep up. Half that (a quarter the pixels) while turning.
constexpr double kPixelCap = 1'000'000;

// Lanes are never loaded quite evenly: a fixed, slight difference per lane.
float loading(size_t lane)
{
    static const float wobble[] = {1, 0.92f, 1.04f, 0.96f, 1.06f, 0.94f, 1.01f, 0.95f, 1.05f, 0.98f, 1.02f};
    return wobble[lane % 11];
}

// A small, repeatable random sequence, so the gel looks the same each time.
struct Noise {
    uint32_t seed;
    float next()
    {
        seed = seed * 1664525u + 1013904223u;
        return float(seed >> 8) / float(1 << 24);
    }
    // Smooth noise over a plane, 0 to 1, repeatable.
    static float value(float x, float y)
    {
        auto hash = [](int32_t i, int32_t j) {
            uint32_t h = uint32_t(i) * 1973u + uint32_t(j) * 9277u + 7u;
            h ^= h >> 16;
            h *= 0x7feb352du;
            h ^= h >> 15;
            h *= 0x846ca68bu;
            h ^= h >> 16;
            return float(h >> 8) / float(1 << 24);
        };
        const float fx = std::floor(x), fy = std::floor(y);
        const int32_t i = int32_t(fx), j = int32_t(fy);
        float u = x - fx, v = y - fy;
        u = u * u * (3 - 2 * u);
        v = v * v * (3 - 2 * v);
        const float top = hash(i, j) + (hash(i + 1, j) - hash(i, j)) * u;
        const float bottom = hash(i, j + 1) + (hash(i + 1, j + 1) - hash(i, j + 1)) * u;
        return top + (bottom - top) * v;
    }
};

// A triangle over the whole viewport.
const char *const kVertex = R"(#version 330 core
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)";

// What both passes share: the uniforms and the noise.
const char *const kCommon = R"(#version 330 core
struct Uniforms {
    vec4 camPos, camF, camR, camU, sigma, lens, slab, emitter, look, plate, stain, band, lip;
    // contrast, the spotlight's brightness, size and cone; bloom, vignette,
    // the free stain's and the DNA's glow as multiples.
    vec4 tune0, tune1;
    uvec4 counts;
};
uniform Uniforms u;
out vec4 fragColor;

uint hashu(uint x) {
    x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16;
    return x;
}
float rnd(inout uint s) {
    s = hashu(s + 0x9e3779b9u);
    return float(s >> 8) * (1.0 / 16777216.0);
}
float hashf(vec2 p) {
    uvec2 q = uvec2(ivec2(floor(p)) + ivec2(1 << 20));
    return float(hashu(q.x * 1973u + q.y * 9277u + 7u) >> 8) * (1.0 / 16777216.0);
}
float vnoise(vec2 p) {
    vec2 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = hashf(i), b = hashf(i + vec2(1, 0)), c = hashf(i + vec2(0, 1)), d = hashf(i + vec2(1, 1));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}
)";

// One sample per pixel added to the ones so far (as the Mac's gelTrace).
const char *const kTrace = R"(
uniform sampler2D prev;
uniform sampler2D bands;

const float IOR = 1.34;
const float EPS = 0.00004;

float fresnel(float cosi, float cost, float eta) {
    float rs = (eta * cosi - cost) / (eta * cosi + cost);
    float rp = (cosi - eta * cost) / (cosi + eta * cost);
    return 0.5 * (rs * rs + rp * rp);
}

// A new direction after scattering in the gel (Henyey-Greenstein).
vec3 sampleHG(vec3 d, float g, inout uint seed) {
    float u1 = rnd(seed), u2 = rnd(seed);
    float cosT;
    if (abs(g) < 0.001) {
        cosT = 1.0 - 2.0 * u1;
    } else {
        float sq = (1.0 - g * g) / (1.0 - g + 2.0 * g * u1);
        cosT = (1.0 + g * g - sq * sq) / (2.0 * g);
    }
    cosT = clamp(cosT, -1.0, 1.0);
    float sinT = sqrt(1.0 - cosT * cosT), phi = 6.2831853 * u2;
    vec3 a = abs(d.x) > 0.9 ? vec3(0, 1, 0) : vec3(1, 0, 0);
    vec3 t = normalize(cross(a, d)), b = cross(d, t);
    return normalize(t * (sinT * cos(phi)) + b * (sinT * sin(phi)) + d * cosT);
}

// Dust on the plate: a round speck in a few of the cells.
float dust(vec2 p, float share) {
    vec2 cell = floor(p);
    if (hashf(cell) > share) return 1.0;
    vec2 c = cell + vec2(hashf(cell + 13.0), hashf(cell + 29.0));
    float r = 0.2 + 0.25 * hashf(cell + 41.0);
    return mix(0.45, 1.0, smoothstep(r * 0.3, r, length(p - c)));
}

// One spotlight, high up behind the gel, aimed at it: what it throws on a
// matte surface at x facing n (a cone with a soft edge, falling off with
// distance), and how it looks head on.
const vec3 LAMP = vec3(0.03, 0.55, -0.30);
const vec3 LAMP_COLOUR = vec3(1.0, 0.94, 0.86);
vec3 spot(vec3 x, vec3 n) {
    vec3 axis = normalize(vec3(0.0, 0.0, 0.0) - LAMP);
    vec3 to = x - LAMP;
    float dist2 = dot(to, to);
    vec3 dir = to * inversesqrt(dist2);
    float cone = smoothstep(cos(u.tune0.w), cos(u.tune0.w * 0.55), dot(dir, axis));
    float lambert = max(0.0, -dot(dir, n));
    return LAMP_COLOUR * (u.tune0.y * 0.035 * cone * lambert / max(dist2, 0.05));
}

// The plate: a diffuser lit from below, brightest under the gel, mottled,
// dusty, dark beyond its edges.
vec3 plate(vec2 q) {
    float halfX = u.emitter.x, front = u.emitter.y, back = u.emitter.z;
    float zmin = -u.slab.y * 0.5 - back;
    float edge = smoothstep(0.0, 0.012, halfX - abs(q.x))
               * smoothstep(0.0, 0.012, q.y - zmin) * smoothstep(0.0, 0.012, front - q.y);
    vec3 dark = u.plate.xyz * 0.016 + 0.0004;
    if (edge <= 0.0) {
        // The box's black frame around the glass, with a faint bevel lit by the glass.
        float past = max(max(abs(q.x) - halfX, q.y - front), zmin - q.y);
        float bevel = smoothstep(0.004, 0.0, past);
        float frame = smoothstep(0.03, 0.018, past);
        // Beyond the frame, the bench; both matte, lit by the spotlight.
        vec3 lit = spot(vec3(q.x, 0.0, q.y), vec3(0.0, 1.0, 0.0));
        vec3 bench = (1.0 - frame) * lit * vec3(0.30, 0.27, 0.24) * (0.9 + 0.2 * vnoise(q * 200.0));
        return dark + bench + frame * ((vec3(0.012) + lit * 0.06) * (0.8 + 0.4 * vnoise(q * 300.0))
                                       + u.plate.xyz * u.emitter.w * 0.05 * bevel);
    }
    vec2 rel = (q - vec2(0.0, -0.3 * front)) / vec2(halfX, front);
    float fall = mix(1.0, 0.06, smoothstep(0.12, 1.1, length(rel)));
    float mott = 0.80 + 0.40 * vnoise(q * 25.0 + 3.0);
    float blot = 1.0 - 0.12 * smoothstep(0.55, 0.8, vnoise(q * 60.0 + 9.0));
    float speck = dust(q * 500.0, 0.06) * dust(q * 1500.0 + 5.0, 0.03);
    float grain = 0.95 + 0.10 * vnoise(q * 900.0);
    vec3 lit = u.plate.xyz * u.emitter.w;
    return mix(dark, lit * fall * mott * blot * speck * grain, edge);
}

// The room: dark, with the spotlight itself where a ray meets it, a wall
// behind the gel lit by it, and the bench the light box stands on.
vec3 room(vec3 p, vec3 d) {
    vec3 dark = vec3(0.002, 0.001, 0.006);
    if (d.y > 0.0001) {
        float t = (LAMP.y - p.y) / d.y;
        vec3 h = p + d * t;
        // A round lamp, soft towards its rim, so its reflection in the wet
        // top is one soft blob however the surface undulates.
        float r = length(h.xz - LAMP.xz) / u.tune0.z;
        if (r < 1.0) return dark + LAMP_COLOUR * u.tune0.y * smoothstep(1.0, 0.25, r);
    }
    if (d.z < -0.0001) {
        // The wall behind, 60 cm back: a matte grey the spotlight washes.
        float t = (-0.6 - p.z) / d.z;
        vec3 w = p + d * t;
        if (w.y > -0.2) return dark + spot(w, vec3(0.0, 0.0, 1.0)) * vec3(0.32, 0.30, 0.28);
    }
    return dark;
}

// The wet top isn't quite smooth: a fine ripple tilts its reflections.
vec3 ripple(vec3 x, vec3 n) {
    if (n.y < 0.5) return n;
    vec2 q = x.xz * 160.0;
    float dx = vnoise(q + vec2(0.7, 0.0)) - vnoise(q - vec2(0.7, 0.0));
    float dz = vnoise(q + vec2(0.0, 0.7)) - vnoise(q - vec2(0.0, 0.7));
    return normalize(n + vec3(dx, 0.0, dz) * 0.02);
}

// The slab's shape: its top sags a little and climbs the tray's walls in a
// lip, its outline wanders, its edges are softened, the wells are cut in.
float slabTop(vec2 xz) {
    float hw = u.slab.x * 0.5, hd = u.slab.y * 0.5, thick = u.slab.z;
    vec2 uv = vec2(xz.x / u.slab.x + 0.5, xz.y / u.slab.y + 0.5);
    float well = texture(bands, uv).b;
    float inset = min(hw - abs(xz.x), hd - abs(xz.y));
    float lip = exp(-max(inset, 0.0) / 0.0015) * 0.00055;
    float sag = (vnoise(xz * 120.0) - 0.5) * 0.00007 + (vnoise(xz * 23.0 + 7.0) - 0.5) * 0.00015;
    float cut = smoothstep(0.55, 0.85, well) * u.slab.w * thick;
    return thick + lip + sag - cut;
}
// Signed distance to the slab: negative inside.
float slabSDF(vec3 p) {
    float hw = u.slab.x * 0.5, hd = u.slab.y * 0.5;
    float r = 0.0007;
    vec2 e = vec2(hw + (vnoise(vec2(p.z * 90.0, 1.5)) - 0.5) * 0.0006,
                  hd + (vnoise(vec2(p.x * 90.0, 8.5)) - 0.5) * 0.0006);
    vec2 q = abs(p.xz) - (e - r);
    float dxz = length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
    float dy = max(p.y - slabTop(p.xz), -p.y);
    float k = 0.0004;
    float h = clamp(0.5 + 0.5 * (dxz - dy) / k, 0.0, 1.0);
    return mix(dy, dxz, h) + k * h * (1.0 - h);
}
vec3 slabNormal(vec3 p) {
    float e = 0.00003;
    vec3 n = vec3(slabSDF(p + vec3(e, 0, 0)) - slabSDF(p - vec3(e, 0, 0)),
                  slabSDF(p + vec3(0, e, 0)) - slabSDF(p - vec3(0, e, 0)),
                  slabSDF(p + vec3(0, 0, e)) - slabSDF(p - vec3(0, 0, e)));
    return normalize(n);
}
// The box around the slab with room for its lip and wandering outline.
bool slabBox(vec3 o, vec3 d, out float t0, out float t1) {
    float pad = 0.0015;
    vec3 bmin = vec3(-u.slab.x * 0.5 - pad, -0.0001, -u.slab.y * 0.5 - pad);
    vec3 bmax = vec3(u.slab.x * 0.5 + pad, u.slab.z + pad, u.slab.y * 0.5 + pad);
    vec3 inv = 1.0 / d;
    vec3 ta = (bmin - o) * inv, tb = (bmax - o) * inv;
    vec3 tmin = min(ta, tb), tmax = max(ta, tb);
    t0 = max(max(max(tmin.x, tmin.y), tmin.z), 0.0);
    t1 = min(min(tmax.x, tmax.y), tmax.z);
    return t1 > t0;
}
// Where a ray from outside meets the slab, walking the distance field.
bool enterSlab(vec3 o, vec3 d, out float tHit) {
    float t0, t1;
    tHit = 0.0;
    if (!slabBox(o, d, t0, t1)) return false;
    float t = max(t0, 0.0002), prv = t;
    float dp = slabSDF(o + d * t);
    for (int i = 0; i < 80 && t < t1; i++) {
        if (dp < 0.00002) {
            if (dp < -0.00002) {
                // Stepped through a well's wall or the lip: find the face.
                float a = prv, b = t;
                for (int j = 0; j < 5; j++) {
                    float m = 0.5 * (a + b);
                    if (slabSDF(o + d * m) > 0.0) a = m; else b = m;
                }
                t = b;
            }
            tHit = t;
            return true;
        }
        prv = t;
        t += max(dp * 0.9, 0.00004);
        dp = slabSDF(o + d * t);
    }
    return false;
}

// What a ray sees once it's clear of the slab: the plate, wet around the
// gel where buffer spilt (a film that mirrors the room), or the room.
vec3 outside(vec3 p, vec3 d) {
    if (d.y >= -0.0001) return room(p, d);
    float t = -p.y / d.y;
    vec3 h = p + d * t;
    vec3 c = plate(h.xz);
    vec2 q = abs(h.xz) - vec2(u.slab.x * 0.5, u.slab.y * 0.5);
    float away = length(max(q, 0.0));
    float wet = smoothstep(0.012, 0.0, away + (vnoise(h.xz * 180.0) - 0.5) * 0.008)
              + smoothstep(0.64, 0.76, vnoise(h.xz * 45.0 + 3.0)) * 0.7;
    wet = clamp(wet, 0.0, 1.0);
    if (wet > 0.0) {
        vec2 w = h.xz * 320.0;
        vec3 n = normalize(vec3((vnoise(w) - 0.5) * 0.10, 1.0, (vnoise(w + 5.0) - 0.5) * 0.10));
        float cosi = max(0.01, -dot(d, n));
        float eta = 1.0 / 1.33;
        float cost = sqrt(max(0.0, 1.0 - eta * eta * (1.0 - cosi * cosi)));
        float F = fresnel(cosi, cost, eta);
        c = mix(c, c * (1.0 - F) + room(h, reflect(d, n)) * F, wet);
    }
    return c;
}

// The gel at a point: what it gives off and what it drinks.
void medium(vec3 p, out vec3 emit, out vec3 absorb) {
    vec2 uv = vec2(p.x / u.slab.x + 0.5, p.z / u.slab.y + 0.5);
    vec4 m = texture(bands, uv);
    float yf = p.y / u.slab.z;
    float prof = smoothstep(0.0, 0.15, yf) * smoothstep(1.0, 0.85, yf);
    float wall = (m.b > 0.2 && m.b <= 0.8 && yf > 1.0 - u.slab.w) ? 1.0 : 0.0;
    float uneven = 0.72 + 0.36 * vnoise(uv * vec2(9.0, 5.0) + 1.0) + 0.12 * vnoise(uv * vec2(60.0, 33.0));
    vec3 stain = u.stain.xyz * u.stain.w * u.tune1.z, band = u.band.xyz * u.band.w * u.tune1.w;
    emit = (stain * uneven * (1.0 + 6.0 * wall) + band * m.r * prof) * u.look.z;
    absorb = u.sigma.xyz + vec3(0.6, 0.9, 0.15) * 260.0 * m.g;
}

void main() {
    uint W = u.counts.z, H = u.counts.w;
    uvec2 gid = uvec2(gl_FragCoord.xy);
    uint seed = hashu(gid.x * 1973u + gid.y * 9277u + u.counts.x * 26699u + 1u);

    // The camera ray: through a random point in the pixel and a thin lens.
    vec2 px = (vec2(gid) + vec2(rnd(seed), rnd(seed))) / vec2(W, H);
    vec2 ndc = px * 2.0 - 1.0;
    float tanH = u.lens.x, aspect = u.lens.y;
    vec3 dir = normalize(u.camF.xyz + u.camR.xyz * (ndc.x * tanH * aspect) + u.camU.xyz * (ndc.y * tanH));
    vec3 o = u.camPos.xyz;
    {
        float r = sqrt(rnd(seed)) * u.lens.z, a = rnd(seed) * 6.2831853;
        vec3 focus = o + dir * (u.lens.w / dot(dir, u.camF.xyz));
        o += u.camR.xyz * (r * cos(a)) + u.camU.xyz * (r * sin(a));
        dir = normalize(focus - o);
    }

    float hw = u.slab.x * 0.5, hd = u.slab.y * 0.5;
    vec3 L = vec3(0.0), T = vec3(1.0);
    vec3 p = o, d = dir;
    bool inside = false;
    int scatters = 0;
    for (int bounce = 0; bounce < 12; bounce++) {
        if (!inside) {
            float tHit;
            if (!enterSlab(p, d, tHit)) { L += T * outside(p, d); break; }
            // Into the gel, or off its surface.
            vec3 x = p + d * tHit;
            vec3 ng = slabNormal(x);
            // The top's cut edges scatter the trapped light: a thin bright lip.
            if (ng.y > 0.5) {
                float ex = min(hw - abs(x.x), hd - abs(x.z));
                float lip = smoothstep(0.0012, 0.0, ex);
                L += T * u.lip.xyz * u.lip.w * lip * u.emitter.w;
            }
            vec3 n = ripple(x, ng);
            float cosi = max(0.01, -dot(d, n));
            float eta = 1.0 / IOR;
            float sin2t = eta * eta * (1.0 - cosi * cosi);
            float cost = sqrt(max(0.0, 1.0 - sin2t));
            float F = fresnel(cosi, cost, eta);
            // What the wet surface mirrors (the lamp, the room, the plate) is
            // taken outright rather than by chance: a lamp 14 times brighter
            // than anything else, found by a few rays in a hundred, would
            // leave white specks that take ages to average out.
            vec3 rd = reflect(d, n);
            if (dot(rd, ng) > 0.0) L += T * F * outside(x + ng * EPS, rd);
            T *= 1.0 - F;
            d = normalize(eta * d + (eta * cosi - cost) * n);
            p = x - ng * EPS;
            inside = true;
            continue;
        }
        // Through the gel, gathering the stain's light step by step until
        // the ray leaves the block or is scattered off course.
        float t0, t1;
        if (!slabBox(p - d * 0.001, d, t0, t1)) break;
        t1 = max(t1 - 0.001, 0.0005);
        int steps = clamp(int(t1 / 0.0005) + 1, 3, 64);
        float ds = t1 / float(steps);
        float ps = 1.0 - exp(-u.sigma.w * ds);
        bool scattered = false, left = false;
        float tPrev = 0.0, tExit = 0.0;
        vec3 emit, absorb;
        for (int i = 0; i < steps; i++) {
            float tq = (float(i) + 1.0) * ds;
            vec3 q = p + d * tq;
            if (slabSDF(q) > 0.0) {
                // Crossed a face: find it, and take the gel up to it.
                float a = tPrev, b = tq;
                for (int j = 0; j < 5; j++) {
                    float m = 0.5 * (a + b);
                    if (slabSDF(p + d * m) > 0.0) b = m; else a = m;
                }
                float dl = a - tPrev;
                medium(p + d * (tPrev + 0.5 * dl), emit, absorb);
                L += T * emit * dl;
                T *= exp(-absorb * dl);
                tExit = 0.5 * (a + b);
                left = true;
                break;
            }
            medium(p + d * (tq - 0.5 * ds), emit, absorb);
            L += T * emit * ds;
            T *= exp(-absorb * ds);
            if (scatters < 4 && rnd(seed) < ps) {
                d = sampleHG(d, u.look.w, seed);
                p = q;
                scatters++;
                scattered = true;
                break;
            }
            tPrev = tq;
        }
        if (scattered) continue;
        if (!left) break;
        // At the face: out, or back in off its inside.
        vec3 x = p + d * tExit;
        vec3 ng = slabNormal(x);
        vec3 n = ripple(x, ng);
        float cosi = max(0.01, dot(d, n));
        float eta = IOR;
        float sin2t = eta * eta * (1.0 - cosi * cosi);
        if (sin2t >= 1.0) { d = reflect(d, n); p = x - ng * EPS; continue; }
        float cost = sqrt(1.0 - sin2t);
        float F = fresnel(cosi, cost, eta);
        vec3 out_ = normalize(eta * d - (eta * cosi - cost) * n);
        // The bottom face lies wet on the glass, near enough index matched:
        // the plate's light comes through whole and nothing mirrors back
        // (a second, dimmer lamp off the bottom would read as a stray blob).
        if (ng.y < -0.5) { L += T * plate(x.xz); break; }
        // Out through the top or a side: what's there is taken outright, and the
        // ray goes on inside with what the face reflected (a small share).
        L += T * (1.0 - F) * outside(x + ng * EPS, out_);
        T *= F;
        if (max(T.x, max(T.y, T.z)) < 0.01) break;
        d = reflect(d, n);
        p = x - ng * EPS;
    }
    // A stray NaN would stain the pixel for good.
    if (any(isnan(L)) || any(isinf(L))) L = vec3(0.0);
    L = min(L, vec3(6.0));
    vec4 acc = (u.counts.y == 0u) ? vec4(0.0) : texelFetch(prev, ivec2(gid), 0);
    fragColor = acc + vec4(L, 1.0);
}
)";

// The samples so far, averaged and developed as a phone would: a little
// bloom off the bright parts, darker corners, grain, a filmic curve. Draws
// at the widget's size, scaling the traced picture up if it's smaller.
const char *const kResolve = R"(
uniform sampler2D accum;
uniform vec2 outSize;

vec3 at(vec2 uv, vec2 offset, float n) {
    return texture(accum, uv + offset / vec2(u.counts.zw)).rgb / n;
}

void main() {
    vec2 uv = gl_FragCoord.xy / outSize;
    float n = max(1.0, float(u.counts.y));
    vec3 c = at(uv, vec2(0.0), n);
    // With few samples yet, blur the grain a little: a moving picture is soft anyway.
    float soft = clamp(1.0 - n / 10.0, 0.0, 0.75);
    if (soft > 0.0) {
        vec3 avg = vec3(0.0);
        for (int j = -1; j <= 1; j++)
            for (int i = -1; i <= 1; i++)
                avg += at(uv, vec2(i, j), n);
        c = mix(c, avg / 9.0, soft);
    }
    vec3 bloom = vec3(0.0);
    float wsum = 0.0;
    float stride = max(1.0, float(u.counts.z) / 420.0) * 2.0;
    for (int j = -3; j <= 3; j++) {
        for (int i = -3; i <= 3; i++) {
            float w = exp(-0.35 * float(i * i + j * j));
            bloom += max(at(uv, vec2(i, j) * stride, n) - vec3(1.1), vec3(0.0)) * w;
            wsum += w;
        }
    }
    c = (c + bloom / wsum * u.tune1.x) * u.look.x;
    vec2 v = uv - 0.5;
    c *= 1.0 - u.tune1.y * dot(v, v);
    // Contrast, about mid grey: the plate's dark corners sink and the bands
    // stand out, as a phone's camera would push the picture.
    c = max(0.18 + (c - 0.18) * u.tune0.x, 0.0);
    // Shot noise: more where there's more light, none in the black.
    float g = (hashf(gl_FragCoord.xy + vec2(float(u.counts.x % 977u), 0.0)) - 0.5) * u.look.y;
    c += g * 2.5 * sqrt(max(c, vec3(0.0)));
    // A black and white camera sees only how much light.
    if (u.plate.w > 0.5) c = vec3(dot(c, vec3(0.2126, 0.7152, 0.0722)));
    // A gentle curve that keeps the colours saturated, with a soft shoulder.
    float lum = dot(c, vec3(0.2126, 0.7152, 0.0722));
    c *= 1.0 / (1.0 + 0.25 * lum);
    c = mix(0.85 + 0.15 * (1.0 - exp(-(c - 0.85) / 0.15)), c, lessThan(c, vec3(0.85)));
    c = pow(clamp(c, vec3(0.0), vec3(1.0)), vec3(1.0 / 2.2));
    fragColor = vec4(c, 1.0);
}
)";

GLuint compile(GLenum type, const std::string &source, std::string &error)
{
    const GLuint s = glCreateShader(type);
    const char *text = source.c_str();
    glShaderSource(s, 1, &text, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        error = log;
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(const std::string &fragment, std::string &error)
{
    const GLuint v = compile(GL_VERTEX_SHADER, kVertex, error);
    const GLuint f = v ? compile(GL_FRAGMENT_SHADER, fragment, error) : 0;
    if (!f) {
        if (v)
            glDeleteShader(v);
        return 0;
    }
    const GLuint p = glCreateProgram();
    glAttachShader(p, v);
    glAttachShader(p, f);
    glLinkProgram(p);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048] = {};
        glGetProgramInfoLog(p, sizeof log, nullptr, log);
        error = log;
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

} // namespace

using Vec3 = Gel3DView::Vec3;
static Vec3 operator+(Vec3 a, Vec3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
static Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
static Vec3 operator*(Vec3 a, float k) { return {a.x * k, a.y * k, a.z * k}; }
static Vec3 cross(Vec3 a, Vec3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
static Vec3 normalize(Vec3 a)
{
    const float l = std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
    return {a.x / l, a.y / l, a.z / l};
}

const char *gel3DSceneTitle(Gel3DScene scene)
{
    switch (scene) {
    case Gel3DScene::PinkOnBlue:
        return "Blue Light · Pink";
    case Gel3DScene::GreenOnBlue:
        return "Blue Light · Green";
    case Gel3DScene::BlackAndWhite:
        return "Black & White";
    }
    return "";
}

Gel3DView::Gel3DView()
{
    m_area = gtk_gl_area_new();
    gtk_gl_area_set_required_version(GTK_GL_AREA(m_area), 3, 3);
#if GTK_CHECK_VERSION(4, 12, 0)
    gtk_gl_area_set_allowed_apis(GTK_GL_AREA(m_area), GDK_GL_API_GL);
#endif
    gtk_gl_area_set_auto_render(GTK_GL_AREA(m_area), FALSE);
    gtk_widget_set_hexpand(m_area, TRUE);
    gtk_widget_set_vexpand(m_area, TRUE);
    g_signal_connect(m_area, "realize", G_CALLBACK(realize), this);
    g_signal_connect(m_area, "unrealize", G_CALLBACK(unrealize), this);
    g_signal_connect(m_area, "render", G_CALLBACK(renderCb), this);
    g_signal_connect_swapped(m_area, "resize", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Gel3DView *>(p)->restart();
                             }),
                             this);
    g_signal_connect_swapped(m_area, "map", G_CALLBACK(+[](gpointer p) {
                                 static_cast<Gel3DView *>(p)->restart();
                             }),
                             this);

    // Drag to orbit, scroll to come closer, double-click to go back.
    // Drag to orbit; drag with the right or middle button, or with Shift, to
    // move the gel across the view.
    GtkGesture *drag = gtk_gesture_drag_new();
    gtk_gesture_single_set_button(GTK_GESTURE_SINGLE(drag), 0);
    g_signal_connect(drag, "drag-begin", G_CALLBACK(+[](GtkGestureDrag *g, double, double, gpointer p) {
                         auto *self = static_cast<Gel3DView *>(p);
                         self->m_dragX = self->m_dragY = 0;
                         const guint button = gtk_gesture_single_get_current_button(GTK_GESTURE_SINGLE(g));
                         const GdkModifierType mods = gtk_event_controller_get_current_event_state(GTK_EVENT_CONTROLLER(g));
                         self->m_panning = button != GDK_BUTTON_PRIMARY || (mods & GDK_SHIFT_MASK);
                     }),
                     this);
    g_signal_connect(drag, "drag-update", G_CALLBACK(+[](GtkGestureDrag *, double x, double y, gpointer p) {
                         auto *self = static_cast<Gel3DView *>(p);
                         const double dx = x - self->m_dragX;
                         const double dy = y - self->m_dragY;
                         self->m_dragX = x;
                         self->m_dragY = y;
                         if (self->m_panning) {
                             self->pan(dx, dy);
                         } else {
                             self->m_yaw -= float(dx) * 0.008f;
                             // Past the top it snaps to straight down, the imager's view.
                             self->m_pitch = std::clamp(self->m_pitch + float(dy) * 0.008f, 0.22f, float(M_PI_2));
                             if (self->m_pitch > 1.5f)
                                 self->m_pitch = float(M_PI_2);
                         }
                         self->interacted();
                     }),
                     this);
    gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(drag));
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_VERTICAL);
    g_signal_connect(scroll, "scroll", G_CALLBACK(+[](GtkEventControllerScroll *c, double, double dy, gpointer p) {
                         auto *self = static_cast<Gel3DView *>(p);
                         // A mouse wheel's notch is one unit; a touchpad's are pixels.
                         const bool wheel = gtk_event_controller_scroll_get_unit(c) == GDK_SCROLL_UNIT_WHEEL;
                         self->m_zoom = std::clamp(self->m_zoom * (1 + float(dy) * (wheel ? 0.06f : 0.004f)), 0.45f, 2.0f);
                         self->interacted();
                         return TRUE;
                     }),
                     this);
    gtk_widget_add_controller(m_area, scroll);
    GtkGesture *click = gtk_gesture_click_new();
    g_signal_connect(click, "pressed", G_CALLBACK(+[](GtkGestureClick *, int n, double, double, gpointer p) {
                         if (n != 2)
                             return;
                         auto *self = static_cast<Gel3DView *>(p);
                         self->m_yaw = 0;
                         self->m_pitch = kHomePitch;
                         self->m_zoom = 1;
                         self->m_pan.x = self->m_pan.y = self->m_pan.z = 0;
                         self->restart();
                     }),
                     this);
    gtk_widget_add_controller(m_area, GTK_EVENT_CONTROLLER(click));
    gtk_widget_set_tooltip_text(m_area, "Drag to turn the gel, right-drag or Shift-drag to move it, scroll to come closer, double-click to go back");
    gtk_accessible_update_property(GTK_ACCESSIBLE(m_area), GTK_ACCESSIBLE_PROPERTY_LABEL, "Gel in 3D", -1);

    m_note = gtk_label_new("");
    gtk_label_set_wrap(GTK_LABEL(m_note), TRUE);
    gtk_label_set_justify(GTK_LABEL(m_note), GTK_JUSTIFY_CENTER);
    gtk_widget_add_css_class(m_note, "dim-label");
    gtk_widget_set_halign(m_note, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(m_note, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_start(m_note, 30);
    gtk_widget_set_margin_end(m_note, 30);
    gtk_widget_set_visible(m_note, FALSE);

    m_root = gtk_overlay_new();
    gtk_overlay_set_child(GTK_OVERLAY(m_root), m_area);
    gtk_overlay_add_overlay(GTK_OVERLAY(m_root), m_note);
    g_object_ref_sink(m_root);
    rebuild();
}

Gel3DView::~Gel3DView()
{
    if (m_tick)
        gtk_widget_remove_tick_callback(m_area, m_tick);
    if (m_idle)
        g_source_remove(m_idle);
    if (m_buildTimer)
        g_source_remove(m_buildTimer);
    g_signal_handlers_disconnect_by_data(m_area, this);
    g_object_unref(m_root);
}

void Gel3DView::show(const std::vector<GelLaneModel> &lanes, double runLevel, const std::vector<double> &dyeRuns)
{
    m_lanes = lanes;
    m_runLevel = runLevel;
    m_dyeRuns = dyeRuns;
    // Redrawing the stain map is the slow part: at most ~30 times a second.
    const gint64 wait = m_lastBuild + G_USEC_PER_SEC / 30 - g_get_monotonic_time();
    if (wait <= 0)
        rebuild();
    else if (!m_buildTimer)
        m_buildTimer = g_timeout_add(guint(wait / 1000) + 1, +[](gpointer p) -> gboolean {
            auto *self = static_cast<Gel3DView *>(p);
            self->m_buildTimer = 0;
            self->rebuild();
            return G_SOURCE_REMOVE;
        }, this);
}

void Gel3DView::setScene(Gel3DScene scene)
{
    if (scene == m_scene)
        return;
    m_scene = scene;
    restart();
}

void Gel3DView::setLook(const Gel3DLook &look)
{
    m_look = look;
    restart();
}

void Gel3DView::setComb(const GelComb &comb)
{
    if (comb.teeth == m_comb.teeth && comb.width == m_comb.width && comb.thickness == m_comb.thickness)
        return;
    m_comb = comb;
    rebuild();
}

float Gel3DView::pitch() const
{
    return std::max(0.062f / float(m_comb.teeth), float(m_comb.width) / 1000 + 0.0008f);
}

float Gel3DView::margin() const
{
    return (0.070f - pitch() * float(m_comb.teeth)) / 2;
}

void Gel3DView::rebuild()
{
    m_lastBuild = g_get_monotonic_time();
    // The tray's width; more lanes than the comb has teeth spill over it.
    m_width = 2 * margin() + pitch() * float(std::max(m_comb.teeth, int(m_lanes.size())));
    m_fit = std::max(m_width * 1.15f, kDepth * 1.6f);
    makeBandMap();
    restart();
}

// Starts the picture over: the next frames sample it afresh.
void Gel3DView::restart()
{
    m_samples = 0;
    if (!m_problem.empty() || !gtk_widget_get_mapped(m_area))
        return;
    if (!m_tick)
        m_tick = gtk_widget_add_tick_callback(m_area, tick, this, nullptr);
}

// Being turned or zoomed: trace coarsely until it rests for a moment.
void Gel3DView::interacted()
{
    m_moving = true;
    if (m_idle)
        g_source_remove(m_idle);
    m_idle = g_timeout_add(180, +[](gpointer p) -> gboolean {
        auto *self = static_cast<Gel3DView *>(p);
        self->m_idle = 0;
        self->m_moving = false;
        self->restart();
        return G_SOURCE_REMOVE;
    }, this);
    restart();
}

gboolean Gel3DView::tick(GtkWidget *, GdkFrameClock *, gpointer p)
{
    auto *self = static_cast<Gel3DView *>(p);
    if (self->m_samples >= self->maxSamples() || !self->m_problem.empty()) {
        self->m_tick = 0;
        return G_SOURCE_REMOVE;
    }
    gtk_gl_area_queue_render(GTK_GL_AREA(self->m_area));
    return G_SOURCE_CONTINUE;
}

uint32_t Gel3DView::maxSamples() const
{
    // A frame slower than 60 ms a sample (software GL, a tired laptop)
    // settles for a grainier picture rather than tracing for minutes.
    return m_frameMs > 60 ? 48 : kMaxSamples;
}

// The camera orbits a point just above the middle of the gel, moved by panning.
Gel3DView::Camera Gel3DView::camera() const
{
    Camera c;
    // Tilted towards straight down, the camera stands further back: from
    // above the whole gel has to fit, with room under it for the pill.
    const float fitTop = kDepth * 0.5f * 1.55f / std::tan(17.5f * float(M_PI) / 180);
    const float above = std::clamp((m_pitch - kHomePitch) / (float(M_PI_2) - kHomePitch), 0.0f, 1.0f);
    c.distance = (m_fit + (std::max(fitTop, m_fit) - m_fit) * above) * m_zoom;
    const Vec3 target = Vec3{0, kThick / 2, 0.004f} + m_pan;
    c.position = target + Vec3{c.distance * std::sin(m_yaw) * std::cos(m_pitch), c.distance * std::sin(m_pitch),
                               c.distance * std::cos(m_yaw) * std::cos(m_pitch)};
    c.forward = normalize(target - c.position);
    // Right comes from the yaw alone, so looking straight down still has a
    // well-defined up (the wells' side at the top of the picture).
    c.right = {std::cos(m_yaw), 0, -std::sin(m_yaw)};
    c.up = cross(c.right, c.forward);
    return c;
}

// Moves the gel with the pointer: by `dx`, `dy` pixels across the view.
void Gel3DView::pan(double dx, double dy)
{
    const Camera c = camera();
    const double h = std::max(1, gtk_widget_get_height(m_area));
    const float perPx = float(2.0 * c.distance * std::tan(17.5 * M_PI / 180) / h);
    m_pan = m_pan - c.right * (float(dx) * perPx) + c.up * (float(dy) * perPx);
    // Not out of sight: the gel stays within reach of the middle.
    const float limit = m_fit * 0.7f;
    m_pan.x = std::clamp(m_pan.x, -limit, limit);
    m_pan.y = std::clamp(m_pan.y, -limit, limit);
    m_pan.z = std::clamp(m_pan.z, -limit, limit);
}

void Gel3DView::fail(const std::string &message)
{
    m_problem = message;
    g_warning("%s", message.c_str());
    gtk_label_set_text(GTK_LABEL(m_note), message.c_str());
    gtk_widget_set_visible(m_note, TRUE);
    gtk_widget_set_opacity(m_area, 0);
}

// Where the DNA is, seen from above, as a picture the shaders sample: red =
// DNA, green = loading dye, blue = well (half: its walls). Row 0 is the
// wells' side (−z).
//
// A band is a cloud of DNA, not a shape: along the run its brightness is a
// bell curve that widens the farther it ran (diffusion), with a faint wider
// haze and, when heavily loaded, a tail back toward the well; across the
// lane it's a little brighter in the middle and fades at the walls; its ends
// curl toward the well; it leans a touch; the stain in it is speckled; and
// no two are alike.
void Gel3DView::makeBandMap()
{
    const int w = 1024, h = std::max(128, std::min(2048, int(float(w) * kDepth / m_width)));
    const float width = m_width, depth = kDepth;
    const float ppm = float(w) / width, mm = ppm / 1000;
    const float laneW = float(m_comb.width) / 1000, pit = pitch(), mar = margin();
    const float thickness = float(m_comb.thickness);
    auto laneX = [&](size_t i) { return -width / 2 + mar + (float(i) + 0.5f) * pit; };
    auto px = [&](float x) { return (x + width / 2) * ppm; };
    auto pz = [&](float z) { return (z + depth / 2) * ppm; };
    const float wellZ = -depth / 2 + 0.010f;
    const float runTop = wellZ + 0.004f, runBottom = depth / 2 - 0.008f;
    auto runZ = [&](double p) { return runTop + float(p) * (runBottom - runTop); };
    Noise random{11};
    const float half = laneW * ppm * 0.46f;
    std::vector<float> r(size_t(w * h)), g(size_t(w * h)), b(size_t(w * h));
    // The rows or columns a thing covers, clipped to the picture.
    struct Span {
        int lo, hi;
    };
    auto span = [](float a, float b, int n) {
        const int lo = std::min(n, std::max(0, int(std::floor(a))));
        return Span{lo, std::max(lo, std::min(n, int(std::ceil(b)) + 1))};
    };

    // The loading dyes' fronts: broad, soft, dark bands across every lane,
    // the first (bromophenol blue) the darker, a little uneven lane to lane.
    for (size_t d = 0; d < m_dyeRuns.size(); ++d) {
        const double p = m_dyeRuns[d] * m_runLevel;
        if (p > 1.02)
            continue;
        const float y = pz(runZ(p)), sigma = (1.3f + 0.3f * float(d)) * mm, strength = d == 0 ? 0.55f : 0.32f;
        const float left = px(-width / 2) + 2 * mm, right = px(width / 2) - 2 * mm;
        const Span rows = span(y - 4 * sigma, y + 4 * sigma, h), cols = span(left, right, w);
        for (int j = rows.lo; j < rows.hi; ++j) {
            const float dz = (float(j) + 0.5f - y) / sigma;
            const float along = std::exp(-0.5f * dz * dz);
            for (int i = cols.lo; i < cols.hi; ++i) {
                const float x = float(i) + 0.5f;
                const float side = std::min(1.0f, std::min(x - left, right - x) / (2 * mm));
                const float uneven = 0.85f + 0.3f * Noise::value(x / (laneW * ppm) + 3, y / (8 * mm));
                g[size_t(j * w + i)] += strength * along * side * uneven;
            }
        }
    }

    for (size_t i = 0; i < m_lanes.size(); ++i) {
        const float cx = px(laneX(i)), load = loading(i);
        for (const GelBandModel &band : m_lanes[i].bands) {
            const double p = band.run * m_runLevel;
            if (p > 1.03)
                continue;
            // A thin comb loads a thin plug, so its bands start sharper and brighter.
            const float k = std::min(1.0f, float(band.intensity) * load / std::sqrt(thickness));
            const float y0 = pz(runZ(p));
            // How far the DNA has spread: the plug's thickness, more for a
            // longer run, a touch more when there's more of it.
            const float sigma = (0.12f + 0.22f * thickness + 0.22f * k) * mm * (1 + 0.7f * float(p));
            const float smile = (0.12f + 0.12f * random.next()) * mm * (1 + float(p));
            const float lean = (random.next() - 0.5f) * 0.3f * mm;
            const float reach = half * (0.97f + 0.05f * random.next());
            const float tail = std::max(0.0f, k - 0.45f) * 0.5f;
            const float tailLength = (0.6f + 1.6f * k) * mm;
            const float speckle = random.next() * 100;
            const float above = y0 - 4 * sigma - (tail > 0 ? 4 * tailLength : 0), below = y0 + 4 * sigma;
            const Span rows = span(above, below, h), cols = span(cx - reach - 0.4f * mm, cx + reach + 0.4f * mm, w);
            for (int row = rows.lo; row < rows.hi; ++row) {
                const float yy = float(row) + 0.5f;
                for (int col = cols.lo; col < cols.hi; ++col) {
                    const float xx = float(col) + 0.5f;
                    const float u = (xx - cx) / reach;
                    // The ends lag behind the middle, and the band leans a little.
                    const float centre = y0 - smile * u * u + lean * u;
                    const float dz = yy - centre;
                    float along = std::exp(-0.5f * (dz / sigma) * (dz / sigma));
                    along += 0.15f * std::exp(-0.5f * (dz / (3 * sigma)) * (dz / (3 * sigma)));
                    if (tail > 0 && dz < 0)
                        along += tail * std::exp(dz / tailLength);
                    const float across = std::min(1.0f, std::max(0.0f, (reach - std::fabs(xx - cx)) / (0.3f * mm)))
                                       * (0.86f + 0.14f * (1 - u * u));
                    const float grain = 0.88f + 0.24f * Noise::value(xx / (0.35f * mm) + speckle, yy / (0.35f * mm));
                    r[size_t(row * w + col)] += k * along * across * grain;
                }
            }
        }
    }
    // Every well the comb cast, loaded or not: its cavity, and its walls
    // (half blue) which catch the light.
    const size_t wells = size_t(std::max(m_comb.teeth, int(m_lanes.size())));
    for (size_t i = 0; i < wells; ++i) {
        const float cx = px(laneX(i));
        const float wx0 = cx - half * 0.95f, wx1 = cx + half * 0.95f;
        const float wy0 = pz(wellZ) - 0.5f * thickness * mm, wy1 = pz(wellZ) + 0.5f * thickness * mm;
        const float wall = 0.3f * mm;
        const Span rows = span(wy0 - wall, wy1 + wall, h), cols = span(wx0 - wall, wx1 + wall, w);
        for (int row = rows.lo; row < rows.hi; ++row) {
            const float yy = float(row) + 0.5f;
            for (int col = cols.lo; col < cols.hi; ++col) {
                const float xx = float(col) + 0.5f;
                const bool inside = xx > wx0 && xx < wx1 && yy > wy0 && yy < wy1;
                b[size_t(row * w + col)] = inside ? 1 : 0.5f;
            }
        }
    }

    auto byte = [](float v) { return uint8_t(std::clamp(v * 255, 0.0f, 255.0f)); };
    m_map.assign(size_t(w * h * 4), 255);
    for (size_t i = 0; i < size_t(w * h); ++i) {
        m_map[i * 4] = byte(r[i]);
        m_map[i * 4 + 1] = byte(g[i]);
        m_map[i * 4 + 2] = byte(b[i]);
    }
    m_mapW = w;
    m_mapH = h;
    m_bandDirty = true;
}

void Gel3DView::realize(GtkGLArea *area, gpointer p)
{
    auto *self = static_cast<Gel3DView *>(p);
    gtk_gl_area_make_current(area);
    if (GError *error = gtk_gl_area_get_error(area)) {
        self->fail(std::string("The 3D gel needs OpenGL 3.3: ") + error->message);
        return;
    }
    if (!self->setUpGl())
        return;
    self->m_glReady = true;
    self->restart();
}

bool Gel3DView::setUpGl()
{
    std::string error;
    m_trace = link(std::string(kCommon) + kTrace, error);
    if (m_trace)
        m_resolve = link(std::string(kCommon) + kResolve, error);
    if (!m_trace || !m_resolve) {
        fail("The 3D gel's shaders didn't compile: " + error);
        return false;
    }
    glGenVertexArrays(1, &m_vao);
    glGenTextures(1, &m_bandTex);
    glBindTexture(GL_TEXTURE_2D, m_bandTex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenTextures(2, m_accum);
    glGenFramebuffers(2, m_fbo);
    m_bandDirty = true;
    m_accumW = m_accumH = 0;
    return true;
}

void Gel3DView::unrealize(GtkGLArea *area, gpointer p)
{
    auto *self = static_cast<Gel3DView *>(p);
    gtk_gl_area_make_current(area);
    if (gtk_gl_area_get_error(area))
        return;
    if (self->m_trace)
        glDeleteProgram(self->m_trace);
    if (self->m_resolve)
        glDeleteProgram(self->m_resolve);
    if (self->m_vao)
        glDeleteVertexArrays(1, &self->m_vao);
    if (self->m_bandTex)
        glDeleteTextures(1, &self->m_bandTex);
    if (self->m_accum[0])
        glDeleteTextures(2, self->m_accum);
    if (self->m_fbo[0])
        glDeleteFramebuffers(2, self->m_fbo);
    self->m_trace = self->m_resolve = self->m_vao = self->m_bandTex = 0;
    self->m_accum[0] = self->m_accum[1] = self->m_fbo[0] = self->m_fbo[1] = 0;
    self->m_glReady = false;
}

gboolean Gel3DView::renderCb(GtkGLArea *, GdkGLContext *, gpointer p)
{
    auto *self = static_cast<Gel3DView *>(p);
    if (self->m_glReady)
        self->render();
    return TRUE;
}

void Gel3DView::render()
{
    const int scale = gtk_widget_get_scale_factor(m_area);
    const int outW = gtk_widget_get_width(m_area) * scale, outH = gtk_widget_get_height(m_area) * scale;
    if (outW <= 0 || outH <= 0)
        return;
    // Trace up to about a megapixel, a quarter of that while it's turning,
    // and let the resolve pass scale it up.
    double tw = outW, th = outH;
    const double cap = m_moving ? kPixelCap / 4 : kPixelCap;
    if (tw * th > cap) {
        const double s = std::sqrt(cap / (tw * th));
        tw *= s;
        th *= s;
    }
    const int w = std::max(8, int(std::lround(tw))), h = std::max(8, int(std::lround(th)));
    if (w != m_accumW || h != m_accumH) {
        for (int i = 0; i < 2; ++i) {
            glBindTexture(GL_TEXTURE_2D, m_accum[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA32F, w, h, 0, GL_RGBA, GL_FLOAT, nullptr);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[i]);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_accum[i], 0);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                gtk_gl_area_attach_buffers(GTK_GL_AREA(m_area));
                fail("The 3D gel needs floating-point render targets, which this GPU doesn't offer.");
                return;
            }
        }
        m_accumW = w;
        m_accumH = h;
        m_samples = 0;
    }
    if (m_bandDirty && !m_map.empty()) {
        glBindTexture(GL_TEXTURE_2D, m_bandTex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, m_mapW, m_mapH, 0, GL_RGBA, GL_UNSIGNED_BYTE, m_map.data());
        m_bandDirty = false;
    }

    const SceneLook &look = lookOf(m_scene);
    const Camera cam = camera();
    const Vec3 position = cam.position, f = cam.forward, r = cam.right, up = cam.up;
    auto setUniforms = [&](GLuint prog) {
        auto v4 = [&](const char *name, float a, float b, float c, float d) {
            glUniform4f(glGetUniformLocation(prog, name), a, b, c, d);
        };
        auto v4a = [&](const char *name, const float *a) { v4(name, a[0], a[1], a[2], a[3]); };
        v4("u.camPos", position.x, position.y, position.z, 0);
        v4("u.camF", f.x, f.y, f.z, 0);
        v4("u.camR", r.x, r.y, r.z, 0);
        v4("u.camU", up.x, up.y, up.z, 0);
        v4("u.sigma", look.sigma[0], look.sigma[1], look.sigma[2], m_look.scatter);
        v4("u.lens", std::tan(17.5f * float(M_PI) / 180), float(w) / float(h), 0.0005f, cam.distance);
        v4("u.slab", m_width, kDepth, kThick, 0.7f);
        v4("u.emitter", m_width / 2 + 0.13f, kDepth / 2 + 0.11f, 0.025f, 1.0f);
        v4("u.look", m_look.exposure, m_look.grain, 14, 0.75f);
        v4("u.tune0", m_look.contrast, m_look.lamp, m_look.lampSize, m_look.spot);
        v4("u.tune1", m_look.bloom, m_look.vignette, m_look.stain, m_look.band);
        v4a("u.plate", look.plate);
        v4a("u.stain", look.stain);
        v4a("u.band", look.band);
        v4a("u.lip", look.lip);
        glUniform4ui(glGetUniformLocation(prog, "u.counts"), m_frame, m_samples, GLuint(w), GLuint(h));
    };

    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glBindVertexArray(m_vao);
    if (m_samples < maxSamples()) {
        const gint64 now = g_get_monotonic_time();
        if (m_lastRender && now - m_lastRender < G_USEC_PER_SEC)
            m_frameMs = 0.8 * m_frameMs + 0.2 * double(now - m_lastRender) / 1000;
        m_lastRender = now;
        // accum[0] holds the samples so far; add one into accum[1], then swap.
        glBindFramebuffer(GL_FRAMEBUFFER, m_fbo[1]);
        glViewport(0, 0, w, h);
        glUseProgram(m_trace);
        setUniforms(m_trace);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, m_accum[0]);
        glUniform1i(glGetUniformLocation(m_trace, "prev"), 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, m_bandTex);
        glUniform1i(glGetUniformLocation(m_trace, "bands"), 1);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        std::swap(m_accum[0], m_accum[1]);
        std::swap(m_fbo[0], m_fbo[1]);
        ++m_samples;
        ++m_frame;
    }
    gtk_gl_area_attach_buffers(GTK_GL_AREA(m_area));
    glViewport(0, 0, outW, outH);
    glUseProgram(m_resolve);
    setUniforms(m_resolve);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_accum[0]);
    glUniform1i(glGetUniformLocation(m_resolve, "accum"), 0);
    glUniform2f(glGetUniformLocation(m_resolve, "outSize"), float(outW), float(outH));
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
}

} // namespace oc::gtk
