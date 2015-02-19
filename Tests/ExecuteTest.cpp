#include "../Analyzer/Analyzer.h"
#include "../Catalog/Catalog.h"
#include "../DataMgr/DataMgr.h"
#include "../Parser/parser.h"
#include "../Parser/ParserNode.h"
#include "../Planner/Planner.h"
#include "../QueryEngine/Execute.h"
#include "../SqliteConnector/SqliteConnector.h"

#include <boost/filesystem.hpp>
#include <gtest/gtest.h>

#include <string>
#include <cstdlib>
#include <memory>


using namespace std;

namespace {

Catalog_Namespace::Catalog get_catalog() {
  string db_name { MAPD_SYSTEM_DB };
  string user_name { "mapd" };
  string passwd { "HyperInteractive" };
  boost::filesystem::path base_path { "/tmp" };
  CHECK(boost::filesystem::exists(base_path));
  auto system_db_file = base_path / "mapd_catalogs" / "mapd";
  CHECK(boost::filesystem::exists(system_db_file));
	auto data_dir = base_path / "mapd_data";
	Data_Namespace::DataMgr *dataMgr = new Data_Namespace::DataMgr(data_dir.string());
  Catalog_Namespace::SysCatalog sys_cat(base_path.string(), *dataMgr);
  Catalog_Namespace::UserMetadata user;
  CHECK(sys_cat.getMetadataForUser(user_name, user));
  CHECK_EQ(user.passwd, passwd);
  Catalog_Namespace::DBMetadata db;
  CHECK(sys_cat.getMetadataForDB(db_name, db));
  CHECK(user.isSuper || (user.userId == db.dbOwner));
  Catalog_Namespace::Catalog cat(base_path.string(), user, db, *dataMgr);
  return cat;
}

Catalog_Namespace::Catalog g_cat(get_catalog());

vector<ResultRow> run_multiple_agg(
    const string& query_str,
    const ExecutorDeviceType device_type,
    std::shared_ptr<Executor>& executor) {
  SQLParser parser;
  list<Parser::Stmt*> parse_trees;
  string last_parsed;
  CHECK_EQ(parser.parse(query_str, parse_trees, last_parsed), 0);
  CHECK_EQ(parse_trees.size(), 1);
  auto stmt = parse_trees.front();
  unique_ptr<Stmt> stmt_ptr(stmt); // make sure it's deleted
  Parser::DDLStmt *ddl = dynamic_cast<Parser::DDLStmt *>(stmt);
  CHECK(!ddl);
  Parser::DMLStmt *dml = dynamic_cast<Parser::DMLStmt*>(stmt);
  Analyzer::Query query;
  dml->analyze(g_cat, query);
  Planner::Optimizer optimizer(query, g_cat);
  Planner::RootPlan *plan = optimizer.optimize();
  unique_ptr<Planner::RootPlan> plan_ptr(plan); // make sure it's deleted
  executor = std::make_shared<Executor>(g_cat.get_currentDB().dbId);
  return executor->execute(plan, device_type, ExecutorOptLevel::LoopStrengthReduction);
}

AggResult run_simple_agg(
    const string& query_str,
    const ExecutorDeviceType device_type,
    std::shared_ptr<Executor>& executor) {
  return run_multiple_agg(query_str, device_type, executor).front().agg_result(0);
}

template<class T>
T v(const AggResult& r) {
  auto p = boost::get<T>(&r);
  CHECK(p);
  return *p;
}

void run_ddl_statement(const string& create_table_stmt) {
  SQLParser parser;
  list<Parser::Stmt*> parse_trees;
  string last_parsed;
  CHECK_EQ(parser.parse(create_table_stmt, parse_trees, last_parsed), 0);
  CHECK_EQ(parse_trees.size(), 1);
  auto stmt = parse_trees.front();
  unique_ptr<Stmt> stmt_ptr(stmt); // make sure it's deleted
  Parser::DDLStmt *ddl = dynamic_cast<Parser::DDLStmt *>(stmt);
  CHECK(ddl);
  if ( ddl != nullptr)
    ddl->execute(g_cat);
}

bool skip_tests(const ExecutorDeviceType device_type) {
  return device_type == ExecutorDeviceType::GPU && !g_cat.get_dataMgr().gpusPresent();
}

bool approx_eq(const double v, const double target, const double eps = 0.01) {
  return target - eps < v && v < target + eps;
}

class SQLiteComparator {
public:
  SQLiteComparator() : connector_("main", "") {}

