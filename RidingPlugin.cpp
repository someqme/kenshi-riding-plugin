#include <Debug.h>

#include <kenshi/Character.h>
#include <kenshi/CharMovement.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include "RidingContextMenu.h"
#include <kenshi/InputHandler.h>
#include <kenshi/Animation/AnimationClass.h>
#include <kenshi/Animation/AnimationClassHuman.h>
#include <kenshi/Building/Building.h>
#include <kenshi/Enums.h>

#include <ois/OISKeyboard.h>

#include <core/Functions.h>

#include <ogre/OgreVector3.h>
// OgreSceneNode.h is load-bearing: AnimationClass::node is an Ogre::SceneNode* and
// no other included header pulls the complete type in transitively.
#include <ogre/OgreSceneNode.h>

#include <mygui/MyGUI_Window.h>

#include <boost/unordered_map.hpp>
#include <string>

#include <windows.h>
#include <stdio.h>

// v100-safe int->string helper (std::to_string is ambiguous in old MSVC)
static std::string IntToStr(int v)
{
    char buf[32];
    _itoa_s(v, buf, 10);
    return std::string(buf);
}

// ---------------------------------------------------------------------------
// Riding plugin for Kenshi
//
// Numpad1: mount the selected human rider onto the selected animal
// Numpad2: dismount the rider
//
// While mounted, the rider's position is synced to the mount's back once per
// frame from GameWorld::mainLoop_GPUSensitiveStuff (a stable per-frame hook).
// ---------------------------------------------------------------------------

std::string GetSpecies(Character* c);   // defined below (name-keyed blocklist in IsRideable)

// Species excluded from riding by user request (2026-08-23): the beak-ape family
// (frog_gorilla skeleton) - the seat never settled well on that skeleton.  IsRideable
// is the single chokepoint (menu rename + every mount path), so returning false here
// keeps them fully vanilla: the context menu keeps the original bodyguard entry and
// all mount attempts are rejected.  Their riding.cfg entries stay so a future
// re-enable restores the old tuning untouched.
static const char* const kNonRideable[] = {
    "\xE5\x96\x99\xE5\x98\xB4\xE7\x8C\xA9\xE7\x8C\xA9",                 // 喙嘴猩猩
    "\xE5\x96\x99\xE5\x98\xB4\xE7\x8C\xA9\xE7\x8C\xA9\xE4\xB9\x8B\xE7\x8E\x8B", // 喙嘴猩猩之王
    "\xE6\x88\x98\xE6\x96\x97\xE5\x96\x99\xE5\x98\xB4\xE7\x8C\xA9\xE7\x8C\xA9", // 战斗喙嘴猩猩
    "\xE9\xBB\x91\xE8\x89\xB2\xE5\x96\x99\xE5\x98\xB4\xE7\x8C\xA9\xE7\x8C\xA9", // 黑色喙嘴猩猩
    "\xE5\xB7\xA8\xE5\x9E\x8B\xE7\x99\xBD\xE8\x89\xB2\xE5\x96\x99\xE5\x98\xB4\xE7\x8C\xA9\xE7\x8C\xA9" // 巨型白色喙嘴猩猩
};

static bool IsRideable(Character* c)
{
    if (!c) return false;
    // only animals owned/by the player are allowed
    if (!c->isAnimal()) return false;
    // isWithThePlayer() exists in Character and indicates player ownership/party
    try { if (!c->isWithThePlayer()) return false; } catch(...) { return false; }
    // name-keyed exclusion list (see kNonRideable above)
    {
        std::string sp = GetSpecies(c);
        for (int i = 0; i < (int)(sizeof(kNonRideable) / sizeof(kNonRideable[0])); ++i)
            if (sp == kNonRideable[i]) return false;
    }
    return true;
}

// rider -> mount
boost::unordered_map<Character*, Character*> riderToMount;
// mount -> rider
boost::unordered_map<Character*, Character*> mountToRider;


// Seat modes: how the rider's seat anchor is chosen for this animal.
//   0 = EXACT    - pin to the back bone (Spine2 etc.)
//   1 = MIDPOINT - torso midpoint between the front back bone and the pelvis
//   2 = NECK     - highest point (neck bone), for huge animals whose body would
//                  otherwise swallow the rider
enum SeatMode
{
    SEAT_EXACT    = 0,
    SEAT_MIDPOINT = 1,
    SEAT_NECK     = 2
};

// Rider upper-body posture while mounted.
//   0 = SIT   - "sitting chair" (the toilet-sitting pose the player confirmed)
//   1 = STAND - "idle_stand_normal" (arms/upper body exactly like standing on the ground)
enum RiderPosture
{
    POSTURE_SIT   = 0,
    POSTURE_STAND = 1
};

// Per-mount seat setup (computed at mount time).
struct SeatInfo
{
    std::string   species;       // mount->getName() - key for per-species tuning ("" if unknown)
    std::string   backBone;      // bone used for slave attach (orientation + fallback anchor)
    std::string   frontBone;     // front torso anchor bone (Spine2/Spine1/Spine/Head)
    std::string   rearBone;      // rear torso anchor bone (Pelvis), "" if none
    int           seatMode;      // SeatMode (0/1/2)
    bool          forceWalk;     // mount's walk animation is suppressed by carry mode (pack_beast)
    bool          rootAnchor;    // anchor to root bone instead of the swinging back bone
                                 // (fling skeletons: Crab/robot_worker/dog/gorilla/Crimper/beak)
    bool          neckFollow;    // 卷缩者 only: vertical follows the NECK bone (butt rides
                                 // up/down with the neck), no bob damping; horizontal still root
    bool          forceSit;      // rider: re-assert the sitting pose every frame (overrides carried prone)
    int           posture;       // RiderPosture (0=sit, 1=stand)
    float         lateral;       // side offset (right/left), world units
    float         torsoLen;      // front<->rear bone distance at mount time (world units)
    Ogre::Vector3 lift;          // base seat offset (mostly +Y, auto-sized)
    Ogre::Vector3 userOffset;    // live-tuned delta on top of lift (x = forward, y = up)
    float         sizeScale;     // always 1.0 - per-individual size scaling was removed
                                 // (2026-08-23): bone reads at mount/load instants are
                                 // untrustworthy (can return bind-space coords), so the
                                 // measured factor poisoned the tuned offsets
};

// mount -> seat setup
boost::unordered_map<Character*, SeatInfo> mountSeat;
// mount -> last recorded position (for walk-animation movement detection)
boost::unordered_map<Character*, Ogre::Vector3> mountLastPos;
// mount -> constant vertical offset (seat height above the mount's ROOT bone),
// captured on the first ridden frame.  The seat anchor bone (neck/spine) bobs up and
// down in the run cycle; the root bone does not.  DampSeatBob rebuilds the seat height
// as rootY + this offset + a scaled fraction of the bob, so we can shrink the bob
// amplitude directly (stateless - independent of how many times per frame it runs,
// unlike the old exponential low-pass which converged to instant and did nothing).
boost::unordered_map<Character*, float> mountBaseVOffset;
// (speciesRefScale removed 2026-08-23 together with per-individual size scaling:
//  bone reads at the SeatInfo build instants are unreliable, see BuildSeatInfo)

// Debug: toggled with Numpad . for continuous ride diagnostics (every 10 frames).
static bool debugContinuous = false;
// Debug: per-mount last position for movement detection (independent of mountLastPos,
// which is only written for forceWalk mounts).
boost::unordered_map<Character*, Ogre::Vector3> debugLastPos;
// mount -> smoothed orientation for fling skeletons (rootAnchor).  The crab/etc. run
// animations flip the root/back bone orientation ~180 degrees frame to frame, which
// would fling the rider's facing; an nlerp low-pass keeps the rider's orientation
// stable.
boost::unordered_map<Character*, Ogre::Quaternion> mountSmoothOrient;

// mount -> travel-heading tracking.  The rider faces the mount's actual DIRECTION OF
// TRAVEL (movement position delta between frames), not the head/body bone forward, so
// when the animal turns its head or wiggles its body while running straight the rider
// keeps facing the way the mount is really moving.  mountHeadingPos is last frame's
// movement position; mountHeadingDir is the last non-trivial travel direction, HELD
// while the mount stands still so the rider keeps its last heading instead of spinning.
boost::unordered_map<Character*, Ogre::Vector3> mountHeadingPos;
boost::unordered_map<Character*, Ogre::Vector3> mountHeadingDir;

// mount -> root-bone anchor offset, captured on the first synced frame as
// (rBip - node).  SyncRiderNode keeps the rider's RENDER root bone at
// seatPos + anchor, so the rider stops swinging while the existing per-species
// seat tuning (which was calibrated against that offset) stays valid.
boost::unordered_map<Character*, Ogre::Vector3> mountAnchor;

// Per-mount constant-capture bookkeeping.  The anchor (above) and the DampSeatBob
// baseline are only trustworthy once the post-mount / post-load pose storm (carried-
// pose blend-out, skeleton settling) has finished - capturing them on the FIRST sync
// froze transient values (anchor 9.3 vs the settled sitting ~6.5) that then never
// corrected, leaving the rider mis-seated after every save/load.  These counters
// track how many consecutive syncs each quantity has held steady; capture and
// revalidation key off them.  (SyncRiderNode/DampSeatBob run from animUpdate AND
// mainLoop, so a "sync" is either hook - thresholds count syncs, not frames.)
struct CapTrack
{
    Ogre::Vector3 prevRel;   // last raw (rBip - node)
    float         prevBase;  // last raw seat-height-above-reference
    int           relStable;
    int           baseStable;
    CapTrack() : prevRel(Ogre::Vector3::ZERO), prevBase(0.0f), relStable(0), baseStable(0) {}
};
boost::unordered_map<Character*, CapTrack> mountCap;
// A quantity must hold this many consecutive stable syncs before it is captured
// or a stored copy is adopted over the live reading.
static const int kCaptureStableNeed = 15;
// Anchor clamp shared by SyncRiderNode and SeedPersistedConstants (was local to
// SyncRiderNode; hoisted when cfg persistence needed it).
static const float kMaxAnchorLen = 12.0f;
// DBG only: last scene-node value WE wrote per rider.  When a later read back shows
// drift from this, some engine writer re-positioned the carried rider AFTER us -
// quantifies the who-writes-last race instead of guessing.
boost::unordered_map<Character*, Ogre::Vector3> dbgNodeWritten;

// Pending "approach then mount" requests.  When the player picks "上马" (the repurposed
// Bodyguard menu order) on an animal, the rider does NOT teleport onto it if it is far
// away.  Instead it paths toward the animal - exactly like the native "pick up" order -
// and only actually boards once it is within order/interaction range.  This map holds
// those in-flight requests; mainLoop_hook services them every frame.
struct PendingMount
{
    Character* mount;   // the animal the rider is walking toward
    int        age;     // frames since the request (timeout guard)
    int        refresh; // frames since the destination was last re-issued (mount may roam)
};
// rider -> pending approach-to-mount
boost::unordered_map<Character*, PendingMount> pendingMount;
// Give up approaching after this many frames (the mount may be unreachable).
static const int kMountApproachTimeout = 1800;
// Re-issue the move destination this often so the rider keeps chasing a walking mount.
static const int kMountApproachRefresh = 15;
// Horizontal distance (world units) at which the rider is deemed "walked up to" the
// mount and boards.  Generous so it fires reliably for medium/large animals; small
// animals just board from a touch farther.  isDestinationReached() is the scale-free
// backup for animals whose body keeps the rider farther from centre than this.
static const float kMountArriveDist = 10.0f;

// Per-species tuning: seat mode + (x = forward, y = up) world-space delta.
// Orientation is NOT tunable - the rider always faces the mount's travel direction.
// (cfg column 4 "mount" is a dead legacy field: every ride uses native carry since
//  2026-08-20; the column position survives read-and-ignore / write-0 for file compat.)
struct SpeciesTuning
{
    int           seatMode;
    bool          forceSit;      // re-assert the sitting pose every frame
    int           posture;       // RiderPosture (0=sit, 1=stand)
    float         lateral;       // side offset (right/left), world units
    Ogre::Vector3 offset;
    // Settled per-pose constants persisted in riding.cfg columns 11-14 (2026-08-23):
    // anchor = rider root-bone offset from its scene node while seated, base = seat
    // height above the bob reference.  Mount/load replays SEED these instead of
    // live-capturing them, so placement is correct from frame one ("remember, don't
    // re-derive").  ZERO/0 = never captured for this species.
    Ogre::Vector3 anchor;
    float         base;
    SpeciesTuning() : seatMode(SEAT_MIDPOINT), forceSit(true), posture(POSTURE_SIT), lateral(0.0f), offset(Ogre::Vector3::ZERO), anchor(Ogre::Vector3::ZERO), base(0.0f) {}
    SpeciesTuning(int m, const Ogre::Vector3& o) : seatMode(m), forceSit(true), posture(POSTURE_SIT), lateral(0.0f), offset(o), anchor(Ogre::Vector3::ZERO), base(0.0f) {}
};
boost::unordered_map<std::string, SpeciesTuning> speciesTuning;

// ---- Stale-pointer defences (2026-08-23 save-load crash) --------------------
//
// Loading a save MID-SESSION frees every character the old world owned, but our
// per-frame maps keep the old Character* around.  The first main-loop frame after
// such a load dereferences freed memory: member reads hand back garbage and the
// first virtual call faults (crash dump: pendingMount loop -> move->halt() on a
// junk CharMovement*, rax=0004e1a600000000, INVALID_POINTER_READ).  Three layers:
//   1. CharacterLooksLive - cheap gate before any game call on a map pointer
//   2. one validation sweep at the top of mainLoop's tracked-state section that
//      wipes ALL ride state when anything looks dead (after a world reset every
//      cached pointer is equally stale, so surgical erase is pointless)
//   3. SEH shells around both hot hooks - an access violation wipes state instead
//      of killing the process

static bool PlausibleHeapPtr(const void* p)
{
    uintptr_t v = (uintptr_t)p;
    // Reject null/tiny values, kernel space, and the packed-uid pattern seen in
    // the crash dump (0004e1a600000000); require 8-byte alignment.
    return v > 0x10000ull && v < 0x0000700000000000ull && (v & 0x7) == 0;
}

