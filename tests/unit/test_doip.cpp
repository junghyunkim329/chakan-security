// DoIP (ISO 13400-2) Gateway message handling tests (project.md §9.1).
#include <cstring>

#include "doip.hpp"
#include "sil_test.h"

using namespace sil::doip;

namespace {

const std::set<std::uint16_t> ECUS{0x1001, 0x1002};

Bytes routing_activation(std::uint16_t sa, std::uint8_t type = 0x00)
{
	return message(kRoutingActivationRequest, {std::uint8_t(sa >> 8), std::uint8_t(sa), type, 0, 0, 0, 0});
}

Bytes diag(std::uint16_t sa, std::uint16_t ta, Bytes uds)
{
	Bytes p{std::uint8_t(sa >> 8), std::uint8_t(sa), std::uint8_t(ta >> 8), std::uint8_t(ta)};
	p.insert(p.end(), uds.begin(), uds.end());
	return message(kDiagnosticMessage, p);
}

std::vector<Event> feed(Session &s, const Bytes &b)
{
	return s.feed(b.data(), b.size());
}

bool has(const std::vector<Event> &ev, Event::Kind k)
{
	for (auto &e : ev)
		if (e.kind == k)
			return true;
	return false;
}

const Bytes *first_send(const std::vector<Event> &ev)
{
	for (auto &e : ev)
		if (e.kind == Event::Kind::SendToTester)
			return &e.data;
	return nullptr;
}

void test_header_format()
{
	Bytes m = message(kDiagnosticAck, {1, 2, 3});
	CHECK(m.size() == 11 && m[0] == 0x02 && m[1] == 0xFD && m[2] == 0x80 && m[3] == 0x02 && m[7] == 3);
}

void test_routing_activation()
{
	Session s(ECUS);
	auto ev = feed(s, routing_activation(0x0E80));
	const Bytes *r = first_send(ev);
	CHECK(r && be16(r->data() + 2) == kRoutingActivationResponse && r->size() == 8 + 9);
	CHECK(r && be16(r->data() + 8) == 0x0E80 && be16(r->data() + 10) == 0x1000 && (*r)[12] == kRaSuccess);
	CHECK(s.routing_active());
	CHECK(!has(ev, Event::Kind::Close));
}

void test_routing_denied()
{
	Session a(ECUS);
	auto ev = feed(a, routing_activation(0x0E81));
	CHECK(first_send(ev) && (*first_send(ev))[12] == kRaDeniedUnknownSourceAddress);
	CHECK(has(ev, Event::Kind::Close) && !a.routing_active());

	Session b(ECUS);
	ev = feed(b, routing_activation(0x0E80, 0x01)); // WWH-OBD activation type not supported
	CHECK(first_send(ev) && (*first_send(ev))[12] == kRaDeniedUnsupportedType);
	CHECK(has(ev, Event::Kind::Close));
}

void test_diag_forward_and_ack()
{
	Session s(ECUS);
	feed(s, routing_activation(0x0E80));
	auto ev = feed(s, diag(0x0E80, 0x1001, {0x22, 0xF1, 0x00}));
	CHECK(ev.size() == 1 && ev[0].kind == Event::Kind::Forward && ev[0].target == 0x1001);
	CHECK(ev[0].data == (Bytes{0x22, 0xF1, 0x00}));

	Bytes ack = s.diagnostic_ack(0x1001);
	CHECK(be16(ack.data() + 2) == kDiagnosticAck && be16(ack.data() + 8) == 0x1001 && be16(ack.data() + 10) == 0x0E80 && ack[12] == 0);
	const std::uint8_t uds[] = {0x62, 0xF1, 0x00, 0x03, 0x20};
	Bytes rsp = s.diagnostic_response(0x1001, uds, sizeof(uds));
	CHECK(be16(rsp.data() + 2) == kDiagnosticMessage && rsp.size() == 8 + 4 + 5 && be16(rsp.data() + 8) == 0x1001 && rsp[12] == 0x62);
}

void test_diag_before_activation_rejected()
{
	Session s(ECUS);
	auto ev = feed(s, diag(0x0E80, 0x1001, {0x22, 0xF1, 0x00}));
	CHECK(!has(ev, Event::Kind::Forward));
	CHECK(first_send(ev) && be16(first_send(ev)->data() + 2) == kDiagnosticNack && (*first_send(ev))[12] == kDiagNackInvalidSourceAddress);
	CHECK(has(ev, Event::Kind::Close));
}

void test_spoofed_source_and_unknown_target()
{
	Session s(ECUS);
	feed(s, routing_activation(0x0E80));
	auto ev = feed(s, diag(0x1000, 0x1001, {0x3E, 0x00})); // pretends to be the gateway
	CHECK(!has(ev, Event::Kind::Forward) && has(ev, Event::Kind::Close));

	Session t(ECUS);
	feed(t, routing_activation(0x0E80));
	ev = feed(t, diag(0x0E80, 0x1003, {0x3E, 0x00}));
	CHECK(!has(ev, Event::Kind::Forward) && !has(ev, Event::Kind::Close));
	CHECK(first_send(ev) && (*first_send(ev))[12] == kDiagNackUnknownTargetAddress);
	ev = feed(t, diag(0x0E80, 0x1000, {0x3E, 0x00})); // the gateway has no UDS server of its own
	CHECK(first_send(ev) && (*first_send(ev))[12] == kDiagNackUnknownTargetAddress);
}

void test_stream_reassembly()
{
	Session s(ECUS);
	Bytes all = routing_activation(0x0E80);
	Bytes d1 = diag(0x0E80, 0x1001, {0x22, 0xF1, 0x00});
	Bytes d2 = diag(0x0E80, 0x1002, {0x22, 0xF1, 0x01});
	all.insert(all.end(), d1.begin(), d1.end());
	all.insert(all.end(), d2.begin(), d2.end());

	std::vector<Event> ev;
	for (std::size_t i = 0; i < all.size(); i += 3) { // arrives in 3-byte pieces
		auto e = s.feed(all.data() + i, std::min<std::size_t>(3, all.size() - i));
		ev.insert(ev.end(), e.begin(), e.end());
	}
	int forwards = 0;
	for (auto &e : ev)
		forwards += e.kind == Event::Kind::Forward;
	CHECK_EQ_INT(forwards, 2);
}

void test_malformed_headers()
{
	Session a(ECUS);
	Bytes m = routing_activation(0x0E80);
	m[1] = 0x00; // inverse version wrong
	auto ev = feed(a, m);
	CHECK(first_send(ev) && be16(first_send(ev)->data() + 2) == kGenericNack && (*first_send(ev))[8] == kNackIncorrectPattern);
	CHECK(has(ev, Event::Kind::Close));

	Session b(ECUS);
	Bytes big{0x02, 0xFD, 0x80, 0x01, 0x00, 0x01, 0x00, 0x00}; // 65536-byte payload announced
	ev = feed(b, big);
	CHECK(first_send(ev) && (*first_send(ev))[8] == kNackMessageTooLarge && has(ev, Event::Kind::Close));

	Session c(ECUS);
	feed(c, routing_activation(0x0E80));
	ev = feed(c, message(0x4242, {}));
	CHECK(first_send(ev) && (*first_send(ev))[8] == kNackUnknownPayloadType && !has(ev, Event::Kind::Close));
	ev = feed(c, message(kRoutingActivationRequest, {0x0E, 0x80}));
	CHECK(first_send(ev) && (*first_send(ev))[8] == kNackInvalidPayloadLength && has(ev, Event::Kind::Close));
	// nothing is processed after close
	CHECK(feed(c, diag(0x0E80, 0x1001, {0x3E, 0x00})).empty());
}

void test_udp_vehicle_identification()
{
	Bytes req = message(kVehicleIdRequest, {});
	auto r = handle_udp(req.data(), req.size());
	CHECK(r && be16(r->data() + 2) == kVehicleAnnouncement && r->size() == 8 + 32);
	CHECK(r && std::memcmp(r->data() + 8, SIL_DOIP_VIN, 17) == 0 && be16(r->data() + 25) == 0x1000);

	req[0] = 0xFF;
	req[1] = 0x00; // default protocol version is allowed for identification
	CHECK(handle_udp(req.data(), req.size()).has_value());

	Bytes eid = message(kVehicleIdRequestEid, {0x02, 0, 0, 0, 0x10, 0});
	CHECK(handle_udp(eid.data(), eid.size()).has_value());
	Bytes other = message(kVehicleIdRequestEid, {0x02, 0, 0, 0, 0x10, 1});
	CHECK(!handle_udp(other.data(), other.size()).has_value());
	Bytes vin = message(kVehicleIdRequestVin, Bytes(SIL_DOIP_VIN, SIL_DOIP_VIN + 17));
	CHECK(handle_udp(vin.data(), vin.size()).has_value());

	Bytes bad = message(kVehicleIdRequest, {1});
	r = handle_udp(bad.data(), bad.size());
	CHECK(r && be16(r->data() + 2) == kGenericNack && (*r)[8] == kNackInvalidPayloadLength);
	CHECK(handle_udp(bad.data(), 3).has_value());
}

} // namespace

int main()
{
	test_header_format();
	test_routing_activation();
	test_routing_denied();
	test_diag_forward_and_ack();
	test_diag_before_activation_rejected();
	test_spoofed_source_and_unknown_target();
	test_stream_reassembly();
	test_malformed_headers();
	test_udp_vehicle_identification();
	return TEST_REPORT();
}
