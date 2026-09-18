/*=====================================================================
WorkerThread.h
--------------
Copyright Glare Technologies Limited 2018 -
=====================================================================*/
#pragma once


#include "../shared/URLString.h"
#include <RequestInfo.h>
#include <MessageableThread.h>
#include <Platform.h>
#include <MyThread.h>
#include <EventFD.h>
#include <MySocket.h>
#include <SocketBufferOutStream.h>
#include <Vector.h>
#include "../shared/UserID.h"
#include "../shared/UID.h"
#include <BufferInStream.h>
#include <AtomicInt.h>
#include <string>
class Server;
class ServerAllWorldsState;
class ServerWorldState;
class BuilderAISession;


/*=====================================================================
WorkerThread
------------
This thread runs on the server, and handles communication with a single client.
=====================================================================*/
class WorkerThread : public MessageableThread
{
public:
	// May throw glare::Exception from constructor if EventFD init fails.
	WorkerThread(const Reference<SocketInterface>& socket, Server* server, bool is_websocket_connection);
	virtual ~WorkerThread();

	virtual void doRun() override;

	virtual void kill() override;

	Reference<ServerWorldState> cur_world_state; // World the client is connected to.

	void enqueueDataToSend(const SocketBufferOutStream& packet); // threadsafe
	void enqueueDataToSend(const ArrayRef<uint8> data); // threadsafe

	// UID of the avatar assigned to this connection's client, or an invalid UID before one has been assigned.
	// Stored atomically so other threads (see Server::enqueuePacketToClientWithAvatarUID) can route packets to
	// a single client without taking this thread's locks.
	UID getClientAvatarUID() const { return UID((uint64)client_avatar_uid_atomic.getVal()); } // threadsafe
	void setClientAvatarUID(UID uid) { client_avatar_uid_atomic = (glare::atomic_int)uid.value(); } // threadsafe

	web::RequestInfo websocket_request_info; // If the client connected via a websocket, this the HTTP request data.  Is used for accessing the login cookie.

private:
	void sendGetFileMessageIfNeeded(const URLString& resource_URL);
	void handleResourceUploadConnection();
	void handleResourceDownloadConnection();
	void handleScreenshotBotConnection();
	void handleEthBotConnection();
	void conPrintIfNotFuzzing(const std::string& msg);
	void sendPerWorldInitialDataToClient(ServerAllWorldsState* world_state, uint32 client_protocol_version);
	void enqueuePacketToBroadcast(const SocketBufferOutStream& packet_buffer);
	void handleBuilderAIUserMessage(UserID client_user_id, const std::string& client_user_name);

	Reference<SocketInterface> socket;
	Server* server;
	EventFD event_fd;	

	Mutex data_to_send_mutex;
	js::Vector<uint8, 16> data_to_send			GUARDED_BY(data_to_send_mutex);
	js::Vector<uint8, 16> temp_data_to_send;

	js::Vector<uint8, 16> m_temp_buf;

	SocketBufferOutStream scratch_packet;

	BufferInStream msg_buffer;
public:
	bool fuzzing; // Are we currently doing fuzz-testing?
private:
	bool write_trace; // Should we write a record of network traffic to disk for fuzz seeding?

	glare::AtomicInt should_quit;

	glare::AtomicInt client_avatar_uid_atomic; // See getClientAvatarUID().  Initialised to UID::invalidUID().

	bool is_websocket_connection;

	Reference<BuilderAISession> builder_ai_session; // The in-world Builder AI session for this connection.  Created lazily on the first Builder AI message.
};
