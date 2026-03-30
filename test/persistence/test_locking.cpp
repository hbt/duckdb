#include "catch.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/enums/lock_config.hpp"
#include "duckdb.hpp"
#include "test_helpers.hpp"

#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>

using namespace duckdb;

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
