// Click Buffer - input helper for Geometry Dash 2.2081 (Geode 5.x)
//
// Two features:
//
// 1) LANDING BUFFER (cube, ball, robot, spider + orbs/rings)
//    If you press jump up to N physics ticks BEFORE the press would actually do
//    something (landing, touching an orb/ring), the mod remembers the press and
//    re-sends it on the first tick where it works.
//
// 2) HOLD-MODE DELAY (ship, UFO, wave, swing)
//    These modes react to a click instantly, so there is nothing to wait for.
//    Instead every click (press AND release) is delayed by a fixed number of
//    ticks. If you tend to click N ticks early, it lands on time.
//
// How it hooks in:
//   * GJBaseGameLayer::handleButton    - sees every real press/release and
//                                        either forwards, buffers or delays it.
//   * GJBaseGameLayer::processCommands - runs once per physics tick (before the
//                                        player moves), releases delayed clicks,
//                                        counts the buffer down and fires it.
//   * PlayLayer::resetLevel / onQuit   - clears any pending state.

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <deque>

using namespace geode::prelude;

namespace {

constexpr int JUMP_BUTTON = 1; // PlayerButton::Jump. Left/Right (platformer) are never touched.

// A press or release that is being held back by the hold-mode delay.
struct DelayedEvent {
    bool down;
    uint64_t fireTick;
};

// One of these exists per control side: [0] = player 1 input, [1] = player 2 input.
struct Side {
    bool physDown = false;          // what the player's finger/key is really doing
    // landing buffer
    bool pending = false;           // a click is waiting to be fired
    uint64_t pressTick = 0;         // tick on which the real press happened
    uint64_t deadline = 0;          // last tick on which the buffered click may still fire
    uint64_t heldTicks = 1;         // how long the real press was held while waiting
    bool releaseScheduled = false;  // we fired a click for a finger that was already up
    uint64_t releaseAt = 0;         // tick on which we send the matching release
    // hold-mode delay
    bool pressDelayed = false;      // the current press went into the delay queue
    std::deque<DelayedEvent> queue; // delayed events, oldest first
};

Side g_sides[2];
uint64_t g_tick = 0;      // counts physics ticks while a level is running
bool g_injecting = false; // true while WE call handleButton, so our own hook ignores it

void resetState() {
    g_sides[0] = Side{};
    g_sides[1] = Side{};
}

struct InjectGuard {
    InjectGuard() { g_injecting = true; }
    ~InjectGuard() { g_injecting = false; }
};

// ---- settings -------------------------------------------------------------

bool optEnabled()      { return Mod::get()->getSettingValue<bool>("enabled"); }
int64_t optTicks()     { return Mod::get()->getSettingValue<int64_t>("buffer-ticks"); }
bool optGround()       { return Mod::get()->getSettingValue<bool>("buffer-ground"); }
bool optOrbs()         { return Mod::get()->getSettingValue<bool>("buffer-orbs"); }
int64_t optHoldDelay() { return Mod::get()->getSettingValue<int64_t>("hold-delay"); }
bool optPracticeOnly() { return Mod::get()->getSettingValue<bool>("practice-only"); }
bool optDebug()        { return Mod::get()->getSettingValue<bool>("debug-log"); }

// Only act inside a real PlayLayer (not the editor playtest), and only if enabled.
bool shouldRun(GJBaseGameLayer* gl) {
    auto* pl = PlayLayer::get();
    if (!pl || static_cast<GJBaseGameLayer*>(pl) != gl) return false;
    if (!optEnabled()) return false;
    if (optPracticeOnly() && !(gl->m_isPracticeMode || gl->m_isTestMode)) return false;
    return true;
}

// ---- reading the game state ----------------------------------------------

using Players = std::array<PlayerObject*, 2>;

// Which player(s) does a press on this control side affect?
// In dual mode on a normal (non 2-player) level, one input drives both icons.
Players playersFor(GJBaseGameLayer* gl, bool isPlayer1) {
    const bool dual = gl->m_gameState.m_isDualMode;
    const bool twoPlayer = gl->m_levelSettings && gl->m_levelSettings->m_twoPlayerMode;
    if (dual && !twoPlayer) return {gl->m_player1, gl->m_player2};
    return isPlayer1 ? Players{gl->m_player1, nullptr} : Players{nullptr, gl->m_player2};
}

// Ship, UFO, wave and swing: the click does something at any moment.
bool isHoldMode(PlayerObject* p) {
    return p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing;
}

// Hold modes plus a dash orb in progress: nothing to land-buffer.
bool reactsAnytime(PlayerObject* p) {
    return isHoldMode(p) || p->m_isDashing;
}

// Is the player touching at least one orb/ring that it has not used yet?
bool ringAvailable(PlayerObject* p) {
    auto* rings = p->m_touchingRings;
    if (!rings) return false;
    for (unsigned int i = 0; i < rings->count(); ++i) {
        auto* ring = static_cast<GameObject*>(rings->objectAtIndex(i));
        if (ring && !ring->hasBeenActivatedByPlayer(p)) return true;
    }
    return false;
}

struct Assessment {
    bool anyGroundType = false; // at least one affected player is cube/ball/robot/spider
    bool anyHoldMode = false;   // at least one affected player is ship/UFO/wave/swing
    bool groundServed = false;  // a ground-type player is on the ground right now
    bool orbServed = false;     // a ground-type player is touching an unused orb/ring right now
};

Assessment assess(GJBaseGameLayer* gl, bool isPlayer1) {
    Assessment a;
    for (auto* p : playersFor(gl, isPlayer1)) {
        if (!p || p->m_isDead) continue;
        if (isHoldMode(p)) a.anyHoldMode = true;
        if (reactsAnytime(p)) continue;
        a.anyGroundType = true;
        if (p->m_isOnGround) a.groundServed = true;
        if (ringAvailable(p)) a.orbServed = true;
    }
    return a;
}

// ---- sending clicks to the game ------------------------------------------

void sendButton(bool down, bool isPlayer1) {
    auto* pl = PlayLayer::get();
    if (!pl) return;
    InjectGuard guard;
    pl->handleButton(down, JUMP_BUTTON, isPlayer1);
}

// Add an event to a side's delay queue. Events keep their order, and never share
// a tick, so a press and its release can't collapse into the same tick even if
// the delay setting is changed while clicks are queued.
void enqueue(Side& s, bool down, uint64_t delay) {
    uint64_t fireTick = g_tick + delay;
    if (!s.queue.empty()) fireTick = std::max(fireTick, s.queue.back().fireTick + 1);
    s.queue.push_back({down, fireTick});
}

// Send every delayed event right now (used when the mod turns off mid-level, so
// the game never ends up thinking a button is stuck down).
void flushQueues() {
    for (int i = 0; i < 2; ++i) {
        Side& s = g_sides[i];
        while (!s.queue.empty()) {
            const DelayedEvent e = s.queue.front();
            s.queue.pop_front();
            sendButton(e.down, i == 0);
        }
    }
}

void fireBufferedClick(int index, Side& s) {
    const bool isPlayer1 = (index == 0);
    const bool fingerStillDown = s.physDown;

    // The game already thinks the button is down if the finger is still down,
    // so release first to make it see a brand new press.
    if (fingerStillDown) sendButton(false, isPlayer1);
    sendButton(true, isPlayer1);

    if (optDebug()) {
        log::info("[ClickBuffer] fired buffered click (P{}) after {} tick(s)",
                  isPlayer1 ? 1 : 2, g_tick - s.pressTick);
    }

    s.pending = false;

    // If the finger was already up, send the release ourselves after the same
    // hold time the player used.
    if (!fingerStillDown) {
        s.releaseScheduled = true;
        s.releaseAt = g_tick + std::max<uint64_t>(1, s.heldTicks);
    }
}

// Runs once per physics tick, BEFORE the game processes that tick.
void onTick(GJBaseGameLayer* gl) {
    const bool ground = optGround();
    const bool orbs = optOrbs();

    for (int i = 0; i < 2; ++i) {
        Side& s = g_sides[i];

        // 1) delayed clicks that are due (hold-mode delay)
        while (!s.queue.empty() && g_tick >= s.queue.front().fireTick) {
            const DelayedEvent e = s.queue.front();
            s.queue.pop_front();
            sendButton(e.down, i == 0);
        }

        // 2) the release that matches a buffered click we fired for a lifted finger
        if (s.releaseScheduled && g_tick >= s.releaseAt) {
            s.releaseScheduled = false;
            if (!s.physDown) sendButton(false, i == 0);
        }

        // 3) the landing buffer
        if (!s.pending) continue;

        if (g_tick > s.deadline) {
            s.pending = false;
            if (optDebug()) log::info("[ClickBuffer] buffered click expired (P{})", i + 1);
            continue;
        }

        const Assessment a = assess(gl, i == 0);

        // Everyone affected died or switched to a ship/UFO/wave: drop it.
        if (!a.anyGroundType) {
            s.pending = false;
            continue;
        }

        if ((ground && a.groundServed) || (orbs && a.orbServed)) {
            fireBufferedClick(i, s);
        }
    }
}

} // namespace

