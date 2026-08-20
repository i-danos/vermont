/*
 * Routing Plane interface class
 * Copyright (C) 2015 Brocade Communications Systems
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 *
 */

#ifdef ZMQ_SUPPORT_ENABLED

#include <czmq.h>
#include <boost/config.hpp>
#include <cstdint>

#include "RoutingPlane.hpp"
#include "common/msg.h"
#include "common/Time.h"

/*
 * Static array, used to easily match an error code to a human readable string,
 * for debug and logging purposes.
 */
const char* routing_plane_errors[] = {
		"ROUTING_PLANE_SUCCESS",
		"ROUTING_PLANE_ERROR_GENERAL",
		"ROUTING_PLANE_ERROR_ZMQ_API",
		"ROUTING_PLANE_ERROR_INPUT",
		"ROUTING_PLANE_INFO_TIMEOUT",
		"ROUTING_PLANE_INFO_INTERRUPTED",
		"ROUTING_PLANE_MISMATCHING_AF",
		"ROUTING_PLANE_ERROR_EMPTY_REPLY",
		"ROUTING_PLANE_ERROR_UNKNOWN_AF",
		"ROUTING_PLANE_ERROR_INVALID_ARGUMENT",
		"ROUTING_PLANE_ERROR_INVALID_REP_TYPE",
		"ROUTING_PLANE_ERROR_INVALID_REP_SIZE",
		"ROUTING_PLANE_ERROR_REPLY_NO_IP",
		"ROUTING_PLANE_ERROR_REPLY_MULTIPLE_IP",
		"ROUTING_PLANE_ERROR_INVALID_FLAGS"
};

/*
 * The caches and the lock are static, so that they get shared between all the
 * RoutinPlane modules, in order to absolutely minimize sending requests.
 */
std::unordered_map<std::pair<routing_plane_data_t *, uint16_t>,
	routing_plane_data_t *, IpHasher, IpMatcher> RoutingPlane::cache_ip;
std::unordered_map<routing_plane_data_t *, routing_plane_data_t *,
	SubnetHasher, SubnetMatcher> RoutingPlane::cache_subnet;
Mutex RoutingPlane::cache_lock;


void
RoutingPlane::clear_cache ()
{
	msg(LOG_INFO, "Routing Plane API: beginning cache clear");

	RoutingPlane::cache_lock.lock();

	for (auto &it : RoutingPlane::cache_subnet) {
		routing_plane_data_t *tmp_route =
				(routing_plane_data_t *)RoutingPlane::cache_subnet[it.second];
		RoutingPlane::cache_subnet[tmp_route] = NULL;
		RoutingPlane::routing_plane_cleanup_data_t(tmp_route, true);
		free(tmp_route);
	}

	RoutingPlane::cache_subnet.clear();
	// Content is pointers to the same elements of cache_subnet
	RoutingPlane::cache_ip.clear();

	RoutingPlane::cache_lock.unlock();

	msg(LOG_INFO, "Routing Plane API: finished cache clear");
}

void
RoutingPlane::setUpHelper ()
{
	zpoller = zpoller_new(NULL);
	if (!zpoller) {
		THROWEXCEPTION("Could not create ZMQ poller, cannot start "
				"Routing Plane interface");
	}

	zmq_sub_socket = zsock_new(ZMQ_SUB);
	if (!zmq_sub_socket) {
		THROWEXCEPTION("RoutingPlane: Could not create ZMQ SUB socket");
	}

	zsock_set_sndhwm(zmq_sub_socket, zmq_high_watermark);
	zsock_set_rcvhwm(zmq_sub_socket, zmq_high_watermark);

	if (zsock_connect(zmq_sub_socket, "%s", zmq_sub_endpoint.c_str())) {
		THROWEXCEPTION("RoutingPlane: Could not connect ZMQ SUB socket %s",
			zmq_sub_endpoint.c_str());
	}

	zsock_set_subscribe(zmq_sub_socket, "");

	zmq_dealer_socket = zsock_new(ZMQ_DEALER);
	if (!zmq_dealer_socket) {
		THROWEXCEPTION("RoutingPlane: Could not create ZMQ DEALER socket");
	}

	zsock_set_sndhwm(zmq_dealer_socket, zmq_high_watermark);
	zsock_set_rcvhwm(zmq_dealer_socket, zmq_high_watermark);

	if (zsock_connect(zmq_dealer_socket, "%s", zmq_dealer_endpoint.c_str())) {
		THROWEXCEPTION("RoutingPlane: Could not connect ZMQ DEALER socket %s",
			zmq_dealer_endpoint.c_str());
	}

	// discard buffered messages when socket is destroyed
	zsock_set_linger(zmq_dealer_socket, 0);

	if (0 != zpoller_add(zpoller, zmq_sub_socket)) {
		THROWEXCEPTION("Could not add %s ZMQ socket to ZMQ poller",
				zmq_sub_endpoint.c_str());
	}

	msg(LOG_NOTICE, "RoutingPlane listening on %s", zmq_sub_endpoint.c_str());
	msg(LOG_NOTICE, "RoutingPlane connected to %s", zmq_dealer_endpoint.c_str());

	exitFlag = false;
	thread.run(this);
}

void
RoutingPlane::tearDownHelper ()
{
	// stop pub-sub thread
	exitFlag = true;
	thread.join();

	zpoller_destroy(&zpoller);
	zsock_destroy(&zmq_sub_socket);
	zsock_destroy(&zmq_dealer_socket);

	msg(LOG_NOTICE, "Routing Plane ZMQ poller and sockets destroyed");
}

RoutingPlane::RoutingPlane(std::string zmq_sub_endpoint,
		std::string zmq_dealer_endpoint, int zmq_high_watermark,
		int zmq_poll_timeout, uint32_t cache_wiping_interval)
: zmq_sub_endpoint(zmq_sub_endpoint),
  zmq_dealer_endpoint(zmq_dealer_endpoint),
  zmq_high_watermark(zmq_high_watermark), zmq_poll_timeout(zmq_poll_timeout),
  cache_wiping_interval(cache_wiping_interval),
  thread(threadWrapper, "RPUpdater")
{
	if (zmq_sub_endpoint.empty() || zmq_dealer_endpoint.empty()) {
		THROWEXCEPTION("Need DEALER and SUB endpoints, cannot start "
				"Routing Plane interface");
	}

	// Stop CZMQ from hijacking signal handling
	zsys_handler_set(NULL);

	setUpHelper();
}

