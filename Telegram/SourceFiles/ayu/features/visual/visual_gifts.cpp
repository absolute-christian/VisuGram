#include "ayu/features/visual/visual_gifts.h"

#include "api/api_premium.h"
#include "base/unixtime.h"
#include "data/components/credits.h"
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
#include <QtCore/QTimer>
#include <QtCore/QUuid>
#include <QtNetwork/QNetworkAccessManager>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QNetworkRequest>

namespace Ayu::Visual {
namespace {

constexpr auto kVersion = 1;
constexpr auto kMaxFileSize = 16 * 1024 * 1024;
constexpr auto kMaxGifts = 1000;
constexpr auto kMaxPinned = 6;
constexpr auto kResourceRetryDelay = crl::time(60 * 1000);

struct Record {
	MTPStarGift source;
	Data::SavedStarGift gift;
	PeerId recipient;
	QByteArray encodedSource;
	CreditsAmount price;
	PeerId sender;
	QString serverId;
	bool active = true;
	bool resourcesRequested = false;
	crl::time resourceAttempt = 0;
};

class State final {
public:
	explicit State(not_null<Main::Session*> session);
	[[nodiscard]] bool save();
	void notify();
	void loadRecords();
	void restoreMessages(History *only = nullptr);
	void restoreMessage(
		const Record &record,
		not_null<History*> history,
		bool newlySent = false);
	void refreshGift(Data::SavedStarGiftId id);
	void requestNextResource();
	void removeMessage(const Record &record);
	void request(QString route, QJsonObject body,
		Fn<void(QJsonObject, QString)> done);
	void sync();
	void view(not_null<PeerData*> peer);
	void acceptGift(QJsonObject object, bool newlySent = false);
	void applyServerGifts();
	[[nodiscard]] QJsonObject profile(not_null<PeerData*> peer);

