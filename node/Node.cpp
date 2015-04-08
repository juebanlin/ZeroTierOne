/*
 * ZeroTier One - Network Virtualization Everywhere
 * Copyright (C) 2011-2015  ZeroTier, Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * --
 *
 * ZeroTier may be used and distributed under the terms of the GPLv3, which
 * are available at: http://www.gnu.org/licenses/gpl-3.0.html
 *
 * If you would like to embed ZeroTier into a commercial application or
 * redistribute it in a modified binary form, please contact ZeroTier Networks
 * LLC. Start here: http://www.zerotier.com/
 */

#include "../version.h"

#include "Constants.hpp"
#include "Node.hpp"
#include "RuntimeEnvironment.hpp"
#include "NetworkConfigMaster.hpp"
#include "CMWC4096.hpp"
#include "Switch.hpp"
#include "Multicaster.hpp"
#include "AntiRecursion.hpp"
#include "Topology.hpp"
#include "Buffer.hpp"
#include "Packet.hpp"
#include "Logger.hpp"
#include "Address.hpp"
#include "Identity.hpp"
#include "SelfAwareness.hpp"
#include "Defaults.hpp"

namespace ZeroTier {

/****************************************************************************/
/* Public Node interface (C++, exposed via CAPI bindings)                   */
/****************************************************************************/

Node::Node(
	uint64_t now,
	ZT1_DataStoreGetFunction dataStoreGetFunction,
	ZT1_DataStorePutFunction dataStorePutFunction,
	ZT1_WirePacketSendFunction wirePacketSendFunction,
	ZT1_VirtualNetworkFrameFunction virtualNetworkFrameFunction,
	ZT1_VirtualNetworkConfigFunction virtualNetworkConfigFunction,
	ZT1_StatusCallback statusCallback,
	const char *overrideRootTopology) :
	RR(new RuntimeEnvironment(this)),
	_dataStoreGetFunction(dataStoreGetFunction),
	_dataStorePutFunction(dataStorePutFunction),
	_wirePacketSendFunction(wirePacketSendFunction),
	_virtualNetworkFrameFunction(virtualNetworkFrameFunction),
	_virtualNetworkConfigFunction(virtualNetworkConfigFunction),
	_statusCallback(statusCallback),
	_networks(),
	_networks_m(),
	_now(now),
	_startTimeAfterInactivity(0),
	_lastPingCheck(0),
	_lastHousekeepingRun(0),
	_coreDesperation(0)
{
	_newestVersionSeen[0] = ZEROTIER_ONE_VERSION_MAJOR;
	_newestVersionSeen[1] = ZEROTIER_ONE_VERSION_MINOR;
	_newestVersionSeen[2] = ZEROTIER_ONE_VERSION_REVISION;

	std::string idtmp(dataStoreGet("identity.secret"));
	if ((!idtmp.length())||(!RR->identity.fromString(idtmp))||(!RR->identity.hasPrivate())) {
		RR->identity.generate();
		idtmp = RR->identity.toString(true);
		if (!dataStorePut("identity.secret",idtmp,true)) {
			delete RR;
			throw std::runtime_error("unable to write identity.secret");
		}
		idtmp = RR->identity.toString(false);
		if (!dataStorePut("identity.public",idtmp,false)) {
			delete RR;
			throw std::runtime_error("unable to write identity.public");
		}
	}

	try {
		RR->prng = new CMWC4096();
		RR->sw = new Switch(RR);
		RR->mc = new Multicaster(RR);
		RR->antiRec = new AntiRecursion();
		RR->topology = new Topology(RR);
		RR->sa = new SelfAwareness(RR);
	} catch ( ... ) {
		delete RR->sa;
		delete RR->topology;
		delete RR->antiRec;
		delete RR->mc;
		delete RR->sw;
		delete RR->prng;
		delete RR->log;
		delete RR;
		throw;
	}

	Dictionary rt;
	if (overrideRootTopology) {
		rt.fromString(std::string(overrideRootTopology));
	} else {
		std::string rttmp(dataStoreGet("root-topology"));
		if (rttmp.length() > 0) {
			rt.fromString(rttmp);
			if (!Topology::authenticateRootTopology(rt))
				rt.clear();
		}
		if (!rt.size())
			rt.fromString(ZT_DEFAULTS.defaultRootTopology);
	}
	RR->topology->setSupernodes(Dictionary(rt.get("supernodes","")));

	postEvent(ZT1_EVENT_UP);
}

Node::~Node()
{
	delete RR->sa;
	delete RR->topology;
	delete RR->antiRec;
	delete RR->mc;
	delete RR->sw;
	delete RR->prng;
	delete RR->log;
	delete RR;
}

ZT1_ResultCode Node::processWirePacket(
	uint64_t now,
	const struct sockaddr_storage *remoteAddress,
	unsigned int linkDesperation,
	const void *packetData,
	unsigned int packetLength,
	uint64_t *nextBackgroundTaskDeadline)
{
	if (now >= *nextBackgroundTaskDeadline) {
		ZT1_ResultCode rc = processBackgroundTasks(now,nextBackgroundTaskDeadline);
		if (rc != ZT1_RESULT_OK)
			return rc;
	} else _now = now;

	RR->sw->onRemotePacket(*(reinterpret_cast<const InetAddress *>(remoteAddress)),linkDesperation,packetData,packetLength);

	return ZT1_RESULT_OK;
}

ZT1_ResultCode Node::processVirtualNetworkFrame(
	uint64_t now,
	uint64_t nwid,
	uint64_t sourceMac,
	uint64_t destMac,
	unsigned int etherType,
	unsigned int vlanId,
	const void *frameData,
	unsigned int frameLength,
	uint64_t *nextBackgroundTaskDeadline)
{
	if (now >= *nextBackgroundTaskDeadline) {
		ZT1_ResultCode rc = processBackgroundTasks(now,nextBackgroundTaskDeadline);
		if (rc != ZT1_RESULT_OK)
			return rc;
	} else _now = now;

	try {
		SharedPtr<Network> nw(network(nwid));
		if (nw)
			RR->sw->onLocalEthernet(nw,MAC(sourceMac),MAC(destMac),etherType,vlanId,frameData,frameLength);
		else return ZT1_RESULT_ERROR_NETWORK_NOT_FOUND;
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}

	return ZT1_RESULT_OK;
}

class _PingPeersThatNeedPing
{
public:
	_PingPeersThatNeedPing(const RuntimeEnvironment *renv,uint64_t now) :
		lastReceiveFromSupernode(0),
		RR(renv),
		_now(now),
		_supernodes(RR->topology->supernodeAddresses()) {}

