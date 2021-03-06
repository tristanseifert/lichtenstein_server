#include "DataStore.h"

#include "version.h"

#include <nlohmann/json.hpp>

#include <glog/logging.h>

using json = nlohmann::json;

/**
 * When set, locking support is compiled into the application. It's not active
 * unless the config switch `db.serializeAccess` is set to true.
 */
// disclaimer:
// i literally don't fucking know what i'm doing but it works like 99% of
// the time but basically fuck computers and i need a new career hahahahaha
#define USE_LOCKING 1
/// set to enable logging when the locks are accessed
static const bool lockLogging = false;

#if USE_LOCKING
	// gets the thread name into the "threadName" variable
  #ifdef pthread_getname_np
  	#define LOCK_GET_THREAD_NAME() \
  		const static int threadNameSz = 64; \
  		char threadName[threadNameSz]; \
  		pthread_getname_np(pthread_self(), threadName, threadNameSz);
  #else
    #define LOCK_GET_THREAD_NAME() const char *threadName = "";
  #endif

	// checks if locking is enabled
	#define LOCK_IS_ENABLED() (this->useDbLock)

	// acquires the db lock
	#define LOCK_START() std::unique_lock<std::mutex> lk; \
		if(LOCK_IS_ENABLED()) { \
			lk = std::unique_lock<std::mutex>(this->dbLock); \
			{ \
				LOCK_GET_THREAD_NAME(); \
				if(lockLogging) VLOG(2) << "Acquired db lock from thread: " << threadName; \
		} \
	}

	// releases the db lock
	#define LOCK_END()  if(LOCK_IS_ENABLED()) { \
		lk.unlock(); { \
			LOCK_GET_THREAD_NAME(); \
			if(lockLogging) VLOG(2) << "Released db lock from thread: " << threadName; \
		} \
	}
#else
	#define LOCK_START()
	#define LOCK_END()
#endif

// include the v1 schema
const char *schema_v1 =
#include "sql/schema_v1.sql"
;

// lastest schema
const char *schema_latest = schema_v1;
const std::string latestSchemaVersion = "1";

/**
 * Default info properties that are inserted into the database after it's been
 * provisioned.
 */
const char *schema_info_default =
//	"INSERT INTO info (key, value) VALUES (\"server_build\", \"" gVERSION_HASH "/" gVERSION_BRANCH "\");"
//	"INSERT INTO info (key, value) VALUES (\"server_version\", \"" gVERSION "\");"
  ""
;

/**
 * Initializes the datastore with the persistent database located at the given
 * path. The database is attempted to be loaded – if it doesn't exist, a new
 * database is created and a schema applied.
 *
 * If there is an error during the loading process, the process will terminate.
 * It is then up to the user to remediate this issue, usually by either deleting
 * the existing database, or by fixing it manually.
 */
DataStore::DataStore(INIReader *reader) {
	// read the db path from the config
	this->config = reader;

	this->path = this->config->Get("db", "path", "");
	this->useDbLock = this->config->GetBoolean("db", "serializeAccess", false);

	// open the db
	this->open();
}

/**
 * Cleans up any resources associated with the datastore. This ensures all data
 * is safely commited to disk, compacts the database, and de-allocates any in-
 * memory structures.
 *
 * @note No further access to the database is possible after this point.
 */
DataStore::~DataStore() {
	// kill the checkpoint thread
	this->terminateCheckpointThread();

	// close the database
	this->commit();
	this->close();
}

#pragma mark - SQL Execution
/**
 * A thin wrapper around sqlite3_exec; passes the current DB instance, the given
 * SQL, and the error pointer, and returns its error code.
 */
int DataStore::sqlExec(const char *sql, char **errmsg) {
	int err = 0;

	// run the query
	LOCK_START();

	VLOG(2) << "Executing SQL: " << sql;

	err = sqlite3_exec(this->db, sql, nullptr, nullptr, errmsg);
	LOCK_END();

	return err;
}

/**
 * A thin wrapper around sqlite3_prepare_v2; passes the current DB instance, the
 * given SQL, and a pointer to an sqlite statement. Returns the error code of
 * the sqlite3_prepare_v2 function.
 */
