#include "../../main/account_backend.h"
#include "../../main/account_backend_store.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QFileDevice>
#include <QtCore/QFileInfo>
#include <QtCore/QTemporaryDir>
#include <QtTest/QtTest>

using Main::AccountBackend;
using Main::AccountBackendBindResult;
using Main::AccountBackendBindState;
using Main::AccountBackendMigrationEvidence;
using Main::AccountBackendMigrationNeed;
using Main::AccountBackendRecordKind;
using Main::AccountBackendStoreReadStatus;
using Main::AccountBackendStoreWriteStatus;

namespace {

AccountBackendMigrationEvidence Fresh() {
	AccountBackendMigrationEvidence evidence;
	evidence.record = AccountBackendRecordKind::FreshMissing;
	return evidence;
}

AccountBackendMigrationEvidence Legacy(
		bool tagPresent,
		quint32 tag,
		bool keys,
		bool coreMarker) {
	AccountBackendMigrationEvidence evidence;
	evidence.record = AccountBackendRecordKind::LegacyExisting;
	evidence.tagPresent = tagPresent;
	evidence.storedTag = tag;
	evidence.telegramAuthorizationKeysPresent = keys;
	evidence.provenLegacyCoreMarker = coreMarker;
	return evidence;
}

[[nodiscard]] QByteArray ReadAll(const QString &path) {
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly)) {
		return {};
	}
	return file.readAll();
}

[[nodiscard]] bool WriteAll(const QString &path, const QByteArray &bytes) {
	if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
		return false;
	}
	QFile file(path);
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		return false;
	}
	return file.write(bytes) == bytes.size() && file.flush();
}

[[nodiscard]] bool IsOwnerPrivate(const QString &path) {
	const auto perms = QFileInfo(path).permissions();
	if (!perms.testFlag(QFileDevice::ReadOwner)
		|| !perms.testFlag(QFileDevice::WriteOwner)) {
		return false;
	}
	const auto shared = QFileDevice::ReadGroup
		| QFileDevice::WriteGroup
		| QFileDevice::ExeGroup
		| QFileDevice::ReadOther
		| QFileDevice::WriteOther
		| QFileDevice::ExeOther;
	return !(perms & shared);
}

struct RestorePermissions {
	QString path;
	QFileDevice::Permissions permissions;
	~RestorePermissions() {
		if (!path.isEmpty()) {
			QFile::setPermissions(path, permissions);
		}
	}
};

struct StoreFixture {
	QTemporaryDir dir;
	QByteArray identity = QByteArrayLiteral("0123456789ABCDEF");
	QByteArray otherIdentity = QByteArrayLiteral("FEDCBA9876543210");
	QByteArray mapSentinel = QByteArrayLiteral("MAP-SENTINEL-BYTES");
	QByteArray authSentinel = QByteArrayLiteral("AUTH-SENTINEL-BYTES");

	[[nodiscard]] QString tdataRoot() const {
		return dir.path() + QStringLiteral("/tdata");
	}
	[[nodiscard]] QString pathFor(const QByteArray &id) const {
		return Main::AccountBackendStorePath(tdataRoot(), id);
	}
	[[nodiscard]] QString path() const {
		return pathFor(identity);
	}
	[[nodiscard]] QString mapPath() const {
		return tdataRoot()
			+ QChar('/')
			+ QString::fromLatin1(identity)
			+ QStringLiteral("/map0");
	}
	[[nodiscard]] QString authPath() const {
		return tdataRoot()
			+ QChar('/')
			+ QString::fromLatin1(identity)
			+ QChar('0');
	}

	bool writeSentinels() {
		return WriteAll(mapPath(), mapSentinel)
			&& WriteAll(authPath(), authSentinel);
	}
	[[nodiscard]] bool sentinelsUnchanged() const {
		return ReadAll(mapPath()) == mapSentinel
			&& ReadAll(authPath()) == authSentinel;
	}
};

} // namespace