	uint64_t lastReceiveFromSupernode;

	inline void operator()(Topology &t,const SharedPtr<Peer> &p)
	{
		if (std::find(_supernodes.begin(),_supernodes.end(),p->address()) != _supernodes.end()) {
			p->doPingAndKeepalive(RR,_now);
			if (p->lastReceive() > lastReceiveFromSupernode)
				lastReceiveFromSupernode = p->lastReceive();
		} else if (p->alive(_now)) {
			p->doPingAndKeepalive(RR,_now);
		}
	}
private:
	const RuntimeEnvironment *RR;
	uint64_t _now;
	std::vector<Address> _supernodes;
};

ZT1_ResultCode Node::processBackgroundTasks(uint64_t now,uint64_t *nextBackgroundTaskDeadline)
{
	_now = now;
	Mutex::Lock bl(_backgroundTasksLock);

	if ((now - _lastPingCheck) >= ZT_PING_CHECK_INVERVAL) {
		_lastPingCheck = now;

		if ((now - _startTimeAfterInactivity) > (ZT_PING_CHECK_INVERVAL * 3))
			_startTimeAfterInactivity = now;

		try {
			_PingPeersThatNeedPing pfunc(RR,now);
			RR->topology->eachPeer<_PingPeersThatNeedPing &>(pfunc);

			_coreDesperation = (unsigned int)((now - std::max(_startTimeAfterInactivity,pfunc.lastReceiveFromSupernode)) / (ZT_PING_CHECK_INVERVAL * ZT_CORE_DESPERATION_INCREMENT));
		} catch ( ... ) {
			return ZT1_RESULT_FATAL_ERROR_INTERNAL;
		}

		try {
			Mutex::Lock _l(_networks_m);
			for(std::map< uint64_t,SharedPtr<Network> >::const_iterator n(_networks.begin());n!=_networks.end();++n) {
				if ((now - n->second->lastConfigUpdate()) >= ZT_NETWORK_AUTOCONF_DELAY)
					n->second->requestConfiguration();
			}
		} catch ( ... ) {
			return ZT1_RESULT_FATAL_ERROR_INTERNAL;
		}
	}

	if ((now - _lastHousekeepingRun) >= ZT_HOUSEKEEPING_PERIOD) {
		_lastHousekeepingRun = now;

		try {
			RR->topology->clean(now);
		} catch ( ... ) {
			return ZT1_RESULT_FATAL_ERROR_INTERNAL;
		}

		try {
			RR->mc->clean(now);
		} catch ( ... ) {
			return ZT1_RESULT_FATAL_ERROR_INTERNAL;
		}
	}

	try {
		*nextBackgroundTaskDeadline = now + (uint64_t)std::max(std::min((unsigned long)ZT_PING_CHECK_INVERVAL,RR->sw->doTimerTasks(now)),(unsigned long)ZT_CORE_TIMER_TASK_GRANULARITY);
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}

	return ZT1_RESULT_OK;
}

ZT1_ResultCode Node::join(uint64_t nwid)
{
	Mutex::Lock _l(_networks_m);
	SharedPtr<Network> &nw = _networks[nwid];
	if (!nw)
		nw = SharedPtr<Network>(new Network(RR,nwid));
	return ZT1_RESULT_OK;
}

ZT1_ResultCode Node::leave(uint64_t nwid)
{
	Mutex::Lock _l(_networks_m);
	std::map< uint64_t,SharedPtr<Network> >::iterator nw(_networks.find(nwid));
	if (nw != _networks.end()) {
		nw->second->destroy();
		_networks.erase(nw);
	}
}

ZT1_ResultCode Node::multicastSubscribe(uint64_t nwid,uint64_t multicastGroup,unsigned long multicastAdi)
{
	Mutex::Lock _l(_networks_m);
	std::map< uint64_t,SharedPtr<Network> >::iterator nw(_networks.find(nwid));
	if (nw != _networks.end())
		nw->second->multicastSubscribe(MulticastGroup(MAC(multicastGroup),(uint32_t)(multicastAdi & 0xffffffff)));
}

ZT1_ResultCode Node::multicastUnsubscribe(uint64_t nwid,uint64_t multicastGroup,unsigned long multicastAdi)
{
	Mutex::Lock _l(_networks_m);
	std::map< uint64_t,SharedPtr<Network> >::iterator nw(_networks.find(nwid));
	if (nw != _networks.end())
		nw->second->multicastUnsubscribe(MulticastGroup(MAC(multicastGroup),(uint32_t)(multicastAdi & 0xffffffff)));
}

void Node::status(ZT1_NodeStatus *status)
{
}

ZT1_PeerList *Node::peers()
{
}

ZT1_VirtualNetworkConfig *Node::networkConfig(uint64_t nwid)
{
	Mutex::Lock _l(_networks_m);
	std::map< uint64_t,SharedPtr<Network> >::iterator nw(_networks.find(nwid));
	if (nw != _networks.end()) {
		ZT1_VirtualNetworkConfig *nc = (ZT1_VirtualNetworkConfig *)::malloc(sizeof(ZT1_VirtualNetworkConfig));
		nw->second->externalConfig(nc);
		return nc;
	}
	return (ZT1_VirtualNetworkConfig *)0;
}

ZT1_VirtualNetworkList *Node::networks()
{
}

void Node::freeQueryResult(void *qr)
{
	if (qr)
		::free(qr);
}

void Node::setNetconfMaster(void *networkConfigMasterInstance)
{
	RR->netconfMaster = reinterpret_cast<NetworkConfigMaster *>(networkConfigMasterInstance);
}

/****************************************************************************/
/* Node methods used only within node/                                      */
/****************************************************************************/

std::string Node::dataStoreGet(const char *name)
{
	char buf[16384];
	std::string r;
	unsigned long olen = 0;
	do {
		long n = _dataStoreGetFunction(reinterpret_cast<ZT1_Node *>(this),name,buf,sizeof(buf),r.length(),&olen);
		if (n <= 0)
			return std::string();
		r.append(buf,n);
	} while (r.length() < olen);
	return r;
}

void Node::postNewerVersionIfNewer(unsigned int major,unsigned int minor,unsigned int rev)
{
	if (Peer::compareVersion(major,minor,rev,_newestVersionSeen[0],_newestVersionSeen[1],_newestVersionSeen[2]) > 0) {
		_newestVersionSeen[0] = major;
		_newestVersionSeen[1] = minor;
		_newestVersionSeen[2] = rev;
		this->postEvent(ZT1_EVENT_SAW_MORE_RECENT_VERSION);
	}
}

} // namespace ZeroTier

