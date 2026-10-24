#include "delta_rpc.h"
#include "draft_store.h"
#include "account_scope.h"
#include "account_inventory.h"
#include "message_order.h"
#include "message_visibility.h"
#include "message_download.h"
#include "message_capabilities.h"
#include "history_mutations.h"
#include "media_identity.h"

#include <QtTest/QtTest>

using namespace Qt::StringLiterals;

namespace {

QJsonObject CoreCall(DeltaRpc &rpc, const QString &method, const QJsonArray &params) {
	QEventLoop loop;
	QTimer timeout;
	timeout.setSingleShot(true);
	QObject::connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
	auto reply = QJsonObject();
	auto completed = false;
	rpc.call(method, params, [&](QJsonObject result) {
		reply = std::move(result);
		completed = true;
		loop.quit();
	});
	if (!completed) {
		timeout.start(10000);
		loop.exec();
	}
	if (!completed) rpc.stop();
	return reply;
}

} // namespace

class IntegrityTests final : public QObject {
	Q_OBJECT

private slots:
	void draftHandoffFailures_data() {
		QTest::addColumn<QString>("mode");
		QTest::addColumn<bool>("success");
		QTest::addColumn<int>("sends");
		QTest::addColumn<bool>("attachment");
		QTest::newRow("accepted") << u"handoff-ok"_s << true << 1 << false;
		QTest::newRow("accepted-reply-lost") << u"handoff-lost-ack"_s << true << 1 << false;
		QTest::newRow("save-rejected") << u"handoff-reject-save"_s << false << 0 << false;
		QTest::newRow("send-rejected") << u"handoff-reject-send"_s << false << 1 << false;
		QTest::newRow("attachment-preserved") << u"attachment"_s << false << 0 << false;
		QTest::newRow("file-accepted") << u"handoff-ok"_s << true << 1 << true;
		QTest::newRow("file-accepted-reply-lost") << u"handoff-lost-ack"_s << true << 1 << true;
	}

	void draftHandoffFailures() {
		QFETCH(QString, mode);
		QFETCH(bool, success);
		QFETCH(int, sends);
		QFETCH(bool, attachment);
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(QCoreApplication::applicationDirPath() + u"/delta_rpc_fixture"_s, mode);
		Delta::DraftStore drafts(rpc, [](QString) {});
		auto completed = false;
		auto duplicateRejected = false;
		const auto done = [&](int id, QString error) {
			QCOMPARE(id > 0, success);
			QCOMPARE(error.isEmpty(), success);
			completed = true;
		};
		if (attachment) {
			drafts.sendAttachment(2, 10, Delta::ComposeDraft{
				u"Keep this message"_s, 0, u"/fixture/upload.bin"_s, u"upload.bin"_s, u"File"_s,
			}, done);
		} else {
			drafts.sendText(2, 10, u"Keep this message"_s, 0, done);
		}
		drafts.sendText(2, 10, u"Duplicate"_s, 0, [&](int id, QString error) {
			QCOMPARE(id, 0);
			QVERIFY(!error.isEmpty());
			duplicateRejected = true;
		});
		QVERIFY(duplicateRejected);
		drafts.save(2, 10, {}, 0);
		QTRY_VERIFY(completed);
		QCOMPARE(CoreCall(*rpc, u"sends"_s, {})[u"result"_s].toInt(), sends);
		const auto draft = CoreCall(*rpc, u"inspect_draft"_s, {2, 10})[u"result"_s].toObject();
		if (success) {
			QVERIFY(draft.isEmpty());
		} else if (sends) {
			QCOMPARE(draft[u"text"_s].toString(), u"Keep this message"_s);
		} else {
			QCOMPARE(draft[u"text"_s].toString(), u"account-2"_s);
		}
		QCOMPARE(CoreCall(*rpc, u"inspect_draft"_s, {1, 10})[u"result"_s]
			.toObject()[u"text"_s].toString(), u"account-1"_s);
		rpc->stop();
	}

	void attachmentRemoval_data() {
		QTest::addColumn<QString>("mode");
		QTest::addColumn<QString>("expectedFile");
		QTest::addColumn<bool>("success");
		QTest::newRow("remove-file-keep-caption") << u"attachment"_s << u"/fixture/attachment.bin"_s << true;
		QTest::newRow("remove-reply-lost") << u"discard-lost-ack"_s << u"/fixture/attachment.bin"_s << true;
		QTest::newRow("remove-rejected") << u"reject"_s << u"/fixture/attachment.bin"_s << false;
		QTest::newRow("stale-file-preserved") << u"attachment"_s << u"/fixture/older.bin"_s << false;
	}

	void attachmentRemoval() {
		QFETCH(QString, mode);
		QFETCH(QString, expectedFile);
		QFETCH(bool, success);
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(QCoreApplication::applicationDirPath() + u"/delta_rpc_fixture"_s, mode);
		Delta::DraftStore drafts(rpc, [](QString) {});
		auto completed = false;
		drafts.discardAttachment(2, 10, expectedFile, [&](bool removed) {
			QCOMPARE(removed, success);
			completed = true;
		});
		QVERIFY(drafts.preparingSend());
		drafts.save(2, 10, {}, 0);
		QTRY_VERIFY(completed);
		QVERIFY(!drafts.sending(2, 10));
		auto loaded = false;
		drafts.load(2, 10, [&](Delta::ComposeDraft draft, QString error) {
			QVERIFY(error.isEmpty());
			QCOMPARE(draft.file.isEmpty(), success);
			QCOMPARE(draft.text, u"account-2"_s);
			loaded = true;
		});
		QTRY_VERIFY(loaded);
		QCOMPARE(CoreCall(*rpc, u"sends"_s, {})[u"result"_s].toInt(), 0);
		QCOMPARE(CoreCall(*rpc, u"inspect_draft"_s, {1, 10})[u"result"_s]
			.toObject()[u"file"_s].toString(), u"/fixture/attachment.bin"_s);
		rpc->stop();
	}

