/*
	This file is part of FISCO-BCOS.

	FISCO-BCOS is free software: you can redistribute it and/or modify
	it under the terms of the GNU General Public License as published by
	the Free Software Foundation, either version 3 of the License, or
	(at your option) any later version.

	FISCO-BCOS is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
	GNU General Public License for more details.

	You should have received a copy of the GNU General Public License
	along with FISCO-BCOS.  If not, see <http://www.gnu.org/licenses/>.
*/
/** @file HostSSL.cpp
 * @author toxotguo
 * @date 2018
 */

#include <set>
#include <chrono>
#include <thread>
#include <mutex>
#include <memory>
#include <stdlib.h>
#include <boost/algorithm/string.hpp>
#include <libdevcore/Assertions.h>
#include <libdevcore/Common.h>
#include <libdevcore/easylog.h>
#include <libdevcore/CommonIO.h>
#include <libethcore/CommonJS.h>
#include <libdevcore/Exceptions.h>
#include <libdevcore/FileSystem.h>
#include <libdevcore/SHA3.h>
#include <libethereum/SystemContract.h>
#include <libdevcrypto/Rsa.h>
#include <libethereum/NodeConnParamsManagerApi.h>
#include "Session.h"
#include "Common.h"
#include "Capability.h"
#include "UPnP.h"
#include "RLPxHandshakeSSL.h"
#include "SessionWBCAData.h"
#include "HostSSL.h"
#include "ParseCert.h"
using namespace std;
using namespace dev;
using namespace dev::p2p;
using namespace dev::eth;
using namespace dev::crypto;

/// Interval at which HostSSL::run will call keepAlivePeers to ping peers.
std::chrono::seconds const c_keepAliveIntervalSSL = std::chrono::seconds(20);
std::chrono::seconds const c_reconnectNodesIntervalSSL = std::chrono::seconds(60);
std::chrono::seconds const c_AnnouncementConnectNodesIntervalSSL = std::chrono::seconds(60);
const size_t c_maxAnnouncementSize = 100;

std::chrono::milliseconds const c_keepAliveTimeOutSSL = std::chrono::milliseconds(10000);

void HostSSL::doneWorking()
{
	// reset ioservice (cancels all timers and allows manually polling network, below)
	m_ioService.reset();

	DEV_GUARDED(x_timers)
	m_timers.clear();

	// shutdown acceptor
	m_tcp4Acceptor.cancel();
	if (m_tcp4Acceptor.is_open())
		m_tcp4Acceptor.close();

	while (m_accepting)
		m_ioService.poll();

	// stop capabilities (eth: stops syncing or block/tx broadcast)
	for (auto const& h : m_capabilities)
		h.second->onStopping();

	// disconnect pending handshake, before peers, as a handshake may create a peer
	for (unsigned n = 0;; n = 0)
	{
		DEV_GUARDED(x_connecting)
		for (auto const& i : m_connecting)
			if (auto h = i.lock())
			{
				h->cancel();
				n++;
			}
		if (!n)
			break;
		m_ioService.poll();
	}

	// disconnect peers
	for (unsigned n = 0;; n = 0)
	{
		DEV_RECURSIVE_GUARDED(x_sessions)
		for (auto i : m_sessions)
			if (auto p = i.second.lock())
				if (p->isConnected())
				{
					p->disconnect(ClientQuit);
					n++;
				}
		if (!n)
			break;

		// poll so that peers send out disconnect packets
		m_ioService.poll();
	}

	// stop network (again; helpful to call before subsequent reset())
	m_ioService.stop();

	// reset network (allows reusing ioservice in future)
	m_ioService.reset();

	// finally, clear out peers (in case they're lingering)
	RecursiveGuard l(x_sessions);
	m_sessions.clear();
}