/****************************************************************************/
/* CAPI bindings                                                            */
/****************************************************************************/

extern "C" {

enum ZT1_ResultCode ZT1_Node_new(
	ZT1_Node **node,
	uint64_t now,
	ZT1_DataStoreGetFunction dataStoreGetFunction,
	ZT1_DataStorePutFunction dataStorePutFunction,
	ZT1_WirePacketSendFunction wirePacketSendFunction,
	ZT1_VirtualNetworkFrameFunction virtualNetworkFrameFunction,
	ZT1_VirtualNetworkConfigFunction virtualNetworkConfigFunction,
	ZT1_StatusCallback statusCallback,
	const char *overrideRootTopology)
{
	*node = (ZT1_Node *)0;
	try {
		*node = reinterpret_cast<ZT1_Node *>(new ZeroTier::Node(now,dataStoreGetFunction,dataStorePutFunction,wirePacketSendFunction,virtualNetworkFrameFunction,virtualNetworkConfigFunction,statusCallback,overrideRootTopology));
		return ZT1_RESULT_OK;
	} catch (std::bad_alloc &exc) {
		return ZT1_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch (std::runtime_error &exc) {
		return ZT1_RESULT_FATAL_ERROR_DATA_STORE_FAILED;
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}
}

void ZT1_Node_delete(ZT1_Node *node)
{
	try {
		delete (reinterpret_cast<ZeroTier::Node *>(node));
	} catch ( ... ) {}
}

enum ZT1_ResultCode ZT1_Node_processWirePacket(
	ZT1_Node *node,
	uint64_t now,
	const struct sockaddr_storage *remoteAddress,
	unsigned int linkDesperation,
	const void *packetData,
	unsigned int packetLength,
	uint64_t *nextBackgroundTaskDeadline)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->processWirePacket(now,remoteAddress,linkDesperation,packetData,packetLength,nextBackgroundTaskDeadline);
	} catch (std::bad_alloc &exc) {
		return ZT1_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT1_RESULT_ERROR_PACKET_INVALID;
	}
}

