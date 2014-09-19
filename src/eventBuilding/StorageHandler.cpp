/*
 * StorageHandler.cpp
 *
 *  Created on: Mar 4, 2014
 \*      Author: Jonas Kunze (kunze.jonas@gmail.com)
 */

#include "StorageHandler.h"

#include <asm-generic/errno-base.h>
#include <eventBuilding/Event.h>
#include <eventBuilding/SourceIDManager.h>
#include <tbb/spin_mutex.h>
#include <sstream>

#ifdef USE_GLOG
#include <glog/logging.h>
#endif
#include <l0/MEPFragment.h>
#include <l0/Subevent.h>
#include <LKr/LkrFragment.h>
#include <structs/Event.h>
#include <zmq.h>
#include <zmq.hpp>
#include <cstdbool>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>
#include <thread>
#include <socket/ZMQHandler.h>
#include <glog/logging.h>

#include "../options/MyOptions.h"

namespace na62 {

zmq::socket_t* StorageHandler::MergerSocket_;

std::atomic<uint> StorageHandler::InitialEventBufferSize_;
int StorageHandler::TotalNumberOfDetectors_;

tbb::spin_mutex StorageHandler::sendMutex_;

void freeZmqMessage(void *data, void *hint) {
	delete[] ((char*) data);
}

std::string StorageHandler::GetMergerAddress() {
	std::stringstream address;
	address << "tcp://" << Options::GetString(OPTION_MERGER_HOST_NAME) << ":"
			<< Options::GetInt(OPTION_MERGER_PORT);
	return address.str();
}

void StorageHandler::Initialize() {
	LOG(INFO)<< "Connecting to merger: " << GetMergerAddress().c_str();
	MergerSocket_ = ZMQHandler::GenerateSocket(ZMQ_PUSH);
	MergerSocket_->connect(GetMergerAddress().c_str());

	/*
	 * L0 sources + LKr
	 */
	if (SourceIDManager::NUMBER_OF_EXPECTED_CREAM_PACKETS_PER_EVENT == 0) {
		TotalNumberOfDetectors_ = SourceIDManager::NUMBER_OF_L0_DATA_SOURCES;
	} else {
		TotalNumberOfDetectors_ = SourceIDManager::NUMBER_OF_L0_DATA_SOURCES
		+ 1;
	}
	InitialEventBufferSize_ = 1000;
}

void StorageHandler::OnShutDown() {
	ZMQHandler::DestroySocket(MergerSocket_);
}

char* StorageHandler::ResizeBuffer(char* buffer, const int oldLength,
		const int newLength) {
	char* newBuffer = new char[newLength];
	memcpy(newBuffer, buffer, oldLength);
	delete[] buffer;
	return newBuffer;
}

EVENT_HDR* StorageHandler::GenerateEventBuffer(const Event* event) {

	uint eventBufferSize = InitialEventBufferSize_;
	char* eventBuffer = new char[InitialEventBufferSize_];

	struct EVENT_HDR* header = (struct EVENT_HDR*) eventBuffer;

	header->eventNum = event->getEventNumber();
	header->format = 0x62; // TODO: update current format
	// header->length will be written later on
	header->burstID = event->getBurstID();
	header->timestamp = event->getTimestamp();
	header->triggerWord = event->getTriggerTypeWord();
	header->reserved1 = 0;
	header->fineTime = event->getFinetime();
	header->numberOfDetectors = TotalNumberOfDetectors_;
	header->reserved2 = 0;
	header->processingID = event->getProcessingID();
	header->SOBtimestamp = 0; // Will be set by the merger

	uint32_t sizeOfPointerTable = 4 * TotalNumberOfDetectors_;
	uint32_t pointerTableOffset = sizeof(struct EVENT_HDR);
	uint32_t eventOffset = sizeof(struct EVENT_HDR) + sizeOfPointerTable;

	for (int sourceNum = SourceIDManager::NUMBER_OF_L0_DATA_SOURCES - 1;
			sourceNum >= 0; sourceNum--) {
		l0::Subevent* subevent = event->getL0SubeventBySourceIDNum(sourceNum);

		if (eventOffset + 4 > eventBufferSize) {
			eventBuffer = ResizeBuffer(eventBuffer, eventBufferSize,
					eventBufferSize + 1000);
			eventBufferSize += 1000;
			header = (struct EVENT_HDR*) eventBuffer;
		}

		/*
		 * Put the sub-detector  into the pointer table
		 */
		uint32_t eventOffset32 = eventOffset / 4;
		std::memcpy(eventBuffer + pointerTableOffset, &eventOffset32, 3);
		std::memset(eventBuffer + pointerTableOffset + 3,
				SourceIDManager::SourceNumToID(sourceNum), 1);
		pointerTableOffset += 4;

		/*
		 * Write the L0 data
		 */
		int payloadLength;
		for (int i = subevent->getNumberOfFragments() - 1; i >= 0; i--) {
			l0::MEPFragment* e = subevent->getFragment(i);
			payloadLength = e->getDataLength()
					- sizeof(struct l0::MEPFragment_HDR)
					+ sizeof(struct L0_BLOCK_HDR);
			if (eventOffset + payloadLength > eventBufferSize) {
				eventBuffer = ResizeBuffer(eventBuffer, eventBufferSize,
						eventBufferSize + payloadLength);
				eventBufferSize += payloadLength;
				header = (struct EVENT_HDR*) eventBuffer;
			}

			struct L0_BLOCK_HDR* blockHdr = (struct L0_BLOCK_HDR*) (eventBuffer
					+ eventOffset);
			blockHdr->dataBlockSize = payloadLength;
			blockHdr->sourceSubID = e->getSourceSubID();
			blockHdr->reserved = 0;

			memcpy(eventBuffer + eventOffset + sizeof(struct L0_BLOCK_HDR),
					e->getData(), payloadLength - sizeof(struct L0_BLOCK_HDR));
			eventOffset += payloadLength;

			/*
			 * 32-bit alignment
			 */
			if (eventOffset % 4 != 0) {
				memset(eventBuffer + eventOffset, 0, eventOffset % 4);
				eventOffset += eventOffset % 4;
			}
		}
	}

	/*
	 * Write the LKr data
	 */
	if (eventOffset + 4 > eventBufferSize) {
		eventBuffer = ResizeBuffer(eventBuffer, eventBufferSize,
				eventBufferSize + 1000);
		eventBufferSize += 1000;
		header = (struct EVENT_HDR*) eventBuffer;
	}

	if (SourceIDManager::NUMBER_OF_EXPECTED_CREAM_PACKETS_PER_EVENT > 0) {
		uint32_t eventOffset32 = eventOffset / 4;
		/*
		 * Put the LKr into the pointer table
		 */
		std::memcpy(eventBuffer + pointerTableOffset, &eventOffset32, 3);
		std::memset(eventBuffer + pointerTableOffset + 3, SOURCE_ID_LKr, 1); // 0x24 is the LKr sourceID

		for (int localCreamID = event->getNumberOfZSuppressedLkrFragments() - 1;
				localCreamID >= 0; localCreamID--) {
			cream::LkrFragment* e = event->getZSuppressedLkrFragment(
					localCreamID);

			if (eventOffset + e->getEventLength() > eventBufferSize) {
				eventBuffer = ResizeBuffer(eventBuffer, eventBufferSize,
						eventBufferSize + e->getEventLength());
				eventBufferSize += e->getEventLength();
			}

			memcpy(eventBuffer + eventOffset, e->getDataWithHeader(),
					e->getEventLength());
			eventOffset += e->getEventLength();

			/*
			 * 32-bit alignment
			 */
			if (eventOffset % 4 != 0) {
				memset(eventBuffer + eventOffset, 0, eventOffset % 4);
				eventOffset += eventOffset % 4;
			}
		}
	}

	/*
	 * Trailer
	 */
	EVENT_TRAILER* trailer = (EVENT_TRAILER*) (eventBuffer + eventOffset);
	trailer->eventNum = event->getEventNumber();
	trailer->reserved = 0;

	const int eventLength = eventOffset + 4/*trailer*/;

	if (eventBufferSize > InitialEventBufferSize_) {
		InitialEventBufferSize_ = eventBufferSize;
	}

	header->length = eventLength / 4;

	return header;
}

int StorageHandler::SendEvent(const Event* event) {
	/*
	 * TODO: Use multimessage instead of creating a separate buffer and copying the MEP data into it
	 */
	const EVENT_HDR* data = GenerateEventBuffer(event);

	/*
	 * Send the event to the merger with a zero copy message
	 */
	zmq::message_t zmqMessage((void*) data, data->length * 4,
			(zmq::free_fn*) freeZmqMessage);

	while (ZMQHandler::IsRunning()) {
		tbb::spin_mutex::scoped_lock my_lock(sendMutex_);
		try {
			MergerSocket_->send(zmqMessage);
			break;
		} catch (const zmq::error_t& ex) {
			if (ex.num() != EINTR) { // try again if EINTR (signal caught)
				LOG(ERROR)<< ex.what();

				ZMQHandler::DestroySocket(MergerSocket_);
				return 0;
			}
		}
	}

	return data->length * 4;
}
} /* namespace na62 */
