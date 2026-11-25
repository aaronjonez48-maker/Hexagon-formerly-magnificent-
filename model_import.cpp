// model_import.cpp - import .zip / .prisma containers, Wavefront .obj (+ .mtl), glTF binary .glb and PNG / JPEG textures.
// Std-only, C++11, no zlib needed (own inflate). Included by presto_ui.cpp inside `namespace mx { using namespace std; ... }`
// right after mesh_export.cpp (needs Mesh3, Bytes, readFileBytes, exportDir, makeDir from there).
#include <array>
#include <functional>
#include <map>
#include <set>

// ============================================================ inflate (RFC 1951), compact "puff"-style decoder
struct InfHuf { short count[16]; short symbol[320]; };
static int infConstruct(InfHuf* h, const short* length, int n) {
  int symbol, len, left; short offs[16];
  for (len = 0; len <= 15; len++) h->count[len] = 0;
  for (symbol = 0; symbol < n; symbol++) h->count[length[symbol]]++;
  if (h->count[0] == n) return 0;
  left = 1;
  for (len = 1; len <= 15; len++) { left <<= 1; left -= h->count[len]; if (left < 0) return left; }
  offs[1] = 0;
  for (len = 1; len < 15; len++) offs[len + 1] = (short)(offs[len] + h->count[len]);
  for (symbol = 0; symbol < n; symbol++) if (length[symbol] != 0) h->symbol[offs[length[symbol]]++] = (short)symbol;
  return left;
}
struct Inflater {
  const uint8_t* in; size_t inLen, inPos; long bitbuf; int bitcnt; Bytes* out; bool err;
  int bits(int need) {
    long val = bitbuf;
    while (bitcnt < need) { if (inPos >= inLen) { err = true; return 0; } val |= (long)in[inPos++] << bitcnt; bitcnt += 8; }
    bitbuf = val >> need; bitcnt -= need;
    return (int)(val & ((1L << need) - 1));
  }
  int decode(const InfHuf* h) {
    int len, code = 0, first = 0, count, index = 0;
    for (len = 1; len <= 15; len++) {
      code |= bits(1); if (err) return -1;
      count = h->count[len];
      if (code - count < first) return h->symbol[index + (code - first)];
      index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
  }
  bool codes(const InfHuf* lc, const InfHuf* dc) {
    static const short lens[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
    static const short lext[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const short dists[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const short dext[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    for (;;) {
      int sym = decode(lc);
      if (sym < 0 || err) return false;
      if (sym < 256) out->push_back((uint8_t)sym);
      else if (sym == 256) return true;
      else {
        sym -= 257; if (sym >= 29) return false;
        int len = lens[sym] + bits(lext[sym]);
        int ds = decode(dc); if (ds < 0 || ds >= 30 || err) return false;
        size_t dist = (size_t)dists[ds] + (size_t)bits(dext[ds]);
        if (err || dist > out->size()) return false;
        size_t from = out->size() - dist;
        for (int i = 0; i < len; i++) out->push_back((*out)[from + (size_t)i]);
      }
    }
  }
  bool stored() {
    bitbuf = 0; bitcnt = 0;
    if (inPos + 4 > inLen) return false;
    unsigned len = in[inPos] | (in[inPos + 1] << 8), nlen = in[inPos + 2] | (in[inPos + 3] << 8);
    inPos += 4;
    if (len != (~nlen & 0xFFFF) || inPos + len > inLen) return false;
    out->insert(out->end(), in + inPos, in + inPos + len); inPos += len;
    return true;
  }
  bool fixedBlock() {
    static bool ready = false; static InfHuf lc, dc;
    if (!ready) {
      short l[288]; int s;
      for (s = 0; s < 144; s++) l[s] = 8;
      for (; s < 256; s++) l[s] = 9;
      for (; s < 280; s++) l[s] = 7;
      for (; s < 288; s++) l[s] = 8;
      infConstruct(&lc, l, 288);
      for (s = 0; s < 30; s++) l[s] = 5;
      infConstruct(&dc, l, 30);
      ready = true;
    }
    return codes(&lc, &dc);
  }
  bool dynamicBlock() {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320]; InfHuf lc, dc;
    int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
    if (err || nlen > 286 || ndist > 30) return false;
    int idx;
    for (idx = 0; idx < ncode; idx++) lengths[order[idx]] = (short)bits(3);
    for (; idx < 19; idx++) lengths[order[idx]] = 0;
    if (infConstruct(&lc, lengths, 19) != 0) return false;
    idx = 0;
    while (idx < nlen + ndist) {
      int sym = decode(&lc); if (sym < 0 || err) return false;
      if (sym < 16) lengths[idx++] = (short)sym;
      else {
        int len = 0;
        if (sym == 16) { if (idx == 0) return false; len = lengths[idx - 1]; sym = 3 + bits(2); }
        else if (sym == 17) sym = 3 + bits(3);
        else sym = 11 + bits(7);
        if (idx + sym > nlen + ndist) return false;
        while (sym--) lengths[idx++] = (short)len;
      }
    }
    if (lengths[256] == 0) return false;
    int e = infConstruct(&lc, lengths, nlen);
    if (e < 0 || (e > 0 && nlen - lc.count[0] != 1)) return false;
    e = infConstruct(&dc, lengths + nlen, ndist);
    if (e < 0 || (e > 0 && ndist - dc.count[0] != 1)) return false;
    return codes(&lc, &dc);
  }
};
static bool inflateRaw(const uint8_t* p, size_t n, Bytes& out, size_t reserveHint = 0) {
  Inflater s; s.in = p; s.inLen = n; s.inPos = 0; s.bitbuf = 0; s.bitcnt = 0; s.out = &out; s.err = false;
  if (reserveHint) out.reserve(reserveHint);
  int last;
  do {
    last = s.bits(1); int type = s.bits(2);
    if (s.err) return false;
    bool ok = type == 0 ? s.stored() : type == 1 ? s.fixedBlock() : type == 2 ? s.dynamicBlock() : false;
    if (!ok) return false;
  } while (!last);
  return true;
}
static bool zlibInflate(const Bytes& d, Bytes& out, size_t hint = 0) {   // 2-byte zlib header, adler ignored
  if (d.size() < 3) return false;
  return inflateRaw(&d[2], d.size() - 2, out, hint);
}
static bool gunzip(const Bytes& d, Bytes& out) {
  if (d.size() < 18 || d[0] != 0x1f || d[1] != 0x8b || d[2] != 8) return false;
  size_t p = 10; int fl = d[3];
  if (fl & 4) { if (p + 2 > d.size()) return false; p += 2 + (d[p] | (d[p + 1] << 8)); }
  if (fl & 8) { while (p < d.size() && d[p]) p++; p++; }
  if (fl & 16) { while (p < d.size() && d[p]) p++; p++; }
  if (fl & 2) p += 2;
  if (p >= d.size()) return false;
  return inflateRaw(&d[p], d.size() - p, out);
}

// ============================================================ small path helpers
static string lowerStr(string s) { for (size_t i = 0; i < s.size(); i++) s[i] = (char)tolower((unsigned char)s[i]); return s; }
static string slashFix(string s) { for (size_t i = 0; i < s.size(); i++) if (s[i] == '\\') s[i] = '/'; return s; }
static string baseNameOf(const string& p) { size_t s = p.find_last_of("/\\"); return s == string::npos ? p : p.substr(s + 1); }
static string dirNameOf(const string& p) { size_t s = p.find_last_of("/\\"); return s == string::npos ? string() : p.substr(0, s); }
static string extOf(const string& p) { string b = baseNameOf(p); size_t d = b.rfind('.'); return d == string::npos ? string() : lowerStr(b.substr(d)); }
static string trimStr(const string& s) {
  size_t a = 0, b = s.size();
  while (a < b && isspace((unsigned char)s[a])) a++;
  while (b > a && isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}
static string normPath(const string& in) {   // resolves "." and ".." ; keeps relative
  string s = slashFix(in), cur; vector<string> parts;
  for (size_t i = 0; i <= s.size(); i++) {
    if (i == s.size() || s[i] == '/') {
      if (cur == "..") { if (!parts.empty()) parts.pop_back(); }
      else if (!cur.empty() && cur != ".") parts.push_back(cur);
      cur.clear();
    } else cur += s[i];
  }
  string r; for (size_t i = 0; i < parts.size(); i++) { if (i) r += "/"; r += parts[i]; }
  return r;
}
static bool mkdirP(const string& path) {
  string cur;
  for (size_t i = 0; i < path.size(); i++) {
    cur += path[i];
    if ((path[i] == '/' && i > 0) || i + 1 == path.size()) if (!makeDir(cur) && i + 1 == path.size()) return false;
  }
  return true;
}
static bool writeFileBytes(const string& p, const Bytes& b) {
  FILE* f = fopen(p.c_str(), "wb"); if (!f) return false;
  bool ok = b.empty() || fwrite(&b[0], 1, b.size(), f) == b.size();
  fclose(f); return ok;
}

// ============================================================ ZIP (stored + deflate), read fully in memory
struct ZipEnt { string name; uint32_t method, csize, usize, off; };
static uint32_t le16(const Bytes& b, size_t o) { return o + 2 <= b.size() ? (uint32_t)(b[o] | (b[o + 1] << 8)) : 0; }
static uint32_t le32(const Bytes& b, size_t o) { return o + 4 <= b.size() ? (uint32_t)b[o] | ((uint32_t)b[o + 1] << 8) | ((uint32_t)b[o + 2] << 16) | ((uint32_t)b[o + 3] << 24) : 0; }
struct ZipVfs {
  Bytes z; vector<ZipEnt> ents;
  bool open(const Bytes& data, string& err) {
    z = data; ents.clear();
    if (z.size() < 22) { err = "not a zip (too small)"; return false; }
    long e = -1;
    for (long i = (long)z.size() - 22; i >= 0 && i >= (long)z.size() - 65600; i--)
      if (z[(size_t)i] == 'P' && z[(size_t)i + 1] == 'K' && z[(size_t)i + 2] == 5 && z[(size_t)i + 3] == 6) { e = i; break; }
    if (e < 0) { err = "not a zip (no end record)"; return false; }
    uint32_t n = le16(z, (size_t)e + 10), cdOff = le32(z, (size_t)e + 16);
    if (n == 0xFFFF || cdOff == 0xFFFFFFFFu) { err = "zip64 not supported"; return false; }
    size_t p = cdOff;
    for (uint32_t i = 0; i < n; i++) {
      if (p + 46 > z.size() || le32(z, p) != 0x02014b50) break;
      ZipEnt en; en.method = le16(z, p + 10); en.csize = le32(z, p + 20); en.usize = le32(z, p + 24);
      uint32_t nl = le16(z, p + 28), xl = le16(z, p + 30), cl = le16(z, p + 32); en.off = le32(z, p + 42);
      if (p + 46 + nl > z.size()) break;
      en.name = slashFix(string((const char*)&z[p + 46], nl));
      if (en.csize == 0xFFFFFFFFu || en.usize == 0xFFFFFFFFu) { err = "zip64 entry not supported"; return false; }
      if (!en.name.empty() && en.name[en.name.size() - 1] != '/') ents.push_back(en);
      p += 46 + nl + xl + cl;
    }
    if (ents.empty()) { err = "zip is empty or unreadable"; return false; }
    return true;
  }
  bool read(const ZipEnt& e, Bytes& out, string& err) const {
    out.clear();
    size_t p = e.off;
    if (p + 30 > z.size() || le32(z, p) != 0x04034b50) { err = "bad zip entry " + e.name; return false; }
    size_t ds = p + 30 + le16(z, p + 26) + le16(z, p + 28);
    if (ds + e.csize > z.size()) { err = "truncated zip entry " + e.name; return false; }
    if (e.method == 0) { out.assign(z.begin() + (long)ds, z.begin() + (long)(ds + e.csize)); return true; }
    if (e.method == 8) {
      if (!inflateRaw(&z[ds], e.csize, out, e.usize)) { err = "inflate failed for " + e.name; return false; }
      return true;
    }
    err = "unsupported zip compression in " + e.name; return false;
  }
  const ZipEnt* find(const string& want) const {   // exact, then case-insensitive, then same file name anywhere
    string w = lowerStr(normPath(want)), b = lowerStr(baseNameOf(w));
    for (size_t i = 0; i < ents.size(); i++) if (lowerStr(normPath(ents[i].name)) == w) return &ents[i];
    for (size_t i = 0; i < ents.size(); i++) if (lowerStr(baseNameOf(ents[i].name)) == b) return &ents[i];
    return nullptr;
  }
  const ZipEnt* best(const char* ext) const {   // largest entry with this extension
    const ZipEnt* r = nullptr;
    for (size_t i = 0; i < ents.size(); i++)
      if (extOf(ents[i].name) == ext && (!r || ents[i].usize > r->usize)) r = &ents[i];
    return r;
  }
};

// ============================================================ images -> RGBA
struct Img { int w, h; Bytes rgba; Img() : w(0), h(0) {} };

static bool decodePNG(const Bytes& d, Img& im, string& err) {
  static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  if (d.size() < 33 || memcmp(&d[0], sig, 8) != 0) { err = "not a PNG"; return false; }
  int W = 0, H = 0, depth = 0, ct = 0, inter = 0;
  Bytes idat, plte, trns;
  size_t p = 8;
  while (p + 12 <= d.size()) {
    uint32_t len = (uint32_t)(d[p] << 24 | d[p + 1] << 16 | d[p + 2] << 8 | d[p + 3]);
    string type((const char*)&d[p + 4], 4);
    if (p + 12 + (size_t)len > d.size()) break;
    const uint8_t* c = &d[p + 8];
    if (type == "IHDR" && len >= 13) {
      W = (int)(c[0] << 24 | c[1] << 16 | c[2] << 8 | c[3]); H = (int)(c[4] << 24 | c[5] << 16 | c[6] << 8 | c[7]);
      depth = c[8]; ct = c[9]; inter = c[12];
    } else if (type == "PLTE") plte.assign(c, c + len);
    else if (type == "tRNS") trns.assign(c, c + len);
    else if (type == "IDAT") idat.insert(idat.end(), c, c + len);
    else if (type == "IEND") break;
    p += 12 + (size_t)len;
  }
  if (W <= 0 || H <= 0 || W > 16384 || H > 16384) { err = "bad PNG size"; return false; }
  if (inter) { err = "interlaced PNG not supported (re-save without interlacing)"; return false; }
  int chans = ct == 0 ? 1 : ct == 2 ? 3 : ct == 3 ? 1 : ct == 4 ? 2 : ct == 6 ? 4 : 0;
  if (!chans || (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)) { err = "unsupported PNG type"; return false; }
  size_t bpp = max<size_t>(1, (size_t)(chans * depth) / 8), stride = ((size_t)W * chans * depth + 7) / 8;
  Bytes raw;
  if (!zlibInflate(idat, raw, (stride + 1) * (size_t)H) || raw.size() < (stride + 1) * (size_t)H) { err = "PNG data is damaged"; return false; }
  Bytes cur(stride), prev(stride, 0);
  im.w = W; im.h = H; im.rgba.assign((size_t)W * H * 4, 255);
  for (int y = 0; y < H; y++) {
    const uint8_t* row = &raw[(size_t)y * (stride + 1)];
    int ft = row[0];
    for (size_t i = 0; i < stride; i++) {
      int a = i >= bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0, x = row[1 + i], v;
      switch (ft) {
        case 1: v = x + a; break;
        case 2: v = x + b; break;
        case 3: v = x + ((a + b) >> 1); break;
        case 4: { int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c); v = x + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c); break; }
        default: v = x;
      }
      cur[i] = (uint8_t)v;
    }
    uint8_t* o = &im.rgba[(size_t)y * W * 4];
    for (int x = 0; x < W; x++) {
      int r, g, b, a = 255;
      if (depth == 8) {
        const uint8_t* s = &cur[(size_t)x * chans];
        if (ct == 0) r = g = b = s[0]; else if (ct == 2) { r = s[0]; g = s[1]; b = s[2]; }
        else if (ct == 3) { int k = s[0]; r = (size_t)k * 3 + 2 < plte.size() ? plte[(size_t)k * 3] : 0; g = (size_t)k * 3 + 2 < plte.size() ? plte[(size_t)k * 3 + 1] : 0; b = (size_t)k * 3 + 2 < plte.size() ? plte[(size_t)k * 3 + 2] : 0; if ((size_t)k < trns.size()) a = trns[(size_t)k]; }
        else if (ct == 4) { r = g = b = s[0]; a = s[1]; } else { r = s[0]; g = s[1]; b = s[2]; a = s[3]; }
      } else if (depth == 16) {
        const uint8_t* s = &cur[(size_t)x * chans * 2];
        if (ct == 0) r = g = b = s[0]; else if (ct == 2) { r = s[0]; g = s[2]; b = s[4]; }
        else if (ct == 4) { r = g = b = s[0]; a = s[2]; } else { r = s[0]; g = s[2]; b = s[4]; a = s[6]; }
      } else {   // 1, 2, 4 bit gray or palette
        int per = 8 / depth, byte = cur[(size_t)x / per], sh = 8 - depth * (x % per + 1), k = (byte >> sh) & ((1 << depth) - 1);
        if (ct == 3) { r = (size_t)k * 3 + 2 < plte.size() ? plte[(size_t)k * 3] : 0; g = (size_t)k * 3 + 2 < plte.size() ? plte[(size_t)k * 3 + 1] : 0; b = (size_t)k * 3 + 2 < plte.size() ? plte[(size_t)k * 3 + 2] : 0; if ((size_t)k < trns.size()) a = trns[(size_t)k]; }
        else r = g = b = k * 255 / ((1 << depth) - 1);
      }
      o[x * 4] = (uint8_t)r; o[x * 4 + 1] = (uint8_t)g; o[x * 4 + 2] = (uint8_t)b; o[x * 4 + 3] = (uint8_t)a;
    }
    prev.swap(cur);
  }
  return true;
}

// ---- baseline / extended-sequential JPEG (Huffman, 8-bit). Progressive JPEG is reported as unsupported.
namespace jp {
static const int ZZ[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                           35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};
struct Huff { uint8_t bits[17]; uint8_t vals[256]; int mincode[17], maxcode[17], valptr[17]; bool ok;
  Huff() : ok(false) { memset(bits, 0, sizeof bits); memset(vals, 0, sizeof vals); }
  void build() {
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) { valptr[l] = k; mincode[l] = code; code += bits[l]; k += bits[l]; maxcode[l] = bits[l] ? code - 1 : -1; code <<= 1; }
    ok = true;
  }
};
struct Comp { int id, h, v, tq, td, ta, bw, bh, pred; vector<uint8_t> px; };
struct Bits {
  const uint8_t* p; size_t n, pos; int cur, cnt;
  int bit() {
    if (cnt == 0) {
      int b = 0;
      if (pos < n) { b = p[pos]; if (b == 0xFF) { int b2 = pos + 1 < n ? p[pos + 1] : 0; if (b2 == 0) pos += 2; else b = 0; } else pos++; }
      cur = b; cnt = 8;
    }
    cnt--; return (cur >> cnt) & 1;
  }
  int get(int k) { int v = 0; while (k--) v = (v << 1) | bit(); return v; }
  int dec(const Huff& h) {
    int code = 0;
    for (int l = 1; l <= 16; l++) { code = (code << 1) | bit(); if (h.maxcode[l] >= 0 && code <= h.maxcode[l] && code >= h.mincode[l]) return h.vals[h.valptr[l] + code - h.mincode[l]]; }
    return 0;
  }
  void restart() {
    cnt = 0;
    while (pos + 1 < n && !(p[pos] == 0xFF && p[pos + 1] >= 0xD0 && p[pos + 1] <= 0xD7)) { if (p[pos] == 0xFF && p[pos + 1] != 0 && p[pos + 1] != 0xFF) return; pos++; }
    if (pos + 1 < n) pos += 2;
  }
};
static void idct(const float* in, uint8_t* out, int stride) {
  static float C[8][8]; static bool init = false;
  if (!init) { for (int u = 0; u < 8; u++) for (int x = 0; x < 8; x++) C[u][x] = (u == 0 ? 0.70710678f : 1.f) * cosf((2 * x + 1) * u * 3.14159265f / 16.f) * 0.5f; init = true; }
  float t[64];
  for (int v = 0; v < 8; v++) for (int x = 0; x < 8; x++) { float s = 0; for (int u = 0; u < 8; u++) s += C[u][x] * in[v * 8 + u]; t[v * 8 + x] = s; }
  for (int x = 0; x < 8; x++) for (int y = 0; y < 8; y++) {
    float s = 0; for (int v = 0; v < 8; v++) s += C[v][y] * t[v * 8 + x];
    int r = (int)floorf(s + 128.5f); out[y * stride + x] = (uint8_t)(r < 0 ? 0 : r > 255 ? 255 : r);
  }
}
}  // namespace jp

static bool decodeJPEG(const Bytes& d, Img& im, string& err) {
  using namespace jp;
  if (d.size() < 4 || d[0] != 0xFF || d[1] != 0xD8) { err = "not a JPEG"; return false; }
  uint16_t qt[4][64]; memset(qt, 0, sizeof qt);
  Huff dc[4], ac[4]; Comp comps[4]; int nc = 0, W = 0, H = 0, ri = 0, hmax = 1, vmax = 1;
  size_t p = 2; bool gotSOF = false;
  while (p + 4 <= d.size()) {
    if (d[p] != 0xFF) { p++; continue; }
    int m = d[p + 1]; if (m == 0xFF) { p++; continue; }
    if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0) { p += 2; continue; }
    if (m == 0xD9) break;
    size_t len = (size_t)(d[p + 2] << 8 | d[p + 3]), s = p + 4, e = p + 2 + len;
    if (e > d.size()) break;
    if (m == 0xDB) {
      while (s < e) {
        int pq = d[s] >> 4, tq = d[s] & 15; s++; if (tq > 3) return err = "bad JPEG table", false;
        for (int i = 0; i < 64 && s < e; i++) { if (pq) { qt[tq][ZZ[i]] = (uint16_t)(d[s] << 8 | d[s + 1]); s += 2; } else qt[tq][ZZ[i]] = d[s++]; }
      }
    } else if (m == 0xC4) {
      while (s + 17 <= e) {
        int tc = d[s] >> 4, th = d[s] & 15; s++; if (th > 3) return err = "bad JPEG huffman table", false;
        Huff& h = tc ? ac[th] : dc[th]; int tot = 0;
        for (int i = 1; i <= 16; i++) { h.bits[i] = d[s++]; tot += h.bits[i]; }
        if (tot > 256 || s + (size_t)tot > e) return err = "bad JPEG huffman table", false;
        for (int i = 0; i < tot; i++) h.vals[i] = d[s++];
        h.build();
      }
    } else if (m == 0xC0 || m == 0xC1) {
      if (d[s] != 8) return err = "only 8-bit JPEG supported", false;
      H = d[s + 1] << 8 | d[s + 2]; W = d[s + 3] << 8 | d[s + 4]; nc = d[s + 5];
      if (nc != 1 && nc != 3) return err = "unsupported JPEG colour mode", false;
      for (int i = 0; i < nc; i++) { comps[i].id = d[s + 6 + i * 3]; comps[i].h = d[s + 7 + i * 3] >> 4; comps[i].v = d[s + 7 + i * 3] & 15; comps[i].tq = d[s + 8 + i * 3] & 3; comps[i].pred = 0;
        if (comps[i].h < 1 || comps[i].h > 4 || comps[i].v < 1 || comps[i].v > 4) return err = "bad JPEG sampling", false;
        hmax = max(hmax, comps[i].h); vmax = max(vmax, comps[i].v); }
      gotSOF = true;
    } else if (m == 0xC2 || m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
      err = "progressive/lossless JPEG not supported (re-save as baseline)"; return false;
    } else if (m == 0xDD) ri = d[s] << 8 | d[s + 1];
    else if (m == 0xDA) {
      if (!gotSOF || W <= 0 || H <= 0 || W > 16384 || H > 16384) return err = "bad JPEG header", false;
      int ns = d[s]; if (ns != nc) return err = "multi-scan JPEG not supported", false;
      for (int i = 0; i < ns; i++) { int cid = d[s + 1 + i * 2], t = d[s + 2 + i * 2];
        for (int k = 0; k < nc; k++) if (comps[k].id == cid) { comps[k].td = t >> 4 & 3; comps[k].ta = t & 3; } }
      int mcux = (W + 8 * hmax - 1) / (8 * hmax), mcuy = (H + 8 * vmax - 1) / (8 * vmax);
      for (int k = 0; k < nc; k++) { comps[k].bw = mcux * comps[k].h * 8; comps[k].bh = mcuy * comps[k].v * 8; comps[k].px.assign((size_t)comps[k].bw * comps[k].bh, 0); }
      Bits br; br.p = &d[0]; br.n = d.size(); br.pos = e; br.cur = 0; br.cnt = 0;
      int cnt = 0;
      for (int my = 0; my < mcuy; my++) for (int mx_ = 0; mx_ < mcux; mx_++) {
        if (ri && cnt && cnt % ri == 0) { br.restart(); for (int k = 0; k < nc; k++) comps[k].pred = 0; }
        cnt++;
        for (int k = 0; k < nc; k++) {
          Comp& c = comps[k];
          if (!dc[c.td].ok || !ac[c.ta].ok) return err = "JPEG missing huffman table", false;
          for (int by = 0; by < c.v; by++) for (int bx = 0; bx < c.h; bx++) {
            float blk[64]; memset(blk, 0, sizeof blk);
            int t = br.dec(dc[c.td]); int diff = 0;
            if (t) { diff = br.get(t); if (diff < (1 << (t - 1))) diff += -(1 << t) + 1; }
            c.pred += diff; blk[0] = (float)(c.pred * qt[c.tq][0]);
            for (int kk = 1; kk < 64;) {
              int rs = br.dec(ac[c.ta]), r = rs >> 4, sz = rs & 15;
              if (sz == 0) { if (r == 15) { kk += 16; continue; } break; }
              kk += r; if (kk > 63) break;
              int v = br.get(sz); if (v < (1 << (sz - 1))) v += -(1 << sz) + 1;
              blk[ZZ[kk]] = (float)(v * qt[c.tq][ZZ[kk]]); kk++;
            }
            int ox = (mx_ * c.h + bx) * 8, oy = (my * c.v + by) * 8;
            idct(blk, &c.px[(size_t)oy * c.bw + ox], c.bw);
          }
        }
      }
      im.w = W; im.h = H; im.rgba.assign((size_t)W * H * 4, 255);
      for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float s[3] = {0, 0, 0};
        for (int k = 0; k < nc; k++) { const Comp& c = comps[k]; s[k] = c.px[(size_t)(y * c.v / vmax) * c.bw + (x * c.h / hmax)]; }
        float r, g, b;
        if (nc == 1) r = g = b = s[0];
        else { r = s[0] + 1.402f * (s[2] - 128.f); g = s[0] - 0.344136f * (s[1] - 128.f) - 0.714136f * (s[2] - 128.f); b = s[0] + 1.772f * (s[1] - 128.f); }
        uint8_t* o = &im.rgba[((size_t)y * W + x) * 4];
        o[0] = (uint8_t)max(0.f, min(255.f, r + 0.5f)); o[1] = (uint8_t)max(0.f, min(255.f, g + 0.5f)); o[2] = (uint8_t)max(0.f, min(255.f, b + 0.5f));
      }
      return true;
    }
    p = e;
  }
  err = "JPEG has no image data"; return false;
}

