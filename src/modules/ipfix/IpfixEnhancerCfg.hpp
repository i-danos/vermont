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

#ifndef _IPFIXENHANCERCFG_H_
#define _IPFIXENHANCERCFG_H_

#include <core/XMLElement.h>
#include <core/Cfg.h>
#include <core/InfoElementCfg.h>

#include "modules/ipfix/IpfixEnhancer.hpp"
#include "modules/ipfix/RoutingPlane.hpp"

#include <string>

using namespace std;

class IpfixEnhancerCfg
	: public CfgHelper<IpfixEnhancer, IpfixEnhancerCfg>
{
public:
	friend class ConfigManager;

	virtual IpfixEnhancerCfg* create(XMLElement* e);
	virtual ~IpfixEnhancerCfg();

	virtual IpfixEnhancer* createInstance();
	virtual bool deriveFrom(IpfixEnhancerCfg* old);

	RoutingPlane *routing_plane;

protected:
	std::list<InfoElementCfg> fields;
	std::string zmqSubEndpoint;
	std::string zmqReqEndpoint;
	int zmqHighWaterMark;
	int zmqPollTimeout;
	uint32_t rpCacheWipingInterval;

	IpfixEnhancerCfg(XMLElement*);
};


#endif // _IPFIXENHANCERCFG_H_