  void query(const std::string& query_string) {
    connector_.query(query_string);
  }

  void compare(const std::string& query_string, const ExecutorDeviceType device_type) {
    connector_.query(query_string);
    std::shared_ptr<Executor> executor;
    const auto mapd_results = run_multiple_agg(query_string, device_type, executor);
    ASSERT_EQ(connector_.getNumRows(), mapd_results.size());
    const int num_rows { static_cast<int>(connector_.getNumRows()) };
    if (mapd_results.empty()) {
      ASSERT_EQ(0, num_rows);
      return;
    }
    ASSERT_EQ(connector_.getNumCols(), mapd_results.front().size());
    const int num_cols { static_cast<int>(connector_.getNumCols()) };
    for (int row_idx = 0; row_idx < num_rows; ++row_idx) {
      for (int col_idx = 0; col_idx < num_cols; ++col_idx) {
        const auto ref_col_type = connector_.columnTypes[col_idx];
        const auto mapd_variant = mapd_results[row_idx].agg_result(col_idx);
        switch (ref_col_type) {
        case SQLITE_INTEGER: {
          const auto ref_val = connector_.getData<int64_t>(row_idx, col_idx);
          const auto mapd_as_int_p = boost::get<int64_t>(&mapd_variant);
          ASSERT_NE(nullptr, mapd_as_int_p);
          const auto mapd_val = *mapd_as_int_p;
          ASSERT_EQ(ref_val, mapd_val);
          break;
        }
        case SQLITE_FLOAT: {
          const auto ref_val = connector_.getData<double>(row_idx, col_idx);
          const auto mapd_as_double_p = boost::get<double>(&mapd_variant);
          ASSERT_NE(nullptr, mapd_as_double_p);
          const auto mapd_val = *mapd_as_double_p;
          ASSERT_TRUE(approx_eq(ref_val, mapd_val));
          break;
        }
        case SQLITE_TEXT: {
          const auto ref_val = connector_.getData<std::string>(row_idx, col_idx);
          const auto mapd_as_str_p = boost::get<std::string>(&mapd_variant);
          ASSERT_NE(nullptr, mapd_as_str_p);
          const auto mapd_val = *mapd_as_str_p;
          ASSERT_EQ(ref_val, mapd_val);
          break;
        }
        case SQLITE_NULL:
          break;
        default:
          CHECK(false);
        }
      }
    }
  }
private:
  SqliteConnector connector_;
};

const ssize_t g_num_rows { 10 };
SQLiteComparator g_sqlite_comparator;

void c(const std::string& query_string, const ExecutorDeviceType device_type) {
  g_sqlite_comparator.compare(query_string, device_type);
}

}

#define SKIP_NO_GPU()                                        \
  if (skip_tests(dt)) {                                      \
    CHECK(dt == ExecutorDeviceType::GPU);                    \
    LOG(WARNING) << "GPU not available, skipping GPU tests"; \
    continue;                                                \
  }

