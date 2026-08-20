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

#ifndef ROUTING_PLANE_H
#define ROUTING_PLANE_H

#include <stdexcept>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <unordered_map>
#include <boost/functional/hash.hpp>
#include <utility>

#include "common/Thread.h"
#include "common/Mutex.h"
#include "common/ipfixlolib/encoding.h"

using namespace std;

/*
 * Message Definitions for Routing Plane protocol
 * Message Header
 * 1                           16                               32
 * +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 * |      Message Type         |             Family              |
 * ---------------------------------------------------------------
 * |                         Length                              |
 * ---------------------------------------------------------------
 * |                                                             |
 * |                                                             |
 * |                          Data                               |
 * |                                                             |
 * |                                                             |
 * +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 *
 * Message Types
 * #define NF_ROUTE_REQUEST                 (1 << 0)
 * #define NF_ROUTE_RESPONSE                (1 << 1)
 *
 * Family
 * AF_INET
 * AF_INET6
 *
 * NF_ROUTE_REQUEST Message
 * 1                            16                              32
 * +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 * |           Data Type         |           Data Length         |
 * ---------------------------------------------------------------
 * |                                                             |
 * |                            Data                             |
 * |                                                             |
 * +++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 *
 *
 * NF_ROUTE_RESPONSE Message
 * 1                          16                                 32
 * ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 * |      Data Type            |     Data Length                  |
 * ----------------------------------------------------------------
 * |                                                              |
 * |                          Data                                |
 * |                                                              |
 * ++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++++
 *
 * Example:
 * Request:
 * NF_ROUTE_REQUEST, AF_INET, 14,
 * NF_DATA_TYPE_REQUIRED_INFO, 2, (NF_DATA_TYPE_HOST_ASN |
 *     NF_DATA_TYPE_IP_NHOP_ADDRESSES)
 * NF_DATA_TYPE_IP_ADDRESS, 4, 1.1.1.1
 * NF_ROUTE_REQUEST, AF_INET, 14,
 * NF_DATA_TYPE_REQUIRED_INFO, 2, (NF_DATA_TYPE_HOST_ASN |
 *     NF_DATA_TYPE_IP_NHOP_ADDRESSES)
 * NF_DATA_TYPE_IP_ADDRESS, 4, 2.2.2.2
 *
 * Response:
 * NF_ROUTE_RESPONSE, AF_INET, 24,
 * NF_DATA_TYPE_IP_ADDRESS, 4, 1.1.1.1, NF_DATA_TYPE_HOST_ASN, 4, 10,
 *     NF_DATA_TYPE_IP_NHOP_ADDRESSES, 4, 10.10.10.10
 * NF_ROUTE_RESPONSE, AF_INET, 24,
 * NF_DATA_TYPE_IP_ADDRESS, 4, 2.2.2.2, NF_DATA_TYPE_HOST_ASN, 4, 20,
 *     NF_DATA_TYPE_IP_NHOP_ADDRESSES, 8, 10.10.10.10, 20.20.20.20
 */

/*
 * Message Types
 */
#define NF_ROUTE_REQUEST                 1
#define NF_ROUTE_RESPONSE                2
#define NF_ROUTE_WITHDRAW                3

/*
 * Length fields:
 *  - The Length for the nexthop addresses will be n * length of
 *     (IPv4 / IPv6 address), where the type of family is determined
 *     from the Message Header.
 *  - VRF Name is a string, NOT NULL-terminated
 *  - PBR Table is of 1 byte length
 */
#define NF_MSG_HEADER_LENGTH            8
#define NF_DATA_HEADER_LENGTH           4
#define NF_REQ_DATA_LENGTH              2
#define NF_PBR_TABLE_ID_LENGTH          1
#define NF_VRF_ID_LENGTH                4
#define NF_TLV_VRF_NAME_LEN_MAX         255

/*
 * As per definition in the protocol, a message payload to RIBd
 * contains: data header, 4 bytes, and an ip address, 4/16 bytes, plus the
 * request type flag, 2 bytes, and its header, 4 bytes, plus message header, 8
 * bytes. The optional VRF and PBR id are not included in this macro.
 */
#define NF_REQUEST_SIZE_V4          (NF_MSG_HEADER_LENGTH + 2 * \
	NF_DATA_HEADER_LENGTH + NF_REQ_DATA_LENGTH + sizeof(struct in_addr))
#define NF_REQUEST_SIZE_V6          (NF_MSG_HEADER_LENGTH + 2 * \
	NF_DATA_HEADER_LENGTH + NF_REQ_DATA_LENGTH + sizeof(struct in6_addr))

