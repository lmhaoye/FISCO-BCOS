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
/**
 * @file: ChannelRPCServer.cpp
 * @author: fisco-dev
 * 
 * @date: 2017
 */

#include "ChannelRPCServer.h"

#include <cstdlib>
#include <sys/socket.h>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/random.hpp>
#include <libethereum/EthereumHost.h>
#include <libdevcore/easylog.h>
#include <uuid/uuid.h>
#include "JsonHelper.h"
#include <libweb3jsonrpc/RPCallback.h>

using namespace dev;
using namespace dev::eth;

ChannelRPCServer::~ChannelRPCServer() {
	StopListening();
}

bool ChannelRPCServer::StartListening() {
	if (!_running)
	{
		LOG(INFO) << "启动ChannelRPCServer: " << _listenAddr << ":" << _listenPort;

		_ioService = std::make_shared<boost::asio::io_service>();
		
		if (dev::getSSL() == SSL_SOCKET_V2)
		{
#if ETH_ENCRYPTTYPE
			initContext();
#else
			initSSLContext();
#endif
		}
		else{
			initContext();
		}

		_server->run();

		LOG(INFO) << "ChannelRPCServer started";

		_running = true;
		_topicThread = std::make_shared<std::thread>([ = ]() {
			pthread_setThreadName("ChannelHeartBeat" );

			while (_running) {
				sleep(1);
				if(_running) {
				try {
						auto host = _host.lock();
						if(host) {
					_host.lock()->sendTopicsMessage(p2p::NodeID(), 0, _host.lock()->getTopicsSeq(), std::make_shared<std::set<std::string> >());
				}
					}
				catch (std::exception &e) {
						LOG(ERROR) << "发送topics错误:" << e.what();
					}
				}
			}
		});

		_topicThread->detach();

	}

	return true;
}

void ChannelRPCServer::initContext()
{
	string certData = asString( contents( getDataDir() + "/ca.crt") );
	if (certData == "")
	{
		LOG(ERROR)<<"Get ca.crt File Err......................";
		exit(-1);
	}
	certData = asString( contents( getDataDir() + "/server.crt") );
	if (certData == "")
	{
		LOG(ERROR)<<"Get server.crt File Err......................";
		exit(-1);
	}
	certData = asString( contents( getDataDir() + "/server.key") );
	if (certData == "")
	{
		LOG(ERROR)<<"Get server.key File Err......................";
		exit(-1);
	}
	_sslContext->load_verify_file(getDataDir() + "ca.crt");
	_sslContext->use_certificate_chain_file(getDataDir() + "server.crt");
	_sslContext->use_private_key_file(getDataDir() + "server.key", boost::asio::ssl::context_base::pem);

	_server = make_shared<dev::channel::ChannelServer>();
	_server->setIOService(_ioService);
	_server->setSSLContext(_sslContext);

	_server->setEnableSSL(true);
	_server->setBind(_listenAddr, _listenPort);

	std::function<void(dev::channel::ChannelException, dev::channel::ChannelSession::Ptr)> fp = std::bind(&ChannelRPCServer::onConnect, shared_from_this(), std::placeholders::_1, std::placeholders::_2);
	_server->setConnectionHandler(fp);
}
void ChannelRPCServer::initSSLContext()
{
	vector< pair<string,Public> >  certificates;
	string nodepri;
	CertificateServer::GetInstance().getCertificateList(certificates,nodepri);

	EC_KEY * ecdh=EC_KEY_new_by_curve_name(NID_X9_62_prime256v1);
	SSL_CTX_set_tmp_ecdh(_sslContext->native_handle(),ecdh);
	EC_KEY_free(ecdh);

	_sslContext->set_verify_depth(3);
	_sslContext->add_certificate_authority(boost::asio::const_buffer(certificates[0].first.c_str(), certificates[0].first.size()));
	
	string chain=certificates[0].first+certificates[1].first;
	_sslContext->use_certificate_chain(boost::asio::const_buffer(chain.c_str(), chain.size()));
	_sslContext->use_certificate(boost::asio::const_buffer(certificates[2].first.c_str(), certificates[2].first.size()),ba::ssl::context::file_format::pem);
	
	_sslContext->use_private_key(boost::asio::const_buffer(nodepri.c_str(), nodepri.size()),ba::ssl::context_base::pem);
	
	_server = make_shared<dev::channel::ChannelServer>();
	_server->setIOService(_ioService);
	_server->setSSLContext(_sslContext);

	_server->setEnableSSL(true);
	_server->setBind(_listenAddr, _listenPort);

	std::function<void(dev::channel::ChannelException, dev::channel::ChannelSession::Ptr)> fp = std::bind(&ChannelRPCServer::onConnect, shared_from_this(), std::placeholders::_1, std::placeholders::_2);
	_server->setConnectionHandler(fp);
}