TEST(Select, FilterAndSimpleAggregation) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT COUNT(*) FROM test;", dt);
    c("SELECT MIN(x) FROM test;", dt);
    c("SELECT MAX(x) FROM test;", dt);
    c("SELECT MIN(z) FROM test;", dt);
    c("SELECT MAX(z) FROM test;", dt);
    c("SELECT MIN(t) FROM test;", dt);
    c("SELECT MAX(t) FROM test;", dt);
    c("SELECT SUM(x + y) FROM test;", dt);
    c("SELECT SUM(x + y + z) FROM test;", dt);
    c("SELECT SUM(x + y + z + t) FROM test;", dt);
    c("SELECT COUNT(*) FROM test WHERE x > 6 AND x < 8;", dt);
    c("SELECT COUNT(*) FROM test WHERE x > 6 AND x < 8 AND z > 100 AND z < 102;", dt);
    c("SELECT COUNT(*) FROM test WHERE x > 6 AND x < 8 OR (z > 100 AND z < 103);", dt);
    c("SELECT COUNT(*) FROM test WHERE x > 6 AND x < 8 AND z > 100 AND z < 102 AND t > 1000 AND t < 1002;", dt);
    c("SELECT COUNT(*) FROM test WHERE x > 6 AND x < 8 OR (z > 100 AND z < 103);", dt);
    c("SELECT COUNT(*) FROM test WHERE x > 6 AND x < 8 OR (z > 100 AND z < 102) OR (t > 1000 AND t < 1003);", dt);
    c("SELECT COUNT(*) FROM test WHERE x <> 7;", dt);
    c("SELECT COUNT(*) FROM test WHERE z <> 102;", dt);
    c("SELECT COUNT(*) FROM test WHERE t <> 1002;", dt);
    c("SELECT COUNT(*) FROM test WHERE x + y = 49;", dt);
    c("SELECT COUNT(*) FROM test WHERE x + y + z = 150;", dt);
    c("SELECT COUNT(*) FROM test WHERE x + y + z + t = 1151;", dt);
    c("SELECT SUM(x + y) FROM test WHERE x + y = 49;", dt);
    c("SELECT SUM(x + y + z) FROM test WHERE x + y = 49;", dt);
    c("SELECT SUM(x + y + z + t) FROM test WHERE x + y = 49;", dt);
    c("SELECT COUNT(*) FROM test WHERE x - y = -35;", dt);
    c("SELECT COUNT(*) FROM test WHERE x - y + z = 66;", dt);
    c("SELECT COUNT(*) FROM test WHERE x - y + z + t = 1067;", dt);
    c("SELECT COUNT(*) FROM test WHERE y - x = 35;", dt);
    c("SELECT SUM(2 * x) FROM test WHERE x = 7;", dt);
    c("SELECT SUM(2 * x + z) FROM test WHERE x = 7;", dt);
    c("SELECT SUM(x + y) FROM test WHERE x - y = -35;", dt);
    c("SELECT SUM(x + y) FROM test WHERE y - x = 35;", dt);
    c("SELECT SUM(x + y - z) FROM test WHERE y - x = 35;", dt);
    c("SELECT SUM(x * y + 15) FROM test WHERE x + y + 1 = 50;", dt);
    c("SELECT SUM(x * y + 15) FROM test WHERE x + y + z + 1 = 151;", dt);
    c("SELECT SUM(x * y + 15) FROM test WHERE x + y + z + t + 1 = 1152;", dt);
    c("SELECT MIN(x * y + 15) FROM test WHERE x + y + 1 = 50;", dt);
    c("SELECT MIN(x * y + 15) FROM test WHERE x + y + z + 1 = 151;", dt);
    c("SELECT MIN(x * y + 15) FROM test WHERE x + y + z + t + 1 = 1152;", dt);
    c("SELECT MAX(x * y + 15) FROM test WHERE x + y + 1 = 50;", dt);
    c("SELECT MAX(x * y + 15) FROM test WHERE x + y + z + 1 = 151;", dt);
    c("SELECT MAX(x * y + 15) FROM test WHERE x + y + z + t + 1 = 1152;", dt);
    c("SELECT MIN(x) FROM test WHERE x = 7;", dt);
    c("SELECT MIN(z) FROM test WHERE z = 101;", dt);
    c("SELECT MIN(t) FROM test WHERE t = 1001;", dt);
    c("SELECT AVG(x + y) FROM test;", dt);
    c("SELECT AVG(x + y + z) FROM test;", dt);
    c("SELECT AVG(x + y + z + t) FROM test;", dt);
    c("SELECT AVG(y) FROM test WHERE x > 6 AND x < 8;", dt);
    c("SELECT AVG(y) FROM test WHERE x > 6 AND x < 8;", dt);
    c("SELECT AVG(y) FROM test WHERE z > 100 AND z < 102;", dt);
    c("SELECT AVG(y) FROM test WHERE t > 1000 AND t < 1002;", dt);
    std::shared_ptr<Executor> executor;
    ASSERT_EQ(v<int64_t>(run_simple_agg("SELECT MIN(x) FROM test WHERE x <> 7 AND x <> 8;", dt, executor)), numeric_limits<int64_t>::max());
    ASSERT_EQ(v<int64_t>(run_simple_agg("SELECT MIN(x) FROM test WHERE z <> 101 AND z <> 102;", dt, executor)), numeric_limits<int64_t>::max());
    ASSERT_EQ(v<int64_t>(run_simple_agg("SELECT MIN(x) FROM test WHERE t <> 1001 AND t <> 1002;", dt, executor)), numeric_limits<int64_t>::max());
  }
}

