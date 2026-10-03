#pragma once

#include "data/data_star_gift.h"

namespace Main {
class Session;
}

namespace Window {
class SessionController;
}

namespace Ui {
class GenericBox;
class VerticalLayout;
}

namespace Ayu::Visual {

[[nodiscard]] QString Text(QString english, QString russian);
[[nodiscard]] rpl::producer<QString> TextValue(
	QString english,
	QString russian);
[[nodiscard]] bool Enabled(not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<bool> EnabledValue(
	not_null<Main::Session*> session);
[[nodiscard]] rpl::producer<> Changes(not_null<Main::Session*> session);
[[nodiscard]] bool SetEnabled(not_null<Main::Session*> session, bool enabled);
[[nodiscard]] QString Phone(not_null<Main::Session*> session);
[[nodiscard]] QStringList Usernames(not_null<Main::Session*> session);
[[nodiscard]] bool SetProfile(
	not_null<Main::Session*> session,
	QString phone,
	QStringList usernames);
[[nodiscard]] std::vector<Data::SavedStarGift> Gifts(
	not_null<PeerData*> peer,
	bool pinnedOnly = false);
[[nodiscard]] bool IsLocal(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id);
[[nodiscard]] std::optional<Data::SavedStarGift> FindGift(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id);
[[nodiscard]] std::optional<Data::SavedStarGift> AddGift(
	not_null<PeerData*> recipient,
	const MTPStarGift &gift,
	QString message,
	bool anonymous);
[[nodiscard]] bool SetPinned(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id,
	bool pinned);
[[nodiscard]] bool SetHidden(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id,
	bool hidden);
[[nodiscard]] bool RemoveGift(
	not_null<Main::Session*> session,
	Data::SavedStarGiftId id);
void RestoreMessages(not_null<Main::Session*> session);
void ShowProfileEditor(
	not_null<Window::SessionController*> window,
	bool editPhone);
void ShowCatalog(
	not_null<Window::SessionController*> window,
	not_null<PeerData*> recipient);
void ShowLocalGift(
	not_null<Window::SessionController*> window,
	const Data::SavedStarGift &gift);
void AddProfileRows(
	not_null<Ui::VerticalLayout*> container,
	not_null<Window::SessionController*> window);

}