RoutingPlane::~RoutingPlane()
{
	tearDownHelper();
}

void
RoutingPlane::reconfigure(std::string zmq_sub_endpoint,
		std::string zmq_dealer_endpoint, int zmq_high_watermark,
		int zmq_poll_timeout, uint32_t cache_wiping_interval)
{
	if (zmq_dealer_endpoint.empty() || zmq_sub_endpoint.empty()) {
		THROWEXCEPTION("Need DEALER and SUB endpoints, cannot start "
				"Routing Plane interface");
	}

	if (this->zmq_poll_timeout != zmq_poll_timeout) {
		this->zmq_poll_timeout = zmq_poll_timeout;
	}

	if (this->zmq_high_watermark != zmq_high_watermark) {
		this->zmq_high_watermark = zmq_high_watermark;

		zsock_set_sndhwm(zmq_sub_socket, zmq_high_watermark);
		zsock_set_rcvhwm(zmq_sub_socket, zmq_high_watermark);
		zsock_set_sndhwm(zmq_dealer_socket, zmq_high_watermark);
		zsock_set_rcvhwm(zmq_dealer_socket, zmq_high_watermark);
	}

	if (this->cache_wiping_interval != cache_wiping_interval) {
		this->cache_wiping_interval = cache_wiping_interval;
	}

	if (this->zmq_dealer_endpoint != zmq_dealer_endpoint ||
			this->zmq_sub_endpoint != zmq_sub_endpoint) {
		tearDownHelper();
		setUpHelper();
	}
}

static int
routing_plane_data_t_deep_copy (routing_plane_data_t *output,
		routing_plane_data_t *input, bool copy_ip_addresses)
{
	RoutingPlane::routing_plane_cleanup_data_t(output, copy_ip_addresses);

	output->host_asn = input->host_asn;
	output->adj_asn = input->adj_asn;
	output->pbr_table_id = input->pbr_table_id;
	output->vrf_id = input->vrf_id;
	output->prefix_length = input->prefix_length;

	if (copy_ip_addresses && input->ip_count) {
		output->family = input->family;
		output->ip_count = input->ip_count;
		output->ip_addresses = (ip46 *)malloc(sizeof(ip46) *
				input->ip_count);
		if (!output->ip_addresses) {
			return -1;
		}
		memcpy(output->ip_addresses, input->ip_addresses, sizeof(ip46) *
				input->ip_count);
	}

	if (copy_ip_addresses) {
		memcpy(&output->ip_subnet, &input->ip_subnet, sizeof(ip46));
	}

	if (input->vrf_name_length) {
		output->vrf_name_length = input->vrf_name_length;
		output->vrf_name = (char *)malloc(input->vrf_name_length);
		if (!output->vrf_name) {
			if (copy_ip_addresses) {
				free(output->ip_addresses);
			}
			return -1;
		}
		memcpy(output->vrf_name, input->vrf_name, input->vrf_name_length);
	}

	if (input->ip_next_hop_count) {
		output->ip_next_hop_count = input->ip_next_hop_count;
		output->ip_next_hops = (ip46 *)malloc(sizeof(ip46) *
				input->ip_next_hop_count);
		if (!output->ip_next_hops) {
			if (copy_ip_addresses) {
				free(output->ip_addresses);
			}
			free(output->vrf_name);
			return -1;
		}
		memcpy(output->ip_next_hops, input->ip_next_hops, sizeof(ip46) *
				input->ip_next_hop_count);
	}

	if (input->bgp_next_hop_count) {
		output->bgp_next_hop_count = input->bgp_next_hop_count;
		output->bgp_next_hops = (ip46 *)malloc(sizeof(ip46) *
				input->bgp_next_hop_count);
		if (!output->bgp_next_hops) {
			if (copy_ip_addresses) {
				free(output->ip_addresses);
			}
			free(output->vrf_name);
			free(output->ip_next_hops);
			return -1;
		}
		memcpy(output->bgp_next_hops, input->bgp_next_hops, sizeof(ip46) *
				input->bgp_next_hop_count);
	}

	return 0;
}

void
RoutingPlane::routing_plane_cleanup_data_t (routing_plane_data_t *routing_data,
		bool clean_ip_addresses)
{
	if (routing_data) {
		DPRINTF_DEBUG("Routing Plane API: freeing routing_data buffers");

		if (clean_ip_addresses && routing_data->ip_count) {
			free(routing_data->ip_addresses);
			routing_data->ip_addresses = NULL;
			routing_data->ip_count = 0;
		}

		if (routing_data->vrf_name_length) {
			free(routing_data->vrf_name);
			routing_data->vrf_name = NULL;
			routing_data->vrf_name_length = 0;
		}

		if (routing_data->bgp_next_hop_count) {
			free(routing_data->bgp_next_hops);
			routing_data->bgp_next_hops = NULL;
			routing_data->bgp_next_hop_count = 0;
		}

		if (routing_data->ip_next_hop_count) {
			free(routing_data->ip_next_hops);
			routing_data->ip_next_hops = NULL;
			routing_data->ip_next_hop_count = 0;
		}
	}
}

static inline void
set_subnet_from_ip_prefix (routing_plane_data_t *routing_data)
{
	char debug_buffer[40];

	if (routing_data->ip_count == 0) {
		return;
	}
	/*
	 * Derive subnet value from IP and Prefix Length
	 */
	if (routing_data->family == AF_INET) {
		routing_data->ip_subnet.addr.s_addr = htonl((UINT32_MAX <<
			(32 - routing_data->prefix_length)) &
			ntohl(routing_data->ip_addresses[0].addr.s_addr));
	} else if (routing_data->family == AF_INET6) {
		/*
		 * For IPv6, not so easy to do it efficiently and sanely.
		 * GCC/Clang come in our help, but we really should avoid
		 * hard dependency on the compiler, so we have a fall back.
		 */
#if defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
		/*
		 * Compilers define 128 bit type, but do not support 128 constants.
		 * Ridiculous! We have to use memset instead of shifting UINT_MAX128.
		 */
		memset(&routing_data->ip_subnet, UINT8_MAX, sizeof(unsigned __int128));
		routing_data->ip_subnet.v128 = h128tobe(
				(routing_data->ip_subnet.v128 <<
						(128 - routing_data->prefix_length)) &
				be128toh(routing_data->ip_addresses[0].v128));
#pragma GCC diagnostic pop
#else
		memset(&routing_data->ip_subnet, 0, sizeof(ip46));
#endif
	}

	inet_ntop(routing_data->family, &routing_data->ip_subnet, debug_buffer,
			40);
	DPRINTF_DEBUG("Routing Plane API: parsing response: subnet %s",
			debug_buffer);
}