TEST(Select, FloatAndDoubleTests) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT MIN(f) FROM test;", dt);
    c("SELECT MAX(f) FROM test;", dt);
    c("SELECT AVG(f) FROM test;", dt);
    c("SELECT MIN(d) FROM test;", dt);
    c("SELECT MAX(d) FROM test;", dt);
    c("SELECT AVG(d) FROM test;", dt);
    c("SELECT SUM(f) FROM test;", dt);
    c("SELECT SUM(d) FROM test;", dt);
    c("SELECT SUM(f + d) FROM test;", dt);
    c("SELECT AVG(x * f) FROM test;", dt);
    c("SELECT COUNT(*) FROM test WHERE f > 1.0 AND f < 1.2;", dt);
    c("SELECT COUNT(*) FROM test WHERE f > 1.1 AND f < 1.3;", dt);
    c("SELECT COUNT(*) FROM test WHERE f > 1.2 AND f < 1.4;", dt);
    c("SELECT COUNT(*) FROM test WHERE f > 1.0 AND f < 1.2 AND d > 2.0 AND d < 2.4;", dt);
    c("SELECT COUNT(*) FROM test WHERE f > 1.0 AND f < 1.2 OR (d > 2.0 AND d < 3.0);", dt);
    c("SELECT SUM(x + y) FROM test WHERE f > 1.0 AND f < 1.2;", dt);
    c("SELECT SUM(x + y) FROM test WHERE d + f > 3.0 AND d + f < 4.0;", dt);
    c("SELECT SUM(f + d) FROM test WHERE x - y = -35;", dt);
    c("SELECT SUM(f + d) FROM test WHERE x + y + 1 = 50;", dt);
    c("SELECT SUM(f * d + 15) FROM test WHERE x + y + 1 = 50;", dt);
    c("SELECT MIN(x), AVG(x * y), MAX(y + 7), AVG(x * f + 15), COUNT(*) FROM test WHERE x + y > 47 AND x + y < 51;", dt);
    c("SELECT AVG(f), MAX(y) FROM test WHERE x = 7 GROUP BY z HAVING AVG(y) > 42.0;", dt);
    c("SELECT AVG(f), MAX(y) FROM test WHERE x = 7 GROUP BY z HAVING AVG(f) > 1.09;", dt);
    c("SELECT AVG(f), MAX(y) FROM test WHERE x = 7 GROUP BY z HAVING AVG(f) > 1.09 AND AVG(y) > 42.0;", dt);
    c("SELECT AVG(d), MAX(y) FROM test WHERE x = 7 GROUP BY z HAVING AVG(d) > 2.2 AND AVG(y) > 42.0;", dt);
    c("SELECT AVG(f), MAX(y) FROM test WHERE x = 7 GROUP BY z HAVING AVG(d) > 2.2 AND AVG(y) > 42.0;", dt);
    c("SELECT AVG(f) + AVG(d), MAX(y) FROM test WHERE x = 7 GROUP BY z HAVING AVG(f) + AVG(d) > 3.0;", dt);
    c("SELECT AVG(f) + AVG(d), MAX(y) FROM test WHERE x = 7 GROUP BY z HAVING AVG(f) + AVG(d) > 3.5;", dt);
    c("SELECT f + d AS s, x * y FROM test ORDER by s DESC;", dt);
    c("SELECT COUNT(*) FROM test GROUP BY f;", dt);
    c("SELECT COUNT(*) FROM test GROUP BY d;", dt);
    c("SELECT MIN(x + y) FROM test WHERE x + y > 47 AND x + y < 53 GROUP BY f + 1, f + d;", dt);
    c("SELECT f + d AS s FROM test GROUP BY s ORDER BY s DESC;", dt);
  }
}

TEST(Select, FilterAndMultipleAggregation) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT AVG(x), AVG(y) FROM test;", dt);
    c("SELECT MIN(x), AVG(x * y), MAX(y + 7), COUNT(*) FROM test WHERE x + y > 47 AND x + y < 51;", dt);
  }
}

TEST(Select, FilterAndGroupBy) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT MIN(x + y) FROM test WHERE x + y > 47 AND x + y < 53 GROUP BY x, y;", dt);
    c("SELECT MIN(x + y) FROM test WHERE x + y > 47 AND x + y < 53 GROUP BY x + 1, x + y;", dt);
    c("SELECT x, y, COUNT(*) FROM test GROUP BY x, y;", dt);
  }
}

