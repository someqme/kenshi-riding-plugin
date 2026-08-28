#include <Debug.h>

#include <kenshi/Character.h>
#include <kenshi/CharMovement.h>
#include <kenshi/RaceData.h>
#include <kenshi/GameData.h>
#include <kenshi/GameWorld.h>
#include <kenshi/Globals.h>
#include <kenshi/PlayerInterface.h>
#include "RidingContextMenu.h"
#include <kenshi/InputHandler.h>
#include <kenshi/Animation/AnimationClass.h>
#include <kenshi/Animation/AnimationClassHuman.h>
#include <kenshi/Building/Building.h>
#include <kenshi/Enums.h>
// Vanilla rebindable-keybinding support: DatapanelGUI::addCustomLine lets us inject
// a DataPanelLine_KeyConfig row into the Settings->Controls page, and
// InputHandler::loadConfig is where we register our commands so their bindings
// persist to controls.cfg.  Both are copied from RE_Kenshi/MiscHooks.cpp (the loader
// itself does exactly this for its "Toggle Free Camera mode" row).
#include <kenshi/gui/DatapanelGUI.h>
#include <kenshi/gui/DataPanelLine.h>

#include <ois/OISKeyboard.h>

#include <core/Functions.h>

#include <ogre/OgreVector3.h>
// OgreSceneNode.h is load-bearing: AnimationClass::node is an Ogre::SceneNode* and
// no other included header pulls the complete type in transitively.
#include <ogre/OgreSceneNode.h>

#include <mygui/MyGUI_Window.h>