static inline bool
parse_ip_address (bool *parsed_ip, uint16_t ip_length,
		routing_plane_data_t *routing_data, uint8_t *data)
{
	char debug_buffer[40];

	/*
	 * We expect _exactly_ one IP, otherwise we can't match the data to
	 * the address it refers to. If a frame has multiple IPs, raise an
	 * error so that the frame gets discarded.
	 */
	if (BOOST_UNLIKELY(*parsed_ip)) {
		return false;
	}

	/*
	 * Message is an update about subnet, so record the IP
	 */
	if (routing_data->ip_count == 0) {
		routing_data->ip_count = 1;
		routing_data->ip_addresses = (ip46 *)malloc(sizeof(ip46));
		if (!routing_data->ip_addresses) {
			return false;
		}
		memcpy(&routing_data->ip_addresses[0], data, ip_length);

		*parsed_ip = true;

		return true;
	}

	for (uint16_t i = 0; i < routing_data->ip_count; ++i) {
		if (!memcmp(&routing_data->ip_addresses[i], data, ip_length)) {
			inet_ntop(routing_data->family, &routing_data->ip_addresses[i],
					debug_buffer, 40);
			DPRINTF_DEBUG("Routing Plane API: parsing response: IP %s",
					debug_buffer);
			*parsed_ip = true;
			return true;
		}
	}

	inet_ntop(routing_data->family, data, debug_buffer,
			40);
	msg(LOG_ERR, "Routing Plane API: parsed IP does not match: %s",
			debug_buffer);

	return false;
}

static inline bool
parse_vrf_name (uint16_t data_length, routing_plane_data_t *routing_data,
		uint8_t *data)
{
	routing_data->vrf_name_length = data_length;
	routing_data->vrf_name = (char*)malloc(data_length + 1);
	if (BOOST_UNLIKELY(!routing_data->vrf_name)) {
		return false;
	}

	/*
	 * As per protocol definition, vrf_name will NOT be NULL-terminated
	 * and I _really_ don't like that, so we shove in a \0 at the end.
	 */
	memcpy(routing_data->vrf_name, data, data_length);
	routing_data->vrf_name[data_length] = '\0';

	DPRINTF_DEBUG("Routing Plane API: parsing response: VRF %s",
			routing_data->vrf_name);

	return true;
}

static inline bool
parse_ip_nexthop (uint16_t data_length, uint16_t ip_length,
		routing_plane_data_t *routing_data, uint8_t *data)
{
	uint16_t j;

	if (routing_data->family == AF_INET) {
		routing_data->ip_next_hop_count = data_length / sizeof(struct in_addr);
	} else {
		routing_data->ip_next_hop_count = data_length / sizeof(struct in6_addr);
	}

	routing_data->ip_next_hops = (ip46*)malloc(sizeof (ip46) *
			routing_data->ip_next_hop_count);
	if (BOOST_UNLIKELY(!routing_data->ip_next_hops)) {
		return false;
	}

	for (j = 0; j < routing_data->ip_next_hop_count; ++j) {
		memcpy(&routing_data->ip_next_hops[j], data + j * ip_length,
				ip_length);
	}

	DPRINTF_DEBUG("Routing Plane API: parsing response: %u IP next hop parsed",
			routing_data->ip_next_hop_count);

	return true;
}

static inline bool
parse_bgp_nexthop (uint16_t data_length, uint16_t ip_length,
		routing_plane_data_t* routing_data, uint8_t *data)
{
	uint16_t j;

	if (routing_data->family == AF_INET) {
		routing_data->bgp_next_hop_count = data_length / sizeof(struct in_addr);
	} else {
		routing_data->bgp_next_hop_count = data_length /
				sizeof(struct in6_addr);
	}

	routing_data->bgp_next_hops = (ip46*)malloc(sizeof (ip46) *
			routing_data->bgp_next_hop_count);
	if (BOOST_UNLIKELY(!routing_data->bgp_next_hops)) {
		return false;
	}

	for (j = 0;j < routing_data->bgp_next_hop_count;++j) {
		memcpy(&routing_data->bgp_next_hops[j], data + j * ip_length,
				ip_length);
	}

	DPRINTF_DEBUG("Routing Plane API: parsing response: %u BGP next hop "
			"parsed", routing_data->bgp_next_hop_count);

	return true;
}

/**
 * Parses data block from a Routing Plane message.
 *
 * @param[in,out]  routing_data  pointers to routing_plane_data_t to be filled
 *
 * @param[in]      data   pointer to byte array, data block from Routing Plane
 *
 * @param[in]      payload_length  total length of data, for sanity check
 *
 * @return         int, number of bytes read, negative on error
 */