static bool decodeImage(const Bytes& d, Img& im, string& err) {
  if (d.size() > 8 && d[0] == 0x89 && d[1] == 'P') return decodePNG(d, im, err);
  if (d.size() > 3 && d[0] == 0xFF && d[1] == 0xD8) return decodeJPEG(d, im, err);
  err = "unknown image format (PNG and baseline JPEG only)"; return false;
}
static void shrinkImg(Img& im, int maxSide) {   // box-filter by an integer factor so big textures stay light
  int f = 1; while (max(im.w, im.h) / f > maxSide) f++;
  if (f == 1) return;
  Img o; o.w = im.w / f; o.h = im.h / f; o.rgba.assign((size_t)o.w * o.h * 4, 255);
  for (int y = 0; y < o.h; y++) for (int x = 0; x < o.w; x++) for (int c = 0; c < 4; c++) {
    int s = 0; for (int j = 0; j < f; j++) for (int i = 0; i < f; i++) s += im.rgba[((size_t)(y * f + j) * im.w + x * f + i) * 4 + c];
    o.rgba[((size_t)y * o.w + x) * 4 + c] = (uint8_t)(s / (f * f));
  }
  im.w = o.w; im.h = o.h; im.rgba.swap(o.rgba);
}

// ============================================================ Wavefront OBJ (+ MTL, textures)
typedef function<bool(const string&, Bytes&)> Provider;   // relative file name -> bytes