bool ChannelRPCServer::StopListening()
{
	if (_running) {
		_ioService->stop();
	}

	_running = false;

	return true;
}

bool ChannelRPCServer::SendResponse(const std::string& _response, void* _addInfo) {
	std::string addInfo = *((std::string*)_addInfo);

	std::lock_guard<std::mutex> lock(_seqMutex);
	auto it = _seq2session.find(addInfo);

	delete (std::string*)_addInfo;

	if (it != _seq2session.end()) {
		LOG(INFO) << "发送ethereum响应 seq：" << it->first << " response:" << _response;

		std::shared_ptr<bytes> resp(new bytes());

		dev::channel::Message::Ptr message = make_shared<dev::channel::Message>();
		message->setSeq(it->first);
		message->setResult(0);
		message->setType(0x12);
		message->setData((const byte*)_response.data(), _response.size());

		it->second->asyncSendMessage(message, dev::channel::ChannelSession::CallbackType(), 0);

		_seq2session.erase(it);
	}
	else {
		LOG(ERROR) << "未找到来源seq，可能已超时:" << addInfo;
	}

	return false;
}

void dev::ChannelRPCServer::removeSession(int sessionID) {
	std::lock_guard<std::mutex> lock(_sessionMutex);
	auto it = _sessions.find(sessionID);

	if (it != _sessions.end()) {
		_sessions.erase(it);
	}
}

void ChannelRPCServer::onConnect(dev::channel::ChannelException e, dev::channel::ChannelSession::Ptr session) {
	if (e.errorCode() == 0) {
		LOG(INFO) << "channel收到新连接";

		auto sessionID = ++_sessionCount;
		{
			std::lock_guard<std::mutex> lock(_sessionMutex);
		_sessions.insert(std::make_pair(sessionID, session));
		}

		std::function<void(dev::channel::ChannelSession::Ptr, dev::channel::ChannelException, dev::channel::Message::Ptr)> fp =
		    std::bind(&dev::ChannelRPCServer::onClientRequest,
		              this, std::placeholders::_1,
		              std::placeholders::_2, std::placeholders::_3);
		session->setMessageHandler(fp);

		session->run();
		LOG(INFO) << "开始接收数据";
	}
	else {
		LOG(ERROR) << "连接错误: " << e.errorCode() << ", " << e.what();
	}
}

void ChannelRPCServer::onDisconnect(dev::channel::ChannelException e, dev::channel::ChannelSession::Ptr session) {
	LOG(ERROR) << "移除该session: " << session->host() << ":" << session->port() << " 成功";

	{
		std::lock_guard<std::mutex> lockSession(_sessionMutex);
		std::lock_guard<std::mutex> lockSeqMutex(_seqMutex);
		std::lock_guard<std::mutex> lockSeqMessageMutex(_seqMessageMutex);

	for (auto it : _sessions) {
		if (it.second == session) {
				auto c = _sessions.erase(it.first);
				LOG(DEBUG) << "已移除sessions: " << c;
			break;
			}
		}

		for(auto it: _seq2session) {
			if (it.second == session) {
				auto c = _seq2session.erase(it.first);
				LOG(DEBUG) << "已移除seq2session: " << c;
				break;
			}
		}

		for(auto it: _seq2MessageSession) {
			if(it.second.fromSession == session || it.second.toSession == session) {
				auto c = _seq2MessageSession.erase(it.first);
				LOG(DEBUG) << "已移除seq2MessageSession: " << c;
				break;
			}
		}
	}

	updateHostTopics();
}