int DataStore::sqlPrepare(const char *sql, sqlite3_stmt **stmt) {
	int err = 0;

	// prepare the query
	LOCK_START();
	err = sqlite3_prepare_v2(this->db, sql, -1, stmt, 0);


	if(err != SQLITE_OK) {
		VLOG(2) << "Created statement " << std::hex << *stmt << std::dec << " with SQL `"
				<< sql << "`, err: " << sqlite3_errstr(err);
	} else {
		VLOG(2) << "Created statement " << std::hex << *stmt << std::dec << " with SQL `"
				<< sql << "`, success";
	}

	LOCK_END();

	return err;
}

/**
 * Binds a string to a named parameter in the given statement.
 */
int DataStore::sqlBind(sqlite3_stmt *stmt, const char *param, std::string value,
                       bool optional) {
	int err, idx;

	LOCK_START();
	VLOG(2) << "Binding string `" << value << "` to parameter `" << param << '`'
			<< " on statement " << std::hex << stmt;

	// resolve the parameter
	idx = sqlite3_bind_parameter_index(stmt, param);

	if(idx == 0 && optional) {
		VLOG(2) << "Couldn't resolve optional parameter `" << param
				<< "`, ignoring error";
		return SQLITE_OK;
	}

	CHECK(idx != 0) << "Couldn't resolve parameter " << param;

	// bind
	err = sqlite3_bind_text(stmt, idx, value.c_str(), -1, SQLITE_TRANSIENT);
	LOCK_END();

	return err;
}

/**
 * Binds a blob to a named parameter in the given statement.
 */
int DataStore::sqlBind(sqlite3_stmt *stmt, const char *param, void *data, int len, bool optional) {
	int err, idx;

	LOCK_START();
	VLOG(2) << "Binding blob " << std::hex << data << ", length " << std::dec << len
			<< " bytes to parameter `" << param << '`'
			<< " on statement " << std::hex << stmt;

	// resolve the parameter
	idx = sqlite3_bind_parameter_index(stmt, param);

	if(idx == 0 && optional) {
		VLOG(2) << "Couldn't resolve optional parameter `" << param
				<< "`, ignoring error";
		return SQLITE_OK;
	}

	CHECK(idx != 0) << "Couldn't resolve parameter " << param;

	// bind
	err = sqlite3_bind_blob(stmt, idx, data, len, SQLITE_TRANSIENT);
	LOCK_END();

	return err;
}

/**
 * Binds an integer to a named parameter in the given statement.
 */
int DataStore::sqlBind(sqlite3_stmt *stmt, const char *param, int value, bool optional) {
	int err, idx;

	LOCK_START();
	VLOG(2) << "Binding integer `" << value << "` to parameter `" << param << '`'
			<< " on statement " << std::hex << stmt;

	// resolve the parameter
	idx = sqlite3_bind_parameter_index(stmt, param);

	if(idx == 0 && optional) {
		VLOG(2) << "Couldn't resolve optional parameter `" << param
				<< "`, ignoring error";
		return SQLITE_OK;
	}

	CHECK(idx != 0) << "Couldn't resolve parameter " << param;

	// bind
	err = sqlite3_bind_int(stmt, idx, value);
	LOCK_END();

	return err;
}

/**
 * Steps through the query.
 */
int DataStore::sqlStep(sqlite3_stmt *stmt) {
	int result = 0;

	LOCK_START();
	result = sqlite3_step(stmt);

	VLOG(2) << "Stepping through statement " << std::hex << stmt << std::dec << ": "
			<< result;

	LOCK_END();
	return result;
}

/**
 * Finalizes/closes a previously created statement.
 */
int DataStore::sqlFinalize(sqlite3_stmt *stmt) {
	int result = 0;

	LOCK_START();
	result = sqlite3_finalize(stmt);

	VLOG(2) << "Finalized statement " << std::hex << stmt << std::dec << ": " << result;

	LOCK_END();
	return result;
}

/**
 * Returns the rowid of the last INSERT/UPDATE operation.
 */
