# BodyControl Principal Authentication Design

## Intent

Issue #1 is fixed by making the BodyControl authorization decision depend on a deployment-assigned OS principal and an authenticated AP-to-Gateway channel, not on the caller-controlled SOME/IP client ID. This preserves the existing service, payload, vehicle-state, rate-limit, and LIN behavior while making client ID a routing value only.

## Goals

- Only the BodyControl service principal may attach to the local vSomeIP routing manager or open the AP-to-Gateway TCP/30512 flow.
- TCP/30512 traffic must be protected by IPsec in both directions.
- A process using the approved client ID but a different OS principal must be rejected before the Gateway application receives a request.
- A BodyControl principal using a different client ID must retain normal service behavior.
- No long-lived IPsec secret may be stored in the repository or result artifacts.

## Non-goals and claim boundary

- This does not implement an OEM PKI, production key provisioning, AUTOSAR IAM, or claim AUTOSAR conformance.
- The SIL models deployment-assigned service credentials through mandatory `BODY_CONTROL_UID` and `BODY_CONTROL_GID` setup inputs. They deliberately have no project defaults because the numeric credentials belong to the target deployment.
- Setup generates per-run IPsec keys and installs them directly into the two network namespaces. This proves protected transport behavior, not production key lifecycle management.
- Compromise of the dedicated BodyControl principal remains inside the trusted computing base; client ID is not restored as a second authentication factor.

## Constraints

- The existing `ns_ap` and `ns_gateway` architecture, IP addresses, SOME/IP IDs, and ports remain unchanged.
- All ECU implementation work remains under `ECU/`; practice network controls remain under `practice/`.
- vSomeIP UNIX credential policy, nftables, and Linux XFRM are the enforcement points available in the current SIL.
- Existing user changes and unrelated services on TCP/30502 and TCP/13400 must not be affected.

## Approaches considered

1. Keep the Gateway client-ID allowlist. Rejected because the identity is self-declared and directly preserves the vulnerability.
2. Add only an AP-side UID firewall rule. Better, but node-to-node traffic is still unauthenticated and the Gateway cannot distinguish protected traffic from source-address spoofing within the SIL boundary.
3. Use a dedicated OS principal, vSomeIP local credential policy, AP UID egress policy, and bidirectional IPsec policy for TCP/30512. Selected because it closes both direct network access and local routing-manager confused-deputy paths while mirroring a real vehicle's application identity, inter-ECU trust boundary, and layered enforcement without introducing an unsubstantiated OEM architecture.
4. Split BodyControl into another VM or network namespace. Potentially realistic, but it changes the declared central-node architecture and addresses more than this finding requires.

## Design

`practice/scripts/setup.sh` requires numeric, non-root `BODY_CONTROL_UID` and `BODY_CONTROL_GID`, records them in root-owned runtime state, renders the BodyControl vSomeIP configurations into that runtime area, generates per-run AEAD keys, installs matching XFRM states and policies in `ns_ap` and `ns_gateway`, and loads nftables rules with the UID as a define. The rendered vSomeIP configuration enables `check_credentials`, binds the configured local client IDs and routing manager to the dedicated UID/GID, and restricts routing UDS permissions. The AP firewall permits TCP/30512 only when the socket owner matches the dedicated UID and the packet is covered by the BodyControl IPsec policy. The Gateway firewall accepts TCP/30512 only when the packet arrived under that policy.

`BodyControl` no longer receives or stores an allowed client-ID set. The Gateway passes only method, payload, state, time, LIN master, and response token. Existing validation, rate limiting, state gating, response codes, and security-event behavior remain.

The integration scenario runs the legitimate client as the dedicated UID/GID, then probes the live routing manager from an untrusted principal, proves an untrusted principal cannot get a response even when it uses client ID `0x2002`, and finally proves the trusted principal still succeeds with alternate client ID `0x2FFF`. Packet evidence must show ESP on the veth path and no clear TCP/30512 packet.

## Source basis

The design applies UN R155 mitigations for authenticating messages and controlling unauthorized access, while the functional safety impact claim remains limited to this SIL. AUTOSAR SOME/IP defines client ID as part of request correlation, not an authenticated application identity; AUTOSAR Adaptive IAM separates caller identity and policy enforcement. `project.md` remains the implementation source of truth and is updated with the SIL-specific decisions above.
