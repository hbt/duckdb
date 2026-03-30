#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/enums/lock_config.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace duckdb;

// Helper: configure a DBConfig for read-only with LOCK_CONFIG 'try'
static void SetTryLockConfig(DBConfig &cfg) {
	cfg.options.access_mode = AccessMode::READ_ONLY;
	cfg.options.lock_config = LockConfig::TRY;
}

#define BOOL_COUNT 3

TEST_CASE("Test write lock with multiple processes", "[persistence][.]") {
	uint64_t *count =
	    (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*count = 0;

	string dbdir = TestCreatePath("writelocktest");
	DeleteDatabase(dbdir);
	// test write lock
	// fork away a child
	pid_t pid = fork();
	if (pid == 0) {
		// child process
		// open db for writing
		DuckDB db(dbdir);
		Connection con(db);
		// opened db for writing
		// insert some values
		(*count)++;
		con.Query("CREATE TABLE a(i INTEGER)");
		con.Query("INSERT INTO a VALUES(42)");
		while (true) {
			con.Query("SELECT * FROM a");
			usleep(100);
		}
	} else if (pid > 0) {
		duckdb::unique_ptr<DuckDB> db;
		// parent process
		// sleep a bit to wait for child process
		while (*count == 0) {
			usleep(100);
		}
		// try to open db for writing, this should fail
		REQUIRE_THROWS(db = make_uniq<DuckDB>(dbdir));
		// kill the child
		if (kill(pid, SIGKILL) != 0) {
			FAIL();
		}
	}
}

TEST_CASE("Test read lock with multiple processes", "[persistence][.]") {
	uint64_t *count =
	    (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*count = 0;

	string dbdir = TestCreatePath("readlocktest");
	DeleteDatabase(dbdir);

	// create the database
	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (42)"));
	}
	// test read lock
	pid_t pid = fork();
	DBConfig config;
	config.options.access_mode = AccessMode::READ_ONLY;
	if (pid == 0) {
		// child process
		// open db for reading
		DuckDB db(dbdir, &config);
		Connection con(db);

		(*count)++;
		// query some values
		con.Query("SELECT i+2 FROM a");
		while (true) {
			usleep(100);
			con.Query("SELECT * FROM a");
		}
	} else if (pid > 0) {
		duckdb::unique_ptr<DuckDB> db;
		// parent process
		// sleep a bit to wait for child process
		while (*count == 0) {
			usleep(100);
		}
		// try to open db for writing, this should fail
		REQUIRE_THROWS(db = make_uniq<DuckDB>(dbdir));
		// but opening db for reading should work
		REQUIRE_NOTHROW(db = make_uniq<DuckDB>(dbdir, &config));
		// we can query the database
		Connection con(*db);
		REQUIRE_NO_FAIL(con.Query("SELECT * FROM a"));
		// kill the child
		if (kill(pid, SIGKILL) != 0) {
			FAIL();
		}
	}
}

TEST_CASE("Test read-only with try lock while writer is active", "[persistence][.]") {
	uint64_t *count =
	    (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*count = 0;

	string dbdir = TestCreatePath("trylocktest");
	DeleteDatabase(dbdir);

	// Create DB and checkpoint data
	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (42)"));
	}

	pid_t pid = fork();
	if (pid == 0) {
		// child: open for writing (holds F_WRLCK)
		DuckDB db(dbdir);
		Connection con(db);
		(*count)++;
		while (true) {
			con.Query("SELECT * FROM a");
			usleep(100);
		}
	} else if (pid > 0) {
		while (*count == 0) {
			usleep(100);
		}

		// Without LOCK_CONFIG 'try' — must still throw (regression guard)
		DBConfig ro_config;
		ro_config.options.access_mode = AccessMode::READ_ONLY;
		duckdb::unique_ptr<DuckDB> db;
		REQUIRE_THROWS(db = make_uniq<DuckDB>(dbdir, &ro_config));

		// With LOCK_CONFIG 'try' — must NOT throw
		DBConfig try_config;
		try_config.options.access_mode = AccessMode::READ_ONLY;
		try_config.options.lock_config = LockConfig::TRY;
		REQUIRE_NOTHROW(db = make_uniq<DuckDB>(dbdir, &try_config));

		// Must read last checkpoint data
		Connection con(*db);
		auto result = con.Query("SELECT i FROM a");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->GetValue(0, 0) == Value::INTEGER(42));

		kill(pid, SIGKILL);
	}
}