static int
parse_data (routing_plane_data_t *routing_data, uint8_t *data,
		size_t payload_length)
{
	size_t ret;
	uint16_t data_length, data_type, ip_length = routing_data->family ==
			AF_INET ? sizeof(struct in_addr) : sizeof(struct in6_addr);
	/*
	 * We need at the very least an IP address. A frame might contain a lot of
	 * data, but no address, and we have to discard it if that's the case.
	 */
	bool parsed_ip = false;

	/*
	 * Iterate over all the data relative to one IP address.
	 */
	for (ret = 0; ret + NF_DATA_HEADER_LENGTH <= payload_length;
			ret += NF_DATA_HEADER_LENGTH + data_length, data += data_length) {
		data_type = NF_MSG_GET_DATA_TYPE(data);
		data_length = NF_MSG_GET_DATA_LENGTH(data);

		/*
		 * payload_length is set by ZMQ, so we give it a higher level of trust.
		 * Routing Plane might be communicating corrupted or wrong data in the
		 * header length field.
		 */
		if (BOOST_UNLIKELY(ret + NF_DATA_HEADER_LENGTH + data_length >
				   payload_length)) {
			ret = ROUTING_PLANE_ERROR_INVALID_REP_SIZE;
			goto error;
		}

		data += NF_DATA_HEADER_LENGTH;

		switch (data_type) {
		case NF_DATA_TYPE_IP_ADDRESS:
			if (BOOST_UNLIKELY(data_length < ip_length)) {
				break;
			}
			if (BOOST_UNLIKELY(!parse_ip_address(&parsed_ip, ip_length,
					routing_data,
					data))) {
				goto error;
			}
			break;
		case NF_DATA_TYPE_PBR_TABLE_ID:
			routing_data->pbr_table_id = *data;

			DPRINTF_DEBUG("Routing Plane API: parsing response: PBR ID %u",
					routing_data->pbr_table_id);

			break;
		case NF_DATA_TYPE_VRF_NAME:
			if (BOOST_UNLIKELY(data_length < 1)) {
				break;
			}
			if (BOOST_UNLIKELY(!parse_vrf_name(data_length, routing_data,
					data))) {
				goto error;
			}
			break;
		case NF_DATA_TYPE_VRF_ID:
			if (BOOST_UNLIKELY(data_length < sizeof(uint32_t))) {
				break;
			}
			routing_data->vrf_id = *(uint32_t *)data;

			DPRINTF_DEBUG("Routing Plane API: parsing response: VRF ID %u",
					ntohl(routing_data->vrf_id));

			break;
		case NF_DATA_TYPE_HOST_ASN:
			if (BOOST_UNLIKELY(data_length < sizeof(uint32_t))) {
				break;
			}
			routing_data->host_asn = *(uint32_t *)data;

			DPRINTF_DEBUG("Routing Plane API: parsing response: host ASN %u",
					ntohl(routing_data->host_asn));

			break;
		case NF_DATA_TYPE_BGP_ADJACENT_ASN:
			if (BOOST_UNLIKELY(data_length < sizeof(uint32_t))) {
				break;
			}
			routing_data->adj_asn = *(uint32_t *)data;

			DPRINTF_DEBUG("Routing Plane API: parsing response: BGP adjacent "
					"ASN %u", ntohl(routing_data->adj_asn));

			break;
		case NF_DATA_TYPE_SUBNET_MASK:
			routing_data->prefix_length = *data;

			DPRINTF_DEBUG("Routing Plane API: parsing response: prefix length "
					"%u", routing_data->prefix_length);

			break;
		case NF_DATA_TYPE_IP_NHOP_ADDRESSES:
			if (BOOST_UNLIKELY(data_length < ip_length)) {
				break;
			}
			if (BOOST_UNLIKELY(!parse_ip_nexthop(data_length, ip_length,
					routing_data, data))) {
				goto error;
			}
			break;
		case NF_DATA_TYPE_BGP_NHOP_ADDRESSES:
			if (BOOST_UNLIKELY(data_length < ip_length)) {
				break;
			}
			if (BOOST_UNLIKELY(!parse_bgp_nexthop(data_length, ip_length,
					routing_data, data))) {
				goto error;
			}
			break;
		case NF_DATA_TYPE_REQUIRED_INFO:
			msg(LOG_NOTICE, "Routing Plane API: non-fatal error parsing frame "
					"from Routing Plane: received NF_DATA_TYPE_REQUIRED_INFO "
					"frame");
			break;
		default:
			msg(LOG_NOTICE, "Routing Plane API: non-fatal error parsing frame "
					"from Routing Plane: received unknown %u frame", data_type);
			break;
		}
	}

	/*
	 * A frame might have a lot of data, but no address, in which case we must
	 * discard the frame.
	 */
	if (BOOST_UNLIKELY(!parsed_ip)) {
		ret = ROUTING_PLANE_ERROR_REPLY_NO_IP;
		goto error;
	}

	return ret;

error:
RoutingPlane::routing_plane_cleanup_data_t(routing_data, false);
	memset(routing_data, 0, sizeof(routing_plane_data_t));

	return ret;
}

/**
 * Parses frame from Routing Plane message. This function will try hard to fill
 * data in routing_data only if it appears to be correct
 *
 * @param[in,out]  routing_data  pointer, content will be overwritten
 *
 * @param[in]      reply         byte array, message from Routing Plane
 *
 * @param[in]      reply_size    size of reply from Routing Plane
 *
 * @param[in,out]  msg_type      ptr to uint, will be filled with message type:
 *          NF_ROUTE_REQUEST, NF_ROUTE_RESPONSENF_ROUTE_WITHDRAW, undef on error
 *
 * @return         int, total bytes parsed or negative on error
 */
