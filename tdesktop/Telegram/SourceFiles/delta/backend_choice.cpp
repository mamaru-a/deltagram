/*
This file is part of Delta Tel, a Telegram Desktop based Delta Chat client.
*/
#include "delta/backend_choice.h"

#include "lang/lang_keys.h"
#include "ui/painter.h"
#include "ui/widgets/buttons.h"
#include "ui/widgets/labels.h"
#include "styles/style_intro.h"
#include "styles/style_window.h"

#include <algorithm>

namespace Delta {

BackendChoice::BackendChoice(QWidget *parent)
: RpWidget(parent)
, _title(
	this,
	tr::lng_delta_account_choice_title(),
	st::introCoverTitle)
, _description(
	this,
	tr::lng_delta_account_choice_about(),
	st::introCoverDescription)
, _core(
	this,
	tr::lng_delta_account_choice_core(),
	st::introNextButton)
, _telegram(
	this,
	tr::lng_delta_account_choice_telegram(),
	st::introNextButton)
, _error(this, QString(), st::introErrorCentered) {
	_error->hide();
	_core->setClickedCallback([=] {
		_chosen.fire_copy(Main::AccountBackend::Core);
	});
	_telegram->setClickedCallback([=] {
		_chosen.fire_copy(Main::AccountBackend::Telegram);
	});
}

BackendChoice::~BackendChoice() = default;

void BackendChoice::setInnerFocus() {
	_core->setFocus(Qt::OtherFocusReason);
}

void BackendChoice::showError(const QString &text) {
	_error->setText(text);
	_error->setVisible(!text.isEmpty());
	updateControlsGeometry();
}

rpl::producer<Main::AccountBackend> BackendChoice::chosen() const {
	return _chosen.events();
}

void BackendChoice::paintEvent(QPaintEvent *e) {
	QPainter p(this);
	p.fillRect(e->rect(), st::windowBg);
}

void BackendChoice::resizeEvent(QResizeEvent *e) {
	updateControlsGeometry();
}

void BackendChoice::updateControlsGeometry() {
	const auto top = std::max(
		(height() - st::introHeight) / 2,
		int(st::introStepTopMin));
	const auto left = (width() - st::introNextButton.width) / 2;
	_title->moveToLeft(
		(width() - _title->width()) / 2,
		top + st::introCoverTitleTop);
	_description->resizeToWidth(st::introNextButton.width);
	_description->moveToLeft(
		(width() - _description->width()) / 2,
		top + st::introCoverDescriptionTop);
	_core->moveToLeft(left, top + st::introNextTop);
	_telegram->moveToLeft(
		left,
		_core->y() + _core->height() + st::introLinkTop);
	_error->resizeToWidth(width());
	_error->moveToLeft(
		0,
		_telegram->y() + _telegram->height() + st::introLinkTop);
}

QString BackendChoiceErrorText(Main::AccountBackendBindResult result) {
	switch (result) {
	case Main::AccountBackendBindResult::RejectedPersistFailed:
		return tr::lng_delta_account_persist_failed(tr::now);
	case Main::AccountBackendBindResult::RejectedCoreWithKeys:
		return tr::lng_delta_account_core_keys_denied(tr::now);
	case Main::AccountBackendBindResult::RejectedChange:
	case Main::AccountBackendBindResult::RejectedClear:
		return tr::lng_delta_account_change_denied(tr::now);
	case Main::AccountBackendBindResult::RejectedLiveSession:
		return tr::lng_delta_account_session_denied(tr::now);
	default:
		return tr::lng_delta_account_start_failed(tr::now);
	}
}

QString CoreSessionStartErrorText(Main::CoreSessionStartResult result) {
	switch (result) {
	case Main::CoreSessionStartResult::RejectedNotCore:
		return tr::lng_delta_account_core_required(tr::now);
	case Main::CoreSessionStartResult::RejectedOtherBridge:
		return tr::lng_delta_account_core_busy(tr::now);
	default:
		return tr::lng_delta_account_start_failed(tr::now);
	}
}

} // namespace Delta