static bool loadObj(const Bytes& data, const Provider& prov, Mesh3& out, string& note, string& err) {
  struct Mat { float kd[3]; string tex; bool texOk; Mat() : texOk(false) { kd[0] = kd[1] = kd[2] = 0.86f; } };
  vector<float> V, T, N;
  vector<Mat> mats; map<string, int> matIdx; int curMat = -1;
  string chosenTex; int texCount = 0, texSkipped = 0; string texErr;
  map<array<int, 4>, uint32_t> vmap;
  vector<float> P, NR, UV, FN; vector<uint8_t> COL; vector<uint32_t> idx;
  vector<char> hasN;
  size_t i = 0, n = data.size(); int faces = 0;
  while (i < n) {
    size_t e = i; while (e < n && data[e] != '\n') e++;
    string ln((const char*)&data[i], e - i); i = e + 1;
    size_t k = 0; while (k < ln.size() && isspace((unsigned char)ln[k])) k++;
    if (k >= ln.size() || ln[k] == '#') continue;
    size_t ke = k; while (ke < ln.size() && !isspace((unsigned char)ln[ke])) ke++;
    string kw = ln.substr(k, ke - k); const char* rest = ln.c_str() + ke;
    if (kw == "v") { float a = 0, b = 0, c = 0; if (sscanf(rest, "%f %f %f", &a, &b, &c) >= 2) { V.push_back(a); V.push_back(b); V.push_back(c); } }
    else if (kw == "vt") { float a = 0, b = 0; sscanf(rest, "%f %f", &a, &b); T.push_back(a); T.push_back(b); }
    else if (kw == "vn") { float a = 0, b = 0, c = 0; sscanf(rest, "%f %f %f", &a, &b, &c); N.push_back(a); N.push_back(b); N.push_back(c); }
    else if (kw == "mtllib") {
      Bytes mb; string name = trimStr(rest);
      if (!name.empty() && prov(name, mb)) {
        string cur; int mi = -1; size_t j = 0;
        while (j < mb.size()) {
          size_t je = j; while (je < mb.size() && mb[je] != '\n') je++;
          string l2 = trimStr(string((const char*)&mb[j], je - j)); j = je + 1;
          if (l2.empty() || l2[0] == '#') continue;
          size_t sp = 0; while (sp < l2.size() && !isspace((unsigned char)l2[sp])) sp++;
          string w = lowerStr(l2.substr(0, sp)), arg = trimStr(l2.substr(sp));
          if (w == "newmtl") { mats.push_back(Mat()); mi = (int)mats.size() - 1; matIdx[arg] = mi; }
          else if (mi >= 0 && w == "kd") { float a, b, c; if (sscanf(arg.c_str(), "%f %f %f", &a, &b, &c) == 3) { mats[(size_t)mi].kd[0] = a; mats[(size_t)mi].kd[1] = b; mats[(size_t)mi].kd[2] = c; } }
          else if (mi >= 0 && w == "map_kd") {
            // drop "-option value" pairs, keep the file name (may contain spaces)
            string a2 = arg; while (!a2.empty() && a2[0] == '-') { size_t s1 = a2.find(' '); if (s1 == string::npos) { a2.clear(); break; } a2 = trimStr(a2.substr(s1)); size_t s2 = a2.find(' '); if (s2 == string::npos) { a2.clear(); break; } a2 = trimStr(a2.substr(s2)); }
            mats[(size_t)mi].tex = slashFix(a2);
          }
        }
        for (size_t m = 0; m < mats.size(); m++) {
          if (mats[m].tex.empty() || mats[m].texOk) continue;
          if (chosenTex.empty()) {
            Bytes ib; Img im; string ie;
            if (prov(mats[m].tex, ib) && decodeImage(ib, im, ie)) {
              shrinkImg(im, 2048); out.tex.swap(im.rgba); out.tw = im.w; out.th = im.h; chosenTex = mats[m].tex; texCount++;
            } else { texErr = ie.empty() ? "texture file not found: " + mats[m].tex : mats[m].tex + ": " + ie; texSkipped++; continue; }
          }
          if (mats[m].tex == chosenTex) mats[m].texOk = true; else texSkipped++;
        }
      }
    }
    else if (kw == "usemtl") { map<string, int>::iterator it = matIdx.find(trimStr(rest)); curMat = it == matIdx.end() ? -1 : it->second; }
    else if (kw == "f") {
      vector<uint32_t> ids;
      const char* s = rest;
      while (*s) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (!*s) break;
        char* ep; long a = strtol(s, &ep, 10), t = 0, nn = 0; bool ht = false, hn = false;
        if (ep == s) break;
        s = ep;
        if (*s == '/') { s++; if (*s != '/') { t = strtol(s, &ep, 10); ht = ep != s; s = ep; } if (*s == '/') { s++; nn = strtol(s, &ep, 10); hn = ep != s; s = ep; } }
        long vc = (long)(V.size() / 3);
        long vi = a > 0 ? a - 1 : vc + a, ti = ht ? (t > 0 ? t - 1 : (long)(T.size() / 2) + t) : -1, ni = hn ? (nn > 0 ? nn - 1 : (long)(N.size() / 3) + nn) : -1;
        if (vi < 0 || vi >= vc) continue;
        if (ti >= (long)(T.size() / 2)) ti = -1;
        if (ni >= (long)(N.size() / 3)) ni = -1;
        bool useTex = ti >= 0 && (chosenTex.empty() || (curMat >= 0 && mats[(size_t)curMat].texOk));   // keep UVs when no texture was found yet (a loose picture may be added later)
        array<int, 4> key = {{(int)vi, useTex ? (int)ti : -1, (int)ni, curMat}};
        map<array<int, 4>, uint32_t>::iterator it = vmap.find(key);
        if (it != vmap.end()) { ids.push_back(it->second); continue; }
        uint32_t nid = (uint32_t)(P.size() / 3); vmap[key] = nid;
        P.push_back(V[(size_t)vi * 3]); P.push_back(V[(size_t)vi * 3 + 1]); P.push_back(V[(size_t)vi * 3 + 2]);
        if (ni >= 0) { NR.push_back(N[(size_t)ni * 3]); NR.push_back(N[(size_t)ni * 3 + 1]); NR.push_back(N[(size_t)ni * 3 + 2]); hasN.push_back(1); }
        else { NR.push_back(0); NR.push_back(0); NR.push_back(0); hasN.push_back(0); }
        if (useTex) { UV.push_back(T[(size_t)ti * 2]); UV.push_back(1.f - T[(size_t)ti * 2 + 1]); } else { UV.push_back(1e9f); UV.push_back(1e9f); }
        const float* kd = curMat >= 0 ? mats[(size_t)curMat].kd : nullptr;
        for (int c = 0; c < 3; c++) COL.push_back((uint8_t)(kd ? max(0.f, min(1.f, kd[c])) * 255.f + 0.5f : 220.f));
        ids.push_back(nid);
      }
      for (size_t q = 1; q + 1 < ids.size(); q++) { idx.push_back(ids[0]); idx.push_back(ids[q]); idx.push_back(ids[q + 1]); faces++; }
    }
  }
  if (idx.empty()) { err = V.empty() ? "OBJ has no vertices" : "OBJ has no faces"; return false; }
  size_t nv = P.size() / 3;
  vector<float> acc(nv * 3, 0.f);   // smooth normals, used where the file gives none
  for (size_t t = 0; t + 2 < idx.size(); t += 3) {
    const float* a = &P[idx[t] * 3]; const float* b = &P[idx[t + 1] * 3]; const float* c = &P[idx[t + 2] * 3];
    float ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2], vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    float nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    for (int q = 0; q < 3; q++) { acc[idx[t + (size_t)q] * 3] += nx; acc[idx[t + (size_t)q] * 3 + 1] += ny; acc[idx[t + (size_t)q] * 3 + 2] += nz; }
  }
  for (size_t v = 0; v < nv; v++) {
    float* q = hasN[v] ? &NR[v * 3] : &acc[v * 3];
    float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]); if (l < 1e-12f) { q[0] = 0; q[1] = 1; q[2] = 0; l = 1; }
    NR[v * 3] = q[0] / l; NR[v * 3 + 1] = q[1] / l; NR[v * 3 + 2] = q[2] / l;
  }
  out.pos.swap(P); out.nrm.swap(NR); out.idx.swap(idx); out.col.swap(COL);
  out.uv.swap(UV);
  calcBounds(out);
  note = to_string(faces) + " TRIS";
  if (!out.tex.empty()) note += ", TEXTURE " + to_string(out.tw) + "X" + to_string(out.th);
  if (texSkipped) note += " (" + to_string(texSkipped) + " TEXTURE(S) SKIPPED" + (texErr.empty() ? string() : ": " + texErr) + ")";
  return true;
}

