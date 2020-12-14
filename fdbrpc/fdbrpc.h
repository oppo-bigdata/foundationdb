/*
 * fdbrpc.h
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef FDBRPC_FDBRPC_H
#define FDBRPC_FDBRPC_H
#pragma once

#include "flow/flow.h"
#include "flow/serialize.h"
#include "fdbrpc/FlowTransport.h" // NetworkMessageReceiver Endpoint
#include "fdbrpc/FailureMonitor.h"
#include "fdbrpc/networksender.actor.h"

struct FlowReceiver : public NetworkMessageReceiver {
	// Common endpoint code for NetSAV<> and NetNotifiedQueue<>

	FlowReceiver() : m_isLocalEndpoint(false), m_stream(false) {
	}

	FlowReceiver(Endpoint const& remoteEndpoint, bool stream)
	  : endpoint(remoteEndpoint), m_isLocalEndpoint(false), m_stream(stream) {
		FlowTransport::transport().addPeerReference(endpoint, m_stream);
	}

	~FlowReceiver() {
		if (m_isLocalEndpoint) {
			FlowTransport::transport().removeEndpoint(endpoint, this);
		} else {
			FlowTransport::transport().removePeerReference(endpoint, m_stream);
		}
	}

	bool isLocalEndpoint() { return m_isLocalEndpoint; }
	bool isRemoteEndpoint() { return endpoint.isValid() && !m_isLocalEndpoint; }

	// If already a remote endpoint, returns that.  Otherwise makes this
	//   a local endpoint and returns that.
	const Endpoint& getEndpoint(TaskPriority taskID) {
		if (!endpoint.isValid()) {
			m_isLocalEndpoint = true;
			FlowTransport::transport().addEndpoint(endpoint, this, taskID);
		}
		return endpoint;
	}

	void setEndpoint(Endpoint const& e) {
		ASSERT(!endpoint.isValid());
		m_isLocalEndpoint = true;
		endpoint = e;
	}

	void makeWellKnownEndpoint(Endpoint::Token token, TaskPriority taskID) {
		ASSERT(!endpoint.isValid());
		m_isLocalEndpoint = true;
		endpoint.token = token;
		FlowTransport::transport().addWellKnownEndpoint(endpoint, this, taskID);
	}

	const Endpoint& getRawEndpoint() {
		return endpoint;
	}

private:
	Endpoint endpoint;
	bool m_isLocalEndpoint;
	bool m_stream;
};

template <class T>
struct NetSAV final : SAV<T>, FlowReceiver, FastAllocated<NetSAV<T>> {
	using FastAllocated<NetSAV<T>>::operator new;
	using FastAllocated<NetSAV<T>>::operator delete;

	NetSAV(int futures, int promises) : SAV<T>(futures, promises) {}
	NetSAV(int futures, int promises, const Endpoint& remoteEndpoint)
	  : SAV<T>(futures, promises), FlowReceiver(remoteEndpoint, false) {
	}

	void destroy() override { delete this; }
	void receive(ArenaObjectReader& reader) override {
		if (!SAV<T>::canBeSet()) return;
		this->addPromiseRef();
		ErrorOr<EnsureTable<T>> message;
		reader.deserialize(message);
		if (message.isError()) {
			if(message.getError().code() == error_code_broken_promise) {
				IFailureMonitor::failureMonitor().endpointNotFound( getRawEndpoint() );
			}
			SAV<T>::sendErrorAndDelPromiseRef(message.getError());
		} else {
			SAV<T>::sendAndDelPromiseRef(message.get().asUnderlyingType());
		}
	}
};

template <class T>
class ReplyPromise sealed : public ComposedIdentifier<T, 1> {
public:
	template <class U>
	void send(U&& value) const {
		sav->send(std::forward<U>(value));
	}
	template <class E>
	void sendError(const E& exc) const { sav->sendError(exc); }

	Future<T> getFuture() const { sav->addFutureRef(); return Future<T>(sav); }
	bool isSet() { return sav->isSet(); }
	bool isValid() const { return sav != nullptr; }
	ReplyPromise() : sav(new NetSAV<T>(0, 1)) {}
	ReplyPromise(const ReplyPromise& rhs) : sav(rhs.sav) { sav->addPromiseRef(); }
	ReplyPromise(ReplyPromise&& rhs) noexcept : sav(rhs.sav) { rhs.sav = 0; }
	~ReplyPromise() { if (sav) sav->delPromiseRef(); }

	ReplyPromise(const Endpoint& endpoint) : sav(new NetSAV<T>(0, 1, endpoint)) {}
	const Endpoint& getEndpoint(TaskPriority taskID = TaskPriority::DefaultPromiseEndpoint) const { return sav->getEndpoint(taskID); }

	void operator=(const ReplyPromise& rhs) {
		if (rhs.sav) rhs.sav->addPromiseRef();
		if (sav) sav->delPromiseRef();
		sav = rhs.sav;
	}
	void operator=(ReplyPromise&& rhs) noexcept {
		if (sav != rhs.sav) {
			if (sav) sav->delPromiseRef();
			sav = rhs.sav;
			rhs.sav = 0;
		}
	}
	void reset() {
		*this = ReplyPromise<T>();
	}
	void swap(ReplyPromise& other) {
		std::swap(sav, other.sav);
	}

	// Beware, these operations are very unsafe
	SAV<T>* extractRawPointer() { auto ptr = sav; sav = nullptr; return ptr; }
	explicit ReplyPromise<T>(SAV<T>* ptr) : sav(ptr) {}

	int getFutureReferenceCount() const { return sav->getFutureReferenceCount(); }
	int getPromiseReferenceCount() const { return sav->getPromiseReferenceCount(); }

private:
	NetSAV<T> *sav;
};

template <class Ar, class T>
void save(Ar& ar, const ReplyPromise<T>& value) {
	auto const& ep = value.getEndpoint().token;
	ar << ep;
}

template <class Ar, class T>
void load(Ar& ar, ReplyPromise<T>& value) {
	UID token;
	ar >> token;
	Endpoint endpoint = FlowTransport::transport().loadedEndpoint(token);
	value = ReplyPromise<T>(endpoint);
	networkSender(value.getFuture(), endpoint);
}

template <class T>
struct serializable_traits<ReplyPromise<T>> : std::true_type {
	template<class Archiver>
	static void serialize(Archiver& ar, ReplyPromise<T>& p) {
		if constexpr (Archiver::isDeserializing) {
			UID token;
			serializer(ar, token);
			auto endpoint = FlowTransport::transport().loadedEndpoint(token);
			p = ReplyPromise<T>(endpoint);
			networkSender(p.getFuture(), endpoint);
		} else {
			const auto& ep = p.getEndpoint().token;
			serializer(ar, ep);
		}
	}
};

template <class Reply>
ReplyPromise<Reply> const& getReplyPromise(ReplyPromise<Reply> const& p) { return p; }

template <class Request>
void resetReply(Request& r) { r.reply.reset(); }

template <class Reply>
void resetReply(ReplyPromise<Reply> & p) { p.reset(); }

template <class Request>
void resetReply(Request& r, TaskPriority taskID) { r.reply.reset(); r.reply.getEndpoint(taskID); }

template <class Reply>
void resetReply(ReplyPromise<Reply> & p, TaskPriority taskID) { p.reset(); p.getEndpoint(taskID); }

template <class Request>
void setReplyPriority(Request& r, TaskPriority taskID) { r.reply.getEndpoint(taskID); }

template <class Reply>
void setReplyPriority(ReplyPromise<Reply> & p, TaskPriority taskID) { p.getEndpoint(taskID); }

template <class Reply>
void setReplyPriority(const ReplyPromise<Reply> & p, TaskPriority taskID) { p.getEndpoint(taskID); }

struct ReplyPromiseStreamReply {
	Optional<Endpoint> acknowledgeEndpoint;
	ReplyPromiseStreamReply() {}
};

struct AcknowledgementReply {
	constexpr static FileIdentifier file_identifier = 1378929;
	int64_t bytes;

	AcknowledgementReply() : bytes(0) {}
	explicit AcknowledgementReply(int64_t bytes) : bytes(bytes) {}

	template <class Ar>
	void serialize( Ar& ar ) {
		serializer(ar, bytes);
	}
};

struct AcknowledgementReceiver final : FlowReceiver, FastAllocated<AcknowledgementReceiver> {
	using FastAllocated<AcknowledgementReceiver>::operator new;
	using FastAllocated<AcknowledgementReceiver>::operator delete;

	int64_t bytesSent;
	int64_t bytesAcknowledged;
	Promise<Void> ready;

	AcknowledgementReceiver() : bytesSent(0), bytesAcknowledged(0), ready(nullptr) {}
	AcknowledgementReceiver(const Endpoint& remoteEndpoint) : FlowReceiver(remoteEndpoint, false),
		bytesSent(0), bytesAcknowledged(0), ready(nullptr) {}

	void receive(ArenaObjectReader& reader) override {
		ErrorOr<EnsureTable<AcknowledgementReply>> message;
		reader.deserialize(message);
		ASSERT(!message.isError() && message.get().asUnderlyingType().bytes > bytesAcknowledged);
		bytesAcknowledged = message.get().asUnderlyingType().bytes;
		if(bytesSent - bytesAcknowledged < 2e6) {
			ready.send(Void());
		}
	}
};

template <class T>
struct NetNotifiedQueueWithErrors final : NotifiedQueue<T>, FlowReceiver, FastAllocated<NetNotifiedQueueWithErrors<T>> {
	using FastAllocated<NetNotifiedQueueWithErrors<T>>::operator new;
	using FastAllocated<NetNotifiedQueueWithErrors<T>>::operator delete;

	AcknowledgementReceiver acknowledgements;

	NetNotifiedQueueWithErrors(int futures, int promises) : NotifiedQueue<T>(futures, promises) {}
	NetNotifiedQueueWithErrors(int futures, int promises, const Endpoint& remoteEndpoint)
	  :  NotifiedQueue<T>(futures, promises), FlowReceiver(remoteEndpoint, false) {}

	void destroy() override { delete this; }
	void receive(ArenaObjectReader& reader) override {
		this->addPromiseRef();
		ErrorOr<EnsureTable<T>> message;
		reader.deserialize(message);

		if (message.isError()) {
			if(message.getError().code() == error_code_broken_promise) {
				IFailureMonitor::failureMonitor().endpointNotFound( getRawEndpoint() );
			}
			this->sendError(message.getError());
		} else {
			if(message.get().asUnderlyingType().acknowledgeEndpoint.present()) {
				acknowledgements = AcknowledgementReceiver(message.get().asUnderlyingType().acknowledgeEndpoint.get());
			}
			this->send(std::move(message.get().asUnderlyingType()));
		}
		this->delPromiseRef();
	}

	T pop() override {
		T res = this->popImpl();
		if(acknowledgements.getRawEndpoint().isValid()) {
			acknowledgements.bytesAcknowledged += res.expectedSize();
			FlowTransport::transport().sendUnreliable(SerializeSource<AcknowledgementReply>(AcknowledgementReply(acknowledgements.bytesAcknowledged)), acknowledgements.getEndpoint(TaskPriority::DefaultPromiseEndpoint), true);
		}
		return res;
	}
};

template <class T>
class ReplyPromiseStream {
public:
	// stream.send( request )
	//   Unreliable at most once delivery: Delivers request unless there is a connection failure (zero or one times)

	template<class U>
	void send(U && value) const {
		if (queue->isRemoteEndpoint()) {
			if(!queue->acknowledgements.getRawEndpoint().isValid()) {
				value.acknowledgeEndpoint = queue->acknowledgements.getEndpoint(TaskPriority::DefaultEndpoint);
			}
			queue->acknowledgements.bytesSent += value.expectedSize();
			if((!queue->acknowledgements.ready.isValid() || queue->acknowledgements.ready.isSet()) && queue->acknowledgements.bytesSent - queue->acknowledgements.bytesAcknowledged >= 2e6) {
				queue->acknowledgements.ready = Promise<Void>();
			}
			FlowTransport::transport().sendUnreliable(SerializeSource<T>(std::forward<U>(value)), getEndpoint(), true);
		}
		else
			queue->send(std::forward<U>(value));
	}

	template <class E>
	void sendError(const E& exc) const { 
		queue->sendError(exc);
		if(errors && errors->canBeSet()) {
			errors->sendError(exc);
		}
	}

	FutureStream<T> getFuture() const { queue->addFutureRef(); return FutureStream<T>(queue); }
	ReplyPromiseStream() : queue(new NetNotifiedQueueWithErrors<T>(0, 1)), errors(new SAV<Void>(0, 1)) {}
	ReplyPromiseStream(const ReplyPromiseStream& rhs) : queue(rhs.queue), errors(rhs.errors) { 
		queue->addPromiseRef();
		if(errors) {
			errors->addPromiseRef();
		}
	}
	ReplyPromiseStream(ReplyPromiseStream&& rhs) noexcept : queue(rhs.queue), errors(rhs.errors) { rhs.queue = nullptr; rhs.errors = nullptr; }
	explicit ReplyPromiseStream(const Endpoint& endpoint) : queue(new NetNotifiedQueueWithErrors<T>(0, 1, endpoint)), errors(nullptr) {}

	Future<Void> getErrorFutureAndDelPromiseRef() {
		ASSERT(errors && errors->getPromiseReferenceCount() > 1);
		errors->addFutureRef();
		errors->delPromiseRef();
		return Future<Void>(errors);
	}
	
	~ReplyPromiseStream() {
		if (queue)
			queue->delPromiseRef();
		if (errors)
			errors->delPromiseRef();
	}

	const Endpoint& getEndpoint(TaskPriority taskID = TaskPriority::DefaultEndpoint) const { return queue->getEndpoint(taskID); }
	
	bool operator == (const ReplyPromiseStream<T>& rhs) const { return queue == rhs.queue; }
	bool isEmpty() const { return !queue->isReady(); }
	uint32_t size() const { return queue->size(); }

	Future<Void> onReady() {
		if(queue->acknowledgements.bytesSent - queue->acknowledgements.bytesAcknowledged < 2e6) {
			return Void();
		}
		return queue->acknowledgements.ready.getFuture() || 
		       tagError<Void>(makeDependent<T>(IFailureMonitor::failureMonitor()).onDisconnectOrFailure(queue->acknowledgements.getEndpoint(TaskPriority::DefaultEndpoint)), request_maybe_delivered());
	}

	void operator=(const ReplyPromiseStream& rhs) {
		rhs.queue->addPromiseRef();
		if (queue) queue->delPromiseRef();
		queue = rhs.queue;
		if(rhs.errors) rhs.errors->addPromiseRef();
		if (errors) errors->delPromiseRef();
		errors = rhs.errors;
	}
	void operator=(ReplyPromiseStream&& rhs) noexcept {
		if (queue != rhs.queue) {
			if (queue) queue->delPromiseRef();
			queue = rhs.queue;
			rhs.queue = 0;
		}
		if (errors != rhs.errors) {
			if (errors) errors->delPromiseRef();
			errors = rhs.errors;
			rhs.errors = 0;
		}
	}

private:
	NetNotifiedQueueWithErrors<T>* queue;
	SAV<Void>* errors;
};

template <class Ar, class T>
void save(Ar& ar, const ReplyPromiseStream<T>& value) {
	auto const& ep = value.getEndpoint();
	ar << ep;
}

template <class Ar, class T>
void load(Ar& ar, ReplyPromiseStream<T>& value) {
	Endpoint endpoint;
	ar >> endpoint;
	value = ReplyPromiseStream<T>(endpoint);
}

template <class T>
struct serializable_traits<ReplyPromiseStream<T>> : std::true_type {
	template <class Archiver>
	static void serialize(Archiver& ar, ReplyPromiseStream<T>& stream) {
		if constexpr (Archiver::isDeserializing) {
			Endpoint endpoint;
			serializer(ar, endpoint);
			stream = ReplyPromiseStream<T>(endpoint);
		} else {
			const auto& ep = stream.getEndpoint();
			serializer(ar, ep);
		}
	}
};

template <class T>
struct NetNotifiedQueue final : NotifiedQueue<T>, FlowReceiver, FastAllocated<NetNotifiedQueue<T>> {
	using FastAllocated<NetNotifiedQueue<T>>::operator new;
	using FastAllocated<NetNotifiedQueue<T>>::operator delete;

	NetNotifiedQueue(int futures, int promises) : NotifiedQueue<T>(futures, promises) {}
	NetNotifiedQueue(int futures, int promises, const Endpoint& remoteEndpoint)
	  : NotifiedQueue<T>(futures, promises), FlowReceiver(remoteEndpoint, true) {}

	void destroy() override { delete this; }
	void receive(ArenaObjectReader& reader) override {
		this->addPromiseRef();
		T message;
		reader.deserialize(message);
		this->send(std::move(message));
		this->delPromiseRef();
	}
	bool isStream() const override { return true; }
};

template <class T>
class RequestStream {
public:
	// stream.send( request )
	//   Unreliable at most once delivery: Delivers request unless there is a connection failure (zero or one times)

	template<class U>
	void send(U && value) const {
		if (queue->isRemoteEndpoint()) {
			FlowTransport::transport().sendUnreliable(SerializeSource<T>(std::forward<U>(value)), getEndpoint(), true);
		}
		else
			queue->send(std::forward<U>(value));
	}

	/*void sendError(const Error& error) const {
	ASSERT( !queue->isRemoteEndpoint() );
	queue->sendError(error);
	}*/

	// stream.getReply( request )
	//   Reliable at least once delivery: Eventually delivers request at least once and returns one of the replies if communication is possible.  Might deliver request
	//      more than once.
	//   If a reply is returned, request was or will be delivered one or more times.
	//   If cancelled, request was or will be delivered zero or more times.
	template <class X>
	Future< REPLY_TYPE(X) > getReply(const X& value) const {
		ASSERT(!getReplyPromise(value).getFuture().isReady());
		if (queue->isRemoteEndpoint()) {
			return sendCanceler(getReplyPromise(value), FlowTransport::transport().sendReliable(SerializeSource<T>(value), getEndpoint()), getEndpoint());
		}
		send(value);
		return reportEndpointFailure(getReplyPromise(value).getFuture(), getEndpoint());
	}
	template <class X>
	Future<REPLY_TYPE(X)> getReply(const X& value, TaskPriority taskID) const {
		setReplyPriority(value, taskID);
		return getReply(value);
	}
	template <class X>
	Future<X> getReply() const {
		return getReply(ReplyPromise<X>());
	}
	template <class X>
	Future<X> getReplyWithTaskID(TaskPriority taskID) const {
		ReplyPromise<X> reply;
		reply.getEndpoint(taskID);
		return getReply(reply);
	}

	// stream.tryGetReply( request )
	//   Unreliable at most once delivery: Either delivers request and returns a reply, or returns failure (Optional<T>()) eventually.
	//   If a reply is returned, request was delivered exactly once.
	//   If cancelled or returns failure, request was or will be delivered zero or one times.
	//   The caller must be capable of retrying if this request returns failure
	template <class X>
	Future<ErrorOr<REPLY_TYPE(X)>> tryGetReply(const X& value, TaskPriority taskID) const {
		setReplyPriority(value, taskID);
		if (queue->isRemoteEndpoint()) {
			Future<Void> disc = makeDependent<T>(IFailureMonitor::failureMonitor()).onDisconnectOrFailure(getEndpoint(taskID));
			if (disc.isReady()) {
				return ErrorOr<REPLY_TYPE(X)>(request_maybe_delivered());
			}
			Reference<Peer> peer = FlowTransport::transport().sendUnreliable(SerializeSource<T>(value), getEndpoint(taskID), true);
			auto& p = getReplyPromise(value);
			return waitValueOrSignal(p.getFuture(), disc, getEndpoint(taskID), p, peer);
		}
		send(value);
		auto& p = getReplyPromise(value);
		return waitValueOrSignal(p.getFuture(), Never(), getEndpoint(taskID), p);
	}

	template <class X>
	Future<ErrorOr<REPLY_TYPE(X)>> tryGetReply(const X& value) const {
		if (queue->isRemoteEndpoint()) {
			Future<Void> disc = makeDependent<T>(IFailureMonitor::failureMonitor()).onDisconnectOrFailure(getEndpoint());
			if (disc.isReady()) {
				return ErrorOr<REPLY_TYPE(X)>(request_maybe_delivered());
			}
			Reference<Peer> peer = FlowTransport::transport().sendUnreliable(SerializeSource<T>(value), getEndpoint(), true);
			auto& p = getReplyPromise(value);
			return waitValueOrSignal(p.getFuture(), disc, getEndpoint(), p, peer);
		}
		else {
			send(value);
			auto& p = getReplyPromise(value);
			return waitValueOrSignal(p.getFuture(), Never(), getEndpoint(), p);
		}
	}

	//FIXME comment

	template <class X>
	FutureStream<REPLYSTREAM_TYPE(X)> getReplyStream(const X& value, TaskPriority taskID) const {
		setReplyPriority(value, taskID);
		if (queue->isRemoteEndpoint()) {
			Future<Void> disc = makeDependent<T>(IFailureMonitor::failureMonitor()).onDisconnectOrFailure(getEndpoint(taskID));
			auto& p = getReplyPromiseStream(value);
			if (disc.isReady()) {
				p.sendError(request_maybe_delivered());
				return p.getFuture();
			}
			Reference<Peer> peer = FlowTransport::transport().sendUnreliable(SerializeSource<T>(value), getEndpoint(taskID), true);
			endStreamOnDisconnect(disc, p, peer);
			return p.getFuture();
		}
		send(value);
		auto& p = getReplyPromiseStream(value);
		return p.getFuture();
	}

	template <class X>
	FutureStream<REPLYSTREAM_TYPE(X)> getReplyStream(const X& value) const {
		if (queue->isRemoteEndpoint()) {
			Future<Void> disc = makeDependent<T>(IFailureMonitor::failureMonitor()).onDisconnectOrFailure(getEndpoint());
			auto& p = getReplyPromiseStream(value);
			if (disc.isReady()) {
				p.sendError(request_maybe_delivered());
				return p.getFuture();
			}
			Reference<Peer> peer = FlowTransport::transport().sendUnreliable(SerializeSource<T>(value), getEndpoint(), true);
			endStreamOnDisconnect(disc, p, peer);
			return p.getFuture();
		}
		else {
			send(value);
			auto& p = getReplyPromiseStream(value);
			return p.getFuture();
		}
	}

	// stream.getReplyUnlessFailedFor( request, double sustainedFailureDuration, double sustainedFailureSlope )
	//   Reliable at least once delivery: Like getReply, delivers request at least once and returns one of the replies. However, if
	//     the failure detector considers the endpoint failed permanently or for the given amount of time, returns failure instead.
	//   If a reply is returned, request was or will be delivered one or more times.
	//   If cancelled or returns failure, request was or will be delivered zero or more times.
	//   If it returns failure, the failure detector considers the endpoint failed permanently or for the given amount of time
	//   See IFailureMonitor::onFailedFor() for an explanation of the duration and slope parameters.
	template <class X>
	Future<ErrorOr<REPLY_TYPE(X)>> getReplyUnlessFailedFor(const X& value, double sustainedFailureDuration, double sustainedFailureSlope, TaskPriority taskID) const {
		// If it is local endpoint, no need for failure monitoring
		return waitValueOrSignal(getReply(value, taskID),
				makeDependent<T>(IFailureMonitor::failureMonitor()).onFailedFor(getEndpoint(taskID), sustainedFailureDuration, sustainedFailureSlope),
				getEndpoint(taskID));
	}

	template <class X>
	Future<ErrorOr<REPLY_TYPE(X)>> getReplyUnlessFailedFor(const X& value, double sustainedFailureDuration, double sustainedFailureSlope) const {
		// If it is local endpoint, no need for failure monitoring
		return waitValueOrSignal(getReply(value),
				makeDependent<T>(IFailureMonitor::failureMonitor()).onFailedFor(getEndpoint(), sustainedFailureDuration, sustainedFailureSlope),
				getEndpoint());
	}

	template <class X>
	Future<ErrorOr<X>> getReplyUnlessFailedFor(double sustainedFailureDuration, double sustainedFailureSlope) const {
		return getReplyUnlessFailedFor(ReplyPromise<X>(), sustainedFailureDuration, sustainedFailureSlope);
	}

	explicit RequestStream(const Endpoint& endpoint) : queue(new NetNotifiedQueue<T>(0, 1, endpoint)) {}

	FutureStream<T> getFuture() const { queue->addFutureRef(); return FutureStream<T>(queue); }
	RequestStream() : queue(new NetNotifiedQueue<T>(0, 1)) {}
	RequestStream(const RequestStream& rhs) : queue(rhs.queue) { queue->addPromiseRef(); }
	RequestStream(RequestStream&& rhs) noexcept : queue(rhs.queue) { rhs.queue = 0; }
	void operator=(const RequestStream& rhs) {
		rhs.queue->addPromiseRef();
		if (queue) queue->delPromiseRef();
		queue = rhs.queue;
	}
	void operator=(RequestStream&& rhs) noexcept {
		if (queue != rhs.queue) {
			if (queue) queue->delPromiseRef();
			queue = rhs.queue;
			rhs.queue = 0;
		}
	}
	~RequestStream() {
		if (queue)
			queue->delPromiseRef();
		//queue = (NetNotifiedQueue<T>*)0xdeadbeef;
	}

	const Endpoint& getEndpoint(TaskPriority taskID = TaskPriority::DefaultEndpoint) const { return queue->getEndpoint(taskID); }
	void makeWellKnownEndpoint(Endpoint::Token token, TaskPriority taskID) {
		queue->makeWellKnownEndpoint(token, taskID);
	}

	bool operator == (const RequestStream<T>& rhs) const { return queue == rhs.queue; }
	bool isEmpty() const { return !queue->isReady(); }
	uint32_t size() const { return queue->size(); }

	std::pair<FlowReceiver*, TaskPriority> getReceiver( TaskPriority taskID = TaskPriority::DefaultEndpoint ) {
		return std::make_pair((FlowReceiver*)queue, taskID);
	}