// True if p lies inside the host exe's image (game-class vtables live there).
static bool InGameImage(const void* p)
{
    static uintptr_t s_lo = 0, s_hi = 0;
    if (!s_lo)
    {
        HMODULE h = GetModuleHandleA(NULL);            // host process = kenshi_x64.exe
        if (!h) return true;                           // cannot resolve: do not block
        const IMAGE_DOS_HEADER* dos = (const IMAGE_DOS_HEADER*)h;
        if (dos->e_magic != IMAGE_DOS_SIGNATURE) return true;
        const IMAGE_NT_HEADERS* nt = (const IMAGE_NT_HEADERS*)((const char*)h + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE) return true;
        s_lo = (uintptr_t)h;
        s_hi = s_lo + nt->OptionalHeader.SizeOfImage;
    }
    uintptr_t v = (uintptr_t)p;
    return v >= s_lo && v < s_hi;
}

// Cheap "is this Character* still believable" gate for pointers out of our maps,
// which may be freed after a mid-session load.  Member reads only + one guarded
// vtable peek - it never dispatches through object state, so it cannot crash on
// garbage the way isAnimal()/getPosition() would.
static bool CharacterLooksLive(Character* c)
{
    if (!PlausibleHeapPtr(c)) return false;
    bool ok = false;
    __try { ok = InGameImage(*(void* const*)c); }      // first qword = vtable pointer
    __except (EXCEPTION_EXECUTE_HANDLER) { ok = false; }
    return ok;
}

// Drop every piece of tracked ride state at once.  Deliberately does NOT call
// back into the engine (no dropCarriedObject etc.) - when we wipe, the pointers
// are not trustworthy.  If the freshly loaded save has someone mounted, the
// native carry link survives in the save and TryRestoreOrphanedMount rebuilds
// the pair from it on the next animation frame.
static void WipeAllRideState(const char* why)
{
    DebugLog(std::string("Riding: WIPE all ride state (") + why + ")");
    riderToMount.clear();
    mountToRider.clear();
    mountSeat.clear();
    mountAnchor.clear();
    mountCap.clear();
    dbgNodeWritten.clear();
    mountBaseVOffset.clear();
    mountSmoothOrient.clear();
    mountHeadingPos.clear();
    mountHeadingDir.clear();
    debugLastPos.clear();
    mountLastPos.clear();
    pendingMount.clear();
}

// Back bones tried in order. Different animals use different skeletons:
//   bull / dog / Big-Bones / elephant_turtle / cage_beast: Spine2, Spine1, Spine, Pelvis
//   Crab: Spine + Pelvis (no Spine1/Spine2)
//   antilop250: only the root bone Bip01 (+ Bip01 Head)
// We pick the first bone the mount actually has so riding works on every animal.
static const char* const kBackBones[] =
{
    "Bip01 Spine2",
    "Bip01 Spine1",
    "Bip01 Spine",
    "Bip01 Pelvis",
    "Bip01"
};

// MIDPOINT base lift = torso length * this ratio (scales with animal size).
const float kSeatLiftRatio = 0.2f;
const float kTuningClamp   = 15.0f;

bool IsRiding(Character* c)
{
    if (!c) return false;
    return riderToMount.find(c) != riderToMount.end();
}

Character* GetMount(Character* rider)
{
    boost::unordered_map<Character*, Character*>::iterator it = riderToMount.find(rider);
    if (it != riderToMount.end())
        return it->second;
    return NULL;
}

// Horizontal (ground-plane) distance from the rider to the mount, in world units.
// The native AI::isTargetInRangeForOrders turned out NOT to mean "pickup distance"
// (for a player squad it is essentially always true), so we gate the approach on a
// real distance instead.  Height is ignored so a tall mount's back doesn't inflate it.
static float RiderMountDist(Character* rider, Character* mount)
{
    if (!rider || !mount) return 1.0e9f;
    Ogre::Vector3 rp = rider->getMovement() ? rider->getMovement()->getPosition() : rider->getPosition();
    Ogre::Vector3 mp = mount->getPosition();
    Ogre::Vector3 d = rp - mp;
    d.y = 0.0f;
    return d.length();
}

std::string PickBackBone(Character* mount)
{
    if (!mount) return "Bip01";
    AnimationClass* mountAnim = mount->getAnimationClass();
    if (!mountAnim) return "Bip01";
    for (int i = 0; i < 5; ++i)
    {
        if (mountAnim->getHasBone(kBackBones[i]))
            return kBackBones[i];
    }
    return "Bip01";
}

std::string GetSpecies(Character* c)
{
    if (!c) return "";
    try { return c->getName(); } catch (...) { return ""; }
}

// --- riding.cfg persistence (v100-safe plain text, no JSON) ----------------
// Format per line: <species>=<mode>,<up>,<forward>
//   mode    0=exact, 1=midpoint, 2=neck
//   up      positive = higher seat, negative = lower
//   forward positive = toward the head, negative = toward the tail
// Legacy 2-field lines (<species>=<up>,<forward>) are accepted as MIDPOINT.

static std::string GetModDir()
{
    char buf[MAX_PATH] = {0};
    HMODULE h = GetModuleHandleA("RidingPlugin.dll");
    if (!h) h = GetModuleHandleA(NULL);
    if (GetModuleFileNameA(h, buf, MAX_PATH))
    {
        std::string p(buf);
        size_t pos = p.find_last_of("\\/");
        if (pos != std::string::npos)
            return p.substr(0, pos);
    }
    return ".";
}

static std::string GetConfigPath()
{
    return GetModDir() + "\\riding.cfg";
}

static void TrimStr(std::string& s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) { s.clear(); return; }
    size_t b = s.find_last_not_of(" \t\r\n");
    s = s.substr(a, b - a + 1);
}

static void LoadConfig()
{
    speciesTuning.clear();
    FILE* f = fopen(GetConfigPath().c_str(), "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        std::string s(line);
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string name = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        TrimStr(name);
        TrimStr(val);
        if (name.empty()) continue;
        float up = 0.0f, fwd = 0.0f;
        int mode = SEAT_MIDPOINT;
        int sit = 1;
        // columns 4 and 6-8 of the line format are obsolete: column 4 "mount" is a dead
        // legacy field (every ride uses native carry since 2026-08-20), columns 6-8 are
        // roll/pitch/yaw orientation tunes - facing is always the mount's travel direction
        // now.  Parse them into dummies only so existing cfg files keep loading with the
        // same column layout.
        int mountIg = 0;
        float rollIg = 0.0f, pitchIg = 0.0f, yawIg = 0.0f;
        int posture = POSTURE_SIT;
        float lateral = 0.0f;
        int n = sscanf(val.c_str(), "%d,%f,%f,%d,%d,%f,%f,%f,%d,%f", &mode, &up, &fwd, &mountIg, &sit, &rollIg, &pitchIg, &yawIg, &posture, &lateral);
        if (n >= 3)
        {
            if (mode < SEAT_EXACT) mode = SEAT_EXACT;
            if (mode > SEAT_NECK)  mode = SEAT_NECK;
            if (posture < POSTURE_SIT) posture = POSTURE_SIT;
            if (posture > POSTURE_STAND) posture = POSTURE_STAND;
            SpeciesTuning st(mode, Ogre::Vector3(fwd, up, 0.0f));
            st.forceSit = (sit != 0);
            st.posture = posture;
            st.lateral = lateral;
            // columns 11-14: persisted seat constants (anchor xyz + bob baseline),
            // written by the capture path - optional so old cfg files still load.
            float ax = 0.0f, ay = 0.0f, az = 0.0f, abase = 0.0f;
            if (sscanf(val.c_str(), "%*d,%*f,%*f,%*d,%*d,%*f,%*f,%*f,%*d,%*f,%f,%f,%f,%f", &ax, &ay, &az, &abase) == 4)
            {
                st.anchor = Ogre::Vector3(ax, ay, az);
                st.base   = abase;
            }
            speciesTuning[name] = st;
        }
        else if (sscanf(val.c_str(), "%f,%f", &up, &fwd) == 2)
        {
            speciesTuning[name] = SpeciesTuning(SEAT_MIDPOINT, Ogre::Vector3(fwd, up, 0.0f));
        }
    }
    fclose(f);
}

static void SaveConfig()
{
    FILE* f = fopen(GetConfigPath().c_str(), "w");
    if (!f) return;
    fprintf(f, "# riding.cfg - per-species seat tuning\n");
    fprintf(f, "# <species>=<mode>,<up>,<forward>,<mount>,<sit>,<roll>,<pitch>,<yaw>,<posture>,<lateral>  mode 0=exact 1=midpoint 2=neck  sit 0=off 1=on  posture 0=sit 1=stand  lateral=side offset\n");
    fprintf(f, "# columns 4 and 6-8 are OBSOLETE legacy fields (mount method / roll-pitch-yaw) - parsed-and-ignored, always written as 0\n");
    fprintf(f, "# columns 11-14 = persisted seat constants (anchor x/y/z + bob baseline), auto-captured - do not hand-edit\n");
    boost::unordered_map<std::string, SpeciesTuning>::iterator it = speciesTuning.begin();
    for (; it != speciesTuning.end(); ++it)
        fprintf(f, "%s=%d,%.2f,%.2f,%d,%d,0.0,0.0,0.0,%d,%.2f,%.3f,%.3f,%.3f,%.3f\n", it->first.c_str(), it->second.seatMode,
                it->second.offset.y, it->second.offset.x, 0,
                it->second.forceSit ? 1 : 0, it->second.posture, it->second.lateral,
                it->second.anchor.x, it->second.anchor.y, it->second.anchor.z, it->second.base);
    fclose(f);
}

// A save/load can leave a character's animation-class returning garbage for SOME bones
// while the rest of the skeleton reads fine (seen 2026-08-23: after load, a 野牛's
// "Bip01 Pelvis" read ~140 units off-body and 130 underground, dragging the midpoint
// seat ~70 units below the spine -> rider placed inside the belly/under the terrain =
// "model vanished"; GetMountForward was steered by the same ghost bone).  Cross-check
// a rear-anchor read against the skeleton's own root<->back span before trusting it.
static bool RearBoneReadSane(Character* mount, const SeatInfo& seat, const Ogre::Vector3& rear)
{
    AnimationClass* mAnim = mount ? mount->getAnimationClass() : NULL;
    if (!mAnim || !mAnim->getHasBone("Bip01") || seat.backBone.empty()) return true;  // can't cross-check -> trust
    Ogre::Vector3 rootP = mount->getBoneWorldPosition("Bip01");
    Ogre::Vector3 backP = mount->getBoneWorldPosition(seat.backBone);
    float maxSpan = (rootP - backP).length() * 4.0f + 10.0f;
    return (rear - backP).length() <= maxSpan;
}

// Build the seat setup for a mount:
//   - seat mode comes from riding.cfg per species (default MIDPOINT)
//   - EXACT:    anchor = back bone, lift from root/pelvis fallbacks
//   - MIDPOINT: anchor = torso midpoint, lift = torsoLen * ratio
//   - NECK:     anchor = neck bone (highest point), no base lift
// The live tuning delta (userOffset) is applied on top of whatever base is chosen.
SeatInfo BuildSeatInfo(Character* mount)
{
    SeatInfo info;
    info.species = GetSpecies(mount);
    info.backBone = PickBackBone(mount);
    info.frontBone = info.backBone;
    info.lift = Ogre::Vector3::ZERO;
    info.userOffset = Ogre::Vector3::ZERO;
    info.seatMode = SEAT_MIDPOINT;
    info.forceWalk = false;
    info.forceSit = true;
    info.posture = POSTURE_SIT;
    info.lateral = 0.0f;
    info.torsoLen = 0.0f;
    info.rootAnchor = false;
    info.neckFollow = false;
    info.sizeScale = 1.0f;

    AnimationClass* mountAnim = mount ? mount->getAnimationClass() : NULL;
    if (mountAnim)
    {
        if (mountAnim->getHasBone("Bip01 Pelvis"))
            info.rearBone = "Bip01 Pelvis";

        if (info.backBone == "Bip01")
        {
            // root bone sits at ground level; estimate back height from the head
            float seatH = 1.0f;
            if (mountAnim->getHasBone("Bip01 Head"))
            {
                float headH = mountAnim->getBoneInitialWorldPosition("Bip01 Head").y;
                if (headH > 0.0f)
                    seatH = headH * 0.5f;
            }
            if (seatH < 0.5f) seatH = 0.5f;
            if (seatH > 2.5f) seatH = 2.5f;
            info.lift.y = seatH;
        }
        else if (info.backBone == "Bip01 Pelvis")
        {
            // pelvis sits low at the rear; lift the rider up onto the back
            info.lift.y = 0.5f;
        }

        if (!info.rearBone.empty() && info.rearBone != info.frontBone)
        {
            Ogre::Vector3 front = mount->getBoneWorldPosition(info.frontBone);
            Ogre::Vector3 rear  = mount->getBoneWorldPosition(info.rearBone);
            // ghost read guard: an unscaled-space pelvis read must not become
            // torsoLen (= midpoint lift base)
            if (RearBoneReadSane(mount, info, rear))
                info.torsoLen = (front - rear).length();
        }
    }

    // per-species override: mode + tuned delta
    boost::unordered_map<std::string, SpeciesTuning>::iterator tit = speciesTuning.find(info.species);
    if (tit != speciesTuning.end())
    {
        info.seatMode = tit->second.seatMode;
        info.userOffset = tit->second.offset;
        info.forceSit = tit->second.forceSit;
        info.posture = tit->second.posture;
        info.lateral = tit->second.lateral;
    }

    // pack_beast family (Garru / Pack Beast / Dead Pack Beast) share the "beast walk"
    // animation, which the carry system suppresses.  Detect it from the animation
    // data so we can force the walk back on while ridden.
    if (mountAnim && mountAnim->getAnimationData("beast walk"))
        info.forceWalk = true;

    // Fling skeletons: their back bone is rigid / the root bone is thrown up and down
    // heavily during running, which flings a rider anchored to the back bone.  Anchor
    // these to the root bone instead (follows the whole animal, no bone swing).
    if (!info.species.empty())
    {
        static const char* const kFlingSkeletons[] = {
            "螃蟹", "巨型螃蟹", "提多", "巴纳布斯", "螃蟹终结者", "巨蟹先生",   // Crab
            "铁蜘蛛", "安全蜘蛛",                                              // robot_worker
            "骨犬", "埋骨地狼", "定居者的小狗",                                 // dog
            "喙嘴猩猩", "喙嘴猩猩之王", "战斗喙嘴猩猩", "黑色喙嘴猩猩", "巨型白色喙嘴猩猩", // frog_gorilla
            "卷缩者", "国王",                                                  // Crimper
            "喙嘴兽"                                                          // Mammal_beak
        };
        for (int i = 0; i < (int)(sizeof(kFlingSkeletons) / sizeof(kFlingSkeletons[0])); ++i)
        {
            if (info.species == kFlingSkeletons[i])
            {
                info.rootAnchor = true;
                break;
            }
        }
    }

    // 卷缩者 (Crimper) ONLY: the player wants the rider's butt glued to the NECK bone so
    // it rides up and down together with the neck - the flattened root-anchored height
    // looks right while running but floats when the mount stands still.  Keep the stable
    // root horizontal (rootAnchor stays on), but follow the neck bone's Y with no bob
    // damping.  Deliberately scoped to this one species.
    if (info.species == "\xE5\x8D\xB7\xE7\xBC\xA9\xE8\x80\x85"    // 卷缩者 (UTF-8)
        && mountAnim && mountAnim->getHasBone("Bip01 Neck"))
        info.neckFollow = true;

    // base lift per mode
    if (info.seatMode == SEAT_MIDPOINT)
    {
        if (!info.rearBone.empty() && info.rearBone != info.frontBone)
            info.lift = Ogre::Vector3(0.0f, info.torsoLen * kSeatLiftRatio, 0.0f);
        // no rear bone (root-only skeletons): keep the head-based EXACT lift
    }
    else if (info.seatMode == SEAT_NECK)
    {
        info.lift = Ogre::Vector3::ZERO; // anchor already at the highest point
    }
    // SEAT_EXACT keeps the per-bone lift set above

    // Per-individual size scaling is gone (2026-08-23): bone world reads are
    // untrustworthy exactly when we build a SeatInfo - right after pickup/load they
    // come back unscaled (~10x node-scale multiple), so no measured factor is reliable.
    // Tuning is purely per-species via riding.cfg.
    info.sizeScale = 1.0f;

    return info;
}