static int
parse_frame (routing_plane_data_t *routing_data, uint8_t *reply,
		size_t reply_size, unsigned int *msg_type)
{
	size_t payload_length, ip_length;
	int ret;
	sa_family_t family;

	DPRINTF_DEBUG("Routing Plane API: parsing response: ZMQ frame size %zu "
			"bytes", reply_size);

	/*
	 * If we don't get a least a header, something went very wrong.
	 */
	if (BOOST_UNLIKELY(!reply || reply_size < NF_MSG_HEADER_LENGTH)) {
		return ROUTING_PLANE_ERROR_EMPTY_REPLY;
	}

	/*
	 * Accept every valid type, leave up to the caller to make sure it's what it
	 * is expecting.
	 */
	*msg_type = NF_MSG_GET_HEADER_TYPE(reply);
	if (BOOST_UNLIKELY(*msg_type != NF_ROUTE_REQUEST &&
			*msg_type != NF_ROUTE_RESPONSE && *msg_type != NF_ROUTE_WITHDRAW)) {
		return ROUTING_PLANE_ERROR_INVALID_REP_TYPE;
	}

	family = NF_MSG_GET_HEADER_AF(reply);
	DPRINTF_DEBUG("Routing Plane API: parsing response: AF family %u", family);
	if (family != AF_INET && family != AF_INET6) {
		return ROUTING_PLANE_ERROR_UNKNOWN_AF;
	}
	ip_length = family == AF_INET ?
			sizeof(struct in_addr) : sizeof(struct in6_addr);

	/*
	 * The length field of the header does NOT include the length of the header
	 * We need at the very least one IP address in a response to proceed.
	 */
	payload_length = NF_MSG_GET_HEADER_LENGTH(reply);
	DPRINTF_DEBUG("Routing Plane API: parsing response: payload length %zu",
			payload_length);
	if (BOOST_UNLIKELY(payload_length < NF_DATA_HEADER_LENGTH + ip_length)) {
		return ROUTING_PLANE_ERROR_REPLY_NO_IP;
	}

	/*
	 * The message header might lie if something goes wrong, so make sure that
	 * the frame was at least as big as the header claims it to be.
	 */
	if (BOOST_UNLIKELY(reply_size < payload_length + NF_MSG_HEADER_LENGTH)) {
		return ROUTING_PLANE_ERROR_INVALID_REP_SIZE;
	}

	/*
	 * We start inspecting the array at the next byte after the header, where
	 * we expect to find a data header.
	 */
	reply += NF_MSG_HEADER_LENGTH;

	/*
	 * We can extract AF_FAMILY from the header. If we got this far it means
	 * that there will be (some sort of) data in the payload, so we start to
	 * fill data into routing_data.
	 */
	routing_data->family = family;

	/*
	 * parse_data will return the total bytes parsed. The only time it
	 * returns an error is if either a malloc failed, or there was a data
	 * header without payload, which means Routing Plane told us it would send
	 * something but it didn't, or the payload was shorter than the header said
	 * it would be, or there was no IP or multiple IPs.
	 * In any cases, return an error so that it is logged.
	 * Also return -1 if the reply had more data than we parsed (which means
	 * the header lied and the frame was longer than expected).
	 */
	ret = parse_data(routing_data, reply, payload_length);
	if (BOOST_LIKELY(ret >= 0)) {
		if (BOOST_UNLIKELY((size_t)ret != payload_length ||
				(size_t)ret + NF_MSG_HEADER_LENGTH < reply_size)) {
			ret = ROUTING_PLANE_ERROR_INVALID_REP_SIZE;
		} else {
			/*
			 * Add the header length to return the total number of bytes parsed
			 * from the frame.
			 */
			ret += NF_MSG_HEADER_LENGTH;
		}
	}

	set_subnet_from_ip_prefix(routing_data);

	return ret;
}

/**
 * Allocates a message for Routing Plane based on data contained in routing_data
 *
 * @param[in]      request_type  16 bits mask of NF_DATA_TYPE_*
 *
 * @param[in]      routing_data  pointer to routing_plane_data_t
 *
 * @param[in,out]  msg           pointer to pointer to buffer, will be allocated
 *
 * @param[in,out]  msg_size      pointer to size of buffer, will be filled
 *
 * @return         int, 0 on success or negative on error.
 */
static int
create_request (uint16_t request_type, routing_plane_data_t *routing_data,
		uint8_t buffer[], size_t *buffer_size)
{
	char debug_buffer[40];
	size_t offset = 0;
	uint16_t ip_length;

	if (BOOST_UNLIKELY(!routing_data || !buffer || !buffer_size)) {
		return ROUTING_PLANE_ERROR_INPUT;
	}

	/*
	 * Figure out how large a buffer we need. PBR ID and VRF name are optional,
	 * so the buffer has to be allocated at runtime and cannot be on the stack.
	 */
	if (BOOST_LIKELY(routing_data->family == AF_INET)) {
		*buffer_size = NF_REQUEST_SIZE_V4;
	} else if (BOOST_LIKELY(routing_data->family == AF_INET6)) {
		*buffer_size = NF_REQUEST_SIZE_V6;
	} else {
		return ROUTING_PLANE_ERROR_UNKNOWN_AF;
	}
	DPRINTF_DEBUG("Routing Plane API: parsing request: AF family %u",
			routing_data->family);

	if (routing_data->pbr_table_id) {
		*buffer_size += NF_DATA_HEADER_LENGTH + NF_PBR_TABLE_ID_LENGTH;
	}

	if (routing_data->vrf_name_length) {
		*buffer_size += NF_DATA_HEADER_LENGTH + routing_data->vrf_name_length;
	}

	if (routing_data->vrf_id) {
		*buffer_size += NF_DATA_HEADER_LENGTH + NF_VRF_ID_LENGTH;
	}

	DPRINTF_DEBUG("Routing Plane API: parsing request: payload length %zu",
			*buffer_size - NF_MSG_HEADER_LENGTH);

	/*
	 * Set the header values. As per the protocol, the header comes first in
	 * each frame, with type, AF_FAMILY and payload size.
	 */
	NF_MSG_SET_HEADER_TYPE(buffer + offset, NF_ROUTE_REQUEST);
	NF_MSG_SET_HEADER_AF(buffer + offset, routing_data->family);
	NF_MSG_SET_HEADER_LENGTH(buffer + offset,
			*buffer_size - NF_MSG_HEADER_LENGTH);

	/*
	 * Bump the pointer to the first data header and as per protocol definition
	 * add length of data body and type, which for the request type is first of
	 * all the definition of what we are asking for.
	 */
	offset += NF_MSG_HEADER_LENGTH;
	NF_MSG_SET_DATA_TYPE(buffer + offset, NF_DATA_TYPE_REQUIRED_INFO);
	NF_MSG_SET_DATA_LENGTH(buffer + offset, NF_REQ_DATA_LENGTH);

	/*
	 * Info is the request type, which is a 16 bits mask, bitwise OR of
	 * possible values. See RoutingPlane.hpp for details about the values and
	 * various #defines. It will tell Routing Plane what we want to know about
	 * the IP address we include in the next section of the frame.
	 */
	offset += NF_DATA_HEADER_LENGTH;
	NF_MSG_SET_REQ_INFO(buffer + offset, request_type);


	offset += NF_REQ_DATA_LENGTH;
	ip_length = routing_data->family == AF_INET ?
			sizeof(struct in_addr) : sizeof(struct in6_addr);
	NF_MSG_SET_DATA_TYPE(buffer + offset, NF_DATA_TYPE_IP_ADDRESS);
	NF_MSG_SET_DATA_LENGTH(buffer + offset, ip_length);

	offset += NF_DATA_HEADER_LENGTH;
	memcpy(buffer + offset, &routing_data->ip_addresses[0], ip_length);
	offset += ip_length;

	inet_ntop(routing_data->family, &routing_data->ip_addresses[0],
			debug_buffer, 40);
	DPRINTF_DEBUG("Routing Plane API: parsing request: IP %s", debug_buffer);


	if (routing_data->pbr_table_id) {
		NF_MSG_SET_DATA_TYPE(buffer + offset, NF_DATA_TYPE_PBR_TABLE_ID);
		NF_MSG_SET_DATA_LENGTH(buffer + offset, NF_PBR_TABLE_ID_LENGTH);
		offset += NF_DATA_HEADER_LENGTH;
		memcpy(buffer + offset, &routing_data->pbr_table_id,
				NF_PBR_TABLE_ID_LENGTH);
		DPRINTF_DEBUG("Routing Plane API: parsing request: PBR ID %u",
				routing_data->pbr_table_id);
		offset += NF_PBR_TABLE_ID_LENGTH;
	}

	if (routing_data->vrf_name_length) {
		NF_MSG_SET_DATA_TYPE(buffer + offset, NF_DATA_TYPE_VRF_NAME);
		NF_MSG_SET_DATA_LENGTH(buffer + offset,
				routing_data->vrf_name_length);
		offset += NF_DATA_HEADER_LENGTH;
		memcpy(buffer + offset, routing_data->vrf_name,
				routing_data->vrf_name_length);
		DPRINTF_DEBUG("Routing Plane API: parsing request: VRF %s",
				routing_data->vrf_name);
		offset += routing_data->vrf_name_length;
	}

	if (routing_data->vrf_id) {
		NF_MSG_SET_DATA_TYPE(buffer + offset, NF_DATA_TYPE_VRF_ID);
		NF_MSG_SET_DATA_LENGTH(buffer + offset, NF_VRF_ID_LENGTH);
		offset += NF_DATA_HEADER_LENGTH;
		memcpy(buffer + offset, &routing_data->vrf_id,
				NF_VRF_ID_LENGTH);
		DPRINTF_DEBUG("Routing Plane API: parsing request: VRF ID %u",
				ntohl(routing_data->vrf_id));
		offset += NF_VRF_ID_LENGTH;
	}

	return 0;
}

