// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/policy/core/common/policy_service_impl.h"

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/macros.h"
#include "base/run_loop.h"
#include "base/test/scoped_task_environment.h"
#include "base/values.h"
#include "components/policy/core/common/external_data_fetcher.h"
#include "components/policy/core/common/mock_configuration_policy_provider.h"
#include "components/policy/core/common/mock_policy_service.h"
#include "components/policy/core/common/policy_types.h"
#include "components/policy/policy_constants.h"
#include "components/strings/grit/components_strings.h"
#include "testing/gmock/include/gmock/gmock.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::AnyNumber;
using ::testing::Mock;
using ::testing::Return;
using ::testing::_;

namespace policy {

namespace {

const char kExtension[] = "extension-id";
const char kSameLevelPolicy[] = "policy-same-level-and-scope";
const char kDiffLevelPolicy[] = "chrome-diff-level-and-scope";

// Helper to compare the arguments to an EXPECT_CALL of OnPolicyUpdated() with
// their expected values.
MATCHER_P(PolicyEquals, expected, "") {
  return arg.Equals(*expected);
}

// Helper to compare the arguments to an EXPECT_CALL of OnPolicyValueUpdated()
// with their expected values.
MATCHER_P(ValueEquals, expected, "") {
  return *expected == *arg;
}

// Helper that fills |bundle| with test policies.
void AddTestPolicies(PolicyBundle* bundle,
                     const char* value,
                     PolicyLevel level,
                     PolicyScope scope) {
  PolicyMap* policy_map =
      &bundle->Get(PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()));
  policy_map->Set(kSameLevelPolicy, POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                  POLICY_SOURCE_ENTERPRISE_DEFAULT,
                  std::make_unique<base::Value>(value), nullptr);
  policy_map->Set(kDiffLevelPolicy, level, scope, POLICY_SOURCE_PLATFORM,
                  std::make_unique<base::Value>(value), nullptr);
  policy_map =
      &bundle->Get(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension));
  policy_map->Set(kSameLevelPolicy, POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                  POLICY_SOURCE_ENTERPRISE_DEFAULT,
                  std::make_unique<base::Value>(value), nullptr);
  policy_map->Set(kDiffLevelPolicy, level, scope, POLICY_SOURCE_PLATFORM,
                  std::make_unique<base::Value>(value), nullptr);
}

// Observer class that changes the policy in the passed provider when the
// callback is invoked.
class ChangePolicyObserver : public PolicyService::Observer {
 public:
  explicit ChangePolicyObserver(MockConfigurationPolicyProvider* provider)
      : provider_(provider),
        observer_invoked_(false) {}

  void OnPolicyUpdated(const PolicyNamespace&,
                       const PolicyMap& previous,
                       const PolicyMap& current) override {
    PolicyMap new_policy;
    new_policy.Set("foo", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                   POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(14),
                   nullptr);
    provider_->UpdateChromePolicy(new_policy);
    observer_invoked_ = true;
  }

  bool observer_invoked() const { return observer_invoked_; }

 private:
  MockConfigurationPolicyProvider* provider_;
  bool observer_invoked_;
};

}  // namespace

class PolicyServiceTest : public testing::Test {
 public:
  PolicyServiceTest() {}
  void SetUp() override {
    EXPECT_CALL(provider0_, IsInitializationComplete(_))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(provider1_, IsInitializationComplete(_))
        .WillRepeatedly(Return(true));
    EXPECT_CALL(provider2_, IsInitializationComplete(_))
        .WillRepeatedly(Return(true));

    provider0_.Init();
    provider1_.Init();
    provider2_.Init();

    policy0_.Set("pre", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                 POLICY_SOURCE_ENTERPRISE_DEFAULT,
                 std::make_unique<base::Value>(13), nullptr);
    provider0_.UpdateChromePolicy(policy0_);

    PolicyServiceImpl::Providers providers;
    providers.push_back(&provider0_);
    providers.push_back(&provider1_);
    providers.push_back(&provider2_);
    policy_service_ = std::make_unique<PolicyServiceImpl>(std::move(providers));
  }

  void TearDown() override {
    provider0_.Shutdown();
    provider1_.Shutdown();
    provider2_.Shutdown();
  }

  MOCK_METHOD2(OnPolicyValueUpdated, void(const base::Value*,
                                          const base::Value*));

