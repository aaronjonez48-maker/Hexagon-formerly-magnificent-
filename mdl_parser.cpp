// Single-file IDST (GoldSrc / Half-Life "studio" v10) .mdl parser.
// Std-only, C++11, little-endian host assumed (Android ARM/x86 are fine).
// Layouts follow the public GoldSrc studio format (studiohdr_t / mstudiobone_t).
#include <iostream>
#include <sstream>
#include <fstream>
#include <vector>
#include <string>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cmath>
using namespace std;

// ---------- Constants ----------
static const uint32_t IDST_MAGIC   = 0x54534449;  // 'I''D''S''T' little-endian
static const int32_t  MDL_VERSION  = 10;
static const int32_t  MAX_BONES    = 128;
static const int32_t  SRC_MIN_VERSION = 44, SRC_MAX_VERSION = 49;   // Source engine (HL2, Portal, TF2, CS:GO ...)
static const int32_t  SRC_MAX_BONES    = 256;
static const int32_t  MAX_BODYPARTS = 64;

// ---------- On-disk structs (packed, byte-exact) ----------
#pragma pack(push, 1)
struct StudioHeader {            // prefix of studiohdr_t (everything we need)
    int32_t id;                  // "IDST"
    int32_t version;             // 10
    char    name[64];
    int32_t length;              // total file size
    float   eyeposition[3];
    float   hullMin[3], hullMax[3];
    float   viewMin[3], viewMax[3];
    int32_t flags;
    int32_t numbones,           boneindex;
    int32_t numbonecontrollers, bonecontrollerindex;
    int32_t numhitboxes,        hitboxindex;
    int32_t numseq,             seqindex;
    int32_t numseqgroups,       seqgroupindex;
    int32_t numtextures,        textureindex, texturedataindex;
    int32_t numskinref, numskinfamilies, skinindex;
    int32_t numbodyparts,       bodypartindex;
};

struct StudioBone {              // mstudiobone_t (112 bytes)
    char    name[32];
    int32_t parent;              // -1 = root
    int32_t flags;
    int32_t bonecontroller[6];
    float   value[6];            // default pose: [0..2] position, [3..5] euler XYZ (radians)
    float   scale[6];
};

struct StudioBodypart {          // mstudiobodypart_t (76 bytes)
    char    name[64];
    int32_t nummodels;
    int32_t base;
    int32_t modelindex;
};

// ---- Source engine (versions 44-49): studiohdr_t prefix + mstudiobone_t
struct SrcHeader {
    int32_t id, version, checksum;
    char    name[64];
    int32_t length;
    float   eyeposition[3], illumposition[3], hullMin[3], hullMax[3], viewMin[3], viewMax[3];
    int32_t flags;
    int32_t numbones, boneindex;
};
struct SrcBone {                 // mstudiobone_t (216 bytes)
    int32_t sznameindex;         // name offset, relative to THIS bone struct
    int32_t parent;              // -1 = root
    int32_t bonecontroller[6];
    float   pos[3];              // local position (inches, Z-up)
    float   quat[4];             // local rotation x,y,z,w
    float   rot[3];              // same rotation as radian euler
    float   posscale[3], rotscale[3];
    float   poseToBone[12];
    float   qAlignment[4];
    int32_t flags, proctype, procindex, physicsbone, surfacepropidx, contents;
    int32_t unused[8];
};
#pragma pack(pop)

static_assert(sizeof(SrcHeader) == 164, "SrcHeader must be 164 bytes");
static_assert(sizeof(SrcBone) == 216, "SrcBone must be 216 bytes");
static_assert(sizeof(StudioBone) == 112, "StudioBone must be 112 bytes");
static_assert(sizeof(StudioBodypart) == 76, "StudioBodypart must be 76 bytes");

// ---------- In-memory model ----------
struct Vec3 { float x, y, z; };
struct Quat { float x, y, z, w; };

struct Bone {
    string name;
    int    parent;
    Vec3   pos;       // default bind-pose position (local)
    Vec3   euler;     // default bind-pose rotation, radians
    Quat   rot;       // same rotation as quaternion
    int    depth;     // hierarchy depth (root = 0)
};

struct Bodypart { string name; int nummodels, base, modelindex; };

struct Model {
    string name;
    int version = 0, fileLength = 0, numBodyparts = 0;
    vector<Bone> bones;
    vector<Bodypart> bodyparts;
};

// ---------- Helpers ----------
static string fixedStr(const char* s, size_t maxLen) {   // safe: no terminator needed
    size_t n = 0;
    while (n < maxLen && s[n]) n++;
    return string(s, n);
}