TEST(Select, FilterAndGroupByMultipleAgg) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT MIN(x + y), COUNT(*), AVG(x + 1) FROM test WHERE x + y > 47 AND x + y < 53 GROUP BY x, y;", dt);
    c("SELECT MIN(x + y), COUNT(*), AVG(x + 1) FROM test WHERE x + y > 47 AND x + y < 53 GROUP BY x + 1, x + y;", dt);
  }
}

TEST(Select, Having) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT MAX(y) FROM test WHERE x = 7 GROUP BY z HAVING MAX(x) > 5;", dt);
    c("SELECT MAX(y) FROM test WHERE x > 7 GROUP BY z HAVING MAX(x) < 100;", dt);
    c("SELECT z, SUM(y) FROM test WHERE x > 6 GROUP BY z HAVING MAX(x) < 100;", dt);
    c("SELECT z, SUM(y) FROM test WHERE x > 6 GROUP BY z HAVING MAX(x) < 100 AND COUNT(*) > 5;", dt);
    c("SELECT z, SUM(y) FROM test WHERE x > 6 GROUP BY z HAVING MAX(x) < 100 AND COUNT(*) > 9;", dt);
  }
}

TEST(Select, CountDistinct) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT COUNT(*), MIN(x), MAX(x), AVG(y), SUM(z), COUNT(distinct x) FROM test GROUP BY y;", dt);
  }
}

TEST(Select, ScanNoAggregation) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT * FROM test;", dt);
    c("SELECT t.* FROM test t;", dt);
    c("SELECT x, z, t FROM test;", dt);
    c("SELECT x + z, t FROM test WHERE x <> 7 AND y > 42;", dt);
    c("SELECT * FROM test WHERE x > 8;", dt);
  }
}

TEST(Select, OrderBy) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    std::shared_ptr<Executor> executor;
    const auto rows = run_multiple_agg(
      "SELECT x, y, z + t, x * y as m FROM test ORDER BY 3 desc LIMIT 5;",
      dt, executor);
    CHECK_EQ(rows.size(), std::min(5, static_cast<int>(g_num_rows)));
    for (const auto& row : rows) {
      CHECK_EQ(row.size(), 4);
      ASSERT_TRUE(v<int64_t>(row.agg_result(0)) == 8 || v<int64_t>(row.agg_result(0)) == 7);
      ASSERT_EQ(v<int64_t>(row.agg_result(1)), 43);
      ASSERT_EQ(v<int64_t>(row.agg_result(2)), 1104);
      ASSERT_TRUE(v<int64_t>(row.agg_result(3)) == 344 || v<int64_t>(row.agg_result(3)) == 301);
    }
  }
}

TEST(Select, ComplexQueries) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT COUNT(*) * MAX(y) - SUM(z) FROM test;", dt);
    c("SELECT x + y AS a, COUNT(*) * MAX(y) - SUM(z) AS b FROM test WHERE z BETWEEN 100 AND 200 GROUP BY x, y;", dt);
    c("SELECT x + y AS a, COUNT(*) * MAX(y) - SUM(z) AS b FROM test WHERE z BETWEEN 100 AND 200 "
      "GROUP BY x, y HAVING y > 2 * x AND MIN(y) > MAX(x);", dt);
    c("SELECT x + y AS a, COUNT(*) * MAX(y) - SUM(z) AS b FROM test WHERE z BETWEEN 100 AND 200 "
      "GROUP BY x, y HAVING y > 2 * x AND MIN(y) > MAX(x) + 35;", dt);
    c("SELECT x + y AS a, COUNT(*) * MAX(y) - SUM(z) AS b FROM test WHERE z BETWEEN 100 AND 200 "
      "GROUP BY x, y HAVING y > 2 * x AND MIN(y) > MAX(x) + 36;", dt);
    c("SELECT x + y AS a, COUNT(*) * MAX(y) - SUM(z) AS b FROM test "
      "WHERE z BETWEEN 100 AND 200 GROUP BY a, y;", dt);
    std::shared_ptr<Executor> executor;
    const auto rows = run_multiple_agg(
      "SELECT x + y AS a, COUNT(*) * MAX(y) - SUM(z) AS b FROM test "
      "WHERE z BETWEEN 100 AND 200 GROUP BY x, y ORDER BY a DESC LIMIT 2;",
      dt, executor);
    ASSERT_EQ(rows.size(), 2);
    {
      const auto row = rows[0];
      ASSERT_EQ(v<int64_t>(row.agg_result(0)), 51);
      ASSERT_EQ(v<int64_t>(row.agg_result(1)), -59 * g_num_rows / 2);
    }
    {
      const auto row = rows[1];
      ASSERT_EQ(v<int64_t>(row.agg_result(0)), 50);
      ASSERT_EQ(v<int64_t>(row.agg_result(1)), -59 * g_num_rows / 2);
    }
  }
}