class AccountBackendTests final : public QObject {
	Q_OBJECT

private slots:
	void tagValidation() {
		QVERIFY(Main::IsValidAccountBackendTag(0));
		QVERIFY(Main::IsValidAccountBackendTag(1));
		QVERIFY(Main::IsValidAccountBackendTag(2));
		QVERIFY(!Main::IsValidAccountBackendTag(3));
		QVERIFY(!Main::IsValidAccountBackendTag(0x1F));
		QVERIFY(!Main::IsValidAccountBackendTag(quint32(-1)));
		QCOMPARE(
			Main::EncodeAccountBackendTag(AccountBackend::Unspecified),
			quint32(0));
		QCOMPARE(
			Main::EncodeAccountBackendTag(AccountBackend::Core),
			quint32(1));
		QCOMPARE(
			Main::EncodeAccountBackendTag(AccountBackend::Telegram),
			quint32(2));
		QCOMPARE(
			Main::EncodeAccountBackendTag(AccountBackend(99)),
			quint32(0));
		const auto invalid = Main::InspectAccountBackendTag(99);
		QVERIFY(!invalid.valid);
		QCOMPARE(invalid.backend, AccountBackend::Unspecified);
		QCOMPARE(
			Main::DecodeAccountBackendTag(99),
			AccountBackend::Unspecified);
		QCOMPARE(
			Main::DecodeAccountBackendTag(2),
			AccountBackend::Telegram);
		QVERIFY(!Main::IsBoundAccountBackend(AccountBackend::Unspecified));
		QVERIFY(Main::IsBoundAccountBackend(AccountBackend::Telegram));
	}