  MOCK_METHOD0(OnPolicyRefresh, void());

  // Returns true if the policies for namespace |ns| match |expected|.
  bool VerifyPolicies(const PolicyNamespace& ns,
                      const PolicyMap& expected) {
    return policy_service_->GetPolicies(ns).Equals(expected);
  }

  void RunUntilIdle() {
    base::RunLoop loop;
    loop.RunUntilIdle();
  }

 protected:
  base::test::ScopedTaskEnvironment task_environment_;
  MockConfigurationPolicyProvider provider0_;
  MockConfigurationPolicyProvider provider1_;
  MockConfigurationPolicyProvider provider2_;
  PolicyMap policy0_;
  PolicyMap policy1_;
  PolicyMap policy2_;
  std::unique_ptr<PolicyServiceImpl> policy_service_;

 private:
  DISALLOW_COPY_AND_ASSIGN(PolicyServiceTest);
};

TEST_F(PolicyServiceTest, LoadsPoliciesBeforeProvidersRefresh) {
  PolicyMap expected;
  expected.Set("pre", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_ENTERPRISE_DEFAULT,
               std::make_unique<base::Value>(13), nullptr);
  EXPECT_TRUE(VerifyPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()), expected));
}

TEST_F(PolicyServiceTest, NotifyObservers) {
  MockPolicyServiceObserver observer;
  policy_service_->AddObserver(POLICY_DOMAIN_CHROME, &observer);

  PolicyMap expectedPrevious;
  expectedPrevious.Set("pre", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                       POLICY_SOURCE_ENTERPRISE_DEFAULT,
                       std::make_unique<base::Value>(13), nullptr);

  PolicyMap expectedCurrent;
  expectedCurrent.CopyFrom(expectedPrevious);
  expectedCurrent.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                      POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(123),
                      nullptr);
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(123),
               nullptr);
  EXPECT_CALL(observer, OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_CHROME,
                                                        std::string()),
                                        PolicyEquals(&expectedPrevious),
                                        PolicyEquals(&expectedCurrent)));
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(&observer);

  // No changes.
  EXPECT_CALL(observer, OnPolicyUpdated(_, _, _)).Times(0);
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(&observer);
  EXPECT_TRUE(VerifyPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()), expectedCurrent));

  // New policy.
  expectedPrevious.CopyFrom(expectedCurrent);
  expectedCurrent.Set("bbb", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                      POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(456),
                      nullptr);
  policy0_.Set("bbb", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(456),
               nullptr);
  EXPECT_CALL(observer, OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_CHROME,
                                                        std::string()),
                                        PolicyEquals(&expectedPrevious),
                                        PolicyEquals(&expectedCurrent)));
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(&observer);

  // Removed policy.
  expectedPrevious.CopyFrom(expectedCurrent);
  expectedCurrent.Erase("bbb");
  policy0_.Erase("bbb");
  EXPECT_CALL(observer, OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_CHROME,
                                                        std::string()),
                                        PolicyEquals(&expectedPrevious),
                                        PolicyEquals(&expectedCurrent)));
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(&observer);

  // Changed policy.
  expectedPrevious.CopyFrom(expectedCurrent);
  expectedCurrent.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                      POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(789),
                      nullptr);
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(789),
               nullptr);

  EXPECT_CALL(observer, OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_CHROME,
                                                        std::string()),
                                        PolicyEquals(&expectedPrevious),
                                        PolicyEquals(&expectedCurrent)));
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(&observer);

  // No changes again.
  EXPECT_CALL(observer, OnPolicyUpdated(_, _, _)).Times(0);
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(&observer);
  EXPECT_TRUE(VerifyPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()), expectedCurrent));

  policy_service_->RemoveObserver(POLICY_DOMAIN_CHROME, &observer);
}

