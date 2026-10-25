#pragma once

#include <cstdint>
#include <limits>

namespace Delta {

// The bridge uses positive signed 32-bit Core IDs. Keep both identities in
// native media/cache keys, including across process restarts and profile order.
[[nodiscard]] constexpr std::uint64_t MediaIdentity(
		std::int32_t accountId,
		std::int32_t messageId) {
	return (accountId > 0 && messageId > 0)
		? (std::uint64_t(1) << 62)
			| (std::uint64_t(accountId) << 31)
			| std::uint64_t(messageId)
		: 0;
}

// Device-talk membership and ChannelRequest still use 32-bit Core chat IDs.
// Sender identities (1<<40)+contact must not be truncated into that set.
[[nodiscard]] constexpr bool CoreIdFitsInt(std::uint64_t id) {
	return id > 0
		&& id <= std::uint64_t(std::numeric_limits<std::int32_t>::max());
}

} // namespace Delta
