#include "CommandServer.h"

#include "DataStore.h"
#include "Routine.h"
#include "EffectRunner.h"
#include "OutputMapper.h"

#include "json.hpp"
#include "INIReader.h"
#include <glog/logging.h>

#include <thread>
#include <sstream>

#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ucred.h>
#include <sys/resource.h>
#include <arpa/inet.h>

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
CommandServer::CommandServer(DataStore *store, INIReader *reader, EffectRunner *runner) {
	this->config = reader;
	this->store = store;
	this->runner = runner;

	// get the path of the socket
	this->socketPath = this->config->Get("command", "socketPath", "");
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
	this->worker = new std::thread(CommandServerEntry, this);
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
			int fd = std::get<0>(value);
			std::thread *t = std::get<1>(value);

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

	// delete the existing socket if needed
	if(this->config->GetBoolean("command", "unlinkSocket", true)) {
		err = unlink(this->socketPath.c_str());
		PLOG_IF(INFO, (err != 0 && errno != ENOENT)) << "Couldn't unlink command socket: ";
	}

	// create the bare socket
	this->sock = socket(AF_UNIX, SOCK_STREAM, 0);
	PCHECK(this->sock > 0) << "Creating command server socket failed";

	// set up properties for binding
	server.sun_family = AF_UNIX;
	strcpy(server.sun_path, this->socketPath.c_str()); // TODO: length checking

	// bind the socket
	err = bind(this->sock, (struct sockaddr *) &server, (socklen_t) sizeof(struct sockaddr_un));
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
	std::thread *t = new std::thread(CommandClientEntry, ctx);

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
      // JSON type errors
      catch(json::type_error e) {
				LOG(WARNING) << "Type error processing command: " << e.what();

				// close the connection to tell the client to sod off
				close(fd);
				break;
      }
      // handle all other types of exceptions
      catch(std::exception e) {
        LOG(WARNING) << "Error executing command (" << jsonStr << "): " << e.what();

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
			this->clientRequestStatus(response, j);
			break;

		case kMessageGetNodes:
			this->clientRequestListNodes(response, j);
			break;
    case kMessageUpdateNode:
      this->clientRequestUpdateNode(response, j);
      break;

		case kMessageGetGroups:
			this->clientRequestListGroups(response, j);
			break;
    case kMessageUpdateGroup:
      this->clientRequestUpdateGroup(response, j);
      break;

		case kMessageAddMapping:
			this->clientRequestAddMapping(response, j);
			break;
		case kMessageRemoveMapping:
			this->clientRequestRemoveMapping(response, j);
			break;

    case kMessageGetBrightness:
      // TODO: implememnt
      break;
    case kMessageSetBrightness:
      this->clientRequestSetBrightness(response, j);
      break;

    case kMessageGetRoutines:
      this->clientRequesListRoutines(response, j);
      break;
    case kMessageUpdateRoutine:
      this->clientRequestUpdateRoutine(response, j);
      break;

    case kMessageGetChannels:
      this->clientRequesListChannels(response, j);
      break;
    case kMessageUpdateChannel:
      this->clientRequesUpdateChannel(response, j);
      break;
	}

	// add the txn field if it exists
	try {
		if(j.at("txn")) {
			response["txn"] = j["txn"];
		}
	} catch(std::exception e) {}

	// serialize it to a string and send it
	bool humanReadable = this->config->GetBoolean("command", "humanReadableResponses", false);

	string responseStr = humanReadable ? response.dump(4) : response.dump();

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
void CommandServer::clientRequestStatus(json &response, json &request) {
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
		response["status"] = kErrorSyscallError;
		response["error"] = string(strerror(errno));
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
void CommandServer::clientRequestListNodes(json &response, json &request) {
	response["status"] = 0;

	// iterate over all nodes and insert them
	response["nodes"] = std::vector<DbNode>();

	std::vector<DbNode *> nodes = this->store->getAllNodes();
	for(auto node : nodes) {
		response["nodes"].push_back(json(*node));

		// delete the nodes in the vector; they're temporary
		delete node;
	}
}
/**
 * Updates one or more properties on an existing node.
 *
 * Parameters:
 * - id: ID of node to update.
 * - set: Key/value array of keys to update: TODO: figure out keys
 *
 * TODO: find a way to propagate this to all existing instances of the node
 * so that code changes are reflected immediately?
 */
void CommandServer::clientRequestUpdateNode(nlohmann::json &response, nlohmann::json &request) {
  int nodeId = request["id"];

  // try to find routine
  DbNode *node = this->store->findNodeWithId(nodeId);

  if(node == nullptr) {
		response["status"] = kErrorInvalidNodeId;
		response["error"] = "Couldn't find node with the specified ID";
		response["id"] = nodeId;

    return;
  }

  // TODO: what keys can we update?

  // we need to save this node now
  this->store->update(node);
  delete node;

  // done!
  response["status"] = 0;
}



/**
 * Returns a listing of all groups known to the server.
 */
void CommandServer::clientRequestListGroups(nlohmann::json &response, json &request) {
	response["status"] = 0;

	// iterate over all groups and insert them
	response["groups"] = std::vector<DbGroup>();

	std::vector<DbGroup *> groups = this->store->getAllGroups();
	for(auto group : groups) {
		response["groups"].push_back(json(*group));

		// delete the groups in the vector; they're temporary
		delete group;
	}
}
/**
 * Updates one or more properties on an existing group.
 *
 * Parameters:
 * - id: ID of group to update.
 * - set: Key/value array of keys to update: enabled, start, end, name.
 *
 * TODO: find a way to propagate this to all existing instances of the group
 * so that code changes are reflected immediately?
 */
void CommandServer::clientRequestUpdateGroup(nlohmann::json &response, nlohmann::json &request) {
  int groupId = request["id"];

  // try to find routine
  DbGroup *group = this->store->findGroupWithId(groupId);

  if(group == nullptr) {
		response["status"] = kErrorInvalidGroupId;
		response["error"] = "Couldn't find group with the specified ID";
		response["id"] = groupId;

    return;
  }

  // update keys
  if(request.count("enabled") == 1) {
    group->enabled = request["enabled"];
  }

  if(request.count("start") == 1) {
    group->start = request["start"];
  }
  if(request.count("end") == 1) {
    group->end = request["end"];
  }

  if(request.count("name") == 1) {
    group->name = request["name"];
  }

  // we need to save this group now
  this->store->update(group);
  delete group;

  // done!
  response["status"] = 0;
}



/**
 * Returns a listing of all routines on the server.
 */
void CommandServer::clientRequesListRoutines(nlohmann::json &response, nlohmann::json &request) {
	response["status"] = 0;

	// iterate over all routines and insert them
	response["routines"] = std::vector<DbRoutine>();

	std::vector<DbRoutine *> routines = this->store->getAllRoutines();
	for(auto routine : routines) {
		response["routines"].push_back(json(*routine));

		// delete the routines in the vector; they're temporary
		delete routine;
	}
}
/**
 * Updates one or more properties on an existing routine.
 *
 * Parameters:
 * - id: ID of routine to update.
 * - set: Key/value array of keys to update: can be name, code, or defaults.
 *
 * TODO: find a way to propagate this to all existing instances of the routine
 * so that code changes are reflected immediately?
 */
void CommandServer::clientRequestUpdateRoutine(nlohmann::json &response, nlohmann::json &request) {
  int routineId = request["id"];

  // try to find routine
  DbRoutine *routine = this->store->findRoutineWithId(routineId);

  if(routine == nullptr) {
		response["status"] = kErrorInvalidRoutineId;
		response["error"] = "Couldn't find routine with the specified ID";
		response["id"] = routineId;

    return;
  }

  // update keys
  if(request.count("name") == 1) {
    routine->name = request["name"];
  }

  if(request.count("code") == 1) {
    routine->code = request["code"];
  }

  if(response.count("defaults") == 1) {
		std::map<string, double> params = request["defaults"];
    routine->defaultParams = params;
  }

  // we need to save this routine now
  this->store->update(routine);
  delete routine;

  // done!
  response["status"] = 0;
}



/**
 * Returns a listing of all channels on the server.
 */
void CommandServer::clientRequesListChannels(nlohmann::json &response, nlohmann::json &request) {
	response["status"] = 0;

	// iterate over all routines and insert them
	response["channels"] = std::vector<DbChannel>();

	std::vector<DbChannel *> channels = this->store->getAllChannels();
	for(auto channel : channels) {
		response["channels"].push_back(json(*channel));

		// delete the channels in the vector; they're temporary
		delete channel;
	}
}
/**
 * Updates one or more properties on an existing channel.
 *
 * Parameters:
 * - id: ID of the channel to update.
 * - set: Key/value array of keys to update: can be fbOffset, node, nodeIndex, size.
 *
 * TODO: find a way to propagate this to all existing instances of the channel
 * so all changes are reflected immediately?
 */
void CommandServer::clientRequesUpdateChannel(nlohmann::json &response, nlohmann::json &request) {
  int channelId = request["id"];

  // try to find channel
  DbChannel *channel = this->store->findChannelWithId(channelId);

  if(channel == nullptr) {
		response["status"] = kErrorInvalidChannelId;
		response["error"] = "Couldn't find channel with the specified ID";
		response["id"] = channelId;

    return;
  }

  // update keys
  if(request.count("fbOffset") == 1) {
    // offset into internal computed framebuffer
    channel->fbOffset = request["fbOffset"];
  }

  if(request.count("node") == 1) {
    // find node
    DbNode *node = this->store->findNodeWithId(request["node"]);

    if(node == nullptr) {
  		response["status"] = kErrorInvalidNodeId;
  		response["error"] = "Couldn't find node with the specified ID";
  		response["id"] = channelId;

      // be sure to clean up the channel
      delete channel;
      return;
    }

    // assign the found node
    channel->node = node;
  }

  if(request.count("nodeIndex") == 1) {
    // channel number on node
    channel->nodeOffset = request["nodeIndex"];
  }

  if(request.count("size") == 1) {
    // size (number of pixels)
    channel->numPixels = request["size"];
  }

  // we need to save this routine now
  this->store->update(channel);
  delete channel;

  // done!
  response["status"] = 0;
}



/**
 * Adds a mapping between one or more groups (creating an ubergroup if required)
 * and a specified effect routine. An optional parameter array may be passed to
 * the routine.
 *
 * {"type": 3, "routine": {"id": 27}, "groups": [1]}
 */
void CommandServer::clientRequestAddMapping(json &response, json &request) {
	Routine *routine = nullptr;

	// fetch the routine
	int routineId = request["routine"]["id"];
	DbRoutine *dbRoutine = this->store->findRoutineWithId(routineId);

	if(dbRoutine == nullptr) {
		response["status"] = kErrorInvalidRoutineId;
		response["error"] = "Couldn't find routine with the specified ID";
		response["id"] = routineId;

		return;
	}

	// do we have any parameters?
	bool hasParams = (request["routine"].count("params") > 0);


	// get each of the groups
	std::vector<DbGroup *> groups;

	for(int id : request["groups"]) {
		DbGroup *group = this->store->findGroupWithId(id);

		// couldn't find the group
		if(group == nullptr) {
      response["status"] = kErrorInvalidGroupId;
      response["error"] = "Couldn't find group with the specified ID";
      response["id"] = id;

			return;
		}

		groups.push_back(group);
	}


	// create the routine
	OutputMapper *mapper = this->runner->getMapper();

	if(hasParams) {
		std::map<string, double> params = request["routine"]["params"];
		routine = new Routine(dbRoutine, params);
	} else {
		routine = new Routine(dbRoutine);
	}

	// add the mapping
	if(groups.size() == 1) {
		// we've got a single group so add it directly
		auto *og = new OutputMapper::OutputGroup(groups[0]);
		mapper->addMapping(og, routine);
	} else {
		// create output groups for each group
		std::vector<OutputMapper::OutputGroup *> outputGroups;

		for(auto group : groups) {
			outputGroups.push_back(new OutputMapper::OutputGroup(group));
		}

		// then create an ubergroup and add that
		auto *ug = new OutputMapper::OutputUberGroup(outputGroups);
		mapper->addMapping(ug, routine);
	}

	// if we get down here, there probably weren't any issues
	response["status"] = 0;
}
/**
 * Removes a mapping for the given group(s). If any of the given groups are in
 * an ubergroup, they're removed from that group, with the mapping being deleted
 * if that leaves an empty ubergroup.
 */
void CommandServer::clientRequestRemoveMapping(json &response, json &request) {
	// get all of the groups
	std::vector<DbGroup *> groups;

	for(int id : request["groups"]) {
		DbGroup *group = this->store->findGroupWithId(id);

		// couldn't find the group
		if(group == nullptr) {
      response["status"] = kErrorInvalidGroupId;
      response["error"] = "Couldn't find group with the specified ID";
      response["id"] = id;

			return;
		}

		groups.push_back(group);
	}

	// for each group, request it be removed
	OutputMapper *mapper = this->runner->getMapper();

	for(auto group : groups) {
		// delete the mapping
		auto *og = new OutputMapper::OutputGroup(group);
		mapper->removeMappingForGroup(og);

		delete og;
	}

	// if we get down here, there probably weren't any issues
	response["status"] = 0;
}



/**
 * Returns the brightness of a given group.
 *
 * Input variables:
 * - group: Group id
 */
void CommandServer::clientRequestGetBrightness(nlohmann::json &response, nlohmann::json &request) {
  // group id
  int groupId = request["group"];
  double brightness = request["brightness"];

  // get groups
	OutputMapper *mapper = this->runner->getMapper();

  std::vector<OutputMapper::OutputGroup *> groups;
  mapper->getAllGroups(groups);

  // find the output group
  for(auto group : groups) {
    // does the id match?
    if(group->getGroupId() == groupId) {
      response["brightness"] = group->getBrightness();
      response["status"] = 0;

      return;
    }
  }

  // if we get down here, the group could not be found
  response["status"] = kErrorInvalidGroupId;
  response["error"] = "Couldn't find group with the specified ID";
  response["id"] = groupId;
}
/**
 * Sets the brightness of a given group.
 *
 * Input variables:
 * - group: Group id
 * - brightness: Brightness value
 */
void CommandServer::clientRequestSetBrightness(nlohmann::json &response, nlohmann::json &request) {
  // group id
  int groupId = request["group"];
  double brightness = request["brightness"];

  // get groups
	OutputMapper *mapper = this->runner->getMapper();

  std::vector<OutputMapper::OutputGroup *> groups;
  mapper->getAllGroups(groups);

  // find the output group
  for(auto group : groups) {
    // does the id match?
    if(group->getGroupId() == groupId) {
      group->setBrightness(brightness);

      response["status"] = 0;

      return;
    }
  }

  // if we get down here, the group could not be found
	response["status"] = kErrorInvalidGroupId;
	response["error"] = "Couldn't find group with the specified ID";
	response["id"] = groupId;
}