TEST_F(PolicyServiceTest, NotifyObserversInMultipleNamespaces) {
  const std::string kExtension0("extension-0");
  const std::string kExtension1("extension-1");
  const std::string kExtension2("extension-2");
  MockPolicyServiceObserver chrome_observer;
  MockPolicyServiceObserver extension_observer;
  policy_service_->AddObserver(POLICY_DOMAIN_CHROME, &chrome_observer);
  policy_service_->AddObserver(POLICY_DOMAIN_EXTENSIONS, &extension_observer);

  PolicyMap previous_policy_map;
  previous_policy_map.Set("pre", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                          POLICY_SOURCE_ENTERPRISE_DEFAULT,
                          std::make_unique<base::Value>(13), nullptr);
  PolicyMap policy_map;
  policy_map.CopyFrom(previous_policy_map);
  policy_map.Set("policy", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                 POLICY_SOURCE_CLOUD, std::make_unique<base::Value>("value"),
                 nullptr);

  std::unique_ptr<PolicyBundle> bundle(new PolicyBundle());
  // The initial setup includes a policy for chrome that is now changing.
  bundle->Get(PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()))
      .CopyFrom(policy_map);
  bundle->Get(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension0))
      .CopyFrom(policy_map);
  bundle->Get(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension1))
      .CopyFrom(policy_map);

  const PolicyMap kEmptyPolicyMap;
  EXPECT_CALL(
      chrome_observer,
      OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()),
                      PolicyEquals(&previous_policy_map),
                      PolicyEquals(&policy_map)));
  EXPECT_CALL(
      extension_observer,
      OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension0),
                      PolicyEquals(&kEmptyPolicyMap),
                      PolicyEquals(&policy_map)));
  EXPECT_CALL(
      extension_observer,
      OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension1),
                      PolicyEquals(&kEmptyPolicyMap),
                      PolicyEquals(&policy_map)));
  provider0_.UpdatePolicy(std::move(bundle));
  RunUntilIdle();
  Mock::VerifyAndClearExpectations(&chrome_observer);
  Mock::VerifyAndClearExpectations(&extension_observer);

  // Chrome policy stays the same, kExtension0 is gone, kExtension1 changes,
  // and kExtension2 is new.
  previous_policy_map.CopyFrom(policy_map);
  bundle.reset(new PolicyBundle());
  bundle->Get(PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()))
      .CopyFrom(policy_map);
  policy_map.Set("policy", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
                 POLICY_SOURCE_CLOUD,
                 std::make_unique<base::Value>("another value"), nullptr);
  bundle->Get(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension1))
      .CopyFrom(policy_map);
  bundle->Get(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension2))
      .CopyFrom(policy_map);

  EXPECT_CALL(chrome_observer, OnPolicyUpdated(_, _, _)).Times(0);
  EXPECT_CALL(
      extension_observer,
      OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension0),
                      PolicyEquals(&previous_policy_map),
                      PolicyEquals(&kEmptyPolicyMap)));
  EXPECT_CALL(
      extension_observer,
      OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension1),
                      PolicyEquals(&previous_policy_map),
                      PolicyEquals(&policy_map)));
  EXPECT_CALL(
      extension_observer,
      OnPolicyUpdated(PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension2),
                      PolicyEquals(&kEmptyPolicyMap),
                      PolicyEquals(&policy_map)));
  provider0_.UpdatePolicy(std::move(bundle));
  RunUntilIdle();
  Mock::VerifyAndClearExpectations(&chrome_observer);
  Mock::VerifyAndClearExpectations(&extension_observer);

  policy_service_->RemoveObserver(POLICY_DOMAIN_CHROME, &chrome_observer);
  policy_service_->RemoveObserver(POLICY_DOMAIN_EXTENSIONS,
                                  &extension_observer);
}

TEST_F(PolicyServiceTest, ObserverChangesPolicy) {
  ChangePolicyObserver observer(&provider0_);
  policy_service_->AddObserver(POLICY_DOMAIN_CHROME, &observer);
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(123),
               nullptr);
  policy0_.Set("bbb", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(1234),
               nullptr);
  // Should not crash.
  provider0_.UpdateChromePolicy(policy0_);
  policy_service_->RemoveObserver(POLICY_DOMAIN_CHROME, &observer);
  EXPECT_TRUE(observer.observer_invoked());
}