#include <boost/unordered_map.hpp>
#include <string>
#include <vector>

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
// Mounting is done from the context menu ("Ride", the renamed bodyguard order).
// Dismount is via the right-click "put down" order (no dedicated hotkey).
// Seat tuning uses rebindable vanilla commands registered in ridingLoadConfig_hook
// (see Settings->Controls); defaults are the numpad layout in HotkeyPass.
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
//   3 = REAR     - the pelvis (root bone as fallback), i.e. the animal's hindquarters.
//                  For huge animals the NECK anchor is the WORST choice available even
//                  though it is the only one that clears the body: measured over a 7727-
//                  frame straight-line Leviathan ride (travel frame, relative to the
//                  gait-free CharMovement position), per-frame sideways jerk was
//                  root 0.097 < pelvis 0.137 < Spine2 0.413 < the live seat 0.571.  The
//                  seat is jitterier than ANY bone because mode 2 anchors it to the neck
//                  and then levers it 44.3 units back down the body: 44.3 * sin(1 degree)
//                  = 0.77 units of sideways travel for every degree the skeleton's
//                  forward axis wobbles.  Anchoring at the rear puts the seat where the
//                  player asked for it (the hindquarters read as "the part that holds
//                  still") AND deletes the lever, so the tuned offsets collapse to small
//                  numbers and seat jitter drops to the anchor bone's own ~4x lower
//                  figure.  Opt-in per species via riding.cfg column 1.
enum SeatMode
{
    SEAT_EXACT    = 0,
    SEAT_MIDPOINT = 1,
    SEAT_NECK     = 2,
    SEAT_REAR     = 3
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
    std::string   species;       // mount->getName() - the LOCALIZED species name.  Still the
                                 // primary tuning key and the key for every name-scoped
                                 // behaviour below (neckFollow, the dog family, the blocklist).
    std::string   raceKey;       // mount's race stringID ("3998-gamedata.base"), "" if unreadable
    std::string   tuneKey;       // the speciesTuning key actually serving this mount: `species`
                                 // when a name row exists, else `raceKey` when a race row does.
                                 // Everything that WRITES tuning back (anchor/baseline capture,
                                 // the tuning keys, cfg persistence) must use this, not species -
                                 // otherwise a race-served mount would silently fork a new
                                 // name row the moment its anchor was captured.
    std::string   backBone;      // bone used for slave attach (orientation + fallback anchor)
    std::string   frontBone;     // front torso anchor bone (Spine2/Spine1/Spine/Head)
    std::string   rearBone;      // rear torso anchor bone (Pelvis), "" if none
    int           seatMode;      // SeatMode (0=exact 1=midpoint 2=neck 3=rear)
    bool          forceWalk;     // mount's walk animation is suppressed by carry mode (pack_beast)
    bool          rootAnchor;    // anchor to root bone instead of the swinging back bone
                                 // (fling skeletons: Crab/robot_worker/dog/gorilla/Crimper/beak)
    bool          neckFollow;    // 卷缩者 only: vertical follows the NECK bone (butt rides
                                 // up/down with the neck), no bob damping; horizontal still root
    bool          flexTrack;     // dog family: vertical = LIVE mean of front+rear bone offsets
                                 // above root - their waist see-saws against the butt/root, so a
                                 // bind-pose constant glues the rider to one end of the flex while
                                 // the other sweeps under them; the mean cancels anti-phase swing
    bool          forceSit;      // rider: re-assert the sitting pose every frame (overrides carried prone)
    int           posture;       // RiderPosture (0=sit, 1=stand)
    float         lateral;       // side offset (right/left), world units
    float         torsoLen;      // front<->rear bone distance at mount time (world units)
    Ogre::Vector3 lift;          // base seat offset (mostly +Y, auto-sized)
    Ogre::Vector3 userOffset;    // live-tuned delta on top of lift (x = forward, y = up)
    Ogre::Vector3 homeOffset;    // the player's declared "zero" for this species (2026-08-26):
    float         homeLateral;   // Numpad9 restores userOffset/lateral to these instead of
                                 // clearing to the bare geometric base, so a dialled-in seat
                                 // survives experimenting.  Ctrl+Numpad9 commits the current
                                 // position here.  Seeded from riding.cfg cols 15-17.
    float         sizeScale;     // k = liveScale / refScale, the per-individual adaptation
                                 // factor applied to the TUNED offsets (never to the bone
                                 // anchor, which scales with the mount's node by itself,
                                 // and never to the columns 11-13 pose constants, which
                                 // belong to the human rider).  1.0 = no adaptation, which
                                 // is what an unknown refScale or an unreadable live scale
                                 // falls back to - see kSeatUpConstB for the law and
                                 // SeatSizeRatio for the guards.
    float         refScale;      // the body size these tuned numbers were CONFIRMED at
                                 // (riding.cfg column 18); 0 = unknown -> no adaptation
    float         liveScale;     // this individual's size, read at mount time; 0 = unreadable
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

// Debug: toggled with Ctrl+Numpad . for continuous ride diagnostics (every 10 frames).
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
    // Stationarity watch for the DampSeatBob baseline (2026-08-24).  The old belief
    // that "running keeps the stability counter from accumulating" is FALSE for slow,
    // shallow bobs (Beak Thing: +-0.33u, per-frame delta well under the 0.35 tolerance)
    // - the counter filled WHILE RUNNING, so the stale-baseline wipe + recapture fired
    // mid-gallop and every gait change flipped the baseline by ~2.2u = a visible
    // one-frame vertical JUMP (8 captures in 3s in the field log).  These fields watch
    // the mount's MOVEMENT position over discrete windows instead: only a full window
    // with negligible translation counts as "still", and baseline mutation (stale wipe
    // or fresh capture) is allowed ONLY then.
    Ogre::Vector3 winAnchor;      // movement pos sampled at last window boundary
    bool          winValid;       // winAnchor holds a real sample
    int           winCountdown;   // syncs until next window boundary
    int           stillWindows;   // consecutive stationary windows
    int           syncCount;      // syncs since this ride's tracking started (ride age)
    CapTrack() : prevRel(Ogre::Vector3::ZERO), prevBase(0.0f), relStable(0), baseStable(0),
                 winAnchor(Ogre::Vector3::ZERO), winValid(false), winCountdown(0), stillWindows(0),
                 syncCount(0) {}
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
// DBG only: (movement position read straight back) - (seat we just wrote), per rider.
// _setPositionSimple runs on a movement that pickupObject DESTROYED, so "did the write
// land at all" and "did somebody move it afterwards" are two different questions and the
// node-style drift meter above cannot separate them.  This one is sampled in the same
// breath as the write, so a nonzero value means the call itself did not take.
boost::unordered_map<Character*, Ogre::Vector3> dbgMoveWritten;

// Pending "approach then mount" requests.  When the player picks "上马" (the repurposed
// Bodyguard menu order) on an animal, the rider does NOT teleport onto it if it is far
// away.  Instead it paths toward the animal - exactly like the native "pick up" order -
// and only actually boards once it is within order/interaction range.  This map holds
// those in-flight requests; mainLoop_hook services them every frame.
struct PendingMount
{
    Character*    mount;   // the animal the rider is walking toward
    int           age;     // frames since the request (timeout guard)
    int           refresh; // frames since the destination was last re-issued (mount may roam)
    // Where the rider was last told to walk to, and how long it has been standing
    // still.  Re-issuing a destination is NOT free (see kMountRepathGap below), so
    // these two decide when a re-path actually buys something.
    Ogre::Vector3 destPos;
    int           idle;    // consecutive frames with (almost) no movement
    // Approach progress (2026-08-26).  A fixed "close enough" radius cannot board a
    // Leviathan: its collision hull parks the rider ~60 units from the body centre and
    // no distance test on the centre will ever pass.  These fields let the arrival test
    // be scale-free instead - "the rider walked and can get no closer" - without
    // re-opening the destroyed-movement hole (a rider whose CharMovement was destroyed
    // reports "destination reached" forever but never actually moves, so `moved` stays 0).
    Ogre::Vector3 lastPos;  // rider ground position last frame (travel accumulator)
    float         moved;    // total horizontal distance walked since the request
    float         bestDist; // closest approach to the mount centre seen so far
    int           stall;    // frames since bestDist last improved
    // Distance walked since bestDist last improved (2026-08-27).  `moved` above answers
    // "did this rider ever walk", which the envelope needs; this answers "is it walking
    // right now and getting nowhere", which is what being blocked by a body looks like.
    // Reset together with `stall`, so a rider that keeps closing in never accumulates any.
    float         pressMoved;
    int           report;   // frames since the last approach diagnostic line
    // Consecutive re-path requests that bought no progress.  Every setDestination cancels
    // the path request the previous one queued, so a rider that cannot path must be given
    // progressively MORE time, not hammered (see the back-off below).  Reset by any real
    // progress (bestDist improving).
    int           repaths;
    int           envAge;   // frames since the envelope was last re-measured
    // How close the rider has to be before ANY arrival signal may board it, measured
    // from this animal's own body length at order time (see MountBoardEnvelope).  The
    // scale-free signals below are about "cannot get closer to THIS body", so they need a
    // body-sized sanity bound: without one an engine-side "destination reached" that
    // really means "your path was cancelled" boards the rider from across the map.
    // Re-measured while the rider closes in (kMountEnvelopeGap) because a distant
    // animal's bones are not live yet - the order-time reading is usually the floor.
    float         envelope;
};
// rider -> pending approach-to-mount
boost::unordered_map<Character*, PendingMount> pendingMount;
// Give up approaching after this many frames (the mount may be unreachable).
static const int kMountApproachTimeout = 1800;
// Re-pathing policy (2026-08-26).  Character::setDestination clears the rider's tasks
// and queues a FRESH path request, and the rider stands still until that path comes
// back.  With one rider en route the recompute is invisible; with a second mount order
// in flight the requests queue up behind each other and the pause becomes visible - the
// reported symptom was "order a second character to mount and the first one stops for a
// moment mid-approach".  A blanket every-N-frames re-issue therefore costs more than it
// buys, so we re-path only when it changes something: the mount has walked away from the
// point we aimed at, or the rider stopped walking and needs a new path.  Either way at
// most one request per kMountRepathGap frames.
static const int   kMountRepathGap = 20;
// The mount has to have drifted this far (world units, horizontal) from the destination
// we last issued before chasing it is worth a new path request.
static const float kMountDestMoveEps = 4.0f;
// Per-frame travel below this counts as "not walking"...
static const float kMountStepEps = 0.02f;
// ...and this many consecutive standing-still frames mean the rider lost its path
// (something cancelled the order, or the path ended short) - re-issue it promptly, on
// its own shorter gap, so an externally cancelled approach resumes before the player
// can see it stop.
static const int   kMountIdleFrames = 6;
static const int   kMountIdleRepathGap = 8;
// Before the rider has proven it can walk (kMountMovedProof) standing still is ambiguous:
// it may simply be waiting for its first path, and a request every few frames would cancel
// and restart that computation forever.  So in that state wait this long (~1s) before
// assuming the order was lost, and use the same value as the gap between attempts.  This is
// what keeps the old "an order that never took effect still recovers" safety net.
static const int   kMountStartIdleFrames = 60;
// Horizontal distance (world units) at which the rider is deemed "walked up to" the
// mount and boards.  Generous so it fires reliably for medium/large animals; small
// animals just board from a touch farther.  Huge animals (Leviathan) can never satisfy
// it - their hull stops the rider tens of units from the body centre - so the two
// scale-free arrival signals below take over for them.
static const float kMountArriveDist = 10.0f;
// Proof the rider actually walked this far since the request.  It no longer decides
// WHETHER the scale-free signals may board (the envelope below does that - it is the bound
// that keeps the destroyed-movement rider from boarding across the map), only how much
// patience they get: a rider that walked may still be rounding an obstacle, one that never
// took a step is already parked where it will stay.  Gating permission on it was wrong -
// a rider ordered onto an animal it is ALREADY standing next to never walks at all, so it
// could never board (log: d=10..18 moved=0 for 1800 frames, then timeout).
static const float kMountMovedProof = 4.0f;
// Boarding also fires when the rider stops making progress: pressed against a big hull
// the pathfinder may keep walking in place without ever raising its "reached" flag.
// bestDist has to improve by this much to count as progress...
static const float kMountProgressEps = 0.35f;
// ...and this many stalled frames (~2s) mean the rider is as close as it will ever get.
static const int   kMountStallFrames = 120;
// A rider that has never walked (it was ordered while already standing at the animal) has
// nothing to wait for: this much standing still inside the envelope IS the arrival.  Short
// (~0.25s) because the alternative is the 2s stall wait for someone already in place.
static const int   kMountSettleFrames = 30;
// "Pressing against the body" (2026-08-27) - the envelope's escape hatch for animals whose
// footprint has nothing to do with their torso length.  A swamp turtle's shell parks the
// rider at d=40 while its spine bones measure small enough that the envelope stays on its
// 30-unit floor, so both scale-free signals stayed blocked and it timed out every time
// (in-game log: d=38..48, moved climbing 59 -> 182 while best never improved past 38, six
// consecutive "approach to mount timed out").  Walking hard and getting no closer is itself
// scale-free proof that something solid is in the way, and it separates cleanly from the
// failure the envelope exists to reject: a rider whose path was cancelled (or whose
// movement was destroyed) stands STILL - its travel accumulator freezes.  So: this much
// distance walked since the closest approach...
static const float kMountPressProof = 12.0f;
// ...while still parked at that closest approach (a rider chasing an animal that is simply
// walking away is not being blocked by it, and must not board from across the gap)...
static const float kMountPressSlack = 6.0f;
// Body-sized boarding envelope (2026-08-26).  Neither scale-free signal may board a rider
// that is not actually AT the animal.  The bound is the mount's own torso length (front
// bone <-> rear bone at order time): a bull measures ~10-15 so nothing changes for it,
// a Leviathan measures ~85 which comfortably contains the ~60 units its hull parks the
// rider at.  Clamped at both ends - kMountArriveDist as the floor, and this ceiling to
// survive the known "bone reads come back in unscaled space" window (a 10x read would
// otherwise hand out a 850-unit licence to teleport).
static const float kMountEnvelopeMax = 120.0f;
// ...and this floor, because torsoLen (front bone <-> rear bone) is NOT the animal's
// footprint: a spider measures 0.5-5 with legs that hold the rider 18 units out, so the old
// kMountArriveDist floor made the envelope smaller than the standoff it had to contain.
// In-game proof: a rider walked 530 units in, the hull parked it at exactly d=18 with
// env=14, stall climbed past 700 frames and it timed out without ever boarding - "slightly
// larger animals can no longer be mounted".  30 units is still unmistakably "standing at
// the animal" (the bogus cross-map "reached" this bound exists to reject came in at 200+).
static const float kMountEnvelopeMin = 30.0f;
// ...and this is the pad on the RADIUS term, which the 2026-08-28 session showed is the
// term that actually predicts where the rider stops.  rad= (Character::getRadius) against
// the parked distance, one row per species from that log:
//   6.6->~10   6.8->10   15.2->18.5   20.7->24   21.7->27   22.0->25   34.9->38
// plus two older rows (25.5->29, 7.0->10.5).  Nine points spanning a 5x size range, all
// parking at radius + 3.0..3.5, so 5.0 leaves ~1.5u of margin without loosening the bound
// enough to matter (the cross-map bogus "reached" this gate rejects came in at 200+).
static const float kMountRadiusPad = 5.0f;
// ...and re-measured this often while the rider walks in.  At order time the animal is
// usually far away and its bones are NOT live: an in-game log had a blood spider that
// reports torso=18 the instant it is boarded measure ~0 when the order was given, so the
// envelope collapsed to its floor.  Harmless for small animals (they board on plain
// distance) but fatal for a Leviathan, whose hull parks the rider ~60 units out - outside
// a floored envelope both scale-free signals stay blocked and it can never board.  Keep
// the widest sane reading seen: by the time the rider is close the skeleton is live, and
// MountBoardEnvelope's own ceiling still bounds a garbage unscaled-space read.
static const int   kMountEnvelopeGap = 15;
// A rider that reports "destination reached" while still outside the envelope did not
// arrive - something cancelled its path (proven cause: issuing a mount order to a second
// character invalidates the first one's in-flight path request; before this it showed up
// as the first rider hitching, and once the blanket re-path was removed it turned into
// "the first rider boards instantly from any distance").  Treat it as a lost path and
// re-issue the destination on this gap instead of boarding.
static const int   kMountReachedRepathGap = 24;

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
    // User-defined "zero"/home (2026-08-26): the position the player declares as the
    // reset target.  Numpad9 restores offset/lateral to this instead of clearing to the
    // bare geometric base; Ctrl+Numpad9 commits the current position as the new home.
    // Persisted in riding.cfg columns 15-17.  Old cfg files (no home columns) migrate by
    // seeding home = the already-tuned offset, so the player's existing good spot becomes
    // the zero automatically.
    Ogre::Vector3 home;          // (x = forward, y = up), same layout as offset
    float         homeLateral;
    // The body size offset/lateral/base were CONFIRMED correct at (riding.cfg column 18,
    // 2026-08-28).  0 = unknown, which disables adaptation for this species and leaves it
    // behaving exactly as it did before the feature existed.  A species acquires one
    // automatically the first time the player tunes it (PersistTuning adopts the live
    // size), so no hand-editing is needed for anything ridden after an update.
    float         refScale;
    SpeciesTuning() : seatMode(SEAT_MIDPOINT), forceSit(true), posture(POSTURE_SIT), lateral(0.0f), offset(Ogre::Vector3::ZERO), anchor(Ogre::Vector3::ZERO), base(0.0f), home(Ogre::Vector3::ZERO), homeLateral(0.0f), refScale(0.0f) {}
    SpeciesTuning(int m, const Ogre::Vector3& o) : seatMode(m), forceSit(true), posture(POSTURE_SIT), lateral(0.0f), offset(o), anchor(Ogre::Vector3::ZERO), base(0.0f), home(Ogre::Vector3::ZERO), homeLateral(0.0f), refScale(0.0f) {}
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
    dbgMoveWritten.clear();
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
// Default tuning range and per-keypress step.  Both scale up for animals bigger than the
// range itself - see SeatTuneLimit / SeatTuneStep.
const float kTuningClamp   = 15.0f;
const float kTuneStep      = 0.1f;
// Extra range given to the giants on top of their own torso length.  One torso length is
// NOT enough headroom: the Leviathan's pelvis sits 134 units above its rigid body but the
// visible hump above it is taller still - at the full 85.37 (its old ceiling, = torsoLen)
// the rider was buried up to the waist.  Vertical clearance is unrelated to how LONG the
// animal is, so the ceiling has to be generous rather than exact.  Species inside the
// default +-15 are unaffected (the headroom only applies once torsoLen exceeds it).
const float kTuneHeadroom  = 2.0f;

// --- per-individual size adaptation (2026-08-28) ---------------------------
// A seat is tuned by hand on ONE animal, but a Kenshi character record carries a scale
// RANGE, not a fixed value ("scale min"/"scale max": crab .10-1.00, leviathan .40-1.50),
// so two animals with the same name can differ several-fold and one hand-measured offset
// only fits the individual it was measured on.  Each seat therefore records the size it
// was CONFIRMED correct at (refScale, cfg column 18) and the live individual's size is
// read at mount time; the ratio k = live/ref adapts the tuned numbers.
//
// ⚠️ The vertical term is NOT a proportional scale.  The height a rider needs above the
// anchor bone is
//        up(scale) = A * scale + B
// with B a UNIVERSAL constant: the rider's OWN sit height, which does not scale because
// the rider is always the same human (it is the same ~6.36 that lands in cfg column 12).
// Only "how far above the anchor this animal's back sits" scales.  So there is a constant
// term in up, and naive proportional scaling (up * k) is wrong in principle.  Rearranged
// for adaptation from one confirmed sample:
//        A = (up_ref - B) / ref        ->   up(live) = (up_ref - B) * k + B
// Regression against the 12 same-race pairs that had ALREADY been hand-tuned separately
// (predicting each family's other members from its smallest one, sizes from the live nsc=
// readings): this law lands within 0.07-0.34u, naive proportional scaling misses by up to
// 7.97u.  The decisive case is 沼泽沼泽速龙 1.482 -> 巨型沼泽速龙 3.350, a 2.26x span
// where the law predicts 7.61 against the hand-tuned 7.52 while proportional gives -0.45.
// (Two documented non-refutations: 驮牛->野牛 0.69u on a 0.70->0.96 span where A is
// ill-conditioned, and the two tiny robot spiders whose hand values differ by 2.6u on an
// animal 1u long - a pure taste difference, not a size effect.)
//
// forward / lateral scale PROPORTIONALLY: horizontally the rider is a point, so there is
// no rider constant to subtract out.  Same for the bob baseline (cfg column 14), which
// DampSeatBob measures with the user's height tune already stripped off - it is purely
// the animal's own geometry above the bob reference.
const float kSeatUpConstB = -6.4f;     // B: the rider's own sit height, world units
                                       // (negative - the rider's root bone sits BELOW the
                                       // seat point, which is why up goes more negative on
                                       // smaller animals rather than toward zero)
// k is clamped to the widest range any FCS scale bracket can produce (crab .10-1.00 = 10x
// against a reference near 1.0), which still separates cleanly from a garbage read.  Bone
// world reads land in UNSCALED space (~10x) for the first frames after a mount - see
// HISTORY §C, the failure that killed per-individual scaling the first time round - but
// this is the scene NODE scale, a different data path: the 2026-08-28 diagnostic found
// nsc == lsc on every mount, no spike at the mount instant, 570 consecutive stable frames,
// and readings that reproduce the FCS brackets one for one.  The clamp and the sanity
// checks in SeatSizeRatio are the guard rails, not the plan.
const float kSizeRatioMin  = 0.1f;
const float kSizeRatioMax  = 10.0f;
// Below this a scale reading is treated as "not available" rather than as a real size.
const float kSizeReadMin   = 0.02f;

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

// The race record is the only language-independent species key the engine offers us.
// getName() returns the LOCALIZED ANIMAL_CHARACTER name, which is why kDefaultSeats[]
// has to spell Chinese names out in \xNN escapes and why the same tuned seat has to be
// duplicated for every character that happens to share a skeleton.  Every animal points
// at a RaceData, and RaceData::data->stringID (e.g. "3998-gamedata.base") is the same
// byte string in every locale AND is shared by every character built on that
// skeleton/mesh/collision radius - read straight out of the FCS binaries, race
// 3998-gamedata.base = Pack Beast / Pack Beast Dead / Garru, 56089-Newwworld.mod = the
// whole crab family, 3992-gamedata.base = Goat / Cornelius / Beloved Goat.  That is
// exactly the identity a riding seat wants.
//
// Seats are STILL keyed by name; this is logged at mount time so one game session tells
// us the race key of every species the player rides, and so we can confirm these two
// raw field reads are safe on live characters before anything depends on them.
static std::string GetRaceKey(Character* c, std::string* nameOut = 0)
{
    if (nameOut) nameOut->clear();
    if (!c) return "";
    try
    {
        RaceData* rd = c->_NV_getRace();
        if (!rd) return "";
        GameData* gd = rd->data;
        if (!gd) return "";
        if (nameOut) *nameOut = gd->name;
        return gd->stringID;
    }
    catch (...) { return ""; }
}

// --- individual body size (LIVE, 2026-08-28) -------------------------------
// A seat is tuned by hand on ONE individual, but a character record carries a scale
// RANGE, not a fixed value ("scale min"/"scale max" in the FCS binaries: crab .10-1.00,
// leviathan .40-1.50), so two animals with the SAME name can differ several-fold in size
// and a hand-measured offset only fits the one it was measured on.  This is the input to
// seat.sizeScale - see kSeatUpConstB for the adaptation law and why the vertical term is
// affine rather than proportional.
//
// KenshiLib exports no scale accessor (AnimationClass offers only getSceneNodePosition /
// getSceneNodeOrientation), but AnimationClass::node is a public Ogre::SceneNode* and
// Node::getScale() / _getDerivedScale() are right there.  Both are logged: LOCAL tells us
// whether the per-individual scaling lives on the animal's own node, DERIVED tells us the
// total including any ancestor.  DERIVED is the one the adaptation uses.
//
// This was diagnostic-only for one session first, on purpose, because HISTORY §C is the
// record of per-individual scaling being removed after mount/load-instant reads came back
// in UNSCALED space (~10x).  That objection turned out not to reach this data path - it is
// about BONE world reads, not node scale - and the 34250-line session that watched exactly
// that window found nsc == lsc on every mount, bsc flat at 1.0 (size is not in bone
// scale), the human rider's own reading pinned at 1.000 as a control, no 10x spike
// anywhere including the mount instant, and values that reproduce the FCS brackets one for
// one.  That is what promoted it from diagnostic to load-bearing.
static Ogre::Vector3 ReadNodeScale(AnimationClass* a, bool derived)
{
    if (!a || !a->node) return Ogre::Vector3::ZERO;
    try
    {
        return derived ? a->node->_getDerivedScale() : a->node->getScale();
    }
    catch (...) { return Ogre::Vector3::ZERO; }
}

// Third data point, static so it is only worth logging once per mount: Kenshi's own
// per-bone scale.  If the size lives here instead of on the scene node, the node reads
// will come back 1.0 for every animal and this will not.
static Ogre::Vector3 ReadBoneScale(AnimationClass* a, const std::string& bone)
{
    if (!a) return Ogre::Vector3::ZERO;
    try
    {
        if (!a->getHasBone(bone)) return Ogre::Vector3::ZERO;
        return a->getBoneScale(bone);
    }
    catch (...) { return Ogre::Vector3::ZERO; }
}

// scale printed as x1000 ints: the values sit near 1.0, so the x100 convention the rest
// of the logging uses would quantise away exactly the differences we are looking for.
static std::string ScaleToStr(const Ogre::Vector3& s)
{
    return "(" + IntToStr((int)(s.x * 1000.0f)) + ","
               + IntToStr((int)(s.y * 1000.0f)) + ","
               + IntToStr((int)(s.z * 1000.0f)) + ")";
}

// One scalar size for a mount, or 0 for "could not read it".  Kenshi scales animals
// uniformly (every reading in the diagnostic session had x == y == z), so a reading whose
// components disagree is not a body size we understand - refuse it rather than guess which
// axis the seat should follow.
static float ReadMountSize(Character* mount)
{
    if (!mount) return 0.0f;
    Ogre::Vector3 s = ReadNodeScale(mount->getAnimationClass(), true);
    if (s.y < kSizeReadMin) return 0.0f;
    if (fabsf(s.x - s.y) > 0.05f * s.y || fabsf(s.z - s.y) > 0.05f * s.y) return 0.0f;
    return s.y;
}

// k for a (reference size, live size) pair.  Returns exactly 1.0 whenever adaptation must
// not happen - unknown reference, unreadable live size, or a ratio outside anything an FCS
// scale bracket can produce - so every caller can multiply unconditionally and a species
// with no reference behaves bit-for-bit as it did before this feature.
static float SeatSizeRatio(float refScale, float liveScale)
{
    if (refScale < kSizeReadMin || liveScale < kSizeReadMin) return 1.0f;
    float k = liveScale / refScale;
    if (k < kSizeRatioMin || k > kSizeRatioMax) return 1.0f;
    return k;
}

// --- the adapted seat numbers ----------------------------------------------
// Everything that consumes a tuned value goes through these three, so the stored numbers
// stay in the REFERENCE frame (that is what riding.cfg holds and what the tuning keys
// edit) while every consumer sees the value for the individual actually being ridden.
// ⚠️ ComputeSeatPosition and DampSeatBob must agree to the bit: DampSeatBob strips the
// height tune off the seat to isolate the animal's own bob and adds it back afterwards, so
// if the two disagreed about what "the height tune" is the difference would land straight
// in the rider's altitude.
static float SeatUp(const SeatInfo& seat)
{
    if (seat.sizeScale == 1.0f) return seat.userOffset.y;    // exact, not merely equal
    return (seat.userOffset.y - kSeatUpConstB) * seat.sizeScale + kSeatUpConstB;
}

static float SeatForward(const SeatInfo& seat)
{
    return seat.userOffset.x * seat.sizeScale;
}

static float SeatLateral(const SeatInfo& seat)
{
    return seat.lateral * seat.sizeScale;
}

// The bob baseline (cfg column 14) is measured with the height tune already stripped off,
// so it is pure mount geometry above the bob reference and scales proportionally - no
// rider constant to carry.
static float SeatBase(const SeatInfo& seat, float storedBase)
{
    return storedBase * seat.sizeScale;
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

// --- built-in default seats (2026-08-26, one row per RACE since v9) --------
// A player who downloads the mod must ride correctly on the very FIRST mount, without
// tuning anything: these are the seats dialled in in-game by the author, compiled into
// the DLL.  riding.cfg therefore becomes purely the player's own override file - it does
// not have to ship with the mod, and deleting it restores exactly this table.
//   * KEYS ARE RACE stringIDs ("3998-gamedata.base"), not localized animal names.  v8's 42
//     name rows were 42 hand-tuned seats for these same 21 races: the duplicates existed
//     only because a seat could not follow body size, so every scaled-up relative needed its
//     own numbers.  Column 18 (v7) removed that need, so v9 keeps ONE standardized row per
//     race and lets the size law cover the rest.  Where two relatives' hand-tuned numbers
//     disagreed the author chose the survivor: bull family -> 野牛 (not 驮牛), robot spiders
//     -> 铁蜘蛛 and 安全蜘蛛 (one row per race, they are two different races), Crimper family
//     -> 卷缩者 (not 国王), crab family -> the small-crab mode-2 seat.
//   * ⚠️ THE CRAB ROW COVERS TWO SCALE BRACKETS TUNED IN DIFFERENT SEAT MODES.  56089's
//     .10-1.00 members (螃蟹/提多) were tuned on the NECK anchor, its 1.5-2.25 members
//     (巨型螃蟹/螃蟹终结者/巨蟹先生/巴纳布斯) on the EXACT anchor, and the size law only
//     converts within one anchor frame - so the four big crabs are misplaced until someone
//     re-tunes them.  The author picked the small-crab seat knowingly.  The fix is to ride a
//     big crab and tune it (that writes a NAME row, which wins over this race row), not to
//     rescale these numbers.
//   * a name-keyed row still overrides its race row - that is how a player gives one animal
//     its own seat, and it is what the tuning keys write.  BuildSeatInfo looks up the name
//     first and the race second; this table simply no longer ships any name rows.
//   * a race row is the ONLY layer that works on a non-Chinese install, where getName()
//     returns "Garru" and no name row could ever match.
//   * every race key here is CONFIRMED, never inferred: from the plugin's own mount-log
//     race= field, or from an offline scan of data\*.base/.mod that reproduces those same
//     pairings exactly.  The numeric half of a stringID is unique only WITHIN the file that
//     created the record and the suffix is that file's name kept forever (hence
//     small_changes_otto.mod), so a suffix can never be guessed - two such guesses were
//     already disproved (the raptors are 42071-small_changes_otto.mod, NOT 42068-gamedata.base
//     which is an animation, and 42068-small_changes_otto.mod is the Cage Beast).
//   * home == the tuned offset, so Numpad9 always returns to the shipped default seat;
//     Ctrl+Numpad9 still lets a player declare a zero of their own.
//   * columns 11-14 (rider anchor + bob baseline) are shipped too, so frame one of the
//     first ride is already placed instead of drifting in during capture.  A 0 there
//     just means "was never captured", and the runtime captures it as before.
//   * keys are pure ASCII now, but any Chinese literal added to this file would still have to
//     be an explicit \xNN UTF-8 escape - a plain one compiles to GBK and never matches
//     getName() (see the encoding note near kNonRideable).  Regenerate rows by dumping a
//     tuned riding.cfg through tools\gen_default_seats.py, never by hand.
//   * column 18 (the last field) is the body size the row was CONFIRMED correct at, which
//     is what lets one tuned individual cover every size in its race - see kSeatUpConstB.
//     It is NOT a guess: every non-zero value here is an nsc= reading logged on a mount the
//     player then reported as correctly seated.  0 means "never measured", and such a row
//     is used unadapted, exactly as it was before the feature existed - which is harmless
//     for the one row that has it (42070's scale is fixed 1.0-1.0).
//   * the frog_gorilla family is deliberately absent: it stays in kNonRideable, so no seat
//     of ours would ever be used.
struct DefaultSeat
{
    const char* species;
    int   mode;                  // SeatMode (cfg column 1)
    float up, forward, lateral;  // the tuned seat, and its home
    int   posture;               // POSTURE_SIT / POSTURE_STAND
    int   sit;                   // force-sit
    float ax, ay, az, base;      // captured constants (cfg columns 11-14), 0 = capture live
    float ref;                   // body size these numbers were confirmed at (column 18),
                                 // 0 = unknown -> this species is never size-adapted
};

static const DefaultSeat kDefaultSeats[] = {
    { "2860-gamedata.base", 2,   -1.20f,   -3.50f,   0.00f, 0, 1,  0.414f,  6.363f, -0.561f,   6.039f, 0.999f },  // 2860-gamedata.base
    { "3966-gamedata.base", 1,   69.84f,  -16.50f,   0.00f, 0, 1, -0.527f,  6.364f,  0.168f,   0.608f, 1.394f },  // 3966-gamedata.base
    { "3976-gamedata.base", 1,   -5.40f,    0.20f,   0.00f, 0, 1, -0.090f,  6.336f, -0.673f,   1.382f, 1.039f },  // 3976-gamedata.base
    { "3985-gamedata.base", 1,   -2.70f,    0.20f,   0.00f, 0, 1,  0.520f,  6.508f,  0.429f,  -0.384f, 0.956f },  // 3985-gamedata.base
    { "3987-gamedata.base", 2,    2.00f,   11.90f,   0.00f, 0, 1, -0.551f,  6.355f,  0.428f,   4.773f, 1.441f },  // 3987-gamedata.base
    { "3992-gamedata.base", 1,   -4.20f,    0.00f,   0.00f, 0, 1, -0.008f,  6.362f,  0.607f,   0.397f, 0.938f },  // 3992-gamedata.base
    { "3998-gamedata.base", 2,    1.10f,   -3.30f,   0.00f, 0, 1, -0.234f,  5.978f, -0.404f,  -6.313f, 0.959f },  // 3998-gamedata.base
    { "42068-small_changes_otto.mod", 2,   -4.02f,   -6.50f,   0.00f, 0, 1, -0.148f,  6.354f, -0.681f,  11.576f, 1.088f },  // 42068-small_changes_otto.mod
    { "42070-small_changes_otto.mod", 0,    6.56f,    5.80f,   0.10f, 0, 1,  0.047f,  6.360f,  0.694f,   1.745f, 0.000f },  // 42070-small_changes_otto.mod
    { "42071-small_changes_otto.mod", 1,   -0.20f,    0.00f,   0.00f, 0, 1,  0.656f,  6.355f, -0.229f,   3.553f, 1.482f },  // 42071-small_changes_otto.mod
    { "43870-rebirth.mod", 2,   -4.40f,    0.00f,  -0.90f, 0, 1, -0.211f,  6.356f, -0.663f,   2.059f, 0.987f },  // 43870-rebirth.mod
    { "43947-rebirth.mod", 1,   -4.50f,    0.00f,   0.00f, 0, 1,  0.277f,  6.353f, -0.640f,   1.495f, 0.957f },  // 43947-rebirth.mod
    { "44910-rebirth.mod", 1,   -1.70f,    0.00f,   0.00f, 0, 1,  0.168f,  6.357f, -0.676f,   2.631f, 0.985f },  // 44910-rebirth.mod
    { "48689-rebirth.mod", 1,    1.40f,    2.00f,   0.00f, 0, 1,  0.316f,  6.358f,  0.619f,   0.733f, 0.600f },  // 48689-rebirth.mod
    { "50641-rebirth.mod", 1,   -5.50f,    0.00f,   0.10f, 0, 1, -0.070f,  6.353f, -0.693f,   0.662f, 0.295f },  // 50641-rebirth.mod
    { "56088-rebirth.mod", 0,   -0.40f,    2.80f,   0.00f, 0, 1,  0.598f,  6.363f, -0.354f,   0.623f, 0.790f },  // 56088-rebirth.mod
    { "56089-Newwworld.mod", 2,    0.00f,   -0.70f,   0.00f, 0, 1, -0.688f,  6.361f,  0.102f,   1.157f, 0.988f },  // 56089-Newwworld.mod
    { "56112-Newwworld.mod", 1,   -0.71f,    5.70f,   0.00f, 0, 1, -0.230f,  6.103f, -0.695f,   0.503f, 1.000f },  // 56112-Newwworld.mod
    { "65260-Newwworld.mod", 2,    0.10f,   -0.50f,   0.00f, 0, 1, -0.688f,  6.362f,  0.102f,   0.000f, 1.500f },  // 65260-Newwworld.mod
    { "66294-Newwworld.mod", 0,   -2.50f,   -5.10f,   0.00f, 0, 1,  0.398f,  6.354f, -0.570f,  -1.054f, 1.441f },  // 66294-Newwworld.mod
    { "97570-Newwworld.mod", 1,   -2.40f,    5.10f,   0.00f, 0, 1, -0.426f,  6.361f,  0.551f,   1.484f, 1.196f }  // 97570-Newwworld.mod
};
static const int kDefaultSeatCount = (int)(sizeof(kDefaultSeats) / sizeof(kDefaultSeats[0]));

// Bumped whenever kDefaultSeats changes.  Written to riding.cfg as "defaults=<N>" and used
// for one thing only: a cfg written by an older build recorded every species the player
// ever rode, INCLUDING the ones they never tuned (riding once captures the anchor and
// saves the file, so those lines exist with 0.00 offsets).  Letting such a line win would
// mean an updated mod still rides badly for them, so while the file is older than the
// table an all-zero line yields to the built-in default.  Once the file has been rewritten
// with the current version it is authoritative for everything, zeros included.
// v7 (2026-08-28): every row gained column 18, the body size its numbers were confirmed at.
// v8 (2026-08-28): 12 race-keyed fallback rows appended (keys are race stringIDs, not
// species names).  They are byte copies of a confirmed name row and are only consulted
// when the mount's own localized name has no row - see BuildSeatInfo's two-layer lookup.
// v9 (2026-08-28): the 42 name rows are GONE - the table is now one standardized race row
// per race (21 of them).  This version number does a second job here: a cfg written by v8 or
// earlier lists up to 42 name-keyed rows, and a name row WINS over a race row, so those rows
// would mask the entire new table and nothing shipped here would ever be used.  While the
// file is older than v9, LoadConfig therefore drops its name-keyed rows and keeps only the
// race-keyed ones (which are byte-identical to the rows here anyway).  Once the file has been
// rewritten it is authoritative again, name rows included - a player's own tuning survives
// because tuning rewrites its row (a RACE row, see BuildSeatInfo) under the current version.
// v10 (2026-08-28): exactly one number moved - the crab race (56089-Newwworld.mod) up
// 0.00 -> 0.27, tuned in game on 巨型螃蟹 (k=1.846) and then confirmed by the player on BOTH
// crab scale brackets at once, which is what falsified v9's prediction that the four big crabs
// would need name rows of their own.  Everything else is byte-identical to v9.
// v11 (2026-08-28): that same number moved back - crab up 0.27 -> 0.00, so the row is once more
// byte-identical to v9.  Not a retraction of the size law: the player rode the 0.27 default on
// six freshly randomized crabs (k=0.93..2.28) and reported it fine, then Numpad9 reset one of
// them to the 0.00 home, compared the two seats and picked 0.00.  Pure taste, 0.27u apart in
// the reference frame.
static const int kDefaultsVersion = 11;

// The version at which name rows were merged away.  ⚠️ Keep this FIXED at 9 and do NOT write
// kDefaultsVersion in the drop test: the drop is a one-time v8->v9 migration, not "always
// discard name rows from an older file".  Since v9 nothing in the plugin creates a name row
// (tuning writes back through tuneKey, which is the race key whenever the race layer served
// the mount), so any name row in a cfg written at v9 or later was typed there by hand as a
// deliberate per-animal override - and a later kDefaultsVersion bump must not silently eat it.
static const int kNameRowMergeVersion = 9;

static const DefaultSeat* FindDefaultSeat(const std::string& species)
{
    for (int i = 0; i < kDefaultSeatCount; ++i)
        if (species == kDefaultSeats[i].species)
            return &kDefaultSeats[i];
    return NULL;
}

// Is this cfg key a race stringID rather than a localized animal name?  Kenshi's IDs look
// like "3998-gamedata.base" / "42070-small_changes_otto.mod": a decimal number, a dash, the
// name of the file that created the record, and that file's extension.  getName() can never
// produce such a string, which is why both kinds of key share one map with no collision risk.
static bool IsRaceKey(const std::string& key)
{
    size_t dash = key.find('-');
    if (dash == std::string::npos || dash == 0) return false;
    for (size_t i = 0; i < dash; ++i)
        if (key[i] < '0' || key[i] > '9') return false;
    size_t n = key.size();
    if (n > 5 && key.compare(n - 5, 5, ".base") == 0) return true;
    if (n > 4 && key.compare(n - 4, 4, ".mod")  == 0) return true;
    return false;
}

// Populate speciesTuning from the built-in table.  Always runs before riding.cfg is read,
// so a missing/partial file leaves every shipped species correctly seated.
static void SeedDefaultSeats()
{
    for (int i = 0; i < kDefaultSeatCount; ++i)
    {
        const DefaultSeat& d = kDefaultSeats[i];
        SpeciesTuning st(d.mode, Ogre::Vector3(d.forward, d.up, 0.0f));
        st.forceSit    = (d.sit != 0);
        st.posture     = d.posture;
        st.lateral     = d.lateral;
        st.anchor      = Ogre::Vector3(d.ax, d.ay, d.az);
        st.base        = d.base;
        st.refScale    = d.ref;
        st.home        = st.offset;    // the shipped seat IS the zero Numpad9 returns to
        st.homeLateral = st.lateral;
        speciesTuning[d.species] = st;
    }
}

static void LoadConfig()
{
    speciesTuning.clear();
    SeedDefaultSeats();          // built-in defaults first; the file overrides them
    FILE* f = fopen(GetConfigPath().c_str(), "r");
    if (!f) return;
    // The "defaults=<N>" marker can sit anywhere in the file (and is absent from files
    // written before the built-in table existed), so collect the lines and find it first.
    std::vector<std::string> lines;
    char line[512];
    while (fgets(line, sizeof(line), f))
        lines.push_back(std::string(line));
    fclose(f);
    int cfgVersion = 0;
    size_t li;
    for (li = 0; li < lines.size(); ++li)
    {
        std::string s = lines[li];
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string key = s.substr(0, eq);
        TrimStr(key);
        if (key == "defaults")
        {
            std::string v = s.substr(eq + 1);
            TrimStr(v);
            int n = 0;
            if (sscanf(v.c_str(), "%d", &n) == 1) cfgVersion = n;
            break;
        }
    }
    int droppedNameRows = 0;
    for (li = 0; li < lines.size(); ++li)
    {
        std::string s = lines[li];
        size_t eq = s.find('=');
        if (eq == std::string::npos) continue;
        std::string name = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        TrimStr(name);
        TrimStr(val);
        if (name.empty() || name == "defaults") continue;
        // v9 MIGRATION: the built-in table stopped shipping name-keyed rows and now carries
        // one standardized row per race.  A name row still WINS over a race row (that is how
        // a player overrides one animal), so a cfg written by v8 or earlier - which lists a
        // name row for every species its author ever rode - would mask the entire new table
        // and the merge would have no effect at all.  While the file is older, keep only its
        // race-keyed rows; those are byte copies of the same seats, and anything the player
        // retunes from here on is written back under the current version and then persists.
        // ⚠️ The test is against kNameRowMergeVersion (fixed at 9), NOT kDefaultsVersion - see
        // that constant: a name row in a v9+ file is a hand-typed override and must survive.
        if (cfgVersion < kNameRowMergeVersion && !IsRaceKey(name))
        {
            ++droppedNameRows;
            continue;
        }
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
            // mode 4 was the rigid-body seat, removed 2026-08-27.  Its numbers were offsets
            // from the mount's rigid body rather than from a bone, so a stale row like that
            // must not be reinterpreted as a rear/neck anchor - drop it to the neutral mode
            // and let the built-in default / the tuning keys take it from there.
            if (mode > SEAT_REAR) mode = SEAT_MIDPOINT;
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
            // columns 15-17: the player's declared "zero" (home up / forward / lateral).
            // MIGRATION (2026-08-26): a cfg written before this feature has no home
            // columns - seed home from the tuned offset that is already in the file, so
            // the seat the player dialled in becomes the reset target instead of the bare
            // geometric base.  Nothing to do by hand.
            float hup = 0.0f, hfwd = 0.0f, hlat = 0.0f;
            if (sscanf(val.c_str(), "%*d,%*f,%*f,%*d,%*d,%*f,%*f,%*f,%*d,%*f,%*f,%*f,%*f,%*f,%f,%f,%f", &hup, &hfwd, &hlat) == 3)
            {
                st.home        = Ogre::Vector3(hfwd, hup, 0.0f);
                st.homeLateral = hlat;
            }
            else
            {
                st.home        = st.offset;
                st.homeLateral = st.lateral;
            }
            // column 18: the body size this row's numbers were confirmed at.
            //
            // MIGRATION.  A cfg written before this column existed cannot say what size its
            // numbers belong to, and guessing would silently MOVE a seat the player already
            // dialled in - so the default is 0 = "unknown" = no adaptation, i.e. exactly
            // the old behaviour.  The one case where the answer is actually known is a row
            // the player never retuned: if its offsets still match the built-in seat byte
            // for byte, it IS the built-in seat and shares its reference size.  That covers
            // most of an upgrading player's file (riding.cfg records every species they ever
            // rode, tuned or not), and everything else self-heals on the next tune, which
            // adopts the live size.
            float rsc = 0.0f;
            if (sscanf(val.c_str(), "%*d,%*f,%*f,%*d,%*d,%*f,%*f,%*f,%*d,%*f,%*f,%*f,%*f,%*f,%*f,%*f,%*f,%f", &rsc) == 1)
                st.refScale = rsc;
            else
            {
                const DefaultSeat* rd = FindDefaultSeat(name);
                if (rd && fabsf(rd->up - up) < 0.005f && fabsf(rd->forward - fwd) < 0.005f
                       && fabsf(rd->lateral - lateral) < 0.005f)
                    st.refScale = rd->ref;
            }
            // A pre-defaults cfg lists every species the player ever rode, tuned or not
            // (the first ride captures the anchor and saves the file).  An untouched line
            // like that must not bury the seat this build ships - see kDefaultsVersion.
            const DefaultSeat* def = FindDefaultSeat(name);
            if (cfgVersion < kDefaultsVersion && def
                && up == 0.0f && fwd == 0.0f && lateral == 0.0f)
            {
                SpeciesTuning& cur = speciesTuning[name];   // the seeded default
                if (st.anchor.y != 0.0f) cur.anchor = st.anchor;  // rider-side, size-invariant
                // The bob baseline is NOT size-invariant, so it can only be adopted if we
                // know which individual it was captured on.  With column 18 present that is
                // answerable - convert it into the built-in row's frame.  Without it, drop it:
                // the built-in row already ships a baseline in the reference frame, and
                // letting an unknown-sized animal's reading through is exactly the hybrid-
                // constant trap the v5/v6 clone rows hit (a never-tuned line donated a
                // baseline 0.5-0.9u off and quietly undid the clone it was meant to inherit).
                if (st.base != 0.0f)
                {
                    if (st.refScale >= kSizeReadMin && cur.refScale >= kSizeReadMin)
                        cur.base = st.base * (cur.refScale / st.refScale);
                    else if (cur.refScale < kSizeReadMin)
                        cur.base = st.base;      // no reference on either side: as before
                }
                continue;
            }
            speciesTuning[name] = st;
        }
        else if (sscanf(val.c_str(), "%f,%f", &up, &fwd) == 2)
        {
            if (cfgVersion < kDefaultsVersion && FindDefaultSeat(name)
                && up == 0.0f && fwd == 0.0f)
                continue;                 // same rule for the 2-column legacy form
            SpeciesTuning st(SEAT_MIDPOINT, Ogre::Vector3(fwd, up, 0.0f));
            st.home = st.offset;          // same migration for the 2-column legacy form
            speciesTuning[name] = st;
        }
    }
    if (droppedNameRows)
        DebugLog("Riding: cfg v" + IntToStr(cfgVersion) + " < " + IntToStr(kNameRowMergeVersion)
                 + " - dropped " + IntToStr(droppedNameRows)
                 + " name-keyed row(s) so the built-in race seats apply");
}

static void SaveConfig()
{
    FILE* f = fopen(GetConfigPath().c_str(), "w");
    if (!f) return;
    fprintf(f, "# riding.cfg - per-species seat tuning\n");
    fprintf(f, "# <species>=<mode>,<up>,<forward>,<mount>,<sit>,<roll>,<pitch>,<yaw>,<posture>,<lateral>  mode 0=exact 1=midpoint 2=neck 3=rear  sit 0=off 1=on  posture 0=sit 1=stand  lateral=side offset\n");
    fprintf(f, "# columns 4 and 6-8 are OBSOLETE legacy fields (mount method / roll-pitch-yaw) - parsed-and-ignored, always written as 0\n");
    fprintf(f, "# columns 11-14 = persisted seat constants (anchor x/y/z + bob baseline), auto-captured - do not hand-edit\n");
    fprintf(f, "# columns 15-17 = the declared zero/home (up, forward, lateral): Numpad9 returns here, Ctrl+Numpad9 sets it to the current seat\n");
    fprintf(f, "# column 18 = the animal size columns 2/3/10/14 were tuned at; the seat is rescaled for bigger/smaller individuals of the same species (0 = unknown, no rescaling)\n");
    fprintf(f, "# rows whose name has the shape <number>-<datafile>.base/.mod are keyed by RACE, not species: they seat any animal of that race that has no row of its own (and are the only rows that work on a non-Chinese install)\n");
    fprintf(f, "# this file only OVERRIDES the seats built into RidingPlugin.dll - delete it to go back to the shipped defaults\n");
    fprintf(f, "defaults=%d\n", kDefaultsVersion);
    boost::unordered_map<std::string, SpeciesTuning>::iterator it = speciesTuning.begin();
    for (; it != speciesTuning.end(); ++it)
        fprintf(f, "%s=%d,%.2f,%.2f,%d,%d,0.0,0.0,0.0,%d,%.2f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.3f\n", it->first.c_str(), it->second.seatMode,
                it->second.offset.y, it->second.offset.x, 0,
                it->second.forceSit ? 1 : 0, it->second.posture, it->second.lateral,
                it->second.anchor.x, it->second.anchor.y, it->second.anchor.z, it->second.base,
                it->second.home.y, it->second.home.x, it->second.homeLateral,
                it->second.refScale);
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
//   - REAR:     anchor = pelvis (root bone fallback), no base lift
// The live tuning delta (userOffset) is applied on top of whatever base is chosen.
SeatInfo BuildSeatInfo(Character* mount)
{
    SeatInfo info;
    info.species = GetSpecies(mount);
    info.raceKey = GetRaceKey(mount);
    info.tuneKey = info.species;
    info.backBone = PickBackBone(mount);
    info.frontBone = info.backBone;
    info.lift = Ogre::Vector3::ZERO;
    info.userOffset = Ogre::Vector3::ZERO;
    info.homeOffset = Ogre::Vector3::ZERO;
    info.homeLateral = 0.0f;
    info.seatMode = SEAT_MIDPOINT;
    info.forceWalk = false;
    info.forceSit = true;
    info.posture = POSTURE_SIT;
    info.lateral = 0.0f;
    info.torsoLen = 0.0f;
    info.rootAnchor = false;
    info.neckFollow = false;
    info.flexTrack  = false;
    info.sizeScale = 1.0f;
    info.refScale  = 0.0f;
    info.liveScale = ReadMountSize(mount);

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

    // Which row serves this mount?  LOCALIZED NAME first, RACE stringID second (v8,
    // 2026-08-28).  Since v9 the built-in table ships ONLY race rows - one standardized seat
    // per race - so on a fresh install every mount is served by the second layer:
    //   * a race row covers every member we have never seen (宠物犬 needed a hand-added clone
    //     row in v6 for exactly this reason) and it is the ONLY layer that works on a
    //     non-Chinese install, where getName() returns "Garru" and no name row can match.
    //   * a name row is a hand-written OVERRIDE for one animal.  ⚠️ Nothing in the plugin ever
    //     CREATES one: the tuning keys write back through info.tuneKey, which IS the race key
    //     whenever the race layer won, so tuning any member re-tunes its whole race.  The only
    //     way to get a name row is to type it into riding.cfg.  That turned out to be enough -
    //     the four big crabs (56089's 1.5-2.25 bracket) were supposed to be the case that
    //     needed one, since their old hand tune was on the EXACT anchor while the shipped race
    //     row is the small crabs' NECK anchor and the size law only converts within one anchor
    //     frame; in game (2026-08-28) a single +0.27 nudge on the shared row seated the whole
    //     0.94-2.24 span correctly, big crabs and small ones alike.
    // Whichever layer wins is recorded in info.tuneKey, and ⚠️ every write-back path must use
    // tuneKey rather than species - otherwise the first anchor capture on a race-served mount
    // silently forks a name row for it and the race layer stops covering it.
    info.tuneKey = info.species;
    boost::unordered_map<std::string, SpeciesTuning>::iterator tit = speciesTuning.find(info.species);
    if (tit == speciesTuning.end() && !info.raceKey.empty())
    {
        tit = speciesTuning.find(info.raceKey);
        if (tit != speciesTuning.end())
            info.tuneKey = info.raceKey;
    }
    if (tit != speciesTuning.end())
    {
        info.seatMode = tit->second.seatMode;
        info.userOffset = tit->second.offset;
        info.forceSit = tit->second.forceSit;
        info.posture = tit->second.posture;
        info.lateral = tit->second.lateral;
        info.homeOffset = tit->second.home;
        info.homeLateral = tit->second.homeLateral;
        info.refScale = tit->second.refScale;
        info.sizeScale = SeatSizeRatio(info.refScale, info.liveScale);
    }

    // pack_beast family (Garru / Pack Beast / Dead Pack Beast) share the "beast walk"
    // animation, which the carry system suppresses.  Detect it from the animation
    // data so we can force the walk back on while ridden.
    if (mountAnim && mountAnim->getAnimationData("beast walk"))
        info.forceWalk = true;

    // Fling skeletons: their back bone is rigid / the root bone is thrown up and down
    // heavily during running, which flings a rider anchored to the back bone.  Anchor
    // these to the root bone instead (follows the whole animal, no bone swing).
    //
    // !!! INERT SINCE IT WAS WRITTEN - READ BEFORE TOUCHING (verified 2026-08-26) !!!
    // These are plain Chinese literals.  This file starts with a UTF-8 BOM, so MSVC v100
    // decodes them and RE-ENCODES the narrow literals into the system ANSI codepage
    // (936/GBK) - confirmed in the built DLL: "骨犬" is present only as GBK B9 C7 C8 AE,
    // while the \x-escaped 卷缩者 below is present as UTF-8.  GetSpecies() returns the
    // game's UTF-8 name, so NONE of these 19 names ever compares equal: rootAnchor has
    // never once been enabled in game, and flexTrack (gated on rootAnchor AND on its own
    // GBK names below) is dead twice over.  Every seat in riding.cfg was tuned against the
    // neck/midpoint anchor that these species silently fall through to.
    // neckFollow is NOT part of the dead set (2026-08-28): its comparison below IS
    // \x-escaped, so it fires for 卷缩者 - and although ComputeSeatPosition only reads it
    // inside the (unreachable) rootAnchor branch, DampSeatBob reads it too, outside that
    // branch, where it skips the 15% bob damping outright.  That is live behaviour: the
    // Crimper's mode-2 neck seat follows the neck bone's Y at full strength, which is what
    // the player asked for.  Escaping the fling names would move all of those seats at once
    // (the crab's neck anchor is ~7.85 units from its root), so it is left switched off
    // deliberately - and measurement says root-anchoring is not even the fix for the
    // reported sway: over a 4708-frame crab ride the ROOT bone's sideways swing (3.45
    // units) is WORSE than the neck anchor's (2.41).  Convert a name to \x escapes only
    // when that species is going to be re-tuned in the same session.
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

    // No lateral gait damping, for any species.  It was tried for the crab family (whose walk
    // cycle rolls the whole skeleton sideways) and removed again on 2026-08-27: the sideways
    // swing is the SHELL moving, so a rider tracking it in phase is a rider staying put on the
    // shell.  Damping it pinned the rider near the rigid-body centreline and the shell rolled
    // out from under them - which is what the "人物在螃蟹背上来回横移" report was actually
    // describing.  Full follow is the correct look; see RidingPlugin_RE_NOTES.md for the
    // measurements.  The standing-still sway is a DIFFERENT bug with its own fix that stays:
    // GetMountForward's degenerate-baseline guard.

    // Crimper race ONLY: the player wants the rider's butt glued to the NECK bone so
    // it rides up and down together with the neck - the flattened root-anchored height
    // looks right while running but floats when the mount stands still.  Keep the stable
    // root horizontal (rootAnchor stays on), but follow the neck bone's Y with no bob
    // damping.  Deliberately scoped to this one race.
    // Matched on the RACE as well as the name since v9: 卷缩者 and 国王 are the same race at
    // the same fixed scale and now share one seat row, so leaving this name-only would make
    // the merged row behave two different ways depending on which name the animal wears
    // (国王 would keep the 15% bob damping the row's baseline of 0.000 does not belong to).
    if ((info.species == "\xE5\x8D\xB7\xE7\xBC\xA9\xE8\x80\x85"    // 卷缩者 (UTF-8)
         || info.raceKey == "65260-Newwworld.mod")
        && mountAnim && mountAnim->getHasBone("Bip01 Neck"))
        info.neckFollow = true;

    // Dog family ONLY (2026-08-24): their run cycle flexes the body - the WAIST see-saws
    // against the butt/root - so a bind-pose constant over the root glues the rider to the
    // butt end while the waist sweeps under them.  Track the LIVE front/rear mean instead
    // (see ComputeSeatPosition rootAnchor branch).  Scoped like neckFollow so already-tuned
    // rigid fling skeletons keep their exact current seat.
    if (info.rootAnchor
        && (info.species == "骨犬" || info.species == "埋骨地狼" || info.species == "定居者的小狗"))
        info.flexTrack = true;

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
    else if (info.seatMode == SEAT_REAR)
    {
        // anchor IS the hindquarter bone: no geometric guess, the player tunes up from it
        info.lift = Ogre::Vector3::ZERO;
    }
    // SEAT_EXACT keeps the per-bone lift set above

    // ⚠️ Do NOT reset info.sizeScale here.  A line doing exactly that survived the
    // 2026-08-23 removal of the old bone-read-based size scaling and sat at the end of this
    // function until 2026-08-28, silently wiping the value computed from refScale/liveScale
    // ~100 lines above - every mount logged k=1000 and the v7 adaptation never ran once
    // (found by a diagnostic session where 骨犬 printed ref=802 live=350 k=1000).  The only
    // place sizeScale is decided is the speciesTuning branch above; anything that wants
    // "no adaptation" must get it from SeatSizeRatio returning exactly 1.0.

    return info;
}

// How close a rider has to be to THIS animal before the two scale-free arrival signals
// (the movement system's own "destination reached", and "stopped making progress") are
// allowed to board it.  Both signals answer "can I get any closer to this body?", which
// says nothing about scale - so on their own they will happily board a rider that is
// nowhere near the animal the moment the engine reports a path as finished for some other
// reason.  Sizing the bound from the animal's own body length is what lets one rule cover
// a goat (boards within ~10 units) and a Leviathan (its hull parks the rider ~60 units
// from the body centre, which no fixed radius can accept without also re-opening the
// teleport hole).  Floored at kMountArriveDist and capped at kMountEnvelopeMax so the
// known "bone reads come back in unscaled space" window cannot hand out a 10x licence.
static float MountBoardEnvelope(Character* mount)
{
    float len = 0.0f;
    float rad = 0.0f;
    if (mount)
    {
        len = BuildSeatInfo(mount).torsoLen;   // pure measurement, no side effects
        rad = mount->getRadius();              // non-virtual, exported stub
    }
    // distance is measured centre-to-centre, so a body reaches out roughly half its own
    // length in any direction; add the rider's own standing-next-to-it distance on top.
    float env = len * 0.75f + kMountArriveDist;
    // 2026-08-28: torsoLen is a PROXY and it is the wrong one - the rad= field logged over
    // 8 species (radius 6.6 .. 34.9) shows the rider always parks at radius + 3.0..3.5,
    // with torsoLen uncorrelated and sometimes inverted (a crab measures torsoLen 3.8 but
    // parks the rider 25u out; a cow measures 10.4 and parks it at 10).  The Swamp Turtle
    // is the case the torso term cannot reach: radius 34.9, parked at d=38, envelope 30 -
    // it stood there for 461 frames with both scale-free signals gated off and only
    // boarded when `pressing` finally opened the stall path.  So take the larger of the
    // two terms.  kMountRadiusPad is the measured standoff (3.5) plus margin.
    float radEnv = rad + kMountRadiusPad;
    if (radEnv > env) env = radEnv;
    if (env < kMountEnvelopeMin)  env = kMountEnvelopeMin;
    if (env > kMountEnvelopeMax)  env = kMountEnvelopeMax;
    return env;
}

// Travel heading of the mount (normalised, ground plane) if one has been recorded this
// ride.  ApplyRiderOrientation maintains it from the CharMovement position delta and
// HOLDS it while the mount stands still, so it is the one horizontal frame on the mount
// that carries no gait wobble at all - used as the fallback facing source when the
// front/rear bone baseline degenerates.  false = this mount has not travelled yet.
static bool GetMountTravelHeading(Character* mount, Ogre::Vector3& out)
{
    boost::unordered_map<Character*, Ogre::Vector3>::iterator hd = mountHeadingDir.find(mount);
    if (hd == mountHeadingDir.end()) return false;
    Ogre::Vector3 d = hd->second;
    d.y = 0.0f;
    if (d.length() < 0.001f) return false;
    d.normalise();
    out = d;
    return true;
}

// The mount's own facing vector, straight out of its movement component
// (AbstractMovementBase::getFacingDirection, non-virtual, CharMovement.h:122).  This is what
// the ENGINE thinks the animal is pointing at, so unlike the bone axis it cannot degenerate,
// and unlike the travel heading it exists before the animal has ever taken a step.  Used as
// the facing source when the bone baseline is unusable AND no heading has been recorded -
// i.e. exactly the "mounted a crab that is standing still" case, which was rotating the tuned
// forward/lateral offsets around a circle every frame (2026-08-28 log: 167 frames of
// mv=0 hdSrc=0 fwSrc=3 on the Barnabus ride, shaking, then rock solid from the first step).
// It is a real world-space direction, so no axis convention has to be guessed.
static bool GetMountFacingDirection(Character* mount, Ogre::Vector3& out)
{
    CharMovement* mv = mount ? mount->getMovement() : 0;
    if (!mv) return false;
    Ogre::Vector3 d;
    try
    {
        d = mv->getFacingDirection();
    }
    catch (...) { return false; }
    d.y = 0.0f;
    if (d.length() < 0.001f) return false;
    d.normalise();
    out = d;
    return true;
}

// Smallest front<->rear HORIZONTAL separation that can be trusted as a forward axis.
// Measured on the crab 2026-08-26: "Bip01 Spine" sits almost straight above
// "Bip01 Pelvis" (horizontal 0.02..0.16, vertical 1.83, torsoLen 1.7), so normalising
// that residue yielded a forward axis that jumped by up to ~190 degrees between frames.
// ComputeSeatPosition ROTATES the tuned forward/lateral offsets by this axis, so the
// crab's -0.70 forward tune was being thrown around a 1.4-unit circle every frame -
// with the mount standing perfectly still ("静止时也会有轻微的左右摇摆").
static const float kMinForwardBaseline = 0.5f;

// Facing direction of the mount (headward), projected on the ground plane.
// srcOut (optional, for the DBG line): 0 = default axis, 1 = bones, 2 = travel heading
// after rejecting a degenerate bone baseline, 4 = the mount's own facing vector (bones
// degenerate and never travelled), 3 = nothing left but the noisy bone residue.
// The codes are NOT in ladder order on purpose: 3 kept its number so the field stays
// comparable with the logs the crab shake was diagnosed from.
Ogre::Vector3 GetMountForward(const SeatInfo& seat, Character* mount, int* srcOut = 0)
{
    Ogre::Vector3 fwd(0.0f, 0.0f, 1.0f);
    if (srcOut) *srcOut = 0;
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
        // Vertically stacked torso bones leave a horizontal residue whose DIRECTION is
        // pure noise (see kMinForwardBaseline).  Relative test as well as absolute so a
        // big animal with a short horizontal torso is caught too.
        bool degenerate = (len < kMinForwardBaseline)
                       || (seat.torsoLen > 0.001f && len < 0.35f * seat.torsoLen);
        if (!degenerate && len > 0.001f)
        {
            fwd = d / len;
            if (srcOut) *srcOut = 1;
        }
        else if (GetMountTravelHeading(mount, fwd))
        {
            if (srcOut) *srcOut = 2;   // gait-free substitute for the unusable bone axis
        }
        else if (GetMountFacingDirection(mount, fwd))
        {
            if (srcOut) *srcOut = 4;   // standing still and never walked: ask the engine
        }
        else if (len > 0.001f)
        {
            fwd = d / len;             // nothing left: noisy, but still on-model
            if (srcOut) *srcOut = 3;
        }
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
                float bindOff   = backInitY - rootInitY;
                anchor.y = rootPos.y + bindOff;

                // flexTrack (dog family, plan A): track the LIVE midpoint of the front/rear
                // spine bones as a DEVIATION from their own bind-pose midpoint, applied on
                // top of the classic root+bindSpine2 anchor.  At bind pose delta == 0, so
                // the tuned seat position is preserved EXACTLY; while running, the
                // waist-vs-butt see-saw becomes a smooth 3-axis delta the rider follows -
                // anti-phase swing cancels in the mean, fore-aft spinal flex is tracked too
                // (the old Y-only version missed that horizontal half).  Clamped so a ghost
                // read can't yank the seat; falls back to the pure constant.
                if (seat.flexTrack)
                {
                    Ogre::Vector3 sum(0, 0, 0), initSum(0, 0, 0);
                    int cnt = 0;
                    if (mountAnim->getHasBone(seat.frontBone))
                    {
                        sum    += mount->getBoneWorldPosition(seat.frontBone);
                        initSum += mountAnim->getBoneInitialWorldPosition(seat.frontBone);
                        cnt++;
                    }
                    if (!seat.rearBone.empty() && seat.rearBone != seat.frontBone
                        && mountAnim->getHasBone(seat.rearBone))
                    {
                        sum    += mount->getBoneWorldPosition(seat.rearBone);
                        initSum += mountAnim->getBoneInitialWorldPosition(seat.rearBone);
                        cnt++;
                    }
                    if (cnt > 0)
                    {
                        float inv = 1.0f / cnt;
                        Ogre::Vector3 liveRel = (sum * inv) - rootPos;
                        Ogre::Vector3 bindRel = (initSum * inv)
                                              - mountAnim->getBoneInitialWorldPosition("Bip01");
                        Ogre::Vector3 delta   = liveRel - bindRel;
                        // Soft saturation, not a binary fallback: a deep idle-stretch pose
                        // crossing the limit used to snap the seat by the whole excess
                        // ("idle 突变").  Clamping the MAGNITUDE keeps the direction and
                        // stays continuous for any pose.
                        static const float kFlexMaxDeviation = 6.0f;
                        float dl = delta.length();
                        if (dl > kFlexMaxDeviation)
                            delta *= (kFlexMaxDeviation / dl);
                        anchor += delta;
                    }
                }
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
    else if (seat.seatMode == SEAT_REAR)
    {
        // Hindquarters anchor (2026-08-26).  For a huge animal the neck is both the
        // jitteriest bone in the skeleton AND 40+ units from where the rider ends up, so
        // mode 2 pays for clearing the body with a long lever arm that converts every
        // degree of skeletal wobble into sideways slide.  The pelvis carries the seat
        // directly: on the logged Leviathan ride it sits 4 units behind the root bone
        // horizontally but 39 units higher, i.e. it is the highest rear-facing bone
        // available, and its per-frame sideways jerk (0.137) is a quarter of the live
        // mode-2 seat's (0.571).  Root bone as the fallback (smoother still, 0.097, but
        // lower and absent as a distinct bone on some rigs).
        AnimationClass* mountAnim = mount->getAnimationClass();
        if (!seat.rearBone.empty() && seat.rearBone != seat.frontBone)
        {
            Ogre::Vector3 rear = mount->getBoneWorldPosition(seat.rearBone);
            // same ghost-read cross-check the midpoint anchor uses: an unscaled-space
            // pelvis read would throw the seat right off the animal
            if (RearBoneReadSane(mount, seat, rear))
                anchor = rear;
            else if (mountAnim && mountAnim->getHasBone("Bip01"))
                anchor = mount->getBoneWorldPosition("Bip01");
        }
        else if (mountAnim && mountAnim->getHasBone("Bip01"))
        {
            anchor = mount->getBoneWorldPosition("Bip01");
        }
        // neither available: keep the back-bone anchor
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
    // user-tuned offsets, adapted to this individual's body size (SeatUp is affine, not a
    // plain multiply - see kSeatUpConstB).  The bone anchor and seat.lift above already
    // scale with the mount's own scene node, so only the hand-tuned deltas need this.
    if (seat.userOffset != Ogre::Vector3::ZERO)
    {
        Ogre::Vector3 fwd = GetMountForward(seat, mount);
        pos += fwd * SeatForward(seat)
             + Ogre::Vector3(0.0f, SeatUp(seat), 0.0f);
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
        pos += side * SeatLateral(seat);
    }
    return pos;
}

// How far outside the default range this animal's own size pushes things.  1.0 = a normal
// animal that fits inside +-kTuningClamp and must behave exactly as it always has.
static float SeatTuneScale(const SeatInfo& seat)
{
    return (seat.torsoLen > kTuningClamp) ? (seat.torsoLen / kTuningClamp) : 1.0f;
}

// Tuning range for this seat.  A fixed +-15 cannot describe a huge animal: the Leviathan's
// torso alone is 85 units and its dialled-in seat is 66 up / -44.3 forward.  Those values
// survived only because the cfg loader does not clamp - the FIRST +/- or */ keypress ran
// them through the old fixed clamp and snapped the seat to 15/-15, teleporting the rider
// ~50 units (which is also why that species' auto-captured anchor columns stayed 0.000:
// every attempt to tune it destroyed the tune).  Species that already fit inside the
// default are completely unaffected.
static float SeatTuneLimit(const SeatInfo& seat)
{
    float scale = SeatTuneScale(seat);
    if (scale <= 1.0f)
        return kTuningClamp;                                 // normal animal: unchanged
    return kTuningClamp * scale * kTuneHeadroom;             // giant: 170.6 for the Leviathan
}

// Per-keypress step, scaled so a full sweep of the animal's own length always costs the same
// number of presses.  0.1 units on an 85-unit animal would need ~1700 presses to cross its
// back.  Deliberately keyed to SeatTuneScale, not to SeatTuneLimit: the headroom above the
// animal's length widens the ceiling without coarsening the feel of every keypress.
static float SeatTuneStep(const SeatInfo& seat)
{
    return kTuneStep * SeatTuneScale(seat);
}

// The same limit and step expressed in the REFERENCE frame that the stored numbers live in.
// Both are derived from the LIVE torsoLen, and torsoLen is itself already a scaled quantity
// (torsoLen / nsc is constant inside a race), so on a half-size individual they come back
// half-size as well.  Applying them straight to a stored reference-frame value would clamp
// a perfectly good tune down to the small individual's ceiling - the same silent
// destruction HISTORY §F records for the Leviathan, one size bracket away from happening
// again.  Dividing by k converts both into the stored frame, which also makes a keypress
// move the rider by the on-screen step no matter which individual is being ridden (the
// on-screen response to a stored delta is exactly k, for up as well as for forward and
// lateral).  sizeScale is either exactly 1.0 or inside [kSizeRatioMin, kSizeRatioMax], so
// there is nothing to guard against here.
static float SeatTuneLimitRef(const SeatInfo& seat)
{
    return SeatTuneLimit(seat) / seat.sizeScale;
}

static float SeatTuneStepRef(const SeatInfo& seat)
{
    return SeatTuneStep(seat) / seat.sizeScale;
}

static void ClampTuning(Ogre::Vector3& v, float limit)
{
    if (v.x < -limit) v.x = -limit;
    if (v.x >  limit) v.x =  limit;
    if (v.y < -limit) v.y = -limit;
    if (v.y >  limit) v.y =  limit;
}

// "Huge animal" test for the combat paths: the rider is a passenger on something far too
// big to fight from, so attack orders are redirected to the mount and the rider never
// swings.  NECK and REAR are both huge-animal anchors (REAR exists precisely because the
// giants were stuck on NECK), so switching a giant between them must not change who fights.
static bool IsBigMount(const SeatInfo& seat)
{
    return (seat.seatMode == SEAT_NECK || seat.seatMode == SEAT_REAR);
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
// recomputed every frame so a cfg posture change self-corrects in one frame.
static void SyncRiderNode(Character* rider, Character* mount, AnimationClass* rAnim,
                          const Ogre::Vector3& seatPos, bool mainPhase)
{
    if (!rider || !mount || !rAnim || !rAnim->node) return;

    // EXP5 2026-08-25: kill the carry ragdoll on EVERY sync.  Probe timeline proved
    // the mystery ~10u node writer IS the AnimationClass ragdoll drag: one instant
    // _NV_ragdollModeUT(false,CARRY_MODE) flipped aRag 1->0 and wn read (0,0,0) for
    // exactly that frame - then an engine restorer re-set the ragdoll and the drag
    // resumed.  All three sync paths (mainLoop + mount-gated + rider-gated resync)
    // funnel through here, so the last caller before render re-kills it each frame.
    // Instant AnimationClass-level call = no postRagdollCallback recovery, no
    // message queue (the queued Character version never processes mid-ride - probe
    // rev2, three presses, chRag stuck at 1).  Human-only: uses the _NV_ export stub
    // on AnimationClassHuman (vtable-order-independent).
    if (rAnim->isRagdoll() && rider && !rider->isAnimal())
        static_cast<AnimationClassHuman*>(rAnim)->_NV_ragdollModeUT(
            false, Ogre::Vector3::ZERO, RagdollPart::CARRY_MODE, std::string(), rider);

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
    // and staleness (a cfg posture change changes the pose constant too).
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
            if (ssi != mountSeat.end() && !ssi->second.tuneKey.empty())
            {
                speciesTuning[ssi->second.tuneKey].anchor = rel;
                SaveConfig();
                try { DebugLog("Riding: anchor captured+saved [" + ssi->second.tuneKey + "]"); } catch(...) {}
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
// assumes lift is constant, which it is: each species' seat mode is fixed by the
// built-in defaults / riding.cfg and never changes at runtime.
static const float kSeatBobScale = 0.15f;
static void DampSeatBob(Character* mount, const SeatInfo& seat, Ogre::Vector3& seatPos)
{
    if (!mount) return;
    // 卷缩者: intentionally follow the neck bob at full strength (no damping) so the
    // rider's butt stays glued to the neck as it rises and falls.  ComputeSeatPosition
    // already set seatPos.y to the neck bone Y; leave it untouched.
    if (seat.neckFollow) return;
    // flexTrack (dog family): same philosophy, different geometry.  The live midpoint
    // anchor already cancelled the wild see-saw; what survives is true body motion that
    // must NOT be flattened - flattening is exactly what re-created the float/sink
    // complaints.  Full geometric follow also sidesteps the whole baseline lifecycle
    // (capture/wipe snaps) these fast anchors used to trigger.
    if (seat.flexTrack) return;
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

    // strip the user's height tune so only the bone-anchor bob gets damped.
    // ⚠️ This must be the SAME number ComputeSeatPosition added, i.e. the ADAPTED one -
    // if the two disagreed about what "the height tune" is, the difference would land
    // straight in the rider's altitude.  Hence SeatUp on both sides, never userOffset.y.
    float uy      = SeatUp(seat);
    float baseY   = seatPos.y - uy;
    float rawBase = baseY - refY;   // seat height above the reference (bob-free mean, no user tune)

    // Stationarity watch (2026-08-24).  The old assumption that running keeps baseStable
    // from accumulating is FALSE for slow shallow bobs (Beak Thing: +-0.33u, per-frame
    // delta under the 0.35 tolerance) - the counter filled WHILE RUNNING, the stale-wipe
    // + recapture fired mid-gallop, and each gait change flipped the baseline ~2.2u in
    // one frame = the reported up/down jitter.  Baseline mutation (stale wipe OR fresh
    // capture) now requires the mount to have held still across a whole observation
    // window; while moving, an existing baseline is always trusted and damping runs on.
    static const float kStillWindowSyncs = 20;   // syncs per observation window (~1/3 s)
    static const float kStillMaxDrift    = 0.35f;
    // No baseline mutation (capture OR obsolete-seed wipe) before this many syncs of a
    // ride: the first seconds after mount/load are pose-storm (pickup carry-blend,
    // skeleton settling) and the field log caught a capture firing 0.11s in, baking
    // garbage into cfg.  Counted against ~2 syncs per rendered frame now that both the
    // animUpdate resync and the main-loop pass call DampSeatBob - 240 syncs ~= 2s real
    // time.  Damping with the SEEDED baseline runs from frame one regardless - only
    // mutation is delayed.
    static const int   kMinSyncsBeforeCapture = 240;
    // Pause guard (2026-08-25, rev2).  A pause freezes BOTH the animation (seat-anchor
    // bone stuck at an arbitrary gait phase) and movement, so every stillness counter
    // would fill with "still" evidence drawn from a frozen mid-gait rawBase.  rev1 only
    // gated settledStill with !paused, which merely DELAYED the mutation one frame: the
    // counters kept accumulating through the pause, so the instant the player unpaused
    // all gates were already satisfied and the stale-wipe + recapture fired on the first
    // unpaused frame - the jump simply moved from pause-START to pause-END (field log:
    // 9 captures, each an unpause event).  Correct fix: while paused, do NOT advance the
    // capture state machine at all (syncCount, the stillness window, baseStable all stay
    // put); just re-apply damping with the EXISTING baseline and return, so the paused
    // frame reproduces the last unpaused seat height and capture only resumes after a
    // real post-unpause running/standing window.  ou is the global GameWorld*.
    if (ou && ou->isPaused())
    {
        boost::unordered_map<Character*, float>::iterator vp = mountBaseVOffset.find(mount);
        if (vp != mountBaseVOffset.end())
        {
            float stableY    = refY + vp->second;
            float dampedBase = stableY + (baseY - stableY) * kSeatBobScale;
            seatPos.y = dampedBase + uy;
        }
        return;   // no baseline yet -> leave seatPos (full follow); either way freeze counters
    }
    CharMovement* mmv = mount->getMovement();
    Ogre::Vector3 mpos = mmv ? mmv->getPosition() : Ogre::Vector3::ZERO;
    CapTrack& ct = mountCap[mount];
    ct.syncCount++;
    if (--ct.winCountdown <= 0)
    {
        // First boundary only SAMPLES the anchor; stillness is judged from the SECOND
        // boundary on.  (The old first-window shortcut granted "still" with zero
        // evidence, which let a baseline capture fire ~0.1s after mounting - mid
        // pose-storm - and bake a garbage constant into cfg.)
        if (ct.winValid)
        {
            float drift = (mpos - ct.winAnchor).length();
            ct.stillWindows = drift < kStillMaxDrift ? ct.stillWindows + 1 : 0;
        }
        ct.winAnchor    = mpos;
        ct.winValid     = true;
        ct.winCountdown = (int)kStillWindowSyncs;
    }
    bool settledStill = (ct.stillWindows >= 1) && ct.syncCount >= kMinSyncsBeforeCapture;

    // Same settle-gate as the SyncRiderNode anchor: right after mount/load rawBase is
    // transient (pose blending, ghost bones); freezing THEN baked a wrong baseline
    // into every later frame (persistent "rider sits too low" after loading a save).
    // Capture only once rawBase has held steady AND the mount has stood still through
    // an observation window; until then an existing baseline is always kept and used.
    // The poison check (>25u = ghost-bone garbage) stays ungated - it must fire even
    // mid-run or a poisoned constant would ride forever.
    static const float kBaseStableTol     = 0.35f;
    static const float kBaselineStaleDiff = 3.0f;
    ct.baseStable = (fabsf(rawBase - ct.prevBase) < kBaseStableTol) ? ct.baseStable + 1 : 0;
    ct.prevBase = rawBase;

    boost::unordered_map<Character*, float>::iterator vo = mountBaseVOffset.find(mount);
    // Poison threshold 300 (25 -> 80 2026-08-24 -> 200 -> 300 2026-08-26): the value has to
    // sit above every species' LEGITIMATE baseline and below a ghost read.  25 was below the
    // Beak Thing's real ~29u, which erased the baseline every sync so damping silently
    // never ran (the "rider's head jerks up" report) and pre-fix sessions capture-looped
    // (8 captures in 3s).  80 then turned out to clear the Leviathan by only 1.4% on the old
    // NECK anchor: measured seat-above-root baseline 78.60 (max 79.01 over 7828 frames), so
    // any retune of that animal would have re-armed exactly the same silent failure.  On the
    // REAR anchor its baseline is larger again (pelvis sits ~40u above the root bone and the
    // up tune stacks on top: 125 measured at up=85.37, up to ~210 at the tuning ceiling), so
    // 200 was itself only one retune away from the same trap.  Ghost reads are unscaled-space
    // (~10x real, ~480 for this mount in REAR), so 300 still separates cleanly.
    static const float kBasePoisonLimit = 300.0f;
    if (vo != mountBaseVOffset.end() && fabsf(vo->second) > kBasePoisonLimit)
    {
        // poisoned baseline (captured on a ghost-bone frame): drop it and recapture
        // below from this frame's values
        mountBaseVOffset.erase(mount);
        vo = mountBaseVOffset.end();
    }
    // Obsolete-seed guard (2026-08-24): a persisted baseline THIS far from live reality
    // is not gait drift - it predates a reference-frame change (the Beak Thing's cfg held
    // 6.63 from the old root-bone-relative era while the current move-relative formula
    // reads ~29.9; damping toward the stale seed dragged the seat ~20u low = deep body
    // clipping).  Wipe immediately once readings are steady, motion or not; damping then
    // stays OFF (natural full follow, slight bounce but no clip) until the stationary
    // capture path below rebuilds a trusted value.
    static const float kBaselineHugeDiff = 12.0f;
    if (vo != mountBaseVOffset.end()
        && ct.syncCount >= kMinSyncsBeforeCapture
        && ct.baseStable >= kCaptureStableNeed
        && fabsf(rawBase - vo->second) > kBaselineHugeDiff)
    {
        mountBaseVOffset.erase(mount);
        vo = mountBaseVOffset.end();
    }
    if (vo != mountBaseVOffset.end()
        && settledStill
        && ct.baseStable >= kCaptureStableNeed
        && fabsf(rawBase - vo->second) > kBaselineStaleDiff)
    {
        // stale: stored baseline disagrees with a settled+stationary reading
        // (captured mid-blend); only ever acted on while truly standing still
        mountBaseVOffset.erase(mount);
        vo = mountBaseVOffset.end();
    }
    if (vo == mountBaseVOffset.end())
    {
        if (ct.baseStable >= kCaptureStableNeed && settledStill)
        {
            mountBaseVOffset[mount] = rawBase;   // settled + stationary -> begin damping next sync
            // Persist once per ride so future mounts/loads seed it directly instead of
            // live-capturing through the post-mount/post-load pose storm.
            boost::unordered_map<Character*, SeatInfo>::iterator ssi = mountSeat.find(mount);
            if (ssi != mountSeat.end() && !ssi->second.tuneKey.empty())
            {
                // rawBase was measured on THIS individual; the species row is in the
                // reference frame, so undo the size adaptation before storing it.  Without
                // this divide, riding one oversized crab would overwrite the family's
                // baseline with a number ~2.2x too large and every normal-sized member
                // would then be damped toward a seat well above its back.
                speciesTuning[ssi->second.tuneKey].base = rawBase / ssi->second.sizeScale;
                SaveConfig();
                try { DebugLog("Riding: baseline captured+saved [" + ssi->second.tuneKey + "]"); } catch(...) {}
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
//
// There is deliberately no lateral damping here.  One was written (2026-08-26) after
// measuring the seat's side offset swinging ~2.4 units per step on a crab, and removed
// again (2026-08-27): that swing is the SHELL moving, the rider tracking it in phase is
// the rider staying put on the shell, and scaling it toward the rigid-body centreline
// makes the shell roll out from under them - which is what "人在横向上左右摇" actually
// was.  Full lateral follow is correct.  The sway visible while the animal stands STILL
// is a different bug with a different fix (GetMountForward's degenerate-baseline guard,
// still in place).  Measurements are archived in RidingPlugin_RE_NOTES.md.
static Ogre::Vector3 ComputeDampedSeatPos(Character* mount, const SeatInfo& seat)
{
    Ogre::Vector3 seatPos = ComputeSeatPosition(seat, mount);
    DampSeatBob(mount, seat, seatPos);     // vertical
    return seatPos;
}

// SEH-guarded read of CharMovement::movementMode (+0x378, dumpbin-verified offset).
// mode=0 was MOVE_NORMAL on rev4's 30-frame watch; -1 = unreadable (dead pointer).
// Standalone helper because __try forbids unwindable objects in the same frame.
static int SafeReadMovementMode(CharMovement* mv)
{
    if (!mv) return -1;
    __try { return *(volatile int*)((char*)mv + 0x378); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return -1; }
}

// Continuous diagnostics (Ctrl+Numpad .): log EVERY frame while the mount is moving,
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
    // Idle frames are logged too (2026-08-26): the crab's side-to-side sway is present
    // while STANDING STILL, so the old "moving only" gate hid exactly the cleanest data
    // (no travel, so any lateral motion is pure gait/idle animation).  The moving flag
    // is kept as the mv= field instead of a filter.
    AnimationClass* mAnim = mount->getAnimationClass();
    AnimationClass* rAnim = rider->getAnimationClass();
    if (mAnim && rAnim)
    {
        Ogre::Vector3 rootP = mAnim->getBoneWorldPosition("Bip01", 1.0f);
        Ogre::Vector3 backP = mAnim->getBoneWorldPosition(seat.backBone, 1.0f);
        int fwSrc = 0;
        Ogre::Vector3 fwd = GetMountForward(seat, mount, &fwSrc);
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
        // read-back of OUR movement write from last frame (see dbgMoveWritten): nonzero =
        // _setPositionSimple did not land, which is the click-hull question, not a race
        Ogre::Vector3 mvwDbg(0.0f, 0.0f, 0.0f);
        boost::unordered_map<Character*, Ogre::Vector3>::iterator mwi = dbgMoveWritten.find(rider);
        if (mwi != dbgMoveWritten.end()) mvwDbg = mwi->second;
        // ragdoll/carry state of the RIDER, per frame.  Needed because the Numpad8 probe
        // can only read the flags in the same frame it calls ragdollModeUT, and a same-
        // frame read is inconclusive when a call lands deferred (exactly how carryModeT
        // behaved).  With these fields a probe press shows up as a 1->0 transition on a
        // later frame - or proves the flag is being re-asserted every frame.
        int ragDbg  = rider->isRagdoll() ? 1: 0;
        int aRagDbg = rAnim->isRagdoll() ? 1 : 0;
        int bcDbg   = rider->_isBeingCarried ? 1 : 0;
        // alive vs destroyed CharMovement (rev6 says pickupObject destroys it; verify
        // per-ride) - and whether something is actively DRIVING it (locomotion modes
        // != 0 while we only ever halt + _setPositionSimple would expose the writer)
        int mmDbg = SafeReadMovementMode(rider->getMovement());
        // ---- lateral (side-to-side) decomposition, 2026-08-26 -------------------
        // World triples can't answer "is the rider sliding sideways on the back?" when
        // the mount also turns.  Project the horizontal offsets onto the mount's TRAVEL
        // frame (mountHeadingDir - the smooth movement-derived heading, not a bone, so
        // it does not itself wobble with the gait):
        //   bRel = anchor bone relative to the mount's rigid movement position
        //   rRel = the rider's rendered root bone relative to that same position
        // A crab-style side sway shows up as bRel.s oscillating with the step cycle and
        // rRel.s following it.  fwdDev = signed yaw of the BONE-derived forward axis
        // (GetMountForward, which scales userOffset/lateral) against the travel heading:
        // if that swings, the tuned offsets get rotated every frame and produce sway of
        // their own even with a perfectly steady anchor.
        // hdSrc: 1 = travel heading (trustworthy, does not wobble), 0 = bone-derived
        // fallback for a mount that has never traveled this ride (then the frame itself
        // wobbles with the gait, so read bRel/rRel side values with that in mind).
        Ogre::Vector3 hdD(0.0f, 0.0f, 0.0f);
        int hdSrc = 0;
        boost::unordered_map<Character*, Ogre::Vector3>::iterator hdi = mountHeadingDir.find(mount);
        if (hdi != mountHeadingDir.end() && hdi->second.length() > 0.001f)
        {
            hdD = hdi->second;
            hdSrc = 1;
        }
        else
        {
            hdD = fwd;      // never traveled: fall back to the bone-derived forward
        }
        float bFwdC = 0.0f, bLatC = 0.0f, rFwdC = 0.0f, rLatC = 0.0f, fwdDevDeg = 0.0f;
        hdD.y = 0.0f;
        if (hdD.length() > 0.001f)
        {
            hdD.normalise();
            Ogre::Vector3 hdSide = Ogre::Vector3(0.0f, 1.0f, 0.0f).crossProduct(hdD);
            if (hdSide.length() > 0.001f) hdSide.normalise();
            Ogre::Vector3 bRel = rootP - dmvP;      bRel.y = 0.0f;
            Ogre::Vector3 rRel = riderBip01 - dmvP; rRel.y = 0.0f;
            bFwdC = bRel.dotProduct(hdD);  bLatC = bRel.dotProduct(hdSide);
            rFwdC = rRel.dotProduct(hdD);  rLatC = rRel.dotProduct(hdSide);
            Ogre::Vector3 fwdFlat = fwd; fwdFlat.y = 0.0f;
            if (fwdFlat.length() > 0.001f)
            {
                fwdFlat.normalise();
                fwdDevDeg = Ogre::Math::ATan2(hdSide.dotProduct(fwdFlat),
                                              hdD.dotProduct(fwdFlat)).valueDegrees();
            }
        }
        // ---- rider POSE state (2026-08-26) --------------------------------------
        // Everything above measures where the seat is.  A rider can be provably rigid on
        // the animal (per-frame world motion identical to the anchor bone's) and STILL
        // visibly tremble, because none of it touches the rider's own animation.  The
        // standing posture is the exposed case: "sitting chair" is a static pose so
        // re-asserting it every frame is invisible, while idle_stand_normal is a live idle
        // loop.  Fields: whether our pose is in the animation set, its weight and its
        // progress (a progress pinned near 0 = something restarts it every frame), the
        // weight of the OTHER posture's pose (nonzero = two full-body poses blending -
        // Mount always starts the sitting pose, so a stand-posture ride used to carry both)
        // and the total action-animation weight (nonzero = the engine is playing something
        // of its own on top).
        const char* poseNameDbg  = (seat.posture == POSTURE_STAND) ? "idle_stand_normal" : "sitting chair";
        const char* otherNameDbg = (seat.posture == POSTURE_STAND) ? "sitting chair" : "idle_stand_normal";
        int   posePlayDbg = 0;
        float poseWDbg = 0.0f, posePDbg = 0.0f, otherWDbg = 0.0f, actWDbg = 0.0f;
        AnimationData* poseDataDbg  = rAnim->getAnimationData(poseNameDbg);
        AnimationData* otherDataDbg = rAnim->getAnimationData(otherNameDbg);
        if (poseDataDbg)
        {
            posePlayDbg = rAnim->getAnimationPlaying(poseDataDbg) ? 1 : 0;
            poseWDbg    = rAnim->getAnimationCurrentWeight(poseDataDbg);
            posePDbg    = rAnim->getAnimationProgress(poseDataDbg);
        }
        if (otherDataDbg)
            otherWDbg = rAnim->getAnimationCurrentWeight(otherDataDbg);
        actWDbg = rAnim->getTotalActionAnimationWeight();
        // body-size watch (diagnostic only, see ReadNodeScale): msc = mount node DERIVED
        // scale, mls = its LOCAL scale, rsc = the RIDER's derived scale for a control
        // reading (a human should be a flat 1.0 - if rsc moves, the field is not what it
        // claims to be).  The point of logging these per frame is stability: a value that
        // is right at mount and drifts, or right and then briefly 10x, is unusable as a
        // seat multiplier no matter how correct its steady state looks.
        Ogre::Vector3 mscDbg = ReadNodeScale(mAnim, true);
        Ogre::Vector3 mlsDbg = ReadNodeScale(mAnim, false);
        Ogre::Vector3 rscDbg = ReadNodeScale(rAnim, true);
        // mfd = the mount's own facing vector, the new fwSrc=4 source.  Logged even when it
        // is NOT the active source so it can be checked against hd= (the proven travel
        // heading) on animals that do walk: if the two point the same way, fwSrc=4 is
        // trustworthy for the animals that never walk; if they are consistently opposed the
        // sign can be flipped without another round trip.  (0,0) = unavailable.
        Ogre::Vector3 mfdDbg;
        if (!GetMountFacingDirection(mount, mfdDbg)) mfdDbg = Ogre::Vector3::ZERO;
        // buffer 3072 (was 2048): the new scale fields sit at the END of the line, so a
        // truncation would silently eat exactly the data this pass was added to collect.
        char dbg[3072];
        _snprintf_s(dbg, 3072, _TRUNCATE,
            "Riding: DBG root=(%.2f,%.2f,%.2f) back=(%.2f,%.2f,%.2f) fwd=(%.2f,%.2f,%.2f) rawT=(%.2f,%.2f,%.2f) tgt=(%.2f,%.2f,%.2f) node=(%.2f,%.2f,%.2f) rMove=(%.2f,%.2f,%.2f) rRoot=(%.2f,%.2f,%.2f) rBip=(%.2f,%.2f,%.2f) nodeQ=(%.2f,%.2f,%.2f,%.2f) rBipQ=(%.2f,%.2f,%.2f,%.2f) move=(%.2f,%.2f,%.2f) anch=(%.2f,%.2f,%.2f) st=%d pelv=(%.2f,%.2f,%.2f) rel=(%.2f,%.2f,%.2f) rs=%d bs=%d wn=(%.2f,%.2f,%.2f) mvW=(%.2f,%.2f,%.2f) rag=%d aRag=%d bc=%d mMode=%d mv=%d hd=(%.2f,%.2f) hdSrc=%d bRel=(f%.2f,s%.2f) rRel=(f%.2f,s%.2f) fwdDev=%.1f fwSrc=%d pose=%d/%.2f/%.2f oth=%.2f act=%.2f msc=(%.3f,%.3f,%.3f) mls=(%.3f,%.3f,%.3f) rsc=%.3f mfd=(%.2f,%.2f)",
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
            mvwDbg.x, mvwDbg.y, mvwDbg.z,
            ragDbg, aRagDbg, bcDbg,
            mmDbg,
            moving ? 1 : 0,
            hdD.x, hdD.z,
            hdSrc,
            bFwdC, bLatC,
            rFwdC, rLatC,
            fwdDevDeg,
            fwSrc,
            posePlayDbg, poseWDbg, posePDbg,
            otherWDbg,
            actWDbg,
            mscDbg.x, mscDbg.y, mscDbg.z,
            mlsDbg.x, mlsDbg.y, mlsDbg.z,
            rscDbg.y,
            mfdDbg.x, mfdDbg.z);
        DebugLog(dbg);
    }
}

// Diagnostic tag naming the tuning row that actually served this seat.  Prints nothing in
// the common case (a per-name row) so existing log lines stay unchanged; prints
// " key=<raceID>" whenever the race fallback layer is what supplied the numbers.  Reading
// a log without this you cannot tell "this species is tuned" from "this species inherited
// its family's row", and those two want opposite follow-up actions.
static std::string SeatKeyTag(const SeatInfo& seat)
{
    if (seat.tuneKey.empty() || seat.tuneKey == seat.species) return std::string();
    return " key=" + seat.tuneKey;
}

// Persist a seat's full tuning state to its tuning entry + riding.cfg.  Writes ALL
// five fields (not just the one a hotkey just changed): the seat IS the live mountSeat
// entry, so every field is current.  Accepted trade-off (2026-08-23): two mounts of the
// same species tuned in interleaved steps overwrite each other's cfg copy - identical to
// what the old full-field writers (up/fwd tune, reset) already did.
// The key is seat.tuneKey, NOT seat.species: a mount served by a race row must keep
// writing back to that race row, otherwise the first tuning keypress (or anchor capture)
// would silently fork a fresh per-name row and the race layer would stop covering it.
static void PersistTuning(const SeatInfo& seat)
{
    if (!seat.tuneKey.empty())
    {
        SpeciesTuning& st = speciesTuning[seat.tuneKey];
        st.seatMode = seat.seatMode;
        st.forceSit = seat.forceSit;
        st.posture = seat.posture;
        st.lateral = seat.lateral;
        st.offset = seat.userOffset;
        st.home = seat.homeOffset;
        st.homeLateral = seat.homeLateral;
        // A species that had no reference size adopts this individual's: the player is
        // looking at the seat they just dialled in, so the animal in front of them IS the
        // size these numbers are confirmed at.  That is the same rule the whole feature
        // rests on, applied to the one moment where it is provably true.  Never overwrite
        // an existing reference - the stored numbers are expressed in THAT frame, and the
        // tuning keys hand us deltas already converted into it.
        if (st.refScale < kSizeReadMin && seat.liveScale >= kSizeReadMin)
            st.refScale = seat.liveScale;
    }
    SaveConfig();
}

// Apply a live tuning step to the currently mounted seat and persist it.  The deltas come
// in as ON-SCREEN world units and are converted into the frame the numbers are stored in,
// so one keypress always moves the rider by the nominal step - see SeatTuneLimitRef.
static void TuneSeat(SeatInfo& seat, float dUp, float dFwd)
{
    seat.userOffset.x += dFwd / seat.sizeScale;
    seat.userOffset.y += dUp / seat.sizeScale;
    ClampTuning(seat.userOffset, SeatTuneLimitRef(seat));

    PersistTuning(seat);

    // up=/fwd= are the STORED numbers (reference frame, what riding.cfg holds); live= is
    // what this individual actually gets.  They differ whenever k != 1, and printing only
    // one of them would make a tuning log unreadable a size bracket later.
    DebugLog("Riding: tuned " + seat.species + SeatKeyTag(seat) + " up=" + IntToStr((int)(seat.userOffset.y * 100.0f))
             + " fwd=" + IntToStr((int)(seat.userOffset.x * 100.0f))
             + " live=" + IntToStr((int)(SeatUp(seat) * 100.0f))
             + "/" + IntToStr((int)(SeatForward(seat) * 100.0f))
             + " k=" + IntToStr((int)(seat.sizeScale * 1000.0f)));
}

// Seed persisted per-species constants into a fresh ride's maps so placement is
// correct from frame one - mounting or restoring after a load no longer depends on
// live-capturing through the post-mount/post-load pose storm.  Species never ridden
// before simply have nothing to seed; the live-capture path remains their fallback.
static void SeedPersistedConstants(Character* mount)
{
    boost::unordered_map<Character*, SeatInfo>::iterator si = mountSeat.find(mount);
    if (si == mountSeat.end() || si->second.tuneKey.empty()) return;
    boost::unordered_map<std::string, SpeciesTuning>::iterator ti = speciesTuning.find(si->second.tuneKey);
    if (ti == speciesTuning.end()) return;
    if (ti->second.anchor.length() > 0.01f && ti->second.anchor.length() <= kMaxAnchorLen)
        mountAnchor.insert(std::make_pair(mount, ti->second.anchor));   // rider-side, size-invariant
    if (fabsf(ti->second.base) > 0.001f)
    {
        // The baseline is stored per species in the reference frame but consumed in world
        // units by DampSeatBob, so it has to be adapted to this individual on the way in -
        // otherwise a scaled member would be damped toward the reference member's back and
        // the kBaselineHugeDiff wipe would fire on every big one.
        float base = SeatBase(si->second, ti->second.base);
        mountBaseVOffset.insert(std::make_pair(mount, base));
    }
    try { DebugLog("Riding: seeded constants [" + si->second.species + "]" + SeatKeyTag(si->second) + " anchor=" + IntToStr((int)(ti->second.anchor.length() * 100.0f))
                   + " base=" + IntToStr((int)(SeatBase(si->second, ti->second.base) * 100.0f))
                   + " k=" + IntToStr((int)(si->second.sizeScale * 1000.0f))); } catch(...) {}
}

void Mount(Character* rider, Character* mount)
{
    if (!rider || !mount) return;
    if (rider == mount) return;
    // Only the player's OWN animals can be ridden (animal + in the player's party).
    // IsRideable = isAnimal() && isWithThePlayer().  This is the single chokepoint all
    // mount paths (the context menu order and its approach state machine) funnel through,
    // so wild/other-faction animals are blocked here regardless of how the mount was
    // requested.
    if (!IsRideable(mount)) { DebugLog("Riding: mount rejected - not a player-owned animal"); return; }
    if (IsRiding(rider)) return;
    // One animal carries one rider (2026-08-26).  mountSeat / mountAnchor / the bob
    // baseline are all keyed by MOUNT, so a second rider on the same animal would share
    // one seat position and one pose constant, and the first dismount would erase those
    // tables out from under the other rider (riderToMount still set = a ride no frame pass
    // places any more).  Refuse here, the one chokepoint every mount path funnels through:
    // whoever boards first keeps the animal and the other order is void.
    {
        boost::unordered_map<Character*, Character*>::iterator occ = mountToRider.find(mount);
        if (occ != mountToRider.end() && occ->second != rider)
        {
            DebugLog("Riding: mount rejected - animal already carries another rider");
            return;
        }
    }

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
    // Dropping the link does NOT end the ragdoll pickupObject started: the "carry
    // dissolved:" line below reports rag=1 on every single mount (the probes read
    // pre/post-pickup/carried = 0/1/1 every time).
    mount->dropCarriedObject(false, true);
    // RESOLVED 2026-08-25.  This used to say the ragdoll CANNOT be cleared mid-ride and
    // that the sitting pose was only a per-frame cover over a live ragdoll (Path A of
    // 2026-08-24 cleared it once here, saw rag=1 again, and concluded it was unclearable).
    // It is clearable - it just has to be re-killed EVERY frame, with the right API:
    //   - the node writer that dragged the rider ~10u off the seat is the ANIMATION-LAYER
    //     ragdoll itself, not the carry link and not _isBeingCarried (bc=0 kept dragging;
    //     zeroing aRag killed the drag outright, wn=(0,0,0) for that frame).
    //   - an engine restorer re-sets it the very next frame, and the QUEUED
    //     Character::ragdollMode is never processed while the movement is destroyed and
    //     the character is carried (probe rev2: three presses, chRag stuck at 1).  So the
    //     teardown must be the INSTANT animation-layer call, _NV_ragdollModeUT(false,
    //     ZERO, CARRY_MODE, ...), issued from SyncRiderNode and HaltAndForceSitPass so the
    //     last writer before render kills it again on every frame of the ride.
    // The sitting pose is therefore just the pose, not a cover.  Two things did NOT change:
    // _isBeingCarried must stay 1 for the whole ride (clearing it lets the mount's collision
    // volume shove the rider off the back), and the carried-side teardown - getDropped,
    // which restores the movement and stands the rider up - still happens at Dismount().
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

    std::string raceName;
    std::string raceKey = GetRaceKey(mount, &raceName);

    // body-size probe.  Logged HERE because the mount instant is the documented worst case:
    // HISTORY §C has the mount's BONE reads landing in unscaled space for the first frames.
    // The 2026-08-28 session showed the scene-node reads are clean at this exact moment,
    // which is what allowed the seat adaptation to key off them - keep logging it so a
    // future regression shows up as a bad nsc= on the very line the seat was built from.
    AnimationClass* mAnimDbg = mount->getAnimationClass();
    Ogre::Vector3 nscDbg = ReadNodeScale(mAnimDbg, true);
    Ogre::Vector3 lscDbg = ReadNodeScale(mAnimDbg, false);
    Ogre::Vector3 bscDbg = ReadBoneScale(mAnimDbg, "Bip01");

    DebugLog("Riding: mounted [" + seat.species + "]" + SeatKeyTag(seat) + " mode=" + IntToStr(seat.seatMode)
             + " race=" + (raceKey.empty() ? std::string("?") : raceKey)
             + (raceName.empty() ? std::string("") : "(" + raceName + ")")
             + " nsc=" + ScaleToStr(nscDbg)
             + " lsc=" + ScaleToStr(lscDbg)
             + " bsc=" + ScaleToStr(bscDbg)
             + (seat.forceWalk ? " walk=1" : " walk=0")
             + " bone=" + seat.backBone
             + " torso=" + IntToStr((int)(seat.torsoLen * 10.0f))
             + " lift=" + IntToStr((int)(seat.lift.y * 100.0f))
             // ref/live/k are the size adaptation: ref= the size the stored numbers were
             // confirmed at (0 = never measured -> no adaptation), live= this individual,
             // k= the ratio actually applied.  tune= stays the STORED pair and live= the
             // adapted one, so a wrong seat can be read off this single line.
             + " ref=" + IntToStr((int)(seat.refScale * 1000.0f))
             + " live=" + IntToStr((int)(seat.liveScale * 1000.0f))
             + " k=" + IntToStr((int)(seat.sizeScale * 1000.0f))
             + " tune=" + IntToStr((int)(seat.userOffset.y * 100.0f)) + "/" + IntToStr((int)(seat.userOffset.x * 100.0f))
             + " adapt=" + IntToStr((int)(SeatUp(seat) * 100.0f)) + "/" + IntToStr((int)(SeatForward(seat) * 100.0f)));
}

void Dismount(Character* rider)
{
    if (!rider) return;

    // stop the ride pose.  BOTH postures are ended: HaltAndForceSitPass asserts whichever
    // one the species asks for, so a stand-posture rider would otherwise walk away still
    // running idle_stand_normal as a slave loop.  Ending an animation that is not playing
    // is a no-op, so this stays safe for the common sitting case.
    rider->endSlaveAnim("sitting chair");
    rider->endSlaveAnim("idle_stand_normal");

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
        dbgMoveWritten.erase(rider);
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
    dbgMoveWritten.erase(rider);
    SeedPersistedConstants(mount);   // frame-one placement from riding.cfg constants

    DebugLog("Riding: restored ride after load [" + seat.species + "]" + SeatKeyTag(seat) + " mode="
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
        // One animal takes one rider, so with several humans selected we have to pick one.
        // selectedCharacters is a hash set, so "the first non-animal" was whichever one the
        // hash happened to hand over - pick the CLOSEST human instead, which is both stable
        // and what the player means by clicking that animal.  An already-mounted human can
        // still end up as the pick when it is the only one selected: Mount() then refuses it
        // and the order is swallowed, exactly as before (letting it fall through to the
        // vanilla Bodyguard order would hand a real task to a seated rider).
        Character* rider = NULL;
        float riderDist = 0.0f;
        bool  riderFree = false;
        ogre_unordered_set<hand>::type::iterator sit = thisptr->selectedCharacters.begin();
        for (; sit != thisptr->selectedCharacters.end(); ++sit)
        {
            Character* c = sit->getCharacter();
            if (!c || c->isAnimal()) continue;
            bool free = !IsRiding(c);
            float cd = RiderMountDist(c, target);
            if (!rider || (free && !riderFree) || (free == riderFree && cd < riderDist))
            {
                rider = c;
                riderDist = cd;
                riderFree = free;
            }
        }
        if (rider)
        {
            // The animal already carries someone: the order cannot be completed, so do not
            // send the rider walking for it at all (Mount() refuses too, but only after a
            // pointless jog).  Swallow it so the vanilla Bodyguard order does not fire
            // instead of the mount the player asked for.
            boost::unordered_map<Character*, Character*>::iterator occ = mountToRider.find(target);
            if (occ != mountToRider.end() && occ->second != rider)
            {
                pendingMount.erase(rider);
                DebugLog("Riding: mount order ignored - animal already ridden");
                return;
            }
            // Behave like the native "pick up" order: mount immediately only if the
            // rider is already next to the animal; otherwise send the rider walking to
            // it and board once it arrives (serviced per-frame in mainLoop_hook).
            if (riderDist < kMountArriveDist)
            {
                pendingMount.erase(rider); // drop any stale in-flight request
                Mount(rider, target);
            }
            else
            {
                PendingMount pm;
                pm.mount    = target;
                pm.age      = 0;
                pm.refresh  = 0;
                pm.lastPos  = rider->getMovement() ? rider->getMovement()->getPosition()
                                                   : rider->getPosition();
                pm.destPos  = target->getPosition();
                pm.idle     = 0;
                pm.moved    = 0.0f;
                pm.bestDist = riderDist;
                pm.stall    = 0;
                pm.pressMoved = 0.0f;
                pm.report   = 0;
                pm.repaths  = 0;
                pm.envAge   = 0;
                pm.envelope = MountBoardEnvelope(target);
                pendingMount[rider] = pm;
                rider->setDestination(pm.destPos, false);
                DebugLog("Riding: rider walking to mount before boarding (d="
                         + IntToStr((int)riderDist)
                         + " env=" + IntToStr((int)pm.envelope) + ")");
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
            if (mit != mountSeat.end() && IsBigMount(mit->second))
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

    // RE-ENABLED 2026-08-24 (disabled 2026-08-23).  The original disable reason - early
    // writes lose to the engine's absolute carried-node pin - died with the immediate-
    // dissolve-carry architecture: nobody else writes the rider's node anymore, so this
    // pass OWNS placement and running it here means the seat is computed from THIS
    // frame's bone transforms.  That freshness is load-bearing for flexTrack anchors
    // (dog family): the waist see-saw is fast, and computing once per game frame BEFORE
    // the skeletons update fed the rider last frame's pose - a visible one-beat lag
    // ("pause the game and he lines up").  Same shared ComputeDampedSeatPos as the
    // main-loop pass so both writers agree bit-for-bit.
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
            // Fire on the mount's own animation update (fresh mount bones) AND - probe
            // 2026-08-25 - on the RIDER's own update.  Bonedog DBG proved an engine
            // writer re-positions the carried rider ~10u every frame AFTER our main-loop
            // write (wn swings 5..14u while unpaused; paused it freezes and the seat
            // snaps correct).  Suspect: the per-frame carried-character maintenance,
            // which keys on _isBeingCarried=1.  If it runs before the rider's own
            // animation update, syncing here makes us the last writer and the pin loses.
            // Still gated per character (NOT per animated char on screen): ungated this
            // hook ran 41x/frame on a busy screen and triple-jittered against the
            // engine's node maintenance.
            bool mountUpdating = (mount->getAnimationClass() == thisptr);
            bool riderUpdating = (rAnim == thisptr);
            if (!mountUpdating && !riderUpdating) continue;
            boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
            if (sit == mountSeat.end()) continue;
            const SeatInfo& seat = sit->second;
            if (!SeatNeedsPlacement(seat)) continue;

            // Horizontal instant; vertical per DampSeatBob - same rule as the main loop
            // so every sync point agrees.
            Ogre::Vector3 seatPos = ComputeDampedSeatPos(mount, seat);
            if (rider->getMovement())
                rider->getMovement()->_setPositionSimple(seatPos);
            SyncRiderNode(rider, mount, rAnim, seatPos, false);
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
    // A paused game freezes exactly what this state machine measures: the rider cannot
    // walk, so every paused frame reads as "it stopped - it must have lost its path".
    // Confirmed in-game: 13 re-path requests inside 1.4s of pause, each one cancelling
    // the path request the previous one had just queued.  Same treatment as DampSeatBob
    // (2026-08-25) - do not advance ANY counter, just leave the request as it is.
    if (ou && ou->isPaused())
        return;

    if (!pendingMount.empty())
    {
        boost::unordered_map<Character*, PendingMount>::iterator it = pendingMount.begin();
        while (it != pendingMount.end())
        {
            Character* rider = it->first;
            Character* mount = it->second.mount;

            bool drop = false;
            bool taken = false;
            if (!rider || !mount || !CharacterLooksLive(rider) || !CharacterLooksLive(mount))
                drop = true;                                        // freed / invalid
            else if (IsRiding(rider))
                drop = true;                                        // already mounted
            else if (mountToRider.find(mount) != mountToRider.end())
                drop = taken = true;                                // mount taken by someone else
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
                // Two riders ordered onto the SAME animal: whoever boards first wins and
                // the other order is void, so stop the loser instead of leaving it running
                // at an animal it can no longer board (the destination we issued is still
                // in its task list).
                if (taken)
                {
                    CharMovement* lost = rider->getMovement();
                    if (lost) lost->halt();
                    DebugLog("Riding: approach cancelled - mount already taken by another rider");
                }
                pendingMount.erase(it++);
                continue;
            }

            CharMovement* move = rider->getMovement();
            float d = RiderMountDist(rider, mount);

            // Keep the boarding envelope honest: the order-time measurement is taken
            // while the animal is far away and its bones are not live (see
            // kMountEnvelopeGap), so widen it from the live skeleton as the rider
            // closes in.  Widest sane reading wins; it is never narrowed.
            if (++it->second.envAge >= kMountEnvelopeGap)
            {
                it->second.envAge = 0;
                float env = MountBoardEnvelope(mount);
                if (env > it->second.envelope)
                    it->second.envelope = env;
            }

            // Track how far the rider has actually walked and whether it is still
            // closing in.  Both arrival fallbacks below hang off these two numbers.
            Ogre::Vector3 rp = move ? move->getPosition() : rider->getPosition();
            Ogre::Vector3 step = rp - it->second.lastPos;
            step.y = 0.0f;
            float stepLen = step.length();
            it->second.moved += stepLen;
            it->second.lastPos = rp;
            if (stepLen < kMountStepEps)
                ++it->second.idle;      // standing still - may have lost its path
            else
                it->second.idle = 0;
            ++it->second.refresh;
            if (d < it->second.bestDist - kMountProgressEps)
            {
                it->second.bestDist = d;
                it->second.stall = 0;
                it->second.pressMoved = 0.0f;
                it->second.repaths = 0;   // the approach is working - reset the back-off
            }
            else
            {
                ++it->second.stall;
                it->second.pressMoved += stepLen;  // walking, but no closer
            }
            // Geometric back-off for the recovery re-paths below: 1x, 2x, 4x, 8x, 16x the
            // base gap, capped.  Only failed recoveries escalate; any progress above puts
            // it back to 1x.
            int repathShift = it->second.repaths < 4 ? it->second.repaths : 4;

            // Proof of travel: only a rider that really walked may use the scale-free
            // signals.  A rider whose CharMovement was destroyed never simulates - it
            // reports "destination reached" forever and never moves - so it is excluded.
            bool walked = it->second.moved > kMountMovedProof;
            // Neither scale-free signal may fire from outside the animal's own body
            // envelope (see MountBoardEnvelope).  This is what stops "the first rider
            // teleports onto its animal the moment a second character is ordered to
            // mount": issuing that second order invalidates the first rider's in-flight
            // path, and a rider with no path reports "destination reached" - which used to
            // be taken at face value from any distance.  Note this bound, not the travel
            // proof, is what keeps a never-simulating rider from boarding across the map.
            bool inEnvelope = d < it->second.envelope;
            // The movement system's own "arrived / can get no closer" flag.  Trusted only
            // after a short settle so a spurious first-frame "reached" (set before pathing
            // starts) cannot board instantly.
            bool nativeReached = move && move->isDestinationReached() && it->second.age > 20;
            bool reached = nativeReached && inEnvelope;
            // Escape hatch for bodies the envelope cannot size (2026-08-27): a swamp
            // turtle's shell holds the rider at d=40 while its spine bones keep the
            // envelope on the 30-unit floor, so nothing could ever board it.  "Kept
            // walking a real distance and stayed at my closest approach" is scale-free
            // evidence of a body in the way, and it is exactly what the failures the
            // envelope guards against do NOT do: a rider with a cancelled path or a
            // destroyed CharMovement stands still, so pressMoved never accumulates.
            // Still bounded - by the hard ceiling, and by having to be AT the closest
            // approach, so chasing an animal that is simply walking away never boards.
            bool pressing = it->second.pressMoved > kMountPressProof
                            && d < it->second.bestDist + kMountPressSlack
                            && d < kMountEnvelopeMax;
            // Huge mounts (Leviathan): the hull parks the rider tens of units from the
            // body centre and the pathfinder may keep pushing without ever setting
            // "reached".  No progress for this long means this IS the arrival - sooner for
            // a rider that never walked, since it was ordered already in place.
            bool stalled = (inEnvelope || pressing)
                           && it->second.stall > (walked ? kMountStallFrames : kMountSettleFrames);
            // "Reached" while still far away is not an arrival, it is a lost path - the
            // order is still ours to finish, so re-issue the destination (rate limited) and
            // keep walking instead of boarding.  Unless the rider is pressing: then the
            // flag means what it says (it can get no closer) and cancelling its walk would
            // only stop the very motion the stall test is waiting on.
            bool lostByReached = nativeReached && !inEnvelope && !pressing
                                 && it->second.refresh >= (kMountReachedRepathGap << repathShift);

            if (debugContinuous && ++it->second.report >= 30)
            {
                it->second.report = 0;
                // rad= is ground truth for a future envelope: torsoLen (front<->rear spine
                // spread) is not a footprint, which is why shell/leg animals keep parking
                // riders outside a torso-sized envelope.  Printed x10 like torso=.
                int radx10 = (int)(mount->getRadius() * 10.0f);
                DebugLog("Riding: approach [" + GetSpecies(rider) + "] d=" + IntToStr((int)d)
                         + " best=" + IntToStr((int)it->second.bestDist)
                         + " env=" + IntToStr((int)it->second.envelope)
                         + " rad=" + IntToStr(radx10)
                         + " moved=" + IntToStr((int)it->second.moved)
                         + " press=" + IntToStr((int)it->second.pressMoved)
                         + " stall=" + IntToStr(it->second.stall)
                         + " idle=" + IntToStr(it->second.idle)
                         + " try=" + IntToStr(it->second.repaths)
                         + " reached=" + IntToStr(move && move->isDestinationReached() ? 1 : 0)
                         + " age=" + IntToStr(it->second.age));
            }

            if (d < kMountArriveDist || reached || stalled)
            {
                // Arrived: stop, board, and consume the request.
                if (move) move->halt();
                Character* r = rider;
                Character* m = mount;
                pendingMount.erase(it++);
                Mount(r, m);
                continue;
            }

            // Still en route: re-path only when it buys something (see kMountRepathGap).
            // Chasing a mount that wandered off the point we aimed at is worth a request;
            // a rider that stopped walking needs one promptly; a rider already walking
            // toward a standing animal does not, and giving it one anyway is what made the
            // first rider hitch whenever a second mount order was issued.
            {
                Ogre::Vector3 mp = mount->getPosition();
                Ogre::Vector3 drift = mp - it->second.destPos;
                drift.y = 0.0f;
                bool mountRoamed = drift.length() > kMountDestMoveEps
                                   && it->second.refresh >= kMountRepathGap;
                // Recovery for a rider that is standing still when it should be walking.
                // Before it has proven it can walk it may simply be waiting for its first
                // path, and re-requesting every few frames would cancel and restart that
                // computation forever - so wait a full second in that case.  Once it HAS
                // walked, recover fast (that pause is what the player sees), but give up
                // once the stall is old: a rider pressed against a huge hull is idle too
                // and boards on the stall test in a moment.
                bool lostPath;
                if (walked)
                {
                    // A rider pressed against a huge hull is "idle" too - but it is inside
                    // the envelope (or pressing against a body the envelope cannot size)
                    // and about to board on the stall test, so churning its path buys
                    // nothing.  Genuinely stuck FAR from the animal means the opposite:
                    // this order is not progressing at all and re-pathing is the only way
                    // to finish it, just on a slower gap.
                    bool hullPressed = (inEnvelope || pressing)
                                       && it->second.stall >= kMountStallFrames / 2;
                    int gap = (it->second.stall < kMountStallFrames / 2) ? kMountIdleRepathGap
                                                                        : kMountReachedRepathGap;
                    lostPath = it->second.idle >= kMountIdleFrames
                               && !hullPressed
                               && it->second.refresh >= (gap << repathShift);
                }
                else
                    lostPath = it->second.idle >= kMountStartIdleFrames
                               && it->second.refresh >= (kMountStartIdleFrames
                                                         << (repathShift < 2 ? repathShift : 2));
                if (lostPath || mountRoamed || lostByReached)
                {
                    it->second.refresh = 0;
                    it->second.idle = 0;
                    it->second.destPos = mp;
                    // A recovery that changed nothing must wait longer next time; chasing a
                    // mount that genuinely walked off is not a failed recovery, so it does
                    // not escalate (it already has its own gap and the mount really moved).
                    if (lostPath || lostByReached)
                        ++it->second.repaths;
                    rider->setDestination(mp, false);
                    if (debugContinuous)
                        DebugLog(std::string("Riding: approach re-path (")
                                 + (lostByReached ? "path cancelled while far"
                                                  : (lostPath ? "rider idle" : "mount roamed"))
                                 + ", d=" + IntToStr((int)d) + ")");
                }
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

            // Same carry-ragdoll re-kill as SyncRiderNode (2026-08-25), but for seats
            // that skip placement entirely (EXACT mode with zero tune): those never
            // reach SyncRiderNode, and a live CARRY_MODE ragdoll drags their node to a
            // garbage slot every frame.  Runs early in the frame; SyncRiderNode repeats
            // it late for the seats that DO get placed.
            {
                AnimationClass* rAnim = rider->getAnimationClass();
                if (rAnim && rAnim->isRagdoll() && !rider->isAnimal())
                    static_cast<AnimationClassHuman*>(rAnim)->_NV_ragdollModeUT(
                        false, Ogre::Vector3::ZERO, RagdollPart::CARRY_MODE,
                        std::string(), rider);
            }

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
                        // The OTHER posture's pose has to go too (2026-08-26).  Mount()
                        // always starts "sitting chair" while the cfg may ask for the
                        // stand posture, so a standing rider used to carry BOTH full-body
                        // poses and render whatever blend of the two the engine settled on.
                        const char* otherAnim = (sit->second.posture == POSTURE_STAND) ? "sitting chair" : "idle_stand_normal";
                        AnimationData* otherData = rAnim->getAnimationData(otherAnim);
                        if (otherData && rAnim->getAnimationPlaying(otherData))
                            rAnim->stopAnimation(otherData);
                        // Re-assert the pose only when it is not already running at full
                        // weight.  Restarting a STATIC pose ("sitting chair") every frame is
                        // invisible, which is why the unconditional version went unnoticed
                        // for months - but idle_stand_normal is a live idle LOOP, and
                        // re-adding it every frame keeps resetting its playback, so the body
                        // trembles instead of standing still.  The weight test keeps the
                        // original guarantee (anything that dilutes our pose gets answered
                        // the very next frame) without touching a pose that is already won.
                        AnimationData* poseData = rAnim->getAnimationData(poseAnim);
                        if (!poseData || !rAnim->getAnimationPlaying(poseData)
                                      || rAnim->getAnimationCurrentWeight(poseData) < 0.99f)
                        {
                            rAnim->runSlaveAnim(poseAnim, 1.0f, 1.0f, 1.0f);
                            rAnim->runAnimation(poseAnim, 1.0f, 1.0f);
                        }
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
            //
            // 2026-08-28: the rider cannot be CLICKED while mounted, and not just on the
            // Beak Thing - on every species tested.  The DBG rMove field is 10..27u off the
            // seat and down at ground level, sitting where the carry system parks a carried
            // body (+23.6 fwd / -4.3 down on a Swamp Turtle; the bison drag slot measured
            // +17.6 / -6.4), so the logical position never gets to the back at all.  Two
            // things follow from that: the mouse picks characters through
            // CharMovement::clickHull - a separate physics object that has to be told where
            // the body went - and _setPositionSimple is being called on a movement that
            // pickupObject DESTROYED, so it may not be landing in the first place.  Hence
            // the read-back into mvW (answers exactly that, in the same breath as the
            // write) and the click hull refresh.  Read mvW like this: ~0 means the write
            // DID land and something later in the frame drags it back to the slot (the
            // engine's own beingCarriedUpdate is the prime suspect, and we are forbidden
            // from hooking it - see the DISABLED HOOKS block - so the answer there is to
            // move this write to the last pass of the frame, not to fight it here);
            // nonzero means the call itself is a no-op on a destroyed movement, and the
            // next lever is teleportCollisionHull(seatPos).
            //
            // There is no setCurrentPosition to write the field behind getPosition():
            // that name belongs to FlockingTools (get-out-of-the-way state), not to
            // AbstractMovementBase.  The field itself is AbstractMovementBase::pos
            // (+0xC4, public) if it ever comes to poking it directly.
            if (rider->getMovement())
            {
                CharMovement* rMv = rider->getMovement();
                rMv->_setPositionSimple(seatPos);
                dbgMoveWritten[rider] = rMv->getPosition() - seatPos;
                if (rMv->hasClickHull())
                    rMv->refreshClickHull();
            }
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
            if (sit != mountSeat.end() && IsBigMount(sit->second))
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

// ---- vanilla-rebindable riding hotkeys ------------------------------------
// Each seat-tuning action is registered as a real Kenshi input command so it
// shows up in Settings->Controls as a rebindable row and its binding persists
// to controls.cfg.  This replaces the old scheme of polling fixed OIS keycodes
// (which could never be rebound and fired even while typing in a text box).
//
// The engine-written state bool lives inside the table (stable static storage),
// so addCommand's bool& has something real to point at.  Defaults preserve the
// 2026-08-27 muscle memory; the player can rebind any of them.  Ctrl variants get
// CTRL_MASK so the vanilla input layer discriminates them from the bare key.
enum RidingCmdId
{
    RCMD_UP, RCMD_DOWN, RCMD_FORWARD, RCMD_BACK,
    RCMD_LEFT, RCMD_RIGHT, RCMD_FORCESIT, RCMD_HOME,
    RCMD_SETHOME, RCMD_DEBUG, RCMD_COUNT
};

struct RidingCommand
{
    const char*         name;        // command id stored in controls.cfg
    const char*         caption;     // English label shown in the controls page (v1)
    int                 defaultKey;  // OIS keycode default binding
    InputHandler::Masks mask;        // modifier the default binding wants
    bool                state;       // engine writes key state here (unused, but a real target)
};

static RidingCommand gRidingCommands[RCMD_COUNT] =
{
    // Modifier defaults go in the KEY argument as a composite int (keycode | mask),
    // NOT in the masks argument - that one registers the command on the BARE key and
    // collides with whoever else owns it (proven 2026-08-28: passing CTRL_MASK as the
    // masks arg made the controls page show "*", not "Ctrl+*", and blanked the bare-key
    // command that lost the slot).  A composite is a DIFFERENT map slot, so Ctrl+NUM*
    // and NUM* coexist - vanilla stores editor_toggle=Shift+F12 the same way.
    // CONFIRMED in-game 2026-08-28: bindings resolved [.. 693 567 .. 585 595] = the four
    // Ctrl variants live, with bare NUM9/NUM. still driving home/force-sit.  This
    // reproduces the whole v1.2.1 numpad layout as the out-of-the-box default, and the
    // vanilla rebind UI captures Ctrl combos too, so players can move any of them.
    // (NUM+/NUM- still collide with vanilla build_tilt_inc/dec, build-mode-only, rebindable.)
    { "riding_seat_up",       "Riding: seat up",           OIS::KC_ADD,        InputHandler::NONE_MASK, false },
    { "riding_seat_down",     "Riding: seat down",         OIS::KC_SUBTRACT,   InputHandler::NONE_MASK, false },
    { "riding_seat_forward",  "Riding: seat forward",      OIS::KC_MULTIPLY,   InputHandler::NONE_MASK, false },
    { "riding_seat_back",     "Riding: seat back",         OIS::KC_DIVIDE,     InputHandler::NONE_MASK, false },
    { "riding_seat_left",     "Riding: seat left",         OIS::KC_MULTIPLY | InputHandler::CTRL_MASK, InputHandler::NONE_MASK, false },
    { "riding_seat_right",    "Riding: seat right",        OIS::KC_DIVIDE   | InputHandler::CTRL_MASK, InputHandler::NONE_MASK, false },
    { "riding_force_sit",     "Riding: toggle force-sit",  OIS::KC_DECIMAL,    InputHandler::NONE_MASK, false },
    { "riding_seat_home",     "Riding: reset seat to zero",OIS::KC_NUMPAD9,    InputHandler::NONE_MASK, false },
    { "riding_seat_sethome",  "Riding: set seat zero here",OIS::KC_NUMPAD9  | InputHandler::CTRL_MASK, InputHandler::NONE_MASK, false },
    { "riding_toggle_debug",  "Riding: toggle diagnostics",OIS::KC_DECIMAL  | InputHandler::CTRL_MASK, InputHandler::NONE_MASK, false },
};

// Set true once our commands are registered, so HotkeyPass never calls
// isKeyState() on a name the engine has not heard of yet.
static bool gRidingCommandsRegistered = false;

// Is our first command still in the engine's name list?  Guards against an
// engine-side initialise() that rebuilds the command map after we registered
// (possible when we register very early, from startPlugin).  Templated so we never
// spell out the map's Ogre::STLAllocator type.
template <class MapT>
static bool HasRidingCommandIn(MapT& m)
{
    return m.find(std::string(gRidingCommands[0].name)) != m.end();
}

static void ValidateRidingRegistration(InputHandler* h)
{
    if (!gRidingCommandsRegistered || !h)
        return;
    bool present = true;
    try { present = HasRidingCommandIn(h->commands); } catch (...) { return; }
    if (!present)
    {
        gRidingCommandsRegistered = false;   // engine wiped them - register again
        DebugLog("Riding: commands vanished from InputHandler, re-registering");
    }
}

// Register all riding commands into the given InputHandler.  Idempotent: the
// flag stops it running twice, and addCommand on an existing name is a no-op /
// overwrite anyway.  Shared by the loadConfig hook and the lazy path below.
//
// addCommand only adds the command to the name list; the engine wires the
// keycode->command binding map inside loadConfig().  The freecam precedent
// registers BEFORE the original loadConfig so that wiring includes it.  We are a
// post-load plugin and register AFTER the boot-time loadConfig, so on the lazy
// path we must re-run loadConfig ourselves (applyNow=true) to get our defaults
// wired and any saved bindings from controls.cfg applied.  On the hook path the
// original loadConfig runs right after us, so applyNow=false there.
static void RegisterRidingCommands(InputHandler* h, bool applyNow)
{
    if (gRidingCommandsRegistered || !h)
        return;
    try
    {
        for (int i = 0; i < RCMD_COUNT; ++i)
            h->addCommand(gRidingCommands[i].name, gRidingCommands[i].state,
                          gRidingCommands[i].defaultKey, OIS::KC_UNASSIGNED,
                          gRidingCommands[i].mask, InputHandler::GLOBAL);
        gRidingCommandsRegistered = true;
        if (applyNow)
            h->loadConfig();   // re-runs our hook (no-op now) then rebuilds bindings
        DebugLog("Riding: registered " + IntToStr(RCMD_COUNT)
                 + " seat commands (applyNow=" + IntToStr(applyNow ? 1 : 0) + ")");
    }
    catch (...) {}
}

// ---- reading the keys back ------------------------------------------------
// isKeyState(name) is DEAD for plugin-added commands (2026-08-28 in-game proof:
// all 10 commands wired, isBound=1111111111, six default bindings visible and
// correct in Settings->Controls, a manual rebind of seat-left to G accepted --
// and not one press ever produced a fire).  Either the engine's own
// clearMessages() zeroes the state bool before our mainLoop pass reads it, or the
// bool is simply never written for commands the engine did not declare itself.
// The freecam precedent does not use isKeyState either: RE_Kenshi dispatches by
// reading key->map[keycode]->name from its own KeyListener.  That is the tell.
//
// So we resolve each command's CURRENT binding out of the engine's live keycode
// map and poll the physical key with OIS ourselves.  Everything the vanilla
// registration bought us is kept (a rebindable row, persistence in controls.cfg,
// a rebind taking effect the moment the player makes it -- because we re-read the
// map, never a compiled-in keycode), while the actual read is the same OIS
// polling that shipped working in v1.2.1.
//
// Map keys are COMPOSITE ints: low byte = OIS keycode (every keycode is <= 0xED),
// high bits = Masks (SHIFT 0x100 / CTRL 0x200 / ALT 0x400).  That is how vanilla
// keeps "Shift+F12" for editor_toggle in a std::map<int, Command*>.
static int gRidingBound[RCMD_COUNT];

// Templated so we never have to spell out the map's Ogre::STLAllocator type.
template <class MapT>
static void ScanRidingBindings(MapT& m)
{
    for (typename MapT::iterator it = m.begin(); it != m.end(); ++it)
    {
        InputHandler::Command* c = it->second;
        if (!c)
            continue;
        for (int i = 0; i < RCMD_COUNT; ++i)
            if (c->name == gRidingCommands[i].name)
            {
                gRidingBound[i] = it->first;
                break;
            }
    }
}

static void RefreshRidingBindings(InputHandler* h)
{
    for (int i = 0; i < RCMD_COUNT; ++i)
        gRidingBound[i] = 0;                       // 0 == unbound (KC_UNASSIGNED)
    if (!h)
        return;
    try { ScanRidingBindings(h->map); } catch (...) {}
}

// True while the command's bound key is held with exactly its modifier set.
// Unbound commands (blank row) simply never fire.
static bool RidingCmdDown(int id)
{
    if (id < 0 || id >= RCMD_COUNT || !(key && key->keyboard))
        return false;
    int b  = gRidingBound[id];
    int kc = b & 0xFF;
    if (kc == 0)
        return false;
    if (!key->keyboard->isKeyDown((OIS::KeyCode)kc))
        return false;
    int mk = b & InputHandler::ALL_MASK;
    bool ctrlNow  = key->keyboard->isKeyDown(OIS::KC_LCONTROL) || key->keyboard->isKeyDown(OIS::KC_RCONTROL);
    bool shiftNow = key->keyboard->isKeyDown(OIS::KC_LSHIFT)   || key->keyboard->isKeyDown(OIS::KC_RSHIFT);
    bool altNow   = key->keyboard->isKeyDown(OIS::KC_LMENU)    || key->keyboard->isKeyDown(OIS::KC_RMENU);
    return ctrlNow  == ((mk & InputHandler::CTRL_MASK)  != 0)
        && shiftNow == ((mk & InputHandler::SHIFT_MASK) != 0)
        && altNow   == ((mk & InputHandler::ALT_MASK)   != 0);
}
// InputHandler::loadConfig only loads bindings for commands that already exist,
// so we register ours BEFORE the original runs (same ordering as the freecam
// precedent).  From then on the bindings ride along with controls.cfg for free.
// CAVEAT: we are a POST-LOAD plugin, so if the game's boot-time loadConfig runs
// before this hook installs, the hook never fires - HotkeyPass registers lazily
// as a fallback (see there), which re-runs loadConfig to wire the bindings.
void (*ridingLoadConfig_orig)(InputHandler* thisptr) = NULL;
void ridingLoadConfig_hook(InputHandler* thisptr)
{
    RegisterRidingCommands(thisptr, false);
    ridingLoadConfig_orig(thisptr);
}

// Inject our rebindable rows into the Settings->Controls page.  addCommand alone
// does NOT make a row appear - the panel is built from DataPanelLine_KeyConfig
// objects fed through addCustomLine.  We anchor on the vanilla "editor_toggle"
// row (stable, always present) and append our rows right after it.  RE_Kenshi
// also hooks addCustomLine and anchors on the same row; the hooks chain, so both
// its freecam row and our rows appear.  Appended rows re-enter this hook but
// their command != "editor_toggle", so they pass straight through - no recursion.
void (*ridingAddCustomLine_orig)(DatapanelGUI* thisptr, DataPanelLine* line) = NULL;
void ridingAddCustomLine_hook(DatapanelGUI* thisptr, DataPanelLine* line)
{
    try
    {
        DataPanelLine_KeyConfig* keyConf = dynamic_cast<DataPanelLine_KeyConfig*>(line);
        // The controls page is the earliest hook we get at MAIN MENU time, where no
        // mainLoop/HotkeyPass runs yet - without this the main-menu page shows all ten
        // rows blank (reported 2026-08-28) because nothing had registered the commands.
        if (keyConf)
        {
            ValidateRidingRegistration(key);
            RegisterRidingCommands(key, true);
        }
        ridingAddCustomLine_orig(thisptr, line);
        if (keyConf && keyConf->command == "editor_toggle")
        {
            for (int i = 0; i < RCMD_COUNT; ++i)
                thisptr->addCustomLine(new DataPanelLine_KeyConfig(
                    gRidingCommands[i].name, gRidingCommands[i].caption, 25));
        }
    }
    catch (...) {}
}

// 2) mount/dismount/tune keys (numpad, physical key detection via OIS) and
// 3) live seat tuning (applies to the currently mounted animal).  The prev-state
//    flags are function locals on purpose: one edge consumer each, persist across
//    frames, reset naturally on DLL reload.
static void HotkeyPass()
{
    if (!(key && key->keyboard && ou->player))
        return;

    // Lazy registration fallback: our loadConfig hook installs at post-load time
    // and may miss the game's boot-time loadConfig, in which case the commands
    // were never wired.  Register on the first frame we have a valid input
    // handler and re-run loadConfig to wire the bindings.  (Idempotent.)
    ValidateRidingRegistration(key);
    RegisterRidingCommands(key, true);

    // Resolve the live bindings out of the engine's keycode map.  Cheap (~80 map
    // entries) and re-run every few frames on purpose: that is what makes a rebind
    // in Settings->Controls take effect immediately, with no compiled-in keycode
    // anywhere in the dispatch path.
    {
        static int bindTick = 0;
        if ((bindTick++ % 15) == 0)
        {
            int before[RCMD_COUNT];
            for (int i = 0; i < RCMD_COUNT; ++i)
                before[i] = gRidingBound[i];
            RefreshRidingBindings(key);
            bool changed = false;
            for (int i = 0; i < RCMD_COUNT; ++i)
                if (before[i] != gRidingBound[i])
                    changed = true;
            if (changed)
            {
                std::string s;
                for (int i = 0; i < RCMD_COUNT; ++i)
                {
                    if (i) s += " ";
                    s += IntToStr(gRidingBound[i]);
                }
                DebugLog("Riding: bindings resolved [" + s + "]");
            }
        }
    }

    // One-shot diagnostic: prints the guard states, so a "keys are dead" report says
    // straight away WHICH layer failed (registration vs the UI keyboard guard).
    static bool hotkeyProbed = false;
    if (!hotkeyProbed)
    {
        hotkeyProbed = true;
        DebugLog("Riding: HotkeyPass first-entry controlEnabled="
                 + IntToStr(key->controlEnabled ? 1 : 0)
                 + " registered=" + IntToStr(gRidingCommandsRegistered ? 1 : 0));
    }

    // Don't act while a UI owns the keyboard (typing in a rename/search box).
    // controlEnabled (InputHandler+0xD0) is the vanilla "gameplay input is live"
    // flag; the old OIS-polling scheme ignored it and fired mid-typing.
    if (!key->controlEnabled)
        return;

    // Nothing to read until our commands are registered.
    if (!gRidingCommandsRegistered)
        return;

    {
        // Seat-tuning keys are vanilla rebindable commands now (Settings->Controls),
        // but the READ is our own: RidingCmdDown polls the physical key the engine
        // map currently holds for that command (isKeyState never fires for
        // plugin-added commands - see the note above RidingCmdDown).  A rebind still
        // takes effect at once, and each command owning its own binding killed the
        // old shared-key KeyEdge-once-per-frame hack (ctrlD/decE).
        static bool prevEdge[RCMD_COUNT] = { false };
        bool edge[RCMD_COUNT];
        for (int i = 0; i < RCMD_COUNT; ++i)
        {
            bool down = false;
            try { down = RidingCmdDown(i); } catch (...) { down = false; }
            edge[i] = KeyEdge(down, prevEdge[i]);
            // Per-press trace, diagnostics only: says WHICH command a key resolved to,
            // which is the one thing a "wrong action fired" report cannot tell us.
            if (edge[i] && debugContinuous)
                DebugLog(std::string("Riding: input '") + gRidingCommands[i].name + "' fired");
        }

        // Debug: toggles continuous ride diagnostics (default Ctrl+Numpad.).  Every
        // ~10 frames we log the mount bones, our target seat, and the rider node.
        if (edge[RCMD_DEBUG])
        {
            debugContinuous = !debugContinuous;
            DebugLog(std::string("Riding: debug continuous ") + (debugContinuous ? "ON" : "OFF"));
        }


        // Dismount has NO hotkey of its own (2026-08-27, user call): the right-click
        // "put down" order is the real dismount entry point, and Numpad2 collided with
        // the vanilla toggle_passive command - one press silently flipped the rider's
        // combat stance as well as dismounting.  Every dismount path (right-click
        // PUT_DOWN, forced dismount in combat) still goes through Dismount(), which runs
        // the full rev6 put-down (getDropped + ragdoll clear) confirmed in-game
        // 2026-08-24 (both hull A/B variants stood the rider up and restored control;
        // hull=true settles straight down, hull=false pops the body up ~19u first, so
        // true won).  The Numpad4/Numpad8 A/B probes and the 30-frame PostDismountWatch
        // that carried the investigation were removed once the fix landed on the real
        // path; the dead ends are preserved in CLAUDE.md.
        // (The 2026-08-25 Numpad8 probe series - bc toggle, queued CARRY_MODE, instant
        // _NV_ragdollModeUT - identified the carried-ragdoll drag as the node writer; the
        // winning fix lives permanently in SyncRiderNode + HaltAndForceSitPass, so the
        // probe key is gone again.)

        // 3) Live seat tuning (applies to the currently mounted animal).
        //    Rider FACING is not tunable - it always follows the mount's travel direction.
        //    Every action is a rebindable vanilla command (Settings->Controls); the
        //    defaults reproduce the 2026-08-27 numpad layout:
        //      up NUM+   down NUM-   forward NUM*   back NUM/
        //      left Ctrl+NUM*   right Ctrl+NUM/   force-sit NUM.
        //      reset-to-zero NUM9   set-zero-here Ctrl+NUM9   diagnostics Ctrl+NUM.
        //    Home returns to the species' declared zero (a separate persisted slot,
        //    cfg cols 15-17, that +/- never touch); set-home declares the current seat
        //    as that zero.  Bindings persist in controls.cfg and are fully rebindable,
        //    so the old "avoid the vanilla numpad commands" hazard no longer applies -
        //    a collision just means two commands share a key, and the player can move
        //    ours.  (Numpad2 dismount and Numpad7 posture were deleted 2026-08-27.)
        {
            bool stepUp      = edge[RCMD_UP];
            bool stepDn      = edge[RCMD_DOWN];
            bool stepFw      = edge[RCMD_FORWARD];
            bool stepBk      = edge[RCMD_BACK];
            bool stepLatR    = edge[RCMD_RIGHT];
            bool stepLatL    = edge[RCMD_LEFT];
            bool stepRst     = edge[RCMD_HOME];
            bool stepSetHome = edge[RCMD_SETHOME];
            bool stepSit     = edge[RCMD_FORCESIT];


            if (stepUp || stepDn || stepFw || stepBk || stepLatR || stepLatL || stepRst || stepSetHome || stepSit)
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
                        else if (stepLatR || stepLatL)
                        {
                            // reference frame, exactly like TuneSeat: the step the player
                            // feels is SeatTuneStep, what gets stored is that divided by k.
                            float lim  = SeatTuneLimitRef(seat);
                            float step = SeatTuneStepRef(seat);
                            float dLat = stepLatR ? step : -step;
                            seat.lateral += dLat;
                            if (seat.lateral < -lim) seat.lateral = -lim;
                            if (seat.lateral >  lim) seat.lateral =  lim;
                            PersistTuning(seat);
                            DebugLog("Riding: lateral " + seat.species + " -> " + IntToStr((int)(seat.lateral * 100.0f))
                                     + " live=" + IntToStr((int)(SeatLateral(seat) * 100.0f)));
                        }
                        else if (stepSetHome)
                        {
                            // "make where I am now the zero" - the reset target moves to
                            // the current seat, so later experimenting can always come
                            // back to it with a bare Numpad9.
                            seat.homeOffset  = seat.userOffset;
                            seat.homeLateral = seat.lateral;
                            PersistTuning(seat);
                            DebugLog("Riding: home set " + seat.species
                                     + " up=" + IntToStr((int)(seat.homeOffset.y * 100.0f))
                                     + " fwd=" + IntToStr((int)(seat.homeOffset.x * 100.0f))
                                     + " lat=" + IntToStr((int)(seat.homeLateral * 100.0f)));
                        }
                        else if (stepRst)
                        {
                            // back to the declared zero (NOT the bare geometric base):
                            // for a cfg migrated from before this feature the zero is the
                            // seat that was already tuned in, so nothing dialled in is lost.
                            seat.userOffset = seat.homeOffset;
                            seat.lateral    = seat.homeLateral;
                            PersistTuning(seat);
                            DebugLog("Riding: reset " + seat.species + " to home"
                                     + " up=" + IntToStr((int)(seat.userOffset.y * 100.0f))
                                     + " fwd=" + IntToStr((int)(seat.userOffset.x * 100.0f))
                                     + " lat=" + IntToStr((int)(seat.lateral * 100.0f)));
                        }
                        else
                        {
                            float st = SeatTuneStep(seat);
                            float dUp = 0.0f, dFwd = 0.0f;
                            if (stepUp)   dUp = st;
                            if (stepDn)   dUp = -st;
                            if (stepFw)   dFwd = st;
                            if (stepBk)   dFwd = -st;
                            TuneSeat(seat, dUp, dFwd);
                        }
                    }
                }
                else
                {
                    DebugLog("Riding: select the rider first, then use the seat-tuning keys (see Settings->Controls)");
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

// True if the player's current selection contains at least one non-animal
// (a human who could actually mount).  The context-menu rename gates on this:
// an animal-only selection right-clicking a rideable animal issues a REAL
// Bodyguard order (animal guards animal), not a mount, so the vanilla "侍卫"
// label must stay - "上马" only makes sense when a human is selected to ride.
// Reads selectedCharacters with a selectedCharacter fallback, the same way the mount
// order handler picks the rider.
static bool SelectionHasRider()
{
    PlayerInterface* player = ou ? ou->player : NULL;
    if (!player) return false;
    ogre_unordered_set<hand>::type::iterator sit = player->selectedCharacters.begin();
    for (; sit != player->selectedCharacters.end(); ++sit)
    {
        Character* c = sit->getCharacter();
        if (c && !c->isAnimal()) return true;
    }
    if (player->selectedCharacter)
    {
        Character* sel = player->selectedCharacter.getCharacter();
        if (sel && !sel->isAnimal()) return true;
    }
    return false;
}

void (*contextMenu_show_orig)(ContextMenuGUI* thisptr, const lektor<int>& ordersList, const std::string& _name, bool offset) = NULL;

// hook implementation: rename the Bodyguard entry to 上马 for rideable player-owned
// animals only, AND only when a human is selected to ride.  All other targets
// (humans, wild animals) and animal-only selections are left untouched.
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

        bool rideable    = IsRideable(target);
        bool selHasRider = SelectionHasRider();  // is a human selected to actually ride?

        try { char dbg0[256]; _snprintf_s(dbg0, 256, _TRUNCATE, "Riding: ctxMenu target=%p orders=%d children=%d rideable=%d rider=%d", (void*)target, orderCount, childCount, rideable ? 1 : 0, selHasRider ? 1 : 0); DebugLog(dbg0); } catch(...) {}


        // IMPORTANT: the context-menu ordersList uses a DIFFERENT enumeration than
        // TaskType.  In-game dump (Kenshi 1.0.65) proved: 26=Trade, 31=Bodyguard,
        // 45=Follow, 69=KnockOut, 225=PickUp.  TaskType::BODYGUARD compiles to 45 here
        // (which is Follow in the menu enum), so the menu id for Bodyguard is 31.
        // (newPlayerTask still uses TaskType::BODYGUARD correctly: clicking the
        // order-31 row is dispatched by the game as TaskType 45.)
        const int kMenuOrderBodyguard = 31;

        // per-row mapping dump, only when continuous diagnostics is on (Ctrl+Numpad .).
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
        // Bodyguard order AND when the current selection contains a human who
        // could mount.  Everything else stays untouched so vanilla "Bodyguard"
        // keeps working - no hiding, no layout change.  The selHasRider gate is
        // the fix for "animal selected + right-click another animal shows 上马":
        // an animal can't ride, so that Bodyguard order is a real guard command
        // and must keep its vanilla label.
        if (!rideable || !hasBodyguardOrder || !selHasRider)
        {
            if (debugContinuous && rideable && hasBodyguardOrder && !selHasRider)
                DebugLog("Riding: ctxMenu no human in selection - vanilla bodyguard label kept");
            return;
        }

        // ⚠️ optionsList children are NOT guaranteed to be index-parallel to
        // ordersList: observed 2026-08-23 on wounded animals - the 急救 (first aid,
        // order 25) row renders at the TOP while sitting at the END of ordersList,
        // shifting every later row down by one.  Index-based lookup then renamed the
        // FOLLOW row -> "right-click UI scrambled" on bull / caged beast / beak thing.
        // Locate the button BY ITS VANILLA CAPTION instead: the caption is literally
        // what the user sees, so a match can never scramble the menu.  Never map by
        // position, whatever changes around here.
        //
        // The vanilla caption is localized, so match a per-language whitelist.
        // Byte-collected from RE_Kenshi_log.txt row dumps (the order=31 line), one
        // run per game language, 2026-08-25: en/de/fr/ru/es/pt/ja/ko (+zh earlier).
        // Order mapping 26=Trade 45=Follow 31=Bodyguard 225=PickUp came out identical
        // in every language = cross-check passed.  All non-ASCII text is explicit
        // UTF-8 byte escapes (v100 has no /utf-8).  An unknown language finds no
        // match -> menu left untouched; clicking the vanilla label still mounts via
        // the newPlayerTask hook (dispatch keys on the internal order id, not the
        // label).  To add a language: turn on Numpad-. diagnostics, right-click a
        // rideable animal in that language, copy the order=31 cap='...' verbatim.
        static const char* const kBodyguardCaps[] = {
            "\xE4\xBE\x8D\xE5\x8D\xAB",                                             // zh-CN 侍卫
            "Bodyguard",                                                            // en
            "Leibw\xC3\xA4" "chter",                                                // de Leibwächter ("c" after \xA4 would extend the hex escape -> split literals)
            "Garde du corps",                                                       // fr
            "\xD0\x9E\xD1\x85\xD1\x80\xD0\xB0\xD0\xBD\xD1\x8F\xD1\x82\xD1\x8C",     // ru Охранять
            "Guardaespaldas",                                                       // es
            "Proteger",                                                             // pt-BR
            "\xE3\x83\x9C\xE3\x83\x87\xE3\x82\xA3\xE3\x82\xAC\xE3\x83\xBC\xE3\x83\x89", // ja ボディガード
            "\xED\x98\xB8\xEC\x9C\x84\xED\x95\x98\xEA\xB8\xB0",                     // ko 호위하기
        };
        // Replacement label for the SAME INDEX above - the match itself identifies
        // the game language, no separate detection needed.  Non-ASCII = explicit
        // UTF-8 bytes.  (Space after \x8C in ru is safe: it can't extend a hex escape.)
        static const char* const kMountLabels[] = {
            "\xE4\xB8\x8A\xE9\xA9\xAC",                                             // zh-CN 上马
            "Ride",                                                                 // en
            "Reiten",                                                               // de
            "Monter",                                                               // fr
            "\xD0\xA1\xD0\xB5\xD1\x81\xD1\x82\xD1\x8C \xD0\xB2\xD0\xB5\xD1\x80\xD1\x85\xD0\xBE\xD0\xBC", // ru Сесть верхом
            "Montar",                                                               // es
            "Montar",                                                               // pt-BR
            "\xE9\xA8\x8E\xE4\xB9\x97",                                             // ja 騎乗
            "\xED\x83\x80\xEA\xB8\xB0",                                             // ko 타기
        };
        const int nCaps = (int)(sizeof(kBodyguardCaps) / sizeof(kBodyguardCaps[0]));
        MyGUI::Widget* found = NULL;
        int foundIdx = -1, foundLang = -1, matchCount = 0;
        for (int i = 0; i < childCount; ++i)
        {
            MyGUI::Widget* child = opts->getChildAt(i);
            if (!child) continue;
            std::string cap;
            try { cap = GetWidgetCaptionDeep(child); } catch(...) { cap.clear(); }
            if (cap.empty()) continue;
            bool isBodyguard = false;
            for (int c = 0; c < nCaps && !isBodyguard; ++c)
                if (cap == kBodyguardCaps[c]) { isBodyguard = true; foundLang = c; }
            if (!isBodyguard) continue;
            found = child; foundIdx = i; ++matchCount;
        }
        // Rename only on a UNIQUE match: zero hits (unknown language) and multiple
        // hits (never guess between rows) both leave the menu untouched.
        if (matchCount == 1 && found && foundLang >= 0)
        {
            // localized rename, e.g. Bodyguard -> Ride / 上马 / Reiten ...
            const char* label = kMountLabels[foundLang];
            bool ok = SetWidgetCaptionDeep(found, label);
            try { opts->_updateChilds(); } catch(...) {}
            try { DebugLog(std::string("Riding: rename Bodyguard->label idx=") + IntToStr(foundIdx) + " lang=" + IntToStr(foundLang) + (ok ? " ok" : " FAILED(no caption widget)")); } catch(...) {}
        }
        else if (debugContinuous)
        {
            try { DebugLog(std::string("Riding: bodyguard caption matches=") + IntToStr(matchCount) + " - menu untouched"); } catch(...) {}
        }

    } catch(...) {}
}

__declspec(dllexport) void startPlugin()
{
    DebugLog("RidingPlugin: start");

    LoadConfig();
    if (!speciesTuning.empty())
        DebugLog("RidingPlugin: " + IntToStr((int)speciesTuning.size()) + " seat tuning entries active ("
                 + IntToStr(kDefaultSeatCount) + " built-in defaults + riding.cfg overrides)");

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

    // Register our seat-tuning commands before the config loads, and inject their
    // rebindable rows into the Settings->Controls page.  Both mirror the freecam
    // precedent in RE_Kenshi/MiscHooks.cpp; the hooks chain with RE_Kenshi's own.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&InputHandler::loadConfig), (void*)&ridingLoadConfig_hook, (void**)&ridingLoadConfig_orig))
        ErrorLog("RidingPlugin: Could not hook InputHandler::loadConfig!");

    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&DatapanelGUI::addCustomLine), (void*)&ridingAddCustomLine_hook, (void**)&ridingAddCustomLine_orig))
        ErrorLog("RidingPlugin: Could not hook DatapanelGUI::addCustomLine!");

    DebugLog("RidingPlugin: hooks installed");

    // Register the seat commands RIGHT NOW if the input handler already exists.
    // Post-load plugins arrive after the engine's boot-time loadConfig, so the hook
    // above will not fire for it; the lazy paths (controls page / first in-game
    // frame) do catch up, but the FIRST controls page built in the same call that
    // triggers a lazy registration draws our rows blank (reported 2026-08-28 for the
    // main-menu page).  Registering here beats every panel build.  If the engine has
    // not constructed `key` yet, or wipes our commands in a later initialise(), the
    // lazy paths self-heal (see HasRidingCommand).
    if (key)
    {
        RegisterRidingCommands(key, true);
        DebugLog("RidingPlugin: early command registration "
                 + std::string(gRidingCommandsRegistered ? "done" : "failed"));
    }
    else
        DebugLog("RidingPlugin: no InputHandler yet, deferring command registration");
}