/*
 * The max possible size of a request buffer, used to be able to stack-allocate
 * the request buffer and re-use it, in order to be sure to have enough space
 * for any type of query.
 */
#define NF_REQUEST_BUFFER_MAX_SIZE      (NF_REQUEST_SIZE_V6 + \
	NF_DATA_HEADER_LENGTH + NF_PBR_TABLE_ID_LENGTH + \
	NF_DATA_HEADER_LENGTH + NF_TLV_VRF_NAME_LEN_MAX + \
	NF_VRF_ID_LENGTH)


/*
 * Macros to get and set message/data header fields. Will take care of swapping
 * bytes to/from network order.
 */
#define NF_MSG_HEADER_AF_OFFSET         2
#define NF_MSG_HEADER_LENGTH_OFFSET     4
#define NF_MSG_DATA_LENGTH_OFFSET       2

#define NF_MSG_GET_HEADER_TYPE(p) (ntohs(*(uint16_t *)((uint8_t *)(p))))
#define NF_MSG_GET_HEADER_AF(p) \
	(ntohs(*(sa_family_t *)((uint8_t *)(p) + NF_MSG_HEADER_AF_OFFSET)))
#define NF_MSG_GET_HEADER_LENGTH(p) \
	(ntohl(*(uint32_t *)((uint8_t *)(p) + NF_MSG_HEADER_LENGTH_OFFSET)))
#define NF_MSG_GET_DATA_TYPE(p) (ntohs(*(uint16_t *)((uint8_t *)(p))))
#define NF_MSG_GET_DATA_LENGTH(p) \
	(ntohs(*(uint16_t *)((uint8_t *)(p) + NF_MSG_DATA_LENGTH_OFFSET)))

#define NF_MSG_SET_HEADER_TYPE(p,val) \
	((*(uint16_t *)((uint8_t *)(p))) = htons(val))
#define NF_MSG_SET_HEADER_AF(p,val) \
	((*(sa_family_t *)((uint8_t *)(p) + NF_MSG_HEADER_AF_OFFSET)) = htons(val))
#define NF_MSG_SET_HEADER_LENGTH(p,val) \
	((*(uint32_t *)((uint8_t *)(p) + NF_MSG_HEADER_LENGTH_OFFSET)) = htonl(val))
#define NF_MSG_SET_DATA_TYPE(p,val) \
	((*(uint16_t *)((uint8_t *)(p))) = htons(val))
#define NF_MSG_SET_DATA_LENGTH(p,val) \
	((*(uint16_t *)((uint8_t *)(p) + NF_MSG_DATA_LENGTH_OFFSET)) = htons(val))
#define NF_MSG_SET_REQ_INFO(p,val) \
	((*(uint16_t *)((uint8_t *)(p))) = htons(val))

/*
 * Data types defines. get_data takes an OR'ed mask. An ALL_FIELDS define
 * is provided for convenience.
 */
#define NF_DATA_TYPE_IP_ADDRESS               (1 << 0)
#define NF_DATA_TYPE_PBR_TABLE_ID             (1 << 1)
#define NF_DATA_TYPE_VRF_NAME                 (1 << 2)
#define NF_DATA_TYPE_REQUIRED_INFO            (1 << 3)
#define NF_DATA_TYPE_HOST_ASN                 (1 << 4)
#define NF_DATA_TYPE_SUBNET_MASK              (1 << 5)
#define NF_DATA_TYPE_IP_NHOP_ADDRESSES        (1 << 6)
#define NF_DATA_TYPE_BGP_NHOP_ADDRESSES       (1 << 7)
#define NF_DATA_TYPE_BGP_ADJACENT_ASN         (1 << 8)
#define NF_DATA_TYPE_VRF_ID                   (1 << 9)
#define NF_DATA_TYPE_ALL_FIELDS (NF_DATA_TYPE_HOST_ASN | \
	NF_DATA_TYPE_SUBNET_MASK | NF_DATA_TYPE_IP_NHOP_ADDRESSES | \
	NF_DATA_TYPE_BGP_NHOP_ADDRESSES | NF_DATA_TYPE_BGP_ADJACENT_ASN)

/*
 * Error codes returned by get_data (note: they will be negated).
 */