TEST_F(PolicyServiceTest, Priorities) {
  PolicyMap expected;
  expected.Set("pre", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_ENTERPRISE_DEFAULT,
               std::make_unique<base::Value>(13), nullptr);
  expected.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(0), nullptr);
  expected.GetMutable("aaa")->AddError(IDS_POLICY_CONFLICT_DIFF_VALUE);
  expected.GetMutable("aaa")->AddError(IDS_POLICY_CONFLICT_DIFF_VALUE);
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(0), nullptr);
  policy1_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(1), nullptr);
  policy2_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(2), nullptr);
  provider0_.UpdateChromePolicy(policy0_);
  provider1_.UpdateChromePolicy(policy1_);
  provider2_.UpdateChromePolicy(policy2_);
  expected.GetMutable("aaa")->AddConflictingPolicy(*policy1_.Get("aaa"));
  expected.GetMutable("aaa")->AddConflictingPolicy(*policy2_.Get("aaa"));

  EXPECT_TRUE(VerifyPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()), expected));

  expected.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(1), nullptr);
  expected.GetMutable("aaa")->AddError(IDS_POLICY_CONFLICT_DIFF_VALUE);
  policy0_.Erase("aaa");
  provider0_.UpdateChromePolicy(policy0_);
  expected.GetMutable("aaa")->AddConflictingPolicy(*policy2_.Get("aaa"));
  EXPECT_TRUE(VerifyPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()), expected));

  expected.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(2), nullptr);
  expected.GetMutable("aaa")->AddError(IDS_POLICY_CONFLICT_SAME_VALUE);
  policy1_.Set("aaa", POLICY_LEVEL_RECOMMENDED, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(1), nullptr);
  expected.GetMutable("aaa")->AddConflictingPolicy(*policy2_.Get("aaa"));
  provider1_.UpdateChromePolicy(policy2_);
  EXPECT_TRUE(VerifyPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()), expected));
}

TEST_F(PolicyServiceTest, PolicyChangeRegistrar) {
  std::unique_ptr<PolicyChangeRegistrar> registrar(new PolicyChangeRegistrar(
      policy_service_.get(),
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string())));

  // Starting to observe existing policies doesn't trigger a notification.
  EXPECT_CALL(*this, OnPolicyValueUpdated(_, _)).Times(0);
  registrar->Observe("pre", base::Bind(
      &PolicyServiceTest::OnPolicyValueUpdated,
      base::Unretained(this)));
  registrar->Observe("aaa", base::Bind(
      &PolicyServiceTest::OnPolicyValueUpdated,
      base::Unretained(this)));
  RunUntilIdle();
  Mock::VerifyAndClearExpectations(this);

  // Changing it now triggers a notification.
  base::Value kValue0(0);
  EXPECT_CALL(*this, OnPolicyValueUpdated(NULL, ValueEquals(&kValue0)));
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue0.CreateDeepCopy(), nullptr);
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(this);

  // Changing other values doesn't trigger a notification.
  EXPECT_CALL(*this, OnPolicyValueUpdated(_, _)).Times(0);
  policy0_.Set("bbb", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue0.CreateDeepCopy(), nullptr);
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(this);

  // Modifying the value triggers a notification.
  base::Value kValue1(1);
  EXPECT_CALL(*this, OnPolicyValueUpdated(ValueEquals(&kValue0),
                                          ValueEquals(&kValue1)));
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue1.CreateDeepCopy(), nullptr);
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(this);

  // Removing the value triggers a notification.
  EXPECT_CALL(*this, OnPolicyValueUpdated(ValueEquals(&kValue1), NULL));
  policy0_.Erase("aaa");
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(this);

  // No more notifications after destroying the registrar.
  EXPECT_CALL(*this, OnPolicyValueUpdated(_, _)).Times(0);
  registrar.reset();
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue1.CreateDeepCopy(), nullptr);
  policy0_.Set("pre", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_ENTERPRISE_DEFAULT, kValue1.CreateDeepCopy(),
               nullptr);
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(this);
}

