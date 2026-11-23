# BodyControl Principal Authentication Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace BodyControl authorization by SOME/IP client ID with a deployment-assigned OS principal and IPsec-protected AP-to-Gateway transport.

**Architecture:** vSomeIP UNIX credential policy prevents an untrusted local process from using the trusted routing manager, Linux nftables enforces the BodyControl process UID at AP egress, and matching XFRM protection is required at both AP and Gateway. The Gateway application treats SOME/IP client ID only as routing metadata and retains its payload, rate, vehicle-state, and LIN checks.

**Tech Stack:** C++20, vSomeIP 3.7.6, Python integration tests, Bash, nftables, Linux XFRM/IPsec

**Spec:** `docs/superpowers/specs/2026-11-17-bodycontrol-principal-auth-design.md`

## Global Constraints

- `BODY_CONTROL_UID` and `BODY_CONTROL_GID` are mandatory and have no repository defaults.
- Keep `ns_ap`, `ns_gateway`, 10.10.0.10/24, 10.10.0.1/24, and TCP/30512 unchanged.
- Do not store IPsec key material in the repository or result artifacts.
- SOME/IP client ID is routing metadata only.
- Do not weaken existing BodyControl payload, rate-limit, vehicle-state, or LIN checks.

## Review Focus

- Untrusted process using client ID `0x2002` must not establish the BodyControl flow.
- Untrusted process must not attach to the trusted vSomeIP routing manager and use it as a network deputy.
- Trusted principal using client ID `0x2FFF` must retain normal BodyControl behavior.
- Traffic to TCP/30512 without XFRM protection must be dropped at the Gateway.
- Re-running setup must replace keys and policies without accumulating stale state.
- VehicleState and DoIP traffic must retain their existing firewall behavior.

---

### Task 1: Regression scenario and client-ID decoupling

**Files:**
- Modify: `ECU/tests/integration/lin_scenarios.py`
- Modify: `ECU/scripts/run/lin-scenario-test.sh`
- Modify: `ECU/tests/unit/test_lin_master.cpp`
- Modify: `ECU/src/gateway/body_control.hpp`
- Modify: `ECU/src/gateway/body_control.cpp`
- Modify: `ECU/src/gateway/gateway_main.cpp`

**Interfaces:**
- Consumes: mandatory deployment UID/GID supplied through `BODY_CONTROL_UID` and `BODY_CONTROL_GID`
- Produces: `BodyControl(unsigned max_per_second, int64_t confirm_timeout_ns)` and `request(method, payload, now_ns, ignition_ok, lin, token)` with no caller identity parameter

- [ ] Add integration assertions for an untrusted principal using client ID `0x2002` and a trusted principal using `0x2FFF`; run the scenario and observe the unauthorized case fail before implementation.
- [ ] Change unit tests to demonstrate client-ID-independent policy and run them red against the old API.
- [ ] Remove the allowed-client set and client argument from `BodyControl` and its Gateway caller.
- [ ] Build and run `ctest --test-dir build -R lin_master --output-on-failure` green.

### Task 2: Principal and protected-transport enforcement

**Files:**
- Modify: `practice/scripts/setup.sh`
- Modify: `practice/scripts/cleanup.sh`
- Modify: `practice/firewall/ns_ap.nft`
- Modify: `practice/firewall/ns_gateway.nft`
- Modify: `ECU/tests/integration/lin_scenarios.py`
- Modify: `config/vsomeip/body_client.json`
- Modify: `config/vsomeip/body_client_untrusted.json`

**Interfaces:**
- Consumes: numeric `BODY_CONTROL_UID` and `BODY_CONTROL_GID`
- Produces: root-owned runtime credential/config record, vSomeIP credential and UDS policy, AP socket-owner allow rule, and matching XFRM state/policy identified by a BodyControl-specific reqid

- [ ] Extend the integration scenario to verify protected packets and clear TCP/30512 absence; run it red before network enforcement exists.
- [ ] Validate and record the deployment UID/GID, render credential-bound vSomeIP configurations, generate per-run AEAD keys, and install symmetric XFRM states and selector policies for TCP/30512.
- [ ] Enable vSomeIP credential checking and restrictive routing UDS permissions; verify a different-credential process cannot use the live routing manager.
- [ ] Parameterize the AP nftables policy with the UID and require XFRM policy at both boundaries.
- [ ] Make cleanup remove only the BodyControl runtime state owned by this practice setup.
- [ ] Recreate the practice network and run the full LIN scenario green.

### Task 3: Source of truth and full verification

**Files:**
- Modify: `project.md`
- Modify: `docs/notion/phase6-lin-body.md`

**Interfaces:**
- Consumes: verified behavior from Tasks 1 and 2
- Produces: explicit real-vehicle basis, deployment-specific UID boundary, ephemeral-key SIL limitation, and updated acceptance evidence

- [ ] Document that client ID is routing-only and that authorization is enforced by the dedicated principal plus protected transport.
- [ ] Document the mandatory deployment UID and the non-production key-management claim boundary.
- [ ] Run focused build/tests, the security trigger, the legitimate control, and the repository's applicable full test suite.
- [ ] Inspect the final diff for unrelated changes and run one fresh bypass/regression review.
