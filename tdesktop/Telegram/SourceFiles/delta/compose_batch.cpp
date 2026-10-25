#include "compose_batch.h"

#include <QFileInfo>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace Delta {
namespace {

bool SameQueue(const ComposeQueue &a, const ComposeQueue &b) {
	return a.id == b.id && a.entries.size() == b.entries.size()
		&& std::equal(a.entries.begin(), a.entries.end(), b.entries.begin(),
			[](const QueuedCompose &a, const QueuedCompose &b) {
				return a.id == b.id && a.messageId == b.messageId
					&& a.draft.text == b.draft.text
					&& a.draft.quotedMessageId == b.draft.quotedMessageId
					&& a.draft.file == b.draft.file
					&& a.draft.filename == b.draft.filename
					&& a.draft.viewType == b.draft.viewType;
			});
}

} // namespace

std::shared_ptr<ComposeBatch> ComposeBatch::Create(
		std::shared_ptr<ComposeQueueStore> queues,
		int account, int chat, size_t count, Changed changed) {
	if (!queues || account <= 0 || chat <= 0 || !count) return nullptr;
	return std::shared_ptr<ComposeBatch>(new ComposeBatch(
		std::move(queues), account, chat, count, std::move(changed)));
}

ComposeBatch::ComposeBatch(std::shared_ptr<ComposeQueueStore> queues,
		int account, int chat, size_t count, Changed changed)
: _queues(std::move(queues))
, _account(account)
, _chat(chat)
, _slots(count)
, _changed(std::move(changed)) {
}

bool ComposeBatch::accepting(size_t index) const {
	return (_state == State::Preparing || _state == State::Failed)
		&& index < _slots.size() && !_slots[index].draft
		&& _slots[index].error.isEmpty();
}

bool ComposeBatch::prepared(size_t index, ComposeDraft draft,
		std::shared_ptr<void> owner) {
	if (!accepting(index)) return false;
	if (draft.quotedMessageId < 0 || !owner
		|| !QFileInfo(draft.file).isFile() || QFileInfo(draft.file).size() <= 0) {
		return failed(index, u"An attachment could not be prepared. No files were sent."_s);
	}
	_slots[index].draft = std::move(draft);
	_slots[index].owner = std::move(owner);
	if (ready()) save();
	else update(_state, _error);
	return true;
}

bool ComposeBatch::failed(size_t index, QString error) {
	if (!accepting(index)) return false;
	if (error.isEmpty()) error = u"An attachment could not be prepared. No files were sent."_s;
	_slots[index].error = error;
	update(State::Failed, std::move(error));
	return true;
}

bool ComposeBatch::ready() const {
	return std::all_of(_slots.begin(), _slots.end(), [](const Slot &slot) {
		return slot.draft.has_value();
	});
}

void ComposeBatch::save() {
	const auto self = shared_from_this();
	update(State::Saving);
	auto drafts = std::vector<ComposeDraft>();
	for (const auto &slot : _slots) drafts.push_back(*slot.draft);
	_queues->stage(_account, _chat, std::move(drafts),
		[self](ComposeQueue queue, QString error) {
			self->_queue = std::move(queue);
			const auto state = error.isEmpty() ? State::Saved : State::Failed;
			if (state == State::Saved) self->releaseOwners();
			self->update(state, std::move(error));
		});
}

bool ComposeBatch::retrySave() {
	if (_state != State::Failed || !ready()) return false;
	if (_queue.id.isEmpty()) {
		save();
		return true;
	}
	const auto self = shared_from_this();
	update(State::Saving);
	_queues->load(_account, _chat, [self](ComposeQueue queue, QString error) {
		if (error.isEmpty() && SameQueue(queue, self->_queue)) {
			self->_queue = std::move(queue);
			self->_reconciled = true;
			self->releaseOwners();
			self->update(State::Saved);
		} else {
			self->update(State::Failed, error.isEmpty()
				? u"The saved batch changed. Review its recovery queue before continuing."_s
				: std::move(error));
		}
	});
	return true;
}

bool ComposeBatch::discard() {
	if ((_state != State::Preparing && _state != State::Failed)
		|| !_queue.id.isEmpty()) return false;
	const auto self = shared_from_this();
	releaseOwners();
	update(State::Discarded);
	return true;
}

void ComposeBatch::releaseOwners() {
	for (auto &slot : _slots) slot.owner.reset();
}

void ComposeBatch::update(State state, QString error) {
	const auto self = shared_from_this();
	_state = state;
	_error = std::move(error);
	if (_changed) _changed();
}

} // namespace Delta
