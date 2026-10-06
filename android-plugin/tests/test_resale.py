import types
import unittest
from unittest.mock import patch

from test_profile import plugin
from test_gifts import named, values

class Balance:
    def __init__(self, ton=False, amount=0, nanos=0):
        self.ton, self.amount, self.nanos = ton, amount, nanos

    def getClass(self):
        return types.SimpleNamespace(getSimpleName=lambda: "TL_starsTonAmount" if self.ton else "TL_starsAmount",
                                     newInstance=lambda: Balance(self.ton))

class OnClickListener:
    def __init__(self, callback):
        self.callback = callback

    def onClick(self, view):
        self.callback(view)

class Button:
    def __init__(self, callback):
        self.mListenerInfo = types.SimpleNamespace(mOnClickListener=OnClickListener(callback))

    def setOnClickListener(self, listener):
        self.mListenerInfo.mOnClickListener = listener

class ResaleTests(unittest.TestCase):
    def setUp(self):
        self.subject = plugin.VisuGramPlugin()
        self.subject._loaded = True
        self.subject.settings["visual_gifts"] = True

    def attach(self, callback, account=0):
        button = Button(callback)
        dialog = types.SimpleNamespace(positiveButton=button, currentAccount=account)
        with patch("android_utils.OnClickListener", OnClickListener, create=True):
            plugin.GiftConfirmationHook(self.subject).after_hooked_method(types.SimpleNamespace(thisObject=dialog))
        return button

    def balance_result(self, real, account=0):
        result = [real]
        param = types.SimpleNamespace(thisObject=types.SimpleNamespace(currentAccount=account),
                                      getResult=lambda: real, setResult=lambda value: result.append(value))
        plugin.GiftConfirmationBalanceHook(self.subject).after_hooked_method(param)
        return result[-1]

    def test_form_prices_are_readable_while_real_payment_requests_stay_blocked(self):
        for name in ("TL_payments_getPaymentForm", "TL_payments_getPaymentForm_layer222"):
            self.assertFalse(hasattr(self.subject.pre_request_hook(name, 0, object()), "strategy"))
        with patch.object(plugin, "run_on_ui_thread"):
            for name in plugin.PAYMENT_REQUESTS:
                self.assertEqual(self.subject.pre_request_hook(name, 0, object()).strategy, "cancel")

    def test_resale_keeps_original_price_currency_and_recipient(self):
        gift = named("TL_starGiftUnique", id=17, title="Pepe", slug="PlushPepe-17", num=17,
                     attributes=values([]), availability_issued=100, availability_total=1000)
        for recipient in (101, 202):
            for ton, price in ((False, 700), (True, 10_000_000_000)):
                with self.subTest(recipient=recipient, ton=ton):
                    form = types.SimpleNamespace(invoice=types.SimpleNamespace(prices=values([types.SimpleNamespace(amount=price)])))
                    results = []
                    param = types.SimpleNamespace(thisObject=types.SimpleNamespace(currentAccount=0, ton=ton),
                                                  args=[form, gift, recipient, None], setResult=lambda value: results.append(value))
                    with patch.object(self.subject, "_send_visual") as send:
                        plugin.GiftBuyHook(self.subject, resale=True).before_hooked_method(param)
                    self.assertEqual(results, [None])
                    self.assertEqual(send.call_args.args[0], 0)
                    self.assertEqual(send.call_args.args[1]["recipient_id"], str(recipient))
                    self.assertEqual(send.call_args.args[1]["price"], str(price))
                    self.assertEqual(send.call_args.args[1]["currency"], "TON" if ton else "XTR")
                    self.assertEqual(send.call_args.args[1]["slug"], gift.slug)

    def test_only_confirmation_uses_a_copy_of_the_balance(self):
        for ton in (False, True):
            with self.subTest(ton=ton):
                real = Balance(ton, amount=0, nanos=5)
                self.assertIs(self.balance_result(real), real)
                observed = []
                button = self.attach(lambda view: observed.append(self.balance_result(real)))
                button.mListenerInfo.mOnClickListener.onClick(object())
                virtual = observed[0]
                self.assertIsNot(virtual, real)
                self.assertEqual(virtual.amount, 8_000_000_000_000_000_000 if ton else 8_000_000_000)
                self.assertEqual(virtual.nanos, 0)
                self.assertLessEqual(virtual.amount if ton else virtual.amount * 1_000_000_000, 2**63 - 1)
                self.assertEqual((real.amount, real.nanos), (0, 5))
                self.assertIsNone(self.subject._gift_confirmation_scope)
                self.assertIs(self.balance_result(real), real)

    def test_other_accounts_and_threads_keep_the_real_balance(self):
        real = Balance(amount=12)
        results = []

        def clicked(view):
            results.append(self.balance_result(real, account=1))
            with patch.object(plugin, "get_ident", return_value=-1):
                results.append(self.balance_result(real))

        button = self.attach(clicked)
        button.mListenerInfo.mOnClickListener.onClick(object())
        self.assertEqual(results, [real, real])
        self.assertIsNone(self.subject._gift_confirmation_scope)

    def test_native_listener_error_clears_the_temporary_scope(self):
        def clicked(view):
            raise RuntimeError("native listener failed")

        button = self.attach(clicked)
        with self.assertRaises(RuntimeError):
            button.mListenerInfo.mOnClickListener.onClick(object())
        self.assertIsNone(self.subject._gift_confirmation_scope)

    def test_disabling_visual_mode_keeps_the_original_confirmation(self):
        real = Balance(amount=17)
        observed = []
        button = self.attach(lambda view: observed.append(self.balance_result(real)))
        self.subject.settings["visual_gifts"] = False
        button.mListenerInfo.mOnClickListener.onClick(object())
        self.assertEqual(observed, [real])
        self.assertIsNone(self.subject._gift_confirmation_scope)

    def test_wrapped_confirmation_still_intercepts_the_real_buy_method(self):
        stopped = []
        self.subject._send_visual = lambda *args: None
        gift = types.SimpleNamespace(stars=25)
        param = types.SimpleNamespace(thisObject=types.SimpleNamespace(currentAccount=0),
                                      args=[gift, False, False, 202, None, None],
                                      setResult=lambda value: stopped.append(value))
        with patch.object(plugin, "gift_payload", return_value={}):
            button = self.attach(lambda view: plugin.GiftBuyHook(self.subject).before_hooked_method(param))
            button.mListenerInfo.mOnClickListener.onClick(object())
        self.assertEqual(stopped, [None])
        self.assertIsNone(self.subject._gift_confirmation_scope)

if __name__ == "__main__":
    unittest.main()