TEST_F(PolicyServiceTest, RefreshPolicies) {
  EXPECT_CALL(provider0_, RefreshPolicies()).Times(AnyNumber());
  EXPECT_CALL(provider1_, RefreshPolicies()).Times(AnyNumber());
  EXPECT_CALL(provider2_, RefreshPolicies()).Times(AnyNumber());

  EXPECT_CALL(*this, OnPolicyRefresh()).Times(0);
  policy_service_->RefreshPolicies(base::Bind(
      &PolicyServiceTest::OnPolicyRefresh,
      base::Unretained(this)));
  // Let any queued observer tasks run.
  RunUntilIdle();
  Mock::VerifyAndClearExpectations(this);

  EXPECT_CALL(*this, OnPolicyRefresh()).Times(0);
  base::Value kValue0(0);
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue0.CreateDeepCopy(), nullptr);
  provider0_.UpdateChromePolicy(policy0_);
  Mock::VerifyAndClearExpectations(this);

  EXPECT_CALL(*this, OnPolicyRefresh()).Times(0);
  base::Value kValue1(1);
  policy1_.Set("aaa", POLICY_LEVEL_RECOMMENDED, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue1.CreateDeepCopy(), nullptr);
  provider1_.UpdateChromePolicy(policy1_);
  Mock::VerifyAndClearExpectations(this);

  // A provider can refresh more than once after a RefreshPolicies call, but
  // OnPolicyRefresh should be triggered only after all providers are
  // refreshed.
  EXPECT_CALL(*this, OnPolicyRefresh()).Times(0);
  policy1_.Set("bbb", POLICY_LEVEL_RECOMMENDED, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue1.CreateDeepCopy(), nullptr);
  provider1_.UpdateChromePolicy(policy1_);
  Mock::VerifyAndClearExpectations(this);

  // If another RefreshPolicies() call happens while waiting for a previous
  // one to complete, then all providers must refresh again.
  EXPECT_CALL(*this, OnPolicyRefresh()).Times(0);
  policy_service_->RefreshPolicies(base::Bind(
      &PolicyServiceTest::OnPolicyRefresh,
      base::Unretained(this)));
  RunUntilIdle();
  Mock::VerifyAndClearExpectations(this);

  EXPECT_CALL(*this, OnPolicyRefresh()).Times(0);
  policy2_.Set("bbb", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue0.CreateDeepCopy(), nullptr);
  provider2_.UpdateChromePolicy(policy2_);
  Mock::VerifyAndClearExpectations(this);

  // Providers 0 and 1 must reload again.
  EXPECT_CALL(*this, OnPolicyRefresh()).Times(2);
  base::Value kValue2(2);
  policy0_.Set("aaa", POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_CLOUD, kValue2.CreateDeepCopy(), nullptr);
  provider0_.UpdateChromePolicy(policy0_);
  provider1_.UpdateChromePolicy(policy1_);
  Mock::VerifyAndClearExpectations(this);

  const PolicyMap& policies = policy_service_->GetPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()));
  EXPECT_EQ(kValue2, *policies.GetValue("aaa"));
  EXPECT_EQ(kValue0, *policies.GetValue("bbb"));
}

TEST_F(PolicyServiceTest, NamespaceMerge) {
  std::unique_ptr<PolicyBundle> bundle0(new PolicyBundle());
  std::unique_ptr<PolicyBundle> bundle1(new PolicyBundle());
  std::unique_ptr<PolicyBundle> bundle2(new PolicyBundle());

  AddTestPolicies(bundle0.get(), "bundle0",
                  POLICY_LEVEL_RECOMMENDED, POLICY_SCOPE_USER);
  AddTestPolicies(bundle1.get(), "bundle1",
                  POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER);
  AddTestPolicies(bundle2.get(), "bundle2",
                  POLICY_LEVEL_MANDATORY, POLICY_SCOPE_MACHINE);

  PolicyMap expected;
  // For policies of the same level and scope, the first provider takes
  // precedence, on every namespace.
  expected.Set(kSameLevelPolicy, POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
               POLICY_SOURCE_ENTERPRISE_DEFAULT,
               std::make_unique<base::Value>("bundle0"), nullptr);
  expected.GetMutable(kSameLevelPolicy)
      ->AddError(IDS_POLICY_CONFLICT_DIFF_VALUE);
  expected.GetMutable(kSameLevelPolicy)
      ->AddError(IDS_POLICY_CONFLICT_DIFF_VALUE);
  expected.GetMutable(kSameLevelPolicy)
      ->AddConflictingPolicy(
          *bundle1->Get(PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()))
               .Get(kSameLevelPolicy));
  expected.GetMutable(kSameLevelPolicy)
      ->AddConflictingPolicy(
          *bundle2->Get(PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()))
               .Get(kSameLevelPolicy));
  // For policies with different levels and scopes, the highest priority
  // level/scope combination takes precedence, on every namespace.
  expected.Set(kDiffLevelPolicy, POLICY_LEVEL_MANDATORY, POLICY_SCOPE_MACHINE,
               POLICY_SOURCE_PLATFORM, std::make_unique<base::Value>("bundle2"),
               nullptr);
  expected.GetMutable(kDiffLevelPolicy)
      ->AddError(IDS_POLICY_CONFLICT_DIFF_VALUE);
  expected.GetMutable(kDiffLevelPolicy)
      ->AddConflictingPolicy(
          *bundle0->Get(PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()))
               .Get(kDiffLevelPolicy));
  expected.GetMutable(kDiffLevelPolicy)
      ->AddConflictingPolicy(
          *bundle1->Get(PolicyNamespace(POLICY_DOMAIN_CHROME, std::string()))
               .Get(kDiffLevelPolicy));

  provider0_.UpdatePolicy(std::move(bundle0));
  provider1_.UpdatePolicy(std::move(bundle1));
  provider2_.UpdatePolicy(std::move(bundle2));
  RunUntilIdle();

  EXPECT_TRUE(policy_service_->GetPolicies(
      PolicyNamespace(POLICY_DOMAIN_CHROME, std::string())).Equals(expected));
  EXPECT_TRUE(policy_service_->GetPolicies(
      PolicyNamespace(POLICY_DOMAIN_EXTENSIONS, kExtension)).Equals(expected));
}

