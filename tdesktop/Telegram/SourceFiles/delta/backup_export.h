#pragma once

#include "account_inventory.h"
#include "account_scope.h"

#include <map>
#include <QtCore/QDir>
#include <QtCore/QFileInfo>
#include <QtCore/QFile>
#include <QtCore/QTemporaryDir>

namespace Delta {

// Every attempt owns a new directory. Core's filename selection is then
// isolated from other processes exporting the same account fingerprint.
[[nodiscard]] inline QString CreateBackupExportDirectory(const QString &parent) {
	if (parent.isEmpty() || !QFileInfo(parent).isDir()) return {};
	auto directory = QTemporaryDir(QDir(parent).filePath(
		QStringLiteral("chatmail-backup-XXXXXX")));
	if (!directory.isValid()) return {};
#if defined(Q_OS_UNIX)
	const auto mode = QFileInfo(directory.path()).permissions();
	const auto owner = QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner;
	const auto shared = QFileDevice::ReadGroup | QFileDevice::WriteGroup | QFileDevice::ExeGroup
		| QFileDevice::ReadOther | QFileDevice::WriteOther | QFileDevice::ExeOther;
	if ((mode & owner) != owner || (mode & shared)) return {};
#endif
	directory.setAutoRemove(false);
	return directory.path();
}

struct BackupExportState {
	bool busy = false;
};

// Keep one export running on each RPC, including across profile switches.
// Closing a dialog must not authorize another in-flight export.
[[nodiscard]] inline auto &BackupExportRegistry() {
	static auto states = std::map<const void*, std::weak_ptr<BackupExportState>>();
	return states;
}

[[nodiscard]] inline bool AnyBackupExportInProgress() {
	for (const auto &[rpc, weak] : BackupExportRegistry()) {
		if (const auto state = weak.lock(); state && state->busy) return true;
	}
	return false;
}

[[nodiscard]] inline std::shared_ptr<BackupExportState> BackupExportStateFor(const void *rpc) {
	auto &states = BackupExportRegistry();
	for (auto i = states.begin(); i != states.end(); ) {
		if (i->second.expired()) i = states.erase(i); else ++i;
	}
	auto &weak = states[rpc];
	if (const auto state = weak.lock()) return state;
	auto state = std::make_shared<BackupExportState>();
	weak = state;
	return state;
}

struct BackupExportResult {
	bool confirmed = false;
	QString error;
};

inline void ExportBackupFile(
		AccountInventoryRequest request,
		std::shared_ptr<BackupExportState> state,
		const AccountScope &scope,
		int accountId,
		const QString &destination,
		const QString &passphrase,
		std::function<void(BackupExportResult)> done) {
	if (!request || !state || accountId <= 0 || destination.isEmpty()) {
		if (done) done({ false, QStringLiteral("Backup export is not available.") });
		return;
	}
	if (state->busy) {
		if (done) done({ false, QStringLiteral("A backup export is already in progress.") });
		return;
	}
	state->busy = true;
	const auto current = scope.guard();
	request(QStringLiteral("export_backup"),
		{ accountId, destination, passphrase.isEmpty()
			? QJsonValue() : QJsonValue(passphrase) },
		[state = std::move(state), current, done = std::move(done)](QJsonObject reply) {
			state->busy = false;
			if (!current() || !done) return;
			if (!reply.contains(QStringLiteral("error"))
				&& reply.contains(QStringLiteral("result"))
				&& reply.value(QStringLiteral("result")).isNull()) {
				done({ true, {} });
				return;
			}
			auto error = reply.value(QStringLiteral("error"));
			const auto text = error.isString() ? error.toString()
				: error.toObject().value(QStringLiteral("message")).toString();
			done({ false, text.isEmpty()
				? QStringLiteral("Backup export was not confirmed. Check the destination folder before retrying.")
				: text });
		});
}

} // namespace Delta