// Valve's AngleQuaternion (euler XYZ -> quaternion)
static Quat eulerToQuat(const Vec3& a) {
    float sr = sin(a.x * 0.5f), cr = cos(a.x * 0.5f);
    float sp = sin(a.y * 0.5f), cp = cos(a.y * 0.5f);
    float sy = sin(a.z * 0.5f), cy = cos(a.z * 0.5f);
    Quat q;
    q.x = sr * cp * cy - cr * sp * sy;
    q.y = cr * sp * cy + sr * cp * sy;
    q.z = cr * cp * sy - sr * sp * cy;
    q.w = cr * cp * cy + sr * sp * sy;
    return q;
}

template <class T>
static bool readAt(istream& in, streamoff off, T* out, size_t count = 1) {
    in.clear();
    in.seekg(off, ios::beg);
    in.read(reinterpret_cast<char*>(out), sizeof(T) * count);
    return (bool)in && in.gcount() == (streamsize)(sizeof(T) * count);
}

static bool rangeOk(int64_t off, int64_t count, int64_t elemSize, int64_t fileSize) {
    return off >= 0 && count >= 0 && off + count * elemSize <= fileSize;
}

// ---------- Parser ----------
static bool parseSourceMDL(istream& in, streamoff size, Model& out, string& err) {
    if (size < (streamoff)sizeof(SrcHeader)) { err = "file smaller than header"; return false; }
    SrcHeader h;
    if (!readAt(in, 0, &h)) { err = "failed to read header"; return false; }
    if (h.numbones < 0 || h.numbones > SRC_MAX_BONES) { err = "bone count out of range"; return false; }
    if (!rangeOk(h.boneindex, h.numbones, sizeof(SrcBone), size)) { err = "bone table out of bounds"; return false; }

    out = Model();
    out.name = fixedStr(h.name, sizeof(h.name));
    out.version = h.version;
    out.fileLength = h.length;
    out.numBodyparts = 0;                      // bodyparts/meshes live in other tables (and .vvd/.vtx) - not read

    vector<SrcBone> raw(h.numbones);
    if (h.numbones > 0 && !readAt(in, h.boneindex, raw.data(), raw.size())) {
        err = "failed to read bone table"; return false;
    }
    for (int i = 0; i < h.numbones; i++) {
        const SrcBone& b = raw[i];
        if (b.parent < -1 || b.parent >= i) {  // parents must precede children
            err = "bone " + to_string(i) + " has invalid parent " + to_string(b.parent);
            return false;
        }
        Bone nb;
        int64_t nameOff = (int64_t)h.boneindex + (int64_t)i * (int64_t)sizeof(SrcBone) + b.sznameindex;
        nb.name = "bone_" + to_string(i);
        if (b.sznameindex != 0 && nameOff >= 0 && nameOff < size) {
            char buf[64]; memset(buf, 0, sizeof(buf));
            size_t n = (size_t)min<int64_t>(sizeof(buf), size - nameOff);
            in.clear(); in.seekg((streamoff)nameOff, ios::beg); in.read(buf, (streamsize)n);
            string nm = fixedStr(buf, n);
            if (!nm.empty()) nb.name = nm;
        }
        nb.parent = b.parent;
        nb.pos    = Vec3{b.pos[0], b.pos[1], b.pos[2]};
        nb.euler  = Vec3{b.rot[0], b.rot[1], b.rot[2]};
        nb.rot    = Quat{b.quat[0], b.quat[1], b.quat[2], b.quat[3]};
        nb.depth  = b.parent < 0 ? 0 : out.bones[b.parent].depth + 1;
        out.bones.push_back(nb);
    }
    return true;
}

