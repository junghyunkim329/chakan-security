import importlib.util
import json
import re
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "ECU/tools/security/render_body_vsomeip.py"
EVIDENCE_MODULE = ROOT / "ECU/tools/security/scenario_evidence.py"


class BodySecurityConfigTest(unittest.TestCase):
    def _load_evidence_module(self):
        spec = importlib.util.spec_from_file_location("scenario_evidence", EVIDENCE_MODULE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def test_recognizes_vsomeip_routing_lock_permission_denial(self):
        evidence = self._load_evidence_module()
        runtime = (
            "Could not open /tmp/vsomeip-sil-apbody.lck: errno 13\n"
            "configured as routing but other routing manager present. Won't instantiate routing\n"
        )
        self.assertEqual(evidence.routing_rejection("init failed", runtime), "routing lock permission denied")
        self.assertIsNone(evidence.routing_rejection("init failed", "generic init error"))

    def test_fade_steps_only_compare_contiguous_valid_samples(self):
        evidence = self._load_evidence_module()
        samples = [(0.0, 0), (0.1, 10), (0.2, 20), (0.5, 50), (0.6, 60)]
        self.assertEqual(evidence.contiguous_positive_steps(samples), [10, 10, 10])

    def test_firewall_exposes_keyless_bodycontrol_path_counters(self):
        ap_rules = (ROOT / "practice/firewall/ns_ap.nft").read_text()
        gateway_rules = (ROOT / "practice/firewall/ns_gateway.nft").read_text()

        for label in (
            "bodycontrol_esp_sent",
            "bodycontrol_tcp_observed",
            "bodycontrol_uid_matched",
            "bodycontrol_ipsec_matched",
            "bodycontrol_authorized",
        ):
            self.assertIn(f'comment "{label}"', ap_rules)
        for label in (
            "bodycontrol_esp_received",
            "bodycontrol_esp_sent",
            "bodycontrol_tcp_received",
            "bodycontrol_ipsec_verified",
        ):
            self.assertIn(f'comment "{label}"', gateway_rules)
        self.assertNotRegex(ap_rules + gateway_rules, re.compile(r'comment "bodycontrol_[^"]+"\s+accept'))
        self.assertIn('counter accept comment "bodycontrol_authorized"', ap_rules)
        self.assertIn('counter accept comment "bodycontrol_esp_received"', gateway_rules)
        self.assertIn('counter accept comment "bodycontrol_ipsec_verified"', gateway_rules)

    def test_binds_both_routing_ids_to_deployment_credentials(self):
        spec = importlib.util.spec_from_file_location("render_body_vsomeip", MODULE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as td:
            destination = Path(td) / "body.json"
            module.render(ROOT / "config/vsomeip/body_client.json", destination, 61001, 61002)
            document = json.loads(destination.read_text())

        self.assertEqual(document["file-permissions"]["permissions-uds"], "0600")
        self.assertEqual(document["routing-credentials"], {"uid": "61001", "gid": "61002"})
        policies = {policy["client"]: policy for policy in document["security"]["policies"]}
        self.assertEqual(set(policies), {"0x2002", "0x2FFF"})
        for policy in policies.values():
            self.assertEqual(policy["credentials"], {"uid": "61001", "gid": "61002"})
            self.assertEqual(policy["allow"]["requests"], [{"service": "0x1100", "instance": "0x0001"}])
        self.assertEqual(document["security"]["check_credentials"], "true")

    def test_rejects_root_or_out_of_range_credentials(self):
        spec = importlib.util.spec_from_file_location("render_body_vsomeip", MODULE)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for uid, gid in ((0, 1000), (1000, 0), (-1, 1000), (1000, 2**32)):
            with self.subTest(uid=uid, gid=gid), tempfile.TemporaryDirectory() as td:
                with self.assertRaises(ValueError):
                    module.render(ROOT / "config/vsomeip/body_client.json", Path(td) / "body.json", uid, gid)


if __name__ == "__main__":
    unittest.main()