enum ZT1_ResultCode ZT1_Node_processVirtualNetworkFrame(
	ZT1_Node *node,
	uint64_t now,
	uint64_t nwid,
	uint64_t sourceMac,
	uint64_t destMac,
	unsigned int etherType,
	unsigned int vlanId,
	const void *frameData,
	unsigned int frameLength,
	uint64_t *nextBackgroundTaskDeadline)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->processVirtualNetworkFrame(now,nwid,sourceMac,destMac,etherType,vlanId,frameData,frameLength,nextBackgroundTaskDeadline);
	} catch (std::bad_alloc &exc) {
		return ZT1_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT1_ResultCode ZT1_Node_processBackgroundTasks(ZT1_Node *node,uint64_t now,uint64_t *nextBackgroundTaskDeadline)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->processBackgroundTasks(now,nextBackgroundTaskDeadline);
	} catch (std::bad_alloc &exc) {
		return ZT1_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT1_ResultCode ZT1_Node_join(ZT1_Node *node,uint64_t nwid)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->join(nwid);
	} catch (std::bad_alloc &exc) {
		return ZT1_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT1_ResultCode ZT1_Node_leave(ZT1_Node *node,uint64_t nwid)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->leave(nwid);
	} catch (std::bad_alloc &exc) {
		return ZT1_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT1_ResultCode ZT1_Node_multicastSubscribe(ZT1_Node *node,uint64_t nwid,uint64_t multicastGroup,unsigned long multicastAdi)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->multicastSubscribe(nwid,multicastGroup,multicastAdi);
	} catch (std::bad_alloc &exc) {
		return ZT1_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}
}