void HostSSL::startPeerSession( RLP const& _rlp, unique_ptr<RLPXFrameCoder>&& _io, std::shared_ptr<RLPXSocketSSL> const& _s, RLPBaseData &/*rlpBaseData*/)
{
	
	auto protocolVersion = _rlp[0].toInt<unsigned>();
	auto clientVersion = _rlp[1].toString();
	auto caps = _rlp[2].toVector<CapDesc>();
	auto listenPort = _rlp[3].toInt<unsigned short>();
	auto pub = _rlp[4].toHash<Public>();
	LOG(INFO) << "HostSSL::startPeerSession! " << pub.abridged() ;
	Public  _id=pub;

	//connection must be disconnect before the creation of session object and peer object - morebtcg
	if(_id == id()) {
		LOG(TRACE) << "Disconnect self: "
			<< _id.abridged() << "@" << _s->nodeIPEndpoint().address.to_string()
			<< ":" << _s->nodeIPEndpoint().tcpPort;

		_s->close();
		throw Exception("Disconnect self");

		//ps->disconnect(LocalIdentity);
		return;
	}

	NodeIPEndpoint _nodeIPEndpoint;
	_nodeIPEndpoint.address = _s->remoteEndpoint().address();
	_nodeIPEndpoint.tcpPort=listenPort;
	_nodeIPEndpoint.udpPort=listenPort;
	_nodeIPEndpoint.host=_s->nodeIPEndpoint().host;

	shared_ptr<Peer> p;
	DEV_RECURSIVE_GUARDED(x_sessions)
	{
		if (m_peers.count(_nodeIPEndpoint.name()))
			p = m_peers[_nodeIPEndpoint.name()];
		else
		{
			p = make_shared<Peer>(Node(_id, _nodeIPEndpoint));

			m_peers[_nodeIPEndpoint.name()] = p;
		}
	}
	if (p->isOffline())
		p->m_lastConnected = std::chrono::system_clock::now();
	p->endpoint=_nodeIPEndpoint;
	
	stringstream capslog;
	caps.erase(remove_if(caps.begin(), caps.end(), [&](CapDesc const & _r) { return !haveCapability(_r) || any_of(caps.begin(), caps.end(), [&](CapDesc const & _o) { return _r.first == _o.first && _o.second > _r.second && haveCapability(_o); }); }), caps.end());

	for (auto cap : caps)
		capslog << "(" << cap.first << "," << dec << cap.second << ")";

	LOG(INFO) << "Hello: " << clientVersion << "V[" << protocolVersion << "]" << _id << showbase << capslog.str() << dec << listenPort;

	shared_ptr<SessionFace> ps = make_shared<Session>(this, move(_io), _s, p, PeerSessionInfo({_id, clientVersion, p->endpoint.address.to_string(), listenPort, chrono::steady_clock::duration(), _rlp[2].toSet<CapDesc>(), 0, map<string, string>(), _nodeIPEndpoint}));
	//((Session *)ps.get())->setStatistics(new InterfaceStatistics(getDataDir() + "P2P" + p->id.hex(), m_statisticsInterval));

	if (protocolVersion < dev::p2p::c_protocolVersion - 1)
	{
		ps->disconnect(IncompatibleProtocol);
		return;
	}
	if (caps.empty())
	{
		ps->disconnect(UselessPeer);
		return;
	}

	if (m_netPrefs.pin && !m_requiredPeers.count(_id))
	{
		LOG(WARNING) << "Unexpected identity from peer (got" << _id << ", must be one of " << m_requiredPeers << ")";
		ps->disconnect(UnexpectedIdentity);
		return;
	}

	{
		RecursiveGuard l(x_sessions);
		if (m_sessions.count(_id) && !!m_sessions[_id].lock())
			if (auto s = m_sessions[_id].lock())
				if (s->isConnected())
				{
					// Already connected.
					LOG(WARNING) << "Session already exists for peer with id " << _id.abridged();
					ps->disconnect(DuplicatePeer);
					return;
				}

		if (!peerSlotsAvailable())
		{
			LOG(WARNING) << "too many  peer ! " ;
			ps->disconnect(TooManyPeers);
			return;
		}

		unsigned offset = (unsigned)UserPacket;
		uint16_t cnt = 1;

		for (auto const& i : caps)
		{
			auto pcap = m_capabilities[i];
			if (!pcap)
				return ps->disconnect(IncompatibleProtocol);

			if (Session::isFramingAllowedForVersion(protocolVersion))
				pcap->newPeerCapability(ps, 0, i, cnt++);
			else
			{
				pcap->newPeerCapability(ps, offset, i, 0);
				offset += pcap->messageCount();
			}
		}

		ps->start();
		m_sessions[_id] = ps;
	}

	LOG(INFO) << "p2p.host.peer.register: " << _id;
}

