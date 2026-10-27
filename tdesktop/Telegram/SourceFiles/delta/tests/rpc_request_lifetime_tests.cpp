#include "backup_export.h"
#include "delta_rpc.h"

#include <QtCore/QCoreApplication>
#include <QtCore/QDir>
#include <QtCore/QEventLoop>
#include <QtCore/QFile>
#include <QtCore/QJsonDocument>
#include <QtCore/QPointer>
#include <QtCore/QTemporaryDir>
#include <QtCore/QThread>
#include <QtCore/QTimer>
#include <QtTest/QtTest>

#include <iostream>

namespace {

constexpr auto kChild = "DELTA_RPC_REQUEST_LIFETIME_TEST_CHILD";

bool Touch(const QString &path) {
	QFile file(path);
	return file.open(QIODevice::WriteOnly);
}

int RunChild() {
	const auto root = qEnvironmentVariable("DC_ACCOUNTS_PATH");
	const auto mode = qEnvironmentVariable(kChild);
	for (std::string line; std::getline(std::cin, line);) {
		const auto request = QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
		if (!Touch(root + "/started")) return 20;
		for (auto i = 0; !QFileInfo::exists(root + "/release"); ++i) {
			if (i == 500) return 21;
			QThread::msleep(10);
		}
		if (mode == "crash") return 22;
		if (!Touch(root + "/completed")) return 23;
		if (mode == "malformed") {
			std::cout << "{invalid" << std::endl;
			continue;
		}
		auto reply = QJsonObject{ { "id", request.value("id") } };
		if (mode == "error") {
			reply.insert("error", QJsonObject{ { "message", "fixture rejection" } });
		} else {
			reply.insert("result", QJsonValue());
		}
		std::cout << QJsonDocument(reply).toJson(QJsonDocument::Compact).constData() << std::endl;
	}
	return 0;
}

std::shared_ptr<DeltaRpc> StartRpc(const QString &root, const QByteArray &mode) {
	const auto previous = qgetenv(kChild);
	const auto wasSet = qEnvironmentVariableIsSet(kChild);
	qputenv(kChild, mode);
	auto rpc = std::make_shared<DeltaRpc>();
	rpc->start(QCoreApplication::applicationFilePath(), root);
	if (wasSet) qputenv(kChild, previous); else qunsetenv(kChild);
	return rpc;
}

} // namespace

class RpcRequestLifetimeTests final : public QObject {
	Q_OBJECT
private slots:
	void exportSurvivesOwnerLoss_data() {
		QTest::addColumn<QByteArray>("mode");
		for (const auto mode : { "success", "error", "crash", "malformed" }) {
			QTest::newRow(mode) << QByteArray(mode);
		}
	}
	void exportSurvivesOwnerLoss() {
		QFETCH(QByteArray, mode);
		QTemporaryDir root;
		QVERIFY(root.isValid());
		auto rpc = StartRpc(root.path(), mode);
		const auto weak = std::weak_ptr<DeltaRpc>(rpc);
		const auto pointer = QPointer<DeltaRpc>(rpc.get());
		auto state = Delta::BackupExportStateFor(rpc.get());
		Delta::AccountScope scope;
		auto displayed = false;
		Delta::ExportBackupFile(
			[rpc](QString method, QJsonArray args, DeltaRpc::Callback done) {
				CallOwnedDeltaRpc(rpc, method, args, std::move(done));
			}, state, scope, 5, root.path(), {}, [&](auto) { displayed = true; });
		QTRY_VERIFY(QFileInfo::exists(root.filePath("started")));
		scope.invalidate();
		rpc.reset();
		QVERIFY(!weak.expired());
		QVERIFY(state->busy);
		QVERIFY(Delta::AnyBackupExportInProgress());
		QVERIFY(Touch(root.filePath("release")));
		QTRY_VERIFY(!state->busy);
		QTRY_VERIFY(weak.expired());
		QTRY_VERIFY(pointer.isNull());
		QVERIFY(!displayed);
		QVERIFY(!Delta::AnyBackupExportInProgress());
		QCOMPARE(QFileInfo::exists(root.filePath("completed")), mode != "crash");
	}
	void callbackCannotDestroyRpcInsideReadHandler() {
		QTemporaryDir root;
		auto rpc = StartRpc(root.path(), "success");
		const auto pointer = QPointer<DeltaRpc>(rpc.get());
		const auto weak = std::weak_ptr<DeltaRpc>(rpc);
		auto replied = false;
		auto aliveAfterNestedLoop = false;
		CallOwnedDeltaRpc(rpc, "fixture", {}, [&](QJsonObject reply) {
			replied = reply.contains("result");
			QEventLoop nested;
			QTimer::singleShot(10, &nested, &QEventLoop::quit);
			nested.exec();
			aliveAfterNestedLoop = !weak.expired() && pointer;
		});
		QTRY_VERIFY(QFileInfo::exists(root.filePath("started")));
		rpc.reset();
		QVERIFY(Touch(root.filePath("release")));
		QTRY_VERIFY(replied);
		QVERIFY(aliveAfterNestedLoop);
		QTRY_VERIFY(pointer.isNull());
		QVERIFY(weak.expired());
	}
	void explicitStopCompletesAndReleasesOwner() {
		QTemporaryDir root;
		auto rpc = StartRpc(root.path(), "success");
		const auto pointer = QPointer<DeltaRpc>(rpc.get());
		auto replied = false;
		CallOwnedDeltaRpc(rpc, "export_backup", {}, [&](QJsonObject reply) {
			replied = reply.contains("error");
		});
		QTRY_VERIFY(QFileInfo::exists(root.filePath("started")));
		rpc.reset();
		QVERIFY(pointer);
		pointer->stop();
		QVERIFY(replied);
		QVERIFY(pointer); // Not destroyed while stop() is unwinding.
		QTRY_VERIFY(pointer.isNull());
		QVERIFY(!QFileInfo::exists(root.filePath("completed")));
	}
	void parentedRpcIsRejectedWithoutStartingRequest() {
		QObject parent;
		auto rpc = std::make_shared<DeltaRpc>(&parent);
		auto rejected = false;
		CallOwnedDeltaRpc(rpc, "fixture", {}, [&](QJsonObject reply) {
			rejected = reply.contains("error");
		});
		QVERIFY(rejected);
		QVERIFY(!rpc->running());
		rpc.reset();
		QVERIFY(parent.children().isEmpty());
	}
	void stoppedAndNullRpcCompleteWithoutLeak() {
		auto rpc = std::make_shared<DeltaRpc>();
		const auto pointer = QPointer<DeltaRpc>(rpc.get());
		auto errors = 0;
		const auto done = [&](QJsonObject reply) { if (reply.contains("error")) ++errors; };
		CallOwnedDeltaRpc(rpc, "fixture", {}, done);
		rpc.reset();
		CallOwnedDeltaRpc({}, "fixture", {}, done);
		QCOMPARE(errors, 2);
		QTRY_VERIFY(pointer.isNull());
	}
};

int main(int argc, char **argv) {
	QCoreApplication app(argc, argv);
	if (qEnvironmentVariableIsSet(kChild)) return RunChild();
	RpcRequestLifetimeTests tests;
	return QTest::qExec(&tests, argc, argv);
}

#include "rpc_request_lifetime_tests.moc"
