// This is the source code of AyuGram for Desktop.
//
// We do not and cannot prevent the use of our code,
// but be respectful and credit the original author.
//
// Copyright @Radolyn, 2026
#include "ayu/ayu_worker.h"

#include "apiwrap.h"
#include "ayu/ayu_settings.h"
#include "base/timer.h"
#include "base/unixtime.h"
#include "core/application.h"
#include "data/data_changes.h"
#include "data/data_user.h"
#include "main/main_account.h"
#include "main/main_domain.h"
#include "main/main_session.h"
#include "rpl/flatten_latest.h"
#include "rpl/map.h"

#include <crl/crl_time.h>

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

namespace AyuWorker {
namespace {

constexpr auto kOfflineDelay = crl::time(3000);

struct PendingOffline {
	crl::time deadline = 0;
	bool force = false;
};

std::unordered_map<Main::Session*, PendingOffline> pending;
crl::time scheduledAt = 0;
std::unordered_set<Main::Session*> trackedSessions;
std::unordered_set<Main::Account*> trackedAccounts;
rpl::lifetime lifetime;

void RunOnce();

base::Timer &WorkerTimer() {
	static base::Timer timer(RunOnce);
	return timer;
}

void Schedule() {
	if (pending.empty()) {
		WorkerTimer().cancel();
		scheduledAt = 0;
		return;
	}
	const auto first = std::min_element(
		pending.begin(),
		pending.end(),
		[](const auto &a, const auto &b) {
			return a.second.deadline < b.second.deadline;
		})->second.deadline;
	if (!WorkerTimer().isActive() || scheduledAt != first) {
		scheduledAt = first;
		WorkerTimer().callOnce(std::max(crl::time(1), first - crl::now()));
	}
}

void Queue(not_null<Main::Session*> session, bool force) {
	if (AyuSettings::ghost(session).sendOfflinePacketAfterOnline()) {
		auto &entry = pending[session];
		entry.force = entry.force || force;
		if (force || !entry.deadline) {
			entry.deadline = crl::now() + kOfflineDelay;
		}
	} else {
		pending.erase(session);
	}
	Schedule();
}

void RunOnce() {
	if (Core::Quitting()) {
		pending.clear();
		return;
	}
	const auto now = crl::now();
	if (!Core::IsAppLaunched() || !Core::App().domain().started()) {
		for (auto &[session, entry] : pending) {
			entry.deadline = now + kOfflineDelay;
		}
		Schedule();
		return;
	}
	const auto unixNow = base::unixtime::now();
	for (auto i = pending.begin(); i != pending.end();) {
		const auto session = not_null<Main::Session*>(i->first);
		if (!AyuSettings::ghost(session).sendOfflinePacketAfterOnline()) {
			i = pending.erase(i);
			continue;
		}
		if (i->second.deadline > now) {
			++i;
			continue;
		}
		const auto online = session->user()->lastseen().isOnline(unixNow);
		if (i->second.force || online) {
			session->api().request(MTPaccount_UpdateStatus(
				MTP_bool(true)
			)).send();
			i->second.force = false;
			DEBUG_LOG(("[AyuGram] Sent offline for account with id %1"
			).arg(session->userId().bare));
		}
		if (online) {
			i->second.deadline = now + kOfflineDelay;
			++i;
		} else {
			i = pending.erase(i);
		}
	}
	Schedule();
}

void TrackSession(not_null<Main::Session*> session) {
	if (!trackedSessions.insert(session).second) {
		return;
	}
	AyuSettings::getInstance().useGlobalGhostModeValue(
	) | rpl::map([=] {
		return AyuSettings::ghost(session).sendOfflinePacketAfterOnlineValue();
	}) | rpl::flatten_latest(
	) | rpl::start_with_next([=](bool enabled) {
		Queue(session, enabled);
	}, session->lifetime());
	session->changes().peerUpdates(
		session->user(),
		Data::PeerUpdate::Flag::OnlineStatus
	) | rpl::start_with_next([=] {
		if (session->user()->lastseen().isOnline(base::unixtime::now())) {
			Queue(session, false);
		}
	}, session->lifetime());
	session->lifetime().add([session] {
		pending.erase(session);
		trackedSessions.erase(session);
		Schedule();
	});
}

void TrackAccounts() {
	for (const auto &[index, account] : Core::App().domain().accounts()) {
		if (!trackedAccounts.insert(account.get()).second) {
			continue;
		}
		account->sessionValue(
		) | rpl::start_with_next([](Main::Session *session) {
			if (session) {
				TrackSession(session);
			}
		}, account->lifetime());
		account->lifetime().add([pointer = account.get()] {
			trackedAccounts.erase(pointer);
		});
	}
}

} // namespace

void markAsOnline(not_null<Main::Session*> session) {
	TrackSession(session);
	Queue(session, true);
}

void initialize() {
	Core::App().domain().accountsChanges(
	) | rpl::start_with_next(TrackAccounts, lifetime);
	TrackAccounts();
}

} // namespace AyuWorker
