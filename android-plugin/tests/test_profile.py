import importlib.util
from importlib.machinery import SourceFileLoader
import ast
import json
import sys
import types
import unittest
from pathlib import Path
from unittest.mock import patch

UI, NETWORK, DELAYED = [], [], []
USERS, CONTROLLERS = {}, {}
VISIBLE = None

def module(name, **values):
    result = types.ModuleType(name)
    result.__dict__.update(values)
    sys.modules[name] = result
    return result

class BasePlugin:
    def __init__(self):
        self.settings, self.writes = {}, []

    def log(self, text):
        pass

    def get_setting(self, key, default=None):
        return self.settings.get(key, default)

    def set_setting(self, key, value, reload_settings=False):
        self.settings[key] = value
        self.writes.append((key, value, reload_settings))

class Setting:
    def __init__(self, **kwargs):
        self.__dict__.update(kwargs)

class UserConfig:
    selectedAccount = 0

    @staticmethod
    def getInstance(account):
        return types.SimpleNamespace(getCurrentUser=lambda: USERS.get(account))

class AppEvent:
    START, STOP, PAUSE, RESUME = range(4)

class HookStrategy:
    CANCEL = "cancel"

def run_ui(fn, delay=0):
    (DELAYED if delay else UI).append(fn)

for name in ("java", "org", "org.telegram", "ui"):
    module(name)
module("android_utils", run_on_ui_thread=run_ui)
module("base_plugin", AppEvent=AppEvent, BasePlugin=BasePlugin, MethodHook=object,
       HookResult=Setting, HookStrategy=HookStrategy)
module("client_utils", EXTERNAL_NETWORK_QUEUE="external", get_last_fragment=lambda: VISIBLE,
       run_on_queue=lambda fn, queue: NETWORK.append(fn))
module("hook_utils", find_class=lambda name: None, get_private_field=lambda obj, name: getattr(obj, name, None))
module("java.util", Locale=types.SimpleNamespace(getDefault=lambda: types.SimpleNamespace(getLanguage=lambda: "en")))
module("org.telegram.messenger", UserConfig=UserConfig,
       MessagesController=types.SimpleNamespace(getInstance=lambda account: CONTROLLERS[account]))
module("ui.alert", AlertDialogBuilder=object)
module("ui.bulletin", BulletinHelper=types.SimpleNamespace(show_info=lambda text: None))
module("ui.settings", **{name: Setting for name in ("Divider", "Header", "Input", "Switch", "Text")})
source = Path(__file__).parents[1] / "visugram.plugin"
spec = importlib.util.spec_from_loader("visugram", SourceFileLoader("visugram", str(source)))
plugin = importlib.util.module_from_spec(spec)
spec.loader.exec_module(plugin)

def snapshot(name="dev", revision=1):
    return {"cursor": "phone-format-v1:1", "profile": plugin.validate_profile({
        "usernames": [name], "revision": revision,
    }), "gift_count": 2, "updated_at": 1}

def drain_ui():
    while UI:
        UI.pop(0)()