// ============================================================ glTF binary (.glb): geometry + PBR materials + embedded textures
// Static meshes (node hierarchy baked into world space). Skins / animations / Draco / Meshopt are not read.
namespace gj {
struct V {
  enum T { NUL, BOOL, NUM, STR, ARR, OBJ } t; double n; string s; vector<V> a; map<string, V> o;
  V() : t(NUL), n(0) {}
  const V& k(const char* key) const { static V nul; map<string, V>::const_iterator it = o.find(key); return it == o.end() ? nul : it->second; }
  const V& i(long idx) const { static V nul; return idx >= 0 && (size_t)idx < a.size() ? a[(size_t)idx] : nul; }
  bool has(const char* key) const { return o.find(key) != o.end(); }
  double num(double d = 0) const { return t == NUM ? n : d; }
  int in(int d = 0) const { return t == NUM ? (int)n : d; }
};
struct Parser {
  const char* p; const char* e; bool ok; int depth;
  void ws() { while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++; }
  bool lit(const char* w) { size_t l = strlen(w); if ((size_t)(e - p) >= l && strncmp(p, w, l) == 0) { p += l; return true; } return false; }
  string str() {
    string r; p++;
    while (p < e && *p != '"') {
      if (*p == '\\' && p + 1 < e) {
        p++;
        switch (*p) {
          case 'n': r += '\n'; break; case 't': r += '\t'; break; case 'r': r += '\r'; break; case 'b': case 'f': break;
          case 'u': { unsigned cp = 0; for (int q = 0; q < 4 && p + 1 < e; q++) { p++; char c = *p; cp = cp * 16 + (unsigned)(c >= '0' && c <= '9' ? c - '0' : ((c | 32) - 'a' + 10) & 15); }
            if (cp < 0x80) r += (char)cp; else if (cp < 0x800) { r += (char)(0xC0 | (cp >> 6)); r += (char)(0x80 | (cp & 63)); } else { r += (char)(0xE0 | (cp >> 12)); r += (char)(0x80 | ((cp >> 6) & 63)); r += (char)(0x80 | (cp & 63)); } break; }
          default: r += *p;
        }
        p++;
      } else r += *p++;
    }
    if (p < e) p++;
    return r;
  }
  void val(V& v) {
    ws(); if (p >= e || depth > 64) { ok = false; return; }
    char c = *p;
    if (c == '{' || c == '[') {
      bool obj = c == '{'; v.t = obj ? V::OBJ : V::ARR; p++; depth++; ws();
      if (p < e && *p == (obj ? '}' : ']')) { p++; depth--; return; }
      while (ok && p < e) {
        if (obj) { ws(); if (p >= e || *p != '"') { ok = false; break; } string key = str(); ws(); if (p >= e || *p != ':') { ok = false; break; } p++; val(v.o[key]); }
        else { v.a.push_back(V()); val(v.a.back()); }
        ws(); if (p < e && *p == ',') { p++; continue; } if (p < e && *p == (obj ? '}' : ']')) { p++; break; } ok = false;
      }
      depth--;
    }
    else if (c == '"') { v.t = V::STR; v.s = str(); }
    else if (lit("true")) { v.t = V::BOOL; v.n = 1; }
    else if (lit("false")) { v.t = V::BOOL; v.n = 0; }
    else if (lit("null")) {}
    else { char* en = nullptr; v.n = strtod(p, &en); if (en == p) { ok = false; return; } v.t = V::NUM; p = en; }
  }
};
}  // namespace gj