void dev::ChannelRPCServer::onClientRequest(dev::channel::ChannelSession::Ptr session, dev::channel::ChannelException e, dev::channel::Message::Ptr message) {
	if (e.errorCode() == 0) {
		LOG(INFO) << "接收来自sdk消息 length:" << message->length() << " type:" << message->type() << " sessionID:" << message->seq();

		switch (message->type()) {
		case 0x20:
		case 0x21:
			onClientMessage(session, message);
			break;
		case 0x12:
			onClientEthereumRequest(session, message);
			break;
		case 0x13:
		{
			std::string data((char*)message->data(), message->dataSize());

			if (data == "0") {
				data = "1";
				message->setData((const byte*)data.data(), data.size());
				session->asyncSendMessage(message, dev::channel::ChannelSession::CallbackType(), 0);
			}
			else if (data == "1") {
			}
			break;
		}
		case 0x30:
		case 0x31:
			onClientChannelRequest(session, message);
			break;
		case 0x32:
			onClientTopicRequest(session, message);
			break;
		default:
			LOG(ERROR) << "未知客户端消息类型: " << message->type();
			break;
		}
	}
	else {
		LOG(ERROR) << "错误: " << e.errorCode() << ", " << e.what();

		onDisconnect(dev::channel::ChannelException(), session);
	}
}

void dev::ChannelRPCServer::onClientMessage(dev::channel::ChannelSession::Ptr session, dev::channel::Message::Ptr message) {
	LOG(DEBUG) << "收到来自sdk的链上链下消息";

	if (message->dataSize() < 128) {
		//长度不足
		LOG(ERROR) << "非法链上链下消息，长度过短:" << message->dataSize();
		return;
	}

	LOG(DEBUG) << "目的node:" << std::string((char*)message->data(), 128);

	h512 nodeID(std::string((char*)message->data(), 128), dev::h512::FromHex);

	int result = 0;
	try {
		std::lock_guard<std::mutex> lock(_seqMutex);
		auto it = _seq2session.insert(std::make_pair(message->seq(), session));
		if (it.second == false) {
			LOG(DEBUG) << "seq已存在";
			it.first->second = session;
		}

		auto buffer = std::make_shared<bytes>();
		message->encode(*buffer);

		LOG(DEBUG) << "打包消息到其他node:" << buffer->size();

		_host.lock()->sendCustomMessage(nodeID, buffer);
	}
	catch (exception &e) {
		LOG(ERROR) << "链上链下消息发往对端节点错误，返回100:" << e.what();
		result = REMOTE_PEER_UNAVAILIBLE;

		message->setType(0x21);
		message->setResult(result);
		message->clearData();

		session->asyncSendMessage(message, dev::channel::ChannelSession::CallbackType(), 0);
	}

	LOG(DEBUG) << "发送结果:" << result;
}

void dev::ChannelRPCServer::onClientEthereumRequest(dev::channel::ChannelSession::Ptr session, dev::channel::Message::Ptr message) {
	LOG(DEBUG) << "收到来自前置的区块链请求";

	

	std::string body(message->data(), message->data() + message->dataSize());

	LOG(DEBUG) << "seq:" << message->seq() << "  区块链请求:" << std::string((char*)message->data(), message->dataSize());

	{
		std::lock_guard<std::mutex> lock(_seqMutex);
		_seq2session.insert(std::make_pair(message->seq(), session));
	}

	std::string *addInfo = new std::string(message->seq());

	OnRequest(body, addInfo);
	RPCallback::getInstance().parseAndSaveSession(body, message->seq(), session);
}

void dev::ChannelRPCServer::onClientTopicRequest(dev::channel::ChannelSession::Ptr session, dev::channel::Message::Ptr message) {
	LOG(DEBUG) << "收到来自SDK的topic请求";

	std::string body(message->data(), message->data() + message->dataSize());

	LOG(DEBUG) << "seq:" << message->seq() << "  topic请求:" << body;

	try {
		std::stringstream ss;
		ss << body;

		Json::Value root;
		ss >> root;

		std::shared_ptr<std::set<std::string> > topics = std::make_shared<std::set<std::string> >();
		Json::Value topicsValue = root;
		if (!topicsValue.empty()) {
			for (size_t i = 0; i < topicsValue.size(); ++i) {
				std::string topic = topicsValue[(int)i].asString();

				LOG(DEBUG) << "topic:" << topic;

				topics->insert(topic);
			}
		}

		session->setTopics(topics);

		updateHostTopics();
	}
	catch (exception &e) {
		LOG(ERROR) << "解析请求错误:" << e.what();
	}
}

