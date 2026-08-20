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

#include "IpfixEnhancer.hpp"

InstanceManager<IpfixDataRecord> IpfixEnhancer::dataRecordIM(
		"IpfixEnhancerIpfixDataRecord", 0);
InstanceManager<IpfixTemplateRecord> IpfixEnhancer::templateRecordIM(
		"IpfixEnhancerIpfixTemplateRecord", 0);
InstanceManager<IpfixTemplateDestructionRecord> IpfixEnhancer::templateDestructionRecordIM(
		"IpfixEnhancerIpfixTemplateDestructionRecord", 0);

IpfixEnhancer::IpfixEnhancer(std::list<InfoElementCfg> fields,
		RoutingPlane *routing_plane)
	: fields(fields), routing_plane(routing_plane)
{
}

IpfixEnhancer::~IpfixEnhancer()
{
}

void IpfixEnhancer::onTemplate(IpfixTemplateRecord* record)
{
	if (fields.empty()) {
		send(record);
		return;
	}

	std::list<InfoElementCfg> newFields(fields.begin(), fields.end());
	boost::shared_ptr<TemplateInfo> templateInfo = record->templateInfo;
	bool src_v4_addr_found, src_v6_addr_found, dst_v4_addr_found,
		dst_v6_addr_found;

	src_v4_addr_found =
			templateInfo->getFieldInfo(IPFIX_TYPEID_sourceIPv4Address, 0) ?
					true : false;

	src_v6_addr_found =
		templateInfo->getFieldInfo(IPFIX_TYPEID_sourceIPv6Address, 0) ?
					true : false;

	dst_v4_addr_found =
			templateInfo->getFieldInfo(IPFIX_TYPEID_destinationIPv4Address, 0) ?
					true : false;

	dst_v6_addr_found =
			templateInfo->getFieldInfo(IPFIX_TYPEID_destinationIPv6Address, 0) ?
					true : false;

	/*
	 * newFields starts as a list of supported fields by the module.
	 * We then remove all the fields that are already present in the received
	 * template, since they won't need to be added.
	 */
	for (std::list<InfoElementCfg>::iterator it=newFields.begin();
			it != newFields.end();) {
		if (templateInfo->getFieldInfo((*it).getIeId(), 0)) {
			it = newFields.erase(it);
		} else {
			++it;
		}
	}

	/*
	 * Remove all fields that are supported and present, but that we cannot
	 * add due to missing dependencies that are needed to derive them.
	 */
	if (!src_v4_addr_found || !src_v6_addr_found) {
		for (std::list<InfoElementCfg>::iterator it=newFields.begin();
				it != newFields.end();) {
			if ((((*it).getIeName() == "bgpSourceAsNumber" ||
						(*it).getIeName() == "bgpPrevAdjacentAsNumber") &&
						!src_v4_addr_found && !src_v6_addr_found) ||
					((*it).getIeName() == "sourceIPv4PrefixLength" &&
							!src_v4_addr_found) ||
					((*it).getIeName() == "sourceIPv6PrefixLength" &&
							!src_v6_addr_found)) {
				it = newFields.erase(it);
			} else {
				++it;
			}
		}
	}

	if (!dst_v4_addr_found || !dst_v6_addr_found) {
		for (std::list<InfoElementCfg>::iterator it=newFields.begin();
				it != newFields.end();) {
			if ((((*it).getIeName() == "bgpDestinationAsNumber" ||
						(*it).getIeName() == "bgpNextAdjacentAsNumber") &&
						!src_v4_addr_found && !src_v6_addr_found) ||
					(((*it).getIeName() == "destinationIPv4PrefixLength" ||
							(*it).getIeName() == "ipNextHopIPv4Address" ||
							(*it).getIeName() == "bgpNextHopIPv4Address") &&
							!src_v4_addr_found) ||
					(((*it).getIeName() == "destinationIPv6PrefixLength" ||
							(*it).getIeName() == "ipNextHopIPv6Address" ||
							(*it).getIeName() == "bgpNextHopIPv6Address") &&
							!src_v6_addr_found)) {
				it = newFields.erase(it);
			} else {
				++it;
			}
		}
	}

	if (newFields.empty()) {
		msg(LOG_INFO, "IpfixEnhancer: Received Template (id=%u) does not have,"
				" required fields, skip enhancing.", templateInfo->templateId);
		send(record);
		return;
	}

	if(uniqueIdToEnhanceInfo.find(templateInfo->getUniqueId()) !=
			uniqueIdToEnhanceInfo.end()) {
		msg(LOG_ERR, "IpfixEnhancer: Received known Template (id=%u) again, "
				"which should not happen.", templateInfo->templateId);
		record->removeReference();
		return;
	}

	// Generate new Template, starting with a copy of the Template
	boost::shared_ptr<TemplateInfo> newTemplateInfo(
			new TemplateInfo(*templateInfo.get()));

	// Make unique
	newTemplateInfo->setUniqueId();

	// Generate conversion info
	EnhanceInfo myEnhanceInfo;
	myEnhanceInfo.templateInfo = newTemplateInfo;
	myEnhanceInfo.newFields = newFields;
	myEnhanceInfo.additionalDataLength = 0;

	/*
	 * Compute the additional length of the resulting record for later reuse
	 * when enhancing the actual records.
	 */
	for (std::list<InfoElementCfg>::iterator it=newFields.begin();
			it != newFields.end(); ++it) {
		myEnhanceInfo.additionalDataLength += (*it).getIeLength();
	}


	newTemplateInfo->fieldInfo = (TemplateInfo::FieldInfo*)realloc(
			newTemplateInfo->fieldInfo,
			(newTemplateInfo->fieldCount + newFields.size()) *
			sizeof(TemplateInfo::FieldInfo));
	if (newTemplateInfo->fieldInfo == NULL) {
		THROWEXCEPTION("Could not allocate memory for new fields!");
	}

	TemplateInfo::FieldInfo newField;
	newField.isVariableLength = false;
	newField.type.enterprise = 0;
	newField.privDataOffset = 0;

	/*
	 * Add all the new fields at the end of the template, while computing the
	 * resulting offset of the data in the records.
	 */
	for (std::list<InfoElementCfg>::iterator it=newFields.begin();
			it != newFields.end(); ++it) {
		newField.type.id = (*it).getIeId();
		newField.type.length = (*it).getIeLength();
		newField.offset =
			newTemplateInfo->fieldInfo[newTemplateInfo->fieldCount - 1].offset +
			newTemplateInfo->fieldInfo[newTemplateInfo->fieldCount - 1].type.length;
		memcpy(&newTemplateInfo->fieldInfo[newTemplateInfo->fieldCount],
				&newField, sizeof(TemplateInfo::FieldInfo));
		++newTemplateInfo->fieldCount;
	}

	// Save conversion info in map
	uniqueIdToEnhanceInfo[templateInfo->getUniqueId()] = myEnhanceInfo;

	// Generate and send new Template record
	IpfixTemplateRecord* newTemplateRecord = templateRecordIM.getNewInstance();
	newTemplateRecord->sourceID = record->sourceID;
	newTemplateRecord->templateInfo = newTemplateInfo;
	send(newTemplateRecord);

	// Release original Template record
	record->removeReference();
}