int DataStore::sqlGetLastRowId() {
	LOCK_START();
	int result = sqlite3_last_insert_rowid(this->db);

	LOG_IF(ERROR, result == 0) << "ROWID is zero… potential misuse of sqlGetLastRowId()";

	LOCK_END();
	return result;
}

/**
 * Returns the number of columns that the result of this statement contains.
 */
int DataStore::sqlGetNumColumns(sqlite3_stmt *statement) {
	LOCK_START();
	int numColumns = sqlite3_column_count(statement);
	LOCK_END();

	// check for errors
	CHECK(numColumns >= 0) << "Got negative result from sqlite3_column_count";

	return numColumns;
}

/**
 * Returns the value of the given column as an integer.
 */
int DataStore::sqlGetColumnInt(sqlite3_stmt *statement, int index) {
	LOCK_START();
	int result = sqlite3_column_int(statement, index);
	LOCK_END();

	return result;
}

/**
 * Returns the value of the given column as a string.
 */
std::string DataStore::sqlGetColumnString(sqlite3_stmt *statement, int index) {
	LOCK_START();

	// get the UTF-8 string and its length from sqlite, then allocate a buffer
	const unsigned char *name = sqlite3_column_text(statement, index);
	int nameLen = sqlite3_column_bytes(statement, index);

	char *nameStr = new char[nameLen + 1];
	std::fill(nameStr, nameStr + nameLen + 1, 0);

	// copy the string, but only the number of bytes sqlite returned
	memcpy(nameStr, name, nameLen);

	// create a C++ string and then deallocate the buffer
  std::string retVal = std::string(nameStr);
	delete[] nameStr;

	LOCK_END();

	return retVal;
}

/**
 * Returns a pointer to the blob value of the given column, and writes the size
 * of the blob to the integer specified.
 */
const void *DataStore::sqlGetColumnBlob(sqlite3_stmt *statement, int index, size_t &size) {
	// get the blob lmao
	LOCK_START();

	const void *blobData = sqlite3_column_blob(statement, index);
	int length = sqlite3_column_bytes(statement, index);
	CHECK(length >= 0) << "got negative length from sqlite3_column_bytes";


	// return the data
	size = length;

	LOCK_END();
	return blobData;
}

/**
 * Returns the name of the given column.
 */
std::string DataStore::sqlColumnName(sqlite3_stmt *statement, int index) {
  std::string nameStr;

	LOCK_START();
	const char *colName = sqlite3_column_name(statement, index);
  nameStr = std::string(colName);
	LOCK_END();

	return nameStr;
}

#pragma mark - Background Checkpointing
/**
 * Background checkpoint thread entry point
 */
void BackgroundCheckpointThreadEntry(void *ctx) {
#ifdef __APPLE__
	pthread_setname_np("Database Background Checkpointing");
#else
  #ifdef pthread_setname_np
    pthread_setname_np(pthread_self(), "Database Background Checkpointing");
  #endif
#endif

	DataStore *datastore = static_cast<DataStore *>(ctx);
	datastore->_checkpointThreadEntry();
}

/**
 * Checks whether the conditions for a checkpoint thread are met (WAL journal
 * mode and non-zero checkpoint interval)
 */
void DataStore::createCheckpointThread() {
	// verify journal mode
  std::string journalMode = this->config->Get("db", "journal", "WAL");

	if(journalMode != "WAL") {
		VLOG(1) << "Not creating checkpoint thread: journal mode is " << journalMode;
		return;
	}

	// verify duration
	int duration = this->config->GetInteger("db", "checkpointInterval", 0);

	if(duration <= 0) {
		VLOG(1) << "Not creating checkpoint thread: interval is " << duration;
		return;
	}

	// we're good to create the thread
	this->checkpointThread = new std::thread(BackgroundCheckpointThreadEntry, this);
}

/**
 * Entry point for the checkpoint thread. This just sits in a loop sleeping
 * until the checkpoint interval has elapsed.
 */