TEST(Select, GroupByExprNoFilterNoAggregate) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT x + y AS a FROM test GROUP BY a;", dt);
  }
}

TEST(Select, Case) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT SUM(CASE WHEN x BETWEEN 6 AND 7 THEN 1 WHEN x BETWEEN 8 AND 9 THEN 2 ELSE 3 END) FROM test;", dt);
    c("SELECT SUM(CASE WHEN x BETWEEN 6 AND 7 THEN 1 WHEN x BETWEEN 8 AND 9 THEN 2 ELSE 3 END) "
      "FROM test WHERE CASE WHEN y BETWEEN 42 AND 43 THEN 5 ELSE 4 END > 4;", dt);
    c("SELECT SUM(CASE WHEN x BETWEEN 6 AND 7 THEN 1 WHEN x BETWEEN 8 AND 9 THEN 2 ELSE 3 END) "
      "FROM test WHERE CASE WHEN y BETWEEN 44 AND 45 THEN 5 ELSE 4 END > 4;", dt);
    c("SELECT CASE WHEN x + y > 50 THEN 77 ELSE 88 END AS foo, COUNT(*) FROM test GROUP BY foo;", dt);
  }
}

TEST(Select, Strings) {
  for (auto dt : { ExecutorDeviceType::CPU, ExecutorDeviceType::GPU }) {
    SKIP_NO_GPU();
    c("SELECT str, COUNT(*) FROM test GROUP BY str HAVING COUNT(*) > 5;", dt);
    c("SELECT str, COUNT(*) FROM test WHERE str = 'bar' GROUP BY str HAVING COUNT(*) > 4;", dt);
    c("SELECT str, COUNT(*) FROM test WHERE str = 'bar' GROUP BY str HAVING COUNT(*) > 5;", dt);
    c("SELECT str, COUNT(*) FROM test GROUP BY str ORDER BY str;", dt);
  }
}

int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  try {
    const std::string drop_old_test { "DROP TABLE IF EXISTS test;" };
    run_ddl_statement(drop_old_test);
    g_sqlite_comparator.query(drop_old_test);
    const std::string create_test {
      "CREATE TABLE test(x int, y int, z smallint, t bigint, f float, d double, str text encoding dict);" };
    run_ddl_statement(create_test);
    g_sqlite_comparator.query(
      "CREATE TABLE test(x int, y int, z smallint, t bigint, f float, d double, str text);");
  } catch (...) {
    LOG(ERROR) << "Failed to (re-)create table 'test'";
    return -EEXIST;
  }
  CHECK_EQ(g_num_rows % 2, 0);
  for (ssize_t i = 0; i < g_num_rows; ++i) {
    const std::string insert_query { "INSERT INTO test VALUES(7, 42, 101, 1001, 1.1, 2.2, 'foo');" };
    std::shared_ptr<Executor> executor;
    run_multiple_agg(insert_query, ExecutorDeviceType::CPU, executor);
    g_sqlite_comparator.query(insert_query);
  }
  for (ssize_t i = 0; i < g_num_rows / 2; ++i) {
    const std::string insert_query { "INSERT INTO test VALUES(8, 43, 102, 1002, 1.2, 2.4, 'bar');" };
    std::shared_ptr<Executor> executor;
    run_multiple_agg(insert_query, ExecutorDeviceType::CPU, executor);
    g_sqlite_comparator.query(insert_query);
  }
  for (ssize_t i = 0; i < g_num_rows / 2; ++i) {
    const std::string insert_query { "INSERT INTO test VALUES(7, 43, 102, 1002, 1.3, 2.6, 'baz');" };
    std::shared_ptr<Executor> executor;
    run_multiple_agg(insert_query, ExecutorDeviceType::CPU, executor);
    g_sqlite_comparator.query(insert_query);
  }
  int err { 0 };
  try {
    err = RUN_ALL_TESTS();
  } catch (const exception& e) {
    LOG(ERROR) << e.what();
  }
  const std::string drop_test { "DROP TABLE test;" };
  run_ddl_statement(drop_test);
  g_sqlite_comparator.query(drop_test);
  boost::filesystem::remove_all("/tmp/mapd_strings_1");
  return err;
}
