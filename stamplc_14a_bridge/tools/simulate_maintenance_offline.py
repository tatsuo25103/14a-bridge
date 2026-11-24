"""Deterministic simulation of maintenance-offline multi-inverter behavior.

This does not open a serial port and cannot write real inverter registers.
It models the proposed availability layer before it is added to firmware.
"""

from __future__ import annotations

from dataclasses import dataclass, field


OFFLINE_FAILURE_THRESHOLD = 3
RECOVERY_SUCCESS_THRESHOLD = 2


@dataclass
class Inverter:
    slave_id: int
    configured: bool = True
    reachable: bool = True
    failure_streak: int = 0
    recovery_streak: int = 0
    state: str = "OK"
    rse_percent: int | None = 100
    rse_watts: int | None = 10_000
    winter: bool | None = False
    ac_current_tenths: int | None = 30
    writes: list[str] = field(default_factory=list)

    def lose_communication(self) -> None:
        self.reachable = False
        self.recovery_streak = 0

    def health_check(self) -> None:
        if self.reachable:
            self.failure_streak = 0
            if self.state == "OFFLINE":
                self.recovery_streak += 1
                self.state = "CHECK" if self.recovery_streak < 2 else "SYNCING"
            elif self.state == "CHECK":
                self.recovery_streak += 1
                if self.recovery_streak >= RECOVERY_SUCCESS_THRESHOLD:
                    self.state = "SYNCING"
            return

        self.recovery_streak = 0
        self.failure_streak += 1
        self.state = (
            "OFFLINE"
            if self.failure_streak >= OFFLINE_FAILURE_THRESHOLD
            else "CHECK"
        )

    @property
    def active(self) -> bool:
        return self.configured and self.reachable and self.state != "OFFLINE"

    def write(self, operation: str) -> None:
        assert self.reachable, f"unsafe write attempted to offline ID{self.slave_id}"
        self.writes.append(operation)


class Controller:
    def __init__(self, devices: list[Inverter]) -> None:
        self.devices = devices
        self.desired_rse = 100
        self.desired_watts = 10_000
        self.desired_winter = False
        self.events: list[str] = []

    def apply_rse(self, percent: int, watts: int) -> None:
        self.desired_rse = percent
        self.desired_watts = watts
        for device in self.devices:
            if not device.active:
                self.events.append(
                    f"ID{device.slave_id} skipped: OFFLINE / output unknown"
                )
                continue
            device.write(f"RSE {percent}% -> {watts}W + verify")
            device.rse_percent = percent
            device.rse_watts = watts
            device.state = "OK"

    def apply_winter(self, enabled: bool) -> None:
        self.desired_winter = enabled
        for device in self.devices:
            if not device.active:
                self.events.append(
                    f"ID{device.slave_id} skipped: OFFLINE / AC state unknown"
                )
                continue
            if enabled:
                device.write("0x011F=1000 verify; 0x0007 AC enable verify")
                device.winter = True
                device.ac_current_tenths = 1000
            else:
                device.write("0x0007 AC disable verify; 0x011F=30 verify")
                device.winter = False
                device.ac_current_tenths = 30
            device.state = "OK"

    def synchronize_recovered(self, device: Inverter, *, succeed: bool = True) -> None:
        assert device.state == "SYNCING"
        if not succeed:
            device.state = "ERROR"
            self.events.append(
                f"ID{device.slave_id} recovery sync failed; remains isolated"
            )
            return
        device.write(
            f"RECOVERY RSE {self.desired_rse}% -> {self.desired_watts}W + verify"
        )
        device.rse_percent = self.desired_rse
        device.rse_watts = self.desired_watts
        if self.desired_winter:
            device.write("RECOVERY 0x011F=1000 verify; AC enable verify")
            device.winter = True
            device.ac_current_tenths = 1000
        else:
            device.write("RECOVERY AC disable verify; 0x011F=30 verify")
            device.winter = False
            device.ac_current_tenths = 30
        device.state = "OK"
        device.failure_streak = 0
        device.recovery_streak = 0
        self.events.append(f"ID{device.slave_id} recovered and synchronized")

    def summary(self, label: str) -> None:
        online = sum(device.state == "OK" for device in self.devices)
        configured = sum(device.configured for device in self.devices)
        mode = "WINTER" if self.desired_winter else "NORMAL"
        suffix = "" if online == configured else f" PARTIAL {online}/{configured}"
        print(f"\n[{label}] RSE={self.desired_rse}% MODE={mode}{suffix}")
        print("ID  Config  State     RSE      Output      AC mode   AC current  Writes")
        for device in self.devices:
            output = "UNKNOWN" if device.state == "OFFLINE" else f"{device.rse_watts}W"
            ac_mode = "UNKNOWN" if device.state == "OFFLINE" else (
                "WINTER" if device.winter else "NORMAL"
            )
            current = "UNKNOWN" if device.state == "OFFLINE" else (
                f"{device.ac_current_tenths / 10:.1f}A"
            )
            print(
                f"{device.slave_id:<3} yes     {device.state:<9} "
                f"{str(device.rse_percent) + '%':<8} {output:<11} "
                f"{ac_mode:<9} {current:<11} {len(device.writes)}"
            )