void IpfixEnhancer::onTemplateDestruction(
		IpfixTemplateDestructionRecord* record)
{
	if (fields.empty()) {
		send(record);
		return;
	}

	boost::shared_ptr<TemplateInfo> templateInfo = record->templateInfo;
	// This should be a known Template for us
	map<uint16_t, EnhanceInfo>::iterator iter =
			uniqueIdToEnhanceInfo.find(templateInfo->getUniqueId());
	if(iter == uniqueIdToEnhanceInfo.end()) {
		send(record);
		return;
	}

	// Generate and send Template destruction record
	IpfixTemplateDestructionRecord* newTemplateDestructionRecord =
			templateDestructionRecordIM.getNewInstance();
	newTemplateDestructionRecord->sourceID = record->sourceID;
	newTemplateDestructionRecord->templateInfo = iter->second.templateInfo;
	send(newTemplateDestructionRecord);

	// Delete conversion info from map
	uniqueIdToEnhanceInfo.erase(iter);

	// Release original Template record
	record->removeReference();
}

void IpfixEnhancer::onDataRecord(IpfixDataRecord* record)
{
	if (fields.empty()) {
		send(record);
		return;
	}

	IpfixDataRecord* myRecord;
	boost::shared_ptr<TemplateInfo> templateInfo = record->templateInfo;
	IpfixRecord::Data* data = record->data;

	map<uint16_t, EnhanceInfo>::iterator iter =
			uniqueIdToEnhanceInfo.find(templateInfo->getUniqueId());
	if(iter == uniqueIdToEnhanceInfo.end()) {
		msg(LOG_ERR, "IpfixEnhancer: Received Data Record associated to "
				"unknown Template (id=%u), which should not happen.",
				templateInfo->templateId);
		record->removeReference();
		return;
	}

	if (iter->second.newFields.empty()) {
		send(record);
		return;
	}

	/*
	 * However unlikely and strange it might be, a flow with both v4 and v6
	 * addresses is possible in theory, so we have to support it.
	 */
	routing_plane_data_t *routing_plane_data[4] = {NULL, NULL, NULL, NULL},
		routing_plane_data_src_v4, routing_plane_data_dst_v4,
		routing_plane_data_src_v6, routing_plane_data_dst_v6;
	size_t routing_plane_count = 0;
	ip46 src_v4, dst_v4, src_v6, dst_v6;

	memset(&routing_plane_data_src_v4, 0, sizeof(routing_plane_data_t));
	memset(&routing_plane_data_dst_v4, 0, sizeof(routing_plane_data_t));
	memset(&routing_plane_data_src_v6, 0, sizeof(routing_plane_data_t));
	memset(&routing_plane_data_dst_v6, 0, sizeof(routing_plane_data_t));

	routing_plane_data_src_v4.ip_count = 1;
	routing_plane_data_dst_v4.ip_count = 1;
	routing_plane_data_src_v6.ip_count = 1;
	routing_plane_data_dst_v6.ip_count = 1;

	routing_plane_data_src_v4.ip_addresses = &src_v4;
	routing_plane_data_dst_v4.ip_addresses = &dst_v4;
	routing_plane_data_src_v6.ip_addresses = &src_v6;
	routing_plane_data_dst_v6.ip_addresses = &dst_v6;

	for (int i = 0; i < templateInfo->fieldCount; i++) {
		TemplateInfo::FieldInfo* fi = &templateInfo->fieldInfo[i];

		if (fi->type.id == IPFIX_TYPEID_sourceIPv4Address) {
			memcpy(&routing_plane_data_src_v4.ip_addresses[0].addr.s_addr,
					data + fi->offset, fi->type.length);
			routing_plane_data_src_v4.family = AF_INET;
			routing_plane_data[routing_plane_count++] =
					&routing_plane_data_src_v4;
		} else if (fi->type.id == IPFIX_TYPEID_destinationIPv4Address) {
			memcpy(&routing_plane_data_dst_v4.ip_addresses[0].addr.s_addr,
					data + fi->offset, fi->type.length);
			routing_plane_data_dst_v4.family = AF_INET;
			routing_plane_data[routing_plane_count++] =
					&routing_plane_data_dst_v4;
		} else if (fi->type.id == IPFIX_TYPEID_sourceIPv6Address) {
			memcpy(&routing_plane_data_src_v6.ip_addresses[0],
					data + fi->offset, fi->type.length);
			routing_plane_data_src_v6.family = AF_INET6;
			routing_plane_data[routing_plane_count++] =
					&routing_plane_data_src_v6;
		} else if (fi->type.id == IPFIX_TYPEID_destinationIPv6Address) {
			memcpy(&routing_plane_data_dst_v6.ip_addresses[0],
					data + fi->offset, fi->type.length);
			routing_plane_data_dst_v6.family = AF_INET6;
			routing_plane_data[routing_plane_count++] =
					&routing_plane_data_dst_v6;
		} else if (fi->type.id == IPFIX_TYPEID_ingressVRFID) {
			memcpy(&routing_plane_data_src_v4.vrf_id,
					data + fi->offset, fi->type.length);
			memcpy(&routing_plane_data_src_v6.vrf_id,
					data + fi->offset, fi->type.length);
		} else if (fi->type.id == IPFIX_TYPEID_egressVRFID) {
			memcpy(&routing_plane_data_dst_v4.vrf_id,
					data + fi->offset, fi->type.length);
			memcpy(&routing_plane_data_dst_v6.vrf_id,
					data + fi->offset, fi->type.length);
		}
	}

	int rc = routing_plane->get_data(routing_plane_data, routing_plane_count,
			NF_DATA_TYPE_ALL_FIELDS);

	if (rc) {
		send(record);
		RoutingPlane::routing_plane_cleanup_data_t(&routing_plane_data_src_v4,
				false);
		RoutingPlane::routing_plane_cleanup_data_t(&routing_plane_data_src_v6,
				false);
		RoutingPlane::routing_plane_cleanup_data_t(&routing_plane_data_dst_v4,
				false);
		RoutingPlane::routing_plane_cleanup_data_t(&routing_plane_data_dst_v6,
				false);
		return;
	}

	myRecord = dataRecordIM.getNewInstance();
	myRecord->sourceID = record->sourceID;
	myRecord->templateInfo = iter->second.templateInfo;
	myRecord->dataLength = record->dataLength +
			iter->second.additionalDataLength;
	myRecord->message = boost::shared_array<IpfixRecord::Data>(
			new IpfixRecord::Data[myRecord->dataLength]);
	memcpy(myRecord->message.get(), record->data, record->dataLength);
	myRecord->data = myRecord->message.get();
	data = myRecord->data;
	record->removeReference();

	/*
	 * This is a bit terrible, especially since it is in the critical code path,
	 * but there's really not much that can be done. We need to loop over all
	 * the fields in the record, and then if it matches one of the fields we
	 * have marked as enhanced we copy the data.
	 */
	for (int i = 0; i < myRecord->templateInfo->fieldCount; ++i) {
		TemplateInfo::FieldInfo* fi = &myRecord->templateInfo->fieldInfo[i];

		for (std::list<InfoElementCfg>::iterator it =
				iter->second.newFields.begin();
				it != iter->second.newFields.end(); ++it) {
			if (fi->type.id == (*it).getIeId()) {
				switch (fi->type.id) {
				case IPFIX_TYPEID_bgpSourceAsNumber:
					if (routing_plane_data_src_v4.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_src_v4.host_asn,
								fi->type.length);
					} else if (routing_plane_data_src_v6.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_src_v6.host_asn,
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_bgpDestinationAsNumber:
					if (routing_plane_data_dst_v4.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v4.host_asn,
								fi->type.length);
					} else if (routing_plane_data_dst_v6.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v6.host_asn,
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_sourceIPv4PrefixLength:
					if (routing_plane_data_src_v4.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_src_v4.prefix_length,
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_sourceIPv6PrefixLength:
					if (routing_plane_data_src_v6.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_src_v6.prefix_length,
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_destinationIPv4PrefixLength:
					if (routing_plane_data_dst_v4.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v4.prefix_length,
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_destinationIPv6PrefixLength:
					if (routing_plane_data_dst_v6.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v6.prefix_length,
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_bgpPrevAdjacentAsNumber:
					if (routing_plane_data_src_v4.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_src_v4.adj_asn,
								fi->type.length);
					} else if (routing_plane_data_src_v6.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_src_v6.adj_asn,
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_bgpNextAdjacentAsNumber:
					if (routing_plane_data_dst_v4.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v4.adj_asn,
								fi->type.length);
					} else if (routing_plane_data_dst_v6.family != 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v6.adj_asn,
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_ipNextHopIPv4Address:
					if (routing_plane_data_dst_v4.ip_next_hop_count > 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v4.ip_next_hops[0],
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_ipNextHopIPv6Address:
					if (routing_plane_data_dst_v6.ip_next_hop_count > 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v6.ip_next_hops[0],
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_bgpNextHopIPv4Address:
					if (routing_plane_data_dst_v4.bgp_next_hop_count > 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v4.bgp_next_hops[0],
								fi->type.length);
					}
					break;
				case IPFIX_TYPEID_bgpNextHopIPv6Address:
					if (routing_plane_data_dst_v6.bgp_next_hop_count > 0) {
						memcpy(data + fi->offset,
								&routing_plane_data_dst_v6.bgp_next_hops[0],
								fi->type.length);
					}
					break;
				default:
					break;
				}
			}
		}
	}

	RoutingPlane::routing_plane_cleanup_data_t(&routing_plane_data_src_v4,
			false);
	RoutingPlane::routing_plane_cleanup_data_t(&routing_plane_data_src_v6,
			false);
	RoutingPlane::routing_plane_cleanup_data_t(&routing_plane_data_dst_v4,
			false);
	RoutingPlane::routing_plane_cleanup_data_t(&routing_plane_data_dst_v6,
			false);

	send(myRecord);
}

void IpfixEnhancer::onReconfiguration2()
{
	// we do not need to destroy templates during reconfiguration
	// forget all conversion info
	uniqueIdToEnhanceInfo.clear();
}