void dev::ChannelRPCServer::onClientChannelRequest(
    dev::channel::ChannelSession::Ptr session,
    dev::channel::Message::Ptr message) {
	LOG(DEBUG) << "收到来自SDK的链上链下2请求";

	if (message->dataSize() < 1) {
		//长度不足
		LOG(ERROR) << "非法链上链下2消息，长度过短:" << message->dataSize();
		return;
	}

	uint8_t topicLen = *((uint8_t*)message->data());
	std::string topic((char*)message->data() + 1, topicLen - 1);

	LOG(DEBUG) << "目的topic:" << topic;

	std::lock_guard<std::mutex> lock(_seqMessageMutex);
	auto it = _seq2MessageSession.find(message->seq());

	if (message->type() == 0x30) {
		try {
			LOG(DEBUG) << "链上链下2新请求:" << message->seq();

			ChannelMessageSession messageSession;
			messageSession.fromSession = session;
			messageSession.message = message;

			auto newIt = _seq2MessageSession.insert(std::make_pair(message->seq(), messageSession));
			if (newIt.second == false) {
				LOG(WARNING) << "seq:" << message->seq() << " session重复，覆盖";

				newIt.first->second.fromSession = session;
			}
			it = newIt.first;

			LOG(DEBUG) << "发送消息到其他node:" << it->second.message->seq();
			h512 nodeID = sendChannelMessageToNode(topic, it->second.message, it->second.failedNodeIDs);

			it->second.toNodeID = nodeID;
		}
		catch (exception &e) {
			LOG(ERROR) << "发送消息错误:" << e.what();

			message->setType(0x31);
			message->setResult(REMOTE_PEER_UNAVAILIBLE);
			message->clearData();

			it->second.fromSession->asyncSendMessage(message, dev::channel::ChannelSession::CallbackType(), 0);
		}
	}
	else if (message->type() == 0x31) {
		try {
			if (it == _seq2MessageSession.end()) {
				LOG(WARNING) << "未找到回包对应seq，已超时？";

				return;
			}

			if (message->result() != 0) {
				try {
					LOG(DEBUG) << "消息" << message->seq() << "push到 " << it->second.toSession->host() << ":" << it->second.toSession->port() << " 失败:" << message->result();
					it->second.failedSessions.insert(it->second.toSession);

					auto session = sendChannelMessageToSession(topic, message, it->second.failedSessions);

					LOG(DEBUG) << "尝试push到" << session->host() << ":" << session->port() << " 失败:" << message->result();
					it->second.toSession = session;
				}
				catch (exception &e) {
					LOG(ERROR) << "消息push完全失败:" << e.what();

					message->setResult(REMOTE_CLIENT_PEER_UNAVAILBLE);
					message->setType(0x31);//回包

					auto buffer = make_shared<bytes>();
					message->encode(*buffer);

					_host.lock()->sendCustomMessage(it->second.fromNodeID, buffer);
				}
			}
			else {
				LOG(DEBUG) << "来自SDK的链上链下2回包:" << message->seq();

				auto buffer = std::make_shared<bytes>();
				message->encode(*buffer);

				LOG(DEBUG) << "打包消息到node:" << it->second.fromNodeID;
				_host.lock()->sendCustomMessage(it->second.fromNodeID, buffer);

				_seq2MessageSession.erase(it);
			}
		}
		catch (exception &e) {
			LOG(ERROR) << "发送回包错误:" << e.what();
		}
	}
	else {
		LOG(ERROR) << "未知的消息类型:" << message->type();
	}
}

