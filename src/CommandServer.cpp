#include "CommandServer.h"

#include "json.hpp"
#include <glog/logging.h>

#include <thread>

#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ucred.h>
#include <sys/resource.h>
#include <arpa/inet.h>

using namespace std;
using json = nlohmann::json;

/**
 * Main server entry point
 */
void CommandServerEntry(void *ctx) {
#ifdef __APPLE__
	pthread_setname_np("Command Server");
#else
	pthread_setname_np(pthread_self(), "Command Server");
#endif

	CommandServer *srv = static_cast<CommandServer *>(ctx);
	srv->threadEntry();
}

/**
 * Client thread entry point
 */
void CommandClientEntry(void *ctx) {
#ifdef __APPLE__
	pthread_setname_np("Command Server Client Thread");
#else
	pthread_setname_np(pthread_self(), "Command Server Client Thread");
#endif

	CommandServer::ClientThreadCtx *info = static_cast<CommandServer::ClientThreadCtx *>(ctx);
	info->srv->clientThreadEntry(info->fd);
}


/**
 * Initializes the command server. This creates some internal structures and
 * prepares for the thread to start.
 */
CommandServer::CommandServer(string socket, DataStore *store) {
	this->socketPath = socket;
	this->store = store;
}

/**
 * Closes the listening socket.
 */
CommandServer::~CommandServer() {
	delete this->worker;
}

/**
 * Starts the command server thread.
 */
void CommandServer::start() {
	LOG(INFO) << "Starting command server thread";
	this->worker = new thread(CommandServerEntry, this);
}

/**
 * Prepares the server to stop. This closes down the listening socket and kills
 * the thread.
 */
void CommandServer::stop() {
	int err = 0;

	LOG(INFO) << "Shutting down command server...";

	// signal for the thread to stop, and force the socket to close
	this->run = false;

	// close the socket; this'll terminate the accept() call
	err = close(this->sock);
	PLOG_IF(ERROR, err != 0) << "Couldn't close command socket: ";

	// wait for the thread to terminate
	this->worker->join();

	// now, terminate any remaining clients
	if(!this->clients.empty()) {
		LOG(INFO) << "Closing " << this->clients.size() << " client connections";

		for(auto value : this->clients) {
			int fd = get<0>(value);
			thread *t = get<1>(value);

			// check if the fd is closed already
		   	if(fcntl(fd, F_GETFD) != -1) {
				// if not, close it
		   		err = close(fd);
		   		PLOG_IF(ERROR, err != 0) << "Couldn't close client socket: ";
		   	}

			// now, wait for the thread to terminate
			t->join();
		}
	} else {
		LOG(INFO) << "No active client connections to close, we're done";
	}
}

/**
 * Main loop for the command server thread; this is called when the thread is
 * first initialized. This sets up the socket and waits for commands.
 */
void CommandServer::threadEntry() {
	int msgsock = 0;
	int err = 0;

	// create the socket
	this->createSocket();

	// listen on the socket
	while(this->run) {
		// accept incoming connections
		msgsock = accept(sock, 0, 0);

		// check return value; if it's -1, there was an error
		if(msgsock == -1) {
			// ignore ECONNABORTED messages if we're shutting down
			if(errno != ECONNABORTED && this->run == true) {
				PLOG(WARNING) << "Couldn't accept command connection: ";
			}

			// continue the loop
			continue;
		}

		// otherwise, accept the connection and start a new thread
		this->acceptClient(msgsock);
	}

	// clean up the socket (i.e. delete the file on disk)
	LOG(INFO) << "Closing command connection";

	err = unlink(this->socketPath.c_str());
	PLOG_IF(ERROR, err != 0) << "Couldn't unlink command socket: ";
}

/**
 * Creates the domain socket (using the stream mode) and prepares it for
 * listening.
 */
