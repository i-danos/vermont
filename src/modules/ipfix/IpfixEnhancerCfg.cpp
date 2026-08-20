/*
 * IPFIX enhancer module
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

#include "IpfixEnhancerCfg.hpp"

/*
 * Static array to match supported fields
 */
const std::set<string> supported_fields = {
		"ipNextHopIPv4Address", "bgpNextHopIPv4Address",
		"sourceIPv4PrefixLength", "destinationIPv4PrefixLength",
		"bgpSourceAsNumber", "bgpDestinationAsNumber",
		"bgpPrevAdjacentAsNumber", "bgpNextAdjacentAsNumber",
		"ipNextHopIPv6Address", "bgpNextHopIPv6Address",
		"sourceIPv6PrefixLength", "destinationIPv6PrefixLength"
};

IpfixEnhancerCfg* IpfixEnhancerCfg::create(XMLElement* e)
{
	assert(e);
	assert(e->getName() == getName());
	return new IpfixEnhancerCfg(e);
}

IpfixEnhancerCfg::IpfixEnhancerCfg(XMLElement* elem)
	: CfgHelper<IpfixEnhancer, IpfixEnhancerCfg>(elem, "ipfixEnhancer"),
	  zmqSubEndpoint(""), zmqReqEndpoint(""),
	  zmqHighWaterMark(0), zmqPollTimeout(RP_ZMQ_POLL_TIMEOUT_DEFAULT),
	  rpCacheWipingInterval(RP_CACHE_CLEAN_INTERVAL_DEFAULT)
{
	if (!elem) {
		return;
	}

	routing_plane = NULL;
	bool enable_routing_plane = false;

	XMLNode::XMLSet<XMLElement*> set = elem->getElementChildren();
	for (XMLNode::XMLSet<XMLElement*>::iterator it = set.begin();
			it != set.end();
			it++) {
		XMLElement* e = *it;
		if (e->matches("routingPlane") && !enable_routing_plane) {
#ifdef ZMQ_SUPPORT_ENABLED
			XMLNode::XMLSet<XMLElement*> sub_set = e->getElementChildren();
			for (XMLNode::XMLSet<XMLElement*>::iterator sub_it = sub_set.begin();
					sub_it != sub_set.end();
					sub_it++) {
				XMLElement* sub_e = *sub_it;
				if (sub_e->matches("zmqSubEndpoint")) {
					zmqSubEndpoint = sub_e->getContent();
				} else if (sub_e->matches("zmqReqEndpoint")) {
					zmqReqEndpoint = sub_e->getContent();
				} else if (sub_e->matches("zmqHighWaterMark")) {
					zmqHighWaterMark = atoi(sub_e->getContent().c_str());
				} else if (sub_e->matches("zmqPollTimeout")) {
					zmqPollTimeout = atoi(sub_e->getContent().c_str());
				} else if (sub_e->matches("cacheWipingInterval")) {
					rpCacheWipingInterval = atoi(sub_e->getContent().c_str());
				} else {
					msg(LOG_CRIT, "Unknown IpfixEnhancer config statement "
							"%s\n", sub_e->getName().c_str());
					continue;
				}
			}
			enable_routing_plane = true;
#else
			THROWEXCEPTION("ZMQ not supported, cannot use RoutingPlane!");
#endif
		} else if (e->matches("fieldsList")) {
			XMLNode::XMLSet<XMLElement*> sub_set = e->getElementChildren();
			for (XMLNode::XMLSet<XMLElement*>::iterator sub_it = sub_set.begin();
					sub_it != sub_set.end();
					sub_it++) {
				XMLElement* sub_e = *sub_it;
				if (sub_e->matches("field")) {
					InfoElementCfg ie(sub_e);
					if (!ie.isKnownIE() ||
							supported_fields.find(ie.getIeName()) ==
									supported_fields.end()) {
						msg(LOG_CRIT, "Unsupported IpfixEnhancer field %s "
								"(id=%u, ent=%" PRIu32 ").",
								(ie.getIeName()).c_str(),
								ie.getIeId(), ie.getEnterpriseNumber());
						continue;
					}
					fields.push_back(ie);
				} else {
					msg(LOG_CRIT, "Unsupported IpfixEnhancer option %s\n",
							sub_e->getName().c_str());
					continue;
				}
			}
		} else if (e->matches("next")) { // ignore next
		} else {
			msg(LOG_CRIT, "Unknown IpfixEnhancer config statement %s\n",
					e->getName().c_str());
			continue;
		}
	}

	if (enable_routing_plane &&
			(zmqSubEndpoint.empty() || zmqReqEndpoint.empty())) {
		THROWEXCEPTION("Cannot configure RoutingPlane without ZMQ endpoints");
	}
}

IpfixEnhancerCfg::~IpfixEnhancerCfg()
{
	if (routing_plane) {
		msg(LOG_NOTICE, "Deleting Routing Plane");
		delete routing_plane;
	} else {
		msg(LOG_NOTICE, "No Routing Plane to delete");
	}
}

IpfixEnhancer* IpfixEnhancerCfg::createInstance()
{
	if (!zmqSubEndpoint.empty() && !zmqReqEndpoint.empty()) {
		if (routing_plane == NULL) {
			routing_plane = new RoutingPlane(zmqSubEndpoint, zmqReqEndpoint,
					zmqHighWaterMark, zmqPollTimeout, rpCacheWipingInterval);
		} else {
			routing_plane->reconfigure(zmqSubEndpoint, zmqReqEndpoint,
					zmqHighWaterMark, zmqPollTimeout, rpCacheWipingInterval);
		}
	}
	instance = new IpfixEnhancer(fields, routing_plane);
	return instance;
}

bool IpfixEnhancerCfg::deriveFrom(IpfixEnhancerCfg* old)
{
	if (zmqSubEndpoint == old->zmqSubEndpoint &&
			zmqReqEndpoint == old->zmqReqEndpoint &&
			zmqHighWaterMark == old->zmqHighWaterMark &&
			zmqPollTimeout == old->zmqPollTimeout &&
			fields == old->fields) {
		return true;
	} else {
		return false;
	}
}