// Facing direction of the mount (headward), projected on the ground plane.
Ogre::Vector3 GetMountForward(const SeatInfo& seat, Character* mount)
{
    Ogre::Vector3 fwd(0.0f, 0.0f, 1.0f);
    std::string fB = seat.frontBone.empty() ? seat.backBone : seat.frontBone;
    Ogre::Vector3 p1 = mount->getBoneWorldPosition(fB);
    Ogre::Vector3 p2;
    bool haveP2 = false;
    if (!seat.rearBone.empty())
    {
        Ogre::Vector3 rp = mount->getBoneWorldPosition(seat.rearBone);
        // ghost read guard: a corrupted rear bone would steer facing too - skip it
        if (RearBoneReadSane(mount, seat, rp))
        {
            p2 = rp;
            haveP2 = true;
        }
    }
    else if (mount->getAnimationClass() && mount->getAnimationClass()->getHasBone("Bip01 Head"))
    {
        p2 = mount->getBoneWorldPosition("Bip01 Head");
        haveP2 = true;
    }
    if (haveP2)
    {
        Ogre::Vector3 d = p1 - p2;
        d.y = 0.0f;
        float len = d.length();
        if (len > 0.001f)
            fwd = d / len;
    }
    return fwd;
}

// Final seat position in world space for the given mount.
Ogre::Vector3 ComputeSeatPosition(const SeatInfo& seat, Character* mount)
{
    Ogre::Vector3 anchor = mount->getBoneWorldPosition(seat.backBone);
    if (seat.rootAnchor)
    {
        // Fling skeletons: the back bone is thrown around by the run cycle, so anchor
        // to the root bone instead.  Horizontal follows the root bone (whole animal,
        // no bone swing); vertical = root height + the fixed height of the back bone
        // above the root (bind-pose difference, constant during movement).
        AnimationClass* mountAnim = mount->getAnimationClass();
        if (mountAnim && mountAnim->getHasBone("Bip01"))
        {
            Ogre::Vector3 rootPos = mount->getBoneWorldPosition("Bip01");
            anchor = rootPos;
            if (mountAnim->getHasBone(seat.backBone))
            {
                float rootInitY = mountAnim->getBoneInitialWorldPosition("Bip01").y;
                float backInitY  = mountAnim->getBoneInitialWorldPosition(seat.backBone).y;
                anchor.y = rootPos.y + (backInitY - rootInitY);
            }
            // 卷缩者: keep the root horizontal, but pin the vertical to the live NECK bone
            // so the seat rides up and down with the neck (DampSeatBob is skipped for it).
            if (seat.neckFollow && mountAnim->getHasBone("Bip01 Neck"))
                anchor.y = mount->getBoneWorldPosition("Bip01 Neck").y;
        }
    }
    else if (seat.seatMode == SEAT_NECK)
    {
        // highest point of the body: the rider starts above the huge torso
        // and is lowered with the tuning keys
        if (mount->getAnimationClass() && mount->getAnimationClass()->getHasBone("Bip01 Neck"))
            anchor = mount->getBoneWorldPosition("Bip01 Neck");
        else if (!seat.rearBone.empty())
        {
            Ogre::Vector3 rear = mount->getBoneWorldPosition(seat.rearBone);
            // ghost read guard: fall back to the plain back-bone anchor otherwise
            if (RearBoneReadSane(mount, seat, rear))
                anchor = (mount->getBoneWorldPosition(seat.frontBone) + rear) * 0.5f;
        }
    }
    else if (seat.seatMode == SEAT_MIDPOINT)
    {
        if (!seat.rearBone.empty() && seat.rearBone != seat.frontBone)
        {
            Ogre::Vector3 rear = mount->getBoneWorldPosition(seat.rearBone);
            // ghost read guard: keep the plain back-bone anchor otherwise
            if (RearBoneReadSane(mount, seat, rear))
                anchor = (mount->getBoneWorldPosition(seat.frontBone) + rear) * 0.5f;
        }
        // root-only skeletons fall through to the back-bone anchor
    }
    // SEAT_EXACT keeps the back-bone anchor

    Ogre::Vector3 pos = anchor + seat.lift;
    // user-tuned offsets (seat.sizeScale is fixed 1.0 since the per-individual size
    // scaling was removed - kept as a multiplier so the plumbing stays)
    if (seat.userOffset != Ogre::Vector3::ZERO)
    {
        Ogre::Vector3 fwd = GetMountForward(seat, mount);
        pos += fwd * (seat.userOffset.x * seat.sizeScale)
             + Ogre::Vector3(0.0f, seat.userOffset.y * seat.sizeScale, 0.0f);
    }
    if (seat.lateral != 0.0f)
    {
        // side axis: perpendicular to the mount's forward, on the ground plane
        Ogre::Vector3 fwd = GetMountForward(seat, mount);
        Ogre::Vector3 side = Ogre::Vector3(0.0f, 1.0f, 0.0f).crossProduct(fwd);
        if (side.length() < 0.001f)
            side = Ogre::Vector3(1.0f, 0.0f, 0.0f);
        else
            side.normalise();
        pos += side * (seat.lateral * seat.sizeScale);
    }
    return pos;
}

static void ClampTuning(Ogre::Vector3& v)
{
    if (v.x < -kTuningClamp) v.x = -kTuningClamp;
    if (v.x >  kTuningClamp) v.x =  kTuningClamp;
    if (v.y < -kTuningClamp) v.y = -kTuningClamp;
    if (v.y >  kTuningClamp) v.y =  kTuningClamp;
}

// Orient the rider's scene node after the game's own update.  The carry physics pins
// the rider horizontally (lying flat, belly up); we override the render-node
// orientation so the seated pose is drawn upright.  Facing is ALWAYS the mount's
// direction of travel - never a head/body bone, never a manual tune (dropped
// 2026-08-23); the node is world-space so no bone-local conversion is needed.
static void ApplyRiderOrientation(Character* rider, const SeatInfo& seat, Character* mount)
{
    if (!rider || !mount) return;
    AnimationClass* rAnim = rider->getAnimationClass();
    if (!rAnim || !rAnim->node) return;
    AnimationClass* mountAnim = mount->getAnimationClass();
    if (!mountAnim) return;

    // Heading = frame-to-frame horizontal delta of the mount's pathfinding position:
    // while moving that IS "the way it is going", and it ignores head/back bones
    // swivelling mid-run.  Nearly stationary -> HOLD last heading; never moved yet ->
    // fall back to body-bone forward.
    static const float kHeadingMoveEps = 0.03f;   // per-frame horizontal move to count as "traveling"
    Ogre::Vector3 fwd(0.0f, 0.0f, 1.0f);
    bool haveFwd = false;
    CharMovement* mv = mount->getMovement();
    if (mv)
    {
        Ogre::Vector3 cur = mv->getPosition();
        boost::unordered_map<Character*, Ogre::Vector3>::iterator lp = mountHeadingPos.find(mount);
        if (lp != mountHeadingPos.end())
        {
            Ogre::Vector3 d = cur - lp->second;
            d.y = 0.0f;
            if (d.length() > kHeadingMoveEps)   // moving: refresh the travel heading
            {
                d.normalise();
                mountHeadingDir[mount] = d;
            }
        }
        mountHeadingPos[mount] = cur;
        boost::unordered_map<Character*, Ogre::Vector3>::iterator hd = mountHeadingDir.find(mount);
        if (hd != mountHeadingDir.end()) { fwd = hd->second; haveFwd = true; }
    }
    if (!haveFwd)
        fwd = GetMountForward(seat, mount);   // never traveled yet: body-bone forward
    fwd.y = 0.0f;
    if (fwd.length() < 0.001f)
        fwd = Ogre::Vector3(0.0f, 0.0f, 1.0f);
    else
        fwd.normalise();

    // Build a world orientation that makes the rider face the mount's forward.
    // FromAxes maps local +Z to its third argument (worldFace).  In-game testing
    // showed the human sitting skeleton's forward is +Z (not -Z as the old code
    // assumed), so local +Z must point ALONG fwd - hence worldFace = +fwd.  (Setting
    // it to -fwd left every rider facing exactly backwards.)
    Ogre::Vector3 worldUp(0.0f, 1.0f, 0.0f);
    Ogre::Vector3 worldFace = fwd;
    Ogre::Vector3 worldRight = worldUp.crossProduct(worldFace);
    if (worldRight.length() < 0.001f)
        worldRight = Ogre::Vector3(1.0f, 0.0f, 0.0f);
    else
        worldRight.normalise();

    Ogre::Quaternion worldQ;
    worldQ.FromAxes(worldRight, worldUp, worldFace);

    // Low-pass the heading (nlerp, short path) so a turning mount - and the back-bone
    // swing on fling skeletons - doesn't snap the rider's facing frame to frame.
    boost::unordered_map<Character*, Ogre::Quaternion>::iterator so = mountSmoothOrient.find(mount);
    if (so == mountSmoothOrient.end())
        mountSmoothOrient[mount] = worldQ;
    else
    {
        if (so->second.Dot(worldQ) < 0.0f)
            worldQ = -worldQ;
        Ogre::Quaternion q = so->second + (worldQ - so->second) * 0.35f;
        q.normalise();
        mountSmoothOrient[mount] = q;
        worldQ = q;
    }

    // The rider node is not parented to the mount bone (no slaveAttachToBoneMode), so
    // the node orientation IS world space - apply worldQ directly.
    rAnim->node->setOrientation(worldQ);
}

