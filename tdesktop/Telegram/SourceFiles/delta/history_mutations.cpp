#include "history_mutations.h"

using namespace Qt::StringLiterals;

namespace Delta {
namespace {

QString Error(const QJsonObject &reply) {
	return reply.value(u"error"_s).toObject().value(u"message"_s)
		.toString(u"Could not delete chat history."_s);
}

} // namespace

void DeleteChatHistory(
		std::shared_ptr<DeltaRpc> rpc,
		const AccountScope &scope,
		int accountId,
		int chatId,
		bool removeChat,
		bool revoke,
		std::function<void(HistoryDeletionResult)> done,
		std::optional<HistoryDateRange> dates) {
	if (dates && (removeChat || dates->after >= dates->before)) {
		done({ {}, false, u"Invalid history date range."_s });
		return;
	}
	const auto request = [rpc, &scope](
			const QString &method,
			const QJsonArray &params,
			DeltaRpc::Callback callback) {
		rpc->call(method, params, scope.bind(std::move(callback)));
	};
	const auto finish = [=](QVector<int> deleted) {
		if (!removeChat) {
			done({ std::move(deleted), false, {} });
			return;
		}
		request(u"delete_chat"_s, { accountId, chatId }, [=](QJsonObject reply) {
			const auto failed = reply.contains(u"error"_s);
			done({ deleted, !failed, failed ? Error(reply) : QString() });
		});
	};
	if (removeChat && !revoke) {
		finish({});
		return;
	}
	const auto erase = [=](QJsonArray values) {
		if (values.empty()) {
			finish({});
			return;
		}
		auto ids = QVector<int>();
		for (const auto &value : values) ids.push_back(value.toInt());
		request(revoke ? u"delete_messages_for_all"_s : u"delete_messages"_s,
			{ accountId, values }, [=](QJsonObject deleted) {
				if (deleted.contains(u"error"_s)) {
					done({ {}, false, Error(deleted) });
					return;
				}
				finish(ids);
			});
	};
	request(u"get_message_ids"_s, { accountId, chatId, false, false },
		[=](QJsonObject reply) {
			if (reply.contains(u"error"_s)) {
				done({ {}, false, Error(reply) });
				return;
			}
			const auto result = reply.value(u"result"_s);
			if (!result.isArray()) {
				done({ {}, false, u"Core returned an invalid message list."_s });
				return;
			}
			const auto values = result.toArray();
			for (const auto &value : values) {
				const auto id = value.toInt();
				if (id <= 0 || value.toDouble() != id) {
					done({ {}, false, u"Core returned an invalid message ID."_s });
					return;
				}
			}
			if (!dates) {
				erase(values);
				return;
			}
			struct Selection {
				qsizetype next = 0;
				QJsonArray ids;
			};
			const auto selected = std::make_shared<Selection>();
			const auto run = std::make_shared<std::function<void()>>();
			const auto weakRun = std::weak_ptr<std::function<void()>>(run);
			*run = [=] {
				if (selected->next == values.size()) {
					erase(selected->ids);
					return;
				}
				auto batch = QJsonArray();
				while (selected->next < values.size() && batch.size() < 100) {
					batch.append(values[selected->next++]);
				}
				const auto continuation = weakRun.lock();
				request(u"get_messages"_s, { accountId, batch }, [=](QJsonObject reply) {
					if (reply.contains(u"error"_s)) {
						done({ {}, false, Error(reply) });
						return;
					}
					const auto messages = reply.value(u"result"_s).toObject();
					for (const auto &id : batch) {
						const auto message = messages.value(QString::number(id.toInt())).toObject();
						const auto timestamp = message.value(u"timestamp"_s);
						if (message.value(u"kind"_s).toString() != u"message"_s
							|| !timestamp.isDouble()) {
							done({ {}, false, u"Could not load messages for history deletion."_s });
							return;
						}
						if (timestamp.toInteger() > dates->after
							&& timestamp.toInteger() < dates->before) {
							selected->ids.append(id);
						}
					}
					if (continuation) (*continuation)();
				});
			};
			(*run)();
		});
}

} // namespace Delta
