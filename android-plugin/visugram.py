import json
import re
import time
import urllib.error
import urllib.request
import uuid
from collections import OrderedDict
from decimal import Decimal

from android_utils import run_on_ui_thread
from base_plugin import AppEvent, BasePlugin, MethodHook
from client_utils import (
    EXTERNAL_NETWORK_QUEUE, get_last_fragment, get_messages_controller,
    get_user_config, run_on_queue,
)
from hook_utils import find_class, get_private_field
from java.util import Locale
from org.telegram.messenger import UserConfig
from ui.alert import AlertDialogBuilder
from ui.bulletin import BulletinHelper
from ui.settings import Divider, EditText, Header, Input, Switch, Text

__id__ = "visugram"
__name__ = "VisuGram Preview"
__description__ = "Shared visual usernames and anonymous numbers. Gift port in development."
__author__ = "VisuGram"
__version__ = "0.2.0"
__icon__ = "exteraPlugins/1"
__app_version__ = ">=12.5.1"
__sdk_version__ = ">=1.4.5.7"

SERVER = "https://visugram-api-production.up.railway.app"
POLL_MS = 30_000
REMOTE_LIMIT = 32
MAX_RESPONSE = 256 * 1024
NAME_RE = re.compile(r"[a-z][a-z0-9_]{0,31}")

class ApiError(Exception):
    def __init__(self, code):
        self.code = code
        super().__init__(code)

class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, url):
        return None

def post_json(path, user_id, body, timeout=8):
    request = urllib.request.Request(
        SERVER + path,
        data=json.dumps(body, separators=(",", ":")).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "X-VisuGram-User-Id": user_id,
        },
        method="POST",
    )
    try:
        response = urllib.request.build_opener(NoRedirect).open(request, timeout=timeout)
    except urllib.error.HTTPError as error:
        with error:
            try:
                payload = json.loads(error.read(4096))
                code = payload.get("error", "SERVER_UNAVAILABLE")
            except (ValueError, AttributeError):
                code = "SERVER_UNAVAILABLE"
        raise ApiError(code if isinstance(code, str) else "SERVER_UNAVAILABLE") from error
    with response:
        data = response.read(MAX_RESPONSE + 1)
    if len(data) > MAX_RESPONSE:
        raise ValueError("RESPONSE_TOO_LARGE")
    result = json.loads(data)
    if not isinstance(result, dict):
        raise ValueError("INVALID_RESPONSE")
    return result

def validate_profile(profile):
    if not isinstance(profile, dict):
        raise ValueError("INVALID_PROFILE")
    phone, names = profile.get("phone", ""), profile.get("usernames", [])
    if not isinstance(phone, str) or len(phone) > 32:
        raise ValueError("INVALID_PROFILE")
    if phone and not re.fullmatch(r"888[0-9]{1,8}", re.sub(r"[\s()+-]", "", phone)):
        raise ValueError("INVALID_PROFILE")
    if not isinstance(names, list) or len(names) > 20:
        raise ValueError("INVALID_PROFILE")
    if any(not isinstance(name, str) or not NAME_RE.fullmatch(name) for name in names) or len(set(names)) != len(names):
        raise ValueError("INVALID_PROFILE")
    primary, revision = profile.get("primary", ""), profile.get("revision", 0)
    metadata = profile.get("collectibles", {})
    if not isinstance(primary, str) or (primary and primary not in names):
        raise ValueError("INVALID_PROFILE")
    if type(revision) is not int or revision < 0 or revision > 2**63 - 1:
        raise ValueError("INVALID_PROFILE")
    if not isinstance(metadata, dict) or any(not isinstance(value, dict) for value in metadata.values()):
        raise ValueError("INVALID_PROFILE")
    return {"phone": phone, "usernames": names, "primary": primary,
            "revision": revision, "collectibles": metadata}