// ---- put the rider's RENDER position on the mount's back --------------------
//
// The renderer does NOT read the movement position or rootBonePosition - it reads the
// rider's root bone world position (rBip), which satisfies with zero residual
// (RE_NOTES section 5):
//
//     rBip = node + nodeQ * boneLocal        (boneLocal ~6.4, constant per pose)
//
// Writing the node straight to seatPos therefore floats the rider ~6.4 above the
// seat AND swings that offset around as nodeQ rotates mid-run.  Solve for the node
// that puts rBip on the seat instead:
//
//     node = (seatPos + anchor) - nodeQ * boneLocal
//
// `anchor` is captured once per mount on the first synced frame as (rBip - node),
// preserving the height the riding.cfg tuning was calibrated against; boneLocal is
// recomputed every frame so a posture switch (Numpad 7) self-corrects in one frame.
static void SyncRiderNode(Character* rider, Character* mount, AnimationClass* rAnim,
                          const Ogre::Vector3& seatPos, bool mainPhase)
{
    if (!rider || !mount || !rAnim || !rAnim->node) return;

    Ogre::Vector3    nodeP = rAnim->getSceneNodePosition();
    Ogre::Quaternion nodeQ = rAnim->getSceneNodeOrientation();
    Ogre::Vector3    rBip  = rAnim->getBoneWorldPosition("Bip01", 1.0f);

    // root bone offset expressed in the node's frame (constant per pose)
    Ogre::Vector3 boneLocal = nodeQ.Inverse() * (rBip - nodeP);

    // The pose offset rBip-node for a playable character is small (~6.4 sitting).
    // Right after pickupObject / right after a load the rider is still blending out
    // of the carried/bind pose, so rel reads TRANSIENT values there (2026-08-23:
    // anchor frozen at 9.3 while the settled sitting relation is ~6.5 -> rider
    // permanently mis-seated after every save/load).  A value is only captured once
    // it has held steady for kCaptureStableNeed syncs, and a stored anchor is
    // re-validated against firmly-settled live readings - that heals both poison
    // and staleness (a Numpad-7 posture switch changes the pose constant too).
    //
    // Capture/heal bookkeeping happens ONLY at the main-loop sync: the raw relation
    // read mid-animation-phase differs by >1.5u between call sites (bones lag node
    // writes until the scene flush), which first kept rs at 0 forever, then - once
    // every character's update re-synced - made capture/stale ping-pong rewrite the
    // cfg several times a second.  One consistent measurement point fixes both.
    static const float kRelStableTol    = 0.35f;
    static const float kAnchorStaleDiff = 1.5f;
    Ogre::Vector3 rel = rBip - nodeP;

    if (mainPhase)
    {
        CapTrack& ct = mountCap[mount];
        ct.relStable = ((rel - ct.prevRel).length() < kRelStableTol) ? ct.relStable + 1 : 0;
        ct.prevRel = rel;

        boost::unordered_map<Character*, Ogre::Vector3>::iterator ai = mountAnchor.find(mount);
        if (ai != mountAnchor.end())
        {
            bool poisoned = ai->second.length() > kMaxAnchorLen;
            bool stale = (ct.relStable >= kCaptureStableNeed)
                         && (ai->second - rel).length() > kAnchorStaleDiff;
            if (poisoned || stale)
            {
                // drop it AND the bob baseline captured in the same corrupted instant;
                // both recapture from settled frames
                mountAnchor.erase(ai);
                mountBaseVOffset.erase(mount);
                ct.baseStable = 0;
                try { DebugLog(std::string("Riding: anchor recapture (") + (poisoned ? "poisoned" : "stale") + ")"); } catch(...) {}
            }
        }
        if (mountAnchor.find(mount) == mountAnchor.end()
            && rel.length() <= kMaxAnchorLen && ct.relStable >= kCaptureStableNeed)
        {
            mountAnchor.insert(std::make_pair(mount, rel));
            // Persist so every future mount/load SEEDS this value instead of re-capturing
            // through the pose storm ("remember, don't re-derive").
            boost::unordered_map<Character*, SeatInfo>::iterator ssi = mountSeat.find(mount);
            if (ssi != mountSeat.end() && !ssi->second.species.empty())
            {
                speciesTuning[ssi->second.species].anchor = rel;
                SaveConfig();
                try { DebugLog("Riding: anchor captured+saved [" + ssi->second.species + "]"); } catch(...) {}
            }
        }
    }

    // Until capture succeeds, use the LIVE offset so this frame still lands sensibly.
    // With anchor = live rel the solved node lands exactly ON seatPos - i.e. the
    // original direct node placement the riding.cfg tuning was calibrated against -
    // so deferring costs no jump once capture completes.  The live fallback must stay
    // CLAMPED, otherwise a stretched post-load pose feeds an unbounded self-
    // referential offset back into the placement (seen 2026-08-23 as constant ~74 drift).
    Ogre::Vector3 anchor;
    boost::unordered_map<Character*, Ogre::Vector3>::iterator af = mountAnchor.find(mount);
    if (af != mountAnchor.end())
        anchor = af->second;
    else
    {
        anchor = rel;
        if (anchor.length() > kMaxAnchorLen)
            anchor *= kMaxAnchorLen / anchor.length();
    }

    Ogre::Vector3 target = seatPos + anchor;
    // Servo removed 2026-08-24: it existed to pre-cancel the engine's ragdoll-carry
    // slot pin, which no longer exists - Mount() dissolves the carry link right after
    // pickupObject (dropCarriedObject removeOnly).  With no pinner, the measured
    // "drift" was ordinary physics displacement (gravity/ground snap) and pre-
    // subtracting it inflated the write height every frame until the clamp - the
    // free rider hovered above the back (Numpad8 probe, 2026-08-24).
    Ogre::Vector3 written = target - nodeQ * boneLocal;
    rAnim->node->setPosition(written);
    rAnim->rootBonePosition = seatPos;
    // DBG: remember what we wrote - a later read-back that differs from this proves
    // an engine writer re-positioned the carried rider after us (who-writes-last race).
    dbgNodeWritten[rider] = written;
}

// Shrink the vertical run-cycle bob of the seat.  The seat anchor bone (neck/spine)
// rises and falls as the animal runs, so the bone-anchor height oscillates.  We keep a
// stable baseline = the mount's ROOT bone Y (which translates smoothly, no bob) plus
// the constant seat-above-root height captured on the first frame, then blend the live
// bone-anchor height toward that baseline: kept fraction = kSeatBobScale.
//
//   1.0 -> full natural bob,  0.0 -> perfectly flat ride.
//
// IMPORTANT: the user's live height tuning (seat.userOffset.y, the +/- keys) is
// EXCLUDED from the damping - it is stripped off before scaling and added back at full
// strength after, otherwise each +/- step would only move the rider by kSeatBobScale
// (~15%) of its intended amount.  Only the bone bob is damped, never the user's height.
//
// Stateless/idempotent per frame (starts from the freshly computed seatPos), so calling
// it once or several times per frame gives the same result.  The captured baseline
// assumes lift is constant; CycleSeatMode changes lift, so it clears mountBaseVOffset.
static const float kSeatBobScale = 0.15f;
static void DampSeatBob(Character* mount, const SeatInfo& seat, Ogre::Vector3& seatPos)
{
    if (!mount) return;
    // 卷缩者: intentionally follow the neck bob at full strength (no damping) so the
    // rider's butt stays glued to the neck as it rises and falls.  ComputeSeatPosition
    // already set seatPos.y to the neck bone Y; leave it untouched.
    if (seat.neckFollow) return;
    AnimationClass* mAnim = mount->getAnimationClass();
    if (!mAnim || !mAnim->getHasBone("Bip01")) return;   // no root -> leave as-is

    // Stable vertical reference.  Normally the ROOT bone Y is smooth enough, but the
    // fling skeletons (rootAnchor: Bonedog / Beak Thing / Crimper / ...) throw their
    // ROOT bone up and down heavily during the run/curl cycle, so keying the baseline
    // to the root bone lets the whole seat drop to the belly when they run.  For those
    // species use the MOVEMENT (pathfinding) position Y instead - it follows terrain
    // but carries no per-step skeletal bob, so the damped baseline stays flat and the
    // full root throw lands in (baseY - stableY) where kSeatBobScale cuts it to 15%.
    float refY;
    if (seat.rootAnchor && mount->getMovement())
        refY = mount->getMovement()->getPosition().y;
    else
        refY = mount->getBoneWorldPosition("Bip01").y;

    // strip the user's height tune so only the bone-anchor bob gets damped
    float uy      = seat.userOffset.y;
    float baseY   = seatPos.y - uy;
    float rawBase = baseY - refY;   // seat height above the reference (bob-free mean, no user tune)

    // Same settle-gate as the SyncRiderNode anchor: right after mount/load rawBase is
    // transient (pose blending, ghost bones); freezing THEN baked a wrong baseline
    // into every later frame (persistent "rider sits too low" after loading a save).
    // Capture only once rawBase has held steady, and re-validate a stored baseline
    // against firmly-settled readings.  Until captured, damping is skipped entirely -
    // the ride bobs naturally for a moment instead of being pulled toward a wrong
    // constant.  While the mount is RUNNING the bob itself keeps the counter from
    // accumulating, so recapture only fires when readings are genuinely settled.
    static const float kBaseStableTol     = 0.35f;
    static const float kBaselineStaleDiff = 3.0f;
    CapTrack& ct = mountCap[mount];
    ct.baseStable = (fabsf(rawBase - ct.prevBase) < kBaseStableTol) ? ct.baseStable + 1 : 0;
    ct.prevBase = rawBase;

    boost::unordered_map<Character*, float>::iterator vo = mountBaseVOffset.find(mount);
    if (vo != mountBaseVOffset.end() && fabsf(vo->second) > 25.0f)
    {
        // poisoned baseline (captured on a ghost-bone frame): drop it and recapture
        // below from this frame's values
        mountBaseVOffset.erase(mount);
        vo = mountBaseVOffset.end();
    }
    if (vo != mountBaseVOffset.end()
        && ct.baseStable >= kCaptureStableNeed
        && fabsf(rawBase - vo->second) > kBaselineStaleDiff)
    {
        // stale: stored baseline disagrees with a settled reading (captured mid-blend)
        mountBaseVOffset.erase(mount);
        vo = mountBaseVOffset.end();
    }
    if (vo == mountBaseVOffset.end())
    {
        if (ct.baseStable >= kCaptureStableNeed)
        {
            mountBaseVOffset[mount] = rawBase;   // settled -> begin damping next sync
            // Persist once per ride so future mounts/loads seed it directly instead of
            // live-capturing through the post-mount/post-load pose storm.
            boost::unordered_map<Character*, SeatInfo>::iterator ssi = mountSeat.find(mount);
            if (ssi != mountSeat.end() && !ssi->second.species.empty())
            {
                speciesTuning[ssi->second.species].base = rawBase;
                SaveConfig();
                try { DebugLog("Riding: baseline captured+saved [" + ssi->second.species + "]"); } catch(...) {}
            }
        }
        return;                                  // nothing to damp yet (or still settling)
    }

    float stableY    = refY + vo->second;
    float dampedBase = stableY + (baseY - stableY) * kSeatBobScale;
    seatPos.y = dampedBase + uy;                   // user height tune applies at full strength
}

// Nothing to correct when the seat is a pure EXACT bone attach with no user tune -
// both per-frame sync paths skip rider placement for that configuration.
static bool SeatNeedsPlacement(const SeatInfo& seat)
{
    return !(seat.seatMode == SEAT_EXACT && seat.lift == Ogre::Vector3::ZERO && seat.userOffset == Ogre::Vector3::ZERO);
}

// Seat position shared by the two per-frame sync paths (animUpdate + main loop):
// horizontal instant from ComputeSeatPosition, vertical run-bob scaled by DampSeatBob.
// The two writers must agree every frame - the pass running closer to the render would
// otherwise visibly win.
static Ogre::Vector3 ComputeDampedSeatPos(Character* mount, const SeatInfo& seat)
{
    Ogre::Vector3 seatPos = ComputeSeatPosition(seat, mount);
    DampSeatBob(mount, seat, seatPos);
    return seatPos;
}

// Continuous diagnostics (Numpad .): log EVERY frame while the mount is moving,
// comparing the mount's root/back bones, the forward vector, the raw (unsmoothed)
// target, the damped target, and every position/orientation readout of the rider
// (scene node, movement pos, root bone, world rBip).  Called from the main-loop
// sync pass right after seatPos is final for the frame.
static void DebugLogRideFrame(Character* rider, Character* mount, const SeatInfo& seat, const Ogre::Vector3& seatPos)
{
    CharMovement* dmv = mount->getMovement();
    Ogre::Vector3 dmvP = dmv ? dmv->getPosition() : Ogre::Vector3::ZERO;
    boost::unordered_map<Character*, Ogre::Vector3>::iterator dlast = debugLastPos.find(mount);
    bool moving = false;
    if (dlast != debugLastPos.end())
    {
        Ogre::Vector3 dd = dmvP - dlast->second;
        dd.y = 0.0f;
        moving = (dd.length() > 0.02f);
    }
    debugLastPos[mount] = dmvP;
    if (!moving)
        return;

    AnimationClass* mAnim = mount->getAnimationClass();
    AnimationClass* rAnim = rider->getAnimationClass();
    if (mAnim && rAnim)
    {
        Ogre::Vector3 rootP = mAnim->getBoneWorldPosition("Bip01", 1.0f);
        Ogre::Vector3 backP = mAnim->getBoneWorldPosition(seat.backBone, 1.0f);
        Ogre::Vector3 fwd = GetMountForward(seat, mount);
        Ogre::Vector3 rawT = ComputeSeatPosition(seat, mount);
        // ghost-bone watch: the raw rear-bone read + which anchor is active
        Ogre::Vector3 pelvP = seat.rearBone.empty() ? Ogre::Vector3::ZERO
                            : mount->getBoneWorldPosition(seat.rearBone);
        int anchSrc = 0;    // 0 = fallback (live rel), 1 = stored anchor
        Ogre::Vector3 anchP = Ogre::Vector3::ZERO;
        boost::unordered_map<Character*, Ogre::Vector3>::iterator da = mountAnchor.find(mount);
        if (da != mountAnchor.end())
        {
            anchSrc = 1;
            anchP = da->second;
        }
        Ogre::Vector3 nodeP = rAnim->getSceneNodePosition();
        Ogre::Quaternion nodeQ = rAnim->getSceneNodeOrientation();
        // The CHARACTER's own transform - this is what actually renders
        Ogre::Vector3 riderMoveP = rider->getMovement() ? rider->getMovement()->getPosition() : Ogre::Vector3::ZERO;
        Ogre::Vector3 riderRoot = rAnim->rootBonePosition;
        // The RIDER's own root bone in world space (the renderer's true source)
        Ogre::Vector3 riderBip01 = rider->getBoneWorldPosition("Bip01");
        Ogre::Quaternion riderBip01Q = rAnim->getBoneWorldOrientation("Bip01");
        // capture-gate state: raw pose relation + how settled anchor/baseline are
        Ogre::Vector3 relDbg = riderBip01 - nodeP;
        int rsDbg = 0, bsDbg = 0;
        boost::unordered_map<Character*, CapTrack>::iterator ci = mountCap.find(mount);
        if (ci != mountCap.end()) { rsDbg = ci->second.relStable; bsDbg = ci->second.baseStable; }
        // drift since OUR last node write: nonzero = an engine writer re-positioned
        // the rider's scene node after us (who-writes-last race meter)
        Ogre::Vector3 wnDbg(0.0f, 0.0f, 0.0f);
        boost::unordered_map<Character*, Ogre::Vector3>::iterator wi = dbgNodeWritten.find(rider);
        if (wi != dbgNodeWritten.end()) wnDbg = nodeP - wi->second;
        // ragdoll/carry state of the RIDER, per frame.  Needed because the Numpad8 probe
        // can only read the flags in the same frame it calls ragdollModeUT, and a same-
        // frame read is inconclusive when a call lands deferred (exactly how carryModeT
        // behaved).  With these fields a probe press shows up as a 1->0 transition on a
        // later frame - or proves the flag is being re-asserted every frame.
        int ragDbg  = rider->isRagdoll() ? 1 : 0;
        int aRagDbg = rAnim->isRagdoll() ? 1 : 0;
        int bcDbg   = rider->_isBeingCarried ? 1 : 0;
        char dbg[1400];
        _snprintf_s(dbg, 1400, _TRUNCATE,
            "Riding: DBG root=(%.2f,%.2f,%.2f) back=(%.2f,%.2f,%.2f) fwd=(%.2f,%.2f,%.2f) rawT=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f) node=(%.2f,%.2f,%.2f) rMove=(%.2f,%.2f,%.2f) rRoot=(%.2f,%.2f,%.2f) rBip=(%.2f,%.2f,%.2f) nodeQ=(%.2f,%.2f,%.2f,%.2f) rBipQ=(%.2f,%.2f,%.2f,%.2f) move=(%.2f,%.2f,%.2f) anch=(%.2f,%.2f,%.2f) st=%d pelv=(%.2f,%.2f,%.2f) rel=(%.2f,%.2f,%.2f) rs=%d bs=%d wn=(%.2f,%.2f,%.2f) rag=%d aRag=%d bc=%d",
            rootP.x, rootP.y, rootP.z,
            backP.x, backP.y, backP.z,
            fwd.x, fwd.y, fwd.z,
            rawT.x, rawT.y, rawT.z,
            seatPos.x, seatPos.y, seatPos.z,
            nodeP.x, nodeP.y, nodeP.z,
            riderMoveP.x, riderMoveP.y, riderMoveP.z,
            riderRoot.x, riderRoot.y, riderRoot.z,
            riderBip01.x, riderBip01.y, riderBip01.z,
            nodeQ.w, nodeQ.x, nodeQ.y, nodeQ.z,
            riderBip01Q.w, riderBip01Q.x, riderBip01Q.y, riderBip01Q.z,
            dmvP.x, dmvP.y, dmvP.z,
            anchP.x, anchP.y, anchP.z,
            anchSrc,
            pelvP.x, pelvP.y, pelvP.z,
            relDbg.x, relDbg.y, relDbg.z,
            rsDbg, bsDbg,
            wnDbg.x, wnDbg.y, wnDbg.z,
            ragDbg, aRagDbg, bcDbg);
        DebugLog(dbg);
    }
}