private:
	NetNotifiedQueue<T>* queue;
};

template <class Ar, class T>
void save(Ar& ar, const RequestStream<T>& value) {
	auto const& ep = value.getEndpoint();
	ar << ep;
	UNSTOPPABLE_ASSERT(ep.getPrimaryAddress().isValid());  // No serializing PromiseStreams on a client with no public address
}

template <class Ar, class T>
void load(Ar& ar, RequestStream<T>& value) {
	Endpoint endpoint;
	ar >> endpoint;
	value = RequestStream<T>(endpoint);
}

template <class T>
struct serializable_traits<RequestStream<T>> : std::true_type {
	template <class Archiver>
	static void serialize(Archiver& ar, RequestStream<T>& stream) {
		if constexpr (Archiver::isDeserializing) {
			Endpoint endpoint;
			serializer(ar, endpoint);
			stream = RequestStream<T>(endpoint);
		} else {
			const auto& ep = stream.getEndpoint();
			serializer(ar, ep);
			if constexpr (Archiver::isSerializing) { // Don't assert this when collecting vtable for flatbuffers
				UNSTOPPABLE_ASSERT(ep.getPrimaryAddress()
				                       .isValid()); // No serializing PromiseStreams on a client with no public address
			}
		}
	}
};

#endif
#include "fdbrpc/genericactors.actor.h"
