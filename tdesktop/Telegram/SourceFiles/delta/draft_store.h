#pragma once

#include "delta_rpc.h"

#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace Delta {

struct ComposeDraft {
	QString text;
	int quotedMessageId = 0;
	QString file;
	QString filename;
	QString viewType;
};

class DraftStore final {
public:
	using Callback = std::function<void(ComposeDraft, QString)>;
	DraftStore(std::shared_ptr<DeltaRpc> rpc, std::function<void(QString)> error);
	void load(int accountId, int chatId, Callback done);
	void save(int accountId, int chatId, const QString &text, int quotedMessageId);

private:
	using Key = std::pair<int, int>;
	struct Entry {
		ComposeDraft value;
		std::optional<std::pair<QString, int>> pending;
		std::vector<Callback> readers;
		bool loaded = false;
		bool loading = false;
		bool saving = false;
	};
	void ensureLoaded(Key key);
	void flush(Key key);
	void notifyReaders(Entry &entry, const QString &error);
	std::shared_ptr<DeltaRpc> _rpc;
	std::function<void(QString)> _error;
	std::map<Key, Entry> _entries;
	std::shared_ptr<bool> _alive = std::make_shared<bool>(true);
};

} // namespace Delta