void HostSSL::onNodeTableEvent(NodeID const& _n, NodeTableEventType const& _e)
{
	
}

void HostSSL::determinePublic()
{
	auto ifAddresses = Network::getInterfaceAddresses();
	auto laddr = m_netPrefs.listenIPAddress.empty() ? bi::address() : bi::address::from_string(m_netPrefs.listenIPAddress);
	auto lset = !laddr.is_unspecified();
	auto paddr = m_netPrefs.publicIPAddress.empty() ? bi::address() : bi::address::from_string(m_netPrefs.publicIPAddress);
	auto pset = !paddr.is_unspecified();

	bool listenIsPublic = lset && isPublicAddress(laddr);
	bool publicIsHost = !lset && pset && ifAddresses.count(paddr);

	bi::tcp::endpoint ep(bi::address(), m_listenPort);
	if (m_netPrefs.traverseNAT && listenIsPublic)
	{
		LOG(INFO) << "Listen address set to Public address:" << laddr << ". UPnP disabled.";
		ep.address(laddr);
	}
	else if (m_netPrefs.traverseNAT && publicIsHost)
	{
		LOG(INFO) << "Public address set to Host configured address:" << paddr << ". UPnP disabled.";
		ep.address(paddr);
	}
	else if (m_netPrefs.traverseNAT)
	{
		bi::address natIFAddr;
		ep = Network::traverseNAT(lset && ifAddresses.count(laddr) ? std::set<bi::address>({laddr}) : ifAddresses, m_listenPort, natIFAddr);

		if (lset && natIFAddr != laddr)
			LOG(WARNING) << "Listen address:" << laddr << " differs from local address:" << natIFAddr << " returned by UPnP!";

		if (pset && ep.address() != paddr)
		{
			LOG(WARNING) << "Specified public address:" << paddr << " differs from external address:" << ep.address() << " returned by UPnP!";
			ep.address(paddr);
		}
	}
	else if (pset)
		ep.address(paddr);

	m_tcpPublic = ep;
}


void HostSSL::sslHandshakeServer(const boost::system::error_code& error, std::shared_ptr<RLPXSocketSSL> socket)
{
	if (error)
	{
		LOG(WARNING) << "HostSSL::async_handshake err:" << error.message();
	}

	bool success = false;
	try
	{
		// incoming connection; we don't yet know nodeid
		auto handshake = make_shared<RLPXHandshakeSSL>(this, socket);
		m_connecting.push_back(handshake);
		handshake->start();
		success = true;
	}
	catch (Exception const& _e)
	{
		LOG(WARNING) << "ERROR: " << diagnostic_information(_e);
	}
	catch (std::exception const& _e)
	{
		LOG(WARNING) << "ERROR: " << _e.what();
	}
	if (!success)
		socket->ref().close();
	runAcceptor(); 
}


bool HostSSL::sslVerifyCert(bool preverified, ba::ssl::verify_context& ctx)
{
	ParseCert parseCert;
	parseCert.ParseInfo(ctx);
	string subjectName = parseCert.getSubjectName();
	int certType = parseCert.getCertType();
	bool isExpire = parseCert.getExpire();
	string serialNumber = parseCert.getSerialNumber();
	LOG(DEBUG) << "subjectName:" << subjectName;
	LOG(DEBUG) << "serialNumber:" << serialNumber;
	LOG(DEBUG) << "preverified:" << preverified;
	LOG(DEBUG) << "certType:" << certType;

	if (isExpire)
	{
		LOG(WARNING) << "Verify Certificate Expire Data Error!";
		return false;
	}

	if (certType == 1)
	{
		if ( true == NodeConnManagerSingleton::GetInstance().checkCertOut(serialNumber) )
		{
			LOG(WARNING) << "Verify Certificate: Has Out! ("<<serialNumber<<")";
			return false;
		}
	}
	return preverified;
}