bool parseMDL(istream& in, Model& out, string& err) {
    in.seekg(0, ios::end);
    streamoff size = in.tellg();
    if (size < 8) { err = "file too small to be an MDL"; return false; }

    int32_t idver[2];
    if (!readAt(in, 0, idver, 2)) { err = "failed to read header"; return false; }
    if ((uint32_t)idver[0] != IDST_MAGIC) { err = "bad magic (expected IDST)"; return false; }
    if (idver[1] >= SRC_MIN_VERSION && idver[1] <= SRC_MAX_VERSION) return parseSourceMDL(in, size, out, err);
    if (idver[1] != MDL_VERSION) {
        err = "unsupported version " + to_string(idver[1]) + " (supported: 10, 44-49)";
        return false;
    }
    if (size < (streamoff)sizeof(StudioHeader)) { err = "file smaller than header"; return false; }

    StudioHeader h;
    if (!readAt(in, 0, &h)) { err = "failed to read header"; return false; }

    if ((uint32_t)h.id != IDST_MAGIC) { err = "bad magic (expected IDST)"; return false; }
    if (h.version != MDL_VERSION) {
        err = "unsupported version " + to_string(h.version) + " (expected 10)";
        return false;
    }
    if (h.numbones < 0 || h.numbones > MAX_BONES) { err = "bone count out of range"; return false; }
    if (h.numbodyparts < 0 || h.numbodyparts > MAX_BODYPARTS) { err = "bodypart count out of range"; return false; }
    if (!rangeOk(h.boneindex, h.numbones, sizeof(StudioBone), size)) { err = "bone table out of bounds"; return false; }
    if (!rangeOk(h.bodypartindex, h.numbodyparts, sizeof(StudioBodypart), size)) { err = "bodypart table out of bounds"; return false; }

    out = Model();
    out.name = fixedStr(h.name, sizeof(h.name));
    out.version = h.version;
    out.fileLength = h.length;
    out.numBodyparts = h.numbodyparts;

    // --- skeleton ---
    vector<StudioBone> raw(h.numbones);
    if (h.numbones > 0 && !readAt(in, h.boneindex, raw.data(), raw.size())) {
        err = "failed to read bone table"; return false;
    }
    for (int i = 0; i < h.numbones; i++) {
        const StudioBone& b = raw[i];
        if (b.parent < -1 || b.parent >= i) {   // parents must precede children (also blocks cycles)
            err = "bone " + to_string(i) + " has invalid parent " + to_string(b.parent);
            return false;
        }
        Bone nb;
        nb.name   = fixedStr(b.name, sizeof(b.name));
        nb.parent = b.parent;
        nb.pos    = Vec3{b.value[0], b.value[1], b.value[2]};
        nb.euler  = Vec3{b.value[3], b.value[4], b.value[5]};
        nb.rot    = eulerToQuat(nb.euler);
        nb.depth  = b.parent < 0 ? 0 : out.bones[b.parent].depth + 1;
        out.bones.push_back(nb);
    }

    // --- bodyparts ---
    vector<StudioBodypart> bp(h.numbodyparts);
    if (h.numbodyparts > 0 && !readAt(in, h.bodypartindex, bp.data(), bp.size())) {
        err = "failed to read bodyparts"; return false;
    }
    for (auto& p : bp)
        out.bodyparts.push_back(Bodypart{fixedStr(p.name, sizeof(p.name)), p.nummodels, p.base, p.modelindex});

    return true;
}

