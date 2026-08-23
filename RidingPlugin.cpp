#include <Debug.h>

#include <kenshi/Character.h>
#include <kenshi/CharacterHuman.h>
#include <kenshi/CharacterAnimal.h>
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
#include <ogre/OgreSceneNode.h>
#include <ogre/OgreOldBone.h>

#include <mygui/MyGUI_Gui.h>
#include <mygui/MyGUI_Window.h>
#include <mygui/MyGUI_Button.h>
#include <mygui/MyGUI_Delegate.h>

#include <boost/unordered_map.hpp>
#include <string>
#include <functional>

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

// Mount methods: how the rider is held on the mount.
//   0 = CARRY  - use the native carry system (pickupObject).  Holds the rider with
//                no physics fighting, but some animals (pack_beast family) lose
//                their walk animation while in carry mode.
//   1 = SLAVE  - no carry system, pure slave attach + per-frame position sync so
//                the mount keeps its normal locomotion animations.
enum MountMethod
{
    MOUNT_CARRY = 0,
    MOUNT_SLAVE = 1
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

// Per-species tuning: seat mode + (x = forward, y = up) world-space delta + mount method.
// Orientation is NOT tunable - the rider always faces the mount's travel direction.
struct SpeciesTuning
{
    int           seatMode;
    int           mountMethod;   // MountMethod (0=carry, 1=slave)
    bool          forceSit;      // re-assert the sitting pose every frame
    int           posture;       // RiderPosture (0=sit, 1=stand)
    float         lateral;       // side offset (right/left), world units
    Ogre::Vector3 offset;
    SpeciesTuning() : seatMode(SEAT_MIDPOINT), mountMethod(MOUNT_CARRY), forceSit(true), posture(POSTURE_SIT), lateral(0.0f), offset(Ogre::Vector3::ZERO) {}
    SpeciesTuning(int m, const Ogre::Vector3& o) : seatMode(m), mountMethod(MOUNT_CARRY), forceSit(true), posture(POSTURE_SIT), lateral(0.0f), offset(o) {}
    SpeciesTuning(int m, int mm, const Ogre::Vector3& o) : seatMode(m), mountMethod(mm), forceSit(true), posture(POSTURE_SIT), lateral(0.0f), offset(o) {}
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
        int mm = MOUNT_CARRY;
        int sit = 1;
        // columns 6-8 of the line format (roll/pitch/yaw orientation tunes) are obsolete
        // - facing is always the mount's travel direction now.  Parse them into dummies
        // only so existing cfg files keep loading with the same column layout.
        float rollIg = 0.0f, pitchIg = 0.0f, yawIg = 0.0f;
        int posture = POSTURE_SIT;
        float lateral = 0.0f;
        int n = sscanf(val.c_str(), "%d,%f,%f,%d,%d,%f,%f,%f,%d,%f", &mode, &up, &fwd, &mm, &sit, &rollIg, &pitchIg, &yawIg, &posture, &lateral);
        if (n >= 3)
        {
            if (mode < SEAT_EXACT) mode = SEAT_EXACT;
            if (mode > SEAT_NECK)  mode = SEAT_NECK;
            if (mm < MOUNT_CARRY) mm = MOUNT_CARRY;
            if (mm > MOUNT_SLAVE) mm = MOUNT_SLAVE;
            if (posture < POSTURE_SIT) posture = POSTURE_SIT;
            if (posture > POSTURE_STAND) posture = POSTURE_STAND;
            SpeciesTuning st(mode, mm, Ogre::Vector3(fwd, up, 0.0f));
            st.forceSit = (sit != 0);
            st.posture = posture;
            st.lateral = lateral;
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
    fprintf(f, "# <species>=<mode>,<up>,<forward>,<mount>,<sit>,<roll>,<pitch>,<yaw>,<posture>,<lateral>  mode 0=exact 1=midpoint 2=neck  mount 0=carry 1=slave  sit 0=off 1=on  posture 0=sit 1=stand  lateral=side offset\n");
    fprintf(f, "# columns 6-8 (roll/pitch/yaw) are OBSOLETE - facing is always the mount's travel direction; always written as 0\n");
    boost::unordered_map<std::string, SpeciesTuning>::iterator it = speciesTuning.begin();
    for (; it != speciesTuning.end(); ++it)
        fprintf(f, "%s=%d,%.2f,%.2f,%d,%d,0.0,0.0,0.0,%d,%.2f\n", it->first.c_str(), it->second.seatMode,
                it->second.offset.y, it->second.offset.x, it->second.mountMethod,
                it->second.forceSit ? 1 : 0, it->second.posture, it->second.lateral);
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

    // Per-individual size scaling REMOVED (2026-08-23, user decision).  It measured a
    // torso proxy from live bones, but the mount's bone world reads are untrustworthy
    // at exactly the moments we build a SeatInfo: right after pickupObject / right
    // after a load they can come back in UNSCALED (bind) space - the same skeleton
    // that measures torso 10.3 once settled measured 103 at mount time (its node-scale
    // multiple).  That made sizeScale ~10 and amplified the tuned offsets (-7.1 up)
    // into a seat 70 units underground ("rider model vanished"); clamping to 2.0 still
    // put the rider on the floor.  There is no reliable measurement moment, so the
    // factor is now fixed at 1.0 - seat tuning is purely per-species (riding.cfg),
    // exactly the values the user calibrated.
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
// orientation so the seated pose is drawn upright.
//
// Orientation is ALWAYS the mount's DIRECTION OF TRAVEL (not its head/body bone
// forward).  No manual per-species facing tunes anymore - the user dropped them
// (2026-08-23); the node is placed in world space, so no bone-local conversion needed.
static void ApplyRiderOrientation(Character* rider, const SeatInfo& seat, Character* mount)
{
    if (!rider || !mount) return;
    AnimationClass* rAnim = rider->getAnimationClass();
    if (!rAnim || !rAnim->node) return;
    AnimationClass* mountAnim = mount->getAnimationClass();
    if (!mountAnim) return;

    // Heading source = the mount's DIRECTION OF TRAVEL, not its head/body bone forward.
    // We take the horizontal delta of the mount's movement (pathfinding) position
    // between frames: while it is moving that is exactly "the way it is going", and it
    // ignores the head/body/back bone rotating to face elsewhere mid-run.  When the
    // mount is (nearly) stationary we HOLD the last travel direction so the rider keeps
    // facing that way instead of snapping around; before it has ever moved (just
    // mounted) we fall back to the body-bone forward so the rider still faces sensibly.
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
    // the node orientation IS world space - apply worldQ directly.  Facing = travel
    // direction, always; no per-species orientation tunes anymore.
    rAnim->node->setOrientation(worldQ);
}

// ---- put the rider's RENDER position on the mount's back --------------------
//
// The renderer does NOT read the movement position or rootBonePosition - it reads the
// rider's root bone world position (rBip).  Diagnostics (RE_NOTES section 5) measured
// that rBip satisfies, with zero residual:
//
//     rBip = node + nodeQ * boneLocal
//
// where boneLocal is the root bone's offset inside the node's own frame and is
// constant for a given pose (~6.4 units, mostly vertical, for the sitting pose).
// So writing the node straight to seatPos - what the old code did via
// AnimationClass::setPosition - leaves the rider floating ~6.4 above the seat, and
// worse, nodeQ rotates as the mount runs/turns, so that offset sweeps around and the
// rider visibly swings off the back.
//
// Invert the equation instead and solve for the node that puts rBip where we want it:
//
//     node = (seatPos + anchor) - nodeQ * boneLocal
//
// `anchor` is captured once per mount, on the first synced frame, as (rBip - node).
// That keeps the rider at the same height the existing per-species seat tuning in
// riding.cfg was calibrated against - this change removes the swing without
// invalidating anyone's saved offsets.  boneLocal is recomputed every frame so a
// posture switch (Numpad 7, sit <-> stand) self-corrects within one frame.
static void SyncRiderNode(Character* rider, Character* mount, AnimationClass* rAnim,
                          const Ogre::Vector3& seatPos)
{
    if (!rider || !mount || !rAnim || !rAnim->node) return;

    Ogre::Vector3    nodeP = rAnim->getSceneNodePosition();
    Ogre::Quaternion nodeQ = rAnim->getSceneNodeOrientation();
    Ogre::Vector3    rBip  = rAnim->getBoneWorldPosition("Bip01", 1.0f);

    // root bone offset expressed in the node's frame (constant per pose)
    Ogre::Vector3 boneLocal = nodeQ.Inverse() * (rBip - nodeP);

    // The pose offset rBip-node for a playable character is small (~6.4 sitting).
    // A save/load can hand us control while the engine still has the rider's node and
    // bones far apart - capturing THEN stores a huge garbage anchor (~20+) that pins
    // the rider off the seat forever (seen 2026-08-23: rider dragged on the ground
    // beside the mount).  So: refuse to store an implausible offset (keep retrying
    // each frame until the pose relation is sane), and self-heal a previously stored
    // poisoned one.
    static const float kMaxAnchorLen = 12.0f;
    Ogre::Vector3 rel = rBip - nodeP;
    {
        boost::unordered_map<Character*, Ogre::Vector3>::iterator ai = mountAnchor.find(mount);
        if (ai != mountAnchor.end() && ai->second.length() > kMaxAnchorLen)
        {
            // poisoned anchor -> drop it AND the bob baseline captured in the same
            // corrupted instant; both recapture from a sane frame
            mountAnchor.erase(ai);
            mountBaseVOffset.erase(mount);
        }
        ai = mountAnchor.find(mount);
        if (ai == mountAnchor.end() && rel.length() <= kMaxAnchorLen)
            ai = mountAnchor.insert(std::make_pair(mount, rel)).first;
        // while capture is still deferred, use the LIVE offset so this frame still
        // lands sensibly instead of jumping to a wrong fixed point - but CLAMPED,
        // otherwise a stretched post-load pose feeds an unbounded self-referential
        // offset back into the placement (seen 2026-08-23 as a constant ~74 drift)
        Ogre::Vector3 anchor = (ai != mountAnchor.end()) ? ai->second : rel;
        if (anchor.length() > kMaxAnchorLen)
            anchor *= kMaxAnchorLen / anchor.length();

        Ogre::Vector3 target = seatPos + anchor;
        rAnim->node->setPosition(target - nodeQ * boneLocal);
        rAnim->rootBonePosition = seatPos;
    }
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
    float uy    = seat.userOffset.y;
    float baseY = seatPos.y - uy;

    boost::unordered_map<Character*, float>::iterator vo = mountBaseVOffset.find(mount);
    if (vo != mountBaseVOffset.end() && fabsf(vo->second) > 25.0f)
    {
        // poisoned baseline (captured on a ghost-bone frame): drop it and recapture
        // below from this frame's values
        mountBaseVOffset.erase(mount);
        vo = mountBaseVOffset.end();
    }
    if (vo == mountBaseVOffset.end())
    {
        mountBaseVOffset[mount] = baseY - refY;    // stable seat height above the reference (no bob, no user tune)
        return;                                    // first frame: nothing to damp yet
    }

    float stableY    = refY + vo->second;
    float dampedBase = stableY + (baseY - stableY) * kSeatBobScale;
    seatPos.y = dampedBase + uy;                   // user height tune applies at full strength
}

// Apply a live tuning step to the currently mounted seat and persist it.
static void TuneSeat(SeatInfo& seat, float dUp, float dFwd)
{
    seat.userOffset.x += dFwd;
    seat.userOffset.y += dUp;
    ClampTuning(seat.userOffset);

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

    DebugLog("Riding: mode " + seat.species + " -> " + IntToStr(seat.seatMode));
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
    mount->pickupObject(rider);

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

    boost::unordered_map<Character*, Character*>::iterator it = riderToMount.find(rider);
    if (it != riderToMount.end())
    {
        Character* mount = it->second;
        // tell the mount to drop the rider (no ragdoll, no hull)
        if (mount)
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
        debugLastPos.erase(mount);
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
    mountBaseVOffset.erase(mount);
    mountSmoothOrient.erase(mount);
    mountHeadingPos.erase(mount);
    mountHeadingDir.erase(mount);
    debugLastPos.erase(mount);

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

    // After the game's own animation update (which re-pins the carried rider onto
    // the carry bone), re-place the rider onto the mount's back so the carry bone
    // swing can't fling them around.  This runs inside the rider's own animation
    // update, closer to the render path than the main-loop sync.
    if (thisptr)
    {
        Character* ch = thisptr->me;
        if (ch)
        {
            boost::unordered_map<Character*, Character*>::iterator it = riderToMount.find(ch);
            if (it != riderToMount.end())
            {
                Character* mount = it->second;
                if (mount)
                {
                    boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
                    if (sit != mountSeat.end())
                    {
                        const SeatInfo& seat = sit->second;

                        if (!(seat.seatMode == SEAT_EXACT && seat.lift == Ogre::Vector3::ZERO && seat.userOffset == Ogre::Vector3::ZERO))
                        {
                            // Horizontal instant; vertical bob scaled by DampSeatBob -
                            // same rule as the main loop so the two points agree.
                            Ogre::Vector3 seatPos = ComputeSeatPosition(seat, mount);
                            DampSeatBob(mount, seat, seatPos);

                            if (ch && ch->getMovement())
                                ch->getMovement()->_setPositionSimple(seatPos);
                            SyncRiderNode(ch, mount, thisptr, seatPos);
                        }
                    }
                }
            }
        }
    }
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

void (*mainLoop_orig)(GameWorld* thisptr, float time) = NULL;
static void MainLoopImpl(GameWorld* thisptr, float time)
{
    // call original first
    mainLoop_orig(thisptr, time);

    if (!ou) return;

    // World-reset sentinel (2026-08-23): loading a save mid-session frees every
    // tracked Character*.  One implausible pair means the whole world was reset,
    // so wipe ALL ride state instead of trying to surgically erase - everything
    // else in the maps is just as stale.  If the new save has someone mounted,
    // TryRestoreOrphanedMount rebuilds it from the native carry link.
    if (!riderToMount.empty() || !pendingMount.empty())
    {
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
            return;
        }
    }

    // 0) Service pending "approach then mount" requests.  Each rider that was ordered
    //    to mount from out of range is walking toward its animal; board it the moment
    //    it enters order range.  Re-issue the destination periodically so the rider
    //    keeps chasing a mount that is itself wandering, and drop the request if the
    //    animal becomes invalid, gets taken by another rider, or can't be reached.
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
            // before pathing starts) can't board instantly.
            bool reached = move && move->isDestinationReached() && it->second.age > 20;

            // Calibration log (~every 0.5s while approaching): shows whether the rider is
            // actually closing the distance (i.e. setDestination drives movement) and at
            // what distance arrival trips.  Remove once the approach is confirmed.
            if ((it->second.age % 30) == 1)
            {
                char b[160];
                _snprintf_s(b, sizeof(b), _TRUNCATE,
                    "Riding: approach d=%.1f reached=%d age=%d",
                    d, (move && move->isDestinationReached()) ? 1 : 0, it->second.age);
                DebugLog(b);
            }

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

    // 1) Keep riders still + upright on the mount's back.
    //    We halt the rider every frame (keeps it still while the mount walks) and,
    //    for mounts with forceSit, re-assert the sitting pose every frame.  The
    //    carry system forces the ANIM_CARRIED "lying flat" pose each frame; like the
    //    forceWalk fix for pack_beast, re-running the sit anim after the game's own
    //    update overrides it.
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

    // 1b) Apply the computed seat position.  We run after the game's own update,
    //     so our offset on top of the slave attachment sticks until next frame.
    //     Skipped when there is nothing to correct (small/medium animals).
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

            if (seat.seatMode == SEAT_EXACT && seat.lift == Ogre::Vector3::ZERO && seat.userOffset == Ogre::Vector3::ZERO)
                continue; // exact bone attach, no correction needed

            // Horizontal tracks the mount instantly (no backward lag).  DampSeatBob
            // scales the vertical run-cycle bob to kSeatBobScale of its natural size.
            Ogre::Vector3 seatPos = ComputeSeatPosition(seat, mount);
            DampSeatBob(mount, seat, seatPos);

            // Continuous diagnostics (Numpad .): log EVERY frame while the mount is
            // moving, comparing the mount's root/back bones, the forward vector, the
            // raw (unsmoothed) target, the smoothed target, and the rider's node.
            if (debugContinuous)
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
                if (moving)
                {
                    AnimationClass* mAnim = mount->getAnimationClass();
                    AnimationClass* rAnim = rider->getAnimationClass();
                    char dbg[1100];
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
                        _snprintf_s(dbg, 1100, _TRUNCATE,
                            "Riding: DBG root=(%.2f,%.2f,%.2f) back=(%.2f,%.2f,%.2f) fwd=(%.2f,%.2f,%.2f) rawT=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f) node=(%.2f,%.2f,%.2f) rMove=(%.2f,%.2f,%.2f) rRoot=(%.2f,%.2f,%.2f) rBip=(%.2f,%.2f,%.2f) nodeQ=(%.2f,%.2f,%.2f,%.2f) rBipQ=(%.2f,%.2f,%.2f,%.2f) move=(%.2f,%.2f,%.2f) anch=(%.2f,%.2f,%.2f) st=%d pelv=(%.2f,%.2f,%.2f)",
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
                            pelvP.x, pelvP.y, pelvP.z);
                        DebugLog(dbg);
                    }
                }
            }

            // The renderer reads the rider's root bone world position (rBip), not the
            // movement position and not rootBonePosition.  _setPositionSimple keeps the
            // movement/collision position on the back; SyncRiderNode solves for the
            // scene node so the RENDERED root bone lands on the seat and stops swinging.
            if (rider->getMovement())
                rider->getMovement()->_setPositionSimple(seatPos);
            AnimationClass* rAnim = rider->getAnimationClass();
            if (rAnim)
                SyncRiderNode(rider, mount, rAnim, seatPos);
        }
    }

