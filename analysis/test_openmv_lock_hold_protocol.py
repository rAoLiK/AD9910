"""Host regression tests for the OpenMV protocol-v2 lock-hold state."""

import ast
import gc
import struct
from pathlib import Path


class FakeDetector:
    def __init__(self):
        self.reset_count = 0
        self.visual_mode = None
        self.screen_outline_rect = None

    def reset_session(self):
        self.reset_count += 1

    def set_visual_lock_mode(self, lock_mode):
        self.visual_mode = lock_mode

    def update_visual(self, frame, now_ms):
        del frame, now_ms
        return (90000, 25, 80, 0x03)


class FakeSignalDetector:
    def __init__(self):
        self.confirm_frames = 6
        self.consecutive_frames = 0
        self.candidate_rect = None
        self.accept_next = False

    def reset(self):
        self.consecutive_frames = 0
        self.candidate_rect = None
        self.accept_next = False

    def update(self, frame):
        del frame
        if not self.accept_next:
            self.consecutive_frames = 0
            self.candidate_rect = None
            return False
        self.consecutive_frames = self.confirm_frames
        self.candidate_rect = (160, 40, 120, 150)
        return True


class FakeLink:
    def __init__(self):
        self.replies = []
        self.sent = []

    def send_ack(self, message_type, sequence, status):
        self.replies.append(("ACK", message_type, sequence, status))

    def send_nack(self, message_type, sequence, error):
        self.replies.append(("NACK", message_type, sequence, error))

    def send_new(self, message_type, payload):
        self.sent.append((message_type, payload))
        return b"", 0


def load_protocol_controller():
    source_path = (
        Path(__file__).resolve().parents[1]
        / "openmv"
        / "OpenMV_main_task5_uart.py"
    )
    source = source_path.read_text(encoding="utf-8")
    tree = ast.parse(source)
    selected = []
    prefixes = ("PROTOCOL_", "TYPE_", "ACK_", "ERR_", "STATE_", "COARSE_")
    for node in tree.body:
        if isinstance(node, ast.Assign):
            names = [
                target.id
                for target in node.targets
                if isinstance(target, ast.Name)
            ]
            if any(name.startswith(prefixes) for name in names):
                selected.append(node)
        elif isinstance(node, ast.ClassDef) and node.name == "Task5Controller":
            selected.append(node)

    namespace = {
        "gc": gc,
        "struct": struct,
        "InputSignalDetector": FakeSignalDetector,
        "LissajousStabilityDetector": FakeDetector,
    }
    module = ast.Module(body=selected, type_ignores=[])
    exec(compile(module, str(source_path), "exec"), namespace)
    namespace["MODEL_1K_SPEC"] = object()
    namespace["MODEL_10K_SPEC"] = object()
    namespace["ticks_ms"] = lambda: 0
    return namespace, source


def main():
    namespace, source = load_protocol_controller()
    assert namespace["PROTOCOL_VERSION"] == 0x02
    assert "【任务结束】任务%d，原因%d，已回到空闲" not in source

    link = FakeLink()
    controller = namespace["Task5Controller"](link)
    controller.state = namespace["STATE_WAIT_DDS"]
    controller.session_id = 7
    controller.lock_mode = 2

    payload = struct.pack("<HBII", 7, 2, 5200, 10400)
    command_key = (namespace["TYPE_LOCK_HOLD"], 0x21, payload)
    controller._handle_lock_hold(0x21, payload, command_key)
    assert controller.state == namespace["STATE_LOCK_HOLD"]
    assert controller.lock_input_frequency_hz == 5200
    assert controller.lock_output_frequency_hz == 10400
    assert controller.visual_seed_millihz == 10400000
    assert controller.stability_detector.visual_mode == 2
    assert link.replies[-1] == (
        "ACK",
        namespace["TYPE_LOCK_HOLD"],
        0x21,
        namespace["ACK_ACCEPTED"],
    )

    controller._handle_lock_hold(0x21, payload, command_key)
    assert controller.state == namespace["STATE_LOCK_HOLD"]
    assert link.replies[-1][-1] == namespace["ACK_DUPLICATE"]
    assert "STATE_LOCK_HOLD" in source
    assert "STATE_VISUAL_LOCK, STATE_LOCK_HOLD" in source
    controller.process_visual_lock_frame(object())
    assert link.sent[-1][0] == namespace["TYPE_VISUAL_LOCK_SAMPLE"]
    sample = struct.unpack("<HHIIHBB", link.sent[-1][1])
    assert sample[0] == 7
    assert sample[3:] == (90000, 25, 80, 0x03)

    # The retired automatic-success reason cannot release LOCK_HOLD.
    exit_zero = struct.pack("<HB", 7, 0)
    controller._handle_exit(
        0x22,
        exit_zero,
        (namespace["TYPE_EXIT_TASK"], 0x22, exit_zero),
    )
    assert controller.state == namespace["STATE_LOCK_HOLD"]
    assert link.replies[-1] == (
        "NACK",
        namespace["TYPE_EXIT_TASK"],
        0x22,
        namespace["ERR_RANGE"],
    )

    # Explicit operator exit remains supported and is idempotent.
    exit_user = struct.pack("<HB", 7, 1)
    exit_key = (namespace["TYPE_EXIT_TASK"], 0x23, exit_user)
    controller._handle_exit(0x23, exit_user, exit_key)
    assert controller.state == namespace["STATE_IDLE"]
    assert controller.last_completed_session == 7
    assert link.replies[-1][-1] == namespace["ACK_ACCEPTED"]
    controller._handle_exit(0x23, exit_user, exit_key)
    assert link.replies[-1][-1] == namespace["ACK_DUPLICATE"]

    # A different-session START while held is an explicit mode-button switch.
    switch_link = FakeLink()
    switch_controller = namespace["Task5Controller"](switch_link)
    switch_controller.state = namespace["STATE_LOCK_HOLD"]
    switch_controller.session_id = 7
    switch_controller.lock_mode = 2
    switch_payload = struct.pack("<HBI", 8, 1, 1000)
    switch_key = (namespace["TYPE_START_TASK"], 0x24, switch_payload)
    switch_controller._handle_start(0x24, switch_payload, switch_key)
    assert switch_controller.state == namespace["STATE_WAIT_SIGNAL"]
    assert switch_controller.session_id == 8
    assert switch_controller.lock_mode == 1
    assert switch_controller.pending_model_spec is None
    assert switch_link.replies[-1][-1] == namespace["ACK_ACCEPTED"]

    # START only selects the mode. No classifier may be loaded until the
    # signal-presence gate confirms a real two-dimensional trace.
    switch_controller.process_signal_frame(object())
    assert switch_controller.state == namespace["STATE_WAIT_SIGNAL"]
    assert switch_controller.pending_model_spec is None
    switch_controller.prepare_classifier_if_needed()
    assert switch_controller.classifier is None
    switch_controller.signal_detector.accept_next = True
    switch_controller.process_signal_frame(object())
    assert switch_controller.state == namespace["STATE_COARSE"]
    assert switch_controller.pending_model_spec is namespace["MODEL_1K_SPEC"]

    print("openmv lock-hold protocol tests passed")


if __name__ == "__main__":
    main()
