import importlib.util
import json
import os
import re
import subprocess
import struct
import tempfile
import unittest
from unittest.mock import patch, Mock
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MODULE = ROOT / "ECU/tools/security/render_body_vsomeip.py"
EVIDENCE_MODULE = ROOT / "ECU/tools/security/scenario_evidence.py"
PRINCIPAL_HELPER = ROOT / "ECU/scripts/lib/body-control-principal.sh"
BOUNDARY_MODULE = ROOT / "ECU/tests/integration/body_boundary_probes.py"


class BoundaryEvidenceTest(unittest.TestCase):
    def setUp(self):
        spec = importlib.util.spec_from_file_location("body_boundary_probes", BOUNDARY_MODULE)
        self.boundary = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.boundary)

    def test_observer_restoration_accepts_text_xfrm_and_detects_policy_changes(self):
        # iproute2 XFRM outputs text even when its global -j flag is set.
        # Mock only privileged kernel/process I/O; exercise real cleanup/reporting.
        policy = ("src 10.10.0.10/32 dst 10.10.0.1/32 proto tcp dport 30512 uid 0\n"
                  "\tdir out action allow index 1 priority 0 share any flag\n"
                  "\tlifetime config:\n\t  limit: soft (INF)(bytes), hard (INF)(bytes)\n"
                  "\tlifetime current:\n\t  0(bytes), 0(packets)\n"
                  "\t  add 2026-11-23 18:16:50 use 2026-11-23 23:52:13\n"
                  "\ttmpl src 10.10.0.10 dst 10.10.0.1\n"
                  "\t\tproto esp reqid 1100 mode transport\n\t\tlevel required share any\n")
        for mutation, passed in (("runtime", True), ("required", False), ("limit", False)):
            with self.subTest(mutation=mutation), tempfile.TemporaryDirectory() as directory:
                probe = self.boundary.BoundaryProbes.__new__(self.boundary.BoundaryProbes)
                probe.out, probe.reports = Path(directory), []
                probe.snapshot = lambda namespace: {"nftables": []}
                after = policy.replace("0(bytes), 0(packets)", "80(bytes), 2(packets)").replace("23:52:13", "23:52:14")
                if mutation == "required":
                    after = after.replace("level required", "level use")
                elif mutation == "limit":
                    after = after.replace("hard (INF)(bytes)", "hard 1000(bytes)")
                reads = iter((policy, after))

                def kernel_command(namespace, *args, **kwargs):
                    if "policy" in args:
                        return next(reads)
                    return '{"nftables": []}' if "tables" in args else ""

                monitor = Mock()
                monitor.poll.return_value = None
                with patch.object(self.boundary, "command", side_effect=kernel_command), \
                        patch.object(self.boundary.subprocess, "Popen", return_value=monitor):
                    try:
                        with probe.observe("ns_ap", "output", "regression"):
                            pass
                    except json.JSONDecodeError as error:
                        self.fail(f"text XFRM policy must not be decoded as JSON: {error}")
                report = json.loads((probe.out / "report.json").read_text())["boundary_probes"][0]
                self.assertEqual(report["pass"], passed)
                self.assertEqual(report["xfrm_policy_unchanged"], passed)
                self.assertTrue(report["observer_removed"])

    def test_proxy_preserves_security_policy_and_uses_existing_router_id(self):
        source = {"network": "lab", "routing": "body_control_client",
                  "applications": [{"name": "body_control_client", "id": "0x2FFF"}],
                  "security": {"check_credentials": "true"},
                  "routing-credentials": {"uid": "997", "gid": "973"},
                  "file-permissions": {"permissions-uds": "0600"}}
        actual = self.boundary.proxy_configuration(source, "0x2002")
        self.assertIsInstance(actual, dict)
        self.assertNotEqual(actual["routing"], "body_control_client")
        self.assertEqual(actual["applications"], [
            {"name": "body_control_client", "id": "0x2FFF"},
            {"name": actual["routing"], "id": "0x2002"}])
        for key in ("network", "security", "routing-credentials", "file-permissions"):
            self.assertEqual(actual[key], source[key])
        self.assertEqual(source["routing"], "body_control_client")

    def test_proxy_requires_real_connect_permission_denial(self):
        endpoint = "/tmp/vsomeip-sil-apbody-0"
        runtime = "Instantiating routing manager [Proxy]."
        trace = f'connect(5, {{sa_family=AF_UNIX, sun_path="{endpoint}"}}, 110) = -1 EACCES (Permission denied)'
        self.assertTrue(self.boundary.proxy_denied(runtime, trace, endpoint))
        self.assertFalse(self.boundary.proxy_denied(runtime, "", endpoint))
        self.assertFalse(self.boundary.proxy_denied(runtime, trace.replace("EACCES", "ENOENT"), endpoint))
        self.assertFalse(self.boundary.proxy_denied("routing lock permission denied", trace, endpoint))
        self.assertFalse(self.boundary.proxy_denied(runtime, trace, endpoint + "-other"))

    def test_drop_requires_same_trace_id_and_exact_packet(self):
        trace = ("trace id abc inet observer input packet: ip saddr 10.10.0.10 ip daddr 10.10.0.1 tcp sport 45123 tcp dport 30512\n"
                 "trace id abc inet ns_fw input policy drop\n")
        self.assertTrue(self.boundary.trace_dropped(trace, "tcp", 45123, 30512))
        self.assertFalse(self.boundary.trace_dropped(trace, "tcp", 45124, 30512))
        self.assertFalse(self.boundary.trace_dropped(trace.replace("id abc inet ns_fw", "id def inet ns_fw"), "tcp", 45123, 30512))
        self.assertFalse(self.boundary.trace_dropped(trace.replace("policy drop", "policy accept"), "tcp", 45123, 30512))

    def test_direct_requires_nonroot_principal_and_uid_counter_difference(self):
        probe = {"uid": 1001, "gid": 1001, "attempted": True, "connected": False,
                 "errno": 110, "source_port": 45123}
        before = {"bodycontrol_tcp_observed": 2, "bodycontrol_uid_matched": 2,
                  "bodycontrol_authorized": 2}
        after = {"bodycontrol_tcp_observed": 3, "bodycontrol_uid_matched": 2,
                 "bodycontrol_authorized": 2}
        trace = ("trace id abc inet observer output packet: ip saddr 10.10.0.10 ip daddr 10.10.0.1 tcp sport 45123 tcp dport 30512\n"
                 "trace id abc inet ns_fw output policy drop\n")
        self.assertTrue(self.boundary.direct_denied(probe, 997, before, after, trace))
        self.assertFalse(self.boundary.direct_denied({**probe, "uid": 0}, 997, before, after, trace))
        self.assertFalse(self.boundary.direct_denied({**probe, "connected": True}, 997, before, after, trace))
        self.assertFalse(self.boundary.direct_denied(probe, 997, before, before, trace))
        self.assertFalse(self.boundary.direct_denied(probe, 997, before, {**after, "bodycontrol_uid_matched": 3}, trace))
        self.assertFalse(self.boundary.direct_denied(probe, 997, before, after, ""))

    def test_raw_packets_have_valid_ip_transport_checksums_and_method(self):
        def valid_checksum(data):
            data += b"\x00" if len(data) % 2 else b""
            total = sum(struct.unpack(f"!{len(data) // 2}H", data))
            while total > 65535:
                total = (total & 65535) + (total >> 16)
            return total == 65535

        for protocol, number, port in (("tcp", 6, 30512), ("udp", 17, 30511)):
            packet = self.boundary.raw_packet(protocol, 45123, "02:00:00:00:00:10", "02:00:00:00:00:01")
            self.assertEqual(packet[:14], bytes.fromhex("0200000000010200000000100800"))
            ip, segment = packet[14:34], packet[34:]
            self.assertEqual(ip[9], number)
            self.assertEqual(struct.unpack("!H", ip[2:4])[0], len(packet) - 14)
            self.assertTrue(valid_checksum(ip))
            pseudo = ip[12:20] + bytes([0, number]) + struct.pack("!H", len(segment))
            self.assertTrue(valid_checksum(pseudo + segment))
            self.assertEqual(struct.unpack("!HH", segment[:4]), (45123, port))
            if protocol == "tcp":
                self.assertEqual(segment[13], 2)  # SYN
            else:
                self.assertEqual(segment[8:], bytes.fromhex("1100000100000009200200010101000049"))

    def test_restoration_comparison_keeps_policy_changes_but_ignores_counts(self):
        before = {"nftables": [{"metainfo": {"version": "v1"}},
                               {"rule": {"handle": 4, "expr": [{"counter": {"packets": 1, "bytes": 40}},
                                                                    {"match": {"right": ["syn", "ack", 30512]}}]}}]}
        after = json.loads(json.dumps(before))
        after["nftables"][1]["rule"]["expr"][0]["counter"]["packets"] = 9
        self.assertEqual(self.boundary.configuration_only(before), self.boundary.configuration_only(after))
        after["nftables"][1]["rule"]["expr"].append({"accept": None})
        self.assertNotEqual(self.boundary.configuration_only(before), self.boundary.configuration_only(after))


