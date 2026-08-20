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

#ifndef _IPFIXENHANCER_H_
#define _IPFIXENHANCER_H_

#include <map>

#include <core/InfoElementCfg.h>
#include "core/Source.h"
#include "modules/ipfix/IpfixRecordDestination.h"
#include "modules/ipfix/RoutingPlane.hpp"

class IpfixEnhancer : public Source<IpfixRecord*>,
		public IpfixRecordDestination, public Module  {
public:
	IpfixEnhancer(std::list<InfoElementCfg> fields = std::list<InfoElementCfg>(),
			RoutingPlane *routing_plane = NULL);
	virtual ~IpfixEnhancer();
	virtual void onReconfiguration2();

protected:
	virtual void onTemplate(IpfixTemplateRecord* record);
	virtual void onDataRecord(IpfixDataRecord* record);
	virtual void onTemplateDestruction(IpfixTemplateDestructionRecord* record);

	static InstanceManager<IpfixDataRecord> dataRecordIM;
	static InstanceManager<IpfixTemplateRecord> templateRecordIM;
	static InstanceManager<IpfixTemplateDestructionRecord> templateDestructionRecordIM;

	struct EnhanceInfo {
		// IPFIX Template
		boost::shared_ptr<TemplateInfo> templateInfo;

		// fields to add to this template
		std::list<InfoElementCfg> newFields;

		// store data length of the new fields
		uint32_t additionalDataLength;
	};

	std::map<uint16_t, EnhanceInfo> uniqueIdToEnhanceInfo;

private:
	std::list<InfoElementCfg> fields;
	RoutingPlane *routing_plane;
};

#endif // _IPFIXENHANCER_H_