void DataStore::_checkpointThreadEntry() {
	int duration = this->config->GetInteger("db", "checkpointInterval", 0);
	LOG(INFO) << "Performing background checkpoint every " << duration
			  << " seconds";

	// enter a while loop: not ideal, but we can just kill the thread off
	while(1) {
		std::this_thread::sleep_until(std::chrono::system_clock::now() + std::chrono::seconds(duration));

		LOG(INFO) << "Performing background checkpoint";
		this->commit();
	}
}

/**
 * Brutally murders the checkpoint thread. We don't have a way to "wake" the
 * thread so we just get the native handle and kill it. Waiting for the
 * checkpoint lock prevents us from killing it in the middle of a checkpoint.
 */
void DataStore::terminateCheckpointThread() {
	// exit if there's no thread to kill
	if(this->checkpointThread == nullptr) {
		return;
	}

	LOG(INFO) << "Terminating checkpoint thread";

	// get the checkpoint lock; this waits until any checkpoints are done
	std::unique_lock<std::mutex> checkpointLk(this->checkpointLock);

	// platform-specific hax: get the pthread_t handle
	pthread_cancel(this->checkpointThread->native_handle());
	this->checkpointThread->join();

	// delete thread
	delete this->checkpointThread;

	// we need to release the lock again or we'll deadlock later
	checkpointLk.unlock();
}

#pragma mark - Database I/O
/**
 * Explicitly requests that the underlying storage engine commits all writes to
 * disk. This call blocks until the writes have been completed by the OS.
 *
 * This performs a "passive" checkpoint; as many frames from the write-ahead log
 * as possible are copied into the database file, WITHOUT blocking on any active
 * database readers or writers.
 */