class $modify(ClickBufferBaseGameLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        // Pass through: our own injected clicks, non-jump buttons, or mod inactive.
        if (g_injecting || button != JUMP_BUTTON || !shouldRun(this)) {
            GJBaseGameLayer::handleButton(down, button, isPlayer1);
            return;
        }

        Side& s = g_sides[isPlayer1 ? 0 : 1];
        const uint64_t delay = static_cast<uint64_t>(std::max<int64_t>(0, optHoldDelay()));

        if (down) {
            const Assessment a = assess(this, isPlayer1);

            s.physDown = true;
            s.pending = false;
            s.heldTicks = 1;

            // Hold-mode delay: only when every affected icon is ship/UFO/wave/swing.
            // If earlier events are still queued, queue this one too so order is kept.
            const bool holdOnly = a.anyHoldMode && !a.anyGroundType;
            if ((holdOnly && delay > 0) || !s.queue.empty()) {
                s.pressDelayed = true;
                enqueue(s, true, delay);
                if (optDebug()) {
                    log::info("[ClickBuffer] delaying press by {} tick(s) (P{})",
                              delay, isPlayer1 ? 1 : 2);
                }
                return;
            }
            s.pressDelayed = false;

            // Landing buffer: judge the press BEFORE forwarding it.
            const bool used = !a.anyGroundType || a.groundServed || a.orbServed;

            GJBaseGameLayer::handleButton(down, button, isPlayer1);

            const int64_t ticks = optTicks();
            if (!used && ticks > 0 && (optGround() || optOrbs())) {
                s.pending = true;
                s.pressTick = g_tick;
                s.deadline = g_tick + static_cast<uint64_t>(ticks);
                if (optDebug()) {
                    log::info("[ClickBuffer] press not usable yet, buffering for up to {} tick(s) (P{})",
                              ticks, isPlayer1 ? 1 : 2);
                }
            }
        } else {
            s.physDown = false;
            if (s.pending) {
                s.heldTicks = std::max<uint64_t>(1, g_tick - s.pressTick);
            }

            // A release follows its press: if the press was delayed (or events are
            // still queued), delay the release by the same amount.
            if (s.pressDelayed || !s.queue.empty()) {
                s.pressDelayed = false;
                enqueue(s, false, delay);
                if (optDebug()) {
                    log::info("[ClickBuffer] delaying release by {} tick(s) (P{})",
                              delay, isPlayer1 ? 1 : 2);
                }
                return;
            }

            GJBaseGameLayer::handleButton(down, button, isPlayer1);
        }
    }

    void processCommands(float dt, bool isHalfTick, bool isLastTick) {
        // Half ticks are extra mid-step calls; only count and act on whole ticks.
        if (!isHalfTick) {
            if (shouldRun(this)) {
                ++g_tick;
                onTick(this);
            } else {
                flushQueues();
                resetState();
            }
        }
        GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
    }
};

class $modify(ClickBufferPlayLayer, PlayLayer) {
    void resetLevel() {
        resetState();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        resetState();
        PlayLayer::onQuit();
    }
};