void HostSSL::runAcceptor()
{
	assert(m_listenPort > 0);

	if (m_run && !m_accepting)
	{
		LOG(INFO) << "Listening on local port " << m_listenPort << " (public: " << m_tcpPublic << ")";
		m_accepting = true;

		LOG(INFO) << "P2P Start Accept";
		std::shared_ptr<RLPXSocketSSL> socket;
		socket.reset(new RLPXSocketSSL(m_ioService,NodeIPEndpoint()));
			
		socket->sslref().set_verify_callback(boost::bind(&HostSSL::sslVerifyCert, this, _1, _2));
	
		m_tcp4Acceptor.async_accept(socket->ref(), m_strand.wrap([ = ](boost::system::error_code ec)
		{
			auto remoteEndpoint = socket->ref().remote_endpoint();
			LOG(INFO) << "P2P Recv Connect: " << remoteEndpoint.address().to_string() << ":" << remoteEndpoint.port();

			m_accepting = false;
			if (ec || !m_run)
			{
				socket->close();
				return;
			}
			if (peerCount() > peerSlots(Ingress))
			{
				LOG(INFO) << "Dropping incoming connect due to maximum peer count (" << Ingress << " * ideal peer count): " << socket->remoteEndpoint();
				socket->close();
				if (ec.value() < 1)
					runAcceptor();
				return;
			}

			m_tcpClient = socket->remoteEndpoint();
			socket->setNodeIPEndpoint(NodeIPEndpoint(m_tcpClient.address(),(uint16_t)0,m_tcpClient.port()));
			LOG(INFO) << "client port:" << m_tcpClient.port() << "|ip:" << m_tcpClient.address().to_string();
			LOG(INFO) << "server port:" << m_listenPort << "|ip:" << m_tcpPublic.address().to_string();
			
			socket->sslref().async_handshake(ba::ssl::stream_base::server, m_strand.wrap(boost::bind(&HostSSL::sslHandshakeServer, this, ba::placeholders::error, socket)) );
		}));
	}
}

void HostSSL::addPeer(NodeSpec const& _s, PeerType _t)
{
	LOG(TRACE) << "HostSSL::addPeer" << _s.enode();
	connect( _s.nodeIPEndpoint());
	
}

void HostSSL::addNode(NodeID const& _node, NodeIPEndpoint const& _endpoint)
{
	while (!haveNetwork())
		if (isWorking())
			this_thread::sleep_for(chrono::milliseconds(50));
		else
			return;

	connect(_endpoint);
}

void HostSSL::requirePeer(NodeID const& _n, NodeIPEndpoint const& _endpoint)
{
	LOG(TRACE) << "HostSSL::requirePeer" << _n.abridged();
	connect(_endpoint);
}

void HostSSL::relinquishPeer(NodeID const& _node)
{
	Guard l(x_requiredPeers);
	if (m_requiredPeers.count(_node))
		m_requiredPeers.erase(_node);
}

void HostSSL::sslHandshakeClient(const boost::system::error_code& error, std::shared_ptr<RLPXSocketSSL> socket, NodeID id, NodeIPEndpoint& _nodeIPEndpoint)
{
	if (error)
	{
		m_pendingPeerConns.erase(_nodeIPEndpoint.name());
		LOG(WARNING) << "HostSSL::sslHandshakeClient Err:" << error.message();
		return ;
	}

	auto handshake = make_shared<RLPXHandshakeSSL>(this, socket, id);
	{
		Guard l(x_connecting);
		m_connecting.push_back(handshake);
	}
	handshake->start();

	Guard l(x_pendingNodeConns);
	m_pendingPeerConns.erase(_nodeIPEndpoint.name());
}