TEST_F(PolicyServiceTest, IsInitializationComplete) {
  // |provider0| has all domains initialized.
  Mock::VerifyAndClearExpectations(&provider1_);
  Mock::VerifyAndClearExpectations(&provider2_);
  EXPECT_CALL(provider1_, IsInitializationComplete(_))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(provider2_, IsInitializationComplete(_))
      .WillRepeatedly(Return(false));
  PolicyServiceImpl::Providers providers;
  providers.push_back(&provider0_);
  providers.push_back(&provider1_);
  providers.push_back(&provider2_);
  policy_service_ = std::make_unique<PolicyServiceImpl>(std::move(providers));
  EXPECT_FALSE(policy_service_->IsInitializationComplete(POLICY_DOMAIN_CHROME));
  EXPECT_FALSE(
      policy_service_->IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS));
  EXPECT_FALSE(policy_service_->IsInitializationComplete(
      POLICY_DOMAIN_SIGNIN_EXTENSIONS));

  // |provider2_| still doesn't have POLICY_DOMAIN_CHROME initialized, so
  // the initialization status of that domain won't change.
  MockPolicyServiceObserver observer;
  policy_service_->AddObserver(POLICY_DOMAIN_CHROME, &observer);
  policy_service_->AddObserver(POLICY_DOMAIN_EXTENSIONS, &observer);
  policy_service_->AddObserver(POLICY_DOMAIN_SIGNIN_EXTENSIONS, &observer);
  EXPECT_CALL(observer, OnPolicyServiceInitialized(_)).Times(0);
  Mock::VerifyAndClearExpectations(&provider1_);
  EXPECT_CALL(provider1_, IsInitializationComplete(POLICY_DOMAIN_CHROME))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(provider1_, IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(provider1_,
              IsInitializationComplete(POLICY_DOMAIN_SIGNIN_EXTENSIONS))
      .WillRepeatedly(Return(false));
  const PolicyMap kPolicyMap;
  provider1_.UpdateChromePolicy(kPolicyMap);
  Mock::VerifyAndClearExpectations(&observer);
  EXPECT_FALSE(policy_service_->IsInitializationComplete(POLICY_DOMAIN_CHROME));
  EXPECT_FALSE(
      policy_service_->IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS));
  EXPECT_FALSE(policy_service_->IsInitializationComplete(
      POLICY_DOMAIN_SIGNIN_EXTENSIONS));

  // Same if |provider1_| doesn't have POLICY_DOMAIN_EXTENSIONS initialized.
  EXPECT_CALL(observer, OnPolicyServiceInitialized(_)).Times(0);
  Mock::VerifyAndClearExpectations(&provider2_);
  EXPECT_CALL(provider2_, IsInitializationComplete(POLICY_DOMAIN_CHROME))
      .WillRepeatedly(Return(false));
  EXPECT_CALL(provider2_, IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(provider2_,
              IsInitializationComplete(POLICY_DOMAIN_SIGNIN_EXTENSIONS))
      .WillRepeatedly(Return(true));
  provider2_.UpdateChromePolicy(kPolicyMap);
  Mock::VerifyAndClearExpectations(&observer);
  EXPECT_FALSE(policy_service_->IsInitializationComplete(POLICY_DOMAIN_CHROME));
  EXPECT_FALSE(
      policy_service_->IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS));
  EXPECT_FALSE(policy_service_->IsInitializationComplete(
      POLICY_DOMAIN_SIGNIN_EXTENSIONS));

  // Now initialize POLICY_DOMAIN_CHROME on all the providers.
  EXPECT_CALL(observer, OnPolicyServiceInitialized(POLICY_DOMAIN_CHROME));
  Mock::VerifyAndClearExpectations(&provider2_);
  EXPECT_CALL(provider2_, IsInitializationComplete(POLICY_DOMAIN_CHROME))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(provider2_, IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(provider2_,
              IsInitializationComplete(POLICY_DOMAIN_SIGNIN_EXTENSIONS))
      .WillRepeatedly(Return(true));
  provider2_.UpdateChromePolicy(kPolicyMap);
  Mock::VerifyAndClearExpectations(&observer);
  EXPECT_TRUE(policy_service_->IsInitializationComplete(POLICY_DOMAIN_CHROME));
  // Other domains are still not initialized.
  EXPECT_FALSE(
      policy_service_->IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS));
  EXPECT_FALSE(policy_service_->IsInitializationComplete(
      POLICY_DOMAIN_SIGNIN_EXTENSIONS));

  // Initialize the remaining domain.
  EXPECT_CALL(observer, OnPolicyServiceInitialized(POLICY_DOMAIN_EXTENSIONS));
  EXPECT_CALL(observer,
              OnPolicyServiceInitialized(POLICY_DOMAIN_SIGNIN_EXTENSIONS));
  Mock::VerifyAndClearExpectations(&provider1_);
  EXPECT_CALL(provider1_, IsInitializationComplete(POLICY_DOMAIN_CHROME))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(provider1_, IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS))
      .WillRepeatedly(Return(true));
  EXPECT_CALL(provider1_,
              IsInitializationComplete(POLICY_DOMAIN_SIGNIN_EXTENSIONS))
      .WillRepeatedly(Return(true));
  provider1_.UpdateChromePolicy(kPolicyMap);
  Mock::VerifyAndClearExpectations(&observer);
  EXPECT_TRUE(policy_service_->IsInitializationComplete(POLICY_DOMAIN_CHROME));
  EXPECT_TRUE(
      policy_service_->IsInitializationComplete(POLICY_DOMAIN_EXTENSIONS));
  EXPECT_TRUE(policy_service_->IsInitializationComplete(
      POLICY_DOMAIN_SIGNIN_EXTENSIONS));

  // Cleanup.
  policy_service_->RemoveObserver(POLICY_DOMAIN_CHROME, &observer);
  policy_service_->RemoveObserver(POLICY_DOMAIN_EXTENSIONS, &observer);
  policy_service_->RemoveObserver(POLICY_DOMAIN_SIGNIN_EXTENSIONS, &observer);
}

