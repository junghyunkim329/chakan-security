"""Issue #8: bounded probes of the isolated practice network, never production interfaces."""

import argparse
import copy
import errno
import json
import os
import pwd
import re
import socket
import stat
import struct
import subprocess
import time
from contextlib import contextmanager
from pathlib import Path


def proxy_configuration(document, router_id):
    document = copy.deepcopy(document)
    # The application binary has a fixed name. Give the live router a local
    # name alias with its EXISTING ID, so this process elects Proxy, not Host.
    document["routing"] = "body_control_existing_router"
    document["applications"].append({"name": document["routing"], "id": router_id})
    return document


def proxy_denied(runtime, trace, endpoint):
    if "Instantiating routing manager [Proxy]." not in runtime or "Won't instantiate routing" in runtime:
        return False
    return any("connect(" in line and "AF_UNIX" in line and
               f'sun_path="{endpoint}"' in line and "EACCES" in line for line in trace.splitlines())


def trace_dropped(trace, protocol, source_port, destination_port):
    reached, denied = set(), set()
    for line in trace.splitlines():
        match = re.search(r"trace id ([0-9a-f]+) ", line)
        if not match:
            continue
        trace_id = match[1]
        if ("packet:" in line and "ip saddr 10.10.0.10 " in line and
                "ip daddr 10.10.0.1 " in line and
                re.search(rf"\b{protocol} sport {source_port}\b", line) and
                re.search(rf"\b{protocol} dport {destination_port}\b", line)):
            reached.add(trace_id)
        if " inet ns_fw " in line and ("policy drop" in line or "verdict drop" in line):
            denied.add(trace_id)
    return bool(reached & denied)


def direct_denied(probe, trusted_uid, before, after, trace):
    names = ("bodycontrol_tcp_observed", "bodycontrol_uid_matched", "bodycontrol_authorized")
    if any(name not in before or name not in after for name in names):
        return False
    return (probe.get("uid") not in (None, 0, int(trusted_uid)) and probe.get("attempted") is True and
            probe.get("connected") is False and probe.get("errno") in (errno.ETIMEDOUT, errno.EPERM, errno.EACCES) and
            after[names[0]] > before[names[0]] and after[names[1]] == before[names[1]] and
            after[names[2]] == before[names[2]] and
            trace_dropped(trace, "tcp", probe.get("source_port"), 30512))


def command(namespace, *args, **kwargs):
    return subprocess.run(["ip", "netns", "exec", namespace, *map(str, args)],
                          check=True, text=True, capture_output=True, timeout=10, **kwargs).stdout


def counters(ruleset):
    result = {}
    for item in ruleset["nftables"]:
        rule = item.get("rule", {})
        if "comment" in rule:
            for expr in rule.get("expr", []):
                if "counter" in expr:
                    result[rule["comment"]] = expr["counter"]["packets"]
    return result


def configuration_only(value):
    """Ignore only mutable packet/byte counts, retaining rules, handles and policies."""
    if isinstance(value, dict):
        return {key: configuration_only(child) for key, child in value.items()
                if key not in ("packets", "bytes", "metainfo")}
    if isinstance(value, list):
        return [configuration_only(child) for child in value
                if not isinstance(child, dict) or "metainfo" not in child]
    return value


def policy_configuration(text):
    """Keep ip -s xfrm policy configuration, excluding only mutable usage."""
    if not text.strip():
        raise ValueError("XFRM policy snapshot is empty")
    text = re.sub(r"(?m)^\s*lifetime current:\n\s*\d+\(bytes\), \d+\(packets\)\n\s*add [^\n]+ use [^\n]+\n", "", text)
    return re.sub(r"(?m)^\s*lastused [^\n]+\n", "", text).strip()


