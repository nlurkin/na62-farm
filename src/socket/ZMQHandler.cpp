/*
 * ZMQHandler.cpp
 *
 *  Created on: Feb 21, 2014
 *      Author: Jonas Kunze (kunzej@cern.ch)
 */

#include "ZMQHandler.h"

#include <boost/date_time/posix_time/posix_time_duration.hpp>
#include <boost/date_time/time_duration.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/pthread/thread_data.hpp>
#include <glog/logging.h>
#include <zmq.h>
#include <zmq.hpp>
#include <iostream>
#include <map>

#include "../options/MyOptions.h"

namespace na62 {

zmq::context_t* ZMQHandler::context_;
std::set<std::string> ZMQHandler::boundAddresses_;
boost::mutex ZMQHandler::connectMutex_;

void ZMQHandler::Initialize(const int numberOfIOThreads) {
	context_ = new zmq::context_t(numberOfIOThreads);
}

void ZMQHandler::Destroy() {
	delete context_;
}

zmq::socket_t* ZMQHandler::GenerateSocket(int socketType, int highWaterMark) {
	int linger = 0;
	zmq::socket_t* socket = new zmq::socket_t(*context_, socketType);
	socket->setsockopt(ZMQ_LINGER, &linger, sizeof(linger));

	socket->setsockopt(ZMQ_SNDHWM, &highWaterMark, sizeof(highWaterMark));

	return socket;
}

std::string ZMQHandler::GetEBL0Address(int threadNum) {
	std::stringstream address;
	address << "inproc://EB/" << threadNum << "/L0";
	return address.str();
}

std::string ZMQHandler::GetEBLKrAddress(int threadNum) {
	std::stringstream address;
	address << "inproc://EB/" << threadNum << "/LKr";
	return address.str();
}

std::string ZMQHandler::GetMergerAddress() {
	std::stringstream address;
	address << "tcp://" << Options::GetString(OPTION_MERGER_HOST_NAME) << ":"
			<< Options::GetInt(OPTION_MERGER_PORT);
	return address.str();
}

void ZMQHandler::BindInproc(zmq::socket_t* socket, std::string address) {
	socket->bind(address.c_str());

	boost::lock_guard<boost::mutex> lock(connectMutex_);
	boundAddresses_.insert(address);
}

void ZMQHandler::ConnectInproc(zmq::socket_t* socket, std::string address) {
	connectMutex_.lock();
	while (boundAddresses_.find(address) == boundAddresses_.end()) {
		connectMutex_.unlock();
		LOG(INFO)<< "ZMQ not yet bound: " << address;
		boost::this_thread::sleep(boost::posix_time::microsec(500000));
		connectMutex_.lock();
	}
	socket->connect(address.c_str());
	connectMutex_.unlock();
}

} /* namespace na62 */