TEST_F(PolicyServiceTest, SeparateProxyPoliciesMerging) {
  const PolicyNamespace chrome_namespace(POLICY_DOMAIN_CHROME, std::string());
  const PolicyNamespace extension_namespace(POLICY_DOMAIN_EXTENSIONS, "xyz");

  std::unique_ptr<PolicyBundle> policy_bundle(new PolicyBundle());
  PolicyMap& policy_map = policy_bundle->Get(chrome_namespace);
  // Individual proxy policy values in the Chrome namespace should be collected
  // into a dictionary.
  policy_map.Set(key::kProxyServerMode, POLICY_LEVEL_MANDATORY,
                 POLICY_SCOPE_USER, POLICY_SOURCE_CLOUD,
                 std::make_unique<base::Value>(3), nullptr);

  // Both these policies should be ignored, since there's a higher priority
  // policy available.
  policy_map.Set(key::kProxyMode, POLICY_LEVEL_RECOMMENDED, POLICY_SCOPE_USER,
                 POLICY_SOURCE_CLOUD,
                 std::make_unique<base::Value>("pac_script"), nullptr);
  policy_map.Set(key::kProxyPacUrl, POLICY_LEVEL_RECOMMENDED, POLICY_SCOPE_USER,
                 POLICY_SOURCE_CLOUD,
                 std::make_unique<base::Value>("http://example.com/wpad.dat"),
                 nullptr);

  // Add a value to a non-Chrome namespace.
  policy_bundle->Get(extension_namespace)
      .Set(key::kProxyServerMode, POLICY_LEVEL_MANDATORY, POLICY_SCOPE_USER,
           POLICY_SOURCE_CLOUD, std::make_unique<base::Value>(3), nullptr);

  // The resulting Chrome namespace map should have the collected policy.
  PolicyMap expected_chrome;
  std::unique_ptr<base::DictionaryValue> expected_value(
      new base::DictionaryValue);
  expected_value->SetInteger(key::kProxyServerMode, 3);
  expected_chrome.Set(key::kProxySettings, POLICY_LEVEL_MANDATORY,
                      POLICY_SCOPE_USER, POLICY_SOURCE_CLOUD,
                      std::move(expected_value), nullptr);

  // The resulting Extensions namespace map shouldn't have been modified.
  PolicyMap expected_extension;
  expected_extension.Set(key::kProxyServerMode, POLICY_LEVEL_MANDATORY,
                         POLICY_SCOPE_USER, POLICY_SOURCE_CLOUD,
                         std::make_unique<base::Value>(3), nullptr);

  provider0_.UpdatePolicy(std::move(policy_bundle));
  RunUntilIdle();

  EXPECT_TRUE(VerifyPolicies(chrome_namespace, expected_chrome));
  EXPECT_TRUE(VerifyPolicies(extension_namespace, expected_extension));
}