int
RoutingPlane::get_data (routing_plane_data_t *routing_data[],
		size_t req_count, uint16_t flags)
{
	uint8_t request_buffer[NF_REQUEST_BUFFER_MAX_SIZE];
	zmsg_t *msg_request = NULL, *msg_reply = NULL;
	zframe_t *frame_reply;
	size_t i, num_frames, good_frames = 0, request_size = 0;
	int ret = 0;
	std::list<size_t>indexes;

	errno = 0;

	if (!req_count) {
		return 0;
	}

	if (!routing_data || req_count == 0) {
		ret = ROUTING_PLANE_ERROR_INVALID_ARGUMENT;
		goto error;
	}

	if (!flags) {
		flags = NF_DATA_TYPE_ALL_FIELDS;
	} else if ((flags | NF_DATA_TYPE_ALL_FIELDS) != NF_DATA_TYPE_ALL_FIELDS) {
		ret = ROUTING_PLANE_ERROR_INVALID_FLAGS;
		goto error;
	}

	msg_request = zmsg_new();
	if (!msg_request) {
		ret = ROUTING_PLANE_ERROR_ZMQ_API;
		goto error;
	}

	DPRINTF_INFO("Routing Plane API: processing %zu requests for data, with "
			"flags %u", req_count, flags);

	/*
	 * Add empty frame. We are using ZMQ_DEALER, which unlike ZMQ_REQ does not
	 * add it for us.
	 */
	frame_reply = zframe_new_empty();
	if (!frame_reply) {
		ret = ROUTING_PLANE_ERROR_ZMQ_API;
		goto error;
	}
	ret = zmsg_append(msg_request, &frame_reply);
	if (ret < 0) {
		ret = ROUTING_PLANE_ERROR_ZMQ_API;
		goto error;
	}

	/*
	 * Parse the input and create a frame for each routing_plane_data_t
	 * structure (a frame per address), as per protocol definition.
	 * Since some items might be cached, we save the indexes of those we have
	 * to change in a list.
	 */
	for (i = 0; i < req_count; ++i) {
		DPRINTF_DEBUG("Routing Plane API: processing request %zu", i);

		cache_lock.lock();
		if (cache_ip.count(pair<routing_plane_data_t *,
				uint16_t>(routing_data[i], 0))) {
			routing_plane_data_t_deep_copy(routing_data[i],
					(routing_plane_data_t *)cache_ip[pair<routing_plane_data_t *,
													 uint16_t>(routing_data[i],
															 0)], false);
			cache_lock.unlock();
			continue;
		}
		cache_lock.unlock();

		indexes.push_back(i);

		ret = create_request(flags, routing_data[i], request_buffer,
				&request_size);
		if (BOOST_UNLIKELY(ret < 0)) {
			goto error;
		}

		ret = zmsg_addmem(msg_request, request_buffer, request_size);
		if (BOOST_UNLIKELY(ret < 0)) {
			ret = ROUTING_PLANE_ERROR_ZMQ_API;
			goto error;
		}
	}

	if (indexes.empty()) {
		DPRINTF_INFO("Routing Plane API: 100%% cache hit, no message sent");
		ret = 0;
		goto error;
	}

	DPRINTF_INFO("Routing Plane API: sending %zu frames in one message to "
			"Routing Plane", indexes.size());

	ret = zmsg_send(&msg_request, zmq_dealer_socket);
	if (ret < 0) {
		ret = ROUTING_PLANE_ERROR_ZMQ_API;
		goto error;
	}

	DPRINTF_INFO("Routing Plane API: blocking on zmsg_recv waiting for "
			"response from Routing Plane");

	/*
	 * Note, this is a blocking call.
	 */
	msg_reply = zmsg_recv(zmq_dealer_socket);
	if (!msg_reply) {
		/*
		 * Note that even if we could check if recv was interrupted by a signal
		 * and retry, we don't. This call will likely be blocked for a long
		 * while, and if we get a signal it means that we either have to close
		 * the socket to change the recipient, or that the process is either
		 * restarting or terminating. In any case there's no point in
		 * stalling and trying to receive again.
		 */
		ret = ROUTING_PLANE_ERROR_ZMQ_API;
		goto error;
	}

	/*
	 * We expect at least a frame, even when there is no data available.
	 * If there are zero frames, something is wrong with the channel and we
	 * should report it to the caller.
	 */
	num_frames = zmsg_size(msg_reply);
	if (!num_frames) {
		ret = ROUTING_PLANE_ERROR_EMPTY_REPLY;
		goto error;
	}

	DPRINTF_INFO("Routing Plane API: received response from Routing Plane, "
			"one msg with %zu frames",
			num_frames);

	DPRINTF_DEBUG("Routing Plane API: resetting input routing_data before "
			"parsing response");

	/*
	 * Pop the first empty frame. We are using ZMQ_DEALER, which unlike ZMQ_REQ
	 * does not remove it for us.
	 */
	frame_reply = zmsg_pop(msg_reply);
	zframe_destroy(&frame_reply);
	--num_frames;

	/*
	 * Parse the reply and fill in the routing_plane_data_t structure(s) before
	 * returning to the caller. As per protocol definition, we expect one frame
	 * per address request.
	 */
	i = 0; // refuses to compile if this i=0 is added to the for() below.
	for (std::list<size_t>::iterator it = indexes.begin();
			it != indexes.end() && i < num_frames; ++it, ++i) {
		DPRINTF_DEBUG("Routing Plane API: processing request %zu", i);

		frame_reply = zmsg_pop(msg_reply);
		if (BOOST_UNLIKELY(!frame_reply)) {
			/*
			 * This should not happen, since we extracted the count. But it
			 * does not hurt to check. In case a frame is empty, try and go
			 * further, as we are limited by num_frames and req_count anyway.
			 */
			msg(LOG_NOTICE, "Routing Plane API: non-fatal error parsing frame "
					"from Routing Plane: received empty frame");
			continue;
		}

		unsigned int msg_type;
		ret = parse_frame(routing_data[*it], zframe_data(frame_reply),
				zframe_size(frame_reply), &msg_type);
		if (BOOST_UNLIKELY(ret < 0 || msg_type != NF_ROUTE_RESPONSE)) {
			/*
			 * If there are problems in a frame, others might be valid, so we
			 * do not exit immediately. Report and error and try to continue.
			 */
			msg(LOG_NOTICE, "Routing Plane API: non-fatal error parsing frame "
					"from Routing Plane: received malformed frame");
		} else {
			/*
			 * Keep a count of how many good frames the reply had, so that if
			 * there were none we can report an error.
			 */
			++good_frames;
			DPRINTF_DEBUG("Routing Plane API: parsed good frame from "
					"Routing Plane, %d bytes read", ret);

			routing_plane_data_t *new_node = (routing_plane_data_t *)calloc(
					1, sizeof(routing_plane_data_t));
			if (!new_node) {
				THROWEXCEPTION("Could not allocate memory for Routing Plane"
						"cache item!");
			}
			routing_plane_data_t_deep_copy(new_node, routing_data[*it], true);

			cache_lock.lock();
			if (BOOST_LIKELY(!cache_ip.count(pair<routing_plane_data_t *,
					uint16_t>(routing_data[*it], 0)))) {
				if (cache_subnet.count(routing_data[*it])) {
					routing_plane_data_t *tmp =
							(routing_plane_data_t *)cache_subnet[routing_data[*it]];
					tmp->ip_count++;
					tmp->ip_addresses = (ip46 *)realloc(tmp->ip_addresses,
							sizeof(ip46) * tmp->ip_count);
					if (!tmp->ip_addresses) {
						THROWEXCEPTION("Could not allocate memory for Routing "
								"Plane cache item!");
					}
					memcpy(&tmp->ip_addresses[tmp->ip_count - 1],
							&routing_data[*it]->ip_addresses[0], sizeof(ip46));
					cache_ip[pair<routing_plane_data_t *,
							 uint16_t>(tmp, tmp->ip_count - 1)] = tmp;

					cache_lock.unlock();
					routing_plane_cleanup_data_t(new_node, true);
					free(new_node);
				} else {
					cache_ip[pair<routing_plane_data_t *,
							 uint16_t>(new_node, 0)] = new_node;
					cache_subnet[new_node] = new_node;
					cache_lock.unlock();
				}
				DPRINTF_INFO("Routing Plane API: added new item to cache");
			} else {
				cache_lock.unlock();
				routing_plane_cleanup_data_t(new_node, true);
				free(new_node);
			}
		}
		zframe_destroy(&frame_reply);
	}

	if (!good_frames) {
		ret = ROUTING_PLANE_ERROR_EMPTY_REPLY;
		goto error;
	}

	DPRINTF_DEBUG("Routing Plane API: processed %zu good frames from "
			"Routing Plane, returning", good_frames);

	ret = 0;

error:
	if (msg_request) {
		zmsg_destroy(&msg_request);
	}
	if (msg_reply) {
		zmsg_destroy(&msg_reply);
	}
	if (ret < 0) {
		if (errno) {
			msg(LOG_CRIT, "Routing Plane API: fatal system error: %s",
					strerror(errno));
		}
		msg(LOG_CRIT, "Routing Plane API: fatal error contacting Routing Plane"
				" Netflow APIs module error: %s", routing_plane_errors[-ret]);
	}

	return ret;
}