	void freshMissingStaysUnspecifiedForChooser() {
		const auto migration = Main::ResolveStoredAccountBackend(Fresh());
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::Chooser);
		QVERIFY(!migration.needsProvenLegacyCoreMarker);
	}

	void legacyMissingTagWithoutEvidenceStaysUnspecified() {
		const auto migration = Main::ResolveStoredAccountBackend(
			Legacy(false, 0, false, false));
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::Chooser);
		QVERIFY(migration.needsProvenLegacyCoreMarker);
	}

	void legacyTelegramKeysStayTelegram() {
		const auto migration = Main::ResolveStoredAccountBackend(
			Legacy(false, 0, true, false));
		QCOMPARE(migration.backend, AccountBackend::Telegram);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::None);
		QVERIFY(!migration.needsProvenLegacyCoreMarker);
	}

	void provenCoreMarkerWithoutKeysIsCore() {
		QVERIFY(!Main::IsProvenLegacyCoreMarker({}));
		QVERIFY(!Main::IsProvenLegacyCoreMarker(QByteArrayLiteral("core")));
		QVERIFY(Main::IsProvenLegacyCoreMarker(
			QByteArray(Main::kAccountBackendLegacyCoreMarker)));
		const auto migration = Main::ResolveStoredAccountBackend(
			Legacy(false, 0, false, true));
		QCOMPARE(migration.backend, AccountBackend::Core);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::None);
	}

	void telegramKeysWinOverCoreMarker() {
		const auto migration = Main::ResolveStoredAccountBackend(
			Legacy(false, 0, true, true));
		QCOMPARE(migration.backend, AccountBackend::Telegram);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::None);
	}

	void explicitCoreTagWithoutKeysIsCore() {
		const auto migration = Main::ResolveStoredAccountBackend(
			Legacy(true, 1, false, false));
		QCOMPARE(migration.backend, AccountBackend::Core);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::None);
	}

	void explicitCoreTagWithTelegramKeysStaysUnspecified() {
		const auto migration = Main::ResolveStoredAccountBackend(
			Legacy(true, 1, true, false));
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::Chooser);
	}

	void explicitTelegramTagIsTelegramWithOrWithoutKeys() {
		const auto withKeys = Main::ResolveStoredAccountBackend(
			Legacy(true, 2, true, false));
		QCOMPARE(withKeys.backend, AccountBackend::Telegram);
		QCOMPARE(withKeys.need, AccountBackendMigrationNeed::None);
		const auto withoutKeys = Main::ResolveStoredAccountBackend(
			Legacy(true, 2, false, false));
		QCOMPARE(withoutKeys.backend, AccountBackend::Telegram);
		QCOMPARE(withoutKeys.need, AccountBackendMigrationNeed::None);
	}

	void explicitUnspecifiedWithKeysIsTelegram() {
		const auto migration = Main::ResolveStoredAccountBackend(
			Legacy(true, 0, true, false));
		QCOMPARE(migration.backend, AccountBackend::Telegram);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::None);
	}

	void invalidTagCannotAuthorizeTelegram() {
		const auto withKeys = Main::ResolveStoredAccountBackend(
			Legacy(true, 99, true, false));
		QCOMPARE(withKeys.backend, AccountBackend::Unspecified);
		QCOMPARE(withKeys.need, AccountBackendMigrationNeed::Chooser);
		const auto withoutKeys = Main::ResolveStoredAccountBackend(
			Legacy(true, 0x1F, false, true));
		QCOMPARE(withoutKeys.backend, AccountBackend::Unspecified);
		QCOMPARE(withoutKeys.need, AccountBackendMigrationNeed::Chooser);
	}

	void emptyKeysAreNotTelegramAndNotCore() {
		auto evidence = Legacy(false, 0, false, false);
		const auto migration = Main::ResolveStoredAccountBackend(evidence);
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QVERIFY(migration.needsProvenLegacyCoreMarker);
	}

	void migrationDoesNotGuessFromTelegramUserId() {
		const auto telegramUserId = quint64(1'000'000);
		Q_UNUSED(telegramUserId);
		auto evidence = Legacy(false, 0, false, false);
		const auto migration = Main::ResolveStoredAccountBackend(evidence);
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::Chooser);
	}

	void oneTimeBindFromUnspecified() {
		AccountBackendBindState state;
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Core),
			AccountBackendBindResult::Bound);
		QCOMPARE(state.backend, AccountBackend::Core);
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Core),
			AccountBackendBindResult::Unchanged);
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Telegram),
			AccountBackendBindResult::RejectedChange);
		QCOMPARE(state.backend, AccountBackend::Core);
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Unspecified),
			AccountBackendBindResult::RejectedClear);
		QCOMPARE(state.backend, AccountBackend::Core);
	}

	void oneTimeBindTelegramIdempotent() {
		AccountBackendBindState state;
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Telegram),
			AccountBackendBindResult::Bound);
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Telegram),
			AccountBackendBindResult::Unchanged);
		QCOMPARE(state.backend, AccountBackend::Telegram);
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Core),
			AccountBackendBindResult::RejectedChange);
		QCOMPARE(state.backend, AccountBackend::Telegram);
	}

	void bindUnspecifiedToUnspecifiedIsUnchanged() {
		AccountBackendBindState state;
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Unspecified),
			AccountBackendBindResult::Unchanged);
		QCOMPARE(state.backend, AccountBackend::Unspecified);
	}

	void bindRejectsInvalidRequested() {
		AccountBackendBindState state;
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend(99)),
			AccountBackendBindResult::RejectedInvalid);
		QCOMPARE(state.backend, AccountBackend::Unspecified);
	}

	void liveMtpDoesNotSilentlyRetag() {
		AccountBackendBindState state;
		state.mtpAlreadyExists = true;
		const auto result = Main::TryBindAccountBackend(
			state,
			AccountBackend::Telegram);
		QCOMPARE(result, AccountBackendBindResult::RejectedLiveMtp);
		QCOMPARE(state.backend, AccountBackend::Unspecified);
		QVERIFY(Main::AccountBackendBindNeedsMtpRebuild(result));
		QVERIFY(!Main::ShouldPersistAccountBackendTag(
			result,
			AccountBackend::Telegram));
		state.backend = AccountBackend::Telegram;
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Telegram),
			AccountBackendBindResult::Unchanged);
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Core),
			AccountBackendBindResult::RejectedChange);
	}

	void persistOnlyNewlyBoundBackends() {
		QVERIFY(Main::ShouldPersistAccountBackendTag(
			AccountBackendBindResult::Bound,
			AccountBackend::Telegram));
		QVERIFY(Main::ShouldPersistAccountBackendTag(
			AccountBackendBindResult::Bound,
			AccountBackend::Core));
		QVERIFY(!Main::ShouldPersistAccountBackendTag(
			AccountBackendBindResult::Bound,
			AccountBackend::Unspecified));
		QVERIFY(!Main::ShouldPersistAccountBackendTag(
			AccountBackendBindResult::Unchanged,
			AccountBackend::Telegram));
		QVERIFY(!Main::ShouldPersistAccountBackendTag(
			AccountBackendBindResult::RejectedLiveMtp,
			AccountBackend::Telegram));
	}

	void persistFailureDoesNotCountAsDurableBind() {
		QCOMPARE(
			Main::AccountBackendBindPersistOutcome(
				AccountBackendBindResult::Bound,
				AccountBackend::Telegram,
				false),
			AccountBackendBindResult::RejectedPersistFailed);
		QCOMPARE(
			Main::AccountBackendBindPersistOutcome(
				AccountBackendBindResult::Bound,
				AccountBackend::Core,
				true),
			AccountBackendBindResult::Bound);
		QCOMPARE(
			Main::AccountBackendBindPersistOutcome(
				AccountBackendBindResult::Unchanged,
				AccountBackend::Telegram,
				false),
			AccountBackendBindResult::Unchanged);
		QCOMPARE(
			Main::AccountBackendBindPersistOutcome(
				AccountBackendBindResult::RejectedLiveMtp,
				AccountBackend::Telegram,
				false),
			AccountBackendBindResult::RejectedLiveMtp);
	}

	void preSessionBindThenLiveMtpIsIdempotent() {
		AccountBackendBindState state;
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Core),
			AccountBackendBindResult::Bound);
		QVERIFY(Main::ShouldPersistAccountBackendTag(
			AccountBackendBindResult::Bound,
			state.backend));
		state.mtpAlreadyExists = true;
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Core),
			AccountBackendBindResult::Unchanged);
		QCOMPARE(
			Main::TryBindAccountBackend(state, AccountBackend::Telegram),
			AccountBackendBindResult::RejectedChange);
	}

	void storeWriteReadCoreAndTelegram() {
		StoreFixture fx;
		QVERIFY(fx.dir.isValid());
		QVERIFY(fx.writeSentinels());
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Written);
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Unchanged);
		auto read = Main::ReadAccountBackendStore(fx.path(), fx.identity);
		QCOMPARE(read.status, AccountBackendStoreReadStatus::Valid);
		QCOMPARE(read.record.backend, AccountBackend::Core);
		QCOMPARE(read.record.identity, fx.identity);
		QCOMPARE(read.record.version, Main::kAccountBackendStoreVersion);
		QCOMPARE(
			ReadAll(fx.path()),
			Main::SerializeAccountBackendStorePayload(
				Main::kAccountBackendStoreVersion,
				fx.identity,
				Main::EncodeAccountBackendTag(AccountBackend::Core)));
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Telegram),
			AccountBackendStoreWriteStatus::Written);
		read = Main::ReadAccountBackendStore(fx.path(), fx.identity);
		QCOMPARE(read.status, AccountBackendStoreReadStatus::Valid);
		QCOMPARE(read.record.backend, AccountBackend::Telegram);
		QVERIFY(fx.sentinelsUnchanged());
		QVERIFY(!ReadAll(fx.path()).contains(fx.authSentinel));
		QVERIFY(!ReadAll(fx.path()).contains(fx.mapSentinel));
	}

	void storeRejectsUnspecifiedAndUnsafeIdentity() {
		StoreFixture fx;
		QVERIFY(fx.writeSentinels());
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Unspecified),
			AccountBackendStoreWriteStatus::Rejected);
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				QByteArray(),
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Rejected);
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				QByteArrayLiteral("not/hex"),
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Rejected);
		QVERIFY(!QFile::exists(fx.path()));
		QVERIFY(fx.sentinelsUnchanged());
		const auto missing = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		QCOMPARE(missing.status, AccountBackendStoreReadStatus::Missing);
	}

	void storeIdentitiesAreIndependent() {
		StoreFixture fx;
		QVERIFY(fx.writeSentinels());
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Written);
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.pathFor(fx.otherIdentity),
				fx.otherIdentity,
				AccountBackend::Telegram),
			AccountBackendStoreWriteStatus::Written);
		const auto first = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		const auto second = Main::ReadAccountBackendStore(
			fx.pathFor(fx.otherIdentity),
			fx.otherIdentity);
		QCOMPARE(first.record.backend, AccountBackend::Core);
		QCOMPARE(second.record.backend, AccountBackend::Telegram);
		const auto mismatch = Main::ReadAccountBackendStore(
			fx.path(),
			fx.otherIdentity);
		QCOMPARE(mismatch.status, AccountBackendStoreReadStatus::Invalid);
		QVERIFY(QFile::exists(fx.path()));
		QVERIFY(fx.sentinelsUnchanged());
	}

	void storeCorruptIsInvalidPreservedAndNeverTelegram() {
		StoreFixture fx;
		QVERIFY(fx.writeSentinels());
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Telegram),
			AccountBackendStoreWriteStatus::Written);
		const auto corrupt = QByteArrayLiteral("XXXX-not-a-backend-record");
		QVERIFY(WriteAll(fx.path(), corrupt));
		const auto read = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		QCOMPARE(read.status, AccountBackendStoreReadStatus::Invalid);
		QCOMPARE(ReadAll(fx.path()), corrupt);
		const auto migration = Main::ResolveStoredAccountBackend(
			Main::MakeAccountBackendMigrationEvidence(
				AccountBackendRecordKind::LegacyExisting,
				read,
				true,
				false));
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::Chooser);
		QVERIFY(fx.sentinelsUnchanged());
	}

	void storeTruncatedIsInvalidPreservedAndNeverTelegram() {
		StoreFixture fx;
		QVERIFY(fx.writeSentinels());
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Written);
		QFile file(fx.path());
		QVERIFY(file.open(QIODevice::ReadWrite));
		QVERIFY(file.resize(3));
		file.close();
		QCOMPARE(QFileInfo(fx.path()).size(), qint64(3));
		const auto read = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		QCOMPARE(read.status, AccountBackendStoreReadStatus::Invalid);
		QCOMPARE(QFileInfo(fx.path()).size(), qint64(3));
		const auto migration = Main::ResolveStoredAccountBackend(
			Main::MakeAccountBackendMigrationEvidence(
				AccountBackendRecordKind::FreshMissing,
				read,
				true,
				true));
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::Chooser);
		QVERIFY(fx.sentinelsUnchanged());
	}

	void storeUnsupportedVersionIsInvalidPreserved() {
		StoreFixture fx;
		QVERIFY(fx.writeSentinels());
		const auto payload = Main::SerializeAccountBackendStorePayload(
			99,
			fx.identity,
			Main::EncodeAccountBackendTag(AccountBackend::Telegram));
		QVERIFY(WriteAll(fx.path(), payload));
		const auto read = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		QCOMPARE(read.status, AccountBackendStoreReadStatus::Invalid);
		QCOMPARE(ReadAll(fx.path()), payload);
		const auto migration = Main::ResolveStoredAccountBackend(
			Main::MakeAccountBackendMigrationEvidence(
				AccountBackendRecordKind::LegacyExisting,
				read,
				true,
				false));
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::Chooser);
		QVERIFY(fx.sentinelsUnchanged());
	}

	void storeOwnerPrivatePermissions() {
		StoreFixture fx;
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Written);
#if !defined(Q_OS_WIN)
		QVERIFY(IsOwnerPrivate(fx.path()));
		QVERIFY(QFileInfo(fx.path()).permission(QFileDevice::ReadOwner));
		QVERIFY(QFileInfo(fx.path()).permission(QFileDevice::WriteOwner));
