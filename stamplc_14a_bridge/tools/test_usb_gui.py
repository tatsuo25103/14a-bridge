from __future__ import annotations

import unittest

from tools.stamplc_usb_gui import parse_status_line


class UsbGuiParserTests(unittest.TestCase):
    def test_parse_rse_status(self) -> None:
        parsed = parse_status_line("RSE DI mask: 0x02  level: 60%")
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.kind, "rse")
        self.assertEqual(parsed.values["mask"], "0x02")
        self.assertEqual(parsed.values["level"], "60%")

    def test_parse_invalid_rse_status(self) -> None:
        parsed = parse_status_line("RSE DI mask: 0x00  level: INVALID")
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.values["level"], "INVALID")

    def test_parse_hold_and_profile(self) -> None:
        held = parse_status_line("RSE DI mask: 0x06  level: HOLD")
        self.assertIsNotNone(held)
        assert held is not None
        self.assertEqual(held.values["level"], "HOLD")
        profile = parse_status_line("RSE PROFILE=FNN_EZA_3")
        self.assertIsNotNone(profile)
        assert profile is not None
        self.assertEqual(profile.kind, "rse_profile")
        self.assertEqual(profile.values["profile"], "FNN_EZA_3")

    def test_parse_mode_and_register(self) -> None:
        parsed = parse_status_line(
            "Mode: DRY-RUN  RS485: 19200 baud  register: 0x04E5  quantity: 2"
        )
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.kind, "mode")
        self.assertEqual(parsed.values["baud"], "19200")
        self.assertEqual(parsed.values["register"], "0x04E5")

    def test_parse_inverter_row(self) -> None:
        parsed = parse_status_line(
            "3   yes        18000    15000  15000  10800   5400      0    10800     10800  yes"
        )
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.kind, "inverter")
        self.assertEqual(parsed.values["id"], "3")
        self.assertEqual(parsed.values["maximum"], "18000")
        self.assertEqual(parsed.values["inverter_limit"], "15000")
        self.assertEqual(parsed.values["requested"], "10800")
        self.assertEqual(parsed.values["healthy"], "yes")

    def test_parse_probe_result(self) -> None:
        parsed = parse_status_line(
            "PROBE ID=3 REGISTER=0x04E5 VALUE=10000 STATUS=OK DETAIL=readback verified"
        )
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.kind, "probe")
        self.assertEqual(parsed.values["id"], "3")
        self.assertEqual(parsed.values["readback"], "10000")
        self.assertEqual(parsed.values["status"], "OK")

    def test_parse_probe_retry(self) -> None:
        parsed = parse_status_line(
            "PROBE ID=2 REGISTER=0x04E5 VALUE=0 STATUS=RETRY "
            "DETAIL=timeout/incomplete response (failure 1/3)"
        )
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.kind, "probe")
        self.assertEqual(parsed.values["status"], "RETRY")

    def test_id_range_is_two_through_seven(self) -> None:
        self.assertIsNone(parse_status_line(
            "1   yes 10000 10000 10000 6000 3000 0 6000 6000 yes"
        ))
        parsed = parse_status_line(
            "7   yes 18000 15000 15000 10800 5400 0 10800 10800 yes"
        )
        self.assertIsNotNone(parsed)
        assert parsed is not None
        self.assertEqual(parsed.values["id"], "7")


if __name__ == "__main__":
    unittest.main()
