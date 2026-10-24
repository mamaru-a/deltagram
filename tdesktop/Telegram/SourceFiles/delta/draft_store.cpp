#include "draft_store.h"

#include <utility>

using namespace Qt::StringLiterals;

namespace Delta {
namespace {

QString Failure(const QJsonObject &reply) {
	return reply.value(u"error"_s).toObject()
		.value(u"message"_s).toString(u"Could not save draft."_s);
}

} // namespace

DraftStore::DraftStore(std::shared_ptr<DeltaRpc> rpc, std::function<void(QString)> error)
: _rpc(std::move(rpc)), _error(std::move(error)) {
}

void DraftStore::load(int accountId, int chatId, Callback done) {
	const auto key = Key(accountId, chatId);
	auto &entry = _entries[key];
	entry.readers.push_back(std::move(done));
	ensureLoaded(key);
}

void DraftStore::save(int accountId, int chatId, const QString &text, int quotedMessageId) {
	const auto key = Key(accountId, chatId);
	auto &entry = _entries[key];
	if (!entry.loaded && !entry.pending && text.isEmpty() && !quotedMessageId) return;
	if (entry.loaded && !entry.pending
		&& entry.value.text == text && entry.value.quotedMessageId == quotedMessageId) return;
	entry.pending = std::make_pair(text, quotedMessageId);
	ensureLoaded(key);
}

void DraftStore::recoverText(
		int accountId,
		int chatId,
		const QString &text,
		int quotedMessageId,
		std::function<void(ComposeDraft)> recovered) {
	if (text.isEmpty()) return;
	const auto guard = std::weak_ptr<bool>(_alive);
	load(accountId, chatId, [=, this](ComposeDraft draft, QString error) {
		if (guard.expired() || !error.isEmpty()) return;
		const auto combined = draft.text.isEmpty()
			? text
			: text + u"\n\n"_s + draft.text;
		const auto quote = draft.text.isEmpty() && draft.file.isEmpty()
			&& !draft.quotedMessageId
			? quotedMessageId
			: draft.quotedMessageId;
		save(accountId, chatId, combined, quote);
		draft.text = combined;
		draft.quotedMessageId = quote;
		if (recovered) recovered(std::move(draft));
	});
}

void DraftStore::ensureLoaded(Key key) {
	auto &entry = _entries[key];
	if (entry.loaded) {
		flush(key);
		notifyReaders(entry, {});
		return;
	}
	if (entry.loading) return;
	entry.loading = true;
	const auto guard = std::weak_ptr<bool>(_alive);
	_rpc->call(u"get_draft"_s, { key.first, key.second }, [=, this](QJsonObject reply) {
		if (guard.expired()) return;
		auto &loaded = _entries[key];
		loaded.loading = false;
		if (reply.contains(u"error"_s)) {
			const auto error = Failure(reply);
			notifyReaders(loaded, error);
			if (_error) _error(error);
			return;
		}
		const auto value = reply.value(u"result"_s).toObject();
		loaded.value = ComposeDraft{
			value.value(u"text"_s).toString(),
			value.value(u"quote"_s).toObject().value(u"messageId"_s).toInt(),
			value.value(u"file"_s).toString(),
			value.value(u"fileName"_s).toString(),
			value.value(u"viewType"_s).toString(),
		};
		loaded.loaded = true;
		flush(key);
		notifyReaders(loaded, {});
	});
}

void DraftStore::flush(Key key) {
	auto &entry = _entries[key];
	if (entry.saving || !entry.pending) return;
	entry.value.text = entry.pending->first;
	entry.value.quotedMessageId = entry.pending->second;
	entry.pending.reset();
	entry.saving = true;
	const auto value = entry.value;
	const auto remove = value.text.isEmpty() && !value.quotedMessageId && value.file.isEmpty();
	const auto params = remove ? QJsonArray{ key.first, key.second } : QJsonArray{
		key.first, key.second, value.text,
		value.file.isEmpty() ? QJsonValue() : QJsonValue(value.file),
		value.filename.isEmpty() ? QJsonValue() : QJsonValue(value.filename),
		value.quotedMessageId > 0 ? QJsonValue(value.quotedMessageId) : QJsonValue(),
		value.viewType.isEmpty() ? QJsonValue() : QJsonValue(value.viewType),
	};
	const auto guard = std::weak_ptr<bool>(_alive);
	_rpc->call(remove ? u"remove_draft"_s : u"misc_set_draft"_s, params,
		[=, this](QJsonObject reply) {
			if (guard.expired()) return;
			auto &saved = _entries[key];
			saved.saving = false;
			if (reply.contains(u"error"_s)) {
				if (!saved.pending) saved.pending = std::make_pair(value.text, value.quotedMessageId);
				if (_error) _error(Failure(reply));
				return;
			}
			flush(key);
		});
}

void DraftStore::notifyReaders(Entry &entry, const QString &error) {
	const auto readers = std::exchange(entry.readers, {});
	auto value = entry.value;
	if (entry.pending) {
		value.text = entry.pending->first;
		value.quotedMessageId = entry.pending->second;
	}
	for (const auto &done : readers) if (done) done(value, error);
}

} // namespace Delta
