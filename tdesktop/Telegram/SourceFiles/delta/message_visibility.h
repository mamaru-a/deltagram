#pragma once

#include "account_scope.h"
#include "delta_rpc.h"

namespace Delta {

// Core events can name hidden transport messages (for example reactions).
// Only IDs in Core's visible history may create a new conversation item.
inline void CheckMessageVisibility(
		std::shared_ptr<DeltaRpc> rpc,
		const AccountScope &scope,
		int accountId,
		int chatId,
		int messageId,
		std::function<void(bool)> done) {
	rpc->call(QStringLiteral("get_message_ids"),
		{ accountId, chatId, false, false }, scope.bind([=](QJsonObject reply) {
			const auto result = reply.value(QStringLiteral("result"));
			done(!reply.contains(QStringLiteral("error"))
				&& result.isArray()
				&& result.toArray().contains(messageId));
		}));
}

} // namespace Delta
