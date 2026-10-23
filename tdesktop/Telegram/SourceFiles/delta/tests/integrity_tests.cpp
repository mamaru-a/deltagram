#include "delta_rpc.h"
#include "draft_store.h"
#include "account_scope.h"
#include "message_order.h"

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
		const auto chatA = CoreCall(*rpc, u"create_chat_by_contact_id"_s, {first, 1})[u"result"_s].toInt();
		const auto chatB = CoreCall(*rpc, u"create_chat_by_contact_id"_s, {second, 1})[u"result"_s].toInt();
		QVERIFY(chatA > 0);
		QCOMPARE(chatA, chatB);
		QVERIFY(!CoreCall(*rpc, u"misc_set_draft"_s,
			{first, chatA, u"first original"_s, attachment.fileName(), u"attachment.txt"_s, QJsonValue(), u"File"_s}).contains(u"error"_s));
		QVERIFY(!CoreCall(*rpc, u"misc_set_draft"_s,
			{second, chatB, u"second original"_s, QJsonValue(), QJsonValue(), QJsonValue(), QJsonValue()}).contains(u"error"_s));
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
