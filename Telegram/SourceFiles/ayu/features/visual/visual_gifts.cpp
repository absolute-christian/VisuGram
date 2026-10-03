#include "ayu/features/visual/visual_gifts.h"

#include "api/api_premium.h"
#include "base/unixtime.h"
#include "data/data_session.h"
#include "data/data_user.h"
#include "history/history.h"
#include "history/history_item.h"
#include "lang/lang_instance.h"
#include "main/main_session.h"
#include "mtproto/sender.h"

#include <QtCore/QDir>
#include <QtCore/QFile>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QSaveFile>

namespace Ayu::Visual {
namespace {

constexpr auto kVersion = 1;
constexpr auto kMaxFileSize = 16 * 1024 * 1024;
constexpr auto kMaxGifts = 1000;
constexpr auto kMaxPinned = 6;

struct Record {
	MTPStarGift source;
	Data::SavedStarGift gift;
	PeerId recipient;
};

class State final {
public:
	explicit State(not_null<Main::Session*> session);
	[[nodiscard]] bool save();
	void notify();
	void restoreMessages();
	void refreshResources();
	void removeMessage(const Record &record);

	const not_null<Main::Session*> session;
	MTP::Sender api;
	rpl::variable<bool> enabled = false;
	rpl::event_stream<> changes;
	QString phone;
	QStringList usernames;
	std::vector<Record> records;
	QJsonArray unreadableRecords;
	bool writable = true;
	bool resourcesRequested = false;

private:
	[[nodiscard]] QString path() const;
	void load();

};

[[nodiscard]] QByteArray Encode(const MTPStarGift &gift) {
	auto buffer = mtpBuffer();
	gift.write(buffer);
	return QByteArray(
		reinterpret_cast<const char*>(buffer.data()),
		buffer.size() * sizeof(mtpPrime)).toBase64();
}

[[nodiscard]] std::optional<MTPStarGift> Decode(const QByteArray &encoded) {
	const auto bytes = QByteArray::fromBase64(encoded);
	if (bytes.isEmpty()
		|| bytes.size() % sizeof(mtpPrime)
		|| bytes.size() > kMaxFileSize) {
		return {};
	}
	auto buffer = mtpBuffer(bytes.size() / sizeof(mtpPrime));
	memcpy(buffer.data(), bytes.constData(), bytes.size());
	auto from = static_cast<const mtpPrime*>(buffer.data());
	const auto end = from + buffer.size();
	auto gift = MTPStarGift();
	if (!gift.read(from, end) || from != end) {
		return {};
	}
	return gift;
}

[[nodiscard]] Data::SavedStarGift LocalGift(
		not_null<Main::Session*> session,
		Data::StarGift info,
		PeerId recipient,
		QString message,
		TimeId date,
		bool anonymous) {
	if (info.unique) {
		info.unique->ownerId = recipient;
		info.unique->ownerName = QString();
		info.unique->ownerAddress = QString();
		info.unique->hostId = PeerId();
		info.unique->starsForResale = -1;
		info.unique->nanoTonForResale = -1;
		info.unique->originalDetails = {
			.senderId = anonymous ? PeerId() : session->userPeerId(),
			.recipientId = recipient,
			.date = date,
		};
	}
	return {
		.info = std::move(info),
		.manageId = Data::SavedStarGiftId::User(
			session->data().nextLocalMessageId()),
		.message = TextWithEntities{ std::move(message) },
		.fromId = anonymous ? PeerId() : session->userPeerId(),
		.date = date,
		.anonymous = anonymous,
		.mine = recipient == session->userPeerId(),
	};
}

State::State(not_null<Main::Session*> session)
: session(session)
, api(&session->mtp()) {
	load();
}

QString State::path() const {
	return cWorkingDir() + u"tdata/ayu/visual/"_q
		+ QString::number(session->uniqueId()) + u".json"_q;
}

void State::load() {
	auto file = QFile(path());
	if (!file.exists()) {
		return;
	}
	if (!file.open(QIODevice::ReadOnly) || file.size() > kMaxFileSize) {
		writable = false;
		return;
	}
	auto error = QJsonParseError();
	const auto document = QJsonDocument::fromJson(file.readAll(), &error);
	const auto root = document.object();
	if (error.error != QJsonParseError::NoError
		|| root.value(u"version"_q).toInt() != kVersion
		|| root.value(u"account"_q).toString()
			!= QString::number(session->uniqueId())) {
		writable = false;
		return;
	}
	phone = root.value(u"phone"_q).toString();
	for (const auto &name : root.value(u"usernames"_q).toArray()) {
		usernames.push_back(name.toString());
	}
	const auto gifts = root.value(u"gifts"_q).toArray();
	if (gifts.size() > kMaxGifts) {
		writable = false;
		return;
	}
	for (const auto &value : gifts) {
		const auto object = value.toObject();
		const auto source = Decode(object.value(u"source"_q)
			.toString().toLatin1());
		const auto parsed = source ? Api::FromTL(session, *source) : std::nullopt;
		auto validRecipient = false;
		const auto recipient = PeerId(object.value(u"recipient"_q)
			.toString().toULongLong(&validRecipient));
		if (!parsed || !validRecipient || !recipient) {
			unreadableRecords.push_back(value);
			continue;
		}
		auto gift = LocalGift(
			session,
			*parsed,
			recipient,
			object.value(u"message"_q).toString(),
			object.value(u"date"_q).toInt(),
			object.value(u"anonymous"_q).toBool());
		gift.pinned = object.value(u"pinned"_q).toBool() && gift.info.unique;
		gift.hidden = object.value(u"hidden"_q).toBool();
		records.push_back({ *source, std::move(gift), recipient });
	}
	enabled = root.value(u"enabled"_q).toBool();
}

bool State::save() {
	if (!writable) {
		return false;
	}
	auto gifts = unreadableRecords;
	for (const auto &record : records) {
		const auto &gift = record.gift;
		gifts.push_back(QJsonObject{
			{ u"source"_q, QString::fromLatin1(Encode(record.source)) },
			{ u"recipient"_q, QString::number(record.recipient.value) },
			{ u"message"_q, gift.message.text },
			{ u"date"_q, int(gift.date) },
			{ u"anonymous"_q, gift.anonymous },
			{ u"pinned"_q, gift.pinned },
			{ u"hidden"_q, gift.hidden },
		});
	}
	const auto bytes = QJsonDocument(QJsonObject{
		{ u"version"_q, kVersion },
		{ u"account"_q, QString::number(session->uniqueId()) },
		{ u"enabled"_q, enabled.current() },
		{ u"phone"_q, phone },
		{ u"usernames"_q, QJsonArray::fromStringList(usernames) },
		{ u"gifts"_q, gifts },
	}).toJson(QJsonDocument::Compact);
	if (bytes.size() > kMaxFileSize
		|| !QDir().mkpath(cWorkingDir() + u"tdata/ayu/visual"_q)) {
		return false;
	}
	auto file = QSaveFile(path());
	return file.open(QIODevice::WriteOnly)
		&& file.write(bytes) == bytes.size()
		&& file.commit();
}

void State::notify() {
	changes.fire({});
}

void State::removeMessage(const Record &record) {
	const auto id = FullMsgId(record.recipient, record.gift.manageId.userMessageId());
	if (const auto item = session->data().message(id)) {
		item->destroy();
	}
}

void State::restoreMessages() {
	if (!enabled.current()) {
		return;
	}
	for (const auto &record : records) {
		const auto &gift = record.gift;
		const auto id = gift.manageId.userMessageId();
		if (session->data().message(FullMsgId(record.recipient, id))) {
			continue;
		}
		const auto action = [&]() -> MTPMessageAction {
			if (gift.info.unique) {
				const auto &original = record.source.c_starGiftUnique();
				auto attributes = QVector<MTPStarGiftAttribute>();
				for (const auto &attribute : original.vattributes().v) {
					if (attribute.type() != mtpc_starGiftAttributeOriginalDetails) {
						attributes.push_back(attribute);
					}
				}
				using DetailFlag = MTPDstarGiftAttributeOriginalDetails::Flag;
				attributes.push_back(MTP_starGiftAttributeOriginalDetails(
					MTP_flags((gift.anonymous ? DetailFlag() : DetailFlag::f_sender_id)
						| (gift.message.empty() ? DetailFlag() : DetailFlag::f_message)),
					peerToMTP(session->userPeerId()),
					peerToMTP(record.recipient),
					MTP_int(gift.date),
					MTP_textWithEntities(MTP_string(gift.message.text),
						MTPVector<MTPMessageEntity>())));
				using GiftFlag = MTPDstarGiftUnique::Flag;
				const auto local = MTP_starGiftUnique(
					MTP_flags(GiftFlag::f_owner_id),
					original.vid(),
					original.vgift_id(),
					original.vtitle(),
					original.vslug(),
					original.vnum(),
					peerToMTP(record.recipient),
					MTP_string(QString()),
					MTP_string(QString()),
					MTP_vector<MTPStarGiftAttribute>(attributes),
					original.vavailability_issued(),
					original.vavailability_total(),
					MTP_string(QString()),
					MTPVector<MTPStarsAmount>(),
					MTPPeer(),
					MTP_long(0),
					MTP_string(QString()),
					MTP_long(0),
					MTPPeer(),
					MTPPeerColor(),
					MTPPeer(),
					MTP_int(0),
					MTP_int(0));
				using Flag = MTPDmessageActionStarGiftUnique::Flag;
				return MTP_messageActionStarGiftUnique(
					MTP_flags(Flag::f_saved
						| (gift.anonymous ? Flag() : Flag::f_from_id)),
					local,
					MTP_int(0),
					MTP_long(0),
					peerToMTP(session->userPeerId()),
					MTPPeer(),
					MTP_long(0),
					MTPStarsAmount(),
					MTP_int(0),
					MTP_int(0),
					MTP_long(0),
					MTP_int(0));
			}
			using Flag = MTPDmessageActionStarGift::Flag;
			return MTP_messageActionStarGift(
				MTP_flags(Flag::f_saved
					| (gift.anonymous ? Flag::f_name_hidden : Flag())
					| (gift.message.empty() ? Flag() : Flag::f_message)),
				record.source,
				MTP_textWithEntities(MTP_string(gift.message.text),
					MTPVector<MTPMessageEntity>()),
				MTP_long(0),
				MTP_int(0),
				MTP_long(0),
				MTPPeer(),
				MTPPeer(),
				MTP_long(0),
				MTP_string(QString()),
				MTP_int(0),
				MTPPeer(),
				MTP_int(0));
		}();
		using Flag = MTPDmessageService::Flag;
		const auto message = MTP_messageService(
			MTP_flags(Flag::f_from_id | Flag::f_out),
			MTP_int(0),
			peerToMTP(session->userPeerId()),
			peerToMTP(record.recipient),
			MTPPeer(),
			MTPMessageReplyHeader(),
			MTP_int(gift.date),
			action,
			MTPMessageReactions(),
			MTPint());
		const auto history = session->data().history(record.recipient);
		const auto item = history->makeMessage(
			id,
			message.c_messageService(),
			MessageFlag::Local | MessageFlag::HistoryEntry);
		history->addNewLocalMessage(item);
	}
}

void State::refreshResources() {
	if (resourcesRequested || records.empty() || !enabled.current()) {
		return;
	}
	resourcesRequested = true;
	api.request(MTPpayments_GetStarGifts(MTP_int(0))).done([=](
			const MTPpayments_StarGifts &result) {
		if (result.type() == mtpc_payments_starGifts) {
			for (const auto &source : result.c_payments_starGifts().vgifts().v) {
				(void)Api::FromTL(session, source);
			}
		}
	}).send();
	auto slugs = base::flat_set<QString>();
	auto sampleIds = base::flat_set<uint64>();
	for (const auto &record : records) {
		const auto &unique = record.gift.info.unique;
		if (!unique) {
			continue;
		} else if (unique->slug.startsWith(u"Visual-")) {
			sampleIds.emplace(unique->initialGiftId);
		} else {
			slugs.emplace(unique->slug);
		}
	}
	for (const auto &slug : slugs) {
		api.request(MTPpayments_GetUniqueStarGift(MTP_string(slug))).done([=](
				const MTPpayments_UniqueStarGift &result) {
			(void)Api::FromTL(session, result.data().vgift());
		}).send();
	}
	for (const auto id : sampleIds) {
		api.request(MTPpayments_GetResaleStarGifts(
			MTP_flags(MTPpayments_GetResaleStarGifts::Flag::f_attributes_hash),
			MTP_long(0),
			MTP_long(id),
			MTPVector<MTPStarGiftAttributeId>(),
			MTP_string(QString()),
			MTP_int(1)
		)).done([=](const MTPpayments_ResaleStarGifts &result) {
			const auto attributes = result.data().vattributes();
			if (!attributes) {
				return;
			}
			for (const auto &attribute : attributes->v) {
				if (attribute.type() == mtpc_starGiftAttributeModel) {
					(void)Api::FromTL(session, attribute.c_starGiftAttributeModel());
				} else if (attribute.type() == mtpc_starGiftAttributePattern) {
					(void)Api::FromTL(session, attribute.c_starGiftAttributePattern());
				}
			}
		}).send();
	}
}

[[nodiscard]] State &Get(not_null<Main::Session*> session) {
	static auto states = std::map<Main::Session*, std::unique_ptr<State>>();
	const auto found = states.find(session);
	if (found != end(states)) {
		return *found->second;
	}
	auto state = std::make_unique<State>(session);
	const auto result = state.get();
	states.emplace(session, std::move(state));
	session->lifetime().add([session] { states.erase(session); });
	return *result;
}

[[nodiscard]] auto Find(State &state, Data::SavedStarGiftId id) {
	return ranges::find_if(state.records, [&](const Record &record) {
		return record.gift.manageId == id;
	});
}

}

QString Text(QString english, QString russian) {
	const auto &language = Lang::GetInstance();
	return (language.id().startsWith(u"ru")
		|| language.baseId().startsWith(u"ru"))
		? std::move(russian)
		: std::move(english);
}

rpl::producer<QString> TextValue(QString english, QString russian) {
	return rpl::single(rpl::empty) | rpl::then(
		Lang::GetInstance().updated()
	) | rpl::map([=] { return Text(english, russian); });
}

bool Enabled(not_null<Main::Session*> session) {
	return Get(session).enabled.current();
}

rpl::producer<bool> EnabledValue(not_null<Main::Session*> session) {
	return Get(session).enabled.value();
}

rpl::producer<> Changes(not_null<Main::Session*> session) {
	return Get(session).changes.events();
}

bool SetEnabled(not_null<Main::Session*> session, bool enabled) {
	auto &state = Get(session);
	const auto previous = state.enabled.current();
	state.enabled = enabled;
	if (!state.save()) {
		state.enabled = previous;
		return false;
	}
	if (enabled) {
		state.refreshResources();
		state.restoreMessages();
	} else {
		for (const auto &record : state.records) {
			state.removeMessage(record);
		}
	}
	state.notify();
	return true;
}

QString Phone(not_null<Main::Session*> session) {
	return Get(session).phone;
}

QStringList Usernames(not_null<Main::Session*> session) {
	return Get(session).usernames;
}

bool SetProfile(
		not_null<Main::Session*> session,
		QString phone,
		QStringList usernames) {
	auto &state = Get(session);
	const auto previousPhone = state.phone;
	const auto previousUsernames = state.usernames;
	state.phone = std::move(phone);
	state.usernames = std::move(usernames);
	if (!state.save()) {
		state.phone = previousPhone;
		state.usernames = previousUsernames;
		return false;
	}
	state.notify();
	return true;
}

std::vector<Data::SavedStarGift> Gifts(
		not_null<PeerData*> peer,
		bool pinnedOnly) {
	auto result = std::vector<Data::SavedStarGift>();
	auto &state = Get(&peer->session());
	if (!state.enabled.current()) {
		return result;
	}
	for (const auto &record : state.records) {
		if (record.recipient == peer->id
			&& (peer->isSelf() || !record.gift.hidden)
			&& (!pinnedOnly || (record.gift.pinned && !record.gift.hidden))) {
			result.push_back(record.gift);
		}
	}
	return result;
}

bool IsLocal(not_null<Main::Session*> session, Data::SavedStarGiftId id) {
	auto &state = Get(session);
	return Find(state, id) != end(state.records);
}

std::optional<Data::SavedStarGift> FindGift(
		not_null<Main::Session*> session,
		Data::SavedStarGiftId id) {
	auto &state = Get(session);
	const auto found = Find(state, id);
	return (state.enabled.current() && found != end(state.records))
		? std::make_optional(found->gift)
		: std::nullopt;
}

std::optional<Data::SavedStarGift> AddGift(
		not_null<PeerData*> recipient,
		const MTPStarGift &source,
		QString message,
		bool anonymous) {
	auto &state = Get(&recipient->session());
	const auto info = Api::FromTL(state.session, source);
	if (!state.enabled.current() || !info
		|| state.records.size() + state.unreadableRecords.size() >= kMaxGifts) {
		return {};
	}
	auto gift = LocalGift(
		state.session,
		*info,
		recipient->id,
		std::move(message),
		base::unixtime::now(),
		anonymous);
	state.records.push_back({ source, gift, recipient->id });
	if (!state.save()) {
		state.records.pop_back();
		return {};
	}
	state.restoreMessages();
	state.notify();
	return gift;
}

bool SetPinned(
		not_null<Main::Session*> session,
		Data::SavedStarGiftId id,
		bool pinned) {
	auto &state = Get(session);
	const auto found = Find(state, id);
	if (found == end(state.records) || !found->gift.info.unique) {
		return false;
	}
	const auto count = ranges::count_if(state.records, [&](const Record &record) {
		return record.recipient == found->recipient && record.gift.pinned;
	});
	if (pinned && !found->gift.pinned && count >= kMaxPinned) {
		return false;
	}
	const auto previous = found->gift;
	found->gift.pinned = pinned;
	if (pinned) {
		found->gift.hidden = false;
	}
	if (!state.save()) {
		found->gift = previous;
		return false;
	}
	state.notify();
	return true;
}

bool SetHidden(
		not_null<Main::Session*> session,
		Data::SavedStarGiftId id,
		bool hidden) {
	auto &state = Get(session);
	const auto found = Find(state, id);
	if (found == end(state.records)) {
		return false;
	}
	const auto previous = found->gift;
	found->gift.hidden = hidden;
	if (hidden) {
		found->gift.pinned = false;
	}
	if (!state.save()) {
		found->gift = previous;
		return false;
	}
	state.notify();
	return true;
}

bool RemoveGift(not_null<Main::Session*> session, Data::SavedStarGiftId id) {
	auto &state = Get(session);
	const auto found = Find(state, id);
	if (found == end(state.records)) {
		return false;
	}
	const auto index = found - begin(state.records);
	const auto record = *found;
	state.records.erase(found);
	if (!state.save()) {
		state.records.insert(begin(state.records) + index, record);
		return false;
	}
	state.removeMessage(record);
	state.notify();
	return true;
}

void RestoreMessages(not_null<Main::Session*> session) {
	auto &state = Get(session);
	state.refreshResources();
	state.restoreMessages();
}

}