// Persist a seat's full tuning state to its species entry + riding.cfg.  Writes ALL
// five fields (not just the one a hotkey just changed): the seat IS the live mountSeat
// entry, so every field is current.  Accepted trade-off (2026-08-23): two mounts of the
// same species tuned in interleaved steps overwrite each other's cfg copy - identical to
// what the old full-field writers (up/fwd tune, reset) already did.
static void PersistTuning(const SeatInfo& seat)
{
    if (!seat.species.empty())
    {
        SpeciesTuning& st = speciesTuning[seat.species];
        st.seatMode = seat.seatMode;
        st.forceSit = seat.forceSit;
        st.posture = seat.posture;
        st.lateral = seat.lateral;
        st.offset = seat.userOffset;
    }
    SaveConfig();
}

// Apply a live tuning step to the currently mounted seat and persist it.
static void TuneSeat(SeatInfo& seat, float dUp, float dFwd)
{
    seat.userOffset.x += dFwd;
    seat.userOffset.y += dUp;
    ClampTuning(seat.userOffset);

    PersistTuning(seat);

    DebugLog("Riding: tuned " + seat.species + " up=" + IntToStr((int)(seat.userOffset.y * 100.0f))
             + " fwd=" + IntToStr((int)(seat.userOffset.x * 100.0f)));
}

// Cycle the seat mode of the currently mounted animal (exact -> midpoint -> neck).
static void CycleSeatMode(SeatInfo& seat)
{
    seat.seatMode = (seat.seatMode + 1) % 3;
    if (seat.seatMode < SEAT_EXACT) seat.seatMode = SEAT_EXACT;
    if (seat.seatMode > SEAT_NECK)  seat.seatMode = SEAT_NECK;

    // recompute the base lift for the new mode
    if (seat.seatMode == SEAT_MIDPOINT)
    {
        if (!seat.rearBone.empty() && seat.rearBone != seat.frontBone)
            seat.lift = Ogre::Vector3(0.0f, seat.torsoLen * kSeatLiftRatio, 0.0f);
    }
    else if (seat.seatMode == SEAT_NECK)
    {
        seat.lift = Ogre::Vector3::ZERO;
    }
    // SEAT_EXACT: lift stays whatever the EXACT base was (recompute from bones would
    // need the mount, keep current - the user tunes on top anyway)

    PersistTuning(seat);

    DebugLog("Riding: mode " + seat.species + " -> " + IntToStr(seat.seatMode));
}

// Seed persisted per-species constants into a fresh ride's maps so placement is
// correct from frame one - mounting or restoring after a load no longer depends on
// live-capturing through the post-mount/post-load pose storm.  Species never ridden
// before simply have nothing to seed; the live-capture path remains their fallback.
static void SeedPersistedConstants(Character* mount)
{
    boost::unordered_map<Character*, SeatInfo>::iterator si = mountSeat.find(mount);
    if (si == mountSeat.end() || si->second.species.empty()) return;
    boost::unordered_map<std::string, SpeciesTuning>::iterator ti = speciesTuning.find(si->second.species);
    if (ti == speciesTuning.end()) return;
    if (ti->second.anchor.length() > 0.01f && ti->second.anchor.length() <= kMaxAnchorLen)
        mountAnchor.insert(std::make_pair(mount, ti->second.anchor));
    if (fabsf(ti->second.base) > 0.001f)
        mountBaseVOffset.insert(std::make_pair(mount, ti->second.base));
    try { DebugLog("Riding: seeded constants [" + si->second.species + "] anchor=" + IntToStr((int)(ti->second.anchor.length() * 100.0f))
                   + " base=" + IntToStr((int)(ti->second.base * 100.0f))); } catch(...) {}
}

void Mount(Character* rider, Character* mount)
{
    if (!rider || !mount) return;
    if (rider == mount) return;
    // Only the player's OWN animals can be ridden (animal + in the player's party).
    // IsRideable = isAnimal() && isWithThePlayer().  This is the single chokepoint all
    // mount paths (menu, Numpad1) funnel through, so wild/other-faction animals are
    // blocked here regardless of how the mount was requested.
    if (!IsRideable(mount)) { DebugLog("Riding: mount rejected - not a player-owned animal"); return; }
    if (IsRiding(rider)) return;

    SeatInfo seat = BuildSeatInfo(mount);

    // 1) Have the animal carry the rider using Kenshi's native carry system.
    //    This provides auto-follow + no physics collision fighting.
    try { DebugLog(std::string("Riding: ragdoll probe pre-pickup=")
        + IntToStr(rider->isRagdoll() ? 1 : 0)); } catch(...) {}
    mount->pickupObject(rider);
    // Probes (2026-08-23/24): pickupObject ALWAYS ragdolls the rider (pre=0/post=1)
    // and the ragdoll-carry drag ABSOLUTELY pins the rider's node to a carrier-local
    // slot every frame - our writes can never win while it is active.  The Numpad8
    // probe also showed carryModeT(true,false,false) stops the drag but dissolves
    // the whole carry link anyway (carrying/beingCarried -> 0) and leaves the mount
    // stuck in a carry pose - so native carry is all-or-nothing.  Dissolve the link
    // cleanly right away (removeOnly = no ragdoll fling) and own placement outright.
    //
    // CAUTION (2026-08-24, log-proven): dropping the link does NOT end the ragdoll
    // pickupObject started.  The "carry dissolved:" line below reports rag=1 every
    // single mount, and from the second mount on the "pre-pickup=" probe above also
    // reads 1 - the ragdoll survives Dismount and accumulates across rides.  The
    // sitting pose is only a per-frame cover over it (AnimUpdateImpl's forcedSlaveLoop
    // plus HaltAndForceSitPass), which is why the ride looks right but Dismount's
    // endSlaveAnim uncovers a live ragdoll and the rider collapses, and why a mounted
    // rider's context menu offers nothing but "knock down".  Candidate teardown is
    // isolated in the Numpad8 probe (see HotkeyPass) until it is validated in-game.
    mount->dropCarriedObject(false, true);
    // NOTE (2026-08-24): pickupObject ragdolls the rider and it CANNOT be cleared while
    // mounted - DBG proved rag=1/aRag=1 stays for the whole ride even after an immediate
    // ragdollModeUT(false) here (Path A, reverted).  The ragdoll is a consequence of the
    // carry state (_isBeingCarried=1 re-applies it every frame), and _isBeingCarried must
    // stay 1 during the ride or the mount's collision volume shoves the rider off.  The
    // sitting-pose cover hides the ragdoll, so the ride looks correct.  Ragdoll teardown
    // (with the QUEUED recovery so the rider stands up) is done at Dismount instead, where
    // the rider is leaving the mount anyway - see Dismount().
    try { DebugLog(std::string("Riding: carry dissolved: carrying=")
        + IntToStr(mount->isCarryingSomething ? 1 : 0)
        + " beingCarried=" + IntToStr(rider->_isBeingCarried ? 1 : 0)
        + " rag=" + IntToStr(rider->isRagdoll() ? 1 : 0)
        + " down=" + IntToStr(rider->isDown() ? 1 : 0)); } catch(...) {}

    // 2) Do NOT slave-attach the rider's scene node to the mount's back bone.
    //    Attaching re-parents the node under the bone, so the engine re-pins it every
    //    frame from the (swinging) bone transform and our world-space placement fights
    //    it.  SyncRiderNode below owns the rider's node position outright instead, and
    //    it solves rBip = node + nodeQ*boneLocal so the RENDERED rider lands on the
    //    back.  rootAnchor species already skipped the attach and sit correctly, and
    //    runSlaveAnim() works without a slave handle (proven by those species), so the
    //    attach is dropped for every mount.

    // 3) Play the native sitting animation so the rider sits upright on the back.
    //    "sitting chair" is the toilet-sitting pose the player confirmed.
    rider->runSlaveAnim("sitting chair", 1.0f, 1.0f);

    mountSeat[mount] = seat;

    riderToMount[rider] = mount;
    mountToRider[mount] = rider;
    SeedPersistedConstants(mount);   // frame-one placement from riding.cfg constants

    if (rider->getMovement())
        rider->getMovement()->halt();

    DebugLog("Riding: mounted [" + seat.species + "] mode=" + IntToStr(seat.seatMode)
             + (seat.forceWalk ? " walk=1" : " walk=0")
             + " bone=" + seat.backBone
             + " torso=" + IntToStr((int)(seat.torsoLen * 10.0f))
             + " lift=" + IntToStr((int)(seat.lift.y * 100.0f))
             + " size=" + IntToStr((int)(seat.sizeScale * 100.0f))
             + " tune=" + IntToStr((int)(seat.userOffset.y * 100.0f)) + "/" + IntToStr((int)(seat.userOffset.x * 100.0f)));
}

void Dismount(Character* rider)
{
    if (!rider) return;

    // stop the sitting animation
    rider->endSlaveAnim("sitting chair");

    // THE put-down (2026-08-24 rev 6, confirmed in-game).  Mount() only severs the CARRIER's
    // side of the carry link (dropCarriedObject with removeOnly), which leaves the rider stuck
    // in carried-state - and for the carried side pickupObject had DESTROYED its CharMovement
    // (pulled it out of the physics world).  A destroyed movement still honours
    // _setPositionSimple (logical pos + render, which is why the rider looks properly seated)
    // but never simulates: no gravity, no ground, no move orders, and isDestinationReached()
    // answers true forever.  That one fact caused all three symptoms we chased - the ex-rider
    // hanging frozen in mid-air, ignoring click-to-move, and "boarding from any distance".
    // getDropped is the engine's own carried-side handler, the exact inverse of the
    // getPickedUp that pickupObject triggered: it clears carried-state, restore()s the
    // movement (back into the physics world), grounds the body, and with ragdollHim=false
    // stands the rider up.  It must run FIRST, while _isBeingCarried is still 1 - that is the
    // state it tears down - hence before the flag is cleared below.
    //
    // hull=true was picked by A/B (Numpad4 true vs Numpad8 false, same session): both left the
    // rider standing and controllable, but hull=false first yanked the body ~19u ABOVE the
    // mount before it fell (dMv 10.35 -> 18.96 -> 1.01), while hull=true descended straight to
    // the ground (dMv 10.35 -> 9.25 -> -0.85).  No pop, closer settle.
    if (rider->_isBeingCarried)
        rider->getDropped(false, true);
    else
        DebugLog("Riding: dismount without carried-state (skipped getDropped)");

    // Clear the carried-body flag.  Mount() leaves _isBeingCarried=1 on purpose (it makes
    // the separation pass skip the rider so the mount's collision volume can't shove them
    // off), and dropCarriedObject(...,removeOnly) never touches the rider's side, so it
    // stays 1 for the whole ride.  getDropped above normally clears it already; this is the
    // belt-and-suspenders for the path where it was skipped, since a lingering 1 keeps the
    // order system treating the rider as an incapacitated carried object (context menu stuck
    // on "knock down").  Safe here: no carrier means no collision volume to be pushed by.
    rider->_isBeingCarried = false;

    // Clear the pickup/carry ragdoll.  pickupObject ragdolls the rider on the CARRY_MODE part
    // (0x800) - NOT WHOLE (0x1), which is why every earlier teardown attempt was a no-op - and
    // during the ride it is merely hidden under the per-frame sitting-pose cover.  Once
    // endSlaveAnim above drops that cover a leftover ragdoll shows as the rider collapsing
    // limp.  This is the queued Character-level call, so it also runs postRagdollCallback's
    // get-up recovery; the log shows chRag going 1 -> 0 on the following frame.  Normally
    // redundant after getDropped(ragdollHim=false), kept as the safety net.
    rider->ragdollMode(false, RagdollPart::CARRY_MODE);

    boost::unordered_map<Character*, Character*>::iterator it = riderToMount.find(rider);
    if (it != riderToMount.end())
    {
        Character* mount = it->second;
        // tell the mount to drop the rider (no ragdoll, no hull).  Since 2026-08-24
        // Mount() dissolves the carry link right after pickupObject, this is normally
        // a no-op - guarded so we never call teardown on an already-empty link.
        if (mount && mount->isCarryingSomething)
            mount->dropCarriedObject(false, false);
        // stop the walk/run animations we force-played on pack_beast mounts while
        // ridden, so the mount's own animation selection takes back over afterwards.
        if (mount)
        {
            AnimationClass* mountAnim = mount->getAnimationClass();
            if (mountAnim)
            {
                mountAnim->stopAnimation("beast walk");
                mountAnim->stopAnimation("beast run");
            }
        }
        mountToRider.erase(mount);
        riderToMount.erase(it);
        mountSeat.erase(mount);
        mountLastPos.erase(mount);
        mountBaseVOffset.erase(mount);
        mountSmoothOrient.erase(mount);
        mountHeadingPos.erase(mount);
        mountHeadingDir.erase(mount);
        mountAnchor.erase(mount);
        mountCap.erase(mount);
        debugLastPos.erase(mount);
        dbgNodeWritten.erase(rider);
    }

    DebugLog("Riding: dismounted");
}