void HostSSL::connect(NodeIPEndpoint const& _nodeIPEndpoint)
{
	if (!m_run)
		return;

	if (((!m_netPrefs.listenIPAddress.empty()
			&& _nodeIPEndpoint.address
					== boost::asio::ip::address::from_string(
							m_netPrefs.listenIPAddress))
			|| (!m_netPrefs.publicIPAddress.empty()
					&& _nodeIPEndpoint.address
							== boost::asio::ip::address::from_string(
									m_netPrefs.publicIPAddress))
			|| m_ifAddresses.find(_nodeIPEndpoint.address) != m_ifAddresses.end()
			|| _nodeIPEndpoint.address == m_tcpPublic.address()
			|| _nodeIPEndpoint.address == m_tcpClient.address())
			&& _nodeIPEndpoint.tcpPort == m_netPrefs.listenPort) {
		LOG(TRACE)<< "Ignore connect self" << _nodeIPEndpoint.name();

		return;
	}

	if( m_tcpPublic == _nodeIPEndpoint)
	{
		LOG(TRACE) <<"Abort Connect Self("<<_nodeIPEndpoint.name()<<")";
		return;
	}
	if (NodeIPEndpoint(bi::address::from_string(m_netPrefs.listenIPAddress),listenPort(),listenPort()) == _nodeIPEndpoint)
	{
		LOG(TRACE) <<"Abort Connect Self("<<_nodeIPEndpoint.name()<<")";
		return;
	}
	if( m_peers.count(_nodeIPEndpoint.name() ) )
	{
		LOG(TRACE) <<"Don't Repeat Connect ("<<_nodeIPEndpoint.name() <<")";
		if( !_nodeIPEndpoint.host.empty() )
			m_peers[_nodeIPEndpoint.name()]->endpoint.host = _nodeIPEndpoint.host;
		return;
	}
	if( _nodeIPEndpoint.address.to_string().empty() )
	{
		LOG(TRACE) <<"Target Node Ip Is Empty  ("<<_nodeIPEndpoint.name()<<")";
		return;
	}
	
	{
		Guard l(x_pendingNodeConns);
		if (m_pendingPeerConns.count(_nodeIPEndpoint.name()))
			return;
		m_pendingPeerConns.insert(_nodeIPEndpoint.name());
	}

	LOG(INFO) << "Attempting connection to node " << id().abridged() << "@" << _nodeIPEndpoint.name() << "," <<  _nodeIPEndpoint.host;
	std::shared_ptr<RLPXSocketSSL> socket;
	socket.reset(new RLPXSocketSSL(m_ioService,_nodeIPEndpoint));

	m_tcpClient = socket->remoteEndpoint();
	socket->sslref().set_verify_mode(ba::ssl::verify_peer);
	socket->sslref().set_verify_depth(3);
	socket->sslref().set_verify_callback(boost::bind(&HostSSL::sslVerifyCert, this, _1, _2));

	socket->ref().async_connect(_nodeIPEndpoint, m_strand.wrap([ = ](boost::system::error_code const & ec)
	{
		if (ec)
		{
			LOG(WARNING) << "Connection refused to node" << id().abridged() <<  "@" << _nodeIPEndpoint.name() << "(" << ec.message() << ")";
			
			Guard l(x_pendingNodeConns);
			m_pendingPeerConns.erase(_nodeIPEndpoint.name());
		}
		else
		{
			socket->sslref().async_handshake(ba::ssl::stream_base::client, m_strand.wrap(boost::bind(&HostSSL::sslHandshakeClient, this, ba::placeholders::error, socket,NodeID(), _nodeIPEndpoint)) );
		}
	}));
}

PeerSessionInfos HostSSL::peerSessionInfo() const
{
	if (!m_run)
		return PeerSessionInfos();

	std::vector<PeerSessionInfo> ret;
	RecursiveGuard l(x_sessions);
	for (auto& i : m_sessions)
		if (auto j = i.second.lock())
			if (j->isConnected())
				ret.push_back(j->info());
	return ret;
}

void HostSSL::run(boost::system::error_code const&)
{
	if (!m_run)
	{
		m_ioService.stop();

		m_timer.reset();
		return;
	}

	DEV_GUARDED(x_connecting)
	m_connecting.remove_if([](std::weak_ptr<RLPXHandshakeSSL> h) { return h.expired(); });
	DEV_GUARDED(x_timers)
	m_timers.remove_if([](std::shared_ptr<boost::asio::deadline_timer> t)
	{
		return t->expires_from_now().total_milliseconds() < 0;
	});

	keepAlivePeers();
	reconnectAllNodes();

	auto runcb = [this](boost::system::error_code const & error) { run(error); };
	m_timer->expires_from_now(boost::posix_time::milliseconds(c_timerInterval));
	m_timer->async_wait(m_strand.wrap(runcb));  
}

