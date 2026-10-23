#pragma once

#include <memory>
#include <utility>

namespace Delta {

class AccountScope final {
public:
	void invalidate() {
		_token = std::make_shared<bool>(true);
	}

	template <typename Callback>
	auto bind(Callback callback) const {
		return [guard = std::weak_ptr<bool>(_token), callback = std::move(callback)](auto reply) {
			if (!guard.expired()) callback(std::move(reply));
		};
	}

private:
	std::shared_ptr<bool> _token = std::make_shared<bool>(true);
};

} // namespace Delta