static bool b64Decode(const string& s, Bytes& out) {
  int val = 0, bits = -8;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i]; int d;
    if (c >= 'A' && c <= 'Z') d = c - 'A'; else if (c >= 'a' && c <= 'z') d = c - 'a' + 26; else if (c >= '0' && c <= '9') d = c - '0' + 52;
    else if (c == '+' || c == '-') d = 62; else if (c == '/' || c == '_') d = 63; else if (c == '=') break; else continue;
    val = (val << 6) | d; bits += 6; if (bits >= 0) { out.push_back((uint8_t)((val >> bits) & 0xFF)); bits -= 8; }
  }
  return !out.empty();
}

struct GAcc { const uint8_t* base; size_t stride, count; int comp, ncomp; bool norm; GAcc() : base(nullptr), stride(0), count(0), comp(0), ncomp(0), norm(false) {} };
static int gCompSize(int c) { return c == 5120 || c == 5121 ? 1 : c == 5122 || c == 5123 ? 2 : 4; }
static float gAccF(const GAcc& a, size_t i, int k) {
  const uint8_t* q = a.base + i * a.stride + (size_t)k * gCompSize(a.comp);
  switch (a.comp) {
    case 5126: { float f; memcpy(&f, q, 4); return f; }
    case 5123: { uint16_t v; memcpy(&v, q, 2); return a.norm ? v / 65535.f : (float)v; }
    case 5121: return a.norm ? q[0] / 255.f : (float)q[0];
    case 5122: { int16_t v; memcpy(&v, q, 2); return a.norm ? max(v / 32767.f, -1.f) : (float)v; }
    case 5120: { int8_t v = (int8_t)q[0]; return a.norm ? max(v / 127.f, -1.f) : (float)v; }
    case 5125: { uint32_t v; memcpy(&v, q, 4); return (float)v; }
  }
  return 0.f;
}
static uint32_t gAccU(const GAcc& a, size_t i) {
  const uint8_t* q = a.base + i * a.stride;
  if (a.comp == 5121) return q[0];
  if (a.comp == 5123) { uint16_t v; memcpy(&v, q, 2); return v; }
  uint32_t v; memcpy(&v, q, 4); return v;
}
static void gMul4(const float* A, const float* B, float* o) {   // column-major o = A * B
  for (int c = 0; c < 4; c++) for (int r = 0; r < 4; r++) { float s = 0; for (int k = 0; k < 4; k++) s += A[k * 4 + r] * B[c * 4 + k]; o[c * 4 + r] = s; }
}
static void gNodeMat(const gj::V& nd, float* m) {
  const gj::V& mx_ = nd.k("matrix");
  if (mx_.a.size() == 16) { for (int i = 0; i < 16; i++) m[i] = (float)mx_.a[i].num(); return; }
  float t[3] = {0, 0, 0}, s[3] = {1, 1, 1}, q[4] = {0, 0, 0, 1};
  const gj::V& T = nd.k("translation"); if (T.a.size() == 3) for (int i = 0; i < 3; i++) t[i] = (float)T.a[i].num();
  const gj::V& S = nd.k("scale"); if (S.a.size() == 3) for (int i = 0; i < 3; i++) s[i] = (float)S.a[i].num(1);
  const gj::V& Q = nd.k("rotation"); if (Q.a.size() == 4) for (int i = 0; i < 4; i++) q[i] = (float)Q.a[i].num();
  float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); if (l > 1e-8f) for (int i = 0; i < 4; i++) q[i] /= l;
  float x = q[0], y = q[1], z = q[2], w = q[3];
  float r[9] = {1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w), 2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w), 2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)};
  for (int c = 0; c < 3; c++) { for (int k = 0; k < 3; k++) m[c * 4 + k] = r[c * 3 + k] * s[c]; m[c * 4 + 3] = 0; }
  m[12] = t[0]; m[13] = t[1]; m[14] = t[2]; m[15] = 1;
}