void DataStore::commit() {
	int status = 0;

	// we need to acquire the lock
	std::unique_lock<std::mutex> checkpointLk(this->checkpointLock);

	// perform the checkpoint
	LOG(INFO) << "Performing database checkpoint";

	LOCK_START();
	status = sqlite3_wal_checkpoint_v2(this->db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
	LOCK_END();

	LOG_IF(ERROR, status != SQLITE_OK) << "Couldn't complete checkpoint: " << sqlite3_errstr(status);

	// and release the lock
	checkpointLk.unlock();
}

/**
 * Opens the sqlite database for use.
 */
void DataStore::open() {
	int status = 0;

	// use the "serialized" threading model
	status = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
	CHECK(status == SQLITE_OK) << "Couldn't set serialized thread model: " << sqlite3_errstr(status);

	int threadsafe = sqlite3_threadsafe();
	CHECK(threadsafe != 0) << "sqlite3 library isn't threadsafe: " << threadsafe;

	// get the path
	const char *path = this->path.c_str();
	LOG(INFO) << "Opening sqlite3 database at " << path;

	// attempt to open the db
	status = sqlite3_open_v2(path, &this->db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX, nullptr);
	CHECK(status == SQLITE_OK) << "Couldn't open database: " << sqlite3_errmsg(this->db);

	// apply some post-opening configurations
	this->openConfigDb();

	// check the database schema version and upgrade if needed
	this->checkDbVersion();

	// create the background checkpoint thread
	this->createCheckpointThread();
}

/**
 * Configures some pragmas on the database before it's used.
 */
void DataStore::openConfigDb() {
	int status = 0;
	char *errStr;

	static const int sqlBufSize = 256;
	char sqlBuf[sqlBufSize];

	// set incremental autovacuuming mode
	status = this->sqlExec("PRAGMA auto_vacuum=INCREMENTAL;", &errStr);
	CHECK(status == SQLITE_OK) << "Couldn't set auto_vacuum: " << errStr;

	// set UTF-8 encoding
	status = this->sqlExec("PRAGMA encoding=\"UTF-8\";", &errStr);
	CHECK(status == SQLITE_OK) << "Couldn't set encoding: " << errStr;

	// store temporary objects in memory
	status = this->sqlExec("PRAGMA temp_store=MEMORY;", &errStr);
	CHECK(status == SQLITE_OK) << "Couldn't set temp_store: " << errStr;

	// truncate the journal rather than deleting it
  std::string journalMode = this->config->Get("db", "journal", "WAL");

	snprintf(sqlBuf, sqlBufSize, "PRAGMA journal_mode=%s;", journalMode.c_str());

	status = this->sqlExec(sqlBuf, &errStr);
	CHECK(status == SQLITE_OK) << "Couldn't set journal_mode to " << journalMode
	  						   << ": " << errStr;
}

/**
 * Optimizes the database: this is typically called before the database is
 * closed. This has the effect of running the following operations:
 *
 * - Vacuums the database to remove any unneeded pages.
 * - Analyzes indices and optimizes them.
 *
 * This function may be called periodically, but it can take a significant
 * amount of time to execute.
 */
void DataStore::optimize() {
	int status = 0;
	char *errStr;

	LOG(INFO) << "Database optimization requested";

	// vacuums the database
	status = this->sqlExec("VACUUM;", &errStr);
	LOG_IF(ERROR, status != SQLITE_OK) << "Couldn't vacuum: " << errStr;

	// run analyze pragma
	status = this->sqlExec("PRAGMA optimize;", &errStr);
	LOG_IF(ERROR, status != SQLITE_OK) << "Couldn't run optimize: " << errStr;
}

/**
 * Closes the sqlite database.
 *
 * The database _should_ always close, unless there are any open statements. If
 * you get a "couldn't close database" error, it is most likely a bug in the
 * server code, since statements should always be closed.
 */
void DataStore::close() {
	int status = 0;

	LOG(INFO) << "Closing sqlite database";

	// optimize the database before closing
	this->optimize();

	// attempt to close the db
	LOCK_START();

	status = sqlite3_close(this->db);
	LOG_IF(ERROR, status != SQLITE_OK) << "Couldn't close database, data loss may result!";
	VLOG_IF(1, status == SQLITE_OK) << "Database has been closed, no further access is possible";

	LOCK_END();
}

#pragma mark - Schema Version Management
/**
 * Checks the version of the database schema, upgrading it if needed. If the
 * database is empty, this applies the most up-to-date version of the schema.
 */
void DataStore::checkDbVersion() {
	int err = 0, result;
	sqlite3_stmt *statement = nullptr;

	// first, check if the table exists
	err = this->sqlPrepare( "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='info';", &statement);
	CHECK(err == SQLITE_OK) << "Couldn't prepare statement: " << sqlite3_errstr(err);

	result = this->sqlStep(statement);

	if(result == SQLITE_ROW) {
		// retrieve the value of the first column (0-based)
		int count = sqlite3_column_int(statement, 0);

		if(count == 0) {
			LOG(INFO) << "Couldn't find info table, assuming db needs provisioning";

			this->provisonBlankDb();
		}
	}

	// free our statement
	this->sqlFinalize(statement);


	// the database has a schema, we just have to find out what version…
  std::string schemaVersion = this->getInfoValue("schema_version");
	LOG(INFO) << "Schema version: " << schemaVersion;

	// is the schema version the same as that of the latest schema?
	if(schemaVersion != latestSchemaVersion) {
		this->upgradeSchema();
	}

	// log info about what server version last accessed the db
	LOG(INFO) << "Last accessed with version " << this->getInfoValue("server_version") << ", build " << this->getInfoValue("server_build");

	// update the server version that's stored in the DB
	this->updateStoredServerVersion();
}

/**
 * If the schema version is undefined (i.e. the database was just newly created)
 * we apply the schema here and insert some default values.
 */
void DataStore::provisonBlankDb() {
	int status = 0;
	char *errStr;

	// execute the entire schema string in one go. if this fails we're fucked
	status = this->sqlExec(schema_latest, &errStr);
	LOG_IF(ERROR, status != SQLITE_OK) << "Couldn't run schema: " << errStr;

	// set the default values in the info table
	status = this->sqlExec(schema_info_default, &errStr);
	LOG_IF(ERROR, status != SQLITE_OK) << "Couldn't set info default values: " << errStr;

	// force a checkpoint
	this->commit();
}

/**
 * Performs incremental upgrades from the current schema version to the next
 * highest version until we reach the latest version.
 */
void DataStore::upgradeSchema() {
  std::string schemaVersion = this->getInfoValue("schema_version");
	LOG(INFO) << "Latest schema version is " << latestSchemaVersion << ", db is"
			  << " currently on version " << schemaVersion << "; upgrade required";
}

#pragma mark - Function Binding
/**
 * Static callback function called for every custom-defined function by sqlite;
 * this inspects the context (i.e. the CustomFunctionCtx class) and then calls
 * that function.
 */
void sqlFunctionHandler(sqlite3_context *ctx, int numValues, sqlite3_value **values) {
	DataStore::CustomFunctionCtx *cfn;

	// get the context back
	void *ctxClass = sqlite3_user_data(ctx);
	cfn = static_cast<DataStore::CustomFunctionCtx *>(ctxClass);

	// invoke the function
	cfn->function(cfn->ds, cfn->ctx);
}

/**
 * Registers a native function that's called when an SQL function with `name` is
 * called. This is useful for things like triggers.
 */
void
DataStore::registerCustomFunction(std::string name, CustomFunction callback,
                                  void *ctx) {
	int err;

	// allocate a context parameter
	CustomFunctionCtx *sqliteCtx = new CustomFunctionCtx();
	sqliteCtx->function = callback;
	sqliteCtx->name = name;

	sqliteCtx->ds = this;
	sqliteCtx->ctx = ctx;

	// register the function with sqlite
	LOCK_START();

	err = sqlite3_create_function(this->db, name.c_str(), 0, SQLITE_UTF8, sqliteCtx,
								  sqlFunctionHandler, nullptr, nullptr);

	CHECK(err == SQLITE_OK) << "Couldn't register function: " << sqlite3_errstr(err);

	LOCK_END();
}

#pragma mark - Metadata

/**
 * Updates the server version stored in the db.
 */
void DataStore::updateStoredServerVersion() {
  this->setInfoValue("server_build", std::string(gVERSION_HASH) + "/" +
                                     std::string(gVERSION_BRANCH));
  this->setInfoValue("server_version", std::string(gVERSION));
}

/**
 * Sets a DB metadata key to the specified value. This key must be defined in
 * the initial schema so that the update clause can run.
 */
void DataStore::setInfoValue(std::string key, std::string value) {
	int err = 0, result;
	sqlite3_stmt *statement = nullptr;

	// prepare an update query
	err = this->sqlPrepare("UPDATE info SET value = :value WHERE key = :key;", &statement);
	CHECK(err == SQLITE_OK) << "Couldn't prepare statement: " << sqlite3_errstr(err);

	// bind the value
	err = this->sqlBind(statement, ":value", value);
	CHECK(err == SQLITE_OK) << "Couldn't bind value: " << sqlite3_errstr(err);

	// bind the key
	err = this->sqlBind(statement, ":key", key);
	CHECK(err == SQLITE_OK) << "Couldn't bind key: " << sqlite3_errstr(err);

	// execute query
	result = this->sqlStep(statement);
	CHECK(result == SQLITE_DONE) << "Couldn't execute query: " << sqlite3_errstr(result);

	// free the statement
	this->sqlFinalize(statement);
}

/**
 * Returns the value of the given database metadata key. If the key does not
 * exist, an error is raised.
 */
std::string DataStore::getInfoValue(std::string key) {
	int err = 0, result;
	sqlite3_stmt *statement = nullptr;
  std::string returnValue;

	// prepare a query and bind key
	err = this->sqlPrepare("SELECT value FROM info WHERE key = :key;", &statement);
	CHECK(err == SQLITE_OK) << "Couldn't prepare statement: " << sqlite3_errstr(err);

	err = this->sqlBind(statement, ":key", key);
	CHECK(err == SQLITE_OK) << "Couldn't bind key: " << sqlite3_errstr(err);

	// execute the query
	result = this->sqlStep(statement);

	if(result == SQLITE_ROW) {
		const unsigned char *value = sqlite3_column_text(statement, 0);
    returnValue = std::string(reinterpret_cast<const char *>(value));
	}

	// free the statement
	this->sqlFinalize(statement);

	// return a C++ string
	return returnValue;
}
