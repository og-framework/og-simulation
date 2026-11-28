#pragma once
// SPDX-License-Identifier: MPL-2.0
// docs/SimTransport-rationale.md

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

using SimConnectionId = uint32_t;
inline constexpr SimConnectionId kInvalidSimConnection = 0u;

enum class SimLane : uint8_t { Control, SimClock, SimInput, SimRelay, SimState };
inline constexpr size_t kSimLaneCount = 5u;

static_assert(static_cast<uint8_t>(SimLane::Control) == 0u && static_cast<uint8_t>(SimLane::SimClock) == 1u
		&& static_cast<uint8_t>(SimLane::SimInput) == 2u && static_cast<uint8_t>(SimLane::SimRelay) == 3u
		&& static_cast<uint8_t>(SimLane::SimState) == 4u,
	"SimLane: the values are the transport's first five lane numbers, in its order (Control, SimClock, SimInput, SimRelay, "
	"SimState); a reorder or an insertion re-routes every message. Was SimTransport-rationale.md §2's lane-numbering rule");
static_assert(kSimLaneCount == static_cast<size_t>(SimLane::SimState) + 1u,
	"kSimLaneCount: one past the last SimLane, so a per-lane array has a slot for every lane");

template <typename T>
concept ClientSimTransport = requires(T& t, std::span<const std::byte> bytes)
{
	{ t.sendInputBundle(bytes) } -> std::same_as<void>;
	{ t.endInputSends() }        -> std::same_as<void>;
};

template <typename T>
concept ServerSimTransport = requires(T& t, SimConnectionId skipOwner, SimConnectionId conn, std::span<const std::byte> bytes)
{
	{ t.relay(skipOwner, bytes) }  -> std::same_as<void>;
	{ t.sendState(conn, bytes) }   -> std::same_as<void>;
	{ t.sendControl(conn, bytes) } -> std::same_as<void>;
	{ t.broadcastClock(bytes) }    -> std::same_as<void>;
};

namespace clientSimTransportSelfCheck
{
struct Conforming
{
	void sendInputBundle(std::span<const std::byte>);
	void endInputSends();
};

struct MissingEndInputSends
{
	void sendInputBundle(std::span<const std::byte>);
};

struct ReportsSendResult
{
	bool sendInputBundle(std::span<const std::byte>);
	void endInputSends();
};

struct TakesAMutableSpan
{
	void sendInputBundle(std::span<std::byte>);
	void endInputSends();
};

static_assert(ClientSimTransport<Conforming>,
	"ClientSimTransport self-check: the reference shape must satisfy the concept");
static_assert(!ClientSimTransport<MissingEndInputSends>,
	"ClientSimTransport self-check: endInputSends (the end of one send's bundle set) is required. Was SimTransport-rationale.md §3");
static_assert(!ClientSimTransport<ReportsSendResult>,
	"ClientSimTransport self-check: sendInputBundle returns void; a send is fire-and-forget and the sim never branches on it. "
	"Was SimTransport-rationale.md §5");
static_assert(!ClientSimTransport<TakesAMutableSpan>,
	"ClientSimTransport self-check: the bundle bytes are read-only to the transport. Was SimTransport-rationale.md §3");
} // namespace clientSimTransportSelfCheck

namespace serverSimTransportSelfCheck
{
struct Conforming
{
	void relay(SimConnectionId, std::span<const std::byte>);
	void sendState(SimConnectionId, std::span<const std::byte>);
	void sendControl(SimConnectionId, std::span<const std::byte>);
	void broadcastClock(std::span<const std::byte>);
};

struct MissingSendControl
{
	void relay(SimConnectionId, std::span<const std::byte>);
	void sendState(SimConnectionId, std::span<const std::byte>);
	void broadcastClock(std::span<const std::byte>);
};

struct MissingBroadcastClock
{
	void relay(SimConnectionId, std::span<const std::byte>);
	void sendState(SimConnectionId, std::span<const std::byte>);
	void sendControl(SimConnectionId, std::span<const std::byte>);
};

struct RelayWithoutSkipOwner
{
	void relay(std::span<const std::byte>);
	void sendState(SimConnectionId, std::span<const std::byte>);
	void sendControl(SimConnectionId, std::span<const std::byte>);
	void broadcastClock(std::span<const std::byte>);
};

struct ReportsSendResult
{
	void relay(SimConnectionId, std::span<const std::byte>);
	bool sendState(SimConnectionId, std::span<const std::byte>);
	void sendControl(SimConnectionId, std::span<const std::byte>);
	void broadcastClock(std::span<const std::byte>);
};

static_assert(ServerSimTransport<Conforming>,
	"ServerSimTransport self-check: the reference shape must satisfy the concept");
static_assert(!ServerSimTransport<MissingSendControl>,
	"ServerSimTransport self-check: sendControl (roster, floor and tier messages on the reliable lane) is required. Was SimTransport-rationale.md §4");
static_assert(!ServerSimTransport<MissingBroadcastClock>,
	"ServerSimTransport self-check: broadcastClock (the clock publish to every connection) is required. Was SimTransport-rationale.md §4");
static_assert(!ServerSimTransport<RelayWithoutSkipOwner>,
	"ServerSimTransport self-check: relay names the connection it skips, so a sender never receives its own relay. "
	"Was SimTransport-rationale.md §4");
static_assert(!ServerSimTransport<ReportsSendResult>,
	"ServerSimTransport self-check: every send returns void; a send is fire-and-forget and the sim never branches on it. "
	"Was SimTransport-rationale.md §5");
} // namespace serverSimTransportSelfCheck