// ---- rebuild riding state after a save/load ----------------------------------
// Kenshi's native carry link (what pickupObject set up) IS persisted in saves and
// restored on load - but this DLL's in-memory maps (who rides whom, seat setup,
// anchors) start empty every session.  Without a rebuild a loaded-in rider stays
// engine-carried forever: no per-frame sync (position garbage) and every dismount
// path refuses (IsRiding == false).  animUpdate_hook spots an untracked carrying
// animal and re-forms the pair with the exact same steps as a fresh Mount(),
// minus pickupObject (the carry link already exists).
void RestoreRideAfterLoad(Character* rider, Character* mount)
{
    SeatInfo seat = BuildSeatInfo(mount);

    rider->runSlaveAnim("sitting chair", 1.0f, 1.0f);
    mountSeat[mount] = seat;
    riderToMount[rider] = mount;
    mountToRider[mount] = rider;
    if (rider->getMovement())
        rider->getMovement()->halt();

    // stale per-mount caches cannot exist across a reload, but erase anyway to stay
    // symmetric with Dismount() in case a restore ever overwrites a live pair
    mountAnchor.erase(mount);
    mountCap.erase(mount);
    mountBaseVOffset.erase(mount);
    mountLastPos.erase(mount);   // was missed before 2026-08-23: one stale forceWalk tick after load
    mountSmoothOrient.erase(mount);
    mountHeadingPos.erase(mount);
    mountHeadingDir.erase(mount);
    debugLastPos.erase(mount);
    dbgNodeWritten.erase(rider);
    SeedPersistedConstants(mount);   // frame-one placement from riding.cfg constants

    DebugLog("Riding: restored ride after load [" + seat.species + "] mode="
             + IntToStr(seat.seatMode));
}

// Called from animUpdate_hook for every animated character reporting
// isCarryingSomething; the early-outs make it free for normal play.
//
// NOTE: deliberately NOT gated on IsRideable - the blocklist (beak apes) must not
// trap someone who saved while riding one.  Ownership is enough to RESTORE a pair;
// only STARTING a new ride goes through the full IsRideable check in Mount().
void TryRestoreOrphanedMount(Character* carrier)
{
    // Right after a load the engine's carry link can be half-initialised for a few
    // frames - carryingObject.getCharacter() has returned garbage in practice.  So
    // plausibility-gate every pointer and do MEMBER reads before VIRTUAL calls
    // (try/catch does NOT catch access violations under /EHsc).
    if (!CharacterLooksLive(carrier)) return;
    if (mountToRider.find(carrier) != mountToRider.end()) return;   // already tracked
    if (!carrier->isAnimal()) return;                               // virtual, gated above
    try { if (!carrier->isWithThePlayer()) return; } catch (...) { return; }
    if (!carrier->isCarryingSomething) return;                      // member read (0x348)
    Character* rider = carrier->carryingObject.getCharacter();      // non-virtual import stub
    if (!CharacterLooksLive(rider) || riderToMount.find(rider) != riderToMount.end()) return;
    if (!rider->_isBeingCarried) return;                            // member read BEFORE isAnimal()
    if (rider->isAnimal()) return;                                  // riders are humans
    RestoreRideAfterLoad(rider, carrier);
}

// ---- right-click command hook ----
// Kenshi's right-click-hold menu for an animal includes "Bodyguard"
// (BODYGUARD) and, for a carried/mounted target, "Put Down"
// (PUT_DOWN_OBJECT). We repurpose these two:
//   - Bodyguard on an animal  -> mount a selected human onto it
//   - Put Down on a mounted rider -> dismount
// All other vanilla orders pass through untouched.

// Is this a player attack order we should redirect from a large-mount rider to the mount?
bool IsAttackTask(TaskType t)
{
    switch (t)
    {
    case MELEE_ATTACK:
    case FOCUSED_MELEE_ATTACK:
    case CHOOSE_ENEMY_AND_ATTACK:
    case CHOOSE_ATTACKER_OF_ALLY:
    case ATTACK_CHARACTERS_ATTACKER:
    case ATTACK_ATTACKERS_OF:
    case ATTACK_ENEMIES:
    case ATTACK_ENEMIES_AND_NEUTRALS:
    case ATTACK_TOWN:
    case ATTACK_TROUBLE_MAKERS:
    case MELEE_ATTACK_ANIMAL:
    case RANGED_ATTACK:
    case RANGED_ATTACK_FOCUSED:
        return true;
    default:
        return false;
    }
}

void (*newPlayerTask_orig)(PlayerInterface* thisptr, TaskType t, const hand& targetH, Building* destinationIndoors, const Ogre::Vector3& clickpos, bool addDontClear) = NULL;
void newPlayerTask_hook(PlayerInterface* thisptr, TaskType t, const hand& targetH, Building* destinationIndoors, const Ogre::Vector3& clickpos, bool addDontClear)
{
    Character* target = targetH.getCharacter();

    // "Bodyguard" on a PLAYER-OWNED animal -> mount a selected human.  For wild or
    // other-faction animals IsRideable is false, so we fall through and let the vanilla
    // Bodyguard order run untouched (we no longer swallow it).
    if (t == BODYGUARD && target && IsRideable(target))
    {
        Character* rider = NULL;
        ogre_unordered_set<hand>::type::iterator sit = thisptr->selectedCharacters.begin();
        for (; sit != thisptr->selectedCharacters.end(); ++sit)
        {
            Character* c = sit->getCharacter();
            if (!c) continue;
            if (!c->isAnimal())
            {
                rider = c;
                break;
            }
        }
        if (rider)
        {
            // Behave like the native "pick up" order: mount immediately only if the
            // rider is already next to the animal; otherwise send the rider walking to
            // it and board once it arrives (serviced per-frame in mainLoop_hook).
            if (RiderMountDist(rider, target) < kMountArriveDist)
            {
                pendingMount.erase(rider); // drop any stale in-flight request
                Mount(rider, target);
            }
            else
            {
                PendingMount pm;
                pm.mount   = target;
                pm.age     = 0;
                pm.refresh = 0;
                pendingMount[rider] = pm;
                rider->setDestination(target->getPosition(), false);
                DebugLog("Riding: rider walking to mount before boarding (d="
                         + IntToStr((int)RiderMountDist(rider, target)) + ")");
            }
            return; // swallow the original order
        }
    }

    // PUT_DOWN_OBJECT on a mounted rider -> dismount
    if (t == PUT_DOWN_OBJECT && target && IsRiding(target))
    {
        Dismount(target);
        return; // swallow the original order
    }

    // Attack order while riding a NECK (large) mount: the mount fights instead.
    // The per-frame controller below keeps the rider passive, so we just tell the
    // mount to engage the target with its vanilla animal combat.
    if (target && IsAttackTask(t))
    {
        ogre_unordered_set<hand>::type::iterator sit = thisptr->selectedCharacters.begin();
        for (; sit != thisptr->selectedCharacters.end(); ++sit)
        {
            Character* c = sit->getCharacter();
            if (!c) continue;
            Character* mount = GetMount(c);
            if (!mount) continue;
            boost::unordered_map<Character*, SeatInfo>::iterator mit = mountSeat.find(mount);
            if (mit != mountSeat.end() && mit->second.seatMode == SEAT_NECK)
            {
                if (mount->getAttackTarget().getCharacter() != target)
                    mount->attackTarget(target);
            }
        }
    }

    // Any other order issued to a selected character cancels a pending approach-to-mount
    // (the player changed their mind and redirected them).  The mount request itself
    // returns early above, so reaching here means this is a different order.
    if (!pendingMount.empty())
    {
        ogre_unordered_set<hand>::type::iterator sit = thisptr->selectedCharacters.begin();
        for (; sit != thisptr->selectedCharacters.end(); ++sit)
        {
            Character* c = sit->getCharacter();
            if (c) pendingMount.erase(c);
        }
    }

    // otherwise pass through (vanilla orders preserved)
    newPlayerTask_orig(thisptr, t, targetH, destinationIndoors, clickpos, addDontClear);
}

// ---- AnimationClass::update hook: steer the rider's animation selection ----
//
// While a rider is carried (pickupObject), the game's animation selection forces
// the ANIM_CARRIED category every frame, so no matter how often we re-run
// "sitting chair" afterwards it always loses (late-one-frame fight).  We hook
// the character's animation update and, BEFORE the game picks this frame's
// animation, clear the carried flag and point the slave loop at the pose we
// want.  Then the game's own selection plays the toilet-sit pose cleanly.

void (*animUpdate_orig)(AnimationClass* thisptr, float frameTIME) = NULL;
static void AnimUpdateImpl(AnimationClass* thisptr, float frameTIME)
{
    // is this a rider we manage?
    if (thisptr)
    {
        Character* ch = thisptr->me;
        if (ch)
        {
            // save/load restores the engine's carry link but not our ride state -
            // rebuild on first sight of an untracked carrying animal (no-op otherwise)
            if (ch->isCarryingSomething)
                TryRestoreOrphanedMount(ch);

            boost::unordered_map<Character*, Character*>::iterator it = riderToMount.find(ch);
            if (it != riderToMount.end())
            {
                Character* mount = it->second;
                if (mount)
                {
                    boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
                    if (sit != mountSeat.end() && sit->second.forceSit)
                    {
                        const char* poseAnim = (sit->second.posture == POSTURE_STAND) ? "idle_stand_normal" : "sitting chair";
                        AnimationData* poseData = thisptr->getAnimationData(poseAnim);
                        // stop the animation system from choosing the carried pose
                        thisptr->animationRequirements.carried = false;
                        // force the wanted loop as the slave animation for this frame
                        if (poseData)
                            thisptr->animationRequirements.forcedSlaveLoop = poseData;
                    }
                }
            }
        }
    }

    animUpdate_orig(thisptr, frameTIME);

#if 0
    // DISABLED 2026-08-23 (was: re-place every mounted rider after every character's
    // animation update).  DBG proved the engine does not apply a delta drag - it
    // ABSOLUTELY pins the carried node to a carrier-local slot every frame (slot
    // stayed constant at side+4.1 / down+6.4 / fwd+17.6 through a full turn), so no
    // number of early writes wins.  The real fix is stopping the engine's ragdoll-
    // carry drag itself (see the Numpad8 carry probe in HotkeyPass).
    if (!mountSeat.empty())
    {
        boost::unordered_map<Character*, Character*>::iterator rit = riderToMount.begin();
        for (; rit != riderToMount.end(); ++rit)
        {
            Character* rider = rit->first;
            Character* mount = rit->second;
            if (!rider || !mount) continue;
            AnimationClass* rAnim = rider->getAnimationClass();
            if (!rAnim || !rAnim->node) continue;
            boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
            if (sit == mountSeat.end()) continue;
            const SeatInfo& seat = sit->second;
            if (!SeatNeedsPlacement(seat)) continue;

            // Horizontal instant; vertical bob scaled by DampSeatBob - same rule as
            // the main loop so every sync point agrees.
            Ogre::Vector3 seatPos = ComputeDampedSeatPos(mount, seat);
            if (rider->getMovement())
                rider->getMovement()->_setPositionSimple(seatPos);
            SyncRiderNode(rider, mount, rAnim, seatPos, false);
        }
    }
#endif
}

// SEH shell: same rationale as mainLoop_hook.  The restore path runs here for
// every carrying character right after a load, when the engine's carry link can
// still be half-initialised; a fault wipes state instead of ending the session.
void animUpdate_hook(AnimationClass* thisptr, float frameTIME)
{
    __try
    {
        AnimUpdateImpl(thisptr, frameTIME);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        WipeAllRideState("access violation in animUpdate");
    }
}

// ---- Removed: two never-installed hooks (beingCarriedUpdate, updateAnimationTransforms) ----
//
// Both were only ever candidate levers for rider positioning and were NEVER registered
// (never ran), because their KenshiLib RVAs resolve to unsafe targets - a stack-imbalancing
// /LTCG dead copy, and a mid-function entry - and the hook engine copies a fixed 5 bytes
// with no boundary check.  The full reasoning is kept in the DISABLED HOOKS note inside
// startPlugin() below and in CLAUDE.md (§"未注册的危险 hook").  The dead hook bodies were
// deleted in the 2026-08-21 dead-code review; SyncRiderNode owns rider placement instead.


// ---- main loop hook: sync rider positions + handle mount/dismount keys ----

// Edge detector for numpad hotkeys: true only on the frame the key transitions down.
// Latches prev in passing, so each key needs no separate end-of-frame write-back.
static bool KeyEdge(bool down, bool& prev)
{
    bool pressed = (down && !prev);
    prev = down;
    return pressed;
}

void (*mainLoop_orig)(GameWorld* thisptr, float time) = NULL;

// ---- per-frame passes, invoked from MainLoopImpl below in definition order ----

// Loading a save mid-session frees every tracked Character*.  One implausible pair
// means the whole world was reset, so wipe ALL ride state instead of trying to
// surgically erase - everything else in the maps is just as stale.  If the new save
// has someone mounted, TryRestoreOrphanedMount rebuilds it from the native carry
// link.  Returns true when a wipe happened (caller must bail out this frame).
static bool WorldResetDetected()
{
    if (riderToMount.empty() && pendingMount.empty())
        return false;

    bool dead = false;
    boost::unordered_map<Character*, Character*>::iterator vit = riderToMount.begin();
    for (; vit != riderToMount.end() && !dead; ++vit)
        if (!CharacterLooksLive(vit->first) || !CharacterLooksLive(vit->second))
            dead = true;
    if (!dead)
    {
        boost::unordered_map<Character*, PendingMount>::iterator pit = pendingMount.begin();
        for (; pit != pendingMount.end() && !dead; ++pit)
            if (!CharacterLooksLive(pit->first) || !CharacterLooksLive(pit->second.mount))
                dead = true;
    }
    if (dead)
    {
        WipeAllRideState("tracked character vanished (world reset?)");
        return true;
    }
    return false;
}