def sync_profile(user_id, username, cached):
    result = post_json("/v1/sync", user_id, {
        "telegram_username": username, "cursor": cached.get("cursor", ""),
        "profile_only": True,
    })
    if result.get("unchanged") is True:
        return None
    cursor, count = result.get("cursor"), result.get("gift_count")
    if not isinstance(cursor, str) or type(count) is not int or count < 0:
        raise ValueError("INVALID_RESPONSE")
    return {"cursor": cursor, "profile": validate_profile(result.get("profile")),
            "gift_count": count, "updated_at": int(time.time())}

def parse_names(value):
    names = [line.strip().lstrip("@").lower() for line in value.splitlines() if line.strip()]
    if len(names) > 20 or any(not NAME_RE.fullmatch(name) for name in names) or len(set(names)) != len(names):
        raise ApiError("INVALID_USERNAME")
    return names

def phone_from_suffix(value):
    value = re.sub(r"[\s()+-]", "", value.strip())
    if not value:
        return ""
    if not re.fullmatch(r"[0-9]{1,8}", value):
        raise ApiError("INVALID_PHONE")
    return "+888" + value

def visible_names(profile, real_main, real_names):
    names = list(dict.fromkeys([name.lower() for name in [real_main, *real_names, *profile["usernames"]] if name]))
    main = profile["primary"] or (names[0] if names else "")
    return main, [name for name in names if name != main]

def find_method(class_name, name, arguments):
    clazz = find_class(class_name)
    if clazz is not None:
        for method in clazz.getDeclaredMethods():
            types = [str(value.getName()) for value in method.getParameterTypes()]
            if str(method.getName()) == name and types == arguments:
                method.setAccessible(True)
                return method
    return None

class ProfileBindHook(MethodHook):
    def __init__(self, plugin):
        self.plugin = plugin

    def after_hooked_method(self, param):
        try:
            self.plugin._bind_profile(param)
        except Exception:
            self.plugin._native_error()

class ProfileClickHook(MethodHook):
    def __init__(self, plugin):
        self.plugin = plugin

    def before_hooked_method(self, param):
        try:
            if self.plugin._click_profile(param):
                param.setResult(True)
        except Exception:
            self.plugin._native_error()