int
RoutingPlane::parse_update_reply (zmsg_t *msg_reply, size_t num_frames)
{
	zframe_t *frame_reply;
	routing_plane_data_t *routing_data;
	size_t i;
	int ret = 0, good_frames = 0;

	/*
	 * Parse the reply and update the cache. As per protocol definition, we
	 * expect one frame per address update.
	 */
	for (i = 0; i < num_frames; ++i) {
		msg(LOG_DEBUG, "Routing Plane API: processing update %zu", i);

		frame_reply = zmsg_pop(msg_reply);
		if (BOOST_UNLIKELY(!frame_reply)) {
			/*
			 * This should not happen, since we extracted the count. But it
			 * does not hurt to check. In case a frame is empty, try and go
			 * further, as we are limited by num_frames and req_count anyway.
			 */
			msg(LOG_NOTICE, "Routing Plane API: non-fatal error parsing frame "
					"from Routing Plane: received empty frame");
			continue;
		}

		routing_data = (routing_plane_data_t *)calloc(1,
				sizeof(routing_plane_data_t));
		if (!routing_data) {
			msg(LOG_CRIT, "Routing Plane API: fatal calloc error: %s",
					strerror(errno));
			continue;
		}

		unsigned int msg_type;
		ret = parse_frame(routing_data, zframe_data(frame_reply),
				zframe_size(frame_reply), &msg_type);
		if (BOOST_UNLIKELY(ret < 0 || (msg_type != NF_ROUTE_RESPONSE &&
				msg_type != NF_ROUTE_WITHDRAW))) {
			/*
			 * If there are problems in a frame, others might be valid, so we
			 * do not exit immediately. Report and error and try to continue.
			 */
			RoutingPlane::routing_plane_cleanup_data_t(routing_data, true);
			free(routing_data);
			msg(LOG_NOTICE, "Routing Plane API: non-fatal error parsing frame "
					"from Routing Plane: received malformed update frame");
		} else {
			msg(LOG_DEBUG, "Routing Plane API: parsed good update frame from "
					"Routing Plane, %d bytes, updating Routing Plane cache",
					ret);

			if (msg_type == NF_ROUTE_RESPONSE) {
				RoutingPlane::cache_lock.lock();
				if (RoutingPlane::cache_subnet.count(routing_data)) {
					routing_plane_data_t *tmp =
							(routing_plane_data_t *)
							RoutingPlane::cache_subnet[routing_data];
					RoutingPlane::routing_plane_cleanup_data_t(tmp, false);
					routing_plane_data_t_deep_copy(tmp, routing_data, false);

					RoutingPlane::cache_lock.unlock();
					RoutingPlane::routing_plane_cleanup_data_t(routing_data,
							true);
					free(routing_data);
					msg(LOG_DEBUG, "Routing Plane API: updated subnet in "
							"cache");
				} else {
					RoutingPlane::cache_lock.unlock();
					RoutingPlane::routing_plane_cleanup_data_t(routing_data,
							true);
					free(routing_data);
					msg(LOG_DEBUG, "Routing Plane API: Routing Plane cache "
							"replace skipped, subnet not found");
				}
			} else if (msg_type == NF_ROUTE_WITHDRAW) {
				size_t rc = 0;

				RoutingPlane::cache_lock.lock();
				if (RoutingPlane::cache_subnet.count(routing_data)) {
					routing_plane_data_t *tmp_route =
							(routing_plane_data_t *)
							RoutingPlane::cache_subnet[routing_data];
					for (uint16_t i = 0; i < tmp_route->ip_count; ++i) {
						RoutingPlane::cache_ip.erase(pair<routing_plane_data_t *,
								uint16_t>(tmp_route, i));
						msg(LOG_INFO, "Routing Plane API: removed IP from "
								"cache");
					}
					rc = RoutingPlane::cache_subnet.erase(routing_data);

					RoutingPlane::routing_plane_cleanup_data_t(tmp_route, true);
					free(tmp_route);
				}
				RoutingPlane::cache_lock.unlock();

				if (rc) {
					msg(LOG_DEBUG, "Routing Plane API: removed subnet from "
							"cache");
				} else {
					msg(LOG_DEBUG, "Routing Plane API: Routing Plane cache "
							"withdrawal skipped, subnet not found");
				}

				RoutingPlane::routing_plane_cleanup_data_t(routing_data, true);
				free(routing_data);
			}

			++good_frames;
		}
		zframe_destroy(&frame_reply);
	}

	return good_frames;
}