	void attachmentSaveReplyLost() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(QCoreApplication::applicationDirPath() + u"/delta_rpc_fixture"_s,
			u"handoff-save-lost-ack"_s);
		Delta::DraftStore drafts(rpc, [](QString) {});
		auto completed = false;
		drafts.sendAttachment(2, 10, Delta::ComposeDraft{
			u"Caption"_s, 0, u"/picker/upload.bin"_s, u"upload.bin"_s, u"File"_s,
		}, [&](int id, QString error) {
			QCOMPARE(id, 0);
			QVERIFY(!error.isEmpty());
			completed = true;
		});
		QTRY_VERIFY(completed);
		auto loaded = false;
		drafts.load(2, 10, [&](Delta::ComposeDraft draft, QString error) {
			QVERIFY(error.isEmpty());
			QCOMPARE(draft.file, u"/core/managed.bin"_s);
			QCOMPARE(draft.text, u"Caption"_s);
			loaded = true;
		});
		QTRY_VERIFY(loaded);
		QCOMPARE(CoreCall(*rpc, u"sends"_s, {})[u"result"_s].toInt(), 0);
		rpc->stop();
	}

	void realCoreRejectedDraftHandoff() {
		const auto program = qEnvironmentVariable("DELTA_TEST_CORE_RPC");
		if (program.isEmpty()) QSKIP("Set DELTA_TEST_CORE_RPC to test the real Core server.");
		QTemporaryDir temporary;
		auto rpc = std::make_shared<DeltaRpc>();
		const auto accounts = temporary.filePath(u"accounts"_s);
		rpc->start(program, accounts);
		const auto account = CoreCall(*rpc, u"add_account"_s, {})[u"result"_s].toInt();
		const auto chat = CoreCall(*rpc, u"create_chat_by_contact_id"_s, {account, 1})[u"result"_s].toInt();
		QVERIFY(account > 0 && chat > 0);
		Delta::DraftStore drafts(rpc, [](QString) { QFAIL("Unexpected draft failure"); });
		auto completed = false;
		drafts.sendText(account, chat, u"Durable Core handoff"_s, 0, [&](int id, QString error) {
			// An unconfigured account cannot send, even to Saved Messages.
			QVERIFY(!error.isEmpty());
			QCOMPARE(id, 0);
			completed = true;
		});
		QVERIFY(drafts.sending(account, chat));
		drafts.save(account, chat, {}, 0); // A stale UI clear cannot erase the pending draft.
		QTRY_VERIFY(completed);
		QVERIFY(!drafts.sending(account, chat));
		rpc->stop();
		rpc->start(program, accounts);
		const auto persisted = CoreCall(*rpc, u"get_draft"_s, {account, chat})[u"result"_s].toObject();
		QCOMPARE(persisted[u"text"_s].toString(), u"Durable Core handoff"_s);
		QCOMPARE(persisted[u"state"_s].toInt(), 19);
		rpc->stop();
	}

	void realCoreRejectedAttachmentHandoff() {
		const auto program = qEnvironmentVariable("DELTA_TEST_CORE_RPC");
		if (program.isEmpty()) QSKIP("Set DELTA_TEST_CORE_RPC to test the real Core server.");
		QTemporaryDir temporary;
		QVERIFY(temporary.isValid());
		const auto original = temporary.filePath(u"attachment.txt"_s);
		auto file = QFile(original);
		QVERIFY(file.open(QIODevice::WriteOnly));
		const auto bytes = QByteArray("Core owns the recoverable attachment bytes.\n");
		QCOMPARE(file.write(bytes), bytes.size());
		file.close();
		auto rpc = std::make_shared<DeltaRpc>();
		const auto accounts = temporary.filePath(u"accounts"_s);
		rpc->start(program, accounts);
		const auto account = CoreCall(*rpc, u"add_account"_s, {})[u"result"_s].toInt();
		const auto chat = CoreCall(*rpc, u"create_chat_by_contact_id"_s, {account, 1})[u"result"_s].toInt();
		QVERIFY(account > 0 && chat > 0);
		Delta::DraftStore drafts(rpc, [](QString) { QFAIL("Unexpected draft failure"); });
		auto completed = false;
		drafts.sendAttachment(account, chat,
			Delta::ComposeDraft{ u"Recoverable attachment caption"_s, 0, original, u"attachment.txt"_s, u"File"_s },
			[&](int id, QString error) {
				QCOMPARE(id, 0); // This account has no configured transport.
				QVERIFY(!error.isEmpty());
				completed = true;
			});
		QVERIFY(drafts.preparingSend());
		drafts.save(account, chat, {}, 0); // A stale donor clear cannot erase this file.
		QTRY_VERIFY(completed);
		QVERIFY(!drafts.preparingSend());
		auto recovered = Delta::ComposeDraft();
		drafts.load(account, chat, [&](Delta::ComposeDraft draft, QString error) {
			QVERIFY(error.isEmpty());
			recovered = std::move(draft);
		});
		QTRY_VERIFY(!recovered.file.isEmpty());
		QVERIFY(recovered.file != original);
		QVERIFY(QFile::remove(original));
		rpc->stop();
		rpc->start(program, accounts);
		const auto persisted = CoreCall(*rpc, u"get_draft"_s, {account, chat})[u"result"_s].toObject();
		QCOMPARE(persisted[u"text"_s].toString(), u"Recoverable attachment caption"_s);
		QCOMPARE(persisted[u"state"_s].toInt(), 19);
		QCOMPARE(persisted[u"fileName"_s].toString(), u"attachment.txt"_s);
		QCOMPARE(persisted[u"file"_s].toString(), recovered.file);
		QFile saved(recovered.file);
		QVERIFY(saved.open(QIODevice::ReadOnly));
		QCOMPARE(saved.readAll(), bytes);
		auto removed = false;
		drafts.discardAttachment(account, chat, recovered.file, [&](bool success) {
			QVERIFY(success);
			removed = true;
		});
		QTRY_VERIFY(removed);
		const auto caption = CoreCall(*rpc, u"get_draft"_s, {account, chat})[u"result"_s].toObject();
		QVERIFY(caption[u"file"_s].toString().isEmpty());
		QCOMPARE(caption[u"text"_s].toString(), u"Recoverable attachment caption"_s);
		rpc->stop();
	}

	void editingFollowsCoreCapabilities() {
		const auto editable = QJsonObject{
			{u"fromId"_s, 1}, {u"isInfo"_s, false}, {u"hasHtml"_s, false},
			{u"viewType"_s, u"Text"_s}, {u"text"_s, u"Original"_s},
		};
		QVERIFY(Delta::CanEditCoreMessage(editable));
		QVERIFY(!Delta::CanEditCoreMessage({}));
		for (const auto &field : {u"fromId"_s, u"isInfo"_s, u"hasHtml"_s,
				u"viewType"_s, u"text"_s}) {
			auto missing = editable;
			missing.remove(field);
			QVERIFY(!Delta::CanEditCoreMessage(missing));
		}
		for (const auto &change : {
				QJsonObject{{u"fromId"_s, 10}},
				QJsonObject{{u"isInfo"_s, true}},
				QJsonObject{{u"hasHtml"_s, true}},
				QJsonObject{{u"hasHtml"_s, u"false"_s}},
				QJsonObject{{u"viewType"_s, u"Call"_s}},
				QJsonObject{{u"text"_s, u""_s}}}) {
			auto message = editable;
			message.insert(change.begin().key(), change.begin().value());
			QVERIFY(!Delta::CanEditCoreMessage(message));
		}
		auto caption = editable;
		caption.insert(u"viewType"_s, u"File"_s);
		QVERIFY(Delta::CanEditCoreMessage(caption));
	}

	void accountDiscoveryRequiresConfirmedState() {
		for (const auto reply : {QJsonObject{{u"result"_s, QJsonArray()}},
				QJsonObject{{u"result"_s, QJsonValue()}}, QJsonObject{{u"error"_s, u"unavailable"_s}}}) {
			auto completed = false;
			Delta::ReadAccountInventory([&](QString method, QJsonArray, DeltaRpc::Callback done) {
				QCOMPARE(method, u"get_all_account_ids"_s);
				done(reply);
			}, [&](Delta::AccountInventory result) {
				QCOMPARE(result.error.isEmpty(), reply[u"result"_s].isArray());
				QVERIFY(result.configured.empty());
				QCOMPARE(result.unconfigured, 0);
				completed = true;
			});
			QVERIFY(completed);
		}
		const auto replies = QVector<QJsonObject>{
			{{u"result"_s, false}}, {{u"result"_s, true}},
			{{u"result"_s, QJsonValue()}}, {{u"result"_s, 0}},
			{{u"result"_s, u"false"_s}}, {},
			{{u"result"_s, false}, {u"error"_s, u"read failed"_s}},
		};
		for (auto index = 0; index != replies.size(); ++index) {
			auto pending = QMap<int, DeltaRpc::Callback>();
			auto result = std::optional<Delta::AccountInventory>();
			auto completed = 0;
			Delta::ReadAccountInventory([&](QString method, QJsonArray params, DeltaRpc::Callback done) {
				if (method == u"get_all_account_ids"_s) {
					done(QJsonObject{{u"result"_s, QJsonArray{9, 2}}});
				} else {
					QCOMPARE(method, u"is_configured"_s);
					QCOMPARE(params.size(), 1);
					pending.insert(params[0].toInt(), std::move(done));
				}
			}, [&](Delta::AccountInventory inventory) {
				result = std::move(inventory);
				++completed;
			});
			QCOMPARE(pending.size(), 2);
			pending[2](QJsonObject{{u"result"_s, false}});
			QVERIFY(!result.has_value());
			pending[9](replies[index]);
			QCOMPARE(completed, 1);
			QVERIFY(result.has_value());
			if (index >= 2) {
				QVERIFY(!result->error.isEmpty());
				QVERIFY(result->configured.empty());
				QCOMPARE(result->unconfigured, 0);
			} else {
				QVERIFY(result->error.isEmpty());
				QCOMPARE(result->unconfigured, index == 0 ? 9 : 2);
				QCOMPARE(result->configured, index == 0 ? QVector<int>() : QVector<int>{9});
			}
		}
		for (const auto ids : {QJsonArray{0}, QJsonArray{-1}, QJsonArray{9, 9},
				QJsonArray{1.5}, QJsonArray{u"9"_s}}) {
			auto completed = false;
			Delta::ReadAccountInventory([&](QString method, QJsonArray, DeltaRpc::Callback done) {
				QCOMPARE(method, u"get_all_account_ids"_s);
				done(QJsonObject{{u"result"_s, ids}});
			}, [&](Delta::AccountInventory result) {
				QVERIFY(!result.error.isEmpty());
				QVERIFY(result.configured.empty());
				QCOMPARE(result.unconfigured, 0);
				completed = true;
			});
			QVERIFY(completed);
		}
	}

	void downloadsRespectCoreStateAndAccount() {
		for (const auto state : {u"Available"_s, u"Failure"_s, u"Done"_s,
				u"InProgress"_s, u"Undecipherable"_s, u"unknown"_s,
				u"reject-read"_s, u"reject-write"_s}) {
			auto rpc = std::make_shared<DeltaRpc>();
			rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"download-"_s + state);
			Delta::AccountScope scope;
			auto completed = false;
			auto error = QString();
			Delta::DownloadCoreMessage(rpc, scope, 7, 99, [&](QString result) {
				error = result;
				completed = true;
			});
			QTRY_VERIFY(completed);
			const auto accepted = Delta::CanDownloadMessageState(state);
			QCOMPARE(error.isEmpty(), accepted || state == u"Done"_s || state == u"InProgress"_s);
			const auto calls = CoreCall(*rpc, u"download_requests"_s, {})[u"result"_s].toArray();
			QCOMPARE(calls.size(), accepted || state == u"reject-write"_s ? 2 : 1);
			for (const auto &call : calls) {
				QCOMPARE(call.toObject()[u"params"_s].toArray(), (QJsonArray{7, 99}));
			}
			if (calls.size() == 2) {
				QCOMPARE(calls.last().toObject()[u"method"_s].toString(), u"download_full_message"_s);
			}
			rpc->stop();
		}
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"download-Available"_s);
		Delta::AccountScope scope;
		Delta::DownloadCoreMessage(rpc, scope, 7, 99, [](QString) {
			QFAIL("A download callback escaped its account");
		});
		scope.invalidate();
		const auto calls = CoreCall(*rpc, u"download_requests"_s, {})[u"result"_s].toArray();
		QCOMPARE(calls.size(), 1);
		rpc->stop();
	}

	void eventsOnlyExposeVisibleHistory() {
		for (const auto mode : {u"normal"_s, u"history-reject-list"_s}) {
			auto rpc = std::make_shared<DeltaRpc>();
			rpc->start(u"" RPC_FIXTURE_PATH ""_s, mode);
			Delta::AccountScope scope;
			auto completed = 0;
			Delta::CheckMessageVisibility(rpc, scope, 1, 10, 80, [&](bool visible) {
				QCOMPARE(visible, mode == u"normal"_s);
				++completed;
			});
			Delta::CheckMessageVisibility(rpc, scope, 1, 10, 999, [&](bool visible) {
				QVERIFY(!visible);
				++completed;
			});
			QTRY_COMPARE(completed, 2);
			Delta::CheckMessageVisibility(rpc, scope, 1, 10, 80, [&](bool) {
				QFAIL("An old account event escaped cancellation");
			});
			scope.invalidate();
			QVERIFY(CoreCall(*rpc, u"history_state"_s, {}).contains(u"result"_s));
			rpc->stop();
		}
	}

	void attachmentCacheIdentityIncludesAccount() {
		const auto first = Delta::MediaIdentity(1, 12);
		const auto second = Delta::MediaIdentity(2, 12);
		QVERIFY(first != second);
		auto cache = QHash<quint64, QByteArray>();
		cache.insert(first, "primary attachment");
		cache.insert(second, "alternate attachment");
		QCOMPARE(cache.value(Delta::MediaIdentity(1, 12)), QByteArray("primary attachment"));
		QCOMPARE(cache.value(Delta::MediaIdentity(2, 12)), QByteArray("alternate attachment"));
		QVERIFY(Delta::MediaIdentity(1, INT32_MAX) != Delta::MediaIdentity(2, 1));
		QVERIFY(Delta::MediaIdentity(INT32_MAX, 1) != Delta::MediaIdentity(1, INT32_MAX));
		QVERIFY(Delta::MediaIdentity(INT32_MAX, INT32_MAX) <= INT64_MAX);
		QCOMPARE(Delta::MediaIdentity(0, 12), std::uint64_t(0));
		QCOMPARE(Delta::MediaIdentity(1, -1), std::uint64_t(0));
	}

	void realCoreHistoryDeletion() {
		const auto program = qEnvironmentVariable("DELTA_TEST_CORE_RPC");
		if (program.isEmpty()) QSKIP("Set DELTA_TEST_CORE_RPC to test the real Core server.");
		QTemporaryDir temporary;
		QVERIFY(temporary.isValid());
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(program, temporary.filePath(u"accounts"_s));
		const auto account = CoreCall(*rpc, u"add_account"_s, {})[u"result"_s].toInt();
		QVERIFY(account > 0);
		const auto message = CoreCall(*rpc, u"add_device_message"_s,
			{account, u"history-test"_s, QJsonObject{{u"text"_s, u"Delete only this fixture"_s}}})[u"result"_s].toInt();
		QVERIFY(message > 0);
		const auto original = CoreCall(*rpc, u"get_message"_s, {account, message})
			[u"result"_s].toObject();
		const auto chat = original[u"chatId"_s].toInt();
		const auto timestamp = original[u"timestamp"_s].toInteger();
		QVERIFY(chat > 0);
		Delta::AccountScope scope;
		const auto selected = CoreCall(*rpc, u"add_device_message"_s,
			{account, u"selected-delete"_s, QJsonObject{{u"text"_s, u"Selected deletion fixture"_s}}})[u"result"_s].toInt();
		QVERIFY(selected > 0);
		auto deleted = false;
		Delta::DeleteCoreMessages(rpc, scope, account, QJsonArray{selected}, false,
			[&](QString error) {
				QVERIFY(error.isEmpty());
				deleted = true;
			});
		QTRY_VERIFY(deleted);
		const auto remaining = CoreCall(*rpc, u"get_message_ids"_s,
			{account, chat, false, false})[u"result"_s].toArray();
		QVERIFY(!remaining.contains(selected));
		QVERIFY(remaining.contains(message));
		auto cleared = false;
		Delta::DeleteChatHistory(rpc, scope, account, chat, false, false,
			[&](Delta::HistoryDeletionResult result) {
				QVERIFY(result.error.isEmpty());
				QVERIFY(result.deletedIds.contains(message));
				QVERIFY(!result.chatDeleted);
				cleared = true;
			}, Delta::HistoryDateRange{timestamp - 1, timestamp + 1});
		QTRY_VERIFY(cleared);
		const auto messages = CoreCall(*rpc, u"get_message_ids"_s, {account, chat, false, false});
		QVERIFY(!messages.contains(u"error"_s));
		QVERIFY(!messages[u"result"_s].toArray().contains(message));
		cleared = false;
		Delta::DeleteChatHistory(rpc, scope, account, chat, false, false,
			[&](Delta::HistoryDeletionResult result) {
				QVERIFY(result.error.isEmpty());
				cleared = true;
			});
		QTRY_VERIFY(cleared);
		QVERIFY(CoreCall(*rpc, u"get_message_ids"_s, {account, chat, false, false})
			[u"result"_s].toArray().empty());
		const auto group = CoreCall(*rpc, u"create_group_chat"_s,
			{account, u"Disposable history test"_s, false})[u"result"_s].toInt();
		QVERIFY(group > 0);
		auto removed = false;
		Delta::DeleteChatHistory(rpc, scope, account, group, true, false,
			[&](Delta::HistoryDeletionResult result) {
				QVERIFY(result.error.isEmpty());
				QVERIFY(result.chatDeleted);
				removed = true;
			});
		QTRY_VERIFY(removed);
		QVERIFY(CoreCall(*rpc, u"get_full_chat_by_id"_s, {account, group}).contains(u"error"_s));
		rpc->stop();
	}

	void historyDateDeletionUsesTimestampsAndBatches() {
		for (const auto mode : {u"normal"_s, u"history-large"_s, u"history-bad-message"_s}) {
			auto rpc = std::make_shared<DeltaRpc>();
			rpc->start(u"" RPC_FIXTURE_PATH ""_s, mode);
			Delta::AccountScope scope;
			auto done = false;
			const auto large = mode == u"history-large"_s;
			Delta::DeleteChatHistory(rpc, scope, 1, 10, false, false,
				[&](Delta::HistoryDeletionResult result) {
					if (mode == u"history-bad-message"_s) {
						QVERIFY(!result.error.isEmpty());
						QVERIFY(result.deletedIds.empty());
					} else {
						QVERIFY(result.error.isEmpty());
						QCOMPARE(result.deletedIds, large ? QVector<int>({101, 102, 103, 104}) : QVector<int>({12}));
					}
					QVERIFY(!result.chatDeleted);
					done = true;
				}, large ? Delta::HistoryDateRange{100, 105} : Delta::HistoryDateRange{150, 250});
			QTRY_VERIFY(done);
			const auto state = CoreCall(*rpc, u"history_state"_s, {})[u"result"_s].toObject();
			if (mode == u"history-bad-message"_s) {
				QVERIFY(!state[u"calls"_s].toArray().contains(u"delete_messages"_s));
			} else if (large) {
				auto batches = 0;
				for (const auto &call : state[u"calls"_s].toArray()) {
					if (call == u"get_messages"_s) ++batches;
				}
				QCOMPARE(batches, 3);
			}
			rpc->stop();
		}
	}

	void clearsOnlyConfirmedSnapshot() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"history-new-message"_s);
		Delta::AccountScope scope;
		auto done = false;
		Delta::DeleteChatHistory(rpc, scope, 1, 10, false, false,
			[&](Delta::HistoryDeletionResult result) {
				QVERIFY(result.error.isEmpty());
				QVERIFY(!result.chatDeleted);
				QCOMPARE(result.deletedIds, QVector<int>({10, 80, 12}));
				done = true;
			});
		QTRY_VERIFY(done);
		const auto state = CoreCall(*rpc, u"history_state"_s, {})[u"result"_s].toObject();
		QCOMPARE(state[u"ids"_s].toArray(), QJsonArray{999});
		QCOMPARE(state[u"calls"_s].toArray(), (QJsonArray{u"get_message_ids"_s, u"delete_messages"_s}));
		rpc->stop();
	}

	void rejectedHistoryDeletionStopsBeforeRemovingChat() {
		for (const auto mode : {u"history-reject-list"_s, u"history-reject-delete"_s}) {
			auto rpc = std::make_shared<DeltaRpc>();
			rpc->start(u"" RPC_FIXTURE_PATH ""_s, mode);
			Delta::AccountScope scope;
			auto done = false;
			Delta::DeleteChatHistory(rpc, scope, 1, 10, true, true,
				[&](Delta::HistoryDeletionResult result) {
					QCOMPARE(result.error, u"deletion rejected"_s);
					QVERIFY(result.deletedIds.empty());
					QVERIFY(!result.chatDeleted);
					done = true;
				});
			QTRY_VERIFY(done);
			const auto state = CoreCall(*rpc, u"history_state"_s, {})[u"result"_s].toObject();
			QCOMPARE(state[u"ids"_s].toArray(), (QJsonArray{10, 80, 12}));
			QVERIFY(!state[u"calls"_s].toArray().contains(u"delete_chat"_s));
			rpc->stop();
		}
	}

	void reportsPartialHistoryDeletion() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"history-reject-chat"_s);
		Delta::AccountScope scope;
		auto done = false;
		Delta::DeleteChatHistory(rpc, scope, 1, 10, true, true,
			[&](Delta::HistoryDeletionResult result) {
				QCOMPARE(result.error, u"deletion rejected"_s);
				QVERIFY(!result.chatDeleted);
				QCOMPARE(result.deletedIds, QVector<int>({10, 80, 12}));
				done = true;
			});
		QTRY_VERIFY(done);
		rpc->stop();
	}

	void deletesChatWithoutRevokingMessages() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		Delta::AccountScope scope;
		auto done = false;
		Delta::DeleteChatHistory(rpc, scope, 1, 10, true, false,
			[&](Delta::HistoryDeletionResult result) {
				QVERIFY(result.error.isEmpty());
				QVERIFY(result.chatDeleted);
				done = true;
			});
		QTRY_VERIFY(done);
		const auto state = CoreCall(*rpc, u"history_state"_s, {})[u"result"_s].toObject();
		QCOMPARE(state[u"calls"_s].toArray(), (QJsonArray{u"delete_chat"_s}));
		rpc->stop();
	}

	void accountSwitchCancelsRemainingDeletionSteps() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"history-hold-delete"_s);
		Delta::AccountScope scope;
		auto done = false;
		Delta::DeleteChatHistory(rpc, scope, 1, 10, true, true,
			[&](Delta::HistoryDeletionResult) { done = true; });
		QTRY_VERIFY(CoreCall(*rpc, u"history_state"_s, {})[u"result"_s].toObject()
			[u"calls"_s].toArray().contains(u"delete_messages_for_all"_s));
		scope.invalidate();
		CoreCall(*rpc, u"release"_s, {});
		const auto state = CoreCall(*rpc, u"history_state"_s, {})[u"result"_s].toObject();
		QVERIFY(!state[u"calls"_s].toArray().contains(u"delete_chat"_s));
		QVERIFY(!done);
		rpc->stop();
	}

	void realCoreDraftRoundTrip() {
		const auto program = qEnvironmentVariable("DELTA_TEST_CORE_RPC");
		if (program.isEmpty()) QSKIP("Set DELTA_TEST_CORE_RPC to test the real Core server.");
		QTemporaryDir temporary;
		QVERIFY(temporary.isValid());
		auto attachment = QFile(temporary.filePath(u"attachment.txt"_s));
		QVERIFY(attachment.open(QIODevice::WriteOnly));
		attachment.write("draft attachment integrity\n");
		attachment.close();
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(program, temporary.filePath(u"accounts"_s));
		const auto first = CoreCall(*rpc, u"add_account"_s, {})[u"result"_s].toInt();
		const auto second = CoreCall(*rpc, u"add_account"_s, {})[u"result"_s].toInt();
		QVERIFY(first > 0 && second > 0 && first != second);
		auto inventory = std::optional<Delta::AccountInventory>();
		Delta::ReadAccountInventory([=](QString method, QJsonArray params, DeltaRpc::Callback done) {
			rpc->call(method, params, std::move(done));
		}, [&](Delta::AccountInventory result) { inventory = std::move(result); });
		QTRY_VERIFY(inventory.has_value());
		QVERIFY(inventory->error.isEmpty());
		QVERIFY(inventory->configured.empty());
		QCOMPARE(inventory->unconfigured, first);
		const auto chatA = CoreCall(*rpc, u"create_chat_by_contact_id"_s, {first, 1})[u"result"_s].toInt();
		const auto chatB = CoreCall(*rpc, u"create_chat_by_contact_id"_s, {second, 1})[u"result"_s].toInt();
		QVERIFY(chatA > 0);
		QCOMPARE(chatA, chatB);
		QVERIFY(!CoreCall(*rpc, u"misc_set_draft"_s,
			{first, chatA, u"first original"_s, attachment.fileName(), u"attachment.txt"_s, QJsonValue(), u"File"_s}).contains(u"error"_s));
		QVERIFY(!CoreCall(*rpc, u"misc_set_draft"_s,
			{second, chatB, u"second original"_s, QJsonValue(), QJsonValue(), QJsonValue(), QJsonValue()}).contains(u"error"_s));
		const auto items = CoreCall(*rpc, u"get_chatlist_items_by_entries"_s,
			{first, QJsonArray{chatA}})[u"result"_s].toObject();
		const auto preview = items[QString::number(chatA)].toObject()[u"lastMessageId"_s].toInt();
		QVERIFY(preview > 0);
		QCOMPARE(CoreCall(*rpc, u"get_message"_s, {first, preview})
			[u"result"_s].toObject()[u"state"_s].toInt(), 19);
		const auto history = CoreCall(*rpc, u"get_message_ids"_s, {first, chatA, false, false})[u"result"_s].toArray();
		QVERIFY(!history.contains(preview));
		Delta::AccountScope visibilityScope;
		auto checked = false;
		Delta::CheckMessageVisibility(rpc, visibilityScope, first, chatA, preview,
			[&](bool visible) {
				QVERIFY(!visible);
				checked = true;
			});
		QTRY_VERIFY(checked);
		auto errors = 0;
		Delta::DraftStore drafts(rpc, [&](QString) { ++errors; });
		auto loaded = 0;
		drafts.load(first, chatA, [&](Delta::ComposeDraft draft, QString error) {
			QVERIFY(error.isEmpty());
			QCOMPARE(draft.text, u"first original"_s);
			QVERIFY(QFileInfo::exists(draft.file));
			++loaded;
		});
		drafts.load(second, chatB, [&](Delta::ComposeDraft draft, QString error) {
			QVERIFY(error.isEmpty());
			QCOMPARE(draft.text, u"second original"_s);
			++loaded;
		});
		QTRY_COMPARE(loaded, 2);
		drafts.save(first, chatA, u"changed in first"_s, 0);
		auto saved = QJsonObject();
		QTRY_VERIFY(([&] {
			saved = CoreCall(*rpc, u"get_draft"_s, {first, chatA})[u"result"_s].toObject();
			return saved[u"text"_s].toString() == u"changed in first"_s;
		})());
		QCOMPARE(saved[u"fileName"_s].toString(), u"attachment.txt"_s);
		auto persisted = QFile(saved[u"file"_s].toString());
		QVERIFY(persisted.open(QIODevice::ReadOnly));
		QCOMPARE(persisted.readAll(), QByteArray("draft attachment integrity\n"));
		QCOMPARE(CoreCall(*rpc, u"get_draft"_s, {second, chatB})[u"result"_s].toObject()[u"text"_s].toString(), u"second original"_s);
		drafts.recoverText(first, chatA, u"rejected after switching"_s, 0);
		QTRY_VERIFY(([&] {
			saved = CoreCall(*rpc, u"get_draft"_s, {first, chatA})[u"result"_s].toObject();
			return saved[u"text"_s].toString()
				== u"rejected after switching\n\nchanged in first"_s;
		})());
		QCOMPARE(saved[u"fileName"_s].toString(), u"attachment.txt"_s);
		QCOMPARE(CoreCall(*rpc, u"get_draft"_s, {second, chatB})[u"result"_s].toObject()[u"text"_s].toString(), u"second original"_s);
		drafts.save(second, chatB, {}, 0);
		QTRY_VERIFY(CoreCall(*rpc, u"get_draft"_s, {second, chatB})[u"result"_s].isNull());
		QCOMPARE(errors, 0);
		rpc->stop();
	}

	void deliversEventsAndStopsPendingRequests() {
		DeltaRpc rpc;
		auto events = 0;
		auto failed = 0;
		rpc.setEventHandler([&](QJsonObject event) {
			QCOMPARE(event[u"method"_s].toString(), u"fixture_event"_s);
			++events;
		});
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		rpc.call(u"get_next_event"_s, {}, [&](QJsonObject reply) {
			if (reply.contains(u"error"_s)) ++failed;
		});
		rpc.call(u"event"_s, {}, nullptr);
		QTRY_COMPARE(events, 1);
		rpc.stop();
		QCOMPARE(failed, 1);
		QCOMPARE(events, 1);
	}

	void clearsTextDraftInCore() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"plain"_s);
		Delta::DraftStore drafts(rpc, [](QString) { QFAIL("Unexpected draft failure"); });
		auto loaded = false;
		drafts.load(1, 10, [&](Delta::ComposeDraft, QString) { loaded = true; });
		QTRY_VERIFY(loaded);
		drafts.save(1, 10, {}, 0);
		auto checked = false;
		rpc->call(u"inspect_draft"_s, {1, 10}, [&](QJsonObject reply) {
			QVERIFY(reply[u"result"_s].toObject().isEmpty());
			checked = true;
		});
		QTRY_VERIFY(checked);
		rpc->stop();
	}

	void historyUsesCoreOrder() {
		const auto ids = std::vector<int>{10, 80, 12, 65, 20};
		QCOMPARE(*Delta::HistoryPage(ids, 0, 0, 2), (std::vector<int>{20, 65}));
		QCOMPARE(*Delta::HistoryPage(ids, 65, 0, 2), (std::vector<int>{12, 80}));
		QCOMPARE(*Delta::HistoryPage(ids, 80, -3, 2), (std::vector<int>{65, 12}));
		QCOMPARE(*Delta::HistoryPage(ids, 20, -3, 2), (std::vector<int>{}));
		QCOMPARE(*Delta::HistoryPage(ids, 10, 0, 2), (std::vector<int>{}));
		QVERIFY(!Delta::HistoryPage(ids, 999, 0, 2));
	}

	void startupFailureCompletesPendingRequests() {
		DeltaRpc rpc;
		auto failed = false;
		auto events = 0;
		rpc.setEventHandler([&](QJsonObject) { ++events; });
		rpc.start(u"/nonexistent/delta-rpc-fixture"_s, u"normal"_s);
		rpc.call(u"echo"_s, {}, [&](QJsonObject reply) { failed = reply.contains(u"error"_s); });
		QTRY_VERIFY(failed);
		QCOMPARE(events, 1);
		QVERIFY(!rpc.running());
	}

	void incompleteEnvelopeFailsClosed() {
		DeltaRpc rpc;
		auto failed = false;
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		rpc.call(u"incomplete"_s, {}, [&](QJsonObject reply) { failed = reply.contains(u"error"_s); });
		QTRY_VERIFY(failed);
		QVERIFY(!rpc.running());
		rpc.stop();
	}

	void routesOutOfOrderReplies() {
		DeltaRpc rpc;
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		auto completed = QStringList();
		rpc.call(u"hold"_s, {}, [&](QJsonObject) { completed.push_back(u"first"_s); });
		rpc.call(u"echo"_s, {}, [&](QJsonObject) { completed.push_back(u"second"_s); });
		QTRY_COMPARE(completed, QStringList{u"second"_s});
		rpc.call(u"release"_s, {}, nullptr);
		QTRY_COMPARE(completed.size(), 2);
		QCOMPARE(completed.back(), u"first"_s);
		rpc.stop();
	}

	void accountSwitchDropsStaleReplies() {
		DeltaRpc rpc;
		Delta::AccountScope account;
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		auto stale = false;
		auto current = false;
		rpc.call(u"hold"_s, {}, account.bind([&](QJsonObject) { stale = true; }));
		account.invalidate();
		rpc.call(u"release"_s, {}, account.bind([&](QJsonObject) { current = true; }));
		QTRY_VERIFY(current);
		QVERIFY(!stale);
		rpc.stop();
	}

	void crashFailsPendingOnce() {
		DeltaRpc rpc;
		auto failures = 0;
		auto events = 0;
		rpc.setEventHandler([&](QJsonObject reply) { if (reply.contains(u"error"_s)) ++events; });
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		rpc.call(u"get_next_event"_s, {}, [&](QJsonObject reply) {
			if (reply.contains(u"error"_s)) ++failures;
		});
		rpc.call(u"crash"_s, {}, [&](QJsonObject reply) {
			if (reply.contains(u"error"_s)) ++failures;
		});
		QTRY_COMPARE(failures, 2);
		QCOMPARE(events, 1);
		QVERIFY(!rpc.running());
		rpc.stop();
		QCOMPARE(failures, 2);
	}

	void malformedResponseFailsClosed() {
		DeltaRpc rpc;
		auto failures = 0;
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		rpc.call(u"malformed"_s, {}, [&](QJsonObject reply) {
			if (reply.contains(u"error"_s)) ++failures;
		});
		QTRY_COMPARE(failures, 1);
		QVERIFY(!rpc.running());
		rpc.call(u"echo"_s, {}, [&](QJsonObject reply) {
			if (reply.contains(u"error"_s)) ++failures;
		});
		QCOMPARE(failures, 2);
		rpc.stop();
	}

	void timeoutDoesNotRetryMutation() {
		DeltaRpc rpc;
		auto failures = 0;
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		rpc.call(u"hang"_s, {}, [&](QJsonObject reply) {
			if (reply.contains(u"error"_s)) ++failures;
		}, 100);
		QTRY_COMPARE(failures, 1);
		QVERIFY(!rpc.running());
		QTest::qWait(150);
		QCOMPARE(failures, 1);
		rpc.stop();
	}

	void restartDiscardsPartialFrame() {
		DeltaRpc rpc;
		auto failed = false;
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		rpc.call(u"partial"_s, {}, [&](QJsonObject reply) { failed = reply.contains(u"error"_s); });
		QTRY_VERIFY(failed);
		rpc.start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		auto ok = false;
		rpc.call(u"echo"_s, {42}, [&](QJsonObject reply) { ok = reply[u"result"_s].toArray() == QJsonArray{42}; });
		QTRY_VERIFY(ok);
		rpc.stop();
	}

	void draftsAreAccountScopedAndPreserveAttachments() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"normal"_s);
		auto errors = 0;
		Delta::DraftStore drafts(rpc, [&](QString) { ++errors; });
		auto loaded = 0;
		drafts.load(1, 10, [&](Delta::ComposeDraft draft, QString error) {
			QVERIFY(error.isEmpty()); QCOMPARE(draft.text, u"account-1"_s); ++loaded;
		});
		drafts.load(2, 10, [&](Delta::ComposeDraft draft, QString error) {
			QVERIFY(error.isEmpty()); QCOMPARE(draft.text, u"account-2"_s); ++loaded;
		});
		QTRY_COMPARE(loaded, 2);
		drafts.save(1, 10, u"private draft"_s, 19);
		auto checked = false;
		rpc->call(u"inspect_draft"_s, {1, 10}, [&](QJsonObject reply) {
			const auto draft = reply[u"result"_s].toObject();
			QCOMPARE(draft[u"text"_s].toString(), u"private draft"_s);
			QCOMPARE(draft[u"file"_s].toString(), u"/fixture/attachment.bin"_s);
			QCOMPARE(draft[u"quote"_s].toObject()[u"messageId"_s].toInt(), 19);
			checked = true;
		});
		QTRY_VERIFY(checked);
		checked = false;
		rpc->call(u"inspect_draft"_s, {2, 10}, [&](QJsonObject reply) {
			QCOMPARE(reply[u"result"_s].toObject()[u"text"_s].toString(), u"account-2"_s);
			checked = true;
		});
		QTRY_VERIFY(checked);
		QCOMPARE(errors, 0);
		rpc->stop();
	}

	void serializesDraftWrites() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"hold-first-save"_s);
		Delta::DraftStore drafts(rpc, [](QString) {});
		auto loaded = false;
		drafts.load(1, 10, [&](Delta::ComposeDraft, QString) { loaded = true; });
		QTRY_VERIFY(loaded);
		drafts.save(1, 10, u"first"_s, 0);
		drafts.save(1, 10, u"second"_s, 0);
		drafts.save(1, 10, u"latest"_s, 0);
		auto writes = -1;
		rpc->call(u"writes"_s, {}, [&](QJsonObject reply) { writes = reply[u"result"_s].toInt(); });
		QTRY_COMPARE(writes, 1);
		rpc->call(u"release"_s, {}, nullptr);
		auto latest = false;
		QTRY_VERIFY_WITH_TIMEOUT(([&] {
			if (!latest) rpc->call(u"inspect_draft"_s, {1, 10}, [&](QJsonObject reply) {
				latest = reply[u"result"_s].toObject()[u"text"_s].toString() == u"latest"_s;
			});
			return latest;
		})(), 3000);
		rpc->stop();
	}

	void failedDraftWriteIsVisibleWithoutAutomaticRetry() {
		auto rpc = std::make_shared<DeltaRpc>();
		rpc->start(u"" RPC_FIXTURE_PATH ""_s, u"reject"_s);
		auto errors = 0;
		Delta::DraftStore drafts(rpc, [&](QString error) { QCOMPARE(error, u"write rejected"_s); ++errors; });
		drafts.save(1, 10, u"retain me"_s, 0);
		QTRY_COMPARE(errors, 1);
		QTest::qWait(100);
		QCOMPARE(errors, 1);
		rpc->stop();
	}
};

QTEST_GUILESS_MAIN(IntegrityTests)
#include "integrity_tests.moc"