class VisuGramPlugin(BasePlugin):
    def __init__(self):
        super().__init__()
        self._generation = 0
        self._loaded, self._foreground = False, False
        self._busy, self._saving, self._snapshots = {}, {}, {}
        self._remote, self._remote_busy = OrderedDict(), {}
        self._native, self._warned = False, False

    def on_plugin_load(self):
        self._generation = getattr(self, "_generation", 0) + 1
        self._loaded, self._foreground = True, True
        self._busy, self._saving, self._snapshots = {}, {}, {}
        self._remote, self._remote_busy = OrderedDict(), {}
        self._native, self._warned = False, False
        self._install_native()
        self._restart()

    def on_plugin_unload(self):
        self._loaded = False
        self._generation += 1
        self._remote.clear()
        self._snapshots.clear()
        self._rebind_visible()

    def on_app_event(self, event_type):
        if not getattr(self, "_loaded", False):
            return
        if event_type in (AppEvent.PAUSE, AppEvent.STOP):
            self._foreground = False
            self._generation += 1
        elif event_type in (AppEvent.START, AppEvent.RESUME):
            self._foreground = True
            self._restart()

    def _text(self, english, russian):
        return russian if str(Locale.getDefault().getLanguage()) == "ru" else english

    def _native_error(self):
        if not self._warned:
            self._warned = True
            self.logger.warning("VisuGram profile hook is unavailable for this client version.")

    def _install_native(self):
        try:
            bind = find_method("org.telegram.ui.ProfileActivity$ListAdapter", "onBindViewHolder", [
                "androidx.recyclerview.widget.RecyclerView$ViewHolder", "int",
            ])
            click = find_method("org.telegram.ui.ProfileActivity", "processOnClickOrPress", [
                "int", "android.view.View", "float", "float",
            ])
        except Exception:
            self._native_error()
            return
        if bind is None or click is None:
            self._native_error()
            return
        hooks = []
        try:
            from android.text.style import ClickableSpan
            from extera_utils.classes import Base, java_subclass, joverride

            @java_subclass(ClickableSpan)
            class AssetSpan(Base):
                def __init__(self, plugin, fragment_ref, kind, value):
                    self.plugin, self.fragment_ref = plugin, fragment_ref
                    self.kind, self.value = kind, value

                @joverride("onClick", ["android.view.View"])
                def onClick(self, view):
                    fragment = self.fragment_ref.get()
                    if fragment is not None:
                        try:
                            self.plugin._open_asset(fragment, self.kind, self.value)
                        except Exception:
                            self.plugin._native_error()

            self._span_type = AssetSpan
            hooks.append(self.hook_method(bind, ProfileBindHook(self)))
            hooks.append(self.hook_method(click, ProfileClickHook(self)))
            if not all(hooks):
                raise ValueError("HOOK_UNAVAILABLE")
            self._native = True
        except Exception:
            for hook in hooks:
                if hook:
                    self.unhook_method(hook)
            self._native_error()

    def _running(self, generation):
        return (self._loaded and self._foreground and generation == self._generation
                and self.get_setting("sync_enabled", False))

    def _restart(self):
        self._generation += 1
        generation = self._generation
        run_on_ui_thread(lambda: self._tick(generation))

    def _toggle(self, enabled):
        self.set_setting("sync_enabled", enabled)
        if not enabled:
            self._remote.clear()
        self._restart()
        self._rebind_visible()

    def _tick(self, generation):
        if not self._running(generation):
            return
        self._refresh(generation)
        try:
            fragment = get_last_fragment()
            if self._native and self._profile_context(fragment) is not None:
                self._profile_for(fragment)
        except Exception:
            self._native_error()
        run_on_ui_thread(lambda: self._tick(generation), POLL_MS)

    def _cache(self, user_id):
        if user_id in self._snapshots:
            return self._snapshots[user_id]
        try:
            value = json.loads(self.get_setting("snapshot:" + user_id, "{}"))
            value["profile"] = validate_profile(value.get("profile"))
            if not isinstance(value.get("cursor"), str) or type(value.get("gift_count")) is not int or value["gift_count"] < 0:
                raise ValueError("INVALID_CACHE")
        except (TypeError, ValueError, AttributeError):
            value = {}
        self._snapshots[user_id] = value
        return value

    def _store(self, user_id, snapshot):
        self._snapshots[user_id] = snapshot
        selected = get_user_config(int(UserConfig.selectedAccount)).getCurrentUser()
        self.set_setting("snapshot:" + user_id, json.dumps(snapshot, ensure_ascii=False, separators=(",", ":")),
                         reload_settings=selected is not None and str(selected.id) == user_id)
        self._rebind_visible()

    def _refresh(self, generation, manual=False):
        if not self._running(generation):
            if manual:
                self._show_error("SYNC_DISABLED")
            return
        account = int(UserConfig.selectedAccount)
        user = get_user_config(account).getCurrentUser()
        if user is None:
            return
        user_id, username = str(user.id), str(user.username or "")
        if user_id in self._busy or user_id in self._saving:
            return
        cached = self._cache(user_id)
        self._busy[user_id] = generation

        def work():
            try:
                snapshot, error = sync_profile(user_id, username, cached), ""
            except (OSError, ValueError, TypeError, ApiError) as failure:
                snapshot, error = None, getattr(failure, "code", "SERVER_UNAVAILABLE")
            run_on_ui_thread(lambda: self._finish(account, user_id, generation, snapshot, error, manual))

        run_on_queue(work, EXTERNAL_NETWORK_QUEUE)

    def _finish(self, account, user_id, generation, snapshot, error, manual):
        if self._busy.get(user_id) == generation:
            del self._busy[user_id]
        if not self._running(generation):
            return
        user = get_user_config(account).getCurrentUser()
        if user is None or str(user.id) != user_id:
            return
        if error:
            if manual:
                self._show_error(error)
            return
        if snapshot is not None:
            self._store(user_id, snapshot)
        if manual:
            BulletinHelper.show_info(self._text("Profile updated.", "Профиль обновлён."))

    def _profile_context(self, fragment):
        if fragment is None or str(fragment.getClass().getName()) != "org.telegram.ui.ProfileActivity":
            return None
        account = int(fragment.getCurrentAccount())
        user = get_user_config(account).getCurrentUser()
        owner = get_private_field(fragment, "userId")
        if user is None or owner is None or int(owner) <= 0:
            return None
        return account, str(user.id), str(owner)

    def _profile_for(self, fragment):
        context = self._profile_context(fragment)
        if context is None:
            return None
        account, viewer, owner = context
        if owner == viewer:
            return self._cache(viewer).get("profile")
        key = viewer, owner
        cached = self._remote.get(key)
        if cached is None or time.monotonic() - cached[0] >= POLL_MS / 1000:
            self._fetch_remote(account, viewer, owner)
        return cached[1] if cached else None

    def _fetch_remote(self, account, viewer, owner):
        key, generation = (viewer, owner), self._generation
        if key in self._remote_busy or len(self._remote_busy) >= 2:
            return
        self._remote_busy[key] = generation

        def work():
            try:
                result = post_json("/v1/profile/view", viewer, {"owner_id": owner, "profile_only": True})
                profile = validate_profile(result.get("profile"))
            except (OSError, ValueError, TypeError, ApiError):
                profile = None
            run_on_ui_thread(lambda: self._remote_finish(account, key, generation, profile))

        run_on_queue(work, EXTERNAL_NETWORK_QUEUE)

    def _remote_finish(self, account, key, generation, profile):
        if self._remote_busy.get(key) == generation:
            del self._remote_busy[key]
        if not self._running(generation):
            return
        user = get_user_config(account).getCurrentUser()
        if user is None or str(user.id) != key[0]:
            return
        previous = self._remote.pop(key, None)
        self._remote[key] = time.monotonic(), profile or (previous[1] if previous else None)
        while len(self._remote) > REMOTE_LIMIT:
            self._remote.popitem(last=False)
        if profile is not None and (previous is None or profile != previous[1]):
            self._rebind_visible()

    def _name_text(self, fragment, names, profile):
        from android.text import SpannableStringBuilder
        from android.text.style import URLSpan
        from java.lang.ref import WeakReference

        text = ", ".join("@" + name for name in names)
        result, offset = SpannableStringBuilder(text), 0
        for name in names:
            value = "@" + name
            if name in profile["usernames"]:
                span = self._span_type.new_instance(init_args=[self, WeakReference(fragment), "username", name]).java
            else:
                span = URLSpan("https://t.me/" + name)
            result.setSpan(span, offset, offset + len(value), 33)
            offset += len(value) + 2
        return result

    def _bind_profile(self, param):
        if not self._native or not self._running(self._generation):
            return
        fragment = get_private_field(param.thisObject, "this$0")
        position, cell = int(param.args[1]), param.args[0].itemView
        phone_row = get_private_field(fragment, "phoneRow")
        name_row = get_private_field(fragment, "usernameRow")
        if position not in (phone_row, name_row):
            return
        profile = self._profile_for(fragment)
        if profile is None:
            return
        if position == phone_row and profile["phone"]:
            cell.textView.setText(profile["phone"])
            cell.valueTextView.setText(self._text("Anonymous Number", "Анонимный номер"))
        elif position == name_row and profile["usernames"]:
            account, viewer, owner = self._profile_context(fragment)
            user = get_messages_controller(account).getUser(int(owner))
            if user is None:
                return
            real_names = [str(user.usernames.get(index).username or "") for index in range(user.usernames.size())
                          if user.usernames.get(index).active]
            main, others = visible_names(profile, str(user.username or ""), real_names)
            if main in profile["usernames"]:
                cell.textView.setText(self._name_text(fragment, [main], profile))
            prefix = self._text("also ", "также ")
            if others:
                from android.text import SpannableStringBuilder
                subtext = SpannableStringBuilder(prefix)
                subtext.append(self._name_text(fragment, others, profile))
            else:
                subtext = self._text("Username", "Имя пользователя")
            cell.valueTextView.setText(subtext)
            cell.setImage(None)

    def _click_profile(self, param):
        if not self._native or not self._running(self._generation):
            return False
        position, fragment = int(param.args[0]), param.thisObject
        if position != get_private_field(fragment, "phoneRow"):
            return False
        profile = self._profile_for(fragment)
        return bool(profile and profile["phone"] and self._open_asset(fragment, "phone", profile["phone"]))

    def _open_asset(self, fragment, kind, value):
        if not self._running(self._generation):
            return False
        profile = self._profile_for(fragment)
        context = self._profile_context(fragment)
        activity = fragment.getParentActivity()
        if profile is None or context is None or activity is None:
            return False
        canonical = re.sub(r"[\s()+-]", "", value) if kind == "phone" else value
        key = "phone:+" + canonical if kind == "phone" else "username:" + value
        metadata = profile["collectibles"].get(key, {})
        url = "https://fragment.com/" + ("number/" + canonical if kind == "phone" else "username/" + value)
        account, viewer, owner = context
        user = get_messages_controller(account).getUser(int(owner))
        if metadata.get("price_kind") == "last_sale" and user is not None:
            try:
                from org.telegram.tgnet.tl import TL_fragment
                from org.telegram.ui import FragmentUsernameBottomSheet

                info = TL_fragment.TL_collectibleInfo()
                info.purchase_date = max(0, min(int(metadata.get("date", 0)), 2**31 - 1))
                info.currency = str(metadata.get("fiat_currency", "USD")) or "USD"
                info.amount = max(0, min(int(metadata.get("fiat_amount", 0)), 2**63 - 1))
                info.crypto_currency = "TON"
                info.crypto_amount = max(0, min(int(metadata.get("crypto_amount", 0)), 2**63 - 1))
                info.url = url
                FragmentUsernameBottomSheet.open(activity, 1 if kind == "phone" else 0, canonical,
                                                user, info, fragment.getResourceProvider())
                return True
            except Exception:
                self._native_error()
        labels = {
            "visual": self._text("Visual purchase", "Визуальная покупка"),
            "asking": self._text("Price on Fragment", "Цена на Fragment"),
            "bid": self._text("Bid on Fragment", "Ставка на Fragment"),
            "last_sale": self._text("Last sale on Fragment", "Последняя продажа на Fragment"),
        }
        try:
            price = Decimal(str(metadata.get("crypto_amount", "0"))) / 1_000_000_000
            amount = f"{price:,.2f} TON" if price.is_finite() and price >= 0 else "—"
        except (ValueError, ArithmeticError):
            amount = "—"
        title = value if kind == "phone" else "@" + value
        message = labels.get(metadata.get("price_kind"), self._text("Visual asset", "Визуальный объект")) + ": " + amount
        dialog = AlertDialogBuilder(activity)
        dialog.set_title(title)
        dialog.set_message(message)
        from org.telegram.messenger import AndroidUtilities
        from org.telegram.messenger.browser import Browser

        def copy(builder, which):
            AndroidUtilities.addToClipboard(value if kind == "phone" else "https://t.me/" + value)
            builder.dismiss()

        def learn(builder, which):
            Browser.openUrl(activity, url)
            builder.dismiss()

        dialog.set_positive_button(self._text("Learn More", "Подробнее"), learn)
        dialog.set_neutral_button(self._text("Copy", "Копировать"), copy)
        dialog.set_negative_button(self._text("Close", "Закрыть"), lambda builder, which: builder.dismiss())
        dialog.show()
        return True

    def _rebind_visible(self):
        try:
            fragment = get_last_fragment()
            if self._profile_context(fragment) is not None:
                adapter = get_private_field(fragment, "listAdapter")
                if adapter is not None:
                    adapter.notifyDataSetChanged()
        except Exception:
            self._native_error()

    def _show_error(self, code):
        messages = {
            "SYNC_DISABLED": ("Enable synchronization first.", "Сначала включи синхронизацию."),
            "INVALID_USERNAME": ("One username per line, without spaces or duplicates.", "Один юзернейм на строку, без пробелов и повторов."),
            "INVALID_PHONE": ("Enter 1–8 digits after +888.", "Введи от 1 до 8 цифр после +888."),
            "PHONE_NOT_FOUND": ("This +888 number was not found on Fragment.", "Этот номер +888 не найден на Fragment."),
            "ASSET_TAKEN": ("This asset is already used in VisuGram.", "Этот объект уже занят в VisuGram."),
            "PROFILE_CHANGED": ("Profile changed on another device. Refresh before saving.", "Профиль изменён на другом устройстве. Обнови его перед сохранением."),
            "SOURCE_UNAVAILABLE": ("Fragment is unavailable. Try again later.", "Fragment недоступен. Попробуй позже."),
            "SYNC_REQUIRED": ("Refresh the profile before saving.", "Обнови профиль перед сохранением."),
        }
        english, russian = messages.get(code, ("Server unavailable. Try again later.", "Сервер недоступен. Попробуй позже."))
        BulletinHelper.show_info(self._text(english, russian))

    def _save(self, user_id, account, keys):
        generation = self._generation
        user = get_user_config(account).getCurrentUser()
        if not self._running(generation) or user is None or str(user.id) != user_id:
            self._show_error("SYNC_DISABLED")
            return
        cached = self._cache(user_id)
        if "profile" not in cached:
            self._show_error("SYNC_REQUIRED")
            return
        if user_id in self._saving or user_id in self._busy:
            return
        if self.get_setting("draft_revision:" + user_id, -1) != cached["profile"]["revision"]:
            self._show_error("PROFILE_CHANGED")
            return
        try:
            names = parse_names(str(self.get_setting(keys[0], "")))
            phone = phone_from_suffix(str(self.get_setting(keys[1], "")))
            primary = str(self.get_setting(keys[2], "")).strip().lstrip("@").lower()
            if primary and primary not in names:
                raise ApiError("INVALID_USERNAME")
        except (ApiError, ValueError, TypeError) as failure:
            self._show_error(getattr(failure, "code", "INVALID_USERNAME"))
            return
        body = {"phone": phone, "usernames": names, "primary": primary,
                "revision": cached["profile"]["revision"]}
        fingerprint = json.dumps(body, sort_keys=True, separators=(",", ":"))
        previous = self.get_setting("operation:" + user_id, {})
        operation = previous.get("id") if isinstance(previous, dict) and previous.get("body") == fingerprint else None
        if not isinstance(operation, str) or not re.fullmatch(r"[A-Za-z0-9-]{16,64}", operation):
            operation = str(uuid.uuid4())
        self.set_setting("operation:" + user_id, {"body": fingerprint, "id": operation})
        body["operation_id"] = operation
        self._saving[user_id] = generation

        def work():
            try:
                result = post_json("/v1/profile", user_id, body, timeout=40)
                profile, error = validate_profile(result.get("profile")), ""
            except (OSError, ValueError, TypeError, ApiError) as failure:
                profile, error = None, getattr(failure, "code", "SERVER_UNAVAILABLE")
            run_on_ui_thread(lambda: self._save_finish(account, user_id, generation, profile, error))

        run_on_queue(work, EXTERNAL_NETWORK_QUEUE)

    def _save_finish(self, account, user_id, generation, profile, error):
        if self._saving.get(user_id) == generation:
            del self._saving[user_id]
        if not self._running(generation):
            return
        user = get_user_config(account).getCurrentUser()
        if user is None or str(user.id) != user_id:
            return
        if error:
            self._show_error(error)
            return
        cached = dict(self._cache(user_id))
        cached.update(profile=profile, cursor="", updated_at=int(time.time()))
        self.set_setting("draft_revision:" + user_id, profile["revision"])
        self._store(user_id, cached)
        self._refresh(generation)
        BulletinHelper.show_info(self._text("Visual profile saved.", "Визуальный профиль сохранён."))

    def _reset_draft(self, user_id):
        profile = self._cache(user_id).get("profile", validate_profile({}))
        self.set_setting("draft_names:" + user_id, "\n".join(profile["usernames"]))
        self.set_setting("draft_phone:" + user_id, re.sub(r"[\s()+-]", "", profile["phone"])[3:])
        self.set_setting("draft_primary:" + user_id, profile["primary"])
        self.set_setting("draft_revision:" + user_id, profile["revision"], reload_settings=True)

    def _editor(self):
        account = int(UserConfig.selectedAccount)
        user = get_user_config(account).getCurrentUser()
        if user is None:
            return []
        user_id = str(user.id)
        profile = self._cache(user_id).get("profile", validate_profile({}))
        keys = ["draft_names:" + user_id, "draft_phone:" + user_id, "draft_primary:" + user_id]
        defaults = ["\n".join(profile["usernames"]), re.sub(r"[\s()+-]", "", profile["phone"])[3:], profile["primary"]]
        for key, default in zip(keys, defaults):
            if self.get_setting(key, None) is None:
                self.set_setting(key, default)
        if self.get_setting("draft_revision:" + user_id, None) is None:
            self.set_setting("draft_revision:" + user_id, profile["revision"])
        return [
            Header(text=self._text("Visual profile", "Визуальный профиль")),
            EditText(key=keys[0], hint=self._text("@username — one per line", "@username — один на строку"),
                     default=defaults[0], multiline=True, max_length=660),
            Input(key=keys[1], text=self._text("Phone number +888", "Номер телефона +888"), default=defaults[1],
                  subtext=self._text("Only digits after +888. Empty field removes the visual number.",
                                     "Только цифры после +888. Пустое поле убирает визуальный номер.")),
            Input(key=keys[2], text=self._text("Main visual username", "Основной визуальный юзернейм"), default=defaults[2],
                  subtext=self._text("Choose a name from the list above. Leave empty to keep the Telegram username.",
                                     "Укажи юзернейм из списка выше. Пустое поле сохраняет основной юзернейм Telegram.")),
            Text(text=self._text("Save", "Сохранить"), accent=True,
                 on_click=lambda view: self._save(user_id, account, keys)),
            Text(text=self._text("Reset edits to server profile", "Сбросить изменения к профилю с сервера"),
                 on_click=lambda view: self._reset_draft(user_id)),
            Divider(text=self._text("Changes are shared with VisuGram Desktop.", "Изменения синхронизируются с VisuGram Desktop.")),
        ]

    def create_settings(self):
        user = get_user_config(int(UserConfig.selectedAccount)).getCurrentUser()
        cached = self._cache(str(user.id)) if user is not None else {}
        profile = cached.get("profile", validate_profile({}))
        return [
            Header(text="VisuGram"),
            Switch(key="sync_enabled", default=False, text=self._text("Synchronization", "Синхронизация"),
                   subtext=self._text("Sends Telegram ID and basic username to the VisuGram server.",
                                      "Передаёт Telegram ID и основной юзернейм серверу VisuGram."), on_change=self._toggle),
            Text(text=self._text("Refresh now", "Обновить сейчас"), on_click=lambda view: self._refresh(self._generation, True)),
            Text(text=self._text("Edit visual profile", "Изменить визуальный профиль"), create_sub_fragment=self._editor),
            Text(text=self._text("Usernames", "Юзернеймы"), subtext=", ".join("@" + name for name in profile["usernames"]) or "—"),
            Text(text=self._text("Phone number", "Номер телефона"), subtext=profile["phone"] or "—"),
            Text(text=self._text("Visual gifts", "Визуальные подарки"), subtext=str(cached.get("gift_count", 0))),
            Text(text=self._text("Profile integration", "Интеграция профиля"),
                 subtext=self._text("Available", "Доступна") if self._native else self._text(
                     "Unsupported client version. Shared profile is available in these settings.",
                     "Версия клиента не поддерживается. Общий профиль доступен в этих настройках.")),
            Divider(text=self._text("Preview: profile integration is available. Gift shop and gift cards are still being ported.",
                                    "Предварительная версия: интеграция профиля готова. Магазин и карточки подарков ещё переносятся.")),
        ]