TEST_F(PolicyServiceTest, CloudAndPlatformPolicyMerging) {
  const PolicyNamespace chrome_namespace(POLICY_DOMAIN_CHROME, std::string());

  std::unique_ptr<base::ListValue> list1 = std::make_unique<base::ListValue>();
  list1->Append(std::make_unique<base::Value>("google.com"));
  std::unique_ptr<base::ListValue> list2 = std::make_unique<base::ListValue>();
  list2->Append(std::make_unique<base::Value>("example.com"));
  std::unique_ptr<base::ListValue> result = std::make_unique<base::ListValue>();
  result->Append(std::make_unique<base::Value>("google.com"));
  result->Append(std::make_unique<base::Value>("example.com"));

  std::unique_ptr<PolicyBundle> policy_bundle1(new PolicyBundle());
  PolicyMap& policy_map1 = policy_bundle1->Get(chrome_namespace);
  policy_map1.Set(key::kExtensionInstallListsMergeEnabled,
                  POLICY_LEVEL_MANDATORY, POLICY_SCOPE_MACHINE,
                  POLICY_SOURCE_PLATFORM, std::make_unique<base::Value>(true),
                  nullptr);
  policy_map1.Set(key::kExtensionInstallForcelist, POLICY_LEVEL_MANDATORY,
                  POLICY_SCOPE_MACHINE, POLICY_SOURCE_PLATFORM,
                  std::move(list1), nullptr);

  std::unique_ptr<PolicyBundle> policy_bundle2(new PolicyBundle());
  PolicyMap& policy_map2 = policy_bundle2->Get(chrome_namespace);
  policy_map2.Set(key::kExtensionInstallForcelist, POLICY_LEVEL_MANDATORY,
                  POLICY_SCOPE_MACHINE, POLICY_SOURCE_CLOUD, std::move(list2),
                  nullptr);

  PolicyMap expected_chrome;
  expected_chrome.Set(key::kExtensionInstallListsMergeEnabled,
                      POLICY_LEVEL_MANDATORY, POLICY_SCOPE_MACHINE,
                      POLICY_SOURCE_PLATFORM,
                      std::make_unique<base::Value>(true), nullptr);
  expected_chrome.Set(key::kExtensionInstallForcelist, POLICY_LEVEL_MANDATORY,
                      POLICY_SCOPE_MERGED, POLICY_SOURCE_MERGED,
                      std::move(result), nullptr);

  provider0_.UpdatePolicy(std::move(policy_bundle1));
  provider1_.UpdatePolicy(std::move(policy_bundle2));
  RunUntilIdle();

  EXPECT_TRUE(VerifyPolicies(chrome_namespace, expected_chrome));
}

}  // namespace policy
