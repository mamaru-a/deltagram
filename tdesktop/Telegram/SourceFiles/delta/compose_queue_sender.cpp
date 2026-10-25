#include "compose_queue_sender.h"

using namespace Qt::StringLiterals;

namespace Delta {
namespace {

bool SameDraft(const QJsonObject &message, const ComposeDraft &draft) {
	return message[u"file"_s].toString() == draft.file
		&& message[u"text"_s].toString() == draft.text
		&& message[u"quote"_s].toObject()[u"messageId"_s].toInt() == draft.quotedMessageId;
}

QString ReadError() {
	return u"Could not confirm the queued attachment. It was preserved; check the chat before continuing."_s;
}

} // namespace

struct ComposeQueueSender::Sending {
	int account = 0;
	int chat = 0;
	ComposeQueue queue;
	Done done;
	std::shared_ptr<ComposeQueueStore> storage;
};

ComposeQueueSender::ComposeQueueSender(std::shared_ptr<DeltaRpc> rpc,
		std::shared_ptr<ComposeQueueStore> queues, std::shared_ptr<DraftStore> drafts)
: _rpc(std::move(rpc)), _queues(std::move(queues)), _drafts(std::move(drafts)) {
}

bool ComposeQueueSender::busy(int account, int chat) const {
	return _busy.contains({ account, chat });
}

void ComposeQueueSender::call(QString method, QJsonArray params,
		std::function<void(QJsonObject)> done) {
	const auto guard = std::weak_ptr<bool>(_alive);
	_rpc->call(method, params, [=](QJsonObject reply) {
		if (!guard.expired()) done(std::move(reply));
	});
}

void ComposeQueueSender::sendNext(int account, int chat, const QString &queueId,
		const QString &entryId, Done done) {
	begin(account, chat, queueId, entryId, false, std::move(done));
}

void ComposeQueueSender::discardNext(int account, int chat, const QString &queueId,
		const QString &entryId, Done done) {
	begin(account, chat, queueId, entryId, true, std::move(done));
}

void ComposeQueueSender::begin(int account, int chat, const QString &queueId,
		const QString &entryId, bool discard, Done done) {
	if (account <= 0 || chat <= 0 || queueId.isEmpty() || entryId.isEmpty()
		|| _drafts->busy(account, chat) || !_busy.insert({ account, chat }).second) {
		if (done) done(0, {}, u"This compose queue is unavailable or already sending."_s);
		return;
	}
	const auto state = std::make_shared<Sending>(Sending{ account, chat, {}, std::move(done), _queues });
	const auto guard = std::weak_ptr<bool>(_alive);
	_queues->load(account, chat, [=, this](ComposeQueue queue, QString error) {
		if (guard.expired()) return;
		state->queue = std::move(queue);
		if (!error.isEmpty()) finish(state, 0, std::move(error));
		else if (state->queue.id != queueId || state->queue.entries.empty()
			|| state->queue.entries.front().id != entryId) {
			finish(state, 0, u"The compose queue changed. Review it before continuing."_s);
		} else if (discard) this->discard(state);
		else inspect(state);
	});
}

void ComposeQueueSender::discard(std::shared_ptr<Sending> state) {
	const auto valid = [=](const QJsonValue &value) {
		if (value.isNull()) return true;
		if (!value.isObject()) return false;
		const auto draft = value.toObject();
		return draft.isEmpty() || (draft[u"id"_s].toInt() > 0
			&& draft[u"state"_s].toInt() == 19 && draft[u"chatId"_s].toInt() == state->chat);
	};
	call(u"get_draft"_s, { state->account, state->chat }, [=, this](QJsonObject reply) {
		if (reply.contains(u"error"_s) || !reply.contains(u"result"_s) || !valid(reply[u"result"_s])) {
			finish(state, 0, ReadError());
			return;
		}
		const auto draft = reply[u"result"_s].toObject();
		const auto entry = state->queue.entries.front();
		const auto id = draft[u"id"_s].toInt();
		const auto matches = id > 0 && SameDraft(draft, entry.draft)
			&& (!entry.messageId || (id == entry.messageId
				&& draft[u"fileName"_s].toString() == entry.draft.filename
				&& draft[u"viewType"_s].toString() == entry.draft.viewType));
		if (!matches) { advance(state, 0); return; }
		call(u"remove_draft"_s, { state->account, state->chat }, [=, this](QJsonObject) {
			call(u"get_draft"_s, { state->account, state->chat }, [=, this](QJsonObject read) {
				if (read.contains(u"error"_s) || !read.contains(u"result"_s) || !valid(read[u"result"_s])
					|| read[u"result"_s].toObject()[u"id"_s].toInt() == id) {
					finish(state, 0, u"Could not confirm the draft was discarded. The queue was preserved; review it before continuing."_s);
				} else advance(state, 0);
			});
		});
	});
}

void ComposeQueueSender::inspect(std::shared_ptr<Sending> state) {
	const auto entry = state->queue.entries.front();
	const auto known = entry.messageId > 0;
	call(known ? u"get_message"_s : u"get_draft"_s,
		{ state->account, known ? entry.messageId : state->chat }, [=, this](QJsonObject reply) {
			const auto value = reply[u"result"_s];
			const auto message = value.toObject();
			if (reply.contains(u"error"_s) || !reply.contains(u"result"_s)
				|| (!value.isNull() && !value.isObject())) {
				finish(state, 0, ReadError());
				return;
			}
			if (!known && message[u"file"_s].toString().isEmpty()) {
				const auto text = message[u"text"_s].toString();
				const auto quote = message[u"quote"_s].toObject()[u"messageId"_s].toInt();
				if ((!text.isEmpty() || quote)
					&& (text != entry.draft.text || quote != entry.draft.quotedMessageId)) {
					finish(state, 0, u"This chat has a newer draft. It was preserved; review it before sending the queued attachment."_s);
					return;
				}
				send(state);
				return;
			}
			const auto id = message[u"id"_s].toInt();
			const auto status = message[u"state"_s].toInt();
			if (id <= 0 || (known && id != entry.messageId)
				|| message[u"chatId"_s].toInt() != state->chat
				|| !SameDraft(message, entry.draft)) {
				finish(state, 0, ReadError());
				return;
			}
			if (known && (status == 20 || status == 24 || status == 26 || status == 28)) {
				advance(state, id);
				return;
			}
			if (status != 19 || (known
				&& (message[u"fileName"_s].toString() != entry.draft.filename
					|| message[u"viewType"_s].toString() != entry.draft.viewType))) {
				finish(state, 0, ReadError());
				return;
			}
			if (known) { send(state); return; }
			auto canonical = entry.draft;
			canonical.filename = message[u"fileName"_s].toString();
			canonical.viewType = message[u"viewType"_s].toString();
			const auto guard = std::weak_ptr<bool>(_alive);
			checkpoint(state, id, std::move(canonical), [=, this](QString error) {
				if (guard.expired()) return;
				if (!error.isEmpty()) finish(state, 0, std::move(error));
				else send(state);
			});
		});
}

void ComposeQueueSender::checkpoint(std::shared_ptr<Sending> state, int id,
		ComposeDraft draft, std::function<void(QString)> done) {
	const auto guard = std::weak_ptr<bool>(_alive);
	_queues->checkpoint(state->account, state->chat, state->queue.id,
		state->queue.entries.front().id, id,
		[=](ComposeQueue queue, QString error) {
			if (guard.expired()) { done(u"The compose operation was closed."_s); return; }
			if (error.isEmpty()) state->queue = std::move(queue);
			done(std::move(error));
		}, std::move(draft));
}

void ComposeQueueSender::send(std::shared_ptr<Sending> state) {
	const auto guard = std::weak_ptr<bool>(_alive);
	const auto entry = state->queue.entries.front();
	const auto done = [=, this](int id, QString error) {
		if (guard.expired()) return;
		if (!error.isEmpty() || id <= 0) finish(state, 0, error.isEmpty() ? ReadError() : std::move(error));
		else advance(state, id);
	};
	if (entry.messageId > 0) {
		_drafts->resumeAttachment(state->account, state->chat, entry.messageId, entry.draft, done);
	} else {
		_drafts->sendAttachment(state->account, state->chat, entry.draft, done, {},
			[=, this](int id, ComposeDraft draft, std::function<void(QString)> next) {
				if (guard.expired()) { next(u"The compose operation was closed."_s); return; }
				checkpoint(state, id, std::move(draft), std::move(next));
			});
	}
}

void ComposeQueueSender::advance(std::shared_ptr<Sending> state, int id) {
	const auto guard = std::weak_ptr<bool>(_alive);
	_queues->removeFirst(state->account, state->chat, state->queue.id,
		state->queue.entries.front().id, [=, this](ComposeQueue queue, QString error) {
			if (guard.expired()) return;
			state->queue = std::move(queue);
			finish(state, id, std::move(error));
		});
}

void ComposeQueueSender::finish(std::shared_ptr<Sending> state, int id, QString error) {
	_busy.erase({ state->account, state->chat });
	if (state->done) state->done(id, std::move(state->queue), std::move(error));
}

} // namespace Delta