	const not_null<Main::Session*> session;
	MTP::Sender api;
	rpl::variable<bool> enabled = false;
	rpl::event_stream<> changes;
	QString phone;
	QStringList usernames;
	std::vector<Record> records;
	std::map<MsgId, size_t> recordIndex;
	QJsonArray unreadableRecords;
	QJsonArray pendingRecords;
	std::vector<Data::SavedStarGiftId> resourceQueue;
	std::vector<mtpRequestId> resourceRequests;
	base::flat_set<PeerId> restoredPeers;
	int resourcesLoading = 0;
	bool restoring = false;
	bool writable = true;
	bool catalogRefreshed = false;
	QNetworkAccessManager network;
	QTimer syncTimer;
	QString endpoint;
	QString cursor;
	QString primary;
	QJsonObject ownProfile;
	QJsonArray serverGifts;
	QJsonArray pendingServerGifts;
	std::map<QString, QJsonObject> serverVersions;
	std::map<PeerId, QJsonObject> remoteProfiles;
	std::map<PeerId, crl::time> remoteAttempts;
	base::flat_set<PeerId> remoteLoading;
	PeerId watchedPeer;
	bool syncing = false;
	bool profileSaving = false;
	int requestsLoading = 0;

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
			.message = TextWithEntities{ message },
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
	QObject::connect(&syncTimer, &QTimer::timeout, &network, [this] {
		sync();
		if (watchedPeer && enabled.current()) {
			view(this->session->data().peer(watchedPeer));
		}
	});
	syncTimer.start(60 * 1000);
	QTimer::singleShot(0, &network, [this] { sync(); });
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
	endpoint = root.value(u"sync_server"_q).toString();
	cursor = root.value(u"sync_cursor"_q).toString();
	ownProfile = root.value(u"sync_profile"_q).toObject();
	serverGifts = root.value(u"server_gifts"_q).toArray();
	pendingServerGifts = serverGifts;
	primary = root.value(u"primary"_q).toString();
	phone = root.value(u"phone"_q).toString();
	for (const auto &name : root.value(u"usernames"_q).toArray()) {
		usernames.push_back(name.toString());
	}
	const auto gifts = root.value(u"gifts"_q).toArray();
	if (gifts.size() > kMaxGifts) {
		writable = false;
		return;
	}
	pendingRecords = gifts;
	enabled = root.value(u"enabled"_q).toBool();
}

void State::loadRecords() {
	applyServerGifts();
	const auto gifts = std::exchange(pendingRecords, QJsonArray());
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
		if (!gift.mine) {
			gift.pinned = false;
			gift.hidden = false;
		}
		recordIndex.emplace(gift.manageId.userMessageId(), records.size());
		records.push_back({
			*source,
			std::move(gift),
			recipient,
			object.value(u"source"_q).toString().toLatin1(),
			CreditsAmount(
				object.value(u"price_whole"_q).toString().toLongLong(),
				object.value(u"price_nano"_q).toInt(),
				object.value(u"price_ton"_q).toBool()
					? CreditsType::Ton
					: CreditsType::Stars),
		});
	}
}

bool State::save() {
	if (!writable) {
		return false;
	}
	auto gifts = unreadableRecords;
	for (const auto &value : pendingRecords) {
		gifts.push_back(value);
	}
	for (const auto &record : records) {
		if (!record.serverId.isEmpty()) {
			continue;
		}
		const auto &gift = record.gift;
		gifts.push_back(QJsonObject{
			{ u"source"_q, QString::fromLatin1(record.encodedSource) },
			{ u"recipient"_q, QString::number(record.recipient.value) },
			{ u"message"_q, gift.message.text },
			{ u"date"_q, int(gift.date) },
			{ u"anonymous"_q, gift.anonymous },
			{ u"pinned"_q, gift.pinned },
			{ u"hidden"_q, gift.hidden },
			{ u"price_whole"_q, QString::number(record.price.whole()) },
			{ u"price_nano"_q, int(record.price.nano()) },
			{ u"price_ton"_q, record.price.ton() },
		});
	}
	const auto bytes = QJsonDocument(QJsonObject{
		{ u"version"_q, kVersion },
		{ u"account"_q, QString::number(session->uniqueId()) },
		{ u"enabled"_q, enabled.current() },
		{ u"sync_server"_q, endpoint },
		{ u"sync_cursor"_q, cursor },
		{ u"sync_profile"_q, ownProfile },
		{ u"server_gifts"_q, serverGifts },
		{ u"primary"_q, primary },
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
	const auto chat = (record.recipient == session->userPeerId() && record.sender)
		? record.sender : record.recipient;
	const auto id = FullMsgId(chat, record.gift.manageId.userMessageId());
	if (const auto item = session->data().message(id)) {
		item->destroy();
	}
}

void State::restoreMessages(History *only) {
	if (!enabled.current() || restoring
		|| (only && restoredPeers.contains(only->peer->id))) {
		return;
	}
	loadRecords();
	restoring = true;
	auto histories = base::flat_set<History*>();
	for (const auto &record : records) {
		if (!record.serverId.isEmpty() && record.sender != session->userPeerId()
			&& record.recipient != session->userPeerId()) {
			continue;
		}
		const auto chat = (record.recipient == session->userPeerId() && record.sender)
			? record.sender : record.recipient;
		const auto history = session->data().historyLoaded(chat);
		if (!history || (only && history != only)) {
			continue;
		}
		if (!history->isEmpty()
			|| (history->loadedAtTop() && history->loadedAtBottom())) {
			restoreMessage(record, history);
			histories.emplace(history);
		}
	}
	for (const auto history : histories) {
		history->checkLocalMessages();
	}
	if (only && (!only->isEmpty()
		|| (only->loadedAtTop() && only->loadedAtBottom()))) {
		restoredPeers.emplace(only->peer->id);
	}
	restoring = false;
}

void State::restoreMessage(
		const Record &record,
		not_null<History*> history,
		bool newlySent) {
	const auto &gift = record.gift;
	const auto id = gift.manageId.userMessageId();
	if (session->data().message(FullMsgId(history->peer->id, id))) {
		return;
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
				peerToMTP(record.sender ? record.sender : session->userPeerId()),
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
				MTP_flags(Flag::f_saved | Flag::f_peer
					| (gift.anonymous ? Flag() : Flag::f_from_id)
					| (record.price ? Flag::f_resale_amount : Flag())),
				local,
				MTP_int(0),
				MTP_long(0),
				peerToMTP(record.sender ? record.sender : session->userPeerId()),
				peerToMTP(record.recipient),
				MTP_long(0),
				StarsAmountToTL(record.price),
				MTP_int(0),
				MTP_int(0),
				MTP_long(0),
				MTP_int(0));
		}
		using Flag = MTPDmessageActionStarGift::Flag;
		return MTP_messageActionStarGift(
			MTP_flags(Flag::f_saved
				| (gift.anonymous ? Flag::f_name_hidden : Flag())
				| Flag::f_from_id | Flag::f_peer
				| (gift.message.empty() ? Flag() : Flag::f_message)),
			record.source,
			MTP_textWithEntities(MTP_string(gift.message.text),
				MTPVector<MTPMessageEntity>()),
			MTP_long(0),
			MTP_int(0),
			MTP_long(0),
			peerToMTP(record.sender ? record.sender : session->userPeerId()),
			peerToMTP(record.recipient),
			MTP_long(0),
			MTP_string(QString()),
			MTP_int(0),
			MTPPeer(),
			MTP_int(0));
	}();
	using Flag = MTPDmessageService::Flag;
	const auto message = MTP_messageService(
		MTP_flags(Flag::f_from_id | ((record.sender && record.sender != session->userPeerId())
			? Flag() : Flag::f_out)),
		MTP_int(0),
		peerToMTP(record.sender ? record.sender : session->userPeerId()),
		peerToMTP(history->peer->id),
		MTPPeer(),
		MTPMessageReplyHeader(),
		MTP_int(gift.date),
		action,
		MTPMessageReactions(),
		MTPint());
	const auto item = history->makeMessage(
		id,
		message.c_messageService(),
		MessageFlag::Local | MessageFlag::HistoryEntry);
	if (newlySent) {
		history->addNewLocalMessage(item);
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

void State::request(
		QString route,
		QJsonObject body,
		Fn<void(QJsonObject, QString)> done) {
	if (endpoint.isEmpty() || requestsLoading >= 4) {
		done({}, endpoint.isEmpty() ? u"SERVER_REQUIRED"_q : u"BUSY"_q);
		return;
	}
	auto request = QNetworkRequest(QUrl(endpoint + route));
	request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_q);
	request.setRawHeader("X-VisuGram-User-Id",
		QByteArray::number(peerToUser(session->userPeerId()).bare));
	request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
		QNetworkRequest::ManualRedirectPolicy);
	request.setTransferTimeout(45000);
	const auto expectedEndpoint = endpoint;
	const auto reply = network.post(request,
		QJsonDocument(body).toJson(QJsonDocument::Compact));
	++requestsLoading;
	QObject::connect(reply, &QNetworkReply::readyRead, &network, [reply] {
		if (reply->bytesAvailable() > kMaxFileSize) {
			reply->abort();
		}
	});
	QObject::connect(reply, &QNetworkReply::finished, &network, [=] {
		--requestsLoading;
		const auto status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		const auto failed = reply->error() != QNetworkReply::NoError;
		const auto bytes = reply->readAll();
		reply->deleteLater();
		auto parseError = QJsonParseError();
		const auto document = QJsonDocument::fromJson(bytes, &parseError);
		const auto object = document.object();
		if (endpoint != expectedEndpoint) {
			done({}, u"ENDPOINT_CHANGED"_q);
			return;
		}
		if (bytes.size() > kMaxFileSize || parseError.error != QJsonParseError::NoError
			|| !document.isObject()) {
			done({}, u"SERVER_UNAVAILABLE"_q);
		} else if (failed || status != 200) {
			done({}, object.value(u"error"_q).toString(u"SERVER_UNAVAILABLE"_q));
		} else {
			done(object, {});
		}
	});
}

void State::sync() {
	if (endpoint.isEmpty() || syncing || profileSaving) {
		return;
	}
	syncing = true;
	request(u"/v1/sync"_q, {
		{ u"cursor"_q, cursor },
		{ u"telegram_username"_q, session->user()->editableUsername() },
	}, [=](QJsonObject object, QString error) {
		syncing = false;
		if (!error.isEmpty() || object.value(u"unchanged"_q).toBool()) {
			return;
		}
		const auto previousProfile = ownProfile;
		const auto previousGifts = serverGifts;
		const auto previousCursor = cursor;
		ownProfile = object.value(u"profile"_q).toObject();
		serverGifts = object.value(u"gifts"_q).toArray();
		cursor = object.value(u"cursor"_q).toString();
		if (!save()) {
			ownProfile = previousProfile;
			serverGifts = previousGifts;
			cursor = previousCursor;
			return;
		}
		pendingServerGifts = serverGifts;
		if (enabled.current()) {
			applyServerGifts();
			restoredPeers.clear();
			restoreMessages();
		}
		notify();
	});
}

QJsonObject State::profile(not_null<PeerData*> peer) {
	if (!enabled.current()) {
		return {};
	}
	if (peer->isSelf()) {
		return ownProfile;
	}
	view(peer);
	const auto found = remoteProfiles.find(peer->id);
	return found != end(remoteProfiles)
		? found->second.value(u"profile"_q).toObject()
		: QJsonObject();
}

void State::view(not_null<PeerData*> peer) {
	if (peer->isUser() && !peer->isSelf()) {
		watchedPeer = peer->id;
	}
	if (!enabled.current() || endpoint.isEmpty() || !peer->isUser()
		|| peer->isSelf() || remoteLoading.contains(peer->id)) {
		return;
	}
	const auto now = crl::now();
	const auto found = remoteAttempts.find(peer->id);
	if (found != end(remoteAttempts) && now - found->second < 60 * 1000) {
		return;
	}
	if (remoteAttempts.size() >= 32 && found == end(remoteAttempts)) {
		const auto oldest = ranges::min_element(remoteAttempts,
			[](const auto &a, const auto &b) { return a.second < b.second; });
		remoteProfiles.erase(oldest->first);
		remoteAttempts.erase(oldest);
	}
	remoteAttempts[peer->id] = now;
	remoteLoading.emplace(peer->id);
	request(u"/v1/profile/view"_q, {
		{ u"owner_id"_q, QString::number(peerToUser(peer->id).bare) },
	}, [=](QJsonObject object, QString error) {
		remoteLoading.erase(peer->id);
		if (!error.isEmpty()) {
			return;
		}
		remoteProfiles[peer->id] = std::move(object);
		notify();
	});
}

void State::acceptGift(QJsonObject object, bool newlySent) {
	const auto id = object.value(u"id"_q).toString();
	const auto previous = serverVersions.find(id);
	if (object.value(u"sender_id"_q).toString() == u"0" && previous != end(serverVersions)) {
		object.insert(u"sender_id"_q, previous->second.value(u"sender_id"_q));
	}
	if (id.isEmpty() || object == serverVersions[id]) {
		return;
	}
	const auto source = Decode(object.value(u"source"_q).toString().toLatin1());
	const auto parsed = source ? Api::FromTL(session, *source) : std::nullopt;
	if (!parsed) {
		return;
	}
	const auto recipient = peerFromUser(UserId(
		object.value(u"recipient_id"_q).toString().toULongLong()));
	const auto sender = peerFromUser(UserId(
		object.value(u"sender_id"_q).toString().toULongLong()));
	if (!recipient) {
		return;
	}
	auto gift = LocalGift(session, *parsed, recipient,
		object.value(u"message"_q).toString(),
		object.value(u"date"_q).toInt(),
		object.value(u"anonymous"_q).toBool());
	gift.fromId = gift.anonymous ? PeerId() : sender;
	if (gift.info.unique) {
		gift.info.unique->originalDetails.senderId = gift.fromId;
	}
	const auto active = object.value(u"active"_q).toBool(true);
	gift.mine = recipient == session->userPeerId() && active;
	gift.pinned = object.value(u"pinned"_q).toBool() && active;
	gift.hidden = object.value(u"hidden"_q).toBool();
	const auto amount = object.value(u"price"_q).toString().toLongLong();
	const auto ton = object.value(u"currency"_q).toString() == u"TON";
	const auto price = ton
		? CreditsAmount(amount / 1'000'000'000, amount % 1'000'000'000, CreditsType::Ton)
		: CreditsAmount(amount);
	auto found = ranges::find(records, id, &Record::serverId);
	if (found == end(records) && records.size() >= kMaxGifts) {
		for (auto i = records.size(); i != 0; ) {
			--i;
			const auto &record = records[i];
			if (!record.serverId.isEmpty() && record.sender != session->userPeerId()
				&& record.recipient != session->userPeerId() && record.recipient != recipient) {
				serverVersions.erase(record.serverId);
				records.erase(begin(records) + i);
			}
		}
		recordIndex.clear();
		for (auto i = size_t(0); i != records.size(); ++i) {
			recordIndex.emplace(records[i].gift.manageId.userMessageId(), i);
		}
		found = end(records);
	}
	if (found != end(records)) {
		gift.manageId = found->gift.manageId;
		removeMessage(*found);
		*found = Record{ *source, gift, recipient, Encode(*source), price, sender, id, active };
	} else {
		if (records.size() >= kMaxGifts) {
			return;
		}
		recordIndex.emplace(gift.manageId.userMessageId(), records.size());
		records.push_back({ *source, gift, recipient, Encode(*source), price, sender, id, active });
	}
	serverVersions[id] = std::move(object);
	if (enabled.current() && newlySent) {
		const auto record = ranges::find(records, id, &Record::serverId);
		const auto chat = recipient == session->userPeerId() ? sender : recipient;
		restoreMessage(*record, session->data().history(chat), true);
	}
}

void State::applyServerGifts() {
	if (!enabled.current()) {
		return;
	}
	const auto gifts = std::exchange(pendingServerGifts, QJsonArray());
	for (const auto &value : gifts) {
		acceptGift(value.toObject());
	}
}

[[nodiscard]] auto Find(State &state, Data::SavedStarGiftId id) {
	const auto index = state.recordIndex.find(id.userMessageId());
	return (index != end(state.recordIndex)
		&& state.records[index->second].gift.manageId == id)
		? begin(state.records) + index->second
		: end(state.records);
}

void State::refreshGift(Data::SavedStarGiftId id) {
	if (!enabled.current()) {
		return;
	}
	loadRecords();
	const auto found = Find(*this, id);
	if (found == end(records) || found->resourcesRequested
		|| (found->resourceAttempt
			&& crl::now() - found->resourceAttempt < kResourceRetryDelay)) {
		return;
	}
	found->resourcesRequested = true;
	found->resourceAttempt = crl::now();
	if (!found->gift.info.unique) {
		if (catalogRefreshed) {
			return;
		}
		catalogRefreshed = true;
	}
	resourceQueue.push_back(id);
	requestNextResource();
}

void State::requestNextResource() {
	while (enabled.current() && resourcesLoading < 2 && !resourceQueue.empty()) {
		const auto id = resourceQueue.front();
		resourceQueue.erase(begin(resourceQueue));
		const auto found = Find(*this, id);
		if (found == end(records)) {
			continue;
		}
		++resourcesLoading;
		const auto finish = [=](bool success) {
			--resourcesLoading;
			if (!success) {
				const auto found = Find(*this, id);
				if (found != end(records)) {
					found->resourcesRequested = false;
				}
			}
			requestNextResource();
		};
		const auto failed = [=](const MTP::Error &) { finish(false); };
		const auto &unique = found->gift.info.unique;
		if (unique && !unique->slug.startsWith(u"Visual-")) {
			resourceRequests.push_back(api.request(MTPpayments_GetUniqueStarGift(
				MTP_string(unique->slug)
			)).done([=](const MTPpayments_UniqueStarGift &result) {
				const auto &source = result.data().vgift();
				(void)Api::FromTL(session, source);
				const auto found = Find(*this, id);
				if (found != end(records)) {
					found->source = source;
					found->encodedSource = Encode(source);
				}
				finish(true);
			}).fail(failed).send());
		} else if (unique) {
			using Flag = MTPpayments_GetResaleStarGifts::Flag;
			resourceRequests.push_back(api.request(MTPpayments_GetResaleStarGifts(
				MTP_flags(Flag::f_attributes_hash),
				MTP_long(0),
				MTP_long(unique->initialGiftId),
				MTPVector<MTPStarGiftAttributeId>(),
				MTP_string(QString()),
				MTP_int(1)
			)).done([=](const MTPpayments_ResaleStarGifts &result) {
				const auto attributes = result.data().vattributes();
				if (attributes) {
					for (const auto &attribute : attributes->v) {
						if (attribute.type() == mtpc_starGiftAttributeModel) {
							(void)Api::FromTL(session, attribute.c_starGiftAttributeModel());
						} else if (attribute.type() == mtpc_starGiftAttributePattern) {
							(void)Api::FromTL(session, attribute.c_starGiftAttributePattern());
						}
					}
				}
				finish(true);
			}).fail(failed).send());
		} else {
			resourceRequests.push_back(api.request(MTPpayments_GetStarGifts(
				MTP_int(0)
			)).done([=](const MTPpayments_StarGifts &result) {
				if (result.type() == mtpc_payments_starGifts) {
					for (const auto &source : result.c_payments_starGifts().vgifts().v) {
						(void)Api::FromTL(session, source);
					}
				}
				finish(true);
			}).fail([=](const MTP::Error &) {
				catalogRefreshed = false;
				finish(false);
			}).send());
		}
	}
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
	if (previous == enabled) {
		return true;
	}
	state.enabled = enabled;
	if (!state.save()) {
		state.enabled = previous;
		return false;
	}
	if (enabled) {
		state.restoreMessages();
	} else {
		for (const auto request : state.resourceRequests) {
			state.api.request(request).cancel();
		}
		state.resourceRequests.clear();
		state.resourceQueue.clear();
		state.resourcesLoading = 0;
		state.catalogRefreshed = false;
		state.restoredPeers.clear();
		for (auto &record : state.records) {
			record.resourcesRequested = false;
			record.resourceAttempt = 0;
			state.removeMessage(record);
		}
	}
	state.notify();
	return true;
}

QString Phone(not_null<Main::Session*> session) {
	auto &state = Get(session);
	return state.ownProfile.isEmpty() ? state.phone
		: state.ownProfile.value(u"phone"_q).toString();
}

QStringList Usernames(not_null<Main::Session*> session) {
	auto &state = Get(session);
	if (state.ownProfile.isEmpty()) {
		return state.usernames;
	}
	auto result = QStringList();
	for (const auto &name : state.ownProfile.value(u"usernames"_q).toArray()) {
		result.push_back(name.toString());
	}
	return result;
}

QString SyncServer(not_null<Main::Session*> session) {
	return Get(session).endpoint;
}

bool SetSyncServer(not_null<Main::Session*> session, QString endpoint) {
	endpoint = endpoint.trimmed();
	while (endpoint.endsWith('/')) {
		endpoint.chop(1);
	}
	const auto url = QUrl(endpoint);
	if (!endpoint.isEmpty() && (!url.isValid() || url.host().isEmpty()
		|| !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()
		|| (url.scheme() != u"https"
			&& !(url.scheme() == u"http" && (url.host() == u"127.0.0.1"
				|| url.host() == u"localhost"))))) {
		return false;
	}
	auto &state = Get(session);
	if (state.endpoint == endpoint) {
		return true;
	}
	const auto previous = state.endpoint;
	state.endpoint = std::move(endpoint);
	if (!state.save()) {
		state.endpoint = previous;
		return false;
	}
	for (const auto &record : state.records) {
		if (!record.serverId.isEmpty()) {
			state.removeMessage(record);
		}
	}
	state.records.erase(std::remove_if(begin(state.records), end(state.records),
		[](const Record &record) { return !record.serverId.isEmpty(); }), end(state.records));
	state.recordIndex.clear();
	for (auto i = size_t(0); i != state.records.size(); ++i) {
		state.recordIndex.emplace(state.records[i].gift.manageId.userMessageId(), i);
	}
	state.cursor.clear();
	state.ownProfile = {};
	state.serverGifts = {};
	state.pendingServerGifts = {};
	state.serverVersions.clear();
	state.remoteProfiles.clear();
	state.remoteAttempts.clear();
	state.watchedPeer = PeerId();
	state.restoredPeers.clear();
	(void)state.save();
	state.notify();
	QTimer::singleShot(0, &state.network, [&state] { state.sync(); });
	return true;
}

QString PhoneFor(not_null<PeerData*> peer) {
	auto &state = Get(&peer->session());
	if (!state.enabled.current()) {
		return {};
	}
	return peer->isSelf() ? Phone(&peer->session())
		: state.profile(peer).value(u"phone"_q).toString();
}

bool IsVisualUsername(not_null<PeerData*> peer, QString name) {
	auto &state = Get(&peer->session());
	if (!state.enabled.current()) {
		return false;
	}
	const auto profile = state.profile(peer);
	auto names = peer->isSelf() ? Usernames(&peer->session()) : QStringList();
	if (!peer->isSelf()) {
		for (const auto &value : profile.value(u"usernames"_q).toArray()) {
			names.push_back(value.toString());
		}
	}
	return names.contains(name, Qt::CaseInsensitive)
		&& peer->asUser()
		&& !ranges::contains(peer->asUser()->usernames(), name);
}

QStringList DisplayUsernames(not_null<PeerData*> peer) {
	const auto user = peer->asUser();
	if (!user) {
		return {};
	}
	auto names = QStringList();
	for (const auto &name : user->usernames()) {
		names.push_back(name);
	}
	if (names.isEmpty() && !user->username().isEmpty()) {
		names.push_back(user->username());
	}
	auto &state = Get(&peer->session());
	if (!state.enabled.current()) {
		return names;
	}
	const auto profile = state.profile(peer);
	const auto visual = peer->isSelf() ? Usernames(&peer->session()) : [&] {
		auto result = QStringList();
		for (const auto &value : profile.value(u"usernames"_q).toArray()) {
			result.push_back(value.toString());
		}
		return result;
	}();
	for (const auto &name : visual) {
		if (!names.contains(name, Qt::CaseInsensitive)) {
			names.push_back(name);
		}
	}
	const auto primary = profile.isEmpty() ? state.primary
		: profile.value(u"primary"_q).toString();
	const auto index = names.indexOf(primary);
	if (index > 0) {
		names.move(index, 0);
	}
	return names;
}

QJsonObject CollectibleMetadata(not_null<PeerData*> peer, QString entity) {
	auto &state = Get(&peer->session());
	const auto profile = peer->isSelf() ? state.ownProfile : state.profile(peer);
	const auto key = entity.startsWith('+')
		? u"phone:"_q + entity : u"username:"_q + entity.toLower().remove('@');
	return profile.value(u"collectibles"_q).toObject().value(key).toObject();
}

QString SyncError(QString code) {
	if (code == u"ASSET_TAKEN") {
		return Text(u"This collectible already belongs to another VisuGram user."_q,
			u"Этот коллекционный объект уже занят другим пользователем VisuGram."_q);
	} else if (code == u"PHONE_NOT_FOUND" || code == u"INVALID_PHONE") {
		return Text(u"Enter an existing collectible +888 number."_q,
			u"Введите существующий коллекционный номер +888."_q);
	} else if (code == u"SERVER_REQUIRED") {
		return Text(u"Configure the visual sync server in Edit Profile first."_q,
			u"Сначала укажите сервер синхронизации в разделе «Редактировать профиль»."_q);
	} else if (code == u"RECIPIENT_NOT_CONNECTED") {
		return Text(u"The recipient has not connected to this VisuGram server yet."_q,
			u"Получатель ещё не подключился к этому серверу VisuGram."_q);
	} else if (code == u"PROFILE_CHANGED") {
		return Text(u"Your profile changed on another device. Reopen the editor."_q,
			u"Профиль изменён на другом устройстве. Откройте редактор заново."_q);
	} else if (code == u"PIN_LIMIT") {
		return Text(u"Up to six gifts can be pinned."_q, u"Можно закрепить до шести подарков."_q);
	} else if (code == u"NOT_GIFT_OWNER") {
		return Text(u"Only the recipient can manage this gift."_q,
			u"Управлять подарком может только его получатель."_q);
	} else if (code == u"STORAGE_ERROR") {
		return Text(u"Could not save the local data."_q, u"Не удалось сохранить локальные данные."_q);
	}
	return Text(u"The request could not be confirmed. Check synchronization before retrying."_q,
		u"Не удалось подтвердить запрос. Проверьте синхронизацию перед повторной попыткой."_q);
}

void SaveProfile(
		not_null<Main::Session*> session,
		QString phone,
		QStringList names,
		QString primary,
		Fn<void(QString)> done) {
	auto &state = Get(session);
	if (state.profileSaving || state.syncing) {
		done(u"BUSY"_q);
		return;
	}
	state.profileSaving = true;
	state.request(u"/v1/profile"_q, {
		{ u"phone"_q, phone },
		{ u"usernames"_q, QJsonArray::fromStringList(names) },
		{ u"primary"_q, primary },
		{ u"revision"_q, state.ownProfile.value(u"revision"_q).toInt() },
		{ u"operation_id"_q, QUuid::createUuid().toString(QUuid::WithoutBraces) },
	}, [&state, done = std::move(done)](QJsonObject object, QString error) {
		state.profileSaving = false;
		if (!error.isEmpty()) {
			state.cursor.clear();
			state.sync();
			done(std::move(error));
			return;
		}
		state.ownProfile = object.value(u"profile"_q).toObject();
		state.phone.clear();
		state.usernames.clear();
		state.primary.clear();
		const auto saved = state.save();
		state.notify();
		done(saved ? QString() : u"STORAGE_ERROR"_q);
	});
}

void SendGift(
		not_null<PeerData*> recipient,
		const MTPStarGift &source,
		QString message,
		bool anonymous,
		CreditsAmount price,
		QString operation,
		Fn<void(std::optional<Data::SavedStarGift>, QString)> done) {
	auto &state = Get(&recipient->session());
	if (!state.enabled.current() || !recipient->isUser()) {
		done({}, u"NOT_SUPPORTED"_q);
		return;
	}
	const auto info = Api::FromTL(state.session, source);
	if (!info) {
		done({}, u"INVALID_GIFT"_q);
		return;
	}
	if (state.endpoint.isEmpty() && !info->unique) {
		const auto gift = AddGift(recipient, source, std::move(message), anonymous, price);
		done(gift, gift ? QString() : u"STORAGE_ERROR"_q);
		return;
	}
	state.request(u"/v1/gifts"_q, {
		{ u"recipient_id"_q, QString::number(peerToUser(recipient->id).bare) },
		{ u"source"_q, QString::fromLatin1(Encode(source)) },
		{ u"slug"_q, info->unique ? info->unique->slug : QString() },
		{ u"message"_q, message },
		{ u"anonymous"_q, anonymous },
		{ u"price"_q, QString::number(price.ton()
			? price.whole() * 1'000'000'000 + price.nano() : price.whole()) },
		{ u"currency"_q, price.ton() ? u"TON"_q : u"XTR"_q },
		{ u"operation_id"_q, operation },
	}, [&state, done = std::move(done)](QJsonObject object, QString error) {
		if (!error.isEmpty()) {
			state.sync();
			done({}, std::move(error));
			return;
		}
		const auto value = object.value(u"gift"_q).toObject();
		const auto id = value.value(u"id"_q).toString();
		auto replaced = false;
		for (auto i = 0; i != state.serverGifts.size(); ++i) {
			if (state.serverGifts[i].toObject().value(u"id"_q).toString() == id) {
				state.serverGifts[i] = value;
				replaced = true;
				break;
			}
		}
		if (!replaced) {
			state.serverGifts.push_back(value);
		}
		state.acceptGift(value, !state.serverVersions.contains(id));
		const auto found = ranges::find(state.records, id, &Record::serverId);
		const auto saved = found != end(state.records)
			? std::make_optional(found->gift) : std::nullopt;
		(void)state.save();
		state.notify();
		state.sync();
		done(saved, saved ? QString() : u"STORAGE_ERROR"_q);
	});
}

void ManageGift(
		not_null<Main::Session*> session,
		Data::SavedStarGiftId id,
		bool pinned,
		bool hidden,
		Fn<void(QString)> done) {
	auto &state = Get(session);
	const auto found = Find(state, id);
	if (found == end(state.records) || !found->gift.mine) {
		done(u"NOT_GIFT_OWNER"_q);
		return;
	}
	if (found->serverId.isEmpty()) {
		const auto saved = SetHidden(session, id, hidden)
			&& (!found->gift.info.unique || SetPinned(session, id, pinned));
		done(saved ? QString() : u"STORAGE_ERROR"_q);
		return;
	}
	state.request(u"/v1/gifts/manage"_q, {
		{ u"id"_q, found->serverId },
		{ u"pinned"_q, pinned },
		{ u"hidden"_q, hidden },
		{ u"operation_id"_q, QUuid::createUuid().toString(QUuid::WithoutBraces) },
	}, [&state, done = std::move(done)](QJsonObject object, QString error) {
		if (error.isEmpty()) {
			const auto gift = object.value(u"gift"_q).toObject();
			for (auto i = 0; i != state.serverGifts.size(); ++i) {
				if (state.serverGifts[i].toObject().value(u"id"_q) == gift.value(u"id"_q)) {
					state.serverGifts[i] = gift;
					break;
				}
			}
			state.acceptGift(gift);
			(void)state.save();
			state.restoredPeers.clear();
			state.restoreMessages();
			state.notify();
		}
		done(std::move(error));
	});
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
	state.loadRecords();
	if (!peer->isSelf() && !state.endpoint.isEmpty()) {
		state.view(peer);
		const auto found = state.remoteProfiles.find(peer->id);
		if (found != end(state.remoteProfiles)) {
			const auto values = found->second.value(u"gifts"_q).toArray();
			auto visible = base::flat_set<QString>();
			for (const auto &value : values) {
				visible.emplace(value.toObject().value(u"id"_q).toString());
			}
			for (auto &record : state.records) {
				if (!record.serverId.isEmpty() && record.recipient == peer->id
					&& record.sender != state.session->userPeerId()
					&& !visible.contains(record.serverId)) {
					record.active = false;
				}
			}
			for (const auto &value : values) {
				state.acceptGift(value.toObject());
			}
		}
	}
	for (const auto &record : state.records) {
		if (record.recipient == peer->id && record.active
			&& (state.endpoint.isEmpty() || !record.serverId.isEmpty())
			&& (peer->isSelf() || !record.gift.hidden)
			&& (!pinnedOnly || (record.gift.pinned && !record.gift.hidden))) {
			result.push_back(record.gift);
			if (pinnedOnly) {
				state.refreshGift(record.gift.manageId);
			}
		}
	}
	return result;
}

int GiftCount(not_null<PeerData*> peer) {
	auto &state = Get(&peer->session());
	if (!state.enabled.current()) {
		return 0;
	}
	auto count = int(ranges::count_if(state.records, [&](const Record &record) {
		return state.endpoint.isEmpty() && record.serverId.isEmpty() && record.recipient == peer->id
			&& (peer->isSelf() || !record.gift.hidden);
	}));
	for (const auto &value : state.pendingRecords) {
		if (!state.endpoint.isEmpty()) {
			break;
		}
		const auto object = value.toObject();
		if (object.value(u"recipient"_q).toString() == QString::number(peer->id.value)
			&& (peer->isSelf() || !object.value(u"hidden"_q).toBool())) {
			++count;
		}
	}
	const auto server = peer->isSelf() ? state.serverGifts : [&] {
		state.view(peer);
		const auto found = state.remoteProfiles.find(peer->id);
		return found != end(state.remoteProfiles)
			? found->second.value(u"gifts"_q).toArray() : QJsonArray();
	}();
	for (const auto &value : server) {
		const auto object = value.toObject();
		if (object.value(u"recipient_id"_q).toString()
				== QString::number(peerToUser(peer->id).bare)
			&& object.value(u"active"_q).toBool(true)
			&& (peer->isSelf() || !object.value(u"hidden"_q).toBool())) {
			++count;
		}
	}
	return count;
}

bool IsSyncedGift(not_null<Main::Session*> session, Data::SavedStarGiftId id) {
	auto &state = Get(session);
	const auto found = Find(state, id);
	return found != end(state.records) && !found->serverId.isEmpty();
}

bool IsLocal(not_null<Main::Session*> session, Data::SavedStarGiftId id) {
	auto &state = Get(session);
	return Find(state, id) != end(state.records);
}

std::optional<Data::SavedStarGift> FindGift(
		not_null<Main::Session*> session,
		Data::SavedStarGiftId id) {
	auto &state = Get(session);
	if (state.enabled.current()) {
		state.loadRecords();
	}
	const auto found = Find(state, id);
	return (state.enabled.current() && found != end(state.records))
		? std::make_optional(found->gift)
		: std::nullopt;
}

PeerId GiftRecipient(
		not_null<Main::Session*> session,
		Data::SavedStarGiftId id) {
	auto &state = Get(session);
	if (!state.enabled.current()) {
		return PeerId();
	}
	state.loadRecords();
	const auto found = Find(state, id);
	return (found != end(state.records)) ? found->recipient : PeerId();
}

std::optional<Data::SavedStarGift> AddGift(
		not_null<PeerData*> recipient,
		const MTPStarGift &source,
		QString message,
		bool anonymous,
		CreditsAmount price) {
	auto &state = Get(&recipient->session());
	if (!state.enabled.current()) {
		return {};
	}
	state.loadRecords();
	const auto info = Api::FromTL(state.session, source);
	if (!info
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
	state.records.push_back({ source, gift, recipient->id, Encode(source), price });
	if (!state.save()) {
		state.records.pop_back();
		return {};
	}
	state.recordIndex.emplace(
		gift.manageId.userMessageId(),
		state.records.size() - 1);
	state.restoreMessage(
		state.records.back(),
		state.session->data().history(recipient->id),
		true);
	state.notify();
	return gift;
}

bool SetPinned(
		not_null<Main::Session*> session,
		Data::SavedStarGiftId id,
		bool pinned) {
	auto &state = Get(session);
	const auto found = Find(state, id);
	if (found == end(state.records) || !found->gift.info.unique
		|| found->recipient != session->userPeerId()) {
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
	if (found == end(state.records)
		|| found->recipient != session->userPeerId()) {
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
	state.recordIndex.erase(id.userMessageId());
	for (auto &entry : state.recordIndex) {
		if (entry.second > index) {
			--entry.second;
		}
	}
	state.removeMessage(record);
	state.notify();
	return true;
}

void RestoreMessages(not_null<Main::Session*> session) {
	(void)Get(session);
}

void RestoreHistory(not_null<History*> history) {
	Get(&history->session()).restoreMessages(history);
}

void RefreshGift(
		not_null<Main::Session*> session,
		Data::SavedStarGiftId id) {
	if (IsClientMsgId(id.userMessageId())) {
		Get(session).refreshGift(id);
	}
}

}