void HostSSL::startedWorking()
{
	asserts(!m_timer);

	{
		
		Guard l(x_runTimer);
		m_timer.reset(new boost::asio::deadline_timer(m_ioService));
		m_run = true;
	}
	
	for (auto const& h : m_capabilities)
		h.second->onStarting();

	int port = Network::tcp4Listen(m_tcp4Acceptor, m_netPrefs);
	if (port > 0)
	{
		m_listenPort = port;
		determinePublic();
		runAcceptor();
	}
	else
	{
		LOG(ERROR) << "p2p.start.notice id:" << id().abridged() << "TCP Listen port is invalid or unavailable.";
		LOG(ERROR) << "P2pPort Bind Fail！" << "\n";
		exit(-1);
	}

	auto nodeTable = make_shared<NodeTable>(
	                     m_ioService,
	                     m_alias,
	                     NodeIPEndpoint(bi::address::from_string(listenAddress()), listenPort(), listenPort()),
	                     m_netPrefs.discovery
	                 );
	nodeTable->setEventHandler(new HostNodeTableHandler(*this));
	
	m_nodeTable = nodeTable;

	LOG(INFO) << "p2p.started id:" << id().abridged();
	run(boost::system::error_code());
}


void HostSSL::keepAlivePeers()
{
	auto now = chrono::steady_clock::now();

	if ( (now - c_keepAliveIntervalSSL < m_lastPing) && (!m_reconnectnow) )
		return;

	RecursiveGuard l(x_sessions);
	for (auto it = m_sessions.begin(); it != m_sessions.end();) {
		if (auto p = it->second.lock())
		{
			if( p->isConnected() )
			{
				if (now - c_keepAliveTimeOutSSL > m_lastPing && p->lastReceived() < m_lastPing)
				{
					LOG(WARNING) << "HostSSL::keepAlivePeers  timeout disconnect " << p->id().abridged();
					p->disconnect(PingTimeout);
				}
				else
					p->ping(); 

				++it;
			}
			else
			{
				if (m_peers.count(p->info().nodeIPEndpoint.name() ) )
					m_peers.erase(p->info().nodeIPEndpoint.name());
				LOG(WARNING) << "HostSSL::keepAlivePeers m_peers erase " << p->id().abridged() << "," << p->info().nodeIPEndpoint.name();
				it = m_sessions.erase(it);
			}
		}
		else {
			LOG(WARNING) << "HostSSL::keepAlivePeers erase Session " << it->first;
			it = m_sessions.erase(it);
			
		}
	}
	
	for (auto it = m_peers.begin(); it != m_peers.end();) 
	{
		if( !havePeerSession(it->second->id) )
		{
			LOG(WARNING) << "HostSSL::keepAlivePeers m_peers erase " << it->second->id.abridged() << "," << it->second->endpoint.name();
			it = m_peers.erase(it);
		}
		else
			++it;
	}

	m_lastPing = chrono::steady_clock::now();
}

void HostSSL::reconnectAllNodes()
{
	Guard rl(x_reconnectnow);
	if ( (chrono::steady_clock::now() - c_reconnectNodesIntervalSSL < m_lastReconnect) && (!m_reconnectnow) )
		return;

	
	std::map<std::string, NodeIPEndpoint> mConnectParams;
	NodeConnManagerSingleton::GetInstance().getAllConnect(mConnectParams);
	std::map<std::string, NodeIPEndpoint> mMergeConnectParams;//merge 
	

	RecursiveGuard l(x_sessions);
	for (auto stNode : mConnectParams)
	{
		bool hasPeer = false;
		for (auto const& p : m_peers)
		{
			if( !mMergeConnectParams.count(p.second->endpoint.name()))
				mMergeConnectParams[p.second->endpoint.name()] = p.second->endpoint;
			
			if( !p.second->endpoint.host.empty() )
				mMergeConnectParams[p.second->endpoint.name()].host = p.second->endpoint.host;
			
			//LOG(TRACE) << "HostSSL::reconnectAllNodes m_peers " << p.second->endpoint.name() << "," << p.second->endpoint.host;
			if (p.second->endpoint == stNode.second )
			{
				if( !havePeerSession(p.second->id) )
				{
					LOG(TRACE) << "HostSSL::reconnectAllNodes try to reconnect " << p.second->id.abridged() <<":" << stNode.second.name();
					connect(p.second->endpoint);
				}	
				hasPeer = true;
				break;
			}
		}
	
		if( !hasPeer && ( m_tcpPublic != stNode.second) && (NodeIPEndpoint(bi::address::from_string(m_netPrefs.listenIPAddress),listenPort(),listenPort()) != stNode.second) )
		{
			LOG(TRACE) << "HostSSL::reconnectAllNodes try to connect " << stNode.second.name();
			connect(stNode.second);
		}
		if( !mMergeConnectParams.count(stNode.second.name()))
			mMergeConnectParams[stNode.second.name()] = stNode.second;
		
	}//for

	NodeConnManagerSingleton::GetInstance().updateAllConnect(mMergeConnectParams);
	m_lastReconnect = chrono::steady_clock::now();
	m_reconnectnow = false;

	if ( (chrono::steady_clock::now() - c_AnnouncementConnectNodesIntervalSSL < m_lastAnnouncementConnectNodes)  )
		return;
	// broad to other nodes
	std::vector<Node>	peerNodes;
	h256 allPeerHash;
	getAnnouncementNodeList(allPeerHash,peerNodes);
	for (auto& i : m_sessions)
		if (auto j = i.second.lock())
			if (j->isConnected())
				j->announcement(allPeerHash);

	m_lastAnnouncementConnectNodes = chrono::steady_clock::now();

}

