/**
 * The data store is a simple database that's used to keep track of all state in
 * the server: the stored effect routines, available nodes, lighting groups, and
 * mapping the various sections of the framebuffer to output channels.
 */
#ifndef DATASTORE_H
#define DATASTORE_H

#include <string>
#include <vector>

#include <time.h>

#include <sqlite3.h>

#include "json.hpp"

// forward declare some classes the objects are friends with
class CommandServer;

class DataStore {
	public:
		typedef void (*CustomFunction)(DataStore *, void *);

	public:
		DataStore(std::string path);
		~DataStore();

		void commit();

		void optimize();

	public:
		void registerCustomFunction(std::string name, CustomFunction callback, void *ctx);
	private:
		friend void sqlFunctionHandler(sqlite3_context *, int, sqlite3_value **);

		class CustomFunctionCtx {
			friend class DataStore;
			friend void sqlFunctionHandler(sqlite3_context *, int, sqlite3_value **);

			private:
				std::string name;

				CustomFunction function;
				void *ctx;

				DataStore *ds;
		};

	public:
		void setInfoValue(std::string key, std::string value);
		std::string getInfoValue(std::string key);

	// types and functions relating to groups
	public:
		class Group {
			// allow access to id field by command server for JSON serialization
			friend class DataStore;
			friend class CommandServer;

			friend void to_json(nlohmann::json& j, const Group& n);

			private:
				int id = 0;

			public:
				std::string name;

				bool enabled;

				int start;
				int end;

				int currentRoutine;

			public:
				/**
				 * Returns the number of pixels this group encompasses.
				 */
				int numPixels() {
					return (this->end - this->start) + 1;
				}
		};
	public:
		DataStore::Group *findGroupWithId(int id);
		void updateGroup(DataStore::Group *group);

		std::vector<DataStore::Group *> getAllGroups();
	private:
		void _groupFromRow(sqlite3_stmt *statement, DataStore::Group *group);
		void _bindGroupToStatement(sqlite3_stmt *statement, DataStore::Group *group);

		void _createGroup(DataStore::Group *group);
		void _updateGroup(DataStore::Group *group);

		bool _groupWithIdExists(int id);

	// types and functions relating to nodes
	public:
		class Node {
			// allow access to id field by command server for JSON serialization
			friend class DataStore;
			friend class CommandServer;

			friend void to_json(nlohmann::json& j, const Node& n);

			private:
				int id = 0;

			public:
				uint32_t ip;
				uint8_t macAddr[6];

				std::string hostname;

				bool adopted;

				uint32_t hwVersion;
				uint32_t swVersion;

				time_t lastSeen;
		};
	public:
		DataStore::Node *findNodeWithMac(uint8_t mac[6]);
		void updateNode(DataStore::Node *node);

		std::vector<DataStore::Node *> getAllNodes();
	private:
		bool _nodeWithMacExists(uint8_t mac[6]);
		void _nodeFromRow(sqlite3_stmt *statement, DataStore::Node *node);
		void _bindNodeToStatement(sqlite3_stmt *statement, DataStore::Node *node);

		void _createNode(DataStore::Node *node);
		void _updateNode(DataStore::Node *node);

	private:
		void open();
		void openConfigDb();
		void checkDbVersion();
		void provisonBlankDb();
		void updateStoredServerVersion();

		void upgradeSchema();

		void close();

	private:
		std::string _stringFromColumn(sqlite3_stmt *statement, int col);

	private:
		std::string path;

		sqlite3 *db;
};

#endif