// 0) Service pending "approach then mount" requests.  Each rider that was ordered
//    to mount from out of range is walking toward its animal; board it the moment
//    it enters order range.  Re-issue the destination periodically so the rider
//    keeps chasing a mount that is itself wandering, and drop the request if the
//    animal becomes invalid, gets taken by another rider, or can't be reached.
static void ServicePendingMounts()
{
    if (!pendingMount.empty())
    {
        boost::unordered_map<Character*, PendingMount>::iterator it = pendingMount.begin();
        while (it != pendingMount.end())
        {
            Character* rider = it->first;
            Character* mount = it->second.mount;

            bool drop = false;
            if (!rider || !mount || !CharacterLooksLive(rider) || !CharacterLooksLive(mount))
                drop = true;                                        // freed / invalid
            else if (IsRiding(rider))
                drop = true;                                        // already mounted
            else if (mountToRider.find(mount) != mountToRider.end())
                drop = true;                                        // mount taken by someone else
            else if (!IsRideable(mount))
                drop = true;                                        // no longer a player animal

            // age advances every frame the request is alive (timeout + settle guards).
            if (!drop && ++it->second.age > kMountApproachTimeout)
            {
                DebugLog("Riding: approach to mount timed out");
                drop = true;
            }

            if (drop)
            {
                pendingMount.erase(it++);
                continue;
            }

            CharMovement* move = rider->getMovement();
            float d = RiderMountDist(rider, mount);
            // The movement system's own "arrived / can get no closer" flag - scale-free
            // fallback for big-bodied mounts whose centre stays beyond kMountArriveDist.
            // Only trusted after a short settle so a spurious first-frame "reached" (set
            // before pathing starts) can't board instantly, AND only within a sane distance:
            // a rider whose CharMovement has been destroyed (the state a botched dismount
            // leaves behind - see the rev6 notes) never simulates and reports "destination
            // reached" forever, which used to board it from across the map.  The bound keeps
            // the fallback doing its real job (huge mount, centre out of reach) and nothing else.
            bool reached = move && move->isDestinationReached() && it->second.age > 20
                           && d < kMountArriveDist * 6.0f;

            if (d < kMountArriveDist || reached)
            {
                // Arrived: stop, board, and consume the request.
                if (move) move->halt();
                Character* r = rider;
                Character* m = mount;
                pendingMount.erase(it++);
                Mount(r, m);
                continue;
            }

            // Still en route: nudge the destination toward the (possibly moving) mount.
            if (++it->second.refresh >= kMountApproachRefresh)
            {
                it->second.refresh = 0;
                rider->setDestination(mount->getPosition(), false);
            }
            ++it;
        }
    }
}

// The five frame passes below are forward-declared so MainLoopImpl reads as a plain
// ordered checklist (bodies defined after it).
static void HaltAndForceSitPass();
static void SyncMountedRiders();
static void ForceWalkPass();
static void CombatAndForceDismountPass();
static void HotkeyPass();

static void MainLoopImpl(GameWorld* thisptr, float time)
{
    // call original first
    mainLoop_orig(thisptr, time);

    if (!ou) return;

    // Pass ORDER is load-bearing: the looks-live sweep must run before anything
    // dereferences a tracked pointer; pose/orientation before seat placement;
    // combat ejection after placement so an ejected rider isn't re-placed.
    if (WorldResetDetected())
        return;
    ServicePendingMounts();

    HaltAndForceSitPass();          // 1) riders stay still + upright
    SyncMountedRiders();            // 1b) seat position -> movement + render node
    ForceWalkPass();                // 1b2) pack-beast walk/run anim while moving
    CombatAndForceDismountPass();   // 1c) KO'd/dead mounts eject; NECK riders passive

    HotkeyPass();                   // 2+3) numpad mount/dismount/tune/debug keys
}

// 1) Keep riders still + upright on the mount's back.
//    We halt the rider every frame (keeps it still while the mount walks) and,
//    for mounts with forceSit, re-assert the sitting pose every frame.  The
//    carry system forces the ANIM_CARRIED "lying flat" pose each frame; like the
//    forceWalk fix for pack_beast, re-running the sit anim after the game's own
//    update overrides it.
static void HaltAndForceSitPass()
{
    if (!riderToMount.empty())
    {
        boost::unordered_map<Character*, Character*>::iterator it = riderToMount.begin();
        for (; it != riderToMount.end(); ++it)
        {
            Character* rider = it->first;
            if (!rider) continue;

            CharMovement* riderMove = rider->getMovement();
            if (riderMove)
                riderMove->halt();

            // force-sit: clear the carried flag (so the animation system stops forcing
            // the ANIM_CARRIED "limp, limbs dangling" pose) and re-assert the chosen
            // posture after the game's own update.
            Character* mount = it->second;
            if (mount)
            {
                boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
                if (sit != mountSeat.end() && sit->second.forceSit)
                {
                    AnimationClass* rAnim = rider->getAnimationClass();
                    if (rAnim)
                    {
                        rAnim->animationRequirements.carried = false;
                        // The carry system force-plays "carry me" (the being-carried
                        // pose) every frame at 0.5 weight, which blends with and ruins
                        // our pose.  Kick it out after the game's update so our pose
                        // owns the full animation weight.
                        rAnim->stopAnimation("carry me");
                        const char* poseAnim = (sit->second.posture == POSTURE_STAND) ? "idle_stand_normal" : "sitting chair";
                        rAnim->runSlaveAnim(poseAnim, 1.0f, 1.0f, 1.0f);
                        rAnim->runAnimation(poseAnim, 1.0f, 1.0f);
                    }
                    ApplyRiderOrientation(rider, sit->second, mount);
                }
            }
        }
    }
}

// 1b) Apply the computed seat position.  We run after the game's own update,
//     so our offset on top of the slave attachment sticks until next frame.
//     Skipped when there is nothing to correct (small/medium animals).
static void SyncMountedRiders()
{
    if (!mountSeat.empty())
    {
        boost::unordered_map<Character*, Character*>::iterator it = riderToMount.begin();
        for (; it != riderToMount.end(); ++it)
        {
            Character* rider = it->first;
            Character* mount = it->second;
            if (!rider || !mount) continue;

            boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
            if (sit == mountSeat.end()) continue;
            const SeatInfo& seat = sit->second;

            if (!SeatNeedsPlacement(seat))
                continue; // exact bone attach, no correction needed

            // Horizontal tracks the mount instantly (no backward lag).  DampSeatBob
            // scales the vertical run-cycle bob to kSeatBobScale of its natural size.
            Ogre::Vector3 seatPos = ComputeDampedSeatPos(mount, seat);

            if (debugContinuous)
                DebugLogRideFrame(rider, mount, seat, seatPos);

            // The renderer reads the rider's root bone world position (rBip), not the
            // movement position and not rootBonePosition.  _setPositionSimple keeps the
            // movement/collision position on the back; SyncRiderNode solves for the
            // scene node so the RENDERED root bone lands on the seat and stops swinging.
            if (rider->getMovement())
                rider->getMovement()->_setPositionSimple(seatPos);
            AnimationClass* rAnim = rider->getAnimationClass();
            if (rAnim)
                SyncRiderNode(rider, mount, rAnim, seatPos, true);
        }
    }
}

// 1b2) Force the walk/run animation on mounts whose carry mode suppresses it
//      (pack_beast family detected via "beast walk").  Only while the mount is
//      actually moving and not in combat, so normal idle/attack anims play.
static void ForceWalkPass()
{
    if (!mountSeat.empty())
    {
        boost::unordered_map<Character*, Character*>::iterator it = riderToMount.begin();
        for (; it != riderToMount.end(); ++it)
        {
            Character* mount = it->second;
            if (!mount) continue;

            boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
            if (sit == mountSeat.end() || !sit->second.forceWalk) continue;

            CharMovement* move = mount->getMovement();
            if (!move) continue;
            Ogre::Vector3 nowPos = move->getPosition();

            Ogre::Vector3 delta(0.0f, 0.0f, 0.0f);
            boost::unordered_map<Character*, Ogre::Vector3>::iterator pit = mountLastPos.find(mount);
            if (pit != mountLastPos.end())
                delta = nowPos - pit->second;
            mountLastPos[mount] = nowPos;

            delta.y = 0.0f;
            float moveDist = delta.length();
            if (moveDist < 0.05f) continue; // standing still
            if (mount->isInCombatMode(true, true)) continue; // let attack anims play

            AnimationClass* anim = mount->getAnimationClass();
            if (anim)
                anim->runAnimation(moveDist > 0.35f ? "beast run" : "beast walk", 1.0f, 0.1f);
        }
    }
}

// 1c) Mount combat + forced dismount.
//     - Any mount that is down (KO'd) or dead force-dismounts its rider.
//     - On NECK-mode (large) mounts the rider stays passive: their combat is
//       suppressed every frame and the mount fights back with its native
//       animal combat, defending the rider against the rider's attackers.
static void CombatAndForceDismountPass()
{
    if (!riderToMount.empty())
    {
        boost::unordered_map<Character*, Character*>::iterator it = riderToMount.begin();
        while (it != riderToMount.end())
        {
            Character* rider = it->first;
            Character* mount = it->second;

            if (!rider || !mount || mount->isDown() || mount->isDead())
            {
                Character* r = rider;
                ++it; // Dismount erases from the map, advance first
                if (r) Dismount(r);
                continue;
            }

            bool neckMount = false;
            boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
            if (sit != mountSeat.end() && sit->second.seatMode == SEAT_NECK)
                neckMount = true;

            if (neckMount)
            {
                // rider stays passive - never swings its tiny weapon from the back
                if (rider->isInCombatMode(true, true) || rider->getAttackTarget().getCharacter())
                    rider->endCombatMode();
                if (rider->getMovement())
                    rider->getMovement()->halt();

                // the mount defends the rider with vanilla animal combat
                lektor<hand> attackers;
                rider->getAllAttackers(attackers);
                if (attackers.size())
                {
                    Character* current = mount->getAttackTarget().getCharacter();
                    for (lektor<hand>::iterator ait = attackers.begin(); ait != attackers.end(); ++ait)
                    {
                        Character* attacker = ait->getCharacter();
                        if (!attacker) continue;
                        if (current != attacker)
                            mount->attackTarget(attacker);
                    }
                }
            }
            ++it;
        }
    }
}