void dev::ChannelRPCServer::onNodeRequest(h512 nodeID, std::shared_ptr<dev::bytes> message) {
	auto msg = std::make_shared<dev::channel::Message>();
	ssize_t result = msg->decode(message->data(), message->size());

	if (result <= 0) {
		LOG(ERROR) << "解包错误:" << result << " 包大小:" << message->size();
		return;
	}

	LOG(DEBUG) << "接收来自node消息 length:" << message->size() << " type:" << msg->type() << " seq:" << msg->seq();

	switch (msg->type()) {
	case 0x20:
	case 0x21:
		onNodeMessage(nodeID, msg);
		break;
	case 0x30:
	case 0x31:
		onNodeChannelRequest(nodeID, msg);
		break;
	default:
		break;
	}
}

void dev::ChannelRPCServer::onNodeMessage(h512 nodeID, dev::channel::Message::Ptr message) {
	LOG(DEBUG) << "收到来自其它节点的链上链下消息 长度:" << message->dataSize() + 14;

	try {

	bool sended = false;

	std::lock_guard<std::mutex> lock(_seqMutex);
		auto it = _seq2session.find(message->seq());
	if (it != _seq2session.end()) {
			LOG(DEBUG) << "回包消息 seq:" << message->seq();

		if (it->second->actived()) {
			it->second->asyncSendMessage(message, dev::channel::ChannelSession::CallbackType(), 0);

				LOG(DEBUG) << "回包消息至seq[" << it->first << "] [" << it->second->host() << ":" << it->second->port() << "]成功";
			sended = true;
		}
	}

	if (!sended) {

			LOG(DEBUG) << "无seq，PUSH消息";

			std::lock_guard<std::mutex> lock(_sessionMutex);
		for (auto it : _sessions) {
			if (it.second->actived()) {
				it.second->asyncSendMessage(message, dev::channel::ChannelSession::CallbackType(), 0);

					LOG(DEBUG) << "push消息至session[" << it.first << "] [" << it.second->host() << ":" << it.second->port() << "]成功";
				sended = true;
				break;
			}
		}
	}

	if (!sended) {
			LOG(ERROR) << "下发消息失败，无sdk连接，返回101";

			if (message->result() == 0) {
				message->setResult(REMOTE_CLIENT_PEER_UNAVAILBLE);
				message->setType(0x21);
				message->clearData();

			auto buffer = make_shared<bytes>();
			message->encode(*buffer);

			_host.lock()->sendCustomMessage(nodeID, buffer);
		}
		}
	}
	catch(std::exception &e) {
		LOG(ERROR) << "错误:" << e.what();
	}
}