#define ROUTING_PLANE_SUCCESS                  0
#define ROUTING_PLANE_ERROR_GENERAL           -1
#define ROUTING_PLANE_ERROR_ZMQ_API           -2
#define ROUTING_PLANE_ERROR_INPUT             -3
#define ROUTING_PLANE_INFO_TIMEOUT            -4
#define ROUTING_PLANE_INFO_INTERRUPTED        -5
#define ROUTING_PLANE_MISMATCHING_AF          -6
#define ROUTING_PLANE_ERROR_EMPTY_REPLY       -7
#define ROUTING_PLANE_ERROR_UNKNOWN_AF        -8
#define ROUTING_PLANE_ERROR_INVALID_ARGUMENT  -9
#define ROUTING_PLANE_ERROR_INVALID_REP_TYPE  -10
#define ROUTING_PLANE_ERROR_INVALID_REP_SIZE  -11
#define ROUTING_PLANE_ERROR_REPLY_NO_IP       -12
#define ROUTING_PLANE_ERROR_REPLY_MULTIPLE_IP -13
#define ROUTING_PLANE_ERROR_INVALID_FLAGS     -14

// default poll timeout, 1 second
#define RP_ZMQ_POLL_TIMEOUT_DEFAULT 1000
// default cache cleaning interval, default disabled
#define RP_CACHE_CLEAN_INTERVAL_DEFAULT 0

/*
 * 16 bytes union, useful since a request can be either AF_INET or AF_INET6.
 */
typedef union {
	struct in_addr addr;
	struct in6_addr addr6;
#if defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
	unsigned __int128 v128;
#pragma GCC diagnostic pop
#endif
} ip46;

/*
 * family and ip_address are the parameters used to ask for data, so they have
 * to be filled by the caller. RIBd as part of the response returns the input
 * too, the API will overwrite them. NOTE: ip_next_hops, bgp_next_hops and
 * vrf_name have to be allocated at runtime, since the length is not known
 * before the response. The user of the API has to call
 * routing_plane_cleanup_data_t in order to free them.
 */
typedef struct routing_plane_data_t {
	ip46 ip_subnet;
	ip46 *ip_addresses;
	ip46 *ip_next_hops;
	ip46 *bgp_next_hops;
	char *vrf_name;
	uint32_t host_asn;
	uint32_t adj_asn;
	uint32_t vrf_id;
	sa_family_t family;
	uint16_t ip_count;
	uint16_t ip_next_hop_count;
	uint16_t bgp_next_hop_count;
	uint16_t vrf_name_length;
	uint8_t pbr_table_id;
	uint8_t prefix_length;
} routing_plane_data_t;


#ifdef ZMQ_SUPPORT_ENABLED

#include <czmq.h>


struct IpHasher
{
	std::size_t operator()(const std::pair<routing_plane_data_t *,
			uint16_t>& pair) const
	{
		return boost::hash_range(
				(uint8_t *)&pair.first->ip_addresses[pair.second],
				(((uint8_t *)&pair.first->ip_addresses[pair.second])) +
				(pair.first->family == AF_INET ?
						sizeof(struct in_addr) : sizeof(struct in6_addr)));
	}
};

struct SubnetHasher
{
	std::size_t operator()(const routing_plane_data_t *key) const
	{
		// enough for ipv6 + prefix length
		uint8_t buffer[17];
		memcpy(buffer, &key->ip_subnet, (key->family == AF_INET ?
				sizeof(struct in_addr) : sizeof(struct in6_addr)));
		buffer[(key->family == AF_INET ?
				sizeof(struct in_addr) : sizeof(struct in6_addr))] =
						key->prefix_length;

		return boost::hash_range(buffer, buffer + (key->family == AF_INET ?
				sizeof(struct in_addr) : sizeof(struct in6_addr)) + 1);
	}
};


struct IpMatcher
{
	std::size_t operator()(const std::pair<routing_plane_data_t *,
			uint16_t>& rhs, const std::pair<routing_plane_data_t *,
			uint16_t>& lhs) const
	{
		if (rhs.first->family != lhs.first->family) {
			return false;
		}
		if (rhs.first->pbr_table_id !=  lhs.first->pbr_table_id) {
			return false;
		}
		if (rhs.first->vrf_name_length != lhs.first->vrf_name_length) {
			return false;
		}
		if (rhs.first->vrf_id != lhs.first->vrf_id) {
			return false;
		}
		if (memcmp(rhs.first->vrf_name, lhs.first->vrf_name,
				rhs.first->vrf_name_length)) {
			return false;
		}
		if (rhs.first->family == AF_INET) {
			if (memcmp(&rhs.first->ip_addresses[rhs.second].addr,
					&lhs.first->ip_addresses[lhs.second].addr,
					sizeof(struct in_addr))){
				return false;
			}
		} else if (rhs.first->family == AF_INET6) {
			if (memcmp(&rhs.first->ip_addresses[rhs.second].addr6,
					&lhs.first->ip_addresses[lhs.second].addr6,
					sizeof(struct in6_addr))) {
				return false;
			}
		} else {
			return false;
		}

		return true;
	}
};

