import pathlib
import subprocess

repo = pathlib.Path(__file__).resolve().parents[2]
build = repo / "out"
critical = {
    "ayu/features/visual/visual_gifts.cpp",
    "ayu/features/visual/visual_gifts_boxes.cpp",
    "data/components/recent_shared_media_gifts.cpp",
    "boxes/star_gift_box.cpp",
    "boxes/send_credits_box.cpp",
    "boxes/transfer_gift_box.cpp",
    "api/api_premium.cpp",
    "payments/payments_form.cpp",
    "history/view/media/history_view_unique_gift.cpp",
    "history/history_item.cpp",
    "info/profile/info_profile_values.cpp",
    "info/peer_gifts/info_peer_gifts_common.cpp",
    "info/peer_gifts/info_peer_gifts_widget.cpp",
}
listing = subprocess.run(
    ["ninja", "-C", str(build), "-f", "build-Debug.ninja", "-t", "targets", "all"],
    check=True,
    capture_output=True,
    text=True,
).stdout
targets = {}
for line in listing.splitlines():
    target, separator, rule = line.partition(": ")
    normalized = target.replace("\\", "/")
    prefix = "Telegram/CMakeFiles/Telegram.dir/Debug/SourceFiles/"
    if not separator or not normalized.startswith(prefix):
        continue
    source = normalized.removeprefix(prefix).removesuffix(".obj")
    if source in critical and "CXX_COMPILER" in rule:
        targets[source] = target
missing = critical.difference(targets)
if missing:
    raise SystemExit("Missing visual build targets: " + ", ".join(sorted(missing)))
print("Compiling visual integrations before the full client.", flush=True)
subprocess.run(
    [
        "cmake", "--build", str(build), "--config", "Debug",
        "--target", *[targets[name] for name in sorted(targets)], "--parallel", "4",
    ],
    check=True,
)