void ChannelRPCServer::onNodeChannelRequest(h512 nodeID, dev::channel::Message::Ptr message) {
	LOG(DEBUG) << "收到来自node:" << nodeID << " 的链上链下消息 长度:" << message->dataSize() + 14;

	try {
		//取出topic
		if (message->dataSize() < 1) {
			//长度不足
			LOG(ERROR) << "非法链上链下消息，长度过短:" << message->dataSize();
		return;
	}

		uint8_t topicLen = *((uint8_t*)message->data());
		std::string topic((char*)message->data() + 1, topicLen - 1);

		LOG(DEBUG) << "目的topic:" << topic;

	std::lock_guard<std::mutex> lock(_seqMessageMutex);
		auto it = _seq2MessageSession.find(message->seq());

		if (message->type() == 0x30) {
		try {
			if (it == _seq2MessageSession.end()) {
					LOG(DEBUG) << "新链上链下消息";

				ChannelMessageSession messageSession;
				messageSession.fromNodeID = nodeID;
				messageSession.message = message;

					auto newIt = _seq2MessageSession.insert(std::make_pair(message->seq(), messageSession));
				if (newIt.second == false) {
						LOG(WARNING) << "seq:" << message->seq() << " session重复，覆盖";

					newIt.first->second = messageSession;
				}

				it = newIt.first;
			}

			auto session = sendChannelMessageToSession(topic, message, it->second.failedSessions);

			it->second.toSession = session;
		}
			catch (std::exception &e) {
				LOG(ERROR) << "下发消息完全失败:" << e.what();

				message->setResult(REMOTE_CLIENT_PEER_UNAVAILBLE);
				message->setType(0x31);//回包

			auto buffer = make_shared<bytes>();
			message->encode(*buffer);

			_host.lock()->sendCustomMessage(nodeID, buffer);
		}
	}
		else if (message->type() == 0x31) {
		if (it == _seq2MessageSession.end()) {
				LOG(ERROR) << "错误，未找到该session:" << message->seq();
			return;
		}

			if (message->result() != 0) {
				LOG(DEBUG) << "消息:" << message->seq() << " 发送到node" << it->second.toNodeID << "失败:" << message->result();
			try {
				it->second.failedNodeIDs.insert(it->second.toNodeID);

				h512 nodeID = sendChannelMessageToNode(topic, it->second.message, it->second.failedNodeIDs);

					LOG(DEBUG) << "尝试发送至node:" << nodeID << " 成功";
				it->second.toNodeID = nodeID;
			}
				catch (std::exception &e) {
					LOG(ERROR) << "处理其它节点错误回包失败:" << e.what();

					message->setType(0x31);
					message->setResult(REMOTE_PEER_UNAVAILIBLE);
					message->clearData();

				it->second.fromSession->asyncSendMessage(message, dev::channel::ChannelSession::CallbackType(), 0);
			}
		}
		else {
				LOG(DEBUG) << "回包消息 seq:" << message->seq();

			if (it->second.fromSession->actived()) {
				it->second.fromSession->asyncSendMessage(message, dev::channel::ChannelSession::CallbackType(), 0);

					LOG(DEBUG) << "回包消息至seq[" << it->first << "] [" << it->second.fromSession->host()
							   << ":" << it->second.fromSession->port() << "]成功";
			}

			_seq2MessageSession.erase(it);
		}
		}
	}
	catch(std::exception &e) {
		LOG(ERROR) << "错误:" << e.what();
	}
}

void ChannelRPCServer::setListenAddr(const std::string& listenAddr) {
	_listenAddr = listenAddr;
}

void ChannelRPCServer::setListenPort(int listenPort) {
	_listenPort = listenPort;
}

void ChannelRPCServer::CloseConnection(int _socket)
{
	close(_socket);
}

dev::eth::Web3Observer::Ptr ChannelRPCServer::buildObserver() {
	class Web3ObserverImpl: public dev::eth::Web3Observer {
	public:
		Web3ObserverImpl(ChannelRPCServer::Ptr server): _server(server) {};
		virtual ~Web3ObserverImpl() {}

		virtual void onReceiveChannelMessage(const h512 nodeID, std::shared_ptr<bytes> buffer) {
			_server->onNodeRequest(nodeID, buffer);
		}

	private:
		ChannelRPCServer::Ptr _server;
	};

	return std::make_shared<Web3ObserverImpl>(shared_from_this());
}

void ChannelRPCServer::setHost(std::weak_ptr<dev::eth::EthereumHost> host) {
	_host = host;
}

void ChannelRPCServer::setSSLContext(std::shared_ptr<boost::asio::ssl::context> sslContext) {
	_sslContext = sslContext;
}