static bool loadGlb(const Bytes& d, Mesh3& out, string& note, string& err) {
  if (d.size() < 20 || memcmp(d.data(), "glTF", 4) != 0) { err = "NOT A GLB FILE (missing glTF header)"; return false; }
  if (le32(d, 4) != 2) { err = "ONLY glTF 2.0 .glb FILES ARE SUPPORTED"; return false; }
  string js; Bytes bin; bool haveBin = false; size_t off = 12;
  while (off + 8 <= d.size()) {
    size_t cl = le32(d, off), ct = le32(d, off + 4); off += 8; if (off + cl > d.size()) cl = d.size() - off;
    if (ct == 0x4E4F534Au && js.empty()) js.assign((const char*)&d[off], cl);
    else if (ct == 0x004E4942u && !haveBin) { bin.assign(d.begin() + off, d.begin() + off + cl); haveBin = true; }
    off += (cl + 3) & ~(size_t)3;
  }
  if (js.empty()) { err = "GLB HAS NO JSON CHUNK"; return false; }
  gj::Parser ps = {js.c_str(), js.c_str() + js.size(), true, 0}; gj::V root; ps.val(root);
  if (!ps.ok || root.t != gj::V::OBJ) { err = "GLB JSON IS DAMAGED"; return false; }
  { const gj::V& rq = root.k("extensionsRequired");
    for (size_t i = 0; i < rq.a.size(); i++) if (rq.a[i].s == "KHR_draco_mesh_compression" || rq.a[i].s == "EXT_meshopt_compression") { err = "COMPRESSED GLB (" + rq.a[i].s + ") IS NOT SUPPORTED - EXPORT IT WITHOUT COMPRESSION"; return false; } }

  auto getAcc = [&](int ai, GAcc& a) -> bool {
    const gj::V& ac = root.k("accessors").i(ai); if (ac.t != gj::V::OBJ || !ac.has("bufferView")) return false;
    const gj::V& bv = root.k("bufferViews").i(ac.k("bufferView").in(-1)); if (bv.t != gj::V::OBJ || bv.k("buffer").in(0) != 0) return false;
    const string& ty = ac.k("type").s; a.ncomp = ty == "SCALAR" ? 1 : ty == "VEC2" ? 2 : ty == "VEC3" ? 3 : ty == "VEC4" ? 4 : 0; if (!a.ncomp) return false;
    a.comp = ac.k("componentType").in(); if (a.comp < 5120 || a.comp > 5126) return false;
    a.norm = ac.k("normalized").num() != 0; a.count = (size_t)ac.k("count").num();
    size_t cs = (size_t)gCompSize(a.comp), el = cs * a.ncomp; a.stride = (size_t)bv.k("byteStride").num(0); if (!a.stride) a.stride = el;
    size_t o2 = (size_t)bv.k("byteOffset").num(0) + (size_t)ac.k("byteOffset").num(0);
    if (a.count && o2 + (a.count - 1) * a.stride + el > bin.size()) return false;
    a.base = bin.data() + o2; return true;
  };
  map<int, Img> imgs; set<int> imgBad; string texNote;
  auto getImage = [&](int ii) -> const Img* {
    if (ii < 0) return nullptr;
    map<int, Img>::iterator f = imgs.find(ii); if (f != imgs.end()) return &f->second; if (imgBad.count(ii)) return nullptr;
    const gj::V& im = root.k("images").i(ii); Bytes raw;
    if (im.has("bufferView")) { const gj::V& bv = root.k("bufferViews").i(im.k("bufferView").in(-1)); size_t o2 = (size_t)bv.k("byteOffset").num(0), l = (size_t)bv.k("byteLength").num(0); if (o2 + l <= bin.size() && l) raw.assign(bin.begin() + o2, bin.begin() + o2 + l); }
    else if (im.k("uri").s.compare(0, 5, "data:") == 0) { size_t c = im.k("uri").s.find("base64,"); if (c != string::npos) b64Decode(im.k("uri").s.substr(c + 7), raw); }
    Img x; string e2; if (raw.empty() || !decodeImage(raw, x, e2)) { imgBad.insert(ii); texNote = raw.empty() ? "EXTERNAL/EMPTY TEXTURE" : e2; return nullptr; }
    shrinkImg(x, 2048); imgs[ii] = x; return &imgs[ii];
  };
  auto texImage = [&](int ti) -> int { return ti < 0 ? -1 : root.k("textures").i(ti).k("source").in(-1); };

  struct GM { float col[4], metal, rough; int baseImg, mrImg; };
  auto getMat = [&](int mi) -> GM {
    GM g; g.col[0] = g.col[1] = g.col[2] = g.col[3] = 1.f; g.metal = 0.f; g.rough = 0.6f; g.baseImg = g.mrImg = -1;
    const gj::V& m = root.k("materials").i(mi);
    if (m.t == gj::V::OBJ && m.has("pbrMetallicRoughness")) {
      const gj::V& pb = m.k("pbrMetallicRoughness"); g.metal = (float)pb.k("metallicFactor").num(1.0); g.rough = (float)pb.k("roughnessFactor").num(1.0);
      const gj::V& bc = pb.k("baseColorFactor"); if (bc.a.size() == 4) for (int i = 0; i < 4; i++) g.col[i] = (float)bc.a[i].num(1.0);
      g.baseImg = texImage(pb.k("baseColorTexture").k("index").in(-1)); g.mrImg = texImage(pb.k("metallicRoughnessTexture").k("index").in(-1));
    }
    return g;
  };

  // ---- node hierarchy -> list of (mesh, world matrix)
  struct Draw { int mesh; float M[16]; };
  vector<Draw> draws; const float I4[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
  function<void(int, const float*, int)> walk = [&](int ni, const float* Pm, int depth) {
    if (depth > 64) return; const gj::V& nd = root.k("nodes").i(ni); if (nd.t != gj::V::OBJ) return;
    float L[16], M[16]; gNodeMat(nd, L); gMul4(Pm, L, M);
    if (nd.has("mesh")) { Draw dr; dr.mesh = nd.k("mesh").in(); memcpy(dr.M, M, sizeof M); draws.push_back(dr); }
    const gj::V& ch = nd.k("children"); for (size_t c = 0; c < ch.a.size(); c++) walk(ch.a[c].in(-1), M, depth + 1);
  };
  const gj::V& nodes = root.k("nodes");
  if (nodes.a.empty()) { for (size_t m = 0; m < root.k("meshes").a.size(); m++) { Draw dr; dr.mesh = (int)m; memcpy(dr.M, I4, sizeof I4); draws.push_back(dr); } }
  else {
    const gj::V& sc = root.k("scenes").i(root.k("scene").in(0)); vector<int> roots;
    for (size_t i = 0; i < sc.k("nodes").a.size(); i++) roots.push_back(sc.k("nodes").a[i].in(-1));
    if (roots.empty()) { vector<char> isChild(nodes.a.size(), 0); for (size_t i = 0; i < nodes.a.size(); i++) for (size_t c = 0; c < nodes.a[i].k("children").a.size(); c++) { int ci = nodes.a[i].k("children").a[c].in(-1); if (ci >= 0 && (size_t)ci < isChild.size()) isChild[ci] = 1; } for (size_t i = 0; i < nodes.a.size(); i++) if (!isChild[i]) roots.push_back((int)i); }
    for (size_t i = 0; i < roots.size(); i++) walk(roots[i], I4, 0);
  }

  // ---- pass 1: which textures are used, average metal / roughness
  struct PrimRef { const gj::V* prim; int draw; GM mat; };
  vector<PrimRef> prims; vector<int> texOrder; map<int, int> texTile; size_t totalTris = 0;
  for (size_t di = 0; di < draws.size(); di++) {
    const gj::V& mesh = root.k("meshes").i(draws[di].mesh);
    for (size_t pi = 0; pi < mesh.k("primitives").a.size(); pi++) {
      const gj::V& pr = mesh.k("primitives").a[pi]; int mode = pr.k("mode").in(4); if (mode < 4 || mode > 6 || !pr.k("attributes").has("POSITION")) continue;
      PrimRef r; r.prim = &pr; r.draw = (int)di; r.mat = getMat(pr.k("material").in(-1)); prims.push_back(r);
      if (r.mat.baseImg >= 0 && !texTile.count(r.mat.baseImg) && getImage(r.mat.baseImg)) { texTile[r.mat.baseImg] = (int)texOrder.size(); texOrder.push_back(r.mat.baseImg); }
    }
  }
  if (prims.empty()) { err = "GLB HAS NO TRIANGLE MESHES"; return false; }
  int nTex = (int)texOrder.size(), cols = 1, rows = 1, T = 0; Mesh3 res;
  if (nTex > 1) {   // several textures: pack them into one atlas (a Mesh3 carries a single texture)
    cols = (int)ceilf(sqrtf((float)nTex)); rows = (nTex + cols - 1) / cols; T = min(1024, 2048 / cols);
    res.tw = cols * T; res.th = rows * T; res.tex.assign((size_t)res.tw * res.th * 4, 255);
    for (int t = 0; t < nTex; t++) { const Img* im = getImage(texOrder[t]); int tx0 = (t % cols) * T, ty0 = (t / cols) * T;
      for (int y = 0; y < T; y++) for (int x = 0; x < T; x++) { const uint8_t* s = &im->rgba[((size_t)(y * im->h / T) * im->w + (x * im->w / T)) * 4]; memcpy(&res.tex[((size_t)(ty0 + y) * res.tw + tx0 + x) * 4], s, 4); } }
  } else if (nTex == 1) { const Img* im = getImage(texOrder[0]); res.tex = im->rgba; res.tw = im->w; res.th = im->h; }
  auto avgCol = [&](int ii, float* c) { const Img* im = getImage(ii); c[0] = c[1] = c[2] = 1.f; if (!im) return; double s[3] = {0, 0, 0}; int n = 0; int sx = max(1, im->w / 48), sy = max(1, im->h / 48);
    for (int y = 0; y < im->h; y += sy) for (int x = 0; x < im->w; x += sx) { const uint8_t* q = &im->rgba[((size_t)y * im->w + x) * 4]; for (int k = 0; k < 3; k++) s[k] += q[k]; n++; }
    for (int k = 0; k < 3; k++) c[k] = n ? (float)(s[k] / n) / 255.f : 1.f; };

  // ---- pass 2: build the merged mesh
  vector<float> P, N, UV; vector<uint8_t> COL; vector<uint32_t> IDX; bool anyTexPrim = false; double wMetal = 0, wRough = 0, wSum = 0;
  for (size_t ri = 0; ri < prims.size(); ri++) {
    const gj::V& pr = *prims[ri].prim; const Draw& dr = draws[prims[ri].draw]; const GM& gm = prims[ri].mat;
    GAcc ap, an, au, ac, ai; if (!getAcc(pr.k("attributes").k("POSITION").in(-1), ap) || ap.ncomp < 3 || ap.count == 0) continue;
    bool hasN = pr.k("attributes").has("NORMAL") && getAcc(pr.k("attributes").k("NORMAL").in(-1), an) && an.ncomp >= 3 && an.count >= ap.count;
    bool hasUV = pr.k("attributes").has("TEXCOORD_0") && getAcc(pr.k("attributes").k("TEXCOORD_0").in(-1), au) && au.ncomp >= 2 && au.count >= ap.count;
    bool hasC = pr.k("attributes").has("COLOR_0") && getAcc(pr.k("attributes").k("COLOR_0").in(-1), ac) && ac.ncomp >= 3 && ac.count >= ap.count;
    bool hasI = pr.has("indices") && getAcc(pr.k("indices").in(-1), ai) && ai.ncomp == 1 && ai.comp != 5126;
    size_t nv = ap.count, base = P.size() / 3; if (base + nv > 6000000) { err = "GLB IS TOO LARGE FOR THIS DEVICE (over 6M vertices)"; return false; }
    // texture tile for this primitive (atlas needs UVs inside 0..1; tiled materials fall back to their average colour)
    int tile = -1; if (gm.baseImg >= 0 && hasUV && texTile.count(gm.baseImg)) tile = texTile[gm.baseImg];
    if (tile >= 0 && nTex > 1) { for (size_t v = 0; v < nv; v++) { float u = gAccF(au, v, 0), w = gAccF(au, v, 1); if (u < -0.002f || u > 1.002f || w < -0.002f || w > 1.002f) { tile = -1; break; } } }
    float avg[3]; avgCol(gm.baseImg, avg); bool useAvg = gm.baseImg >= 0 && tile < 0 && getImage(gm.baseImg);
    float m3[9] = {dr.M[0], dr.M[1], dr.M[2], dr.M[4], dr.M[5], dr.M[6], dr.M[8], dr.M[9], dr.M[10]};   // normal matrix = cofactor of the 3x3
    float cf[9] = {m3[4] * m3[8] - m3[7] * m3[5], m3[5] * m3[6] - m3[8] * m3[3], m3[3] * m3[7] - m3[6] * m3[4], m3[7] * m3[2] - m3[1] * m3[8], m3[8] * m3[0] - m3[2] * m3[6], m3[6] * m3[1] - m3[0] * m3[7], m3[1] * m3[5] - m3[4] * m3[2], m3[2] * m3[3] - m3[5] * m3[0], m3[0] * m3[4] - m3[3] * m3[1]};
    float det = m3[0] * cf[0] + m3[3] * cf[3] + m3[6] * cf[6]; bool flip = det < 0;
    for (size_t v = 0; v < nv; v++) {
      float x = gAccF(ap, v, 0), y = gAccF(ap, v, 1), z = gAccF(ap, v, 2);
      P.push_back(dr.M[0] * x + dr.M[4] * y + dr.M[8] * z + dr.M[12]); P.push_back(dr.M[1] * x + dr.M[5] * y + dr.M[9] * z + dr.M[13]); P.push_back(dr.M[2] * x + dr.M[6] * y + dr.M[10] * z + dr.M[14]);
      if (hasN) { float a = gAccF(an, v, 0), b = gAccF(an, v, 1), c = gAccF(an, v, 2), nx = cf[0] * a + cf[3] * b + cf[6] * c, ny = cf[1] * a + cf[4] * b + cf[7] * c, nz = cf[2] * a + cf[5] * b + cf[8] * c, l = sqrtf(nx * nx + ny * ny + nz * nz);
        if (l > 1e-12f) { nx /= l; ny /= l; nz /= l; } N.push_back(nx); N.push_back(ny); N.push_back(nz); } else { N.push_back(0); N.push_back(0); N.push_back(0); }
      if (tile >= 0) { float u = gAccF(au, v, 0), w = gAccF(au, v, 1);
        if (nTex > 1) { float e = 0.5f / T; u = ((tile % cols) + e + u * (1 - 2 * e)) / cols; w = ((tile / cols) + e + w * (1 - 2 * e)) / rows; }
        UV.push_back(u); UV.push_back(w); } else { UV.push_back(1e9f); UV.push_back(1e9f); }
      float cc[3] = {gm.col[0], gm.col[1], gm.col[2]}; if (useAvg) for (int k = 0; k < 3; k++) cc[k] *= avg[k] * avg[k];   // average texture colour is sRGB, factors are linear
      if (hasC) for (int k = 0; k < 3; k++) cc[k] *= gAccF(ac, v, k);
      for (int k = 0; k < 3; k++) COL.push_back((uint8_t)(sqrtf(min(1.f, max(0.f, cc[k]))) * 255.f + 0.5f));   // linear -> display (gamma 2)
    }
    size_t ni = hasI ? ai.count : nv; int mode = pr.k("mode").in(4); size_t triStart = IDX.size();
    auto tri = [&](uint32_t a, uint32_t b, uint32_t c) { if (a >= nv || b >= nv || c >= nv || a == b || b == c || a == c) return; IDX.push_back((uint32_t)base + a); IDX.push_back((uint32_t)base + (flip ? c : b)); IDX.push_back((uint32_t)base + (flip ? b : c)); };
    auto at = [&](size_t k) -> uint32_t { return hasI ? gAccU(ai, k) : (uint32_t)k; };
    if (mode == 4) for (size_t k = 0; k + 2 < ni; k += 3) tri(at(k), at(k + 1), at(k + 2));
    else if (mode == 5) for (size_t k = 0; k + 2 < ni; k++) { if (k & 1) tri(at(k + 1), at(k), at(k + 2)); else tri(at(k), at(k + 1), at(k + 2)); }
    else for (size_t k = 1; k + 1 < ni; k++) tri(at(0), at(k), at(k + 1));
    size_t nt = (IDX.size() - triStart) / 3; totalTris += nt; if (tile >= 0) anyTexPrim = true;
    float mt = gm.metal, rg = gm.rough;
    if (gm.mrImg >= 0 && getImage(gm.mrImg)) { float a2[3]; avgCol(gm.mrImg, a2); rg *= a2[1]; mt *= a2[2]; }   // glTF: G = roughness, B = metalness
    wMetal += mt * nt; wRough += rg * nt; wSum += nt;
    if (totalTris > 4000000) { err = "GLB HAS TOO MANY TRIANGLES (over 4M)"; return false; }
  }
  if (IDX.empty()) { err = "GLB HAS NO READABLE TRIANGLES (Draco / sparse data?)"; return false; }
  // normals the file did not carry: smooth per vertex
  { size_t nv = P.size() / 3; vector<char> need(nv, 0); bool any = false; for (size_t v = 0; v < nv; v++) if (N[v * 3] == 0 && N[v * 3 + 1] == 0 && N[v * 3 + 2] == 0) { need[v] = 1; any = true; }
    if (any) { for (size_t t = 0; t + 2 < IDX.size(); t += 3) { uint32_t a = IDX[t], b = IDX[t + 1], c = IDX[t + 2];
        float e1[3], e2[3], fn[3]; for (int k = 0; k < 3; k++) { e1[k] = P[b * 3 + k] - P[a * 3 + k]; e2[k] = P[c * 3 + k] - P[a * 3 + k]; }
        fn[0] = e1[1] * e2[2] - e1[2] * e2[1]; fn[1] = e1[2] * e2[0] - e1[0] * e2[2]; fn[2] = e1[0] * e2[1] - e1[1] * e2[0];
        uint32_t ids[3] = {a, b, c}; for (int q = 0; q < 3; q++) if (need[ids[q]]) for (int k = 0; k < 3; k++) N[ids[q] * 3 + k] += fn[k]; }
      for (size_t v = 0; v < nv; v++) if (need[v]) { float l = sqrtf(N[v * 3] * N[v * 3] + N[v * 3 + 1] * N[v * 3 + 1] + N[v * 3 + 2] * N[v * 3 + 2]); if (l > 1e-12f) for (int k = 0; k < 3; k++) N[v * 3 + k] /= l; else N[v * 3 + 1] = 1; } } }
  res.pos.swap(P); res.nrm.swap(N); res.idx.swap(IDX); res.col.swap(COL);
  if (anyTexPrim) res.uv.swap(UV); else { res.uv.clear(); res.tex.clear(); res.tw = res.th = 0; }
  res.metal = wSum > 0 ? (float)(wMetal / wSum) : 0.f; res.rough = wSum > 0 ? (float)(wRough / wSum) : 0.6f; res.pbr = true;
  calcBounds(res); out.take(res);
  note = to_string(totalTris) + " TRIS (GLB), " + (nTex > 1 ? to_string(nTex) + " TEXTURES PACKED" : nTex == 1 ? string("1 TEXTURE ") + to_string(out.tw) + "X" + to_string(out.th) : string("NO TEXTURE")) + ", METAL " + to_string((int)lroundf(out.metal * 100)) + "% ROUGH " + to_string((int)lroundf(out.rough * 100)) + "%";
  if (!texNote.empty()) note += " (SOME TEXTURES SKIPPED: " + texNote + ")";
  return true;
}

// ============================================================ containers: .zip / .prisma / .obj
static bool hasRealUv(const Mesh3& m) {
  if (m.uv.size() != m.vcount() * 2) return false;
  for (size_t i = 0; i < m.uv.size(); i += 2) if (m.uv[i] < 1e8f) return true;
  return false;
}
struct ImportResult { Mesh3 mesh; string name, note, mdlPath; };

static string hexDump(const Bytes& b, size_t n) {
  string s; char t[8];
  for (size_t i = 0; i < b.size() && i < n; i++) { snprintf(t, sizeof t, "%02x ", b[i]); s += t; if ((i & 15) == 15) s += "\n"; }
  return s;
}
static bool looksLikeObj(const Bytes& b) {
  size_t lim = min<size_t>(b.size(), 4096); int hits = 0;
  for (size_t i = 0; i + 2 < lim; i++) if ((i == 0 || b[i - 1] == '\n') && (b[i] == 'v' || b[i] == 'f') && b[i + 1] == ' ') hits++;
  return hits >= 3;
}
static bool importFromVfs(const ZipVfs& z, const string& baseName, ImportResult& r, string& err, int depth);

static bool importLooseBytes(const Bytes& d0, const ZipVfs& z, const ZipEnt& src, const string& baseName, ImportResult& r, string& err, int depth) {
  Bytes d = d0;
  if (d.size() > 2 && d[0] == 0x1f && d[1] == 0x8b) { Bytes u; if (!gunzip(d, u)) { err = "gzip data is damaged"; return false; } d.swap(u); }
  if (d.size() > 4 && d[0] == 'P' && d[1] == 'K') {
    ZipVfs inner; if (!inner.open(d, err) || depth > 3) { if (depth > 3) err = "nested zips too deep"; return false; }
    return importFromVfs(inner, baseName, r, err, depth + 1);
  }
  if (looksLikeObj(d)) {
    string dir = dirNameOf(src.name);
    Provider prov = [&z, dir](const string& rel, Bytes& out) {
      const ZipEnt* e = z.find(dir.empty() ? rel : dir + "/" + rel); string er;
      return e && z.read(*e, out, er);
    };
    return loadObj(d, prov, r.mesh, r.note, err);
  }
  // unknown internal layout: save a small report so the format can be added
  string rp = exportDir() + "/" + baseName + "_report.txt", t;
  t += "Magnificent import report\nfile: " + src.name + "\nsize: " + to_string(d.size()) + " bytes\n\nfirst bytes:\n" + hexDump(d, 160) + "\nzip entries:\n";
  for (size_t i = 0; i < z.ents.size(); i++) t += "  " + z.ents[i].name + "  (" + to_string(z.ents[i].usize) + ")\n";
  Bytes tb(t.begin(), t.end()); bool saved = writeFileBytes(rp, tb);
  err = "THIS PROJECT FILE (" + baseNameOf(src.name) + ") HAS AN UNKNOWN LAYOUT" + (saved ? " - REPORT SAVED: " + rp : string());
  return false;
}

static bool importFromVfs(const ZipVfs& z, const string& baseName, ImportResult& r, string& err, int depth) {
  string er;
  if (const ZipEnt* glb = z.best(".glb")) {
    Bytes d; if (!z.read(*glb, d, err)) return false;
    r.name = stripExt(baseNameOf(glb->name));
    if (!loadGlb(d, r.mesh, r.note, err)) return false;
    if (r.name.empty()) r.name = baseName;
    return true;
  }
  const ZipEnt* obj = z.best(".obj");
  if (obj) {
    Bytes d; if (!z.read(*obj, d, err)) return false;
    r.name = stripExt(baseNameOf(obj->name));
    string dir = dirNameOf(obj->name);
    Provider prov = [&z, dir](const string& rel, Bytes& out) {
      const ZipEnt* e = z.find(dir.empty() ? rel : dir + "/" + rel); string e2;
      return e && z.read(*e, out, e2);
    };
    if (!loadObj(d, prov, r.mesh, r.note, err)) return false;
  } else if (const ZipEnt* mdlE = z.best(".mdl")) {   // Source model: unpack beside the exports so the .vvd/.vtx are found next to it
    string out = exportDir() + "/import_" + baseName;
    if (!mkdirP(out)) { err = "cannot write to " + out; return false; }
    for (size_t i = 0; i < z.ents.size(); i++) {
      string rel = normPath(z.ents[i].name); if (rel.empty()) continue;
      Bytes b; if (!z.read(z.ents[i], b, er)) continue;
      string dst = out + "/" + baseNameOf(rel);   // flat: same-name sidecars stay together
      writeFileBytes(dst, b);
    }
    r.mdlPath = out + "/" + baseNameOf(normPath(mdlE->name));
    r.name = stripExt(baseNameOf(mdlE->name)); r.note = "UNPACKED TO " + out;
    return true;
  } else {
    const ZipEnt* proj = z.best(".proj");
    if (!proj) for (size_t i = 0; i < z.ents.size() && !proj; i++) { string x = extOf(z.ents[i].name); if (x != ".png" && x != ".jpg" && x != ".jpeg" && x != ".txt" && x != ".json") proj = &z.ents[i]; }
    if (!proj) { err = "ZIP HAS NO MODEL (.obj / .mdl / .proj)"; return false; }
    Bytes d; if (!z.read(*proj, d, err)) return false;
    r.name = stripExt(baseNameOf(proj->name));
    if (!importLooseBytes(d, z, *proj, baseName, r, err, depth)) return false;
  }
  if (r.mesh.tex.empty() && !r.mesh.pos.empty()) {   // no texture from a .mtl: use the first picture in the archive that is not a screenshot
    for (size_t i = 0; i < z.ents.size(); i++) {
      string x = extOf(z.ents[i].name), ln = lowerStr(z.ents[i].name);
      if ((x != ".png" && x != ".jpg" && x != ".jpeg") || ln.find("screenshot") != string::npos || ln.find("thumb") != string::npos || ln.find("preview") != string::npos) continue;
      Bytes ib; Img im; string ie;
      if (z.read(z.ents[i], ib, ie) && decodeImage(ib, im, ie)) {
        shrinkImg(im, 2048); r.mesh.tex.swap(im.rgba); r.mesh.tw = im.w; r.mesh.th = im.h;
        if (!hasRealUv(r.mesh)) { r.mesh.tex.clear(); r.mesh.tw = r.mesh.th = 0; }   // no UVs to put it on
        else r.note += ", TEXTURE " + to_string(im.w) + "X" + to_string(im.h);
        break;
      }
    }
  }
  if (r.name.empty()) r.name = baseName;
  return true;
}

// Entry point. Fills r.mesh (mesh-only models) or r.mdlPath (a Source .mdl that was unpacked and should be loaded the normal way).
static bool importModelFile(const string& path, ImportResult& r, string& err) {
  string ext = extOf(path), base = stripExt(baseNameOf(path));
  Bytes d;
  if (!readFileBytes(path, d)) { err = "cannot read " + path; return false; }
  if (ext == ".glb") { r.name = base; return loadGlb(d, r.mesh, r.note, err); }
  if (ext == ".obj") {
    string dir = dirNameOf(path);
    Provider prov = [dir](const string& rel, Bytes& out) {
      string rr = slashFix(rel);
      if (readFileBytes(dir.empty() ? rr : dir + "/" + rr, out)) return true;
      return readFileBytes(dir.empty() ? baseNameOf(rr) : dir + "/" + baseNameOf(rr), out);
    };
    r.name = base;
    if (!loadObj(d, prov, r.mesh, r.note, err)) return false;
    if (r.mesh.tex.empty()) {   // .obj without usable .mtl: a same-name .png/.jpg beside it
      static const char* ex[] = {".png", ".jpg", ".jpeg", nullptr};
      for (int i = 0; ex[i] && r.mesh.tex.empty(); i++) {
        Bytes ib; Img im; string ie;
        if (readFileBytes((dir.empty() ? base : dir + "/" + base) + ex[i], ib) && decodeImage(ib, im, ie)) { shrinkImg(im, 2048); r.mesh.tex.swap(im.rgba); r.mesh.tw = im.w; r.mesh.th = im.h; }
      }
      if (!hasRealUv(r.mesh)) { r.mesh.tex.clear(); r.mesh.tw = r.mesh.th = 0; }
    }
    return true;
  }
  if (d.size() < 4 || d[0] != 'P' || d[1] != 'K') {
    if (ext == ".prisma") { err = "NOT A PRISMA3D PROJECT - this looks like a text file (a database .prisma schema?)"; return false; }
    err = "NOT A ZIP FILE"; return false;
  }
  ZipVfs z; if (!z.open(d, err)) return false;
  return importFromVfs(z, base, r, err, 0);
}