class PrincipalResolutionTest(unittest.TestCase):
    def resolve(self, variables=None, saved=(), account_exists=True):
        environment = {k: v for k, v in os.environ.items() if k not in ("BODY_CONTROL_UID", "BODY_CONTROL_GID")}
        environment.update(variables or {})
        lookup = (
            'id() { if [[ $1 == -u && $2 == vehicle-body-control ]]; then printf "997\\n"; '
            'elif [[ $1 == -g && $2 == vehicle-body-control ]]; then printf "973\\n"; else return 1; fi; }; '
            if account_exists else 'id() { return 1; }; '
        )
        script = lookup + 'source "$1"; shift; resolve_body_control_principal "$@" || exit 1; printf "%s %s\\n" "$BODY_CONTROL_UID" "$BODY_CONTROL_GID"'
        return subprocess.run(["bash", "-eu", "-c", script, "test", str(PRINCIPAL_HELPER), *saved],
                              env=environment, text=True, capture_output=True)

    def test_looks_up_deployment_account(self):
        result = self.resolve()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "997 973")

    def test_explicit_pair_works_without_account(self):
        result = self.resolve({"BODY_CONTROL_UID": "61001", "BODY_CONTROL_GID": "61002"}, account_exists=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "61001 61002")

    def test_missing_account_fails(self):
        result = self.resolve(account_exists=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("vehicle-body-control", result.stderr)

    def test_partial_empty_root_and_invalid_pairs_fail(self):
        for variables in (
            {"BODY_CONTROL_UID": "997"}, {"BODY_CONTROL_GID": "973"},
            {"BODY_CONTROL_UID": "", "BODY_CONTROL_GID": "973"},
            {"BODY_CONTROL_UID": "0", "BODY_CONTROL_GID": "973"},
            {"BODY_CONTROL_UID": "997", "BODY_CONTROL_GID": "0"},
            {"BODY_CONTROL_UID": "4294967296", "BODY_CONTROL_GID": "973"},
            {"BODY_CONTROL_UID": "other", "BODY_CONTROL_GID": "973"},
        ):
            with self.subTest(variables=variables):
                self.assertNotEqual(self.resolve(variables).returncode, 0)

    def test_runner_uses_saved_pair_without_account_lookup(self):
        result = self.resolve(saved=("997", "973"), account_exists=False)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "997 973")

    def test_explicit_pair_takes_precedence_over_saved_pair(self):
        result = self.resolve({"BODY_CONTROL_UID": "61001", "BODY_CONTROL_GID": "61002"}, saved=("997", "973"))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout.strip(), "61001 61002")


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