class ProfileTests(unittest.TestCase):
    def setUp(self):
        UI.clear()
        NETWORK.clear()
        DELAYED.clear()
        USERS.clear()
        USERS[0] = types.SimpleNamespace(id=101, username="real")
        USERS[1] = types.SimpleNamespace(id=202, username="other")
        UserConfig.selectedAccount = 0
        self.subject = plugin.VisuGramPlugin()
        self.subject.on_plugin_load()
        drain_ui()

    def enable(self):
        self.subject._toggle(True)
        drain_ui()

    def test_disabled_has_no_network_or_poll_chain(self):
        self.assertEqual(NETWORK, [])
        self.assertEqual(DELAYED, [])

    def test_settings_can_open_before_plugin_is_enabled(self):
        disabled = plugin.VisuGramPlugin()
        self.assertGreater(len(disabled.create_settings()), 3)
        self.assertGreater(len(disabled._editor()), 3)
        disabled._refresh(disabled._generation, True)
        self.assertEqual(NETWORK, [])

    def test_sync_coalesces_and_stores_only_own_account(self):
        self.enable()
        self.subject._refresh(self.subject._generation, True)
        self.assertEqual(len(NETWORK), 1)
        UserConfig.selectedAccount = 1
        with patch.object(plugin, "sync_profile", return_value=snapshot()) as request:
            NETWORK.pop(0)()
            drain_ui()
        self.assertEqual(request.call_args.args[:2], ("101", "real"))
        self.assertEqual(self.subject._cache("101")["profile"]["usernames"], ["dev"])
        self.assertEqual(self.subject._cache("202"), {})
        self.assertFalse(self.subject.writes[-1][2])

    def test_disable_discards_inflight_response_and_old_timer(self):
        self.enable()
        self.subject._toggle(False)
        with patch.object(plugin, "sync_profile", return_value=snapshot()):
            NETWORK.pop(0)()
            drain_ui()
        DELAYED.pop(0)()
        self.assertEqual(self.subject._cache("101"), {})
        self.assertEqual(NETWORK, [])
        self.assertEqual(self.subject._busy, {})

    def test_pause_and_reload_reject_old_generation(self):
        self.enable()
        old = NETWORK.pop(0)
        self.subject.on_app_event(AppEvent.PAUSE)
        self.subject.on_plugin_unload()
        self.subject.on_plugin_load()
        drain_ui()
        with patch.object(plugin, "sync_profile", return_value=snapshot()):
            old()
            drain_ui()
        self.assertEqual(self.subject._cache("101"), {})
        self.assertIn("101", self.subject._busy)

    def test_logout_discards_response(self):
        self.enable()
        USERS[0] = None
        with patch.object(plugin, "sync_profile", return_value=snapshot()):
            NETWORK.pop(0)()
            drain_ui()
        self.assertEqual(self.subject._cache("101"), {})

    def test_corrupt_nested_cache_does_not_break_settings(self):
        self.subject.settings["snapshot:101"] = json.dumps({"profile": [], "cursor": "old"})
        self.assertEqual(self.subject._cache("101"), {})
        self.assertGreater(len(self.subject.create_settings()), 3)

    def test_sync_uses_compact_response_and_handles_unchanged(self):
        response = {"profile": {}, "cursor": "phone-format-v1:1", "gift_count": 3}
        with patch.object(plugin, "post_json", return_value=response) as request:
            value = plugin.sync_profile("101", "real", {})
        self.assertTrue(request.call_args.args[2]["profile_only"])
        self.assertEqual(value["gift_count"], 3)
        with patch.object(plugin, "post_json", return_value={"unchanged": True}):
            self.assertIsNone(plugin.sync_profile("101", "real", value))
        with patch.object(plugin, "post_json", return_value={**response, "gift_count": "3"}):
            with self.assertRaises(ValueError):
                plugin.sync_profile("101", "real", {})

    def test_short_usernames_and_phone_prefix(self):
        self.assertEqual(plugin.parse_names("@dev\n own\n"), ["dev", "own"])
        self.assertEqual(plugin.parse_names("@dev, own\nthird"), ["dev", "own", "third"])
        self.assertEqual(plugin.phone_from_suffix(" 8 666 "), "+8888666")
        self.assertEqual(plugin.phone_from_suffix(""), "")
        for value in ("dev\nDEV", "has space", "кириллица", "bad!", "0user"):
            with self.subTest(value=value), self.assertRaises(plugin.ApiError):
                plugin.parse_names(value)
        for value in ("888888888", "+888 12345678", "1foo"):
            with self.subTest(value=value), self.assertRaises(plugin.ApiError):
                plugin.phone_from_suffix(value)

    def test_primary_moves_real_username_to_secondary(self):
        profile = plugin.validate_profile({"usernames": ["dev", "own"], "primary": "own"})
        self.assertEqual(plugin.visible_names(profile, "Real", ["nft", "REAL"]), ("own", ["real", "nft", "dev"]))

    def test_remote_fetches_are_bounded_and_use_profile_only(self):
        self.subject.settings["sync_enabled"] = True
        for owner in ("301", "302", "303"):
            self.subject._fetch_remote(0, "101", owner)
        self.assertEqual(len(NETWORK), 2)
        with patch.object(plugin, "post_json", return_value={"profile": {}}) as request:
            NETWORK.pop(0)()
            drain_ui()
        self.assertTrue(request.call_args.args[2]["profile_only"])
        for index in range(40):
            self.subject._remote_finish(0, ("101", str(400 + index)), self.subject._generation, plugin.validate_profile({}))
        self.assertEqual(len(self.subject._remote), plugin.REMOTE_LIMIT)

    def test_save_retry_keeps_operation_id_and_error_preserves_profile(self):
        self.subject.settings["sync_enabled"] = True
        self.subject._snapshots["101"] = snapshot()
        keys = ["names", "phone", "primary"]
        self.subject.settings.update(names="dev\nown", phone="12345678", primary="own", **{"draft_revision:101": 1})
        sent = []

        def request(path, user_id, body, timeout):
            sent.append(dict(body))
            raise plugin.ApiError("ASSET_TAKEN")

        with patch.object(plugin, "post_json", side_effect=request):
            for _ in range(2):
                self.subject._save("101", 0, keys)
                NETWORK.pop(0)()
                drain_ui()
        self.assertEqual(sent[0]["operation_id"], sent[1]["operation_id"])
        self.assertEqual(sent[0]["phone"], "+88812345678")
        self.assertEqual(self.subject._cache("101")["profile"]["usernames"], ["dev"])

    def test_remote_revision_change_blocks_stale_editor_save(self):
        self.subject.settings["sync_enabled"] = True
        self.subject._snapshots["101"] = snapshot(revision=2)
        self.subject.settings["draft_revision:101"] = 1
        self.subject._save("101", 0, ["names", "phone", "primary"])
        self.assertEqual(NETWORK, [])

    def test_profile_binding_changes_views_without_mutating_telegram_user(self):
        self.subject.settings["sync_enabled"] = True
        self.subject._native = True
        data = snapshot()
        data["profile"].update(phone="+888 1234 5678", primary="dev")
        self.subject._snapshots["101"] = data
        real_user = types.SimpleNamespace(username="real", phone="123456789", usernames=types.SimpleNamespace(size=lambda: 0))
        CONTROLLERS[0] = types.SimpleNamespace(getUser=lambda owner: real_user)
        fragment = types.SimpleNamespace(userId=101, phoneRow=2, usernameRow=3,
                                         getCurrentAccount=lambda: 0,
                                         getClass=lambda: types.SimpleNamespace(getName=lambda: "org.telegram.ui.ProfileActivity"))
        adapter = types.SimpleNamespace()
        setattr(adapter, "this$0", fragment)
        changes = []
        cell = types.SimpleNamespace(textView=types.SimpleNamespace(setText=lambda text: changes.append(("main", text))),
                                     valueTextView=types.SimpleNamespace(setText=lambda text: changes.append(("sub", text))))
        param = types.SimpleNamespace(thisObject=adapter, args=[types.SimpleNamespace(itemView=cell), 2])
        self.subject._bind_profile(param)
        self.assertEqual(changes[0], ("main", "+888 1234 5678"))
        self.assertEqual(real_user.phone, "123456789")
        self.assertEqual(real_user.username, "real")
        param.args[1] = 9
        self.subject._bind_profile(param)
        self.assertEqual(len(changes), 2)

    def test_unchanged_remote_profile_does_not_rebind(self):
        self.subject.settings["sync_enabled"] = True
        key = "101", "301"
        profile = plugin.validate_profile({"usernames": ["own"]})
        self.subject._remote[key] = 0, profile
        with patch.object(self.subject, "_rebind_visible") as redraw:
            self.subject._remote_finish(0, key, self.subject._generation, dict(profile))
        redraw.assert_not_called()

    def test_plugin_metadata_accepts_sdk_140(self):
        values = {node.targets[0].id: ast.literal_eval(node.value)
                  for node in ast.parse(source.read_text(encoding="utf-8")).body
                  if isinstance(node, ast.Assign) and isinstance(node.targets[0], ast.Name)
                  and node.targets[0].id.startswith("__")}
        self.assertEqual(source.suffix, ".plugin")
        self.assertEqual(values["__sdk_version__"], ">=1.4.0")
        self.assertEqual(values["__version__"], "0.3.0")

    def test_legacy_editor_without_edittext_or_logger(self):
        self.assertIsNone(plugin.EditText)
        self.assertFalse(hasattr(self.subject, "logger"))
        self.subject._snapshots["101"] = snapshot()
        fields = self.subject._editor()
        names = fields[1]
        self.assertEqual(names.key, "draft_names:101")
        self.assertIn("commas", names.subtext)
        self.subject.settings[names.key] = "dev, own"
        self.assertEqual(plugin.parse_names(self.subject.get_setting(names.key)), ["dev", "own"])

    def test_modern_editor_still_supports_multiple_lines(self):
        with patch.object(plugin, "EditText", Setting):
            field = self.subject._editor()[1]
        self.assertTrue(field.multiline)
        self.assertEqual(field.max_length, 660)

    def test_asset_span_hook_ignores_unregistered_links(self):
        self.subject.settings["sync_enabled"] = True
        self.subject._native = True
        registered = object()
        span = types.SimpleNamespace(getURL=lambda: "https://fragment.com/username/dev")
        reference = types.SimpleNamespace(get=lambda: registered)
        self.subject._span_fragments = types.SimpleNamespace(get=lambda value: reference if value is span else None)
        with patch.object(self.subject, "_profile_for", return_value=snapshot()["profile"]), \
             patch.object(self.subject, "_open_asset", return_value=True) as opened:
            self.assertFalse(self.subject._click_asset_span(types.SimpleNamespace(thisObject=object())))
            opened.assert_not_called()
            self.assertTrue(self.subject._click_asset_span(types.SimpleNamespace(thisObject=span)))
            opened.assert_called_once_with(registered, "username", "dev")
        self.subject.settings["sync_enabled"] = False
        self.assertFalse(self.subject._click_asset_span(types.SimpleNamespace(thisObject=span)))

if __name__ == "__main__":
    unittest.main()
