#pragma once
#include <string_view>
namespace wingflight {
inline constexpr std::string_view settingsTemplate = R"WF_INI(; Wingflight configuration schema 0.4.0-6. Startup updates this layout and comments.
; Recognized settings are retained; replaced INIs are backed up beside this file.
; Mod Control Panel: Flight, NPC Flight, FreeFlight, Stamina, Jump, Lower Body tuning and controls apply/save live.
; Other settings remain startup-only. External INI edits still require a restart.
; Wingflight: original provider wings are sampled and blended at runtime.
[General]
Enabled=1
; Apply/save live in Mod Control Panel / Logging. Off by default.
; 0: initialization, compatibility decisions, warnings and errors only.
; 1: also record flight, wings, animation, NPC and combat details in Wingflight.log.
; Includes distinct lower-body clip paths (max 256 per launch) for LowerBodyExceptions.txt.
; Replaces the old DiagnosticSamples switch; its old value is intentionally retired.
DetailedLogging=0

[NPCFlight]
; Nearby loaded NPCs currently following the player can follow full flight if their wings bind.
; Uses the same wing-provider INIs as the player. No autonomous NPC takeoff.
; Ground follower packages, recruitment and waiting orders remain owned by their original mods.
; Apply/save live. Disabling prevents takeoff and safely lands airborne NPCs.
Enabled=1
; Apply/save live. 0: land safely for combat. 1: already-flying followers can fight in air.
; Native AI selects enemies, weapons/spells and attacks; Wingflight controls aerial positioning.
; Melee approaches the enemy; ranged flight uses the equipped attack's native range.
; Fleeing or unresolved target/equipment falls back to safe ground combat.
; After combat: resume following if the player is flying, otherwise land.
AirCombat=1
; Maximum simultaneous NPC flight contexts (1..64); startup-only, not a recruitment limit.
MaxActors=32
; Distance behind the player's current position, in Skyrim units (70..2000).
; Apply/save live. No recorded routes. Later rows stay farther behind; collisions remain enabled.
FollowDistance=200.00

[Keyboard]
; DirectInput scan codes. Jump follows Skyrim's remapped Jump control.
; Takeoff: release Jump after a normal jump, then press Jump again in the air.
; K: manual/free flight. Space flaps inside either mode. C: native freefall.
; Outside flight: short AIR tap+release takes off; hold glides until release.
; K choice is remembered for the next takeoff and saved per character in the SKSE co-save.
; Missing preference defaults to Free Flight; outside-flight glide does not change it.
CycleMode=37
Land=46
Descend=29

[Gamepad]
; Raw XInput button masks, 0 = unbound. Jump follows the game mapping.
; Bind CycleMode, Land and Descend in Mod Control Panel / Controls, then Apply and Save.
; Jump and Sprint follow the game mapping. 0 = Unbound. Deadzone is INI-only.
CycleMode=0
Land=0
Descend=0
Deadzone=0.2

[Flight]
; Apply/save live. 1: normal ground contact ends full flight (default).
; 0: retain flight on the ground; Jump lifts off again, Exit Flight releases it.
; Automatic lift pauses on the ground. Held-Jump glide still lands normally.
; Damaging collisions and other flight interruptions still end flight.
EndFlightOnGround=1
; Controller velocity units (Havok); tune in game after basic collision tests.
Speed=7.00
; Native actor sprint state selects this horizontal multiplier in all flight modes.
; Final target speed = Speed * SprintSpeedMultiplier (while sprinting) * flap bonus.
; Range 1..4; 1 disables base sprint acceleration. No effect on vertical speed.
SprintSpeedMultiplier=1.50
; 0 allows armed native sprint/high-speed flight; 1 forbids it during full flight.
; With 1, drawing a weapon ends high speed. Ground sprint and held glide stay native.
DisableSprintWhenArmed=0
; Acceleration changes speed; TurnResponse changes horizontal heading (larger = faster).
TurnResponse=12.00
Acceleration=4.00
Drag=1.20
; Total velocity lift budget per beat, delivered during visible downstroke progress.
FlapImpulse=5.50
MaxRise=10.00
; Shared simulated downward acceleration (Havok units/s^2) in both flight modes.
Gravity=5.00
; Slow-fall cap for manual glide, free-flight held Descend, and outside-flight held Jump.
GlideFallSpeed=3.00
FlapCooldown=0.50
; Actual sprint state + movement: each accepted flap adds this excess over 1x.
; 1.15 adds 0.15 to the separate flap multiplier per beat; 1 disables this bonus.
FlapSpeedMultiplier=1.15
MaxFlapSpeedMultiplier=1.60
FlapSpeedHoldSeconds=1.50
FlapSpeedFadeSeconds=3.00

[Stamina]
; 0..100, once at the first visible manual downstroke, including takeoff/high-speed entry.
; Raising is not charged; canceled beats release their pending reservation.
FlapCost=20.00
; 0..100 per second in Free Flight; this remains a continuous drain.
SustainPerSecond=0.00

[Compatibility]
; Suppress native sprint/exhaustion breathing during full airborne flight; preserves ground/NPC voices.
; Runtime dialogue conditions only; no voice files or saved forms are modified.
MuteFlightSprintBreath=1
; Optional AWU input and grounded-wing compatibility; auto-detected. AWU jump/damage settings stay unchanged.
AWU=1
; Keep custom weapon idle during flight: graph walk/run weights and visual sprint requests are suppressed.
ArmedFlightIdle=1
; Suppress foot contacts and direct foot SoundPlay annotations during flight, including retained ground contact.
; Ordinary ground movement and real landings after flight exit remain native.
MuteFlightFootsteps=1
; Use the resource's drawn-weapon / spell-hand flight poses too.
; Custom BFCO/MCO attack slots keep their own animations unless separately adapted.
PoseWhenWeaponsDrawn=1
; Optional TDM V2 yaw control during armed/unarmed forward flight; no hard dependency.
; Armed attacks/aim, blocking and TDM target lock retain their own facing control.
TDMFlightFacing=1

[FreeFlight]
; Automatic hover uses visible downstroke lift and Wingflight's simulated gravity.
; Full flight temporarily disables the owned player's native gravity; exit/load restores it.
; Flight/Gravity and Flight/FlapImpulse are shared by manual and free flight.
; The reference altitude schedules raising early enough to absorb descent near it.
; Velocity budget per automatic beat, NOT a height distance. Flight/MaxRise caps added lift.
; Lower impulse makes smaller hover excursions; very weak lift or slow raising can still lose altitude.
AutoFlapImpulse=6.50
; Stamina once at the first visible automatic downstroke. 0 is free; manual Space uses Stamina/FlapCost.
; Raising is not charged. Canceled beats release their pending reservation.
; Stamina/SustainPerSecond remains a separate optional free-flight drain.
AutoFlapCost=10.00
; Seconds for automatic raising and the resisted power downstroke.
UpstrokeSeconds=0.65
DownstrokeSeconds=0.34
; Maximum recovery playback speed; automatically slows to bridge the next altitude-triggered beat.
; Recovery stays in motion. Does not change gravity, impulse or the downstroke duration.
RecoveryRate=0.90
; Manual Space uses FastUpstrokeSeconds/FastDownstrokeSeconds from the wing adapter INI.
; Hold Descend for flutter-glide at Flight/GlideFallSpeed; Space can still climb.
; Release resumes automatic hover at the new altitude. High-speed flight ignores Descend.

[Body]
; Add flight-path elevation to the existing high-speed body pose (armed and unarmed).
; Level flight keeps the authored/base lean. Climbing reduces lean; diving increases it.
; Code-only COM rotation; 0 disables this additional layer. Startup-only; restart after edits.
HighSpeedPitch=1
; Response time constant in seconds: larger values follow climbing/diving more slowly.
HighSpeedPitchBlendSeconds=0.25

; Unarmed humanoid flight poses: native idle, movement and sprint entries.
UnarmedFlightPoses=1
; Native flight-only pose transition times in seconds (0.05..3). Larger is slower.
; Movement covers Hover <-> Forward Flight; sprint has separate in/out times.
; These do not change physical acceleration or OAR's custom replacement blends.
MovementBlendSeconds=0.75
SprintBlendInSeconds=0.75
SprintBlendOutSeconds=0.75
; Master switch for left/right turn banking, armed and unarmed.
Banking=1

; Armed high-speed COM lean, retaining custom idle/attacks. Independent of LowerBody/Enabled.
; Target pitch0..90 degrees; FAR sprint COM is about 59.34. 0 switch disables the new lean.
; In/out are full-range blend seconds0.05..3; larger values change posture more slowly.
ArmedSprintLean=1
; Auto-straighten authored root/pelvis/spine yaw and side tilt during armed high-speed idle.
; Retain forward/back bends and hand keys; release for attack/aim/block. 0 disables it.
ArmedSprintAutoAlign=1
ArmedSprintLeanDegrees=60
ArmedSprintLeanInSeconds=0.5
ArmedSprintLeanOutSeconds=0.5

; Shared runtime banking for armed/unarmed ordinary and high-speed forward flight.
; OAR selects neutral forward poses; the code overlay supplies all turn banking.
; ArmedBanking below disables ONLY armed banking.
; Applied about actor forward, after armed sprint straightening. Banking is the shared master switch.
; Attacks, blocking and target lock release banking.
ArmedBanking=1

; Shared maximum left/right bank in degrees (0..45); 0 disables runtime banking for both states.
BankDegrees=20

; Shared seconds from flat to maximum bank, or maximum back to flat (0.05..3).
; Does not control the authored forward/sprint pose transitions.
; Changing from full left to full right takes twice this time.
BankBlendSeconds=0.35

[Collision]
; Physical collision damage during full flight, including hard landings. 0 disables.
; Damaging impacts end flight and play a vanilla body-impact sound.
; Ordinary walking/falling and outside-flight held-Jump glide are unchanged.
Enabled=1
; Scale for player and directly contacted NPC/creature damage (0..10). 0 disables damage.
Multiplier=1.00
; Also hurt directly contacted actors, including allies. 0 = player damage only.
DamageActors=1
; Safe velocity into the collision surface in Havok units (0..100), not total movement speed.
; Base damage = 5 * max(0, impact speed - MinImpactSpeed)^2 * Multiplier.
; Native health/difficulty rules apply. Sliding along a wall does not count as a head-on hit.
; One continuous contact causes at most one hit; separate before hitting again.
MinImpactSpeed=8.00

[Wings]
; One Space-equivalent power beat on actual high-speed entry. 0 disables; 1 enables.
; Includes normal flap bonus/cost; waits for cooldown and cancels on high-speed exit.
FlapOnHighSpeedEntry=1
; Seconds after a high-speed large downstroke before blending back to small wingbeats (0..5).
; 0 starts the blend immediately; does not change the downstroke or blend duration.
; Does not affect ordinary flight or the Vampire Lord continuous large-wing loop. Restart after editing.
HighSpeedFlapHoldSeconds=0.50
; Wing-only counter-pitch during high-speed large beats, degrees 0..90.
; Fitted root stays fixed; subsequent two wing joints share correction 25/75%.
; Relative correction, not absolute world pitch. 0 disables; body/cruise unchanged.
HighSpeedPowerStrokeWingPitch=30
; Expanded flutter / slow descent: manual-mode glide and outside-flight held-Jump glide.
; Independent controls: playback speed changes frequency; amplitude changes motion around the expanded pose.
; Playback speed multiplier: 1 = original, 0.5 = half speed, 2 = double speed. Range: 0.25..4.
SpreadPlaybackMultiplier=1.00
; Flutter amplitude: 1 = original, 0.5 = half, 0 = fixed expanded center pose, 2 = double joint excursion. Range: 0..2.
; Amplitude does not change descent speed, cycle duration, large power strokes or high-speed small wingbeats.
SpreadAmplitudeMultiplier=0.5

; Sprint / high-speed flight: playback speed of the small-amplitude cruise wingbeats. Range: 0.25..4.
; 1 = original speed, 1.2 = 1.2x speed. Does not control Space-triggered large power strokes.
; Fairy/Chaurus: flight (including Descend) and held-Jump glide use the full jump loop at 4x.
; Sprint wing rate is 4x multiplied by this value. Ground animation is unchanged.
FastFlapPlaybackMultiplier=1.20

[Jump]
; AWU dragon/feather wing opening during ordinary jumps (0..100 percent).
; 0 = folded, 30 = subtle opening, 100 = fully spread. Live in Mod Control Panel / Jump.
; Final opening after the initial OverOpenPercent pose; small continuous motion remains.
; Does not change flight/glide wing control, lift or jump height.
WingOpenPercent=30.00
; Initial opening (0..100), independently set. 100 = fully open before settling.
; Example: OverOpenPercent=100 and WingOpenPercent=20 opens fully, then settles to 20.
; Set both percentages equal to remove the return motion.
OverOpenPercent=35.00
; Seconds to open to the peak, also used to fold on landing (0.05..3).
WingOpenSeconds=0.22
; Seconds to transition from OverOpenPercent to WingOpenPercent (0.05..3).
WingSettleSeconds=0.43
; Seconds per jump flutter cycle; smaller is faster (0.2..5).
; Same original spread/flutter loop as glide, with independent timing and amplitude.
WingFlutterPeriod=1.60
; Jump-only flutter amplitude (0..2): 0 = still, 1 = original, 2 = double.
; NOT multiplied by Wings/SpreadAmplitudeMultiplier. Partial WingOpenPercent still blends the motion with folded wings.
WingFlutterAmplitude=1.00
; Both AWU and Wingflight jump boosts can stack. Set WingedHeightMultiplier=1 for no extra Wingflight boost.
; Height multiplier, not velocity multiplier. Only the winged player native ground jump.
WingedHeightMultiplier=1.00

[Glide]
; Outside full flight: 1 = Wingflight held-Jump glide; 0 = AWU/other mods control it.
; Full-flight flapping/coasting are unaffected. AWU keeps its own MCM Gliding toggle.
Enabled=1
; Outside flight only: hold mapped Jump to spread wings; release returns to native falling.
HoldSeconds=0.25

[LowerBody]
; Lower Body tuning applies live through Mod Control Panel / Apply and Save.
; Direct INI edits require a restart. Enabled remains startup-only.
; Animation path logging follows General/DetailedLogging and applies live.
; Armed full flight only: preserve some authored thigh motion and add bounded knee inertia.
; 0 uses the selected flight animation legs without thigh retention or secondary swing.
RelaxedArmedLegs=1
; Original weapon/attack thigh animation retained: 0 = flight animation, 1 = original thighs. Range 0..1.
; Applied gradually with BlendInSeconds/BlendOutSeconds on drawing/sheathing.
ThighAnimationWeight=0.40
; Visual spring motion for calves/feet; 0 disables swing while keeping thigh retention.
SecondaryMotion=1
; Maximum added knee hinge rotation around the flight reference, degrees (0..30). 0 disables swing.
; This is a limit, not constant swing. Motion settles during steady flight.
; Feet inherit the calf and receive a small ankle correction. No sideways knee bending.
SwingMaxDegrees=20.00
; Restoring spring strength (1..100): larger values return to the target more firmly/quickly.
SwingStiffness=10.00
; Angular damping (0..40): larger values reduce wobble; 0 allows continued oscillation.
SwingDamping=5.00
; Acceleration/body-rotation influence (0..3): larger values make swing more noticeable.
; 0 removes driven swing. Bounded procedural motion only, not Havok ragdoll physics.
SwingInertia=2.00

; Armed third-person flight legs follow Hover, Forward Flight and Fast Flight HKX loops.
; Uses the three optional local pose files first, otherwise the corresponding FAR resources.
; Changes between references use Body/MovementBlendSeconds and SprintBlendIn/OutSeconds.
; Upper-body animation/events and bone translations/scales stay authored.
; Body/ArmedSprintLean separately controls armed high-speed COM pitch.
; Full flight only; outside-flight held-space glide keeps its native falling pose.
; 0 restores the original weapon/attack legs. Unarmed full-body poses are unaffected.
; FlightLegPose.ini is obsolete; the reference is sampled from the animation files.
Enabled=1
BlendInSeconds=0.30
BlendOutSeconds=0.25
)WF_INI";
}
