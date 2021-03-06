// Copyright 2020 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/bigtable/instance_admin.h"
#include "google/cloud/bigtable/testing/table_integration_test.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/testing_util/assert_ok.h"
#include "google/cloud/testing_util/chrono_literals.h"
#include <gmock/gmock.h>

namespace google {
namespace cloud {
namespace bigtable {
inline namespace BIGTABLE_CLIENT_NS {
namespace {
namespace btadmin = google::bigtable::admin::v2;
namespace bigtable = google::cloud::bigtable;
using namespace google::cloud::testing_util::chrono_literals;

class AdminIAMPolicyIntegrationTest
    : public bigtable::testing::TableIntegrationTest {
 protected:
  std::shared_ptr<AdminClient> admin_client_;
  std::unique_ptr<TableAdmin> table_admin_;
  std::string service_account_;

  void SetUp() {
    service_account_ = google::cloud::internal::GetEnv(
                           "GOOGLE_CLOUD_CPP_BIGTABLE_TEST_SERVICE_ACCOUNT")
                           .value_or("");
    ASSERT_FALSE(service_account_.empty());

    TableIntegrationTest::SetUp();
    admin_client_ = CreateDefaultAdminClient(
        testing::TableTestEnvironment::project_id(), ClientOptions());
    table_admin_ = google::cloud::internal::make_unique<TableAdmin>(
        admin_client_, bigtable::testing::TableTestEnvironment::instance_id());
  }

  void TearDown() {}

  int CountMatchingTables(std::string const& table_id,
                          std::vector<btadmin::Table> const& tables) {
    std::string table_name =
        table_admin_->instance_name() + "/tables/" + table_id;
    auto count = std::count_if(tables.begin(), tables.end(),
                               [&table_name](btadmin::Table const& t) {
                                 return table_name == t.name();
                               });
    return static_cast<int>(count);
  }
};

TEST_F(AdminIAMPolicyIntegrationTest, AsyncSetGetTestIamAPIsTest) {
  using namespace google::cloud::testing_util::chrono_literals;

  std::string const table_id = RandomTableId();

  auto iam_policy = bigtable::IamPolicy({bigtable::IamBinding(
      "roles/bigtable.reader", {"serviceAccount:" + service_account_})});

  TableConfig table_config({{"fam", GcRule::MaxNumVersions(5)},
                            {"foo", GcRule::MaxAge(std::chrono::hours(24))}},
                           {"a1000", "a2000", "b3000", "m5000"});

  CompletionQueue cq;
  std::thread pool([&cq] { cq.Run(); });

  future<void> chain =
      table_admin_->AsyncListTables(cq, btadmin::Table::NAME_ONLY)
          .then([&](future<StatusOr<std::vector<btadmin::Table>>> fut) {
            StatusOr<std::vector<btadmin::Table>> result = fut.get();
            EXPECT_STATUS_OK(result);
            auto previous_count = CountMatchingTables(table_id, *result);
            EXPECT_EQ(0, previous_count)
                << "Table (" << table_id << ") already exists."
                << " This is unexpected, as the table ids are"
                << " generated at random.";
            return table_admin_->AsyncCreateTable(cq, table_id, table_config);
          })
          .then([&](future<StatusOr<btadmin::Table>> fut) {
            StatusOr<btadmin::Table> result = fut.get();
            EXPECT_STATUS_OK(result);
            EXPECT_THAT(result->name(), ::testing::HasSubstr(table_id));
            return table_admin_->AsyncSetIamPolicy(cq, table_id, iam_policy);
          })
          .then([&](future<StatusOr<google::iam::v1::Policy>> fut) {
            StatusOr<google::iam::v1::Policy> get_result = fut.get();
            EXPECT_STATUS_OK(get_result);
            return table_admin_->AsyncGetIamPolicy(cq, table_id);
          })
          .then([&](future<StatusOr<google::iam::v1::Policy>> fut) {
            StatusOr<google::iam::v1::Policy> get_result = fut.get();
            EXPECT_STATUS_OK(get_result);
            return table_admin_->AsyncTestIamPermissions(
                cq, table_id,
                {"bigtable.tables.get", "bigtable.tables.readRows"});
          })
          .then([&](future<StatusOr<std::vector<std::string>>> fut) {
            StatusOr<std::vector<std::string>> get_result = fut.get();
            EXPECT_STATUS_OK(get_result);
            EXPECT_EQ(2, get_result->size());
            return table_admin_->AsyncDeleteTable(cq, table_id);
          })
          .then([&](future<Status> fut) {
            Status delete_result = fut.get();
            EXPECT_STATUS_OK(delete_result);
            return table_admin_->AsyncListTables(cq, btadmin::Table::NAME_ONLY);
          })
          .then([&](future<StatusOr<std::vector<btadmin::Table>>> fut) {
            StatusOr<std::vector<btadmin::Table>> result = fut.get();
            EXPECT_STATUS_OK(result);
            auto previous_count = CountMatchingTables(table_id, *result);
            ASSERT_EQ(0, previous_count)
                << "Table (" << table_id << ") already exists."
                << " This is unexpected, as the table ids are"
                << " generated at random.";
          });

  chain.get();
  SUCCEED();  // we expect that previous operations do not fail.

  cq.Shutdown();
  pool.join();
}

/// @test Verify that IAM Policy APIs work as expected.
TEST_F(AdminIAMPolicyIntegrationTest, SetGetTestIamAPIsTest) {
  using GC = bigtable::GcRule;
  std::string const table_id = RandomTableId();

  // verify new table id in current table list
  auto previous_table_list =
      table_admin_->ListTables(btadmin::Table::NAME_ONLY);
  ASSERT_STATUS_OK(previous_table_list);
  auto previous_count = CountMatchingTables(table_id, *previous_table_list);
  ASSERT_EQ(0, previous_count) << "Table (" << table_id << ") already exists."
                               << " This is unexpected, as the table ids are"
                               << " generated at random.";
  // create table config
  bigtable::TableConfig table_config(
      {{"fam", GC::MaxNumVersions(5)},
       {"foo", GC::MaxAge(std::chrono::hours(24))}},
      {"a1000", "a2000", "b3000", "m5000"});

  // create table
  ASSERT_STATUS_OK(table_admin_->CreateTable(table_id, table_config));

  auto iam_policy = bigtable::IamPolicy({bigtable::IamBinding(
      "roles/bigtable.reader", {"serviceAccount:" + service_account_})});

  auto initial_policy = table_admin_->SetIamPolicy(table_id, iam_policy);
  ASSERT_STATUS_OK(initial_policy);

  auto fetched_policy = table_admin_->GetIamPolicy(table_id);
  ASSERT_STATUS_OK(fetched_policy);

  EXPECT_EQ(initial_policy->version(), fetched_policy->version());
  EXPECT_EQ(initial_policy->etag(), fetched_policy->etag());

  auto permission_set = table_admin_->TestIamPermissions(
      table_id, {"bigtable.tables.get", "bigtable.tables.readRows"});
  ASSERT_STATUS_OK(permission_set);

  EXPECT_EQ(2, permission_set->size());
  EXPECT_STATUS_OK(table_admin_->DeleteTable(table_id));
}
// Test Cases Finished
}  // namespace
}  // namespace BIGTABLE_CLIENT_NS
}  // namespace bigtable
}  // namespace cloud
}  // namespace google

int main(int argc, char* argv[]) {
  ::testing::InitGoogleMock(&argc, argv);
  (void)::testing::AddGlobalTestEnvironment(
      new google::cloud::bigtable::testing::TableTestEnvironment);
  return RUN_ALL_TESTS();
}