def run() -> None:
    devices = [Inverter(slave_id=slave_id) for slave_id in (2, 3, 4, 5)]
    controller = Controller(devices)
    id4 = devices[2]

    controller.summary("Initial: all online")

    id4.lose_communication()
    for _ in range(OFFLINE_FAILURE_THRESHOLD):
        id4.health_check()
    assert id4.state == "OFFLINE"
    assert id4.configured, "maintenance must not erase configuration"
    controller.summary("ID4 confirmed maintenance-offline")

    writes_before = len(id4.writes)
    controller.apply_rse(60, 6_000)
    assert len(id4.writes) == writes_before, "offline ID received an RSE write"
    assert all(d.rse_watts == 6_000 for d in devices if d.slave_id != 4)
    controller.summary("RSE 60% applied to reachable IDs")

    controller.apply_winter(True)
    assert len(id4.writes) == writes_before, "offline ID received a Winter write"
    assert all(d.winter for d in devices if d.slave_id != 4)
    controller.summary("Winter applied to reachable IDs")

    id4.reachable = True
    id4.health_check()
    assert id4.state == "CHECK"
    id4.health_check()
    assert id4.state == "SYNCING"
    controller.summary("ID4 returned; waiting for synchronization")

    controller.synchronize_recovered(id4)
    assert id4.rse_watts == 6_000 and id4.winter is True
    controller.summary("ID4 synchronized to current RSE and Winter")

    controller.apply_winter(False)
    assert all(not d.winter and d.ac_current_tenths == 30 for d in devices)
    controller.summary("All IDs returned to Normal")

    # Negative branch: a recovered device is never declared OK when its
    # synchronization readback fails.
    id4.lose_communication()
    for _ in range(OFFLINE_FAILURE_THRESHOLD):
        id4.health_check()
    id4.reachable = True
    id4.health_check()
    id4.health_check()
    writes_before_failed_sync = len(id4.writes)
    controller.synchronize_recovered(id4, succeed=False)
    assert id4.state == "ERROR"
    assert len(id4.writes) == writes_before_failed_sync

    print("\n[Safety assertions]")
    print("PASS: configured ID remains configured while offline")
    print("PASS: no write is sent to a confirmed-offline ID")
    print("PASS: reachable IDs continue RSE and Winter control")
    print("PASS: recovered ID receives the current state, not a stale state")
    print("PASS: failed recovery synchronization is isolated as ERROR")
    print("PASS: Normal leaves every synchronized ID at AC disabled / 3.0A")
    print("\nEvents:")
    for event in controller.events:
        print(f"- {event}")


if __name__ == "__main__":
    run()