void CommandServer::createSocket() {
	struct sockaddr_un server;
	int err = 0;

	// create the bare socket
	this->sock = socket(AF_UNIX, SOCK_STREAM, 0);
	PCHECK(this->sock > 0) << "Creating command server socket failed";

	// set up properties for binding
	server.sun_family = AF_UNIX;
	strcpy(server.sun_path, this->socketPath.c_str()); // TODO: length checking

	// bind the socket
	err = ::bind(this->sock, (struct sockaddr *) &server, (socklen_t) sizeof(struct sockaddr_un));
	PCHECK(err == 0) << "Binding command server socket failed on " << this->socketPath;

	// accept connections
	err = listen(this->sock, 5);
	PCHECK(err == 0) << "Listening on command server socket failed";

	// log some info
	LOG(INFO) << "Created command socket at " << this->socketPath;
	this->run = true;
}

/**
 * Processes a request from a client. This has the effect of creating a new
 * thread for that client and continuously reading from that socket.
 */
void CommandServer::acceptClient(int fd) {
	// allocate context and pass it to the new thread
	ClientThreadCtx *ctx = new ClientThreadCtx(this, fd);
	thread *t = new thread(CommandClientEntry, ctx);

	// store this info for later so we can cleanly terminate connections
	this->clients.push_back(make_tuple(fd, t));
}

/**
 * Entry point for the client thread.
 */
void CommandServer::clientThreadEntry(int fd) {
	int err = 0, rsz = 0, len = 0;

	// get the process that opened this connection
	struct ucred ucred;
	len = sizeof(struct ucred);

	/*
	 * get the pid of the remote process. this is only done for logging purposes
	 * so on systems where this isn't supported (*cough* macOS) it just doesn't
	 * get compiled in. really lame, but meh.
	 */
#ifdef SO_PEERCRED
	err = getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len);

	PLOG_IF(ERROR, err != 0) << "Couldn't get peer info on " << fd << ": ";

	if(err == 0) {
		LOG(INFO) << "Received connection from pid " << ucred.pid << " on fd " << fd;
	}

	// set thread name
	char name[64];
	snprintf(name, 64, "Command Server Client - pid %u", ucred.pid);
	pthread_setname_np(pthread_self(), name);
#endif

	// allocate a read buffer
	char *buffer = new char[kClientBufferSz];
	// std::fill(buffer, buffer + kClientBufferSz, 0);

	// keep reading from the socket while we are running
	while(this->run) {
		// clear the buffer, then read from the socket
		std::fill(buffer, buffer + kClientBufferSz, 0);
		rsz = read(fd, buffer, kClientBufferSz);

		// LOG(INFO) << "Read " << rsz << " bytes";

		// if the read size was zero, the connection was closed
		if(rsz == 0) {
			LOG(WARNING) << "Connection " << fd << " closed by host";
			break;
		}
		// handle error conditions
		else if(rsz == -1) {
			// ignore ECONNABORTED messages if we're shutting down
			if(this->run == true) {
				PLOG(WARNING) << "Couldn't read from client: ";
				break;
			}
		}
		// otherwise, try to parse the JSON
		else {
			// zero-terminate the string for good measure
			int zeroByte = rsz;

			if(zeroByte >= kClientBufferSz) {
				zeroByte = kClientBufferSz - 1;
			}

			buffer[zeroByte] = 0;

			// create a string from it
			string jsonStr = std::string(buffer);
			// LOG(INFO) << jsonStr;

			try {
				json j = json::parse(jsonStr);

				// process the message
				this->processClientRequest(j, fd);
			} catch(json::parse_error e) {
				LOG(WARNING) << "Parse error in client message: " << e.what();

				// close the connection to tell the client to sod off
				close(fd);
				break;
			}
		}
	}

	/*
	 * we _could_ remove this thread from the clients vector, but since the
	 * socket is already closed, we can simply check if it's been closed by
	 * the shutdown procedure (or another external source) and only close it
	 * if needed. the shutdown procedure will only close file descriptors that
	 * haven't been closed and wait for the thread to terminate.
 	 */
	if(fcntl(fd, F_GETFD) != -1) {
		err = close(fd);
		PLOG_IF(ERROR, err != 0) << "Couldn't close client socket: ";
	}

	// de-allocate the buffer
	delete[] buffer;
}

/**
 * Processes a client request by unwrapping the JSON.
 */