struct SubnetMatcher
{
	std::size_t operator()(const routing_plane_data_t *rhs,
			const routing_plane_data_t *lhs) const
	{
		if (rhs->family != lhs->family) {
			return false;
		}
		if (rhs->pbr_table_id !=  lhs->pbr_table_id) {
			return false;
		}
		if (rhs->vrf_name_length != lhs->vrf_name_length) {
			return false;
		}
		if (rhs->vrf_id != lhs->vrf_id) {
			return false;
		}
		if (memcmp(rhs->vrf_name, lhs->vrf_name, rhs->vrf_name_length)) {
			return false;
		}
		if (rhs->family == AF_INET) {
			if (memcmp(&rhs->ip_subnet.addr, &lhs->ip_subnet.addr,
					sizeof(struct in_addr))){
				return false;
			}
		} else if (rhs->family == AF_INET6) {
			if (memcmp(&rhs->ip_subnet.addr6, &lhs->ip_subnet.addr6,
					sizeof(struct in6_addr))) {
				return false;
			}
		} else {
			return false;
		}

		return true;
	}
};

class RoutingPlane
{
public:
	RoutingPlane(std::string zmq_sub_endpoint = std::string(),
			std::string zmq_dealer_endpoint = std::string(),
			int zmq_high_watermark = 0,
			int zmq_poll_timeout = RP_ZMQ_POLL_TIMEOUT_DEFAULT,
			uint32_t cache_wiping_interval = RP_CACHE_CLEAN_INTERVAL_DEFAULT);
	virtual ~RoutingPlane();
	void reconfigure(std::string zmq_sub_endpoint,
			std::string zmq_dealer_endpoint, int zmq_high_watermark,
			int zmq_poll_timeout, uint32_t cache_wiping_interval);
	int get_data(routing_plane_data_t *routing_data[], size_t req_count,
			uint16_t flags);
	static void routing_plane_cleanup_data_t(routing_plane_data_t *routing_data,
			bool clean_ip_addresses);
	virtual void run();

protected:
	std::string zmq_sub_endpoint;
	std::string zmq_dealer_endpoint;
	int zmq_high_watermark;
	int zmq_poll_timeout;
	uint32_t cache_wiping_interval;
	bool exitFlag;

private:
	static void *threadWrapper(void *instance);
	int parse_update_reply(zmsg_t *msg_reply, size_t num_frames);
	int routing_plane_process_update(zsock_t *sock);
	void setUpHelper();
	void tearDownHelper();
	void clear_cache();

	zsock_t *zmq_dealer_socket;
	zsock_t *zmq_sub_socket;
	zpoller_t *zpoller;
	Thread thread;

	static std::unordered_map<std::pair<routing_plane_data_t *, uint16_t>,
		routing_plane_data_t *, IpHasher, IpMatcher> cache_ip;
	static std::unordered_map<routing_plane_data_t *, routing_plane_data_t *,
		SubnetHasher, SubnetMatcher> cache_subnet;
	static Mutex cache_lock;
};

#else // ZMQ_SUPPORT_ENABLED

class RoutingPlane
{
public:
	RoutingPlane(std::string zmq_sub_endpoint = std::string(),
			std::string zmq_dealer_endpoint = std::string(),
			int zmq_high_watermark = 0,
			int zmq_poll_timeout = RP_ZMQ_POLL_TIMEOUT_DEFAULT,
			uint32_t cache_wiping_interval = RP_CACHE_CLEAN_INTERVAL_DEFAULT) {
		THROWEXCEPTION("ZMQ not supported!");
	}
	void reconfigure(std::string zmq_sub_endpoint,
			std::string zmq_dealer_endpoint, int zmq_high_watermark,
			int zmq_poll_timeout, uint32_t cache_wiping_interval) {
		THROWEXCEPTION("ZMQ not supported!");
	}
	int get_data(routing_plane_data_t *routing_data[], size_t req_count,
			uint16_t flags) {
		THROWEXCEPTION("ZMQ not supported!");
	}
	static void routing_plane_cleanup_data_t(routing_plane_data_t *routing_data,
			bool clean_ip_addresses) {
		THROWEXCEPTION("ZMQ not supported!");
	}
};

#endif // ZMQ_SUPPORT_ENABLED

#endif // ROUTING_PLANE_H