class BoundaryProbes:
    def __init__(self, root, out, body_uid, body_gid, untrusted_user):
        self.root, self.out = Path(root), Path(out)
        self.body_uid, self.body_gid = int(body_uid), int(body_gid)
        self.actor = pwd.getpwnam(untrusted_user)
        if self.actor.pw_uid in (0, self.body_uid):
            raise ValueError("probe actor must be a different non-root principal")
        self.reports = []
        self.script = Path(__file__).resolve()

    def save(self, report):
        self.reports.append(report)
        (self.out / "report.json").write_text(json.dumps({"boundary_probes": self.reports}, indent=2) + "\n")
        return report

    def snapshot(self, namespace):
        return json.loads(command(namespace, "nft", "-j", "list", "table", "inet", "ns_fw"))

    def actor_command(self, *args):
        return ["setpriv", f"--reuid={self.actor.pw_uid}", f"--regid={self.actor.pw_gid}",
                "--clear-groups", "--no-new-privs", "--reset-env", *map(str, args)]

    @contextmanager
    def observe(self, namespace, hook, label):
        # Observation only: no allow/drop rules or modifications to ns_fw/XFRM.
        table = f"body_probe_{os.getpid()}"
        before = self.snapshot(namespace)
        # XFRM's iproute2 printer emits text even with the global -j option.
        policies = command(namespace, "ip", "-s", "xfrm", "policy", "list")
        (self.out / f"{label}.xfrm-before.txt").write_text(policies)
        trace_path = self.out / f"{label}.nft-trace.txt"
        output = trace_path.open("w")
        process = None
        created = False
        try:
            command(namespace, "nft", "-f", "-", input=(
                f"create table inet {table}\n"
                f"add chain inet {table} observe {{ type filter hook {hook} priority -1; policy accept; }}\n"
                f"add rule inet {table} observe ip saddr 10.10.0.10 ip daddr 10.10.0.1 "
                "tcp dport 30512 meta nftrace set 1\n"
                f"add rule inet {table} observe ip saddr 10.10.0.10 ip daddr 10.10.0.1 "
                "udp dport 30511 meta nftrace set 1\n"))
            created = True
            process = subprocess.Popen(["ip", "netns", "exec", namespace, "stdbuf", "-oL", "nft", "monitor", "trace"],
                                       stdout=output, stderr=subprocess.STDOUT, text=True)
            time.sleep(0.3)
            if process.poll() is not None:
                raise RuntimeError("nft trace monitor failed")
            yield trace_path
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=3)
            output.close()
            if created:
                command(namespace, "nft", "delete", "table", "inet", table)
            remaining = json.loads(command(namespace, "nft", "-j", "list", "tables"))
            removed = not any(item.get("table", {}).get("name") == table for item in remaining["nftables"])
            unchanged = configuration_only(before) == configuration_only(self.snapshot(namespace))
            after_policies = command(namespace, "ip", "-s", "xfrm", "policy", "list")
            (self.out / f"{label}.xfrm-after.txt").write_text(after_policies)
            policy_unchanged = policy_configuration(policies) == policy_configuration(after_policies)
            self.save({"probe": label + "_restoration", "pass": removed and unchanged and policy_unchanged,
                       "observer_removed": removed, "firewall_unchanged": unchanged, "xfrm_policy_unchanged": policy_unchanged})

    def direct(self):
        before = counters(self.snapshot("ns_ap"))
        with self.observe("ns_ap", "output", "direct_socket") as trace_path:
            output = command("ns_ap", *self.actor_command("python3", "-I", self.script, "socket"))
            probe = json.loads(output)
            time.sleep(0.2)
            after = counters(self.snapshot("ns_ap"))
        trace = trace_path.read_text()
        return self.save({"probe": "direct_socket", "pass": direct_denied(probe, self.body_uid, before, after, trace),
                          "principal_and_result": probe, "before": before, "after": after,
                          "denied_boundary": "AP UID firewall", "trace_file": trace_path.name})

    def proxy(self, client, source_config, router_id, label):
        source = json.loads(Path(source_config).read_text())
        config = proxy_configuration(source, router_id)
        runtime_dir = Path("/run/chakan-security/vsomeip")
        config_path = runtime_dir / f"{label}_{os.getpid()}.json"
        endpoint = f"/tmp/{source['network']}-0"
        trace_path = self.out / f"{label}.connect-trace.txt"
        process = None
        created = False
        try:
            # Exclusive creation: never replace a deployment-owned file.
            with config_path.open("x") as file:
                file.write(json.dumps(config))
            created = True
            config_path.chmod(0o644)
            endpoint_stat = os.stat(endpoint)
            router_present = stat.S_ISSOCK(endpoint_stat.st_mode) and endpoint_stat.st_uid == self.body_uid
            args = self.actor_command("timeout", "--kill-after=1", "5", "strace", "-f", "-s", "256",
                                      "-e", "trace=connect", "-o", trace_path, "env",
                                      f"VSOMEIP_CONFIGURATION={config_path}",
                                      "LD_LIBRARY_PATH=/run/chakan-security/body-runtime/lib",
                                      "python3", "-I", self.script, "exec", "--binary", client)
            with (self.out / f"{label}.runtime.log").open("w") as output, (self.out / f"{label}.log").open("w") as errors:
                process = subprocess.Popen(["ip", "netns", "exec", "ns_ap", *args], stdin=subprocess.PIPE,
                                           stdout=output, stderr=errors, text=True)
                # Keep stdin open so the client can actually attempt UDS connection.
                process.wait(timeout=8)
                process.stdin.close()
            runtime = (self.out / f"{label}.runtime.log").read_text()
            principal = next((json.loads(line) for line in runtime.splitlines() if line.startswith('{"type": "probe_principal"')), {})
            trace = trace_path.read_text() if trace_path.exists() else ""
            passed = router_present and principal.get("uid") == self.actor.pw_uid and proxy_denied(runtime, trace, endpoint)
            return self.save({"probe": label, "pass": passed, "principal": principal,
                              "proxy_mode": "Instantiating routing manager [Proxy]." in runtime,
                              "routing_socket": endpoint, "trusted_router_present": router_present,
                              "exit_code": process.returncode, "denied_boundary": "UDS connect permission",
                              "trace_file": trace_path.name})
        finally:
            if process is not None and process.poll() is None:
                process.kill()
                process.wait(timeout=3)
            if created:
                config_path.unlink()
                self.save({"probe": label + "_restoration", "pass": not config_path.exists(),
                           "temporary_proxy_config_removed": not config_path.exists()})

    def unprotected(self):
        # Layer-2 injection bypasses only the AP egress path. The original
        # Gateway firewall and required inbound XFRM policy remain installed.
        link = json.loads(command("ns_gateway", "ip", "-j", "-d", "link", "show", "dev", "veth_gw"))[0]
        if link.get("linkinfo", {}).get("info_kind") != "veth":
            raise RuntimeError("Gateway interface is not a virtual test link")
        reports = []
        for protocol, port in (("tcp", 30512), ("udp", 30511)):
            label = "unprotected_" + protocol
            before = counters(self.snapshot("ns_gateway"))
            with self.observe("ns_gateway", "input", label) as trace_path:
                probe = json.loads(command("ns_ap", "python3", "-I", self.script, "raw",
                                           "--protocol", protocol, "--destination-mac", link["address"]))
                time.sleep(0.5)
                after = counters(self.snapshot("ns_gateway"))
            denied = trace_dropped(trace_path.read_text(), protocol, probe["source_port"], port)
            verified_unchanged = after.get("bodycontrol_ipsec_verified") == before.get("bodycontrol_ipsec_verified")
            reports.append(self.save({"probe": label, "pass": denied and verified_unchanged and probe["sent"] > 0,
                                      "principal_and_result": probe, "before": before, "after": after,
                                      "denied_boundary": "Gateway input firewall", "trace_file": trace_path.name}))
        return reports


