#pragma once

#include "account_inventory.h"
#include "account_scope.h"

namespace Delta {

inline void SetCoreChatMute(
		AccountInventoryRequest request,
		const AccountScope &scope,
		int account,
		int chat,
		int seconds,
		std::function<void(QString)> done) {
	if (account <= 0 || chat <= 0 || seconds < -1) {
		done(QStringLiteral("This chat notification setting is not available."));
		return;
	}
	auto duration = QJsonObject{{QStringLiteral("kind"), seconds < 0
		? QStringLiteral("Forever") : seconds == 0
		? QStringLiteral("NotMuted") : QStringLiteral("Until")}};
	if (seconds > 0) duration.insert(QStringLiteral("duration"), seconds);
	request(QStringLiteral("set_chat_mute_duration"), {account, chat, duration},
		scope.bind([done = std::move(done)](QJsonObject reply) {
			const auto success = !reply.contains(QStringLiteral("error"))
				&& reply.contains(QStringLiteral("result"))
				&& reply.value(QStringLiteral("result")).isNull();
			done(success ? QString() : QStringLiteral(
				"The chat notification change was not confirmed. Check its current state before trying again."));
		}));
}

} // namespace Delta