// 2) mount/dismount/tune keys (numpad, physical key detection via OIS) and
// 3) live seat tuning (applies to the currently mounted animal).  The prev-state
//    flags are function locals on purpose: one edge consumer each, persist across
//    frames, reset naturally on DLL reload.
static void HotkeyPass()
{
    if (!(key && key->keyboard && ou->player))
        return;

    {
        static bool prevNumpad1 = false;
        static bool prevNumpad2 = false;
        static bool prevAdd = false;
        static bool prevSub = false;
        static bool prevMul = false;
        static bool prevDiv = false;
        static bool prevNP0 = false;
        static bool prevNP5 = false;
        static bool prevNP6 = false;
        static bool prevNP7 = false;
        static bool prevDecimal = false;

        // Debug: Numpad decimal (.) toggles continuous ride diagnostics.  Every ~10
        // frames we log the mount's root/back bone position+orientation, our computed
        // target seat position, and the rider's actual render node, so we can see
        // whether the "fling while running" comes from position or orientation.
        if (KeyEdge(key->keyboard->isKeyDown(OIS::KC_DECIMAL), prevDecimal))
        {
            debugContinuous = !debugContinuous;
            DebugLog(std::string("Riding: debug continuous ") + (debugContinuous ? "ON" : "OFF"));
        }

        if (KeyEdge(key->keyboard->isKeyDown(OIS::KC_NUMPAD1), prevNumpad1))
        {
            PlayerInterface* player = ou->player;
            Character* rider = NULL;
            Character* mount = NULL;

            // iterate selected characters to find a human and an animal
            ogre_unordered_set<hand>::type::iterator sit = player->selectedCharacters.begin();
            for (; sit != player->selectedCharacters.end(); ++sit)
            {
                Character* c = sit->getCharacter();
                if (!c) continue;
                if (c->isAnimal())
                    mount = c;
                else
                    rider = c;
            }

            // fallback: if selection was via the single selectedCharacter
            if (!rider && !mount && player->selectedCharacter)
            {
                Character* sel = player->selectedCharacter.getCharacter();
                if (sel)
                {
                    if (sel->isAnimal())
                        mount = sel;
                    else
                        rider = sel;
                }
            }

            if (rider && mount)
                Mount(rider, mount);
            else
                DebugLog("Riding: select one human + one animal, then press Numpad1");
        }

        if (KeyEdge(key->keyboard->isKeyDown(OIS::KC_NUMPAD2), prevNumpad2))
        {
            PlayerInterface* player = ou->player;
            Character* rider = NULL;
            if (player->selectedCharacter)
                rider = player->selectedCharacter.getCharacter();
            if (rider)
                Dismount(rider);
            else
                DebugLog("Riding: select the rider, then press Numpad2");
        }

        // Numpad2 above now runs the full rev6 put-down (getDropped + ragdoll clear) inside
        // Dismount() itself, confirmed in-game 2026-08-24 (both hull A/B variants stood the
        // rider up and restored control; hull=true settles straight down, hull=false pops the
        // body up ~19u first, so true won).  The Numpad4/Numpad8 A/B probes and the 30-frame
        // PostDismountWatch that carried the investigation have been removed now that the fix
        // lives on the real path.  History of the dead ends is preserved in CLAUDE.md.

        // 3) Live seat tuning (applies to the currently mounted animal).
        //    Rider FACING is not tunable - it always follows the mount's travel direction.
        //    Numpad +/- : up/down 0.1    Numpad */ : forward/back 0.1
        //    Numpad 5   : cycle seat mode (exact -> midpoint -> neck)
        //    Numpad 6   : toggle force-sit on/off
        //    Numpad 7   : cycle rider posture (sit -> stand)
        //    Ctrl+*/ /   : move the seat left/right (lateral)
        //    Numpad 0   : reset this species to 0
        //    (The old Numpad 3/9 ±0.02 fine step was removed 2026-08-24 - redundant with
        //     +/- now that per-species presets cover the common animals; user call.)
        {
            bool ctrlD = key->keyboard->isKeyDown(OIS::KC_LCONTROL) || key->keyboard->isKeyDown(OIS::KC_RCONTROL);

            bool addE = KeyEdge(key->keyboard->isKeyDown(OIS::KC_ADD),      prevAdd);
            bool subE = KeyEdge(key->keyboard->isKeyDown(OIS::KC_SUBTRACT), prevSub);
            bool mulE = KeyEdge(key->keyboard->isKeyDown(OIS::KC_MULTIPLY), prevMul);
            bool divE = KeyEdge(key->keyboard->isKeyDown(OIS::KC_DIVIDE),   prevDiv);
            bool np0E = KeyEdge(key->keyboard->isKeyDown(OIS::KC_NUMPAD0),  prevNP0);
            bool np5E = KeyEdge(key->keyboard->isKeyDown(OIS::KC_NUMPAD5),  prevNP5);
            bool np6E = KeyEdge(key->keyboard->isKeyDown(OIS::KC_NUMPAD6),  prevNP6);
            bool np7E = KeyEdge(key->keyboard->isKeyDown(OIS::KC_NUMPAD7),  prevNP7);

            bool stepUp = addE && !ctrlD;
            bool stepDn = subE && !ctrlD;
            bool stepFw = mulE && !ctrlD;
            bool stepBk = divE && !ctrlD;
            bool stepLatR = mulE && ctrlD;
            bool stepLatL = divE && ctrlD;
            bool stepRst = np0E;
            bool stepMode = np5E;
            bool stepSit = np6E;
            bool stepPosture = np7E;

            if (stepUp || stepDn || stepFw || stepBk || stepLatR || stepLatL || stepRst || stepMode || stepSit || stepPosture)
            {
                PlayerInterface* player = ou->player;
                Character* rider = NULL;
                if (player->selectedCharacter)
                    rider = player->selectedCharacter.getCharacter();
                // if the mount itself is selected, resolve to its rider
                if (rider && !IsRiding(rider))
                {
                    boost::unordered_map<Character*, Character*>::iterator mit = mountToRider.find(rider);
                    if (mit != mountToRider.end())
                        rider = mit->second;
                }
                if (rider && IsRiding(rider))
                {
                    Character* mount = GetMount(rider);
                    boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
                    if (sit != mountSeat.end())
                    {
                        SeatInfo& seat = sit->second;
                        if (stepSit)
                        {
                            seat.forceSit = !seat.forceSit;
                            PersistTuning(seat);
                            DebugLog("Riding: force-sit " + seat.species + " -> " + IntToStr(seat.forceSit ? 1 : 0));
                        }
                        else if (stepPosture)
                        {
                            seat.posture = (seat.posture == POSTURE_STAND) ? POSTURE_SIT : POSTURE_STAND;
                            PersistTuning(seat);
                            DebugLog("Riding: posture " + seat.species + " -> " + IntToStr(seat.posture));
                        }
                        else if (stepLatR || stepLatL)
                        {
                            float dLat = stepLatR ? 0.1f : -0.1f;
                            seat.lateral += dLat;
                            if (seat.lateral < -kTuningClamp) seat.lateral = -kTuningClamp;
                            if (seat.lateral >  kTuningClamp) seat.lateral =  kTuningClamp;
                            PersistTuning(seat);
                            DebugLog("Riding: lateral " + seat.species + " -> " + IntToStr((int)(seat.lateral * 100.0f)));
                        }
                        else if (stepMode)
                        {
                            CycleSeatMode(seat);
                            // lift/anchor changed -> re-capture the bob baseline next frame
                            mountBaseVOffset.erase(mount);
                        }
                        else if (stepRst)
                        {
                            seat.userOffset = Ogre::Vector3::ZERO;
                            seat.lateral = 0.0f;
                            PersistTuning(seat);
                            DebugLog("Riding: reset " + seat.species + " tuning");
                        }
                        else
                        {
                            float dUp = 0.0f, dFwd = 0.0f;
                            if (stepUp)   dUp = 0.1f;
                            if (stepDn)   dUp = -0.1f;
                            if (stepFw)   dFwd = 0.1f;
                            if (stepBk)   dFwd = -0.1f;
                            TuneSeat(seat, dUp, dFwd);
                        }
                    }
                }
                else
                {
                    DebugLog("Riding: select the rider, then tune with Numpad +/- * / 0");
                }
            }
        }
    }
}

// SEH shell around the whole main-loop plugin section: if a stale Character*
// ever slips past the looks-live sweep and faults, wipe the ride state and keep
// the game running instead of crashing.  __try cannot share a frame with
// unwindable C++ objects, hence the Impl split.
void mainLoop_hook(GameWorld* thisptr, float time)
{
    __try
    {
        MainLoopImpl(thisptr, time);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        WipeAllRideState("access violation in mainLoop");
    }
}

// ---- Context menu hook declarations added by plugin ----
// original pointer

// Depth-first search for the first TextBox/Button caption under a widget.  MyGUI's
// Button derives from TextBox, and isType() walks the base chain, so castType<TextBox>
// matches buttons too.  Returns the caption as a std::string (MyGUI stores UTF-8).
static std::string GetWidgetCaptionDeep(MyGUI::Widget* w)
{
    if (!w) return "";
    MyGUI::TextBox* tb = w->castType<MyGUI::TextBox>(false);
    if (tb)
    {
        std::string s = tb->getCaption();
        if (!s.empty()) return s;
    }
    int cc = 0;
    try { cc = (int)w->getChildCount(); } catch(...) { cc = 0; }
    for (int i = 0; i < cc; ++i)
    {
        MyGUI::Widget* cw = w->getChildAt(i);
        if (cw)
        {
            std::string s = GetWidgetCaptionDeep(cw);
            if (!s.empty()) return s;
        }
    }
    return "";
}

// Set the caption on the first TextBox/Button found under a widget (DFS), then stop.
// utf8 must be UTF-8 bytes; MyGUI::UString takes std::string as UTF-8.
static bool SetWidgetCaptionDeep(MyGUI::Widget* w, const std::string& utf8)
{
    if (!w) return false;
    MyGUI::TextBox* tb = w->castType<MyGUI::TextBox>(false);
    if (tb) { tb->setCaption(utf8); return true; }
    int cc = 0;
    try { cc = (int)w->getChildCount(); } catch(...) { cc = 0; }
    for (int i = 0; i < cc; ++i)
    {
        MyGUI::Widget* cw = w->getChildAt(i);
        if (cw && SetWidgetCaptionDeep(cw, utf8)) return true;
    }
    return false;
}

void (*contextMenu_show_orig)(ContextMenuGUI* thisptr, const lektor<int>& ordersList, const std::string& _name, bool offset) = NULL;

// hook implementation: rename the Bodyguard entry to 上马 for rideable player-owned
// animals only.  All other targets (humans, wild animals) are left untouched.
void contextMenu_show_hook(ContextMenuGUI* thisptr, const lektor<int>& ordersList, const std::string& _name, bool offset)
{
    // call original so menu is constructed
    if (contextMenu_show_orig) contextMenu_show_orig(thisptr, ordersList, _name, offset);

    try {
        if (!thisptr) return;
        Character* target = thisptr->contextMenuTarget.getCharacter();
        if (!target) return;
        MyGUI::Widget* opts = thisptr->optionsList;
        if (!opts) return;

        int childCount = (int)opts->getChildCount();
        int orderCount = (int)ordersList.size();

        try { char dbg0[256]; _snprintf_s(dbg0, 256, _TRUNCATE, "Riding: ctxMenu target=%p orders=%d children=%d rideable=%d", (void*)target, orderCount, childCount, IsRideable(target) ? 1 : 0); DebugLog(dbg0); } catch(...) {}

        bool rideable = IsRideable(target);

        // IMPORTANT: the context-menu ordersList uses a DIFFERENT enumeration than
        // TaskType.  In-game dump (Kenshi 1.0.65) proved: 26=Trade, 31=Bodyguard,
        // 45=Follow, 69=KnockOut, 225=PickUp.  TaskType::BODYGUARD compiles to 45 here
        // (which is Follow in the menu enum), so the menu id for Bodyguard is 31.
        // (newPlayerTask still uses TaskType::BODYGUARD correctly: clicking the
        // order-31 row is dispatched by the game as TaskType 45.)
        const int kMenuOrderBodyguard = 31;

        // per-row mapping dump, only when continuous diagnostics is on (Numpad .).
        // Dumps EVERY child against the order AT THE SAME INDEX - keeping the two
        // side by side is exactly what exposed the misalignment documented below.
        if (debugContinuous)
        {
            for (int i = 0; i < childCount; ++i)
            {
                MyGUI::Widget* child = opts->getChildAt(i);
                if (!child) continue;
                try { std::string cap = GetWidgetCaptionDeep(child);
                      char drow[256]; _snprintf_s(drow, 256, _TRUNCATE, "Riding:   row %d order=%s type=%s cap='%s'", i,
                      (i < orderCount ? IntToStr(ordersList[i]).c_str() : "-"), child->getTypeName().c_str(), cap.c_str()); DebugLog(drow); } catch(...) {}
            }
        }

        bool hasBodyguardOrder = false;
        for (int i = 0; i < orderCount; ++i)
            if (ordersList[i] == kMenuOrderBodyguard) { hasBodyguardOrder = true; break; }

        // Only rename for rideable player-owned animals that actually offer the
        // Bodyguard order.  Everything else (humans, wild animals) stays untouched so
        // vanilla "Bodyguard" keeps working - no hiding, no layout change.
        if (!rideable || !hasBodyguardOrder)
            return;

        // ⚠️ optionsList children are NOT guaranteed to be index-parallel to
        // ordersList: observed 2026-08-23 on wounded animals - the 急救 (first aid,
        // order 25) row renders at the TOP while sitting at the END of ordersList,
        // shifting every later row down by one.  Index-based lookup then renamed the
        // FOLLOW row -> "right-click UI scrambled" on bull / caged beast / beak thing.
        // Locate the button BY ITS VANILLA CAPTION instead: the caption is literally
        // what the user sees, so a match can never scramble the menu.  If no such row
        // is found, leave the menu as-is - clicking vanilla 侍卫 still mounts via the
        // newPlayerTask hook (dispatch keys on the internal order id, not the label).
        static const std::string kBodyguardCap = "\xE4\xBE\x8D\xE5\x8D\xAB"; // "侍卫" UTF-8, explicit bytes (v100 has no /utf-8)
        for (int i = 0; i < childCount; ++i)
        {
            MyGUI::Widget* child = opts->getChildAt(i);
            if (!child) continue;
            std::string cap;
            try { cap = GetWidgetCaptionDeep(child); } catch(...) { cap.clear(); }
            if (cap != kBodyguardCap) continue;

            // rename Bodyguard -> 上马 (UTF-8 bytes; MyGUI captions are UTF-8)
            bool ok = SetWidgetCaptionDeep(child, "\xE4\xB8\x8A\xE9\xA9\xAC");
            try { opts->_updateChilds(); } catch(...) {}
            try { DebugLog(std::string("Riding: rename Bodyguard->shangma idx=") + IntToStr(i) + (ok ? " ok" : " FAILED(no caption widget)")); } catch(...) {}
            break;
        }

    } catch(...) {}
}

__declspec(dllexport) void startPlugin()
{
    DebugLog("RidingPlugin: start");

    LoadConfig();
    if (!speciesTuning.empty())
        DebugLog("RidingPlugin: loaded " + IntToStr((int)speciesTuning.size()) + " seat tuning entries from riding.cfg");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&GameWorld::_NV_mainLoop_GPUSensitiveStuff), (void*)&mainLoop_hook, (void**)&mainLoop_orig))
        ErrorLog("RidingPlugin: Could not hook GameWorld::mainLoop_GPUSensitiveStuff!");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&PlayerInterface::newPlayerTaskSelectedCharacters), (void*)&newPlayerTask_hook, (void**)&newPlayerTask_orig))
        ErrorLog("RidingPlugin: Could not hook PlayerInterface::newPlayerTaskSelectedCharacters!");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&AnimationClass::_NV_update), (void*)&animUpdate_hook, (void**)&animUpdate_orig))
        ErrorLog("RidingPlugin: Could not hook AnimationClass::update!");

    // Human riders use AnimationClassHuman::update (vtable dispatch to 0x5C4FD0),
    // not the base AnimationClass::update - hook the human override too so the
    // animation-selection steering + position enforcement actually run for
    // player-race riders.
    void* humanUpdateOrig = NULL;
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&AnimationClassHuman::_NV_update), (void*)&animUpdate_hook, (void**)&humanUpdateOrig))
        ErrorLog("RidingPlugin: Could not hook AnimationClassHuman::update!");

    // ---- DISABLED HOOKS (safety fix) ----------------------------------------
    // Two hooks are deliberately NOT installed.  Disassembly (see
    // RidingPlugin_RE_NOTES.md section 0) showed the addresses they resolve to are
    // unsafe, and the hook engine copies a fixed 5 bytes without checking
    // instruction boundaries:
    //
    //   AnimationClass::beingCarriedUpdate - the KenshiLib RVA (0x5B5200) lands in
    //     the middle of an unrelated constructor [0x5B50F0..0x5B5263).  The real
    //     function (0x5B5980) is a /LTCG dead copy that never fires (carry
    //     positioning is inlined into the caller at 0x5CDA20), and its prologue is
    //     five 1-byte pushes, so skipping 5 bytes unbalances the stack against a
    //     6-pop epilogue -> crash while a native carried NPC moves.
    //
    //   AnimationClass::updateAnimationTransforms - the KenshiLib RVA (0x5B0E30)
    //     lands mid-function inside a larger routine that uses uninitialised
    //     rsi/rbp/rax past that point; patching there corrupts that routine.  The
    //     real entry is still unlocated.  The hook body was only ever a
    //     verification experiment (zero the root bone local + log rBip).
    //
    // The hook bodies were deleted in the 2026-08-21 dead-code review (they never ran).
    // Do NOT re-add and register either without first locating a safe entry (pure
    // push/sub prologue) or adding a replay trampoline - KenshiLib::AddHook has none.
    // -------------------------------------------------------------------------

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&ContextMenuGUI::show), (void*)&contextMenu_show_hook, (void**)&contextMenu_show_orig))
        ErrorLog("RidingPlugin: Could not hook ContextMenuGUI::show!");

    DebugLog("RidingPlugin: hooks installed");
}