def principal():
    return {"uid": os.getuid(), "gid": os.getgid(), "groups": os.getgroups(), "pid": os.getpid()}


def socket_probe():
    result = principal()
    result.update(attempted=False, connected=False, errno=None)
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as client:
        client.bind(("10.10.0.10", 0))
        result["source_port"] = client.getsockname()[1]
        client.settimeout(1.5)
        result["attempted"] = True
        try:
            client.connect(("10.10.0.1", 30512))
            result["connected"] = True
        except TimeoutError:
            result["errno"] = errno.ETIMEDOUT
        except OSError as error:
            result["errno"] = error.errno
    return result


def checksum(data):
    if len(data) % 2:
        data += b"\x00"
    total = sum(struct.unpack(f"!{len(data) // 2}H", data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def raw_packet(protocol, source_port, source_mac, destination_mac):
    src, dst = socket.inet_aton("10.10.0.10"), socket.inet_aton("10.10.0.1")
    if protocol == "tcp":
        number = socket.IPPROTO_TCP
        segment = struct.pack("!HHIIBBHHH", source_port, 30512, 8, 0, 0x50, 0x02, 1024, 0, 0)
        offset = 16
    else:
        number = socket.IPPROTO_UDP
        # SOME/IP SetWindowPosition(73), existing service/method/client IDs.
        request = struct.pack("!HHIHHBBBBB", 0x1100, 1, 9, 0x2002, 1, 1, 1, 0, 0, 73)
        segment = struct.pack("!HHHH", source_port, 30511, 8 + len(request), 0) + request
        offset = 6
    pseudo = src + dst + struct.pack("!BBH", 0, number, len(segment))
    check = checksum(pseudo + segment) or 0xFFFF
    segment = segment[:offset] + struct.pack("!H", check) + segment[offset + 2:]
    header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(segment), 8, 0, 64, number, 0, src, dst)
    header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
    mac = lambda value: bytes.fromhex(value.replace(":", ""))
    return mac(destination_mac) + mac(source_mac) + b"\x08\x00" + header + segment


def raw_probe(protocol, destination_mac):
    # Fail closed outside the existing isolated practice AP and veth interface.
    namespace = subprocess.check_output(["ip", "netns", "identify"], text=True).strip()
    link = json.loads(subprocess.check_output(["ip", "-j", "-d", "link", "show", "dev", "veth_ap"], text=True))[0]
    if namespace != "ns_ap" or link.get("linkinfo", {}).get("info_kind") != "veth":
        raise RuntimeError("raw probes require ns_ap/veth_ap virtual practice network")
    kind = socket.SOCK_STREAM if protocol == "tcp" else socket.SOCK_DGRAM
    with socket.socket(socket.AF_INET, kind) as reservation:
        reservation.bind(("10.10.0.10", 0))
        source_port = reservation.getsockname()[1]
        packet = raw_packet(protocol, source_port, link["address"], destination_mac)
        with socket.socket(socket.AF_PACKET, socket.SOCK_RAW, socket.htons(0x0800)) as sender:
            sender.bind(("veth_ap", 0))
            sent = sender.send(packet)
    return {**principal(), "test_only_root_injection": True, "protocol": protocol,
            "source_port": source_port, "sent": sent, "namespace": namespace, "interface": "veth_ap"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("socket", "raw", "exec"))
    parser.add_argument("--protocol", choices=("tcp", "udp"))
    parser.add_argument("--destination-mac")
    parser.add_argument("--binary")
    args = parser.parse_args()
    if args.mode == "exec":
        print(json.dumps({"type": "probe_principal", **principal()}), flush=True)
        os.execv(args.binary, [args.binary])
    elif args.mode == "socket":
        print(json.dumps(socket_probe()), flush=True)
    else:
        if not args.protocol or not args.destination_mac:
            parser.error("raw mode requires protocol and destination MAC")
        print(json.dumps(raw_probe(args.protocol, args.destination_mac)), flush=True)