// Guard: LOCK_CONFIG 'try' must be rejected when access mode is not READ_ONLY.
// Verifies StorageManager::Initialize enforcement (no fork required).
TEST_CASE("Test try lock guard — requires read-only mode", "[persistence][.]") {
	string dbdir = TestCreatePath("trylockguardtest");
	DeleteDatabase(dbdir);

	{
		DuckDB db(dbdir);
	}

	// LOCK_CONFIG 'try' without READ_ONLY must throw
	DBConfig rw_try;
	rw_try.options.lock_config = LockConfig::TRY;
	// access_mode defaults to AUTOMATIC (read-write) → guard should fire
	duckdb::unique_ptr<DuckDB> db;
	REQUIRE_THROWS(db = make_uniq<DuckDB>(dbdir, &rw_try));

	// LOCK_CONFIG 'try' with explicit READ_ONLY must succeed
	DBConfig ro_try;
	SetTryLockConfig(ro_try);
	REQUIRE_NOTHROW(db = make_uniq<DuckDB>(dbdir, &ro_try));
}

// Guard: ATTACH SQL path — LOCK_CONFIG 'try' without READ_ONLY must be rejected.
// Exercises the attach option parser + StorageManager guard through SQL (no fork required).
TEST_CASE("Test ATTACH LOCK_CONFIG try requires read-only", "[persistence][.]") {
	string dbdir = TestCreatePath("trylockattachguard");
	DeleteDatabase(dbdir);

	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (7)"));
	}

	// Open an in-memory DB as the host connection
	DuckDB host(":memory:");
	Connection con(host);

	// ATTACH without READ_ONLY must fail
	auto bad = con.Query("ATTACH '" + dbdir + "' AS bad (LOCK_CONFIG 'try')");
	REQUIRE(bad->HasError());

	// ATTACH with READ_ONLY true must succeed
	auto ok = con.Query("ATTACH '" + dbdir + "' AS ok (READ_ONLY true, LOCK_CONFIG 'try')");
	REQUIRE_NO_FAIL(*ok);

	// Data must be readable through the attached DB
	auto result = con.Query("SELECT i FROM ok.a");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->GetValue(0, 0) == Value::INTEGER(7));
}

// Isolation: writer's un-checkpointed WAL data must NOT be visible to a try-lock reader.
// Verifies the WAL-skip path in SingleFileStorageManager::LoadDatabase.
TEST_CASE("Test try lock reader does not see writer WAL data", "[persistence][.]") {
	uint64_t *count =
	    (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*count = 0;

	string dbdir = TestCreatePath("trylockwaltest");
	DeleteDatabase(dbdir);

	// Checkpoint: table a = {42}
	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (42)"));
	}

	pid_t pid = fork();
	if (pid == 0) {
		// child: open for writing, insert 99 into WAL (no checkpoint)
		DuckDB db(dbdir);
		Connection con(db);
		con.Query("INSERT INTO a VALUES (99)"); // goes to WAL only
		(*count)++;
		while (true) {
			usleep(100);
		}
	} else if (pid > 0) {
		while (*count == 0) {
			usleep(100);
		}

		DBConfig try_cfg;
		SetTryLockConfig(try_cfg);
		duckdb::unique_ptr<DuckDB> db;
		REQUIRE_NOTHROW(db = make_uniq<DuckDB>(dbdir, &try_cfg));

		Connection con(*db);
		// Must see only the checkpoint row (42), not the WAL row (99)
		auto result = con.Query("SELECT i FROM a ORDER BY i");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->RowCount() == 1);
		REQUIRE(result->GetValue(0, 0) == Value::INTEGER(42));

		kill(pid, SIGKILL);
	}
}

// Concurrency: multiple independent try-lock opens while a writer is active must all succeed.
// Verifies the non-exclusive nature of the try-lock path.
TEST_CASE("Test multiple try-lock readers while writer is active", "[persistence][.]") {
	uint64_t *count =
	    (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*count = 0;

	string dbdir = TestCreatePath("trylockMultiReader");
	DeleteDatabase(dbdir);

	{
		DuckDB db(dbdir);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE a(i INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO a VALUES (42)"));
	}

	pid_t pid = fork();
	if (pid == 0) {
		// child: hold write lock
		DuckDB db(dbdir);
		Connection con(db);
		(*count)++;
		while (true) {
			usleep(100);
		}
	} else if (pid > 0) {
		while (*count == 0) {
			usleep(100);
		}

		DBConfig try_cfg;
		SetTryLockConfig(try_cfg);

		// Open two independent try-lock instances simultaneously
		duckdb::unique_ptr<DuckDB> db1, db2;
		REQUIRE_NOTHROW(db1 = make_uniq<DuckDB>(dbdir, &try_cfg));
		REQUIRE_NOTHROW(db2 = make_uniq<DuckDB>(dbdir, &try_cfg));

		Connection con1(*db1);
		Connection con2(*db2);

		auto r1 = con1.Query("SELECT i FROM a");
		auto r2 = con2.Query("SELECT i FROM a");
		REQUIRE_NO_FAIL(*r1);
		REQUIRE_NO_FAIL(*r2);
		REQUIRE(r1->GetValue(0, 0) == Value::INTEGER(42));
		REQUIRE(r2->GetValue(0, 0) == Value::INTEGER(42));

		kill(pid, SIGKILL);
	}
}
