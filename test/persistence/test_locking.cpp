#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"

#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace duckdb;
using namespace std;

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

// Test 2.1: LOCK_CONFIG 'try' requires READ_ONLY mode (no fork needed).
TEST_CASE("Test try lock guard requires read-only mode", "[persistence]") {
	string dbpath = TestCreatePath("trylock_guard");
	DeleteDatabase(dbpath);
	// Create the database first.
	{
		DuckDB db(dbpath);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
	}

	// Opening with LOCK_CONFIG TRY but without READ_ONLY must throw.
	DBConfig config;
	config.options.lock_config = LockConfig::TRY;
	// access_mode defaults to AUTOMATIC (not READ_ONLY) — must throw InvalidInputException.
	REQUIRE_THROWS_AS(DuckDB(dbpath, &config), duckdb::InvalidInputException);
}

// Test 2.2: SQL ATTACH path — LOCK_CONFIG 'try' requires READ_ONLY.
TEST_CASE("Test ATTACH LOCK_CONFIG try requires read-only", "[persistence]") {
	string dbpath = TestCreatePath("trylock_attach");
	DeleteDatabase(dbpath);
	{
		DuckDB db(dbpath);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
	}

	DuckDB db(nullptr);
	Connection con(db);
	// Without READ_ONLY must fail.
	REQUIRE_FAIL(con.Query("ATTACH '" + dbpath + "' AS db1 (LOCK_CONFIG 'try')"));
	// With READ_ONLY must succeed.
	REQUIRE_NO_FAIL(con.Query("ATTACH '" + dbpath + "' AS db2 (READ_ONLY true, LOCK_CONFIG 'try')"));
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM db2.t"));
}

// Test 2.3: WAL isolation — try-lock reader must NOT see uncommitted WAL rows.
// Uses [.] to opt out of default test runs (requires fork).
TEST_CASE("Test try lock reader skips WAL replay", "[persistence][.]") {
	uint64_t *ready = (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*ready = 0;

	string dbpath = TestCreatePath("trylock_wal_isolation");
	DeleteDatabase(dbpath);

	// Create the DB and insert a checkpointed row.
	{
		DuckDB db(dbpath);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES(1)"));
		// Explicit checkpoint to flush to storage.
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}

	pid_t pid = fork();
	if (pid == 0) {
		// Child: open writer, insert a row (WAL only, no checkpoint), then signal ready.
		DuckDB db(dbpath);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES(2)"));
		(*ready)++;
		while (true) {
			usleep(1000);
		}
	} else if (pid > 0) {
		while (*ready == 0) {
			usleep(100);
		}
		// Parent: open with TRY lock — must only see checkpointed row (i=1), not WAL row (i=2).
		DBConfig config;
		config.options.access_mode = AccessMode::READ_ONLY;
		config.options.lock_config = LockConfig::TRY;
		{
			DuckDB db2(dbpath, &config);
			Connection con2(db2);
			auto result = con2.Query("SELECT i FROM t ORDER BY i");
			REQUIRE_NO_FAIL(*result);
			REQUIRE(result->RowCount() == 1);
			REQUIRE(result->GetValue(0, 0) == Value(1));
		}
		if (kill(pid, SIGKILL) != 0) {
			FAIL();
		}
	}
}

// Test 2.4: Multiple concurrent try-lock readers while writer holds F_WRLCK.
// Uses [.] to opt out of default test runs (requires fork).
TEST_CASE("Test multiple concurrent try-lock readers", "[persistence][.]") {
	uint64_t *ready = (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	uint64_t *r1_ok = (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	uint64_t *r2_ok = (uint64_t *)mmap(NULL, sizeof(uint64_t), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_SHARED, 0, 0);
	*ready = 0; *r1_ok = 0; *r2_ok = 0;

	string dbpath = TestCreatePath("trylock_multi_reader");
	DeleteDatabase(dbpath);

	{
		DuckDB db(dbpath);
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE t(i INT)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO t VALUES(42)"));
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}

	// Writer process (holds F_WRLCK).
	pid_t writer_pid = fork();
	if (writer_pid == 0) {
		DuckDB db(dbpath);
		(*ready)++;
		while (true) {
			usleep(1000);
		}
	}

	// Wait for writer to be ready.
	while (*ready == 0) {
		usleep(100);
	}

	DBConfig ro_try_config;
	ro_try_config.options.access_mode = AccessMode::READ_ONLY;
	ro_try_config.options.lock_config = LockConfig::TRY;

	// Reader 1.
	pid_t r1_pid = fork();
	if (r1_pid == 0) {
		{
			DuckDB db(dbpath, &ro_try_config);
			Connection con(db);
			REQUIRE_NO_FAIL(con.Query("SELECT * FROM t"));
		}
		(*r1_ok)++;
		_exit(0);
	}

	// Reader 2.
	pid_t r2_pid = fork();
	if (r2_pid == 0) {
		{
			DuckDB db(dbpath, &ro_try_config);
			Connection con(db);
			REQUIRE_NO_FAIL(con.Query("SELECT * FROM t"));
		}
		(*r2_ok)++;
		_exit(0);
	}

	// Wait for both readers.
	int status;
	waitpid(r1_pid, &status, 0);
	waitpid(r2_pid, &status, 0);

	REQUIRE(*r1_ok == 1);
	REQUIRE(*r2_ok == 1);

	if (kill(writer_pid, SIGKILL) != 0) {
		FAIL();
	}
}
