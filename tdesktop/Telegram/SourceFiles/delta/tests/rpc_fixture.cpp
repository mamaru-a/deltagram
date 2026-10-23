#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>

#include <iostream>
#include <optional>

using namespace Qt::StringLiterals;

void Reply(const QJsonObject &request, QJsonValue value) {
	std::cout << QJsonDocument(QJsonObject{
		{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, request[u"id"_s]},
		{u"result"_s, value},
	}).toJson(QJsonDocument::Compact).constData() << std::endl;
}

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	auto drafts = QMap<QString, QJsonObject>();
	auto held = std::optional<QJsonObject>();
	auto writes = 0;
	auto historyIds = QJsonArray{10, 80, 12};
	auto historyCalls = QJsonArray();
	const auto mode = qEnvironmentVariable("DC_ACCOUNTS_PATH");
	if (mode == u"history-large"_s) {
		historyIds = {};
		for (auto id = 1; id <= 205; ++id) historyIds.append(id);
	}
	for (std::string line; std::getline(std::cin, line);) {
		const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
		const auto method = request[u"method"_s].toString();
		const auto args = request[u"params"_s].toArray();
		if (method == u"crash"_s) return 17;
		if (method == u"malformed"_s) { std::cout << "{broken\n" << std::flush; continue; }
		if (method == u"incomplete"_s) {
			std::cout << QJsonDocument(QJsonObject{{u"id"_s, request[u"id"_s]}})
				.toJson(QJsonDocument::Compact).constData() << std::endl;
			continue;
		}
		if (method == u"partial"_s) { std::cout << "{unfinished" << std::flush; return 0; }
		if (method == u"hang"_s || method == u"get_next_event"_s) continue;
		if (method == u"hold"_s) { held = request; continue; }
		if (method == u"event"_s) {
			std::cout << "{\"method\":\"fixture_event\",\"params\":{}}" << std::endl;
			Reply(request, true);
			continue;
		}
		if (method == u"release"_s) {
			if (held) { Reply(*held, u"released"_s); held.reset(); }
			Reply(request, true);
			continue;
		}
		if (method == u"writes"_s) { Reply(request, writes); continue; }
		if (method == u"history_state"_s) {
			Reply(request, QJsonObject{{u"ids"_s, historyIds}, {u"calls"_s, historyCalls}});
			continue;
		}
		if (method == u"get_messages"_s) {
			historyCalls.append(method);
			auto messages = QJsonObject();
			for (const auto &value : args[1].toArray()) {
				const auto id = value.toInt();
				messages.insert(QString::number(id), QJsonObject{
					{u"kind"_s, mode == u"history-bad-message"_s ? u"loadingError"_s : u"message"_s},
					{u"timestamp"_s, mode == u"history-large"_s ? id : id == 10 ? 100 : id == 80 ? 300 : 200},
				});
			}
			Reply(request, messages);
			continue;
		}
		if (method == u"get_message_ids"_s || method == u"delete_messages"_s
			|| method == u"delete_messages_for_all"_s || method == u"delete_chat"_s) {
			historyCalls.append(method);
			const auto reject = (mode == u"history-reject-list"_s && method == u"get_message_ids"_s)
				|| (mode == u"history-reject-delete"_s && method == u"delete_messages_for_all"_s)
				|| (mode == u"history-reject-chat"_s && method == u"delete_chat"_s);
			if (reject) {
				std::cout << QJsonDocument(QJsonObject{
					{u"id"_s, request[u"id"_s]},
					{u"error"_s, QJsonObject{{u"message"_s, u"deletion rejected"_s}}},
				}).toJson(QJsonDocument::Compact).constData() << std::endl;
				continue;
			}
			if (method == u"get_message_ids"_s) {
				Reply(request, historyIds);
				if (mode == u"history-new-message"_s) historyIds.append(999);
				continue;
			}
			if (method == u"delete_chat"_s) {
				historyIds = {};
			} else {
				for (const auto &id : args[1].toArray()) {
					for (auto i = historyIds.size(); i > 0; --i) {
						if (historyIds[i - 1] == id) historyIds.removeAt(i - 1);
					}
				}
				if (mode == u"history-hold-delete"_s) { held = request; continue; }
			}
			Reply(request, QJsonValue());
			continue;
		}
		if (method == u"get_draft"_s || method == u"inspect_draft"_s
			|| method == u"misc_set_draft"_s || method == u"remove_draft"_s) {
			const auto key = QString::number(args[0].toInt()) + ':' + QString::number(args[1].toInt());
			if (!drafts.contains(key)) drafts[key] = {
				{u"text"_s, u"account-%1"_s.arg(args[0].toInt())},
				{u"file"_s, mode == u"plain"_s ? QString() : u"/fixture/attachment.bin"_s},
				{u"fileName"_s, u"attachment.bin"_s},
				{u"viewType"_s, u"File"_s},
			};
			if (method == u"get_draft"_s || method == u"inspect_draft"_s) {
				Reply(request, drafts[key]);
				continue;
			}
			++writes;
			if (mode == u"reject"_s) {
				std::cout << QJsonDocument(QJsonObject{
					{u"id"_s, request[u"id"_s]},
					{u"error"_s, QJsonObject{{u"message"_s, u"write rejected"_s}}},
				}).toJson(QJsonDocument::Compact).constData() << std::endl;
				continue;
			}
			drafts[key] = method == u"remove_draft"_s ? QJsonObject() : QJsonObject{
				{u"text"_s, args[2]}, {u"file"_s, args[3]}, {u"fileName"_s, args[4]},
				{u"quote"_s, QJsonObject{{u"messageId"_s, args[5]}}}, {u"viewType"_s, args[6]},
			};
			if (mode == u"hold-first-save"_s && writes == 1) { held = request; continue; }
			Reply(request, true);
			continue;
		}
		Reply(request, args);
	}
}
