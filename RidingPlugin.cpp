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
#include <kenshi/CharacterHuman.h>   // P4-1h: weaponInHands 0x6D8 / sheath location 0x6E0
#include <kenshi/Building/Building.h>
#include <kenshi/Enums.h>
#include <kenshi/Damages.h>
// Inventory.h is load-bearing for P4-1e: mounting sheathes the rider's weapon, so the
// subject for drawWeapon() has to come out of the equipment slots rather than from
// getThePreferredWeapon() (which reads NULL in that state).  getPrimaryWeapon() /
// getSecondaryWeapon() / getEquippedWeapons() live on Inventory and Character.h only
// forward-declares the class.  Unlike combat/CombatClass.h this one #includes cleanly:
// its neighbours are spelled "Enums.h" / "util/lektor.h" / "Item.h" and it sits in
// kenshi/ itself, so every one of those resolves relative to its own directory.
#include <kenshi/Inventory.h>
// Back for P4-3 step 2's THIRD probe (2026-09-02): the member-pointer typedef that picks the
// detachItem(const std::string&) overload needs the class, and the hand-slot poll needs
// getAttachedEntity (Appearance.h:69).  There is no `class Appearance`: both are declared on
// AppearanceBase, which is exactly what Character::getAppearance() (Character.h:584) hands back.
#include <kenshi/Appearance.h>
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
// OgreAnimationState.h is load-bearing for the pose-weight pin (v1.6):
// SingleAnimation::mainState is an Ogre::AnimationState* and setWeight() is the
// value Ogre actually blends from at render time.  Nothing else pulls it in.
#include <ogre/OgreAnimationState.h>
// OgreOldBone.h / OgreOldSkeletonInstance.h are load-bearing for the P2-1b leg pose:
// AnimationClass::_getBone returns Ogre::OldBone* (setManuallyControlled / reset live
// there) and AnimationClass::skeleton is an Ogre::OldSkeletonInstance* whose base
// Skeleton gives getNumBones/getBone for the bone-name inventory.
#include <ogre/OgreOldBone.h>
#include <ogre/OgreOldSkeletonInstance.h>
// OgreAnimation.h + OgreAnimationTrack.h + OgreKeyFrame.h are load-bearing for P4-6m:
// Skeleton::hasAnimation/getAnimation plus OldNodeAnimationTrack::getInterpolatedKeyFrame
// sample the technique clip's own hand tracks (the wrist-delta writer).
#include <ogre/OgreAnimation.h>
#include <ogre/OgreAnimationTrack.h>
#include <ogre/OgreKeyFrame.h>

#include <mygui/MyGUI_Window.h>

#include <boost/unordered_map.hpp>
#include <string>
#include <vector>

#include <windows.h>
#include <stdio.h>
#include <intrin.h>     // P4-3-1: _ReturnAddress() - names the caller inside a jmp-detour hook

// ---- minimal CombatClass shim (RE_NOTES §13 technique, second use) -------------------
// P4-1b needs the engine's OWN combat bookkeeping to EXPLAIN why a mounted rider is not a
// combat participant, instead of only observing that it isn't.  That lives in
// kenshi/combat/CombatClass.h, which cannot be #included: it pulls its neighbours in as
// "Enums.h" / "util/hand.h" / "util/lektor.h" / "util/OgreUnordered.h" while those actually
// sit at kenshi/Enums.h and kenshi/util/, so it only resolves with /I...\Include\kenshi
// added to the committed build script.
//   Instead declare exactly the members we call.  The call mangles to the same symbol
// KenshiLib.lib exports and RE_Kenshi patches that stub to the real address at load, so a
// successful LINK is itself the proof that the signature matches.  This is safe because
//   - every member below is NON-virtual (verified against the lib's own mangled names:
//     QEAA/QEBA = public non-virtual; the virtual ones come out UEAA), so there is no vtable
//     layout to reproduce and no this-adjustment;
//   - the shim declares no data members and we never construct, copy or size one - the
//     object always arrives as a Character::getCombatClass() pointer passed through
//     verbatim as `this`;
//   - no header in the tree includes CombatClass.h (zero hits) and `class CombatClass;` is
//     only ever forward-declared (kenshi/Character.h:33, kenshi/CharBody.h:13), so this
//     definition cannot collide (C2011).
// ⚠ MEASURED, and it overturns what this comment used to claim: the rider is NOT a
// CombatClassPlayer.  P41C logged the vtable pointer of the rider's, the mount's and the
// enemy's CombatClass and got vpR = vpM = vpE = 41DB5688 in 4/4 reads - the same class for
// all three, and the mount and the enemy are plainly AI.  So virtual dispatch on
// initCombatMode() reaches CombatClassAI::initCombatMode (CombatClass.h:286, RVA 0x667A60),
// not the base body at 0x665230.  The old reasoning ("CombatClassPlayer overrides nothing,
// so the base body IS the player body") was sound but applied to the wrong class.
//   ⚠ This is NOT a bug fix.  Calling 0x665230 non-virtually DID work in P4-1c: rungs 1/2
// flipped combatModeActive and rTgt to 1 and they stayed up to end of log.  Swapping to the
// AI twin below (also exported as a non-virtual _NV_ stub, so it costs one declaration)
// replaces "happens to work" with "correct by semantics".
// (gotMoreImportantThingsToDoThanFightingYou() and hasFocusedTarget() are still absent -
// they are overridden in the AI class and we have never needed them.)
// ⚠ KenshiLib's "// protected" / "// private" annotations in that header describe the
// ORIGINAL game source, not the exported symbol: the lib mangles ALL of these with public
// access, which is why weaponReach / getNearestEnemyInAttackZone / isInAttackZone /
// setAttackTarget link fine from a public shim.  Do not "fix" the access to match the
// comments - that would mangle to IEAA and fail to link.
class CombatClass
{
public:
    bool       _isInCombatMode() const;              // RVA 0x43FCD0
    int        getNumOpponents() const;              // RVA 0x2B2B90
    int        getNumWaitingAttackers() const;       // RVA 0x2B2670
    bool       isInAttackerListH(Character* c);      // RVA 0x664FD0
    bool       addAttackerH(Character* c);           // RVA 0x6666A0
    hand       _getAttackTarget() const;             // RVA 0x339E30
    float      isAttacking(Character* who);          // RVA 0x664CA0
    float      weaponReach();                        // RVA 0x607BA0  ("protected" in header)
    bool       isInAttackZone(Character* who);       // RVA 0x607CE0  ("protected" in header)
    Character* getNearestEnemyInAttackZone();        // RVA 0x6090B0  ("protected" in header)
    void       setAttackTarget(Character* c);        // RVA 0x664E00  ("protected" in header)
    void       setAttackTargetHandle(Character* c);  // RVA 0x664ED0  ("protected" in header)
    // P4-1c additions.  Everything above only READS or moves a target pointer; these are the
    // first members here that can change what the engine does with a rider on its own.
    swordStateEnum getCombatState() const;           // RVA 0x333D30
    swordStateEnum getBlockStateEnum();              // RVA 0x664BD0
    void           changeState(swordStateEnum newState, float minTime);
                                                     // RVA 0x2B25F0  ("protected" in header)
    // P4-1c had calculateTargetsInAttackZone() (RVA 0x608020) here as rung 3.  REMOVED: the
    // offset-ordered timeline puts the run's access violation immediately after rung 2, i.e.
    // in this call, and the likeliest reason is that it dereferences currentTechnique(0x150)
    // which was NULL in 4/4 reads.  P4-1d supplies a technique instead of asking the engine
    // to recompute a zone without one, so there is nothing left to call it for.
};

// CombatClassAI - the rider's ACTUAL class (measured; see the ⚠ above).  One method, same
// shim technique, same safety argument: non-virtual _NV_ twin, no data members, and
// `class CombatClassAI` appears nowhere in the include tree (CombatClass.h is never included
// and nothing forward-declares it), so this cannot collide.  Single inheritance from
// CombatClass (CombatClass.h:273) with CombatClass as the first and only base means the AI
// subobject starts at offset 0, so a CombatClass* may be cast across without adjustment.
class CombatClassAI
{
public:
    bool _NV_initCombatMode(const hand& subject, int end, bool focusedTargetMode);
                                                     // RVA 0x667A60 (CombatClass.h:287)
};

// CharStats - the technique chooser.  Collision-free: nothing in the tree includes
// CharStats.h, and `class CharStats;` is only ever forward-declared (AI/AI.h:46,
// Character.h:34, CharBody.h:14, CharMovement.h:221, combat/CombatClass.h:19, Dialogue.h:273,
// MedicalSystem.h:90).  Its only base is the empty Ogre::GeneralAllocatedObject, as with
// CombatClass, so `this` needs no adjustment.
//   ⚠ chooseAttack takes weaponReach as an ARGUMENT.  That is the whole reason this shim
// exists: P4-1c measured the rider's own reach at 0.00 in 4/4 reads, so asking the engine
// "which attack would you pick" with a synthetic reach breaks the no-reach/no-technique/
// no-reach circle instead of being trapped by it.
class CharStats
{
public:
    CombatTechniqueData* getBashAnimation(float range);              // RVA 0x885C70
    CombatTechniqueData* chooseAttack(float range, float weaponReach,
                                      CombatTechniqueData* lastAttack,
                                      bool opponentIsStationary);    // RVA 0x886880
    // Resolve the exact six-field damage packet the engine prepares for this target.
    // MSVC x64 returns this 24-byte aggregate through its standard hidden result pointer;
    // the method's decorated name does not encode its return type, so this ABI-compatible
    // shim links to CharStats::getTotalAttackDamageFor(Character*) unchanged.
    Damages getTotalAttackDamageFor(Character* target);               // RVA 0x885100 (CharStats.h:186)
    // Kenshi's finished, equipment- and skill-adjusted attack-speed multiplier.
    // Consume the engine getter so injury/equipment recalculation remains authoritative.
    float getAttackSpeed();                                           // RVA 0x2ADAF0 (CharStats.h:231)
};

// Weapon - only to name the category of whatever getThePreferredWeapon() hands back, so that
// "the rider is unarmed" and "the rider has a weapon it never drew" are distinguishable in
// the log.  There is no Weapon.h in KenshiLib; Weapon lives in Gear.h:41 as
// `class Weapon : public Gear` and Gear.h:5 as `class Gear : public Item`, both annotated
// "offset = 0x0".  Collision-free: nothing includes Gear.h except Gear.h's own consumers of
// Item.h, and `class Weapon;` is only forward-declared elsewhere (Character.h:39,
// CharStats.h:18, Inventory.h:115, Item.h:105, Animation/AnimationClassHuman.h:5).
class Weapon
{
public:
    WeaponCategory getCategory() const;              // RVA 0x5C71D0 (Gear.h:49)
};

// P4-1c raw-field reads.  The shim above deliberately declares no data members, so
// KenshiLib's documented offsets are applied to the raw pointer instead.  They ARE offsets
// from `this`: CombatClass's only base, Ogre::GeneralAllocatedObject, is an empty allocator
// policy with neither data nor a vtable of its own, so the class's own vptr sits at 0.
// Every read is paired with an API call that must agree with it - an offset that has drifted
// between game versions shows up as a disagreement instead of as a plausible wrong number.
static inline int CcBool(const void* cc, int off)
{ return cc ? (int)*(const unsigned char*)((const char*)cc + off) : -1; }
static inline int CcInt(const void* cc, int off)
{ return cc ? *(const int*)((const char*)cc + off) : -1; }
static inline float CcFloat(const void* cc, int off)
{ return cc ? *(const float*)((const char*)cc + off) : -1.0f; }
static inline int CcPtrSet(const void* cc, int off)
{ return cc ? (*(void* const*)((const char*)cc + off) ? 1 : 0) : -1; }
// Low 32 bits only: the vtable pointer is just an identity token here ("same class or not"),
// and a full 64-bit address makes the log line unreadable for no gain.
static inline unsigned int CcVptrLo(const void* cc)
{ return cc ? (unsigned int)((unsigned __int64)*(void* const*)cc & 0xFFFFFFFFu) : 0u; }
// P4-1d: the one WRITE into the raw layout.  currentTechnique(0x150) is what P4-1c measured
// as NULL in 4/4 reads while combatModeActive was already 1 - i.e. the state machine is
// turning but has nothing to swing with.  There is no setter in the header, so rung 2 pokes
// the field and lets the running machine pick it up.  Only ever called with a pointer that
// CharStats::chooseAttack just returned, never with a fabricated one.
static inline void CcSetPtr(void* cc, int off, void* v)
{ if (cc) *(void**)((char*)cc + off) = v; }

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

// The ride pose.  ONE record name, no alternatives (TASK.md P2-0, 2026-08-29).
//
// This used to be a two-valued RiderPosture (0 = "sitting chair", 1 = the standing
// idle "idle_stand_normal", selectable per species via riding.cfg column 9).  The
// standing branch is gone: every one of the 21 shipped seat rows was posture 0, the
// standing path was reachable only by hand-editing a cfg, and it cost a permanent
// tax everywhere - two poses to assert, two to stop, two to end on dismount, plus
// the "stop the OTHER pose" machinery that existed purely because Mount() always
// started the sitting pose while the cfg might ask for the other one.  cfg column 9
// is now a dead field (parsed into a dummy, written as literal 0) exactly like
// column 4 and columns 6-8.
//
// ⚠️ This name is a RECORD name (the key in AnimsListsManager's allAnims), not a
// clip name - clip names carry a numeric suffix the engine reassigns every load.
// It is verified present in the vanilla human table (P1: lay=UPPER cat=NORMAL
// flags=whole,loop,action,Rarm), which is what makes the per-frame
// getAnimationData(kRidePose) safe - that call INSERTS a NULL on a miss.
static const char* const kRidePose = "sitting chair";

// 🆕 P2-4 Option A (2026-09-07): the cushion tier pins THIS record instead.  The legs were always
// ours there (kRideCushionLeg, baked from this same clip @ t=8.8238), but the UPPER BODY - hands
// included - was still the host's 'sitting chair', and the user read the difference on screen
// (R UpperArm differs 55.5° between the two clips, so the hands were visibly NOT sitting idle).
// Same record class as kRidePose (verified in gamedata.base: UPPER cat=NORMAL flags=whole,loop),
// so every getAnimationData below stays miss-free.  Three facts that make the swap safe:
//   * root bone (Bip01) translation is essentially constant over the whole 10.9 s (ty span 0.02
//     at -9.07, vs sitting chair's -3.62) - the node solver already absorbs a constant offset;
//   * all four leg tracks are CONSTANT in this record, so the host writes exactly the values our
//     baked table writes - no fight between the clip and kRideCushionLeg;
//   * the torso/hand fidget is a 10.9 s loop - exactly the 原版动作 the user asked for.
static const char* const kRideCushionPose = "sitting idle";

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
// Frames on which that refresh was VETOED are kept in RideCombatRuntime, keyed by rider.
// They are printed on the ungated P43SW ride line because every ride emits it, not because
// heading vetoes have anything to do with swinging.

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

// ---- P3-0 probe (2026-08-29): WHEN in the frame is the logical position dragged
//      back to the carry park slot, and does the click hull still exist? ----------
// mvW above already settled "does the write land": ZERO on all 19511 frames of the
// v1.6 log, while rMove stayed 5..12u off the seat and down at ground level.  So the
// write is fine and something later puts it back - but the existing DBG line samples
// rMove BEFORE this frame's own write (DebugLogRideFrame runs ahead of it inside
// SyncMountedRiders), so no existing log can say WHERE in the frame that happens.
// Four samples of (movement position - seat) bracket the two writers we own:
//     aPre / aPost = animUpdate resync (fires INSIDE the engine's update phase)
//     mPre / mPost = main-loop SyncMountedRiders (after the whole update, pre-render)
// Frame order is: engine update (animation updates happen inside it) -> our main-loop
// passes -> render.  So the sample order is aPre, aPost, mPre, mPost, then next
// frame's aPre.  Read the gaps:
//     mPre big, aPre ~0  => dragged AFTER the animation updates, same frame
//     aPre big, mPre ~0  => dragged EARLY in the update phase, before animation
//     both big           => dragged twice per frame
//     hits == 0          => the resync never fired for this rider, so aPre/aPost are
//                           stale and mPre covers the whole frame by itself
// A resync that is throttled or never firing is itself a candidate root cause, hence
// the per-frame fire count rather than a bare "did it run" flag.
// ANSWERED 2026-08-29 (P3-0, 105 lines / 2 rides, no exceptions): aPre=ZERO always,
// mPre=7.6..13.5u, mPost=ZERO always, hits=2 always => the drag-back runs after the
// rider's animUpdate and before our main-loop pass, once per frame, and the seat write
// then holds all the way through render into the next frame's animUpdate.  Kept as the
// regression check for any future change to where/when we write the position.
struct P3Sample
{
    Ogre::Vector3 aPre, aPost, mPre, mPost;
    int  animHits;   // resync fires since the last main-loop pass
    bool sawAnim;    // has the resync EVER fired for this rider
    P3Sample() : aPre(Ogre::Vector3::ZERO), aPost(Ogre::Vector3::ZERO),
                 mPre(Ogre::Vector3::ZERO), mPost(Ogre::Vector3::ZERO),
                 animHits(0), sawAnim(false) {}
};
boost::unordered_map<Character*, P3Sample> p3Probe;
static const unsigned int kP3LogGap    = 60;   // frames between P3 lines
static const int          kP3LogBudget = 80;   // lines per ride, debug only
static int          gP3Budget = 0;
static unsigned int gP3Frames = 0;
// refreshClickHull() calls the rider needed this ride.  ANSWERED 2026-08-29 (P3-2): it is
// NOT 1 - it climbs by about one per 30 frames, i.e. something destroys the mounted rider's
// click hull periodically.  has0=0 on 5 of 41 sampled frames, p0 != p2 on those 5, and 26
// distinct p0 values across the ride (one of them NULL) - while the mount's own hull pointer
// stayed single-valued all session.  So the create-on-demand shape is load-bearing AND cheap:
// it rebuilds ~2x/second instead of P3-1's unconditional 60-130x/second, and the destruction
// happens inside the engine update phase that we return from, so the hull is present again
// before render/input.  Clicking is stable (user-confirmed twice).  Prime suspect for the
// destroyer is the same carried-character update tail that drags the position back to the
// ground carry slot - beingCarriedUpdate, whose address is FORBIDDEN to hook.
static int          gP3HullCreates = 0;

// ---- P3-3 / P4-1 probe: what the combat system sees of a mounted rider -------------
// Read-only.  TASK.md's P3 plan proposes writing a combat position at 鞍座 XZ + 贴地 Y
// on the theory that a rider 6-10u up is out of 3D melee reach ⇒ 「骑手无敌」.  That is a
// hypothesis about a mechanism we have never measured, and the cheaper measurement comes
// first: getAllAttackers() is the engine's OWN record of who is swinging at the rider, so
// one fight answers whether the lever is needed at all.
//   Pre-registered readings:
//     rAtk>0 and the rider visibly takes damage  ⇒ reach already works, SKIP the ground-Y
//                                                  lever entirely and go to P4-1.
//     rAtk=0 while mAtk>0                        ⇒ the mount is a valid target and the rider
//                                                  is not.  Then compare dR3/dRxz: if the
//                                                  horizontal distance is small and only the
//                                                  3D one is large, the lever is the fix; if
//                                                  both are small, targeting is refusing the
//                                                  rider for a non-distance reason (bc=1 is
//                                                  the first suspect) and the lever is useless.
//     rAtk=0 and mAtk=0                          ⇒ the test fight never engaged; retest.
//   rTgt/cm also give P4-1 its first read (can the rider hold an attack target while its
//   CharMovement is destroyed and we halt() it every frame).  ⚠ The note that used to sit
//   here - "CombatClass is deliberately NOT dereferenced, cc= is just the pointer's
//   nullness" - is obsolete since P4-1b: the minimal shim at the top of this file reaches
//   its non-virtual members without touching the build script.  P3CMB still only reads the
//   pointer, because its job is a cheap per-frame signature; the deep read is P41B's.
static const unsigned int kCmbBaseGap = 120;  // baseline line every N frames
static const int          kCmbBudget  = 400;  // lines per ride, debug only
static int          gCmbBudget = 0;
static int          gCmbSig    = -1;   // last logged state signature; -1 = nothing logged yet

// ---- P4-1b: WHY is a carried rider not a combat participant? -----------------------
// P3-3 and P4-1a both answered the gate question from the outside and both said "not a
// participant in either direction": over 158 + 61 samples of two independent fights the
// rider had rAtk=0 / cm=0 / cmM=0 / rTgt=0 while the mount collected up to 16 attackers and
// held a target of its own.  Not our doing - the suppression in CombatAndForceDismountPass
// and the redirect in newPlayerTask_hook are both IsBigMount-gated and both rides were
// mode=1 (neck=0 in every sample).
//
// P4-1a tried to drive the rider->enemy direction by hand and came back INCONCLUSIVE, for
// three reasons that this phase removes one by one:
//   1. getAllAttackers() registers an attacker the moment it DECIDES to attack, not when it
//      arrives, so the single order that fired went out at d=971.36.  ⇒ gate on kAtkTryRange.
//   2. `if (best == gAtkAsked) return;` (one attempt per distinct target) then suppressed
//      every short-range retry once that same character stayed the nearest.  ⇒ retry on a
//      frame cooldown instead, and cycle through escalating levers.
//   3. The player had 被动 (passive) on, so the rider had no counter-attack trigger.
//      ⇒ the retest instruction is passive OFF.
//
// The real upgrade is on the READ side.  P41B asks the engine its own questions instead of
// inferring from distances, and the two that matter most are new:
//   reach= CombatClass::weaponReach()  vs  d=  : the actual melee reach number.  TASK.md's
//          P3 premise 「把骑手抬到 6-10u 高可能让双方都打不到」 has never been checked against
//          it.  d<reach with nothing happening ⇒ reach is NOT the blocker and the ground-Y
//          lever stays skipped; d>reach always ⇒ it is, and the fix is positional after all.
//   inZ= / nearZ= : isInAttackZone() / getNearestEnemyInAttackZone(), i.e. the engine's own
//          verdict on "can I hit that from here", which is the question the whole phase is
//          about.  nearZ=1 (it names OUR enemy) with rTgt=0 is the sharpest possible split:
//          the rider can see a hittable target and still refuses to hold it.
//   eGet= enemy->areYouGonnaGetMe(rider) : the enemy's own verdict on whether it will come
//          for the rider.  0 at short range IS the enemy->rider refusal, and _isBeingCarried
//          is the first suspect.
//   ordProb= rider->checkPlayerOrderForProblems(FOCUSED_MELEE_ATTACK, enemy) : if the engine
//          itself rejects a player attack order on that target, attackTarget() doing nothing
//          needs no further explanation.
//
// Write side = an escalating ladder, one rung per cooldown, logged before and after so each
// rung is attributable.  Rungs 0-1 are AI-path, 2 bypasses the AI, 3-4 inject the trigger
// the engine never delivered:
//   0  rider->attackTarget(enemy)                     - the P4-1a order, now at short range
//   1  the same + rider->reThinkCurrentAIAction()      - in case the order lands but the AI
//                                                        never re-plans (destroyed movement
//                                                        makes isDestinationReached() true
//                                                        forever, so the approach step looks
//                                                        finished and may be re-planned away)
//   2  cc->setAttackTarget(enemy)                      - direct field write, no AI involved.
//                                                        Readback splits "the AI refuses" from
//                                                        "the target slot itself won't hold".
//   3  rider->attackingYou(enemy, true, false)         - tell the RIDER it is under attack.
//                                                        This is the trigger 被动 needs and the
//                                                        one the engine failed to deliver.
//   4  enemy->attackingYou(rider, true, false)         - tell the ENEMY the rider is attacking
//                                                        it, with doAwarenessCheck=false, which
//                                                        directly tests whether the awareness
//                                                        path is what hides a carried rider.
// Pre-registered readings:
//   any rung makes rTgt stick and act>1 appears  -> a carried rider CAN fight; P4-1 passes on
//                                                   candidate (2) and the load-bearing carried
//                                                   state is never touched.  Cheapest rung wins.
//   rTgt flips then drops back every time        -> something clears it per frame; suspect the
//                                                   destroyed CharMovement first, our own
//                                                   halt() second.
//   rung 2 holds but rungs 0-1 do not            -> the AI layer is the refuser, not the combat
//                                                   class; drive the attack directly (P4-3).
//   nothing holds anywhere, ordProb/eGet explain -> only candidate (1) (restore() the movement
//     it                                            back into the physics world) can move
//                                                   this, at the cost of 「不被坐骑碰撞体推挤」.
//   act>1 in the pose DBG line                   -> the rider actually swung.  P3-3 and P4-1a
//                                                   both had ZERO act>1 frames, so that field
//                                                   is a clean baseline.
// Debug-gated on purpose: unlike every other probe in this file this one WRITES real game
// state, so a normal player never reaches it.
//
// ---- P4-1b RESULT (2026-08-29, DLL 258560 B / md5 F959E470F426399008129365D7ED7E1C) ------
// The gate PASSES on candidate (2).  User report: 「诊断已开，骑手掉血，并且攻击了敌人，只是
// 没有攻击动作。」  20 rungs all fired in one ride (tries_left 19->0) and the timeline
// attributes every flip:
//   rung 2 (cc->setAttackTarget + setAttackTargetHandle) : cTgt 0->1 same frame, held for all
//          30 later reads to end of ride.
//   rung 3 (rider->attackingYou(enemy,true,false))       : opp 0->1 same frame, lst 0->1 on
//          the next read, P3CMB rAtk 0->1 on the very next sample - all held to end of ride.
//          rAtk was 0/61 in P4-1a and is {0:37, 1:29} here.
//   rungs 0-1 (Character::attackTarget, ±reThinkCurrentAIAction) : rTgt=0 in 20/20 rung
//          readbacks and 33/33 reads.  That is verbatim the pre-registered 「rung 2 holds but
//          rungs 0-1 do not -> the AI layer is the refuser」 reading.
//   rung 4 (enemy->attackingYou(rider,...)) : no visible change; eLstR 0/33 and eTgt=2 (the
//          mount) in 33/33 reads - the enemy never retargets onto the rider.
// ⇒ candidate (1) (restore() the CharMovement back into the physics world, at the cost of
//   「不被坐骑碰撞体推挤」) stays UNOPENED, and the load-bearing carried state is untouched.
//
// The one thing still missing is the swing, and the read line named its mechanism:
//   reach=0.00 in ALL 33 reads - including the first, before any of our writes - against the
//   enemy's eReach=9.00.  With reach 0, isInAttackZone() can never be true, and indeed
//   in=0 / cm=0 / cmM=0 / inZ=0 / nearZ=0 / atkg=0.00 throughout, with ZERO act>1 pose frames.
// Distance is exonerated, which independently keeps the 「鞍座 XZ + 貼地 Y」 lever skipped:
//   reads got to d=9.60 / dxz=9.25 and P3CMB to dM3=7.79 / dR3=9.69, i.e. INSIDE the enemy's
//   own 9.00 reach, while inZ stayed 0.  And nothing refuses: ordProb=0, rGet=0, eGet=0,
//   noAgg=0 in 33/33.  The rider simply never enters combat mode.
//
// ---- P4-1c RESULT (2026-08-29, DLL 262656 B / md5 4B4C230A6535C5FC15F9186DB2389B9D) -------
// User report: 「配备了武器，骑手发动攻击并掉血，没有攻击动作，忘记下马了。」  Only 3 rungs and
// 4 reads ran (the budgets are 20/60) because the SEH shell zeroed both on the rung-3 AV.  The
// timeline below is interleaved BY FILE OFFSET - that ordering is what makes per-rung
// attribution valid at all:
//   read  d=39.92 | cma=0 tech=0 reach=0.00 in=0 cst=3/3 | wpn=0 aCW=5 aCM=0 | e cma=1 reach=9
//   RUNG 0 rAnim->setCombatMode    icm=-1 -> cma=0 tech=0 reach=0.00 cst=3 | rTgt=0 aCM=1
//   read  d=19.38 | cma=0 tech=0 reach=0.00 in=0 cst=3/3 | aCM=0       <- engine cleared aCM
//   RUNG 1 initCombatMode(focus)   icm=1  -> cma=1 tech=0 reach=0.00 cst=4 | rTgt=1 aCM=0
//   read  d=15.61 | cma=1 tech=0 reach=0.00 in=1 cst=3/3 nxt=8 | aCM=1 <- engine set aCM itself
//   RUNG 2 initCombatMode(nofocus) icm=1  -> cma=1 tech=0 reach=0.00 cst=4 | rTgt=1 aCM=0
//   read  d=19.27 | cma=1 tech=0 reach=0.00 in=1 cst=4/4 | aCM=1
//   !! AV - lever abandoned
// ⇒ initCombatMode IS the lever P4-1b lacked: combatModeActive 0->1 with _isInCombatMode()
//   agreeing, rTgt 0->1 (it was 0 in 33/33 P4-1b reads), combatState 3->4, and it HOLDS to the
//   end of the log (P3CMB 273 rows; cm/cmM/rTgt all {0:62, 1:211}).
// ⇒ the pre-registered branch that fired is 「cma=1 while reach=0.00 ⇒ the missing piece is the
//   technique」.  currentTechnique(0x150) read NULL in 4/4.
// The raw fields are trustworthy as a batch, because the enemy is a positive control: eCma=1
//   4/4, eTech=1 3/4, eReach=9.00 4/4, eCst=0 (CHOP_WEAPON) 3/4, mei=5.60/19.00, blk=1 4/4, and
//   cst(0x1F0) vs getCombatState() disagreed in 0/4.
// Four things the run settled for good:
//   ⚠ nxt(0x1F4, nextMove) is UNUSABLE and is dropped from the probe: 1818135763 in the two
//     reads before combat mode, 8 in the two after.  Not a wrong offset - uninitialised until
//     combat mode starts, which turns the engine's own write to it into evidence instead.
//   ⚠ the engine ticks the rider's combat machine by itself: cst went 4 (DECISION, ours) -> 3
//     (STARTUP_STATE) between reads, and the aCM we set was cleared and then set AGAIN by the
//     engine once cma=1.  So aCM is a SLAVE of combatModeActive, not a lever - old rung 0
//     (rAnim->setCombatMode) is deleted - and hand-ticking go(float)@0x60C4D0 is not needed.
//   ⚠ the rider holds NO weapon.  wpn=0 in 4/4 (getCurrentWeapon()==NULL) although the player
//     had equipped one, and the animation side independently reported aCW=5 = SKILL_UNARMED
//     against the enemy's eWpn=1.  Both sides agree ⇒ sheathed, or never drawn.  That is the
//     most boring explanation for tech=0/reach=0, and it becomes rung 0 below.
//   ⚠ the AV was rung 3, calculateTargetsInAttackZone()@0x608020 - the offset-ordered timeline
//     puts !! AV directly after RUNG 2.  Likeliest cause: it dereferences the NULL
//     currentTechnique.  The rung is gone and __except now disarms only the faulting rung.
// Honesty boundary: the four reads sat at d=39.92/19.38/15.61/19.27, so the rider never got
//   within 9u and inZ=0/nearZ=0 is ALSO explainable by distance.  reach=0 is the one hard,
//   distance-independent fact; this run does NOT support the stronger claim that the attack
//   zone failed to open purely because of reach.
// Still no swing: act>1 in 0 of 24579 pose frames.
// ---- P4-1d: the boring explanation first, then the direct swing --------------------------
// Probe repairs carried in from the run above: old rung 0 (rAnim->setCombatMode, a slave) and
// old rung 3 (calculateTargetsInAttackZone, the AV source) are deleted, nxt= is dropped, every
// log prefix is P41D so a new log can never be mistaken for a P41C one, and __except now marks
// gRungDead[gAtkCurRung] instead of zeroing the budget - one faulting rung no longer costs the
// other four their turn.
// The two rungs P4-1b proved keep being applied every tick, idempotently (only when the field
// does not already say what we want), and the precondition now ALSO applies
// initCombatMode(tgt, 0, false) whenever cma != 1.  So combat mode is up from the first tick
// and every rung below is tested on top of it rather than racing it.
// New read-only fields:
//   pWpn= / pCat=  does getThePreferredWeapon() (vtable 0x3C8) hand back anything, and what
//        WeaponCategory is it (Gear.h:49, @0x5C71D0).  This is what separates "the rider owns
//        no weapon" from "the rider owns one and never drew it" - the P4-1c wpn= field only
//        ever answered the second half, because getCurrentWeapon() is a DRAWN-weapon test.
//   cTech=  AnimationRequirement::_currentCombatTechnique (0x118 within animationRequirements,
//        public member) - the animation side's own technique pointer, independent of
//        CombatClass::currentTechnique(0x150).
//   chTech= / chAnim= / chInit= / chMinS=  a DRY RUN of
//        CharStats::chooseAttack(range, syntheticReach, NULL, false).
//        ⚠ the synthetic reach IS the point.  chooseAttack takes weaponReach as an ARGUMENT, so
//        asking it "which attack would you pick if reach were R" steps outside the
//        no-reach -> no-technique -> no-reach circle that P4-1c measured, instead of being
//        trapped in it.  R = the rider's own weaponReach() if > 0.01, else the enemy's, else
//        9.0f.  range = the measured distance to the target; lastAttack = NULL;
//        opponentIsStationary = false.
//        chAnim is CombatTechniqueData::animation (offset 0x0) = the human swing clip's RECORD
//        name.  That single field answers TASK.md P4-3 premise 1 (「原料不在 AnimList::attacks」)
//        outright, and feeding it through allAnims.find() + LogAnimRow prints its layer and
//        `whole` bit = premise 2.  ⚠ find() ONLY, never getAnimationData(): that one has
//        operator[] semantics and would insert a permanent NULL into the engine's own table on
//        a miss (CLAUDE.md「关键机制」).  A miss is logged as ABSENT and is itself a finding.
// The ladder, most boring first:
//   0  drawWeapon(getThePreferredWeapon(), "")     - vtable 0x3D8 / 0x3C8.  The Weapon* is
//                                                    reinterpret_cast to Item* (the whole
//                                                    Weapon:Gear:Item chain is annotated
//                                                    offset 0x0; a language upcast will not
//                                                    compile while both types are incomplete).
//   1  ((CombatClassAI*)cc)->_NV_initCombatMode(tgt, 0, true)
//                                                  - the FOCUSED variant, on the corrected
//                                                    dispatch target 0x667A60.  The unfocused
//                                                    one already runs every tick above, so this
//                                                    rung is now only about focusedTargetMode.
//   2  CcSetPtr(cc, 0x150, chTech)                 - hand the already-turning state machine the
//                                                    technique it lacks.  Only ever fed a
//                                                    pointer chooseAttack just returned.
//   3  rAnim->runCombatAnimation(chTech, 1.0f, "") - @0x5B6E80, public, no shim.  The direct
//                                                    swing that bypasses combat state, attack
//                                                    zone, reach and target lists entirely.
//                                                    Deliberately late: it is the one rung that
//                                                    proves capability rather than correctness.
//   4  cc->changeState(CHOP_WEAPON, 0.0f)          - the rung P4-1c never reached (the AV ate
//                                                    it), now run with a technique present.
// endCombatAnimation()@0x5B34E0 runs in Dismount(), unconditionally, so rung 3 can never leave
// a swing hanging on a rider that is no longer mounted.
// Pre-registered readings:
//   act>1 right after rung 0                       ⇒ the entire problem was a sheathed weapon;
//        the AI layer was never broken and P4-2/P4-3 start from a much smaller place.
//   act>1 after rung 2 (0 did nothing)             ⇒ the state machine only lacked a technique,
//        and chooseAttack is where techniques come from - that is the production path.
//   act>1 after rung 3 while 0 and 2 did nothing   ⇒ we CAN swing but have to drive it
//        ourselves; P4-3 changes from "find the clip" to "call runCombatAnimation on every
//        attack", and the engine's own combat animation dispatch stays unused.
//   all four rungs run and act stays 0             ⇒ the swing is being suppressed by the POSE.
//        If chAnim carries `whole`, it collides with kRidePose by exactly the mechanism P2-1b
//        measured (a `whole` clip pins everything else at w=0.000), and P4-3 goes straight to
//        layering / manually-controlled bones instead of clip hunting.
//   pWpn=0                                         ⇒ not even a PREFERRED weapon exists; the
//        answer is in the equipment slots, not in the combat layer, and rung 0 is a no-op.
//   chTech=0                                       ⇒ chooseAttack refuses even with a synthetic
//        reach; then the blocker is skills/encumbrance/weapon category, not reach at all.
//   chAnim resolves to ABSENT in allAnims          ⇒ attack clips are not keyed by record name
//        in the human table, and P4-3's clip lookup needs a different container after all.
//   cst/gcs disagree, or mei= is absurd            ⇒ an offset is wrong for this build; discard
//        every raw field in the line and re-derive before believing any of it.
static const int          kAtkTryBudget = 20;     // hand-issued attempts per ride (4 cycles)
static const int          kAtkReadBudget = 60;    // read lines per ride; outlives the ladder
static const unsigned int kAtkTryGap    = 75;     // frames between rungs
static const float        kAtkTryRange  = 40.0f;  // only ever order a target this close
static const int          kAtkStages    = 5;
static int          gAtkTries     = 0;
static int          gAtkReads     = 0;
static unsigned int gAtkLastFrame = 0;
static int          gAtkStage     = 0;   // next rung of the ladder
// Which rung is executing right now, so __except knows what to blame; -1 = none in flight.
// gRungDead[] is P4-1c's lesson: that run's single AV on rung 3 zeroed the whole budget and
// cost rung 4 its turn entirely.  A rung that faults is now disarmed on its own and the ladder
// keeps going past it.
static int          gAtkCurRung   = -1;
static bool         gRungDead[kAtkStages] = { false, false, false, false, false };

// ---- P4-1e: mounting SHEATHES the rider's weapon -------------------------------------
// MEASURED IN GAME (2026-08-29, user report): the weapon can only be drawn on foot, and
// the moment the rider boards it goes back onto the back.  That single observation explains
// the entire weapon column of the P4-1d read line at once - wpn=0 (getCurrentWeapon is a
// DRAWN-weapon test), pWpn=0 (getThePreferredWeapon reads NULL in that state), aCW=5
// SKILL_UNARMED - and it demotes "chooseAttack refuses to name a technique" (ch=0 43/43)
// from a mystery to a consequence: an unarmed character has no weapon technique to choose.
// ⚠️ 2026-09-02: THAT ch=0 IS NOW STALE, AND THE CONSEQUENCE ABOVE IS EXACTLY WHY.  T16 (sheath
// slot) + T18 (stance precondition) put a real weapon in the rider's hand, and a re-read of the
// trip-13 log shows chooseAttack naming real techniques in a long run - ch=1 with
// 'downward combo' / 'chop left-3' / 'bigchopv2', alongside wpn=1 and reach=10.50.  ⇒ Do NOT
// quote ch=0 43/43 as evidence that this route is closed; it is P4-3-4's foundation.  Full
// reading at the RideSwing block.
// ⚠ The other half of the same report - "no other action while mounted" - is NOT evidence
// about the weapon: kRidePose carries wholeBodyAllLayer and PoseLayerPin holds it at 1.0,
// which P2-1b-1 measured to hold every other clip at w=0.000.  Invisible is by design.
//   So this probe stops asking the combat layer anything and puts the weapon back in the
// hand first: getPrimaryWeapon() asks the equipment slots (drawn or not), drawWeapon()
// (vtable 0x3D8, CharacterHuman @0x5DB800) is the engine's own draw.
//   ⚠ Deliberately NOT every frame, and NOT unconditional.  The shape looks like the
// every-frame ragdoll kill, but drawWeapon moves equipment and animation while clearing a
// ragdoll bit is nearly free, so it is gated on getCurrentWeapon()==NULL, rationed by
// budget, and spaced by kDrawTryGap.  Which of the two shapes this actually is - a one-shot
// state transition at mount, or a per-frame overwrite - is the thing the counter decides:
// re-drawing against a per-frame writer is exactly the failure HISTORY §B is about, so it
// has to be settled by data before any repeat-forever version gets written.
// Pre-registered readings (P41E lines):
//   sw=0                     ⇒ the equipment slots do not show a weapon either; then the
//        dump line's per-section [slot:count] pairs say where it actually went.
//   post=1 on attempt 1, no attempt 2 ⇒ one-shot sheathe, and this prologue IS the fix.
//   n climbing every ~kDrawTryGap frames ⇒ per-frame re-sheathe; do NOT raise the rate,
//        find and kill the writer instead (HISTORY §B).
//   post=0 every attempt but a later P41D read shows wpn=1 ⇒ the draw is deferred through
//        an animation/queue and works anyway; judge it from the read line, not from post=.
//   post=0 and wpn=0 forever ⇒ drawWeapon is refused while mounted.  Then the suspects are
//        the carried state (_isBeingCarried / destroyed CharMovement) and the draw ANIMATION
//        being suppressed by the whole-body pose pin - i.e. it would be a pose-side problem
//        wearing an equipment-side mask, and P4-3 swallows it.
static const int          kDrawTryBudget = 12;   // drawWeapon calls per ride
static const unsigned int kDrawTryGap    = 10;   // frames between attempts; fine enough to
                                                 // catch a per-frame re-sheathe, coarse
                                                 // enough not to spam equipment changes
static int          gDrawTries     = 0;
static int          gDrawCalls     = 0;   // attempts so far this ride (the discriminator)
static int          gDrawNoWpn     = 0;   // "slots are empty" already reported this ride
static unsigned int gDrawLastFrame = 0;
static int          gInvDumped     = 0;   // one equipment-slot dump per ride

// ---- P4-3-3: one re-draw on the stance edge (2026-09-02, user ruling) -------------------------
// T13 half A shipped the sheathe suppressor and the tenth trip proved it bears load (real=57) -
// but only for a blade that was ALREADY out when the stance came up.  Trip 10's three skip
// clusters split cleanly: 213.048-214.073 was ten samples of wih=0 (that segment ran real=0
// noop=132) and the user saw an empty-handed rider, while 252.729-254.076 and 385.886-387.419
// were ten of ten wih=1 (real=41 / real=16) and the user saw 「人物在战斗中上马可以保持一会儿掏刀
// 的姿势」.  So the suppressor GUARDS a weapon and never DRAWS one; this issues exactly one draw
// on the 0 -> 1 stance edge and nothing anywhere else.
//
// Why an edge and nothing else:
//   * 「每帧重拔」 is banned outright (HISTORY §B): a write-side servo against an every-frame
//     overwriter is the one shape this project refuses.  The overwriter is already dead - that is
//     what the suppressor did - so what remains is a single state transition, which is the shape
//     §B prescribes instead.
//   * Edges are rationed by the release tail, not by a budget: kRideStanceHoldMs (1200 ms) has to
//     drain before the stance can fall to 0, so no amount of flickering produces edges faster
//     than ~1 per 1.2 s.
// Budget accounting, per the user's ruling 「补拔失败不消耗 kDrawTryBudget，失败退回梯子」:
//   * A SUCCESSFUL re-draw spends one kDrawTryBudget unit (a call really was made).  It leaves
//     gDrawCalls alone on purpose - that counter is the P41E ladder's own run numbering and
//     ridelog.py groups ladder runs by `n=1`, so borrowing it would corrupt a judgement.
//   * A REFUSED one spends nothing, so the ladder keeps all 12.  ⚠️ That ladder is
//     debugContinuous-gated (RiderArmProbe, called from CombatAndForceDismountPass), so in a
//     player build "fall back to the ladder" resolves to "fall back to the next stance edge";
//     kStanceDrawFails is what bounds the retries there.
//   * kStanceDrawFails exists because an engine that refuses this draw will refuse it on every
//     edge, and re-issuing an equipment+animation mutation forever on a refusal is still a servo,
//     just on a slower clock.  P4-1h measured 12/12 post=1, so the cap should stay unspent.
static const int kStanceDrawFails = 6;   // refused stance-edge re-draws tolerated per ride
static const int kStanceDrawLines = 8;   // per-ride P43RD line budget
// Keep a successful stance-edge draw visible for a short grace period after the threat
// briefly leaves the stance radius.  The last field test showed the engine transition
// wpn=0->1 and then wpn=1->0 about 1.8 s later, which is fast enough to look like no draw
// at all.  This is a read-side grace in the sheathe suppressor, not a redraw servo.
static const int kStanceDrawKeepMs = 3000;

// This state is per rider.  A global edge latch lets one mounted rider overwrite another
// rider's draw edge, which was reproduced when two riders exchanged mounts.
struct RideStanceDrawRuntime
{
    DWORD keepUntil;
    bool pending;
    bool prev;
    bool busy;
    int ok;
    int fail;
    int noWpn;
    int lines;
};
static boost::unordered_map<Character*, RideStanceDrawRuntime> rideStanceDrawByRider;

static RideStanceDrawRuntime* FindRideStanceDrawRuntime(Character* rider)
{
    if (!rider) return NULL;
    boost::unordered_map<Character*, RideStanceDrawRuntime>::iterator it = rideStanceDrawByRider.find(rider);
    return (it != rideStanceDrawByRider.end()) ? &it->second : NULL;
}

static RideStanceDrawRuntime& GetRideStanceDrawRuntime(Character* rider)
{
    RideStanceDrawRuntime* have = FindRideStanceDrawRuntime(rider);
    if (have) return *have;
    RideStanceDrawRuntime& fresh = rideStanceDrawByRider[rider];
    memset(&fresh, 0, sizeof(fresh));
    return fresh;
}

static void ClearRideStanceDrawRuntime(Character* rider)
{
    if (rider) rideStanceDrawByRider.erase(rider);
}

static void ClearAllRideStanceDrawRuntime()
{
    rideStanceDrawByRider.clear();
}

// P4-1e-2: the whole block above used to live inside RiderCombatLever, i.e. behind "a live
// attacker is within kAtkTryRange".  MEASURED 2026-08-30 (user report): fighting on foot and
// THEN mounting drops the existing aggro - the enemies stop treating the pair as a target -
// so that gate cannot be relied on to fire at all, and the P4-1e build produced zero P41E
// lines for a second reason on top of diagnostics being off.  "Can a mounted rider draw a
// weapon" is not a combat question anyway, so the probe now runs every frame while mounted
// under debugContinuous with no attacker requirement: mount, press Ctrl+NUM., read the log.
//   The aggro loss itself becomes data here rather than an anecdote - the state line reports
// both attacker lists alongside the weapon fields, on signature-change + periodic baseline
// (same shape as P3CMB) so "aggro was never there" and "aggro was there and vanished" are
// distinguishable, and so is "aggro survives but the weapon does not".
static const int          kArmBudget  = 60;   // state lines per ride
static const unsigned int kArmBaseGap = 120;  // baseline line every N frames
static int          gArmBudget = 0;
static int          gArmSig    = -1;

// P4-1f: WHICH LAYER puts the weapon back on the back?  P4-1e-2 measured (2026-08-30) that
// drawWeapon is NOT refused while mounted - 12/12 post=1 - and that the weapon then returns to
// the back after ~14 frames, with cma=1 for the whole ride.  So "combat ended -> auto-sheathe"
// explains the FIRST sheathe (cm was already 0 the frame after boarding) but not the repeat:
// there is a second writer, and it is bound to the mounted/carried state rather than to combat.
//   The discriminator is ORDERING between the equipment flag (getCurrentWeapon) and the
// animation requirement (animationRequirements.currentWeapon), and single-frame samples are
// enough because this probe runs every frame:
//   an intermediate frame wpn=1 aCW=5 ⇒ the ANIMATION layer let go first.  First suspect is our
//        own pinned kRidePose - "sitting chair" carries wt=0x1F, narrower than the 0x13F generic
//        rows - i.e. the engine forcing the weapon away to keep the clip legal, which would make
//        this a pose-side problem wearing an equipment-side mask (and P4-3 swallows it).
//   an intermediate frame wpn=0 aCW=0 ⇒ the EQUIPMENT layer went first and the animation only
//        follows; then the writer is sheatheWeapon and its callers.
//   a direct (1,0)->(0,5) jump ⇒ one writer does both in the same frame, and the next lever is
//        timing rather than layering.
// Logged on CHANGE only - the pair holds for ~14 frames at a time - so a two-step flip shows up
// as two lines with different f=, which is precisely the reading above.  Sampled at the TOP of
// the probe, before our own draw attempt, so each line is the state the engine left behind and
// our own 0->1 edges are attributable by the matching f= on the P41E draw line.
static const int kEdgeBudget = 60;   // weapon-state edges per ride
static int gEdgeBudget = 0;
static int gEdgeWpn    = -1;         // -1/-99 = nothing sampled yet this ride, so the first
static int gEdgeACW    = -99;        // line always prints and records the entry state

// ---- P4-1g: does a draw animation ever reach the layers? -----------------------------
// P4-1f answered its own question and killed its own leading suspect: 47/47 edges flipped
// getCurrentWeapon and animationRequirements.currentWeapon in the SAME frame (disagree=0),
// so one writer does both and the animation layer is not "letting go first".  Then the user's
// visual report reframed the whole thing: the weapon is never in the rider's hands ON SCREEN,
// not even during the ~18 frames each forced drawWeapon holds wpn=1 (12 attempts x 18 frames
// is ~2 seconds of "drawn" field state that nobody could see).  So the field write succeeds
// and the visible draw never happens - which puts the cause UPSTREAM of the revert.
//   The named mechanism for that is P2-1b-1 (measured 2026-08-29): a pose carrying
// wholeBodyAllLayer presses every other clip to w=0.000 even when the request lands in
// addList - 'crawl idle down' sat at w=0.000/ms=-1.000 for 900 frames while its t01 advanced
// normally.  We pin kRidePose ("sitting chair", whole) at weight 1.0 EVERY frame, so that
// mechanism is armed against anything the draw wants to play.
//   Readings, pre-registered:
//   a draw/sheathe/action clip present with w=0.000 (ms=-1.000) ⇒ suppression CONFIRMED, and
//        the line hands us its RECORD name (dataName), which is what P4-3 needs and what the
//        allAnims.find() rule requires before that name may ever reach getAnimationData().
//   no such clip in any layer, ever ⇒ drawing is not animation-driven here; the revert has
//        another cause and the next lever moves to the attach side (weaponInHands 0x6D8 /
//        weaponInHandsSheathLocation 0x6E0 / ATTACH_WEAPON=0).
//   the clip present with w>0 ⇒ it does play, the animation is exonerated, and the missing
//        visual is purely an attachment problem - same next lever, different reason.
// The req= header line is the other half: checkWeaponArms/checkModes score every candidate
// against animationRequirements, so weaponL/weaponR/carried/isCombatMode are the fields that
// can refuse a draw clip before it is ever added, and _currentAction names it directly if
// drawWeapon does set one.  ⚠️ YesNoMaybe has BOTH operator bool and operator ynm, so an
// (int) cast on it is ambiguous - read the .key member.
// READ-ONLY on purpose: this phase changes no behaviour, so every v1.6/v1.7 result stays
// valid.  Windowed rather than continuous because one hold is only ~18 frames, so a single
// window covers a whole draw-to-revert cycle including both endpoints; the budget is sized
// for the baseline plus two full cycles.
static const int kArmDumpFrames = 20;   // frames dumped per draw attempt (>= one 18f hold)
static const int kArmDumpBase   = 3;    // baseline frames at the start of a ride
static const int kArmDumpBudget = 44;   // total dump frames per ride (~2 full cycles)
static int gArmDumpLeft   = 0;
static int gArmDumpBudget = 0;

// ---- P4-1h -------------------------------------------------------------------------------
// P4-1g answered its pre-registered question in the sharpest of the three forms: no draw clip
// is ever SUPPRESSED because none is ever REQUESTED.  132 dump frames over 3 rides only ever
// held 'sitting chair' (ours, pinned at 1.000), 'carry me' (asked for at dw=1.000 every single
// frame and held at w=0.000/ms=-1.000 with no Ogre::AnimationState at all - the exact P2-1b-1
// signature, re-proved in the shipping pose) and the pre-mount locomotion fading out.
// _currentAction was NULL in 132/132 samples INCLUDING all 10 taken in the same frame the
// forced drawWeapon returned, while the bookkeeping that call does write is correct (aCW 5->0,
// wR 0->1).  So two independent things are broken and neither one is "the draw clip lost its
// weight":
//   (A) the visible draw is an ATTACHMENT matter that the animation path was never going to do;
//   (B) once the whole-body pose is pinned, no NEW clip can play at all.
// This phase reads the deciding field for each, and fixes the windowing flaw that put all 132
// P4-1g frames out of combat (the budget went in the first ~48 frames of every ride,
// f=14503-33764, while the fight was f=34859-38760 - overlap zero), which is why that run's
// iCM=0 carries no claim whatsoever about combat.  Still strictly READ-ONLY: no behaviour
// changes, so every v1.6/v1.7 result stays valid.
//
// (A) weaponInHands (CharacterHuman 0x6D8) is the logical "in the hands" slot, and
//     weaponInHandsSheathLocation (0x6E0) names the sheath it left.  Pre-registered binary
//     reading, taken across the forced draw:
//       non-NULL after -> the logical hand slot IS set and only the scene attachment never
//         refreshed; the next lever is the attach call itself (ATTACH_WEAPON).
//       still NULL     -> drawWeapon does not get that far while carried and the field is
//         gated behind the action that never runs; the next lever is driving attach directly.
//     Read via isHuman() (virtual, hands back the derived pointer or NULL) rather than
//     !isAnimal(), so the downcast is the engine's own answer instead of our inference.  Both
//     annotated offsets are only ever READ here - a wrong one would be a runtime AV, which is
//     precisely why the read comes before anything writes.
//
// (B) the slave sub-block: isActionSlave (0xB8), forcedSlaveLoop (0x110) and
//     attachRootToMastersBone (0xC0).  We set forcedSlaveLoop ourselves every frame, so the
//     point is not to discover it but to see whether the engine KEEPS it and whether the rider
//     is flagged an action slave - i.e. whether the freeze is our own channel (P4-3 then has
//     to give that channel up) or a property of the carried state (P4-3 then cannot).
static const int kArmDumpCmbGap    = 60;  // stride between combat dumps (~0.5s at 130fps)
static const int kArmDumpCmbBudget = 72;  // combat-only dumps per ride, on its own budget
                                          // (P4-1i: three probe modes share it, ~24 each)
static int gArmDumpCmbBudget  = 0;
static int gArmDumpCmbEntries = -1;       // layer entry count last sample (-1 = not in combat)
// ---- P4-1i: is the engine's requirement chooser reachable for a mounted rider? -------
//
// P4-1h answered both pre-registered questions and left one fork.  (A) the forced drawWeapon
// DOES set CharacterHuman::weaponInHands (12/12 wih=0->1) - the logical hand slot is fine and
// only the scene attachment never refreshes.  (B) isActionSlave=1 with
// forcedSlaveLoop='sitting chair' in 92/92: the engine keeps OUR slave channel, so the
// animation freeze is our own doing.  And the combat window finally landed: iCM=1 in 48/48
// combat samples (the combat-mode gate IS satisfied) while cTech=0, act='', wpn=0, wih=0 and
// ZERO cmb+ lines - the rider's layer set never gained or lost an entry for the whole fight
// (48 dumps x exactly 2 records: pinned 'sitting chair' + suppressed 'carry me').  moveSpeed
// sat byte-identical at 61.13 across all 92 samples spanning mount fade-in, standing, moving
// and a real fight.  That last number is the tell: the requirement chooser looks like it
// never runs for this character at all.
//
// Two candidate gates, leading to completely different P4-3 designs:
//   our own pinned pose -> P4-3 swaps the whole-body pose pin for manual bones + blend masks
//                          (LegPosePass already proves a clip cannot move a manually
//                          controlled bone) and hands the upper body back to the engine.
//   the carried state   -> P4-3 has to restore the rider's CharMovement (pickupObject
//                          destroy()ed it, which is also why moveSpeed can never update), or
//                          drive attack poses bone by bone ourselves.
//
// So this phase is a DRY RUN of the target architecture, not another read: while the rider is
// in combat, stop asserting the channel and watch whether the chooser wakes up.  Three modes
// cycle every kP41iModeGap frames so one fight covers all of them, control included:
//   0  ship behaviour, untouched (the in-fight control)
//   1  slave channel released - no forcedSlaveLoop, no runSlaveAnim/runAnimation, no
//      PoseLayerPin.  Carried-pose suppression (carried=false + stopAnimation("carry me")) is
//      KEPT, so a chooser that does run is free to pick something combat-like instead of
//      being shoved straight back into ANIM_CARRIED.
//   2  fully hands off - mode 1 plus giving up that suppression too.  It needs its own slot
//      because "chooser runs but picks the carried pose" and "chooser never runs" are
//      indistinguishable from mode 1 alone.
//
// Pre-registered reading: if cTech/act go non-NULL, spd unfreezes, or cmb+ lines appear in
// modes 1/2 but not in mode 0, the blocker is our pose pin and the target architecture is
// reachable.  If all three modes look like mode 0, the gate is the carried state / destroyed
// movement and P4-3 goes the movement-restore route.
//
// Safety: gated on debugContinuous AND rider-in-combat, so a normal session and every
// out-of-combat frame stay byte-identical to the shipping path.  SyncRiderNode and the
// per-frame ragdoll kill are untouched - the rider stays in the saddle; only the pose is let
// go, and it recovers by itself when mode 0 comes round (runSlaveAnim/runAnimation fade it
// back in, and LegPosePass re-arms through its own weight>=0.5 gate - the same gate that
// releases the legs while the pose is down, so no leg can be left stuck at 45 degrees).
//
// P4-1i VERDICT (2026-08-30): the release worked and NOTHING woke up - but the probe had a
// hole, which is why P4-1j exists.  Combat was continuous for ~8400 frames (30 mode slots,
// iCM=1 in all 72 samples; the player's enemy-kiting counts as combat).  All twelve sampled
// fields were byte-identical across all three modes: aCW=5 iCM=1 cTech=0 act='' wpn=0 wih=0
// slave=1 fsl='sitting chair' cat=0 spd=48.04 lays=5 idle=0/0/0, and spd held that same value
// in the 44 out-of-combat samples too (48.04 this session vs 61.13 last => a snapshot taken at
// mount time, frozen thereafter).  The release itself was real: 28/28 mode-1 dumps had an
// EMPTY addList, LegPosePass released 9 times and re-armed 10, and the player saw three
// distinct states matching the three modes one-for-one - bind pose (mode 1, i.e. no clip at
// all), the vanilla carry pose (mode 2, the carry system's force-play simply not being kicked
// out), and our seat (mode 0).  The 9 cmb+ lines all sit within 1-25 frames of a mode
// boundary => our own switching, zero engine-side events.
//
// The hole: forcedSlaveLoop and isActionSlave are STICKY.  Modes 1/2 stopped writing them but
// never cleared them, so slave=1 fsl='sitting chair' stood the whole time.  Handing the layers
// back is therefore not the same as handing the CHARACTER back, and "the engine won't take the
// upper body" currently has two possible causes.  P4-1j zeroes both fields every frame in
// modes 1/2 to split them - see the else branch in the animUpdate pre-pass.
//
// One conclusion is already firm and it constrains P4-3: handing the upper body back is not a
// thing the engine will do on its own.  With nothing asserted it renders the BIND pose, not an
// idle or a guard stance, so whatever P4-3 becomes, something has to drive the upper body
// actively.
// A+C build (2026-08-30) rotated three slots for one fight: 0 = ship, 1 = route A ('guard 1h'
// pinned at 1.0, the stance owning the whole torso and LegPosePass owning the legs), 2 = route
// C (ride pose AND stance both at 0.5).  ⚠️ THE ROTATION IS RETIRED - it answered its question
// and route A won.  Verdict and the numbers behind it:
//   * The player saw A as a real sword guard with the straddle intact; C kept half the seat's
//     own shaping but the arms did not read as "on guard".
//   * C's weak arms are STRUCTURAL, not a pin failure: mode 2's 30 samples read w/dw/acw/ms
//     all exactly 0.500, and pw/pms 0.500 on 27 of them, i.e. the pin held BOTH clips at half
//     weight on the engine and the render side.  There is no weight to tune that does not
//     simply turn C into A, so do not revisit C by nudging 0.5.
//   * Mode 1 held w=1.000 on 27/30 with pw=-1.000 throughout (the ride pose genuinely leaves
//     the layers, as designed), and kept= was 1.0000 on all 66 leg samples, so masking every
//     weighted contributor covers a stance host too.
// P4-1M therefore replaces the rotation with RideCombatStance() (defined next to
// MountCombatEligible, which needs SeatInfo and so cannot be reached from here): route A is
// what a mounted fighter looks like, in normal play, with no debugContinuous gate.
static bool gRideStanceOn   = false;  // last processed rider's decision; read by the DBG tag

// Total entries over every layer's addList+removeList.  A clip entering or leaving the rider's
// layers moves this number, and that event is the one worth spending combat budget on: a
// stride on its own would very likely step straight over a swing.  Same bounds guards as the
// dump below - this walks the same dangling-prone lektors.
static unsigned int CountRiderLayerEntries(AnimationClass* rAnim)
{
    if (!rAnim || !rAnim->layer.valid()) return 0;
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return 0;
    unsigned int total = 0;
    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay) continue;
        for (int pass = 0; pass < 2; ++pass)
        {
            lektor<AnimationClassBase::SingleAnimation*>& lst =
                pass ? lay->removeList : lay->addList;
            if (!lst.valid()) continue;
            unsigned int n = lst.size();
            if (n > 64) continue;
            total += n;
        }
    }
    return total;
}

// weaponInHands plus the sheath it names, or NULL/"?" when the rider is not a CharacterHuman.
// The returned pointer aliases the engine's std::string, so a caller that keeps it across an
// engine call has to copy it first (the draw probe does).
static Weapon* RiderWeaponInHands(Character* rider, const char** sheathOut)
{
    CharacterHuman* h = rider ? rider->isHuman() : NULL;
    if (!h) { if (sheathOut) *sheathOut = "?"; return NULL; }
    if (sheathOut) *sheathOut = h->weaponInHandsSheathLocation.c_str();
    return h->weaponInHands;
}

// drawWeapon's SECOND argument is the sheath location the blade is leaving, and it is not
// decoration: drawWeapon hands it straight to leaveSheathEquipped (RE_NOTES §18.12), which
// whitelists exactly "hip" and "back" and returns having done NOTHING for any other string - the
// empty one we used to pass included.  That skipped block is the three missing steps of every
// mounted draw: detachItem(location) takes the blade's mesh OFF the back, the empty scabbard mesh
// goes on in its place, and weaponInHandsSheathLocation (0x6E0) records where it came from.  Which
// is exactly the observed failure - hand slot occupied in the data layer, blade still on the back
// on screen, sh='' on every edge (RE_NOTES §18.11.1).
// ⚠️ Never pass "back2".  leaveSheathEquipped derives that itself from the item's own field; the
// whitelist rejects the 5-character string outright, which would put us right back here.
// ⚠️ Keep the value at 15 characters or fewer.  drawWeapon clears the string in place on the way
// out, and for a heap-allocated one it takes the `0xf < _Myres` branch into operator_delete - the
// game's allocator on our buffer.  "hip"/"back" live in MSVC's internal buffer, so it is dead code.
static std::string RideSheathSlotFor(Character* rider)
{
    // The back family is checked first and wins ties: it is where a primary weapon rides, and it is
    // what the engine's own draw used for the one ride that renders correctly (sh='back').  "hip"
    // is taken only when both back slots are provably empty, so a shield or a pack sitting on the
    // hip can never be detached out from under the player by a wrong guess.
    AppearanceBase* app = rider ? rider->getAppearance() : NULL;
    if (app)
    {
        void* onBack  = (void*)app->getAttachedEntity(std::string("back"));
        void* onBack2 = (void*)app->getAttachedEntity(std::string("back2"));
        void* onHip   = (void*)app->getAttachedEntity(std::string("hip"));
        if (!onBack && !onBack2 && onHip) return std::string("hip");
    }
    return std::string("back");
}

// P4-3 step 1's sheatheWeapon naming probe (a hook on CharacterHuman::sheatheWeapon, a per-ride
// caller-site table, and ShDescribeAddr's "<module>+0x<rva>" formatter) lived HERE and is gone as
// of the probe-free build.  Its verdict: the second writer is Character::_ragdollMode (real=16,
// median gap 22 frames), with Character::_carryMode(on=true) secondary - both named in
// RE_NOTES §18.10, reasoning in TASK.md P4-3 step 1, code in `git show 61872dc` (the commit that
// shipped it) or `git show 7838deb:RidingPlugin.cpp` (last source with the full diagnostics set).
// (!) 每帧重拔 STAYS BANNED (HISTORY §B).  What P4-3-2 does instead is suppress the sheathe
// itself - see RideSheatheSuppressed, right after RideCombatStance - so nothing here re-draws.

// P4-3 step 2's attachItem naming probe (two hooks on the AppearanceBase::attachItem overloads,
// a per-ride site table, and the "hands" slot filter) lived HERE and is gone as of the probe-free
// build.  Its verdict, in one line: the hand slot IS refreshed while the rider is carried
// (hands=12 on one ride, other=0, slot0='hands'), and yet the blade is on the back on screen =>
// whoever takes it away does NOT go through either hooked overload.  Details in TASK.md P4-3
// step 2, addresses in RE_NOTES §18.10, code in `git show 07f3588`.

// P4-3 step 2's THIRD probe (a hook on AppearanceBase::detachItem(const std::string&) with a
// per-ride caller-site table = P43DT, a once-per-frame poll of getAttachedEntity("hands") = P43HD,
// and ShDescribeAddr's "<module>+0x<rva>" formatter) lived HERE and is gone as of this build.
// Both its verdicts are closed:
//   * exactly ONE site ever carried a slot='hands' detach (kenshi_x64.exe+0x5CBDF8, 903 calls on
//     trip 15), and that site is attachItem's own clear-before-write positive control (RE_NOTES
//     §18.11) => there is NO third-party writer taking the blade off the rider's hand.  T15.
//   * the sheath-slot fix took: slots back/back2/hip only start appearing once RideSheathSlotFor()
//     passes a real name as drawWeapon's 2nd argument, i.e. leaveSheathEquipped's previously
//     skipped detach now fires (RE_NOTES §18.12, runtime confirmation §18.12.1).  T16.
// ⚠️ RECOVERY IS NOT `git show` - this probe was never committed.  The only source that still holds
// it is the snapshot D:\KenshiModDev\RidingPlugin_src_E83DB50D.cpp (the source of the T18-passing
// DLL, md5 E83DB50D17267C7C2DFA67A4BB144D3C).  ShDescribeAddr alone is also in `git show 07f3588`.
// ⚠️ NAMING ONLY was the gate and still is: neither verdict may turn into "so we re-attach it
// ourselves every frame" - that is write-side compensation for an absolute overwrite, HISTORY.md
// §B's servo road, and 每帧重拔 stays banned.

static void DumpRiderAnimLayers(Character* rider, AnimationClass* rAnim, const char* tag)
{
    if (!rAnim) return;

    const AnimationRequirement& rq = rAnim->animationRequirements;
    const char* sh = "?";
    Weapon* wih = RiderWeaponInHands(rider, &sh);
    char hd[512];
    _snprintf_s(hd, 512, _TRUNCATE,
        "Riding: P41G req %s aCW=%d wL=%d wR=%d carried=%d iCM=%d cTech=%d act='%s' "
        "idle=%d/%d/%d wpn=%d wih=%d sh='%s' slave=%d fsl='%s' arb='%s' cat=%d spd=%.2f "
        "lays=%u f=%u",
        tag, (int)rq.currentWeapon,
        rq.weaponL ? 1 : 0, rq.weaponR ? 1 : 0, rq.carried ? 1 : 0,
        (int)rq.isCombatMode.key,
        rq._currentCombatTechnique ? 1 : 0,
        rq._currentAction ? rq._currentAction->dataName.c_str() : "",
        rq.idle ? 1 : 0, rq.legsIdle ? 1 : 0, rq.upperIdle ? 1 : 0,
        rider->getCurrentWeapon() ? 1 : 0,
        wih ? 1 : 0, sh,
        rq.isActionSlave ? 1 : 0,
        rq.forcedSlaveLoop ? rq.forcedSlaveLoop->dataName.c_str() : "",
        rq.attachRootToMastersBone.c_str(),
        (int)rq.currentAnimCategory, rq.moveSpeed,
        rAnim->layer.valid() ? rAnim->layer.size() : 0u,
        gP3Frames);
    DebugLog(std::string(hd));

    if (!rAnim->layer.valid()) return;
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return;                 // garbage/dangling guard, as PoseLayerPin

    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay) continue;
        for (int pass = 0; pass < 2; ++pass)
        {
            lektor<AnimationClassBase::SingleAnimation*>& lst =
                pass ? lay->removeList : lay->addList;
            if (!lst.valid()) continue;
            unsigned int n = lst.size();
            if (n > 64) continue;
            for (unsigned int ai = 0; ai < n; ++ai)
            {
                AnimationClassBase::SingleAnimation* sa = lst[ai];
                if (!sa) continue;
                const AnimationData* ad = sa->animationData;
                char pl[480];
                _snprintf_s(pl, 480, _TRUNCATE,
                    "Riding:   P41G L%u %c%u rec='%s' clip='%s' w=%.3f dw=%.3f ms=%.3f "
                    "t01=%.2f lay=%d cat=%d wt=0x%X hw=%d/%d dcm=%d act=%d whole=%d/%d "
                    "flags=%s%s%s%s",
                    li, pass ? 'R' : 'A', ai,
                    ad ? ad->dataName.c_str() : "?",
                    sa->animName.c_str(),
                    sa->weight, sa->desiredWeight,
                    sa->mainState ? sa->mainState->getWeight() : -1.0f,
                    sa->currentFrameTime01,
                    ad ? (int)ad->layername : -1,
                    ad ? (int)ad->category  : -1,
                    ad ? ad->weaponTypeFlags : 0u,
                    ad ? (int)ad->holdingWeaponL.key : -1,
                    ad ? (int)ad->holdingWeaponR.key : -1,
                    ad ? (int)ad->isCombatMode.key   : -1,
                    ad ? (ad->isAction ? 1 : 0) : -1,
                    ad ? (ad->wholeBodyAllLayer ? 1 : 0) : -1,
                    sa->isAWholeBodyAction ? 1 : 0,
                    sa->looped      ? "loop," : "",
                    sa->autoRemove  ? "auto," : "",
                    sa->stillWanted ? "want," : "",
                    (sa->usingRightArm || sa->usingLeftArm) ? "arm" : "");
                DebugLog(std::string(pl));
            }
        }
    }
}

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
//  2026-08-20; column 9 "posture" joined it 2026-08-29 when the standing posture was
//  deleted; the column positions survive read-and-ignore / write-0 for file compat.)
struct SpeciesTuning
{
    int           seatMode;
    bool          forceSit;      // re-assert the sitting pose every frame
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
    SpeciesTuning() : seatMode(SEAT_MIDPOINT), forceSit(true), lateral(0.0f), offset(Ogre::Vector3::ZERO), anchor(Ogre::Vector3::ZERO), base(0.0f), home(Ogre::Vector3::ZERO), homeLateral(0.0f), refScale(0.0f) {}
    SpeciesTuning(int m, const Ogre::Vector3& o) : seatMode(m), forceSit(true), lateral(0.0f), offset(o), anchor(Ogre::Vector3::ZERO), base(0.0f), home(Ogre::Vector3::ZERO), homeLateral(0.0f), refScale(0.0f) {}
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

// Drop every piece of tracked ride state at once.  A world-reset wipe cannot call
// the engine because its pointers are stale.  A live SEH wipe is different: its
// Characters may still own our manual bones and blend-mask entries, so it first
// attempts a narrow, per-rider handback before discarding the ownership maps.
static void ResetRideSwingWindow(Character* rider);
static void ClearRideStanceRuntime(Character* rider);
static void RideSwingArmRelease(AnimationClass* rAnim);
static void ClearAllRideMaskRuntime();
static void ClearAllRidePoseRuntime();
static void LiveRideHandbackAll();

static void WipeAllRideState(const char* why, bool liveObjects)
{
    DebugLog(std::string("Riding: WIPE all ride state (") + why + ")");
    if (liveObjects)
        LiveRideHandbackAll();
    riderToMount.clear();
    mountToRider.clear();
    mountSeat.clear();
    mountAnchor.clear();
    mountCap.clear();
    dbgNodeWritten.clear();
    dbgMoveWritten.clear();
    p3Probe.clear();
    mountBaseVOffset.clear();
    mountSmoothOrient.clear();
    mountHeadingPos.clear();
    mountHeadingDir.clear();
    debugLastPos.clear();
    mountLastPos.clear();
    pendingMount.clear();
    ClearAllRideMaskRuntime();
    ClearAllRidePoseRuntime();
    // P4-3-3: the only latch in this file that an unwind could leave stuck.  mainLoop_hook's
    // __except lands here, and gStanceDrawBusy is set across a drawWeapon call - so an AV inside
    // that call would unwind past its own reset and leave the sheathe suppressor switched off for
    // the rest of the session.  Fail-open is the safe direction, but not silently and not forever.
    ClearAllRideStanceDrawRuntime();
    // The active host/target can still hold rider-local pointers.  The live path handed
    // engine ownership back above; the world-reset path deliberately only forgets them.
    ResetRideSwingWindow(NULL);
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
// 🆕 2026-09-09 ground-combat seat drop.  With the lower body back on the ride pose in
// combat (user: 固定下半身为原本的骑乘姿势), hips are already sit-height - keep 0 unless a
// later eyeball pass says the standing upper still reads as floating.  Was -2.5.
static const float kRideCombatSeatDropY = 0.0f;
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
    int   posture;               // DEAD column (2026-08-29): mirrors cfg column 9, which
                                 // became a dead field when the standing posture was
                                 // deleted.  Read by nobody.  Kept - all 21 rows already
                                 // hold 0 - so the table body stays byte-identical and
                                 // tools\gen_default_seats.py / apply_default_seats.py
                                 // need no change (and kDefaultsVersion needs no bump).
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
        // columns 4, 6-8 and 9 of the line format are obsolete: column 4 "mount" is a dead
        // legacy field (every ride uses native carry since 2026-08-20), columns 6-8 are
        // roll/pitch/yaw orientation tunes - facing is always the mount's travel direction
        // now - and column 9 "posture" died with the standing posture (2026-08-29, TASK.md
        // P2-0).  Parse them into dummies only so existing cfg files keep loading with the
        // same column layout.
        int mountIg = 0;
        float rollIg = 0.0f, pitchIg = 0.0f, yawIg = 0.0f;
        int postureIg = 0;
        float lateral = 0.0f;
        int n = sscanf(val.c_str(), "%d,%f,%f,%d,%d,%f,%f,%f,%d,%f", &mode, &up, &fwd, &mountIg, &sit, &rollIg, &pitchIg, &yawIg, &postureIg, &lateral);
        if (n >= 3)
        {
            if (mode < SEAT_EXACT) mode = SEAT_EXACT;
            // mode 4 was the rigid-body seat, removed 2026-08-27.  Its numbers were offsets
            // from the mount's rigid body rather than from a bone, so a stale row like that
            // must not be reinterpreted as a rear/neck anchor - drop it to the neutral mode
            // and let the built-in default / the tuning keys take it from there.
            if (mode > SEAT_REAR) mode = SEAT_MIDPOINT;
            SpeciesTuning st(mode, Ogre::Vector3(fwd, up, 0.0f));
            st.forceSit = (sit != 0);
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
    fprintf(f, "# <species>=<mode>,<up>,<forward>,<mount>,<sit>,<roll>,<pitch>,<yaw>,<posture>,<lateral>  mode 0=exact 1=midpoint 2=neck 3=rear  sit 0=off 1=on  lateral=side offset\n");
    fprintf(f, "# columns 4, 6-8 and 9 are OBSOLETE legacy fields (mount method / roll-pitch-yaw / posture) - parsed-and-ignored, always written as 0\n");
    fprintf(f, "# columns 11-14 = persisted seat constants (anchor x/y/z + bob baseline), auto-captured - do not hand-edit\n");
    fprintf(f, "# columns 15-17 = the declared zero/home (up, forward, lateral): Numpad9 returns here, Ctrl+Numpad9 sets it to the current seat\n");
    fprintf(f, "# column 18 = the animal size columns 2/3/10/14 were tuned at; the seat is rescaled for bigger/smaller individuals of the same species (0 = unknown, no rescaling)\n");
    fprintf(f, "# rows whose name has the shape <number>-<datafile>.base/.mod are keyed by RACE, not species: they seat any animal of that race that has no row of its own (and are the only rows that work on a non-Chinese install)\n");
    fprintf(f, "# this file only OVERRIDES the seats built into RidingPlugin.dll - delete it to go back to the shipped defaults\n");
    fprintf(f, "defaults=%d\n", kDefaultsVersion);
    boost::unordered_map<std::string, SpeciesTuning>::iterator it = speciesTuning.begin();
    for (; it != speciesTuning.end(); ++it)
        // the two bare 0 arguments are the dead columns 4 (mount method) and 9 (posture);
        // they hold their position in the line so old cfg files keep parsing.
        fprintf(f, "%s=%d,%.2f,%.2f,%d,%d,0.0,0.0,0.0,%d,%.2f,%.3f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%.3f\n", it->first.c_str(), it->second.seatMode,
                it->second.offset.y, it->second.offset.x, 0,
                it->second.forceSit ? 1 : 0, 0, it->second.lateral,
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

// Existence-checked animation lookup, defined further down next to the P1 table dump (it
// needs the EngineAnimMap typedef).  Forward-declared here because BuildSeatInfo is the
// earliest caller: getAnimationData() must never be handed a name that may be absent.
static AnimationData* FindAnimData(AnimationClass* rAnim, const char* name);

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
    //   * a name row is a hand-written OVERRIDE for one animal.  ⚠️ The plugin creates one only
    //     in ONE case, see below: tuning writes back through info.tuneKey, which IS the race key
    //     whenever the race layer won, so tuning any member re-tunes its whole race.  Otherwise
    //     the only way to get a name row is to type it into riding.cfg.  That turned out to be
    //     enough - the four big crabs (56089's 1.5-2.25 bracket) were supposed to be the case
    //     that needed one, since their old hand tune was on the EXACT anchor while the shipped
    //     race row is the small crabs' NECK anchor and the size law only converts within one
    //     anchor frame; in game (2026-08-28) a single +0.27 nudge on the shared row seated the
    //     whole 0.94-2.24 span correctly, big crabs and small ones alike.
    // Whichever layer wins is recorded in info.tuneKey, and ⚠️ every write-back path must use
    // tuneKey rather than species - otherwise the first anchor capture on a race-served mount
    // silently forks a name row for it and the race layer stops covering it.
    //
    // ⚠️ THE ONE CASE THAT USED TO FORK A NAME ROW (found 2026-08-29, FIXED 2026-08-30, TASK.md
    // X-2): tuneKey defaults to species and was only replaced by the race key when speciesTuning
    // ALREADY HELD a row for that race.  So an animal whose race is not among the 21 shipped rows -
    // a modded species, or any mount where getRaceKey() came back empty - kept a name-keyed tuneKey,
    // and its first anchor capture wrote a row keyed on the LOCALIZED NAME.  That is exactly the
    // fragility the race layer exists to avoid (a different language install, or the player renaming
    // the animal, and the row stops matching).  The live example found in this machine's riding.cfg
    // was `Brooke` (home columns all 0 = never seeded from the table, refScale 1.033 = adopted on
    // first tune).  Now: when NEITHER layer has a row, tuneKey falls back to the race key so the new
    // capture lands as a RACE row, which is the covering layer for every other member of that race.
    // Two things deliberately unchanged:
    //   * a hand-written name override still wins - it is found by the FIRST lookup, before this.
    //   * an empty raceKey still leaves tuneKey on the name.  There is nothing better to key on, and
    //     a row is still better than losing the capture; that path is also the one to suspect if a
    //     name row ever appears again (check the mount log's `race=` field for `()`).
    info.tuneKey = info.species;
    boost::unordered_map<std::string, SpeciesTuning>::iterator tit = speciesTuning.find(info.species);
    if (tit == speciesTuning.end() && !info.raceKey.empty())
    {
        tit = speciesTuning.find(info.raceKey);
        // Race key whether or not the row exists yet: if it does, it is serving us; if it does
        // not, it is where this mount's first capture belongs.
        info.tuneKey = info.raceKey;
    }
    if (tit != speciesTuning.end())
    {
        info.seatMode = tit->second.seatMode;
        info.userOffset = tit->second.offset;
        info.forceSit = tit->second.forceSit;
        info.lateral = tit->second.lateral;
        info.homeOffset = tit->second.home;
        info.homeLateral = tit->second.homeLateral;
        info.refScale = tit->second.refScale;
        info.sizeScale = SeatSizeRatio(info.refScale, info.liveScale);
    }

    // pack_beast family (Garru / Pack Beast / Dead Pack Beast) share the "beast walk"
    // animation, which the carry system suppresses.  Detect it from the animation
    // data so we can force the walk back on while ridden.
    //
    // ⚠️ MUST go through FindAnimData(), never getAnimationData() (TASK.md X-1, fixed
    // 2026-08-30).  getAnimationData() is operator[]: on a miss it INSERTS a NULL value
    // under that key into the engine's own allAnims - and here the miss is the common
    // case, since every non-pack-beast species reaches this line.  So the old direct call
    // dropped one permanent null pointer into the animal list per ride of any other
    // animal, in a container the ENGINE iterates.  find() first, resolve only what is
    // already there; the boolean result is identical for the pack beasts themselves.
    if (FindAnimData(mountAnim, "beast walk"))
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

// ---------------------------------------------------------------------------
// P4-0: the mounted-combat BODY SIZE gate.  "Only small animals" (user ruling)
// means the rider swings only when the mount is at most 1.5x the ceiling group
// (bison/cow, max=10.3) => kCombatSizeMax = 15.5.
//
// The metric is max(torsoLen, getRadius()) - ONE ceiling applied to the larger
// of two numbers, NOT two constants each clamping its own number.  Both metrics
// have a measured blind spot and the blind spots DO NOT OVERLAP:
//   * torsoLen is the horizontal front<->rear bone distance, so it collapses on
//     species with VERTICALLY STACKED spines (giant crab reads 3.8, smaller than
//     a goat).  That is exactly the fwSrc=3 species list.
//   * getRadius() is one hull axis * nsc, hand-authored in FCS, and it MIS-ORDERS:
//     dogs read 9.8/9.9 while the far wider bison reads 6.7.
// max() = "if either number says big, it IS big", and that conservative direction
// is the same direction as the default-deny.  The original draft's pair of
// DIFFERENT ceilings (torso 12 / rad 9) is what inverted dog-vs-bison; it never
// reached code.
//
// Measured live anchors (torso / rad / max -> verdict):
//   goat 3.2/3.9/3.9 in | pack bull 7.6/4.9/7.6 in | garru 9.5/7.0/9.5 in
//   pet dog 8.4/9.8/9.8 in | settler's pup 8.5/9.9/9.9 in | bison 10.3/6.7/10.3 in
//   cow 10.3/6.7/10.3 in | bonedog wolf 12.5/14.5/14.5 in (user ruling)
//   giant crab 3.8/22/22 out | swamp turtle -/34.9 out | leviathan 85.4/-/ out
// The max column is monotone with visual size with NO inversion, and the only
// real gap in the series is 14.5 -> 22.  15.5 sits inside it: the wolf clears by
// 1.0u, the nearest excluded species by 6.5u.
//
// WARNING: nsc CANNOT be the gate.  It is a multiplier against each race's OWN
// base mesh (machine spider nsc 1.0 and leviathan nsc 1.0 differ by orders of
// magnitude), so it is not comparable across races.  torso and rad are already
// multiplied by nsc => they are absolute world units, which is what a gate needs.
//
// WARNING: this is NOT IsBigMount().  That one asks "does the mount swing instead
// of the rider" (mode 2||3); the garru is mode 2 yet comfortably inside the size
// gate.  Two separate questions - never merge them.
//
// Residual risk = "big animal + degenerate torso + small hull" all at once.  The
// only unmeasured candidate is King/Curled-One (race 65260-Newwworld.mod, mode 2,
// on the vertical-spine list so its torso must degenerate, rad unknown).  Handling
// = the mount log prints size=/torso=/rad= plus a RECORD-ONLY h= (anchor bone
// height above the mount's movement position; goat measured 6.99u) so a misjudged
// species is visible the first time it is ridden.
static const float kCombatSizeMax = 15.5f;

static float MountCombatSize(Character* mount, const SeatInfo& seat)
{
    float rad = mount ? mount->getRadius() : 0.0f;
    if (!(rad > 0.0f) || rad > 1000.0f) rad = 0.0f;   // same distrust as RideLegAbductDeg
    float t = (seat.torsoLen > 0.0f) ? seat.torsoLen : 0.0f;
    return (t > rad) ? t : rad;
}

// 🆕 P4-5 (2026-09-06, user ruling 「人物在坐坐垫的时候不参与战斗」).  Forward declaration only: the
// definition needs kRideLegStyleRows and the geometric fallback, both of which live with the leg
// pose ~3500 lines below, while the COMBAT gate is the first thing that has to know the answer.
static bool RideLegPoseIsCushion(Character* mount, const SeatInfo& seat);

// 🆕 P2-4 Option A: same forward-declaration game as above.  The pose NAME is per-style now
// (cushion mounts pin 'sitting idle' as the host clip so the hands/torso read as the 原版 sitting
// fidget; straddle and chair stay on 'sitting chair'), and the sites that need it run both before
// and after the real definition - Mount() at ~7100, the diagnostic at ~4660.
static const char* RidePoseNameForSeat(Character* mount, const SeatInfo& seat);

// TWO independent denials, answering different questions - ⛔ never merge them (same warning as
// IsBigMount above):
//   * size    - "is the animal small enough that the RIDER's own weapon reaches" (P4-0).
//   * 坐坐垫  - "is this seat a fighting seat at all".  Cross-legged with one shin folded over the
//     other is not, so P4-5 turns the WHOLE combat route off for that style: RideStanceRaw calls
//     this first, so no stance means no twist, no auto-draw (P43RD), no sheathe suppression
//     (P43SUP passes straight through) and no swing window; and CombatAndForceDismountPass's
//     !riderFights branch additionally ends the rider's combat mode every frame - the same passive
//     tier the oversized mounts have shipped in since P4-2.
// ⚠️ 利维坦 was ALREADY denied by size (recorded torso 83.7 >> 15.5).  What P4-5 adds is 螃蟹
// (trip 28: size 9.5), 沼泽乌龟 (8.8), 巨型沼泽速龙 and 陆地蝙蝠 - every one of them read elig=1
// before today, so the crab and the turtle CHANGE BEHAVIOUR and are what the next trip has to look
// at (they now walk the passive branch, which only leviathan-class mounts had ever exercised).
// ⚠️ The MOUNT is untouched: it keeps its own engine AI and still fights back (user-confirmed).
static bool MountCombatEligible(Character* mount, const SeatInfo& seat)
{
    if (RideLegPoseIsCushion(mount, seat)) return false;   // 🆕 P4-5, see the block above
    float s = MountCombatSize(mount, seat);
    return s > 0.0f && s <= kCombatSizeMax;   // a failed read (0) DENIES
}

// Nearest live threat, or NULL; *distOut = HORIZONTAL distance to it (y ignored, because a
// mounted rider is several units above everything he fights).  Declared attack target first -
// that is the one a swing is for - then the nearest live entry in getAllAttackers(), because
// P4-1b measured that list fills on the DECISION to attack, so it is the earlier signal rather
// than a fallback of last resort.
// ⚠️ isDown() disqualifies exactly as much as isDead() does: a knocked-out enemy ENDS the fight
// but STAYS the engine's attack target.  P4-1M shipped with that check on the attacker scan and
// missing on the attack target, and that single omission is most of why the stance never let go
// (measured: target 122u behind the mount, isDead() == false, twist saturated at 60 deg).
// 🆕 T18: THE MOUNT'S BOOKS ARE CONSULTED AFTER THE RIDER'S.  In a player build the rider has
// neither term - mounting drops aggro and the enemy keeps swinging at the mount (P4-1b: rTgt 0
// of 33, eTgt=2 33 of 33) - so a rider-only search returns NULL for an entire fight and the
// stance's third term can never come true.  The mount's books are the read-only evidence that
// a fight is on, and trip 13's log shows them filling 3.3 s BEFORE the diagnostics lever was
// ever pressed: 200.497 mTgt=1 mAtk=1, climbing to mAtk=6 by 202.263, all of it with cm=0 and
// rTgt=0.  ⚠️ Distances are still measured FROM THE RIDER, so kRideThreatDist and the twist
// angle keep exactly their old meaning; only the candidate list grew.
static void RideThreatConsider(Character* c, Character* rider, Character* mount,
                               const Ogre::Vector3& rp, Character** best, float* bestSq)
{
    // The mount is excluded as well as the rider: a player who orders an attack on their own
    // mount is technically in combat, but holding a battle stance aimed at the animal you are
    // sitting on is nonsense, and RideTwistTargetDeg would try to face straight down.
    if (!c || c == rider || c == mount) return;
    if (c->isDown() || c->isDead()) return;
    Ogre::Vector3 dv = c->getPosition() - rp;
    float dsq = dv.x * dv.x + dv.z * dv.z;
    if (!*best || dsq < *bestSq) { *best = c; *bestSq = dsq; }
}

static Character* RideNearestThreat(Character* rider, Character* mount, float* distOut)
{
    if (distOut) *distOut = -1.0f;
    if (!rider) return NULL;

    Ogre::Vector3 rp = rider->getPosition();
    Character* best  = NULL;
    float bestSq     = 0.0f;

    // Tier order is a PRIORITY order, not merely a fallback chain: a declared attack target wins
    // even when somebody else is nearer, because that is the one a swing would be for.  Inside
    // the two attacker tiers the nearest live entry wins.
    RideThreatConsider(rider->getAttackTarget().getCharacter(),
                       rider, mount, rp, &best, &bestSq);
    if (!best)
    {
        lektor<hand> atk;
        rider->getAllAttackers(atk);
        for (lektor<hand>::iterator ait = atk.begin(); ait != atk.end(); ++ait)
            RideThreatConsider(ait->getCharacter(), rider, mount, rp, &best, &bestSq);
    }
    if (!best && mount)
        RideThreatConsider(mount->getAttackTarget().getCharacter(),
                           rider, mount, rp, &best, &bestSq);
    if (!best && mount)
    {
        lektor<hand> mAtk;
        mount->getAllAttackers(mAtk);
        for (lektor<hand>::iterator mit = mAtk.begin(); mit != mAtk.end(); ++mit)
            RideThreatConsider(mit->getCharacter(), rider, mount, rp, &best, &bestSq);
    }
    if (best && distOut) *distOut = Ogre::Math::Sqrt(bestSq);
    return best;
}

// Normal-play combat promotion for a small eligible mount.  The diagnostics lever below is a
// deliberately exhaustive reverse-engineering ladder, but it must not be the only code path that
// gives a mounted rider a combat relationship: with two riders, one rider can otherwise remain a
// passive carried character until Ctrl+Numpad. happens to run the ladder.  Keep this path narrow and
// idempotent.  It only installs the same three preconditions already proven by P4-1d (target,
// attacker-list membership, combat mode); it never runs the ladder, never changes big-mount policy,
// and never clears combat mode.
static void RideCombatPromoteImpl(Character* rider, Character* mount)
{
    if (!rider || !mount || rider->isDown() || rider->isDead()) return;

    float d = -1.0f;
    Character* threat = RideNearestThreat(rider, mount, &d);
    if (!threat || d < 0.0f || d > kAtkTryRange) return;

    CombatClass* rcc = rider->getCombatClass();
    if (!rcc) return;

    int targetSet = 0;
    int attackerSet = 0;
    int combatSet = 0;

    Character* current = rcc->_getAttackTarget().getCharacter();
    if (current != threat)
    {
        rcc->setAttackTarget(threat);
        rcc->setAttackTargetHandle(threat);
        targetSet = 1;
    }
    if (!rcc->isInAttackerListH(threat))
    {
        rider->attackingYou(threat, true, false);
        attackerSet = 1;
    }

    // Do not construct `hand` in the DLL.  Copy the engine-owned handle, exactly as the
    // diagnostic ladder does, then call the non-virtual AI entry only when the rider is not
    // already in combat mode.
    hand targetHandle = rcc->_getAttackTarget();
    if (CcBool(rcc, 0x130) != 1 && targetHandle.getCharacter())
    {
        reinterpret_cast<CombatClassAI*>(rcc)->_NV_initCombatMode(targetHandle, 0, false);
        combatSet = 1;
    }

    if (targetSet || attackerSet || combatSet)
    {
        char b[256];
        _snprintf_s(b, 256, _TRUNCATE,
            "Riding: P43AUTO rider=%p mount=%p tgt=%p d=%.2f target=%d attacker=%d combat=%d f=%u",
            (void*)rider, (void*)mount, (void*)threat, d,
            targetSet, attackerSet, combatSet, gP3Frames);
        DebugLog(std::string(b));
    }
}

static void RideCombatPromote(Character* rider, Character* mount)
{
    __try { RideCombatPromoteImpl(rider, mount); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { DebugLog("Riding: P43AUTO access violation - promotion abandoned"); }
}

// ⚠️ P4-1M measured that isInCombatMode(true, true) DOES NOT DROP when a mounted fight ends: the
// log holds STANCE 1 for ~8700 frames (≈67 s) after the last blow and never returns to 0, which
// is exactly the "出战斗后没有回到纯坐姿" the player reported.  So the engine flag cannot be the
// only term - we need our own "is anybody still fighting me" test.  ⚠️ The answer is NOT to clear
// combat mode ourselves: the user ruled that out ("不需要无条件清，他自己会掉") and it would also
// take the weapon out of the rider's hands, which is the one thing route A depends on.
//   kRideThreatDist - a live threat must be at least this close.  Generous on purpose (a mounted
//     fighter circles and re-approaches; getAllAttackers() itself registers out to ~1000u, so
//     with no distance term at all the list is effectively "has ever fought anyone").
//   kRideStanceHoldMs - tail after the last raw frame, so the stance does not flicker in the
//     gaps between swings or while the target is briefly unresolvable.
// ⚠️ This tail used to be kRideStanceHoldFrames = 150 DECREMENTED ONCE PER FRAME, i.e. its real
// length was 150 ÷ framerate: ≈1.15 s on the 130 fps machine it was tuned on, but 2.5 s at 60 fps
// and ~5 s at 30 fps - and dropping into that range mid-fight is completely normal.  It is a
// wall-clock budget now (GetTickCount delta, see RideCombatStance), so every machine gets the same
// 1.2 s.  ⚠️ The acceptance test does NOT become "it went back within N seconds" - it stays
// "STANCE was observed going 1 -> 0" (TASK.md P4-1N / TEST_REQUIRED.md T1); a seconds threshold
// just re-imports the framerate dependency into the judging instead of the code.
static const float kRideThreatDist   = 60.0f;
static const int   kRideStanceHoldMs = 1200;
// One frame may never contribute more than this to draining the tail.  A load hitch or an alt-tab
// hands us a multi-second delta; without the clamp one such frame empties the whole budget and the
// stance snaps back to the seat pose mid-fight.  A genuinely long stall therefore takes a few
// frames to drain, which is invisible (no frames were rendered during the stall anyway).
static const int   kRideStanceStepMaxMs = 250;

// The stance tail is mutable runtime state, so it must be keyed by rider just like the swing
// transaction below.  Animation updates interleave riders; a single hold/tick pair makes rider B
// reset or drain rider A's tail and produces the observed same-frame STANCE 1/0 flicker.
struct RideStanceRuntime
{
    Character* rider;
    int holdMs;
    DWORD tick;
    int last;       // diagnostics-only edge latch; -1 means not logged yet
    bool on;
};

static boost::unordered_map<Character*, RideStanceRuntime> rideStanceByRider;

static RideStanceRuntime* FindRideStanceRuntime(Character* rider)
{
    if (!rider) return NULL;
    boost::unordered_map<Character*, RideStanceRuntime>::iterator it =
        rideStanceByRider.find(rider);
    return (it != rideStanceByRider.end()) ? &it->second : NULL;
}

static RideStanceRuntime& GetRideStanceRuntime(Character* rider)
{
    boost::unordered_map<Character*, RideStanceRuntime>::iterator it =
        rideStanceByRider.find(rider);
    if (it == rideStanceByRider.end())
    {
        RideStanceRuntime& fresh = rideStanceByRider[rider];
        memset(&fresh, 0, sizeof(fresh));
        fresh.rider = rider;
        fresh.last = -1;
        return fresh;
    }
    RideStanceRuntime& rt = it->second;
    if (rt.rider != rider)
    {
        memset(&rt, 0, sizeof(rt));
        rt.rider = rider;
        rt.last = -1;
    }
    return rt;
}

static void ClearRideStanceRuntime(Character* rider)
{
    if (!rider) rideStanceByRider.clear();
    else       rideStanceByRider.erase(rider);
}

static int RideStanceHoldMs(Character* rider)
{
    RideStanceRuntime* rt = FindRideStanceRuntime(rider);
    return rt ? rt->holdMs : 0;
}

// The raw, stateless predicate.  True => the rider gives the pose channel back and holds a combat
// stance instead, with the straddle carried entirely by LegPosePass's manual bones.  ⚠️ NO
// debugContinuous gate anywhere in this path: this is what a mounted fighter looks like now, so
// toggling diagnostics must not change the posture (same discipline as LegPosePass).  All three
// terms are load-bearing:
//   * MountCombatEligible - a big mount swings for itself (IsBigMount, mode 2||3 attack
//     redirect) and its rider has no business waving a sword from up there.  Note this is the
//     SIZE gate, not IsBigMount: the garru is mode 2 yet inside the size gate.
//   * RideFightIsOn - "is anybody actually fighting".  Out of combat there is no weapon in hand
//     and the seat pose is the right answer.  P4-1i measured that kiting counts as combat, so
//     this does not drop out the moment the rider stops swinging.  🆕 T18 widened it away from
//     the rider's own isInCombatMode(true, true), which a player build NEVER reaches; the whole
//     rationale is on RideFightIsOn itself.
//   * a live threat within kRideThreatDist - the term that actually ENDS the stance, see above.
// 🆕 T18 - term ② of the stance, widened.  ⚠️ EVERY QUESTION HERE IS A READ; this function must
// never write engine state.  That is the whole difference between it and RiderCombatLever, which
// does write and is therefore debugContinuous-gated (:593).
//
// Why it had to be widened (2026-09-02 trip 14 / T17, RE_NOTES §17.7): the old term was the
// rider's own isInCombatMode(true, true), and in a player build that is PERMANENTLY FALSE.
// Mounting drops the rider's aggro (:7596) and the enemy goes on attacking the mount, never
// retargeting onto the rider (P4-1b: eTgt=2, 33 of 33).  Every STANCE 1 in every earlier log was
// produced by pressing the diagnostics lever first - trip 13 has the causality inside 13 ms
// (203.828 rung=0 writes cm 0->1, 203.841 the first stance edge of the trip).  With diagnostics
// off: two fights, stance never armed, P43RD drawn=0 fail=0 nowpn=0 twice.
//
// ⚠️ The mount's isInCombatMode is UNMEASURED - no log has ever carried it (P3CMB's cmM= is
// isInCombatMode(true, FALSE) on the RIDER, :7278, not the mount).  So it is an OR term here, not
// the fix: the two terms carrying the weight are the mount's attack target and attacker list,
// which trip 13 recorded as true from 200.497 onward, 3.3 s before any lever press.
// ⚠️ This does NOT widen who may hold a stance: MountCombatEligible still runs first, and
// kRideThreatDist still decides when the stance ends.  getAllAttackers registers out to ~1000u
// and does not clear (P4-1M), which is exactly why the distance term exists.
static bool RideFightIsOn(Character* rider, Character* mount)
{
    if (!rider || !mount) return false;
    if (rider->isInCombatMode(true, true)) return true;   // the original term, kept
    if (mount->isInCombatMode(true, true)) return true;   // unmeasured, free to ask
    Character* mt = mount->getAttackTarget().getCharacter();
    if (mt && mt != rider && mt != mount && !mt->isDead() && !mt->isDown()) return true;
    lektor<hand> mAtk;
    mount->getAllAttackers(mAtk);
    for (lektor<hand>::iterator it = mAtk.begin(); it != mAtk.end(); ++it)
    {
        Character* a = it->getCharacter();
        if (a && a != rider && a != mount && !a->isDown() && !a->isDead()) return true;
    }
    return false;
}

static bool RideStanceRaw(Character* rider, Character* mount, const SeatInfo& seat)
{
    if (!rider || !mount) return false;
    if (!MountCombatEligible(mount, seat)) return false;   // size gate AND 🆕 P4-5 坐坐垫
    if (!RideFightIsOn(rider, mount))      return false;
    float d = -1.0f;
    if (!RideNearestThreat(rider, mount, &d)) return false;
    return (d >= 0.0f && d <= kRideThreatDist);
}

// `advance` = "you are the once-per-frame caller".  HaltAndForceSitPass passes true (it is also
// the last writer before render); the animUpdate pre-pass passes false and only reads, so the two
// passes can never disagree about the stance inside one frame.
static bool RideCombatStance(Character* rider, Character* mount, const SeatInfo& seat, bool advance)
{
    bool raw = RideStanceRaw(rider, mount, seat);
    RideStanceRuntime* readRt = FindRideStanceRuntime(rider);
    if (!advance) return raw || (readRt && readRt->holdMs > 0);
    if (!rider) return false;

    RideStanceRuntime& rt = GetRideStanceRuntime(rider);

    // Wall-clock step.  DWORD subtraction is wrap-safe, so the 49.7-day GetTickCount rollover
    // needs no special case (the one tick that lands exactly on 0 just contributes nothing).
    DWORD now = GetTickCount();
    int   dt  = 0;
    if (rt.tick != 0)
    {
        DWORD elapsed = now - rt.tick;
        if (elapsed > (DWORD)kRideStanceStepMaxMs) elapsed = (DWORD)kRideStanceStepMaxMs;
        dt = (int)elapsed;
    }
    rt.tick = now;
    // ⚠️ A paused game must not drain the tail: same discipline as the capture state machine and
    // ServicePendingMounts (a pause freezes exactly what this budget is measuring).  The tick is
    // still refreshed above, so unpausing resumes with a normal-sized delta instead of the whole
    // pause duration.
    if (ou && ou->isPaused()) dt = 0;

    if (raw) rt.holdMs = kRideStanceHoldMs;
    else if (rt.holdMs > 0)
    {
        rt.holdMs -= dt;
        if (rt.holdMs < 0) rt.holdMs = 0;
    }
    rt.on = raw || rt.holdMs > 0;
    return rt.on;
}

// ---- P4-3-4: the swing itself, played by the ENGINE'S own combat dispatch --------------------
// P4-3 step 3 closed with 「缺的不是机制而是触发条件」 and the experimental 3 s/1 s window was
// deleted (its full shape is archived in HISTORY.md §U).  Two candidates were left alive: hang the
// swing on chooseAttack's rhythm, or on an attack event.  A 2026-09-02 re-read of the trip-13 log
// (offline, no game time) answered it, and the answer is better than the archived window:
//
//   * chooseAttack DOES name a technique now.  `P41D read` carries ch=1 in a long run with real
//     technique names - 'downward combo' (init/minS 10.00/10.00), 'chop left-3' (0.00/-10.00),
//     'bigchopv2' (25.00/20.00).  ⚠️ The "chooseAttack refuses to name a technique (ch=0 43/43)"
//     recorded at the P4-1e block is UNARMED-ERA evidence - that block says so itself: an unarmed
//     character has no weapon technique to choose.  T16 (sheath slot) + T18 (stance precondition)
//     put a real weapon in the hand, which retired the premise, and nobody re-read the log until
//     now.
//   * rung 3 - runCombatAnimation(chTech, 1.0f, "") @0x5B6E80 - really executed, 7 times, with a
//     non-NULL technique and ZERO access violations (rung=3 at 206.338 / 210.175 / 213.864 /
//     217.541 / 379.445 / 405.453 ..., same-frame readback tech=1 wpn=1 reach=10.50 cma=1 aCM=1
//     cst=3/4).  inZ=0 nearZ=0 on every one of them ⇒ the engine will never DISPATCH a swing for a
//     carried rider on its own, but it will PLAY one when asked.
//   * 🔑 runCombatAnimation takes a CombatTechniqueData*, NOT a clip name.  That walks straight
//     past the dead end RE_NOTES §19 documents (43 of 44 technique clip names have no
//     AnimationData record at all, so FindAnimData/ClipPin can only ever play the 'mid blow'
//     stand-in).  This route plays the swing the engine itself picked.
//
// ⚠️ THE ONE NECESSARY CHANGE, and it is also measured: after rung 3 the guard clip was still
// `play=1 w=1.000` and only its render-side mainState moved, 1.000 -> 0.909 (P41K @206.280 vs
// @206.600).  Our per-frame runAnimation(guard) + ClipPin(guard, 1.0, true) held it, and ClipPin's
// global `target + others <= 1.02f` door then refused the guard's own setWeight.  So the swing WAS
// playing and the stance was sitting on top of it.  ⇒ while a swing is in flight the guard must not
// be asserted at all - the same trick §U used to get past that door, applied for the same reason.
// ⚠️ TWO sites assert it, not one: HaltAndForceSitPass (render side true) and the animUpdate
// pre-pass (render side false).  §U could leave the second alone because OUR pinned one-shot got
// the guard EVICTED from the play list (trip 10); an engine-driven swing does not evict it, so this
// time both sites have to stand down.
//
// ⚠️ Deliberately NOT the archived window's unconditional 3 s timer: 「不管眼前这一架在干什么都
// 照点」 is exactly why that code was not shipping posture.  The trigger is the technique's own
// opinion of the range it is usable at (initialDistance 0x38 / minDistanceVsStatic 0x3C) plus a
// minimum interval, so a swing only happens when the engine would consider one usable.
// ⚠️ TRIP 18 (2026-09-02, diagnostics OFF, log RE_Kenshi_log_trip18_AF771D99_diagoff.txt) proved the
// MECHANISM and then handed back a purely cosmetic defect, measured to the digit:
//   * It works.  swing=17 over 4 rides, guardoff=5126 frames, and ZERO `LEGPOSE released grace=`
//     lines - so 'mid blow' really does become the skeleton's host and trip 17's stand-up is gone.
//   * It does not look like ONE swing.  「有点像右手给左手割腕」.  Two independent prog= readings
//     agree on why: prog 0 -> 0.517 took 2.502 s and 0.518 -> 0.900 took 1.828 s, i.e. a rate of
//     ~0.207/s ⇒ 'mid blow' is a ~4.8 s clip and a 2500 ms window shows ~52% of it.
//   * 🔑 AND the second window RESUMED at 0.518 where the first stopped at 0.517.  The
//     SingleAnimation entry survives between windows with its currentFrameTime01 intact (frozen
//     while unrequested), so every window after the first shows an arbitrary MIDDLE slice - and a
//     window that happens to open on a leftover prog >= kRideSwingDoneProg closes on its very next
//     frame, having played nothing at all.
// ⇒ Three knobs, all three turned here, none of them touching the trigger or the host guarantee:
//   1) restart the clip at each open (RideSwingRestart below) - fixes both the middle slice and the
//      instant-close;
//   2) play it faster, so a full pass is a swing-length event rather than a 4.8 s one;
//   3) a cap with enough headroom that the CLIP closes the window, not the clock.
// 🆕 2026-09-09: Len/Win/Gap live together just below (enemy-cadence cut).
// 🆕 T27 - THE WINDOW'S HOST IS THE GUARD AGAIN, and the window therefore closes on our own clock.
// Trip 24 (the T26 aimed arc) got the eyeball to 「有点劈砍的意思了」 and named what is left:
// 「角色的右手总是想找左手因为原版就是双手劈砍的，所以把动作带崩了」.  That reading is structurally
// right even though the mechanism it names cannot be the one at work - the right hand's POSITION is
// ours now (dot= mean 0.9920 over 36 samples, and every sample below 0.99 is a read-lag frame), so no
// clip is pulling that hand anywhere.  What 'mid blow' still owned was everything the arc does not:
// the LEFT arm, the right WRIST, the spine.  It is one of the six `blow` records, all of them
// 「击倒类重击」 carrying whole+action+reloc (doc.md :245) = a two-handed committed strike, so its
// left-arm track reaches across for a grip that is no longer there and its wrist track rolls the
// blade to match.  ⇒ the window stops swapping the host at all: 'guard 1h' (UPPER, loop, weapon-class
// bit 0x04 = ONE-HANDED, no whole/reloc - doc.md :248) stays pinned straight through, and our arc cuts
// on top of it.  Three things come free: no root motion inside the window, a wrist that keeps a
// one-handed attitude for the whole stroke, and a swing length that stops being a restatement of
// 'mid blow's own.  What it costs is the torso whip - if the verdict is 「太僵」 the answer is to
// author the spine (P4-1M's twist is already ours, :6295), NOT to bring a ground record back.
// 🆕 2026-09-09 user: mounted cadence felt slower than on-foot enemies.  Vanilla technique
// clips measure 1.07-2.83 s (T22/T24); enemies on a ~1.2-1.5 s cycle with attackSpeed.  The
// old 2000/1650 pair was a fixed ~2 s rest.  New chain keeps the invariants and sits nearer
// the enemy clock: open-to-open ~0.8 s at aspd=1.0, floored at Win+50.
// 🆕 2026-09-09 user: 攻击改到 0.8 秒左右.  Window/hit/gap move together so the
// invariant chain and the mid-window hit beat both hold.
// 🆕 P4-6z (2026-09-12 user: 「骑砍挥砍的时候动作太快了，所以胳膊扭曲。调成和地面战斗
// 时一样的速度，攻击间隔提升到1.2秒」).  The 700 ms window compressed a 1.07-2.83 s
// ground technique into 700 ms (~1.5-4x) and the arms thrashed.  Play the clip at 1.0x
// (Drive uses the clip's own length as the phase timeline); the window clock only
// decides how long we keep showing it.  Cadence at aspd=1.0 is 1.2 s open-to-open.
static const int   kRideSwingHitMs   = 600;   // mid-arc resolution beat, fixed on the window clock
static const int   kRideSwingWinMs    = 1000; // window FLOOR / pre-measurement fallback
static const int   kRideSwingLenMs    = 1100; // ⛔ superseded by kRideSwingLenMaxMs (P4-6af-3);
                                              // kept named because older remarks quote it
static const int   kRideSwingMinGapMs = 1200; // open-to-open floor at attackSpeed=1.0
// 🆕 P4-6af-3 (2026-09-12 user: 「在马上的时候技法动作没做完也可能是刚做完就回收了就回收到握姿了，
// 导致最后一段播不出来，看起来像刀没完全砍出去。等动作做完再让他保持一点时间」).
// The window is no longer a fixed 1000 ms: it is THE CLIP'S OWN LENGTH (at ground speed, measured
// into rt.fitMs by RideSwingDrive) plus a follow-through hold.  A fixed window cut every ground
// technique (1.07-2.83 s) off mid-arc - `bigchopv2` only ever showed ~35% of its stroke, so the
// blade never finished travelling - and RideSwingBlend's tail ramp then handed the last 120 ms
// straight back to the host's grip, which is the 「刚做完就回收」 half of the report.
//   * kRideSwingHoldMs is the follow-through: the clip is frozen on its LAST frame (Drive clamps
//     the phase at 1.0) and held at full weight, then the fade-down ramp runs inside the hold -
//     so the stroke's end pose is visible for (hold - fade) before the grip returns.
//   * kRideSwingLenMaxMs is the open-tick escape cap, now sized for the longest ground technique
//     (2.833 s) plus a hold plus margin.  A garbage length reading can never extend a window.
static const int   kRideSwingHoldMs   = 250;  // follow-through hold after the clip's own end
static const int   kRideSwingLenMaxMs = 4000; // hard cap on ANY window (escape safety)
// ⚠️ INVARIANT (P4-6af-3): window = clamp(fit + kRideSwingHoldMs, kRideSwingWinMs, kRideSwingLenMaxMs)
//    and kRideSwingMinGapMs is a FLOOR on open-to-open, not a ceiling - a long clip legitimately
//    outlives the gap, and the window itself is what keeps the next attempt from opening.
// (ArcMs only feeds the retired authored arm path; do not use it to close the window.)
static const float kRideSwingSpeed    = 2.5f;  // ⛔ RETIRED BY T27 (nothing one-shot is pinned any
                                               // more; kept because RideSwingRestart still compiles
                                               // and §17.11's 「sp=2.50 是真旋钮」 is a measured fact
                                               // worth not losing).  SingleAnimation::speed for a
                                               // pinned one-shot.
                                               // 4.84 s * 0.90 / 2.5 = 1.74 s per swing, which is
                                               // what a heavy chop should take.  ⚠️ UNVERIFIED knob:
                                               // nothing in this DLL has ever passed a speed other
                                               // than 1.0f.  If the engine ignores it the trip still
                                               // gains - knob 1 alone turns a random middle slice
                                               // into a swing that starts at the start - and the
                                               // close line's sp=/t01=/olen= read-back says which.
static const float kRideSwingDoneProg = 0.90f; // ⛔ RETIRED BY T27: the host is a LOOP now, so its
                                               // progress cycles and closing on it would end windows
                                               // at random.  kRideSwingWinMs closes them.  (Kept so
                                               // the trip-21 reasoning below stays readable.)
// The gap is measured from the OPEN attempt, scaled by CharStats::getAttackSpeed below.
// Floor = Win+50 so positive attackSpeed can shorten the 1200 ms base cadence while the
// fastest rider still gets a short rest after the window.
static const int   kRideSwingMinGapFloorMs = kRideSwingWinMs + 50;
static const float kRideSwingAttackSpeedMin = 0.50f;
static const float kRideSwingAttackSpeedMax = 1.20f;
static const int   kRideSwingLines    = 24;    // per-ride log budget (open + close lines share it)
static const int   kRideSwingHandLines = 8;    // per-ride budget for the P4-6m handtake rows
// 🆕 T22 - driving the TECHNIQUE's own Ogre state (RideSwingDrive).
// P4-6z: TechMs is NO LONGER a compress target.  Drive uses the clip's own length as the
// phase timeline (ground speed, 1.0x).  Kept as a named ceiling so older comments and
// ridelog fit= notes stay readable; nothing in the shipping path reads this value.
static const int   kRideSwingTechMs   = 3000;
static const float kRideSwingTechW    = 1.0f;
// P4-6l (2026-09-12 user: 「骑砍时胳膊转筋」): the technique state used to hit weight 1.0 on
// the open frame and 0.0 on the close frame, while guard's upper-body mask flipped 0<->1 in
// the same instant - the arm TELEPORTED between the guard pose and the technique's frame 0
// twice per window.  This is the cross-fade both edges now share: technique weight =
// w, guard upper-body mask entry = 1 - w, w ramps 0 -> 1 across the first kRideSwingFadeMs
// and 1 -> 0 across the last kRideSwingFadeMs of the live window.  Hit at 600 ms sits well
// inside the flat middle (120..880 ms at fade=120, window=1000).
static const int   kRideSwingFadeMs   = 120;
// Mounted combat has one authored visual state: guard 1h plus the baked right-arm slash.  The
// engine's picked technique is retained only as the subject of hitByMeleeAttack; it must not make
// the visible state stop and start at a different distance for every technique.  25 matches the
// proven mounted envelope (the previous successful bigchop gate) and covers a target beside a
// mount whose rider-to-target distance is larger than the weapon's on-foot reach.
static const float kRideSwingEngageDist = 25.0f;
// 🆕 P4-6h (trip 37): a gate with init=2.00 is ABOVE this threshold, so the open decision used to
// treat it as a real "engage from 2u" range.  Mounted structural distance is ~7..25 (the mount's
// body holds the enemy off), so d>2 skipped every attempt while the stance stayed armed - the
// user-visible shape 「骑牛只有架势，没有动作」.  The mount-side fix is the reach floor below,
// not raising this constant: a 2u opinion is legitimate for a footman and useless on a saddle.
static const int   kRideSwingSkipLines = 8;    // per-ride budget for P43SW skip rows (own pool)

// 🆕 T23 - the trip-20 fix.  The drive WORKS and the composition does not.  Trip 20: drv>0 on every
// ride that swung, en=1 on 11/11 closes, w=1.000, arc=100% - and the eyeball says the state very much
// reaches the skeleton: 「把刀从右手往胸口收，然后刀飘到头顶，整个人缩成一团瞬移到坐骑前面。动作
// 幅度很大，但是毫无意义」.  That is the OPPOSITE of the pre-registered kill criterion (§17.12: en=1
// w=1.000 t≈len with nothing visible ⇒ route dead) ⇒ the route lives, it is simply unconstrained.
// Two unconstrained things, one per symptom:
//
//   * 「瞬移到坐骑前面」= the technique is a GROUND clip and its root/pelvis tracks step the whole
//     skeleton forward.  Kenshi's own AnimationData records carry the whole/reloc bits for exactly
//     that, and a record-less technique clip has nobody to apply them (§19) ⇒ raw root motion.
//   * 「整个人缩成一团」= two full-body clips driving the same bones at 1.0 each.  'mid blow' is
//     pinned at 1.0 as the host and the technique is now enabled at 1.0 with NO blend mask at all -
//     LegMaskApply only reaches states that own a SingleAnimation entry, and a record-less clip has
//     none - so every spine/arm bone receives both, which is not a blend of two poses in any mode.
//
// The fix is a complementary split, built out of the blend-mask machinery that already carries the
// straddle (createBlendMask/setBlendMaskEntry on an AnimationState, in this DLL since P2-1b, leak
// discipline in §21.2).  The TECHNIQUE gets root/pelvis/legs/feet zeroed - it may have the torso,
// never the seat.  The HOST gets its upper body zeroed for the duration of the window - it keeps the
// legs, which is the only thing it is load-bearing for (P4-1i / §17.9: assert nothing down there and
// the engine renders BIND and the rider stands up on the mount's back).  Fingers are in NEITHER list
// on purpose: the host keeps the grip closed around the weapon.
//
// ⚠️ Both tables are resolved BY NAME at the same site as the leg bones and every handle is logged
// once per DLL load.  A name that is not in this skeleton reads has=0 and is skipped - no handle is
// ever assumed (the 30-bone rider makes 0/1 look like root/pelvis, and that is exactly the kind of
// guess this project has been burned by).
// P4-6B (2026-09-12 user): during mounted combat the LOWER BODY is fully released - the
// character plays the ground technique end-to-end.  Only the seat NODE keeps height.
// Hold table is unused (Drive creates no mask).  Do not reintroduce straddle/pelvis snap
// on the combat path without a new user ruling.
static const char* kRideSwingHoldBones[] = {
    "Bip01",
    "Bip01 L Thigh", "Bip01 R Thigh",
    "Bip01 L Calf",  "Bip01 R Calf",
    "Bip01 L Foot",  "Bip01 R Foot"
};
static const int   kRideSwingHoldCount = 7;
// 🆕 P4-6af (2026-09-12 user: 「骑砍时进入战斗状态还用的是坐姿，导致破坏后续挥砍出招动作，
// 改为进入战斗时上半身就采用战斗待机姿势」).  Combat stance 'guard 1h' is the STANCE base host
// again: the sit pose owns the body out of combat, the combat idle stance owns the upper body
// while the stance is armed (both resolved through RideCombatHost).  Lower body is unchanged -
// LegPosePass keeps writing straddle / cushion / sit-height pelvis through the whole ride.
// The technique Ogre state owns the whole UPPER BODY for the window - spine, both arms AND the
// entire wield chain (both hands + both weapon props).  That is exactly what the free list below
// hands over, and it is the only shape that can look like ground combat.
// Free list = what the BASE HOST yields during a window (must cover every bone the technique
// writes, or two writers share a bone).  Masking by blend entry is per-clip (LegMaskApply walks
// every weighted clip), so one table serves either host - the combat idle or the sit pose.
// 🆕 P4-6af-1/2, both from the same user report thread, and BOTH earlier shapes were half-wrong:
//   * P4-6ad had the HANDS here but the two PROPS out of the table - the props were then written
//     by the host clip AND by the technique at 1.0 each, i.e. a blend of two orientations, which
//     is the 「挥砍时刀竖直/自转」 the user reported;
//   * P4-6af-1 took the hands back OUT (host kept them).  Single-writer, and the blade stopped
//     spinning - but the hand then held the HOST'S GUARD GRIP for the whole stroke: the palm
//     started DOWN and only rolled up as the technique-driven arm carried it there, which is the
//     「一开始手心向下、快完成时才向上，幅度小又扭曲」 the user reported next.  Ground combat
//     starts palm-UP and holds it, because there the CLIP owns that hand.
// ⇒ the whole wield chain belongs to the technique (single writer, the ground answer).  The weapon
// hangs off `Bip01 Prop2`, a CHILD of `Bip01 R Hand` (RE_NOTES §19), and every clip animates all
// four: `bigchopv2` spans Prop2 108.1 deg / Hand 70.0 deg, `chop down static` 64.3 / 48.5,
// `downward combo` 51.5 / 103.4 - so a missing bone here is a visible defect, not a detail.
// ⛔ Do not "fix" a hand/prop symptom by moving bones between this table and the host: the host's
// grip is a GUARD stance, the clip's is the stroke, and a bone in neither table is written twice.
static const char* kRideSwingFreeBones[] = {
    "Bip01 Spine", "Bip01 Spine1", "Bip01 Spine2",
    "Bip01 Neck", "Bip01 Head",
    "Bip01 L Clavicle", "Bip01 R Clavicle",
    "Bip01 L UpperArm", "Bip01 R UpperArm",
    "Bip01 L Forearm", "Bip01 R Forearm",
    "Bip01 L Hand", "Bip01 R Hand",
    "Bip01 Prop1", "Bip01 Prop2"
};
static const int   kRideSwingFreeCount = 15;
// Legacy names kept so the runtime POD and release paths still compile.  P4-6af-2: the hands are
// in kRideSwingFreeBones like every other bone the technique drives, and this stays as the
// hand-custody table the retired hand writer used to key off.
static const char* kRideWristBones[] = {
    "Bip01 L Hand", "Bip01 R Hand"
};
static const int   kRideWristCount = 2;
static const char* kRideTechKeepGuardBones[] = {
    "Bip01 Neck", "Bip01 Head",
    "Bip01 L Clavicle",
    "Bip01 L UpperArm", "Bip01 L Forearm"
};
static const int   kRideTechKeepGuardCount = 5;
// The old single-rider swing diagnostics were removed once all live fields moved into
// RideCombatRuntime.  Keeping dead globals here would make future edits accidentally restore
// cross-rider state.

// ---- 🆕 P4-6: the swing's hit resolution ("刀找人") ------------------------------------
// v1.7's swing is visual only - §17.8's verdict was that the engine will PLAY a combat
// animation for a carried rider but never DISPATCH one for him.  This block adds the
// dispatch ourselves: one Character::hitByMeleeAttack per window, timed to the arc's own
// clock, with the engine resolving everything after it (dodge chance, block, body part,
// wound, stumble, sound - the whole vanilla pipeline).  HIT_MISSED back IS "the enemy
// dodged", which is the behaviour the user explicitly wants preserved.
// Hit time is kRideSwingHitMs = 600, defined with the window chain above.
static const int   kRideSwingHitLines = 24;   // per-ride budget for the P43ST lines, its own
                                               // pool (NOT shared with gRideSwingLines): a
                                               // skipped-resolution line is diagnostic for a
                                               // different question than the window lines.
static const int   kRideAtkAnimCount = 1;
static const char* kP41kGuardAnim = "guard 1h";
// Resolved for the P41K diagnostic line and for leftover stopAnimation on dismount only.
// ⛔ NEVER pin/request as a window host: the whole `blow` family carries a `stumbles`
// body-part map and IS the vanilla hit reaction (gamedata --refs).  Pinning it made the
// rider play "I was hit" with no attacker (P4-6S).
static const char* kP41kBlowAnim  = "mid blow";
// Window hosts: ANIMATION records that have a male_skeleton track AND no `stumbles` refs.
// Full-table scan 2026-09-12: after dropping the six blow-family hit reactions the only
// remaining non-stumble action clip with a track is `chop down static` (UPPER, not whole,
// loop, Attack sfx).  Technique clips (chop left / bigchopv2 / ...) still have no record.
static const char* const kRideAtkAnim[kRideAtkAnimCount] = {
    "chop down static"
};
// P4-6V: window VISUAL is the engine technique's own Ogre::AnimationState (RideSwingDrive).
// pick.name is CombatTechniqueData::animation (clip name: bigchopv2 / chop left-3 / ...).
// Those tracks have no ANIMATION record (§19) but DO own a state (§17.10, 17/17 found).
// Host stays guard 1h so the straddle never goes hostless (trip 17).  Technique state gets
// root/pelvis/legs masked out (hold bones in Drive); guard's upper body is masked out via
// swingFree so the two do not both write the torso at 1.0 (trip 22's 「缩成一团」).
// Explicit ownership for the in-flight mounted combat transaction.  This POD answers the lifecycle
// question: which rider/mount/target/technique currently owns the one allowed resolution.  Keep it
// POD-only for the VS2010 toolchain and for callers protected by the existing SEH shells.
enum RideCombatPhase
{
    RIDE_COMBAT_IDLE = 0,
    RIDE_COMBAT_SWINGING,
    RIDE_COMBAT_HIT_RESOLVED,
    RIDE_COMBAT_RECOVERY
};

// Per-rider transaction and authored-arm state.  A single global window was safe only
// while exactly one rider could fight.  Animation updates interleave riders, so every
// mutable subject, timing latch and captured arm pose lives under its rider key.
// 🆕 P4-6ag: THE MIX.  The engine's own answer at in-reach distance is deterministic for a given
// character: trip 42 (2026-09-12, log `RE_Kenshi_log_20260912_p46af3.txt`) logged 17/17 windows
// with `dq = min(d, reach)` answering `chop left-3` - one technique, every time - and the GATE
// answer above reach is `bigchopv2` (init=25/minS=20), a charging chop whose footwork T23 masks
// away.  So variety cannot come from asking the engine more cleverly; it has to come from us.
// Every name below is a REAL clip in male_skeleton.skeleton (measured with
// `python tools\skelanims.py --find ...`), never guessed - the project has been burned twice by
// guessed names.  Lengths are the asset's own, and because the window is clip-length driven
// (P4-6af-3) each of them plays whole.
// ⚠️ The engine's `CombatTechniqueData*` is still what the HIT uses (`rt.technique`): we choose
// the visible stroke, we do not invent damage.  Only the clip name changes.
// ⚠️ A curated name whose Ogre state is missing is SKIPPED (the cursor moves on); if none of them
// resolves, the engine's own answer is used unchanged - a missing clip must never cost a swing.
// ⚠️ P4-6ai: A CLIP IS ONLY AS AIMED AS ITS OWN STROKE PLANE.  The user caught a down-chop that
// lands 「往旁边空地上劈，几乎垂直于目标90度」, and the clip is the reason, not the facing: measure
// where the R Hand travels from its highest to its lowest key, in the BODY frame
// (`skelanims.py`, angle of that horizontal direction from the body's +Z):
//     chop down static   14 deg   <- forward down-chop
//     chop down          21 deg   <- forward down-chop
//     heavy downcut      65 deg   <- diagonal
//     bigchopv2          65 deg   <- same stroke as heavy downcut, 2.8 s long
//     chop left          85 deg   <- a HORIZONTAL slash (that is what a horizontal cut is)
//     desperate attack   68 deg   <- diagonal
//     downward combo    105 deg   <- a "downward" clip that actually sweeps SIDEWAYS  <- the defect
//     heavy swing       115 deg   /  flying big chop 3  154 deg   <- worse still
// ⇒ `downward combo` and `bigchopv2` are out: one is the reported defect, the other is a longer
// copy of `heavy downcut`'s diagonal.  Body facing (P4-6ah) cannot fix a lateral stroke - the
// blade still crosses sideways relative to the body.
// ⛔ The `blow` family (`back blow low` measures 0 deg, a perfect forward chop) is STILL BANNED:
// those records carry `stumbles` body-part maps and ARE the vanilla hit reaction (P4-6S).
// ⚠️ P4-6aj: THE ROOT TRACK IS THE SECOND FILTER, and the user's report points straight at it.
// We MASK the root (`Bip01` is in the hold table - the rider cannot step), so a clip whose
// footwork we delete is distorted by exactly that deletion.  Measure the clip's own `Bip01` track
// (`skelanims.py`): rotation span / translation:
//     chop down static   15.8 deg /  1.42 u   <- authored IN PLACE, masking costs nothing
//     chop down          22.4 deg /  6.53 u   <- its closing step is deleted
//     chop left          31.7 deg /  7.64 u   <- a horizontal slash: lateral stroke BY DESIGN
//     downward combo     33.4 deg / 13.44 u
//     heavy downcut      92.7 deg / 17.88 u   <- A SPIN: the body turn is half the attack
//     bigchopv2          92.6 deg / 25.01 u   <- the same spin, 2.8 s long
//   Mask a 90 deg spin and the arm's authored sweep - written to accompany that turn - is left
//   sweeping across a body that never turned: 「往旁边空地上劈，几乎垂直于目标」.  The `static`
//   suffix is the authoring signal: that is the variant meant to be played standing still.
// ⚠️ The stroke plane agrees (`chop down static` 14 deg vs `downward combo` 105 / `heavy swing`
// 115 / `flying big chop 3` 154).  The two metrics DISAGREE about `chop down` (21 deg sweep, but
// a 6.5 u step deleted), and the user named it as one of the two suspects with no measurement
// able to clear it - so it is out too.  ⛔ Do not re-add a clip without measuring BOTH numbers.
static const char* const kRideMixClips[] = {
    "chop down static",   // 1.300 s - root 15.8 deg /  1.42 u - 正前方下劈（唯一原地的攻击 clip）
    "chop left"           // 1.067 s - root 31.7 deg /  7.64 u - 横扫（引擎自己一直在选的那条）
};
static const int kRideMixCount = 2;

// 🆕 P4-6ag: the hit beat follows the CLIP.  A fixed 600 ms was the validated point on `chop left`
// (600 / 1067 = 56% of the stroke); with 0.97-2.83 s clips in the mix a fixed beat would land the
// damage while a long clip is still winding up.  Same fraction of every clip, floored by the old
// constant, so `chop left` keeps exactly the beat trip 42 measured.
static const float kRideSwingHitFrac = 0.5625f;   // 600 / 1067, the validated point

struct RideCombatRuntime
{
    Character* rider;
    Character* mount;
    Character* target;
    CombatTechniqueData* technique;
    RideCombatPhase phase;
    DWORD openTick;
    DWORD lastAttemptTick;
    DWORD pauseTick;
    unsigned int serial;
    bool hitResolved;
    bool wasOpen;
    // Which vanilla attack clip this window plays (-1 = not chosen yet this window).
    // Resolution belongs to the rider's animation list; no pointer crosses riders.
    int atkIdx;
    bool attackClipsResolved;
    AnimationData* guardClip;
    AnimationData* blowClip;
    AnimationData* attackClips[kRideAtkAnimCount];
    int attackCursor;
    int attackLogBudget;

    // Arm custody: armHeld/refHave/freeHas gate RideSwingArmRelease and host upper-body mask.
    bool armHeld;
    bool refHave;
    bool swingBonesResolved;
    bool holdHas[kRideSwingHoldCount];
    unsigned short holdHandle[kRideSwingHoldCount];
    bool freeHas[kRideSwingFreeCount];
    unsigned short freeHandle[kRideSwingFreeCount];
    // P4-6aa: left arm / neck / head zeroed on the technique, kept by guard (not free).
    bool keepHas[kRideTechKeepGuardCount];
    unsigned short keepHandle[kRideTechKeepGuardCount];
    bool wristHas[kRideWristCount];
    unsigned short wristHandle[kRideWristCount];
    // P4-6m hand-delta custody, per window.  Float quaternions (x,y,z,w) keep the struct
    // POD-shaped; every read is gated on handTrackHas.  handKF is scratch sample storage
    // (heap: a destructor-bearing local inside an SEH shell is C2712), created at capture
    // and deleted at release by RideSwingArmRelease.
    bool                     handTrackHas[kRideWristCount];
    float                    handG0[kRideWristCount][4];
    float                    handT0Inv[kRideWristCount][4];
    Ogre::TransformKeyFrame* handKF[kRideWristCount];
    int                      handLines;
    bool maskMine;
    int holdN;
    int fitMs;
    int armFrames;
    int noRef;
    float armT;
    float attackSpeed;
    int gapMs;

    char techniqueName[64];

    int swingCount;
    int techCount;
    int skipCount;
    int skipLines;
    int noClipCount;
    int hostKeepFrames;
    int logLines;
    unsigned int restartedSerial;
    int restarts;
    int driveFrames;
    float minDistance;
    float lastLimit;
    int lastHit;
    int hits;
    int misses;
    int hitSkips;
    int hitLines;
    int freeFrames;
    int postArm;
    int headVeto;
    // Sheathe-suppressor diagnostics are per rider; two simultaneous rides must not reset
    // or report one another's calls.
    int shSupReal;
    int shSupNoop;
    int shPass;
    int shSupLines;
    // Geometry captured when this window opened.  Damage is deliberately resolved against the
    // same target, but only while that target is still plausibly where the authored stroke can
    // reach; the engine's dodge result remains untouched after this gate.
    bool geometryHave;
    Ogre::Vector3 targetOpenPos;
    Ogre::Vector3 riderOpenPos;
    float targetOpenLimit;
    float targetOpenDistance;
    int hitSkipReason;
    // 🆕 P4-6ag: the beat this window actually used (kRideSwingHitMs, or fit * kRideSwingHitFrac
    // when the clip is longer).  Logged so the hit timing can be judged per window.
    int hitMs;
    // P4-6S phantom-hit probe.  foreignHits counts stance frames where addList held an
    // entry that is NOT the latched host; foreignLines budgets the name rows.
    int foreignHits;
    int foreignLines;
    // 🆕 P4-6ag: the clip mix.  mixCursor is the next slot to try, mixIdx the slot this window
    // actually plays (-1 = engine's own answer), mixCount is the per-ride frequency table.
    int mixCursor;
    int mixIdx;
    int mixCount[kRideMixCount];
    // 🆕 P4-6ah/P4-6ak: combat facing (node yaw toward the enemy).  faceDeg is this frame's
    // applied angle, faceDegCur the rate-limited value it ramps through, faceTgt the HELD enemy
    // (kept while it stays a live near threat - see kRideCombatFaceRateDegPerSec), faceTick the
    // wall clock that ramp is measured against, faceFrames/faceDegMax the per-ride self-proof.
    float faceDeg;
    float faceDegCur;
    DWORD faceTick;
    DWORD headTick;   // 🆕 P4-6am: wall clock the heading's turn rate is measured against
    Character* faceTgt;
    int faceFrames;
    int faceDegMax;
    // 🆕 P4-6al: facing-jump diagnostic state (previous heading + wanted offset, and the budget).
    Ogre::Vector3 faceFwd;
    bool  faceFwdHave;
    float faceWantPrev;
    bool  faceWantHave;
    int   faceLog;
    int stumbleNameLines;
};

// The transaction owner runs before the animation lookup implementations later in this file.
static void ResolveRideCombatClips(RideCombatRuntime& rt, AnimationClass* rAnim);
static AnimationData* RideCombatHost(RideCombatRuntime& rt, const char** nameOut);
static bool SelectRideCombatAttack(RideCombatRuntime& rt, AnimationClass* rAnim);
// Defined with the animation hooks; RideSwingPass's close edge reasserts guard through it.
static void ClipPin(AnimationClass* rAnim, AnimationData* ad, float target, bool renderSide);

static boost::unordered_map<Character*, RideCombatRuntime> rideCombatByRider;

static RideCombatRuntime* FindRideCombatRuntime(Character* rider)
{
    if (!rider) return NULL;
    boost::unordered_map<Character*, RideCombatRuntime>::iterator it = rideCombatByRider.find(rider);
    return (it != rideCombatByRider.end()) ? &it->second : NULL;
}

static RideCombatRuntime& GetRideCombatRuntime(Character* rider, Character* mount)
{
    boost::unordered_map<Character*, RideCombatRuntime>::iterator it = rideCombatByRider.find(rider);
    if (it == rideCombatByRider.end())
    {
        // operator[] default-constructs the aggregate, leaving scalar members indeterminate
        // under VS2010.  Zero it before any field is read.
        RideCombatRuntime& fresh = rideCombatByRider[rider];
        memset(&fresh, 0, sizeof(fresh));
        fresh.rider = rider;
        fresh.phase = RIDE_COMBAT_IDLE;
        fresh.attackSpeed = 1.0f;
        fresh.gapMs = kRideSwingMinGapMs;
        fresh.armT = -1.0f;
        fresh.minDistance = -1.0f;
        fresh.lastLimit = -1.0f;
        fresh.lastHit = -2;
        fresh.techniqueName[0] = 0;
        fresh.atkIdx = -1;
        if (mount) fresh.mount = mount;
        return fresh;
    }
    RideCombatRuntime& rt = it->second;
    if (rt.rider != rider)
    {
        memset(&rt, 0, sizeof(rt));
        rt.rider = rider;
        rt.phase = RIDE_COMBAT_IDLE;
        rt.attackSpeed = 1.0f;
        rt.gapMs = kRideSwingMinGapMs;
        rt.armT = -1.0f;
        rt.minDistance = -1.0f;
        rt.lastLimit = -1.0f;
        rt.lastHit = -2;
        rt.techniqueName[0] = 0;
        rt.atkIdx = -1;
    }
    if (mount) rt.mount = mount;
    return rt;
}

static void RideCombatSessionClear(RideCombatRuntime& rt)
{
    rt.mount       = NULL;
    rt.target      = NULL;
    rt.technique   = NULL;
    rt.phase       = RIDE_COMBAT_IDLE;
    rt.openTick    = 0;
    rt.hitResolved = false;
}

static void ClearRideCombatRuntime(Character* rider)
{
    if (!rider) return;
    rideCombatByRider.erase(rider);
}

static void ClearAllRideCombatRuntime()
{
    rideCombatByRider.clear();
}

static bool RideCombatPairLive(Character* rider, Character* mount)
{
    if (!rider || !mount) return false;
    boost::unordered_map<Character*, Character*>::const_iterator rit = riderToMount.find(rider);
    if (rit == riderToMount.end() || rit->second != mount) return false;
    boost::unordered_map<Character*, Character*>::const_iterator it = mountToRider.find(mount);
    if (it == mountToRider.end() || it->second != rider) return false;
    return CharacterLooksLive(rider) && CharacterLooksLive(mount);
}

static void RideCombatSessionBegin(RideCombatRuntime& rt, Character* rider, Character* mount,
                                   Character* target, CombatTechniqueData* technique,
                                   DWORD now)
{
    ++rt.serial;
    if (rt.serial == 0) ++rt.serial;
    rt.rider       = rider;
    rt.mount       = mount;
    rt.target      = target;
    rt.technique   = technique;
    rt.phase       = RIDE_COMBAT_SWINGING;
    rt.openTick    = now;
    rt.hitResolved = false;
}

static bool RideCombatSessionOwns(RideCombatRuntime& rt, Character* rider, Character* mount,
                                  Character* target, CombatTechniqueData* technique)
{
    return rt.phase == RIDE_COMBAT_SWINGING
        && !rt.hitResolved
        && rt.rider == rider
        && rt.mount == mount
        && rt.target == target
        && rt.technique == technique
        && RideCombatPairLive(rider, mount);
}

// Clear only this rider's in-flight state.  The world-wipe caller passes NULL.
static void ResetRideSwingWindow(Character* rider)
{
    if (!rider)
    {
        ClearAllRideCombatRuntime();
        ClearRideStanceRuntime(NULL);
        return;
    }
    ClearRideCombatRuntime(rider);
}

// 🆕 P4-6af-3: how long THIS window runs.  The clip's own length at ground speed (1.0x) plus the
// follow-through hold, clamped between the old fixed floor and the escape cap.
// ⚠️ fitMs is measured by RideSwingDrive on its first drive frame, so the frames between the open
// and that measurement answer the floor - which is why kRideSwingWinMs is still a REAL value and
// not just documentation.  A clip whose length reads absurd is refused, never trusted.
// ⚠️ ONE definition, four consumers: the open predicate (RideSwingPass), RideSwingInFlight (the
// hostkeep= self-proof), RideSwingDrive's cross-fade and LegPosePass's swingYield.  They must never
// disagree - a window that closes on one clock and fades on another is a visible snap.
static int RideSwingWindowMs(const RideCombatRuntime& rt)
{
    int ms = kRideSwingWinMs;
    if (rt.fitMs > 0 && rt.fitMs <= kRideSwingLenMaxMs - kRideSwingHoldMs)
        ms = rt.fitMs + kRideSwingHoldMs;
    if (ms < kRideSwingWinMs)   ms = kRideSwingWinMs;
    if (ms > kRideSwingLenMaxMs) ms = kRideSwingLenMaxMs;
    return ms;
}

// Pure read: never creates runtime state from a guard/assertion site.
static bool RideSwingInFlight(Character* rider)
{
    RideCombatRuntime* rt = FindRideCombatRuntime(rider);
    if (!rt || rt->openTick == 0) return false;
    return (GetTickCount() - rt->openTick) < (DWORD)RideSwingWindowMs(*rt);
}

// Read Kenshi's finished attack-speed multiplier once per open attempt.  The getter is an
// engine call on a live Character, so keep the same narrow AV containment used by the other
// shipping probes.  Invalid/zero values fall back to neutral; only then are modded extremes
// clamped to a bounded range.
static float RideSwingAttackSpeed(Character* rider)
{
    float speed = 1.0f;
    __try
    {
        CharStats* stats = rider ? rider->getStats() : NULL;
        if (stats) speed = stats->getAttackSpeed();
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        speed = 1.0f;
    }
    if (!(speed == speed) || speed <= 0.0f) speed = 1.0f; // NaN / unusable engine result
    if (speed < kRideSwingAttackSpeedMin) speed = kRideSwingAttackSpeedMin;
    if (speed > kRideSwingAttackSpeedMax) speed = kRideSwingAttackSpeedMax;
    return speed;
}

static int RideSwingGapMs(float speed)
{
    int gap = (int)((float)kRideSwingMinGapMs / speed + 0.5f);
    if (gap < kRideSwingMinGapFloorMs) gap = kRideSwingMinGapFloorMs;
    return gap;
}

// Everything the open decision and its log line need, in one POD - no destructors, so the whole
// thing including the raw-offset reads below can sit inside the __try frame.
struct RideSwingPick
{
    CombatTechniqueData* tech;
    char  name[64];   // the CLIP we drive - CombatTechniqueData +0x00, a std::string, COPIED
    char  gate[64];   // 🆕 T24: the technique the GATE question returned (see below).  Equal to
                      // name[] would mean the engine gives one answer for both distances.
    float init;       // +0x38 initialDistance      } the engine's opinion of the range, read off
    float minS;       // +0x3C minDistanceVsStatic  } the GATE technique - that is what lim= uses
    float d;          // horizontal rider->threat distance that produced it
    float dq;         // 🆕 T24: the distance the CLIP question was asked with (min(d, reach))
    float reach;      // the weaponReach fed to chooseAttack
    Character* tgt;   // 🆕 P4-6: the threat the window opened for.  The hit resolution needs the
                      // WHO, not just the distance - RideNearestThreat already returned it, and
                      // re-asking at hit time could name a different enemy than the one the
                      // arc was aimed at.
};

// READ-ONLY technique producer.  ⚠️ NOT ONE ENGINE WRITE IN HERE - that is exactly what lets it
// live outside the debugContinuous gate while RiderCombatLever, which does write, cannot (:593).
// RideNearestThreat is T18's four-tier search, so the target may come from the MOUNT's books; in a
// player build that is the only reason it resolves at all.
static bool RideSwingChooseImpl(Character* rider, Character* mount, RideSwingPick* out)
{
    float d = -1.0f;
    Character* threat = RideNearestThreat(rider, mount, &d);
    if (!threat || d < 0.0f) return false;

    // Reach from the rider when it has one, else the enemy's, else the synthetic - the same ladder
    // the P41D dry run uses (:7427).  ⚠️ The ORDER matters and the meaning has changed: trip 13
    // measured the rider's own weaponReach() at 10.50, so 9.0f is now the last resort instead of
    // the normal case it was when P4-1c read 0.00 four times out of four.
    CombatClass* rcc = rider->getCombatClass();
    CombatClass* ecc = threat->getCombatClass();
    float reach = 9.0f;
    if (rcc && rcc->weaponReach() > 0.01f)      reach = rcc->weaponReach();
    else if (ecc && ecc->weaponReach() > 0.01f) reach = ecc->weaponReach();

    CharStats* rst = rider->getStats();
    if (!rst) return false;
    // chooseAttack takes weaponReach as an ARGUMENT (@0x886880, RE_NOTES §17.2) - that is the whole
    // reason the CharStats shim exists.  lastAttack=NULL / opponentIsStationary=false, as in the dry
    // run: we are asking "what would you swing", not continuing a combo.
    //
    // 🆕 T24 - TWO questions, one geometry.  Trip 21 answered 9/9 windows with 'bigchopv2'
    // (init=25.00) and the eyeball read 「双手持刀然后反转刀身把刀朝下然后向下刺去」.  Two measured
    // facts explain that: init=25.00 is more than DOUBLE the rider's own reach=10.50, and trip 20
    // (same clip, no mask) slid the rider bodily in front of the mount => that record is a
    // CLOSING attack whose footwork is half the animation.  A rider cannot step in, and T23 masks
    // the root off, so what reaches the screen is the arrival half with the travel deleted.
    //   Why 'bigchopv2' EVERY time: the mount's own body holds the enemy off, so the
    // rider->threat distance is structurally 12..25 (trip 21 dmin= 12.56 / 13.89 / 21.40 / 15.47)
    // and never inside reach.  Asked about that distance the engine correctly answers "close the
    // gap first".  So ask it twice instead of inventing a number:
    //   * GATE  - chooseAttack(d, reach): the engine's opinion of the ENGAGEMENT.  Its init=/minS=
    //     are what lim= keeps using below, so the swing RHYTHM is byte-for-byte the one trip 21
    //     measured (9 windows over 3 fights) - this change must not silently re-tune that.
    //   * CLIP  - chooseAttack(min(d, reach), reach): what a fighter ALREADY within a blade of the
    //     enemy throws, i.e. an attack with no footwork to lose.  min() and not `reach` flat so a
    //     genuinely adjacent enemy is still asked about honestly.
    // ⚠️ Both numbers are the engine's.  Nothing here invents a distance constant, which is the one
    // thing this route has been forbidden to do since the plan was written.
    // ⚠️ If the two questions return the SAME technique the log says so on its own (gate= vs tech=)
    // and this rung is a clean negative: the next lever would be naming a clip ourselves.
    CombatTechniqueData* gate = rst->chooseAttack(d, reach, NULL, false);
    if (!gate) return false;
    float dq = (d < reach) ? d : reach;
    CombatTechniqueData* tech = rst->chooseAttack(dq, reach, NULL, false);
    if (!tech) tech = gate;   // never lose a swing to the second question

    // Raw offsets, read the same way and for the same reason as the P41D read line (:7432-7445):
    // CombatTechniqueData's own header drags in MedicalSystem.h, which this tree never compiles.
    const std::string* an = (const std::string*)((const char*)tech + 0x00);
    _snprintf_s(out->name, sizeof(out->name), _TRUNCATE, "%s", an->c_str());
    const std::string* gn = (const std::string*)((const char*)gate + 0x00);
    _snprintf_s(out->gate, sizeof(out->gate), _TRUNCATE, "%s", gn->c_str());
    out->tech  = tech;
    // ⚠️ init/minS come off the GATE technique on purpose - see lim= at the open decision.
    out->init  = *(const float*)((const char*)gate + 0x38);
    out->minS  = *(const float*)((const char*)gate + 0x3C);
    out->d     = d;
    out->dq    = dq;
    out->reach = reach;
    out->tgt   = threat;
    return true;
}

static bool RideSwingChoose(Character* rider, Character* mount, RideSwingPick* out)
{
    if (!rider || !out) return false;
    out->tech = NULL;
    out->name[0] = 0;
    out->gate[0] = 0;
    out->init = out->minS = out->d = out->dq = out->reach = -1.0f;
    __try { return RideSwingChooseImpl(rider, mount, out); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
}

// ⚠️⚠️ TRIP 17 (2026-09-02, diagnostics OFF, 4 rides / 2 fights, log
// RE_Kenshi_log_trip17_635436E9_diagoff.txt) KILLED THE runCombatAnimation ROUTE, and the way it
// died is worth more than the route was:
//   * The trigger works.  swing=5 / tech=17 / skip=12 / fail=0 on the first ride, 6 windows total,
//     technique names 'bigchopv2' (init 25.00, fired at d=21-23) and 'chop left-3' (no opinion =>
//     reach 10.50, fired at d=9.95/10.19).  runCombatAnimation was called with a real technique and
//     never faulted.
//   * Nothing played.  The player saw the rider STAND UP STRAIGHT on the ox for about the length of
//     each window, and the log says exactly why: SIX `LEGPOSE released grace=12` lines, one per
//     window, 110-160 ms after each open (175.748->175.854, 178.256->178.377, 184.745->184.862,
//     205.297->205.443, 209.798->209.919, 422.040->422.199).  grace=12 means LegPoseFindHost found
//     NO host - no SingleAnimation owning an AnimationState - for 12 consecutive frames, so the
//     straddle mask was handed back and the engine's own 'idle_stand_normal' drove the legs.
//   * ⇒ runCombatAnimation does NOT put the technique into rAnim->layer[] as a state-owning clip.
//     LegPoseFindHost is an independent detector here and it saw an empty stage.  (It only scans the
//     layer add/remove lists, so "the engine keeps combat animations somewhere else" is not
//     excluded - but nothing reached the screen either way.)
//   * ⇒ AND the real lesson, which is P4-1i's constraint restated: withholding the guard is only
//     safe if something ELSE becomes the host.  §U got away with it because it pinned 'mid blow',
//     a clip with a record and therefore a state; this route withheld the guard and put nothing in
//     its place.  ⇒ never leave the skeleton hostless, not for one frame.
// The technique lookup below is KEPT - it is a good trigger and it is read-only.  What changed is
// what the window plays: 'mid blow' through ClipPin, which trip 10 measured end to end.  The dead
// call is recorded here rather than left in the build:
//     rAnim->runCombatAnimation(chTech, 1.0f, "");   // @0x5B6E80, RE_NOTES §17.2
// (its std::string-by-value third argument is why it needed an Impl/shell SEH split at all - C2712).
// Reviving it needs a reason to believe the clip can reach the layer; the pure-read probe on the
// technique's own Ogre state, logged below, is what would supply one.

// ---- 🆕 P4-6: THE DISPATCH (刀找人) -----------------------------------------------------
// These checks answer "did the target remain in the stroke's geometry?" before calling the
// engine.  They are not a second hit/dodge system: once they pass, hitByMeleeAttack still owns
// dodge, block, wound and body-part resolution.  The movement allowance is intentionally broad
// because the target can walk during the 552 ms authored wind-up; it only rejects a target that
// clearly left the opening neighbourhood.
enum RideHitSkipReason
{
    RIDE_HIT_SKIP_NONE = 0,
    RIDE_HIT_SKIP_DOWN,
    RIDE_HIT_SKIP_MOVED,
    RIDE_HIT_SKIP_RANGE,
    RIDE_HIT_SKIP_HEIGHT,
    RIDE_HIT_SKIP_SECTOR
};

static const float kRideSwingTargetMoveMax = 12.0f;
static const float kRideSwingSectorCosMin  = -0.35f; // about 110 degrees, broad side-slash cone

static const char* RideHitSkipReasonName(int reason)
{
    switch (reason)
    {
    case RIDE_HIT_SKIP_DOWN:   return "down";
    case RIDE_HIT_SKIP_MOVED:  return "moved";
    case RIDE_HIT_SKIP_RANGE:  return "range";
    case RIDE_HIT_SKIP_HEIGHT: return "height";
    case RIDE_HIT_SKIP_SECTOR: return "sector";
    default:                   return "none";
    }
}

static int RideSwingGeometryCheck(Character* rider, Character* threat, RideCombatRuntime* rt)
{
    if (!rider || !threat) return RIDE_HIT_SKIP_DOWN;
    if (threat->isDown() || threat->isDead()) return RIDE_HIT_SKIP_DOWN;
    if (!rt || !rt->geometryHave) return RIDE_HIT_SKIP_NONE;

    Ogre::Vector3 tp = threat->getPosition();
    Ogre::Vector3 rp = rider->getPosition();
    Ogre::Vector3 moved = (tp - rt->targetOpenPos) - (rp - rt->riderOpenPos);
    if (moved.squaredLength() > kRideSwingTargetMoveMax * kRideSwingTargetMoveMax)
        return RIDE_HIT_SKIP_MOVED;

    Ogre::Vector3 flat = tp - rp;
    float horizontal = Ogre::Math::Sqrt(flat.x * flat.x + flat.z * flat.z);
    float targetRadius = threat->getRadius();
    if (!(targetRadius > 0.0f) || targetRadius > 200.0f) targetRadius = 0.0f;
    float limit = rt->targetOpenLimit > 0.0f ? rt->targetOpenLimit : kRideThreatDist;
    if (horizontal > limit + targetRadius) return RIDE_HIT_SKIP_RANGE;
    if (Ogre::Math::Abs(flat.y) > limit + targetRadius) return RIDE_HIT_SKIP_HEIGHT;

    // The rider's node is the same facing used by the authored pose.  Keep the cone wide enough
    // for the deliberate mounted side-slash, while rejecting a target that crossed fully behind
    // the rider during the wind-up.  If the node is unavailable, do not invent a facing and let
    // the range/height checks decide.
    AnimationClass* rAnim = rider->getAnimationClass();
    if (rAnim && rAnim->node && horizontal > 0.01f)
    {
        Ogre::Vector3 f = rAnim->node->getOrientation() * Ogre::Vector3::UNIT_Z;
        f.y = 0.0f;
        float fl = Ogre::Math::Sqrt(f.x * f.x + f.z * f.z);
        if (fl > 0.01f)
        {
            f /= fl;
            Ogre::Vector3 d = flat;
            d.y = 0.0f;
            d /= horizontal;
            if (f.dotProduct(d) < kRideSwingSectorCosMin)
                return RIDE_HIT_SKIP_SECTOR;
        }
    }
    return RIDE_HIT_SKIP_NONE;
}

// One hitByMeleeAttack per swing window, so the blade the arc drew actually reaches someone.
// Everything about WHO resolves what is the engine's:
//   * the ARGUMENTS are all engine products - the threat came from RideNearestThreat (the same
//     four-tier search the stance aims by), the technique from chooseAttack (the same call that
//     gated the window), and the three damage numbers from the rider's own CharStats getters.
//     Not one number of ours enters this call.  (Same rule the chooseAttack ladder has always
//     followed: no invented distance, no invented damage.)
//   * the VERDICT is the engine's - hitByMeleeAttack internally rolls the dodge chance, decides
//     the block, picks the body part, opens the wound (MedicalSystem::addWound is called from
//     inside it), applies the stumble, plays the sounds.  HIT_MISSED back means the enemy dodged
//     it, which is exactly the vanilla behaviour we are told to keep (「原版敌人会躲避，我们也
//     允许敌人躲避」).
//   * CUT_DEFAULT for the direction: the engine's own pipeline converts it against the attacker's
//     facing (Character::convertCutDirection, Character.h:513-514).  Reading the technique's
//     ImpactPoint[0].direction would add a raw-offset assumption on the impactPoints lektor
//     (+0xB0); CUT_DEFAULT costs none and lets the engine translate.
//   * comboID 0: we are not continuing anyone's combo.
// The Impl/shell split is the house pattern (C2712: the std::string in the log line cannot sit
// in the __try frame).  ⚠️ The ONLY writer of gRideSwingHitDone is RideSwingPass; this function
// resolves and reports, it does not own the window.
// Return value: 1 = engine called it a hit, 0 = engine dodged it, -1 = not attempted (caller
// logs the reason itself); the ints also feed the ride's hit=/miss= totals.
static int RideSwingDamageImpl(Character* rider, Character* threat, CombatTechniqueData* tech)
{
    RideCombatRuntime* rt = FindRideCombatRuntime(rider);
    if (rt) rt->hitSkipReason = RIDE_HIT_SKIP_NONE;
    // These are cached at window-open and used roughly one second later.  Map ownership only
    // protects mounted pairs; it does not keep an enemy alive across a death or world unload.
    // Reject implausible cached pointers before any virtual/member dispatch.
    if (!CharacterLooksLive(rider) || !CharacterLooksLive(threat) || !tech) return -1;
    // The window opened seconds ago - the target may have gone down since (our own previous
    // hit included).  A corpse or a KO cannot be hit, and hitByMeleeAttack would still run its
    // rolls on it, so refuse first.  Same liveness predicates as RideThreatConsider.
    int geometry = RideSwingGeometryCheck(rider, threat, rt);
    if (geometry != RIDE_HIT_SKIP_NONE)
    {
        if (rt) rt->hitSkipReason = geometry;
        return -1;
    }

    CharStats* rst = rider->getStats();
    if (!rst) return -1;

    // Do not reconstruct a partial packet from three convenience getters.  Kenshi computes
    // target-dependent blunt, stun and armour-penetration values in this call as well.
    Damages dmg = rst->getTotalAttackDamageFor(threat);

    // The non-virtual twin, exactly the way initCombatMode is called: vtable slot 0x0 is the
    // base's, and the _NV_ export is the same body without the dispatch.  (Character.h:507-508,
    // RVA 0x438D70.)
    HitMaterialType ret = threat->_NV_hitByMeleeAttack(CUT_DEFAULT, dmg,
                                                       rider, tech, 0);

    // Ungated and budgeted, the P43RD discipline: this CHANGES game state, and state-changing
    // events are never debugContinuous-gated.  The line is the whole verdict on the dispatch -
    // ret= is the engine's own dodge/block answer, dmg= is what it was asked to apply.
    // 🆕 P4-6ai: the 2026-09-12 session logged `ret=2` (engine: landed) with
    // `dmg=0.0/0.0/0.0/0.0` on ALL 22 resolutions, while the rider was demonstrably armed
    // (`P43RD wih=0->1`, `P43SUP real=10` suppressing ten sheathes).  Trip 33/35 on the same code
    // path read 8.3-20.9.  `reach=`/`d=` are added to separate the two surviving explanations -
    // "the rider is not actually holding a weapon any more" (reach<=0) vs "the engine computes no
    // damage for this attacker/target pair" (reach>0 and dmg still 0) - without another trip.
    float hitReach = -1.0f;
    float hitDist  = -1.0f;
    {
        CombatClass* hcc = rider->getCombatClass();
        if (hcc) hitReach = hcc->weaponReach();
        Ogre::Vector3 hd = threat->getPosition() - rider->getPosition();
        hitDist = Ogre::Math::Sqrt(hd.x * hd.x + hd.z * hd.z);
    }
    if (rt && rt->hitLines < kRideSwingHitLines)
    {
        ++rt->hitLines;
        const std::string* an = (const std::string*)((const char*)tech + 0x00);
        char b[320];
        _snprintf_s(b, 320, _TRUNCATE,
            "Riding: P43ST rider=%p n=%d ret=%d dmg=%.1f/%.1f/%.1f/%.1f reach=%.2f d=%.2f tech='%s' f=%u",
            (void*)rider, rt->swingCount, (int)ret,
            dmg.cut, dmg.blunt, dmg.pierce, dmg.bleedMult,
            hitReach, hitDist, an->c_str(), gP3Frames);
        DebugLog(std::string(b));
    }
    return (ret == HIT_MISSED) ? 0 : 1;
}

// The shell.  An AV in here disarms nothing permanently - RideSwingPass latches
// gRideSwingHitDone before calling, so one faulting call costs exactly one window's
// resolution, not a per-frame retry loop (the kRideSwingMinGapMs clock already governs
// the next ATTEMPT).
static int RideSwingDamage(Character* rider, Character* threat, CombatTechniqueData* tech)
{
    __try { return RideSwingDamageImpl(rider, threat, tech); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return -1; }
}

// The in-ride close.  Dismount() already calls endCombatAnimation() unconditionally (:5489); this is
// the ordinary expiry, and it stays even though we no longer start a combat animation ourselves -
// it is a no-op when nothing is playing (HISTORY:453) and it costs one call per swing.
static void RideSwingEnd(AnimationClass* rAnim)
{
    if (!rAnim) return;
    __try { rAnim->endCombatAnimation(); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { }
}

// Pure READ of the Ogre side, by clip name, for the one question trip 17 left open: does a technique
// clip with NO AnimationData record have an Ogre::AnimationState at all?  §21.1(1) is the call chain
// (mapper -> id -> hasAnimationState -> getAnimationState, NULL when absent, nothing inserted), and
// RidingPlugin.cpp:4643 already uses the very same call for the mask handback, so this adds no new
// address and no new hook.  ⚠️ §21 never claimed a record-less track HAS a state - that is one step
// past what is written down, and §21.5 forbids settling it from upstream Ogre source.  So it is
// measured here and nowhere else.  ⚠️ Read-only: not one field is written.
static Ogre::AnimationState* RideSwingStateImpl(AnimationClass* rAnim, const char* clip)
{
    std::string nm(clip);
    return rAnim->getAnimationState(nm);
}

// The std::string above needs unwinding, which C2712 forbids in a function holding __try - the same
// Impl/shell split RideSheatheSuppressed uses, for the same reason.
static Ogre::AnimationState* RideSwingState(AnimationClass* rAnim, const char* clip)
{
    if (!rAnim || !clip || !clip[0]) return NULL;
    __try { return RideSwingStateImpl(rAnim, clip); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return NULL; }
}

static void RideSwingProbeState(AnimationClass* rAnim, const char* clip, char* out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    if (!rAnim || !clip || !clip[0]) return;
    Ogre::AnimationState* st = RideSwingState(rAnim, clip);
    if (!st) { _snprintf_s(out, cap, _TRUNCATE, "ogre=absent"); return; }
    _snprintf_s(out, cap, _TRUNCATE, "ogre=found en=%d w=%.3f t=%.3f len=%.3f",
                st->getEnabled() ? 1 : 0, st->getWeight(),
                st->getTimePosition(), st->getLength());
}

// 🆕 T22: DRIVE the technique's own Ogre::AnimationState, which is the lever trips 18 and 19
// opened by measuring `ogre=found` 31/31 - a technique clip with no AnimationData record still
// owns a state (§17.10 / §19.4 / §21.5).  ClipPin can never reach these clips (43/44 have no
// record at all, §19), so this is the only route to the motion the ENGINE would play, and the
// only reason to want it is the trip-19 report: the pinned stand-in now plays whole, but
// "这个动作实在不像挥砍" - 'mid blow' is not an attack clip and never will be one.
//
// ⚠️ ADDITIVE ONLY.  'mid blow' keeps its pin, its weight, its speed and its restart, because it
// is the HOST that stops the engine rendering BIND (P4-1i / trip 17: assert nothing and the
// straddle leg mask is handed back and the rider stands up).  Nothing that passed trips 18-19 is
// bet on this working.  If the write is refused or ignored the close line reads en=0 / w=0.000
// exactly as it does today and the ride behaves as it does today.
//
// ⚠️ Ogre does NOT advance a state by itself - whoever owns it calls addTime.  Kenshi advances
// the ones IT drives (trip 18 caught the engine playing 'chop left-3' to t=0.251 then disabling
// it), and it knows nothing about this one, so the time is written outright from the window's own
// elapsed wall clock.  Writing it absolutely (rather than accumulating) also means a dropped
// frame cannot desynchronise the arc from the window.
// P4-6l: the one cross-fade curve both writers share - RideSwingDrive sets the technique
// state's weight to w, LegMaskApply sets guard's free-bone mask entries to (1 - w).  Same
// elapsed-ms input from the same openTick, so the two can never disagree by more than a
// frame.  Abnormal closes (stance dropped mid-window) skip the tail ramp - the window is
// already closing and Undrive/ArmRelease own that edge.
// P4-6af-3: winMs is the LIVE window length (RideSwingWindowMs) - it is the clip's own length plus
// the follow-through hold, so the tail ramp lands at the END of the hold and the stroke's last
// frame is shown at full weight first.  Passing the old fixed constant here would fade the
// technique out while the clip was still travelling, which is exactly the reported defect.
static float RideSwingBlend(DWORD elapsedMs, int winMs)
{
    if (kRideSwingFadeMs <= 0) return 1.0f;
    float e   = (float)elapsedMs;
    float f   = (float)kRideSwingFadeMs;
    float rem = (float)winMs - e;
    if (rem > f) rem = f;
    float w = (e < rem ? e : rem) / f;
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    return w;
}

// Pause-frozen elapsed for a live window.  openTick itself is only slid forward on the
// unpause frame inside RideSwingPass; every other consumer (Drive's phase, LegPosePass's
// swingYield) must freeze the same way or the arc / cross-fade keep walking on the wall
// clock while the game is paused - which is exactly the bug the user reported
// (「我们的挥砍动作在暂停游戏时还在播放」).  Latches pauseTick on the first paused frame
// so whichever pass runs first freezes everyone else to the same instant.
static DWORD RideSwingFrozenElapsed(RideCombatRuntime& rt, DWORD now)
{
    if (rt.openTick == 0) return 0;
    if (ou && ou->isPaused())
    {
        if (rt.pauseTick == 0) rt.pauseTick = now;
        return rt.pauseTick - rt.openTick;
    }
    return now - rt.openTick;
}

// ---- P4-6m / P4-6ac: the hand writer ------------------------------------------------------
// Trip-38 log: 12/12 windows picked 'chop left-3' - a two-handed ground chop whose Forearm
// tracks roll hard, while P4-6C pins both Hand bones to the guard grip.  The forearm rolls
// under a fixed hand and the wrist reads 「转筋」.
// P4-6m first used a RELATIVE form (qT(t)*qT(0)^-1 * g0) so the wrist stayed inside the
// clip's own range with a constant guard offset.  User later: ground flat-slash palm is
// UP, mounted is DOWN - because g0 (guard 1h) was the attitude base, so the palm colour
// was locked to the guard grip no matter how the technique rolled.
// 🆕 P4-6ac (2026-09-12 user: 「guard是我们加的吗？能去掉吗？感觉是他在捣乱」→ chose
// absolute technique hand): at full blend the right hand IS the technique's Hand track
// (same absolute pose ground combat shows); only the 120 ms edges slerp back to the
// guard grip so open/close stay continuous.  Left hand is not written (P4-6aa).
//   hand = slerp(w, g0, qT(t))     // w=1 ⇒ ground absolute; w=0 ⇒ guard grip
// Sampling writes into the window's own KeyFrame (handKF): a destructor-bearing local
// inside __try is C2712, and a shared scratch would race two riders.
static bool RideSwingSampleTrackQ(Ogre::OldNodeAnimationTrack* tr, float t,
                                  Ogre::KeyFrame* kf, Ogre::Quaternion* out)
{
    if (!tr || !kf || !out) return false;
    __try
    {
        const Ogre::TimeIndex ti((Ogre::Real)t);
        tr->getInterpolatedKeyFrame(ti, kf);
        *out = static_cast<Ogre::TransformKeyFrame*>(kf)->getRotation();
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
}

// Once per window, on the first Drive frame (armHeld is the custody flag the release path
// already gates on - it had been dead since the authored arm retired; this revives it).
// Captures each hand's guard grip, samples the technique's hand track at t=0 and takes the
// bones manual.  A hand with no track (or a skeleton rebuild mid-window) simply stays with
// the guard grip; per-hand gating everywhere, nothing assumed.
static void RideSwingHandCapture(AnimationClass* rAnim, RideCombatRuntime* rt)
{
    if (!rAnim || !rt || rt->armHeld || !rt->techniqueName[0]) return;
    Ogre::OldSkeletonInstance* skel = rAnim->skeleton;
    if (!skel) return;
    std::string tn(rt->techniqueName);
    if (!skel->hasAnimation(tn)) return;
    Ogre::Animation* anim = skel->getAnimation(tn);
    if (!anim) return;
    unsigned short nb = skel->getNumBones();
    int found = 0;
    bool miss[kRideWristCount] = { false, false };
    for (int i = 0; i < kRideWristCount; ++i)
    {
        rt->handTrackHas[i] = false;
        if (!rt->wristHas[i] || rt->wristHandle[i] >= nb) { miss[i] = true; continue; }
        std::string bn(kRideWristBones[i]);
        if (!rAnim->getHasBone(bn)) { miss[i] = true; continue; }
        Ogre::OldBone* b = rAnim->_getBone(bn);
        if (!b) { miss[i] = true; continue; }
        if (!anim->hasOldNodeTrack(rt->wristHandle[i])) { miss[i] = true; continue; }
        Ogre::OldNodeAnimationTrack* tr = anim->getOldNodeTrack(rt->wristHandle[i]);
        if (!tr) { miss[i] = true; continue; }
        if (!rt->handKF[i]) rt->handKF[i] = new Ogre::TransformKeyFrame(tr, 0.0f);
        Ogre::Quaternion q0;
        if (!RideSwingSampleTrackQ(tr, 0.0f, rt->handKF[i], &q0)) { miss[i] = true; continue; }
        Ogre::Quaternion g0 = b->getOrientation();
        Ogre::Quaternion t0inv = q0.Inverse();
        rt->handG0[i][0] = g0.x;  rt->handG0[i][1] = g0.y;  rt->handG0[i][2] = g0.z;  rt->handG0[i][3] = g0.w;
        rt->handT0Inv[i][0] = t0inv.x;  rt->handT0Inv[i][1] = t0inv.y;
        rt->handT0Inv[i][2] = t0inv.z;  rt->handT0Inv[i][3] = t0inv.w;
        rt->handTrackHas[i] = true;
        b->setManuallyControlled(true);
        b->needUpdate();
        ++found;
    }
    // Taken even when both hands missed: the window simply has no hand writer, and an
    // unconditional latch keeps the capture from retrying (and re-logging) every frame.
    rt->armHeld = true;
    if (rt->handLines < kRideSwingHandLines)
    {
        ++rt->handLines;
        char b2[160];
        _snprintf_s(b2, 160, _TRUNCATE,
            "Riding: SWING handtake rider=%p L=%d R=%d found=%d f=%u",
            (void*)rAnim->me, miss[0] ? 0 : 1, miss[1] ? 0 : 1, found, gP3Frames);
        DebugLog(std::string(b2));
    }
}

// Per Drive frame.  w is RideSwingBlend's cross-fade: the hand rides the same curve as the
// technique's weight, so both window edges return it to the guard grip instead of snapping.
static void RideSwingHandWrite(AnimationClass* rAnim, RideCombatRuntime* rt, float tSec, float w)
{
    if (!rAnim || !rt || !rt->techniqueName[0] || tSec < 0.0f) return;
    // First window frame: capture runs here and latches armHeld.  The gate below the call
    // (not above it) is what makes the capture reachable at all.
    if (!rt->armHeld)
    {
        RideSwingHandCapture(rAnim, rt);
        if (!rt->armHeld) return;
    }
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    Ogre::OldSkeletonInstance* skel = rAnim->skeleton;
    if (!skel) return;
    std::string tn(rt->techniqueName);
    if (!skel->hasAnimation(tn)) return;
    Ogre::Animation* anim = skel->getAnimation(tn);
    if (!anim) return;
    for (int i = 0; i < kRideWristCount; ++i)
    {
        // P4-6aa: RIGHT HAND ONLY.  Left arm stays on guard (keep list), so applying the
        // technique's left-hand roll to a guard-pinned left arm was pure extra twist.
        if (i != 1) continue;
        if (!rt->handTrackHas[i] || !rt->handKF[i]) continue;
        if (!rt->wristHas[i]) continue;
        std::string bn(kRideWristBones[i]);
        if (!rAnim->getHasBone(bn)) continue;
        Ogre::OldBone* b = rAnim->_getBone(bn);
        if (!b) continue;
        if (!b->isManuallyControlled())
        {
            // skeleton rebuilt under us (§21.3): re-take custody.  The captured baseline is
            // stale by whatever the rebuild reset, and the NEXT window's capture refreshes it.
            b->setManuallyControlled(true);
            b->needUpdate();
        }
        if (!anim->hasOldNodeTrack(rt->wristHandle[i])) continue;
        Ogre::OldNodeAnimationTrack* tr = anim->getOldNodeTrack(rt->wristHandle[i]);
        if (!tr) continue;
        Ogre::Quaternion qT;
        if (!RideSwingSampleTrackQ(tr, tSec, rt->handKF[i], &qT)) continue;
        Ogre::Quaternion g0(rt->handG0[i][3], rt->handG0[i][0], rt->handG0[i][1], rt->handG0[i][2]);
        // P4-6ac: ABSOLUTE technique hand at full blend (ground palm).  g0 only anchors the
        // 120 ms edge fades so open/close stay on the guard grip.
        Ogre::Quaternion want = (w >= 0.999f) ? qT : Ogre::Quaternion::Slerp(w, g0, qT, true);
        b->setOrientation(want);
        b->needUpdate();
    }
}

static bool RideSwingDrive(AnimationClass* rAnim, const char* clip, DWORD elapsedMs)
{
    RideCombatRuntime* rt = rAnim ? FindRideCombatRuntime(rAnim->me) : NULL;
    if (!rt) return false;
    Ogre::AnimationState* st = RideSwingState(rAnim, clip);   // own string + SEH shell
    if (!st) return false;
    __try
    {
        // 🆕 P4-6z - PLAY AT GROUND SPEED.  User: the compressed 700 ms arc thrashed the arms
        // (「骑砍挥砍的时候动作太快了，所以胳膊扭曲。调成和地面战斗时一样的速度」).  The
        // phase timeline is the clip's own length at 1.0x - the same rate ground combat plays
        // it ('bigchopv2' 2.833 s, 'chop left-3' 1.067 s, §17.10).  The window clock
        // (kRideSwingWinMs) only decides how long we keep showing it; a clip longer than the
        // window is simply cut off.  T24's "never stretch" still holds in the other direction:
        // we never speed up a short clip to fill the window either.  Clamped at the end: a
        // non-looping state holds its last pose (follow-through).
        float len = st->getLength();
        if (len > 0.001f)
        {
            float fitMs = len * 1000.0f;   // natural length = 1.0x
            rt->fitMs = (int)fitMs;
            float f = (float)elapsedMs / fitMs;
            if (f > 1.0f) f = 1.0f;
            st->setLoop(false);
            st->setTimePosition(len * f);
        }
        st->setEnabled(true);
        // P4-6l: ramped, not hard 1.0 - see RideSwingBlend.
        st->setWeight(kRideSwingTechW * RideSwingBlend(elapsedMs, RideSwingWindowMs(*rt)));
        // P4-6af-2: the technique owns the whole upper body for the window - spine, arms, hands
        // AND the weapon props.  Only HOLD bones (root+legs) are zeroed here, so a ground clip
        // can never step the skeleton or fight the straddle / sit-height writes in LegPosePass.
        // The host yields the rest through kRideSwingFreeBones; nothing on the wield chain is
        // zeroed any more (that was P4-6af-1, and it cost the ground palm colour).
        unsigned short nb = rAnim->skeleton ? rAnim->skeleton->getNumBones() : 0;
        if (nb)
        {
            if (!st->hasBlendMask())
            {
                st->createBlendMask(nb, 1.0f);
                rt->maskMine = true;
            }
            for (int i = 0; i < kRideSwingHoldCount; ++i)
            {
                if (!rt->holdHas[i] || rt->holdHandle[i] >= nb) continue;
                st->setBlendMaskEntry((size_t)rt->holdHandle[i], 0.0f);
            }
        }
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
}

// Hand the state back the way it was found.  Called on the close edge AND from the per-ride
// reset, so an abnormal end (dismount mid-window, knocked down, load) cannot leave a technique
// enabled at weight 1 on a character that is no longer riding.
static void RideSwingUndrive(AnimationClass* rAnim, const char* clip)
{
    RideCombatRuntime* rt = rAnim ? FindRideCombatRuntime(rAnim->me) : NULL;
    if (!rt) return;
    Ogre::AnimationState* st = RideSwingState(rAnim, clip);
    if (!st) return;
    __try
    {
        // 🆕 T23: the mask goes back FIRST.  A 0.0 entry left on this state outlives the ride and the
        // dismount - the same shape as the thigh leak §21.2, except the victim is the player's own
        // ground swing (dead root ⇒ an attack that no longer steps into the blow).  Re-found by name
        // on every call, so a rebuilt state set simply reads absent and there is nothing to leak.
        unsigned short nb = rAnim->skeleton ? rAnim->skeleton->getNumBones() : 0;
        if (st->hasBlendMask())
        {
            if (rt->maskMine) st->destroyBlendMask();
            else
            {
                for (int i = 0; i < kRideSwingHoldCount; ++i)
                    if (rt->holdHas[i] && (nb == 0 || rt->holdHandle[i] < nb))
                        st->setBlendMaskEntry((size_t)rt->holdHandle[i], 1.0f);
                // P4-6af-2: nothing to restore for the wield chain - those entries are never
                // written now (the hands/props ride kRideSwingFreeBones like the rest of the
                // upper body, and LegMaskRelease covers them with that table).
            }
        }
        rt->maskMine = false;
        st->setWeight(0.0f);
        st->setEnabled(false);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { }
}

// Find the live entry for a clip WITHOUT calling into the engine.  ClipPin (:6407) already walks
// exactly these lists every frame in the shipping build, so this reuses a traversal that has survived
// every trip since T14 and adds no new address.  (getAnimationPlaying(AnimationData*) does exist in
// AnimationClass.h, but a brand-new engine call in the shipping path buys nothing over a walk we are
// doing anyway.)  addList only - same rule as ClipPin's `mine`: an entry on the removeList is on its
// way out and writing to it would be writing to a corpse.
static AnimationClassBase::SingleAnimation* RideSwingFindEntry(AnimationClass* rAnim, AnimationData* ad)
{
    if (!rAnim || !ad || !rAnim->layer.valid()) return NULL;
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return NULL;            // garbage/dangling guard, as in ClipPin
    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay || !lay->addList.valid()) continue;
        unsigned int n = lay->addList.size();
        if (n > 64) continue;
        for (unsigned int ai = 0; ai < n; ++ai)
        {
            AnimationClassBase::SingleAnimation* sa = lay->addList[ai];
            if (sa && sa->animationData == ad) return sa;
        }
    }
    return NULL;
}

static void ReleaseRideCombatHost(Character* rider, AnimationClass* rAnim,
                                  AnimationData* host, const char* name)
{
    if (!rAnim || !host || !name || !name[0]) return;
    AnimationClassBase::SingleAnimation* sa = RideSwingFindEntry(rAnim, host);
    if (sa)
    {
        sa->weight = 0.0f;
        sa->desiredWeight = 0.0f;
        sa->stillWanted = false;
        if (sa->mainState) sa->mainState->setWeight(0.0f);
    }
    rAnim->stopAnimation(name);
    if (rider) rider->endSlaveAnim(name);
}

// 🔑 THE trip-21 fix.  The pinned 'mid blow' entry outlives its window with currentFrameTime01
// intact, so without this every swing after the first starts wherever the last one stopped (trip 18:
// window 2 resumed at 0.518 where window 1 stopped at 0.517) - an arbitrary middle slice, and the
// reason it read as 「右手给左手割腕」 rather than as a chop.  Called once per window, at the request
// site, before runAnimation.
//   * currentFrameTime / currentFrameTime01 are the fields the engine's own update drives, so they
//     are what actually decides which frame plays; zeroing them IS the restart.
//   * speed is written here as well as passed to runAnimation, because runAnimation only sets it when
//     it creates the entry - and after the first window the entry already exists.
//   * mainState->setTimePosition is belt-and-braces for ONE frame: the engine re-derives it from
//     currentFrameTime01 next update, but this way even the first rendered frame of the window is the
//     start of the clip instead of the stale pose.  Same pointer, same NULL check, same footing as
//     ClipPin's mainState->setWeight (:6442) - the only write in here that reaches the render side.
static bool RideSwingRestart(AnimationClass* rAnim, AnimationData* blow, float speed)
{
    __try
    {
        AnimationClassBase::SingleAnimation* sa = RideSwingFindEntry(rAnim, blow);
        if (!sa) return false;
        sa->currentFrameTime   = 0.0f;
        sa->currentFrameTime01 = 0.0f;
        sa->speed              = speed;
        if (sa->mainState) sa->mainState->setTimePosition(0.0f);
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
}

// The genuinely NEW read this trip buys (the technique-side ogre= probe above was already in the
// shipping build and trip 18 already answered it: ogre=found 17/17, en=0 17/17).  This one reads the
// clip we actually pin, which no probe has ever looked inside:
//   sp=   did the speed write survive the engine's next update, or does it re-impose 1.00?
//   t01=  on the OPEN line this is the leftover the restart is there to erase (trip 18's ~0.52);
//         on the CLOSE line it is where the clip really got to.
//   olen= 'mid blow's true length, straight from Ogre.  Trip 18 could only DERIVE ~4.8 s from two
//         prog readings and a stopwatch; this prints it, and every window-length number above is
//         only as good as that estimate.
// ⚠️ Read-only, and deliberately reads the SAME fields RideSwingRestart writes - that is what makes
// the close line a verdict on the fix rather than a restatement of the intent.
static void RideSwingProbePin(AnimationClass* rAnim, AnimationData* ad, char* out, size_t cap)
{
    if (!out || cap == 0) return;
    out[0] = 0;
    __try
    {
        AnimationClassBase::SingleAnimation* sa = RideSwingFindEntry(rAnim, ad);
        if (!sa) { _snprintf_s(out, cap, _TRUNCATE, "pinst=none"); return; }
        // ⚠️ EVERY key here is spelled differently from RideSwingProbeState's (en=/w=/t=/len=) even
        // where it means the same thing, and the leading key is `pinst=` rather than `pin=` because
        // the open line ALREADY carries pin='<clip name>'.  Both probes land on ONE log line, and
        // HISTORY §U's parsing trap is exactly this: a repeated key makes a kv() reader silently
        // return the wrong value.  Splitting on " | " is the judge's job, unique keys are the belt.
        _snprintf_s(out, cap, _TRUNCATE, "pinst=live sp=%.2f pw=%.3f pdw=%.3f pt=%.3f t01=%.3f psw=%d",
                    sa->speed, sa->weight, sa->desiredWeight,
                    sa->currentFrameTime, sa->currentFrameTime01, sa->stillWanted ? 1 : 0);
        size_t used = strlen(out);
        if (used + 1 >= cap) return;
        if (!sa->mainState) { _snprintf_s(out + used, cap - used, _TRUNCATE, " ostate=none"); return; }
        _snprintf_s(out + used, cap - used, _TRUNCATE, " oen=%d ow=%.3f ot=%.3f olen=%.3f",
                    sa->mainState->getEnabled() ? 1 : 0, sa->mainState->getWeight(),
                    sa->mainState->getTimePosition(), sa->mainState->getLength());
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { _snprintf_s(out, cap, _TRUNCATE, "pinst=AV"); }
}

// The once-per-frame owner of the window.  Called from CombatAndForceDismountPass and nowhere else,
// for the reason in RideStanceRedraw's header: HaltAndForceSitPass is the last writer before render
// and owns the pose pins, so an animation mutation belongs in the same pass - and at the same point
// in the frame - where the P41E ladder has been calling runCombatAnimation all along.
//
// No SEH in this function: it builds std::string for its log lines, which C2712 forbids alongside
// __try.  Every engine call it makes goes through one of the shells above, which is why they exist.
//
// ⚠️ ONE clock, and it times ATTEMPTS, not successes: chooseAttack is a deep engine call and this
// runs in the shipping path, so asking once per kRideSwingMinGapMs (rather than once per frame
// whenever the range test fails) is what keeps the cost bounded.  A swing may therefore be up to
// that interval late, which is invisible next to a 3 s window.
// `host` / `hostNm` are the single resolver's answer for this transaction frame.  The caller
// resolves the rider-local table once; open latches atkIdx before the first attack frame, and
// close probes and releases that same selected pointer.
static void RideSwingPass(Character* rider, Character* mount, AnimationClass* rAnim, bool stance,
                          AnimationData* host, const char* hostNm)

{
    if (!rider) return;
    RideCombatRuntime& rt = GetRideCombatRuntime(rider, mount);
    // All latches below are owned by rt.  This pass is called once for each mounted rider,
    // and must never reset a different rider merely because update order changed.

    DWORD now  = GetTickCount();
    // Freeze every wall-clock anchor owned by this rider while the game is paused.  Returning
    // alone would merely defer the same large elapsed delta to the first frame after unpause.
    if (ou && ou->isPaused())
    {
        if (rt.pauseTick == 0) rt.pauseTick = now;
        return;
    }
    if (rt.pauseTick != 0)
    {
        DWORD pausedMs = now - rt.pauseTick;
        if (rt.openTick != 0) rt.openTick += pausedMs;
        if (rt.lastAttemptTick != 0) rt.lastAttemptTick += pausedMs;
        rt.pauseTick = 0;
    }
    // Window duration follows the selected technique (P4-6af-3): the clip's own length at ground
    // speed plus the follow-through hold.  `prog=` is informational; the HIT beat is still the
    // transaction clock's fixed kRideSwingHitMs, and the close is the window computed below.
    float prog = (rAnim && host) ? rAnim->getAnimationProgress(host) : -1.0f;
    // 🆕 T28 - `stance` belongs in the OPEN predicate, not only in the open DECISION.  The call site has
    // always said "the pass has to be able to CLOSE a window it opened, and stance=false is what closes
    // it" (the reason this pass is called unconditionally), but the predicate below only ever looked at
    // the clock, so a fight that ended mid-window left the window nominally open: the mask kept freeing
    // the two arm bones and RideSwingArmPose kept authoring them with the guard already gone from the
    // body.  Trip 25 caught exactly that - 32 frames of arm=536 swfree=536 against hostkeep=504, and
    // `pinst=none` on the close line - which is trip 17's hostless-skeleton disease for half a second.
    // ⚠️ Safe against flicker BY CONSTRUCTION: this pass reads the stance with advance=false, and that
    // form is sticky (raw || per-rider holdMs > 0), so a one-frame dropout of the raw fight
    // test cannot cut a window short.  Only the hold running out can, which is the intended meaning.
    // 🆕 P4-6af-3: the window is the CLIP's own length plus the follow-through hold
    // (RideSwingWindowMs), so the close edge lands after the stroke has finished playing - not
    // 1000 ms into a 2.83 s ground technique.  `prog=` above stays informational.
    const int winMs = RideSwingWindowMs(rt);
    bool  open = (rt.openTick != 0)
               && stance
               && ((now - rt.openTick) < (DWORD)winMs);

    // ---- close edge ---------------------------------------------------------------------------
    if (rt.wasOpen && !open)
    {
        DWORD ms = now - rt.openTick;   // ⚠️ read BEFORE the reset on the next line
        rt.openTick = 0;
        // 🆕 T28: the capture dies with the window.  Keeping it would rotate the NEXT window off a pose
        // the host has since left, which is the one way this model can produce a jump - and it would do
        // it silently, because every self-check here is relative to the capture.
        RideSwingEnd(rAnim);
        // Ungated and budgeted, the P43RD discipline: this CHANGES game state, and state-changing
        // events are never debugContinuous-gated. `hostkeep=` is the count of frames the pin pass saw
        // a live window and kept this transaction's latched vanilla attack host requested at full
        // weight. It retains the T27 field name for log compatibility; it no longer means guard.
        // hostkeep=0 with n>0 means the window never reached the pin sites. `prog=` and the pin probe
        // describe that same attack entry. `ogre=` still probes the selected COMBAT TECHNIQUE name,
        // which is diagnostic only and is not the host. `ms=` near kRideSwingWinMs is expected.
        // `rst=` counts one serial-latched restart decision per transaction, so rst must equal swing.
        // 🆕 T28: `noref=` is the arm's own refusal count - frames that wanted to author but could not
        // resolve BOTH bones.  It must be 0: the two are freed by the same two resolve flags they are
        // authored by, so a nonzero reading means a freed bone went unwritten (§17.9 at bind).
        // 🆕 P4-6: `hit=` closes the loop between the window and its resolution - 1 the engine called
        // a hit, 0 it dodged, -1 no attempt was made (target down / fault / window ended before
        // kRideSwingHitMs - the stance dropping mid-window is the ordinary one of those).
        int hitv = -1;
        if (rt.hitResolved)
        {
            // hit>0 hit, 0 dodge - read straight off the LAST resolution's outcome, which is this
            // window's because RideSwingDamage only ever runs inside a live window and the counters
            // advance with it.  -1 = attempted but unresolved (shell fault), -2 = not attempted.
            hitv = (rt.lastHit == 0) ? 0 : ((rt.lastHit > 0) ? 1 : -1);
        }
        else ++rt.hitSkips;
        if (rt.logLines < kRideSwingLines)
        {
            ++rt.logLines;
            char pr[96];
            RideSwingProbeState(rAnim, rt.techniqueName, pr, sizeof(pr));
            char pn[192];
            RideSwingProbePin(rAnim, host, pn, sizeof(pn));
            char b[640];
            _snprintf_s(b, 640, _TRUNCATE,
                "Riding: P43SW close rider=%p n=%d prog=%.3f ms=%u rst=%d drv=%d armt=%.2f noref=%d hold=%d "
                "fit=%d hitms=%d mix=%d hostkeep=%d tech=%d skip=%d noclip=%d hit=%d | pin='%s' %s | tech='%s' %s f=%u",
                (void*)rider, rt.swingCount, prog, (unsigned)ms, rt.restarts, rt.driveFrames,
                rt.armT, rt.noRef, rt.holdN, rt.fitMs, rt.hitMs, rt.mixIdx, rt.hostKeepFrames,
                rt.techCount, rt.skipCount, rt.noClipCount, hitv, hostNm ? hostNm : "",
                pn, rt.techniqueName, pr, gP3Frames);
            DebugLog(std::string(b));
        }

        // ⚠️ AFTER the log block and OUTSIDE its budget.  The close line's tech= en=/w=/t= block is
        // the entire verdict on T22's drive, so undriving first would print the zeros this call
        // writes; and a ride that has spent its 24 lines must still hand the state back.
        RideSwingUndrive(rAnim, rt.techniqueName);
        // Soft handback on the ordinary close edge (P4-6T).  Hard stopAnimation tore the attack
        // entry out on the SAME frame that last pinned it, so render saw a hostless skeleton for
        // one or more frames - the 「做完动作顿一下」 hitch.  Zero the weights (opens ClipPin's
        // global door) but leave the entry; the next window RideSwingRestart reuses it.  Dismount
        // and live-wipe still call ReleaseRideCombatHost (hard stop + endSlaveAnim).
        // P4-6U: authored-arc windows also drop the manual arm on this edge.
        RideSwingArmRelease(rAnim);
        // 🆕 P4-6af: nothing to hand back or re-assert here.  The window never swapped the base
        // host (the stance base is 'guard 1h' before, during and after a window), so the close
        // edge only has to stop the technique's Ogre state - which RideSwingUndrive just did.  The
        // guard's upper-body mask entries return to 1.0 on the next LegMaskApply (swingYield = 0
        // once techWin goes false), and both animation passes re-pin the guard every frame, so
        // there is no hostless frame for the P4-6T 「顿一下」 to come back through.
        (void)host; (void)hostNm;

        rt.phase = RIDE_COMBAT_RECOVERY;
        RideCombatSessionClear(rt);

        rt.wasOpen = false;
        rt.atkIdx  = -1;
        // The target and technique are borrowed engine objects captured at open.  Release them
        // at the close edge so a later fault or rider switch can never reuse an old window's
        // pointers.  The aggregate hit counters intentionally remain per-ride diagnostics.
        rt.lastHit = -2;
        // ⚠️ Return on the close frame instead of falling through to the open decision.  Trip 10's
        // hard finding is that a one-shot is only followed by the loop coming back if WE re-request
        // it, so the frame after a close has to be a guard frame - otherwise back-to-back swings
        // would keep the stance from ever being rebuilt, and the "did it come back" criterion (the
        // NEXT open row) would have nothing to read.
        return;
    }
    rt.wasOpen = open;
    // ---- 🆕 P4-6: the window's hit resolution, on the window's own clock --------------------
    // Runs BEFORE the `if (open) return` below, i.e. once per frame while a window is live.  The
    // gate is elapsed time (NOT RideSwingArmPose's t - that is authored in the render pass, which
    // runs twice per frame through the two update hooks; this clock ticks exactly once), and
    // gRideSwingHitDone is latched BEFORE the call so a faulting call cannot turn into a per-frame
    // retry - one window, one resolution, same budget discipline as the open attempt itself.
    // 🆕 P4-6ag: the beat scales with the clip (see kRideSwingHitFrac).  fitMs is measured by
    // Drive a frame or two after the open and is stable for the rest of the window, so reading it
    // here is the same number the close line prints as fit=.  Unknown length (no Ogre state) =>
    // the old fixed constant, i.e. exactly the beat trip 42 validated.
    int hitMs = kRideSwingHitMs;
    if (rt.fitMs > 0)
    {
        int scaled = (int)((float)rt.fitMs * kRideSwingHitFrac + 0.5f);
        if (scaled > hitMs) hitMs = scaled;
    }
    rt.hitMs = hitMs;   // logged on the close line so the beat is auditable per window
    if (open && !rt.hitResolved
        && RideCombatSessionOwns(rt, rider, mount, rt.target, rt.technique)
        && (now - rt.openTick) >= (DWORD)hitMs)
    {
        rt.hitResolved = true;
        rt.phase = RIDE_COMBAT_HIT_RESOLVED;
        int hit = RideSwingDamage(rider, rt.target, rt.technique);
        rt.lastHit = hit;   // per-window outcome for the close line's hit= field
        if      (hit > 0) ++rt.hits;
        else if (hit == 0) ++rt.misses;
        else
        {
            // Not attempted: the target died mid-window, or a pointer the shell could not trust.
            // Counted and (budgeted) named, because a ride of miss=0 hit=0 with swing>0 has to be
            // able to say WHY (T18's P43FT lesson: "never fired" without a distance is undiagnosable).
            ++rt.hitSkips;
            if (rt.hitLines < kRideSwingHitLines)
            {
                ++rt.hitLines;
                char stline[192];
                _snprintf_s(stline, 192, _TRUNCATE,
                            "Riding: P43ST rider=%p n=%d skip=%s f=%u",
                            (void*)rider, rt.swingCount,
                            RideHitSkipReasonName(rt.hitSkipReason), gP3Frames);
                DebugLog(std::string(stline));
            }
        }
    }
    if (open) return;                  // one swing at a time; the guard keeps the torso meanwhile


    // ---- open decision ------------------------------------------------------------------------
    if (!stance) return;               // the stance is the gate for everything on this route, so a
                                       // big-mount rider can never reach here (MountCombatEligible
                                       // runs first inside it - 🆕 P4-5: nor can a 坐坐垫 rider, by
                                       // the same one gate) and neither can a rider out of a
                                       // fight - the same gate the suppressor and the re-draw use.
    // ⚠️ A paused game must not open a window: a pause freezes exactly what the interval measures
    // (same discipline as the stance tail and ServicePendingMounts).
    if (ou && ou->isPaused()) return;
    if (rt.lastAttemptTick != 0 && (now - rt.lastAttemptTick) < (DWORD)rt.gapMs) return;
    // Pace only the recovery/attempt interval. The authored arc, live window, hit timing
    // and bone hand-back remain unchanged; a fast rider spends less time idle.  The cached
    // gate avoids calling into CharStats every frame; it is refreshed only when an attempt
    // is due (the same cadence that already bounds chooseAttack).
    rt.attackSpeed = RideSwingAttackSpeed(rider);
    rt.gapMs = RideSwingGapMs(rt.attackSpeed);
    if (rt.lastAttemptTick != 0 && (now - rt.lastAttemptTick) < (DWORD)rt.gapMs) return;
    rt.lastAttemptTick = now;          // spent by the ATTEMPT, so a persistent skip or a faulting
                                       // call cannot turn into a per-frame retry

    RideSwingPick pick;
    if (!RideSwingChoose(rider, mount, &pick)) return;
    ++rt.techCount;

    // Do not let ground-only technique metadata select a different mounted behaviour.  In the
    // live two-rider test it produced lim=14 from martial-arts picks while both targets were
    // 19..22 units from their riders: stance stayed up, but no authored slash could open.  The
    // mounted state has one stable engagement envelope instead.  Keep the weapon reach as a
    // floor for unusually long weapons, and record the selected technique only for diagnostics
    // and the engine's hit resolution below.
    float lim = kRideSwingEngageDist;
    if (pick.reach > 0.01f && lim < pick.reach) lim = pick.reach;
    rt.lastLimit = lim;
    if (rt.minDistance < 0.0f || pick.d < rt.minDistance) rt.minDistance = pick.d;
    if (pick.d > lim)
    {
        ++rt.skipCount;
        // Budgeted, same family as the open/close rows: without this the only trace of a
        // never-opening ride is limlast= on the summary, which cannot name the gate technique.
        if (rt.skipLines < kRideSwingSkipLines)
        {
            ++rt.skipLines;
            char b[320];
            _snprintf_s(b, 320, _TRUNCATE,
                "Riding: P43SW skip rider=%p n=%d tech='%s' gate='%s' init=%.2f minS=%.2f "
                "lim=%.2f d=%.2f reach=%.2f aspd=%.3f f=%u",
                (void*)rider, rt.skipCount, pick.name, pick.gate, pick.init, pick.minS,
                lim, pick.d, pick.reach, rt.attackSpeed, gP3Frames);
            DebugLog(std::string(b));
        }
        return;
    }

    // Lock this window's host before openTick becomes visible to either animation pass.
    ResolveRideCombatClips(rt, rAnim);
    // 🆕 P4-6ag: pick this window's slot in the clip mix (per rider, one slot per window).  The
    // engine's technique stays the HIT's subject; only the visible clip name changes.
    if (!SelectRideCombatAttack(rt, rAnim)) { ++rt.noClipCount; return; }
    const char* clipName = (rt.mixIdx >= 0 && rt.mixIdx < kRideMixCount)
                         ? kRideMixClips[rt.mixIdx] : pick.name;
    host = RideCombatHost(rt, &hostNm);
    (void)host;   // may be NULL - the technique Ogre state is the visual
    AnimationData* attackHost = host;
    (void)attackHost;

    // Technique clip name BEFORE openTick so LegPosePass can Drive on the first window frame.
    _snprintf_s(rt.techniqueName, sizeof(rt.techniqueName), _TRUNCATE, "%s", clipName ? clipName : "");
    if (!rt.techniqueName[0]) { ++rt.noClipCount; rt.atkIdx = -1; return; }

    rt.openTick = now;
    rt.wasOpen  = true;
    ++rt.swingCount;
    rt.holdN    = 0;   // T23: per WINDOW, or a window whose state came back absent would
    rt.fitMs    = 0;   // report the previous window's hold count (T24: and its fit=) on its
    rt.armT     = -1.0f;   // close line.  T25: armt= likewise - it is the arc's own progress,
                                  // so carrying it over would report the last window's completion
    rt.noRef    = 0;   // 🆕 T28: same rule for noref=, and belt-and-braces for the capture flag -
    rt.refHave  = false;   // the close edge clears it, but an open that follows a ride reset
                                  // (Mount / RestoreRideAfterLoad) never saw a close edge at all.
    // 🆕 P4-6: the resolution's subjects, captured at open (RideSwingChoose's threat - the same one
    // the arc was aimed at, not a re-ask at hit time which could name someone else) and its per-
    // window state.  Same reset rule as the fields above: an open that follows a ride reset never
    // saw the close edge that would normally clear these.
    rt.target      = pick.tgt;
    rt.technique  = pick.tech;
    rt.hitResolved  = false;
    rt.geometryHave = (pick.tgt != NULL);
    rt.targetOpenPos = pick.tgt ? pick.tgt->getPosition() : Ogre::Vector3::ZERO;
    rt.riderOpenPos  = rider->getPosition();
    // The hit uses the same mounted envelope that opened the stroke.  This is not a ground
    // footwork allowance: it is the rider-to-target distance while the rider sits behind the
    // mount's front half.  The target's own radius and the sector check still apply.
    rt.targetOpenLimit = lim;
    rt.targetOpenDistance = pick.d;
    rt.hitSkipReason = RIDE_HIT_SKIP_NONE;
    rt.phase = RIDE_COMBAT_SWINGING;
    rt.lastHit  = -2;
    RideCombatSessionBegin(rt, rider, mount, pick.tgt, pick.tech, now);
    // techniqueName already latched above (before openTick).

    host = RideCombatHost(rt, &hostNm);
    if (rt.logLines < kRideSwingLines)
    {
        ++rt.logLines;
        char pr[96];
        RideSwingProbeState(rAnim, rt.techniqueName, pr, sizeof(pr));
        char pn[192];
        RideSwingProbePin(rAnim, host, pn, sizeof(pn));
        char b[640];
        _snprintf_s(b, 640, _TRUNCATE,
            "Riding: P43SW open rider=%p n=%d tech='%s' gate='%s' mix=%d clip='%s' dq=%.2f init=%.2f "
            "minS=%.2f lim=%.2f d=%.2f reach=%.2f aspd=%.3f gap=%d | pin='%s' %s | pre %s f=%u",
            (void*)rider, rt.swingCount, pick.name, pick.gate, rt.mixIdx,
            rt.mixIdx >= 0 ? kRideMixClips[rt.mixIdx] : "(engine)",
            pick.dq, pick.init, pick.minS, lim, pick.d,
            pick.reach, rt.attackSpeed, rt.gapMs,
            hostNm ? hostNm : "", pr, pn, gP3Frames);
        DebugLog(std::string(b));
    }
}

// ---- P4-3-2: kill the re-sheathe instead of compensating for it -----------------------------
// P4-3 step 1 named two writers, both engine trunks: Character::_ragdollMode (real=16 per ride,
// median gap 22 frames) and Character::_carryMode(on=true) (real=3, ~once per mount), addresses
// in RE_NOTES §18.10.  Neither may be hooked - §18.10 calls them the two trunks that maintain
// the carried state - and re-drawing the weapon after each sheathe is a write-side servo, which
// HISTORY §B bans outright ("对绝对覆写别做写入端补偿，直接消灭覆写者本身").  §B's own
// prescription is what this is: the sheathe is a virtual call through CharacterHuman vtable
// +0x2D0 (§18.6), so hooking THAT and declining to run the engine body destroys the overwriter
// with no servo anywhere.
//
// Three properties of the filter, each load-bearing:
//   * STATE-based, never caller-address-based.  Gating on _ReturnAddress would import the
//     unexplained RUNTIME_RVA_DELTA (+0xA90, RE_NOTES §18.9) into shipping code; naming is
//     already done, so the site is not even logged (`git show 61872dc` for the formatter).
//   * `stance`, not "is mounted".  The mount-time sheathe comes from _carryMode(on=true) at a
//     moment when stance is necessarily FALSE (not in combat mode, no threat inside
//     kRideThreatDist), so the engine's own carry setup is never touched - only the in-combat
//     repeats are.  Putting the weapon away on boarding is correct behaviour and stays.
//   * self-clearing.  Once the release tail (kRideStanceHoldMs) runs out the next sheathe call
//     passes straight through and the weapon goes away by itself: there is no restore path to
//     write, and nothing to leak if a ride ends abnormally.
// ⚠️ Known invariant violation, tracked riders only: weaponInHands (0x6D8) stays non-NULL while
// the rider is carried.  Nothing in the decoded part of sheatheWeapon asserts against that
// (§18.7), but "nothing in the part we decoded" is not "nothing anywhere" - that is precisely
// what this trip has to answer, alongside how a KO/ragdoll looks with the blade still in hand
// (cosmetic at worst: P4-4 force-dismounts on rider down).
static const int kShSupLines = 10;   // per-ride line budget; NOT debugContinuous-gated

// The map/stance half.  Separate from the SEH shell below because it holds iterators (objects
// with destructors), which C2712 forbids in a function containing __try.
static bool RideSheatheSuppressedImpl(Character* rider)
{
    if (!rider) return false;
    RideStanceDrawRuntime* drawRt = FindRideStanceDrawRuntime(rider);
    // P4-3-3: our own drawWeapon is on the stack.  The suppressor exists to kill the engine's
    // PERIODIC re-sheathe (_ragdollMode / _carryMode); if drawWeapon puts the old weapon away as
    // part of the swap, swallowing that would be us fighting a request we made ourselves.  Checked
    // before the map lookup so the pass-through cannot be lost to a missing seat entry.
    if (drawRt && drawRt->busy) return false;
    boost::unordered_map<Character*, Character*>::iterator it = riderToMount.find(rider);
    if (it == riderToMount.end() || !it->second) return false;
    boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(it->second);
    if (sit == mountSeat.end()) return false;
    // advance=false = pure read: HaltAndForceSitPass owns the once-per-frame hold counter, and
    // this hook fires from the engine's own call graph at an arbitrary point in the frame.
    // Precedent for the non-advancing call from a non-per-frame context: :5664.
    bool stance = RideCombatStance(rider, it->second, sit->second, false);
    // A successful stance-edge draw gets a short post-stance grace.  Without it the engine can
    // end combat on the very next update, call sheatheWeapon, and make a real wpn=0->1 draw
    // effectively invisible to the player.  DWORD subtraction is wrap-safe; an expired grace
    // falls through to the original engine behavior.
    DWORD now = GetTickCount();
    bool keepDrawn = (!stance && drawRt && drawRt->keepUntil != 0
                    && (int)(drawRt->keepUntil - now) > 0);

    RideCombatRuntime& combatRt = GetRideCombatRuntime(rider, NULL);
    const char* sh  = "?";
    Weapon*     wih = RiderWeaponInHands(rider, &sh);
    if (!stance && !keepDrawn) { ++combatRt.shPass; return false; }
    if (wih) ++combatRt.shSupReal; else ++combatRt.shSupNoop;

    // Unconditional, budgeted: this SUPPRESSES an engine action, and state-changing events are
    // never debugContinuous-gated (same discipline as `LEGPOSE takeover` / `force dismount`).
    if (combatRt.shSupLines < kShSupLines)
    {
        ++combatRt.shSupLines;
        char b[320];
        _snprintf_s(b, 320, _TRUNCATE,
            "Riding: P43SUP skip wih=%d sh='%s' cm=%d bc=%d keep=%d real=%d noop=%d pass=%d f=%u",
            wih ? 1 : 0, sh ? sh : "",
            rider->isInCombatMode(true, true) ? 1 : 0,
            rider->_isBeingCarried            ? 1 : 0,
            keepDrawn ? 1 : 0,
            combatRt.shSupReal, combatRt.shSupNoop, combatRt.shPass, gP3Frames);
        DebugLog(std::string(b));
    }
    return true;
}

// Fail OPEN on an access violation: letting the engine sheathe is always a valid game state,
// swallowing the call on a rider we could not even read is not.
static bool RideSheatheSuppressed(Character* rider)
{
    __try { return RideSheatheSuppressedImpl(rider); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
}

// The draw half of P4-3-3 (state and reasoning at the gStanceDraw* block).  Called at most once
// per 0 -> 1 stance edge and never on any other trigger.
//
// ⚠️ Deliberately NOT called from HaltAndForceSitPass, which is where the edge is detected: that
// pass is the last writer before render and owns the pose pins, so an equipment+animation mutation
// belongs in CombatAndForceDismountPass instead - the same pass, and the same point in the frame,
// where the P41E ladder has been calling drawWeapon all along.
//
// No SEH here: its caller holds map iterators, which C2712 forbids in a function containing __try,
// and every dereference below is either a tracked-rider pointer the looks-live sweep already
// validated this frame or an engine return value that is null-checked.  Same footing as the ladder.
static void RideStanceRedraw(Character* rider)
{
    if (!rider) return;
    RideStanceDrawRuntime& drawRt = GetRideStanceDrawRuntime(rider);
    if (rider->getCurrentWeapon()) return;             // already armed - the suppressor holds it
    if (drawRt.fail >= kStanceDrawFails) return;       // refused before; stop hammering

    // getPrimaryWeapon() reads the equipment slots, so it answers while the weapon is sheathed -
    // which is the whole point here, and the reason getThePreferredWeapon() is not used (NULL in
    // every P4-1d read).  Weapon -> Item is offset-0 single inheritance the whole way down.
    Inventory* rInv = rider->getInventory();
    if (!rInv) return;
    Weapon* sw = rInv->getPrimaryWeapon();
    if (!sw) sw = rInv->getSecondaryWeapon();
    if (!sw)
    {
        // An unarmed rider is a legitimate state, not a refusal: it must not spend the failure
        // budget, or a fist-fighter would burn the cap and a later real refusal would go unread.
        ++drawRt.noWpn;
        return;
    }

    // The sheath name has to be COPIED across the call - the const char* aliases the engine's
    // std::string and drawWeapon is entitled to reallocate it (P4-1h learned this the hard way).
    const char* shPreP = "?";
    Weapon* wihPre = RiderWeaponInHands(rider, &shPreP);
    std::string shPre(shPreP ? shPreP : "");

    drawRt.busy = true;
    std::string sheathArg = RideSheathSlotFor(rider);   // NOT std::string() - see RideSheathSlotFor
    rider->drawWeapon(reinterpret_cast<Item*>(sw), sheathArg);
    drawRt.busy = false;

    const char* shPost = "?";
    Weapon* wihPost = RiderWeaponInHands(rider, &shPost);
    bool ok = rider->getCurrentWeapon() ? true : false;
    if (ok)
    {
        ++drawRt.ok;
        drawRt.keepUntil = GetTickCount() + (DWORD)kStanceDrawKeepMs;
        if (gDrawTries > 0) --gDrawTries;   // success spends budget; refusal does not
    }
    else ++drawRt.fail;

    // Unconditional, budgeted: this CHANGES game state, and state-changing events are never
    // debugContinuous-gated (same discipline as `LEGPOSE takeover` / `force dismount` / P43SUP).
    // `post=` is the self-proving field - post=0 on every line means the draw is being refused in
    // the stance and the fix bore no load, exactly the way real=0 would invalidate the suppressor.
    if (drawRt.lines < kStanceDrawLines)
    {
        ++drawRt.lines;
        AnimationClass* dAnim = rider->getAnimationClass();
        char b[352];
        _snprintf_s(b, 352, _TRUNCATE,
            "Riding: P43RD edge post=%d wih=%d->%d sh='%s'->'%s' aCW=%d cm=%d "
            "ok=%d fail=%d left=%d f=%u",
            ok ? 1 : 0, wihPre ? 1 : 0, wihPost ? 1 : 0,
            shPre.c_str(), shPost ? shPost : "",
            dAnim ? (int)dAnim->animationRequirements.currentWeapon : -1,
            rider->isInCombatMode(true, true) ? 1 : 0,
            drawRt.ok, drawRt.fail, gDrawTries, gP3Frames);
        DebugLog(std::string(b));
    }
}

// 🆕 P4-6ah (2026-09-12, user: 「打着打着人物就头朝后面去了…能不能让人物战斗时就用原版的索敌，
// 招式都往敌人身上打」).  COMBAT FACING comes back - but at the NODE, not the spine.
// Why not the spine: the technique's own torso tracks are live (the free list hands them the spine)
// and they carry the stroke's whip; masking them to twist the spine was tried in P4-1M and reads
// stiff.  Why the node works: the rider's node is world-space and the whole skeleton hangs off it,
// so ONE yaw turns the body - and every clip, authored to strike "forward", then strikes toward
// the enemy.  Measured torso yaw per mix clip (skelanims.py, relative to the BODY): `chop left`
// -2..+2 deg, `chop down*` -9..+16, `heavy downcut` -29..0, `bigchopv2` -29..+18,
// `downward combo` -42..0 - i.e. the clip's aim is a rotation of the body, so a body that faces
// the mount's travel heading instead of the enemy puts the whole stroke on the wrong side.
// ⚠️ The cap is real and deliberately modest: this yaw rotates the STRADDLE too (the legs hang off
// the same node while the mount is a different node), so a large angle sits the rider sideways on
// the animal.  60 deg keeps the legs near the flanks while covering every ordinary fight; raise it
// only if the user reports the aim still falling short.
static const float kRideCombatFaceMaxDeg = 60.0f;
// 🆕 P4-6ak (2026-09-12, user: 「那个姿势没了。但是人物在马上有时会突然胡乱转向」).
// The yaw above was applied DIRECTLY every frame, from a fresh `RideNearestThreat` answer.  That
// function is a PRIORITY search whose tiers are the engine's own books - rider target, rider
// ATTACKERS, mount target, mount ATTACKERS - and those lists churn: a declared target can sit
// 200 u away, an attacker list can hold stale entries, and the "nearest live entry" inside a tier
// changes the instant any of them moves.  Any of those flips re-aimed the rider in one frame,
// which is the reported 「突然胡乱转向」.  Three brakes, all of them inside this one block:
//   1) DISTANCE - only a threat inside kRideThreatDist (60 u, the stance's own gate) may aim us.
//      Without it, a book entry hundreds of units away turned the rider.
//   2) HOLD - once a frame's enemy is chosen, KEEP it while it stays a live threat inside that
//      radius; only then ask the search again.  This is what stops two attackers from trading the
//      facing every frame (each of them is "nearest" as soon as the other twitches).
//   3) RATE - the applied angle ramps toward the wanted one at kRideCombatFaceRateDegPerSec with a
//      per-frame ceiling, so even a genuine target switch is a turn instead of a snap.  The
//      ceiling also covers the pause case: a paused game keeps GetTickCount running, and an
//      unclamped ms delta would hand back the whole paused duration in one step.
static const float kRideCombatFaceRateDegPerSec = 240.0f;  // 0 -> 60 deg in 250 ms
static const float kRideCombatFaceMaxStepDeg    = 15.0f;   // per frame, whatever the clock says
// 🆕 P4-6al: the hold's radius has hysteresis, and a genuinely nearer enemy still wins.
static const float kRideCombatFaceHoldDist      = kRideThreatDist * 1.2f;  // 72 u
static const float kRideCombatFaceSwitchFrac    = 0.5f;    // switch when the candidate is 2x nearer
// 🆕 P4-6al: the facing-jump diagnostic.  Logs the FIRST few events of a ride (a budget, like
// every other probe) whenever the heading OR the wanted offset moves more than this in one frame.
static const float kRideFaceJumpDeg   = 25.0f;
static const int   kRideFaceJumpLines = 6;

// Orient the rider's scene node after the game's own update.  The carry physics pins
// the rider horizontally (lying flat, belly up); we override the render-node
// orientation so the seated pose is drawn upright.  Facing is ALWAYS the mount's
// direction of travel - never a head/body bone, never a manual tune (dropped
// 2026-08-23); the node is world-space so no bone-local conversion is needed.
// ⚠️ P4-6ah adds ONE exception: while the combat stance is up, facing is the ENEMY (see below).
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
    // 🆕 P4-6am: the heading is a CONTINUOUS quantity.  Until now it was HOLD-or-REPLACE: a travel
    // delta that failed the veto left the last heading untouched, and the first delta that passed
    // replaced it outright.  The P43FC probe measured what that costs (2026-09-12 session,
    // logs\RE_Kenshi_log_20260912_p46al.txt): hdveto=21544 on ONE ride - the veto fired on roughly
    // a third of all frames - and every release came back as a 30-60 deg jump (hdjump=60 x6).
    // That is BOTH user reports in one mechanism: after a fight the heading stays STUCK on a stale
    // direction (「退出战斗后朝向有时不跟随动物前进方向」) and when it finally moves it JUMPS
    // (「突然向后转」).
    // ⇒ the heading is now ROTATED toward the best available answer, never assigned, and the
    //   answer itself is never allowed to be a lie.  Sources, in order of preference:
    //   (a) the travel delta, while the animal's own facing agrees it is going that way (an animal
    //       walks where it points, so this is the ordinary frame);
    //   (b) the animal's own facing - getFacingDirection() is the engine's non-virtual answer and,
    //       unlike the delta, a shove or knockback cannot make it lie.  This is what keeps the
    //       heading LIVE instead of frozen while the delta keeps failing (a);
    //   (c) nothing new -> keep rotating toward the last answer.
    static const float kHeadingMoveEps = 0.03f;   // per-frame horizontal move to count as "traveling"
    // The delta may only be used while it agrees with where the animal's BODY points.  0.25 ~= 75
    // deg: a real walk/turn keeps dot near 1, a sidestep/shove reads dot ~= 0 or less.
    static const float kHeadingFaceMinDot = 0.25f;
    // The held heading's turn rate, wall-clock based, plus a per-frame ceiling (which also covers a
    // paused game: GetTickCount keeps running while the game does not).
    static const float kHeadingRateDegPerSec = 360.0f;   // 0 -> 90 deg in 250 ms
    static const float kHeadingMaxStepDeg    = 20.0f;    // per frame, whatever the clock says

    Ogre::Vector3 fwd(0.0f, 0.0f, 1.0f);
    bool haveFwd = false;
    Ogre::Vector3 faceDir(0.0f, 0.0f, 1.0f);
    bool haveFace = GetMountFacingDirection(mount, faceDir);
    if (haveFace)
    {
        faceDir.y = 0.0f;
        if (faceDir.length() > 0.001f) faceDir.normalise(); else haveFace = false;
    }
    Ogre::Vector3 wantDir(0.0f, 0.0f, 1.0f);
    bool haveWant = false;
    CharMovement* mv = mount->getMovement();
    if (mv)
    {
        Ogre::Vector3 cur = mv->getPosition();
        boost::unordered_map<Character*, Ogre::Vector3>::iterator lp = mountHeadingPos.find(mount);
        if (lp != mountHeadingPos.end())
        {
            Ogre::Vector3 d = cur - lp->second;
            d.y = 0.0f;
            if (d.length() > kHeadingMoveEps)
            {
                d.normalise();
                if (!haveFace || d.dotProduct(faceDir) > kHeadingFaceMinDot)
                {
                    wantDir = d;            // (a) the animal is travelling the way it points
                    haveWant = true;
                }
                else
                {
                    RideCombatRuntime* rt = FindRideCombatRuntime(rider);
                    if (rt) ++rt->headVeto;   // the delta is a shove, not a direction
                }
            }
        }
        mountHeadingPos[mount] = cur;
    }
    if (!haveWant && haveFace) { wantDir = faceDir; haveWant = true; }   // (b) the body's own answer

    boost::unordered_map<Character*, Ogre::Vector3>::iterator hd = mountHeadingDir.find(mount);
    if (hd == mountHeadingDir.end())
    {
        if (haveWant) { mountHeadingDir[mount] = wantDir; }
    }
    else if (haveWant)
    {
        RideCombatRuntime* rt = FindRideCombatRuntime(rider);
        DWORD now = GetTickCount();
        DWORD dtms = 0;
        if (rt)
        {
            if (rt->headTick != 0) dtms = now - rt->headTick;
            rt->headTick = now;
        }
        float step = kHeadingRateDegPerSec * (float)dtms / 1000.0f;
        if (step < 0.0f) step = 0.0f;
        if (step > kHeadingMaxStepDeg) step = kHeadingMaxStepDeg;
        Ogre::Vector3 held = hd->second;
        held.y = 0.0f;
        if (held.length() > 0.001f)
        {
            held.normalise();
            float dotH = held.dotProduct(wantDir);
            if (dotH >  1.0f) dotH =  1.0f;
            if (dotH < -1.0f) dotH = -1.0f;
            float ang = Ogre::Math::ACos(dotH).valueDegrees();
            if (ang > 0.05f)
            {
                if (ang <= step || step <= 0.0f)
                    held = wantDir;
                else
                {
                    // Yaw about the world up - everything here is horizontal, and it makes the
                    // antiparallel case (a true 180) a turn rather than an undefined axis.
                    const Ogre::Vector3 up(0.0f, 1.0f, 0.0f);
                    float sgn = (up.dotProduct(held.crossProduct(wantDir)) >= 0.0f) ? 1.0f : -1.0f;
                    Ogre::Quaternion q(Ogre::Degree(step * sgn), up);
                    Ogre::Vector3 turned = q * held;
                    turned.y = 0.0f;
                    if (turned.length() > 0.001f) { turned.normalise(); held = turned; }
                    else held = wantDir;
                }
                mountHeadingDir[mount] = held;
            }
        }
    }
    {
        boost::unordered_map<Character*, Ogre::Vector3>::iterator hd2 = mountHeadingDir.find(mount);
        if (hd2 != mountHeadingDir.end()) { fwd = hd2->second; haveFwd = true; }
    }
    if (!haveFwd)
        fwd = GetMountForward(seat, mount);   // never traveled yet: body-bone forward
    fwd.y = 0.0f;
    if (fwd.length() < 0.001f)
        fwd = Ogre::Vector3(0.0f, 0.0f, 1.0f);
    else
        fwd.normalise();

    // 🆕 P4-6ah/P4-6ak: COMBAT FACING.  Out of combat the rider faces the mount's direction of
    // travel; IN A FIGHT he faces the ENEMY (the user's ask: 「战斗时就用原版的索敌」).  The enemy
    // comes from RideNearestThreat - the same four-tier search the stance aims by, i.e. the
    // engine's own books (rider target -> rider attackers -> mount target -> mount attackers).
    // P4-6ak adds the three brakes described at kRideCombatFaceRateDegPerSec: distance gate,
    // target hold, rate limit.  The yaw is applied to the NODE below (world-space, carries the
    // whole skeleton), which is what makes an ordinary "strike forward" clip strike at the enemy.
    // Read with advance=false: HaltAndForceSitPass owns the once-per-frame hold counter.
    const Ogre::Vector3 fwdHeading = fwd;   // the mount's heading, BEFORE our combat offset
    float wantDeg = 0.0f;      // desired yaw this frame; 0 = face the mount's heading
    {
        RideCombatRuntime* frt = FindRideCombatRuntime(rider);
        if (frt) frt->faceDeg = 0.0f;
        Character* foe = NULL;
        float foeD = -1.0f;
        if (RideCombatStance(rider, mount, seat, false))
        {
            // 2) HOLD: keep last frame's enemy while it is still a live, NEAR threat.  The radius
            //    has hysteresis (hold at 1.2x the acquisition gate) so a target hovering on the
            //    gate cannot make the facing flip on and off frame by frame.
            foe = frt ? frt->faceTgt : NULL;
            bool foeOk = false;
            if (foe && foe != mount && CharacterLooksLive(foe) && !foe->isDown() && !foe->isDead())
            {
                Ogre::Vector3 fd = foe->getPosition() - rider->getPosition();
                fd.y = 0.0f;
                foeD = Ogre::Math::Sqrt(fd.squaredLength());
                foeOk = (foeD <= kRideCombatFaceHoldDist);
            }
            // 1) DISTANCE: the search's answer is only allowed to aim us if it is inside the
            //    stance's own threat radius - its books hold entries hundreds of units away.
            float candD = -1.0f;
            Character* cand = RideNearestThreat(rider, mount, &candD);
            if (cand && (cand == mount || candD < 0.0f || candD > kRideThreatDist)) cand = NULL;
            // A MUCH nearer enemy is a real switch (the fight moved); anything else keeps the
            // held one, which is what stops two similar attackers from trading the facing.
            if (foeOk && cand && cand != foe && candD >= 0.0f && foeD >= 0.0f
                && candD < foeD * kRideCombatFaceSwitchFrac)
                foeOk = false;
            if (!foeOk)
            {
                foe = cand;
                foeD = candD;
                if (frt) frt->faceTgt = foe;
            }
            if (foe)
            {
                Ogre::Vector3 toFoe = foe->getPosition() - rider->getPosition();
                toFoe.y = 0.0f;
                if (toFoe.length() > 0.05f)
                {
                    toFoe.normalise();
                    float dt = fwd.dotProduct(toFoe);
                    if (dt >  1.0f) dt =  1.0f;
                    if (dt < -1.0f) dt = -1.0f;
                    // +deg turns fwd toward the rider's LEFT, because the skeleton frame is
                    // +X = left, +Y = up, +Z = forward (P2-1b), so a rotation about +Y by the
                    // signed angle atan2(up . fwd x toFoe, fwd . toFoe) lands exactly on toFoe.
                    const Ogre::Vector3 up(0.0f, 1.0f, 0.0f);
                    float cr = up.dotProduct(fwd.crossProduct(toFoe));
                    wantDeg = Ogre::Math::ATan2(cr, dt).valueDegrees();
                    if (wantDeg >  kRideCombatFaceMaxDeg) wantDeg =  kRideCombatFaceMaxDeg;
                    if (wantDeg < -kRideCombatFaceMaxDeg) wantDeg = -kRideCombatFaceMaxDeg;
                }
            }
        }
        else if (frt)
            frt->faceTgt = NULL;   // out of combat: drop the hold, so the next fight re-aims

        // 🆕 P4-6al DIAGNOSTIC: name WHY the facing jumped, budgeted.  「砍着砍着突然向后转然后转
        // 回来」 has two candidate causes and the log could not tell them apart: the mount's HEADING
        // flipping (a shove the veto let through - `hdjump=`) versus OUR target offset flipping
        // (`wantjump=`, e.g. the enemy changed or was lost).  Whichever is non-zero at the event is
        // the answer; `veto=` counts heading holds, `foe=`/`fd=` identify the target at that moment.
        if (frt && frt->faceLog < kRideFaceJumpLines)
        {
            float hdj = -1.0f;
            if (frt->faceFwdHave)
            {
                Ogre::Vector3 a = frt->faceFwd, b = fwdHeading;
                a.y = 0.0f; b.y = 0.0f;
                if (a.length() > 0.001f && b.length() > 0.001f)
                {
                    a.normalise(); b.normalise();
                    float dj = a.dotProduct(b);
                    if (dj >  1.0f) dj =  1.0f;
                    if (dj < -1.0f) dj = -1.0f;
                    hdj = Ogre::Math::ACos(dj).valueDegrees();
                }
            }
            float wj = frt->faceWantHave ? Ogre::Math::Abs(wantDeg - frt->faceWantPrev) : 0.0f;
            if (hdj > kRideFaceJumpDeg || wj > kRideFaceJumpDeg)
            {
                ++frt->faceLog;
                char fb[288];
                _snprintf_s(fb, 288, _TRUNCATE,
                    "Riding: P43FC f=%u hdjump=%.0f wantjump=%.0f want=%.0f app=%.0f foe=%p fd=%.1f veto=%d",
                    gP3Frames, hdj, wj, wantDeg, frt->faceDegCur, (void*)foe, foeD, frt->headVeto);
                DebugLog(std::string(fb));
            }
            frt->faceFwd = fwdHeading;  frt->faceFwdHave  = true;
            frt->faceWantPrev = wantDeg; frt->faceWantHave = true;
        }
        // 3) RATE: ramp the APPLIED angle toward the wanted one.  Out of combat wanted is 0, so
        //    the release is a turn too - the rider does not snap back to the mount's heading.
        if (frt)
        {
            DWORD now = GetTickCount();
            if (frt->faceTick == 0) frt->faceTick = now;
            DWORD dtms = now - frt->faceTick;
            frt->faceTick = now;
            float step = kRideCombatFaceRateDegPerSec * (float)dtms / 1000.0f;
            if (step < 0.0f) step = 0.0f;
            if (step > kRideCombatFaceMaxStepDeg) step = kRideCombatFaceMaxStepDeg;
            float diff = wantDeg - frt->faceDegCur;
            if (diff >  step) diff =  step;
            if (diff < -step) diff = -step;
            frt->faceDegCur += diff;
            if (frt->faceDegCur >  kRideCombatFaceMaxDeg) frt->faceDegCur =  kRideCombatFaceMaxDeg;
            if (frt->faceDegCur < -kRideCombatFaceMaxDeg) frt->faceDegCur = -kRideCombatFaceMaxDeg;
            float mag = (frt->faceDegCur < 0.0f) ? -frt->faceDegCur : frt->faceDegCur;
            if (mag > 0.01f)
            {
                frt->faceDeg = frt->faceDegCur;
                ++frt->faceFrames;
                if (frt->faceDegMax < (int)mag) frt->faceDegMax = (int)mag;
            }
        }
        float deg = frt ? frt->faceDegCur : 0.0f;
        if (deg > 0.001f || deg < -0.001f)
        {
            const Ogre::Vector3 up(0.0f, 1.0f, 0.0f);
            Ogre::Quaternion turn(Ogre::Degree(deg), up);
            Ogre::Vector3 turned = turn * fwd;
            turned.y = 0.0f;
            if (turned.length() > 0.001f)
            {
                turned.normalise();
                fwd = turned;
            }
        }
    }

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
// recomputed every frame so any pose change self-corrects in one frame.
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
    // and staleness (a different ride pose changes the pose constant too).
    //
    // Capture/heal bookkeeping happens ONLY at the main-loop sync: the raw relation
    // read mid-animation-phase differs by >1.5u between call sites (bones lag node
    // writes until the scene flush), which first kept rs at 0 forever, then - once
    // every character's update re-synced - made capture/stale ping-pong rewrite the
    // cfg several times a second.  One consistent measurement point fixes both.
    static const float kRelStableTol    = 0.35f;
    static const float kAnchorStaleDiff = 1.5f;
    Ogre::Vector3 rel = rBip - nodeP;

    bool captureAnchor = mainPhase;
    if (captureAnchor)
    {
        boost::unordered_map<Character*, SeatInfo>::const_iterator csi = mountSeat.find(mount);
        if (csi != mountSeat.end() && RideCombatStance(rider, mount, csi->second, false))
            captureAnchor = false;
    }
    if (captureAnchor)
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

static Ogre::Vector3 ComputeRiderSeatPos(Character* rider, Character* mount, const SeatInfo& seat)
{
    Ogre::Vector3 seatPos = ComputeDampedSeatPos(mount, seat);
    if (rider && RideCombatStance(rider, mount, seat, false) && kRideCombatSeatDropY != 0.0f)
        seatPos.y += kRideCombatSeatDropY;
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
        // visibly tremble, because none of it touches the rider's own animation.  Fields:
        // whether our pose is in the animation set, its weight and its progress (a progress
        // pinned near 0 = something restarts it every frame), `oth` and the total
        // action-animation weight (nonzero = the engine is playing something of its own on
        // top).
        //
        // `oth` = the residual weight of the STANDING IDLE.  Until 2026-08-29 this was "the
        // other posture's pose", back when a species could ask for idle_stand_normal instead
        // (P2-0 deleted that).  It is still worth logging under the same name: the standing
        // idle is what a rider is playing at the instant Mount() fires, and it cross-fades
        // out over ~14-21 frames afterwards - the 0.94/0.89 readings in the v1.6 log are
        // exactly that fade, not a blend fault.  Nonzero in STEADY STATE still means two
        // full-body poses are mixing.  The lookup is safe: idle_stand_normal is a real
        // vanilla record (P1 probe: lay=UPPER cat=NORMAL flags=whole,loop), so this
        // getAnimationData cannot insert a NULL.
        int   posePlayDbg = 0;
        float poseWDbg = 0.0f, posePDbg = 0.0f, otherWDbg = 0.0f, actWDbg = 0.0f;
        // 🆕 Option A: diagnose the pose this seat actually pins ('sitting idle' for cushion
        // mounts) - same forward-declared helper the shipping sites use.  Miss-free for both
        // names, same as the shipping path.
        AnimationData* poseDataDbg  = rAnim->getAnimationData(RidePoseNameForSeat(mount, seat));
        AnimationData* otherDataDbg = rAnim->getAnimationData("idle_stand_normal");
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

// ---- P1 diagnostic: REMOVED (the human animation table has been enumerated) --------
//
// TASK.md P1 is answered and the whole table lives in RidingPlugin_RE_NOTES.md §15, so the
// one-shot ANIMTABLE dump (a debugContinuous-gated walk of AnimsListsManager::AnimList::allAnims
// fired from Mount()) is gone as of the probe-free build.  What it settled, for anyone reading
// the pin code below: our pose clip is `sitting_new`, registered on L1 (UPPER) with
// wholeBodyAllLayer=true; there is NO vanilla straddle/side-sit clip to swap to (that is why P2
// went procedural); and allAnims is keyed by RECORD name, not clip name, which is why every pin
// in this file keys off the record name.  `git show 7838deb:RidingPlugin.cpp` if the dump itself is
// ever needed again.

// The exact type of AnimList::allAnims / actionAnims (AnimationClass.h:275-276),
// spelled out instead of inferred so that iterating it with EngineAnimMap's iterator
// is a COMPILE-time check on the boost declaration - a mismatch fails the build here
// rather than walking wrong offsets in the game.
typedef boost::unordered::unordered_map<std::string, AnimationData*,
    boost::hash<std::string >, std::equal_to<std::string >,
    Ogre::STLAllocator<std::pair<std::string const, AnimationData*>,
                       Ogre::GeneralAllocPolicy > > EngineAnimMap;

static const char* AnimLayerName(int l)
{
    switch (l)
    {
    case LOWER:   return "LOWER";
    case UPPER:   return "UPPER";
    case OVERLAY: return "OVERLAY";
    case TAIL:    return "TAIL";
    case EARS:    return "EARS";
    case ALL:     return "ALL";
    default:      return "?";
    }
}

static const char* AnimCatName(int c)
{
    switch (c)
    {
    case ANIM_NORMAL:     return "NORMAL";
    case ANIM_IMPRISONED: return "PRISON";
    case ANIM_SLEEPING:   return "SLEEP";
    case ANIM_CARRIED:    return "CARRIED";
    case ANIM_SWIMMING:   return "SWIM";
    case ANIM_GROUNDED:   return "GROUND";
    case ANIM_COMBAT:     return "COMBAT";
    case ANIM_ATTACKS:    return "ATTACKS";
    case ANIM_RANGED:     return "RANGED";
    default:              return "?";
    }
}

// Plain-POD snapshot of one AnimationData.  Kept POD because the SEH-guarded read
// below must not share a frame with unwindable C++ objects.
struct AnimRowSnap
{
    int   category;
    int   layer;
    unsigned int weaponFlags;
    float ideal, minSp, maxSp, playSp;
    bool  relocates, looped, whole, synched, isAction, normalise;
    bool  crouched, prone, rightArm, leftArm, carried, restricts;
    char  dataName[80];
    char  animName[80];
    char  slave[80];
};

static bool SafeSnapAnimRow(AnimationData* ad, AnimRowSnap* o)
{
    if (!ad || !o) return false;
    __try
    {
        o->category    = (int)ad->category;
        o->layer       = (int)ad->layername;
        o->weaponFlags = ad->weaponTypeFlags;
        o->ideal       = ad->idealMoveSpeed;
        o->minSp       = ad->minMoveSpeed;
        o->maxSp       = ad->maxMoveSpeed;
        o->playSp      = ad->playSpeed;
        o->relocates   = ad->relocates;
        o->looped      = ad->looped;
        o->whole       = ad->wholeBodyAllLayer;
        o->synched     = ad->synched;
        o->isAction    = ad->isAction;
        o->normalise   = ad->normalise;
        o->crouched    = ad->crouched;
        o->prone       = ad->prone;
        o->rightArm    = ad->usesRightArm;
        o->leftArm     = ad->usesLeftArm;
        o->carried     = ad->carried;
        o->restricts   = ad->restrictsMovementOrders;
        _snprintf_s(o->dataName, sizeof(o->dataName), _TRUNCATE, "%s", ad->dataName.c_str());
        _snprintf_s(o->animName, sizeof(o->animName), _TRUNCATE, "%s", ad->animName.c_str());
        _snprintf_s(o->slave,    sizeof(o->slave),    _TRUNCATE, "%s", ad->slave.c_str());
        return true;
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { return false; }
}

static void LogAnimRow(const char* tag, const char* key, AnimationData* ad)
{
    AnimRowSnap s;
    if (!SafeSnapAnimRow(ad, &s))
    {
        char bad[192];
        _snprintf_s(bad, 192, _TRUNCATE, "Riding:   %s key='%s' ptr=%p UNREADABLE",
                    tag, key ? key : "?", (void*)ad);
        DebugLog(std::string(bad));
        return;
    }
    char ln[640];
    _snprintf_s(ln, 640, _TRUNCATE,
        "Riding:   %s key='%s' data='%s' clip='%s' lay=%s cat=%s "
        "flags=%s%s%s%s%s%s%s%s%s%s%s slave='%s' wt=%08X spd=%.2f/%.2f/%.2f play=%.2f",
        tag, key ? key : "?", s.dataName, s.animName,
        AnimLayerName(s.layer), AnimCatName(s.category),
        s.whole     ? "whole,"    : "",
        s.looped    ? "loop,"     : "",
        s.isAction  ? "action,"   : "",
        s.normalise ? "norm,"     : "",
        s.relocates ? "reloc,"    : "",
        s.restricts ? "restrict," : "",
        s.synched   ? "sync,"     : "",
        s.rightArm  ? "Rarm,"     : "",
        s.leftArm   ? "Larm,"     : "",
        s.crouched  ? "crouch,"   : "",
        s.carried   ? "carried"   : "",
        s.slave, s.weaponFlags, s.minSp, s.ideal, s.maxSp, s.playSp);
    DebugLog(std::string(ln));
}

// ---- P4-1k: can a clip we ask for play on a mounted rider at all? ---------------------
//
// P4-1j closed the diagnosis: the engine will not choose animations for a mounted rider, and
// neither key to that door is available.  Clearing forcedSlaveLoop stuck (fsl='' in 44 steady
// -state samples) and changed nothing; isActionSlave read 1 in 72/72 post-update samples even
// though we zeroed it before every update, so it is engine-maintained - a consequence of the
// rider's carried state (prime suspect _isBeingCarried=1, which is load-bearing: clearing it
// lets the mount's collision push the rider off the back) rather than a leftover of ours.  And
// the other candidate, restoring the CharMovement that pickupObject destroy()ed, puts the rider
// back into the physics world with gravity, ground and orders - the "free character" risk.
// P4-3 therefore has to drive the upper body itself, so the one open question is whether we
// CAN: put a clip of our own onto the rider's layers and see if it takes.
//
// Now is the moment to ask, because mode 1 leaves the addList EMPTY (24/24 dumps, and the
// player sees a bind pose).  P2-1b-1's result - a requested LOWER clip pinned at w=0.000 for
// 900 frames - was measured with the whole-body pose pinned on top of it; with nothing pinned
// there is nothing left to do the suppressing, so the answer may well be different.
//
// Two clips, one per repurposed mode slot (mode 2's own question was answered - it shows the
// vanilla carry pose - so the slot is free):
//   'guard 1h'  UPPER, loop, NO 'whole', flags norm,Rarm,Larm - a weapon stance, i.e. exactly
//               the "combat upper body" half of mounted combat, and non-'whole' so it will not
//               fight LegPosePass for the legs.
//   'mid blow'  UPPER, 'whole,action,norm,reloc,restrict' - a real melee swing.  'reloc' means
//               it relocates the character, which may well fight SyncRiderNode; this phase only
//               asks whether it PLAYS, not whether it looks right.
// FindAnimData / ResolveRideCombatClips / RideCombatHost / SelectRideCombatAttack are already
// forward-declared earlier (BuildSeatInfo needs FindAnimData; the combat-runtime block declares
// the other three).  Do not re-declare them here.

// 🆕 P4-6ag: resolve once per ride AND print the standing evidence line, from whichever site gets
// there first.  Since P4-6af the animUpdate pre-pass resolves for a stance rider, and that runs
// BEFORE HaltAndForceSitPass inside a frame - so the old "resolve inside HaltAndForceSitPass and
// log there" shape silently stopped printing `P41K resolve`.  `ridelog.py` identifies the current
// route by the five `atk=` markers ON THAT LINE (`is_vanilla_attack_route`), so losing it made
// every current log read as a pre-route one and skipped the whole judge.  One log line per ride,
// budget-free (it fires once by construction: attackClipsResolved is latched here).
static void ResolveRideCombatClipsLogged(RideCombatRuntime& rt, AnimationClass* rAnim, Character* rider)
{
    if (rt.attackClipsResolved) return;
    ResolveRideCombatClips(rt, rAnim);
    char rl[320];
    _snprintf_s(rl, 320, _TRUNCATE,
        "Riding: P41K resolve rider=%p guard='%s' %s blow='%s' %s "
        "atk=['%s','%s','%s','%s','%s']",
        (void*)rider,
        kP41kGuardAnim, rt.guardClip ? "found" : "ABSENT",
        kP41kBlowAnim,  rt.blowClip  ? "found" : "ABSENT",
        rt.attackClips[0] ? "ok" : "-",
        rt.attackClips[1] ? "ok" : "-",
        rt.attackClips[2] ? "ok" : "-",
        rt.attackClips[3] ? "ok" : "-",
        rt.attackClips[4] ? "ok" : "-");
    DebugLog(std::string(rl));
}

// ---- P4-3-2 swing window: REMOVED 2026-09-02, question answered ------------------------------
// A debugContinuous-gated experiment used to open a 1000 ms window every 3 s while the stance was
// up, swap the pinned clip from 'guard 1h' to 'mid blow', and log P43SW open/hold/close/after
// lines.  The tenth trip (T13 half B) answered every question it was built to ask, so it came out
// before this build shipped - an experiment that fires regardless of what the fight is doing is
// not shipping posture, and leaving debugContinuous-only code in a player package means the
// package cannot be A/B'd against what was tested.  Two hard mechanisms it established are now
// constraints on any future ClipPin work and live in `doc.md` 关键机制 (route-A bullet):
//   * Pinning a one-shot action clip at 1.0 makes the engine EVICT the looping stance from the
//     playing list (guard play=0 on 37 of the open/hold rows), not fade it - so a one-shot always
//     has to be followed by re-requesting the loop; waiting for it to fade back never happens.
//   * The loop does come all the way back (w/acw/ms all 1.000), but not within ~0.3 s of the
//     close: ClipPin's gate is the GLOBAL `target + others <= 1.02f` test and the dying one-shot
//     is still inside `others`.  Judge recovery on the NEXT request, not on the closing frames.
//   * 1000 ms of a pinned one-shot plays only ~20% of 'mid blow' (prog 0.010 -> 0.201).
// Turning a swing into shipping behaviour needs a TRIGGER (what makes the rider swing, and when),
// which is TASK.md P4-3 step 3, not a periodic timer.  Full trip-10 readings: TASK.md, and
// HISTORY §U for the removed code's shape.  'mid blow' itself stays RESOLVED below - that one
// resolve line is the standing evidence that the rider's own table holds a real swing.
// ⛔ 2026-09-03, T27: nothing REQUESTS 'mid blow' any more either.  T20/T21 brought the swap back and
// it shipped for four rungs; trip 24 ruled it out for what it drove BESIDE the arm (left arm, right
// wrist, spine - it is a two-handed knockdown record).  The current ground-combat route now cycles
// resolved vanilla records during the window; every resolved pointer and cursor is rider-local.

// Existence-checked lookup.  getAnimationData() has operator[] semantics and inserts a NULL
// into the engine's own allAnims on a miss, so a name that might be absent must never reach it
// - find() decides first, and only a name the map already holds is ever resolved.
//
// Works for animals as well as humans: getAnimationDatasList() is the list THAT character
// resolves against (the character list for humans, the per-race list for animals), so the
// same helper answers "does this mount know 'beast walk'" without poisoning the animal list.
static AnimationData* FindAnimData(AnimationClass* rAnim, const char* name)
{
    if (!rAnim || !name) return NULL;
    AnimsListsManager::AnimList* lst = rAnim->getAnimationDatasList();
    if (!lst) return NULL;
    // Same boost-layout self-check the table dump uses: if allAnims is not where the header
    // says it is, find() would walk garbage.
    long actOff = (long)((char*)&lst->actionAnims - (char*)lst);
    long allOff = (long)((char*)&lst->allAnims    - (char*)lst);
    if (allOff != 0xB8 || actOff != 0x78) return NULL;
    EngineAnimMap::const_iterator mi = lst->allAnims.find(std::string(name));
    if (mi == lst->allAnims.end()) return NULL;
    return mi->second;
}

// Resolve and select the one body host used by pre-pass, post-pass and transaction probes.
// atkIdx is latched when the transaction opens; this helper never advances the cursor.
static void ResolveRideCombatClips(RideCombatRuntime& rt, AnimationClass* rAnim)
{
    if (rt.attackClipsResolved || !rAnim) return;
    rt.attackClipsResolved = true;
    rt.guardClip = FindAnimData(rAnim, kP41kGuardAnim);
    rt.blowClip  = FindAnimData(rAnim, kP41kBlowAnim);
    for (int i = 0; i < kRideAtkAnimCount; ++i)
        rt.attackClips[i] = FindAnimData(rAnim, kRideAtkAnim[i]);
    rt.attackCursor = 0;
    rt.attackLogBudget = 60;
}

static AnimationData* RideCombatHost(RideCombatRuntime& rt, const char** nameOut)
{
    // 🆕 P4-6af: the combat idle stance is the STANCE base host again.  Out of combat the ride
    // sit pose owns the body; the moment the stance arms, 'guard 1h' (UPPER, loop, no 'whole')
    // holds the upper body instead, so a swing starts from a weapon stance rather than from a
    // seated torso - the user-visible defect was 「进入战斗状态还用的是坐姿，破坏后续挥砍出招动作」.
    // The lower body does NOT change with the stance: LegPosePass owns it by hand (thigh/calf
    // replay + sit-height pelvis) in both modes, and 'guard 1h' carries no leg tracks.
    // The visible attack is still the technique's own Ogre::AnimationState on top of this host,
    // and it takes the upper body for the length of a window through the free-bone mask.
    // ⛔ Never blowClip: the whole `blow` family carries a `stumbles` body-part map and IS the
    // vanilla hit reaction (P4-6S).
    // Returning NULL (no such record in this rider's table) is the caller's cue to keep the
    // sit-pose base instead - never a hostless skeleton (P4-6ae: 「战斗动画变成立直不动」).
    if (nameOut) *nameOut = kP41kGuardAnim;
    return rt.guardClip;
}

// 🆕 P4-6ag: see the mix table above (it lives next to the other clip tables, before the runtime
// POD that counts into it).  This advances the per-rider cursor by one window.
static bool SelectRideCombatAttack(RideCombatRuntime& rt, AnimationClass* rAnim)
{
    // One slot per window, per rider, deterministic.  `mixIdx` = the slot this window will play
    // (-1 = none of the curated names has a state, so the caller keeps the engine's answer).
    rt.mixIdx = -1;
    for (int i = 0; i < kRideMixCount; ++i)
    {
        int idx = (rt.mixCursor + i) % kRideMixCount;
        const char* nm = kRideMixClips[idx];
        // The Ogre side is the gate: a state that does not exist would drive nothing at all.
        // Pure read - §21.1's mapper returns NULL on a miss and inserts nothing.
        if (rAnim && nm && nm[0] && RideSwingState(rAnim, nm))
        {
            rt.mixCursor = (idx + 1) % kRideMixCount;
            rt.mixIdx    = idx;
            if (rt.mixCount[idx] < 1000000) ++rt.mixCount[idx];
            return true;
        }
    }
    // Nothing curated resolved: the engine's own pick is still a legal swing.
    return true;
}

// P4-2: how many ATTACK rows the animal's own animation list holds.  The mount's list is a
// per-race one (getAnimationDatasList()), NOT the character list P1 enumerated.
// ⚠️ 2026-08-31, three independent facts: `attacks` IS EMPTY FOR EVERY SPECIES, so 0 here is
// NOT evidence that the animal cannot swing, and must NEVER trigger a per-species fallback.
//   1. tools\gamedata.py --type 5: no ANIMAL ANIM record carries attack rows for anybody.
//   2. --tech: 加鲁兽 itself owns `gar attack legs` / `gar attack long`, i.e. the attack material
//      exists as COMBAT TECHNIQUE + bare skeleton tracks, in a container this lektor never sees.
//   3. 43/44 techniques name a clip that has no ANIMATION record at all (see RE_NOTES §19).
// The 2026-08-31 trip confirmed the behaviour side too: `mAtk=0` on all 7 mounts while P3CMB
// logged mTgt=1 on 178 rows - 坐骑护主 fired regardless.  So this stays a pure diagnostic field:
// it tells you which container you are reading, not what the animal can do.
// Returns -1 for "could not read" so a failed read never looks like a real zero.
static int AnimListAttackCount(AnimationClass* anim)
{
    if (!anim) return -1;
    AnimsListsManager::AnimList* lst = anim->getAnimationDatasList();
    if (!lst) return -1;
    // Same layout self-check FindAnimData does: a boost size mismatch would make every
    // member after actionAnims land on the wrong offset.
    long actOff = (long)((char*)&lst->actionAnims - (char*)lst);
    long allOff = (long)((char*)&lst->allAnims    - (char*)lst);
    if (allOff != 0xB8 || actOff != 0x78) return -1;
    if (!lst->attacks.valid()) return -1;
    unsigned int n = lst->attacks.size();
    return (n > 4096u) ? -1 : (int)n;   // absurd count = we are not reading a lektor
}

// ---- P2-1b-2: the real straddle pose (TASK.md P2-1 route b, step 2) ----------------
//
// Route b poses the rider's legs OURSELVES because P1 proved there is nothing to swap
// to: all 11 vanilla sit/kneel/lie records are UPPER *and* carry wholeBodyAllLayer, so
// no vanilla clip leaves the legs to us (that killed route a), and P2-1b-1 proved a
// pinned `whole` pose holds every other layer at w=0.000 however cleanly the request is
// accepted (that killed the base-clip half of b, and route c standalone).  What
// P2-1b-1 DID establish, and what this stage builds on:
//
//   * a manual write + blend mask is bit-exact (kept=1.0000 over 14 samples) while the
//     write alone comes out as ours-composed-with-the-pose (kept=0.764).  The mask is a
//     REQUIRED component, not an optimisation.
//   * writing only the two thighs relocates the calves with them while their LOCAL knee
//     bend still comes from the pose track - so "thighs ours, knee borrowed" works and
//     the calves stay out of this.
//   * thigh local axes, reverse-solved from stage 2's derived positions: X = femur
//     twist (the knee does not move at all), Z = abduction, therefore Y = flexion.
//     Signs from the same measurement (skeleton frame: +X = rider's left, +Y = up,
//     +Z = forward): R thigh +40deg local Z put the knee at x=-2.69 = OUTWARD, so the
//     right leg abducts on +Z and the left needs -Z (the two bind orientations are
//     near-IDENTICAL, not mirrored, so a symmetric pose needs opposite deltas).  That
//     same number makes local Z = skeleton -Z, hence local Y = skeleton -X, hence
//     +Y swings the knee FORWARD - and flexion is NOT mirrored, both legs take +Y.
//
// That last link was derived rather than seen, so P2-1b-2 spent one sweep stage on each
// sign instead of trusting it.  ANSWERED 2026-08-29, in-game + in the log: the player
// reports stage 1 (+local Y) straddles, and the knee's fore/aft displacement flips with
// the sign at equal magnitude (fore=+2.96 on +Y vs -2.95 on -Y, femur length 4.18 either
// way) => local Y is pure flexion and +Y is forward.  Same run: kept=1.0000 on every
// sample (mask holds), L and R rows numerically identical at every sample (the mirror is
// right - this was the asymmetry the player reported in P2-1b-1), and boneLocal (DBG
// rel.y) sat at 6.35/6.36 in all four stage windows INCLUDING after restore, i.e. taking
// the thighs does not move Bip01 => the seat table needs no migration (TASK.md P2-2).
//
// Taking a thigh over DISCARDS the chair pose's ~90deg hip flexion (writes start from
// the bind pose = legs straight down), so flexion has to be re-supplied explicitly -
// which is why stage 0 (abduction only) is a splayed kneel and not a bug.
//
// Write point is this pass (mainLoop: after the game's update, before render): Ogre
// applies skeletal animation at render time, so this is the only window where a manual
// write is what the frame actually draws.  P2-1b-3 (2026-08-29) locks stage 1 in and
// takes the debugContinuous gate off, so this is now the pose every ride gets; the
// 4-stage sweep is gone and only the (budgeted, debug-gated) sampling lines remain.
// Cleanup is Dismount()'s job plus the release path below - we never hold two bones out
// of the animation system for longer than we hold the pose itself.

// Degrees.  The signs are settled above; these magnitudes are pure taste, and stage 1's
// 45deg flexion with a width-adapted spread is what the player accepted in-game.
static const float kRideLegFlexDeg      = 45.0f;  // hip flexion, +local Y = forward
static const float kRideLegAbductAtRef  = 20.0f;  // abduction at kRideLegRadRef
static const float kRideLegAbductMin    = 15.0f;  // narrowest mount we would ever seat
static const float kRideLegAbductMax    = 45.0f;  // hit at rad ~9.7 = the P4-0 size gate
static const float kRideLegRadRef       = 3.0f;   // getRadius() of a dog-sized mount
static const float kRideLegAbductPerRad = 3.75f;  // degrees per world unit of radius
static const float kRideLegTorsoToRad   = 0.65f;  // fallback fit: 7.6->4.9 and 10.3->6.7

// 🆕 P2-4 (2026-09-05) - the lower body does NOT have to straddle.  User's ruling:
// 「下半身贴在动物身上不一定非要跨骑，有的动物比如驮兽加鲁兽坐椅子的动作更合适」.
//   The FIRST cut of this was 「按背宽自动判」 (⛔ no hotkey, ⛔ no cfg column, ⛔ no species list).
// The user then supplied the split BY SPECIES, and it is THREE styles, not two, and it is ⛔ not a
// width function - which is a measurement, not an opinion:
//   * 牛 torso 10.3 / rad 6.7 => stout 0.650 wants STRADDLE, while 驮兽 9.0 / 6.7 => 0.744 wants
//     CHAIR.  The bull is LONGER and LESS stout on both axes, so no threshold pair can order them.
//   * 利维坦 (torso 83.7, stout 0.67) and 螃蟹类 (torso 5.9, stout ~5.7) both want CUSHION from
//     OPPOSITE geometric extremes => no width rule produces a third class at all.
// >= 3 of the 8 measured species contradict any geometric cut, so the LIST is the truth source:
// kRideLegStyleRows keys the 21 shipped species by race stringID, and the rule below survives as
// the fallback for a race that is NOT in that list (i.e. mods).  kDefaultsVersion still does not
// move - this is a source-side table, not a new cfg column.
enum RideLegStyle { kLegStyleStraddle = 0, kLegStyleChair = 1, kLegStyleCushion = 2 };
static const char* const kRideLegStyleName[3] = { "straddle", "chair", "cushion" };

// The two thresholds of the FALLBACK rule only (see RideLegPoseWantsChair).  ⚠️ They are still
// printed on every takeover line (wth=/sth=) so an unlisted mount's cut can be re-judged from a
// log without another design pass.
static const float kRideChairTorsoMin = 8.5f;   // front<->rear span, LIVE units, >= this = chair
static const float kRideChairStoutMax = 0.90f;  // rad/torso above this = a compact animal
                                                // (bonedog 1.16, crabs 5.7-5.9) => straddle

// Order is load-bearing: 0/1 are the thighs we take over (masked + written), 2/3 are
// the calves, whose local knee bend is borrowed from the pose track or replayed from a
// snapshot (index i's calf is at i+2), and 4/5 are the spine bones the P4-1M torso twist
// takes.  Anything indexed off these positions is written as an explicit range, never as
// "everything past 2" - see LegMaskApply's per-bone policy.  Names confirmed present by
// P2-1b-1's inventory, which still prints every bone once per DLL load.
static const char* kLegPoseBones[] = {
    "Bip01 L Thigh", "Bip01 R Thigh", "Bip01 L Calf", "Bip01 R Calf",
    "Bip01 Spine1",  "Bip01 Spine2",
    "Bip01 Pelvis"   // sit-height custody during combat host (see pelvisHave)
};
static const int          kLegPoseBoneCount   = 7;
static const int          kLegBoneCalfFirst   = 2;   // [2,4)
static const int          kLegBoneSpineFirst  = 4;   // [4,6)
static const int          kLegBonePelvis      = 6;   // [6]

// 🆕 P2-4 坐坐垫: the THIRD style's pose, and the only one with no vanilla clip behind it at
// RUNTIME - a combat clip is UPPER and carries no leg track, so "let go and let the game pose the
// legs" straightens them (that is the entire reason gLegCalfSnap exists).  So it is baked the way
// T30 bakes the swing: a vanilla clip's own leg keys, read out of male_skeleton.skeleton OFFLINE
// and stored as BIND-RELATIVE deltas, replayed by OUR writer under OUR mask:
//     want[i] = bone->getInitialOrientation() * kRideCushionLeg[i]
// which is algebraically the same shape straddle already writes (bind * delta) => a third target
// for an existing writer, not new machinery.  Keys on disk are bind-relative (local = bind * key),
// which is exactly the space Bone::setOrientation takes, so the table is a straight copy.
//   Row order IS kLegPoseBones[0..3] - both thighs then both calves.
//   Source clip = 'sitting idle' = VANILLA'S OWN GROUND/CUSHION SIT (the only floor sit among the
// 174 human clips; 'sitting chair' is the chair pose chair mode borrows, 'sitting dazed' is the hurt
// variant).  ⚠️ Its record is UPPER, so only the torso fidgets: the four LEG tracks are CONSTANT
// across all 10.9 s (t=0 / 2.5 / 5.5 / medoid all read the same pose to 0.01 units) => a static
// snapshot of it is EXACT, not an approximation.  Geometry: left knee 90 deg and 1.6 ABOVE the
// pelvis, right knee 56 deg folded under, both ankles pulled INBOARD of the knees (lateral 4.98 vs
// 5.85) => cross-legged with one shin over the other.  The 35 deg left/right asymmetry IS the pose
// (it is on every frame of the clip), not a sampling artefact.  Both calf rows are a PURE hinge
// (x = z = 0 exactly, i.e. rotation about the bone's own -Y), so nothing below the knee gets rolled
// - the same structural argument T30's grip rests on.
//   (The first bake used 'squat': symmetric, 22 deg, knees wide.  That is a squat, not a sit - the
// user pointed out 坐坐垫 is a motion vanilla already has, and this clip is it.)
// ⚠️ GENERATED - ⛔ do not hand-edit a value.  `python tools\legpose.py --mirror` re-bakes this
// table out of the asset and diffs it value by value, and EVERY legpose.py mode runs that first and
// refuses to report on a mismatch (armarc.py's rule).  To change the pose, re-bake it
// (`--bake <clip>`) and paste the whole table, never nudge a number.
// Generated by `python tools\legpose.py --bake sitting idle` (t=8.8238) - DO NOT hand-edit a value.
static const float kRideCushionLeg[4][4] = {   // w, x, y, z - bind-relative delta
    {   0.561357f,   0.009353f,   0.787889f,  -0.253027f },   // Bip01 L Thigh
    {   0.475541f,   0.734646f,   0.475172f,  -0.091468f },   // Bip01 R Thigh
    {   0.711202f,  -0.000000f,  -0.702988f,   0.000000f },   // Bip01 L Calf
    {   0.470522f,  -0.000000f,  -0.882388f,   0.000000f }    // Bip01 R Calf
};
// ⚠️ Ogre::Quaternion's ctor is (w, x, y, z) - the same order the table is emitted in, which is why
// the row goes through verbatim (RideSwingBakeAt relies on the identical convention).
static Ogre::Quaternion RideCushionQ(int i)
{
    return Ogre::Quaternion(kRideCushionLeg[i][0], kRideCushionLeg[i][1],
                            kRideCushionLeg[i][2], kRideCushionLeg[i][3]);
}

// 🆕 T23 - the HOST's half of the split, ⚠️ CUT DOWN TO THE ARM BY T25 (2026-09-03, trip 22).
// These are the bones the host must let go of WHILE A WINDOW IS OPEN: zeroed on every weighted clip
// for the duration, back to 1.0 the frame it closes.  Written by LegMaskApply, which is already
// idempotent-per-frame and already hands its entries back on dismount, so this rides on machinery
// whose leak discipline is settled (§21.2) instead of adding a second table of tracked states.
//   ⚠️⚠️ THE RULE THAT SIZES THIS TABLE: a bone in here has NO contributor unless somebody writes it.
// T23/T24 listed eleven (Spine/Neck/Head/both Clavicles/UpperArms/Forearms/Hands) because a whole
// ground technique clip was being driven onto them.  Trip 22 ruled that clip out for good - a
// ground record is authored around a standing pelvis and reads 「刀砍不出去，只能在自己肚子那块拉，
// 动作都挤成一团了」 no matter which record the engine names - and the user's ruling with it:
// 「不一定非要和原版一样，只要像骑砍那样挥砍的动作就行」.  So the swing is AUTHORED now
// (RideSwingArmPose) and this table must be exactly the set of bones that authoring writes.
// T36 established that the original two-bone replay reaches the hand's position but leaves the
// blade at the guard's wrist angle, so it cannot strike down at a ground target.  The vanilla
// hand curve is now the third writer: UpperArm -> Forearm -> Hand all receive the same clip's
// local delta.  Fingers remain host-owned; the weapon is attached below the hand bone itself.
// ⚠️ The table itself now lives beside kRideWristBones / kRideTechKeepGuardBones (P4-6aa:
// spine + right arm only).  This comment block is the history of HOW it was sized.
// Hands are NOT here - wrist stays on guard (kRideWristBones / P4-6C).  Left arm / neck /
// head are also not here (P4-6aa: they stay on guard via kRideTechKeepGuardBones).
// Host upper-body bones zeroed on guard while a technique state is driven (P4-6V).
// ---- P4-1M torso side-twist -------------------------------------------------------------
// The player's requirement: "地面是往正前方砍，我们应该往侧方，测前方砍" - a ground fighter
// swings straight ahead, a mounted one has to swing to the side / side-front, because the
// mount's body is in the way of everything directly forward.
//
// Mechanism: 'guard 1h' (and any P4-3 swing) is authored for a body facing its own +Z, and we
// cannot re-author it, so instead the SPINE is rotated and the arms come along as children.
// Bip01 Spine1 + Spine2 go manual + masked and each takes half the yaw about its own local +X.
// Local +X is the along-the-bone axis for this rig (RE_NOTES §16, measured on the femur), and
// for an upward-pointing spine bone the along-bone axis IS the vertical twist axis - so this
// needs no new axis measurement, only a SIGN, which the TWIST log line below settles by
// printing the requested angle next to the shoulder line it actually produced.
//
// Cost, accepted deliberately: masking the spine discards the host clip's own spine tracks, so
// the torso renders at bind o yaw.  For 'guard 1h' that means a straight back instead of its
// slight lean.  Fixing it would be capture-and-replay like the calf snapshot; not worth a
// second state machine before the direction itself has been seen in game.
//
// ⚠️ 2026-09-09 RETIRED by user: 「先去掉脊柱侧转，战斗朝向保持正前」.
// RideTwistTargetDeg always returns 0 (its early return is the real switch).  Constants stay
// for the log/reader; do not re-open by only raising the max.
static const float kRideTwistMaxDeg   = 0.0f;   // retired (was 90 after the face-enemy ask)
// 🆕 P4-6ah: the face-enemy ask comes back at the NODE.  The constant lives with the function
// that applies it (kRideCombatFaceMaxDeg, defined just above ApplyRiderOrientation); the spine
// route stays retired.
// kRideTwistNoTgtDeg (was 30 until 2026-09-05) deleted: unused after the early-return switch.
// That constant was the only source of gratuitous twist when no target was identifiable.
static const float kRideTwistMinDeg   = 1.0f;   // below this we hand the spine back entirely
static const float kRideTwistLerp     = 0.12f;  // per-frame low pass, see below
// ⚠️ The exponential low pass is legitimate HERE even though CLAUDE.md warns about them: that
// warning is about filters called SEVERAL TIMES per frame (they converge to the target and the
// coefficient stops meaning anything).  This one lives in LegPosePassImpl, which runs exactly
// once per frame from HaltAndForceSitPass.  It exists because the target can change instantly -
// without it a target swap snaps the head and sword across in one frame.
// ✅ SIGN MEASURED AND CORRECT - but NOT by the sign-comparison rule this comment used to cite.
// ⚠️ sh= is an ABSOLUTE world-space angle (ATan2 of the L-R UpperArm vector), NOT a twist delta,
// and its baseline is the rider's world facing, which ApplyRiderOrientation slaves to the mount's
// heading.  So "want and sh have opposite signs" only holds while the mount happens to hold one
// heading; the 2026-08-30 「112/112 反号」 was a single constant-heading fight and is NOT a rule.
// THE VALID TEST IS BASELINE REMOVAL: if the sign is right then sh = base - want, so sh+want is
// the slowly varying baseline and sh-want is the jumpy one (and vice versa if it is wrong).
// 2026-08-31, 280 samples with |want| > 5 across several fights and headings, adjacent pairs
// wrapped to +-180: median jitter sh+want 3.9 deg vs sh-want 41.6 deg, and sh+want was the
// smoother of the two on 172/280 pairs => this sign is CORRECT.  Player confirmed
// "上半身侧向敌人（可以侧前方砍）".  Do not flip it, and do not re-derive it from raw sign pairs;
// tools\ridelog.py runs the baseline-removal test for you.
static const float kRideTwistSign     = 1.0f;
// TWIST sampling is deliberately DENSER than the leg sampling (300 frames): the sign question
// only has an answer while a fight is actually running, and a fight is a few seconds long.
static const unsigned int kRideTwistLogGap    = 45;
static const int          kRideTwistLogBudget = 40;
// Twist state is held in RidePoseRuntime, keyed by rider.

// ⚠️ kRideLegTakeoverW (0.5f) IS GONE, deliberately - do not put it back.  It used to gate
// the straddle on the RIDE POSE's weight, which was correct only while the blend mask was
// pose-only.  Route A parks the ride pose at zero weight (the combat stance is the host) and
// route C parks the host at 0.5, so a gate keyed to the ride pose either releases the legs
// mid-combat or sits exactly on the boundary.  Its replacement is kRideLegHostMinW below,
// measured against whichever clip is actually driving the skeleton; the crossfade
// contamination the 0.5 existed to avoid (kept=0.764) is now handled by masking every
// weighted contributor instead of by waiting.
static const unsigned int kRideLegLogGap       = 300;  // frames between sample lines
static const int          kRideLegLogBudget    = 30;   // lines per takeover, debug only

// Animation ownership is per rider.  The old globals were harmless while the mod could
// only animate one rider, but animation hooks interleave characters: rider B could resolve
// its handles, overwrite rider A's snapshots, and then A would write B's pose on the next
// callback.  Keep every mutable leg-pose value beside the rider key, just like combat and
// mask runtime.  This remains POD-only for the VS2010 toolchain.
struct RidePoseRuntime
{
    bool skelDumped;
    bool bonesResolved;
    int poseBudget;
    unsigned int poseFrames;
    bool poseArmed;
    bool poseHas[kLegPoseBoneCount];
    unsigned short poseHandle[kLegPoseBoneCount];
    Ogre::Quaternion poseWrote[kLegPoseBoneCount];
    bool calfHave[2];
    Ogre::Quaternion calfSnap[2];
    bool calfManual;
    bool calfWarned;
    bool thighHave[2];
    Ogre::Quaternion thighSnap[2];
    bool thighManual;
    bool chairWarned;
    // Sit-height custody.  'sitting chair'/'sitting idle' are whole-body and depress the
    // pelvis; 'guard 1h' is UPPER and claims none of it, so dropping the sit host for a
    // combat stance used to hand the pelvis to BIND (standing height) = 「进入战斗上移」.
    // Capture the sit pelvis while the pose is host, replay it under manual+mask in stance.
    bool pelvisHave;
    Ogre::Quaternion pelvisSnap;
    bool pelvisManual;
    bool twistManual;
    float twistDeg;
    int twistBudget;
    int hostGrace;
    const AnimationData* hostLast;
    int hostLogBudget;
};

static boost::unordered_map<Character*, RidePoseRuntime> ridePoseByRider;

static RidePoseRuntime* FindRidePoseRuntime(Character* rider)
{
    if (!rider) return NULL;
    boost::unordered_map<Character*, RidePoseRuntime>::iterator it = ridePoseByRider.find(rider);
    return (it != ridePoseByRider.end()) ? &it->second : NULL;
}

static RidePoseRuntime& GetRidePoseRuntime(Character* rider)
{
    boost::unordered_map<Character*, RidePoseRuntime>::iterator it = ridePoseByRider.find(rider);
    if (it == ridePoseByRider.end())
    {
        RidePoseRuntime& fresh = ridePoseByRider[rider];
        memset(&fresh, 0, sizeof(fresh));
        fresh.twistDeg = 0.0f;
        fresh.hostLogBudget = 40;
        return fresh;
    }
    return it->second;
}

static void ClearRidePoseRuntime(Character* rider)
{
    if (rider) ridePoseByRider.erase(rider);
}

static void ClearAllRidePoseRuntime()
{
    ridePoseByRider.clear();
}

// P4-1L (route A): the pose that carries the skeleton is no longer necessarily
// kRidePose.  When a combat stance drives the torso instead, two things that used to
// be constants become variable:
//
//  1) THE MASK MOVES.  A blend mask lives on ONE Ogre::AnimationState (the clip's own),
//     so masking only the ride pose leaves every OTHER weighted clip's thigh tracks
//     composing onto our manual write - that is literally the kept=0.764 contamination
//     P2-1b-1 measured during the mount crossfade.  So the mask goes on EVERY weighted
//     addList entry that has a mainState, and the bone entries are re-asserted every
//     frame (idempotent, and it self-corrects when calf ownership flips below).
//     We remember which states we masked AND whether we were the one who created the
//     mask, because destroying a mask the engine owns would silently unmask its bones.
//     On restore, a tracked pointer is validated against the live addList BY POINTER
//     COMPARISON ONLY - a stale AnimationState must never be dereferenced.
//
//  2) THE KNEE BEND IS NOT ALWAYS THERE.  Today the calves are read-only and borrow
//     their local bend from the sitting pose's own tracks.  Combat clips are UPPER
//     WITHOUT 'whole' (P4-1k measured 'guard 1h' as the sole UPPER entry at w=1.000),
//     so they carry no leg tracks at all - swap the torso and the calves snap straight.
//     Guessing a knee angle would be two guesses (axis and sign are unmeasured), so
//     instead we CAPTURE it: while the ride pose is host at >= kLegCalfSnapW and the
//     calf is still non-manual, the calf's local orientation IS the pure pose
//     contribution (bind o track from the last render - the Ogre write-window fact),
//     so we snapshot it and replay it under manual control + mask once the host
//     changes.  No snapshot yet => calves are left alone and we say so in the log,
//     which reads as straight legs rather than as a crash.
static const int   kLegMaskMax   = 8;      // AnimationStates we track masks for
static const float kLegCalfSnapW = 0.99f;  // pose weight required to trust a snapshot
// How much weight the HOST needs before we take the legs.  Deliberately much lower than the
// retired kRideLegTakeoverW: that 0.5 existed because the mask was pose-only, so writing during
// the mount crossfade produced "ours o theirs" (kept=0.764).  With the mask on every
// weighted contributor that reason is gone, and route C deliberately parks the host at
// 0.5 - a 0.5 gate would sit exactly on the boundary.  This is now only a "is anything
// driving the skeleton at all" check.
static const float kRideLegHostMinW = 0.05f;
// A host swap costs exactly one frame of "no host" and the A+C log measured it: each of the 11
// mid-ride releases was followed by a takeover 0.008-0.015 s later (one frame at 130 fps), and
// mode 1 showed exactly 3 samples with ms=-1.000 - the frames where the stance exists but has
// no Ogre::AnimationState yet.  Releasing on the first such frame snaps the legs straight and
// re-bends them the next frame; that was probe-induced churn there but would land once at the
// START OF EVERY FIGHT in shipping code.  So a missing host is tolerated for a few frames:
// manual bones survive Skeleton::reset(), so simply doing nothing HOLDS the last pose - we do
// not have to write anything to bridge the gap, and we must not (there is no mask target).
static const int kLegHostGraceFrames = 12;
// Host grace is held in RidePoseRuntime, keyed by rider.
// A rider's animation lists and AnimationState objects are independent.  Keeping this table
// global made two interleaved riders release whichever masks happened to be recorded last;
// worse, a dismount could restore masks belonging to the other rider.  Keep the ownership and
// by-name rescue data beside the rider key instead.  This is intentionally POD-only for the
// VS2010 toolchain, just like RideCombatRuntime above.
struct RideMaskRuntime
{
    Ogre::AnimationState* masked[kLegMaskMax];
    bool mine[kLegMaskMax];
    char name[kLegMaskMax][64];
    int count;
    bool overflow;
    int dropped;
    int late;
};

static boost::unordered_map<Character*, RideMaskRuntime> rideMaskByRider;

static RideMaskRuntime* FindRideMaskRuntime(Character* rider)
{
    if (!rider) return NULL;
    boost::unordered_map<Character*, RideMaskRuntime>::iterator it = rideMaskByRider.find(rider);
    return (it != rideMaskByRider.end()) ? &it->second : NULL;
}

static RideMaskRuntime& GetRideMaskRuntime(Character* rider)
{
    boost::unordered_map<Character*, RideMaskRuntime>::iterator it = rideMaskByRider.find(rider);
    if (it == rideMaskByRider.end())
    {
        RideMaskRuntime& fresh = rideMaskByRider[rider];
        memset(&fresh, 0, sizeof(fresh));
        return fresh;
    }
    return it->second;
}

static void ClearRideMaskRuntime(Character* rider)
{
    if (rider) rideMaskByRider.erase(rider);
}

static void ClearAllRideMaskRuntime()
{
    rideMaskByRider.clear();
}
// The clip name that state was bound from, so the release can re-fetch it by name instead of
// hunting for it in the live lists.  RE_NOTES 21.1(2): SingleAnimation::initialiseMainState
// looks the state up with exactly this string on exactly this AnimationClass, so the re-fetch
// below is the SAME lookup that produced mainState in the first place - not a guess at it.
// A name too long to fit simply fails to match later, which degrades to the old drop.
// The clip name is stored in RideMaskRuntime for by-name release rescue.
// Tracked states that were untraceable at release time, i.e. the documented leak path in
// LegMaskRelease.  Counted rather than acted on: a pointer we cannot find by EITHER route
// must never be dereferenced, so the only honest thing to do is drop it AND say we did.  Read
// it in the LEGPOSE handback line - a non-zero value is the one mechanism that could leave a
// clip permanently unable to drive the thighs, which is what "legs stuck apart" would look like.
// Tracked states the live lists had LOST but the by-name re-fetch got back (RE_NOTES 21.2: a
// stopped clip nulls SingleAnimation::mainState, so a plain ride loses 2-4 this way).  This is
// the counter that proves the fix fired: before it existed these were exactly the dropped= ones.
// 🆕 P2-4: the same borrow-and-replay machinery, one joint up, for chair mode only.  In straddle
// mode these stay unused (the thighs are authored from the bind pose every frame); in chair mode
// the pose's own hip orientation is the thing we have to keep alive across a combat clip, exactly
// as the knee bend already is.  ⚠️ Snapshot, not a constant: the chair record is shared by every
// mount but the individual's scale is not, and this way nothing about the pose is hardcoded here.
// Every change of host, up to a budget, NOT debug-gated: "which clip owned the skeleton
// when the legs looked wrong" is unanswerable after the fact otherwise, and the takeover
// line only fires once per arming.  Budgeted rather than gated because a stance that
// flickers would otherwise flood the log.

// One shot per DLL load: every bone handle + name, so route b can be written against
// the real skeleton instead of guessed names.  Handles are what blend masks index by.
static void DumpRiderSkeletonImpl(AnimationClass* rAnim)
{
    Ogre::OldSkeletonInstance* sk = rAnim ? rAnim->skeleton : NULL;
    if (!sk) { DebugLog("Riding: SKEL no skeleton instance"); return; }
    unsigned short nb = sk->getNumBones();
    char hd[160];
    _snprintf_s(hd, 160, _TRUNCATE, "Riding: SKEL bones=%u entity=%p node=%p",
                (unsigned int)nb, (void*)rAnim->body, (void*)rAnim->node);
    DebugLog(std::string(hd));
    if (nb > 400) { DebugLog("Riding: SKEL count implausible - not walking"); return; }
    for (unsigned short i = 0; i < nb; ++i)
    {
        Ogre::OldBone* b = sk->getBone(i);
        if (!b) continue;
        const Ogre::Quaternion& q = b->getOrientation();
        const Ogre::Vector3&    p = b->getPosition();
        char ln[288];
        _snprintf_s(ln, 288, _TRUNCATE,
            "Riding:   SKEL h=%u '%s' man=%d q=(%.3f,%.3f,%.3f,%.3f) p=(%.2f,%.2f,%.2f)",
            (unsigned int)b->getHandle(), b->getName().c_str(),
            b->isManuallyControlled() ? 1 : 0, q.w, q.x, q.y, q.z, p.x, p.y, p.z);
        DebugLog(std::string(ln));
    }
    DebugLog("Riding: SKEL end");
}

static void DumpRiderSkeleton(AnimationClass* rAnim)
{
    __try { DumpRiderSkeletonImpl(rAnim); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { DebugLog("Riding: SKEL access violation - inventory abandoned"); }
}

// The layer entry for one AnimationData, so this pass can read a clip's real weight /
// desired weight / Ogre-side weight the same way PoseLayerPin does.  Separate walk on
// purpose: PoseLayerPin is load-bearing shipping code and must not grow a second path.
static AnimationClassBase::SingleAnimation* LegPoseFindSingle(
    AnimationClass* rAnim, AnimationData* ad, int* layerOut)
{
    if (layerOut) *layerOut = -1;
    if (!rAnim || !ad || !rAnim->layer.valid()) return NULL;
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return NULL;
    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay || !lay->addList.valid()) continue;
        unsigned int n = lay->addList.size();
        if (n > 64) continue;
        for (unsigned int ai = 0; ai < n; ++ai)
        {
            AnimationClassBase::SingleAnimation* sa = lay->addList[ai];
            if (sa && sa->animationData == ad)
            {
                if (layerOut) *layerOut = (int)li;
                return sa;
            }
        }
    }
    return NULL;
}

// The clip that currently CARRIES THE SKELETON: the highest-weight addList entry that
// owns an Ogre::AnimationState.  In the shipping path that is kRidePose; under route A it
// is the combat stance.  Gating the leg takeover on "is there a host at all" rather than
// "is the ride pose weighted" is what lets the straddle survive a torso swap - the
// pre-route-A gate keyed off the ride pose's own SingleAnimation and therefore RELEASED
// the legs the instant the pose stood down, which is exactly when they are needed most.
static AnimationClassBase::SingleAnimation* LegPoseFindHost(AnimationClass* rAnim,
                                                            int* layerOut)
{
    if (layerOut) *layerOut = -1;
    if (!rAnim || !rAnim->layer.valid()) return NULL;
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return NULL;
    AnimationClassBase::SingleAnimation* best = NULL;
    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay || !lay->addList.valid()) continue;
        unsigned int n = lay->addList.size();
        if (n > 64) continue;
        for (unsigned int ai = 0; ai < n; ++ai)
        {
            AnimationClassBase::SingleAnimation* sa = lay->addList[ai];
            if (!sa || !sa->mainState) continue;
            if (!best || sa->weight > best->weight)
            {
                best = sa;
                if (layerOut) *layerOut = (int)li;
            }
        }
    }
    return best;
}

// Remember one masked AnimationState so the restore can undo exactly what we did - and
// nothing else.  "mine" records that WE created the mask; destroying one the engine owns
// would silently unmask whatever bones it was holding out.  The clip name is kept for the
// by-name re-fetch in LegMaskRelease (see RideMaskRuntime::name).
static void LegMaskTrack(RideMaskRuntime& masks, Ogre::AnimationState* st, bool mine,
                         const char* clip)
{
    if (!st) return;
    for (int i = 0; i < masks.count; ++i)
        if (masks.masked[i] == st) return;
    if (masks.count >= kLegMaskMax)
    {
        if (!masks.overflow)
        {
            masks.overflow = true;
            DebugLog("Riding: LEGPOSE mask table full - a mask may outlive the ride");
        }
        return;
    }
    masks.masked[masks.count] = st;
    masks.mine[masks.count]   = mine;
    masks.name[masks.count][0] = '\0';
    if (clip)
        _snprintf_s(masks.name[masks.count], 64, _TRUNCATE, "%s", clip);
    ++masks.count;
}

// Mask our bones out of EVERY weighted clip, not just the pose.  Re-asserted every frame
// on purpose: it is idempotent, it picks up clips that appear mid-ride, and it self-heals
// when calf or spine ownership flips.  Only the bones we are actually writing this frame get
// masked to 0 - a bone we are not holding must stay at 1.0 so it keeps receiving its track
// (that is the borrowed-knee-bend path, and the same rule now covers the spine, which is only
// ours while the torso twist is engaged).
static int LegMaskApply(AnimationClass* rAnim, unsigned short nb, bool thighMask, bool calfMask,
                        bool spineMask, float swingYield, bool pelvisMask)
{
    if (!rAnim || !rAnim->layer.valid() || nb == 0) return 0;
    RidePoseRuntime* poseRt = rAnim->me ? FindRidePoseRuntime(rAnim->me) : NULL;
    if (!poseRt) return 0;
    RideCombatRuntime* swingRt = FindRideCombatRuntime(rAnim->me);
    RideMaskRuntime* maskRt = rAnim->me ? &GetRideMaskRuntime(rAnim->me) : NULL;
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return 0;
    int touched = 0;
    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay || !lay->addList.valid()) continue;
        unsigned int n = lay->addList.size();
        if (n > 64) continue;
        for (unsigned int ai = 0; ai < n; ++ai)
        {
            AnimationClassBase::SingleAnimation* sa = lay->addList[ai];
            if (!sa || !sa->mainState) continue;
            if (sa->weight <= 0.001f) continue;
            bool mine = false;
            if (!sa->mainState->hasBlendMask())
            {
                sa->mainState->createBlendMask((size_t)nb, 1.0f);
                mine = true;
            }
            if (maskRt) LegMaskTrack(*maskRt, sa->mainState, mine, sa->animName.c_str());
            for (int i = 0; i < kLegPoseBoneCount; ++i)
            {
                if (!poseRt->poseHas[i] || poseRt->poseHandle[i] >= nb) continue;
                bool ours;
                // 🆕 P2-4: the thighs are no longer unconditional.  Straddle and cushion mode pass
                // thighMask=true every frame (we author them - from the bind pose and from the
                // baked 盘腿 delta respectively); chair mode passes it only on the frames we are
                // replaying the pose's own hips, so during ordinary riding the chair clip keeps its
                // legs at mask 1.0 and we own nothing below the waist.  Same rule the calves have
                // had since route A - except that cushion also passes calfMask=true every frame,
                // because there the knee fold IS the pose rather than a borrowed detail.
                if (i < kLegBoneCalfFirst)       ours = thighMask;    // thighs: straddle/cushion/replay
                else if (i < kLegBoneSpineFirst) ours = calfMask;     // calves: replay or cushion
                else if (i < kLegBonePelvis)     ours = spineMask;    // spine: only when twisting
                else                             ours = pelvisMask;   // pelvis: sit-height hold in stance
                sa->mainState->setBlendMaskEntry((size_t)poseRt->poseHandle[i],
                                                 ours ? 0.0f : 1.0f);
            }
            // 🆕 T23: the host's upper body, ours only for the length of a swing window.  Same
            // write-every-frame rule, which is what returns them to 1.0 on the close frame without a
            // close-edge hook of their own; LegMaskRelease covers a ride that ends mid-window.
            // 🆕 P4-6l: swingYield is the SAME cross-fade the technique state's weight uses
            // (RideSwingBlend), so guard hands the torso over gradually instead of snapping.
            for (int i = 0; i < kRideSwingFreeCount; ++i)
            {
                if (!swingRt || !swingRt->freeHas[i] || swingRt->freeHandle[i] >= nb) continue;
                sa->mainState->setBlendMaskEntry((size_t)swingRt->freeHandle[i],
                                                 1.0f - swingYield);
            }
            ++touched;
        }
    }
    return touched;
}

// Undo LegMaskApply.  A tracked AnimationState is validated BY POINTER COMPARISON ONLY - a
// state that has been torn down under us must never be dereferenced - but there are now TWO
// ways to prove it is still the right object, and the second one is the fix for the leak:
//
//   1. it is still in a layer's addList/removeList with mainState == st  (the clip is playing);
//   2. AnimationClassBase::getAnimationState(clipName) hands back that same pointer.
//
// Route 1 alone is what leaked.  RE_NOTES 21.1(3): stopping a clip writes SingleAnimation::
// mainState = 0 WITHOUT destroying the state and without touching its blend mask, so every
// clip that finished during the ride became untraceable and kept our thighs masked to 0 for
// the rest of the session - the reported "thighs clamped together after dismount, only the
// calves move".  Route 2 is the same table lookup that bound the state in the first place
// (21.1(2)) and is a pure read that returns NULL on a miss (21.1(1) - hasAnimationState is
// checked first, so it cannot insert the way getAnimationData() does).  If the whole state
// set was rebuilt under us (21.3) the pointer comes back NULL or different and we still
// refuse to touch it, which is the only reason the comparison exists.
static void LegMaskRelease(AnimationClass* rAnim, unsigned short nb)
{
    RideCombatRuntime* swingRt = rAnim ? FindRideCombatRuntime(rAnim->me) : NULL;
    RideMaskRuntime* maskRt = rAnim ? FindRideMaskRuntime(rAnim->me) : NULL;
    if (!maskRt) return;
    RidePoseRuntime* poseRt = rAnim && rAnim->me ? FindRidePoseRuntime(rAnim->me) : NULL;
    if (!poseRt) return;
    maskRt->dropped = 0;
    maskRt->late    = 0;
    for (int t = 0; t < maskRt->count; ++t)
    {
        Ogre::AnimationState* st = maskRt->masked[t];
        maskRt->masked[t] = NULL;
        if (!st) continue;
        bool live = false;
        if (rAnim && rAnim->layer.valid())
        {
            unsigned int nl = rAnim->layer.size();
            if (nl <= 32)
            {
                for (unsigned int li = 0; li < nl && !live; ++li)
                {
                    AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
                    if (!lay) continue;
                    for (int pass = 0; pass < 2 && !live; ++pass)
                    {
                        lektor<AnimationClassBase::SingleAnimation*>* lst =
                            (pass == 0) ? &lay->addList : &lay->removeList;
                        if (!lst->valid()) continue;
                        unsigned int n = lst->size();
                        if (n > 64) continue;
                        for (unsigned int ai = 0; ai < n; ++ai)
                        {
                            AnimationClassBase::SingleAnimation* sa = (*lst)[ai];
                            if (sa && sa->mainState == st) { live = true; break; }
                        }
                    }
                }
            }
        }
        // Route 2.  Only asked when route 1 failed, so late= counts exactly the entries that
        // used to leak.  Needs the host entity (AnimationClass::body, 0xA8) to be there at all.
        if (!live && rAnim && rAnim->body && maskRt->name[t][0])
        {
            std::string clip(maskRt->name[t]);
            if (rAnim->getAnimationState(clip) == st) { live = true; ++maskRt->late; }
        }
        if (!live) { ++maskRt->dropped; continue; }
        if (maskRt->mine[t])
        {
            if (st->hasBlendMask()) st->destroyBlendMask();
        }
        else if (st->hasBlendMask())
        {
            for (int i = 0; i < kLegPoseBoneCount; ++i)
                if (poseRt->poseHas[i] && (nb == 0 || poseRt->poseHandle[i] < nb))
                    st->setBlendMaskEntry((size_t)poseRt->poseHandle[i], 1.0f);
            // 🆕 T23: and the swing's upper-body entries.  A ride that ends INSIDE a window (dismount,
            // knocked down, load) leaves them at 0.0, and LegMaskApply is not going to run again to
            // put them back - that is a leak with a visible symptom (a walking character whose arms
            // and head stay at bind).
            for (int i = 0; i < kRideSwingFreeCount; ++i)
                if (swingRt && swingRt->freeHas[i]
                    && (nb == 0 || swingRt->freeHandle[i] < nb))
                    st->setBlendMaskEntry((size_t)swingRt->freeHandle[i], 1.0f);
        }
    }
    maskRt->count = 0;
}

// Post-release audit: how many of our bone entries are STILL held below 1.0 on a live
// AnimationState.  Zero is the only healthy answer - anything else means a mask outlived the
// ride and that clip can no longer drive the thigh (which renders as the binding-pose straddle:
// legs slightly apart, forever).  Scans the live lists fresh instead of the tracked table on
// purpose: the leak we are hunting is exactly the case where the tracked pointer was already
// untraceable, so the table is the wrong place to look.  Reads are size-guarded twice over -
// getBlendMaskEntry() asserts on an out-of-range handle, and an engine-owned mask need not have
// been created with this skeleton's bone count.
//
// Judging it needs the dropped= counter beside it, because an engine-owned mask is allowed to
// hold one of our bones down for its own reasons: residue>0 with dropped=0 means somebody
// else's mask (not our leak - every tracked state was handled), while dropped>0 is our leak
// and is the value to act on.
static int LegMaskResidueCount(AnimationClass* rAnim)
{
    if (!rAnim || !rAnim->layer.valid()) return 0;
    RidePoseRuntime* poseRt = rAnim->me ? FindRidePoseRuntime(rAnim->me) : NULL;
    if (!poseRt) return 0;
    RideCombatRuntime* swingRt = FindRideCombatRuntime(rAnim->me);
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return 0;
    int residue = 0;
    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay) continue;
        for (int pass = 0; pass < 2; ++pass)
        {
            lektor<AnimationClassBase::SingleAnimation*>* lst =
                (pass == 0) ? &lay->addList : &lay->removeList;
            if (!lst->valid()) continue;
            unsigned int n = lst->size();
            if (n > 64) continue;
            for (unsigned int ai = 0; ai < n; ++ai)
            {
                AnimationClassBase::SingleAnimation* sa = (*lst)[ai];
                if (!sa || !sa->mainState) continue;
                if (!sa->mainState->hasBlendMask()) continue;
                const Ogre::AnimationState::BoneBlendMask* bm = sa->mainState->getBlendMask();
                if (!bm) continue;
                size_t bmn = bm->size();
                for (int i = 0; i < kLegPoseBoneCount; ++i)
                {
                    if (!poseRt->poseHas[i]) continue;
                    if ((size_t)poseRt->poseHandle[i] >= bmn) continue;
                    if (sa->mainState->getBlendMaskEntry((size_t)poseRt->poseHandle[i]) < 0.999f)
                        ++residue;
                }
                // 🆕 T23: the swing's upper-body entries are audited by the same counter, because they
                // leak the same way and the audit runs after the release either way.
                for (int i = 0; i < kRideSwingFreeCount; ++i)
                {
                    if (!swingRt || !swingRt->freeHas[i]) continue;
                    if ((size_t)swingRt->freeHandle[i] >= bmn) continue;
                    if (sa->mainState->getBlendMaskEntry((size_t)swingRt->freeHandle[i]) < 0.999f)
                        ++residue;
                }
            }
        }
    }
    return residue;
}

// ---- P4-3-4g/4h: AUTHOR the swing instead of borrowing one ---------------------------------
// ⚠️ THE RULING THAT PUT THIS HERE (trip 22, 2026-09-03).  Three rungs tried to play one of the
// engine's own attack records on a rider: pin a record-backed stand-in ('mid blow', T20/T21), drive
// the named technique's own Ogre state (T22/T23), then ask the engine for an IN-PLACE technique
// instead of its closing one (T24).  T24's mechanism worked - the second question returned a
// different record and it reached the skeleton - and the eyeball verdict was still
// 「不是劈砍…平地上的动作在马上用有些放不开，刀砍不出去，只能在自己肚子那块拉，动作都挤成一团了」.
// That is not a mixing problem and not a selection problem: a ground record is authored around a
// standing pelvis with the shoulders free to travel, and a seated pelvis crushes it.  The user's
// ruling closes the whole family: 「我们要的其实不一定非要和原版一样，只要像骑砍那样挥砍的动作就行」
// ⇒ fidelity to vanilla is no longer a requirement, a readable sabre arc is.
//
// So: hand-write the right arm, exactly the way the straddle has hand-written the thighs since
// P2-1b (§16).  Same machine, same three rules - manual control + a blend-mask 0 on every weighted
// clip (a manually controlled bone survives Skeleton::reset() but STILL receives
// NodeAnimationTrack::applyToNode, so only the mask protects it), write every frame at the
// pre-render point, hand back on the close edge and on dismount.
//   * ⛔ WHAT TRIP 23 KILLED (2026-09-03), and it was not the arc: the first version authored JOINT
//     ANGLES about each bone's own bind axes (abd/flx/elbow).  Every mechanical criterion passed -
//     kept=1.0000 on 51/51 samples, arm= on every ride, spans of 7.5..9.1 on a 6.09 arm, armback
//     man=0x00 minDot=1.0000, zero AV - and the eyeball verdict was 「更像是往下戳，位移像 \ 這個符號」.
//     `python tools\armarc.py --log <trip23 log>` says why with no model at all: two frames inside ONE
//     window wrote the SAME three angles (-25/10/-55) and the hand landed 72.3 deg apart, while
//     |measured|/|predicted| never left 1.04 (so the arm's own geometry was right all along).  A
//     bind-relative angle only fixes a bone against its PARENT, and the parent here - clavicle,
//     spine - is host-driven by 'mid blow' at speed 2.5; the shoulder itself travelled 3.0/4.8/5.1
//     units during the window.  The screen was therefore showing our arc TIMES the host's torso
//     sweep.  ⇒ authored angles are structurally the wrong parameterisation on a moving torso.
//   * ✅ WHAT REPLACED IT (T26): author DIRECTIONS IN SKELETON SPACE.  Aim each bone's local +X (which
//     §16 measured to be the bone axis, and --bind confirms: both child offsets are pure +X,
//     (2.849,0,0) and (3.244,0,0)) along an authored direction by cancelling whatever its parent is
//     doing - local = conj(parentDerived) * aim.  (Only the UpperArm reads a parent back; the Forearm's
//     parent IS the UpperArm, whose derived orientation after our write is its own aim by construction,
//     so the pair needs exactly one node read and no fresh-cache assumption.)  The host may then heave
//     the torso as much as it likes; the stroke does not move with it, and the hand sits at exactly
//     2.849*dirUpper + 3.244*dirForearm.  That makes tools\armarc.py's arithmetic the same arithmetic
//     the game runs, so the shape is known before the DLL is built - and the log carries its own
//     falsifier: want= is the intended vector, dot= is intended vs measured, and dot < 0.99 means
//     this paragraph is wrong (also the one thing that would catch a bad _getDerivedOrientation
//     vtable slot).
//   * ✅ TRIP 24 CONFIRMED THIS PARAGRAPH (2026-09-03): dot= mean 0.9920 over 36 samples, the only
//     sub-0.9 sample being a window's FIRST frame (kept=-1.0000, no derived transform recomputed yet),
//     kept= worst 0.9998, len= exactly 2.85/3.24 off the live skeleton, want= spans 6.40/6.07/8.81
//     against the table's 6.20/6.09/8.93, armback clean, zero AV.  Eyeball: 「有点劈砍的意思了」.
//   * 🆕 T27 - WHAT STILL WENT WRONG, AND IT IS NOT THE ARM.  Same trip: 「我猜测动作奇怪的原因是我们想要
//     的骑砍动作姿势应该是单手劈砍，但是角色的右手总是想找左手因为原版就是双手劈砍的，所以把动作带崩了」.
//     One step off on the mechanism (the right HAND's position is ours - that is what dot=0.9920 means),
//     dead right on the cause: the window was still SWAPPING 'mid blow' onto the body as its host, and
//     that record drove everything the arc does not - the LEFT arm, the right WRIST, the SPINE.  It is
//     one of six `blow` records, all of them 'whole,action,norm,reloc,restrict' knockdown heavy strikes
//     (doc.md:245) = a two-handed committed strike, so the left hand kept reaching across for a hilt
//     the arc had already carried away.  ⇒ the window stops swapping.  'guard 1h' (UPPER, LOOP,
//     weaponTypeFlags 0x04 = one-handed, no whole/reloc, doc.md:248) is the host all the way through,
//     which is also the strongest possible answer to §17.9: the body is never hostless for one frame.
//     Costs: the window must close on OUR clock (kRideSwingWinMs - a loop has no progress to close on)
//     and the torso no longer whips at all.  If the verdict is 「太僵」 the answer is P4-1M's own spine
//     twist (:6295), NOT a ground record coming back - trip 22 closed that family.
// 🆕 T28/T29 - ONE RIGID ROTATION, RETIRED HERE (why it was built, and trip 25's 「正手变反手」/
// 「大臂旋转小臂不动」 that forced it, are in HISTORY §V; the axis derivation and the six-key arc table
// went with them).  It delivered: elbow frozen by construction, 正手 stays 正手, and trip 26 read on
// screen as 「侧面张开大臂带动刀，简单美观」 with both model-free witnesses far past threshold.
// ⇒ ITS CEILING IS GEOMETRIC, NOT A TUNING PROBLEM.  A rigid rotation moves the hand on a CIRCLE and
// leaves the elbow exactly where it was found - witness 1 measured precisely that (`r=` span 0.000 over
// 13 windows).  But every `chop` Kenshi ships bends the elbow 62..80 deg (`skelanims.py --sweep chop`),
// so the shape an eye reads as a chop is not on any circle and NO arc table can reach it.  Axis and angle
// profile were the model's only two dials and both were spent; there was no third one inside it.
// 🆕 T30 - PLAY THE VANILLA CURVE, TWO BONES, AS A DELTA ON THE CAPTURED POSE.  Ogre stores every
// keyframe rotation RELATIVE TO THE BONE'S BINDING POSE (local = bind * key, RE_NOTES §19.7) - the same
// space Bone::setOrientation takes - so `chop down`'s own shoulder and elbow curves can be read OFFLINE
// out of male_skeleton.skeleton and replayed by OUR writer under OUR bone mask.  ⚠️ This is NOT trip 22's
// dead family: nothing here drives an engine AnimationState, nothing plays an ANIMATION record, nothing
// asks the animation system for anything.  Two float tables and a lerp.
//   local_upper(t) = capturedUpperLocal * Xu(t),   Xu(t) = conj(Ku(0)) * Ku(t)   <- kRideSwingBakeUp
//   local_fore (t) = capturedForeLocal  * Xf(t),   Xf(t) = conj(Kf(0)) * Kf(t)   <- kRideSwingBakeFo
// Both tables START AT IDENTITY, so §17.19's 「窗口开在屏幕上已有的姿势上」 survives verbatim - the one
// property T28 was built to get, and the reason the ABSOLUTE form (local = bind * K(t), vanilla's own
// frame, the shape the user already accepts on foot) was rejected: measured offline it opens 33.0 deg /
// 19.0 deg away from the pose on screen = a 3.2 unit hand jump that only a cross-fade could hide.
// ⚠️ AND IT RETIRES THE PARENT READ-BACK.  A local write needs no conj(parentDerived), so the 7.3 deg
// settled dot= residual that trips 24->25->26 differenced down to "one frame of stale R Clavicle" has no
// path into the POSE any more - it survives only in the logged dot=, where the read still happens.
// The price paid for that: the cut plane now rides the torso instead of being fixed in skeleton space.
// That is the correct sign for a chop (vanilla authors locals for exactly this reason) and it is measured,
// not assumed - the anchor block above shows the torso frame itself moving 0..39 deg between windows.
// ⚠️ WHAT THE DELTA FORM COSTS, STATED UP FRONT: vanilla's stroke is RE-BASED onto whatever pose the
// saddle guard happens to be holding, so its on-screen direction is that re-basing's, not the standing
// clip's.  Trip 23 (「往下戳」) is the standing precedent for a correctly measured curve pointing the wrong
// way ⇒ the direction was PREDICTED OFFLINE, before this build, against trip 26's own log:
//   `python tools\armarc.py --bake "chop down" --ref-log <trip 26 log>`
// anchors the offline arm on each window's first measured R Hand frame (bx=/bz= give a full orthonormal
// frame; out=/fore=/down= on the same row give the target) and lands |err| 0.01..0.03 u on all six
// windows against r = 5.42 - i.e. ~0.5%, with NOTHING fitted.  The stroke it then predicts, in the game's
// own frame, is the accept test for this build:
//   t=0.000  out  1.84 fore  3.69 down  3.52  r 5.42  elbow 126   the pose already on screen
//   t=0.276  out -1.16 fore -0.32 down -4.94  r 2.90  elbow  56   cocked high AND FOLDED - the fold is
//                                                                 the point: r travels 2.52 units
//   t=0.387  out  2.31 fore  3.63 down  1.64  r 5.42  elbow 125   through, arm extended again
//   t=0.690  out  1.79 fore  4.11 down  1.38  r 4.46  elbow  94   follow-through = vanilla's last key
//   t=1.000  identity, i.e. back on the captured pose (ONE synthetic key, marked in the table)
// cut = 6.58 vertical / 3.47 lateral / 3.95 forward (vert/lat 1.90, between T28's accepted 0.83 and
// T29's 2.17) over 155 ms, chord 8.43 = 54.5 u/s = 1.80x T29's cut - because this is VANILLA'S TEMPO,
// not a tempo anybody chose.
// 🆕 P4-6c trip 35 - THE CLIP CHANGED AGAIN: 'heavy downcut' -> 'downward combo'.  Trip 34's verdict
// was 「和原来差距不大」, and the trip 34 log explains it: the new table WAS playing (elbow spanned
// 48..146, r 2.57..5.76, dot=0.999 - all 'heavy downcut' signatures) but its LANDING is the same
// point the window opened on: measured through sat fore 3.38 / down 2.48 vs open 3.65 / 3.49, i.e.
// net travel ~0, and the elbow was still folded 102 deg at the strike.  The reach metric that picks
// a clip is the LANDING's net travel from window-open, not the cock-to-through sweep: 'downward
// combo' is the one attack clip that maximizes it.  Relative to open: strike 2 (t=0.818) at fore
// +4.16 / down -4.71, elbow OPENED to 107 deg (arm extended, r 5.37), peak cut 88 u/s over 127 ms,
// and the settle LEAVES the arm half-extended in front (fore ~3.8, r 5.3) instead of snapping back
// to the guard.  Two strikes inside one clip: wind to side-up, strike at t~0.50, re-cock, strike
// again at t~0.818.  The strike lands 1209 ms, 160 ms after kRideSwingHitMs (1050) - 12% late,
// under 1.5 units at cut speed; 'lead' cannot fix that (its cock anchor is the highest hand, this
// clip has two peaks).  Masks/writer/window clock unchanged, third tables-only swap.
//   t=0.000  out 3.21 fore 0.56 down 4.34  r 5.42  elbow 126   the pose already on screen (guard)
//   t=0.341  out 0.70 fore 0.78 down -2.48 r 2.70  elbow  52   cocked: side-up, folded (r 1.90 min)
//   t=0.500  out 0.08 fore 3.51 down 1.65  r 3.88  elbow  79   strike 1 lands forward
//   t=0.818  out -1.34 fore 4.72 down -0.37 r 4.92 elbow 108   strike 2: the long forward-low reach
//   t=1.000  vanilla's last key, arm half-extended in front (fore 3.78, r 5.34) - no synthetic
//             settle row (stretch covers the whole arc); the handback pop is the gap from THERE to
//             the guard: 64 deg on the upper arm, 3 on the forearm (the last delta rows) - 4x the
//             X-6 bounce the user waived.  If trip 35 flags that snap as ugly, the fix is a settle
//             blend in the writer, never a hand-edited row.
// ⚠️ THE TABLES BELOW ARE GENERATED, NOT AUTHORED.  Do not hand-edit a row.  `python tools\armarc.py
// --mirror` RE-BAKES the clip out of male_skeleton.skeleton and diffs every value against these arrays
// (and the two Keys constants against the row counts), so a hand tweak reads as drift and every armarc
// mode refuses to report - which is STRICTLY STRONGER than the AXIS/ARC2 hand-mirror it replaces:
// nothing is copied by hand twice any more, the checker derives its side from the asset.
// ⚠️ THE FOREARM CURVE IS A PURE HINGE - x and z are exactly 0.0 on all 45 keys, i.e. rotation about the
// bone's own -Y and nothing else.  That is why the grip survives WITHOUT freezing the elbow: a rotation
// whose axis is perpendicular to the bone axis induces no twist ABOUT that axis, so 'Bip01 R Hand'
// (host-driven, in neither mask table by design) keeps the roll it was captured with.  T28 bought 正手 by
// freezing the elbow; T30 buys it from the hinge, and the elbow is free again.
// ⚠️ THIS RETIRES T28'S TWO WITNESSES BY CONSTRUCTION: `r=` span 0.000 and a flat angle(bx,arm) were what
// "the elbow cannot move" looked like.  T30's witnesses have the OPPOSITE SIGN - r= and elbow= must SPAN
// (offline: 2.52 units and 69 deg within one window, 'chop down'; 'downward combo' spans 3.52 / 90) - plus
// offline-predicted vs measured hand path.
// Xu(t) and Xf(t): 'bigchopv2' own keys, delta-form, sign-aligned at bake time so the lerp below
// can stay dumb (the DISK is not sign-aligned - the forearm track flips between keys 1 and 2 - and an
// unaligned pair would take the long way round).  Columns: t (0..1 over kRideSwingArcMs), then w, x, y, z.
// stretch (0.49x vanilla) covers the WHOLE arc, so there is no synthetic settle row: vanilla's last key
// IS the window's last pose.  🆕 P4-6d trip 35: the clip moved AGAIN, and this time the metric had to
// grow a SIGN.  Trip 35's log showed the delta curve's hand never went BELOW the window-open pose (down
// 4.34) - 'downward combo' could only turn the arm upward from a guard that already sits low, which is
// exactly what 「手臂停在半空，没往下劈到位」 looks like.  `armarc.py --sweep-down` ranks all 174 clips
// by where the stroke POINTS; 'bigchopv2' is the one attack clip whose hand reaches down 4.90 (0.56
// BELOW the open pose) while still carrying a real elbow (span 96 deg).  ⚠️ Generated by
// `armarc.py --bake`; see above.
// Authored mounted arm / bake tables RETIRED 2026-09-09 (P4-6Q ships vanilla attack clips only).


// Hand the arm back.  Same three steps and the same self-proof as LegPoseRestoreImpl: clear the
// manual flag, reset() to the binding pose, then READ BOTH BACK - a bone still manual, or still away
// from bind, is a rider who walks off with a raised sword arm for the rest of the session.
// ⚠️ Returns WITHOUT clearing the flag when there is no AnimationClass to work on, so a later call
// with a live one still does the release.
static void RideSwingArmRelease(AnimationClass* rAnim)
{
    if (!rAnim) return;
    RideCombatRuntime* rt = FindRideCombatRuntime(rAnim->me);
    if (!rt || !rt->armHeld) return;
    rt->armHeld = false;
    rt->refHave = false;   // 🆕 T28: a released arm has no reference pose.  Third net on the same
                                 // rule (close edge, ride reset, here), and the local one: whatever the
                                 // reason for the handback, the next authored frame re-captures.
    int   man    = 0;
    float minDot = -1.0f;
    int   seen   = 0;
    for (int i = 0; i < kRideSwingFreeCount; ++i)
    {
        if (!rt->freeHas[i]) continue;
        std::string bn(kRideSwingFreeBones[i]);
        if (!rAnim->getHasBone(bn)) continue;
        Ogre::OldBone* b = rAnim->_getBone(bn);
        if (!b) continue;
        b->setManuallyControlled(false);
        b->reset();
        b->needUpdate();
        if (b->isManuallyControlled()) man |= (1 << i);
        float d = (float)Ogre::Math::Abs(b->getOrientation().Dot(b->getInitialOrientation()));
        if (seen == 0 || d < minDot) minDot = d;
        ++seen;
    }
    // 🆕 P4-6m: the hands ride the same custody (manual during a window, guard re-applies
    // its grip the frame after this).  Only a bone WE took is touched, and the window's
    // scratch KeyFrames are freed here too, so a dismount mid-window cannot leak them.
    for (int i = 0; i < kRideWristCount; ++i)
    {
        rt->handTrackHas[i] = false;
        if (rt->handKF[i])
        {
            delete rt->handKF[i];
            rt->handKF[i] = NULL;
        }
        if (!rt->wristHas[i]) continue;
        std::string wn(kRideWristBones[i]);
        if (!rAnim->getHasBone(wn)) continue;
        Ogre::OldBone* b = rAnim->_getBone(wn);
        if (!b || !b->isManuallyControlled()) continue;
        b->setManuallyControlled(false);
        b->reset();
        b->needUpdate();
    }
    char ln[160];
    _snprintf_s(ln, 160, _TRUNCATE,
         "Riding: SWING armback rider=%p man=0x%02X minDot=%.4f seen=%d lastt=%.2f f=%u",
         (void*)rAnim->me, (unsigned)man, minDot, seen, rt->armT, gP3Frames);
    DebugLog(std::string(ln));
}

// Hand the legs back: clear the manual flags, return the bones to the binding pose and
// drop the blend mask.  Must run on dismount and whenever the pose stops carrying the
// skeleton, or the rider walks away with a leg stuck at 40 degrees for the rest of the
// session (Skeleton::reset() will not touch a manually-controlled bone).  Bones are
// re-resolved by NAME every time - never cached across frames, because a skeleton
// instance can be rebuilt under us.
static void LegPoseRestoreImpl(AnimationClass* rAnim, AnimationData* poseData)
{
    if (!rAnim) return;
    // 🆕 T25: the authored arm rides on the same custody rule as the legs, so it is released from the
    // same place.  First, and unconditionally: this is the path a ride that ended INSIDE a swing
    // window takes (dismount, knocked down, load), and it is the only thing standing between that and
    // a rider who walks around with a raised sword arm for the rest of the session.
    RideSwingArmRelease(rAnim);
    RidePoseRuntime* poseRt = rAnim->me ? FindRidePoseRuntime(rAnim->me) : NULL;
    if (!poseRt) return;
    int   man    = 0;       // bones STILL manually controlled after we cleared them
    float minDot = -1.0f;   // worst |dot(orientation, initialOrientation)| after reset()
    int   seen   = 0;
    for (int i = 0; i < kLegPoseBoneCount; ++i)
    {
        if (!poseRt->poseHas[i]) continue;
        std::string bn(kLegPoseBones[i]);
        if (!rAnim->getHasBone(bn)) continue;
        Ogre::OldBone* b = rAnim->_getBone(bn);
        if (!b) continue;
        b->setManuallyControlled(false);
        b->reset();
        b->needUpdate();
        // Read the flag and the orientation back rather than assuming the two calls above
        // landed.  This is the only place that can prove the bone left our custody: a bone
        // still manual, or still sitting away from its binding pose, IS the reported
        // "legs stuck slightly apart after dismount" - and neither shows up in the kept=
        // samples or in the takeover/restored balance, both of which were clean on the
        // trip that produced that report.
        if (b->isManuallyControlled()) man |= (1 << i);
        float d = Ogre::Math::Abs(b->getOrientation().Dot(b->getInitialOrientation()));
        if (seen == 0 || d < minDot) minDot = d;
        ++seen;
    }
    poseRt->calfManual = false;
    poseRt->thighManual = false;  // P2-4: chair mode leaves the hips manual between fights, and this
                              // is the only path that can hand them back for good
    poseRt->twistManual = false;
    poseRt->twistDeg    = 0.0f;   // so the next fight ramps in instead of snapping
    // Drop the masks.  Ownership-aware and pointer-validated (see LegMaskRelease): the
    // pose's own AnimationState is just one of the tracked entries now, so the old
    // pose-only destroyBlendMask() is gone - it would have destroyed a mask the engine
    // owned in the case where the clip already had one.
    RideMaskRuntime* maskRt = FindRideMaskRuntime(rAnim->me);
    int states = maskRt ? maskRt->count : 0;
    LegMaskRelease(rAnim, rAnim->skeleton ? rAnim->skeleton->getNumBones() : 0);
    // Ungated on purpose: one line per handback (bounded by dismounts and mid-ride releases),
    // and the defect it measures was reported from a trip whose diagnostics were OFF.
    // late= is the by-name rescue count (RE_NOTES 21.4): before that route existed every one of
    // these was a dropped= leak, so late>0 with dropped=0 is the fix doing its job.
    {
        char hb[192];
        _snprintf_s(hb, 192, _TRUNCATE,
            "Riding: LEGPOSE handback man=0x%02X minDot=%.4f residue=%d dropped=%d late=%d states=%d",
            man, minDot, LegMaskResidueCount(rAnim),
            maskRt ? maskRt->dropped : 0, maskRt ? maskRt->late : 0, states);
        DebugLog(std::string(hb));
    }
    (void)poseData;
}

// How wide the legs have to spread for THIS mount.  Primary metric is
// Character::getRadius() - 2026-08-29 measured it as hull size Y * node scale, i.e. a
// LIVE physical half-size that already tracks the individual (race 3985: 0.706 -> 4.9,
// 0.959 -> 6.7), which is exactly the quantity a straddle cares about and the same one
// MountBoardEnvelope leans on.  torsoLen is the fallback because it is a front<->rear
// bone distance, not a girth (crab: torso 3.8 / rad 22) - the 0.65 factor is fitted to
// the two anchors we have live numbers for (7.6 -> 4.9, 10.3 -> 6.7).
static float RideLegAbductDeg(Character* mount, const SeatInfo& seat)
{
    float rad = mount ? mount->getRadius() : 0.0f;
    if (!(rad > 0.1f) || rad > 200.0f)
        rad = (seat.torsoLen > 0.1f) ? seat.torsoLen * kRideLegTorsoToRad : kRideLegRadRef;
    float a = kRideLegAbductAtRef + (rad - kRideLegRadRef) * kRideLegAbductPerRad;
    if (a < kRideLegAbductMin) a = kRideLegAbductMin;
    if (a > kRideLegAbductMax) a = kRideLegAbductMax;
    return a;
}

// 🆕 P2-4: straddle or chair for a mount that is NOT in kRideLegStyleRows - i.e. a modded race we
// have never seen.  ⚠️ This was the WHOLE decision under 「按背宽自动判」 and is now the FALLBACK
// only (the user's species list overrides it for all 21 shipped races, and no geometric cut can
// reproduce that list - see the falsification at kRideChairTorsoMin).  It can still only answer
// two-way: 坐坐垫 has no geometric signature at all (利维坦 and 螃蟹 want it from opposite extremes),
// so an unknown race is never given the third style.
//   The honest version of the ruling is that neither quantity we can read is a back WIDTH, so the
// split is cut with the two that exist, each answering a different half of the question:
//   * torsoLen (front<->rear bone span, live) = "is this animal big enough to sit ON".  Measured:
//     garru 9.0 / pack beast 9.0 / bison 10.3 above the line; goat 7.6, bonedog 6.9, crab 1.6,
//     Mr.Crab 3.9 below it.  ⚠️ LIVE, so it moves with the individual's scale - a runt garru
//     straddles.  That is deliberate: the pose that fits the body in front of us wins over a
//     species label, and this is the same live quantity the abduction above already adapts to.
//   * rad/torso (scale-INVARIANT, both terms scale together) = "is it a long back or a compact
//     one".  This is the gate that makes the bonedog decision scale-proof: a reference-scale
//     bonedog is 8.9 long (it would clear the size gate) but its ratio is 1.16, and the crabs sit
//     at 5.7-5.9, while every long-backed quadruped measured lands in 0.64-0.74.
// Both must pass, and anything unreadable falls back to the straddle - that is the pose 27 trips
// have shipped, so an unknown mount must never be the one experimenting.
// ⚠️ ORDERING FACT, do not lose it: the garru (0.73) is at the TOP of the low ratio band, so no
// threshold can chair the garru and leave the goat/bison straddling.  The goat is separated by the
// SIZE gate alone (7.6 vs 8.5), which is why that gate exists and why 8.5 is the number to move if
// the trip says a species landed wrong.  All measured values are in TASK.md P2-4.
static bool RideLegPoseWantsChair(Character* mount, const SeatInfo& seat,
                                  float* torsoOut, float* stoutOut)
{
    float torso = seat.torsoLen;
    float rad   = mount ? mount->getRadius() : 0.0f;
    bool  radOk = (rad > 0.1f && rad < 200.0f);
    float stout = (torso > 0.1f && radOk) ? (rad / torso) : -1.0f;
    if (torsoOut) *torsoOut = torso;
    if (stoutOut) *stoutOut = stout;
    if (stout < 0.0f) return false;   // ⛔ no derived fallback here: rad ~= 0.65*torso IS the
                                      // fallback fit, and feeding it in would read as ratio 0.65
                                      // = "chair" for every mount we cannot measure.
    return torso >= kRideChairTorsoMin && stout <= kRideChairStoutMax;
}

// 🆕 P2-4: the user's own split, by species (2026-09-05).  ⚠️ THIS TABLE IS THE TRUTH SOURCE for
// the 21 shipped mounts - the geometric rule above is consulted only for a race that is not here.
//   ⚠️ Keys are COPIED verbatim out of kDefaultSeats.  ⛔ Never retype a stringID suffix (CLAUDE.md:
// falsified twice); a race absent from kDefaultSeats cannot be seated at all, so it cannot need a
// row here either.  The Chinese name on each line is the user's own word for it, resolved through
// locale\zh_CN\gamedata.po -> the RACE record the seat row is keyed on.
//   ⚠️ TWO ROWS WERE INFERRED, not given: the user's list covers 19 of the 21 races.  Both were put
// to the user on 2026-09-05 and CONFIRMED as they stand (铁之蜘蛛 ⇒ chair, 埋骨地狼 ⇒ straddle), so
// the ⓘ marks below are provenance, not open questions.
//   ⚠️ ONE GENUINE COLLISION, user-accepted ("接受，两者都坐坐垫"): 巨型沼泽速龙 (list: 坐坐垫) and
// 河之沼泽速龙 (list: 跨骑) are separate
// creatures but 42071 is the race BOTH plain 沼泽速龙 and the Megaraptor carry, while 河之沼泽速龙 is
// its own race 44910.  So the list is expressible after all - but any OTHER swamp raptor variant
// sharing 42071 inherits 坐坐垫 with it.
struct RideLegStyleRow { const char* raceKey; int style; };
static const RideLegStyleRow kRideLegStyleRows[] = {
    { "2860-gamedata.base",             kLegStyleStraddle },  // 喙嘴兽   Beak Thing
    { "3966-gamedata.base",             kLegStyleCushion  },  // 利维坦   Leviathan
    { "3976-gamedata.base",             kLegStyleStraddle },  // 骨犬     Bonedog        (犬类)
    { "3985-gamedata.base",             kLegStyleStraddle },  // 野牛     Bull           (牛类)
    { "3987-gamedata.base",             kLegStyleCushion  },  // 沼泽乌龟 Swamp Turtle
    { "3992-gamedata.base",             kLegStyleStraddle },  // 山羊     Goat           (山羊类)
    { "3998-gamedata.base",             kLegStyleChair    },  // 驮兽/加鲁兽 Garru       (驮兽类)
    { "42068-small_changes_otto.mod",   kLegStyleStraddle },  // 笼中野兽 Cage Beast
    { "42070-small_changes_otto.mod",   kLegStyleChair    },  // ⓘ 铁之蜘蛛 Robot Spider Worker
                                                              //   - not in the user's list; every
                                                              //   other spider is 坐椅子
    { "42071-small_changes_otto.mod",   kLegStyleCushion  },  // 巨型沼泽速龙 Swamp Raptor
    { "43870-rebirth.mod",              kLegStyleChair    },  // 人皮蜘蛛 Spider
    { "43947-rebirth.mod",              kLegStyleStraddle },  // 家牛     Bull (domesticated)
    { "44910-rebirth.mod",              kLegStyleStraddle },  // 河之沼泽速龙 River Raptor
    { "48689-rebirth.mod",              kLegStyleChair    },  // 安全蜘蛛 Robot Guard Spider
    { "50641-rebirth.mod",              kLegStyleChair    },  // 血之蜘蛛 Small Spider
    { "56088-rebirth.mod",              kLegStyleStraddle },  // 剪嘴鸥   Skimmer
    { "56089-Newwworld.mod",            kLegStyleCushion  },  // 螃蟹     Crab           (螃蟹类)
    { "56112-Newwworld.mod",            kLegStyleChair    },  // 清洁组件 Big-Bones
    { "65260-Newwworld.mod",            kLegStyleChair    },  // 卷缩者 + 国王 Crimper (one race)
    { "66294-Newwworld.mod",            kLegStyleStraddle },  // ⓘ 埋骨地狼 Boneyard Wolf
                                                              //   - not in the user's list; 犬类
                                                              //   is 跨骑
    { "97570-Newwworld.mod",            kLegStyleCushion  }   // 陆地蝙蝠 Land Bat
};
static const int kRideLegStyleRowCount =
    (int)(sizeof(kRideLegStyleRows) / sizeof(kRideLegStyleRows[0]));

// Which of the three lower bodies THIS mount gets.  ⚠️ The geometric rule runs unconditionally,
// even for a listed race, because torso=/stout= are the calibration instrument on the takeover line
// and the fallback has to stay re-cuttable from a log of any ride.  fromRowOut says which half
// answered, so a log can never be misread as "the geometry chose 坐坐垫" (it cannot).
static int RideLegPoseStyle(Character* mount, const SeatInfo& seat,
                           float* torsoOut, float* stoutOut, bool* fromRowOut)
{
    bool chairGeo = RideLegPoseWantsChair(mount, seat, torsoOut, stoutOut);
    if (!seat.raceKey.empty())
    {
        for (int i = 0; i < kRideLegStyleRowCount; ++i)
        {
            if (seat.raceKey == kRideLegStyleRows[i].raceKey)
            {
                if (fromRowOut) *fromRowOut = true;
                return kRideLegStyleRows[i].style;
            }
        }
    }
    if (fromRowOut) *fromRowOut = false;
    return chairGeo ? kLegStyleChair : kLegStyleStraddle;
}

// 🆕 P4-5: the ONE question the combat gate asks of the leg pose (declared up next to
// MountCombatEligible, defined here because this is where the table and the fallback live).
// ⚠️ Deliberately re-derived on every call instead of cached in a global that LegPosePass fills:
// the stance is read from two passes per frame plus the sheathe hook (which fires from the engine's
// own call graph at an arbitrary point in the frame), so a cache would make the answer depend on
// pass ORDER, and one frame of "straddle" is long enough to open a swing window on a cushion mount.
// The cost is 21 string compares plus the one getRadius() the geometric fallback already does.
static bool RideLegPoseIsCushion(Character* mount, const SeatInfo& seat)
{
    return RideLegPoseStyle(mount, seat, NULL, NULL, NULL) == kLegStyleCushion;
}

// 🆕 P2-4 Option A: the host clip a seat's ride pose should be pinned to.  Cushion mounts answer
// kRideCushionPose ('sitting idle' - the record the leg table was baked from, so the upper body
// and the legs finally come from the SAME clip); everything else answers kRidePose unchanged.
// 🆕 Re-derived per call on purpose, same discipline as RideLegPoseIsCushion above: the pose is
// requested from Mount(), from the per-frame reassert and from the dismount teardown, and caching
// it would let the answer depend on which of those ran last - Mount() would happily switch the
// host clip a frame after the per-frame pass already pinned the other one.
static const char* RidePoseNameForSeat(Character* mount, const SeatInfo& seat)
{
    return RideLegPoseIsCushion(mount, seat) ? kRideCushionPose : kRidePose;
}

// Where the rider should be aiming, in degrees of yaw away from the mount's heading.
// Positive = toward the rider skeleton's local +X, which RE_NOTES §16 measured as the rider's
// LEFT.  Zero (and only zero) means "hand the spine back".
//
// Reference direction is the rider's OWN node orientation rather than a fresh GetMountForward:
// ApplyRiderOrientation already resolved the heading question once per frame (move direction,
// held when stationary, bone forward as a last resort) and low-passed it, and the node is where
// that answer landed.  Reading it back means the twist can never disagree with the facing it is
// measured against.  It is one frame stale (LegPosePass runs just before ApplyRiderOrientation
// in HaltAndForceSitPass) - at 0.12 smoothing that is invisible.
static float RideTwistTargetDeg(Character* rider, Character* mount, AnimationClass* rAnim,
                                bool stance, Character** tgtOut, float* distOut)
{
    if (tgtOut)  *tgtOut  = NULL;
    if (distOut) *distOut = -1.0f;
    // 2026-09-09 user: 「先去掉脊柱侧转，战斗朝向保持正前」.  Facing is the mount's heading
    // only (ApplyRiderOrientation); no spine yaw toward the threat.  Return 0 so twistOn
    // stays false (min threshold is 1 deg) and the spine is never taken from the host.
    // ⛔ Do not re-enable by only raising kRideTwistMaxDeg - this early return is the switch.
    (void)rider; (void)mount; (void)rAnim; (void)stance;
    return 0.0f;
}

// `stance` = RideCombatStance(): mounted, small enough to fight from, and in combat mode.  It
// only ever decides the TORSO; the straddle itself runs unconditionally, because it is the
// shipping seat pose and not a combat feature.
static void LegPosePassImpl(AnimationClass* rAnim, AnimationData* poseData, Character* rider,
                             Character* mount, const SeatInfo& seat, bool stance)
{
    RidePoseRuntime& poseRt = GetRidePoseRuntime(rider);
    ++poseRt.poseFrames;
    RideCombatRuntime& swingRt = GetRideCombatRuntime(rider, mount);
    bool logNow = debugContinuous && poseRt.poseBudget > 0
               && (poseRt.poseFrames % kRideLegLogGap) == 0;

    // ~30 lines, once per DLL load, and only when someone is actually watching.
    if (debugContinuous && !poseRt.skelDumped) { poseRt.skelDumped = true; DumpRiderSkeleton(rAnim); }

    if (!poseRt.bonesResolved)
    {
        poseRt.bonesResolved = true;
        for (int i = 0; i < kLegPoseBoneCount; ++i)
        {
            poseRt.poseHandle[i] = 0;
            poseRt.poseHas[i] = rAnim->getHasBone(std::string(kLegPoseBones[i]));
            if (poseRt.poseHas[i])
            {
                Ogre::OldBone* b = rAnim->_getBone(std::string(kLegPoseBones[i]));
                if (b) poseRt.poseHandle[i] = b->getHandle(); else poseRt.poseHas[i] = false;
            }
            char bn[176];
            _snprintf_s(bn, 176, _TRUNCATE, "Riding: LEGPOSE bone '%s' has=%d handle=%u",
                        kLegPoseBones[i], poseRt.poseHas[i] ? 1 : 0,
                        (unsigned int)poseRt.poseHandle[i]);
            DebugLog(std::string(bn));
        }
    }

    // Resolve swing-mask handles per rider/skeleton.  The first rider's handles are not
    // safe to reuse for a different race or a rebuilt skeleton instance.
    if (!swingRt.swingBonesResolved)
    {
        swingRt.swingBonesResolved = true;
        for (int i = 0; i < kRideSwingHoldCount; ++i)
        {
            swingRt.holdHandle[i] = 0;
            swingRt.holdHas[i] = rAnim->getHasBone(std::string(kRideSwingHoldBones[i]));
            if (swingRt.holdHas[i])
            {
                Ogre::OldBone* b = rAnim->_getBone(std::string(kRideSwingHoldBones[i]));
                if (b) swingRt.holdHandle[i] = b->getHandle(); else swingRt.holdHas[i] = false;
            }
        }
        for (int i = 0; i < kRideSwingFreeCount; ++i)
        {
            swingRt.freeHandle[i] = 0;
            swingRt.freeHas[i] = rAnim->getHasBone(std::string(kRideSwingFreeBones[i]));
            if (swingRt.freeHas[i])
            {
                Ogre::OldBone* b = rAnim->_getBone(std::string(kRideSwingFreeBones[i]));
                if (b) swingRt.freeHandle[i] = b->getHandle(); else swingRt.freeHas[i] = false;
            }
        }
        for (int i = 0; i < kRideTechKeepGuardCount; ++i)
        {
            swingRt.keepHandle[i] = 0;
            swingRt.keepHas[i] = rAnim->getHasBone(std::string(kRideTechKeepGuardBones[i]));
            if (swingRt.keepHas[i])
            {
                Ogre::OldBone* b = rAnim->_getBone(std::string(kRideTechKeepGuardBones[i]));
                if (b) swingRt.keepHandle[i] = b->getHandle(); else swingRt.keepHas[i] = false;
            }
        }
        for (int i = 0; i < kRideWristCount; ++i)
        {
            swingRt.wristHandle[i] = 0;
            swingRt.wristHas[i] = rAnim->getHasBone(std::string(kRideWristBones[i]));
            if (swingRt.wristHas[i])
            {
                Ogre::OldBone* b = rAnim->_getBone(std::string(kRideWristBones[i]));
                if (b) swingRt.wristHandle[i] = b->getHandle(); else swingRt.wristHas[i] = false;
            }
        }
    }

    // ---- knocked-out rider: hand the legs back at once -------------------------------
    // Measured 2026-08-31 (P4-1N trip): every one of the 12 sub-1.0 kept samples (worst
    // 0.8900 ~ 54 deg of contamination, one row rendering the thigh backwards at
    // out=-1.80 fore=-2.98) and BOTH grace-exhausted releases landed inside the three
    // P3CMB down=1 windows; the other five rides were kept=1.0000 throughout.  While the
    // rider is out cold the fall/prone clips own the skeleton, and the grace branch below
    // returns BEFORE LegMaskApply -> those clips are unmasked, and a manually controlled
    // bone survives Skeleton::reset() but still receives NodeAnimationTrack::applyToNode.
    // So the straddle must not merely be skipped, it must be UNARMED: restore now, re-arm
    // when the rider stands back up (the normal path re-arms by itself).
    // ⚠️ Do NOT "fix" this by widening kLegHostGraceFrames - that only renders more
    // contaminated frames.  A downed rider has no business holding a straddle anyway.
    bool riderDown = false;
    try { riderDown = (rider && (rider->isDown() || rider->isDead())); } catch (...) { riderDown = false; }
    if (riderDown)
    {
        if (poseRt.poseArmed)
        {
            LegPoseRestoreImpl(rAnim, poseData);
            poseRt.poseArmed = false;
            poseRt.hostGrace = 0;
            DebugLog("Riding: LEGPOSE released (rider down) f=" + IntToStr((int)poseRt.poseFrames));
        }
        return;
    }

    // ---- gate + mask ----------------------------------------------------------------
    // The gate asks "is SOMETHING driving the skeleton", not "is the ride pose weighted"
    // (see LegPoseFindHost): keying it off kRidePose released the legs the moment a combat
    // stance took the torso, which is exactly when the straddle is needed.  A host without
    // an Ogre AnimationState is still a reason to skip the frame - that state is where the
    // blend mask lives, and writing bones without the mask is measurably worse than not
    // writing them (kept=0.764).
    int hostLayer = -1;
    AnimationClassBase::SingleAnimation* host = LegPoseFindHost(rAnim, &hostLayer);
    unsigned short nb = rAnim->skeleton ? rAnim->skeleton->getNumBones() : 0;
    if (!host || !host->mainState || nb == 0 || host->weight < kRideLegHostMinW)
    {
        // Grace period (P4-1M).  The A+C run released the legs 11 times mid-ride and EVERY
        // one of them was followed by a fresh takeover 0.008-0.015 s later - one frame at
        // 130 fps.  Cause is visible in the same log: mode 1 logged exactly 3 samples with
        // ms=-1.000, i.e. the incoming stance clip exists in the addList but has no
        // Ogre::AnimationState yet, so there is nothing to hang a blend mask on for that
        // single frame.  Restoring across it costs a visible leg snap at the start of every
        // fight.  Doing NOTHING is correct instead of merely cheaper: a manually controlled
        // bone survives Skeleton::reset(), so the pose we wrote last frame simply stays.  We
        // must not WRITE either - without a mask a write renders as "ours o theirs"
        // (kept=0.764) - hence a bare return rather than falling through.
        //
        // The knockdown case that used to be listed here as a NEXT-BUILD item is handled
        // ABOVE now (see the riderDown block): a bare return is only safe against a MISSING
        // host, never against a host that is a knockdown clip, because returning early skips
        // LegMaskApply and an unmasked clip still reaches our manual bones through
        // NodeAnimationTrack::applyToNode.  ridelog.py computes the down-window correlation,
        // so do not re-derive it by hand.
        if (poseRt.poseArmed && poseRt.hostGrace < kLegHostGraceFrames)
        {
            ++poseRt.hostGrace;
            return;
        }
        if (poseRt.poseArmed)
        {
            LegPoseRestoreImpl(rAnim, poseData);
            poseRt.poseArmed = false;
            char rl[160];
            _snprintf_s(rl, 160, _TRUNCATE,
                "Riding: LEGPOSE released - nothing is carrying the skeleton (grace=%d f=%u)",
                poseRt.hostGrace, poseRt.poseFrames);
            DebugLog(std::string(rl));
        }
        poseRt.hostGrace = 0;
        return;
    }
    poseRt.hostGrace = 0;

    bool poseIsHost = (host->animationData == poseData);

    if (host->animationData != poseRt.hostLast && poseRt.hostLogBudget > 0)
    {
        --poseRt.hostLogBudget;
        poseRt.hostLast = host->animationData;
        char hl[224];
        _snprintf_s(hl, 224, _TRUNCATE,
            "Riding: LEGPOSE host -> '%s' w=%.3f L=%d pose=%d f=%u",
            host->animName.c_str(), host->weight, hostLayer, poseIsHost ? 1 : 0,
            poseRt.poseFrames);
        DebugLog(std::string(hl));
    }

    // Capture the borrowed knee bend while it is still trustworthy: the ride pose owns the
    // skeleton at full weight and the calf is still non-manual, so its local orientation
    // is bind o (pose track) and nothing else.  Re-taken every qualifying frame so the
    // snapshot stays fresh; the pose is static, so this converges immediately.
    if (poseIsHost && host->weight >= kLegCalfSnapW && !poseRt.calfManual)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (!poseRt.poseHas[i + 2]) continue;
            std::string cn(kLegPoseBones[i + 2]);
            if (!rAnim->getHasBone(cn)) continue;
            Ogre::OldBone* cb = rAnim->_getBone(cn);
            if (!cb || cb->isManuallyControlled()) continue;
            poseRt.calfSnap[i] = cb->getOrientation();
            poseRt.calfHave[i] = true;
        }
    }

    // Same window for the pelvis: the sit track's own local orientation is the height we
    // must keep under a combat host ('guard 1h' has no pelvis track; without us the engine
    // falls to bind = standing hips = the reported 上移 when stance arms).
    if (poseIsHost && host->weight >= kLegCalfSnapW && !poseRt.pelvisManual
        && poseRt.poseHas[kLegBonePelvis])
    {
        std::string pn(kLegPoseBones[kLegBonePelvis]);
        if (rAnim->getHasBone(pn))
        {
            Ogre::OldBone* pb = rAnim->_getBone(pn);
            if (pb && !pb->isManuallyControlled())
            {
                poseRt.pelvisSnap = pb->getOrientation();
                poseRt.pelvisHave = true;
            }
        }
    }

    // 🆕 P2-4: the hips, same window, same reason.  In straddle mode the thighs are manual from
    // the first armed frame, so isManuallyControlled() shuts this off by itself and nothing is
    // ever captured (or spent); in chair mode this is the only place the pose's own hip
    // orientation can be read cleanly.  ⚠️ Not gated on the chair decision on purpose: the
    // decision is per-MOUNT and a re-arm can change mounts, so the snapshot has to already exist.
    if (poseIsHost && host->weight >= kLegCalfSnapW && !poseRt.thighManual)
    {
        for (int i = 0; i < 2; ++i)
        {
            if (!poseRt.poseHas[i]) continue;
            std::string tn(kLegPoseBones[i]);
            if (!rAnim->getHasBone(tn)) continue;
            Ogre::OldBone* tb = rAnim->_getBone(tn);
            if (!tb || tb->isManuallyControlled()) continue;
            poseRt.thighSnap[i] = tb->getOrientation();
            poseRt.thighHave[i] = true;
        }
    }

    // Calves: borrow the bend from the host while the ride pose IS the host, replay the
    // snapshot once anything else takes over (combat clips are UPPER without 'whole' and
    // carry no leg tracks, so borrowing there means straight knees).  No snapshot => stay
    // out of it entirely, and mask nothing, so the legs look wrong rather than broken.
    bool calfReplay = !poseIsHost && poseRt.calfHave[0] && poseRt.calfHave[1];
    if (!poseIsHost && !calfReplay && !poseRt.calfWarned)
    {
        poseRt.calfWarned = true;
        DebugLog("Riding: LEGPOSE knee bend unavailable - calves left to the host clip");
    }

    // 🆕 P2-4: which of the three lower bodies this mount gets, and therefore who owns the thighs
    // and the calves this frame.
    //   * straddle => thighs ours every frame, authored from the bind pose (unchanged since
    //                 P2-1b-3); calves borrowed/replayed exactly as above.
    //   * chair    => the vanilla pose's legs are already the answer, so we take nothing while it
    //                 is the host and step in ONLY when something else is (a stance/combat clip
    //                 is UPPER and carries no leg tracks - borrowing there means straight legs,
    //                 which is the exact defect poseRt.calfSnap exists for, one joint up).
    //   * cushion  => thighs ours every frame like straddle, but from the baked 盘腿 delta instead
    //                 of the abduction/flexion pair, and the CALVES come with them EVERY frame -
    //                 not just in combat.  The chair pose's own shins hang down, so borrowing them
    //                 out of combat would un-fold the sit between fights.
    // Missing snapshot in chair mode => hold nothing and mask nothing, same failure posture as
    // the calves: the legs read as the host's, not as bind.  Cushion has no such hole: its pose is
    // a compiled-in constant, so it cannot be unavailable.
    float chairTorso = 0.0f, chairStout = -1.0f;
    bool  styleRow   = false;
    int   legStyle   = RideLegPoseStyle(mount, seat, &chairTorso, &chairStout, &styleRow);
    bool  chair      = (legStyle == kLegStyleChair);
    bool  cushion    = (legStyle == kLegStyleCushion);
    bool thighReplay = chair && !poseIsHost && poseRt.thighHave[0] && poseRt.thighHave[1];
    // P4-6D: lower body is fixed again in combat (straddle / cushion / chair replay +
    // sit-height pelvis).  Technique mask zeros root+legs so the ground clip cannot
    // fight these writes.  Wrist stays on guard (P4-6C).
    bool thighOurs   = !chair || thighReplay;
    bool calfOurs    = cushion || calfReplay;
    RideCombatRuntime* techRtEarly = FindRideCombatRuntime(rider);
    bool techWinEarly = stance && techRtEarly && techRtEarly->openTick != 0
                     && techRtEarly->techniqueName[0] != 0;
    bool pelvisOurs  = !poseIsHost && poseRt.pelvisHave
                    && poseRt.poseHas[kLegBonePelvis];
    if (chair && !poseIsHost && !thighReplay && !poseRt.chairWarned)
    {
        poseRt.chairWarned = true;
        DebugLog("Riding: LEGPOSE chair hips unavailable - thighs left to the host clip");
    }

    // ---- how far to twist the torso ---------------------------------------------------
    // Low-passed toward the target angle.  ⚠️ An exponential low pass is only legitimate
    // because this runs EXACTLY once per frame (HaltAndForceSitPass); CLAUDE.md's "per-frame
    // exponential low passes are useless" warning is about the position code, which is called
    // several times per frame and therefore converges to the target immediately.  Smoothing
    // matters here for target SWITCHES: a fight with two enemies flips the raw angle by tens
    // of degrees in one frame, and the rider should turn, not teleport.
    float twistDist = -1.0f;
    Character* twistTgt = NULL;
    float twistWant = RideTwistTargetDeg(rider, mount, rAnim, stance, &twistTgt, &twistDist);
    poseRt.twistDeg += (twistWant - poseRt.twistDeg) * kRideTwistLerp;
    // Hand the spine back once the residue is invisible, so out of combat the torso is the
    // pose clip's again and nothing of ours is left masked.
    bool twistOn = (Ogre::Math::Abs(poseRt.twistDeg) >= kRideTwistMinDeg)
                && poseRt.poseHas[kLegBoneSpineFirst] && poseRt.poseHas[kLegBoneSpineFirst + 1];

    // Mask our bones out of every weighted contributor, not just the pose.
    // 🆕 T23: the fifth argument is the window itself - while a swing is in flight the host's upper
    // body is masked out too, so the driven technique is the only thing on those bones.  Counted per
    // frame (not per clip) into the ride line's swfree=, which is this half's self-proving field the
    // way hostkeep= is the pin's: swfree=0 with swing>0 means the split never happened.
    // ⚠️ T26/T27: the free table is down to the two bones the arc writes (upper arm + forearm) and it
    // must stay EXACTLY equal to what RideSwingArmPose authors - freed-but-unwritten renders at BIND
    // (§17.9), written-but-unfreed is overwritten by the host.  T27 does not touch either table: the
    // host clip changed, the split did not.
    // The legacy authored-arm diagnostic writer is never part of the shipping ground-combat path.
    // Keeping this false also prevents a one-frame resurrection when the sticky stance tail expires
    // before RideSwingPass observes and closes its window later in the same main-loop iteration.
    bool swingFree = false;
    // P4-6V: while a technique Ogre state is driven, the host (guard) yields the entire upper
    // body via LegMaskApply's free-bone write.  Technique state keeps spine/arms (Drive only
    // zeros hold bones = root/pelvis/legs).  Out of window swingFree is false so guard owns
    // the torso again.  Legs stay ours the whole stance (straddle/chair replay).
    // 🆕 P4-6l: the yield is the technique's own cross-fade curve, so the hand-over is a
    // ~120 ms blend at each window edge instead of a one-frame snap.
    RideCombatRuntime* techRt = techRtEarly;
    bool techWin = techWinEarly;
    float swingYield = 0.0f;
    if (stance && techWin)
    {
        twistOn   = false;
        swingFree = true;
        // P4-6ab: same freeze as Drive.  A raw GetTickCount delta would walk the cross-fade
        // (and thus the guard free-mask) to 0 during a long pause while the technique state
        // was frozen mid-arc - two writers disagreeing on a paused screen.
        swingYield = RideSwingBlend(RideSwingFrozenElapsed(*techRt, GetTickCount()),
                                    RideSwingWindowMs(*techRt));
    }
    int msk = LegMaskApply(rAnim, nb, thighOurs, calfOurs, twistOn, swingYield, pelvisOurs);
    if (swingFree && msk > 0) ++swingRt.freeFrames;

    if (techWin)
    {
        // P4-6B: no root pin - full ground clip.  Seat node owns height only.
        ++techRt->driveFrames;
    }
    else
    {
        RideSwingArmRelease(rAnim);
    }

    bool takeover = !poseRt.poseArmed;
    if (takeover) poseRt.poseBudget = kRideLegLogBudget;

    // ---- take the legs away from the animation system and pose them ourselves --------
    // Abduction is mirrored (left -Z, right +Z) because the two bind orientations are
    // near-IDENTICAL rather than mirrored; flexion is NOT mirrored, for the same reason.
    // Both deltas stay in the BIND frame (qAbd * qFlx rotates by flexion first, then
    // abducts about the bind Z), which is what keeps P2-1b-1's single-axis measurements
    // applicable here.  The calves stay untouched: their local knee bend comes from the
    // pose track and rides along with the thigh.
    {
        float abd = RideLegAbductDeg(mount, seat);
        float flx = kRideLegFlexDeg;
        for (int i = 0; i < 2; ++i)
        {
            if (!poseRt.poseHas[i]) continue;
            std::string bn(kLegPoseBones[i]);
            if (!rAnim->getHasBone(bn)) continue;      // re-resolve by name every frame:
            Ogre::OldBone* b = rAnim->_getBone(bn);    // a skeleton instance can be rebuilt
            if (!b) continue;

            // 🆕 P2-4: chair mode with the pose still hosting (or with no snapshot) - the thigh is
            // not ours this frame.  ⚠️ NO reset() on the way out, unlike LegPoseRestoreImpl: this
            // pass runs after the game's update, so reset() would render ONE frame of bind pose
            // (straight leg) at every fight->idle transition.  Clearing the flag is enough - the
            // next Skeleton::reset() inside the game's update covers this bone again - and what
            // stays on screen for that one frame is the chair pose's own hip, which is what the
            // track is about to write anyway.
            if (!thighOurs)
            {
                if (poseRt.thighManual) { b->setManuallyControlled(false); b->needUpdate(); }
                continue;
            }

            // Read back BEFORE writing.  What sits here now is whatever survived last
            // frame's Skeleton::reset() + track application, so |dot| with what we wrote
            // is the direct answer to "does a manual write hold, or does the pose win?"
            // ⚠️ poseRt.thighManual as well as poseRt.poseArmed: in chair mode we skip whole
            // stretches of frames, and comparing against a write from before the gap would
            // report a fault that is really just the pose having driven the bone meanwhile.
            Ogre::Quaternion had = b->getOrientation();
            float kept = (poseRt.poseArmed && poseRt.thighManual)
                       ? (float)Ogre::Math::Abs(had.Dot(poseRt.poseWrote[i])) : -1.0f;

            b->setManuallyControlled(true);
            Ogre::Quaternion qAbd(Ogre::Degree(abd * ((i == 0) ? -1.0f : 1.0f)),
                                  Ogre::Vector3::UNIT_Z);
            Ogre::Quaternion qFlx(Ogre::Degree(flx), Ogre::Vector3::UNIT_Y);
            // Chair replays the pose's own hip verbatim, cushion replays the baked 盘腿 delta, and
            // straddle authors the abduction/flexion pair - all three are `bind * delta`, which is
            // why one writer covers them (kRideCushionLeg's header has the algebra).
            Ogre::Quaternion want = thighReplay ? poseRt.thighSnap[i]
                                  : cushion     ? (b->getInitialOrientation() * RideCushionQ(i))
                                                : (b->getInitialOrientation() * (qAbd * qFlx));
            b->setOrientation(want);
            b->needUpdate();
            poseRt.poseWrote[i] = want;

            if (logNow)
            {
                Ogre::Vector3 dp = b->_getDerivedPosition();
                Ogre::Vector3 cp = Ogre::Vector3::ZERO;
                if (poseRt.poseHas[i + 2] && rAnim->getHasBone(std::string(kLegPoseBones[i + 2])))
                {
                    Ogre::OldBone* cb = rAnim->_getBone(std::string(kLegPoseBones[i + 2]));
                    if (cb) cp = cb->_getDerivedPosition();
                }
                // hip -> knee in skeleton space.  `out` is sign-normalised so positive
                // always means "away from the body" for either leg (that is the mirror
                // check in one number), and `fore` is what settles the flexion sign
                // (+Z = forward).  `down` keeps the 4.17 femur length in view, so a
                // nonsense read is obvious rather than plausible.
                Ogre::Vector3 rel = cp - dp;
                char ln[352];
                _snprintf_s(ln, 352, _TRUNCATE,
                    "Riding: LEGPOSE f=%u '%s' kept=%.4f abd=%.1f flx=%.1f "
                    "out=%.2f fore=%.2f down=%.2f hip=(%.2f,%.2f,%.2f) knee=(%.2f,%.2f,%.2f) st=%d",
                    poseRt.poseFrames, kLegPoseBones[i], kept, abd, flx,
                    rel.x * ((i == 0) ? 1.0f : -1.0f), rel.z, -rel.y,
                    dp.x, dp.y, dp.z, cp.x, cp.y, cp.z,
                    thighReplay ? 1 : (cushion ? 2 : 0));   // st=: 0 straddle / 1 chair / 2 cushion
                DebugLog(std::string(ln));
                --poseRt.poseBudget;
            }
        }
        // 🆕 P2-4: whoever we ended up holding, that is the custody state the mask, the kept=
        // baseline and the hand-back branch above all key off next frame.
        poseRt.thighManual = thighOurs;
        // Calves.  Ours = manual + masked; otherwise make sure we are NOT holding them, so the
        // host's own bend rides along with the thigh exactly as it did before route A.
        //   * replay (straddle/chair, combat hosting) = the bend borrowed off the pose track,
        //     because the host has no knee track to borrow from.
        //   * cushion = the baked delta, EVERY frame, for the same reason the thigh is: the fold IS
        //     the pose, so handing the knee back between fights would straighten the sit out.
        for (int i = 0; i < 2; ++i)
        {
            if (!poseRt.poseHas[i + 2]) continue;
            std::string cn(kLegPoseBones[i + 2]);
            if (!rAnim->getHasBone(cn)) continue;
            Ogre::OldBone* cb = rAnim->_getBone(cn);
            if (!cb) continue;
            if (calfOurs)
            {
                cb->setManuallyControlled(true);
                cb->setOrientation(cushion
                                   ? (cb->getInitialOrientation() * RideCushionQ(i + 2))
                                   : poseRt.calfSnap[i]);
                cb->needUpdate();
            }
            else if (poseRt.calfManual)
            {
                cb->setManuallyControlled(false);
                cb->reset();
                cb->needUpdate();
            }
        }
        poseRt.calfManual = calfOurs;

        // Pelvis: replay the captured sit height under a non-sit host.  Same custody rule as
        // the calves - mask then write - so 'guard 1h' cannot drag the hips to standing bind.
        if (poseRt.poseHas[kLegBonePelvis])
        {
            std::string pn(kLegPoseBones[kLegBonePelvis]);
            if (rAnim->getHasBone(pn))
            {
                Ogre::OldBone* pb = rAnim->_getBone(pn);
                if (pb)
                {
                    if (pelvisOurs)
                    {
                        pb->setManuallyControlled(true);
                        pb->setOrientation(poseRt.pelvisSnap);
                        pb->needUpdate();
                    }
                    else if (poseRt.pelvisManual)
                    {
                        pb->setManuallyControlled(false);
                        pb->reset();
                        pb->needUpdate();
                    }
                }
            }
        }
        poseRt.pelvisManual = pelvisOurs;

        // ---- torso side-twist ---------------------------------------------------------
        // 「地面是往正前方砍，我们应该往侧方，测前方砍」 - the ground swing squares up dead
        // ahead, a mounted one must not.  The mount's heading is not negotiable (it is where
        // the animal is going), so the aim has to come out of the rider's spine.
        // 2026-09-09: twist RETIRED - RideTwistTargetDeg always returns 0, so twistOn is
        // always false and the spine is never taken from the host.  Facing = mount heading.
        //
        // Half the yaw on each of Spine1/Spine2 about that bone's own local +X.  RE_NOTES §16
        // measured local +X as "along the bone toward the child", and for a spine bone the
        // child is straight up, so along-bone IS the vertical twist axis - no new axis
        // measurement needed.  Splitting it over two joints is what keeps it reading as a
        // torso turn rather than a broken neck.  Everything downstream (neck/head, clavicles,
        // arms, and therefore the weapon) inherits, so gaze and blade follow for free.
        //
        // ⚠️ Accepted cost: masking the spine throws away the stance clip's OWN spine track,
        // so the back renders as bind o yaw - straight. Capture-and-replay (the calf trick)
        // was deliberately NOT used here: the spine's interesting frames are mid-crossfade,
        // and a snapshot taken then bakes a blend into a frozen pose.
        // ✅ The rotation SIGN about local +X is now MEASURED (2026-08-31, baseline-removal test
        // on 280 samples - see kRideTwistSign): it is correct.  ⚠️ Do NOT re-check it by comparing
        // the signs of want= and sh= frame by frame; sh= is an absolute world angle, so that
        // comparison only means anything while the mount holds one heading.
        if (twistOn)
        {
            // Refill the log budget on the edge, i.e. once per fight rather than once per
            // ride: the sign question can only be answered while the torso is actually
            // turning, and a ride can contain several fights minutes apart.
            if (!poseRt.twistManual) poseRt.twistBudget = kRideTwistLogBudget;
            float half = poseRt.twistDeg * 0.5f;
            for (int i = kLegBoneSpineFirst; i < kLegBonePelvis; ++i)
            {
                if (!poseRt.poseHas[i]) continue;
                std::string sn(kLegPoseBones[i]);
                if (!rAnim->getHasBone(sn)) continue;
                Ogre::OldBone* sb = rAnim->_getBone(sn);
                if (!sb) continue;
                sb->setManuallyControlled(true);
                Ogre::Quaternion qTw(Ogre::Degree(half), Ogre::Vector3::UNIT_X);
                Ogre::Quaternion want = sb->getInitialOrientation() * qTw;
                sb->setOrientation(want);
                sb->needUpdate();
                poseRt.poseWrote[i] = want;
            }
        }
        else if (poseRt.twistManual)
        {
            for (int i = kLegBoneSpineFirst; i < kLegBonePelvis; ++i)
            {
                if (!poseRt.poseHas[i]) continue;
                std::string sn(kLegPoseBones[i]);
                if (!rAnim->getHasBone(sn)) continue;
                Ogre::OldBone* sb = rAnim->_getBone(sn);
                if (!sb) continue;
                sb->setManuallyControlled(false);
                sb->reset();
                sb->needUpdate();
            }
        }
        poseRt.twistManual = twistOn;

        // What the twist actually did, measured off the shoulder line rather than trusted.
        // v = L shoulder - R shoulder points along skeleton +X (= the rider's LEFT) at rest,
        // so sh = atan2(v.z, v.x) is 0 when squared up.  A yaw of +phi about +Y (which takes
        // forward +Z toward +X) rotates that line to (cos phi, 0, -sin phi) => sh = -phi.
        // ⚠️ READ IT LIKE THIS - and NOT by comparing signs: sh is an ABSOLUTE WORLD angle, so
        // it carries the rider's own facing (= the mount's heading) as a baseline, and
        // sh = baseline - want.  Opposite signs therefore prove nothing on their own: they only
        // appear when the baseline happens to sit near 0, and the 2026-08-30 「112/112 反号」 was
        // one constant-heading fight.  The valid criterion is BASELINE REMOVAL: sh+want should be
        // the slowly varying series and sh-want the jumpy one.  tools\ridelog.py runs that test
        // (adjacent samples, wrapped to +-180) and reported 3.9 vs 41.6 deg median jitter on
        // 2026-08-31 => the sign is right.  |sh| far below |want| still means what it did: the
        // mask is not covering some contributor that owns the spine (cross-check msk=).
        if (debugContinuous && poseRt.twistBudget > 0
            && (poseRt.poseFrames % kRideTwistLogGap) == 0)
        {
            --poseRt.twistBudget;
            float sh = 0.0f; bool haveSh = false;
            if (rAnim->getHasBone(std::string("Bip01 L UpperArm"))
                && rAnim->getHasBone(std::string("Bip01 R UpperArm")))
            {
                Ogre::OldBone* la = rAnim->_getBone(std::string("Bip01 L UpperArm"));
                Ogre::OldBone* ra = rAnim->_getBone(std::string("Bip01 R UpperArm"));
                if (la && ra)
                {
                    Ogre::Vector3 v = la->_getDerivedPosition() - ra->_getDerivedPosition();
                    sh = Ogre::Math::ATan2(v.z, v.x).valueDegrees();
                    haveSh = true;
                }
            }
            char tw[288];
            _snprintf_s(tw, 288, _TRUNCATE,
                "Riding: TWIST f=%u want=%.1f raw=%.1f on=%d tgt=%d d=%.1f sh=%.1f/%d "
                "host='%s' msk=%d holdms=%d",
                poseRt.poseFrames, poseRt.twistDeg, twistWant, twistOn ? 1 : 0,
                twistTgt ? 1 : 0, twistDist, sh, haveSh ? 1 : 0,
                host->animName.c_str(), msk, RideStanceHoldMs(rider));
            DebugLog(std::string(tw));
        }

        poseRt.poseArmed = true;

        // One line per takeover, NOT debug-gated: it is the only record of what spread
        // this mount actually got, and the width adaptation can only be checked by
        // comparing it across body sizes.  Handles are in it because a blend mask indexes
        // by handle - a wrong one silently masks somebody else's bone.  host=/msk= are the
        // route-A additions: which clip opened the gate, and how many AnimationStates got
        // masked (msk=1 while a stance is up means we missed a contributor, which is what
        // a kept< 1.0000 further down would then be blamed on).
        //   🆕 P2-4 fields, and they are the whole calibration instrument: style= is the verdict
        // (straddle|chair|cushion) and src= says WHO gave it - `row` = the user's species list,
        // `geo` = the fallback rule, which can only ever answer straddle|chair.  torso=/stout= are
        // the two measurements the fallback reads and wth=/sth= the thresholds that build used, so
        // an unlisted mount's cut can be re-judged from any log without guessing which DLL it came
        // from - they are printed for listed races too, where they are informational only.  thigh=
        // is who owns the hips on the arming frame, calf= who owns the knees (⚠️ cushion holds both
        // every frame => 1/1), snap= whether the chair snapshot existed yet.  ⚠️ ONE LINE PER MOUNT
        // - to calibrate, ride one animal of each species and read style=/src=/torso=/stout=, not
        // these numbers averaged.
        if (takeover)
        {
            char tk[448];
            _snprintf_s(tk, 448, _TRUNCATE,
                "Riding: LEGPOSE takeover abd=%.1f flx=%.1f rad=%.1f torso=%.1f "
                "h=(%u,%u) bones=%u host='%s' hw=%.3f L=%d msk=%d calf=%d "
                "spine=(%u,%u) stance=%d twist=%.1f style=%s src=%s stout=%.3f "
                "wth=%.1f sth=%.2f thigh=%d snap=%d",
                abd, flx, mount ? mount->getRadius() : -1.0f, seat.torsoLen,
                (unsigned int)poseRt.poseHandle[0], (unsigned int)poseRt.poseHandle[1],
                (unsigned int)nb, host->animName.c_str(), host->weight, hostLayer,
                msk, calfOurs ? 1 : 0,
                (unsigned int)poseRt.poseHandle[kLegBoneSpineFirst],
                (unsigned int)poseRt.poseHandle[kLegBoneSpineFirst + 1],
                stance ? 1 : 0, poseRt.twistDeg,
                kRideLegStyleName[legStyle], styleRow ? "row" : "geo", chairStout,
                kRideChairTorsoMin, kRideChairStoutMax,
                thighOurs ? 1 : 0, (poseRt.thighHave[0] && poseRt.thighHave[1]) ? 1 : 0);
            DebugLog(std::string(tk));
        }
    }
}

// SEH shells: kept free of anything that needs C++ unwinding (C2712), the same split every
// __try in this file uses.  Everything that allocates lives in the *Impl functions.
static void LegPoseRestore(AnimationClass* rAnim, AnimationData* poseData)
{
    __try { LegPoseRestoreImpl(rAnim, poseData); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { DebugLog("Riding: LEGPOSE restore access violation"); }
}

// Best-effort cleanup for a contained fault in a live world.  Each rider gets an
// independent SEH shell so one damaged animation object cannot prevent another rider's
// manual bones, masks and mounted host from being returned.  World-reset cleanup never
// calls this: CharacterLooksLive cannot prove that the rest of a stale world object is safe.
static void LiveRideHandbackOne(Character* rider)
{
    if (!rider || !CharacterLooksLive(rider)) return;
    AnimationClass* rAnim = rider->getAnimationClass();
    if (!rAnim) return;

    RideCombatRuntime* combatRt = FindRideCombatRuntime(rider);
    // LegPoseRestoreImpl releases authored arm/manual leg ownership and all tracked
    // blend-mask entries while the host states are still easy to rediscover.
    if (FindRidePoseRuntime(rider))
        LegPoseRestoreImpl(rAnim, NULL);
    else if (combatRt)
        RideSwingArmRelease(rAnim);

    if (combatRt)
    {
        if (combatRt->techniqueName[0])
            RideSwingUndrive(rAnim, combatRt->techniqueName);
        const char* hostNm = NULL;
        AnimationData* host = RideCombatHost(*combatRt, &hostNm);
        ReleaseRideCombatHost(rider, rAnim, host, hostNm);
    }
}

static void LiveRideHandbackOneSEH(Character* rider)
{
    __try { LiveRideHandbackOne(rider); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { DebugLog("Riding: live SEH handback access violation"); }
}

static void LiveRideHandbackAll()
{
    boost::unordered_map<Character*, RidePoseRuntime>::iterator pit = ridePoseByRider.begin();
    for (; pit != ridePoseByRider.end(); ++pit)
        LiveRideHandbackOneSEH(pit->first);

    // A combat runtime can exist before pose takeover finishes.  Cover those riders too,
    // without processing riders already visited through the pose map twice.
    boost::unordered_map<Character*, RideCombatRuntime>::iterator cit = rideCombatByRider.begin();
    for (; cit != rideCombatByRider.end(); ++cit)
        if (ridePoseByRider.find(cit->first) == ridePoseByRider.end())
            LiveRideHandbackOneSEH(cit->first);
}

static void LegPosePassSEH(AnimationClass* rAnim, AnimationData* poseData, Character* rider,
                            Character* mount, const SeatInfo& seat, bool stance)
{
    __try { LegPosePassImpl(rAnim, poseData, rider, mount, seat, stance); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        // Preserve ownership state.  A partial pass may already have made bones manual or
        // installed blend-mask entries; clearing poseArmed here makes Dismount skip the only
        // handback that can undo them.  The next healthy pass may recover in place, while
        // Dismount/live-SEH cleanup now restores whenever a rider runtime exists.
        DebugLog("Riding: LEGPOSE access violation - ownership retained for handback");
    }
}

// Thin wrapper.  ⚠️ NO debugContinuous gate any more (P2-1b-3): the straddle is the
// shipping pose, so it has to run in normal play.  Handing the legs back is now driven
// by whether ANY clip is driving the skeleton (kRideLegHostMinW against the host clip's
// weight, NOT against the ride pose's - see LegPoseFindHost) and by Dismount().  Toggling
// diagnostics must not change what the rider looks like, only how much gets logged.
static void LegPosePass(AnimationClass* rAnim, AnimationData* poseData, Character* rider,
                         Character* mount, const SeatInfo& seat, bool stance)
{
    if (!rAnim) return;
    LegPosePassSEH(rAnim, poseData, rider, mount, seat, stance);
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
    //    kRidePose is the toilet-sitting pose the player confirmed; 🆕 cushion mounts pin
    //    'sitting idle' instead (Option A, 2026-09-07) so the hands match the baked legs.
    rider->runSlaveAnim(RidePoseNameForSeat(mount, seat), 1.0f, 1.0f);

    mountSeat[mount] = seat;

    riderToMount[rider] = mount;
    mountToRider[mount] = rider;
    SeedPersistedConstants(mount);   // frame-one placement from riding.cfg constants
    p3Probe.erase(rider);
    gP3Budget = kP3LogBudget;        // P3-0: fresh line budget for this ride
    gP3HullCreates = 0;              // P3-2: rebuilds this ride (~1 per 30 frames)
    gCmbBudget = kCmbBudget;         // P3-3: combat-visibility lines for this ride
    gCmbSig    = -1;
    gAtkTries  = kAtkTryBudget;      // P4-1b: hand-issued attack orders for this ride
    gAtkReads  = kAtkReadBudget;
    gAtkLastFrame = 0;
    gAtkStage  = 0;
    gAtkCurRung = -1;                // P4-1d: nothing in flight, every rung re-armed
    for (int p41dRung = 0; p41dRung < kAtkStages; ++p41dRung) gRungDead[p41dRung] = false;
    gDrawTries = kDrawTryBudget;     // P4-1e: re-arm the draw attempts for this ride
    gDrawCalls = 0;
    gDrawNoWpn = 0;
    gDrawLastFrame = 0;
    gInvDumped = 0;
    gArmBudget = kArmBudget;         // P4-1e-2: arm/aggro state lines for this ride
    gArmSig    = -1;
    gEdgeBudget = kEdgeBudget;       // P4-1f: weapon-state edges for this ride
    gEdgeWpn    = -1;
    gEdgeACW    = -99;
    gArmDumpBudget = kArmDumpBudget;  // P4-1g: layer dumps for this ride
    gArmDumpLeft   = kArmDumpBase;    // a short baseline before the first forced draw
    gArmDumpCmbBudget  = kArmDumpCmbBudget;  // P4-1h: combat-only dumps, own budget
    gArmDumpCmbEntries = -1;
    gRideStanceOn      = false;
    ClearRideStanceRuntime(rider);           // P4-1N: no release tail into a new ride
    ClearRideStanceDrawRuntime(rider);
    ResetRideSwingWindow(rider);
    // All leg-pose state is keyed by rider.  A fresh mount/load must not inherit a
    // snapshot, manual flag, handle, or diagnostic budget from another rider.
    ClearRidePoseRuntime(rider);
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

    // P4-0 body-size gate.  Printed on EVERY mount, deliberately: the gate's one residual
    // risk is "big animal + degenerate torso + small hull" all at once, and the only way that
    // shows up is somebody riding such an animal and the line reading elig=1 next to an
    // obviously huge mount.  h= is RECORD ONLY and never decides anything - it is the
    // candidate third term (anchor-bone height above the mount's own movement position; goat
    // measured 6.99u), collected now so the decision has data if it is ever needed.
    float sizeDbg = MountCombatSize(mount, seat);
    float radDbg  = mount->getRadius();
    float hDbg    = -999.0f;                  // sentinel: "not measurable", not a height
    CharMovement* mMoveDbg = mount->getMovement();
    if (mMoveDbg && !seat.backBone.empty())
        hDbg = mount->getBoneWorldPosition(seat.backBone).y - mMoveDbg->getPosition().y;

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
             + " adapt=" + IntToStr((int)(SeatUp(seat) * 100.0f)) + "/" + IntToStr((int)(SeatForward(seat) * 100.0f))
             // P4-0: rad=/size=/elig= are the gate itself (all ×10), h= is the record-only
             // candidate third term.  size = max(torso, rad) so it is comparable to torso=
             // and rad= printed beside it; elig= is what the gate would answer today.
             // ⚠️ 🆕 P4-5: elig= now has TWO terms, so `elig=0` next to a small size= is not a bug -
             // it is a 坐坐垫 mount (the LEGPOSE takeover line's style= says which, and ridelog.py's
             // T32 invariant ⑨ cross-checks exactly this pair).
             + " rad=" + IntToStr((int)(radDbg * 10.0f))
             + " size=" + IntToStr((int)(sizeDbg * 10.0f))
             + " elig=" + IntToStr(MountCombatEligible(mount, seat) ? 1 : 0)
             // P4-2: mAtk= the number of ATTACK rows in THIS ANIMAL's own list (-1 = unreadable).
             // 0 on a small-tier mount would mean the mount physically cannot swing and the
             // both-sides ruling has to fall back to rider-only for that species.
             + " mAtk=" + IntToStr(AnimListAttackCount(mAnimDbg))
             + " h=" + IntToStr((int)(hDbg * 10.0f)));
}

void Dismount(Character* rider)
{
    if (!rider) return;
    RideCombatRuntime* rideRt = FindRideCombatRuntime(rider);
    RidePoseRuntime* poseRt = FindRidePoseRuntime(rider);
    RideStanceDrawRuntime* drawRt = FindRideStanceDrawRuntime(rider);

    // P2-1b: hand the legs back before anything else.  If the straddle is armed when the ride
    // ends, the rider walks away with manually-controlled thighs (Skeleton::reset() will not
    // touch them) and a blend-masked pose - i.e. permanently broken until a reload.  This is
    // a no-op when it was never armed.
    // 🆕 Option A: the pose NAME follows the style that was actually ridden.  The mount is
    // looked up from the map here because the map erase only happens at the END of this
    // function - the pair is still tracked on entry by construction (Dismount is only called
    // for a rider in riderToMount).  Same NULL tolerance as everywhere: a miss answers
    // kRidePose, which endSlaveAnim/getAnimationData already treat as a no-op/absent.
    {
        boost::unordered_map<Character*, Character*>::const_iterator dit = riderToMount.find(rider);
        Character* dm = (dit != riderToMount.end()) ? dit->second : NULL;
        SeatInfo dSeat;
        boost::unordered_map<Character*, SeatInfo>::const_iterator dSit = mountSeat.find(dm);
        if (dm && dSit != mountSeat.end()) dSeat = dSit->second;
        const char* poseNm = RidePoseNameForSeat(dm, dSeat);
        // Runtime existence, not poseArmed, is the custody test.  A contained fault may occur
        // after a bone/mask write but before poseArmed is set; skipping restore in that state
        // would discard the only ownership record at the end of Dismount.
        if (poseRt)
        {
            AnimationClass* pAnim = rider->getAnimationClass();
            if (pAnim)
            {
                LegPoseRestore(pAnim, FindAnimData(pAnim, poseNm));
                poseRt->poseArmed = false;
                DebugLog("Riding: LEGPOSE restored on dismount");
            }
        }
    }

    // P4-1d: undo rung 3.  runCombatAnimation() puts the animation layer into a combat swing
    // that nothing else in this plugin ever ends, so a rider who dismounts mid-probe would walk
    // away stuck in it.  @0x5B34E0, public, and a no-op when no combat animation is playing -
    // same discipline as the LegPose restore above: the probe cleans up unconditionally.
    {
        AnimationClass* cAnim = rider->getAnimationClass();
        if (cAnim) cAnim->endCombatAnimation();
        // 🆕 2026-09-09: stop the combat host clips so idle is not still fighting them after
        // dismount (「下马后人物会发抖」 - a pinned whole-body attack at w=1 vs idle is a
        // two-writer shake).  endSlaveAnim/stopAnimation are no-ops on a name that is not playing.
        if (cAnim)
        {
            // Release the one rider-local host this transaction owns.  The broad name sweep below
            // remains a defensive fallback for legacy/stale entries, but it is not the primary path.
            const char* ownedNm = NULL;
            AnimationData* ownedHost = NULL;
            if (rideRt)
            {
                ResolveRideCombatClips(*rideRt, cAnim);
                ownedHost = RideCombatHost(*rideRt, &ownedNm);
            }
            ReleaseRideCombatHost(rider, cAnim, ownedHost, ownedNm);
            cAnim->stopAnimation(kP41kGuardAnim);
            cAnim->stopAnimation(kP41kBlowAnim);
            for (int ai = 0; ai < kRideAtkAnimCount; ++ai)
                cAnim->stopAnimation(kRideAtkAnim[ai]);
            rider->endSlaveAnim(kP41kGuardAnim);
            rider->endSlaveAnim(kP41kBlowAnim);
            for (int ai = 0; ai < kRideAtkAnimCount; ++ai)
                rider->endSlaveAnim(kRideAtkAnim[ai]);
            // Zero any residual SingleAnimation weight so idle is not still fighting a ghost
            // host (the post-dismount shake).  Idempotent; skips layers that are gone.
            if (cAnim->layer.valid())
            {
                unsigned int nlD = cAnim->layer.size();
                if (nlD > 0 && nlD <= 32)
                {
                    for (unsigned int li = 0; li < nlD; ++li)
                    {
                        AnimationClassBase::AnimationLayer* lay = cAnim->layer[li];
                        if (!lay || !lay->addList.valid()) continue;
                        unsigned int nD = lay->addList.size();
                        if (nD > 64) continue;
                        for (unsigned int ai2 = 0; ai2 < nD; ++ai2)
                        {
                            AnimationClassBase::SingleAnimation* sa = lay->addList[ai2];
                            if (!sa) continue;
                            bool isCmb = (sa->animName == kP41kGuardAnim)
                                      || (sa->animName == kP41kBlowAnim);
                            for (int k = 0; !isCmb && k < kRideAtkAnimCount; ++k)
                                isCmb = (sa->animName == kRideAtkAnim[k]);
                            if (!isCmb) continue;
                            sa->weight = 0.0f;
                            sa->desiredWeight = 0.0f;
                            if (sa->mainState) sa->mainState->setWeight(0.0f);
                        }
                    }
                }
            }
        }
        // 🆕 T22: and hand back the technique's own Ogre state, for the same reason and with the
        // same unconditional discipline - a dismount inside a swing window (knocked down, forced
        // off, player key) skips RideSwingPass's close edge, and an enabled state at weight 1
        // would ride along on a character that is walking again.  No-op when nothing was driven.
        RideCombatRuntime* dismountRt = FindRideCombatRuntime(rider);
        if (cAnim && dismountRt && dismountRt->techniqueName[0])
            RideSwingUndrive(cAnim, dismountRt->techniqueName);
        // The authored swing owns the upper-arm bones while its window is live.  A forced
        // dismount skips the normal close edge, so hand them back before clearing the latch.
        if (dismountRt)
            RideSwingArmRelease(cAnim);
    }
    // A forced dismount can bypass RideSwingPass's close edge.  Clear the cached target and
    // technique only after the old technique state has been handed back to the animation layer.
    // Keep the runtime until the per-ride summary below has been emitted.  The animation
    // state has already been handed back; erasing here would turn that summary into zeros.

    // stop the ride pose.  Only one pose exists now (P2-0, 2026-08-29); the companion
    // endSlaveAnim("idle_stand_normal") was deleted with the standing posture.  Ending an
    // animation that is not playing is a no-op, so nothing here depends on it having run.
    // 🆕 Option A: cushion rides end their own clip ('sitting idle'); endSlaveAnim on a
    // never-run name is a no-op, so a mismatched pair is still safe.
    {
        boost::unordered_map<Character*, Character*>::const_iterator dit2 = riderToMount.find(rider);
        Character* dm2 = (dit2 != riderToMount.end()) ? dit2->second : NULL;
        SeatInfo dSeat2;
        boost::unordered_map<Character*, SeatInfo>::const_iterator dSit2 = mountSeat.find(dm2);
        if (dm2 && dSit2 != mountSeat.end()) dSeat2 = dSit2->second;
        rider->endSlaveAnim(RidePoseNameForSeat(dm2, dSeat2));
    }

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
        p3Probe.erase(rider);
    }

    // P4-3-2: one summary per ride, unconditional.  `real` is the load-bearing number - it counts
    // sheathe calls that actually had a weapon in the hands, i.e. the ones the old probe attributed
    // to Character::_ragdollMode (real=16, median gap 22 frames) - so `real=0` means the fix never
    // bore load and a "weapon stayed in hand" reading proves nothing about it, exactly the way
    // `late=0` invalidates the mask rescue (RE_NOTES §21).  MEASURED trip 10: real=57 / pass=667 /
    // noop=311, and its only self-proving field is `real` - the P41E draw ladder can NOT serve as a
    // second one, because that ladder arms on empty hands while the suppressor only fires inside
    // the stance, so the two are disjoint by construction (all three trip-10 ladder runs started
    // 0.000-0.156 s AFTER `STANCE -> 0`).  P4-3-3 adds the second half of the story: `drawn`
    // counts stance-edge re-draws that succeeded, `fail` the ones the engine refused.
    {
        char shs[224];
        _snprintf_s(shs, 224, _TRUNCATE,
            "Riding: P43SUP ride rider=%p real=%d noop=%d pass=%d | P43RD drawn=%d fail=%d nowpn=%d",
            (void*)rider,
            rideRt ? rideRt->shSupReal : 0,
            rideRt ? rideRt->shSupNoop : 0,
            rideRt ? rideRt->shPass : 0,
            drawRt ? drawRt->ok : 0, drawRt ? drawRt->fail : 0, drawRt ? drawRt->noWpn : 0);
        DebugLog(std::string(shs));
    }
    // P4-3-4: the swing's own per-ride summary, unconditional and on its own line so it can be read
    // without disturbing the P43SUP judge. `swing=` is the load-bearing number - swing=0 means no
    // attack transaction opened this ride. `rst=` is the serial-latched restart decision and must
    // equal swing: each transaction gets exactly one decision, whether it reused or created its
    // rider-local vanilla attack entry. `hostkeep=` retains its historical key for compatibility but
    // now counts frames that pinned the current attack host, not guard frames. `arm=`/`swfree=` belong
    // to the retired authored-arm route and must remain zero on this route.
    {
        // 🆕 P4-6ag: the mix's own frequency table for this ride ("every clip, how many windows").
        // The 24-line budget only ever shows the first rows of a ride, so this is the ONLY place
        // the real distribution is visible - it is what answers 「每种动作出现的频率」.
        char mixv[160];
        mixv[0] = 0;
        {
            int mo = 0;
            for (int i = 0; i < kRideMixCount; ++i)
            {
                if (mo >= (int)sizeof(mixv) - 8) break;
                int n = _snprintf_s(mixv + mo, sizeof(mixv) - (size_t)mo, _TRUNCATE, "%s%d",
                                    (i == 0) ? "" : ",", rideRt ? rideRt->mixCount[i] : 0);
                if (n < 0) break;
                mo += n;
            }
        }
        char sws[512];
        _snprintf_s(sws, 512, _TRUNCATE,
            "Riding: P43SW ride rider=%p swing=%d rst=%d drv=%d arm=%d postarm=%d swfree=%d tech=%d skip=%d noclip=%d "
            "hostkeep=%d hdveto=%d dmin=%.2f limlast=%.2f aspd=%.3f gap=%d hskipN=%d face=%d/%d mix=[%s]",
            (void*)rider,
            rideRt ? rideRt->swingCount : 0,
            rideRt ? rideRt->restarts : 0,
            rideRt ? rideRt->driveFrames : 0,
            rideRt ? rideRt->armFrames : 0,
            rideRt ? rideRt->postArm : 0,
            rideRt ? rideRt->freeFrames : 0,
            rideRt ? rideRt->techCount : 0,
            rideRt ? rideRt->skipCount : 0,
            rideRt ? rideRt->noClipCount : 0,
            rideRt ? rideRt->hostKeepFrames : 0,
            rideRt ? rideRt->headVeto : 0,
            rideRt ? rideRt->minDistance : -1.0f,
            rideRt ? rideRt->lastLimit : -1.0f,
            rideRt ? rideRt->attackSpeed : 1.0f,
            rideRt ? rideRt->gapMs : kRideSwingMinGapMs,
            rideRt ? rideRt->hitSkips : 0,
            rideRt ? rideRt->faceFrames : 0,
            rideRt ? rideRt->faceDegMax : 0,
            mixv);
        DebugLog(std::string(sws));
    }
    ClearRideCombatRuntime(rider);
    ClearRideStanceRuntime(rider);
    ClearRideStanceDrawRuntime(rider);
    ClearRideMaskRuntime(rider);
    ClearRidePoseRuntime(rider);
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

    // 🆕 Option A: same per-style pose choice as a fresh Mount() - cushion mounts come back
    // from a load on 'sitting idle', everything else on kRidePose.
    rider->runSlaveAnim(RidePoseNameForSeat(mount, seat), 1.0f, 1.0f);
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
    p3Probe.erase(rider);
    gP3Budget = kP3LogBudget;        // P3-0: fresh line budget for this ride
    gP3HullCreates = 0;              // P3-2: rebuilds this ride (~1 per 30 frames)
    gCmbBudget = kCmbBudget;         // P3-3: combat-visibility lines for this ride
    gCmbSig    = -1;
    gAtkTries  = kAtkTryBudget;      // P4-1b: hand-issued attack orders for this ride
    gAtkReads  = kAtkReadBudget;
    gAtkLastFrame = 0;
    gAtkStage  = 0;
    gAtkCurRung = -1;                // P4-1d: nothing in flight, every rung re-armed
    for (int p41dRung = 0; p41dRung < kAtkStages; ++p41dRung) gRungDead[p41dRung] = false;
    gDrawTries = kDrawTryBudget;     // P4-1e: re-arm the draw attempts for this ride
    gDrawCalls = 0;
    gDrawNoWpn = 0;
    gDrawLastFrame = 0;
    gInvDumped = 0;
    gArmBudget = kArmBudget;         // P4-1e-2: arm/aggro state lines for this ride
    gArmSig    = -1;
    gEdgeBudget = kEdgeBudget;       // P4-1f: weapon-state edges for this ride
    gEdgeWpn    = -1;
    gEdgeACW    = -99;
    gArmDumpBudget = kArmDumpBudget;  // P4-1g: layer dumps for this ride
    gArmDumpLeft   = kArmDumpBase;    // a short baseline before the first forced draw
    gArmDumpCmbBudget  = kArmDumpCmbBudget;  // P4-1h: combat-only dumps, own budget
    gArmDumpCmbEntries = -1;
    gRideStanceOn      = false;
    ClearRideStanceRuntime(rider);           // P4-1N: no release tail into a new ride
    ClearRideStanceDrawRuntime(rider);
    ResetRideSwingWindow(rider);
    // All leg-pose state is keyed by rider.  A fresh load must not inherit a snapshot,
    // manual flag, resolved handle, or diagnostic budget from another rider.
    ClearRidePoseRuntime(rider);
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

// The whole order handler lives in an Impl so the hook can hold the SEH shell (C2712:
// __try cannot share a frame with unwindable objects, and this body builds std::strings).
static void NewPlayerTaskImpl(PlayerInterface* thisptr, TaskType t, const hand& targetH, Building* destinationIndoors, const Ogre::Vector3& clickpos, bool addDontClear)
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

    // Attack order while riding: the mount engages the ordered target too.
    //
    // ⚠️ P4-2 (2026-08-30) dropped the IsBigMount() condition that used to sit here.  On the
    // big tier the per-frame controller keeps the rider passive, so the mount is the only one
    // that can act on the order; on the small tier 已定决策「骑手与坐骑都出手」 wants BOTH, and
    // this hook does not swallow the order (it falls through to newPlayerTask_orig below), so
    // the rider's own attack order still reaches the engine either way.
    if (target && IsAttackTask(t))
    {
        ogre_unordered_set<hand>::type::iterator sit = thisptr->selectedCharacters.begin();
        for (; sit != thisptr->selectedCharacters.end(); ++sit)
        {
            Character* c = sit->getCharacter();
            if (!c) continue;
            Character* mount = GetMount(c);
            if (!mount) continue;
            // Only that the mount is one we are tracking - no seat/size condition.  A mount
            // with no seat entry is skipped because we would be ordering a stranger around.
            if (mountSeat.find(mount) == mountSeat.end()) continue;
            if (mount->getAttackTarget().getCharacter() != target)
                mount->attackTarget(target);
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

// SEH shell: same rationale as mainLoop_hook and animUpdate_hook (2026-09-08, after a
// player-reported "sometimes crashes on mount/dismount").  This hook is where the two
// heaviest native engine calls outside the per-frame passes actually run - Mount()'s
// pickupObject/dropCarriedObject right here when the rider is already in range, and
// Dismount()'s getDropped/ragdollMode on the "put down" order - and it was the one
// registered hook that never got the shell the per-frame ones have.  Every Mount()/
// Dismount() call site is under an SEH shell after this: the pendingMount boarding and
// the forced dismount both run inside mainLoop_hook's, and the load restore inside
// animUpdate_hook's.  On a fault the order is swallowed - no newPlayerTask_orig, no
// WipeAllRideState: the pointers here come from the live selection, not from a
// post-load world reset, so wiping everyone's state for one bad order is the wrong
// tool, and the mainLoop shell remains the second net for whatever an interrupted
// Mount() left behind.  Worst case after this shell is a stuck rider, not a crash.
void newPlayerTask_hook(PlayerInterface* thisptr, TaskType t, const hand& targetH, Building* destinationIndoors, const Ogre::Vector3& clickpos, bool addDontClear)
{
    __try
    {
        NewPlayerTaskImpl(thisptr, t, targetH, destinationIndoors, clickpos, addDontClear);
    }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        DebugLog("Riding: access violation in newPlayerTask - order swallowed");
    }
}

// ---- pose weight pin + animation layer dump (v1.6, 2026-08-29) ------------
//
// v1.5 pinned the REQUEST (unconditional runAnimation every frame) and that did
// kill the old two-frame 1.00/0.96 limit cycle.  A residual survived it: exactly
// 57 one-frame dips to 0.95-0.97 in 1975 settled frames, spaced a rock-steady
// 35 frames apart (0.267 s at the 131 fps that run was measured on).  Locked to
// a FRAME count, not to wall time, so it is somebody's counter, not a physical
// cadence.  `act` (total weight) dips with it while `oth` stays 0.00, so nothing
// was added - our own pose simply loses 3-5% for one frame, i.e. that fraction
// of the skeleton renders as whatever the layer falls back to.  Vanilla's
// "sitting chair" barely moves Bip01 so nobody ever saw it; Wakigawa's Animation
// Overhaul (workshop 2810262017) moves the root ~3.7u per unit weight, so the
// same dip is a visible flick four times a second.
//
// Why a request cannot fix it: runAnimation only sets desiredWeight, which the
// NEXT animUpdate fades toward.  A dip that happens inside that update is
// already rendered by the time our next request lands.  So write the fields:
//   weight / desiredWeight / stillWanted   engine-side truth for the fade
//   mainState->setWeight()                 render-side truth for THIS frame
//                                          (Ogre blends from the AnimationState
//                                          at render time, i.e. after mainLoop)
// Deliberate limits:
//   - only pins once the pose is past 0.5, so the mount crossfade (the standing
//     idle takes ~21 frames to fade out) still happens instead of snapping;
//   - render-side write only when nothing else carries weight, so a combat swing
//     or any future blend is never pushed over 1.0 total.
// The POSEDUMP half of this pass (a budgeted, debugContinuous-gated walk that PRINTED every
// addList/removeList entry on the dip frames) is gone as of the probe-free build: it had already
// named the drainer, and T2/T9 closed with `0.94-0.995` at 0 frames.  What stays is the walk
// itself - `others` is the INPUT to the render-side gate below, not diagnostics.
static void PoseLayerPin(AnimationClass* rAnim, AnimationData* poseData,
                         bool renderSide)
{
    if (!rAnim || !poseData || !rAnim->layer.valid()) return;
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return;                 // garbage/dangling guard

    AnimationClassBase::SingleAnimation* mine = NULL;
    float others = 0.0f;

    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay) continue;
        for (int pass = 0; pass < 2; ++pass)
        {
            lektor<AnimationClassBase::SingleAnimation*>& lst =
                pass ? lay->removeList : lay->addList;
            if (!lst.valid()) continue;
            unsigned int n = lst.size();
            if (n > 64) continue;
            for (unsigned int ai = 0; ai < n; ++ai)
            {
                AnimationClassBase::SingleAnimation* sa = lst[ai];
                if (!sa) continue;
                bool isMine = (pass == 0 && sa->animationData == poseData);
                if (isMine) mine = sa;
                else others += sa->weight;
            }
        }
    }

    if (mine && mine->weight >= 0.5f)
    {
        mine->desiredWeight = 1.0f;
        mine->weight = 1.0f;
        mine->stillWanted = true;
        if (renderSide && mine->mainState && others < 0.02f)
            mine->mainState->setWeight(1.0f);
    }
}

// Same trick as PoseLayerPin but for an ARBITRARY clip at an ARBITRARY weight, which is
// what route C needs (ride pose + combat stance both held at 0.5).  It is a separate
// function on purpose - PoseLayerPin is shipping code with two hardcoded 1.0s and a
// render-side condition (others < 0.02f) that is deliberately FALSE the moment two clips
// share the body, so route C cannot be expressed by parameterising it without putting the
// shipping pose at risk.
//
// Why a pin rather than a request: runAnimation's two floats are speed and blend, and
// runSlaveAnim's startWeight is only a starting point - there is no API that asks for a
// steady 0.5.  So the request stays normal and the pin owns the weight.
//   arm gate:  weight >= target * 0.5f   (same "let the crossfade happen" idea as the
//              0.5 in PoseLayerPin, expressed relative to the target)
//   render:    only when target + others <= 1.02f, so we never push total weight past 1
static void ClipPin(AnimationClass* rAnim, AnimationData* ad, float target, bool renderSide)
{
    if (!rAnim || !ad || target <= 0.0f || !rAnim->layer.valid()) return;
    unsigned int nl = rAnim->layer.size();
    if (nl == 0 || nl > 32) return;                 // garbage/dangling guard, as above

    AnimationClassBase::SingleAnimation* mine = NULL;
    float others = 0.0f;

    for (unsigned int li = 0; li < nl; ++li)
    {
        AnimationClassBase::AnimationLayer* lay = rAnim->layer[li];
        if (!lay) continue;
        for (int pass = 0; pass < 2; ++pass)
        {
            lektor<AnimationClassBase::SingleAnimation*>& lst =
                pass ? lay->removeList : lay->addList;
            if (!lst.valid()) continue;
            unsigned int n = lst.size();
            if (n > 64) continue;
            for (unsigned int ai = 0; ai < n; ++ai)
            {
                AnimationClassBase::SingleAnimation* sa = lst[ai];
                if (!sa) continue;
                if (pass == 0 && sa->animationData == ad) mine = sa;
                else others += sa->weight;
            }
        }
    }

    if (mine && mine->weight >= target * 0.5f)
    {
        mine->desiredWeight = target;
        mine->weight = target;
        mine->stillWanted = true;
        if (renderSide && mine->mainState && (target + others) <= 1.02f)
            mine->mainState->setWeight(target);
    }
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
                        // 🆕 Option A: same per-style name as HaltAndForceSitPass's reassert
                        // (both read the same mountSeat entry this frame, so the two sites can
                        // never disagree on the name).
                        AnimationData* poseData =
                            FindAnimData(thisptr, RidePoseNameForSeat(mount, sit->second));
                        // 🆕 P4-6af: the stance swaps the BASE host again.  Out of combat the ride
                        // sit pose owns the body; in combat the upper body is held by the combat
                        // idle stance 'guard 1h', so the swing window starts from a weapon stance
                        // instead of from a seated torso.  The lower body is LegPosePass's either
                        // way.  ⚠️ This pass must make the SAME choice as HaltAndForceSitPass:
                        // both resolve it through RideCombatHost, and both fall back to the sit
                        // pose when that record is absent.
                        // `advance=false`: HaltAndForceSitPass owns the once-per-frame hold
                        // counter, this pass only reads its decision.
                        bool stance = RideCombatStance(ch, mount, sit->second, false);
                        Character* stWho = thisptr ? thisptr->me : NULL;
                        RideCombatRuntime* wrt = FindRideCombatRuntime(stWho);
                        AnimationData* hostWant = NULL;
                        if (stance && wrt)
                        {
                            // P4-6ag: resolve + print the P41K evidence line here as well - this
                            // pass runs first in a frame, so it is usually the one that latches.
                            ResolveRideCombatClipsLogged(*wrt, thisptr, stWho);
                            hostWant = RideCombatHost(*wrt, NULL);
                        }
                        // stop the animation system from choosing the carried pose.
                        // Unconditional in both branches: the carried pose has to go whether the
                        // torso ends up on the ride pose or on the combat stance.
                        thisptr->animationRequirements.carried = false;
                        if (hostWant)
                        {
                            // Route A: the combat stance owns the whole torso, the legs are ours
                            // via manual bones.  Keep P4-1j's zeroing of the slave channel.
                            thisptr->animationRequirements.forcedSlaveLoop = NULL;
                            thisptr->animationRequirements.isActionSlave   = false;
                            // ...and pin the stance's weight fields BEFORE the engine's own update
                            // fades them (the render-side half is HaltAndForceSitPass's ClipPin).
                            ClipPin(thisptr, hostWant, 1.0f, false);
                        }
                        else
                        {
                            // Out of combat - or a rider whose table holds no 'guard 1h' record:
                            // the sit pose stays the base exactly as it was in P4-6ad.
                            if (poseData)
                                thisptr->animationRequirements.forcedSlaveLoop = poseData;
                            PoseLayerPin(thisptr, poseData, false);
                        }
                        if (wrt && wrt->openTick != 0 && wrt->techniqueName[0])
                        {
                            // P4-6Y: Drive BEFORE animUpdate_orig so this frame's apply
                            // includes hand/forearm.  P4-6ab: freeze on pause.
                            DWORD el = RideSwingFrozenElapsed(*wrt, GetTickCount());
                            if (!ou || !ou->isPaused())
                            {
                                if (RideSwingDrive(thisptr, wrt->techniqueName, el))
                                    ++wrt->driveFrames;
                            }
                        }
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
            if (!SeatNeedsPlacement(seat)
                && !RideCombatStance(rider, mount, seat, false)) continue;

            // Both placement writers use the same final target, including the unscaled rider
            // combat drop, so neither can win the frame with a different Y.
            Ogre::Vector3 seatPos = ComputeRiderSeatPos(rider, mount, seat);
            if (rider->getMovement())
            {
                // P3-0 probe: bracket THIS write.  aPre is the drag that happened
                // between the previous main-loop write and now (i.e. the early part of
                // the engine's update phase); aPost proves the write still lands here
                // too, on the same destroyed movement.
                CharMovement* rMv = rider->getMovement();
                P3Sample& p3 = p3Probe[rider];
                p3.aPre = rMv->getPosition() - seatPos;
                rMv->_setPositionSimple(seatPos);
                p3.aPost = rMv->getPosition() - seatPos;
                ++p3.animHits;
                p3.sawAnim = true;
            }
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
        WipeAllRideState("access violation in animUpdate", true);
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
        WipeAllRideState("tracked character vanished (world reset?)", false);
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
            // the ANIM_CARRIED "limp, limbs dangling" pose) and re-assert the ride pose
            // after the game's own update.
            Character* mount = it->second;
            if (mount)
            {
                boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
                if (sit != mountSeat.end() && sit->second.forceSit)
                {
                    AnimationClass* rAnim = rider->getAnimationClass();
                    if (rAnim)
                    {
                        // P4-1M: route A ships.  See RideCombatStance() - eligible mount, rider in
                        // combat mode, AND a live threat within kRideThreatDist, with NO
                        // debugContinuous gate.  Route C is gone.  `advance=true`: this is the
                        // once-per-frame caller that ticks the release tail.
                        bool stance = RideCombatStance(rider, mount, sit->second, true);
                        gRideStanceOn = stance;   // read by the DBG tag, nothing else
                        // P4-3-3: latch the 0 -> 1 edge for the one-shot re-draw.  UNGATED, and
                        // kept separate from the per-rider diagnostic latch below, which only
                        // advances inside the debugContinuous block - toggling diagnostics must never change whether
                        // the rider arms itself (same discipline as the stance itself).  Whose
                        // edge it is travels with the flag; this loop walks every tracked pair.
                        RideStanceDrawRuntime& drawRt = GetRideStanceDrawRuntime(rider);
                        if (stance && !drawRt.prev) drawRt.pending = true;
                        drawRt.prev = stance;
                        RideStanceRuntime* stanceRt = FindRideStanceRuntime(rider);
                        if (debugContinuous && stanceRt
                            && (stance ? 1 : 0) != stanceRt->last)
                        {
                            stanceRt->last = stance ? 1 : 0;
                            // d= / holdms= are how a stuck stance is diagnosed now: cm=1 with no
                            // threat (d=-1) is the engine flag lingering and OUR term doing its
                            // job; cm=1 with a real d beyond kRideThreatDist is a fight that
                            // moved away; holdms= counting down is the tail, not a fault.
                            // ⚠️ holdms is WALL-CLOCK MILLISECONDS since this build (it was
                            // frames, printed as hold=, up to and including 297472 B).
                            float td = -1.0f;
                            RideNearestThreat(rider, mount, &td);
                            char pl[160];
                            _snprintf_s(pl, 160, _TRUNCATE,
                                "Riding: STANCE %d f=%u cm=%d d=%.1f holdms=%d",
                                stance ? 1 : 0, gP3Frames,
                                rider->isInCombatMode(true, true) ? 1 : 0, td,
                                stanceRt->holdMs);
                            DebugLog(std::string(pl));
                        }
                        // Unconditional: both branches want the carried pose gone and
                        // "carry me" evicted.
                        rAnim->animationRequirements.carried = false;
                        // The carry system force-plays "carry me" (the being-carried
                        // pose) every frame at 0.5 weight, which blends with and ruins
                        // our pose.  Kick it out after the game's update so our pose
                        // owns the full animation weight.
                        rAnim->stopAnimation("carry me");
                        // Pose weight has to be held CONSTANT, not merely high (2026-08-29).
                        //
                        // The old code re-asserted only when the weight had already sagged
                        // below 0.99.  That gate is itself an oscillator: runAnimation sets a
                        // TARGET that the next animUpdate applies, so the steady state is a
                        // two-frame limit cycle - measure 0.96, re-assert, next frame reads
                        // 1.00, skip, the carry system's per-frame dilution takes it back to
                        // 0.96, repeat.  Measured in both a vanilla run and a run with
                        // Wakigawa's Animation Overhaul (workshop 2810262017): weight
                        // alternates 1.00 / 0.96 forever, identically, in both.
                        //
                        // Why the 4% matters: SyncRiderNode measures boneLocal BEFORE this
                        // frame's skeleton update and solves the node from it, so any pose
                        // change between the measurement and the render lands on rBip
                        // uncancelled.  Vanilla "sitting chair" barely moves Bip01, so the
                        // 4% dip costs ~0.01u and nobody ever saw it (1797 of 1800 frames had
                        // rel.y inside 5.90-5.94).  The overhaul's sitting pose moves the root
                        // bone ~3.7u per unit weight, so the SAME dip became rel.y alternating
                        // 5.99 <-> 6.14 every frame = a 0.3u peak-to-peak vibration at frame
                        // rate.  That is the shake players report with that mod, and the fix
                        // belongs here rather than in the position solver: with the weight
                        // pinned there is no delta left to cancel.
                        //
                        // So: top the animation layer up EVERY frame (unconditional), and keep
                        // the weight test on runSlaveAnim only.  The historical reason for the
                        // gate - "re-adding the pose every frame resets a live idle LOOP's
                        // playback, so a standing rider trembles" - was blamed on the wrong
                        // call.  Control-run evidence that runAnimation does NOT reset
                        // progress: re-asserts landed on the frames reading progress 1.00 and
                        // 0.50, and the following frames read 0.24 and 0.76 - the loop kept
                        // advancing ~0.26/frame straight through them.  And since P2-0
                        // (2026-08-29) deleted the standing posture there is no live idle loop
                        // in the ride path at all, so that whole class of risk is gone.
                        // runSlaveAnim stays gated because it is the untested half (and the
                        // slave side is already maintained every frame by
                        // animationRequirements.forcedSlaveLoop in the animUpdate pre-pass).
                        // 🆕 Option A: the pose NAME is per-style - cushion mounts pin
                        // 'sitting idle' (same record the leg table was baked from), so the
                        // getAnimationData below stays miss-free for BOTH names (each is a
                        // verified vanilla record) and poseData flowing into PoseLayerPin and
                        // LegPosePass follows the switch automatically.  sit->second is the
                        // seat this frame's decision was made from; mount is in scope above.
                        const char* poseName = RidePoseNameForSeat(mount, sit->second);
                        AnimationData* poseData = FindAnimData(rAnim, poseName);
                        // 🆕 P4-6af: in stance the pose channel is handed back entirely - the
                        // combat idle stance owns the torso and LegPosePass owns the legs, exactly
                        // the way the ride pose owns them out of combat.  `want` is resolved
                        // through the same resolver the pre-pass uses, so the two sites can never
                        // pin two different base clips in one frame.
                        RideCombatRuntime* hostState = NULL;
                        AnimationData* want   = NULL;
                        const char*    wantNm = NULL;
                        if (stance)
                        {
                            hostState = &GetRideCombatRuntime(rider, mount);
                            // Resolve + print the P41K evidence line (P4-6ag: the helper makes the
                            // pre-pass and this pass share one site, so whichever runs first logs).
                            ResolveRideCombatClipsLogged(*hostState, rAnim, rider);
                            want = RideCombatHost(*hostState, &wantNm);
                        }
                        if (want)
                        {
                            // Hostkeep= is this half's self-proving field: frames on which the
                            // render-side pin saw a live window and still kept the base host up.
                            // It must be > 0 on every ride that fired a swing.
                            if (hostState && RideSwingInFlight(rider))
                                ++hostState->hostKeepFrames;
                            // Requested EVERY frame, exactly the way the ride pose is requested
                            // out of stance: a single request would not distinguish "refused" from
                            // "accepted then continuously drained".
                            rAnim->runAnimation(want, 1.0f, 1.0f);
                            // Pin every frame, INCLUDING while a window is open: the technique
                            // drives its own Ogre state (never a layer entry), so it does not evict
                            // or outrank this host - the free-bone mask is what hands the upper
                            // body over, and it is written by LegMaskApply below.
                            ClipPin(rAnim, want, 1.0f, true);
                        }
                        else
                        {
                            // Out of combat - or a rider whose table holds no 'guard 1h' record:
                            // the sit pose is the base (P4-6ad's shape).  The fallback keeps the
                            // skeleton from ever going hostless (P4-6ae's 「立直不动」).
                            if (!poseData || !rAnim->getAnimationPlaying(poseData)
                              || rAnim->getAnimationCurrentWeight(poseData) < 0.99f)
                                rAnim->runSlaveAnim(poseName, 1.0f, 1.0f, 1.0f);
                            rAnim->runAnimation(poseName, 1.0f, 1.0f);
                            // Render-side Ogre weight pin (v1.6).  The stance base is pinned by
                            // ClipPin instead; PoseLayerPin's `others < 0.02f` door is exactly why
                            // the two are separate functions.
                            if (poseData)
                                PoseLayerPin(rAnim, poseData, true);
                        }
                        // P2-1b-3 straddle.  Same window as the render-side weight pin.
                        LegPosePass(rAnim, poseData, rider, mount, sit->second, stance);
                    }
                    ApplyRiderOrientation(rider, sit->second, mount);
                }
            }
        }
    }
}

// ---- Rider click hull while mounted (P3-1 fix, P3-2 churn cut) ---------------------
// The mouse does not pick characters through the movement position at all - it picks
// through CharMovement::clickHull (@0x3B0), a separate physics object.
//
// P3-0 (2026-08-29) settled what the state actually is, and it is the worse of the two
// possibilities: 105/105 sampled frames read has=0 / p=0000000000000000, i.e. the rider
// has NO click hull at all while mounted - it died with the CharMovement that
// pickupObject destroy()ed.  So the old teleportCollisionHull() action never even ran
// (it was gated on hasClickHull()), and 点不中 was never a position problem: getAABB()
// returned a centre exactly equal to seatPos on all 105 lines, +-(2.50,8.50,2.50).
// There was nothing to move; there was nothing there.
//
// P3-1 (2026-08-29) fixed it with one line: call refreshClickHull() even when clickHull
// is NULL and the engine builds one.  79/79 sampled lines then read has0=1, and the
// user could click the rider.  restore() was NOT needed, so we keep the property the
// carry-dissolve architecture bought ("the rider is not shoved around by the mount's
// collision body").  nrc (dontEverRecreateMe, 0x330) read 0 throughout, i.e. restore()
// was not being blocked either - it is simply unnecessary.
//
// ⚠️ P3-2 is about what P3-1 also showed: p0 != p2 on every single line, and no two
// lines shared a pointer, while the mount's own hull pointer never changed all session.
// refreshClickHull() DESTROYS AND REBUILDS the hull; it is not an update.  P3-1 called
// it every frame, so we were allocating and freeing a physics hull 60-130 times a
// second for no reason.  So: create it once (only when it is missing) and just
// teleportCollisionHull() it to the seat every frame.
//   Judging the log: creates= should be 1 per ride and p0 == p2 == the same pointer on
//   every line.  creates= climbing with the frame count means something destroys the
//   hull every frame and the per-frame rebuild was load-bearing after all (put it back
//   and accept the allocation).  Clicking breaking while creates=1 and the pointer is
//   stable means teleport alone does not move the hull for picking, i.e. the rebuild is
//   what re-placed it - same fix, same cost note.
// ⚠️ getAABB() cannot answer where the hull is: in P3-0 its centre equalled seatPos
// while clickHull was NULL, so it is derived from the movement position.  PhysicsHullT
// is only forward-declared in KenshiLib, so the hull's own transform is not readable
// from here; "does the click land on the rider" stays an in-game observation.
//
// The mount's own hull is logged next to the rider's as a control group: the mount IS
// clickable, so mHas=1/mp!=0 proves hasClickHull() reports honestly and has0=0 was a
// real absence rather than an API that always says no.
//
// Separate Impl/wrapper pair on purpose: the __try shell cannot live in a function
// that needs object unwinding (C2712, and SyncMountedRiders holds map iterators), and
// an AV inside the engine's hull code must cost a log line, not the session.
static void P3HullPassImpl(CharMovement* rMv, CharMovement* mMv,
                           const Ogre::Vector3& seatPos, bool logNow)
{
    bool  has0 = rMv->hasClickHull();
    void* hp0  = (void*)rMv->clickHull;
    bool  nrc  = rMv->dontEverRecreateMe;

    // The P3-1 fix: build one when there is none.  Rebuilds are counted, not repeated -
    // see the P3-2 note above for why calling this every frame was wrong.
    if (!has0)
    {
        rMv->refreshClickHull();
        ++gP3HullCreates;
    }

    bool has1 = rMv->hasClickHull();
    if (has1)
        rMv->teleportCollisionHull(seatPos);

    void* hp2 = (void*)rMv->clickHull;
    Ogre::Aabb box = rMv->getAABB();

    bool  mHas = false;
    void* mhp  = 0;
    if (mMv) { mHas = mMv->hasClickHull(); mhp = (void*)mMv->clickHull; }

    if (!logNow) return;
    char b[512];
    _snprintf_s(b, 512, _TRUNCATE,
        "Riding: P3HULL has0=%d p0=%p nrc=%d -> has1=%d p2=%p creates=%d | mHas=%d mp=%p "
        "seat=(%.2f,%.2f,%.2f) box=(%.2f,%.2f,%.2f)+-(%.2f,%.2f,%.2f)",
        has0 ? 1 : 0, hp0, nrc ? 1 : 0, has1 ? 1 : 0, hp2, gP3HullCreates,
        mHas ? 1 : 0, mhp,
        seatPos.x, seatPos.y, seatPos.z,
        box.mCenter.x, box.mCenter.y, box.mCenter.z,
        box.mHalfSize.x, box.mHalfSize.y, box.mHalfSize.z);
    DebugLog(std::string(b));
}

static void P3HullPass(CharMovement* rMv, CharMovement* mMv,
                       const Ogre::Vector3& seatPos, bool logNow)
{
    __try { P3HullPassImpl(rMv, mMv, seatPos, logNow); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { DebugLog("Riding: P3HULL access violation - hull probe abandoned"); }
}

// One P3 timeline line: the four (movement position - seat) samples of this frame.
// Own function for the same C2712 reason as above (std::string temporaries).
static void P3LogTimeline(const P3Sample& p3)
{
    char b[512];
    _snprintf_s(b, 512, _TRUNCATE,
        "Riding: P3 f=%u hits=%d sawA=%d aPre=(%.2f,%.2f,%.2f) aPost=(%.2f,%.2f,%.2f) "
        "mPre=(%.2f,%.2f,%.2f) mPost=(%.2f,%.2f,%.2f) |aPre|=%.2f |mPre|=%.2f",
        gP3Frames, p3.animHits, p3.sawAnim ? 1 : 0,
        p3.aPre.x,  p3.aPre.y,  p3.aPre.z,
        p3.aPost.x, p3.aPost.y, p3.aPost.z,
        p3.mPre.x,  p3.mPre.y,  p3.mPre.z,
        p3.mPost.x, p3.mPost.y, p3.mPost.z,
        p3.aPre.length(), p3.mPre.length());
    DebugLog(std::string(b));
}

// 1b) Apply the computed seat position.  We run after the game's own update,
//     so our offset on top of the slave attachment sticks until next frame.
//     Skipped when there is nothing to correct (small/medium animals).
static void SyncMountedRiders()
{
    ++gP3Frames;   // P3-0 probe cadence, counted whether or not anyone is mounted
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

            bool combatPlacement = RideCombatStance(rider, mount, seat, false);
            if (!SeatNeedsPlacement(seat) && !combatPlacement)
                continue; // exact attach only needs correction while applying the combat drop

            Ogre::Vector3 seatPos = ComputeRiderSeatPos(rider, mount, seat);

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
            // the body went - and _setPositionSimple might not even be landing, being
            // called on a movement that pickupObject DESTROYED.
            //
            // 2026-08-29, mvW settled the second half: ZERO on all 19511 frames of the v1.6
            // log, i.e. the write DOES land and reads back exactly.  So this is a DRAG-BACK
            // problem, not a no-op problem, and the "next lever is teleportCollisionHull"
            // note that used to sit here was reasoning from the wrong premise.  Note also
            // that this pass is already the last writer before render and the value is
            // still back at the slot next frame => the drag happens in the NEXT frame's
            // update phase; moving this write later cannot fix it on its own.
            //
            // P3-0 (below) brackets both of our writes to find out WHERE in the frame that
            // happens, because no existing log can: DebugLogRideFrame above samples rMove
            // BEFORE this frame's own write, so its rMove field is last frame's post-drag
            // value.  The prime suspect is the engine's own beingCarriedUpdate, which we
            // are forbidden from hooking (see the DISABLED HOOKS block).
            //
            // There is no setCurrentPosition to write the field behind getPosition():
            // that name belongs to FlockingTools (get-out-of-the-way state), not to
            // AbstractMovementBase.  The field itself is AbstractMovementBase::pos
            // (+0xC4, public) if it ever comes to poking it directly.
            // P3-0 result (2026-08-29, 105 sampled frames over 2 rides, all identical):
            // aPre = ZERO on every line, mPre = 7.6..13.5u off.  So the drag-back happens
            // in the part of the engine update that runs AFTER the rider's animUpdate, and
            // nothing touches the position between this pass and the next frame's
            // animUpdate.  The rider IS on the saddle at render time; the carry slot only
            // owns the tail of the update phase.  hits=2 on every line (the resync fires
            // twice per frame: once for the mount's animUpdate, once for the rider's).
            if (rider->getMovement())
            {
                CharMovement* rMv = rider->getMovement();
                P3Sample& p3 = p3Probe[rider];
                p3.mPre = rMv->getPosition() - seatPos;
                rMv->_setPositionSimple(seatPos);
                p3.mPost = rMv->getPosition() - seatPos;
                dbgMoveWritten[rider] = p3.mPost;

                bool p3Log = debugContinuous && gP3Budget > 0
                             && (gP3Frames % kP3LogGap) == 0;
                if (p3Log)
                {
                    --gP3Budget;
                    P3LogTimeline(p3);
                }
                // Was `if (hasClickHull()) refreshClickHull();`, which never fired: the
                // mounted rider has no hull to refresh.  This pass builds one and keeps
                // it on the seat - it is the 「骑手点不中」 fix, not a probe.
                P3HullPass(rMv, mount->getMovement(), seatPos, p3Log);
                p3.animHits = 0;   // per-frame count, consumed by the line above
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

// P3-3 / P4-1: read out what the combat system thinks of a mounted rider.  Pure probe -
// nothing here writes.  See the kCmbBudget block near the top of the file for the
// pre-registered readings; the short version is that getAllAttackers() on the rider is the
// engine's own answer to 「敌人会不会走过来打骑手」, so one fight decides whether the
// 鞍座 XZ + 贴地 Y lever is needed before we write a line of it.
//
// Distances are logged three ways because that is what discriminates the failure modes:
//   dR3/dRxz  nearest attacker to the rider's LOGICAL position (what combat reach uses)
//   dB3       nearest attacker to the rider's RENDER root bone (where the player sees them)
//   dM3/dMxz  nearest attacker to the mount - the control group.  The mount is a normal
//             character with a live CharMovement, so if the mount is engaged and the rider
//             is not, the difference is about the rider, not about the fight.
// A large dR3 with a small dRxz is the vertical-reach story the lever would fix; both small
// with rAtk=0 means targeting rejects the rider for some other reason and the lever is a
// waste.  SEH shell for the same reason as P3HULL: DebugLog takes a std::string, so the
// string temporaries cannot live inside __try (C2712).
static void CombatProbeImpl(Character* rider, Character* mount, bool neck)
{
    lektor<hand> rAtk;  rider->getAllAttackers(rAtk);
    lektor<hand> mAtk;  mount->getAllAttackers(mAtk);
    int rn = (int)rAtk.size();
    int mn = (int)mAtk.size();

    bool cm   = rider->isInCombatMode(true, true);
    bool cmM  = rider->isInCombatMode(true, false);
    bool rTgt = rider->getAttackTarget().getCharacter() != NULL;
    bool mTgt = mount->getAttackTarget().getCharacter() != NULL;
    bool cc   = rider->getCombatClass() != NULL;
    bool bc   = rider->_isBeingCarried ? true : false;
    bool dn   = rider->isDown();

    // Signature: log on any change, plus a periodic baseline.  Counts are clamped so a big
    // brawl cannot alias two different states onto the same signature bits.
    int sig = (rn > 7 ? 7 : rn) | ((mn > 7 ? 7 : mn) << 3)
            | (cm ? 0x40 : 0) | (cmM ? 0x80 : 0) | (rTgt ? 0x100 : 0) | (mTgt ? 0x200 : 0)
            | (cc ? 0x400 : 0) | (bc ? 0x800 : 0) | (dn ? 0x1000 : 0) | (neck ? 0x2000 : 0);

    bool changed  = (sig != gCmbSig);
    bool baseline = (gP3Frames % kCmbBaseGap) == 0;
    if (!changed && !baseline) return;
    gCmbSig = sig;
    --gCmbBudget;

    CharMovement* rMv = rider->getMovement();
    CharMovement* mMv = mount->getMovement();
    Ogre::Vector3 rP  = rMv ? rMv->getPosition() : Ogre::Vector3::ZERO;
    Ogre::Vector3 mP  = mMv ? mMv->getPosition() : Ogre::Vector3::ZERO;
    Ogre::Vector3 rB  = rider->getBoneWorldPosition("Bip01");

    // Nearest attacker over BOTH lists - whoever is fighting this pair is a data point for
    // reach whichever of the two they picked as their target.
    float dR3 = -1.0f, dRxz = -1.0f, dB3 = -1.0f, dM3 = -1.0f, dMxz = -1.0f;
    for (int pass = 0; pass < 2; ++pass)
    {
        lektor<hand>& L = (pass == 0) ? rAtk : mAtk;
        for (lektor<hand>::iterator ait = L.begin(); ait != L.end(); ++ait)
        {
            Character* a = ait->getCharacter();
            if (!a) continue;
            CharMovement* aMv = a->getMovement();
            if (!aMv) continue;
            Ogre::Vector3 aP = aMv->getPosition();
            float r3  = (aP - rP).length();
            float rdx = aP.x - rP.x, rdz = aP.z - rP.z;
            float rxz = sqrtf(rdx * rdx + rdz * rdz);
            float b3  = (aP - rB).length();
            float m3  = (aP - mP).length();
            float mdx = aP.x - mP.x, mdz = aP.z - mP.z;
            float mxz = sqrtf(mdx * mdx + mdz * mdz);
            if (dR3  < 0.0f || r3  < dR3)  dR3  = r3;
            if (dRxz < 0.0f || rxz < dRxz) dRxz = rxz;
            if (dB3  < 0.0f || b3  < dB3)  dB3  = b3;
            if (dM3  < 0.0f || m3  < dM3)  dM3  = m3;
            if (dMxz < 0.0f || mxz < dMxz) dMxz = mxz;
        }
    }

    char b[512];
    _snprintf_s(b, 512, _TRUNCATE,
        "Riding: P3CMB neck=%d rAtk=%d mAtk=%d cm=%d cmM=%d rTgt=%d mTgt=%d cc=%d bc=%d down=%d "
        "| dR3=%.2f dRxz=%.2f dB3=%.2f dM3=%.2f dMxz=%.2f | rY=%.2f bY=%.2f mY=%.2f",
        neck ? 1 : 0, rn, mn, cm ? 1 : 0, cmM ? 1 : 0, rTgt ? 1 : 0, mTgt ? 1 : 0,
        cc ? 1 : 0, bc ? 1 : 0, dn ? 1 : 0,
        dR3, dRxz, dB3, dM3, dMxz, rP.y, rB.y, mP.y);
    DebugLog(std::string(b));
}

static void CombatProbe(Character* rider, Character* mount, bool neck)
{
    __try { CombatProbeImpl(rider, mount, neck); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { DebugLog("Riding: P3CMB access violation - combat probe abandoned"); }
}

// P4-1e-2: put the weapon back in the mounted rider's hand, and report the aggro/weapon state.
// No attacker requirement - see the kArmBudget block for why that gate had to go.  Called every
// frame while mounted on a small mount under debugContinuous; the rationing lives inside.
static void RiderArmProbeImpl(Character* rider, Character* mount)
{
    Inventory* rInv = rider->getInventory();

    // ---- P4-1g: spend one window frame (see kArmDumpFrames) ------------------------------
    // Sampled at the TOP, i.e. the state the engine left behind after its own update, before
    // this probe touches anything - the same reasoning as the P4-1f edge sampler.  The window
    // is armed by the draw below, so frame 1 of every cycle is the "post" line and the rest
    // are these; windows cannot chain because a successful draw holds wpn=1 for ~18 frames and
    // no new attempt is made while the weapon is out.
    if (gArmDumpLeft > 0 && gArmDumpBudget > 0)
    {
        --gArmDumpLeft;
        --gArmDumpBudget;
        DumpRiderAnimLayers(rider, rider->getAnimationClass(), "win");
    }

    // ---- P4-1h: make sure some dump budget actually lands IN combat -----------------------
    // The P4-1g windows are armed by our forced draw, which happens in the opening frames of a
    // ride, so every one of its 132 samples was out of combat and its iCM=0 says nothing about
    // the state P4-3 has to work in.  This budget is separate so those windows cannot eat it,
    // it re-arms for each fight, and it spends on two triggers: a slow stride for coverage,
    // plus any change in the number of entries in the rider's layers.  The second is the one
    // that matters - "something new entered the layers during a fight" IS the question, and at
    // one dump per kArmDumpCmbGap frames a stride would step over a whole swing.
    if (gArmDumpCmbBudget > 0)
    {
        if (rider->isInCombatMode(true, true))
        {
            unsigned int ec = CountRiderLayerEntries(rider->getAnimationClass());
            bool entered    = (gArmDumpCmbEntries >= 0 && (int)ec != gArmDumpCmbEntries);
            bool stride     = (gP3Frames % kArmDumpCmbGap) == 0;
            gArmDumpCmbEntries = (int)ec;
            if (entered || stride)
            {
                --gArmDumpCmbBudget;
                // P4-1M: the tag carries whether the combat stance was up on the last frame
                // HaltAndForceSitPass ran ("cmb1" / "cmb1+"), so a layer dump can still be read
                // against what the torso was supposed to be doing.
                char ct[16];
                _snprintf_s(ct, 16, _TRUNCATE, "cmb%d%s",
                            gRideStanceOn ? 1 : 0, entered ? "+" : "");
                DumpRiderAnimLayers(rider, rider->getAnimationClass(), ct);
            }
        }
        else gArmDumpCmbEntries = -1;   // entering combat again re-arms the change detector
    }

    // One dump per ride, before anything is touched: if getPrimaryWeapon() comes back NULL
    // this is the line that says whether the slots are empty or just not where we looked.
    // Accessors only - no raw field reads - because InventorySection's annotated offsets
    // depend on this build's std::string/std::vector sizes and getting that wrong is a
    // runtime AV, while a wrong accessor signature is a link error we would see immediately.
    if (!gInvDumped && rInv)
    {
        gInvDumped = 1;
        lektor<InventorySection*>& secs = rInv->getAllSections();
        lektor<Item*> eqw;  rInv->getEquippedWeapons(eqw);
        char sl[512];
        int  used = 0;
        sl[0] = 0;
        for (uint32_t si = 0; si < secs.size() && used < 400; ++si)
        {
            InventorySection* sc = secs[si];
            if (!sc) continue;
            int n = _snprintf_s(sl + used, 512 - used, _TRUNCATE, "[%d:%u%s]",
                                (int)sc->getLimitedSlot(), sc->getNumItems(),
                                sc->getItem() ? "*" : "");
            if (n <= 0) break;
            used += n;
        }
        DebugLog("Riding: P41E slots sec=" + IntToStr((int)secs.size())
                 + " eqw=" + IntToStr((int)eqw.size())
                 + " prim=" + IntToStr(rInv->getPrimaryWeapon() ? 1 : 0)
                 + " sec2=" + IntToStr(rInv->getSecondaryWeapon() ? 1 : 0)
                 + " | " + std::string(sl));
    }

    // ---- P4-1f: name the writer that puts the weapon away (ordering test, see kEdgeBudget) ----
    if (gEdgeBudget > 0)
    {
        AnimationClass* eAnim = rider->getAnimationClass();
        int eWpn = rider->getCurrentWeapon() ? 1 : 0;
        int eACW = eAnim ? (int)eAnim->animationRequirements.currentWeapon : -1;
        if (eWpn != gEdgeWpn || eACW != gEdgeACW)
        {
            --gEdgeBudget;
            char eb[224];
            _snprintf_s(eb, 224, _TRUNCATE,
                "Riding: P41F edge wpn=%d->%d aCW=%d->%d pref=%d cma=%d bc=%d draws=%d f=%u",
                gEdgeWpn, eWpn, gEdgeACW, eACW,
                rider->getThePreferredWeapon()      ? 1 : 0,
                rider->isInCombatMode(true, true)   ? 1 : 0,
                rider->_isBeingCarried              ? 1 : 0,
                gDrawCalls, gP3Frames);
            DebugLog(std::string(eb));
            gEdgeWpn = eWpn;
            gEdgeACW = eACW;
        }
    }

    if (!rider->getCurrentWeapon() && gDrawTries > 0 && rInv
        && (!gDrawLastFrame || (gP3Frames - gDrawLastFrame) >= kDrawTryGap))
    {
        // getPrimaryWeapon() reads the equipment slots, so it answers even while the weapon
        // is sheathed - that is the whole difference from getThePreferredWeapon(), which was
        // NULL in every P4-1d read.  Weapon -> Item is offset-0 single inheritance all the
        // way down (Gear.h annotates the base offsets), so reinterpret_cast is the sound cast
        // here: both types are incomplete to the compiler, which rules out a language upcast.
        Weapon* sw = rInv->getPrimaryWeapon();
        if (!sw) sw = rInv->getSecondaryWeapon();
        gDrawLastFrame = gP3Frames;
        if (sw)
        {
            --gDrawTries;
            ++gDrawCalls;
            // P4-1h: the deciding read, taken across the call.  The sheath name has to be
            // COPIED here - the const char* aliases the engine's std::string and drawWeapon is
            // entitled to reallocate it, which would leave the pre-value dangling.
            const char* shPreP = "?";
            Weapon* wihPre = RiderWeaponInHands(rider, &shPreP);
            std::string shPre(shPreP ? shPreP : "");
            std::string sheathArg = RideSheathSlotFor(rider);   // see RideSheathSlotFor
            rider->drawWeapon(reinterpret_cast<Item*>(sw), sheathArg);
            const char* shPost = "?";
            Weapon* wihPost = RiderWeaponInHands(rider, &shPost);
            AnimationClass* dAnim = rider->getAnimationClass();
            char dw[384];
            _snprintf_s(dw, 384, _TRUNCATE,
                "Riding: P41E draw n=%d left=%d sw=1 post=%d pref=%d aCW=%d bc=%d "
                "wih=%d->%d sh='%s'->'%s' f=%u",
                gDrawCalls, gDrawTries,
                rider->getCurrentWeapon()        ? 1 : 0,
                rider->getThePreferredWeapon()   ? 1 : 0,
                dAnim ? (int)dAnim->animationRequirements.currentWeapon : -1,
                rider->_isBeingCarried ? 1 : 0,
                wihPre ? 1 : 0, wihPost ? 1 : 0,
                shPre.c_str(), shPost,
                gP3Frames);
            DebugLog(std::string(dw));

            // P4-1g: catch a SYNCHRONOUS draw request in the frame it is made, then arm the
            // window so the following kArmDumpFrames cover the hold and the revert.  Only the
            // first couple of cycles are sampled (kArmDumpBudget); that is enough because the
            // question is categorical - does a draw clip appear at all, and at what weight.
            if (gArmDumpBudget > 0)
            {
                --gArmDumpBudget;
                DumpRiderAnimLayers(rider, dAnim, "post");
                gArmDumpLeft = kArmDumpFrames;
            }
        }
        else if (!gDrawNoWpn)
        {
            // Log the empty-handed case once, not every kDrawTryGap frames: "no weapon in
            // the slots" is a single fact about this ride, and it is answered by the dump.
            gDrawNoWpn = 1;
            DebugLog("Riding: P41E draw skipped - no equipped weapon in the slots");
        }
    }

    // ---- state line: aggro AND weapon, same sample ---------------------------------------
    if (gArmBudget <= 0) return;

    lektor<hand> rAtk;  rider->getAllAttackers(rAtk);
    lektor<hand> mAtk;  mount->getAllAttackers(mAtk);
    int rn = (int)rAtk.size();
    int mn = (int)mAtk.size();

    int wpn  = rider->getCurrentWeapon()      ? 1 : 0;
    int pref = rider->getThePreferredWeapon() ? 1 : 0;
    int prim = (rInv && rInv->getPrimaryWeapon()) ? 1 : 0;
    int cma  = rider->isInCombatMode(true, true) ? 1 : 0;
    int bc   = rider->_isBeingCarried ? 1 : 0;
    AnimationClass* aAnim = rider->getAnimationClass();
    int aCW  = aAnim ? (int)aAnim->animationRequirements.currentWeapon : -1;

    int sig = (rn > 7 ? 7 : rn) | ((mn > 7 ? 7 : mn) << 3)
            | (wpn ? 0x40 : 0) | (pref ? 0x80 : 0) | (prim ? 0x100 : 0)
            | (cma ? 0x200 : 0) | (bc ? 0x400 : 0) | ((aCW & 0xF) << 11);

    bool changed  = (sig != gArmSig);
    bool baseline = (gP3Frames % kArmBaseGap) == 0;
    if (!changed && !baseline) return;
    gArmSig = sig;
    --gArmBudget;

    char b[288];
    _snprintf_s(b, 288, _TRUNCATE,
        "Riding: P41E arm rAtk=%d mAtk=%d wpn=%d pref=%d prim=%d aCW=%d cma=%d bc=%d "
        "draws=%d left=%d f=%u",
        rn, mn, wpn, pref, prim, aCW, cma, bc, gDrawCalls, gDrawTries, gP3Frames);
    DebugLog(std::string(b));
}

static void RiderArmProbe(Character* rider, Character* mount)
{
    __try { RiderArmProbeImpl(rider, mount); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    { DebugLog("Riding: P41E access violation - arm probe abandoned"); }
}

// P4-1b/1c: ask the engine WHY the rider is not a combat participant, then try to make it one.
// See the kAtkTryBudget block near the top of the file for the P4-1b result and the P4-1c
// pre-registered readings.  P4-1b settled the gate (the rider CAN be a participant, via
// cc->setAttackTarget + rider->attackingYou); P4-1c is about the one thing still missing, the
// swing, whose named mechanism is reach=0.00 with combat mode never active.
//   Target = nearest live attacker over BOTH attacker lists (the rider's is empty at the start
// of a ride - that is the bug - so the first one always comes from the mount's), and it must be
// within kAtkTryRange, which was the whole point of the P4-1b revision: P4-1a's single order
// went out at d=971 because getAllAttackers() registers an attacker the moment it DECIDES to
// attack, so distance was never controlled for.
//   The read lines are emitted on the same cadence as the ladder and keep going (own budget)
// after the ladder is spent, so "nothing stuck" and "stuck then cleared" stay separable.
static void RiderCombatLeverImpl(Character* rider, Character* mount)
{
    lektor<hand> rAtk;  rider->getAllAttackers(rAtk);
    lektor<hand> mAtk;  mount->getAllAttackers(mAtk);
    if (!rAtk.size() && !mAtk.size()) return;

    CharMovement* rMv = rider->getMovement();
    Ogre::Vector3 rP  = rMv ? rMv->getPosition() : Ogre::Vector3::ZERO;

    Character* best  = NULL;
    float      bestD = 0.0f, bestXZ = 0.0f;
    for (int pass = 0; pass < 2; ++pass)
    {
        lektor<hand>& L = (pass == 0) ? rAtk : mAtk;
        for (lektor<hand>::iterator ait = L.begin(); ait != L.end(); ++ait)
        {
            Character* a = ait->getCharacter();
            if (!a || a == rider || a->isDown() || a->isDead()) continue;
            CharMovement* aMv = a->getMovement();
            if (!aMv) continue;
            Ogre::Vector3 aP = aMv->getPosition();
            float d  = (aP - rP).length();
            float dx = aP.x - rP.x, dz = aP.z - rP.z;
            if (!best || d < bestD) { best = a; bestD = d; bestXZ = sqrtf(dx * dx + dz * dz); }
        }
    }
    if (!best || bestD > kAtkTryRange) return;   // short range only - P4-1a's core mistake

    // P4-1e used to sit here, inside this attacker gate.  It was moved out to RiderArmProbe,
    // which runs every frame with no attacker requirement: mounting turns out to DROP an
    // existing aggro relationship (measured 2026-08-30), so this gate can never be relied on
    // to fire, and "can a mounted rider draw a weapon" is not a combat question anyway.

    if (gAtkLastFrame && (gP3Frames - gAtkLastFrame) < kAtkTryGap) return;
    gAtkLastFrame = gP3Frames;

    CombatClass* rcc = rider->getCombatClass();
    CombatClass* ecc = best->getCombatClass();

    // Classify targets by identity instead of by nullness: "holds A target" and "holds THE
    // target we asked for" are different answers and P4-1a could not tell them apart.
    Character* rt  = rider->getAttackTarget().getCharacter();
    Character* ct  = rcc ? rcc->_getAttackTarget().getCharacter() : NULL;
    Character* et  = best->getAttackTarget().getCharacter();
    Character* nz  = rcc ? rcc->getNearestEnemyInAttackZone() : NULL;
    int rTgt  = !rt ? 0 : (rt == best ? 1 : 2);
    int cTgt  = !ct ? 0 : (ct == best ? 1 : 2);
    int eTgt  = !et ? 0 : (et == rider ? 1 : (et == mount ? 2 : 3));
    int nearZ = !nz ? 0 : (nz == best ? 1 : 2);

    // Prefix note: every line in this probe is P41D now, so a fresh log can never be mistaken
    // for a P41C one.  The two read lines keep DIFFERENT tokens on purpose - "P41D ai" is this
    // one (the P41B-era AI-layer view, kept verbatim as a regression check) and "P41D read" is
    // the deep raw/API one below.  One token per line type, or the log parsers conflate them.
    char b[768];
    _snprintf_s(b, 768, _TRUNCATE,
        "Riding: P41D ai d=%.2f dxz=%.2f reach=%.2f eReach=%.2f | r cm=%d cmM=%d rTgt=%d "
        "cTgt=%d in=%d opp=%d wait=%d inZ=%d nearZ=%d lst=%d atkg=%.2f | e eTgt=%d eLstR=%d "
        "eLstM=%d eIn=%d | v ordProb=%d rGet=%d eGet=%d noAgg=%d bc=%d",
        bestD, bestXZ,
        rcc ? rcc->weaponReach() : -1.0f,
        ecc ? ecc->weaponReach() : -1.0f,
        rider->isInCombatMode(true, true)  ? 1 : 0,
        rider->isInCombatMode(true, false) ? 1 : 0,
        rTgt, cTgt,
        rcc ? (rcc->_isInCombatMode() ? 1 : 0) : -1,
        rcc ? rcc->getNumOpponents() : -1,
        rcc ? rcc->getNumWaitingAttackers() : -1,
        rcc ? (rcc->isInAttackZone(best) ? 1 : 0) : -1,
        nearZ,
        rcc ? (rcc->isInAttackerListH(best) ? 1 : 0) : -1,
        rcc ? rcc->isAttacking(best) : -1.0f,
        eTgt,
        ecc ? (ecc->isInAttackerListH(rider) ? 1 : 0) : -1,
        ecc ? (ecc->isInAttackerListH(mount) ? 1 : 0) : -1,
        ecc ? (ecc->_isInCombatMode() ? 1 : 0) : -1,
        rider->checkPlayerOrderForProblems(FOCUSED_MELEE_ATTACK, best) ? 1 : 0,
        rider->areYouGonnaGetMe(best)  ? 1 : 0,
        best->areYouGonnaGetMe(rider)  ? 1 : 0,
        rider->iShouldntAggravateThisTarget(best) ? 1 : 0,
        rider->_isBeingCarried ? 1 : 0);
    DebugLog(std::string(b));
    if (gAtkReads > 0) --gAtkReads;

    // ---- P4-1d precondition: the three rungs already proven, re-applied idempotently -------
    // All of them stuck for the rest of the ride that proved them, so on a fresh ride we want
    // them in place BEFORE the new ladder runs - otherwise rung 0 gets tested without a combat
    // relationship and rung 4 with one, and the round-robin confounds the two.  Guarded so a
    // repeat is a no-op: the target only when the slot does not already name our enemy, and
    // attackingYou only when the attacker list does not already contain it, because that one is
    // an EVENT rather than a field write and re-firing it every 75 frames is a real state change.
    // Deliberately outside the gAtkTries budget: it has to outlive the ladder, or the later
    // reads measure a relationship that has quietly lapsed.
    int preT = 0, preA = 0, preC = 0;
    if (rcc && ct != best)
    {
        rcc->setAttackTarget(best);
        rcc->setAttackTargetHandle(best);
        preT = 1;
    }
    if (rcc && !rcc->isInAttackerListH(best))
    {
        rider->attackingYou(best, true, false);
        preA = 1;
    }

    // The subject for initCombatMode comes straight out of the combat class, so no `hand` is
    // ever constructed in this DLL: hand has virtuals and its ctor would be one more exported
    // stub to depend on.  Read AFTER setAttackTarget above, so the slot is already populated.
    hand tgtH = rcc ? rcc->_getAttackTarget() : hand();

    // P4-1d promotes initCombatMode into the precondition, because P4-1c proved it is THE lever
    // P4-1b was missing: cma 0->1 with _isInCombatMode() agreeing, rTgt 0->1 (33/33 zero in
    // P4-1b), cst 3->4, and all of it held to the end of the log.  So combat mode is now part of
    // the baseline and every rung below is tested ON TOP of it instead of racing it.  Idempotent
    // via the cma!=1 guard.  Unfocused on purpose - the focused variant stays a ladder rung so
    // the two remain distinguishable.  Cast: CombatClassPlayer and CombatClassAI are
    // single-inheritance siblings with CombatClass first and only at offset 0 (CombatClass.h:
    // 55/257/273), so this needs no this-adjustment; reinterpret_cast because the shim declares
    // CombatClassAI standalone rather than restating the whole hierarchy.
    if (rcc && CcBool(rcc, 0x130) != 1 && tgtH.getCharacter())
    {
        reinterpret_cast<CombatClassAI*>(rcc)->_NV_initCombatMode(tgtH, 0, false);
        preC = 1;
    }
    if (preT || preA || preC)
        DebugLog("Riding: P41D precond tgt=" + IntToStr(preT) + " atk=" + IntToStr(preA)
                 + " icm=" + IntToStr(preC));

    // ---- P4-1d deep read --------------------------------------------------------------
    // Raw fields first, then the API calls that must agree with them, then the ENEMY's same
    // three raw fields as the positive control: that character is provably in combat mode, so
    // its cma/tech/cst show what "in combat mode" looks like in these very bytes.  Without it
    // a rider reading cma=0 tech=0 cst=0 is indistinguishable from three bad offsets.
    // ⚠ nxt (0x1F4, nextMove) is GONE from this line.  P4-1c read 1818135763 in the two reads
    // before combat mode came up and 8 in the two after: the field is uninitialised until the
    // state machine starts, so it can never be read as a state.  (That it changed at all is
    // still evidence the machine turns - that argument is recorded, the field is not.)
    CombatClass*    mcc   = mount->getCombatClass();
    AnimationClass* rAnim = rider->getAnimationClass();

    // The boring explanation P4-1c left open.  getCurrentWeapon() (wpn= below) is a DRAWN-
    // weapon test and read NULL 4/4 even though the player had equipped one, so it cannot tell
    // "owns nothing" from "owns one, never drew it".  getThePreferredWeapon() (vtable 0x3C8)
    // asks the inventory instead and getCategory() (Gear.h:49, @0x5C71D0) says what it is.  If
    // pWpn=1 with a real pCat while aCW stays SKILL_UNARMED(5), the entire reach=0 chain is
    // just a sheathed weapon and rung 0 is the whole fix.
    Weapon* pw   = rider->getThePreferredWeapon();
    int     pWpn = pw ? 1 : 0;
    int     pCat = pw ? (int)pw->getCategory() : -1;

    // The dry run that breaks the reach=0 -> no technique -> reach=0 circle: chooseAttack takes
    // weaponReach as an ARGUMENT (@0x886880), so we can hand it a reach the rider does not have
    // and find out whether a technique would exist at all.  R = the rider's own reach when it is
    // non-zero, else the enemy's (9.00 in P4-1c), else 9.0f.  READ-ONLY: the returned pointer is
    // deliberately not installed here - that is rung 2's job, so the ladder stays attributable.
    float synthReach = 9.0f;
    if (rcc && rcc->weaponReach() > 0.01f)      synthReach = rcc->weaponReach();
    else if (ecc && ecc->weaponReach() > 0.01f) synthReach = ecc->weaponReach();
    CharStats*           rst    = rider->getStats();
    CombatTechniqueData* chTech = rst ? rst->chooseAttack(bestD, synthReach, NULL, false) : NULL;

    // CombatTechniqueData is read by raw offset on purpose: its own header drags in
    // MedicalSystem.h, which this tree never compiles.  `animation` at 0x0 is a std::string -
    // the same ABI bet SafeSnapAnimRow already makes on AnimationData::dataName, so this adds no
    // new dependency.  0x38 initialDistance / 0x3C minDistanceVsStatic are the engine's own
    // opinion of the range this swing is usable at, which is the number to compare against d=.
    char  chAnim[64];
    chAnim[0] = 0;
    float chInit = -1.0f, chMinS = -1.0f;
    if (chTech)
    {
        const std::string* an = (const std::string*)((const char*)chTech + 0x00);
        _snprintf_s(chAnim, 64, _TRUNCATE, "%s", an->c_str());
        chInit = *(const float*)((const char*)chTech + 0x38);
        chMinS = *(const float*)((const char*)chTech + 0x3C);
    }

    char p[1280];
    _snprintf_s(p, 1280, _TRUNCATE,
        "Riding: P41D read d=%.2f | raw cma=%d tech=%d cst=%d atk=%.2f mei=%.2f/%.2f "
        "| api in=%d gcs=%d blk=%d reach=%.2f | vp R=%08X M=%08X E=%08X "
        "| wpn=%d eWpn=%d aCW=%d aCM=%d | pWpn=%d pCat=%d cTech=%d "
        "| ch=%d sR=%.2f anim='%s' init=%.2f minS=%.2f "
        "| e cma=%d tech=%d cst=%d reach=%.2f",
        bestD,
        CcBool(rcc, 0x130), CcPtrSet(rcc, 0x150),
        CcInt(rcc, 0x1F0),
        CcFloat(rcc, 0x140), CcFloat(rcc, 0x278), CcFloat(rcc, 0x27C),
        rcc ? (rcc->_isInCombatMode() ? 1 : 0) : -1,
        rcc ? (int)rcc->getCombatState() : -1,
        rcc ? (int)rcc->getBlockStateEnum() : -1,
        rcc ? rcc->weaponReach() : -1.0f,
        CcVptrLo(rcc), CcVptrLo(mcc), CcVptrLo(ecc),
        rider->getCurrentWeapon() ? 1 : 0,
        best->getCurrentWeapon()  ? 1 : 0,
        rAnim ? (int)rAnim->animationRequirements.currentWeapon : -1,
        rAnim ? (int)rAnim->animationRequirements.isCombatMode.key  : -1,
        pWpn, pCat,
        // cTech: _currentCombatTechnique lives on AnimationRequirement (0x118 within that
        // struct), NOT on AnimationClass - AnimationClass.h:349 sits inside the
        // AnimationRequirement body (318-377) while AnimationClass itself only opens at :401.
        // Same struct the two lines above already read (currentWeapon / isCombatMode).
        (rAnim && rAnim->animationRequirements._currentCombatTechnique) ? 1 : 0,
        chTech ? 1 : 0, synthReach, chAnim, chInit, chMinS,
        CcBool(ecc, 0x130), CcPtrSet(ecc, 0x150), CcInt(ecc, 0x1F0),
        ecc ? ecc->weaponReach() : -1.0f);
    DebugLog(std::string(p));

    // P4-3 premise #2, answered here instead of by a separate probe: resolve that clip name in
    // allAnims and print its layer and flags, so we learn whether the swing carries
    // wholeBodyAllLayer - i.e. whether it collides with kRidePose the same way kRidePose
    // collides with everything else.  Once per DISTINCT name: the name only changes when
    // chooseAttack picks differently, and that is exactly when a new row is worth a log line.
    // ⚠ find() ONLY.  getAnimationData() has operator[] semantics and would insert a permanent
    // NULL into the engine's own table on a miss - and a miss is a live possibility here, since
    // P1 proved the human table has no ATTACKS category at all.
    // ⚠ std::string comparison, not strcmp: this file includes no <string.h>/<cstring> and uses
    // no C string functions anywhere, and this probe is not the place to start.
    static std::string lastChAnim;
    if (chAnim[0] && lastChAnim != chAnim)
    {
        lastChAnim = chAnim;
        AnimsListsManager*           mgr = AnimsListsManager::getSingleton();
        AnimsListsManager::AnimList* own = rAnim ? rAnim->getAnimationDatasList() : NULL;
        AnimsListsManager::AnimList* lst = own ? own : (mgr ? mgr->getCharacterList() : NULL);
        // Same boost-layout self-check FindAnimData and AnimListAttackCount use (it was also the
        // P1 ANIMTABLE dump's gate, before that dump was removed): if allAnims is not at 0xB8
        // the map we would search is not the map the game has, so refuse rather than guess.
        long allOff = lst ? (long)((char*)&lst->allAnims - (char*)lst) : -1;
        if (!lst || allOff != 0xB8)
            DebugLog(std::string("Riding:   P41D clip '") + chAnim + "' unresolved (no list)");
        else
        {
            EngineAnimMap::const_iterator ci = lst->allAnims.find(std::string(chAnim));
            if (ci == lst->allAnims.end())
                DebugLog(std::string("Riding:   P41D clip '") + chAnim + "' ABSENT in allAnims");
            else
                LogAnimRow("P41D clip", chAnim, ci->second);
        }
    }

    if (gAtkTries <= 0) return;

    // Round-robin, but skip rungs an AV already disarmed (gRungDead[], set by the __except
    // shell).  P4-1c burned the whole run on one bad rung: the SEH shell zeroed the budget, so
    // 3 rungs / 4 reads happened instead of 20 / 60 and rung 4 was never reached.  Now one bad
    // rung costs only itself.  If every rung is dead there is nothing left to try - stop.
    int stage = -1;
    for (int si = 0; si < kAtkStages; ++si)
    {
        int cand = (gAtkStage + si) % kAtkStages;
        if (!gRungDead[cand]) { stage = cand; break; }
    }
    if (stage < 0)
    {
        DebugLog("Riding: P41D all rungs disarmed - ladder abandoned");
        gAtkTries = 0;
        return;
    }
    gAtkStage = (stage + 1) % kAtkStages;
    --gAtkTries;

    int icm = -1;   // _NV_initCombatMode's return value, -1 = not called this rung

    // gAtkCurRung is what the __except shell reads to know WHICH rung faulted.  Set immediately
    // before the switch and cleared immediately after, so an AV anywhere else stays attributed
    // to "not a rung" and keeps the old abandon-everything behaviour.
    gAtkCurRung = stage;
    switch (stage)
    {
    case 0:
        // The most boring explanation first: the weapon is simply never drawn (P4-1c: wpn=0 4/4
        // with a weapon equipped, animation side reporting SKILL_UNARMED).  drawWeapon is pure
        // virtual (vtable 0x3D8) and getThePreferredWeapon (0x3C8) picks the subject, so this
        // rung needs no shim.  Weapon -> Item is offset-0 single inheritance all the way down
        // (Gear.h annotates the base offsets), so reinterpret_cast is the sound cast here: both
        // types are incomplete to the compiler, which makes a language-level upcast impossible.
        if (pw)
        {
            std::string sheathArg = RideSheathSlotFor(rider);   // see RideSheathSlotFor
            rider->drawWeapon(reinterpret_cast<Item*>(pw), sheathArg);
        }
        break;
    case 1:
        // The FOCUSED variant.  The unfocused one already runs every tick in the idempotent
        // precondition above, so this rung tests only the difference the focus flag makes.
        // ⚠ The dispatch target is CombatClassAI::_NV_initCombatMode @0x667A60, not the base's
        // 0x665230: the rider, the mount and the enemy all read the same vptr (41DB5688) and two
        // of those three are provably AI.  Offset-0 sibling, so no this-adjustment.
        if (rcc && tgtH.getCharacter())
            icm = reinterpret_cast<CombatClassAI*>(rcc)->_NV_initCombatMode(tgtH, 0, true) ? 1 : 0;
        break;
    case 2:
        // Hand the already-turning state machine the one thing it lacks.  P4-1c's fired branch
        // was "cma=1 but reach=0 => the missing piece is a technique", and tech was NULL in 4/4
        // reads.  chTech comes from the read block's dry run, so if this rung swings, chooseAttack
        // is the producer and the engine drives everything after it.
        if (rcc && chTech) CcSetPtr(rcc, 0x150, chTech);
        break;
    case 3:
        // The decisive rung: bypasses combat state, attack zone, reach and technique selection
        // entirely and asks the animation layer to play the swing (@0x5B6E80).  If this is the
        // only rung that produces act>1, P4-3 changes shape - we would have to drive every swing
        // ourselves instead of just finding the clip.  Dismount() calls endCombatAnimation().
        if (rAnim && chTech) rAnim->runCombatAnimation(chTech, 1.0f, "");
        break;
    default:
        // Skips the engine's own preconditions, so it is last: if only this one swings we have
        // a hack, not a fix.  P4-1c never reached it (the AV ate the budget); this time it runs
        // with a technique installed by rung 2 rather than against tech=NULL.
        if (rcc) rcc->changeState(CHOP_WEAPON, 0.0f);
        break;
    }
    gAtkCurRung = -1;

    // Same-frame readback.  The first six fields are unchanged from P41B on purpose: they are
    // now a REGRESSION check that the two proven rungs still hold while the new ones fire.
    // The rest are the outcome variables of the hypothesis chain, in engine order, so one line
    // says how far down the chain this rung got: cma -> tech -> reach -> inZ -> nearZ.
    Character* rt2 = rider->getAttackTarget().getCharacter();
    Character* ct2 = rcc ? rcc->_getAttackTarget().getCharacter() : NULL;
    Character* et2 = best->getAttackTarget().getCharacter();
    Character* nz2 = rcc ? rcc->getNearestEnemyInAttackZone() : NULL;
    char c[640];
    _snprintf_s(c, 640, _TRUNCATE,
        "Riding: P41D rung=%d tries_left=%d d=%.2f icm=%d | post rTgt=%d cTgt=%d eTgt=%d cm=%d "
        "cmM=%d in=%d opp=%d | chain cma=%d tech=%d reach=%.2f inZ=%d nearZ=%d | cst=%d "
        "aCM=%d wpn=%d",
        stage, gAtkTries, bestD, icm,
        !rt2 ? 0 : (rt2 == best ? 1 : 2),
        !ct2 ? 0 : (ct2 == best ? 1 : 2),
        !et2 ? 0 : (et2 == rider ? 1 : (et2 == mount ? 2 : 3)),
        rider->isInCombatMode(true, true)  ? 1 : 0,
        rider->isInCombatMode(true, false) ? 1 : 0,
        rcc ? (rcc->_isInCombatMode() ? 1 : 0) : -1,
        rcc ? rcc->getNumOpponents() : -1,
        CcBool(rcc, 0x130), CcPtrSet(rcc, 0x150),
        rcc ? rcc->weaponReach() : -1.0f,
        rcc ? (rcc->isInAttackZone(best) ? 1 : 0) : -1,
        !nz2 ? 0 : (nz2 == best ? 1 : 2),
        CcInt(rcc, 0x1F0),
        rAnim ? (int)rAnim->animationRequirements.isCombatMode.key : -1,
        rider->getCurrentWeapon() ? 1 : 0);
    DebugLog(std::string(c));
}

static void RiderCombatLever(Character* rider, Character* mount)
{
    __try { RiderCombatLeverImpl(rider, mount); }
    __except (GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION
                  ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
    {
        // P4-1d: disarm only the rung that faulted, keep the budget.  P4-1c did the opposite and
        // one bad rung (calculateTargetsInAttackZone, deleted) cost the whole run: 3 rungs and 4
        // reads out of a 20/60 budget, with rung 4 never reached.  gAtkCurRung is only in range
        // while the switch is executing, so an AV in the read block or the readback still
        // abandons everything - that is a different kind of fault and worth stopping for.
        // ⚠ Fixed char buffer, no operator+ chains: this handler lives in a function with __try,
        // where every extra object needing unwinding is a C2712 risk.  One implicit std::string
        // temporary at the DebugLog call is the shape that already compiles here.
        int deadRung = gAtkCurRung;
        gAtkCurRung = -1;
        if (deadRung >= 0 && deadRung < kAtkStages)
        {
            gRungDead[deadRung] = true;
            char av[128];
            _snprintf_s(av, 128, _TRUNCATE,
                "Riding: P41D access violation in rung %d - that rung disarmed, budget kept",
                deadRung);
            DebugLog(av);
        }
        else
        {
            DebugLog("Riding: P41D access violation outside a rung - combat lever abandoned");
            gAtkTries = 0;
            gAtkReads = 0;
        }
    }
}

// 1c) Mount combat + forced dismount.
//     - Any mount that is down (KO'd) or dead force-dismounts its rider.
//     - Any RIDER that is down (KO'd) or dead force-dismounts too (P4-4, see below).
//     - Outside the P4-0 SIZE gate (big tier) the rider stays passive: their combat is
//       suppressed every frame and the mount fights back with its native animal combat,
//       defending the rider against the rider's attackers.
//     - Inside the size gate (small tier) BOTH fight (已定决策「骑手与坐骑都出手」): nothing
//       of ours touches the rider's combat, and the mount is still pointed at the rider's
//       attackers.
//
// ⚠️ P4-2 (2026-08-30) split the gate here.  Both branches used to key off IsBigMount()
// (seat mode 2||3), which is a DIFFERENT question - "where does the seat sit", not "how big is
// the animal" - and the two disagree on real species: the garru is mode 2 yet size 9.5, well
// inside kCombatSizeMax=15.5, so the old code force-ended its rider's combat mode every frame
// on an animal the P4-0 ruling calls small.  Never merge the two predicates back together
// (same warning as at kCombatSizeMax); IsBigMount is still what steers the SEAT, and
// MountCombatEligible is what steers COMBAT.
static void CombatAndForceDismountPass()
{
    if (!riderToMount.empty())
    {
        boost::unordered_map<Character*, Character*>::iterator it = riderToMount.begin();
        while (it != riderToMount.end())
        {
            Character* rider = it->first;
            Character* mount = it->second;

            // Why this ride ends.  The MOUNT half is the original rule (a KO'd or dead animal
            // cannot carry anyone).  The RIDER half is P4-4 (2026-08-31, user ruling 「骑手被击倒
            // 就下马」): until now nothing ended the ride when the rider went down, and the
            // fourth trip showed what that looked like - the rider sat in the saddle with
            // down=1 from 156 s to the manual dismount at 260.972 s (~105 s unconscious in the
            // saddle), legs handed back to the fall-over clip by LegPosePass's isDown() guard.
            //
            // ⚠️ Do NOT fold this into that guard.  The guard answers "who owns the thigh bones
            // this frame" and must keep working on its own (it is what stops the mask pollution
            // this pass cannot: the release has to happen the same frame, before render, while
            // Dismount() is a whole-ride teardown).  Both now fire on a KO'd rider - the guard
            // first, this pass right after - which is exactly the 「takeover ＝ restored + 中途
            // released」 accounting the fourth trip established.
            const char* dropWhy = NULL;
            if (!rider || !mount)                        dropWhy = "stale";
            else if (mount->isDown() || mount->isDead()) dropWhy = "mount down";
            else if (rider->isDown() || rider->isDead()) dropWhy = "rider down";

            if (dropWhy)
            {
                Character* r = rider;
                ++it; // Dismount erases from the map, advance first
                if (r)
                {
                    // ungated, one line per event - the same discipline as LEGPOSE takeover,
                    // and the only record of WHICH half ended the ride.
                    DebugLog(std::string("Riding: force dismount (") + dropWhy + ")");
                    Dismount(r);
                }
                continue;
            }

            // No seat entry means we know nothing about this mount, so riderFights stays
            // false = default-deny, the same direction MountCombatEligible takes on a
            // failed size read.  bigMount is diagnostic only from here on.
            bool bigMount    = false;
            bool riderFights = false;
            boost::unordered_map<Character*, SeatInfo>::iterator sit = mountSeat.find(mount);
            if (sit != mountSeat.end())
            {
                bigMount    = IsBigMount(sit->second);
                riderFights = MountCombatEligible(mount, sit->second);
            }

            // A diagnostics-off session must still promote each eligible rider into the same
            // combat state that the old P41D ladder proved necessary.  Keep the exhaustive ladder
            // below diagnostic-only; this small idempotent bridge is the shipping path.
            if (riderFights)
                RideCombatPromote(rider, mount);

            // Read the combat state BEFORE the suppression below runs, or the probe would
            // only ever see our own endCombatMode().  neck= keeps its original meaning
            // (seat mode 2||3) so old and new logs stay comparable.
            if (debugContinuous && gCmbBudget > 0)
                CombatProbe(rider, mount, bigMount);

            // ---- P4-3-3: spend the pending stance edge, ungated ------------------------------
            // Consumed here rather than where it was latched (HaltAndForceSitPass) for the reason
            // in RideStanceRedraw's header.  The flag is cleared whether or not a draw happens, so
            // one edge can only ever cost one attempt.  `riderFights` is not re-tested: the edge
            // could only have been latched while the stance was up, and the stance already
            // requires MountCombatEligible, so a big-mount rider can never reach this.
            // (🆕 P4-5: nor a 坐坐垫 rider - same one gate, so no draw edge can ever be latched.)
            RideStanceDrawRuntime* drawRt = FindRideStanceDrawRuntime(rider);
            if (drawRt && drawRt->pending)
            {
                drawRt->pending = false;
                RideStanceRedraw(rider);
            }

            // ---- P4-3-4: the swing, ungated ---------------------------------------------------
            // Same pass and same point in the frame as the re-draw above and as the P41E ladder,
            // for the reason in RideStanceRedraw's header.  Called UNCONDITIONALLY for every
            // tracked pair, including the big tier and a rider whose fight just ended: the pass
            // has to be able to CLOSE a window it opened, and `stance=false` is what closes it.
            // The stance is read with advance=false - HaltAndForceSitPass owns the hold counter, so
            // the two passes can never disagree inside one frame.
            {
                bool swStance = (sit != mountSeat.end())
                             && RideCombatStance(rider, mount, sit->second, false);
                AnimationClass* swingAnim = rider->getAnimationClass();
                RideCombatRuntime& hostRt = GetRideCombatRuntime(rider, mount);
                ResolveRideCombatClips(hostRt, swingAnim);
                const char* swingHostNm = NULL;
                AnimationData* swingHost = RideCombatHost(hostRt, &swingHostNm);
                RideSwingPass(rider, mount, swingAnim, swStance,
                              swingHost, swingHostNm);
            }

            if (!riderFights)
            {
                // big tier: rider stays passive - never swings its tiny weapon from up there.
                // 🆕 P4-5: 坐坐垫 mounts land here too (MountCombatEligible denies the style), which
                // is what 「人物在坐坐垫的时候不参与战斗」 asked for - the engine's own combat
                // bookkeeping is cleared, not merely our writes withheld.  ⚠️ The crab and the swamp
                // turtle are the first COMMONLY RIDDEN animals to walk this branch (trip 28 rode
                // both with elig=1), so this frame-by-frame halt() is the P4-5 regression to watch.
                if (rider->isInCombatMode(true, true) || rider->getAttackTarget().getCharacter())
                    rider->endCombatMode();
                if (rider->getMovement())
                    rider->getMovement()->halt();
            }
            else if (debugContinuous)
            {
                // P4-1e-2: the arm probe first, and with NO attacker requirement - mounting
                // drops existing aggro, so gating it on a live attacker means it may never
                // run.  It rations itself internally (kDrawTryGap / kArmBudget).
                RiderArmProbe(rider, mount);

                // P4-1b: small mount, so nothing of ours suppresses this rider.  Interrogate
                // the engine's own combat bookkeeping about it, then walk the lever ladder.
                if (gAtkTries > 0 || gAtkReads > 0)
                    RiderCombatLever(rider, mount);
            }

            // The mount defends its rider in BOTH tiers (P4-2).  On the big tier this is the
            // only combat happening; on the small tier it is the mount's half of 「双方都出手」.
            // Only ever re-issued when the target actually differs, so a fight the player
            // ordered themselves is not cancelled and re-ordered every frame.
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

    // ---- raw keyboard sniffer + guard trace (diagnostics only, 2026-08-31) -------------
    // The player reported "pressed the seat-tuning keys, the seat did not move" and the log was
    // SILENT: zero `input '...' fired`, zero `tuned` (that one is an UNCONDITIONAL DebugLog, so
    // TuneSeat truly never ran), and not even the "select the rider first" line the tune gate
    // prints when an edge fires with nobody suitable selected.  Yet the same session logged
    // `debug continuous ON` exactly once, which proves the entire chain - map scan ->
    // RidingCmdDown -> OIS isKeyDown -> modifier check -> KeyEdge -> per-frame HotkeyPass - worked
    // for Ctrl+Numpad. minutes before the first mount, with all ten bindings resolved non-zero.
    // So no command edge ever happened, and the remaining candidates are all environmental and
    // invisible offline: the physical keys pressed were not the bound ones (NumLock, a laptop
    // keypad, a remap), the window never received them, or controlEnabled was 0 at that moment.
    // These two blocks make the next trip say which, out loud.
    //
    // ⚠️ The sniffer sits BEFORE the controlEnabled guard on purpose - a stuck guard is one of the
    // candidates, and it must not be able to hide itself.  ⚠️ It is debugContinuous-gated and
    // budgeted; it is a DIAGNOSTIC, never a dispatch path (nothing here can fire a command).
    if (debugContinuous)
    {
        static bool rawPrev[0x100] = { false };
        static int  rawBudget      = 400;
        bool ctrlNow  = false, shiftNow = false, altNow = false;
        try {
            ctrlNow  = key->keyboard->isKeyDown(OIS::KC_LCONTROL) || key->keyboard->isKeyDown(OIS::KC_RCONTROL);
            shiftNow = key->keyboard->isKeyDown(OIS::KC_LSHIFT)   || key->keyboard->isKeyDown(OIS::KC_RSHIFT);
            altNow   = key->keyboard->isKeyDown(OIS::KC_LMENU)    || key->keyboard->isKeyDown(OIS::KC_RMENU);
        } catch (...) {}
        int mask = (ctrlNow ? InputHandler::CTRL_MASK : 0)
                 | (shiftNow ? InputHandler::SHIFT_MASK : 0)
                 | (altNow ? InputHandler::ALT_MASK : 0);
        for (int kc = 1; kc <= 0xED; ++kc)
        {
            bool dn = false;
            try { dn = key->keyboard->isKeyDown((OIS::KeyCode)kc); } catch (...) { dn = false; }
            if (dn && !rawPrev[kc] && rawBudget > 0)
            {
                --rawBudget;
                // Name the command this press would resolve to, if any: "the key I pressed maps to
                // nothing" and "it maps to the wrong command" are different bugs with different
                // fixes, and only this comparison can tell them apart.
                const char* cmd = "-";
                for (int i = 0; i < RCMD_COUNT; ++i)
                    if (gRidingBound[i] == (kc | mask))
                    { cmd = gRidingCommands[i].name; break; }
                char rb[160];
                _snprintf_s(rb, 160, _TRUNCATE,
                    "Riding: RAWKEY kc=%d(0x%02X) mask=0x%X ce=%d cmd=%s",
                    kc, kc, mask, key->controlEnabled ? 1 : 0, cmd);
                DebugLog(std::string(rb));
            }
            rawPrev[kc] = dn;          // tracked even with the budget spent, so no state desync
        }
    }

    // controlEnabled transitions, bounded and NOT diagnostics-gated (a stuck-0 guard has to be
    // visible in a plain log too).  A UI opening/closing flips this, hence the budget.
    {
        static int ceLast   = -1;
        static int ceBudget = 40;
        int ceNow = key->controlEnabled ? 1 : 0;
        if (ceNow != ceLast)
        {
            if (ceBudget > 0)
            {
                --ceBudget;
                DebugLog("Riding: controlEnabled -> " + IntToStr(ceNow));
            }
            ceLast = ceNow;
        }
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
                bool haveSel = false;
                if (player->selectedCharacter)
                {
                    rider = player->selectedCharacter.getCharacter();
                    haveSel = (rider != NULL);
                }
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
                    if (sit == mountSeat.end())
                    {
                        // Was silent until 2026-08-31.  This is the one rejection that looks
                        // exactly like "the key did nothing": the rider IS mounted, so the
                        // helper line below never prints, yet no seat row means no tuning.
                        DebugLog("Riding: seat-tuning key ignored - no seat row for this mount"
                                 " (mounted=1 mountSeat=miss)");
                    }
                    else
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
                    // Say WHICH half failed: "nothing selected" and "the selected character is
                    // neither a rider nor a tracked mount" send the player to different actions.
                    DebugLog(std::string("Riding: seat-tuning key ignored - ")
                             + (haveSel ? "selection is not a rider or a ridden mount"
                                        : "nothing selected")
                             + " (select the rider first; keys are in Settings->Controls)");
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
        WipeAllRideState("access violation in mainLoop", true);
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

// ---- P4-3-2 hook: CharacterHuman::sheatheWeapon (SUPPRESSION, not a probe) -------------------
// This is the one hook in the plugin that DECLINES to run the engine body, so read
// RideSheatheSuppressed's comment block before touching it.  The hook point itself is not new:
// the same address carried P4-3 step 1's naming probe through a full ride with zero
// `Could not hook` lines, and §18.6 established that vtable +0x2D0 is the only entry (pure
// virtual call, no base-class bypass - the base slot is `ret 0`).  The only change from the probe
// is the early return.
//
// ⚠️ CharacterHuman::Character offset is 0x0 (single inheritance, annotated in the headers), which
// is what makes the cast below sound; both types are incomplete-ish to the compiler, so this is a
// reinterpret, not a language upcast.
void (*sheatheWeapon_orig)(CharacterHuman* thisptr) = NULL;

void sheatheWeapon_hook(CharacterHuman* thisptr)
{
    if (RideSheatheSuppressed((Character*)thisptr))
        return;                                    // the overwriter dies for this call
    if (sheatheWeapon_orig) sheatheWeapon_orig(thisptr);
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

    // P4-3-2: the sheathe SUPPRESSION hook.  Same address the step-1 naming probe used (it
    // installed cleanly across full rides), but the body now declines the engine call for a
    // tracked rider whose combat stance is up - HISTORY §B's "destroy the overwriter itself"
    // instead of a per-frame re-draw.  ⚠️ This lands on a function every human in the world calls,
    // so the very first thing the body does is `riderToMount.find` and bail: one hash lookup for
    // everyone who is not our rider.
    if (KenshiLib::SUCCESS != KenshiLib::AddHook(KenshiLib::GetRealAddress(&CharacterHuman::_NV_sheatheWeapon), (void*)&sheatheWeapon_hook, (void**)&sheatheWeapon_orig))
        ErrorLog("RidingPlugin: Could not hook CharacterHuman::sheatheWeapon!");

    // The THREE P4-3 naming probes that used to be registered here (CharacterHuman::sheatheWeapon,
    // both AppearanceBase::attachItem overloads, and AppearanceBase::detachItem(slot)) are GONE as
    // of the probe-free build: they had delivered their verdicts (RE_NOTES §18.10 keeps every
    // address, and the answers are in TASK.md P4-3 steps 1-2), and they were the only hooks this
    // plugin installed on functions that EVERY character in the world calls.  Getting the first two
    // back means `git show 61872dc` (sheathe) / `git show 07f3588` (attachItem) - or
    // `git show 7838deb:RidingPlugin.cpp` for that whole set at once - not retyping them; and the
    // prologue gates in those commits' comments are the record of why they were safe to cut.
    // ⚠️ The detachItem one was never committed: it is only in the file snapshot
    // D:\KenshiModDev\RidingPlugin_src_E83DB50D.cpp (see the tombstone above RideThreatConsider's
    // neighbourhood, near DumpRiderAnimLayers).  Its answer - one 'hands' detach site, and it is
    // attachItem's own clear-before-write - is what closed T15 and T16.
    // (P4-3-2 update: the sheathe ADDRESS is hooked again just above, but as a suppressor, not a
    // probe - no caller-site table, no ShDescribeAddr.  The attachItem pair stays gone.)
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