enum ZT1_ResultCode ZT1_Node_multicastUnsubscribe(ZT1_Node *node,uint64_t nwid,uint64_t multicastGroup,unsigned long multicastAdi)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->multicastUnsubscribe(nwid,multicastGroup,multicastAdi);
	} catch (std::bad_alloc &exc) {
		return ZT1_RESULT_FATAL_ERROR_OUT_OF_MEMORY;
	} catch ( ... ) {
		return ZT1_RESULT_FATAL_ERROR_INTERNAL;
	}
}

void ZT1_Node_status(ZT1_Node *node,ZT1_NodeStatus *status)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->status(status);
	} catch ( ... ) {}
}

ZT1_PeerList *ZT1_Node_peers(ZT1_Node *node)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->peers();
	} catch ( ... ) {
		return (ZT1_PeerList *)0;
	}
}

ZT1_VirtualNetworkConfig *ZT1_Node_networkConfig(ZT1_Node *node,uint64_t nwid)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->networkConfig(nwid);
	} catch ( ... ) {
		return (ZT1_VirtualNetworkConfig *)0;
	}
}

ZT1_VirtualNetworkList *ZT1_Node_networks(ZT1_Node *node)
{
	try {
		return reinterpret_cast<ZeroTier::Node *>(node)->networks();
	} catch ( ... ) {
		return (ZT1_VirtualNetworkList *)0;
	}
}

void ZT1_Node_freeQueryResult(ZT1_Node *node,void *qr)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->freeQueryResult(qr);
	} catch ( ... ) {}
}

void ZT1_Node_setNetconfMaster(ZT1_Node *node,void *networkConfigMasterInstance)
{
	try {
		reinterpret_cast<ZeroTier::Node *>(node)->setNetconfMaster(networkConfigMasterInstance);
	} catch ( ... ) {}
}

void ZT1_version(int *major,int *minor,int *revision,unsigned long *featureFlags)
{
	if (major) *major = ZEROTIER_ONE_VERSION_MAJOR;
	if (minor) *minor = ZEROTIER_ONE_VERSION_MINOR;
	if (revision) *revision = ZEROTIER_ONE_VERSION_REVISION;
	if (featureFlags) {
		*featureFlags = (
			ZT1_FEATURE_FLAG_THREAD_SAFE
#ifdef ZT_OFFICIAL_BUILD
			| ZT1_FEATURE_FLAG_OFFICIAL
#endif
		);
	}
}

} // extern "C"