void ChannelRPCServer::asyncPushChannelMessage(std::string topic, dev::channel::Message::Ptr message,
		std::function<void(dev::channel::ChannelException, dev::channel::Message::Ptr)> callback) {
	try {
		class Callback: public std::enable_shared_from_this<Callback> {
		public:
			typedef std::shared_ptr<Callback> Ptr;

			Callback(std::string topic,
					dev::channel::Message::Ptr message,
					ChannelRPCServer::Ptr server,
					std::function<void(dev::channel::ChannelException, dev::channel::Message::Ptr)> callback):
						_topic(topic), _message(message), _server(server), _callback(callback) {};

			void onResponse(dev::channel::ChannelException e, dev::channel::Message::Ptr message) {
				try {
					//处理消息
					if(e.errorCode() != 0) {
						LOG(ERROR) << "收到错误:" << e.errorCode() << " message:" << e.what();

						_exclude.insert(_currentSession);

						sendMessage();

						return;
					}
				}
				catch(dev::channel::ChannelException &e) {
					LOG(ERROR) << "发送消息错误:" << e.errorCode() << " " << e.what();

					try {
						_callback(e, dev::channel::Message::Ptr());
					}
					catch(exception &e) {
						LOG(ERROR) << "错误" << e.what();
					}
				}

				try {
					_callback(e, message);
				}
				catch(exception &e) {
					LOG(ERROR) << "错误" << e.what();
				}
			}

			void sendMessage() {
				//选取关注该topic的session
				std::vector<dev::channel::ChannelSession::Ptr> activedSessions = _server->getSessionByTopic(_topic);

				if (activedSessions.empty()) {
					LOG(ERROR)<< "无session使用该topic:" << _topic;
					throw dev::channel::ChannelException(104, "无session使用该topic:" + _topic);
				}

				//清除已失败的session
				for (auto sessionIt = activedSessions.begin(); sessionIt != activedSessions.end();) {
					if (_exclude.find(*sessionIt) != _exclude.end()) {
						sessionIt = activedSessions.erase(sessionIt);
					} else {
						++sessionIt;
					}
				}

				if(activedSessions.empty()) {
					LOG(ERROR) << "所有session已重试失败";

					throw dev::channel::ChannelException(104, "所有session已重试失败");
				}

				boost::mt19937 rng(static_cast<unsigned>(std::time(0)));
				boost::uniform_int<int> index(0, activedSessions.size() - 1);

				auto ri = index(rng);
				LOG(DEBUG)<< "随机节点:" << ri;

				auto session = activedSessions[ri];

				std::function<void(dev::channel::ChannelException, dev::channel::Message::Ptr)> fp = std::bind(&Callback::onResponse, shared_from_this(),
										std::placeholders::_1, std::placeholders::_2);
				session->asyncSendMessage(_message,	fp, 5000);

				LOG(INFO)<< "push消息至session: " << session->host() << ":" << session->port() << " 成功";
				_currentSession = session;
			}

		private:
			std::string _topic;
			dev::channel::Message::Ptr _message;
			ChannelRPCServer::Ptr _server;
			dev::channel::ChannelSession::Ptr _currentSession;
			std::set<dev::channel::ChannelSession::Ptr> _exclude;
			std::function<void(dev::channel::ChannelException, dev::channel::Message::Ptr)> _callback;
		};

		Callback::Ptr pushCallback = std::make_shared<Callback>(topic, message, shared_from_this(), callback);
		pushCallback->sendMessage();
	}
	catch(exception &e) {
		LOG(ERROR) << "错误:" << e.what();
	}
}

dev::channel::TopicMessage::Ptr ChannelRPCServer::pushChannelMessage(dev::channel::TopicMessage::Ptr message) {
	try {
		std::string topic = message->topic();

		LOG(DEBUG) << "向SDK推送消息:" << message->seq();
		std::vector<dev::channel::ChannelSession::Ptr> activedSessions = getSessionByTopic(topic);

		if(activedSessions.empty()) {
			LOG(ERROR) << "无SDK关注该topic:" << topic;

			throw dev::channel::ChannelException(103, "发送失败，没有节点关注该topic:" + topic);
		}

		dev::channel::TopicMessage::Ptr response;
		for(auto it: activedSessions) {
			dev::channel::Message::Ptr responseMessage = it->sendMessage(message, 0);

			if(responseMessage.get() != NULL && responseMessage->result() == 0) {
				response = std::make_shared<TopicMessage>(responseMessage.get());
				break;
			}
		}

		if(!response) {
			throw dev::channel::ChannelException(99, "发送失败，所有重试均失败");
		}

		return response;

#if 0
		struct Callback {
			typedef std::shared_ptr<Callback> Ptr;

			Callback() {
				_mutex.lock();
			}

			void onResponse(dev::channel::ChannelException error, dev::channel::Message::Ptr message) {
				_error = error;
				_response = message;

				_mutex.unlock();
			}

			dev::channel::ChannelException _error;
			dev::channel::Message::Ptr _response;
			std::mutex _mutex;
		};

		Callback::Ptr callback = std::make_shared<Callback>();

		std::function<void(dev::channel::ChannelException, dev::channel::Message::Ptr)> fp = std::bind(&Callback::onResponse, callback,
						std::placeholders::_1, std::placeholders::_2);
		asyncPushChannelMessage(topic, message, fp);

		callback->_mutex.lock();
		callback->_mutex.unlock();

		if (callback->_error.errorCode() != 0) {
			throw callback->_error;
		}

		return callback->_response;
#endif
	} catch (exception &e) {
		LOG(ERROR)<< "错误:" << e.what();

		throw e;
	}

	return dev::channel::TopicMessage::Ptr();
}