int
RoutingPlane::routing_plane_process_update (zsock_t *sock)
{
	zmsg_t *msg_reply = NULL;
	size_t num_frames;
	int ret = 0;

	msg(LOG_DEBUG, "Routing Plane API: reading message from Routing Plane "
			"PUB-SUB ZMQ socket");

	msg_reply = zmsg_recv(sock);
	if (!msg_reply) {
		ret = ROUTING_PLANE_ERROR_ZMQ_API;
		goto error;
	}

	/*
	 * We expect at least a frame, since the message was published on PUB socket
	 */
	num_frames = zmsg_size(msg_reply);
	if (!num_frames) {
		ret = ROUTING_PLANE_ERROR_EMPTY_REPLY;
		goto error;
	}

	msg(LOG_DEBUG, "Routing Plane API: received update from Routing Plane, one "
			"msg with %zu frames", num_frames);

	ret = parse_update_reply(msg_reply, num_frames);
	if (ret <= 0) {
		ret = ROUTING_PLANE_ERROR_EMPTY_REPLY;
		goto error;
	}

	msg(LOG_DEBUG, "Routing Plane API: processed all frames from Routing Plane "
			"update, returning");

error:
	if (msg_reply) {
		zmsg_destroy(&msg_reply);
	}
	if (ret < 0) {
		if (errno) {
			msg(LOG_CRIT, "Routing Plane API: fatal system error: %s",
					strerror(errno));
		}
		msg(LOG_CRIT, "Routing Plane API: fatal error receiving update, "
				"Routing Plane Netflow APIs  module error: %s",
				routing_plane_errors[-ret]);
	}

	return ret;
}

/**
 * ZMQ specific listener function. This function is called by @c listenerThread
 */
void
RoutingPlane::run()
{
	timeval last_cleared, now;
	gettimeofday(&last_cleared, 0);

	while (!zsys_interrupted && !exitFlag) {
		void *sock = zpoller_wait(zpoller, zmq_poll_timeout);
		if (!sock) {
			if (zpoller_terminated(zpoller)) {
				msg(LOG_INFO, "Routing Plane: ZMQ termination signal received");
				break;
			} else {
				if (cache_wiping_interval > 0) {
					gettimeofday(&now, 0);
					if (last_cleared.tv_sec + cache_wiping_interval <
							now.tv_sec) {
						clear_cache();
						gettimeofday(&last_cleared, 0);
					}
				}
				continue;
			}
		}

		int rc = routing_plane_process_update((zsock_t *)sock);
		if (rc < 0) {
			msg(LOG_ERR, "Routing Plane: Empty ZMQ SUB message");
		}

		if (cache_wiping_interval > 0) {
			gettimeofday(&now, 0);
			if (last_cleared.tv_sec + cache_wiping_interval < now.tv_sec) {
				clear_cache();
				gettimeofday(&last_cleared, 0);
			}
		}
	}
}

/**
 * Thread function responsible for receiving packets from the Routing Plane SUB
 * @param routingPlane_ pointer to a RoutingPlane instance
 * @return NULL
 */
void *
RoutingPlane::threadWrapper(void *routingPlane_) {
	RoutingPlane *routingPlane = static_cast<RoutingPlane *>(routingPlane_);

	routingPlane->run();

	return NULL;
}

#endif // ZMQ_SUPPORT_ENABLED
