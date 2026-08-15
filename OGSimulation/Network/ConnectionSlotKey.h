#pragma once
// SPDX-License-Identifier: MPL-2.0

#include <cstddef>
#include <cstdint>
#include <functional>

#include "OGSimulation/Network/ConnectionTierTable.h"

// ---------------------------------------------------------------------------
// ConnectionSlotKey<Address> — (wire identity, player slot) composite key.
// (Stage 3 / T17; proposal_ogbrawler_netcode.md §8.2.)
//
// WHY THIS TYPE EXISTS — the distinction it encodes is the whole point.
//
// This game ships MULTIPLE PLAYER-CONTROLLED CHARACTERS ON ONE CLIENT: a shared
// isometric camera for couch brawls and co-op. UE implements that with one real
// UNetConnection per machine plus one UChildConnection per additional local
// player. That is a NORMAL, DESIGNED topology here — not split-screen exotica
// and not an edge case.
//
// It splits the per-connection state of this subsystem in two, and the two
// halves must be keyed DIFFERENTLY:
//
//   PER-WIRE  (key = Address alone). RTT, the derived tier, the effective input
//             delay, liveness/reaping. These are properties of the PHYSICAL LINK
//             — two characters sharing a cable share a latency. ConnectionTierTable
//             is keyed this way and MUST STAY that way (proposal §8.2 touchpoint 1).
//
//   PER-SLOT  (key = Address + playerSlot, i.e. THIS type). Input. Inputs are
//             produced per CHARACTER, so N characters on one wire produce N
//             independent input streams that must not be conflated.
//
// The defect this type fixes: ServerInputDelayQueue was originally keyed on
// Address alone, so in couch co-op every character on a machine collided on one
// deque and the queue's capture-tick dedup silently ate all but the first
// character's input for any tick they both captured on. The interim T10 mitigation
// detected the collision and refused to park the second character's input at all,
// which meant exactly ONE character per client received tier input delay while the
// rest ran undelayed — two players on one couch on two different input timelines
// against the same server tick, in precisely the mode the feature exists to serve.
//
// SHARED WITH STAGE 4 BY DESIGN. Proposal §8.2 specifies the substitution mask as
// indexed per (Address, player_slot) over a uint8. This type is that same
// player_slot notion, factored out so Stage 4's ServerSubstitutionTracker binds
// the existing concept rather than inventing a second one that could drift from
// it. `kMaxPlayerSlot` is the uint8 mask bound, and it is the reason the range
// check lives here rather than at one call site.
//
// ENGINE-AGNOSTIC. Sim-core header: includes ONLY other `OGSimulation/` headers
// and the STL. The `Address` half stays opaque, bound in production to
// `FUEConnectionHandle` and in the Catch2 suite to `FStandaloneTestHandle`.
//
// NO IMPLICIT CONVERSION FROM `Address`, DELIBERATELY. A converting constructor
// defaulting playerSlot to 0 would make every un-migrated call site compile and
// silently collapse onto slot 0 — reintroducing the exact defect this type was
// written to remove, but without the T10 guard that at least made it loud. Both
// halves must be named at every construction site.
//
// NAMESPACE NOTE: declared in the GLOBAL namespace, matching the rest of the
// OGSim core (see the same note on ConnectionTierTable.h and
// ServerInputDelayQueue.h). The design corpus writes `ogsim::` but no such
// namespace exists in this tree.
// ---------------------------------------------------------------------------

template <ConnectionAddress Address>
struct ConnectionSlotKey
{
    // Inclusive upper bound on playerSlot. Fixed by proposal §8.2: the Stage-4
    // substitution mask is a uint8 with one bit per local player, so slot 7 is
    // the highest index that mask can represent. Local-player counts in this
    // game are far below that, so the bound is a correctness fence against a
    // malformed/hostile derivation rather than a practical limit.
    static constexpr uint8_t kMaxPlayerSlot = 7;

    // The WIRE. Shared by every local player on one machine; this is the half
    // the tier table keys on.
    Address address{};

    // WHICH local player on that wire. 0 = the primary/parent connection's
    // player, 1..N = additional local players. See the UE binding
    // (SimulationManagerUImpl::derivePlayerSlot) for how this is produced from
    // the engine's own child-connection id, and why that source rather than the
    // Children array index.
    uint8_t playerSlot{ 0 };

    ConnectionSlotKey() = default;

    // Explicit: see the NO IMPLICIT CONVERSION note above.
    explicit ConnectionSlotKey(const Address& addr, uint8_t slot)
        : address(addr)
        , playerSlot(slot)
    {
    }

    // Range predicate, NOT an assert. Kept as a plain always-compiled bool so the
    // engine binding can consult it and take a documented fallback path, and so
    // the Catch2 suite can exercise the out-of-range branch without tripping an
    // abort. A core-side OG_CHECK would compile out of shipping and would give
    // the caller no way to recover.
    bool hasValidSlot() const
    {
        return playerSlot <= kMaxPlayerSlot;
    }

    // Hidden friend, matching FUEConnectionHandle's operator==: supplies
    // std::equality_comparable (and the C++20-rewritten operator!=) without
    // adding a name to the enclosing scope.
    friend bool operator==(const ConnectionSlotKey& lhs, const ConnectionSlotKey& rhs)
    {
        return lhs.address == rhs.address && lhs.playerSlot == rhs.playerSlot;
    }
};

namespace std
{
    template <ConnectionAddress Address>
    struct hash<ConnectionSlotKey<Address>>
    {
        std::size_t operator()(const ConnectionSlotKey<Address>& key) const noexcept
        {
            // Reuses the Address's own hash — which for FUEConnectionHandle is the
            // weak-pointer remote id, deliberately stable after the pointee dies so
            // a table keyed on possibly-dead connections can still find and reap
            // its entries. The slot is mixed in with the usual boost-style
            // constant; slots are small consecutive integers and would otherwise
            // land in adjacent buckets.
            const std::size_t addressHash = std::hash<Address>{}(key.address);
            return addressHash ^ (static_cast<std::size_t>(key.playerSlot) + 0x9e3779b9u +
                                  (addressHash << 6) + (addressHash >> 2));
        }
    };
}