std::string ChannelRPCServer::newSeq() {
	uuid_t uuid;
	uuid_generate(uuid);

	return toHex(uuid);
}

h512 ChannelRPCServer::sendChannelMessageToNode(std::string topic, dev::channel::Message::Ptr message, const std::set<h512> &exclude) {
	try {
	std::vector<p2p::NodeID> peers = _host.lock()->getPeersByTopic(topic);

	for (auto it = peers.begin(); it != peers.end();) {
		if (exclude.find(*it) != exclude.end()) {
			it = peers.erase(it);
		}
		else {
			++it;
		}
	}

	if (peers.empty()) {
		LOG(ERROR) << "发送失败，没有节点关注该topic:" << topic;

		throw dev::channel::ChannelException(103, "发送失败，没有节点关注该topic:" + topic);
	}

	boost::mt19937 rng(static_cast<unsigned>(std::time(0)));
	boost::uniform_int<int> index(0, peers.size() - 1);

	auto ri = index(rng);
	LOG(DEBUG) << "随机数:" << ri;
	p2p::NodeID targetNodeID = peers[ri];

	auto buffer = std::make_shared<bytes>();
	message->encode(*buffer);

	_host.lock()->sendCustomMessage(targetNodeID, buffer);

	LOG(DEBUG) << "消息发送至" << targetNodeID;

	return targetNodeID;
}
	catch(exception &e) {
		LOG(ERROR) << "错误:" << e.what();

		return h512();
	}
}

dev::channel::ChannelSession::Ptr ChannelRPCServer::sendChannelMessageToSession(std::string topic, dev::channel::Message::Ptr message, const std::set<dev::channel::ChannelSession::Ptr> &exclude) {
	std::vector<dev::channel::ChannelSession::Ptr> activedSessions =
	    getSessionByTopic(topic);

	for (auto sessionIt = activedSessions.begin();
	        sessionIt != activedSessions.end();) {
		if (exclude.find(*sessionIt)
		        != exclude.end()) {
			sessionIt = activedSessions.erase(sessionIt);
		} else {
			++sessionIt;
		}
	}

	if (activedSessions.empty()) {
		LOG(ERROR) << "无session使用该topic:" << topic;
		throw dev::channel::ChannelException(104, "无session使用该topic:" + topic);
	}

	boost::mt19937 rng(static_cast<unsigned>(std::time(0)));
	boost::uniform_int<int> index(0, activedSessions.size() - 1);

	auto ri = index(rng);
	LOG(DEBUG) << "随机节点:" << ri;

	auto session = activedSessions[ri];

	session->asyncSendMessage(message,
	                          dev::channel::ChannelSession::CallbackType(), 0);

	LOG(DEBUG) << "push消息至session: " << session->host() << ":" << session->port() << " 成功";

	return session;
}

void ChannelRPCServer::updateHostTopics() {
	std::shared_ptr<std::set<std::string> > allTopics = std::make_shared<std::set<std::string> >();

	std::lock_guard<std::mutex> lock(_sessionMutex);
	for (auto it : _sessions) {
		auto topics = it.second->topics();
		allTopics->insert(topics->begin(), topics->end());
	}

	_host.lock()->setTopics(allTopics);
}

std::vector<dev::channel::ChannelSession::Ptr> ChannelRPCServer::getSessionByTopic(const std::string &topic) {
	std::vector<dev::channel::ChannelSession::Ptr> activedSessions;

	std::lock_guard<std::mutex> lock(_sessionMutex);
	for (auto it : _sessions) {
		if (it.second->topics()->empty() || !it.second->actived()) {
			continue;
		}

		auto topicIt = it.second->topics()->find(topic);
		if (topicIt != it.second->topics()->end()) {
			activedSessions.push_back(it.second);
		}
	}

	return activedSessions;
}