bool parseMDL(const string& path, Model& out, string& err) {
    ifstream f(path.c_str(), ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    return parseMDL(f, out, err);
}

// ---------- Printing ----------
void printModel(const Model& m) {
    printf("Model '%s'  v%d  length=%d  bones=%zu  bodyparts=%zu\n",
           m.name.c_str(), m.version, m.fileLength, m.bones.size(), m.bodyparts.size());
    for (size_t i = 0; i < m.bones.size(); i++) {
        const Bone& b = m.bones[i];
        printf("  %*s[%zu] %s (parent %d) pos(%.2f, %.2f, %.2f) rot(%.3f, %.3f, %.3f, %.3f)\n",
               b.depth * 2, "", i, b.name.c_str(), b.parent,
               b.pos.x, b.pos.y, b.pos.z, b.rot.x, b.rot.y, b.rot.z, b.rot.w);
    }
    for (auto& p : m.bodyparts)
        printf("  bodypart '%s' models=%d base=%d\n", p.name.c_str(), p.nummodels, p.base);
}

// ---------- Simulated file ----------
static void setStr(char* dst, size_t n, const char* s) { memset(dst, 0, n); strncpy(dst, s, n - 1); }

static StudioBone makeBone(const char* name, int parent, float px, float py, float pz,
                           float rx = 0, float ry = 0, float rz = 0) {
    StudioBone b; memset(&b, 0, sizeof(b));
    setStr(b.name, sizeof(b.name), name);
    b.parent = parent;
    for (int i = 0; i < 6; i++) b.bonecontroller[i] = -1;
    b.value[0] = px; b.value[1] = py; b.value[2] = pz;
    b.value[3] = rx; b.value[4] = ry; b.value[5] = rz;
    for (int i = 0; i < 6; i++) b.scale[i] = 0.01f;
    return b;
}

static vector<uint8_t> buildFakeMDL() {
    vector<StudioBone> bones;
    bones.push_back(makeBone("Bip01",        -1, 0, 0, 36));
    bones.push_back(makeBone("Bip01 Spine",   0, 0, 0, 10, 0, 0, 1.5708f));
    bones.push_back(makeBone("Bip01 Head",    1, 0, 0, 12));
    bones.push_back(makeBone("Bip01 L Arm",   1, 4, 0,  8, 0, 0.7854f, 0));
    bones.push_back(makeBone("Bip01 R Arm",   1,-4, 0,  8, 0,-0.7854f, 0));

    StudioBodypart bp; memset(&bp, 0, sizeof(bp));
    setStr(bp.name, sizeof(bp.name), "body");
    bp.nummodels = 1; bp.base = 1; bp.modelindex = 0;

    StudioHeader h; memset(&h, 0, sizeof(h));
    h.id = (int32_t)IDST_MAGIC; h.version = MDL_VERSION;
    setStr(h.name, sizeof(h.name), "test_character.mdl");
    h.numbones = (int32_t)bones.size();
    h.boneindex = sizeof(StudioHeader);
    h.numbodyparts = 1;
    h.bodypartindex = h.boneindex + (int32_t)(bones.size() * sizeof(StudioBone));
    h.length = h.bodypartindex + sizeof(StudioBodypart);

    vector<uint8_t> buf(h.length);
    memcpy(&buf[0], &h, sizeof(h));
    memcpy(&buf[h.boneindex], bones.data(), bones.size() * sizeof(StudioBone));
    memcpy(&buf[h.bodypartindex], &bp, sizeof(bp));
    return buf;
}

static vector<uint8_t> buildFakeSourceMDL() {    // 3-bone Source v49 model, names stored after the bone table
    const int nb = 3; const char* names[nb] = {"ValveBiped.Bip01_Pelvis", "ValveBiped.Bip01_Spine", "ValveBiped.Bip01_Head1"};
    int parents[nb] = {-1, 0, 1}; float pz[nb] = {38, 8, 20};
    SrcHeader h; memset(&h, 0, sizeof(h));
    h.id = (int32_t)IDST_MAGIC; h.version = 49;
    setStr(h.name, sizeof(h.name), "source_test.mdl");
    h.numbones = nb; h.boneindex = sizeof(SrcHeader);
    int nameBase = h.boneindex + nb * (int)sizeof(SrcBone);
    int total = nameBase; for (int i = 0; i < nb; i++) total += (int)strlen(names[i]) + 1;
    h.length = total;
    vector<uint8_t> buf(total, 0);
    memcpy(&buf[0], &h, sizeof(h));
    int namePos = nameBase;
    for (int i = 0; i < nb; i++) {
        SrcBone b; memset(&b, 0, sizeof(b));
        int boneAddr = h.boneindex + i * (int)sizeof(SrcBone);
        b.sznameindex = namePos - boneAddr; b.parent = parents[i];
        b.pos[2] = pz[i]; b.quat[3] = 1;
        memcpy(&buf[boneAddr], &b, sizeof(b));
        memcpy(&buf[namePos], names[i], strlen(names[i]) + 1);
        namePos += (int)strlen(names[i]) + 1;
    }
    return buf;
}

static bool runTest(const char* label, const vector<uint8_t>& data) {
    string s(reinterpret_cast<const char*>(data.data()), data.size());
    istringstream in(s, ios::in | ios::binary);
    Model m; string err;
    printf("\n--- %s ---\n", label);
    if (parseMDL(in, m, err)) { printModel(m); return true; }
    printf("parse failed: %s\n", err.c_str());
    return false;
}

#ifdef MAGNIFICENT_BUILD
int mdl_demo() {
#else
int main() {
#endif
    printf("sizeof(StudioHeader)=%zu sizeof(StudioBone)=%zu\n", sizeof(StudioHeader), sizeof(StudioBone));

    vector<uint8_t> good = buildFakeMDL();
    runTest("valid model", good);
    runTest("source engine v49 model", buildFakeSourceMDL());

    vector<uint8_t> badMagic = good; badMagic[0] = 'X';
    runTest("bad magic", badMagic);

    vector<uint8_t> badVer = good; badVer[4] = 99;
    runTest("wrong version", badVer);

    vector<uint8_t> truncated(good.begin(), good.begin() + 200);
    runTest("truncated file", truncated);

    vector<uint8_t> badParent = good;
    StudioHeader h; memcpy(&h, &badParent[0], sizeof(h));
    StudioBone* b2 = reinterpret_cast<StudioBone*>(&badParent[h.boneindex]) + 2;
    int32_t evil = 4; memcpy(&b2->parent, &evil, 4);  // bone 2 points to a later bone
    runTest("invalid parent index", badParent);

    // To load a real file on your phone instead:
    //   Model m; string err;
    //   if (parseMDL("/sdcard/Download/model.mdl", m, err)) printModel(m);
    return 0;
}