void HostSSL::getAnnouncementNodeList(h256& _allNodeHash,std::vector<Node> & _nodes)
{
	_nodes.clear();
	std::vector<Node>	peerNodes;

	RecursiveGuard l(x_sessions);
	for (auto const& p : m_peers)
	{
		peerNodes.push_back( Node(p.second->address(),p.second->endpoint) );
	}
	
	std::string allPeer;
	std::vector<Node> allNode;
	allNode=peerNodes;
	allNode.push_back( Node(id(),NodeIPEndpoint(m_tcpPublic.address(),m_listenPort,m_listenPort)) );
	std::sort(allNode.begin(), allNode.end(), [&](const Node & a, const Node &  b) {
		return a.endpoint.name() < b.endpoint.name();
	});
	for (auto const& n : allNode)
	{
		allPeer += n.endpoint.name();
	}

	_allNodeHash = sha3(allPeer);

	if( peerNodes.size() < c_maxAnnouncementSize )
	{
		_nodes = peerNodes;
	}
	else
	{
		for( size_t i=0;i<peerNodes.size();i++)
		{
			Node t = peerNodes[i];
			int r = rand()%peerNodes.size();
			peerNodes[i]  =peerNodes[r];
			peerNodes[r] = t;
		}
		for( size_t i = 0;i < c_maxAnnouncementSize; i++)
		{
			LOG(TRACE) << "HostSSL::getAnnouncementNodeList sendNode name=" << peerNodes[i].endpoint.name();
			_nodes.push_back(peerNodes[i]);

		}
	}
	LOG(TRACE) << "HostSSL::getAnnouncementNodeList " << toString(_allNodeHash) << ",Peers=" << _nodes.size();
}

void HostSSL::disconnectLatePeers()
{
	auto now = chrono::steady_clock::now();
	if (now - c_keepAliveTimeOutSSL < m_lastPing)
		return;

	RecursiveGuard l(x_sessions);
	for (auto p : m_sessions)
		if (auto pp = p.second.lock())
			if (now - c_keepAliveTimeOutSSL > m_lastPing && pp->lastReceived() < m_lastPing)
				pp->disconnect(PingTimeout);
}

void HostSSL::disconnectByNodeId(const std::string &sNodeId)
{
	if (id().hex() == sNodeId)
	{
		LOG(WARNING) << "disconnectByNodeId  self " << id().abridged() << "|" << sNodeId << "\n";

		return;
	}

	RecursiveGuard l(x_sessions);
	if (m_sessions.find(dev::jsToPublic(dev::toJS(sNodeId))) != m_sessions.end())
	{
		auto pp = m_sessions[jsToPublic(toJS(sNodeId))].lock();
		if (pp && pp->isConnected())
		{
			if (pp->isConnected())
			{
				pp->disconnect(UserReason);
			}
		}
	}
	else {
		LOG(WARNING) << "disconnectByNodeId  can not find " << sNodeId << "\n";
	}

}

void HostSSL::addConnParamsToNodeTable()
{
}
