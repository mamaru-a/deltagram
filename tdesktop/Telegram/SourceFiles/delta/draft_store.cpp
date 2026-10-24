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
	// The composer is read-only during the local Core handoff. In particular,
	// a stale donor draft-clear must not delete the message being handed over.
	if (entry.send) return;
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
			if (loaded.send) finishSend(key, 0, error);
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
	if (entry.saving || (entry.send && entry.send->started)) return;
	if (!entry.pending) {
		if (entry.send) startSend(key);
		return;
	}
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
				if (saved.send) finishSend(key, 0, Failure(reply));
				if (_error) _error(Failure(reply));
				return;
			}
			flush(key);
		});
}

bool DraftStore::sending(int accountId, int chatId) const {
	const auto i = _entries.find(Key(accountId, chatId));
	return i != _entries.end() && i->second.send.has_value();
}

bool DraftStore::preparingSend() const {
	for (const auto &[key, entry] : _entries) {
		if (entry.send && !entry.send->prepared) return true;
	}
	return false;
}

void DraftStore::sendText(int accountId, int chatId, const QString &text,
		int quotedMessageId, SendCallback done) {
	const auto key = Key(accountId, chatId);
	auto &entry = _entries[key];
	if (entry.send || text.trimmed().isEmpty()) {
		if (done) done(0, u"A message is already being saved, or its text is empty."_s);
		return;
	}
	entry.send = Entry::Send{ text, quotedMessageId, std::move(done) };
	ensureLoaded(key);
}

void DraftStore::startSend(Key key) {
	auto &entry = _entries[key];
	if (!entry.value.file.isEmpty()) {
		finishSend(key, 0, u"This chat has an attachment draft. It was preserved; send or remove the attachment before sending text."_s);
		return;
	}
	entry.send->started = true;
	const auto text = entry.send->text;
	const auto quote = entry.send->quote;
	const auto guard = std::weak_ptr<bool>(_alive);
	_rpc->call(u"misc_set_draft"_s, {key.first, key.second, text,
		QJsonValue(), QJsonValue(), quote > 0 ? QJsonValue(quote) : QJsonValue(), u"Text"_s},
		[=, this](QJsonObject reply) {
			if (guard.expired()) return;
			if (reply.contains(u"error"_s)) {
				finishSend(key, 0, Failure(reply));
				return;
			}
			_entries[key].value.text = text;
			_entries[key].value.quotedMessageId = quote;
			_rpc->call(u"get_draft"_s, {key.first, key.second}, [=, this](QJsonObject read) {
				if (guard.expired()) return;
				const auto draft = read.value(u"result"_s).toObject();
				const auto id = draft.value(u"id"_s).toInt();
				if (read.contains(u"error"_s) || id <= 0
					|| draft.value(u"state"_s).toInt() != 19
					|| draft.value(u"chatId"_s).toInt() != key.second
					|| draft.value(u"text"_s).toString() != text
					|| !draft.value(u"file"_s).toString().isEmpty()) {
					finishSend(key, 0, u"Could not confirm the saved draft. Nothing was sent."_s);
					return;
				}
				_entries[key].send->prepared = true;
				_rpc->call(u"misc_send_draft"_s, {key.first, key.second},
					[=, this](QJsonObject sent) {
						if (guard.expired()) return;
						if (!sent.contains(u"error"_s) && sent.value(u"result"_s).toInt() == id) {
							finishSend(key, id, {});
							return;
						}
						// Core may have committed before an error reached us. Inspect
						// the known ID; never retry a possibly completed send.
						_rpc->call(u"get_message"_s, {key.first, id},
							[=, this](QJsonObject checked) {
								if (guard.expired()) return;
								const auto message = checked.value(u"result"_s).toObject();
								const auto state = message.value(u"state"_s).toInt();
								const auto owned = !checked.contains(u"error"_s)
									&& message.value(u"id"_s).toInt() == id
									&& message.value(u"chatId"_s).toInt() == key.second
									&& (state == 20 || state == 24 || state == 26 || state == 28);
								finishSend(key, owned ? id : 0, owned ? QString()
									: u"The send was not confirmed. Check the chat and its saved draft before retrying."_s);
							});
					});
			});
		});
}

void DraftStore::finishSend(Key key, int messageId, QString error) {
	auto &entry = _entries[key];
	const auto done = std::move(entry.send->done);
	entry.send.reset();
	if (messageId > 0) entry.value = {};
	if (done) done(messageId, std::move(error));
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