#endif
	}

	void storeAtomicFailureLeavesOriginal() {
		StoreFixture fx;
		QVERIFY(fx.writeSentinels());
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Written);
		const auto original = ReadAll(fx.path());
		QVERIFY(!original.isEmpty());

		const auto blocked = fx.pathFor(QByteArrayLiteral("AAAAAAAAAAAAAAAA"));
		QVERIFY(QDir().mkpath(blocked));
		QCOMPARE(
			Main::WriteAccountBackendStore(
				blocked,
				QByteArrayLiteral("AAAAAAAAAAAAAAAA"),
				AccountBackend::Telegram),
			AccountBackendStoreWriteStatus::Failed);
		QVERIFY(QFileInfo(blocked).isDir());

#if !defined(Q_OS_WIN)
		const auto parent = QFileInfo(fx.path()).absolutePath();
		const auto previous = QFileInfo(parent).permissions();
		RestorePermissions restore{ parent, previous };
		QVERIFY(QFile::setPermissions(
			parent,
			QFileDevice::ReadOwner | QFileDevice::ExeOwner));
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Telegram),
			AccountBackendStoreWriteStatus::Failed);
		QCOMPARE(ReadAll(fx.path()), original);
#endif
		QCOMPARE(ReadAll(fx.path()), original);
		QVERIFY(fx.sentinelsUnchanged());
	}

	void missingMapPresentMetadataResolvesBeforeStartMtp() {
		StoreFixture fx;
		QVERIFY(fx.writeSentinels());
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Core),
			AccountBackendStoreWriteStatus::Written);
		const auto read = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		const auto core = Main::ResolveStoredAccountBackend(
			Main::MakeAccountBackendMigrationEvidence(
				AccountBackendRecordKind::FreshMissing,
				read,
				false,
				false));
		QCOMPARE(core.backend, AccountBackend::Core);
		QCOMPARE(core.need, AccountBackendMigrationNeed::None);
		QVERIFY(!core.needsProvenLegacyCoreMarker);

		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				AccountBackend::Telegram),
			AccountBackendStoreWriteStatus::Written);
		const auto telegram = Main::ResolveStoredAccountBackend(
			Main::MakeAccountBackendMigrationEvidence(
				AccountBackendRecordKind::FreshMissing,
				Main::ReadAccountBackendStore(fx.path(), fx.identity),
				false,
				false));
		QCOMPARE(telegram.backend, AccountBackend::Telegram);
		QCOMPARE(telegram.need, AccountBackendMigrationNeed::None);
		QVERIFY(fx.sentinelsUnchanged());
	}

	void missingMetadataWithKeysMigratesTelegramWithoutRewritingAuth() {
		StoreFixture fx;
		QVERIFY(fx.writeSentinels());
		const auto missing = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		QCOMPARE(missing.status, AccountBackendStoreReadStatus::Missing);
		const auto migration = Main::ResolveStoredAccountBackend(
			Main::MakeAccountBackendMigrationEvidence(
				AccountBackendRecordKind::LegacyExisting,
				missing,
				true,
				false));
		QCOMPARE(migration.backend, AccountBackend::Telegram);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::None);
		QVERIFY(Main::ShouldPersistAccountBackendTag(
			AccountBackendBindResult::Bound,
			migration.backend));
		QCOMPARE(
			Main::WriteAccountBackendStore(
				fx.path(),
				fx.identity,
				migration.backend),
			AccountBackendStoreWriteStatus::Written);
		const auto stored = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		QCOMPARE(stored.record.backend, AccountBackend::Telegram);
		QVERIFY(fx.sentinelsUnchanged());
	}

	void freshMissingWithoutMetadataStaysChooser() {
		StoreFixture fx;
		const auto missing = Main::ReadAccountBackendStore(
			fx.path(),
			fx.identity);
		const auto migration = Main::ResolveStoredAccountBackend(
			Main::MakeAccountBackendMigrationEvidence(
				AccountBackendRecordKind::FreshMissing,
				missing,
				false,
				false));
		QCOMPARE(migration.backend, AccountBackend::Unspecified);
		QCOMPARE(migration.need, AccountBackendMigrationNeed::Chooser);
		QVERIFY(!migration.needsProvenLegacyCoreMarker);
		QVERIFY(!QFile::exists(fx.path()));
	}

};

QTEST_GUILESS_MAIN(AccountBackendTests)
#include "account_backend_tests.moc"