void CommandServer::processClientRequest(json &j, int fd) {
	int err = 0;

	// extract the message type
	MessageType msgType = static_cast<MessageType>(j["type"]);
	VLOG(2) << "Received request type " << msgType;

	// invoke the correct handler
	json response;

	switch(msgType) {
		case kMessageStatus:
			this->clientRequestStatus(response);
			break;

		case kMessageGetNodes:
			this->clientRequestListNodes(response);
			break;

		case kMessageGetGroups:
			this->clientRequestListGroups(response);
			break;
	}

	// add the txn field if it exists
	try {
		if(j.at("txn")) {
			response["txn"] = j["txn"];
		}
	} catch(exception e) {}

	// serialize it to a string and send it
	string responseStr = response.dump();

	const char *responseBuf = responseStr.c_str();
	size_t length = strlen(responseBuf);

	err = write(fd, responseBuf, length);

	// check for error
	PLOG_IF(ERROR, err == -1) << "Couldn't write to client: ";

	if(err != length) {
		LOG(WARNING) << "Wrote " << err << " bytes, buffer was " << length << "!";
	}
}

/**
 * Processes the status request. This simply builds the response, and sends it
 * over the wire.
 */
void CommandServer::clientRequestStatus(json &response) {
	int err = 0;

	// get loadavg
	double load[3];
	getloadavg(load, 3);

	// get memory usage
	struct rusage usage;
	memset(&usage, 0, sizeof(struct rusage));

	err = getrusage(RUSAGE_SELF, &usage);

	if(err != 0) {
		PLOG(ERROR) << "Couldn't get resource usage: ";
		response["status"] = errno;
	} else {
		response["status"] = 0;
	}

	// build response
	response["version"] = string(VERSION);
	response["build"] = string(GIT_HASH) + "/" + string(GIT_BRANCH);

	response["load"] = {load[0], load[1], load[2]};

	response["mem"] = usage.ru_maxrss;
}

/**
 * Returns a listing of all nodes that are known to the server.
 */
void CommandServer::clientRequestListNodes(json &response) {
	response["status"] = 0;

	// iterate over all nodes and insert them
	response["nodes"] = vector<DataStore::Node>();

	vector<DataStore::Node *> nodes = this->store->getAllNodes();
	for(auto node : nodes) {
		response["nodes"].push_back(*node);

		// delete the nodes in the vector; they're temporary
		delete node;
	}
}

/**
 * Returns a listing of all groups known to the server.
 */
void CommandServer::clientRequestListGroups(nlohmann::json &response) {
	response["status"] = 0;

	// iterate over all groups and insert them
	response["groups"] = vector<DataStore::Group>();

	vector<DataStore::Group *> groups = this->store->getAllGroups();
	for(auto group : groups) {
		response["groups"].push_back(*group);

		// delete the groups in the vector; they're temporary
		delete group;
	}
}

#pragma mark - JSON Conversion Routines
/**
 * Converts a node object to a json representation.
 */
void to_json(json& j, const DataStore::Node& n) {
	// convert the MAC address to a string
	char mac[24];
	snprintf(mac, 24, "%02X-%02X-%02X-%02X-%02X-%02X", n.macAddr[0],
			 n.macAddr[1], n.macAddr[2], n.macAddr[3], n.macAddr[4],
			 n.macAddr[5]);

	// convert IP to a string
	char ipAddr[16];
	inet_ntop(AF_INET, &n.ip, ipAddr, 16);

	// build the JSON representation
	j = json{
		{"id", n.id},

		{"mac", string(mac)},
		{"ip", string(ipAddr)},
		{"hostname", n.hostname},

		{"adopted", n.adopted},

		{"hwVersion", n.hwVersion},
		{"swVersion", n.swVersion},

		{"lastSeen", n.lastSeen}
	};
}

/**
 * Converts a group object to a json representation.
 */
void to_json(json& j, const DataStore::Group& group) {
	// build the JSON representation
	j = json{
		{"id", group.id},

		{"name", group.name},

		{"enabled", group.enabled},

		{"start", group.start},
		{"end", group.end},

		{"currentRoutine", group.currentRoutine}
	};
}