    // 1b2) Force the walk/run animation on mounts whose carry mode suppresses it
    //      (pack_beast family detected via "beast walk").  Only while the mount is
    //      actually moving and not in combat, so normal idle/attack anims play.
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

    // 1c) Mount combat + forced dismount.
    //     - Any mount that is down (KO'd) or dead force-dismounts its rider.
    //     - On NECK-mode (large) mounts the rider stays passive: their combat is
    //       suppressed every frame and the mount fights back with its native
    //       animal combat, defending the rider against the rider's attackers.
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

    // 2) mount/dismount/tune keys (numpad, physical key detection via OIS)
    if (key && key->keyboard && ou->player)
    {
        static bool prevNumpad1 = false;
        static bool prevNumpad2 = false;
        static bool prevAdd = false;
        static bool prevSub = false;
        static bool prevMul = false;
        static bool prevDiv = false;
        static bool prevNP3 = false;
        static bool prevNP9 = false;
        static bool prevNP0 = false;
        static bool prevNP5 = false;
        static bool prevNP6 = false;
        static bool prevNP7 = false;
        static bool prevDecimal = false;

        bool numpad1down = key->keyboard->isKeyDown(OIS::KC_NUMPAD1);
        bool numpad2down = key->keyboard->isKeyDown(OIS::KC_NUMPAD2);

        // Debug: Numpad decimal (.) toggles continuous ride diagnostics.  Every ~10
        // frames we log the mount's root/back bone position+orientation, our computed
        // target seat position, and the rider's actual render node, so we can see
        // whether the "fling while running" comes from position or orientation.
        bool decimalDown = key->keyboard->isKeyDown(OIS::KC_DECIMAL);
        if (decimalDown && !prevDecimal)
        {
            debugContinuous = !debugContinuous;
            DebugLog(std::string("Riding: debug continuous ") + (debugContinuous ? "ON" : "OFF"));
        }

        if (numpad1down && !prevNumpad1)
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

        if (numpad2down && !prevNumpad2)
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

        // 3) Live seat tuning (applies to the currently mounted animal).
        //    Rider FACING is not tunable - it always follows the mount's travel direction.
        //    Numpad +/- : up/down 0.1    Numpad */ : forward/back 0.1
        //    Numpad 3/9 : fine up/down 0.02    Numpad 0 : reset this species to 0
        //    Numpad 5   : cycle seat mode (exact -> midpoint -> neck)
        //    Numpad 6   : toggle force-sit on/off
        //    Numpad 7   : cycle rider posture (sit -> stand)
        //    Ctrl+*/ /   : move the seat left/right (lateral)
        if (!prevAdd || !prevSub || !prevMul || !prevDiv || !prevNP3 || !prevNP9 || !prevNP0 || !prevNP5 || !prevNP6 || !prevNP7)
        {
            bool addD = key->keyboard->isKeyDown(OIS::KC_ADD);
            bool subD = key->keyboard->isKeyDown(OIS::KC_SUBTRACT);
            bool mulD = key->keyboard->isKeyDown(OIS::KC_MULTIPLY);
            bool divD = key->keyboard->isKeyDown(OIS::KC_DIVIDE);
            bool np3D = key->keyboard->isKeyDown(OIS::KC_NUMPAD3);
            bool np9D = key->keyboard->isKeyDown(OIS::KC_NUMPAD9);
            bool np0D = key->keyboard->isKeyDown(OIS::KC_NUMPAD0);
            bool np5D = key->keyboard->isKeyDown(OIS::KC_NUMPAD5);
            bool np6D = key->keyboard->isKeyDown(OIS::KC_NUMPAD6);
            bool np7D = key->keyboard->isKeyDown(OIS::KC_NUMPAD7);
            bool ctrlD = key->keyboard->isKeyDown(OIS::KC_LCONTROL) || key->keyboard->isKeyDown(OIS::KC_RCONTROL);

            bool stepUp = (addD && !prevAdd && !ctrlD) || (np3D && !prevNP3);
            bool stepDn = (subD && !prevSub && !ctrlD) || (np9D && !prevNP9);
            bool stepFw = mulD && !prevMul && !ctrlD;
            bool stepBk = divD && !prevDiv && !ctrlD;
            bool stepLatR = mulD && !prevMul && ctrlD;
            bool stepLatL = divD && !prevDiv && ctrlD;
            bool stepRst = np0D && !prevNP0;
            bool stepMode = np5D && !prevNP5;
            bool stepSit = np6D && !prevNP6;
            bool stepPosture = np7D && !prevNP7;

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
                            if (!seat.species.empty())
                            {
                                SpeciesTuning& st = speciesTuning[seat.species];
                                st.forceSit = seat.forceSit;
                            }
                            SaveConfig();
                            DebugLog("Riding: force-sit " + seat.species + " -> " + IntToStr(seat.forceSit ? 1 : 0));
                        }
                        else if (stepPosture)
                        {
                            seat.posture = (seat.posture == POSTURE_STAND) ? POSTURE_SIT : POSTURE_STAND;
                            if (!seat.species.empty())
                            {
                                SpeciesTuning& st = speciesTuning[seat.species];
                                st.posture = seat.posture;
                            }
                            SaveConfig();
                            DebugLog("Riding: posture " + seat.species + " -> " + IntToStr(seat.posture));
                        }
                        else if (stepLatR || stepLatL)
                        {
                            float dLat = stepLatR ? 0.1f : -0.1f;
                            seat.lateral += dLat;
                            if (seat.lateral < -kTuningClamp) seat.lateral = -kTuningClamp;
                            if (seat.lateral >  kTuningClamp) seat.lateral =  kTuningClamp;
                            if (!seat.species.empty())
                            {
                                SpeciesTuning& st = speciesTuning[seat.species];
                                st.lateral = seat.lateral;
                            }
                            SaveConfig();
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
                            DebugLog("Riding: reset " + seat.species + " tuning");
                        }
                        else
                        {
                            float dUp = 0.0f, dFwd = 0.0f;
                            if (stepUp)   dUp = (addD && !prevAdd) ? 0.1f : 0.02f;
                            if (stepDn)   dUp = (subD && !prevSub) ? -0.1f : -0.02f;
                            if (stepFw)   dFwd = 0.1f;
                            if (stepBk)   dFwd = -0.1f;
                            TuneSeat(seat, dUp, dFwd);
                        }
                    }
                }
                else
                {
                    DebugLog("Riding: select the rider, then tune with Numpad +/- * / 3 9 0");
                }
            }
        }

        prevNumpad1 = numpad1down;
        prevNumpad2 = numpad2down;
        prevAdd = key->keyboard->isKeyDown(OIS::KC_ADD);
        prevSub = key->keyboard->isKeyDown(OIS::KC_SUBTRACT);
        prevMul = key->keyboard->isKeyDown(OIS::KC_MULTIPLY);
        prevDiv = key->keyboard->isKeyDown(OIS::KC_DIVIDE);
        prevNP3 = key->keyboard->isKeyDown(OIS::KC_NUMPAD3);
        prevNP9 = key->keyboard->isKeyDown(OIS::KC_NUMPAD9);
        prevNP0 = key->keyboard->isKeyDown(OIS::KC_NUMPAD0);
        prevNP5 = key->keyboard->isKeyDown(OIS::KC_NUMPAD5);
        prevNP6 = key->keyboard->isKeyDown(OIS::KC_NUMPAD6);
        prevNP7 = key->keyboard->isKeyDown(OIS::KC_NUMPAD7);
        prevDecimal = decimalDown;
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

        // The ordersList is parallel to the optionsList children (same count, same
        // order).  Identify the Bodyguard row by its ORDER value rather than caption
        // text (caption matching is fragile: encoding + nested widgets).
        //
        // IMPORTANT: the context-menu ordersList uses a DIFFERENT enumeration than
        // TaskType.  In-game dump (Kenshi 1.0.65) proved: 26=Trade, 31=Bodyguard,
        // 45=Follow, 69=KnockOut, 225=PickUp.  TaskType::BODYGUARD compiles to 45 here
        // (which is Follow in the menu enum), so we must NOT use it - the menu id for
        // Bodyguard is 31.  (newPlayerTask still uses TaskType::BODYGUARD correctly:
        // clicking the order-31 row is dispatched by the game as TaskType 45.)
        const int kMenuOrderBodyguard = 31;
        int n = childCount < orderCount ? childCount : orderCount;
        for (int i = 0; i < n; ++i)
        {
            int order = ordersList[i];
            MyGUI::Widget* child = opts->getChildAt(i);
            if (!child) continue;

            // per-row mapping dump, only when continuous diagnostics is on (Numpad .)
            if (debugContinuous)
            {
                try { std::string cap = GetWidgetCaptionDeep(child);
                      char drow[256]; _snprintf_s(drow, 256, _TRUNCATE, "Riding:   row %d order=%d type=%s cap='%s'", i, order, child->getTypeName().c_str(), cap.c_str()); DebugLog(drow); } catch(...) {}
            }

            if (order != kMenuOrderBodyguard) continue;   // only touch the Bodyguard row

            // Only rename for rideable player-owned animals.  For everything else
            // (humans, wild animals) leave the menu completely untouched so vanilla
            // "Bodyguard" keeps working - no hiding, no layout change.
            if (!rideable) continue;

            // rename Bodyguard -> 上马 (UTF-8 bytes; MyGUI captions are UTF-8)
            bool ok = SetWidgetCaptionDeep(child, "\xE4\xB8\x8A\xE9\xA9\xAC");
            try { opts->_updateChilds(); } catch(...) {}
            try { DebugLog(std::string("Riding: rename Bodyguard->shangma idx=") + IntToStr(i) + (ok ? " ok" : " FAILED(no caption widget)")); } catch(...) {}
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
